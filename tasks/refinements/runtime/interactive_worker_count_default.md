# runtime.interactive_worker_count_default — Choose and ship a non-zero default worker count for InteractiveRenderer

## TaskJuggler entry

[`tasks/65-runtime.tji:153-158`](../../65-runtime.tji):

```
task interactive_worker_count_default "Choose and ship a non-zero default worker count for InteractiveRenderer" {
  effort 2d
  allocate team
  depends !worker_dispatch_leaf_only, compositor.refine_frame_composite_idempotence
  note "Choose and ship a non-zero default WorkerPoolConfig::worker_count for
        InteractiveRenderer, backed by a behavioral-counter benchmark over
        representative scenes (leaf-heavy, operator-heavy, nested-deep): measure
        renders-per-frame and deadline-miss counts at worker counts 0/1/2/4/
        hardware_concurrency-1 and pick the default from the data, rather than the
        current 0 (inline). runtime.worker_dispatch_leaf_only made worker_count > 0
        correct and TSan-clean but deliberately left the default at 0 — there is no
        measurement behind any other number. Never a wall-clock assertion; the
        benchmark reports counters and the default is a reviewed choice.
        Source-of-debt: tasks/refinements/runtime/worker_dispatch_leaf_only.md
        'Not this task'. Docs 02/16."
}
```

## Effort estimate

**2d.** One day for the benchmark harness and scene workloads; one day for
`default_interactive_worker_count()` / `default_interactive_pool_config()`,
the three behavioral counters (`frames_rendered`, `deadline_expiries`,
`tiles_cancelled`), and the behavioral property test + claims entry.

## What this task is

`runtime.worker_dispatch_leaf_only` made `worker_count > 0` correct and
TSan-clean — it enforces the leaf-only invariant so operator contents always
render inline. This task picks the actual default by benchmarking, rather
than leaving `worker_count = 0` (inline-only) as the indefinite placeholder.

The deliverable is:
- `default_interactive_worker_count()` / `default_interactive_pool_config()`
  helpers that return `min(hardware_concurrency - 1, k_max_interactive_workers)`.
- Three behavioral counters on `InteractiveRenderer` (`frames_rendered`,
  `deadline_expiries`, `tiles_cancelled`) exposed through a `Counters` struct.
- `tests/interactive_worker_default.t.cpp`: A1 property test (inline render
  cannot degrade; default render enforces deadline) and A3 claim entry.
- Google Benchmark suite over representative scenes.

## Blocker discovered during implementation

**`compositor.refine_frame_composite_idempotence`** — a damage-gated refine
frame re-composites onto the caller-persisted target without clearing first,
so source-over lands twice. Observed: 2/60 runs with `worker_count=1` and
CPU oversubscription on a nested semi-transparent scene produce `composites=6`
where the single-pass oracle does `composites=3`. The loop quiesces cleanly
(`schedule_follow_up=false`, pending empty, zero degraded composites, nothing
cancelled) — it converges on wrong pixels silently.

At `worker_count == 0` every miss settles inline inside the frame so the
refine pass essentially never fires and the bug never manifests. A non-zero
default is precisely what makes refine frames the normal interactive path —
so the flip converts a latent bug into routine behavior.

The A1 golden (through the real `HostViewport`) and the A5 sweep both caught
it; the existing `worker_dispatch_leaf_only.t.cpp` byte-identity case does
not because its scene shape never triggers the double-composite.

## Status

**Re-deferred** — 2026-07-12.

- Nothing landed; working tree was restored to HEAD by the implementer.
- Implementation is blocked by `compositor.refine_frame_composite_idempotence`
  (see bug description above and `tasks/parking-lot.md` 2026-07-12 entry).
- Work preserved in `git stash@{0}` (2265 insertions, 17 files): helpers,
  counters, `tests/interactive_worker_default.t.cpp`, Google Benchmark suite +
  shared workloads + bench-smoke, doc 02/00 deltas, new claim id, and a
  `HostViewport::step()` idle-check fix (ignored renders-in-flight).
- `compositor.refine_frame_composite_idempotence` added as explicit `depends`
  in the WBS; `compositor.in_flight_tile_dedup` registered as a follow-up
  optimization (waste, not correctness).
- Re-dispatch this task once `compositor.refine_frame_composite_idempotence`
  is `complete 100`; the stash should apply cleanly.
