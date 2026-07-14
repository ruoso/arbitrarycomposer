# packaging.version_api — version API + changelog discipline

## TaskJuggler entry

[`tasks/75-packaging.tji:17-21`](../../75-packaging.tji) —

```
task version_api "Version API + changelog discipline" {
  effort 0.5d
  allocate team
  note "Real version symbols/macros, semver from first tag, hand-kept CHANGELOG.md. Docs 10/16."
}
```

Note the absent `depends`: `version_api` does **not** wait on `!install`, unlike
its `plugin_helper` and `examples` siblings. It inherits only the parent
container's edges (`tasks/75-packaging.tji:4`).

Milestone: **M9** (`tasks/99-milestones.tji:70-73`, the v0.1 release) — reached
through `packaging.release_01`, which `depends !install, !version_api, !examples`
(`tasks/75-packaging.tji:28-33`).

## Effort estimate

**Booked 0.5d; realistic 0.5d.** The booking is right, and this is one of the
few leaves where it is. The C++ is a generated header of macros plus two
one-line functions; there is no algorithm, no concurrency, no rendering, and no
new component. The cost is entirely in three fiddly-but-small CMake seams and
one honest changelog.

Budget:

- 0.2d — the generated header: `configure_file` from a template, and getting it
  onto the umbrella's `FILE_SET HEADERS` with a **build-tree** `BASE_DIRS` entry
  alongside the source-tree component dirs (`cmake/ArbcComponent.cmake:173-175`).
  This is the only genuinely new CMake shape in the task.
- 0.1d — `src/version.cpp` rewritten against the generated header; the in-tree
  test.
- 0.1d — extending the existing out-of-tree consumer
  (`tests/consumer/core_only.cpp`, `tests/consumer/CMakeLists.txt`) to close the
  loop across a real staged install prefix.
- 0.1d — `CHANGELOG.md` and the doc-10 delta.

## Inherited dependencies

**Settled (formal `depends`, inherited from the `packaging` container,
`tasks/75-packaging.tji:4`):**

- **`runtime.host_objects`**
  ([`../runtime/host_objects.md`](../runtime/host_objects.md), Done) — nothing in
  it bears on this task. It is a container-level edge that gates *packaging as a
  whole* on the library having a host-facing surface worth packaging; it hands
  this leaf no seam.
- **`kinds.raster`** ([`../kinds/raster.md`](../kinds/raster.md), Done) — likewise
  a container edge. Its one relevance: it established that an `arbc::kind-*` is
  standalone-linkable into a per-kind `.so` through the doc-03 `extern "C"` entry
  point. Plugins compiled against a *shipped header set* are exactly the
  consumers a version macro exists to serve, which is why Decision 4 below has to
  say out loud what this task is *not* giving them.
- **`serialize.kind_params`**
  ([`../serialize/kind_params.md`](../serialize/kind_params.md), Done) — container
  edge. Its Constraint 7 is the rule this task's header must obey: a header goes
  in `PUBLIC_HEADERS` only if it names no private third-party type
  (`deserialize.hpp` is internal *precisely because* it names `nlohmann::json`).
  Separately, its `kind_version` is a **per-kind** string in the serialized
  content body — orthogonal to the library-wide semver this task mints, and the
  two must not be confused (Constraint 6).

**Settled (informal, but the real context — this task exists in the shape it does
because they landed):**

- **`quality.testing_artifact`**
  ([`../quality/testing_artifact.md`](../quality/testing_artifact.md), Done
  2026-07-14, commit `f34d731`) — the whole reason this leaf is cheap. It landed
  the install/export seam early, out of `packaging.install`'s scope: the umbrella
  `install(TARGETS arbc … FILE_SET HEADERS)` (`cmake/ArbcInstall.cmake:60-66`),
  the generated `arbcConfig.cmake` + `arbcConfigVersion.cmake`
  (`:80-91`), and — decisively — the **out-of-tree staged-install consumer**
  (`tests/consumer/`, driven by the `install.consumer` CTest at
  `tests/CMakeLists.txt:1041-1079`). That consumer is a genuinely foreign CMake
  project that `find_package(arbc CONFIG)`es a staged prefix. It is the seam this
  task's strongest acceptance criterion rides on, and it already exists.
  It also routed this work here explicitly (`testing_artifact.md:205-210`):
  *"the real version API … is `packaging.version_api` … **not** this task."*
