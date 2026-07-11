# kinds.dual_build — Dual-build dlopen proof

## TaskJuggler entry

`tasks/55-kinds.tji:81-85`:

```
  task dual_build "Dual-build dlopen proof" {
    effort 1d
    allocate team
    note "Each in-lib kind also links into a CI-only shared library loaded via the extern C entry point, keeping the plugin path honest. Docs 03/17."
  }
```

Milestone **M9** (`tasks/99-milestones.tji:71`). No `depends` clause of its own;
it inherits `kinds`' `depends contract.conformance_suite` (`tasks/55-kinds.tji:4`).

## Effort estimate

**1d.** Every seam this task needs already exists and is proven: the `extern "C"`
entry point (`src/contract/arbc/contract/plugin.hpp:20`), the `Registry`
(`src/contract/arbc/contract/registry.hpp`), the production loader
(`src/runtime/arbc/runtime/plugin_host.hpp`), the hand-rolled `MODULE` +
compile-definition test pattern (`plugins/imageseq/CMakeLists.txt:29-30`,
`tests/CMakeLists.txt:617-624`), a CI-only `MODULE` fixture precedent
(`tests/fixtures/noentry_plugin.cpp`), and the conformance suite
(`testing/arbc/testing/contract_tests.hpp`). The day is six small plugin
translation units, six `MODULE` targets, one test driver, and one claim. No new
component, no new public API on any shipped target, no new dependency.

## Inherited dependencies

**Settled (landed, and this task builds directly on them):**

- **`kinds.imageseq_plugin`** (`tasks/55-kinds.tji:75-80`, refinement
  `tasks/refinements/kinds/imageseq_plugin.md`) — landed the `Registry` +
  `extern "C" arbc_plugin_register(Registry&)` seam and the hand-rolled
  out-of-lib `MODULE` build. Its Decision 1
  (`tasks/refinements/kinds/imageseq_plugin.md:381-389`) scoped the seam; its
  deferral line (`:376-377`) names *this* task as the consumer: "`kinds.dual_build`
  reuses this entry point for the in-lib kinds' CI `.so`s — already wired."
- **`runtime.plugin_loading`** (+ `runtime.plugin_loading_win32`) — landed
  `arbc::PluginHost` (`src/runtime/arbc/runtime/plugin_host.hpp:119-156`): the
  by-path `load_plugin`, the opt-in `ARBC_PLUGIN_PATH` `scan_plugin_path`, the
  errors-as-values surface, and — load-bearing here — the **destroy-order
  contract** (`plugin_host.hpp:154-155`: `d_handles` declared before
  `d_registry`, so every factory dies before its image is unmapped). This task
  drives the production loader rather than re-hand-rolling
  `dlopen`/`dlsym`/`dlclose`.
- **`contract.conformance_suite`** — `arbc-testing`
  (`testing/arbc/testing/contract_tests.hpp:44`,
  `arbc::testing::ContentFactory = std::function<std::unique_ptr<Content>()>`).
  Every in-lib kind already runs it in-process; this task re-runs the *same*
  suite over a factory obtained across the `dlopen` boundary.
- **The six kinds themselves** — `kinds.tone`, `kinds.raster` (+
  `raster_pool_backing`, `raster_runtime_binding`, `raster_resampling_quality`),
  `kinds.nested` (+ `nested_audio`, `nested_audio_resampling`),
  `operators.fade`, `operators.crossfade`, and `org.arbc.solid`. Their public
  headers (`src/kind_*/arbc/kind_*/*.hpp`) are the entire construction surface
  this task consumes.
- **`serialize.kind_params` / `operators.*_runtime_binding`** — established the
  *in-lib* instantiation shape this task mirrors across the boundary: construct
  the content from params, then **bind services separately**
  (`src/runtime/codec_fade.cpp:104-140`, `register_fade_binder`'s
  `attach`/`detach` pair).

**Pending (explicitly NOT depended on):**

- **`packaging.plugin_helper`** (`tasks/75-packaging.tji:11-16`, also M9) — the
  shipped `arbc_add_plugin()` CMake helper. It is scoped to *third-party,
  post-install* plugin authors and depends on `packaging.install`; there is no
  ordering edge between it and this task
  (`tasks/99-milestones.tji:71` lists both flat under M9). These CI modules are
  in-tree, never installed, and hand-rolled exactly as imageseq's is (Decision 4).
- **`packaging.install`** — nothing here is an installed target.
- **A shared `libarbc`** — see Decision 6 and the doc-17 delta; not a
  prerequisite, and its absence is what `packaging.shared_library_build` (a
  registered follow-up) closes.

## What this task is

