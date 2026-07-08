# audio.device_monitor — Device monitor + clock mastering

## TaskJuggler entry

Back-link: [`tasks/45-audio.tji`](../../45-audio.tji), `task device_monitor`
(lines 33–38). WBS note, verbatim:

> Device sink interface with a reference implementation as a separate plugin
> artifact (device APIs stay out of libarbc per the doc 17 codec-line policy);
> the device clock masters the transport; video chases audio. Doc 12.

## Effort estimate

`effort 3d` (`tasks/45-audio.tji:34`). The heavy machinery already exists and
is TSan-clean: the L4 ring, the L5 `LookaheadPump` with its injected
`playhead_source`/`direction_source`/`tick_source`, the `AudioWorkerPool`, and
the pure-consume `drain` all shipped with `audio.lookahead` and its
successors. This task is the **thin runtime adapter** the predecessor
explicitly left it (`lookahead.md:132`, "a thin adapter (device I/O +
clock)"): a `DeviceSink` interface, a `DeviceMonitor` that (a) turns a device
callback into `pump.drain` consumption, (b) masters the transport from
delivered samples, and (c) publishes the mastered playhead so video chases
audio — plus a separate out-of-lib reference backend artifact and the **first
`Transport` concurrency coverage** (the transport deferred it to here,
`transport.md:338–339`). The three days go to the new cross-thread
mastered-clock surface (lock-free publish + TSan), the plugin artifact, and
the codec-line design-doc delta — not to new engine machinery.

## Inherited dependencies

The parent `task audio` carries `depends contract.audio_facet,
timeline.transport`; `device_monitor` adds `depends !lookahead`, and inherits
the two lookahead successors transitively through the shared code.

