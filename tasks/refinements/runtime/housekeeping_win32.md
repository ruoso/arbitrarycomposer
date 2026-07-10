# runtime.housekeeping_win32 — Housekeeping test suites, Windows port

## TaskJuggler entry

`tasks/65-runtime.tji:104-109` — `task housekeeping_win32` under `task runtime`,
eighth leaf. `depends !housekeeping_thread, pool.checkpoints_win32`. Wired into
milestone `m9_release` (`tasks/99-milestones.tji:71`).

## Effort estimate

**1d.** No new mechanism, no production source. This task pays down a debt the
`pool.mmap_backing_win32` port deliberately took on: it un-gates two existing
runtime test suites (`housekeeping.t.cpp`, `housekeeping_thread.t.cpp`) on the
`msvc-debug` lane by replacing their `#if … && !defined(_WIN32)` exclusions with
the same capability-gate + platform-two-branch shape the sibling
`pool.checkpoints_win32` already applied to `checkpoint.t.cpp`. The only real
work is porting the POSIX temp-file recipe (`mkstemp`/`/tmp`/`unlink`) in each
suite's `TempPath` helper to the Win32 recipe. The reclamation/housekeeper logic
under test is platform-neutral; the fault-injection seam is already portable and
Windows-validated by the checkpoint port. Deliverable: the two source edits,
CI-green on `win-dev`, and confirmation that the existing housekeeping claims are
re-enforced on Windows.

## Inherited dependencies

All settled; none pending.

- **`runtime.housekeeping_thread`** (DONE 2026-07-05,
  `tasks/refinements/runtime/housekeeping_thread.md`) — produced
  `src/runtime/t/housekeeping_thread.t.cpp` (6 unit/stress cases: background-tick
  drain, graceful stop + final drain, serialized writer `after_commit`,
  deterministic tick-interval checkpoint via an injected `tick_source`,
  background checkpoint-error surfacing, and the 8-producer TSan-ready stress
  test) and the `HousekeepingThread` wrapper it exercises. The stress test
  (`single-drainer`, `housekeeping_thread.t.cpp:288`) and the graceful-stop /
  background-tick cases sit **outside** the workspace-file guard and already
  build and run on Windows — they use only portable `std::thread` /
  `std::condition_variable` / `std::mutex`.
- **`runtime.housekeeping`** (DONE 2026-07-05,
  `tasks/refinements/runtime/housekeeping.md`) — produced
  `src/runtime/t/housekeeping.t.cpp` (7 cases) and the passive `Housekeeper`
  policy object. The between-transaction-drain and anonymous-arena cases
  (`housekeeping.t.cpp:63`, `:107`) sit above the guard and already run on
  Windows.
- **`pool.checkpoints_win32`** (DONE 2026-07-10,
  `tasks/refinements/pool/checkpoints_win32.md`) — proved the checkpoint
  durability protocol green on `msvc-debug` and established the **reference
  idiom** this task mirrors: `checkpoint.t.cpp:24-38` (capability gate + inner
  `#if defined(_WIN32)/#else` include split) and `checkpoint.t.cpp:47-80` (the
  `TempPath` two-branch — `GetTempPathA`/`GetTempFileNameA`/`DeleteFileA` on
  Windows, `mkstemp`/`close`/`unlink` on POSIX). This is the direct dependency
  because the housekeeping suites drive checkpoint **cadence** over a real
  workspace-file-backed `Checkpointer`; those cases can only be un-gated once the
  protocol underneath them is validated on Windows.
- **`pool.mmap_backing_win32`** (DONE 2026-07-10, source-of-debt,
  `tasks/refinements/pool/mmap_backing_win32.md`) — flipped
  `ARBC_HAS_WORKSPACE_FILES` to `1` on `_WIN32` and, to keep that flip from
  dragging `unistd.h`/`mkstemp` into the MSVC build, added the
  `&& !defined(_WIN32)` guards this task now removes (its Status block names all
  four affected test files, including these two).

