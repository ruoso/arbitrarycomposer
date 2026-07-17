# runtime.placement_damage_maps_to_device — Map layer-keyed placement damage to device damage

## TaskJuggler entry

`tasks/65-runtime.tji:261-265` — task `placement_damage_maps_to_device` under
the `runtime` stream. Blocks `packaging.tag_01` (`tasks/75-packaging.tji:42`)
and the `m9_release` milestone (`tasks/99-milestones.tji:72`).

## Effort estimate

1d.

## Inherited dependencies

- `runtime.damage_router` — **settled** (Done 2026-07-10,
  `tasks/refinements/runtime/damage_router.md`). The router fans the model's
  flushed batch verbatim to every registered viewport; whatever this task
  makes mappable repaints *every* viewport displaying the edited node, so the
  per-viewport isolation cases below are load-bearing.

Settled context this task builds on (not formal `depends`):

- `runtime.camera_change_damage` — Done 2026-07-17. Its Decision 1 scoped the
  *camera* class as device-side (no model-space key) and explicitly left
  `map_damage_to_device` untouched so **this** task owns the mapper
  extension; its "repaints, never invalidates" rule is the template for
  placement damage. Its refinement's Inherited-deps section names this task
  as the pending sibling.
- `runtime.operator_model_damage_routing` — Done 2026-07-11. Settled that
  routing lives in the L5 driver and that `route_model_damage` copies the
  original records through (`src/runtime/interactive.cpp:175`), so
  layer-keyed records already reach both consumers of the routed set — they
  just match nothing today.
- `compositor.damage_planning` (`tasks/refinements/compositor/damage_planning.md`)
  — the mapper's own refinement: pure L4 function, no resolver in the
  signature, `Damage.object` read as the content id throughout.

## What this task is

Placement mutators key their auto-emitted damage with the **edited layer's
own ObjectId** — `Model::Transaction::set_transform` flushes
`Damage{layer, Rect::infinite(), TimeRange::all()}` (`src/model/model.cpp:1086`;
likewise span `:1119`, time_map `:1152`, opacity/visible/gain/audible
`:1185/:1218/:1255/:1292`) — and membership mutators key the **composition
id** (`:1020/:1053/:1462/:1503/:1536`). But `map_damage_to_device`
(`src/compositor/damage_planning.cpp:82-118`) matches `d.object` against
`layer.content` only (`:100`) — the *content* id — so structural damage
naming a layer or a composition maps to **zero device rects**: the commit
wakes a `step()` (the drained batch is non-empty,
`src/runtime/host_viewport.cpp:193`), but `render_frame` maps it to an empty
dirty region and hits the no-work early-out
(`src/runtime/interactive.cpp:308`). A `set_layer_transform` never repaints.

The fix: extend `map_damage_to_device`'s matching from "the visited leaf's
content id" to **every ObjectId on the displayed tree's walk path** — the
leaf layer's own id, each descended group layer's id, each descended child
composition's id, and the anchor composition's id — so that structural
damage naming any displayed node maps (conservatively, via the existing
infinite-rect rule at `damage_planning.cpp:104-108`) to the full viewport,
and structural damage naming a node this viewport does not display maps to
nothing.

## Why it needs to be done

- Doc 01:147-151 promises it: "Placement changes (transform, opacity, order)
  generate damage in the parent automatically. Damage propagates … to every
  viewport observing an affected composition; each viewport maps it to a
  dirty device region and schedules re-rendering." Doc 02:51-52 lists
  "placement changes" as a frame damage source, and doc 02:90-91 classifies
  placement damage as model-space that "name[s] an object and map[s] through
  the camera to device rects." The model half is shipped and claimed
  (`01-core-concepts#placement-change-auto-damages`, registry row 29;
  `14-data-model-and-editing#membership-edit-damages-composition`, row 22);
  the mapping half silently drops it. This is a conformance fix, not a
  redesign.
- It is the last damage-source gap of the release trio surfaced by the
  host-interactive example (source-of-debt:
  `tasks/refinements/packaging/examples.md`, Status): camera edits shipped in
  `runtime.camera_change_damage`, placement edits are this task,
  `compositor.bounded_content_tile_clip` is independent. `packaging.tag_01`
  (the v0.1.0 tag) depends on all three.
- Test evidence of the hole: `tests/interactive_model_damage_routing.t.cpp:8`
  documents "`map_damage_to_device` matches damage against LAYER ROOTS only";
  `tests/per_object_revision_keys.t.cpp:341-370` must hand-forge
  content-keyed damage after a `set_layer_transform` to make the frame
  re-plan at all.

