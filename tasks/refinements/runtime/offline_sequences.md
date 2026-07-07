# runtime.offline_sequences — Offline sequence renderer + export (frame-sequence loop + single-revision snapshot + exact no-degrade renders)

## TaskJuggler entry

`tasks/65-runtime.tji:21-26` → `runtime.offline_sequences` ("Offline sequence
renderer + export"), the third leaf under `task runtime`. Its own edge is
`depends !threading, timeline.temporal_placement` (`65-runtime.tji:24`); the
parent `task runtime` adds `depends compositor.tile_planning`
(`65-runtime.tji:6`) as an inherited edge. Note line:

> "Frame-sequence loop: snapshot per frame time, exact renders,
> revision-consistent output while editing continues; pairs with the audio
> export monitor. Docs 02/11/12."

It is the milestone leaf `m5_video → runtime.offline_sequences`
(`tasks/99-milestones.tji:40-42`). Every other `m5_video` dependency —
`timeline.transport`, `timeline.temporal_cache`, `timeline.playback_hints`,
`kinds.imageseq_plugin`, `surfaces.provided_surfaces`,
`compositor.temporal_placement_culling` — is already landed (all DONE
2026-07-07, per the recent commit stream). **`runtime.offline_sequences` is
therefore `m5_video`'s last remaining dependency**: on completion the closer
adds `complete 100` to this leaf *and* to `m5_video` in
`tasks/99-milestones.tji` (milestones do not infer completion from their
dependencies — doc `tasks/refinements/README.md:69-72`), and appends the
`Refinement:` back-link to this task's note.

## Effort estimate

**2d.** Doc 11:209-213 sets the ceiling explicitly: "a sequence render is a loop
over output frame times — snapshot, set time, exact render — and the existing
snapshot mechanism is precisely what keeps frame N from seeing frame N+1's
edits. **This path needs essentially nothing new beyond `time` in the
requests.**" Almost everything this task drives already exists: the one-shot
exact driver `render_offline` (`src/runtime/arbc/runtime/offline.hpp:19-20`,
which already pins a snapshot, allocates a working-space target, and calls the
compositor); the temporal-aware tiled compositor `render_frame_interactive`
(`src/compositor/arbc/compositor/tile_planning.hpp:293-299`) that already
threads `composition_time` and applies per-layer span-cull + time-map + achieved
-time coalescing (`tile_planning.cpp:161,270,285`); the snapshot/pin primitive
(`Document::pin()`, `src/runtime/arbc/runtime/document.hpp:34`); the reusable
`WorkerPool` (`src/runtime/arbc/runtime/worker_pool.hpp`); and the temporal tile
cache. The genuinely new work is thin and concentrated: a small stateful driver
that (1) **pins one revision for the whole export** and renders every frame
against it, (2) **steps a caller-supplied output frame-time series** into
`composition_time`, (3) enforces the **exact, no-degrade discipline** (every miss
rendered to completion; no deadline, no stale/coarser/placeholder fallback), and
(4) **reuses a long-lived `SurfacePool` + `TileCache` across frames** so a
mostly-still export does not re-rasterize every layer every frame (doc 02:82-85).
The deliverable is one header/impl pair, one unit + golden + focused-concurrency
test file, three claims, a possible one-line no-degrade selector on the
compositor entry (realizing existing doc-02 behavior, no design-doc delta), and
CMake wiring (no new `DEPENDS` edge). The ~1d premium over trivially looping
`render_offline` is the single-revision pinning across a whole sequence, the
exact/no-degrade guarantee under a slow renderer, cross-frame cache reuse, and
the concurrent-writer test.

## Inherited dependencies

**Settled:**

- `runtime.threading` (DONE 2026-07-06, `65-runtime.tji:8-12`, this task's
  direct `!threading` edge) — shipped **`arbc::WorkerPool`**
  (`src/runtime/arbc/runtime/worker_pool.hpp:78`): `submit(RenderTask)` (`:96`),
  the completion-wake `poke()`/`wait_completions(until)` (`:102,110`), the
  byte-identical `worker_count == 0` inline mode, and graceful stop/join.
  **Offline has no deadline, so it can genuinely block-and-fan-out**: drive
  misses onto workers and `wait_completions(nullopt)` until every one settles —
  real parallel *exact* renders, no fallback. Its no-cache-from-workers rule
  (workers render into caller-owned surfaces; cache inserts stay on the driver
  thread) carries over unchanged.
- `timeline.temporal_placement` (DONE 2026-07-07,
  `tasks/refinements/timeline/temporal_placement.md`) — put `TimeRange span` and
  `TimeMap time_map` on the layer record (defaulting to "always present /
  identity", so a still is the degenerate case) and the transactional setters.
  It is model storage only; **time is *evaluated* in the compositor
  frame-planning path**, driven by the `composition_time` this task supplies.
- `compositor.temporal_placement_culling` (DONE 2026-07-07, transitively via
  `m5_video`; not a direct WBS edge but the seam this task drives) — added the
  per-layer temporal walk inside `render_frame_interactive`: span-cull via
  `present_in_span(span, parent_time)` (`src/base/arbc/base/rational_time.hpp:163`,
  used at `src/compositor/tile_planning.cpp:270`), time-map evaluation
  `layer.time_map.evaluate(composition_time)` returning
  `expected<Time, TimeError>` (`tile_planning.cpp:285`, overflow culls the
  layer), and achieved-time coalescing `content->quantize_time(time)`
  (`tile_planning.cpp:161`). This is exactly the "set time" step of doc 11:210;
  this task loops it over output frame instants.
- `compositor.tile_planning` (DONE, the parent `runtime`'s edge,
  `65-runtime.tji:6`) — `render_frame_interactive(...)`
  (`src/compositor/arbc/compositor/tile_planning.hpp:293-299`, `Time
  composition_time = Time::zero()` at `:298`), the tiled/cached temporal driver;
  and the exact synchronous full-frame `render_frame(...)`
  (`src/compositor/arbc/compositor/compositor.hpp:41`) that today's
  `render_offline` uses.
- `contract` + `model` (L3/L2, DONE) — the snapshot/pin machinery this task's
  consistency guarantee rests on: `DocRoot` the immutable pinnable version
  (`src/model/arbc/model/model.hpp:37`), `DocRoot::revision()` (`:45`), `using
  DocStatePtr = std::shared_ptr<const DocRoot>` (`:101`), `Model::current()`
  (`:163`) publishing a new immutable version per commit; and the host-facing
  `Document::pin()` / `Document::resolve()`
  (`src/runtime/arbc/runtime/document.hpp:34-35`). `StateHandle snapshot` rides
  each render request lock-free (`tasks/refinements/contract/snapshot_pins.md`,
  which names `runtime.offline_sequences` as a consumer at `:95,:228`).

**Pending:** none — every predecessor is landed.

## What this task is

Deliver **`arbc::SequenceRenderer`** (L5, doc 17:60), a stateful driver over the
compositor core that renders a *sequence of exact frames at chosen output times
against one frozen document revision* — the offline/export counterpart to the
interactive frame loop. In a new header/impl
`src/runtime/arbc/runtime/offline_sequence.hpp` (+ `offline_sequence.cpp`):

1. **Construction pins the export.** The constructor takes a `Document&`,
   `Viewport`, and `Backend&`, and immediately pins one snapshot
   (`DocStatePtr d_pinned = document.pin()`). Every frame this instance renders
   reads `*d_pinned` — a single, frozen `DocRoot::revision()` — so an export is
   internally consistent even while the host keeps editing on the writer thread
   (doc 02:77-80, doc 11:211-213). The instance also owns the **cross-frame
   state**: a long-lived `SurfacePool` (temp reuse across frames), a `TileCache`
   (so a mostly-still export reuses tiles instead of re-rasterizing every layer
   every frame, doc 02:82-85; Timed content coalesces sub-native-grid output
   instants onto one cached tile), a `CompositorCounters`, and — when parallel
   exact rendering is requested — a `WorkerPool`.
2. **`render_frame_at(Time composition_time) -> expected<std::unique_ptr<Surface>,
   SurfaceError>`** — the core method. It allocates a frame target at the pinned
   version's `working_space()` (`state->working_space()`, capability-honest via
   the `SurfaceError` value path exactly as `render_offline` does,
   `offline.cpp:15-16`), then drives the compositor at `composition_time` in
   **exact, no-degrade mode**: every visible miss is rendered to completion
   (blocking; inline or fanned out to the `WorkerPool` and reaped via
   `wait_completions(nullopt)` — **no deadline**), and only fresh, exact-scale,
   current-revision tiles are composited — **never a stale, coarser, or
   placeholder tile**, regardless of how long a render takes (doc 02:73-85). The
   per-layer span-cull + time-map + `quantize_time` walk that
   `compositor.temporal_placement_culling` already built runs against
   `composition_time`; this method threads the time and enforces exactness. It
   returns one owned `Surface` per frame; the caller keeps or hands it to an
   encoder.
3. **`render_sequence(std::span<const Time> frame_times, FrameSink&& sink)`** —
   a thin convenience loop calling `render_frame_at` for each supplied instant
   and handing each frame to the caller's sink. The **host owns the output
   frame-time series** (computed from its chosen output rate and time range —
   frame rate is a property of the render, not the model, doc 11:38-40; instants
   are integer flicks, doc 11:41-43) and owns encoding/muxing (doc 12:157). A
   small helper `frame_times_over(TimeRange range, Rational output_rate)` may be
   offered for the common fixed-rate case, reusing the exact rational stepping
   the transport already uses — but the loop does **not** require a `Transport`
   (offline supplies each `Time` directly into `composition_time`, the seam that
   already accepts a raw instant).

**Not this task:**

- **The audio export monitor.** Sample-exact mix over a time range is
  `audio.export_monitor` (`tasks/45-audio.tji:29-33`, milestone `m6_audio`),
  which per its note "is driven by the offline frame loop under snapshots." This
  task builds that frame loop and its single-revision pinning; the audio monitor
  hooks into the same per-frame-time + pinned-snapshot structure later. Its
  `AudioRequest` mix machinery, the `[t0,t1)` window sampling, and A/V pairing are
  `audio.*`, not here. (Note doc 12:160-167's device-clock mastering —
  "video chases audio" — is the *device/interactive* discipline; **export has no
  device clock**, so the frame loop is the sole driver, `12-audio.md:154-155`.)
- **File writing, containers, muxing, encoding.** The engine core hands back
  `Surface`s (and, for audio, sample blocks); "Muxing audio with exported frames
  is the host's business (or a container plugin's), not the core's"
  (`12-audio.md:157-158`); the host "owns surfaces, drives frames"
  (`02-architecture.md:6-14`). No codec, container, or file I/O in this task.