## What this task is

Bring `src/runtime/t/housekeeping.t.cpp` and
`src/runtime/t/housekeeping_thread.t.cpp` green on the `msvc-debug` CI lane by
un-gating their workspace-file-backed test bodies. Concretely: at each of the two
`#if ARBC_HAS_WORKSPACE_FILES && !defined(_WIN32)` sites in each file, drop the
`&& !defined(_WIN32)` term so the block is gated only on the capability macro,
split the POSIX-only includes behind an inner `#if defined(_WIN32)/#else`, and
port each suite's `TempPath` helper to the Win32 temp-file recipe. The stale
"a Windows housekeeping port is future work" comments are updated to record that
the port has landed. No production source, no CMake, no test-behavior changes.

## Why it needs to be done

`pool.mmap_backing_win32` turned on Windows workspace files but consciously left
the POSIX-only test bodies excluded so the macro flip wouldn't break the MSVC
build (its Decision 5 / Status). `pool.checkpoints_win32` then proved the
checkpoint durability protocol on Windows and paid down the `checkpoint.t.cpp`
share of that debt. This task pays down the runtime share: with the protocol
green, the housekeeper's cadence policy (transaction-count / tick-interval /
explicit-request checkpoint triggers over a real workspace file) and its
memory-panel stats (which read `durable_epoch` / `slots_freed_to_list` off the
`Checkpointer`) can and should be validated on Windows, not just POSIX. It is one
of the leaves gating milestone `m9_release`; leaving these two suites POSIX-only
would silently drop the Windows validation of the doc-15 checkpoint-cadence and
memory-panel promises.

## Inputs / context

- **Test files to edit:**
  - `src/runtime/t/housekeeping.t.cpp` — guard sites at `:18` (includes,
    `#endif` at `:25`) and `:136` (the `TempPath` / `ErrnoInjector` / `WsFixture`
    block and the workspace-backed cases, `#endif // ARBC_HAS_WORKSPACE_FILES` at
    `:319`). Stale comment at `:15-17`. POSIX `TempPath` at `:139-156`
    (`mkstemp` template `"/tmp/arbc_hk_XXXXXX"` at `:142`).
  - `src/runtime/t/housekeeping_thread.t.cpp` — guard sites at `:21` (includes,
    `#endif` at `:28`) and `:150` (`#endif // ARBC_HAS_WORKSPACE_FILES` at
    `:285`). Stale comment at `:19-20`. POSIX `TempPath` uses the
    `"/tmp/arbc_hkt_XXXXXX"` template.
- **Reference idiom (already merged):** `src/pool/t/checkpoint.t.cpp:24-38`
  (capability gate + include two-branch) and `:47-80` (`TempPath` two-branch:
  `GetTempPathA`/`GetTempFileNameA` create, `DeleteFileA` teardown).
- **Portable fault-injection seam (no change):** `ErrnoInjector` subclasses
  `arbc::SyscallInjector` and matches on `arbc::WorkspaceSyscall`
  (`housekeeping.t.cpp:161-178`); its `before()` returns an errno-style code the
  `io_*` shims translate to a `WorkspaceFileError` value on both platforms —
  already exercised on Windows by `checkpoint.t.cpp`'s fault-injection cases.
- **CMake:** both files are already listed unconditionally in the runtime
  component test (`src/runtime/CMakeLists.txt:39-49`); `arbc_component_test`
  (`cmake/ArbcComponent.cmake:46-56`) has no platform gating. **No CMake edit.**
- **CI lane:** `msvc-debug` = `.github/workflows/ci.yml:60`
  (`{ name: msvc-debug, os: windows-latest, preset: win-dev }`), a Debug + tests
  build (MSVC + Ninja) that runs `ctest --preset win-dev`; presets at
  `CMakePresets.json:70-75,94,105`. Because it is a Debug build, the
  `#ifndef NDEBUG` witnesses inside the checkpoint protocol stay live.