## Inputs / context

Code seams:

- `src/compositor/damage_planning.cpp:82-118` — `map_damage_to_device`; the
  match predicate at `:100`
  (`!(layer.content == d.object) || !layer.visible() || layer.opacity <= 0.0`),
  the structural-infinite branch at `:104-108` (saturates to the viewport
  rect; "this signature carries no resolver to tighten to `bounds()`").
  Contract comment `src/compositor/arbc/compositor/damage_planning.hpp:32-34`
  ("`Damage.object` is read as the *content* id throughout") and `:150-164`
  (function contract + signature) must be rewritten to the widened domain.
- `src/compositor/anchored_viewports.cpp:40-64` — `walk_composition`: the
  layer's own ObjectId (`member`) is in hand at `:43`
  (`for_each_layer_in` callback) but the leaf visitor (`:63`,
  `visit(*layer, composed)`) does not receive it; the group-layer descent
  branch is `:49-58` (checks `find_composition(layer->content)`, applies
  visibility/opacity and `subtree_culled` pruning, then recurses). `cull_walk`
  is `:141-154`; the other visitor call sites are `render_frame_anchored`
  (`:162-164`), `clock_advance_damage`
  (`src/compositor/damage_planning.cpp:126`), and tests
  (`src/compositor/t/anchored_viewports.t.cpp:225,:300`).
- `src/model/arbc/model/records.hpp:68-92` — `LayerRecord` carries no self
  id; the id is the DocState HAMT key, so it exists only at the walk site.
  `DocRoot::find_layer` (`src/model/arbc/model/model.hpp:53`) is the
  canonical layer-id lookup if ever needed; the chosen design does not need
  it (Decision 1).
- `src/model/model.cpp:1086` etc. — placement/membership damage keying (see
  "What this task is"); single flush per commit at `:1689-1692`.
- `src/runtime/interactive.cpp` — the consumer: `route_model_damage`
  (`:173-215`, originals copied through at `:175`; a layer id resolves to
  `nullptr` through the `ContentResolver` and routes nowhere — correctly),
  the two `map_damage_to_device` calls (`:292-296`), the no-work early-out
  (`:308`), `invalidate_damage(cache, content_damage)` (`:327` — matches
  `TileKey.content`, i.e. content ids, so layer-keyed records evict nothing),
  and the null-`dirty_ptr` full-viewport plan path (`:417-421`).
- `src/model/arbc/model/damage.hpp:20-26` — `Damage{object, rect, range}`;
  no kind discriminant: structural ⇔ `Rect::infinite()` + `TimeRange::all()`
  by convention (claim rows 33/34).

Design docs (normative):

- `docs/design/01-core-concepts.md:147-151` — placement changes auto-damage;
  every observing viewport maps to a dirty device region and re-renders.
