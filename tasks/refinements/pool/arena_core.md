# pool.arena_core — Typed slab arenas

## TaskJuggler entry

`tasks/05-pool.tji` → `pool.arena_core` ("Typed slab arenas").

## Effort estimate

3d.

## Inherited dependencies

- `bootstrap.walking_skeleton` — settled (commit `42dbd22`): the component
  machinery (`cmake/ArbcComponent.cmake`), the gate, and `arbc::base`
  exist. `src/pool/` is a new component; the `check_levels.py` table
  already admits `pool: {base}`.

## What this task is

The core of the `arbc::pool` component (design doc 15): **instance-owned
slab arenas** — per-size-class fixed-slot storage in two-level chunk
directories with monotonic growth, stable slot addresses for the life of
the arena, writer-thread allocation, in-place slot reuse, and per-arena
accounting. This task delivers raw slot storage and the typed wrapper;
counted references (`pool.refs`), deferred reclamation
(`pool.reclamation`), and mmap backing (`pool.mmap_backing`) build on it
and are explicitly out of scope here.

## Why it needs to be done

Doc 15 makes the arena the foundation of the versioned model: DocState
map nodes, object records, and content state nodes all live in slabs
(`model.persistent_state` is the direct consumer). Every stream above
`model` in the doc 17 levelization transitively depends on this task —
it is the deepest incomplete leaf in the plan, gating M1 through M9.

## Inputs / context

- `docs/design/15-memory-model.md` — the governing doc: "Memory
  populations", "Evaluation: poc-inside-out-objects", and the reclamation
  preview (whose contract this task must leave room for).
- `docs/design/17-internal-components.md` — component table (`pool`,
  level 1, depends `base`); repo layout and FILE_SET conventions.
- `../poc-inside-out-objects/src/cpioo/managed_entity.hpp` — the
  reference prototype. Adopt: two-level tables, fixed slots, perfect-hole
  reuse. Deliberately NOT adopted (doc 15 caveats): `inline static`
  storage (we build instance arenas), missing destructor invocation
  (release contract below), `short` refcounts (refs task), abort-on-OOM
  (errors as values).
- `src/base/arbc/base/ids.hpp` — `ObjectId` (unrelated to slot indices;
  do not conflate: slot indices are storage addresses, ObjectIds are
  document identities).
- `cmake/ArbcComponent.cmake:24` — `arbc_add_component` usage;
  `src/media/CMakeLists.txt` is the minimal example to copy.

## Constraints / requirements

- Levelization: `src/pool` DEPENDS `base` only (CI-enforced).
- **Address stability**: a slot's address never changes after allocation —
  growth appends chunks, never reallocates. This is what makes lock-free
  pinned reads possible (docs 14/15).
- **Concurrent index resolution**: worker threads resolve (store, index) →
  pointer while the writer grows the directory. The directory structure
  must make that race-free without reader locks.
- **Allocation is writer-thread-only** in this task (doc 15's thread
  rules); enforce with a debug-build thread assert. Thread-local free
  pools and cross-thread release arrive with `pool.reclamation`.
- **Release does not destroy**: `release(index)` marks the slot reusable;
  running (or deferring) the destructor is the *caller's* obligation —
  this is the seam `pool.reclamation`'s deferred queue plugs into, and it
  must be documented on the API. (This is the deliberate inversion of the
  cpioo gap: destruction becomes explicit rather than silently absent.)
- **Backing seam**: chunks come from a `ChunkSource` interface (acquire /
  release of page-aligned spans) with an anonymous-memory default, so
  `pool.mmap_backing` swaps in file backing without touching store logic.
- **Reusable table machinery**: the two-level directory is parameterized
  over element payload so `pool.refs` can instantiate *parallel* tables
  (refcounts, generation tags) with the same indexing — the inside-out
  layout falls out of reuse, not duplication.
- Errors as values (doc 10): allocation failure returns an error, never
  throws, never aborts. This task introduces the minimal
  `arbc::expected<T, E>` in `base` (doc 10 planned it; first consumer
  decides the shape) with its own unit tests.
- 32-bit slot indices; chunk capacity 2^k slots (k per store, default
  from slot size targeting ~64 KiB chunks); slots aligned to
  `alignof(std::max_align_t)` minimum, per-store override.
