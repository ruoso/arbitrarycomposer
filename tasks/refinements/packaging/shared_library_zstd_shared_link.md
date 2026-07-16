# packaging.shared_library_zstd_shared_link — Fix ELF shared zstd link and shared install.consumer relink

## TaskJuggler entry

[`tasks/75-packaging.tji:49-53`](../../75-packaging.tji) —

```
task shared_library_zstd_shared_link "Fix ELF shared zstd link and shared install.consumer relink" {
  effort 1d
  allocate team
  depends !shared_library_build_msvc
  note "Under BUILD_SHARED_LIBS=ON, link system libzstd as a .so dependency rather than folding a possibly-non-PIC static archive into libarbc.so (which causes SIGSEGV in ZSTD_decompress on systems with non-PIC libzstd.a), and repair the shared install.consumer relink. Source-of-debt: tasks/refinements/packaging/shared_library_build_msvc.md (surfaced 2026-07-16); predecessor D4 stated shared+fetched-static-zstd was an ELF PIC-fold concern. Docs 03/17."
}
```

Milestone: **M9** (`tasks/99-milestones.tji:70-73`, the v0.1 release) —
`m9_release` already names `packaging.shared_library_zstd_shared_link` in its
`depends` list (`:72`), directly beside `packaging.shared_library_build` and
`packaging.shared_library_build_msvc`. The closer therefore wires **no new
milestone edge**; the leaf is already in the release gate. It gates nothing
else in M9. The release ships `libarbc` in *"shared and static"* form
(doc 17:13); the ELF and MSVC predecessors proved the *symbol surface* loads
shared on both toolchains, but the ELF shared build still crashes or fails to
relink when its one compiled dependency — zstd — is resolved from the system as
a static archive. This task closes that corner so the shared build is honest
end-to-end, not just at the plugin-symbol boundary.

## Effort estimate

**Booked 1d; realistic 1d.** This is a surgical CMake correctness fix in two
already-written seams plus one CI-lane adjustment, pinned by tests that already
exist and already run on the shared lane. No new C++, no new test file, no new
claim. Budget:

- 0.3d — make the `arbc_zstd` normalization shim (`CMakeLists.txt:209-226`)
  `BUILD_SHARED_LIBS`-aware: under a shared `libarbc`, prefer the system
  **shared** zstd target so `libarbc.so` records a `libzstd.so` `DT_NEEDED`
  entry instead of folding a system `libzstd.a` of unknown PIC-ness
  (Decision D1).
- 0.3d — repair the install/export discriminator
  (`cmake/ArbcInstall.cmake:45-54`) so a *shared* `libarbc` neither folds a
  non-PIC static archive nor re-imposes `zstd` on the embedder's link line —
  the dependency is already encapsulated in the `.so` (Decision D2).
- 0.2d — make the `gcc-shared` CI lane resolve a **system** zstd (install
  `libzstd-dev`), so the exact combination that crashed (system zstd + shared
  `libarbc`) is the one under test, turning the existing round-trip smoke and
  `install.consumer` relink into a real regression gate (Decision D3).
- 0.2d — debug the ELF shared relink fallout the green-lane requirement
  surfaces (the `install.consumer` embedder path linking `arbc::arbc` alone),
  and confirm the static lanes and the fetched-zstd path are byte-unchanged.

There is no `ARBC_API` work (the predecessors finished it), no new preset (the
`shared` trio exists, `CMakePresets.json:95-101,124,138`), and no design-doc
delta (Decision D4).

## Inherited dependencies

**Settled (formal `depends`, `tasks/75-packaging.tji:52`):**

- **`packaging.shared_library_build_msvc`**
  ([`./shared_library_build_msvc.md`](./shared_library_build_msvc.md),
  **Done 2026-07-16**) — the source-of-debt. Its Status block
  (`:534`) records the surfacing verbatim: *"pre-existing gcc-shared zstd PIC
  issue (non-PIC system `libzstd.a` folded into `libarbc.so` → SIGSEGV in
  `ZSTD_decompress`; `install.consumer` shared relink failure); registered as
  tech-debt task `packaging.shared_library_zstd_shared_link`."* Its Constraint 7
  (`:345-350`) scoped **why the debt is ELF-only**: Windows object code carries
  no PIC/non-PIC distinction, so folding a static zstd into `arbc.dll` is
  unproblematic and `win-shared` inherits `win-dev`'s zstd unchanged. The bug
  lives on ELF; this task fixes ELF and leaves Windows untouched.

**Settled (transitive — the seams this task edits):**

