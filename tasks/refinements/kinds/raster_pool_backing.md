# Refinement — `kinds.raster_pool_backing`

## TaskJuggler entry

`tasks/55-kinds.tji:32-37` — `task raster_pool_backing "org.arbc.raster
big-block pool tile backing"`, under `task kinds "Reference kinds"`
(55 — Reference kinds, docs 03/05/11/12/17).

> note "Migrate tile-pixel payloads from refcounted shared_ptr onto the
> doc-15 big-block pool once that L1 primitive exists; restores the doc-15
> tile-table/tile-pixel storage split. Docs 14/15. Source-of-debt:
> tasks/refinements/kinds/raster.md"

## Effort estimate

`effort 2d`, `allocate team`.

## Inherited dependencies

Declared edges (`tasks/55-kinds.tji:35`): `depends !raster, pool.big_block_pool`.

**Settled predecessors this task builds on (both `complete 100`):**

- `kinds.raster` — the consumer being migrated. Shipped `org.arbc.raster` as
  `src/kind_raster/` with a persistent CoW tile table whose tile-pixel blobs
  are held in **refcounted `std::shared_ptr<const TileBlob>` as a documented
  stand-in** (`tasks/refinements/kinds/raster.md:401`,
  `src/kind_raster/arbc/kind_raster/raster_content.hpp:54-57`). Its Decision
  *"Tile table in slabs, tile pixels in the big-block pool, shared by
  refcount"* (`raster.md:377-381`) named this migration as the follow-up, and
  its Status registered `kinds.raster_pool_backing` as the tech-debt leaf that
  closes it (`raster.md:405`). Everything raster's tests pin — conformance,
  byte-exact goldens, the CoW behavioral-counter claim, the TSan stress — is
  the regression envelope this task must keep green unchanged.
- `pool.big_block_pool` — the L1 primitive this task migrates onto (shipped
  commit at `tasks/refinements/pool/big_block_pool.md:359`). Provides, in
  `src/pool/arbc/pool/big_block_pool.hpp` (`namespace arbc`):
  - `BigBlockPool` (`:139`) — the dedicated page-aligned, size-classed,
    refcount-managed bulk-payload allocator, holding its own `Arena` over a
    caller-supplied `ChunkSource` (anonymous vs workspace-file).
  - `BlockSlotRef` (`:49`) — the compact **8-byte in-record** handle
    (`{ SlotIndex slot; std::uint32_t size; }` + debug generation), carrying
    its own logical size; the persistent stored form that replaces
    `TileBlobPtr`.
  - `BlockRef` (`:84`) — the owning transient RAII pin (`data()` `:95`,
    `size()` `:96`, `bytes() -> std::span<std::byte>` `:97`) returned by
    `allocate`/`resolve` for the fill-once and read phases.
  - `allocate(size) -> expected<BlockRef, PoolError>` (`:174`, writer-only),
    `retain(BlockSlotRef) -> expected<uint32_t, RefError>` (`:178`),
    `release(BlockSlotRef)` (`:184`, any-thread), `resolve(BlockSlotRef)`
    (`:188`), `peek(BlockSlotRef) -> std::span<const std::byte>` (`:193`,
    **zero-refcount hot-path read, touches no count**), `count(BlockSlotRef)`
    (`:196`), `blobs_allocated()` (`:208`, the allocation witness).

  The pool's threading contract is the load-bearing inheritance:
  `allocate` writer-only; `retain`/`release`/`resolve`/`peek`/`count`
  any-thread over per-slot atomics; blob pages immutable after fill (retain,
  release and peek never write a data page); zero-count release is an inline
  `SlotStore::release` on the writer/drain thread — RT threads only `peek`
  (`pool/big_block_pool.md:199-215, 265-283`).

**Pending (must not be assumed at implementation time):**

- `model.content_binding` — still not `complete 100`. Production runtime
  auto-registration of raster's sinks (and, downstream, the choice of a
  durable workspace-file `ChunkSource` for the pool) rides it via the existing
  `kinds.raster_runtime_binding` leaf; this task keeps raster's tests driving
  registration and pool construction directly, as `kinds.raster` already does.

## What this task is

