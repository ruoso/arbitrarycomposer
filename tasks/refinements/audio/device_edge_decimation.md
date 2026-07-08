# audio.device_edge_decimation — Device-edge decimating SRC (widened-lowpass)

## TaskJuggler entry

Back-link: [`tasks/45-audio.tji`](../../45-audio.tji) — `task device_edge_decimation`
(lines 47–52).

> "Add a ratio-scaled widened-lowpass decimating path to the polyphase resampler
> so device_rate < working_rate (e.g. 48 kHz → 44.1 kHz) stays anti-aliased at
> the device edge, removing the remaining downsample rejection at construction.
> Source: audio.device_edge_resample tech-debt; see
> tasks/refinements/audio/device_edge_resample.md Status block. Doc 12."

## Effort estimate

`effort 1d` (`tasks/45-audio.tji:48`). This closes the one direction the
predecessor deferred, and the whole surrounding machinery already ships: the
streaming seam (`StreamingResampler` — rational cursor + allocation-free history
carry, `src/media/arbc/media/streaming_resampler.hpp:40–81`), its wiring into the
RT drain (`DeviceMonitor::fill_rt` SRC branch, `src/runtime/device_monitor.cpp:198–234`),
construction pre-sizing and the transport-change flush handoff
(`device_monitor.cpp:63–69,203–207,288–294`), and the byte-exact / TSan golden
harness (`src/runtime/t/device_monitor.t.cpp`, `tests/device_monitor_concurrency.t.cpp`)
are all in place from `audio.device_edge_resample`. The genuinely new work is
one DSP extension — a **ratio-scaled widened lowpass** cut at the device Nyquist,
generated off the RT thread over the same windowed-sinc prototype — plus relaxing
two guards (the media no-op guard and the runtime rejection) and turning the
predecessor's rejection *test* into a decimation *golden*. One focused day of DSP
against shipped seams.

## Inherited dependencies

- **`audio.device_edge_resample`** — ***settled*** (Done 2026-07-08,
  `tasks/refinements/audio/device_edge_resample.md` Status block; git `d6851fa`).
  It shipped everything this task builds over: the `StreamingResampler` seam
  (`src/media/arbc/media/streaming_resampler.hpp`), the shared kernel helpers it
  factored (`FramePos`/`frame_pos`/`mac_frame`/`support_*`,
  `src/media/audio_resampler.cpp:562–612`), the `fill_rt` SRC branch and two-rate-axis
  split (`device_monitor.cpp:59–61,107–120,198–234`), construction pre-sizing and
  the atomic flush handoff (`device_monitor.cpp:63–69,203–207,288–294`), and — the
  explicit deferral **this task closes** — the below-working device-rate rejection
  at `device_monitor.cpp:46–53`, whose message already names
  `audio.device_edge_decimation`. Its Decision D3
  (`device_edge_resample.md:317–331`) is this task's charter: the shipped bank's
  sinc is cut at the *input* Nyquist (correct for upsampling); downsampling needs
  the lowpass cut at the lower *device* Nyquist to stay anti-aliased, "an extension
  of the fixed-cutoff table, not a reuse of it."
- **`kinds.nested_audio_resampling`** — ***settled*** (git `4cd7faa`). Shipped the
  frozen 16-tap / 32-phase Blackman-Harris polyphase kernel `resample_audio`
  (`src/media/arbc/media/audio_resampler.hpp:33`) and its exact-integer phase math,
  enforced by claim `12-audio#nested-boundary-resamples-below-rate-children`
  (`tests/claims/registry.tsv:72`). This task extends the same kernel with a new
  rate direction, exactly as `audio.mix_engine` extended it for below-rate
  reconstruction (`tasks/refinements/audio/mix_engine.md`).
- **`audio.device_monitor`** — ***settled*** (git `13ca1bb`). Landed the
  `DeviceMonitor` / `DeviceSink` / clock-mastering machinery and the device-rate
  clock advance this task must preserve unchanged (`device_monitor.cpp:270`).