- **`packaging.shared_library_build`**
  ([`./shared_library_build.md`](./shared_library_build.md), **Done
  2026-07-15**) — the ELF/gcc `BUILD_SHARED_LIBS=ON` lane and the umbrella's
  no-`STATIC`/`SHARED`-keyword shape (`cmake/ArbcComponent.cmake:187`, flips
  with `BUILD_SHARED_LIBS`). Its **Constraint 7 / Decision D4** already named
  this exact hazard but scoped it out: *"folding a fetched static zstd into a
  shared `libarbc.so` needs PIC objects the fetched build does not guarantee
  (`cmake/ArbcInstall.cmake:45-54`), so the ELF shared lane uses system
  (find-first) zstd as an ordinary `.so` dependency; shared+fetched-static-zstd
  is an explicit non-goal."* What that decision **missed** — and what MSVC's
  green-lane requirement surfaced — is that the *system* copy the shared lane
  falls back to can itself be a **non-PIC static archive** (`libzstd.a`), because
  the `arbc_zstd` shim **prefers `zstd::libzstd_static` over
  `zstd::libzstd_shared`** (`CMakeLists.txt:215-218`). This task corrects that
  preference for the shared build.

- **`serialize.zstd_dep`**
  ([`../serialize/zstd_dep.md`](../serialize/zstd_dep.md), **Done 2026-07-14**)
  — landed the whole zstd wiring: the find-first `FetchContent` block
  (`CMakeLists.txt:194-207`), the `arbc_zstd` INTERFACE shim (`:209-226`), the
  PRIVATE link onto `arbc_serialize` (`src/serialize/CMakeLists.txt:35`), and
  the `compress_blob`/`decompress_blob` seam the round-trip smoke exercises. Its
  **Decision 3** created the shim this task makes `BUILD_SHARED_LIBS`-aware; its
  Acceptance criteria explicitly handed the packaging stream the exported-config
  obligation — *"`find_dependency(zstd)` when the system copy was used, nothing
  when the pinned static build was folded in"* — which this task extends with
  the missing shared-libarbc branch. Its **Decision 6** (stateless one-shot
  `ZSTD_*` API) and **Decision 7** (bounded, untrusted decompression) are
  untouched: this is a *link-shape* fix, not a seam-behavior change.

**Informal, but the real context:**

- **`quality.testing_artifact`** — landed `cmake/ArbcInstall.cmake` and the
  `install.consumer` relink CTest (`tests/CMakeLists.txt:1333-1361`,
  `tests/consumer/run_staged_install.cmake`, `tests/consumer/CMakeLists.txt`).
  The driver stages `cmake --install` into a fresh prefix and relinks a foreign
  consumer against `arbc::arbc` via `find_package(arbc CONFIG)`. It runs under
  every preset's `ctest`, so it *already* runs against a shared `libarbc` on the
  `gcc-shared` lane — which is exactly where the shared relink failure appears
  and where the fix is proven.

**Pending, and deliberately not a dependency:**

- **`packaging.install`** (`tasks/75-packaging.tji:6-10`) — owns pkg-config,
  CPS, `VERIFY_INTERFACE_HEADER_SETS`, CPack. No edge: this task edits only the
  already-present umbrella install/export seam, imposing nothing new on
  `install`.

**Downstream (this task serves):**

- **`packaging.release_01`** (`tasks/75-packaging.tji:29-33`) — a released
  `libarbc` advertised *"shared and static"* (doc 17:13) must actually *work*
  shared when its lone compiled dependency comes from the system. The plugin
  predecessors proved the symbol surface; this proves the dependency link. Only
  then can `release_01` tag "shared" without a "crashes if you have a system
  libzstd.a" asterisk.

## What this task is

Under `BUILD_SHARED_LIBS=ON` on ELF, make `libarbc.so` take **system libzstd as
a shared `.so` dependency** (a `DT_NEEDED` entry resolved by the runtime loader)
rather than linking a system `libzstd.a` — which, when that archive is built
non-PIC (the common distro state for the static variant), links with bad
relocations and **SIGSEGVs inside `ZSTD_decompress`** the first time a tile blob
is decompressed. Then repair the install/export path so a *shared* `libarbc`
neither folds a non-PIC static archive into itself nor re-imposes `zstd` on the
embedder's final link line — a shared library encapsulates its private
dependencies, so the exported `arbc::arbc` config must ask the consumer for
nothing.

The predecessors left the *symbol* surface correct and the *static* zstd path
correct; the one uncovered corner is **shared `libarbc` × system-resolved
zstd**, and it is uncovered in two files:

1. **The `arbc_zstd` normalization shim** (`CMakeLists.txt:209-226`) prefers
   `zstd::libzstd_static` over `zstd::libzstd_shared` unconditionally — correct
   for a static `libarbc` (avoids a runtime `libzstd.so` on the embedder,
   doc 10:32-35), wrong for a shared one (folds/links a non-PIC archive into the
   `.so`).
2. **The install/export discriminator** (`cmake/ArbcInstall.cmake:45-54`) has
   two branches — fold the fetched plain `libzstd_static`, else emit
   `find_dependency(zstd)` + append the imported target to `arbc::arbc`'s
   `INTERFACE_LINK_LIBRARIES` — both written for a **static** `libarbc` (its own
   comment, `:32-40`). For a shared `libarbc` the append re-imposes `zstd` on
   the consumer's link line, the transitive burden doc 10:32-35 forbids.

The task lands four things:

1. **A `BUILD_SHARED_LIBS`-aware shim** — under a shared build, prefer
   `zstd::libzstd_shared` / `zstd::libzstd` (system `.so`) ahead of
   `zstd::libzstd_static`; the fetched plain `libzstd_static` (built PIC at
   `CMakeLists.txt:233-240`) stays a safe fallback to fold. Under a static
   build, the current static-first order is byte-unchanged.
2. **A shared branch in the install/export discriminator** — a shared `libarbc`
   that folds a PIC static (fetched) or `DT_NEEDED`s a system `.so` asks the
   consumer for nothing (both substitution vars empty,
   `cmake/arbcConfig.cmake.in:20,24`). The static-libarbc relink path is
   untouched.
3. **`gcc-shared` resolves system zstd** — install `libzstd-dev` on that lane so
   `find_package(zstd)` wins there, making the crashing combination the one CI
   actually exercises.
4. **The regression is pinned by existing tests** — `arbc_serialize_zstd_dep_smoke_t`
   (round-trip through `ZSTD_decompress`) and `install.consumer` both run under
   `ctest --preset shared`; with system zstd present they go from crash/relink-
   failure to green.

It does **not** touch the seam behavior (`compress_blob`/`decompress_blob`),
the `ARBC_API` export macro, the static-libarbc path, the Windows/MSVC lanes
(Constraint 7 of the MSVC predecessor keeps them out of scope), the plugin ABI,
or any design-doc normative text (Decision D4).

## Why it needs to be done

- **doc 17:13's "shared and static" is a live crash, not just an ELF-vs-Windows
  gap.** The predecessors made the symbol surface load on both toolchains, but
  on any Linux with `libzstd-dev` installed presenting a non-PIC `libzstd.a`,
  the shared build either fails to link `libarbc.so` (text-relocation error) or
  links and then SIGSEGVs in `ZSTD_decompress` at first save/load. A "shared"
  library that crashes on its first compression is not honestly shared.
- **doc 10:32-35 — "embedding the core must never transitively impose …" — is
  violated in the shared install path.** The `INTERFACE_LINK_LIBRARIES` append
  (`cmake/ArbcInstall.cmake:52-53`) is unconditional; for a shared `libarbc` it
  forces every embedder's link line to carry `zstd`, even though the `.so`
  already resolves it via `DT_NEEDED`. The fix restores the promise the shim's
  own comment (`CMakeLists.txt:211-213`) was reaching for — encapsulate the
  dependency — but does it the way a shared library actually can.
- **`packaging.release_01` should not tag a shared library that crashes on a
  stock developer machine.** The same anti-drift argument the two predecessors
  made: advertising "shared" while the shared build is untested against a
  system-resolved zstd would tag a surface the project does not actually keep.

## Inputs / context

### Design docs (normative, doc 16)

- **doc 10:27 — the zstd row of §Dependency policy.** *"chosen: `zstd`
  (`serialize.zstd_dep`) … Consumed find-first with a version-pinned
  `FetchContent` fallback, pinned in CI, mirroring the JSON wiring."* The
  consumption mechanism this task refines for the shared build; the choice
  itself is not reopened.
- **doc 10:32-35 — the transitive-burden clause.** *"consume through standard
  find mechanisms (`find_package`), never vendored copies in-tree … embedding
  the core must never transitively impose codecs, GPU SDKs, or a GUI toolkit."*
  `zstd` is **not** in that prohibited list — a runtime `libzstd.so` `DT_NEEDED`
  on a shared `libarbc` is a standard system dependency, not a codec/GPU/GUI
  burden — so linking the shared system zstd does **not** cross this line, while
  the current unconditional `INTERFACE_LINK_LIBRARIES` append (forcing the
  embedder to relink zstd) needlessly leans against it. This is the doc basis
  for Decisions D1/D2 and for **why no doc delta is needed** (D4).
