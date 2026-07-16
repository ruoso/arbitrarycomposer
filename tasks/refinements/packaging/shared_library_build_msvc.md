# packaging.shared_library_build_msvc — MSVC shared-build lane for libarbc

## TaskJuggler entry

[`tasks/75-packaging.tji:42-47`](../../75-packaging.tji) —

```
task shared_library_build_msvc "MSVC shared-build lane for libarbc" {
  effort 1d
  allocate team
  depends !shared_library_build
  note "Add a Windows/MSVC BUILD_SHARED_LIBS=ON lane and verify ARBC_API's __declspec(dllexport)/__declspec(dllimport) branches are correct across the plugin LoadLibrary boundary — the one platform where the export/import asymmetry bites and where MODULE resolution and DLL search paths differ. Source-of-debt: tasks/refinements/packaging/shared_library_build.md (Decision D4). Docs 03/17."
}
```

Milestone: **M9** (`tasks/99-milestones.tji:70-73`, the v0.1 release) — `m9_release`
already names `packaging.shared_library_build_msvc` in its `depends` list
(`:72`), directly beside `packaging.shared_library_build`. It gates nothing else
in M9. The release ships `libarbc` in *"shared and static"* form (doc 17:13); the
ELF predecessor made that true on Linux, and this task makes it true on the one
other platform the project already builds — Windows/MSVC — so the "shared" promise
is not silently ELF-only.

## Effort estimate

**Booked 1d; realistic 1d.** This is a Windows-parity leaf in the exact mould of
`runtime.plugin_loading_win32` (1d, Done 2026-07-10): the design and most of the
code already exist and are Windows-complete; the work is standing up the lane,
writing the platform-forked proof, wiring the DLL-search path, and debugging the
MSVC-specific fallout the new lane surfaces. Critically, **the `ARBC_API` macro's
`__declspec` branch is already written** — `src/api/arbc/arbc_api.h:39-44` carries
the `dllexport`/`dllimport` split keyed on `ARBC_BUILDING`, threaded onto every
object library and the umbrella (`cmake/ArbcComponent.cmake:58,193`) by the
predecessor. This task does not author the macro; it *proves the branch correct
across the LoadLibrary boundary* and lands the lane that keeps it correct.

Budget:

- 0.2d — the `win-shared` preset (inherit `win-dev`, add `BUILD_SHARED_LIBS: ON`)
  and the `msvc-shared` CI matrix lane (one `include` row on `windows-latest`,
  reusing the already-present msvc-dev-cmd + ninja setup steps).
- 0.4d — the PE/COFF symbol-resolution proof: fork the reader in
  `tests/shared_symbol_resolution.t.cpp` behind `#if defined(_WIN32)` — parse
  `arbc.dll`'s PE export directory and each plugin `.dll`'s PE import directory —
  keeping the shared TEST_CASE bodies, core-symbol list, and assertions
  (Decision D2).
- 0.2d — DLL-search wiring: make `arbc.dll` findable by the plugin `.dll`s and the
  installed-consumer at load, on the shared Windows lane only (Decision D4).
- 0.2d — debugging the MSVC-specific export/RTTI/search fallout the green-lane
  requirement surfaces (Decision D3), and the doc-17 delta discharging the
  remaining honesty sentence (Decision D5).

If the lane surfaces a *broad* `ARBC_API`-placement bug — a polymorphic contract
base whose vtable/RTTI must cross the DLL boundary but is annotated only
per-method — the annotation fix is in scope and concrete (adjust the class-level
`ARBC_API` placement); but the ELF proof already asserts `typeinfo for
arbc::Content` is exported (`shared_symbol_resolution.t.cpp:151`), which means the
polymorphic contract base is *already* class-level-exported, so the RTTI base most
likely to break is already correct. That bounds the risk to 1d.

## Inherited dependencies

**Settled (formal `depends`, `tasks/75-packaging.tji:45`):**

