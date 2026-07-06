# compositor.damage_planning — Damage-driven frames

## TaskJuggler entry

`tasks/35-compositor.tji:22-27` → `compositor.damage_planning` ("Damage-driven
frames"), the third leaf under `task compositor`. It carries
`depends !tile_planning, model.damage, cache.invalidation`
(`35-compositor.tji:25`) and, through the parent `task compositor`, inherits
`depends contract.async_render, cache.key_shapes, color.resampling`
(`35-compositor.tji:7`). Note line:

> "No damage, no work: map model damage to dirty device regions per viewport;
> clock advance as the temporal damage axis. Docs 02/11."

## Effort estimate

**2d.** The same weight as the `refinement` leaf, and for the same reason: this
task builds no new rendering pipeline — it turns the *already-produced* `Damage`
stream (model-emitted via `model.damage`, refinement-emitted via
`poll_refinements`) into three pure, caller-owned functions plus one optional
driver seam, all over machinery the predecessors landed. The deliverable is
(1) **`map_damage_to_device`** — project each `Damage{content, region, range}`
through the per-viewport camera to device-pixel dirty rects, temporally gated to
the displayed instant and viewport-clipped (doc 02:51,57-60); (2)
**`clock_advance_damage`** — turn a clock advance into homogeneous `Damage` for
the visible **non-`Static`** layers only, the "clock advance is the temporal
damage" axis (doc 11:133-137); (3) **`invalidate_damage`** — the compositor-side
driver that binds `cache::invalidate_region`'s caller-injected `Geom` to the tile
grid and drops the damaged `(content, region)` tiles across all rungs
(doc 02:94-95); and (4) a **`const DirtyRegion*` seam** on
`render_frame_interactive` that, when supplied, plans and composites only tiles
intersecting the dirty region — realizing "no damage → no work" as a *behavioral*
property (empty region ⇒ zero renders, zero composites). Plus unit tests on each
function, one byte-exact damaged-region golden, three claims-register entries
(two new, one re-asserted), and the CMake wiring. It reuses everything
tile_planning/refinement/anchored_viewports/model.damage/cache.invalidation
shipped; no new capability is built, only their composition into a damage planner.

## Inherited dependencies

**Settled:**

- **`compositor.tile_planning`** (DONE 2026-07-05,
  [`tile_planning.md`](tile_planning.md)) — the direct predecessor
  (`depends !tile_planning`). It delivered the interactive planner and driver this
  task gates, in `src/compositor/arbc/compositor/tile_planning.hpp`
  (+ `tile_planning.cpp`):
  - **`void render_frame_interactive(const DocRoot&, const ContentResolver&,
    const Viewport&, TileCache&, Backend&, SurfacePool&, Surface& target,
    Deadline, std::optional<std::uint64_t> prior_revision,
    RefinementQueue* pending = nullptr, CompositorCounters* counters = nullptr)`**
    (`tile_planning.hpp:162`, impl `tile_planning.cpp:200`) — the synchronous
    tiled driver. It walks `state.for_each_layer`, culls, composes
    `compose(viewport.camera, layer.transform)`, maps the **whole** device
    viewport rect `Rect::from_size(viewport.width, viewport.height)`
    (`tile_planning.cpp:208`) back to layer-local via `inverse()->map_rect`, calls
    `select_rung`, then `plan_layer` and fills misses inline
    (`tile_planning.cpp:214-288`). **There is no dirty-region gating today — every
    frame plans the entire viewport.** Closing that is the core of this task; the
    two trailing-optional parameters (`pending`, `counters`) are the exact
    add-a-defaulted-pointer shape this task's `DirtyRegion*` seam copies.
  - **`LayerTilePlan plan_layer(TileCache&, ObjectId content, std::uint64_t
    revision, std::optional<std::uint64_t> prior_revision, const RungSelection&,
    const Rect& local_region, const Affine& local_to_device, Stability, Time,
    StateHandle, Deadline)`** (`tile_planning.hpp:128`, impl `tile_planning.cpp:114`)
    — the pure planner. The dirty-region seam narrows the `local_region` this
    function is called with; the planner itself is untouched and stays
    allocation-free/lock-free. It already consumes the layer's **`Stability`** —
    the value `clock_advance_damage` reads to decide whether a clock advance
    damages a layer.
  - **`std::vector<TileCoord> tiles_covering(ScaleRung, const Rect&)`**
    (`tile_planning.hpp:69`, `.cpp:81`) and **`Rect tile_local_rect(ScaleRung,
    TileCoord)`** (`tile_planning.hpp:75`, `.cpp:107`) — the tile-grid geometry.
    `tile_local_rect` is exactly the `Geom` (`Rect(ScaleRung, TileCoord)`) that
    `cache::invalidate_region` was authored to take injected; `invalidate_damage`
    binds it.
- **`model.damage`** (DONE 2026-07-06,
  [`../model/damage.md`](../model/damage.md)) — the type this task consumes. Live
  source `src/model/arbc/model/damage.hpp`:
  - **`struct Damage { ObjectId object; Rect rect; TimeRange range; }`** (`:19`) —
    the well-typed damage record. `rect` is content-local; `range` is a half-open
    `[start, end)` in flicks. Helpers `rect_union` (`:33`), `range_union` (`:48`),
    `damage_add` (unions by object id, `:64`), and the abstract
    `class DamageSink { virtual void flush(const std::vector<Damage>&) = 0; }`
    (`:79`).
  - Settled damage taxonomy this task must map faithfully: **structural/placement**
    damage is emitted by the model as one `Damage{edited_id, Rect::infinite(),
    TimeRange::all()}` — whole object, all time (the model is L2 and cannot call
    `Content::bounds()`/`time_extent()`, so it soundly over-approximates and leaves
    the *up-nesting* to the compositor). **Content** damage is caller-supplied by
    the kind with a precise `(region, range)`. **infinite/all** is encoded as
    `Rect::infinite()` / `TimeRange::all()`, **absorbing under union**. This task
    is a *consumer* of `Damage` — it maps and invalidates; it is not a
    `DamageSink`.
- **`cache.invalidation`** (DONE 2026-07-05,
  [`../cache/invalidation.md`](../cache/invalidation.md)) — the invalidation API
  this task drives. Live source `src/cache/arbc/cache/invalidation.hpp`
  (header-only, `arbc::cache`, **no `model` edge — damage geometry is
  caller-injected**):
  - **`template <class Geom> std::size_t invalidate_region(TileCache&, ObjectId
    content, const Rect& region, Geom&& tile_rect)`** (`:48`) — drops every tile of
    `content` whose footprint intersects `region`, **across all rungs, revisions,
    and achieved-times** (a sound over-approximation; doc 02:94-95). `Geom` is
    `Rect(ScaleRung, TileCoord)`. `invalidation.md` explicitly deferred **the
    compositor-side binding of `Geom` and the model-damage → invalidate driver to
    `damage_planning`** — this task is that driver.
  - **`std::size_t invalidate_content(TileCache&, ObjectId)`** (`:62`) — wholesale
    drop; the natural target for structural `Rect::infinite()` damage.
  - **`std::size_t drop_superseded(TileCache&, ObjectId, std::uint64_t
    live_revision)`** (`:78`) — opt-in eager reclaim of prior-revision tiles;
    **runtime-driven, not this task's** (revision invalidation is lazy-by-keying,
    see Decision 5). All three are thin wrappers over
    `KeyedStore::remove_if(Pred)` (`keyed_store.hpp:189`).
  - Settled invalidation semantics: damage invalidates by `(content id, region)`
    across all rungs; **revision bumps invalidate wholesale but lazily** — a bumped
    `revision` yields fresh keys and old keys survive as stale fallback until LRU
    reclaims them (no op fires on the bump hot path). Pin-defer: invalidating a
    pinned tile makes the key unreachable but defers the surface drop to last
    unpin; `evictions()` counts only budget evictions, not invalidations.