- **`serialize.zstd_dep`** ([`../serialize/zstd_dep.md`](../serialize/zstd_dep.md),
  Done 2026-07-14) — its Constraint 3 ("no `#include <zstd.h>` may appear in any
  header installed with `libarbc`") generalizes to the law this task's header
  satisfies trivially and maximally: `arbc/version.hpp` includes **nothing at
  all**.

**Pending, and deliberately not a dependency:**

- **`packaging.install`** (`tasks/75-packaging.tji:6-10`, 2d, unstarted) — owns
  pkg-config, CPS metadata, `VERIFY_INTERFACE_HEADER_SETS ON`, and CPack
  (`cmake/ArbcInstall.cmake:17-20`). No edge is added in either direction: the
  minimal install seam this task needs already landed with
  `quality.testing_artifact`, and `VERIFY_INTERFACE_HEADER_SETS` — which will
  compile `arbc/version.hpp` standalone — is a check that *ratifies* this header,
  not one it waits on (Decision 5).
- **`packaging.shared_library_build`** (`tasks/75-packaging.tji:34-38`) — owns
  `ARBC_API` and `SOVERSION`. This task must not pre-empt either (Constraint 5).

**Downstream (this task unblocks):**

- **`packaging.release_01`** (`tasks/75-packaging.tji:28-33`) — *"Tag, release
  notes, README quickstart"*. The tag itself, and the stale
  `README.md:30-32` (*"Design phase. No code yet."*), are **its** work, not this
  task's (Decision 7).

## What this task is

Give `libarbc` a real, reachable version — and start the changelog.

Today the entire version surface is `src/version.cpp:1-7`: a hardcoded
`arbc::version_string()` returning `"0.1.0"`, declared in **no header anywhere in
the tree**, called by nothing, and carrying a literal that can silently drift from
the `project(arbitrarycomposer VERSION 0.1.0)` at `CMakeLists.txt:3-7`. Its own
comment admits it: *"the real version API arrives with install/packaging (doc
10)."* This is that arrival.

The task lands four things:

1. **`arbc/version.hpp`** — a public header owned by the L6 umbrella, generated
   by `configure_file` from `project(VERSION …)`, carrying the compile-time
   macros (`ARBC_VERSION_MAJOR/MINOR/PATCH`, an encoded comparable
   `ARBC_VERSION`, `ARBC_VERSION_STRING`) and a `constexpr`
   `arbc::compiled_version()`.
2. **The linked symbols** — `arbc::linked_version()` /
   `arbc::linked_version_string()`, compiled into `libarbc` from that same
   header, replacing the orphan `version_string()`. These report the version of
   the library *actually linked or loaded*, which the macros structurally cannot.
3. **`CHANGELOG.md`** — Keep-a-Changelog form, seeded with the `[Unreleased]`
   surface the first tag will name.
4. **The single-source-of-truth guarantee, tested** — an in-tree test plus an
   extension to the existing out-of-tree consumer that pins header, library, and
   CMake package config to the same triple across a real staged install prefix.

It does **not** cut a tag, mint a plugin ABI number, or touch `SOVERSION`.

## Why it needs to be done

Three consumers are blocked or lying without it.

- **`packaging.release_01` cannot tag.** Doc 16:143 says *"semver from the first
  tag"*, and there is no tag yet (`git tag -l` is empty). A first tag that names
  a version the library cannot report, and that a consumer cannot test against,
  is a tag over a promise nothing keeps.
- **Every embedder and plugin author.** `find_package(arbc CONFIG)` already
  produces an `arbc_VERSION` from `arbcConfigVersion.cmake`
  (`cmake/ArbcInstall.cmake:88-91`), but there is no way to ask the *library* what
  it is — no header declares a symbol, so `#if`-gating against a future release,
  or logging what you actually linked, is impossible from C++. For plugin authors
  this is sharper than for embedders: doc 03 (§ Stage 1) accepts same-toolchain
  coupling for v1, which makes "what libarbc am I actually loaded into" a
  question they will genuinely need to answer at runtime, and today they cannot.
- **The drift is already live.** `"0.1.0"` exists twice — `CMakeLists.txt:5` and
  `src/version.cpp:5` — with nothing keeping them equal. The second literal is a
  latent bug, not a hypothetical one; the first version bump introduces it.

## Inputs / context

