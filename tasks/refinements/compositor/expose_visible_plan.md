# expose_visible_plan — Expose visible LayerTilePlan from render_frame_interactive

## TaskJuggler entry

Back-link: [`tasks/35-compositor.tji:41-46`](../../35-compositor.tji), leaf
`task expose_visible_plan` under `task compositor`.

- **Declared depends:** `!tile_planning` (the intra-`compositor` predecessor
  that owns `render_frame_interactive`, `LayerTilePlan`, and `plan_layer`).
- **Inherited through the parent** `task compositor`:
  `contract.async_render, cache.key_shapes, color.resampling`.
- **Note (quoted):** "Surface the visible LayerTilePlan from
  render_frame_interactive so the interactive loop can drive prime_prefetch
  (speculation step 7) without re-planning, avoiding counter perturbation.
  Doc 02. Source of debt: tasks/refinements/runtime/interactive.md."

This leaf is already wired into the **M4** milestone: `m4_recursion`
(`tasks/99-milestones.tji:37`) lists `compositor.expose_visible_plan` in its
`depends`. The closer marks `complete 100`; no new milestone edge is needed.

## Effort estimate

**1d.** The smallest leaf in the compositor stream — on par with
`counters` (1d). The work is two mechanical, well-precedented edits plus a
behavioral test:

1. Add one trailing defaulted out-parameter to `render_frame_interactive`
   (the family's established "add-a-defaulted-pointer" seam shape — the
   fifth time this signature grows this way) that captures the per-visible-
   layer `LayerTilePlan`s the driver already builds, instead of dropping
   them at layer-scope exit.
2. Replace the deferred Step-7 comment in the runtime loop
   (`src/runtime/interactive.cpp:119-125`) with a live speculation pass that
   drives `compositor::prime_prefetch` from the surfaced plans.
3. One runtime behavioral test asserting the speculation step is render-free
   and counter-neutral, plus a compositor unit assertion that the surfaced
   plans match what was composited, plus a golden regression run (no new
   golden).

No new kernel, no new component, no new levelization edge, no design-doc
delta.

## Inherited dependencies

**Settled:**

- **`compositor.tile_planning`** (DONE 2026-07-05) —
  `src/compositor/arbc/compositor/tile_planning.hpp`. Owns everything this
  task extends:
  - `LayerTilePlan` (`tile_planning.hpp:113-122`): the pure, move-only
    per-visible-layer value (`content`, `rung`, `remainder`,
    `local_to_device`, `time`, `snapshot`, `deadline`,
    `std::vector<PlannedTile> tiles`), move-only through each
    `PlannedTile`'s `CacheHold<TileValue> hold` (`tile_planning.hpp:98-107`).
  - `render_frame_interactive` (declared `tile_planning.hpp:198-205`,
    defined `src/compositor/tile_planning.cpp:208-364`): builds one
    `LayerTilePlan plan` per visible layer via `plan_layer(...)` at
    `tile_planning.cpp:273-275`, composites its `PlannedTile`s onto `target`
    (switch at `:340-361`), then **discards the plan at layer-scope exit**
    (the `for_each_layer` lambda closes at `:363`). That discard is the seam
    this task removes.
  - The seam-growth precedent: the signature already grows by one trailing
    defaulted parameter per sibling refinement — `RefinementQueue* pending`
    (`refinement`), `CompositorCounters* counters` (`counters`),
    `const DirtyRegion* dirty` (`damage_planning`), `Time composition_time`
    (`temporal_coalescing`). The **null/default path is byte-for-byte
    `tile_planning`'s**, guarded by the landed goldens — the invariant this
    task must preserve.
