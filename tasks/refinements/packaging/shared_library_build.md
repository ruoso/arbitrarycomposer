# packaging.shared_library_build — Export-annotate libarbc and verify shared-build plugin loads

## TaskJuggler entry

[`tasks/75-packaging.tji:35-40`](../../75-packaging.tji) —

```
task shared_library_build "Export-annotate libarbc and verify shared-build plugin loads" {
  effort 3d
  allocate team
  depends kinds.dual_build, kinds.imageseq_plugin
  note "Export-annotate libarbc's public symbol surface (ARBC_API macro on public headers, per doc 17:135-141), add a BUILD_SHARED_LIBS=ON CMake preset and CI lane, and re-run arbc-plugin-imageseq, arbc-plugin-miniaudio, and the six arbc-ci-plugin-* loads against the shared libarbc — so a plugin resolves core symbols from the host image instead of carrying a private copy. Source-of-debt: tasks/refinements/kinds/dual_build.md (Decision 6). Docs 03/17."
}
```

The note's `doc 17:135-141` citation is stale: the symbol-visibility / export-macro
text actually lives at **17:173-191** (the "one honesty limit" paragraph at 173-182
and the CMake mechanics at 186-191). The doc never spells the macro `ARBC_API`
literally — it says *"an export macro"* (17:187); naming it `ARBC_API` is this
refinement's call (Decision D1), consistent with the already-shipped
`ARBC_PLUGIN_EXPORT` idiom (`src/contract/arbc/contract/plugin.hpp:8-12`).

Milestone: **M9** (`tasks/99-milestones.tji:70-73`, the v0.1 release) — this task is
a direct `depends` of `m9_release` (`:72`). It gates nothing else in M9; the release
ships *"the single library … installable"* (`:73`) and doc 17:13 promises `libarbc`
in *"shared and static"* form, a promise no lane has yet produced.

## Effort estimate

**Booked 3d; realistic 3d.** The booking is right, and the cost is not in the CMake
seams — those are three small, known edits (a macro header, one preset trio, one CI
matrix row). It is in the *breadth*: annotating the public declarations of every
component's public header set (the fifteen-odd components of the doc-17 table,
`docs/design/17-internal-components.md:57-72`) with `ARBC_API`, then re-running the
whole plugin-load surface against a `.so` that has never existed and fixing whatever
the shared build surfaces. `kinds.dual_build` Decision 6 sized this exact deferral
as *"multi-day, whole-library, packaging-shaped work"* and *"doing it badly (a
half-annotated surface) is worse than not doing it"* (`../kinds/dual_build.md:500-503`).

Budget:

- 0.3d — the `ARBC_API` macro header + threading the build-side define onto every
  object library through `arbc_add_component()` (`cmake/ArbcComponent.cmake:23-52`)
  and the umbrella through `arbc_finalize_library()` (`:161-209`). This is the one
  genuinely new CMake mechanic (Decision D2).
- 1.3d — annotating the public surface: every public declaration in every
  component's `FILE_SET HEADERS` gets `ARBC_API`. Mechanical but wide and
  unforgiving — a missed symbol a plugin references is an undefined-symbol load
  failure under the shared build, which is precisely what the new lane will catch.
- 0.3d — the `shared` preset trio + CI matrix lane.
- 0.5d — the symbol-resolution proof (new test) + the claims-register entry, and
  re-running the eight enumerated plugin loads + the three containment scans green
  against `libarbc.so`, fixing fallout.
- 0.3d — `SOVERSION`/`VERSION` on the installed `.so`, the consumer-side assertion,
  the doc-17 delta discharging the honesty limit, and `scripts/gate`.

## Inherited dependencies

**Settled (formal `depends`, `tasks/75-packaging.tji:38`):**

- **`kinds.dual_build`** ([`../kinds/dual_build.md`](../kinds/dual_build.md), Done
  2026-07-11) — the **source-of-debt**, and the reason this task exists in the shape
  it does. Its Decision 6 (`../kinds/dual_build.md:490-506`) is worth quoting in
  full, because this task is its discharge:

  > 6. **The proof runs against the *static* `libarbc`, and the doc says so.**
  >    `arbc_finalize_library()` calls `add_library(arbc "version.cpp")` with no
  >    `STATIC`/`SHARED` (`cmake/ArbcComponent.cmake:133`), `BUILD_SHARED_LIBS` is set
  >    by no preset, and the global `-fvisibility=hidden` (`CMakeLists.txt:16`) with
  >    **no `ARBC_API` export macro anywhere** means a `SHARED` `libarbc` would export
  >    nothing at all today. So every plugin in the tree — the shipped ones included —
  >    links the static archive and carries a private copy of the core objects it
  >    references. This task therefore proves the entry point, the factory, the
  >    facets, the service injection, and pixel equivalence cross the boundary; it does
  >    **not** prove a plugin resolves core symbols from the host image.
  >    *Rejected: adding the export macro here.* Annotating the public surface of
  >    fifteen components is multi-day, whole-library, packaging-shaped work — it is
  >    not a 1d kinds task … It is registered as `packaging.shared_library_build`.

  `dual_build` built the machine this task now runs shared: six CI-only `MODULE`
  targets `arbc-ci-plugin-{solid,tone,raster,nested,fade,crossfade}`
  (`tests/CMakeLists.txt:1185-1193`), each linking the umbrella `arbc` PRIVATE and
  loaded through the production `runtime::PluginHost` by the `arbc_dual_build_t`
  driver (`:1202-1222`). That driver already proves byte-exact in-lib/plugin render
  equivalence, both facets, and host-side service injection cross the boundary — it
  simply cannot prove *single-instance* symbol resolution while `arbc` is a static
  archive. The doc-17 line numbers in Decision 6 (`ArbcComponent.cmake:133`,
  `CMakeLists.txt:16`) drifted after `version_api` landed; the current seams are
  `cmake/ArbcComponent.cmake:166` and `CMakeLists.txt:16` (Inputs below).

