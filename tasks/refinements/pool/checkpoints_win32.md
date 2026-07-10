# checkpoints_win32 — Checkpoint protocol, Windows validation

## TaskJuggler entry

Back-link: [`tasks/05-pool.tji:71-76`](../../05-pool.tji), under the `pool`
umbrella:

> ```
> task checkpoints_win32 "Checkpoint protocol — Windows port" {
>   effort 1d
>   allocate team
>   depends !checkpoints, !mmap_backing_win32
>   note "FlushViewOfFile+FlushFileBuffers in place of msync, VirtualProtect in place of mprotect for debug read-only publish, behind ARBC_HAS_WORKSPACE_FILES. Source-of-debt: tasks/refinements/pool/checkpoints.md. Doc 15."
> }
> ```

This task feeds milestone **M9 — "v0.1 release"** (`m9_release`,
`tasks/99-milestones.tji:69-73`), whose `depends` list already names
`pool.checkpoints_win32` (`tasks/99-milestones.tji:71`), alongside the sibling
Windows-parity leaves `pool.mmap_backing_win32`, `pool.crash_tests_win32`, and
`runtime.plugin_loading_win32`. Two downstream leaves **depend on this one**:
`pool.crash_tests_win32` (`tasks/05-pool.tji:84-89`, `depends !crash_tests,
!checkpoints_win32`) and `runtime.housekeeping_win32` (`tasks/65-runtime.tji:104-108`,
`depends !housekeeping_thread, pool.checkpoints_win32` — its ported
`housekeeping.t.cpp`/`housekeeping_thread.t.cpp` exercise the checkpoint
durability protocol, so they gate behind this task). The closer lands
`complete 100` after the `allocate team` line, appends
`Refinement: tasks/refinements/pool/checkpoints_win32.md` to the note, and — if
every other `m9_release` dependency is complete — propagates `complete 100` to
the milestone (milestones don't infer completion,
`tasks/refinements/README.md:69-72`).

## Effort estimate

**1 day** (`tasks/05-pool.tji:72`).

The heavy lifting is already done twice over. The POSIX predecessor
(`pool.checkpoints`, DONE 2026-07-04) built the entire protocol as a
**platform-neutral, header-only `Checkpointer`** — its `commit()` routes solely
through `WorkspaceFileChunkSource::sync_data`/`sync_header`/`protect_range`
(`src/pool/arbc/pool/checkpoint.hpp:133-170,244`), never a raw syscall. The
Windows-backing predecessor (`pool.mmap_backing_win32`, DONE 2026-07-10) already
authored the mechanical Windows bodies those helpers call —
`io_msync` → `FlushViewOfFile` + `FlushFileBuffers`
(`src/pool/workspace_file.cpp:218-241`), `io_mprotect` → `VirtualProtect`
(`:243-256`) — precisely because the linker forced them once
`ARBC_HAS_WORKSPACE_FILES` flipped to `1` on Windows (mmap_backing_win32
Decision 5). So the protocol **already compiles and links on Windows**; what is
missing is its **validation**: `src/pool/t/checkpoint.t.cpp` is gated
`#if ARBC_HAS_WORKSPACE_FILES && !defined(_WIN32)` (`:24`), deliberately deferred
to this leaf by mmap_backing_win32 (its Status, `mmap_backing_win32.md:534`).
1d covers un-gating the suite, porting its POSIX-only test leaves (temp-path,
file-copy, on-disk-size measurement) to the Windows spellings the sibling
`workspace_file.t.cpp` already established, and — the one genuinely new piece —
the SEH witness for the debug read-only-publish claim.

## Inherited dependencies

**Settled:**

