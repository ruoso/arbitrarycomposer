# Contributing

The rules of this repo live in tools, not in prose. This file tells you which
tools, in what order, and what "done" means; wherever a rule has an owner
elsewhere in the tree, this file states it in one line and links to the owner
rather than restating it — a second copy is a copy that drifts.

The normative source for everything below is
[design doc 16](docs/design/16-sdlc-and-quality.md) (SDLC and quality) and
[design doc 17](docs/design/17-internal-components.md) (component levelization).

## Prerequisites

- A C++20 compiler — GCC 13+ or Clang 18+ (the sanitizer lanes want Clang 20;
  `-fsanitize=realtime` needs LLVM ≥ 20).
- CMake ≥ 3.24 and Ninja or Make.
- Python 3.12+ — the structure checks and the doc check are stdlib-only scripts.
- `clang-format` **19.1.7** — pinned; see [Style](#style).

Dependencies are fetched by the build (design doc
[10](docs/design/10-tooling-and-packaging.md)); there is nothing to install by
hand.

## Quick start

```bash
cmake --preset dev
cmake --build --preset dev --parallel "$(nproc)"
ctest --preset dev
```

## The gate

Run it before every push. It is the whole local contract:

```bash
scripts/gate
```

It runs, in order:

1. **configure** — `cmake --preset dev`
2. **build** — `cmake --build --preset dev`
3. **test** — `ctest --preset dev` (the full CTest suite, including the
   `docs.contributing` check that keeps *this* file honest)
4. **format** — `clang-format --dry-run -Werror` over every tracked `.cpp`/`.hpp`
   (warns and skips if `clang-format` is not installed; CI still enforces it)
5. **levelization** — `python3 scripts/check_levels.py`: a component may only
   include from its declared dependency closure (doc 17). Dependency direction is
   a build error, not a review comment.
6. **claims register** — `python3 scripts/check_claims.py`: see
   [The claims register](#the-claims-register)
7. **rt-safety lint** — `python3 scripts/check_rt_safety.py`: no allocation,
   locking, or `shared_ptr` traffic in an `ARBC_RT_NONBLOCKING` call graph
8. **worker-dispatch lint** — `python3 scripts/check_worker_dispatch.py`: worker
   dispatch is leaf-only, and the rule lives in exactly one helper

Steps 4–8 are exactly the five checks CI's `lint` job runs, and step 3 is exactly
what CI's matrix runs — a gate-green tree is a tree CI's `lint` job agrees with.

Two environment knobs:

| Variable | Default | Meaning |
| --- | --- | --- |
| `ARBC_GATE_PRESET` | `dev` | Which preset the gate configures, builds, and tests. |
| `CXX` | auto-probed | The compiler. The gate probes `c++`, `g++`, `clang++`, … and prints what it picked. |

Doc 16:96-100 asks for the local gate to run under **Debug+ASan**. It runs a
single preset by default, because a gate that always builds twice is a gate people
stop running (and `clang-asan` runs on every push anyway). The sanitizer build is a
knob, and it is the *expected* extra step for a change that touches memory or
concurrency:

```bash
ARBC_GATE_PRESET=asan scripts/gate
ARBC_GATE_PRESET=tsan scripts/gate    # if you touched threading
```

### Presets

Every preset name is a configure, build, and test preset alike
(`cmake --preset <name>`, `cmake --build --preset <name>`, `ctest --preset <name>`).

| Preset | What it is |
| --- | --- |
| `dev` | Debug + tests. The default for the gate and for daily work. |
| `release` | Release + tests. |
| `asan` | Debug + AddressSanitizer/UBSan. |
| `tsan` | Debug + ThreadSanitizer. |
| `rtsan` | Debug + RealtimeSanitizer (Clang ≥ 20 only). |
| `coverage` | Debug + coverage instrumentation; what the CI diff-coverage gate builds. |
| `fuzz` | Debug + the libFuzzer loader harness (Clang only); nightly. |
| `install-fetched` | Debug + tests with every dependency *fetched*, never found. |
| `bench` | Release + Google Benchmark. Opt-in, off the merge path. |

## Definition of done

Doc 16:138-142 defines done as *code + contract/claims tests + design-doc delta +
benchmark run clean + examples still building*, "mechanized where possible,
checklist where not". This is that checklist — copy it into your PR body or a
scratch buffer. Each item names the mechanism that proves it, or says *judgment*
where no machine can.

```markdown
- [ ] Code — `scripts/gate` green (build, tests, format, levelization, claims, rt-safety, worker-dispatch)
- [ ] Contract/claims tests — new behavior has a test; a new doc promise has a
      claims-register row and an `// enforces:` tag (`python3 scripts/check_claims.py`)
- [ ] Diff coverage — changed lines in `src/` are ≥90% covered (hard gate in CI; see
      "Not yet mechanized" for the local story)
- [ ] Design-doc delta — behavior that differs from the design docs lands its doc
      edit in the same commit (judgment; the docs are normative, not descriptive)
- [ ] Sanitizers — memory/concurrency changes ran `ARBC_GATE_PRESET=asan scripts/gate`
      (and `tsan` if threading) (judgment on applicability, mechanized once chosen)
- [ ] Benchmark run clean — for performance-shaped changes, `cmake --preset bench` and
      look, or justify the movement in the commit body. *Never a merge gate* (doc 16:224-226).
- [ ] Examples still building — *not applicable until `packaging.examples` lands*; there
      is no `examples/` directory yet.
- [ ] Changelog — a consumer-observable change lands its `CHANGELOG.md` entry in the same
      commit. The rule is in [CHANGELOG.md](CHANGELOG.md)'s own header; docs-only,
      test-only, and internal-refactor commits belong in the git log alone.
- [ ] WBS-tracked work — the completion ritual in
      [tasks/refinements/README.md](tasks/refinements/README.md) (refinement `## Status`,
      `complete 100` in the `.tji`, milestone propagation, tech-debt registration)
```

## The claims register

The design docs make falsifiable promises. A **claim** is one such promise, given
an id and pinned to a test, so a doc sentence cannot quietly become false.

- **The id** is `<doc-file-stem>#<slug>` — e.g.
  `13-effects-as-operators#fade-attenuates-both-facets`.
- **The row** goes in [tests/claims/registry.tsv](tests/claims/registry.tsv), as
  `<claim-id><TAB><description>`. The description is the promise in one sentence,
  falsifiably.
- **The tag** is a `// enforces: <claim-id>` comment stacked immediately above the
  `TEST_CASE` that would fail if the promise broke. One line per claim; a test may
  enforce several.
- **The check is bidirectional** (`python3 scripts/check_claims.py`, in the gate and
  in CI): a registered claim with no enforcing test fails, and an `// enforces:` tag
  naming an unregistered claim fails. The register and the tests cannot drift apart.

Claims are *behavioral* promises the library makes. Process guarantees ("the lane
is green", "the doc names real presets") are enforced by CI structure, not by a
claims row.

## Style

**The configuration files are the style guide** (doc 16:156-162). Style is not
debated in review; it is `.clang-format`, `.clang-tidy`, and the warning flags.

- **clang-format is a hard gate** and its version is **pinned to 19.1.7** — the
  same version CI installs (`.github/workflows/ci.yml`). Formatters drift between
  releases; an unpinned one turns a clean tree red. Install it with
  `pip install clang-format==19.1.7`.
- **Include order is format-owned** — the clang-format include categories encode
  the convention; do not hand-sort.
- **Reformat commits are isolated.** A commit that only reformats (config change,
  tool-version bump) touches nothing else and gets listed in
  `.git-blame-ignore-revs`, so archaeology never trips over formatting noise.
- **Levelization is physical** (doc 17): a component includes only from its
  declared dependency closure. If `scripts/check_levels.py` rejects an edge you
  need, the answer is a design-doc-17 delta, not an exception in the checker.

## When main goes red

Trunk-based development with hard local gates means CI is the *net*, not the gate.
So the net has to be loud, and the response is a protocol, not a discussion
(doc 16:106-111):

> A failing main is the top priority for whoever landed it — **fix forward within
> the hour or revert**; either is one command and never shameful.

Further landings pause while main is red.

## Not yet mechanized

Doc 16 promises these; they do **not** run today. They are listed here so nobody
reads the gate as covering ground it does not, and so the gap ages visibly rather
than invisibly. Each names the WBS leaf that will land it.

| Promise (doc 16) | State today | Lands in |
| --- | --- | --- |
| Diff coverage ≥90% *in the local gate script* (16:115-118) | CI-only; the gate does not measure coverage | `quality.gate_diff_coverage` |
| `markdownlint` + a doc link checker, `typos`, `shellcheck`/`shfmt`, `gersemi`/`cmake-lint` (16:203-218) | none wired | `quality.repo_linters` |
| clang-tidy as a blocking check (16:180-190) | nightly, and `continue-on-error` | `quality.tidy_promotion` |
| "Examples still building" (16:138-142) | there is no `examples/` directory | `packaging.examples` |
| Tracked benchmark history (16:223-226) | benchmarks build and run; no history is kept | `quality.benchmark_history` |

The task tree that tracks these is [tasks/](tasks/) (TaskJuggler); the per-task
refinements are in [tasks/refinements/](tasks/refinements/).

## Keeping this file true

`python3 scripts/check_contributing.py` runs as the `docs.contributing` CTest — in
the gate and in CI, since they run the same suite. It fails if this document names
a script that does not exist (or invokes a non-executable one directly), a
`--preset` that `CMakePresets.json` does not define, or a repo-relative link that
does not resolve. Its own negative fixtures run as `docs.contributing.selftest`.

It does not check prose. If you change a command here, run the gate.
