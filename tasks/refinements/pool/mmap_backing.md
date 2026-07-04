# pool.mmap_backing — mmap workspace backing

## TaskJuggler entry

`tasks/05-pool.tji` → `pool.mmap_backing` ("mmap workspace backing").

## Effort estimate

3d.

## Inherited dependencies

- `pool.arena_core` — pending at refinement time: provides the
  `ChunkSource` seam (acquire/release of page-aligned spans) this task
  implements, and the anonymous default it generalizes.

## What this task is

The two mmap-based `ChunkSource` implementations (design doc 15,
"File-backed arenas"): anonymous mappings, and the **per-document
workspace file** — header with layout schema + arena directory,
chunk-at-a-time file growth with stable mappings, hole-punch release of
emptied chunks, and the strict split where only immutable data chunks are
file-backed while refcounts, generation tags, and free lists stay
anonymous. Scope boundary: this task creates and grows *fresh* workspace
files; opening an existing file, root discovery, and bookkeeping rebuild
are `pool.checkpoints` (which owns the A/B root protocol this file layout
must leave room for).

## Why it needs to be done

Doc 15's four motivations: crash recovery (with checkpoints),
larger-than-RAM documents via clean-page eviction, real memory+disk
release via hole punch (fixing cpioo's grow-forever high-water mark), and
the `MAP_SHARED` path to out-of-process plugin isolation (doc 03). It is
the prerequisite of `pool.checkpoints`, which gates M1.

## Inputs / context

- `docs/design/15-memory-model.md` — "File-backed arenas: mmap instead of
  process memory" (all four numbered motivations, the inside-out/
  persistence split, position independence, the checkpoint interaction
  this task must not preclude).
- `docs/design/08-serialization.md` — the workspace-vs-interchange
  distinction (the workspace file is a same-machine session artifact;
  JSON remains the document format).
- `src/pool/` as landed by arena_core: `ChunkSource` interface, chunk
  sizing (~64 KiB target), directory machinery.

## Constraints / requirements

- `AnonymousChunks`: `mmap(MAP_PRIVATE|MAP_ANONYMOUS)` acquire,
  `munmap` release (replacing/backing whatever placeholder arena_core
  shipped); `MAP_NORESERVE` considered per doc 15's demand-paging framing.
- `WorkspaceFileChunks`:
  - File header: magic, format-major, page size, per-type slot sizes /
    arena directory, and **two root slots reserved for
    `pool.checkpoints`' A/B protocol** (written as zero now; the layout
    is this task's contract, the protocol is not).
  - Growth: `ftruncate` extends the file by whole chunks; each chunk gets
    its own `mmap(MAP_SHARED)` region — existing mappings never move
    (address stability), mirroring chunk-at-a-time directory growth.
  - Release: `fallocate(FALLOC_FL_PUNCH_HOLE|KEEP_SIZE)` returns storage;
    the chunk's mapping is dropped and its directory entry cleared.
  - Only data chunks go through this source. Bookkeeping tables
    (refcounts, generations, free lists) are constructed over
    `AnonymousChunks` unconditionally — enforce structurally (the store
    wires two sources), not by convention.
- **Position independence is asserted, not assumed**: records contain
  `SlotRef`s only (pool.refs); this task adds a debug check hook the
  model layer can use later, and documents the workspace file as
  native-endianness, same-machine, no portability promise (doc 15).
- POSIX implementation. **MSVC/Windows builds must stay green**: the
  file-backed source compiles out on non-POSIX platforms
  (`ARBC_HAS_WORKSPACE_FILES` capability macro + runtime query), with
  anonymous backing as the universal fallback. The Windows
  (`MapViewOfFile`) port is registered as tech-debt:
  `pool.mmap_backing_win32` (est. 2d), gated on M9 — deferred to
  `pool.mmap_backing_win32` (closer registers in WBS, wiring it into
  `m9_release`).
- Error handling: all syscall failures surface as `arbc::expected`
  errors with errno context; disk-full during growth is an error the
  caller sees, never an abort (doc 15/16 crash-test scope note: the full
  kill-injection sweep is `pool.crash_tests`).

## Acceptance criteria

- Unit tests: fresh workspace create/grow/release lifecycle; chunk
  contents survive remap-free growth (address stability with file
  backing); bookkeeping-stays-anonymous verified structurally (the file
  never grows when only refcount traffic occurs); header round-trip
  (write, reopen read-only, validate fields — content rebuild explicitly
  out of scope).
- Claim (register + `enforces:`): `15-memory-model#hole-punch-returns-storage`
  — after releasing chunks, the file's allocated block count
  (`st_blocks`) drops accordingly (Linux-only test, guarded).
- Disk-full path exercised via a size-capped tmpfs or `RLIMIT_FSIZE`
  guard, asserting the `expected` error (not death).
- Gate green including asan (asan + MAP_SHARED interplay verified).

## Decisions

- **Chunk-per-mapping rather than one big remappable mapping**: growth
  never invalidates existing addresses, matching the directory design;
  the cost (more vmas) is bounded by chunk size choice. Rejected:
  `mremap`-based growth (address instability breaks pinned readers) and
  reserving a giant PROT_NONE region up front (portability + address
  space accounting on 32-bit-index scale is unnecessary).
- **Header owns the root slots now, protocol later**: reserving the A/B
  root area in this task's layout means `pool.checkpoints` changes no
  on-disk layout, only behavior — layout-major bumps are the expensive
  kind (doc 08 discipline applied to the workspace).
- **Capability macro + runtime query for platform coverage** instead of
  stubbing file backing on Windows with anonymous memory silently:
  callers (Document construction policy, doc 15) must be able to tell the
  user recovery is unavailable rather than believing it exists. The
  honest gap is registered debt, not a stub.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-04.

- Created `src/pool/arbc/pool/workspace_file.hpp`: `WorkspaceFileChunkSource` — chunk-per-`MAP_SHARED`-mapping growth, `fallocate` hole-punch release, file header with zeroed A/B root slots for `pool.checkpoints`.
- Created `src/pool/workspace_file.cpp`: POSIX implementation; `ARBC_HAS_WORKSPACE_FILES` compiles it out on non-POSIX, with anonymous backing as the universal fallback.
- Created `src/pool/t/workspace_file.t.cpp`: 17 unit tests covering create/grow/release lifecycle, address stability under growth, header round-trip + bad-magic reject, disk-full via `RLIMIT_FSIZE` → `expected` error (not death), free-list traffic never grows file (bookkeeping stays anonymous), position-independence debug hook.
- Edited `src/pool/slot_store.cpp`: replaced placeholder `AnonymousChunkSource` with real `mmap(MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE)` / `munmap`.
- Edited `src/pool/CMakeLists.txt`: wired new source files into build; `src/pool/arbc/pool/slot_store.hpp`, `src/pool/arbc/pool/typed_store.hpp` minor adjustments.
- Edited `tests/claims/registry.tsv`: registered + enforced claim `15-memory-model#hole-punch-returns-storage` (Linux-guarded `st_blocks` drop after hole-punch).
- Tech-debt registered: `pool.mmap_backing_win32` (`MapViewOfFile` Windows port, est. 2d), wired into `m9_release`.
