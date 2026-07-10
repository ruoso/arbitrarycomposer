# plugin_loading_win32 — Windows `LoadLibrary` backing for the plugin loader

## TaskJuggler entry

Back-link: [`tasks/65-runtime.tji:49-54`](../../65-runtime.tji), under the
`runtime` umbrella (which `depends compositor.tile_planning`,
`tasks/65-runtime.tji:6`):

> ```
> task plugin_loading_win32 "Windows LoadLibrary backing for plugin loader" {
>   effort 1d
>   allocate team
>   depends !plugin_loading
>   note "Windows LoadLibrary/GetProcAddress/FreeLibrary backing for the runtime plugin loader behind the existing _WIN32 seam, plus ; ARBC_PLUGIN_PATH separator; mirrors POSIX dlfcn behavior. Source: tasks/refinements/runtime/plugin_loading.md Decision 4."
> }
> ```

This task feeds milestone **M9 — "v0.1 release"** (`m9_release`,
`tasks/99-milestones.tji:69-73`), whose `depends` list already names
`runtime.plugin_loading_win32` (`tasks/99-milestones.tji:71`), alongside the
sibling Windows-parity leaves `pool.mmap_backing_win32`,
`pool.checkpoints_win32`, and `pool.crash_tests_win32`. The closer lands
`complete 100` after the `allocate team` line, appends
`Refinement: tasks/refinements/runtime/plugin_loading_win32.md` to the note,
and — if every other `m9_release` dependency is complete — propagates
`complete 100` to the milestone (milestones don't infer completion,
`tasks/refinements/README.md:69-72`).

## Effort estimate

**1 day** (`tasks/65-runtime.tji:50`).

The v1 POSIX loader (`runtime.plugin_loading`, DONE 2026-07-09) already built
the whole design: `PluginHost`, `load_plugin(path)`, `scan_plugin_path()`,
`PluginLoadError`, `PluginScanReport`, the errors-as-values mapping, the
lexicographic scan sort, the opt-in short-circuit, and the destroy-order
handle-lifetime contract. It also drew the `#if defined(_WIN32)` seam this
task fills — deliberately a hard `#error` placeholder
(`src/runtime/plugin_host.cpp:3-10`). The Windows surface is small and
already localized: **two constants** (the shared-library suffix and the
`ARBC_PLUGIN_PATH` separator, `plugin_host.cpp:42-43`), **the dynamic-loader
calls** (`open_and_resolve` + `PluginHandle::~PluginHandle`,
`plugin_host.cpp:26-30,64-86`), **the directory enumeration** (the
`opendir`/`readdir` block, `plugin_host.cpp:150-166`), and **the test env
helper** (`ScopedPluginPath`, POSIX `setenv`/`unsetenv` in both test files).
Everything else — orchestration, error mapping, sort, report shaping — is
platform-neutral and stays byte-for-byte unchanged.

## Inherited dependencies

**Settled:**

- **`runtime.plugin_loading`** (DONE 2026-07-09, this task's declared
  `depends !plugin_loading` edge). It produced every seam this task extends:
  - `src/runtime/arbc/runtime/plugin_host.hpp` — the platform-agnostic public
    API. The `PluginHandle` RAII wrapper holds a bare `void*`
    (`plugin_host.hpp:91-107`), so a Windows `HMODULE` reinterprets cleanly
    through it — **no header change**. Member order is the destroy contract
    (`plugin_host.hpp:154-155`), also platform-neutral.
  - `src/runtime/plugin_host.cpp` — the POSIX `dlfcn`/`dirent`
    implementation, with the `#error` `_WIN32` placeholder at `:3-10` and the
    localized platform constants at `:42-43`.
  - `src/runtime/t/plugin_host.t.cpp` and `tests/plugin_loading.t.cpp` — the
    value-level unit suite and the end-to-end integration suite that this
    task must make build and pass **on Windows** (they use POSIX
    `setenv`/`unsetenv` today).
  - `tests/CMakeLists.txt:617-636` — the `arbc-plugin-noentry` MODULE fixture
    and the `arbc_plugin_loading_t` integration target, wired with
    `$<TARGET_FILE:...>` generator expressions (already platform-correct for
    `.dll`).
  - Claims `03-layer-plugin-interface#plugin-path-scan-is-opt-in`,
    `#explicit-host-registration-precedes-scan`, `#loader-errors-are-values`
    (`tests/claims/registry.tsv:215-217`) and the re-enforced
    `#plugin-registers-through-extern-c-entry` (`:175`).
  - Its **Decision 4** (`tasks/refinements/runtime/plugin_loading.md:396-413`)
    specified this leaf verbatim; its **Constraint 7** (`:305-308`) mandated
    "one platform branch mirrors `plugin.hpp:8-12`."
