# runtime.camera_change_damage — Emit damage when HostViewport camera changes

## TaskJuggler entry

`tasks/65-runtime.tji:254-259`:

> ```
> task camera_change_damage "Emit damage when HostViewport camera changes" {
>   effort 1d
>   allocate team
>   depends !host_viewport_document_binding
>   note "Doc 02:51 lists camera-transform edits as a damage source, but
>   HostViewport::set_camera and reanchor/rebase do not flush a damage
>   notification -- a camera-only step() produces no repaint. The fix: after
>   any camera mutation that changes the device mapping, emit full-viewport
>   device damage through the registered DamageSink so the interactive
>   driver repaints. Surfaced by the host-interactive example (host must
>   call force_repaint() after camera edits; documented workaround in
>   examples/host-interactive/). Source-of-debt:
>   tasks/refinements/packaging/examples.md. Docs 01/02."
> }
> ```

Milestone wiring: `packaging.tag_01` (the v0.1.0 tag, `tasks/75-packaging.tji:42`)
depends on this task, and the v0.1 milestone (`tasks/99-milestones.tji:72`)
lists it — this is one of the three release-blocking correctness gaps the
examples task surfaced.

One scope correction against the `.tji` note, settled here (Decision 1): the
damage is full-viewport and device-side, but it must **not** travel "through
the registered DamageSink" — that seam carries model-space, content-keyed
damage and (under a `DamageRouter`) fans out to *every* viewport, while a
camera edit is strictly per-viewport. The note's phrasing summarizes the
symptom; doc 02 is the normative source, and the doc 02 delta landed with
this refinement (see Inputs) states the mechanism precisely.

## Effort estimate

