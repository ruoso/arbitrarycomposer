# Refinement — `operators.fade`

## TaskJuggler entry

`tasks/50-operators.tji:9-13` — `task fade "org.arbc.fade"`, under
`task operators "Operators"` (50 — Effects as operators, doc 13). This is
the first refinement in the `operators/` area.

> note "Single-input operator, both facets (alpha + gain envelope),
> Timed-over-Static stability aggregation, identity() pass-through outside
> the envelope window. Doc 13."

## Effort estimate

`effort 2d`, `allocate team`.

## Inherited dependencies

The parent `task operators` declares
`depends compositor.pull_service, contract.operator_members`
(`tasks/50-operators.tji:7`); `task fade` adds none of its own. Every
predecessor is `complete 100`, so this task rests entirely on settled
seams.

**Settled predecessors this task builds on (all `complete 100`):**

- `contract.operator_members` (`tasks/25-contract.tji:33-37`) — the three
  operator-graph virtuals on `class Content`: `inputs()`
  (`src/contract/arbc/contract/content.hpp:579`), `map_input_damage()`
  (`:591`), `identity()` (`:598`), plus the abstract `PullService`
  interface (`:617-645`) with `pull()` (`:625-626`) and defaulted
  `pull_audio()` (`:640-641`). `ContentRef = Content*` (`:212`). Fade is
  the reference **single-input** operator over these seams.
  Refinement: `tasks/refinements/contract/operator_members.md`.
- `compositor.pull_service` (`tasks/35-compositor.tji:49-54`) — the
  concrete L4 engine `PullServiceImpl`
  (`src/compositor/arbc/compositor/pull_service.hpp:149`) behind the L3
  interface. Cache-first serve, worker-dispatched miss, snapshot + deadline
  carried **verbatim**, aggregate-revision keying, recursion-depth budget +
  cycle diagnostic. Fade pulls its input through this, never through
  `input->render()`. Claims already landed and re-used:
  `13-effects-as-operators#pull-is-cache-first` (`registry.tsv:137`),
  `#pull-inherits-snapshot-and-deadline` (`:138`).
  Refinement: `tasks/refinements/compositor/pull_service.md`.
- `compositor.operator_graph` (`tasks/35-compositor.tji:36-40`, pulled in
  transitively via `pull_service depends !operator_graph`) — the core-side
  reader of `inputs()`: `aggregate_revision` fold (fade's output cache
  key), `route_operator_damage` through `map_input_damage`, and
  `resolve_identity` (the `identity()` short-circuit that makes fade cost
  nothing at envelope == 1). Claims:
  `13-effects-as-operators#identity-plan-issues-no-operator-render`
  (`registry.tsv:136`), the three `05-recursive-composition#` graph claims
  (`:129-131`). Refinement: `tasks/refinements/compositor/operator_graph.md`.
- `contract.audio_facet` (`tasks/25-contract.tji:27-31`) — **now
  `complete 100`**, unlike when `kinds.nested` was written. `class
  AudioFacet` (`content.hpp:395-427`) with `render_audio()` (`:422-423`),
  `audio_extent()`, `audio_stability()`, `latency()` (defaulted
  `Time::zero()`, `:414`); facet discovery via `Content::audio()` (`:572`,
  null default). `AudioRequest` (`:370-383`), `AudioResult` (`:220-223`),
  `PullService::pull_audio` (`:640-641`). Because this landed, fade
  implements **both** facets in one task — the audio half is *not*
  deferred (contrast `kinds.nested`, which had to defer its audio to
  `kinds.nested_audio`). Refinement:
  `tasks/refinements/contract/audio_facet.md`.
- `contract.conformance_suite` (`tasks/25-contract.tji`, `complete 100`) —
  the public `arbc-testing` property suite
  (`arbc::contract_tests(factory, options)`,
  `testing/arbc/testing/contract_tests.hpp:201`) with the operator-graph
  family entry points `check_operator_damage_covers`,
  `check_operator_identity_faithful` (`contract_tests.hpp:172-188`). Fade's
  conformance gate.

