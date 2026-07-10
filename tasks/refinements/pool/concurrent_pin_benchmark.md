# pool.concurrent_pin_benchmark — Concurrent pin-interference benchmark

## TaskJuggler entry

`tasks/05-pool.tji` → `pool.concurrent_pin_benchmark` ("Concurrent
pin-interference benchmark"). Task block at `tasks/05-pool.tji:96-101`;
`depends !benchmarks`; wired to `milestones.m9_release`
(`tasks/99-milestones.tji:69-71`).

## Effort estimate

1d.

## Inherited dependencies

- `pool.benchmarks` — **settled** (commit landing 2026-07-04,
  `tasks/refinements/pool/benchmarks.md:309-318`). Provides the entire seam
  this task extends rather than builds:
  - The gated Google Benchmark harness: `ARBC_BENCHMARKS` option (default
    OFF, `CMakeLists.txt:64`), `FetchContent_Declare(benchmark …)`
    (`CMakeLists.txt:91-109`), the `arbc_component_bench` helper
    (`cmake/ArbcComponent.cmake:58-73`), the Release `bench` preset
    (`CMakePresets.json:77-106`), and the pool registration
    (`src/pool/CMakeLists.txt:16`). This task adds **no** new build seam —
    it registers additional benchmarks in the existing target.
  - The producer/consumer workload substrate
    (`src/pool/bench/pool_bench_workloads.hpp`): `BenchNode` (managed,
    `SlotRef` children, releasing destructor) and `SharedNode`
    (`std::shared_ptr` children, `make_shared`-allocated — the co-located
    control-block layout doc 15:59 names), the tree builders, and the
    traversal primitives `traverse_managed_peek` (`:126`),
    `traverse_shared_constref` (`:141`).
  - The registration file (`src/pool/bench/allocator_bench.cpp`): the
    existing solo-traversal benches `BM_Traversal_Managed_Peek` (`:70`) and
    `BM_Traversal_Shared_ConstRef` (`:82`) are the **no-concurrent-pin
    reference** the new interfered benches trend against; `BENCHMARK_MAIN()`
    (`:183`).
  - The bench-smoke coverage pattern (`src/pool/t/bench_smoke.t.cpp`):
    drives workload bodies once under the normal `dev`/`asan` build with
    behavioral assertions, and the `MmapRefcountBacking` `mprotect` witness
    (`:134-175`) enforcing `15-memory-model#const-ref-traversal-touches-no-refcount-page`
    (`:177-204`).
  - The two settled claims this task composes:
    `15-memory-model#const-ref-traversal-touches-no-refcount-page`
    (`tests/claims/registry.tsv:58`) — a `peek` traversal writes no count
    page — and `15-memory-model#refcounts-outside-data-pages`
    (`registry.tsv:51`, enforced by `src/pool/t/refs.t.cpp:311` via
    `source.protect(PROT_READ)`) — pin/unpin churn writes no data page.
- `pool.reclamation` / `pool.free_pools` — **settled** (transitively via
  `benchmarks`). Provide the any-thread `retain`/`release`/`peek`
  (`src/pool/arbc/pool/refs.hpp:313,324`), the writer-only `create`, the
  deferred reclaim path, and the thread-local free pools with global spill
  that make cross-thread pin/release safe (`registry.tsv:60-61`). A
  concurrent pinner is exactly the cross-thread release traffic those tasks
  admit.
- Concurrent pin/peek substrate — **settled**:
  `tests/stress_publish_pin.t.cpp` already races a pinner loop (pin → peek
  → unpin) against a committing writer and a `HousekeepingThread` drainer
  under seeded schedule perturbation, on the `tsan` lane, enforcing
  `15-memory-model#const-ref-traversal-touches-no-refcount-page` and
  `15-memory-model#housekeeping-thread-single-drainer`. This task reuses
  that harness's shape for the new claim's concurrent witness.

## What this task is

One concurrent producer/consumer benchmark, plus its non-wall-clock
witnesses, that isolates the single performance advantage the settled
single-threaded rerun (`pool.benchmarks`) provably **cannot** capture: the
*interference-free concurrent pin* (doc 15:96-100). A producer thread churns
version pins — `retain`/`release` on the structurally-shared interior nodes
of a published version — while a consumer thread `peek`-traverses that same
version. In `arbc::pool` the pin traffic lands in the parallel count column,
never on the immutable data pages the consumer reads, so the consumer's
traversal does not degrade under the concurrent pinner. In the `make_shared`
baseline the control block is co-located with the node data (doc 15:58-62),
so every `retain`/`release` dirties the exact cache line the consumer is
reading, and the traversal degrades under cross-core coherency traffic.

The task lands three artifacts, all inside the existing `pool` bench seam:

1. **Two new benchmarks** (`src/pool/bench/allocator_bench.cpp`, workload
   bodies in `src/pool/bench/pool_bench_workloads.hpp`):
   `BM_ConcurrentPin_Managed` and `BM_ConcurrentPin_Shared`, each timing the
   **consumer** traversal while a background producer churns pins on the
   shared interior nodes. The existing solo-traversal benches are the
   uncontended reference; the interference-free story is that the managed
   interfered/solo ratio stays ≈ 1 while the shared ratio climbs. Timing
   trends per-commit through the JSON output — never a quoted ratio, never a
   gate (doc 16:82-87, 225-226).
2. **A bench-smoke coverage carrier** (`src/pool/t/bench_smoke.t.cpp`):
   drives the concurrent workload body for a bounded op count under the
   normal `dev`/`asan` build, asserting the **behavioral** facts — the
   consumer observed no torn value and the arena returns to its pre-run
   `total_slots_live()` baseline after `drain()`. No timing.
3. **One new claim**,
   `15-memory-model#interference-free-concurrent-pin`, the concurrent
   composition of the two settled single-threaded page-cleanliness claims,
   machine-checked by a deterministic two-thread test with the data chunks
   `mprotect`ed read-only (Linux-guarded, also on the `tsan` lane).

## Why it needs to be done

- **It is the honest concurrent performance story `pool.benchmarks`
  deferred.** The settled rerun showed the managed single-threaded `peek`
  traversal is ~2.8× *slower* than a fair `const&` `shared_ptr` walk
  (doc 15:88-100), because the 4-byte index-only `SlotRef` pays a two-level
  directory resolve per edge. Doc 15:96-100 is explicit that the design's
  read-path win is therefore **not** a traversal speedup but the
  interference-free concurrent pin, "none of which a single-threaded
  traversal ratio captures." That measurement does not exist yet; this task
  produces it. Without it, `arbc::pool`'s only quoted read-path numbers are
  the ones where it loses — the concurrent advantage that justifies the
  inside-out layout is asserted in prose but never measured.
- **It gates M9.** `m9_release` (`tasks/99-milestones.tji:71`) lists
  `pool.concurrent_pin_benchmark` among its dependencies; the v0.1 release's
  performance story for `arbc::pool` is incomplete until the concurrent
  advantage is trended, not merely claimed.
- **It closes the loop `pool.benchmarks` opened.** That task registered this
  follow-up precisely because its single-threaded harness could not witness
  the concurrent property (`benchmarks.md:318`); this task discharges the
  registered debt.

## Inputs / context

- `docs/design/15-memory-model.md`:
  - **Inside-out refcounts** (lines 37-44): "refcounts live in *parallel*
    buffers, not next to the data. After construction, data pages are never
    written again — reader threads traversing a version touch only genuinely
    immutable pages (no refcount-dirtied cache lines … safely shareable
    across cores)" and "only ownership points bump counts, so *reads don't
    touch refcount pages at all*." The two-sided disjointness the new claim
    composes.
  - **The interference subtlety** (lines 58-62): "with `make_shared`-style
    layouts, *pinning and unpinning versions from the render thread would
    dirty the same cache lines the traversal reads*; inside-out storage
    makes version pins interference-free." This is the exact property the
    benchmark measures and the baseline the `make_shared` arm reproduces.
  - **Caveat 3, the read-path win** (lines 96-100): "the design's read-path
    win is the *interference-free concurrent pin* (a producer
    pinning/unpinning versions never dirties the cache lines the consumer
    reads — the property caveat's `const-ref` claim guards) … none of which
    a single-threaded traversal ratio captures. Numbers trend per-commit
    through the benchmark's JSON output, never a quoted ratio in the docs."
  - **Thread rules** (lines 140-141): "Render and audio-engine threads may
    pin/unpin (one refcount op) and enqueue reclamation, nothing more" — the
    producer's op is exactly this single refcount op, any-thread.
  - **The parallel count column** (lines 225-236): counts live in a buffer
    "owned by the size-class slab store and indexed by **physical slot**,
    not by the typed view over it." The page the producer dirties; disjoint
    from the data chunk the consumer reads.
  - **The inside-out split is the persistence split** (lines 184-190):
    refcounts are anonymous runtime state, data buffers are the file
    mapping — the same physical-page separation the `mprotect` witness
    exploits.
- `docs/design/16-sdlc-and-quality.md`:
  - **Behavioral-counter tests** (lines 54-62): "Wall-clock tests lie in CI;
    counters don't." The gating assertions here are of that kind (no torn
    read, arena-back-to-baseline), never timing.
  - **Benchmarks** (lines 82-87): Google Benchmark, results "uploaded
    per-commit to a tracked history … regressions alert, humans judge —
    wall-clock gates stay out of the merge path."
  - **Wall-clock gates rejected** (lines 225-226): "behavioral counters
    gate, benchmarks trend."
  - **Claims register** (lines 14-21): each normative claim gets an id and a
    test referenced by `// enforces: <claim-id>`; CI fails if a registered
    claim has no live test.
  - **Short-form / nightly** (lines 101-105): a small per-push seed range,
    the wide sweep nightly.
- Source seams:
  - `src/pool/bench/pool_bench_workloads.hpp` — `BenchNode` (`:44`),
    `SharedNode` (`:71`, `make_shared`-built at `:96` of the version
    builder), `traverse_managed_peek` (`:126`), `traverse_shared_constref`
    (`:141`). The concurrent workload adds a producer thunk that
    `retain`/`release`es the shared interior nodes and a driver that runs it
    against a consumer traversal.
  - `src/pool/bench/allocator_bench.cpp` — existing solo benches at
    `:70,:82`; the new registrations sit beside them, before
    `BENCHMARK_MAIN()` (`:183`).
  - `src/pool/arbc/pool/refs.hpp` — `peek` (`:313`, zero-count traversal),
    `retain` (`:324`, overflow-checked CAS), `release`, `slots_live`
    (`:457`), `reclaim_chunks_published` (`:467`); `RefcountTableBacking`
    hook (`:52,:254`).
  - `src/pool/arbc/pool/slot_store.hpp` — `Arena::total_slots_live()`
    (`:354`), the physical-slot count column, `spill_count()`/`refill_count`
    (`:257-258`).
  - `src/pool/t/bench_smoke.t.cpp` — the bench-smoke pattern; the
    `MmapRefcountBacking` `mprotect` count-table witness (`:134-175`) and the
    const-ref claim test (`:177-204`).
  - `src/pool/t/refs.t.cpp:311` — the `refcounts-outside-data-pages`
    witness: a page-aligned mprotectable data source, `source.protect(
    PROT_READ)`, then reference churn proceeds without faulting. The new
    claim's test extends this pattern with a concurrent traverser.
  - `tests/stress_publish_pin.t.cpp` — the seeded pin/peek/unpin concurrency
    harness (`Perturber`, `tests/support/schedule_perturb.hpp`) and the
    natural home for the new concurrent claim test; already on the `tsan`
    lane.
- Build / CI seams:
  - `CMakePresets.json` — `bench` preset (`:77-106`, Release), `tsan` preset
    (`:32-40`).
  - `.github/workflows/ci.yml:47` — the `gcc-tsan` per-push lane;
    `.github/workflows/nightly.yml:44` — the `tsan-full` nightly sweep.
- `docs/design/17-internal-components.md` — pool is Level 1 (depends `base`
  only); benchmark and test TUs link the umbrella `arbc` and use only
  `arbc::pool` + `arbc::base` + `std`. The `make_shared` baseline is pure
  `std`. No new component edge.

## Constraints / requirements

- **No wall-clock assertion anywhere.** The interference-free advantage is
  intrinsically a timing property (throughput does not degrade); it therefore
  **trends** via the benchmark JSON and is never gated (doc 16:82-87,
  225-226). Every "done" check is a behavioral counter (no torn read,
  arena-to-baseline) or a page-disjointness `mprotect` witness — never a
  duration or a ratio threshold.
- **Model structural sharing so pin traffic hits the read set.** The
  producer must `retain`/`release` the *interior* nodes the consumer
  traverses (the shared subtree of a persistent version), not merely a root
  handle. Pinning only a root would dirty one line and understate the
  effect; the honest scenario — and the one doc 15:58-62 describes — is
  concurrent refcount traffic on the shared nodes a pinned reader is
  reading. This is the property a single-threaded rerun structurally cannot
  exercise.
- **The baseline is `make_shared`, co-located count and data.** The
  `shared_ptr` arm must use the existing `SharedNode`/`make_shared`
  substrate (control block adjacent to node data), reproducing the layout
  doc 15:59 names as the one whose pins dirty the traversal's cache lines. A
  separately-allocated `shared_ptr` (control block off to the side) would
  understate the interference and is not the honest comparator.
- **Benchmark the shipped `arbc::pool`.** Use `RefStore`/`SlotRef`/`peek`
  and the any-thread `retain`/`release` as they ship; no fork, no vendored
  prototype (inherited from `benchmarks.md`'s decision).
- **Off the default build; no merge-path cost.** The new benches compile
  only under `ARBC_BENCHMARKS=ON` (the `bench` preset). `dev`/`asan`/
  `coverage` neither fetch nor build Google Benchmark. The concurrent
  workload body lives in the shared `pool_bench_workloads.hpp` so the
  bench-smoke CTest can drive it under the normal build for coverage.
- **CI upload stays out of scope.** JSON emission is native Google
  Benchmark; the per-commit upload / regression-alert wiring is
  `quality.benchmark_history`'s (`tasks/70-quality.tji:26-30`), unchanged
  here — one owner of the upload path.
- **Levelization (doc 17).** Benchmark and test code stay at Level 1.
- **Concurrency coverage is explicit.** The concurrent workload's
  correctness runs under the existing `tsan` preset and the `gcc-tsan`
  per-push lane (`ci.yml:47`), with the wide seed sweep on the `tsan-full`
  nightly lane (`nightly.yml:44`) — the concurrent claim test and the
  bench-smoke driver both build clean there. (The "no TSan lane" caveat the
  older pool refinements parked is stale — the lane exists.)
- **Coverage-exclusion justification.** Timing-only lines the bench-smoke
  pass cannot reach carry a `GCOV_EXCL` comment justified "trend-only,
  exercised by the nightly bench run" (doc 16:114-116), not a gate-game.

## Acceptance criteria

- **Concurrent benchmarks registered and green under `bench`.**
  `BM_ConcurrentPin_Managed` and `BM_ConcurrentPin_Shared`
  (`src/pool/bench/allocator_bench.cpp`, bodies in
  `pool_bench_workloads.hpp`) each time a consumer `peek`/`const&`
  traversal of a shared version while a background producer churns
  `retain`/`release` on that version's interior nodes.
  `cmake --preset bench && cmake --build --preset bench && ctest --preset
  bench` builds and runs them; `cmake --preset dev` neither fetches nor
  builds Google Benchmark. JSON emits via
  `--benchmark_format=json --benchmark_out=<file>` unchanged. No timing
  assertion enters the benchmark, the smoke test, or any gate.
- **Bench-smoke coverage carrier** (`src/pool/t/bench_smoke.t.cpp`, in the
  normal `dev`/`asan` build): drives the concurrent workload body for a
  bounded op count on both the managed and `make_shared` substrates and
  asserts the behavioral facts —
  - the consumer observed **no torn value** (every node in the pinned
    version still carries its published payload throughout the concurrent
    pin churn), and
  - after joining the producer and `drain()`ing, the arena returns to its
    pre-run `Arena::total_slots_live()` baseline
    (`enforces: 15-memory-model#slots-recycle-in-place`, existing claim; and
    exercising the cross-thread release path of
    `15-memory-model#thread-local-free-pools-spill-to-global`, existing
    claim — no new claim needed for either).
- **New claim (register row + `enforces:` test)**
  `15-memory-model#interference-free-concurrent-pin`: a producer thread's
  version-pin churn (`retain`/`release` on the shared interior nodes) and a
  consumer thread's `peek` traversal of that version run **concurrently** on
  one size-class store with the **data** chunks `mprotect`ed read-only; the
  producer's count-only writes never fault the read-only data pages and the
  consumer observes no torn value, so the two threads' page-write and
  page-read sets are disjoint under real contention. Enforced by a
  deterministic two-thread test (Linux-only, `mprotect`-guarded), homed
  alongside the concurrent pin substrate in `tests/stress_publish_pin.t.cpp`
  and extending the mprotectable-data-source pattern of `refs.t.cpp:311`;
  built clean on the `tsan` lane. Row added to `tests/claims/registry.tsv`.
  This is the concurrent composition the two single-threaded claims
  (`#const-ref-traversal-touches-no-refcount-page`,
  `#refcounts-outside-data-pages`) do not individually witness — the
  concurrency, the shared read/write set, and the no-torn-read are the new
  facts.
- **Design-doc delta (same commit, doc 16 rule).** The implementer adds a
  factual pointer at `docs/design/15-memory-model.md:96-100` — from "the
  property caveat's `const-ref` claim guards" to also cite the concurrent
  benchmark (`src/pool/bench/`) and the new
  `15-memory-model#interference-free-concurrent-pin` claim, mirroring how
  caveat 3 already points at `allocator_bench.cpp` and the const-ref claim.
  This is a pointer to the now-measured concurrent property, not an
  architectural change, so it needs no doc-00 decision bullet.
- **Coverage:** ≥90% diff coverage on changed lines — the bench-smoke driver
  and the claim test carry the workload body; CMake lines are unchanged
  (no new build seam). Gate green including the ASan and `tsan` lanes.

## Decisions

- **Extend the existing `pool` bench seam; add no new build wiring.**
  `pool.benchmarks` already landed `ARBC_BENCHMARKS`, `arbc_component_bench`,
  the `bench` preset, and the producer/consumer workload substrate. This
  task registers two more benchmarks and one workload body in that seam.
  *Rejected:* a separate benchmark target or a second workloads header — it
  would fragment pool's bench code for no gain; the concurrent workload is
  the same tree topology with a second thread.
- **Time the consumer; run the producer as a background pin-churn thread.**
  The advantage is *the consumer's traversal not degrading*, so the consumer
  is the timed axis and the producer is the interference source; the
  existing solo-traversal benches are the uncontended reference the trend
  compares against. *Rejected:* timing the producer (version-production rate)
  — that axis is the `benchmarks.md` version-production bench, and it is not
  where the interference-free property shows; *rejected:* a single fused
  metric — keeping solo and interfered as separate trended series makes the
  degradation delta (managed ≈ 1, shared > 1) legible in the history.
- **The pin churn targets shared interior nodes, not a root handle.** The
  honest, doc-15:58-62 scenario is refcount traffic on the shared subtree a
  pinned reader is reading; root-only pinning would dirty one line and
  understate the effect. This is also precisely what a single-threaded rerun
  cannot do — the reason the task exists. *Rejected:* pin/unpin of the
  version root only — cheaper to write but measures a strawman.
- **`make_shared` is the baseline, deliberately.** The co-located control
  block is the layout whose refcount ops dirty the data cache line
  (doc 15:59); it is the honest comparator, and it is already the substrate
  `benchmarks.md` chose. *Rejected:* an off-to-the-side `shared_ptr(new T)`
  control block — it would understate the interference and contradict the
  layout the doc names.
- **The new claim is the concurrent page-disjointness witness, not a perf
  ratio.** The timing trends ungated (doc 16:225-226); the *architectural*
  fact underneath — pin churn and traversal touch disjoint pages **under
  concurrency** — is behavioral and gateable, so it becomes the registered
  claim, exactly the pattern `benchmarks.md` set for the single-threaded
  case. *Rejected:* registering a speedup/no-degradation figure as a claim —
  a wall-clock gate in disguise; *rejected:* reusing only the two existing
  single-threaded claims — neither witnesses the concurrency, the shared
  read/write set, or the no-torn-read that are this task's whole point.
- **Concurrency coverage rides the existing `tsan` lane.** The `tsan` preset
  and `gcc-tsan`/`tsan-full` CI lanes already exist and already carry
  `stress_publish_pin.t.cpp`; the new claim test and the bench-smoke driver
  join them. *Rejected:* parking TSan as a future task the way the older
  pool refinements did — that gap is closed.
- **CI tracked-history wiring stays out of scope.** Unchanged from
  `benchmarks.md`: `quality.benchmark_history` owns the upload path; this
  task stops at JSON-emitting benchmarks. *Rejected:* touching
  `nightly.yml`/`github-action-benchmark` here — two owners of one path.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-10.

- Added concurrent-pin workload bodies to `src/pool/bench/pool_bench_workloads.hpp`: `collect_interior_slots_managed`, `churn_pins_managed`, `collect_interior_shared`, `churn_pins_shared`.
- Registered `BM_ConcurrentPin_Managed` and `BM_ConcurrentPin_Shared` in `src/pool/bench/allocator_bench.cpp` (consumer timed; background producer churns `retain`/`release` on shared interior nodes).
- Added bounded concurrent-churn coverage carrier (managed + make_shared sections) to `src/pool/t/bench_smoke.t.cpp`; asserts no torn read and arena-to-baseline after `drain()`.
- Added `MmapRecordingSource` and the new claim's two-thread `mprotect` witness to `tests/stress_publish_pin.t.cpp`.
- Registered claim `15-memory-model#interference-free-concurrent-pin` in `tests/claims/registry.tsv`.
- Added pointer in `docs/design/15-memory-model.md` at caveat-3 to the concurrent benches and the new claim.
