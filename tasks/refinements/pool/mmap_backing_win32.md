# mmap_backing_win32 — Windows `MapViewOfFile` backing for the workspace file

## TaskJuggler entry

Back-link: [`tasks/05-pool.tji:57-62`](../../05-pool.tji), under the `pool`
umbrella:

> ```
> task mmap_backing_win32 "mmap workspace backing — Windows port" {
>   effort 2d
>   allocate team
>   depends !mmap_backing
>   note "MapViewOfFile port of WorkspaceFileChunkSource for MSVC/Windows builds; enables ARBC_HAS_WORKSPACE_FILES on non-POSIX. Source-of-debt: tasks/refinements/pool/mmap_backing.md. Doc 15."
> }
> ```

This task feeds milestone **M9 — "v0.1 release"** (`m9_release`,
`tasks/99-milestones.tji:69-73`), whose `depends` list already names
`pool.mmap_backing_win32` (`tasks/99-milestones.tji:71`), alongside the
sibling Windows-parity leaves `pool.checkpoints_win32`,
`pool.crash_tests_win32`, and `runtime.plugin_loading_win32`. Two pool leaves
**depend on this one**:
`pool.checkpoints_win32` (`tasks/05-pool.tji:70-75`, `depends !checkpoints,
!mmap_backing_win32`) and — transitively — `pool.crash_tests_win32`. The
closer lands `complete 100` after the `allocate team` line, appends
`Refinement: tasks/refinements/pool/mmap_backing_win32.md` to the note, and —
if every other `m9_release` dependency is complete — propagates `complete 100`
to the milestone (milestones don't infer completion,
`tasks/refinements/README.md:69-72`).

## Effort estimate

**2 days** (`tasks/05-pool.tji:58`).

The POSIX predecessor (`pool.mmap_backing`, DONE 2026-07-04) already built the
entire design: the `WorkspaceFileChunkSource` class, the header/directory
layout, the chunk-per-mapping growth rhythm, the eager/fenced hole-punch
release, the A/B root-slot reservation, the errors-as-values taxonomy, and —
decisively — the two seams that localize every platform syscall:

- **The five `io_*` shims** (`workspace_file.hpp:264-268`:
  `io_ftruncate`/`io_mmap`/`io_fallocate`/`io_msync`/`io_mprotect`), already
  the single funnel through which every syscall routes (each consults
  `d_injector` then calls the real `::` primitive,
  `workspace_file.cpp:75-149`).
- **The `#else // !ARBC_HAS_WORKSPACE_FILES` stub arm**
  (`workspace_file.cpp:495-542`), the exact block a Windows backend supplants.

So the divergence is bounded to the syscall leaves plus a handful of
file-open/read/size primitives in the three static factories; the
platform-neutral orchestration (acquire's grow→map→record sequence, release's
fence dispatch, the directory bookkeeping, `d_live` tracking, the injector
dispatch, the address-stability contract) stays byte-for-byte shared. 2d
covers the leaf mappings, the allocation-granularity alignment wrinkle, the
sparse-file hole-punch, and porting `workspace_file.t.cpp` to the `msvc-debug`
lane.

## Inherited dependencies

**Settled:**

- **`pool.mmap_backing`** (DONE 2026-07-04, this task's declared
  `depends !mmap_backing` edge, `tasks/refinements/pool/mmap_backing.md`). It
  produced every seam this task extends:
  - `src/pool/arbc/pool/workspace_file.hpp` — the platform-agnostic public
    API. `WorkspaceFileChunkSource` (`:180-302`); the capability macro
    (`:20-24`, currently `0` on non-POSIX) and its runtime companion
    `workspace_files_supported()` (`:31`); the on-disk `WorkspaceHeader`
    (`:59-72`, with `page_size`/`data_offset` and the two zeroed A/B root
    slots) and `WorkspaceChunkEntry` (`:77-82`, `offset`/`size`/`state`
    recorded explicitly per chunk); the five private `io_*` shims (`:264-268`);
    the `SyscallInjector`/`WorkspaceSyscall` fault-injection seam (`:123-163`)
    and the `ChunkReleaseFence` deferred-punch seam (`:110-117`); the
    `WorkspaceFileErrc` taxonomy (`:36-45`).
  - `src/pool/workspace_file.cpp` — the POSIX implementation (`#if
    ARBC_HAS_WORKSPACE_FILES`, `:58-493`) and the `#else` `Unsupported` stub
    arm (`:495-542`) this task replaces on Windows. The five shims'
    real-syscall bodies (`:75-149`); `punch_now`'s Linux-`fallocate` hole-punch
    (`:294-309`); the `create`/`read_header`/`open` static factories'
    `open(2)`/`pread`/`fstat`/`sysconf`/`close` leaf calls.
  - `src/pool/t/workspace_file.t.cpp` — the 17-test lifecycle suite (create/
    grow/release, address stability, header round-trip + bad-magic reject,
    disk-full → `expected`, bookkeeping-stays-anonymous, position-independence
    hook) that this task must make **build and pass on Windows**; its main body
    is gated `#if ARBC_HAS_WORKSPACE_FILES` (`:27-275`) with a Linux-only
    hole-punch section (`:195`) and a runtime-honesty check in the `#else`
    (`:17-24`).
  - Claims `15-memory-model#hole-punch-returns-storage`
    (`tests/claims/registry.tsv:50`) and
    `15-memory-model#workspace-io-faults-surface-as-values` (`:57`), plus
    `#chunk-growth-preserves-addresses` (`:49`) — re-enforced on Windows by the
    same lifecycle assertions.
  - Its **Constraint** (`tasks/refinements/pool/mmap_backing.md:73-80`) and
    **Decision 3** (`:113-117`) specified this leaf verbatim: "the file-backed
    source compiles out on non-POSIX … The Windows (`MapViewOfFile`) port is
    registered as tech-debt: `pool.mmap_backing_win32` (est. 2d)."