- **`kinds.imageseq_plugin`** ([`../kinds/imageseq_plugin.md`](../kinds/imageseq_plugin.md),
  Done 2026-07-07) — landed the minimal `contract::Registry` + `extern "C"
  arbc_plugin_register(Registry&)` seam (`src/contract/arbc/contract/plugin.hpp:20`)
  and the first out-of-`libarbc` shipped plugin, `arbc-plugin-imageseq`
  (`plugins/imageseq/CMakeLists.txt:32`, a `MODULE` linking `arbc` PRIVATE). It is
  one of the eight plugin modules this task re-runs shared, and it established the
  `ARBC_PLUGIN_EXPORT` default-visibility annotation on the *entry symbol* — the
  narrow precedent `ARBC_API` generalizes to the *whole library surface* (Decision D7
  keeps the two distinct).

**Informal, but the real context:**

- **`packaging.version_api`** ([`./version_api.md`](./version_api.md), Done
  2026-07-14) — landed the pattern this task reuses for its macro header: a
  header generated/placed into a build-tree root and added to the umbrella's
  `FILE_SET HEADERS` via a dedicated `BASE_DIRS` entry so it installs at the include
  *root* (`<prefix>/include/arbc/…`), not under `arbc/<component>/`
  (`cmake/ArbcComponent.cmake:187-204`). Its **Constraint 5** explicitly reserved
  `ARBC_API`, the visibility annotation, and `SOVERSION`/`VERSION` for *this* task
  (`./version_api.md:274-277`), and left `src/arbc/version.hpp` including *nothing*
  so a consumer compiles it under their own flags — a rule `arbc/arbc_api.h` inherits
  (Constraint 2 below).
- **`quality.testing_artifact`** ([`../quality/testing_artifact.md`](../quality/testing_artifact.md),
  Done) — landed the umbrella install/export seam
  (`cmake/ArbcInstall.cmake:60-66`), whose `install(TARGETS arbc …)` **already**
  carries `LIBRARY DESTINATION` and `RUNTIME DESTINATION` (`:63-65`) — the shared
  `.so`/`.dll` install paths this task activates need no new `install()` call. Its D2
  scrubbed `arbc_build_flags` out of the export interface via `$<BUILD_INTERFACE:>`
  (`cmake/ArbcComponent.cmake:169`), which is why `arbc/arbc_api.h` must be
  warning-clean under a consumer's own flags.
- **`serialize.zstd_dep`** ([`../serialize/zstd_dep.md`](../serialize/zstd_dep.md),
  Done) — its install branch folds a *fetched static* zstd's objects into `arbc` via
  `$<TARGET_OBJECTS:libzstd_static>` (`cmake/ArbcInstall.cmake:45-54`). Folding a
  static archive's objects into a **shared** `libarbc` needs those objects to be PIC,
  which the fetched build does not guarantee — hence Decision D4 puts the new lane on
  the *system*-zstd (find-first) path, where zstd is an ordinary `.so` dependency.

**Pending, and deliberately not a dependency:**

- **`packaging.install`** (`tasks/75-packaging.tji:6-10`, unstarted) — owns pkg-config,
  CPS metadata, `VERIFY_INTERFACE_HEADER_SETS ON`, CPack, and the *plugin* artifacts'
  install layout (`cmake/ArbcInstall.cmake:17-18`). No edge either way: the umbrella
  install seam already landed with `testing_artifact`, and
  `VERIFY_INTERFACE_HEADER_SETS` — which will compile `arbc/arbc_api.h` standalone —
  *ratifies* this header, it is not a gate this task waits on.
- **`packaging.plugin_helper`** (`tasks/75-packaging.tji:11-16`) — owns
  `arbc_add_plugin()`. The eight plugin modules stay hand-rolled here
  (`plugins/*/CMakeLists.txt`, `tests/CMakeLists.txt:1185-1193`), exactly as
  `dual_build` and `imageseq_plugin` left them.

**Downstream (this task unblocks/serves):**

- **`packaging.release_01`** (`tasks/75-packaging.tji:29-33`) — a released `libarbc`
  in *"shared and static"* form (doc 17:13) is only honest once a shared build exists
  and its plugins load. This task makes doc 17:13 true; `release_01` tags it.

## What this task is

Give `libarbc` a shared build that actually works, and prove a plugin loaded into it
resolves core symbols from the **one** `libarbc.so` in the process rather than from a
private static copy baked into the plugin.

Today (`kinds.dual_build` D6, doc 17:173-182) the whole plugin surface links the
static `libarbc.a`. Every plugin — the CI dual-build modules and the shipped
`arbc-plugin-*` alike — carries its own copy of every core object it touches. The
dual-build proves the *boundary* (entry point, factory, facets, service injection,
byte-exact render); it is *constitutionally unable* to prove *single-instance*
resolution, because under the static archive there is no single instance to observe.

The task lands five things:

1. **`arbc/arbc_api.h`** — a hand-rolled umbrella-owned public header defining the
   `ARBC_API` visibility macro (default-visibility when compiling `libarbc`'s own
   translation units; the dllexport/dllimport branch for a future MSVC shared build),
   included by every public header.
2. **The annotated surface** — `ARBC_API` on every public declaration in every
   component's `FILE_SET HEADERS`, so that under `-fvisibility=hidden` the shared
   `libarbc` exports exactly the deliberate public API (doc 16) and nothing else.
3. **The `shared` build** — a `BUILD_SHARED_LIBS=ON` preset trio in
   `CMakePresets.json` and one CI matrix lane in `.github/workflows/ci.yml`, so a
   `libarbc.so` is produced and its whole test suite runs on every push.
4. **`SOVERSION`/`VERSION`** on the `arbc` target, off `PROJECT_VERSION` — the soname
   the shared build needs (no-ops for the static archive it stays for other lanes).
5. **The single-instance proof, tested** — a new symbol-resolution scan asserting
   that under the shared build the plugin modules reference chosen `ARBC_API` core
   symbols as *undefined imports* while `libarbc.so` *exports* them; plus re-running
   the eight enumerated plugin loads (`arbc-plugin-imageseq`, `arbc-plugin-miniaudio`,
   the six `arbc-ci-plugin-*`) green through the production `PluginHost` against the
   shared `libarbc`.

It does **not** touch the plugin ABI / `ARBC_PLUGIN_EXPORT` entry symbol (Decision
D7), add an MSVC shared lane (Decision D4, deferred), or migrate plugins to
`arbc_add_plugin()` (`packaging.plugin_helper`).

## Why it needs to be done

- **doc 17:13's promise is currently a lie.** The design ships `libarbc` in *"shared
  and static"* form; no preset sets `BUILD_SHARED_LIBS` (`CMakePresets.json` has
  none — confirmed by grep) and no lane has ever produced a `.so`. `dual_build` D7
  wrote the static-only limit into the constitution (doc 17:173-182) precisely so the
  gap would be *recorded rather than hidden* until this task closes it.
- **The honesty gap is the one doc 17:180-182 names.** *"It does not yet prove that a
  plugin resolves core symbols from the host image. Export-annotating the public
  surface and adding the `BUILD_SHARED_LIBS` CI lane is
  `packaging.shared_library_build`."* This task is that sentence.
- **`packaging.release_01` should not tag a shared library it cannot load.** A first
  tag over *"shared and static"* while only static has ever been built would tag a
  surface the project does not actually keep — the same anti-drift argument
  `version_api` made for the version symbol (`./version_api.md:144-147`).
- **Plugin authors will link against the shared library.** Doc 03:227-234 accepts
  same-toolchain coupling for v1, which makes *"which `libarbc` am I loaded into"* a
  live question — and the answer is only meaningful if there is a single shared
  `libarbc` to be loaded into, not a private copy per plugin.

## Inputs / context

### Design docs (normative, doc 16)

- **Doc 17:173-182 — the "one honesty limit".** The exact gap this task closes,
  verbatim, and the source of the task's existence:
  > One honesty limit is recorded rather than hidden. `libarbc` carries no export
  > annotation on its public symbols yet, so under the global `-fvisibility=hidden` a
  > `SHARED` build would export nothing, and every plugin … links the **static**
  > `libarbc` and carries a private copy of the core objects it references. The
  > dual-build therefore proves that the entry point, the factory, the facets, and
  > service injection all cross the boundary; it does *not* yet prove that a plugin
  > resolves core symbols **from the host image**. Export-annotating the public
  > surface and adding the `BUILD_SHARED_LIBS` CI lane is
  > `packaging.shared_library_build`.

  This paragraph becomes **false** the moment this task lands, and doc 16's
  same-commit rule requires the closer to discharge it in the implementation commit
  (Decision D6).
- **Doc 17:186-191 — the CMake mechanics.** `POSITION_INDEPENDENT_CODE ON` globally
  (already `CMakeLists.txt:15`); *"default `-fvisibility=hidden` with an export macro
  so only the deliberate public API (doc 16) is visible from the shared build"*
  (already `CMakeLists.txt:16-17` for the visibility, missing the macro — this task's
  core). This is the doc's own instruction to build exactly `ARBC_API`.
- **Doc 17:33, 17:72 — the umbrella owns the *"public symbol surface"*.** The
  `arbc` L6 umbrella is *"umbrella target; public symbol surface; built-in kind
  registration"*. `ARBC_API` making that surface concrete is already an umbrella
  responsibility; this task only realizes it.
- **Doc 17:52-55, 57-72 — levelization.** The `ARBC_API` macro header is
  umbrella-owned but *included by every component's public headers*, which is a
  component (L0–L4) including a header that lives with the umbrella (L6) — the one
  case that would invert the level graph. Constraint 3 resolves this: `arbc_api.h`
  ships at the include *root* (`arbc/arbc_api.h`), a spelling
  `scripts/check_levels.py`'s `INCLUDE_RE` (matching `arbc/<component>/…`)
  deliberately does not flag, exactly as `arbc/version.hpp` is handled
  (`cmake/ArbcComponent.cmake:180-186`).
