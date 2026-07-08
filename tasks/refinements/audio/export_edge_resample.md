# audio.export_edge_resample — Export-edge sample-rate conversion

## TaskJuggler entry

Back-link: [`tasks/45-audio.tji`](../../45-audio.tji) — `task export_edge_resample`
(lines 68–73).

> "Convert working-rate export blocks to a caller-requested container output rate
> at the export edge, reusing the shipped StreamingResampler (which already ships
> up-sampling and the ratio-scaled widened-lowpass decimation), so exports can
> target 44.1/96 kHz container rates byte-exactly and continuously across block
> boundaries; a matched output rate keeps the 1:1 pass unchanged. Source:
> audio.export_monitor scope boundary; see
> tasks/refinements/audio/export_monitor.md. Doc 12."

## Effort estimate

`effort 1d` (`tasks/45-audio.tji:69`). Both halves of the machinery already ship:
the offline mix producer (`ExportMonitor::render_range` tiling working-rate blocks
against one pinned revision, `src/runtime/export_monitor.cpp:145–168`) and the
band-limited streaming resampler for **both** rate directions
(`arbc::media::StreamingResampler` — up-sampling from `audio.device_edge_resample`,
ratio-scaled widened-lowpass decimation from `audio.device_edge_decimation`,
`src/media/arbc/media/streaming_resampler.hpp:44–73`). The genuinely new work is
one integration seam — configure/feed/drain the resampler between the working-rate
mix and the export sink, plus a finite end-of-range tail drain — with no realtime
pressure (export is offline, unlike the device edge). One focused day of wiring and
goldens against shipped seams.

## Inherited dependencies

- **`audio.export_monitor`** — ***settled*** (Done 2026-07-08,
  `tasks/refinements/audio/export_monitor.md` Status block; git `15f68ba`). It
  shipped the producer this task sits downstream of: `arbc::ExportMonitor` (L5
  `arbc::runtime`, `src/runtime/arbc/runtime/export_monitor.hpp:67`), the
  `block_windows_over` half-open working-rate tiler (`export_monitor.hpp:60–62`),
  the per-block mix `render_block_at` (`export_monitor.hpp:103`) and the
  convenience loop `render_range` (`export_monitor.hpp:111`), plus the `BlockSink`
  callback contract (`export_monitor.hpp:73`). It also **named this task as the
  explicit deferral** it left open: "The mix is produced at the composition working
  rate; converting to a different container output rate is the same shared working
  → edge resampler the device monitor uses, deferred to `audio.export_edge_resample`"
  (`export_monitor.hpp:42–44`; refinement "Out of scope", `export_monitor.md`).
  Its decision to route output-rate conversion through the shipped edge resampler —
  rather than requesting `mix_composition` at a non-working `sample_rate` — is this
  task's charter (`export_monitor.md` Decisions: a contributor native *above* the
  requested rate would hit `mix_composition`'s decimation path "whose faithfulness
  is a mix-engine property not established for arbitrary sub-working request rates").
- **`audio.device_edge_decimation`** — ***settled*** (Done 2026-07-08,
  `tasks/refinements/audio/device_edge_decimation.md` Status block; git `b007d78`).
  Together with its predecessor `audio.device_edge_resample` (git `d6851fa`) it
  shipped and hardened the exact seam this task reuses: `StreamingResampler`'s
  `configure`/`reset`/`push_input`/`can_produce`/`produce` API
  (`streaming_resampler.hpp:52,59,65,69,73`), the rational input cursor +
  allocation-free filter-support history carry that makes streaming output
  **byte-identical to a single whole-stream `resample_audio`** across chunk
  boundaries, and **both rate directions** — up-sampling through the frozen
  input-Nyquist table and decimation through the ratio-scaled widened lowpass cut
  at the lower Nyquist. Its device-edge wiring (`src/runtime/device_monitor.cpp:51,
  63,221–240`: engage-on-mismatch, configure-once, feed-`push_input`,
  drain-`produce`, `convert_frames`) is the reference pattern this task replicates
  minus the RT/atomic-flush machinery.
- **`kinds.nested_audio_resampling`** — ***settled*** (git `4cd7faa`). Shipped the
  frozen 16-tap / 32-phase Blackman-Harris polyphase kernel `resample_audio`
  (`src/media/arbc/media/audio_resampler.hpp`) and its exact-integer phase math, the
  stateless whole-stream oracle every equivalence golden compares against (claim
  `12-audio#nested-boundary-resamples-below-rate-children`, `tests/claims/registry.tsv:72`).

