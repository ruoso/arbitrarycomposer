# orchestrator/

Python driver that loops a planning agent session ("the orchestrator") with
a sequence of working agent sessions ("sub-agents"). Ported from
a-conversa's orchestrator; adapted to this repo's gate-based verification.

Each iteration is at least two agent CLI invocations, plus — after every
`implementer` dispatch — a deterministic verification + fixer + closer tail
that the driver owns:

1. **Orchestrator turn** — system prompt at `prompts/orchestrator_system.md`,
   plus the carried-over `context_summary` and the previous sub-agent's
   output. Final assistant message must be a JSON envelope:
   `{"next": {"template": "refinement_writer"|"implementer", "vars": {...}}, "context_summary": "..."}`
   or `{"stop": "<reason>"}`.
2. **Sub-agent turn** — `prompts/<template>.md` rendered with `vars`, run as
   a fresh top-level agent session. Exception: a `refinement_writer`
   dispatch whose `refinement_path` already exists (non-empty) on disk is
   skipped by the driver — the orchestrator gets a `(driver notice)` return
   telling it to dispatch the implementer next, and no sub-agent runs.
3. **(implementer only) deterministic tail** — the driver runs
   `clang-format -i` (auto-fixup), then `scripts/gate`, then
   `ARBC_GATE_PRESET=asan scripts/gate` (each output tee'd to
   `logs/iter-NNNN-verify-<step>.log`). On any failure it dispatches the
   `fixer` sub-agent against the failing log and loops (cap:
   `MAX_FIXER_ATTEMPTS`). Once green, the driver dispatches the `closer`
   sub-agent with the pass-block as `$test_results`. The closer's return is
   what feeds back into the next orchestrator turn.

All invocations stream JSON so the driver prints live event summaries as
they arrive. Full event streams are tee'd to `logs/iter-NNNN-<phase>.log`.

## Persistent state

The orchestrator's `context_summary` field is persisted to
`state/context_summary.md` after every orchestrator turn and re-loaded on
the next driver startup, so the loop is resumable across runs (Ctrl-C,
crashes, manual stops). The contents of `state/` and `logs/` are gitignored.

If the verification chain exhausts `MAX_FIXER_ATTEMPTS`, the driver appends
a `## Verification chain exhausted at iter <N>` block to
`state/context_summary.md` and exits non-zero. The next driver run picks
that context back up and the orchestrator sees the failure block on its
first turn — its prompt directs it to `stop` with a `corrupted: ...` reason
so the human user can intervene.

## Running

```
cd orchestrator
python3 driver.py
```

The default CLI is Claude Code. Use Codex CLI with:

```
AGENT_CLI=codex python3 driver.py
```

No dependencies beyond stdlib (plus `tj3` and the C++ toolchain the gate
needs). Stop with Ctrl-C; in-flight sub-agent processes get SIGTERM'd
cleanly. Prompt files are loaded immediately before each dispatch, so edits
under `prompts/` take effect on the next agent turn without restarting the
driver.

Before every orchestrator turn, the driver also injects persisted
`orchestrator/state/context_summary.md` snapshots from all registered git
worktrees. The orchestrator uses those snapshots to avoid assigning sibling
worktrees overlapping tasks or workstreams.

## Models

Per-template model selection lives in the selected `AgentCli` adapter;
override via env vars (`ORCH_MODEL`, `SUB_MODEL`, `CLOSER_MODEL`) when you
want to experiment.

## Prompts layout

- `prompts/orchestrator_system.md` — orchestrator system prompt (mission,
  read-only rule, pick heuristics, JSON envelope contract).
- `prompts/refinement_writer.md` — refinement-writer sub-agent brief.
  Vars: `$task_id`, `$refinement_path`.
- `prompts/implementer.md` — implementer sub-agent brief.
  Vars: `$refinement_path` (plus `$task_id` for log labelling).
- `prompts/closer.md` — closer sub-agent brief (driver-internal).
  Vars: `$task_id`, `$refinement_path`, `$implementer_summary`,
  `$test_results`.
- `prompts/fixer.md` — fixer sub-agent brief (driver-internal). Vars:
  `$task_id`, `$refinement_path`, `$implementer_summary`, `$failing_step`,
  `$failing_command`, `$failing_log`, `$prior_attempts`.

Vars are substituted via `string.Template.safe_substitute`, so `$var` is
the substitution syntax — escape literal dollar signs as `$$` (e.g. the
`$$(cat <<'EOF' ...)` HEREDOC in `closer.md`).

Cross-cutting policies (design-docs-as-constitution, claims-register
growth, conformance-suite coverage, tech-debt registration, test-output
handling, "what sub-agents must NOT do") are embedded into each template
that needs them, since sub-agents are fresh sessions with no shared state.

## Permissions

For Claude, the driver passes no permission flags to `claude -p`. Whatever
`~/.claude/settings.json` provides as the default is what the sub-agents
get. If headless runs block on tool prompts, add e.g.
`--permission-mode acceptAll` in `ClaudeCli.command()`.

For Codex, the adapter runs
`codex -a never exec --sandbox workspace-write`.
