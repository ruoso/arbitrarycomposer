# 11 — Time and Video

How the design extends from stills to video: a global composition timeline,
each layer occupying a time span on it, with the layer's local time
interpolated from composition time. Status: designed; scheduling relative to
v1 recorded at the end.

## The symmetry principle

The entire spatial design rests on one move: content lives in its own
unbounded local space, placement maps that space into the parent by an
affine transform, and the render request carries *where and at what
resolution*. Time extends the design by making the same move on a 1D axis:

| Spatial (docs 01–05) | Temporal (this doc) |
| --- | --- |
| Content's local 2D plane | Content's local time axis |
| Placement: 2D affine transform | Temporal placement: span + 1D affine time map |
| `RenderRequest.region` + `scale` | `RenderRequest.time` |
| Bounds (spatial extent, may be infinite) | Time extent (duration, may be infinite) |
| Culling: outside visible region → skip | Culling: outside span → skip |
| Raster bottoms out at native pixels (`achieved_scale`) | Video bottoms out at native frame times (`achieved_time`) |
| Nested composition remaps space | Nested composition retimes its child |
| Viewport camera (anchor + matrix) | Viewport transport (clock: play/pause/seek/rate) |
| Zoom-in-zoom via structural nesting | Slow-motion-in-slow-motion via rate nesting |

Because every mechanism already answers "what does the compositor ask for,
and what does content promise back", adding *when* to the question is
additive. Nothing structural breaks. The genuinely new machinery is listed
honestly in "New machinery" below.

## Time model

- **Composition time.** Each composition has its own time axis — the
  "global timeline" is simply the root composition's axis, the same way the
  "global canvas" is the root's space. Time is continuous; **frame rate is
  a property of the render, not the model** (an output samples the timeline
  at its rate), mirroring resolution independence. A composition may declare
  a preferred frame rate and duration as authoring hints, like the canvas
  rectangle (doc 01) — nothing is clocked by them.
- **Representation.** Instants are integer counts of a fixed fine timebase
  (flicks: 1/705,600,000 s, exactly divisible by all common video and audio
  rates); rates in time maps are exact rationals (e.g. 24000/1001).
  Composed time maps are evaluated in rational arithmetic and rounded to the
  timebase once, at the leaf — the temporal sibling of doc 04's
  "recompute from per-edge matrices, never accumulate". The precision story
  is *simpler* than the spatial one: no rebasing needed, because exact
  rational composition doesn't degrade with depth. The single leaf rounding
  is to the nearest flick, ties to even — sign-symmetric so reverse playback
  (negative rate) is unbiased; because the flick timebase divides all common
  rates exactly, the rounding is a no-op on realistic rate stacks and bites
  only on adversarial rationals. Rational composition keeps its operands
  reduced to lowest terms in a fixed integer width; a composition that would
  exceed that width even after reduction surfaces as an error value
  (faults-as-values, doc 10), never a silent wrap or an abort — pathological
  rate stacks fail honestly rather than corrupting a time.

## Model changes

**Layer instance** (doc 01) gains temporal placement beside the spatial one:

- `span`: half-open interval [in, out) in parent-composition time during
  which the layer exists. Default (−∞, +∞): a still image is simply a layer
  that is always present — stills are the degenerate case, not a special
  one. A single-frame flash is a span one output-frame long.
- `time_map`: `local_time = (parent_time − in) × rate + offset`, with
  `rate` a rational, negative allowed (reverse playback). This is the
  "interpolation" of composition time into the layer: a 1D affine map,
  exactly as spatial placement is a 2D one. Non-linear retiming curves are
  the temporal analog of non-affine warps — same door, same "later, if
  ever" (doc 04's projective discussion applies verbatim).
- Outside its span a layer is culled, exactly like content outside the
  visible region.

**Content** (docs 01/03) gains temporal metadata beside the spatial:

- `time_extent()`: optional local-time range where the content varies /
  exists — the temporal `bounds()`. A video clip reports its duration;
  static content reports "none" (time-invariant).
- The doc-01 `Stability` enum refines into two orthogonal facts and a
  three-way temporal one:
  - `Static` — output independent of time (all current still kinds),
  - `Timed` — output a *deterministic* function of request time (video
    file, baked animation): cacheable per time,
  - `Live` — non-deterministic (camera feed, running simulation): the old
    `Volatile`; cacheable only within a frame/snapshot.

**Viewport** (doc 01) gains a **transport**: a clock with play/pause/seek,
rate (including reverse and scrub), and loop bounds. It is per-viewport
state like the camera — two viewports may observe the same composition *at
different times* (an editor's preview at the playhead and a filmstrip of
thumbnails at other times), which the pull-based design gives for free, and
which push-based video frameworks structurally cannot do.

The transport's semantics are:

- **Playhead + rate + pause are three independent facts.** The playhead is an
  instant on the composition axis (`Time`, flicks); `rate` is a rational
  playback speed (negative = reverse, retained across pause so resume restores
  it); `pause` is a separate boolean, *not* `rate == 0` — a paused transport
  can still be seeked/scrubbed, and resume plays at the pre-pause rate.
- **The transport reads no wall clock.** Advancing is driven by an elapsed
  *real* duration the host supplies (it owns the sole wall-clock read, as the
  interactive renderer already does for its deadline); the transport scales
  that real duration by `rate` in exact rational arithmetic with one
  ties-to-even leaf rounding — the same sign-symmetric path the time-map math
  uses, so reverse playback is unbiased and pathological rates fault as a
  value, never wrap. A paused advance moves the playhead zero flicks.
- **Loop bounds are half-open `[in, out)` and wrap only on advance, never on
  seek.** A forward advance that reaches or passes `out` re-enters at `in`
  (true modulo, so an advance longer than the loop still lands in range);
  reverse wraps symmetrically at `in`. Seek/scrub sets the playhead to the
  exact requested instant, unconstrained by the loop or pause — a filmstrip
  thumbnail may sit anywhere, including outside the loop window. With no loop
  set, advance runs unbounded and span culling handles emptiness.

## Contract changes (doc 03)

```cpp
struct RenderRequest {
  Rect     region;
  double   scale;
  Time     time;          // NEW: content-local time, computed by the
                          //  compositor through the composed time map
  // Surface& target, Exactness, Deadline, snapshot — unchanged
};

struct RenderResult {
  double achieved_scale;
  bool   exact;
  std::optional<Time> achieved_time;  // NEW: the local time actually
                                      //  rendered, if quantized (e.g. the
                                      //  nearest source video frame)
  std::optional<SurfaceRef> provided; // (doc 09)
};
```

- `Static` content ignores `time` entirely — a solid-color plugin is
  unchanged except for the field existing.
- **`achieved_time` is the temporal `achieved_scale`.** A 24 fps clip asked
  for t=0.31 s renders its frame at 7/24 s and says so. The compositor then
  serves every request in [7/24, 8/24) from that cached entry — on a 60 fps
  output, more than half of all playback requests against 24 fps content
  become cache hits by achieved-time coalescing, with zero decoder work.
- **`quantize_time(t)` — the render-free grid query (compositor pre-lookup).**
  To *serve* [7/24, 8/24) from one entry, the compositor must form the cache
  key at 7/24 **before** it renders — and it cannot learn the bucket edges from
  a post-render `achieved_time` alone (one sample gives the floor, never the
  span; a floor-probe over cached instants over-serves a skipped frame under
  seek). So `Content` gains one pure, render-free query —
  `std::optional<Time> quantize_time(Time) const`, defaulting to `nullopt` (no
  quantization: the requested time is used as-is, the pre-coalescing behaviour)
  — returning the native grid instant a render at `t` would resolve to (a
  24 fps clip returns `floor(t·24)/24`). Contract (conformance-tested): when
  `quantize_time(t)` has a value it MUST equal `render(time = t).achieved_time`,
  and it is idempotent. The compositor keys `Timed` tiles by
  `quantize_time(time).value_or(time)`, so every request in one native frame
  collapses to a single key and the second issues zero renders — sound under
  seek because the content, not the compositor, owns the grid.
- **Playback hints.** New optional content interface:
  `playback_hint(direction, rate, horizon)` — advisory, issued by the
  transport, so decoder-backed content can pre-roll sequentially. Video
  decoders are the one content kind whose cost model is aggressively
  order-dependent (seeking is expensive, decoding forward is cheap); the
  async render path already tolerates their latency, but the hint is what
  makes smooth playback *cheap* rather than merely possible.

The contract tests (doc 10) extend accordingly: time-honesty (`Timed`
content returns identical pixels for identical times), `achieved_time`
correctness, span/extent consistency.

## Pipeline changes (doc 02)

- **Frame planning** samples the transport's current composition time, then
  computes each layer's local time by composing time maps down the tree —
  the same walk that composes transforms, one rational multiply-add per
  edge — and then snaps each `Timed` layer's local time to that content's
  grid via `quantize_time` before the tile-cache lookup, so a sub-frame clock
  advance keys the same native frame and hits the cache (zero renders).
- **Clock advance is the temporal damage.** Advancing time invalidates
  nothing spatial: `Static` layers' cached tiles remain valid and playback
  over a mostly-still scene re-renders only the moving layers. Spatial
  damage (content edits) and temporal advance are orthogonal invalidation
  axes; both funnel into the same "what must re-render this frame" plan.
- **Cache** (doc 02): the key gains a time component for `Timed` content —
  `(content id, revision, scale rung, tile coords, achieved_time)`; `Static`
  content's keys are unchanged (no time dimension, no cache growth for
  stills). Priority classes gain a **temporal prefetch ring** (upcoming
  times in playback direction) alongside the spatial pan-prefetch ring; the
  playback-hint horizon bounds it. The temporal ring is a *distinct*
  priority class, ranked between recently-visible and the spatial
  pan-prefetch ring: an upcoming-playback frame is a stronger prediction
  than a maybe-return recently-visible tile, but yields to the pan ring so
  scrubbing/panning stays responsive. The full eviction order (victim-first)
  is therefore speculative < recently-visible < temporal-prefetch <
  pan-prefetch (adjacent, doc 02) < visible.