**Pending (must not be assumed at implementation time):**

- `serialize.kind_params` (`tasks/60-serialize.tji:23-27`) and
  `serialize.sharing` (`:29-33`) — the `serialize()/deserialize()` param
  hooks and the core-owned `inputs` edge / `contents` table machinery are
  **not** `complete 100`. JSON round-trip of a fade node is therefore out
  of scope here (it lands with those tasks and M7). Fade defines a
  serialization-*ready* in-memory params model matching doc 13's shape
  (`docs/design/13-effects-as-operators.md:145-153`); it does not parse or
  emit JSON. See **Decisions**.
- `model.content_binding` (`tasks/10-model.tji:57`) — not `complete 100`.
  Production runtime auto-wiring of fade's attach-time injection
  (`PullService`, `Backend`) rides it; see the deferred follow-up
  `operators.fade_runtime_binding` under **Acceptance criteria**. Fade's
  own tests drive the attach seam manually, exactly as
  `tests/nested_conformance.t.cpp:85` does with inline `PullService` +
  `CpuBackend` doubles.

## What this task is

Implement `org.arbc.fade` — a single-input operator kind that attenuates
its input by a time-evaluated envelope on both facets: visual **alpha**
(a fade of picture) and audio **gain** (a coherent fade of sound), so a
video fade-in dims sound and picture together through one node
(`docs/design/13-effects-as-operators.md:24-27`). It is a `Content`
subclass (with an inner `AudioFacet`) that wraps one input, reports its
input edge through `inputs()`, pulls that input **only** through the
injected `PullService`, and produces its output by scaling the pulled
result by the envelope value at the request's time. Outside the fade
window — where the envelope is exactly 1 — it declares `identity()` on the
visual facet so the compositor serves the input's cached tiles with no
render, no copy, no cache entry
(`docs/design/13-effects-as-operators.md:59-65, 128`). It is `Timed` even
over a `Static` input, because its envelope depends on time
(`:93-95`). New L4 component `kind_fade`, mirroring
`kind_solid`/`kind_tone`/`kind_nested`.

## Why it needs to be done

Fade is the first reference operator kind, and it proves the operator
mechanism end-to-end over real pixels and samples: single-input pull,
both-facet transformation, `Timed`-over-`Static` aggregation, and the
`identity()` short-circuit that the compositor built but no shipped kind
yet exercises with real content (`operator_graph.md` tests use synthetic
operator doubles only; `operator_graph.md:167-171`). Downstream:

- `operators.crossfade` (`tasks/50-operators.tji:17`, `depends !fade`) —
  the two-input temporal-transition primitive, built on fade's per-input
  envelope machinery.
- `operators.operator_conformance` (`:23`, `depends !fade`) — adds
  operator suite entries (identity honesty, damage-mapping honesty, input
  pulls only via PullService) that need a real operator kind to run
  against.
- Milestone `m7_effects` (`tasks/99-milestones.tji:56-58`,
  `depends operators.fade, operators.crossfade,
  operators.operator_conformance, serialize.sharing`) — "Fade and crossfade
  over both facets with identity pass-through; operator graphs serialize.
  Doc 13 realized."

## Inputs / context