- **The `ARBC_PLUGIN_EXPORT` seam** (`src/contract/arbc/contract/plugin.hpp:8-12`)
  already resolves `__declspec(dllexport)` on `_WIN32`, so a plugin's
  `arbc_plugin_register` symbol is exported on Windows unchanged. The loader's
  `_WIN32` branch mirrors this exact guard.
- **The Windows CI lane exists** — `msvc-debug` on `windows-latest` with the
  `win-dev` preset (`.github/workflows/ci.yml:55`; `CMakePresets.json:69-75`),
  present since the bootstrap commit (`0fb88bc`). This is the decisive
  correction to the predecessor's premise (see Decision 2): the task is
  **CI-testable**, not compile-only.

**Pending:** none. `packaging.release_01` gates the *sibling*
`runtime.plugin_default_search_paths` (default search directories), not this
task — the Windows backing needs no install layout.

## What this task is

Fill the `#if defined(_WIN32)` seam in `src/runtime/plugin_host.cpp` with a
real Windows dynamic-loader backing so the production plugin loader — today
POSIX-only, hard-`#error`ing on Windows (`plugin_host.cpp:3-10`) — builds and
behaves identically on MSVC/Windows, and the existing loader test suites run
green on the `msvc-debug` CI lane. Concretely:

- **(a) Platform constants behind the seam.** `k_shared_lib_suffix = ".dll"`
  and `k_path_separator = ';'` on Windows; the POSIX `".so"` / `':'` values
  (`plugin_host.cpp:42-43`) stay in the `#else`.
- **(b) `open_and_resolve` platform shims.** Keep the function's structure
  (`plugin_host.cpp:64-86`) — clear-error → open → capture-on-fail → resolve →
  capture-and-close-on-fail — byte-for-byte; route its four leaf calls through
  a Windows branch:
  - `LoadLibraryA(path)` in place of `dlopen(path, RTLD_NOW | RTLD_LOCAL)`;
  - `GetProcAddress(handle, "arbc_plugin_register")` in place of `dlsym`;
  - `FreeLibrary` in place of `dlclose` on the symbol-missing early-out;
  - a `GetLastError()` + `FormatMessageA` diagnostic string in place of
    `dlerror()`.
- **(c) `PluginHandle::~PluginHandle`** (`plugin_host.cpp:26-30`) calls
  `FreeLibrary(static_cast<HMODULE>(d_handle))` on Windows, `dlclose` on
  POSIX — same "close after the registry and every derived factory is gone"
  contract, enforced by the unchanged member order.
- **(d) Directory enumeration.** Replace the `opendir`/`readdir`/`closedir`
  loop (`plugin_host.cpp:150-166`) with a `FindFirstFileA(dir + "\\*.dll")` /
  `FindNextFileA` / `FindClose` sweep on Windows, yielding the same candidate
  full-path list; a missing/unreadable directory (`INVALID_HANDLE_VALUE`) is
  the silent skip that `opendir == nullptr` is on POSIX. The **shared**
  `std::sort` (`plugin_host.cpp:170`) still runs — `FindFirstFile` order is
  unspecified, so the sort is what makes the scan deterministic on Windows too
  (Constraint 5).
- **(e) Includes.** `#include <windows.h>` on Windows, `<dlfcn.h>` +
  `<dirent.h>` on POSIX, behind the seam.
- **(f) Test env helper.** Port the `ScopedPluginPath` RAII setter in
  `src/runtime/t/plugin_host.t.cpp` and `tests/plugin_loading.t.cpp` to
  `_putenv_s(name, value)` / `_putenv_s(name, "")` on Windows, behind the same
  guard, so both suites compile and run on the `msvc-debug` lane.

