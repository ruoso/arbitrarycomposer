#include <arbc/runtime/offline_sequence.hpp>

#include <arbc/compositor/pull_service.hpp> // PullServiceImpl, RenderDispatch, PullConfig
#include <arbc/compositor/refinement.hpp>   // RefinementQueue, poll_refinements
#include <arbc/compositor/tile_planning.hpp> // render_frame_interactive
#include <arbc/contract/content.hpp>         // Deadline, Exactness
#include <arbc/surface/surface.hpp>          // Surface

#include <optional>
#include <utility>

namespace arbc {

std::vector<Time> frame_times_over(const TimeRange& range, const Rational& output_rate) {
  std::vector<Time> times;
  // A non-positive rate clocks nothing and an empty range spans no instant: an
  // empty series, never an abort (faults-as-values, doc 10).
  if (output_rate.num() <= 0 || range.empty()) {
    return times;
  }
  // Flicks per output frame = flicks_per_second * (1 / output_rate), formed by an
  // exact rational multiply so a pathological rate whose period overflows the
  // int64 flick width surfaces as a `TimeError` (and an empty series) rather than
  // wrapping (doc 11:52-56). `Rational(den, num)` is the inverse rate seconds-per-
  // frame; multiplying by flicks-per-second gives flicks-per-frame.
  const expected<Rational, TimeError> flicks_per_frame =
      Rational(Time::flicks_per_second, 1).mul(Rational(output_rate.den(), output_rate.num()));
  if (!flicks_per_frame.has_value()) {
    return times;
  }
  // Frame k lands at `k * flicks_per_frame + range.start`, evaluated through the
  // same rational-compose + single ties-to-even leaf rounding the transport uses
  // (`TimeMap::evaluate`), so the series is exact and byte-reproducible.
  const TimeMap step{Time::zero(), *flicks_per_frame, range.start};
  for (std::int64_t k = 0;; ++k) {
    const expected<Time, TimeError> t = step.evaluate(Time{k});
    if (!t.has_value()) {
      break; // the instant overflowed the flick width: stop honestly
    }
    if (!range.contains(*t)) {
      break; // reached or passed the half-open endpoint `range.end` (excluded)
    }
    times.push_back(*t);
  }
  return times;
}

SequenceRenderer::SequenceRenderer(const Document& document, Viewport viewport, Backend& backend,
                                   WorkerPoolConfig pool_config, std::size_t cache_budget_bytes)
    : d_document(document),
      d_viewport(std::move(viewport)),
      d_backend(backend),
      // Pin ONCE for the whole export (Decision 2): this snapshot outlives every
      // frame and every later commit, so the sequence is revision-consistent.
      d_pinned(document.pin()),
      d_surfaces(backend),
      d_cache(cache_budget_bytes),
      d_parallel(pool_config.worker_count != 0),
      d_pool(std::move(pool_config)) {}

expected<std::unique_ptr<Surface>, SurfaceError>
SequenceRenderer::render_frame_at(Time composition_time) {
  const DocRoot& state = *d_pinned;
  // The frame target carries the pinned version's configured working space (doc 07
  // rule 2), read from the frozen revision -- capability-honest via the
  // `SurfaceError` value path exactly as `render_offline` (offline.cpp:15-16): a
  // backend that cannot store that format yields the error path, never an abort.
  expected<std::unique_ptr<Surface>, SurfaceError> target =
      d_backend.make_surface(d_viewport.width, d_viewport.height, state.working_space());
  if (!target.has_value()) {
    return unexpected(target.error());
  }
  Surface& frame = **target;

  const ContentResolver resolve = [this](ObjectId id) { return d_document.resolve(id); };

  // Exact / no-degrade discipline (doc 02:73-85): `Exactness::Exact` requests carry
  // `Deadline::none()` (no deadline; every miss rendered to completion), and
  // `prior_revision == nullopt` disables the stale probe -- so a miss is never
  // served a stale-revision tile. The per-layer span-cull + time-map +
  // `quantize_time` walk `compositor.temporal_placement_culling` built runs against
  // `composition_time`; this driver only threads the time and enforces exactness,
  // reusing the temporal tile cache for cross-frame reuse (Decision 3).
  if (!d_parallel) {
    // Inline exact: one pass, every miss rendered to completion before composite,
    // so every tile is a fresh exact source -- the byte-deterministic path
    // (Decision 4). No `pending`/`pulls`: the compositor's inline fill drives each
    // miss's `render` to completion synchronously.
    render_frame_interactive(state, resolve, d_viewport, d_cache, d_backend, d_surfaces, frame,
                             Deadline::none(), /*prior_revision=*/std::nullopt,
                             /*pending=*/nullptr, &d_counters, /*dirty=*/nullptr, composition_time,
                             /*visible_plans=*/nullptr, /*diagnostics=*/nullptr, /*pulls=*/nullptr,
                             Exactness::Exact);
    return std::move(*target);
  }

  // Parallel exact (opt-in): dispatch misses onto the worker pool, then -- because
  // offline has NO deadline -- genuinely block-and-fan-out, reaping every dispatched
  // completion via `wait_completions(nullopt)` until all settle, inserting each on
  // the DRIVER thread (workers never touch the cache, `runtime.threading`'s rule).
  // A final all-fresh pass composites the exact result. Exactness is order-
  // independent, so this yields identical pixels to the inline path.
  RenderDispatch dispatch = [this](Content* content, const RenderRequest& request,
                                   std::shared_ptr<RenderCompletion> done) {
    d_pool.submit(RenderTask{content, request, std::move(done)});
  };
  PullServiceImpl pulls(d_cache, d_backend, std::move(dispatch), PullConfig{});
  RefinementQueue pending;
  render_frame_interactive(state, resolve, d_viewport, d_cache, d_backend, d_surfaces, frame,
                           Deadline::none(), /*prior_revision=*/std::nullopt, &pending, &d_counters,
                           /*dirty=*/nullptr, composition_time, /*visible_plans=*/nullptr,
                           /*diagnostics=*/nullptr, &pulls, Exactness::Exact);
  // Reap to quiescence: no deadline, so wait until EVERY dispatched render settles
  // into the cache. `poll_refinements` inserts settled arrivals (driver thread) and
  // drops them from the queue; unsettled ones are retained across the park.
  while (!pending.tiles.empty()) {
    d_pool.wait_completions(std::nullopt);
    poll_refinements(pending, d_cache, &d_counters, &d_backend);
  }
  // Re-composite from the now fully-warm cache: every tile is a fresh exact hit, so
  // this pass dispatches nothing and composites the exact frame (Decision 3/4).
  render_frame_interactive(state, resolve, d_viewport, d_cache, d_backend, d_surfaces, frame,
                           Deadline::none(), /*prior_revision=*/std::nullopt, /*pending=*/nullptr,
                           &d_counters, /*dirty=*/nullptr, composition_time,
                           /*visible_plans=*/nullptr, /*diagnostics=*/nullptr, /*pulls=*/nullptr,
                           Exactness::Exact);
  return std::move(*target);
}

void SequenceRenderer::render_sequence(std::span<const Time> frame_times, const FrameSink& sink) {
  for (const Time t : frame_times) {
    sink(t, render_frame_at(t));
  }
}

} // namespace arbc
