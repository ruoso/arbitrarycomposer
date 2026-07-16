# packaging.install — Install + package metadata

## TaskJuggler entry

`tasks/75-packaging.tji:6-10`:

```
task install "Install + package metadata" {
  effort 2d
  allocate team
  note "FILE_SET aggregation onto the umbrella install, CMake package config, pkg-config, CPS metadata; VERIFY_INTERFACE_HEADER_SETS in CI. Docs 10/17."
}
```

The task carries no own `depends`; it inherits the parent `packaging`
block's `depends runtime.host_objects, kinds.raster, serialize.kind_params`
(`tasks/75-packaging.tji:4`). Milestone **M9** (`m9_release`) — the whole
packaging block feeds the v0.1 release, and `packaging.plugin_helper`,
`packaging.examples`, and `packaging.release_01` all `depends !install`
(`tasks/75-packaging.tji:14,26,32`).

## Effort estimate

Booked 2d; realistic ~2d.

Budget:

- pkg-config `arbc.pc` (template + `configure_file` + install + the
  zstd-discriminator wiring) — ~0.25d.
- CPS `arbc.cps` (template mirroring the two-component CMake package +
  install + JSON-validity check) — ~0.5d.
- `VERIFY_INTERFACE_HEADER_SETS` (property on `arbc`/`arbc-testing`, one CI
  lane that builds the verify target) **plus** fixing whatever
  non-self-contained public header the check surfaces — ~0.5d (the tail
  risk lives here).
- Plugin install layout (three `MODULE` artifacts to a conventional dir) +
  the end-to-end `ARBC_PLUGIN_PATH` scan assertion in the staged-install
  consumer test — ~0.5d.
- Minimal CPack metadata + consumer-test extension + the claims-register
  entry — ~0.25d.

## Inherited dependencies

**Settled (formal `depends`, all Done):**

- `runtime.host_objects` (Done 2026-07-10), `kinds.raster` (Done
  2026-07-06), `serialize.kind_params` (Done 2026-07-09) — the parent
  block's deps. None set up install/export of their own; each adds an
  `OBJECT` library + `PUBLIC_HEADERS` that fold onto the umbrella FILE_SET
  (`cmake/ArbcComponent.cmake`), which is the pattern this task's residual
  deliverables ride on.

**Settled (informal — the real context this task builds on):**

- `quality.testing_artifact` already landed the umbrella install/export
  seam: `install(TARGETS arbc EXPORT arbcTargets … FILE_SET HEADERS)`
  (`cmake/ArbcInstall.cmake:68-74`), the `arbc-testing` install
  (`:76-80`), both `install(EXPORT …)` files (`:82-85`), and the generated
  `arbcConfig.cmake` / `arbcConfigVersion.cmake`
  (`configure_package_config_file` / `write_basic_package_version_file`,
  `:88-103`). **So the note's "FILE_SET aggregation onto the umbrella
  install, CMake package config" clauses are already done** — this task
  does *not* re-implement them; it lands the residual the deferral comment
  at `cmake/ArbcInstall.cmake:16-20` names.
- `packaging.version_api` (Done 2026-07-14) — `PROJECT_VERSION` is the
  single version source (`CMakeLists.txt:3`); `arbc.pc` and `arbc.cps`
  read it, no new literal.
- `packaging.shared_library_build` → `…_msvc` → `…_zstd_shared_link` (all
  Done) — landed `ARBC_API`, `SOVERSION`, and the zstd export
  discriminator (`cmake/ArbcInstall.cmake:53-62`). This task reuses that
  same discriminator to express the zstd dependency in the `.pc` and
  `.cps`, so all three metadata forms agree.
- `runtime` already ships `HostPlugins::scan_plugin_path()` reading
  `ARBC_PLUGIN_PATH` (`src/runtime/arbc/runtime/plugin_host.hpp:145-152`)
  — so the plugin *install layout* this task adds has a real runtime
  consumer and an end-to-end test seam; the discovery mechanism itself is
  not this task's work.

**Pending, and deliberately not a dependency:**

