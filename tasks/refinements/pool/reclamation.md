# pool.reclamation — Deferred reclamation

## TaskJuggler entry

`tasks/05-pool.tji` → `pool.reclamation` ("Deferred reclamation").

## Effort estimate

2d.

## Inherited dependencies

- `pool.refs` — **settled** (commit `3898878`). Provides the exact seams
  this task plugs into:
  - The zero-count sink interface `ZeroCountSink::on_zero(SlotIndex)` and
    the per-store swap point `RefStore<T>::set_zero_sink(ZeroCountSink*)`
    (`src/pool/arbc/pool/refs.hpp:42-50`, `:350-352`) — refs.hpp shipped
    an *immediate* sink for tests and explicitly reserved this seam for
    "the deferred reclamation queue (pool.reclamation)".
  - `RefStore<T>::reclaim(SlotIndex)` — run `~T`, return the slot to the
    free list, and (debug) bump the generation
    (`src/pool/arbc/pool/refs.hpp:339-348`). The doc comment already names
    it "what a deferred queue calls when it pops."
  - `RefStore<T>::release`/`release_index` — the last decrement dispatches
    to the sink (`src/pool/arbc/pool/refs.hpp:414-420`).
  - The inside-out parallel-table publish pattern
    (`ensure_parallel_chunks`, `src/pool/arbc/pool/refs.hpp:381-389`) and
    the `SlabDirectory<std::atomic<std::uint32_t>>` tables
    (`:436-438`) — the reclaim-link table added here mirrors the
    generation table exactly.
- `pool.arena_core` — **settled** (commit `32c0d5d`). Provides the
  release-does-not-destroy contract: `SlotStore::release` marks a slot reusable **without**
  running `~T` and is **writer-thread-only** (a debug assert binds it to
  the first releasing thread), with a LIFO free list kept outside the data
  pages (`src/pool/arbc/pool/slot_store.hpp:49-57`, `:73`, `:101`);
  `TypedStore<T>::destroy` = `~T` then `release`, `TypedStore<T>::release`
  = release without `~T` (`src/pool/arbc/pool/typed_store.hpp:42-49`).

## What this task is

Deferred reclamation (design doc 15, "Version reclamation: refcounts as
the GC, budgets as the policy"): when a reference count hits zero, release
must **enqueue** the slot on a type-erased reclamation queue rather than
destroy it inline, so that dropping the last reference to a large subtree
never makes the releasing thread (potentially a render or audio-engine
thread) run thousands of destructors. A later **drain** pass pops the
queue, runs each object's destructor — whose child-reference releases
enqueue *their* targets onto the same queue — and continues the cascade
**iteratively** (a loop, not recursion): bounded stack, bounded latency,
warm slots back to the free list. Running the destructors is precisely
what fixes cpioo's missing-destructor gap (doc 15 "gap 1"): child
refcounts that cpioo never decremented on slot reuse now get decremented,
so nested subtrees no longer leak.

Concretely this task ships:

1. A `DeferredReclaimSink<T>` — a `ZeroCountSink` installed on a
   `RefStore<T>` via the existing `set_zero_sink` seam — whose
   `on_zero(index)` performs a **lock-free, allocation-free** push of the
   slot onto an anonymous inside-out reclaim-link stack, touching no data
   page and taking no lock (RT-thread-safe).
2. A central `ReclamationQueue` that registered stores drain through; its
   `drain()` pops and reclaims across all registered stores until global
   quiescence, driving the iterative destructor cascade.

## Why it needs to be done

- It is the deferred half of the ownership model doc 15 promises: `pool.refs`
  shipped the immediate sink as a stopgap "for tests" and named this task
  as its replacement (`src/pool/arbc/pool/refs.hpp:44-45`).
- It fixes cpioo's real correctness bug (leaked child references on slot
  reuse) — the whole reason the model layer's node types (composition /
  layer / content records, doc 14) can hold `SlotRef` edges at all.
- Direct downstream WBS consumers gate on it:
  - `runtime.housekeeping` (`tasks/65-runtime.tji:36-40`, depends
    `pool.reclamation`) owns the **drain *policy*** — when to run the pass
    (writer thread between transactions / low-priority thread), checkpoint
    cadence, GC observability. This task must therefore expose the drain as
    a callable *mechanism*, not schedule it.
  - `pool.checkpoints` (`tasks/05-pool.tji:47-52`, depends
    `pool.reclamation`) adds the **durability-epoch quarantine fence**
    ("reusable after checkpoint N", doc 15:183-194) between "reclaimed" and
    "free-listed". This task must leave that interposition point clean, not
    build it.
  - `pool.benchmarks` (`tasks/05-pool.tji:59-63`) measures arena
    churn/reclaim.
  - M1 (`tasks/99-milestones.tji:14-16`, "Memory foundation") depends on
    checkpoints/crash_tests/benchmarks, all transitively on this task.