**1d.** The mechanism is a small delta on two existing seams (the renderer's
first-frame whole-viewport plan and the viewport's idle early-out); the bulk
of the day is the test matrix (camera-pan byte-equality, no-op set_camera,
cache-survival counters, router two-viewport isolation, bootstrap frame),
the amendment of one existing claims row plus three new rows, and deleting
the `force_repaint()` workaround from the host-interactive example with its
README rewrite.

## Inherited dependencies

**Settled:**

- `runtime.host_viewport_document_binding`
  (`tasks/refinements/runtime/host_viewport_document_binding.md`, Done
  2026-07-11) — the `Document&` constructor installs the damage sink and
  settle hook; its acceptance pinned damage-driven repaint end-to-end
  (`src/runtime/t/host_viewport.t.cpp:874-886`). This task extends the same
  `step()` loop.
- `runtime.damage_router` (Done 2026-07-10) — one model slot, N viewports,
  batch-fidelity fan-out. Load-bearing here as a *negative* constraint:
  anything flushed into the model's sink reaches every registered viewport,
  which is exactly why camera damage must not go there (Decision 1).
- `runtime.interactive_pull_wiring` / `runtime.operator_model_damage_routing`
  (both Done 2026-07-11) — established the pattern this task follows: each
  damage class is detected at the seam that owns its state, routed before
  `map_damage_to_device`, and pinned with behavioral counters. Also
  established that `map_damage_to_device` stays a pure L4 function matching
  layer roots only (`damage_planning.cpp:39`) — this task does not touch it.
- `packaging.examples` (`tasks/refinements/packaging/examples.md`, Done
  2026-07-16) — the source of debt; ships the `force_repaint()` workaround
  this task deletes (`examples/host-interactive/main.cpp:136-154`).

**Pending (deliberately not depended on — must stay disjoint):**

- `runtime.placement_damage_maps_to_device` (`tasks/65-runtime.tji:260-264`)
  — the sibling gap. Placement damage is layer-ObjectId-keyed *model*
  damage that the mapper drops; its fix extends the mapping of model
  damage. Camera damage is not model damage at all (Decision 1), so the two
  fixes share no code. This task must not modify `map_damage_to_device`.
- `compositor.bounded_content_tile_clip` — the third example-surfaced gap,
  unrelated (tile clipping, not damage).

## What this task is

Make a camera edit repaint. After `HostViewport::set_camera` changes the
device mapping, the next `step()` must issue a frame whose repaint region is
the full viewport, composited at the new camera — without the host doing
anything else. Concretely:

1. `InteractiveRenderer::render_frame` learns to detect a device-mapping
   delta (camera matrix, anchor id, or device dims) against its own
   previous-frame state and, on delta, plans the whole viewport — the same
   plan shape the first frame already uses (`interactive.cpp:233`, `:404`).
2. `HostViewport::step()`'s idle early-out stops suppressing the *first*
   frame of a freshly constructed viewport, so a viewport bound to a
   document whose commits predate the binding composites the scene on its
   first step (the bootstrap half of the example's workaround).
3. The `force_repaint()` workaround is deleted from
   `examples/host-interactive/` (both call sites and the README paragraph).

It is **not**: a change to `map_damage_to_device` or any model-damage
mapping (that is `placement_damage_maps_to_device`); a new damage payload
type or `DamageSink` protocol change; a cache-invalidation change (camera
damage invalidates nothing — Constraint 3); a viewport-resize seam (no
public mutator for `Viewport::width/height` exists, and none is added); or
any change to the rebase math (`rebase-preserves-composed-appearance` stays
pinned by its golden).

## Why it needs to be done

1. **Doc 02:51-52 promises it and nothing implements it.** Step 1 of the
   interactive frame collects "content damage, placement changes, camera
   changes" — but no mechanism converts a camera delta into a repaint
   region. `step()` already *issues* the frame (the `scene_moved` term,
   `host_viewport.cpp:186-188`), but with an empty damage span the
   renderer's non-first-frame early-out (`interactive.cpp:294`) returns
   having painted nothing: a camera-only step is a frame that repaints
   nothing, and a static scene pans as a frozen image.
2. **Real embedders hit it immediately.** The host-interactive example had
   to ship a documented workaround — remove-and-re-add a transparent
   `SolidContent` after every gesture to synthesize structural damage
   (`examples/host-interactive/main.cpp:136-154`, `README.md:41-45`). An
   embedding API whose first interactive host needs a damage-forging ritual
   is broken at the seam v0.1 exists to prove.
3. **It blocks the v0.1.0 tag.** `packaging.tag_01` depends on this task
   (`tasks/75-packaging.tji:42`); the release-blocking criterion is the
   example shipping workaround-free.

## Inputs / context

Design docs (normative):

- doc 02:51-52 — step 1 lists camera changes among the collected damage.
- doc 02:89-130 — the repaint-region rules (clear first, clip, disjoint)
  and the byte-identity invariant a full-viewport plan must satisfy.
- **doc 02 delta (rides with this refinement)** — "A camera change is
  device damage, and it covers the whole viewport" (after doc 02:87): the
  mapping delta has no model-space key, is detected against the frame's own
  previous-frame state, plans the full viewport exactly as the first frame
  does, repaints without invalidating, and the never-rendered viewport is
  the degenerate case. This paragraph is the anchor for the three new
  claims below.
- doc 01:108-110 — pan/zoom/rotate are camera-transform edits; multiple
  viewports observe one composition (why camera damage is per-viewport).
- doc 01:116-123 — binding a viewport to a document is the host's single
  wiring step (why the bootstrap frame belongs to the viewport, not the
  host).
- doc 04:62-67, :81-84 — rebase rebuilds `(anchor, matrix)` preserving the
  composed mapping; camera position is the pair, not the matrix.

Source seams:

| Seam | Where | Change |
| --- | --- | --- |
| `set_camera` (unconditional assign) | `src/runtime/arbc/runtime/host_viewport.hpp:190-193` | none (Decision 2) |
| `scene_moved` + idle early-out | `src/runtime/host_viewport.cpp:185-192` | gate early-out on `d_rendered_once` (bootstrap frame) |
| rebase application in `step()` | `src/runtime/host_viewport.cpp:144-164` | none (Decision 4) |
| `render_frame` signature | `src/runtime/arbc/runtime/interactive.hpp:271-276` | unchanged — detection is internal (Decision 2) |
| first-frame whole-viewport plan | `src/runtime/interactive.cpp:233`, `:404` (`dirty_ptr = nullptr`) | extend the condition: `first_frame || mapping_changed` |
| non-first-frame early-out | `src/runtime/interactive.cpp:294-298` | must not fire when the mapping changed |
| previous-frame state | `src/runtime/arbc/runtime/interactive.hpp:435` (`d_prev_time`), `:441` (`d_prev_camera_scale`) | add previous `(camera, anchor, width, height)`, updated where `d_prev_time` is (`interactive.cpp:556`) |
| `Viewport` (the mapping) | `src/compositor/arbc/compositor/compositor.hpp:16-31` | field-wise comparison (or defaulted `==`) |
| example workaround | `examples/host-interactive/main.cpp:136-154`, `:159`, `:175-181`; `examples/host-interactive/README.md:41-45` | delete `force_repaint()`, rewrite the KNOWN GAP comment/README |
| idle-viewport claim + test | `tests/claims/registry.tsv` (`02-architecture#idle-viewport-issues-no-frames`); `src/runtime/t/host_viewport.t.cpp:547-558` | amend for the bootstrap frame (Decision 3) |

Predecessor decisions inherited verbatim:

- `operator_model_damage_routing` Decision 5 / `interactive_pull_wiring`
  scope rule: the offline/export driver has no damage stream and is
  untouched.
- `damage_router` Constraint 2: whatever reaches the model's sink is fanned
  to every viewport verbatim — nothing viewport-local may be flushed there.
- `host_viewport_document_binding` Constraint 7 ordering (settle → pin →
  drain) is unchanged; this task adds no step to `step()`.

## Constraints / requirements

1. **Detection lives in the renderer, against its own previous frame.**
   `render_frame` already owns the per-viewport frame-to-frame state
   (`d_prev_time`, `d_prev_camera_scale`, `d_prior_stamps` — the header
   comment at `interactive.hpp:230-238` names this state per-viewport by
   design) and already synthesizes damage from one previous-frame delta
   (clock advance, `interactive.cpp:239`). The camera is the second such
   delta, detected the same way: previous `(camera, anchor, width, height)`
   vs current. No new parameter, no host-side flag (Decision 2).
2. **A mapping delta plans the full viewport — through the existing
   first-frame path.** Reuse the `dirty_ptr = nullptr` whole-viewport plan
   (`interactive.cpp:404`); do not build a synthetic full-viewport
   `DirtyRegion`. The non-first-frame early-out (`interactive.cpp:294`)
   gains the mapping-delta term so a camera-only frame is never a no-op.
   Doc 02:120-130's byte-identity invariant is the correctness bar: the
   resulting target must be byte-identical to a fresh single-pass render at
   the new camera.
3. **Camera damage repaints; it never invalidates.** No content changed, so
   `invalidate_damage` must not see any camera-derived record and no cached
   tile may be evicted. Model-damage routing and invalidation
   (`interactive.cpp:241-260`, step 3) run unchanged and independently when
   real model damage accompanies the camera edit. Pinned by A3's
   pan-away-and-back counter assertion.
4. **Camera damage is per-viewport and never touches the model's sink.**
   Nothing is flushed into `DamageAccumulator`, `Model::set_damage_sink`,
   or `DamageRouter`. A camera edit on viewport 1 must leave a
   router-sharing viewport 2 idle (A5).
5. **`set_camera` no-op stays free.** Setting an identical camera must not
   issue a frame: the renderer's comparison is value-based, and
   `scene_moved` already compares values (`host_viewport.cpp:186-188`), so
   this falls out — but it is asserted (A2), because the still-scene
   guarantees (`02-architecture#idle-viewport-issues-no-frames`,
   `#interactive-still-scene-schedules-no-frame`) are claims-pinned.
6. **The bootstrap frame is the degenerate mapping delta.** The idle
   early-out (`host_viewport.cpp:190-191`) becomes
   `d_rendered_once && damage.empty() && …` so the first `step()` always
   reaches `render_frame`, whose first-frame path already plans the whole
   viewport. This amends one claims row (Decision 3) and the doc 02 delta
   makes it normative.
7. **Rebase stays byte-identical.** The reanchor golden
   (`tests/host_viewport_reanchor_golden.t.cpp:83`) and the doc 04 claims
   stay green: a rebase-driven `(anchor, camera)` rewrite may now trigger a
   full-viewport repaint, but rebase preserves the composed mapping, so the
   repaint is pixel-identical — and a rebase only ever fires mid-zoom, when
   the camera edit itself already forces the repaint (Decision 4).
8. **Levelization.** Changes confine to L5 `arbc::runtime`
   (`host_viewport.cpp`, `interactive.cpp`/`.hpp`) plus possibly a
   defaulted `operator==` on the L3 `compositor::Viewport` aggregate. No
   new component edges; `scripts/check_levels.py` stays silent.
9. **Concurrency.** `set_camera` and `step()` are host-thread-confined
   (single-owner, drained on the driving thread —
   `host_viewport.hpp:238-256`); the new previous-mapping state is
   renderer-private and frame-serial. No new TSan/stress obligation.

## Acceptance criteria

- **A1 — camera pan repaints, byte-identical.** In
  `src/runtime/t/host_viewport.t.cpp`: render a still multi-layer scene at
  camera A, `set_camera(B)` (pure translation and, separately, a zoom),
  `step()`; the persistent target is byte-identical to a fresh viewport
  rendered directly at B. `frames_issued()` delta is exactly 1.
  `// enforces: 02-architecture#camera-change-repaints-full-viewport`
  (new claims row, anchored in the doc 02 delta).
- **A2 — no-op set_camera stays free.** `set_camera` with the identical
  matrix followed by repeated `step()`s: `frames_issued()` delta 0. Second
  tag on the amended `02-architecture#idle-viewport-issues-no-frames` row.
- **A3 — camera damage evicts nothing.** Pan A→B→A over a still scene
  (cache ample): the third frame re-plans entirely from cache — compositor
  behavioral counters show a `requests_issued` delta of 0 for that frame
  (renders happened for B; returning to A is all hits). Never a wall-clock
  assertion.
  `// enforces: 02-architecture#camera-change-does-not-invalidate`
  (new claims row).
- **A4 — bootstrap frame.** Commit a scene, *then* construct the
  document-bound viewport (commits predate the sink install), `step()`
  once: `frames_issued() == 1` and the target matches a full render;
  subsequent still steps stay at 1.
  `// enforces: 02-architecture#first-step-composites-bound-scene`
  (new claims row). The existing idle test
  (`host_viewport.t.cpp:547-558`) and the
  `02-architecture#idle-viewport-issues-no-frames` row are amended to
  measure idleness *after* the bootstrap frame (Decision 3).
- **A5 — per-viewport isolation.** Two viewports on one `DamageRouter`:
  `set_camera` on viewport 1 only; viewport 1 repaints, viewport 2's
  `frames_issued()` delta is 0. Second tag on
  `01-core-concepts#multiple-viewports-observe-one-composition`.
- **A6 — renderer-level delta detection.** In
  `src/runtime/t/interactive.t.cpp`: `render_frame` called twice with the
  same state and empty damage but a changed `Viewport` composites the full
  viewport on the second call; with an unchanged `Viewport` it plans
  nothing (protects `02-architecture#interactive-still-scene-schedules-no-frame`,
  second tag).
- **A7 — example workaround deleted.** `force_repaint()` and the KNOWN GAP
  comment removed from `examples/host-interactive/main.cpp` (all three
  uses: bootstrap `:159`, per-gesture `:175-181`, the lambda itself);
  `README.md:41-45` rewritten to state that camera edits repaint on the
  next `step()`. The example's CI-validated PNGs are byte-identical (the
  workaround was a pixel-exact no-op, and the bootstrap frame replaces the
  forged first-frame damage).
- **A8 — rebase goldens stay green.** `host_viewport_reanchor_golden.t.cpp`
  and the doc 04 claims (`rebase-preserves-composed-appearance`,
  `zoom-out-reanchors-along-anchor-path`) pass unchanged.
- **Gates.** `scripts/gate` green; `scripts/check_levels.py` and the
  claims-register audit silent; ≥90% diff coverage on changed lines;
  `-Werror`/clang-format clean.
- **Deferred follow-ups: (none.)** The sibling gaps are already WBS leaves
  (`runtime.placement_damage_maps_to_device`,
  `compositor.bounded_content_tile_clip`); no viewport-resize seam exists
  to owe damage for.

## Decisions

**Decision 1 — Camera damage is device-side and per-viewport; it never
travels through the model's `DamageSink`, deviating from the `.tji` note's
literal phrasing.** The sink protocol carries model-space
`Damage{ObjectId, Rect, TimeRange}` (`damage.hpp:20-26`); a camera edit has
no model-space key — `map_damage_to_device` matches layer roots only, so a
synthetic `Damage{ObjectId{}}` maps to nothing. The doc 02 delta states the
mechanism this task implements: the mapping delta is detected device-side
and plans the full viewport.
*Rejected: synthesizing per-visible-layer structural model damage and
flushing it through the sink (the example's workaround, mechanized).* Three
independent failures: (a) it feeds `invalidate_damage`, evicting cached
tiles of unchanged content — defeating A3 and the cache-survival claims;
(b) under a `DamageRouter` the model sink fans the flush to every viewport,
repainting siblings whose cameras did not change — violating doc 01:108-110's
free multi-viewport promise; (c) it launders a device-space event through a
model-space protocol, forcing the mapper to undo the disguise.

**Decision 2 — The renderer detects the delta against its own previous
frame; no `render_frame` signature change, no `set_camera` dirty flag.**
The renderer already owns exactly this shape of state and precedent:
`d_prev_time` turns clock advances into damage (`interactive.cpp:239`), and
`d_prev_camera_scale` (`interactive.hpp:441`) already tracks one camera
component frame-to-frame. Adding previous `(camera, anchor, width, height)`
beside them makes camera damage the exact structural twin of clock-advance
damage, and every `render_frame` caller (including direct renderer tests)
gets the behavior with zero plumbing.
*Rejected: a `d_camera_dirty` flag set by `set_camera` and passed into
`render_frame`.* It splits one fact ("the mapping changed since the frame I
composited") across two objects, misses direct-renderer callers, requires a
signature change or a new parameter for no gain, and must separately handle
construction (bootstrap) — which the renderer's first-frame path already
handles as the degenerate case of "no previous mapping".
*Rejected: comparing composed mappings across anchor changes (folding the
anchor-path edges) to skip repaints after a rebase.* Requires matrix
inversion round-trips whose float equality is fragile, to optimize a case
(rebase on a still camera) that cannot occur — see Decision 4.

**Decision 3 — The bootstrap frame is in scope, and the
`idle-viewport-issues-no-frames` claim is amended, not weakened.** The
`.tji` names the camera mutation, but the example needed `force_repaint()`
for the first frame too (`main.cpp:159` — commits predated the sink
install), and the fix is one term: gate the idle early-out on
`d_rendered_once`. Doc 01:116-123 makes binding "the host's single wiring
step"; a bound viewport that shows nothing until the next edit betrays
that. The claims row and test are amended to assert idleness *after* the
first composite — the claim's substance ("a still scene costs nothing") is
untouched; a never-rendered viewport is not still, it is stale. The doc 02
delta makes the first-frame sentence normative, per doc 16's same-commit
rule.
*Rejected: deferring bootstrap to a follow-up task.* It would leave
`force_repaint()` in the example (A7 impossible), and the deferred task
would be a one-line condition plus one amended test — below the granularity
of a WBS leaf.

**Decision 4 — Rebase-driven `(anchor, camera)` rewrites may trigger the
full-viewport repaint; no suppression is added.** Rebase preserves the
composed mapping (doc 04:62-67, golden-pinned), so the repaint is
byte-identical — correct, merely redundant. And it is unreachable waste: a
rebase fires only when the camera's scale left the band, i.e. only on a
frame whose camera edit already forces the full repaint. Suppressing it
would buy nothing and cost the fragile composed-mapping comparison rejected
in Decision 2.

**Decision 5 — Claims anchor in doc 02, beside the frame rules.** The three
new rows (`camera-change-repaints-full-viewport`,
`camera-change-does-not-invalidate`, `first-step-composites-bound-scene`)
anchor in the doc 02 delta paragraph, next to
`damage-maps-to-device-dirty-regions` and the repaint-region claims —
camera damage is a frame-plan fact, not a binding fact, so doc 01 (where
`host_viewport_document_binding` Decision 4 anchored the binding claim) is
the wrong home. Existing rows are amended (`idle-viewport-issues-no-frames`)
or second-tagged (`multiple-viewports-observe-one-composition`,
`interactive-still-scene-schedules-no-frame`) rather than duplicated, per
`interactive_pull_wiring` Decision 5.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-17.

- Camera-mapping delta detection added to `InteractiveRenderer::render_frame` via a stored previous `(camera, anchor, width, height)` in `src/runtime/arbc/runtime/interactive.hpp`; on delta, the full-viewport first-frame plan path (`interactive.cpp`) is re-entered without invalidating any cached tiles (`src/runtime/interactive.cpp`).
- `HostViewport::step()` idle early-out gated on `d_rendered_once` so the bootstrap frame always reaches the renderer (`src/runtime/host_viewport.cpp`, `src/runtime/arbc/runtime/host_viewport.hpp`).
- `Viewport::operator==` defaulted on `src/compositor/arbc/compositor/compositor.hpp` to enable renderer-side mapping comparison.
- `force_repaint()` workaround deleted from `examples/host-interactive/main.cpp` and `examples/host-interactive/README.md`.
- Unit tests A1–A6 added: camera-pan/zoom byte-identity (A1), bootstrap frame (A4), pan-away-and-back cache-hit counter (A3), two-viewport router isolation (A5), renderer-level delta detection (A6) in `src/runtime/t/host_viewport.t.cpp` and `src/runtime/t/interactive.t.cpp`.
- 3 new claims rows added and `idle-viewport-issues-no-frames` amended in `tests/claims/registry.tsv`; bootstrap-sensitive counter assertions updated in `tests/async_external_load.t.cpp` and `src/runtime/t/host_viewport.t.cpp`.
- Pre-existing pan-step assertion in `tests/per_object_revision_keys.t.cpp` amended by fixer: `requests_issued` delta corrected from `1` to `4` (full 2×2 grid planned correctly now that the mapping delta drives a full-viewport plan).
- `docs/design/02-architecture.md` delta (normative camera-damage paragraph) staged alongside implementation.