- `packaging.plugin_helper` (`arbc_add_plugin()`), `packaging.examples`,
  `packaging.release_01` — all `depends !install`; downstream consumers,
  not inputs.

**Downstream (this task serves/unblocks):** `packaging.plugin_helper`,
`packaging.examples`, `packaging.release_01` (M9).

## What this task is

Land the **residual install-metadata surface** the umbrella install seam
left for `packaging.install` — the four artifacts `cmake/ArbcInstall.cmake:17-18`
explicitly defers: (1) a **pkg-config** `arbc.pc` for the core library, (2)
**CPS** (Common Package Specification) metadata `arbc.cps` mirroring the
one-package/optional-`testing`-component shape, (3) `VERIFY_INTERFACE_HEADER_SETS`
enabled and exercised in CI, and (4) the **plugin artifacts' install
layout** (the three shipped `MODULE` plugins to a conventional,
`ARBC_PLUGIN_PATH`-discoverable directory). Plus a minimal **CPack**
configuration so `cpack` produces a tarball of the installed tree. The
CMake package config, umbrella FILE_SET aggregation, and export sets are
already in place; this task completes the "install exports" promise around
them (doc 10:43-46) and closes the file-set self-containedness gate (doc
17:223-226).

## Why it needs to be done

Doc 10:43-46 promises the install exports as **"CMake package config files,
pkg-config files, and CPS metadata"** — only the first exists today. Doc
17:11-18's Shipped-artifacts table lists `CMake/pkg-config/CPS metadata` and
the two plugin artifacts (`arbc-plugin-image`, `arbc-plugin-imageseq`) as
shipped; the plugins are built but installed nowhere. `packaging.release_01`
(M9) must not tag a v0.1 whose install surface silently omits half of what
the constitution says it ships — the same anti-drift argument the sibling
packaging refinements make. `VERIFY_INTERFACE_HEADER_SETS` (doc 17:223-226)
is the gate that keeps the public header set honest; every sibling
(`version_api`, `shared_library_build`) hand-verified its umbrella headers
against a future enabling of this check — this task is that future.

## Inputs / context

### Design docs (normative, doc 16)

- **doc 10:39-46** (`docs/design/10-tooling-and-packaging.md`) — CMake ≥
  3.24, `FILE_SET HEADERS` single-tree layout with
  `VERIFY_INTERFACE_HEADER_SETS` in CI (`:39-42`); **install exports =
  CMake package config + pkg-config + CPS metadata** (`:43-46`, CPS
  qualified *"as it becomes consumable by tooling"*).
- **doc 10:47-53** — plugins build as shared libraries with the
  `extern "C"` register entry; runtime discovery is explicit host
  registration first plus **opt-in `ARBC_PLUGIN_PATH` + platform-conventional
  locations** — the discovery contract the plugin install dir must feed.
- **doc 10:32-35** — the public dependency promise: *"embedding the core
  must never transitively impose codecs, GPU SDKs, or a GUI toolkit."* The
  `.pc`/`.cps` core metadata must honor this literally (the claim below).
- **doc 17:11-28** — Shipped-artifacts table (`CMake/pkg-config/CPS
  metadata`, the two plugin artifacts) and the find_package contract:
  `find_package(arbc CONFIG)` → `arbc::arbc` imposing nothing;
  `COMPONENTS testing` → `arbc::testing` + Catch2; **"plugin artifacts
  install alongside as loadable modules, not as link targets"** (`:28`).
- **doc 17:216-226** — the install-aggregation mechanic (the umbrella's
  one FILE_SET, already implemented) and **`VERIFY_INTERFACE_HEADER_SETS
  ON` in CI: every public header compiled standalone** (`:223-226`).
- **doc 00:408-423** — the one-package/optional-`testing`-component
  decision record the `.cps` must mirror.

### Source seams

- `cmake/ArbcInstall.cmake:1-104` — `arbc_install()`, the single install
  entry (called from `CMakeLists.txt:317`). The deferral comment
  (`:17-18`) names this task's exact scope. The zstd export discriminator
  (`:53-62`, `get_target_property(zstd_link arbc_zstd …)` →
  `ARBC_ZSTD_FIND_DEPENDENCY`/`ARBC_ZSTD_LINK_INTERFACE`) is the value the
  `.pc`/`.cps` reuse.