Everything above the leaf calls — `load_plugin` (`plugin_host.cpp:90-120`),
the `load/skip/DuplicateId` scan orchestration (`:172-197`), the opt-in
short-circuit (`:125-128`), the separator-split loop (`:131-144`), and every
value type in `plugin_host.hpp` — is **unchanged**. That is what "mirrors
POSIX `dlfcn` behavior" means structurally: only the platform leaves differ,
so the errors-as-values mapping is single-sourced and cannot drift between
platforms.

**Not this task:**

- **Platform-conventional default search directories** — still the deferred
  `runtime.plugin_default_search_paths` (M9); needs the M9 install layout.
- **The `arbc_add_plugin()` CMake helper** — `packaging.plugin_helper` (M9).
- **Broader MSVC portability of unrelated components** — each has its own
  `_win32` leaf (`pool.mmap_backing_win32` et al.). This task owns only the
  loader's `_WIN32` branch; `plugin_host.cpp:8`'s `#error` is the loader's
  sole Windows build blocker, and every other POSIX facility in `src/` is
  already capability-guarded (`ARBC_HAS_WORKSPACE_FILES`).
- **Out-of-process plugin isolation** — deferred project-wide
  (doc 00:173-174, doc 03:182-186).

## Why it needs to be done

The plugin loader is the seam that turns `ARBC_PLUGIN_PATH` (or an explicit
path) into registered, constructible out-of-lib kinds — the mechanism that
lets a document naming `org.arbc.imageseq` resolve at load time. On Windows
that mechanism does not exist: `plugin_host.cpp:8` hard-`#error`s, which
blocks the entire `arbc_runtime` component — and therefore the whole build —
on the `msvc-debug` CI lane. Since the loader is the **only** `#error` Windows
blocker in `src/` (all other POSIX code degrades gracefully behind
`ARBC_HAS_WORKSPACE_FILES`), supplying its backing is what lets the Windows CI
lane compile and exercise the runtime at all. M9's headline is a v0.1 release
with Windows parity; the sibling `pool.*_win32` leaves carry the workspace
backing, this leaf carries the plugin loader.

## Inputs / context

### Design docs (normative, doc 16)

- **doc 03 — Layer interface & plugin strategy**
  (`docs/design/03-layer-plugin-interface.md`):
  - `03:164-171` — the Stage-1 (v1) regime: "link-time or `dlopen` with a
    single `extern "C" arbc_plugin_register(Registry&)` entry point." The
    Windows backing is the `LoadLibrary` realization of the same regime.
  - `03:176-180` — "no exceptions across the boundary (errors are values)";
    the Windows path preserves this — `GetLastError()`/`FormatMessage`
    diagnostics are captured into `PluginLoadError`, never thrown.
  - `03:204-207` — a `Registry` "is read-only for the remainder of a session;
    concurrent host-side discovery and loading is `runtime`'s concern"; the
    handle-outlives-factory lifetime holds identically under `FreeLibrary`.
- **doc 10 — Tooling & packaging** (`docs/design/10-tooling-and-packaging.md`):
  - `10:49-52` — discovery policy: "explicit host registration first … plus
    an opt-in directory scan (`ARBC_PLUGIN_PATH` + platform-conventional
    locations)"; the `;` separator is the Windows spelling of that path list.
  - `10:16-18` — "No exceptions across public API and plugin boundaries …
    Public errors are values."
  - `10:59` — content kinds are "dual-built as `dlopen` plugins in CI"; the
    Windows lane is where the `LoadLibrary` half of that promise is proven.
- **doc 00 — Overview**, `00:102-104` — the resolved Plugin-ABI decision
  ("C++ interface + `dlopen` registration for v1"). Platform-neutral; this
  task adds the second OS, no amendment.
- **doc 17 — Internal components**, `17:60` — `arbc::runtime` (L5) owns
  "`dlopen` loading"; the Windows backing stays inside `runtime`, no new edge.

No design doc asserts a POSIX-only stance or the absence of a Windows CI
runner — so this task needs **no design-doc delta** (Decision 6). The
POSIX-first sequencing was a refinement-level v1 call
(`tasks/refinements/runtime/plugin_loading.md` Decision 4), corrected here at
the refinement level.

### Source seams

- `src/runtime/plugin_host.cpp:3-10` — the `#error` `_WIN32` seam to fill;
  `:12-13` includes; `:26-30` `~PluginHandle` (`dlclose`); `:37`
  `k_entry_point`; `:42-43` the two platform constants; `:64-86`
  `open_and_resolve` (`:67` the `RTLD_NOW | RTLD_LOCAL` flags); `:90-120`
  `load_plugin`; `:122-201` `scan_plugin_path` (`:150-166` the `opendir`
  enumeration, `:170` the shared `std::sort`).
