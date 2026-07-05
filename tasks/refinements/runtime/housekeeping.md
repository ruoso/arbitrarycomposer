# runtime.housekeeping — Housekeeping (reclamation drain + checkpoint cadence + GC observability)

## TaskJuggler entry

`tasks/65-runtime.tji:36-41` → `runtime.housekeeping` ("Housekeeping"), the sixth
leaf under `task runtime`. It carries `depends pool.reclamation`
(`65-runtime.tji:39`); the parent `task runtime` adds
`depends compositor.tile_planning` (`65-runtime.tji:6`). No sibling `depends
!housekeeping` today. Note line:

> "Reclamation-queue draining between transactions / low-priority thread;
> checkpoint cadence policy; version GC observability. Docs 14/15."

It is a dependency of milestone `m2_editing` ("M2: Versioned editing",
`tasks/99-milestones.tji:21-25`).

## Effort estimate

**1d.** The three mechanisms this task drives are already built and tested at
L1 (`arbc::pool`): `ReclamationQueue::drain()`, `Checkpointer::commit()` with its
`is_dirty` clean-skip and its behavioral counters, and the per-arena live-count
accessors. Each of those seams explicitly names `runtime.housekeeping` as the
owner of the *policy* — "deciding WHEN to run is runtime.housekeeping's job"
(`reclamation.hpp:47-48`), "deciding WHEN to checkpoint is runtime.housekeeping's
cadence policy (doc 17: L5), not this layer's (L1)" (`checkpoint.hpp:32-33`). So
this task builds **no new mechanism**: it is a small, passive `arbc::runtime`
policy object (`Housekeeper`) that decides *when* to call the existing drain and
commit entry points, and surfaces a wall-clock-free observability snapshot
aggregating the counters the mechanisms already expose. The deliverable is one
header/impl pair, unit tests against real pool fixtures, three claims, and the
CMake `DEPENDS pool` edge.

## Inherited dependencies

**Settled:**

- `pool.reclamation` (commit `c5b828e`, DONE 2026-07-04) — the direct predecessor
  (`depends pool.reclamation`). It shipped the deferred-reclamation machinery this
  task schedules, in `src/pool/arbc/pool/reclamation.hpp`:
  - **`template <class T> class DeferredReclaimSink`** (`:34-45`) — a
    `ZeroCountSink` whose `on_zero(SlotIndex)` (`:41`) does a lock-free,
    allocation-free `store.enqueue_reclaim(index)`; **no `~T` runs on release**.
    RT threads (render, audio-engine) may enqueue; nothing more (doc 15:137-143).
  - **`class ReclamationQueue`** (`:63-107`) — the registry of stores a drain
    sweeps. **`void drain()`** (`:82-92`) loops every registered store's
    `drain_pending()` thunk until a full pass reclaims nothing (global quiescence),
    unrolling the destructor cascade **iteratively** (C++ stack O(1) in subtree
    depth); empty- and double-drain are no-ops. **`register_store`** (`:71-74`)
    installs the sink writer-only.
  - The class comment (`:47-62`) is emphatic about the seam this task fills:
    "deciding WHEN to run is runtime.housekeeping's job"; drain is
    "**SINGLE-DRAINER, ANY ONE THREAD** … the drainer may be the writer between
    transactions **OR** the low-priority housekeeping thread — exactly one at a
    time (**runtime.housekeeping serializes the choice**)."
  - `reclamation.md` explicitly deferred **all scheduling/cadence** here: "This
    task ships `ReclamationQueue::drain()` / drain-to-quiescence and nothing that
    decides *when* it runs" (`reclamation.md` Decisions). It also reserved the
    teardown drain-to-quiescence callable (`drain()` run before arena drop for
    externally-resourced types, doc 15:144-147) for this task to drive on a
    schedule.