**Design docs (normative — doc 16's constitution rule):**

- `docs/design/13-effects-as-operators.md` — the governing doc.
  - Both-facet motivation and facet-selectivity (`:24-27`).
  - The operator contract members and the `identity()` doc-comment that
    names "a fade at envelope == 1" as the canonical pass-through
    (`:52-65`).
  - `PullService` — operators pull, never call `render()` directly
    (`:69-89`); metadata composes synchronously — "a fade is `Timed` even
    over `Static` input" (`:91-95`).
  - Region/scale/time dependencies: "a fade pulls the same region"
    (`:103-105`), evaluates its envelope at the request's time and pulls
    the input at that same time (`:112-113`); audio "a gain envelope
    multiplies in place" (`:117-120`).
  - Caching: output keyed by aggregate revision; `identity()`
    short-circuits both cache levels (`:122-128`).
  - Serialization shape (amends doc 08): `inputs` outside `params`,
    `params: { shape, in, out }` (`:140-153`) — the params model fade must
    be ready for.
  - Reference-kinds table — fade's authoritative one-line spec (`:167`).
- `docs/design/12-audio.md` — the facet model; audio-facet honesty and
  latency (a pure-gain fade declares zero latency).
- `docs/design/07-color-and-pixel-formats.md` — premultiplied linear-light
  RGBA working space (claim
  `07-color-and-pixel-formats#compositing-in-working-space`,
  `registry.tsv:28`); the reason an alpha fade is a single premultiplied
  scalar multiply (see **Decisions**).
- `docs/design/16-sdlc-and-quality.md` — testing taxonomy: byte-exact
  goldens (`:48-53`), behavioral counters incl. the literal "a fade at
  envelope=1 issues zero operator renders" example (`:54-62`), claims
  register (`:15-25`).
- `docs/design/17-internal-components.md:28` — L4 reference-kind row
  (`kind-solid kind-tone`); fade is a new L4 component depending only on
  `contract`.

**Source seams (real paths + current lines):**

- `src/contract/arbc/contract/content.hpp` — `render()` (`:525-526`),
  `RenderRequest` (`:83-91`, `target` is a caller-owned `Surface&`),
  `RenderResult` (`:93-119`, carries `achieved_scale`, `exact`,
  `achieved_time`), `AudioFacet` (`:395-427`), `render_audio()`
  (`:422-423`), `AudioRequest` (`:370-383`, `AudioBlock& target`),
  `AudioResult` (`:220-223`), `Content::audio()` (`:572`), `inputs()`
  (`:579`), `map_input_damage()` (`:591`), `identity()` (`:598`),
  `PullService::pull()` (`:625-626`), `pull_audio()` (`:640-641`).
- `src/surface/arbc/surface/backend.hpp` — `Backend::composite(dst, src,
  src_to_dst, opacity)` (`:37-40`, source-over on premultiplied alpha
  scaled by `opacity`), `clear()` (`:35`). Reached through `contract`'s
  transitive closure — no direct component edge to `surface`.
- `src/media/arbc/media/pixel_traits.hpp:17,25-33` — `WorkingPixel`,
  `premultiply()`; premultiplied storage is why alpha attenuation is one
  scalar multiply of all four channels.
- `src/media/arbc/media/audio_block.hpp:42-47` — interleaved float32
  `AudioBlock`; sample `f`, channel `c` at `samples[f * channels + c]`.
- `src/kind_nested/arbc/kind_nested/nested_content.{hpp,cpp}` — the closest
  template: an operator with an injected `PullService`/`Backend` attach
  seam. `compose_child_layer` (`nested_content.cpp:238`) builds a
  sub-`RenderRequest` carrying `snapshot`/`exactness`/`deadline` verbatim
  (`:297-298`), pulls (`:305`), and on inline settle composites at
  `layer.opacity` (`:325`); the inline-or-placeholder branch is `:306-318`.
  `mix_child_layer` (`:394`) does the audio twin: `samples[...] += gain * s`
  (`:603`). `inputs()`/`map_input_damage()`/`identity()` overrides at
  `nested_content.hpp:100-106`; attach seam at `:68`.
- `src/kind_solid/.../solid_content.hpp:20-35`,
  `src/kind_tone/.../tone_content.hpp:18-59` — minimal `Content`-subclass
  shape; tone shows the inner-`AudioFacet`-member idiom
  (`AudioFacet* audio() { return &d_facet; }`) fade should mirror.
- `src/compositor/arbc/compositor/counters.hpp:47-54` —
  `operator_renders()` / `note_operator_render()`, the behavioral proof
  surface for identity.
- `tests/nested_conformance.t.cpp`, `tests/contract_conformance.t.cpp:366-383`
  — the operator-conformance driver pattern (inline `PullService` +
  `CpuBackend`, `arbc::contract_tests(factory, {.operator_graph = true})`,
  granular `check_operator_*`).
- `src/kind_raster/t/raster_goldens.t.cpp`, `src/kind_tone/t/tone_goldens.t.cpp`
  — the frozen-`constexpr`-byte golden pattern (`memcmp`, no tolerance,
  `[.regen]` case).

## Constraints / requirements

1. **Levelization (doc 17, CI-enforced).** New component `kind_fade` at L4,
   `arbc_add_component(NAME kind_fade ... DEPENDS contract)` — `contract`
   is the only edge, exactly as `kind_solid`/`kind_tone`/`kind_nested`.
   `Backend`, `PullService`, `AudioFacet`, `Surface`, `AudioBlock` all
   arrive through `contract`'s transitive closure; fade must name no
   `compositor`, `runtime`, `backend_cpu`, or `surface` component directly.
2. **Pull discipline.** Fade pulls its input **only** via
   `PullService::pull` (video) / `pull_audio` (audio) — never
   `input->render()`. The sub-request carries `request.snapshot`,
   `request.exactness`, and `request.deadline` **verbatim** into the pull;
   never reset, never sub-budgeted (`pull_service.md` Decision 5;
   `doc 05:96-100`).
3. **Synchronous inline-or-placeholder.** `render`/`render_audio` are
   synchronous producers: fade can composite/mix only when its pull settles
   **inline** (`done->settled()`). On a worker-dispatched miss it cancels
   and shows placeholder for that frame (transparent target / silence),
   exactly as `nested_content.cpp:306-313`. It must not block or busy-wait.
4. **Both facets.** Visual `render()` attenuates premultiplied RGBA by the
   envelope; audio `render_audio()` attenuates gain by the same envelope.
   Facet-selectivity is not in scope — fade touches both.
5. **`RenderRequest` stays cheap by-value** (`operator_members.md`
   Constraint 3): `identity(const RenderRequest&)` and the pull path
   allocate nothing, add no atomic.
6. **`map_input_damage` covers** (normative, `content.hpp:584-590`,
   claim `03-layer-plugin-interface#operator-damage-covers`): fade neither
   moves nor inflates pixels, so it returns `rect` unchanged — the
   reciprocal of "pulls the same region." Over-approximation is sound;
   under-approximation is a bug.
7. **`identity()` honesty** (claim
   `03-layer-plugin-interface#operator-identity-faithful`): fade returns
   input index `0` for a request **iff** its envelope evaluates to exactly
   `1.0` at `request.time`, and `std::nullopt` otherwise. When it returns
   `0`, its rendered output must be bit-identical to input 0's output for
   the same request (the compositor serves input 0's tiles directly).
8. **Stability aggregation.** `stability()` and `audio_stability()` report
   `Timed` regardless of the input's stability (the envelope depends on
   time) — the `Timed`-over-`Static` rule (`doc 13:93-95`). `bounds()` and
   `time_extent()` pass the input's through unchanged (spatial/temporal
   identity); `latency()` stays `Time::zero()` (pure gain, no delay).