- **Codec-backed video content.** A real video kind "brings ffmpeg-class
  dependencies" and "remains a plugin outside the core per doc 10"
  (`11-time-and-video.md:288-291`). The in-core temporal reference kind is
  `org.arbc.imageseq` (`kinds.imageseq_plugin`, DONE), which this task's tests use
  to exercise `achieved_time` / spans.
- **The interactive frame loop, deadlines, transport, playback hints.** Damage
  collection, deadline enforcement, progressive refinement, the degraded fallback
  order, and transport sampling are `runtime.interactive`
  (`tasks/refinements/runtime/interactive.md`, which draws this scope boundary at
  doc `02:73-80`, `:295`). Offline inverts every one of those: no deadline, no
  degradation, no transport, exact only.

## Why it needs to be done

`m5_video` is "Timeline, spans, transport, achieved_time coalescing, playback
hints, image-sequence playback … **and offline frame sequences**"
(`99-milestones.tji:41-43`). Every other piece has landed: the model stores
temporal placement, the compositor culls spans and evaluates time maps, the tile
cache coalesces achieved-time, the transport drives interactive playback, and the
imageseq plugin plays back content-provided surfaces. What is still missing is the
**export path** — the driver that turns "render one frame at the current version"
into "render a *deterministic, revision-consistent sequence* of exact frames at
chosen output instants while the artist keeps editing." Today's `render_offline`
renders exactly one frame of the *current* version with no time argument
(`offline.hpp:10-13,19-20`) — it neither steps time nor guarantees a whole
sequence comes from one revision. Doc 02:77-80 calls out precisely why that guarantee
matters: "needed for 'export while editing' and for video where frame N must not
see frame N+1's edits." Without a single-revision pin held across the whole
sequence, an edit committed between frame 4 and frame 5 would leak into the
output, producing a temporally-inconsistent export. And doc 02:73-85 defines the
export's correctness contract — exact scale, every request rendered to completion,
no placeholders — which the deadline-driven interactive loop deliberately does not
provide. Building this as its own leaf, on top of the already-landed temporal
compositor, realizes doc 11:209-213's "nothing new beyond `time` in the requests"
and closes `m5_video`.