- `pool.checkpoints` (DONE) — the checkpoint mechanism, inherited transitively.
  `src/pool/arbc/pool/checkpoint.hpp`:
  - **`class Checkpointer`** (`:87-301`) — the ordered A/B-root commit protocol.
    **`expected<std::monostate, WorkspaceFileError> commit(SlotIndex root_index)`**
    (`:133-171`): msync data chunks → publish+flip the inactive header root slot →
    msync header → drain the durability fences. **An unmutated scene skips the
    data msync** (`is_dirty()` false, `:270-273`, private) but still flips+syncs
    the header; errors surface as values.
  - **Behavioral counters** (`:209-216`, "doc 16, never wall-clock"):
    `commit_count()`, `data_msyncs()`, `header_msyncs()`, `slots_freed_to_list()`,
    `epoch()`, `durable_epoch()`, `generation()`. Plus
    **`DurabilityEpochFence::quarantined_total()`** (`:72`).
  - The header comment (`:32-33`) hands cadence here: "deciding WHEN to checkpoint
    is runtime.housekeeping's cadence policy (doc 17: L5), not this layer's (L1)."
    `checkpoints.md` (`:109-112, 373-377`) confirms it deferred "checkpoint cadence
    policy (timer / transaction count / explicit host call)" to this task.
- `pool.free_pools` (DONE) — inherited transitively; relaxed drain to
  **single-drainer, any one thread** by admitting cross-thread release into a
  thread-local free pool (claims `15-memory-model#thread-local-free-pools-spill-to-global`,
  `#reuse-is-thread-affine`, `registry.tsv:48-49`). This is what "**unlocks
  runtime.housekeeping's low-priority-thread drain mode**" (`free_pools.md`); this
  task is free to choose the writer-between-transactions or the low-priority-thread
  drain site, and the mechanism is safe under either provided exactly one thread
  drains at a time.

**Pending:** none — every predecessor is landed.

## What this task is

Deliver the **housekeeping policy** for `arbc::runtime` (L5): the small passive
object that decides *when* to run the pool's already-built reclamation drain and
checkpoint commit, and that exposes a host-facing "memory panel" observability
snapshot. In a new header/impl `src/runtime/arbc/runtime/housekeeping.hpp`
(+ `housekeeping.cpp`):

1. **`struct HousekeepingConfig`** — the cadence knobs, all defaulted:
   `bool drain_between_transactions = true`;
   `std::optional<std::uint64_t> checkpoint_every_n_transactions`;
   `std::optional<std::uint64_t> checkpoint_tick_interval`. An absent optional
   disables that checkpoint trigger; an anonymous (no-checkpointer) arena ignores
   both. These realize doc 15:213-214's three triggers — **transaction count**,
   **timer**, **explicit host call** — as declarative policy.