- **`audio.lookahead`** — *settled* (DONE 2026-07-08,
  [`tasks/refinements/audio/lookahead.md`](lookahead.md)). Landed the L4
  `LookaheadRing`, the L5 `AudioWorkerPool`, and the L5 `LookaheadPump`, and
  **hands this task exactly its seam**: the pump "reads the transport, it does
  not master it (device-clock mastering is `audio.device_monitor`)"
  (`src/runtime/arbc/runtime/lookahead_pump.hpp:30–32`); its injected
  `playhead_source`/`direction_source`/`tick_source`
  (`lookahead_pump.hpp:57–61`) are the master seam; its `drain`
  (`lookahead_pump.hpp:76`) is the RT-callback consumption point; and a code
  comment reserves "a lock-free double-buffer for a true RT device thread" for
  this task (`src/runtime/lookahead_pump.cpp:35–37`). Decision *"the device
  sink and device-clock mastering (the pump's injected clock becomes the
  device clock; video chases) → `audio.device_monitor`"* (`lookahead.md:109–
  112`) pre-assigned this whole task.
- **`audio.lookahead_recursive_prefetch`** — *settled* (DONE 2026-07-08,
  [`lookahead_recursive_prefetch.md`](lookahead_recursive_prefetch.md)). Made
  the threaded fill (`worker_count > 0`) byte-identical to the inline fill by
  warming the transitive contributor closure. `device_monitor` **runs the
  production threaded path with real worker threads**, so it directly relies
  on this being correct — the refinement names `device_monitor` as that
  consumer (`lookahead_recursive_prefetch.md:113–114`). No new work here; this
  task inherits the byte-identity invariant.
- **`audio.latency`** — *settled* (DONE 2026-07-08,
  [`latency.md`](latency.md)). Constant `latency()` is honored as a fill-lead
  extension in the ring; `device_monitor` "primes the ring against a live
  device clock — a latent contributor that isn't pre-rolled underruns at the
  device callback" (`latency.md:66–67`). Inherited unchanged: the device path
  gets the pre-rolled horizon for free.
- **`timeline.transport`** — *settled* (DONE 2026-07-07,
  [`tasks/refinements/timeline/transport.md`](../timeline/transport.md)). The
  `Transport` is deliberately **per-viewport, single-owner, no-TSan-obligation
  state** (`src/runtime/arbc/runtime/transport.hpp:26–30`), and it explicitly
  reserves the concurrency this task now introduces: *"The future audio-clock
  master (`01:103–106`) drives the same single-owner seek surface and brings
  its own concurrency coverage in the audio milestone"* (`transport.md:338–
  339`). This task **is** that audio-clock master.
- **`contract.audio_facet`** — *settled* (DONE 2026-07-07). Supplies
  `AudioBlock`/`AudioResult` and the `render_audio` facet the workers run; the
  device path touches none of it directly (the callback only consumes mixed
  blocks).

All dependencies are settled; nothing this task needs is pending.

## What this task is

The interactive audio driver and the transport's audio-clock master. Three
deliverables, all in `arbc::runtime` (L5) except the reference backend
(out-of-lib):

1. **`DeviceSink` interface (`arbc::runtime`, L5, OS-dependency-free).** A
   pure-virtual seam a concrete backend implements: it owns/opens a device
   stream at a device format and, on the device's RT thread, invokes a
   monitor-supplied fill callback to obtain interleaved Float32 frames. The
   interface names **no** OS audio API — it is the audio analog of a codec's
   in-lib decode interface, with the real backend behind it (see Decisions,
   the codec-line delta).

2. **`DeviceMonitor` (`arbc::runtime`, L5).** The runtime object that binds a
   `Transport`, a `LookaheadPump`, and a `DeviceSink`, and owns clock
   mastering:
   - **RT callback (pure consume + count).** For each output block the device
     needs, it calls `LookaheadPump::drain` (`lookahead_pump.hpp:76`) to copy
     an already-mixed block (or silence + an underrun on a starved block,
     never an inline mix), converting the working format to the device format
     at this edge (doc 12:100–104). It then bumps an **atomic delivered-frame
     counter** — its only mutation. No mix, no `render_audio`, no `pull_audio`,
     no allocation, no lock on this thread.
   - **Mastering (single-owner).** A single non-RT owner thread reads the
     delivered-frame delta and advances the `Transport` by
     `delivered_frames / device_rate` as an exact `Time`
     (`transport.hpp:86`) — so the transport derives composition time from
     samples delivered (doc 12:171–178). The `Transport` stays single-owner:
     the RT thread never touches it.
   - **Video chases audio.** The mastered playhead is published as a
     **lock-free atomic `Time` snapshot**; the pump's `playhead_source` and
     video viewports on the same transport sample it to schedule against the
     audio clock (doc 12:173–175, doc 01:103–106). A host `seek`/`set_rate`
     rebases the master and calls `pump.notify_transport_change`
     (`lookahead_pump.hpp:80`) to flush + reprime.
   - **One monitor per transport; free-run fallback.** Attaching a second
     device monitor to a transport is rejected; a transport with **no** device
     monitor free-runs on the injected system clock exactly as today
     (doc 12:176–178).

3. **Reference device backend — a separate out-of-lib plugin artifact.**
   `plugins/<device>/` built as a hand-rolled `MODULE` (`arbc-plugin-<device>`)
   carrying a **private, single-header, cross-platform** audio-backend
   dependency (miniaudio-class), mirroring `plugins/imageseq/CMakeLists.txt:29`
   and its stb-class isolation. The backend dependency never enters `libarbc`
   or `arbc-testing`. This is the end-to-end proof of the real device path,
   the way imageseq is for the codec path (doc 17:157–159).

**Out of scope — each maps to a named leaf or host territory** (see Acceptance
criteria's "Registers no successor"): RealtimeSanitizer annotation of the
callback chain → `audio.rt_safety` (`tasks/45-audio.tji:58–63`, `depends
!device_monitor`); general device-sink **discovery/registration** through a
plugin loader → `runtime.plugin_loading` (M8, the registry is content-kind-only
today, `src/contract/arbc/contract/registry.hpp:45–50`); Spatial pan/attenuation
→ `audio.spatial_policy`; the offline sample-exact drive with **no** device
clock → `audio.export_monitor` (`offline_sequences.md:160–162` — "video chases
audio" is the *device/interactive* discipline; export has no device clock); and
muxing audio with exported frames → the host (doc 12:169).

## Why it needs to be done

`device_monitor` is the gate to **interactive audio playback and A/V sync**.
Today the pump samples an injected `playhead_source`/`tick_source` and no
device consumes `drain`; nothing masters the transport from a hardware clock,
so playback is headless and A/V viewports free-run independently. Doc 12 makes
"device monitor with audio-clock mastering" explicit v1 scope
(`docs/design/12-audio.md:246–259`, sequenced **last**), and the transport
deliberately deferred its clock-master concurrency to this milestone
(`transport.md:338–339`). Directly downstream, `audio.rt_safety`
(`depends !device_monitor`) needs the callback chain to exist before it can
annotate it build-failingly. This task lands the interactive sink, the
sample-mastered clock the transport reserved, and the video-chases-audio
publish — turning the deadline-free ring into an actual playing device.

## Inputs / context

### Governing design doc — doc 12 (normative, doc 16)

- **Device monitor** (`docs/design/12-audio.md:155–164`) — "owns an audio
  device stream … worker threads execute `render_audio` pulls and the mix
  graph *off* the device thread; the device callback only consumes prepared,
  mixed blocks. Arbitrary plugin code never runs on the audio callback — the
  price is lookahead latency on transport changes (play/seek flushes and
  re-primes the ring)." The RT-safety posture this task's callback realizes.
- **Clock mastering** (`:171–178`) — "the device clock *is* the timebase: the
  transport derives composition time from samples delivered, and video
  viewports on the same transport schedule frames against it — **video chases
  audio, never the reverse** … A transport without a device monitor free-runs
  on the system clock as before. One device monitor per transport." The
  behavior this task pins.
- **Working format** (`:100–104`) — "the monitor converts working → device."
  The edge conversion the callback performs.
- **Scheduling decision** (`:246–259`) — device monitor + audio-clock
  mastering are v1; sequenced "device monitor + lookahead scheduler last."
- **Doc 01 §core concepts** (`docs/design/01-core-concepts.md:103–106`) — "a
  device monitor's hardware clock masters the transport, and video chases it."

### Supporting docs

- **Doc 17 §"The codec line"** (`docs/design/17-internal-components.md:150–
  159`, **amended by this task**) — codecs must never ride into an embedder's
  link line; `arbc-plugin-imageseq` ships as a separate `plugins/` artifact
  with its private dep. This task's delta generalizes the line to device
  backends (see Decisions).
- **Doc 17 §levelization** (`:41–44,57,60,84–86`) — "no same-level edges"; and
  "the two render drivers live in `runtime`, not the engines … device clocks
  are runtime policy." Places the `DeviceSink`/`DeviceMonitor` at L5 and the
  backend out-of-lib.
- **Doc 17 §repo layout** (`:161–176`) — `plugins/<name>/` is the out-of-lib
  reference-plugin home (own deps).

### Code seams the implementation extends

- **The pump (the object `DeviceMonitor` adapts):**
  `src/runtime/arbc/runtime/lookahead_pump.hpp` — `LookaheadPumpConfig`
  (`:47–62`) with `playhead_source` (`:59`), `direction_source` (`:61`),
  `tick_source` (`:57`); `LookaheadPump::drain` (`:76`),
  `notify_transport_change` (`:80`), `flush` test barrier (`:90`). The
  "reads-not-masters" callout (`:30–32`) and the RT double-buffer reservation
  (`src/runtime/lookahead_pump.cpp:35–37`) are this task's entry points.
- **The ring drain contract (RT invariant):**
  `LookaheadRing::drain(std::int64_t, AudioBlock&, AudioResult&)`
  (`src/audio_engine/arbc/audio_engine/lookahead.hpp:146`) — prepared block or
  silence + `silence_mixed()` underrun (`:171`), never mixes/allocates/blocks;
  chronic underrun is already flagged "a device_monitor tuning concern"
  (`src/audio_engine/lookahead.cpp:271`).
- **The transport (mastering target):**
  `src/runtime/arbc/runtime/transport.hpp:34` — `position()` (`:42`, the
  immutable sampled `Time` that crosses threads), `advance(Time real_elapsed)`
  (`:86`), `seek(Time)` (`:65`), `set_rate(Rational)` (`:59`), and the
  single-owner / no-TSan design (`:26–30`).
- **Working / device format types:**
  `struct AudioFormat { std::uint32_t sample_rate; ChannelLayout layout; }`
  (`src/media/arbc/media/audio_format.hpp:17–22`, `k_working_audio` `:28`);
  `struct AudioBlock` (non-owning interleaved Float32 view — the `drain`
  target) (`src/media/arbc/media/audio_block.hpp:72–77`),
  `ChannelLayout`/`channel_count` (`:44–51`).
- **The reference-plugin precedent:** `plugins/imageseq/CMakeLists.txt` — the
  STATIC impl + `add_library(arbc-plugin-imageseq MODULE …)` (`:29`), the
  private-decode-dep isolation comment (`:1–8`, "a hand-rolled `MODULE`,
  NOT an `arbc_add_component`"), migrated onto `arbc_add_plugin()` by
  `packaging.plugin_helper` (M9) later.
- **The transport's reserved concurrency seam:** `transport.md:338–339` (this
  task's TSan obligation), `transport.hpp:26–30` (why it is new surface).

### Existing claims to extend, not duplicate

The audio claims are `tests/claims/registry.tsv:67–81`. This task extends the
drain invariant `12-audio#lookahead-prepares-ahead-of-playhead`
(`registry.tsv:76`) — the device callback is its **RT consumer**, not a
restatement — and adds two new claims (see Acceptance criteria). It relies on,
without restating, `12-audio#latency-prerolls-declared-content` (`:81`) and
the recursive-closure byte-identity claim.