- **Doc 03:227-243 — the two-stage plugin ABI.** Stage 1 (v1): *"link-time or
  `dlopen` with a single `extern "C" arbc_plugin_register(Registry&)` entry point …
  compiler/ABI coupling … acceptable while the interface is still moving."* Stage 2
  (post-v1) is the stable C ABI shim. `ARBC_API` is a **Stage-1** concern — it governs
  how C++ symbols are exported from `libarbc.so`, not the C ABI shim — so this task
  touches neither the entry symbol nor a capability-flag scheme (Decision D7).
- **Doc 16:143-147 — versioning/compatibility.** *"semver from the first tag; pre-1.0
  moves freely … At 1.0: ABI checking (abi-compliance-checker or libabigail) joins
  CI …"* This is where `SOVERSION` policy comes from: pre-1.0 makes no ABI promise, so
  the soname is coarse and the strict ABI-stability discipline activates at 1.0
  (Decision D5).

### Source seams

- **`CMakeLists.txt:1-18`** — `project(… VERSION 0.1.0)` (`:3-7`, `PROJECT_VERSION`
  source of truth for `SOVERSION`/`VERSION`); `CMAKE_POSITION_INDEPENDENT_CODE ON`
  (`:15`, already set); `CMAKE_CXX_VISIBILITY_PRESET hidden` +
  `CMAKE_VISIBILITY_INLINES_HIDDEN ON` (`:16-17`, already set — so today a `SHARED`
  build exports *nothing*, exactly doc 17:174). The comment at `:13-14` already calls
  the components *"object libraries composed into the single shared/static libarbc"*.
- **`cmake/ArbcComponent.cmake:161-209`** — `arbc_finalize_library()`.
  `add_library(arbc "…/version.cpp")` at **`:166`** with **no `STATIC`/`SHARED`
  keyword** — so `arbc` is STATIC by default and becomes SHARED under
  `BUILD_SHARED_LIBS=ON` with no edit to this line. Components are `OBJECT` libraries
  (`:30`) folded in PRIVATE (`:206`); `arbc_build_flags` is `$<BUILD_INTERFACE:>`-
  guarded (`:169`). The `arbc/version.hpp.in` → build-tree-root → `FILE_SET BASE_DIRS`
  pattern (`:187-204`) is the exact shape `arbc/arbc_api.h` reuses.
- **`cmake/ArbcComponent.cmake:23-52`** — `arbc_add_component()`, where the per-object-
  library build-side define (`ARBC_BUILDING`, Decision D2) is threaded so every TU
  that compiles into `libarbc` takes the *export* branch of `ARBC_API`.
- **`cmake/ArbcInstall.cmake:17-20`** — the explicit deferral note: *"the
  BUILD_SHARED_LIBS variant (SOVERSION, ARBC_API) stays with
  `packaging.shared_library_build`."* This task's charter, in the code.
- **`cmake/ArbcInstall.cmake:45-54`** — the fetched-static-zstd object fold
  (`$<TARGET_OBJECTS:libzstd_static>`); the PIC caveat behind Decision D4.
- **`cmake/ArbcInstall.cmake:60-66`** — `install(TARGETS arbc … ARCHIVE/LIBRARY/RUNTIME
  DESTINATION …)` already carries the shared-artifact destinations; no new
  `install()` needed. `SOVERSION`/`VERSION` set as target properties feed this.
- **`src/contract/arbc/contract/plugin.hpp:5-20`** — the existing `ARBC_PLUGIN_EXPORT`
  (default-visibility on the *entry symbol*) and the `extern "C"
  arbc_plugin_register` declaration. The idiom `ARBC_API` mirrors; the symbol it stays
  distinct from (Decision D7).
- **`plugins/imageseq/CMakeLists.txt:32-33`, `plugins/miniaudio/CMakeLists.txt:31-32`,
  `plugins/image/CMakeLists.txt:38`** — the three shipped `MODULE` plugins, each
  linking `arbc` PRIVATE; under the shared build they link `libarbc.so` and gain a
  `DT_NEEDED` on it, so `dlopen` resolves their core references from the single host
  image.
- **`tests/CMakeLists.txt:1185-1193`** — the six `arbc-ci-plugin-*` `MODULE` targets;
  **`:1202-1222`** — `arbc_dual_build_t`, the production-`PluginHost` driver that
  loads them (and `arbc-plugin-imageseq`) and asserts render equivalence + facets +
  service injection.
- **`tests/CMakeLists.txt:1153-1169`** — `arbc_miniaudio_smoke_t` (hardware-gated,
  skipped headless) and `arbc_miniaudio_containment_t` (a **byte scan of the built
  artifacts** taking `$<TARGET_FILE:arbc>` / `$<TARGET_FILE:arbc-plugin-miniaudio>` as
  compile defs). This containment-scan shape — Catch2-only test, artifact paths as
  compile defs — is the exact template for the new symbol-resolution proof.
- **`CMakePresets.json`** (version 5) — ten configure presets, no `BUILD_SHARED_LIBS`
  one; each has a matching build + test preset (`:95-118`). A new lane needs a preset
  trio here.
- **`.github/workflows/ci.yml:40-107`** — the `build-test` matrix; each lane is one
  `include` row naming a `preset` consumed uniformly at `:103`/`:105`/`:107`. Adding a
  lane = one matrix row pointing at the new preset. `install.consumer` runs on the
  ordinary `ctest` step of every lane.
- **`tests/claims/registry.tsv`** — `<claim-id>\t<description>`, claim id
  `<doc-file-stem>#<slug>`; `scripts/check_claims.py` gates bidirectionally.