2. **`class Housekeeper`** — constructed with a `ReclamationQueue&` (always), a
   `Checkpointer*` (nullable — null for anonymous arenas, doc 15:160-162), an
   `Arena*` (nullable — the live-count source for observability,
   `slot_store.hpp:354`), and a `HousekeepingConfig`. Thread-agnostic entry
   points, **serialized by the caller so exactly one drains at a time**
   (`reclamation.hpp:57-62`):
   - **`void after_commit(SlotIndex root)`** — the *between-transactions* drain
     site (doc 15:129-136). The writer calls it after each published transaction:
     it drains the reclamation queue to quiescence (when
     `drain_between_transactions`), records `root` as the checkpointable tip,
     bumps the transaction counters, and — if the transaction-count trigger has
     reached its threshold — commits a checkpoint at `root` and resets the
     since-checkpoint counter.
   - **`expected<std::monostate, WorkspaceFileError> tick(std::uint64_t monotonic_tick)`**
     — the timer / low-priority-thread entry. It drains the queue, then, if the
     tick-interval trigger has elapsed **and at least one transaction has occurred
     since the last checkpoint**, commits at the recorded tip. The monotonic tick
     is a **handed value**, never a wall-clock read (doc 16; mirrors the
     compositor's handed `Deadline`).
   - **`expected<std::monostate, WorkspaceFileError> request_checkpoint()`** — the
     explicit host-call trigger; commits at the recorded tip unconditionally
     (`Checkpointer::commit` still skips the data msync on a clean scene).
   - **`void drain_and_quiesce()`** — the teardown / bulk-release path
     (doc 15:144-147); drains to quiescence so externally-resourced types run
     their destructors before arena drop.
   - **`HousekeepingStats stats() const`** — the observability snapshot (below).
3. **`struct HousekeepingStats`** — the wall-clock-free "memory panel" surface
   (doc 15:149-151, doc 14:199-201): `std::uint64_t transactions_seen`,
   `drains_run`, `checkpoints_committed`, `checkpoints_skipped_clean`;
   `std::size_t live_slots` (from `Arena::total_slots_live()`, or 0 when no arena
   is bound); pass-through `std::uint64_t slots_freed_to_list` and
   `std::uint32_t durable_epoch` from the `Checkpointer`. This aggregates the
   existing per-arena / per-checkpointer counters into one runtime-facing view a
   host memory panel reads.

**Not this task:**

- **The reclamation/checkpoint *mechanisms* themselves** — `ReclamationQueue`,
  `DeferredReclaimSink`, `Checkpointer`, `DurabilityEpochFence`, the ordered
  A/B-root commit and its counters — are `pool.reclamation` / `pool.checkpoints`
  (DONE). This task only *drives* them; it adds no L1 code.
- **The owned background low-priority housekeeping thread** — a `std::thread` that
  parks/wakes and calls `tick()` on an interval, serializing as the single drainer
  against writer drains — is **deferred to `runtime.housekeeping_thread`** (closer
  registers in WBS; see Acceptance criteria). The `Housekeeper` here is passive
  and thread-agnostic; v1's caller drives its hooks synchronously on the writer
  thread. Building/owning the thread is genuinely concurrency-touching (needs
  TSan/stress coverage) and additive over this policy object.
- **Wiring the `Housekeeper` into the live `Document`/`Model` arenas.** Today
  `runtime::Document` holds a `Model d_model` and an `std::unordered_map` of
  content, not slab-arena-backed content records with installed
  `DeferredReclaimSink`s and a `Checkpointer` (`document.hpp:29-32`: content
  "migrates into versioned content records when the Editable facet and the slab
  arenas land (docs 14/15)"). This task validates the policy against **real pool
  fixtures** (an `Arena` + `RefStore` + `ReclamationQueue`, and a workspace-backed
  `Checkpointer`); the Document→arena rewire rides that later migration and is not
  in this task's scope (surfaced for the parking lot — see Open questions).
- **The journal byte-budget trim and cache-budget eviction** — the *other* two
  "version GC" knobs (doc 15:124-128). Journal trim is `model.journal`'s
  (`14-data-model-and-editing#journal-trims-to-byte-budget`, `registry.tsv:15`);
  cache budgets are `cache.*`'s (`15-memory-model#cache-budget-is-eviction-policy`,
  `registry.tsv:55`). This task observes and drives *reclamation and checkpoint*
  cadence; it does not re-implement those budgets.
- **`.github/workflows/` TSan lane.** The follow-up thread task's sanitizer
  coverage wants a TSan CI lane the repo still lacks (`parking-lot.md:33-41`); this
  task adds no CI infra and has no concurrency surface of its own.

## Why it needs to be done

`pool.reclamation` and `pool.checkpoints` deliberately shipped mechanism without
policy: `drain()` reclaims to quiescence but nothing decides when to call it, and
`commit()` writes a durable checkpoint but nothing decides when. Until this task
lands, a long editing session's released slots accumulate on the reclamation
queue with no scheduled drain (bounded only by the eventual teardown drain), and
the workspace file is never checkpointed on a cadence, so "an editor crash costs
at-most-since-last-checkpoint" (doc 15:164-167) has no *since-last-checkpoint*
boundary to lean on. Doc 15's version-reclamation model ("refcounts as the GC,
budgets as the policy", `:112-136`) is only half-real without the policy layer
that turns the exposed knobs into a running discipline. This task is that layer,
and it is a dependency of `m2_editing` (versioned editing realized,
`99-milestones.tji:21-25`): editing that publishes versions needs the
housekeeping that keeps their reclamation bounded and their checkpoints durable.

## Inputs / context

- `docs/design/15-memory-model.md`:
  - `:112-136` — **"Version reclamation: refcounts as the GC, budgets as the
    policy."** `:118-123` a version is memory-live exactly while reachable (pinned
    `DocState` root or journal-referenced); unpin/trim cascades the unique nodes —
    "reclamation is exact and incremental by construction." `:124-128` the tunables
    are the journal byte budget and the cache budgets, "now with **per-arena
    accounting** to drive them." **`:129-136`** the load-bearing paragraph:
    "Cascades are deferred, never inline … Release enqueues the object on a
    type-erased **reclamation queue**; a housekeeping pass — **writer thread
    between transactions, or a low-priority thread** — pops, runs destructors …
    continues the cascade *iteratively*." Both drain sites are sanctioned here.
  - `:137-143` — **Thread rules.** The audio callback touches no allocator/refcount
    ever; render and audio-engine threads "may pin/unpin … and **enqueue
    reclamation, nothing more**"; "the writer thread is the only structural
    allocator." Constrains who may drain vs. who may only enqueue.
  - `:144-147` — teardown is arena drop, "bulk-release path runs the reclamation
    queue to quiescence first for types with external resources" —
    `drain_and_quiesce()`'s reason to exist.
  - `:149-154` — **Debug discipline** (contract-test-enforced): "**per-arena live
    counts and byte accounting exposed through the API** (hosts *will* want a
    memory panel)"; generation tags; "leak check = arena live count at teardown."
    The observability this task surfaces at the runtime level.
  - `:205-216` — **"Checkpointing rides the version model."** `:205-209` the
    ordered A/B-root commit. **`:213-214`** "**Checkpoint cadence is policy (timer,
    transaction count, explicit host call)**" — the three triggers this task
    implements. `:209-213` the durability-epoch fence couples freed-slot reuse to
    checkpoint N (owned by `pool.checkpoints`; this task just triggers the commit
    that advances the durable epoch).
  - `:164-167` — crash recovery: the workspace file "always contains the records
    of every checkpointed version … crash costs at-most-since-last-checkpoint" —
    the durability boundary the checkpoint cadence establishes.