## Inputs / context

- `docs/design/02-architecture.md`:
  - **`:40-41`** — "two drivers over the same core. Interactive owns a frame
    loop, deadlines, and progressive refinement. **Offline owns exact
    evaluation.**"
  - **`:73-85`** (`## The frame, offline`) — the normative contract:
    "**Same steps without deadlines, quantization, or placeholders: exact scale,
    every request rendered to completion, output guaranteed to reflect exactly
    revision-consistent content.** A **snapshot** mechanism (freeze revisions
    during a frame) keeps a frame consistent even if the scene is being mutated
    concurrently — needed for 'export while editing' and for video where frame N
    must not see frame N+1's edits." And `:82-85`: "The offline path still uses
    the tile cache when content stability allows … but correctness rules are
    strict: **only exact-scale, current-revision entries qualify.**"
  - **`:118-137`** (`## Threading model`) — scene single-writer publishing
    immutable versions (`:120-122`); the compositor "reads the scene under a
    snapshot — concretely, a pinned document version (doc 14) — so planning never
    races edits and never takes a lock" (`:123-125`).
  - `:6-14` — the host "owns surfaces, drives frames"; the engine hands frames
    to it.
- `docs/design/11-time-and-video.md`:
  - `:38-43` — "**frame rate is a property of the render, not the model**"; a
    composition's preferred rate/duration are authoring hints, "nothing is
    clocked by them"; instants are integer flicks (1/705,600,000 s), rates are
    exact rationals.
  - **`:186-191`** — frame planning "computes each layer's local time by composing
    time maps down the tree … then snaps each `Timed` layer's local time to that
    content's grid via `quantize_time` before the tile-cache lookup" — the
    per-frame temporal walk this task loops.
  - **`:209-213`** — "a sequence render is a loop over output frame times —
    snapshot, set time, exact render — and the existing snapshot mechanism is
    precisely what keeps frame N from seeing frame N+1's edits. This path needs
    essentially nothing new beyond `time` in the requests."
  - `:232-233` — "revisions track *edits*, time tracks *playback*; the cache key
    carries both" — the export pins the edit axis (one revision) and steps the
    playback axis (frame times).