- **`compositor.refinement`** (DONE 2026-07-06) —
  `src/compositor/arbc/compositor/refinement.hpp`. Owns the consumer:
  - `prime_prefetch(TileCache&, const LayerTilePlan&, int zoom_direction,
    std::int32_t pan_radius)` (`refinement.hpp:121-122`, defined
    `src/compositor/refinement.cpp:30-65`): reads the visible `TileKey` set
    and covered region from `plan.tiles`, primes the pan annulus
    (`cache::pan_prefetch_ring`, `Adjacent`) and the zoom next-rung ring
    (`zoom_prefetch_ring`, `Speculative`) through `cache::prime_ring`,
    returns the merged want-list of **absent** ring keys. Renders nothing,
    evicts nothing (claim `02-architecture#prefetch-ring-classifies-resident-reports-absent`).
  - `refinement.hpp:95`: "`zoom_direction` is a caller-supplied sign — the
    compositor infers no gesture." So the *caller* (the runtime loop) is the
    correct place to supply the sign. Today `prime_prefetch` has **no
    production caller** — only `src/compositor/t/refinement.t.cpp` exercises
    it via a `single_tile_plan()` fixture.
- **`runtime.interactive`** (DONE 2026-07-06) —
  `src/runtime/interactive.cpp`, `src/runtime/arbc/runtime/interactive.hpp`.
  Owns the loop and the debt:
  - The frame runs doc 02's six steps as numbered Steps 1-8; the driver call
    is `render_frame_interactive(...)` at `interactive.cpp:85-86`.
  - **Step 7 is the deferred debt** (`interactive.cpp:119-125`): speculation
    is a comment because "Driving `compositor::prime_prefetch` needs the
    visible `LayerTilePlan`, which `render_frame_interactive` builds
    internally and does not surface; re-planning it here to feed the prefetch
    driver would double the cache lookups and perturb the very counters the
    loop asserts on." The registered follow-up (interactive.md Status) is
    *this task*.
  - Loop state members (`interactive.hpp:111-115`): `d_pending`,
    `d_counters`, `d_pool`, `d_prior_revision`. The loop already threads
    `&d_counters` into `render_frame_interactive` and calls
    `poll_refinements` (a `compositor.refinement` symbol) at `:109` — so the
    `runtime → compositor.refinement` edge already exists.

**Pending:** none — every predecessor is landed (M3 is closed).

## What this task is

Two edits that together discharge the deferred Step-7 speculation debt
without re-planning:

1. **Compositor seam (primary deliverable).** Add a trailing defaulted
   out-parameter to `render_frame_interactive`:

   ```cpp
   void render_frame_interactive(..., Time composition_time = Time::zero(),
                                 std::vector<LayerTilePlan>* visible_plans = nullptr);
   ```

   When `visible_plans != nullptr`, the driver `assign`s it empty at entry
   and, after each visible layer has been composited, **moves** that layer's
   `LayerTilePlan` into `*visible_plans` (in composite / bottom-to-top order)
   instead of letting it die at layer-scope exit. When `visible_plans ==
   nullptr` (the default) the plan is dropped exactly as today —
   byte-for-byte the current behavior.

2. **Runtime Step-7 wiring (discharges the debt).** Replace the deferred
   comment at `interactive.cpp:119-125` with a live speculation pass:
   - pass a frame-local `std::vector<LayerTilePlan> visible_plans;` into the
     `render_frame_interactive` call;
   - derive `zoom_direction` as the sign of the frame-over-frame change in
     the viewport camera's scale magnitude (retain the prior scale in a new
     `double d_prev_camera_scale` member; `0` on the first frame or when
     unchanged) — the `Viewport` carries scale only inside its `camera`
     `Affine` (`compositor.hpp:15-28`), so the loop extracts the scale
     magnitude with the compositor's existing affine-scale helper;
   - for each surfaced plan call
     `prime_prefetch(cache, plan, zoom_direction, k_pan_prefetch_radius)`
     with a fixed one-ring `pan_radius` constant;
   - the returned want-lists are **not rendered** — rendering them is
     `compositor.pull_service`'s (M4); Step 7 in M3 only warms residency
     (reclassifies resident pan/zoom-ring tiles), which is render-free.

