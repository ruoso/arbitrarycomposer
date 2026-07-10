# runtime.host_objects — Viewport/transport/monitor API

## TaskJuggler entry

`task host_objects` in [`tasks/65-runtime.tji`](../../65-runtime.tji) (lines
28–33), under `task runtime`. Back-link to add on completion:
`Refinement: tasks/refinements/runtime/host_objects.md` appended to the
task's `note`.

## Effort estimate

**2d** (from the `.tji`). This is a *binding/composition* task, not a
green-field engine: the render driver, the anchored-camera math, the
transport, and the audio-clock master already exist as lower seams (see
Inputs). The 2d covers the persistent L5 host object that owns the
cross-frame camera + anchor path, applies the pure `rebase()` result,
surfaces the re-anchor event, wires the model damage subscription into the
live loop, and integrates the transport/monitor clock — plus its tests and
claim rows.

## Inherited dependencies

- **`!interactive` (`runtime.interactive`)** — *settled* (`complete 100`).
  Ships `arbc::InteractiveRenderer` and its per-frame entry point
  `render_frame(...) -> FrameOutcome{schedule_follow_up}`
  (`src/runtime/arbc/runtime/interactive.hpp:68,72-74,97-101`). The renderer
  deliberately owns **no** event loop, transport, camera model, or damage
  subscription and explicitly hands those to this task
  (`interactive.hpp:70,89-91`; refinement `interactive.md:239-244`).
- **`model.content_binding`** — *settled* (`complete 100`). Content is a real
  versioned record referenced by `ObjectId`; the id→`Content*` binding lives
  in the runtime `Document` side-map, resolved via
  `Document::resolve`/`ContentResolver` — the same resolver `render_frame`
  consumes (`src/runtime/arbc/runtime/document.hpp:70-73`). `content_binding`
  names `runtime.host_objects` as one of its downstream consumers.

Both are settled; nothing pending blocks this task.

## What this task is

Build the host-facing object model that turns the interactive render engine
into something a host application drives: a **persistent per-viewport object**
(`HostViewport`, L5) that owns

1. the **anchored camera** as the `(anchor node, matrix)` pair doc 04
   mandates, carried across frames together with an **anchor-path stack** so
   the viewport can re-anchor both inward (zoom in) and *outward* (zoom out);
2. a **`Transport`** (the per-viewport playback clock) and the policy that
   decides whether it free-runs on the injected real clock or *chases* an
   audio master;
3. a **damage subscription** into the model that accumulates model damage
   between frames and drains it into `render_frame`;

and drives `InteractiveRenderer::render_frame` in a step loop, surfacing the
**re-anchor event** and the **follow-up-owed** decision to the host, and
exposing the seams a **monitor** attaches to (`transport()` and a live
`camera_source`).

The task creates the L5 object that *composes* existing pieces; it does not
re-implement the render loop, the rebase math, the transport, or audio-clock
mastering.

## Why it needs to be done

`runtime.interactive` produces a renderer that renders exactly one frame from
an already-formed `Viewport`, an already-sampled `composition_time`, and an
already-drained damage span, then reports whether another frame is owed. It
owns none of the state that makes those inputs exist across frames. Doc 04's
anchored-camera precision strategy "leaks into the public API" precisely here:
"'Camera position' is `(anchor node, matrix)`, not a matrix alone … and
re-anchoring must be a host-visible event" (doc 04:81-84). The pure compositor
`rebase()` step is explicit that the persistent camera and the zoom-out
anchor path are the runtime caller's job and defers them to
`runtime.host_objects` by name
(`src/compositor/arbc/compositor/anchored_viewports.hpp:28-30,101-107`).

Downstream, `packaging.release_01` (M9) depends on `runtime.host_objects`
([`tasks/75-packaging.tji:4`](../../75-packaging.tji)): the v0.1 release needs
a real host-driven viewport. The audio milestone already built its half of the
seam — the device monitor masters the transport and expects a live viewport
camera to follow (claims `12-audio#device-clock-masters-transport`,
`12-audio#camera-follow-tracks-viewport-live`); this task supplies the video
viewport that samples the mastered playhead and feeds the camera.

