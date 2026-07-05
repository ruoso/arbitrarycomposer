# compositor.tile_planning — Tile-based request planning

## TaskJuggler entry

`tasks/35-compositor.tji:15-20` → `compositor.tile_planning` ("Tile-based
request planning"), the second leaf under `task compositor`. It carries its own
`depends !scale_ladder, contract.snapshot_pins` (`35-compositor.tji:18`) and, through
the parent `task compositor`, inherits `depends contract.async_render,
cache.key_shapes, color.resampling` (`35-compositor.tji:7`). Five siblings chain
off it: `damage_planning`, `pull_service`, `anchored_viewports`, `refinement`,
`counters` (all `depends !tile_planning`, `35-compositor.tji:21-55`). Note line:

> "Visible-region planning into local-space-aligned tiles per rung, cache
> lookups, misses become deadline-carrying requests; stale/coarser/placeholder
> preference order. Doc 02."

## Effort estimate

**4d.** Twice the scale-ladder leaf, and rightly: this task turns doc 02's
interactive request-plan (step 3, `02:57-60`) and miss-degradation (step 4,
`02:61-65`) into concrete code. The deliverable is (1) pure tile-grid geometry
per rung, (2) a **pure planner** that maps a layer's visible local region to a
set of tiles, does the cache lookup for each, classifies the disposition
(fresh / stale / coarser-rescaled / placeholder), and turns misses into
deadline-carrying render descriptors, and (3) a **synchronous tiled
resolve+composite driver** that fills misses inline (mirroring the offline
`render_frame` drive) and composites tiles with the ≤1-octave remainder — the
end-to-end path that lets the plan be golden-tested. Plus exhaustive unit tests
on the planner with a hand-populated `TileCache`, one byte-exact tiling golden,
three claims-register entries, and the CMake wiring. It reuses everything the
predecessors landed — `select_rung`/`reduce_rung` (the ladder), `TileKey`/
`TileValue`/`TileCache`/`PriorityClass` (the cache), `RenderRequest`/`Deadline`/
`RenderCompletion` (the contract), `Backend::composite`/`downsample` (resampling)
— so no new capability is built, only their composition into a planner.

## Inherited dependencies

**Settled:**

- `compositor.scale_ladder` (commit `2b74aa6`, DONE 2026-07-05) — the direct
  predecessor (`depends !scale_ladder`). It delivered the rung algebra this
  planner is the first production caller of, in
  `src/compositor/arbc/compositor/scale_ladder.hpp`:
  - **`RungSelection { ScaleRung rung; double remainder; }`** (`:39-42`) and
    **`RungSelection select_rung(double scale)`** (`:63`) — the quantization
    that is doc 02's plan step 3 "quantize scale to the ladder" (`02:57-60`).
    `scale` is `Affine::max_scale()` (`src/base/arbc/base/transform.hpp:31-34`),
    the larger singular value of the composed mapping (doc 04:103-106). The
    planner calls this once per visible layer; `remainder ∈ (0.5, 1.0]` is what
    it hands the composite pass.
  - **`double rung_scale(ScaleRung)`** (`:84`) = `2^index` — device pixels per
    local unit at a rung; the planner uses it to size the local-space tile grid
    and to scale each tile's render request.
  - **`ScaleRung reduce_rung(Backend&, Surface& dst, const Surface& src,
    ScaleRung)`** (`:100-108`) — exact 2:1 box reduction to the next-coarser
    rung. The ladder's Status explicitly reserved this reducer's **production
    call site** (populating coarser cache rungs on a miss / for the degradation
    fallback) for tile_planning; this task is that call site.
  - The ladder's Status names, verbatim, the work handed here: the
    "visible-region → local-space-aligned **tile grid** split", "per-rung
    **tile-coordinate** enumeration", "**cache lookup/fill** of rung tiles", and
    the "**coarser-tile fallback rescale** in the deadline-degradation order"
    (`scale_ladder.md:102-112`). The even-dims guarantee on `reduce_rung` is
    "a property of power-of-two tile geometry, **owned by tile_planning**"
    (`scale_ladder.md:251-253`) — this task establishes it.
- `contract.snapshot_pins` (DONE 2026-07-05) — the other direct predecessor
  (`depends contract.snapshot_pins`). It added
  **`StateHandle snapshot{}`** to `RenderRequest`
  (`src/contract/arbc/contract/content.hpp:71`), a trivially-copyable resolved
  handle defaulting to `k_state_none`. Its refinement pins the load-bearing
  fact for this task: "`cache.*` builds tile keys that include the pinned
  revision; the handle is what guarantees the key uniquely identifies the
  pixels" (`snapshot_pins.md`). The planner projects the pin's revision into
  `TileKey.revision` and copies the pin's `StateHandle` onto every miss request,
  so a tile's cached pixels are honestly keyed to the state that produced them
  (doc 03 `#render-pure-over-pinned-state`, `registry.tsv:50`). Resolving
  `content_state(id)` from the pinned `DocState` onto the request stays deferred
  to the runtime binding path (`snapshot_pins.md`); the walking-skeleton planner
  passes the snapshot handle it is given (default `StateHandle{}` until runtime
  wires it), exactly as `render_frame` does (`compositor.cpp:66-71`).
- `cache.key_shapes` (commit `a198981`, DONE 2026-07-05) — the type predecessor
  (inherited via the parent). `src/cache/arbc/cache/key_shapes.hpp`:
  - **`struct TileCoord { std::int32_t col, row; }`** (`:48-53`) — the integer
    grid coordinate, "meaningful only relative to a `ScaleRung`"; this task's
    grid enumeration produces it.
  - **`struct TileKey { ObjectId content; std::uint64_t revision; ScaleRung rung;
    TileCoord coord; std::optional<Time> achieved_time; }`** (`:64-76`) — the
    lookup key the planner assembles. `achieved_time` is **`nullopt` for
    `Static` content**, present for `Timed` — the property that makes a still
    scene's tile keys clock-invariant.
  - **`struct TileMeta { double achieved_scale; bool exact; }`** (`:99-102`,
    mirrors `RenderResult`) and **`struct TileValue { std::unique_ptr<Surface>
    surface; TileMeta meta; }`** (`:110-113`) — the value the planner reads to
    qualify a hit and fills on a miss.
  - **`std::size_t tile_byte_cost(const Surface&)`** (`:120-124`) — the byte cost
    passed to `insert`.
  - **`using TileCache = KeyedStore<TileKey, TileValue>;`** (`:129`) — the store
    the planner queries. `key_shapes.md` explicitly deferred **achieved-time
    coalescing/quantization** and the **tile grid geometry** to the compositor;
    the geometry lands here (achieved-time bucketing routes to `damage_planning`
    — see Decisions). `TileCache` "has no production caller yet — tile_planning
    is its first real consumer."
  - `KeyedStore` API (`src/cache/arbc/cache/keyed_store.hpp`):
    **`std::optional<hold_type> lookup(const Key&)`** (`:162`, LRU-refreshes on
    hit, bumps `hits()`/`misses()`), **`hold_type insert(Key, Value, bytes,
    PriorityClass)`** (`:156`), **`reclassify`** (`:166`), and the move-only RAII
    residency pin **`CacheHold<Value>`** (`:71-119`). "Whether a hit *qualifies*
    — exact scale, current revision — is the consumer's read of the value's
    metadata, not the store's": this task owns that qualification. **`enum class
    PriorityClass { Speculative, Recent, Adjacent, Visible }`** (`:21-26`),
    declaration order = eviction order = doc 02:92-93's "visible > adjacent >
    recently visible > speculative".
- `contract.async_render` (DONE) — the request/completion contract. `content.hpp`:
  **`RenderRequest { Rect region; double scale; Time time; StateHandle snapshot;
  Surface& target; Exactness exactness; Deadline deadline; }`** (`:67-75`) — the
  descriptor a miss materializes into; **`enum class Exactness { BestEffort,
  Exact }`** (`:26`) — the planner sets `BestEffort` (interactive); **`struct
  Deadline`** (`:47-54`) — a **value-only** monotonic instant, "carries value
  only — no `now()`/`expired()`; reading/enforcing the clock is runtime policy";
  **`RenderResult { double achieved_scale; bool exact; }`** (`:77-80`) — copied
  into `TileMeta` on fill; **`RenderCompletion`** (`:100-132`) and
  `Content::render(request, done)` (`:167`) — the one settle path the synchronous
  driver reuses (doc 03 `#render-inline-or-async`, `registry.tsv:51`).
- `color.resampling` (commit `fa895d5`, DONE 2026-07-05) — the kernels behind the
  composite/downsample, on the abstract L2 `surface::Backend` seam
  (`src/surface/arbc/surface/backend.hpp`): **bilinear composite tap folded into
  `composite(dst, src, src_to_dst, opacity)`** (`:39-40`, absorbs the ≤1-octave
  remainder, collapses byte-exact to nearest at integer alignment) and **box
  2:1 `downsample(dst, src)`** (`:42-48`, what `reduce_rung` wraps). Claim
  `07-…#resampling-in-linear-premultiplied-space` (`registry.tsv:31`) stays owned
  by color; this task depends on it, does not re-register it.

**Pending:** none — every predecessor is landed.

## What this task is

Deliver the **interactive tile-based request planner** for `arbc::compositor`
(L4): the machinery that takes a visible layer, splits its visible local region
into local-space-aligned tiles at the layer's scale rung, looks each tile up in
the `TileCache`, and turns the result into a plan — cache hits served directly,
misses turned into deadline-carrying render requests, and the degraded-fallback
preference order applied when a fresh tile is absent. In a new header/impl
`src/compositor/arbc/compositor/tile_planning.hpp` (+ `tile_planning.cpp`):

1. **Tile-grid geometry (pure).** A fixed tile size `constexpr int k_tile_size =
   256` (device pixels; doc 02:59's "e.g. 256²"). `std::vector<TileCoord>
   tiles_covering(ScaleRung rung, const Rect& local_region)` enumerates the
   local-space-aligned grid cells overlapping a layer-local rectangle — the grid
   for rung `r` has cell edge `k_tile_size / rung_scale(r)` local units, aligned
   to the local origin, so a tile is exactly `k_tile_size²` device pixels *at
   that rung*. `Rect tile_local_rect(ScaleRung rung, TileCoord coord)` is the
   inverse. One grid per rung; tiles are **axis-aligned in local space**
   (doc 04:103-106), and the full affine (with the ≤1-octave remainder) is
   applied at composite time.
2. **The disposition classification.** `enum class TileSource { Fresh, Stale,
   Coarser, Placeholder };` — the source chosen for a tile after the cache
   lookup and the doc 02:63-65 degradation order.
3. **`struct PlannedTile`** — per tile: its `TileCoord`, its `local_rect`, its
   fresh `TileKey` (`(content, current_revision, rung, coord[, achieved_time])`),
   an `is_miss` flag (fresh exact source absent → a render is owed), the chosen
   `display_source` + the `CacheHold<TileValue>` pinning it (invalid iff
   `Placeholder`) + that source's rung, and its `PriorityClass` (`Visible` for
   the planned region).
4. **`struct LayerTilePlan`** — per visible layer: `content`, the `ScaleRung
   rung`, the `double remainder`, the `Affine local_to_device`, the frame `Time`,
   `StateHandle snapshot`, `Deadline deadline`, and the `std::vector<PlannedTile>
   tiles`. The plan is a **pure value** — no target surfaces, no pool, no
   backend — so planning never allocates and never takes a lock (doc 02:123-125).
5. **`LayerTilePlan plan_layer(const TileCache&, ObjectId content, std::uint64_t
   revision, std::optional<std::uint64_t> prior_revision, const RungSelection&,
   const Rect& local_region, const Affine& local_to_device, Stability, Time,
   StateHandle, Deadline)`** — the planner. For each covering tile it probes, in
   the doc 02:63-65 order: the **fresh** key (current revision, current rung) →
   `Fresh` if a qualifying hit (`meta.exact`, `achieved_scale == rung_scale`);
   else the **stale** key (`prior_revision`, same rung/coord) → `Stale`; else
   successively **coarser** rungs at the current revision (down to
   `k_max_fallback_octaves`) → `Coarser`; else **`Placeholder`**. A tile whose
   fresh key missed sets `is_miss` — the caller owes it a render — *independently*
   of which fallback it displays meanwhile (doc 02:61-65: issue the request, and
   "proceed with what it has" until it lands).
6. **The synchronous tiled driver.** `render_frame_interactive(const DocRoot&,
   const ContentResolver&, const Viewport&, TileCache&, Backend&, SurfacePool&,
   Surface& target, Deadline, std::optional<std::uint64_t> prior_revision)` — the
   interactive analog of `render_frame`. It reuses `render_frame`'s per-layer
   cull/compose/region walk (`compositor.cpp:16-46`), calls `select_rung`,
   `plan_layer`, then for each `PlannedTile`: on `is_miss`, acquire a tile-sized
   surface from `pool`, materialize a `BestEffort` `RenderRequest{tile_local_rect,
   rung_scale, time, snapshot, tile_surface, BestEffort, deadline}`, drive it
   **inline** through a `RenderCompletion` (the exact settle path at
   `compositor.cpp:77-91`), and on success `insert` `{surface, {achieved_scale,
   exact}}` into the cache under the tile's class; then composite the display
   source (fresh/stale/coarser-rescaled/placeholder) onto `target` via the
   per-tile affine (`tile_local_rect → local_to_device`, remainder folded into
   the composite tap exactly as `compositor.cpp:95-99` builds `temp_to_dst`),
   upscaling a `Coarser` source through the same affine. This is the end-to-end
   path the goldens exercise.

**Not this task:**

- **Async worker dispatch, deadline-clock enforcement, target-allocation on
  completion, servicing pending (`nullopt`) renders, re-compositing on arrival**
  → `compositor.pull_service` (`35-compositor.tji:32-37`) + `runtime.interactive`
  (doc 17:60). This task drives misses *synchronously* (inline `render`), stamps
  each request with the `Deadline` *value* it is handed, and never reads a
  wall-clock. `pull_service` replaces the inline drive with worker scheduling and
  the real deadline clock; the plan's miss descriptors are its input.
- **Damage-driven planning ("no damage, no work"), model-damage → dirty device
  regions, clock-advance as the temporal-damage axis, and `achieved_time`
  bucketing/coalescing** → `compositor.damage_planning`
  (`35-compositor.tji:21-25`) + `cache.invalidation`. This task keys `Timed`
  tiles by the *raw* requested time (identity bucket) and `Static` tiles with
  `achieved_time == nullopt`; the coalescing that maps requested time → a shared
  bucket is `damage_planning`'s (it owns the clock axis, docs 02/11).
- **Prefetch ring (`Adjacent`), speculative next-rung requests during zoom
  (`Speculative`), and async-arrival progressive refinement** →
  `compositor.refinement` (`35-compositor.tji:44-49`; doc 04:99-101). This task
  plans and classifies only the **visible** region (`PriorityClass::Visible`);
  it neither pans nor speculates.
- **`inputs()`, aggregate revisions, damage routing, cycle detection, and the
  `identity()` short-circuit in planning** → `compositor.operator_graph`
  (`35-compositor.tji:27-31`; docs 05/13). This task plans a flat layer's tiles;
  it does not walk an operator graph or short-circuit identity fades.
- **Anchor-walk / viewport-outward culling and the `2^16` re-anchor threshold**
  → `compositor.anchored_viewports` (`35-compositor.tji:38-43`). This task reuses
  `render_frame`'s existing bounds-intersect cull.
- **Exposing the debug counters (requests / cache hits / composites) through the
  `arbc::base` counter registry** → `compositor.counters`
  (`35-compositor.tji:50-55`). This task proves its behavioral claims by direct
  assertion on the returned `LayerTilePlan` (miss count, source classification),
  not through a counter surface; `counters` later wires the same behaviors to the
  exposed counters.
- Any change to the **offline `render_frame`**, which stays exact-scale by design
  (doc 02:74-85 — offline has "no quantization"). The interactive path is a new
  function beside it; the offline frame is untouched (same posture as
  `scale_ladder.md`).

## Why it needs to be done

Tile planning is where doc 02's interactive frame becomes real. Step 3
(`02:57-60`) — "map the visible region into layer-local space, quantize scale to
the ladder, split into **tiles** … and look each tile up in the cache" — is
exactly this task; `scale_ladder` delivered the quantize, and this task delivers
the split + lookup that consume it. Step 4 (`02:61-65`) — "Cache misses become
render requests with a deadline … the frame proceeds with what it has:
stale-revision tiles, coarser-scale tiles rescaled, or checkerboard/transparent,
in that preference order" — is the degradation this task encodes. Until it lands,
the `TileCache` shipped by `cache.key_shapes` has no production caller, the
`reduce_rung` reducer has no production call site, and the four siblings that
`depend !tile_planning` (`damage_planning`, `pull_service`, `anchored_viewports`,
`refinement`, `counters`) have no plan to hang damage, async dispatch, prefetch,
or counters on. This task is the structural spine of the interactive renderer:
the planner every later compositor task extends.

## Inputs / context

- `docs/design/02-architecture.md`:
  - `:49-71` — **The frame, interactively**. `:57-60` step 3 "Plan requests" (the
    governing sentence: map to local space, quantize, split into tiles "fixed
    local-space-aligned tile grid per scale rung, e.g. 256² device pixels", look
    up in cache). `:61-65` step 4 "Render misses within budget": "Cache misses
    become render requests with a deadline … the frame proceeds with what it has:
    stale-revision tiles, coarser-scale tiles rescaled, or checkerboard/
    transparent, in that preference order" — the exact fallback order. `:66-68`
    step 5 "Composite": "Tiles rendered at a ladder rung are resampled by the
    ≤1-octave remainder during this pass". `:69-71` step 6 "Refine": async
    arrivals produce damage scheduling a follow-up frame (this task's `is_miss`
    descriptors are what those arrivals fill; the scheduling is `refinement`).
  - `:74-85` — the offline discipline ("exact scale", "no quantization"): this
    task leaves `render_frame` untouched.
  - `:87-116` — **Tile cache**. `:89-91` key `(content id, revision, scale rung,
    tile coords)` and value `surface + {achieved_scale, exact}`. `:92-93`
    priority classes "visible > adjacent (pan prefetch ring) > recently visible >
    speculative" (this task assigns `Visible`; the rest are `refinement`).
    `:94-95` "Damage invalidates by `(content id, region)` across all rungs;
    revision bumps invalidate wholesale by making old keys unreachable" — the
    reason stale tiles are reached only by a *deliberate* prior-revision probe
    (see Decisions). `:100-116` the residency pin (a `lookup` yields a pinned
    hold the frame composites under, eviction skips pins) and the soft byte
    budget — the store machinery this task drives.
  - `:123-125` — "The **compositor** runs frame planning on the render thread. It
    reads the scene under a snapshot — concretely, a pinned document version — so
    planning never races edits and never takes a lock." The planner is a pure
    function of the snapshot; this pins the "no allocation, no lock in
    `plan_layer`" constraint.
- `docs/design/04-transforms-and-infinite-zoom.md`:
  - `:88-106` — **Scale ladders and tile geometry**. `:93-94` "Cache tiles are
    keyed by rung, so a smooth pinch-zoom reuses one rung's tiles across an
    octave". `:103-106` request scale = "the larger singular value of the
    composed mapping", "tiles are axis-aligned in *local* space, and the
    composite pass applies the full affine" — the tile-grid alignment rule.
  - `:99-101` — the speculative next-rung request "once the next rung is
    available": explicitly `compositor.refinement`, not this task.
- `docs/design/03-layer-plugin-interface.md` — the `RenderRequest`/`Deadline`
  sketch and "two request disciplines" (BestEffort interactive vs Exact offline):
  the planner emits `BestEffort` with the frame deadline. Claim
  `#render-pure-over-pinned-state` (`registry.tsv:50`) — the tile's pixels are a
  pure function of `(snapshot, region, scale, time)`, so the `TileKey` honestly
  identifies them.
- `docs/design/11-time-and-video.md` — `Static` content omits `achieved_time`
  so no still grows the cache and its tile keys are clock-invariant; the temporal
  damage axis (clock advance) and `achieved_time` bucketing are
  `compositor.damage_planning`. Claim `#tile-key-carries-time-and-revision`
  (`registry.tsv:56`, owned by `cache.key_shapes`) pins the key shape; this task
  adds the *behavioral* still-scene claim (below).
- `docs/design/05-recursive-composition.md` / `docs/design/13-effects-as-operators.md`
  — a synthetic child viewport "inherits the outer request's deadline" and
  budgets/deadlines "flow through pulls exactly as they flow through nesting":
  the deadline is a **propagated value**, never recomputed — reinforcing the
  single-frame-deadline decision. Operator-graph awareness itself is
  `compositor.operator_graph`.
- `docs/design/16-sdlc-and-quality.md`:
  - `:14-25` — the claims register: id is `<doc-file-stem>#<slug>`, enforced by a
    `// enforces: <claim-id>` test comment; CI fails on a registered claim with
    no live test.
  - `:46` — unit tests for core machinery ("… culling …"), Catch2, fast,
    exhaustive on edge cases: the planner's classification is a unit-test-tier
    item.
  - `:47-53` — byte-exact CPU goldens (deterministic fixed-FP kernels): the
    tiled-composite golden is byte-exact.
  - `:54-62` — behavioral-counter tests: "playback of a still scene issues zero
    visual renders … Most claims-register entries about efficiency land here."
    This task pins the still-scene claim as a **plan-level** behavioral assertion
    (zero miss descriptors); `counters` re-exposes it through the counter surface.
  - `:112-118` — ≥90% diff coverage on changed lines.
- `docs/design/17-internal-components.md`:
  - `:56` — `arbc::compositor` is **Level 4**, "transform resolution, culling,
    **request planning**, scale ladder, damage routing over inputs(), …",
    `Depends on: contract, cache (+ below)`. The `contract` and `cache` edges are
    directly authorised; `surface`/`model`/`pool` are the transitive closure
    (contract → model, surface; cache → surface). **No direct `backend-cpu`
    edge** — the composite/downsample kernels are reached only through the
    abstract `surface::Backend` vtable.
  - `:40-44` — "depend only on strictly lower levels; no same-level edges"; CI
    validates the CMake target graph and include graph against the table.
  - repo layout: public headers under `src/<component>/arbc/<component>/`, unit
    tests in `src/<component>/t/`, cross-component (golden) tests in top-level
    `tests/`.
- `src/compositor/compositor.cpp` — the offline `render_frame` this task's
  interactive driver mirrors: `:16-46` the per-layer cull/compose/region walk
  (`for_each_layer`, `compose(viewport.camera, layer.transform)`, `inverse()`
  degenerate cull, `inv->map_rect(device_rect)`, `bounds` intersect, empty cull,
  `max_scale()` positive-finite cull) — **reused verbatim** as the interactive
  walk's front half; `:47-64` the pool `acquire` + clear pattern (mirrored per
  tile); `:71` the `RenderRequest` construction; `:77-91` the inline
  `RenderCompletion` settle path (reused per miss); `:95-99` the `temp_to_dst`
  composite affine with the remainder (reused per tile).
- `src/compositor/arbc/compositor/compositor.hpp:16-24` — `struct Viewport {
  int width, height; Affine camera; }` and `using ContentResolver =
  std::function<Content*(ObjectId)>`, reused by the interactive entry.
- `src/compositor/arbc/compositor/scale_ladder.hpp:39-42,:63,:84,:100-108` —
  `RungSelection`, `select_rung`, `rung_scale`, `reduce_rung`.
- `src/cache/arbc/cache/key_shapes.hpp:48-53,:64-76,:99-113,:120-124,:129` —
  `TileCoord`, `TileKey`, `TileMeta`, `TileValue`, `tile_byte_cost`, `TileCache`.
- `src/cache/arbc/cache/keyed_store.hpp:21-26,:71-119,:156,:162,:166` —
  `PriorityClass`, `CacheHold`, `insert`, `lookup`, `reclassify`.
- `src/contract/arbc/contract/content.hpp:26,:47-54,:67-75,:77-80,:100-132,:167`
  — `Exactness`, `Deadline`, `RenderRequest`, `RenderResult`, `RenderCompletion`,
  `Content::render`.
- `src/surface/arbc/surface/backend.hpp:39-48` — `composite`, `downsample`.
- `src/base/arbc/base/transform.hpp:31-34` — `Affine::max_scale()`.
- `src/compositor/CMakeLists.txt:1-8` — `DEPENDS contract cache`, the
  `arbc_add_component` + `arbc_component_test` pattern; this task adds the header,
  impl, unit test, and (in `tests/CMakeLists.txt`) the golden. `surface`/`model`/
  `pool` stay transitive — **no new `DEPENDS` edge** (see Constraints).
- `tests/claims/registry.tsv` — 2-column TAB-separated `<claim-id>\t
  <description>`; this task appends three rows. No tile-planning claim exists yet.

## Constraints / requirements

- **Levelization (doc 17:40-44, :56).** The module lives in **L4
  `arbc::compositor`** and uses only `contract`, `cache`, and their transitive
  closure (`model`, `surface`, `pool`). It reaches the composite/downsample
  kernels **only through the abstract L2 `surface::Backend` vtable** — no direct
  `arbc::backend-cpu` edge, no same-level edge. The compositor's `DEPENDS contract
  cache` is **unchanged** (`render_frame` already pulls `SurfacePool`, `Backend`,
  `DocRoot` transitively through this set; this task adds no include outside it).
  The CI dependency check must stay green with no new listed edge.
- **Pure, allocation-free, lock-free planning (doc 02:123-125).** `plan_layer`
  and the geometry functions take the `TileCache` by `const&`... note: `lookup`
  mutates LRU/counters, so the planner takes `TileCache&` but performs **no
  insert and no allocation** — it reads and pins. It reads the snapshot's
  revision as a plain `std::uint64_t`; it never touches the model under a lock.
  Filling the cache (`insert`, surface acquisition, inline render) happens only
  in the **driver**, never in `plan_layer`.
- **Local-space-aligned grid, one per rung (doc 02:57-60, 04:103-106).** The tile
  grid for rung `r` is aligned to the layer-local origin with cell edge
  `k_tile_size / rung_scale(r)` local units; `tiles_covering` returns every cell
  overlapping the visible local region (half-open cell bounds, no double-cover of
  a boundary). Tiles are axis-aligned in local space; the composed affine (with
  rotation/shear) is applied at composite time, not baked into the grid.
- **Miss = deadline-carrying `BestEffort` request targeting the tile footprint
  (doc 02:61-65).** A tile whose fresh key misses is marked `is_miss`; the driver
  materializes a `RenderRequest` with `region = tile_local_rect(rung, coord)`,
  `scale = rung_scale(rung)`, `time`, `snapshot`, a pool-acquired tile target,
  `exactness = BestEffort`, and the frame `deadline` (copied, never recomputed).
  A cache hit issues **no request**. `Deadline` is stamped as a value; the driver
  reads no wall-clock (enforcement is runtime, doc 17:60).
- **Degradation preference order (doc 02:63-65).** When the fresh exact tile is
  absent, the display source is chosen in the order **Fresh → Stale-revision →
  Coarser-rung-rescaled → Placeholder**, first available wins. Stale is reached
  by a deliberate lookup at the caller-supplied `prior_revision` (revision bumps
  make old keys "unreachable" only via the *normal* current-revision query,
  doc 02:94 — a targeted probe still reaches a not-yet-evicted prior tile; if
  `cache.invalidation` later drops it, the probe misses and falls to Coarser,
  gracefully). Coarser probes successively coarser rungs at the current revision
  down to `k_max_fallback_octaves`; the source is upscaled through the composite
  affine. Placeholder is checkerboard/transparent and holds no cache pin.
- **Hit qualification is the consumer's (keyed_store contract).** A `lookup` hit
  is `Fresh` only if `meta.exact && meta.achieved_scale == rung_scale(rung)`; a
  best-effort or wrong-scale entry at the fresh key is treated as not-fresh
  (falls through the order). The store returns any key match; this task reads
  `TileValue.meta` to qualify.
- **`Static` tiles omit `achieved_time`; `Timed` tiles carry the raw time
  (doc 11, `registry.tsv:56`).** The `TileKey.achieved_time` is `nullopt` for
  `Stability::Static` and the requested `Time` for `Stability::Timed`/`Live`, so
  a still (all-Static) scene's keys are clock-invariant and a clock-only advance
  re-plans to all fresh hits. Bucketing/coalescing of `Timed` keys is deferred to
  `damage_planning`.
- **Remainder resampled at composite time, in the backend (doc 02:66-68, 07).**
  The ≤1-octave `remainder` and any coarser-rung upscale are applied by
  `Backend::composite`'s bilinear tap; the compositor never touches pixels
  (doc 02:36-39). At a power-of-two scale (`remainder == 1.0`, integer
  alignment) the tap collapses to nearest byte-exactly — the property the tiling
  golden asserts. Coarser-rung reduction (for filling coarser cache rungs) uses
  `reduce_rung` (box mean in linear premultiplied space).
- **Tile-boundary seamlessness.** Abutting tiles composited onto the target must
  produce no seam or double-blend: tiles tile the local region with half-open
  coverage, and at a power-of-two scale their device footprints fall on integer
  pixel boundaries, so the union is byte-identical to a single-surface render of
  the region (the golden). Non-axis-aligned / non-power-of-two placement is
  exercised behaviorally, not byte-exactly (justified: resampling at a rotated
  seam is not bit-reproducible against a whole-region reference).
- **Even-dimensioned tiles (doc from `scale_ladder.md:251-253`).** `k_tile_size =
  256` is even at every rung, satisfying `reduce_rung`'s even-source-dims
  precondition — this task establishes the power-of-two tile geometry the ladder
  reserved.
- **Single-threaded synchronous driver; no concurrency surface.** The v1 driver
  renders misses inline (doc 02:135-137 "v1 may degenerate to everything on one
  thread"); `RenderCompletion` is driven exactly as `render_frame` does. The
  async worker pool + real deadline clock is `pull_service`. **No TSan
  obligation** in this task (the cache's own thread-safety is `cache.*`'s; this
  task issues single-threaded `lookup`/`insert`).
- **CI diff coverage ≥90%** (doc 16:112-118); the public header compiles
  standalone.

## Acceptance criteria

- **Unit tests — `src/compositor/t/tile_planning.t.cpp` (new, Catch2), registered
  via `arbc_component_test`.** Against a hand-populated `TileCache` (no backend
  needed for the pure planner):
  - **Grid geometry:** `tiles_covering(rung, local_region)` returns exactly the
    cells overlapping a region — a region spanning 2.5 cells covers 3 columns;
    a region exactly on a cell boundary does not double-cover; `tile_local_rect`
    round-trips (`tiles_covering` of `tile_local_rect(r, c)` contains `c`); the
    cell edge scales as `k_tile_size / rung_scale` across several rungs.
  - **Fresh hit → no request:** a cache pre-populated with qualifying tiles
    (`exact`, `achieved_scale == rung_scale`) at the fresh key plans every tile
    `Fresh` with `is_miss == false` and **zero** miss descriptors.
  - **Miss → deadline-carrying descriptor:** an empty cache plans every tile
    `is_miss == true`; each materialized request (built by a thin test harness or
    the driver) carries `exactness == BestEffort`, the passed `deadline`, the
    passed `snapshot`, `scale == rung_scale(rung)`, and `region ==
    tile_local_rect(rung, coord)`.
  - **Preference order:** with the fresh key absent but a `prior_revision` tile
    present → `Stale`; with neither fresh nor stale but a coarser-rung tile
    present → `Coarser` (and the coarser source's rung is recorded); with none →
    `Placeholder` (no pin). Assert the order is honored when *several* fallbacks
    coexist (fresh-absent + stale-present + coarser-present picks `Stale`).
  - **Hit qualification:** a resident entry at the fresh key with `exact ==
    false` or `achieved_scale != rung_scale` is **not** `Fresh` (falls through);
    only a qualifying entry is `Fresh`.
  - **Static vs Timed keying:** `Static` content plans keys with `achieved_time
    == nullopt`; `Timed` content plans keys carrying the requested `Time`; a
    second plan of the same `Static` scene at a different `Time` produces
    identical keys (clock-invariant).
  - **Visible class:** every planned tile carries `PriorityClass::Visible`.
- **Golden — `tests/tile_planning_golden.t.cpp` (new, cross-component, links
  `arbc` + `CpuBackend`).** Byte-exact (doc 16:47-53):
  - **Tiled == whole (power-of-two, axis-aligned):** `render_frame_interactive`
    of a solid-color / known-pattern layer at a power-of-two scale with an
    axis-aligned camera is **byte-identical** to `render_frame` (the offline
    whole-region path) of the same scene — proving the tile split + per-tile
    composite reconstructs the whole with no seam, gap, or double-blend, and that
    a power-of-two rung pays no resampling cost.
  - **Warm-cache second frame issues zero renders:** driving
    `render_frame_interactive` twice over an unchanged scene fills the cache on
    frame 1 and, on frame 2, plans **zero** `is_miss` tiles (assert on the plan /
    a render-request count captured by a test `Content` stub) while producing the
    byte-identical target — the cache-reuse property doc 04:93-94 promises.
- **Claims (register + `enforces:` tags)** appended to `tests/claims/registry.tsv`,
  enforced from the tests above:
  - `02-architecture#miss-becomes-deadline-request` — "A visible tile whose fresh
    key (current revision, current rung) misses the cache becomes a `BestEffort`
    render request carrying the frame's `Deadline` value and the pinned
    `StateHandle`, targeting exactly the tile's local-space footprint at
    `rung_scale`; a qualifying cache hit issues no request." (doc 02:57-65)
  - `02-architecture#degraded-fallback-preference-order` — "When a fresh exact
    tile is unavailable at plan time, the tile's display source is chosen in the
    order fresh → stale-revision → coarser-rung-rescaled → checkerboard/
    transparent, taking the first available; a stale tile is reached by a
    deliberate prior-revision probe, a coarser tile by probing successively
    coarser rungs at the current revision." (doc 02:63-65)
  - `11-time-and-video#static-tiles-survive-clock` — "Planning an all-`Static`,
    warm-cache scene after advancing only the clock re-plans to all fresh cache
    hits and issues zero render requests, because `Static` tile keys omit
    `achieved_time` and are therefore clock-invariant." (doc 11; doc 16:54's
    enforce-tag example) The tiled-== -whole golden additionally exercises
    `16-sdlc-and-quality#byte-exact-goldens` (`registry.tsv:35`); this task does
    not re-register that meta-claim.
