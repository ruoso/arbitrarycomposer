#pragma once

#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/cache/key_shapes.hpp>
#include <arbc/cache/keyed_store.hpp>
#include <arbc/compositor/compositor.hpp>
#include <arbc/compositor/counters.hpp>
#include <arbc/compositor/scale_ladder.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/model/model.hpp>
#include <arbc/model/records.hpp>
#include <arbc/surface/backend.hpp>
#include <arbc/surface/surface.hpp>
#include <arbc/surface/surface_pool.hpp>

#include <cstdint>
#include <optional>
#include <vector>

// The interactive tile-based request planner for `arbc::compositor` (L4, doc
// 17:56). It concretizes doc 02's interactive frame steps 3-4 (02:57-65): map a
// visible layer's local region into a local-space-aligned tile grid per scale
// rung, look each tile up in the `TileCache`, classify the disposition
// (fresh / stale / coarser-rescaled / placeholder), and turn misses into
// deadline-carrying render descriptors. The plan is a **pure value** (no target
// surfaces, no pool, no backend) so planning never allocates and never takes a
// lock (doc 02:123-125); filling the cache happens only in the synchronous
// driver `render_frame_interactive`.
//
// This is the first production caller of the scale ladder (`select_rung`,
// `rung_scale`, `reduce_rung`) and the first real consumer of `TileCache`. It
// reaches the composite/downsample kernels only through the abstract L2
// `surface::Backend` seam -- no direct `backend-cpu` edge (doc 17:56).
//
// The offline `render_frame` stays exact-scale with no quantization by design
// (doc 02:74-85); `render_frame_interactive` is a new function beside it.