- **`compositor.refinement`** (DONE 2026-07-05, [`refinement.md`](refinement.md))
  — the *other* `Damage` producer this task's device mapping must consume. Live
  source `src/compositor/arbc/compositor/refinement.hpp`:
  - **`std::vector<Damage> poll_refinements(RefinementQueue&, TileCache&,
    CompositorCounters* = nullptr)`** (`refinement.hpp:139`, impl
    `refinement.cpp:67`) — emits `Damage{content, tile_local_rect(rung, coord),
    TimeRange{when, when}}` per settled arrival (`refinement.cpp:92`), where
    `when = pending.key.achieved_time.value_or(Time::zero())`. The header
    **explicitly defers device mapping here** (`refinement.hpp:129`,
    `tile_planning.cpp:131`: "device mapping is `damage_planning`'s"). Caveat:
    `TimeRange{when, when}` is **empty** under `TimeRange::empty()` (`end <=
    start`); the temporal gate must treat a degenerate range as *present-frame*
    damage, not *no-time* damage (Decision 3).
- **`compositor.counters`** (DONE 2026-07-06, [`counters.md`](counters.md)) — the
  behavioral-counter surface that pins this task's "no work" claim. Live source
  `src/compositor/arbc/compositor/counters.hpp`: **`CompositorCounters`** (`:34`,
  fields `requests_issued`/`composites`/`follow_up_frames`) and
  `counters_snapshot` (`:74`). The empty-`DirtyRegion` case asserts
  `requests_issued == 0` **and** `composites == 0` through this surface.
- **`compositor.anchored_viewports`** (DONE 2026-07-06,
  [`anchored_viewports.md`](anchored_viewports.md)) — the camera/cull model the
  device mapping composes through. `struct Viewport { int width, height; Affine
  camera; ObjectId anchor; }` (`compositor.hpp:15`); `camera` maps **anchor-local
  → device pixels**; `k_root_anchor` (`anchored_viewports.hpp:37`) reproduces flat
  behavior. `cull_walk` (`anchored_viewports.hpp:126`) is the viewport-outward,
  transform-composing walk this task reuses to locate a damaged object and compose
  its local→device transform; `render_frame_anchored` (`:133`).

**Pending:** none — every predecessor is landed.

## What this task is