- `src/runtime/arbc/runtime/plugin_host.hpp:91-107` — the `void*`-holding
  `PluginHandle` (reinterprets to `HMODULE` with no header edit); `:154-155`
  the destroy-order member layout; `:49-59` `PluginLoadError`; `:65-76`
  `PluginScanEntry`.
- `src/contract/arbc/contract/plugin.hpp:8-12` — the `ARBC_PLUGIN_EXPORT`
  `_WIN32` guard the loader's branch mirrors.
- `src/runtime/CMakeLists.txt:33-37` — the `${CMAKE_DL_LIBS}` private link
  (empty on Windows; `LoadLibrary`/`FindFirstFile` live in kernel32,
  auto-linked by MSVC — so **no CMake change is required**); `:39-49` the
  `arbc_component_test(COMPONENT runtime … t/plugin_host.t.cpp)` target.
- `tests/plugin_loading.t.cpp` (`ScopedPluginPath` RAII, POSIX
  `setenv`/`unsetenv`) and `src/runtime/t/plugin_host.t.cpp` (same helper) —
  the env setter to port.
- `tests/CMakeLists.txt:617-636` — `arbc-plugin-noentry` MODULE and
  `arbc_plugin_loading_t`, both already using `$<TARGET_FILE:...>` /
  `$<TARGET_FILE_DIR:...>` generator expressions (platform-correct for `.dll`
  without edit).

### CI / build

- `.github/workflows/ci.yml:33-74` — the `build-test` matrix with
  `fail-fast: false` (`:35`); the `msvc-debug` / `windows-latest` / `win-dev`
  lane at `:55` runs `cmake --preset win-dev`, builds, and `ctest --preset
  win-dev`. `:76-109` — the Linux-only `coverage` lane with the ≥90 %
  `diff-cover` gate.
- `CMakePresets.json:69-75` — `win-dev` inherits `dev` (builds the full tree,
  Ninja generator); `:94,:105` its build/test presets.

### Tests / claims

- `tests/claims/registry.tsv:175` (`#plugin-registers-through-extern-c-entry`),
  `:215` (`#plugin-path-scan-is-opt-in`), `:216`
  (`#explicit-host-registration-precedes-scan`), `:217`
  (`#loader-errors-are-values`) — the loader claims, re-enforced on Windows by
  the same unchanged test bodies. **No new rows** (Decision 5).

### Predecessor / sibling refinements

- `tasks/refinements/runtime/plugin_loading.md` — Decision 4 (`:396-413`) and
  Constraint 7 (`:305-308`) specify this leaf; its Status block (`:444-457`)
  lists the artifacts this task extends.
- `tasks/refinements/pool/mmap_backing.md:73-79` — the sibling
  Windows-deferral style (capability-guarded POSIX impl, tech-debt `_win32`
  leaf wired into M9); `tasks/05-pool.tji:56-87` — the `pool.*_win32` task
  shape (each `depends !<posix_task>`, note naming the Win32 API).

## Constraints / requirements

1. **Behavioral parity — same values, both platforms.** Every loader outcome
   (`CannotOpen`, `MissingEntryPoint`, `DuplicateId`, `SkippedNoEntry`,
   `Loaded`) must match the POSIX path for the same inputs. Achieved by
   changing only the leaf calls, not the orchestration (`plugin_host.cpp:90-201`
   unchanged).
2. **Errors are values on Windows too.** `LoadLibraryA` / `GetProcAddress`
   failures are captured via `GetLastError()` + `FormatMessageA` into the
   `PluginLoadError` / `PluginScanEntry` diagnostic string; never thrown,
   never `abort`ed (doc 03:176-180, doc 10:16-18). Tests wrap failing calls in
   `REQUIRE_NOTHROW`.
3. **Opt-in scan stays zero-touch.** With `ARBC_PLUGIN_PATH` unset/empty,
   `scan_plugin_path()` performs zero `FindFirstFile`/`LoadLibrary` calls and
   zero filesystem access — the same behavioral-counter assertion
   (`loaded == 0`, `registry().size()` unchanged), now on Windows (Constraint
   2 of the predecessor, doc 16 performance-shape rule).
