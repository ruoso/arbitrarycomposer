# quality.testing_artifact — arbc-testing shipped artifact

## TaskJuggler entry

[`tasks/70-quality.tji:8-13`](../../70-quality.tji) —

```
task testing_artifact "arbc-testing shipped artifact" {
  effort 1d
  allocate team
  depends contract.conformance_suite
  note "Package the conformance suite as the second shipped artifact, linkable by
        plugin-author test binaries. Docs 16/17."
}
```

Milestone: **M9** (`tasks/99-milestones.tji:72`, the 0.1 release — "The single
library + arbc-testing + imageseq plugin, installable with package metadata").

## Effort estimate

**Booked 1d; realistic 2d.** The 1d figure was written on the assumption that
`packaging.install` would have landed the install/export seam first and this
task would merely add one `install(TARGETS arbc-testing …)` line to it. That
assumption does not hold: `packaging.install` is unstarted, `testing_artifact`
does not depend on it, and **the tree contains no `install()` rule at all** —
`cmake/ArbcComponent.cmake:126-128` says so outright ("Header install
aggregation onto an installed FILE_SET is deferred until install/packaging
lands"). A conformance suite that cannot be found, linked, and run from outside
the build tree is not a shipped artifact, so the seam lands here.

Budget:

- 0.5d — the export blockers on the umbrella `arbc` (object-library link
  interface, the zstd requirement `CMakeLists.txt:161-164` already writes down).
- 0.5d — `cmake/ArbcInstall.cmake`: export set, `arbcConfig.cmake` +
  version file, the optional `testing` package component, `find_dependency`
  wiring.
- 0.5d — decoupling `arbc-testing` from `BUILD_TESTING` so a
  `-DBUILD_TESTING=OFF` packager build still produces it.
- 0.5d — the out-of-tree consumer test (`tests/consumer/`), its staged-install
  CTest driver, the CI lane, and the claim.

The 1d not spent here is not new project cost: `packaging.install` (2d,
`tasks/75-packaging.tji:6-10`) is correspondingly narrowed — see Decision 1.

## Inherited dependencies

**Settled (Status: Done)**

- **`contract.conformance_suite`**
  ([`../contract/conformance_suite.md`](../contract/conformance_suite.md), Done
  2026-07-05) — built the suite itself: `testing/contract_tests.cpp`,
  `testing/arbc/testing/contract_tests.hpp`, the `arbc-testing` STATIC target
  (`testing/CMakeLists.txt:10-14`), and `arbc_add_testing_library()`
  (`cmake/ArbcComponent.cmake:93-119`). It explicitly parked the shipping half:
  its Constraint 1 says the header is "installed for plugin authors", but as its
  own audit records, **there are no `install()` rules anywhere in the repo** —
  consumption is only via the build-tree `arbc::testing` ALIAS.
- **`contract.audio_conformance`**
  ([`../contract/audio_conformance.md`](../contract/audio_conformance.md), Done
  2026-07-07) — added `check_audio_facet_consistency` /`check_audio_async`
  (`testing/arbc/testing/contract_tests.hpp:140,148`). Nothing further to do
  here; it widens the surface this task ships, it does not change its shape.
- **`quality.stress_harness`** ([`stress_harness.md`](stress_harness.md), Done)
  — the sibling in this area; it perturbs the suite's seeded PRNG surface. It
  consumes `arbc-testing` in-tree and is unaffected by the install seam.

**Pending, and deliberately not a dependency**

- **`packaging.install`** (`tasks/75-packaging.tji:6-10`, 2d, unstarted) — owns
  "FILE_SET aggregation onto the umbrella install, CMake package config,
  pkg-config, CPS metadata; VERIFY_INTERFACE_HEADER_SETS in CI". This task lands
  the *minimal* seam of that description that `arbc-testing` cannot ship
  without; `packaging.install` keeps the rest (Decision 1). No WBS edge is added
  in either direction — `packaging.install` extends what lands here, and
  `check_levels.py` is not involved (neither is a component).
- **`packaging.shared_library_build`** (`tasks/75-packaging.tji:34-38`) — the
  `ARBC_API` export annotation and the `BUILD_SHARED_LIBS=ON` lane. This task
  ships the **static** `libarbc` + `arbc-testing` pair only; the shared-build
  variant of the same install seam is that task's problem, and its refinement
  will build on `cmake/ArbcInstall.cmake` rather than re-derive it.

## What this task is

Turn `arbc-testing` from a build-tree byproduct into the second **shipped
artifact** doc 17:14 promises: `cmake --install` produces a prefix from which a
plugin author's *own* test binary, in its *own* CMake project, can
`find_package(arbc CONFIG REQUIRED COMPONENTS testing)`, link `arbc::arbc` +
`arbc::testing`, and call `arbc::contract_tests(my_factory)` against their kind.

Concretely, three things are broken today and this task fixes all three:

1. **It is not built unless you are testing.** `CMakeLists.txt:257-258` guards
   `add_subdirectory(testing)` on `BUILD_TESTING`, so a packager configuring
   with `-DBUILD_TESTING=OFF` gets no `arbc-testing` at all — while the two
   *other* out-of-lib shipped artifacts (`plugins/imageseq`,
   `plugins/miniaudio`) deliberately build "regardless of `BUILD_TESTING`"
   (`CMakeLists.txt:241-243`, `:249-251`). The conformance suite is a shipped
   artifact by the same doc 17 table row and must follow the same rule.
2. **Its interface is not installable.** `arbc_add_testing_library()` propagates
   the component headers with a bare
   `$<TARGET_PROPERTY:arbc_<dep>,INTERFACE_INCLUDE_DIRECTORIES>`
   (`cmake/ArbcComponent.cmake:114-118`) — absolute build-tree paths with no
   `BUILD_INTERFACE` guard. `install(EXPORT)` rejects that outright.
3. **There is nothing to install it into.** No export set, no
   `arbcConfig.cmake`, no `arbc::` install namespace — the `arbc::*` names are
   build-tree `ALIAS` targets only (`cmake/ArbcComponent.cmake:26,103,134`).
   And `arbc-testing` deliberately carries *unresolved* contract symbols
   (`cmake/ArbcComponent.cmake:87-92`: objects resolve at the consumer's final
   link against `arbc`) — so shipping it is meaningless unless `libarbc` ships
   with it. That is why the umbrella install cannot be deferred out of this
   task.

## Why it needs to be done

Doc 16:31-44 calls the conformance suite "the crown jewel" and states the
business case in one sentence: "**Shipped as public API**:
`arbc::contract_tests(my_content_factory)` is the plugin ecosystem's conformance
story and **the reason plugin quality scales without review capacity**." Doc
10:71 repeats it — "shipped so plugin authors get it too". Doc 17:14 books it as
an artifact of the release. None of that is true of an artifact that only exists
inside our own build tree; today every consumer of the suite
(`tests/*_conformance.t.cpp`, eight of them) is *us*, linking a build-tree alias.
The promise is untested and, as written, unimplementable by an outsider.

Downstream, M9 (`tasks/99-milestones.tji:73`) states the release as "The single
library + arbc-testing + imageseq plugin, **installable with package metadata**"
and three unstarted packaging leaves — `packaging.plugin_helper`,
`packaging.examples`, `packaging.release_01` — all `depends !install`.
`tasks/refinements/kinds/dual_build.md:75` names the blocker flatly: "**`packaging.install`**
— nothing here is an installed target." The seam this task lands is the one all
of them are waiting on.

## Inputs / context

### Design docs (normative)

- **`docs/design/17-internal-components.md:9-18`** — the Shipped-artifacts
  table. `:13` `libarbc` (shared and static); **`:14` `arbc-testing` (static) |
  the contract conformance suite (doc 16) — linked by plugin authors' test
  binaries, never by `libarbc`**; `:17` "headers | `include/arbc/<component>/…`,
  one directory per public-facing component"; `:18` "CMake/pkg-config/CPS
  metadata | doc 10". Note `arbc-testing` has **no row in the levelization table**
  (`:47-62`) — it is an artifact, not a levelized component, and
  `scripts/check_levels.py` correctly ignores it (it only parses
  `arbc_add_component()` blocks in `src/*/CMakeLists.txt`).
- **`docs/design/17-internal-components.md:275-287`** — "The two `testing` trees
  are not the same thing and do not merge. Top-level `testing/` is the
  **`arbc-testing` static library**: … a separate artifact plugin authors link
  against and `libarbc` never does." The in-component
  `arbc/<component>/testing/` header-only doubles are *not* part of this task's
  install surface beyond riding the components' existing FILE_SETs.
- **`docs/design/17-internal-components.md:252`** — a plugin's device-backend
  dependency stays "private and never in `libarbc` / `arbc-testing`". The
  install surface must not smuggle one in.
- **`docs/design/16-sdlc-and-quality.md:31-44`** — the suite and the shipped-as-
  public-API promise; `:45-47` Catch2 as the assertion runtime.
- **`docs/design/16-sdlc-and-quality.md:88-90`** — "every code sample in docs and
  the plugin template compiles and runs in CI." The out-of-tree consumer this
  task adds is the first instance of that discipline.
- **`docs/design/16-sdlc-and-quality.md:112-118`** — diff coverage ≥90%, hard
  gate, per-push CI **and** `scripts/gate`.
- **`docs/design/10-tooling-and-packaging.md:19-35`** — the dependency policy
  table (`:28` "Test framework | tests only, not shipped | … lean Catch2") and
  `:32-35` "consume through standard find mechanisms (`find_package`), never
  vendored copies in-tree … **embedding the core must never transitively impose
  codecs, GPU SDKs, or a GUI toolkit.**" The Catch2 row needs a delta — see
  Decision 4.
- **`docs/design/10-tooling-and-packaging.md:71`** — "**Contract tests**: … shipped
  so plugin authors get it too."

### Source seams

- **`CMakeLists.txt:3-7`** — `project(arbitrarycomposer VERSION 0.1.0)`. The
  version the generated `arbcConfigVersion.cmake` reports.
- **`CMakeLists.txt:83-97`** — the Catch2 block, find-first + pinned
  `FetchContent` (v3.7.1), **inside `if(BUILD_TESTING)`**. Must be hoisted
  (Decision 3).
- **`CMakeLists.txt:135-147`** — `nlohmann_json` v3.11.3, header-only, INTERFACE.
  It appears in no shipped header (doc 08 is an implementation detail of
  `arbc_serialize`), so it must be scrubbed from the install interface, not
  `find_dependency`'d.
- **`CMakeLists.txt:161-164`** — the zstd install note, written by
  `serialize.zstd_dep` and **directly binding on this task**: "When libarbc grows
  an install/export …, that exported config must express the zstd requirement:
  `find_dependency(zstd)` when the system copy was used, nothing when the pinned
  static build was folded in."
- **`CMakeLists.txt:194-219`** — the zstd `FetchContent` + the `arbc_zstd`
  INTERFACE shim normalizing `zstd::libzstd_static` / `libzstd_static`.
- **`CMakeLists.txt:236-262`** — the `add_subdirectory` order: `src`,
  `plugins/imageseq`, `plugins/miniaudio`, `testing` (BUILD_TESTING-guarded),
  `tests`.
- **`cmake/ArbcComponent.cmake:18-40`** — `arbc_add_component()`; public headers
  are `FILE_SET HEADERS BASE_DIRS <component dir>` members under
  `<dir>/arbc/<name>/`. This FILE_SET is what the install aggregates.
- **`cmake/ArbcComponent.cmake:93-119`** — `arbc_add_testing_library()`; the
  `FATAL_ERROR` on missing Catch2 (`:98-100`), the `Catch2::Catch2` PUBLIC link
  (`:112`), and the un-guarded include-dir genex (`:114-118`).
- **`cmake/ArbcComponent.cmake:129-141`** — `arbc_finalize_library()`; the 18
  component OBJECT libraries linked `PRIVATE` onto `arbc` and each component dir
  re-exported as `$<BUILD_INTERFACE:${dir}>` with **no `$<INSTALL_INTERFACE:>`
  peer**. Called from `src/CMakeLists.txt:22`.
- **`cmake/ArbcComponent.cmake:126-128`** — the deferral this task discharges.
- **`src/version.cpp:3-5`** — the placeholder `version_string()`; "the real
  version API arrives with install/packaging (doc 10)" — that is
  `packaging.version_api` (0.5d, `tasks/75-packaging.tji:17-21`), **not** this
  task. The generated `arbcConfigVersion.cmake` reads `PROJECT_VERSION`; it does
  not need the C++ symbol.
- **`testing/CMakeLists.txt:10-14`** — the whole `arbc-testing` build.
- **`testing/arbc/testing/contract_tests.hpp:201`** — the umbrella entry point
  `void arbc::contract_tests(const testing::ContentFactory&, const testing::Options& = {})`;
  `:44` `ContentFactory`, `:49-78` `Options`, `:87-187` the granular
  `check_*` families.
- **`tests/tone_conformance.t.cpp:16,32-37`** — the canonical in-tree consumer
  pattern (`#include <arbc/testing/contract_tests.hpp>`, stacked `// enforces:`
  tags, `arbc::contract_tests(tone_factory())`). The out-of-tree consumer this
  task adds is the same three lines, from a foreign project.
- **`tests/CMakeLists.txt`** — 98 `catch_discover_tests()` registrations, **zero
  `add_test()` calls**. The staged-install driver is the first `add_test()` in
  the tree (Decision 5).
- **`scripts/check_claims.py:35`** — the claim scanner rglobs `*.cpp`/`*.hpp`
  under its roots (extended to include `testing/` by conformance_suite's
  Decision 4). The consumer project's source lives under `tests/`, so its
  `// enforces:` tag is picked up with no scanner change.
- **`scripts/gate:38-39`**, **`.github/workflows/ci.yml:22-38`** — where the
  lint/levelization/claims checks are wired; the install lane hooks in beside
  them.

## Constraints / requirements

1. **`arbc-testing` builds unconditionally**, exactly as the two plugin
   artifacts do (`CMakeLists.txt:241-243`, `:249-251`). `BUILD_TESTING=OFF` must
   still produce and install it. Its *tests* stay under `tests/`, gated as
   before. Consequently the Catch2 acquisition block moves out of
   `if(BUILD_TESTING)` and `arbc_add_testing_library()`'s `FATAL_ERROR` on a
   missing Catch2 (`cmake/ArbcComponent.cmake:98-100`) stops being reachable via
   `BUILD_TESTING`.
2. **`libarbc` never links `arbc-testing`** (doc 17:14, doc 17:277). The
   existing separation (STATIC lib, not folded into the umbrella, headers-only
   `DEPENDS`) is preserved verbatim; the install must not create an edge that
   the build does not have. Specifically: `arbc`'s exported interface must not
   name `Catch2::Catch2`, and `find_package(arbc CONFIG)` **without**
   `COMPONENTS testing` must succeed on a machine with no Catch2 installed.
3. **The install interface carries no build-tree paths.** Every
   `target_include_directories` on an exported target is
   `$<BUILD_INTERFACE:...>` / `$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>`
   paired. Headers land at `<prefix>/include/arbc/<component>/…` — doc 17:17's
   stated layout — and `<prefix>/include/arbc/testing/contract_tests.hpp`.
4. **The umbrella's export interface is scrubbed of internal targets.** The 18
   component OBJECT libraries and the `arbc_zstd` / `nlohmann_json` /
   `arbc_build_flags` helper targets are build-time machinery; none may appear
   in `arbc`'s `INTERFACE_LINK_LIBRARIES`. Their objects are already archived
   into `libarbc` (`cmake/ArbcComponent.cmake:129-141`), so nothing is lost by
   removing them from the interface — see Decision 2.
5. **The zstd requirement is expressed exactly as `CMakeLists.txt:161-164`
   dictates**: `find_dependency(zstd 1.5)` in the generated config when a system
   zstd was found; **nothing** when the pinned `FetchContent` static copy was
   built (its objects fold into `libarbc`). Both paths must be exercised — CI
   already resolves a system zstd on some Linux lanes and builds the pinned tag
   on Windows (`CMakeLists.txt:180-186`).
6. **No new dependency.** Doc 10:19-22's policy is untouched: this task adds
   `GNUInstallDirs` and `CMakePackageConfigHelpers` (both CMake builtins) and
   nothing else. Catch2's *status* changes (Decision 4 / the doc 10 delta) but
   Catch2 itself is already in the tree.
7. **`-Werror` cleanliness is a consumer-facing property, not a shipped one.**
   `arbc_build_flags` is linked `PRIVATE` everywhere
   (`cmake/ArbcComponent.cmake:32,109,136`) and must stay off the export — a
   plugin author's project must not inherit `-Wpedantic -Werror` from us.
8. **The out-of-tree consumer is a real foreign project**: its own
   `cmake_minimum_required` / `project()`, its own Catch2 acquisition, its own
   `find_package(arbc)`. It may not `include()` anything from our `cmake/`, may
   not reference `CMAKE_SOURCE_DIR`, and must be configured as a fresh CMake
   invocation against the *staged install prefix* — not a subdirectory of our
   build.
9. **Levelization is unaffected.** `arbc-testing` has no row in the doc 17 table
   and `scripts/check_levels.py` parses only `src/*/CMakeLists.txt`. Do not add
   one; do not touch the script.

## Acceptance criteria

- **`cmake/ArbcInstall.cmake` exists** and provides `arbc_install()`, called
  once from the top-level `CMakeLists.txt` after `testing/` is added. It
  installs `arbc` and `arbc-testing` into the export set `arbcTargets` under the
  `arbc::` namespace (`arbc::arbc`, `arbc::testing` — matching the build-tree
  ALIAS names exactly, so in-tree and out-of-tree consumers write identical
  `target_link_libraries` lines), aggregates every component's `FILE_SET
  HEADERS`, and generates `arbcConfig.cmake` + `arbcConfigVersion.cmake`
  (`SameMajorVersion`, reading `PROJECT_VERSION` = 0.1.0).

- **New claim `17-internal-components#arbc-testing-links-out-of-tree`**
  (`tests/claims/registry.tsv`): *The installed `arbc` package exposes the
  conformance suite as the optional `testing` component: a foreign CMake project
  that `find_package(arbc CONFIG REQUIRED COMPONENTS testing)` against a staged
  install prefix links `arbc::arbc` + `arbc::testing`, calls
  `arbc::contract_tests(factory)` on a `Content` it defines itself, and passes —
  with no reference to the arbitrarycomposer source or build tree.* Enforced by
  the `// enforces:` tag in `tests/consumer/conformance_consumer.cpp`.

- **New claim `17-internal-components#libarbc-never-requires-arbc-testing`**
  (`tests/claims/registry.tsv`): *`libarbc` never links `arbc-testing` (doc
  17:14): `find_package(arbc CONFIG)` without `COMPONENTS testing` succeeds and
  yields a usable `arbc::arbc` on a machine where Catch2 cannot be found, and
  `arbc::testing` is not defined in that configuration.* Enforced by the
  Catch2-suppressed configure case in `tests/consumer/` — configured with
  `CMAKE_DISABLE_FIND_PACKAGE_Catch2=ON` so the negative is real and not merely
  incidental to the CI image.

- **`tests/consumer/` — the out-of-tree consumer project.** A standalone CMake
  project (its own `project(arbc_consumer)`), containing:
  - `conformance_consumer.cpp` — defines a minimal `Content` (a flat-fill leaf
    modelled on the doubles in `tests/contract_conformance.t.cpp`), builds a
    `ContentFactory`, and calls `arbc::contract_tests(factory)` from inside a
    Catch2 `TEST_CASE`. This is verbatim the plugin author's three-line story
    from `tests/tone_conformance.t.cpp:32-37`, from a foreign project.
  - `core_only.cpp` — links `arbc::arbc` alone, calls one plain core API, and
    exists to prove the no-Catch2 configuration.

- **Staged-install CTest driver.** A CTest test `install.consumer` (the tree's
  first `add_test()`; every other test is `catch_discover_tests`) that runs a
  script doing: `cmake --install <build> --prefix <build>/_stage` →
  configure/build/`ctest` the `tests/consumer/` project against `_stage` → then
  re-configure it with `-DCMAKE_DISABLE_FIND_PACKAGE_Catch2=ON` and **without**
  `COMPONENTS testing`, asserting it still configures and builds `core_only`.
  Registered so both `scripts/gate` and per-push CI run it.

- **Header-set completeness.** The installed prefix contains
  `include/arbc/testing/contract_tests.hpp` and every component's public headers
  under `include/arbc/<component>/`. The consumer's compile is the assertion —
  a header that failed to install shows up as a missing-include build failure,
  not a silent pass. (The stronger `VERIFY_INTERFACE_HEADER_SETS` check, which
  compiles every installed header in isolation, stays with `packaging.install` —
  Decision 1.)

- **Both zstd paths.** The install lane runs once with
  `FETCHCONTENT_TRY_FIND_PACKAGE_MODE=NEVER` (pinned static zstd folded in;
  generated config emits no `find_dependency(zstd)`) and once find-first (system
  zstd; config emits `find_dependency(zstd 1.5)`). The consumer links
  successfully in both — that is the assertion `CMakeLists.txt:161-164` asks for.

- **Diff coverage ≥90%** on the changed C++ lines (`tests/consumer/*.cpp` are
  themselves the covering tests; the CMake modules are not C++ and carry no
  coverage obligation). Doc 16:112-118.

- **Design-doc delta** (landed with this refinement; rides the closer's commit
  per doc 16's same-commit rule — see Decision 4):
  - `docs/design/10-tooling-and-packaging.md:28` — the dependency-policy table
    row for the test framework, restated as "tests, and the conformance
    artifact": Catch2 is not in `libarbc` but *is* the public assertion runtime
    of the separately-shipped `arbc-testing`, carried as an optional CMake
    component so an embedder of the core never sees it.
  - `docs/design/17-internal-components.md:19-28` — a paragraph after the
    Shipped-artifacts table stating the package shape: one `arbc` package,
    `arbc::arbc` by default, `arbc::testing` behind `COMPONENTS testing`.
  - `docs/design/00-overview.md` § Resolved questions — the decision-record
    bullet ("One CMake package, with the conformance suite as an optional
    component"), because the package shape is what every future consumer,
    plugin template, and example is written against.

- **Deferred to `packaging.install`** (already a WBS leaf,
  `tasks/75-packaging.tji:6-10`, milestone M9 — closer does **not** register a
  new task, it is narrowed in place, see Decision 1): pkg-config `.pc`
  generation, CPS metadata, `VERIFY_INTERFACE_HEADER_SETS ON`, CPack, and the
  install layout for the plugin artifacts (`arbc-plugin-imageseq`,
  `arbc-plugin-miniaudio`) and their runtime search paths.

- **Deferred to `packaging.shared_library_build`** (already a WBS leaf,
  `tasks/75-packaging.tji:34-38`): the `BUILD_SHARED_LIBS=ON` variant of this
  install (SOVERSION, `ARBC_API` export annotation, the shared-`libarbc` consumer
  run). This task ships static only.

## Decisions

**D1. This task lands the minimal install seam; `packaging.install` is narrowed
in place, not duplicated.**
*Rationale:* `arbc-testing` deliberately carries unresolved contract symbols
(`cmake/ArbcComponent.cmake:87-92`) — it is meaningless without an installed
`libarbc` to resolve them against. "Shipped artifact, linkable by plugin-author
test binaries" is therefore *not implementable* without the umbrella install, and
`testing_artifact` has no dependency edge on `packaging.install` to wait behind.
The seam lands here in its minimal form (export set + package config + header
aggregation); `packaging.install` keeps everything in its note that is not
load-bearing for linking a test binary: pkg-config, CPS, `VERIFY_INTERFACE_HEADER_SETS`,
CPack, plugin install layout. Net WBS effort is unchanged; this task is
re-estimated 1d → 2d and `packaging.install` correspondingly 2d → 1d.
*Rejected: add `depends packaging.install` to this task.* It would make the task
un-pickable today and invert the natural order — the *consumer* of an install
surface is what proves the surface is correct, so building the surface with its
first real consumer in hand is strictly better than building it blind and
discovering at `plugin_helper` time that the export interface leaks build-tree
paths.
*Rejected: install `arbc-testing` alone, defer `libarbc`.* `install(EXPORT)`
would fail on the first generate — the suite's include interface references the
component targets — and even if coerced, the resulting artifact would not link.

**D2. Component OBJECT libraries leave the umbrella's export interface via
`$<BUILD_INTERFACE:>`, not by being exported themselves.**
*Rationale:* `arbc_finalize_library()` links each `arbc_<name>` OBJECT library
`PRIVATE` onto `arbc` (`cmake/ArbcComponent.cmake:138`). For a static `arbc`
CMake keeps PRIVATE deps in the interface as `$<LINK_ONLY:arbc_<name>>`, and
`install(EXPORT)` then refuses to generate ("requires target … not in any export
set"). The fix is to wrap the link in `$<BUILD_INTERFACE:arbc_<name>>`: the
objects are *already archived into `libarbc`* by the time anything links it, so
there is genuinely nothing for a consumer to link against and the interface entry
is pure noise. The `$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>` include
peer replaces what the components contributed.
*Rejected: `install(TARGETS arbc_base arbc_pool … EXPORT arbcTargets)`.* CMake
permits exporting OBJECT libraries, but it installs their `.o` files and exposes
18 internal targets in the public package — precisely the "one shipped library"
shape doc 17:13 exists to avoid, and a compatibility surface we would then be
stuck with.

**D3. Catch2 is acquired unconditionally; `arbc-testing` builds regardless of
`BUILD_TESTING`.**
*Rationale:* doc 17:14 books `arbc-testing` as a shipped artifact, and the tree
already encodes the right precedent for shipped-but-out-of-lib artifacts twice:
"it is a shipped artifact, so it builds regardless of `BUILD_TESTING` (its tests
live under `tests/`, gated there)" (`CMakeLists.txt:241-243`, `:249-251`). A
packager's `-DBUILD_TESTING=OFF` release build silently dropping the crown jewel
is the bug. The Catch2 block (`CMakeLists.txt:83-97`) is already find-first with
a pinned `FetchContent` fallback — moving it out of the guard costs a fetch on
`BUILD_TESTING=OFF` builds and nothing else.
*Rejected: a separate `ARBC_BUILD_TESTING_LIBRARY` option defaulting ON.* An
option nobody would ever turn off, whose only effect is to let a build produce a
release that is missing an artifact the release promises. The plugins do not have
one; neither should this.

**D4. Catch2 becomes the public assertion runtime of a shipped artifact — a
`find_dependency` of the optional `testing` package component, never of
`libarbc`. Requires a doc 10 delta.**
*Rationale:* doc 10:28's policy table currently books the test framework as
"tests only, **not shipped**". That is now false in the letter: `arbc-testing` is
shipped and links `Catch2::Catch2` PUBLIC (`cmake/ArbcComponent.cmake:112`),
because doc 16's Decision 1 has the suite assert from inside the *caller's*
`TEST_CASE`. It remains true in the spirit — doc 10:33-35's actual promise is
that *embedding the core* imposes nothing — and the package layout is what keeps
it true: one package `arbc`, with `arbc::testing` behind an optional CMake
COMPONENT whose `find_dependency(Catch2 3)` only fires when a consumer asks for
it. `find_package(arbc)` on a Catch2-less machine works, and that is pinned by a
claim, not by a comment.
*Design-doc delta:* amend the doc 10:28 table row to say the test framework is not
in `libarbc` but *is* the public assertion runtime of the separately-shipped
`arbc-testing`, carried as an optional package component so an embedder of the
core never sees it; add a decision-record bullet to `docs/design/00-overview.md`
recording the packaged consumption surface (one `arbc` package, `arbc::arbc` +
optional `arbc::testing`) — project-shaping, since it is the shape every future
consumer, plugin template, and example is written against.
*Rejected: ship `arbc-testing` as its own separate CMake package
(`find_package(arbc-testing)`).* It buys nothing — the suite is useless without
`arbc`, so the "separate package" would immediately `find_dependency(arbc)` — and
it costs a second config file, a second version file, and a version-skew failure
mode between two things that are always released together.
*Rejected: make the suite framework-agnostic (report failures through a callback
so the consumer's own framework renders them).* A real design, and the right one
if we ever have GoogleTest users — but doc 10:28 already picked Catch2 for the
project, doc 16:45-47 confirms it, and the suite's entire assertion model
(conformance_suite Decision 1) is built on asserting inside the caller's
`TEST_CASE`. Rewriting that to ship an install is scope inversion. If a
GoogleTest-based plugin author ever asks, that is a real future task with a real
design — it is not this one.

**D5. The out-of-tree consumer runs as a CTest test over a staged install, not
as a CI-only shell step.**
*Rationale:* doc 16:97-100 requires the local pre-push gate (`scripts/gate`) to
run the same tiers CI does — a check that only exists in `ci.yml` is one you
discover you broke after pushing. Making it a CTest test puts it in both with one
registration. It is the tree's first `add_test()` (all 98 existing registrations
are `catch_discover_tests`), which is correct: it drives `cmake`, not a Catch2
binary.
*Rejected: `ExternalProject_Add` on the consumer inside the main build.* It would
configure at build time against a prefix that does not exist until `cmake
--install` runs, and its failure mode (a configure error buried in build output)
is much worse than a named failing test.
*Rejected: `FetchContent`-style `add_subdirectory` of the consumer.* That
consumes the build-tree targets and proves nothing about the install — the exact
false-confidence this task exists to eliminate.

**D6. The consumer defines its own `Content`; it does not link a reference kind.**
*Rationale:* the claim is about the *plugin author's* path — someone with a kind
we have never seen. A consumer that instantiated `org.arbc.tone` would prove the
kind's headers install, not that the conformance surface is usable from outside.
A flat-fill leaf (modelled on the doubles in `tests/contract_conformance.t.cpp`)
exercises exactly the surface a third party has: the `Content` base, `Options`,
`ContentFactory`, `arbc::contract_tests`.
*No design-doc delta.*

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-14.

- Landed the install/export seam (`cmake/ArbcInstall.cmake`, `cmake/arbcConfig.cmake.in`): `cmake --install` produces a prefix from which a foreign CMake project can `find_package(arbc CONFIG REQUIRED COMPONENTS testing)`, link `arbc::arbc` + `arbc::testing`, and call `arbc::contract_tests(factory)`.
- Deviation from spec: two export sets (`arbcTargets` + `arbcTestingTargets`) rather than the single `arbcTargets` the refinement names — necessary so `find_package(arbc CONFIG)` without `COMPONENTS testing` succeeds and leaves `arbc::testing` undefined, preserving the second claim.
- `arbc-testing` now builds unconditionally: Catch2 block hoisted out of `if(BUILD_TESTING)` in `CMakeLists.txt`; `testing/` add_subdirectory moved outside the guard; `cmake/ArbcComponent.cmake` updated accordingly.
- Out-of-tree consumer project at `tests/consumer/` (`conformance_consumer.cpp`, `core_only.cpp`, `run_staged_install.cmake`); staged-install CTest test `install.consumer` added to `tests/CMakeLists.txt`.
- CI lane `gcc-install-fetched` (preset `install-fetched` in `CMakePresets.json`) added to `.github/workflows/ci.yml`.
- Claims `17-internal-components#arbc-testing-links-out-of-tree` and `17-internal-components#libarbc-never-requires-arbc-testing` registered in `tests/claims/registry.tsv`.
- Design-doc deltas: `docs/design/00-overview.md` (resolved-questions bullet on package shape), `docs/design/10-tooling-and-packaging.md` (Catch2 dependency-policy table row), `docs/design/17-internal-components.md` (package-shape paragraph after shipped-artifacts table).
- Fixer landed `Completion::settled_ok()` (`src/contract/arbc/contract/content.hpp`, `src/contract/render_completion.cpp`, `src/contract/t/async_render.t.cpp`) and split the `tile_in_flight` dispatch gate to join settled-ok tiles rather than re-dispatch them (`src/compositor/arbc/compositor/refinement.hpp`, `src/compositor/refinement.cpp`, `src/compositor/t/refinement.t.cpp`); claim `02-architecture#settled-undrained-tile-is-joined-not-redispatched` registered in `tests/claims/registry.tsv`.
