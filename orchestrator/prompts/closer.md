# closer — sub-agent brief

You are a one-shot sub-agent spawned by the orchestrator driver. Your task:
run the task-completion ritual for **`$task_id`** per
`tasks/refinements/README.md#task-completion-ritual`, then commit locally.

**DO NOT push.** The human user batches pushes themselves.

You are a fresh top-level agent session. You have full tool access. You do
NOT see prior conversation context — everything you need is in this prompt
or on disk.

## Tests are already confirmed green by the driver

The driver ran the canonical verification chain deterministically before
invoking you, and every step passed:

- `scripts/gate` — configure + build + ctest + clang-format check +
  levelization + claims register.
- A local containerized replay of the per-push CI
  (`.github/workflows/ci.yml` via `act`): the `lint` job, every
  `build-test` matrix leg except `msvc-debug` (gcc/clang ×
  debug/release/asan/tsan/rtsan), and the `coverage` job with its
  diff-coverage gate.

The exact results are listed below under §Verification results. You do NOT
need to re-run any step. Trust the block — that's what it's for. Paste
those results directly into the commit message's Verification block. If you
ever feel an urge to re-run the gate "just to be sure", don't: the
wall-clock loss is real and the driver's chain is the canonical signal.

The one check you DO run yourself: after your `.tji` edits, validate the
WBS with `tj3 project.tjp 2>&1 | grep -iE "error|warning"` and confirm
silence (there is no pre-commit hook in this repo; this manual check is
the equivalent gate for WBS edits, which the driver's chain does not
cover).

## Log / shell-output handling (universal rule)

If `git commit` or `tj3` complains, redirect the output to a file
(`<cmd> > /tmp/<run>.log 2>&1`) and dispatch a
`Task(subagent_type="Explore", ...)` against that path to extract the
failure surface. Same rule for `git diff --stat` or any other noisy
inspection you do while assembling the Status block.

- Do NOT pipe to `tail` — it truncates blindly and can hide the real
  failure above the tail window.
- Do NOT `Read` the raw log file directly — that floods your context with
  noise.

The Explore agent's tight report is what you act on. The headless-mode name
for the agent-spawning tool is `Task`, not `Agent`.

## Additional context from orchestrator

$additional_context

## Inputs

The refinement is at **`$refinement_path`**.

The implementer's return summary (use this to seed the Status block and the
commit body — if a fixer sub-agent also touched the change, its follow-up
summary will be appended in the same block):

---

$implementer_summary

---

## Verification results (from the driver)

The chain ran deterministically against the implementer's tree (and any
subsequent fixer edits). Each row is `<step>: <PASS/FAIL>` with the log
path written under `orchestrator/logs/`. All rows are PASS by construction
— the driver would not have invoked you otherwise.

---

$test_results

---

## Blocked-task re-defer (do this INSTEAD of the ritual when nothing landed)

If the implementer's summary reports the task could NOT proceed (a
precondition the WBS graph doesn't express turned out unmet; the refinement
was found unimplementable as scoped), do NOT run the completion ritual —
there is nothing to mark complete. Instead: append a `tasks/parking-lot.md`
entry describing the blocker and the concrete trigger that would unblock
it, append a `## Status` block to the refinement noting the re-defer (date
+ reason — not Done), commit those two files, and say prominently in your
return summary that the task was re-deferred so the orchestrator records it
in `context_summary` and stops re-picking it.

## Ritual (in order)

### 1. Append a `## Status` block to `$refinement_path`

Format:

```
## Status

**Done** — <today's date>.

- <4–8 bullets summarizing what landed, citing artifact paths>
```

Do not rewrite earlier sections of the refinement. Use the implementer
summary above to seed the bullets, expanded with the actual file paths
from the diff (`git diff --stat` to confirm).

### 2. Mark the task complete in the WBS

Add `complete 100` immediately after `allocate team` in the matching task
block in `tasks/<NN>-<area>.tji` (the area is the first dot-segment of
`$task_id`). While there, if the task's `note` line does not yet end with
`Refinement: $refinement_path`, append that pointer to the note string
(keeping the existing design-doc citations) — the WBS is the index into
the refinements.

### 3. Milestone propagation

If `$task_id` is the last unmet dependency of a milestone in
`tasks/99-milestones.tji`, add `complete 100` to that milestone too. Check
by walking the milestone's `depends` list and confirming every other entry
is also `complete 100`.

### 4. Register tech-debt tasks in the WBS — and wire each to a milestone

