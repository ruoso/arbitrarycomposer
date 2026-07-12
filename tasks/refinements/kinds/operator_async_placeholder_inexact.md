# kinds.operator_async_placeholder_inexact — audio-facet async-placeholder exactness

**TaskJuggler entry:** `tasks/55-kinds.tji`, `task operator_async_placeholder_inexact`
("Fix audio-facet async-placeholder exact=true bug in fade and crossfade").

**Effort:** 1d.

**Inherited dependencies:** `operators.fade_runtime_binding`,
`operators.crossfade_runtime_binding` (both settled) — the operators are bound to a
live `PullService` on every driver, which is what makes an async pull reachable.

## What this task is

The audio-facet twin of the visual bug fixed under `runtime.worker_dispatch_leaf_only`.
When an operator's input audio pull is dispatched to a worker, the service returns with
the completion unsettled. The operator's descent is synchronous on the frame thread, so
it cannot wait: it mixes silence for that input and answers. It then reported that
silence with `AudioResult::exact == true` — and exactness is what the caller caches on.
The silent block is cached as a *final* answer, the `Exactness::Exact` pass that was
supposed to re-request the input serves the cached silence straight back, and the
dispatched render, once it lands, is never composed. A cold-cache parallel audio render
exports silence, deterministically.

Fix the three operator kinds to report the transient placeholder inexact on the audio
facet, and promote the rule — which existed only as a code comment — into the doc 13
operator contract with a claims-register row enforcing it per kind and per facet.

## Why it needs to be done

It is a wrong-output bug in an advertised facet (`13-effects-as-operators#fade-attenuates-both-facets`,
`#crossfade-mixes-both-facets`), on the parallel path every export takes. It stays in M9
for that reason while the rest of the audio tail moved to M10.

## Inputs / context

- `docs/design/13-effects-as-operators.md` — "The operator contract", specifically the
  "A pull delivers into the caller's target" paragraph (13:105-120), which already says
  an async pull leaves the completion unsettled and that the arrival re-drives the
  operator, but never says the degraded pass must be reported *inexact*.
- `docs/design/12-audio.md:58-61` — `AudioResult { achieved_rate, exact }`, the
  temporal twin of `RenderResult`.
- `src/kind_fade/fade_content.cpp` — `FadeAudioFacet::render_audio`, the deferred-pull
  branch.
- `src/kind_crossfade/crossfade_content.cpp` — `CrossfadeAudioFacet::render_audio`, which
  did not even *track* whether either pull deferred, and discarded both settled inputs'
  `AudioResult`s. Its visual `PullOutcome` struct (the TRANSIENT/FINAL split) is the
  shape to mirror.
- `src/kind_nested/nested_content.cpp` — `mix_child_layer`, whose deferred-pull branch
  returned early without clearing the `exact` out-param. The WBS note called nested
  "already-correct"; that is true of the *visual* descent (`compose_child_layer` returns
  `false`) and false of the audio one.
- `tasks/refinements/runtime/worker_dispatch_leaf_only.md`, Decision 4 — the visual fix
  and the source of this debt.

## Constraints / requirements

1. The TRANSIENT/FINAL split is preserved, not flattened. A pull the service
   **dispatched** (completion unsettled) is transient → inexact. A pull that **failed**
   or exceeded its budget is final → exact. Reporting *everything* inexact would be a
   different bug: an operator that never converges.
2. Crossfade must keep issuing BOTH input pulls on every pass, even when the first one
   deferred — a pull is what dispatches a cold input's render.
3. Levelization (doc 17) unchanged: the kinds stay on `contract` + `surface` + `media`.
   No new dependencies.
4. No golden may change. The fix alters only the `exact`/`achieved_rate` metadata of a
   *degraded* pass; every existing golden renders through settled pulls.

## Acceptance criteria

- A claims-register row, `13-effects-as-operators#transient-placeholder-is-never-exact`,
  enforced by a live test driving each operator kind (fade, crossfade, nested) on each
  facet through a pull double that leaves the completion unsettled: the deferring pass
  reports `exact == false`, the failing pass reports `exact == true`, and crossfade
  still issues both pulls when the first defers.
- The doc 13 delta stating the rule normatively.
- Existing fade / crossfade / nested audio goldens and conformance runs unchanged.

## Decisions

1. **The rule lands in doc 13, not doc 12.** It is facet-symmetric and it is a property
   of the *operator contract* (what a pull's degradation means), not of audio. Doc 13's
   "Audio: identical shape one dimension down" already carries audio semantics for this
   section, and one rule stated once beats two that can drift.
2. **Crossfade folds its inputs' honesty, not just the transient bits.** While adding
   the transient tracking it was necessary to stop discarding the settled inputs'
   `AudioResult`s — the mix reported `exact == true` at the request rate even over an
   input that answered inexactly or below-rate. Folding is the same AND the visual path
   already performs (`r0.exact && r1->exact && !transient`); leaving it would have made
   the new claim false.
3. **Nested's audio descent is fixed here too**, despite the task title naming only fade
   and crossfade. It has the identical defect (the WBS note's premise that nested was
   correct is stale on the audio side), it is the kind the other two cite as the
   reference implementation, and the claim registered here quantifies over all three
   operator kinds — it could not honestly be registered while nested still lied.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-12.

- Promoted the transient-placeholder inexactness rule into the operator contract in `docs/design/13-effects-as-operators.md`, adding normative text that a deferred-pull pass must be reported `exact=false` on both facets.
- Added claim `13-effects-as-operators#transient-placeholder-is-never-exact` to `tests/claims/registry.tsv`, enforcing the rule per operator kind and per facet.
- Fixed `src/kind_fade/fade_content.cpp` (`FadeAudioFacet::render_audio`) to return `exact=false` when the input pull defers to a worker.
- Fixed `src/kind_crossfade/crossfade_content.cpp` (`CrossfadeAudioFacet::render_audio`) to track per-pull transient/final outcome and fold settled inputs' `AudioResult`s (it was discarding them, reporting `exact=true` even over inexact or below-rate inputs).
- Fixed `src/kind_nested/nested_content.cpp` (`mix_child_layer`) to clear the `exact` out-param in the deferred-pull branch (audio-side bug — the visual descent was already correct).
- Added `tests/operator_async_placeholder_inexact.t.cpp` with 7 cases (fade/crossfade/nested × audio + visual, TRANSIENT-vs-FINAL split, crossfade's both-pulls-issued rule, crossfade input-honesty fold); registered in `tests/CMakeLists.txt`.
- All existing fade/crossfade/nested audio goldens and conformance runs pass unchanged (zero golden changes — the fix only affects `exact`/`achieved_rate` metadata of degraded passes).
