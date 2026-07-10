# pool.big_block_pool — Big-block pool

## TaskJuggler entry

`tasks/05-pool.tji` → `pool.big_block_pool` ("Big-block pool").

## Effort estimate

2d.

## Inherited dependencies

- `pool.refs` — **settled** (commit `3898878`), the task's declared
  `depends !refs`. Provides the ownership model this task mirrors for byte
  blobs: `Ref<T>` (pointer+index owning handle, `src/pool/arbc/pool/refs.hpp:100`),
  `SlotRef<T>` (compact position-independent in-record form, `:64`), the
  overflow-checked retain / release / `count` discipline (`:322-346`,
  `RefError::CountOverflow` at `:40`), the `try_retain` CAS loop (`:517-527`),
  and the underflow-checked `release_index` (`:529-535`). `BlockRef` /
  `BlockSlotRef` are the byte-blob analogues of `Ref` / `SlotRef`; the
  retain/release logic copies refs.hpp's discipline verbatim (uint32,
  loud overflow, `assert` underflow).
- `pool.refcounts_in_store` — **settled** (commit `e5d164c`). Moved the
  inside-out refcount column into `SlotStore`, exposing
  `SlotStore::count_ref(SlotIndex) -> std::atomic<std::uint32_t>&`
  (`src/pool/arbc/pool/slot_store.hpp:220-224`) and (debug)
  `generation_ref` (`:231-235`). This is what lets the big-block pool do
  refcounting over a **bare `SlotStore`** without minting a `RefStore<T>`:
  the count column is a store property, not a typed-view property. Without
  this task the counts would live in `RefStore<T>` and byte blobs (which
  have no `T`) could not be counted.
- `pool.arena_core` — **settled** (commit `32c0d5d`). Provides the
  substrate this task composes: `Arena::store_for(slot_size, slot_align,
  chunk_bits, refcount_backing)` already routes by size class, keyed by
  `(slot_size, slot_align)` (`src/pool/arbc/pool/slot_store.hpp:350-368`);
  `SlotStore` (fixed-slot, chunk-backed, address-stable, no fragmentation,
  `:132-330`); `SlotStore::resolve(index) -> void*` (`:177`),
  `slots_live`/`bytes_reserved` accounting; `default_chunk_bits(stride)`
  (`:36-44`); `ChunkSource` / `AnonymousChunkSource`
  (`src/pool/arbc/pool/chunk_source.hpp`) and `expected<…, PoolError>`.
- `pool.free_pools` — **settled** (commit — see `tasks/refinements/pool/free_pools.md`).
  Relaxed `SlotStore::release`/`free_now` to **any thread** with thread-local
  free pools and global spill (`slot_store.hpp:112-131,157-174`). The
  big-block zero-count handler bottoms out in `SlotStore::release`, so blob
  reclamation inherits cross-thread release and thread-affine reuse for
  free — a blob freed on the housekeeping drain thread costs no lock on the
  hot path.
- `pool.checkpoints` — **settled** (commit `b1036d9`). Installed the nullable
  `ReleaseFence` between `SlotStore::release` and the free list
  (`slot_store.hpp:52-66,163-174`). Because big-block release routes through
  `SlotStore::release`, a workspace-file-backed big-block pool inherits the
  durability-epoch quarantine of freed blobs with **no new code** — the
  fence sees blob slots exactly as it sees record slots.
- `pool.mmap_backing` — **settled** (commit — see
  `tasks/refinements/pool/mmap_backing.md`). Made backing a `ChunkSource`
  policy (anonymous vs file-backed `WorkspaceFileChunkSource`). The big-block
  pool takes a `ChunkSource&`, so the consumer chooses per population:
  painted-tile pixels (undo state, must persist) → workspace-file source;
  decoded frames (re-decodable) → anonymous.

Note on the declared edge: the WBS declares only `depends !refs`, but the
design leans on `refcounts_in_store`, `free_pools`, and `checkpoints`
deliverables above. All are `complete 100` at refinement time, so there is
no scheduling hazard; the closer may optionally tighten the `.tji` edge to
`!free_pools` (which transitively pulls refs/refcounts_in_store/reclamation)
to make the graph honest, but it is not load-bearing today.