- `docs/design/14-data-model-and-editing.md`:
  - `:199-205` — **History / version GC.** "Budgeted by bytes via `state_cost` …
    **version GC releases unpinned `DocState` nodes and unreferenced state handles
    by refcount**"; "A pinned old version … keeps only what it references alive."
    The refcount-exact GC this task's drain realizes (structural sharing bounds
    the cost). Claim `14-data-model-and-editing#dropping-pin-reclaims-only-unique-nodes`
    (`registry.tsv:6`) is `model`'s; this task depends on it, does not re-register.
  - `:111-115` — aborted transactions' working records "are reclaimed" (claim
    `#abort-publishes-nothing`, `registry.tsv:8`) — feeds the same queue this task
    drains.
- `docs/design/17-internal-components.md`:
  - `:24-26` — `arbc::runtime` (L5) contents include "**housekeeping (reclamation,
    checkpoints)**."
  - `:49` — `arbc::pool` (L1) owns "deferred reclamation … checkpoint protocol" —
    the mechanism side of the split.
  - `:60` — `arbc::runtime` is **Level 5**, "Depends on: **everything below**" — so
    adding a `pool` edge to `runtime`'s CMake `DEPENDS` is pre-authorized (pool is
    L1). **No levelization delta.**
  - `:41-44` — depend only on strictly lower levels; the CI dependency check
    validates CMake + include graph against the table.
- `docs/design/16-sdlc-and-quality.md`:
  - `:14-25` — the claims register (`<doc-file-stem>#<slug>`, enforced by a
    `// enforces: <claim-id>` test comment; CI fails a registered claim with no
    live test).
  - `:54-62` — **behavioral-counter tests, never wall-clock.** The cadence triggers
    are proven by driving deterministic transaction/tick counts and asserting on
    the `Checkpointer`/`Housekeeper` counters — never by sleeping on a real clock.
  - `:112-118` — ≥90% diff coverage on changed lines.
- `src/pool/arbc/pool/reclamation.hpp:34-45,:63-107,:82-92,:71-74,:47-62` —
  `DeferredReclaimSink`, `ReclamationQueue`, `drain`, `register_store`, and the
  class comment naming this task the cadence owner.
- `src/pool/arbc/pool/checkpoint.hpp:32-33,:87-171,:124,:133-171,:209-216,:270-273,:58-85`
  — the cadence-is-L5 comment, `Checkpointer`, `slot_fence`, `commit` (+ its
  clean-scene msync skip), the behavioral counters, private `is_dirty`, and
  `DurabilityEpochFence`/`quarantined_total`.
