# 12 — Audio

How audio maps into the composition. Status: designed; scheduling relative
to v1 recorded at the end.

## The symmetry, continued

Doc 11 extended the pull contract from *where at what resolution* to
*when*. Audio is the same contract on a 1D signal:

| Visual | Audio |
| --- | --- |
| `region` + `scale` (px per local unit) | time `window` + `sample_rate` (samples per local second) |
| Surface (2D pixel block) | Block (1D sample run × channels) |
| `achieved_scale` (raster at native res) | `achieved_rate` (recording at native rate) |
| Working color space + pixel format (doc 07) | Working sample rate + channel layout + float32 |
| Compositing (source-over) | Mixing (additive) |
| `opacity` | `gain` |
| Sub-pixel cull terminates recursion | Sub-audible cull terminates recursion |
| Viewport (camera + transport) | Monitor (mix policy + same transport) |
| Tile cache (2D tiles per scale rung) | Block cache (fixed sample blocks per rate) |

Sample-rate independence is resolution independence: procedural audio (a
synth, a tone) renders at any requested rate; recorded audio bottoms out at
its native rate and reports it, and the engine resamples — the exact analog
of upscaling a raster past native. `Static`/`Timed`/`Live` stability (docs
01/11) applies verbatim (a tone is `Static` — time-invariant output for a
given window shape is rare, so most audio is `Timed`; a microphone is
`Live`).

The one deliberate **asymmetry**: video under deadline pressure degrades
gracefully (stale tile, coarser rung, checkerboard); audio has no graceful
degradation — a late block is a glitch. This asymmetry shapes the engine
(below): audio never renders on a deadline, it renders *ahead*.

## Audio as a content facet

A layer's audio comes from the same content object as its pixels, not from
a parallel audio layer: a video clip is *one* layer whose content has both
facets, and the shared placement guarantees its audio and video can never
drift apart under editing (moving/retiming the layer moves both).

```cpp
class Content {
  // … visual interface as in doc 03 …
  virtual AudioFacet* audio() { return nullptr; }  // optional capability
};

struct AudioRequest {
  TimeRange window;        // content-local, half-open [t0, t1)
  uint32_t  sample_rate;   // working rate — the temporal "scale"
  ChannelLayout layout;    // working layout
  AudioBlock& target;      // float32, zero-initialized
  Exactness exactness;     // BestEffort (lookahead) | Exact (offline)
  StateHandle snapshot;    // the pinned content state (doc 14), the audio
};                         //  twin of RenderRequest.snapshot

struct AudioResult {
  uint32_t achieved_rate;  // native rate if lower than requested
  bool     exact;
};

class AudioFacet {
public:
  virtual std::optional<TimeRange> audio_extent() const = 0;
  virtual Stability audio_stability() const = 0;
  virtual Time latency() const { return Time::zero(); }  // see "Sync"
  virtual std::optional<AudioResult>
  render_audio(const AudioRequest&, std::shared_ptr<AudioCompletion>) = 0;
};
```

The completion is `AudioCompletion`, the audio instantiation of the one shared
one-shot settlement primitive — `RenderCompletion` and `AudioCompletion` are
both `Completion<Result>` (over `RenderResult` / `AudioResult`), so the
thread-safe settle/cancel/`take` machinery is written and verified once and
`render_audio` answers along the identical "return inline or settle later via
the completion" code path as `Content::render` (doc 03). Likewise
`AudioRequest.snapshot` is the same `StateHandle` a `RenderRequest` pins (doc
14): audio and video share one content object and one revision space, so they
must pin the same frozen state or a clip's samples could come from a different
revision than its pixels.

Audio-only content (a music track, a synth plugin) implements the audio
facet and reports empty visual bounds; it is culled from every visual pass
and participates fully in the mix. Purely visual content returns no facet
and costs the audio engine nothing.

Placement (doc 01) gains the audio siblings of its visual fields: `gain`
(0..∞, analog of opacity but not clamped at 1 — boosting is legitimate) and
`audible` (analog of `visible`). The temporal placement — span and time map
— is *shared*, which is the point of the facet design.

## Working format

Per composition, the audio analog of doc 07's working space: a working
sample rate (default 48 kHz) and channel layout (default stereo), samples
always float32 (audio's numbers are already linear — there is no transfer
function problem, so the doc-07 kernel/variant machinery is unnecessary;
one format suffices). Conversions live at the edges exactly as in doc 07:
content converts its native format when rendering; the monitor converts
working → device. Nested compositions may declare different working
formats; the nesting boundary converts (resample + remix), homogeneous
trees pay nothing.

