# timeline.transport — Transport

## TaskJuggler entry

`task transport "Transport"` in
[`tasks/40-time.tji`](../../40-time.tji) (lines 19–24), inside the
`timeline` container. Verbatim note:

> Per-viewport clock: play/pause/seek/rate (negative allowed), loop bounds;
> multiple transports observing one composition at different times. Doc 11.

```
task transport "Transport" {
  effort 2d
  allocate team
  depends !temporal_placement
  note "Per-viewport clock: play/pause/seek/rate (negative allowed), loop
        bounds; multiple transports observing one composition at different
        times. Doc 11."
}
```

## Effort estimate

**2d** (`tasks/40-time.tji:20`). Pure addition, no existing behaviour to
migrate: a new value type plus its tests, wired to the one render-driver
seam (`InteractiveRenderer::render_frame`) that already accepts a
transport-sampled time. Doc 11 sizes it "Moderate; pure addition"
(`docs/design/11-time-and-video.md:239-240`, new-machinery item 3).

## Inherited dependencies

- **`timeline.temporal_placement`** — **settled** (landed 2026-07-07,
  `complete 100` at `tasks/40-time.tji:15`; refinement
  [`temporal_placement.md`](temporal_placement.md)). Put `span` +
  `time_map` on `LayerRecord` (`src/model/arbc/model/records.hpp:74-75`)
  with transactional setters `set_span` / `set_time_map`
  (`src/model/arbc/model/model.hpp:252,259`). That refinement explicitly
  deferred *"compose time maps down the tree or cull by span at frame
  time"* to `timeline.transport` and the pull pipeline
  (`temporal_placement.md:69,76-83,204-206`) — this task is the sampler it
  named.
- **`timeline.rational_time`** (transitive, via temporal_placement) —
  **settled** (`complete 100` at `tasks/40-time.tji:9`; refinement
  [`rational_time.md`](rational_time.md)). Landed the exact-rational
  `Time` / `Rational` / `TimeMap` / `ComposedTimeMap` vocabulary in
  `arbc::base`. rational_time forecast that *"`transport` and
  `playback_hints` sample and drive the composition time these maps
  consume"* (`rational_time.md:96`) and that reverse playback via negative
  rate is first-class (`rational_time.md:208-210`).
- **`contract.temporal_fields`** (inherited from parent `timeline`,
  `tasks/40-time.tji:4`) — **settled** (`complete 100`,
  `tasks/25-contract.tji:24`). Landed `RenderRequest.time` and
  `RenderResult.achieved_time`
  (`src/contract/arbc/contract/content.hpp:77-113`) — the field the
  transport's sampled time ultimately fills.
- **`model.transactions`** (inherited from parent `timeline`) —
  **settled** (`complete 100`, `tasks/10-model.tji:25`).

No **pending** inherited dependencies — all four are landed at `HEAD`.

## What this task is

