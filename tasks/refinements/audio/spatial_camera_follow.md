# audio.spatial_camera_follow — Live viewport camera binding for device-path spatialization

## TaskJuggler entry

Back-link: [`tasks/45-audio.tji`](../../45-audio.tji), `task spatial_camera_follow`
("Live viewport camera binding for device-path spatialization", lines 110–115).
This refinement expands that one-line WBS leaf. WBS note, verbatim:

> Bind the interactive runtime's live visual viewport camera into the
> DeviceMonitor's Spatialization listener each tick and on camera/transport
> change, so audio spatialization tracks the user's zoom live (the interactive
> 'camera is the listener' coupling); audio.spatial_policy ships only the
> static seam. Source: audio.spatial_policy scope boundary; see
> tasks/refinements/audio/spatial_policy.md. Doc 12.

## Effort estimate

`effort 1d` (`tasks/45-audio.tji:111`). Every load-bearing seam already
exists and is settled: the `Spatialization` context and its `Affine listener`
(`spatial_policy`), the `LookaheadRingConfig::spatial` static seed the device
path already reads (`spatial_policy`), the master owner-thread loop with its
flush + reprime path (`device_monitor`), the deterministic `flush_master()`
test barrier, and the block-cache spatial-context digest that re-keys a
listener change (`spatial_blockkey_disambiguation`). The work is narrow: (a) a
`camera_source` closure injected into `DeviceMonitorConfig`, sampled once per
mastering step; (b) a change-gated re-seed on the master thread that stages a
new `Spatialization` and rides the existing flush + reprime; (c) a runtime
mutator on the pump/ring to apply the staged seed at the reprime boundary; and
(d) a behavioral counter plus goldens tying the live path to the already-goldened
static Spatial mix. No new spatial math, no mix-graph change, no RT-callback change.

## Inherited dependencies

The parent `task audio` carries `depends contract.audio_facet,
timeline.transport`; `spatial_camera_follow` adds `depends !spatial_policy,
!device_monitor` (`tasks/45-audio.tji:113`).

- **`audio.spatial_policy`** — *settled* (DONE 2026-07-08,
  `tasks/refinements/audio/spatial_policy.md`). Shipped the **static seam this
  task makes live**:
  - `struct Spatialization { Affine listener; double viewport_w; double
    viewport_h; float accum_atten; float sub_audible; }`
    (`src/contract/arbc/contract/content.hpp:248-265`; `k_sub_audible_atten`,
    `spatial_edge_atten` `:274`, `spatial_pan_gains` `:294`), carried as
    `std::optional<Spatialization> spatial{}` on `AudioRequest`. The `listener`
    is an L0 `Affine` — "the audio twin of the visual `Viewport::camera`"
    (`content.hpp:249-252`).
  - `LookaheadRingConfig::spatial{}` seed (`src/audio_engine/arbc/audio_engine/lookahead.hpp:82`,
    read as immutable `d_config.spatial` in `mix_block`/`prime`,
    `src/audio_engine/lookahead.cpp:186,191,248-266,385-396`) — the device
    path's listener source, which this task rebinds at runtime.
  - The scope boundary this task closes (spatial_policy "What this task is"
    item 5): *"The **live** per-tick camera-follow binding (audio
    spatialization tracking the interactive viewport's camera in real time) is
    **deferred** to a named leaf; this task ships the static seam and its
    byte-exact export goldens."* and D5: *"Live interactive camera-follow is
    runtime interactive wiring separable into `audio.spatial_camera_follow`;
    the static seam here is fully testable via export goldens."*
  - Established that the listener composes per edge down a descent
    (`compose(listener, layer.transform)`) and the camera's uniform scale
    attenuates the root mix (`accum_atten = clamp(max_scale(camera), 0, 1)`) —
    the derivations this task feeds a *live* camera into unchanged.
