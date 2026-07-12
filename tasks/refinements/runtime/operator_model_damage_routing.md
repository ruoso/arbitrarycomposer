# runtime.operator_model_damage_routing — Route Model Damage Through the Operator Graph

## TaskJuggler entry

[`tasks/65-runtime.tji:125-130`](../../65-runtime.tji)

> **operator_model_damage_routing** — "Route model damage through
> route_operator_damage in interactive and export drivers"
>
> Route model damage (content_damage, not refinement-arrival damage) through
> `route_operator_damage` in the interactive and export drivers, so an edit to a
> content that an operator consumes by `$ref` (and is not itself a visible layer)
> re-plans and invalidates the operator layers that reach it. Today
> `map_damage_to_device` matches damage against layer roots only
> (`damage_planning.cpp:39`), so such an edit is silently dropped — an
> under-approximation that doc 13:124-128 calls a correctness bug.
> Source-of-debt: `tasks/refinements/runtime/interactive_pull_wiring.md`. Docs
> 02/13.

Not to be confused with **`runtime.damage_router`** (done, 2026-07-10), which is
the L5 fan-out of one model-damage *batch* to N `HostViewport`s. This task is the
L4 fan-out of one damage *rect* up an operator's `inputs()` DAG. Same word, two
axes.

## Effort estimate

**1d.** The kernel (`route_operator_damage`), the per-revision memo it needs
(`refresh_identity_memo`), and the operator-layer set it walks all landed in
`runtime.interactive_pull_wiring`. What is left is: a second router entry point
that resolves a *model* `ObjectId` (not a pull identity) to its `Content*`, a
re-ordering of `InteractiveRenderer::render_frame` so the memo is warm before the
no-damage early-out, the pull-identity arm of cache invalidation, and the tests —
one driver-level regression test, one finite-rect covering test, one
non-reaching-edit counter test, and the invalidation assertion. Roughly half the
day is the frame-step re-ordering and proving it does not perturb the still-scene
counters (claim `02-architecture#interactive-still-scene-schedules-no-frame`).

## Inherited dependencies

**Settled:**

- **`runtime.interactive_pull_wiring`** (7322746) — landed `PullServiceImpl` on
  the interactive frame path, the revision-memoized pull-identity map
  (`interactive.cpp:40-66`), the visible operator-layer set `d_operator_layers`,
  and `route_arrival_damage` (`interactive.cpp:68-92`) — the *first and only*
  production caller of `route_operator_damage`. Its Decision 4 routes the
  **refinement-arrival** damage class; its Constraint 6 explicitly scopes model
  damage out and names this task.
- **`runtime.operator_identity_offline_delivery`** — the identity-endpoint
  delivery branch the follow-up frame re-enters.
- **`compositor.operator_graph`** — `route_operator_damage` /
  `aggregate_revision` / `is_operator`, with the covering contract and the cycle
  backstop already tested at unit level
  (`src/compositor/t/operator_graph.t.cpp:232-288`).
- **`runtime.damage_router`**, **`runtime.host_viewport_document_binding`** — the
  model-damage delivery path into `render_frame`
  (`host_viewport.cpp:145-158`: `d_sink.drain()` → `render_frame(..., damage,
  ...)`).

**Pending (deliberately downstream — this task must not wait on them):**

- **`runtime.interactive_binder_wiring`** — calls `bind_operators` per
  interactive frame. Until it lands, `NestedContent::inputs()` is empty on the
  interactive path (`operator_binding.hpp:133-137`), so only construction-bound
  operators (`org.arbc.fade`, `org.arbc.crossfade` — inputs fixed in the
  constructor, `fade_content.cpp:18`, `crossfade_content.cpp:57`) are walkable
  there. That is enough to test this task end to end, and it is exactly the set
  the predecessor's test uses.
- **`runtime.worker_dispatch_leaf_only`** — worker-backed dispatch. Irrelevant
  here: routing is a pure walk on the frame thread.

## What this task is

The interactive driver already routes one of its two damage classes up the
operator graph. It routes the class it *created* — a refinement arrival, whose
`Damage.object` names an operator input's synthesized pull identity — and leaves
the other class, **model damage**, going straight to `map_damage_to_device`
unrouted (`interactive.cpp:127-133`, where `present_damage` is built from
`model_damage` verbatim).