- **`packaging.shared_library_build`**
  ([`./shared_library_build.md`](./shared_library_build.md), **Done 2026-07-15**) —
  the source-of-debt, and the reason this task exists in the shape it does. Its
  **Decision D4** (`./shared_library_build.md:517-535`) scoped exactly this split:

  > **D4. The new lane is Linux/gcc `BUILD_SHARED_LIBS=ON` with find-first (system)
  > zstd; MSVC stays static and shared+fetched-zstd is a non-goal.** … On ELF
  > `ARBC_API` is the symmetric `visibility("default")`, so the export/import
  > asymmetry that makes MSVC harder simply does not arise, and the proof is clean.
  > *Rejected: add the MSVC shared lane in this task.* The `__declspec`
  > dllexport/dllimport branch, DLL search-path and MODULE-resolution differences,
  > and Windows load debugging are a distinct concrete chunk — registered as
  > `packaging.shared_library_build_msvc` (M9). Bundling it risks a half-working
  > Windows shared build, which D6's own logic (a half-annotated surface is worse
  > than none) argues against.

  The predecessor left every seam this task needs already MSVC-shaped:
  - **`src/api/arbc/arbc_api.h:39-44`** — the `_WIN32` guard with
    `#if defined(ARBC_BUILDING)` → `__declspec(dllexport)` / `__declspec(dllimport)`,
    and the ELF `visibility("default")` `#else`. The header comment (`:20-34`) states
    the asymmetry *"only bites on MSVC, where the shared build is the deferred
    follow-up `packaging.shared_library_build_msvc`"* — i.e. it names this task. The
    macro is already declspec-complete; only its correctness-under-load is unproven.
  - **`cmake/ArbcComponent.cmake:58,193`** — `ARBC_BUILDING` as a PRIVATE compile
    define on every `arbc_<component>` object library and on the umbrella, so every
    TU compiled into `libarbc` takes the *export* branch under both toolchains.
  - **`cmake/ArbcComponent.cmake:202-205`** — `VERSION`/`SOVERSION` off
    `PROJECT_VERSION`; inert on Windows (no soname concept; `arbc.dll` is unversioned),
    so no version work is left here (Decision D6-note below).
  - **`cmake/ArbcInstall.cmake:60-66`** — `install(TARGETS arbc …)` already routes
    `ARCHIVE`→`lib` (`:63`), `LIBRARY`→`lib` (`:64`), `RUNTIME`→`bin` (`:65`)
    unconditionally, which is exactly the Windows split: the `arbc.dll` lands in
    `bin`, the import `arbc.lib` in `lib`. **No install edit is needed** for the
    Windows shared artifact.
  - **`tests/shared_symbol_resolution.t.cpp`** — the ELF64 proof
    (`#include <elf.h>`, `.dynsym` walk, `SHN_UNDEF`/`STB_GLOBAL`/`STV_DEFAULT`),
    gated `if(BUILD_SHARED_LIBS)` (`tests/CMakeLists.txt:1234`). Its comment already
    records that it is *"ELF-only, which is exactly the scope of the gcc
    `BUILD_SHARED_LIBS` lane; the MSVC shared build is
    `packaging.shared_library_build_msvc`"* — the file this task extends with the PE
    branch.

- **`kinds.dual_build` / `kinds.imageseq_plugin`** (transitive, via the predecessor)
  — built the eight plugin modules this task re-runs shared on Windows: the six
  `arbc-ci-plugin-{solid,tone,raster,nested,fade,crossfade}` and the shipped
  `arbc-plugin-imageseq` / `arbc-plugin-miniaudio`, each a `MODULE` linking `arbc`
  PRIVATE and loaded through the production `runtime::PluginHost`
  (`tests/CMakeLists.txt:1202-1222`).

**Informal, but the real context:**

- **`runtime.plugin_loading_win32`**
  ([`../runtime/plugin_loading_win32.md`](../runtime/plugin_loading_win32.md),
  **Done 2026-07-10**) — landed the Windows dynamic-loader backing this task's
  shared plugins load *through*: `src/runtime/plugin_host.cpp:101-147` uses
  `LoadLibraryA` (documented at `:104-106` as the faithful analog of
  `dlopen(RTLD_NOW | RTLD_LOCAL)`), `GetProcAddress`, `FreeLibrary`, and
  `FindFirstFileA`/`FindNextFileA` enumeration (`:222-247`). Its **Decision 2**
  established the decisive fact this task builds on: **the Windows CI lane already
  exists** — `msvc-debug` on `windows-latest` with the `win-dev` preset
  (`.github/workflows/ci.yml:85`; `CMakePresets.json:78-84`), present since the
  bootstrap commit. That lane is *static*; this task adds its *shared* sibling.
  Its parity-across-platforms testing pattern (re-enforce existing claims with the
  same assertion bodies, add no registry rows) is the template this task follows
  (Decision D2, claims note).

- **`packaging.version_api`** ([`./version_api.md`](./version_api.md), Done
  2026-07-14) — the single-source `PROJECT_VERSION` that feeds `VERSION`/`SOVERSION`;
  nothing new here, the properties are already set and Windows-inert.

**Pending, and deliberately not a dependency:**

- **`packaging.install`** (`tasks/75-packaging.tji:6-10`) — owns pkg-config, CPS,
  `VERIFY_INTERFACE_HEADER_SETS`, CPack, and the plugin-artifact install layout
  (`cmake/ArbcInstall.cmake:17-19`). No edge: the umbrella install seam
  (`:60-66`) already routes the Windows `.dll`/`.lib` correctly, so this task waits
  on nothing from `install`.
- **`packaging.plugin_helper`** — the eight plugin modules stay hand-rolled, as the
  predecessor and the win32 loader left them.