- `docs/design/02-architecture.md:51-52` (damage sources), `:89-96` (camera
  damage is device-side — the contrast class; "repaints … does not
  invalidate: no content changed, so cached tiles stay resident"), `:107-136`
  (repaint-region semantics the mapped rects feed).
- `docs/design/04-transforms-and-infinite-zoom.md:70-72` — culling walks
  top-down from the anchor composing viewport-relative transforms; the walk
  this task extends.
- `docs/design/14-data-model-and-editing.md:111-112,:127-129` — placement
  mutators all auto-damage; damage rides the transaction, one flush per
  commit.

Predecessor refinements: `tasks/refinements/runtime/damage_router.md`,
`tasks/refinements/runtime/camera_change_damage.md`,
`tasks/refinements/runtime/operator_model_damage_routing.md`,
`tasks/refinements/compositor/damage_planning.md`,
`tasks/refinements/packaging/examples.md` (source-of-debt).

## Constraints / requirements

1. **L4 purity holds.** `map_damage_to_device` keeps its resolver-free
   signature (`damage_planning.hpp:163-164`); the fix uses only the `DocRoot`
   it already receives. No routing moves into the mapper
   (operator_model_damage_routing Decision 1 rejected that twice).
2. **Walk-based matching, not blind id resolution.** Matching must be scoped
   to the viewport's displayed tree so a viewport anchored to an unrelated
   composition maps a placement edit to zero rects — the per-viewport
   isolation content damage already has by construction, and which the
   `DamageRouter`'s verbatim fan-out (damage_router Decision 4) makes
   load-bearing. A `find_layer(d.object) != nullptr → full viewport`
   shortcut is ruled out (Decision 1, alternative b).
3. **Placement damage repaints, never invalidates.** Layer-keyed records must
   keep matching nothing in `invalidate_damage` (`TileKey.content` is a
   content id): a moved layer's tiles stay resident and re-composite through
   the new transform. This is the status quo falling out of the key spaces —
   the task pins it with a claim so it cannot regress (mirror of
   `02-architecture#camera-change-does-not-invalidate`).
4. **Structural matches must survive the edit that hides them.** The damage
   is flushed after commit, so the walk sees only the post-state: a
   `set_layer_visible(false)`, an opacity→0, or a transform that moves the
   layer off-view must still repaint the pixels the layer used to occupy.
   Therefore layer-id matches bypass the visitor's visible/opacity gates, and
   the descent hook fires before the group-layer pruning (Decision 2/4).
   Content-id matches keep the existing gates — an invisible layer's content
   edit contributes nothing, exactly as today.
5. **Existing mapping behavior is frozen.** Content-keyed matching, the
   temporal gate (`damage_planning.cpp:89-92`), the finite-rect projection,
   and the infinite-rect saturation are untouched for content-keyed records;
   all existing damage-planning/interactive tests and goldens pass unchanged.
6. **Levelization** (doc 17): the change lives in `arbc-compositor` (L4) plus
   mechanical visitor-signature updates at its in-repo call sites; no new
   dependency, no new component.
7. **Concurrency**: none touched — planning runs on the frame thread against
   a pinned `DocRoot`; the flush path is untouched. No new TSan/stress scope
   (damage_router Decision 5 pattern).

## Acceptance criteria

- **A1 — the headline regression, claimed.** New claims-register row
  `02-architecture#placement-damage-maps-to-device`: *structural damage
  naming any node of a viewport's displayed tree — a leaf layer, a group
  layer, a descended child composition, or the anchor composition — maps to
  the full viewport; structural damage naming a node the viewport does not
  display contributes nothing.* Enforced by (a) unit sections in
  `src/compositor/t/damage_planning.t.cpp` covering each match kind plus the
  not-displayed miss, and (b) a shipped-driver test in
  `src/runtime/t/host_viewport.t.cpp`: a Document-bound `HostViewport`,
  `document.set_layer_transform(...)`, next `step()` issues a frame whose
  target is byte-identical to a fresh render of the edited scene — no forged
  damage, no `force_repaint`.
- **A2 — hide/show soundness.** Sections under A1's claim: a
  `set_layer_visible(false)` (and an opacity→0) edit repaints — the
  previously-drawn pixels are cleared — pinned by byte-identity with a fresh
  render of the post-edit scene (Constraint 4's predicate exemption is the
  code under test).
- **A3 — never invalidates, claimed.** New row
  `02-architecture#placement-change-does-not-invalidate`: *a placement edit
  on a warm scene re-composites from resident tiles — `requests_issued`
  delta 0 while the frame composites — and the moved layer's tiles are not
  evicted.* Behavioral-counter assertion, never wall-clock; twin of the
  camera claim.
- **A4 — router fan-out + isolation.** Two viewports on one `DamageRouter`,
  anchored to different compositions: a placement edit in composition A
  repaints A's viewport and leaves B's counters at zero (frames may issue —
  the batch fans verbatim — but B maps zero rects and early-outs). Second
  `enforces:` tag on `01-core-concepts#multiple-viewports-observe-one-composition`
  (registry row 32), per interactive_pull_wiring Decision 5's
  prefer-second-tags rule.
- **A5 — membership mapping.** `attach_layer`/`detach`/`reorder` damage
  (composition-keyed, claim row 22) on the anchor composition and on a
  descended child composition maps to the full viewport — unit sections under
  A1's claim. Second `enforces:` tag on
  `02-architecture#damage-maps-to-device-dirty-regions` (row 152) for the now
  layer-/composition-keyed structural full-viewport branch.
- **A6 — byte-exact golden.** New case in
  `tests/damage_planning_golden.t.cpp`: a placement-edit-gated repaint is
  byte-identical to a full re-render of the edited scene (doc 16: goldens,
  no tolerances).
- **A7 — example exercises the shipped path.**
  `examples/host-interactive/` gains a placement gesture
  (`set_layer_transform` drag) in its `GestureScript`; the example composites
  it with no forged damage; the README's gesture list is updated. (The
  original `force_repaint()` workaround was already deleted by
  `runtime.camera_change_damage` — the `.tji` note's "documented workaround"
  reference is stale; nothing remains to delete, only the positive
  demonstration to add.)
