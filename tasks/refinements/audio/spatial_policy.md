# audio.spatial_policy — Spatialization monitor policy

## TaskJuggler entry

Back-link: [`tasks/45-audio.tji`](../../45-audio.tji), `task spatial_policy`
("Spatialization monitor policy", lines 82–87). This refinement expands that
one-line WBS leaf. WBS note, verbatim:

> Flat (default) and Spatial (camera-as-listener pan/attenuation, sub-audible
> cull terminating recursion); gain<1 cycles converge in flat mode. Doc 12.

## Effort estimate

`effort 2d` (`tasks/45-audio.tji:83`). The `MixPolicy` seam is already
reserved and wired: `mix_composition` (`src/audio_engine/arbc/audio_engine/mix.hpp:59-61`),
the two monitors, and the lookahead ring already thread `MixPolicy::Flat`
and reach a `(void)policy;` stub (`src/audio_engine/mix.cpp:170`). The work
is (a) a small contract field carrying the spatial context across the pull
boundary, (b) the pan/attenuation/sub-audible-cull branch at the two
duplicated walk sites (`mix_layer` L4 and `NestedContent::mix_child_layer`
L3), and (c) threading the context through the monitors and the lookahead
ring's mix request. The heavy machinery (pull, block cache, resampler,
composition walk) is untouched; this is a bounded extension of settled
seams, with a design-doc delta pinning the concrete v1 model.

## Inherited dependencies

The parent `task audio` carries `depends contract.audio_facet,
timeline.transport`; `spatial_policy` adds `depends !mix_engine`
(`tasks/45-audio.tji:85`) — notably **not** `!lookahead` or
`!device_monitor`, though it lands after both (which are complete) and
touches the ring and monitors they shipped.

- **`audio.mix_engine`** — *settled* (DONE 2026-07-07,
  `tasks/refinements/audio/mix_engine.md`). Landed the seam this task fills:
  - `mix_composition(const DocRoot&, ObjectId, const MixResolver&,
    PullService&, const AudioRequest&, MixPolicy policy = MixPolicy::Flat)`
    (`src/audio_engine/arbc/audio_engine/mix.hpp:59-61`), pure over the pin;
    equal `(pin, composition, window, rate, layout)` ⇒ bit-identical samples.
  - `enum class MixPolicy { Flat }` (`mix.hpp:41`) — this task adds `Spatial`.
  - `mix_layer` (`src/audio_engine/mix.cpp:25-200`): the per-layer cull loop
    with the reserved `(void)policy;` at `mix.cpp:170` and the additive
    1:1 mix at `mix.cpp:164-186`. mix_engine Decision "Flat mix only, behind
    a `MixPolicy` seam" and Constraint 6 explicitly hand the Spatial branch —
    "the pan/attenuation/sub-audible-cull branch … with the layer's composed
    transform already in hand" — here.
- **`audio.lookahead` / `audio.lookahead_recursive_prefetch`** — *settled*
  (complete). Landed `LookaheadRing`
  (`src/audio_engine/arbc/audio_engine/lookahead.hpp`) whose
  `LookaheadRingConfig::policy{MixPolicy::Flat}` (`lookahead.hpp:71`) and
  transitive-contributor descent (`descend`/`contributions_for`/
  `collect_wants`, `lookahead.hpp:255-363`) this task threads the spatial
  context through so the *device* path spatializes. The established
  `nested_composition` injection (`lookahead.hpp:88`) is the precedent for how
  an L4 walk learns nesting structure without introspecting L3.
- **`audio.export_monitor` / `audio.device_monitor`** — *settled* (complete).
  `ExportMonitor` (`src/runtime/arbc/runtime/export_monitor.hpp`, ctor takes
  `MixPolicy policy`, `d_policy` at `:156`, threaded in `render_block_at`) and
  `DeviceMonitor` (via the `LookaheadPump`/`LookaheadRing`) are the two
  monitors that select the policy. There is **no `Monitor` base class** — the
  policy is realized purely through the `MixPolicy` value + the spatial
  context each monitor threads into the `AudioRequest`.