## Inputs / context

Design docs (normative — doc 16):

- **Doc 01 `docs/design/01-core-concepts.md`, `## Viewport` (lines 92-113).**
  The viewport is "the consumer side: a device-pixel rectangle … plus a
  mapping into a composition," defined by **anchor** (96-97), **camera
  transform** (98), and **transport** (99-101: "play/pause/seek/rate … Per-viewport … two views may observe the same composition at different times").
  The **monitor** attaches to a transport (103-106: "a device monitor's
  hardware clock masters the transport, and video chases it"). The offline
  renderer is "just a viewport with no deadline" (112-113). `## Invalidation`
  (134-141): "Damage propagates up … to every viewport observing an affected
  composition; each viewport maps it to a dirty device region and schedules
  re-rendering. This is the only path by which anything re-renders in
  interactive mode." `## Identity and versioning` (143-153): consistency via
  immutable document versions published transactionally.
- **Doc 04 `docs/design/04-transforms-and-infinite-zoom.md`, `## The solution`
  (lines 49-86).** Anchor-to-node not root (54-61); **rebasing** in/out
  (62-69: "Zooming out re-anchors upward"); the bolded API consequence
  (81-86): camera position is `(anchor node, matrix)`, host-visible camera
  APIs "must speak that pair, and re-anchoring must be a host-visible event."
  `## Numeric conventions` (116-117): a degenerate composed transform culls
  the layer rather than propagating NaNs.

Existing seams this task composes (real paths):

- **Render driver** — `class InteractiveRenderer`
  (`src/runtime/arbc/runtime/interactive.hpp:68`), ctor `(WorkerPoolConfig,
  Clock)` (`:81`), `Clock = std::function<steady_clock::time_point()>`
  (`:79`), `render_frame(state, resolve, viewport, cache, backend, pool,
  target, model_damage, composition_time, budget) -> FrameOutcome` (`:97-101`),
  `FrameOutcome{bool schedule_follow_up}` (`:72-74`). Interactive does **not**
  call `rebase` (verified) — the persistent camera step is wholly this task's.
- **Anchored camera (pure, L4)** —
  `src/compositor/arbc/compositor/anchored_viewports.hpp`: `k_root_anchor`
  (`:37`), `k_reanchor_scale_threshold` (`:47`), `RebaseNeed{none,zoom_in,zoom_out}`
  (`:59`), `struct Reanchor{bool occurred; ObjectId from,to;}` (`:69-73`),
  `struct RebaseResult{Viewport viewport; Reanchor event; RebaseNeed need;}`
  (`:79-83`), `Affine reanchor_camera(const Affine& camera, const Affine& edge)`
  (`:93`), `RebaseResult rebase(const DocRoot& state, const Viewport& viewport)`
  (`:110`). Zoom-out ancestor selection is explicitly deferred here to this
  task (`:101-107`): the pure step reports `need == zoom_out` and "leaves the
  viewport for the caller to re-anchor upward via `reanchor_camera(camera,
  ancestor_edge.inverse())`."
- **Viewport value** — `struct Viewport{Rect device; Affine camera; ObjectId anchor;}`
  (`src/compositor/arbc/compositor/compositor.hpp:15,18,27`).
- **Transport** — `class Transport`
  (`src/runtime/arbc/runtime/transport.hpp:34`): `position()` (`:42`, pure
  read, reads no clock), `rate()/is_paused()/loop()`, `play/pause/set_rate/seek/set_loop`,
  and `expected<Time,TimeError> advance(Time real_elapsed)` (`:86`, scales the
  host-sampled real elapsed by rate, wraps to loop, all-or-nothing). Header
  frames it as "the temporal sibling of the viewport camera … per-viewport
  host-owned value state … reads NO wall clock … carries no TSan/stress
  obligation by design" (`:9-30`).
- **Monitor / audio-clock master (already landed, M6)** —
  `src/runtime/arbc/runtime/device_monitor.hpp`: ctor
  `DeviceMonitor(Transport& transport, LookaheadPump&, DeviceSink&, …)`
  (`:92`) binds the viewport's transport; owner thread advances the transport
  from delivered frames and **publishes a lock-free mastered-playhead
  snapshot** `Time playhead_snapshot() const noexcept` (acquire load, `:102`)
  that "the pump's `playhead_source` and **video viewports** … read" (`:46-50`);
  `seek(Time)`/`set_rate(Rational)` on the monitor rebase the master when it is
  mastering (`:109-112`); a **live viewport-camera source**
  `std::function<Affine()> camera_source` (`:79`) is sampled each mastering
  step for Spatial camera-follow. `ExportMonitor`
  (`src/runtime/arbc/runtime/export_monitor.hpp:71`) is the offline audio twin
  (driven by the offline frame loop, no device clock).
- **Model damage seam** — `class DamageSink{virtual void flush(const
  std::vector<Damage>&)=0;}` (`src/model/arbc/model/damage.hpp:79`), installed
  via `DocState::set_damage_sink(DamageSink*)` (`src/model/arbc/model/model.hpp:199`,
  single-slot field `:441`). A commit flushes the union of its per-mutation
  damage to the sink exactly once (`src/model/model.cpp:569-570`).

Predecessor refinements (style/decision continuity):
`tasks/refinements/runtime/interactive.md` (the stateful-L5-driver /
one-clock-instant / returns-decision-does-not-own-the-loop pattern),
`tasks/refinements/runtime/offline_sequences.md` (the monitor-as-attachment
concept and pin-one-revision discipline),
`tasks/refinements/runtime/threading.md` (the inject-downward levelization
seam), `tasks/refinements/model/content_binding.md` (content referenced by
`ObjectId`, resolved in runtime).

Levelization (doc 17): `arbc::runtime` is L5 and "may depend on everything
below" (`docs/design/17-internal-components.md:60`). Every seam above is in
`runtime`'s already-allowed set — `src/runtime/CMakeLists.txt:21` already
lists `compositor`, `model`, `contract`; `Affine` is `base`. **No new
levelization edge.** The one compositor-side change (Decision 4) is
within-component and uses only `base` types.

## Constraints / requirements

1. **Camera is `(anchor node, matrix)`, carried across frames.** `HostViewport`
   owns the persistent `compositor::Viewport` value and the anchor path; every
   host-visible camera read/write speaks the pair (doc 04:81-84). Pan/zoom/rotate
   mutate the camera matrix; the anchor changes only via re-anchoring.
2. **Re-anchoring is a host-visible event.** Each step surfaces the
   `Reanchor{occurred,from,to}` value the pure `rebase()` produced (or the
   zoom-out event this task synthesizes); a frame with no re-anchor surfaces
   `occurred == false` (doc 04:83-84).
3. **Zoom-out walks the runtime-held anchor path.** On `need == zoom_out`,
   pop the anchor-path top and rebuild the camera via
   `reanchor_camera(camera, edge.inverse())` using the descent edge captured on
   the corresponding zoom-in; the round-trip zoom-in→zoom-out restores the
   original `(anchor, matrix)` to within one double rounding
   (`anchored_viewports.hpp:101-107`, doc 04:62-69).
4. **The transport produces `composition_time`; who advances it is policy.**
   When the viewport is free-running (no device monitor mastering its
   transport), `HostViewport` advances the owned `Transport` by the
   host-sampled real elapsed (from the same injected `Clock` the renderer
   uses) and samples `position()`. When a `DeviceMonitor` masters the
   transport, `HostViewport` **does not advance** — it derives
   `composition_time` from the monitor's `playhead_snapshot()` (video chases
   audio, doc 01:103-106; the monitor is the sole transport mutator,
   `device_monitor.hpp:43,102`).