Doc 02's interactive frame opens with **"Collect damage … No damage → no work"**
(`02:51`) — the frame does work only for what changed. `tile_planning` landed the
plan/render/composite steps but plans the *whole* viewport every frame; the
damage axis is absent. This task supplies it, in a new header/impl
`src/compositor/arbc/compositor/damage_planning.hpp` (+ `damage_planning.cpp`)
plus one seam on the existing driver:

1. **Spatial mapping — `std::vector<Rect> map_damage_to_device(const DocRoot&,
   const Viewport&, std::span<const Damage>, Time now)`.** For each `Damage`:
   (a) **temporal gate** — skip unless `range.empty() || range.contains(now)`, so
   an edit that only affects a non-displayed instant contributes nothing
   (Decision 3); (b) locate `object` in the composition and compose its
   local→device transform via the anchor walk (`cull_walk` machinery,
   `compose(viewport.camera, transform_anchor→object)`); (c) map the content-local
   `rect` to device via `Affine::map_rect`, intersect with `Rect::from_size(width,
   height)`, sub-pixel-cull; push the non-empty device rect. An
   `Rect::infinite()` content rect (structural damage) clips to the object's
   device-projected bounds ∩ viewport (in practice the full viewport rect —
   conservative and correct). **Empty input, or all-gated/culled, returns an empty
   vector** — the concrete "no damage → no work" (doc 02:51). Consumes both
   model-emitted and `poll_refinements`-emitted `Damage` through the one path.

2. **Temporal axis — `std::vector<Damage> clock_advance_damage(const DocRoot&,
   const ContentResolver&, const Viewport&, const TimeRange& advanced)`.** Walk the
   visible layers (the same cull walk the driver uses); for each layer whose
   resolved `Stability` is **not `Static`** (i.e. `Timed`/`Live`), emit
   `Damage{layer, Rect::infinite(), advanced}`; `Static` layers emit **nothing**.
   The result is homogeneous with model damage, so `map_damage_to_device` consumes
   it directly. This is doc 11:133-137's "**Clock advance is the temporal
   damage** … `Static` layers' cached tiles remain valid and playback over a
   mostly-still scene re-renders only the moving layers." An advance over an
   **all-`Static`** scene (or an empty `advanced` range) returns empty — playback
   of a still scene produces no damage, so the runtime schedules no frame
   (doc 16:54-62 behavioral floor).

3. **Cache invalidation — `std::size_t invalidate_damage(TileCache&,
   std::span<const Damage>)`.** For each `Damage{content, region, range}`, drive
   `cache::invalidate_region(cache, content, region, tile_local_rect)` — binding
   the injected `Geom` to the tile grid — dropping the damaged tiles across all
   rungs (doc 02:94-95); a `Rect::infinite()` region routes to
   `invalidate_content` (Decision 6). Returns the total tiles dropped. This is the
   compositor-side binding `cache.invalidation` reserved for this task.

4. **Frame gating — `const DirtyRegion* dirty = nullptr` on
   `render_frame_interactive`.** A new trailing-optional parameter
   (`struct DirtyRegion { std::vector<Rect> device_rects; }`, defined in
   `damage_planning.hpp`, forward-declared in `tile_planning.hpp` exactly as
   `RefinementQueue` is). When **null** (the default), the driver plans the whole
   viewport — **byte-identical** to today (the `tile_planning`/`refinement`/
   `anchored_viewports` goldens are the guard). When **non-null**, each visible
   layer's planned `local_region` is intersected with the dirty region mapped into
   that layer's local space, so only tiles intersecting a device dirty rect are
   planned, rendered, and composited onto the (caller-persisted) `target`. A
   **non-null but empty** `DirtyRegion` plans nothing: **zero renders, zero
   composites** — the behavioral realization of "no damage → no work."

**Not this task:**

- **The follow-up-frame scheduler / the frame loop that re-invokes on damage** →
  `runtime.interactive` (doc 17:60). This task produces the device dirty regions,
  the temporal damage, and the invalidation, and gates a single driver pass; it
  owns no persistent camera/clock state and no loop. Every function is pure and
  caller-owned, exactly as `poll_refinements` returns a vector rather than
  scheduling (the boundary `refinement.md` drew).
- **Recursive up-nesting of damage through composite content (doc 05):** mapping a
  *child's* damage up through a containing composite via `inputs()` /
  `map_input_damage` → `compositor.operator_graph`
  (`35-compositor.tji:28-32`), which owns those seams and is not yet built. This
  task maps damage for objects that resolve to a **visible layer**; damage keyed to
  an object nested inside composite content is routed by `operator_graph` when it
  lands. Existing WBS leaf, no new task.
- **Sub-frame `achieved_time` coalescing / bucketing for `Timed` content**
  (doc 11:111-114 — a 24fps clip serving every request in `[7/24, 8/24)` from one
  entry, so a within-one-frame-period clock advance issues zero renders) →
  deferred to a **named new task `compositor.temporal_coalescing`** (see
  Acceptance criteria). This task delivers the temporal *axis* (moving layers
  dirty, still layers not); the sub-frame *coalescing optimization* needs the
  content's reported temporal grid and is separable (Decision 7).