- **`contract.audio_facet`** — *settled*. Owns `AudioRequest`
  (`src/contract/arbc/contract/content.hpp:242-249`) and `AudioResult`
  (`:217-220`) — the request this task extends with the optional spatial
  context.

## What this task is

Turn the reserved `MixPolicy` seam into a working **Spatial** monitor policy
alongside the default **Flat**, per doc 12:140–165 and the concrete v1 model
this task adds there (design-doc delta). Deliverables:

1. **Contract (L3): the spatial context on `AudioRequest`.** A new
   `struct Spatialization { Affine listener; double viewport_w; double
   viewport_h; float accum_atten; float sub_audible; }` and an optional
   `std::optional<Spatialization> spatial{}` field appended to `AudioRequest`
   (`content.hpp:242-249`). Absent ⇒ Flat (existing 6-field aggregate inits
   stay valid, byte-identical behavior). Present ⇒ Spatial. This is the *only*
   carrier that crosses the pull boundary into nested contributors, so nested
   compositions spatialize on the same footing as the root. `Affine` is L0
   (`src/base/arbc/base/transform.hpp:12-40`), already in `contract`'s
   closure — no levelization edge added.
2. **Audio-engine (L4): the Spatial branch in `mix_layer`.** Add
   `MixPolicy::Spatial` (`mix.hpp:41`). When `request.spatial` is present:
   compose `composed = compose(spatial.listener, layer.transform)`; derive the
   per-edge attenuation `edge_atten = clamp(max_scale(layer.transform), 0, 1)`
   and the constant-power pan gains from `composed`'s viewport position; apply
   the **sub-audible cull** (`spatial.accum_atten * edge_atten <
   spatial.sub_audible` ⇒ return before pulling — terminating recursion); build
   the child `AudioRequest` with `spatial = { compose(listener,
   layer.transform), viewport_w, viewport_h, accum_atten * edge_atten,
   sub_audible }` so the context accumulates down the descent; mono-collapse
   the child block and mix `gain * edge_atten * pan_gain[c] * m` into the
   target. When `request.spatial` is absent, the existing Flat path runs
   unchanged.
3. **kind-nested (L3): the same branch in `NestedContent::mix_child_layer`.**
   The audio nested walk (`src/kind_nested/nested_content.cpp:394-567`, today
   ignoring `transform`) gets the identical `request.spatial`-keyed branch — it
   reads only the contract `AudioRequest.spatial` (never the L4 `MixPolicy`
   enum), so a nested subtree pulled by the mixer spatializes and accumulates
   its own contributors, and the sub-audible cull terminates Droste scenes at
   the natural depth. The walk stays duplicated between L4 and L3 exactly as
   the Flat walk and the visual walk are (doc 17:41 Decision).
4. **Lookahead ring (L4): thread the context into the device mix.**
   `LookaheadRingConfig` gains an `std::optional<Spatialization> spatial{}`
   seed; the ring sets it on the `AudioRequest` it hands `mix_composition`
   (`mix_block`), so the device monitor's mix spatializes. The ring's
   *descent-cull tightening* (culling the warmed closure by attenuation) is
   **deferred** (see Acceptance criteria) — the drain stays byte-exact without
   it because the ring warms a superset of what the culling mixer pulls.
5. **Monitors (L5): accept and seed the context.** `ExportMonitor` and
   `DeviceMonitor` accept a static spatialization seed (listener/viewport/
   threshold) alongside their existing `MixPolicy`; on Spatial they populate
   the top-level `AudioRequest.spatial` (export directly in `render_block_at`;
   device via the ring config) with `listener = camera`, `accum_atten =
   clamp(max_scale(camera), 0, 1)`, and post-scale the top mix by the camera's
   uniform scale-attenuation. The **live** per-tick camera-follow binding
   (audio spatialization tracking the interactive viewport's camera in real
   time) is **deferred** to a named leaf; this task ships the static seam and
   its byte-exact export goldens.