### Design docs (normative, doc 16)

- **Doc 16:143-148** — the *entire* normative versioning text in the project, and
  the source of this task's `note`:
  > **Versioning and compatibility**: semver from the first tag; pre-1.0 moves
  > freely with changelog honesty (`CHANGELOG.md`, kept by hand, one entry per
  > landed change). At 1.0: ABI checking (abi-compliance-checker or libabigail)
  > joins CI, deprecation policy is two minor versions with attributes, and the
  > doc-03 C-ABI work item activates with its own conformance suite.

  Note what it does *not* say: nothing about macros, a header, `SOVERSION`,
  compatibility mode, or changelog format. Those were an open design surface, now
  closed by the doc-10 delta below.
- **Doc 16:138-142** — definition of done, *"`CONTRIBUTING.md` carries it"*. That
  file does not exist (Decision 8).
- **Doc 16:8-25** — the claims register and the same-commit design-doc rule.
- **Doc 16:128-132** — *"Public API is a deliberate surface"*; anything crossing
  into public headers gets doc references in the same change.
- **Doc 17:33** — `L6  arbc  (umbrella: symbol surface, **version**, registry
  bootstrap)`. The umbrella owning the version is *already settled*; this task
  only makes it concrete. Table row at **17:72**.
- **Doc 17:192-213** — public-vs-private headers via `FILE_SET HEADERS`; install
  aggregation onto the umbrella; `VERIFY_INTERFACE_HEADER_SETS ON` compiles every
  public header standalone.
- **Doc 03:188-201** — the two-stage plugin ABI. Stage 1 (v1) accepts
  compiler/ABI coupling with a bare `extern "C" arbc_plugin_register(Registry&)`;
  *"versioned, semver-gated capability flags"* are **Stage 2, post-v1**, which
  doc 16:147 activates at 1.0. Also **doc 00:110-112** (§ Resolved questions).
- **Doc 10:15-17** — no exceptions across public API and plugin boundaries;
  errors are values.
- **Doc 10:55-106 — § Versioning and the version API.** *New; the design-doc
  delta this refinement lands* (see Decision 9). It records the mechanism doc
  16's policy is carried by: one source of truth, the generated header, the
  linked symbols, skew-reported-never-enforced, no plugin ABI number,
  `SOVERSION` deferred, and the changelog's form.

### Source seams

- **`CMakeLists.txt:3-7`** — `project(arbitrarycomposer VERSION 0.1.0 …)`. The
  single source of truth this task makes real. `cmake_minimum_required(VERSION
  3.24)` at `:1`.
- **`src/version.cpp:1-7`** — the whole thing being replaced. Undeclared,
  uncalled, hardcoded.
- **`cmake/ArbcComponent.cmake:161-180`** — `arbc_finalize_library()`, which owns
  the umbrella. `:166` is `add_library(arbc "${CMAKE_CURRENT_SOURCE_DIR}/version.cpp")`
  — the umbrella's *only* source file, and it has **no header file set of its
  own**; `:173-175` aggregates every *component's* headers onto one `FILE_SET`
  with `BASE_DIRS ${component_dirs}`. Adding the generated header means adding a
  build-tree `BASE_DIRS` entry here. `:5-7` states the header-layout rule;
  `:23-52` is `arbc_add_component()`.
- **`cmake/ArbcInstall.cmake:60-66`** — `install(TARGETS arbc … FILE_SET HEADERS
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")`. Whatever lands on the umbrella's
  file set installs, with **no edit to this file**.
- **`cmake/ArbcInstall.cmake:85-91`** — `write_basic_package_version_file(…
  VERSION "${PROJECT_VERSION}" COMPATIBILITY SameMajorVersion)`, with the comment
  that names this very task: *"The C++ version API is a separate leaf
  (packaging.version_api); this reads PROJECT_VERSION and needs no symbol."*
  Compatibility mode is therefore **already decided: `SameMajorVersion`** — do
  not revisit it.
- **`tests/consumer/core_only.cpp:1-40`** — the foreign-project consumer. It
  already includes `<arbc/base/geometry.hpp>`, `<arbc/contract/content.hpp>`,
  `<arbc/kind_solid/solid_content.hpp>` from a staged prefix and links the
  installed archive; `:22` carries an `// enforces:` tag. This is where
  `<arbc/version.hpp>` gets proven *installed*, not merely *generated*.