- **Revision-bump eager reclaim (`drop_superseded`)** → runtime-driven; revision
  invalidation is lazy-by-keying by `cache.invalidation`'s contract (Decision 5).
- **Any change to the offline `render_frame`** (no cache, renders every tile) and
  to the **null-`dirty` path** of `render_frame_interactive` (unchanged for the
  landed goldens).

## Why it needs to be done

Doc 02's frame is damage-driven from its first sentence — "**Collect damage.**
Since the last frame: content damage, placement changes, camera changes. No
damage → no work" (`02:51-52`). Today the interactive driver ignores that: it
re-plans the entire viewport every frame (`tile_planning.cpp:208`), so a scene
that changed in one small region still walks and composites every tile, and a
still scene during playback still runs a full frame. Two damage streams already
exist with **no consumer that maps them to device work**: the model emits
`Damage` on every edit (`model.damage`), and `poll_refinements` emits `Damage` on
every async tile arrival (`refinement.cpp:92`) — both are dropped on the floor
because nothing turns `(content, region, range)` into device dirty rects or into
cache invalidations. And doc 11's promise that makes playback cheap — "**Clock
advance is the temporal damage** … playback over a mostly-still scene re-renders
only the moving layers" (`11:133-137`) — has no mechanism: a clock tick is
indistinguishable from a full re-render.

This task closes all three: it maps damage to per-viewport device dirty regions,
drives the cache invalidation `cache.invalidation` was built to receive, and makes
clock advance a first-class damage source that is empty for still scenes. Until it
lands, "no damage, no work" is unrealized — every frame is a full frame — and
`runtime.interactive` (which will consume the dirty regions and the temporal
damage to decide *whether and where* to run a frame) has no plan value to hang its
scheduling on.

## Inputs / context

- `docs/design/02-architecture.md`:
  - **`:49-72` — The frame, interactively.** `:51-52` step 1 "Collect damage …
    content damage, placement changes, camera changes. **No damage → no work**" —
    the governing invariant. `:53-56` cull (walk from the anchor, compose
    transforms, intersect bounds with the visible region). `:57-61` step 3 "map the
    visible region into layer-local space … split into tiles" — the model↔device
    mapping this task inverts for damage. `:69-71` step 6 "Refine": async results
    "produce damage for their region, scheduling a follow-up frame" — the
    refinement-emitted damage this task's device mapping consumes.
  - **`:87-99` — Tile cache invalidation.** `:94-95` (load-bearing): "Damage
    invalidates by `(content id, region)` across all rungs; revision bumps
    invalidate wholesale by making old keys unreachable." — the exact shape and
    scope of `invalidate_damage` and Decision 5's lazy-revision rule. `:89` the key
    `(content id, revision, scale rung, tile coords)`.
  - `:119-137` threading — frame planning reads a pinned document version so it
    "never races edits and never takes a lock" (`:125-127`); damage is collected
    against that immutable snapshot.
- `docs/design/11-time-and-video.md`:
  - **`:15-25` — the space↔time symmetry table.** `:24` "Viewport camera (anchor +
    matrix) ⟷ Viewport transport (clock)" — the two orthogonal axes this task
    joins. `:20` extents "may be infinite" — the nearest doc anchor for `all`/
    infinite temporal extent.
  - **`:72-79` — the `Stability` enum.** `Static` (time-independent — all current
    still kinds), `Timed` (deterministic function of time — cacheable per time),
    `Live` (non-deterministic — cacheable only within a snapshot). This is what
    `clock_advance_damage` reads: `Static` ⇒ no temporal damage; `Timed`/`Live` ⇒
    damaged on advance.
  - **`:128-137` — Pipeline changes / the temporal damage axis (central).**
    `:129-131` "Frame planning samples the transport's current composition time,
    then computes each layer's local time by composing time maps down the tree."
    **`:133-137`** "**Clock advance is the temporal damage.** Advancing time
    invalidates nothing spatial: `Static` layers' cached tiles remain valid and
    playback over a mostly-still scene re-renders only the moving layers. Spatial
    damage (content edits) and temporal advance are orthogonal invalidation axes;
    both funnel into the same 'what must re-render this frame' plan." — the
    governing text for functions 2 and the device-region merge.
  - `:111-114`/`:138-149` — `achieved_time` coalescing and the time-keyed cache
    (`(…, achieved_time)`, `nullopt` for `Static`): cited here to **scope out** the
    sub-frame coalescing optimization to `compositor.temporal_coalescing`.
