# runtime.housekeeping_document_wiring — Wire Housekeeper onto the live Document arenas

## TaskJuggler entry

`tasks/65-runtime.tji:158-163` → `runtime.housekeeping_document_wiring` ("Wire
Housekeeper onto the live Document arenas"). It carries
`depends !housekeeping_thread, model.workspace_backing` (`65-runtime.tji:161`).
Note line:

> "Drive the live Document/Model slab arenas with the Housekeeper
> (reclamation-drain cadence, checkpoint cadence policy over `Model::checkpoint`,
> memory observability) — `runtime.housekeeping` validated the policy against pool
> fixtures only, before Model was arena-backed with installed sinks and a
> Checkpointer. Source: `tasks/parking-lot.md` 2026-07-05 (Document-to-slab-arena
> rewire). Docs 14/15."

It is wired into milestone M9 (`tasks/99-milestones.tji:72`).

## Effort estimate

**1d as scheduled — and this is a full, tight day, not a half one.** The estimate
was set at triage (parking-lot resolution, 2026-07-10) when the task read as pure
plumbing: hand the shipped `HousekeepingThread` the Model's queue and arena, done.
Two facts discovered while refining make it more than that, and both are
*prerequisites for correctness*, not scope creep:

1. **`Housekeeper` cannot be pointed at `Model`'s `ReclamationQueue` as-is.** It
   calls `ReclamationQueue::drain()` bare (`src/runtime/housekeeping.cpp:22,39,72`),
   while `~HamtNode` reaches its stores through a **thread-local reclaim context**
   and *silently returns without releasing its child edges* when that context is
   null (`src/model/arbc/model/hamt.hpp:103-109`). `Model::drain()` publishes the
   context (`src/model/model.cpp:764-767`); a `Housekeeper` bound to the raw queue
   would not — a silent, total leak of every interior HAMT edge, with no crash and
   no failing assertion. The drain must route through `Model::drain()`.
2. **A background drainer on a live `Document` races the writer in the runtime's
   routing table.** `EditableRouter::d_table` is a bare `std::unordered_map<ObjectId,
   Editable*>` (`src/runtime/arbc/runtime/editable_binding.hpp:80`), read on the
   drain thread via the state-release path and mutated on the writer thread by
   `bind`/`unbind` (`src/runtime/editable_binding.cpp:26,38,52`). Concurrent
   `find`-vs-rehashing-`insert` is UB whose failure mode is "release another
   content's tile blobs".

So the day is: the target seam (1), the router guard (2), file-backed `Document`
construction (without it the checkpoint half of the task has no live consumer —
see Constraint 4), the `Document` wiring itself, and the tests. It is achievable
because every underlying mechanism — `Housekeeper`, `HousekeepingThread`,
`Checkpointer`, `Model::checkpoint`, `Model::create/open` — already exists and is
tested. Nothing new is *invented* here; it is connected, and the two hazards above
are closed.

## Inherited dependencies

**Settled:**

- `runtime.housekeeping` (DONE 2026-07-05, refinement
  `tasks/refinements/runtime/housekeeping.md`). Landed `arbc::Housekeeper`
  (`src/runtime/arbc/runtime/housekeeping.hpp:64-119`): the passive,
  thread-agnostic cadence policy — `after_commit`, `tick(monotonic_tick)`,
  `request_checkpoint`, `drain_and_quiesce`, `stats`. Its `HousekeepingConfig`
  (`:37-48`) carries `drain_between_transactions`,
  `checkpoint_every_n_transactions`, `checkpoint_tick_interval`; its
  `HousekeepingStats` (`:54-62`) carries `transactions_seen`, `drains_run`,
  `checkpoints_committed`, `checkpoints_skipped_clean`, `live_slots`,
  `slots_freed_to_list`, `durable_epoch`. Its Decisions section states the
  deferral this task now discharges, verbatim
  (`tasks/refinements/runtime/housekeeping.md:454-463`): *"Validate against pool
  fixtures, not the live `Document`."*
- `runtime.housekeeping_thread` (DONE 2026-07-05, refinement
  `tasks/refinements/runtime/housekeeping_thread.md`). Landed
  `arbc::HousekeepingThread` (`src/runtime/arbc/runtime/housekeeping_thread.hpp:67-135`):
  owns a `Housekeeper` **by value** behind a `std::mutex` that guards *every*
  entry point, so it is structurally the single drainer; parks on a condition
  variable with a `tick_period` (default 50ms); `flush()` synchronizes on a tick
  counter, never a clock; graceful `request_stop()` + join-in-dtor with a final
  drain-to-quiescence.
- `model.workspace_backing` (DONE 2026-07-11, refinement
  `tasks/refinements/model/workspace_backing.md`). Made `Model` optionally
  file-backed: `Model::create(path)` / `Model::open(path)`
  (`src/model/arbc/model/model.hpp:210,227`) build the `Arena` over a
  `WorkspaceFileChunkSource`, own a `std::optional<Checkpointer>`, and install the
  `DurabilityEpochFence` on both stores. `Model::checkpoint()` (`:243`, impl
  `src/model/model.cpp:730-739`) resolves its own current root and commits;
  `Model::checkpointer()` (`:252`) is documented in-header as *"the seam
  `runtime.housekeeping` drives for cadence"*. The default-constructed `Model`
  stays anonymous and byte-identical: no checkpointer, no fence.
- `runtime.editable_sink_multiplex` (DONE, `65-runtime.tji:151-157`). Landed the
  per-`Document` `EditableBinding` / `EditableRouter` that routes
  `retain`/`release`/`state_cost` by owning `ObjectId`. This task hardens its
  routing table for concurrent lookup (Decision 3).

**Pending:** none. Every dependency is complete.

## What this task is

Make a live `Document`'s memory actually get managed. Today a `Document`
default-constructs an anonymous `Model` (`src/runtime/arbc/runtime/document.hpp:211`)
and its entire housekeeping surface is `Document::drain()`, a synchronous
`d_model.drain()` (`src/runtime/document.cpp:55`) that only tests ever call
(`tests/raster_runtime_binding.t.cpp:97,146,158`,
`src/runtime/t/editable_sink_multiplex.t.cpp:65,79,145`). Nothing drains between
transactions. Nothing ever checkpoints. Nothing reports how much memory the
document holds. The `Housekeeper` and `HousekeepingThread` that were built to do
all three have **zero production call sites** — they are referenced only by their
own unit tests and `tests/stress_publish_pin.t.cpp`, which builds a hand-rolled
pool substrate precisely because `Model` was not yet wirable
(`tests/stress_publish_pin.t.cpp:16-19`).

This task connects them. A `Document` gains a `HousekeepingThread` driving its
`Model`'s arena through a small abstract target seam; every transaction commit
routes through `after_commit()` so the between-transaction drain and the
transaction-count checkpoint trigger fire; `Document` gains file-backed
construction (`create`/`open`) so the checkpoint path has something to commit to;
and `Document` gains a `memory_stats()` accessor so a host memory panel — doc 15's
stated reason for per-arena accounting — has an API to read.

It also closes the two correctness hazards that wiring exposes: the drain must
publish the HAMT reclaim context (else every interior edge leaks silently), and
the state-release routing table must tolerate a lookup on the drain thread
concurrent with a bind on the writer thread (else it is UB that frees the wrong
content's pixels).

## Why it needs to be done

The reclamation queue is the mechanism doc 15 chose so that "a render thread
unpinning after an edit-heavy export" does not destroy thousands of nodes inline
(`docs/design/15-memory-model.md:134-137`). That deferral is only half a design:
enqueueing without draining is just leaking on a delay. Today, a `Document` that
is edited and then left idle never reclaims anything, because the only drain is a
manual `Document::drain()` no production code calls. The background thread that
makes idle reclamation happen has been sitting unused since 2026-07-05.

Downstream, three promises are blocked on this:

- **Doc 15's memory panel** (`15-memory-model.md:164-169`: *"per-arena live counts
  and byte accounting exposed through the API (hosts will want a memory panel)"*).
  There is no memory-stats API on `Document` at all today.
- **Doc 15's crash recovery** (`15-memory-model.md:179-187`: *"An editor crash
  costs at-most-since-last-checkpoint, not the document"*). `Model` can checkpoint;
  no `Document` is file-backed, so no document ever does.
- **Doc 14's version GC** (`14-data-model-and-editing.md:286-291`: *"version GC
  releases unpinned `DocState` nodes and unreferenced state handles by refcount"*).
  The release path exists end-to-end and is never driven.

This is also the last consumer the parking-lot entry named. The 2026-07-05
"Document→slab-arena rewire" item was resolved at triage on 2026-07-10 with:
*"the trigger has fired — `model.editable_facet` and the slab-arena migration
landed … but the Housekeeper still has no live-document consumer."* This task is
that consumer.

## Inputs / context

**Design docs (normative):**

- `docs/design/15-memory-model.md:134-151` — deferred cascades, the reclamation
  queue, the drainer-published reclaim context. `:152-172` — thread rules,
  **including this task's delta** (Decision 1/2 below): the drainer is not the
  writer, and the checkpointer is. `:173-176` — document teardown is arena drop,
  "bulk-release path runs the reclamation queue to quiescence first".
  `:164-169` — debug discipline: per-arena live counts + byte accounting through
  the API. `:243-264` — checkpointing rides the version model; cadence is policy;
  **including this task's delta**: cadence decides *when*, never *where*.
- `docs/design/14-data-model-and-editing.md:168-181` — the `Editable` facet's
  `retain`/`release` contract, **including this task's delta**: `retain` is
  writer-thread, `release` is drain-thread, and they are not the same thread.
  `:286-291` — version GC by refcount.
- `docs/design/17-internal-components.md:46-61` — levelization. `pool` is L1,
  `model` is L2, `runtime` is L5 and owns *"`Document` (arenas + model + registry
  + loaders) … housekeeping thread"*. `:113-127` — *"`runtime` composes a
  `HousekeepingStats` snapshot; `pool` counts on its arenas"*.
- `docs/design/00-overview.md` § Resolved questions — this task's decision-record
  bullet ("The housekeeping thread drains; the writer checkpoints").

**Sources this task extends:**

- `src/runtime/arbc/runtime/housekeeping.hpp:37-119` — `HousekeepingConfig`,
  `HousekeepingStats`, `Housekeeper`. Ctor at `:70-71` takes
  `(ReclamationQueue&, Checkpointer*, Arena*, HousekeepingConfig)`;
  `after_commit(SlotIndex root)` at `:81` records a commit tip in `d_tip` (`:113`).
- `src/runtime/housekeeping.cpp:20-74` — `after_commit`, `tick`,
  `request_checkpoint`, `drain_and_quiesce`. **All three call `d_queue->drain()`
  bare** (`:22,39,72`) — no reclaim context.
- `src/runtime/arbc/runtime/housekeeping_thread.hpp:46-135` — the thread wrapper;
  ctor at `:73-74`, `d_mutex` at `:125` guarding every entry point.
- `src/runtime/arbc/runtime/document.hpp:29-226` — `Document`. Members at
  `:187-226`; **declaration order is the teardown contract**. `Model d_model;` at
  `:211` is default-constructed (anonymous). `Document::drain()` decl at `:124`,
  impl `src/runtime/document.cpp:55`. `document_access.hpp:21-23` —
  `HostViewportDocumentAccess::model(Document&)`, the established attorney-client
  pattern for reaching `d_model` without widening `Document`'s public shape.
- `src/runtime/document.cpp:33` (`add_content` → `d_binding.bind`), `:51`
  (`transact()`), `:57` (`add_layer`), `:64` (`set_layer_transform`) — the
  mutators, each of which calls `t.commit()` directly. **There is no post-commit
  hook today.**
- `src/model/arbc/model/model.hpp:210,227,243,246,252,257,269,274` —
  `create`/`open`/`checkpoint`/`workspace_backed`/`checkpointer`/`workspace_source`/`drain`/`live_slots`.
  `d_arena` (`:563`), `d_queue` (`:569`), `d_reclaim_ctx` (`:570`) are **private,
  with no accessors** — the `Housekeeper` ctor's `(ReclamationQueue&, Arena*)`
  cannot be satisfied from outside `Model` today.
- `src/model/arbc/model/model.hpp:157-173` — `ContentStateReclaimSink::on_zero`,
  which calls `(*d_sink)->release(id, state)` on the drain thread; `d_sink` is a
  `StateRefSink**` aliasing `Model::d_state_ref_sink` (`:580`), a plain pointer.
- `src/model/arbc/model/hamt.hpp:50-109` — `ReclaimContext`,
  `active_reclaim_context()` (thread-local), `ReclaimContextGuard`, and
  `~HamtNode`'s early return when the context is null.
- `src/runtime/arbc/runtime/editable_binding.hpp:56-121` — `EditableRouter::route`
  (`d_table.find`), `d_table` (`:80`, unsynchronized), `d_unrouted` (`:82`,
  non-atomic, incremented from both threads), `EditableStateRefSink::release`
  (`:97-101`). `src/runtime/editable_binding.cpp:26,37-38,44,52` — `bind`,
  `unbind` (which itself calls `d_model->drain()`), `unbind_all`.
- `src/pool/arbc/pool/checkpoint.hpp:96-235` — `Checkpointer`; `commit()` at
  `:158`, whose seal loop (`:209-213`), high-water publish (`:185-188`), and
  `drain_fences()` (`:363-389`) are all writer-thread-only structures.
- `src/kind_raster/arbc/kind_raster/raster_content.hpp:216-219` — `RasterStore`'s
  `std::mutex` guarding versions + free list; `raster_content.cpp:420-448` —
  `retain_version`/`release_version` both under it. **The kind side is already
  thread-safe**; only the runtime router is not.

**Tests to extend / model on:**

- `src/runtime/t/housekeeping.t.cpp:206-226` — the `WsFixture` recipe (source,
  arena, store, `Checkpointer`, `DeferredReclaimSink`, queue, fence install,
  `TempPath` mkstemp/unlink) and the `Msync` fault-injection case at `:292`.
- `src/runtime/t/housekeeping_thread.t.cpp` — the injected-`tick_source`
  determinism recipe and the stress-test shape.
- `tests/stress_publish_pin.t.cpp:117,176-177` — the existing background-drainer
  stress test, over an **anonymous** arena with a `nullptr` checkpointer. It is
  the template for this task's Document-level stress test.
- `src/model/t/workspace_backing.t.cpp:680-735` — checkpoint-with-a-pinned-reader.
- `tests/claims/registry.tsv:111-115` — the five shipped housekeeping claims.

## Constraints / requirements

1. **The drain must publish the HAMT reclaim context.** Any drain of a `Model`'s
   queue, on any thread, must run inside a `ReclaimContextGuard` — otherwise
   `~HamtNode` (`hamt.hpp:103-109`) returns without releasing its child edges and
   the arena leaks silently. `Model::drain()` already does this; the wiring must go
   through it, not around it (Decision 1).

2. **Exactly one drainer at a time, with no unsynchronized back door.**
   `pool/reclamation.hpp:57-62` requires it. `HousekeepingThread`'s mutex delivers
   it *only if every drain entry point goes through the thread object*. Today two
   paths bypass it: `Document::drain()` (`document.cpp:55`) and
   `EditableBinding::unbind`/`unbind_all`, which call `d_model->drain()` directly
   (`editable_binding.cpp:37,44`). Both must be re-routed.

3. **The background thread must never commit a checkpoint.** Per the doc 15 delta
   and the analysis behind it, `Checkpointer::commit` racing a writer transaction
   is memory-unsafe in four distinct ways — `WorkspaceFileChunkSource::d_live` is a
   `std::map` iterated by `sync_data` (`src/pool/workspace_file.cpp:1078-1089`)
   while `grow()` inserts into it (`:713`); the seal loop mprotects chunks
   read-only off a **non-atomic** `d_high_water` (`slot_store.hpp:326`) that
   `allocate()` bumps *before* the caller's placement-new
   (`src/pool/slot_store.cpp:208`); the seal→reopen window
   (`checkpoint.hpp:209-229`) lets the writer pop a free-list slot in a
   just-sealed chunk; and `drain_fences()` resizes the quarantine vector
   (`checkpoint.hpp:377`) that `on_release` push_backs to. Therefore the
   `Document`'s `HousekeepingConfig` **must leave `checkpoint_tick_interval`
   empty**, and every commit must originate on the writer thread.

4. **The checkpoint path needs a file-backed `Document`.** `Model::checkpoint()`
   on an anonymous model returns `WorkspaceFileErrc::Unsupported`
   (`src/model/model.cpp:731-733`). `Document` has no `create`/`open` — every
   `Document` is anonymous today. Without file-backed construction, "checkpoint
   cadence policy over `Model::checkpoint`" is untestable at the `Document` level
   and the task delivers nothing the note asks for.

5. **Teardown ordering.** The housekeeping thread must stop and run its final
   drain-to-quiescence while *every* sink target is still alive — the
   `EditableBinding` router, the `Content` objects in `d_contents`, and the `Model`
   itself. Since destruction is reverse-declaration order, the `HousekeepingThread`
   must be the **last-declared member** of `Document`, after `d_pending_loads`
   (`document.hpp:226`). `~Model` also drains (`model.cpp:741-749`); a
   double-drain-to-quiescence is idempotent and harmless.

6. **Anonymous `Document`s stay cheap and correct.** The default-constructed
   `Document` gets a drain-only housekeeper: no checkpointer, no fence, no msync.
   `Document::checkpoint()` on it returns `Unsupported` rather than asserting.

7. **Levelization (doc 17).** `runtime` (L5) → `model` (L2) → `pool` (L1). All
   edges used here already exist (`src/runtime/CMakeLists.txt` already `DEPENDS`
   `pool`, added by `runtime.housekeeping`). **No new component edge**, and nothing
   in `model` or `pool` may learn about `runtime` — which is exactly why the target
   seam (Decision 1) lives in `runtime` and `Model` gains no `Housekeeper`
   knowledge.

8. **The writer's hot path stays lock-free.** The router guard (Decision 3) sits on
   the *state-release routing table*, not on the transaction/publish path.
   `Model::d_current` remains a single atomic store (`model.hpp:573`); no mutex may
   be added to `Model::Transaction`.

## Acceptance criteria

**New claims-register entries** (`tests/claims/registry.tsv` + `enforces:`-tagged
tests):

- `15-memory-model#document-drain-runs-through-housekeeper` — "A Document's
  reclamation drain runs through its Housekeeper: after each transaction commit the
  queue is drained to quiescence, so every record the transaction released — and
  every content state handle those records held — is reclaimed before the next
  transaction begins, and the Model's live-slot count returns to its no-garbage
  baseline." Enforced by a component test in a new
  `src/runtime/t/housekeeping_document.t.cpp`: commit N transactions that churn
  layers on a live `Document`, assert `Model::live_slots()` returns to baseline
  with no manual `drain()` call, and assert the interior HAMT edges were actually
  released (a bare-queue drain without the reclaim context would leave live_slots
  pinned above baseline — this is the regression test for Hazard 1).
- `15-memory-model#checkpoint-commit-is-writer-thread` — "The background
  housekeeping thread never commits a checkpoint: on a workspace-backed Document
  driven concurrently by a writer, every Checkpointer commit is issued from the
  writer thread, and the background thread's ticks advance only the drain counter."
  Enforced by a **behavioral-counter assertion** in the stress test: across a run
  with ≥1000 background ticks, `stats().checkpoints_committed` advances exactly
  once per writer-side trigger and `HousekeepingThread::background_ticks()` > 0
  while no commit is attributable to a background tick.
- `15-memory-model#document-checkpoint-cadence` — "A workspace-backed Document
  commits a durable checkpoint exactly when a writer-side trigger fires — every Nth
  transaction, or an explicit host request — and an explicit request on a Document
  with no transaction since the last checkpoint skips the data msync, so an idle
  document issues no redundant durable writes." Enforced by an integration test
  `tests/document_workspace_checkpoint.t.cpp`.
- `14-data-model-and-editing#state-release-routes-under-concurrent-binding` —
  "Content-state release routed on the drain thread while the writer concurrently
  binds and unbinds editable contents releases each state handle exactly once and
  to its owning content." Enforced by the stress test (Hazard 2's regression test).

**Existing claims that must keep passing unchanged:**
`15-memory-model#housekeeping-drains-between-transactions`,
`#checkpoint-cadence-is-policy`, `#housekeeping-reports-memory-panel-stats`,
`#housekeeping-thread-single-drainer`, `#housekeeping-thread-stops-gracefully`
(`tests/claims/registry.tsv:111-115`) — the target-seam refactor (Decision 1)
rewrites their fixtures' construction lines but must not change what they assert.
`#freed-slot-quarantined-until-durable` (`:62`) and the `model.workspace_backing`
recovery claims (`:73-74`) likewise.

**Test files:**

- `src/runtime/t/housekeeping_document.t.cpp` (new) — component tests: drain
  between transactions on a live `Document`; content-state release reaches the
  bound `Editable`; anonymous `Document::checkpoint()` → `Unsupported`;
  `memory_stats()` reports live slots and reserved bytes; **teardown ordering** —
  destroy a `Document` with a still-running background thread and a bound editable
  content, and assert (ASan-clean, plus a release-count assertion on a test double)
  that the final drain ran while the router and contents were alive.
- `tests/document_workspace_checkpoint.t.cpp` (new) — integration: `Document::create(path)`,
  `checkpoint_every_n_transactions` cadence, explicit `Document::checkpoint()`,
  still-document skip (`checkpoints_skipped_clean` advances), and a `Document::open(path)`
  round trip resuming the last durable root. Reuses the `TempPath` recipe from
  `src/runtime/t/housekeeping.t.cpp:206-226`.
- `tests/stress_document_housekeeping.t.cpp` (new) — the concurrency coverage doc
  16 requires of a concurrency-touching task. A live workspace-backed `Document`:
  one writer thread committing transactions that add/remove editable raster
  contents (driving `bind`/`unbind` churn through the router) and paint into them;
  two render threads pinning/unpinning `DocState`s; the `Document`'s own background
  `HousekeepingThread` draining. Fixed op count, `std::this_thread::yield()`
  perturbation, **outcome-only assertions** (never wall-clock): after a final
  `drain_and_quiesce()`, `Model::live_slots()` equals the no-garbage baseline, every
  `RasterStore` version refcount is zero, and the counter assertions of
  `#checkpoint-commit-is-writer-thread` hold. Mirrors
  `tests/stress_publish_pin.t.cpp:174-201`.
  **Note:** `FakeEditable` (`src/runtime/t/fake_editable.hpp`) has unsynchronized
  `std::map` refcounts and **cannot** be used as the editable in this test — use
  `RasterContent`, whose store is mutex-guarded (`raster_content.hpp:216-219`), or
  give the stress test its own atomically-counted double.

**Lanes:** the gate (dev + ASan/UBSan) must be green. There is still no TSan preset
(`CMakePresets.json` has only `asan`) — the stress test is written TSan-ready and
the TSan lane remains the standing parked item that `runtime.housekeeping_thread`
and `tests/stress_publish_pin.t.cpp` already depend on. Do **not** register a TSan
task here; it is already parked.

**Coverage:** ≥90% diff coverage on changed lines (CI gate). Note the diff-coverage
gate silently SKIPs when run from a git worktree — run it from the main checkout.

**Deferred follow-up** (closer registers in WBS, milestone M9 —
`tasks/99-milestones.tji:72`, the same milestone as this task):

- `runtime.background_checkpoint_quiesce` — **3d** — "Add a writer↔checkpointer
  quiesce seam so a timer-cadence checkpoint can commit off the writer thread:
  guard `WorkspaceFileChunkSource::d_live` against `sync_data` iteration racing
  `grow()` insertion (`src/pool/workspace_file.cpp:713,1078-1089`), make
  `SlotStore::d_high_water` atomic and close the seal→reopen window
  (`src/pool/arbc/pool/checkpoint.hpp:209-229`, `slot_store.hpp:326`), fix
  `reusable_slots()`'s unlocked read of per-thread local pools
  (`src/pool/slot_store.cpp:255-262`), and serialize `DurabilityEpochFence`'s
  quarantine against `drain_fences()` compaction; then enable
  `checkpoint_tick_interval` on `Document` and extend
  `tests/stress_document_housekeeping.t.cpp` with a background committer.
  Source-of-debt: `tasks/refinements/runtime/housekeeping_document_wiring.md`
  (Decision 2). Docs 15/17." Depends on `!housekeeping_document_wiring`.

## Decisions

### 1. `Housekeeper` drives an abstract `HousekeepingTarget`, not a raw `(queue, checkpointer, arena)` triple

`Housekeeper`'s ctor (`housekeeping.hpp:70-71`) takes pool primitives and calls
`ReclamationQueue::drain()` directly. Pointing it at `Model`'s queue is not merely
awkward — it is *wrong*, because the drain would run without the thread-local
`ReclaimContext` that `~HamtNode` needs (`hamt.hpp:103-109`), silently leaking every
interior edge. And `Housekeeper::after_commit(SlotIndex root)`'s `d_tip` mechanism
duplicates work `Model::checkpoint()` already does: it resolves its own root from
`d_current` (`model.cpp:734-736`).

**Chosen:** introduce a small abstract seam in `runtime`:

```cpp
class HousekeepingTarget {
 public:
  virtual ~HousekeepingTarget() = default;
  // Drain to quiescence. The implementation publishes whatever per-thread
  // context its destructors need (Model publishes the HAMT ReclaimContext).
  virtual void drain() = 0;
  // Commit a durable checkpoint at the target's current published root.
  // WRITER-THREAD ONLY (doc 15). Unsupported when not workspace-backed.
  virtual expected<std::monostate, WorkspaceFileError> checkpoint() = 0;
  virtual bool checkpointable() const noexcept = 0;
  virtual std::size_t live_slots() const noexcept = 0;
  virtual std::size_t bytes_reserved() const noexcept = 0;
  virtual std::uint64_t slots_freed_to_list() const noexcept = 0;
  virtual std::uint32_t durable_epoch() const noexcept = 0;
};
```

with two implementations in a new `src/runtime/arbc/runtime/housekeeping_targets.hpp`:
`ModelHousekeepingTarget` (wraps `Model&`; `drain()` → `Model::drain()`,
`checkpoint()` → `Model::checkpoint()`, stats → `Model::live_slots()` and two new
thin `Model` accessors) and `PoolHousekeepingTarget` (wraps
`(ReclamationQueue&, Checkpointer*, Arena*)` plus a `set_root(SlotIndex)`,
reproducing today's semantics exactly for the existing pool-fixture tests).
`Housekeeper` and `HousekeepingThread` take `HousekeepingTarget&`;
`Housekeeper::after_commit()` loses its `SlotIndex` parameter — the target owns the
root question.

*Why:* it puts the reclaim-context responsibility in the one place that already
discharges it correctly (`Model::drain()`), so no future drainer can reintroduce
Hazard 1 by construction. It keeps `Model`'s arena, queue, and reclaim context
**private** — no `Model::arena()` / `Model::reclamation_queue()` accessors leak out,
which would be the alternative's price. And it lets `Housekeeper` stop knowing pool
types at all, which is the honest shape for a policy object.

*Rejected — expose `Model::arena()` + `Model::reclamation_queue()` and have the
runtime install a `ReclaimContextGuard` on each draining thread.* This is the
smaller diff, and it is a trap: the guard is a thread-local publish that must be
established on *every* thread that can reach a drain (background thread, writer via
`after_commit`, host via `drain_and_quiesce`), and forgetting one produces no crash
and no failing test — just a leak. It also widens `Model`'s public surface with two
accessors whose only correct use is "hand these to something that will guard them",
which is precisely the API that invites the bug.

*Rejected — a pool-level `DrainScope` hook on `ReclamationQueue`* (a virtual
`enter()`/`leave()` that `Model` implements to publish the context, so
`ReclamationQueue::drain()` is self-guarding for every drainer forever). This is
arguably the most robust fix, and it is tempting. But it adds a virtual dispatch to
L1's hot drain loop for one consumer, and it leaves the root/tip duplication
untouched — so the target seam would still be wanted. Reusing an existing seam
(`Model::drain()`) beats adding a new one two levels down.

*Rejected — keep the old ctor as an overload alongside the target ctor.* Fewer test
edits, but two live code paths through the same policy object and an
`after_commit(SlotIndex)` whose argument is meaningless for the `Model` target.
One code path, mechanical fixture edits.

### 2. The background thread drains; every checkpoint commits on the writer thread

The `Housekeeper` supports three cadence triggers, one of which
(`checkpoint_tick_interval`) fires from `Housekeeper::tick()` — i.e. on the
background thread. For a live `Document` that trigger is **unsafe**, for the four
concrete reasons enumerated in Constraint 3: a `Checkpointer::commit` concurrent
with a writer transaction races a `std::map` iteration against insertion, mprotects
chunks read-only out from under a placement-new, reopens free-list pages off a
racily-read snapshot, and resizes the quarantine vector under a concurrent
push_back. Two of those are memory-unsafe in release builds; one is a guaranteed
SIGSEGV in debug. Nothing in the existing corpus covers it — the only
background-drainer stress test passes `nullptr` for the checkpointer
(`tests/stress_publish_pin.t.cpp:176-177`) and the only concurrent-checkpoint test
races a *read-only pinner*, never an allocator
(`src/model/t/workspace_backing.t.cpp:680-735`).

**Chosen:** `Document` configures its `HousekeepingThread` with
`checkpoint_tick_interval` **empty**, so the background thread's `tick()` drains and
nothing more. Checkpoints fire from `after_commit()` (the
`checkpoint_every_n_transactions` trigger, on the writer thread) and from
`Document::checkpoint()` → `HousekeepingThread::request_checkpoint()` (the explicit
host call, on the writer thread). Both enter through the `HousekeepingThread` mutex,
so they are serialized against background drains — which also closes the
quarantine-vector race, since `drain_fences()` and `on_release` can then never
overlap.

This is a **design-doc delta**, because doc 15's "Checkpoint cadence is policy
(timer, transaction count, explicit host call)" reads as though the timer could fire
anywhere. Amended: `docs/design/15-memory-model.md` § Version reclamation (thread
rules) now states that the drainer is not the writer and the checkpointer is, and
§ File-backed arenas now states that cadence decides *when*, never *where*.
`docs/design/00-overview.md` § Resolved questions carries the decision-record bullet
("The housekeeping thread drains; the writer checkpoints"), since it constrains
every future housekeeping consumer.

*Why not just fix the races now?* Because they are four separate defects across two
components (`workspace_file.cpp`, `slot_store.hpp`/`.cpp`, `checkpoint.hpp`), at
least one of which — making `d_high_water` atomic and closing the seal→reopen window
— changes the allocator's hot path. That is a 3d task's worth of work with its own
stress coverage, not a rider on a 1d wiring task. It is registered as
`runtime.background_checkpoint_quiesce`. Meanwhile, the transaction-count and
explicit-host-call triggers deliver everything doc 14's autosave scenario actually
needs; only *idle* autosave waits.

*Rejected — drop the background thread entirely and drain on the writer between
transactions.* `HousekeepingConfig::drain_between_transactions` already does this,
and it would make the whole task trivially safe: no concurrent drainer, no router
race, no Hazard 2. But it defeats the point. A document edited and then left idle
would never reclaim the garbage the last transaction produced, and a render thread's
big unpin would sit on the queue until the *next* edit — which may never come. Idle
reclamation is why `runtime.housekeeping_thread` exists, and this task is its only
consumer. Wire it.

### 3. Guard `EditableRouter`'s table with a `std::shared_mutex`; make its counter and `Model::d_state_ref_sink` atomic

Enabling the background drainer makes `EditableRouter::route()` a **cross-thread
read**: `ContentStateReclaimSink::on_zero` (`model.hpp:157-173`) calls
`StateRefSink::release` on the drain thread, which lands in
`EditableStateRefSink::release` → `EditableRouter::route` →
`d_table.find` (`editable_binding.hpp:56-65,80,97-101`). The writer concurrently
mutates `d_table` on every content add/remove (`editable_binding.cpp:26,38,52`).
`find` racing a rehashing `insert` on a `std::unordered_map` is UB, and the failure
mode is not benign: a torn read yields a stale `Editable*` and releases another
content's tile blobs. The kind side is already safe (`RasterStore` guards versions
and its free list with a `std::mutex`, `raster_content.hpp:216-219`); the runtime
router is the only unsafe link.

**Chosen:** a `std::shared_mutex` on `EditableRouter` — shared for `route()`, unique
for `insert`/`erase`/`clear`; `d_unrouted` becomes `std::atomic<std::uint64_t>`; and
`Model::d_state_ref_sink` (`model.hpp:580`) becomes `std::atomic<StateRefSink*>`
(the writer clears it in `unbind_all` while a drain may be reading it). Plus
Constraint 2's re-routing: `EditableBinding::unbind`/`unbind_all` must stop calling
`d_model->drain()` directly (`editable_binding.cpp:37,44`) — they take an injected
drain hook that `Document` sets to `[this]{ d_housekeeping.drain_and_quiesce(); }`,
defaulting to `d_model->drain()` when unset so the standalone `EditableBinding`
component tests keep working unchanged.

*Why a shared_mutex and not copy-on-write?* Reads (releases) dominate writes
(binds), which argues for COW with an atomic snapshot pointer — but COW needs a
reclamation scheme for the retired snapshots, which is a hazard-pointer or
epoch problem this task has no appetite for. The table holds a handful of entries
and `route()` is called once per reclaimed content record, not per pixel; a shared
lock is nowhere near a bottleneck. If profiling ever shows contention, COW is a
clean later swap behind the same interface.

*Why is this in scope?* Because it is not an optimization — it is the difference
between the wiring being correct and being UB. Shipping the background drainer
without it would ship a known use-after-free. The doc 14 delta records the contract
this establishes: `retain` is writer-thread, `release` is drain-thread, and a kind's
state store and the runtime's routing table must both admit the concurrency.

### 4. `Document` gains `create(path)` / `open(path)` and holds its `Model` by `unique_ptr`

`Model::create`/`open` return `expected<std::unique_ptr<Model>, WorkspaceFileError>`
(`model.hpp:210,227`), but `Document` holds `Model d_model` **by value**
(`document.hpp:211`), so no `Document` can be file-backed — and the checkpoint half
of this task would have no live consumer, making Constraint 4 unsatisfiable and the
task's own note undeliverable.

**Chosen:** `Document` holds `std::unique_ptr<Model> d_model`, default-constructed
to an anonymous `Model` by the existing public ctor (so every current caller and
test is source-compatible), plus two new factories
`Document::create(path)` / `Document::open(path)` returning
`expected<std::unique_ptr<Document>, WorkspaceFileError>` and forwarding to
`Model::create`/`open`. `d_journal{*d_model}` still binds a reference, and
declaration order (`d_model` before `d_journal`) already guarantees it is
constructed first. `HostViewportDocumentAccess::model()` (`document_access.hpp:21-23`)
and `DocumentSerializeAccess` return `*doc.d_model` — the attorney-client pattern is
unchanged.

*Rejected — defer file-backed `Document` to a follow-up task and land only the drain
+ stats halves.* It would fit the 1d budget more comfortably, but it guts the task:
"checkpoint cadence policy over `Model::checkpoint`" is one of the three deliverables
the WBS note names, and it cannot be tested at the `Document` level without it. The
`unique_ptr` indirection is mechanical (`d_model.` → `d_model->`, confined to
`document.cpp`, `document_access.hpp`, and `document_serialize.cpp`).

### 5. One `HousekeepingThread` per `Document`, always on

`Document` owns a `HousekeepingThread` by value as its **last-declared member**
(Constraint 5), constructed with a `ModelHousekeepingTarget` over its `Model`.
Anonymous documents get a drain-only housekeeper (no checkpointer → every checkpoint
branch is already guarded on `d_checkpointer != nullptr`,
`src/runtime/housekeeping.cpp:31,44,63`); workspace-backed documents additionally get
`checkpoint_every_n_transactions`. A `DocumentHousekeepingConfig` (thin wrapper over
`HousekeepingConfig` + `HousekeepingThreadConfig`) is accepted at construction so
tests can inject a deterministic `tick_source` and a short `tick_period`.

*Cost accepted:* every `Document` now spawns a thread that parks on a condition
variable. For the test suite (many short-lived `Document`s) that is a few tens of
microseconds each, and the thread does nothing but wait. The alternative — making the
thread opt-in and defaulting to writer-only drains — would leave the default `Document`
with exactly the idle-reclamation hole this task exists to close, and would mean the
concurrent-drain path is exercised by one stress test instead of by the whole suite.
Turning it on everywhere is how Hazard 2 gets found if the router guard is ever
regressed.

*Surfaced for the parking lot (human judgment, not a WBS task):* whether
thread-per-`Document` is the right shape for a host that opens dozens of documents at
once, or whether a process-wide housekeeping thread servicing N registered targets is
wanted. There is no such host today and no data to decide on; the target seam
(Decision 1) is exactly the abstraction a shared thread would need, so the change stays
cheap if it is ever wanted.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-11.

- Introduced `HousekeepingTarget` abstract seam (`src/runtime/arbc/runtime/housekeeping_targets.hpp`) with `ModelHousekeepingTarget` (routes drain through `Model::drain()`, publishing the HAMT `ReclaimContext`) and `PoolHousekeepingTarget` (preserves existing pool-fixture test semantics); `Housekeeper` and `HousekeepingThread` now take `HousekeepingTarget&` instead of raw pool primitives.
- `Document` gains `create(path)` / `open(path)` factories, holds its `Model` by `unique_ptr`, and owns a `HousekeepingThread` as its last-declared member (teardown ordering per Constraint 5); every transaction commit calls `after_commit()` so the between-transaction drain and checkpoint-count trigger fire automatically.
- `EditableRouter::d_table` guarded by `std::shared_mutex` (shared for `route()`, unique for bind/unbind/clear); `d_unrouted` and `Model::d_state_ref_sink` made atomic; `EditableBinding::unbind`/`unbind_all` re-routed through an injected drain hook instead of calling `Model::drain()` directly.
- `Document` gains `checkpoint()`, `memory_stats()`, and `checkpointer()` accessors; anonymous documents return `Unsupported` from `checkpoint()`.
- New claims registered: `15-memory-model#document-drain-runs-through-housekeeper`, `#checkpoint-commit-is-writer-thread`, `#document-checkpoint-cadence`, `14-data-model-and-editing#state-release-routes-under-concurrent-binding`; `bytes_reserved` assertion added to existing memory-panel claim.
- Test additions: `src/runtime/t/housekeeping_document.t.cpp` (component tests), `tests/document_workspace_checkpoint.t.cpp` (integration — create/open round-trip, cadence, skip), `tests/stress_document_housekeeping.t.cpp` (concurrency stress — writer + two render threads + background housekeeper, TSan-clean).
- `FakeEditable` (`src/runtime/t/fake_editable.hpp`) hardened with atomic counters and mutex-guarded refcount map to close the genuine data race that background draining exposed in existing tests.
- Design-doc delta: `docs/design/15-memory-model.md` and `docs/design/00-overview.md` carry the decision record ("The housekeeping thread drains; the writer checkpoints") per Decision 2.
- Tech-debt follow-up registered: `runtime.background_checkpoint_quiesce` (3d, M9) — writer↔checkpointer quiesce seam enabling `checkpoint_tick_interval` on `Document`.
