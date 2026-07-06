#pragma once

#include <arbc/base/geometry.hpp>
#include <arbc/base/time.hpp>
#include <arbc/cache/key_shapes.hpp>
#include <arbc/compositor/compositor.hpp>
#include <arbc/model/damage.hpp>
#include <arbc/model/model.hpp>

#include <cstddef>
#include <span>
#include <vector>

// The damage planner for `arbc::compositor` (L4, doc 17:56). Doc 02's
// interactive frame opens with "Collect damage ... No damage -> no work"
// (02:51): a frame does work only for what changed. `compositor.tile_planning`
// landed the plan/render/composite steps but plans the *whole* viewport every
// frame; the damage axis is absent. This component supplies it, turning the
// already-produced `Damage` stream -- model-emitted (`model.damage`),
// refinement-emitted (`poll_refinements`) -- into device dirty regions, cache
// invalidations, and temporal damage, plus one optional gating seam on the
// existing interactive driver.
//
// Three pure, caller-owned free functions plus one driver seam (refinement
// Decision 1): no `model::DamageSink` subscription, no cross-frame state at L4.
// The runtime loop (`runtime.interactive`, doc 17:60) owns *when* to collect
// damage and *whether* to schedule a follow-up frame; this component only maps,
// invalidates, and gates a single pass, returning plain values exactly as
// `poll_refinements` returns a vector rather than scheduling.
//
// `Damage.object` is read as the *content* id throughout (the id
// `poll_refinements` emits and the `TileKey.content` the cache is keyed by), so
// model-emitted and refinement-emitted damage flow through one homogeneous path.
// Levelization (doc 17:56): `model::Damage` is reached through the same
// transitive `model` visibility `render_frame_interactive` already uses for
// `DocRoot`; `cache::invalidate_region` is part of `cache`. No new DEPENDS edge,
// no `backend-cpu` edge.

namespace arbc {

// A caller-owned device-space dirty region: the union of viewport-pixel rects a
// gated interactive frame must re-plan and re-composite (Decision 2). Threaded
// into `render_frame_interactive` by optional pointer, exactly like
// `RefinementQueue*` / `CompositorCounters*`. Empty `device_rects` (a non-null
// but empty region) is the concrete "no damage -> no work": the driver plans
// nothing, issuing zero renders and zero composites (doc 02:51).
struct DirtyRegion {
  std::vector<Rect> device_rects;
};

// Project model/refinement `Damage` onto per-viewport device dirty rects (doc
// 02:51,57-60). For each `Damage`: (a) a temporal gate -- skip unless
// `range.empty() || range.contains(now)`, so an edit that only affects a
// non-displayed instant contributes nothing, while a degenerate/instant
// `TimeRange{when, when}` (the `poll_refinements` arrival shape) is read as
// present-frame damage, not no-time damage (Decision 3); (b) locate every
// visible layer showing `object` and compose its local->device transform via
// the anchor walk (`cull_walk`); (c) map the content-local `rect` to device via
// `Affine::map_rect`, intersect with the viewport rect, and push the non-empty
// result. Structural `Rect::infinite()` damage clips to the viewport rect (the
// conservative full-viewport footprint; this signature carries no
// `ContentResolver` to tighten to `bounds()`). Empty input, or all-gated /
// all-culled, returns an empty vector -- "no damage -> no work".
std::vector<Rect> map_damage_to_device(const DocRoot& state, const Viewport& viewport,
                                       std::span<const Damage> damage, Time now);

// Turn a clock advance into homogeneous `Damage` for the visible non-`Static`
// layers only (doc 11:133-137, the "clock advance is the temporal damage" axis).
// Walk the visible layers (the same cull walk the driver uses); for each layer
// whose resolved `Stability` is not `Static` (`Timed`/`Live`), emit
// `Damage{content, Rect::infinite(), advanced}` -- the sound whole-footprint
// over-approximation, since the compositor does not know *which* sub-region of a
// moving layer changed between two times (Decision 4). `Static` layers emit
// nothing (their keys omit `achieved_time` and are clock-invariant,
// re-asserting `11-time-and-video#static-tiles-survive-clock`). An advance over
// an all-`Static` scene, or an empty `advanced` range, returns empty -- playback
// of a still scene produces no damage, so the runtime schedules no frame. The
// result is homogeneous with model damage, so `map_damage_to_device` consumes it
// directly.
std::vector<Damage> clock_advance_damage(const DocRoot& state, const ContentResolver& resolve,
                                         const Viewport& viewport, const TimeRange& advanced);

// Drive the cache invalidation `cache.invalidation` reserved for this task (doc
// 02:94-95). For each `Damage{object, rect, range}`, drop the damaged tiles of
// `object` across all rungs, revisions, and achieved-times: a finite `rect`
// routes to `cache::invalidate_region` -- binding the injected `Geom` to
// `tile_local_rect` -- and a structural `Rect::infinite()` (or any non-finite)
// region routes to `cache::invalidate_content` (Decision 6). Revision
// invalidation stays lazy-by-keying; this driver covers only the spatial
// `(content, region)` axis and never calls `drop_superseded` (Decision 5).
// Returns the total number of tiles dropped.
std::size_t invalidate_damage(TileCache& cache, std::span<const Damage> damage);

} // namespace arbc