- **`audio.device_monitor`** — *settled* (complete,
  `tasks/refinements/audio/device_monitor.md`). Shipped the monitor and the
  single owner (mastering) thread this task hooks:
  - `DeviceMonitor(Transport&, LookaheadPump&, DeviceSink&, DeviceMonitorConfig)`
    (`src/runtime/arbc/runtime/device_monitor.hpp:80-81`); `DeviceMonitorConfig`
    (`:60-68`) — the config struct this task extends with `camera_source`.
    The monitor holds **no** `Spatialization` today (`d_transport`, `d_pump`,
    `d_sink`, `d_config` only, `:140-143`).
  - `run_master()` / `master_step()` (`src/runtime/device_monitor.cpp:249-330`):
    the owner loop that alone mutates the `Transport`, applies pending
    seek/rate, advances from the delivered-frame delta, publishes the playhead
    (`:283`), and on a rebase re-seats the drain cursor and calls
    `d_pump.notify_transport_change()` (`:295-304`), else `d_pump.poke()`
    (`:306`). This is the exact per-tick site the camera sample and re-seed sit in.
  - `flush_master()` (`device_monitor.hpp:106`) — the wall-clock-free
    deterministic barrier the tests drive; behavioral counters
    `master_steps()` (`:113`), `drain_realigns()` (`:119`) — the pattern the
    new `listener_reseeds()` counter mirrors.
  - RT discipline: the whole callback chain is `ARBC_RT_NONBLOCKING`
    (`device_monitor.hpp:124-135`, `audio.rt_safety`), so the re-seed must live
    on the owner thread, never the RT callback.
- **`audio.spatial_blockkey_disambiguation`** — *settled* (DONE 2026-07-08,
  `tasks/refinements/audio/spatial_blockkey_disambiguation.md`). Added the
  `spatial_context_digest` (`content.hpp:323`) to the block key
  (doc 12:250-256), so a listener change produces a **distinct cache key** —
  a re-seeded camera never collides with a block warmed under the prior
  listener. This task relies on that key correctness: the flush + reprime it
  triggers drops the now-stale *ring* blocks, and the digest keeps the *cache*
  from serving a prior-camera block for the new one.
- **`contract.audio_facet`** — *settled*. Owns `AudioRequest`/`Spatialization`
  (`content.hpp`), unchanged by this task — the live camera flows into the same
  optional field, only its *source* changes from a static config seed to the
  master thread's per-tick sample.

## What this task is

Make the device monitor's Spatialization listener **follow the live interactive
viewport camera**, per doc 12's "the camera is the listener" and the **Camera
follow** paragraph this task adds there (design-doc delta,
`docs/design/12-audio.md`, after the Prefetch-and-caching paragraph). Today the
device path spatializes against a *static* seed (`LookaheadRingConfig::spatial`)
fixed at construction; this task rebinds that seed from the live camera each
mastering step so zooming the viewport moves the soundscape in real time.
Deliverables:

1. **Runtime (L5): a `camera_source` on `DeviceMonitorConfig`.** Add
   `std::function<Affine()> camera_source{}` to `DeviceMonitorConfig`
   (`device_monitor.hpp:60-68`). When set (and the ring seed is Spatial), the
   interactive host supplies a closure that reads the live `Viewport::camera`
   (`src/compositor/arbc/compositor/compositor.hpp:18`). Injecting an L0
   `Affine`-returning closure — not a `Viewport&` — mirrors the pump's existing
   injected-source idiom (`playhead_source`/`direction_source`/`tick_source`,
   `src/runtime/arbc/runtime/lookahead_pump.hpp`) and keeps `DeviceMonitor`
   free of any dependency on the compositor's `Viewport` type. Absent ⇒ the
   static seed stands (no follow), so every existing device-path test is
   byte-unchanged.
2. **Runtime (L5): change-gated per-tick sample + re-seed in `master_step`.**
   In `master_step()` (`device_monitor.cpp:249-308`), after the playhead
   publish (`:283`), if `camera_source` is set, sample `Affine cam =
   d_config.camera_source()` (on the non-RT owner thread — allocation/locking
   allowed here). Compare to the last-applied camera (`d_last_camera`); on a
   change (exact `Affine`-coefficient inequality) — or the first sample — build
   a new `Spatialization` (`listener = cam`, `accum_atten =
   clamp(max_scale(cam), 0, 1)`, `viewport_w/h` and `sub_audible` carried from
   the static seed), stage it into the pump via a new mutator (deliverable 3),
   force a **flush + reprime** (the listener is part of the block key), and
   bump a new `listener_reseeds()` behavioral counter. A camera change re-uses
   the existing `notify_transport_change()` reprime path but **does not**
   re-seat the drain cursor (`d_realign_*`) — the playhead has not moved, so
   only the ring contents re-render, not the block-index window. An unchanged
   camera does nothing extra (no re-seed, no reprime).
