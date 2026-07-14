# quality.contributing_doc — CONTRIBUTING.md definition-of-done checklist

## TaskJuggler entry

```tji
task contributing_doc "CONTRIBUTING.md definition-of-done checklist" {
  effort 0.5d
  allocate team
  depends packaging.version_api
  note "Write CONTRIBUTING.md with DoD checklist (code + contract/claims tests + design-doc delta + benchmark clean + examples still building), scripts/gate invocation, claims-register ritual, and changelog discipline. Doc 16:138-142. Source-of-debt: tasks/refinements/packaging/version_api.md (Decision 8)."
}
```

[`tasks/70-quality.tji:48-53`](../../70-quality.tji). Milestone **M9**
(v0.1 release) — `tasks/99-milestones.tji:72` already lists
`quality.contributing_doc` in `m9_release`'s `depends`.

## Effort estimate

**Booked 0.5d; realistic 0.7d.**

- 0.35d — write `CONTRIBUTING.md`: quick start, the gate, the
  definition-of-done checklist, the claims-register ritual, style, the
  red-main protocol, and the *not-yet-mechanized* honesty table.
- 0.1d — `scripts/check_contributing.py` (the anti-drift checker: every
  script, preset, and repo-relative path the document names must resolve)
  and its `docs.contributing` CTest registration.
- 0.1d — close the gate/CI parity drift: `scripts/check_worker_dispatch.py`
  runs in CI's `lint` job but not in `scripts/gate`.
- 0.05d — README pointer + Status-line correction (README still says
  "Design phase. No code yet.").
- 0.1d — gate run, CI-lane sanity, closer ritual.

## Inherited dependencies

**Settled.**

- **`packaging.version_api`** (Done 2026-07-14,
  [`tasks/refinements/packaging/version_api.md`](../packaging/version_api.md)) —
  landed `CHANGELOG.md` with its discipline block, and **Decision 8**
  (`version_api.md:496-513`) is this task's charter: the changelog rule
  lives in the changelog's own header, and the full `CONTRIBUTING.md` was
  registered as `quality.contributing_doc` rather than smuggled into a
  0.5d version task. The registration text (`version_api.md:361-373`)
  enumerates exactly what this file owes: the DoD checklist, the
  `scripts/gate` invocation, the claims-register ritual, and the changelog
  rule.
- **`quality.testing_artifact`** (Done 2026-07-14) — `arbc-testing` is a
  real, installed artifact, so the DoD's "contract/claims tests" line can
  name a concrete suite instead of a future one. Its **D5**
  (`testing_artifact.md:464-470`) also settles *where* a check gets
  registered: a CTest test runs in both `scripts/gate` and CI with one
  registration — the pattern `docs.contributing` follows.