Replace the placeholder `std::shared_ptr<const TileBlob>` backing of
`org.arbc.raster`'s tile-pixel payloads with allocations from the shipped
`arbc::pool` `BigBlockPool`, storing a compact `BlockSlotRef` in the tile
table where a `TileBlobPtr` lives today. This restores the doc-15 storage
split verbatim — **the tile *table* stays in slabs; the tile *pixels* move to
the dedicated big-block pool** (`docs/design/15-memory-model.md:20, 257`) —
and swaps the ad-hoc `shared_ptr` refcount for the pool's inside-out per-slot
refcount as the mechanism that shares untouched tiles across CoW versions.
This is a **storage-backing migration, not a behavior change**: every rendered
byte, every mip level, every `achieved_scale`/`exact` value, and the O(touched
tiles) capture/undo/damage discipline stay byte-for-byte identical — the
byte-exact goldens are the contract that they do.

## Why it needs to be done

- **Doc 15 mandates the split and names `shared_ptr` as the thing it
  replaces.** The **Bulk media data** population — "raster tile pixels …
  page-scale blobs, immutable once filled" — is specified to live in a
  "dedicated big-block pool (page-aligned, size-classed), refcounted from the
  owning nodes" (`15-memory-model.md:20`), with the explicit split "bulk pixel
  payloads go to the big-block pool with the tile *table* in slabs" (`:257`).
  `shared_ptr` appears in doc 15 only as the *rejected baseline the pool
  benchmarks against* (`:45-51, 77-107`), never as the intended tile-pixel
  backing. Raster's current `shared_ptr` blobs are an acknowledged
  implementation stopgap; this task lands the designed end state.