5. **Damage subscription lifecycle is owned here.** `HostViewport` owns a
   `DamageSink` accumulator, installs it on the `DocState` for its lifetime
   (RAII), accumulates flushed model damage between frames, and drains it into
   `render_frame`'s `model_damage` span each step (doc 01:134-141;
   `interactive.hpp:91`). *Single-viewport for this task* — multi-viewport
   fan-out is deferred (Decision 6).
6. **Monitor attachment is a seam, not a new type.** `HostViewport` exposes
   `Transport& transport()` (a `DeviceMonitor`/`ExportMonitor` binds to it) and
   a live `camera_source` (`std::function<Affine()>` returning the current
   camera) to wire into `DeviceMonitorConfig::camera_source`. No new monitor
   class — the audio monitors already exist and already master/follow.
7. **The renderer owns the loop decision, not the loop.** `HostViewport::step`
   returns the `schedule_follow_up` decision; the host owns re-invocation
   (matching `interactive.md:568-577`). A step with no pending damage and no
   follow-up owed issues **zero** `render_frame` invocations (doc 01:140).
8. **No wall-clock reads outside the injected `Clock`.** The transport reads no
   clock (`transport.hpp:28-30`); `HostViewport`'s only clock read is the
   injected `Clock` (doc 16:54-62). Tests assert behavior via counters, never
   wall time.
