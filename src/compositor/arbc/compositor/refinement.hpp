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

// Forward-declared so `poll_refinements` can take an optional `Backend*` for the
// copy-to-cache of an async-arriving content-provided surface (doc 09:87-100)
// without pulling the backend header into this one.
class Backend;

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

// One operator output tile's REFINEMENT WAVE
// (`compositor.operator_refinement_wave_amplification`, doc 02 § The frame,
// interactively). When an operator renders and one or more of its input tiles
// answer asynchronously it paints a placeholder and reports `exact = false`
// (doc 13:135-146 -- it MUST, or the empty tile freezes into the cache as a final
// answer). That transient tile is resident under its exact key but is not a hit,
// so the next plan is a miss and the whole chain renders again -- once per
// INDEPENDENTLY ARRIVING input tile, which is the amplification.
//
// `output` is that transient tile; `unmet` is the set of input tiles its render
// left unmet -- its UNMET SET, and the wave's boundary. The wave is a SET, not a
// duration: it ends when the last of `unmet` leaves `RefinementQueue::tiles`
// (settled, failed, or dropped), and no timer, wall-clock, or frame counter is
// consulted anywhere.
//
// The set is TRANSITIVE through a nested chain for free, because worker dispatch
// is leaf-only (doc 02:220-233): an operator renders INLINE on the driver thread,
// so a nested composition's render pulls its child fades, each of which pulls its
// own leaf within the same call. The pull service accumulates every unmet input
// tile as it goes, and each render site takes the tail since its own mark -- so a
// fade waits on just its leaf, and the nested tile above it waits on the union of
// the whole subtree.
struct OperatorWait {
  TileKey output;             // the transient tile whose render left inputs unmet
  std::vector<TileKey> unmet; // the input tiles it is waiting on
};

// The caller-owned pending-completion registry (doc 02:69-71). A plain value the
// runtime loop holds across frames; not compositor-global state (which would be
// hidden L4 frame-to-frame state and would make two viewports share a queue --
// see Decisions).
//
// `waits` rides beside `tiles` because a wave spans frames and so is persistent
// state, which doc 17:118-128 puts in `runtime`, not in the stateless per-frame
// compositor library -- and `RefinementQueue` is the caller-owned struct runtime
// already holds and already threads to BOTH gate sites (the driver's tile loop and
// `PullServiceImpl`). Putting the waits beside the tiles they name costs zero new
// plumbing and zero new levelization edges. It is bounded by the number of distinct
// operator output tiles with work in flight -- the same order as `tiles` itself --
// and `poll_refinements` prunes a wait whose unmet set has fully drained.
struct RefinementQueue {
  std::vector<PendingTile> tiles;
  std::vector<OperatorWait> waits;
};

// Record (or replace) the wave `output` is waiting on. Called by the two RENDER
// sites -- the driver's tile loop and `PullServiceImpl::pull`'s dispatch -- with
// the input tiles the pull service left unmet during that render, whenever the
// render came back INEXACT. An empty `unmet` records nothing (an inexact leaf
// pulled no inputs and is waiting on nobody, so it has no wave and must keep
// re-rendering as it does today). A re-render of the same `output` REPLACES its
// predecessor's wait: the fresh render's unmet set is the current truth.
void record_operator_wait(RefinementQueue& queue, const TileKey& output,
                          std::vector<TileKey> unmet);

