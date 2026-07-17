# runtime.plugin_default_search_paths — Platform-conventional default plugin directories

## TaskJuggler entry

[`tasks/65-runtime.tji:56-61`](../../65-runtime.tji):

```
task plugin_default_search_paths "Platform-conventional default plugin directories" {
  effort 1d
  allocate team
  depends !plugin_loading, packaging.release_01
  note "Add platform-conventional default plugin directories (install-relative libdir + XDG/AppData data dirs) to the opt-in scan after ARBC_PLUGIN_PATH; ordering and dedup against explicitly-listed dirs. Source: tasks/refinements/runtime/plugin_loading.md Decision 5."
}
```

## Effort estimate

1d, decomposed:

- ~0.35d — `default_plugin_directories()` resolver (env reads, XDG/`AppData`
  fallback expansion, image-relative dir via `dladdr`/`GetModuleFileNameA`,
  compiled-in libdir constant) + the combined `scan_standard_paths()` entry
  point over a factored-out per-directory scan helper.
- ~0.45d — tests: unit coverage of the resolver's ordering/fallbacks under
  controlled env, cross-component precedence/dedup tests over fixture
  plugins, shared-lane consumer test proving env-var-free discovery from a
  staged install; two new claims-register rows + `enforces:` tags.
- ~0.2d — build plumbing (private compile definition for the configured
  libdir), Windows-lane parity, gate/coverage/tj3 ritual.

## Inherited dependencies

**Settled (formal `depends`):**

- `runtime.plugin_loading` — **Done.** `PluginHost` with `load_plugin()` +
  opt-in `scan_plugin_path()`, errors as values, additive
  `DuplicateId`-guarded scan. This task extends that seam; it changes none
  of its behavior. Refinement:
  [`plugin_loading.md`](plugin_loading.md) (Decision 5 is this task's
  charter).
