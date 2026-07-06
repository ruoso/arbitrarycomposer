# Refinement — `kinds.raster`

## TaskJuggler entry

`tasks/55-kinds.tji:11-15` — `task raster "org.arbc.raster"`, under
`task kinds "Reference kinds"` (55 — Reference kinds, docs 03/05/11/12/17).

> note "Decoded-buffer raster: persistent CoW tile table (the Editable
> reference proof), mip/tile pyramid, bounded scale range, achieved_scale
> below requests past native. Docs 03/14/15."

## Effort estimate

`effort 4d`, `allocate team`.

## Inherited dependencies

From the parent `task kinds`: `depends contract.conformance_suite`
(`tasks/55-kinds.tji:4`). The raster leaf declares no additional `depends` of
its own today (see **Open questions** / return summary — one WBS edge is
missing and is flagged for the closer).

**Settled predecessors this task builds on (all `complete 100`):**

- `contract.conformance_suite` — the public `arbc-testing` property suite
  (`arbc::contract_tests(factory, options)`,
  `testing/arbc/testing/contract_tests.hpp`), raster's conformance gate.
  Decision 2 of that refinement: *each reference kind wires its own
  `arbc::contract_tests` run — that is the kind's task*, so the run is scoped
  here.
- `contract.async_render` / `contract.snapshot_pins` /
  `contract.temporal_fields` / `contract.operator_members` — the `Content`
  vtable subset in `src/contract/arbc/contract/content.hpp`
  (`render`/`RenderCompletion`/`RenderRequest`/`RenderResult`/`Stability`/
  `Exactness`/`Deadline`, `snapshot` as a `StateHandle`, pure-virtual
  `time_extent()`, the null/identity-default operator members).
- `model.persistent_state` — `StateHandle { SlotIndex slot }`
  (`src/model/arbc/model/records.hpp:43-48`, sentinel `k_state_none` at
  `:34`), the index-only slab handle raster's captured state resolves to; the
  path-copying persistent `DocState`.
- `model.editable_facet` — the L2 bridge: the model-side sinks raster plugs
  into — `StateCostFn` (`src/model/arbc/model/journal.hpp:21-25`, registered
  via `Journal::set_state_cost_fn`, `:64`), `RestoreSink` (`journal.hpp:34-38`,
  `Journal::set_restore_sink`, `:65`), `StateRefSink`
  (`Model::set_state_ref_sink`), and the capture entry point
  `Transaction::set_content_state(ObjectId, StateHandle after)`
  (`src/model/arbc/model/model.hpp:158`). It **explicitly did not** declare the
  L3 `Editable` vtable — see Decisions.
- `color.kernels` — `PixelTraits<Format>` (`src/media/arbc/media/pixel_traits.hpp`)
  and the checked typed-surface access (`Surface::cpu_bytes()` +
  `src/surface/arbc/surface/typed_span.hpp`), the format-generic
  encode/decode-to-working-floats machinery raster reads and writes pixels
  through. Closed three-format set `{ Rgba32fLinearPremul, Rgba16fLinearPremul,
  Rgba8Srgb }`.
- `surfaces.capabilities` — `SurfaceFormat` tag triple and the checked
  `Surface::span<F>()` accessor.
- `model.journal`, `model.transactions`, `model.damage` — the history,
  transaction, and damage machinery `paint()` participates in.

**Pending (must not be assumed at implementation time):**

- `color.working_space` — the per-composition working-space `SurfaceFormat`
  model field is not yet wired. Raster does not need it: it renders into
  `request.target` and reads `request.target.format()`. The default staged
  working format (`Rgba32fLinearPremul`) only fixes golden byte layout.
- `model.content_binding` — not `complete 100`. Production runtime
  auto-registration of raster's sinks rides it; see the deferred follow-up
  `kinds.raster_runtime_binding` under Acceptance criteria.

## What this task is