- **It discharges a registered source-of-debt with its L1 dependency now
  shipped.** `kinds.raster` deferred exactly this (`raster.md:377-381, 405`)
  and `pool.big_block_pool` shipped the primitive specifically to satisfy it
  ("it registers no new downstream WBS leaf — it satisfies one that already
  exists", `pool/big_block_pool.md:108-110`). Both halves are `complete 100`;
  this leaf joins them.
- **It proves the pool primitive under its first real consumer.** The pool's
  own tests exercise it in isolation; raster is the reference bulk-media
  population doc 15 designed the pool *for*. Migrating raster is the
  end-to-end proof that per-content dedicated pools, inside-out refcount
  sharing, and RT-safe `peek` reads carry a real CoW editable kind.
- Downstream milestone: the source-of-debt leaves from `kinds.raster` were
  wired to `m9_release` (`raster.md:405`); this task shares that milestone.

## Inputs / context

**Governing design-doc sections (normative, doc 16):**

- **doc 15 — Memory Model.** The **Memory populations** table (`15:16-22`):
  the **Content state nodes** row — "persistent tile *tables* … `Editable`
  states → same slabs" — and the **Bulk media data** row (`:20`) — "raster
  tile pixels … page-scale blobs, immutable once filled → dedicated big-block
  pool (page-aligned, size-classed), refcounted from the owning nodes." The
  storage-split statement (`15:257`): "bulk pixel payloads go to the big-block
  pool with the tile *table* in slabs." Inside-out refcount discipline
  (`15:40-44, 244-251`) — counts in parallel columns keyed by physical slot;
  `peek`-style reads touch no count. Version reclamation "refcounts as the GC"
  (`15:117-151`); render/audio threads "may pin/unpin (one refcount op)" but
  are otherwise read-only (`15:137-143, 156`). File-backed arenas / durability
  epoch quarantine (`15:171-236`) — inherited by the pool for free, out of
  scope to *wire* here.
- **doc 14 — Data Model and Editing.** The `Editable` facet + CoW tile-table
  reference proof (`14:133-174`): "a paint stroke copies only touched tiles,
  so `capture()` is O(1), undo memory is O(touched tiles), and damage equals
  the stroke's tile set"; "render workers see frozen pixels while the user
  keeps painting" (`14:161-165`). Render purity over the pinned snapshot
  (`14:181-190`). Doc 14 defers the *allocator* explicitly to doc 15
  (`14:207-208`) — the pixel-backing binding this task lands is doc 15's, and
  doc 14's tile-*table* contract is what must stay observably unchanged.
- **doc 17 — Internal Components.** Levelization (`17:41-49`): `arbc::pool` is
  **L1** (depends only on `base`); `kind-raster` is **L4**. A dependency on L1
  `arbc::pool` is a legal strictly-lower edge. The CI dependency + include-
  hygiene check validates the CMake target graph and the include graph
  (`17:41-44, 126-131`) — a direct `#include <arbc/pool/…>` requires a direct
  CMake edge.

**Real source seams (the migration surface):**

- `src/kind_raster/arbc/kind_raster/raster_content.hpp`:
  - `struct TileBlob { std::vector<float> px; }` (`:54-56`) and
    `using TileBlobPtr = std::shared_ptr<const TileBlob>;` (`:57`) — **the
    types this task removes.**
  - `struct Level { …; std::vector<TileBlobPtr> tiles; }` (`:63-69`) — the
    in-table blob-pointer grid; `tiles` becomes `std::vector<BlockSlotRef>`.
  - `class TileTable` (`:74-102`), holding `std::vector<Level> d_levels`
    (`:101`); `TileTable::pixel()` (`:88`), `level_pixels()` (`:92`),
    `byte_cost()` (`:95`) read blob floats and move to reading via `peek`.
  - `class RasterStore` (`:117-172`) — the CoW store: `build()` (`:124`),
    `resolve()` (`:127`), `paint()` (`:140`), `retain_version`/`release_version`
    (`:144-145`), `state_cost()` (`:147`), `blobs_allocated()` (`:152`), the
    `Version { TileTablePtr table; uint32_t refcount; }` vector (`:158-168`).
    Gains ownership of a per-content `BigBlockPool`.
- `src/kind_raster/raster_content.cpp`:
  - `new_blob(int edge, uint64_t& alloc)` (`:62-68`) — `make_shared<TileBlob>()`
    + zero-fill; the base-build and mip allocation path → `pool.allocate(...)`
    + fill-once through `BlockRef::bytes()`.
  - `put(std::shared_ptr<TileBlob>&, …)` (`:70`) — the pixel-write helper,
    rebased onto the writable `BlockRef` during fill.
  - level-0 + mip build (`:96, :126`), the **CoW clone path** in
    `RasterStore::paint` (`:307-321`, `make_shared<TileBlob>(*predecessor)` copy
    → touched-pixel mutate → swap) and the mip recompute above the touched
    region (`:331-344`) — the "copy only touched tiles" seam that becomes a
    pool `allocate` + copy-from-`peek` + mutate.
  - the level-list shallow copy `levels = baseTable->levels();` (`:296`) — where
    untouched tiles are shared; each shared `BlockSlotRef` must be `retain`ed.
  - the `alloc` counter threaded through `new_blob`/`paint` (`:293, :310, :344,
    :256, :375`) feeding `d_blobs_allocated` — cross-checked against
    `BigBlockPool::blobs_allocated()`.
- `src/kind_raster/CMakeLists.txt` — `arbc_add_component(NAME kind_raster …
  DEPENDS contract)` (`:2-5`); the `arbc_component_test` line (`:7`). The
  `DEPENDS` set gains a direct `pool` edge.
- Tests already gating raster behavior (the regression envelope):
  `src/kind_raster/t/raster_goldens.t.cpp` (byte-exact goldens, frozen tables
  embedded inline at `:128`, `[.regen]` case at `:283`, `k_edge = 4`),
  `src/kind_raster/t/raster_paint.t.cpp` (CoW touched-tile allocation-witness),
  `tests/raster_conformance.t.cpp`, `tests/raster_concurrency_stress.t.cpp`,
  `tests/pull_multitile_golden.t.cpp`.
- `src/pool/t/big_block_pool.t.cpp` — the pool's own test idiom to mirror for
  the new pool-backed assertions.

## Constraints / requirements

1. **Byte-exact output preservation (the primary gate).** No rendered pixel,
   mip level, `achieved_scale`, or `exact` value may change. The frozen golden
   tables in `raster_goldens.t.cpp` **must not be regenerated** — if they
   change, the migration is wrong. Blob pixels remain `edge·edge·4`
   working-linear premultiplied RGBA `float`s in row-major order; the
   `std::vector<float>` payload becomes the byte image of a pool blob (a
   `float[edge*edge*4]` view over `BlockRef::bytes()`), same values, same
   layout, different container.
2. **Store the in-record form, not the pin.** `Level::tiles` holds
   `BlockSlotRef` (the 8-byte position-independent handle carrying its own
   size), matching doc 15's "tile *table* in slabs holds refs to pixel blobs."
   `BlockRef` is used only transiently — the writable pin during fill and a
   short-lived resolved pin never stored in the table.
3. **Refcount sharing over the pool, driven writer/drain-side.** Untouched
   tiles are shared across CoW versions by the pool's per-slot refcount:
   copying a `Level` into a new version `retain`s each carried `BlockSlotRef`;
   destroying a `TileTable` version `release`s every `BlockSlotRef` it holds.
   These retain/release calls run on the writer or the housekeeping/drain
   thread (the `StateRefSink`/version-lifetime path,
   `raster_content.hpp:143-145, 224-232`), **never on an RT render worker** —
   preserving doc 15's "RT threads only peek" rule (`15:137-143`;
   `pool/big_block_pool.md:199-215`). See Decision 4 for why the version-pin
   discipline already guarantees this.