Six CI-only loadable modules, one per in-lib kind (`org.arbc.solid`,
`org.arbc.tone`, `org.arbc.raster`, `org.arbc.nested`, `org.arbc.fade`,
`org.arbc.crossfade`), each a single translation unit exposing the doc-03
`extern "C" arbc_plugin_register(Registry&)` entry point and registering exactly
that one kind id with a `ContentFactory` and `KindMetadata`. Plus one
cross-component test driver that loads each module through the production
`arbc::PluginHost`, obtains the kind's factory *only* across the boundary,
constructs the content, injects host-side services where the kind needs them,
renders it, runs the `arbc-testing` conformance suite over it, and asserts its
pixels are **byte-identical** to the same content constructed in-lib.

Nothing here ships. The modules are built under `tests/`, are never installed,
and add no public API to any `arbc_*` component, to `arbc`, or to `arbc-testing`.

## Why it needs to be done

Doc 17:167-174 ("The codec line") states the consequence of the built-in kinds
living inside `libarbc`: "This also makes imageseq the permanent end-to-end test
of the real plugin path, **which the in-lib kinds no longer exercise at
runtime**." Doc 03:233-234 says the same from the other side: "the other six
kinds link into `libarbc` and only exercise that path through the CI-only
dual-build (doc 17)."

So today exactly one kind — imageseq, whose shape (a `Timed` source over an
opaque directory config, no service injection, no `Editable` facet) is the
*easiest* case — proves the plugin path. The six in-lib kinds cover the branches
imageseq does not: an `Editable` content with a second base subobject
(`RasterContent : public Content, public Editable`,
`src/kind_raster/arbc/kind_raster/raster_content.hpp:225`), an audio-facet
content (`ToneContent::audio()`), and three kinds that are **unrenderable until
a host injects a `PullService` and a `Backend`** (`NestedContent::attach`,
`src/kind_nested/arbc/kind_nested/nested_content.hpp:68`; `FadeContent::attach`,
`src/kind_fade/arbc/kind_fade/fade_content.hpp:54`). None of those has ever been
constructed in one image and driven from another.

Doc 17:129-132 is the promise this task discharges, and its rationale for the
whole object-library architecture: "Dual-build of kinds falls out. Each `kind-*`
object library also links into a tiny per-kind shared library in CI, loaded via
`dlopen` through the doc-03 entry point — **proving the plugin path stays
honest** without shipping six .so files nobody wants." Until this lands, that
bullet is an unproven claim about the build, and a regression in the plugin seam
(a header that stops being self-contained across a boundary, a facet whose
pointer adjustment breaks, a service injection that a plugin-image content
cannot receive) is caught by nothing.

Downstream: `packaging.shared_library_build` (registered below) re-runs exactly
these six loads against a shared `libarbc`; `runtime.plugin_operator_registration`
(registered below) closes the operator-plugin gap this task's fade/crossfade
modules make visible.

## Inputs / context

**Governing design-doc sections (normative, doc 16):**

- `docs/design/17-internal-components.md` §"Why object libraries specifically"
  bullet 3 (17:129-132, amended by this task — see Decisions) — the dual-build
  promise. §"Shipped artifacts" (17:9-17) — `libarbc` is the *only* artifact
  the six kinds ship in; there is no per-kind `.so` in the shipped set.
  §"The codec line" (17:167-174) — why the in-lib kinds stopped exercising the
  runtime plugin path. §CMake mechanics (17:135-141) — `POSITION_INDEPENDENT_CODE
  ON` globally, "default `-fvisibility=hidden` with an export macro so only the
  deliberate public API is visible from the shared build". §L4 `kind-*` row
  (17:59) — the six kinds depend on `contract` (+ below) only.
