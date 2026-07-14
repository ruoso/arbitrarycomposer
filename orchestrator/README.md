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
   `clang-format -i` (auto-fixup), then `scripts/gate` (fast host-side
   pre-check), then replays the real per-push CI
   (`.github/workflows/ci.yml`) locally with `act` in docker containers:
   the `lint` job, every `build-test` matrix leg except `msvc-debug` (no
   local Windows container runtime), and the `coverage` job including its
   diff-coverage gate (each output tee'd to
   `logs/iter-NNNN-verify-<step>.log`). `ci.yml` is the single source of
   truth for what runs — the driver only selects jobs/legs, so local
   verification cannot drift from CI. On any failure it dispatches the
   `fixer` sub-agent against the failing log and loops (cap:
   `MAX_FIXER_ATTEMPTS`). Once green, the driver dispatches the `closer`
   sub-agent with the pass-block as `$test_results`. The closer's return is
   what feeds back into the next orchestrator turn.

   The act steps run against a repo-local runner image
   (`arbitrarycomposer/act-runner`, built automatically by the chain from
   `.github/act/runner.Dockerfile`) and a synthesized push event
   (`state/act-event.json`, `before` = pre-closer HEAD) so the coverage
   job's diff-coverage gate measures exactly the uncommitted change under
   verification. A shared ccache docker volume keeps the fresh-container
   builds incremental across iterations. Repo-root `.actrc` carries the
   same image mapping for manual `act -j <job>` runs.

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

### Replaying a step

`--resume <log>` re-runs the step recorded in an `iter-NNNN-<phase>.log`,
**continuing that agent session** (the CLI is invoked with `--resume` /
`exec resume`) so partial work is preserved, then rejoins the normal loop at
the next iteration. The prior log is kept as `.attempt-N`.

```
python3 driver.py --resume logs/iter-0031-implementer.log
python3 driver.py --resume logs/iter-0031-implementer.log --note "you were mid-refactor"
```

`--fresh` replays the same step in a **brand-new session** instead — the step,
its model and its prompt are still recovered from the log and the dispatch
manifest, so the same work is re-dispatched, but with no memory of the recorded
attempt:

```
python3 driver.py --resume logs/iter-0031-implementer.log --fresh
python3 driver.py --resume logs/iter-0031-implementer.log --fresh --note "the previous attempt tried X; don't"
```

Use `--fresh` when the session is the *problem* rather than the victim — it
wedged, thrashed, or talked itself into a bad approach, and resuming would only
carry the mistake forward. Use a plain `--resume` when the session was merely
interrupted and its partial work is worth keeping.

Note what a fresh replay does *not* discard: only the agent's conversation is
thrown away. The recorded prompt is a snapshot of the rendered **template**, and
every substantive input it points at — the refinement, the design docs, the WBS,
the working tree — is re-read from disk by the sub-agent. So a fresh replay picks
those up at their *current* state, which is what makes it the right tool after
you have edited a refinement or the task graph underneath a failing run.

Everything downstream is unchanged, so a fresh replay is a drop-in for a
session-resuming one: same log path and archival, same model resolution, and an
`implementer` replay still runs the post-implementer verify → fixer → closer
chain.

**`--resume` does not re-read `state/context_summary.md`.** The replayed prompt
comes from the log's `---PROMPT---` block, and for an *orchestrator* step the
context summary is inlined into that text rather than pointed at — so a resumed
orchestrator turn re-runs against the summary it originally saw, edits to the
file notwithstanding. To hand the orchestrator a new summary, edit the file and
start a plain run (see `--archive`).

### Starting a fresh run

A plain `python3 driver.py` restarts iteration numbering at 0, which overwrites
`iter-0000-*` onwards in place. `--archive` moves the previous run's logs,
dispatch manifests and act event into a dated directory under `logs/` first:

```
python3 driver.py --archive
```

`state/context_summary.md` is *copied* into the archive but left where it is —
it is the state the new run carries forward. Edit it before starting if you want
the fresh run to begin from different context. `--archive` cannot be combined
with `--resume` (archiving would move the log being replayed).

No dependencies beyond stdlib (plus `tj3`, the C++ toolchain the gate
needs, and `docker` + `act` for the CI-replay verification steps). Stop with Ctrl-C; in-flight sub-agent processes get SIGTERM'd
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