- **The Windows CI lane already exists** — `msvc-debug` on `windows-latest`
  with the `win-dev` preset (`.github/workflows/ci.yml:60`;
  `CMakePresets.json:70-75,94,105`), running `cmake --preset win-dev` →
  `--build` → `ctest --preset win-dev`. As the sibling
  `runtime.plugin_loading_win32` established (its Decision 2), this makes the
  task **CI-testable, not compile-only**: the deliverable is the real
  workspace lifecycle suite running green on Windows.
- **`AnonymousChunkSource`** (`chunk_source.hpp:42-46`,
  `slot_store.cpp:48-81`) already degrades cleanly on non-POSIX (heap
  `operator new` fallback under `#else`), so the store keeps working
  regardless; this task upgrades Windows from *anonymous-only* to *file-backed
  available*.

**Pending:** none. `pool.checkpoints_win32` (`FlushViewOfFile`/`VirtualProtect`
protocol validation) and `pool.crash_tests_win32` (the kill-injection sweep on
Windows) both **depend on this task**, not the reverse — this leaf carries the
data lifecycle, they carry the durability protocol and crash sweep (see
Decision 5 for the boundary).

## What this task is

Flip `ARBC_HAS_WORKSPACE_FILES` to `1` on Windows and supply a real
`MapViewOfFile`/`CreateFileMapping` backend for `WorkspaceFileChunkSource`, so
the file-backed workspace **lifecycle** — create a fresh file, grow it
chunk-at-a-time with stable addresses, release+hole-punch emptied chunks,
reopen read-only (`read_header`) or read-write (`open`) — works on MSVC/Windows
exactly as it does on POSIX, and `src/pool/t/workspace_file.t.cpp` runs green on
the `msvc-debug` CI lane. Concretely:

- **(a) Capability macro.** Add a Windows arm to `workspace_file.hpp:20-24` so
  `ARBC_HAS_WORKSPACE_FILES` is `1` on `_WIN32` (and `workspace_files_supported()`
  reports `true`). This is the switch that makes the store wire file backing and
  compiles the real implementation into the translation unit on Windows.
- **(b) The five `io_*` syscall shims** get Windows bodies, each preserving the
  injector dispatch (consult `d_injector->before(kind, …)` → real call →
  `after(…)`, one branch, matching the POSIX shims at `workspace_file.cpp:75-149`)
  so the `SyscallInjector`/`WorkspaceSyscall` seam stays live for
  `pool.crash_tests_win32`:
  - `io_ftruncate` → extend the file's logical size (`SetEndOfFile` after
    `SetFilePointerEx`, or the `CreateFileMapping` max-size bump of the grow
    step) — the `WorkspaceSyscall::Ftruncate` injection point.
  - `io_mmap` → `CreateFileMapping` (sized to the new file end) + `MapViewOfFile`
    of the new chunk at its granularity-aligned offset — the
    `WorkspaceSyscall::Mmap` injection point.
  - `io_fallocate` → `DeviceIoControl(FSCTL_SET_ZERO_DATA, FILE_ZERO_DATA_INFORMATION)`
    on the sparse file — the `WorkspaceSyscall::Fallocate` injection point (the
    hole-punch).
  - `io_msync` → `FlushViewOfFile` + `FlushFileBuffers` — the
    `WorkspaceSyscall::Msync` injection point. **Mechanical body only** here;
    its checkpoint-ordering correctness is `pool.checkpoints_win32` (Decision 5).
  - `io_mprotect` → `VirtualProtect` — the `WorkspaceSyscall::Mprotect`
    injection point. Mechanical body only; debug read-only-publish semantics are
    `pool.checkpoints_win32`.