- `src/pool/arbc/pool/slot_store.hpp:354-355,:359,:238` — `Arena::total_slots_live()`
  / `total_high_water()`, `for_each_store`, the per-store accounting comment
  ("hosts want a memory panel") — the live-count source for `HousekeepingStats`.
- `src/model/arbc/model/model.hpp:164-166` — `Model::live_slots()`, "the per-arena
  live count exposed for a host memory panel, and the behavioral witness" — the
  precedent for the observability accessor shape.
- `src/runtime/arbc/runtime/document.hpp:16-33` — the current `Document` (`:29`
  `Model d_model`; `:30-32` content not yet arena-backed) — why this task tests
  against pool fixtures, not `Document`.
- `src/runtime/CMakeLists.txt:1-5` — `DEPENDS base model contract compositor`; this
  task adds `pool`, the header/impl to `SOURCES`/`PUBLIC_HEADERS`, and the
  component test.
- `tasks/refinements/pool/reclamation.md`, `pool/checkpoints.md`,
  `pool/free_pools.md` — the predecessors' Decisions/Status establishing the
  mechanism-vs-policy split and the single-drainer-any-thread relaxation.
- `tests/claims/registry.tsv:40-45` — the reclamation/checkpoint claims this task
  depends on (`#release-enqueues-never-destroys-inline`,
  `#deferred-cascade-reclaims-whole-subtree`, `#checkpoint-recovers-consistent-root`,
  `#freed-slot-quarantined-until-durable`); this task appends three new rows.
- `tasks/parking-lot.md:33-41` — the standing TSan-CI-lane gap the follow-up thread
  task inherits.

## Constraints / requirements

- **Levelization (doc 17:41-44, :60).** `arbc::runtime` is L5 and depends on
  everything below; this task adds a `pool` `DEPENDS` edge (pre-authorized). It
  reaches the mechanisms only through the public pool headers (`reclamation.hpp`,
  `checkpoint.hpp`, `slot_store.hpp`). No same-level edge; the CI dependency check
  stays green.
- **Policy only — no new mechanism.** The `Housekeeper` calls `ReclamationQueue::
  drain()`, `Checkpointer::commit()`, and reads the existing counters. It adds no
  L1 primitive, no new sink, no new fence. Filling in a mechanism the pool layer
  did not ship is out of scope.
- **Both drain sites sanctioned; caller serializes the single drainer
  (doc 15:129-136, `reclamation.hpp:57-62`).** The `Housekeeper` never spawns a
  thread; it exposes thread-agnostic hooks. v1 drives them on the **writer thread**
  (`after_commit`, and `tick`/`request_checkpoint` called by the same owner). The
  contract is: **exactly one thread calls a draining entry point at a time.** RT
  threads only enqueue (they never call `Housekeeper`).
- **No wall-clock read (doc 16:54-62).** The timer trigger is driven by a
  **monotonic tick value handed to `tick()`** by the caller (the runtime frame
  loop owns the real clock, doc 17:60). The `Housekeeper` compares handed ticks;
  it calls no `now()`. Tests advance ticks deterministically.
- **Three checkpoint triggers, clean scenes skipped (doc 15:213-214).** Commit
  fires when (a) transactions-since-checkpoint reaches
  `checkpoint_every_n_transactions`, (b) `tick − last_checkpoint_tick` reaches
  `checkpoint_tick_interval` **and** at least one transaction occurred since the
  last checkpoint, or (c) `request_checkpoint()` is called. Triggers (a)/(b) skip
  when no transaction has occurred since the last checkpoint (counted
  `checkpoints_skipped_clean`), so a still scene does not thrash the header —
  belt-and-suspenders with `Checkpointer::commit`'s own data-msync skip
  (`checkpoint.hpp:130-132`). The task does **not** call the private
  `Checkpointer::is_dirty()`; the transaction-count proxy is its dirtiness signal.
- **Errors as values (doc 15, `checkpoint.hpp` contract).** `tick`,
  `request_checkpoint`, and `after_commit` return
  `expected<std::monostate, WorkspaceFileError>` (or propagate it), never abort. A
  checkpoint I/O failure surfaces to the caller; the file remains recoverable to
  its last durable root (`registry.tsv:45`).