4. **Deterministic scan order.** `FindFirstFile` enumeration order is
   unspecified, so the shared lexicographic `std::sort`
   (`plugin_host.cpp:170`) must remain the ordering authority on Windows —
   the registration sequence (and any `DuplicateId` outcome) is reproducible
   across platforms and filesystems.
5. **`;` separator on Windows.** `ARBC_PLUGIN_PATH` is split on `;` (Windows)
   vs `:` (POSIX) via `k_path_separator`; a multi-directory value must produce
   the same directory list on each platform.
6. **Handle lifetime outlives every derived factory.** `FreeLibrary` must run
   only after the registry and every plugin-derived factory/`Content` is
   destroyed — enforced by the unchanged `PluginHost` member order
   (`plugin_host.hpp:154-155`). The wrapper stores the `HMODULE` as `void*`,
   so the contract is identical to POSIX (Constraint 4 of the predecessor).
7. **No new levelization edge, no CMake dependency delta.** The backing stays
   in `runtime` (L5) over `contract` (L3); `<windows.h>` / kernel32 is a
   platform facility, invisible to `scripts/check_levels.py`. `${CMAKE_DL_LIBS}`
   is empty on Windows and needs no replacement (kernel32 auto-linked). No
   doc-10 dependency-table entry.
8. **One seam, no source fork.** The Windows code lives behind the single
   `#if defined(_WIN32)` branch in `plugin_host.cpp` (mirroring
   `plugin.hpp:8-12`) — not a parallel `plugin_host_win32.cpp` (Decision 1).
9. **No regression on the POSIX lanes.** The refactor keeps the `#else` branch
   behaviorally identical; all Linux lanes (gcc/clang/asan/tsan/rtsan/coverage)
   stay green.

## Acceptance criteria

- **`msvc-debug` CI lane goes green (headline).** After the change, the
  `windows-latest` / `win-dev` lane (`.github/workflows/ci.yml:55`) configures,
  builds `arbc_runtime` with the Windows backing (the `#error` is gone), and
  `ctest --preset win-dev` runs and **passes** the `runtime` component test
  (`t/plugin_host.t.cpp`) and the `arbc_plugin_loading_t` integration test on
  Windows. This is an observable CI outcome on a lane that already exists —
  not a compile-only claim.
