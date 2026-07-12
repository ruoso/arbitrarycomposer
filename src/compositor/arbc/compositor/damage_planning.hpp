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

// The **degenerate one-rect normalization** of the frame's damage: the bounding
// box of `dirty.device_rects` -- each first intersected with the viewport, so a
// structural `Rect::infinite()` rect saturates rather than poisoning the box --
// rounded **out** to whole device pixels. Empty (the default `Rect{}`) iff the
// region is empty or maps entirely outside the viewport, which is the concrete
// "no damage -> no work".
//
// This is no longer what a gated frame repaints -- `repaint_regions` below is
// (doc 02 § The frame, interactively) -- but it remains load-bearing in two
// places, which is why it stays a published, tested seam:
//
//   1. it is the **fallback** `repaint_regions` returns when a pathological rect
//      count blows past `k_max_repaint_rects` (`disjoint_dirty_repaint` Decision
//      3): not an approximation but the shipped, byte-exact behavior, whose union
//      is a superset of the disjoint set's, so no damage is ever missed;
//   2. it is the **bounding box** of `repaint_regions`' output, always -- because
//      `floor`/`ceil` distribute over `min`/`max`, so rounding out each rect and
//      then boxing equals boxing and then rounding out. That identity is what ties
//      the two functions together and makes (1) coherent rather than a special
//      case.
//
// *Why the bounding box is not enough:* the per-layer gate, the clear and the clip
// all derive from the repaint region, so a one-rect region makes the idempotence
// proof trivial -- the planned set, the cleared set and the painted set are the
// same set. But `map_damage_to_device` emits one rect per (damage, layer) pair,
// which may OVERLAP, and clipping each tile once per *raw* dirty rect would
// composite it twice in the overlap: the very double-blend the clear exists to
// prevent. The bbox dodged that by collapsing the rects to one -- at the cost of
// repainting everything between two far-apart damages. `repaint_regions` solves it
// properly, by making the rects genuinely disjoint first.
//
// *Why round out:* the dirty rects are `map_rect` outputs -- arbitrary doubles.
// Rounding in would leave a sub-pixel fringe of the true damage unpainted (a
// stale seam); rounding out is conservative in the safe direction, and because
// the same rounded rects gate the plan, the extra pixels are covered by every
// layer that covers them.
Rect repaint_region(const DirtyRegion& dirty, const Viewport& viewport);

// The rect-count cap on a frame's repaint set (`disjoint_dirty_repaint` Decision
// 3). A band sweep over *n* input rects can emit O(n^2) output rects in the worst
// case (a diagonal staircase: ~n bands x ~n runs), and the input is
// one-rect-per-(damage, layer) -- a 30-damage commit over 10 visible layers is 300
// input rects, a plausible number whose worst-case decomposition is far worse than
// the bbox it would replace. Over the cap, `repaint_regions` returns the bbox, so a
// pathological input degrades gracefully to today's behavior instead of off a
// performance cliff in a loop that has a deadline. 64 is comfortably above any
// realistic interactive frame (a handful of damages x a handful of layers, most of
// which overlap and collapse) and comfortably below the point where per-rect frame
// overhead -- N plan-gate `map_rect`s per layer, N `clear_rect` calls -- would
// exceed the blend work it saves.
inline constexpr std::size_t k_max_repaint_rects = 64;

// The frame's device **repaint region**: a set of **pairwise-disjoint**,
// integer-aligned rects inside the viewport whose union is EXACTLY the union of
// the (viewport-clipped, rounded-out) device dirty rects (doc 02 § The frame,
// interactively; `compositor.disjoint_dirty_repaint`). Empty iff the region is
// empty or maps entirely outside the viewport -- "no damage -> no work" (doc
// 02:51), realized as a vector whose emptiness makes every per-rect loop in the
// frame run zero times.
//
// The gated frame uses this ONE set three times, per rect: it gates each layer's
// plan (each rect mapped back through the layer's inverse into local space), each
// rect is a `Backend::clear_rect` argument, and each rect is the `device_clip` on
// every composite of a tile that rect planned. The planned set, the cleared set and
// the painted set are therefore the same set -- and **disjointness** is what keeps
// that statement true now that the set is more than one rect, because it is what
// guarantees no pixel belongs to two rects and so no pixel is cleared twice or
// composited twice. A tile straddling two rects is planned twice and composited
// once *per rect*, each composite clipped to its own rect: the pixels are still
// written exactly once each.
//
// Three properties are the whole contract, and all three fall out of the
// construction rather than being checked:
//
//   - **Pairwise disjoint.** The band sweep emits one rect per (y-band, merged
//     x-run), and bands are half-open and runs within a band are disjoint by the
//     merge.
//   - **Integer-aligned.** Each input rect is clipped to the viewport and rounded
//     OUT *before* the decomposition, so the sweep runs on integer rects and its
//     outputs -- whose edges are all input edges -- are integer by construction.
//     (Rounding in would leave a sub-pixel stale seam; see `repaint_region`.)
//   - **Union-exact.** Not a superset (that is the waste this replaces) and
//     emphatically not a subset (that leaves an undamaged-looking pixel that is in
//     fact damaged). The sweep partitions the input union and re-emits every part.
//
// Clipping to the viewport BEFORE the sweep is also what keeps a structural
// `Rect::infinite()` damage rect from poisoning it: infinite saturates to the
// viewport rect, exactly as it does in `repaint_region`.
//
// The algorithm (Decision 2), the classic scanline region decomposition: clip and
// round out each input rect; collect the distinct y-edges into half-open y-bands;
// within each band merge the x-intervals of every rect spanning it into disjoint
// x-runs; emit one rect per (band, run); coalesce vertically-adjacent bands whose
// runs are identical, so two plainly non-overlapping rects come back out as two
// rects rather than three bands. Over `k_max_repaint_rects` output rects the
// decomposition is abandoned for `{repaint_region(dirty, viewport)}`.
std::vector<Rect> repaint_regions(const DirtyRegion& dirty, const Viewport& viewport);

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
