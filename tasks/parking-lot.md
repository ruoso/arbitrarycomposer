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

### 2026-07-04 — TSan CI lane for pool reclamation concurrent enqueue-while-draining
- **Source**: closer for `pool.reclamation` (commit TBD — see commit immediately after this entry's merge).
- **Question**: Should a ThreadSanitizer lane be added to CI (`.github/workflows/`) to cover the 8-producer concurrent enqueue-while-draining smoke test in `src/pool/t/reclamation.t.cpp`? The test runs under the gate's dev+asan lanes today, but no TSan preset/lane exists in this repo.
- **Why parked**: Editing `.github/workflows/` is out of the permitted scope for `pool.reclamation`. Requires a human decision on CI resource cost and the right preset configuration for TSan.
