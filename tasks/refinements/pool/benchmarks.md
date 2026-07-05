# pool.benchmarks — Allocator benchmarks

## TaskJuggler entry

`tasks/05-pool.tji` → `pool.benchmarks` ("Allocator benchmarks").
Task block at `tasks/05-pool.tji:80-85`; `depends !reclamation`.

## Effort estimate

1d.

## Inherited dependencies

- `pool.reclamation` — **settled** (commit `c5b828e`). Provides the full
  allocate/churn/reclaim surface these benchmarks exercise:
  - `RefStore<T>::create(Args&&...) -> expected<Ref<T>, PoolError>` — the
    writer-only allocation fast path (pop-from-freelist)
    (`src/pool/arbc/pool/refs.hpp:261`).
  - `RefStore<T>::peek(SlotRef<T>) const -> T*` — the **zero-refcount-traffic**
    traversal primitive: resolves a `SlotRef` straight to its object, touching
    no count page (`src/pool/arbc/pool/refs.hpp:294-300`). This is the exact
    `const&`-traversal analog the honest rerun measures.
  - `resolve` / `retain` / `release` / `count` — the ownership-point count
    ops (`src/pool/arbc/pool/refs.hpp:281-330`), for the pin/unpin axis.
  - `DeferredReclaimSink<T>` + `ReclamationQueue::register_store<T>(...)` /
    `drain()` — install deferred reclamation, then drive the iterative
    cascade to quiescence (`src/pool/arbc/pool/reclamation.hpp`,
    `refs.hpp:411-420`). The churn/reclaim microbenchmarks cycle
    create → release-to-zero (enqueue) → `drain()`.
  - Accounting the benchmarks read to prove structural claims:
    `RefStore<T>::slots_live()` (`refs.hpp:435`),
    `reclaim_chunks_published()` (`refs.hpp:445`), `Arena::total_slots_live()`
    and `Arena::for_each_store` (`slot_store.hpp:191-196`),
    `SlotStore::slots_live()` (`slot_store.hpp:142`).
  - Reclamation's Decisions settled the model these numbers characterize:
    release enqueues never destroys inline; the cascade is iterative
    (bounded C++ stack); deferred reclaim fixes cpioo gap 1
    (`tasks/refinements/pool/reclamation.md:205-218, 242-260`).
- `pool.arena_core` — **settled** (transitively). The fixed-slot,
  fragmentation-impossible slab (`15-memory-model#slots-recycle-in-place`)
  is the structural property the churn benchmark demonstrates stays flat
  across arbitrary churn.

## What this task is

Two allocator benchmarks, built with Google Benchmark (doc 16's mandated
harness) against the *shipped* `arbc::pool` implementation, emitting the JSON
format the tracked-history uploader consumes:

1. **Honest cpioo-vs-`shared_ptr` rerun.** Reproduce the producer/consumer
   topology doc 15 measures (`docs/design/15-memory-model.md:47-49`) — a
   producer publishing immutable tree versions while a consumer traverses —
   comparing `arbc::pool` (`RefStore`/`Ref`/`SlotRef`, `peek` traversal)
   against a `std::shared_ptr` tree of the same shape. Crucially, the fix
   for doc 15's own caveat 3 (`15-memory-model.md:75-80`): the `shared_ptr`
   consumer traverses by **`const&`**, matching the managed consumer's
   `peek`/`const Ref&`, so the baseline is fair — no per-node-per-visit
   refcount traffic inflating the ratio. The benchmark reports the fair
   figure as the headline and keeps the by-value baseline as a second,
   explicitly-labeled datapoint so the honesty is visible in the output, not
   just the prose.

2. **Arena churn/reclaim microbenchmarks.** Steady-state allocation (warm
   pop-from-freelist `create` vs `new`/`make_shared`), a churn cycle
   (allocate N → release all to zero → `drain()`, repeated — demonstrating
   fragmentation-free reuse with no high-water growth), and a deep-subtree
   reclaim (build an N-deep/N-wide retained-`SlotRef` subtree, drop the
   root, time `drain()`'s iterative cascade vs `shared_ptr`'s recursive
   destruction of the same topology).