**Out of scope**, each mapped to a named leaf or an existing deferral:

- The ring's warmed-closure attenuation cull → `audio.spatial_fill_cull`
  (deferred leaf; closer registers).
- Live interactive camera-follow of the device monitor's listener →
  `audio.spatial_camera_follow` (deferred leaf; closer registers).
- HRTF, distance models beyond attenuation, non-collapsing per-leaf pan →
  doc-deferred "monitor-implementation territory" (doc 12:162-165), **not** a
  WBS leaf; noted in the return summary for the parking lot.
- Flat-mode gain<1 feedback-echo *convergence over blocks* → already the
  `audio.lookahead` ring's concern (mix_engine Decision); this task keeps the
  Flat recursion terminator (the doc-05 depth budget) unchanged.

## Why it needs to be done

`spatial_policy` is the last audio-semantics leaf the M6 milestone
(`tasks/99-milestones.tji:49`, `task m6_audio`) waits on for its stated
capability "spatialization as monitor policy" (doc 00:124-129). The Flat
default already ships (`mix_engine`); without this task the `Spatial`
enumerator, the camera-as-listener attenuation/pan, and the sub-audible cull
that makes infinite-zoom/Droste scenes terminate *gracefully* (rather than
only at the hard depth budget) do not exist — the `(void)policy;` stub
(`mix.cpp:170`) is the whole of it today. It closes the audio side of the
"the composed transform is available to the mix behind a policy" promise
(doc 12:162-165) that the visual side already honors via `render_layer`'s
`max_scale`/`inverse` cull (`src/compositor/compositor.cpp:36-44`).

## Inputs / context