9. **Determinism.** The CPU render/mix path must be byte-exact for goldens
   (doc 16: fixed FP flags, ordered reductions) — no `Math.random`, no
   unordered accumulation.
10. **Attach seam.** Fade receives `PullService&` and `Backend&` at attach
    (mirroring `nested_content.hpp:68`); it owns neither. Tests inject
    inline doubles; production wiring is the deferred follow-up.

## Acceptance criteria

**Conformance (property suite):** a new cross-component driver
`tests/fade_conformance.t.cpp` (mirroring `tests/nested_conformance.t.cpp`)
wires a `testing::ContentFactory` over a `FadeContent` fronted by an inline
`PullService` + `CpuBackend`, then runs
`arbc::contract_tests(factory, {.operator_graph = true,
.snapshot_sensitive = false})` plus the granular operator families
`check_operator_damage_covers(factory, edit)` and
`check_operator_identity_faithful(factory, /*a time where envelope == 1*/)`.
Because `factory()->audio() != nullptr`, the suite's audio families
(purity, rate/time honesty) run automatically. This **re-asserts** (does
not re-register) via `// enforces:` tags:
`03-layer-plugin-interface#operator-damage-covers` (`registry.tsv:105`),
`03-layer-plugin-interface#operator-identity-faithful` (`:106`).

