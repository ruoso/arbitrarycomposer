# pool.free_pools — Thread-local free pools

## TaskJuggler entry

`tasks/05-pool.tji` → `pool.free_pools` ("Thread-local free pools").

## Effort estimate

2d.

## Inherited dependencies

- `pool.reclamation` — **settled** (commit `c5b828e`). This task is the
  deferred follow-up `pool.reclamation` registered
  (`tasks/refinements/pool/reclamation.md:236-238,276-290`). Reclamation
  shipped the drain *mechanism* but bound it to the writer thread because
  `reclaim` bottoms out in `SlotStore::release`, which arena_core made
  writer-only. The seams this task plugs into:
  - `ReclamationQueue::drain()` and `RefStore<T>::drain_pending()` are the
    single-consumer detach-and-reclaim loop
    (`src/pool/arbc/pool/reclamation.hpp:78-88`,
    `src/pool/arbc/pool/refs.hpp:428-439`). Both carry a
    **"writer-thread-only"** doc-comment this task relaxes to
    "single-drainer, any one thread."
  - `RefStore<T>::reclaim` runs `~T` then bottoms out in
    `SlotStore::release` via `TypedStore<T>::destroy`
    (`src/pool/arbc/pool/refs.hpp:391-398`). This is the cross-thread path
    that the writer-only release assert currently forbids.
  - `RefStore<T>::enqueue_reclaim` is already lock-free / any-thread
    (`src/pool/arbc/pool/refs.hpp:411-418`) — RT threads already enqueue;
    only the *drain-side* release is gated.
- `pool.arena_core` — **settled** (commit `32c0d5d`). Owns the structure
  this task edits:
  - `SlotStore::d_free` — the single LIFO free-list vector kept outside the
    data pages (`src/pool/arbc/pool/slot_store.hpp:242`), the `allocate`
    pop path (`src/pool/slot_store.cpp:111-148`), the `release` /
    `free_now` push paths (`src/pool/slot_store.cpp:150-166`).
  - `SlotStore::assert_writer_thread()` — the debug guard binding the first
    calling thread (`src/pool/slot_store.cpp:99-109`; the load-bearing
    assert message `"SlotStore allocate/release is writer-thread-only"` at
    `:106`), invoked from `allocate`/`release`/`free_now`/`reserve_restored`/
    `finalize_restore`. Members `d_writer` / `d_writer_bound` at
    `src/pool/arbc/pool/slot_store.hpp:249-252`.
  - The class-comment threading contract at
    `src/pool/arbc/pool/slot_store.hpp:106-114` — already forward-references
    "thread-local free pools and cross-thread release" as future work (it
    names `pool.reclamation`; that pointer is now stale and this task
    corrects it to `pool.free_pools`).
- `pool.refcounts_in_store` — **settled** (commit `e5d164c`). Relocated the
  refcount/generation columns into `SlotStore` so several typed views over
  one size class share one free list; this task's thread-local restructuring
  of that one shared free list therefore serves every view over the class.
- `pool.checkpoints` — **settled** (commit `b1036d9`). Installed the
  `ReleaseFence` interposition between `release` and the free list
  (`src/pool/arbc/pool/slot_store.hpp:52-60,138,143`,
  `src/pool/slot_store.cpp:150-166`). This task must keep that seam intact
  and make the fence-pointer read race-free (see Constraints).

## What this task is

Restructure `SlotStore`'s single writer-only LIFO free list into
**per-thread free pools with spill to a shared global pool**, and relax the
writer-only guard so a slot may be **released from a thread other than the
writer** — concretely, the low-priority housekeeping thread running the
reclamation drain (design doc 15, `15-memory-model.md:45-46,129-143`).
Release becomes push-to-the-calling-thread's-local-pool; the writer's
allocation fast path pops from its own local pool and refills a batch from
the global pool only when its local pool runs dry; a local pool that grows
past a high-water threshold spills a batch to the global pool. Allocation
stays writer-only (the writer is the only *structural* allocator — arena
growth must remain single-threaded), so the guard relaxation is asymmetric:
`release`/`free_now` admit any thread; `allocate`/`reserve_restored`/
`finalize_restore` keep the writer assert. The drain stays **single-drainer**
(exactly one thread drains at a time — writer *between* transactions **or**
the low-priority thread, never both concurrently); this task relaxes *which*
thread may be that drainer, not the single-consumer invariant the
reclaim-link detach relies on.