- Standard-layout-friendly: no per-slot headers in the data pages (tags
  and counts live in parallel tables later) — data pages stay truly
  immutable after construction (doc 15's inside-out property).

## Acceptance criteria

- Unit tests (`src/pool/t/`): distinct allocations yield distinct slots;
  release→allocate reuses the released slot; accounting (bytes reserved,
  slots live/capacity per store, arena aggregate) tracks alloc/release
  exactly; multiple stores of different slot sizes coexist in one arena;
  `expected` error path on a `ChunkSource` that refuses to grow.
- Claims (register + `enforces:` tags — doc 16):
  - `15-memory-model#slots-recycle-in-place` — a freed slot is the next
    same-class allocation's perfect hole (the fragmentation-impossibility
    witness).
  - `15-memory-model#chunk-growth-preserves-addresses` — pointers taken
    before growth remain valid and unchanged across arbitrary growth.
- A two-thread test: workers resolve indices→pointers concurrently with
  writer growth (TSan job covers it via the asan/tsan CI lanes; the
  seeded stress harness proper is `quality.stress_harness`).
- Gate green including the ASan preset; new public headers under
  `src/pool/arbc/pool/` via FILE_SET; component registered in
  `src/CMakeLists.txt` between `base` and `media`.

## Decisions

- **Instance arenas, not statics.** `Arena` is an object owning its
  stores; documents own arenas (doc 15 explicitly: accounting, teardown,
  multi-document hosts, plugin dlopen lifetimes). Rejected: cpioo's
  `inline static` per-type storage — no per-document ownership, no
  teardown, static-init hazards.
- **Untyped `SlotStore` + thin `TypedStore<T>` wrapper.** The arena
  manages size-class stores; `TypedStore<T>` is a header-only veneer
  (placement-new on `allocate`, typed pointer resolution) mapping `T` to
  its size class. Rejected: fully templated storage per T (cpioo style) —
  template bloat across components, awkward for the future C ABI, and
  size-class sharing between same-sized record types is free memory
  efficiency.
- **Directory shape: fixed root, on-demand second level.** A fixed root
  array of 2^10 second-level-table pointers (atomic), each second-level
  table 2^10 chunk pointers (atomic), chunk = 2^12 slots max → 32-bit
  index = root(10) | table(10) | slot-in-chunk(12) at the default chunk
  size. Readers do two acquire-loads and an add — no locks, no RCU, no
  reallocation ever. Rejected: cpioo's flat superbuffer sized
  `max_index >> chunk_bits` (32 MiB of pointer array *per store* when
  instance-owned); rejected: growable directory with republication
  (readers would need epoch protection — machinery doc 15 deliberately
  avoids).
- **`release()` without destruction** (see Constraints) — the deferred
  reclamation queue (`pool.reclamation`) is the designed place where
  destructors run; baking destruction into the store would force it
  inline on whatever thread drops the last reference, exactly what
  doc 15 forbids.
- **`arbc::expected` lands in `base` now**, minimal (value-or-error,
  `and_then`-free until needed): first consumer defines the need; doc 10
  already committed to carrying it. Rejected: `std::optional` + error
  getter (stateful, races), exceptions (banned across public API).

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-04.

- `arbc::expected<T,E>` (minimal value-or-error) added to `base`: `src/base/arbc/base/expected.hpp` with unit tests at `src/base/t/expected.t.cpp`; `src/base/CMakeLists.txt` updated.
- `arbc::pool` component created at `src/pool/` and registered in `src/CMakeLists.txt` between `base` and `media`.
- Public headers under `src/pool/arbc/pool/`: `chunk_source.hpp`, `slab_directory.hpp`, `slot_store.hpp`, `typed_store.hpp`; implementation at `src/pool/slot_store.cpp`; build wiring in `src/pool/CMakeLists.txt`.
- Unit tests at `src/pool/t/pool.t.cpp`: distinct slots, accounting, multi-store coexistence, `expected` error path on refusing `ChunkSource`, `TypedStore` round-trip, concurrent resolve-during-growth (TSan covered by asan/ubsan gate).
- Claims registered in `tests/claims/registry.tsv`: `15-memory-model#slots-recycle-in-place` and `15-memory-model#chunk-growth-preserves-addresses` (both with `enforces:` tags).
- Gate green under dev and ASan/UBSan presets (driver-verified).