**Downstream (this task serves):**

- **`packaging.release_01`** (`tasks/75-packaging.tji:29-33`) — a released `libarbc`
  advertised as *"shared and static"* (doc 17:13) is only honest cross-platform once
  the shared build works on *both* toolchains the project ships. The ELF task made it
  true on Linux; this makes it true on Windows, so `release_01` can tag it without an
  ELF-only asterisk.

## What this task is

Stand up a Windows/MSVC `BUILD_SHARED_LIBS=ON` lane for `libarbc`, and prove that on
MSVC — where `ARBC_API` is the *asymmetric* `__declspec(dllexport)`/`(dllimport)`
pair rather than ELF's symmetric `visibility("default")` — a plugin `.dll` loaded
by the production `PluginHost` resolves core symbols from the single `arbc.dll` in
the process, exactly as the ELF lane proved for `libarbc.so`.

The predecessor made the *code* Windows-ready: the macro's declspec branch, the
`ARBC_BUILDING` threading, the install RUNTIME/ARCHIVE split, and the `LoadLibrary`
loader all exist. What is unproven — and what MSVC's export/import asymmetry can
break where ELF cannot — is whether the annotated surface actually exports enough
(and imports it correctly) for the plugin boundary to work as a single-instance
DLL. The task lands four things:

1. **The `win-shared` preset** — a configure/build/test trio in `CMakePresets.json`
   inheriting `win-dev` (Ninja + MSVC) with `BUILD_SHARED_LIBS: ON`, producing
   `arbc.dll` + its import lib.
2. **The `msvc-shared` CI lane** — one `include` row on `windows-latest` in
   `.github/workflows/ci.yml`, reusing the msvc-dev-cmd + ninja setup already gated
   `runner.os == 'Windows'`, running the full `ctest` suite against `arbc.dll`.
3. **The PE/COFF single-instance proof** — a `#if defined(_WIN32)` branch inside
   `tests/shared_symbol_resolution.t.cpp` that reads `arbc.dll`'s PE **export**
   directory and each plugin `.dll`'s PE **import** directory, asserting the same
   property the ELF branch asserts against `.dynsym`: each plugin imports core
   `arbc::`-namespaced symbols *from `arbc.dll`* (proving no private static copy),
   and `arbc.dll` exports them.
4. **The DLL-search wiring** — make `arbc.dll` findable at load by the plugin `.dll`s
   and the installed consumer on the shared Windows lane, since Windows resolves the
   plugin's `arbc.dll` import via the DLL search order (no ELF rpath / already-loaded
   global namespace).

It does **not** re-author `ARBC_API` (already declspec-complete), touch the plugin
entry ABI (`ARBC_PLUGIN_EXPORT`, Decision D3-note), change the static `msvc-debug`
lane or any ELF lane, or blanket-export via `WINDOWS_EXPORT_ALL_SYMBOLS` (Decision
D1). Fixing a *specific* `ARBC_API` placement the MSVC lane proves insufficient
(e.g. a class whose vtable/RTTI must cross) is in scope; wholesale re-annotation is
not expected (the RTTI base is already class-level-exported, per the ELF assertion).

## Why it needs to be done

