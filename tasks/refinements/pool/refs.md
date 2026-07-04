# pool.refs — Counted references

## TaskJuggler entry

`tasks/05-pool.tji` → `pool.refs` ("Counted references").

## Effort estimate

2d.

## Inherited dependencies

- `pool.arena_core` — pending at refinement time: provides `Arena`,
  `SlotStore`/`TypedStore<T>`, the reusable two-level table machinery
  (parameterized over element payload — the seam this task instantiates
  parallel tables through), 32-bit slot indices, and `arbc::expected` in
  `base`.

## What this task is

The ownership layer over the arenas (design doc 15): atomically counted
references whose counts live in **parallel tables, not next to the data**
(the inside-out property), in two forms — a pointer-carrying transient
reference for stack use and a 4-byte index-only reference for in-record
use — plus generation tags in debug builds so stale references fault
loudly instead of silently reading recycled slots.

## Why it needs to be done

Pins, journal entries, and record-to-record edges (docs 14/15) are all
reference-counted reachability; `pool.reclamation` needs the
zero-count-detection seam this task provides; `model.persistent_state`
stores index-only refs inside records (mandatory for the mmapped
workspace, where data pages have no stable virtual address across runs).

## Inputs / context

- `docs/design/15-memory-model.md` — inside-out refcounts, the caveat
  list this task fixes (uint32 + overflow check, assignable references,
  index-only in-record form, generation tags), and the thread rules
  (pin/unpin allowed from render/audio-engine threads).
- `../poc-inside-out-objects/src/cpioo/managed_entity.hpp:25-67` — the
  `reference` class being superseded: keeps the parallel-buffer idea;
  fixes deleted assignment, `short` counts, and the pointer+index size.
- `src/pool/` as landed by `pool.arena_core` (table machinery, stores).

## Constraints / requirements

- Refcounts are `std::atomic<std::uint32_t>` in a parallel table indexed
  identically to slots; increment/decrement **never write a data chunk**
  — data pages stay clean for shared/readonly mappings (docs 15/17
  isolation path).
- Overflow-checked: hitting the max count is a loud error (`expected`
  path on pin; debug assert), never a silent wrap.
- Two reference types, one ownership model:
  - `Ref<T>` — pointer + index, copy/move/assign all supported (fixing
    the cpioo deletion), for stack/API use; deref is one indirection.
  - `SlotRef<T>` (name at implementation) — 4-byte index-only,
    position-independent, trivially copyable, standard-layout: the ONLY
    reference form allowed inside records (doc 15's mmap requirement).
    Resolving a `SlotRef` through its store yields a `Ref<T>`.
  - Conversions are explicit; a `SlotRef` does not own (records are owned
    via the count the containing record's lifetime holds — the
    convention: whoever stores a `SlotRef` holds a count until release).
- **Traversal convention documented on the type**: pass `const Ref<T>&`,
  never copy in loops — reads do zero refcount traffic (the cpioo
  benchmark lesson; doc 15).
- Zero-count handling is a **sink interface**, not hardwired freeing:
  `on_zero(index)` dispatches to a per-store sink. This task ships an
  immediate sink (run destructor, release slot) for tests;
  `pool.reclamation` replaces it with the deferred queue. Destructor
  invocation lives in the sink, honoring arena_core's
  release-does-not-destroy contract.
- **Generation tags (debug builds)**: parallel `uint32_t` table; slot
  release bumps the generation; `Ref`/`SlotRef` carry the expected
  generation in debug builds and resolution asserts a match. Zero
  overhead in release builds (`#ifdef`-gated members — the size of
  `SlotRef` in release is exactly 4 bytes, static_asserted).
- Thread rules: pin/unpin (count ops) from any thread; resolution from
  any thread; allocation still writer-only (arena_core).

## Acceptance criteria

- Unit tests: copy/move/assign semantics; count lifecycle to zero-sink;
  SlotRef round-trip through records; overflow error path; release-build
  `sizeof(SlotRef) == 4` static_assert compiles.
- Generation-tag test (debug config): resolving a stale `SlotRef` after
  slot recycling trips the assert (death test or assert-hook).
- Claim (register + `enforces:`): `15-memory-model#refcounts-outside-data-pages`
  — with data chunks `mprotect`ed read-only, arbitrary pin/unpin traffic
  proceeds without faulting (Linux-only test, guarded; the strongest
  executable witness of the inside-out property).
- Concurrent pin/unpin smoke under the asan lane (full seeded stress is
  `quality.stress_harness`).
- Gate green including asan.

## Decisions

- **Sink-based zero handling** rather than direct free-on-zero: the
  reclamation design (doc 15: "release enqueues, never destroys inline")
  needs the seam, and tests get deterministic immediate reclamation
  through the same interface. Rejected: policy template parameter (ABI
  surface, no runtime swap for tests).
- **Generation tags debug-only** per doc 15's debug-discipline framing.
  Rejected: always-on tags (4 bytes/slot of release overhead for a bug
  class the debug lane + asan catch; revisit only with field evidence).
- **`SlotRef` non-owning with holder-holds-the-count convention**, matching
  how persistent records share substructure (doc 14: the DocState node
  that stores the edge owns the count; path-copying transfers it).
  Rejected: owning smart `SlotRef` (would need store access in its
  destructor — records must stay trivially destructible so reclamation
  can walk them explicitly).

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-04.

- `src/pool/arbc/pool/refs.hpp` (new) — `Ref<T>`, `SlotRef<T>`, `RefStore<T>`, `ZeroCountSink`, `RefError`; inside-out refcounts in anonymous parallel tables, 4-byte position-independent `SlotRef`, overflow-checked pin, debug generation tags.
- `src/pool/t/refs.t.cpp` (new) — unit tests: copy/move/assign + count lifecycle to zero-sink, sink-defers-reclaim seam, `SlotRef` record round-trip, retain/resolve overflow error path, `sizeof(SlotRef)==4` static_assert; debug-only stale-`SlotRef` generation-tag test via `generation_matches` assert-hook predicate; concurrent pin/unpin smoke.
- `src/pool/CMakeLists.txt` — registered `refs.hpp` and `t/refs.t.cpp`.
- `tests/claims/registry.tsv` — registered claim `15-memory-model#refcounts-outside-data-pages` (mprotect data pages read-only; `__linux__`-guarded).
- `src/pool/arbc/pool/workspace_file.hpp`, `src/pool/workspace_file.cpp`, `src/pool/t/workspace_file.t.cpp` — clang-format normalization from gate run.