All dependencies are settled; nothing this task needs is pending. The peer leaf
`audio.seek_drain_realign` (Done, git `be4e69e`) already realigns the RT drain
cursor on seek for the SRC path — so unlike the predecessor, this task's post-seek
goldens can assert full byte-exactness (both halves of the seek path now ship).

## What this task is

Realize the working → device conversion for the **decimation** direction
(`device_rate < working_rate`), the last case `device_edge_resample` left rejected
at construction. Four deliverables:

1. **A ratio-scaled widened-lowpass decimation path in the `arbc::media` kernel.**
   Extend the polyphase machinery so that, for `src_rate > dst_rate`, the
   reconstruction/anti-alias lowpass is cut at the *device* (lower) Nyquist rather
   than the fixed input Nyquist — the impulse response is widened by the decimation
   ratio `src_rate/dst_rate`, and the tap support scales with it so stopband
   attenuation is preserved. Coefficients are generated off the RT thread over the
   **same** Blackman-Harris windowed-sinc prototype and 32-phase bank as the frozen
   upsampling table (one generator, not a second algorithm); the RT inner loop stays
   the same no-libm ordered float32 MACs. Both the stateless whole-stream
   `resample_audio` (the oracle) and the stateful `StreamingResampler` (the device
   path) are extended over this shared generator so they stay byte-identical.
2. **Relax the two upsample-only guards.** The media no-op guard
   `in.rate >= out.rate` (`audio_resampler.cpp:619`) relaxes to still no-op only on
   `in.rate == out.rate` (and zero/layout/null), admitting the `in.rate > out.rate`
   decimation branch; the runtime rejection `device_rate < working_rate`
   (`device_monitor.cpp:46–53`) is removed, and `d_resampling`
   (`device_monitor.cpp:54`) widens from `device_rate > working_rate` to
   `device_rate != working_rate` so the SRC branch engages for both directions.
3. **Size the widened state at construction and drain it decimating in `fill_rt`.**
   `StreamingResampler::configure` sizes its history capacity from the widened
   support (scaled by the decimation ratio) so `push_input`/`can_produce`/`produce`
   stay allocation-free on the callback thread; `fill_rt`'s existing SRC batch loop
   (`device_monitor.cpp:198–234`) must be audited for the *opposite* frame-count
   direction (decimation emits **fewer** device frames than working frames consumed,
   where upsampling emitted more).
4. **Reuse the existing flush + realign handoff unchanged.** The
   transport-change flush (`device_monitor.cpp:203–207,288–294`) and the peer
   `seek_drain_realign` drain-cursor realignment already cover the SRC path once
   `d_resampling` is true for decimation — no new flush machinery, only its goldens
   extended to the decimation ratio.

**Out of scope.** *Upsampling and matched-rate* are unchanged (the frozen table and
1:1 memcpy path are untouched — no regression). *Time-stretch / pitch-preserving*
SRC stays deferred with the effects stack (doc 12:131–133). *Device hot-plug /
mid-stream format change* remains parked (device_monitor return summary). This task
registers **no successor** — it is the terminal leaf of the device-edge SRC chain
(both rate directions handled after it lands).

## Why it needs to be done

