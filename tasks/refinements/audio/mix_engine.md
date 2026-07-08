# audio.mix_engine — Pull-based mix engine

## TaskJuggler entry

Back-link: `tasks/45-audio.tji`, `task audio.mix_engine` ("Pull-based mix
engine"). This refinement expands that one-line WBS leaf. The `.tji` note:
"Additive mixing through the layer graph: gain/audible placement, time maps
applied as varispeed resampling (exact rational rates), facet-less content
costs nothing. Doc 12."

## Effort estimate

`3d` (from the `.tji`). The per-layer descent already exists in prototype
form inside `org.arbc.nested`'s audio facet (`mix_child_layer`,
`src/kind_nested/nested_content.cpp:394-567`); the work here is to (a) stand
up the `arbc::audio-engine` component (doc 17:28,57) with the standalone
composition mixer the monitors will drive, and (b) land the concrete
`PullService::pull_audio` in the one concrete `PullService` (compositor's
`PullServiceImpl`) so that mixer — and nested's already-shipped recursion —
pull real per-content audio through actual cache-first/worker machinery
instead of the `ResourceUnavailable` stub.

## Inherited dependencies

The parent `task audio` carries `depends contract.audio_facet,
timeline.transport`; `mix_engine` adds `depends !audio_types`, so it inherits
all three.

- **`audio.audio_types`** — *settled* (DONE 2026-07-07,
  `tasks/refinements/audio/audio_types.md`). Landed the typed working format
  the mixer reads and converts *toward*:
  - `struct AudioFormat { uint32_t sample_rate; ChannelLayout layout; }` +
    `k_working_audio` = `{48000, Stereo}` at
    `src/media/arbc/media/audio_format.hpp:17-28`.
  - `AudioFormat working_audio_format{}` on the composition record
    (`src/model/arbc/model/records.hpp:140`), resolved via
    `DocRoot::working_audio_format()` (`src/model/arbc/model/model.hpp:64-69`).
  - Its Constraint 5 and Decision "Conversion deferred to the mix engine"
    explicitly hand the resample/remix normalization here.
- **`contract.audio_facet`** — *settled* (DONE 2026-07-07). Landed the whole
  pull-audio vocabulary this task makes concrete:
  - `AudioFacet` (`src/contract/arbc/contract/content.hpp:261-293`),
    `AudioRequest` (`:242-249`), `AudioResult` (`:217-220`),
    `AudioCompletion` = `Completion<AudioResult>` (`:226`),
    `Content::audio()` null-default probe (`:438`).
  - `PullService` interface (`:483-507`) with `pull` pure (`:491-492`) and
    **`pull_audio` defaulted** (`:502-503`); the default stub settles
    `ResourceUnavailable` exactly once (`src/contract/content.cpp:19-26`),
    whose comment names "the L4 mix engine" as the real overrider.
- **`timeline.transport`** — *settled* (DONE 2026-07-07,
  `src/runtime/arbc/runtime/transport.hpp:34-93`). No type-level constraint
  here: the mix engine is a pure library over a pinned `DocRoot` and takes
  `AudioRequest.window` (a sampled `TimeRange`) by value; the transport/clock
  that produces those windows is a *monitor* concern in `arbc::runtime` (doc
  17:60,84-86) and belongs to the later `device_monitor`/`export_monitor`
  leaves, not this task.

## What this task is

Two same-task deliverables that together are "the pull-based mix":

1. **`arbc::audio-engine` (new L4 component, doc 17:28,57): the composition
   mixer.** A pure function — the audio analog of `compositor::render_frame`
   (`src/compositor/compositor.cpp:103-118`) — that, given a pinned
   `DocRoot`, a root composition id, a `ContentResolver`, a `PullService&`,
   and an `AudioRequest` (window + working rate/layout + zeroed target +
   exactness + snapshot), walks the composition's layers bottom-to-top and
   **additively** mixes each audible layer's contribution into the target:
   per layer it culls (facet-less / `!audible()` / `gain <= 0` / span
   non-overlap), computes the **varispeed** child rate from the layer's
   composed rational `time_map`, requests the layer's content audio at that
   composed rate through `pull_audio`, band-limit-reconstructs a below-rate
   child (`resample_audio`), **remixes** to the working layout, scales by
   `gain`, and sums — folding `achieved_rate`/`exact` honestly. This is the
   reusable core the export/device monitors drive and the spatial-policy leaf
   extends.