- `docs/design/12-audio.md`:
  - **`:154-158`** (`## Export monitor`) — "sample-exact rendering of the mix
    over a time range, **driven by the offline renderer's frame loop**; snapshot
    semantics per doc 02 keep an export consistent with concurrently-edited
    scenes. **Muxing audio with exported frames is the host's business (or a
    container plugin's), not the core's.**"
  - `:160-167` — clock mastering is the *device* discipline; export has no device
    clock (relevant only as the contrast that makes the offline loop the driver).
  - `:219-221` — the export monitor is v1 scope, sequenced before the device
    monitor ("no realtime pressure").
- `docs/design/17-internal-components.md`:
  - **`:60`** — `arbc::runtime` is **L5**; its contents include "interactive
    frame loop, **offline/export drivers**"; "Depends on: **everything below**."
  - **`:78-86`** — "**The two render drivers live in `runtime`, not the engines.**
    The engines are libraries over pinned versions; deadlines, frame loops, and
    device clocks are runtime policy." The sequence driver is that runtime policy.
  - `:41-44` — depend only on strictly lower levels; CI validates CMake + include
    graph. `:95-98` — a pure per-frame library takes a caller-owned counters
    struct by pointer so the persistent value lives in `runtime` — the sequence
    driver owns the `CompositorCounters`.
- `docs/design/16-sdlc-and-quality.md` — `:15-25` claims register
  (`<doc-stem>#<slug>` + `enforces:` tag); `:54-62` behavioral counters not
  wall-clock; `:66-73` concurrency tests under TSan; deterministic rendering →
  byte-exact goldens; `:112-118` ≥90% diff coverage.
- `src/runtime/arbc/runtime/offline.hpp:10-20` / `src/runtime/offline.cpp:7-29` —
  the existing one-shot exact driver: pins (`offline.cpp:9`), allocates the target
  at `state->working_space()` with the `SurfaceError` value path
  (`offline.cpp:15-16`), builds a `SurfacePool` (`:24`), calls the exact
  `render_frame` (`:25`). The header comment already anticipates this task: "a
  looping renderer would hold a long-lived pool and reuse across frames"
  (`offline.hpp:20-24` region / `offline.cpp:20-23`).
- `src/runtime/arbc/runtime/interactive.hpp:68,97-101,118,120-139` — the sibling
  stateful driver: `InteractiveRenderer` (class, not free function, because it
  carries cross-frame state), `render_frame(... Time composition_time ...)`
  (`:100`), `worker_pool()` accessor (`:118`), the owned state block
  (`:120-139`). The pattern this task mirrors — minus deadlines, damage, and
  refinement, plus the single-revision pin.
- `src/compositor/arbc/compositor/tile_planning.hpp:293-299` —
  `render_frame_interactive` with `Time composition_time` (`:298`); impl
  `src/compositor/tile_planning.cpp:161` (`quantize_time`), `:270`
  (`present_in_span`), `:285-287` (`time_map.evaluate`, overflow culls). The
  temporal seam this task drives per frame.
- `src/model/arbc/model/model.hpp:37,45,101,163` and
  `src/runtime/arbc/runtime/document.hpp:34-35` — the snapshot/pin primitive
  (`DocRoot`, `revision()`, `DocStatePtr`, `Model::current()`, `Document::pin()`).