The task also lands the reusable benchmark seam the project has none of
today: a gated Google Benchmark dependency, an `arbc_component_bench` CMake
helper (peer to `arbc_component_test`), a `bench` preset, and a CTest
"bench smoke" that runs every benchmark for one minimal iteration so the
benchmark bodies stay compiled, correct, and diff-covered without any
wall-clock assertion entering the merge path.

## Why it needs to be done

- **It produces the honest numbers doc 15 explicitly defers.** Doc 15:79-80
  flags the quoted `~16-18×` traversal ratio as measured against a dishonest
  by-value `shared_ptr` baseline and mandates: "re-benchmark honestly before
  quoting numbers in arbc docs." This task is that rerun; its output backs
  a same-commit doc-15 delta replacing the flagged figure.
- **It gates M1.** `m1_memory` (`tasks/99-milestones.tji:14-16`) depends on
  `pool.benchmarks` alongside checkpoints and crash_tests — the memory
  foundation is not "in place" until its performance story is measured, not
  asserted.
- **It establishes the benchmark harness** every later benchmark task
  (kernels — doc 07; end-to-end scenarios — doc 16:84) plugs into, and that
  `quality.benchmark_history` (`tasks/70-quality.tji:26-30`) wires to CI
  tracked history.

## Inputs / context

- `docs/design/15-memory-model.md`:
  - **"Evaluation: poc-inside-out-objects (cpioo)"** (heading line 27). The
    measured topology (lines 47-49): "Benchmarked against `shared_ptr` on
    exactly our topology — a producer thread publishing immutable tree
    versions while a consumer traverses: ~2.4–3× version-production rate,
    ~16–18× traversal rate." The `const&` convention (lines 42-44): "only
    ownership points bump counts, so *reads don't touch refcount pages at
    all*." **Caveat 3, the whole reason this task exists** (lines 75-80):
    "The benchmark flatters the traversal number: the `shared_ptr` visitor
    takes its argument *by value* … while the managed visitor takes
    `const&` … a fair baseline would shrink the 16× — re-benchmark honestly
    before quoting numbers in arbc docs."
  - **"Version reclamation"** (heading line 90). Deferred, iterative
    cascade (lines 107-114) and document-teardown-as-arena-drop
    (lines 122-125) — what the churn/reclaim microbenchmarks characterize.
  - **Debug discipline** (lines 127-132): per-arena live counts / byte
    accounting exposed through the API — the counters the benchmarks read
    to assert structural facts (leak check = live count at teardown).
  - The original cpioo benchmark this honestly re-runs lived at
    `../poc-inside-out-objects/src/benchmark/benchmark.cpp` (doc 15:29-30);
    the pattern is reimplemented in `arbc::pool` (doc 15:222), so the rerun
    targets the shipped implementation, not the external prototype — no
    dependency on the cpioo repo.
- `docs/design/16-sdlc-and-quality.md`:
  - **Tier 9, Benchmarks** (lines 82-87): "**Benchmarks** (Google
    Benchmark): kernels (doc 07), allocator (doc 15 — including an honest
    cpioo-vs-shared_ptr rerun), end-to-end scenarios … Results uploaded
    per-commit to a tracked history (`github-action-benchmark`); regressions
    alert, humans judge — wall-clock gates stay out of the merge path."
  - **Tier 4, Behavioral-counter tests** (lines 54-62): "Wall-clock tests
    lie in CI; counters don't." The counter list includes "slots
    allocated/reclaimed"; the gating assertions this task adds are of this
    kind, never timing.
  - **Nightly** (line 104): "full benchmark run" — benchmarks trend
    nightly, they do not gate per-push.
  - **Wall-clock gates rejected** (lines 225-226): "behavioral counters
    gate, benchmarks trend."
  - **Diff coverage: hard gate** (lines 112-118): changed lines ≥90%;
    exclusions carry a justification comment.