The working → device edge reuses the same band-limited polyphase resampler
as the nesting boundary and the below-native reconstruction (one shipped
windowed-sinc kernel, not a second algorithm) — the audio analog of the
single raster resampler serving every zoom remainder. A device whose rate
equals the working rate keeps a byte-for-byte 1:1 drain (no SRC cost); a
device rate *above* the working rate is upsampled at the edge through the
fixed input-Nyquist table. Downsampling (device rate *below* the working
rate) is decimated at the edge with a **ratio-scaled widened lowpass cut at
the device Nyquist** — generated off the RT thread over the same
windowed-sinc prototype and 32-phase bank (the impulse response widened by
the decimation ratio so it stays anti-aliased below the device Nyquist, an
extension of the fixed-cutoff table, not a reuse of it). Both rate directions
are now handled; only a device rate *equal* to the working rate takes the 1:1
drain. Because the device edge is downstream of the pull graph, the
resampler's constant group delay is absorbed by the monitor's lookahead
pre-roll, not declared through a facet `latency()`.

## Rate maps, varispeed, and pitch

The layer time map (doc 11) applies to audio as to video: a rate-½ layer
plays its audio at half speed. The *model* defines only the time mapping;
what that does to pitch is a **rendering policy**:

- **Varispeed** (default): resample through the composed rational rate —
  tape-style, pitch shifts with rate. Exact, cheap, always correct as a
  *signal* interpretation of the time map, and well-defined at any nesting
  depth because composed rates are exact rationals (doc 11).
- **Time-stretch** (pitch-preserving) is DSP with quality/latency
  tradeoffs; it is an extension (per-layer policy flag routed to a
  stretcher implementation), deferred with the effects stack.

Scrubbing is the transport driving short varispeed windows — no special
audio path.

## Spatialization: the transform as an optional mix input

The genuinely novel question: layers have 2D transforms — does position
affect the mix? Both answers are right for different hosts, so the choice
is **monitor policy**, not model:

- **Flat** (default): spatial transform ignored; contribution = gain ×
  mix. What a video editor expects.
- **Spatial**: the monitor derives per-layer pan from composed position in
  the viewport and attenuation from composed scale — the camera is the
  listener. Zooming toward a nested composition brings its soundscape
  forward; zooming out fades it into the ambience. **Sub-audible cull**:
  below an attenuation threshold a subtree contributes nothing and is not
  descended — the audio analog of the sub-pixel cull, and what makes the
  audio of infinitely deep (and Droste) scenes terminate in Spatial mode.

In Flat mode recursion termination needs its own rule, and gets a
satisfying one: a recursive embedding with gain < 1 converges (each turn
quieter — this is literally a feedback echo, well-defined when the cycle's
time offset is at least one block); gain ≥ 1 cycles hit the doc-05 depth
budget and diagnostic, like their visual siblings.

Real spatial-audio rendering (HRTF, distance models beyond attenuation) is
monitor-implementation territory, extensible later; the design point is
only that *the composed transform is available to the mix* behind a
policy.

## The engine: monitors, clocking, lookahead

A **monitor** is the audio analog of a viewport: attached to a transport,
it pulls the mix. Two implementations matter:

- **Device monitor** (interactive): owns an audio device stream. Because
  audio cannot glitch, the engine renders **ahead of the playhead** into a
  ring of prepared blocks (lookahead budget, e.g. 100–500 ms, configurable
  against interactivity): worker threads execute `render_audio` pulls and
  the mix graph *off* the device thread; the device callback only consumes
  prepared, mixed blocks. Arbitrary plugin code never runs on the audio
  callback — the price is lookahead latency on transport changes
  (play/seek flushes and re-primes the ring), which is the standard,
  correct trade for a plugin host that cannot vouch for third-party
  RT-safety. The whole callback chain — the device fill, the drain that
  hands a prepared block from the ring to the callback, and the edge
  format/rate conversion — is itself lock-free, allocation-free, and
  refcount-free: nothing on it blocks. This is not a convention but a
  build-failing guarantee, enforced by RealtimeSanitizer annotations on the
  chain plus a debug allocator/refcount/lock guard (doc 16).
- **Export monitor** (offline): sample-exact rendering of the mix over a
  time range, driven by the offline renderer's frame loop; snapshot
  semantics per doc 02 keep an export consistent with concurrently-edited
  scenes. The mix is produced at the composition working rate; converting to
  a different container output rate is the same shared working → edge
  resampler the device monitor uses (a separate export-edge step), not a
  second path. Muxing audio with exported frames is the host's business (or a
  container plugin's), not the core's.