2. **Concrete `PullService::pull_audio` in `compositor`'s `PullServiceImpl`
   (doc 17:56).** The audio arm of `pull`: probe the 1D block cache first
   (`(content id, revision, block index, rate)`), serve a resident
   exact-fresh block with zero dispatch, otherwise dispatch the miss exactly
   once onto an injected audio worker seam — carrying the request's snapshot,
   exactness, and rate verbatim — and settle the `AudioCompletion` exactly
   once. Same cache-first / budget-backstop / single-settle machinery as
   `pull`, never rebuilt.

The **monitors, the lookahead ring, the device clock, the offline drive
loop, the Spatial policy, and latency pre-roll are all out of scope** —
each is an existing named leaf (see Decisions and Acceptance criteria). This
task is the mix core and the per-content audio pull it stands on.

## Why it needs to be done

`audio.mix_engine` is the hinge of the audio stream: `lookahead`,
`export_monitor`, and `spatial_policy` all `depends !mix_engine`
(`tasks/45-audio.tji:21,33,45`), and `device_monitor`/`latency`/`rt_safety`
sit behind those. None of them can be built until there is a reusable engine
that turns a composition + a window + a working format into a mixed block.
Today the audio path is half-wired: content kinds render audio
(`org.arbc.tone`), the nested kind mixes *its child composition* via a
prototype walk, and the block-cache/resampler vocabulary exists — but there
is (a) no top-level composition mixer a monitor can drive, and (b) no
concrete `pull_audio`, so `nested`'s `d_pull->pull_audio` (production) hits
the `ResourceUnavailable` stub. This task lands both, making "rendering is
recursion" real for audio (doc 12:201-208): a monitor mixes the root
composition through the same `pull_audio` that nested already calls for its
children.

## Inputs / context

**Governing design doc — doc 12 (normative, doc 16):**
- **The symmetry** (`docs/design/12-audio.md:11-21`) — mixing is **additive**
  (`:17`), `opacity`→`gain` (`:18`), sub-audible cull terminates recursion
  (`:19`), viewport→**monitor** (`:20`), tile cache→**block cache** (`:21`).
- **The asymmetry** (`:31-34`) — "a late block is a glitch … audio never
  renders on a deadline, it renders *ahead*." Load-bearing: the mixer pulls
  every layer through `pull_audio` (off-thread-capable) rather than calling
  `render_audio` inline.
- **Audio as a content facet** (`:37-92`) — `AudioRequest`/`AudioResult`/
  `AudioFacet` (`:49-70`); target is float32 **zero-initialized** (`:53`);
  "Purely visual content … costs the audio engine nothing" (`:87`); Placement
  gains `gain` (0..∞, **unclamped**) and `audible` (`:89-92`); temporal
  placement (span + time map) is **shared** with video.
- **Working format** (`:94-104`) — the mixer pulls at the composition's
  working rate/layout; "the nesting boundary converts (resample + remix),
  homogeneous trees pay nothing."
- **Rate maps, varispeed, pitch** (`:106-122`) — **varispeed is the
  default** (`:112-116`): "resample through the composed rational rate …
  well-defined at any nesting depth because composed rates are exact
  rationals (doc 11)." Time-stretch and reverse/negative rate are deferred
  (`:117-118`).
- **Spatialization** (`:123-148`) — the mix policy is a **monitor** choice,
  not model. **Flat is the default**: "contribution = gain × mix" (`:128-129`).
  Spatial (pan/attenuation, sub-audible cull) is the `spatial_policy` leaf.
  Flat-mode recursion termination: gain<1 converges, gain≥1 hits the doc-05
  depth budget (`:139-143`).
- **The engine** (`:150-190`) — monitors are runtime; the block cache is the
  tile cache with a 1D key (`:180-185`); damage is a local-time range
  (`:186-190`).
- **Recursion** (`:201-208`) — a nested composition's audio facet "mixes its
  children … exactly as its visual facet composites them"; the audio revision
  space is the visual one.