- **Anonymous arenas: checkpoint triggers inert.** With `Checkpointer* == nullptr`
  the checkpoint triggers are no-ops (anonymous, live-only hosts, doc 15:160-162);
  reclamation cadence and live-count observability still apply.
- **Observability is wall-clock-free and aggregates existing counters
  (doc 15:149-151).** `HousekeepingStats` reports the arena live count
  (`Arena::total_slots_live()`) plus the `Housekeeper`'s own event counters and
  pass-through `Checkpointer` counters. No new counting mechanism; it reads what
  the mechanisms already publish.
- **Single-threaded, no concurrency surface — no TSan obligation.** The passive
  policy object issues single-threaded `drain`/`commit` calls; the pool primitives'
  own thread-safety is `pool.*`'s. The owned background thread (with its TSan
  obligation) is the deferred follow-up.
- **CI diff coverage ≥90%** (doc 16:112-118); the public header compiles
  standalone.

## Acceptance criteria

- **Unit tests — `src/runtime/t/housekeeping.t.cpp` (new, Catch2), registered via
  `arbc_component_test`.** Against real pool fixtures (an `Arena` + a `RefStore<T>`
  of a trivially-destructible test record with a `DeferredReclaimSink` registered
  on a `ReclamationQueue`; and, for the checkpoint cases, a workspace-file-backed
  `Checkpointer` in a temp file):
  - **Drain between transactions:** build and release a retained-`SlotRef` subtree
    so the queue is non-empty, call `after_commit(root)`, and assert the arena's
    `total_slots_live()` has returned to its no-garbage baseline (queue drained to
    quiescence) and `stats().drains_run` advanced by exactly one. With
    `drain_between_transactions = false`, `after_commit` leaves the queue
    un-drained (live count unchanged) until an explicit `drain_and_quiesce()`.
  - **Transaction-count checkpoint trigger:** with
    `checkpoint_every_n_transactions = 3`, driving 7 `after_commit` calls over a
    mutated arena yields exactly `commit_count() == 2` (at the 3rd and 6th);
    `stats().checkpoints_committed == 2`.
  - **Tick checkpoint trigger:** with `checkpoint_tick_interval = 100` and one
    transaction since the last checkpoint, `tick(50)` commits nothing and
    `tick(100)` commits exactly once; a subsequent `tick(250)` with **no**
    intervening transaction commits nothing and bumps `checkpoints_skipped_clean`.
  - **Explicit request:** `request_checkpoint()` after a transaction commits once;
    on a clean scene it still calls `commit` (which skips the data msync —
    `data_msyncs()` unchanged — but advances `commit_count()`), proving the
    explicit trigger is unconditional while the automatic triggers are not.
  - **Anonymous arena:** with `Checkpointer* == nullptr`, all checkpoint entry
    points are no-ops (no crash, `checkpoints_committed == 0`) while drains still
    run and `stats().live_slots` tracks the arena.
  - **Error propagation:** a `Checkpointer` over a fault-injected workspace source
    (a `sync_header` failure) makes `request_checkpoint()` return the
    `WorkspaceFileError` value, not abort.
  - **Stats snapshot:** after a mixed run, `HousekeepingStats` fields
    (`transactions_seen`, `drains_run`, `checkpoints_committed`,
    `checkpoints_skipped_clean`, `live_slots`, `slots_freed_to_list`,
    `durable_epoch`) match the driven counts and the underlying `Arena` /
    `Checkpointer` counters.
- **Claims (register + `enforces:` tags)** appended to
  `tests/claims/registry.tsv`, enforced from the tests above:
  - `15-memory-model#housekeeping-drains-between-transactions` — "After a
    transaction commit, the housekeeper's between-transaction drain runs the
    reclamation queue to quiescence, so every slot released during that
    transaction is reclaimed before the next transaction begins and the arena's
    live count returns to its no-garbage baseline; disabling the between-transaction
    drain leaves those slots on the queue until an explicit quiesce." (doc 15:129-136)
  - `15-memory-model#checkpoint-cadence-is-policy` — "The housekeeper commits a
    checkpoint exactly when a configured trigger fires — every Nth transaction, a
    tick past the interval with at least one intervening transaction, or an
    explicit host request — and an automatic trigger on a scene with no transaction
    since the last checkpoint is skipped, so a still scene issues no checkpoint."
    (doc 15:213-214)
  - `15-memory-model#housekeeping-reports-memory-panel-stats` — "The housekeeper
    exposes a wall-clock-free stats snapshot whose live-slot count reflects the
    arena's reachable slots after a drain and whose drain/commit event counters
    advance exactly once per drain/commit, giving a host memory panel an honest
    view of version-GC activity." (doc 15:149-151)