Build the **transport**: a per-viewport playback clock that owns the
playhead (an instant on the composition's time axis), a rational playback
`rate` (negative allowed for reverse), a `pause` state, and optional
half-open loop bounds. It exposes `play` / `pause` / `seek` / `set_rate` /
`set_loop` controls and an `advance(real_elapsed)` step that moves the
playhead by the real elapsed duration scaled by the rate, wrapped into the
loop. Its sampled playhead (`position()`) is the `composition_time` already
threaded into `InteractiveRenderer::render_frame`
(`src/runtime/arbc/runtime/interactive.hpp:97-101`, whose comment at `:92`
already labels that argument *"the transport-sampled content-local time"*).
Being per-viewport value state, several transports drive one shared
document at different instants without any new machinery — the composition
is a stateless function of time, so independence is free.

This task delivers the **clock type and its semantics only**. Bundling the
transport into the host-facing viewport object (camera + transport +
monitor attachment, re-anchor events) is `runtime.host_objects`
(`tasks/65-runtime.tji:27-31`, `depends !interactive`); external
hardware-clock mastering of the transport by an audio monitor
(`docs/design/01-core-concepts.md:103-106`) is audio-milestone scope
(doc 12). This task provides the plain seek/set-position surface those
later tasks drive.

## Why it needs to be done

Every temporal promise below the transport is already built and tested
against a **directly-supplied** `composition_time`: achieved-time
coalescing, clock-advance temporal damage, static-tile survival, span
culling. Nothing yet *produces* that time from a playback state — the
interactive tests hand `render_frame` a literal instant. The transport is
the missing producer that turns "here is a time" into "here is a clock the
host plays, pauses, scrubs, and reverses," closing the interactive-playback
story of doc 11 and unblocking:

- **`timeline.playback_hints`** (`tasks/40-time.tji:32-37`,
  `depends !transport`) — derives `playback_hint(direction, rate, horizon)`
  from the transport's play state to feed the temporal prefetch ring.
- **`runtime.host_objects`** (`tasks/65-runtime.tji:27-31`) — wires the
  transport into the host viewport/monitor object model.
- **Milestone M5 "Video"** (`tasks/99-milestones.tji:42-45`) depends on
  `timeline.transport` directly.

## Inputs / context

### Design docs (normative — doc 16's executable-spec discipline)

- **`docs/design/11-time-and-video.md`** — primary.
  - The transport definition and its now-specified semantics (playhead /
    rate / pause independence; wall-clock-free advance; half-open looping;
    unclamped seek): **`11:88-115`** (the paragraph plus the semantics block
    added by this task's design-doc delta, below).
  - The spatial↔temporal symmetry table (viewport camera ↔ viewport
    transport): **`11:24`**.
  - Frame planning "samples the transport's current composition time":
    **`11:174-180`**.
  - `RenderRequest.time` / `RenderResult.achieved_time` the sampled time
    feeds: **`11:117-159`**.
  - Reverse-playback rounding is sign-symmetric (ties-to-even):
    **`11:48-51`**; negative rate first-class in the time map:
    **`11:66-67`**.
  - Playback hints are the *next* task's consumer, issued by the transport:
    **`11:160-166`** (scope boundary).
- **`docs/design/01-core-concepts.md`** — the viewport's transport
  attribute (**`01:98-101`**); the audio monitor mastering the transport
  (**`01:103-106`**, deferred); the offline renderer as a deadline-free
  viewport (**`01:112-113`**).
- **`docs/design/02-architecture.md`** — the concurrency model the
  transport lives in: scene single-writer, compositor reads a pinned
  snapshot, "v1 may degenerate to everything on one thread" (**`02:120-137`**).
- **`docs/design/17-internal-components.md`** — levelization: transport is
  **L5 `arbc::runtime`** (graph **`17:22-25`**; runtime row **`17:60`**,
  "viewport/transport/monitor objects … | everything below"); "the two
  render drivers live in `runtime`, not the engines … device clocks are
  runtime policy" (**`17:84-86`**); `base` holds the `Time`/rational
  vocabulary (**`17:48`**).
- **`docs/design/16-sdlc-and-quality.md`** — no-wall-clock test rule,
  behavioural counters, claims register (**`16:54-62`**).

### Source seams this task extends (all current at `HEAD`)

- **Time vocabulary (`arbc::base`, L0):**
  `src/base/arbc/base/time.hpp` — `struct Time { std::int64_t flicks; }`
  (`:11-20`), `struct TimeRange` half-open with `contains()` (`:29-54`).
  `src/base/arbc/base/rational_time.hpp` — `class Rational` (`:36-100`),
  `struct TimeMap` with `expected<Time,TimeError> evaluate(Time)` (`:108-118`,
  evaluate at `:115`), `struct ComposedTimeMap` (`:126-156`),
  `struct TimeError` (`:23-28`).
  `src/base/arbc/base/expected.hpp` — the faults-as-values channel.
- **The consumer seam (`arbc::runtime`, L5):**
  `src/runtime/arbc/runtime/interactive.hpp:97-101` —
  `InteractiveRenderer::render_frame(..., Time composition_time, ...)`;
  the argument doc-comment at `:92`; `d_prev_time` (the clock-advance
  memory) at `:126`; `std::optional<Time> previous_time()` at `:113`; the
  wall-clock `Clock` deadline source at `:79` (distinct from media time).
  `src/runtime/interactive.cpp:42,54` — the `advanced` range
  `{prev_time, composition_time}` the loop already forms from successive
  samples.
- **Camera-side counterpart (`arbc::compositor`, L4):**
  `src/compositor/arbc/compositor/compositor.hpp:15-28` — `struct Viewport`
  carries `width/height/camera/anchor` and **no time field**; media time is
  passed separately. The transport is the temporal sibling of this camera,
  kept out of the L4 struct by levelization.
- **Build:** `src/runtime/CMakeLists.txt` — the `arbc_add_component(NAME
  runtime … DEPENDS base model contract compositor pool)` declaration a new
  `transport.{hpp,cpp}` joins; helper `cmake/ArbcComponent.cmake`
  (`arbc_add_component`, `arbc_component_test`). Level gate
  `scripts/check_levels.py` (`ALLOWED["runtime"]` already includes `base`).
- **Downstream/sibling tasks:** `tasks/40-time.tji:32-37`
  (`playback_hints`), `tasks/65-runtime.tji:27-31` (`host_objects`),
  `tasks/65-runtime.tji:21-25` (`offline_sequences`).

### Predecessor / sibling conventions followed

- Value types in `base`, *policy* in `runtime` — mirrors how
  `InteractiveRenderer` (frame-loop policy) lives in runtime while `Time`
  lives in base.
- Wall-clock injection: the transport reads no clock itself, exactly as
  `InteractiveRenderer` takes an injected `Clock` and samples it once
  (`interactive.hpp:76-81`, doc 16:54-62).
- Faults-as-values for rate overflow, reusing `expected<…,TimeError>`
  (`rational_time.md` Decision on overflow-as-value).
- Behavioural counters / claims register, never wall-clock timing
  (`temporal_cache.md`, `temporal_placement.md` acceptance patterns).

## Constraints / requirements

1. **Placement — `arbc::runtime` (L5), not a new component, not `base`.**
   Add `src/runtime/arbc/runtime/transport.hpp` + `transport.cpp` to the
   existing `arbc_runtime` component (doc 17:60,84-86: transport is runtime
   *policy*; `base` holds only the time *vocabulary*). It must **not** be a
   field of the L4 `compositor::Viewport` (levelization forbids L4→L5, and
   doc 11 keeps camera edits and clock advance orthogonal).
2. **Depends only on `base`.** The header includes only `arbc/base/time.hpp`,
   `arbc/base/rational_time.hpp`, `arbc/base/expected.hpp` — a subset of the
   runtime component's already-allowed deps; `scripts/check_levels.py` must
   stay green with no new `DEPENDS`/`ALLOWED` edge.
3. **State model — playhead, rate, pause, loop are independent.** Playhead is
   a `Time`; `rate` a `Rational` (negative allowed, retained across pause);
   `pause` a distinct boolean (**not** `rate == 0`); loop an
   `std::optional<TimeRange>` (half-open). Sampling the playhead
   (`position()`) is a pure read.
4. **Advance is wall-clock-free and exact.** `advance(Time real_elapsed)`
   moves the playhead by `real_elapsed × rate`, computed in exact rational
   arithmetic with one ties-to-even leaf rounding — reuse the existing
   `TimeMap{in=0, rate, offset=0}.evaluate(real_elapsed)` path (no new base
   math), inheriting its sign-symmetric reverse-playback rounding
   (`11:48-51`) and its overflow-as-`TimeError` (`rational_time.hpp:115`).
   A paused advance moves the playhead **zero** flicks. `advance` returns
   `expected<Time, TimeError>` (never throws, never wraps).
5. **Looping — half-open, wrap on advance only.** With loop `[in, out)`, a
   forward advance reaching/passing `out` re-enters via true modulo at `in`
   (an advance longer than one loop length still lands in `[in, out)`);
   reverse wraps symmetrically at `in`; a degenerate loop (`out <= in`) is
   rejected/ignored (no wrap). With no loop, advance is unbounded. Half-open
   matches the span half-open convention (`11:62`).
6. **Seek is exact and unclamped.** `seek(Time)` sets the playhead to the
   requested instant regardless of loop bounds or pause — scrubbing must
   reach any instant, including outside the loop window (`11:88-93` two
   viewports at arbitrary different times).
7. **Not concurrency-touching.** The transport is per-viewport host-owned
   value state mutated only by the driving thread (like the camera,
   `02:120-137`); only an immutable sampled `Time` crosses to the render
   thread by value. It holds no shared mutable state, owns no thread, and is
   copyable. It therefore carries **no** TSan/stress obligation (doc 16's
   concurrency-coverage rule does not apply — justified by design, not
   omission). External hardware-clock mastering (`01:103-106`) is deferred;
   the seek surface is what a future master will drive.
8. **Deterministic, no wall-clock read** anywhere in the type or its tests
   (doc 16:54-62).

## Acceptance criteria

Unit tests in `src/runtime/t/transport.t.cpp` (Catch2, tier-2 exact
equality; wired in `src/runtime/CMakeLists.txt` via `arbc_component_test`),
plus one runtime integration test. New claims land in
`tests/claims/registry.tsv` with `// enforces: <claim-id>` comments
(both-direction validation by `scripts/check_claims.py`):

1. **`11-time-and-video#transport-advance-scales-real-time-by-rate`** — a
   `Time` element. Advancing a playing transport by real elapsed Δ at rate
   `r` moves the playhead by exactly `round_ties_even(Δ·r)` flicks; a
   negative rate steps backward by the same magnitude (sign-symmetric); a
   realistic rate (e.g. `24000/1001`, `1/2`) is exact; a pathological rate
   that overflows returns `TimeError`, never wraps. Enforced by a numeric
   test over hand-picked and seeded (`mt19937`) rate/Δ pairs.
2. **`11-time-and-video#transport-loop-wraps-half-open`** — with loop
   `[in, out)`: a forward advance to/past `out` re-enters in `[in, out)` via
   true modulo (including an advance longer than one loop length); a reverse
   advance past `in` wraps at `in`; a transport with no loop advances
   unbounded; a degenerate `[out ≤ in)` loop applies no wrap.
3. **`11-time-and-video#transport-pause-and-seek-are-exact`** — a paused
   transport's `advance` is a no-op (playhead unchanged, delta zero) and
   `play()` resumes at the pre-pause rate; `seek(t)` sets the playhead to
   exactly `t` regardless of loop bounds or pause state (scrub-anywhere).
4. **`11-time-and-video#transports-observe-composition-independently`** —
   two transports over one document are independent: advancing/seeking one
   never mutates the other, and rendering the shared document through each
   yields that transport's own instant. Pins doc 11's per-viewport claim.
5. **Integration (behavioural counter, no new claim):** a runtime test that
   drives `render_frame`'s `composition_time` from a real `Transport` over a
   warm all-`Static` scene and asserts `requests_issued == 0` across a
   play→advance cycle (a sub-native-frame advance) — re-enforcing the
   existing **`02-architecture#interactive-still-scene-schedules-no-frame`**
   / **`11-time-and-video#static-tiles-survive-clock`** claims now that the
   time is transport-produced, proving the transport wires into the loop
   without regressing the zero-render promise. Behavioural counter only,
   never wall-clock (doc 16:54-62).

No byte-exact golden applies: the transport produces time, not pixels
(mirrors `rational_time.md`, which had no golden). CI gates unchanged:
`scripts/check_levels.py`, `scripts/check_claims.py`,
`diff-cover --fail-under=90`, clang-format, warning-free
`tj3 project.tjp`.

**Design-doc delta (this task):** `docs/design/11-time-and-video.md:88-115`
gains a "The transport's semantics are:" block specifying the three points
doc 11 previously named but left open — playhead/rate/pause independence,
wall-clock-free rational advance, and half-open loop wrapping with unclamped
seek. Recorded here per doc 16's same-commit rule; the closer's commit
carries it.

**No new WBS leaf deferred.** The two follow-ups this task's boundary names
— `timeline.playback_hints` and `runtime.host_objects` — already exist as
WBS leaves depending (transitively) on transport; M5 already lists it.
External hardware-clock mastering is a doc-12/host-objects design item, not
agent-implementable clock work, and is surfaced to the parking lot below,
not encoded as a task.

## Decisions

1. **Transport is an L5 `arbc::runtime` value type in a new
   `transport.{hpp,cpp}`, depending only on `base`.**
   *Rejected: a new component* — doc 17:60 already assigns transport to
   `runtime`; a new object library would add a levelization edge for no
   isolation benefit.
   *Rejected: putting it in `base` beside `Time`* — doc 17:48,84-86 draw the
   line at vocabulary-vs-policy: `Time`/`Rational` are base vocabulary, a
   *clock that plays them* is runtime policy (the same reason the render
   drivers live in runtime, not the engines).
   *Rejected: a `Time` field on `compositor::Viewport`* — L4 may not see L5,
   and doc 11 keeps camera edits and clock advance orthogonal invalidation
   axes (`11:180-184`); the compositor stays time-agnostic beyond the sampled
   `Time` passed by value.
2. **Pause is a boolean distinct from `rate`, not `rate == 0`.**
   *Rejected: model pause as `rate == 0`* — doc 11 lists play/pause and rate
   as separate capabilities (`11:88-89`); a separate flag lets resume restore
   the pre-pause rate and distinguishes "stopped clock" from a legitimately
   frozen-but-live `rate == 0` playing state. Recorded as a doc-11 delta.
3. **Advance reuses `TimeMap.evaluate`; the transport reads no wall clock.**
   The host samples its own injected clock (it already owns the sole
   wall-clock read for the frame deadline, `interactive.hpp:76-81`) and hands
   the transport the elapsed real duration as a `Time`; the transport scales
   it via `TimeMap{in=0, rate, offset=0}.evaluate(real_elapsed)`.
   *Rejected: the transport sampling `steady_clock` internally* — violates
   doc 16:54-62's no-wall-clock rule, makes playback non-deterministic under
   test, and duplicates the clock-injection seam already in the renderer.
   *Rejected: a new bespoke rate-scaling helper in `base`* — `TimeMap.evaluate`
   already realizes `round_ties_even((t − in)·rate + offset)` with the exact
   rational + single-leaf-rounding + overflow-as-value behaviour
   rational_time proved; reuse over re-derivation.
4. **Loop bounds are half-open and wrap only on advance; seek is unclamped.**
   *Rejected: clamp-and-stop at bounds* — that is "play once," the degenerate
   loop where bounds span the whole content; looping is the named feature
   (`11:88-89`).
   *Rejected: clamping seek into the loop* — doc 11's two-viewports-at-
   different-times premise (`11:88-93`) requires scrub to reach any instant,
   including outside the loop. Half-open + true-modulo matches the span
   half-open convention (`11:62`) and stays correct when one advance spans multiple
   loop lengths. Recorded as a doc-11 delta.
5. **Not concurrency-touching; no TSan obligation.**
   *Rejected: making the transport internally thread-safe / scoping TSan* —
   it is per-viewport host-owned state mutated on one thread like the camera
   (`02:120-137`); only an immutable sampled `Time` crosses threads. Adding
   synchronisation would invent a sharing model the design does not have. The
   future audio-clock master (`01:103-106`) drives the same single-owner seek
   surface and brings its own concurrency coverage in the audio milestone.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-07.

- `src/runtime/arbc/runtime/transport.hpp` — `arbc::Transport` class: playhead (`Time`), `Rational` rate, pause boolean, optional `TimeRange` loop bounds; `play`/`pause_transport`/`seek`/`set_rate`/`set_loop` controls; wall-clock-free `advance(Time real_elapsed)` returning `expected<Time,TimeError>`; pure `position()` read.
- `src/runtime/transport.cpp` — advance implementation via `TimeMap{in=0, rate, offset=0}.evaluate(real_elapsed)`, half-open true-modulo loop wrap, unclamped seek.
- `src/runtime/t/transport.t.cpp` — 4 TEST_CASEs enforcing claims: `transport-advance-scales-real-time-by-rate`, `transport-loop-wraps-half-open`, `transport-pause-and-seek-are-exact`, `transports-observe-composition-independently`.
- `src/runtime/CMakeLists.txt` — transport source/header/test wired into `arbc_runtime` component.
- `src/runtime/t/interactive.t.cpp` — integration test driving `render_frame` with a real `Transport` over a static scene, re-enforcing `interactive-still-scene-schedules-no-frame` / `static-tiles-survive-clock`.
- `tests/claims/registry.tsv` — 4 new claims registered (all `11-time-and-video#transport-*` and `transports-observe-composition-independently`).
- `docs/design/11-time-and-video.md:95-115` — transport semantics block added (playhead/rate/pause independence, wall-clock-free rational advance, half-open looping with unclamped seek).