**Governing design doc — doc 12 (normative, doc 16):**
- **Spatialization** (`docs/design/12-audio.md:140-165`) — the mix policy is a
  **monitor** choice, not model; **Flat is the default** ("contribution = gain
  × mix", `:146`); **Spatial** derives "per-layer pan from composed position in
  the viewport and attenuation from composed scale — the camera is the
  listener" (`:148-149`); the **sub-audible cull** ("below an attenuation
  threshold a subtree contributes nothing and is not descended … what makes
  the audio of infinitely deep (and Droste) scenes terminate", `:151-154`).
- **The v1 Spatial model (concrete)** — the design-doc delta this task adds
  (`12-audio.md`, the `### The v1 Spatial model (concrete)` subsection): the
  optional spatial context on the request, per-edge multiplicative attenuation
  == full composed scale, √-law constant-power pan (byte-exact), mono-collapse
  before placement, accumulated-attenuation sub-audible cull.
- **Flat-mode termination** (`:156-160`) — gain<1 converges (feedback echo),
  gain≥1 hits the doc-05 depth budget; **unchanged** by this task.
- **Deferred** (`:162-165, 265-271`) — HRTF/3D audio monitors, distance models
  beyond attenuation, reverbs/DSP library: "monitor-implementation territory,
  extensible later."
- **The symmetry** (`:19-20`) — "Sub-pixel cull terminates recursion" ↔
  "Sub-audible cull terminates recursion"; "Viewport (camera + transport)" ↔
  "Monitor (mix policy + same transport)".
- **Recursion / determinism** (`:242-263`) — "The closure the fill warms is
  exactly the tree the mixer would walk: the doc-05 recursion-depth budget and
  the Flat-mode sub-audible/`gain ≤ 0`/facet-less/out-of-span culls bound it
  identically." This task preserves byte-exact threaded == inline drain by
  warming a *superset* in Spatial mode (the ring cull-tightening is the
  deferred `audio.spatial_fill_cull`).

**Doc 04 (transforms), doc 05 (recursion budgets):**
- `compose(outer, inner)` composes per edge, never accumulated
  (`docs/design/04-transforms-and-infinite-zoom.md`); the sub-pixel cull
  (`:70-72`) is the visual analog the sub-audible cull mirrors.
- Recursion terminates on the sub-pixel/sub-audible cull or the shared
  recursion-depth budget; budgets flow *through* nesting, never reset
  (`docs/design/05-recursive-composition.md:61-67,93-100`).

**Doc 17 levelization (CI-enforced):**
- `AudioRequest`/`Spatialization` live in `contract` **L3**
  (`docs/design/17-internal-components.md:53`); `Affine` is base **L0/L1**;
  `mix_layer` and the ring are `audio-engine` **L4** (`:28,57`);
  `NestedContent` is `kind-nested` **L3**; the monitors are `runtime` **L5**
  (`:60,84-86`). The spatial context on the L3 request is what lets an L3
  nested contributor and the L4 engine spatialize without either naming the
  other (doc 17:41 "no same-level edge") — mirroring how the L4 ring already
  learns nesting via an injected `nested_composition` predicate rather than
  introspecting L3 `NestedContent`.

**Code seams the implementation extends:**
- `mix_composition`/`mix_layer` — `src/audio_engine/arbc/audio_engine/mix.hpp:59-61`,
  `src/audio_engine/mix.cpp:25-200` (culls `:28-73`, pull `:100-101`, additive
  1:1 mix `:164-186`, the `(void)policy;` seam `:170`).
- `NestedContent::mix_child_layer` / `NestedAudioFacet::render_audio` —
  `src/kind_nested/nested_content.cpp:394-567,569-611` (the L3 recursion twin;
  visual side already composes `compose(camera, layer.transform)` at `:252`,
  the audio side ignores `transform` today).
- `LookaheadRing` — `src/audio_engine/arbc/audio_engine/lookahead.hpp:53-105`
  (`LookaheadRingConfig`), `mix_block`/`prime` (`lookahead.hpp:271,365-405`),
  `descend`/`contributions_for` (`:255-262`), `nested_composition` (`:88`).
- Monitors — `src/runtime/arbc/runtime/export_monitor.hpp:85-146,156`,
  `src/runtime/arbc/runtime/device_monitor.hpp`, and the pump config
  `src/runtime/arbc/runtime/lookahead_pump.hpp`.
- Model / geometry — `LayerRecord.transform` (`src/model/arbc/model/records.hpp:70`),
  `.gain` (`:78`); `Affine::max_scale`/`compose`/`apply`
  (`src/base/arbc/base/transform.hpp:24,34,43`); `Vec2`
  (`src/base/arbc/base/geometry.hpp:8`); `Viewport{width,height,camera}`
  (`src/compositor/arbc/compositor/compositor.hpp:15-28`).
- Contract — `AudioRequest`/`AudioResult`
  (`src/contract/arbc/contract/content.hpp:242-249,217-220`).

**Existing claims to extend, not duplicate** (`tests/claims/registry.tsv`,
`12-audio#…`): `#mix-engine-mixes-layers-additively`,
`#mix-engine-facetless-costs-nothing` (Flat behavior stays byte-identical);
`#export-monitor-mixes-exactly-over-range`; the facet contract is already
covered by `check_audio_facet_consistency`/`check_audio_async`
(`testing/arbc/testing/contract_tests.hpp:140,148`).

## Constraints / requirements

1. **Levelization (doc 17:41-44).** `Spatialization` + the `AudioRequest`
   field are `contract` L3 (using only L0/L1 `Affine`); the Spatial branch and
   the ring change are `audio-engine` L4; the nested branch is `kind-nested`
   L3 (keying off the L3 request field, never naming L4 `MixPolicy`); the
   monitor seeding is `runtime` L5. No new component edge — CI levelization
   check stays green.
2. **Flat is byte-identical.** `request.spatial == nullopt` ⇒ the exact
   current mix path (`mix.cpp:164-186`) with the same samples. Every existing
   Flat golden and claim passes unchanged; the 6-field `AudioRequest` aggregate
   inits across the tree keep compiling (the new field is defaulted and last).
3. **Determinism / byte-exactness (doc 16).** Spatial output is a pure
   function of `(pin, composition, window, rate, layout, spatial)`. All Spatial
   arithmetic is byte-exact: attenuation is `clamp`/multiply on `Affine`
   coefficients; the pan law is the **square-root constant-power** law
   (`t=(p+1)/2`, `gL=√(1−t)`, `gR=√(t)`) using IEEE-754 `std::sqrt`
   (correctly-rounded, platform-stable) — **not** `sin`/`cos`; the mono
   collapse is `0.5*(L+R)`. Golden oracles use the integer-flick `parab_sine`
   tone (as the existing audio goldens do), never `std::sin`.
4. **Attenuation composes per edge, applied once.** Per layer the sample gain
   is `clamp(max_scale(layer.transform), 0, 1)`; the product down a nesting
   chain equals the full composed-scale attenuation with no double counting
   (identical composition rule to `gain`). Amplification is capped at unity
   (Spatial never exceeds Flat loudness). The camera's uniform scale-atten is
   applied once to the root mix by the monitor.
5. **Sub-audible cull terminates recursion and is consistent between the two
   walk sites.** A layer with `accum_atten * edge_atten < sub_audible` is not
   pulled and not descended, in *both* `mix_layer` and `mix_child_layer`, so a
   Droste chain crosses the threshold at a finite depth; the doc-05 depth
   budget remains the hard backstop. The default `sub_audible` is a named
   constant `k_sub_audible_atten = 2^-12` (≈ −72 dBFS; below single-contributor
   audibility, terminating a scale-½ chain at depth 12 — well inside the 64
   depth budget), carried in the context so a monitor can override it without a
   code change.
6. **Pan model.** Pan is derived from the layer's composed viewport x-position
   (`composed.apply({0,0}).x` normalized to `[-1,1]` about `viewport_w/2`,
   clamped); the child is mono-collapsed before placement. Pan is exact for a
   composition's direct layers and collapses across nesting boundaries (a
   nested subtree is placed at its embedding position). Mono output applies
   attenuation only (no pan field).