- **Governing design docs (normative, doc 16):** doc 15 (`15-memory-model.md`)
  — "Checkpointing rides the version model" (~:210-221: ordered
  msync-data → A/B root flip → msync-header; slot reuse fenced by durability
  epoch; "checkpoint cadence is policy — timer, transaction count, explicit host
  call"), "Version reclamation" (~:117-152: release enqueues, the housekeeping
  pass drains iteratively), and the same-machine/no-portability-promise clause
  for workspace files (~:204-207). Doc 14 (`14-data-model-and-editing.md`)
  transactions/history (~:86-99, :190-205). These are the promises the ported
  cases continue to validate on Windows.

## Constraints / requirements

- **Behavioral parity.** The ported cases must assert the same counters and
  outcomes on Windows as on POSIX (`commit_count`, `data_msyncs`,
  `checkpoints_skipped_clean`, `durable_epoch`, `total_slots_live`,
  fault-injected `WorkspaceFileError`). Only the temp-path mechanism differs
  across platforms; no test's assertions change.
- **One seam over shared orchestration.** Follow the established Windows-port
  house style (`checkpoints_win32`, `mmap_backing_win32`, `plugin_loading_win32`):
  the platform divergence is confined to leaf calls (`TempPath`, the include
  split); the `Housekeeper`/`HousekeepingThread` fixtures, cadence assertions,
  and `ErrnoInjector` usage stay byte-for-byte shared. No parallel
  `*_win32.t.cpp` file.
- **Capability gate preserved.** Keep `#if ARBC_HAS_WORKSPACE_FILES` — a platform
  with the macro at `0` must still compile the suite down to the unconditional
  cases above the gate. Only the `&& !defined(_WIN32)` term is removed.
- **CI-green, not compile-only.** The deliverable is the two suites passing under
  `ctest --preset win-dev`, not merely building.
- **No POSIX regression.** All Linux lanes (dev + ASan/UBSan; the stress case's
  behavior) stay green; the POSIX `TempPath` arm is byte-for-byte preserved
  inside the `#else`.
- **No new levelization edge / no CMake dependency delta.** `<windows.h>` /
  kernel32 is a platform facility invisible to `scripts/check_levels.py` and
  auto-linked by MSVC; the runtime component's `DEPENDS pool` edge already
  exists.
- **Errors stay values.** The workspace-sync-failure cases must return the
  `WorkspaceFileError` value on Windows exactly as on POSIX — no exception, no
  abort.

## Acceptance criteria

- **Un-gated suites build and pass on `msvc-debug`.** After the edits, both files
  compile and all their workspace-file-backed cases run and pass under
  `ctest --preset win-dev`:
  - `housekeeping.t.cpp` cases at `:205` (transaction-count trigger), `:226`
    (tick-interval trigger + skip-clean), `:248` (explicit `request_checkpoint`
    unconditional + clean-scene data-msync skip), `:268` (workspace sync failure
    surfaces as a value), `:290` (stats aggregate the driven counts + underlying
    counters).
  - `housekeeping_thread.t.cpp` cases at `:218` (background tick drives the
    tick-interval checkpoint deterministically via injected `tick_source`) and
    `:247` (background checkpoint failure captured + surfaced, never aborts).
- **Claims re-enforced on Windows — no new `registry.tsv` rows.** The
  now-Windows-covered `enforces:` tags remain green:
  `15-memory-model#checkpoint-cadence-is-policy`
  (`tests/claims/registry.tsv:99`) and
  `15-memory-model#housekeeping-reports-memory-panel-stats` (`:100`) — these were
  POSIX-only for their workspace-backed cases and now also run on the MSVC leg.
  `15-memory-model#housekeeping-drains-between-transactions` (`:98`),
  `15-memory-model#housekeeping-thread-single-drainer` (`:101`), and
  `15-memory-model#housekeeping-thread-stops-gracefully` (`:102`) already ran on
  Windows (their cases are outside the guard) and stay green. No claim text
  changes: only the witness (Win32 temp file) differs, exactly as
  `checkpoints_win32` established (its Decision 4).