## Constraints / requirements

1. **RT-callback purity.** The device callback does only: `pump.drain` per
   needed block (prepared or silence, never inline mix), working→device format
   conversion, and one atomic delivered-frame increment. No `render_audio`,
   `mix_composition`, `pull_audio`, heap allocation, or lock on the callback
   thread — the RT-safety invariant the whole engine buys (doc 12:31–34,155–
   164). This is the surface `audio.rt_safety` later annotates; asserted here
   by a behavioral counter.
2. **Single-owner transport discipline preserved.** Only one (non-RT) owner
   thread mutates the `Transport` (`advance`/`seek`); the RT device thread
   never touches it, communicating solely through the atomic frame counter.
   `transport.hpp:26–30` stays true — no synchronization is added *inside*
   `Transport`.
3. **Exact sample-derived mastering.** The transport advances by
   `delivered_frames / device_rate` as exact `Time` (doc 12:171–172) — the
   device frame count *is* the clock; no wall-clock read on the mastering
   path. Working→device format conversion lives at the callback edge
   (doc 12:100–104).
4. **Video-chases-audio publish is lock-free.** The mastered playhead is
   published as a single-writer atomic `Time` snapshot (release), read by the
   pump's `playhead_source` and by video viewports (acquire) — realizing "only
   an immutable sampled `Time` crosses threads" (`transport.hpp:26–30`)
   without locking the transport. This is **new cross-thread surface** and
   carries the transport-reserved TSan obligation (`transport.md:338–339`).