7. **Device path spatializes via the ring; drain stays byte-exact.** The ring
   sets `request.spatial` for its `mix_composition` calls. Without the
   deferred descent-cull tightening the ring warms a *superset* of the
   culling mixer's pulls (bounded by the existing `max_depth`), so the
   threaded fill == inline fill drain remains byte-identical and
   `silence_mixed()` stays 0.
8. **No RT-safety regression.** The drain path is untouched (spatial mixing is
   producer-side, in `mix_composition`/`mix_block`, never on the RT drain);
   `ARBC_RT_NONBLOCKING` guarantees hold as-is.
9. **Design-doc delta (this task).** doc 12 gains the concrete v1 Spatial model
   subsection (the mechanism doc 12 left "behind a policy"); it rides in the
   closer's commit (doc 16 same-commit rule). No doc 00 bullet: the
   project-shaping decision ("spatialization as monitor policy (flat by
   default)") is already recorded at doc 00:126; the concrete pan/atten/
   threshold model is task-level detail the doc 12 delta captures.
10. **Diff coverage ≥ 90%** on changed lines (doc 16 CI gate); tests ship in
    this task.

## Acceptance criteria

- **Claims-register growth** (`tests/claims/registry.tsv`, `12-audio#…`),
  each enforced by an `// enforces: 12-audio#<slug>`-tagged test:
  1. `12-audio#spatial-attenuates-by-composed-scale` — "In Spatial mode a
     layer's contribution is scaled by `clamp(max_scale(composed_transform), 0,
     1)`, composing multiplicatively down nesting so a contributor's net
     attenuation equals its full composed scale (≤ 1); a Flat request (no
     spatial context) is byte-identical to the mix-engine Flat path." Enforced
     by a **byte-exact golden** in `src/audio_engine/t/mix.t.cpp` / a
     `tests/audio_spatial_goldens.t.cpp`.
  2. `12-audio#spatial-pans-by-composed-position` — "A layer is panned by a
     square-root constant-power law from its composed viewport x-position
     (center = equal power L/R; full-left = L only), byte-exact via IEEE
     `√`; mono output applies attenuation only." Byte-exact golden.
  3. `12-audio#spatial-sub-audible-cull-terminates-recursion` — "A recursive
     (Droste) embedding in Spatial mode contributes nothing and is not
     descended once its accumulated attenuation falls below the sub-audible
     threshold, terminating the recursion before the doc-05 depth budget;
     the equivalent Flat scene terminates only at the depth budget." Enforced
     by a behavioral-counter test (below) plus a golden that the Spatial mix
     converges (finite, deterministic samples).