- `cmake/arbcConfig.cmake.in` — the config template that already consumes
  `@ARBC_ZSTD_FIND_DEPENDENCY@` / `@ARBC_ZSTD_LINK_INTERFACE@` and the
  optional `testing` component; the `.pc`/`.cps` templates parallel it.
- `cmake/ArbcComponent.cmake:187,268-279` — `arbc_finalize_library()`, the
  umbrella `arbc` target and its aggregated FILE_SET; `arbc-testing` at
  `testing/CMakeLists.txt:10-14` (EXPORT_NAME `testing`). These are the
  two targets that get `VERIFY_INTERFACE_HEADER_SETS`.
- `plugins/image/CMakeLists.txt:38`, `plugins/imageseq/CMakeLists.txt:32`,
  `plugins/miniaudio/CMakeLists.txt:31` — the three shipped `MODULE`
  targets (`arbc-plugin-image`, `arbc-plugin-imageseq`,
  `arbc-plugin-miniaudio`) currently built but not installed.
- `src/runtime/arbc/runtime/plugin_host.hpp:145-152` —
  `scan_plugin_path()` / `PluginScanReport`, the runtime consumer of the
  install dir; `src/runtime/t/plugin_host.t.cpp` shows the
  `ARBC_PLUGIN_PATH` mutation idiom.
- `tests/consumer/CMakeLists.txt`, `tests/consumer/run_staged_install.cmake`,
  and the `install.consumer` CTest (`tests/CMakeLists.txt:1333-1362`) — the
  staged `cmake --install` → foreign-project `find_package` vehicle this
  task extends for the `.pc`/`.cps`/plugin-scan assertions.
- `CMakeLists.txt:3` (`project(… VERSION 0.1.0 …)`), `:154-267` (the zstd
  block + `arbc_zstd` shim), `.github/workflows/ci.yml`,
  `CMakePresets.json` — where the version literal, dependency shape, and CI
  lanes live.

## Constraints / requirements

1. **No re-implementation of the umbrella install.** The
   `install(TARGETS arbc …)`, `install(EXPORT …)`, and config-file
   generation already exist (`cmake/ArbcInstall.cmake:68-103`); leave them
   untouched. This task adds *alongside* them inside `arbc_install()`.
2. **One dependency source of truth.** The `.pc` and `.cps` express the
   zstd dependency from the **same** discriminator `arbc_install()` already
   computes (`:53-62`) — never a second, independently-derived decision.
   The three metadata forms (config.cmake, `.pc`, `.cps`) must agree on
   what the consumer is asked to find.
3. **The core imposes nothing beyond conditional zstd** (doc 10:32-35). The
   core `arbc.pc` and the `arbc.cps` `arbc` component name no codec, no GPU
   SDK, no GUI toolkit, and no Catch2. Catch2 appears only on the CPS
   `testing` component, mirroring the CMake `COMPONENTS testing` gate.
4. **CPS is generated but not CI-gated on a consumer tool** (doc 10:43-46,
   *"as it becomes consumable by tooling"*). No new dependency on
   `cps-config` or any CPS reader; validity is checked as well-formed JSON
   with the expected shape only.
5. **`VERIFY_INTERFACE_HEADER_SETS` must pass with the public header set
   unchanged in spelling** — no header may be dropped from a component's
   `PUBLIC_HEADERS` to make the check green; a header that fails standalone
   compilation gets a real `#include` fix, in-scope.
6. **Plugins install as loadable modules, not link targets** (doc 17:28):
   `install(TARGETS … LIBRARY DESTINATION …)` with **no** `EXPORT` — they
   must never enter `arbcTargets` or the config's imported-target set.
7. **Levelization unchanged.** `scripts/check_levels.py` passes with no new
   rule and no widened edge; this is CMake/packaging plumbing, no C++
   component graph change.
8. **CMake ≥ 3.24 idioms only** (doc 10:39); `GNUInstallDirs` for every
   destination (`CMAKE_INSTALL_LIBDIR` / `_DATAROOTDIR` / `_INCLUDEDIR`) —
   no hardcoded `lib`/`share`.