- `docs/design/16-sdlc-and-quality.md`: `:14-25` claims register
  (`<doc-file-stem>#<slug>`, `// enforces:` tag); `:47-53` byte-exact CPU goldens
  (the damaged-region golden matches a full re-render byte-for-byte); `:54-62`
  behavioral-counter tests ("playback of a still scene issues zero visual renders …
  Most claims-register entries about efficiency land here") — the empty-`DirtyRegion`
  zero-work assertion; `:112-118` ≥90% diff coverage on changed lines.
- `docs/design/17-internal-components.md`: `:56` `arbc::compositor` is **Level 4**,
  `Depends on: contract, cache (+ below)` — `model::Damage` is reached through the
  same transitive `model` visibility `render_frame_interactive` already uses for
  `DocRoot`; `cache::invalidate_region` is part of the `cache` component. **No new
  `DEPENDS` edge, no `backend-cpu` edge.** `:40-44` depend only on strictly lower
  levels; the doc-17 dependency check must stay green.
- `src/compositor/arbc/compositor/tile_planning.hpp` — `render_frame_interactive`
  (`:162`), `plan_layer` (`:128`), `tiles_covering` (`:69`), `tile_local_rect`
  (`:75`); `src/compositor/tile_planning.cpp` — whole-viewport plan at `:208`,
  cull/compose/fill loop `:214-288`, `DocRoot::revision()` read `:207`, the
  "device mapping is `damage_planning`'s" note `:131`.
- `src/compositor/arbc/compositor/compositor.hpp:15` — `Viewport`;
  `src/compositor/arbc/compositor/anchored_viewports.hpp` — `k_root_anchor`
  (`:37`), `cull_walk` (`:126`), `render_frame_anchored` (`:133`).
- `src/base/arbc/base/transform.hpp` — `Affine` (`:12`: `map_rect`, `inverse`,
  `max_scale`), `compose(outer, inner)` (`:43`).
- `src/base/arbc/base/geometry.hpp:15` — `Rect { double x0,y0,x1,y1; }`,
  `empty()`, `intersect()`, `from_size`, `Rect::infinite()`.
- `src/base/arbc/base/time.hpp` — `TimeRange { Time start, end; }` (`:29`),
  `TimeRange::all()` (`:38`), `empty()` (`:45`), `contains(Time)` (`:49`);
  `Time { std::int64_t flicks; }` (`:11`, `Time::zero()`).
- `src/model/arbc/model/damage.hpp` — `Damage` (`:19`), `rect_union` (`:33`),
  `range_union` (`:48`), `damage_add` (`:64`), `DamageSink` (`:79`).
- `src/model/arbc/model/model.hpp:44` — `DocRoot::revision()`.
- `src/cache/arbc/cache/invalidation.hpp` — `invalidate_region` (`:48`),
  `invalidate_content` (`:62`), `drop_superseded` (`:78`);
  `src/cache/arbc/cache/keyed_store.hpp:189` — `remove_if`.
- `src/compositor/arbc/compositor/refinement.hpp:139` / `refinement.cpp:67,92` —
  `poll_refinements`, the content-local `Damage` emission with `TimeRange{when,
  when}`.
- `src/compositor/arbc/compositor/counters.hpp:34,74` — `CompositorCounters`,
  `counters_snapshot`.
- `tests/claims/registry.tsv` — 2-column TAB-separated `<claim-id>\t<description>`,
  `<doc-file-stem>#<slug>`; enforced by `scripts/check_claims.py` (bidirectional).
  Existing rows to **re-assert, not re-register**:
  `02-architecture#damage-invalidates-by-content-region-across-rungs` (registered
  by `cache.invalidation`), `11-time-and-video#static-tiles-survive-clock`
  (registered by `tile_planning`), `16-sdlc-and-quality#byte-exact-goldens`.
- Test conventions: unit tests `src/compositor/t/<name>.t.cpp` (Catch2,
  `arbc_component_test`); cross-component goldens `tests/<name>_golden.t.cpp`
  (links `arbc` + `CpuBackend`). Enforce-tag example:
  `src/compositor/t/tile_planning.t.cpp:142` (`// enforces:
  02-architecture#miss-becomes-deadline-request`).

## Constraints / requirements

- **Levelization (doc 17:40-44,:56).** L4 `arbc::compositor`, using only
  `contract`, `cache` (incl. `cache/invalidation.hpp`), and their transitive
  closure (`model` for `Damage`, reached exactly as `DocRoot` already is;
  `surface`, `pool`, `base`). Composite reached only through the abstract L2
  `surface::Backend`. **`DEPENDS contract cache` is unchanged** (no new listed
  edge); the CI dependency check stays green.
- **Pure per-frame library; all state caller-owned (compositor is stateless).**
  `map_damage_to_device`, `clock_advance_damage`, and `invalidate_damage` are pure
  free functions taking their inputs by value/const-ref; the `DirtyRegion` is a
  caller-owned value threaded by pointer, exactly like `RefinementQueue*`/
  `CompositorCounters*`. No cross-frame damage accumulation lives in the
  compositor; the "last frame's clock/camera" is the runtime loop's.
- **Zero behavior change when null (byte-exact).** With `dirty == nullptr` (the
  default), `render_frame_interactive` produces byte-identical output and identical
  cache state to today. The landed `tile_planning`, `refinement`, and
  `anchored_viewports` goldens must pass unchanged.
- **"No damage → no work" is total (doc 02:51).** Empty damage in ⇒ empty device
  region out (`map_damage_to_device`); an all-`Static` scene or empty advance ⇒
  empty temporal damage (`clock_advance_damage`); a non-null **empty**
  `DirtyRegion` ⇒ zero renders and zero composites in the driver. No busy path
  does work for a quiescent frame.
- **Damage mapping is faithful to the model's over-approximation.** Structural
  damage's `Rect::infinite()`/`TimeRange::all()` maps conservatively (whole
  viewport / always-in-range); precise content damage maps precisely. The temporal
  gate is `range.empty() || range.contains(now)` — a degenerate/instant range (the
  refinement-arrival case) is present-frame damage, not no-time damage
  (Decision 3).