- **doc 10:99-101 — SOVERSION rides the shared build.** `VERSION`/`SOVERSION`
  derive from `project(VERSION …)`; already set (`cmake/ArbcComponent.cmake:202-205`),
  inert here — no version work.
- **doc 17:13 — shipped artifact `libarbc` (shared and static).** The promise
  this task makes true for the shared variant's dependency link.
- **doc 17:173-193 — shared-library symbol resolution.** The `ARBC_API`/
  `-fvisibility=hidden` mechanism and the `shared_symbol_resolution` proof, both
  finished by the predecessors. Read for context; unchanged here (this task is
  about zstd's link shape, not `libarbc`'s exported symbols).
- **doc 17:195-226 — CMake mechanics.** `POSITION_INDEPENDENT_CODE ON` globally
  (`CMakeLists.txt:15`), the object-library components, and the one manual
  install-aggregation step. The global PIC setting is exactly why the *fetched*
  static zstd is safe to fold (`CMakeLists.txt:234` sets it on the fetched
  target too); a *system* `libzstd.a` predates our build and carries whatever
  PIC-ness the distro chose — which is the whole bug.
- **doc 17:254-263 — "The compressor does not cross this line."** Normatively
  places `zstd` **inside** `libarbc`: *"`zstd` compresses bytes we produced, in
  a container we defined, and parses no foreign file format."* This task keeps
  `zstd` internal on both linkage shapes; it changes only whether the shared
  `libarbc` reaches it via `DT_NEEDED` or a folded PIC archive — an
  implementation detail entirely below the codec line.
- **doc 08 Principle 8** — the tile blobs are *"per blob: zstd with a
  byte-shuffle"*; the content hash is over **uncompressed** bytes
  (`zstd_dep` Decision 1), so *which* zstd (system vs fetched, shared vs static)
  builds is unobservable in the saved format. This is why swapping the shared
  build to a system `.so` zstd changes nothing a document round-trips.

### Source seams

- **`CMakeLists.txt:13-17`** — global `CMAKE_POSITION_INDEPENDENT_CODE ON`,
  `CMAKE_CXX_VISIBILITY_PRESET hidden`. The PIC default covers *our* objects and
  the *fetched* zstd; it cannot retro-fit a system `libzstd.a`.
- **`CMakeLists.txt:194-207`** — the find-first `FetchContent_Declare(zstd …
  GIT_TAG v1.5.7 SOURCE_SUBDIR build/cmake FIND_PACKAGE_ARGS 1.5)` with
  `ZSTD_BUILD_SHARED OFF` / `ZSTD_BUILD_STATIC ON` (`:198-199`). Unchanged — the
  fetched fallback stays static-and-PIC, which is safe to fold on either linkage.
- **`CMakeLists.txt:209-226`** — the `arbc_zstd` INTERFACE shim. Preference
  cascade `zstd::libzstd_static` (`:215`) → `zstd::libzstd_shared` (`:217`) →
  `zstd::libzstd` (`:219`) → plain `libzstd_static` (`:221`) → `FATAL_ERROR`.
  Comment `:209-213` states static-first is deliberate, *"to keep a shared
  libarbc from imposing a runtime libzstd.so on its embedder"* — the rationale
  this task corrects: for a shared `libarbc`, a folded/linked static is the
  *worse* option (non-PIC crash), and a `DT_NEEDED` `.so` imposes nothing on the
  embedder's link line. **The edit site for Decision D1.**
- **`CMakeLists.txt:233-240`** — the fetched-only PIC + SYSTEM-include fixup;
  guarded `if(TARGET libzstd_static)` (the plain fetched name), so it never
  touches a system imported target. Read to confirm the fetched fold stays safe.
- **`src/serialize/CMakeLists.txt:35`** — `target_link_libraries(arbc_serialize
  PRIVATE arbc_zstd)`, the single PRIVATE link. Unchanged; the shim it names is
  what changes underneath it.
- **`cmake/ArbcInstall.cmake:31-54`** — the install/export zstd discriminator.
  `:45` reads `arbc_zstd`'s `INTERFACE_LINK_LIBRARIES`; `:48-49` folds the plain
  fetched `libzstd_static` via `target_sources(arbc PRIVATE
  $<TARGET_OBJECTS:libzstd_static>)`; `:50-54` else-branch emits
  `find_dependency(zstd 1.5)` + an unconditional `arbc::arbc`
  `INTERFACE_LINK_LIBRARIES` append. Comment `:32-40` scopes it to *"A STATIC
  libarbc carries its dependencies to its consumer's final link."* **The edit
  site for Decision D2** — a `BUILD_SHARED_LIBS` branch that asks the shared
  consumer for nothing.