## Acceptance criteria

- **pkg-config.** `arbc.pc` installs to
  `${CMAKE_INSTALL_LIBDIR}/pkgconfig`. On lanes where `pkg-config` is
  present (guard with `find_program`), the staged-install consumer test
  asserts `pkg-config --modversion arbc` equals `PROJECT_VERSION`, and that
  `--cflags`/`--libs` against the staged prefix compile+link a core-only
  translation unit. On the system-zstd + static lane the `.pc` carries
  `Requires.private: libzstd >= 1.5`; on the fetched-folded and
  shared lanes it carries none — matching `arbc_install()`'s
  discriminator.
- **CPS.** `arbc.cps` installs to `${CMAKE_INSTALL_DATAROOTDIR}/cps`;
  parsed by the consumer test as well-formed JSON with `name: "arbc"`,
  `version` == `PROJECT_VERSION`, a default `arbc` component (type `dylib`
  under `BUILD_SHARED_LIBS`, else `archive`) and an optional `testing`
  component; the `arbc` component's `requires` names only zstd
  (conditionally), never Catch2/codec/GPU.
- **`VERIFY_INTERFACE_HEADER_SETS`.** The property is `ON` on `arbc` and
  `arbc-testing`; a CI step builds `all_verify_interface_header_sets` (or
  the per-target verify targets) green — every public header of both
  compiles standalone. Any header fix the check forces ships in the same
  commit.
- **Plugin install layout (behavioral-counter assertion).** The three
  `MODULE` plugins install to `${CMAKE_INSTALL_LIBDIR}/arbc/plugins`; the
  staged-install consumer test sets `ARBC_PLUGIN_PATH` to that dir and
  asserts `HostPlugins::scan_plugin_path()`'s `PluginScanReport` loads
  **exactly** the shipped count (a count assertion over the report, not a
  wall-clock or pixel check) with zero failures.
- **CPack (smoke, un-gated).** `include(CPack)` with name/vendor/version/
  description/license set; `cpack -G TGZ` produces a tarball of the
  installed tree. Not a CI gate (no design promise rides on a packaging
  format); documented as a local convenience.
- **Claims register.** New entry
  `packaging.core-metadata-imposes-only-zstd` in
  `tests/claims/registry.tsv`, enforcing doc 10:32-35: the installed core
  metadata (`arbc.pc` + `arbc.cps` `arbc` component) exposes no dependency
  beyond the conditional zstd. An `enforces: packaging.core-metadata-imposes-only-zstd`
  test in the consumer suite greps the installed `.pc` `Requires*/Libs*`
  and the `.cps` `requires` and asserts neither names Catch2, a codec, a
  GPU SDK, or a GUI toolkit.
