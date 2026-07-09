# plugin_loading — dlopen plugin loading (the production host-side loader)

## TaskJuggler entry

Back-link: [`tasks/65-runtime.tji:34-39`](../../65-runtime.tji), under the
`runtime` umbrella (which itself `depends compositor.tile_planning`,
`tasks/65-runtime.tji:6`):

> ```
> task plugin_loading "dlopen plugin loading" {
>   effort 2d
>   allocate team
>   depends kinds.imageseq_plugin
>   note "extern C registration entry point, explicit host registration first, opt-in ARBC_PLUGIN_PATH scan; errors as values across the boundary. Docs 03/10."
> }
> ```

This task feeds milestone **M8 — "Documents as files"**
(`m8_persistence`, `tasks/99-milestones.tji:63-65`), whose note promises
"plugins load by path." M8's other dependencies —
`serialize.format_tests`, `runtime.document_serialize`,
`runtime.operator_codecs` (all DONE) and `model.workspace_backing` — mean
`plugin_loading` is one of the last two leaves M8 waits on. The closer lands
`complete 100` after the `allocate team` line here, appends
`Refinement: tasks/refinements/runtime/plugin_loading.md` to the note, and —
if `model.workspace_backing` is already complete — adds `complete 100` to
`m8_persistence` too (milestones don't infer completion,
`tasks/refinements/README.md:69-72`).

## Effort estimate

**2 days** (`tasks/65-runtime.tji:35`).

The load-bearing seam is **already built and proven end-to-end**, so this is
not a from-scratch effort:

- `arbc::Registry` (id → factory + metadata) exists at
  `src/contract/arbc/contract/registry.hpp:51`, with `add` / `factory` /
  `metadata` / `ids` / `size` (`:57-70`) and value-typed `RegistryError`
  (`EmptyId`, `DuplicateId`, `:18-21`).
- The `extern "C"` boundary symbol
  `arbc_plugin_register(arbc::Registry&)` exists at
  `src/contract/arbc/contract/plugin.hpp:20`, with the
  `ARBC_PLUGIN_EXPORT` visibility macro (`:8-12`).
- The whole `dlopen → dlsym → register → factory → render → dlclose` path
  is already exercised — by hand, in one test — at
  `tests/imageseq_plugin_path.t.cpp:32-81` (link `${CMAKE_DL_LIBS}`,
  `tests/CMakeLists.txt:557`).

What is **genuinely new** is the *production loader* that generalizes that
hand-rolled test into a reusable, host-facing API:

1. A `PluginHost` type in `src/runtime/` that owns a `Registry` plus the
   `dlopen` handles, with the correct teardown ordering (handles outlive the
   registry and every factory derived from them).
2. `load_plugin(path)` — explicit, by-path load: `dlopen` + resolve
   `arbc_plugin_register` + invoke against the registry, every failure a
   value.
3. `scan_plugin_path()` — the **opt-in** `ARBC_PLUGIN_PATH` directory scan,
   a no-op (zero filesystem access) when the variable is unset.
4. A `PluginLoadError` value type for the loader's own failure modes
   (cannot-open, missing-entry-point) alongside the bubbled `RegistryError`.
5. Linking `${CMAKE_DL_LIBS}` into the `runtime` component
   (`src/runtime/CMakeLists.txt`) — the first in-lib use of the platform
   loader.

**No new `arbc_*` levelization edge.** The loader lives in `runtime` (L5),
which already lists `contract` in its allowed direct deps
(`scripts/check_levels.py:35-40`); `dlopen`/`libdl` is a platform facility,
not an `arbc` component, so `check_levels.py` (which only tracks
`#include <arbc/<component>/…>`, `:47`) is unaffected.

## Inherited dependencies

**Settled:**