- **`cmake/arbcConfig.cmake.in:20,24`** — the `@ARBC_ZSTD_FIND_DEPENDENCY@` and
  `@ARBC_ZSTD_LINK_INTERFACE@` substitution points the discriminator fills.
  Under the new shared branch both stay empty; no template edit needed.
- **`cmake/ArbcComponent.cmake:187-205,256-258`** — the umbrella `add_library(arbc
  …)` (no `STATIC`/`SHARED`), `VERSION`/`SOVERSION` off `PROJECT_VERSION`, and
  the PRIVATE `$<BUILD_INTERFACE:arbc_${name}>` component links. Confirms
  `arbc_serialize`'s PRIVATE `arbc_zstd` link propagates a `DT_NEEDED` onto
  `libarbc.so` without appearing in the umbrella's `INTERFACE_LINK_LIBRARIES` —
  the property Decision D2 relies on.
- **`tests/CMakeLists.txt:56-59`** — `arbc_serialize_zstd_dep_smoke_t`, linking
  `arbc` alone and round-tripping a realistic tile blob through
  `compress_blob`/`decompress_blob`. Runs on every lane, including
  `gcc-shared`; it is the behavioral pin that *executes* `ZSTD_decompress` and
  therefore catches the SIGSEGV.
- **`tests/CMakeLists.txt:1333-1361`** — the `install.consumer` relink CTest
  (`RUN_SERIAL TRUE`, `:1350`; WIN32-only DLL-on-PATH wiring, `:1358-1361`).
- **`tests/consumer/run_staged_install.cmake`** — stages `cmake --install` and
  relinks the foreign consumer twice (plugin-author + embedder). Its comment
  (`:33-37`) describes a *static* libarbc; under the `shared` preset it stages a
  shared `libarbc.so` and relinks against it — the path that fails today and
  passes after D2.
- **`tests/consumer/CMakeLists.txt`** — `core_only` links `arbc::arbc` alone
  (`:66-68`); this is the target that would carry a spurious `zstd` on its link
  line under the current unconditional append, and that must relink cleanly
  after D2.
- **`.github/workflows/ci.yml:84`** — `{ name: gcc-shared, os: ubuntu-latest,
  cxx: g++, preset: shared }`, the lane that resolves system zstd once
  `libzstd-dev` is installed (Decision D3) and runs the full `ctest --preset
  shared` including the two pinning tests.
- **`tests/CMakeLists.txt:1252-1281`** — `arbc_shared_symbol_resolution_t`, the
  `.dynsym`/PE symbol proof. Read to confirm this task adds nothing to it: it
  proves *plugin↔host symbol resolution*, an axis orthogonal to zstd's link
  shape.

### Predecessor / sibling refinements

[`shared_library_build_msvc.md`](./shared_library_build_msvc.md) (source-of-debt;
its Status `:534` and Constraint 7 `:345-350`),
[`shared_library_build.md`](./shared_library_build.md) (Constraint 7 / D4 — the
scoped-out non-goal this task discharges; the PIC-fold seam at
`ArbcInstall.cmake:45-54`),
[`../serialize/zstd_dep.md`](../serialize/zstd_dep.md) (Decisions 2/3 — the shim
and the PRIVATE link; the `find_dependency(zstd)` export obligation this task
completes).

## Constraints / requirements

1. **The shared build must not link a non-PIC static zstd into `libarbc.so`.**
   Under `BUILD_SHARED_LIBS=ON`, the shim prefers the system **shared** zstd
   (`zstd::libzstd_shared` / `zstd::libzstd`) so `libarbc.so` gets a `libzstd.so`
   `DT_NEEDED`; the only static it may fold is the *fetched* `libzstd_static`,
   which is built PIC (`CMakeLists.txt:234`). A system `zstd::libzstd_static` is
   the last resort on the shared build, never the preference (Decision D1).

2. **The static build is byte-for-byte unchanged.** With `BUILD_SHARED_LIBS`
   OFF the shim keeps `zstd::libzstd_static` first (avoiding a runtime
   `libzstd.so` on the embedder, doc 10:32-35), and the install discriminator
   keeps its current fold / `find_dependency` + append behavior. All existing
   static lanes configure, build, and test identically (Decision D1/D2).