- **No new WBS leaf.** Every deferral above lands on an **existing sibling**:
  async dispatch/deadline-clock → `compositor.pull_service`; prefetch/speculation/
  progressive refinement → `compositor.refinement`; damage / `achieved_time`
  bucketing → `compositor.damage_planning` (+ `cache.invalidation`); identity /
  operator graph → `compositor.operator_graph`; anchor-walk cull / `2^16` →
  `compositor.anchored_viewports`; counter-surface exposure → `compositor.counters`.
  The closer registers **no** new task for this refinement.
- **Component wiring & CI dependency check:** `src/compositor/CMakeLists.txt` adds
  `tile_planning.cpp` to `SOURCES`, `arbc/compositor/tile_planning.hpp` to
  `PUBLIC_HEADERS`, and `t/tile_planning.t.cpp` to the component test; the golden
  is added to `tests/CMakeLists.txt`; `DEPENDS` stays `contract cache`; the header
  compiles standalone; the doc-17 dependency check passes (no `backend-cpu` or
  same-level edge introduced).
- **Gate green (build + tiers 1-5 in Debug + ASan/UBSan).** No TSan obligation
  (single-threaded synchronous driver; the cache's concurrency is `cache.*`'s).

## Decisions

- **The plan is a pure value; filling the cache is the driver's, not the
  planner's.** `plan_layer` reads and pins (`lookup`) but never allocates,
  renders, or `insert`s — doc 02:123-125 requires planning to run under a
  snapshot with no lock and no race. This makes the planner unit-testable against
  a hand-populated `TileCache` with no backend/pool, keeps the L4 planning free of
  surface allocation, and gives `pull_service` a clean value to schedule against.
  *Rejected:* a fused plan-and-fill that acquires tile surfaces and renders inside
  `plan_layer` — it would couple planning to the pool/backend, force every planner
  test to stand up a full render stack, and blur the seam `pull_service` needs.