- `src/runtime/arbc/runtime/worker_pool.hpp:78,96,102,110` — `WorkerPool`,
  `submit`, `poke`, `wait_completions` — the optional parallel-exact path.
- `src/surface/arbc/surface/backend.hpp:31` (`make_surface` → `expected<…,
  SurfaceError>`) and `src/surface/arbc/surface/surface_pool.hpp` — target
  allocation and cross-frame temp reuse.
- `src/runtime/CMakeLists.txt` — `DEPENDS base model contract compositor pool`
  (all already present; **no new edge**); this task appends `offline_sequence.cpp`
  to `SOURCES`, `arbc/runtime/offline_sequence.hpp` to `PUBLIC_HEADERS`, and
  `t/offline_sequence.t.cpp` to the component test.
- `tests/claims/registry.tsv` — `:75` `#static-tiles-survive-clock`, `:102`
  `#achieved-time-coalescing-issues-zero-renders`, `:124` `#span-cull-is-half-open`,
  `:135` `#compositor-retimes-request-through-time-map` (claims this task
  re-exercises through the offline driver); this task appends three rows.
- `tasks/refinements/contract/snapshot_pins.md:95,228` and
  `tasks/refinements/runtime/interactive.md:236,295` — predecessors that already
  name `runtime.offline_sequences` and draw its scope boundary.

## Constraints / requirements

- **Levelization (doc 17:41-44,:60,:78-86).** `SequenceRenderer` is L5
  `arbc::runtime`, reaching the compositor, contract, model, and surface through
  their public headers exactly as `render_offline` and `InteractiveRenderer` do.
  **No new `DEPENDS` edge** — `compositor`, `contract`, `model`, `pool` are
  already present. The CI dependency check stays green.
- **Single-revision snapshot for the whole sequence (doc 02:77-80, doc
  11:211-213).** The instance pins one `DocStatePtr` at construction and renders
  **every** frame against that one `DocRoot::revision()`. A writer thread
  committing new revisions during the export must not change any exported frame.
  This is stronger than the interactive driver's per-frame revision handling: an
  export is consistent *across* frames, not merely within one.
- **Exact, no-degrade renders (doc 02:73-85).** Every visible miss is rendered to
  completion (no deadline); only fresh, exact-scale, current-revision tiles are
  composited. No stale-revision, coarser-scale, or placeholder/checkerboard tile
  ever appears in an exported frame, no matter how slow a render is. The frame
  blocks until sharp.
- **Tile cache reuse across frames (doc 02:82-85).** The instance holds one
  `TileCache` across the sequence so a mostly-static export does not re-rasterize
  every layer every frame, and an output rate finer than a Timed content's native
  grid coalesces onto one cached tile per native frame (achieved-time coalescing).
  Only exact-scale, current-revision entries qualify — the compositor's existing
  temporal-key correctness rule governs reuse.
- **Frame rate is the caller's, in flicks (doc 11:38-43).** The driver samples the
  composition at caller-supplied output instants; it declares no rate itself and
  clocks nothing. No `Transport` is required — each `Time` feeds `composition_time`
  directly.
- **No cache access from workers (reaffirms `runtime.threading`).** When the
  parallel-exact path fans misses onto the `WorkerPool`, workers render only into
  caller-owned surfaces; cache inserts stay on the driver thread. Keeps the parked
  `KeyedStore` concurrency-hardening item (`parking-lot.md`) untriggered — the same
  render-into-surface / insert-on-driver-thread model `runtime.threading` chose.
- **Output boundary (doc 12:157, doc 02:6-14).** The driver returns owned
  `Surface`s; it writes no files, muxes nothing, and links no codec/container.
  The host (or a plugin) owns encoding.
- **Capability honesty via values (doc 10, mirrors `offline.cpp:12-18`).** A
  backend that cannot store the pinned version's working space yields a
  `SurfaceError` value from `render_frame_at`, never an abort.
- **Deterministic rendering → byte-exact goldens; concurrency → TSan (doc 16).**
  A fixed scene exported at a fixed rate/range at rung-aligned scale is
  byte-reproducible; the concurrent-writer consistency guarantee is exercised
  under TSan. CI diff coverage ≥90%.

## Acceptance criteria