9. **Degenerate transforms cull, never NaN** (doc 04:116-117) — inherited from
   the pure `rebase`/`cull_walk`; `HostViewport` must not reintroduce a NaN
   path when composing the anchor-path edges.

## Acceptance criteria

Testable checks that pin the behavior. New claim rows land in
`tests/claims/registry.tsv`, each referenced by an `// enforces: <claim-id>`
test comment (doc 16).

- **Claim `04-transforms-and-infinite-zoom#zoom-out-reanchors-along-anchor-path`**
  (new) — a scripted zoom-in past the threshold followed by a zoom-out
  re-anchors outward along the anchor path: the camera is rebuilt by inverting
  the stored descent edge, the composed device-space image of a probe point is
  preserved to within one double rounding, and the round-trip restores the
  original `(anchor, matrix)`. Enforced by a unit test in
  `src/runtime/t/host_viewport.t.cpp` plus a byte-/ULP-exact probe golden in
  `tests/host_viewport_reanchor_golden.t.cpp` (mirrors the existing
  `tests/anchored_viewports_golden.t.cpp` style and the tolerance of the
  landed `#rebase-preserves-composed-appearance` claim, `registry.tsv:115`).
- **Claim `04-transforms-and-infinite-zoom#reanchor-surfaced-as-host-event`**
  (new) — every re-anchor (in or out) is surfaced from `step` as
  `Reanchor{occurred=true, from, to}` with the correct old/new anchor ids; a
  frame with no re-anchor surfaces `occurred == false`. Enforced by a
  behavioral test asserting a `reanchor_events()` counter equals the scripted
  count over a zoom sequence.
- **Claim `01-core-concepts#viewport-step-drives-transport-damage-frame`**
  (new) — a `HostViewport::step` samples `composition_time` from its playhead
  policy, drains its accumulated model damage into `render_frame`, and honors
  `FrameOutcome::schedule_follow_up`; a free-running viewport advances its
  transport by exactly `round_ties_even(real_elapsed*rate)` (delegating to
  `Transport::advance`), while an audio-mastered viewport advances the
  transport **zero** times and derives `composition_time` from
  `playhead_snapshot()`. Enforced by two behavioral tests (free-run vs.
  audio-master) asserting a `transport_advances()` counter (== N free-run, ==
  0 audio-master) and that `composition_time` tracks the snapshot; the
  audio-master test drives a `DeviceMonitor` bound to `viewport.transport()`
  with `camera_source` wired to `viewport.camera_source()`, exercising the
  landed `12-audio#device-clock-masters-transport` and
  `12-audio#camera-follow-tracks-viewport-live` seams from the video side (no
  new audio claim, no new TSan obligation — see Decision 5).