3. **Audio-engine (L4): a runtime listener mutator on the pump/ring.** Add
   `LookaheadPump::set_spatial(std::optional<Spatialization>)` (and the
   `LookaheadRing` mutator it delegates to) that **stages** a new
   `LookaheadRingConfig::spatial` value, applied at the next reprime boundary —
   the same boundary `notify_transport_change()` already synchronizes (flush,
   then re-warm on the pump/worker thread). The staged seed crosses the
   owner→pump thread edge under the pump's existing transport-change
   synchronization (release on the master-side store, acquire on the pump-side
   reprime read), so no worker ever reads a torn seed and no new lock is added
   to the fill loop. Sequencing on the master thread: `set_spatial(new_ctx)`
   **before** `notify_transport_change()`, so the reprime warms under the new
   listener.

**Out of scope**, each mapped or surfaced:

- **Live viewport-*extent* follow (window resize).** The camera transform is
  the only per-frame-varying quantity this task tracks; `viewport_w/h` stay
  from the static seed (pan normalization is stable under a fixed window).
  Whether v1 needs audio pan to re-normalize on window resize is a product
  judgment (rare event, windowing-dependent), surfaced to the parking lot —
  **not** a WBS leaf. It is a trivial extension of this same seam if wanted.
- **Export/offline camera-follow.** An export renders one decided camera path
  and takes its listener from the static `ExportMonitor` seed
  (`spatial_policy`); the follow is device-monitor-only (doc 12 delta). No task.
- **HRTF / real spatial rendering / non-collapsing per-leaf pan** —
  doc-deferred "monitor-implementation territory" (doc 12:162-165), **not** a
  WBS leaf.

## Why it needs to be done

`spatial_policy` shipped the Spatial *mechanism* — pan, attenuation,
sub-audible cull, the camera post-scale — but bound it to a **static** listener
seeded once at construction. Without this task, zooming the interactive
viewport does not move the soundscape: the promise "Zooming toward a nested
composition brings its soundscape forward; zooming out fades it into the
ambience" (doc 12:150-151) holds only for an offline export with a pre-decided
camera, never for the live editor. This leaf is the interactive realization of
the symmetry doc 12:20 states — "Viewport (camera + transport)" ↔ "Monitor (mix
policy + same transport)" — closing the last coupling between the visual and
audio sides of a live session. It is one of the audio-semantics leaves the M6
milestone (`tasks/99-milestones.tji:49`, `task m6_audio`) carries; `spatial_policy`'s
closer registered it and wired it into `m6_audio`'s `depends`.

## Inputs / context

**Governing design doc — doc 12 (normative, doc 16):**
- **Spatialization** (`docs/design/12-audio.md:148-154`) — "the monitor derives
  per-layer pan from composed position in the viewport and attenuation from
  composed scale — **the camera is the listener**. Zooming toward a nested
  composition brings its soundscape forward; zooming out fades it into the
  ambience." This task makes "the camera" the *live* camera for the device path.
- **The v1 Spatial model — Threading** (`:174-182`) — the listener transform is
  "camera → this composition's local frame"; the request carries listener,
  viewport extent, accumulated attenuation, sub-audible threshold. This task
  changes only the *source* of the top-level listener (static seed → live
  camera), not the model.
- **Device monitor / clock mastering** (`:213-247`) — the interactive monitor
  renders ahead into a ring; "play/seek flushes and re-primes the ring", the
  standard trade for a plugin host. A **camera** change now rides that same
  flush + reprime (the listener is part of the block key).
- **Prefetch and caching** (`:249-273`) — the block key is `(content id,
  revision, block index, rate, spatial-context digest)`; the **spatial-context
  digest** is "a 64-bit digest of the `Spatialization` under which the block is
  rendered … zero exactly when the request is Flat." A listener change ⇒ a new
  digest ⇒ correct re-key — the invariant this task's flush + reprime rests on.
- **Camera follow** (`docs/design/12-audio.md`, the paragraph this task adds
  after Prefetch-and-caching — design-doc delta) — the normative statement of
  the per-mastering-step sample, change-gated re-seed + flush/reprime,
  no-drain-re-seat, still-camera-costs-nothing, and owner-thread-only rule.