**Not this task:**

- **Rendering the prime_prefetch want-list** → already scoped to the
  existing leaf `compositor.pull_service` (`tasks/35-compositor.tji:47-52`,
  M4). Its note already owns "pulls hit the cache first, schedule on
  workers." No new leaf; re-assert the boundary.
- **`prime_prefetch`'s internal ring semantics** (pan/zoom ring assembly,
  reclassification, want-list) → owned by `compositor.refinement`; this task
  only *drives* it and re-asserts its landed claims.
- **Gesture / camera-motion tracking beyond a scalar scale delta** → the
  loop needs only the sign of the scale change to pick a zoom direction; a
  richer gesture model (velocity, direction of pan for a directional ring)
  is not required by any M3/M4 promise and is out of scope.
- **Frame-to-frame retention of the plan or its pins** → the surfaced
  `visible_plans` is a frame-local; it is consumed by Step 7 and dropped at
  frame end. The plan stays a pure per-frame value (`tile_planning`'s rule,
  cited in `refinement.md:451`) — no plan state crosses frames.

## Why it needs to be done

Doc 02's tile-cache priority ladder (`docs/design/02-architecture.md:92-93`)
promises a *speculative* working set — "visible > adjacent (pan prefetch
ring) > recently visible > speculative (next/previous zoom rung)" — and the
interactive frame's Step "Refine" (`02-architecture.md:66-71`) plus doc 04's
zoom speculation are the design's answer to "why zooming/panning shows
progressively sharper content rather than blocking." `compositor.refinement`
built the mechanism (`prime_prefetch` + the two rings) and
`runtime.interactive` built the loop — but the loop's speculation Step 7 is a
*deferred comment* because the only `LayerTilePlan` the frame computes dies
inside `render_frame_interactive`. Re-planning it in the loop to feed
`prime_prefetch` would double every visible tile's cache lookup and perturb
the behavioral counters the loop's own claims assert on
(`interactive.cpp:120-123`).

Surfacing the plan the driver already built closes that gap at zero counting
cost: the loop reuses the exact plan the frame composited from, drives the
prefetch rings render-free, and leaves the plan pass's counters untouched.
This is the last piece before `compositor.pull_service` (M4) can turn the
resulting want-lists into scheduled prefetch renders — hence the M4 gate.

## Inputs / context

- **Design docs (normative):**
  - `docs/design/02-architecture.md:49-71` — "The frame, interactively":
    the six-step pipeline; Step "Refine" (`:66-71`) is the speculation/
    progressive-sharpening promise this task's Step 7 serves.
  - `docs/design/02-architecture.md:88-99` — Tile cache: the priority
    classes (`:92-93`, pan prefetch ring / speculative zoom rung) that
    `prime_prefetch` reclassifies into, and the residency-pin semantics
    (`:100-116`) that govern the surfaced plan's `CacheHold`s.
  - `docs/design/02-architecture.md:118-137` — Threading: planning "never
    allocates and never takes a lock"; the compositor stays a pure per-frame
    library and frame-to-frame state is the runtime loop's.
  - `docs/design/04-transforms-and-infinite-zoom.md` (zoom speculation, the
    next-rung ring) — the design anchor for `zoom_direction`.
