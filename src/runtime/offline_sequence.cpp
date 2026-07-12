#include <arbc/compositor/pull_service.hpp>  // PullServiceImpl, RenderDispatch, PullConfig
#include <arbc/compositor/refinement.hpp>    // RefinementQueue, poll_refinements
#include <arbc/compositor/tile_planning.hpp> // render_frame_interactive
#include <arbc/contract/content.hpp>         // Deadline, Exactness
#include <arbc/runtime/offline_sequence.hpp>
#include <arbc/runtime/operator_binding.hpp> // bind_operators, register_builtin_operator_binders
#include <arbc/runtime/pull_identity.hpp>    // make_pull_identity_of (child-distinct id_of)
#include <arbc/runtime/worker_dispatch.hpp>  // worker_backed_dispatch (the leaf-only rule)
#include <arbc/surface/surface.hpp>          // Surface

#include <cstdint>
#include <memory>
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
    : d_document(document), d_viewport(std::move(viewport)), d_backend(backend),
      // Pin ONCE for the whole export (Decision 2): this snapshot outlives every
      // frame and every later commit, so the sequence is revision-consistent.
      d_pinned(document.pin()), d_surfaces(backend), d_cache(cache_budget_bytes),
      d_parallel(pool_config.worker_count != 0), d_pool(std::move(pool_config)) {
  // Populate the operator-binder registry once (thread-safe, idempotent) so
  // `render_frame_at` can bind each `org.arbc.fade` (and any sibling operator kind)
  // to this driver's live services before the compositor renders it
  // (operators.fade_runtime_binding, doc 13:69-71).
  register_builtin_operator_binders();
}

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

  // The live-service config the fade (and any operator kind) pulls its input through
  // once bound (operators.fade_runtime_binding, Constraint 5): a reverse
  // `Content* -> ObjectId` map keys each input's tiles under its identity
  // (doc 13:141-154), and every node contributes the one pinned revision
  // (doc 05:82-91). Built off the frozen revision. The map seeds from the layer
  // roots AND assigns every operator input child a distinct synthesized id
  // (runtime.operator_input_cache_identity) so two same-stability inputs of one
  // operator no longer collide on `ObjectId{}` and alias one cache key. The walk is
  // deterministic over the immutable pinned graph, so a child's id is stable across
  // the frames of this sequence (Constraint 3); shared read-only on parallel workers.
  auto id_of = make_pull_identity_of(state, resolve);
  const std::uint64_t revision = state.revision();
  const auto make_config = [&](RefinementQueue* pending_queue) {
    PullConfig config;
    config.counters = &d_counters;
    config.pending = pending_queue;
    config.id_of = id_of;
    config.contribution = [revision](const Content*) { return revision; };
    return config;
  };

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
    // miss's `render` to completion synchronously. A fade still needs a live service
    // to pull its OWN input, so bind every operator to a synchronous direct-dispatch
    // service on this thread before the fill drives their render (Constraint 2). The
    // scope tears the binding down on return (Constraint 3); `inline_pull` (declared
    // first) destructs after it, so the borrowed service outlives the binding
    // (Constraint 4).
    PullServiceImpl inline_pull(d_cache, d_backend, direct_dispatch(), make_config(nullptr));
    // `d_pinned` -- the export's ONE snapshot -- not a fresh pin: a nested content
    // reads its child's membership from the version this frame renders against
    // (doc 05:71-75). Re-binding the same pin each frame is free: nested re-keys its
    // metadata memo only on an actually-new snapshot
    // (`kinds.nested_runtime_binding` Decision 3).
    const OperatorBindingScope binding =
        bind_operators(d_document, inline_pull, d_backend, d_pinned);
    // Pass the already-built `inline_pull` (not `nullptr`) so the frame driver can
    // SERVE an operator layer's identity short-circuit -- deliver input N's tiles
    // into the layer footprint through `pull` (runtime.operator_identity_offline_
    // delivery). Byte-for-byte safe on the non-identity path: `inline_pull` uses
    // `direct_dispatch`, so `pulls->dispatch` is exactly the prior inline
    // `content->render` + `done->complete` (pull_service.hpp:195-199).
    render_frame_interactive(state, resolve, d_viewport, d_cache, d_backend, d_surfaces, frame,
                             Deadline::none(), /*prior_revision=*/std::nullopt,
                             /*pending=*/nullptr, &d_counters, /*dirty=*/nullptr, composition_time,
                             /*visible_plans=*/nullptr, /*diagnostics=*/nullptr, &inline_pull,
                             Exactness::Exact);
    return std::move(*target);
  }

  // Parallel exact (opt-in): dispatch LEAF misses onto the worker pool, then --
  // because offline has NO deadline -- genuinely block-and-fan-out, reaping every
  // dispatched completion via `wait_completions(nullopt)` until all settle, inserting
  // each on the DRIVER thread (workers never touch the cache, `runtime.threading`'s
  // rule). A final all-fresh pass composites the exact result. Exactness is order-
  // independent, so this yields identical pixels to the inline path.
  //
  // `worker_backed_dispatch` is where the leaf-only rule lives (doc 02 § Threading
  // model): an operator content renders inline on THIS driver thread and only leaves
  // fan out, because an operator's render re-enters the render-thread-confined
  // `PullService`. This driver states the rule nowhere itself -- it obtains a
  // dispatch that already enforces it, which is the point of the helper.
  RefinementQueue pending;
  PullServiceImpl pulls(d_cache, d_backend, worker_backed_dispatch(d_pool), make_config(&pending));
  // Bind every operator content to the live service on the DRIVER thread, before any
  // worker dispatch (Constraint 8): its borrowed pointers are read-only on workers
  // during render. The fade's own nested input pull rides `pulls` (async-dispatched
  // and reaped through `pending` below). `pulls` (declared first) outlives `binding`.
  const OperatorBindingScope binding = bind_operators(d_document, pulls, d_backend, d_pinned);

  // Composite, reap, repeat until a pass dispatches NOTHING -- that pass composited a
  // fully-warm cache and is the exact frame (Decision 3/4).
  //
  // One composite + one reap is NOT enough, and assuming it was is how an operator
  // scene silently exported blank. A pass only discovers the misses it actually pulls
  // for, and an operator whose input answers asynchronously degrades to a placeholder
  // and returns `exact == false` (`nested_content.cpp:398-403`) -- so the tiles BELOW
  // it (its input's input, or a sibling input it never reached because an earlier one
  // came back a placeholder) are not even requested until the operator is re-rendered
  // against warm inputs on the NEXT pass. Each round therefore warms strictly more of
  // the graph and dispatches strictly less; the loop terminates when a round dispatches
  // nothing, which is bounded by the graph's depth. A leaf-only scene still costs
  // exactly two passes, as before.
  //
  // `pending` is non-null on every pass (an async dispatch requires a reap sink, and a
  // pass that discovers a NEW miss must be able to record it), and `pulls` is served on
  // every pass so an operator layer's identity short-circuit is delivered here too
  // (`runtime.operator_identity_offline_delivery`, Constraint 6): an endpoint tile has
  // no operator-output cache entry to hit, so it is always a miss and must deliver its
  // terminal input through `pull`.
  for (;;) {
    render_frame_interactive(
        state, resolve, d_viewport, d_cache, d_backend, d_surfaces, frame, Deadline::none(),
        /*prior_revision=*/std::nullopt, &pending, &d_counters, /*dirty=*/nullptr, composition_time,
        /*visible_plans=*/nullptr, /*diagnostics=*/nullptr, &pulls, Exactness::Exact);
    if (pending.tiles.empty()) {
      break; // nothing was dispatched: this pass read an all-warm cache and is exact
    }
    // Reap to quiescence: offline has NO deadline, so wait until EVERY dispatched render
    // settles into the cache. `poll_refinements` inserts settled arrivals on the DRIVER
    // thread (workers never touch the cache, `runtime.threading`'s rule) and drops them
    // from the queue; unsettled ones are retained across the park.
    while (!pending.tiles.empty()) {
      d_pool.wait_completions(std::nullopt);
      poll_refinements(pending, d_cache, &d_counters, &d_backend);
    }
  }
  return std::move(*target);
}

void SequenceRenderer::render_sequence(std::span<const Time> frame_times, const FrameSink& sink) {
  for (const Time t : frame_times) {
    sink(t, render_frame_at(t));
  }
}

} // namespace arbc