- **The symmetry** (`:19-20`) — "Viewport (camera + transport)" ↔ "Monitor (mix
  policy + same transport)": the visual and audio interactive objects share a
  transport; this task shares the camera too.

**Doc 11 (time and video):**
- The **viewport camera** is "anchor + matrix" and the **transport** its
  temporal sibling (`docs/design/11-time-and-video.md:24`, transport at
  `:88-115`); both are per-viewport host-owned value state on the interactive
  frame loop — sampled, not event-pushed (`:102-104`).

**Doc 17 levelization (CI-enforced):**
- Viewports, transports, and monitors are all `runtime` **L5**
  (`docs/design/17-internal-components.md:24,60`); `DeviceMonitor`/`DeviceSink`
  are runtime and dependency-free of any OS audio API (`:163-172`). The
  `LookaheadRing`/`LookaheadPump` are `audio-engine` **L4**; `Affine` is base
  **L0**; `Spatialization` is `contract` **L3**. Injecting a
  `std::function<Affine()>` (an L0 return) into the L5 config adds **no**
  component edge — `DeviceMonitor` never names `Viewport` (L4 compositor); the
  host closure, itself L5, closes over the live viewport. The new L4 pump
  mutator touches only types already in its closure (`Spatialization` L3,
  `Affine` L0). CI levelization stays green.

**Code seams the implementation extends:**
- `DeviceMonitorConfig` / `DeviceMonitor` — `src/runtime/arbc/runtime/device_monitor.hpp:60-68,70-143`;
  `master_step()`/`run_master()` — `src/runtime/device_monitor.cpp:249-330`
  (playhead publish `:283`, rebase/flush path `:287-304`, `poke` `:306`).
- `LookaheadRingConfig::spatial` — `src/audio_engine/arbc/audio_engine/lookahead.hpp:82`;
  its reads in `mix_block`/`prime` — `src/audio_engine/lookahead.cpp:186,191,248-266,385-396`.
- `LookaheadPump` — `src/runtime/arbc/runtime/lookahead_pump.hpp`
  (`notify_transport_change()`, the tick reprime `tick_once`; impl
  `src/runtime/lookahead_pump.cpp:174-187` calling the ring `reprime`/`prime`).
- `Spatialization` + helpers — `src/contract/arbc/contract/content.hpp:248-265,274,294,323`.
- `Viewport{width,height,camera,anchor}` — `src/compositor/arbc/compositor/compositor.hpp:15-28`
  (camera `:18`); `Affine::max_scale`/`compose`
  (`src/base/arbc/base/transform.hpp:24,34`).
- Interactive driver (the host that supplies the closure) —
  `src/runtime/arbc/runtime/interactive.hpp:68`, `render_frame` (`:97-101`); the
  existing camera read `viewport.camera.max_scale()` (`src/runtime/interactive.cpp:164`).

**Existing claims to extend, not duplicate** (`tests/claims/registry.tsv`,
`12-audio#…`): the Spatial-mix claims from `spatial_policy`
(`#spatial-attenuates-by-composed-scale`, `#spatial-pans-by-composed-position`,
`#spatial-sub-audible-cull-terminates-recursion`) already pin the *mix* under a
given listener — this task adds only the *live-binding* claim, reusing those
mixes' byte-exactness as its oracle.

## Constraints / requirements

1. **Levelization (doc 17:41-44).** `camera_source` is `std::function<Affine()>`
   on the L5 `DeviceMonitorConfig`; the pump mutator adds only L3/L0 types to L4.
   No new component edge — the CI levelization check stays green.
2. **Absent `camera_source` is byte-identical.** With no closure the static seed
   stands and every existing device-monitor / lookahead test passes unchanged;
   the added config field is defaulted and last, so aggregate inits keep compiling.
3. **A camera change invalidates exactly like a seek — and no more.** On a change
   the ring flushes + reprimes so blocks warmed under the old listener are
   discarded (they carry a different spatial-context digest, doc 12:250-256).
   The playhead is unmoved, so the drain cursor is **not** re-seated
   (`d_realign_request` stays untouched) and `drain_realigns()` does **not**
   advance — a camera reprime is distinguishable from a seek reprime.
