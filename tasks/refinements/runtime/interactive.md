# runtime.interactive — Interactive renderer (deadline-bounded, damage-driven frame loop assembling the compositor free functions + worker pool, owning the frame-to-frame state)

## TaskJuggler entry

`tasks/65-runtime.tji:14-18` → `runtime.interactive` ("Interactive renderer"),
the second leaf under `task runtime`. Its own edges:
`depends !threading, compositor.refinement, cache.prefetch` (`:17`); the parent
`task runtime` adds `depends compositor.tile_planning` (`:6`), inherited
transitively. Note line (`:18`):

> "Frame loop with deadlines: collect damage, plan, render misses within budget,
> composite, refine; placeholder policy (stale > coarser > transparent). Doc 02."

It is a direct dependency of the milestone `m3_still_compositor`
(`tasks/99-milestones.tji:28-31`, `depends … runtime.interactive …`) and the
direct predecessor of `runtime.host_objects` (`:26-30`,
`depends !interactive, …`). It is **not** the milestone's last dependency —
`m3_still_compositor` also waits on `compositor.anchored_viewports`,
`kinds.raster`, and `color.kernel_goldens` (`:30`) — so completing this task
adds no `complete 100` to `99-milestones.tji`; the closer wires only the
`.tji` completion marker and the note back-link.

## Effort estimate

**4d.** This task writes almost no new *mechanism* — every behavior doc 02's
frame loop promises is already shipped as a caller-owned compositor free
function or the worker pool. The 4d is the **assembly, the deadline policy, and
the persistent state**: (1) an `arbc::runtime::InteractiveRenderer` (L5) that
owns the frame-to-frame `RefinementQueue`, the persistent `CompositorCounters`,
the `WorkerPool`, the last-completed revision and the last composition time, and
threads them into the stateless compositor by pointer (doc 17:88-95); (2) the
**deadline enforcement** the worker pool and compositor both deliberately left
to the runtime (`worker_pool.hpp:33-36`, `tile_planning.hpp:163-164`, doc
17:78-80) — reading an injected `steady_clock`, parking in `wait_completions`
only to the frame deadline, and cancelling expired `BestEffort` pending renders
via `RenderCompletion::cancel`; (3) the **follow-up-frame scheduling decision**
(re-plan iff `poll_refinements` / damage collection produced a non-empty dirty
region); (4) the collect-damage front of the loop wiring `clock_advance_damage`
+ model damage through `map_damage_to_device` into a `DirtyRegion`. The premium
over a pure-glue task is the deadline/cancel policy, the multi-frame
progressive-refinement golden, and the reap/schedule concurrency case (render
thread parks in `wait_completions` while an external-async completion settles
off-thread and `poke()`s it). The deliverable is one header/impl pair
(`interactive.hpp`/`.cpp`), a unit + golden + focused-concurrency test file, two
new claims, CMake wiring, and **no design-doc delta** (see Decisions — this
realizes already-normative doc-02/16/17 promises, deviating from none).

## Inherited dependencies

**Settled:**

