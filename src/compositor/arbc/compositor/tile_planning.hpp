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

// The caller-owned device-space dirty region a gated interactive frame plans
// against. Defined in `arbc/compositor/damage_planning.hpp`; forward-declared
// here so `render_frame_interactive` can take an optional pointer to it without
// pulling the damage-planning header into this one, exactly as `RefinementQueue`
// is forward-declared above (doc 02:51's "no damage -> no work").
struct DirtyRegion;

// The caller-owned cycle/too-deep diagnostic sink an operator-aware frame reports
// budget-exceeded planning descents into. Defined in
// `arbc/compositor/operator_graph.hpp`; forward-declared here so
// `render_frame_interactive` can take an optional pointer to it without pulling
// the operator-graph header into this one, exactly as `DirtyRegion` is
// (doc 05:66-70, `compositor.operator_graph` Decision 6).
struct GraphDiagnostics;

// The concrete pull engine a frame may delegate its miss-fill's render dispatch
// to (`compositor.pull_service`, doc 13:69-89). Defined in
// `arbc/compositor/pull_service.hpp`; forward-declared here so
// `render_frame_interactive` can take an optional pointer to it without pulling
// that header (which includes this one) into this one -- the same forward-declare
// discipline `RefinementQueue` / `DirtyRegion` / `GraphDiagnostics` follow above.
class PullServiceImpl;

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
//
// `Timed`/`Live` tiles are keyed by the requested `time` snapped to the
// content's native grid via `content_ptr->quantize_time(time)` (achieved-time
// coalescing, doc 11:115-129): every requested instant in one native frame
// period collapses to a single key, so a sub-frame clock advance re-plans to
// all-fresh hits and issues zero renders. `Static` tiles omit `achieved_time`
// regardless (clock-invariant, doc 11:139-140). A null `content_ptr` (the
// default) or a `nullopt` from `quantize_time` keeps the raw requested time --
// the pre-coalescing, byte-identical behaviour; the snap is a single query
// evaluated once per layer.
LayerTilePlan plan_layer(TileCache& cache, ObjectId content, std::uint64_t revision,
                         std::optional<std::uint64_t> prior_revision,
                         const RungSelection& selection, const Rect& local_region,
                         const Affine& local_to_device, Stability stability, Time time,
                         StateHandle snapshot, Deadline deadline,
                         const Content* content_ptr = nullptr);