- **Invalidation is sound over-approximation across all rungs (doc 02:94-95).**
  `invalidate_damage` drops every tile of `content` whose footprint intersects
  `region`, revision- and `achieved_time`-agnostic, matching
  `cache::invalidate_region`'s contract. Pin-defer and the `evictions()`-unchanged
  property carry through (`cache.invalidation`).
- **Single-threaded; no concurrency surface added.** All functions run on the
  frame thread against a pinned snapshot; no atomics, no locks. **No TSan
  obligation** (the async completion's thread-safety is `contract`'s; the worker
  dispatch is `pull_service`'s).
- **CI diff coverage ≥90% (doc 16:112-118)** on changed lines — tests ship in this
  task.

## Acceptance criteria

**Claims (register + `enforces:` tags)** — two new rows appended to
`tests/claims/registry.tsv`, enforced from the tests below; one existing row
re-asserted (not re-registered):

- **`02-architecture#damage-maps-to-device-dirty-regions`** (new) — "Model damage
  `(content, region, range)` maps through the per-viewport camera to device dirty
  rects: damage outside the displayed instant or the viewport contributes none,
  structural infinite damage maps to the full viewport, and empty damage yields an
  empty dirty region — no damage, no work." (doc 02:51,57-60.) Enforced by
  `src/compositor/t/damage_planning.t.cpp`.
- **`11-time-and-video#clock-advance-damages-only-moving-layers`** (new) — "A clock
  advance emits temporal damage only for visible non-`Static` layers; an advance
  over an all-`Static` scene emits zero damage, so a still scene during playback
  schedules no frame and the interactive driver issues zero renders and zero
  composites." (doc 11:133-137; doc 16:54-62 — behavioral, never wall-clock.)
  Enforced by `src/compositor/t/damage_planning.t.cpp` (value + counter
  assertions).
- **Re-asserted (no new row):** `02-architecture#damage-invalidates-by-content-region-across-rungs`
  (registered by `cache.invalidation`) gains a second enforcing test — the
  compositor-side `invalidate_damage` driver — via its `// enforces:` tag; and
  `11-time-and-video#static-tiles-survive-clock` (registered by `tile_planning`)
  is re-exercised by the still-scene clock-advance case. A claim may carry more
  than one enforcing test.

**Behavioral / unit test — `src/compositor/t/damage_planning.t.cpp`** (new, Catch2,
`arbc_component_test`):

- **`map_damage_to_device`:** a `Damage` in a layer's local space maps through an
  identity camera to the expected device rect, and through a translated/scaled
  camera to the correspondingly transformed rect; damage wholly outside
  `Rect::from_size(w,h)` clips to empty and is dropped; `Rect::infinite()`
  structural damage yields the full-viewport rect; **empty input → empty output**;
  temporal gate — a `Damage` whose `range` excludes `now` is dropped, a degenerate
  `TimeRange{when, when}` (refinement-arrival shape) is **kept** as present-frame
  damage.
- **`clock_advance_damage`:** a scene with one `Static` and one `Timed` layer over
  a non-empty `advanced` range emits exactly one `Damage` (the `Timed` layer,
  `range == advanced`); an **all-`Static`** scene emits **empty**; an empty
  `advanced` range emits empty.
- **`invalidate_damage`:** a hand-populated `TileCache` holding tiles of `content`
  at several rungs intersecting `region` drops exactly those (return count matches;
  a follow-up `lookup` misses), while tiles of another content or outside `region`
  survive; a `Rect::infinite()` region drops all of `content`'s tiles
  (`invalidate_content` path); `evictions()` is unchanged across the call (drops
  are not budget evictions).
- **Driver gating (counter-backed, doc 16:54-62):** driving
  `render_frame_interactive` with a `DirtyRegion` covering one tile's device rect
  bumps `requests_issued`/`composites` for **only** that tile (a `CompositorCounters`
  bound); with a **non-null empty** `DirtyRegion`, `requests_issued == 0` **and**
  `composites == 0`; with `dirty == nullptr`, the frame plans the whole viewport
  (counts equal the ungated run).

**Golden — `tests/damage_planning_golden.t.cpp`** (new, cross-component, links
`arbc` + `CpuBackend`; byte-exact, doc 16:47-53):

- **Damaged-region re-render is byte-identical to a full re-render.** Render a
  two-layer scene fully into a persisted `target`; damage one layer's region;
  re-render *gated* to that layer's device dirty rect (from `map_damage_to_device`).
  The resulting `target` is **byte-identical** to a full `render_frame_interactive`
  of the post-damage scene — proving the gated frame composites the damaged tiles
  identically and the untouched region survives from the previous frame and the
  warm cache, with no seam or double-blend.
- **Quiescent frame does nothing.** With no damage (empty `DirtyRegion`) the target
  is **unchanged** byte-for-byte and the counters show zero renders / zero
  composites (doc 02:51; the behavioral class, doc 16:54-62).

**Golden regression (no new golden):** the existing `tile_planning`, `refinement`,
and `anchored_viewports` goldens pass unchanged, confirming the null-`dirty` path
is byte-exact (`16-sdlc-and-quality#byte-exact-goldens`,
`tests/claims/registry.tsv` — a meta-claim this task does not re-register).

**Deferred follow-up — named new WBS leaf (closer registers):**

- **`compositor.temporal_coalescing`** — effort **2d**, `allocate team`,
  `depends !damage_planning, cache.key_shapes`, wired into the **same milestone as
  `compositor.damage_planning`**. One-line: *"Quantize a `Timed` layer's requested
  `Time` to the content's reported temporal grid (`achieved_time` bucketing,
  doc 11:111-114) so a clock advance within one frame period reuses the cached
  tile and issues zero renders — the temporal analog of no-damage-no-work."* Note:
  `Docs 11. Refinement: tasks/refinements/compositor/temporal_coalescing.md`.
  Concrete and agent-implementable: a quantization kernel keyed off the content's
  `RenderResult::achieved_time`, a keying change, and a behavioral test that a
  sub-frame-period advance bumps `requests_issued` by 0. This re-scopes the
  `achieved_time` bucketing that `key_shapes.md`/`tile_planning.md` provisionally
  routed to `damage_planning` (Decision 7). Deferred to `compositor.temporal_coalescing`
  (closer registers in WBS).

