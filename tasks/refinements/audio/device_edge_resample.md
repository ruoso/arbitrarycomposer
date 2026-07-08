# audio.device_edge_resample — Device-edge sample-rate conversion

## TaskJuggler entry

Back-link: [`tasks/45-audio.tji`](../../45-audio.tji) — `task device_edge_resample`
(lines 40–45).

> "Device-edge sample-rate conversion when device_rate ≠ working_rate
> (currently rejected at construction), reusing the shipped windowed-sinc
> polyphase resampler. Source: audio.device_monitor implementation gap; see
> tasks/refinements/audio/device_monitor.md Status block. Doc 12."

## Effort estimate

`effort 1d` (`tasks/45-audio.tji:41`). The heavy machinery already exists: the
frozen 16-tap / 32-phase Blackman-Harris coefficient table and its exact
integer phase arithmetic ship in `resample_audio`
(`src/media/audio_resampler.cpp:40–604`), and the RT drain that must host the
conversion ships in `DeviceMonitor::fill_rt`
(`src/runtime/device_monitor.cpp:133–162`). This task adds a *streaming* seam
over the existing kernel (a rational input cursor + a small history carry),
removes one construction guard, and wires the seam into the drain with its
goldens. One focused day against shipped DSP.

## Inherited dependencies

- **`audio.device_monitor`** — ***settled*** (Done 2026-07-08,
  `tasks/refinements/audio/device_monitor.md` Status block; git `13ca1bb`). It
  landed the `DeviceSink` interface (`src/runtime/arbc/runtime/device_sink.hpp`),
  the clock-mastering `DeviceMonitor`
  (`src/runtime/arbc/runtime/device_monitor.hpp` + `device_monitor.cpp`), the
  out-of-lib `arbc-plugin-miniaudio` reference backend (`plugins/miniaudio/`),
  and — critically — the *explicit deferral this task closes*: the
  device-rate ≠ working-rate rejection at `device_monitor.cpp:46–52`, whose
  comment already names the fix ("reusing the shipped windowed-sinc
  resampler"). It hands this task a working RT drain, a device-rate-based clock
  master, and a documented insertion point.
- **`kinds.nested_audio_resampling`** — ***settled*** (git `4cd7faa`). It
  shipped the polyphase kernel `resample_audio`
  (`src/media/arbc/media/audio_resampler.hpp:33`) this task reuses, enforced by
  claim `12-audio#nested-boundary-resamples-below-rate-children`
  (`tests/claims/registry.tsv:72`).
- **`audio.mix_engine`** — ***settled*** (git `e57e5f8`). It extended the
  kernel for below-rate reconstruction and is the `mix_composition` oracle the
  device-edge golden reconstructs against.

All dependencies are settled; nothing this task needs is pending. The sibling
`audio.seek_drain_realign` (`tasks/45-audio.tji:46–51`) is a *peer*, not a
predecessor — see Constraint 8 for the seam split on the seek path.

## What this task is

Realize the working → device conversion at the rate axis, which
`device_monitor` deferred. Four deliverables:

1. **A streaming resampler seam in `arbc::media`.** A stateful
   continuous-stream front-end over the shipped polyphase bank — it reuses the
   checked-in `k_resampler_coeffs` table and the exact-integer phase math from
   `resample_audio` (no second coefficient table, no second algorithm), adding
   a rational input-sample cursor and a filter-support history carry so a
   long stream fed in successive chunks is byte-continuous with a single
   whole-stream reconstruction.
2. **Remove the construction rejection for `device_rate > working_rate`**
   (`device_monitor.cpp:46–52`) and size the resampler's RT-owned state at
   construction (mirroring the pre-sized `d_scratch`, `device_monitor.hpp:140`).
3. **Wire the seam into `DeviceMonitor::fill_rt`** so the drain produces device
   frames at the device rate from working-rate blocks, keeping the existing
   1:1 memcpy/layout path when `device_rate == working_rate` (no regression, no
   SRC cost).
4. **Flush the resampler state** (phase cursor + history) on the
   transport-change reprime so post-seek/-rate output restarts byte-exact.

**Out of scope.** *Downsampling* (`device_rate < working_rate`, e.g. 48 kHz
working → 44.1 kHz device) needs the reconstruction filter cut at the lower
device Nyquist to stay anti-aliased — an extension of the fixed-cutoff bank,
not a reuse of it — and is deferred to the named leaf
`audio.device_edge_decimation` (Decision D3, Acceptance). This task keeps a
construction rejection for the below-working device rate (now a *tested*
rejection). *Post-seek RT-drain-cursor realignment* is the peer leaf
`audio.seek_drain_realign`; this task flushes only the resampler's own filter
memory (Constraint 8). *Time-stretch / pitch-preserving* SRC stays deferred
with the effects stack (doc 12:116–118). *Device hot-plug / mid-stream format
change* remains parked (device_monitor return summary).

## Why it needs to be done

Without it, `DeviceMonitor` can only be attached to a device whose native rate
exactly equals the composition's working rate; any other device throws at
construction (`device_monitor.cpp:50–51`). That makes the M6 promise —
"Glitch-free device playback mastering the transport clock"
(`tasks/99-milestones.tji:52`) — conditional on the host having pre-matched the
rates. High-resolution interfaces (96 kHz, 192 kHz) against the 48 kHz working
default are the common upsample case this unblocks. Doc 12 states the edge
conversion as a settled promise ("the monitor converts working → device",
`docs/design/12-audio.md:100–101`); this task turns that promise into working,
tested code. `m6_audio` depends on it directly (`tasks/99-milestones.tji:51`).

## Inputs / context

### Governing design doc — doc 12 (normative, doc 16)

- **`docs/design/12-audio.md:94–104` ("Working format")** — the edge-conversion
  contract: "Conversions live at the edges … the monitor converts working →
  device." This task's design-doc delta (below) extends this section to name
  the polyphase reuse and the v1 upsample-only scope.
- **`docs/design/12-audio.md:171–178` ("Clock mastering")** — the device clock
  *is* the timebase; the transport derives time from *device frames delivered*;
  video chases audio. The clock advance is already device-rate-based
  (`device_monitor.cpp:54,161,194`) and is unchanged by this task — only the
  working-block → device-frame *production* in `fill_rt` changes.
- **`docs/design/12-audio.md:112–115` ("Varispeed")** — the same
  "resample through the composed rational rate, exact rationals" discipline the
  device edge inherits: `device_rate : working_rate` is an exact rational ratio.
- **`docs/design/12-audio.md:192–212` ("Sync and latency")** — v1 honors
  declared `latency()` as a *fill-lead extension only*, never window
  re-alignment. The device edge is downstream of the pull graph, so the
  resampler's constant group delay is absorbed by the monitor's existing
  lookahead pre-roll, not expressed through `AudioFacet::latency()`
  (Decision D6).

### Design-doc delta shipped with this task

`docs/design/12-audio.md:100–104` (the "Working format" section) gains a
paragraph: the working → device edge reuses the one shipped polyphase kernel;
`device_rate == working_rate` is a 1:1 drain, `device_rate > working_rate` is
upsampled, and downsampling is deferred (mirroring the section's existing
time-stretch/PDC deferrals). Per doc 16 the delta rides in the closer's commit.
This refines an under-specified promise (not a new seam or a deviation), so no
doc-00 decision-record bullet is added.

### Code seams the implementation extends

- **`src/media/arbc/media/audio_resampler.hpp:33`** + **`audio_resampler.cpp`**
  — the shipped kernel. `resample_audio(const AudioBlock& in, AudioBlock& out)`:
  16-tap / 32-phase Blackman-Harris, frozen coefficients `k_resampler_coeffs`
  (`audio_resampler.cpp:40–555`), exact-integer phase math with one rounding
  (`audio_resampler.cpp:577–589`), OOB taps read as zero, per-channel. It is
  **block-anchored and stateless**: `out` frame 0 samples `in` frame 0's
  instant, and it **no-ops unless `0 < in.rate < out.rate`**
  (`audio_resampler.cpp:563–568`). Both properties drive Decisions D2 and D3.
- **`src/runtime/device_monitor.cpp:46–52`** — the guard this task removes for
  the upsample direction (and covers with a rejection test for the downsample
  direction). Currently untested (the only construction-throw test asserts the
  second-monitor `logic_error`, `device_monitor.t.cpp:539`).
- **`src/runtime/device_monitor.cpp:133–162`** — `fill_rt`, the RT callback:
  drains a working-rate block (`d_pump.drain`, line 145), carries partial blocks
  across callbacks (`d_carry_frames`/`d_carry_pos`), and hands frames to the
  device buffer via `convert_frames` (layout-only, lines 107–131). The
  resampler front-end sits between the drain and the layout convert.
- **`src/runtime/device_monitor.cpp:54` & `93–105`** — `d_flicks_per_frame` and
  `start_block_index()`. Both mix a device-rate frame duration with a
  working-rate block count; they coincide only when rates match. Under SRC the
  implementation must keep the two axes distinct (Constraint 3).
- **`src/runtime/device_monitor.hpp:139–143`** — the RT-callback-owned scratch /
  cursor block; the resampler's history + phase state joins it, pre-sized at
  construction.
- **`src/runtime/device_monitor.cpp:200–206`** (`master_step` reprime) — the
  transport-change path (`notify_transport_change`) the resampler flush hooks.
- **`src/audio_engine/mix.cpp:156`** — the sole existing `resample_audio` call
  site (below-rate nesting), a reference for the AudioBlock-in/-out convention.

### Existing claims to extend, not duplicate

- `12-audio#device-callback-consumes-prepared-blocks-only`
  (`tests/claims/registry.tsv:83`) — the drain-equals-oracle, worker-count
  identity claim this task's new claim extends to the resampled edge.
- `12-audio#device-clock-masters-transport` (`registry.tsv:82`) — the
  device-frame → transport-time mapping this task must preserve unchanged.
- `12-audio#nested-boundary-resamples-below-rate-children` (`registry.tsv:72`)
  — the kernel-reuse and byte-exactness discipline the new claim inherits.

## Constraints / requirements

1. **RT-safety (the whole-engine invariant).** `fill_rt` stays allocation-free
   and lock-free: the resampler's history buffer and phase state are pre-sized
   at construction from `block_frames` and the rate ratio, and resampling is
   ordered no-libm float32 MACs over the checked-in table. No `std::sin`, no
   heap, no lock on the callback thread (`device_monitor.hpp:30–34`, the
   Constraint-1 lineage).
2. **Byte-exact, tolerance-free reuse.** The streaming seam produces samples
   bit-identical to a single whole-stream `resample_audio` reconstruction of
   the same input — it reuses `k_resampler_coeffs` and the exact-integer phase
   arithmetic verbatim; it does not introduce float accumulation, a second
   table, or a tolerance (doc 16:48–53).
3. **Two rate axes, kept distinct.** The lookahead-ring block geometry and the
   drain index are in **working-rate** frames; the clock advance and delivered
   count are in **device-rate** frames. `start_block_index()` /
   `d_flicks_per_frame` (`device_monitor.cpp:54,93–105`) must not conflate them
   under SRC — the drain block span derives from the *working* rate, the clock
   advance from the *device* rate. The mastering step
   (`device_monitor.cpp:186–194`) already advances by
   `delivered_device_frames × device_flicks_per_frame`; keep it unchanged.
4. **Continuity across block and callback boundaries.** Because the kernel is
   block-anchored with OOB-zero taps, per-block calls would restart the phase
   and truncate filter support at every seam. The streaming seam carries a
   rational input cursor and filter-support history so output is continuous —
   no boundary click, byte-identical to the whole-stream reconstruction
   (Constraint 4's golden pins this).
5. **1:1 path preserved.** When `device_rate == working_rate` the drain keeps
   the existing memcpy/layout path (`convert_frames`, `device_monitor.cpp:107–131`)
   — zero resampler engagement, byte-for-byte unchanged, so the
   `12-audio#device-callback-consumes-prepared-blocks-only` matched-rate golden
   does not regress.
6. **Upsample only; downsample rejected and tested.** `device_rate >
   working_rate` is handled; `device_rate < working_rate` throws
   `std::invalid_argument` at construction with a message naming the successor
   (`audio.device_edge_decimation`). This rejection gets the test the current
   guard lacks.
7. **Levelization (doc 17, CI-enforced).** The streaming resampler is DSP and
   belongs in `arbc::media` (L1) beside `resample_audio` — it may depend only on
   `base` (L0, rational rates) and `media`'s own `AudioBlock`. The
   **device rate is a parameter passed down from `runtime` (L5)**, never
   discovered by the engine querying a device; no OS audio API enters `libarbc`
   / `arbc-testing` (`docs/design/17-internal-components.md`, "The codec line" /
   device-backend policy).
8. **Seek/rate-change flushes resampler state only.** On
   `notify_transport_change` (`device_monitor.cpp:200–206`) the resampler's
   phase cursor + history reset so post-flush output restarts byte-exact. This
   task owns *only* the resampler's filter memory; realigning the RT *drain
   cursor* to the reprimed window is the peer leaf `audio.seek_drain_realign`.
   Consequently this task's goldens exercise steady-state SRC and a
   flush-resets-filter-state counter, not full post-seek byte-exactness (which
   needs both tasks).
9. **Diff coverage ≥ 90%** on changed lines (doc 16 CI gate); tests ship in
   this task.

## Acceptance criteria

- **Claims-register growth.** Add
  `12-audio#device-edge-resamples-working-to-device` to
  `tests/claims/registry.tsv`, invariant:
  *"A DeviceMonitor whose device rate exceeds the working rate resamples the
  drained working-rate mix to the device rate with the shipped polyphase kernel
  (`resample_audio`'s coefficient table + exact phase math, no second
  algorithm): the device-delivered bytes equal a single whole-stream
  `resample_audio` reconstruction of the `mix_composition` oracle over the same
  window, byte-exact with no tolerance and continuous across block/callback
  boundaries (no per-block phase restart), byte-identical between
  worker_count == 0 and worker_count > 0; the mastered clock still advances by
  delivered_device_frames / device_rate; a seek/rate change flushes the
  resampler phase + history so post-flush output restarts byte-exact; a device
  rate equal to the working rate keeps the 1:1 drain unchanged, and a device
  rate below the working rate is rejected at construction (extends
  12-audio#device-callback-consumes-prepared-blocks-only)."* Enforced by a
  Catch2 block tagged `// enforces: 12-audio#device-edge-resamples-working-to-device`.
- **Byte-exact goldens** (no tolerances, doc 16), driving `org.arbc.tone`
  through a fake `DeviceSink` whose `format()` reports a device rate above the
  working rate, asserting the drained device bytes equal a whole-stream
  `resample_audio` of the working-rate `mix_composition` oracle:
  - an **integer ratio** (48 kHz working → 96 kHz device, 1:2), and
  - a **coprime rational ratio** (e.g. 32 kHz working → 48 kHz device, 2:3) to
    exercise the phase accumulator and history carry;
  - a **continuity golden**: many successive `fill_rt` chunks concatenated
    equal one whole-stream reconstruction of the concatenated working mix (pins
    Constraint 4 — no boundary click);
  - each asserted **byte-identical between `worker_count == 0` and
    `worker_count > 0`**, extending `device_monitor.t.cpp`'s golden pattern and
    living beside it (and/or `tests/audio_mix_goldens.t.cpp` /
    `tests/nested_audio_resampling_goldens.t.cpp` for the pure-kernel streaming
    equivalence).
- **Behavioral-counter assertions** (never wall-clock, doc 16), on the
  deterministic `flush_master()` barrier (`device_monitor.hpp:105`):
  - **flush-resets-resampler-state** — after a `seek`/`set_rate`, the first
    post-flush output block byte-matches a fresh resampler start (Constraint 8);
  - **matched-rate no-regression** — `device_rate == working_rate` still drains
    byte-identical to the current 1:1 oracle with zero resampler engagement
    (Constraint 5);
  - **downsample rejection** — `device_rate < working_rate` throws
    `std::invalid_argument` (`REQUIRE_THROWS_AS`), closing the untested-guard
    gap and pinning the deferral to `audio.device_edge_decimation`.
- **Concurrency / TSan.** Extend `tests/device_monitor_concurrency.t.cpp` with
  an upsample (`device_rate > working_rate`) config so the streaming resampler
  runs on the RT callback concurrently with the master thread; assert clean
  under TSan. The resampler state is RT-thread-single-owner (like `d_scratch`),
  so the only cross-thread channels remain the atomic `d_delivered` / published
  playhead — the assertion is that the resampler adds no new shared mutable
  state.
- **No new conformance family.** This extends the audio engine's device path;
  it lands no content kind or operator, so the contract conformance suite is
  untouched.
- **WBS gate.** After the closer's `.tji` edits, `tj3 project.tjp 2>&1 |
  grep -iE "error|warning"` is silent.
- **Registers exactly one successor** — `audio.device_edge_decimation`
  (below); no re-audit/revisit leaf.

### Named future task (closer registers in WBS)

- **`audio.device_edge_decimation`** — effort **1d**. Add a ratio-scaled
  (widened-lowpass) decimating path to the polyphase resampler so
  `device_rate < working_rate` (e.g. 48 kHz → 44.1 kHz) stays anti-aliased at
  the device edge, and remove the remaining downsample rejection. `depends
  !device_edge_resample`; `note` cites this refinement. Wire into **`m6_audio`**
  (`tasks/99-milestones.tji:49–53`), the milestone this task already belongs to.

## Decisions

- **D1 — Resampling lives in the device drain, over the media-level kernel.**
  The conversion is inserted in `DeviceMonitor::fill_rt`
  (`device_monitor.cpp:133–162`), reusing the `arbc::media` (L1) polyphase bank;
  the device rate flows down from `runtime` (L5) as a parameter. Authority: doc
  12:100–101 ("the monitor converts working → device"), doc 17 device-backend
  policy. *Rejected:* a new resampler owned by the monitor/plugin — would
  duplicate the frozen coefficient table and risk the OS-audio-free `libarbc`
  boundary; the kernel is engine-level DSP, the device rate is runtime policy.

- **D2 — Add a streaming seam over the shipped kernel, not per-block calls.**
  The shipped `resample_audio` is block-anchored (out[0] ↔ in[0] instant) and
  stateless with OOB-zero taps (`audio_resampler.hpp:20–32`). A continuous
  device stream therefore needs a rational input cursor + filter-support history
  carried across callbacks, reusing `k_resampler_coeffs` and the exact-integer
  phase math verbatim. *Rejected:* calling `resample_audio` once per drained
  working block — restarts the phase each block and truncates filter support at
  every seam (OOB-zero taps), producing a periodic boundary click and output
  that is *not* byte-continuous with the whole-stream reconstruction; it fails
  the continuity golden and is audibly broken.

- **D3 — v1 handles upsampling; downsampling is a named deferral.** The shipped
  bank's sinc is cut at the *input* Nyquist — correct band-limited
  reconstruction for `device_rate > working_rate`. Downsampling needs the
  lowpass cut at the lower *device* Nyquist to avoid aliasing, which the
  fixed-cutoff table does not provide. v1 accepts device rate ≥ working rate and
  rejects below-working device rates (now tested), deferring the decimating
  path to `audio.device_edge_decimation`. Rationale: the 48 kHz working default
  matches most modern hardware (no SRC) or is exceeded by high-res interfaces
  (upsample); the below-working (legacy 44.1 kHz) case is the minority and a
  host can sidestep it entirely by setting the working rate to the device rate.
  *Rejected:* fixed-cutoff downsampling now — aliases audible-band content for
  larger ratios and mutates a media-level function with its own byte-exact
  golden and claim, past a 1d scope. *Rejected:* rejecting *all* mismatches and
  requiring the host to pre-match — abandons the high-res upsample case the
  task exists to unblock.

- **D4 — Flush resets resampler filter memory on transport change.** The
  reprime path (`device_monitor.cpp:200–206`) resets the phase cursor + history
  so post-seek output carries no pre-seek filter tail. *Rejected:* leaving the
  filter primed across a seek — post-seek output would smear pre-seek samples,
  breaking the flush-resets-filter-state counter and post-seek byte-exactness.

- **D5 — Pre-size all resampler state at construction; RT drain stays
  allocation-free.** The history buffer and phase state size from `block_frames`
  and the rate ratio in the ctor, beside `d_scratch`
  (`device_monitor.hpp:140,55`). *Rejected:* per-callback allocation — a
  Constraint-1 (RT-safety) violation the whole engine is built to avoid.

- **D6 — The edge group delay is absorbed by the lookahead pre-roll, not a
  facet `latency()`.** The device edge is downstream of the pull graph, and v1
  latency handling is fill-lead only (doc 12:208–212); the resampler's constant
  half-filter-support group delay (≈ 8 working samples) is a fixed edge offset
  within the configured pre-roll floor
  (`src/audio_engine/lookahead.cpp:61–99`), well below perceptual A/V-sync
  thresholds. *Rejected:* plumbing the edge delay through
  `AudioFacet::latency()` — that mechanism is for in-graph contributors and v1
  uses it only to extend the fill lead, not to re-align device output; the edge
  is not a `Content`.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-08.

- `src/media/arbc/media/streaming_resampler.hpp` — `StreamingResampler` seam declaration: rational input cursor + allocation-free filter-support history carry over the shipped polyphase kernel.
- `src/media/t/streaming_resampler.t.cpp` — 4 media-unit test cases for `StreamingResampler` (continuity, integer ratio, coprime rational, flush resets state).
- `src/media/audio_resampler.cpp` — factored shared `frame_pos`/`mac_frame`/support-bound helpers, rewrote `resample_audio` over them, added `StreamingResampler` impl (rational cursor + allocation-free history carry, exact-integer phase math reused verbatim).
- `src/media/CMakeLists.txt` — registered `streaming_resampler.hpp` header and `streaming_resampler.t.cpp` test.
- `src/runtime/arbc/runtime/device_monitor.hpp` / `src/runtime/device_monitor.cpp` — removed the upsample rejection (keeping downsample rejection, renamed `audio.device_edge_decimation`), pre-sized RT-owned resampler state at construction, wired SRC path into `fill_rt` (1:1 path untouched), split drain-block/clock-rate axes in `start_block_index` (Constraint 3), flush-on-transport-change via RT-consumed atomic flag.
- `src/runtime/t/device_monitor.t.cpp` — upsample goldens (1:2, 2:3, continuity; both worker counts vs whole-stream oracle), matched-rate no-regression, downsample rejection, flush-resets-filter-state counter.
- `tests/device_monitor_concurrency.t.cpp` — upsample TSan case with mid-race seek exercising the flush handoff.
- `tests/claims/registry.tsv` — claim `12-audio#device-edge-resamples-working-to-device` added.
- `docs/design/12-audio.md` — "Working format" section extended: polyphase reuse for `device_rate > working_rate`, 1:1 drain when matched, downsampling deferred to `audio.device_edge_decimation`.