3. **A shared `libarbc` imposes no zstd on the embedder's link line.** The
   install/export path leaves both `arbcConfig.cmake.in` substitution vars empty
   for a shared `libarbc` (the dependency is folded PIC or `DT_NEEDED`-resolved),
   so the exported `arbc::arbc` never references `zstd::*` and the consumer's
   `find_package(arbc)` pulls nothing extra (Decision D2, doc 10:32-35).

4. **The regression is exercised where it crashed.** The `gcc-shared` lane
   installs `libzstd-dev` so `find_package(zstd)` resolves the system copy —
   reproducing system-zstd × shared-libarbc — and `ctest --preset shared` runs
   both `arbc_serialize_zstd_dep_smoke_t` (executes `ZSTD_decompress`) and
   `install.consumer` (relinks `arbc::arbc`) green (Decision D3). A developer's
   local shared build without `libzstd-dev` falls to the fetched PIC static and
   is equally green; both paths are covered.

5. **No seam-behavior change.** `compress_blob`/`decompress_blob`, the one-shot
   stateless `ZSTD_*` calls (`zstd_dep` Decision 6), and the bounded
   untrusted-input decompression (`zstd_dep` Decision 7) are untouched. This is a
   link-shape fix; the saved format and the seam's error semantics are
   invariant (doc 08 Principle 8: the hash is over uncompressed bytes, so which
   zstd links is unobservable).

6. **No new component-graph edge.** `scripts/check_levels.py` scans
   `#include <arbc/…>` only; a link-preference reshuffle in the shim adds no
   include and the checker stays green unchanged (same finding `zstd_dep`
   Constraint 5 recorded).

7. **Lints/gate.** The edited CMake passes `gersemi` + `cmake-lint`; the
   `gcc-shared` apt step matches the workflow's existing dependency-install
   idiom; the C++ lanes stay green under `-Wall -Wextra -Wpedantic -Werror`
   (no C++ changed). Because the crash reproduces only under a shared build with
   a system zstd, the load-bearing pre-commit signal is CI-green on
   `gcc-shared` — the same "CI is the shared-lane gate" posture the predecessors
   accepted.

## Acceptance criteria

- **`gcc-shared` goes green against a *system* zstd (headline).** The lane
  installs `libzstd-dev` (extending the workflow's existing Linux dependency
  step, or a lane-scoped `apt-get install -y libzstd-dev` gated on the
  `gcc-shared` row), so `find_package(zstd)` resolves the system copy and the
  shared `libarbc.so` links the shared `libzstd.so`. `ctest --preset shared`
  then runs the full suite green — critically `arbc_serialize_zstd_dep_smoke_t`
  (`tests/CMakeLists.txt:56-59`), which byte-exactly round-trips a realistic
  tile blob through `ZSTD_decompress` and thereby *executes* the code path that
  SIGSEGV'd. This is an observable CI outcome on a runner the project already
  uses, and it is a genuine regression gate: reverting D1 makes this lane crash.

- **`install.consumer` relinks a shared `libarbc` cleanly, no transitive zstd.**
  The out-of-tree `install.consumer` CTest (`tests/CMakeLists.txt:1333-1361`),
  run under the `shared` preset on `gcc-shared`, stages and relinks the foreign
  `core_only` consumer (`tests/consumer/CMakeLists.txt:66-68`) against
  `arbc::arbc` via `find_package(arbc CONFIG)`. It passes with the exported
  config referencing **no** `zstd::*` target and requiring **no**
  `find_dependency(zstd)` — proving a shared `libarbc` encapsulates its
  compressor and the embedder's link line is clean (Constraint 3). The embedder
  path (`-DCMAKE_DISABLE_FIND_PACKAGE_Catch2=ON`) in particular must not fail on
  a dangling `zstd` reference.