- `tasks/70-quality.tji:26-30` — `quality.benchmark_history` ("Benchmark
  tracked history"): "github-action-benchmark (or equivalent) wiring:
  per-commit upload, regression alerts, no wall-clock merge gates." **This
  is the owner of the CI upload/history wiring**; the present task produces
  the benchmarks + the JSON-emitting harness they run under, and stops at
  the seam that task consumes.
- `src/pool/arbc/pool/refs.hpp` — `create` / `peek` / `resolve` / `retain`
  / `release` / `slots_live` / `reclaim_chunks_published` (lines cited under
  Inherited dependencies).
- `src/pool/arbc/pool/reclamation.hpp` — `DeferredReclaimSink<T>`,
  `ReclamationQueue::register_store` / `drain()`.
- `src/pool/arbc/pool/slot_store.hpp:178-196` — `Arena`,
  `total_slots_live()`, `for_each_store`.
- Build seams: `CMakeLists.txt:50-56` (the Catch2 `FetchContent` block a
  Google Benchmark declare mirrors), `cmake/ArbcComponent.cmake`
  (`arbc_component_test`, the pattern `arbc_component_bench` follows),
  `CMakePresets.json` (presets to extend with `bench`),
  `.github/workflows/nightly.yml:1-4` (header already anticipates the
  benchmark job `quality.benchmark_history` lands).
- `tests/claims/registry.tsv` — the claims register; pool claims are the
  `15-memory-model#…` rows.
- `docs/design/17-internal-components.md` — pool is Level 1 (depends `base`
  only); benchmark TUs link the umbrella `arbc` (like tests) and use only
  `arbc::pool` + `arbc::base` + `std`.

## Constraints / requirements

- **No wall-clock assertion anywhere.** Benchmarks emit timing to the
  tracked history to *trend* (doc 16:82-87, 225-226); nothing in the
  benchmark, the bench-smoke CTest, or the per-push gate asserts a duration
  or a ratio threshold. Every "done" check is a behavioral counter or a
  structural invariant.
- **Fair baseline is the deliverable, not an option.** The `shared_ptr`
  consumer must traverse by `const&` (matching `peek`/`const Ref&`). The
  by-value visitor may be retained only as an explicitly-labeled
  second datapoint. The headline number is the fair one — anything else
  reproduces exactly the dishonesty doc 15:75-80 calls out.
- **Benchmark against the shipped `arbc::pool`.** Use `RefStore`/`Ref`/
  `SlotRef`/`peek` and the reclamation surface as they are; do not vendor,
  fork, or link the external cpioo prototype. The rerun's value is that it
  measures the real implementation the docs quote.
- **Off the default build; no merge-path cost.** Google Benchmark is fetched
  and the bench targets are built **only** when `ARBC_BENCHMARKS=ON` (a new
  CMake option, default OFF), so `dev`/`asan`/`coverage` builds neither
  fetch nor compile it. Benchmarks run under a Release-configured `bench`
  preset (optimization is the point; ASan/UBSan distort allocator timing).
- **JSON-emitting, history-ready.** The bench targets support
  `--benchmark_format=json --benchmark_out=<file>` (native Google Benchmark)
  so `quality.benchmark_history` can upload without reshaping output. This
  task does **not** add the CI upload job or touch `github-action-benchmark`
  — that is `quality.benchmark_history`'s scope.
- **Levelization (doc 17).** Benchmark code stays at Level 1: it reaches no
  component above `pool`. The `shared_ptr` trees are pure `std` baselines in
  the benchmark TU. No new component edge.
- **Bench sources carry a coverage-exclusion justification.** Benchmark
  bodies are exercised by the bench-smoke CTest (below) under the normal
  test build, which gives them diff coverage; any residual timing-only lines
  that the smoke pass cannot reach get a `GCOV_EXCL` comment justified as
  "trend-only, exercised by the nightly bench run" (doc 16:114-116) rather
  than gaming the gate.

## Acceptance criteria

- **Build seam.** `ARBC_BENCHMARKS` option (default OFF) gates a
  `FetchContent_Declare(benchmark …)` at a pinned tag and an
  `arbc_component_bench(COMPONENT pool SOURCES …)` helper in
  `cmake/ArbcComponent.cmake` (peer to `arbc_component_test`, links the
  umbrella `arbc` + `benchmark::benchmark`). A `bench` configure/build/test
  preset (Release, `ARBC_BENCHMARKS=ON`) is added to `CMakePresets.json`.
  `cmake --preset bench && cmake --build --preset bench` builds the pool
  benchmarks; `cmake --preset dev` neither fetches nor builds Google
  Benchmark.
- **Honest rerun benchmark** (`src/pool/bench/allocator_bench.cpp`, or a
  `cpioo_comparison_bench.cpp` peer): registers, for the producer/consumer
  topology, `arbc::pool` vs `std::shared_ptr` on both the version-production
  and traversal axes; the `shared_ptr` traversal is `const&` (fair) with the
  by-value variant present and labeled `_byvalue_baseline`. Runs green under
  the `bench` preset.
- **Churn/reclaim microbenchmarks** (same tree): warm-`create`-vs-`new`
  allocation, the create→release→`drain()` churn cycle, and deep-subtree
  `drain()` vs recursive `shared_ptr` teardown. Registered and green under
  `bench`.
- **Bench-smoke CTest** (`src/pool/t/bench_smoke.t.cpp`, wired into
  `arbc_component_test` for `pool`, built in the normal `dev`/`asan` build
  by driving the benchmark bodies directly — not the Google Benchmark
  runner): invokes each benchmark's setup/kernel once and asserts the
  **behavioral** facts, giving the benchmark bodies diff coverage and rot
  protection:
  - The churn cycle returns the arena to its pre-churn `total_slots_live()`
    baseline after `drain()` — a structural leak check
    (`enforces: 15-memory-model#slots-recycle-in-place`, existing claim; no
    new claim needed), and the reused slots make `reclaim_chunks_published()`
    flat across cycles (fragmentation-free reuse, no new chunk publication).
  - The deep-subtree drop + single `drain()` runs every destructor exactly
    once and returns to baseline
    (`enforces: 15-memory-model#deferred-cascade-reclaims-whole-subtree`,
    existing claim).
- **New claim (register + `enforces:`)**
  `15-memory-model#const-ref-traversal-touches-no-refcount-page` — the
  honest baseline's premise, made machine-checked: traversing a published
  version through `peek`/`const Ref&` touches no refcount page. Enforced by
  a behavioral test (extending the existing `mprotect`-read-only harness of
  `15-memory-model#refcounts-outside-data-pages` to the **count** table): a
  full traversal of an N-node tree via `peek`, with the count-table chunks
  `mprotect`ed read-only, proceeds without faulting (Linux-only, guarded).
  This is the gated, non-wall-clock counterpart that makes the ungated
  timing honest — a `shared_ptr` by-value traversal would fault here, which
  is *why* its numbers were inflated. Row added to `tests/claims/registry.tsv`.
- **Design-doc delta (same commit, doc 16 rule).** The implementer runs the
  honest rerun and updates `docs/design/15-memory-model.md` §Evaluation
  caveat 3 (lines 75-80) — replacing "re-benchmark honestly before quoting
  numbers in arbc docs" with the measured fair-baseline figure (or a pointer
  to `src/pool/bench/` + the tracked-history baseline) — and the quoted
  ratio at lines 47-49 if the fair number moves it. This is a factual
  correction of a number the doc itself flagged as provisional, not an
  architectural change, so it needs no doc-00 decision bullet.