- **A miss carries its render descriptor *in the `PlannedTile`*, not as a
  pre-built `RenderRequest`.** `RenderRequest` holds a `Surface& target`
  (`content.hpp:73`) — a live reference that cannot exist in an allocation-free
  plan. The `PlannedTile` carries the target-free facts (`local_rect`, rung,
  class, `is_miss`); the driver (and later `pull_service`) materializes the
  `RenderRequest` when it acquires the tile target. *Rejected:* a parallel
  `TileRenderSpec` struct — redundant with the fields `PlannedTile` already needs;
  and *rejected:* embedding `RenderRequest` in the plan — forces target allocation
  into planning, the exact coupling the first decision avoids.
- **Single frame `Deadline`, copied verbatim onto every miss request; no per-tile
  sub-budgeting.** Doc 02 step 4 treats "the deadline" as one frame-level instant
  ("when the deadline nears, the frame proceeds"), and docs 05/13 make deadlines
  *propagated* values that "flow through pulls exactly as they flow through
  nesting" — never recomputed. The frame loop and the wall-clock live in
  `runtime` (doc 17:60), so the planner is *handed* a `Deadline` and stamps it on
  each request unchanged. *Rejected:* dividing a per-frame time budget across
  tiles — invents a scheduling policy the docs do not specify and needs a clock
  the L4 compositor must not read (that policy, if ever needed, is
  `pull_service`'s). *Rejected:* deriving the deadline from a frame-duration
  target inside the planner — reads a wall-clock at L4, forbidden. **No
  design-doc delta:** the `Deadline` type, its BestEffort-only applicability, its
  expiry semantics, and its propagation are all already normative; this is the
  minimal reading, not a change.
- **Stale reached by a caller-supplied `prior_revision` probe, not a
  cross-revision cache index.** Doc 02:94 says a revision bump makes old keys
  "unreachable" — meaning via the *normal* current-revision query. The stale
  fallback (listed first in the degradation order, so it must be servable)
  requires *some* reach to a prior-revision tile; the cheapest correct mechanism
  is a single extra `lookup` at the revision the caller (runtime frame loop) knows
  it last completed. The tile is still resident (invalidation, `cache.invalidation`
  + `damage_planning`, has not dropped it) — and once invalidation lands, the
  probe simply misses and falls to Coarser, so the mechanism is forward-compatible
  and needs no `cache.*` change. *Rejected:* a secondary `(content, rung, coord) →
  latest revision` index in the cache — that is `cache.invalidation`'s
  `(content, region)` index territory (a sibling not yet built), and building a
  second index here would duplicate it. *Rejected:* a frame-persistent hold map in
  the compositor — frame-to-frame state is the runtime loop's, not L4 planning's
  (keeps the planner pure). **No design-doc delta:** the probe realizes doc
  02:64's stale fallback without altering designed behavior.
- **Coarser fallback probes existing coarser rungs (bounded), not synthesizes on
  the spot in `plan_layer`.** The pure planner only *looks up* coarser rungs
  (current revision, rung−1, rung−2, …, to `k_max_fallback_octaves`) and picks
  the first covering hit; building a coarser rung that isn't cached (via
  `reduce_rung`) is a *fill*, so it belongs to the driver, not the planner. A
  bound (default 4 octaves) prevents an unbounded probe on a cold cache; past it,
  the tile falls to Placeholder. *Rejected:* an unbounded coarser probe — a cold
  cache would scan every rung to the coarsest; the bound is a cheap cutoff with a
  graceful Placeholder tail. The bound is a tunable constant, not designed
  behavior (doc 02:64 says "coarser-scale tiles rescaled" without a depth), so no
  delta.