- **`scripts/check_levels.py`**, **`scripts/gate`** — the levelization gate (must pass
  *unchanged*, Constraint 3) and the local quality gate.

## Constraints / requirements

1. **`arbc/arbc_api.h` is umbrella-owned, includes nothing, and ships at the include
   root.** Like `arbc/version.hpp` it belongs to no component (doc 17:33), installs at
   `<prefix>/include/arbc/arbc_api.h`, includes no other header, and compiles
   standalone and warning-clean under a consumer's own flags (the export interface is
   scrubbed of `arbc_build_flags`, `cmake/ArbcComponent.cmake:169`). It is
   *hand-written and checked in*, not generated — it derives from nothing (Decision
   D1). Its `BASE_DIRS` entry lives only on the umbrella's `FILE_SET`.
2. **The build-side define is on every object library, or the surface exports
   nothing.** CMake sets a target's `<target>_EXPORTS` define only on *that target's
   own* sources; `libarbc`'s public symbols live in the `arbc_<component>` **object
   libraries**, not in the umbrella's lone `version.cpp`. So `ARBC_API` must key off a
   define (`ARBC_BUILDING`) that `arbc_add_component()` puts on **every** object
   library and `arbc_finalize_library()` puts on the umbrella — otherwise every
   component TU takes the *import* branch and the shared build reproduces today's
   "exports nothing" bug (Decision D2).
3. **`scripts/check_levels.py` passes unchanged.** `arbc/arbc_api.h` is included by
   L0–L4 component headers but is umbrella (L6)-owned; its include spelling
   (`arbc/arbc_api.h`, at the root) is not matched by the levelization `INCLUDE_RE`
   (which matches `arbc/<component>/…`), exactly as `arbc/version.hpp` is exempt. No
   new rule, no widened edge.
4. **The static build is unchanged and stays the default.** Every existing lane keeps
   building `libarbc.a`. `ARBC_API` expands to `visibility("default")` when building
   the library and is otherwise inert, so it changes no static-build symbol. `VERSION`
   / `SOVERSION` are ignored for `STATIC` archives, so setting them is a no-op there.
   The new shared build is *additive*: a new preset + lane, never a change to an
   existing one.
5. **Do not narrow or touch the plugin ABI.** `ARBC_PLUGIN_EXPORT` and the `extern "C"
   arbc_plugin_register` entry point (`src/contract/arbc/contract/plugin.hpp`) are
   unchanged. `ARBC_API` governs the *C++ library surface*; the entry symbol is the
   *C ABI seam* (doc 03:227-234). No capability-flag scheme, no plugin ABI number —
   that is Stage 2, post-1.0 (doc 03:236-243, Decision D7).
6. **`SOVERSION` reflects pre-1.0 policy.** `VERSION = ${PROJECT_VERSION}` (0.1.0),
   `SOVERSION = ${PROJECT_VERSION_MAJOR}` (0). Pre-1.0 makes no ABI promise
   (doc 16:143-145); the strict soname / ABI-checking discipline activates at 1.0
   (doc 16:147). A comment on the property records this so a later reader does not read
   `SOVERSION 0` as an ABI guarantee (Decision D5).
7. **The shared lane uses find-first (system) zstd.** Folding a *fetched static* zstd
   into a shared `libarbc` needs PIC objects the fetched build does not guarantee
   (`cmake/ArbcInstall.cmake:45-54`). The new lane leaves zstd found-first, so it is an
   ordinary `.so` dependency of `libarbc.so`. Shared + fetched-static zstd is an
   explicit non-goal (Decision D4).
8. **Lints/gate.** The new/edited CMake passes `gersemi` + `cmake-lint`
   (doc 16:204-205); the new C++ header and test pass the format + warning gates; run
   `scripts/gate` before committing.

## Acceptance criteria