All dependencies are settled; nothing this task needs is pending. Because both edge
directions already ship, this task lands the export edge for up-sampling **and**
decimation in one step — there is no successor rate-direction leaf (unlike the
device-edge chain, which split across two tasks for RT-hardening reasons).

## What this task is

Realize the working → container-output-rate conversion at the **export edge**: the
last named deferral `audio.export_monitor` left open. The export monitor mixes
every block at the composition working rate; a host exporting to a container whose
rate differs from the working rate (a 48 kHz working composition written to a
44.1 kHz or 96 kHz file) needs those working-rate blocks resampled to the container
rate before muxing. Three deliverables:

1. **An export-edge resampler stage in `arbc::runtime`, reusing the L1 media
   `StreamingResampler`.** Add an owned `StreamingResampler` member to
   `ExportMonitor` and a render entry point that takes a caller-requested container
   output rate. When the output rate differs from the working rate, the stage
   configures the resampler `working_rate → output_rate` once at the start of the
   range, feeds each working-rate mixed block via `push_input`, drains `produce`
   into container-rate output blocks tiled at `block_frames`, and hands those
   container-rate `(window, block, result)` tuples to the `BlockSink`. Both rate
   directions come for free — up-sampling and the ratio-scaled widened-lowpass
   decimation are already selected internally by `StreamingResampler::configure`
   from the rate ordering, so 96 kHz (upsample) and 44.1 kHz (decimate, 160:147)
   containers both work with no new DSP.
2. **A finite end-of-range tail drain.** Unlike the device edge (an unbounded
   stream), an export covers a finite range and must emit exactly the whole-stream
   output-frame count. After the last working block is fed, the stage drains the
   resampler to the container-rate frame count that covers the range
   (`block_windows_over(range, output_rate, …)` sample total), replicating the
   whole-stream oracle's implicit end-of-input edge behavior so the concatenated
   export output is byte-exact against a single `resample_audio` of the whole-range
   working mix (Decision D3).
3. **The matched-rate 1:1 pass is untouched.** When the requested output rate
   equals the working rate, the existing `render_range`/`render_block_at` path runs
   verbatim — no resampler configured, no SRC cost, working-rate blocks handed
   straight to the sink — so every shipped `audio.export_monitor` golden and claim
   is preserved byte-for-byte.

**Out of scope.** *Container **layout** conversion at the export edge* (e.g. a
stereo working composition written to a mono container) — this task converts sample
rate only, keeping the export's working layout; the device edge's `convert_frames`
layout remix is not wired here (surfaced for the parking lot, not a WBS leaf — no
milestone consumes it and an unwired leaf would orphan). *Muxing / encoding / file
writing* stays host territory (`export_monitor.hpp:69–72`; the engine writes no
files, links no codec). *Time-stretch / pitch-preserving* SRC stays deferred with
the effects stack (doc 12:127–133). This task registers **no successor** — both edge
rate directions land here and the working-rate producer already ships.

## Why it needs to be done

`audio.export_monitor` produces a sample-exact mix but only at the composition
working rate (`export_monitor.cpp:140,148,164` all stamp `d_format.sample_rate`);
it has no container-output-rate parameter and no conversion stage — the deferral is
stated in source (`export_monitor.hpp:42–44`) and in the refinement's registered
successor. Until this lands, a host targeting the two most common consumer/delivery
container rates — 44.1 kHz (CD lineage, most consumer/streaming delivery) and
96 kHz (high-res masters) — from a 48 kHz working composition must either pre-lower
the composition working rate or run its own resampler, duplicating the exact
band-limited machinery `libarbc` already ships and forfeiting the byte-exact
guarantee. The M6 audio milestone (`m6_audio`, `tasks/99-milestones.tji:51`) depends
on this leaf directly; doc 12 already promises the behavior as "the same shared
working → edge resampler the device monitor uses (a separate export-edge step), not
a second path" (doc 12:190–194) — this task turns that standing promise into shipped,
tested code and closes the export monitor's last scope boundary.

## Inputs / context

### Governing design doc — doc 12 (normative, doc 16)

- **`docs/design/12-audio.md:187–194` ("Export monitor" bullet)** — the load-bearing
  promise this task realizes: "The mix is produced at the composition working rate;
  converting to a different container output rate is the same shared working → edge
  resampler the device monitor uses (a separate export-edge step), not a second
  path." This already governs the behavior affirmatively (it is a standing promise,
  not a deferral/rejection marker), so this task closes the named deferral **in
  place** with no doc rewrite (see "Design-doc delta" below).