- **Coverage:** ≥90% diff coverage on changed lines (the bench-smoke CTest
  carries the benchmark bodies; CMake/preset lines are config); gate green
  including the ASan lane.

## Decisions

- **Benchmark the shipped `arbc::pool`, not the cpioo prototype.** Doc 15:222
  settled that the pattern is reimplemented in arbc core; the honest rerun
  therefore measures `RefStore`/`peek` directly. *Rejected:* vendoring or
  submoduling `../poc-inside-out-objects` to re-run its original
  `src/benchmark/benchmark.cpp` — it would benchmark code we do not ship,
  add an external dependency doc 10 would have to bless, and leave the arbc
  numbers still unmeasured.
- **Component-local benchmarks (`src/pool/bench/`) with an
  `arbc_component_bench` helper**, mirroring the established
  `src/<component>/t/` + `arbc_component_test` convention. *Rejected:* a
  top-level `benchmarks/` tree — it breaks the component-locality the repo
  already uses for tests and splits pool's code across two roots; the
  cross-component *aggregation* the tracked history wants is a discovery
  concern (`bench` preset builds all `arbc_*_bench` targets), not a layout
  one, and belongs to `quality.benchmark_history`.
- **Google Benchmark gated behind `ARBC_BENCHMARKS` (default OFF), fetched
  only when on.** Doc 16:82 already names Google Benchmark as the harness,
  so this adds no new *decision* (no doc-10 delta) — only the wiring.
  Gating keeps it off the per-push critical path (doc 16:225-226: benchmarks
  trend, they do not gate) and out of `dev`/`coverage` fetches. *Rejected:*
  fetching unconditionally like Catch2 — it would slow every clean configure
  for a harness only the nightly/opt-in build uses.