- **Offline rendering** was already framed as "video frames" (doc 02): a
  sequence render is a loop over output frame times — snapshot, set time,
  exact render — and the existing snapshot mechanism is precisely what
  keeps frame N from seeing frame N+1's edits. This path needs essentially
  nothing new beyond `time` in the requests.

## Recursion (doc 05)

A nested composition's temporal placement maps parent time to child time:
**time remapping falls out of nesting**, exactly as spatial remapping does.
A child running at rate ½ inside a parent at rate ½ plays at ¼ — slow-motion
nests like zoom. This is Lottie's precomp time-remap and After Effects'
time remapping, generalized to arbitrary depth and to plugin content.

- Shared children evaluated at *different times* by different embeddings
  work without new machinery: the time-keyed cache distinguishes them, and
  the layer-level caches inside the child are shared where times coincide.
- **Temporal cycles** (a composition embedding itself with a time offset —
  video feedback/echo): the spatial termination rule has a temporal sibling
  — a cycle whose time offset is non-zero and whose spatial scale is <1
  terminates by the sub-pixel cull as before; the recursion-depth budget
  (doc 05) backstops the rest. True same-instant self-reference is
  ill-defined and reports the existing cycle diagnostic.
- The doc-05 aggregate revision is untouched: revisions track *edits*,
  time tracks *playback*; the cache key carries both.

## What does NOT change

Docs 04 (spatial precision, rebasing, scale ladders), 07 (color), 09
(surfaces; it already anticipated video decoder output as the second
motivating case for content-provided transient surfaces), and 10 (tooling)
are untouched. Doc 08 anticipated exactly this: temporal fields land as a
format major bump — `span`, `time_map` beside `transform` in placement,
preferred-rate/duration hints beside `canvas`.

## New machinery (the honest cost list)

1. A `Time` type (flicks) + rational rate arithmetic, and temporal placement
   on layer instances. Small, self-contained.
2. `time`/`achieved_time` in the contract + the refined stability enum.
   Small, but it is a *contract* change — the reason to decide scheduling
   now (below).
3. The transport (per-viewport clock + play/pause/seek/rate). Moderate;
   pure addition.
4. Time-keyed caching + temporal prefetch ring + playback hints. Moderate;
   the only part with real performance subtlety (decode-ahead, cache
   pressure between temporal and spatial prefetch).
5. A reference `Timed` kind to prove the contract (simplest honest option:
   an image-sequence kind — exercises `achieved_time`, spans, prefetch —
   before a real codec-backed video kind, which brings the usual ffmpeg
   integration weight and belongs in a plugin regardless, per doc 10's
   dependency policy).

## Explicitly deferred (separate from "video")

- **Keyframed property animation** (animating transform/opacity/params over
  the timeline): orthogonal to timed *content* and a much larger authoring
  surface (interpolators, curves, expression of "property over time" in the
  format). The pipeline is already shaped for it — placement is sampled
  every frame anyway — so it stages cleanly later. "Video" (this doc) and
  "animation" (that one) are distinct features that happen to share a clock.
- **Motion blur**: the seam is `time` becoming a shutter *interval* in the
  request; noted so the field's semantics don't preclude it, not designed.
- **Audio**: since taken up — designed in doc 12 and in v1 scope. The
  model pieces this doc introduces (spans, time maps, exact rational rates,
  transport, temporal prefetch, playback hints) are shared by the audio
  pipeline verbatim.

## Scheduling decision

Three viable positions: temporal-from-day-one (the contract and model carry
time from the first commit; stills are the degenerate case; transport and
the timed reference kind can still land incrementally), full video in v1
(everything above is v1 scope), or stills-only v1 with this doc as the v2
design (accepting a contract major bump later).

**Decision: full video in v1.** Everything in this doc is v1 scope: the
`Time` type and temporal placement, `time`/`achieved_time` in the contract,
the refined stability enum, the transport, time-keyed caching with the
temporal prefetch ring, playback hints, and an image-sequence reference
kind (`org.arbc.imageseq`) proving the `Timed` contract. A codec-backed
video kind remains a plugin outside the core per doc 10's dependency policy
(it brings ffmpeg-class dependencies), but the core it plugs into is fully
temporal at first release. Rationale: the contract fields were the only
piece that was expensive to retrofit, but shipping the machinery too means
v1 demonstrates the design's central claim — one scene model, one contract,
stills and video and interactive and offline — rather than promising it.
Within v1, implementation still sequences naturally: contract fields and
model first, transport and temporal cache next, reference kind last.