4. **Render reads via `peek`, zero refcount traffic.** `render` resolves the
   version the request pins (unchanged: snapshot handle or base) and reads tile
   pixels through `pool.peek(BlockSlotRef)` — a zero-count, any-thread,
   data-page-clean read. Render takes no per-read `retain`/`release`; the
   version pin (snapshot pin + `d_base_table` base pin) keeps the version — and
   therefore its blobs — alive across the read. `render_thread_safe()` stays
   honestly `true`.
5. **Per-content dedicated pool, anonymous backing by default.** `RasterStore`
   owns one `BigBlockPool` (doc 15's "dedicated" pool — a separate population
   from document records) over an `AnonymousChunkSource` it owns by default,
   and exposes the `ChunkSource` as a construction-time injection point so the
   runtime can later supply a durable workspace-file source (Decision 3). The
   pool's lifetime is the content's: content teardown reclaims every blob.
6. **Levelization / build wiring (CI-enforced, doc 17:41-44, 126-131).**
   `kind_raster` (L4) directly `#include`s `<arbc/pool/big_block_pool.hpp>`
   (L1) and links `BigBlockPool`'s non-header-only symbols, so
   `src/kind_raster/CMakeLists.txt` **must add a direct `pool` edge** to
   `DEPENDS` (a legal strictly-lower L1 edge). No `backend-cpu`/`cache`/
   `compositor` edge is introduced. The include-hygiene check must stay green.
7. **Pixels still flow through `media` only.** The mip box-downsample and the
   format-generic render still read/write working-linear floats via
   `PixelTraits`/`visit_surface`; only the *container* of those floats changes
   (pool blob instead of `std::vector`). No `backend-cpu` kernel is reached.
   The box-downsample / bilinear kernels are untouched — higher-order filters
   remain the separate `kinds.raster_resampling_quality` leaf.
8. **`blobs_allocated()` semantics preserved.** Raster's allocation witness
   still advances by exactly the number of distinct blob allocations (one per
   `pool.allocate`), and does **not** advance on retain/release/peek — now
   backed by (or cross-checked against) `BigBlockPool::blobs_allocated()`
   (`:208`), which the pool guarantees has exactly this behavior. The
   `#raster-paint-copies-only-touched-tiles` counter claim stays witnessed.

## Acceptance criteria

**Regression — the existing suite stays green, unchanged.** The migration is
correct iff the following pass with no golden regeneration and no assertion
edits:

- `src/kind_raster/t/raster_goldens.t.cpp` — byte-exact mip-pyramid and render
  goldens (native/0.5×/0.25×/past-native × three surface formats) remain
  **byte-identical**; the frozen embedded tables are untouched. This is the
  primary proof the backing swap changed no pixel.
- `tests/raster_conformance.t.cpp` — the `arbc::contract_tests` run
  (render-scale honesty, within-bounds, undamaged-region stability,
  render-purity-over-pinned-state, capture/restore round-trip, facet
  consistency, static-time invariance) stays green.
- `src/kind_raster/t/raster_paint.t.cpp` — the CoW touched-tile
  allocation-witness tests and the L2 end-to-end sink tests
  (`#pin-holds-content-state`, `#content-state-reclaimed-by-refcount`,
  `#coalesced-gesture-captures-once`) stay green, now over pool-backed blobs.
- `tests/pull_multitile_golden.t.cpp` — multi-tile compositor golden stays
  byte-identical.

**Existing claim, re-witnessed.**
`14-data-model-and-editing#raster-paint-copies-only-touched-tiles` — a paint
touching tile set T allocates exactly |T| new level-0 blobs (plus the mip
blobs geometrically above T) and shares every tile outside T by refcount.
Re-pin as a **behavioral-counter** test over the pool: the paint advances
`BigBlockPool::blobs_allocated()` by exactly |T|+mip, and each untouched
shared tile's `BigBlockPool::count(BlockSlotRef)` increments (no new
`allocate`) when the new version is interned. Never wall-clock.

**New claims-register entry (the storage split, doc 15 reference proof).** Add
to `tests/claims/registry.tsv` and pin with an `enforces:`-tagged test:

- `15-memory-model#raster-tile-pixels-pool-backed` — *`org.arbc.raster`'s
  tile-pixel payloads are allocated from a `BigBlockPool` (page-aligned,
  size-classed) with the tile table holding `BlockSlotRef`s in slabs; a level's
  tile handle resolves through `peek` to exactly `edge·edge·4·sizeof(float)`
  page-aligned bytes, untouched tiles are shared across CoW versions by pool
  refcount, and dropping a no-longer-shared version returns its blob to the
  pool's class free pool (slot reused, `total_bytes_reserved` flat).* Witnessed
  by a unit test asserting: (a) a resolved tile blob is `k_page`-aligned and
  the expected byte length; (b) a paint shares untouched blobs by
  `count()` bump with no `blobs_allocated()` advance for them; (c) releasing a
  non-base version drops the count to zero, `SlotStore::release` returns the
  slot, and the next same-size allocation reuses it with no capacity growth.
  This pins the O(touched-tiles) undo-memory reference proof at the pool layer.

**Concurrency (doc 16 — concurrency-touching task, explicit coverage).**
`tests/raster_concurrency_stress.t.cpp` re-runs green under TSan/asan with
pool-backed blobs: N worker threads `peek` a pinned `StateHandle` H
concurrently while an editor thread paints new versions (writer-side
`pool.allocate` + retain/release); assert every reader observes byte-stable
pixels for H, no reader ever drives a blob refcount (peek-only), and TSan
reports no data race. This is the end-to-end proof of Decision 4 — that blob
release stays off the RT path and `peek` is race-free against concurrent
writer allocation. (Standing caveat, per `pool/big_block_pool.md:279-283`: the
repo has no per-push TSan CI *lane* yet; the stress runs under the asan lane
meanwhile — wiring a TSan lane is a `.github/workflows/` infrastructure edit
for the parking lot, not a WBS leaf.)

**CI.** ≥90% diff coverage on changed lines; the doc-17 dependency +
include-hygiene checks pass with the new direct `kind_raster → pool` edge and
no `backend-cpu`/`cache`/`compositor` edge; `scripts/check_claims.py` green.

**Deferred follow-ups.** *None as a WBS leaf.* This task fulfills an existing
source-of-debt and introduces no new agent-implementable deferral: the mip
kernel quality is already `kinds.raster_resampling_quality`, and durable
workspace-file backing of raster's pool is runtime wiring gated on
`model.content_binding` (Decision 3), belonging to the existing
`kinds.raster_runtime_binding` scope rather than a new leaf. Surfaced to the
orchestrator in the return summary, not encoded here.

## Decisions

- **`BlockSlotRef` in the tile table; `BlockRef` only transient.** Doc 15's
  split is "tile *table* in slabs" holding refs to pool blobs; the in-record,
  position-independent, 8-byte `BlockSlotRef` is exactly that stored form and
  is what makes `capture()` a copy of index slots (O(touched)). `BlockRef` is
  an owning RAII pin — correct for the fill-once write and a transient resolved
  read, wrong to persist in the table (it would keep a live count per stored
  entry and is not a compact in-record type). *Alternative rejected:* storing
  `BlockRef` in `Level::tiles` — inflates the table, entangles table lifetime
  with pin lifetime, and defeats the slab-resident tile-table half of the
  split.
- **One dedicated `BigBlockPool` per content, not a process-global pool.** Doc
  15 calls the big-block pool a "dedicated" population with per-population
  accounting; a per-content pool makes content teardown deterministically
  reclaim all its blobs and keeps the CoW refcount arithmetic local to one
  content's versions. *Alternative rejected:* a shared global pool — conflates
  unrelated contents' lifetimes and accounting, and offers no benefit for the
  reference proof (blob sharing is *within* a content's version chain, never
  across contents).
- **Anonymous `ChunkSource` now; durable workspace-file backing deferred to
  runtime wiring.** Raster is codec-free and in-memory (its `DecodedImage` is
  handed in), and no per-document workspace file is wired at the kind layer
  yet — that plumbing rides `model.content_binding`. `AnonymousChunkSource` is
  the correct backing for the reference proof; the pool's `ChunkSource`
  injection point is left open so the runtime can supply a
  `WorkspaceFileChunkSource` (inheriting the checkpoint durability-epoch
  quarantine "for free", `pool/big_block_pool.md:210-215`) when it instantiates
  a content. *Alternative rejected:* wiring workspace-file backing here — it
  depends on runtime workspace plumbing that does not yet exist, so it cannot
  be a well-formed WBS leaf today; forcing it would couple this migration to an
  unbuilt seam. It belongs to `kinds.raster_runtime_binding`'s production-wiring
  scope, noted for the human in the return summary.