**Behavioral counter (identity):** a test binding a `CompositorCounters`
and driving fade through the compositor asserts
`CompositorCounters::operator_renders()` delta **0** when the envelope is
exactly 1 at the request time (identity short-circuit) and delta **1**
otherwise — re-asserting
`13-effects-as-operators#identity-plan-issues-no-operator-render`
(`registry.tsv:136`). Never a wall-clock assertion.

**Byte-exact goldens:** `src/kind_fade/t/fade_goldens.t.cpp` — frozen
`constexpr` byte tables (with the documented `[.regen]` regen case,
`memcmp`, no tolerance) for (a) a visual tile at a partial envelope value
(e.g. 0.5) over a known solid input, proving premultiplied-RGBA
attenuation, and (b) an audio block at a partial envelope over a known tone
input, proving per-frame gain attenuation. Tagged
`// enforces: 16-sdlc-and-quality#byte-exact-goldens` (`registry.tsv:42`).
These are the operator-output pixel goldens that `operator_graph.md`
explicitly deferred to "the real fade/crossfade kinds."

**New claims register entries** (`tests/claims/registry.tsv` +
`enforces:`-tagged tests; namespace `13-effects-as-operators#`):

1. `13-effects-as-operators#fade-attenuates-both-facets` — "org.arbc.fade
   scales its input's premultiplied RGBA (visual) and its samples (audio)
   by the same envelope evaluated at the request time; a fully-open
   envelope is a no-op and a fully-closed envelope yields transparent /
   silent output." Enforced by the goldens + a fully-closed transparency
   test.
2. `13-effects-as-operators#fade-timed-over-static` — "org.arbc.fade
   reports `Timed` stability on both facets even when its input is
   `Static`, because its envelope depends on time." Enforced by a stability
   test over a `Static` solid/tone input.
3. `13-effects-as-operators#fade-identity-at-open-envelope` — "org.arbc.fade
   `identity()` returns its single input iff the envelope evaluates to
   exactly 1.0 at the request time, and `nullopt` otherwise." Enforced by
   a unit test over envelope-window boundary times.

**Coverage:** CI gates ≥90% diff coverage on changed lines; tests ship in
this task.

**Deferred follow-up (closer registers in WBS):**