5. **Free-run fallback is a no-regression.** A transport with no device
   monitor keeps its injected-system-clock advance byte-for-byte; the feature
   adds nothing to the non-device interactive/pump path.
6. **One device monitor per transport** (doc 12:177–178). Attaching a second
   is rejected (asserted), consistent with per-viewport ownership.
7. **Levelization (doc 17).** `DeviceSink` (interface) and `DeviceMonitor`
   live in `arbc::runtime` (L5) — they own a thread/clock/device, which is
   runtime policy (doc 17:84–86) — and name **no** OS audio API. The concrete
   backend and its OS-audio dependency ship as a separate `plugins/<device>/`
   MODULE (`arbc-plugin-<device>`), never on `libarbc`/`arbc-testing`'s link
   line. CI's levelization gate and the link-line check stay green.
8. **Transport change flushes and reprimes.** A host `seek`/`set_rate` rebases
   the master's sample origin and calls `pump.notify_transport_change`
   (flush + reprime); the standard lookahead-latency-on-transport-change trade
   (doc 12:161–164) is preserved.
9. **Determinism of drained audio.** Bytes copied out through the device path
   equal a direct `mix_composition` oracle for the same output windows, and
   are identical between `worker_count == 0` and `worker_count > 0` (inherits
   the recursive-prefetch byte-identity).