- **Byte-exact goldens** (deterministic, `std::memcmp`, no tolerances):
  - A two-layer stereo composition (distinct `parab_sine` tones, offset
    `transform` positions, unit scale) mixed Spatial vs a hand-computed
    √-law-panned reference — byte-identical.
  - A nested composition embedded at scale ½ mixed Spatial: the nested
    subtree's contribution equals the flat nested mix scaled by the
    per-edge/composed attenuation and placed by the √-law pan — byte-identical
    (attenuation composition + nesting collapse pinned).
  - A Flat request over the same scenes equals the pre-existing mix-engine /
    export goldens byte-for-byte (no Flat regression).
- **Behavioral-counter assertions** (never wall-clock, doc 16): drive a
  self-embedding (Droste) composition in Spatial mode and assert the
  `pull_audio` dispatch count is **finite and equals the sub-audible depth's
  contributor count** (recursion stops at the cull, not the budget); the same
  scene in Flat mode dispatches down to the depth budget. Reuse the audio
  dispatch counter (`note_audio_dispatch`, `src/compositor/arbc/compositor/counters.hpp`).
- **Threaded == inline determinism (device path)**: an existing lookahead
  determinism test extended with `LookaheadRingConfig.spatial` set — the
  threaded fill (`worker_count > 0`) drain is byte-identical to the inline
  (`worker_count == 0`) Spatial mix, and `silence_mixed()` stays 0 (the ring
  warms a superset of the culling mixer's pulls).
- **Export-path Spatial golden**: `ExportMonitor` constructed with
  `MixPolicy::Spatial` + a static listener/viewport renders a scene to a
  byte-exact golden via `render_range` — the testable end-to-end Spatial path.
- **No new conformance family.** The Spatial policy is engine/monitor
  machinery, not a content kind or operator; it adds no `arbc-testing` family.
  Its tests drive `org.arbc.tone`/`org.arbc.nested` through the Spatial mix.
- **Deferred follow-ups** (closer registers each as a WBS leaf under
  `task audio`, wired into `m6_audio`'s `depends`,
  `tasks/99-milestones.tji:49`):
  - `audio.spatial_fill_cull` (effort `1d`, depends `!spatial_policy`) —
    "Apply the Spatial sub-audible attenuation cull inside
    `LookaheadRing::descend`/`contributions_for` so the warmed contributor
    closure terminates at the sub-audible depth instead of `max_depth` in
    Spatial Droste/infinite-zoom scenes (a warming-cost optimization; drain
    byte-exactness already holds via superset warming). Behavioral-counter:
    warmed-block count in a Spatial Droste scene drops to the cull depth.
    Source: audio.spatial_policy scope boundary; see
    tasks/refinements/audio/spatial_policy.md. Doc 12."
  - `audio.spatial_camera_follow` (effort `1d`, depends `!spatial_policy`,
    `!device_monitor`) — "Bind the interactive runtime's live visual viewport
    camera into the `DeviceMonitor`'s `Spatialization` listener each tick and
    on camera/transport change, so audio spatialization tracks the user's zoom
    live (the interactive 'camera is the listener' coupling); this task ships
    only the static seam. Source: audio.spatial_policy scope boundary; see
    tasks/refinements/audio/spatial_policy.md. Doc 12."
- **WBS gate.** After the closer adds `complete 100` and the `Refinement:`
  back-link to `tasks/45-audio.tji`, `tj3 project.tjp 2>&1 | grep -iE
  "error|warning"` is silent.

## Decisions

- **D1 — The spatial context crosses the pull boundary on `AudioRequest`, not
  as an `audio-engine` parameter.** *Authority:* doc 12:148-154 (Spatial
  recurses — "not descended" is a per-subtree decision), doc 17:41,53.
  *Rationale:* the only channel from a mixing level into a nested contributor's
  render is the `AudioRequest` it hands `pull_audio`; the L3 nested walk cannot
  see the L4 `MixPolicy` enum. Putting an optional `Spatialization` (with an L0
  `Affine`) on the L3 request lets both the L4 engine and the L3 nested
  contributor spatialize and *accumulate* attenuation down the tree — which is
  what makes the sub-audible cull terminate Droste scenes at all. *Rejected:*
  keeping the policy an L4-only `mix_composition` parameter (cannot reach
  nested renders across the pull boundary → no accumulation → the cull can only
  see the first embedding, so infinite zoom would not terminate via the cull as
  doc 12:154 requires); a flag on `AudioResult` self-describing "I
  spatialized" (invasive to a hot trivially-copyable type, and still cannot
  carry the *listener* a child needs).