- `operators.fade_runtime_binding` — "org.arbc.fade runtime injection
  wiring", `effort 0.5d`, `allocate team`, `depends !fade,
  model.content_binding`. When the runtime instantiates an `org.arbc.fade`
  content, wire its attach injection (`PullService`, `Backend`) onto live
  services and tear down on release — closing the production wiring fade's
  tests drive manually. `note` cites this refinement as source-of-debt.
  Wire into milestone `m9_release` (`tasks/99-milestones.tji:68`), beside
  the existing `kinds.nested_runtime_binding` /
  `kinds.raster_runtime_binding`.

## Decisions

1. **New L4 component `kind_fade`, `DEPENDS contract` only — no doc 17
   delta.** *Rationale:* mirrors the shipped `kind_solid`/`kind_tone`/
   `kind_nested`, all L4-with-only-`contract`; `Backend`/`PullService`/
   `AudioFacet` are abstract `contract`-reachable interfaces injected at
   attach, so no new dependency edge is created. The doc 17 L4 row lists
   reference kinds illustratively (`kind-solid kind-tone`); `kind_nested`
   already ships un-enumerated, so adding a reference kind at an existing
   level with the established edge is not a new architectural seam.
   *Rejected:* a doc 17 amendment per new reference kind — the table is
   illustrative, and per-kind edits would churn the constitution without
   adding a constraint.

2. **Envelope semantics: two time-window ramps over `[0,1]`.** The envelope
   `E(t) ∈ [0,1]` is defined by `params { shape, in, out }` matching
   `doc 13:150`. `in = [t0, t1]` is the fade-**in** time window over which
   `E` ramps monotonically 0→1; `out = [t2, t3]` the fade-**out** window
   ramping 1→0; either may be `null` (absent). Piecewise: `E = 0` for
   `t < t0`; ramps 0→1 across `in`; `E = 1` between `in` end and `out`
   start; ramps 1→0 across `out`; `E = 0` for `t ≥ t3`. `shape: "linear"`
   is the v1 interpolation. **Both facets evaluate this one function.**
   *Rationale:* `[t0,t1]`/`[t2,t3]` as time intervals is the reading that
   makes a "fade window" and yields the doc's clean 1-second fade-in
   (`in: [0, 1.0]`, `out: null`); it makes the identity condition (`E == 1`)
   a well-defined interval. *Rejected:* reading `in` as `[start_time,
   target_value]` — leaves the ramp duration unspecified and gives no
   fade-*out* symmetry; a normalized param-time domain — needless indirection
   when requests already carry absolute `Time`.

3. **Linear is the only v1 shape.** *Rationale:* it is the only shape doc 13
   names, and it is sufficient for cuts-and-dissolves (the M7 goal). *Not a
   WBS deferral:* additional shapes (smoothstep, exponential) are promised
   by no design doc — adding them speculatively is YAGNI. `shape` parsing
   should reject unknown values (fail closed) so a later shape addition is
   an additive change.

4. **Visual attenuation = `clear` then `composite` at `opacity = E(t)`.**
   `render` clears the target transparent, then
   `backend.composite(target, pulled_input_tile, identity_transform, E(t))`.
   Because the working space is premultiplied
   (`07-color-and-pixel-formats#compositing-in-working-space`), source-over
   of a premultiplied source onto a transparent target at `opacity = E`
   yields exactly `src * E` — the alpha fade — reusing the existing L2
   `Backend::composite` seam (`backend.hpp:37-40`), the same primitive
   `nested_content.cpp:325` uses. *Rejected:* a hand-rolled per-pixel
   `visit_surface` scale loop — reinvents `composite`, adds a second
   deterministic pixel path to keep byte-stable, and buys nothing over the
   existing seam.

5. **Audio attenuation = per-frame gain multiply.** After `pull_audio`
   settles the input into `request.target`, fade multiplies each frame's
   samples in place by `E(t_frame)` — the envelope evaluated at that
   frame's time across the request window — over `frames * channels`
   samples (the in-place twin of `nested_content.cpp:603`). *Rationale:*
   per-frame evaluation produces a smooth ramp with no zipper artifacts and
   is the correct realization of doc 13's "a gain envelope multiplies in
   place" for a window that spans the ramp. *Rejected:* one scalar per
   window (`E` at the window midpoint) — audible stepping on any window that
   straddles the ramp. Latency stays `Time::zero()` (pure gain).

6. **`identity()` is exact-`E == 1`, visual-facet only.** `identity(request)`
   returns `0` iff `E(request.time) == 1.0` exactly, else `nullopt` — never
   at `E == 0` (there the output is transparent, **not** equal to input 0,
   so claiming identity would be unsound). The audio facet has no
   `identity()` hook in the contract; a full-open audio window still runs
   `render_audio`, but the gain-of-1 multiply is a cheap numeric no-op, and
   its output equals the input by construction. *Rationale:* matches the
   `identity()` doc-comment's "a fade at envelope == 1" and keeps the
   short-circuit sound; the visual/audio asymmetry is inherent to the
   contract (identity is defined over `RenderRequest`, not `AudioRequest`).