After `device_edge_resample`, a `DeviceMonitor` handles matched and above-working
device rates but still throws at construction for any device rate *below* the
working rate (`device_monitor.cpp:46–53`). The 44.1 kHz consumer-audio rate against
the 48 kHz working default is the single most common such case — CD-lineage DACs,
Bluetooth codecs, and much consumer hardware run 44.1 kHz. Until this lands, the M6
promise — "Glitch-free device playback mastering the transport clock"
(`tasks/99-milestones.tji:52`) — excludes that hardware unless the host pre-lowers
the composition working rate to 44.1 kHz. Doc 12 currently states downsampling as an
*explicit deferral* ("a monitor whose device rate is below the working rate is
rejected", `docs/design/12-audio.md:111–116`); this task turns that deferral into
working, anti-aliased, tested code and closes the last device-edge rate case.
`m6_audio` depends on it directly (`tasks/99-milestones.tji:51`).

## Inputs / context

### Governing design doc — doc 12 (normative, doc 16)

- **`docs/design/12-audio.md:94–119` ("Working format")** — the edge-conversion
  contract and the paragraph the predecessor added: "The working → device edge
  reuses the same band-limited polyphase resampler … A device whose rate equals the
  working rate keeps a byte-for-byte 1:1 drain; a device rate *above* the working
  rate is upsampled at the edge. **Downsampling … needs the reconstruction filter
  cut at the lower device Nyquist to stay anti-aliased, an extension of the
  fixed-cutoff kernel that is deferred … until it lands a monitor whose device rate
  is below the working rate is rejected**" (lines 111–116). This task's design-doc
  delta (below) closes that deferral in place.
- **`docs/design/12-audio.md:127–130` ("Varispeed")** — the "resample through the
  composed rational rate, exact rationals" discipline the device edge inherits:
  `device_rate : working_rate` is an exact rational ratio, decimation included.
- **`docs/design/12-audio.md:186–196` ("Clock mastering")** — the device clock *is*
  the timebase; the transport advances by delivered device frames / device rate. The
  clock advance is already device-rate-based (`device_monitor.cpp:270`) and is
  **unchanged** by this task — only the working-block → device-frame *production*
  gains the decimation direction.
- **`docs/design/12-audio.md:210–230` ("Sync and latency")** — v1 honors declared
  `latency()` as a fill-lead extension only. The decimation filter's constant group
  delay (now *larger* than the upsample case, because the widened lowpass has longer
  support) is absorbed by the monitor's existing lookahead pre-roll, not expressed
  through `AudioFacet::latency()` (Decision D6, inherited from
  `device_edge_resample.md` D6).

### Design-doc delta shipped with this task

`docs/design/12-audio.md:106–119` (the "Working format" paragraph): replace the
"downsampling is deferred / rejected" sentences (lines 111–116) with the shipped
behavior — a device rate *below* the working rate is decimated at the edge with a
**ratio-scaled widened lowpass cut at the device Nyquist** over the same windowed-sinc
prototype (both rate directions now handled; only `device_rate == working_rate`
takes the 1:1 drain). Per doc 16 the delta rides in the closer's commit. This closes
a stated deferral by implementing the already-designed extension — it is neither a
new seam nor a deviation from designed behavior — so, symmetric with the
predecessor's upsample delta, **no doc-00 decision-record bullet** is added.

### Code seams the implementation extends

- **`src/media/audio_resampler.cpp:13–44`** — the FROZEN 16-tap / 32-phase
  Blackman-Harris table `k_resampler_coeffs` (`k_resampler_taps = 16`,
  `k_resampler_phases = 32`) and its deliberate-regeneration discipline
  (regen procedure lines 29–39). The frozen table is the *input-Nyquist* filter;
  it is **retained verbatim** for the upsample/matched paths. Decimation cannot use
  it: a continuous decimation ratio has no single pre-freezable table, and the
  ratio-scaled cutoff needs a wider support than 16 taps.
- **`src/media/audio_resampler.cpp:562–612`** — the shared helpers the predecessor
  factored: `FramePos` + `frame_pos` (lines 571–582, the exact-integer phase math:
  `pos_num = n·src_rate`, `center = pos_num/dst_rate`, `phase = (rem·phases +
  dst_rate/2)/dst_rate`, the single rounding at line 576); `mac_frame` (588–601,
  `half = taps/2−1 = 7`, coeff row `&k_resampler_coeffs[phase·taps]`); and
  `support_oldest`/`support_newest` (606–612, currently `center−7` … `center+8`).
  The decimation path **reuses `frame_pos` verbatim** (integer `src_rate:dst_rate`
  ratio math is direction-agnostic — for `src_rate > dst_rate` `center` simply
  strides by more than one input frame per output frame) but needs **widened support
  bounds and a wider MAC** over the generated table.
- **`src/media/audio_resampler.cpp:616–622`** — the no-op guard; the
  `in.rate >= out.rate` clause at line 619 is what excludes decimation today. Relax
  to `in.rate == out.rate` (keep zero-rate / layout-mismatch / null no-ops).
- **`src/media/audio_resampler.cpp:641–714`** — the `StreamingResampler` impl:
  `configure` (641–652; capacity `d_capacity_frames = block_frames + 2·taps` at line
  649 — this `2·taps` residual is the upsample support width and must scale with the
  decimation ratio), `reset` (654–658), `can_produce` (660–667), `produce` (669–684),
  `push_input` (686–714; compaction via `support_oldest`, with a "drop > hist_len
  never in practice (upsampling)" comment at line 699 that a decimation stride can
  make reachable — audit it).
- **`src/media/arbc/media/streaming_resampler.hpp:29–32,40–81`** — the seam's
  public API (`configure`/`reset`/`push_input`/`can_produce`/`produce`) and its
  **"upsampling only … fixed sinc cut at the input Nyquist"** precondition comment
  (29–32), which this task widens; and its state members (69–80: `d_capacity_frames`,
  `d_history`, `d_hist_len`, `d_hist_base`, `d_out_index`).
- **`src/media/arbc/media/audio_resampler.hpp:29–32`** — the `0 < in.rate < out.rate`
  precondition doc on `resample_audio`, widened to cover the decimation direction.
- **`src/runtime/device_monitor.cpp:46–54`** — the rejection guard to remove (its
  message names this task) and `d_resampling` (line 54) to widen from `>` to `!=`.
  Related doc comments to update: constructor doc
  `src/runtime/arbc/runtime/device_monitor.hpp:73–77`, and the `d_resampling` field
  comment `device_monitor.hpp:143`.
- **`src/runtime/device_monitor.cpp:59–61,107–120,270`** — the two flicks-per-frame
  members (`d_flicks_per_frame` device-rate, `d_working_flicks_per_frame`
  working-rate), `start_block_index()` (working-rate drain span), and `master_step`'s
  device-rate clock advance. All **kept unchanged**; they are already direction-agnostic.
- **`src/runtime/device_monitor.cpp:63–69,198–234`** — the construction pre-size
  (`d_resampler.configure` + `d_src_out` sizing) and the `fill_rt` SRC branch
  (`can_produce` at 212, `push_input` 216–218, `produce` 226–230, `convert_frames`
  231). `d_src_out` (`block_frames·channels`) is large enough for decimation (fewer
  outputs), but the pull/push loop's *termination and feed* logic assumes the
  upsample frame-count direction and must be audited (Constraint 4).
- **`src/runtime/device_monitor.cpp:203–207,288–294`** — the flush consume/set
  handoff, engaged automatically once `d_resampling` is true for decimation.

### Existing claims to extend, not duplicate

- `12-audio#device-edge-resamples-working-to-device` (`tests/claims/registry.tsv:160`)
  — the sibling upsample claim; this task's new claim is its decimation mirror and
  cites it as `(extends …)`. Its closing clause "*a device rate below the working
  rate is rejected at construction*" is precisely what this task overturns; the new
  claim records the successor behavior (the sibling claim's historical text is left
  intact as the record of the upsample step).
- `12-audio#device-callback-consumes-prepared-blocks-only` (`registry.tsv:83`) — the
  drain-equals-oracle, worker-count identity claim the resampled edge inherits.
- `12-audio#device-clock-masters-transport` (`registry.tsv:82`) — the
  device-frame → transport-time mapping this task must preserve unchanged.
- `12-audio#device-drain-realigns-on-transport-change` (`registry.tsv:84`) — the peer
  `seek_drain_realign` claim whose realignment now also covers the decimation SRC path.
- `12-audio#nested-boundary-resamples-below-rate-children` (`registry.tsv:72`) — the
  kernel-reuse and byte-exactness discipline the new claim inherits.

## Constraints / requirements

1. **RT-safety (the whole-engine invariant).** `fill_rt` stays allocation-free and
   lock-free: the widened history buffer and all decimation coefficients are
   generated/sized **at construction** (`configure`, off the RT thread), and the RT
   inner loop is ordered no-libm float32 MACs over the resident table. Any
   windowed-sinc / `std::sin` evaluation used to *generate* the widened coefficients
   runs only at construction, never on the callback thread (the Constraint-1 lineage,
   `device_monitor.hpp:30–34`). `audio.rt_safety` will later assert this build-failingly.
2. **Anti-aliased at the device Nyquist.** For `device_rate < working_rate` the
   reconstruction lowpass is cut at the *device* Nyquist (`device_rate/2`), not the
   fixed input Nyquist — content above the device Nyquist is attenuated, not folded
   back as audible-band aliasing. This is the substance the predecessor's D3 deferred;
   a fixed-cutoff reuse would alias and is rejected (Decision D2).
3. **Byte-exact, tolerance-free equivalence between the two implementations.** The
   stateful `StreamingResampler` decimation output is bit-identical to a single
   whole-stream `resample_audio` decimation of the same input — both extended over the
   *same* generated widened-lowpass coefficients, with the same exact-integer phase
   math, no float accumulation, no second rounding, continuous across block/callback
   boundaries (no per-block phase restart). The equivalence golden carries no
   tolerance (doc 16:48–53). (A separate spectral anti-alias assertion is the one
   *justified* tolerance — Constraint 9 / Acceptance.)
4. **Two rate axes, kept distinct; the decimating frame-count direction audited.** The
   lookahead-ring block geometry and drain index stay in **working-rate** frames; the
   clock advance stays in **device-rate** frames (`start_block_index` /
   `d_flicks_per_frame`, unchanged). The `fill_rt` SRC batch loop must correctly
   handle decimation emitting **fewer** device frames per working block than it
   consumes (the inverse of upsampling): the pull loop feeds enough working input
   before `can_produce`, terminates, and the `push_input` compaction bound holds when
   the output stride skips input frames (`audio_resampler.cpp:699` comment).
5. **1:1 and upsample paths preserved.** `device_rate == working_rate` keeps the
   existing memcpy/layout drain (`convert_frames`); `device_rate > working_rate` keeps
   the frozen-table upsample path byte-for-byte. Neither regresses — the decimation
   extension is a new branch, not a mutation of the existing ones. The frozen
   `k_resampler_coeffs` table is unchanged (no regen).
6. **Widened support sizes at construction.** History capacity scales with the
   decimation ratio (`≈ block_frames + 2·ceil(taps·working_rate/device_rate)` or the
   equivalent support-bound derivation) so `can_produce`/`produce`/`push_input` never
   allocate and the first output has full resident support. `support_oldest`/
   `support_newest` (or their decimation analogues) must reflect the widened window.
7. **Levelization (doc 17, CI-enforced).** The decimating lowpass is DSP and stays in
   `arbc::media` (L1) beside `resample_audio` — it depends only on `base` (L0, rational
   rates) and `media`'s own `AudioBlock`. The **device rate is a parameter passed down
   from `runtime` (L5)**, never discovered by the engine querying a device; no OS audio
   API enters `libarbc` / `arbc-testing`
   (`docs/design/17-internal-components.md`, "The codec line" / device-backend policy).
8. **Seek/rate-change reuses the shipped flush + realign.** No new flush machinery: the
   atomic-flag flush (`device_monitor.cpp:203–207,288–294`) and the peer
   `seek_drain_realign` drain-cursor realignment engage for decimation once
   `d_resampling` is true. This task extends their goldens to a decimation ratio; it
   adds no new transport-change seam.
9. **Diff coverage ≥ 90%** on changed lines (doc 16 CI gate); tests ship in this task.

## Acceptance criteria

- **Claims-register growth.** Add
  `12-audio#device-edge-decimates-working-to-device` to `tests/claims/registry.tsv`,
  invariant:
  *"A DeviceMonitor whose device rate is below the working rate decimates the drained
  working-rate mix to the device rate with the same polyphase machinery extended by a
  ratio-scaled widened lowpass cut at the device Nyquist (the same windowed-sinc
  prototype and exact-integer phase math, coefficients generated off the RT thread at
  construction, no second algorithm): the anti-alias lowpass suppresses content above
  the device Nyquist rather than folding it into the audible band; the device-delivered
  bytes equal a single whole-stream `resample_audio` decimation of the `mix_composition`
  oracle over the same window, byte-exact with no tolerance and continuous across
  block/callback boundaries (no per-block phase restart), byte-identical between
  worker_count == 0 and worker_count > 0; the mastered clock still advances by
  delivered_device_frames / device_rate; a seek/rate change flushes the resampler and
  realigns the drain so post-flush output restarts byte-exact; the matched-rate 1:1
  drain and the above-working upsample path are unchanged (extends
  12-audio#device-edge-resamples-working-to-device)."* Enforced by a Catch2 block tagged
  `// enforces: 12-audio#device-edge-decimates-working-to-device`.
- **Byte-exact equivalence goldens** (no tolerance, doc 16), driving `org.arbc.tone`
  through a fake `DeviceSink` whose `format()` reports a device rate *below* the working
  rate, asserting the drained device bytes equal a whole-stream `resample_audio`
  decimation of the working-rate `mix_composition` oracle (reusing
  `device_monitor.t.cpp`'s `whole_stream_resample` helper, lines 283–293, and
  `drive_device`, 299–365 — the oracle extended for the decimation direction):
  - an **integer ratio** (48 kHz working → 24 kHz device, 2:1), and
  - the **flagship coprime rational ratio** (48 kHz working → 44.1 kHz device = 160:147)
    to exercise the phase accumulator and widened history carry;
  - a **continuity golden**: many successive `fill_rt` chunks concatenated equal one
    whole-stream decimation of the concatenated working mix (pins Constraint 3/4 — no
    boundary click);
  - each asserted **byte-identical between `worker_count == 0` and `worker_count > 0`**,
    living beside the upsample goldens (`device_monitor.t.cpp:769–805`).
- **Anti-alias spectral assertion** (the single *justified* tolerance, doc 16 — a DSP
  stopband property, not a deterministic-render golden): drive a tone above the device
  Nyquist through a decimating monitor and assert the aliased-band energy is suppressed
  by a generous margin (e.g. ≥ 60 dB below a control tone in-band), i.e. a **fixed-cutoff
  (un-widened) decimation would fail this** — the non-degenerate golden that
  distinguishes correct anti-aliasing from a naive stride. Justify the tolerance in the
  test comment (stopband suppression is inherently a threshold, not a byte match).
- **Behavioral-counter assertions** (never wall-clock, doc 16), on the deterministic
  `flush_master()` barrier (`device_monitor.hpp:105`):
  - **matched-rate no-regression** — `device_rate == working_rate` still drains
    byte-identical to the 1:1 oracle with zero resampler engagement (Constraint 5);
  - **upsample no-regression** — an above-working device rate still matches the frozen
    upsample golden (Constraint 5);
  - **flush + realign restarts byte-exact** — after a `seek`/`set_rate` on a decimating
    monitor, the first post-flush output block byte-matches a fresh whole-stream
    decimation (Constraint 8; both flush and drain-realign now ship, so full post-seek
    byte-exactness is asserted, unlike the predecessor).
- **Rejection → acceptance.** The predecessor's downsample-rejection test
  (`device_monitor.t.cpp:807–879`, the `REQUIRE_THROWS_AS(std::invalid_argument)` block
  at 833–878 constructing a 44.1 kHz sink under a 48 kHz working rate) is **removed and
  replaced** by a decimation golden for the same config; its stale "closing the
  previously-untested guard" comment (833–834) is deleted. The construction path for a
  below-working device rate now succeeds and drains anti-aliased.
- **Concurrency / TSan.** Extend `tests/device_monitor_concurrency.t.cpp` with a
  decimation config (mirroring the upsample TSan case at lines 400–507, e.g. working
  96 kHz → device 48 kHz, workers > 0, a seed-perturbed seek storm) so the widened
  streaming resampler runs on the RT callback concurrently with the master thread;
  assert clean under TSan, `drain_realigns > 0`, and `tasks_completed ==
  tasks_submitted`. The decimation state is RT-thread-single-owner like `d_scratch` —
  the assertion is that it adds no new shared mutable state.
- **No new conformance family.** This extends the audio engine's device path; it lands
  no content kind or operator, so the contract conformance suite is untouched.
- **WBS gate.** After the closer's `.tji` edits, `tj3 project.tjp 2>&1 |
  grep -iE "error|warning"` is silent.
- **Registers no successor.** Both device-edge rate directions are handled after this
  lands; this is the terminal leaf of the device-edge SRC chain. No re-audit / revisit
  leaf is spawned.

## Decisions

- **D1 — The decimating lowpass lives in the `arbc::media` (L1) kernel, over the same
  streaming seam.** The extension is added to `resample_audio` (oracle) and
  `StreamingResampler` (device path), reusing `frame_pos`'s exact-integer phase math and
  the streaming cursor/history seam; the device rate flows down from `runtime` (L5) as a
  parameter. Authority: doc 12:106–116 ("the working → device edge reuses the same
  band-limited polyphase resampler"), doc 17 device-backend policy, and the
  `audio.mix_engine` precedent of extending this one kernel with a new rate direction
  (`tasks/refinements/audio/mix_engine.md`). *Rejected:* a separate decimator owned by
  the monitor/plugin — duplicates the phase math and risks the OS-audio-free `libarbc`
  boundary; decimation is engine-level DSP, the device rate is runtime policy.

- **D2 — Anti-alias with a ratio-scaled widened lowpass cut at the device Nyquist —
  the substance D3 deferred.** For `src_rate > dst_rate` the lowpass cutoff is
  `dst_rate/2` (the device Nyquist), so the impulse response widens by the decimation
  ratio and the tap support scales with it. Authority: doc 12:112–113 (downsampling
  "needs the reconstruction filter cut at the lower device Nyquist to stay
  anti-aliased"), `device_edge_resample.md` D3. *Rejected:* reusing the fixed
  input-Nyquist frozen table for decimation — it does not band-limit below the input
  Nyquist, so it folds above-device-Nyquist content into the audible band (aliasing);
  the anti-alias spectral golden pins that this is wrong. *Rejected:* nearest/hold or
  drop-sample decimation — decisively aliases, the same failure the nested-boundary
  claim (`registry.tsv:72`) forbids for reconstruction.

- **D3 — Generate the widened coefficients off the RT thread over the same prototype,
  rather than checking in a table.** A decimation ratio is continuous — no single table
  can be pre-frozen for every device rate — so the widened-lowpass coefficients are
  generated at `configure()` time (construction / device attach, not the callback)
  using the *same* Blackman-Harris windowed-sinc generator and 32-phase bank that
  produced the frozen upsampling table (`audio_resampler.cpp:29–39` regen procedure),
  parameterized by cutoff = device Nyquist and a ratio-scaled tap count. The RT inner
  loop MACs over the resident generated table, no-libm. Authority: Constraint 1
  (RT-safety is a *callback* property; construction may compute), doc 12:127–130
  (exact-rational discipline — generation is a deterministic function of the rational
  `src:dst` ratio). *Rejected:* generating on the RT callback — a Constraint-1 violation
  (libm + potential alloc on the audio thread). *Rejected:* pre-freezing tables for a
  blessed set of ratios and rejecting others — abandons arbitrary device rates, the
  case this task exists to unblock; and the frozen upsampling table is retained
  precisely because its ratio-independent (input-Nyquist) cutoff *can* be pre-frozen,
  which the ratio-dependent decimation cutoff cannot.

- **D4 — Byte-exactness is enforced as streaming-vs-whole-stream equivalence, not
  against a checked-in absolute table.** Both `resample_audio` and `StreamingResampler`
  decimation share the same generated coefficients, so the equivalence golden is
  bit-exact on any single platform regardless of the generator's libm; anti-aliasing is
  pinned separately by a spectral threshold (the one justified tolerance). Authority:
  doc 16:48–53 (byte-exact goldens; tolerances the justified exception), the
  predecessor's oracle-relative golden structure (`device_edge_resample.md:246–260`).
  *Rejected:* a checked-in absolute-sample decimation golden — it would pin the
  generator's libm output cross-platform (which is not guaranteed bit-reproducible) and
  add no assurance the equivalence + spectral goldens don't already give. (The
  cross-platform reproducibility of the *device* decimation path is not asserted by any
  current claim and is surfaced for the parking lot, not encoded as a WBS task.)

- **D5 — Pre-size the widened state at construction; the RT drain stays allocation-free.**
  History capacity and coefficient storage size from `block_frames` and the decimation
  ratio in `configure`, beside the existing pre-sized `d_src_out`/`d_scratch`
  (`device_monitor.cpp:63–69`, `device_monitor.hpp:152–153`). *Rejected:* per-callback
  sizing/allocation — a Constraint-1 (RT-safety) violation.

- **D6 — Reuse the shipped flush + drain-realign; no new transport-change seam.** The
  atomic-flag flush (`device_monitor.cpp:203–207,288–294`) and the peer
  `seek_drain_realign` cursor realignment already cover the SRC branch; widening
  `d_resampling` to `!=` engages them for decimation automatically. The widened
  filter's larger group delay is absorbed by the existing lookahead pre-roll, not a
  facet `latency()` (inherited from `device_edge_resample.md` D6). *Rejected:* adding a
  decimation-specific flush or plumbing the edge delay through `AudioFacet::latency()` —
  duplicates shipped machinery and mis-uses a mechanism reserved for in-graph
  contributors.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-08.

- Added ratio-scaled widened-lowpass decimation bank to `src/media/audio_resampler.cpp`; `resample_audio` and `StreamingResampler` both extended via the shared Blackman-Harris windowed-sinc generator, coefficients generated at `configure()` off the RT thread.
- Updated `src/media/arbc/media/audio_resampler.hpp` and `src/media/arbc/media/streaming_resampler.hpp` to widen precondition docs and expose the new `src>dst` direction.
- Removed below-working construction rejection from `src/runtime/device_monitor.cpp`; widened `d_resampling` guard from `>` to `!=`; audited `fill_rt` frame-count-direction comments for decimation.
- Updated `src/runtime/arbc/runtime/device_monitor.hpp` to reflect the widened constructor contract.
- Fixed `src/runtime/lookahead_pump.cpp` / `src/runtime/arbc/runtime/lookahead_pump.hpp`: `request_stop()` now joins the loop thread before returning, eliminating a pre-existing post-stop submit race surfaced by ASan timing.
- Added claim `12-audio#device-edge-decimates-working-to-device` to `tests/claims/registry.tsv`.
- Updated `docs/design/12-audio.md`: deferral paragraph replaced with shipped decimation behavior.
- New tests: streaming-vs-whole-stream unit (2:1 + 3:2) in `src/media/t/streaming_resampler.t.cpp`; byte-exact decimation goldens (2:1, 160:147, continuity, both worker counts), rejection→golden replacement, anti-alias spectral assertion (≥60 dB), seek + set_rate post-realign in `src/runtime/t/device_monitor.t.cpp`; TSan decimation concurrency (96k→48k) in `tests/device_monitor_concurrency.t.cpp`.
