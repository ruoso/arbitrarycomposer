# Task refinements

This directory holds per-task refinement documents. Each refinement expands
the one-line description in [`project.tjp`](../../project.tjp) (and its
included `tasks/*.tji` files) into the constraints, prior decisions, and
open questions that bound the task's scope. A refinement is the source of
truth for "what does this task mean?" — read it before doing the work.

## Layout

Refinements are organized by work-stream, mirroring the `tasks/*.tji` file
split. The directory name is the first dot-segment of the fully-qualified
task id; the filename is `<task_name>.md` matching the `task <task_name>`
identifier in the `.tji` file:

```
tasks/refinements/<area>/<task_name>.md
```

Areas: `pool`, `model`, `color`, `surfaces`, `contract`, `cache`,
`compositor`, `timeline`, `audio`, `operators`, `kinds`, `serialize`,
`runtime`, `quality`, `packaging` (matching `tasks/05-…75-*.tji`).

## Refinement document shape

Each refinement covers, in roughly this order:

1. **TaskJuggler entry** — back-link to the `.tji` task definition.
2. **Effort estimate** and **Inherited dependencies** (settled or pending).
3. **What this task is** — one paragraph of plain-language scope.
4. **Why it needs to be done** — the dependency chain and downstream
   consumers.
5. **Inputs / context** — the governing design-doc sections (the `.tji`
   `note` line names the docs), real file paths with line numbers, and the
   predecessor refinements' decisions. No invented references.
6. **Constraints / requirements** — what the implementation must satisfy,
   including the doc 17 levelization edges it must respect.
7. **Acceptance criteria** — the concrete, testable check that says
   "done": claims-register entries, byte-exact goldens, behavioral-counter
   assertions, conformance-suite runs, TSan/stress coverage (see doc 16's
   taxonomy for which applies).
8. **Decisions** — pre-settled choices with rationale against alternatives.
9. **Open questions** — what remains unresolved, or `(none — all decided)`.
10. **Status** — appended on completion (see ritual below); until then a
    `## Status` heading with `_pending implementation_`.

## Task-completion ritual

When a task ships, these happen in the **same commit**:

1. **Refinement `## Status` block.** Append a `## Status` section at the
   bottom of the refinement noting **Done** with the date and brief
   pointers to the produced artifacts (files, tests, claims, design-doc
   deltas). Don't edit the prior sections — Decisions and Acceptance
   criteria are the historical record of why the task existed; the Status
   section is the historical record of how it landed.
2. **`complete 100` in the `.tji`.** Add `complete 100` immediately after
   the `allocate team` line of the matching task block in the relevant
   `tasks/*.tji` file. After editing, run
   `tj3 project.tjp 2>&1 | grep -iE "error|warning"` and confirm silence —
   there is no pre-commit hook; this manual check is the WBS gate. The
   `complete 100` marker is what tj3's scheduler (and
   `scripts/unblocked.py`) reads — refinement Status alone is for human
   readers.
3. **Milestone propagation.** If the task is the last one a milestone
   depends on, also add `complete 100` to the milestone in
   `tasks/99-milestones.tji`. Milestones don't infer completion from their
   dependencies — they need the explicit marker too.
4. **Tech-debt registration.** Any follow-up work the refinement or
   implementation deferred becomes a real WBS leaf (effort, `allocate
   team`, `depends`, `note` citing the source refinement), wired into a
   milestone's `depends` — an unwired task is an orphan, flagged by
   `scripts/unblocked.py`'s ORPHANS audit. Human-judgment items go to
   `tasks/parking-lot.md` instead, never into the WBS.

## Why refinements exist alongside the design docs

The two layers serve different purposes:

- **Design docs** (`docs/design/00…17`) are the **constitution** —
  normative architecture, decided once and amended explicitly (doc 16's
  same-commit rule: a change that alters designed behavior updates the
  governing doc in the same commit; project-shaping decisions also land a
  decision-record bullet in doc 00).
- **Refinements** capture **task scope** — the constraints, prior
  decisions, and open questions that bound a single piece of work. They
  cite design-doc sections as Inputs but they aren't decision records
  themselves; they're work-shaping documents that get a `## Status` block
  on completion.

A refinement may surface a gap that becomes a design-doc delta; the
amended doc then constrains future refinements that depend on it. The two
layers stay separate so each can be read for what it is.