## Why it needs to be done

- It is the structure design doc 15 names as the reason the reclamation
  model performs: "release is push-to-local-queue, reuse is thread-affine"
  (`15-memory-model.md:45-46`) and "the writer thread is the only
  structural allocator — which is what makes thread-local free pools
  effective: churn is concentrated where the free pool lives"
  (`15-memory-model.md:141-143`).
- It unlocks the drain mode `pool.reclamation` could not ship. Doc 15's
  housekeeping pass runs on the "writer thread between transactions, **or a
  low-priority thread**" (`15-memory-model.md:131-133`). The low-priority
  variant requires cross-thread `SlotStore::release` into a free pool that
  does not contend with the writer's concurrent allocation — exactly what
  thread-local pools provide.
- Direct downstream WBS consumer: `runtime.housekeeping`
  (`tasks/65-runtime.tji:36-41`, note "Reclamation-queue draining between
  transactions / **low-priority thread**") owns the drain *policy*; it can
  only choose the low-priority-thread schedule once the mechanism admits
  cross-thread release. This task ships the mechanism, not the schedule.
- `pool.free_pools` is a named dependency of milestone `m2_editing`
  (`tasks/99-milestones.tji:23`), alongside `runtime.housekeeping`.

## Inputs / context

- `docs/design/15-memory-model.md`:
  - **"Thread-local free pools"** bullet (`:45-46`): "Thread-local free
    pools with spill to a global pool: release is push-to-local-queue,
    reuse is thread-affine." The normative spec for this task's structure.
    (The doc says "push/pop queue"; **LIFO** is this refinement's tightening
    of that, matching arena_core's existing perfect-hole LIFO discipline —
    `15-memory-model.md:34-36`.)
  - **"Cascades are deferred"** bullet (`:129-136`): the drain runs on the
    "writer thread between transactions, or a low-priority thread"; "free
    slots return to the pools warm."
  - **"Thread rules"** bullet (`:137-143`): "Render and audio-engine threads
    may pin/unpin (one refcount op) and enqueue reclamation, nothing more…
    The writer thread is the only structural allocator." Scopes the
    single-allocator rule to *allocation* and explicitly permits cross-thread
    release-via-enqueue — this task's relaxation is consistent with the doc,
    not a departure.
  - **Anonymous-runtime-state** framing (`:187-190`): "refcounts, free
    pools, generation tags, and the reclamation queue are anonymous runtime
    state, rebuilt on open." The thread-local + global pools are never
    persisted; recovery (`finalize_restore`) rebuilds a single free set.
- `src/pool/arbc/pool/slot_store.hpp` / `src/pool/slot_store.cpp` — the
  `d_free` free list, `allocate`/`release`/`free_now`,
  `assert_writer_thread`, `free_slots()`/`slots_live()` accounting, the
  `ReleaseFence` seam, and the class-comment threading contract (line refs
  under Inherited dependencies).
- `src/pool/arbc/pool/refs.hpp:428-439` — `drain_pending`'s single-consumer
  `exchange` detach (the invariant this task must preserve while widening
  the permitted drainer thread); `:391-398` `reclaim`; `:411-418`
  `enqueue_reclaim` (already any-thread).
- `src/pool/arbc/pool/reclamation.hpp:57-58,78-88` — `ReclamationQueue::drain`
  and its "writer-thread-only" comment.
- `docs/design/16-sdlc-and-quality.md`:
  - Tier 6 **"Concurrency tests"** (`:66-73`): "TSan on the full suite;
    dedicated stress tests for the publish/pin protocol and the reclamation
    queue with schedule perturbation… debug allocator hooks asserting no
    allocation/refcount/lock on RT threads."
  - Behavioral-counter discipline (`:54-62`, esp. `:57` "slots
    allocated/reclaimed" counters) — perf-shaped promises assert on counters,
    never wall-clock.
  - CI wiring (`:101-104`): TSan job per-push; long-form stress nightly.
- `docs/design/17-internal-components.md`: `arbc::pool` is Level 1, depends
  only on `base` (`:49`); the levelization rule (`:41`); the low-priority
  drain *consumer* lives in `arbc::runtime` (Level 5, `:60`) and calls down
  into pool. This task's edit stays entirely in `arbc::pool`.

## Constraints / requirements

- **Thread-local free pool + global spill.** Each thread that touches a
  `SlotStore` gets a per-(thread, store) LIFO free pool. `release`/`free_now`
  push the freed slot onto the calling thread's local pool with **no lock and
  no allocation** on the hot path. When a local pool exceeds a high-water
  threshold it spills a batch to the store's shared global pool; when the
  writer's local pool is empty on `allocate` it refills a batch from the
  global pool before falling back to high-water growth. The global pool is
  the existing `d_free` storage, now guarded by a lock touched **only** on
  the spill/refill (cold) boundary. Batch size is a small fixed constant
  (documented in the header); it need not be tuned in this task.
- **Reuse is thread-affine.** A thread's most-recently-released slot is
  reused by that same thread's next allocation from its local pool before any
  global-pool round-trip (`15-memory-model.md:46`). In practice only the
  writer allocates, so this is the writer reusing its own just-freed slots
  without touching the global lock.
- **Asymmetric guard relaxation.** Remove the writer assert from `release`
  and `free_now` (cross-thread release is now legal). **Keep** it on
  `allocate`, `reserve_restored`, and `finalize_restore`: the writer remains
  the only structural allocator (arena growth — `d_directory.publish`,
  `publish_parallel_columns`, capacity accounting — stays single-threaded and
  unsynchronized). Update the class-comment threading contract
  (`slot_store.hpp:106-114`) to state the new rule and re-point the stale
  "arrive with pool.reclamation" reference to `pool.free_pools`.
- **Single-drainer invariant preserved.** `drain_pending`'s `exchange`
  detach assumes one consumer. This task does not make concurrent multi-thread
  drain legal; it makes the single drainer be *any one thread* (writer between
  transactions **or** the low-priority thread). Choosing/serializing the
  drainer is `runtime.housekeeping`'s policy. Update the "writer-thread-only"
  comments on `drain`/`drain_pending`/`reclaim` to "single-drainer, any one
  thread"; no behavioral change to the detach itself.
- **Concurrency-safe accounting.** `d_slots_live` is mutated by both the
  writer's `allocate` and a cross-thread `release`; promote it to
  `std::atomic<std::size_t>` (relaxed inc/dec). `free_slots()` becomes
  best-effort (global-pool size under the lock; local-pool contents are not
  globally visible) and its doc-comment must say so — it is a diagnostic
  counter, not an invariant source.
- **ReleaseFence seam intact and race-free.** `release` still consults the
  durability fence before the free pool. Because a cross-thread `release`
  now reads `d_release_fence` concurrently with the writer's writer-only
  `set_release_fence`, promote the pointer to `std::atomic<ReleaseFence*>`
  (relaxed load on the release path, release store on install) so the read is
  race-free under TSan. `ReleaseFence::on_release` runs on the releasing
  thread; the fence's later `free_now` return pushes onto that caller's local
  pool — unchanged mechanics. Checkpoints' crash-recovery sweep must stay
  green.
- **No new persistence surface.** Thread-local and global pools are anonymous
  runtime state (`15-memory-model.md:187-190`). `reserve_restored` /
  `finalize_restore` rebuild a single free set on the writer thread at open,
  before any concurrent drainer exists; they keep the writer assert and need
  no thread-local machinery.
- **Levelization.** Everything stays in `arbc::pool` (header
  `slot_store.hpp` + `slot_store.cpp`), depending only on `base` (doc 17
  `:49`). No new component edge; the low-priority-thread *consumer* is
  `arbc::runtime`'s (doc 17 `:60`), out of this task's scope.

## Acceptance criteria

- **Unit tests** (`src/pool/t/free_pools.t.cpp`, wired into
  `src/pool/CMakeLists.txt`'s `arbc_component_test`):
  - Cross-thread release no longer aborts: releasing a slot from a thread
    other than the writer succeeds (the previous debug assert would have
    fired). `allocate` from a non-writer thread still trips the writer assert
    in a debug build (`ASSERT_DEATH`/guarded) — the asymmetry is pinned.
  - Round-trip recycle: writer allocates N slots, a second thread releases
    them all, they spill to the global pool, and the writer's subsequent
    `allocate` reuses them (live count and capacity return to baseline; no
    high-water growth beyond N).
  - Thread-affine reuse: the writer releasing then immediately allocating on
    the same thread gets its own slot back from its local pool without a
    global-pool round-trip.
  - `ReleaseFence` still interposes: with a fence installed, release diverts
    to the fence and only `free_now` returns the slot to a pool (the
    checkpoints behavior is unchanged).
- **Claim (register + `enforces:`)**
  `15-memory-model#thread-local-free-pools-spill-to-global` — a non-writer
  release pushes to a thread-local pool (no arena growth, no global lock on
  the push) and the slot returns to the writer's allocator only via the
  global spill; a concurrent "writer allocates while a second thread
  releases" run recycles every slot with **no loss and no duplication** and
  the arena returns to baseline. Registered in `tests/claims/registry.tsv`.
- **Claim (register + `enforces:`)**
  `15-memory-model#reuse-is-thread-affine` — a thread's just-released slot is
  the one its next same-thread allocation returns, proven before any global
  round-trip occurs.
- **Behavioral-counter assertions** (doc 16 `:54-62`): a per-store spill/
  refill counter (or lock-acquisition counter) stays at **zero** across a
  sub-batch churn burst (release-then-reallocate fewer than one batch of
  slots) — proving the hot path takes no global lock — and advances only when
  the local pool crosses the spill/refill threshold. No wall-clock assertion.
- **Concurrency (TSan + asan, explicit per doc 16 `:66-73`)**: a smoke test
  — the writer allocates in a loop while one low-priority thread drains a
  `ReclamationQueue` (releasing slots cross-thread) concurrently — runs clean
  under TSan and asan; every destructor fires exactly once, every slot is
  accounted, and the arena returns to baseline. The full seeded/randomized
  reclamation-queue schedule-perturbation stress belongs to
  `quality.stress_harness` (existing task) — scoped there, not duplicated
  here.
- **Regression**: the existing `reclamation.t.cpp` multi-producer smoke and
  the crash-recovery sweep (`pool.crash_tests`) stay green; the drain now
  optionally running off-writer must not change their outcomes.
- **Coverage**: ≥90% diff coverage on changed lines; gate green including the
  asan lane (`scripts/gate`, `scripts/check_claims.py`).
- **No deferred WBS follow-up**: this restructuring is self-contained
  (mirrors `pool.refcounts_in_store`'s "self-contained refactor"). The repo
  still has no per-push TSan CI *lane* (noted by `pool.reclamation` and
  `pool.refcounts_in_store`); wiring one is a `.github/workflows/` /
  infrastructure edit, not agent-implementable feature work — surfaced for
  the parking lot, not registered as a task.

## Decisions

- **Per-thread LIFO pool + lock-guarded global spill, not a single lock-free
  free list.** Thread-local pools give the doc-mandated lock-free/allocation-
  free hot path (`15-memory-model.md:45,141-143`) and let the writer's
  allocation and the drainer's release proceed without touching shared state
  except at the batch boundary. *Rejected:* a single mutex-guarded `d_free`
  for every op — correct but takes the lock on every release/allocate,
  contradicting the "churn concentrated where the free pool lives" premise
  and the RT-adjacent hot-path discipline. *Rejected:* a lock-free Treiber
  global stack with no thread-local cache — more machinery than the two-
  thread (writer + one drainer) reality needs today, and it still bounces a
  cache line per op; the thread-local cache is where the win is. A lock-free
  global spill is a later, orthogonal optimization if profiling ever demands
  it (not scoped — no dead code, doc 16).
- **Asymmetric relaxation: release cross-thread, allocate writer-only.**
  Doc 15 scopes the single-allocator rule to *allocation*
  (`15-memory-model.md:141-142`) and explicitly permits cross-thread release
  (`:139-141`). Arena growth (chunk publish, column publish, capacity
  accounting) is unsynchronized and stays writer-only; opening `allocate` to
  workers would require a thread-safe growth path — genuinely post-M1
  worker-parallelism work with no consumer today. *Rejected:* relaxing
  `allocate` too — untestable dead concurrency (no second allocator exists)
  and a much larger change than 2d.
- **Single-drainer preserved; relax the thread, not the count.**
  `drain_pending`'s `exchange` detach is single-consumer
  (`refs.hpp:428-439`); making concurrent multi-drain legal would reintroduce
  the ABA hazard `pool.reclamation` deliberately sidestepped. The unlock
  doc 15 asks for is "writer *or* low-priority thread" (`:131-133`) — an
  either/or, one drainer at a time. `runtime.housekeeping` serializes the
  choice. *Rejected:* multi-consumer drain — solves a problem the design
  never poses and breaks the detach invariant.
- **`d_slots_live` and `d_release_fence` become atomic.** Cross-thread
  release now races the writer on both. `std::atomic<std::size_t>` (relaxed)
  for the live counter and `std::atomic<ReleaseFence*>` for the fence pointer
  are the minimal race-free changes; `free_slots()` degrades to a best-effort
  diagnostic (global-pool size only). *Rejected:* a mutex around the counter
  — unnecessary contention for a monotonic-ish counter an atomic handles.
- **No design-doc delta.** Doc 15 already designs thread-local free pools
  with global spill (`:45-46`), the low-priority drain mode (`:131-133`), and
  the thread rules that permit cross-thread release (`:137-143`). The
  asymmetric guard and single-drainer clarifications are refinements of the
  mechanism, fully consistent with the text. The only edit outside tests is
  the `SlotStore` code and its stale class-comment forward-reference, which
  rides the closer's implementation commit (doc 16 same-commit rule) — not a
  design-doc change.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-04.

- `src/pool/arbc/pool/slot_store.hpp`, `src/pool/slot_store.cpp` — thread-local LIFO free pools per (thread, store) with lock-guarded global spill/refill; `d_slots_live` and `d_release_fence` promoted to atomics; asymmetric guard (release/free_now any-thread, allocate/reserve_restored/finalize_restore writer-only); `free_slots()` degraded to best-effort global-only diagnostic; spill/refill counters; `k_free_pool_batch` batch constant.
- `src/pool/arbc/pool/reclamation.hpp`, `src/pool/arbc/pool/refs.hpp` — "writer-thread-only" comments on drain/drain_pending/reclaim relaxed to "single-drainer, any one thread"; single-consumer detach invariant preserved.
- `src/pool/t/free_pools.t.cpp` (new) — cross-thread release no longer aborts, writer-allocate-death fork test, round-trip recycle, thread-affine reuse, sub-batch counter at-zero, fence interposition, concurrent writer-allocate/low-priority-drain smoke test.
- `src/pool/CMakeLists.txt` — `free_pools.t.cpp` wired into `arbc_component_test`.
- `src/pool/t/checkpoint.t.cpp` — two `free_slots()==1` assertions updated to `==0` (freed slot sits in writer's local pool; subsequent reuse assertions still prove reusability).
- `tests/claims/registry.tsv` — claims `15-memory-model#thread-local-free-pools-spill-to-global` and `15-memory-model#reuse-is-thread-affine` registered.