- **Behavioral-counter discipline (doc 16:54-62).** Every cadence assertion is on a
  counter (`commit_count`, `data_msyncs`, `slots_live`, the `Housekeeper`'s event
  counters) driven by deterministic transaction/tick inputs; **no test reads a
  wall-clock or sleeps**.
- **Named future task (closer registers in WBS).** The **owned background
  low-priority housekeeping thread** is deferred to
  **`runtime.housekeeping_thread`** — effort **2d**, `depends !housekeeping`
  (this task), wired into milestone **`m2_editing`** (`99-milestones.tji:21-25`),
  `note`: "Low-priority background housekeeping thread: a `std::thread` that
  parks/wakes on a condition variable and periodically calls
  `Housekeeper::tick()`, serializing as the single drainer against writer-thread
  drains, with graceful stop/join; stress + TSan coverage of RT-thread
  enqueue-while-draining. Docs 14/15. Refinement:
  tasks/refinements/runtime/housekeeping.md." Its TSan coverage inherits the
  standing CI-lane gap (`parking-lot.md:33-41`).
- **Component wiring & CI dependency check:** `src/runtime/CMakeLists.txt` adds
  `housekeeping.cpp` to `SOURCES`, `arbc/runtime/housekeeping.hpp` to
  `PUBLIC_HEADERS`, `t/housekeeping.t.cpp` to the component test, and **`pool`** to
  `DEPENDS`; the header compiles standalone; the doc-17 dependency check passes
  (no same-level edge; `pool` is a lower level).