- **(c) `acquire` growth** (`workspace_file.cpp:221-272`) keeps its structure —
  directory-full guard → round up → extend → map the new chunk → write the
  directory entry → bump counters → track in `d_live` — with the round-up and
  offset stride computed against the **allocation-granularity** quantum on
  Windows, not the page (Decision 2), and growth realized by recreating the
  `CreateFileMapping` object each grow (Decision 3).
- **(d) `release`/dtor** (`workspace_file.cpp:207-217,281`) drop the mapping via
  `UnmapViewOfFile` (and `CloseHandle` the mapping/file handles at teardown) in
  place of `munmap`/`close`; the eager-vs-fenced dispatch stays platform-neutral.
- **(e) `punch_now`** (`workspace_file.cpp:294-309`) hole-punches the released
  range via `io_fallocate` → `FSCTL_SET_ZERO_DATA` and clears the directory
  entry; on a non-sparse-capable volume the punch degrades to directory-clear
  only (memory already reclaimed), mirroring POSIX-on-macOS (Decision 4).
- **(f) The static factories** `create`/`read_header`/`open`
  (`workspace_file.cpp:151-176,311-...`) get Windows leaf spellings:
  `CreateFileA`/`CreateFileW` in place of `open(2)` (with `FSCTL_SET_SPARSE` at
  create), `ReadFile` with an offset in place of `pread`, `GetFileSizeEx` in
  place of `fstat`'s short-file guard, `GetSystemInfo` (`dwPageSize`,
  `dwAllocationGranularity`) in place of `sysconf(_SC_PAGESIZE)`.
- **(g) Private state types.** A platform file-handle typedef for `d_fd`
  (`int` on POSIX, `HANDLE`/`void*` on Windows) plus a Windows-only
  file-mapping `HANDLE` member — **private only; the public API is unchanged**
  (Decision 6).
- **(h) Includes / test env.** `#include <windows.h>` behind the seam; the test
  suite's Linux-only hole-punch section (`workspace_file.t.cpp:195`) gains a
  Windows branch measuring on-disk allocation via `GetCompressedFileSize`, and
  the disk-full assertion moves to the `SyscallInjector` on Windows (no
  `RLIMIT_FSIZE`, Decision 4).

Everything above the leaves — the header/directory layout, the A/B root-slot
reservation, the `WorkspaceChunkEntry` bookkeeping, the `d_reopened` recovery
queue, the fence and injector dispatch, and every value type — is **unchanged**.
That is what "MapViewOfFile port … mirrors POSIX" means structurally: only the
platform leaves differ, so the on-disk layout and the errors-as-values mapping
are single-sourced and cannot drift between platforms.

**Not this task:**

- **Checkpoint-protocol correctness on Windows** — the ordered commit (`msync`
  data → flip A/B root → `msync` header, doc 15:210-213), the debug read-only
  publish, and the durability-epoch quarantine, proven on Windows: that is
  `pool.checkpoints_win32` (`tasks/05-pool.tji:70-75`), which `depends
  !mmap_backing_win32`. This task authors the *mechanical* `io_msync`/`io_mprotect`
  Windows bodies (the linker requires them once the macro is `1`, Decision 5)
  but validates only the data lifecycle, not the protocol.
- **The Windows crash-recovery sweep** — kill-at-every-syscall via the injector
  on Windows: `pool.crash_tests_win32`.
- **Any change to the workspace file format or the backing policy** — the layout
  is the predecessor's contract (doc 15); this task implements a second platform,
  it amends nothing (Decision 7).

## Why it needs to be done

On Windows today `ARBC_HAS_WORKSPACE_FILES` is `0`
(`workspace_file.hpp:20-24`), so `WorkspaceFileChunkSource` compiles to the
`Unsupported` stub arm (`workspace_file.cpp:495-542`) and every document falls
back to anonymous backing. That means **no crash recovery, no real
memory+disk release via hole-punch, and no larger-than-RAM documents** on
Windows — the four motivations doc 15 gives for file-backed arenas
(`docs/design/15-memory-model.md:161-180`) are POSIX-only. M9's headline is a
v0.1 release with Windows parity; this leaf is the one that makes the
file-backed workspace real on Windows, and it is the **prerequisite** of the two
downstream Windows pool leaves (`pool.checkpoints_win32`,
`pool.crash_tests_win32`) that finish the durability story — both `depend
!mmap_backing_win32`.

## Inputs / context

### Design docs (normative, doc 16)