// Is `output`'s refinement wave still running -- i.e. is any input tile its last
// render left unmet still pending? (`compositor.operator_refinement_wave_amplification`,
// doc 02 § The frame, interactively.) The THIRD thing a miss is checked against,
// after the cache and the pending set, at the same two dispatch sites. A `true`
// answer means "more is genuinely coming for this tile -- do not re-render the chain
// yet"; the frame composites the resident transient tile instead (see
// `TileSource::Transient`), drives no render, and issues no pull.
//
// A null `queue` is "no wave" (the offline driver's exact first pass plans with no
// queue), so the null path stays byte-identical.
//
// AN UNMET INPUT IS "STILL PENDING" IFF IT IS STILL IN `queue.tiles` -- presence, and
// nothing else. NOT `!settled()`, and NOT `!cancelled()`. Both omissions are
// load-bearing, and both are where this predicate deliberately parts company with
// `tile_in_flight`:
//
//  * `tile_in_flight` excludes CANCELLED entries because suppressing a DISPATCH against
//    one risks a permanent hole: `cancel` is advisory, a conformant content may honor it
//    and settle via `fail`, and `poll_refinements` drops a failed arrival with no insert
//    and no damage -- so the tile would be in neither the cache nor the queue, with
//    nothing to re-plan it. Deferring a RE-RENDER against one cannot do that: the
//    operator's tile is already resident and already composited, so the user sees the
//    placeholder they were going to see anyway, and both futures of a cancelled entry
//    open the gate (it lands and `poll_refinements` drains it, or it fails and
//    `poll_refinements` drops it -- either way it leaves `queue.tiles`). If this gate
//    used `tile_in_flight` instead, the interactive driver's deadline sweep -- which
//    cancels EVERY unsettled tile on expiry -- would open every gate at every frame
//    boundary, and the coalescing would evaporate in exactly the regime it exists for.
//
//  * A SETTLED-but-not-yet-drained entry likewise keeps the wave open, because its
//    pixels are not in the CACHE yet: the insert happens in `poll_refinements`, on the
//    frame thread, not on the worker that settled it. A chain re-rendered against it
//    right now would miss it in the cache and re-dispatch a render that has already
//    finished. More IS coming for that tile -- from the next poll, which also emits the
//    damage that re-drives the chain properly.
//
// So the wave ends when its inputs leave the QUEUE, which is the same event
// `poll_refinements`' prune keys on (Constraint 3: settled, failed, or dropped). That
// poll runs unconditionally every frame, before any re-plan, so no entry is held past
// the drain it is about to get and the gate cannot stall.
//
// A linear scan, for the same reason `tile_in_flight` is one: `queue.tiles` is bounded
// by the frame's outstanding misses, and this is paid only on the miss path (a warm
// frame never calls it), so the seam is here to hide an index behind if it ever
// profiles hot.
bool operator_wave_pending(const RefinementQueue* queue, const TileKey& output) noexcept;

// The still-pending members of `output`'s recorded unmet set (empty iff
// `operator_wave_pending` is false). This is what makes the unmet set transitive
// through a WAVE-DEFERRED pull: when a parent operator pulls a child operator tile
// whose wave is still running, the child delivers its transient tile and the parent
// inherits the child's outstanding inputs as its own unmet keys -- so the parent's
// wait names leaf tiles that are really in `queue.tiles`, and its gate opens on the
// same wave the child's does, not one frame later.
std::vector<TileKey> operator_wave_unmet(const RefinementQueue* queue, const TileKey& output);

// Is `key`'s render already in flight? (`compositor.in_flight_tile_dedup`, doc 02
// § The frame, interactively.) The pending set is the second suppression key
// beside the cache: a dispatched-but-unlanded tile is ABSENT from the cache, so on
// the cache alone it is indistinguishable from a tile nobody has asked for, and
// both dispatch sites (the driver's tile loop, `PullService::pull`'s per-covering-
// tile loop) re-dispatch it. They consult this on a miss instead; a `true` answer
// means "someone is already rendering exactly this tile -- do not render it again,
// their arrival will re-drive you".
//
// It is a linear scan, deliberately, not an index inside `RefinementQueue`: the
// answer is not a function of the queue's MUTATIONS. It depends on `settled()` and
// `cancelled()`, two atomics a renderer thread flips with no notification to the
// queue, so any precomputed set would be stale the instant a worker settled and
// would have to be re-validated against the completions on every query anyway --
// which is this scan, plus an invariant to get wrong. `pending` is bounded by the
// frame's outstanding misses and this is paid only on the miss path (a warm frame
// never calls it), so the seam is here to hide an index behind if it ever profiles
// hot.
//
// A null `queue` is "nothing in flight" (the offline driver's first pass plans with
// no queue; `render_frame_interactive`'s `pending` is optional by contract), so the
// null path stays byte-identical.
//
// SETTLED and CANCELLED entries both answer `false`, and the `cancelled()` clause is
// load-bearing, not defensive. `cancel` is ADVISORY: it does not settle the
// completion, and it leaves a conformant content free to honor it and settle via
// `fail` (`content.hpp:161-163`). `poll_refinements` drops a failed arrival with no
// cache insert and NO DAMAGE (`refinement.cpp:122-124`) -- which is what stops a
// persistently-failing tile from spinning the refinement loop forever. Suppress
// against a cancelled entry and those two facts compose into a permanent hole: the
// tile is in neither the cache nor the queue, nothing ever damaged it, and nothing
// re-plans it. It shows a placeholder until some unrelated edit happens to repaint
// the region. Re-dispatching a cancelled tile costs a render that was probably
// going to land anyway; suppressing against one costs the tile.
//
// The cross-thread reads are the same ones the interactive driver's own unsettled()
// park predicate already makes (`interactive.cpp:369-372`) -- no lock, no new
// atomic, no new shared state; the queue stays frame-thread-only.
bool tile_in_flight(const RefinementQueue* queue, const TileKey& key) noexcept;