- `docs/design/03-layer-plugin-interface.md` §"Plugin mechanism" Stage 1
  (03:165-174) — "link-time or `dlopen` with a single `extern "C"
  arbc_plugin_register(Registry&)` entry point". §Boundary discipline
  (03:176-183) — "no exceptions across the boundary (errors are values) … all
  allocation ownership one-directional". §Registry (03:191-210) — reverse-DNS
  ids, "validated only for non-emptiness and per-registry uniqueness: they are
  opaque persistent tokens, not structurally parsed"; "A `Registry` is populated
  during single-threaded startup or plugin load and is read-only for the
  remainder of a session". §Reference implementations (03:227-234) — "the other
  six kinds link into `libarbc` and only exercise that path through the CI-only
  dual-build".
- `docs/design/16-sdlc-and-quality.md` — claims register, ≥90% diff coverage,
  byte-exact goldens as the default.

**Existing code this task consumes (unchanged):**

- `src/contract/arbc/contract/plugin.hpp:8-20` — `ARBC_PLUGIN_EXPORT`
  (`__declspec(dllexport)` on `_WIN32`, `__attribute__((visibility("default")))`
  elsewhere) and the sole symbol
  `extern "C" ARBC_PLUGIN_EXPORT void arbc_plugin_register(arbc::Registry&)`.
  **No ABI version, no descriptor struct** — Stage 2's concern (03:176-183).
- `src/contract/arbc/contract/registry.hpp:35-40` —
  `using ContentConfig = std::string_view;` and
  `ContentFactory = std::function<expected<std::unique_ptr<Content>, std::string>(ContentConfig)>`.
  The config is **opaque and kind-defined**; it carries no input edges and no
  service handles. `:57` `Registry::add(id, factory, metadata)`, `:61`
  `factory(id)`, `:64` `metadata(id)`, `:67` `size()`, `:70` `ids()`.
- `src/runtime/arbc/runtime/plugin_host.hpp:119-156` — `PluginHost::registry()`,
  `load_plugin(path)`, `scan_plugin_path()`; `:49-59` `PluginLoadError`; `:65-84`
  `PluginScanEntry` / `PluginScanReport`; `:154-155` the destroy-order contract.
- `src/runtime/plugin_host.cpp:46` — `k_entry_point = "arbc_plugin_register"`;
  `:127` `dlopen(path, RTLD_NOW | RTLD_LOCAL)`; `:173-179` — `DuplicateId` is
  inferred from `registry.size()` not growing, because the entry point returns
  `void`.
- `testing/arbc/testing/contract_tests.hpp:44` —
  `arbc::testing::ContentFactory = std::function<std::unique_ptr<Content>()>`
  (**no config argument** — different type from `arbc::ContentFactory`; the
  driver adapts). `:48-79` `Options` (`seed`, `cases`, `width`/`height`,
  `snapshot_sensitive`, per-family toggles including the audio families).

**Kind construction surfaces (the whole of what the six plugin TUs touch):**

| kind | constructor | needs after construction |
| --- | --- | --- |
| `org.arbc.solid` | `SolidContent(Rgba premultiplied, std::optional<Rect> bounds = nullopt)` (`src/kind_solid/arbc/kind_solid/solid_content.hpp:22`) | — |
| `org.arbc.tone` | `ToneContent(std::uint32_t frequency_hz, float amplitude)` (`src/kind_tone/arbc/kind_tone/tone_content.hpp:23`) | — |
| `org.arbc.raster` | `RasterContent(DecodedImage image, int tile_edge = k_default_tile_edge)` (`src/kind_raster/arbc/kind_raster/raster_content.hpp:227`) | — (exposes `Editable*` via `editable()`, `:237`) |
| `org.arbc.nested` | `NestedContent(ObjectId child)` (`src/kind_nested/arbc/kind_nested/nested_content.hpp:57`) | `attach(PullService&, Backend&, NestedResolver, const DocRoot&)` (`:68`) |
| `org.arbc.fade` | `FadeContent(ContentRef input, FadeParams)` (`src/kind_fade/arbc/kind_fade/fade_content.hpp:48`) | `attach(PullService&, Backend&)` (`:54`), `detach()` (`:60`) |
| `org.arbc.crossfade` | `CrossfadeContent(ContentRef from, ContentRef to, CrossfadeParams)` (`src/kind_crossfade/arbc/kind_crossfade/crossfade_content.hpp:47`) | `attach(PullService&, Backend&)` |

`ContentRef` is `Content*` (`src/contract/arbc/contract/content.hpp:212`) — a raw
borrowed pointer. That is the crux for fade/crossfade: an input edge **cannot** be
expressed in a `ContentConfig` string, so the plugin owns the input it hands its
operator (Decision 3).

**Build/test patterns to mirror:**

- `plugins/imageseq/CMakeLists.txt:29-30` — `add_library(<name> MODULE <one.cpp>)`,
  `target_link_libraries(<name> PRIVATE ... arbc arbc_build_flags)`.
- `tests/CMakeLists.txt:630-631` — `arbc-plugin-noentry`, the existing CI-only
  `MODULE` fixture built from `tests/fixtures/noentry_plugin.cpp`.
- `tests/CMakeLists.txt:617-624`, `:636-645` — `$<TARGET_FILE:...>` /
  `$<TARGET_FILE_DIR:...>` handed to the test binary as compile definitions;
  `${CMAKE_DL_LIBS}` on the link line.
- `tests/CMakeLists.txt:556-562` — why plugin-driving tests live under `tests/`:
  `check_claims.py` and `check_levels.py` scan `src`/`tests`/`testing`, not
  `plugins/`.
- `tests/imageseq_plugin_path.t.cpp` — the boundary-only discipline: the test
  binary does **not** link the plugin's impl; it reaches the kind solely across
  `extern "C"`.
- `tests/fade_conformance.t.cpp:30-58` — `InlineAudioPull`, the host-side
  `PullService` double the operator conformance drivers already inject; the
  dual-build driver reuses this exact shape (Decision 3).
- `.github/workflows/ci.yml:38-60` — the build/test matrix (`gcc-debug`,
  `gcc-release`, `clang-debug`, `clang-asan`, `gcc-tsan`, `clang-rtsan`,
  `msvc-debug`). These modules build and run in **every** lane, including
  `msvc-debug`; no new lane is added.

## Constraints / requirements

1. **The test binary must never link a kind's implementation directly.** It
   reaches each of the six kinds **only** across the `extern "C"` boundary, as
   `tests/imageseq_plugin_path.t.cpp` does. The one exception is the byte-equality
   assertion (Acceptance §3), which deliberately constructs the *same* content
   in-lib to compare against — and that is precisely why the driver must obtain
   its plugin-side instance through `PluginHost::registry().factory(id)` and never
   through the in-lib constructor it also calls.

2. **Nothing this task adds ships.** No new public header on any `arbc_*`
   component; no new symbol on `arbc`; no addition to `arbc-testing`; no
   installed target. The six modules are `MODULE` libraries declared in
   `tests/CMakeLists.txt`, output into a dedicated directory (Constraint 6), and
   guarded by `BUILD_TESTING` like every other test target. Doc 17:129-132's
   "without shipping six .so files nobody wants" is a requirement, not a
   flourish.

3. **The CI plugins are codec-free and dependency-free.** `org.arbc.raster`'s
   module synthesizes its `DecodedImage` arithmetically (a deterministic
   gradient) from a `WxH` config — it must not gain a decoder. This is doc
   17:169's codec line applied to CI glue: the only artifact in the tree that
   carries a decode dependency is `arbc-plugin-imageseq`.

4. **Each module registers exactly one kind id.** After `load_plugin`, the
   host's `Registry::size()` must have grown by **exactly 1**, and `ids()` must
   contain exactly that kind's reverse-DNS id. This is the assertion that
   catches an accidental static registrar or a transitively-dragged registration
   — the failure mode doc 17:119-124's bullet 1 is about.

5. **Errors are values across the boundary** (03:176-183). Every module's factory
   returns `expected<std::unique_ptr<Content>, std::string>`; a malformed config
   is an error **value**, never a throw. The driver asserts this for each kind
   (a garbage config string yields `!has_value()`), under `REQUIRE_NOTHROW`.

6. **Lifetime: no `Content` may outlive its image.** The driver holds every
   plugin-derived `Content` in a scope strictly inside the `PluginHost`'s, so the
   `PluginHost` destroy-order contract (`plugin_host.hpp:154-155`) does the
   `dlclose` last. The fade/crossfade modules' plugin-owned inputs (Decision 3)
   live in a module-local holder destroyed at image unload — i.e. **after** every
   operator that borrows them. The driver must not stash a plugin `Content` in a
   Catch2 static or a `GENERATE`d fixture that outlives the host.

7. **The six modules get their own output directory.** `LIBRARY_OUTPUT_DIRECTORY`
   (and `RUNTIME_OUTPUT_DIRECTORY` for the Windows `.dll`) set to a dedicated
   `ci_plugins/` subdirectory of the test binary dir, so the
   `scan_plugin_path()` assertion (Acceptance §2) sweeps exactly these six and
   nothing else. Without this the scan would also find `arbc-plugin-imageseq`,
   `arbc-plugin-noentry`, and `arbc-plugin-miniaudio`, and the count assertion
   would be a moving target.

8. **Every CI lane, including `msvc-debug`.** The entry point is already
   `__declspec(dllexport)`-annotated (`plugin.hpp:9`) and `PluginHost` already
   has its `LoadLibrary`/`FreeLibrary` seam (`plugin_host.cpp:9-16, 31-39`), so a
   `MODULE` target builds a `.dll` and loads on Windows with no new platform
   code. No `#ifdef _WIN32` may appear in the six plugin TUs.

