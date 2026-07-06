# compositor.refinement — Progressive refinement

## TaskJuggler entry

`tasks/35-compositor.tji:45-50` → `compositor.refinement` ("Progressive
refinement"), the seventh leaf under `task compositor`. It carries
`depends !tile_planning` (`35-compositor.tji:48`) and, through the parent
`task compositor`, inherits `depends contract.async_render, cache.key_shapes,
color.resampling` (`35-compositor.tji:7`). Note line:

> "Async tile arrivals emit damage scheduling follow-up frames; speculative
> next-rung requests during zoom gestures. Docs 02/04."

## Effort estimate

**2d.** Half the tile_planning leaf, and rightly: this task builds no new
rendering pipeline — it wires two predictive/refinement behaviors onto the
planner and driver `tile_planning` already shipped, reusing the ring builders
and `prime_ring` driver `cache.prefetch` already landed. The deliverable is
(1) the **progressive-refinement loop** — a caller-owned pending-completion
registry, a driver change that *records* a deferred (async) tile render instead
of dropping it, and a poll that turns each arrival into a cache insert plus
damage for its region (doc 02:69-71 step 6); and (2) **zoom speculation** — a
compositor-side next-rung ring builder fed through the existing
`cache::prime_ring` under `PriorityClass::Speculative`, plus driving the
already-built pan ring under `Adjacent` (doc 02:92-93, 04:99-101). Both halves
produce *want-lists* and *damage*, not renders — the actual speculative/async
render dispatch and the frame-scheduling loop stay `pull_service` +
`runtime.interactive`. Plus unit tests on the ring geometry, the priming
invariant, and the refinement-queue poll; one byte-exact coarse-then-refine
golden; three claims-register entries; and the CMake wiring.

## Inherited dependencies

**Settled:**

- `compositor.tile_planning` (DONE 2026-07-05) — the direct predecessor
  (`depends !tile_planning`). It delivered the planner and synchronous driver
  this task extends, in `src/compositor/arbc/compositor/tile_planning.hpp`
  (+ `tile_planning.cpp`):
  - **`std::vector<TileCoord> tiles_covering(ScaleRung, const Rect&)`** (`:60`)
    and **`Rect tile_local_rect(ScaleRung, TileCoord)`** (`:66`) — the tile-grid
    geometry. This task's **zoom ring** re-tiles the visible local region at the
    *next* rung with `tiles_covering`, so the ring is the same geometry at a
    neighbouring rung.
  - **`enum class TileSource { Fresh, Stale, Coarser, Placeholder }`** (`:71`),
    **`struct PlannedTile`** (`:82`, `is_miss` / `display_source` / `hold`),
    **`struct LayerTilePlan`** (`:97`), **`LayerTilePlan plan_layer(...)`**
    (`:119`) — the pure plan. A deferred (async) miss is a `PlannedTile` with
    `is_miss == true` whose fresh render did not settle inline; this task gives
    that tile a life beyond the pass.
  - **`void render_frame_interactive(const DocRoot&, const ContentResolver&,
    const Viewport&, TileCache&, Backend&, SurfacePool&, Surface& target,
    Deadline, std::optional<std::uint64_t> prior_revision)`** (`:138`) — the
    synchronous tiled driver. Its per-tile miss loop (`tile_planning.cpp:246-276`)
    renders a miss inline through a `RenderCompletion` and, **critically, drops
    the tile when the content answers async** (`inline_result.has_value()` gates
    the fill at `:262-274`; a `nullopt` result leaves the tile on its fallback
    display source with no record). Closing that drop is the core of this task.
  - Constants `k_tile_size = 256` (`:46`) and `k_max_fallback_octaves = 4`
    (`:52`), and the composite switch + `composite_coarser` (`tile_planning.cpp:39,
    :280-297`) — reused unchanged. tile_planning's refinement explicitly routed
    the **prefetch ring (`Adjacent`), speculative next-rung requests during zoom
    (`Speculative`), and async-arrival progressive refinement** to this task
    (`tile_planning.md:214-218`); this refinement is that work order.
- `cache.prefetch` (commit `2ba77e7`, DONE) — the de-facto predecessor whose
  predictive-residency layer this task drives (the parent `depends` line does not
  list it, but it is landed, so scheduling is unaffected — see the return
  summary's note to the orchestrator). Header-only in `arbc::cache` over the
  store's public surface, `src/cache/arbc/cache/prefetch.hpp`:
  - **`std::vector<TileKey> pan_prefetch_ring(std::span<const TileKey> visible,
    std::int32_t radius)`** (`:71`) — the spatial pan ring (doc 02:92-93): the
    Chebyshev annulus around the visible tile set, excluding the visible set.
    Pure `TileCoord` arithmetic. This task assembles the visible key set and calls
    it; it builds no pan geometry of its own.
  - **`template <class Key, class Value> std::vector<Key> prime_ring(
    KeyedStore<Key,Value>&, std::span<const Key> ring, PriorityClass klass)`**
    (`:132`) — the classify-resident / report-absent driver: `reclassify` every
    resident ring member onto `klass` and return the *absent* members as a
    want-list; **renders nothing, inserts nothing** — a `prime_ring` call "leaves
    `resident_bytes()` and `evictions()` unchanged" (`prefetch.hpp:17-25`). This
    task feeds it the pan ring (`Adjacent`) and the zoom ring (`Speculative`);
    the header comment names exactly this use — "zoom rung -> `Speculative`"
    (`:126-128`).
  - **`template <class Key> std::vector<Key> temporal_prefetch_ring(const Key&,
    int direction, Time step, Time horizon)`** (`:110`) and
    `prefetch_temporal_step` (`:44`) — the *playback* ring (doc 11). Its
    `direction` sign parameter is the pattern this task's zoom ring copies. The
    temporal ring is **not** this task's to drive (it is playback, not zoom — see
    "Not this task").
- `cache.key_shapes` (commit `a198981`, DONE 2026-07-05) — the types
  (`src/cache/arbc/cache/key_shapes.hpp`): `TileKey` (`:64-76`), `TileValue`
  (`:110-113`), `TileMeta` (`:99-102`), `tile_byte_cost` (`:120-124`),
  `using TileCache = KeyedStore<TileKey, TileValue>` (`:129`). The
  **`enum class PriorityClass { Speculative, Recent, Temporal, Adjacent,
  Visible }`** (`src/cache/arbc/cache/keyed_store.hpp:26-31`) is now five-valued —
  `cache.prefetch` inserted `Temporal` between `Recent` and `Adjacent`
  (`keyed_store.hpp:18-25`); this task uses `Speculative` (zoom) and `Adjacent`
  (pan), the two ends. `KeyedStore::reclassify` (`:166`), `insert` (`:156`),
  `lookup` (`:162`), and the counters `hits()`/`misses()`/`evictions()`
  (`:193-195`) plus `resident_bytes()` are the store surface the priming and the
  poll drive.
- `contract.async_render` (DONE) — the completion contract
  (`src/contract/arbc/contract/content.hpp`): **`RenderCompletion`** (`:100-132`),
  the one-shot thread-safe settle handle whose renderer side (`complete`/`fail`)
  a worker calls and whose caller side (`take`/`settled`) the poll reads — with
  the load-bearing note that **"how the caller is woken on completion" is runtime
  policy** (`:117-118`); `RenderRequest` (`:67-75`), `Deadline` (`:47-54`,
  value-only), and `Content::render(request, done)` (`:167`), the async entry a
  deferred miss rides. This task holds the pending `RenderCompletion` past the
  frame; it does not implement the wake.
- `contract.snapshot_pins` (DONE 2026-07-05) — `StateHandle snapshot` on
  `RenderRequest` (`content.hpp:71`); a pending tile carries the snapshot its
  deferred request was issued under, so its arrival is honestly keyed.
- `color.resampling` (commit `fa895d5`, DONE 2026-07-05) — the composite/downsample
  kernels behind the coarse-then-refine display (a coarser fallback shown while a
  fresh tile is pending is composited exactly as tile_planning does,
  `tile_planning.cpp:280-297`). Owned by color; depended on, not re-registered.

**Pending:** none — every predecessor is landed.

## What this task is

Two behaviors, layered onto the interactive tiled path `tile_planning` shipped,
in a new header/impl `src/compositor/arbc/compositor/refinement.hpp`
(+ `refinement.cpp`) — plus a minimal seam added to `render_frame_interactive`:

1. **The progressive-refinement loop (doc 02:69-71 step 6 "Refine").** Today a
   miss the content answers *asynchronously* (`content->render` returns
   `nullopt`) is dropped when the pass ends (`tile_planning.cpp:262-274`): the
   tile shows its coarser/stale/placeholder fallback this frame and is forgotten,
   so the sharp result never lands. This task closes that:
   - **`struct RefinementQueue`** — a caller-owned registry of **pending tiles**,
     each `{ TileKey key; Rect local_rect; ObjectId content; std::size_t bytes;
     std::shared_ptr<RenderCompletion> done; }`. It is frame-to-frame state, so
     the *caller* (runtime frame loop) owns it, not the planner (matching
     tile_planning's "frame-to-frame state is the runtime loop's" rule).
   - **A driver seam:** `render_frame_interactive` gains an optional
     `RefinementQueue* pending = nullptr` parameter. When non-null, a miss whose
     inline `render` returns `nullopt` (still-live `RenderCompletion`) is
     **recorded** into `pending` instead of dropped; the tile still composites its
     best available fallback this frame (the "coarse-then-refine" display). When
     null, behavior is byte-for-byte what tile_planning's tests assert (the drop).
   - **`std::vector<Damage> poll_refinements(RefinementQueue&, TileCache&)`** —
     for each pending tile whose `RenderCompletion` has **settled** (`done->settled()`
     with a value): `insert` the completed `TileValue` into the cache under
     `PriorityClass::Visible`, emit a `model::Damage{content, tile_local_rect(...),
     time…}` for the tile's region, and drop it from the queue. Unsettled tiles
     stay. The returned damage is what "async arrivals emit damage scheduling a
     follow-up frame" (doc 02:69) *is*, concretely: the runtime, seeing non-empty
     damage, re-invokes the frame; the now-resident tile plans `Fresh` and
     composites sharp. An empty poll (nothing settled) returns **empty** damage —
     no follow-up frame ("no damage → no work", doc 02:51).
2. **Zoom speculation + pan priming (doc 02:92-93, 04:99-101).**
   - **`std::vector<TileKey> zoom_prefetch_ring(const RungSelection& current,
     const Rect& local_region, ObjectId content, std::uint64_t revision,
     std::optional<Time> achieved_time, int zoom_direction)`** — the next-rung
     ring. It re-tiles `local_region` at the rung the gesture is heading toward
     (rung − 1 when zooming in toward finer detail, rung + 1 when zooming out, by
     the `zoom_direction` sign) via `tiles_covering`, and assembles the `TileKey`
     set at that rung. The compositor owns this (it needs the tile-grid geometry);
     the ring is then fed to `cache::prime_ring(store, ring,
     PriorityClass::Speculative)`.
   - **Pan priming** reuses `cache::pan_prefetch_ring` directly: assemble the
     visible layer's `TileKey` set, build the annulus, feed it to
     `cache::prime_ring(store, ring, PriorityClass::Adjacent)`.
   - **`std::vector<TileKey> prime_prefetch(TileCache&, const LayerTilePlan&,
     int zoom_direction, std::int32_t pan_radius)`** — the thin compositor driver
     that assembles the visible keys from a plan, builds both rings, primes each
     under its class, and returns the merged **want-list** (the absent tiles the
     caller will render opportunistically). It renders nothing and evicts nothing
     — the `prime_ring` invariant carries through (`resident_bytes()`/`evictions()`
     unchanged across the call).

**Not this task:**

- **Async worker dispatch, off-thread completion, and rendering the speculative /
  pan want-list on workers** → `compositor.pull_service` (`35-compositor.tji:33-38`)
  + `runtime.interactive` (doc 17:60). This task *records* a pending completion
  and *polls* whether it settled; it does not schedule the render, run a worker,
  or wake the caller (`content.hpp:117-118`: the wake is runtime policy). The
  want-list `prime_prefetch` returns is `pull_service`'s to render opportunistically
  under the low-priority classes; this task never renders a speculative or
  adjacent tile inline (that would block the frame — see Decisions).
- **Mapping the emitted `Damage` to per-viewport device dirty regions, the
  follow-up-frame *scheduler* (the loop that re-invokes on damage), and
  clock-advance as the temporal-damage axis** → `compositor.damage_planning`
  (`35-compositor.tji:22-27`) + `runtime.interactive`. This task *emits* `Damage`
  (content + local rect), homogeneous with model damage; it does not map it to
  device space or own the frame loop. Its tests re-invoke the whole-viewport
  driver directly to prove the arrived tile now composites `Fresh`.
- **Driving the temporal prefetch ring (`Temporal`, playback prefetch)** → the
  runtime playback/transport path, which owns the playback hint (doc 11:110-121);
  `cache.prefetch` already built the ring, but its driver reads a playback
  direction/horizon this task has no gesture for. Zoom and pan only.
- **Exposing debug counters (requests / cache hits / composites / follow-up
  frames)** → `compositor.counters` (`35-compositor.tji:51-56`). This task proves
  its behavioral claims by direct assertion on returned values (the damage vector,
  the want-list, the re-plan's miss count) and the store's existing
  `evictions()`/`resident_bytes()`, not a new counter surface.
- **`inputs()` / operator-graph awareness, `identity()` short-circuit** →
  `compositor.operator_graph`; **anchor-walk cull / `2^16` re-anchor** →
  `compositor.anchored_viewports`. Untouched here.
- Any change to the **offline `render_frame`** (exact-scale, no quantization, no
  placeholders — doc 02:74-85) and to the **null-`pending` path** of
  `render_frame_interactive` (unchanged for tile_planning's goldens).

## Why it needs to be done

Doc 02's interactive frame has six steps; `tile_planning` landed steps 2-5 (cull,
plan, render-misses-inline, composite). **Step 6, "Refine"** (`02:69-71`) — "Async
results that arrive later produce damage for their region, scheduling a follow-up
frame. Zooming therefore shows progressively sharper content rather than
blocking" — is unbuilt: the driver drops async results, so a layer that renders
off-thread never sharpens. And doc 04's promise that makes infinite zoom *feel*
continuous — "During a zoom gesture, the compositor speculatively requests the
next rung while displaying the current one" (`04:99-101`) — has no driver: the
`Speculative` and `Adjacent` cache classes `key_shapes` defined and the ring
builders `cache.prefetch` shipped have no compositor caller. This task supplies
both: the loop that turns async arrivals into follow-up frames, and the priming
that keeps the rung ahead of the gesture warm. Until it lands, the interactive
renderer blocks on slow layers and re-thrashes on every zoom step, and
`pull_service` (which schedules the want-lists and completions this task
produces) and `runtime.interactive` (which consumes the damage this task emits)
have no plan value to hang their scheduling on.

## Inputs / context

- `docs/design/02-architecture.md`:
  - `:49-71` — **The frame, interactively**. `:51-52` step 1 "Collect damage …
    No damage → no work" (the quiescent-frame invariant the poll honors). `:61-65`
    step 4 "Render misses within budget": misses become deadline requests, "Layers
    may answer synchronously, or asynchronously with a placeholder policy … the
    frame proceeds with what it has" — the async answer this task stops dropping.
    **`:69-71` step 6 "Refine"** — the governing sentence: async results "produce
    damage for their region, scheduling a follow-up frame", so "Zooming …
    shows progressively sharper content rather than blocking".
  - `:87-116` — **Tile cache**. `:92-93` the priority ladder "visible > adjacent
    (pan prefetch ring) > recently visible > speculative (next/previous zoom
    rung)" — this task assigns `Adjacent` (pan) and `Speculative` (zoom). `:94-95`
    "Damage invalidates by `(content id, region)` across all rungs" — the shape of
    the `Damage` an arrival emits (content + region). `:100-104` the residency pin
    — a just-completed `insert` yields a pinned hold, so the follow-up frame's
    `lookup` composites from a live entry.
  - `:118-137` — **Threading model**. `:135-137` "v1 may degenerate to everything
    on one thread" — this task's poll/record is single-threaded; the off-thread
    completion is `pull_service`. Async layers "integrate through the async
    completion path rather than occupying a worker" — the path this task consumes.
- `docs/design/04-transforms-and-infinite-zoom.md`:
  - `:88-106` — **Scale ladders and tile geometry**. `:93-94` "a smooth pinch-zoom
    reuses one rung's tiles across an octave" — why priming the *next* rung (not
    every intermediate scale) suffices. `:95-98` the downsample convention ("rung
    ≥ needed scale … once the next rung is available") — sets which rung is "next"
    for the zoom ring by direction. **`:99-101`** the speculative next-rung request
    "while displaying the current one — this is the progressive-refinement path
    from doc 02". `:103-106` tiles axis-aligned in local space — the zoom ring is
    the same local grid at a neighbouring rung.
- `docs/design/11-time-and-video.md` — `:110-121`/`:141-149` the temporal prefetch
  ring and its playback hint: named here only to *exclude* it (playback, not zoom).
  A pending `Timed` tile carries its requested `Time`; a `Static` pending tile's
  key omits `achieved_time`, so a still layer's arrival re-plans clock-invariantly.
- `docs/design/16-sdlc-and-quality.md`:
  - `:14-25` — the claims register (`<doc-file-stem>#<slug>`, `// enforces:` tag).
  - `:47-53` — byte-exact CPU goldens: the coarse-then-refine golden's post-arrival
    frame is byte-identical to the all-inline render of the same tile.
  - `:54-62` — behavioral-counter tests ("playback of a still scene issues zero
    visual renders … Most claims-register entries about efficiency land here"):
    this task pins the quiescent-poll-emits-no-damage claim as a **value-level**
    behavioral assertion; `counters` re-exposes it through the counter surface.
  - `:112-118` — ≥90% diff coverage on changed lines.
- `docs/design/17-internal-components.md`:
  - `:56` — `arbc::compositor` is **Level 4**, `Depends on: contract, cache
    (+ below)`. `cache.prefetch`'s ring builders + `prime_ring` are part of the
    `cache` component (public header `prefetch.hpp`); `model::Damage` is reached
    through the same transitive `model` visibility `render_frame_interactive`
    already uses for `DocRoot`. **No new `DEPENDS` edge, no `backend-cpu` edge.**
  - `:40-44` — "depend only on strictly lower levels; no same-level edges"; the
    doc-17 dependency check must stay green.
  - repo layout: public headers under `src/compositor/arbc/compositor/`, unit tests
    in `src/compositor/t/`, cross-component goldens in top-level `tests/`.
- `src/compositor/arbc/compositor/tile_planning.hpp` — `tiles_covering` (`:60`),
  `tile_local_rect` (`:66`), `TileSource` (`:71`), `PlannedTile` (`:82`),
  `LayerTilePlan` (`:97`), `plan_layer` (`:119`), `render_frame_interactive`
  (`:138`), `k_tile_size` (`:46`).
- `src/compositor/tile_planning.cpp:246-276` — the per-tile miss loop; `:262-274`
  the inline-fill gate (the drop-on-async this task closes); `:280-297` the
  composite switch reused for the fallback display; `composite_coarser` (`:39`).
- `src/cache/arbc/cache/prefetch.hpp` — `pan_prefetch_ring` (`:71`),
  `temporal_prefetch_ring` (`:110`), `prime_ring` (`:132`), the residency-only
  invariant (`:17-25`, `:124-131`).
- `src/cache/arbc/cache/keyed_store.hpp` — `PriorityClass` (`:26-31`), `reclassify`
  (`:166`), `insert` (`:156`), `lookup` (`:162`), `hits`/`misses`/`evictions`
  (`:193-195`) + `resident_bytes()`.
- `src/cache/arbc/cache/key_shapes.hpp` — `TileKey` (`:64-76`), `TileValue`
  (`:110-113`), `tile_byte_cost` (`:120-124`), `TileCache` (`:129`).
- `src/contract/arbc/contract/content.hpp` — `RenderCompletion` (`:100-132`, the
  "wake is runtime policy" note `:117-118`), `RenderRequest`/`snapshot` (`:67-75`),
  `Deadline` (`:47-54`), `Content::render` (`:167`).
- `src/model/arbc/model/damage.hpp` — `struct Damage { ObjectId object; Rect rect;
  Time start, end; }` (`:19`), `rect_union` (`:30`), `damage_add` (`:44`),
  `DamageSink` (`:60`), and the "no consumer yet — wired from above" note
  (`:56-59`): this task is a *producer* of `Damage`, not the sink consumer.
- `src/compositor/arbc/compositor/compositor.hpp:16-24` — `Viewport`,
  `ContentResolver`.
- `src/compositor/CMakeLists.txt:1-8` — `DEPENDS contract cache`, the
  `arbc_add_component` + `arbc_component_test` pattern; this task adds the header,
  impl, unit test, and (in `tests/CMakeLists.txt`) the golden. `DEPENDS` stays
  `contract cache`.
- `tests/claims/registry.tsv` — 2-column TAB-separated; this task appends three
  rows. Enforce-tag example: `src/compositor/t/tile_planning.t.cpp:142`
  (`// enforces: 02-architecture#miss-becomes-deadline-request`).

## Constraints / requirements

- **Levelization (doc 17:40-44, :56).** L4 `arbc::compositor`, using only
  `contract`, `cache` (incl. `cache.prefetch`'s `prefetch.hpp`), and their
  transitive closure (`model` for `Damage`, `surface`, `pool`). Composite/downsample
  reached only through the abstract L2 `surface::Backend`. `DEPENDS contract cache`
  is **unchanged** (no new listed edge); the CI dependency check stays green.
- **The `RefinementQueue` is caller-owned frame-to-frame state; planning stays
  pure (doc 02:123-125).** `render_frame_interactive` records into a
  caller-provided queue but the queue is not compositor-global state; `plan_layer`
  is untouched and still allocation-free/lock-free. The null-`pending` path is
  byte-identical to tile_planning's current behavior.
- **An arrival emits `model::Damage` in content-local terms, not device regions
  (doc 02:69, :94).** `poll_refinements` emits `Damage{content, tile_local_rect,
  time…}`; mapping to per-viewport device dirty regions is `damage_planning`'s.
  The damage is homogeneous with model damage so both feed one downstream
  scheduler.
- **Quiescent poll emits zero damage (doc 02:51 "No damage → no work").** A poll
  with nothing settled returns an empty vector; a queue that never receives an
  arrival never schedules a follow-up frame. No busy-loop of empty follow-up
  frames.
- **Priming is residency-only; the want-list is rendered elsewhere (doc 02:92-93,
  prefetch invariant).** `prime_prefetch` and the two ring primings `reclassify`
  residents and report absent keys but **render nothing and insert nothing** —
  `resident_bytes()` and `evictions()` are unchanged across the call
  (`prefetch.hpp:17-25`). Speculative/adjacent tiles are **never** rendered inline
  in the frame (they are the lowest priority classes, rendered opportunistically
  by `pull_service`). The frame's critical path is exactly `tile_planning`'s.
- **The zoom ring is the visible region re-tiled at the neighbouring rung
  (doc 04:99-101, :103-106).** `zoom_prefetch_ring` calls `tiles_covering` at
  `rung − 1` (zoom-in, `zoom_direction < 0`, heading toward finer scale, the
  downsample-preferred rung once available) or `rung + 1` (zoom-out,
  `zoom_direction > 0`); the keys carry the current revision and, for `Timed`
  content, the current `achieved_time`. `zoom_direction == 0` yields an empty ring
  (no gesture → no speculation).
- **Zoom direction is a caller-supplied sign, never inferred (doc 11 temporal-ring
  parity).** The compositor reads no inter-frame camera velocity; the caller
  (runtime, which owns the gesture) passes a sign exactly as
  `temporal_prefetch_ring` takes `direction`. Keeps refinement stateless.
- **A pending tile is honestly keyed to the snapshot it was requested under.** The
  `RefinementQueue` record carries the `TileKey` (with the request's revision and,
  for `Timed`, `achieved_time`); the arrival inserts under that exact key, so a
  follow-up frame at the same revision plans it `Fresh` and one at a bumped
  revision does not (it stays reachable only as a stale probe — tile_planning's
  degradation order).
- **Single-threaded; no concurrency surface added.** `poll_refinements` reads
  `RenderCompletion::settled()`/`take()` (already thread-safe, `content.hpp:100-132`)
  from the frame thread; the record and poll are serial. The off-thread `complete`
  is `pull_service`'s worker. **No TSan obligation** in this task (it issues no
  concurrent access itself; the completion's own thread-safety is `contract`'s).
- **CI diff coverage ≥90%** (doc 16:112-118); the public header compiles standalone.

## Acceptance criteria

- **Unit tests — `src/compositor/t/refinement.t.cpp` (new, Catch2), registered via
  `arbc_component_test`.** Against a hand-populated `TileCache` and a test
  `Content` that answers async (returns `nullopt`, settles its `RenderCompletion`
  on demand) — no backend needed for the ring/queue logic:
  - **Zoom-ring geometry:** `zoom_prefetch_ring` with `zoom_direction < 0` returns
    the visible region's coverage at `rung − 1` (finer: more, smaller tiles);
    `> 0` returns `rung + 1` (coarser: fewer tiles); `== 0` returns empty. Keys
    carry the current revision; `Timed` content carries the requested `Time`,
    `Static` omits `achieved_time`.
  - **Priming invariant:** `prime_prefetch` over a cache with some ring members
    resident reclassifies exactly those residents onto their class
    (`Speculative`/`Adjacent`, asserted via a follow-up `lookup` + a `reclassify`
    observation) and returns exactly the **absent** members as the want-list;
    `evictions()` and `resident_bytes()` are **unchanged** across the call.
  - **Queue record + poll:** driving `render_frame_interactive` with a non-null
    `RefinementQueue` over an async-answering content records one pending tile per
    async miss (and the tile still composites its fallback this frame); a
    `poll_refinements` before the completion settles returns **empty** damage and
    retains the tile; after `done->complete(...)`, the poll inserts the tile under
    `Visible`, returns one `Damage{content, tile_local_rect, …}`, and empties the
    queue. A poll of an **empty** queue returns empty damage.
  - **Null-`pending` unchanged:** `render_frame_interactive` with `pending ==
    nullptr` over the same async content drops the tile exactly as today (no record,
    no crash) — the tile_planning contract is preserved.
- **Golden — `tests/refinement_golden.t.cpp` (new, cross-component, links `arbc` +
  `CpuBackend`).** Byte-exact (doc 16:47-53), the coarse-then-refine loop:
  - **Frame 1 (async) shows the fallback; the fresh arrival composites sharp.**
    A layer whose content answers async at a power-of-two, axis-aligned scale:
    frame 1 (empty cache, `RefinementQueue`) composites the coarser/placeholder
    fallback and records the pending tile; settle the completion; `poll_refinements`
    inserts + emits damage; frame 2 re-plans the tile `Fresh` and composites a
    target **byte-identical** to `render_frame_interactive` of the same scene with a
    *synchronous* content (all-inline) — proving the arrived tile is placed and
    composited identically to an inline render, with no seam or double-blend.
  - **Quiescent third frame schedules nothing.** With the queue drained and no new
    async miss, `poll_refinements` returns **empty** damage and a re-plan issues
    **zero** `is_miss` tiles — a still scene after refinement completes does no
    follow-up work (doc 02:51; the behavioral-counter class, doc 16:54-62).
- **Claims (register + `enforces:` tags)** appended to `tests/claims/registry.tsv`,
  enforced from the tests above:
  - `02-architecture#async-arrival-emits-damage` — "A tile render answered
    asynchronously is recorded pending rather than dropped; on completion it is
    inserted into the cache under `Visible` and emits `Damage` for its tile region,
    so a follow-up frame re-plans it as a fresh cache hit and composites it sharp."
    (doc 02:69-71)
  - `02-architecture#quiescent-refinement-schedules-no-frame` — "A refinement poll
    with no settled arrivals emits zero damage, so a scene with no pending tiles and
    no other damage schedules no follow-up frame." (doc 02:51; doc 16:54)
  - `04-transforms-and-infinite-zoom#zoom-speculates-next-rung` — "During a zoom
    gesture the compositor primes the next rung's tiles for the visible region under
    `PriorityClass::Speculative`: resident next-rung tiles are reclassified
    Speculative and absent ones are returned as a want-list, the prime pass
    rendering and evicting nothing." (doc 04:99-101, doc 02:92-93)
  The coarse-then-refine golden additionally exercises
  `16-sdlc-and-quality#byte-exact-goldens` (`registry.tsv:35`); this task does not
  re-register that meta-claim.
- **No new WBS leaf.** Every deferral above lands on an **existing sibling**:
  async worker dispatch + rendering the want-list + off-thread completion →
  `compositor.pull_service` + `runtime.interactive`; damage→device mapping + the
  follow-up-frame scheduler + clock-advance temporal damage →
  `compositor.damage_planning` + `runtime.interactive`; temporal-ring driving →
  the runtime playback path; counter-surface exposure → `compositor.counters`.
  The closer registers **no** new task for this refinement.
- **Component wiring & CI dependency check:** `src/compositor/CMakeLists.txt` adds
  `refinement.cpp` to `SOURCES`, `arbc/compositor/refinement.hpp` to
  `PUBLIC_HEADERS`, and `t/refinement.t.cpp` to the component test; the golden is
  added to `tests/CMakeLists.txt`; `render_frame_interactive`'s new optional
  parameter keeps the existing callers source-compatible; `DEPENDS` stays
  `contract cache`; the header compiles standalone; the doc-17 dependency check
  passes (no new edge).
- **Gate green (build + tiers 1-5 in Debug + ASan/UBSan).** No TSan obligation
  (single-threaded record/poll; the completion's concurrency is `contract`'s).

## Decisions

- **The `RefinementQueue` is caller-owned frame-to-frame state; the driver records,
  a free `poll_refinements` drains.** Doc 02:123-125 keeps `plan_layer` pure and
  lock-free, and tile_planning established that frame-to-frame state belongs to the
  runtime loop, not L4 planning. A caller-owned queue keeps the planner and the
  null-`pending` driver path untouched, makes the poll unit-testable with a
  hand-driven completion and no backend, and gives `pull_service`/`runtime` a plain
  value to own across frames. *Rejected:* a compositor-global static pending
  registry — hidden frame-to-frame state at L4, un-testable in isolation, and it
  would make two concurrent viewports share a queue. *Rejected:* threading the
  pending set through `LayerTilePlan` — the plan is a pure per-frame value
  (tile_planning); pending tiles outlive the frame.
- **An arrival emits `model::Damage` (content + local rect), not device dirty
  regions, and `poll_refinements` *returns* it rather than pushing a
  `DamageSink`.** Doc 02:69 says arrivals "produce damage for their region"; doc
  02:94 keys damage by `(content id, region)` — content-local, exactly `Damage`'s
  shape. Returning the vector (not calling a sink) keeps refinement from depending
  on the `DamageSink` virtual and from owning the frame-scheduling loop, and makes
  the emitted damage homogeneous with model damage so `damage_planning`'s
  device-mapping consumes both through one path. *Rejected:* emitting device
  dirty regions here — duplicates `damage_planning`'s model-damage→device mapping
  and needs a viewport the poll doesn't have. *Rejected:* pushing into a
  `model::DamageSink` — couples L4 to the sink virtual and buries the scheduling
  decision that is runtime's.
- **Priming is residency-only; the want-list is rendered by `pull_service`, never
  inline.** Doc 02:92-93 ranks `Speculative`/`Adjacent` *below* `Visible`, and
  `cache.prefetch`'s `prime_ring` renders and evicts nothing by contract
  (`prefetch.hpp:17-25`). Rendering a speculative tile inline would put
  lowest-priority work on the frame's critical path — the opposite of "display the
  current rung while the next is prepared". So `prime_prefetch` returns the absent
  keys and leaves them for `pull_service` to render opportunistically under their
  class. *Rejected:* rendering the want-list inline in the frame — blocks the
  interactive frame on speculation, defeating the point of the priority ladder and
  doc 04:99's "while displaying the current one".
- **The zoom ring is built in the compositor (it needs `tiles_covering`); the pan
  ring stays in `cache.prefetch`.** `cache.prefetch`'s rings are pure key
  arithmetic on injected keys — the pan ring is neighbour-`coord` arithmetic, the
  temporal ring is `achieved_time` stepping — and the cache is levelized below the
  tile-grid geometry (doc 17). The zoom ring is *not* neighbour arithmetic: a tile
  at rung r maps to a different set of tiles at rung r±1, so building it requires
  re-tiling the visible *local region* with `tiles_covering`, which is L4 compositor
  knowledge. So the builder lives here; it then reuses the generic
  `cache::prime_ring` unchanged. *Rejected:* adding `zoom_prefetch_ring` to
  `cache.prefetch` — the cache layer cannot re-tile a local region (it has no
  `tiles_covering`, no rung geometry) without pulling compositor concerns below its
  level, breaking the levelization the pan/temporal split was designed around.
- **Zoom direction is a caller-supplied sign, mirroring `temporal_prefetch_ring`'s
  `direction`; the compositor infers no gesture.** The gesture (and thus which rung
  is "next") is known to the runtime that drives the camera; inferring it inside the
  compositor would require inter-frame camera state — frame-to-frame state that is
  the runtime loop's, not L4 planning's (tile_planning's rule). The temporal ring
  already established the "caller passes a direction sign, the cache/compositor
  never calls up" pattern (doc 11:110-121). *Rejected:* deriving zoom direction from
  the scale delta between the last two frames inside the compositor — smuggles
  frame-to-frame state into a stateless planner and duplicates the gesture tracking
  the runtime already does. The `previous`-rung half of "next/previous zoom rung"
  (doc 02:92-93) is served by the same builder with the opposite sign — no separate
  path.
- **The async-record seam is an optional `RefinementQueue*` parameter on
  `render_frame_interactive`, defaulting null to today's drop behavior.** The
  record-vs-drop decision lives inside the driver's per-tile miss loop
  (`tile_planning.cpp:262-274`); reaching it means either an added parameter or a
  duplicated driver. A defaulted `nullptr` parameter is source-compatible with
  tile_planning's existing callers and goldens (which assert the drop), adds one
  branch, and needs no second copy of the tiled walk. *Rejected:* a separate
  `render_frame_progressive` — duplicates the entire cull/plan/composite walk for a
  one-branch difference, a maintenance fork. *Rejected:* making the queue
  mandatory/always-on — changes the existing signature and the null-behavior tests.
- **Claims registered against `02-architecture`/`04-transforms` heading stems.**
  Following the register's established slug form (tile_planning's three claims are
  `02-architecture#…` / `11-time-and-video#…` slugs, not literal markdown anchors),
  the three new claims reuse the governing doc stems. The behaviors are already
  normative — step 6 "Refine", "No damage → no work", and doc 04:99's next-rung
  speculation — so minting the slugs needs no doc edit.
- **No design-doc delta.** Every rule here is settled doc text: async arrival →
  damage → follow-up frame (doc 02:69-71), the quiescent-frame invariant (doc
  02:51), the priority ladder with pan/zoom classes (doc 02:92-93), next-rung zoom
  speculation and the downsample convention (doc 04:95-101), and damage keyed by
  `(content, region)` (doc 02:94). The `contract, cache` edge (doc 17:56) is
  unchanged and `cache.prefetch`'s facilities are already landed. This task
  *concretizes* those promises into C++ without altering designed behavior — no doc
  edit, no doc-00 bullet.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-05.

- Delivered `src/compositor/arbc/compositor/refinement.hpp` + `src/compositor/refinement.cpp`: `RefinementQueue`, `poll_refinements`, `zoom_prefetch_ring`, `prime_prefetch`.
- Added optional `RefinementQueue* pending = nullptr` seam to `render_frame_interactive` in `src/compositor/tile_planning.cpp`; null-pending path unchanged (tile_planning goldens byte-identical).
- Forward-declared `RefinementQueue` in `src/compositor/arbc/compositor/tile_planning.hpp`; null-pending parameter added source-compatibly.
- Unit tests in `src/compositor/t/refinement.t.cpp` (6 cases): zoom-ring geometry, `prime_prefetch` want-list/residency/reclassify-to-Speculative, `poll_refinements` settled/unsettled/failed/empty, driver record vs. null-drop.
- Golden in `tests/refinement_golden.t.cpp`: coarse-then-refine byte-exact match + quiescent zero-work frame.
- Three claims registered in `tests/claims/registry.tsv`: `02-architecture#async-arrival-emits-damage`, `02-architecture#quiescent-refinement-schedules-no-frame`, `04-transforms-and-infinite-zoom#zoom-speculates-next-rung`.
- CMake wiring: `src/compositor/CMakeLists.txt` (new source + header + component test) and `tests/CMakeLists.txt` (golden target).
- Deviation (zoom sign): implemented `zoom_direction < 0` → `rung.index + 1` (finer) because higher rung index = finer in this codebase (`rung_scale = 2^index`); refinement prose had the arithmetic backwards relative to `ScaleRung` orientation.
- Deviation (struct field): `PendingTile` carries `std::unique_ptr<Surface> surface` beyond the 5-field list — the async render target must outlive the frame for `cache.insert` on arrival.