- **A8 — doc 02 delta rides the commit** (same-commit rule, doc 16), giving
  A1/A3's claims their anchor. Proposed text, appended to the doc 02
  damage-classes passage (after `:96`): *"Placement and membership damage
  keep their model-space key — the edited layer's or composition's own id —
  and map to the full viewport of every viewport whose displayed tree
  contains that node, and to nothing elsewhere. Like a camera change, a
  placement change repaints and never invalidates: no content's pixels
  changed, so its tiles stay resident and re-composite through the new
  placement."*
- **A9 — existing behavior frozen.** All existing damage-planning,
  interactive, and golden tests pass unchanged (Constraint 5).
- **A10 — deferred follow-up registered** (closer registers in WBS):
  `runtime.nested_inner_placement_damage` — effort 1d — *route structural
  damage naming a layer or composition inside a nested-kind-referenced
  composition to the embedding `NestedContent`'s id, so inner placement and
  membership edits repaint the embedder (the per-object revision fold
  already re-keys the composed tiles; `tests/per_object_revision_keys.t.cpp:341-370`
  forges exactly this damage by hand today).* Driver-side seam (a reverse
  composition→embedder lookup), out of `cull_walk`'s reach by construction —
  the walk descends `CompositionRecord` children only
  (`anchored_viewports.cpp:49`), never nested-kind leaves. Milestone:
  `m10_post_01` (nested inner editing is not part of the v0.1 example
  surface, and the gap predates this task).
- **Coverage**: ≥90% diff coverage on changed lines (CI gate); tests land in
  the same commit.

## Decisions