- **`kinds.imageseq_plugin`** (DONE 2026-07-07, `tasks/55-kinds.tji`, this
  task's declared `depends` edge). It landed exactly the seam this task
  builds on and deliberately stopped short of the loader — Constraint 2 of
  `tasks/refinements/kinds/imageseq_plugin.md` states it does "**not** build
  the `ARBC_PLUGIN_PATH` scanner or host registration API; those are
  `runtime.plugin_loading`'s (M8)." Concretely it produced:
  `src/contract/arbc/contract/registry.hpp` (the `Registry`),
  `src/contract/arbc/contract/plugin.hpp` (the `extern "C"` symbol),
  `arbc-plugin-imageseq` (the permanent out-of-lib `.so` under
  `plugins/imageseq/`, registering `org.arbc.imageseq` at
  `plugins/imageseq/imageseq_plugin.cpp:9-14`), and the end-to-end
  `dlopen` test `tests/imageseq_plugin_path.t.cpp`. It also landed claim
  `03-layer-plugin-interface#plugin-registers-through-extern-c-entry`
  (`tests/imageseq_plugin_path.t.cpp:30`), which this task re-enforces
  through the *production* loader.
- **`arbc::expected<T,E>` / `arbc::unexpected<E>`**
  (`src/base/arbc/base/expected.hpp:13,29`) — the value-or-error vehicle the
  loader returns everything through. In-house, not `std::expected`.
- The `runtime` component (L5) and its CMake shape
  (`src/runtime/CMakeLists.txt`) are established and stable; this task adds a
  source pair + a `${CMAKE_DL_LIBS}` link line to it.

**Pending:** none — every predecessor is landed. `model.workspace_backing`
(M8 sibling) is unrelated to the loader seam and does not gate this work.

## What this task is

Build the **production host-side plugin loader** the design docs promise:
turn the proven-by-hand `dlopen`/`extern "C"` path into a reusable,
value-erroring API that an embedder or an application host uses to bring
out-of-lib content kinds (starting with `org.arbc.imageseq`) into a
`Registry` the document-load path can resolve against.

Concretely:

- **(a) `PluginHost`** — a `src/runtime/arbc/runtime/plugin_host.hpp` type
  that owns a `Registry` (exposed as `registry()` / `const Registry&`) and a
  private list of RAII `dlopen`-handle wrappers. Member declaration order
  guarantees the registry (and all plugin-derived factories) is destroyed
  **before** the handles are `dlclose`d — the exact ordering
  `tests/imageseq_plugin_path.t.cpp:79-81` documents by hand.
- **(b) `load_plugin(path)`** — explicit, by-path load. `dlopen(path,
  RTLD_NOW | RTLD_LOCAL)`, `dlsym("arbc_plugin_register")`, invoke it with
  `registry()`. Returns `expected<std::monostate, PluginLoadError>`. A path
  that cannot be opened → `PluginLoadError::CannotOpen`; a library missing
  the entry-point symbol → `PluginLoadError::MissingEntryPoint`; a kind id
  the plugin tries to re-register → the bubbled `RegistryError::DuplicateId`.
  Never throws, never aborts.
- **(c) `scan_plugin_path()`** — the **opt-in** directory scan. Reads
  `ARBC_PLUGIN_PATH` (platform path-separator-delimited directory list). If
  the variable is unset or empty, the scan does **nothing** and touches the
  filesystem zero times. If set, for each directory it enumerates entries
  with the platform shared-library extension, **sorted lexicographically**
  (deterministic load order), and attempts to load each. During a *scan* a
  library that opens but lacks `arbc_plugin_register` is **skipped, not an
  error** (a plugin directory may hold support libraries); a
  `DuplicateId` collision is collected as a per-entry value and the earlier
  registration is left intact. Returns a small `PluginScanReport`
  (loaded-count + per-entry outcomes) so a host can log what happened.
- **(d) `PluginLoadError`** — a value enum for the loader's own failure
  modes (`CannotOpen`, `MissingEntryPoint`), carrying the `dlerror()`-style
  diagnostic string so a host can surface *why* a load failed without
  parsing.
- **(e) CMake wiring** — add `plugin_host.cpp` / `plugin_host.hpp` to the
  `arbc_add_component(NAME runtime …)` in `src/runtime/CMakeLists.txt`, and
  link `${CMAKE_DL_LIBS}` (POSIX `libdl`; empty on platforms where the
  loader is in libc). Mirrors the private-link idiom already used there for
  `nlohmann_json`.

The **ordering contract — "explicit host registration first"** — is the
policy `PluginHost` encodes: the host populates `registry()` (its own
`add(...)` calls and any `load_plugin(path)` it chooses) **before** invoking
`scan_plugin_path()`; the scan is additive and cannot clobber an
already-registered id (a colliding scanned kind is a `DuplicateId` value,
so **explicit registration wins**). This is doc 10:49-52's "explicit host
registration first … plus an opt-in directory scan" realized on the
existing `DuplicateId` mechanism.

**Not this task:**

- **Built-in-kind → `Registry` bootstrap.** Today the built-in kinds
  (`org.arbc.solid`, `tone`, `fade`, `crossfade`, `nested`) populate the
  serialize `CodecTable` / `KindBridge`
  (`src/runtime/document_serialize.cpp:113-120`), **not** any `Registry`.
  Wiring built-ins into a `Registry` is the **L6 umbrella `arbc` target's**
  concern per doc 17:61 ("registry bootstrap; built-in kind registration"),
  not the L5 loader's. The loader operates on whatever `Registry` the host
  hands it and is agnostic to what is already in it.
- **The `arbc_add_plugin()` CMake helper** — deferred to
  `packaging.plugin_helper` (M9, `tasks/75-packaging.tji`); plugins stay
  hand-rolled `add_library(… MODULE …)` for now.
- **Windows `LoadLibrary` backing** — POSIX `dlfcn` is the v1 deliverable;
  see Decision 4 and the deferred `runtime.plugin_loading_win32` leaf.
- **Platform-conventional default search directories** — `ARBC_PLUGIN_PATH`
  is the v1 scan; the install-relative / XDG default locations are deferred
  to `runtime.plugin_default_search_paths` (needs the M9 install layout).
- **Out-of-process plugin isolation** — explicitly deferred project-wide
  (doc 00:173-174, doc 03:182-186).

## Why it needs to be done

M8's headline promise is that documents are files you can save and reopen —
including files that reference **out-of-lib kinds**. `org.arbc.imageseq`
ships as a separate `.so` precisely so codecs never enter an embedder's link
line (the "codec line", doc 17:150-159); without a production loader, the
only way to bring that kind in is the by-hand `dlopen` dance duplicated in
one test. The loader is the seam that lets a host — an application or a
third-party embedder — turn `ARBC_PLUGIN_PATH` (or an explicit path) into
registered, constructible kinds, closing the loop between "a document names
`org.arbc.imageseq`" and "the runtime can build it." It is the last runtime
piece M8 needs beyond `model.workspace_backing`.

## Inputs / context

### Design docs (normative, doc 16)

- **doc 03 — Layer interface & plugin strategy**
  (`docs/design/03-layer-plugin-interface.md`):
  - §Plugin mechanism, `03:164-171` — the Stage-1 (v1) regime: "link-time or
    `dlopen` with a single `extern "C" arbc_plugin_register(Registry&)`
    entry point." Boundary shaping `03:176-180`: "no exceptions across the
    boundary (errors are values)."
  - §Registry, `03:188-207` — id → factory + metadata; and the load-time
    lifecycle: "A `Registry` is populated during single-threaded startup or
    plugin load and is read-only for the remainder of a session; concurrent
    host-side discovery and loading is `runtime`'s concern (doc 17), not the
    registry seam's" (`03:204-207`). This task is that runtime concern.
  - `03:224-231` — `org.arbc.imageseq` "is … the permanent, out-of-lib
    exercise of the `extern "C" arbc_plugin_register(Registry&)` path"; the
    loader's primary real-world fixture.
- **doc 10 — Tooling & packaging** (`docs/design/10-tooling-and-packaging.md`):
  - `10:49-52` — the discovery policy this task implements verbatim:
    "Plugin discovery at runtime: explicit host registration first
    (embedders usually want control), plus an opt-in directory scan
    (`ARBC_PLUGIN_PATH` + platform-conventional locations) for
    application-style hosts."
  - `10:16-18` — "No exceptions across public API and plugin boundaries
    (doc 03) … Public errors are values (`expected`-style result)."
  - `10:19-34` — dependency policy: `libdl`/`dlopen` is not an enumerated
    third-party dependency; it is a platform loader facility ("dual-built as
    `dlopen` plugins in CI", `10:59`), so no doc-10 dependency-table delta is
    needed.
- **doc 00 — Overview**, `00:102-104` — the resolved **Plugin ABI**
  decision: "two stages — C++ interface + `dlopen` registration for v1 …
  Decided in doc 03." This task is the v1 loader; the Stage-2 stable C ABI
  is future.
- **doc 17 — Internal components** (`docs/design/17-internal-components.md`):
  - `17:60` — `arbc::runtime` (L5) owns "`dlopen` loading"; the loader
    belongs here.
  - `17:61` — the L6 umbrella `arbc` target owns "registry bootstrap;
    built-in kind registration" (the not-this-task boundary above).
  - `17:150-159` — the codec line: why `imageseq` ships as a separate
    `MODULE` artifact, and the permanent end-to-end test this loader's
    integration test extends.

### Source seams

- `src/contract/arbc/contract/registry.hpp:51-81` — `Registry`; note the
  in-code deferral pointer at `:46-50` naming *this* task as the owner of
  "explicit host registration API, opt-in `ARBC_PLUGIN_PATH` directory scan,
  error plumbing across the boundary."
- `src/contract/arbc/contract/plugin.hpp:8-20` — the `extern "C"` symbol +
  export macro the loader resolves.
- `tests/imageseq_plugin_path.t.cpp:20,32-81` — the hand-rolled reference
  path (`<dlfcn.h>`, `dlopen`/`dlsym`/`dlclose`, the destroy-order comment)
  the production loader generalizes; and `tests/CMakeLists.txt:555-562` — the
  `${CMAKE_DL_LIBS}` link + `$<TARGET_FILE:arbc-plugin-imageseq>` /
  `ARBC_IMAGESEQ_FIXTURE_DIR` compile-def wiring the new integration test
  mirrors.
- `src/runtime/CMakeLists.txt` — the `arbc_add_component(NAME runtime …)` to
  extend (sources, public headers) and the `target_link_libraries(… PRIVATE
  …)` idiom to copy for `${CMAKE_DL_LIBS}`.
- `src/base/arbc/base/expected.hpp:13,29` — `expected` / `unexpected`.
- `scripts/check_levels.py:17-42` — the `ALLOWED` map; `runtime`'s block at
  `:35-40` already contains `contract`, so no allow-list edit is needed.

### Tests / claims

- Claims register `tests/claims/registry.tsv` — id format `<doc-stem>#<slug>`
  (`:2-3`); each id needs a live `// enforces: <id>` test or CI fails.
  Existing relevant row:
  `03-layer-plugin-interface#plugin-registers-through-extern-c-entry`
  (enforced at `tests/imageseq_plugin_path.t.cpp:30`), which this task
  re-enforces (second `// enforces:` tag, no new row).

### Predecessor / sibling refinements

- `tasks/refinements/kinds/imageseq_plugin.md` — the predecessor; its
  Decision 1 / Constraint 2 draw the exact line between the seam (its scope)
  and the loader (this task), and its closer note requested the
  `depends kinds.imageseq_plugin` edge that is now in place.
- `tasks/refinements/runtime/{operator_codecs,document_serialize,threading}.md`
  — sibling style for section shape, claims-register citation, the
  "no new `arbc_*` levelization edge" argument, and closer-ready deferred-leaf
  registration.

## Constraints / requirements

1. **Errors are values across the boundary — never exceptions, never
   abort.** Every loader entry point returns through `arbc::expected`; a
   missing file, an un-`dlopen`-able library, a library without the
   entry-point symbol, and a `DuplicateId` are all values (doc 03:176-180,
   doc 10:16-18). `dlerror()` is captured into the `PluginLoadError`
   diagnostic string, not propagated as state. The integration test asserts
   `REQUIRE_NOTHROW` around every failing call.
2. **The opt-in scan is genuinely opt-in.** With `ARBC_PLUGIN_PATH` unset or
   empty, `scan_plugin_path()` performs **zero** `dlopen` attempts and zero
   filesystem access — pinned by a behavioral counter (loaded-count == 0,
   registry `size()` unchanged), never a wall-clock assertion (doc 16
   performance-shape rule).
3. **Explicit registration wins.** The scan is additive; a scanned kind id
   already present is a `DuplicateId` outcome that leaves the earlier
   registration intact. `PluginHost` documents (and tests) that the host
   registers explicitly *before* scanning.
4. **Handle lifetime outlives every derived factory.** `dlclose` must not
   run while a factory or a `Content` produced from a plugin is still live
   (the code backing them would be unmapped). `PluginHost` enforces this by
   member ordering — handles destroyed **after** the registry — mirroring
   `tests/imageseq_plugin_path.t.cpp:79-81`. Verified under the CI
   sanitizer (ASan/UBSan) lane.
5. **Deterministic scan order.** Directory entries load sorted
   lexicographically, so the sequence of registrations — and thus any
   `DuplicateId` outcome — is reproducible across runs and filesystems.
6. **Levelization: no new edge, no JSON below L5.** The loader lives in
   `runtime` (L5) and depends only on the L3 `contract::Registry` (already
   allowed, `scripts/check_levels.py:39`). `dlopen`/`libdl` is a platform
   facility, invisible to `check_levels.py`. The public `plugin_host.hpp`
   names no `nlohmann::json` type. `scripts/check_levels.py` stays green.
7. **POSIX `dlfcn` v1; Windows behind the existing guard.** The loader uses
   `<dlfcn.h>`. The one platform branch (`#if defined(_WIN32)`) mirrors the
   guard already in `plugin.hpp:8-12`; the Windows `LoadLibrary` backing is
   not built here (Decision 4).

## Acceptance criteria

- **Production loader loads `org.arbc.imageseq` end-to-end (integration).**
  A new `tests/plugin_loading.t.cpp` — linking `arbc` +
  `${CMAKE_DL_LIBS}`, with compile defs
  `ARBC_IMAGESEQ_PLUGIN_FILE="$<TARGET_FILE:arbc-plugin-imageseq>"`,
  `ARBC_IMAGESEQ_PLUGIN_DIR="$<TARGET_FILE_DIR:arbc-plugin-imageseq>"`, and
  `ARBC_IMAGESEQ_FIXTURE_DIR` (mirroring `tests/CMakeLists.txt:555-562`) —
  builds a `PluginHost`, calls `load_plugin(ARBC_IMAGESEQ_PLUGIN_FILE)`,
  and asserts the registry now yields the `org.arbc.imageseq` factory, which
  constructs a `Timed` content over the fixtures and renders a frame across
  the boundary. **Re-enforces** claim
  `03-layer-plugin-interface#plugin-registers-through-extern-c-entry`
  (second `// enforces:` tag on the existing row — no new row).
- **Opt-in scan behaves (integration + behavioral counter).** In
  `tests/plugin_loading.t.cpp`: with `ARBC_PLUGIN_PATH` unset,
  `scan_plugin_path()` leaves `registry().size()` unchanged and reports
  zero load attempts; with it `setenv`-set to `ARBC_IMAGESEQ_PLUGIN_DIR`,
  the scan loads the plugin and `org.arbc.imageseq` becomes available.
  **Lands** claim `03-layer-plugin-interface#plugin-path-scan-is-opt-in`
  (new `registry.tsv` row + `// enforces:` tag).
- **Explicit registration precedes and beats the scan.** Load
  `org.arbc.imageseq` explicitly (or `add` a colliding stub), then scan its
  directory; assert the collision surfaces as a `DuplicateId` per-entry
  outcome and the original factory is untouched. **Lands** claim
  `03-layer-plugin-interface#explicit-host-registration-precedes-scan`.
- **Every failure is a value (integration + unit).** `load_plugin` on a
  nonexistent path → `PluginLoadError::CannotOpen`; on a shared library
  lacking the symbol → `PluginLoadError::MissingEntryPoint`; a duplicate id
  → `RegistryError::DuplicateId`; all wrapped in `REQUIRE_NOTHROW`.
  **Lands** claim `03-layer-plugin-interface#loader-errors-are-values`.
- **Value-level unit coverage in-component.** A
  `src/runtime/t/plugin_host.t.cpp` component test exercises the value-level
  API without a real plugin — `CannotOpen` on a bogus path, the empty-`PATH`
  no-op, `PluginLoadError` diagnostic strings — keeping unit coverage inside
  the `runtime` component's test target
  (`arbc_component_test(COMPONENT runtime …)`).
- **Handle-lifetime is sanitizer-clean.** The integration test uses a
  plugin factory, then destroys the `PluginHost`, and runs clean under the
  CI ASan/UBSan lane (Constraint 4).
- **Deferred — two new WBS leaves.** `runtime.plugin_loading_win32` and
  `runtime.plugin_default_search_paths` are named in Decisions 4–5 for the
  closer to register (both M9).
- **Coverage / build / WBS gate.** ≥90 % diff coverage on changed lines;
  `-Werror -Wpedantic` clean; `scripts/check_levels.py` green (no new
  `arbc_*` edge); three new `registry.tsv` rows each backed by a live
  `enforces:` test; and after the closer lands `complete 100` + the
  refinement back-link, `tj3 project.tjp 2>&1 | grep -iE "error|warning"` is
  silent (`tasks/refinements/README.md:57-68`).

## Decisions

1. **`PluginHost` owns the `Registry` and the `dlopen` handles together;
   the loader is not a set of free functions.** Bundling registry + handles
   in one object makes the destroy-order contract (Constraint 4) a property
   of member declaration order rather than a burden the embedder must
   re-derive at every call site. It serves both audiences doc 10:49-52 names
   — embedders drive `registry()` + `load_plugin()` explicitly;
   application hosts call `scan_plugin_path()`.
   *Rejected: free `load_plugin(Registry&, path) → PluginHandle` functions
   returning handles the caller must keep alive* — pushes the
   handle-outlives-factory ordering onto every embedder, exactly the footgun
   `tests/imageseq_plugin_path.t.cpp:79-81` had to annotate by hand.
   *Rejected: the loader `dlclose`s eagerly after `arbc_plugin_register`
   returns* — factories and `Content` capture code in the plugin's mapped
   image; closing early is a use-after-unmap. Handles must live for the
   host's session (doc 03:204-207, "read-only for the remainder of a
   session").
2. **The scan is additive and `DuplicateId`-guarded; explicit registration
   wins.** "Explicit host registration first" (doc 10:49-52) is realized on
   the `Registry`'s existing `DuplicateId` return — no new precedence
   mechanism. A scanned kind colliding with a host-registered one is a
   collected value, not a silent override.
   *Rejected: last-writer-wins / scan overrides* — contradicts "embedders
   usually want control" (doc 10:49) and would let a stray plugin directory
   shadow a host's deliberate choice.
3. **During a *scan*, a library missing the entry-point symbol is skipped,
   not an error; during an *explicit* `load_plugin(path)` it is
   `MissingEntryPoint`.** A plugin directory may legitimately contain
   support/`.so` dependencies; failing the whole scan on the first non-plugin
   file would make `ARBC_PLUGIN_PATH` unusable. An explicit by-path load, by
   contrast, is the host asserting "this *is* a plugin," so a missing symbol
   there is a real error worth surfacing.
   *Rejected: symbol-missing is always an error* — breaks realistic plugin
   directories. *Rejected: symbol-missing is always a skip* — hides a
   genuine mistake when the host named a specific file.
4. **POSIX `dlfcn` is the v1 backing; Windows `LoadLibrary` is a deferred
   leaf.** The entire existing plugin path is POSIX `dlfcn`
   (`tests/imageseq_plugin_path.t.cpp:20`), CI builds POSIX, and the project
   already tracks Windows as a distinct M9 lane (`pool.*_win32`). The loader
   keeps the one `#if defined(_WIN32)` seam consistent with
   `plugin.hpp:8-12` so a Windows backing slots in cleanly.
   **Deferred WBS leaf** (closer registers): id
   **`runtime.plugin_loading_win32`**, effort **1d**, `allocate team`,
   `depends !plugin_loading`, milestone **M9** (`m9_release`,
   `tasks/99-milestones.tji:67`, alongside the sibling `pool.*_win32`
   Windows tasks), note: "Windows `LoadLibrary`/`GetProcAddress`/
   `FreeLibrary` backing for the runtime plugin loader behind the existing
   `_WIN32` seam, plus `;` `ARBC_PLUGIN_PATH` separator; mirrors POSIX
   `dlfcn` behavior. Source: tasks/refinements/runtime/plugin_loading.md
   Decision 4."
   *Rejected: build the Windows backing now* — no Windows CI to prove it
   against; would ship untested code, and the repo already sequences Windows
   parity into M9.
5. **`ARBC_PLUGIN_PATH` is the v1 scan; platform-conventional default
   directories are a deferred leaf.** doc 10:51 names "`ARBC_PLUGIN_PATH` +
   platform-conventional locations," but the default install-relative / XDG
   directories depend on the M9 install layout (`packaging.release_01`),
   which is not yet fixed. M8's promise ("plugins load by path") is fully met
   by explicit load + `ARBC_PLUGIN_PATH`.
   **Deferred WBS leaf** (closer registers): id
   **`runtime.plugin_default_search_paths`**, effort **1d**, `allocate
   team`, `depends !plugin_loading, packaging.release_01`, milestone **M9**,
   note: "Add platform-conventional default plugin directories
   (install-relative libdir + XDG/`AppData` data dirs) to the opt-in scan
   after `ARBC_PLUGIN_PATH`; ordering and dedup against explicitly-listed
   dirs. Source: tasks/refinements/runtime/plugin_loading.md Decision 5."
   *Rejected: hard-code default paths now* — the install layout is
   undecided; guessing it here would be reworked at packaging time.
6. **No doc-00 decision-record bullet, no design-doc delta.** Every behavior
   here — the `dlopen` regime, errors-as-values, explicit-first + opt-in
   scan — is already normative in docs 03/10 (and doc 00:102-104 already
   records the Plugin ABI decision). This task implements settled policy; it
   does not amend it.

## Open questions

(none — all decided.) Two adjacent items are surfaced to the orchestrator's
return summary rather than encoded as WBS work: (a) whether a WBS home
exists for the L6 umbrella's built-in-kind → `Registry` bootstrap (doc
17:61) — explicitly out of scope here; and (b) confirmation that no Windows
CI runner exists today, which is the premise of Decision 4's deferral. Both
are human/project-state checks, not implementable leaves.

## Status

**Done** — 2026-07-09.

- Created `src/runtime/arbc/runtime/plugin_host.hpp` — `PluginHost` class owning `Registry` + RAII `dlopen` handles, with member-order destroy contract.
- Created `src/runtime/plugin_host.cpp` — `load_plugin(path)`, `scan_plugin_path()`, `PluginLoadError`, `PluginScanReport` implementations.
- Created `src/runtime/t/plugin_host.t.cpp` — unit tests: `CannotOpen`, opt-in no-op, nonexistent-dir, duplicate-add value cases.
- Created `tests/plugin_loading.t.cpp` — integration tests: end-to-end load+render, opt-in scan with/without `ARBC_PLUGIN_PATH`, explicit-precedes-scan, all failures as values (64 assertions).
- Created `tests/fixtures/noentry_plugin.cpp` — shared library fixture missing the entry-point symbol (for `MissingEntryPoint` test).
- Edited `src/runtime/CMakeLists.txt` — added `plugin_host.cpp` / `plugin_host.hpp` sources, `${CMAKE_DL_LIBS}` private link, component test.
- Edited `tests/CMakeLists.txt` — added `noentry_plugin` MODULE fixture and `arbc_plugin_loading_t` integration test target.
- Edited `tests/claims/registry.tsv` — 3 new rows: `03-layer-plugin-interface#plugin-path-scan-is-opt-in`, `#explicit-host-registration-precedes-scan`, `#loader-errors-are-values`; `#plugin-registers-through-extern-c-entry` re-enforced (second tag, no new row).
- Deferred WBS leaves registered by closer: `runtime.plugin_loading_win32` (1d, M9) and `runtime.plugin_default_search_paths` (1d, M9).
