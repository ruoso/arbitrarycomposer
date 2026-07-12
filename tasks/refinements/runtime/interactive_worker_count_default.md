# runtime.interactive_worker_count_default — Choose and ship a non-zero default worker count for `InteractiveRenderer`

## TaskJuggler entry

[`tasks/65-runtime.tji:153-158`](../../65-runtime.tji) — `task interactive_worker_count_default`,
inside `task runtime`. Milestone: `m9_release` (`tasks/99-milestones.tji:72`).

## Effort estimate

`2d`.

## Inherited dependencies

**Settled (all `complete 100`):**

- **`runtime.worker_dispatch_leaf_only`** (`!worker_dispatch_leaf_only`, the declared
  dependency) — hoisted the leaf-only rule into `worker_backed_dispatch(WorkerPool&)`
  (`src/runtime/arbc/runtime/worker_dispatch.hpp:57`,
  `src/runtime/worker_dispatch.cpp:13-26`) and wired it into the interactive frame at
  `src/runtime/interactive.cpp:274`. It made `worker_count > 0` *correct* and TSan-clean
  on the interactive path, registered
  `02-architecture#worker-dispatch-is-leaf-only` (`tests/claims/registry.tsv:149`,
  which already claims byte-identity across worker counts 0/1/4), and **deliberately left
  the default at `0`**. This task is its named source-of-debt successor.
- **`runtime.interactive_pull_wiring`** (transitive) — built the frame-local
  `PullServiceImpl` (`src/runtime/interactive.cpp:274`) and staged the dispatch argument as
  the one-line swap the predecessor then took.
- **`kinds.nested_runtime_binding`** (transitive) — the operator kinds that make
  operator-heavy and nested-deep scenes renderable interactively at all.

**Pending:** none. Nothing this task needs is unbuilt.

**Adjacent, deliberately not depended on:** `quality.benchmark_history`
(`tasks/70-quality.tji:27-31`, unimplemented) will later upload this task's benchmark JSON
to a tracked history. This task emits the JSON in the standard shape
(`--benchmark_format=json --benchmark_out=…`, the idiom at
`src/pool/bench/allocator_bench.cpp:23-24`) and does not wait for that wiring.

## What this task is

Today `InteractiveRenderer` ships inline: its constructor defaults to
`WorkerPoolConfig{}` (`src/runtime/arbc/runtime/interactive.hpp:168`), whose
`worker_count` is `0` (`src/runtime/arbc/runtime/worker_pool.hpp:63`), and at `0` the pool
is the *degenerate inline executor* — `WorkerPool::submit` runs the render on the calling
thread and returns (`src/runtime/worker_pool.cpp:66`, `submit_inline` at `:88`;
`tests/claims/registry.tsv:146`). So every leaf miss in every interactive frame is
rendered on the frame thread, synchronously, inside Step 4.

This task ships a **non-zero interactive default**: a named runtime policy
`default_interactive_worker_count()` derived from `std::thread::hardware_concurrency()`,
installed as `InteractiveRenderer`'s constructor default, leaving
`WorkerPoolConfig::worker_count`'s `0` and `SequenceRenderer`'s inline-exact default
untouched. It adds the two behavioral counters the choice has to be argued from
(`frames_rendered()`, `deadline_expiries()` — plus `tiles_cancelled()`, which the
existing cancel loop bumps for free), a per-push counter test that pins the *invariants*
the default must satisfy, and an opt-in Google Benchmark over three representative scenes
(leaf-heavy, operator-heavy, nested-deep) at worker counts `0/1/2/4/hardware_concurrency-1`
whose numbers the implementer records in this document's `## Status` block as the evidence
for the shipped constant.

## Why it needs to be done

Two reasons, and they are different in kind.