- **`tests/consumer/CMakeLists.txt`** — the foreign `project()` +
  `find_package(arbc CONFIG)`. Where `arbc_VERSION` gets compared to the header.
- **`tests/CMakeLists.txt:1041-1079`** — the `install.consumer` CTest that stages
  `cmake --install` and drives the above. A CTest and not a CI-only step, so
  `scripts/gate` and CI run the same check.
- **`tests/claims/registry.tsv:1-3`** — `<claim-id>\t<description>`; a claim id is
  `<doc-file-stem>#<slug>`; each entry must be referenced by an
  `// enforces: <claim-id>` test comment. `scripts/check_claims.py` gates this
  **bidirectionally** (registered-but-unenforced *and* enforced-but-unregistered
  both fail) and scans `*.cpp`/`*.hpp` under `src/`, `tests/`, `testing/`.
- **`.github/workflows/ci.yml:76`** — the `gcc-install-fetched` lane, the one that
  forces the pinned-FetchContent zstd install path. `install.consumer` runs on the
  ordinary `ctest` step, so the new consumer assertions get exercised on both
  install paths for free.

## Constraints / requirements

1. **One source of truth, mechanically.** After this task there is exactly one
   `0.1.0` literal in the repository outside `CHANGELOG.md`: the one in
   `project(… VERSION …)` at `CMakeLists.txt:5`. The header is generated; the
   `.cpp` reads the header; the package config already reads `PROJECT_VERSION`. A
   version bump must be a one-line edit, and the acceptance test must **fail** if
   anyone reintroduces a second literal.
2. **`arbc/version.hpp` includes nothing.** No `<cstdint>`, no `<string>`,
   nothing. Use plain `int` fields and `const char*`. It must compile standalone
   (doc 17:210-213's `VERIFY_INTERFACE_HEADER_SETS`, landing with
   `packaging.install`) and warning-clean under a consumer's own flags — the
   export interface is deliberately scrubbed of `arbc_build_flags`
   (`cmake/ArbcComponent.cmake:169`, `testing_artifact.md` Constraint 7), so
   `-Wpedantic -Werror` is *our* discipline and cannot be assumed downstream.
3. **The header is the umbrella's, and no component may include it.** It installs
   to `<prefix>/include/arbc/version.hpp` — at the include root, *not* under an
   `arbc/<component>/` subdirectory, because it belongs to no component
   (doc 17:33). This is enforced **by construction, not by lint**: the generated
   header's `BASE_DIRS` entry lives only on the umbrella target's `FILE_SET`,
   components link only their declared `arbc_<dep>` object libraries and never
   the umbrella (`cmake/ArbcComponent.cmake:38-40`, `:176-178`), so a component
   that wrote `#include <arbc/version.hpp>` would simply fail to find the file.
   An L4 kind reaching *up* to L6 is exactly the cycle doc 17:52-55 forbids, and
   the build already makes it impossible. `scripts/check_levels.py` must pass
   **unchanged** — its `INCLUDE_RE` (`:64`) matches `arbc/<component>/…`, which
   `arbc/version.hpp` deliberately is not.
4. **The two halves must be able to disagree.** `compiled_version()` is
   `constexpr`, inlined from whatever header the *consumer* compiled against;
   `linked_version()` is an out-of-line symbol in `libarbc`, compiled from
   whatever header *the library* was built from. Do not make `linked_version()`
   `constexpr` or `inline`, and do not define it in the header — that would
   collapse the two into one number and destroy the only thing the pair is for.
5. **Do not pre-empt `packaging.shared_library_build`.** No `ARBC_API` macro, no
   visibility annotation, no `SOVERSION`/`VERSION` target property
   (`cmake/ArbcInstall.cmake:17-20` and `tasks/75-packaging.tji:34-38` assign all
   of these there). The library stays static-only here.
6. **Library version ≠ kind version.** `serialize.kind_params` put a
   `kind_version` **string** in each serialized content body, per-kind and
   independently evolving (doc 08 Principle 4). The `ARBC_VERSION_*` macros are
   the *library's* semver. Nothing in this task may read, write, or derive one
   from the other, and the header should say so in a comment — the names are
   close enough to invite the confusion.
7. **Lints.** `CHANGELOG.md` must pass `markdownlint` and the link checker
   (doc 16:206-209) and `typos` (16:210-211); the new/edited CMake must pass
   `gersemi` + `cmake-lint` (16:204-205). Run `scripts/gate` before committing.