- **doc 15 — Memory model** (`docs/design/15-memory-model.md`),
  "File-backed arenas: mmap instead of process memory" (`:161-226`):
  - `:163-167` — backing is a construction-time arena policy (`anonymous` |
    file-backed); file-backed is the default for document arenas. This task
    makes the file-backed choice *available* on Windows rather than silently
    unavailable.
  - `:177-180` — "file-backed chunks can be hole-punched (`fallocate(PUNCH_HOLE)`
    / `madvise`) when reclamation empties them, returning memory *and* disk."
    `FSCTL_SET_ZERO_DATA` on a sparse Windows file is the same-behavior
    realization (Decision 4).
  - `:181` — `MAP_SHARED`; `MapViewOfFile` of a `CreateFileMapping(PAGE_READWRITE)`
    is the Windows equivalent (shared, file-backed).
  - `:204-206` — "workspace files are same-machine artifacts (native endianness
    and padding, no portability promise)." This clause is what *permits* the
    Windows file to use the coarser allocation-granularity offset alignment
    (Decision 2): a Windows-created file is only ever reopened on Windows, so
    the alignment quantum need not match POSIX.
  - `:210-218` — the checkpoint ordering contract and the durability-epoch slot/
    chunk quarantine. This task must not break the seam these rely on (the
    `io_msync`/`io_mprotect` shims and the `ChunkReleaseFence` deferred punch stay
    functional on Windows); proving the protocol is `pool.checkpoints_win32`.