4. **A still camera costs nothing (performance-shaped, doc 16).** Over any number
   of mastering steps with an unchanging camera, `listener_reseeds() == 0` and no
   flush/reprime is issued — proven by a behavioral counter, never wall-clock. A
   static scene issues zero re-renders.
5. **RT-safety unchanged (`audio.rt_safety`).** The camera sample, the change
   test, the `Spatialization` construction, and the `set_spatial` staging all run
   on the non-RT owner thread; the RT callback chain (`fill_rt`/`drain_block`,
   `device_monitor.hpp:128-135`) is untouched and issues no listener work. No new
   allocation, lock, or blocking call is added to any `ARBC_RT_NONBLOCKING` path.
6. **Determinism (doc 16).** Given a scripted `camera_source` sequence and the
   `flush_master()` barrier, the sequence of staged `Spatialization` values and
   the drained bytes are a pure function of inputs — no wall-clock read, no
   nondeterministic ordering. The device drain under a live-bound camera `C` is
   **byte-identical** to a static-seed run with `LookaheadRingConfig::spatial.listener
   = C`, so this task inherits `spatial_policy`'s Spatial-mix goldens as its oracle.
7. **Owner→pump hand-off is race-free (concurrency-touching).** The staged seed
   crosses the master→pump/worker edge under the pump's existing transport-change
   synchronization; TSan proves no data race on `LookaheadRingConfig::spatial`
   under concurrent camera changes and ring fills (Constraint tested below).
8. **Diff coverage ≥ 90%** on changed lines (doc 16 CI gate); tests ship in this task.
9. **Design-doc delta (this task).** doc 12 gains the **Camera follow** paragraph
   (the live camera → listener binding, cadence, and threading the doc left
   unstated); it rides in the closer's commit (doc 16 same-commit rule). No
   doc 00 bullet: "spatialization as monitor policy (flat by default)" is already
   recorded at doc 00:126, and the concrete live-binding cadence is task-level
   detail the doc 12 delta captures.

## Acceptance criteria

- **Claims-register growth** (`tests/claims/registry.tsv`, `12-audio#…`), enforced
  by an `// enforces: 12-audio#<slug>`-tagged test:
  1. `12-audio#camera-follow-tracks-viewport-live` — "In Spatial mode the
     interactive device monitor re-seeds the mix listener from the live viewport
     camera on the mastering step following a camera change, flushing and
     repriming the lookahead ring; the drain output thereafter is byte-identical
     to a static-seed run under the new camera." Enforced by a **byte-exact
     golden** (below).
  2. `12-audio#camera-follow-still-camera-costs-nothing` — "An unchanging camera
     issues no listener re-seed and no reprime across any number of mastering
     steps." Enforced by the behavioral-counter test (below).
- **Byte-exact goldens** (deterministic, `std::memcmp`, no tolerances), in a
  device-monitor test (`src/runtime/t/device_monitor.t.cpp`):
  - Drive the monitor with a `camera_source` returning a fixed camera `C`
    (distinct from identity), step past the initial reprime with `flush_master()`,
    and assert the drained bytes equal a companion monitor seeded *statically*
    with `LookaheadRingConfig::spatial.listener = C` — byte-identical (the live
    binding routes to the same Spatial mix `spatial_policy` already goldened).
  - Drive a **camera change** mid-run: camera `C0` for the first N steps, then
    `C1`; assert the post-change drained bytes equal a static-`C1` run and differ
    from the `C0` bytes — the re-seed took effect and the stale `C0` ring was
    dropped.
- **Behavioral-counter assertions** (never wall-clock, doc 16), via a new
  `DeviceMonitor::listener_reseeds()` counter (mirroring `master_steps()`/
  `drain_realigns()`, `device_monitor.hpp:113-121`):
  - *Still camera*: a constant `camera_source` over M `flush_master()` steps ⇒
    `listener_reseeds() == 1` (the initial seed) and `drain_realigns()` unchanged
    from the no-camera baseline after step 1 — i.e. no per-tick reprime for an
    unmoving camera.
  - *Change*: a `camera_source` scripted to change once ⇒ `listener_reseeds()`
    increments by exactly 1 at that step, and the reprime is a camera reprime
    (`drain_realigns()` does **not** advance, distinguishing it from a seek).
  - *No `camera_source`*: `listener_reseeds() == 0` for the run's lifetime.