- **Scheduling** (`:218-232`) — "export monitor … second, device monitor +
  lookahead scheduler last," i.e. the mix core comes before its drivers.

**Doc 11 (exact rational time), doc 05 (recursion budgets):**
- Rates in time maps are exact rationals; composed time maps evaluate in
  rational arithmetic with **one** ties-to-even leaf rounding, never
  accumulated across depth (`docs/design/11-time-and-video.md:43-52,187-188`).
- Recursion terminates on the sub-pixel/sub-audible cull or the per-graph
  **recursion-depth budget** (`docs/design/05-recursive-composition.md:61-67`);
  budgets flow *through* nesting, never reset per pull (`:93-100`).

**Doc 17 levelization (CI-enforced):**
- `arbc::audio-engine` is **L4**, contents "pull-based mix, lookahead
  scheduler, block pipeline, clock mastering, latency pre-roll," deps
  "contract, cache (+ below)" (`docs/design/17-internal-components.md:28,57`).
  "(+ below)" includes `model` (in `contract`'s closure, `:53`), so the
  mixer may walk a `DocRoot` exactly as the compositor does.
- The `PullService` **interface** is `contract` L3 (`:53`); the **concrete
  implementation** is `compositor` L4 (`:56`). `audio-engine` and
  `compositor` are **L4 peers — no same-level edge** (`:41`), which forces
  where `pull_audio` lives (see Decisions).
- The cache is engine-agnostic — "tiles and blocks are the same machinery
  with different key shapes" (`:79-80`). Engines are libraries over pinned
  versions; drivers/monitors are runtime (`:84-86`).

**Code seams the implementation extends:**
- **Video precedent to mirror** — `compositor::render_frame`
  (`src/compositor/compositor.cpp:103-118`; walks `for_each_layer`
  bottom-to-top, clears target, delegates per layer) and `render_layer`
  (`:8-101`; the visibility/opacity cull `:10-12`, resolve, invert, region
  pull, sub-scale cull, single settle, accumulate). `PullServiceImpl`
  (`src/compositor/arbc/compositor/pull_service.hpp:89-136`) with the
  `RenderDispatch` worker seam (`:43-44`) and `PullConfig` (`:57-82`);
  cache-first probe + single dispatch (`src/compositor/pull_service.cpp:130-141,
  143-219`), the depth-budget backstop (`:70-76`), and the plan==render key
  assert (`:190`). `GraphBudget{max_depth = 64}` /`GraphDiagnostics`
  (`src/compositor/arbc/compositor/operator_graph.hpp:56-60,76-78`).
- **The prototype mixer to generalize** — `NestedContent::mix_child_layer`
  (`src/kind_nested/nested_content.cpp:394-567`) and
  `NestedAudioFacet::render_audio` (`:569-611`): silence-init, bottom-to-top
  `for_each_layer_in`, gain/audible/span/facet-less culls, varispeed child
  rate `sample_rate*den/num` recomputed per descent, `time_map.evaluate`,
  layout remix, `resample_audio` below-rate branch (`:483-532`), additive
  gain-scaled sum (`:539-553`), `achieved_rate`/`exact` fold (`:555-566`),
  all pulling through `d_pull->pull_audio` (`:470,520`).
- **Model** — `LayerRecord.gain` (`src/model/arbc/model/records.hpp:78`),
  `audible()` (`:91`), `span` (`:87`), `time_map` (`:88`);
  `CompositionRecord.working_audio_format` (`:140`);
  `DocRoot::for_each_layer_in`/`find_layer`/`find_composition`/
  `working_audio_format` (`src/model/arbc/model/model.hpp:49,51,64-69,83`).
- **Media** — `AudioBlock` interleaved-float32 view
  (`src/media/arbc/media/audio_block.hpp:42-47`), `channel_count` (`:21-29`);
  `resample_audio(const AudioBlock&, AudioBlock&)` **upsample-only**, 16-tap
  Blackman-Harris / 32-phase polyphase, byte-exact
  (`src/media/arbc/media/audio_resampler.hpp:7-33`).
- **Base** — `Time`/`flicks_per_second`
  (`src/base/arbc/base/time.hpp:11-20`), `TimeRange` half-open (`:29-54`);
  `Rational`, `TimeMap.evaluate` → `expected<Time,TimeError>`, `ComposedTimeMap`
  (`src/base/arbc/base/rational_time.hpp:36-118,126-156`).
- **Cache** — the 1D block-cache shape (`BlockKey` over the shared
  `KeyedStore`, `src/cache/arbc/cache/key_shapes.hpp:21,30,127`), enforced
  today by `tests/claims/registry.tsv:67`
  (`12-audio#block-cache-is-tile-cache-1d`). The prefetch ring
  (`src/cache/arbc/cache/prefetch.hpp:104,128`) is the `lookahead` fill
  driver, not this task.
- **Existing audio claims** to extend, not duplicate:
  `tests/claims/registry.tsv:67-72` (block cache, working format, tone rate,
  nested-mixes-through-pull, nested metadata, nested boundary resample) and
  `:146-147` (facet optional / consistent).

## Constraints / requirements

1. **New `arbc::audio-engine` component (L4), deps ⊆ {contract, cache,
   model, media, base}.** Create `src/audio_engine/` with the public header
   `arbc/audio_engine/mix.hpp` and impl, wired into the build and the CI
   levelization check (doc 17:41-44). It must **not** depend on
   `compositor`, `serialize`, `kind-*`, or `runtime` (all L4/L5 peers or
   above). This is the first code in the component; `lookahead`/monitors fill
   the rest of doc 17:57 later.
2. **The composition mixer is a pure function over a pinned `DocRoot`.**
   Signature mirrors `render_frame`: it takes `const DocRoot&`, the root
   composition `ObjectId`, a `const ContentResolver&`, a `PullService&`, and
   the `AudioRequest` (whose `target` it treats as the write surface). It
   reads membership and records only from the frozen pin (no wall clock, no
   transport, no mutation), and returns an `AudioResult{achieved_rate,
   exact}`. Determinism: two calls with equal (pin, composition, window,
   rate, layout) settle to **bit-identical** samples.
3. **Walk semantics mirror the nested prototype exactly** (so the top-level
   mix and nested's recursion are the same behavior at two levels):
   silence-init the target; iterate members bottom-to-top via
   `for_each_layer_in`; per layer skip when the content is unresolved, has no
   `audio()` facet, `!audible()`, `gain <= 0`, or the `span` does not overlap
   `window`; compute `child_rate = sample_rate * den / num` from the layer's
   `time_map.rate` (cull `num <= 0` — reverse/zero is deferred with
   time-stretch, doc 12:117-118); map the window start through
   `time_map.evaluate` (cull on `TimeError`, doc 11:52); request the child at
   its composition's working layout and the composed rate through
   **`pull_audio`, never `content->audio()->render_audio` directly**
   (doc 13 operator rule / doc 12 Decision 5); on a child that reports
   `achieved_rate < child_rate`, issue the second native-rate pull and
   `resample_audio` it up (never a nearest/hold); remix to the request layout
   (mono↔stereo per the baseline rules at `nested_content.cpp:542-550`); add
   `gain × sample` into `target`; fold `achieved_rate = min(...)` and
   `exact = conjunction`. The composed rate is recomputed **per layer from
   the per-edge rational, never accumulated** (doc 11:187-188).
4. **The mixer pulls every layer through `pull_audio`, not `render_audio`
   inline** — unlike video's `render_frame`, which renders root layers
   inline. Rationale is the doc-12 asymmetry (`:31-34,154-164`): all
   `render_audio` (arbitrary plugin code) must be able to run off the RT
   callback on workers via the pull/dispatch seam; the mixer never calls it
   inline. A `pull_audio` that does not settle inline (a worker miss) mixes
   silence for that layer this pass and cancels the completion, exactly as
   nested's descent shows the placeholder (`nested_content.cpp:471-476`);
   priming those blocks so the pass is all-hits is the `lookahead` leaf's job.
5. **Concrete `pull_audio` lands in `compositor`'s `PullServiceImpl`, the
   sole concrete `PullService` (doc 17:56).** It is forced there — not into
   `audio-engine` — because `pull` is pure-virtual and implemented in
   `compositor`, and an L4 peer cannot inherit it (doc 17:41). The override:
   block-cache-first probe on `(content id, revision, block index, rate)`;
   a resident exact-fresh hit completes synchronously with **zero dispatch**;
   a miss dispatches exactly once onto an injected audio worker seam
   (`AudioDispatch = std::function<void(Content*, const AudioRequest&,
   std::shared_ptr<AudioCompletion>)>`, the audio twin of `RenderDispatch`)
   carrying the request verbatim; the shared recursion-depth budget backstop
   applies (a depth-exceeded pull settles the placeholder, doc 05:61-67);
   the `AudioCompletion` settles **exactly once** on every path. `PullConfig`
   gains the `AudioDispatch` seam, the block-cache handle, and audio dispatch
   counters. Existing `pull` behavior and every existing `PullService`
   implementer stay byte-identical.
6. **Flat mix only; leave the Spatial seam.** Contribution is `gain × mixed`
   (doc 12:128-129). Structure the per-layer contribution behind a minimal
   `MixPolicy` parameter defaulting to `Flat` so `spatial_policy` inserts the
   pan/attenuation/sub-audible-cull branch additively, with the layer's
   composed transform already in hand — no signature churn later. Do **not**
   implement Spatial here.
7. **Recursion termination is the shared depth budget.** gain≥1 self-embedding
   terminates on `GraphBudget.max_depth` via the `pull_audio` backstop
   (doc 12:143); a gain<1 shrinking cycle also terminates under the same
   budget within one pass. True block-delayed feedback echo (doc 12:140-142,
   "cycle's time offset ≥ one block") needs prepared-block history and is the
   `lookahead` ring's concern, not this task.
8. **No design-doc delta.** doc 12 specifies the mix; doc 17 already lists
   `audio-engine` (L4) and assigns the `PullService` implementation to
   `compositor`. This task turns settled design into a work order. The two
   stub comments (`content.hpp:497`, `content.cpp:19-22`) that loosely say
   "arbc::audio-engine can override it" are prose the implementer refines
   in-place to name `PullServiceImpl` as the concrete home — a same-commit
   code-comment touch-up, **not** a design change. If implementation surfaces
   a genuine architectural gap, that is an escalation (return summary →
   parking lot), not a silent delta.
9. **Diff coverage ≥ 90%** on changed lines (doc 16 CI gate); tests ship in
   this task.

## Acceptance criteria

- **Claims-register growth** (`tests/claims/registry.tsv`, `12-audio#…`):
  1. `12-audio#mix-engine-mixes-layers-additively` — "The composition mixer
     walks a composition's layers bottom-to-top and additively sums each
     audible in-span layer's audio — pulled through `PullService::pull_audio`
     (never `render_audio` directly), scaled by `gain`, varispeed
     time-mapped through the layer's composed rational `time_map` at the
     composed rate, and remixed to the working layout — into a
     silence-initialized target; a composition of tones produces samples
     byte-identical to summing those tone renders directly, and
     `achieved_rate`/`exact` fold as `min`/conjunction over contributors."
     Enforced by an `enforces:`-tagged **byte-exact golden** test in
     `src/audio_engine/t/`.
  2. `12-audio#pull-audio-is-cache-first-single-settle` — "`PullService::
     pull_audio` serves a resident exact-fresh block from the 1D block cache
     with zero dispatch, dispatches a miss exactly once carrying the
     request's snapshot/exactness/rate verbatim, honors the recursion-depth
     budget, and settles the `AudioCompletion` exactly once on every path —
     the audio arm of `pull`." Enforced in the compositor `pull_service`
     tests (including the TSan case below).
  3. `12-audio#mix-engine-facetless-costs-nothing` — "Layers whose content
     has no audio facet, or that are `!audible()`/`gain<=0`/out-of-span,
     contribute exactly zero and issue **zero** `pull_audio` dispatches; the
     mixer issues exactly one dispatch per audible in-span layer with an
     audio facet." Enforced by the behavioral-counter test below.
- **Byte-exact goldens** (deterministic; no tolerances, doc 16):
  - A multi-tone composition (distinct frequencies, gains, and rate-r
    `time_map`s) mixed through the engine vs a hand-summed reference —
    byte-identical.
  - A **nested-of-tones** layer mixed through the engine driven by the *real*
    `PullServiceImpl::pull_audio` equals the same tones mixed flat at top
    level, byte-identical — the engine-level "rendering is recursion" for
    audio (the prototype's claim `:70` now exercised through real machinery,
    not a test double).
  - A below-rate child (a tone capped to a native rate < composed rate) is
    band-limit-reconstructed via `resample_audio` and the aggregate reports
    the native rate / `exact == false` — reusing the resampler's byte-exact
    guarantee (`registry.tsv:72`).
- **Behavioral-counter assertions** (performance-shaped "costs nothing"
  promise, never wall-clock, doc 16): drive the mixer over a composition of
  all-silent layers (facet-less / inaudible / out-of-span) and assert the
  `pull_audio` dispatch counter is **0**; over N audible in-span layers,
  assert it is **N** (one pull per audible layer, mirroring `registry.tsv:70`).
- **Concurrency / TSan** (doc 16 requires it for the audio engine
  explicitly): a TSan/stress test firing concurrent `pull_audio` for the same
  and for distinct `BlockKey`s through a real thread-pool `AudioDispatch`,
  asserting each `AudioCompletion` settles exactly once and the block cache
  sees no data race — the audio twin of the existing `pull` TSan case.
- **No new conformance family.** The mix engine is an engine, not a content
  kind or operator, so it adds no `arbc-testing` family; its tests instead
  drive `org.arbc.tone` and `org.arbc.nested` through the mixer. (The facet
  contract those kinds honor is already covered by
  `check_audio_facet_consistency`/`check_audio_async`,
  `testing/arbc/testing/contract_tests.hpp:140,148`.)
- **WBS gate.** After the closer adds `complete 100` and the `Refinement:`
  back-link to `tasks/45-audio.tji`, `tj3 project.tjp 2>&1 | grep -iE
  "error|warning"` is silent.
- **Registers no successor.** Every piece of implementable work this task
  does not do already maps to a named WBS leaf under `task audio`
  (milestone: the audio milestone in `tasks/99-milestones.tji`): the
  render-ahead ring + block-cache fill + damage re-mix → `audio.lookahead`;
  the device stream + clock mastering → `audio.device_monitor`; the offline
  sample-exact drive loop → `audio.export_monitor`; the Spatial pan/
  attenuation/sub-audible-cull branch behind the `MixPolicy` seam →
  `audio.spatial_policy`; declared-latency pre-roll → `audio.latency`;
  RT-safety enforcement → `audio.rt_safety`. This task creates no new leaf.

## Decisions

- **`pull_audio` implementation lives in `compositor`'s `PullServiceImpl`,
  not in `audio-engine`.** Forced by levelization: `PullService::pull` is
  pure-virtual and its concrete implementation is `compositor` (doc
  17:53,56); `audio-engine` is an L4 peer and "no same-level edge" (doc
  17:41) means it cannot inherit or extend that concrete service. Content
  holds a *single* `PullService` that must answer both `pull` and
  `pull_audio`, so the one concrete service owns both — and the cache-first/
  dispatch/budget plumbing is genuinely identical to `pull`'s, so it belongs
  beside it. `audio-engine` owns the *mix* (doc 17:57 "pull-based mix"); the
  per-content pull is shared plumbing. *Rejected:* an `audio-engine`
  `PullService` subclass (impossible without depending on `compositor`); a
  composite `PullService` assembled in `runtime` (doc 17:56 puts the concrete
  service in `compositor`, and this adds an L5 indirection the design does
  not ask for).
- **The mixer is a standalone function that re-expresses the walk, not an
  extraction of `nested`'s `mix_child_layer`.** `arbc::kind-nested` is L3 and
  cannot call the L4 engine, so the per-layer descent is *duplicated* between
  the engine (root composition, monitor-driven) and nested (child
  composition, recursion) — exactly as `compositor::render_frame` and
  nested's visual `compose_child_layer` duplicate the video walk today. The
  duplication is a thin cull loop; the heavy machinery (`PullService`, the
  block cache, `resample_audio`) is shared. *Rejected:* hoisting a common
  mixer into a lower component for both to call — it would either invert the
  levelization or manufacture a new shared component the design never named,
  to dedup a loop the video side already accepts as duplicated. (Not a
  WBS-deferrable "refactor" task either, per the refinement policy.)
- **The mixer pulls every layer through `pull_audio`; it never calls
  `render_audio` inline.** This is the deliberate audio/video asymmetry
  (doc 12:31-34,154-164): audio "renders ahead," so arbitrary plugin code
  must be dispatchable off the RT thread. Video's `render_frame` can render
  root layers inline because it degrades gracefully; audio cannot.
  *Rejected:* inline `render_audio` at the top level for a simpler
  synchronous path — it would put plugin code on whatever thread the monitor
  calls from, forfeiting the RT-safety the whole engine design exists to buy.
- **Flat mix only, behind a `MixPolicy` seam.** doc 12:127-129 makes the mix
  policy a *monitor* choice with Flat the default; Spatial is a whole leaf
  (`spatial_policy`) with pan/attenuation/sub-audible-cull. Landing the seam
  (default `Flat`) now keeps that leaf additive. *Rejected:* implementing
  Spatial here (out of scope, and it needs the composed-transform-as-listener
  machinery that is `spatial_policy`'s subject); hard-coding Flat with no
  seam (forces a signature change later).
- **Recursion terminates on the shared depth budget; block-delayed feedback
  is deferred to `lookahead`.** gain≥1 cycles hit `GraphBudget.max_depth`
  (doc 12:143, doc 05:61-67) via the `pull_audio` backstop — reusing the
  video mechanism, one dimension over. True feedback-echo convergence
  (reading a prior *block's* output, doc 12:140-142) needs the prepared-block
  ring, which is `lookahead`. *Rejected:* a bespoke audio depth counter
  (the graph budget already threads through the pull, doc 05:93-100);
  attempting block-history feedback here (no ring exists yet).
- **No block-cache *fill* here — cache-first read only.** `pull_audio` probes
  the existing 1D block cache (`registry.tsv:67`) and serves hits, but the
  prefetch-ring fill driver (`prefetch.hpp:104,128`) and the damage-forced
  re-mix of prepared blocks (doc 12:186-190) are `lookahead`'s. Audio is
  cheap to re-render (doc 12:183-185), so a miss simply dispatches; caching's
  real payoff (decode-behind-seek) arrives with the ring. *Rejected:* wiring
  the prefetch ring now — it belongs to the render-ahead scheduler leaf and
  would pull transport/horizon concerns into a pure per-window library.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-07.

- New `arbc::audio-engine` L4 component: `src/audio_engine/arbc/audio_engine/mix.hpp`, `src/audio_engine/mix.cpp`, `src/audio_engine/CMakeLists.txt`; wired into `src/CMakeLists.txt`.
- Concrete `PullService::pull_audio` in `PullServiceImpl`: `src/compositor/arbc/compositor/pull_service.hpp`, `src/compositor/pull_service.cpp`, `src/compositor/arbc/compositor/counters.hpp`.
- Contract touch-up refining `pull_audio` stub comment to name `PullServiceImpl`: `src/contract/arbc/contract/content.hpp`, `src/contract/content.cpp`.
- Windowed-sinc polyphase resampler extended for below-rate reconstruction: `src/media/audio_resampler.cpp`, `src/media/t/audio_resampler.t.cpp`, `tests/nested_audio_resampling_goldens.t.cpp`.
- Byte-exact goldens (multi-tone additive vs hand-sum; nested-of-tones == flat; below-rate resample reconstruction): `src/audio_engine/t/mix.t.cpp`, `tests/audio_mix_goldens.t.cpp`.
- Behavioral-counter test (0 dispatches on facet-less/silent, N on N audible layers): `src/compositor/t/pull_service.t.cpp` (added `pull_audio` + `direct_audio_dispatch` cases).
- TSan/stress concurrency test (same/distinct `BlockKey` concurrent pulls, single-settle): `tests/pull_audio_concurrency.t.cpp`; test targets wired in `tests/CMakeLists.txt`.
- Three new claims registered in `tests/claims/registry.tsv`: `12-audio#mix-engine-mixes-layers-additively`, `#mix-engine-facetless-costs-nothing`, `#pull-audio-is-cache-first-single-settle`.