- **Gate green (build + tests in Debug + ASan/UBSan).** No TSan obligation
  (single-threaded passive policy object; the pool primitives' concurrency is
  `pool.*`'s).

## Decisions

- **A passive, thread-agnostic policy object; the caller owns the thread and
  serializes the drainer.** The `Housekeeper` exposes hooks (`after_commit`,
  `tick`, `request_checkpoint`, `drain_and_quiesce`) and never spawns a thread or
  reads a clock. Doc 15:129-136 sanctions *either* drain site (writer between
  transactions or a low-priority thread), and `reclamation.hpp:57-62` requires the
  caller to serialize the single drainer — so making the object passive keeps it
  correct under both regimes and lets v1 drive it synchronously on the writer with
  no concurrency surface. *Rejected:* baking a `std::thread` into the `Housekeeper`
  now — it would pull thread lifecycle, park/wake, and a TSan obligation into a 1d
  policy task, and the mechanism (single-drainer-any-thread, `free_pools.md`) is
  already thread-agnostic, so the thread is cleanly additive later.
- **The timer trigger is a handed monotonic tick, not a wall-clock read.** Doc
  16:54-62 forbids wall-clock assertions and the runtime frame loop owns the real
  clock (doc 17:60); passing `tick(monotonic)` makes the cadence deterministically
  testable and keeps the L5 policy from reading a clock the way the compositor's
  `Deadline` is a handed value, not a `now()`. *Rejected:* an internal
  `steady_clock::now()` — untestable without sleeps, and it duplicates the frame
  loop's clock ownership.
- **Dirtiness is proxied by "transactions since last checkpoint," not
  `Checkpointer::is_dirty()`.** `is_dirty()` is private (`checkpoint.hpp:270-273`),
  and the `Housekeeper` already sees every `after_commit`, so counting
  transactions-since-checkpoint is a sufficient, public, deterministic dirtiness
  signal for the *policy* decision (whether to fire a commit at all).
  `Checkpointer::commit` independently skips the data msync on a truly-unchanged
  scene (`:130-132`), so the two layers are belt-and-suspenders. *Rejected:*
  widening `Checkpointer::is_dirty()` to public — a pool API change to serve a
  runtime policy the transaction count already answers; and *rejected:* sampling
  `Arena::total_slots_live()`/`total_high_water()` deltas in the `Housekeeper` —
  redundant with the transaction count and blind to same-live-count in-place
  edits.
- **Explicit `request_checkpoint()` is unconditional; automatic triggers gate on
  dirtiness.** Doc 15:213-214 lists the explicit host call as a first-class
  trigger (autosave, export, quit); a host that asks for a checkpoint gets one
  (the underlying `commit` still no-ops the data msync if genuinely clean). The
  automatic triggers gate on the transaction proxy so a still scene does not
  thrash. *Rejected:* gating the explicit call too — it would silently drop a
  host's autosave-on-quit request when the last edit happened to be a no-op.
- **Observability aggregates existing counters into a runtime `HousekeepingStats`;
  no new counting mechanism.** Doc 15:149-151 promises "per-arena live counts …
  exposed through the API (hosts will want a memory panel)"; the numbers already
  exist (`Arena::total_slots_live()`, the `Checkpointer` counters,
  `DurabilityEpochFence::quarantined_total()`). This task composes them plus its
  own event counts into one struct — the runtime-facing memory panel — rather than
  minting parallel counters. *Rejected:* threading new atomics through the pool
  layer — duplicates counting the mechanisms already do and pushes runtime
  observability policy down into L1.
- **Validate against pool fixtures, not the live `Document`.** `Document` is not
  yet slab-arena-backed with installed sinks and a `Checkpointer`
  (`document.hpp:29-32`, "migrates … when the Editable facet and the slab arenas
  land"). Testing the policy against a directly-constructed `Arena` + `RefStore` +
  `ReclamationQueue` + workspace `Checkpointer` exercises the real mechanisms
  without waiting on that migration, and matches how the pool refinements test
  their own seams. *Rejected:* blocking this task on the Document→arena rewire —
  that is a larger migration (docs 14/15) this 1d policy task should not swallow;
  the rewire consumes the `Housekeeper` when it lands (surfaced for the parking
  lot).
- **`pool` `DEPENDS` edge added; no design-doc delta.** Doc 17:60 already declares
  runtime depends on "everything below," and doc 17:24-26/:49 already assign
  reclamation+checkpoint *cadence* to runtime and the *mechanism* to pool. This
  task concretizes settled doc text (doc 15:129-136, :149-154, :213-214;
  doc 14:199-205) into a C++ policy object without altering designed behavior, so
  it needs no doc edit and no doc-00 bullet.

## Open questions

(none — all decided.)

The Document→slab-arena rewire that will let the live `runtime::Document` install
`DeferredReclaimSink`s and own a `Checkpointer` (so the `Housekeeper` drives real
document memory rather than a test fixture) is a larger migration gated on the
Editable-facet/arena work (docs 14/15, `document.hpp:30-31`); its exact task shape
and dependencies are not settled here. Surfaced in the return summary for the
parking lot rather than encoded as a WBS leaf.

## Status

**Done** — 2026-07-05.

- `src/runtime/arbc/runtime/housekeeping.hpp` (new) — `HousekeepingConfig`, `Housekeeper`, `HousekeepingStats` structs/class; passive thread-agnostic policy object; no wall-clock reads.
- `src/runtime/housekeeping.cpp` (new) — policy implementation: drain sites, 3 checkpoint triggers (transaction-count, tick-interval, explicit request), stats aggregation.
- `src/runtime/t/housekeeping.t.cpp` (new) — 7 Catch2 unit cases: drain on/off, transaction-count trigger, tick trigger + skip-clean, explicit request + clean-scene msync-skip, anonymous arena, error propagation via `Msync` fault injection, stats aggregation.
- `src/runtime/CMakeLists.txt` (edited) — adds `housekeeping.cpp` to SOURCES, `arbc/runtime/housekeeping.hpp` to PUBLIC_HEADERS, `pool` to DEPENDS, and `arbc_component_test` entry.
- `tests/claims/registry.tsv` (edited) — 3 new rows: `15-memory-model#housekeeping-drains-between-transactions`, `#checkpoint-cadence-is-policy`, `#housekeeping-reports-memory-panel-stats`.
- Signature note: `after_commit` returns `expected<std::monostate, WorkspaceFileError>` (not `void` as sketched in the prose) — a transaction-count-triggered commit can fail, and "errors as values, never abort" is unsatisfiable with a void return.