- **`k_tile_size = 256`, even at every rung; reuse `TileCoord`/`TileKey` directly.**
  Doc 02:59 gives "e.g. 256² device pixels"; 256 is even so tiles satisfy
  `reduce_rung`'s even-dims precondition at every rung (the power-of-two tile
  geometry `scale_ladder.md:251-253` reserved for this task), and it is a named
  `constexpr` a later task can tune. The planner produces `cache::TileCoord`/
  `cache::TileKey` directly — the cache reserved grid geometry for the compositor
  (`key_shapes.md`), so there is no compositor-local tile-coordinate type.
  *Rejected:* a per-rung variable tile size — needless; a fixed device-pixel tile
  keeps every rung's grid uniform and the cache byte cost predictable.
- **Hit qualification (`exact` + matching `achieved_scale`) lives in the planner.**
  The `KeyedStore` returns any key match and leaves "does this hit qualify?" to
  the consumer (`keyed_store.hpp` contract). A best-effort or wrong-scale entry
  at the fresh key is *not* Fresh — the planner reads `TileValue.meta` and falls
  through the order. This keeps the store policy-free and puts the correctness
  rule (doc 02:84-85 "only exact-scale, current-revision entries qualify") where
  it belongs. *Rejected:* a `lookup` variant that filters by metadata inside the
  store — pushes a compositor policy into `cache.*` and duplicates the
  `Coarser`/`Stale` reads the planner already does.