- **Version-pin lifetime keeps blob release off the RT thread — no per-read
  refcount.** A render worker resolves the version a request pins, but the
  version is held live independently by the model's snapshot pin and
  `RasterStore::d_base_table` (`raster_content.hpp:130-133, 170`), so the
  worker's transient view is never the last reference and dropping it never
  runs the `TileTable` destructor (hence never a `release`). Blob retain/release
  therefore only ever runs on the writer/drain thread via the version-lifetime
  path — matching doc 15's RT-thread rule and the pool's "RT threads only peek"
  contract. Render reads exclusively via `peek` (zero count traffic). This is
  the invariant the TSan stress test proves. *Alternative rejected:* wrapping
  `BlockSlotRef` in an RAII retain/release holder that fires in its destructor —
  a last-reference drop on a render worker would then call `release`
  (potentially to zero → `SlotStore::release`) on an RT thread, which doc 15
  and the pool refinement deliberately keep off the hot path; centralizing
  retain/release on the version-lifetime path avoids that.
- **Behavior-preserving migration — kernels and floats untouched.** Only the
  container of the tile floats changes; the box-downsample, bilinear sampler,
  and `PixelTraits` encode/decode math are byte-for-byte the same, which is why
  the frozen goldens must not move. This keeps the task at its 2d estimate and
  isolates the risk surface to the backing swap. *Alternative rejected:*
  bundling any resampling-quality change — that is the separate
  `kinds.raster_resampling_quality` leaf and would contaminate the goldens that
  are this task's correctness gate.
- **No design-doc delta.** Docs 14/15 already specify pool-backed tile pixels
  as the intended end state (`15:20, 257`), with `shared_ptr` named only as the
  rejected baseline; both predecessor refinements record "No design-doc delta"
  for this exact split. This task is a faithful implementation of the settled
  design, not an amendment — the only non-test edits are `src/kind_raster/`
  source + its `CMakeLists.txt`, riding the closer's commit (doc 16 same-commit
  rule).

## Open questions

(none — all decided.) One non-blocking item is surfaced to the closer in the
return summary rather than encoded as a WBS task: durable (workspace-file)
`ChunkSource` backing for raster's pool is real future runtime work but is
gated on `model.content_binding` plumbing that does not yet exist, so it
belongs to the existing `kinds.raster_runtime_binding` production-wiring scope,
not a new leaf.

## Status

**Done** — 2026-07-10.

- Migrated `Level::tiles` from `std::vector<TileBlobPtr>` to `std::vector<BlockSlotRef>` in `src/kind_raster/arbc/kind_raster/raster_content.hpp`; removed `TileBlob`/`TileBlobPtr` types entirely.
- Rewrote blob allocation and CoW clone paths in `src/kind_raster/raster_content.cpp` to use `BigBlockPool::allocate` (fill-once via `BlockRef`) and `retain`/`release` for version-lifetime sharing; render reads via `peek` (zero refcount traffic).
- `RasterStore` now owns an `AnonymousChunkSource` by default with a `ChunkSource&` injection constructor; `blobs_allocated()` backed by `BigBlockPool::blobs_allocated()`.
- Added `src/kind_raster/t/raster_pool_backing.t.cpp` — new unit test for claim `15-memory-model#raster-tile-pixels-pool-backed`: page-alignment/byte-length via `peek`, refcount-sharing across CoW versions, slot-reuse after non-base version release.
- Added `15-memory-model#raster-tile-pixels-pool-backed` to `tests/claims/registry.tsv`.
- Re-witnessed `#raster-paint-copies-only-touched-tiles` at the pool `blobs_allocated()` layer in `src/kind_raster/t/raster_paint.t.cpp`.
- Restructured `tests/raster_concurrency_stress.t.cpp` so main thread is the sole pool writer (build+paint loop) with 4 reader threads `peek`ing concurrently — matches pool's single-writer `SlotStore` contract without weakening any invariant.
- **Deviation noted for human review:** did not add a direct `pool` edge to `src/kind_raster/CMakeLists.txt` (refinement Constraint 6) because `scripts/check_levels.py` encodes `ALLOWED["kind_raster"]={contract}` and rejects it without editing the checker. Both `check_levels` and `check_claims` pass green via the transitive `kind_raster→contract→model→pool` path. See parking-lot entry 2026-07-10.