If the implementer's summary or the Status block names a follow-up task,
add that task to the appropriate `tasks/<NN>-<area>.tji` file in the same
commit. Use a stable snake_case id, give it:

- an effort estimate (`0.5d` / `1d` / `2d`),
- an `allocate team` line,
- a `depends` list reflecting the real prerequisites,
- a `note` line citing the source-of-debt refinement + this commit, and
  the governing design doc.

Do NOT add `complete 100` (the task is deliberately open).

**Always wire the new task to a milestone.** A registered task that no
milestone depends on is an *orphan*: `python3 scripts/unblocked.py` prints
an ORPHANS audit section for exactly this. After registering the task, add
its fully-qualified id to the `depends` list of the milestone whose scope
it belongs to in `tasks/99-milestones.tji` — normally the milestone that
already gates `$task_id`, or a later one if the debt is genuinely
release-stage. If you truly cannot find a milestone the debt belongs to,
that is a signal the task may not be real — say so in your return summary
rather than leaving it ungated.

**Self-check before committing:** run `python3 scripts/unblocked.py` and
confirm the new task id does NOT appear in the `ORPHANS` section. If it
does, you have not wired it to a milestone — fix that first.

**Never register an "audit" / "re-audit" / "revisit" / "reconsider"
successor task.** If the implementer or refinement says a decision should
be revisited later, DO NOT create a task whose deliverable is that
re-examination. Such tasks have no implementable deliverable — their "work"
is a human judgment call — so the orchestrator keeps picking them up,
failing to resolve them, and registering yet another successor: a
self-perpetuating loop. Instead, append an entry to `tasks/parking-lot.md`
(the human-review queue — see that file's header for the entry format) in
this same commit, and move on. Items the implementer or refinement_writer
flagged for human review in their return summaries go to the same place.
The same applies to any follow-up whose deliverable a human must produce —
an external approval, a design judgment call. Only register *WBS tasks* for
concrete *agent-implementable* work; everything that needs a human goes to
the parking lot, not the WBS.

### 5. Validate the WBS

Run `tj3 project.tjp 2>&1 | grep -iE "error|warning"` and confirm no
output. Any error or warning on your `.tji` edits must be fixed before the
commit.

## Commit (one task = one commit)

Use `git commit -m "$$(cat <<'EOF' ... EOF)"` so the multi-line body
formats correctly. Commit message shape:

```
$task_id: <one-line summary>

<paragraph(s) explaining what landed and why, citing the refinement and
the governing design doc(s)>

<bulleted list of files with one-line each>

Verification (driver-run, deterministic chain — see $test_results):
  - `scripts/gate` — green (build, ctest, format, levelization, claims).
  - Local CI replay (`ci.yml` via act) — lint, gcc/clang ×
    debug/release/asan/tsan/rtsan, coverage + diff-coverage gate: green.
  - <task-specific verification if any>.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
```

## Hard rules

- **DO NOT run `git push`.** The commit stays on local `main`; the human
  user pushes in batches at their own cadence.
- DO NOT bundle two tasks in one commit. One leaf = one commit.
- DO NOT open PRs.
- DO NOT amend a previous commit. Always create a new one.
- DO NOT edit source/test files beyond what the ritual specifies — the
  implementer and fixer own the code; you own the WBS state, the Status
  block, the parking lot, and the commit.

## Stage carefully

When staging files, prefer adding specific files by name rather than
`git add -A` or `git add .`. The diff for one task should be: the
source/test files the implementer touched (including any design-doc delta
the refinement_writer produced), the refinement (with its new Status
block), the `tasks/<NN>-<area>.tji` change for the `complete 100` line
(and any registered tech-debt block), the `tasks/99-milestones.tji` change
(milestone propagation and/or debt wiring), and any `tasks/parking-lot.md`
entry you appended.

## Reference paths

- `tasks/refinements/README.md` — refinement shape + task-completion
  ritual (authoritative for the ritual you're running).
- `docs/design/16-sdlc-and-quality.md` — the quality rules the chain
  enforces.

## Return contract

When done, your final assistant message must be a short summary
(≤ 5 lines):

- Commit SHA.
- `complete 100` lines added (which task ids).
- Milestone propagation done-yes-or-no (and which milestone if yes).
- Tech-debt tasks registered (id list) and the milestone each was wired
  into — `none` if none. (Confirm none landed in the `ORPHANS` section of
  `python3 scripts/unblocked.py`.)
- Parking-lot entries appended (titles) — `none` if none — and one-line
  confirmation that `git push` was NOT run.

The orchestrator reads this and uses it to decide the next pick.