## Decisions

1. **Three pure functions + one optional driver seam, not a `DamageSink`
   consumer.** Doc 02:69 and `model.damage` model damage as *produced* values
   (the model flushes to a `DamageSink`; `poll_refinements` returns a vector). This
   task is the *device-side* consumer, and it returns device rects / drives the
   cache the same value-oriented way — it does **not** subscribe as a
   `model::DamageSink`. Rationale: coupling L4 to the sink virtual would bury the
   scheduling decision that is `runtime.interactive`'s, and the runtime already
   owns *when* to collect damage and run a frame. Returning plain values keeps each
   function unit-testable with hand-built damage and no backend, mirroring the
   boundary `refinement.md` drew ("returns the vector rather than pushing a sink").
   *Rejected:* a compositor `DamageSink` implementation that eagerly maps/invalidates
   on flush — hidden cross-frame state at L4 and a coupling to model's virtual, for
   no gain over the runtime calling these functions.

2. **The dirty region is device-space, mapped into each layer's local space at
   plan time.** Damage arrives content-local, but the frame's unit of work is the
   device viewport (culling, tiling, compositing are all device-driven), and one
   device dirty region serves *all* layers uniformly. The driver already computes
   each layer's `inverse()` to map the device rect to local (`tile_planning.cpp`
   maps the viewport rect this way); the seam reuses that inverse to intersect the
   dirty region with each layer's `local_region`. *Rejected:* a per-layer
   local-space dirty set — would require the caller to pre-partition damage by
   layer, duplicating the compose/inverse walk the driver already does, and does
   not compose across layers that share a damaged screen region.