- **`quality.stress_harness`** (Done 2026-07-05) — its **D5**
  (`stress_harness.md:360-369`) settles the claims-register question this
  task would otherwise have to re-litigate: process guarantees ("the lane
  is green") belong in CI structure, not the behavioral claims register.

**Pending, and deliberately not a dependency.**

- **`quality.repo_linters`** (`tasks/70-quality.tji:33-37`, M10) —
  markdownlint, the doc link checker, `typos`, `shellcheck`/`shfmt`,
  `gersemi`/`cmake-lint`. Doc 16:203-218 promises all of them; none are
  wired today. `CONTRIBUTING.md` must therefore *not* claim they run, and
  must not wait for them either (see Decision 5).
- **`packaging.examples`** (`tasks/75-packaging.tji:23-27`) — the DoD's
  "examples still building" line names a directory that does not exist
  yet. The checklist item is written now and marked *not applicable until
  `packaging.examples` lands* (Decision 4).
- **`quality.tidy_promotion`** (M10) — clang-tidy is nightly and
  `continue-on-error` (`.github/workflows/nightly.yml:16`), not a gate.

**Downstream.** `packaging.release_01` (`tasks/75-packaging.tji:32`,
`depends !install, !version_api, !examples`) ships the 0.1 tag; a repo
that tags a release with no contributor-facing build instructions and a
README claiming "no code yet" is not a release-shaped repo.

## What this task is

Write the `CONTRIBUTING.md` that doc 16:138-142 has promised since the
design phase (*"`CONTRIBUTING.md` carries it"*) and that does not exist.
It carries the **definition of done** — code + contract/claims tests +
design-doc delta + benchmark run clean + examples still building — plus
the four things a person needs before they can satisfy it: how to build
and test, how to run `scripts/gate` and what it checks, how the
claims-register ritual works (`tests/claims/registry.tsv` +
`// enforces:`), and where the changelog rule lives.

Two small pieces of engineering ride along, because a checklist that
points at a lie is worse than no checklist. First, `scripts/gate` is
missing `scripts/check_worker_dispatch.py`, which CI's `lint` job does run
(`.github/workflows/ci.yml:37-38`) — so a gate-green tree can still fail
CI, and doc 16:97-100's *"CI runs the same steps"* is currently false.
Second, `scripts/check_contributing.py` + a `docs.contributing` CTest pin
the document to the tree: every `scripts/<name>` it names must exist and
be executable, every `--preset <name>` must exist in `CMakePresets.json`,
and every repo-relative link must resolve.

## Why it needs to be done

Doc 16's opening philosophy (`docs/design/16-sdlc-and-quality.md:8-26`) is
that the design docs are an *executable* specification and that aspirational
documentation is the failure mode to design against. A file the constitution
names by path, assigns a job to, and never writes is precisely that failure
mode — and it is the one remaining unwritten promise in doc 16's "Physical
design and maintainability" section.

Concretely: `version_api` (D8) deferred here rather than shipping a token
file; M9 lists this task in its `depends`; and the project is about to tag
0.1 with a README whose Status section reads *"Design phase. No code
yet."* (`README.md:32`) above seventeen built components, a nine-lane CI
matrix, and a 295-row claims register. The build/test/gate knowledge that
should be in `CONTRIBUTING.md` currently exists only inside the
orchestrator's agent prompts (`orchestrator/prompts/implementer.md:21,126`,
`orchestrator/prompts/closer.md:18,194`) — readable by the agents, invisible
to a human.

## Inputs / context

### Design docs (normative)

- **`docs/design/16-sdlc-and-quality.md:138-142`** — the definition of done,
  verbatim: *"code + contract/claims tests + design-doc delta + benchmark run
  clean (or a justified entry in the history) + examples still building.
  Mechanized where possible, checklist where not (`CONTRIBUTING.md` carries
  it; solo discipline now, contributor onboarding later)."* This is the
  document's table of contents.
- **`docs/design/16-sdlc-and-quality.md:96-100`** — *"Local pre-push gate
  (also runnable as `ctest` presets + a `scripts/gate` wrapper): build +
  tiers 1–5 in Debug+ASan on the developer's compiler."* The "same steps"
  clause is what the `check_worker_dispatch.py` omission violates; the
  "Debug+ASan" clause is addressed by Decision 3.
- **`docs/design/16-sdlc-and-quality.md:112-118`** — diff coverage is a hard
  ≥90% gate on changed lines, *"in per-push CI and in the local gate script"*.
  The CI half exists (`.github/workflows/ci.yml:143`, `--fail-under=90`); the
  local half does not (Decision 6, deferred to a named task).
