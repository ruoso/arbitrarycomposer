# crash_tests — crash-recovery test sweep

## TaskJuggler entry

`tasks/05-pool.tji` → `pool.crash_tests` ("Crash-recovery test sweep").
Task block at `tasks/05-pool.tji:67-72`; `depends !checkpoints`.

## Effort estimate

2d.

## Inherited dependencies

Settled:

- `pool.checkpoints` (commit `b1036d9`, `tasks/refinements/pool/checkpoints.md`)
  — the A/B-root ordered-commit protocol this sweep fault-injects:
  `Checkpointer::commit()` (`src/pool/arbc/pool/checkpoint.hpp:133-171`),
  `Checkpointer::open()` recovery (`:185-200`), `finalize_open()` (`:205-207`),
  the durability-epoch fence, and the behavioral counters
  `commit_count()` / `data_msyncs()` / `header_msyncs()` (`:209-212`).
- `pool.mmap_backing` (commit `22a1c71`,
  `tasks/refinements/pool/mmap_backing.md`) — `WorkspaceFileChunkSource`
  (`src/pool/arbc/pool/workspace_file.hpp:134`), the fd owner that issues
  every syscall the shim wraps, plus the `WorkspaceFileErrc` value channel
  (`:36-45`) and the existing `RLIMIT_FSIZE` disk-full test
  (`src/pool/t/workspace_file.t.cpp:229-231`) this task generalizes.
- `pool.reclamation` (commit `c5b828e`) and `pool.refs` (commit `3898878`) —
  the reclamation queue and inside-out counts that recovery rebuilds
  (`finalize_open` reachability walk); needed to assert a freed-but-not-yet-
  durable slot is not reused after a mid-commit death.

Pending: none — every seam this task exercises is landed.

## What this task is

Build the fault-injection harness the checkpoint protocol was designed to be
tested against, and drive it. Concretely: introduce an installable syscall
intercept over `WorkspaceFileChunkSource`'s I/O (`msync`, `mmap`, `ftruncate`,
`fallocate`, `pread`, `mprotect`), then run **kill-at-every-syscall sweeps**
through the ordered commit and through chunk growth/release — after each
injected death, remap the workspace file and verify recovery lands on a
consistent root (the old root before the header `msync`, the new root after,
each resolving a valid graph). Add the disk-full and short-file/truncated-file
paths: every syscall failure and every partially-written file surfaces as a
`WorkspaceFileErrc` value and remains recoverable to the last durable root,
never an abort or UB. This is the exhaustive generalization of the focused
two/three-boundary crash checks `pool.checkpoints` already pinned.

## Why it needs to be done

The checkpoint refinement deliberately deferred the shim: it pinned the
ordering invariant at the two/three commit boundaries directly but left "the
general `msync`/`write`/`mmap` intercept shim and the kill-at-every-syscall
sweep" to this task (`tasks/refinements/pool/checkpoints.md:376-384`), and
proved disk-full/short-file *handling* only "at focused injection points; the
exhaustive kill sweep is `pool.crash_tests`" (`:242-248`). The existing crash
tests simulate a crash by byte-copying the `msync`'d file at a clean boundary
and reopening (`copy_file`, `src/pool/t/checkpoint.t.cpp:148-153`) — they never
exercise death *mid-syscall*, which is exactly the gap here.