- **Behavioral-counter discipline preserved.** Every cadence assertion stays
  driven by deterministic transaction/tick inputs and synchronized via `flush()`
  (tick-counter condition); no test reads a wall clock or sleeps — unchanged by
  the port.
- **Linux lanes stay green.** dev + ASan/UBSan and the stress case pass
  unchanged.
- **No deferred follow-up task.** This leaf fully pays down its share of the
  `mmap_backing_win32` debt for the runtime suites; nothing new is deferred to
  the WBS. The two standing parked items the housekeeping stack already carries
  (the TSan CI lane/preset; the Document→slab-arena rewire) are inherited, not
  introduced here, and remain in `tasks/parking-lot.md`.

## Decisions

1. **Test-porting only; no production source, no CMake, no design-doc delta.**
   The housekeeper logic, checkpoint protocol, and the `SyscallInjector` seam are
   all platform-neutral and already Windows-validated by `checkpoints_win32`; the
   only POSIX residue in these two files is the `TempPath` recipe. Following the
   three predecessor Windows ports, doc 15's POSIX primitives are illustrative and
   the workspace file is already declared same-machine / no-portability-promise
   (doc 15 ~:204-207), so adding a second OS behind settled platform-neutral
   policy triggers no doc-16 amendment. *Alternative rejected:* a design-doc delta
   spelling out Win32 checkpoint equivalents — already unnecessary and already
   declined by `checkpoints_win32` (its Decision 6) and `mmap_backing_win32` (its
   Decision 7), which is where the Win32 durability mechanics actually live.
2. **Mirror `checkpoint.t.cpp`'s two-branch `TempPath` verbatim in shape.** Reuse
   `GetTempPathA`/`GetTempFileNameA` (create) + `DeleteFileA` (teardown) on
   Windows, keeping the existing `mkstemp`/`unlink` arm under `#else`, and keep
   the per-suite prefixes distinct. *Alternative rejected:* a shared test-support
   `TempPath` header hoisted across pool + runtime — a larger refactor outside a
   1d port's scope and not what the sibling suites do; the copy-per-suite idiom is
   the established pattern and avoids a new cross-component test-support edge.
3. **Keep the `ErrnoInjector`/`WorkspaceSyscall` fault-injection unchanged.** It
   already returns errno-style codes the `io_*` shims translate to
   `WorkspaceFileError` values on both platforms and is exercised on Windows by
   the checkpoint port. *Alternative rejected:* a Windows-specific injector — no
   divergence exists to justify one.
4. **Re-enforce existing claims; add no `registry.tsv` rows.** The behaviors are
   the same doc-15 promises; only the temp-file witness differs by platform.
   *Alternative rejected:* platform-suffixed claim rows — would double-count a
   single normative claim, contrary to `checkpoints_win32`'s Decision 4.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-10.

- Edited `src/runtime/t/housekeeping.t.cpp`: dropped `&& !defined(_WIN32)` at both guard sites, split includes behind `#if defined(_WIN32)` / `#else`, ported `TempPath` to `GetTempPathA`/`GetTempFileNameA`/`DeleteFileA` on Windows, refreshed stale "future work" comments.
- Edited `src/runtime/t/housekeeping_thread.t.cpp`: same mechanical port mirroring the `checkpoint.t.cpp` idiom — include two-branch, two-branch `TempPath`, stale-comment refresh.
- POSIX arms (`mkstemp`/`unlink`) preserved byte-for-byte under `#else` in both files; no production source, CMake, or `registry.tsv` changes.
- Claims `15-memory-model#checkpoint-cadence-is-policy` and `15-memory-model#housekeeping-reports-memory-panel-stats` now enforced on the `msvc-debug` lane as well as all Linux lanes.
- No follow-up tech-debt task; the two standing parked items (TSan CI preset; Document→slab-arena rewire) are inherited, not introduced here.
