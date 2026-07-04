# 16 — SDLC and Quality

How the project is built, tested, and kept maintainable. Parameters chosen:
GitHub-hosted, trunk-based development with local gates, hard diff-coverage
requirement. The premise throughout: quality infrastructure is cheapest at
commit zero — the harness exists *before* the first feature code.

## Philosophy: the design docs are an executable specification

Docs 00–15 make dozens of falsifiable promises: "static layers' tiles
remain valid across clock advance" (doc 11), "rebasing changes the composed
mapping by less than one rounding" (doc 04), "a pinned version never
observes a later edit" (doc 14), "unknown kinds round-trip byte-equivalent"
(doc 08), "release enqueues, never destroys inline" (doc 15). The central
quality discipline is a **claims register**: each normative claim in the
design docs gets an identifier and a test that enforces it, referenced from
the test (`// enforces: 11-time-and-video#static-tiles-survive-clock`).
CI fails if a registered claim has no live test. This is what keeps the
docs *normative* instead of aspirational as the code ages — the docs and
the tests can't drift apart silently, and a change that breaks a design
promise fails loudly with a pointer to the paragraph it violated.

Corollary discipline: a change that alters designed behavior updates the
design doc *in the same commit*. The docs are the constitution; commits
amend it explicitly or comply with it.

## Test taxonomy

Ordered by how much of the project's correctness each tier carries:

1. **The contract conformance suite** (the crown jewel, doc 10 sketch
   expanded). A reusable, property-based suite that any `Content`
   implementation — core, reference kind, or third-party plugin — runs
   against: render purity (same pinned state + request ⇒ same pixels),
   scale/time honesty (`achieved_*` never overclaims), bounds/extent
   honesty (nothing rendered outside declared bounds), damage soundness
   (undamaged regions bit-identical across edits), capture/restore
   round-trips, async completion and cancellation behavior, facet
   consistency. Property-based generation (randomized regions, scales,
   times, edit sequences under a seed) rather than hand-picked cases —
   the contract is algebraic, so test it algebraically. **Shipped as
   public API**: `arbc::contract_tests(my_content_factory)` is the plugin
   ecosystem's conformance story and the reason plugin quality scales
   without review capacity.
2. **Unit tests** for core machinery (persistent map, transforms, rational
   time, scale ladder, culling, journal) — Catch2, fast, exhaustive on
   edge cases.
3. **Golden rendering tests.** The CPU backend is specified deterministic
   (fixed FP flags, no FMA variance in kernels, ordered reductions), so
   goldens are **byte-exact**, cheap to diff, and regenerate with an
   audited script. Perceptual-tolerance comparison exists only where
   platform libm variance is unavoidable — tolerances are the exception
   and each carries a justifying comment.
4. **Behavioral-counter tests** — the non-flaky performance semantics.
   Wall-clock tests lie in CI; counters don't. The core exposes debug
   counters (render requests issued, cache hits/misses, tiles composited,
   blocks mixed, slots allocated/reclaimed) and tests assert *behavioral*
   claims: playback of a still scene issues zero visual renders; a
   fade at envelope=1 issues zero operator renders (identity); an edit
   deep in a shared child invalidates exactly the embeddings' mapped
   regions; unpinning after export reclaims exactly the delta. Most
   claims-register entries about efficiency land here.
5. **Numeric invariant tests** (doc 04/11): property-based deep-nesting
   and rebase-continuity checks, rational-time exactness across pathological
   rate stacks, degenerate-transform culling (NaN containment).
6. **Concurrency tests.** TSan on the full suite; dedicated stress tests
   for the publish/pin protocol and the reclamation queue with schedule
   perturbation (randomized yields under a seed); litmus tests for the
   arena growth handshake. The audio callback path is guarded by
   **RealtimeSanitizer** (`[[clang::nonblocking]]` on the callback chain)
   plus debug allocator hooks asserting no allocation/refcount/lock on RT
   threads — doc 12's "never on the callback" as a build-failing check,
   not a convention.