9. **Levelization is untouched.** The modules are not components; they are not
   registered with `arbc_add_component()`, so `scripts/check_levels.py` neither
   sees them nor needs to. They link the umbrella `arbc` (Decision 4), exactly
   as `plugins/imageseq` does.

## Acceptance criteria

**1. Six CI-only modules, one per kind, each loaded through the production loader.**
`tests/ci_plugins/{solid,tone,raster,nested,fade,crossfade}_ci_plugin.cpp`, each
built into an `arbc-ci-plugin-<kind>` `MODULE` target. A new cross-component
driver `tests/dual_build.t.cpp` (linking `arbc`, `arbc-testing`, Catch2, and
`${CMAKE_DL_LIBS}`; **not** linking any plugin) parameterizes over the six and,
for each:

- `PluginHost host; REQUIRE(host.load_plugin(ARBC_CI_PLUGIN_<KIND>_FILE));`
- `REQUIRE(host.registry().size() == 1);` and `ids()` names exactly the kind's
  reverse-DNS id (Constraint 4);
- `metadata(id)->human_name` is non-empty and `version` is `"1"`;
- the factory constructs a `Content` from the module's config; a garbage config
  is an error **value** under `REQUIRE_NOTHROW` (Constraint 5);
- the constructed content answers its description methods (`bounds()`,
  `stability()`, `time_extent()`) with the same values the in-lib instance does;