Doc 16's crash-recovery tier is this task's charter
(`docs/design/16-sdlc-and-quality.md:74-78`): "a fault-injection shim over
msync/write/mmap drives kill-at-every-syscall sweeps through the checkpoint
protocol; after each injected death, remap and verify a consistent root
(SQLite's discipline, scaled to our simpler protocol). Disk-full and
short-file paths included." Downstream, milestone **m1_memory**
(`tasks/99-milestones.tji:14-17`) is defined as "the mmapped workspace file
surviving kill-injection sweeps" — this task is the last leaf it gates on
(alongside `pool.benchmarks`), so M1 cannot close without it.

## Inputs / context

Design docs (normative — doc 16):

- `docs/design/15-memory-model.md:142-145` — recovery promise: "map the file,
  read the last valid root, resume. An editor crash costs at-most-since-last-
  checkpoint, not the document."
- `docs/design/15-memory-model.md:183-187` — ordered commit: "msync data
  chunks, then publish the root by flipping an A/B root slot in the header,
  then msync the header. A crash lands on the old or new root, both
  consistent."
- `docs/design/15-memory-model.md:187-191` — durability-epoch quarantine: a
  slot freed after the last durable checkpoint "may still be referenced by the
  on-disk root, so reclamation quarantines freed slots per durability epoch."
- `docs/design/15-memory-model.md:162-168` — runtime bookkeeping (refcounts,
  free pools, generation tags, reclamation queue) is "anonymous runtime state,
  rebuilt on open"; the recovery walk this task's sweep exercises.
- `docs/design/15-memory-model.md:196-199` — debug `mprotect` read-only of
  published chunks; the sweep must not fight this hardening.
- `docs/design/16-sdlc-and-quality.md:74-78` — the crash-recovery tier (this
  task's charter, quoted above).
- `docs/design/16-sdlc-and-quality.md:227-229` — fault injection is the
  explicitly sanctioned mocking exception: "mocks are reserved for fault
  injection (I/O shim, allocator hooks)."
- `docs/design/16-sdlc-and-quality.md:102-105` — per-push CI runs tiers
  short-form; "Nightly: long-form stress/fuzz/crash-sweeps."
- `docs/design/16-sdlc-and-quality.md:112-118` — ≥90% diff-coverage hard gate;
  `GCOV_EXCL` regions need a justification comment.
- `docs/design/17-internal-components.md:41-61` — levelization: `arbc::pool`
  is Level 1, depends only on `arbc::base`; the arrow set is complete and
  CI-validated.
- `docs/design/17-internal-components.md:14` and `:145-155` — `arbc-testing`
  is the *contract conformance suite* (unbuilt today); component unit tests
  live in `src/<component>/t/`, cross-component crash-recovery tests in
  `tests/`.

Source seams this task wraps and drives:

- `src/pool/arbc/pool/workspace_file.hpp:134` — `WorkspaceFileChunkSource
  final : public ChunkSource`, the single fd owner (`d_fd` at `:228`); the
  choke point the shim installs on. `set_release_fence(ChunkReleaseFence*)`
  at `:191` is the established nullptr-default installable-hook idiom to
  mirror.
- `src/pool/workspace_file.cpp` — raw `::`-qualified syscall sites, no
  existing indirection: `::ftruncate` (`:81`, `:163`), `::mmap` (`:86`,
  `:167`, `:273`, `:301`), `::fallocate(PUNCH_HOLE)` (`:215`), `::pread`
  (`:233`, `:257`), `::msync` (`:315` data, `:325` header), `::mprotect`
  (`:350`, `:368`). Root-slot access: `root_slot()` (`:332`) and the flip
  store `publish_root_slot()` (`:338-343`, "a single naturally-aligned 8-byte
  store").
- `src/pool/arbc/pool/checkpoint.hpp:133-171` — `commit()` ordering:
  `sync_data()` (`:135`) → `publish_root_slot()` new root into inactive slot
  (`:143`) → `sync_header()` (`:145`) → A/B flip (`:155`) → debug
  `protect_range` (`:162-165`) → `drain_fences()` (`:167`). `open()` recovery
  selecting highest valid generation (`:185-200`); `WorkspaceRoot{generation,
  root_index}` (`:38-50`). Counters at `:209-212`.
- `src/pool/arbc/pool/workspace_file.hpp:36-45` — `WorkspaceFileErrc`
  (`GrowFailed` incl. disk-full `:43`, `HeaderIoFailed` `:40`, `BadMagic`
  `:41`, `UnsupportedFormat` `:42`, `DirectoryFull` `:44`); `WorkspaceFileError
  {code, sys_errno}` (`:49-52`); `last_error()` (`:199`).
- `src/pool/t/checkpoint.t.cpp` — `copy_file` clean-boundary reopen idiom
  (`:148-153`), the `sigsetjmp`/SIGSEGV fault-trap harness for the mprotect
  claim (`:526-565`), the `TempPath` RAII helper, the concurrent-writer smoke
  (`:574`). `src/pool/t/workspace_file.t.cpp:229-231` — the `RLIMIT_FSIZE`
  disk-full test to generalize.
- `src/pool/CMakeLists.txt` — the `arbc_component_test(COMPONENT pool
  SOURCES …)` list to extend with `t/crash_tests.t.cpp`; Catch2 v3, auto-
  registered via `cmake/ArbcComponent.cmake:46-56`.

Predecessor decisions honored: `tasks/refinements/pool/checkpoints.md:104-108`
(this task builds the shim), `:242-248` (errors as values; focused disk-full
here, exhaustive sweep there), `:376-384` (shim is a distinct, larger
deliverable the WBS separates). `tasks/refinements/pool/reclamation.md:305`
(TSan lane gap, not re-litigated here).

## Constraints / requirements

- **Levelization.** The shim seam is a member of `WorkspaceFileChunkSource`
  and stays entirely within `arbc::pool` (L1 → `arbc::base` only). No new
  component, no new dependency edge, no reach up to `runtime` (L5). The test
  lives in `src/pool/t/crash_tests.t.cpp`, wired into the pool component test
  target — not in the unbuilt, contract-scoped `arbc-testing`.
- **POSIX guard.** The harness (fork/`_exit`, `RLIMIT_FSIZE`, truncation) sits
  under `#if ARBC_HAS_WORKSPACE_FILES`, matching the backing it tests; on
  non-POSIX it compiles to a skipped/empty translation unit, exactly as
  `workspace_file.cpp` does.
- **Errors as values, never abort.** Every injected syscall failure surfaces
  through `WorkspaceFileErrc` / `last_error()` / `arbc::expected`; the harness
  asserts the exact error code and that a subsequent reopen still recovers the
  last durable root. No injected fault may reach an `abort`/UB
  (`tasks/refinements/pool/checkpoints.md:242-248`).
- **Zero-cost seam when unset.** The intercept is a nullptr-default hook on
  rare, non-hot-path syscalls (chunk growth + commit, never per-slot); when no
  injector is installed the added cost is one predictable branch. It ships in
  all build configs (it is a first-class sanctioned I/O shim, doc 16:227-229),
  not `#ifdef`-gated on debug.
- **Determinism.** The sweep is *exhaustive over syscall index N*, not
  randomized — deterministic by construction, no seed needed. The in-process
  variant reopens from a captured durable snapshot; the fork variant reaps the
  child and reopens the real file. No wall-clock assertions anywhere.
- **No untested scaffolding (doc 16).** The seam exists only to be exercised;
  every intercept branch is hit by the sweep. Genuinely-unreachable defensive
  arms get `GCOV_EXCL` with a justification comment, not gaming.
- **CI cadence (doc 16:102-105).** The exhaustive fork-and-kill N-sweep is a
  nightly long-form job (Catch2 `[.nightly]` hidden tag); a bounded subset
  (the three commit-ordering boundaries + a fixed handful of disk-full/
  short-file cases) runs per-push short-form. The nightly-only breadth is
  `log`/comment-documented so it does not read as "covered everything."

## Acceptance criteria

**Unit / sweep tests** — new `src/pool/t/crash_tests.t.cpp` (Catch2 v3, added
to `src/pool/CMakeLists.txt`'s pool test sources):

- **Commit-ordering kill sweep.** For a workload of ≥2 commits, enumerate the
  injection points from the behavioral counters (`data_msyncs()` +
  `header_msyncs()` + the root-flip stores) and, at each point, inject a death
  both *before* and *after* the syscall's durable effect; reopen and assert:
  recovery lands on the **old** root before the header `msync`, the **new**
  root after it, and the resolved root always yields a valid graph
  (`decode_root` generation valid, reachable slots consistent). Assert the
  sweep visited every enumerated boundary (counter-derived count == injection
  count) — no silently skipped syscall.
- **Freed-slot durability under mid-commit death.** Free a slot after a
  checkpoint, begin a commit, kill before the header `msync` makes the freeing
  durable; reopen and assert the on-disk root still references the slot and
  the slot is not re-allocated nor hole-punched (the durability-epoch fence
  survived the crash).
- **Disk-full paths.** Inject `ENOSPC`/`EFBIG` on `ftruncate` / `mmap` /
  `msync` / `fallocate` (generalizing `workspace_file.t.cpp:229-231`); assert
  the call returns `WorkspaceFileErrc::GrowFailed` (or the matching code) with
  `sys_errno` set, no abort, and the file reopens to its last durable root.
- **Short-file / truncated-file paths.** Truncate the workspace file at
  boundaries (mid-header, mid-root-slot, mid-directory, mid-data-chunk) and at
  a torn root-slot generation; reopen and assert either a clean
  `WorkspaceFileErrc` value (`BadMagic` / `HeaderIoFailed` /
  `UnsupportedFormat` / `DirectoryFull`) or recovery to the prior durable
  root — never a crash or out-of-bounds read.
- **fork-and-kill faithful sweep** (`[.nightly]`): child runs the workload,
  the injector `_exit(0)`s the child at real syscall N (before/after the real
  call), parent reaps and reopens; exhaustive over N. Per-push runs the
  bounded subset.

**Claims register** (`tests/claims/registry.tsv`, `enforces:` tags in the
test, validated by `scripts/check_claims.py`):

- **Reuse — stronger witness.**
  `15-memory-model#checkpoint-recovers-consistent-root` (registry `:13`): add
  a second `enforces:` tag from the exhaustive commit-ordering sweep (the
  focused checkpoints test pinned the boundaries; this pins them under
  mid-syscall death).
- **Reuse — crash witness.**
  `15-memory-model#freed-slot-quarantined-until-durable` (registry `:14`): add
  an `enforces:` tag from the mid-commit-death freed-slot test.
- **New claim.** Register
  `15-memory-model#workspace-io-faults-surface-as-values` — "Every
  workspace-file syscall failure (grow / msync / hole-punch / header I/O) and
  every truncated or short file surfaces as a `WorkspaceFileErrc` value, never
  an abort or UB; the file remains recoverable to its last durable root."
  Enforced by the disk-full and short-file tests. This pins an existing
  design promise (doc 15:47 errors-as-values; doc 16:74-78 disk-full/short-
  file) that has no registered claim today — claims-register growth, not a
  doc change (no delta).

**Behavioral counters (doc 16, never wall-clock).** The sweep derives its
injection-point enumeration from `commit_count()` / `data_msyncs()` /
`header_msyncs()` and asserts the intercept fired exactly as many times as the
counters predict — the coverage-completeness check is itself a counter
assertion.

**Concurrency (TSan).** This task introduces no new concurrency — the
checkpoint protocol is writer-only and the fork sweep runs single-threaded
children. The suite runs clean under the asan lane; the TSan lane runs it
if/when present (this repo has no TSan preset today — same gap noted in
`tasks/refinements/pool/reclamation.md:305`; not re-litigated here).

**Coverage.** ≥90% diff coverage on the changed lines (the seam + harness);
the seam's every intercept arm is exercised by the sweep, so coverage is
intrinsic, not bolted on. The asan gate stays green.

**Deferred follow-up** (closer registers in WBS):
`pool.crash_tests_win32` (est. 1d, `depends !crash_tests, !checkpoints_win32`)
— port the in-process durable-snapshot sweep plus the disk-full/short-file
cases to the Win32 workspace backing (`VirtualProtect`/`MapViewOfFile` fault
injection; `CreateProcess`/`TerminateProcess` for the kill sweep in place of
fork/`_exit`), behind `ARBC_HAS_WORKSPACE_FILES`. Wire into milestone
**m9_release** (`tasks/99-milestones.tji:62-65`), where the other `_win32`
ports already live.

## Decisions

- **Fault-injection seam = an installable `SyscallInjector` hook on
  `WorkspaceFileChunkSource`, mirroring `set_release_fence`.** Route the six
  syscalls through one nullptr-default indirection member; default is a direct
  `::` call, so no behavior change when unset. This reuses the exact
  installable-hook idiom the pool already ships (`set_release_fence`,
  `workspace_file.hpp:191`) and keeps everything inside the one class that
  owns the fd. *Rejected:* `LD_PRELOAD` / weak-symbol interposition — process-
  global, cannot target one instance, breaks Catch2's parallel test binary,
  and gives no clean per-call errno/kill-count control. *Rejected:*
  subclassing `WorkspaceFileChunkSource` — the sync/protect helpers are
  non-virtual on a `final` class, so a subclass cannot intercept them.
  *Rejected:* a compile-time syscall-policy template parameter — it would
  template-explode the whole pool over a test-only axis, for calls rare enough
  that a runtime branch is free.
- **Two death models, split by CI cadence.** (a) In-process durable-snapshot
  reopen — fast, deterministic, per-push; a direct generalization of the
  landed `copy_file` reopen idiom. (b) fork-and-kill with real syscalls — the
  faithful SQLite-style model that exercises real kernel page writeback and
  partial `msync`; exhaustive N-sweep runs nightly. *Rejected:* fork-only —
  too slow for per-push and the exhaustive sweep is explicitly a nightly job
  (doc 16:102-105). *Rejected:* in-process-only — never sees a real partial
  `msync`, so it would not honor "SQLite's discipline" (doc 16:74-78).
- **Reuse the two landed claims as stronger witnesses; add exactly one new
  claim.** The sweep is the exhaustive witness for `checkpoint-recovers-
  consistent-root` and `freed-slot-quarantined-until-durable` (a claim may be
  enforced by more than one test). The disk-full/short-file "errors as values,
  never abort" promise has no claim today, so it gets one
  (`workspace-io-faults-surface-as-values`). *Rejected:* a fresh
  crash-consistency claim — it would duplicate `checkpoint-recovers-consistent-
  root`. *Rejected:* no new claim — the errors-as-values/short-file promise
  would then be untracked by the register.
- **Home in `src/pool/` + `src/pool/t/`, not `arbc-testing` or `tests/`.** The
  seam belongs with the fd-owning class (L1); `arbc-testing` is unbuilt and
  scoped to the *plugin contract* conformance suite (doc 17:14), and the
  cross-component `tests/` tree is for cross-component work (doc 17:145-155).
  A pool-internal I/O shim is neither. *Rejected:* placing the shim in
  `tests/` — it would have to reach into pool internals from above the
  levelization boundary.
- **Errors surface as values; the harness asserts the code and re-recovery.**
  Consistent with `checkpoints.md:242-248` and doc 15:47 — an injected fault
  is a `WorkspaceFileErrc`, and the invariant proven is that after the fault
  the file still reopens to its last durable root. *Rejected:* asserting only
  "no crash" — that would leave the recover-to-last-durable-root promise
  untested.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-04.

- `src/pool/arbc/pool/workspace_file.hpp` — added `WorkspaceSyscall` enum, `SyscallInjector` interface, `set_syscall_injector()` (mirroring `set_release_fence`), private `io_*` shim declarations, and `d_injector` member.
- `src/pool/workspace_file.cpp` — `io_*` shims (one predictable branch when unset), routed the instance-method syscalls (`ftruncate`/`mmap`/`fallocate`/`msync`/`mprotect`) + the root-flip store through them; added `fstat`/per-chunk truncation guards to `open()` so short/truncated files surface as `HeaderIoFailed` instead of SIGBUS.
- `src/pool/t/crash_tests.t.cpp` — new file: in-process durable-snapshot injector, errno injector, fork-kill injector + full workload (commit-ordering kill sweep, freed-slot-survives-crash-before-durable, disk-full errno injection, short/truncated/corrupt-file sweep, fork-and-kill bounded per-push `[pool]` + exhaustive `[.nightly]`).
- `src/pool/CMakeLists.txt` — added `t/crash_tests.t.cpp` to the pool component test sources.
- `src/pool/arbc/pool/checkpoint.hpp` — minor updates to support the sweep (counter/fence exposure).
- `src/pool/t/checkpoint.t.cpp` — added `enforces:` tags for `checkpoint-recovers-consistent-root` and `freed-slot-quarantined-until-durable` from the exhaustive mid-syscall-death tests.
- `tests/claims/registry.tsv` — new claim `15-memory-model#workspace-io-faults-surface-as-values`: "Every workspace-file syscall failure and truncated/short file surfaces as a `WorkspaceFileErrc` value, never abort or UB; the file remains recoverable to its last durable root."