- **`docs/design/12-audio.md:94–104` ("Working format")** — the edge-conversion
  contract: "Conversions live at the edges exactly as in doc 07 … the monitor
  converts working → device." The export edge is the offline analog of the
  working → device edge, obeying the same "conversions live at the edges" discipline.
- **`docs/design/12-audio.md:106–121` (the shared band-limited polyphase resampler)**
  — "one shipped windowed-sinc kernel, not a second algorithm … A device whose rate
  equals the working rate keeps a byte-for-byte 1:1 drain (no SRC cost)." The export
  edge reuses this single resampler; the **matched-rate 1:1 pass** (Constraint 5) is
  normatively grounded here (line 109–110, 117–118).
- **`docs/design/12-audio.md:127–130` ("Varispeed" / exact rationals)** — the
  resample-through-an-exact-rational-ratio discipline the export edge inherits:
  `working_rate : output_rate` is an exact rational (48000:44100 = 160:147), the
  same phase math the streaming resampler already uses.

### Design-doc delta shipped with this task

**None required.** Unlike `audio.device_edge_decimation`, whose doc-12 paragraph
carried an explicit "downsampling is deferred / rejected" sentence to overturn, the
export-edge behavior is already stated affirmatively at doc 12:190–194 ("the same
shared working → edge resampler … not a second path") and at doc 12:106–108 (one
shared resampler across every edge). This task implements already-designed behavior —
no new seam, no deviation — so, symmetric with how the sibling closed its deferral,
**no doc-00 decision-record bullet** is added and no normative doc text changes. The
only text update is a source comment: `export_monitor.hpp:42–44`'s "deferred to
`audio.export_edge_resample`" becomes a statement of the shipped stage (an
implementation edit, not a design delta).

### Code seams the implementation extends

- **`src/runtime/arbc/runtime/export_monitor.hpp:67`** — `class ExportMonitor` (L5
  `arbc::runtime`, non-copyable/non-movable). Gains an owned `StreamingResampler`
  member (mirroring `device_monitor.hpp:158` `StreamingResampler d_resampler;`) and
  a container-output-rate render entry point.
- **`src/runtime/arbc/runtime/export_monitor.hpp:73`** — `using BlockSink =
  std::function<void(TimeRange, const AudioBlock&, AudioResult)>`. Unchanged: the
  resampled path emits container-rate `AudioBlock`s through the same sink type
  (block `rate` field stamped to the output rate, `audio_block.hpp:46`).
- **`src/runtime/arbc/runtime/export_monitor.hpp:103` / `export_monitor.cpp:128–143`**
  — `render_block_at`, the pure working-rate per-block mix (`Exactness::Exact`
  request at `d_format.sample_rate`). **Kept byte-for-byte** — the resampler stage
  sits downstream of it, never inside it, so the shipped mix goldens don't regress.
- **`src/runtime/arbc/runtime/export_monitor.hpp:111` / `export_monitor.cpp:145–168`**
  — `render_range`, the convenience loop tiling working-rate windows via
  `block_windows_over` and handing each mixed block to the sink (seam at `cpp:158–167`).
  The new resampled path is a sibling entry point that reuses this producer loop
  internally to pull working blocks, feeding the resampler instead of the sink.
- **`src/runtime/arbc/runtime/export_monitor.hpp:60–62`** — `block_windows_over(range,
  rate, block_frames)`. Reused twice: at the working rate to size the input pull, and
  at the container output rate to compute the exact output-frame count the tail drain
  must reach (Deliverable 2).
- **`src/media/arbc/media/streaming_resampler.hpp:44–73`** — the reused seam:
  `configure(src_rate, dst_rate, channels, block_frames)` (line 52, selects
  up-sample vs widened-lowpass decimation from the rate ordering, generates the
  decimation bank off the hot path, sizes history — resets all state), `reset()`
  (line 59), `push_input(samples, frames)` (line 65), `can_produce()` (line 69),
  `produce(out_frame)` (line 73). Configured `working_rate → output_rate` once per
  render call; **no media-side change** — both directions already ship.
- **`src/media/arbc/media/audio_resampler.hpp` — `resample_audio(const AudioBlock&
  in, AudioBlock& out)`** (impl `src/media/audio_resampler.cpp:700`), the stateless
  whole-stream oracle the byte-exact equivalence goldens compare against (the same
  oracle role it plays for the device-edge goldens).