- `compositor.tile_planning` (DONE; the parent `runtime`'s edge,
  `65-runtime.tji:6`) — the spine the loop drives. From
  `src/compositor/arbc/compositor/tile_planning.hpp`:
  - **`render_frame_interactive`** (`:152-205`; full signature `:198-205`) — the
    synchronous resolve+plan+fill+composite driver. It plans each visible layer
    (`plan_layer`), fills a fresh-key miss **inline** via a `BestEffort`
    `RenderRequest`/`RenderCompletion`, composites the display source, and stamps
    `deadline` onto each miss request **as a value** ("never recomputed; no
    wall-clock read — enforcement is runtime policy, doc 17:60", `:161`). It is
    explicitly "Single-threaded (doc 02:135-137); the async worker pool and real
    deadline clock are `compositor.pull_service`" (`:163-164`). The loop calls it
    with the optional `pending`/`counters`/`dirty`/`composition_time` seams below.
  - **`PlannedTile`** / **`LayerTilePlan`** (`:98-122`) — the pure plan value; a
    tile whose fresh key missed sets `is_miss` regardless of the fallback shown.
  - **`TileSource { Fresh, Stale, Coarser, Placeholder }`** (`tile_planning.md`)
    — the **placeholder / degradation preference order** the note names
    (stale > coarser > transparent) is *already implemented here*, at plan time,
    behind the `prior_revision` stale probe. This task supplies the **deadline
    dimension** of doc 02:61-65 ("when the deadline nears, the frame proceeds with
    what it has"): it does not re-implement the order, it drives the loop so the
    already-chosen fallback is what shows while the sharp render is deferred.
- `compositor.refinement` (DONE 2026-07-05; direct edge). From
  `src/compositor/arbc/compositor/refinement.hpp`:
  - **`RefinementQueue`** (`:85-87`) / **`PendingTile`** (`:72-79`) — the
    caller-owned frame-to-frame registry of deferred (async) tile renders;
    `surface` + `done` are caller-owned and outlive the frame. **This task owns
    the queue** — "the *caller* (the runtime frame loop) owns the queue"
    (`:60-63`).
  - **`render_frame_interactive`'s `RefinementQueue* pending`** (`:59-71`,
    `tile_planning.hpp:166-173`) — non-null records an async miss (`render`
    returns `nullopt`) into the queue instead of dropping it; the tile still
    composites its best fallback this frame.
  - **`poll_refinements`** (`:124-140`) — drains settled arrivals: inserts each
    into the cache under `Visible` **on the render thread**, emits
    `Damage{content, tile_local_rect, TimeRange{when, when}}`, and bumps
    `follow_up_frames`. An empty poll returns **empty** damage → no follow-up
    frame. **Load-bearing caveat:** the emitted `TimeRange{when, when}` is
    `empty()`; `map_damage_to_device`'s gate reads a degenerate range as
    present-frame damage (`damage_planning.hpp:51-64`), so the loop must not drop
    arrival damage on an empty-range test.
  - **`prime_prefetch`** (`:121-122`) — the thin pan+zoom speculation driver;
    residency-only (reclassifies residents, returns a want-list, renders/inserts
    nothing).
- `cache.prefetch` (DONE 2026-07-05; direct edge). From
  `src/cache/arbc/cache/prefetch.hpp`: `pan_prefetch_ring`,
  `temporal_prefetch_ring`, and the classify-resident/report-absent driver
  **`prime_ring`** (`:132-147`). The interactive loop reaches these **through**
  `compositor::prime_prefetch` (which wraps the pan + zoom rings), not directly —
  so no direct `cache` edge is needed. Priority enum is now 5-valued
  (`Speculative < Recent < Temporal < Adjacent < Visible`).
- `runtime.threading` (DONE 2026-07-06; the `!threading` edge). From
  `src/runtime/arbc/runtime/worker_pool.hpp`:
  - **`WorkerPool`** (`:77-127`) — `submit` (`:95`), `poke() noexcept` (`:101`,
    the async-completion wake), **`wait_completions(std::optional<steady_clock::
    time_point> until)`** (`:109`, parks the render thread on a settled-count
    condition to an optional bound), `request_stop()` (`:112`), and counters
    `tasks_submitted`/`tasks_completed`/`max_in_flight_per_content` (`:115-127`).
    `worker_count == 0` (the **default**) is the inline executor.
  - **The pool is clock-free and enforces no deadline** (`:33-36`): "carries each
    task's `Deadline` value untouched and reads no clock; deadline enforcement
    (cancelling expired `BestEffort` work) is `runtime.interactive`." **This task
    is that enforcement.** The pool's "Not this task" list (`threading.md:206-216`)
    hands the frame loop, the `RefinementQueue` ownership, the transport clock,
    and deadline enforcement explicitly to `runtime.interactive`.
- Supporting compositor leaves (DONE, transitive through the above):
  - **`compositor.damage_planning`** (`damage_planning.hpp`): **`DirtyRegion`**
    (`:47-49`, `{std::vector<Rect> device_rects}` — non-null empty = "no damage →
    no work"), **`map_damage_to_device`** (`:51-64`, projects
    `Damage{content,region,range}` through the per-viewport camera; temporal gate
    `range.empty() || range.contains(now)`), **`clock_advance_damage`** (`:66-`,
    emits `Damage{content, Rect::infinite(), advanced}` for visible non-`Static`
    layers only; all-`Static` → empty), and **`invalidate_damage`** (drops damaged
    tiles across rungs). `render_frame_interactive` gained the trailing
    `const DirtyRegion* dirty` gating seam. The **follow-up-frame scheduler / the
    loop that re-invokes on damage is `runtime.interactive`** (damage_planning
    owns no persistent clock/camera/loop).
  - **`compositor.temporal_coalescing`**: `render_frame_interactive`'s trailing
    `Time composition_time` — "the runtime interactive loop threads a real sampled
    composition time here (the compositor stays stateless; sampling the transport
    clock is runtime's job)."
  - **`compositor.counters`** (`counters.hpp`): **`CompositorCounters`**
    (`:34-56`, `requests_issued`/`composites`/`follow_up_frames`) and
    **`counters_snapshot`** (`:74-82`, composes with cache hit/miss/eviction);
    caller-owned, "the persistent value lives in `runtime`" (doc 17:88-95).
- `contract` (L3, DONE), `src/contract/arbc/contract/content.hpp`:
  **`Deadline`** carries the value only, no `now()`/`expired()` (`:50-63`);
  **`RenderCompletion::cancel`/`cancelled`** are the cooperative, best-effort
  advisory the loop uses to shed expired `BestEffort` work (`:122-123`); the wake
  is runtime policy (`:117-118`).

**Pending:** none — every predecessor is landed.

## What this task is

Deliver **`arbc::runtime::InteractiveRenderer`** (L5, doc 17:60) in a new
header/impl `src/runtime/arbc/runtime/interactive.hpp` (+ `interactive.cpp`): the
deadline-bounded, damage-driven frame loop that is doc 02:49-71 ("The frame,
interactively") made concrete. It is the **second render driver** over the same
compositor (doc 02:40-41), the interactive analog of the offline free function
`render_offline` (`src/runtime/arbc/runtime/offline.hpp:13-14`) — but a **class,
not a free function**, because the interactive frame carries state *between*
frames (the pending-refinement registry, the persistent counters, the
last-completed revision, the last composition time) that the offline one-shot
does not.

The renderer owns, as the persistent state doc 17:88-95 assigns to runtime:

1. **`RefinementQueue d_pending`** — the frame-to-frame registry of async
   renders recorded this frame and drained on later frames.
2. **`CompositorCounters d_counters`** — persistent behavioral counts across
   frames; exposed via `counters()` and `stats(const TileCache&)`
   (`counters_snapshot`).
3. **`WorkerPool d_pool`** — constructed from an injected `WorkerPoolConfig`
   (default `worker_count == 0`, i.e. inline, for M3; see Decisions). Its
   `wait_completions`/`poke` are the loop's async-arrival park/wake; its `submit`
   is the injected `PullService` dispatch seam a future `compositor.pull_service`
   binds to (not exercised for real-thread miss dispatch in M3).
4. **`std::optional<std::uint64_t> d_prior_revision`** — the last-completed
   revision, threaded into `render_frame_interactive` to enable the stale probe.
5. **`std::optional<Time> d_prev_time`** — the previous frame's composition time,
   so a clock advance is `TimeRange{d_prev_time, composition_time}` fed to
   `clock_advance_damage`.
6. **An injected clock** `std::function<std::chrono::steady_clock::time_point()>`
   (default `steady_clock::now`) — the *only* wall-clock read in the loop, so
   tests drive the deadline path with a fake clock and stay wall-clock-free.

The frame method — `render_frame(state, resolve, viewport, cache, backend, pool,
target, std::span<const Damage> model_damage, Time composition_time, budget) ->
FrameOutcome{bool schedule_follow_up}` — runs doc 02's six steps as one bounded
pass:

1. **Collect damage (doc 02:51-52).** Union the caller-accumulated
   `model_damage` (drained from the model's `DamageSink` by the host — see "Not
   this task") with `clock_advance_damage(state, resolve, viewport,
   TimeRange{d_prev_time.value_or(composition_time), composition_time})` for
   temporal damage on moving layers. On the *very first* frame (`d_prev_time ==
   nullopt`) plan the whole viewport (null `DirtyRegion`).
2. **Map to device (doc 02:51,57-60).** `map_damage_to_device(state, viewport,
   collected, composition_time)` → `DirtyRegion`. If the region is empty **and**
   `d_pending` is empty → collect no work, return `schedule_follow_up == false`
   (the "No damage → no work" fast exit; zero renders, zero composites).
3. **Invalidate (doc 02:63).** `invalidate_damage(cache, collected)` drops
   damaged tiles across rungs so the re-plan sees a miss where content changed.
4. **Plan + render misses within budget (doc 02:61-65).** Derive one frame
   deadline instant `d = now() + budget`; construct the `Deadline` value stamped
   onto miss requests and the `until` park-bound from the *same* `d`. Call
   `render_frame_interactive(…, target, deadline, d_prior_revision, &d_pending,
   &d_counters, &dirty, composition_time)`: fresh-key misses fill inline (or,
   with `worker_count > 0`, via the bound `submit`), async misses are recorded
   into `d_pending` and composite their best fallback this frame.
5. **Park to the deadline; enforce it (doc 02:61-65, 140-143).**
   `d_pool.wait_completions(d)` parks for async arrivals only until `d`. On
   return (ready or timeout), **cancel expired `BestEffort` pending renders**:
   for each still-unsettled `PendingTile` whose deadline has passed at `now()`,
   call `done->cancel()` (advisory, `content.hpp:122-123`) so a worker/external
   renderer can shed it and the loop stops waiting. The frame never blocks past
   `d`.
6. **Refine + composite arrivals (doc 02:66-71).** `poll_refinements(d_pending,
   cache, &d_counters)` inserts settled arrivals and returns arrival damage;
   feed that damage through `map_damage_to_device` to decide
   `schedule_follow_up` (non-empty → a follow-up frame is owed; the sharp tile
   re-plans `Fresh` next frame). A failed/cancelled arrival is dropped by
   `poll_refinements` (no insert, no damage) — degrading to the placeholder
   policy, never thrown through the loop.
7. **Speculation (doc 02:92-93, 04:99-101).** Drive `compositor::prime_prefetch`
   for the visible plan to reclassify resident pan/zoom-ring tiles onto their
   priority class, keeping them warm across pan/zoom. Rendering the returned
   want-list is `compositor.pull_service`'s (M4), left to it.
8. **Advance state.** Set `d_prior_revision` to the frame's revision and
   `d_prev_time` to `composition_time`.

## Not this task

- **Real worker-thread miss dispatch through `PullService`.** The cache-first
  request machinery that "pulls hit the cache first, schedule on workers, inherit
  deadline + snapshot" is `compositor.pull_service` (L4,
  `35-compositor.tji`), an **M4** dependency this task does not have. For M3 the
  renderer runs `render_frame_interactive`'s inline fill (`worker_count == 0`);
  the `WorkerPool` provides the **async-completion park/wake** for externally-
  async content (a `render` that returns `nullopt` and settles off-thread), not
  parallel miss dispatch. The `submit` seam is exposed and bindable, but wiring it
  as the pull dispatch and choosing the low-priority want-list render policy is
  pull_service's.
- **The offline / export driver** — `render_offline`
  (`offline.hpp:13-14`) and `runtime.offline_sequences` (`65-runtime.tji:20-24`):
  same steps *without* deadlines, quantization, or placeholders (doc 02:73-80).
  Distinct driver; not this task.
- **Host-facing viewport/transport/monitor objects** and the `DamageSink`
  subscription lifecycle — `runtime.host_objects` (`65-runtime.tji:26-30`,
  `depends !interactive`). This task's `render_frame` consumes an *already-
  accumulated* `std::span<const Damage>`; the sink that subscribes to the model
  and accumulates damage between frames, the transport that samples the clock into
  `composition_time`, and the anchored-camera object model are host_objects'.
- **Playback transport + temporal (playback) prefetch ring** — the temporal ring
  and playback-hint horizon are `timeline.*` / M5 (`m5_video`,
  `99-milestones.tji:40-43`). The M3 interactive loop's clock drives pan/zoom over
  stills and the still-scene zero-work path; sustained playback is M5.
- **Cache concurrency hardening.** Workers (when enabled) render only into
  caller-owned surfaces; all cache inserts stay render-thread-only via
  `poll_refinements`, exactly as `runtime.threading` decided — so the parked
  `KeyedStore` hardening (`parking-lot.md`, "KeyedStore concurrency hardening")
  stays untriggered. This task introduces no worker cache access.

## Why it needs to be done

Every predecessor shipped one *piece* of doc 02's interactive frame as a pure,
caller-owned compositor free function that "returns a value rather than
scheduling" and holds no cross-frame state: `render_frame_interactive` plans and
fills and composites but is single-threaded and stamps the deadline as an inert
value (`tile_planning.hpp:161,163-164`); `poll_refinements` drains arrivals and
emits damage but "wakes no caller … schedules no render"
(`refinement.hpp:49-55`); `map_damage_to_device` / `clock_advance_damage` /
`invalidate_damage` map and gate one pass but own "no persistent camera/clock/
loop"; `WorkerPool` renders and wakes but "reads no clock and enforces no
deadline" (`worker_pool.hpp:33-36`). **Nothing yet closes the loop:** samples the
transport clock, collects damage between frames, decides the frame deadline and
enforces it, parks for arrivals within budget, and schedules the follow-up frame
that makes zooming refine rather than block. Doc 17:78-80 places exactly this —
"deadlines, frame loops, and device clocks are runtime policy" — in `runtime`,
and every predecessor named `runtime.interactive` as the owner of the frame-to-
frame state and the deadline authority they each declined. This task is that
closure: the driver that owns the persistent state (doc 17:88-95) and the
deadline policy (doc 17:78-80), assembling the settled free functions into the
responsive frame loop `m3_still_compositor` requires for "pan/zoom at frame rate
over mixed solid/raster scenes" (`99-milestones.tji:31`). `runtime.host_objects`
depends on it because the host-facing viewport/transport objects are built *over*
this loop.

## Inputs / context

- `docs/design/02-architecture.md`:
  - **`:40-41`** — "**Renderers**: two drivers over the same core. Interactive
    owns a frame loop, deadlines, and progressive refinement." — this task is that
    driver.
  - **`:49-71`** (`## The frame, interactively`) — the six steps the loop runs:
    `:51-52` collect damage / "No damage → no work"; `:53-56` resolve and cull;
    `:57-60` plan requests (tiles); **`:61-65`** render misses within budget +
    the **placeholder preference order** ("stale-revision tiles, coarser-scale
    tiles rescaled, or checkerboard/transparent, in that preference order");
    `:66-68` composite; **`:69-71`** refine ("Async results that arrive later
    produce damage for their region, scheduling a follow-up frame … progressively
    sharper content rather than blocking").
  - `:73-80` — offline contrast (no deadlines/quantization/placeholders): the
    scope boundary against `runtime.offline_sequences`.
  - `:118-137` (`## Threading model`) — planning on the render thread under a
    pinned snapshot, "never races edits and never takes a lock" (`:123-125`); the
    worker pool + per-content serialization (`:126-132`); the degenerate
    one-thread mode (`:135-137`).
  - **`:140-143`** — "A failed render request yields a diagnostic and the
    placeholder policy (hold stale tile, or transparent). Failures are reported to
    the host per-layer, not thrown through the frame loop." — the loop's
    failure-degrades discipline.
- `docs/design/16-sdlc-and-quality.md`: `:54-62` behavioral-counter tests
  ("playback of a still scene issues zero visual renders … Most claims-register
  entries about efficiency land here"); `:66-73` concurrency tests (TSan + stress
  with schedule perturbation); `:112-118` ≥90% diff coverage.
- `docs/design/17-internal-components.md`: **`:60`** the `arbc::runtime` L5 row
  ("interactive frame loop … Depends on: everything below"); **`:78-80`** "The two
  render drivers live in `runtime`, not the engines … deadlines, frame loops, and
  device clocks are runtime policy"; **`:88-95`** pure per-frame libraries take a
  caller-owned counters struct by pointer "so the persistent value lives in
  `runtime` and the library stays stateless"; `:41-44` levelization (depend only
  on strictly lower levels; CI validates the CMake + include graph).
- `docs/design/11-time-and-video.md:149,151-152` — playback zero-render promises
  (a within-native-frame advance "hits the cache (zero renders)"; a mostly-still
  scene "re-renders only the moving layers"), realized end-to-end by the loop's
  still-scene path (M3 scope: the pan/zoom-over-stills half; sustained playback is
  M5).
- `src/runtime/arbc/runtime/worker_pool.hpp:33-36,95,101,109,112,115-127` — the
  pool seams the loop drives, and the clock-free / deadline-is-runtime contract.
- `src/runtime/arbc/runtime/offline.hpp:10-14` — `render_offline`, the sibling
  one-shot driver whose header comment already names "The interactive renderer —
  frame loop, deadlines, progressive refinement — is a separate driver."
- `src/runtime/arbc/runtime/housekeeping_thread.hpp` — the injected-clock /
  `steady_clock` idiom (`:50-53`) this renderer's fake-clock injection mirrors for
  wall-clock-free deadline tests.
- `src/compositor/arbc/compositor/tile_planning.hpp:98-205` — `PlannedTile`,
  `LayerTilePlan`, `plan_layer`, and the full `render_frame_interactive`
  signature (`:198-205`) with `pending`/`counters`/`dirty`/`composition_time`.
- `src/compositor/arbc/compositor/refinement.hpp:72-79,85-87,121-122,124-140` —
  `PendingTile`, `RefinementQueue`, `prime_prefetch`, `poll_refinements` (and the
  `TimeRange{when,when}` empty-range caveat, `:127-129`).
- `src/compositor/arbc/compositor/damage_planning.hpp:47-79` — `DirtyRegion`,
  `map_damage_to_device`, `clock_advance_damage`, `invalidate_damage`.
- `src/compositor/arbc/compositor/counters.hpp:34-82` — `CompositorCounters`,
  `counters_snapshot`, `CompositorStats`.
- `src/contract/arbc/contract/content.hpp:50-63,117-123,229-230` — `Deadline`
  (value-only), the wake-is-runtime note, `RenderCompletion::cancel`/`cancelled`
  (advisory), `Content::render`.
- `src/runtime/CMakeLists.txt:1-9` — `DEPENDS base model contract compositor
  pool` (compositor + contract + model + pool already present; the compositor
  headers transitively provide `TileCache`/`Backend`/`SurfacePool`/`Surface`, as
  `offline.hpp` already relies on — **no new `DEPENDS` edge**). This task appends
  `interactive.cpp` to `SOURCES`, `arbc/runtime/interactive.hpp` to
  `PUBLIC_HEADERS`, `t/interactive.t.cpp` to the component test.
- `tests/claims/registry.tsv` — existing predecessor claims this loop
  re-exercises end-to-end: `:68` `#miss-becomes-deadline-request`, `:69`
  `#degraded-fallback-preference-order`, `:88` `#async-arrival-emits-damage`,
  `:89` `#quiescent-refinement-schedules-no-frame`, `:95`
  `#damage-maps-to-device-dirty-regions`, `:96`
  `#clock-advance-damages-only-moving-layers`, `:94`
  `#compositor-exposes-behavioral-counters`; this task appends two rows.
- `tasks/parking-lot.md` — three profiling-dependent items whose trigger is "the
  interactive renderer exists / is profiled" (SurfacePool byte-budget eviction;
  recycling evicted backend surfaces; and the KeyedStore concurrency hardening,
  kept untriggered by the no-worker-cache-access decision). This task builds the
  renderer those items name but resolves none of them — they stay human-call
  profiling items (see Open questions).

## Constraints / requirements

- **Levelization (doc 17:41-44,:60).** `InteractiveRenderer` is L5
  `arbc::runtime`, reaching the compositor free functions and the render contract
  only through public `compositor`/`contract` headers and the same-component
  `worker_pool.hpp`. **No new `DEPENDS` edge** — `compositor`, `contract`,
  `model`, `pool` are already present, and `TileCache`/`Backend`/`SurfacePool`/
  `Surface` arrive transitively through the compositor headers exactly as
  `offline.hpp` already relies on. The CI dependency check stays green.
- **Runtime owns all persistent state; the compositor stays stateless (doc
  17:88-95).** The `RefinementQueue`, `CompositorCounters`, prior revision, and
  previous time live in the renderer and are threaded into the compositor free
  functions by pointer/value per call. The renderer adds no state to the
  compositor and holds no `model::DamageSink` subscription (that is host_objects').
- **Deadline is the loop's, and the only wall-clock read is the injected clock
  (doc 17:78-80, `content.hpp:50-63`, `worker_pool.hpp:33-36`).** The renderer
  reads its injected `steady_clock` source exactly to (a) compute the frame
  deadline instant `d`, (b) bound `wait_completions(d)`, and (c) test
  `PendingTile` expiry for `cancel`. The `Deadline` value it stamps and the `d`
  park-bound derive from the *same* sampled instant. No other component reads a
  clock; tests inject a fake clock so the deadline path is deterministic.
- **Never block past the deadline (doc 02:61-65).** `wait_completions` is bounded
  by `d`; on timeout the frame composites the best-available fallback (the
  placeholder order `render_frame_interactive` already chose) and returns.
  Expired `BestEffort` pending renders are `cancel()`ed (advisory); the loop owes
  them a follow-up frame only if/when they later settle.
- **"No damage → no work" (doc 02:51).** A frame whose collected damage maps to an
  empty `DirtyRegion` **and** whose `RefinementQueue` is empty plans nothing,
  renders nothing, composites nothing (does not clear `target`), and schedules no
  follow-up frame — observable as zero deltas on
  `requests_issued`/`composites`/`follow_up_frames`.
- **Follow-up frame is scheduled iff there is damage (doc 02:69-71).**
  `render_frame` returns `schedule_follow_up == true` exactly when
  `poll_refinements` (or newly collected damage) yields a non-empty device dirty
  region; a quiescent poll (no settled arrivals, empty queue) schedules nothing.
  The caller (host_objects' transport) is responsible for actually re-invoking;
  this task returns the decision, it does not own the event loop.
- **Failure degrades, never propagates (doc 02:140-143).** A render answered by
  `fail` (or an expired-and-cancelled request) is dropped by `poll_refinements`
  with no insert and no damage; the loop keeps the stale/coarser fallback and
  never throws through the frame. No exception escapes `render_frame` on a
  per-layer render failure.
- **First cross-frame `SurfacePool` / cache caller (`parking-lot.md`).** The
  renderer is the first sustained cross-frame consumer the parked SurfacePool-
  eviction and surface-recycling items were waiting on. It must **function
  correctly** under sustained pan/zoom (distinct temp sizes accumulating), but it
  adds **no** eviction/recycling policy — those stay profiling-gated human calls
  (Open questions). Correctness, not a memory-policy optimization, is the bar.
- **Concurrency surface is the reap/wake, not new shared state.** The renderer's
  own concurrency is: the render thread parks in `wait_completions` while an
  externally-async completion settles off-thread and `poke()`s it. The heavy
  TSan/stress of the pool itself is `runtime.threading`'s (already landed); this
  task adds one focused, deterministic reap/schedule test (synchronized on the
  completion, no sleep) that also builds clean under the `tsan` preset. No new
  cross-thread shared mutable state is introduced beyond the pool's.
- **Public header compiles standalone; CI diff coverage ≥90% (doc 16:112-118).**

## Acceptance criteria

- **Unit + golden tests — deterministic, fake-clock, no sleep — in
  `src/runtime/t/interactive.t.cpp` (new, Catch2, registered via
  `arbc_component_test`).** With a stub `Content` (solid-fill `render` that can be
  configured sync or async, records requested region/scale/time, and — for the
  async case — retains its `RenderCompletion` for the test to settle later via a
  worker or directly) and a hand-built `DocRoot`/`Viewport`, an injected fake
  clock, and a caller-supplied per-frame budget:
  - **Still scene does no work (behavioral counter):** a frame with an empty
    `model_damage` span, a clock advance over an all-`Static` scene (or a sub-
    native-period advance), and an empty `RefinementQueue` returns
    `schedule_follow_up == false` and leaves `counters().requests_issued()`,
    `composites()`, and `follow_up_frames()` **unchanged** (zero deltas); `target`
    is not cleared. Enforces the new `#interactive-still-scene-schedules-no-frame`
    claim and re-enforces `11-time-and-video#clock-advance-damages-only-moving-
    layers` (`registry.tsv:96`) and `02-architecture#quiescent-refinement-
    schedules-no-frame` (`:89`) through the assembled loop.
  - **Deadline is never blocked past + expired best-effort is cancelled:** submit
    a frame whose stub answers a miss **asynchronously** (never settles within
    budget); advance the fake clock past the frame deadline; assert `render_frame`
    returns without blocking, the tile composited its best fallback (stale/coarser/
    placeholder per the plan), the pending render's `RenderCompletion::cancelled()`
    is true after the frame, and `schedule_follow_up` reflects that nothing
    settled. A companion case lets the async stub settle *before* the deadline
    (via a `worker_count > 0` pool that `poke()`s) and asserts the frame reaps it,
    inserts it, and `schedule_follow_up == true`. Enforces the new
    `#interactive-frame-loop-bounded-by-deadline` claim.
  - **Progressive-refinement golden (byte-exact):** a deterministic two-frame
    sequence over a stub scene — frame 1 has a fresh-key miss the stub answers
    async, so it composites a coarse/placeholder fallback (recorded to a golden);
    settle the async completion; frame 2 re-plans the now-resident tile `Fresh`
    and composites it **sharp** (a second golden). Assert both target buffers are
    **byte-identical** to committed goldens, pinning the coarse-then-refine loop
    end-to-end and re-enforcing `02-architecture#degraded-fallback-preference-
    order` (`registry.tsv:69`) and `#async-arrival-emits-damage` (`:88`) through
    the driver. (Deterministic rendering → byte-exact goldens, doc 16; no
    tolerance.)
  - **Damage-gated re-plan touches only changed tiles:** after a first full frame,
    feed a `model_damage` span confined to one content region; assert the second
    frame's `requests_issued` / `composites` deltas count only the tiles
    intersecting that region (the rest stay cache hits), re-enforcing
    `02-architecture#damage-maps-to-device-dirty-regions` (`registry.tsv:95`)
    through the loop.
  - **State advances across frames:** assert `d_prior_revision` enables the stale
    probe on frame N+1 (a revision bump between frames shows the prior-revision
    tile as the `Stale` fallback, not a placeholder) and that `d_prev_time`
    produces the correct `clock_advance_damage` `TimeRange` on the next advance.
- **Focused concurrency test — outcome assertions only, builds under `tsan` —
  same file, guarded like the pool concurrent tests.** A `worker_count > 0` pool;
  the render thread runs `render_frame` and parks in `wait_completions` while an
  async stub content settles its `RenderCompletion` **on a worker thread** and
  `poke()`s. Drive a fixed number of frames; assert **outcomes only**: every
  arrival that settled before its deadline was reaped and inserted exactly once,
  every cancelled one left no insert, `schedule_follow_up` matched the arrival
  outcome each frame, and no crash/hang. Synchronized on the completion counter /
  `wait_completions` return — **no sleep, no timing assertion.** Runs green under
  `dev`, `asan`, and `ctest --preset tsan`. (The exhaustive pool stress is
  `runtime.threading`'s; this test pins the *loop's* reap/schedule interaction.)
- **Claims (register + `enforces:` tags)** appended to
  `tests/claims/registry.tsv` (format `<claim-id><TAB><description>`), enforced
  from the tests above:
  - `02-architecture#interactive-frame-loop-bounded-by-deadline` — "The
    interactive frame loop never blocks past its frame deadline: it parks for
    async arrivals only until the deadline, cancels expired `BestEffort` pending
    renders via `RenderCompletion::cancel`, and composites the best-available
    fallback in the placeholder preference order rather than waiting; the sharp
    result refines on a follow-up frame and a failed or cancelled render degrades
    to the placeholder policy, never thrown through the loop." (doc 02:61-71,
    140-143; doc 17:78-80) — enforced by the deadline/cancel and golden tests.
  - `02-architecture#interactive-still-scene-schedules-no-frame` — "An interactive
    frame with no model damage, no clock-advance damage (all-`Static` or a
    sub-native-frame advance), and no settled pending refinements collects empty
    damage, plans nothing, and issues zero render requests and zero composites
    through the assembled loop, scheduling no follow-up frame
    (`requests_issued`/`composites`/`follow_up_frames` deltas all 0)." (doc 02:51;
    doc 16:54-62) — enforced by the still-scene behavioral-counter test.
  - Re-enforced via a second `enforces:` tag (no new row) through the loop path:
    `02-architecture#degraded-fallback-preference-order` (`:69`),
    `#async-arrival-emits-damage` (`:88`),
    `#quiescent-refinement-schedules-no-frame` (`:89`),
    `#damage-maps-to-device-dirty-regions` (`:95`),
    `11-time-and-video#clock-advance-damages-only-moving-layers` (`:96`).
- **No design-doc delta.** This task realizes already-normative promises (doc
  02:40-41,49-71,140-143; doc 17:78-80,88-95; doc 16:54-62). It adds no contract
  surface, no new dependency, and deviates from no designed behavior, so — unlike
  `runtime.threading`'s doc-03 delta — **no `docs/design/` edit rides this
  commit.** (Confirm at close that no header comment introduces new normative
  content; if a reviewer reads one as a delta, that is a parking-lot judgment
  call, not a doc edit made by fiat.)
- **Behavioral-counter discipline (doc 16:54-62).** Every assertion is on a
  `CompositorCounters` count, a `RenderCompletion::settled()`/`cancelled()`/
  `take()` result, a byte-exact golden, or `schedule_follow_up`; synchronization
  is via `wait_completions` and the injected fake clock. No test reads the real
  wall clock or sleeps to synchronize.
- **Component wiring & CI dependency check:** `src/runtime/CMakeLists.txt` adds
  `interactive.cpp` to `SOURCES`, `arbc/runtime/interactive.hpp` to
  `PUBLIC_HEADERS`, and `t/interactive.t.cpp` to the component test; **no
  `DEPENDS` change**; the header compiles standalone; the doc-17 dependency check
  passes.
- **Gate green (build + tests in dev + ASan/UBSan + TSan).** All tests pass under
  `dev`, `asan`, and `tsan` presets; ≥90% diff coverage on changed lines.

## Decisions

- **`InteractiveRenderer` is a stateful L5 class, not a free function like
  `render_offline`.** The offline driver is a one-shot pure evaluation
  (`offline.hpp:13-14`); the interactive frame carries state *between* frames —
  the `RefinementQueue` of in-flight async renders, the persistent counters, the
  last-completed revision (for the stale probe), and the previous composition time
  (for `clock_advance_damage`). Doc 17:88-95 assigns exactly that persistent state
  to runtime ("the persistent value lives in `runtime` and the library stays
  stateless"), so a class owning it and threading it into the stateless compositor
  by pointer is the shape the levelization prescribes. *Rejected:* a free function
  taking every piece of state by reference from the caller — it would push the
  frame-to-frame bookkeeping (which queue, which revision, which prev-time) onto
  every caller and re-derive the loop's invariants at each call site; a class that
  owns the invariant is the simpler, safer abstraction and matches how
  `HousekeepingThread` (`housekeeping_thread.hpp`) owns its own loop state.
- **Deadline enforcement lives here and reads the only clock; the `Deadline`
  value and the `wait_completions` bound derive from one sampled instant.** Both
  the pool (`worker_pool.hpp:33-36`) and the tiled driver
  (`tile_planning.hpp:161,163-164`) deliberately carry the `Deadline` as an inert
  value and read no clock, naming `runtime.interactive` as the enforcer; doc
  17:78-80 puts device clocks in the render driver. So the renderer samples its
  injected clock **once per frame** to get `d = now() + budget`, stamps the
  `Deadline` value from `d` onto miss requests, and bounds `wait_completions(d)`
  and the `PendingTile`-expiry `cancel` on the same `d` — one instant, two uses,
  no drift. Injecting the clock (default `steady_clock::now`) keeps the whole
  deadline path deterministic under a fake clock, honoring doc 16:54-62's
  no-wall-clock rule. *Rejected:* reading `steady_clock` at multiple points in the
  loop — it would let the stamped deadline and the park bound disagree and make
  tests timing-dependent. *Rejected:* pushing deadline enforcement into the pool —
  it would duplicate the frame loop's authority and put a clock in a library-level
  primitive, the exact split `worker_pool.hpp:33-36` was written to avoid.
- **M3 runs the inline pool (`worker_count == 0`); the `WorkerPool`'s role here is
  the async-completion park/wake, not parallel miss dispatch.** `render_frame_
  interactive` fills fresh-key misses inline and is single-threaded by
  construction; the machinery that dispatches misses to workers is
  `compositor.pull_service` (L4), an **M4** dependency this task does not have. So
  for M3 the renderer keeps `worker_count == 0` (inline, byte-identical to the
  landed driver) and uses `wait_completions`/`poke` purely to park for
  **externally-async** content (`render` → `nullopt`, settling off-thread). The
  `submit` seam is exposed and bindable so pull_service can later inject
  real-thread dispatch with no change here. *Rejected:* defaulting to a
  hardware-concurrency pool and dispatching misses through `submit` now — that
  needs pull_service's cache-first/budget policy (not in scope) and would couple
  this leaf's landing to M4; the inline default lands the responsive loop for M3
  and lets real threading switch on as the doc-02:135-137 optimization later.
- **The loop returns a `schedule_follow_up` decision; it does not own the event
  loop.** `poll_refinements` and `map_damage_to_device` return values rather than
  scheduling, and every predecessor left "*whether* to schedule a follow-up frame"
  to the runtime (`damage_planning.md`, `refinement.hpp:52-55`). Keeping
  `render_frame` a pure "run one frame, report whether another is owed" function —
  with the actual re-invocation owned by host_objects' transport / the host's
  event loop — preserves that seam and keeps `render_frame` testable without a
  live event loop. *Rejected:* having the renderer own a driving thread that
  re-invokes itself — that is the transport's job (`runtime.host_objects`), and
  baking it in would make the loop untestable as a unit and duplicate the
  transport's cadence policy.
- **No worker cache access, so the parked `KeyedStore` hardening stays
  untriggered — inherited from `runtime.threading`, re-affirmed here.** The
  renderer inserts arrivals into the cache only on the render thread via
  `poll_refinements`; when workers are enabled they render solely into caller-
  owned surfaces. This is the same async-fill model `runtime.threading` chose
  (render-into-surface, insert-on-render-thread), which keeps `KeyedStore` single-
  thread-confined and the parked hardening item untriggered. This task lands the
  live cross-frame caller but forces no concurrent cache. *Rejected:* letting the
  loop insert arrivals off the render thread — it would trigger the parked
  hardening for no benefit `poll_refinements` does not already deliver.
- **No design-doc delta.** Every behavior here is already normative: the frame
  loop and placeholder order (doc 02:49-71), failure-degrades (doc 02:140-143),
  deadlines-are-runtime (doc 17:78-80), persistent-state-in-runtime (doc
  17:88-95), still-scene-zero-renders (doc 16:54-62). The task assembles and
  realizes them, adds no contract surface, and deviates from none, so no
  `docs/design/` edit is warranted (contrast `runtime.threading`, which added
  `Content::render_thread_safe()` and rode a doc-03 delta). Making the *default*
  worker_count inline and returning a `schedule_follow_up` value are
  implementation shapes consistent with — not amendments to — the docs.

## Open questions

(none — all decided.)

Three standing items this task **enables the evaluation of but does not resolve**,
surfaced in the return summary for the human's awareness (already in
`tasks/parking-lot.md`; none is a WBS leaf, each is profiling/judgment-gated):

- **SurfacePool byte-budget / one-frame-trim eviction** — its trigger was "the
  interactive/video renderer exists and is profiled under sustained camera
  motion." This task builds that renderer; whether accumulation of distinct
  temp-size pool entries under sustained pan/zoom is a real problem is a
  profiling-dependent human call, not decidable now.
- **Recycling evicted backend surfaces back into `SurfacePool`** — same trigger
  (interactive-renderer profiling) and same profiling-dependent judgment; it
  couples the cache eviction path to the pool lifecycle and pays only if profiling
  shows churn.
- **`KeyedStore` concurrency hardening** — stays *untriggered by design* (workers
  never touch the cache; inserts stay render-thread-only). If a future task ever
  moves cache inserts onto workers, that item re-activates — but nothing here
  forces it.

None is encoded as a WBS task (each is a "revisit when profiled / when the model
changes" judgment call, exactly the self-perpetuating shape the parking lot
exists to keep out of the WBS).

## Status

**Done** — 2026-07-06.

- `src/runtime/arbc/runtime/interactive.hpp` + `src/runtime/interactive.cpp` — `arbc::runtime::InteractiveRenderer` (L5): deadline-bounded, damage-driven frame loop owning `RefinementQueue`, `CompositorCounters`, `WorkerPool`, `d_prior_revision`, `d_prev_time`, and an injected clock; `render_frame` runs doc 02's six steps (collect damage, map to device, invalidate, plan+render within budget, park-to-deadline+cancel expired BestEffort, poll+follow-up decision).
- `src/runtime/t/interactive.t.cpp` — unit tests (still-scene counters, deadline/cancel+reap, damage-gated re-plan, state-advance+stale-probe+moving-layer), byte-exact progressive-refinement golden (coarse→sharp two-frame sequence), and TSan-clean concurrency test (off-thread settle/poke, synchronized on completion counter, no sleep).
- `src/runtime/CMakeLists.txt` — `interactive.cpp` added to `SOURCES`, `arbc/runtime/interactive.hpp` to `PUBLIC_HEADERS`, `t/interactive.t.cpp` to the component test; no new `DEPENDS` edge.
- `tests/claims/registry.tsv` — two new rows: `02-architecture#interactive-frame-loop-bounded-by-deadline` and `02-architecture#interactive-still-scene-schedules-no-frame`; six existing claims re-enforced through the assembled loop (`#degraded-fallback-preference-order`, `#async-arrival-emits-damage`, `#quiescent-refinement-schedules-no-frame`, `#damage-maps-to-device-dirty-regions`, `11-time-and-video#clock-advance-damages-only-moving-layers`).
- No design-doc delta — every behavior realizes already-normative doc 02/16/17 promises.
- Tech-debt registered: `compositor.expose_visible_plan` — surface the visible `LayerTilePlan` from `render_frame_interactive` so the loop can drive `compositor::prime_prefetch` without re-planning; gates M4 pull_service speculation.