// Does the frame still WANT `key`? (`runtime.deadline_cancel_retains_wanted`, doc 02
// § The frame, interactively.) The predicate the interactive driver's deadline sweep
// narrows its blanket cancel against: on expiry it cancels an unsettled pending tile
// only when this answers `false`, and leaves the rest in flight, uncancelled, so the
// next frame that re-plans them joins the render already running (`tile_in_flight`)
// rather than dispatching a second one. It answers WANTED, not LIVE -- a settled entry
// is wanted like any other -- and the sweep composes it with `!settled()`.
//
//     wanted.contains(key)
//       || any wait in queue.waits where wanted.contains(wait.output)
//                                 and  wait.unmet contains key
//
// The second clause is not an optimization; without it the sweep strands operator
// inputs. When the wave gate defers an operator's output re-render
// (`operator_wave_pending`), the operator does not render, so it does not pull, so its
// in-flight input tiles are named by NOTHING in this frame's footprint -- the footprint
// holds the operator's OUTPUT tiles, a different content and a different key. Cancel
// those inputs and the wave never ends: the deferred output tile composites its
// transient placeholder forever.
//
// The `wanted.contains(wait.output)` guard is what stops the clause degenerating into
// "anything ever waited on is forever wanted". A wait whose output the frame no longer
// wants -- panned away, revision-bumped -- retains nothing, and its inputs are cancelled
// like any other unwanted pending. That is exactly the case
// `operator_refinement_wave_amplification`'s Decision 4 anticipated, and it stays
// harmless: `operator_wave_pending` ignores `cancelled()` by construction, so a
// cancelled input still holds its (now-unwanted) output's gate closed until it leaves
// the queue, and both leave together on the next drain.
//
// It lives here, beside the gates it complements, rather than in the driver: it is a
// statement about `RefinementQueue` semantics (L4), and the runtime loop (L5) should not
// be the second place that knows what `OperatorWait::unmet` means. `O(pending x waits x
// unmet)`, paid only on the expiry path.
//
// `wanted` is taken by CONST REFERENCE, not as a nullable pointer like the sinks above,
// and that asymmetry is deliberate: an absent wanted set means "nothing is wanted",
// which is the blanket cancel wearing a disguise, and it must not be representable at
// the one call site that would act on it. The compositor-side SINKS stay nullable (the
// offline and one-shot drivers pass nothing, and they never sweep).
bool tile_wanted(const RefinementQueue& queue, const WantedTiles& wanted, const TileKey& key);

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
//
// `backend` (optional) is used only to copy an async arrival that carried a
// content-provided surface into its cache-owned tile surface (doc 09:87-100),
// exactly as the inline consumption sites do; an ordinary arrival that filled
// its target ignores it, so a null `backend` (the default) is behavior-identical
// for every arrival that did not provide a surface.
//
// It also PRUNES the refinement waves the drain ended: after the retain/insert
// pass, every `OperatorWait` whose `unmet` set names no tile still pending in
// `queue.tiles` is erased. One pass, and it handles every expiry route at once --
// inputs that settled (removed by the retain), inputs that failed and were dropped
// (likewise), and waits whose output key is superseded by a revision bump (their
// inputs drain out like any others, and the stale output key is unmatchable
// anyway). Pruning is what keeps `waits` bounded; it is not what makes the gate
// open, since `operator_wave_pending` already answers `false` for a fully-drained
// unmet set (Constraint 3: a wave that ends always releases what it gated).
std::vector<Damage> poll_refinements(RefinementQueue& queue, TileCache& cache,
                                     CompositorCounters* counters = nullptr,
                                     Backend* backend = nullptr);

} // namespace arbc