namespace arbc {

// The caller-owned pending-completion registry an interactive frame records
// deferred (async) tile renders into, drained by `poll_refinements`. Defined in
// `arbc/compositor/refinement.hpp`; forward-declared here so
// `render_frame_interactive` can take an optional pointer to it without pulling
// the refinement header into this one (doc 02:69-71 step 6, the progressive
// refinement loop).
struct RefinementQueue;

// A fixed device-pixel tile edge (doc 02:59's "e.g. 256^2 device pixels").
// Even at every rung, so tiles satisfy `reduce_rung`'s even-source-dims
// precondition (the power-of-two tile geometry `scale_ladder.md:251-253`
// reserved for this task). A named constant a later task can tune.
inline constexpr int k_tile_size = 256;

// How many octaves of coarser rungs the degradation fallback probes before it
// gives up and shows a placeholder (doc 02:64 "coarser-scale tiles rescaled"
// names no depth, so this is a tunable cutoff, not designed behavior). A cheap
// bound so a cold cache does not scan every rung to the coarsest.
inline constexpr int k_max_fallback_octaves = 4;

// Every local-space-aligned grid cell (at `rung`'s grid) overlapping
// `local_region`, half-open so a region exactly on a cell boundary is not
// double-covered (doc 02:57-60, 04:103-106). The grid for `rung` has cell edge
// `k_tile_size / rung_scale(rung)` local units aligned to the local origin, so
// a cell is exactly `k_tile_size^2` device pixels at that rung. Empty region ->
// no cells.
std::vector<TileCoord> tiles_covering(ScaleRung rung, const Rect& local_region);

// The inverse of `tiles_covering`: the layer-local rectangle a single grid cell
// covers at `rung` (`tiles_covering(rung, tile_local_rect(rung, c))` contains
// `c`). Axis-aligned in local space; the composed affine (with the <=1-octave
// remainder, rotation, shear) is applied at composite time, not baked here.
Rect tile_local_rect(ScaleRung rung, TileCoord coord);

// The source chosen for a tile after the cache lookup and the doc 02:63-65
// degradation order (fresh -> stale-revision -> coarser-rung-rescaled ->
// checkerboard/transparent).
enum class TileSource { Fresh, Stale, Coarser, Placeholder };

// A single planned tile (a pure, target-free value). `key` is the tile's
// *fresh* cache key `(content, current revision, rung, coord[, achieved_time])`.
// `is_miss` is set iff the fresh exact source is absent (a render is owed),
// *independently* of which fallback `display_source` is shown meanwhile (doc
// 02:61-65). `hold` pins the chosen display source in the cache and is invalid
// iff `display_source == Placeholder`; `source_rung` is that source's rung
// (equal to `key.rung` except for `Coarser`). A `RenderRequest` is materialized
// from these facts by the driver when it acquires the tile target -- the plan
// itself holds no live `Surface&` (Decision 2).
struct PlannedTile {
  TileCoord coord;
  Rect local_rect;
  TileKey key;
  bool is_miss{false};
  TileSource display_source{TileSource::Placeholder};
  ScaleRung source_rung;
  CacheHold<TileValue> hold;
  PriorityClass klass{PriorityClass::Visible};
};

// A visible layer's tile plan (a pure value; move-only through `tiles`'
// `CacheHold`s). Carries everything the driver needs to composite and to
// materialize miss requests, and nothing that would force an allocation or a
// lock at plan time (doc 02:123-125).
struct LayerTilePlan {
  ObjectId content;
  ScaleRung rung;
  double remainder{1.0};
  Affine local_to_device;
  Time time;
  StateHandle snapshot;
  Deadline deadline;
  std::vector<PlannedTile> tiles;
};

// Plan one visible layer's tiles (doc 02:57-65). For each covering tile it
// probes, in the degradation order: the **fresh** key (current `revision`,
// current rung) -> `Fresh` iff a qualifying hit (`meta.exact` and
// `meta.achieved_scale == rung_scale(rung)`, the consumer's qualification per
// the keyed_store contract); else the **stale** key (`prior_revision`, same
// rung/coord) -> `Stale`; else successively **coarser** rungs at the current
// revision (down to `k_max_fallback_octaves`) -> `Coarser`; else
// **`Placeholder`**. A tile whose fresh key missed sets `is_miss` regardless of
// the fallback shown. Reads and pins (`lookup`) only -- never inserts,
// allocates, or renders (Decision 1); takes `TileCache&` because `lookup`
// mutates LRU/counters, but performs no fill.
LayerTilePlan plan_layer(TileCache& cache, ObjectId content, std::uint64_t revision,
                         std::optional<std::uint64_t> prior_revision,
                         const RungSelection& selection, const Rect& local_region,
                         const Affine& local_to_device, Stability stability, Time time,
                         StateHandle snapshot, Deadline deadline);

// The synchronous tiled resolve+composite driver: the interactive analog of
// `render_frame` (doc 02:49-71). It reuses `render_frame`'s per-layer
// cull/compose/region walk, calls `select_rung`, `plan_layer`, then for each
// `PlannedTile` fills a miss inline (materializing a `BestEffort`
// `RenderRequest` targeting the tile footprint and driving it through a
// `RenderCompletion`, the exact settle path `render_frame` uses) and composites
// the display source onto `target` with the per-tile affine, folding the
// <=1-octave remainder / coarser-rung upscale into `Backend::composite`'s tap.
// `deadline` is stamped onto each miss request as a value (never recomputed; no
// wall-clock read -- enforcement is runtime policy, doc 17:60).
// `prior_revision` (the caller's last-completed revision) enables the stale
// probe; `std::nullopt` disables it. Single-threaded (doc 02:135-137); the
// async worker pool and real deadline clock are `compositor.pull_service`.
//
// When `pending` is non-null, a miss whose inline `render` answers
// asynchronously (returns `nullopt`, leaving a still-live `RenderCompletion`) is
// **recorded** into `*pending` instead of dropped -- the tile still composites
// its best available fallback this frame (the "coarse-then-refine" display), and
// `poll_refinements` later turns its arrival into a cache insert plus damage
// (doc 02:69-71 step 6). When `pending` is null the async miss is dropped
// exactly as before -- the byte-for-byte behavior `compositor.tile_planning`'s
// tests and goldens assert.
//
// When `counters` is non-null the driver bumps the behavioral counts at their
// seams: `requests_issued` once per inline miss render driven, `composites`
// once per `Backend::composite` call (coarser upscales included). Null (the
// default) is byte-identical -- the counter path only reads and writes the
// caller-owned struct, never a surface (doc 16:54-62, `counters.hpp`).
void render_frame_interactive(const DocRoot& state, const ContentResolver& resolve,
                              const Viewport& viewport, TileCache& cache, Backend& backend,
                              SurfacePool& pool, Surface& target, Deadline deadline,
                              std::optional<std::uint64_t> prior_revision,
                              RefinementQueue* pending = nullptr,
                              CompositorCounters* counters = nullptr);

} // namespace arbc