7. **Crash-recovery tests** (doc 15): a fault-injection shim over
   msync/write/mmap drives kill-at-every-syscall sweeps through the
   checkpoint protocol; after each injected death, remap and verify a
   consistent root (SQLite's discipline, scaled to our simpler protocol).
   Disk-full and short-file paths included.
8. **Fuzzing**: libFuzzer harnesses for the JSON loader (doc 08 — untrusted
   input by definition) and for request streams against contract
   properties; corpus checked in, run per-commit briefly and nightly long.
9. **Benchmarks** (Google Benchmark): kernels (doc 07), allocator
   (doc 15 — including an honest cpioo-vs-shared_ptr rerun), end-to-end
   scenarios (zoom sweep, playback, edit-during-playback). Results
   uploaded per-commit to a tracked history (`github-action-benchmark`);
   regressions alert, humans judge — wall-clock gates stay out of the
   merge path.
10. **Docs and examples**: every code sample in docs and the plugin
    template compiles and runs in CI (extracted or `#include`d into test
    files). A README example that doesn't build is a bug.

## CI structure (GitHub Actions)

Trunk-based with local gates means CI is the *net*, not the *gate* — so
the net must be fast to signal and loud on red:

- **Local pre-push gate** (also runnable as `ctest` presets + a
  `scripts/gate` wrapper): build + tiers 1–5 in Debug+ASan on the
  developer's compiler. Per the standing rule: build and test before every
  commit — this *is* that rule, mechanized.
- **Per-push CI**: full matrix — GCC/Clang/MSVC × Debug/Release; ASan+UBSan
  and TSan jobs; the debug-hardened build (generation tags, mprotect'd
  published chunks, RT-safety hooks); tiers 1–8 short-form; diff coverage.
- **Nightly**: long-form stress/fuzz/crash-sweeps, full benchmark run,
  Valgrind job, and the claims-register audit.
- **Red-main protocol** (the honest price of trunk-based + hard gates):
  a failing main is the top priority for whoever landed it — fix forward
  within the hour or revert; either is one command and never shameful.
  A `main`-is-red badge state pauses further landings. If contributor
  volume ever makes this unworkable, the escape hatch is merge queues /
  PR gating — a workflow change, not a redesign.
- **Diff coverage: hard gate.** Changed lines require ≥90% coverage
  (llvm-cov + a diff-cover step); the gate runs in per-push CI and in the
  local gate script so it is known *before* pushing. Exclusions
  (`GCOV_EXCL` regions) require a justification comment — defensive
  error paths that are genuinely unreachable get excluded, not gamed
  around. Whole-project coverage is reported and tracked but not gated
  (the diff gate holds the floor where it matters: new code).

## Physical design and maintainability

- **Levelized components, acyclic by construction.** The doc-02 layering
  (model → compositor core → renderers; core / backends / kinds) is
  enforced physically: each component is a CMake target with an explicit
  allowed-dependency list, and a CI check walks include graphs to reject
  cycles and layer violations. Dependency direction is a build error, not
  a review comment.
- **Public API is a deliberate surface**: `include/arbc/` is the only
  thing installed; everything else is `src/`-private. Anything crossing
  into public headers gets doc references and contract-test coverage in
  the same change. Include hygiene (IWYU or clang-based equivalent) runs
  in CI to keep header coupling from ratcheting.
- **Formatting and linting**: see the dedicated section below.
- **Error-handling and API idioms** are already decided (doc 10: no
  exceptions across public/plugin boundaries, `expected`-style results);
  clang-tidy custom checks or grep-based lint enforce the boundary rules
  mechanically where possible.
- **Definition of done** for any feature: code + contract/claims tests +
  design-doc delta + benchmark run clean (or a justified entry in the
  history) + examples still building. Mechanized where possible, checklist
  where not (`CONTRIBUTING.md` carries it; solo discipline now,
  contributor onboarding later).
- **Versioning and compatibility**: semver from the first tag; pre-1.0
  moves freely with changelog honesty (`CHANGELOG.md`, kept by hand, one
  entry per landed change). At 1.0: ABI checking (abi-compliance-checker
  or libabigail) joins CI, deprecation policy is two minor versions with
  attributes, and the doc-03 C-ABI work item activates with its own
  conformance suite.
- **The bootstrap sequence enforces all of this**: commit 1 after this
  doc is CI + the gate script + an empty test suite that passes; commit 2
  is the walking skeleton — a solid-color layer through model →
  compositor → CPU backend → golden test, end to end. Every subsequent
  system (time, audio, operators, editing, arenas) lands *inside* a
  harness that already runs, rather than getting a harness retrofitted.

## Formatting and linting

Guiding rule: **the configuration files are the style guide.** Style lives
in versioned tool configs that machines enforce, not in prose that humans
debate; a style question that can't be encoded in a config is usually not
worth having a rule about. Zero style comments in review is the goal — the
gate already settled them.

**Formatting**

- **clang-format** for all C++, config committed at the repo root. Base
  style chosen once at bootstrap (lean: LLVM base with project deltas kept
  to a handful — column limit, pointer alignment, include categories);
  after that, the config only changes with a reformat commit.
- **Format is a hard gate**: `clang-format --dry-run -Werror` in the local
  gate script and per-push CI. No "format later" state ever exists on main.
- **Reformat commits are isolated and blame-transparent**: any commit that
  only reformats (config change, tool version bump) touches nothing else
  and is listed in `.git-blame-ignore-revs`, so archaeology never trips
  over formatting noise. clang-format version is pinned (same major in CI
  and gate script) because formatters drift between releases.
- **Include order is format-owned**: clang-format's include categories
  encode the convention (own header first, then arbc public, then arbc
  private, then deps, then std), making include hygiene mechanical.

**C++ linting**

- **clang-tidy** with a curated, versioned profile: `bugprone-*`,
  `performance-*`, `concurrency-*`, `modernize-*`, `readability-*` bases
  minus an explicit, justified suppression list (each disabled check gets
  a one-line reason in the config — suppressions are decisions, not
  drive-by). Runs on the compile-commands database in per-push CI;
  `NOLINT` requires a reason suffix, greppable and audited nightly.
- **Naming conventions live in the linter**: `readability-identifier-naming`
  encodes case rules (types, functions, members, constants) so the style
  guide's naming section is a config block, not a document.
- **Boundary rules become custom checks over time**: "no exceptions
  across public/plugin boundaries", "no STL types in hot ABI structs"
  (doc 03), "no allocation in `[[clang::nonblocking]]` call graphs"
  (doc 12) start as grep-based lint scripts in the gate and graduate to
  clang-tidy/clang-query checks as they prove their worth. The lint
  scripts directory is a first-class part of the codebase with its own
  tests.
- Warnings-as-errors across GCC/Clang/MSVC (doc 10) stands and is the
  fourth linter: the curated warning flag set is versioned alongside the
  tidy profile.

**Everything else in the repo lints too**

- **CMake**: `gersemi` (formatting) + `cmake-lint`; the build system is
  code and gets code discipline.
- **Markdown** (the design docs): `markdownlint` with a light profile plus
  a link checker (internal doc cross-references like "doc 15" and relative
  links must resolve — the claims register depends on stable anchors).
- **Spelling**: `typos` (fast, low-false-positive) across code, comments,
  and docs.
- **Shell**: `shellcheck` + `shfmt` for the gate and tooling scripts.
- **JSON/YAML**: format + schema validation; `.arbc` examples in docs and
  tests validate against the doc 08 schema once it exists — example
  documents that drift from the format are bugs.
- **EditorConfig** at the root so every editor agrees on the trivia
  (charset, trailing whitespace, final newline) before any tool runs.

All of it is wired into the same two places — the local gate script and
per-push CI — so there is exactly one answer to "is this change clean",
available before pushing.

## What is deliberately not adopted

- **Wall-clock performance gates in the merge path** — flaky by nature;
  behavioral counters gate, benchmarks trend.
- **Mocking-heavy unit style** — the contract suite plus real in-memory
  implementations cover integration seams; mocks are reserved for fault
  injection (I/O shim, allocator hooks).
- **Coverage as a target** beyond the diff gate — the number is a
  thermometer, not a thermostat.
- **Mutation testing, for now** — revisit for the numeric kernels and the
  reclamation protocol once they stabilize; high value there, noise
  elsewhere.