- **D2 — Attenuation is a per-edge multiplicative gain, composing exactly like
  `gain`; no native-vs-nested introspection.** *Authority:* the doc-12 additive
  model; `LayerRecord.gain` composition. *Rationale:* applying
  `clamp(max_scale(layer.transform),0,1)` once per layer at each level makes a
  contributor's net attenuation the product down its chain = its full composed
  scale (for scales ≤ 1), with no double counting — identical to how `gain`
  already composes, so it needs no new machinery and no way to tell a native
  leaf from a nested block. The accumulated scalar is threaded only for the
  *cull* decision. *Rejected:* applying the full accumulated attenuation at
  each level (double-counts outer edges); applying it only at leaves via an
  injected `nested_composition` predicate (works, but adds a `mix_composition`
  signature dependency the per-edge rule makes unnecessary).
- **D3 — Square-root constant-power pan (byte-exact), mono-collapse before
  placement.** *Authority:* doc 12:148-149 (pan from composed position),
  doc 16 (byte-exact goldens). *Rationale:* `√` is IEEE-754 correctly-rounded
  and platform-stable, so a √-law constant-power pan is goldenable where a
  `sin`/`cos` equal-power law is not (the reason the tone oracle is
  `parab_sine`, not `std::sin`). Mono-collapsing the child before placement
  gives a coherent "camera is the listener" point-source and a uniform
  mono/stereo path. *Rejected:* a `sin`/`cos` equal-power law (not byte-exact
  across platforms); a linear/balance law (not constant-power — audible level
  dip at center); preserving nested internal stereo width (needs non-collapsing
  multichannel accumulation — HRTF-adjacent, doc-deferred at doc 12:162-165).
  *Consequence, accepted:* pan is exact for a composition's direct layers and
  collapses across nesting boundaries; true per-leaf composed-position pan is
  the deferred extension.