- `packaging.release_01` — **Done 2026-07-16.** The M9 install layout this
  task was explicitly deferred to wait for
  ([`plugin_loading.md`](plugin_loading.md) Decision 5: "the default
  install-relative / XDG directories depend on the M9 install layout") is
  now fixed: plugins install to `${CMAKE_INSTALL_LIBDIR}/arbc/plugins`
  (`cmake/ArbcInstall.cmake:168`, per
  [`packaging/install.md`](../packaging/install.md) D6).

**Settled (informal, decision continuity):**

- `runtime.plugin_loading_win32` — **Done.** Single `#if defined(_WIN32)`
  seam in `plugin_host.cpp`, platform-neutral orchestration, `;` vs `:`
  separator, `.dll` vs `.so` suffix
  ([`plugin_loading_win32.md`](plugin_loading_win32.md) Decision 1,
  Constraint 5). The default-directory resolver joins that seam; it must
  not fork it.
- `packaging.plugin_helper` / `packaging.shared_library_build` — plugin
  naming (`lib`-prefixed `arbc-plugin-*` modules), `arbc_add_plugin()`
  stays build-side only ("must not grow discovery logic",
  [`packaging/plugin_helper.md`](../packaging/plugin_helper.md)), shared
  lane exists with staged consumer installs.

**Pending:** (none.)

**Downstream:** `packaging.tag_01` lists this task in its `depends`
(`tasks/75-packaging.tji:42`), as does milestone M9
(`tasks/99-milestones.tji:72`). Per
[`packaging/release_01.md`](../packaging/release_01.md), this is the last
runtime leaf the 0.1 tag ritual waits on from this stream.

## What this task is

Give application-style hosts environment-variable-free plugin discovery:
a new opt-in scan entry point that walks `ARBC_PLUGIN_PATH` directories
first (exactly as today) and then a pinned, platform-conventional set of
default directories — the per-user data dir (`$XDG_DATA_HOME` /
`%LOCALAPPDATA%`), the system data dirs (`$XDG_DATA_DIRS`), and the
install-relative libdir — each suffixed `arbc/plugins` to match the
shipped install layout, with each directory scanned at most once (dedup
against explicitly-listed dirs) and all existing precedence semantics
(explicit registration first, additive `DuplicateId`-guarded scan)
unchanged. The existing `scan_plugin_path()` keeps its exact contract.

**Not this task:**

- macOS conventions (`.dylib` suffix, `~/Library/Application Support`) —
  the loader has no macOS seam at all today (`plugin_host.cpp:50-56` seams
  only `.dll`/`.so`); a macOS port is a platform-lane question, not a
  search-path question.
- Any change to where plugins *install* — `ArbcInstall.cmake:163-189` D6
  layout is settled and this task reads it, not rewrites it.
- Growing `arbc_add_plugin()` — discovery stays runtime policy
  ([`packaging/plugin_helper.md`](../packaging/plugin_helper.md)
  Constraint on build-side-only scope).
- An environment kill-switch for defaults — the host opts in or out by
  choosing which scan method to call (Decision 1).

## Why it needs to be done

doc 10:50-53 promises the full discovery story: "explicit host
registration first …, plus an opt-in directory scan (`ARBC_PLUGIN_PATH` +
platform-conventional locations) for application-style hosts." The
shipped loader implements only the env-var half;
[`plugin_loading.md`](plugin_loading.md) Decision 5 deferred the
platform-conventional half to this leaf because the install layout wasn't
fixed. It now is (`packaging.release_01` Done), and the deferral's own
terms — defaults after `ARBC_PLUGIN_PATH`, ordering and dedup against
explicitly-listed dirs, install-relative libdir + XDG/`AppData` data dirs
— are this task's scope. `packaging.tag_01` (the 0.1 release tag) and M9
block on it: an application host that installs the package today cannot
find the shipped plugins without hand-setting an environment variable,
which is exactly the gap doc 10's sentence exists to close.

## Inputs / context

**Design docs (normative, doc 16):**

- `docs/design/10-tooling-and-packaging.md:50-53` — the single normative
  sentence on discovery; ":52 "platform-conventional locations" is the
  envelope this task fills. `:19-35` — dependency policy (no new deps
  without a justified table row; constrains Decision 5).
- `docs/design/03-layer-plugin-interface.md:227-249` (plugin mechanism),
  `:253-311` (Registry; `:268-271` delegates discovery to `runtime`;
  `:303-304` explicit-registration-first precedence). Loader claims are
  minted under this doc's stem (registry rows below).
- `docs/design/17-internal-components.md:35`, `:71` — `arbc::runtime` (L5)
  owns dlopen loading; depends on everything below; CI-enforced
  (`scripts/check_levels.py`).
- `docs/design/16-sdlc-and-quality.md:14-25` + `CONTRIBUTING.md:127-146` —
  claims-register mechanics (`tests/claims/registry.tsv`, tab-separated
  `<claim-id>\t<description>`, `// enforces:` tag above the `TEST_CASE`).

**Source seams:**

- `src/runtime/arbc/runtime/plugin_host.hpp` — `PluginLoadError` `:51-61`,
  `PluginScanEntry`/`PluginScanReport` `:67-86`, `detail::PluginHandle`
  `:93-109`, `PluginHost` `:121-158` (`load_plugin` `:144`,
  `scan_plugin_path` `:153`; handle-before-registry member order
  `:156-157`).
- `src/runtime/plugin_host.cpp` — `k_entry_point` `:46`; platform
  constants `:50-56`; `has_suffix` `:84-87`; `open_and_resolve` `:101-147`;
  `load_plugin` `:151-181`; `scan_plugin_path` `:183-300`: `getenv` `:192`,
  unset/empty early-return `:196-198`, separator split dropping empty
  segments `:200-214`, per-directory loop `:216` (Win32 `FindFirstFileA`
  `:226-247`, POSIX `opendir` `:249-264` — missing dir is a silent skip),
  shared lexicographic `std::sort` `:269`, per-entry outcomes `:273-294`
  (`DuplicateId` observed as registry-size non-growth `:286-291`). The
  per-directory enumeration+load body is what Decision 1 factors into a
  reusable helper.
- `src/contract/arbc/contract/plugin.hpp:20` — the `extern "C"` entry
  point; `:8-12` the export-macro seam the win32 seam mirrors.

**Build / install:**

- `src/runtime/CMakeLists.txt:1-2` (component), `:17` (`plugin_host.cpp`),
  `:41` (public header), `:56` (`${CMAKE_DL_LIBS}` already linked — covers
  `dladdr`).
- `cmake/ArbcInstall.cmake:163-189` — `plugin_dest =
  "${CMAKE_INSTALL_LIBDIR}/arbc/plugins"` `:168`; both LIBRARY and RUNTIME
  destinations `:186-188`; `:176-182` documents that the consumer test
  installs via standalone `cmake --install --prefix` — i.e. the staged
  tree is *relocated* relative to the configure-time prefix (drives
  Decision 3).

**Tests / claims:**

- `src/runtime/t/plugin_host.t.cpp` — component tests (cases `:75`, `:88`,
  `:113`, `:124`; env save/restore helper `:54`).
- `tests/plugin_loading.t.cpp` — cross-component loader tests with
  `enforces:` tags `:108-109`, `:158`, `:194`, `:217`; no-entry fixture
  module wiring `tests/CMakeLists.txt:1020-1035`.
- `tests/consumer/plugin_scan.cpp` — installed-package scan proof;
  `ARBC_PLUGIN_PATH` set to the staged plugin dir at
  `tests/consumer/CMakeLists.txt:175` (dir computed `:91`).
- `tests/claims/registry.tsv:280-282` — the three scan claims
  (`03-layer-plugin-interface#plugin-path-scan-is-opt-in`,
  `#explicit-host-registration-precedes-scan`,
  `#loader-errors-are-values`); `:228`
  (`#plugin-registers-through-extern-c-entry`). Row `:280` pins
  "unset/empty ⇒ zero dlopen attempts and zero filesystem access" for
  `scan_plugin_path()` — the contract Decision 1 preserves.

**Predecessor refinements:** [`plugin_loading.md`](plugin_loading.md)
Decisions 2 (additive, `DuplicateId`-guarded, explicit wins), 3
(scan skips entry-point-less libs), 5 (this task's charter);
[`plugin_loading_win32.md`](plugin_loading_win32.md) Decisions 1 (single
seam) and 4 (enumeration leaves);
[`../packaging/install.md`](../packaging/install.md) D6 (the
`arbc/plugins` subdir chosen *inside* doc 10's envelope, no doc
amendment — the precedent Decision 6 follows).

## Constraints / requirements

1. **`scan_plugin_path()` is untouched.** Its contract — unset/empty
   `ARBC_PLUGIN_PATH` ⇒ zero dlopen attempts and zero filesystem access —
   is pinned by claim `03-layer-plugin-interface#plugin-path-scan-is-opt-in`
   (`registry.tsv:280`) and stays byte-for-byte. Defaults are reachable
   only through the new entry point (Decision 1); a host that never calls
   it never touches the default dirs.
2. **Ordering: env dirs first, then defaults, deterministically.** The
   combined scan walks `ARBC_PLUGIN_PATH` directories in listed order,
   then the default directories in the pinned order of Decision 2.
   Within each directory the existing shared lexicographic `std::sort`
   (`plugin_host.cpp:269`) remains the ordering authority. The full walk
   order is a pure function of environment + build constants — no
   filesystem-enumeration order leaks through.
3. **Dedup against explicitly-listed dirs.** Each directory is scanned at
   most once per combined scan: a default dir that already appeared in
   `ARBC_PLUGIN_PATH` (or earlier in the default list, e.g. a
   non-relocated install where the image-relative and compiled-in dirs
   coincide) is skipped. Dedup is by string comparison after
   trailing-separator trim (Decision 4); the registry's `DuplicateId`
   guard remains the semantic safety net for aliased paths dedup can't see.
4. **Precedence semantics unchanged.** The scan stays additive and
   `DuplicateId`-guarded ([`plugin_loading.md`](plugin_loading.md)
   Decision 2): explicit registration beats env dirs beats defaults, all
   realized by first-registration-wins — no new precedence mechanism.
   Entry-point-less libraries in default dirs are `SkippedNoEntry`, not
   errors (Decision 3 there). All failures remain values in the
   `PluginScanReport`; nothing throws.
5. **Missing/unset pieces degrade silently.** An unset `$XDG_DATA_HOME`
   falls back to `$HOME/.local/share` (skip if `$HOME` unset); unset
   `$XDG_DATA_DIRS` falls back to `/usr/local/share:/usr/share` (XDG Base
   Directory spec); unset `%LOCALAPPDATA%` skips that entry. Nonexistent
   or unreadable directories stay the existing silent `continue`
   (`plugin_host.cpp:250-252`).
6. **Single `_WIN32` seam discipline.**
   ([`plugin_loading_win32.md`](plugin_loading_win32.md) Decision 1.)
   Orchestration — dir-list assembly, dedup, ordering, the scan loop —
   stays platform-neutral in `plugin_host.cpp`; only the leaves diverge:
   which env vars name the data dirs, `dladdr` vs
   `GetModuleHandleExA`+`GetModuleFileNameA`, path-join separator. No
   parallel source file, no header platform branches.
7. **No new dependencies** (doc 10:19-35). Env reads via `std::getenv`;
   image location via `dladdr` (ships with `${CMAKE_DL_LIBS}`, already
   linked at `src/runtime/CMakeLists.txt:56`) / `GetModuleFileNameA`
   (kernel32, auto-linked). No `SHGetKnownFolderPath`, no platform-paths
   library, no new dependency-table row.
8. **Levelization unchanged.** Everything lands inside `arbc::runtime`
   (L5, doc 17:71); no new component edges; `scripts/check_levels.py`
   green.
9. **Hermetic tests.** Tests controlling default discovery must pin the
   relevant env vars (including neutralizing `$XDG_DATA_DIRS` /
   `%LOCALAPPDATA%` to temp paths) and must assert per-kind outcomes and
   per-directory report containment — never machine-total counts — so a
   developer machine with a real `/usr/local/share/arbc/plugins` cannot
   flip a result.

## Acceptance criteria

- **Lands claim** `03-layer-plugin-interface#default-plugin-dirs-follow-platform-convention`
  (`tests/claims/registry.tsv` row + `enforces:` tag): the default
  directory resolver returns, in pinned priority order, the per-user data
  dir (`$XDG_DATA_HOME`/`$HOME/.local/share`, or `%LOCALAPPDATA%`), each
  system data dir (`$XDG_DATA_DIRS`, spec fallback
  `/usr/local/share:/usr/share`), the image-relative libdir, and the
  configured install libdir — each suffixed `arbc/plugins` — honoring env
  overrides and skipping unresolvable entries, with no filesystem access
  and no dlopen. Enforced by unit tests in
  `src/runtime/t/plugin_host.t.cpp` under controlled env (set/unset
  `XDG_DATA_HOME`, `XDG_DATA_DIRS`, `HOME`, `LOCALAPPDATA`) asserting the
  exact ordered vector on both platform branches.
- **Lands claim** `03-layer-plugin-interface#standard-scan-orders-env-before-defaults-and-dedups`:
  the combined scan walks `ARBC_PLUGIN_PATH` dirs first (listed order),
  then default dirs minus already-visited ones — each directory
  contributes entries at most once — and a kind loaded from an env dir
  survives a same-id plugin in a default dir as a per-entry `DuplicateId`
  with the original factory intact. Enforced in
  `tests/plugin_loading.t.cpp` with fixture plugins staged into a temp
  `XDG_DATA_HOME` (`LOCALAPPDATA` on the Windows lane): (a) env unset ⇒
  the fixture kind in the default dir becomes resolvable; (b) the same
  dir listed in both `ARBC_PLUGIN_PATH` and `XDG_DATA_HOME` ⇒ exactly one
  report entry per plugin file, no self-inflicted `DuplicateId`; (c) same
  kind id staged in an env dir and a default dir ⇒ env copy wins,
  default copy reports `DuplicateId`.
- **Re-enforces claims** `registry.tsv:280` (`#plugin-path-scan-is-opt-in`
  — `scan_plugin_path()` unchanged; existing tests keep passing
  untouched) and `:281` (`#explicit-host-registration-precedes-scan` — an
  explicitly-registered kind also beats a default-dir plugin; extend the
  existing precedence test rather than minting a new claim).
- **Consumer proof (shared lane):** a new
  `tests/consumer/plugin_default_scan.cpp`, gated to `BUILD_SHARED_LIBS`
  like the DT_NEEDED machinery (`ArbcInstall.cmake:176-185`), runs
  against the staged install with `ARBC_PLUGIN_PATH` unset and XDG vars
  neutralized, calls the combined scan, and asserts `org.arbc.image` /
  `org.arbc.imageseq` resolve via the image-relative default — the
  end-to-end "installed package discovers its own plugins with zero
  configuration" story.
- **Behavioral, not wall-clock:** the resolver-purity claim (no fs
  access/dlopen) is pinned structurally (nonexistent paths in, exact list
  out), matching how `registry.tsv:280` is enforced today.
- **Gate ritual:** diff coverage ≥90%; `scripts/gate` green (levelization
  `scripts/check_levels.py`, claims `scripts/check_claims.py`); Windows CI
  lane green; `tj3 project.tjp 2>&1 | grep -iE "error|warning"` silent
  after the closer's `.tji` edits.
- **No deferred WBS leaves.** Nothing in scope is split off; macOS
  conventions are explicitly out of scope (no macOS seam or lane exists —
  a platform port would introduce them wholesale, not as a search-path
  follow-up).

## Decisions

1. **One new combined entry point; `scan_plugin_path()` keeps its exact
   contract.** Add `PluginScanReport PluginHost::scan_standard_paths()` —
   env dirs then defaults, deduped, one `PluginScanReport` — implemented
   over a factored-out private per-directory scan helper that
   `scan_plugin_path()` also calls. "Opt-in" stays what it has always
   meant here: the host explicitly calls a scan method; a host that wants
   env-only behavior keeps calling `scan_plugin_path()` and the
   `registry.tsv:280` zero-fs-access promise holds verbatim.
   *Rejected: widening `scan_plugin_path()` to include defaults* — breaks
   claim 280 as written and silently changes every existing caller into
   one that dlopens from user-writable home directories.
   *Rejected: a defaulted `bool include_default_dirs` parameter* — mutates
   the existing exported symbol for no expressive gain and hides the
   behavior switch at call sites.
   *Rejected: a defaults-only method the host sequences after
   `scan_plugin_path()`* — splits the ordering/dedup authority across two
   call sites and makes "dedup against explicitly-listed dirs" depend on
   the host calling both in the right order.
2. **Default directory set and order — user before system, install
   libdir last.** POSIX: `$XDG_DATA_HOME/arbc/plugins` (fallback
   `$HOME/.local/share/arbc/plugins`), then each
   `$XDG_DATA_DIRS/arbc/plugins` in listed order (fallback
   `/usr/local/share/arbc/plugins`, `/usr/share/arbc/plugins`), then the
   install-relative libdir `…/arbc/plugins` (Decision 3). Windows:
   `%LOCALAPPDATA%\arbc\plugins`, then the install-relative libdir.
   The `arbc/plugins` suffix everywhere matches the shipped layout
   ([`../packaging/install.md`](../packaging/install.md) D6) and its
   containment rationale. Because the scan is first-registration-wins
   (Constraint 4), earlier means higher priority: a per-user plugin
   shadows a system/shipped one with the same kind id — the conventional
   XDG semantics — while explicit host registration still beats
   everything (claim `:281`).
   *Rejected: system-before-user* — inverts XDG convention and makes user
   plugin overrides impossible without env vars.
   *Rejected: `%APPDATA%` (roaming)* — machine-specific binaries must not
   roam; `%LOCALAPPDATA%` is the Windows convention for them.
   *Rejected: `%ProgramData%`* — adds a third Windows dir with no current
   consumer; the install libdir covers the machine-wide case.
3. **Install-relative libdir = the arbc image's own directory +
   `arbc/plugins`, plus the configure-time
   `${CMAKE_INSTALL_FULL_LIBDIR}/arbc/plugins` as a final fallback
   entry.** Resolve the directory containing the arbc binary image at
   runtime (`dladdr` on an in-component symbol; `GetModuleHandleExA`
   with `GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS` +
   `GetModuleFileNameA` on Windows) and append `arbc/plugins` — in a
   shared-lane install that is exactly
   `<prefix>/<libdir>/arbc/plugins`, sibling to `libarbc.so`. Also
   append the configure-time absolute libdir (a private compile
   definition on `arbc_runtime`); in a non-relocated install the two
   coincide and dedup (Constraint 3) collapses them. The image-relative
   form is load-bearing: the consumer-test flow installs via standalone
   `cmake --install --prefix` into a staging tree
   (`ArbcInstall.cmake:176-182`), so a configure-time constant alone
   would dangle there and in any relocated package; conversely, in a
   static build the "image" is the host executable and the
   image-relative dir is usually meaningless, which the compiled-in
   entry backstops.
   *Rejected: compiled-in path only* — untestable end-to-end (staged
   installs relocate) and breaks relocatable packages.
   *Rejected: image-relative only* — static-linked hosts lose the
   classic prefix location.
4. **Directory dedup = string equality after trailing-separator trim; no
   canonicalization.** Dedup exists to honor the charter's "dedup against
   explicitly-listed dirs" — keeping the report free of spurious
   double-scan `DuplicateId` noise and avoiding double dlopen — not to
   solve path aliasing. Symlinked or case-aliased duplicates fall through
   to the registry's `DuplicateId` guard, which is the semantic
   protection (Constraint 4).
   *Rejected: `std::filesystem::weakly_canonical`* — buys only the
   symlink-alias case at the cost of extra filesystem stats and a new
   failure surface inside the resolver, which is otherwise pure.
5. **Env-var reads only for the data dirs — no shell APIs, no
   platform-paths library.** `%LOCALAPPDATA%` and the XDG vars via
   `std::getenv`, mirroring the existing `ARBC_PLUGIN_PATH` read
   (`plugin_host.cpp:188-195`); absence degrades to skip/fallback
   (Constraint 5). Doc 10:19-35's dependency policy makes any library
   answer a new justified table row this task doesn't need.
   *Rejected: `SHGetKnownFolderPath`* — drags shell32/COM into a
   component whose only platform deps today are kernel32/dl, for a value
   the env var already provides in every practical session.
6. **No design-doc delta.** Doc 10:52's "platform-conventional locations"
   is the authorizing envelope; picking the concrete set inside it is
   task-level, exactly as [`../packaging/install.md`](../packaging/install.md)
   D6 chose `arbc/plugins` "within doc 10:50-53's envelope … rather than
   via a design-doc amendment", and as the loader's existing claims were
   minted under the `03-layer-plugin-interface#` stem without a doc-03
   text change (`registry.tsv:280-282`). The two new claims rows are the
   normative record of the chosen semantics. Not project-shaping ⇒ no
   doc-00 bullet (mirrors [`plugin_loading.md`](plugin_loading.md)
   Decision 6).

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-16.

- Added `PluginHost::scan_standard_paths()` — walks `ARBC_PLUGIN_PATH` dirs first (existing contract), then platform-conventional defaults (XDG/`AppData` dirs, install-relative libdir via `dladdr`/`GetModuleFileNameA`, compiled-in libdir fallback), each suffixed `arbc/plugins` with string-equality dedup against already-visited dirs. (`src/runtime/plugin_host.cpp`, `src/runtime/arbc/runtime/plugin_host.hpp`)
- Factored a private per-directory scan helper out of `scan_plugin_path()` so both scan entry points share the same enumeration + load body; `scan_plugin_path()` is byte-for-byte unchanged in behavior. (`src/runtime/plugin_host.cpp`)
- Added compile-time libdir constant via private CMake compile definition on `arbc_runtime`; `${CMAKE_DL_LIBS}` (already linked) supplies `dladdr`. (`src/runtime/CMakeLists.txt`)
- Landed claim `03-layer-plugin-interface#default-plugin-dirs-follow-platform-convention` (resolver unit tests in `src/runtime/t/plugin_host.t.cpp` under controlled XDG/`LOCALAPPDATA` env vars) and claim `#standard-scan-orders-env-before-defaults-and-dedups` (cross-component precedence/dedup tests in `tests/plugin_loading.t.cpp`). (`tests/claims/registry.tsv`)
- New consumer proof `tests/consumer/plugin_default_scan.cpp` — runs against the staged install with `ARBC_PLUGIN_PATH` unset and XDG vars neutralized, calls `scan_standard_paths()`, and asserts `org.arbc.image` / `org.arbc.imageseq` resolve via the image-relative default. (`tests/consumer/CMakeLists.txt`, `tests/consumer/plugin_default_scan.cpp`)