- **The interactive driver is a new function beside `render_frame`, driven
  synchronously.** Doc 02:74-85 keeps the offline path exact-scale with no
  quantization; wiring tiles into `render_frame` would break that. The interactive
  `render_frame_interactive` mirrors the offline walk (reusing the cull/compose
  front half, `compositor.cpp:16-46`) but adds the rung-quantize → tile-split →
  cache-lookup → tiled-composite path, driving misses inline via `RenderCompletion`
  exactly as the offline frame does (`compositor.cpp:77-91`). The async worker
  pool and real deadline clock are `pull_service` + `runtime.interactive` (doc
  02:135-137 allows the v1 single-thread degeneration). *Rejected:* quantizing
  `render_frame` — breaks offline exactness and pre-empts nothing this task owns.
- **Claims registered against `02-architecture`/`11-time-and-video` heading
  stems, not new doc anchors.** Following the register's established form
  (`02-architecture#cache-evicts-lru-within-priority-class` et al. are *claim*
  slugs, not literal markdown anchors, `registry.tsv:53-58`), the three new claims
  reuse the governing doc stems with descriptive slugs — no design-doc edit is
  needed to mint them. The behaviors are already normative in doc 02 steps 3-4 and
  doc 11; this task registers and enforces them.
- **No design-doc delta.** Every rule here is settled doc text: the tile grid and
  local-space alignment (doc 02:57-60, 04:103-106), the miss→deadline-request and
  the fallback order (doc 02:61-65), the remainder composite (doc 02:66-68), the
  priority classes (doc 02:92-93), `Static` omits `achieved_time` (doc 11), the
  deadline propagation (docs 05/13), and the `contract, cache` edge (doc 17:56).
  This task *concretizes* those promises into C++ without altering designed
  behavior, so it needs no doc edit and no doc-00 bullet.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-05.

