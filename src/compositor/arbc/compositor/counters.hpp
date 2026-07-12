#pragma once

#include <arbc/cache/key_shapes.hpp>
#include <arbc/cache/keyed_store.hpp>

#include <cstdint>

// The compositor's wall-clock-free behavioral-counter surface (doc 16:54-62,
// L4 doc 17:56). Doc 16 makes counters the non-flaky substitute for wall-clock
// performance gates ("wall-clock tests lie in CI; counters don't"): the
// interactive pipeline's efficiency promises -- a still scene playing back
// issues zero render requests, a warm re-plan is all cache hits -- are asserted
// on *counts*, not timings.
//
// The compositor is a pure per-frame library (`compositor.hpp:26`): it holds no
// persistent counter state. `CompositorCounters` is therefore CALLER-OWNED and
// threaded into `render_frame_interactive` / `poll_refinements` by optional
// pointer, exactly as the `SurfacePool` / `TileCache` / `RefinementQueue` frame
// state already is (doc 17 "debug counters is a convention, not a central
// registry"). Persistence across frames is the runtime loop's business.
//
// Counts are plain `std::uint64_t` bumped on the single-threaded driver path
// (doc 02:135-137) -- no `std::atomic`; the async worker pool is
// `compositor.pull_service`, which owns any threading when it wraps this seam.

namespace arbc {

// The three counts the compositor uniquely knows. `cache` already owns its own
// `hits()/misses()/evictions()` (Decision 2), so this struct never shadows
// them -- the `counters_snapshot` view composes the two. Private `d_` storage
// with `noexcept` accessors matches the established per-component counter style
// (`keyed_store.hpp:193`, `pool/arbc/pool/checkpoint.hpp:209-216`); the `note_*`
// mutators are the three seams the interactive driver bumps in place.
class CompositorCounters {
public:
  // Inline miss renders actually *driven* (Decision 5): one bump per
  // `content->render` call the driver issues for a fresh-key miss, not per
  // planned miss (a coarser/placeholder fallback that is not inline-filled
  // issues no `RenderRequest`).
  std::uint64_t requests_issued() const noexcept { return d_requests_issued; }
  // Every `Backend::composite` call the tiled driver makes, coarser-fallback
  // upscales included.
  std::uint64_t composites() const noexcept { return d_composites; }
  // Refinement arrivals that settled into the cache and emitted damage in a
  // `poll_refinements` call.
  std::uint64_t follow_up_frames() const noexcept { return d_follow_up_frames; }
  // Renders the driver issued on an operator-typed content (`inputs()` non-empty)
  // -- one bump per `content->render` the driver drives for an operator layer's
  // miss (`compositor.operator_graph`, doc 13:124-128). An `identity()`
  // short-circuit issues no operator render, so it leaves this at zero: the
  // behavioral proof of "identity fades issue zero operator renders". A subset
  // of `requests_issued` (every operator render is also a render request); a
  // leaf layer never bumps it, so the counter is 0 on the flat leaf path.
  std::uint64_t operator_renders() const noexcept { return d_operator_renders; }
  // Composites of a DEGRADED display source -- a stale-revision, coarser-rung,
  // or placeholder tile shown when the fresh exact tile was not (yet) resident
  // (doc 02:63-65). The interactive loop expects these during progressive
  // refinement; the OFFLINE driver asserts this reads ZERO, the behavioral proof
  // that an exported frame composites only fresh, exact-scale, current-revision
  // tiles and never substitutes a degraded one (doc 02:73-85,
  // `02-architecture#offline-frame-renders-exactly-no-degrade`). A subset-free
  // sibling of `composites()`: it counts the same composite events partitioned
  // by whether the display source was fresh, so a still-refining interactive
  // frame reads it non-zero while an offline frame reads it zero.
  std::uint64_t degraded_composites() const noexcept { return d_degraded_composites; }
  // Audio renders `PullService::pull_audio` dispatched onto the audio worker seam
  // (doc 12:31-34): one bump per block-cache miss that dispatches, zero on a
  // resident exact-fresh hit -- the audio twin of `requests_issued`. This is the
  // behavioral witness of doc 12's "purely visual content costs the audio engine
  // nothing" (`12-audio#mix-engine-facetless-costs-nothing`): the mix engine over
  // an all-culled composition dispatches zero, over N audible in-span layers with
  // an audio facet dispatches exactly N (`registry.tsv:70`).
  std::uint64_t audio_dispatches() const noexcept { return d_audio_dispatches; }
  // Renders NOT issued because the tile's render was already in flight
  // (`compositor.in_flight_tile_dedup`, doc 02 § The frame, interactively): one
  // bump per fresh-key miss the dispatch guard suppressed, at either dispatch site
  // (the driver's tile loop, `PullService::pull`'s per-covering-tile loop). It is
  // the positive witness of the dedup, and it exists because its absence is not
  // one: "`requests_issued` did not grow" is equally what you observe when the
  // guard never fires, when the cache was quietly warm, or when a refactor
  // disconnects the guard. A suppressed render bumps NEITHER `requests_issued` nor
  // `operator_renders` -- it is not a render driven -- so this counter is disjoint
  // from both, and the frame's total ask is `requests_issued + requests_suppressed`
  // against `requests_issued` distinct tile keys.
  std::uint64_t requests_suppressed() const noexcept { return d_requests_suppressed; }
  // Chain re-renders NOT driven because the operator tile's refinement WAVE is
  // still running (`compositor.operator_refinement_wave_amplification`, doc 02 §
  // The frame, interactively): one bump per fresh-key miss on an operator tile
  // whose recorded unmet inputs are still pending, at either gate site (the
  // driver's tile loop, `PullService::pull`'s per-covering-tile loop). The frame
  // composites the resident TRANSIENT tile instead and drives nothing.
  //
  // It is DISJOINT from `requests_suppressed`, and the two are not merged even
  // though both count "a render we declined to drive", because the mechanisms are
  // different and conflating them destroys the assertion that made the previous
  // task's failure legible: `requests_suppressed == 0` on the operator benchmark
  // scenes is what proved the residual waste was the wave and not duplicate
  // dispatch. A suppressed render is a DUPLICATE of one in flight; a coalesced one
  // is a SECOND render of a tile whose inputs have not all landed yet. A coalesced
  // render bumps neither `requests_issued` nor `operator_renders` -- that is the
  // entire observable point -- so this counter is the POSITIVE witness that the
  // gate is on the live path. Its absence is not one: "`requests_issued` did not
  // grow" is equally what you observe when the gate never fires at all.
  std::uint64_t renders_coalesced() const noexcept { return d_renders_coalesced; }