- **Claim `02-architecture#idle-viewport-issues-no-frames`** (new,
  performance-shaped → behavioral counter, doc 16) — a `HostViewport` with no
  pending damage and no follow-up owed issues zero `render_frame` invocations
  across repeated idle steps (a still scene costs nothing, doc 01:140).
  Enforced by a `frames_issued()` counter assertion, never wall time.
- **Levelization** — `scripts/check_levels.py` stays green (no new edge; the
  compositor `RebaseResult` field is `base`-only).
- **Diff coverage** — ≥90% on changed lines (CI gate); the tests above are
  part of this task, not a follow-up.
- **WBS gate** — after adding `complete 100` and the `note` back-link,
  `tj3 project.tjp 2>&1 | grep -iE "error|warning"` is silent.

Deferred follow-up (closer registers as a real WBS leaf, wired into **M9**
`m9_release`, alongside the existing `runtime.host_objects`→
`packaging.release_01` chain):

- **`runtime.damage_router`** — *~1d.* A fan-out `DamageSink` installed once on
  the `DocState` that dispatches each flushed model-damage batch to N
  registered per-viewport `HostViewport` sinks (RAII register/unregister),
  enabling **multiple viewports observing one composition** (doc 01:108-110)
  which the model's single `set_damage_sink` slot cannot express directly.
  Lands the end-to-end two-`HostViewport` version of
  `11-time-and-video#transports-observe-composition-independently`
  (`registry.tsv:163`) and a `01-core-concepts#multiple-viewports-observe-one-composition`
  claim. `depends runtime.host_objects`; `note` cites this refinement.
  (Deferred to `runtime.damage_router` — closer registers in WBS.)

## Decisions

1. **A single persistent `HostViewport` class composes the existing pieces,
   rather than three separate host objects.** The `.tji` note lists "anchored
   cameras, transports, monitor attachment," but `Transport` and the audio
   monitors already exist as classes; the missing thing is the L5 object that
   *binds* a persistent camera + anchor path + owned transport + damage
   subscription and drives the renderer. One class with clear accessors
   (`camera()`, `transport()`, `camera_source()`, `step()`) is the smaller
   abstraction with one or two call sites today (the release host, the tests).
   *Alternative rejected:* free functions + a bag of caller-held state — it
   would re-scatter the cross-frame state (anchor path, damage accumulator,
   previous clock instant) the interactive refinement deliberately located in a
   stateful L5 object (`interactive.md:523-536`).
   *Naming:* `HostViewport` (not `Viewport`) to avoid collision with the
   `compositor::Viewport` per-frame value struct it owns.

2. **The anchor path is a stack of `(ObjectId anchor, Affine descent_edge)`;
   zoom-out pops and inverts the stored edge.** Zoom-in re-anchors to a
   descendant (pure `rebase` returns it); this task pushes the new anchor and
   the descent edge. Zoom-out (`need == zoom_out`) pops and rebuilds via
   `reanchor_camera(camera, edge.inverse())` — the exact inverse of the descent,
   which is what makes the round trip appearance-preserving and restores the
   original camera. *Alternative rejected:* reconstruct the ancestor edge from
   the model on each zoom-out (find the anchor's transform within its parent
   composition) — it risks choosing a *different* edge than the one applied on
   descent (breaking the exact-inverse guarantee) and needs a model parent
   walk the stored stack makes unnecessary. `anchored_viewports.hpp:101-107`
   explicitly assigns the "runtime-held anchor path" to this task.

3. **`step` returns a poll-style result value (`schedule_follow_up`,
   `Reanchor`, `RebaseNeed`); no host callback registration.** Matches the
   `FrameOutcome` return-a-decision idiom (`interactive.hpp:72-74`) and the
   errors-/events-as-values grain of the codebase; keeps the re-anchor event
   trivially testable. *Alternative rejected:* a `std::function` re-anchor
   callback — an extra lifecycle to manage for no gain over polling the
   returned value each step.