- **Source seams:**
  - `src/compositor/arbc/compositor/tile_planning.hpp:113-122` — `LayerTilePlan`;
    `:98-107` — `PlannedTile` (holds `CacheHold<TileValue> hold`, making the
    plan move-only); `:145-150` — `plan_layer`; `:198-205` —
    `render_frame_interactive` (the signature to grow); `:152-197` — the
    driver's doc-comment style to extend for the new parameter.
  - `src/compositor/tile_planning.cpp:208-364` — the driver body;
    `:273-275` builds the plan, `:340-361` composites it, `:363` is where the
    plan is currently dropped (the move-out point).
  - `src/compositor/arbc/compositor/refinement.hpp:95, 111-122` —
    `prime_prefetch` contract and the caller-supplies-`zoom_direction` note.
  - `src/runtime/interactive.cpp:85-86` (driver call), `:105-125`
    (Steps 6-7, the deferred comment to replace), `:127-130` (Step 8 state
    advance — where `d_prev_camera_scale` is updated).
  - `src/runtime/arbc/runtime/interactive.hpp:94-115` — loop accessors
    (`counters()`, `counters_snapshot()`) and state members (add
    `d_prev_camera_scale` here).
  - `src/compositor/arbc/compositor/compositor.hpp:15-28` — `Viewport`
    (scale lives inside `camera`, not a scalar field).
- **Levelization (doc 17):** `src/runtime/CMakeLists.txt:8` —
  `DEPENDS base model contract compositor pool`. `compositor` is already a
  runtime dependency and `poll_refinements`/`prime_prefetch` share the
  `compositor` component, so the Step-7 wiring adds **no new edge**. The
  seam itself lives entirely inside `compositor`.
- **Claims register:** `tests/claims/registry.tsv`. Relevant landed rows
  this task re-asserts:
  - `02-architecture#prefetch-ring-classifies-resident-reports-absent`
    (registry `:88`) — prime reclassifies resident ring members and returns
    absent keys; inserts/evicts nothing.
  - `02-architecture#interactive-still-scene-schedules-no-frame`
    (registry `:104`) — the assembled loop's still-scene
    `requests_issued/composites/follow_up_frames` deltas are all 0.
  - `04-transforms-and-infinite-zoom#zoom-speculates-next-rung`
    (registry `:91`) — zoom primes the next rung under `Speculative`.