## What this task is

Add a **variable-size, page-aligned, refcount-managed bulk-payload
allocator** — `BigBlockPool` — to `arbc::pool`, the "dedicated big-block
pool" doc 15 names for the *bulk media data* memory population (raster tile
pixels, decoded frames, audio sample runs), as distinct from the small
fixed-size document-record slabs (`15-memory-model.md:16-22`). A caller asks
for a blob of N bytes and gets back a page-aligned, writable byte span it
fills once; the blob is shared across owners by reference count and returns
to its size class's free pool when the last owner releases it.

The implementation is a **thin size-classed façade over the existing
`Arena`/`SlotStore` machinery, not a new allocator**: each size class is one
`SlotStore` whose slot stride is the class's byte size, minted on demand
through `Arena::store_for`. "Variable-size" is realized as round-up to a
power-of-two page-multiple size class; "refcounted" reuses the store-owned
inside-out count column (`SlotStore::count_ref`); "page-aligned" is the
store's `slot_align`. Two reference forms mirror `pool.refs`: `BlockRef`
(owning transient handle carrying a data pointer) and `BlockSlotRef` (compact
position-independent in-record form). The central simplification over
`RefStore<T>` is that **byte blobs run no destructor and hold no child
references**, so there is no zero-count sink, no reclaim-link stack, and no
deferred drain — the zero-count handler is a single `SlotStore::release`.

## Why it needs to be done

- Doc 15 mandates the split explicitly: "bulk pixel payloads go to the
  big-block pool with the tile *table* in slabs"
  (`15-memory-model.md:240-242`) and the memory-population table's
  **Bulk media data** row: "dedicated big-block pool (page-aligned,
  size-classed), refcounted from the owning nodes" (`:20`). Every other
  population already has its allocator; this is the one still missing.
- It is the fulfillment of a registered source-of-debt.
  `kinds.raster`'s tile table currently stores pixels in refcounted
  `std::shared_ptr<const TileBlob>` blobs as a stand-in
  (`tasks/refinements/kinds/raster.md:405`;
  `src/kind_raster/arbc/kind_raster/raster_content.hpp:50-57`). The
  consumer-side migration task `kinds.raster_pool_backing`
  (`tasks/55-kinds.tji:32-37`, `depends !raster, pool.big_block_pool`)
  swaps the `shared_ptr` blobs onto this pool, restoring the doc-15
  tile-table-in-slabs / tile-pixels-in-big-block-pool storage split. This
  task ships the L1 primitive that migration depends on; **it registers no
  new downstream WBS leaf — it satisfies one that already exists.**
- Downstream milestone: `pool.big_block_pool` is a named dependency of
  `m9_release` (`tasks/99-milestones.tji:71`).

## Inputs / context

- `docs/design/15-memory-model.md`:
  - **Memory-populations table** (`:16-22`), the **Bulk media data** row
    (`:20`): "page-scale blobs, immutable once filled … dedicated big-block
    pool (page-aligned, size-classed), refcounted from the owning nodes."
    The normative spec.
  - **Storage-split** paragraph (`:237-242`): "bulk pixel payloads go to the
    big-block pool with the tile *table* in slabs." The tile *index* stays in
    record slabs and holds `BlockSlotRef`s to the pixel blobs.
  - **Inside-out refcount** discipline (`:41-44,:225-236`): counts live in
    parallel columns keyed by physical slot, never in the data page; reads
    (`peek`) touch no count. Blobs are "immutable once filled," so their data
    pages stay clean exactly like record pages.
- `docs/design/14-data-model-and-editing.md:164-171,218-221`: the raster
  reference kind — "a paint stroke copies only touched tiles … undo memory is
  O(touched tiles) … undo is a tile-set swap." The CoW structural-sharing
  behavior this pool's refcount sharing underpins.
- `src/pool/arbc/pool/slot_store.hpp` — `Arena::store_for` size-class routing
  (`:350-368`), `SlotStore` (`:132-330`), `count_ref` (`:220-224`),
  `resolve` (`:177`), `release`/`free_now` any-thread (`:157-174`),
  `set_release_fence` (`:166-168`), `default_chunk_bits` (`:36-44`),
  `align_up` (`:47-49`), accounting (`:238-260`).
