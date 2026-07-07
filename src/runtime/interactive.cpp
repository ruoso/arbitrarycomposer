#include <arbc/base/transform.hpp> // Affine::max_scale
#include <arbc/runtime/interactive.hpp>

#include <chrono>
#include <cstdint>
#include <utility>
#include <vector>

namespace arbc {

namespace {

// One-ring pan prefetch (doc 02:92-93 "adjacent (pan prefetch ring)"): the
// immediately-adjacent tile annulus (Chebyshev radius 1) the next pan step
// reveals at the leading edge. A named constant `compositor.pull_service` (M4)
// can tune when it turns the want-list into scheduled prefetch renders.
constexpr std::int32_t k_pan_prefetch_radius = 1;

} // namespace

int zoom_direction_from_scale_delta(double prev_scale, double scale) noexcept {
  if (!(prev_scale > 0.0)) {
    return 0; // no prior frame -> no gesture
  }
  if (scale > prev_scale) {
    return 1;
  }
  if (scale < prev_scale) {
    return -1;
  }
  return 0; // camera unchanged
}

InteractiveRenderer::InteractiveRenderer(WorkerPoolConfig pool_config, Clock clock)
    : d_pool(std::move(pool_config)),
      d_clock(clock ? std::move(clock) : Clock{[] { return std::chrono::steady_clock::now(); }}) {}

InteractiveRenderer::FrameOutcome
InteractiveRenderer::render_frame(const DocRoot& state, const ContentResolver& resolve,
                                  const Viewport& viewport, TileCache& cache, Backend& backend,
                                  SurfacePool& pool, Surface& target,
                                  std::span<const Damage> model_damage, Time composition_time,
                                  std::chrono::steady_clock::duration budget) {
  const std::uint64_t revision = state.revision();
  // The very first frame has no previous time, so a clock advance is a no-op and
  // the frame plans the WHOLE viewport (null `DirtyRegion`) rather than gating on
  // an as-yet-nonexistent damage stream.
  const bool first_frame = !d_prev_time.has_value();

  // --- Step 1: collect damage (doc 02:51-52). ------------------------------
  // The host-drained model damage unions with the clock-advance temporal damage
  // for the visible moving (non-Static) layers. Together this is the
  // CONTENT-changing damage: what actually invalidates cached tiles.
  const TimeRange advanced{d_prev_time.value_or(composition_time), composition_time};
  const std::vector<Damage> clock_damage = clock_advance_damage(state, resolve, viewport, advanced);
  std::vector<Damage> content_damage(model_damage.begin(), model_damage.end());
  for (const Damage& d : clock_damage) {
    damage_add(content_damage, d);
  }

  // --- Step 2: map to device (doc 02:51,57-60). ----------------------------
  // The dirty region unions three sources, each gated at the instant it applies:
  //  * model damage + the arrival damage carried from the previous frame's poll,
  //    gated at the displayed instant `composition_time` (carried damage re-plans
  //    the refined tiles WITHOUT invalidating them -- see Step 3);
  //  * clock-advance damage, which is THIS frame's moving layers and therefore
  //    present-frame by construction. Its range is the half-open advance interval
  //    `[prev, now)`, which EXCLUDES the endpoint `now`, so gating it at
  //    `composition_time` would drop it; gate it at `advanced.start` instead --
  //    an instant the advance range provably covers -- so a moving layer re-plans.
  std::vector<Damage> present_damage(model_damage.begin(), model_damage.end());
  for (const Damage& d : d_carried_damage) {
    damage_add(present_damage, d);
  }
  d_carried_damage.clear();
  std::vector<Rect> device_rects =
      map_damage_to_device(state, viewport, present_damage, composition_time);
  if (!clock_damage.empty()) {
    const std::vector<Rect> clock_rects =
        map_damage_to_device(state, viewport, clock_damage, advanced.start);
    device_rects.insert(device_rects.end(), clock_rects.begin(), clock_rects.end());
  }
  DirtyRegion dirty{std::move(device_rects)};

  // "No damage -> no work" (doc 02:51): a non-first frame whose collected damage
  // maps to an empty device dirty region AND whose refinement queue is empty
  // plans nothing, renders nothing, composites nothing (it does NOT clear
  // `target`), and schedules no follow-up frame -- zero deltas on
  // requests_issued / composites / follow_up_frames.
  if (!first_frame && dirty.device_rects.empty() && d_pending.tiles.empty()) {
    d_prior_revision = revision;
    d_prev_time = composition_time;
    return FrameOutcome{false};
  }

  // --- Step 3: invalidate (doc 02:63). -------------------------------------
  // Drop the damaged tiles across rungs so the re-plan sees a miss where content
  // changed. Only content damage invalidates (see the carried-damage note above).
  invalidate_damage(cache, content_damage);

  // --- Step 4: plan + render misses within budget (doc 02:61-65). ----------
  // One frame deadline instant `d`, sampled from the injected clock exactly once:
  // it is both the `Deadline` value stamped onto miss requests and the
  // `wait_completions` park bound -- one instant, two uses, no drift.
  const std::chrono::steady_clock::time_point deadline_at = d_clock() + budget;
  const Deadline deadline{deadline_at};
  // The first frame plans the whole viewport (null dirty clears + full re-plan);
  // every later frame is damage-gated onto the caller-persisted `target`.
  const DirtyRegion* const dirty_ptr = first_frame ? nullptr : &dirty;
  // Surface the per-visible-layer plans the driver composited from so Step 7 can
  // drive speculation render-free without re-planning (the whole point of this
  // seam). A frame-local: consumed by Step 7 and dropped at frame end, so the
  // plan stays a pure per-frame value and no plan state crosses frames.
  std::vector<LayerTilePlan> visible_plans;
  render_frame_interactive(state, resolve, viewport, cache, backend, pool, target, deadline,
                           d_prior_revision, &d_pending, &d_counters, dirty_ptr, composition_time,
                           &visible_plans);

  // --- Step 5: park to the deadline; enforce it (doc 02:61-65, 140-143). ----
  // Park for async arrivals only until `d`. `wait_completions` returns whether a
  // completion settled since the last drain: `false` means the park reached the
  // deadline with nothing fresh settled -- the deadline has passed, so cancel the
  // still-unsettled BestEffort pending renders (advisory, `content.hpp:122-123`)
  // rather than wait; the frame never blocks past `d`. A `true` return means an
  // arrival settled (a `poke`), so we may be before `d` -- this frame's misses
  // are not yet expired and are left to settle/be reaped.
  const bool ready = d_pool.wait_completions(deadline_at);
  if (!ready) {
    for (PendingTile& tile : d_pending.tiles) {
      if (!tile.done->settled()) {
        tile.done->cancel();
      }
    }
  }

  // --- Step 6: refine + composite arrivals (doc 02:66-71). -----------------
  // Insert settled arrivals into the cache and collect their content-local
  // damage. A failed/cancelled arrival is dropped by `poll_refinements` (no
  // insert, no damage) -- degrading to the placeholder policy, never thrown.
  const std::vector<Damage> arrival = poll_refinements(d_pending, cache, &d_counters, &backend);
  // A follow-up frame is owed exactly when the arrival damage maps to a non-empty
  // device dirty region (doc 02:69-71). The carried copy re-plans those tiles
  // Fresh on the next frame (device mapping happens fresh there in case the
  // camera moved between frames).
  const std::vector<Rect> arrival_device =
      map_damage_to_device(state, viewport, arrival, composition_time);
  d_carried_damage = arrival;
  const bool schedule_follow_up = !arrival_device.empty();

  // --- Step 7: speculation (doc 02:92-93, 04:99-101). ----------------------
  // Warm the prefetch rings' residency render-free by driving
  // `compositor::prime_prefetch` from the plans the frame ALREADY composited
  // (surfaced through `render_frame_interactive`'s `visible_plans` sink) rather
  // than re-planning -- so this pass adds zero cache probes over the plan pass and
  // leaves `requests_issued`/`composites` untouched (claim
  // `02-architecture#speculation-drives-from-exposed-plan`). `zoom_direction` is
  // the sign of the frame-over-frame camera-scale delta (Decision 5): the loop is
  // the only place that sees successive viewports, and `prime_prefetch` takes a
  // caller-supplied sign because "the compositor infers no gesture"
  // (`refinement.hpp:95`). `0` on the first rendered frame or a still camera. The
  // returned want-lists are NOT rendered here -- rendering them is
  // `compositor.pull_service`'s (M4); M3's Step 7 only reclassifies resident
  // pan/zoom-ring tiles, which is render-free and composite-free.
  const double camera_scale = viewport.camera.max_scale();
  const int zoom_direction = zoom_direction_from_scale_delta(d_prev_camera_scale, camera_scale);
  for (const LayerTilePlan& plan : visible_plans) {
    prime_prefetch(cache, plan, zoom_direction, k_pan_prefetch_radius);
  }

  // --- Step 8: advance state. ----------------------------------------------
  d_prior_revision = revision;
  d_prev_time = composition_time;
  d_prev_camera_scale = camera_scale;
  return FrameOutcome{schedule_follow_up};
}

} // namespace arbc