- **`docs/design/16-sdlc-and-quality.md:106-111`** — the red-main protocol
  (*"fix forward within the hour or revert; either is one command and never
  shameful"*), which belongs in a contributor doc verbatim in spirit.
- **`docs/design/16-sdlc-and-quality.md:27-91`** — the test taxonomy: the
  contract conformance suite is tier 1, and the claims register is the
  mechanism that keeps doc promises falsifiable.
- **`docs/design/16-sdlc-and-quality.md:156-218`** — *"the configuration files
  are the style guide"*, the pinned-clang-format rule, and the repo-wide lint
  roster that is **not yet wired** (`quality.repo_linters`).
- **`docs/design/16-sdlc-and-quality.md:223-226`** — wall-clock performance
  gates stay out of the merge path; the DoD's "benchmark clean" line must not
  be written as a merge gate.
- **`docs/design/17-internal-components.md:57,155,191`** — levelization is a
  build error, not a review comment; `scripts/check_levels.py` is its enforcer.

### Source seams

- `scripts/gate:1-47` — the seven checks, in order: configure (`:22-23`),
  build (`:25-26`), `ctest` (`:28-29`), `clang-format --dry-run -Werror`
  (`:31-36`, soft-skips when clang-format is absent), `check_levels.py`
  (`:38-39`), `check_claims.py` (`:41-42`), `check_rt_safety.py` (`:44-45`).
  Env: `ARBC_GATE_PRESET` (default `dev`, `:20`) and `CXX` (auto-probed,
  `:10-18`). **`scripts/check_worker_dispatch.py` is absent.**
- `.github/workflows/ci.yml:11-38` — the `lint` job runs five checks
  including `check_worker_dispatch.py` (`:37-38`); `:40-107` — the eight-lane
  build/test matrix; `:109-143` — the coverage job and the `--fail-under=90`
  diff gate.
- `.github/workflows/nightly.yml:14-138` — `tidy` (continue-on-error),
  `asan-full`, `tsan-full` (including the hidden `[.nightly]` seed sweeps,
  `:84-90`), and `fuzz`.
- `CMakePresets.json:6-90` — the ten configure presets: `dev`, `release`,
  `asan`, `tsan`, `rtsan`, `coverage`, `fuzz`, `install-fetched`, `win-dev`,
  `bench`.
- `tests/claims/registry.tsv:1-3` — the format: `<claim-id>\t<description>`,
  id is `<doc-file-stem>#<slug>`; 295 claims today.
- `scripts/check_claims.py:15,29,32-42` — the tag regex
  (`enforces:\s*([A-Za-z0-9#_./-]+)`), the `.cpp`/`.hpp` scan over `src/`,
  `tests/`, `testing/`, and the **bidirectional** failure modes
  (registered-but-unenforced, enforced-but-unregistered).
- `CHANGELOG.md:9-18` — the changelog discipline block, landed by
  `packaging.version_api`; explicitly *"kept by hand — there is no CI check,
  deliberately"*.
- `tests/CMakeLists.txt:1079-1096` — the `install.consumer` `add_test(...)`
  registration; the shape `docs.contributing` copies.
- `CMakeLists.txt:70-73` — `ARBC_BENCHMARKS` is OFF by default; the `bench`
  preset turns it on. Benchmarks exist today
  (`src/pool/CMakeLists.txt:16`, `src/runtime/CMakeLists.txt:71`) with
  `bench_smoke` CTests carrying their diff coverage in the normal build.
- `README.md:30-32` — *"## Status / Design phase. No code yet."*, and no
  build, test, or gate command anywhere in the file.
- `tasks/refinements/README.md:47-78` — the task-completion ritual
  (refinement Status + `complete 100` + milestone propagation + tech-debt
  registration) that `CONTRIBUTING.md` links to rather than restates.

## Constraints / requirements

1. **`CONTRIBUTING.md` describes the tree as it is, not as doc 16 wishes it
   were.** Every command it prints must run today; every check it claims runs
   must actually run. Doc 16's unwired promises (markdownlint, `typos`,
   `shellcheck`, blocking clang-tidy, local diff coverage) appear only in an
   explicit *not yet mechanized* section, each naming the WBS leaf that will
   land it. A contributor doc that overstates the gate is the same drift
   failure as an overstating design doc.

