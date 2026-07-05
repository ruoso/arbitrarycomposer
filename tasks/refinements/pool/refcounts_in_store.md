# pool.refcounts_in_store — Refcount tables in SlotStore

## TaskJuggler entry

`tasks/05-pool.tji` → `pool.refcounts_in_store` ("Refcount tables in SlotStore").

## Effort estimate

1d.

## Inherited dependencies

- `pool.refs` — **settled** (`3898878`): landed `Ref<T>`, `SlotRef<T>`,
  `RefStore<T>`, `ZeroCountSink`, `RefcountTableBacking`, and the inside-out
  refcount + debug generation tables that this task relocates
  (`src/pool/arbc/pool/refs.hpp`).
- `pool.arena_core` — **settled** (transitive, via `refs`): provides the
  untyped, size-class-keyed `SlotStore`, the `TypedStore<T>` veneer, the
  reusable two-level `SlabDirectory` table machinery, and 32-bit
  `SlotIndex` (`src/pool/arbc/pool/slot_store.hpp`,
  `src/pool/arbc/pool/typed_store.hpp`).
- `pool.reclamation` — **settled** (`c5b828e`): installed the
  per-`RefStore<T>` deferred-reclaim sink, the reclaim-link Treiber stack,
  and the type-erased drain thunk. This task must leave that machinery
  where it is (see Decisions) — it is type-specific and stays per typed view.

## What this task is

A pure ownership-relocation refactor inside `arbc::pool`. Today each
`RefStore<T>` owns its own refcount column (`d_refcounts`) and, in debug
builds, its own generation-tag column (`d_generations`), even though
`arena_core` already makes two record types that share a `(sizeof, alignof)`
size class resolve to **one** shared, untyped `SlotStore` (and therefore one
shared slot-index space and one shared free list). This task moves those two
parallel columns down into `SlotStore`, so that several `RefStore<T>` /
`RefStore<U>` typed views over one size-class store share a single count
column and a single generation column, indexed by **physical slot**, not by
the typed view. A physical slot then has exactly one logical reference count,
wherever it is viewed from. Everything else — the `SlotRef`/`Ref` API, the
4-byte in-record reference, the zero-count sink, the reclaim-link stack, the
per-store destructor dispatch — stays exactly where it is.

## Why it needs to be done

`model.persistent_state` (doc 14/15) writes `DocState` map nodes and object
records as fixed-size slab types, with node arity chosen so records "land in
a small number of size classes" (doc 15:225-227). That is the *point* of the
size-class design — distinct record types (composition / layer / content)
that happen to be the same size deliberately share one size-class store to
get free memory efficiency (`arena_core.md` Decisions). But with the
refcount column owned per-`RefStore<T>`, two typed views over one shared
`SlotStore` keep two duplicate count tables over the same physical index
space: memory is wasted (N× the count table for N types in a class), and,
worse, the debug generation tag — which must fault a stale `SlotRef<T>` after
its slot is recycled *as a different type* — is invisible across the views,
so a slot migrating from `T` to `U` would not fault a dangling `SlotRef<T>`.
Moving the columns into the store where slot identity actually lives closes
both gaps and is the stated prerequisite for multi-type-per-store sharing in
`model.persistent_state`.

## Inputs / context

- `docs/design/15-memory-model.md`:
  - lines 37-41 — the inside-out invariant this refactor must preserve:
    "refcounts live in *parallel* buffers, not next to the data. After
    construction, data pages are never written again".
  - lines 184-190 — "refcounts, free pools, generation tags, and the
    reclamation queue are anonymous runtime state, rebuilt on open"; the
    columns stay anonymous and unpersisted wherever they are owned.
  - lines 225-227 — the size-class constraint on `DocState` records, and
    **the delta this task lands** (see Decisions / design-doc delta): the
    clarification that the refcount + generation columns are size-class-store
    owned and physical-slot indexed, so typed views share one column.
- `docs/design/17-internal-components.md:36-49` — `arbc::pool` is L1,
  depends only on `arbc::base`; it owns the slab arenas, `arbc::ref`,
  reclamation, and generation tags. This task edits only `arbc::pool` and
  introduces no new levelization edge.