10. **Diff coverage ≥ 90%** on changed lines (doc 16 CI gate); tests ship in
    this task.

## Acceptance criteria

- **Claims-register growth** (`tests/claims/registry.tsv`, `12-audio#…`), each
  enforced by a Catch2 block tagged `// enforces: 12-audio#<slug>`:
  1. `12-audio#device-clock-masters-transport` — *"A `DeviceMonitor` attached
     to a transport derives composition time from samples delivered: after the
     device consumes K frames at the device rate, the transport position — and
     the lock-free mastered-playhead snapshot the lookahead pump and video
     viewports on the same transport sample — advance by exactly K/rate; the
     `Transport` is mutated only on the single owner thread (never the RT
     callback), so its single-owner discipline is preserved; a host seek/rate
     change rebases the master and reprimes; a transport with **no** device
     monitor free-runs on the injected system clock byte-identically (no
     regression); one device monitor per transport."*
  2. `12-audio#device-callback-consumes-prepared-blocks-only` — *"The device
     RT callback consumes only prepared, already-mixed blocks through the
     pump's `drain` and issues **zero** `render_audio`/`mix_composition`/
     `pull_audio` invocations and **zero** heap allocations on the callback
     thread; an unready block yields silence + an underrun count, never an
     inline mix; bytes drained through the device path equal a direct
     `mix_composition` oracle and are identical between `worker_count == 0` and
     `worker_count > 0` (extends `12-audio#lookahead-prepares-ahead-of-
     playhead`)."*
- **Byte-exact goldens** (deterministic, no tolerances, doc 16). In
  `src/runtime/t/device_monitor.t.cpp`: a **fake `DeviceSink`** driven by the
  test (it delivers a scripted frame count per simulated callback) over an
  `org.arbc.tone` / `org.arbc.nested` scene; the drained bytes equal a direct
  `mix_composition` oracle for the same windows, byte-identical, and identical
  between `worker_count == 0` and `worker_count > 0`. No hardware, no wall
  clock — the fake sink is the clock.
- **Behavioral-counter assertions** (never wall-clock, doc 16):
  - Callback-thread `render_audio`/`mix_composition`/`pull_audio` count == 0
    and allocation count == 0 over a full playback pass.
  - After the fake sink delivers K frames at rate R, `transport.position()`
    and the published snapshot both equal `K/R` exactly; a starved block
    increments the underrun counter and mixes no audio inline.
  - The free-run (no-monitor) transport advance is unchanged (the device
    mastering path mutates the transport zero times when no monitor is
    attached).
- **Concurrency / TSan** (doc 16 mandates it for the audio engine; the
  transport reserved it here). A TSan/stress test racing the mastered-clock
  publish: the fake device thread bumps the frame counter + a mastering step
  advances the transport while the pump and a simulated viewport read the
  snapshot concurrently — assert no data race on the snapshot, the transport
  is mutated on one thread only, each `AudioCompletion` settles once, and the
  drained samples equal the inline-mode goldens. New target
  `tests/device_monitor_concurrency.t.cpp` wired in `tests/CMakeLists.txt`,
  alongside the existing `tests/audio_lookahead_concurrency.t.cpp`.