## Inputs / context

- `docs/design/15-memory-model.md`:
  - "Version reclamation: refcounts as the GC, budgets as the policy"
    (heading line 90), the normative section. The governing bullet
    **"Cascades are deferred, never inline"** (lines 107-114): "Release
    enqueues the object on a type-erased **reclamation queue**; a
    housekeeping pass — writer thread between transactions, or a
    low-priority thread — pops, runs destructors (fixing cpioo gap 1),
    whose child-release enqueues continue the cascade *iteratively*:
    bounded stack, bounded latency, amortized cost, and free slots return
    to the pools warm."
  - **"Thread rules"** (lines 115-121): the audio callback touches no
    allocator/refcount ever; "Render and audio-engine threads may
    pin/unpin (one refcount op) and enqueue reclamation, nothing more";
    "The writer thread is the only structural allocator."
  - **"Document teardown"** (lines 122-125): arena drop, "no destructor
    storm on close (bulk-release path runs the reclamation queue to
    quiescence first for types with external resources)."
  - **"Evaluation: poc-inside-out-objects (cpioo)"** (heading line 27),
    caveat 1 **"Releases never run `~T`"** (lines 64-69): `refcnt_subtract`
    pushes the slot to the free pool without destroying the object, so for
    node types holding child references "child refcounts are never
    decremented on reuse: a leak." The witness test cpioo shipped is
    `t/003_deeply_nested.t.cpp` (doc 15:29-30).
  - Durability-epoch interaction (lines 183-194) — the fence
    `pool.checkpoints` adds; anonymous-rebuilt-on-open framing (166-168):
    the reclamation queue and free pools are runtime state, never
    persisted.
- `src/pool/arbc/pool/refs.hpp` — the seams enumerated under Inherited
  dependencies (`ZeroCountSink`, `set_zero_sink`, `reclaim`,
  `release_index`, `ensure_parallel_chunks`, the `SlabDirectory` tables).
- `src/pool/arbc/pool/slot_store.hpp:49-57,73,101` — the writer-only,
  release-doesn't-destroy free-list contract.
- `src/pool/arbc/pool/typed_store.hpp:42-49` — `destroy` vs `release`.
- `tasks/refinements/pool/refs.md` — the sink-based-zero-handling decision
  (lines 68-73, 99-103) this task realizes.
- `docs/design/17-internal-components.md` — the `base ← pool` levelization
  this task must not widen (it stays inside `arbc::pool`, uses only `base`).

## Constraints / requirements

- **Release enqueues, never destroys inline.** The installed
  `DeferredReclaimSink<T>::on_zero` must not run `~T`, must not call
  `SlotStore::release`, must not allocate, and must not take a lock — it
  only records the slot for later. Verified as observable behavior (a
  destructor counter that stays at zero across release-to-zero and only
  moves on drain), not just by inspection.
- **RT-safe, allocation-free enqueue.** `on_zero` runs a single lock-free
  CAS push onto an anonymous per-store **reclaim-link stack**: a parallel
  `SlabDirectory<std::atomic<SlotIndex>>` table indexed by `SlotIndex`
  (the inside-out sibling of the refcount/generation tables) plus one
  `std::atomic<SlotIndex>` head per store. The link table is published
  **writer-only at `create` time**, in lock-step with the refcount table
  via the existing `ensure_parallel_chunks` path — so the enqueue path
  itself never publishes a chunk and never allocates. Empty sentinel is
  `SlotIndex(0xFFFFFFFF)` (slot 0 is a valid index). Because the table is
  anonymous, enqueue touches no data page — consistent with
  `15-memory-model#refcounts-outside-data-pages`.