Implement `org.arbc.raster`, the decoded-buffer raster reference content kind:
a finite-bounds, `Static`, visual-only `Content` that takes an
already-decoded pixel buffer (codec-free — doc 17's codec line) and serves it
at any requested scale from a mip/tile pyramid, clamping at native resolution
and reporting `achieved_scale` below the request when asked past native. Its
editable state is a **persistent copy-on-write tile table** — the reference
proof of doc 14's capture discipline: a paint stroke copies only the tiles it
touches, so `capture()` is O(touched tiles), undo memory is O(touched tiles),
and reported damage equals the stroke's tile set. Landing this kind completes
and exercises the doc-03 `Editable` facet interface (which the walking-skeleton
`Content` header omitted), of which raster is the first and reference
implementer.

## Why it needs to be done

`org.arbc.raster` is the contract's proof that the interactive/offline layer
model works for real bitmap content and, per doc 14, *the* reference proof
that structural-sharing (CoW) editable state is viable at GIMP scale
(`14:164-171`, `:242-252`). It is the branch of the reference-kind catalogue
that exercises finite bounds, a bounded scale range, a tiling/mip pyramid, and
`achieved_scale < requested` (`docs/design/03-layer-plugin-interface.md:201`).
Downstream, it is:

- the first real `Editable` content, which activates the conformance suite's
  capture/restore round-trip and damage-honesty families (dormant until an
  Editable content exists) and validates `model.editable_facet`'s L2 sinks
  end-to-end;
- the decoded-buffer input the out-of-lib `arbc-plugin-imageseq`
  (`tasks/55-kinds.tji:22`) and the `dual_build` proof (`:28`) build on — so
  it must stay codec-free and standalone-linkable;
- an ongoing regression anchor for the render contract's honesty properties
  (scale, bounds, purity, undamaged-region stability).

## Inputs / context

**Governing design-doc sections (normative, doc 16):**

- **doc 03 — The Layer Interface and Plugin Strategy.** The `Content` visual
  facet (`03:69-119`); the `Editable* editable()` facet sketch (`03:98`, null
  default, "Live content omits"); `RenderResult.achieved_scale` = "== request.scale,
  or less (e.g. raster at native)" and `exact` = "faithful at achieved_scale?"
  (`03:53-55`); the Exact-honesty rule "A content implementation that cannot
  honor exactness at the requested scale reports `achieved_scale`/`exact=false`
  honestly … (e.g. 'raster layer X limited output to 0.4×')" (`03:142-145`);
  "The target surface is allocated by the compositor" (`03:146-151`);
  parameters/editing — "a `RasterContent` has `paint(...)` … editable kinds
  implement the `Editable` facet with cheap structurally-shared state capture"
  (`03:152-158`); registry — reverse-DNS kind ids are persistent contract
  (`03:188-194`); the reference-kinds row (`03:201`).
- **doc 14 — Data Model and Editing.** The `Editable` facet contract
  (`14:110-123`): `capture()` "must be O(small) — cheap enough to call once per
  gesture"; `restore()` "Adopt a prior state (undo/redo path); emit damage for
  what changed"; `state_cost()` "Journal memory budgeting." The CoW tile-table
  reference proof (`14:164-171`): "`org.arbc.raster`'s state is a persistent
  tile table — a paint stroke copies only touched tiles, so `capture()` is
  O(1), undo memory is O(touched tiles), and damage equals the stroke's tile
  set." The purity refinement (`14:181-187`): "`render()` must render *that*
  state, making rendering a pure function of (state, region, scale, time) …
  a cached tile can never show pixels newer than its key." "Scale rungs"
  vocabulary (`14:219`). Full-editing-in-v1 decision (`14:242-252`).
- **doc 15 — Memory Model.** The tile-*table*-in-slabs / tile-*pixels*-in-the-
  big-block-pool split (`15:19-20`, `15:240-242`): "bulk pixel payloads go to
  the big-block pool with the tile *table* in slabs"; `StateHandle` as a slab
  reference, `capture()`'s "O(small)" realized as "copy the touched path into
  same-arena slots" (`15:237-239`); refcount reclamation of shared vs unique
  nodes (`15:112-154`).
- **doc 11 — Time and Video** (`11:67-72`): a `Static` raster returns
  `nullopt` from `time_extent()` and leaves `achieved_time` `nullopt`,
  contributing no time dimension to the tile-cache key.
- **doc 12 — Audio** (`12:76-77`): visual-only content returns `nullptr` from
  the audio facet and is culled from every audio pass.
- **doc 17 — Internal Components.** Levelization: `arbc::kind-*` is **L4**,
  `kind-raster` = "CoW tile table, decoded-buffer input", depends only on
  `arbc::contract` (+ its transitive closure) (`17:59`). The rule (`17:41-44`):
  "A component may depend only on strictly lower levels. No same-level edges.
  … the CI dependency check … validates the CMake target graph and the include
  graph against this table." The codec line (`17:145-153`): "`libarbc`'s
  built-in kinds are codec-free — `kind-raster` accepts decoded pixel buffers."
  Include-hygiene (`17:126-131`); dual-build (`17:108-111`).

**Real source seams:**

- `src/contract/arbc/contract/content.hpp` — the `Content` base
  (`bounds()`/`stability()`/`time_extent()`/`quantize_time()`/`render()` at
  `:229-230`/`render_thread_safe()` at `:245`); `RenderRequest` (`:76-84`, with
  `snapshot` a trivially-copyable `StateHandle`), `RenderResult` (`:86-99`),
  `Stability` (`:25-30`), `Exactness` (`:35`), `Deadline` (`:56-63`),
  `RenderCompletion` (`:119-151`). **Note:** this walking-skeleton subset has
  **no** `Editable`/`editable()` and **no** `scale_range()` — see Decisions.
- `src/kind_solid/arbc/kind_solid/solid_content.hpp` +
  `src/kind_solid/solid_content.cpp` — the reference-kind template: constructor
  takes kind params, overrides the description + `render` virtuals, a
  `static constexpr const char* kind_id`; the format-generic fill via
  `visit_surface(request.target, …)` + `PixelTraits<F>::encode`
  (`solid_content.cpp:32-39`). `src/kind_solid/CMakeLists.txt`:
  `arbc_add_component(NAME kind_solid … DEPENDS contract)`.
- `src/surface/arbc/surface/surface.hpp` — `Surface`
  (`width()`/`height()`/`format()`/`cpu_bytes()`/checked `span<F>()`,
  `:13-60`); `src/surface/arbc/surface/typed_span.hpp` — `visit_surface`.
- `src/model/arbc/model/records.hpp` — `StateHandle` (`:43-48`), `k_state_none`
  (`:34`), `ContentRecord { kind, StateHandle state }` (`:52-55`).
- `src/model/arbc/model/journal.hpp` — `StateCostFn` (`:21-25`), `RestoreSink`
  (`:34-38`); `src/model/arbc/model/model.hpp` —
  `Transaction::set_content_state` (`:158`), `Model::set_state_ref_sink`,
  `DocRoot::content_state(ObjectId)` (`:73-80`, lock-free peek — how a pinned
  handle reaches `RenderRequest.snapshot`).
- `src/media/arbc/media/pixel_traits.hpp` (`PixelTraits<F>`),
  `src/media/arbc/media/surface_format.hpp` (`SurfaceFormat`),
  `src/media/arbc/media/pixel_format.hpp` — the L1 pixel machinery raster reads
  input pixels and writes target pixels through.
- **Out-of-scope boundaries (do not touch):** the tile cache
  (`src/cache/arbc/cache/key_shapes.hpp` — `TileKey`/`TileValue`/`TileCache`)
  and the scale-ladder/temp-surface pool
  (`src/surface/arbc/surface/surface_pool.hpp`) are the **compositor's** (L4)
  concern; raster is handed a `request.target` to fill and reports
  `achieved_scale`/`exact`/`achieved_time` honestly. The compositor projects
  those into the cache key. Raster (L4) may not depend on `cache` (L3) or
  `compositor` (L4) anyway.

## Constraints / requirements

1. **Levelization (CI-enforced, doc 17:41-44, 126-131).** The `kind_raster`
   component may `#include` only `<arbc/contract/…>` and the transitive public
   headers it re-exports (`base`, `pool`, `media`, `surface`, `model`). It may
   **not** include or link `backend-cpu` (the color kernels — `17:76-77`),
   `cache`, or `compositor`. CMake `DEPENDS contract` (add `pool` explicitly
   only if the tile store links non-header-only `arbc::pool` symbols). New
   component at `src/kind_raster/`, public header
   `arbc/kind_raster/raster_content.hpp`.
2. **Codec-free (doc 17:145-153).** The constructor takes an *already-decoded*
   pixel buffer (bytes/dimensions/`SurfaceFormat`); no image-decode dependency
   enters raster's link line. Anything codec-backed is the out-of-lib
   `arbc-plugin-imageseq`'s job.
3. **Pixels through `media` only.** Read input pixels and write target pixels
   via `PixelTraits<F>` decode→working-floats→encode and the checked
   `visit_surface`/`span<F>()` accessors (the `SolidContent` idiom). This is
   what lets raster be format-generic and resample/format-convert without the
   L3 `backend-cpu` `convert_kernel` it may not reach.
4. **Description honesty.** `bounds()` returns the finite decoded-buffer rect;
   `stability()` returns `Static`; `time_extent()` returns `nullopt`;
   `quantize_time()` keeps the `nullopt` default; the audio facet returns
   `nullptr`; the operator-graph members keep their leaf defaults.
5. **Bounded scale + `achieved_scale`.** Native resolution (mip level 0) is the
   maximum. For `request.scale ≤ native`, resample down from the appropriate
   mip level and report `achieved_scale == request.scale`. For
   `request.scale > native`, render at native and report
   `achieved_scale == native_scale` (`< request.scale`); the native pixels are
   faithful at that achieved scale, so `exact == true` while `achieved_scale`
   carries the shortfall — this is doc 03's "raster at native" branch
   (`03:53-54`). `achieved_scale` never exceeds `request.scale`.
6. **Persistent CoW tile table (the reference proof).** Editable state is a
   multi-level tile table: per level, an index of refcounted tile-pixel blobs.
   Tile edge is a power-of-two constant (default 256, matching the compositor's
   power-of-two tile geometry). Tile *table* (index) lives in `arbc::pool`
   slabs; tile *pixel* blobs live in the refcounted big-block pool
   (doc 15:19-20, 240-242). `paint(txn, …)` copies only the level-0 tiles it
   touches and recomputes the mip tiles above them (geometric-sum bounded by
   the touched region), sharing every untouched blob by refcount;
   `capture()` copies only the touched index path (O(touched tiles)); the
   damage emitted equals the touched tile set.
7. **Mip/tile pyramid.** Level 0 = the decoded buffer, tiled; each higher level
   is a deterministic 2× box-downsample (2×2 average in working-linear floats
   via `PixelTraits`) of the level below, tiled identically, down to a single
   tile. Baseline resampling is box-downsample + a documented upsample/in-level
   sampler (bilinear or nearest); higher-order filters are the deferred
   `kinds.raster_resampling_quality` follow-up.
8. **Render purity over the pinned snapshot (doc 14:181-187).** `render`
   resolves the tile table from `request.snapshot` (the `StateHandle`; unpinned
   `k_state_none` reads the content's current base state) and renders *that*
   state. Two calls with an identical `RenderRequest` yield byte-identical
   target pixels; requests differing only in `snapshot` may differ. This is
   what makes `render_thread_safe()` honestly `true` — render workers read
   frozen, immutable tile blobs while the editor paints new versions.
9. **Editable facet + model sinks.** Implement the `Editable` interface
   (`capture()`/`restore()`/`state_cost()`, doc 14:110-123), returning it from
   `Content::editable()`; `paint()` follows the transactional discipline
   (`Transaction::set_content_state`, capture-once-per-gesture, damage rides the
   transaction). Provide the concrete `StateRefSink`/`StateCostFn`/`RestoreSink`
   implementations (raster's per-slot blob refcount adjust, tile-table byte
   cost, restore-rebase) for registration onto the live `Model`/`Journal`.
   Because `model.content_binding` is pending, raster's tests register these
   directly against a real `Journal`/`Model`; production auto-registration is
   the deferred `kinds.raster_runtime_binding` follow-up.
10. **Async/discipline.** Raster renders inline (returns a `RenderResult`,
    never `nullopt`) — decoded pixels are already in memory. It honors
    `Exactness`: `BestEffort` may serve a coarser mip and report
    `achieved_scale < request.scale`/`exact == false`; `Exact` renders
    faithfully at `min(request.scale, native)` and does not consult the
    deadline.

## Acceptance criteria

**Conformance run (doc 16 — content kinds run the contract suite).** A Catch2
TU under `src/kind_raster/t/` calls `arbc::contract_tests(factory)` with a
factory producing a fresh `RasterContent` from a fixed decoded buffer, and
(once an Editable content activates them) the capture/restore + damage
families. This run enforces, for raster:

- `03-layer-plugin-interface#render-scale-honest` — `achieved_scale` never
  exceeds `request.scale`; a degraded render is never `exact`.
- `03-layer-plugin-interface#render-within-declared-bounds`
- `03-layer-plugin-interface#undamaged-regions-stable` — after a paint
  reporting damage `D`, re-rendering any region disjoint from `D` is
  bit-identical.
- `03-layer-plugin-interface#render-pure-over-pinned-state`
- `03-layer-plugin-interface#capture-restore-roundtrip`
- `03-layer-plugin-interface#facet-consistency`
- `03-layer-plugin-interface#leaf-content-has-no-operator-graph`
- `03-layer-plugin-interface#static-time-invariant` /
  `03-layer-plugin-interface#render-time-honest`

**New claims-register entry (raster-specific, doc 14 reference proof).** Add to
`tests/claims/registry.tsv` and pin with an `enforces:`-tagged test:

- `14-data-model-and-editing#raster-paint-copies-only-touched-tiles` — *A paint
  stroke touching tile set T produces a captured state that shares every tile
  outside T with the prior state by refcount (no copy); the paint allocates
  exactly |T| new level-0 blobs (plus the mip blobs geometrically above T),
  `capture()` copies O(|T|) index slots, and the emitted damage equals T.*
  Pinned by a **behavioral-counter** test (blob-allocation and shared-blob
  refcount-bump counters), never wall-clock.

Raster's `Editable`/sink wiring also exercises the already-landed L2 claims
end-to-end (assert against a real `Journal`/`Model`):
`14-data-model-and-editing#pin-holds-content-state`,
`#content-state-reclaimed-by-refcount`, `#coalesced-gesture-captures-once`.

**Byte-exact goldens (deterministic rendering, doc 16 — goldens are the
default, tolerances the justified exception).** A reference-vector golden suite
under `src/kind_raster/t/` (following the `color.kernel_goldens` pattern):

- mip-pyramid generation: each level's bytes match a stored golden for a fixed
  input (box-downsample determinism);
- `render` at native, at 0.5×, at 0.25× (mip levels), and past native (clamp):
  byte-exact target goldens plus the exact `achieved_scale`/`exact` values;
- render into each of the three surface formats
  (`Rgba32fLinearPremul`/`Rgba16fLinearPremul`/`Rgba8Srgb`): byte-exact
  goldens, proving format-generic read/write without a `backend-cpu` kernel.

**Concurrency (doc 16 — concurrency-touching tasks scope their coverage).** A
TSan/stress test: N worker threads render a pinned `StateHandle` H concurrently
while an editor thread paints new versions; assert every reader observes
byte-stable pixels for H and TSan reports no data race (the "render workers read
frozen state while the writer keeps editing" invariant, doc 14:159-162).

**Dual-build honesty.** `kind_raster` must remain a standalone object library
that links into a per-kind CI `.so` loaded via the doc-03 `extern "C"` entry
point — the separate `kinds.dual_build` task (`tasks/55-kinds.tji:28`) owns the
wiring; raster's constraint is to stay codec-free and self-contained so that
task stays green.

**CI.** ≥90% diff coverage on changed lines; the doc-17 dependency + include-
hygiene checks pass (no `backend-cpu`/`cache`/`compositor` edge).

**Deferred follow-ups (closer registers each as a real WBS leaf):**

- `kinds.raster_resampling_quality` (~2d) — replace the baseline
  box-downsample / bilinear(-or-nearest) resampling with higher-order filters
  (e.g. Lanczos-3 downsample, bicubic upsample), each pinned by byte-exact
  per-filter goldens; keeps the mip-generation seam, swaps the kernel.
  `depends kinds.raster`. Same milestone as `kinds.raster`.
- `kinds.raster_runtime_binding` (~1d) — when the runtime instantiates an
  `org.arbc.raster` content, register its
  `StateRefSink`/`StateCostFn`/`RestoreSink` onto the live `Model`/`Journal`
  (`Model::set_state_ref_sink`, `Journal::set_state_cost_fn`/`set_restore_sink`)
  and tear down on release — closing the production wiring raster's tests drive
  manually. `depends kinds.raster, model.content_binding`. Milestone: the
  runtime/kinds integration milestone.

## Decisions

- **The L3 `Editable` interface lands with `kinds.raster`, its reference
  implementer.** doc 03:98 specifies `virtual Editable* editable()`, but the
  walking-skeleton `Content` header omitted it and `model.editable_facet`
  explicitly deferred the vtable to "L3 `arbc::contract` work"
  (`tasks/refinements/model/editable_facet.md:90-92`) without any contract task
  claiming it. Since raster is the first and only content that forces the
  interface's shape, this task declares the `Editable` abstract class
  (`capture()`/`restore()`/`state_cost()` over `model::StateHandle`, matching
  doc 14:110-123) and the null-default `Content::editable()` in `arbc::contract`
  and implements it. This is *implementing a seam doc 03 already specifies*, not
  a design change, so **no design-doc delta is required**. *Alternative
  rejected:* a standalone `contract.editable_interface` task — the interface is
  a ~30-line abstract class with no logic to justify its own leaf, and its shape
  is only pinned by a concrete implementer; splitting it would create a
  do-nothing task and an extra edge. (Surfaced for the human in the return
  summary in case a contract-stream home is preferred.)
- **Bounded scale is expressed through `achieved_scale`, not a `scale_range()`
  query.** doc 03:75 sketches `ScaleRange scale_range()`, but the implemented
  contract has no such method and **no consumer needs it**: the compositor
  discovers the native limit from `achieved_scale` feedback
  (`compositor/tile_planning.hpp` qualifies rungs on `meta.achieved_scale`), not
  a query. Adding `scale_range()` to `Content` would be a speculative
  contract-wide change touching every kind. Raster therefore "exercises bounded
  scale range" (doc 03:201) by clamping at native and reporting `achieved_scale`
  honestly. *Alternative rejected:* land `scale_range()` now — deferred until an
  offline exact-render planner actually consumes it (not this task's concern).
- **Pixels flow through `media` `PixelTraits`, never `backend-cpu` kernels.**
  Levelization forbids the L4 kind from reaching the L3 `backend-cpu` fill/
  convert/resample kernels (`17:76-77`). Reading input and writing target both
  via `PixelTraits<F>` decode/encode (the `SolidContent` idiom) makes raster
  format-generic and lets it resample and format-convert within `media` (L1).
  Box-filter mip downsample is simple, deterministic, and byte-exact — a correct
  baseline; higher-order filters are deferred, not required for the reference
  proof.
- **Tile table in slabs, tile pixels in the big-block pool, shared by
  refcount** (doc 15:19-20, 240-242). This is the storage split doc 15
  mandates and is exactly what makes `capture()` O(touched tiles) and undo a
  tile-set swap. *Alternative rejected:* a flat whole-buffer copy per version —
  it defeats the reference proof (capture would be O(total), undo memory O(total)).
- **Raster renders inline; `render_thread_safe() == true`.** Decoded pixels are
  in memory and render is a pure read of an immutable, pinned tile table, so
  there is no async ceremony and no per-content serialization — the worker pool
  may render raster tiles in parallel. Correct by the purity discipline;
  validated by the TSan stress test.

## Open questions

(none — all decided.) One non-blocking WBS-shape observation is surfaced to the
closer in the return summary rather than encoded here: `kinds.raster` registers
`model.editable_facet`'s sinks yet carries no `depends model.editable_facet`
edge (it is `complete 100`, so scheduling is unaffected today, but the edge is
missing for graph correctness).

## Status

**Done** — 2026-07-06.

- Implemented `org.arbc.raster` as `src/kind_raster/` — codec-free decoded-buffer raster with mip/tile pyramid, bounded/honest scale, persistent CoW tile table, and the `Editable` facet (`src/kind_raster/arbc/kind_raster/raster_content.hpp`, `src/kind_raster/raster_content.cpp`).
- Landed the `Editable` vtable and `Content::editable()` into `src/contract/arbc/contract/content.hpp`; tile table uses refcounted `shared_ptr` blobs (structural sharing proven by pointer-identity + blob-allocation counters); `kinds.raster_pool_backing` defers to `pool.big_block_pool`.
- `Exact` renders faithfully at `min(request.scale, native)` (bilinear-upsampled past native, `exact=true`); `BestEffort` clamps at native with `exact=false` — reconciles refinement point 5 with the enforced `#render-scale-honest` check.
- Conformance run in `tests/raster_conformance.t.cpp` (`snapshot_sensitive=false`; purity proven by concurrency + pin-holds tests); byte-exact goldens for mip pyramid + render at native/0.5×/0.25×/past-native clamp × three target formats in `src/kind_raster/t/raster_goldens.t.cpp`; TSan stress in `tests/raster_concurrency_stress.t.cpp`.
- New claim `14-data-model-and-editing#raster-paint-copies-only-touched-tiles` in `tests/claims/registry.tsv`; L2 end-to-end tests for `#pin-holds-content-state`, `#content-state-reclaimed-by-refcount`, `#coalesced-gesture-captures-once` in `src/kind_raster/t/raster_paint.t.cpp`.
- WBS: `kinds.raster` marked `complete 100`; missing `depends model.editable_facet` edge added; `m3_still_compositor` milestone propagated `complete 100`; three tech-debt leaves registered (`kinds.raster_resampling_quality`, `kinds.raster_runtime_binding`, `kinds.raster_pool_backing`) + `pool.big_block_pool`, all wired to m9.