2. **Single-source every rule; the document routes, it does not duplicate.**
   The changelog discipline lives in `CHANGELOG.md:9-18` (version_api D8), the
   completion ritual in `tasks/refinements/README.md:47-78`, the style rules
   in `.clang-format` and the tool configs (doc 16:158-162, *"the configuration
   files are the style guide"*), the levelization table in doc 17. Restating any
   of them creates a second copy that drifts. `CONTRIBUTING.md` states the
   *rule in one line* and links to the owner.

3. **The gate must run what CI's `lint` job runs.** Add
   `scripts/check_worker_dispatch.py` to `scripts/gate` (doc 16:97-100).

4. **No new dependency.** The anti-drift checker is stdlib Python 3, matching
   `check_claims.py` / `check_levels.py` / `check_rt_safety.py`; doc 10's
   dependency policy is not engaged.

5. **No C++ source changes; no component graph changes.**
   `scripts/check_levels.py` must pass unchanged — this task does not touch
   `src/`, so doc 17's levelization edges are untouched by construction.

6. **The DoD checklist is copyable.** It is written as a markdown task list a
   person can paste into a PR body or a scratch buffer, with each item naming
   its mechanism (the command that proves it, or "checklist — judgment") —
   doc 16:140-141's *"mechanized where possible, checklist where not"* made
   literal rather than paraphrased.

## Acceptance criteria

- **`CONTRIBUTING.md` exists at the repo root**, covering, in order:
  (a) prerequisites + quick start (`cmake --preset dev`,
  `cmake --build --preset dev`, `ctest --preset dev`);
  (b) **the gate** — `scripts/gate`, its checks in order, `ARBC_GATE_PRESET`
  and `CXX`, and the preset table from `CMakePresets.json`;
  (c) **the definition of done** as a copyable checklist covering all five
  doc 16:138-142 items plus the changelog entry, the ≥90% diff coverage gate,
  and (for WBS-tracked work) the refinement/`complete 100` ritual;
  (d) the **claims-register ritual** — what a claim is, the
  `<doc-stem>#<slug>` id, the `registry.tsv` row, the `// enforces:` tag above
  the `TEST_CASE`, and the bidirectional check;
  (e) **style** — the configs are the style guide; clang-format is pinned to
  19.1.7 (`.github/workflows/ci.yml:19`); reformat commits are isolated;
  (f) the **red-main protocol** (doc 16:106-111);
  (g) **not yet mechanized** — the doc 16 lint/coverage promises that do not
  run today, each naming its WBS leaf (`quality.repo_linters`,
  `quality.tidy_promotion`, `quality.gate_diff_coverage`).

- **New CTest `docs.contributing`** (registered in `tests/CMakeLists.txt`
  beside `install.consumer:1079-1096`) running
  `scripts/check_contributing.py`, which **fails** when `CONTRIBUTING.md`
  names: a `scripts/<name>` that does not exist or is not executable; a
  `--preset <name>` absent from `CMakePresets.json`; or a repo-relative
  markdown link target that does not exist. Pinned by three negative
  fixtures in the checker's own self-test (a bogus preset, a bogus script, a
  bogus link each produce a non-zero exit) — doc 16:196-197 requires the lint
  scripts directory to have its own tests. Registering it as a **CTest** and
  not a CI-only step means `scripts/gate` and CI run the same check, per
  `testing_artifact.md:464-470` (D5).

- **Gate/CI parity restored.** `scripts/gate` runs
  `scripts/check_worker_dispatch.py`; the gate's structure checks are then
  exactly the CI `lint` job's five (`ci.yml:20-38`) plus the CTest suite.
  Verified by `scripts/gate` green on a tree that CI's `lint` job also passes.

- **`README.md` no longer lies.** The `## Status` section (`README.md:30-32`)
  states the real state (implementation underway, pre-0.1) and links to
  `CONTRIBUTING.md` for building, testing, and the gate. The design-doc index
  table stays.

- **No new claims-register row.** Per `stress_harness.md:360-369` (D5): a
  claim is a *behavioral* promise a design doc makes about the library, and
  "the contributor doc names real presets" is a process guarantee, enforced by
  CI structure. `scripts/check_claims.py` passes unchanged.

- **No new golden, conformance-suite, behavioral-counter, TSan, or stress
  coverage** — and that is a deliberate reading of doc 16's taxonomy, not an
  omission. This task lands no content kind, no operator, no render path, no
  concurrency, and no performance-shaped promise; there is no runtime behavior
  to pin. The anti-drift CTest is the coverage that fits.

- **Diff coverage: not engaged.** The coverage job filters `src/`
  (`ci.yml:109-143`); this task changes markdown, Python, Bash, and one
  `add_test` line, so there are no changed C++ lines for the ≥90% gate to
  measure. The gate is not bypassed — it has nothing to measure.

- **Gate green + WBS gate.** `scripts/gate` passes; `tj3 project.tjp 2>&1 |
  grep -iE "error|warning"` is silent after the `.tji` `complete 100` and the
  refinement back-link land.

- **No design-doc delta** (Decision 7). This task *discharges* doc
  16:138-142; it does not amend it.

- **Deferred to `quality.gate_diff_coverage`** (closer registers as a real
  WBS leaf): *0.5d, `depends quality.contributing_doc`, milestone **M10**.*
  Add an opt-in `ARBC_GATE_COVERAGE=1` path to `scripts/gate` that configures
  and builds the `coverage` preset, runs `gcovr` + `diff-cover` against the
  merge-base with `--fail-under=90`, mirroring `ci.yml:109-143`, and flip
  `CONTRIBUTING.md`'s "not yet mechanized" row to a documented knob. Doc
  16:115-118 promises the diff gate runs *"in the local gate script"*; it does
  not. M10 because it is continuous-quality automation, the group
  `tasks/99-milestones.tji:79` already defines. See Decision 6 for why it is
  not in this task.