**Clock mastering.** When a device monitor is attached to a transport, the
device clock *is* the timebase: the transport derives composition time
from samples delivered, and video viewports on the same transport schedule
frames against it — video chases audio, never the reverse (the universal
A/V discipline, because the ear detects timing error the eye forgives).
A transport without a device monitor free-runs on the system clock as
before. One device monitor per transport; multiple transports (doc 11's
independent playheads) may each have their own. On such a rebase the device
drain cursor is re-seated to the block covering the reprimed playhead — the
pre-change working carry is dropped — so post-seek/-rate device output is
byte-exact from the first delivered frame.

**Prefetch and caching.** The block cache is the tile cache with 1D keys —
`(content id, revision, block index, rate)` — with the temporal prefetch
ring (doc 11) as its primary fill driver; playback hints serve decoders
identically for audio and video (often the same decoder). Caching matters
less than for pixels (audio is cheap to re-render) except for
decode-behind-seek, which the hints and lookahead already cover.

**Damage.** Audio damage is a time range in local time ("samples after t
changed"); it invalidates cached blocks and — if within the lookahead
window — forces a re-mix of prepared blocks. The same revision/damage
machinery as doc 01, one dimension down.

## Sync and latency

Content may declare processing `latency()` (a live 3D engine's sound with
a pipeline delay, a lookahead limiter later): the engine pre-rolls that
content's requests earlier by its latency so contributions align — the
seed of DAW-style plugin delay compensation. v1 honors declared constant
latency in the lookahead scheduler; full PDC (dynamic latency, latency in
nested graphs' effect chains) is deferred with the effects stack.

Concretely, v1 honors it as a **fill-lead extension**: because a pull model
has no real pipeline delay to un-align (`render_audio(window)` returns
exactly that window), a declared latency is a *residency* concern — a
`Live`/stateful source needs more lead to have a block ready. The lookahead
ring therefore extends its transitive fill horizon by the maximum declared
`latency()` among the anchor block's audible direct contributors (floored by
a configurable pre-roll), warming output blocks that much further ahead of
the playhead. The output blocks, their keys, and their windows are
**unchanged** — only more are primed — so the drain stays byte-identical to
the zero-latency mix. Shifting *which* window a content is requested for
(true per-content output-window re-alignment, which the mixer must
compensate for) is part of full PDC and deferred with the effects stack.

## Recursion

Nothing new to invent: a nested composition's audio facet mixes its
children (through the child's working format, through the embedding's time
map and gain) exactly as its visual facet composites them. The synthetic
viewport of doc 05 gains a synthetic monitor; budgets flow through as in
doc 05; the aggregate revision covers audio damage since it is the same
revision space.

Under lookahead (the device monitor above) recursion has one operational
consequence. Because worker threads render `render_audio` *off* the device
thread, the fill must warm the **transitive contributor closure** — the
recursive descent through nested compositions *and* the below-rate native
re-request a resampling boundary provokes — so that when a worker renders a
nested contributor its own descendants are already resident. A contributor
block is dispatched only once its closure is resident, and an output block
is mixed only at full transitive residency; the threaded fill
(`worker_count > 0`) is therefore byte-identical to the inline fill
(`worker_count == 0`) — the recursion never mixes silence for a
not-yet-rendered descendant. The closure the fill warms is exactly the tree
the mixer would walk: the doc-05 recursion-depth budget and the Flat-mode
sub-audible/`gain ≤ 0`/facet-less/out-of-span culls bound it identically.

## Deferred

Audio effects now have their mechanism — operator content (doc 13), fade's
gain envelope being the reference case — but the DSP *library* (reverbs,
EQs, time-stretch), full PDC, HRTF/3D audio monitors, loudness
metering/normalization, and MIDI or control-rate signals remain deferred.
All are additions behind existing seams; none touch the contract.

## Scheduling decision

**Decision: full audio in v1.** The facet and placement fields, the working
audio format, the audio engine with the lookahead scheduler, the device
monitor with audio-clock mastering, and the export monitor are all v1
scope. v1 is thereby a complete A/V compositor: one scene model, one pull
contract, pixels and samples, interactive and offline. Reference proof of
the facet contract rides on the kinds already planned: `org.arbc.solid`
gains an audio sibling (`org.arbc.tone`, the "hello world" of the audio
facet), the image-sequence kind (doc 11) stays visual-only (proving
facet-less content costs nothing), and `org.arbc.nested` implements both
facets (proving recursion). Codec-backed A/V file content remains a plugin
outside the core per doc 10. Within v1, implementation sequences: contract
+ model first, export monitor (no realtime pressure) second, device
monitor + lookahead scheduler last.