- **doc 17:13's "shared and static" is currently ELF-only.** The predecessor
  discharged the honesty limit *"on the ELF/shared lane"* (doc 17:173-174) and doc
  17:188-192 explicitly records the remaining gap: *"The remaining piece is the
  Windows/MSVC shared build, where `ARBC_API`'s `__declspec(dllexport)`/`(dllimport)`
  asymmetry (unlike ELF's symmetric `visibility("default")`) and DLL search /
  `LoadLibrary` module resolution differ: that is `packaging.shared_library_build_msvc`."*
  This task is that sentence — and closing it flips the doc from "remaining piece"
  to "proven on both toolchains" (Decision D5).
- **The asymmetry is where the annotation can be *wrong* invisibly.** On ELF a
  missing/misplaced `ARBC_API` degrades to "not exported" and the ELF proof catches
  it. On MSVC the same annotation error can manifest differently — a class exported
  member-wise but not class-wise fails to export its vtable/RTTI, so `dynamic_cast`
  or virtual dispatch across the DLL boundary breaks at *load or call* time, not
  link time. The gcc lane cannot see this; only an MSVC shared lane can. The
  project's whole posture (doc 17:174-176) is to export *"exactly the deliberate
  public API and nothing else"* — which only holds if the deliberate surface is
  actually sufficient on the asymmetric toolchain.
- **`packaging.release_01` should not tag a shared library that loads on only one
  of the two platforms it builds.** The same anti-drift argument the predecessor
  (`./shared_library_build.md:197-199`) and `version_api` made: advertising "shared"
  while only ELF-shared has ever loaded a plugin would tag a surface the project does
  not actually keep on Windows.

## Inputs / context

### Design docs (normative, doc 16)

- **Doc 17:173-192 — the honesty limit, now half-discharged.** `:173-188` records
  the ELF discharge (the `gcc-shared` lane, the `ARBC_API`-annotated surface, the
  `.dynsym` proof). `:188-192` names *this* task as the remaining piece, calling out
  the two Windows-specific hazards verbatim: the `__declspec` **export/import
  asymmetry** and **DLL search / `LoadLibrary` module resolution**. This paragraph's
  final sentence becomes false when this task lands; doc 16's same-commit rule
  requires the closer to rewrite it (Decision D5).
- **Doc 17:194-203 — the CMake mechanics.** `POSITION_INDEPENDENT_CODE ON` (a no-op
  on Windows, where object code carries no PIC/non-PIC distinction); the export macro
  keyed on `ARBC_BUILDING`. This is the doc's own instruction, already realized; the
  Windows lane exercises it.
- **Doc 03:227-243 — the two-stage plugin ABI.** Stage 1 (v1): a single
  `extern "C" arbc_plugin_register(Registry&)` entry, same-toolchain coupling
  acceptable. `ARBC_API` governs how `libarbc`'s C++ surface exports from `arbc.dll`;
  it is a Stage-1 concern and does not touch the entry symbol or a capability-flag
  scheme — identically on Windows (Decision D3).
- **Doc 16:143-147 — versioning.** Pre-1.0 moves freely; ABI checking joins CI at
  1.0. On Windows there is no soname to bump anyway, so `SOVERSION` is simply inert
  and the pre-1.0 policy needs nothing added here.

### Source seams

- **`src/api/arbc/arbc_api.h:36-49`** — the active branch logic: `#if defined(_WIN32)`
  (`:39`) → `#if defined(ARBC_BUILDING)` (`:40`) → `__declspec(dllexport)` (`:41`) /
  `__declspec(dllimport)` (`:43`); `#else` → `visibility("default")` (`:46`). The
  MSVC path this task proves. Comment `:20-34` documents the asymmetry and names this
  task.
- **`tests/shared_symbol_resolution.t.cpp`** — the ELF64 proof to fork:
  `#include <elf.h>` (`:33`), `#include <cxxabi.h>` (`:32`, `abi::__cxa_demangle`);
  `read_dynsyms()` (`:68-114`, memcpy `Elf64_Ehdr`/`Shdr`/`Sym`, walk `SHT_DYNSYM`);
  core-symbol asserts `"arbc::Registry::add"` + `"typeinfo for arbc::Content"`
  (`:150-151`); the per-plugin undefined-import loop (`:154-186`); the
  `// enforces: 17-internal-components#plugin-resolves-core-symbols-from-host-image`
  tag (`:8`). The `#if defined(_WIN32)` PE branch slots into the reader layer; the
  TEST_CASE bodies and the core-symbol list are shared (Decision D2).
- **`tests/CMakeLists.txt:1224-1257`** — the `if(BUILD_SHARED_LIBS)` block
  (`:1234`) building `arbc_shared_symbol_resolution_t` (`:1235`), passing
  `ARBC_LIBARBC_FILE` + the eight plugin `$<TARGET_FILE:…>` as compile defs
  (`:1238-1248`), with `add_dependencies` (`:1251-1255`). The gate stays
  `if(BUILD_SHARED_LIBS)` — the target compiles on *both* shared lanes and selects
  its reader by platform. The Windows target additionally links `dbghelp` for
  `UnDecorateSymbolName` (Decision D2).
- **`CMakePresets.json`** — the `shared` trio (configure `:94-102` inheriting `dev`
  with `BUILD_SHARED_LIBS: ON`; build `:115`; test `:128`) and the `win-dev` trio
  (configure `:78-84`, Ninja + MSVC, inherits `dev`; build `:113`; test `:126`). The
  `win-shared` trio is `win-dev` + `BUILD_SHARED_LIBS: ON`.
- **`.github/workflows/ci.yml:44-85`** — the `build-test` matrix (`fail-fast: false`);
  `gcc-shared` at `:84`, static `msvc-debug` / `windows-latest` / `win-dev` at `:85`;
  the `ilammy/msvc-dev-cmd@v1` step gated `runner.os == 'Windows'` (`:90-92`); the
  ninja-install step (`:93-95`); the `CXX` export gated on non-empty `matrix.cxx`
  (`:110`, so MSVC uses default `cl`). A new lane is one `include` row reusing all of
  this.
- **`cmake/ArbcComponent.cmake:58`** (`ARBC_BUILDING` per component), **`:193`**
  (umbrella), **`:202-205`** (`VERSION`/`SOVERSION`) — all already correct; this task
  reads them, edits none.
- **`cmake/ArbcInstall.cmake:60-66`** — the `ARCHIVE`/`LIBRARY`/`RUNTIME`
  destinations already route the Windows `.dll`→`bin`, `.lib`→`lib`; no edit.
- **`src/runtime/plugin_host.cpp:101-147`** — the `LoadLibraryA` backing the shared
  plugins load through; `:104-106` documents it as the `dlopen(RTLD_NOW | RTLD_LOCAL)`
  analog. The DLL-search concern (Decision D4) lives here: once a plugin `.dll` is
  `LoadLibrary`'d, its `arbc.dll` import is resolved by the OS DLL search order.
- **`src/contract/arbc/contract/plugin.hpp:8-12`** — the `ARBC_PLUGIN_EXPORT`
  declspec branch (already Windows-complete); the plugin entry symbol this task keeps
  distinct from `ARBC_API` (Decision D3).
- **`tests/CMakeLists.txt:1202-1222`** (`arbc_dual_build_t`, the production-
  `PluginHost` driver), **`:1153-1169`** (imageseq/miniaudio loads + the three
  containment scans), **`:1041-1079`** (`install.consumer`) — the existing suites the
  `msvc-shared` lane re-runs against `arbc.dll` for free; the DLL-search wiring
  (Decision D4) touches the tests that `LoadLibrary` a plugin or the installed lib.
- **`tests/claims/registry.tsv`** — the claim
  `17-internal-components#plugin-resolves-core-symbols-from-host-image` the PE proof
  re-enforces (no new row, Decision D2).

## Constraints / requirements

1. **`ARBC_API` is not re-authored — its `__declspec` branch is verified, and fixed
   only where the MSVC lane proves it insufficient.** The macro
   (`arbc_api.h:39-44`) is already declspec-complete. Any change is a *placement*
   fix on a specific declaration the shared MSVC lane fails on (typically moving a
   polymorphic base's `ARBC_API` from member-level to class-level so its vtable/RTTI
   is exported), never a rewrite of the macro or a blanket `WINDOWS_EXPORT_ALL_SYMBOLS`
   (Decision D1). Any such placement fix must keep the ELF exported surface a superset
   of the deliberate public API (class-level `ARBC_API` on ELF also exports the
   vtable/typeinfo, which the ELF proof already asserts for `arbc::Content`, so this
   does not widen the ELF surface beyond what is already proven).

2. **The proof is platform-forked in one file, with shared assertions.** The
   Windows branch reads PE directories (export directory of `arbc.dll`, import
   directory of each plugin `.dll`); the ELF branch stays `.dynsym`-based. The
   TEST_CASE structure, the core-symbol list, and the "imports ≥1 core symbol from
   the single libarbc, which exports it" assertions are shared — mirroring
   `plugin_host.cpp`'s single-`#if defined(_WIN32)`-seam philosophy so "identical
   property, both platforms" is a structural guarantee, not a parallel test that can
   drift.

3. **The single-instance observable is the PE import directory, not `.dynsym`.**
   On Windows the private-copy-vs-single-instance difference is: under the shared
   build the plugin `.dll`'s PE **import** directory names `arbc.dll` and lists the
   core symbols it imports; under a static build the plugin has no such import entry
   (the symbols are statically linked in). The proof asserts the import-directory
   entry exists and names ≥1 `arbc::` core symbol, and that `arbc.dll`'s **export**
   directory exports it — the exact structural mirror of the ELF `SHN_UNDEF`-in-plugin
   / defined-in-`.so` pair.

4. **`arbc.dll` is on the DLL search path at load, on the shared Windows lane only.**
   Because Windows resolves a plugin's `arbc.dll` import via the DLL search order
   (not ELF rpath or an already-loaded global namespace), every test that
   `LoadLibrary`s a plugin (and the `install.consumer`) must find `arbc.dll`. This is
   wired per-test via CMake `ENVIRONMENT_MODIFICATION`
   (`PATH=path_list_prepend:$<TARGET_FILE_DIR:arbc>`, and the installed `bin/` for the
   consumer), gated `if(WIN32 AND BUILD_SHARED_LIBS)` — additive, touching no other
   lane (Decision D4). The static `msvc-debug` lane is unaffected (nothing to find).

5. **The static build and every existing lane are unchanged.** `win-shared` is a
   new preset and `msvc-shared` a new matrix row; static `msvc-debug`, `gcc-shared`,
   and all Linux lanes build and run exactly as before. `VERSION`/`SOVERSION` are
   already Windows-inert; no install edit is needed (the RUNTIME/ARCHIVE split is
   already correct).

6. **Do not touch the plugin ABI.** `ARBC_PLUGIN_EXPORT` and the
   `extern "C" arbc_plugin_register` entry (`plugin.hpp:8-12,20`) are unchanged;
   `ARBC_API` governs the library surface, not the C entry seam (doc 03:227-234).
   No capability-flag scheme, no plugin ABI number (Decision D3).

7. **zstd on `win-shared` inherits `win-dev` unchanged.** The predecessor's
   Constraint 7 (shared + fetched-static zstd is a non-goal) is **ELF-specific**: it
   arose from the PIC-fold requirement (`cmake/ArbcInstall.cmake:45-54`), and Windows
   object code carries no PIC/non-PIC distinction, so folding a static zstd into
   `arbc.dll` is unproblematic. `win-shared` therefore inherits `win-dev`'s zstd
   acquisition with no change and no new non-goal (Decision D4-note).

8. **Lints/gate.** The new/edited CMake passes `gersemi` + `cmake-lint`; the PE
   test branch passes MSVC `/W4 /WX /permissive-` (`CMakeLists.txt:26`); the Linux
   lanes stay green under `-Wall -Wextra -Wpedantic -Werror`. Because the load-bearing
   proof runs only on `windows-latest`, the pre-commit signal is CI-green on
   `msvc-shared`, exactly as `plugin_loading_win32` accepted CI-green on `msvc-debug`
   (its Decision 2).

## Acceptance criteria

- **`msvc-shared` CI lane goes green (headline).** A new `include` row —
  `{ name: msvc-shared, os: windows-latest, cxx: "", preset: win-shared }` — added to
  `.github/workflows/ci.yml:44-85` beside the static `msvc-debug` (`:85`), reusing the
  msvc-dev-cmd + ninja steps (`:90-95`). The lane configures with
  `BUILD_SHARED_LIBS=ON`, builds `arbc.dll` + import lib, and `ctest --preset
  win-shared` runs the full suite green — including the PE resolution proof, the eight
  plugin loads through the production `PluginHost` (`arbc_dual_build_t`,
  `tests/CMakeLists.txt:1202-1222`), and the imageseq/miniaudio load tests
  (`:1153-1169`) — all against the single `arbc.dll`. This is an observable CI
  outcome on a runner the project already uses, not a compile-only claim.

- **The claim `17-internal-components#plugin-resolves-core-symbols-from-host-image`
  is re-enforced on Windows, no new registry row.** The `#if defined(_WIN32)` PE
  branch in `tests/shared_symbol_resolution.t.cpp` carries the *same* `// enforces:`
  tag and asserts the same property against PE directories: for each of the eight
  plugin `.dll`s, its import directory names `arbc.dll` and imports ≥1 core `arbc::`
  symbol (proving no private copy), and `arbc.dll`'s export directory exports it. The
  core-symbol witnesses are the MSVC equivalents of the ELF pair — the `Registry`
  registration entry and a polymorphic contract base whose class-level export carries
  its vtable/RTTI across the boundary (the Itanium spelling `"typeinfo for
  arbc::Content"` has no direct MSVC named-export analog, so the Windows witness is
  the exported class member/vtable, chosen by the implementer to prove the same
  cross-DLL RTTI/dispatch property). Parity re-enforcement across platforms — the
  `plugin_loading_win32` pattern (its Decision 5) — adds no `registry.tsv` row.

- **`win-shared` preset trio in `CMakePresets.json`.** Configure preset inheriting
  `win-dev` (Ninja + MSVC) with `"BUILD_SHARED_LIBS": "ON"`, plus matching build +
  test presets, mirroring the `shared`↔`dev` relationship (`:94-102,115,128`) one
  platform over.

- **Installed shared `libarbc` is consumable on Windows.** The out-of-tree
  `install.consumer` CTest (`tests/CMakeLists.txt:1041-1079`), run on the
  `msvc-shared` lane, links the staged `arbc.dll` via its import lib through
  `find_package(arbc CONFIG)` and loads/uses it — with `arbc.dll`'s `bin/` on the
  test's PATH (Constraint 4) — proving the `.dll` installs to `bin`, the import
  `.lib` to `lib`, and is consumable exactly as the static archive is.

- **The three containment scans stay green against `arbc.dll`.**
  `arbc_imageseq_containment_t`, `arbc_image_containment_t`, and
  `arbc_miniaudio_containment_t` (`tests/CMakeLists.txt:1153-1169`) pass against the
  `arbc.dll` artifact on the shared Windows lane — the decode/device symbols stay out
  of `libarbc` whether it is `.a`, `.so`, `.lib`, or `.dll`. If any scan reads a
  format-specific table, its Windows path reads the PE directories; the property (no
  codec/device symbol in `libarbc`) is artifact-agnostic.

- **No new golden, conformance, or behavioral-counter suite of its own** — the same
  deliberate reading the predecessor made. This task adds no content kind or operator
  (the conformance suite it re-runs is `dual_build`'s), renders no new pixels, and
  makes no wall-clock promise. Its promise is a build-and-load invariant on a second
  toolchain; the PE proof + the re-run loads pin it. The `msvc-shared` lane also runs
  whatever the `win-shared` `ctest` covers against `arbc.dll` at no extra authoring
  cost.

- **Diff coverage ≥90% on changed C++ lines** is *not gated on this task's changes*:
  the `diff-cover --fail-under=90` gate runs only on the Linux `coverage` lane, and
  the PE proof branch is compiled out there (`#if defined(_WIN32)`), so it is absent
  from `coverage.xml` — it neither raises nor lowers the gate, exactly as
  `plugin_loading_win32` handled its `_WIN32` branch (its Acceptance criteria). Any
  shared (platform-neutral) changed lines — e.g. a refactor of the ELF reader to
  share scaffolding with the PE branch — stay exercised on Linux at ≥90%.

- **Design-doc delta — doc 17:188-192 — lands in the closer's implementation commit
  (same-commit rule), not ahead of it** (Decision D5). The "remaining piece … is
  `packaging.shared_library_build_msvc`" sentence is rewritten to record that the
  MSVC shared build now exists and a plugin resolves core symbols from the single
  `arbc.dll` on the `msvc-shared` lane — flipping the doc from "remaining" to "proven
  on both toolchains." It is *not* pre-written by this refinement; writing "proven"
  before the lane is green would be the aspirational-docs drift doc 16:19-21 forbids.

- **Levelization + build + WBS gate green.** `scripts/check_levels.py` passes
  unchanged (no new `arbc_*` edge; `<windows.h>`/`<dbghelp.h>` are platform
  facilities); `scripts/check_claims.py` passes (the existing claim now has a second
  enforcing test, both tagged); and after the closer lands `complete 100` + the
  refinement back-link, `tj3 project.tjp 2>&1 | grep -iE "error|warning"` is silent.

**No deferred WBS leaves.** This task closes the shared-build story on both
toolchains the project ships; it registers no successor. The predecessor's
shared-`libarbc`-plus-fetched-static-zstd non-goal does not recur here — it was
ELF-PIC-specific and does not arise on Windows (Constraint 7). If the MSVC lane
surfaces a broad `ARBC_API`-placement problem too large to fix within this task, the
concrete remediation (re-annotate the affected declarations class-level) is
implementable work to fold into this task, not a new "audit" leaf — but per the ELF
`typeinfo for arbc::Content` assertion the polymorphic base is already class-level-
exported, so this is not anticipated. (Surfaced to the return summary for the parking
lot only if the lane reveals it, per the same-commit / defensible-call rule.)

## Decisions

**D1. Keep per-symbol `ARBC_API` export; verify and fix placement, never blanket-
export.** The macro's `__declspec` branch is already written and threaded
(`arbc_api.h:39-44`, `ArbcComponent.cmake:58,193`); this task proves it, not authors
it. Where the MSVC shared lane fails on a specific declaration (a class whose
vtable/RTTI must cross the DLL boundary), the fix is a targeted placement change on
that declaration.
*Rejected: `WINDOWS_EXPORT_ALL_SYMBOLS ON` on the umbrella.* It is absent repo-wide
(deliberately) — blanket-exporting every symbol contradicts the design's *"exactly
the deliberate public API and nothing else"* (doc 17:174-176) and the whole point of
`ARBC_API`, and it silently masks the very annotation gaps this lane exists to catch.
The per-symbol path is already 90% built; finishing it is cheaper and honest.

**D2. The Windows proof is a `#if defined(_WIN32)` PE branch inside the *existing*
`shared_symbol_resolution.t.cpp`, not a second test file.** The ELF reader
(`<elf.h>`, `.dynsym`) and the PE reader (export/import directories) are different
bodies, but they share the TEST_CASE structure, the core-symbol list, and the
"imports core symbols from the single libarbc, which exports them" assertions.
Forking only the reader — mirroring `plugin_host.cpp`'s single-seam design — makes
"identical property on both platforms" structural, and re-enforces the existing claim
with no new registry row (the `plugin_loading_win32` parity pattern). MSVC name
demangling uses `UnDecorateSymbolName` (`<dbghelp.h>`, linked `if(WIN32)`) to match
the same human-readable symbol substrings the ELF branch matches via
`abi::__cxa_demangle`.
*Rejected: a separate `shared_symbol_resolution_pe.t.cpp`.* Two files with two
core-symbol lists drift; the value is proving the *same* property both ways.
*Rejected: shell out to `dumpbin /EXPORTS` + `/IMPORTS` in CI.* The predecessor's
D3 chose an in-process artifact scan over `nm`/`readelf` shell-outs for idiom
consistency; the PE branch keeps that idiom. (`dumpbin` is a viable fallback only if
in-process PE import-directory parsing overruns the budget — it is present in the
msvc-dev-cmd environment — but it is the road not taken.)

**D3. `ARBC_API` (library surface) and `ARBC_PLUGIN_EXPORT` (plugin entry symbol)
stay distinct; the plugin ABI is untouched — identically on Windows.** Same rationale
as the predecessor's D7: they answer different questions, and merging them or minting
a plugin ABI number here would pre-empt Stage 2's semver-gated C ABI (doc 03:236-243).
On Windows both are already declspec-complete and independent (`arbc_api.h:39-44`,
`plugin.hpp:8-12`).
*Rejected: fold the two macros, or add a plugin ABI version while here.* Freezes a
Stage-2 promise before its ABI exists.

**D4. DLL search is solved per-test with `ENVIRONMENT_MODIFICATION`
(`PATH` prepend of `$<TARGET_FILE_DIR:arbc>` / the installed `bin/`), gated
`if(WIN32 AND BUILD_SHARED_LIBS)`.** Windows resolves a plugin's `arbc.dll` import
via the DLL search order, so the tests that `LoadLibrary` a plugin (and the
installed consumer) must find `arbc.dll`. A surgical per-test PATH prepend is the
minimal, additive fix and touches no other lane.
*Rejected: force a common `RUNTIME_OUTPUT_DIRECTORY` for `arbc` + all plugins.*
Restructures the output layout for every lane and risks artifact-name collisions to
solve a problem local to the Windows shared lane.
*Rejected: a `POST_BUILD` copy of `arbc.dll` next to each plugin.* Duplicates the
artifact, is fragile under incremental builds, and hides which `arbc.dll` a plugin
actually loads — the opposite of what the single-instance proof asserts.
*Note — zstd:* `win-shared` inherits `win-dev`'s zstd unchanged; the predecessor's
shared+fetched-static-zstd non-goal was an ELF PIC-fold artifact and does not arise
on Windows (Constraint 7), so no new machinery and no new non-goal.

**D5. The doc-17 delta (17:188-192) is scoped here but written by the closer in the
implementation commit — no ahead-of-time delta, no doc-00 bullet.** As with the
predecessor's D6, doc 17 already fully specifies the mechanism (the declspec macro,
the `BUILD_SHARED_LIBS` lane); there is no new design decision, only a *factual
discharge* of the "remaining piece" sentence the doc itself flagged and named this
task to close. Writing "proven on MSVC" before the `msvc-shared` lane is green would
claim an invariant that does not yet hold (doc 16:19-21). The project-shaping calls
(umbrella owns the public surface, doc 17:33; the export-macro mechanism, doc
17:194-203; the two-stage ABI, doc 03:227-243) are already recorded, so no doc-00
record is warranted — this is a doc-17 factual update, not a constitutional
amendment.
*Note — `SOVERSION`:* no version work here. `VERSION`/`SOVERSION`
(`ArbcComponent.cmake:202-205`) are already set off `PROJECT_VERSION` and are inert on
Windows (no soname; `arbc.dll` is unversioned), so the pre-1.0 policy the predecessor
recorded needs no Windows addendum.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-16.

- Added `win-shared` preset trio to `CMakePresets.json` (configure preset inheriting `win-dev` with `BUILD_SHARED_LIBS: ON`, plus matching build and test presets).
- Added `msvc-shared` CI lane to `.github/workflows/ci.yml` as a new `include` row on `windows-latest`, reusing the msvc-dev-cmd + ninja steps.
- Forked `tests/shared_symbol_resolution.t.cpp` with a `#if defined(_WIN32)` PE branch reading `arbc.dll`'s export directory and each plugin `.dll`'s import directory, sharing TEST_CASE bodies and core-symbol list with the ELF branch; `tests/CMakeLists.txt` links `dbghelp` and adds `catch_discover_tests` DL_PATHS override for the Windows shared lane.
- Added DLL-search wiring via `ENVIRONMENT_MODIFICATION` PATH prepend of `$<TARGET_FILE_DIR:arbc>`, gated `if(WIN32 AND BUILD_SHARED_LIBS)`, covering the `catch_discover_tests` override and the `install.consumer` CTest.
- D1 placement fixes (required for shared build to link at all): added `ARBC_API` to `arbc::linked_version()` / `linked_version_string()` in `src/arbc/version.hpp.in`; added out-of-line key-function anchor for `arbc::DeviceSink` via `src/runtime/device_sink.cpp` and updated `src/runtime/arbc/runtime/device_sink.hpp`, mirroring the pattern of `arbc::Content`.
- Surfaced pre-existing gcc-shared zstd PIC issue (non-PIC system `libzstd.a` folded into `libarbc.so` → SIGSEGV in `ZSTD_decompress`; `install.consumer` shared relink failure); registered as tech-debt task `packaging.shared_library_zstd_shared_link`.