- **doc 17 — Internal components** (`docs/design/17-internal-components.md`):
  - `:36,:49` — `arbc::pool` is **Level 1**, depends **only on `base`** ("mmap/
    anonymous backing, checkpoint protocol" live here). `<windows.h>`/kernel32
    is a platform facility, invisible to the CMake/include levelization check —
    no new `arbc_*` edge (Constraint 7).

### Source seams

- `src/pool/arbc/pool/workspace_file.hpp` — the macro (`:20-24`), the class
  (`:180-302`), the five shims (`:264-268`), the private state (`:287-301`,
  incl. `int d_fd`), the injector/fence seams (`:110-163`), the header/entry
  layout (`:59-95`).
- `src/pool/workspace_file.cpp` — the POSIX arm (`:58-493`): shims (`:75-149`),
  `create` (`:151-176`), `acquire` (`:221-272`), `release` (`:281`), dtor
  (`:207-217`), `punch_now` (`:294-309`), `sync_data`/`sync_header`
  (`:422,:432`), `protect_data`/`protect_range` (`:465,:483`), `read_header`
  (`:313-...`), `open` (`:334-...`); and the `#else` `Unsupported` stub arm
  (`:495-542`) the Windows backend supplants.
- `src/pool/slot_store.cpp:15-17,53-81` — the `#if ARBC_HAS_WORKSPACE_FILES`
  guard around the anonymous `mmap`/`munmap` fast path; flipping the macro on
  Windows also selects the real anonymous `mmap` here, so the Windows anonymous
  path must compile too — but Windows has no POSIX `mmap`. **Note:** `slot_store.cpp`'s
  anonymous path is guarded by `ARBC_HAS_WORKSPACE_FILES` and calls `::mmap`
  directly; enabling the macro on Windows would route it to a nonexistent
  `::mmap`. This task must gate the anonymous `mmap`/`munmap` fast path on POSIX
  specifically (`__unix__`/`__APPLE__`) rather than on `ARBC_HAS_WORKSPACE_FILES`,
  leaving Windows anonymous backing on the `MapViewOfFile(INVALID_HANDLE_VALUE)`
  page-backed mapping or the existing `operator new` fallback (Constraint 6).
- `src/pool/CMakeLists.txt:1-9` — `workspace_file.cpp` is already in `SOURCES`;
  the single-file approach (Decision 1) needs **no CMake change** (kernel32 is
  auto-linked by MSVC).
- `src/pool/t/workspace_file.t.cpp` — the lifecycle suite to bring green on
  Windows (`:17-24` honesty check, `:27-275` body, `:195` Linux-only hole-punch
  section).

### CI / build

- `.github/workflows/ci.yml:60` — the `msvc-debug` / `windows-latest` /
  `win-dev` lane: `cmake --preset win-dev` (`:86`), build (`:88`), `ctest
  --preset win-dev` (`:90`). `:92-108` — the Linux-only `coverage` lane with the
  `diff-cover --fail-under=90` gate.
- `CMakePresets.json:70-75,94,105` — the `win-dev` configure/build/test presets
  (Ninja; MSVC dev prompt).

### Tests / claims

- `tests/claims/registry.tsv:50` — `15-memory-model#hole-punch-returns-storage`
  ("Releasing a file-backed chunk hole-punches it, dropping the workspace file's
  allocated block count"); `:57` —
  `15-memory-model#workspace-io-faults-surface-as-values` ("Every workspace-file
  syscall failure … surfaces as a `WorkspaceFileErrc` value, never an abort or
  UB"); `:49` — `#chunk-growth-preserves-addresses`. Re-enforced on Windows by
  the same lifecycle assertions; **no new rows** (Decision 8).

### Predecessor / sibling refinements

- `tasks/refinements/pool/mmap_backing.md` — the POSIX design + its Decisions
  1-3 and the Status block (`:123-133`) listing the artifacts this task extends;
  Constraint (`:73-80`) that specified this leaf.
- `tasks/refinements/runtime/plugin_loading_win32.md` — the sibling Windows-port
  style: single `#if defined(_WIN32)` seam over shared orchestration (its
  Decision 1), CI-green-not-compile-only acceptance (its Decision 2), no
  design-doc delta for an additive OS backing (its Decision 6), parity
  re-enforcement of existing claims with no new rows (its Decision 5).

## Constraints / requirements

1. **Lifecycle parity — same values, both platforms.** `create`, `acquire`
   (grow), `release`, `punch_now`, `read_header`, and `open` produce the same
   observable results (`ChunkSpan`s, `WorkspaceHeader` fields, `WorkspaceFileErrc`
   codes, `live_chunk_count`, address stability) for the same call sequence on
   Windows as on POSIX. Achieved by changing only the leaf calls, not the
   orchestration.
2. **Address stability across growth.** Existing chunk mappings must never move
   when the file grows (the `#chunk-growth-preserves-addresses` claim, doc
   15:191-192). Windows growth recreates the `CreateFileMapping` object; existing
   `MapViewOfFile` views remain valid after the old mapping handle is closed
   (documented Win32 behavior) — the address-stability contract holds
   (Decision 3).
3. **`MapViewOfFile` offset alignment.** Every mapped chunk's file offset (and
   `data_offset`, chunk 0's start) must be a multiple of
   `SYSTEM_INFO.dwAllocationGranularity` on Windows (`MapViewOfFile` rejects
   finer offsets), not merely page-aligned. The offset stride and the length
   round-up in `acquire` use `max(dwPageSize, dwAllocationGranularity)` as the
   quantum on Windows (Decision 2).
4. **Errors are values on Windows too.** Every `CreateFile`/`CreateFileMapping`/
   `MapViewOfFile`/`SetEndOfFile`/`DeviceIoControl`/`ReadFile` failure is captured
   as the matching `WorkspaceFileErrc` with `GetLastError()` in
   `WorkspaceFileError::sys_errno`; disk-full during growth is `GrowFailed` the
   caller sees via `last_error()`, never an abort (doc 15:57, the
   `#workspace-io-faults-surface-as-values` claim). `sys_errno` carries the Win32
   `GetLastError()` code on Windows (vs POSIX `errno`); both are same-machine
   diagnostics with no portability promise (doc 15:204-206).
5. **Injector + fence seams stay live.** The five Windows `io_*` shims route
   through `d_injector` (one branch before/after the real call), and
   `punch_now`/`release` keep honoring `ChunkReleaseFence`, so
   `pool.crash_tests_win32` and `pool.checkpoints_win32` can build their sweeps
   on the same `WorkspaceSyscall` enumeration without re-plumbing.
6. **Anonymous backing must still compile on Windows.** Enabling the macro must
   not route `slot_store.cpp`'s anonymous fast path (`:53-81`) or `slab`/anonymous
   `munmap` to a nonexistent POSIX `::mmap`. Re-gate that fast path on a
   POSIX-specific predicate (`__unix__ || __APPLE__`) so Windows anonymous
   allocation stays on its own path (`operator new` fallback or a
   `MapViewOfFile(INVALID_HANDLE_VALUE)` page mapping); do not couple "has
   workspace files" to "has POSIX `mmap`."
7. **No new levelization edge, no CMake dependency delta.** The backing stays in
   `arbc::pool` (L1) over `base`; `<windows.h>`/kernel32 is a platform facility,
   invisible to `scripts/check_levels.py`. `workspace_file.cpp` is already in
   `SOURCES`; the single-seam approach needs no CMake change. No doc-10
   dependency-table entry.
8. **One seam, no source fork.** The Windows code lives behind `#if
   defined(_WIN32)` branches inside `workspace_file.cpp` (a third arm alongside
   the POSIX and `Unsupported`-stub arms), not a parallel
   `workspace_file_win32.cpp` (Decision 1), so the directory/root-slot layout and
   the fence/injector dispatch stay single-sourced.
9. **No regression on the POSIX lanes.** The refactor (leaf extraction, the
   `slot_store.cpp` re-gate) keeps the POSIX arms behaviorally identical; all
   Linux lanes (gcc/clang/asan/tsan/rtsan/coverage) stay green.

## Acceptance criteria

- **`msvc-debug` CI lane goes green (headline).** After the change, the
  `windows-latest` / `win-dev` lane (`.github/workflows/ci.yml:60`) configures,
  builds `arbc_pool` with the Windows backend (the `Unsupported` stub arm is no
  longer selected on Windows), and `ctest --preset win-dev` **passes**
  `src/pool/t/workspace_file.t.cpp` on Windows — the full create/grow/release/
  reopen lifecycle, address stability, header round-trip + bad-magic reject, and
  bookkeeping-stays-anonymous (the file never grows on refcount-only traffic).
  The suite's `#if ARBC_HAS_WORKSPACE_FILES` honesty check (`:17-24`) now takes
  the *supported* branch on Windows.
- **`#hole-punch-returns-storage` enforced on Windows** (`registry.tsv:50`). The
  hole-punch section currently `#if defined(__linux__)`
  (`workspace_file.t.cpp:195`) gains a `#if defined(_WIN32)` branch that asserts
  the file's on-disk allocation (`GetCompressedFileSize`) drops after releasing
  chunks — the same claim, measured via the Windows allocated-size API, guarded
  to NTFS/sparse-capable volumes. **No new claim row** (parity re-enforcement).
- **`#workspace-io-faults-surface-as-values` enforced on Windows**
  (`registry.tsv:57`). A disk-full growth is exercised via the `SyscallInjector`
  (inject a positive error at `WorkspaceSyscall::Ftruncate`/`::Mmap` during
  `acquire`), asserting `acquire` returns `unexpected` and `last_error()` carries
  `GrowFailed` with a Win32 `sys_errno` — **not** a crash. (POSIX keeps its
  `RLIMIT_FSIZE` real disk-full test; the injector is the platform-neutral
  substitute on Windows, using the sanctioned mocking seam, doc 16:227-229.) A
  truncated/short reopened file still yields `HeaderIoFailed`/`BadMagic` as a
  value via `read_header`/`open`.
- **`#chunk-growth-preserves-addresses` enforced on Windows**
  (`registry.tsv:49`). The address-stability test (chunk pointers taken before
  growth stay valid and unchanged after arbitrary further growth) passes on the
  `msvc-debug` lane, proving the recreate-mapping-on-grow model preserves view
  addresses.
- **Anonymous backing green on Windows.** With the macro flipped, `slot_store`'s
  anonymous path and the whole `pool` test suite (`t/pool.t.cpp`, etc.) still
  build and pass on the `msvc-debug` lane (Constraint 6) — the flip does not
  break the universal fallback.
- **POSIX lanes unregressed; coverage gate held.** All Linux lanes stay green.
  The `#if defined(_WIN32)` branches are compiled out of the Linux coverage TU,
  so they are absent from `coverage.xml` — neither raising nor lowering the
  `diff-cover --fail-under=90` gate (`ci.yml:92-108`); the **shared** and
  **test** changed lines (any leaf extraction, the `slot_store.cpp` re-gate, the
  Windows test-branch's POSIX `#else`) stay exercised on Linux at ≥90 %.
- **Build / WBS gate.** `scripts/check_levels.py` green (no new `arbc_*` edge);
  `-Werror -Wpedantic` (GCC/Clang) and `/W4 /WX /permissive-` (MSVC) clean; and
  after the closer lands `complete 100` + the refinement back-link, `tj3
  project.tjp 2>&1 | grep -iE "error|warning"` is silent
  (`tasks/refinements/README.md:57-68`).
- **No deferred WBS leaves.** This task closes the workspace-file *data lifecycle*
  on Windows in full and registers **no successor** — the two follow-ons
  (`pool.checkpoints_win32`, `pool.crash_tests_win32`) already exist in the WBS
  (`tasks/05-pool.tji:70-88`) and already `depend !mmap_backing_win32`; this
  leaf simply unblocks them.

## Decisions

1. **One `#if defined(_WIN32)` seam in `workspace_file.cpp`, shared
   orchestration — not a source fork.** Add a third platform arm (Windows)
   inside the `ARBC_HAS_WORKSPACE_FILES` region, alongside the existing POSIX
   arm and the `#else` `Unsupported` stub arm; give the five `io_*` shims and the
   file-open/read/size leaves Windows bodies, keep `acquire`'s grow-sequence,
   `release`'s fence dispatch, the directory/root-slot bookkeeping, the
   `d_reopened` recovery queue, and every value type shared. Bundling the
   divergence into the leaves makes "mirrors POSIX" a structural guarantee: the
   on-disk layout and the errors-as-values mapping are single-sourced and cannot
   drift — exactly the argument the sibling `runtime.plugin_loading_win32`
   Decision 1 made for the loader.
   *Rejected: a parallel `workspace_file_win32.cpp` with duplicated
   orchestration* — two copies of the directory bookkeeping, the A/B root-slot
   layout, and the fence/injector dispatch would diverge, and
   `pool.checkpoints_win32`/`pool.crash_tests_win32` depend on byte-identical
   layout and an identical `WorkspaceSyscall` enumeration across platforms.
2. **Allocation-granularity offset alignment on Windows, permitted by the
   non-portability clause.** `MapViewOfFile` requires the view's file offset be a
   multiple of `dwAllocationGranularity` (typically 64 KiB), not the 4 KiB page
   the POSIX path uses. The offset stride and `acquire`'s length round-up use
   `max(dwPageSize, dwAllocationGranularity)` on Windows; `data_offset` and every
   `WorkspaceChunkEntry.offset` are stored explicitly in the header
   (`workspace_file.hpp:65,78`), so `open`/recovery re-maps at the recorded
   offsets with no recomputation. This is safe precisely because doc 15:204-206
   declares the workspace file a same-machine, no-portability artifact — a
   Windows-created file is only reopened on Windows, so a coarser alignment is
   internally consistent and never crosses to a POSIX reader.
   *Rejected: forcing 4 KiB alignment to match POSIX byte-for-byte* —
   `MapViewOfFile` would reject the offsets; portability the doc explicitly
   disclaims buys nothing.
3. **Growth recreates the `CreateFileMapping` object each grow; views survive.**
   To map a chunk beyond the current mapped extent, the file-mapping object must
   cover it. Each grow extends the file's logical size and creates a fresh
   `CreateFileMapping` at the new max size, then `MapViewOfFile`s the new chunk;
   the previously-created views remain valid after the old mapping handle is
   closed (documented Win32 semantics — the view holds its own reference). This
   is the faithful analog of the POSIX `ftruncate`-then-`mmap`-the-new-chunk
   rhythm and preserves address stability (Constraint 2).
   *Rejected: reserve one giant `CreateFileMapping` up front* — the mirror of the
   POSIX "giant `PROT_NONE` region" the predecessor's Decision 1 already rejected
   (address-space accounting, conflicts with chunk-at-a-time growth and
   hole-punch). *Rejected: a `CreateFileMapping` per chunk kept open* — more
   permanently-held handles for no address-stability gain over recreate-on-grow.
4. **Sparse-file hole-punch via `FSCTL_SET_ZERO_DATA`, best-effort disk
   reclamation, unconditional memory reclamation.** Mark the file sparse
   (`FSCTL_SET_SPARSE`) at create; `punch_now` `UnmapViewOfFile`s the chunk
   (memory back) then `DeviceIoControl(FSCTL_SET_ZERO_DATA)` deallocates the range
   (disk back). On a non-NTFS / non-sparse volume `FSCTL_SET_SPARSE` fails and the
   punch degrades to directory-clear only — memory is always reclaimed, on-disk
   reclamation is best-effort — mirroring POSIX-on-macOS where `punch_now`
   compiles to just the directory-clear (`workspace_file.cpp:304-308`). The
   `#hole-punch-returns-storage` Windows assertion is therefore NTFS-guarded, as
   the POSIX one is Linux-guarded. Disk-full is exercised via the injector, since
   Windows has no `RLIMIT_FSIZE`.
   *Rejected: refusing to run without sparse support* — the workspace still
   functions (anonymous-equivalent disk behavior) without hole-punch; a hard
   requirement would strand FAT/ReFS users for a reclamation optimization.
5. **`io_msync`/`io_mprotect` Windows bodies are authored here (linker-forced);
   their protocol correctness is `pool.checkpoints_win32`.** Once
   `ARBC_HAS_WORKSPACE_FILES` is `1` on Windows, every member declared under the
   macro — including `io_msync`, `io_mprotect`, `sync_data`, `sync_header`,
   `protect_data`, `protect_range` — must have exactly one Windows definition for
   the translation unit to link; there is no "partial" definition. So this task
   supplies the mechanical mappings (`io_msync` → `FlushViewOfFile` +
   `FlushFileBuffers`; `io_mprotect` → `VirtualProtect`) as the obvious
   substitution named in `pool.checkpoints_win32`'s own note
   (`tasks/05-pool.tji:74`). This task validates only that they compile and the
   data lifecycle works; the **ordered-commit durability semantics** (msync data
   → flip root → msync header giving crash-consistency), the debug read-only
   publish, and the durability-epoch quarantine are validated by
   `pool.checkpoints_win32` (which `depends !checkpoints, !mmap_backing_win32`
   precisely so it can prove the protocol on top of these bodies). This division
   is why the compile boundary and the validation boundary differ — and it is
   forced, not a judgment call.
   *Rejected: leave the checkpoint helpers as `Unsupported` stubs on Windows and
   let `checkpoints_win32` define them* — impossible: a class member cannot be
   defined twice nor left undefined once its declaring macro arm is active.
6. **Private platform handle typedef + Windows mapping-handle member; public API
   unchanged.** `d_fd` (`int`) becomes a platform typedef (`int` POSIX,
   `HANDLE`/`void*` Windows) and a Windows-only file-mapping `HANDLE` member is
   added — both private. The public `WorkspaceFileChunkSource` interface
   (`create`/`acquire`/`release`/`read_header`/`open`/checkpoint helpers) is
   byte-for-byte unchanged, so no consumer and no other component recompiles
   against a changed signature.
   *Rejected: `_open_osfhandle`/`_get_osfhandle` to keep a bare `int d_fd`* —
   couples a CRT fd's lifetime to the `HANDLE` awkwardly when `CreateFileMapping`/
   `DeviceIoControl` need the raw `HANDLE` anyway; a clean typedef is simpler.
7. **No design-doc delta.** Doc 15 names POSIX primitives (`fallocate`, `msync`,
   `mprotect`, `MAP_SHARED`) illustratively, not as a normative platform
   restriction, and it already declares the workspace file same-machine /
   non-portable (`:204-206`), which accommodates Windows's coarser alignment and
   Win32 `GetLastError()` diagnostics. Adding a second platform backing behind
   the already-settled, platform-neutral backing policy (`:163-167`) alters no
   designed behavior, so doc 16's same-commit amendment rule is not triggered —
   the identical call the sibling `runtime.plugin_loading_win32` Decision 6 made.
   *Rejected: a clarifying doc-15 note pairing each POSIX API with its Win32
   equivalent* — the prose APIs are illustrative; a per-API cross-reference is
   documentation churn, not a normative change, and the non-portability clause
   already covers platform-specific layout.
8. **Re-enforce existing claims, add no rows.** The three lifecycle claims
   (`#hole-punch-returns-storage`, `#workspace-io-faults-surface-as-values`,
   `#chunk-growth-preserves-addresses`) are re-enforced on Windows by the *same*
   assertions running on the `msvc-debug` lane (with the platform-specific
   measurement/injection branches) — behavioral parity, not new behavior.
   *Rejected: Windows-specific claim ids* — the claims are platform-agnostic
   statements; only the measurement (`GetCompressedFileSize` vs `st_blocks`) and
   the fault mechanism (injector vs `RLIMIT_FSIZE`) differ.

## Open questions

(none — all decided.)

## Status

**Done** — 2026-07-10.

- `src/pool/arbc/pool/workspace_file.hpp`: capability macro `ARBC_HAS_WORKSPACE_FILES` flipped to `1` on `_WIN32`; `WorkspaceFileHandle` typedef (`int` POSIX / `HANDLE` Windows) and Windows-only `d_mapping` (`HANDLE`) member added to `WorkspaceFileChunkSource` (Decision 6).
- `src/pool/workspace_file.cpp`: third `#if defined(_WIN32)` arm added inside the `ARBC_HAS_WORKSPACE_FILES` region — `CreateFileMapping`/`MapViewOfFile` bodies for all five `io_*` syscall shims, `FSCTL_SET_ZERO_DATA` hole-punch, `CreateFileA`/`GetFileSizeEx`/`GetSystemInfo` factory leaves, portable-alias constants for shared orchestration; `Unsupported` stub arm superseded on Windows (Decisions 1–5).
- `src/pool/slot_store.cpp`: anonymous `mmap`/`munmap` fast path re-gated on `ARBC_ANON_USES_POSIX_MMAP` (`__unix__ || __APPLE__`) so enabling the macro on Windows does not route to a nonexistent `::mmap` (Constraint 6).
- `src/pool/t/workspace_file.t.cpp`: Windows test arm added — `GetCompressedFileSize`-based `#hole-punch-returns-storage` (NTFS-guarded), injector-based `#workspace-io-faults-surface-as-values` (`ERROR_DISK_FULL`→`GrowFailed`), and cross-platform `#chunk-growth-preserves-addresses` + header round-trip / bad-magic.
- `src/pool/t/crash_tests.t.cpp`, `src/pool/t/checkpoint.t.cpp`, `src/runtime/t/housekeeping.t.cpp`, `src/runtime/t/housekeeping_thread.t.cpp`: POSIX-only test bodies gated `&& !defined(_WIN32)` so the macro flip does not drag `fork`/`RLIMIT`/`unistd.h` into the MSVC build; those suites are ported by `runtime.housekeeping_win32` once `pool.checkpoints_win32` is complete.
- Claims `15-memory-model#hole-punch-returns-storage`, `#workspace-io-faults-surface-as-values`, `#chunk-growth-preserves-addresses` re-enforced on Windows via the new test branches; no new registry rows (Decision 8).
- All Linux lanes (gcc/clang/asan/tsan/rtsan/coverage) unregressed; `ARBC_ANON_USES_POSIX_MMAP` is byte-identical to the old `ARBC_HAS_WORKSPACE_FILES` guard on Linux; diff-cover gate held (Windows branches compiled out of the Linux coverage TU).