- `src/pool/arbc/pool/slot_store.hpp:76-171` — `SlotStore` (untyped,
  size-class-keyed via `Arena::store_for`, `slot_store.hpp:188,205`); today
  owns the `SlabDirectory<std::byte>` data directory, the LIFO free list,
  and per-store accounting, but **no** count or generation column. The new
  columns land here, grown in lock-step with data chunks.
- `src/pool/arbc/pool/refs.hpp`:
  - `refs.hpp:567-585` — `RefStore<T>`'s members: `d_refcounts` and (debug)
    `d_generations` are the two `SlabDirectory<std::atomic<uint32_t>>`
    columns this task relocates; `d_reclaim_links` / `d_reclaim_head` /
    `d_sink` **stay** (type-specific, per view).
  - `refs.hpp:497-518` — `ensure_parallel_chunks` (lock-step column growth)
    and `count_ref(SlotIndex)`: the growth + indexing logic that moves to /
    delegates through `SlotStore`.
  - `refs.hpp:555-562` — `generation_ref` / `assert_generation` (debug).
  - `refs.hpp:340-364, 546-552` — `retain` / `release_index` / `count`: the
    hot path; after the move these read/write the store-owned column via a
    `SlotStore` accessor but keep the identical CAS-overflow and
    zero-dispatch-to-sink behavior.
  - `refs.hpp:62-67, 261-277, 500-502, 585` — `RefcountTableBacking`, the
    live custom-allocation hook for count chunks (constructor param
    `refcount_backing`, member `d_refcount_backing`), consulted in
    `ensure_parallel_chunks`. This hook must move to `SlotStore` with the
    columns (see Decisions).
- `src/pool/t/bench_smoke.t.cpp:134,184` — the one live consumer of
  `RefcountTableBacking` (`MmapRefcountBacking` passed as
  `RefStore<BenchNode> store(arena, &backing)`); its call site changes to
  supply the backing at size-class-store creation.
- `src/pool/t/refs.t.cpp`, `src/pool/t/reclamation.t.cpp`,
  `src/pool/t/checkpoint.t.cpp`, `src/pool/t/crash_tests.t.cpp` — the
  existing behavioral witnesses that must stay green byte-for-byte through
  the refactor (single-type stores exercise the same column, now
  store-owned).