- **Reference-backend artifact.** `plugins/<device>/` builds a hand-rolled
  `MODULE` (`arbc-plugin-<device>`) with its OS-audio dependency **private**
  and absent from `libarbc`/`arbc-testing`'s link line (the imageseq
  constraint, `plugins/imageseq/CMakeLists.txt:1–8`). A build + **hardware-
  gated** smoke check constructs the sink and opens/closes a stream when a
  device is present (skipped in headless CI); the deterministic behavioral
  coverage is the fake sink, not the backend.
- **No new conformance family.** The monitor is runtime machinery, not a
  content kind or operator; its tests drive `org.arbc.tone`/`org.arbc.nested`
  through the monitor — no `arbc-testing` family added.
- **Design-doc delta (same-commit, doc 16).** doc 17 §"The codec line" is
  generalized to device backends and doc 00's "Internal components" bullet
  updated (see Decisions). doc 12's clock-mastering text is implemented
  verbatim — no doc 12 delta.
- **WBS gate.** After the closer adds `complete 100` and the `Refinement:`
  back-link to `tasks/45-audio.tji`, `tj3 project.tjp 2>&1 | grep -iE
  "error|warning"` is silent.
- **Registers no successor.** Every implementable item this task does not do
  already maps to a named leaf (milestone `m6_audio`,
  `tasks/99-milestones.tji:51`): RT-safety annotation → `audio.rt_safety`
  (behind this); general device-sink discovery/registration → the deferred
  plugin loader `runtime.plugin_loading` (M8); Spatial → `audio.spatial_policy`;
  offline drive → `audio.export_monitor`; muxing → host. **Device-loss /
  hot-plug / mid-stream device-format-change resilience** is a v1-scope
  *judgment* call (it needs the real backend to be meaningful and may be v2
  hardening) — surfaced in the return summary for the parking lot, **not**
  encoded as a WBS leaf. This task creates no new leaf.

## Decisions

- **D1 — `DeviceSink` interface + `DeviceMonitor` are L5 runtime; the backend
  is out-of-lib.** doc 17:84–86 places device clocks in `runtime`; the OS
  audio API is the audio analog of a codec, so the codec line keeps it out of
  `libarbc`. *Rejected:* putting the sink interface in `audio-engine` (L4) — it
  cannot own a thread/clock/device and cannot name `runtime` (doc 17:41–44,84–
  86). *Rejected:* linking a backend into `libarbc` — drags an OS audio
  dependency onto every embedder's link line, exactly what the codec line
  forbids.
- **D2 — the RT thread publishes an atomic frame count; a single owner thread
  masters the transport.** Keeps the `Transport` single-owner
  (`transport.hpp:26–30` preserved), keeps the callback pure (one atomic add),
  and confines all transport mutation to one thread. *Rejected:* mutating the
  `Transport` from the RT callback — races the pump/viewport reads of
  `d_playhead` and puts non-RT-safe modulo/`expected` machinery on the RT
  thread. *Rejected:* making `Transport` internally locked — invents a sharing
  model the design refused (`transport.md:338–339` assigns the concurrency to
  *this* task via the published snapshot, not to the transport type).
- **D3 — the mastered playhead is a lock-free single-writer atomic `Time`
  snapshot read by the pump and viewports.** This realizes "only an immutable
  sampled `Time` crosses threads" (`transport.hpp:26–30`) as video-chases-audio
  without locking the transport, and is the concrete cross-thread surface the
  TSan test covers. *Rejected:* viewports reading `transport.position()`
  directly across threads — races `d_playhead` against the owner thread's
  `advance`. *Rejected:* a mutex around `position()` — RT-hostile and needless
  for a single-writer snapshot.