**The sign is a correctness gap, and the design docs already settle it.** Doc 02's Step 4
(`docs/design/02-architecture.md:61-65`) promises: *"Cache misses become render requests
with a deadline… When the deadline nears, the frame proceeds with what it has: stale-revision
tiles, coarser-scale tiles rescaled, or checkerboard/transparent, in that preference
order."* At `worker_count == 0` that promise **cannot be kept for a slow synchronous leaf**:
`submit` *is* the render, so the frame thread is inside the leaf's `render` when the deadline
passes. It reaches the deadline park (`src/runtime/interactive.cpp:328`) only after every
miss has already been rendered to completion — there is nothing left to degrade to, and
nothing to cancel. The deadline is enforced today only against *externally* async content
(the `RefinementQueue` path). Doc 02's own closing caveat
(`docs/design/02-architecture.md:149-151` — *"v1 may degenerate to 'everything on one
thread' while keeping the request/completion structure"*) is precisely the licence the `0`
default has been living under. This task retires that licence for the interactive driver.

**The magnitude is a tuning question with no data behind it**, which is why the
predecessor refused to guess. From `tasks/refinements/runtime/worker_dispatch_leaf_only.md`
§ Not this task: *"This task makes `worker_count > 0` work; it does not make it the default.
Choosing a production worker count is a tuning question with no data behind it yet."* The
in-code pointer at `src/runtime/arbc/runtime/interactive.hpp:52-59` names this task by id.

Downstream: every host that constructs an `InteractiveRenderer` without an explicit pool
config — `HostViewport` borrows one (`src/runtime/host_viewport.cpp:43`, member at
`src/runtime/arbc/runtime/host_viewport.hpp:266`) — inherits whatever this task decides.
It is the first shipped arbitrarycomposer configuration with real threads under the frame
loop.

## Inputs / context

**Design docs (normative, doc 16's same-commit rule applies):**

- `docs/design/02-architecture.md:49-71` — the six interactive frame steps; `:61-65` is
  Step 4, the deadline promise quoted above. No millisecond budget is specified anywhere;
  `budget` is the caller's (`interactive.hpp:190-195`).
- `docs/design/02-architecture.md:118-151` — § Threading model. *"Layer rendering runs on a
  worker pool."* `:133-146` is the leaf-only carve-out the predecessor landed. `:149-151` is
  the degenerate-single-thread caveat this task amends. **Doc 02 says nothing about worker
  counts** — no number, no `hardware_concurrency`, no default. That gap is what the
  design-doc delta below fills.
- `docs/design/02-architecture.md:73-85` — the offline frame: *"exact scale, every request
  rendered to completion"*. Why `SequenceRenderer`'s default must not move.
- `docs/design/16-sdlc-and-quality.md:54-62` — tier 4, behavioral-counter tests: *"Wall-clock
  tests lie in CI; counters don't… Most claims-register entries about efficiency land here."*
- `docs/design/16-sdlc-and-quality.md:82-87` — tier 9, benchmarks: *"regressions alert,
  humans judge — wall-clock gates stay out of the merge path."*
- `docs/design/16-sdlc-and-quality.md:113-118` — the ≥90% diff-coverage hard gate.
- `docs/design/16-sdlc-and-quality.md:225-226` — *"behavioral counters gate, benchmarks
  trend."*
- `docs/design/17-internal-components.md:46-61` — `arbc::runtime` is L5, owns the frame loop
  and the worker pool (there is no separate pool component); `:110-112` — *"deadlines, frame
  loops, and device clocks are runtime policy"*. A worker-count policy is therefore runtime's
  to hold.
- `docs/design/17-internal-components.md:114-127` — the counter convention: persistent
  counter state lives in `runtime`; the compositor takes a caller-owned counters struct by
  pointer.

**Sources the task extends:**

- `src/runtime/arbc/runtime/worker_pool.hpp:57-73` — `WorkerPoolConfig`; `:63`
  `std::size_t worker_count = 0;` with the "degenerate inline executor" comment. `:115-128` —
  the pool's own counters `tasks_submitted()`, `tasks_completed()`,
  `max_in_flight_per_content()`.
- `src/runtime/worker_pool.cpp:12-13` (thread spawn), `:66` (the `worker_count == 0` inline
  branch), `:88` (`submit_inline`).
- `src/runtime/arbc/runtime/interactive.hpp:168` — the constructor whose default argument this
  task changes. `:52-59` — the in-code pointer naming this task. `:197-220` — the existing
  counter accessors (`counters()`, `stats()`, `identity_map_builds()`, `operator_binds()`) whose
  shape the new ones follow. `:262-270` — the load-bearing declaration order (`d_pending` before
  `d_pool`) that makes a `worker_count > 0` renderer safe to destroy with renders in flight;
  this task is what makes that path routine rather than opt-in.
- `src/runtime/interactive.cpp:302-318` — Step 4, the *single* clock read
  (`const auto deadline_at = d_clock() + budget;` at `:306`, "one instant, two uses, no drift").
- `src/runtime/interactive.cpp:320-335` — Step 5, the deadline park and cancel loop.
  **`ready == false` bumps no counter today, and the cancel loop counts nothing** — the two
  observability holes this task fills.
- `src/runtime/interactive.cpp:274` — `worker_backed_dispatch(d_pool)`, the only interactive
  submit path.
- `src/runtime/arbc/runtime/offline_sequence.hpp:71-79`, `src/runtime/offline_sequence.cpp:58` —
  `d_parallel(pool_config.worker_count != 0)`, the offline driver's inline-vs-parallel switch.
  The reason the default cannot move on `WorkerPoolConfig` itself.
- `src/compositor/arbc/compositor/counters.hpp:34-96` — `CompositorCounters`: caller-owned,
  plain `std::uint64_t`, **not atomic** (`:22-24`). `requests_issued()` (`:40`),
  `composites()` (`:43`), `follow_up_frames()` (`:46`), `operator_renders()` (`:54`),
  `degraded_composites()` (`:65`).

**Predecessor decisions this task inherits:**

- Byte-identity across worker counts is already claimed and enforced
  (`tests/claims/registry.tsv:149`; `tests/worker_dispatch_leaf_only.t.cpp:583`, counts 0/1/4
  on both drivers). This task *relies* on it and makes it load-bearing for the whole golden
  suite rather than one test.
- `scripts/check_worker_dispatch.py` (CI `lint`, `.github/workflows/ci.yml:38`) fails the build
  on any `RenderTask` submission outside `src/runtime/worker_dispatch.cpp`.
- `tasks/refinements/runtime/interactive_pull_wiring.md` Decision 5 — the claims-register
  discipline: *"do not mint a claim id for a sentence no design doc contains."* A new claim
  here therefore requires the doc 02 delta, in the same commit.
- The bench-smoke pattern: benchmark bodies live in a shared workload header driven once at
  minimal size by a normal CTest, so bench code carries diff coverage without wall-clock
  assertions entering the merge path (`src/pool/bench/pool_bench_workloads.hpp`,
  `src/pool/t/bench_smoke.t.cpp`, registered `src/pool/CMakeLists.txt:13,16`; the rationale is
  written into `cmake/ArbcComponent.cmake:58-67`).

## Constraints / requirements

1. **`WorkerPoolConfig::worker_count` keeps its `0` default, and `0` keeps meaning inline.**
   The struct is shared by both drivers; `registry.tsv:146` claims `0` ⇒ degenerate inline
   executor. The interactive default is installed at `InteractiveRenderer`'s constructor, not
   in the struct.
2. **`SequenceRenderer` is untouched.** Offline stays inline-exact by default
   (`offline_sequence.cpp:58`, doc 02:73-85). A host that wants parallel-exact offline still
   opts in explicitly. No file under the offline driver changes.
3. **An explicit inline opt-out survives.** `InteractiveRenderer(WorkerPoolConfig{})` must
   still give a thread-free renderer — every existing test that spells `{}` today
   (`src/runtime/t/interactive.t.cpp`, `src/runtime/t/host_viewport.t.cpp`,
   `tests/interactive_operator_identity.t.cpp:169`, `tests/document_serialize_concurrency.t.cpp:829`,
   …) keeps its current behavior with no edit. Debuggability and determinism are worth one
   spelling.
4. **No new dependency.** `<thread>` (`std::thread::hardware_concurrency`) is the standard
   library; doc 10's dependency policy is not engaged. No new levelization edge — everything
   lands in `runtime` (L5), which already names `WorkerPool`.
5. **No second submit site.** `worker_backed_dispatch` stays the only one
   (`scripts/check_worker_dispatch.py`).
6. **No second clock read in the frame loop.** `interactive.cpp:303-306` documents
   `d_clock()` as the loop's only read ("one instant, two uses, no drift"). The new counters
   must be derivable from `wait_completions`' return value and the pending queue — not from
   re-sampling the clock to ask "did we overrun?".
7. **New counters are frame-thread-only, plain `std::uint64_t`.** `CompositorCounters` is
   deliberately non-atomic (`counters.hpp:22-24`) and the driver's counters follow it.
   Nothing on a worker may bump a counter — that would be exactly the data race the leaf-only
   rule exists to prevent, and TSan would catch it.
8. **No test may assert the exact default worker count.** `hardware_concurrency()` varies by
   machine, so the shipped default varies by machine. Tests assert *properties* (`> 0`,
   `<= k_max`, byte-identity with inline), never the number. A test that hard-codes `4` is
   green on the author's box and red in CI.
9. **Zero golden re-baselining.** Every existing golden — interactive, offline, fade,
   crossfade, nested, damage, host-viewport — passes unchanged. This is the cash value of
   `registry.tsv:149`'s byte-identity clause; if a golden moves, the bug is in the parallel
   path, not in the golden.
10. **The default must be reachable by the goldens.** Flipping a constructor default that no
    test exercises would ship an untested configuration. At least the interactive golden
    tests run at the shipped default as well as at inline (see A2).
11. **Wall-clock never gates.** The benchmark reports; the merge path asserts counters only
    (doc 16:225-226). The shipped constant is a reviewed choice recorded in `## Status`, not a
    number a CI assertion defends.

## Acceptance criteria

**A1 — The default is non-zero, and it is a policy, not a magic number.**
`std::size_t default_interactive_worker_count()` and
`WorkerPoolConfig default_interactive_pool_config()` exist in `runtime` (new
`src/runtime/arbc/runtime/interactive.hpp` declarations, defined in
`src/runtime/interactive.cpp`), and
`InteractiveRenderer`'s constructor default argument is `default_interactive_pool_config()`.
Unit test (`src/runtime/t/interactive.t.cpp`): the returned count is `>= 1` and
`<= k_max_interactive_workers` on any machine, and a default-constructed
`InteractiveRenderer`'s `worker_pool()` reports the same count — asserted as a property, never
as a literal (Constraint 8).

**A2 — The shipped default is under golden coverage, byte-identically.**
`tests/interactive_operator_binding_golden.t.cpp` and `tests/host_viewport_reanchor_golden.t.cpp`
are parameterized (Catch2 `GENERATE`) over `{WorkerPoolConfig{}, default_interactive_pool_config()}`
and produce byte-identical output against the existing, unchanged golden bytes. Tagged as a
second `enforces: 02-architecture#worker-dispatch-is-leaf-only` enforcer of
`registry.tsv:149`'s byte-identity clause — a new claim id is *not* minted, per
`interactive_pull_wiring.md` Decision 5.

**A3 — The headline behavioral claim: the shipped default renders leaf misses off the frame
thread, so the deadline is enforceable.** New claim
`02-architecture#interactive-default-renders-leaves-off-the-frame-thread` in
`tests/claims/registry.tsv`, hung on the doc 02 delta (D4), enforced by a new
`tests/interactive_worker_default.t.cpp`. The test drives one frame over a scene whose leaf
`render` parks on a `std::latch` the test holds, with the injected `epoch_clock()`
(`src/runtime/t/interactive.t.cpp:327-330`) so the deadline instant is already in the past —
wall-clock-free and deterministic:

- At `WorkerPoolConfig{}` (inline): the leaf's `render` is observed on the **frame thread**
  (`std::this_thread::get_id()` equality — the idiom at
  `tests/worker_dispatch_leaf_only.t.cpp:541,562`), so `render_frame` provably cannot return
  until the leaf completes.
- At `default_interactive_pool_config()`: the leaf's `render` is observed on a **worker
  thread**; `render_frame` returns with the deadline expired, having composited a degraded
  tile and owing a follow-up frame — `deadline_expiries() == 1`, `tiles_cancelled() >= 1`,
  `counters().degraded_composites() >= 1`, `FrameOutcome::schedule_follow_up == true`. The
  latch is then released and the arrival settles on a later frame
  (`counters().follow_up_frames()` bumps).

This is the whole argument for a non-zero default, stated as counters (doc 16:54-62), and it
is *only* observable once the counters of A4 exist.

**A4 — The two missing counters.** `InteractiveRenderer` gains, in the house style of
`identity_map_builds()`/`operator_binds()` (`interactive.hpp:208-220`, each with its
doc-16:54-62 rationale comment):

- `frames_rendered()` — frames that got past the still-scene early-out. The denominator for
  "renders per frame"; the numerator is the existing `counters().requests_issued()`
  (`compositor/counters.hpp:40`).
- `deadline_expiries()` — bumped where `wait_completions` returns `false`
  (`interactive.cpp:328-329`): the park reached the deadline with renders unsettled.
- `tiles_cancelled()` — bumped in the existing cancel loop (`interactive.cpp:330-334`): how
  much the frame degraded.

Existing counter tests in `src/runtime/t/interactive.t.cpp` (`:379-390`, `:425`, `:458-476`)
are extended to pin the still-scene case: an early-out frame bumps **none** of the three
(consistent with `02-architecture#interactive-still-scene-schedules-no-frame`,
`registry.tsv:151`).

**A5 — Renders-per-frame is invariant across worker count (the anti-waste guard).** In
`tests/interactive_worker_default.t.cpp`: for each of the three scenes, driven to quiescence
at worker counts `0`, `1`, `2`, `4`, `hardware_concurrency()-1`, the tuple
`(requests_issued(), operator_renders(), composites())` per rendered frame is **identical**,
and `worker_pool().max_in_flight_per_content() <= 1` (`worker_pool.hpp:128`) — i.e. adding
workers buys parallelism, never duplicate or wasted renders. Runs in the standard suite, so it
is on the per-push `gcc-tsan` lane (`.github/workflows/ci.yml:59`) with no exclusions: this is
this task's **TSan coverage** for the newly-default-threaded frame loop (Constraint 7 —
a counter bumped from a worker fails here).

**A6 — The benchmark, off the merge path.** `src/runtime/bench/interactive_worker_bench.cpp`
plus the shared workload header `src/runtime/bench/interactive_bench_workloads.hpp`,
registered `arbc_component_bench(COMPONENT runtime SOURCES bench/interactive_worker_bench.cpp)`
in `src/runtime/CMakeLists.txt` (built only under `ARBC_BENCHMARKS`, the `bench` preset,
`CMakePresets.json:77-83`). Three scenes — **leaf-heavy** (many independent leaf contents
tiled across the viewport), **operator-heavy** (fade/crossfade over leaves; every operator
render is inline by the leaf-only rule, so this scene measures that rule's ceiling), and
**nested-deep** (nested compositions) — each swept over worker counts `0/1/2/4/hw-1` with a
realistic per-frame `budget` and the real `steady_clock`. It reports wall-clock **and** emits
the counters as Google Benchmark counters (`requests_issued`, `deadline_expiries`,
`tiles_cancelled`, `degraded_composites`, `follow_up_frames`, `frames_rendered`), JSON out in
the `allocator_bench.cpp:23-24` shape for the future `quality.benchmark_history`. No assertion
of any kind lives in the bench.

**A7 — Bench bodies carry diff coverage.** New `src/runtime/t/bench_smoke.t.cpp` (registered
in `src/runtime/CMakeLists.txt`'s `arbc_component_test` list) drives each workload from
`interactive_bench_workloads.hpp` once at minimal size, exactly as
`src/pool/t/bench_smoke.t.cpp` does for the allocator. This is what keeps the ≥90% diff-coverage
gate (doc 16:113-118) satisfiable for bench-only code.

**A8 — The number is argued from data, in writing.** The implementer runs A6's benchmark and
records the resulting table (scene × worker count × counters + wall-clock, plus the machine's
core count) in this document's `## Status` block, alongside the chosen `k_max_interactive_workers`.
That table *is* the review artifact doc 16:82-87 asks for ("regressions alert, humans judge").
A `## Status` block that ships a constant without the table it came from does not close this
task.

**A9 — Deferred:** `runtime.shared_worker_pool` — "Let several `InteractiveRenderer`s share one
`WorkerPool`", `2d`, `depends runtime.interactive_worker_count_default`, milestone `m9_release`
(closer registers in WBS). `InteractiveRenderer` owns its pool by value and is non-movable
(`interactive.hpp:154`, `:270`), so a host with *K* viewports now spawns *K × N* threads where
before it spawned zero. The fix is a non-owning constructor overload taking `WorkerPool&` (and a
`std::variant`/pointer member), so a host builds one pool and hands it to every viewport. It is
not this task because it changes an ownership seam and needs its own teardown/TSan story, and
because a single-viewport host — the only one that exists today (`src/runtime/host_viewport.cpp:43`)
— is correct and well-sized without it. **This is the only deferral.**

## Decisions

**D1 — The default lives on `InteractiveRenderer`'s constructor, not on `WorkerPoolConfig`.**
Change `interactive.hpp:168` from `WorkerPoolConfig pool_config = {}` to
`WorkerPoolConfig pool_config = default_interactive_pool_config()`.

*Alternative rejected: flip `WorkerPoolConfig::worker_count`'s in-class initializer
(`worker_pool.hpp:63`) to non-zero.* It is one line and it is wrong three times over. It would
silently flip `SequenceRenderer` to the parallel-exact path by default
(`offline_sequence.cpp:58` caches `worker_count != 0`), moving the offline driver off the
byte-deterministic path doc 02:73-85 specifies as *the* offline story — a change no one asked
for, in a task scoped to the interactive driver. It would break `registry.tsv:146`'s claim that
the *default* pool is the degenerate inline executor. And it would put a policy decision
("how many threads should an interactive host use?") in a config struct that has no idea which
driver it is about to configure. Worker count is *runtime driver policy* — doc 17:110-112 puts
deadlines and frame loops in runtime for exactly this reason.

*Alternative rejected: `std::optional<std::size_t> worker_count`, with `nullopt` = "pick for
me".* It buys a third state (auto / explicit-N / explicit-inline) that nothing needs, and it
ripples through `worker_pool.cpp:12-13,66`, `offline_sequence.cpp:58`, and every construction
site — for a distinction a named default-argument function expresses for free.

**D2 — The default is a formula over `hardware_concurrency()`, capped; the benchmark picks the
cap.**

```
std::size_t default_interactive_worker_count() {
  std::size_t n = std::thread::hardware_concurrency();   // 0 == "unknown"
  if (n == 0) { n = 1; }
  return std::clamp<std::size_t>(n - 1, 1, k_max_interactive_workers);
}
```

`n - 1` because the frame thread is a participant, not an observer: it plans, composites, and
parks in `wait_completions` — leaving it a core is the point. `>= 1` because even a
single-core box benefits: the *reason* for a worker is that the frame thread can reach the
deadline park and degrade (A3), which is a latency property, not a throughput one; on one core
the worker still gets scheduled while the frame thread parks. The clamp exists because the pool
is **per-renderer** (`interactive.hpp:270`) — an unclamped `hw-1` spawns 63 threads per viewport
on a 64-core workstation, and until `runtime.shared_worker_pool` (A9) lands, a second viewport
doubles that.

*The benchmark decides `k_max_interactive_workers` from A6's counter table, over the candidate
set the WBS note names: `{2, 4}`. Tie-break, decided now so the implementer never has to
escalate: **if the counters do not separate 2 from 4 on any of the three scenes, ship 2*** —
fewer threads per viewport, less oversubscription under the pending multi-viewport hazard, and
a host that wants more can always pass its own config. The measured table goes in `## Status`
(A8).*

*Alternative rejected: a fixed constant (`worker_count = 4`, unconditionally).* Simpler to
read, but it oversubscribes a 2-core CI runner or laptop by 3× and undersubscribes a
workstation. The whole content of this task is "pick from measurement"; a constant that ignores
the machine is a measurement of the author's machine.

**D3 — The sign of the decision comes from doc 02; only the magnitude comes from the
benchmark.** The benchmark is *not* being asked "is `> 0` better than `0`?" — doc 02:61-65's
deadline promise is unachievable at `0` for a synchronous leaf (see § Why), and that is a
correctness argument, not a performance one. This matters because it fixes what happens if the
benchmark shows the leaf-heavy scene is *slower* at `N > 0` for small scenes (plausible:
`submit`'s queue push, condvar wake, and completion drain cost more than a 1 ms leaf render).
That would not reopen the default — it would mean the *frame* is not the place a small leaf
should pay for threading, and the correct response is to ship the default anyway and note the
crossover in `## Status`. A dispatch heuristic that renders "cheap" leaves inline is a
different task, and not one to invent speculatively: nothing in the design docs promises it,
and `Content` has no cost estimate to key it off.

**D4 — Design-doc delta (doc 02 § Threading model + doc 00 decision record), landed in the
implementation commit, not now.** Doc 02 currently says nothing about worker counts, and
`:149-151` explicitly licenses the single-threaded degenerate mode. Per doc 16's same-commit
rule, the implementer lands, **in the same commit as the code**:

- `docs/design/02-architecture.md` § Threading model (after the leaf-only bullet at
  `:133-146`): a bullet stating that **the interactive driver ships with a non-zero worker
  count** — that Step 4's deadline promise (`:61-65`) is unachievable when `submit` *is* the
  render, so the shipped interactive pool has real workers; that the count is runtime policy
  derived from the machine's hardware concurrency and capped, never a fixed number; and that
  the offline driver keeps inline-exact as *its* default (doc 02:73-85), because exactness has
  no deadline to miss.
- `docs/design/02-architecture.md:149-151`: amend the degenerate-single-thread caveat to say
  the interactive driver has graduated out of it (the *structure* stays — the request/completion
  seam is what let it graduate without a redesign, which is the sentence's original point).
- `docs/design/00-overview.md` § decisions: a decision-record bullet — the first shipped
  configuration with real threads under the frame loop; the default is a formula, not a
  constant; the deadline is what buys the threads, not throughput.

The delta is **specified here and written by the implementer**, deliberately: a doc that
promises a non-zero default while the shipped default is still `0` is a lie for as long as the
refinement sits ahead of the code. The predecessor landed its doc 02 delta the same way.

**D5 — One new claim id, not four.** Only A3 mints
`02-architecture#interactive-default-renders-leaves-off-the-frame-thread`, because only A3
enforces a sentence the doc 02 delta (D4) newly contains. A2's byte-identity and A5's
no-duplicate-renders are *already* covered by `registry.tsv:149`'s existing text ("the frames
are byte-identical to the `worker_count == 0` run") and get second `enforces:` tags instead.
This follows `interactive_pull_wiring.md` Decision 5 verbatim: *"do not mint a claim id for a
sentence no design doc contains."*

**D6 — Counters, not a `FrameOutcome` field, for the deadline data.** `deadline_expiries()`,
`tiles_cancelled()`, and `frames_rendered()` join `identity_map_builds()`/`operator_binds()` as
persistent driver counters (`interactive.hpp:197-220`), rather than being reported per-frame on
`FrameOutcome` (`interactive.hpp:159-161`).

*Alternative rejected: widen `FrameOutcome`.* It is deliberately a one-bit answer to "does the
host owe another frame?" — a host-facing type, not an observability channel. Doc 17:114-127 is
explicit that persistent counter state lives in `runtime` while the per-frame library stays
stateless, and every existing behavioral-counter test in the tree reads accumulated counters
(`src/runtime/t/interactive.t.cpp:379-390`). A benchmark that wants per-frame numbers snapshots
the counters around a frame — one subtraction — which is what A6 does.

**D7 — The parallel-path invariants are pinned in a per-push CTest; the tuning numbers come
from an opt-in bench.** Two artifacts, because doc 16 splits them: tier 4 (counters) gates,
tier 9 (benchmarks) trends. A5's CTest asserts only what must be true at *every* worker count
(same renders, same composites, no duplicate in-flight, byte-identical pixels) and so is safe
on every push, on TSan, on every machine. A6's bench asserts nothing and runs only under
`ARBC_BENCHMARKS`.

*Alternative rejected: one artifact — assert in CI that `N=4` beats `N=0` on the leaf-heavy
scene.* That is a wall-clock gate in the merge path, which doc 16:225-226 names as
deliberately-not-adopted, and the WBS note forbids in its own words: *"Never a wall-clock
assertion; the benchmark reports counters and the default is a reviewed choice."*

## Open questions

(none — all decided. `k_max_interactive_workers` is chosen by the implementer from A6's
measured table under D2's stated procedure, including its tie-break; it is a data lookup with a
defined fallback, not an open design question.)

## Status

**Done** — 2026-07-12.

- Shipped `default_interactive_pool_config()` = `clamp(hardware_concurrency-1, 1, k_max_interactive_workers=2)`
  in `src/runtime/arbc/runtime/interactive.hpp` and `src/runtime/interactive.cpp`;
  installed as `InteractiveRenderer`'s constructor default (D1/D2).
- Added `frames_rendered()`, `deadline_expiries()`, `tiles_cancelled()` counters
  to `InteractiveRenderer` (`src/runtime/arbc/runtime/interactive.hpp`); fixed a
  real deadline-enforcement bug where a fast-settling tile suppressed expiry while
  the deadline was provably gone (`src/runtime/interactive.cpp`).
- New claim `02-architecture#interactive-default-renders-leaves-off-the-frame-thread`
  in `tests/claims/registry.tsv`; enforcing test `tests/interactive_worker_default.t.cpp`
  (A1, A3, A4, A5; 936/936 green, TSan-clean).
- Goldens parameterized over `{WorkerPoolConfig{}, default_interactive_pool_config()}`
  via `GENERATE` in `tests/interactive_operator_binding_golden.t.cpp` and
  `tests/host_viewport_reanchor_golden.t.cpp`; byte-identical — zero golden re-baselining (A2).
- Google Benchmark suite `src/runtime/bench/interactive_worker_bench.cpp` + shared
  workload header `src/runtime/bench/interactive_bench_workloads.hpp`; bench-smoke
  `src/runtime/t/bench_smoke.t.cpp` carries diff coverage (A6/A7).
- Doc 02/00 deltas landed in `docs/design/02-architecture.md` and `docs/design/00-overview.md`
  per doc-16 same-commit rule (D4).
- Deferred follow-up registered: `runtime.shared_worker_pool` (A9) — K viewports
  currently spawn K×N threads; fix is a non-owning `WorkerPool&` constructor overload.

**Benchmark table** (A8 — idle 16-core box, `hardware_concurrency=16`, shipped default = `k_max_interactive_workers=2`):

| scene | w0 | w1 | **w2** | w4 | w15 |
|---|---|---|---|---|---|
| leaf_heavy | 48.6 ms | 45.7 | **40.0** | 35.1 | 36.0 |
| operator_heavy | 2153 ms | 2300 | **2091** | 2100 | 2178 |
| nested_deep | 3027 ms | 6413 | **6247** | 6291 | 6442 |

At `w0`: `degraded_composites=0` and `follow_up_frames=0` in every scene — the
degrade/refine machinery never fires, which is the empirical form of "at
`worker_count==0` there is nothing to degrade to." The case for non-zero is
correctness (doc 02:61-65 deadline promise), not throughput. Beyond 2 workers
buys nothing (operator_heavy and nested_deep are flat; operators render inline
by the leaf-only rule). `nested_deep` is ~2× slower at any non-zero worker count
due to `compositor.in_flight_tile_dedup` waste (already registered); trades
nested-scene throughput for an enforceable deadline until that task lands.