- **Unit + golden tests — deterministic, no sleep — in
  `src/runtime/t/offline_sequence.t.cpp` (new, Catch2, registered via
  `arbc_component_test`).** Using a small stub scene (a `Static` solid-fill layer
  plus a `Timed` stub whose `quantize_time` snaps to a coarse native grid and
  whose `render` records the requested content-local time), a stub `Backend`, and
  a `Viewport` at scale 1.0 (rung 0, so the render is exact-scale with no remainder
  resample):
  - **Byte-exact sequence golden:** export the scene over a fixed `[t0,t1)` range
    at a fixed output rate at scale 1.0; assert each frame's pixels are
    byte-identical to a stored reference and that re-running the export produces
    identical bytes (determinism). Enforces
    `11-time-and-video#offline-sequence-steps-frame-times-exactly`.
  - **Exact / no-degrade under a slow renderer:** give the stub a `render` that
    would blow any interactive deadline; assert the exported frame still composites
    the **fresh exact** result (byte-exact golden) and that a behavioral counter
    of degraded/placeholder composites reads **zero** — no stale/coarser/checker
    tile ever appears. Enforces
    `02-architecture#offline-frame-renders-exactly-no-degrade`.
  - **Achieved-time coalescing across the sequence:** export a Timed layer at an
    output rate several times finer than its native grid over a warm shared
    `TileCache`; assert exactly one render per native frame (subsequent
    sub-native-grid output instants are cache hits, `hits()` up, `misses()` flat) —
    the offline path reusing the temporal key. Re-enforces
    `11-time-and-video#achieved-time-coalescing-issues-zero-renders`
    (`registry.tsv:102`) via a second `enforces:` tag (no new row). A companion
    all-`Static` export asserts the clock-invariant keys reuse across frames,
    re-enforcing `11-time-and-video#static-tiles-survive-clock` (`:75`).
  - **Temporal placement through the offline driver:** a layer with a half-open
    span present for only part of the range appears in exactly the in-span frames
    and is absent (culled) outside; a layer with a non-identity `time_map` is
    requested at its retimed content-local instant. Re-enforces
    `11-time-and-video#span-cull-is-half-open` (`:124`) and
    `11-time-and-video#compositor-retimes-request-through-time-map` (`:135`) via
    second `enforces:` tags.
  - **Capability honesty:** a backend that rejects the pinned working space makes
    `render_frame_at` return a `SurfaceError` value (asserted on the `expected`),
    not abort.
- **Focused concurrency test — outcome assertions only, runs under `tsan` —
  same file.** Construct a `SequenceRenderer` (pinning revision R), then on a
  second thread commit N revisions to the `Model` (spin-synchronized on an atomic
  `go`, `std::this_thread::yield()` at fixed-stride perturbation points — the
  idiom from `interactive.md` / `housekeeping_thread.t.cpp`) **while** the export
  loop runs. Assert **outcomes only**: every exported frame reads
  `DocRoot::revision() == R` (the pin held), the pixels equal the single-revision
  golden (no mid-export edit leaked in), and no crash/hang. **No timing
  assertion.** Runs green under `dev`, `asan`, and **`ctest --preset tsan`**. (The
  exhaustive `WorkerPool` stress is `runtime.threading`'s; this test pins the
  export's snapshot-consistency under concurrent edits, the one concurrency
  property this task introduces.)
- **Claims (register + `enforces:` tags)** appended to `tests/claims/registry.tsv`
  (format `<claim-id><TAB><description>`), enforced from the tests above:
  - `02-architecture#offline-sequence-pins-single-revision` — "An offline sequence
    renderer pins one document revision at construction and renders every frame of
    the sequence against it, so a writer thread committing new revisions during the
    export changes no exported frame; frame N never observes frame N+1's edits and
    the whole sequence is revision-consistent." (doc 02:77-80, doc 11:211-213) —
    enforced by the concurrent-writer test.
  - `02-architecture#offline-frame-renders-exactly-no-degrade` — "An offline frame
    renders every visible cache miss to completion with no deadline and composites
    only fresh, exact-scale, current-revision tiles; it never substitutes a stale,
    coarser, or placeholder tile regardless of how long a render takes, so the
    exported pixels are the exact result." (doc 02:73-85) — enforced by the
    slow-renderer no-degrade golden + the zero-degraded-composites counter.
  - `11-time-and-video#offline-sequence-steps-frame-times-exactly` — "A sequence
    render over a half-open time range at an output rate samples the composition at
    exactly the output frame instants (integer flicks), threading each as
    composition time; per-frame span-cull, time-map evaluation, and quantize_time
    coalescing are applied, so the sequence is deterministic and byte-reproducible
    and a Timed layer yields its native frame at each output instant." (doc
    11:38-43,186-191,209-213) — enforced by the byte-exact sequence golden + the
    coalescing test.
  - Second `enforces:` tags (no new rows) on `11-time-and-video#achieved-time-
    coalescing-issues-zero-renders` (`:102`), `#static-tiles-survive-clock` (`:75`),
    `#span-cull-is-half-open` (`:124`), and `#compositor-retimes-request-through-
    time-map` (`:135`), exercising each through the offline driver.