// The insert-site temporal-linkage predicate (doc 11:134-137). At a cache miss
// the compositor keys a `Timed` tile at `quantize_time(time)` *before* it renders
// (`plan_layer`), then on the render's arrival inserts the surface under that same
// pre-quantized `TileKey`, dropping `RenderResult::achieved_time` (`TileMeta` has
// no time field). Doc 11's **MUST** is that a `Timed` content's `render(time = t)`
// lands on the instant `quantize_time(t)` names -- so the `achieved_time` the
// render reports equals the instant the tile was keyed under, the coalesced tile
// is exactly the frame the transport asked for, and reuse is sound under seek.
// This pure predicate pins that linkage at the insert site: for `Timed` content
// that reports a concrete `achieved_time` it holds iff that instant equals
// `key.achieved_time`. It is deliberately additive and, for conformant content, a
// no-op: `Static`/`Live` content is exempt (they own no achieved-time grid, doc
// 11:139-143), and a `Timed` render that reports `nullopt` `achieved_time` is
// exempt too (the content honored the requested time exactly, or its
// `quantize_time` defaulted to `nullopt` and the key carries the raw requested
// time -- today's identity behaviour, which coalesces nothing yet is still sound,
// content.hpp:227-234). It is called under `assert` at the three insert sites
// (`tile_planning.cpp`, `pull_service.cpp`, `refinement.cpp`) -- catching a
// content that violates the doc-11 MUST as a fail-fast tripwire rather than a
// wrong-frame-under-seek bug -- and *directly* from its enforcing test, so the
// linkage claim holds regardless of `NDEBUG`.
bool timed_insert_key_consistent(const TileKey& key, const RenderResult& result,
                                 Stability stability);

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
//
// When `dirty` is non-null the frame is damage-gated (doc 02:51 "no damage ->
// no work") and repaints exactly one device region: `repaint_region(*dirty,
// viewport)` -- the bounding box of the dirty rects, rounded out (doc 02 § The
// frame, interactively). That single rect is used three times, which is what
// makes the frame idempotent: each visible layer's planned `local_region` is
// intersected with it (mapped into that layer's local space), the driver
// `clear_rect`s it on the caller-persisted `target` before compositing, and every
// composite onto `target` is `composite_clipped` to it -- so the planned set, the
// cleared set and the painted set are the same set, and a tile straddling the
// region's edge cannot spill its overhang source-over onto un-cleared pixels.
//
// The invariant this buys: **a gated frame's repaint region is byte-identical to
// what a single full pass would have put there, and the rest of the target is
// untouched** -- so compositing the same gated frame twice is a no-op, and a
// scene refined over N follow-up frames lands on exactly the pixels one un-gated
// pass would have produced. Without the clear the frame re-composites source-over
// onto pixels a previous frame already painted, which for translucent content
// lands the contribution twice and converges silently on wrong pixels.
//
// A non-null but *empty* `DirtyRegion` (an empty repaint region) clears nothing,
// plans nothing, composites nothing, and leaves `target` byte-identical: zero
// renders, zero composites. When `dirty` is null (the default) the driver clears
// the WHOLE `target` and plans the whole viewport with unclipped composites,
// byte-identical to the pre-damage behavior the `tile_planning` / `refinement` /
// `anchored_viewports` goldens guard.
//
// `composition_time` is the caller-supplied composition-time instant the frame
// plans at (Decision 4): a `Time` value, not a clock -- the compositor stays
// stateless and the transport that samples the clock is `runtime.interactive`'s
// (doc 11:144-149, doc 17:60). Per-layer it drives temporal placement (doc
// 11:60-73, 185-191): a layer whose half-open `span` does not contain
// `composition_time` is culled before content resolve, and a present layer's
// `time_map` is evaluated at `composition_time` to the content-local time that
// threads into `plan_layer` (which snaps each `Timed` layer to its content grid
// via `quantize_time`, so a sub-frame advance keys the same native frame and
// hits the cache) and onto each miss `RenderRequest`. A time_map that overflows
// culls the layer (Decision D3). The still-layer default -- `span ==
// TimeRange::all()` and identity `time_map` -- is present everywhere and maps
// `composition_time` unchanged, so every landed caller and golden is
// byte-unchanged. Defaulted `Time::zero()`.
//
// When `visible_plans` is non-null the driver surfaces the per-visible-layer
// `LayerTilePlan`s it composited from: it `assign`s the sink empty at entry, and
// after each visible layer is composited **moves** that layer's plan into
// `*visible_plans` (in composite / bottom-to-top order) instead of letting it die
// at layer-scope exit. Each entry is the exact plan the frame composited -- same
// `content`, `rung`, and `tiles` (each `PlannedTile`'s post-composite `TileKey`
// and `display_source`) -- so the interactive loop can drive `prime_prefetch`
// (doc 02:92-93 speculation, step 7) render-free from what the frame already
// planned, with no re-plan and no extra cache probe (claim
// `02-architecture#speculation-drives-from-exposed-plan`). When null (the
// default) each plan is dropped exactly as today -- byte-for-byte the current
// behavior the landed goldens guard. `LayerTilePlan` is move-only (its
// `PlannedTile`s pin cache holds), so the surfaced plans retain their pins until
// the caller drops the sink; on this opt-in *active* path an earlier layer's
// tiles therefore stay pinned while later layers' misses insert, which may change
// eviction victim selection/timing versus the null default (intended: the visible
// set stays resident for the prime pass). The null default is untouched; the
// invariant is render/composite neutrality, not `evictions()` equality on the
// active path.
//
// Operator-graph awareness (`compositor.operator_graph`, doc 13:124-128,
// 05:66-70): a visible layer whose content is an operator (`inputs()` non-empty)
// keys its tiles by the **aggregate** revision folded over the reachable
// `inputs()` DAG (so the operator's key changes on any reachable input change),
// resolves its `identity()` chain before issuing a render, and bumps
// `counters->operator_renders` once per operator render driven. An `identity()`
// short-circuit issues no operator render and creates no operator-output cache
// entry (serving input N's tiles is `pull_service`'s); a descent that exceeds the
// recursion budget renders the placeholder and reports one diagnostic through
// `diagnostics` (non-null). A **leaf** layer (empty `inputs()`) takes none of
// these branches: the fold degenerates to the flat revision, no identity check
// fires, no diagnostic is reported -- byte-for-byte the pre-task leaf path the
// landed goldens guard. `diagnostics` defaults null (byte-neutral), the trailing
// caller-owned optional pointer per the established seam-growth discipline.
//
// When `pulls` is non-null the driver delegates each miss's render *dispatch* to
// it (`compositor.pull_service`, doc 13:69-89): instead of calling
// `content->render` inline, it hands the render to `pulls->dispatch`, which
// runtime binds to the worker pool. The driver keeps its own plan-time cache
// probe, tile key, tile-surface ownership, insert, and async recording -- only
// the render call is delegated -- so cache operations are unchanged. When `pulls`
// is null (the default) the driver builds an internal `PullServiceImpl` with a
// `DirectDispatch` (call `content->render` inline), so the null path is
// byte-for-byte the pre-task inline fill the landed goldens guard. Every
// dispatched render carries the layer plan's `snapshot` pin (closing the inert
// `StateHandle{}` gap, doc 02:124, `02-architecture#miss-becomes-deadline-request`)
// and the frame `deadline` verbatim.
//
// `exactness` is the request discipline stamped onto every miss `RenderRequest`
// (doc 03:12-13,124-127). The default `BestEffort` is the interactive discipline
// (a render may answer async, degrade, and observe the deadline) -- byte-identical
// to every existing caller. The offline sequence driver (`runtime.offline_sequences`)
// passes `Exact` with `deadline == Deadline::none()`: an exact render must be
// faithful, may take unbounded time, and ignores the deadline (doc 02:73-85,
// `02-architecture#offline-frame-renders-exactly-no-degrade`). It changes only the
// value stamped on the request; the compositor's inline fill renders every miss to
// completion on both disciplines, so with no deadline pressure an offline frame
// composites only fresh, exact-scale tiles (the `degraded_composites` counter,
// bumped below for any stale/coarser/placeholder display, then reads zero).
void render_frame_interactive(
    const DocRoot& state, const ContentResolver& resolve, const Viewport& viewport,
    TileCache& cache, Backend& backend, SurfacePool& pool, Surface& target, Deadline deadline,
    std::optional<std::uint64_t> prior_revision, RefinementQueue* pending = nullptr,
    CompositorCounters* counters = nullptr, const DirtyRegion* dirty = nullptr,
    Time composition_time = Time::zero(), std::vector<LayerTilePlan>* visible_plans = nullptr,
    GraphDiagnostics* diagnostics = nullptr, PullServiceImpl* pulls = nullptr,
    Exactness exactness = Exactness::BestEffort);

} // namespace arbc