4. **Surface the applied descent edge from the pure `rebase` step (extend
   `RebaseResult` with an `Affine edge`).** So the anchor path stores the exact
   matrix `rebase` used, and zoom-out inverts *that* matrix. This is a trivial
   additive field on a compositor return struct using only `base` types (no
   new edge, no behavioral change — it implements doc 04:62-69's stated "stored
   per-edge matrix" mechanism and the `ancestor_edge.inverse()` the header's
   own comment names, `:106`). *Alternative rejected:* recover the edge in
   `HostViewport` as `compose(old_camera.inverse(), new_camera)` — within the
   stated one-double-rounding tolerance but less exact (two composes + an
   inverse) and less self-documenting than storing what `rebase` actually
   applied. **No design-doc delta:** surfacing the edge realizes doc 04's
   mechanism; it does not change designed behavior.

5. **Transport advance is policy-gated by "am I the clock master," and the
   only cross-thread channel is the existing lock-free
   `playhead_snapshot()`.** Free-run: advance the owned transport from the
   injected real elapsed. Audio-mastered: sample `playhead_snapshot()` (acquire
   load) and never advance — the device monitor is the sole transport mutator
   (`device_monitor.hpp:43,57`). This reuses the master/chase seam the M6 audio
   work already built and TSan-covered (`tests/device_monitor_concurrency.t.cpp`,
   claim `12-audio#device-clock-masters-transport`). `HostViewport` adds **no
   new shared mutable state** — its transport and camera are single-owner value
   state (`transport.hpp:25-30`), and its only cross-thread read is the
   existing acquire load. **No new TSan/stress obligation** (doc 16
   concurrency policy: the obligation is discharged where the shared state
   lives, which is the device monitor, not here).

6. **Multi-viewport damage fan-out is deferred to `runtime.damage_router`, and
   this task lands single-viewport.** The model exposes one
   `set_damage_sink` slot, whose own comment says the concrete consumer "is
   wired from above at L3/L5" (`damage.hpp:74-78`) — a fan-out router is the
   intended occupant. Landing the full router + N-viewport dispatch + its
   independence claim inside this 2d task would overrun it, and the router is a
   clean additive layer (it forwards to each viewport's existing sink), not a
   rewrite. `HostViewport`'s damage sink is therefore designed to sit behind a
   future router unchanged. *Alternative rejected:* build the router now — it
   pushes past the 2d estimate for capability (simultaneous viewports) the v0.1
   release does not require on the critical path.

## Open questions

(none — all decided.)

## Status

**Done** — 2026-07-10.

- `src/runtime/arbc/runtime/host_viewport.hpp`, `src/runtime/host_viewport.cpp` — `HostViewport` L5 class: persistent anchored camera + anchor-path stack, owned `Transport`, RAII `DamageSink` accumulator, free-run/audio-master playhead policy; drives `InteractiveRenderer::render_frame`; surfaces re-anchor event + follow-up decision as a poll-style `StepOutcome`.
- `src/runtime/t/host_viewport.t.cpp`, `tests/host_viewport_reanchor_golden.t.cpp` — 5 unit/behavioral cases (zoom-out anchor-path round-trip, reanchor-as-host-event, free-run step, audio-mastered chase with real `DeviceMonitor`, idle-no-frames) + 1 ULP-probe golden; 86 + 8 assertions all green.
- `src/compositor/arbc/compositor/anchored_viewports.hpp`, `src/compositor/anchored_viewports.cpp` — Decision 4: additive `Affine edge` field on `RebaseResult` (base-only types, no new levelization edge); set to the descent edge on zoom-in.
- `src/runtime/CMakeLists.txt`, `tests/CMakeLists.txt` — registered `host_viewport` compilation unit and both new test targets.
- `tests/claims/registry.tsv` — 4 new claim rows: `04…#zoom-out-reanchors-along-anchor-path`, `04…#reanchor-surfaced-as-host-event`, `01-core-concepts#viewport-step-drives-transport-damage-frame`, `02-architecture#idle-viewport-issues-no-frames`.
- Tech-debt follow-up `runtime.damage_router` registered in WBS (~1d, depends `runtime.host_objects`, wired into `m9_release`): fan-out `DamageSink` for multi-viewport dispatch.