`map_damage_to_device` matches `Damage.object` against **layer roots only**
(`damage_planning.cpp:39`: `layer.content == d.object`, inside a `cull_walk` over
`DocRoot`'s `LayerRecord`s). A content that lives in the document's `contents`
table and is reached only through an operator's `inputs()` — the `$ref` case of
doc 13:181-184 — is not a `LayerRecord`. So an edit to it produces
`Damage{its_own_model_id, …}` (every model mutator emits damage under the edited
object's own id, `model.cpp:1567-1579` publishes and flushes it), that damage
matches no layer, the frame's dirty region comes back empty, the no-damage
early-out (`interactive.cpp:146-150`) fires, and **the frame that should have
repainted the fade is never rendered.** The stale pixels stay on the persisted
`target`. That is the under-approximation doc 13:123-128 calls a correctness bug.

This task closes it: route model damage through the same
`route_operator_damage` kernel before it reaches `map_damage_to_device` *and*
before it reaches `invalidate_damage`, and add the one thing the arrival path did
not need — dropping the damaged input's cached tiles, which are keyed under its
**pull identity**, not its model id.

The export driver needs no change, and the task's title notwithstanding, it gets
none; see Decision 5.

## Why it needs to be done

1. **A shared component is not editable today.** The whole point of doc 13's
   `contents` table + `$ref` is shared content: one graded clip feeding a fade in
   three places. Edit it and nothing repaints. Doc 05:141-144 promises the
   opposite in as many words: *"an edit deep inside a shared component invalidates
   every place it appears, at every viewport, at the right screen rectangles."*
2. **Claim `05-recursive-composition#operator-damage-routes-through-map-input-damage`
   (registry:154) has no driver-level enforcer.** It is enforced today only at
   unit level (`src/compositor/t/operator_graph.t.cpp:232`) and in the nested
   metadata/conformance suites. The kernel is proven; the wiring is not.
3. **It is a silent failure, not a loud one.** No diagnostic, no degraded
   composite, no counter delta — the frame simply does not happen. The class of
   bug that only a regression test catches.
4. **It blocks `runtime.interactive_binder_wiring` from being trustworthy.**
   That task makes nested/fade/crossfade render attached interactively; an
   attached operator whose input is not separately a layer inherits this bug on
   day one.

## Inputs / context

### Design docs (normative)

- **doc 13:114-128** (`docs/design/13-effects-as-operators.md`) — the covering
  contract, verbatim: *"`map_input_damage` is the same mapping in reverse… Its
  contract is a **covering** one: the mapped output damage must cover every output
  pixel the input change can affect — over-approximation is sound (extra pixels
  merely re-render), **under-approximation is a correctness bug (stale pixels
  survive an edit)**."* This is the sentence the current driver violates.
- **doc 13:181-184** — the `$ref` mechanism: *"for shared content the document
  gains an optional `"contents"` table of id → content description, referenced as
  `{"$ref": "id"}` from any `inputs` slot **or layer**."* Note "or layer": a
  `$ref`'d content may be *both* a layer root and an operator input, and the
  routing must handle that (it damages both footprints).
- **doc 13:145-149** — two-level caching: *"input tiles cache under the input's
  identity (shared by every consumer), operator output under the operator's."*
  This is why invalidation needs two ids per damaged input, not one.
- **doc 13:108-112** — metadata composes: a fade over a `Static` input is itself
  `Timed`. Load-bearing for Decision 4.
- **doc 05:141-144** — *"The aggregate revision is also what damage propagation
  uses: child damage maps through each embedding's transform into parent-space
  damage, which is how an edit deep inside a shared component invalidates every
  place it appears, at every viewport, at the right screen rectangles."*
- **doc 02:51-52** — step 1, *"Collect damage… No damage → no work"* — the
  early-out this task must keep honest.
- **doc 02:94-95** — *"Damage invalidates by `(content id, region)` across all
  rungs; revision bumps invalidate wholesale by making old keys unreachable."*
- **doc 01:145-150** — *"Damage propagates up through nesting to every viewport
  observing an affected composition; each viewport maps it to a dirty device
  region and schedules re-rendering."*
- **doc 17:46-63** — levelization. `compositor` is L4 and its damage functions
  are *"pure, caller-owned free functions… no `DamageSink` subscription, no
  cross-frame state at L4"* (`damage_planning.hpp:24-29`); `runtime` is L5 and
  owns *when* damage is collected and *whether* a frame is scheduled. Routing
  therefore stays in the driver. No new DEPENDS edge.

### Source seams

| What | Where |
| --- | --- |
| The layer-root-only match (the bug) | `src/compositor/damage_planning.cpp:38-41` (`layer.content == d.object`) |
| Why the mapper cannot route itself | `src/compositor/arbc/compositor/damage_planning.hpp:60-63` — *"this signature carries no `ContentResolver`"* |
| Homogeneous-id premise the bug breaks | `damage_planning.hpp:31-33` — *"`Damage.object` is read as the content id throughout"* |
| The routing kernel | `src/compositor/arbc/compositor/operator_graph.hpp:109-133`; body `src/compositor/operator_graph.cpp:57-115` |
| Arrival routing (the pattern to mirror) | `src/runtime/interactive.cpp:68-92` (`route_arrival_damage`), decl `interactive.hpp:163-169` |
| The per-revision memo (map, `id_of`, inverse, operator-layer set) | `src/runtime/interactive.cpp:40-66`; members `interactive.hpp:196-201` |
| Model damage enters the frame raw | `src/runtime/interactive.cpp:112` (`content_damage`) and `:127` (`present_damage`) |
| The no-damage early-out | `src/runtime/interactive.cpp:146-150` |
| Invalidation | `src/runtime/interactive.cpp:155`; kernel `src/compositor/damage_planning.cpp:84-99`; `src/cache/arbc/cache/invalidation.hpp:34-37,59` |
| Pull identities: layer roots keep their model id, inputs get **synthesized** ids | `src/runtime/pull_identity.cpp:22-33` (seed), `:45-61` (`ObjectId{next++}`, `next = max_seeded + 1` over *layer* ids) |
| Model publishes damage under the **edited object's own model id** | `src/model/model.cpp:1567-1579` (commit → one revision bump, one `flush`) |
| `damage_add` dedups by `object`, unioning rect + range | `src/model/arbc/model/damage.hpp:64-73` |
| Damage reaches the driver | `src/runtime/host_viewport.cpp:148,156-158` (`d_sink.drain()` → `render_frame`) |
| Export driver has **no** damage stream | `src/runtime/offline_sequence.cpp:134-136,178-180,196-198` (`/*dirty=*/nullptr` every call), `:185-188` (reaps to quiescence, discards `poll_refinements`' damage), `:57` (pins once) |
| Cache observable for the invalidation assertion | `src/cache/arbc/cache/keyed_store.hpp:191` (`resident_bytes()`); `cache::invalidate_content` returns a drop count |
| The predecessor's driver-level test to extend | `tests/interactive_operator_identity.t.cpp` |

### Predecessor decisions this task inherits verbatim

- **`interactive_pull_wiring` Decision 2** — the identity map, `id_of`, the
  inverse `ObjectId → const Content*` map, and `d_operator_layers` are memoized on
  `state.revision()`. This task reuses that memo; it does not build a second one.
- **`interactive_pull_wiring` Decision 4** — route damage in the **driver**, not
  inside `map_damage_to_device`: the mapper takes no `ContentResolver` by design,
  and the operator graph is the driver's knowledge. Routing the *whole* operator
  layer set (not the culled set) is deliberate — the mapper culls after routing,
  and over-approximating the routed set is sound.
- **`interactive_pull_wiring` Decision 5** — re-enforce existing claims with a
  second `enforces:` tag rather than minting driver-specific claim ids when the
  doc language is driver-agnostic.

## Constraints / requirements

1. **Model damage resolves through the `ContentResolver`, never through the
   inverse identity map.** These are two different id spaces.
   `build_pull_identity_map` seeds layer roots under their real model `ObjectId`
   but assigns every non-layer input a **synthesized** id starting at
   `max(layer-root ids) + 1` (`pull_identity.cpp:45,58`). A `$ref` content that is
   only an operator input therefore appears in `d_content_by_id` under a
   *synthetic* id, not its model id — so `d_content_by_id.find(model_id)` is at
   best a miss and at worst a **collision** with an unrelated content's synthetic
   id (the disjointness guarantee is only against *layer-root* ids; claim
   registry:177 says exactly that). `render_frame` already takes the
   `ContentResolver` (`interactive.hpp:125`); use it. See Decision 2 and the
   deferred `runtime.pull_identity_disjoint_ids`.

2. **The routed set must feed both consumers.** The routed model damage goes into
   the set handed to `map_damage_to_device` (`interactive.cpp:133`, so the
   operator layer's footprint re-plans) **and** the set handed to
   `invalidate_damage` (`interactive.cpp:155`, so the operator's cached *output*
   tiles, keyed under the operator's own id, are dropped). This is the split the
   arrival path deliberately does *not* make (`interactive.hpp:183-188`: an
   arrival repaints without invalidating); model damage is the class that
   invalidates.

3. **The damaged input's own tiles must be dropped under its pull identity.**
   `invalidate_damage` keys on `d.object` (`damage_planning.cpp:90,95`), and the
   input's tiles were cached under its *pull* identity (doc 13:145-149;
   `PullConfig::id_of` is `d_id_of`). Model damage naming its model id drops
   nothing. Emit, for each damaged content the identity map knows, an additional
   `Damage{d_id_of(content), d.rect, d.range}` into the invalidation set. It is
   harmless in the device-mapping set (a synthetic id matches no layer root, so it
   contributes zero rects), so one routed set can serve both (Constraint 2).

4. **The memo must be warm before the early-out, and a still frame must still
   build nothing.** `refresh_identity_memo` runs at `interactive.cpp:166`, *after*
   Step 2's mapping (`:133`) and after the early-out (`:146`). Routing needs
   `d_operator_layers` before Step 2. Refresh it in Step 1, guarded on
   `!model_damage.empty()` — a still frame carries no model damage, so it takes no
   walk and `identity_map_builds()` stays put (claim
   `02-architecture#interactive-still-scene-schedules-no-frame`, registry:145;
   `02-architecture#idle-viewport-issues-no-frames`, registry:146). The call at
   `:166` stays (it is memoized on revision, so the second call is a no-op) —
   the first frame at a fresh revision with no model damage still needs the map
   for the pull service. Update `interactive.hpp:142-147`'s comment, which
   currently says the counter *"bumps once per revision that actually renders a
   frame"*; after this change it bumps once per revision that **carries model
   damage or renders a frame**.

5. **Clock-advance damage is NOT routed.** `clock_advance_damage`
   (`damage_planning.cpp:59-82`) already emits whole-footprint damage for *every*
   visible non-`Static` layer — and an operator over a moving input is itself
   non-`Static` by stability composition (doc 13:108-112; claims
   `13-effects-as-operators#fade-timed-over-static` registry:168 and
   `#crossfade-timed-over-static` registry:175). It is therefore already in that
   set with `Rect::infinite()` damage on its own object. Routing it would be a
   strictly redundant O(|operators| × |graph|) walk per moving layer per frame.
   See Decision 4.

6. **Structural (`Rect::infinite()`) damage must survive routing as non-finite.**
   Every model mutator emits `Damage{id, Rect::infinite(), TimeRange::all()}`
   (`model.cpp:822-823` and siblings), so the *common* case is whole-object
   damage. `map_damage_up` folds it through `map_input_damage`, whose default is
   the identity and whose implementations inflate/transform — an infinite (or, at
   worst, NaN) rect stays non-finite, and both `map_damage_to_device:43` and
   `invalidate_damage:87` route non-finite to the conservative whole-content path.
   Pin it with a test (A2); do not "helpfully" clamp it to `bounds()`.

7. **Pointer identity between `resolve(id)` and the operator's `inputs()` edge is
   load-bearing.** Routing finds the damaged content by `Content*` comparison
   (`operator_graph.cpp:61`). `bind_operators` builds its resolver from
   `document.resolve(id)` (`operator_binding.cpp:106`) and the reader binds input
   children from the same instances (`codec_fade.cpp:101-121`), so the production
   graph satisfies this. Any test must too: the `ContentResolver` handed to
   `render_frame` must return the *same* `Content*` the operator's `inputs()`
   points at, or routing silently finds nothing.

8. **Levelization: no new edges.** All of this is L5 (`runtime`) calling an
   existing L4 (`compositor`) free function. `scripts/check_levels.py` must stay
   green.

## Acceptance criteria

**A1 — An edit to an operator's `$ref` input repaints the operator layer.**
A driver-level test (extend `tests/interactive_operator_identity.t.cpp`, or a new
`tests/interactive_model_damage_routing.t.cpp` alongside it): a `Document` whose
`contents` table holds a solid `from` (model id F) that is **not** a layer, and an
`org.arbc.crossfade` layer whose inputs are `(from, to)`. Frame 1 renders and
composites. Commit an edit to `from`; the model flushes `Damage{F, infinite,
all()}`. Frame 2, given that damage, must produce a non-empty dirty region, issue
renders, composite, and land **byte-identical** to a synchronous `SequenceRenderer`
export of the edited document at the same instant. Strip the routing and frame 2
early-outs at `interactive.cpp:146` with `composites` delta 0 and a stale
`target` — that is the regression this test exists for.
`enforces: 05-recursive-composition#operator-damage-routes-through-map-input-damage`

**A2 — Whole-object model damage maps to the whole viewport.** Same fixture: the
structural `Rect::infinite()` damage on `from` routes to non-finite damage on the
crossfade layer, and `map_damage_to_device` takes its whole-viewport branch
(`damage_planning.cpp:43-47`) — the frame's `DirtyRegion` is the full viewport
rect, not an empty or a NaN-degenerate one (Constraint 6).
`enforces: 02-architecture#damage-maps-to-device-dirty-regions`

**A3 — A finite input-damage rect maps through `map_input_damage`, covering.**
Hand the driver a finite `Damage{F, r, range}` (a raster paint stroke's shape;
`14-data-model-and-editing#damage-carries-region-and-time` is the model-side
guarantee that `r` survives the commit intact). The resulting device dirty region
must be the crossfade's composed `map_input_damage` image of `r` clipped to the
viewport — covering it, never under-reporting it — and the frame must re-render
exactly that region byte-identically to the full synchronous export while pixels
outside it stay bit-identical to frame 1's.
`enforces: 03-layer-plugin-interface#operator-damage-covers`
`enforces: 03-layer-plugin-interface#undamaged-regions-stable`

**A4 — An unreached edit routes to nothing.** A third content in the `contents`
table, reached by no operator and placed as no layer. Editing it yields damage
that routes to zero operator layers and maps to zero device rects: the frame
early-outs with `requests_issued` / `composites` / `follow_up_frames` deltas all
0. This is the driver-level half of *"an operator that does not reach the damaged
input receives none"* — the guard against fixing under-approximation by
over-approximating everything.
`enforces: 02-architecture#interactive-still-scene-schedules-no-frame`

**A5 — The edited input's cached tiles are actually dropped.** After the frame
that carries the edit, `cache::invalidate_content(cache, <from's pull identity>)`
returns **0** — there is nothing left to drop, because the frame's invalidation
already dropped it under the pull identity (Constraint 3). Without the
pull-identity arm the same call returns non-zero (the stale tiles are still
resident, merely unreachable by key). Assert the operator's own output tiles are
dropped the same way.
`enforces: 02-architecture#damage-invalidates-by-content-region-across-rungs`

**A6 — A still frame still builds no identity map.** `identity_map_builds()`
does not advance across repeated still frames (no model damage, all-`Static`, no
pending), and `requests_issued` / `composites` / `follow_up_frames` deltas stay 0
— the memo hoist of Constraint 4 must not regress the still-scene contract. Add
the assertion to the existing still-scene test in `src/runtime/t/interactive.t.cpp`.
`enforces: 02-architecture#interactive-still-scene-schedules-no-frame`
`enforces: 02-architecture#idle-viewport-issues-no-frames`

**A7 — Routing terminates on a cyclic graph.** A unit test in
`src/runtime/t/interactive.t.cpp`: model damage on a content inside an operator
cycle routes once per operator layer, terminates, and emits a bounded damage set
— the driver-level exercise of the kernel's visited-set/budget backstop.
`enforces: 05-recursive-composition#graph-walk-bounds-cycles`

**A8 — No new claims-register rows.** Every claim above already exists
(registry:154, 122, 119, 136, 145, 146, 102, 155), and each is stated in
driver-agnostic language. Following `interactive_pull_wiring` Decision 5, this
task adds `enforces:` tags, not rows. `scripts/check_claims.py` stays green.

**A9 — Gates.** `scripts/gate` green, `scripts/check_levels.py` green (Constraint
8), ≥90% diff coverage on the changed lines, and the existing goldens
(`tests/damage_planning_golden.t.cpp`, `tests/interactive_operator_identity_golden.t.cpp`,
`tests/refinement_golden.t.cpp`) byte-unchanged — routing adds damage, it never
changes what a rendered pixel is.

### Deferred follow-up (closer registers in WBS)

- **`runtime.pull_identity_disjoint_ids`** — *"Allocate synthesized pull
  identities from a namespace provably disjoint from **every** model `ObjectId`,
  not merely from every layer-root id. `build_pull_identity_map` seeds `next =
  max(layer-root ids) + 1` (`pull_identity.cpp:45`), so a synthesized id can
  collide with the model id of a `contents`-table content that is not a layer.
  Consequence today: `invalidate_damage` on a model id can drop an unrelated
  content's tiles (a sound over-approximation — spurious drop, never a stale
  read, because tile keys carry the revision — but a silent cache-hygiene bug),
  and the disjointness claim in `interactive_operator_identity.t.cpp:5` is
  stronger than what the code guarantees. Seed above the model's maximum
  allocated `ObjectId` (or tag a high bit) and tighten claim registry:177's
  wording to match. Source-of-debt:
  `tasks/refinements/runtime/operator_model_damage_routing.md` Constraint 1 /
  Decision 2. Docs 02/13."* — effort **1d**, `depends
  !operator_model_damage_routing`, milestone **`m9_release`**.

## Decisions

**Decision 1 — Route model damage in `InteractiveRenderer::render_frame`, mirroring
`route_arrival_damage`; add a sibling `route_model_damage`, do not generalize the
two into one function.** The two damage classes differ in exactly the way that
matters — how a `Damage.object` becomes a `const Content*` (arrival: the inverse
pull-identity map; model: the `ContentResolver`) and what they do to the cache
(arrival: repaint only; model: invalidate + repaint). A single "route anything"
helper would have to take the lookup as a parameter and the caller would still
have to know which to pass, so it buys nothing but a shared `for` loop.
*Alternative rejected:* teaching `map_damage_to_device` to route internally — it
takes no `ContentResolver` by design (`damage_planning.hpp:60-63`), it is an L4
pure function with no cross-frame state, and the operator graph is the driver's
knowledge (`interactive_pull_wiring` Decision 4, restated).

**Decision 2 — Resolve a damaged model `ObjectId` through the `ContentResolver`,
never through `d_content_by_id`.** The inverse identity map is keyed in the *pull*
id space, where an operator's non-layer input carries a **synthesized** id
(`pull_identity.cpp:58`). Looking a model id up there is not merely a miss — the
synthetic range is only guaranteed disjoint from *layer-root* ids, so a model id
belonging to a `contents`-table content can alias a *different* content's
synthetic id and mis-route the damage. `render_frame` already receives the
`ContentResolver`; it is the model id space's own inverse and costs one call.
*Alternative rejected:* extending `build_pull_identity_map` to also key non-layer
inputs under their model id — it would double-key the map, muddle the very
separation that makes pull identities work, and still not fix the underlying
namespace overlap (which gets its own task,
`runtime.pull_identity_disjoint_ids`).

**Decision 3 — One routed set, two consumers; invalidation gets the extra
pull-identity records.** Build the routed model-damage set once, then feed it to
both `map_damage_to_device` and `invalidate_damage`. The extra
`Damage{pull_identity(content), …}` records (Constraint 3) ride in the same set:
a synthetic id matches no `LayerRecord`, so `map_damage_to_device` contributes
zero rects for them and no separate invalidation-only set is needed. `damage_add`
dedups by object, so a content that is both a layer root and an operator input
(doc 13:183 — `$ref` may name a layer *or* an inputs slot) has one record, its
identity id being its model id.
*Alternative rejected:* keeping two sets (a device set and an invalidation set)
for purity — two sets, two loops, and a standing invitation to update one and not
the other; the current code already has two overlapping sets (`content_damage` /
`present_damage`) and that split earns its keep only because arrival damage must
*not* invalidate.

**Decision 4 — Route model damage only; leave clock-advance damage unrouted.**
`clock_advance_damage` enumerates every visible non-`Static` layer, and an
operator's stability composes from its inputs (doc 13:108-112), so an operator
layer over a moving input is already in that set with whole-footprint damage on
its own object. Routing clock damage would emit damage that `damage_add` unions
into a record that already spans the whole object — pure cost, zero effect, and it
runs on *every* frame of playback rather than once per edit. Constraint 5 records
the dependency on stability composition; claims registry:168/175 pin it for the
two operator kinds that exist, and the contract conformance suite pins it for any
future one.
*Alternative rejected:* routing the whole `content_damage` set (model ∪ clock)
for uniformity — simpler to read, but it makes the routing walk a per-frame
playback cost instead of a per-edit cost, for no behavioral gain.

**Decision 5 — The export driver needs no change; the task's "and export drivers"
is discharged by a verified finding, not a code edit.** `SequenceRenderer` has no
damage stream at all: it pins the document once (`offline_sequence.cpp:57`), so
no edit can arrive mid-sequence; it passes `/*dirty=*/nullptr` on every
`render_frame_interactive` call (`:136`, `:180`, `:198`), so every frame plans the
whole viewport; and it reaps async arrivals to quiescence within the frame,
discarding the damage `poll_refinements` returns (`:185-188`) and re-compositing
blind. There is no `Damage` to route and no dirty region to widen. Routing would
be dead code.
*Alternative rejected:* wiring `route_operator_damage` into the export driver
"for symmetry" — it would be an unexercised, untestable branch. If the export
driver ever gains incremental re-render, it will need the split
`build_pull_identity_map` / `pull_identity_of` seam that the interactive driver
already has (`interactive.cpp:49-50`), and that is the task to write then.

**Decision 6 — Keep the kernel's O(|operators| × |graph|) search; build no
reachability index.** `route_operator_damage` re-walks each operator layer's DAG
per damaged content (`operator_graph.cpp:100-113`). After this task that search
runs on frames carrying model damage — i.e. per *edit*, not per frame (Decision 4
keeps it off the playback path), against a memoized operator-layer set that is
rebuilt per revision, not per frame. An interactive edit already costs a
transaction, a publish, and a re-render; a pointer walk over an operator graph is
noise beside it.
*Alternative rejected:* accumulating a `Content* → reaching-layer-ids` index
inside `build_pull_identity_map`'s existing walk — tempting (it visits exactly the
right edges), but its `walked` guard visits a diamond-shared child once, so the
index would record only the first reaching root; making it correct means dropping
the shared-visit short-circuit and reintroducing the cost on precisely the
sharing-heavy graphs the index was meant to speed up. Not worth it without a
measured problem.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-11.

- Added `route_model_damage` to `InteractiveRenderer` (`src/runtime/interactive.cpp`, `src/runtime/arbc/runtime/interactive.hpp`): resolves each damaged model `ObjectId` through the `ContentResolver`, emits one `Damage` record per reaching operator layer plus a pull-identity twin for cache invalidation; the routed set feeds both `map_damage_to_device` and `invalidate_damage`.
- Hoisted `refresh_identity_memo` into Step 1 guarded on `!model_damage.empty()`, so the memo is warm before the no-damage early-out and a still scene still builds no identity map.
- New integration test binary `tests/interactive_model_damage_routing.t.cpp` (registered in `tests/CMakeLists.txt`): covers A1 (crossfade `$ref` input repaints), A2 (infinite rect → whole-viewport dirty), A3 (finite-rect covering), A4 (unreached edit early-outs cleanly), A5 (pull-identity tiles dropped; asserted via tile-count discriminant rather than literal 0-return per scoped-test deviation).
- Extended `src/runtime/t/interactive.t.cpp` with A6 (still-scene memo counter does not advance) and A7 (cycle terminates with bounded damage set).
- `tests/interactive_operator_identity.t.cpp` extended with still-scene assertion and `idle-viewport-issues-no-frames` tag.
- No new claims-register rows (A8): added `enforces:` tags only, per `interactive_pull_wiring` Decision 5.
- Export driver unchanged (Decision 5): `SequenceRenderer` has no damage stream.
- Tech-debt registered: `runtime.pull_identity_disjoint_ids` (1d, depends `!operator_model_damage_routing`, milestone `m9_release`).
