# Parking lot — items for human review

This file is the queue for decisions the automated WBS loop (orchestrator +
closer + refinement_writer) **must not** make on its own: judgment calls,
"should we revisit X later" questions, scope/descope decisions, and anything
that would otherwise be (mis)encoded as a self-perpetuating "audit" WBS
task.

**Why this exists:** an open question encoded as a WBS task gets picked up
by the orchestrator, can't be closed by an implementer (the work is a human
call), and spawns a successor — a loop. Instead of a task, the closer
appends an entry here and moves on. The human triages this file and either
resolves the item, wires real *implementation* work into a milestone, or
deletes the entry.

**Who writes here:** the closer (`orchestrator/prompts/closer.md`, ritual
step 4) appends entries — both items it hits during the ritual and items
the implementer / refinement_writer flagged for human review in their
return summaries. The orchestrator's `human-intervention-needed` stop also
points here.

## Format

Append one `###` block per item, newest at the bottom:

```
### <YYYY-MM-DD> — <short title>
- **Source**: closer for `<task_id>` (commit `<sha>`), or the run that surfaced it.
- **Question**: the decision the loop could not make.
- **Why parked**: judgment call / preconditions unmet / scope decision.
```

### 2026-07-05 — SurfacePool byte-budget / one-frame-trim eviction policy
- **Source**: closer for `surfaces.surface_pool` (see accompanying commit); flagged in refinement Open questions.
- **Question**: Should a byte-budget or one-frame-trim eviction policy be added to `SurfacePool`? A long-lived cross-frame pool under sustained camera motion (many distinct temp sizes) could accumulate stale entries, but the only cross-frame caller — the interactive/video renderer — does not exist yet. Whether accumulation matters is a profiling-dependent judgment.
- **Why parked**: Trigger is interactive-renderer profiling evidence that doesn't exist yet. Adding a policy now is speculative; the refinement explicitly defers this until that renderer's profiling reveals a real problem. Human call once the interactive renderer is built and profiled.

### 2026-07-05 — KeyedStore concurrency hardening for async worker-fill + render-thread lookup
- **Source**: closer for `cache.keyed_store` (see accompanying commit); flagged in refinement Open questions.
- **Question**: `KeyedStore` is single-threaded-confined at v1. When the async render/mix model (doc 02:101-117) drives cache fills from worker threads while the render thread does lookups, the store needs reader-safe lookup and synchronized insert/evict (plus TSan + schedule-perturbation stress, doc 16:64-73). Should `KeyedStore` be hardened for concurrent access at that point?
- **Why parked**: Trigger is the async off-thread fill path landing — a runtime-architecture decision not yet made (doc 02:118-120 leaves it open). Hardening now would guess at the concurrent design and pay synchronization cost on the hot path meanwhile. Human call once the async fill model is decided.

### 2026-07-05 — Recycling evicted backend surfaces back into SurfacePool
- **Source**: closer for `cache.keyed_store` (see accompanying commit); flagged in refinement Open questions.
- **Question**: On eviction `KeyedStore` drops the `Value`, freeing its backend surface. That surface could instead be returned to `surfaces::SurfacePool` for reuse. Should this recycling path be added?
- **Why parked**: Whether the recycling pays is profiling-dependent and it couples the cache's eviction path to the pool's lifecycle. No interactive renderer exists yet to profile against. Human call once the interactive renderer is built and profiling reveals whether the optimization is worth the coupling.

### 2026-07-05 — HAMT interior nodes not trivially destructible (model.persistent_state design deviation)
- **Source**: closer for `model.persistent_state` (see accompanying commit); flagged by implementer.
- **Question**: Docs 14/15 require records to be trivially destructible so `ReclamationQueue::drain` walks them explicitly. The three *object* record types (composition, layer, content) satisfy this and pass the `static_assert` in acceptance criteria. However HAMT interior nodes are **not** trivially destructible — they own child `SlotRef` edges and the drain cascade releases children through `~T`, with nodes reaching their stores via a thread-local `ReclaimContext` the drainer publishes. Should docs 14/15 be amended to distinguish between object records (trivially destructible, mmap-compatible) and map-node records (allowed non-trivial dtor via sink-routed cascade)? Or should the HAMT implementation be redesigned to make interior nodes also trivially destructible?
- **Why parked**: Amending normative design docs is a project-shaping decision (doc 16 same-commit rule + doc 00 bullet); the choice between amending docs vs. redesigning the HAMT is a human call.

### 2026-07-05 — `map_input_damage` covering requirement: clarification vs. new normative content
- **Source**: closer for `contract.operator_members` (see accompanying commit); flagged by implementer (Decision 5).
- **Question**: The header doc-comment for `map_input_damage` explicitly states the covering (over-approximation) direction — "mapped output damage must cover every affected output pixel; over-approximation is sound, under-approximation is a bug" — as a clarification of what `docs/design/13-effects-as-operators.md:104-107` already entails. If a reviewer reads the comment as introducing new normative content rather than clarifying existing intent, should doc 13 be amended to add the explicit covering wording?
- **Why parked**: Whether the header comment constitutes a doc-13 delta triggering the doc 16 same-commit amend rule is a human judgment call. The implementer and refinement both treat it as a clarification (same move as `snapshot_pins` for `SnapshotToken*`→`StateHandle`); a reviewer may disagree.

### 2026-07-05 — Document→slab-arena rewire to let Housekeeper drive live document memory
- **Source**: closer for `runtime.housekeeping` (see accompanying commit); flagged in refinement Open questions and implementer summary.
- **Question**: The `runtime.housekeeping` task validated its policy against pool fixtures (an `Arena` + `RefStore<T>` + `ReclamationQueue` + workspace `Checkpointer`) rather than the live `runtime::Document`, because `Document` is not yet slab-arena-backed with installed `DeferredReclaimSink`s and a `Checkpointer` (`document.hpp:29-32`, "migrates … when the Editable facet and the slab arenas land"). Should a follow-up task be added to wire the `Housekeeper` into the live `Document`/`Model` arenas once the Editable-facet/arena migration (docs 14/15) is complete?
- **Why parked**: The exact task shape and dependencies are not settled — the trigger is the Editable-facet/arena migration landing, which is a larger ongoing effort. Adding a WBS leaf now would encode unknown prerequisites. Human call once `model.editable_facet` and the slab-arena migration are far enough along to scope the rewire concretely.