- a duplicate `registry().add(id, ...)` returns `RegistryError::DuplicateId`.

**2. One `PluginHost` aggregates all six plus imageseq, and the directory scan
finds exactly six.** Two assertions in the same driver:

- explicit: seven successive `load_plugin` calls (the six CI modules +
  `arbc-plugin-imageseq`) leave one `Registry` holding seven distinct ids, in
  registration order, with no collision;
- opt-in scan: `ARBC_PLUGIN_PATH` set to the dedicated `ci_plugins/` directory
  (Constraint 7) yields `PluginScanReport{loaded == 6}` with every entry
  `Outcome::Loaded`, in deterministic lexicographic order. This re-asserts
  `03-layer-plugin-interface#plugin-path-scan-is-opt-in` over six images rather
  than one.

**3. Byte-exact in-lib/plugin render equivalence — the core proof.** For each of
the six, the driver renders (a) the content the plugin factory produced and (b) an
identically-configured content constructed in-lib, through the *same* `CpuBackend`
target, the *same* `RenderRequest`, and the *same* injected services, and asserts
the target pixels are **byte-identical** and the `RenderResult` fields
(`achieved_scale`, `achieved_time`, `exactness`, `provided`) compare equal. This
is an equivalence, not a checked-in golden — the kinds' absolute pixel goldens
already live in `src/kind_*/t/` and `tests/*_goldens.t.cpp`, and duplicating them
here would pin the same bytes twice. Byte-exact, per doc 16 (no tolerance).

**4. Host services inject into a plugin-image content, and dispatch crosses both
ways.** For `nested`, `fade`, and `crossfade`, the driver constructs the content
across the boundary *unattached*, then calls `attach(...)` with a **host-side**
`PullService` double (the `InlineAudioPull` shape of
`tests/fade_conformance.t.cpp:30-58`) and a host-side `CpuBackend`, and renders.
This exercises: host → plugin (the `Content` vtable), and plugin → host (the
plugin's render body calling `PullService::pull` on an interface implemented in
the test binary). `detach()` then `render()` must fault as unattached rather than
touch a released service (`fade_content.hpp:56-60`), asserted across the boundary.

**5. Facets survive the boundary.** `tone`'s `audio()` returns a non-null
`AudioFacet*` with stable pointer identity across calls, and `render_audio`
settles once. `raster`'s `editable()` returns a non-null `Editable*` — a
**second base subobject**, so this asserts the plugin-side pointer adjustment —
and a `capture()` → render → `restore()` round-trip across the boundary
reproduces its pixels byte-for-byte.

**6. Conformance suite over the plugin-obtained factory.** The driver adapts each
`arbc::ContentFactory` (config → `expected<unique_ptr<Content>, string>`) into an
`arbc::testing::ContentFactory` (`() -> unique_ptr<Content>`, closing over the
config and, for the three service-injected kinds, over the host doubles and the
`attach` call) and runs `arbc::testing::contract_tests(factory, options)` on it —
the *same* suite, the *same* seed, the *same* options each kind's in-lib
conformance driver already uses (`tests/{tone,raster,nested,fade,crossfade}_conformance.t.cpp`,
`tests/contract_conformance.t.cpp` for solid; `Options::snapshot_sensitive = true`
for raster). Link order per house rule: `arbc-testing` **before** `arbc`
(`cmake/ArbcComponent.cmake:86-92`).

**7. Claims register.**

- **New entry** in `tests/claims/registry.tsv`:
  `17-internal-components#in-lib-kinds-dual-build-through-plugin-entry` —
  "Each of the six in-lib kinds also builds as a CI-only shared library whose
  `extern "C" arbc_plugin_register` registers exactly that one kind id into a
  `Registry`; dlopening it through `PluginHost` yields a factory that constructs
  a working `Content` whose facets (`audio()`, `editable()`), service injection
  (`attach`/`detach`), and rendered pixels are byte-identical to the in-lib
  instance — the in-lib kinds' only exercise of the runtime plugin path
  (doc 17:129-132, doc 03:233-234)." Tagged `// enforces:` on the driver's
  equivalence test case.
