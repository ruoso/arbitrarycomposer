# crash_tests_win32 — crash-recovery test sweep, Windows validation

## TaskJuggler entry

Back-link: [`tasks/05-pool.tji:85-89`](../../05-pool.tji), under the `pool`
umbrella:

> ```
> task crash_tests_win32 "Crash-recovery test sweep — Windows port" {
>   effort 1d
>   allocate team
>   depends !crash_tests, !checkpoints_win32
>   note "Port the in-process durable-snapshot sweep + disk-full/short-file cases to the Win32 workspace backing (VirtualProtect/MapViewOfFile fault injection; CreateProcess/TerminateProcess for the kill sweep), behind ARBC_HAS_WORKSPACE_FILES. Source-of-debt: tasks/refinements/pool/crash_tests.md. Doc 16."
> }
> ```

This task feeds milestone **M9 — "v0.1 release"** (`m9_release`,
`tasks/99-milestones.tji:69-73`), whose `depends` list **already names**
`pool.crash_tests_win32` (`tasks/99-milestones.tji:71`), alongside the sibling
Windows-parity leaves `pool.mmap_backing_win32`, `pool.checkpoints_win32`, and
`runtime.housekeeping_win32`. No milestone wiring is needed — the closer lands
`complete 100` after the `allocate team` line, appends
`Refinement: tasks/refinements/pool/crash_tests_win32.md` to the note, and — if
every other `m9_release` dependency is complete — propagates `complete 100` to
the milestone (milestones don't infer completion,
`tasks/refinements/README.md:69-72`).

## Effort estimate

**1 day** (`tasks/05-pool.tji:86`).

The seam this sweep drives is already cross-platform and the two sibling suites
already ship every Windows test idiom the port needs but one. `pool.crash_tests`
(DONE 2026-07-04) built the fault-injection harness against a **platform-neutral
`SyscallInjector`/`WorkspaceSyscall`** seam (`workspace_file.hpp:132-172`);
`pool.mmap_backing_win32` (DONE 2026-07-10) authored the Windows `io_*` shim
bodies that route through that same seam (`SetLastError` in place of `errno`,
`workspace_file.cpp` Win32 arms); `pool.checkpoints_win32` (DONE 2026-07-10)
proved the checkpoint protocol green on `msvc-debug` and ported the shared test
idioms (`TempPath` via `GetTempPathA`, `copy_file` via `CopyFileA`,
`allocated_size` via `GetCompressedFileSizeA`, the SEH access-violation witness).
So the in-process durable-snapshot sweep, the disk-full sections, and the
short/truncated/corrupt-file sweep are **helper-porting only** — un-gate the body
and branch the POSIX file surgery to the Win32 spellings the siblings already
established. The one genuinely-new piece is the **Windows process-kill sweep**:
`fork`/`_exit`/`waitpid` has no Windows analog anywhere in `src/`, so the
faithful mid-syscall-death model ports to a `CreateProcess` re-exec child-mode
harness with self-`ExitProcess` at syscall N.

## Inherited dependencies

**Settled:**