1. **Match in the mapper, via the walk (chosen).** Extend
   `map_damage_to_device`'s matching to the walk-path node ids, inside the
   L4 function it already owns. Alternatives rejected:
   - *(a) Driver-side re-key to the content id*
     (`find_layer(d.object)->content` in `route_model_damage`, the `.tji`
     note's first suggestion): the routed set feeds `invalidate_damage` as
     well (operator_model_damage_routing Decision 3), so re-keying a
     placement nudge onto the content id would evict that content's resident
     tiles across every rung (claim row 108's machinery) — violating
     Constraint 3 — and it cannot express a group-layer or membership edit
     (the "content" would be a composition id, which the leaf walk never
     visits either).
   - *(b) Blind resolve — `find_layer(d.object) != nullptr` → full
     viewport*: loses per-viewport isolation (Constraint 2); every viewport
     on the router would repaint on every placement edit anywhere in the
     document.
   - *(c) Renderer-side delta detection à la camera*: the camera precedent
     (camera_change_damage Decision 2) exists precisely because a camera
     edit has *no* model-space key. A placement edit has one — the layer id
     the model already flushes (claim row 29) — and diffing arrangement
     per frame would be O(layers) every frame instead of O(1) per edit.
2. **Walk seam: extend the visitor, add a pre-pruning descent hook.** The
   `cull_walk` leaf visitor gains the layer's ObjectId —
   `visit(ObjectId layer_id, const LayerRecord&, const Affine& composed)` —
   and `cull_walk`/`walk_composition` gain an optional descent callback
   `on_descend(ObjectId group_layer_id, ObjectId child_composition_id,
   const Affine& composed)` fired for every composition-child layer
   encountered, **before** the visibility/opacity/`subtree_culled` pruning
   (`anchored_viewports.cpp:49-57`), so a hidden or moved-off-view group
   still matches (Constraint 4). Only damage planning passes the hook;
   `render_frame_anchored` and `clock_advance_damage` ignore the new
   parameter. Rejected: a separate matching walk in damage_planning
   (duplicates cull policy, drifts); funneling group layers through the leaf
   visitor (every render call site would need a "is this really a leaf"
   re-check).
   Soundness note for the pruning interaction: nodes *inside* a culled
   subtree are unreported, and that is correct — an edit strictly inside an
   off-view or hidden subtree changed nothing that was drawn, and any edit
   that moved/revealed the subtree itself names the group layer or its
   composition, both reported before pruning.
3. **Structural matches map to the full viewport; no old∪new tightening.**
   The old placement is unrepresentable post-commit (the walk sees the new
   state only), so the pre-edit footprint can only be covered conservatively;
   the model already says so by flushing `Rect::infinite()` (claim rows
   29/34), and the mapper's existing infinite-rect rule
   (`damage_planning.cpp:104-108`) saturates to the viewport rect. Finite
   rects on layer-/composition-keyed damage (no producer today) map
   uniformly through `composed`, same as content rects — one rule, no dead
   special case. Rejected: journal-derived before/after footprints (a new
   consumer of the journal in L4, for a repaint-region optimization doc 02
   already declares acceptable to over-approximate).
4. **Layer-id matches bypass the visible/opacity gates; content-id matches
   keep them.** The gate exists so an invisible layer's *content* edit
   contributes no pixels — still right. A *placement* edit may be the very
   thing that made the layer invisible, so gating it would drop the repaint
   that clears the old pixels (Constraint 4). Accepted conservatism, both
   directions: an edit to an already-hidden layer, or an audio-only
   placement edit (`set_layer_gain`/`set_layer_audible` also damage the
   layer id — and must keep doing so: the audio lookahead consumes
   structural damage, claim row 34), buys one byte-identical full-viewport
   composite. Rejected: a damage-kind discriminant on `Damage` to separate
   audio-facet placement (model-schema change rippling through claim rows
   9/29/33/34 for a bounded composite-only cost).
5. **Membership (composition-keyed) mapping is in scope.** The task title
   says "layer-keyed", but `attach`/`detach`/`reorder` damage (claim row 22)
   is dropped by the very same predicate, the fix is the same descent hook
   plus an anchor-id check, and excluding it would ship a successor task
   editing the same ten lines. The pre-camera example workaround only ever
   repainted through its *content* add/remove damage — attach-only damage has
   never mapped.
6. **Nested-kind inner edits are deferred, named.** A layer inside a
   composition referenced by `org.arbc.nested` is not on any embedder
   viewport's walk path, and mapping it needs a composition→embedding-content
   reverse lookup — a driver/routing seam interacting with pull identities,
   not a mapper predicate. Deferred to
   `runtime.nested_inner_placement_damage` (A10) rather than widening this
   task past its 1d box; no shipped scenario regresses (the gap predates this
   task, and the revision fold already prevents stale *tiles* — only the
   repaint scheduling is missing).
7. **Claims strategy: two new rows, two second-enforcer tags.** New rows only
   for the two genuinely new doc promises (A1's match domain, A3's
   non-invalidation — both anchored by A8's doc 02 delta); second tags on
   rows 32 and 152 where the existing claim language already covers the
   driver-agnostic behavior (interactive_pull_wiring Decision 5 continuity).

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-17.

- `map_damage_to_device` extended to match `Damage.object` against the full walk-path node ids — leaf layer id, group layer id, child composition id, anchor composition id — via a visitor-id extension + pre-pruning `on_descend` hook on `cull_walk` (`src/compositor/damage_planning.cpp`, `src/compositor/anchored_viewports.cpp`, `src/compositor/arbc/compositor/damage_planning.hpp`, `src/compositor/arbc/compositor/anchored_viewports.hpp`).
- Layer-id matches bypass visible/opacity gates so a hide/opacity→0/move-off-view edit still repaints the pixels the layer occupied; mechanical visitor-signature updates propagated to all call sites (`src/compositor/t/anchored_viewports.t.cpp`, `src/compositor/t/damage_planning.t.cpp`).
- Placement and membership damage repaints full-viewport and never invalidates; pinned by two new claims-register rows (`tests/claims/registry.tsv` +2 rows: `02-architecture#placement-damage-maps-to-device`, `02-architecture#placement-change-does-not-invalidate`) and driver tests (`src/runtime/t/host_viewport.t.cpp`).
- Per-viewport isolation confirmed: structural damage on composition A repaints A's viewport and maps to nothing in B's viewport; second enforcer tags added on registry rows 32 and 152 (`tests/interactive_model_damage_routing.t.cpp`).
- Byte-exact golden added: placement-edit-gated repaint is byte-identical to a full re-render, `invalidate_damage == 0` pinned (`tests/damage_planning_golden.t.cpp`).
- `docs/design/02-architecture.md` updated with A8 delta — placement/membership damage class documented.
- Host-interactive example gains a `set_layer_transform` drag gesture with no forged damage (`examples/host-interactive/main.cpp`, `examples/host-interactive/README.md`); consumer test expectation updated (`tests/consumer/host_example_artifacts.cpp`).
- Tech-debt follow-up registered in WBS: `runtime.nested_inner_placement_damage` (1d, `m10_post_01`) — route structural damage inside nested-kind-referenced compositions to the embedding `NestedContent` id.