  void note_request_issued() noexcept { ++d_requests_issued; }
  void note_composite() noexcept { ++d_composites; }
  void note_follow_up_frame() noexcept { ++d_follow_up_frames; }
  void note_operator_render() noexcept { ++d_operator_renders; }
  void note_degraded_composite() noexcept { ++d_degraded_composites; }
  void note_audio_dispatch() noexcept { ++d_audio_dispatches; }
  void note_request_suppressed() noexcept { ++d_requests_suppressed; }
  void note_render_coalesced() noexcept { ++d_renders_coalesced; }

private:
  std::uint64_t d_requests_issued{0};
  std::uint64_t d_composites{0};
  std::uint64_t d_follow_up_frames{0};
  std::uint64_t d_operator_renders{0};
  std::uint64_t d_degraded_composites{0};
  std::uint64_t d_audio_dispatches{0};
  std::uint64_t d_requests_suppressed{0};
  std::uint64_t d_renders_coalesced{0};
};

// The composed observability record a host debug panel / downstream
// `pull_service` reads: the compositor's own counts beside the cache's live
// hit/miss/eviction numbers. Mirrors `HousekeepingStats`
// (`runtime/housekeeping.hpp:50-62`) -- "no new counting mechanism, just a
// composed view of numbers the mechanisms publish."
struct CompositorStats {
  std::uint64_t requests_issued{0};
  std::uint64_t composites{0};
  std::uint64_t follow_up_frames{0};
  std::uint64_t operator_renders{0};
  std::uint64_t cache_hits{0};
  std::uint64_t cache_misses{0};
  std::uint64_t cache_evictions{0};
  // Appended last, after the cache block, so every existing positional aggregate
  // init of this struct keeps its meaning. Same rule for every later addition:
  // append, never insert.
  std::uint64_t requests_suppressed{0};
  std::uint64_t renders_coalesced{0};
};

// Compose `counters`' own counts with `cache`'s published hit/miss/eviction
// counts into a single snapshot. Reads only -- no counting mechanism of its own.
inline CompositorStats counters_snapshot(const CompositorCounters& counters,
                                         const TileCache& cache) {
  return CompositorStats{counters.requests_issued(),
                         counters.composites(),
                         counters.follow_up_frames(),
                         counters.operator_renders(),
                         cache.hits(),
                         cache.misses(),
                         cache.evictions(),
                         counters.requests_suppressed(),
                         counters.renders_coalesced()};
}

} // namespace arbc
