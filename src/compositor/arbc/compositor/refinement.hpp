#pragma once

#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/cache/key_shapes.hpp>
#include <arbc/cache/keyed_store.hpp>
#include <arbc/cache/prefetch.hpp>
#include <arbc/compositor/counters.hpp>
#include <arbc/compositor/scale_ladder.hpp>
#include <arbc/compositor/tile_planning.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/model/damage.hpp>
#include <arbc/surface/surface.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

// The progressive-refinement + zoom-speculation layer for `arbc::compositor`
// (L4, doc 17:56), built on the interactive tiled path `compositor.tile_planning`
// shipped. It concretizes two settled doc promises the planner left as follow-up
// work orders (`tile_planning.md:214-218`):
//
//  1. **Doc 02:69-71 step 6 "Refine".** A miss the content answers
//     *asynchronously* (`Content::render` returns `nullopt`) is dropped when the
//     synchronous pass ends, so the sharp result never lands. `RefinementQueue`
//     is the caller-owned registry a frame records those deferred tiles into (via
//     `render_frame_interactive`'s optional `pending` seam), and
//     `poll_refinements` turns each settled arrival into a `Visible` cache insert
//     plus `model::Damage` for its region -- the damage that "schedules a
//     follow-up frame" so zooming shows progressively sharper content rather than
//     blocking. A poll with nothing settled emits **empty** damage ("no damage ->
//     no work", doc 02:51).
//
//  2. **Doc 04:99-101 zoom speculation + doc 02:92-93 pan priming.**
//     `zoom_prefetch_ring` re-tiles the visible local region at the neighbouring
//     rung (compositor knowledge -- it needs `tiles_covering`) and `prime_prefetch`
//     feeds that ring through `cache::prime_ring` under `PriorityClass::Speculative`
//     while driving the already-built `cache::pan_prefetch_ring` under `Adjacent`.
//     Priming is **residency-only**: it reclassifies resident ring members and
//     reports the absent ones as a want-list, rendering and evicting nothing
//     (`prefetch.hpp:17-25`). The want-list is `compositor.pull_service`'s to
//     render opportunistically under its low-priority class -- never inline on the
//     frame's critical path (doc 02:92-93 ranks these below `Visible`).
//
// **Not this component.** Async worker dispatch, off-thread completion, and
// rendering the want-list are `compositor.pull_service` + `runtime.interactive`;
// mapping the emitted `Damage` to device dirty regions and the follow-up-frame
// scheduler are `compositor.damage_planning` + `runtime.interactive`; driving the
// temporal (playback) prefetch ring is the runtime transport path. This
// component records/polls and primes; it schedules no render and wakes no caller
// (the wake is runtime policy, `content.hpp:117-118`).