- **Re-asserted, not re-registered** (per the imageseq convention,
  `tasks/refinements/kinds/imageseq_plugin.md:292-293`): a second `enforces:` tag
  on the driver for
  `03-layer-plugin-interface#plugin-registers-through-extern-c-entry`
  (registry.tsv:181),
  `03-layer-plugin-interface#registry-resolves-kind-id-to-factory-and-metadata`
  (:202), `03-layer-plugin-interface#plugin-path-scan-is-opt-in` (:221), and
  `03-layer-plugin-interface#loader-errors-are-values` (:223) — each now proven
  over six images instead of one.
- The contract-family claims (registry.tsv:64-112, :186-187) are re-asserted
  implicitly by the conformance run and need no new tag; the suite carries its
  own.

**8. Gates.** `scripts/gate` green (configure, build, `ctest`, clang-format,
`check_levels.py`, `check_claims.py`, `check_rt_safety.py`); ≥90% diff coverage
on the changed lines — the six plugin TUs are compiled *and executed* by the
driver, so they carry their own coverage; `msvc-debug` green (Constraint 8). No
TSan/stress scope: this task adds no concurrency (`Registry` is
"populated during single-threaded startup or plugin load and read-only for the
remainder of a session", 03:207-210).

**Deferred follow-ups (closer registers into the WBS / wires the milestone edge):**

- **`packaging.shared_library_build`** — "Export-annotate `libarbc`'s public
  symbol surface (an `ARBC_API` macro on the public headers, per doc 17:135-141),
  add a `BUILD_SHARED_LIBS=ON` CMake preset and CI lane, and re-run the
  `arbc-plugin-imageseq`, `arbc-plugin-miniaudio` and the six `arbc-ci-plugin-*`
  loads against the shared `libarbc` — so a plugin resolves core symbols **from
  the host image** instead of carrying a private copy of them." Effort **3d**,
  area `packaging` (`tasks/75-packaging.tji`), `depends kinds.dual_build,
  kinds.imageseq_plugin`, milestone **M9**. Source-of-debt: this refinement
  (Decision 6). This is the residual honesty gap the doc-17 delta records
  explicitly rather than hides.
- **`runtime.plugin_operator_registration`** — "Extend the plugin entry-point
  seam so an out-of-lib kind can register more than a `ContentConfig` factory:
  its serialize codec (`params` + input arity, the
  `deserialize(params, span<const ContentRef>, LoadContext&)` shape of
  `src/runtime/codec_fade.cpp:104`) and its operator binder (`attach`/`detach`,
  `src/runtime/operator_binding.cpp:45`), so a third-party *operator* kind is
  loadable into a `Document` and not merely constructible." Effort **3d**, area
  `runtime` (`tasks/65-runtime.tji`), `depends kinds.dual_build,
  runtime.plugin_loading, serialize.kind_params`, milestone **M9**.
  Source-of-debt: this refinement (Decision 3) — the fade/crossfade modules make
  the gap concrete: they can construct an operator across the boundary but
  cannot make one participate in a serialized document.

## Decisions

1. **Six separate modules, one kind each — not one module registering all six.**
   *Rejected: a single `arbc-ci-plugin-kinds.so`.* Doc 17:129-130 says "Each
   `kind-*` object library also links into a **tiny per-kind** shared library",
   and the per-kind split is what makes Constraint 4's assertion
   (`registry.size() == 1` after a load) meaningful: a single omnibus module
   would register six ids at once and could not distinguish "kind X registered"
   from "kind X was dragged in transitively by kind Y". Six modules also make the
   `scan_plugin_path()` count assertion (Acceptance §2) a real six-image sweep
   rather than a one-image one. The cost is six near-identical 15-line TUs — an
   acceptable price for an assertion that actually discriminates.

2. **Each module defines its own minimal, opaque config string; no shared config
   grammar, and no JSON.** `ContentConfig` is `std::string_view` and doc 03:203-207
   makes it explicitly kind-defined and unparsed by the core. So: solid takes
   `"r,g,b,a"` (premultiplied working-space floats), tone `"<freq_hz>,<amplitude>"`,
   raster `"<w>x<h>"` (a deterministic synthesized gradient, Constraint 3), nested
   the child `ObjectId` in decimal, fade/crossfade their window/shape parameters.
   *Rejected: reusing the JSON `params` grammar of `serialize.kind_params`
   (`src/runtime/codec_*.cpp`).* That would put `nlohmann::json` on a CI plugin's
   link line for no gain (nlohmann is `PRIVATE` to `arbc_serialize` and does not
   propagate through `arbc`), and it would test the *codec* seam, which
   `tests/serialize_kind_params.t.cpp` already pins in-process. The dual-build's
   subject is the **entry point**, not the parameter grammar.

3. **fade/crossfade's input edge is owned by the plugin module; nested's child is
   resolved host-side.** `FadeContent(ContentRef input, …)` takes a raw
   `Content*` (`content.hpp:212`), and a `ContentConfig` string cannot carry a
   pointer — the v1 `Registry` factory has no input-arity slot at all (the
   codec seam does: `deserialize_fade(params, span<const ContentRef> inputs, ctx)`,
   `src/runtime/codec_fade.cpp:104`). So the fade/crossfade modules construct
   their own inputs inside the module — a `SolidContent`/`ToneContent` for fade's
   one edge, two of them for `CrossfadeContent(ContentRef from, ContentRef to, …)`
   (`crossfade_content.hpp:47`) — and keep them in a module-local holder whose
   destruction runs at image unload, i.e. strictly after the operator that borrows
   them, since `PluginHost` `dlclose`s last (`plugin_host.hpp:154-155`). `NestedContent(ObjectId child)` needs no such
   trick: the child is an id, and resolution happens host-side through the
   injected `NestedResolver` at `attach`.
   *Rejected: deriving a module-local owning subclass* — `FadeContent` is
   `final` (`fade_content.hpp:46`), and a wrapper `Content` that merely delegates
   would hide `inputs()`/`identity()` and defeat the point.
   *Rejected: widening `ContentFactory` to carry inputs* — that is a change to
   the shipped v1 plugin seam for the benefit of a CI proof, and it is exactly
   the design work `runtime.plugin_operator_registration` (registered above) owns,
   with the serialize codec and the operator binder in scope together. Making the
   gap **visible and named** is the right outcome of this task; papering over it
   in the seam is not.

4. **The modules link the umbrella `arbc`, not the `kind-*` object libraries.**
   Doc 17:129 speaks of "each `kind-*` object library also links into" a shared
   library, but `cmake/ArbcComponent.cmake:11-12` states the countervailing rule:
   "Nothing links object libraries except the umbrella `arbc` target … This avoids
   duplicate-object pitfalls with transitive object-library linking." Linking
   `arbc` satisfies the doc's intent — the static archive supplies exactly the
   kind's objects plus the transitive closure the linker needs — while staying on
   the one link shape the repo has proven (`plugins/imageseq/CMakeLists.txt:30`,
   `plugins/miniaudio/`). The doc-17 delta records the concrete shape so the two
   sentences no longer read as being in tension.
   *Rejected: `target_link_libraries(mod PRIVATE arbc_kind_solid)`.* It would put
   the same objects on the link line twice as soon as anything else pulled the
   archive, and it is the pitfall the helper's own comment warns against.

5. **Hand-rolled `MODULE` targets in `tests/CMakeLists.txt`, not
   `arbc_add_plugin()`.** `packaging.plugin_helper` is M9 with no ordering edge to
   this task, and its helper is scoped to *third-party, post-install* authors
   (`tasks/75-packaging.tji:11-16`) — these targets are in-tree, CI-only, and
   never installed. Hand-rolling mirrors what both shipped plugins already do
   (`plugins/imageseq/CMakeLists.txt:5-8` says so in as many words). If
   `packaging.plugin_helper` later wants a second migration target beyond
   imageseq, these six are trivially convertible — but taking a dependency on an
   unwritten helper to save six `add_library(... MODULE ...)` lines is a bad trade.
   *Rejected: declaring the modules under `plugins/`.* They are not shipped
   artifacts, and `check_claims.py` does not scan `plugins/`
   (`tests/CMakeLists.txt:556-562`) — the enforcing test and its fixtures belong
   under `tests/`.

6. **The proof runs against the *static* `libarbc`, and the doc says so.**
   `arbc_finalize_library()` calls `add_library(arbc "version.cpp")` with no
   `STATIC`/`SHARED` (`cmake/ArbcComponent.cmake:133`), `BUILD_SHARED_LIBS` is set
   by no preset, and the global `-fvisibility=hidden` (`CMakeLists.txt:16`) with
   **no `ARBC_API` export macro anywhere** means a `SHARED` `libarbc` would export
   nothing at all today. So every plugin in the tree — the shipped ones included —
   links the static archive and carries a private copy of the core objects it
   references. This task therefore proves the entry point, the factory, the
   facets, the service injection, and pixel equivalence cross the boundary; it does
   **not** prove a plugin resolves core symbols from the host image.
   *Rejected: adding the export macro here.* Annotating the public surface of
   fifteen components is multi-day, whole-library, packaging-shaped work — it is
   not a 1d kinds task, and doing it badly (a half-annotated surface) is worse
   than not doing it. It is registered as `packaging.shared_library_build`.
   What this task **does** owe is honesty: the doc-17 delta below writes the limit
   into the constitution rather than leaving doc 17:13 ("`libarbc` (shared and
   static)") to imply a shared build that no lane has ever produced.

7. **Design-doc delta (doc 16 same-commit rule).**
   - `docs/design/17-internal-components.md` §"Why object libraries specifically":
     **bullet 1 rewritten.** It claimed "Built-in kinds register via static
     registrars", which is not how the implementation works and never was — there
     is no `Registry::add` for any built-in kind anywhere in `src/`
     (`plugin_host.hpp:37-39` names the hole: "The built-in-kind → `Registry`
     bootstrap is the L6 umbrella `arbc` target's concern … NOT the loader's").
     What exists instead is `runtime`'s explicit `builtin_codecs()` table
     (`src/runtime/document_serialize.cpp:138-143`) and
     `register_builtin_operator_binders()` (`src/runtime/operator_binding.cpp:45`).
     The *conclusion* of the bullet (object libraries, so no archive member is
     silently dropped) survives intact; only its stated mechanism was stale, and
     leaving it stale would mislead this task's implementer about which path a
     built-in kind actually takes — the exact question the task exists to answer.
   - Same section, **bullet 3 extended** with the concrete dual-build shape
     (per-kind CI-only `MODULE`, loaded through `PluginHost`, host-side service
     injection for the three kinds the `Registry` factory cannot fully construct)
     and the static-`libarbc` honesty limit of Decision 6.
   - `docs/design/00-overview.md` §"Resolved questions", internal-components
     bullet: one clause recording that the dual-build (and every shipped plugin)
     links the static `libarbc` and that the shared link is deferred. It is
     project-shaping — it determines whether a third-party plugin can be built
     against an *installed* `libarbc` — so it belongs in the decision record.

8. **No new golden files; an equivalence instead.** Acceptance §3 asserts
   plugin-rendered pixels equal in-lib-rendered pixels byte-for-byte, rather than
   checking both against a new golden blob. The absolute bytes are already pinned
   by each kind's own goldens (`src/kind_raster/t/raster_goldens.t.cpp`,
   `tests/nested_goldens.t.cpp`, …); a second copy would have to be regenerated
   every time a kernel legitimately changes, for no added signal. The equivalence
   is strictly stronger for *this* task's question — it fails exactly when the
   boundary, not the kernel, is at fault. Byte-exact either way; no tolerance
   (doc 16).

## Open questions

(none — all decided.)

## Status

**Done** — 2026-07-11.

- Six CI-only `MODULE` targets (`arbc-ci-plugin-{solid,tone,raster,nested,fade,crossfade}`) added in `tests/CMakeLists.txt`, each output into `tests/ci_plugins/` via `LIBRARY_OUTPUT_DIRECTORY` so the `scan_plugin_path()` sweep finds exactly six.
- Plugin translation units: `tests/ci_plugins/ci_kinds.hpp` (shared config helpers), `tests/ci_plugins/{solid,tone,raster,nested,fade,crossfade}_ci_plugin.cpp` — each exposes `arbc_plugin_register(Registry&)` and registers exactly one kind id.
- Cross-component driver `tests/dual_build.t.cpp`: 7 Catch2 cases, 4739 assertions covering sole-registration, metadata, errors-as-values, 7-plugin aggregation, 6-image `ARBC_PLUGIN_PATH` scan, byte-exact plugin-vs-in-lib render equivalence, host service injection + `attach`/`detach` across the boundary, `audio()`/`editable()` facet survival, and the full `arbc-testing` conformance suite over each plugin-obtained factory.
- New claim `17-internal-components#in-lib-kinds-dual-build-through-plugin-entry` registered in `tests/claims/registry.tsv`; re-asserted `03-layer-plugin-interface#{plugin-registers-through-extern-c-entry,registry-resolves-kind-id-to-factory-and-metadata,plugin-path-scan-is-opt-in,loader-errors-are-values}`.
- Design-doc deltas (`docs/design/17-internal-components.md`, `docs/design/00-overview.md`) were applied in-tree by the refinement_writer; they record the static-`libarbc` honesty limit (Decision 6) and correct the stale "static registrar" mechanism text (Decision 7).
- Three letter-deviations from Acceptance criteria, each required by the criteria's own Acceptance 6: raster runs conformance suite with `snapshot_sensitive=false`; crossfade uses a `BoundedAudioLeaf` input (not a bare Solid/Tone); post-detach render asserted via `attached()` not by calling `render()` (which would fire a debug assert).
- Deferred follow-ups registered: `packaging.shared_library_build` and `runtime.plugin_operator_registration` (see WBS).
