# packaging.release_01 — 0.1 release checklist

## TaskJuggler entry

[`tasks/75-packaging.tji:32-37`](../../75-packaging.tji):

```
task release_01 "0.1 release checklist" {
  effort 0.5d
  allocate team
  depends !install, !version_api, !examples
  note "Tag, release notes, README quickstart reflecting the shipped surface."
}
```

The whole packaging block feeds **M9** (`tasks/99-milestones.tji:70-73`, the
v0.1 release); `release_01` is on M9's `depends` list directly. One WBS leaf
depends on this task: `runtime.plugin_default_search_paths`
(`tasks/65-runtime.tji:56-61`, `depends !plugin_loading, packaging.release_01`)
— a fact that shapes Decision D1 below.

## Effort estimate

Booked 0.5d; realistic 0.5d.

- README rewrite (quickstart + shipped-surface refresh): ~0.15d
- CHANGELOG backfill of the four missing surface entries: ~0.1d
- README↔example snippet-sync test + claims-register entry: ~0.15d
- Doc 10 delta (§ Cutting a release) + tag_01 hand-off text: ~0.1d

## Inherited dependencies

**Settled (formal `depends`, all `complete 100`):**

- **`packaging.install`** (`tasks/refinements/packaging/install.md`) — the
  installed package surface the README must describe:
  `find_package(arbc CONFIG)` → `arbc::arbc` imposing nothing;
  `COMPONENTS testing` → `arbc::testing` + Catch2; plugins install as
  loadable modules under `${libdir}/arbc/plugins`, never as link targets
  (install.md:134-138); `arbc.pc` and `arbc.cps` alongside (install.md
  D3/D4).