- **The static lanes and the fetched-zstd path are provably unchanged.** Every
  existing static lane (`gcc-*`, `clang-*`, `msvc-debug`) and the Windows
  `msvc-shared` lane build and test exactly as before; a shared build with no
  system zstd (fetched PIC static, folded) stays green. The static-libarbc
  `install.consumer` relink still emits `find_dependency(zstd 1.5)` +
  the interface append when a system zstd was used, and nothing when the fetched
  static was folded (`zstd_dep`'s original obligation, preserved).

- **No new golden, conformance, claims-register, or behavioral-counter suite —
  the same deliberate reading the two predecessors made.** This task adds no
  content kind or operator (no conformance run), renders no new pixels (no
  golden), and makes no wall-clock promise (no behavioral counter). Its promise
  is a **build-and-link invariant on the shared ELF lane**, and doc 16's
  taxonomy pins that with the CI lane + the two existing tests that already
  execute the crashing path — not a new registry row. (The invariant "a shared
  `libarbc` encapsulates zstd and imposes none on the embedder" is a property of
  the *build*, observed by `install.consumer`, exactly as the predecessors let
  `shared_symbol_resolution` + the plugin loads pin their build invariant with
  no new claim.)

- **Diff coverage ≥90% is not gated by this task.** The changes are CMake
  (`CMakeLists.txt`, `cmake/ArbcInstall.cmake`) and CI YAML — no instrumented
  C++ lines change, so they are absent from `coverage.xml` and the
  `diff-cover --fail-under=90` gate neither rises nor falls, exactly as the
  predecessors handled their CMake-only diffs.

- **Levelization + build + WBS gate green.** `scripts/check_levels.py` passes
  unchanged (no new `arbc_*` include edge); `scripts/check_claims.py` passes (no
  registry change); and after the closer adds `complete 100` + the `note`
  back-link to `tasks/75-packaging.tji`, `tj3 project.tjp 2>&1 | grep -iE
  "error|warning"` is silent. M9's `m9_release` already lists this leaf
  (`tasks/99-milestones.tji:72`), so no milestone edit is needed — but if this is
  the last open M9 dependency at close, the milestone gets its own `complete
  100` per the ritual.

**No deferred WBS leaves.** This closes the shared-zstd link story on the one
platform it bites (ELF); Windows was never affected (MSVC Constraint 7). The
single genuine edge case — a system that presents *only* a non-PIC
`zstd::libzstd_static` with no shared sibling, under a shared `libarbc` build —
is handled by documentation, not a task: the shim still selects it as a last
resort, and the CMake comment records the escape hatch
(`FETCHCONTENT_TRY_FIND_PACKAGE_MODE=NEVER` forces the PIC fetched fallback, or
the packager installs the shared `libzstd`). That is a defensible make-the-call
resolution per the design-doc-level-decisions rule, not an "audit" leaf; it is
surfaced to the return summary for the parking lot as an informational note, not
encoded as WBS work.

## Decisions

**D1. The `arbc_zstd` shim's target preference is `BUILD_SHARED_LIBS`-aware:
system *shared* zstd first under a shared `libarbc`, system *static* first under
a static one.** Under `BUILD_SHARED_LIBS=ON` the cascade becomes
`zstd::libzstd_shared` → `zstd::libzstd` → plain (fetched, PIC) `libzstd_static`
→ `zstd::libzstd_static` (last resort); under OFF it keeps today's
static-first order. *Rationale:* the two linkage shapes have opposite correct
answers. A static `libarbc` should fold/relink a static zstd to avoid imposing a
runtime `libzstd.so` on its embedder (doc 10:32-35 — the shim's original intent,
`CMakeLists.txt:211-213`). A shared `libarbc` must do the reverse: folding or
linking a system `libzstd.a` of unknown PIC-ness into a `.so` is the crash, while
a `DT_NEEDED` `libzstd.so` imposes nothing on the embedder's *link* line (the
loader resolves it, and `libzstd` is not a codec/GPU/GUI burden doc 10:32-35
prohibits). The fetched static stays a safe fallback on both shapes because
`CMakeLists.txt:234` builds it PIC.
*Rejected: prefer shared unconditionally (both shapes).* Regresses the static
`libarbc`, forcing a runtime `libzstd.so` on embedders who deliberately link
statically — the exact burden the shim was built to avoid.
*Rejected: a new `ARBC_ZSTD_SHARED` CMake toggle.* A needless knob: the correct
choice is a deterministic function of `BUILD_SHARED_LIBS`, not a user preference,
and every extra cache variable is one more untested configuration.
*Rejected: force `ZSTD_BUILD_SHARED ON` on the FetchContent fallback under a
shared build.* Would ship someone else's `libzstd.so` — an in-tree-built shared
dependency the project must then install and version — reintroducing the very
transitive-artifact burden doc 10:32-35 warns against; the PIC static fetched
fallback folds cleanly and ships nothing extra.