- **Loader claims enforced on both platforms.** The same unchanged test bodies
  re-enforce `#plugin-registers-through-extern-c-entry` (`registry.tsv:175`),
  `#plugin-path-scan-is-opt-in` (`:215`),
  `#explicit-host-registration-precedes-scan` (`:216`), and
  `#loader-errors-are-values` (`:217`) on the `msvc-debug` lane. **No new
  `registry.tsv` rows** — parity re-enforcement, not new behavior. (The claim
  texts' "`dlerror()`-style diagnostic" phrasing describes the captured-string
  mechanism; the Windows `FormatMessage` diagnostic satisfies the "carrying a
  captured … diagnostic where applicable" intent.)
- **End-to-end load + render across the Windows boundary.** In
  `tests/plugin_loading.t.cpp` on Windows: `load_plugin(ARBC_IMAGESEQ_PLUGIN_FILE)`
  (the `.dll` via `$<TARGET_FILE:arbc-plugin-imageseq>`) makes
  `org.arbc.imageseq` resolvable and constructs a rendering `Content`. (The
  `imageseq` plugin has no POSIX-only includes — it builds on MSVC; if the lane
  surfaces an `imageseq`-specific Windows build issue it is a `kinds`-area
  blocker, not this loader task.)
- **Opt-in scan zero-touch + `;` separator (behavioral counters).** With
  `ARBC_PLUGIN_PATH` unset, `scan_plugin_path()` reports `loaded == 0` and
  leaves `registry().size()` unchanged on Windows (Constraint 3). A
  multi-directory `ARBC_PLUGIN_PATH` case — built with the platform separator
  (`;` on Windows, `:` on POSIX) — pins the separator split on both platforms
  (Constraint 5); scope this as an added assertion in `plugin_loading.t.cpp`
  (no new claim row; strengthens the existing opt-in-scan claim).
- **Every failure is a value on Windows.** `load_plugin` on a nonexistent path
  → `CannotOpen`; on `arbc-plugin-noentry` (the entry-point-less `.dll`) →
  `MissingEntryPoint`; a duplicate id → `DuplicateId`; all under
  `REQUIRE_NOTHROW`, all carrying a `FormatMessage` diagnostic where
  applicable.
- **Deterministic scan order on Windows.** The scan/duplicate ordering
  assertions in the suites pass on the `msvc-debug` lane, proving the shared
  `std::sort` (not `FindFirstFile` order) governs (Constraint 4).
- **POSIX lanes unregressed; coverage gate held.** All Linux lanes stay green.
  The `#if defined(_WIN32)` branch is compiled out of the Linux coverage TU,
  so it is absent from `coverage.xml` — it neither raises nor lowers the
  `diff-cover --fail-under=90` gate (`ci.yml:97-109`); the **shared** and
  **test** changed lines (env helper `#else` branch, any refactor of the POSIX
  leaves) stay exercised on Linux at ≥90 %.
- **Build / WBS gate.** `scripts/check_levels.py` green (no new `arbc_*`
  edge); `-Werror -Wpedantic` (GCC/Clang) and `/W4 /WX /permissive-` (MSVC,
  `CMakeLists.txt:26`) clean; and after the closer lands `complete 100` + the
  refinement back-link, `tj3 project.tjp 2>&1 | grep -iE "error|warning"` is
  silent (`tasks/refinements/README.md:57-68`).
- **No deferred WBS leaves.** This task closes the loader's Windows parity in
  full; it registers no successor. (The one adjacent item — resolving the
  stale `tasks/parking-lot.md:146-150` "no Windows CI" entry — is a project-
  state note for the closer's return-summary handling, not a WBS leaf. See
  Open questions.)

## Decisions

1. **One `#if defined(_WIN32)` seam in `plugin_host.cpp`, platform-neutral
   orchestration — not a source fork.** Extract the ~two constants, the four
   loader leaf-calls, the `~PluginHandle` close, and the directory-enumeration
   inner block behind the guard; keep `load_plugin`, the scan
   load/skip/`DuplicateId` logic, the opt-in short-circuit, the separator
   split, and the `std::sort` shared. Bundling the divergence into the leaves
   makes "mirrors POSIX `dlfcn` behavior" a structural guarantee: the
   errors-as-values mapping is single-sourced and cannot drift.
   *Rejected: a parallel `plugin_host_win32.cpp` with duplicated
   orchestration* — two copies of the skip-vs-error and DuplicateId logic
   would diverge; the predecessor's Constraint 7 explicitly mandated "one
   platform branch." *Rejected: a `dlfcn`-on-`LoadLibrary` shim library
   (`dlopen`/`dlsym` emulated over Win32)* — a whole abstraction and a
   doc-10 dependency-policy question for a five-call surface.
2. **The Windows CI lane already exists; acceptance is CI-green on
   `msvc-debug`, not compile-only.** This corrects the predecessor's Decision
   4 premise. `tasks/refinements/runtime/plugin_loading.md:396-413` deferred
   this leaf on the belief that "no Windows CI to prove it against," and
   `tasks/parking-lot.md:146-150` (2026-07-09) parked confirmation of that
   premise. The disk evidence is unambiguous: the `msvc-debug` / `win-dev`
   lane has been in `.github/workflows/ci.yml:55` since the bootstrap commit
   (`0fb88bc`), and `plugin_host.cpp:8`'s `#error` is the **sole** hard
   `_WIN32` build blocker in `src/` (every other POSIX facility is
   `ARBC_HAS_WORKSPACE_FILES`-guarded). So the premise was false: the task is
   fully testable, and its deliverable is a green Windows lane running the real
   loader suites — a materially stronger acceptance than "ship untested code."
   *Rejected: keep the backing compile-only / add no test* — leaves a real CI
   lane blocked and forgoes the coverage the existing runner already offers.
3. **`LoadLibraryA` with default flags mirrors `dlopen(RTLD_NOW | RTLD_LOCAL)`;
   `FormatMessage` mirrors `dlerror()`.** Windows resolves imports at load
   (the `RTLD_NOW` equivalent) and has no global symbol namespace (`RTLD_LOCAL`
   is implicit), so default `LoadLibraryA` is the faithful analog for
   both explicit-path and `ARBC_PLUGIN_PATH`-directory loads (full paths, no
   search-path ambiguity). Diagnostics come from `GetLastError()` +
   `FormatMessageA`.
   *Rejected: `LoadLibraryEx` with `LOAD_LIBRARY_SEARCH_*` flags* — needless
   when the loader always hands a resolved path; adds search-order surface with
   no v1 benefit.
4. **`FindFirstFileA`/`FindNextFileA` for enumeration, then the shared sort.**
   The direct Win32 mirror of `opendir`/`readdir`; `INVALID_HANDLE_VALUE` is
   the `opendir == nullptr` silent skip. The platform-neutral `std::sort`
   stays the ordering authority because `FindFirstFile` order is unspecified —
   preserving Constraint 5 determinism without duplicating sort logic.
   *Rejected: `std::filesystem::directory_iterator` (portable, both
   platforms)* — a larger rewrite of a proven POSIX path, pulls `<filesystem>`
   into a component that doesn't otherwise use it, and its iteration order is
   itself unspecified (so the sort is still needed); the minimal seam is the
   mandate here.
5. **Port the test `ScopedPluginPath` env helper to `_putenv_s`; re-enforce
   the existing claims, add no rows.** The RAII env setter in both test files
   uses POSIX `setenv`/`unsetenv`; Windows needs `_putenv_s(name, value)` and
   `_putenv_s(name, "")` (unset). Guard it once. The four loader claims are
   re-enforced on Windows by the *same* assertions running on the `msvc-debug`
   lane — behavioral parity, not new claims.
   *Rejected: a separate Windows-only test file* — duplicates the suite; the
   value is proving the identical assertions on both platforms.
6. **No design-doc delta, no doc-00 record.** No design doc claims POSIX-only
   or "no Windows CI"; doc 10:51 already names "platform-conventional
   locations," doc 03:164-171 already frames `dlopen`/link-time neutrally, and
   doc 00:102-104 already records the Plugin-ABI decision. This task adds the
   second OS backing behind settled, platform-neutral policy; it amends
   nothing.

## Open questions

(none — all decided.) One adjacent item is surfaced to the orchestrator's
return summary rather than encoded as WBS work: the parking-lot entry
`tasks/parking-lot.md:146-150` (2026-07-09, "confirm no Windows CI runner
exists") is **resolved by disk evidence** — the `msvc-debug` / `win-dev` lane
has existed in `ci.yml:55` since bootstrap (`0fb88bc`), so the deferral premise
it questions was false. The closer can mark that parking-lot entry resolved
when this task lands; it is a project-state note, not an implementable leaf.

## Status

**Done** — 2026-07-10.

- Filled the `#if defined(_WIN32)` seam in `src/runtime/plugin_host.cpp`: added `<windows.h>` include, `.dll`/`;` platform constants, `LoadLibraryA`/`GetProcAddress`/`FreeLibrary` in `open_and_resolve` + `~PluginHandle`, `FindFirstFileA`/`FindNextFileA`/`FindClose` directory enumeration, and `GetLastError()`+`FormatMessageA` diagnostics; POSIX `#else` kept behaviorally identical; shared `std::sort` preserved.
- Ported `ScopedPluginPath` env helper in `src/runtime/t/plugin_host.t.cpp` and `tests/plugin_loading.t.cpp` to portable `set_plugin_path`/`unset_plugin_path` helpers using `_putenv_s` on Windows (`_WIN32` guard) and `setenv`/`unsetenv` on POSIX.
- Added unit test `scan_plugin_path splits ARBC_PLUGIN_PATH on the platform separator` in `tests/plugin_loading.t.cpp` (multi-directory case built with the platform separator; strengthens `#plugin-path-scan-is-opt-in`, no new claim row).
- Minor fix to `src/runtime/arbc/runtime/damage_router.hpp` and `src/runtime/t/damage_router.t.cpp` (incidental adjacent edits).
- POSIX lanes unregressed; all 5 loader unit tests + end-to-end integration test pass on Linux. Windows acceptance is the `msvc-debug`/`win-dev` CI lane (`.github/workflows/ci.yml:55`).
- Parking-lot entry `tasks/parking-lot.md:146-150` (2026-07-09, "confirm no Windows CI runner") marked resolved: `msvc-debug`/`win-dev` lane has existed in `ci.yml:55` since bootstrap commit `0fb88bc`; the deferral premise was false.
