# packaging.plugin_helper — `arbc_add_plugin()` helper

## TaskJuggler entry

[`tasks/75-packaging.tji:12-17`](../../75-packaging.tji) — task
`plugin_helper "arbc_add_plugin() helper"`, effort 1d, `depends !install`.
Note: "One-line third-party plugin builds shipped with the package; plugin
template directory compiled in CI. Doc 10."

## Effort estimate

**1d**, budgeted as:

1. The `arbc_add_plugin()` function itself (`cmake/ArbcAddPlugin.cmake`),
   its install + `arbcConfig.cmake.in` inclusion, and migrating the three
   shipped plugins onto it — **0.4d**.
2. The `examples/plugin-template/` standalone project, its staged-install
   compile pass in `tests/consumer/run_staged_install.cmake`, and the
   consumer-side load test — **0.4d**.
3. Claims-register entry, CI verification across lanes (static, `gcc-shared`,
   `msvc-shared`), WBS/refinement bookkeeping — **0.2d**.

## Inherited dependencies

**Settled (formal — the `depends !install` edge):**

- `packaging.install` (Done 2026-07-16,
  [`tasks/refinements/packaging/install.md`](install.md)) — the shipped
  plugins install to `${CMAKE_INSTALL_LIBDIR}/arbc/plugins` with **no
  `EXPORT`** ("loadable modules, not link targets", install.md D6;
  `cmake/ArbcInstall.cmake:156-182`); the `install.consumer` CTest stages a
  real `cmake --install` prefix and builds a foreign project against it
  (`tests/consumer/run_staged_install.cmake`); `tests/consumer/plugin_scan.cpp`
  already asserts the installed plugin dir loads via `ARBC_PLUGIN_PATH`.

**Settled (informal — no WBS edge needed, all complete):**

- `packaging.shared_library_build` chain — plugins are `MODULE` targets
  linking `arbc` `PRIVATE`; `ARBC_PLUGIN_EXPORT` (the entry-point macro,
  `src/contract/arbc/contract/plugin.hpp:8-20`) stays distinct from
  `ARBC_API` (shared_library_build.md D7). The helper must not disturb
  either macro.
- `quality.testing_artifact` — the installed package config seam this task
  extends: `arbcConfig.cmake.in`, `arbcTargets.cmake`, install destination
  `cmake_dest` (`cmake/ArbcInstall.cmake:90-111`).
- `kinds.dual_build` / `runtime.plugin_loading` — the production
  `PluginHost` loader (`src/runtime/plugin_host.cpp:46` — entry point
  `"arbc_plugin_register"`) and the registration pattern the template
  mirrors (`tests/ci_plugins/solid_ci_plugin.cpp`).

**Downstream:**

- Milestone M9 (`tasks/99-milestones.tji:72`) depends on this task directly.
- The migration promises recorded in the shipped plugins' CMakeLists
  (`plugins/imageseq/CMakeLists.txt:9-12`, `plugins/image/CMakeLists.txt:19`,
  `plugins/miniaudio/CMakeLists.txt:10`) are discharged here.
- `packaging.examples` will populate the same `examples/` directory this
  task creates; no ordering constraint either way.

## What this task is

Ship the `arbc_add_plugin()` CMake helper doc 10:47-49 promises — "a
`arbc_add_plugin()` CMake helper ships with the package so third-party
plugin builds are one line" — as an installed CMake module that
`find_package(arbc CONFIG)` makes available automatically, migrate the
three shipped in-repo plugins (`arbc-plugin-image`, `arbc-plugin-imageseq`,
`arbc-plugin-miniaudio`) onto it as the in-tree dogfooding proof, and add a
`examples/plugin-template/` directory — a standalone, copy-me third-party
plugin project (`project()` + `find_package(arbc CONFIG REQUIRED)` + one
`arbc_add_plugin()` call + one registration TU) — compiled in CI against
the staged install and loaded through the production `PluginHost`.

## Why it needs to be done