- **Iterative cascade on drain.** Drain detaches a store's stack with a
  single `exchange(head, nil)` (single-consumer pop — no CAS-pop loop, so
  no Treiber-stack ABA hazard), then for each popped index calls
  `RefStore<T>::reclaim(index)`. `reclaim` runs `~T`, whose member
  `Ref`/`SlotRef` releases decrement child counts and re-enter
  `on_zero` → pushing children onto (this or another registered store's)
  reclaim stack. Drain loops over all registered stores until a full pass
  finds every stack empty (quiescence). The C++ call stack stays O(1) in
  subtree depth — the recursion is unrolled through the queue.
- **Type erasure = per-store drain thunk.** The `ReclamationQueue` holds a
  registry of `{ void* store, bool (*drain_pending)(void*) }` — a function
  pointer that `static_cast`s back to `RefStore<T>*` and drains one batch,
  returning whether it did work. No per-slot type tag; type is recovered
  from the store, which is already type-segregated (one store per size
  class). This is the minimal erasure the cascade needs and keeps the hot
  enqueue path free of any tag write.
- **Drain is writer-thread-only** (this task). `reclaim` bottoms out in
  `SlotStore::release`, which arena_core binds to the writer thread. So RT
  threads *enqueue only* (honoring doc 15's thread rules); the drain runs
  on the writer thread. The doc's "low-priority thread" drain variant
  requires cross-thread `SlotStore::release` into thread-local free pools,
  which arena_core does not yet permit — **deferred** (see Decisions and
  Acceptance criteria).
- **Reuse the `set_zero_sink` seam.** Installing deferred reclamation is
  `store.set_zero_sink(&deferred)`; passing `nullptr` restores the
  immediate sink (already implemented). The additive change to
  `refs.hpp`/`RefStore` is limited to (a) publishing the reclaim-link
  chunk inside `ensure_parallel_chunks` and (b) an `enqueue_reclaim(index)`
  push + a `drain_pending()` batch-pop the sink/queue call — mirroring the
  existing generation-table members. No design-doc behavior changes (doc 15
  already designs this), so **no design-doc delta is required**.
- **`set_count`/reconstruction stays intact.** The reclaim-link table is
  anonymous runtime state; on workspace open the reachability walk rebuilds
  counts (doc 15:166-168) and the reclaim stack starts empty — nothing on
  it is persisted. No new persistence surface.
- **Teardown drains first.** Expose a `drain()` / drain-to-quiescence entry
  the document-teardown path (doc 15:122-125) and `runtime.housekeeping`
  call before dropping arenas, so externally-resourced types get their
  destructors before storage goes away. (Driving it on a schedule is
  `runtime.housekeeping`'s job, not this task's.)
- **Levelization**: everything lands in `arbc::pool`, header
  `src/pool/arbc/pool/reclamation.hpp` (+ any minimal `refs.hpp` seam),
  depending only on `base`. No new component edge (doc 17).

## Acceptance criteria

- **Unit tests** (`src/pool/t/reclamation.t.cpp`, wired into
  `src/pool/CMakeLists.txt`'s `arbc_component_test`):
  - Install a `DeferredReclaimSink<T>`; release a slot to zero →
    destructor has **not** run and `slots_live()` is unchanged; after
    `drain()` the destructor ran exactly once and the slot returned to the
    free list (next `create` reuses it).
  - Round-trip against the immediate sink: `set_zero_sink(nullptr)` restores
    inline reclamation (the refs.md behavior still holds).
  - Empty-drain and double-drain are no-ops; drain of a store with no
    installed deferred sink is a no-op.
- **Claim (register + `enforces:`)**
  `15-memory-model#release-enqueues-never-destroys-inline` — a
  destructor-counting type proves release-to-zero leaves the counter at 0
  and the object bit-live; only `drain()` advances it. Behavioral, not
  wall-clock.
- **Claim (register + `enforces:`)**
  `15-memory-model#deferred-cascade-reclaims-whole-subtree` — port cpioo's
  deeply-nested witness (`t/003_deeply_nested.t.cpp`): build an N-deep
  chain of nodes each holding a retained `SlotRef` to its child, release
  the root, `drain()` once → **all N** destructors ran exactly once, the
  arena's live count returns to the pre-build baseline (leak check =
  doc 15:127-132), and a cascade depth probe confirms the C++ stack stayed
  bounded (independent of N — the iterative-not-recursive promise). This is
  the executable proof that cpioo gap 1 is fixed.
- **Behavioral-counter assertions**: across a burst of `on_zero` enqueues,
  `SlotStore` allocation and `slots_live()` are unchanged (enqueue allocates
  nothing / frees nothing); the reclaim-link table's chunk-publish count
  only advances at `create`, never on the enqueue path.
- **Data-page cleanliness**: with data chunks `mprotect`ed read-only
  (extending the harness of
  `15-memory-model#refcounts-outside-data-pages`), a burst of
  release-to-zero enqueues proceeds without faulting — the reclaim-link
  table is anonymous (Linux-only, guarded).
- **Concurrency (TSan, explicit per doc 16)**: a multi-producer smoke —
  several threads each release their own slots to zero concurrently while
  the writer thread drains — runs clean under TSan/asan, every destructor
  fires exactly once, and the arena returns to baseline. The full seeded /
  randomized reclaim stress belongs to `quality.stress_harness` (existing
  task) — scoped there, not duplicated here.
- **Coverage**: ≥90% diff coverage on changed lines; gate green including
  asan and the TSan lane.
- **Deferred follow-up** (closer registers in WBS): thread-local free pools
  + cross-thread release — `pool.free_pools` (est. 2d) — see Decisions;
  wired into `m2_editing`'s `depends`.

## Decisions

- **Inside-out reclaim-link stack over a global node-pool queue.** The
  enqueue node is a `SlotIndex` link stored in an anonymous parallel table
  published at `create` (exactly like the generation table), so the RT
  enqueue path is a single CAS with **zero allocation** and touches no data
  page. *Rejected:* a single global lock-free MPSC queue with heap nodes —
  it needs an RT-safe node allocator on the enqueue path (a wait-free node
  pool), which is strictly more machinery for no ordering benefit, since
  reclamation is order-independent.
- **LIFO Treiber-stack push + single-consumer `exchange` detach**, rather
  than a FIFO MPSC queue. Reclamation does not depend on order, and
  single-consumer detach (`exchange(head, nil)`) sidesteps the Treiber-stack
  pop ABA hazard entirely — only producers CAS, and a slot can appear on the
  stack at most once between hitting zero and being reclaimed. *Rejected:*
  Vyukov MPSC/FIFO — a stub node and more state for ordering nothing needs.
- **Type erasure per store, not per slot.** The queue keeps a
  `{void*, bool(*)(void*)}` drain thunk per registered store; type is
  recovered from the (already type-segregated) store. *Rejected:* a per-slot
  type tag or a `std::function` per entry — a write on the hot enqueue path
  and heap/`std::function` cost for information the store already carries.
- **Drain is a callable mechanism; scheduling lives in
  `runtime.housekeeping`.** This task ships `ReclamationQueue::drain()` /
  drain-to-quiescence and nothing that decides *when* it runs — because
  `runtime.housekeeping` (`tasks/65-runtime.tji:36-40`) explicitly owns
  "reclamation-queue draining between transactions / low-priority thread;
  checkpoint cadence policy." Keeping policy out here avoids two owners of
  the schedule and keeps this task at its 2d size.
- **Durability-epoch fence left as a clean seam, not built.** Doc 15:183-194
  puts the "reusable after checkpoint N" quarantine between reclaim and
  free-list, and `pool.checkpoints` (depends `pool.reclamation`) owns it.
  This task routes reclaim straight to `SlotStore::release`; checkpoints
  interposes the quarantine buffer there. *Rejected:* pre-building the epoch
  fence now — it is dead code without the A/B root protocol checkpoints
  provides, and would be an untestable stub (doc 16 forbids untested
  scaffolding).
- **Thread-local free pools deferred to `pool.free_pools` (est. 2d).**
  Doc 15 lists thread-local free pools with spill-to-global as the structure
  that makes the low-priority-thread drain effective — but they only pay off
  once multiple worker threads *allocate* concurrently, which does not exist
  until post-M1 worker parallelism, and arena_core's `SlotStore::release` is
  writer-only today (a debug assert forbids cross-thread release). Building
  them now yields dead code with no test that exercises concurrent
  allocation. `pool.free_pools` implements thread-local LIFO free lists in
  `SlotStore` with a global spill and relaxes the writer-only release assert
  to admit cross-thread release into thread-local pools, unlocking
  `runtime.housekeeping`'s low-priority-thread drain mode. It is
  agent-implementable (a concrete `SlotStore` change + a TSan test) and
  belongs to the M2 (`m2_editing`) neighborhood alongside
  `runtime.housekeeping`. *Rejected:* folding it into this task — it would
  blow the 2d estimate and ship untestable machinery.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-04.

- `src/pool/arbc/pool/reclamation.hpp` (new) — `DeferredReclaimSink<T>` (ZeroCountSink installed via `set_zero_sink`) + `ReclamationQueue` with per-store drain thunks for iterative cascade.
- `src/pool/arbc/pool/refs.hpp` — added reclaim-link seam: `enqueue_reclaim` (lock-free CAS push onto inside-out Treiber stack), `drain_pending` (single-consumer `exchange` detach + iterative reclaim), `link_ref`, `reclaim_chunks_published`, `k_reclaim_nil`, and reclaim-link chunk publication inside `ensure_parallel_chunks`.
- `src/pool/t/reclamation.t.cpp` (new) — 6 unit tests covering: deferred-enqueue (destructor not run until drain), round-trip to immediate sink, empty/double-drain no-ops, bounded-stack deep-chain cascade, behavioral-counter assertions (enqueue neither frees slot nor publishes chunk), `mprotect` data-page-cleanliness, and 8-producer concurrent enqueue-while-draining smoke.
- `src/pool/CMakeLists.txt` — wired `reclamation.hpp` header and `t/reclamation.t.cpp` test target.
- `tests/claims/registry.tsv` — registered `15-memory-model#release-enqueues-never-destroys-inline` and `15-memory-model#deferred-cascade-reclaims-whole-subtree` (both with `enforces:` in test file).
- Gap: TSan lane has no preset/CI lane in this repo today; full TSan coverage deferred (`.github/workflows/` edit is out of task scope — see parking lot).