namespace arbc {

// One deferred (async) tile render, recorded by `render_frame_interactive` and
// drained by `poll_refinements`. Frame-to-frame state, so the *caller* (the
// runtime frame loop) owns the queue -- planning stays pure and lock-free (doc
// 02:123-125), matching `tile_planning`'s "frame-to-frame state is the runtime
// loop's" rule.
//
// `surface` owns the render target the async worker writes the pixels into: the
// `RenderRequest` carries a `Surface&` into it, so the surface must outlive the
// frame that issued the request and be inserted (moved) into the cache on
// arrival. Moving the `unique_ptr` between frame and queue does not move the
// `Surface` object itself, so the `Surface&` the content holds stays valid.
// `bytes` is the tile byte cost captured at record time (identical to the inline
// path's `tile_byte_cost`), reused for the arrival's `cache.insert`.
struct PendingTile {
  TileKey key;
  Rect local_rect;
  ObjectId content;
  // The content's stability, captured at record time so `poll_refinements` can
  // apply the insert-site temporal-linkage check on the async arrival without
  // re-resolving the `Content*` (which the queue does not retain). `Static`
  // default keeps a hand-constructed test tile exempt (doc 11:134-137,
  // `timed_insert_key_consistent`).
  Stability stability{Stability::Static};
  std::size_t bytes{0};
  std::unique_ptr<Surface> surface;
  std::shared_ptr<RenderCompletion> done;
};

// The caller-owned pending-completion registry (doc 02:69-71). A plain value the
// runtime loop holds across frames; not compositor-global state (which would be
// hidden L4 frame-to-frame state and would make two viewports share a queue --
// see Decisions).
struct RefinementQueue {
  std::vector<PendingTile> tiles;
};

// The next-rung speculation ring for a zoom gesture (doc 04:99-101). Re-tiles
// `local_region` at the rung the gesture is heading toward via `tiles_covering`,
// and assembles the `TileKey` set at that rung carrying `revision` and, for
// `Timed` content, `achieved_time` (`Static` passes `std::nullopt` so the ring
// is clock-invariant, doc 11).
//
// `zoom_direction` is a caller-supplied sign -- the compositor infers no gesture
// (that would need inter-frame camera state, the runtime loop's, not L4
// planning's), mirroring `temporal_prefetch_ring`'s `direction` (doc 11:110-121):
//   * `< 0` (zoom-in, toward finer detail) -> `rung.index + 1`: the finer,
//     smaller-tile rung. In this codebase a *higher* rung index is finer
//     (`rung_scale = 2^index` device px per local unit, `scale_ladder.hpp:79-84`),
//     so the "next" downsample-preferred rung once available (doc 04:95-98) is
//     `index + 1`, and `tiles_covering` yields *more, smaller* tiles there.
//   * `> 0` (zoom-out) -> `rung.index - 1`: the coarser, fewer-tile rung.
//   * `== 0` -> empty ring (no gesture -> no speculation).
// The "previous zoom rung" half of doc 02:92-93 is served by the same builder
// with the opposite sign -- no separate path.
std::vector<TileKey> zoom_prefetch_ring(const RungSelection& current, const Rect& local_region,
                                        ObjectId content, std::uint64_t revision,
                                        std::optional<Time> achieved_time, int zoom_direction);

// The thin compositor prefetch driver (doc 02:92-93, 04:99-101). Assembles the
// visible layer's `TileKey` set from `plan`, builds the pan annulus
// (`cache::pan_prefetch_ring`, radius `pan_radius`) and the zoom next-rung ring
// (`zoom_prefetch_ring`, sign `zoom_direction`), primes each through the generic
// `cache::prime_ring` under `Adjacent` / `Speculative` respectively, and returns
// the merged **want-list** (the absent tiles). Renders nothing and evicts
// nothing -- the `prime_ring` residency-only invariant carries through, so
// `resident_bytes()` and `evictions()` are unchanged across the call
// (`prefetch.hpp:17-25`). The visible region, revision, and `achieved_time` the
// zoom ring needs are derived from `plan` (its tiles' rects and keys).
std::vector<TileKey> prime_prefetch(TileCache& cache, const LayerTilePlan& plan, int zoom_direction,
                                    std::int32_t pan_radius);

// Drain the settled arrivals from `queue` (doc 02:69-71 step 6). For each pending
// tile whose `RenderCompletion` has `settled()` with a value: move its rendered
// surface into a `Visible` cache insert under the tile's exact key, emit one
// `model::Damage{content, tile_local_rect(rung, coord), TimeRange{time, time}}` for the tile
// region (content-local terms, not device -- device mapping is
// `damage_planning`'s, doc 02:94), and drop it. Unsettled tiles are retained; a
// tile that settled via `fail` is dropped with no insert and no damage. The
// returned damage is what a follow-up frame keys off: a re-plan at the same
// revision then finds the tile `Fresh` and composites it sharp. An empty poll
// (nothing settled, or an empty queue) returns **empty** damage -- no follow-up
// frame (doc 02:51).
//
// When `counters` is non-null, `follow_up_frames` bumps once per arrival that
// settled into the cache and emitted damage (doc 16:54-62, `counters.hpp`);
// null (the default) is behavior-identical.
std::vector<Damage> poll_refinements(RefinementQueue& queue, TileCache& cache,
                                     CompositorCounters* counters = nullptr);

} // namespace arbc