- **`pool.crash_tests`** (DONE 2026-07-04, `tasks/refinements/pool/crash_tests.md`,
  this task's `depends !crash_tests` edge). It produced the POSIX suite this task
  validates on Windows, and named this leaf verbatim in its Acceptance-criteria
  deferral (`crash_tests.md:252-259`): "port the in-process durable-snapshot
  sweep plus the disk-full/short-file cases to the Win32 workspace backing
  (`VirtualProtect`/`MapViewOfFile` fault injection; `CreateProcess`/
  `TerminateProcess` for the kill sweep in place of `fork`/`_exit`), behind
  `ARBC_HAS_WORKSPACE_FILES`. Wire into milestone **m9_release**." Its artifacts:
  - `src/pool/t/crash_tests.t.cpp` — the suite to bring green, gated
    `#if ARBC_HAS_WORKSPACE_FILES && !defined(_WIN32)` (`:24`, closing `#endif`
    `:751`). Its structure (Inputs below): the platform-neutral `SnapshotInjector`
    / `ErrnoInjector` / `assert_recovers` / `build_graph` / `walk`, the POSIX-only
    `TempPath` / `copy_file` / capture-and-patch file I/O, and the POSIX-only
    `fork`/`_exit` kill machinery.
  - The `SyscallInjector` seam (`workspace_file.hpp:132-172`, `set_syscall_injector`
    `:250`) and the three claims it enforces — `checkpoint-recovers-consistent-root`
    (`registry.tsv:54`), `freed-slot-quarantined-until-durable` (`:55`),
    `workspace-io-faults-surface-as-values` (`:57`) — re-enforced on Windows by the
    same test cases (Decision 4).
- **`pool.checkpoints_win32`** (DONE 2026-07-10,
  `tasks/refinements/pool/checkpoints_win32.md`, this task's `depends
  !checkpoints_win32` edge). It produced every seam and idiom this sweep rides on
  Windows:
  - A **CI-green** checkpoint protocol on `msvc-debug` — the ordered
    `FlushViewOfFile`+`FlushFileBuffers` commit, the durability-epoch fence, and
    recovery — so the sweep drives a *validated* protocol, not merely a linking one
    (`checkpoints_win32.md:307-313`). `pool.crash_tests_win32` is the leaf its
    Status names as unblocked by its landing.
  - The Windows test idioms in the sibling suites this task mirrors verbatim:
    `TempPath` (`GetTempPathA`/`GetTempFileNameA`/`DeleteFileA`,
    `checkpoint.t.cpp:50-65`, `workspace_file.t.cpp:52-72`); `copy_file` via
    `CopyFileA` (`checkpoint.t.cpp:189-192`); `allocated_size` via
    `GetCompressedFileSizeA` (`workspace_file.t.cpp:88-92`); the SEH
    `EXCEPTION_ACCESS_VIOLATION` witness with a no-unwind helper
    (`checkpoint.t.cpp:579-596,628-631`).
- **`pool.mmap_backing_win32`** (DONE 2026-07-10,
  `tasks/refinements/pool/mmap_backing_win32.md`). It flipped
  `ARBC_HAS_WORKSPACE_FILES` to `1` on `_WIN32` (`workspace_file.hpp:20-24`) and
  authored the Windows `io_*` shim bodies that keep the injector dispatch live:
  each Win32 shim consults `d_injector->before(kind, …)` → real call →
  `after(…)`, and on an injected non-zero return calls `SetLastError(value)` so
  `sys_error()` captures it into `sys_errno` via `GetLastError()`
  (`workspace_file.cpp` Win32 arms: `io_ftruncate` `:116-124`, `io_mmap`
  `:150-158`, `io_fallocate` `:190-201`, `io_msync` `:224-229`, `io_mprotect`
  `:247-254`; `sys_error()` Win32 arm `:76-78`). It also shipped the
  **`WinErrorInjector`** that injects `ERROR_DISK_FULL` at a chosen
  `WorkspaceSyscall` (`workspace_file.t.cpp:329-347`) — the exact disk-full model
  this task's `ErrnoInjector` sections port to.
- **The Windows CI lane already exists** — `msvc-debug` on `windows-latest` with
  the `win-dev` preset (`.github/workflows/ci.yml:60`;
  `CMakePresets.json:70-75,94,105`), a **Debug** build. As
  `mmap_backing_win32`/`checkpoints_win32` established, this makes the deliverable
  **CI-green, not compile-only**.

**Pending:** none. Every seam this task exercises is landed and Windows-green.

## What this task is

Bring the crash-recovery sweep green on the `msvc-debug` CI lane by un-gating
`src/pool/t/crash_tests.t.cpp` off `_WIN32` and porting its POSIX-only harness to
the Windows spellings the sibling suites already established — proving the
Windows workspace backing (`MapViewOfFile`/`FlushViewOfFile`/`SetEndOfFile`/
`FSCTL_SET_ZERO_DATA`) recovers a consistent root after every injected death, and
that every syscall failure and truncated file surfaces as a `WorkspaceFileErrc`
value. Concretely:

- **(a) Restructure the gate into a two-tier platform split.** Change
  `crash_tests.t.cpp:24` from `#if ARBC_HAS_WORKSPACE_FILES && !defined(_WIN32)`
  to `#if ARBC_HAS_WORKSPACE_FILES`, then split the POSIX-only headers (`:26-30`:
  `<fcntl.h>`, `<sys/mman.h>`, `<sys/stat.h>`, `<sys/wait.h>`, `<unistd.h>`)
  behind `#if !defined(_WIN32)` and add a `#if defined(_WIN32)`
  `#include <windows.h>` arm (`WIN32_LEAN_AND_MEAN`/`NOMINMAX`), exactly as
  `checkpoint.t.cpp:26-29` and `workspace_file.t.cpp:31-34` do. Refresh the stale
  header comment (`:15-19`) that says the body "stays gated off `_WIN32`."
- **(b) Port the shared harness helpers to the Windows idioms the siblings
  established.** All live in the anonymous namespace and are currently POSIX-only:
  - `TempPath` (`:36-58`, `mkstemp`/`/tmp`/`::unlink`) → the two-arm
    `GetTempPathA`/`GetTempFileNameA` + `DeleteFileA` version at
    `checkpoint.t.cpp:50-65` / `workspace_file.t.cpp:52-72`.
  - `copy_file` (`:140-158`, `::open`/`::read`/`::write`) → `CopyFileA` on Windows
    (`checkpoint.t.cpp:189-192`).
  - `SnapshotInjector::capture()` (`:229-248`) patches the two durable root slots
    into the copied file with `::open`/`::pwrite`/`offsetof` → on Windows,
    `CreateFileA` + `SetFilePointerEx` + `WriteFile` at
    `offsetof(WorkspaceHeader, root_slot_a/root_slot_b)` (`offsetof` is
    platform-neutral; only the file write changes). The injector's enum-routing
    `before`/`after` logic (`:188-227`) is already platform-neutral.
- **(c) Port the disk-full sections via `WinErrorInjector`.** The three
  cross-platform sections (`:463` ftruncate, `:475` mmap, `:486` msync) inject an
  errno through the `ErrnoInjector` and assert `sys_errno` + the `WorkspaceFileErrc`
  code. On Windows the injected value is a **Win32 error code, not an errno**
  (`workspace_file.cpp` feeds `before()`'s return to `SetLastError`), so replace
  `ErrnoInjector`/`ENOSPC`/`EFBIG`/`EIO` with the sibling's `WinErrorInjector`
  injecting `ERROR_DISK_FULL` / `ERROR_HANDLE_DISK_FULL` / `ERROR_IO_DEVICE`, and
  assert `last_error().sys_errno == ERROR_DISK_FULL` (etc.) with the same
  `GrowFailed` / `HeaderIoFailed` codes. The Linux-only fallocate-drain section
  (`:499-515`, `#if defined(__linux__)`) gains a `#if defined(_WIN32)` arm
  injecting on `WorkspaceSyscall::Fallocate` (the `FSCTL_SET_ZERO_DATA` punch,
  routed through `punch_now`'s Win32 arm), NTFS/sparse-guarded per
  mmap_backing_win32 Decision 4.
- **(d) Port the short/truncated/corrupt-file sweep's file surgery.** The
  section (`:528`) builds broken files with `::truncate` (`:553`) and
  `::open`/`::pwrite`/`offsetof` (`:587-624`); branch those to `SetFilePointerEx`
  + `SetEndOfFile` (truncation) and `CreateFileA` + `WriteFile` at offset
  (magic/format/root-slot clobbers). The recovery assertions
  (`read_header`/`Checkpointer::open` → `HeaderIoFailed`/`BadMagic`/
  `UnsupportedFormat`, and the torn-inactive-slot A/B redundancy case) are
  platform-neutral: the Windows `read_header` (`ReadFile`) and the
  `GetFileSizeEx` short-file guard already surface the same codes.
- **(e) Port the fork-and-kill faithful sweep to a `CreateProcess` re-exec
  child-mode harness** (the one genuinely-new piece — Decision 3). Replace
  `fork`/`waitpid` (`:697-724`) with: the parent `CreateProcess`-es the test
  binary in a **child mode** (a dedicated hidden `[.crashchild]` test selected by
  name, carrying `path`/`target`/`phase` through environment variables), waits on
  it (`WaitForSingleObject` + `GetExitCodeProcess`), then reopens the real file
  and asserts recovery lands on a consistent root — old root (gen 1) until the
  second commit's flip, new root (gen 2) after — never a crash or OOB read. The
  child runs `child_workload` with a `KillInjector` whose Windows arm calls
  `::ExitProcess(0)` (or `::TerminateProcess(::GetCurrentProcess(), 0)`) at
  syscall N — the direct analog of the POSIX `::_exit(0)` (`:644,:651`), running
  no C++ destructors so the crash is faithful. The bounded subset runs per-push
  `[pool]`; the exhaustive N-sweep runs `[.nightly]`.

Everything else is already platform-neutral and needs no change: `GraphNode` /
`build_graph` / `walk` / `assert_recovers` (`:63-136,291-307`, index-only
pointer-free records — doc 15 position independence), the `SnapshotInjector`
enum-routing, the `ErrnoInjector` core, `run_second_commit` (`:314-353`), and the
behavioral-counter coverage-completeness assertion (`:347`).

**Not this task:**

- **Any change to the checkpoint protocol, the workspace layout, the injector
  seam, or the backing policy** — those are `pool.checkpoints[_win32]` and
  `pool.mmap_backing[_win32]`; this task validates a second platform, it amends
  no production code (Decision 1).
- **A separate `crash_tests_win32.t.cpp` file or CMake change** — the port is
  in-file `#if defined(_WIN32)` arms, matching every sibling Windows port
  (Decision 2); `crash_tests.t.cpp` is already wired at
  `src/pool/CMakeLists.txt:13`.
- **A design-doc delta** — doc 15's POSIX API names are illustrative and its
  same-machine non-portability clause already covers platform specifics
  (Decision 6).

## Why it needs to be done

On Windows today `crash_tests.t.cpp`'s body is compiled out (`:24`), so the
crash-recovery discipline doc 16 makes this suite's charter
(`docs/design/16-sdlc-and-quality.md:74-78`) is **entirely unproven** on the
Windows backing: no test confirms that `MapViewOfFile`/`FlushViewOfFile`/
`SetEndOfFile` recovery lands on a consistent root after a mid-syscall death,
that an injected `ERROR_DISK_FULL` on chunk growth surfaces as
`WorkspaceFileErrc::GrowFailed` rather than an abort, or that a truncated Windows
file recovers to its last durable root rather than reading past EOF. M9's
headline is a v0.1 release with Windows parity; `pool.crash_tests_win32` is one
of its listed dependencies (`tasks/99-milestones.tji:71`), so M9 cannot close
without it, and the crash-recovery tier's promise
(`docs/design/15-memory-model.md:142-145`, "map the file, read the last valid
root, resume … an editor crash costs at-most-since-last-checkpoint") stays
POSIX-only proven.

## Inputs / context

### Design docs (normative, doc 16)

- **doc 15 — Memory model** (`docs/design/15-memory-model.md`):
  - `:142-145` — recovery promise: "map the file, read the last valid root,
    resume." Proven on Windows unchanged (recovery is platform-neutral C++ over
    the Windows-mapped file).
  - `:183-187` — ordered commit: "msync data chunks, then publish the root by
    flipping an A/B root slot in the header, then msync the header. A crash lands
    on the old or new root, both consistent." On Windows the msyncs are
    `FlushViewOfFile`+`FlushFileBuffers`; the ordering invariant is identical and
    is what the commit-ordering kill sweep pins.
  - `:187-191` — durability-epoch quarantine of freed slots; the freed-slot
    crash case re-enforces it on Windows (the fence is platform-neutral C++).
  - `:196-199` — debug `mprotect` read-only publish → `VirtualProtect` on Windows
    (`io_mprotect`, `workspace_file.cpp:247-254`); the sweep must not fight this
    hardening.
  - `:204-206` — "workspace files are same-machine artifacts (native endianness
    and padding, no portability promise)." A Windows-created file is only ever
    recovered on Windows, so the Win32 error codes and coarser
    allocation-granularity alignment mmap_backing_win32 chose are internally
    consistent.
- **doc 16 — SDLC & quality** (`docs/design/16-sdlc-and-quality.md`):
  - `:74-78` — the crash-recovery tier (this task's charter): "a fault-injection
    shim … drives kill-at-every-syscall sweeps … after each injected death, remap
    and verify a consistent root … Disk-full and short-file paths included."
  - `:227-229` — fault injection is the sanctioned mocking exception ("mocks are
    reserved for fault injection (I/O shim, allocator hooks)").
  - `:102-105` — per-push runs tiers short-form; "Nightly: long-form
    stress/fuzz/crash-sweeps" — the exhaustive kill sweep's cadence.
  - `:112-118` — ≥90% diff-coverage hard gate; `GCOV_EXCL` needs justification.
- **doc 17 — Internal components** (`docs/design/17-internal-components.md:41-61`)
  — `arbc::pool` is **Level 1**, depends only on `arbc::base`; `<windows.h>`/
  kernel32 is a platform facility invisible to `scripts/check_levels.py` — no new
  `arbc_*` edge.

### Source seams

- `src/pool/arbc/pool/workspace_file.hpp` — the platform-neutral fault-injection
  seam: `WorkspaceSyscall` enum (`:132-139`), `SyscallInjector` (`:154-172`),
  `set_syscall_injector` (`:250`), `io_*` shim declarations (`:273-277`), the
  Win32-only `d_fd`/`d_mapping` state (`:297-299`), `WorkspaceFileHandle` alias
  (`:105-109`), `WorkspaceFileErrc` (`:36-45`), `last_error()`.
- `src/pool/workspace_file.cpp` — the Windows `io_*` shim bodies that keep the
  injector dispatch live and the injected value → `SetLastError` → `sys_errno`
  round-trip: `io_ftruncate` (`:116-124`), `io_mmap` (`:150-158`), `io_fallocate`
  (`:190-201`), `io_msync` (`:224-229`), `io_mprotect` (`:247-254`),
  `sys_error()` Win32 arm (`:76-78`), `punch_now` Win32 arm
  (`FSCTL_SET_ZERO_DATA`), `read_header`/`open` Windows recovery (`ReadFile` +
  `GetFileSizeEx` short-file guard).
- `src/pool/arbc/pool/checkpoint.hpp` — the platform-neutral counters the
  coverage-completeness assertion reads: `commit_count()` (`:210`),
  `data_msyncs()` (`:211`), `header_msyncs()` (`:212`); the fence
  (`slot_fence()`) the freed-slot case installs.
- `src/pool/t/crash_tests.t.cpp` — the suite to bring green: gate (`:24`), stale
  comment (`:15-19`), POSIX headers (`:26-30`), `TempPath` (`:36-58`), `copy_file`
  (`:140-158`), `SnapshotInjector` (`:170-261`, `capture()` `:229-248`),
  `ErrnoInjector` (`:265-286`), `assert_recovers` (`:291-307`), `run_second_commit`
  (`:314-353`); test cases: commit-ordering kill sweep (`:358`), freed-slot survives
  crash (`:378`), disk-full (`:447`, sections `:463/:475/:486/:499-515`), short/
  truncated/corrupt (`:528`, sections `:557-627`), `KillInjector` (`:637-660`),
  `child_workload` (`:665-688`), `fork_kill_and_check` (`:697-724`), bounded fork
  per-push (`:729`), exhaustive nightly (`:740`).
- `src/pool/t/workspace_file.t.cpp` — the Windows idioms to mirror: `TempPath`
  (`:52-72`), `allocated_size` (`:88-92`), and **`WinErrorInjector`** injecting
  `ERROR_DISK_FULL` (`:329-347`) with the disk-full `#if !defined(_WIN32)` RLIMIT
  / `#else` injector split (`:325-366`).
- `src/pool/t/checkpoint.t.cpp` — `copy_file` via `CopyFileA` (`:189-192`),
  `TempPath` Win32 arm (`:50-65`), the SEH access-violation witness with the
  no-unwind helper (`:579-596,628-631`).
- `src/pool/CMakeLists.txt:11-14` — `crash_tests.t.cpp` is **already** wired into
  `arbc_component_test(COMPONENT pool …)` (`:13`); **no CMake change**.

### CI / build

- `.github/workflows/ci.yml:60` — the `msvc-debug` / `windows-latest` / `win-dev`
  lane (a **Debug** build, so `#ifndef NDEBUG` hardening is live); `:92-108` — the
  Linux-only `coverage` lane with the `diff-cover --fail-under=90` gate.
- `CMakePresets.json:70-75,94,105` — the `win-dev` configure/build/test presets
  (Ninja; MSVC dev prompt; Debug config).

### Tests / claims

- `tests/claims/registry.tsv:54` —
  `15-memory-model#checkpoint-recovers-consistent-root`; `:55` —
  `#freed-slot-quarantined-until-durable`; `:57` —
  `#workspace-io-faults-surface-as-values`. Re-enforced on Windows by the same
  test cases; **no new rows** (Decision 4).

### Predecessor / sibling refinements

- `tasks/refinements/pool/crash_tests.md` — the POSIX suite design, its Decisions
  (installable `SyscallInjector` over LD_PRELOAD, two death models split by CI
  cadence, errors-as-values), and the Status block naming this leaf.
- `tasks/refinements/pool/checkpoints_win32.md` — the Windows-port house style:
  un-gate + port helpers, single `#if defined(_WIN32)` seam, CI-green not
  compile-only, SEH witness, re-enforce claims with no new rows, no design-doc
  delta.
- `tasks/refinements/pool/mmap_backing_win32.md` — the Windows backend + the
  `WinErrorInjector` disk-full idiom (its Decision 4, best-effort
  `FSCTL_SET_ZERO_DATA`).

## Constraints / requirements

1. **Test-porting/validation only — no production source change.** The
   `SyscallInjector`/`WorkspaceSyscall` seam is single-sourced and platform-neutral
   and its Windows `io_*` bodies already exist and route through it; the missing
   artifact is the sweep's *proof* on Windows. If — contrary to expectation — a
   genuine link/behavior gap surfaces (e.g. an injected value not round-tripping to
   `sys_errno`), the minimal fix is authored here and noted in Status, mirroring
   `checkpoints_win32.md` Decision 1's fallback clause.
2. **CI-green on the `msvc-debug` lane, not compile-only.** The deliverable is
   `crash_tests.t.cpp` running green under `ctest --preset win-dev`, including the
   Debug-only `VirtualProtect`/`mprotect` interactions.
3. **Errors surface as values, never abort.** Every injected Windows syscall
   failure surfaces through `WorkspaceFileErrc` / `last_error()` / `arbc::expected`;
   the harness asserts the exact code (`GrowFailed`/`HeaderIoFailed`/`BadMagic`/
   `UnsupportedFormat`) and the platform-appropriate `sys_errno` (a **Win32 error
   code** on Windows), and that a subsequent reopen still recovers the last durable
   root. No injected fault reaches an `abort`/UB.
4. **The kill sweep must faithfully die mid-syscall.** The Windows child self-kills
   via `ExitProcess`/`TerminateProcess(GetCurrentProcess())` at syscall N (no C++
   unwinding), the direct analog of POSIX `_exit(0)`; the parent reaps
   (`WaitForSingleObject`) and reopens the real file. Determinism is preserved —
   the sweep is exhaustive over syscall index N, not randomized; no wall-clock
   assertions.
5. **Injector/fence seams stay live and untouched.** This task adds no production
   code; the `SyscallInjector`/`WorkspaceSyscall` enumeration and the
   `ChunkReleaseFence`/durability-epoch fence remain exactly as
   `crash_tests`/`mmap_backing_win32` left them.
6. **No new levelization edge, no CMake dependency delta.** The suite stays in the
   `arbc::pool` (L1) test target over `base`; `<windows.h>`/kernel32 is a platform
   facility, invisible to `scripts/check_levels.py`. `crash_tests.t.cpp` is already
   in the test target; no CMake change.
7. **No regression on POSIX lanes.** The `#if !defined(_WIN32)` / `#if
   defined(_WIN32)` branching keeps the POSIX arms behaviorally identical; all
   Linux lanes (gcc/clang/asan/tsan/rtsan/coverage) stay green, and the
   `#if defined(_WIN32)` branches are compiled out of the Linux coverage TU
   (neither raising nor lowering the diff-cover gate).
8. **CI cadence honored (doc 16:102-105).** The exhaustive `CreateProcess`-and-kill
   N-sweep is a nightly `[.nightly]` job; the bounded subset (a handful of early
   syscall boundaries + the fixed disk-full/short-file cases + the in-process
   snapshot sweep) runs per-push `[pool]`. The nightly-only breadth stays
   comment-documented so it does not read as "covered everything."

## Acceptance criteria

- **`msvc-debug` CI lane goes green (headline).** After the change, the
  `windows-latest` / `win-dev` lane (`.github/workflows/ci.yml:60`) configures,
  builds `arbc_pool` + its tests with the Windows workspace backend, and
  `ctest --preset win-dev` **passes** `src/pool/t/crash_tests.t.cpp` — the
  in-process commit-ordering kill sweep, the freed-slot-survives-crash case, the
  disk-full sections, the short/truncated/corrupt-file sweep, and the bounded
  `CreateProcess`-and-kill subset. The `crash-test harness tracks workspace-file
  support` case (`:20-22`) now takes the *supported* branch on Windows.
- **`15-memory-model#checkpoint-recovers-consistent-root` enforced on Windows**
  (`registry.tsv:54`). The `commit-ordering kill sweep: old root before the header
  sync, new root after` case (`:358`) passes on `msvc-debug`, proving
  `FlushViewOfFile`+`FlushFileBuffers` gives crash-consistent ordered commit; the
  bounded `CreateProcess`-and-kill subset (`:729`, ported) re-enforces it under a
  real process death. **No new claim row.**
- **`15-memory-model#freed-slot-quarantined-until-durable` enforced on Windows**
  (`registry.tsv:55`). The `freed slot survives a crash before the freeing is
  durable` case (`:378`) passes: after an in-process death before the header flush,
  the reopened durable root still references the freed slot's record (value 42, not
  reused), the durability-epoch fence having survived the crash. **No new claim
  row.**
- **`15-memory-model#workspace-io-faults-surface-as-values` enforced on Windows**
  (`registry.tsv:57`). The disk-full sections (`:463/:475/:486`, ported to
  `WinErrorInjector`) assert an injected `ERROR_DISK_FULL`/`ERROR_IO_DEVICE`
  surfaces as `WorkspaceFileErrc::GrowFailed`/`HeaderIoFailed` with `sys_errno`
  set (the Win32 code), no abort, and the file reopens to its last durable root;
  the short/truncated/corrupt-file sweep (`:528`, ported surgery) asserts a clean
  `HeaderIoFailed`/`BadMagic`/`UnsupportedFormat` value or recovery to the prior
  durable root — never a crash or OOB read — including the torn-inactive-root-slot
  A/B-redundancy case. **No new claim row.**
- **Coverage-completeness stays a behavioral-counter assertion (doc 16, never
  wall-clock).** The in-process sweep still derives its injection-point
  enumeration from `data_msyncs()` + the flip + `header_msyncs()` and asserts the
  intercept fired exactly as many times as the counters predict (`:347`) — the
  Windows `io_msync` body increments the same platform-neutral counters as POSIX.
- **`CreateProcess`-and-kill sweep faithful and exhaustive (nightly).** The
  `[.nightly]` exhaustive syscall sweep (`:740`, ported) spawns a re-exec child per
  syscall index N, self-kills at N via `ExitProcess`, and the parent reopens to a
  consistent root; overshooting N lets the child run to completion (full
  recovery). The nightly-only breadth is comment-documented (Constraint 8).
- **Concurrency (TSan/asan, explicit per doc 16).** This task introduces no new
  concurrency — the checkpoint protocol is writer-only and the child runs
  single-threaded. The suite runs clean under the asan-equivalent `msvc-debug`
  functional run; there is no MSVC TSan preset (same gap noted in
  `crash_tests.md`/`reclamation.md:305`; not re-litigated).
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
- **No deferred WBS leaves.** This task closes the crash-recovery-sweep
  *validation* on Windows in full and registers **no successor** — `m9_release`
  already names `pool.crash_tests_win32` (`tasks/99-milestones.tji:71`); this task
  simply completes it.

## Decisions

1. **Test-porting/validation only — no production source change.** The
   `SyscallInjector`/`WorkspaceSyscall` seam is platform-neutral by design
   (`workspace_file.hpp:132-172`), and `mmap_backing_win32` already authored the
   Windows `io_*` shim bodies that route through it and round-trip an injected
   value to `sys_errno` (`workspace_file.cpp:76-78,116-254`). So the sweep already
   compiles and links on Windows; only its *proof* is missing. The task is
   un-gating and porting the test's POSIX-only harness. *Rejected: authoring new
   injector or syscall code here* — the seam is single-sourced; writing new
   production code would fork it. (If a genuine gap surfaces on Windows, the
   minimal fix is authored here and noted in Status — Constraint 1's fallback, the
   identical call `checkpoints_win32` Decision 1 made.)
2. **Extend the existing `crash_tests.t.cpp` with in-file `#if defined(_WIN32)`
   arms; no separate `_win32` file, no CMake change.** Every landed Windows port in
   this tree (`checkpoint.t.cpp`, `workspace_file.t.cpp`, `housekeeping*.t.cpp`)
   handles Windows in-file, not via a parallel `_win32.t.cpp`; there are **no**
   `*_win32*` test files. `crash_tests.t.cpp` is already in the `arbc_component_test`
   list (`CMakeLists.txt:13`), so no CMake edit. *Rejected: a new
   `crash_tests_win32.t.cpp`* — it would duplicate the platform-neutral graph/walk/
   injector scaffolding and drift from the POSIX suite it mirrors; the sibling ports
   set the in-file precedent.
3. **The Windows kill sweep uses a `CreateProcess` re-exec child-mode harness with
   self-`ExitProcess` at syscall N — the faithful analog of `fork`+`_exit`.**
   Windows has no `fork`, so the child cannot inherit the parent's in-memory
   workload state; instead the parent `CreateProcess`-es the test binary in a
   dedicated hidden `[.crashchild]` test (selected by name), passing
   `path`/`target`/`phase` through environment variables, and the child runs
   `child_workload` with a `KillInjector` whose Windows arm calls `::ExitProcess(0)`
   (running no C++ destructors, exactly as POSIX `_exit`). The parent reaps with
   `WaitForSingleObject` and reopens. *Rejected: pure in-process
   `TerminateProcess(GetCurrentProcess())`* — it kills the Catch2 runner itself, so
   a sweep of N points is impossible without re-running the whole binary per N,
   which is what the re-exec harness does deliberately. *Rejected: dropping the
   process-kill sweep and shipping only the in-process snapshot model* — the
   in-process model never sees real OS page writeback after a real process death,
   so it would not honor doc 16:74-78's "kill-at-every-syscall" discipline on
   Windows, and the note explicitly names `CreateProcess`/`TerminateProcess`.
4. **Re-enforce the three existing crash-recovery claims on Windows; add no rows.**
   `#checkpoint-recovers-consistent-root`, `#freed-slot-quarantined-until-durable`,
   and `#workspace-io-faults-surface-as-values` (`registry.tsv:54,55,57`) are
   platform-agnostic statements; only the *witness* (`ExitProcess` vs `_exit`) and
   the *fault constant* (`ERROR_DISK_FULL` vs `ENOSPC`) differ. The same test cases
   on `msvc-debug` re-enforce them. *Rejected: Windows-specific claim ids* — the
   identical call `mmap_backing_win32`/`checkpoints_win32` Decision 4/8 made; a
   per-platform id doubles the register for one guarantee.
5. **Port the disk-full sections to `WinErrorInjector`/Win32 error codes, mirroring
   the sibling suite verbatim.** On Windows the injected value is fed to
   `SetLastError` and read back through `GetLastError` into `sys_errno`, so the
   test injects `ERROR_DISK_FULL`/`ERROR_IO_DEVICE` (not `ENOSPC`/`EIO`) and asserts
   `sys_errno` against the Win32 code — exactly the `WinErrorInjector` +
   `#if !defined(_WIN32)` RLIMIT / `#else` injector split already proven at
   `workspace_file.t.cpp:325-366`. *Rejected: asserting the POSIX errno on Windows*
   — the injected value is a Win32 error code there; asserting `ENOSPC` would fail.
   *Rejected: a fresh disk-full abstraction* — divergent idioms for the same job in
   the same directory is needless drift; the sibling's form is CI-validated on
   Windows.
6. **No design-doc delta.** Doc 15 names POSIX primitives (`msync`, `mprotect`,
   `fallocate`) illustratively, not as a normative platform restriction, and
   already declares the workspace file same-machine / non-portable (`:204-206`).
   Validating the already-designed crash-recovery discipline on a second platform
   alters no designed behavior, so doc 16's same-commit amendment rule is not
   triggered — the identical call `checkpoints_win32` Decision 6 and
   `mmap_backing_win32` Decision 7 made.

## Open questions

(none — all decided.)

## Status

**Done** — 2026-07-10.

- **Ported `src/pool/t/crash_tests.t.cpp`** — only file changed; no CMake, production, or claim-register change.
- **Gate re-structured**: top-level `#if ARBC_HAS_WORKSPACE_FILES && !defined(_WIN32)` changed to `#if ARBC_HAS_WORKSPACE_FILES`; POSIX-only headers moved behind `#if !defined(_WIN32)`; `#if defined(_WIN32) #include <windows.h>` arm added.
- **Shared helpers ported**: `TempPath` → `GetTempPathA`/`GetTempFileNameA`/`DeleteFileA`; `copy_file` → `CopyFileA`; `SnapshotInjector::capture()` file surgery → `CreateFileA`/`SetFilePointerEx`/`WriteFile`.
- **Disk-full sections**: reused the file's existing `ErrnoInjector` (injecting Win32 error codes instead of importing a structurally-identical `WinErrorInjector`) to inject `ERROR_DISK_FULL`/`ERROR_HANDLE_DISK_FULL`/`ERROR_IO_DEVICE`; asserts `sys_errno` against the Win32 code — substance of Decision 5 preserved, one fewer class.
- **Short/truncated/corrupt-file sweep**: file surgery ported to `SetFilePointerEx`/`SetEndOfFile` (truncation) and `CreateFileA`/`WriteFile` at offset (magic/format/root-slot clobbers).
- **Process-kill sweep**: `fork`/`_exit`/`waitpid` replaced by `CreateProcess` re-exec child (`[.crashchild]` hidden TEST_CASE dispatched by env vars) with `ExitProcess` at syscall N; parent reaps via `WaitForSingleObject`; bounded subset runs `[pool]` per-push, exhaustive N-sweep runs `[.nightly]`.
- **Coverage markers**: `GCOV_EXCL_START/STOP`/`GCOV_EXCL_LINE` added to the 11 lines that execute only in the `_exit`-ing crash child or the `[.nightly]` hidden case (never flushed by gcov), holding the `diff-cover --fail-under=90` gate.