- **No design-doc delta.** Docs 02:73-85, 11:209-213, and 12:154-158 already
  specify offline sequence rendering completely; this task realizes them. If the
  implementer finds `render_frame_interactive` needs a one-line no-degrade /
  exact-only selector to suppress the interactive fallback, that is a
  **compositor-local** change realizing doc 02:75's already-normative behavior —
  not a design-doc amendment (contrast `runtime.threading`'s doc-03 delta, which
  added a new contract method).
- **Behavioral-counter discipline (doc 16:54-62).** Every assertion is on pixels
  (goldens), a `DocRoot::revision()` value, a `CompositorCounters` field
  (renders / hits / misses / degraded-composites), or a `RenderCompletion`
  settle — never a wall clock; synchronization is via `wait_completions` and
  atomic `go` flags.
- **Component wiring & CI dependency check:** `src/runtime/CMakeLists.txt` adds
  `offline_sequence.cpp` to `SOURCES`, `arbc/runtime/offline_sequence.hpp` to
  `PUBLIC_HEADERS`, and `t/offline_sequence.t.cpp` to the component test; **no
  `DEPENDS` change**; the header compiles standalone; the doc-17 dependency check
  passes.
- **Gate green (build + tests in dev + ASan/UBSan + TSan).** The concurrency test
  runs green under `dev`, `asan`, and `tsan`; ≥90% diff coverage on changed lines.

## Decisions

- **`SequenceRenderer` is a stateful L5 class that pins one revision for the whole
  export, not a per-call free function.** The consistency guarantee doc 02:77-80
  demands — "frame N must not see frame N+1's edits" — is a property of the
  *sequence*, so the pinned `DocStatePtr` must outlive every frame and be shared by
  all of them; and the cross-frame `SurfacePool` + `TileCache` reuse doc 02:82-85
  wants (don't re-rasterize a still scene every frame) needs the same long-lived
  home. A class holds exactly this state, mirroring `InteractiveRenderer`
  (`interactive.hpp:68,120-139`), which is a class for the same "carries state
  across frames" reason. The existing one-shot `render_offline` free function
  stays as-is (a still-image exact render of the current version). *Rejected:* a
  free `render_sequence(...)` that re-pins per call — it would either re-pin each
  frame (letting a mid-export edit leak in, the exact failure doc 11:211-213 warns
  against) or force the caller to thread the pin, pool, and cache through every
  call; a class owns them once. *Rejected:* extending `render_offline` in place to
  take a time — it renders one frame with no cross-frame state, so overloading it
  with sequence lifetime blurs the one-shot/sequence distinction the two render
  drivers already draw.
- **Pin once for the whole export, not once per frame.** An export is a single
  artifact; a revision change mid-export would make late frames reflect content
  the early frames do not, corrupting the result. Pinning at construction and
  reading `*d_pinned` for every frame gives the sequence-wide consistency doc
  02:77-80 / doc 11:211-213 require, and the pin is lock-free and outlives any
  later commit (`model.hpp:161-163`). *Rejected:* per-frame `Document::pin()` — it
  would pick up whatever revision is current at each frame, producing a
  temporally-inconsistent export whenever the artist edits during a long render.
- **Reuse `render_frame_interactive` in exact/no-degrade mode; do not build a
  second time-aware exact path in the compositor.** `render_frame_interactive`
  already threads `composition_time` and applies the full per-layer temporal walk
  (span-cull, time-map, `quantize_time`) plus the temporal tile cache
  (`tile_planning.cpp:161,270,285`); doc 11:213 says the offline path "needs
  essentially nothing new beyond `time` in the requests." Driving it with
  `Exactness::Exact` requests, **no deadline** (every miss rendered to completion,
  inline or via `wait_completions(nullopt)`), and no degraded fallback yields the
  exact discipline doc 02:73-85 defines, while reusing the most machinery. The
  offline exact-scale rule (doc 02:75,84) is honored at rung-aligned export scales
  — the canonical fixed-resolution export at scale 1.0 (rung 0) renders exact with
  no remainder resample. *Rejected:* threading `composition_time` into the exact
  full-layer `render_frame` (`compositor.hpp:41`) and duplicating the temporal walk
  there — it re-implements span-cull + time-map + coalescing that
  `render_frame_interactive` already has, and loses the tile-cache reuse doc
  02:82-85 wants for a mostly-still export, for no correctness gain at the export
  scales that matter. *Rejected:* a fully separate offline compositor pipeline —
  far more than a 2d runtime task and against doc 11:213's "nothing new."
- **Default to inline exact rendering; parallel-exact via `WorkerPool` is a
  correctness-neutral opt-in.** With `worker_count == 0` every miss renders inline
  to completion before composite — the simplest path to a byte-deterministic
  golden, and exactness is order-independent so parallel fan-out produces identical
  pixels. Because offline has **no deadline**, the driver can genuinely block-and-
  fan-out (`submit` misses, `wait_completions(nullopt)` until all settle) for
  parallel exact renders when a caller opts in — a speed knob, not a behavior
  change, reusing the pool `runtime.threading` shipped and its no-cache-from-workers
  rule. *Rejected:* mandating parallel rendering — it couples the golden's
  determinism to scheduling and buys nothing for small test scenes; the pool is an
  opt-in optimization.
- **The audio export monitor is a separate leaf that reuses this loop; not
  bundled here.** `audio.export_monitor` (`45-audio.tji:29-33`) `depends
  !mix_engine`, sits in `m6_audio`, and per its own note "is driven by the offline
  frame loop under snapshots." Building the video frame-sequence loop + single-
  revision pin here gives that task its driver; bundling sample-exact audio mixing
  into a 2d runtime task would cross into the audio engine (L4) and the `m6_audio`
  milestone. *Rejected:* implementing a combined A/V export now — wrong milestone,
  wrong component boundary, and the audio engine's mix/lookahead machinery is not
  yet a dependency of this task.
- **No file/container/codec output in the core.** The driver returns `Surface`s;
  "Muxing … is the host's business (or a container plugin's), not the core's"
  (`12-audio.md:157-158`) and codec-backed content "remains a plugin outside the
  core" (`11-time-and-video.md:288-291`). *Rejected:* a built-in encoder/container
  — it would pull ffmpeg-class dependencies into L5 against doc 10's dependency
  policy and doc 02's host-owns-output split.

## Open questions

(none — all decided.)

One item this task **addresses by design rather than resolves**, surfaced in the
return summary for the human's awareness (not encoded as a WBS leaf): strict
exact-scale export at *arbitrary fractional* output scales. This task honors doc
02:75's "exact scale" at rung-aligned scales (the canonical fixed-resolution
export at scale 1.0 / rung 0 renders with no remainder resample), which covers the
M5 export use case; a ladder-bypass exact render for arbitrary non-power-of-2
export scales is a compositor capability that no current use case demands. It is a
compositor concern, not a runtime one, and there is no WBS leaf to add now — if a
future export use case requires sub-rung exact scale, it becomes a concrete
compositor render-path task at that point.