- **Coverage.** Diff coverage ≥90% on changed lines; `.pc.in`/`.cps.in`
  templates, `include(CPack)` metadata, and any `#if defined(_WIN32)` /
  `find_program`-guarded branch are excluded as non-executable /
  platform-compiled-out artifacts (the sibling packaging refinements' rule).
- **WBS gate.** `scripts/check_levels.py` unchanged, `scripts/check_claims.py`
  green, `scripts/gate` green, and `tj3 project.tjp 2>&1 | grep -iE
  "error|warning"` silent after `complete 100` + the refinement back-link.

**No deferred WBS leaves.** Every deliverable here is concrete and lands in
this task; `arbc_add_plugin()` (`packaging.plugin_helper`) and the host
examples (`packaging.examples`) are already existing sibling leaves, not
follow-ups this task spawns. A CPS *consumer round-trip* is intentionally
**not** minted as a task — it would be a "revisit when tooling matures"
audit leaf, which is unclosable; doc 10:43-46 already gates CPS as
metadata-only for now, and re-examination when `cps-config` stabilizes is
recorded as an Open-question note for the parking lot, not a WBS leaf.

## Decisions

**D1 — Scope is the residual, not a re-do.** The note's "FILE_SET
aggregation onto the umbrella install, CMake package config" clauses were
landed by `quality.testing_artifact`; this task lands only pkg-config, CPS,
`VERIFY_INTERFACE_HEADER_SETS`, plugin install layout, and CPack.
*Rationale:* `cmake/ArbcInstall.cmake:16-20` is explicit that the umbrella
install "lands here in its minimal form" (out of this task) and that these
five items "stay with `packaging.install`." Re-implementing the umbrella
install would duplicate a working seam. *Rejected:* treating the note
literally and rebuilding the FILE_SET/config path — wasteful and
regression-prone against a tested seam.

**D2 — `.pc` and `.cps` reuse the existing zstd discriminator.** Feed both
templates from the `ARBC_ZSTD_FIND_DEPENDENCY` / `zstd_link` values
`arbc_install()` already computes (`:53-62`). *Rationale:* the fetched-vs-
system-vs-shared exposure decision is subtle (three cases,
`shared_library_zstd_shared_link` D2) and already correct in one place;
deriving it a second time invites the three metadata forms to disagree —
exactly the skew a consumer would hit. *Rejected:* an independent
`pkg_check_modules`/hardcoded `Requires` in the `.pc` — a second source of
truth for the one thing that must stay consistent.

**D3 — pkg-config ships the core `arbc.pc` only; no `arbc-testing.pc`.**
*Rationale:* pkg-config is flat (no component notion) and is the
convenience path for plain C/Makefile embedders of the *core*; the
conformance suite asserts from inside the caller's own Catch2 `TEST_CASE`
(doc 00:414-419), a CMake-idiomatic flow with no pkg-config audience.
Shipping the core `.pc` keeps doc 10:32-35's promise literally true for the
pkg-config consumer. *Rejected:* a `arbc-testing.pc` carrying
`Requires: catch2` — invents a non-existent audience and risks a
pkg-config embedder pulling Catch2 into a core build, the precise thing the
optional-component design prevents.

**D4 — CPS mirrors the CMake package (two components), installed to
`share/cps`, validated as JSON only.** One `arbc.cps` with a default `arbc`
component (`dylib`/`archive` by `BUILD_SHARED_LIBS`) and an optional
`testing` component (requires Catch2). *Rationale:* CPS *has* a native
component model, so it can express exactly the one-package/optional-`testing`
shape doc 00:408-423 settled, unlike pkg-config — the metadata should match
the CMake package's own shape. `share/cps` is the emerging convention.
Doc 10:43-46 gates CPS as metadata "as it becomes consumable by tooling,"
so validity is a JSON-shape check, not a consumer round-trip. *Rejected:*
(a) omitting CPS — doc 10:43-46 and doc 17:18 list it as shipped; (b)
gating CI on `cps-config` — adds a young, unstable dependency for a
promise doc 10 explicitly keeps aspirational.

**D5 — `VERIFY_INTERFACE_HEADER_SETS` via the target property, exercised on
one CI lane.** Set the property `ON` on `arbc` and `arbc-testing`; build
`all_verify_interface_header_sets` on a single gcc lane. *Rationale:*
setting the property only *creates* the verify target — it costs nothing
until built — so ordinary builds are unaffected while one lane pays for the
standalone-compile gate (doc 17:223-226). *Rejected:* `set(CMAKE_VERIFY_INTERFACE_HEADER_SETS
ON)` globally — would recompile every public header on every build across
every lane for a check one lane suffices to enforce.

**D6 — Plugins install to `${libdir}/arbc/plugins` as modules, not export
targets.** `install(TARGETS arbc-plugin-image arbc-plugin-imageseq
arbc-plugin-miniaudio LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/arbc/plugins
RUNTIME DESTINATION …)` with no `EXPORT`. *Rationale:* doc 17:28 —
"plugin artifacts install alongside as loadable modules, not as link
targets"; a conventional subdir is exactly what an application host puts on
`ARBC_PLUGIN_PATH` (doc 10:50-53), and `scan_plugin_path()` already exists
to load it, giving an end-to-end test. *Rejected:* (a) exporting the
plugins into `arbcTargets` — makes them link targets and pollutes the
find_package surface with codec/device deps; (b) installing to the bare
`${libdir}` — collides with unrelated libraries and gives the scan no
containment. The exact dir (`arbc/plugins`) is within doc 10:50-53's
"platform-conventional locations" envelope, so it is chosen here rather
than via a design-doc amendment.

**D7 — Minimal CPack metadata, no CI gate.** *Rationale:* the deferral
comment lists CPack, and metadata-only inclusion is near-zero effort, but no
design promise rides on a specific installer/tarball format — the project's
real distribution vehicle is the staged `cmake --install` the
`install.consumer` test already exercises. Gating CI on `cpack` would add
flakiness for no protected invariant. *Rejected:* dropping CPack entirely
(the comment names it) and, oppositely, a full multi-generator CPack CI
matrix (unjustified by any promise).

**D8 — No design-doc delta.** *Rationale:* every deliverable is already
specified normatively — install exports (doc 10:43-46), plugin discovery
(doc 10:50-53), `VERIFY_INTERFACE_HEADER_SETS` and plugin-as-module install
(doc 17:216-226, 28). The plugin install dir choice is an implementation
detail inside doc 10:50-53's envelope, documented under D6, not a
constitutional amendment. (The stale doc 00:161-164 "not export-annotated
yet" bullet is a `shared_library_build` reconciliation, out of this task's
scope.) *Rejected:* amending doc 10 to pin `share/cps` and `arbc/plugins` —
the sibling refinements set precedent for making within-envelope calls in
the refinement without a doc edit.

## Open questions

(none — all decided. One parking-lot note for the closer: re-examine
gating CI on a CPS consumer round-trip once `cps-config` or an equivalent
reader stabilizes — recorded as a human-review item per D4, not a WBS
leaf, since a "revisit later" task is unclosable.)

## Status

**Done** — 2026-07-16.

- **pkg-config** (`cmake/arbc.pc.in`, installed to `${CMAKE_INSTALL_LIBDIR}/pkgconfig`): generates from the existing zstd discriminator; `pkgconfig_probe` consumer test asserts `--modversion` == `PROJECT_VERSION` and compile+link of a core TU.
- **CPS** (`cmake/arbc.cps.in`, installed to `${CMAKE_INSTALL_DATAROOTDIR}/cps`): two-component shape (`arbc`/`testing`) mirroring the CMake package; `core_metadata` consumer test validates JSON shape, version, and that the `arbc` component's `requires` names only zstd (never Catch2/codec/GPU).
- **`VERIFY_INTERFACE_HEADER_SETS`**: property `ON` on `arbc` and `arbc-testing`; `gcc-debug` CI lane builds `all_verify_interface_header_sets` green (`.github/workflows/ci.yml`).
- **Plugin install layout**: all three `MODULE` plugins (`arbc-plugin-image`, `arbc-plugin-imageseq`, `arbc-plugin-miniaudio`) install to `${CMAKE_INSTALL_LIBDIR}/arbc/plugins` with no EXPORT (`cmake/ArbcInstall.cmake`); `plugin_scan` consumer test sets `ARBC_PLUGIN_PATH` and asserts `loaded == 2` (image+imageseq register a kind entry), zero open failures — `arbc-plugin-miniaudio` exports `arbc_device_sink_create`, not `arbc_plugin_register`, so it is counted as `SkippedNoEntry`.
- **Minimal CPack** (`CMakeLists.txt`): name/vendor/version/description/license metadata; `cpack -G TGZ` produces a tarball — local convenience, not a CI gate.
- **Claims register** (`tests/claims/registry.tsv`): `packaging.core-metadata-imposes-only-zstd` enforcing doc 10:32-35; enforced by `core_metadata` consumer test.
- **Consumer test suite** (`tests/consumer/CMakeLists.txt`, `tests/consumer/pkgconfig_probe.cpp`, `tests/consumer/plugin_scan.cpp`, `tests/consumer/core_metadata.cpp`): three new behavioral tests wired into the staged-install consumer project.
- **CPS consumer round-trip** (D4 parking-lot note): re-examining the CI gate on a `cps-config` reader is recorded in `tasks/parking-lot.md` as a human-review item, not a WBS leaf, per the refinement's own instruction.