- **New claim `17-internal-components#plugin-resolves-core-symbols-from-host-image`**
  (`tests/claims/registry.tsv`): *Under `BUILD_SHARED_LIBS=ON`, a plugin module
  (`arbc-plugin-imageseq`, `arbc-plugin-miniaudio`, and the six `arbc-ci-plugin-*`)
  references core `ARBC_API` symbols as **undefined imports** resolved at load from the
  single `libarbc.so`, rather than carrying a private static copy — and the eight
  modules still load, register, and render correctly through the production
  `PluginHost` against that shared `libarbc`.* Anchored to doc 17:173-182. Enforced by:

  - **`tests/shared_symbol_resolution.t.cpp`** (new; **built only under
    `BUILD_SHARED_LIBS`**, i.e. wired behind an `if(BUILD_SHARED_LIBS)` in
    `tests/CMakeLists.txt`, so the assertion is never vacuous in the static lanes).
    Modeled on `arbc_miniaudio_containment_t` (`tests/CMakeLists.txt:1160-1169`):
    Catch2-only, taking `$<TARGET_FILE:arbc>` and each plugin module's
    `$<TARGET_FILE:…>` as compile defs, and scanning the *dynamic* symbol tables of
    the built artifacts. Carries the `// enforces:` tag. Asserts, for a curated set of
    core symbols each plugin genuinely references (e.g. a `Registry` registration
    entry point and a `Content`/facet base's exported vtable/type_info): (a) the symbol
    is **exported** (defined, global, default-visibility) from `libarbc.so`; (b) the
    same symbol is **undefined/imported** in each plugin module — the observable that
    *distinguishes* single-instance resolution from a private copy (under the static
    build it would be locally defined). This is the load-bearing new proof; the
    private-copy/single-copy difference is invisible to the existing `dual_build`
    render-equivalence assertions, which pass either way.
  - **`arbc_dual_build_t` + the imageseq/miniaudio load tests, re-run in the shared
    lane** (`tests/CMakeLists.txt:1202-1222`, `:1153-1169`, `:975-1003`) — the eight
    modules load through the production `PluginHost` and pass their existing
    register/factory/facet/service-injection/render assertions against `libarbc.so`.
    No change to these tests; the lane exercises them for free. (The
    `arbc_miniaudio_smoke_t` device path stays hardware-gated and skips headless, as
    today.)

- **New `shared` preset trio in `CMakePresets.json`** (configure inheriting `dev`
  with `BUILD_SHARED_LIBS: ON`, plus matching build + test presets) and **one new
  matrix lane in `.github/workflows/ci.yml`** (`gcc-shared`, `os: ubuntu-latest, cxx:
  g++, preset: shared`) added as a single `include` row (`:44-77` pattern). The lane
  builds `libarbc.so` and runs the full `ctest` suite — including the new resolution
  proof and the re-run loads — on every push.

- **`SOVERSION`/`VERSION` on the installed shared `libarbc`**, off `PROJECT_VERSION`
  (Constraint 6). The out-of-tree consumer (`tests/consumer/`, driven by the
  `install.consumer` CTest, `tests/CMakeLists.txt:1041-1079`) links the staged shared
  library through `find_package(arbc CONFIG)` in the shared lane and loads/uses it —
  proving the `.so` installs, carries its soname, and is consumable exactly as the
  static archive is. (This rides the existing consumer harness; no separate consumer
  project.)

- **The three containment scans stay green against `libarbc.so`.**
  `arbc_imageseq_containment_t`, `arbc_image_containment_t`, and
  `arbc_miniaudio_containment_t` (which scan `$<TARGET_FILE:arbc>`,
  `tests/CMakeLists.txt:1160-1169`) must pass against the `.so` artifact — the
  decode/device symbols stay out of `libarbc` whether it is `.a` or `.so`. If a scan
  is archive-format-specific, it is generalized to read the shared object's dynamic
  table; the *property* (no codec/device symbol in `libarbc`) is artifact-agnostic and
  only gets *stronger* under the shared build.

- **No new golden, conformance, or behavioral-counter suite of its own** — a
  deliberate reading of doc 16's taxonomy. This task adds no content kind or operator
  (the conformance suite it re-runs is `dual_build`'s, doc 16:31-44), renders no new
  pixels (the render equivalence it re-runs is `dual_build`'s byte-exact goldens), and
  makes no wall-clock promise. Its promise is a **build-and-load** invariant; the
  symbol-resolution scan + the re-run loads are the shape that pins it. The shared lane
  *does* run the full existing TSan-adjacent and stress suite against `libarbc.so`
  (whatever the `shared` preset's `ctest` covers), so concurrency behavior is
  exercised against the new artifact at no extra authoring cost.

- **Diff coverage ≥90%** on changed C++ lines (doc 16:112-118). The new test is the
  only new compiled C++; `arbc/arbc_api.h` is a macro-only header (no coverable lines)
  and `ARBC_API` annotations are on existing declarations. If the diff-coverage tool
  counts the macro header's lines as uncovered, exclude it as a non-executable artifact
  (the same gotcha `version_api` named for `version.hpp.in`, `./version_api.md:340-345`).

- **Design-doc delta — doc 17:173-182, discharging the honesty limit — lands in the
  closer's implementation commit (same-commit rule), not ahead of it** (Decision D6).
  The paragraph is rewritten to record that the shared build now exists and a plugin
  resolves core symbols from the single host image on the ELF/shared lane, with the
  MSVC shared build called out as the remaining follow-up. It is *not* pre-written by
  this refinement, because writing "proven" before the lane is green would be exactly
  the aspirational-docs drift doc 16:19-21 forbids.

- **Levelization + build + WBS gate green.** `scripts/check_levels.py` passes
  *unchanged* (Constraint 3); `scripts/check_claims.py` passes with the new registry
  entry and its `enforces:` tag; `scripts/gate` is green; and `tj3 project.tjp 2>&1 |
  grep -iE "error|warning"` is silent after the `.tji` `complete 100` + refinement
  back-link land.

**Deferred follow-up (closer registers as a real WBS leaf, wired into M9
`m9_release`):**

- **`packaging.shared_library_build_msvc`** — *1d.* Add a Windows/MSVC
  `BUILD_SHARED_LIBS=ON` lane and verify `ARBC_API`'s `__declspec(dllexport)` /
  `__declspec(dllimport)` branches are correct across the plugin `dlopen`/`LoadLibrary`
  boundary on MSVC — the one platform where the export/import asymmetry (unlike ELF's
  symmetric `visibility("default")`) actually bites, and where MODULE resolution and
  DLL search paths differ. `depends !shared_library_build`; `note` cites this
  refinement. Milestone **M9**. Registered because the ELF and MSVC shared builds are
  genuinely separable work: this task makes the macro *correct for both* in the header
  but proves and lands only the ELF/gcc lane, and debugging Windows DLL export/search
  is a distinct, concrete engineering task — not a decision to "revisit." (Deferred to
  `packaging.shared_library_build_msvc` — closer registers in WBS, M9.)

**Explicit non-goal, not a deferred task:** shared `libarbc` *combined with* a fetched
static zstd (Constraint 7). It has no consumer today and would be speculative
tech-debt; it is a documented limitation of the shared lane, surfaced for the parking
lot rather than encoded as WBS work.

## Decisions

**D1. `ARBC_API` is a hand-written, checked-in `arbc/arbc_api.h`, not
`generate_export_header()`.**
*Rationale:* the tree already hand-rolls exactly this idiom for the plugin entry
symbol — `ARBC_PLUGIN_EXPORT` at `src/contract/arbc/contract/plugin.hpp:8-12` is a
`_WIN32`/ELF `#if` over `__declspec(dllexport)` / `__attribute__((visibility("default")))`.
A library-wide `ARBC_API` is the same shape one level up, so hand-rolling is the
*consistent* choice, and the macro derives from nothing (unlike `version.hpp`, which
`configure_file`s `PROJECT_VERSION`), so there is nothing to generate.
*Rejected: CMake's `generate_export_header()`.* Its default machinery keys the
export/import branch off `<target>_EXPORTS`, which CMake defines only on the umbrella
target's *own* sources (`version.cpp`) — **not** on the `arbc_<component>` object
libraries whose objects actually carry the public symbols. Left as-is it would mark
the entire component surface *import* and export nothing, reproducing the very bug
doc 17:174 describes. Making it correct means manually setting the define on every
object library anyway (Decision D2) — at which point the generated header buys nothing
over the hand-rolled one and adds a generator step the `plugin.hpp` precedent shows we
do not need.

**D2. The export-side define (`ARBC_BUILDING`) is threaded onto every object library
via `arbc_add_component()`, and onto the umbrella via `arbc_finalize_library()`.**
*Rationale:* this is the one non-obvious mechanic and the reason D6 sized the task at
multi-day. `libarbc`'s public symbols are compiled in the object libraries
(`cmake/ArbcComponent.cmake:30`), not in the umbrella's lone TU. `ARBC_API` must take
the *export* branch for every TU that ends up inside `libarbc` — so the define that
selects that branch has to be a `PRIVATE`/build-side compile definition on **all** of
them, not the single `arbc_EXPORTS` CMake would auto-set. Putting it in
`arbc_add_component()` (`:23-52`) makes it uniform and impossible to forget for a new
component.
*Rejected: rely on `arbc_EXPORTS`.* Only present on `version.cpp` — see D1's rejection;
it is the failure mode, not the mechanism.

**D3. The load-bearing proof is a symbol-table scan (exported-from-`.so` /
undefined-in-plugin), not a new runtime hook on the plugin.**
*Rationale:* the difference between "resolves from the host image" and "carries a
private copy" is precisely a *linkage* property: under the shared build the plugin's
core references are undefined imports satisfied by `libarbc.so`; under the static build
they are local definitions. The existing `dual_build` render/facet/service assertions
cannot see this difference — they pass with two copies as happily as with one. A
dynamic-symbol-table scan of the built artifacts sees it directly, is deterministic,
and reuses the exact Catch2-only + artifact-paths-as-compile-defs shape the three
containment tests already use (`tests/CMakeLists.txt:1160-1169`).
*Rejected: a runtime address-identity check via a plugin-exposed hook* (export a core
symbol's address from the host and from a plugin callback, assert equal). It proves the
same thing but *grows the plugin ABI* — a new entry point on every module — to observe
a property the symbol table already exposes without touching the boundary. Doc 03's
whole posture is to keep the v1 entry surface minimal (a single `extern "C"`); adding
an observation hook contradicts it for no gain.
*Rejected: assert on `nm`/`readelf` output from a shell script in CI only.* A CTest
that runs on `scripts/gate` and every lane (not just CI) is the project's standing
preference (`./version_api.md:229-230`), and keeping the scan in-process with the other
containment tests keeps one idiom, not two.

**D4. The new lane is Linux/gcc `BUILD_SHARED_LIBS=ON` with find-first (system) zstd;
MSVC stays static and shared+fetched-zstd is a non-goal.**
*Rationale:* the honesty gap doc 17:180-182 names is *"resolves core symbols from the
host image"* — an ELF `.so` + `dlopen` property, which the gcc lane proves directly and
which is where every existing plugin-load test already runs. On ELF `ARBC_API` is the
symmetric `visibility("default")`, so the export/import asymmetry that makes MSVC
harder simply does not arise, and the proof is clean. System zstd keeps `libzstd` an
ordinary shared dependency and sidesteps the PIC-fold question
(`cmake/ArbcInstall.cmake:45-54`) entirely; the find-first path is what every non-
`install-fetched` lane already uses, so a system zstd is present on the runner.
*Rejected: add the MSVC shared lane in this task.* The `__declspec` dllexport/dllimport
branch, DLL search-path and MODULE-resolution differences, and Windows load debugging
are a distinct concrete chunk — registered as `packaging.shared_library_build_msvc`
(M9). Bundling it risks a half-working Windows shared build, which D6's own logic
(a half-annotated surface is worse than none) argues against.
*Rejected: make the shared lane also exercise fetched-static zstd.* No consumer needs
shared+fetched today; it would require making `libzstd_static` PIC on speculation.
Documented as a limitation, not carried as unused machinery (doc 03:260-262's
"deferred to their first consumer" precedent).

**D5. `VERSION = ${PROJECT_VERSION}`, `SOVERSION = ${PROJECT_VERSION_MAJOR}` (0), with a
comment that pre-1.0 promises no ABI.**
*Rationale:* the soname is what a shared build needs to install correctly, and off
`PROJECT_VERSION` it keeps the single-source-of-truth `version_api` established
(`./version_api.md` Constraint 1). Pre-1.0, doc 16:143-145 says the project *"moves
freely"* and ABI checking joins CI only *at 1.0* (16:147) — so a coarse
`SOVERSION 0` is honest: it does not claim 0.1↔0.2 ABI compatibility, and the strict
soname-per-incompatible-release discipline arrives with the 1.0 ABI-checking work item,
not here. The comment prevents a future reader from over-reading the number.
*Rejected: `SOVERSION` = full `MAJOR.MINOR` to bump the soname each 0.x minor.* That
imports 1.0-grade ABI-stability discipline into a pre-1.0 line doc 16 explicitly says
moves freely — machinery ahead of the policy that would use it.

**D6. The doc-17 honesty-limit delta (17:173-182) is scoped here but written by the
closer in the same commit as the implementation — no ahead-of-time delta.**
*Rationale:* unlike `version_api`, which wrote a genuinely *undecided* mechanism into
doc 10 ahead of implementation, doc 17:173-191 **already fully specifies** this task's
mechanism (the export macro, `-fvisibility=hidden`, the `BUILD_SHARED_LIBS` lane, PIC).
There is no new design decision to record — only a *factual discharge* of a limit the
doc itself flagged as temporary and named this task to close. Writing "proven" before
the lane is green would claim an invariant that does not yet hold, the exact
aspirational-docs drift doc 16:19-21 exists to prevent. So the closer flips 17:173-182
from "does not yet prove … is `packaging.shared_library_build`" to "proven by the
`gcc-shared` lane for the ELF build; the MSVC shared build is
`packaging.shared_library_build_msvc`" in the commit that makes it true.
*No doc-00 decision-record bullet.* The project-shaping calls are already recorded
elsewhere: doc 17:33 settled that the umbrella owns the public symbol surface, doc
17:186-191 settled the export-macro mechanism, and doc 00:110-112 / doc 03:225-243
settled the two-stage plugin ABI D7 declines to touch. This task fills in an
implementation the constitution already blessed; it is a doc-17 factual update, not a
constitutional amendment.

**D7. `ARBC_API` (library surface) and `ARBC_PLUGIN_EXPORT` (plugin entry symbol) stay
distinct; the plugin ABI is untouched.**
*Rationale:* they answer different questions. `ARBC_PLUGIN_EXPORT`
(`plugin.hpp:8-12`) makes the *plugin's* single `extern "C" arbc_plugin_register`
symbol visible *out of the plugin* — a Stage-1 C-ABI seam (doc 03:227-234). `ARBC_API`
makes *`libarbc`'s* C++ public surface visible *out of the host image* so plugins can
resolve it. Merging them, or minting a plugin ABI version number while here, would
pre-empt Stage 2's semver-gated C ABI (doc 03:236-243), which doc 16:147 activates at
1.0 — advertising a compatibility promise the v1 same-toolchain interface explicitly
does not make (the same call `version_api` D4 made, `./version_api.md:438-452`).
*Rejected: fold plugin export and library export into one macro, or add
`arbc_plugin_abi_version()`.* Freezes a Stage-2 promise before its ABI exists; buys
false confidence.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-15.

- `src/api/arbc/arbc_api.h` — new umbrella-owned, hand-written macro header defining `ARBC_API` with `__attribute__((visibility("default")))` / `__declspec(dllexport|dllimport)` branches; includes nothing, ships at the include root.
- `cmake/ArbcComponent.cmake` — `ARBC_BUILDING` compile definition threaded onto every `arbc_<component>` object library via `arbc_add_component()` and onto the umbrella via `arbc_finalize_library()`; `SOVERSION`/`VERSION` set off `PROJECT_VERSION` (pre-1.0 policy, Decision D5).
- ~80 public headers across all components annotated with `ARBC_API` (all `src/**/arbc/**/*.hpp` + `src/contract/render_completion.cpp`), making the deliberate public surface explicit under `-fvisibility=hidden`.
- `CMakePresets.json` — `shared` configure/build/test preset trio with `BUILD_SHARED_LIBS: ON`, inheriting `dev`.
- `.github/workflows/ci.yml` — `gcc-shared` CI matrix lane (ubuntu-latest, g++, preset: shared) running the full ctest suite including the new resolution proof and re-run plugin loads on every push.
- `tests/shared_symbol_resolution.t.cpp` (new) — Catch2 symbol-table scan asserting that core `ARBC_API` symbols are exported from `libarbc.so` and appear as undefined imports in each plugin module; enforces claim `17-internal-components#plugin-resolves-core-symbols-from-host-image`.
- `tests/claims/registry.tsv` — new claim `17-internal-components#plugin-resolves-core-symbols-from-host-image` registered.
- `docs/design/17-internal-components.md` — doc 17:173-182 honesty-limit paragraph rewritten to record that the `gcc-shared` lane now proves single-instance symbol resolution; MSVC shared build noted as `packaging.shared_library_build_msvc` (same-commit rule, Decision D6).
- `tests/stress_arena_accounting_poll.t.cpp` — fixer added writer/poller rendezvous to eliminate pre-existing harness race (unrelated to ARBC_API; surfaced by the new shared lane's test run).