- `src/pool/arbc/pool/refs.hpp` — the ownership pattern to mirror: `Ref<T>`
  (`:100-225`), `SlotRef<T>` (`:64-88`), the `static_assert`s pinning
  standard-layout / trivially-copyable in-record refs (`:568-574`), the
  retain/release/overflow logic (`:322-346,:517-535`).
- `src/kind_raster/arbc/kind_raster/raster_content.hpp:30-68` — the consumer:
  `k_default_tile_edge = 256`, `k_tile_channels = 4`, `struct TileBlob {
  std::vector<float> px; }`, `using TileBlobPtr = std::shared_ptr<const
  TileBlob>;`. A default tile blob is 256·256·4 = 262 144 floats = 1 MiB at
  fp32 — a power-of-two page multiple that hits one size class with zero
  waste.
- `docs/design/16-sdlc-and-quality.md`: behavioral-counter discipline
  (`:54-62`) and the concurrency-test tier (`:66-73`, TSan + stress).
- `docs/design/17-internal-components.md:41-49`: `arbc::pool` is Level 1,
  depends only on `base`; this task stays entirely within `arbc::pool`.

## Constraints / requirements

- **Size-classed round-up, page-aligned.** `allocate(size)` rounds `size` up
  to a size class `class_stride(size)` and mints/finds the class's store via
  `Arena::store_for(class_stride, k_page, /*chunk_bits=*/0)`. The class ladder
  is powers of two with a one-page floor:
  `class_stride(size) = size <= k_page ? k_page : next_pow2(size)`. Every rung
  is a power-of-two ≥ `k_page` (4096), hence page-aligned; `slot_align =
  k_page` guarantees each blob address is page-aligned (chunk bases from
  `ChunkSource` are already page-aligned, and a page-multiple stride keeps
  every slot aligned). Uniform blobs (all raster tiles of a config are
  `edge²·channels·4` bytes) land in exactly one rung with zero internal
  waste; the worst case for arbitrary sizes is the standard <2× size-class
  bound, acceptable for page-scale immutable media and bounded to
  O(log₂(max_blob/page)) distinct stores.
- **Refcount over the store-owned column, no destructor, no cascade.**
  Reference counts are `SlotStore::count_ref(slot)` — the inside-out
  `std::atomic<std::uint32_t>` column `refcounts_in_store` relocated into the
  store. `retain` is the overflow-checked CAS from `refs.hpp:322-333`
  (returns `RefError::CountOverflow` on saturation, never wraps); `release`
  is `fetch_sub` with an underflow assert; on the last release the handler is
  simply `store.release(slot)` — **no `~T` (blobs are trivially
  destructible), no reclaim-link enqueue, no deferred drain** (blobs are
  reclamation leaves; they hold no child references to cascade). This is the
  deliberate divergence from `RefStore<T>`: the big-block pool needs none of
  reclamation's destructor machinery.
- **Immutable-after-fill, inside-out reads.** `allocate` returns a *writable*
  `BlockRef` for the fill-once phase; after publish, holders read through
  `peek(BlockSlotRef) -> std::span<const std::byte>`, which touches **no
  count** (the traversal convention of `refs.hpp:95-99,309-316`). Retain and
  release never write a data page, so blob pages stay clean for shared and
  read-only mappings.