- **Predecessor refinements (style/decision continuity):**
  `tasks/refinements/compositor/tile_planning.md`,
  `.../refinement.md` (the "return the plain value, don't thread frame state
  through `LayerTilePlan`" template, `refinement.md:450-464`),
  `.../counters.md` (the "null path is byte-identical; the surface observes
  without perturbing" discipline), and the source-of-debt
  `tasks/refinements/runtime/interactive.md` (Step-7 deferral, Status
  registration line).

## Constraints / requirements

- **Null/default path is byte-for-byte `tile_planning`'s.** With
  `visible_plans == nullptr`, `render_frame_interactive` produces
  byte-identical `target` pixels and identical cache state to today. The
  landed `tile_planning`/`refinement`/`counters`/`damage_planning`/
  `anchored_viewports` goldens are the guard — they run unchanged.
- **Composite order preserved.** The surfaced plans appear in the same
  bottom-to-top order the layers composited, so a consumer iterating
  `visible_plans` sees visible layers in a stable, meaningful order.
- **Surfaced plans equal what was composited.** Each entry in
  `*visible_plans` is the exact `LayerTilePlan` (same `content`, `rung`,
  `tiles` with the same `TileKey`s and `display_source`s) the driver
  composited from — no re-lookup, no second `plan_layer` call. This is the
  whole point: zero additional cache probes over the plan pass.
- **Speculation is render-free and composite-free.** Step 7 issues zero
  `RenderRequest`s and zero `Backend::composite` calls;
  `d_counters.requests_issued` and `d_counters.composites` are unchanged by
  the prime pass. The counters the debt comment named
  (`interactive.cpp:120-123`) are provably unperturbed.
- **Damage-gated frames surface only planned layers.** On the dirty-gated
  path, a layer whose local region is empty after damage intersection is
  skipped and no plan is built for it (`tile_planning.cpp` continues past
  it); `*visible_plans` therefore contains exactly the layers planned this
  frame. An empty-`DirtyRegion` frame surfaces zero plans → Step 7 primes
  nothing → no speculation work, consistent with no-damage-no-work.
- **Pin-lifetime note (active path only).** Moving each plan into
  `*visible_plans` retains its `PlannedTile` `CacheHold`s until the caller
  drops the vector, rather than releasing them at each layer's scope exit.
  Within `render_frame_interactive`, earlier layers' tiles therefore stay
  pinned while later layers' inline misses insert; under budget pressure
  eviction "skips pinned entries" (`02-architecture.md:113`), so cache
  eviction *victim selection/timing* on the **active** path may differ from
  the default. This is the intended semantics (the visible set stays
  resident for the prime pass) and is confined to the opt-in path; the
  null default is untouched, and the asserted invariant is render/composite
  neutrality, not `evictions()` equality on the active path.
- **Loop counter assertions target render-free counters.** Any runtime loop
  test that pins still-scene behavior asserts on
  `requests_issued/composites/follow_up_frames` (render-free) rather than
  raw `cache.misses()` across the whole loop, because Step 7's prime pass
  legitimately *probes* the ring (a lookup-shaped touch). If an existing
  `interactive.t.cpp` assertion measures raw cache hit/miss deltas spanning
  the full loop, it is updated to isolate the plan pass from the prime pass.
- **Levelization (doc 17):** the seam is `compositor`-internal; the wiring
  uses the existing `runtime → compositor` edge. No `backend-cpu` edge, no
  new dependency. CI's component graph check stays green.
- **Single-threaded.** Both the seam and the loop wiring run on the frame
  thread; no concurrency is introduced, so no TSan/stress obligation.
- **CI diff coverage ≥ 90%** on changed lines (doc 16) — the behavioral
  test plus the unit assertion cover the seam and the wiring.

## Acceptance criteria

**Claims (register + `enforces:` tags).**

- **New row** in `tests/claims/registry.tsv`:
  `02-architecture#speculation-drives-from-exposed-plan` — "render_frame_interactive
  optionally surfaces the per-visible-layer LayerTilePlans it composited
  (in composite order, only when the caller supplies a sink; the null
  default is byte-identical to no sink); the interactive loop's speculation
  step drives prime_prefetch from those exposed plans rather than
  re-planning, so it issues zero render requests and zero composites and
  leaves requests_issued/composites unchanged from the plan pass, while
  reclassifying the resident pan/zoom prefetch-ring tiles." Enforced by a
  `// enforces:`-tagged case in the runtime loop test
  (`src/runtime/t/interactive.t.cpp`).
- **Re-asserted (not re-registered)** — a claim may carry more than one
  enforcing test:
  - `02-architecture#interactive-still-scene-schedules-no-frame`
    (registry `:104`) — the still-scene loop, now with Step 7 wired, still
    shows `requests_issued/composites/follow_up_frames` deltas of 0
    (prime is render-free). Add the `enforces:` tag to the wired still-scene
    case.
  - `02-architecture#prefetch-ring-classifies-resident-reports-absent`
    (registry `:88`) — the loop's live prime pass reclassifies resident ring
    tiles and returns the absent want-list; add an `enforces:` tag on the
    loop-level exercise.
  - `04-transforms-and-infinite-zoom#zoom-speculates-next-rung`
    (registry `:91`) — a two-frame zoom drive supplies a non-zero
    `zoom_direction` and the next rung tiles are reclassified `Speculative`;
    add the `enforces:` tag on the zoom case.

**Runtime behavioral test** (`src/runtime/t/interactive.t.cpp`, Catch2,
`arbc_component_test`):

- **Speculation is counter-neutral.** Drive the assembled loop for two
  frames over a small scene. Assert the wired run's
  `counters().requests_issued` and `counters().composites` deltas equal a
  control run with speculation disabled — the prime pass adds no renders and
  no composites. (Behavioral-counter assertion, never wall-clock.)
- **Prime reclassifies residency.** After a frame, the pan-ring tiles
  adjacent to the visible set are resident under `Adjacent` (and, with a
  non-zero `zoom_direction`, the next-rung tiles under `Speculative`),
  observable through the cache's priority-class query — while
  `cache.evictions()` across the prime pass is unchanged.
- **Still scene schedules no frame.** A still, warm-cache frame with Step 7
  wired schedules no follow-up frame and shows
  `requests_issued/composites/follow_up_frames` deltas of 0 (guards claim
  `:104` survives the wiring).
- **Zoom direction.** A frame whose viewport camera scale increased vs the
  prior frame yields `zoom_direction > 0`; a still camera yields `0`
  (first-frame default `0`).

**Compositor unit assertion** (`src/compositor/t/tile_planning.t.cpp`, the
existing test file):

- **Surfaced plans equal the composited plan.** With `visible_plans`
  non-null, the returned vector holds one entry per visible layer in
  composite order, each carrying exactly the `TileKey`s and `display_source`s
  the frame composited; feeding an entry to `prime_prefetch` yields the same
  want-list a fresh `plan_layer` + `prime_prefetch` would, with **no
  additional cache miss** recorded over the plan pass (no second plan).
- **Null out-parameter is byte-identical.** The same drive with
  `visible_plans == nullptr` yields byte-identical `target` and identical
  `cache.hits()/misses()` deltas — the seam is inert when unused.

**Golden regression (no new golden).** The frame output is unchanged by
surfacing the plan (exposing a value the frame already computed alters no
pixel), so **no new golden is added**; the landed
`tile_planning`/`refinement`/`counters`/`damage_planning`/`anchored_viewports`
goldens are re-run and must stay byte-identical. Deterministic-rendering
policy is satisfied by the unchanged goldens rather than a new tolerance.

**No new WBS leaf / deferred follow-up.** Rendering the `prime_prefetch`
want-list is **already** `compositor.pull_service` (M4,
`tasks/35-compositor.tji:47-52`) — re-asserted, not a new task. This task
registers **no new WBS leaf**; nothing is deferred beyond what an existing
sibling already owns.

**Component wiring & CI dependency check.** `src/runtime/CMakeLists.txt`
DEPENDS is unchanged (`compositor` already present); the component graph
check and the claims bidirectional check (`scripts/check_claims.py`) pass.

**Gate green.** `tj3 project.tjp 2>&1 | grep -iE "error|warning"` is silent
after the closer adds `complete 100`; full build + test suite green before
commit (global rule: always build and test before committing).

## Decisions

1. **Surface the plans through a trailing defaulted out-parameter
   (`std::vector<LayerTilePlan>* visible_plans = nullptr`), not a return
   value or a callback.** This is the exact "add-a-defaulted-pointer" shape
   the four prior compositor seams established (`pending`, `counters`,
   `dirty`, `composition_time`); the null default keeps every landed caller
   and golden byte-unchanged. *Rejected:* changing the `void` return to
   `std::vector<LayerTilePlan>` — churns every existing call site and forces
   goldens/tests to thread a return value they ignore, for no benefit.
   *Rejected:* a `PlanSink`/callback — over-engineered for a single-threaded
   consumer that wants a concrete `const LayerTilePlan&`; `refinement.md`
   already set the precedent of returning/surfacing a plain value rather than
   pushing a sink (`refinement.md:456-464`).

2. **Surface a `std::vector<LayerTilePlan>` (one per visible layer), not a
   single merged plan.** `render_frame_interactive` plans one
   `LayerTilePlan` per visible layer (each with its own `content`, `rung`,
   `local_to_device`), and `prime_prefetch` operates on one plan at a time
   (`const LayerTilePlan&`). The task note's singular "the visible
   LayerTilePlan" is the *set* of visible layers' plans. *Rejected:*
   collapsing to one plan — loses per-layer rung/affine that `prime_prefetch`
   needs to build correct pan/zoom rings.

3. **Move (not copy) each plan into the sink; the sink is a frame-local
   consumed by Step 7 and dropped at frame end.** `LayerTilePlan` is
   move-only (its `PlannedTile`s hold `CacheHold`s), so moving is the only
   option and it is the right one — the plan stays a pure per-frame value and
   no plan state crosses frames (the `tile_planning` rule cited in
   `refinement.md:451`). *Rejected:* retaining the plans as a loop member
   across frames — needlessly extends pin lifetime, invites the plan to
   accrete frame-to-frame state the design forbids, and buys nothing (Step 7
   consumes the plans in the same frame they are produced).

4. **This task both exposes the seam and wires the runtime loop's Step 7.**
   The seam alone would be dead code with no production caller, and the
   deferred Step-7 comment (`interactive.cpp:119-125`) would remain
   undischarged with no other WBS leaf claiming it — leaving the debt to
   fester. Wiring the render-free prime pass now is the M3-safe half of
   speculation (residency warming); rendering the want-list stays M4
   `compositor.pull_service`. *Rejected:* seam-only, defer the wiring — would
   require inventing a separate runtime task for a comment-sized edit,
   leaving `prime_prefetch` uncalled in production and the debt open.

5. **Derive `zoom_direction` from the sign of the frame-over-frame camera
   scale delta; use a fixed one-ring `pan_radius`.** `prime_prefetch`
   explicitly takes a caller-supplied sign ("the compositor infers no
   gesture", `refinement.hpp:95`), and the loop is the only place that sees
   successive viewports. The `Viewport` carries scale inside its `camera`
   `Affine` (`compositor.hpp:15-28`), so the loop retains the prior scale
   magnitude (`d_prev_camera_scale`) and compares — a scalar, gesture-free
   signal sufficient to pick a zoom direction. *Rejected:* a full
   gesture/velocity model — no M3/M4 promise needs it; the sign is enough to
   warm the correct next rung. *Rejected:* hard-coding `zoom_direction = 0`
   (pan ring only) — leaves the zoom ring cold in the live loop even though
   the mechanism exists, wasting the capability `compositor.refinement`
   landed.

**No design-doc delta.** Doc 02 already promises the speculative prefetch
rings (`:92-93`) and the progressive-refinement frame step (`:66-71`), and
doc 04 already promises zoom next-rung speculation. This task *implements the
deferred Step 7* those docs describe — it neither adds a new architectural
seam (the `runtime → compositor` edge exists), a new dependency, nor a
behavior the docs don't already state. No doc is amended.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-06.

- Added `std::vector<LayerTilePlan>* visible_plans = nullptr` trailing out-parameter to `render_frame_interactive` (`src/compositor/arbc/compositor/tile_planning.hpp`); cleared at entry, plan moved in at layer-scope exit when non-null; null path byte-identical.
- Implemented the seam body in `src/compositor/tile_planning.cpp`: sink cleared at entry; each composited `LayerTilePlan` moved into `*visible_plans` after layer composite (bottom-to-top order preserved).
- Wired runtime Step-7 speculation in `src/runtime/interactive.cpp`: frame-local `visible_plans` passed to driver; `zoom_direction` derived from sign of camera scale delta; `prime_prefetch` called per visible plan with `k_pan_prefetch_radius`.
- Added `d_prev_camera_scale` member and `zoom_direction_from_scale_delta` declaration to `src/runtime/arbc/runtime/interactive.hpp`.
- Registered new claim `02-architecture#speculation-drives-from-exposed-plan` in `tests/claims/registry.tsv`.
- Compositor unit tests in `src/compositor/t/tile_planning.t.cpp`: surfaced-plan-equals-composited and null-inert assertions.
- Runtime behavioral tests in `src/runtime/t/interactive.t.cpp`: zoom-direction, counter-neutral drives-from-plan, pan-ring reclassify, zoom next-rung reclassify (4 cases); `enforces:` tags re-asserted for `prefetch-ring-classifies-resident-reports-absent` and `04-transforms-and-infinite-zoom#zoom-speculates-next-rung`.