- **Deferred to `quality.repo_linters`** (existing leaf,
  `tasks/70-quality.tji:33-37`, M10 — closer registers **nothing** new):
  markdownlint, the doc link checker, `typos`, `shellcheck`/`shfmt`. When it
  lands, its generic link checker subsumes the link-resolution half of
  `check_contributing.py`; the preset/script-existence half has no generic
  equivalent and stays.

## Decisions

**D1. `CONTRIBUTING.md` routes; it does not restate.**
*Rationale:* four owners already exist for the rules this file must convey —
`CHANGELOG.md:9-18` (changelog), `tasks/refinements/README.md:47-78`
(completion ritual), the tool configs (style, doc 16:158-162), doc 17 (the
component graph). Duplicating any of them buys nothing and guarantees a
divergence the moment one side is edited. The file states each rule in one
sentence and links to its owner; only the DoD checklist — which has no
owner today, which is exactly why this task exists — is authored in full
here.
*Rejected: a self-contained handbook.* It reads better on day one and is
wrong by month three. The project's whole quality thesis (doc 16:8-26) is
that documentation which restates rather than enforces drifts.

**D2. Close the gate/CI drift in this task rather than registering it.**
*Rationale:* `scripts/check_worker_dispatch.py` runs in CI's `lint`
(`ci.yml:37-38`) and not in `scripts/gate` — so a tree can be gate-green and
CI-red, and doc 16:97-100's *"CI runs the same steps"* is false. This
document's single most important instruction is *run `scripts/gate` before you
push*; shipping that instruction while knowing the gate is incomplete would
put the exact lie this task exists to remove into the file's most-read line.
The fix is two lines of Bash.
*Rejected: register `quality.gate_worker_dispatch`.* A separate WBS leaf for a
two-line omission is ceremony, and it would leave `CONTRIBUTING.md` shipping a
known-wrong claim in the interim.

**D3. The gate stays single-preset; ASan is a documented knob, not a second
build.**
*Rationale:* doc 16:97-98 describes the local gate as *"build + tiers 1–5 in
Debug+ASan"*, and `scripts/gate:20` runs one preset (`dev` by default). Making
the gate always build twice would roughly double the wall-clock of the single
most frequently run command in the project — and a slow gate is a gate people
stop running, which loses more than the ASan lane gains, especially when
`clang-asan` runs on every push anyway (`ci.yml:53`). `CONTRIBUTING.md`
documents `ARBC_GATE_PRESET=asan scripts/gate` as the DoD branch for changes
that touch memory or concurrency, which honors the doc's intent at the
decision point where it matters.
*Rejected: `scripts/gate` runs `dev` then `asan` unconditionally.* Correct on
paper, corrosive in practice.
*Rejected: a doc-16 delta softening the "Debug+ASan" line.* The doc's intent —
sanitizers before push — is right; the knob satisfies it.

**D4. The two DoD items whose machinery does not exist yet are written now and
marked, not omitted.**
*Rationale:* "examples still building" names `packaging.examples`
(`tasks/75-packaging.tji:23-27`), which has not landed; "benchmark run clean"
names benchmarks that *do* exist (`ARBC_BENCHMARKS` /
`cmake --preset bench`, `CMakeLists.txt:70-73`) but have no tracked history
(`quality.benchmark_history`, M10). Omitting the two items would silently
shrink doc 16:138-142's checklist to three; writing them with an explicit
*not applicable until `<task>` lands* marker keeps the checklist complete and
makes the gap visible to the person who lands the enabling task.
*Rationale (benchmarks):* the item is phrased as *run it and look, or justify
in the entry* — never as a merge gate, per doc 16:224-226 (*wall-clock gates
stay out of the merge path*).