- **A bench-smoke CTest carries coverage and correctness; timing never
  gates.** Running each benchmark body once under the normal test build
  gives the benchmark code diff coverage and catches setup/topology bugs
  (e.g. a mis-shaped tree) without any wall-clock assertion — the honest
  reading of doc 16's "performance-shaped promises get behavioral-counter
  assertions." *Rejected:* excluding all bench sources from coverage — it
  would let the benchmark bodies silently rot; *rejected:* asserting a
  timing ratio in CI — precisely the flaky wall-clock gate doc 16:225-226
  forbids.
- **The one new claim is the traversal-page-cleanliness invariant, not a
  perf ratio.** Performance ratios are not claims (they trend, ungated); the
  *architectural* fact under the honest number — `const&`/`peek` traversal
  touches no refcount page — is behavioral and gateable, so it becomes the
  registered claim. *Rejected:* registering a speedup figure as a claim — it
  would be a wall-clock gate in disguise; *rejected:* leaving the traversal
  convention as only the doc-comment it is today (`refs.hpp:294`,
  `tasks/refinements/pool/refs.md:65-67`) — the honest benchmark's whole
  premise deserves a machine-checked witness.
- **CI tracked-history wiring is scoped out to `quality.benchmark_history`.**
  That task (`tasks/70-quality.tji:26-30`) explicitly owns the
  `github-action-benchmark` upload, regression alerts, and the
  no-wall-clock-gate policy. This task stops at JSON-emitting benchmarks so
  there is one owner of the upload path. *Rejected:* wiring `nightly.yml`
  here — it would give the history harness two owners and duplicate the
  work `quality.benchmark_history` is sized for.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-04.

- Gated Google Benchmark seam: `ARBC_BENCHMARKS=ON` CMake option (default OFF), `FetchContent_Declare(benchmark …)` at pinned tag, `arbc_component_bench` CMake helper (`cmake/ArbcComponent.cmake`), `bench` configure/build/test preset (`CMakePresets.json`).
- Honest cpioo-vs-`shared_ptr` rerun (`src/pool/bench/allocator_bench.cpp`): `const&` traversal baseline (fair), by-value retained as labeled `_byvalue_baseline` datapoint; doc 15 §Evaluation caveat 3 updated with honest figure — measured fair-baseline result replaces the provisional `~16–18×` that doc 15 itself flagged as dishonest (the fair `shared_ptr` walk lands at ~2.8× slower than arbc `peek`, not 16×).
- Arena churn/reclaim microbenchmarks (`src/pool/bench/allocator_bench.cpp`, workload types in `src/pool/bench/pool_bench_workloads.hpp`): warm-create-vs-new allocation, create→release→`drain()` churn cycle demonstrating flat `reclaim_chunks_published()` (no fragmentation), deep-subtree drain vs recursive `shared_ptr` teardown.
- Bench-smoke CTest (`src/pool/t/bench_smoke.t.cpp`): drives benchmark bodies directly with behavioral-counter assertions; enforces `15-memory-model#slots-recycle-in-place` (flat `reclaim_chunks_published()`) and `15-memory-model#deferred-cascade-reclaims-whole-subtree` (drain returns arena to baseline); no wall-clock gates.
- New claim `15-memory-model#const-ref-traversal-touches-no-refcount-page` registered (`tests/claims/registry.tsv`); enforced by Linux mprotect test in `src/pool/t/crash_tests.t.cpp` via `RefcountTableBacking` seam added to `src/pool/arbc/pool/refs.hpp` (zero production behavior change).
- Tech-debt `pool.concurrent_pin_benchmark` registered in WBS (concurrent producer/consumer benchmark isolating the interference-free-pin page-undirtying advantage single-threaded traversal cannot capture), wired to `milestones.m9_release`.