- **D4 — the reference backend carries a private single-header, cross-platform
  audio-backend dependency (miniaudio-class), isolated exactly like imageseq's
  `imdec.h`.** Mirrors the shipped reference-plugin precedent
  (`plugins/imageseq/CMakeLists.txt`) and keeps the dep off `libarbc`; the
  backend is the end-to-end proof of the device path. *Rejected:* pulling a
  heavyweight backend (PortAudio) into core (violates the codec line and
  doc 10's minimal-deps posture). *Rejected:* shipping no reference impl (the
  `.tji` note requires one, and it is the device path's honesty test). The
  exact library is an implementation choice bounded by *single-header /
  permissive-license / off-`libarbc`-link-line*.
- **D5 — v1 wires the reference sink by direct construction; general
  device-sink discovery rides the deferred plugin loader (M8).** The `Registry`
  is content-kind-only today (`registry.hpp:39–50`) and no host-side `dlopen`
  loader exists yet; building a device-sink registry now would duplicate M8.
  The host/test harness constructs the reference `DeviceSink` and hands it to
  the `DeviceMonitor`. *Rejected:* a bespoke device-sink loader in this task —
  `runtime.plugin_loading` (M8) is its home; premature here.
- **D6 — design-doc delta generalizing doc 17's "codec line" to device
  backends, plus a doc 00 decision-record bullet.** The `.tji` note invokes
  "the doc 17 codec-line policy" for *device* APIs, but doc 17 literally covers
  only codecs and places the monitor in `runtime` without stating the backend
  is externalized. The gap is closed in the constitution, same-commit
  (doc 16): doc 17 §"The codec line" now states the OS audio backend stays off
  `libarbc`'s link line (interface/monitor in `runtime`, backend as a
  `plugins/<device>/` MODULE), and doc 00's "Internal components" bullet
  records it (packaging/levelization is project-shaping). doc 12's
  clock-mastering behavior is implemented verbatim — no doc 12 delta.
  *Delta written:* `docs/design/17-internal-components.md` (§"The codec line")
  and `docs/design/00-overview.md` (§"Resolved questions" → Internal
  components).

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-08.

- `DeviceSink` pure-virtual interface (L5, OS-API-free): `src/runtime/arbc/runtime/device_sink.hpp`.
- `DeviceMonitor` runtime adapter (L5, single-owner clock mastering, lock-free atomic playhead publish): `src/runtime/arbc/runtime/device_monitor.hpp`, `src/runtime/device_monitor.cpp`.
- Out-of-lib reference backend (`arbc-plugin-miniaudio`, private miniaudio single-header): `plugins/miniaudio/CMakeLists.txt`, `plugins/miniaudio/miniaudio_plugin.cpp`, `plugins/miniaudio/miniaudio_sink.cpp`, `plugins/miniaudio/maudio.cpp`, `plugins/miniaudio/arbc/device_miniaudio/miniaudio_sink.hpp`, `plugins/miniaudio/third_party/maudio.h`.
- Unit tests with byte-exact goldens and behavioral counters (K/R oracle, starved→silence+underrun, seek/set_rate rebase, one-per-transport, free-run no-regression): `src/runtime/t/device_monitor.t.cpp`.
- Concurrency/TSan test racing mastered-clock publish against pump and viewport readers: `tests/device_monitor_concurrency.t.cpp`; wired in `tests/CMakeLists.txt`.
- Hardware-gated smoke check (constructs/opens/closes backend; skips headless) and codec-line containment check (`maudio_` symbols absent from libarbc/arbc-testing): `tests/miniaudio_smoke.t.cpp`, `tests/miniaudio_containment.t.cpp`; wired in `tests/CMakeLists.txt`.
- Claims register: `12-audio#device-clock-masters-transport`, `12-audio#device-callback-consumes-prepared-blocks-only` added to `tests/claims/registry.tsv`.
- Design-doc deltas (same-commit): `docs/design/17-internal-components.md` (§"The codec line" generalized to device backends), `docs/design/00-overview.md` (§"Resolved questions" decision-record bullet).
- Build wiring: `CMakeLists.txt` (`add_subdirectory(plugins/miniaudio)`); `src/runtime/CMakeLists.txt` (device_monitor component).