- **`src/media/arbc/media/audio_block.hpp:42`** — `struct AudioBlock` (`samples`,
  `frames`, `layout` at :45, `rate` at :46). Output blocks carry the container
  `rate`; `channel_count(layout)` (`audio_block.hpp:21`) sizes the interleaved buffer.
- **Reference wiring** — `src/runtime/device_monitor.cpp:51,63,221–240`: the
  configure-once / feed-`push_input` / drain-`produce` pattern (minus the RT
  `d_resampler_flush` atomic and `convert_frames` layout remix, both out of scope
  for the offline, single-layout export path).

### Existing claims to extend, not duplicate

- `12-audio#device-edge-resamples-working-to-device` (`tests/claims/registry.tsv:160`)
  and `12-audio#device-edge-decimates-working-to-device` — the two device-edge claims
  proving `StreamingResampler` is byte-exact against the whole-stream oracle in each
  rate direction. The export-edge claim reuses the same seam offline and cites both as
  `(extends …)`; it does not re-prove the resampler's DSP, only its export-edge wiring.
- `12-audio#nested-boundary-resamples-below-rate-children` (`registry.tsv:72`) — the
  kernel-reuse / byte-exactness discipline the equivalence golden inherits.
- `12-audio#export-monitor-mixes-exactly-over-range` and
  `12-audio#export-monitor-pins-single-revision` (added by `audio.export_monitor`) —
  the working-rate mix producer this edge sits downstream of; **preserved unchanged**
  (the matched-rate 1:1 pass keeps them byte-exact).

## Constraints / requirements

1. **Byte-exact, tolerance-free equivalence to the whole-stream oracle.** The
   concatenated container-rate export output equals a single `resample_audio` of the
   whole-range working mix at the output rate — bit-identical, no tolerance, no float
   accumulation drift, continuous across export block boundaries (no per-block phase
   restart; the streaming cursor + history carry guarantee this, proven by the
   device-edge streaming-vs-whole-stream unit tests). Goldens carry no tolerance
   (doc 16:48–53).
2. **The matched-rate 1:1 pass is untouched.** `output_rate == working_rate` runs the
   existing `render_range` verbatim: no `StreamingResampler` configured, zero SRC
   work, working-rate blocks stamped and handed straight to the sink. No shipped
   `audio.export_monitor` golden, claim, or counter regresses.
3. **Both rate directions, from the shipped resampler only.** Up-sampling
   (`output_rate > working_rate`, e.g. 96 kHz) uses the frozen input-Nyquist table;
   decimation (`output_rate < working_rate`, e.g. 44.1 kHz) uses the ratio-scaled
   widened lowpass cut at the output Nyquist. Both are selected internally by
   `configure` from the rate ordering — this task adds **no DSP**, only wiring, and
   must not touch `src/media` (Decision D1).
4. **Finite tail produces the exact whole-stream frame count.** The export range is
   finite; the stage drains exactly the container-rate frame count covering the range
   (computed via `block_windows_over` at `output_rate`), replicating the oracle's
   end-of-input edge behavior so the last output block is byte-exact — no truncated
   nor over-produced tail (Deliverable 2 / Decision D3).
5. **Configure off the render hot path; state sized once per range.** `configure` (and
   its decimation-bank generation / history sizing) runs once at the start of a render
   call, `reset` between independent ranges — never per block. Export is offline so
   there is no `ARBC_RT_NONBLOCKING` callback constraint, but the configure-once /
   feed / drain discipline still holds (it is what keeps streaming byte-identical to
   whole-stream).
6. **Single revision pin preserved.** The resampler stage adds no second document read;
   every working block still mixes against the one `DocRoot::revision()` pinned at
   construction (`export_monitor.hpp:75–84`), so
   `12-audio#export-monitor-pins-single-revision` still holds.
7. **Levelization (doc 17, CI-enforced).** The `StreamingResampler` stays in
   `arbc::media` (L1); `ExportMonitor` stays in `arbc::runtime` (L5). The edge is an
   L5 → L1 downward call (allowed) reusing a dependency `arbc::runtime` already has
   (`export_monitor` includes `arbc/media` for `AudioBlock`). **No new `DEPENDS`
   edge.** The output rate is a runtime-supplied parameter, never discovered by the
   engine; no OS-audio API enters `libarbc` (there is none for an export edge —
   cleaner than the device path, which needs a plugin split).
8. **Diff coverage ≥ 90%** on changed lines (doc 16 CI gate); tests ship in this task.