- **D4 — Sub-audible threshold is a named default `2^-12` carried in the
  context; the depth budget stays the hard backstop.** *Authority:* doc 12:151-160
  (cull terminates recursion, gain≥1 hits the depth budget). *Rationale:*
  doc 12 gives no numeric threshold, so a defensible default is chosen:
  `2^-12` (≈ −72 dBFS) is below single-contributor audibility yet terminates a
  scale-½ Droste chain by depth 12, far inside `max_depth = 64`; carrying it in
  the context lets a monitor tune it without a code change, and keeping the
  doc-05 budget as the backstop means a pathological scene (e.g. scale ≥ 1
  cycles) still terminates. *Rejected:* a hard-coded threshold (no monitor
  override); deriving the threshold from the working format's quantum (float32
  has none); removing the depth budget (unsafe for non-shrinking cycles). This
  is a made-here default, not a WBS "revisit" task.
- **D5 — Ship the Spatial mix core + static monitor seam now; defer the ring
  cull-tightening and live camera-follow to named leaves.** *Authority:* the
  2d effort; doc 12:258-263 (drain byte-exactness rests on the warmed closure
  covering the mixer's pulls, which a *superset* satisfies). *Rationale:* the
  drain is byte-exact whether the ring warms exactly the culled tree or a
  superset (the mixer only pulls what it doesn't cull, all resident), so the
  ring's attenuation-cull is a bounded warming-cost optimization, not a
  correctness gap — cleanly separable into `audio.spatial_fill_cull`. Live
  interactive camera-follow is runtime interactive wiring separable into
  `audio.spatial_camera_follow`; the static seam here is fully testable via
  export goldens. *Rejected:* doing all four in one 2d task (too broad, and
  bundles a perf optimization and interactive wiring with the semantics core);
  deferring device-path Spatial entirely (the ring must thread the context now
  or the device monitor cannot spatialize at all).

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-08.

- `src/contract/arbc/contract/content.hpp` — `Spatialization` struct with `k_sub_audible_atten` constant, `spatial_edge_atten`/`spatial_pan_gains` pure helpers; `std::optional<Spatialization> spatial{}` field appended to `AudioRequest`.
- `src/audio_engine/arbc/audio_engine/mix.hpp` + `mix.cpp` — `MixPolicy::Spatial` enumerator; Spatial branch in `mix_layer` performing per-edge attenuation, √-law constant-power pan, mono-collapse, and accumulated-attenuation sub-audible cull.
- `src/kind_nested/nested_content.cpp` — identical Spatial branch in `NestedContent::mix_child_layer` (L3 recursion twin), keyed off `AudioRequest::spatial` without naming `MixPolicy`.
- `src/audio_engine/arbc/audio_engine/lookahead.hpp` + `lookahead.cpp` — `LookaheadRingConfig::spatial` seed; `mix_block` threads it onto `AudioRequest` for device-path spatialization + camera post-scale.
- `src/runtime/arbc/runtime/export_monitor.hpp` + `export_monitor.cpp` — ctor `spatial` seed; `render_block_at` threads it + post-scales the top mix by camera's uniform scale-attenuation.
- `docs/design/12-audio.md` — concrete v1 Spatial model subsection (pan law, attenuation composition, sub-audible threshold, mono-collapse).
- `tests/audio_mix_goldens.t.cpp` — byte-exact goldens: hard-L/R/center pan + attenuation, nested attenuation-composition/mono-collapse, export-path Spatial.
- `src/runtime/t/export_monitor.t.cpp` — export Spatial golden and threaded==inline determinism with `silence_mixed()==0`.
- `src/audio_engine/t/lookahead.t.cpp` — ring Spatial threaded==inline golden.
- `tests/claims/registry.tsv` — three new claims: `12-audio#spatial-attenuates-by-composed-scale`, `#spatial-pans-by-composed-position`, `#spatial-sub-audible-cull-terminates-recursion`.
- Deferred: `audio.spatial_fill_cull` (ring descent-cull tightening) and `audio.spatial_camera_follow` (live viewport camera binding) registered as WBS leaves wired into `milestones.m6_audio`.