3. **Temporal gate is `range.empty() || range.contains(now)`; a degenerate range
   is present-frame damage.** Content damage carries a real `range`; structural
   damage carries `TimeRange::all()` (always in range); but `poll_refinements`
   emits `TimeRange{when, when}`, which is **empty** under `TimeRange::empty()`
   (`time.hpp:45`). Treating "empty range" as "no time" would silently drop every
   async-arrival damage. So an empty range is read as *this instant* (present-frame)
   and always maps. Rationale: an arrival is a tile that must be shown *now*;
   dropping it defeats progressive refinement. This keeps the landed
   `poll_refinements` **unchanged** — no edit to a shipped task. *Rejected:*
   widening `poll_refinements`'s emission to a single-frame span — churns a landed,
   golden-guarded task for a case the gate handles cleanly at the consumer, and the
   compositor has no frame-duration to widen by (that is the runtime's clock).
   *Rejected:* a separate device-mapping entry for refinement damage — two code
   paths for one homogeneous type.

4. **`clock_advance_damage` emits per-layer damage from `Stability`, keyed on the
   layer id, `Rect::infinite()` footprint.** Doc 11:133-137 makes clock advance
   damage *only* the moving layers; `Stability` (doc 11:72-79) is exactly that
   predicate and is already resolved on the plan path (`plan_layer` takes it). A
   `Static` layer emits nothing (its keys are `achieved_time == nullopt`,
   clock-invariant — re-asserting `11-time-and-video#static-tiles-survive-clock`);
   a `Timed`/`Live` layer emits `Damage{layer, Rect::infinite(), advanced}` so the
   device mapping dirties its whole visible footprint. Using `Rect::infinite()`
   (clipped to the layer's device bounds ∩ viewport) is the sound
   over-approximation — the compositor does not know *which sub-region* of a moving
   layer changed between two times. *Rejected:* emitting a precise moved-region —
   requires per-frame content diffing the content contract does not expose;
   over-approximation to the layer footprint is correct and cheap. *Rejected:*
   folding the temporal decision into the keying alone (letting `Timed` keys miss
   naturally) — that re-renders moving layers but gives the runtime no *signal* to
   skip a frame for an all-`Static` scene, which is the whole point of the axis.

5. **Revision invalidation stays lazy-by-keying; `invalidate_damage` handles only
   the spatial `(content, region)` axis.** `cache.invalidation` settled that a
   revision bump invalidates wholesale *lazily* — new keys are fresh, old keys
   survive as stale fallback until LRU reclaims them, and **no op fires on the bump
   hot path** (doc 02:94-95). `drop_superseded` is the opt-in eager reclaim, and it
   is runtime-driven (the runtime knows when a prior revision is no longer needed as
   fallback). So this task's invalidation driver covers only spatial damage; it does
   not call `drop_superseded`. *Rejected:* eagerly dropping superseded revisions on
   every damage pass — defeats the stale-fallback tier `tile_planning`'s degradation
   order depends on, and duplicates the runtime's reclaim policy.

6. **Structural `Rect::infinite()` region routes to `invalidate_content`.** An
   infinite region intersects every tile, so `invalidate_region` with
   `Rect::infinite()` would already drop all of `content`'s tiles — but routing it
   explicitly to `invalidate_content` (`invalidation.hpp:62`) is clearer and avoids
   an all-pairs geometry test that always passes. A finite region uses
   `invalidate_region` with the `tile_local_rect` `Geom`. *Rejected:* always using
   `invalidate_region` — correct but wasteful for the common structural-damage case
   (every placement edit emits infinite damage).

7. **Sub-frame `achieved_time` coalescing is a separable follow-up
   (`compositor.temporal_coalescing`), not folded here.** `key_shapes.md` and
   `tile_planning.md` provisionally routed "`achieved_time` bucketing" to
   `damage_planning`. On refinement it is a distinct concern: this task delivers the
   temporal *axis* (which layers a clock advance damages), which is fully realizable
   in 2d from `Stability`; sub-frame *coalescing* (quantizing a `Timed` layer's
   requested time to its frame grid so within-period advances reuse a cached tile,
   doc 11:111-114) needs the content's *reported* temporal grid
   (`RenderResult::achieved_time`) and is an optimization *within* the moving-layer
   set with its own keying change and behavioral test. Splitting it keeps each task
   at its estimate and gives coalescing an honest home. It is a **concrete,
   agent-implementable** leaf (a quantization kernel + keying + a zero-render
   behavioral test), named crisply above so the closer registers it — not an
   "audit"/"revisit". *Rejected:* folding it in — blows the 2d estimate and couples
   the damage-mapping deliverable to a temporal-quantization mechanism that stands
   on its own.

8. **No design-doc delta.** Every rule here is settled doc text: "no damage → no
   work" and the three damage sources (doc 02:51), map-region-to-layer-local
   (doc 02:57-60), damage invalidates by `(content, region)` across rungs
   (doc 02:94-95), clock advance is the temporal damage with orthogonal axes
   (doc 11:133-137), and `Stability` (doc 11:72-79). The `Damage` taxonomy
   (structural/content/value, infinite/all, `TimeRange`) is already authored by
   `model.damage` in its governing doc; this task consumes it unchanged. The
   `contract, cache` edge (doc 17:56) is unchanged and `cache.invalidation`'s
   facilities are landed. This task *concretizes* those promises into C++ without
   altering designed behavior — no doc edit, no doc-00 bullet.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-06.

- New header `src/compositor/arbc/compositor/damage_planning.hpp`: `DirtyRegion`, `map_damage_to_device`, `clock_advance_damage`, `invalidate_damage`.
- New impl `src/compositor/damage_planning.cpp`: all three free functions with temporal gate, cull walk, cache driver binding.
- New unit test `src/compositor/t/damage_planning.t.cpp` (5 cases): map/clock/invalidate/driver-gating with counter assertions.
- New golden `tests/damage_planning_golden.t.cpp`: byte-exact damaged-region re-render + quiescent (zero renders/composites).
- `src/compositor/arbc/compositor/tile_planning.hpp`: forward-declared `DirtyRegion`; trailing `const DirtyRegion* dirty = nullptr` on `render_frame_interactive`.
- `src/compositor/tile_planning.cpp`: null→whole-viewport clear guard; per-layer dirty gating via mapped `local_region` ∩ dirty.
- `src/compositor/CMakeLists.txt`, `tests/CMakeLists.txt`: wiring for new sources and golden.
- `tests/claims/registry.tsv`: 2 new rows — `02-architecture#damage-maps-to-device-dirty-regions`, `11-time-and-video#clock-advance-damages-only-moving-layers`; re-asserted `02-architecture#damage-invalidates-by-content-region-across-rungs`, `11-time-and-video#static-tiles-survive-clock` via `enforces:` tags.
- Tech-debt follow-up registered as `compositor.temporal_coalescing` (2d) in `tasks/35-compositor.tji`, wired to milestone `m3_still_compositor`.