## Acceptance criteria

- **New claim `16-sdlc-and-quality#library-version-is-single-sourced`**
  (`tests/claims/registry.tsv`): *The version the library reports at link time
  (`arbc::linked_version()`), the version its public header declares at compile
  time (`ARBC_VERSION_MAJOR/MINOR/PATCH`, `arbc::compiled_version()`), and the
  version its CMake package advertises (`arbc_VERSION`, from `PROJECT_VERSION`
  via `arbcConfigVersion.cmake`) are one and the same triple — in the build tree
  and, independently, through a staged install prefix consumed by a foreign
  project.* Anchored to the doc-10 § Versioning delta and doc 16:143. Enforced in
  two places, and it takes both to be worth anything:

  - **`tests/version_api.t.cpp`** (new; links the umbrella `arbc`, Catch2, per
    the `tests/CMakeLists.txt` triple convention) — carries the `// enforces:`
    tag. Asserts (a) `arbc::compiled_version() == arbc::linked_version()`;
    (b) the macros agree with the struct, and `ARBC_VERSION ==
    ARBC_VERSION_ENCODE(ARBC_VERSION_MAJOR, ARBC_VERSION_MINOR,
    ARBC_VERSION_PATCH)`; (c) `ARBC_VERSION_STRING` and `linked_version_string()`
    are the same text and spell the same triple; (d) **the load-bearing one** —
    all of the above equal a `PROJECT_VERSION`-derived value injected as a
    `target_compile_definitions` on *the test target only*. (d) is what actually
    pins Constraint 1: it is a second, independent path from `project(VERSION …)`
    to the assertion, so a reintroduced literal in `src/version.cpp` fails the
    test rather than passing vacuously against itself.
  - **`tests/consumer/core_only.cpp` + `tests/consumer/CMakeLists.txt`**
    (extended) — the foreign project `find_package(arbc … CONFIG REQUIRED)`es the
    staged prefix, passes `${arbc_VERSION}` down as a compile definition, includes
    `<arbc/version.hpp>` **from the installed prefix**, and asserts the installed
    header's `compiled_version()`, the installed archive's `linked_version()`, and
    the package config's `arbc_VERSION` all agree. This is the half that proves
    the header is *installed* and reachable — an in-tree test alone would pass
    happily on a header that never ships. Runs under the existing
    `install.consumer` CTest (`tests/CMakeLists.txt:1041-1079`) on both the
    system-zstd and fetched-zstd install paths (`.github/workflows/ci.yml:76`), at
    no new CI cost.

- **`CHANGELOG.md` exists at the repo root**, Keep-a-Changelog form, with an
  `[Unreleased]` section describing the surface the 0.1 tag will name, and a
  header comment stating the discipline (one entry per landed change; the
  changelog tracks the shipped surface, the git log tracks commits). Its
  `[Unreleased]` content is the raw material `packaging.release_01` turns into
  release notes.

- **No new golden, conformance, behavioral-counter, TSan, or stress coverage —
  and that is a deliberate reading of doc 16's taxonomy, not an omission.** This
  task adds no content kind and no operator (nothing to run the contract
  conformance suite over, doc 16:31-44); renders no pixels (no goldens, 16:48-53);
  makes no performance-shaped promise (no behavioral counters, 16:54-62); and
  touches no thread, lock, or shared mutable state (no TSan/stress obligation,
  16:66-70). Its promise is a *build-and-install* invariant, and the two tests
  above are the shape that pins it.

- **Diff coverage ≥90%** on the changed C++ lines (doc 16:112-118). The bodies
  are trivial and both tests call them, so this is free — with **one gotcha worth
  naming**: `src/arbc/version.hpp.in` is a template, not a compiled translation
  unit, and has no coverage data. If the diff-coverage tool counts its changed
  lines as uncovered, exclude the `.in` file as a non-compiled artifact rather
  than contorting the test.

- **Levelization + build + WBS gate green.** `scripts/check_levels.py` passes
  **unchanged** (Constraint 3); `scripts/check_claims.py` passes with the new
  registry entry and its `enforces:` tag; `scripts/gate` is green; and
  `tj3 project.tjp 2>&1 | grep -iE "error|warning"` is silent after the `.tji`
  `complete 100` + refinement back-link land.