Doc 10:47-49 is a normative packaging promise and doc 00:125-127 records it
as a project decision ("plugins as shared libraries with a one-line build
helper. Decided in doc 10."). Today the promise is unmet: every plugin is a
hand-rolled `MODULE` target and `arbc_add_plugin` exists only as forward
references in comments (`plugins/imageseq/CMakeLists.txt:11`,
`plugins/image/CMakeLists.txt:19`, `plugins/miniaudio/CMakeLists.txt:10`,
`tests/CMakeLists.txt:1195`). A third party writing a plugin against the
installed package currently has to reverse-engineer the target shape
(`MODULE`, `PRIVATE arbc::arbc`, C++20) from our repo. The template
directory is the executable documentation of the intended third-party
workflow, and compiling it in CI is what keeps the helper honest against
the installed package rather than the build tree. M9 gates on it.

## Inputs / context

**Design docs (normative):**

- `docs/design/10-tooling-and-packaging.md:47-49` — the helper promise:
  plugins are shared libraries with the single `extern "C"` register entry
  point; `arbc_add_plugin()` ships with the package; third-party builds are
  one line.
- doc 10:50-53 — runtime discovery (`ARBC_PLUGIN_PATH` + explicit host
  registration); the helper is build-side only and must not grow discovery
  logic.
- doc 10:92-98 — **no plugin ABI version in v1**; the helper must not mint
  one (no version defines, no negotiation stubs).
- doc 10:28, 32-35 — the core package imposes nothing transitively; the
  helper therefore cannot drag `arbc::testing`/Catch2 into plugin builds.
- `docs/design/03-layer-plugin-interface.md:227-234, 251-255, 288-296` —
  the `arbc_plugin_register(Registry&)` seam, reverse-DNS kind ids,
  `arbc-plugin-*` artifact naming.
- `docs/design/17-internal-components.md:149-151, 161-171, 173-193,
  295-298` — out-of-lib plugin path, CI dual-build modules (explicitly
  hand-rolled, see D4), shared-build symbol-resolution proof, the
  "hand-rolled `MODULE`" phrasing for shipped plugins that this task
  supersedes in practice (see D8 — no doc delta needed; the normative
  content is the containment, not the hand-rolling).

**Source seams (real anchors):**

- `cmake/ArbcComponent.cmake:188-189` — `arbc::arbc` ALIAS exists in-tree
  and the umbrella carries `cxx_std_20` **PUBLIC**, so the exported
  imported target propagates the language level; the helper can link
  `arbc::arbc` unconditionally and add nothing else (D3).
- `cmake/ArbcInstall.cmake:90-111` — `cmake_dest` install of
  `arbcTargets.cmake` / `arbcConfig.cmake`; the helper file installs
  alongside them.
- `cmake/arbcConfig.cmake.in:22` — the `arbcTargets.cmake` include point;
  the helper is included right after it (D2).
- `cmake/ArbcInstall.cmake:156-182` — plugin install destination, no
  EXPORT, `BUILD_WITH_INSTALL_RPATH` under shared: install-side concerns
  that stay out of the helper (D1).
- `plugins/imageseq/CMakeLists.txt:32-33`, `plugins/image/CMakeLists.txt`
  (MODULE at ~39), `plugins/miniaudio/CMakeLists.txt:31` — the hand-rolled
  pattern being replaced: `add_library(<name> MODULE <entry.cpp>)` +
  `target_link_libraries(<name> PRIVATE <impl> arbc arbc_build_flags)`.
- `tests/CMakeLists.txt:1195-1215` — the six `arbc-ci-plugin-*` modules and
  the recorded decision that "packaging.plugin_helper is scoped to
  third-party, post-install authors" — they stay hand-rolled (D4);
  `tests/CMakeLists.txt:1006-1007` — `arbc-plugin-noentry` links no arbc at
  all, also out of scope.
- `tests/CMakeLists.txt:1333-1362` — the `install.consumer` CTest
  registration (RUN_SERIAL, sanitizer forwarding, MSVC PATH prepend) the
  template pass rides on.
- `tests/consumer/run_staged_install.cmake` — stage (`cmake --install`,
  lines 30-31), consumer pass with testing ON (57-65) and OFF (67-76); the
  template build is a new pass here (D6).
- `tests/consumer/CMakeLists.txt:41-43, 171-175` and
  `tests/consumer/plugin_scan.cpp` — the existing consumer-side plugin
  load assertions and the pattern for handing paths into consumer tests.
- `src/runtime/arbc/runtime/plugin_host.hpp:53, 69, 145-152` and
  `src/runtime/plugin_host.cpp:46, 183, 196-198` — `PluginHost::load()`,
  load statuses, `ARBC_PLUGIN_PATH` scan; the template load test uses
  explicit `load()` (D6).
- `tests/ci_plugins/solid_ci_plugin.cpp` — the one-TU registration shape
  (public headers only: `arbc/contract/plugin.hpp`,
  `arbc/contract/registry.hpp`, a public kind header) the template TU
  mirrors.
- `scripts/check_claims.py:32` — claim-enforcing test comments are scanned
  under `src/`, `tests/`, `testing/` only; the `enforces:` tag therefore
  lives in the consumer test, not in `examples/` (D6).

**Predecessor refinements:** [`install.md`](install.md) (D6 plugin layout,
consumer vehicle, claims style),
[`shared_library_build.md`](shared_library_build.md) (D7 macro separation,
MODULE-PRIVATE-arbc shape), [`version_api.md`](version_api.md) (consumer
test asserts through a real staged `find_package`).

## Constraints / requirements

1. **Ships with the package.** `cmake/ArbcAddPlugin.cmake` installs to the
   same `cmake_dest` as `arbcConfig.cmake` and is `include()`d from
   `arbcConfig.cmake.in` unconditionally — `find_package(arbc CONFIG)`
   alone makes `arbc_add_plugin()` callable; no extra `include()` asked of
   the consumer, no optional component.
2. **One line, honestly.** Against the installed package,
   `arbc_add_plugin(my-plugin SOURCES my_plugin.cpp)` must be the complete
   target definition: `MODULE` library, `PRIVATE arbc::arbc`, C++20 via the
   exported PUBLIC compile feature (`cmake/ArbcComponent.cmake:189`). The
   template project is the proof.
3. **Self-contained and third-party-neutral.** The helper file includes no
   internal machinery (`ArbcComponent.cmake`, `arbc_build_flags`,
   `ARBC_BUILDING`) and carries no install, EXPORT, RPATH, output-directory,
   or naming logic — those are host-project decisions
   (`cmake/ArbcInstall.cmake:156-182` keeps the shipped plugins' install
   concerns). In-tree-only extras (impl libs, `arbc_build_flags`) enter via
   the `LINK_LIBRARIES` argument at in-tree call sites.
4. **Migration is behavior-preserving.** The three shipped plugins migrate
   onto the helper with byte-identical target semantics: same target names,
   default `lib` prefix (installed names like `libarbc-plugin-image.so` are
   pinned by `plugin_scan`), same PRIVATE link sets. Every existing plugin
   test — `arbc_dual_build_t`, `plugin_scan` (`loaded == 2` +
   miniaudio `SkippedNoEntry`), `shared_symbol_resolution.t` on both shared
   lanes — passes unchanged.
5. **Plugin ABI untouched.** No new symbols, no ABI/version defines
   (doc 10:92-98); `ARBC_PLUGIN_EXPORT` and `arbc_plugin_register` are
   consumed as-is from `arbc/contract/plugin.hpp`.
6. **Template is a foreign project.** `examples/plugin-template/` has its
   own `project()`, sees arbc only via `find_package(arbc CONFIG REQUIRED)`
   against the staged prefix (`CMAKE_PREFIX_PATH`, mirroring
   `tests/consumer`), and is never `add_subdirectory`'d into the main
   build. Its registration TU compiles against installed public headers
   only.
7. **Compiled in CI on every lane.** The template pass rides inside the
   existing `install.consumer` CTest, so it runs on all matrix lanes
   including `gcc-shared` and `msvc-shared` — the lanes where a MODULE's
   link against a shared `libarbc` can actually go wrong. The load test
   executes from a consumer binary that already links `arbc::arbc`
   (symbols resolve from the host image, same as `plugin_scan` today).
8. **Levelization.** No new doc 17 component edges; `scripts/check_levels.py`
   output unchanged. The helper is build tooling, not a component.

## Acceptance criteria

- **Headline:** on a staged install, `examples/plugin-template/` configures
  and builds with exactly `find_package(arbc CONFIG REQUIRED)` +
  `arbc_add_plugin(...)` as its target-defining lines, and the produced
  module loads through the production `PluginHost::load()` with status
  `Loaded` and its kind id (`org.example.template`) present in the
  `Registry` — asserted by a new `tests/consumer/plugin_template_load.cpp`
  inside the `install.consumer` CTest, green on all lanes including
  `gcc-shared` and `msvc-shared`.
- **Claims register:** new entry
  `10-tooling-and-packaging#third-party-plugin-builds-are-one-line` in
  `tests/claims/registry.tsv`, enforced by
  `tests/consumer/plugin_template_load.cpp` (`enforces:` tag;
  `scripts/check_claims.py` green).
- **Migration:** `plugins/image`, `plugins/imageseq`, `plugins/miniaudio`
  define their MODULE via `arbc_add_plugin()`; the forward-reference
  comments in those CMakeLists are updated; all existing plugin tests
  (`arbc_dual_build_t`, `plugin_scan`, `shared_symbol_resolution.t`,
  `install.consumer`) pass without modification to their assertions.
- **No new golden, conformance, or behavioral-counter suite** — a
  deliberate reading of doc 16's taxonomy: this is packaging work; its
  falsifiable promise is the claims entry above, pinned end-to-end through
  a real staged install and a real `dlopen`.
- **Diff coverage ≥90%** on changed lines: the new consumer test TU and
  template TU are executable and covered by the `install.consumer` run;
  CMake files and the template's `CMakeLists.txt` are non-executable
  exclusions.
- **Levelization + build + WBS gate green:** `scripts/check_levels.py`
  unchanged, `scripts/gate` green, and after the closer adds `complete 100`
  + the refinement back-link to `tasks/75-packaging.tji`,
  `tj3 project.tjp 2>&1 | grep -iE "error|warning"` is silent.

## Decisions

**D1 — Helper signature: `arbc_add_plugin(<name> SOURCES <srcs...>
[LINK_LIBRARIES <extra...>])`, creating a `MODULE` linking
`arbc::arbc` PRIVATE.**
*Rationale:* the minimal surface that makes the third-party case one line
(`SOURCES` only) while letting in-tree call sites pass their impl libraries
and `arbc_build_flags` through `LINK_LIBRARIES`. Everything else a plugin
project might want (install destination, output directory, custom naming,
extra compile options) is ordinary CMake on the resulting target — the
helper returns a normal target, it doesn't wrap CMake.
*Rejected:* an `arbc_add_component`-style rich signature (FILE_SETs,
export machinery) — plugins have no export surface, by design (install.md
D6); `INSTALL_DESTINATION` / `OUTPUT_DIRECTORY` options — no third-party
call site needs them (in-repo CI modules that do are out of scope per D4),
and install policy is the host project's business.

**D2 — Ships as `cmake/ArbcAddPlugin.cmake`, installed next to
`arbcConfig.cmake`, `include()`d unconditionally from `arbcConfig.cmake.in`
immediately after the `arbcTargets.cmake` include.**
*Rationale:* "ships with the package" (doc 10:48) with zero consumer
ceremony; unconditional because the helper is a ~20-line pure function that
imposes nothing (doc 10:32-35) — gating it behind a component would add
friction with no containment benefit. Living in `cmake/` as a real file
(not a heredoc in the config template) keeps it readable, reviewable, and
identical between the in-tree include and the installed copy.
*Rejected:* an optional `COMPONENTS plugin` gate (ceremony without
containment value); embedding the function body in `arbcConfig.cmake.in`
(untestable in-tree, unreadable); a separate `find_package(arbcPlugin)`
package (two packages for one promise).

**D3 — The helper links `arbc::arbc`, always.**
*Rationale:* the namespaced name is the one identity valid in both worlds —
in-tree via the existing ALIAS (`cmake/ArbcComponent.cmake:188`) and
installed via the imported target. The exported PUBLIC `cxx_std_20`
(`ArbcComponent.cmake:189`) travels with it, so the helper sets no compile
features of its own.
*Rejected:* `if(TARGET arbc::arbc) … else() … arbc` dispatch — dead code,
the alias predates this task; helper-set `target_compile_features` —
duplicates an exported usage requirement.

**D4 — Migrate exactly the three shipped plugins; the six
`arbc-ci-plugin-*` modules and `arbc-plugin-noentry` stay hand-rolled.**
*Rationale:* the shipped plugins' CMakeLists carry the explicit recorded
promise that this task migrates them (`plugins/imageseq/CMakeLists.txt:11`),
and migrating them is the in-tree dogfooding that keeps the helper from
bit-rotting. The CI modules carry the opposite recorded decision —
"packaging.plugin_helper is scoped to third-party, post-install authors"
(`tests/CMakeLists.txt:1195`) — and need output-directory juggling the
helper deliberately doesn't offer (D1); `noentry` links no arbc at all and
is a loader-failure fixture, not a plugin.
*Rejected:* migrating all ten modules — would force `OUTPUT_DIRECTORY` into
the helper's signature purely for test fixtures, growing the shipped
surface to serve non-shipping call sites.

**D5 — Template at `examples/plugin-template/`: one registration TU
(`template_plugin.cpp`, kind id `org.example.template`) delegating to a
public built-in content, plus a minimal `CMakeLists.txt`.**
*Rationale:* `examples/` is where user-facing collateral lives
(`packaging.examples` populates it next); the template must not sit under
`plugins/`, which the main tree `add_subdirectory`s (Constraint 6). One TU
mirroring `tests/ci_plugins/solid_ci_plugin.cpp` — public headers only,
factory delegating to a shipped kind — proves compile-against-installed-
headers and the full register/load seam while keeping the template small
enough to actually read. Content authoring pedagogy is the conformance
suite's and the docs' job (doc 10:28), not the build template's.
*Rejected:* `plugins/template/` (wrong seam — would build in-tree, the one
thing the template must not do); a from-scratch `Content` implementation
in the template (turns a packaging artifact into an unmaintained second
kind-authoring tutorial); installing the template into the prefix (doc 10
promises the *helper* ships with the package; the template is repo
collateral, reachable where third parties actually start — the repo).

**D6 — CI vehicle: a template pass inside `run_staged_install.cmake`; the
load assertion lives in `tests/consumer/plugin_template_load.cpp`.**
*Rationale:* `run_staged_install.cmake` already owns the staged prefix and
runs on every lane; adding a configure+build of the template against that
prefix (then handing the built module's path into the consumer configure as
a `-D`, the same pattern `plugin_scan` uses for paths) reuses the whole
vehicle — serialization, sanitizer forwarding, MSVC PATH prepend — for
free. The enforcing test must live under `tests/` because
`scripts/check_claims.py:32` scans only `src/`, `tests/`, `testing/`;
`examples/` is invisible to the claims gate. Explicit `PluginHost::load()`
(not `scan_plugin_path()`) because the template module is in a build dir,
not the installed plugin dir — and `plugin_scan` already covers the scan
path.
*Rejected:* a self-testing template (own CTest inside
`examples/plugin-template/`) — puts the claim's enforcement outside the
scanned tree and bloats the template beyond "copy me"; a separate CI
workflow step — duplicates the staging the CTest already does.

**D7 — Claim id `10-tooling-and-packaging#third-party-plugin-builds-are-
one-line`.**
*Rationale:* the promise is doc 10's, verbatim ("so third-party plugin
builds are one line"), and the register's convention is
`<doc-file-stem>#<slug>` (`tests/claims/registry.tsv:1-3`). The enforcing
test pins the observable meaning: a foreign project whose only
target-defining lines are `find_package` + `arbc_add_plugin` produces a
module the production loader loads and registers.
*Rejected:* a bare `packaging.*` id (install.md's style is the outlier;
this promise has a doc-anchored home).

**D8 — No design-doc delta.**
*Rationale:* doc 10:47-49 already specifies the helper — this task
implements a promise, it doesn't amend one. The template directory is
packaging collateral named by the WBS note, not designed behavior. Doc
17:297's "hand-rolled `MODULE`" phrasing for plugin artifacts describes the
containment property (own deps private, never in `libarbc`/`arbc-testing`),
which the helper preserves exactly — the normative content is unchanged, so
no amendment is owed.
*Rejected:* adding an `examples/` line to doc 17's repo layout —
`packaging.examples` owns populating that directory and can carry the
layout note if one is ever warranted.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-16.

- `cmake/ArbcAddPlugin.cmake` created: `arbc_add_plugin(<name> SOURCES <srcs...> [LINK_LIBRARIES <extra...>])` helper — `MODULE`, `PRIVATE arbc::arbc`, C++20 via exported usage requirement.
- `cmake/ArbcInstall.cmake` updated: installs `ArbcAddPlugin.cmake` alongside `arbcConfig.cmake` to `cmake_dest`.
- `cmake/arbcConfig.cmake.in` updated: unconditionally `include()`s `ArbcAddPlugin.cmake` immediately after `arbcTargets.cmake`.
- `CMakeLists.txt` updated: wires in the helper installation at top-level configure time.
- `plugins/image/CMakeLists.txt`, `plugins/imageseq/CMakeLists.txt`, `plugins/miniaudio/CMakeLists.txt` migrated: hand-rolled `add_library(… MODULE)` blocks replaced with `arbc_add_plugin()` calls; target names and installed paths byte-identical.
- `examples/plugin-template/CMakeLists.txt` + `examples/plugin-template/template_plugin.cpp` created: standalone foreign project (`project()` + `find_package(arbc CONFIG REQUIRED)` + single `arbc_add_plugin()` call), kind id `org.example.template`.
- `tests/consumer/run_staged_install.cmake` updated: added template-build pass against the staged prefix; produces `libtemplate-plugin.so` fed via `-D` into the consumer configure.
- `tests/consumer/plugin_template_load.cpp` created: consumer test asserting `PluginHost::load()` returns `Loaded` and `org.example.template` is present in the `Registry`; carries `enforces: 10-tooling-and-packaging#third-party-plugin-builds-are-one-line` tag.
- `tests/consumer/CMakeLists.txt` updated: registers `plugin_template_load` inside `install.consumer`.
- `tests/claims/registry.tsv` updated: new entry `10-tooling-and-packaging#third-party-plugin-builds-are-one-line`.
- `tests/consumer/core_metadata.cpp`, `tests/consumer/plugin_scan.cpp` updated: minor consumer-test adjustments to accommodate new staged-install layout.