**D5. The "not yet mechanized" section is a feature of the document, not an
embarrassment in it.**
*Rationale:* doc 16:203-218 promises markdownlint, a link checker, `typos`,
`shellcheck`/`shfmt`, `gersemi`/`cmake-lint`, and a blocking clang-tidy. None
run today (`quality.repo_linters`, `quality.tidy_promotion` — both M10). A
`CONTRIBUTING.md` that quietly implies they do would teach a new contributor
that the gate covers ground it does not, and would let the gap age invisibly.
Naming each unwired promise beside its WBS leaf converts a silent lie into a
tracked debt — and gives the person who lands `repo_linters` a checklist of
rows to delete.
*Rejected: describe the intended lint suite as if it ran.* Aspirational docs
are the failure mode doc 16:8-26 is written against.

**D6. Local diff coverage is deferred, not attempted here.**
*Rationale:* doc 16:115-118 wants the ≥90% diff gate in the local gate script.
Doing it properly means a second full build under `--coverage`, `gcovr`, a
`diff-cover` run against the merge-base, and graceful behavior when no base
exists — that is a task-sized piece of work (0.5d) with a real design question
(default-on and slow, or opt-in and skippable), and folding it into a 0.5d doc
task would either blow the estimate or produce a half-wired knob. Registered as
`quality.gate_diff_coverage` with an opt-in `ARBC_GATE_COVERAGE=1` default; the
CI gate (`ci.yml:143`) holds the floor meanwhile.
*Rejected: implement it here, default-on.* Same corrosion as D3 — the gate
becomes slow enough to skip.
*Rejected: leave it undocumented.* Then the CONTRIBUTING reader believes the
gate covers coverage; the "not yet mechanized" row is what keeps the promise
honest.

**D7. No design-doc delta.**
*Rationale:* this task implements doc 16:138-142 exactly as written — the doc
names the file, assigns it the checklist, and this is that file. Nothing in
the constitution is amended, contradicted, or extended: the gate parity fix
makes 16:97-100 *true* rather than changing it, and the deferred local
coverage gate leaves 16:115-118 stated as-is with a registered task to satisfy
it. Doc 16's same-commit rule applies to changes that alter designed
behavior; discharging a promise is not altering one.
*Rejected: a doc-00 decision bullet.* The decision ("CONTRIBUTING carries the
DoD") was recorded in doc 16 at design time; re-recording it on execution
would make the decision record a build log.

**D8. The anti-drift checker is scoped to `CONTRIBUTING.md`'s *commands*, not
to markdown quality.**
*Rationale:* the failure this file is exposed to is not bad prose — it is a
preset renamed in `CMakePresets.json`, a check script moved, or a link rotted,
leaving the document confidently wrong. `scripts/check_contributing.py` checks
exactly those three things and nothing else, in ~50 lines of stdlib Python,
consistent with the existing `check_*.py` family. General markdown lint and
repo-wide link checking are already owned by `quality.repo_linters`; building
a second, competing markdown linter here would create the duplicate-owner
problem D1 exists to prevent.
*Rejected: no checker at all — "it's just a doc".* This is the one document in
the tree whose entire value is that its commands work. A doc that promises
mechanization while being itself unmechanized would be self-refuting.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-14.

- `CONTRIBUTING.md` created at repo root: quick-start, gate, DoD checklist (all five doc-16:138-142 items + changelog + diff-coverage + completion ritual), claims-register ritual, style, red-main protocol, and *not-yet-mechanized* section naming `quality.repo_linters`, `quality.tidy_promotion`, `quality.gate_diff_coverage`.
- `scripts/check_contributing.py` created: stdlib-Python anti-drift checker verifying every preset, script, and repo-relative link named in `CONTRIBUTING.md`; exits non-zero on any missing target.
- `scripts/gate` updated: adds `check_worker_dispatch.py` call (gate/CI `lint`-job parity, D2); gate and CI now run identical structure checks.
- `tests/CMakeLists.txt` updated: registers `docs.contributing` (runs `check_contributing.py`) and `docs.contributing.selftest` (checker's `--self-test` with three negative fixtures + control) beside `install.consumer`.
- `README.md` updated: `## Status` section replaced — "Design phase. No code yet." → real project state + build/test/gate commands + link to `CONTRIBUTING.md`.
- No claims-register row added (process guarantee per `stress_harness` D5); no goldens, counters, or C++ changes.
- Tech-debt follow-up registered as `quality.gate_diff_coverage` (0.5d, M10): opt-in `ARBC_GATE_COVERAGE=1` path in `scripts/gate` mirroring `ci.yml:109-143`; flips `CONTRIBUTING.md`'s "not yet mechanized" coverage row to a documented knob.