- **Design-doc delta** (already landed with this refinement; rides the closer's
  commit per doc 16's same-commit rule — see Decision 9):
  `docs/design/10-tooling-and-packaging.md:55-106`, new § *Versioning and the
  version API*.

**Deferred follow-up (closer registers as a real WBS leaf, wired into M9
`m9_release`):**

- **`quality.contributing_doc`** — *0.5d.* Write the `CONTRIBUTING.md` that doc
  16:138-142 already promises (*"`CONTRIBUTING.md` carries it"*) and that does
  not exist: the definition-of-done checklist (code + contract/claims tests +
  design-doc delta + benchmark clean + examples still building), the
  `scripts/gate` invocation, the claims-register ritual
  (`registry.tsv` + `// enforces:`), and the changelog rule this task starts. No
  `depends` beyond `!version_api` (it documents the discipline this task begins).
  `note` cites this refinement. Milestone **M9**. Registered because doc 16
  assigns `CONTRIBUTING.md` a whole job — the DoD checklist — that is far wider
  than the one changelog line this 0.5d task can honestly carry, and stuffing the
  discipline into a code comment and calling it done would be exactly the
  aspirational-docs drift doc 16:19-21 exists to prevent.
  (Deferred to `quality.contributing_doc` — closer registers in WBS.)

**Nothing else deferred.** `SOVERSION`/`ARBC_API`
(`packaging.shared_library_build`), `VERIFY_INTERFACE_HEADER_SETS`
(`packaging.install`), and the tag + release notes + README refresh
(`packaging.release_01`) are all **existing** WBS leaves that already own their
piece — no new task, no narrowing, no parking-lot item.

## Decisions

**D1. `arbc/version.hpp` is generated by `configure_file` from
`project(VERSION …)`. The header is derived; the CMake declaration is the truth.**
*Rationale:* the bug this task is fixing *is* a second source of truth —
`src/version.cpp:5` hardcodes `"0.1.0"` next to `CMakeLists.txt:5`'s `0.1.0`,
with nothing holding them equal. Any design that keeps two literals has fixed
nothing; it has only moved the drift. `configure_file` is the CMake-native answer,
it costs one template file, and it automatically adds the template to the
configure dependencies so a version bump reconfigures.
*Rejected: a hand-written `arbc/version.hpp` as the source of truth, with CMake
parsing the macros back out.* This is what libraries that must be usable from a
plain tarball with no CMake do. We are not one of those — CMake ≥ 3.24 is the
canonical and only build (doc 10:39-42) — so we would be paying a regex-parser in
CMake to buy a property we do not use.
*Rejected: hardcode both and add a lint that greps them for equality.* A gate
that detects drift instead of preventing it, and one more script to own. Strictly
worse than not having the second literal.

**D2. Ship macros *and* symbols, because they answer different questions —
`compiled_version()` (constexpr, from the header) and `linked_version()`
(out-of-line, in `libarbc`).**
*Rationale:* this is the substance behind the task's *"real version
symbols/macros"*. Macros describe **the headers you compiled against**; they are
what `#if`-gating needs and they cost nothing at runtime. A symbol describes
**the library you actually linked or loaded** — the number a host wants when it
ships a prebuilt `libarbc`, or `dlopen`s a plugin built against a different one
(doc 03:188-192 accepts precisely that same-toolchain coupling for v1, which is
what makes the question live). A version API with only macros is
*constitutionally unable* to detect header/library skew: it would report the
header's opinion of itself, twice. Hence Constraint 4 — `linked_version()` must
not be `inline` or `constexpr`, or the pair collapses into one number and the
whole point evaporates.
*Rejected: macros only.* Cheaper, and useless for the one failure it should
catch.
*Rejected: symbols only (keep `version_string()`, just declare it).* The minimal
diff, and it would leave `#if ARBC_VERSION >= …` — the thing a plugin author
reaches for first — impossible.

**D3. Skew is *reported*, never *enforced*: no auto-check, no `static_assert`
against the linked version, no abort.**
*Rationale:* the library exposes both numbers and lets the host decide. A
`static_assert` cannot see the linked version by construction (that is D2's whole
point), and a runtime check that aborts is a library killing its host's process
over a version comparison — a strictly worse outcome than the skew it is
"protecting" from, and flatly against doc 10:15-17's errors-are-values,
no-exceptions-across-the-boundary posture. There is also nothing to catch today:
the static build has no skew mode at all, and the shared build that introduces one
is `packaging.shared_library_build`'s.
*Rejected: an `arbc::check_version()` that returns an `arbc::expected`.* The
right *shape* — but a helper with zero call sites, wrapping a comparison the host
can write in one line, and speculatively designed for a shared build that has not
landed. Doc 03:217-221 already sets the precedent for this exact judgment
(capability flags are *"deferred to their first consumer"* rather than carried
unused). Two integers and an `==` is the API; when the shared build gives it a
first consumer, it can grow one.

**D4. No plugin ABI version. The `extern "C" arbc_plugin_register` seam is left
exactly as it is.**
*Rationale:* the temptation here is real — this is a version task, and plugins are
the obvious beneficiary of a version number. But doc 03:194-201 puts *"versioned,
semver-gated capability flags"* squarely in **Stage 2, post-v1**, and doc 16:147
activates the C-ABI work item **at 1.0**. Doc 03:188-192 says the v1 seam accepts
compiler/ABI coupling outright. Minting an ABI number now would advertise a
compatibility promise the v1 interface explicitly does not make — worse than no
number, because a plugin author would reasonably read `abi_version == 1` on both
sides as "these are compatible" when in fact a different compiler already breaks
them. The library version this task ships is the *library's*, and the doc-10 delta
says so in as many words.
*Rejected: add an `arbc_plugin_abi_version()` symbol to the plugin entry point.*
See above: it freezes a number before the ABI it names exists, and buys false
confidence.

**D5. Version-encoding: `ARBC_VERSION = major*1000000 + minor*1000 + patch`, with
an `ARBC_VERSION_ENCODE(major, minor, patch)` macro to build comparands.**
*Rationale:* the macro is the point — consumers write
`#if ARBC_VERSION >= ARBC_VERSION_ENCODE(0, 2, 0)` and never hand-encode. The
1000-radix gives 999 headroom per field and stays legible in decimal (`0.1.0` →
`1000`; `1.2.3` → `1002003`), which matters when the number shows up in a build
log.
*Rejected: the common `major*10000 + minor*100 + patch`.* Caps minor and patch at
99. Doc 16:144 asks for *"one entry per landed change"* on a pre-1.0 project that
is already 184 commits deep — a 0.x line exceeding 99 patch releases is not
far-fetched, and the failure mode is a silent wraparound in a comparison macro.
*Rejected: bit-packing, `(major<<16)|(minor<<8)|patch`.* Same cap at 255, and
unreadable in a log for no gain.

**D6. `CHANGELOG.md` starts at the shipped surface, not at the genesis commit.**
*Rationale:* doc 16:143-145 says *"pre-1.0 moves freely with changelog honesty …
one entry per landed change"*. There are 184 commits and no tag; the honest
reading is that the changelog documents **the evolution of the shipped surface for
its consumers**, and before the first tag there is no shipped surface and no
consumer for anything to have changed *from*. So: seed `[Unreleased]` with the
surface the 0.1 tag will actually name (which `packaging.release_01` then turns
into release notes — that is the same content, and writing it twice would be the
real waste), and apply "one entry per landed change" from this commit forward.
*Rejected: back-fill 184 per-commit entries.* Days of archaeology producing a
document whose every entry describes a change no user could observe, because
nothing was ever released. The git log is already the per-commit record; the
changelog's job begins where the public surface does.
*Rejected: leave `[Unreleased]` empty and start from the next commit.* Would hand
`packaging.release_01` an empty changelog to write release notes from, i.e. defer
exactly the work that is cheapest to do now, while the surface is fresh.

**D7. This task does not cut the `v0.1.0` tag and does not touch `README.md`.**
*Rationale:* `packaging.release_01` (`tasks/75-packaging.tji:28-33`) owns *"Tag,
release notes, README quickstart reflecting the shipped surface"* and `depends
!install, !version_api, !examples` — tagging before `install` and `examples` land
would tag a surface that is not yet the released one. `README.md:30-32` still says
*"Design phase. No code yet."*, which is badly stale, but it is stale in
`release_01`'s file and gets fixed there, with the quickstart, in one pass.
*Rejected: fix the stale README line here since we are in the neighbourhood.* It
would half-fix a file another leaf must rewrite anyway, and split one honest
edit across two commits.

**D8. The changelog *discipline* is documented in `CHANGELOG.md`'s own header, not
enforced by a CI gate — and the full `CONTRIBUTING.md` is registered as a
follow-up rather than smuggled in here.**
*Rationale:* doc 16:143-144 says *"kept by hand"*, and doc 16:138-142 puts the
definition-of-done in `CONTRIBUTING.md` as *"checklist where not [mechanized]"* —
the docs already chose discipline over gate for this one. That choice is also the
right one: a "CHANGELOG.md must be touched" CI check fires on every docs-only,
refinement-only, and test-only commit, and a gate people routinely bypass teaches
that gates are bypassable. Putting the rule in the changelog's own header puts it
in front of the person editing the changelog, which is the only person it is for.
*Rejected: a CI check that the diff touches `CHANGELOG.md`.* Noisy, bypassed,
and contrary to doc 16's explicit "by hand".
*Rejected: write the whole `CONTRIBUTING.md` in this task.* Doc 16:138-142 gives
that file a much bigger job than the changelog line — the entire DoD checklist,
the gate ritual, the claims-register ritual. Bolting it onto a 0.5d version task
would either bloat the task or produce a token `CONTRIBUTING.md` that discharges
doc 16's promise on paper while leaving it unmet. Registered as
`quality.contributing_doc` (M9) instead.

**D9. Design-doc delta to doc 10, § *Versioning and the version API*
(`docs/design/10-tooling-and-packaging.md:55-106`). No doc-00 decision-record
bullet.**
*Rationale:* doc 10 — the *packaging* doc — contained **zero** version, semver,
changelog, or release material; the single normative sentence in the project is
doc 16:143-148, which states *policy* (semver, hand-kept changelog) and no
mechanism. That left the mechanism genuinely undecided, and a new installed public
header is precisely the kind of "crossing into public headers" that doc 16:128-132
says gets doc references in the same change. So the delta records the mechanism
where packaging lives, and cross-references doc 16 for the policy it serves rather
than restating it.
*No doc-00 decision-record bullet.* The project-shaping calls here were already
made and recorded elsewhere: doc 16:143 settled semver-and-hand-kept-changelog,
doc 17:33 settled that the umbrella owns "version", and doc 00:110-112 settled the
two-stage plugin ABI that D4 declines to pre-empt. Nothing in this task overturns
a decision or opens a new axis — it fills in the mechanism under three existing
ones, which is a doc-10 concern and not a constitutional amendment.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-14.

- `src/arbc/version.hpp.in` (new) — `configure_file` template generating `arbc/version.hpp` from `project(VERSION …)` with `ARBC_VERSION_MAJOR/MINOR/PATCH`, `ARBC_VERSION`, `ARBC_VERSION_STRING`, `ARBC_VERSION_ENCODE`, and `constexpr arbc::compiled_version()`.
- `src/version.cpp` — rewritten against the generated header; `arbc::linked_version()` and `arbc::linked_version_string()` replace the orphan hardcoded `version_string()`.
- `cmake/ArbcComponent.cmake` — umbrella `FILE_SET HEADERS` gains a build-tree `BASE_DIRS` entry for the generated header so it installs via the existing `cmake/ArbcInstall.cmake:60-66` path without any edit there.
- `tests/version_api.t.cpp` (new) — 13-assertion Catch2 unit test; enforces claim `16-sdlc-and-quality#library-version-is-single-sourced`; pins header/symbol/`PROJECT_VERSION` agreement via a compile-definition injected on that target alone.
- `tests/consumer/core_only.cpp` + `tests/consumer/CMakeLists.txt` — extended out-of-tree consumer includes `<arbc/version.hpp>` from the staged install prefix and asserts `compiled_version() == linked_version() == arbc_VERSION` through a real `find_package(arbc CONFIG)`.
- `tests/CMakeLists.txt` — wires the new unit test target.
- `tests/claims/registry.tsv` — registers claim `16-sdlc-and-quality#library-version-is-single-sourced`.
- `docs/design/10-tooling-and-packaging.md` — new § *Versioning and the version API* (`:55-106`) documents the mechanism: one source of truth, the generated header, the linked symbols, skew-reported-never-enforced.
- `CHANGELOG.md` (new) — Keep-a-Changelog form, seeded with `[Unreleased]` describing the surface the 0.1 tag will name.