- **New header:** `src/compositor/arbc/compositor/tile_planning.hpp` — `tiles_covering`, `tile_local_rect`, `TileSource`, `PlannedTile`, `LayerTilePlan`, `plan_layer`, `render_frame_interactive`.
- **New impl:** `src/compositor/tile_planning.cpp` — interactive tile planner + synchronous tiled driver; first production caller of `TileCache`, `reduce_rung`, and the full scale-ladder + cache pipeline.
- **Unit tests:** `src/compositor/t/tile_planning.t.cpp` — 16 Catch2 cases covering grid geometry, fresh/miss, hit qualification, fallback order (Fresh→Stale→Coarser→Placeholder), Static/Timed keying + clock-invariance, and Visible priority class.
- **Golden tests:** `tests/tile_planning_golden.t.cpp` — 2 byte-exact cases: tiled==whole at power-of-two scale, and warm-cache second frame issuing zero renders.
- **CMake wiring:** `src/compositor/CMakeLists.txt` (+source, +header, +component test); `tests/CMakeLists.txt` (+golden target). `DEPENDS` stays `contract cache`; no new levelization edge.
- **Claims registered:** `tests/claims/registry.tsv` +3 rows — `02-architecture#miss-becomes-deadline-request`, `02-architecture#degraded-fallback-preference-order`, `11-time-and-video#static-tiles-survive-clock`.
- **Pool note:** misses render into cache-owned surfaces via `backend.make_surface` (since `TileValue` owns its `Surface`); `SurfacePool&` param used for transient coarser-fallback upscale temp only — intent (fill-on-miss, composite display source) preserved.