- **`packaging.version_api`** (`tasks/refinements/packaging/version_api.md`)
  — the version machinery (`project(VERSION 0.1.0)` at `CMakeLists.txt:3-5`
  is the single source of truth; `arbc/version.hpp`;
  `compiled_version()`/`linked_version()`), the seeded `CHANGELOG.md`, and —
  binding on this task — **D6** (:466-484: `[Unreleased]` describes the
  surface the 0.1 tag will name and "`packaging.release_01` then turns
  [it] into release notes — that is the same content, and writing it twice
  would be the real waste") and **D7** (:486-496: the `v0.1.0` tag and the
  README refresh are this task's work, "with the quickstart, in one pass").
- **`packaging.examples`** (`tasks/refinements/packaging/examples.md`) — the
  quickstart's source of truth. Its Downstream note (:90-91) is explicit:
  "the README quickstart it writes should lift its embedding snippet from
  `examples/host-offline/`", and :122-123: "`packaging.release_01` cannot
  write an honest README quickstart without a compiled, CI-run embedding
  example to lift from." Both host examples are configured, built, **and
  run** against a staged install on every lane under the `install.consumer`
  CTest, with byte-exact artifact validation
  (`tests/consumer/host_example_artifacts.cpp:230`, claim
  `16-sdlc-and-quality#shipped-examples-compile-and-run-in-ci`).

**Settled (informal — shipped surface the README/CHANGELOG must reflect):**

- **`packaging.plugin_helper`** — `arbc_add_plugin()` callable from the
  installed package (`cmake/ArbcInstall.cmake` installs and `include()`s
  `ArbcAddPlugin.cmake` beside `arbcConfig.cmake`), plus
  `examples/plugin-template/`; claim
  `10-tooling-and-packaging#third-party-plugin-builds-are-one-line`
  (`tests/consumer/plugin_template_load.cpp:23`).
- **`packaging.shared_library_build` / `_msvc` /
  `shared_library_zstd_shared_link`** — "shared and static" (doc 17:13) is
  now true on both platforms; the README may say it without an asterisk.
- **`runtime.registry_bootstrap`** — `arbc::register_builtin_kinds` at the
  L6 umbrella; the quickstart snippet's first call.
- **`quality.contributing_doc`** — `CONTRIBUTING.md` exists; the README's
  contributor pointer (`README.md:38-39`) stays as-is.

**Pending, and deliberately not a dependency:** the other incomplete M9
leaves — `surfaces.import`, `runtime.camera_change_damage`,
`runtime.placement_damage_maps_to_device`,
`compositor.bounded_content_tile_clip`, and
`runtime.plugin_default_search_paths` (the last *blocked on this task*).
They gate the **tag** (through `packaging.tag_01`, registered below), not
this task's collateral: they are fixes and additions inside the already-
described surface, and the changelog discipline (`CHANGELOG.md:9-18`) has
each of them landing its own entry with its own commit.

**Downstream:** `packaging.tag_01` (the deferred tag-execution leaf this
refinement registers); `runtime.plugin_default_search_paths`
(`tasks/65-runtime.tji:59`); `m9_release` (`tasks/99-milestones.tji:70-73`).

## What this task is

Prepare every artifact of the v0.1.0 release that can be true *today*, and
write down the ritual for the one step that cannot. Concretely: rewrite
`README.md` around the shipped surface — an install-and-embed quickstart
lifted verbatim from `examples/host-offline/`, pinned by a sync test so it
can never drift from a program CI compiles and runs; bring
`CHANGELOG.md`'s `[Unreleased]` section back to changelog honesty by
backfilling the four consumer-observable changes that landed without
entries; and add the release checklist itself to doc 10 (§ Cutting a
release), whose final, irreversible step — rolling `[Unreleased]` to
`[0.1.0]` and creating the annotated `v0.1.0` tag — is executed by the
registered follow-up leaf `packaging.tag_01` once every other M9 leaf is
complete.

## Why it needs to be done

M9's note scopes v0.1 "against the driving v0.1 consumer: an image editor
with simple brushes" — and every predecessor refinement in this area closed
with the same anti-drift argument: `release_01` must not tag a surface the
project does not keep (version_api.md:144-147, shared_library_build.md:196-199,
shared_library_build_msvc.md:211-215, shared_library_zstd_shared_link.md:200-203).
Today the inverse problem also exists: the *kept* surface is not *stated*.
`README.md` has no quickstart — a would-be embedder finds a contributor
build snippet (`README.md:41-46`) and no `find_package` line, no mention of
`examples/`, `arbc_add_plugin()`, or the testing component. And
`CHANGELOG.md` has drifted from its own discipline: four consumer-observable
commits (8c1c1af `arbc_add_plugin()` + plugin template, 2cbfcbd the
codec+binder plugin-seam widening, f91274d `arbc::register_builtin_kinds`,
b731a3e the host examples) landed no changelog line, so the `[Unreleased]`
section no longer names the surface the tag will name. Release notes built
on that section would be dishonest by omission. This task closes both gaps
and leaves the tag itself owned by a leaf that runs last.

## Inputs / context

### Design docs (normative, doc 16)

- `docs/design/16-sdlc-and-quality.md:143-148` — the versioning/release
  policy: "semver from the first tag; pre-1.0 moves freely with changelog
  honesty (`CHANGELOG.md`, kept by hand, one entry per landed change)."
- `docs/design/16-sdlc-and-quality.md:88-90` — test tier 10: "every code
  sample in docs and the plugin template compiles and runs in CI … A README
  example that doesn't build is a bug." This is the constraint that shapes
  the quickstart mechanism (Decision D3).
- `docs/design/16-sdlc-and-quality.md:14-21` — claims-register mechanics;
  `tests/claims/registry.tsv:1-3` for the id/format rules.
- `docs/design/10-tooling-and-packaging.md:55-106` — § Versioning and the
  version API (version_api's delta); :102-106 is the CHANGELOG paragraph
  this task's doc-10 delta extends with the release ritual.
- `docs/design/10-tooling-and-packaging.md:32-35` — the dependency-policy
  promise ("embedding the core must never transitively impose codecs, GPU
  SDKs, or a GUI toolkit") the README should state — it is the pitch's
  sharpest line and already proven by `tests/consumer/core_metadata.cpp`.

### Source seams

- `README.md:1-70` — pitch (1-28, keep), Status + contributor snippet
  (30-46, rewrite per D4), design-doc table (48-70, keep).
- `CHANGELOG.md:9-24` — the in-file discipline and the `[Unreleased]`
  framing ("this section describes what 0.1.0 ships, not a diff");
  :26-68 — the `### Added` group the backfill extends.
- `examples/host-offline/main.cpp:37-120` — the embedding program the
  quickstart snippet is lifted from; `examples/host-offline/CMakeLists.txt:22-26`
  — the four-line consume recipe (`find_package(arbc CONFIG REQUIRED)` +
  `target_link_libraries(… arbc::arbc)`).
- `tests/consumer/host_example_artifacts.cpp:230` — the existing claim that
  makes the lifted snippet "CI-run by proxy".
- `scripts/check_claims.py:33-37` — `enforces:` tags are scanned only in
  `.cpp`/`.hpp` under `src/`, `tests/`, `testing/`: the sync check must be
  a C++ test, not a gate script (D3).
- `tasks/65-runtime.tji:56-61` — `plugin_default_search_paths` depends on
  this task: the WBS itself schedules consumer-observable surface *after*
  `release_01` and *before* M9 closes (D1's decisive fact).
- `git tag -l` is empty; `scripts/unblocked.py` reports M9 at 134/140
  leaves as of 2026-07-16.

## Constraints / requirements

1. **No tag is created and no release is claimed by this task.** Every
   sentence this task lands must be true at its own commit and stay true
   until `packaging.tag_01` runs (D1, D4).
2. **The README quickstart must be code CI compiles and runs** (doc
   16:88-90). Byte-identity with anchored regions of
   `examples/host-offline/` is the mechanism (D3); the anchors are
   comment-only edits and must not perturb
   `tests/consumer/host_example_artifacts.cpp`'s byte-exact PNG validation.
3. **Changelog entries follow the in-file discipline** (`CHANGELOG.md:9-18`):
   surface descriptions folded into `[Unreleased]`'s existing groups, not
   per-commit transcriptions; the section heading stays `[Unreleased]` (D2).
4. **Version literals**: no new machine-consumed version literal anywhere —
   `project(VERSION …)` remains the single source (doc 10:62-67); prose
   references to "0.1.0" in README/CHANGELOG are fine and already exist.
5. **No new dependencies, no levelization impact** (docs 10/17): the task
   touches markdown, comment anchors in `examples/`, one new test under
   `tests/`, and `docs/design/10-*.md`. Nothing under `src/` changes.
6. **Same-commit doc rule** (doc 16:23-25): the doc-10 § Cutting a release
   delta rides the task's commit.

## Acceptance criteria

- **README.md carries a quickstart an embedder can follow end-to-end**:
  install from source (plain `cmake -B … && cmake --build … && cmake
  --install …`, not the contributor preset), the four-line consume recipe
  byte-identical to `examples/host-offline/CMakeLists.txt`'s anchored
  region, an embedding snippet byte-identical to a contiguous anchored
  region of `examples/host-offline/main.cpp` (registry bootstrap → document
  → `render_offline`), a pointer to the full program and to
  `examples/host-interactive/` and `examples/plugin-template/`, the
  `arbc_add_plugin()` one-liner, the `COMPONENTS testing` mention, and the
  doc 10:32-35 dependency promise. The design-doc table (`README.md:51-70`)
  survives; the contributor pointer to `CONTRIBUTING.md` survives.
- **Claims-register entry**
  `16-sdlc-and-quality#readme-quickstart-is-the-shipped-example` in
  `tests/claims/registry.tsv`: *README's quickstart code blocks are
  byte-identical to anchored regions of `examples/host-offline/`, which CI
  configures, builds, and runs against a staged install — so the README
  example cannot drift from a building, running program.* Enforced by a new
  Catch2 test (e.g. `tests/readme_quickstart.t.cpp`, source paths injected
  via compile definition, `// enforces:` tag in the `.cpp` per
  `scripts/check_claims.py:33-37`'s scan scope) that extracts the README's
  fenced quickstart blocks and byte-compares them to the anchored regions.
- **CHANGELOG.md's `[Unreleased]` names the full shipped surface**: entries
  folded in for `arbc_add_plugin()` + the plugin template (8c1c1af), the
  plugin-seam codec + binder widening (2cbfcbd),
  `arbc::register_builtin_kinds` (f91274d), and the host examples
  (b731a3e). The heading remains `[Unreleased]` (D2); the section is, as of
  this commit, release-notes-ready.
- **Doc 10 delta**: a § *Cutting a release* subsection after doc
  10:102-106 stating the ritual — (1) every leaf of the release milestone
  complete per `scripts/unblocked.py`; (2) roll `[Unreleased]` →
  `## [X.Y.Z] - <date>` and open a fresh empty `[Unreleased]`; (3) flip the
  README release-status sentence; (4) `git tag -a vX.Y.Z` on the rolled
  commit, tag message = the rolled changelog section; (5) pushing the tag
  and any publication is a human step; (6) the next cycle's version bump is
  the one line in `project(VERSION …)`. No doc-00 bullet (mechanism under
  doc 16:143-148, precedent version_api D9).
- **Deferred follow-up (closer registers as a real WBS leaf, wired into M9
  `m9_release`'s `depends`):** `packaging.tag_01` — "Cut the v0.1.0 tag",
  0.5d, `depends packaging.release_01, surfaces.import,
  runtime.plugin_default_search_paths, runtime.camera_change_damage,
  runtime.placement_damage_maps_to_device,
  compositor.bounded_content_tile_clip` (every currently-incomplete M9 leaf;
  its implementer re-verifies with `scripts/unblocked.py` at execution time
  in case later tech-debt registration grew M9). Deliverables: execute doc
  10 § Cutting a release steps 1-4 for `v0.1.0` — roll the changelog, flip
  the README sentence, create the annotated tag on the rolled commit.
  Pushing the tag is a human action (parking lot, not WBS).
- **Deliberate reading of doc 16's taxonomy**: no goldens, behavioral
  counters, conformance runs, or TSan scope — this task ships documentation
  and one structural test; tier 10 (docs/examples compile and run in CI) is
  the applicable tier, discharged by the new sync claim riding on the
  existing `#shipped-examples-compile-and-run-in-ci` claim.
- **Diff coverage ≥90% on changed lines**: the only coverable lines are the
  new test's; markdown and design docs are outside the coverage universe.
- **WBS gate**: `scripts/gate` green;
  `tj3 project.tjp 2>&1 | grep -iE "error|warning"` silent after the
  closer's `.tji` edits (this task's `complete 100` + note back-link, and
  the `tag_01` registration).

## Decisions

**D1. Two-phase release: this task ships the collateral and the checklist;
a registered leaf `packaging.tag_01` executes the tag, last.**
*Rationale:* the WBS itself forbids tagging here —
`runtime.plugin_default_search_paths` (`tasks/65-runtime.tji:59`) *depends
on* `release_01` and is consumer-observable surface, and four more M9
leaves are open, three of them wrong-output fixes M9's note calls
"correctness gaps, not polish". Every packaging predecessor closed on the
rule that `release_01` must not tag a surface the project does not keep;
tagging today would tag known wrong-output bugs. Splitting the irreversible
step into a leaf that depends on every other incomplete M9 leaf makes the
sequencing a scheduler fact instead of a hope.
*Rejected:* tagging now (contradicts the anti-drift rule and M9's scoping);
blocking this task until it is last (deadlock — `plugin_default_search_paths`
is blocked on *it*); making the tag a milestone-close ritual with no owning
leaf (invisible to the orchestrator's pick pass — the closer of, say,
`compositor.bounded_content_tile_clip` follows *that* task's refinement,
not doc 10's release section; an unowned step is a step that doesn't
happen).

**D2. `[Unreleased]` is finalized but not rolled; the roll is `tag_01`'s
first step.** *Rationale:* the remaining M9 leaves each land their own
changelog line per the in-file discipline; rolling now would either strand
those lines in a new `[Unreleased]` needing a re-merge at tag time, or
mean editing an already-"released" section — a heading lying about
finality. version_api D6 already decided the section *is* the release
notes ("writing it twice would be the real waste"); the annotated tag
message carries the rolled section verbatim.
*Rejected:* a separate `RELEASE_NOTES.md` (duplicates the changelog against
D6); rolling now (above); generating notes from git log (the discipline
explicitly distinguishes the shipped-surface record from the commit
record, `CHANGELOG.md:14-16`).

**D3. The quickstart is lifted byte-identically from
`examples/host-offline/` via comment anchors, pinned by a claims-registered
sync test.** *Rationale:* doc 16:88-90 demands README samples compile and
run in CI. The example already *is* compiled and run against a staged
install on every lane, with byte-exact output validation — byte-identity
transfers that proof to the README for the cost of one small test, instead
of building a second extraction-and-compile harness that would re-prove
what `install.consumer` proves. The snippet is a contiguous anchored region
(bootstrap → document → render) so it stays readable; the sRGB8-encode +
PNG tail is referenced by path. The test is a `.cpp` under `tests/` because
`scripts/check_claims.py:33-37` scans `enforces:` tags only in
`.cpp`/`.hpp` under `src/`, `tests/`, `testing/`.
*Rejected:* extracting and compiling the README block directly (a second
build harness, duplicated proof); a prose-only README pointing at
`examples/` (examples.md:90-91 planned for a lifted snippet, and a library
README without a visible embedding is a worse pitch than the project
deserves); syncing by convention without a test (drift is exactly the
failure mode doc 16:90 names a bug).

**D4. The README stays truthful at this commit: rewritten around the
shipped surface, but its release-status sentence continues to read
"the first tag will be v0.1.0"; `tag_01` flips that one sentence.**
*Rationale:* claiming a release before the tag exists is false for the
whole window between this commit and `tag_01`'s — a window the WBS
guarantees is non-empty (D1). One sentence of latency beats a false README.
*Rejected:* writing "v0.1.0 is released" now (false until tagged);
leaving the whole current Status block untouched (it under-claims a built,
CI-verified, installable library and hides the quickstart this task
exists to write).

**D5. The release ritual is documented in doc 10 and nothing is
automated.** *Rationale:* one release is being cut, pre-1.0; the ritual is
five short steps, four of them one-liners, and `scripts/unblocked.py`
already provides the readiness check. Automation (a `scripts/cut_release`,
a tag-triggered CI workflow) would itself need tests and has exactly one
consumer today; install.md D7 already decided the CPack TGZ is not a gated
deliverable, so a tag-triggered workflow would have nothing to publish.
Doc 10 is the right home — it owns the versioning mechanism section this
extends (precedent: version_api D9); `CONTRIBUTING.md` links rather than
duplicates.
*Rejected:* `scripts/cut_release` automation and a GitHub release workflow
(above); putting the ritual only in this refinement (refinements are task
records, not the constitution — the *next* release must find the ritual in
the docs).

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-16.

- `README.md`: rewritten with install-from-source quickstart, four-line consume recipe (`find_package(arbc CONFIG REQUIRED)` + `target_link_libraries`), embedding snippet lifted byte-identically from `examples/host-offline/main.cpp` via anchored regions, pointers to full examples and `arbc_add_plugin()`, COMPONENTS testing mention, and dependency promise; release-status sentence reads "the first tag will be v0.1.0" (truthful until `packaging.tag_01` flips it, per D4).
- `CHANGELOG.md`: `[Unreleased]` backfilled with four missing entries: `arbc_add_plugin()` + plugin template (8c1c1af), codec+binder plugin-seam widening (2cbfcbd), `arbc::register_builtin_kinds` (f91274d), and host examples (b731a3e); section now names the full shipped surface.
- `docs/design/10-tooling-and-packaging.md`: added § *Cutting a release* subsection (six-step ritual; human tag-push step noted as parking-lot, not WBS).
- `examples/host-offline/CMakeLists.txt`, `examples/host-offline/main.cpp`: anchor comments added; format-only fixups to `examples/host-interactive/main.cpp` and `tests/consumer/host_example_artifacts.cpp` (latent clang-format violations, untracked at iter-0041 verify).
- `tests/readme_quickstart.t.cpp` (new): two-case Catch2 test extracting README fenced blocks and byte-comparing to anchored regions; passes locally (22 assertions).
- `tests/CMakeLists.txt`: registered `arbc_readme_quickstart_t` target.
- `tests/claims/registry.tsv`: claim `16-sdlc-and-quality#readme-quickstart-is-the-shipped-example` registered and enforced by the new test.