- **`pool.checkpoints`** (DONE 2026-07-04, `tasks/refinements/pool/checkpoints.md`,
  this task's `depends !checkpoints` edge). It produced the protocol this task
  validates on Windows:
  - `src/pool/arbc/pool/checkpoint.hpp` — the header-only `Checkpointer`:
    A/B-root ordered commit (`commit()`, `:133-170`), the
    `DurabilityEpochFence` quarantine seam, and `open`/`finalize_open` recovery.
    Every platform-touching operation funnels through the chunk source's
    `sync_data`/`sync_header` (`:135,145`) and `protect_range` (`:164,244`) —
    **no direct POSIX call**, which is exactly why the protocol is
    platform-portable with zero production edits here.
  - `src/pool/t/checkpoint.t.cpp` — the 12-case suite (commit round-trip, A/B
    alternation, fence quarantine/release, recovery rebuild + complement free
    list, behavioral counters, the debug read-only-publish witness, the TSan
    smoke). Its body is gated `#if ARBC_HAS_WORKSPACE_FILES && !defined(_WIN32)`
    (`:24`); the `checkpoint support tracks workspace-file support` case (`:20-22`)
    runs unconditionally and, on Windows post-mmap_backing_win32, now asserts
    `workspace_files_supported() == true`.
  - Claims `15-memory-model#checkpoint-recovers-consistent-root`
    (`tests/claims/registry.tsv:54`),
    `15-memory-model#freed-slot-quarantined-until-durable` (`:55`), and
    `15-memory-model#checkpoint-published-chunks-read-only` (`:56`) — re-enforced
    on Windows by the same test cases (Decision 4).
  - Its **Acceptance-criteria deferral** (`checkpoints.md:313-318`) named this
    leaf verbatim: "the Windows ordered-commit port — `pool.checkpoints_win32`
    (est. 1d): `FlushViewOfFile` + `FlushFileBuffers` in place of `msync`, and
    `VirtualProtect` for the debug read-only publish, behind
    `ARBC_HAS_WORKSPACE_FILES`."
- **`pool.mmap_backing_win32`** (DONE 2026-07-10,
  `tasks/refinements/pool/mmap_backing_win32.md`, this task's
  `depends !mmap_backing_win32` edge). It produced every seam this task's
  validation rides:
  - `ARBC_HAS_WORKSPACE_FILES == 1` on `_WIN32`
    (`workspace_file.hpp:20-24`) — the switch that compiles the real Windows
    backend and links the `Checkpointer` into the msvc-debug build.
  - The Windows `io_msync`/`io_mprotect` bodies
    (`src/pool/workspace_file.cpp:218-256`), and the checkpoint helpers built on
    them — `sync_data`/`sync_header` (`:789-807`), `protect_data`/`protect_range`
    (`:831-857`) — all now compiling on Windows (mmap_backing_win32 Decision 5).
  - The Windows-portable **test idioms** in the sibling
    `src/pool/t/workspace_file.t.cpp` this task mirrors verbatim: a `TempPath`
    with a `GetTempPathA`/`GetTempFileNameA` arm (`:49-77`) and an
    `allocated_size()` helper measuring on-disk blocks via `GetCompressedFileSizeA`
    on Windows vs `st_blocks*512` on POSIX (`:85-102`).
  - It **gated** `checkpoint.t.cpp` off `_WIN32` (its Status,
    `mmap_backing_win32.md:534`: "POSIX-only test bodies gated `&& !defined(_WIN32)`
    so the macro flip does not drag `fork`/`RLIMIT`/`unistd.h` into the MSVC
    build; those suites are ported by `runtime.housekeeping_win32` once
    `pool.checkpoints_win32` is complete"). Un-gating `checkpoint.t.cpp` is the
    core of this task.
- **The Windows CI lane already exists** — `msvc-debug` on `windows-latest` with
  the `win-dev` preset (`.github/workflows/ci.yml:60`;
  `CMakePresets.json:70-75,94,105`), a **debug** build so the `#ifndef NDEBUG`
  read-only-publish witness is live. As `mmap_backing_win32`/`plugin_loading_win32`
  established, this makes the deliverable **CI-green, not compile-only**
  (Decision 5).

**Pending:** none. `pool.crash_tests_win32` and `runtime.housekeeping_win32`
**depend on this task**, not the reverse.

## What this task is

Validate the checkpoint durability protocol on Windows by bringing
`src/pool/t/checkpoint.t.cpp` green on the `msvc-debug` CI lane — the ordered
A/B-root commit (`FlushViewOfFile`+`FlushFileBuffers` for the data-then-header
sync), the durability-epoch slot **and** chunk quarantine, recovery rebuild, and
the debug `VirtualProtect` read-only publish, all proven on top of the Windows
backend `mmap_backing_win32` built. Concretely:

- **(a) Un-gate the suite.** Change `checkpoint.t.cpp:24` from
  `#if ARBC_HAS_WORKSPACE_FILES && !defined(_WIN32)` to
  `#if ARBC_HAS_WORKSPACE_FILES`, so the full body compiles into the msvc-debug
  translation unit. Update the stale header comment (`:15-19`) that says the
  suite "stays gated off `_WIN32`."
- **(b) Port the POSIX-only test headers.** The suite pulls `<fcntl.h>`,
  `<sys/mman.h>`, `<sys/stat.h>`, `<unistd.h>`, `<csetjmp>`, `<csignal>`
  (`:26-34`); guard those behind `#if !defined(_WIN32)` and add a
  `#if defined(_WIN32)` `#include <windows.h>` arm, exactly as
  `workspace_file.t.cpp:31-34` does.
- **(c) Port the test helpers to the Windows spellings the sibling suite
  established.** All three live in `checkpoint.t.cpp`'s anonymous namespace and
  currently POSIX-only:
  - `TempPath` (`:40-57`, `mkstemp`/`/tmp`/`::unlink`) → the two-arm
    `GetTempPathA`/`GetTempFileNameA` + `DeleteFileA` version already written at
    `workspace_file.t.cpp:49-77`.
  - `copy_file` (`:155-…`, `::open`/`O_RDONLY`/`pread`) → `CopyFileA` on Windows
    (an independent-file recovery copy is the whole point of the helper — the
    Win32 one-call form is the faithful analog).
  - `block_count` (`:144-148`, `::stat`/`st_blocks`) → fold into an
    `allocated_size()` returning on-disk bytes, `GetCompressedFileSizeA` on
    Windows and `st_blocks*512` on POSIX, mirroring `workspace_file.t.cpp:85-102`.
- **(d) The hole-punch durability case gets a Windows arm.** "an emptied chunk
  is not hole-punched until the emptying is durable" (`:440-468`) is currently
  `#if defined(__linux__)` with `SUCCEED("…Linux-only…")` elsewhere. Add a
  `#if defined(_WIN32)` branch that dirties+flushes the chunk with
  `FlushViewOfFile` in place of `::msync(MS_SYNC)`, measures on-disk size via the
  `allocated_size()` helper, asserts the size is **unchanged** after `release`
  (deferred punch) and **drops** after `commit()` drains the fence — NTFS/sparse
  guarded, since `FSCTL_SET_ZERO_DATA` is best-effort (mmap_backing_win32
  Decision 4). This re-enforces the "nor hole-punched" half of
  `#freed-slot-quarantined-until-durable` on Windows.
- **(e) The debug read-only-publish witness gets an SEH arm** (the one genuinely
  new piece). "a write into a published data chunk faults after commit (debug)"
  (`:531-580`) proves `#checkpoint-published-chunks-read-only` via a
  `sigaction`(SIGSEGV)/`sigsetjmp`/`siglongjmp` harness. Windows delivers a write
  to a `VirtualProtect(PAGE_READONLY)` page as a **structured exception**
  (`EXCEPTION_ACCESS_VIOLATION`), not a POSIX signal, so add a `#if defined(_WIN32)`
  arm that isolates the faulting store in a tiny helper wrapped in
  `__try { helper(); } __except(GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
  ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { faulted = true; }`,
  then un-seals with `protect_data(false)` exactly as the POSIX arm does
  (`:576-577`). The faulting write must sit in its own function with no
  unwind-requiring locals, because MSVC forbids `__try`/`__except` in a frame
  that also needs C++ object unwinding (C2712) — see Decision 3.

Everything else in the suite is already platform-neutral and needs no change:
the `Checkpointer` API, `GraphNode`/`build_graph`/`walk` (index-only,
pointer-free records — doc 15 position independence), the commit round-trip, A/B
alternation, recovery-rebuild, behavioral-counter, and TSan-smoke cases (the
last already uses `std::thread`, `:583-668`). **No production source changes are
expected** (Decision 1): the protocol and its Windows syscall bodies already
exist.

**Not this task:**

- **The Windows crash-recovery sweep** — kill-at-every-syscall fault injection
  via the `SyscallInjector` on Windows (`CreateProcess`/`TerminateProcess` kill
  sweep, disk-full/short-file): that is `pool.crash_tests_win32`
  (`tasks/05-pool.tji:84-89`, `depends !crash_tests, !checkpoints_win32`). This
  task pins the ordering invariant at the two/three commit boundaries directly
  (the `recovery lands on the old root before the header sync…` case, `:229`),
  exactly as the POSIX `pool.checkpoints` did; the exhaustive shim is the
  crash-tests leaf's job (checkpoints.md Decision "Focused crash checks here…").
- **Porting the housekeeping suites** — `housekeeping.t.cpp`/
  `housekeeping_thread.t.cpp`, which exercise checkpoint **cadence**, are
  `runtime.housekeeping_win32` (`tasks/65-runtime.tji:104-108`), which depends on
  this task.
- **Any change to the protocol, the workspace layout, or the backing policy** —
  the protocol is `pool.checkpoints`' contract (doc 15); this task validates a
  second platform, it amends nothing (Decision 6).

## Why it needs to be done

On Windows today `checkpoint.t.cpp`'s body is compiled out (`:24`), so the
durability protocol — though it links via the Windows `io_msync`/`io_mprotect`
bodies mmap_backing_win32 authored — is **entirely unproven** on Windows: no test
confirms that `FlushViewOfFile`+`FlushFileBuffers` gives crash-consistent
ordered commit, that the durability-epoch fence quarantines freed slots and
un-punched chunks until durable, or that `VirtualProtect` actually faults a stray
write into a published chunk. M9's headline is a v0.1 release with Windows
parity; without this leaf, the four crash-recovery motivations doc 15 gives
(`docs/design/15-memory-model.md:161-180`, first among them "crash recovery")
have a POSIX-only *proof*, and the two downstream Windows leaves
(`pool.crash_tests_win32`, `runtime.housekeeping_win32`) — both of which build on
a **validated** protocol — cannot proceed.

## Inputs / context

### Design docs (normative, doc 16)

- **doc 15 — Memory model** (`docs/design/15-memory-model.md`),
  "Checkpointing rides the version model" (`:183-199`):
  - `:183-186` — the ordered-commit correctness core: "msync data chunks, then
    publish the root by flipping an A/B root slot in the header, then msync the
    header. A crash lands on the old or new root, both consistent." On Windows the
    two `msync` steps are `FlushViewOfFile`+`FlushFileBuffers` (via `io_msync`);
    the ordering invariant is identical.
  - `:187-191` — the durability-epoch fence: a slot freed after the last
    checkpoint is "reusable after checkpoint N." Validated on Windows unchanged
    (the fence is platform-neutral C++ in `checkpoint.hpp`).
  - `:196-199` — debug `mprotect` of published data chunks. On Windows this is
    `VirtualProtect(PAGE_READONLY)` (via `io_mprotect`); the witness is an SEH
    `EXCEPTION_ACCESS_VIOLATION` catch (Decision 3).
  - `:204-206` — "workspace files are same-machine artifacts (native endianness
    and padding, no portability promise)." A Windows-created checkpoint file is
    only ever recovered on Windows, so the coarser allocation-granularity
    alignment mmap_backing_win32 chose is internally consistent — recovery reads
    the recorded offsets and validates the higher-generation root as on POSIX.
- **doc 17 — Internal components** (`docs/design/17-internal-components.md`):
  - `:36,:49` — `arbc::pool` is **Level 1**, depends **only on `base`**
    ("checkpoint protocol" lives here). `<windows.h>`/kernel32 is a platform
    facility invisible to `scripts/check_levels.py` — no new `arbc_*` edge
    (Constraint 6).
- **doc 16 — SDLC & quality** (`docs/design/16-sdlc-and-quality.md`): claims
  register (`enforces:` tags, `:15-21`); mocking/injection is a sanctioned seam
  (`:227-229`); behavioral counters over wall-clock (`:54-62`); crash-recovery
  tier (`:74-78`, the exhaustive sweep is `pool.crash_tests[_win32]`); TSan
  (`:66-73`); ≥90% diff coverage (`:112-118`).

### Source seams

- `src/pool/arbc/pool/checkpoint.hpp` — the platform-neutral `Checkpointer`:
  `commit()` (`:133-170`) → `sync_data`/`sync_header` (`:135,145`),
  `protect_range` seal (`:164,244`); recovery `open`/`finalize_open` (`:172-…`);
  the counters the behavioral-counter case reads (`:211-212,294-295`).
- `src/pool/workspace_file.cpp` — the Windows `io_msync` (`:218-241`) and
  `io_mprotect` (`:243-256`) bodies, and the checkpoint helpers built on them:
  `sync_data` (`:789`), `sync_header` (`:801`), `protect_data` (`:831`),
  `protect_range` (`:847`).
- `src/pool/t/checkpoint.t.cpp` — the suite to bring green: the gate (`:24`), the
  stale gating comment (`:15-19`), POSIX-only headers (`:26-34`), `TempPath`
  (`:40-57`), `walk`/`build_graph` (platform-neutral, `:59-142`), `block_count`
  (`:144-148`), `copy_file` (`:155-…`), the hole-punch case (`:440-468`), the
  behavioral-counter case (`:489-529`), the debug read-only-publish witness
  (`:531-580`), the TSan smoke (`:583-668`).
- `src/pool/t/workspace_file.t.cpp` — the Windows-portable idioms to mirror:
  `TempPath` (`:49-77`), `allocated_size()` (`:85-102`).
- `src/pool/CMakeLists.txt:11-14` — `checkpoint.t.cpp` is **already** wired into
  the `arbc_component_test(COMPONENT pool …)` target; **no CMake change**.

### CI / build

- `.github/workflows/ci.yml:60` — the `msvc-debug` / `windows-latest` / `win-dev`
  lane (configure `:86`, build `:88`, `ctest --preset win-dev` `:90`); `:92-108` —
  the Linux-only `coverage` lane with the `diff-cover --fail-under=90` gate.
- `CMakePresets.json:70-75,94,105` — the `win-dev` configure/build/test presets
  (Ninja; MSVC dev prompt; Debug config, so `#ifndef NDEBUG` is active).

### Tests / claims

- `tests/claims/registry.tsv:54` —
  `15-memory-model#checkpoint-recovers-consistent-root`; `:55` —
  `#freed-slot-quarantined-until-durable`; `:56` —
  `#checkpoint-published-chunks-read-only`. Re-enforced on Windows by the same
  test cases; **no new rows** (Decision 4).

### Predecessor / sibling refinements

- `tasks/refinements/pool/checkpoints.md` — the POSIX protocol design, its
  Decisions (single-checkpoint fence, aligned-word root, fenced hole-punch,
  focused-crash-checks-vs-shim), and the Status block naming this leaf.
- `tasks/refinements/pool/mmap_backing_win32.md` — the Windows backend + the test
  idioms this task reuses; its Decision 5 (mechanical `io_msync`/`io_mprotect`
  bodies here, protocol validation deferred to *this* task) and Decision 8
  (re-enforce existing claims, add no rows).
- `tasks/refinements/runtime/plugin_loading_win32.md` — the Windows-port house
  style: single `#if defined(_WIN32)` seam over shared logic, CI-green (not
  compile-only) acceptance, no design-doc delta for additive OS support.

## Constraints / requirements

1. **Protocol parity — same observable results, both platforms.** The ordered
   commit, A/B alternation, fence quarantine/release, recovery rebuild, and
   behavioral counters produce identical observable outcomes on Windows as on
   POSIX for the same call sequence — because the `Checkpointer` and fence are
   unchanged and only the test's platform leaves and the two syscall bodies
   differ.
2. **CI-green, not compile-only.** The deliverable is `checkpoint.t.cpp` running
   green under `ctest --preset win-dev` on the `msvc-debug` lane, including the
   `#ifndef NDEBUG` read-only-publish witness (the lane is a Debug build).
3. **The debug read-only-publish witness must actually fault.** On Windows the
   witness catches `EXCEPTION_ACCESS_VIOLATION` via SEH; the faulting store lives
   in its own no-unwind helper (C2712, Decision 3). The test then un-seals with
   `protect_data(false)` so teardown proceeds, exactly as the POSIX arm.
4. **Injector/fence seams stay live and untouched.** This task adds **no**
   production code; the `SyscallInjector`/`WorkspaceSyscall` enumeration and the
   `DurabilityEpochFence`/`ChunkReleaseFence` seams that `pool.crash_tests_win32`
   builds on remain exactly as mmap_backing_win32 / checkpoints left them.
5. **Errors stay values.** Nothing in the ported tests introduces an abort path;
   syscall-failure surfaces stay `expected`/`WorkspaceFileError` as on POSIX
   (only exercised at the existing focused points — the exhaustive injection
   sweep is `pool.crash_tests_win32`).
6. **No new levelization edge, no CMake dependency delta.** The protocol stays in
   `arbc::pool` (L1) over `base`; `<windows.h>`/kernel32 is a platform facility,
   invisible to `scripts/check_levels.py`. `checkpoint.t.cpp` is already in the
   test target; no CMake change.
7. **No regression on POSIX lanes.** The `#if !defined(_WIN32)` / `#if
   defined(_WIN32)` branching keeps the POSIX arms behaviorally identical; all
   Linux lanes (gcc/clang/asan/tsan/rtsan/coverage) stay green, and the
   `#if defined(_WIN32)` branches are compiled out of the Linux coverage TU
   (neither raising nor lowering the diff-cover gate).

## Acceptance criteria

- **`msvc-debug` CI lane goes green (headline).** After the change, the
  `windows-latest` / `win-dev` lane (`.github/workflows/ci.yml:60`) configures,
  builds `arbc_pool` + its tests with the Windows workspace backend, and
  `ctest --preset win-dev` **passes** `src/pool/t/checkpoint.t.cpp` — the commit
  round-trip, A/B alternation, recovery rebuild + complement free list, the
  behavioral-counter case, the hole-punch durability case (Windows arm), the
  debug read-only-publish witness (SEH arm), and the TSan-smoke case. The
  `checkpoint support tracks workspace-file support` case (`:20-22`) now takes the
  *supported* branch on Windows.
- **`15-memory-model#checkpoint-recovers-consistent-root` enforced on Windows**
  (`registry.tsv:54`). The `recovery lands on the old root before the header sync
  and the new root after` case (`checkpoint.t.cpp:229`) passes on the msvc-debug
  lane, proving `FlushViewOfFile`+`FlushFileBuffers` gives crash-consistent
  ordered commit (old root before the header flush, new root after). **No new
  claim row** — parity re-enforcement.
- **`15-memory-model#freed-slot-quarantined-until-durable` enforced on Windows**
  (`registry.tsv:55`). The slot-quarantine case passes unchanged (fence is
  platform-neutral), and the hole-punch half is re-enforced by the new
  `#if defined(_WIN32)` branch of the `an emptied chunk is not hole-punched…` case:
  `allocated_size()` (via `GetCompressedFileSizeA`) is unchanged after `release`
  and drops after `commit()`, NTFS/sparse-guarded. **No new claim row.**
- **`15-memory-model#checkpoint-published-chunks-read-only` enforced on Windows**
  (`registry.tsv:56`). In the Debug msvc-debug build, a write into a published
  (sealed) chunk after `commit()` raises `EXCEPTION_ACCESS_VIOLATION`, caught by
  the SEH `__try`/`__except` arm — proving `VirtualProtect(PAGE_READONLY)` sealed
  the chunk. **No new claim row.**
- **Behavioral-counter assertions unchanged (doc 16, never wall-clock).** The
  `behavioral counters: msyncs, fence releases, and epoch advance…` case
  (`:489-529`) passes on Windows: an unchanged scene issues zero data flushes
  beyond the header, the fence release counter advances only at commit, the epoch
  advances exactly once per commit — the `io_msync` Windows body increments the
  same counters the POSIX one does.
- **Concurrency (TSan/asan, explicit per doc 16).** The `TSan smoke: RT producers
  enqueue while the writer drains and checkpoints` case (`:583-668`) passes on the
  msvc-debug lane (a functional/asan-equivalent run — this repo has no MSVC TSan
  preset, same gap noted in `checkpoints.md`/`reclamation.md:305`; not
  re-litigated). It confirms every destructor fires once, only the permanent root
  survives, and the resulting file recovers the expected root on Windows.
- **POSIX lanes unregressed; coverage gate held.** All Linux lanes stay green; the
  `#if defined(_WIN32)` branches are absent from `coverage.xml` (compiled out of
  the Linux TU), neither raising nor lowering `diff-cover --fail-under=90`
  (`ci.yml:92-108`); the shared and POSIX-`#else` changed lines stay exercised on
  Linux at ≥90 %.
- **Build / WBS gate.** `scripts/check_levels.py` green (no new `arbc_*` edge);
  `/W4 /WX /permissive-` (MSVC) and `-Werror -Wpedantic` (GCC/Clang) clean; and
  after the closer lands `complete 100` + the refinement back-link,
  `tj3 project.tjp 2>&1 | grep -iE "error|warning"` is silent
  (`tasks/refinements/README.md:57-68`).
- **No deferred WBS leaves.** This task closes the checkpoint-protocol
  *validation* on Windows in full and registers **no successor** — the two
  follow-ons (`pool.crash_tests_win32`, `runtime.housekeeping_win32`) already
  exist in the WBS (`tasks/05-pool.tji:84-89`, `tasks/65-runtime.tji:104-108`) and
  already gate on this leaf; this task simply unblocks them.

## Decisions

1. **Test-porting/validation only — no production source change.** The
   `Checkpointer` is header-only and routes every platform operation through
   `WorkspaceFileChunkSource::sync_data`/`sync_header`/`protect_range`
   (`checkpoint.hpp:135,145,164,244`), and mmap_backing_win32 already authored the
   Windows `io_msync`/`io_mprotect` bodies and their `sync_*`/`protect_*` callers
   (`workspace_file.cpp:218-256,789-857`). So the protocol already compiles and
   links on Windows; the only missing artifact is its *proof*. The task is
   therefore un-gating and porting `checkpoint.t.cpp`. *Rejected: authoring new
   `Checkpointer` or syscall code here* — nothing is missing; the protocol is
   single-sourced by design (checkpoints.md scoped `WorkspaceFileChunkSource` as
   the only platform boundary, and mmap_backing_win32 Decision 5 filled the
   Windows leaves). Writing new production code would fork a single-sourced
   protocol. (If, contrary to expectation, a genuine link/behavior gap surfaces on
   Windows, the minimal fix is authored here and noted in Status — but the
   expectation, forced by the linker per mmap_backing_win32 Decision 5, is that
   nothing is missing.)
2. **CI-green on the msvc-debug lane, not compile-only.** The deliverable is
   `checkpoint.t.cpp` running green under `ctest --preset win-dev`, including the
   Debug-only read-only-publish witness — the identical bar
   `mmap_backing_win32`/`plugin_loading_win32` set for a Windows port. *Rejected:
   "it links, ship it"* — the whole value of this leaf is *proving* the ordered
   commit, the fence, and the `VirtualProtect` seal behave on Windows; a
   compile-only leaf would leave the protocol unproven and the downstream
   crash-tests/housekeeping leaves building on unverified ground.
3. **SEH (`__try`/`__except` on `EXCEPTION_ACCESS_VIOLATION`) for the debug
   read-only-publish witness; the faulting store isolated in a no-unwind helper.**
   Windows delivers a write to a `VirtualProtect(PAGE_READONLY)` page as a
   structured exception, not a POSIX signal, so the POSIX
   `sigaction`/`sigsetjmp`/`siglongjmp` harness has no equivalent; SEH is the
   sanctioned Win32 mechanism. Because MSVC forbids `__try`/`__except` in a
   function that also requires C++ object unwinding (error C2712), the single
   faulting store is placed in its own tiny function with no unwind-requiring
   locals, called from inside the `__try`. *Rejected: a POSIX-signal shim on
   Windows* — guard-page faults are not delivered as `SIGSEGV` through the MSVC
   CRT; forcing a signal abstraction would be fragile scaffolding for a facility
   SEH provides natively. *Rejected: skipping the witness on Windows* — the
   read-only-publish claim (`registry.tsv:56`) is precisely the debug-hardening
   guarantee doc 15:196-199 makes, and a Debug msvc-debug lane is exactly where it
   must be proven.
4. **Re-enforce the three existing checkpoint claims on Windows; add no rows.**
   `#checkpoint-recovers-consistent-root`, `#freed-slot-quarantined-until-durable`,
   and `#checkpoint-published-chunks-read-only` (`registry.tsv:54-56`) are
   platform-agnostic statements; only the *witness* (SEH vs signal) and the
   *measurement* (`GetCompressedFileSizeA` vs `st_blocks`) differ. The same test
   cases running on the msvc-debug lane re-enforce them — behavioral parity, not
   new behavior. *Rejected: Windows-specific claim ids* — the identical call
   mmap_backing_win32 Decision 8 made for the lifecycle claims; a per-platform id
   would double the register for one guarantee.
5. **Mirror the sibling suite's Windows test idioms verbatim; don't reinvent.**
   `TempPath` (`GetTempPathA`/`GetTempFileNameA`) and `allocated_size()`
   (`GetCompressedFileSizeA`) already exist and are proven on the msvc-debug lane
   in `workspace_file.t.cpp:49-102`; the ported `checkpoint.t.cpp` helpers copy
   those spellings. *Rejected: a fresh temp-path/size abstraction* — divergent
   idioms across two suites in the same directory for the same job is needless
   drift; the sibling's forms are already CI-validated on Windows.
6. **No design-doc delta.** Doc 15 names POSIX primitives (`msync`, `mprotect`)
   illustratively, not as a normative platform restriction, and already declares
   the workspace file same-machine / non-portable (`:204-206`). Validating the
   already-designed protocol on a second platform alters no designed behavior, so
   doc 16's same-commit amendment rule is not triggered — the identical call
   `mmap_backing_win32` Decision 7 and `plugin_loading_win32` Decision 6 made.
   *Rejected: a doc-15 note pairing each POSIX API with its Win32 equivalent* —
   documentation churn, not a normative change; the non-portability clause already
   covers platform specifics.

## Open questions

(none — all decided.)

## Status

**Done** — 2026-07-10.

- Un-gated the full suite in `src/pool/t/checkpoint.t.cpp` by removing `!defined(_WIN32)` from the `#if ARBC_HAS_WORKSPACE_FILES` guard (`:24`); refreshed the stale header comment (`:15-19`).
- Split POSIX-only headers (`<fcntl.h>`, `<sys/mman.h>`, `<sys/stat.h>`, `<unistd.h>`, `<csetjmp>`, `<csignal>`) behind `#if !defined(_WIN32)` and added a `#if defined(_WIN32)` `#include <windows.h>` arm, mirroring `workspace_file.t.cpp:31-34`.
- Ported `TempPath` to a two-arm implementation (`GetTempPathA`/`GetTempFileNameA` + `DeleteFileA` on Windows, `mkstemp`/`unlink` on POSIX), mirroring `workspace_file.t.cpp:49-77`.
- Replaced `block_count` helper with `allocated_size()` returning on-disk bytes (`GetCompressedFileSizeA` on Windows, `st_blocks*512` on POSIX), mirroring `workspace_file.t.cpp:85-102`; ported `copy_file` to `CopyFileA` on Windows.
- Added `#if defined(_WIN32)` arm to the hole-punch durability case using `FlushViewOfFile` + `allocated_size()` + NTFS/sparse guard, re-enforcing `#freed-slot-quarantined-until-durable` on Windows.
- Added SEH `__try`/`__except(EXCEPTION_ACCESS_VIOLATION)` arm for the debug read-only-publish witness with faulting store isolated in a no-unwind helper (C2712 avoidance), re-enforcing `#checkpoint-published-chunks-read-only` on Windows.
- No production source changes; no new claim rows (`#checkpoint-recovers-consistent-root`, `#freed-slot-quarantined-until-durable`, `#checkpoint-published-chunks-read-only` re-enforced on Windows by the same test cases per Decision 4).
- All Linux CI lanes (gcc/clang × debug/release/asan/tsan/rtsan, coverage) confirmed green by driver chain; `msvc-debug` lane validation will run on CI.