- **Two reference forms, mirroring `pool.refs`.**
  - `BlockSlotRef` — the only in-record form: `{ SlotIndex slot;
    std::uint32_t size; }` in release builds (plus a `std::uint32_t
    generation` in debug, matching `SlotRef`'s `#ifndef NDEBUG` pattern). It
    carries the logical byte length so the exact span is reconstructible from
    the ref alone and the size class is a pure function of `size`
    (`class_stride(size)`) — **no per-slot side column is needed.**
    `static_assert` standard-layout + trivially-copyable (it lives in mmapped
    records); it is 8 bytes in release (larger than `SlotRef`'s 4, which is
    fine — a record holds few big-block refs next to megabyte payloads).
  - `BlockRef` — owning transient handle: pool pointer + slot + size + cached
    `std::byte*` data pointer (+ debug generation). Copy retains, move steals,
    destructor releases (RAII, adopting-constructor pattern from
    `Ref<T>`). API: `data()`, `size()`, `bytes() -> std::span<std::byte>`,
    `slot() -> BlockSlotRef`, `explicit operator bool`.
- **Threading contract inherited from `SlotStore`, unchanged.** `allocate` is
  **writer-only** (arena growth is single-threaded — same rule as
  `SlotStore::allocate`). `retain`/`release`/`resolve`/`peek`/`count` are
  any-thread (per-slot atomics). Driving a count to zero calls
  `SlotStore::release`, which `free_pools` made any-thread; blob release
  therefore happens on the writer or the single housekeeping drain thread
  (the same threads that reclaim records), never on an RT render/audio thread
  — RT threads only `peek` (zero refcount traffic), consistent with doc 15's
  thread rules (`15-memory-model.md:137-143`). No new deferred-release queue
  is introduced; if a future kind ever needs RT-thread blob release, that is
  new work, not scoped here.
- **Backing-policy agnostic; durability for free.** `BigBlockPool` takes a
  `ChunkSource&` (or owns a default `AnonymousChunkSource`) and holds its own
  dedicated `Arena` over it — separate from the model's document-record arena
  (doc 15's "dedicated" pool). A `ReleaseFence` installed on a class store
  (checkpoints) quarantines freed blobs by durability epoch with no big-block
  code change.
- **Levelization.** New files `src/pool/arbc/pool/big_block_pool.hpp` and
  `src/pool/big_block_pool.cpp`, wired into `src/pool/CMakeLists.txt`
  (`arbc_add_component` `SOURCES`/`PUBLIC_HEADERS`). Everything stays in
  `namespace arbc` within `arbc::pool`, depending only on `base` (doc 17
  `:49`). No new component edge.

## Acceptance criteria

- **Unit tests** (`src/pool/t/big_block_pool.t.cpp`, wired into
  `src/pool/CMakeLists.txt`'s `arbc_component_test`; Catch2, matching the
  existing `t/*.t.cpp`):
  - **Page-aligned, size-classed allocation.** `allocate(N)` returns a
    `BlockRef` whose `data()` is `k_page`-aligned and whose `size()` == N,
    over a slot of stride `class_stride(N)`. Two allocations of the same size
    share one store; two allocations in different classes create two stores
    (`arena store_count` / distinct `slot_stride` observed). `allocate(page)`,
    `allocate(page+1)` (→ 2·page rung), and `allocate(1 MiB)` (exact rung,
    zero waste) pin the round-up.
  - **Fill / read-back byte fidelity.** Fill a blob through
    `BlockRef::bytes()`, take its `BlockSlotRef`, and read it back via
    `peek` and via `resolve()` — bytes are identical; `peek` returns exactly
    `size()` bytes.
  - **Refcount sharing and reclaim-to-free-pool.** `allocate` → `count == 1`;
    `retain` → 2; a second retained `BlockSlotRef` keeps the blob live while
    the first owner releases (`peek` still valid); the final `release` drops
    the count to 0 and returns the slot to its class free pool — the next
    same-size `allocate` reuses it with **no capacity growth**
    (`total_bytes_reserved` unchanged, `slots_live` back to baseline).
  - **Overflow is loud.** `retain` at `k_max_count` returns
    `RefError::CountOverflow` (mirror `refs.t.cpp`), never wraps.
  - **Durability fence interposition.** With a `ReleaseFence` installed on the
    class store, a zero-count `release` diverts to the fence and the blob
    stays resolvable until `free_now` returns it (the checkpoints quarantine
    works for blobs unchanged).
  - **Stale-ref generation trap (debug).** A `BlockSlotRef` to a slot recycled
    out from under it fails the generation check (assert-hook style, mirroring
    `refs.hpp:451-453`).
- **Claim (register + `enforces:`)**
  `15-memory-model#bulk-payloads-are-page-aligned-size-classed` — a bulk
  payload is allocated page-aligned from a power-of-two size class; same-size
  payloads share one store and distinct-class payloads get distinct stores.
  Registered in `tests/claims/registry.tsv`, witnessed by the alignment /
  size-class unit test.
- **Claim (register + `enforces:`)**
  `15-memory-model#bulk-payloads-shared-by-refcount` — a blob is shared across
  owners by reference count (every owner `peek`s identical bytes; no data-page
  write on retain/release) and is reclaimed to its class free pool when the
  last owner releases, the slot reused with no capacity growth. Registered in
  `tests/claims/registry.tsv`, witnessed by the sharing / reclaim unit test.
- **Behavioral-counter assertions** (doc 16 `:54-62`): a `blobs_allocated`
  counter advances by exactly the number of distinct `allocate` calls and
  **does not** advance on `retain`/`release`/`resolve`/`peek`; a
  churn-and-reuse burst (allocate → release-to-zero → allocate same size)
  leaves `total_bytes_reserved` flat after the first allocation (proving
  free-pool recycle, not fresh growth). No wall-clock assertion.
- **Concurrency (TSan + asan, explicit per doc 16 `:66-73`)**: a smoke test —
  the writer allocates and fills blobs while a second thread releases blobs
  cross-thread (driving some to zero, i.e. `SlotStore::release` off the
  writer) and N reader threads `peek` published blobs concurrently — runs
  clean under TSan and asan; published blob bytes are byte-stable under
  concurrent readers, every blob is accounted, and the arena returns to
  baseline. The full seeded/randomized schedule-perturbation stress belongs to
  `quality.stress_harness` (existing task) — scoped there, not duplicated
  here. (Restating the standing caveat noted by `pool.reclamation` /
  `pool.free_pools`: the repo has no per-push TSan CI *lane* yet; the smoke
  runs under the asan lane meanwhile, and wiring a TSan lane is a
  `.github/workflows/` infrastructure edit surfaced for the parking lot, not
  an agent-implementable WBS task.)
- **Coverage**: ≥90% diff coverage on changed lines; gate green including the
  asan lane (`scripts/gate`, `scripts/check_claims.py`).
- **No deferred WBS follow-up from this task.** The primitive is
  self-contained; its consumer, `kinds.raster_pool_backing`
  (`tasks/55-kinds.tji:32-37`), already exists and depends on this task. The
  Windows workspace-file path for durable big-block backing is already covered
  by `pool.mmap_backing_win32` / `pool.checkpoints_win32` (the pool takes any
  `ChunkSource`), so no new port task arises here.

## Decisions

- **Size-classed `SlotStore`s, not a bespoke variable-size allocator.** Each
  size class is one fixed-slot `SlotStore` (stride = class byte size), routed
  by the existing `Arena::store_for` map keyed by `(slot_size, slot_align)`.
  This inherits — for free — chunk-backed page-aligned storage, the inside-out
  count column, any-thread free-pool release, the durability release fence,
  and hole-punchable emptied chunks. *Rejected:* a true variable-extent
  allocator (buddy / best-fit free-list over a single arena). It reintroduces
  the fragmentation the whole memory model was designed to make structurally
  impossible (`15-memory-model.md:34-36`), is far more than 2d, and buys
  nothing for the concrete consumer, whose blobs are uniform. Doc 15's own
  word is "size-classed" — round-up to a class *is* the mandated design.
- **Power-of-two page-multiple ladder.** `class_stride = max(page,
  next_pow2(size))`. Makes 1 MiB (the default raster tile) an exact rung with
  zero waste, bounds the store count to O(log₂(max_blob/page)), and keeps
  every slot page-aligned by construction. *Rejected:* a finer geometric
  ladder (k rungs per octave) to cut the <2× worst-case internal waste — a
  tunable constant that is a later profiling decision, not a redesign; the
  pow2 ladder is exact for the only consumer today (no dead generality, doc
  16). *Rejected:* one exact-size store per distinct requested size —
  unbounded store proliferation for continuously-varying sizes (arbitrary
  decoded-frame dimensions); size-classing is what bounds it.
- **`BlockSlotRef` carries the logical size; no per-slot size column.** With
  `size` in the 8-byte in-record ref, the exact span and the owning store are
  both pure functions of the ref, so no inside-out size column (and its
  publish-in-lock-step bookkeeping) is needed. *Rejected:* a parallel size
  column à la reclaim-links — more machinery for data an 8-byte ref already
  carries cheaply; big-block refs are few (one per tile, next to a megabyte
  payload), so the 4 extra bytes are free.
- **No zero-count sink / reclaim queue / deferred drain — release is inline
  `SlotStore::release`.** Byte blobs run no destructor and hold no child
  references, so the whole `RefStore<T>` reclamation apparatus (immediate
  sink, reclaim-link Treiber stack, `DeferredReclaimSink`, `drain_pending`
  cascade) is unnecessary. `free_pools` already made `SlotStore::release`
  any-thread and allocation-free on the hot path, and `checkpoints` already
  wired the durability fence into it, so inline release on the writer/drain
  thread is correct and cheap. *Rejected:* routing blob release through a
  `RefStore<ByteCell>` — a fixed `T` cannot span multiple size classes, and
  it would drag in destructor machinery that has nothing to destroy.
- **Refcount directly over `SlotStore::count_ref`, reusing refs.hpp's checked
  arithmetic.** The count column is store-owned since `refcounts_in_store`, so
  a bare `SlotStore` (no typed view) can be counted; the retain/release CAS
  and underflow assert are copied from `refs.hpp` to keep one overflow
  discipline across the component. *Rejected:* refactoring `refs.hpp` to share
  the arithmetic through a common base — out of scope, risks the shipped refs
  path for a few lines' deduplication.
- **Dedicated `Arena`, `ChunkSource`-parameterized backing.** `BigBlockPool`
  owns its own `Arena` (doc 15's "dedicated" pool, a separate memory
  population from document records) and takes a `ChunkSource&` so the consumer
  picks anonymous vs workspace-file per blob population. *Rejected:* sharing
  the model's record arena — conflates two populations with very different
  size and churn profiles and defeats per-population accounting.
- **No design-doc delta.** Doc 15 already fully designs this allocator
  (`:20,:240-242`): page-aligned, size-classed, refcounted from the owning
  nodes, tile *table* in slabs. The design here is a faithful implementation
  of that text, not an amendment; the only non-test edits are the two new pool
  source files, which ride the closer's implementation commit (doc 16
  same-commit rule).

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-10.

- Added `src/pool/arbc/pool/big_block_pool.hpp` — `BigBlockPool`, `BlockRef` (owning transient handle), `BlockSlotRef` (compact in-record form, 8 bytes release / 12 bytes debug), and `class_stride` size-class ladder (power-of-two page-multiple round-up).
- Added `src/pool/big_block_pool.cpp` — implementation: size-class routing through `Arena::store_for`, retain/release with overflow-checked CAS and underflow assert mirroring `refs.hpp`, inline `SlotStore::release` as sole zero-count handler (no destructor, no cascade), per-class store cache (lock-free peek/retain/release any-thread), `blobs_allocated` behavioral counter.
- Added `src/pool/t/big_block_pool.t.cpp` — 8 Catch2 cases: page-aligned/size-classed allocation; `class_stride` ladder; fill/read-back fidelity; refcount sharing + reclaim-to-free-pool; `blobs_allocated` counter + flat `total_bytes_reserved` on reuse; loud overflow at ceiling; durability-fence interposition; stale-`BlockSlotRef` generation trap (debug); concurrent writer-alloc/cross-thread-release/reader-peek smoke.
- Edited `src/pool/CMakeLists.txt` — wired source, public header, and test into the pool component.
- Edited `tests/claims/registry.tsv` — registered claims `15-memory-model#bulk-payloads-are-page-aligned-size-classed` and `15-memory-model#bulk-payloads-shared-by-refcount`; both `enforces:`-tagged in the test file.
- All 8 new cases pass; full pool binary runs 77 cases / 107 656 assertions; clean build under both default and ASan/UBSan presets.