- Predecessor decision continuity: `refs.md` ("inside-out refcounts in
  parallel tables, 4-byte `SlotRef`, generation tags debug-only"),
  `arena_core.md` ("untyped `SlotStore` + thin `TypedStore<T>`; size-class
  sharing is free memory efficiency"), `reclamation.md` ("one store per size
  class; type recovered from the store pointer via a per-store drain
  thunk").

## Constraints / requirements

- **Inside-out invariant preserved.** The relocated columns remain
  `std::atomic<std::uint32_t>` `SlabDirectory`s, anonymous, never written to
  a data chunk, never persisted (doc 15:37-41, 184-190). Increment /
  decrement / generation-bump still touch only the parallel columns; the
  existing claim `15-memory-model#refcounts-outside-data-pages` (mprotect
  data pages read-only) must still hold and its test still pass.
- **One logical count per physical slot.** After the move, the count and
  (debug) generation for slot `i` in a size-class store are single-instance:
  every `RefStore<T>` / `RefStore<U>` over that store reads and writes the
  same atomic. `SlotRef` stays exactly 4 bytes in release
  (`static_assert` at `refs.hpp:595-597` unchanged); the API and semantics of
  `Ref<T>` / `SlotRef<T>` are unchanged.
- **Lock-step growth is a `SlotStore` invariant.** The count column (and
  debug generation column) grow with the data chunks, minted where slot
  chunks are minted (`SlotStore` allocation / chunk growth), so the columns
  can never be short of the data directory. Growth stays writer-only, as
  today.
- **Type-specific machinery stays in `RefStore<T>`.** The zero-count sink
  (`d_sink`), reclaim-link stack (`d_reclaim_links` / `d_reclaim_head`), and
  per-store drain thunk are keyed to `T` (they run `~T`); `SlotStore` is
  type-erased and cannot. `release_index` decrements the store-owned count
  and, on last count, dispatches to the per-view sink exactly as now
  (`refs.hpp:546-552`). The claims
  `15-memory-model#release-enqueues-never-destroys-inline` and
  `#deferred-cascade-reclaims-whole-subtree` must remain green.
- **`RefcountTableBacking` relocates, not retires.** The hook moves to
  `SlotStore` construction (the store owns the count column, so it owns the
  column's optional custom allocator); it becomes a size-class-store
  property supplied when the store is first created. Single-type stores (the
  only current backing consumer) are unaffected in behavior.
- **No new dependency, no new levelization edge** (doc 17): all edits are
  within `arbc::pool`; no doc-10 dependency policy is touched.
- **Checkpoint / mmap paths unchanged.** Counts and generation tags were
  already anonymous and rebuilt on open (doc 15:184-190); relocating their
  owner does not change recovery. The `RefStore<T>::restore` /
  `set_count(_index)` recovery entry points keep working through the store
  accessor.

## Acceptance criteria

- **Unit tests** (`src/pool/t/refcounts_in_store.t.cpp`, new, wired into
  `arbc_component_test`):
  - Two record types `T` and `U` with identical `(sizeof, alignof)` resolve
    to one `SlotStore`; a slot allocated as `T`, released, and recycled as
    `U` reuses the *same* physical count-column entry (assert the store's
    live/high-water accounting and that the second allocation returns the
    freed index with count reset to 1).
  - Cross-view single-count: retaining a `SlotRef` through `RefStore<T>` and
    reading the count through the shared store observes one column;
    duplicate per-view tables are gone (assert via the store accessor / a
    behavioral counter, not by reaching into removed members).
  - **Debug-only** generation-across-views test: a `SlotRef<T>` to slot `i`,
    then `i` released and recycled as `U`, faults on resolution of the stale
    `SlotRef<T>` (the generation bump made by the `U` view is visible to the
    `T` view) — via the existing `generation_matches` assert-hook predicate
    (`refs.hpp`), as a death/assert-hook test.
  - Regression: all `refs.t.cpp` / `reclamation.t.cpp` / `checkpoint.t.cpp`
    single-type witnesses pass unchanged; `sizeof(SlotRef)==4` static_assert
    still compiles.
- **Claim (register + `enforces:`)**
  `15-memory-model#one-count-column-per-size-class` — new row in
  `tests/claims/registry.tsv`: "Several typed views over one size-class slab
  store share a single per-slot refcount column (and one debug generation
  column); a slot recycled from one type to another reuses the same count
  entry, and its generation bump is visible to a stale reference of the
  original type." Enforced by the cross-view + generation-across-views tests
  above. This is the executable witness of the design-doc delta.
- **Claim (still green, not re-registered)**
  `15-memory-model#refcounts-outside-data-pages` — the existing mprotect
  test (data chunks read-only, arbitrary pin/unpin) must still pass with the
  column store-owned; re-point its `enforces:` test to the relocated
  accessor if the symbol moved, without weakening the assertion.
- **Concurrency (TSan, explicit per doc 16)** — smoke: two typed views over
  one size-class store, each pinning/unpinning its own disjoint slots
  concurrently, races-clean on the now-shared column (per-slot atomics;
  growth writer-only). The seeded / randomized cross-type churn stress
  belongs to `quality.stress_harness` (existing task) — scoped there, not
  duplicated here. Note: this repo has no TSan preset today (parked with the
  same status as `reclamation.md`/`checkpoints.md`); the smoke runs under the
  asan lane meanwhile.
- **Coverage** — ≥90% diff coverage on changed lines; gate green including
  asan (`scripts/gate`, `scripts/check_claims.py`).
- No deferred follow-up: this refactor is self-contained; its consumer
  (`model.persistent_state`) is an already-existing WBS leaf and needs no new
  task from here.

## Decisions

- **Move only the refcount + generation columns into `SlotStore`; keep the
  sink and reclaim-link stack in `RefStore<T>`.** A physical slot's count and
  generation are type-independent (`uint32` per slot) and must be
  single-instance per slot for correctness under size-class sharing, so they
  belong to the untyped store that owns slot identity. The zero-count sink
  and reclaim stack dispatch `~T`, which a type-erased `SlotStore` cannot run
  — they stay per typed view, exactly matching `reclamation.md`'s
  "type recovered from the store pointer via a per-store drain thunk; no
  per-slot type tag on the hot path." *Rejected:* hoisting the whole
  reclamation machinery into `SlotStore` — it would reintroduce a per-slot
  type tag (or a type-erased destructor pointer per slot) that `reclamation`
  deliberately avoided.
- **`SlotStore` grows the columns in lock-step with data chunks; `RefStore<T>`
  delegates `count_ref` / `generation_ref` through `d_typed.store()`.**
  Centralizing column growth where slot chunks are minted makes "the column
  can never be short of the data directory" a single-owner invariant, and
  matches `arena_core`'s framing that "the inside-out layout falls out of
  reuse, not duplication." *Rejected:* leaving `ensure_parallel_chunks` in
  `RefStore<T>` writing into `SlotStore`-owned directories — that splits the
  growth invariant across two owners and races when two typed views over one
  store grow the same column concurrently.
- **Relocate `RefcountTableBacking` to `SlotStore`, don't retire it.** The
  hook is live (`bench_smoke.t.cpp` `MmapRefcountBacking`), and now that the
  store owns the count column, the store owns the column's optional custom
  allocator — supplied at size-class-store creation. This keeps the mmap /
  benchmark placement seam working and makes "one backing per size class"
  fall out naturally. *Rejected:* keeping the backing on `RefStore<T>` — with
  two views over one store, only the first view's backing could apply, an
  incoherent per-column-with-two-owners state; and *rejected:* deleting it —
  it has a live consumer.
- **Design-doc delta in `docs/design/15-memory-model.md:225-227` (this
  commit).** Doc 15 as written implies strict type-segregation
  ("per-type `storage<T>`") and is silent on multiple typed views sharing one
  size-class store's count column — the very capability
  `model.persistent_state` depends on. The delta adds one clause pinning the
  invariant: the refcount + generation columns are size-class-store owned and
  physical-slot indexed, so typed views share one column and a slot has
  exactly one logical count. This preserves every existing doc-15 normative
  property (inside-out, anonymous, rebuilt-on-open) and needs **no doc-00
  bullet** — doc 00's "Memory model" decision ("refcounts stored apart from
  immutable data", `00-overview.md:152`) is unchanged; this is an
  implementation-ownership clarification, not a project-shaping reversal.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-04.

- `src/pool/arbc/pool/slot_store.hpp` — `RefcountTableBacking` relocated here; `SlotStore` now owns the refcount column and (debug) generation column with `count_ref`/`generation_ref` accessors and a `refcount_backing` ctor param; `Arena::store_for` threads the backing.
- `src/pool/slot_store.cpp` — lock-step column growth (`publish_parallel_columns`) in `allocate`/`reserve_restored`, column teardown, backing plumbing.
- `src/pool/arbc/pool/typed_store.hpp` — forwards the backing to `store_for`.
- `src/pool/arbc/pool/refs.hpp` — removed `RefcountTableBacking` + the `d_refcounts`/`d_generations`/`d_refcount_backing` members; `count_ref`/`generation_ref` now delegate to the shared store; `reclaim`'s gen-bump hits the store column; `ensure_parallel_chunks` publishes only the per-view reclaim-link table.
- `src/pool/t/refcounts_in_store.t.cpp` — new unit test file (4 cases: cross-type recycle reuses shared count entry, single per-slot count cell across views, debug-only cross-view generation fault, two-view concurrent race-clean smoke); wired into `arbc_component_test` via `src/pool/CMakeLists.txt`.
- `tests/claims/registry.tsv` — claim `15-memory-model#one-count-column-per-size-class` registered and enforced.
- `docs/design/15-memory-model.md` — design-doc delta (lines 225-227 area) pinning the invariant that refcount + generation columns are size-class-store owned and physical-slot indexed.
- `src/pool/bench/allocator_bench.cpp`, `src/pool/t/bench_smoke.t.cpp` — updated to supply `RefcountTableBacking` at size-class-store creation instead of at `RefStore<T>` construction.