**D2. The install/export discriminator gains a shared-`libarbc` branch that asks
the consumer for nothing.** Fold the fetched plain `libzstd_static` as today;
`elseif(NOT BUILD_SHARED_LIBS)` keep the static-libarbc `find_dependency(zstd
1.5)` + `INTERFACE_LINK_LIBRARIES` append; `else()` (shared `libarbc`) leave both
`arbcConfig.cmake.in` substitution vars empty. *Rationale:* a shared `libarbc`
links its PRIVATE `zstd` into the `.so` itself — either folded (fetched PIC
static) or recorded as `DT_NEEDED` (system shared) — and because the component
link is PRIVATE through the umbrella
(`cmake/ArbcComponent.cmake:256-258`), `zstd` never enters `arbc::arbc`'s
`INTERFACE_LINK_LIBRARIES`. So the exported config correctly references no
`zstd::*` and needs no `find_dependency` (which would otherwise dangle if the
consumer's machine lacks a `zstdConfig.cmake`). The current unconditional append
is the shared relink failure the MSVC task surfaced.
*Rejected: emit `find_dependency(zstd)` for the shared build too "to be safe."*
It re-imposes `zstd` on every embedder's `find_package(arbc)` and their link
line for a dependency the `.so` already resolves — the doc 10:32-35 burden — and
makes `find_package(arbc)` fail on a machine that has `libzstd.so` (runtime) but
no `zstdConfig.cmake` (dev package), which is a perfectly valid deployment.
*Rejected: keep folding for the shared build via `target_sources`.* Only the
fetched target exposes `$<TARGET_OBJECTS:…>`; a system imported archive has no
object set to fold, and folding a non-PIC one is the crash. The shared build's
answer is `DT_NEEDED`, not fold.

**D3. The `gcc-shared` lane installs `libzstd-dev` so the fix is tested against a
system zstd.** *Rationale:* the bug only manifests when `find_package(zstd)`
resolves a system copy; without `libzstd-dev` the lane silently falls to the
fetched PIC static, folds it, and never reproduces the crash — a green lane that
proves nothing. Installing `libzstd-dev` makes the *system* path the one under
test, so `arbc_serialize_zstd_dep_smoke_t` and `install.consumer` become a real
regression gate for D1/D2. The existing tests already execute the exact code
(`ZSTD_decompress`, `arbc::arbc` relink); no new test authoring is needed.
*Rejected: a dedicated "assert `libarbc.so` has a `libzstd` `DT_NEEDED`" scan
test.* Environment-dependent — the fetched-fold path (a valid, green
configuration) has *no* `DT_NEEDED`, so the assertion would be conditional on
which zstd resolved, and it would prove less than the behavioral round-trip
(which catches an actual non-PIC crash, not just a missing tag). Behavior beats
structure here.

**D4. No design-doc delta; no doc-00 bullet.** *Rationale:* doc 10:27,32-35 and
doc 17:195-226,254-263 already fully specify the envelope — `zstd` is an internal
find-first dependency of `libarbc`, consumed shared or static, that must not be
transitively imposed on embedders. This task makes the shared-build linkage
*conform* to that already-written policy; it decides no new architecture. The
static-vs-shared *preference* was a CMake-comment rationale
(`CMakeLists.txt:209-213`), never normative doc text, so correcting it for the
shared case is a build-mechanics fix, and its comment is updated in-place in the
same commit — no `docs/design/NN` normative sentence changes and no
constitutional decision is made.
*Rejected: record a doc-10 note that a shared `libarbc` takes a runtime
`libzstd.so`.* That fact is already inside doc 10:32-35's envelope (`zstd` is not
a prohibited transitive burden); spelling out an implementation-level linkage
detail in the constitution over-specifies it and invites future drift when the
mechanism changes. The refinement's Decisions are the right home for it.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-16.

- `CMakeLists.txt` — `arbc_zstd` shim made `BUILD_SHARED_LIBS`-aware (D1): under a shared build, prefers `zstd::libzstd_shared` → `zstd::libzstd` → fetched `libzstd_static` → `zstd::libzstd_static` (last resort); static order byte-unchanged.
- `cmake/ArbcInstall.cmake` — install/export discriminator gains `elseif(NOT BUILD_SHARED_LIBS)` for the static `find_dependency` + append branch; shared `libarbc` leaves both `@ARBC_ZSTD_FIND_DEPENDENCY@` and `@ARBC_ZSTD_LINK_INTERFACE@` substitution vars empty (D2).
- `cmake/ArbcComponent.cmake` — per-target `BUILD_WITH_INSTALL_RPATH`/`INSTALL_RPATH_USE_LINK_PATH` on the shared umbrella suppress a spurious install-time relink that `cmake --install` never builds, without touching test executables (work-budget item 4).
- `.github/workflows/ci.yml` — `gcc-shared` lane installs `libzstd-dev` so `find_package(zstd)` resolves the system shared copy, turning the existing `arbc_serialize_zstd_dep_smoke_t` + `install.consumer` into a real regression gate (D3).
- `libarbc.so` now records `DT_NEEDED libzstd.so.1`; smoke + `install.consumer` + 13 rpath/plugin tests green on the shared lane; exported `arbc::arbc` config carries no active zstd reference.