- **Concurrency / TSan + RT-safety** (concurrency-touching: device monitor +
  audio engine, doc 16): a stress test cycles `camera_source` through many
  distinct cameras while the pump fills and the drain runs, under **TSan**,
  asserting no data race on `LookaheadRingConfig::spatial`; and under
  **RealtimeSanitizer** (`audio.rt_safety`) the RT callback chain issues no
  listener work (no new violation). Uses `flush_master()` as the barrier, no
  wall-clock.
- **No new conformance family.** Camera-follow is monitor/engine wiring, not a
  content kind or operator; it adds no `arbc-testing` family. Its tests drive
  `org.arbc.tone`/`org.arbc.nested` through the live-bound Spatial mix.
- **Deferred follow-ups.** None registered as WBS leaves — this task is
  self-contained. The one judgment item (whether v1 needs live viewport-*extent*
  / resize follow) is surfaced to `tasks/parking-lot.md`, not the WBS (a product
  decision, not agent-implementable-in-isolation work).
- **WBS gate.** After the closer adds `complete 100` and the `Refinement:`
  back-link to `tasks/45-audio.tji`, `tj3 project.tjp 2>&1 | grep -iE
  "error|warning"` is silent.

## Decisions

- **D1 — Sample the camera on the master (owner) thread via an injected
  `camera_source` closure; do not push from the visual frame loop or hold a
  `Viewport&`.** *Authority:* doc 12 device-monitor/clock-mastering
  (`:213-247`) — the owner thread alone mutates the transport and triggers
  reprime; doc 17:24,60 (viewport/monitor both L5); the pump's existing
  injected-source idiom. *Rationale:* the master thread already owns *when* the
  ring flushes and reprimes; sampling the camera and deciding to re-seed there
  keeps all audio-timing decisions on that single owner thread, so the re-seed
  can share the transport-change reprime path with no second thread racing it.
  A `std::function<Affine()>` returns an L0 value, so `DeviceMonitor` never
  depends on the L4 compositor `Viewport` — exactly how the pump takes
  `playhead_source`/`tick_source` rather than a `Transport&`. *Rejected:* (a) a
  push API where the interactive driver calls `monitor.set_camera(...)` each
  visual frame — puts audio-reprime timing on the visual thread and races the
  master thread's reprime; (b) `DeviceMonitor` holding a `Viewport&` — a
  cross-thread mutable-shared-object read and an L5→L4 dependency on a type the
  injected-closure form avoids; (c) sampling on the RT callback — an
  `audio.rt_safety` violation (allocation/branch on the RT thread).
- **D2 — Only the `listener` (and its derived `accum_atten`) follow live;
  `viewport_w/h`, `sub_audible`, and `MixPolicy` stay from the static seed.**
  *Authority:* doc 12:174-177 (the listener transform is the composed camera;
  viewport extent is pan-normalization state). *Rationale:* the camera
  transform is the sole per-frame-varying quantity of a live zoom/pan; window
  extent changes only on resize (rare) and the threshold/policy are run
  constants. Following just the `Affine` keeps the injected closure minimal
  (`Affine()`), the change test a cheap coefficient compare, and the re-seed a
  single struct rebuild. *Rejected:* injecting a full `std::function<Spatialization()>`
  — pushes the listener/attenuation math into the host, duplicating what the
  monitor already computes from the seed; injecting `std::function<Viewport()>`
  — re-introduces the L4 `Viewport` coupling D1 avoids, for a `w/h` that rarely
  changes.
- **D3 — A camera change rides the existing flush + reprime path but does *not*
  re-seat the drain cursor.** *Authority:* doc 12:249-273 (the listener is part
  of the block key via the spatial-context digest, so a listener change
  invalidates warmed blocks exactly as a seek does);
  `device_monitor.cpp:287-304` (the rebase path). *Rationale:* blocks warmed
  under the old camera carry a stale digest and must be dropped and re-warmed —
  identical invalidation to a seek, so it reuses `notify_transport_change()`.
  But the playhead does not move on a pure camera change, so the block-index
  window is unchanged and the drain cursor stays valid — re-seating it
  (`d_realign_*`) would be wrong and would spuriously bump `drain_realigns()`.
  Keeping the two distinct makes a camera reprime observably different from a
  seek reprime and lets the still-camera counter test assert "no realign."
  *Rejected:* (a) a partial re-warm that keeps the currently-draining block
  under the old listener — a visible spatialization discontinuity across the
  block boundary and a byte-exactness hazard; (b) re-seating the drain cursor
  too (as a seek does) — corrupts the drain window for an unmoved playhead.