## Status

**Done** — 2026-07-07.

- Delivered `arbc::SequenceRenderer` in `src/runtime/arbc/runtime/offline_sequence.hpp` + `src/runtime/offline_sequence.cpp`: pins one `DocStatePtr` at construction, steps a caller-supplied `std::span<const Time>` into `composition_time`, enforces exact/no-degrade discipline (every miss rendered to completion, no deadline), and reuses a long-lived `SurfacePool` + `TileCache` across frames.
- Added `frame_times_over(TimeRange, Rational)` helper for the common fixed-rate case in the same header/impl pair.
- Added host-facing `Document::set_layer_span` / `Document::set_layer_time_map` wrappers (`src/runtime/arbc/runtime/document.hpp` + `src/runtime/document.cpp`) as the minimal seam for temporal test setup.
- Added `Exactness exactness` trailing selector and `degraded_composites` counter bump to `render_frame_interactive` (`src/compositor/arbc/compositor/tile_planning.hpp` + `src/compositor/tile_planning.cpp`) to realize the exact/no-degrade guarantee; new `degraded_composites` counter in `src/compositor/arbc/compositor/counters.hpp`.
- Wired `offline_sequence.cpp` / `offline_sequence.hpp` / `t/offline_sequence.t.cpp` into `src/runtime/CMakeLists.txt` (no new `DEPENDS` edge).
- Unit + golden tests in `src/runtime/t/offline_sequence.t.cpp`: byte-exact sequence, exact/no-degrade (zero `degraded_composites`), achieved-time coalescing, all-Static clock-invariance, half-open span-cull, time-map retiming, capability-honesty, parallel-exact byte-identity, and concurrent-writer single-revision-pin (TSan-clean).
- Appended 3 new claims to `tests/claims/registry.tsv`: `02-architecture#offline-sequence-pins-single-revision`, `02-architecture#offline-frame-renders-exactly-no-degrade`, `11-time-and-video#offline-sequence-steps-frame-times-exactly`; plus second `enforces:` tags on 4 existing claims (coalescing, static-clock, span-cull, retime).