## Acceptance criteria

- **Claims-register growth.** Add `12-audio#export-edge-resamples-working-to-container`
  to `tests/claims/registry.tsv`, invariant:
  *"An ExportMonitor asked for a container output rate different from the composition
  working rate converts the working-rate mix to that output rate at the export edge
  with the same shipped band-limited polyphase `StreamingResampler` the device edge
  uses (up-sampling through the frozen input-Nyquist table for output_rate >
  working_rate; the ratio-scaled widened lowpass cut at the output Nyquist for
  output_rate < working_rate; no second algorithm): the concatenated container-rate
  export output is byte-exact against a single whole-stream `resample_audio` of the
  whole-range working mix over the same range, with no tolerance and continuous across
  export block boundaries (no per-block phase restart); every block still mixes
  against the one pinned revision; a requested output rate equal to the working rate
  keeps the existing 1:1 export path unchanged with zero resampler engagement (extends
  12-audio#device-edge-resamples-working-to-device,
  12-audio#device-edge-decimates-working-to-device)."* Enforced by a Catch2 block
  tagged `// enforces: 12-audio#export-edge-resamples-working-to-container`.
- **Byte-exact equivalence goldens** (no tolerance, doc 16), in
  `tests/audio_export_goldens.t.cpp`, driving `org.arbc.tone` through a real
  `PullServiceImpl` (reusing the shipped export golden harness and the `bytes_equal`
  memcmp + `parab_sine` integer-flick tone oracle, never `std::sin`), asserting the
  concatenated container-rate export bytes equal a whole-stream `resample_audio` of
  the concatenated working-rate `mix_composition` output:
  - a **decimation** container (48 kHz working → 44.1 kHz output = coprime 160:147,
    exercising the phase accumulator and widened history carry — the flagship case),
  - an **up-sample** container (48 kHz working → 96 kHz output = 1:2), and
  - a **continuity golden**: many successive export blocks concatenated equal one
    whole-stream conversion of the concatenated working mix (pins Constraint 1/4 — no
    boundary click, correct finite tail).
- **Behavioral-counter assertion** (never wall-clock, doc 16:54–62): a **matched-rate
  no-regression** case — `output_rate == working_rate` drains byte-identical to the
  shipped 1:1 export golden **with zero resampler engagement** (the `StreamingResampler`
  is never configured; the 1:1 pass is the behavioral-counter promise, the audio
  analog of "a fade at envelope=1 issues zero operator renders"), asserted in
  `src/runtime/t/export_monitor.t.cpp` beside the shipped export counters.
- **Component unit tests** in `src/runtime/t/export_monitor.t.cpp`: the resampled
  render entry point vs the whole-stream oracle for both directions, block-boundary
  invariance (output independent of `block_frames`), and faults-as-values (a zero /
  degenerate output rate drives the sink zero times, mirroring the shipped
  `block_windows_over` degenerate-rate contract, `export_monitor.hpp:56–58`).
- **No new conformance family.** This extends the export path; it lands no content
  kind or operator, so the contract conformance suite is untouched.
- **No TSan addition required.** The export path is single-threaded offline over one
  pinned revision (the resampler state is driver-local, no new shared mutable state);
  the shipped `audio.export_monitor` single-revision-pin TSan case already covers the
  concurrency surface and is unchanged.
- **WBS gate.** After the closer's `.tji` edits, `tj3 project.tjp 2>&1 |
  grep -iE "error|warning"` is silent.
- **Registers no successor.** Both edge rate directions land here; container **layout**
  conversion is surfaced for the parking lot (no milestone consumer), not spawned as a
  WBS leaf. No re-audit / revisit leaf is created.

## Decisions

- **D1 — Reuse the L1 media `StreamingResampler`; add only runtime wiring.** The
  conversion runs through the already-shipped `arbc::media::StreamingResampler`
  (`streaming_resampler.hpp:44`), which already selects up-sample vs widened-lowpass
  decimation internally; this task touches only `arbc::runtime` (`ExportMonitor`).
  Authority: doc 12:190–194 ("the same shared working → edge resampler … not a second
  path"), doc 12:106–108 ("one shipped windowed-sinc kernel, not a second algorithm"),
  the device-edge precedent (`device_edge_resample.md`, `device_edge_decimation.md`),
  doc 17 levelization (L5 → L1 downward). *Rejected:* a second export-only resampler —
  duplicates the phase math and violates the "one kernel" mandate. *Rejected:* getting
  the output rate "for free" by requesting `mix_composition` at a non-working
  `sample_rate` — this is exactly the alternative `audio.export_monitor` weighed and
  rejected: a contributor native *above* the requested rate would hit `mix_composition`'s
  decimation path, "whose faithfulness is a mix-engine property not established for
  arbitrary sub-working request rates." Route conversion through the band-limited edge
  resampler instead, keeping the mix at the honest working rate.

- **D2 — Convert at the export edge downstream of the pure working-rate mix, not inside
  it.** `render_block_at` stays byte-for-byte the shipped Exact working-rate mix; the
  resampler is a stage the resampled render entry point pulls those blocks through.
  Authority: doc 12:100 ("conversions live at the edges"), Constraint 2 (no regression
  to shipped export goldens/claims), the device-edge structure (SRC downstream of the
  pull graph, `device_monitor.cpp:221–240`). *Rejected:* mutating `render_block_at` /
  `render_range` to emit container-rate blocks directly — regresses every shipped
  working-rate golden and conflates the mix and the edge, which the doc keeps separate.

- **D3 — Own the finite tail drain in the runtime driver, no new media API.** The
  export range is finite, so after feeding the last working block the driver drains the
  resampler to the exact container-rate frame count covering the range (via
  `block_windows_over` at `output_rate`), replicating the whole-stream oracle's
  implicit end-of-input edge behavior (the same finite equivalence the device-edge
  streaming-vs-whole-stream unit tests already prove); the byte-exact equivalence
  golden pins that the tail matches. Authority: doc 16:48–53 (byte-exact goldens), the
  device-edge streaming-vs-whole-stream equivalence contract
  (`streaming_resampler.t.cpp`). *Rejected:* adding a `finalize()`/tail-flush method to
  `StreamingResampler` — the finite-input equivalence is already a proven property of
  the shipped seam, so the tail is a runtime driving concern (feed the whole finite
  input, drain the whole-stream output count); no media change is warranted.

- **D4 — Container output rate is a per-render parameter; the matched-rate path is the
  untouched default.** The output rate enters through the resampled render entry point
  (defaulting to / equal to the working rate → the shipped 1:1 `render_range`), not a
  new construction-pinned field, so an `ExportMonitor` can serve matched and converted
  ranges and the 1:1 pass needs no resampler at all. Authority: doc 12:109–110,117–118
  (only a matched rate takes the 1:1 drain), Constraint 2. *Rejected:* forcing the
  output rate at construction — couples an output-side concern to the working-format
  pin and would configure a resampler even for the common matched-rate export.

- **D5 — Convert sample rate only; container layout conversion is parked, not deferred
  to a WBS leaf.** The export edge keeps the export's working channel layout; a
  stereo→mono (or up-mix) container remix is not wired here. Authority: task title
  ("Export-edge **sample-rate** conversion"), scope of the source deferral
  (`export_monitor.hpp:42–44` names container **rate**). *Rejected:* wiring
  `convert_frames` layout remix now — no milestone consumes a container layout differing
  from the working layout, so a WBS leaf would orphan (flagged by
  `scripts/unblocked.py`); surfaced for the parking lot instead, per the rule that a
  deferred follow-up becomes a WBS task only when it has a real consumer.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-08.

- Added `StreamingResampler` owned member and `render_range_to(range, output_rate, block_frames, sink)` declaration to `src/runtime/arbc/runtime/export_monitor.hpp`; updated stale deferral comment at line 42–44 to the shipped stage.
- Implemented `render_range_to` in `src/runtime/export_monitor.cpp`: matched-rate 1:1 delegation to `render_range`; configure-once working→output SRC; feed working blocks + finite zero-pad tail drain to the exact container-frame count; honest min/conjunction `AudioResult` fold; degenerate rate → sink zero times.
- Added claim `12-audio#export-edge-resamples-working-to-container` to `tests/claims/registry.tsv`.
- Unit tests in `src/runtime/t/export_monitor.t.cpp`: both rate directions vs whole-stream oracle, block-boundary invariance, degenerate-rate faults-as-values, matched-rate no-regression via `audio_dispatches` counter.
- Integration goldens in `tests/audio_export_goldens.t.cpp`: 44.1 kHz decimation flagship, 96 kHz up-sample, continuity/finite-tail, matched-rate 1:1 byte-identity. All byte-exact, no tolerance.
- Container layout conversion parked (no milestone consumer, not a WBS leaf per D5); see `tasks/parking-lot.md`.