- **D4 — Gate the re-seed on an actual camera change; a still camera issues
  nothing.** *Authority:* doc 16 (performance promises are behavioral-counter
  assertions — "a still scene issues zero renders"); the "still camera costs
  nothing" clause of the doc 12 delta. *Rationale:* the master step runs every
  ~2 ms (`device_monitor.cpp:314`); re-seeding + reprime unconditionally would
  flush and re-render a static scene continuously, defeating the lookahead. An
  exact `Affine`-coefficient compare against `d_last_camera` makes an unmoving
  camera a no-op, so audio tracks a *live* zoom without ever re-rendering an
  *unmoving* one — and the zero-reseed count is directly assertable. *Rejected:*
  re-seeding every tick (needless flush/re-render of a static scene, a doc-16
  performance-promise violation); an epsilon/tolerance compare (a listener
  change is either bit-different or not — byte-exact goldens demand an exact
  compare, and the digest already keys on exact `Affine` bits).
- **D5 — Stage the new seed through a pump `set_spatial` mutator applied at the
  reprime boundary; reuse the transport-change synchronization, add no new
  lock.** *Authority:* `device_monitor.cpp:252-256,304` (seek/rate are staged
  under `d_master_mutex` and applied at the reprime the pump already
  synchronizes); doc 12 delta ("re-seeded entirely on the non-RT owner
  thread"). *Rationale:* the ring seed is read on the pump/worker threads during
  prime; a live write from the master thread needs a hand-off, and the
  transport-change reprime is already exactly that synchronization point — the
  master stages `set_spatial` (release) then `notify_transport_change`, and the
  pump reads the staged seed (acquire) as it reprimes. This adds no lock to the
  fill loop and no cross-thread surface beyond the one the reprime already
  crosses. *Rejected:* mutating `LookaheadRingConfig::spatial` in place from the
  master thread (a data race with in-flight fills, TSan-red); a dedicated
  spatial-only lock on the ring (redundant with the reprime barrier, and
  reachable from the fill loop — an RT-safety hazard if the fill ever contends it).

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-08.

- `src/audio_engine/arbc/audio_engine/lookahead.hpp`, `src/audio_engine/lookahead.cpp` — `LookaheadRing::spatial()` getter + `set_spatial()` added; re-seeds staged config, retires the entire prepared ring so the new listener is active from the next reprime.
- `src/runtime/arbc/runtime/lookahead_pump.hpp`, `src/runtime/lookahead_pump.cpp` — `LookaheadPump::set_spatial()`/`spatial()` added; staged seed applied under `d_mutex` before each tick's reprime, crossing the owner→pump thread edge under the existing transport-change synchronization.
- `src/runtime/arbc/runtime/device_monitor.hpp`, `src/runtime/device_monitor.cpp` — `DeviceMonitorConfig::camera_source` closure field added; `DeviceMonitor::d_spatial_base` captured at ctor; `listener_reseeds()` behavioral counter added; per-step change-gated camera sample + `set_spatial` + flush/reprime in `master_step` (no drain-cursor re-seat — playhead is unmoved on a camera change).
- `src/runtime/t/device_monitor.t.cpp` — `drive_spatial` harness + 4 tests: golden (live==static-seed), golden (mid-run camera change), behavioral-counter (still/change/no-source), concurrency stress (TSan/RT under concurrent camera cycling and ring fills).
- `tests/claims/registry.tsv` — 2 claims: `12-audio#camera-follow-tracks-viewport-live`, `12-audio#camera-follow-still-camera-costs-nothing`.
- `docs/design/12-audio.md` — Camera follow paragraph added (per-mastering-step sample, change-gated re-seed + flush/reprime, no-drain-re-seat, still-camera-costs-nothing, owner-thread-only rule).
- No interactive host construction site yet (the host supplying the `Viewport`-reading closure isn't assembled in-tree); the seam, logic, and tests are the complete deliverable.