7. **Immutable construction params (not `Editable` in v1).** Fade's
   envelope params are fixed at construction, like `SolidContent`'s color
   and `ToneContent`'s frequency. *Rationale:* keeps fade a pure operator
   with no model-backed state, matching the leaf-kind precedent and
   deferring the entire editable-parameter-animation story. Keyframed /
   scrubbable envelopes are a design-level question (they need an animation
   model that does not yet exist) — surfaced to the parking lot, **not** a
   WBS task.

8. **Serialization is out of scope.** Fade defines a serialization-ready
   in-memory params struct matching `doc 13:145-153` but implements no
   JSON `serialize()/deserialize()` — that mechanism is
   `serialize.kind_params` (kind param hooks) and `serialize.sharing`
   (core-owned `inputs` edges + `contents` table), neither `complete 100`.
   *Rationale:* respects the stream split; fade landing before those tasks
   must not fork a private serializer. The params model is chosen to map
   cleanly onto the doc's `{ shape, in, out }` when those hooks arrive.

9. **Inner-`AudioFacet`-member idiom for both facets.** Fade holds a
   `FadeAudioFacet` member and exposes it via `AudioFacet* audio() { return
   &d_facet; }`, mirroring `ToneContent`/`NestedContent`. *Rationale:* the
   established both-facet shape; keeps the visual and audio code paths in
   one translation unit sharing the envelope evaluator.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-08.

- `src/kind_fade/arbc/kind_fade/fade_content.hpp`, `src/kind_fade/fade_content.cpp` — `FadeContent` operator (visual + audio facets): linear envelope, `Timed`-over-`Static`, `identity()` at E==1, pull-via-`PullService` discipline.
- `src/kind_fade/CMakeLists.txt` — new L4 component (`DEPENDS contract` only, mirroring `kind_solid`/`kind_tone`/`kind_nested`).
- `src/kind_fade/t/fade_envelope.t.cpp` — unit tests: envelope evaluation, identity boundary, Timed-over-Static stability, passthrough.
- `tests/fade_conformance.t.cpp` — cross-component conformance driver: `contract_tests` + `check_operator_damage_covers` + `check_operator_identity_faithful` + audio families; re-asserts `operator-damage-covers`, `operator-identity-faithful`, `identity-plan-issues-no-operator-render`, `byte-exact-goldens`.
- `tests/fade_goldens.t.cpp` — byte-exact visual (E=0.5 premult attenuation via `CpuBackend::composite`) and audio (per-frame gain ramp) goldens; placed in `tests/` (not `src/kind_fade/t/`) because `CpuBackend` is outside `kind_fade`'s `contract`-only include closure — matches `tests/nested_goldens.t.cpp` precedent.
- `tests/fade_identity_counter.t.cpp` — behavioral counter: `CompositorCounters::operator_renders()` delta 0 at E==1 (identity short-circuit), delta 1 otherwise.
- `src/CMakeLists.txt`, `tests/CMakeLists.txt` — wired new component and test targets.
- `tests/claims/registry.tsv` — three new claims registered: `fade-attenuates-both-facets`, `fade-timed-over-static`, `fade-identity-at-open-envelope`.
- Deviation: `fade_goldens.t.cpp` placed in `tests/` not `src/kind_fade/t/` — `check_levels.py` forbids cross-levelization imports in component tests; matches `nested_goldens.t.cpp` precedent.
- Deviation: Fade reports `achieved_time = request.time` and bounded `audio_extent` — both required by the conformance suite (Timed⇒achieved_time present; Timed audio⇒extent present).
- Deferred: `operators.fade_runtime_binding` registered in WBS (runtime injection wiring, `effort 0.5d`, wired to `m9_release`).
