#include <arbc/runtime/host_viewport.hpp>

#include <chrono>
#include <optional>
#include <ratio>
#include <utility>
#include <vector>

namespace arbc {
namespace {

// The injected real-elapsed duration expressed as `Time` flicks. `Time` counts
// integer flicks (1/705'600'000 s, `time.hpp:8-14`); a period-`flicks_per_second`
// duration cast is the exact, wall-clock-free conversion of a monotonic-clock
// delta the free-run transport advance consumes (Constraint 4/8).
Time elapsed_flicks(std::chrono::steady_clock::duration delta) {
  using flick_duration =
      std::chrono::duration<std::int64_t, std::ratio<1, Time::flicks_per_second>>;
  return Time{std::chrono::duration_cast<flick_duration>(delta).count()};
}

} // namespace

HostViewport::HostViewport(InteractiveRenderer& renderer, Model& model, ContentResolver resolve,
                           Backend& backend, SurfacePool& pool, TileCache& cache, Surface& target,
                           Clock clock, Config config)
    : d_renderer(renderer), d_model(model), d_resolve(std::move(resolve)), d_backend(backend),
      d_pool(pool), d_cache(cache), d_target(target),
      d_clock(clock ? std::move(clock) : Clock{[] { return std::chrono::steady_clock::now(); }}),
      d_budget(config.budget), d_viewport(config.viewport), d_transport(config.transport_start),
      d_settle_loads(std::move(config.settle_external_loads)) {
  // Subscribe to model damage for this object's lifetime (Constraint 5, RAII). Two
  // mutually exclusive attach paths (`runtime.damage_router` Constraint 4): with a
  // router supplied, register `&d_sink` with it and hold the move-only
  // `Registration` (the router owns the single `set_damage_sink` slot and fans out
  // to this sink unchanged, `damage.hpp:74-78`); without one, keep the direct
  // single-slot install this object has always used.
  if (config.router != nullptr) {
    d_router = config.router;
    d_registration = d_router->register_sink(d_sink);
  } else {
    d_model.set_damage_sink(&d_sink);
  }
}

HostViewport::~HostViewport() {
  // Router path: `d_registration`'s destructor unregisters from the router (which
  // outlives this viewport). Direct path: release the model's single slot.
  if (d_router == nullptr) {
    d_model.set_damage_sink(nullptr);
  }
}

HostViewport::StepOutcome HostViewport::step() {
  // 0. Settle any external children whose bytes arrived since the last step
  //    (`runtime.async_external_load` Decision 7). This runs BEFORE the pin and before the
  //    damage drain, and both orderings are load-bearing: the install publishes a NEW
  //    revision, so a pin taken first would render the stale one, and it flushes damage
  //    naming the embedding content into `d_sink`, which step 3 drains -- so the frame that
  //    settles an arrival is the frame that composites it, and the placeholder is replaced
  //    live. This IS doc 02 step 1: an arrival is damage, and this is where damage is
  //    collected.
  if (d_settle_loads) {
    d_external_loads_settled += d_settle_loads();
  }

  // 1. Sample `composition_time` from the playhead policy (Decision 5). Audio-
  //    mastered: chase the lock-free snapshot, never advance the transport (the
  //    device monitor is its sole mutator). Free-run: advance the owned transport
  //    by the injected real elapsed and sample `position()`.
  Time composition_time;
  if (d_playhead_source) {
    composition_time = d_playhead_source();
  } else {
    const std::chrono::steady_clock::time_point now = d_clock();
    const Time elapsed =
        d_prev_instant.has_value() ? elapsed_flicks(now - *d_prev_instant) : Time::zero();
    d_prev_instant = now;
    const expected<Time, TimeError> advanced = d_transport.advance(elapsed);
    ++d_transport_advances;
    composition_time = advanced.has_value() ? *advanced : d_transport.position();
  }

  // 2. Pin the current version and apply the pure rebase across frames (Decisions
  //    2 & 4). A zoom-in re-anchor is already applied by `rebase`; push the
  //    ancestor + descent edge onto the anchor path. A zoom-out (`need ==
  //    zoom_out`) walks the runtime-held path: pop the top and rebuild the camera
  //    by inverting the stored edge, restoring the original `(anchor, matrix)`.
  const DocStatePtr state = d_model.current();
  StepOutcome outcome;
  const RebaseResult rebased = rebase(*state, d_viewport);
  outcome.need = rebased.need;
  if (rebased.event.occurred) {
    d_anchor_path.push_back(AnchorFrame{rebased.event.from, rebased.edge});
    d_viewport = rebased.viewport;
    outcome.reanchor = rebased.event;
    ++d_reanchor_events;
  } else if (rebased.need == RebaseNeed::zoom_out && !d_anchor_path.empty()) {
    const AnchorFrame frame = d_anchor_path.back();
    // Invert the stored descent edge to re-anchor upward. A degenerate edge culls
    // rather than propagating NaNs (Constraint 9, doc 04:116-117) -- leave the
    // viewport untouched and keep the frame on the path.
    if (const std::optional<Affine> inv = frame.edge.inverse(); inv.has_value()) {
      const ObjectId from = d_viewport.anchor;
      d_anchor_path.pop_back();
      d_viewport.camera = reanchor_camera(d_viewport.camera, *inv);
      d_viewport.anchor = frame.anchor;
      outcome.reanchor = Reanchor{true, from, frame.anchor};
      ++d_reanchor_events;
    }
  }

  // 3. Drain accumulated model damage and decide whether to render. A step with no
  //    pending damage, no owed follow-up, and a still scene issues zero
  //    `render_frame` invocations (Constraint 7, doc 01:140).
  std::vector<Damage> damage = d_sink.drain();
  const bool scene_moved = d_rendered_once && (d_viewport.camera != d_last_render_camera ||
                                               d_viewport.anchor != d_last_render_anchor ||
                                               composition_time != d_last_render_time);
  if (damage.empty() && !d_follow_up_owed && !scene_moved) {
    return outcome; // idle: a still scene costs nothing
  }

  const InteractiveRenderer::FrameOutcome frame =
      d_renderer.render_frame(*state, d_resolve, d_viewport, d_cache, d_backend, d_pool, d_target,
                              damage, composition_time, d_budget);
  ++d_frames_issued;
  d_follow_up_owed = frame.schedule_follow_up;
  d_rendered_once = true;
  d_last_render_camera = d_viewport.camera;
  d_last_render_anchor = d_viewport.anchor;
  d_last_render_time = composition_time;
  outcome.schedule_follow_up = frame.schedule_follow_up;
  return outcome;
}

} // namespace arbc
