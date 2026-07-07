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

  void note_request_issued() noexcept { ++d_requests_issued; }
  void note_composite() noexcept { ++d_composites; }
  void note_follow_up_frame() noexcept { ++d_follow_up_frames; }
  void note_operator_render() noexcept { ++d_operator_renders; }
  void note_degraded_composite() noexcept { ++d_degraded_composites; }

private:
  std::uint64_t d_requests_issued{0};
  std::uint64_t d_composites{0};
  std::uint64_t d_follow_up_frames{0};
  std::uint64_t d_operator_renders{0};
  std::uint64_t d_degraded_composites{0};
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
                         cache.evictions()};
}

} // namespace arbc
