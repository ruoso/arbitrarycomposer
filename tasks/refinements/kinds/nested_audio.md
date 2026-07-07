# Refinement — `kinds.nested_audio`

## TaskJuggler entry

`tasks/55-kinds.tji:45-50` — `task nested_audio "org.arbc.nested audio facet"`,
under `task kinds "Reference kinds"` (55 — Reference kinds, docs 03/05/11/12/17).

> note "Implement org.arbc.nested's audio facet: aggregate the child
> composition's AudioFacet through the synthetic monitor, time-map + gain
> remix, sub-audible/depth-budget termination — the recursion reference proof
> for the audio facet. Docs 12/05.
> Source-of-debt: tasks/refinements/kinds/nested.md"

## Effort estimate

`effort 2d`, `allocate team`. The visual keystone (`kinds.nested`, 4d) already
landed the hard machinery — synthetic viewport, `PullService` reuse, aggregate
metadata memoization, two-level caching, cycle/Droste termination, snapshot
consistency, the attach seam. This task adds **one facet** on top of that
scaffold: override `Content::audio()`, aggregate the child layers' audio
through the same injected `PullService` (via `pull_audio`), apply each layer's
time map (varispeed) and gain (additive mix), and prove it against the audio
conformance families + byte-exact goldens. The two small model additions
(`gain`/`audible` placement fields) are mechanical twins of `opacity`/`visible`.

## Inherited dependencies

Own `depends`: `!nested, contract.audio_facet` (`tasks/55-kinds.tji:48`), plus
`contract.conformance_suite` from the parent `task kinds` (`:4`).

**Settled predecessors this task builds on (all `complete 100`):**

- **`kinds.nested`** — the shipped visual kind this task extends. `NestedContent`
  (`src/kind_nested/arbc/kind_nested/nested_content.hpp:48`,
  `src/kind_nested/nested_content.cpp`): the attach seam
  `attach(PullService&, Backend&, NestedResolver, const DocRoot&)`
  (`nested_content.hpp:64`, members `d_pull`/`d_backend`/`d_resolver`/`d_doc`
  at `:137-140`; `NestedResolver = std::function<Content*(ObjectId)>` at `:21`);
  the per-layer descent `compose_child_layer` (`nested_content.cpp:169-257`,
  sub-request snapshot/exactness/deadline forwarded verbatim at `:228-229`,
  pull through the injected service at `:236`); the child-facet fold
  `ensure_memo()` (`nested_content.cpp:26-119`: `ChildInput{content, transform,
  opacity}` gathered `:49-60`, `bounds` `:65-86`, `stability` `:88-101`,
  `time_extent` union `:103-118`) memoized on the pinned **aggregate revision**;
  `Memo`/`ChildInput` structs (`nested_content.hpp:109-124`). Nested's audio
  facet was **explicitly deferred to this task**
  (`nested_content.hpp:38-42`, and `tasks/refinements/kinds/nested.md:344-350,
  407-414`: "aggregate the child composition's `AudioFacet` through the
  synthetic monitor, time-map + gain remix, sub-audible/depth-budget
  termination … `depends kinds.nested, contract.audio_facet`. Milestone:
  `m6_audio`"). Refinement: `tasks/refinements/kinds/nested.md`.
- **`contract.audio_facet`** — the L3 `AudioFacet` vtable + value types in
  `src/contract/arbc/contract/content.hpp`: `AudioFacet` (`:261-293` — pure
  `audio_extent()` `:271`, `audio_stability()` `:275`, `render_audio` `:288-289`;
  defaulted `latency()` `:280`), `AudioRequest`
  (`:242-249` — `window`/`sample_rate`/`layout`/`target`/`exactness`/`snapshot`),
  `AudioResult` (`:217-220` — `achieved_rate`/`exact`), `AudioCompletion =
  Completion<AudioResult>` (`:226`), the discovery virtual `Content::audio()`
  (`:438`, `nullptr` default), and **`PullService::pull_audio`**
  (`:502-503`, defaulted to `unexpected(ResourceUnavailable)` at
  `src/contract/content.cpp:23-26`). Refinement:
  `tasks/refinements/contract/audio_facet.md`.
- **`contract.audio_conformance`** — the two audio families the umbrella
  `arbc::contract_tests` auto-runs when `factory()->audio() != nullptr`:
  `check_audio_facet_consistency` (`testing/arbc/testing/contract_tests.hpp:140`,
  impl `testing/contract_tests.cpp:448-541`) and `check_audio_async`
  (`contract_tests.hpp:148`, impl `contract_tests.cpp:544-626`), gated by
  `Options` toggles `audio_consistency`/`audio_async` (`contract_tests.hpp:76-77`),
  dispatched at `contract_tests.cpp:784-792`. Claims
  `03-layer-plugin-interface#audio-facet-consistent` (`tests/claims/registry.tsv:144`)
  and `#audio-facet-optional` (`:143`). Refinement:
  `tasks/refinements/contract/audio_conformance.md`.
- **`audio.audio_types`** — the L1 `arbc::media` audio vocabulary + per-composition
  working format: `ChannelLayout { Mono, Stereo }` + `channel_count()`
  (`src/media/arbc/media/audio_block.hpp:14-29`), the non-owning interleaved
  float32 `AudioBlock` target (`audio_block.hpp:42-47`, interleaving
  `samples[f * channel_count + c]` at `:36`), `AudioFormat` / `k_working_audio`
  (48 kHz stereo, `src/media/arbc/media/audio_format.hpp:17-28`), and the
  composition-carried `CompositionRecord::working_audio_format`
  (`src/model/arbc/model/records.hpp:127`, read via `DocRoot::working_audio_format()`
  `src/model/model.cpp:432-452`, set via `Transaction::set_working_audio_format`
  `model.cpp:656-675`). Claim `12-audio#composition-carries-working-audio-format`
  (`registry.tsv:68`). Refinement: `tasks/refinements/audio/audio_types.md`.
- **`kinds.tone`** — the shipped reference `AudioFacet` implementation this task
  mirrors for facet shape: `AudioFacet* audio() override`
  (`src/kind_tone/arbc/kind_tone/tone_content.hpp:35`, defined
  `src/kind_tone/tone_content.cpp:42`), inner `class ToneFacet final :
  public AudioFacet` (`tone_content.hpp:44-56`), `audio_extent()` → `nullopt`
  (`tone_content.cpp:53`), `audio_stability()` → `Static` (`:55`),
  `render_audio` (`:112-125`, honors any requested rate, `achieved_rate ==
  request.sample_rate`, `exact == true`, settles inline). Refinement:
  `tasks/refinements/kinds/tone.md`.

**Pending (must not be assumed at implementation time):**

- **`audio.mix_engine`** (`tasks/45-audio.tji:12-17`) — the concrete L4
  pull-based mix engine that lands the real `PullService::pull_audio` override
  (block-cache-first audio pull, worker dispatch, the recursion-depth backstop
  threaded across nested audio descents — the audio twin of `PullServiceImpl::pull`).
  `PullServiceImpl` (`src/compositor/arbc/compositor/pull_service.hpp:89`)
  overrides only `pull` (`:112`), **not** `pull_audio` — so today the injected
  service answers audio pulls with the defaulted `ResourceUnavailable`. This is
  a **sibling L4** `kind_nested` may not name (doc 17:59); nested_audio codes
  against the abstract `PullService::pull_audio` seam and drives its tests with
  an audio-routing `PullService` test double, exactly as `kinds.nested` drove
  the visual path with a stub before the runtime wired the real one, and exactly
  as `kinds.tone` proved the facet without the mix engine. Not a dependency.
- **`kinds.nested_runtime_binding`** (`tasks/55-kinds.tji:57-62`) — production
  auto-wiring of the attach injection. The audio facet reuses the **same**
  injected `PullService`/resolver/`DocRoot`, so it adds **no** new attach
  parameter and **no** new runtime-binding follow-up; the existing task already
  wires the shared injection.
- **`serialize.kind_params`** / model serialization — persistence of the new
  `gain`/`audible` placement fields rides the serialize stream that already owns
  span/time_map codec (L4, names the model; the reverse edge is banned). No edge
  is added here.

## What this task is

Implement the **audio facet** of `org.arbc.nested`, the recursion reference
proof of the audio facet (doc 12:224-228). `NestedContent` gains an override of
`Content::audio()` returning an inner `AudioFacet` (a `NestedAudioFacet`,
mirroring tone's `ToneFacet`) that aggregates the child composition's audio the
way the visual facet composites its pixels — doc 12's "nothing new to invent"
recursion rule (`12-audio.md:202-208`): "a nested composition's audio facet
mixes its children (through the child's working format, through the embedding's
time map and gain) exactly as its visual facet composites them. The synthetic
viewport of doc 05 gains a synthetic monitor."

Concretely, `render_audio(request, done)` walks the pinned child composition's
audible layers (the audio analog of the visual synthetic-viewport descent),
and for each layer whose resolved content exposes an `AudioFacet`:

1. maps `request.window` through the layer's `time_map` into child-local time
   (`local_time = (parent_time − in) × rate + offset`, doc 11:59-71), culling
   layers whose `span` does not cover the window — **varispeed** by requesting
   the child at the composed rational rate (doc 12:107-118);
2. pulls the child's audio through the **injected `PullService::pull_audio`**
   (never `child->audio()->render_audio()` directly — the doc-13 operator rule,
   audio twin), inheriting the request's `snapshot`/`exactness` verbatim;
3. **additively mixes** the settled child block into `request.target`, scaled by
   the layer's placement `gain` and channel-remixed to the request layout; and
4. aggregates `achieved_rate` (the minimum over contributing children) and
   `exact` (the conjunction) into the returned `AudioResult`.

Metadata (`audio_extent()`, `audio_stability()`) is aggregated from the
reachable child layers' audio facets and memoized on the child's **aggregate
revision**, extending the visual `ensure_memo()` fold. Termination reuses the
doc-05 depth-budget backstop threaded through the injected pull service (a
`gain < 1` Droste decays audibly and truncates at the budget; a `gain ≥ 1`
Droste yields silence + one diagnostic).

**Scope:** the audio facet over the **homogeneous working-audio-format** case
with children that honor the requested rate (tones, nested-of-tones — the
reference-proof shapes), plus a **baseline** deterministic nearest/hold fill for
the rare below-rate child. High-quality resampling is a named follow-up (see
Acceptance criteria). Spatial-mode spatialization and the sub-audible attenuation
cull are **monitor policy** (`audio.spatial_policy`), out of scope here; this
task ships the **Flat**-mode mix and its depth-budget termination.

Because doc 12:89-92 mandates the audio placement fields `gain` (0..∞) and
`audible` — the audio twins of `opacity`/`visible` — and `LayerRecord`
(`records.hpp:63-79`) does not yet carry them, this task lands them as their
first consumer (see Decisions).

## Why it needs to be done

`kinds.nested_audio` is the last kind-side leaf the **M6 milestone**
`m6_audio` ("M6: Audio") waits on: `depends audio.device_monitor,
audio.export_monitor, audio.rt_safety, audio.latency, audio.spatial_policy,
kinds.tone, contract.audio_conformance, kinds.nested_audio`
(`tasks/99-milestones.tji:49-51`). Doc 12's scheduling decision names three
reference proofs of the facet contract (`12-audio.md:224-229`): tone (audio-only
leaf, landed), the image-sequence kind (facet-less, landed), and
`org.arbc.nested` implementing **both** facets — "proving recursion." The visual
half shipped with `kinds.nested`; this task closes the audio half, making
`org.arbc.nested` the executable proof that the recursion machinery is truly
facet-agnostic: one synthetic-viewport/monitor descent, one `PullService`, one
aggregate revision, pixels *and* samples. Downstream, the mix engine, export
monitor, and device monitor all need a recursion-bearing audio kind to prove
budgets, snapshots, and time maps flow through nesting for audio; nested_audio
is that permanent regression anchor.

## Inputs / context

**Governing design-doc sections (normative, doc 16):**

- `docs/design/12-audio.md:202-208` — **Recursion** (the core rule): a nested
  composition's audio facet mixes its children through the child's working
  format, the embedding's time map, and gain, exactly as the visual facet
  composites; "the synthetic viewport of doc 05 gains a synthetic monitor;
  budgets flow through as in doc 05; the aggregate revision covers audio damage
  since it is the same revision space."
- `docs/design/12-audio.md:6-34` — the visual↔audio symmetry: `window +
  sample_rate` ↔ `region + scale`, `achieved_rate` ↔ `achieved_scale`,
  **additive mixing** ↔ source-over compositing, `gain` ↔ `opacity`, **Monitor**
  ↔ Viewport, sub-audible cull ↔ sub-pixel cull. Audio "renders ahead, never on
  a deadline" (`:31-34`) — no `Deadline` field in `AudioRequest`.
- `docs/design/12-audio.md:89-92` — placement gains the audio siblings `gain`
  (0..∞, **not** clamped at 1 — boosting is legitimate) and `audible` (analog of
  `visible`); the temporal placement (span + time map) is **shared** with video.
- `docs/design/12-audio.md:95-105` — working format: 48 kHz stereo default,
  float32 always; "the nesting boundary converts (resample + remix), homogeneous
  trees pay nothing."
- `docs/design/12-audio.md:107-122` — rate maps: the layer time map applies to
  audio ("a rate-½ layer plays its audio at half speed"); **Varispeed** (default)
  resamples through the composed rational rate — "well-defined at any nesting
  depth because composed rates are exact rationals." Time-stretch is deferred.
- `docs/design/12-audio.md:124-148` — spatialization is **monitor policy**:
  **Flat** (default, `contribution = gain × mix`) vs **Spatial** (camera-as-
  listener pan/attenuation + sub-audible cull). Flat-mode recursion termination
  (`:139-143`): "a recursive embedding with gain < 1 converges (each turn
  quieter — a feedback echo, well-defined when the cycle's time offset is at
  least one block); gain ≥ 1 cycles hit the doc-05 depth budget and diagnostic."
- `docs/design/05-recursive-composition.md` — the machinery audio inherits:
  synthetic viewport (`:22-26`), composed-result keyed on the child's
  **aggregate revision** (`:77-91`), cycle termination via the sub-pixel cull +
  per-request depth budget (`:55-75`), budgets flow *through* nesting, never
  reset (`:93-101`).
- `docs/design/11-time-and-video.md:59-74` — the time map:
  `local_time = (parent_time − in) × rate + offset`, `rate` a rational; outside
  its `span` a layer is culled. `:216-234` — nesting composes time maps in exact
  rational arithmetic, one rounding at the leaf, "never accumulate"; a temporal
  cycle with non-zero offset and `<1` scale terminates by the cull, the depth
  budget backstops the rest, same-instant self-reference reports the cycle
  diagnostic.
- `docs/design/17-internal-components.md:41-59` — `arbc::kind-*` is L4,
  `kind-nested` depends only on `contract` (+ its transitive closure:
  base/pool/media/surface/model); no same-level edge to `compositor` or the
  audio engine (CI-enforced by `scripts/check_levels.py`).

**Real source seams (paths + lines):**

- `src/contract/arbc/contract/content.hpp` — the audio surface to implement /
  consume: `AudioFacet` `:261-293`; `AudioRequest` `:242-249`; `AudioResult`
  `:217-220`; `AudioCompletion` `:226`; `Content::audio()` `:438` (override
  target); `PullService::pull_audio` `:502-503` (the audio pull seam;
  defaulted-`ResourceUnavailable` body at `src/contract/content.cpp:23-26`).
- `src/kind_nested/arbc/kind_nested/nested_content.hpp` +
  `src/kind_nested/nested_content.cpp` — `NestedContent` (`:48`), the attach
  seam (`:64`, members `:137-140`), the per-layer descent (`cpp:169-257`), the
  metadata memo (`cpp:26-119`, `Memo` `hpp:115-124`). The audio facet is
  currently **absent by omission** (documented at `nested_content.hpp:38-42`; no
  `audio()` override, no `throw`/`TODO`).
- `src/kind_tone/arbc/kind_tone/tone_content.hpp:35,44-56` +
  `src/kind_tone/tone_content.cpp:42,53,55,112-125` — the reference `AudioFacet`
  shape (inner facet class + inline settlement) to mirror.
- `src/media/arbc/media/audio_block.hpp:14-47`,
  `src/media/arbc/media/audio_format.hpp:17-28` — `ChannelLayout`,
  `channel_count()`, `AudioBlock`, `AudioFormat`/`k_working_audio`.
- `src/model/arbc/model/records.hpp:63-79` — `LayerRecord`
  (`content`/`transform`/`opacity`/`flags` with `k_layer_visible` `:41`/`span`
  `:75`/`time_map` `:76`); `:127` `CompositionRecord::working_audio_format`.
  `src/base/arbc/base/rational_time.hpp:103-108` — `TimeMap`
  (`local_time = (parent_time − in) × rate + offset`).
- `src/model/arbc/model/model.hpp` — `DocRoot::find_composition` `:51`,
  `find_layer` `:49`, `for_each_layer_in` `:83` (bottom-to-top membership),
  `working_audio_format()` `:69`; `Transaction::set_opacity` `:261`,
  `set_span` `:268`, `set_time_map` `:275` (the setter pattern the new
  `set_gain`/`set_audible` mirror).
- `testing/arbc/testing/contract_tests.hpp:140,148,76-77` +
  `testing/contract_tests.cpp:448-541,544-626,784-792` — the two audio
  conformance families, auto-run when `audio() != nullptr`.
- `tests/nested_conformance.t.cpp` — nested's existing conformance driver
  (audio families auto-skipped today; they activate once `audio()` is non-null).
  `tests/nested_goldens.t.cpp`, `tests/nested_cache.t.cpp`,
  `tests/nested_concurrency.t.cpp` — the golden / behavioral-counter / TSan
  patterns to extend for audio.

## Constraints / requirements

1. **Levelization unchanged (doc 17, CI-enforced).** `kind_nested` keeps its sole
   declared edge `DEPENDS contract`. The audio facet uses the same injected
   `PullService` (calling `pull_audio` on it) and the same `arbc::media` audio
   vocabulary already in `contract`'s closure. It names **no** `audio-engine`,
   `compositor`, or `backend-cpu` type. `scripts/check_levels.py` must pass
   unchanged.
2. **Pull, never call `render_audio` directly** (doc 13, audio twin). Every child
   layer's audio render goes through `d_pull->pull_audio(childContent,
   childRequest, done)`, carrying the outer request's `snapshot` and `exactness`
   **verbatim**. Nested rebuilds none of block-cache lookup, worker dispatch,
   aggregate revision, or the depth backstop — those are the service's, exactly
   as the visual facet relies on `pull`.
3. **Synthetic monitor = the child-layer audio descent.** For each audible child
   layer (`audible()` true and resolved content's `audio() != nullptr`): compose
   the request window through the layer's `time_map` (`local_time =
   (parent_time − in) × rate + offset`), cull if the mapped window lies outside
   the layer's `span`, request the child at the **composed rational rate**
   (varispeed), then additively mix the settled block scaled by the layer's
   `gain`, remixing `ChannelLayout` to `request.layout`. Recompute the composed
   rate/window from the per-edge rational each descent — **never accumulate**
   (doc 11:216-234).
4. **Metadata aggregated and memoized on aggregate revision** (doc 05:77-91,
   doc 12:202-208). Extend `ensure_memo()`: `audio_stability()` = `Static` iff
   every reachable child-layer audio facet is `Static` (children without audio do
   not contribute); `audio_extent()` = the union of the reachable child extents
   mapped through each layer's time map into parent time, or `nullopt` if any
   reachable child audio is `Static`-unbounded (`nullopt`). Both recompute only
   when the child's aggregate revision changes.
5. **Additive Flat-mode mix with gain.** `contribution = gain × child_block`,
   summed into `request.target` (doc 12:129-130). `gain` is read from the layer's
   new placement field (0..∞, unclamped); an `audible == false` layer contributes
   nothing (the audio `visible` cull). Layers with no audio facet are skipped at
   zero cost (doc 12:86-87).
6. **`achieved_rate` / `exact` honesty.** Return `AudioResult{achieved_rate =
   min over contributing children, exact = all children exact}`. A child honoring
   the request rate (tone) reports `achieved_rate == request.sample_rate`, so a
   nested-of-tones scene returns the request rate exactly and the mix is
   byte-exact. A below-rate child is filled to the target length by a **baseline
   nearest/hold** resample (deterministic) and its lower rate lowers the reported
   aggregate — high-quality resampling deferred (see Acceptance criteria).
7. **Depth-budget termination, no second limiter** (doc 05:55-75, doc 12:139-143).
   The injected pull service threads the per-request recursion-depth budget across
   nested `pull_audio` descents (the same backstop `pull` uses); nested adds
   none of its own. A `gain < 1` Droste cycle decays each turn (audibly correct
   echo) and truncates at the budget with inaudible residual; a `gain ≥ 1` cycle
   is caught by the budget, yielding a silent (all-zero) block and exactly one
   diagnostic naming the content path. Same-instant self-reference (cycle time
   offset below one block) is ill-defined → the existing cycle diagnostic.
8. **Snapshot consistency** (doc 05:71-75, doc 12:79-82). `request.snapshot` rides
   every `pull_audio` unchanged; nested reads child-composition membership at the
   pinned model version, so a Droste audio scene is self-consistent within a
   frame (the audio revision space is the visual one — `:208`).
9. **Thread-safety.** `render_audio` runs on worker threads (audio renders ahead
   on workers, never the device callback — doc 12:154-164). It reads only
   immutable-after-attach services and the pinned snapshot, so it keeps
   `render_thread_safe()`'s existing `true` (`nested_content.hpp:84`), mirroring
   the visual argument. The `Memo` mutex already guards the shared metadata memo.
10. **Homogeneous working format is the pay-nothing path** (doc 12:104). When the
    child composition's `working_audio_format` matches the request `(rate,
    layout)`, mix directly. Layout remix (mono↔stereo) is handled inline via
    `channel_count`; cross-rate resample uses the baseline fill of Constraint 6.
11. **New audio placement fields (doc 12:89-92), landed as first consumer.** Add
    `double gain{1.0}` and an `audible` flag (`k_layer_audible` bit, default set)
    to `LayerRecord`, plus `Transaction::set_gain`/`set_audible` mirroring
    `set_opacity`/the visible flag, with the same audio-damage/revision-bump
    semantics. The record stays standard-layout/fixed-size (doc 15). This is a
    shared prerequisite of `audio.mix_engine`, which consumes rather than re-adds
    them (see return summary).

## Acceptance criteria

**Contract conformance (the kind's own `arbc-testing` run).** Overriding
`audio()` makes `factory()->audio() != nullptr`, so nested's existing driver
`tests/nested_conformance.t.cpp` auto-runs both audio families in addition to
the visual + operator-graph families it already runs. The facet must pass:

- `03-layer-plugin-interface#audio-facet-consistent` (`registry.tsv:144`,
  `check_audio_facet_consistency`) — description methods idempotent + coherent, a
  window outside `audio_extent()` yields a silent block, and Static/Timed audio is
  a deterministic, **block-continuous** function of `(snapshot, window, rate,
  layout)` (a window split at an interior frame boundary concatenates
  bit-identically). Re-asserted over nested via an `enforces:` tag, **not**
  re-registered.
- `03-layer-plugin-interface#audio-facet-optional` (`registry.tsv:143`,
  `check_audio_async`, TSan lane) — `audio()` returns the facet by stable pointer
  identity; `render_audio` settles exactly once inline-or-async. Re-asserted, not
  re-registered.

**New claims-register entries (doc 12 recursion, nested-audio-specific).** Add to
`tests/claims/registry.tsv`, each pinned by an `enforces:`-tagged test:

- `12-audio#nested-mixes-child-audio-through-pull` — *`org.arbc.nested`'s audio
  facet aggregates its child composition's audio by pulling each audible
  child layer through `PullService::pull_audio` (never
  `child->audio()->render_audio` directly), additively mixing each contribution
  scaled by the layer's `gain` and time-mapped (varispeed) through the layer's
  `time_map`; a nested-of-tones scene produces samples byte-identical to mixing
  those tones directly at top level under the equivalent monitor* — the
  rendering-is-recursion identity for audio. Pinned by a **byte-exact golden**
  (below) and a **behavioral-counter** assertion (exactly one `pull_audio` per
  audible child layer; the outer `snapshot`/`exactness` carried verbatim).
- `12-audio#nested-audio-metadata-aggregates` — *`audio_stability()` is `Static`
  iff every reachable child-layer audio facet is `Static`, `audio_extent()` is
  the time-mapped union of reachable child extents (`nullopt` if any reachable
  child audio is unbounded), and both recompute only when the child's aggregate
  revision changes* — a **behavioral-counter** assertion on the recompute counter
  (zero recomputes across repeated queries at a stable aggregate revision, one
  re-key on a bump), extending the visual memo.

Re-asserted (a second `enforces:` test, not re-registered):
`05-recursive-composition#graph-walk-bounds-cycles` — a `gain ≥ 1` Droste audio
cycle terminates at the depth budget with one diagnostic + a silent block; a
`gain < 1` Droste decays to an inaudible residual within the budget with stable
samples. `05-recursive-composition#nested-boundary-budget-flows-through` — every
`pull_audio` carries the outer `snapshot`/`exactness` verbatim; a depth-1 nested
audio scene costs one pull per audible child layer.

**Byte-exact goldens (deterministic rendering, doc 16 default; portable because
tone's waveform avoids `std::sin`).** A golden suite under `src/kind_nested/t/`
(or `tests/nested_audio_goldens.t.cpp`, following the `tests/nested_goldens.t.cpp`
pattern):

- **Recursion identity** — a two-tone child mixed through `NestedContent` versus
  the same two tones mixed directly at top level (homogeneous 48 kHz stereo, unit
  gain), byte-identical.
- **Gain remix** — a child at `gain = 0.5` and a mono→stereo remix, pinning the
  weighted additive mix.
- **Varispeed** — a rate-½ child tone pitched down one octave via the composed
  rational rate, byte-exact.
- **Two-level nesting** — a composition-embedding-a-composition audio scene.
- **Droste termination** — a `gain < 0.5` cycle (finite, stable decaying echo)
  and a `gain ≥ 1` cycle (silent block + one diagnostic).

**Async / concurrency (doc 16 — concurrency-touching tasks scope coverage).**
`check_audio_async` (TSan lane, auto-run) covers nested's settle path through
`Completion<AudioResult>`. Additionally extend `tests/nested_concurrency.t.cpp`
(or a sibling) with a TSan/stress case rendering a nested audio scene through a
multi-worker `PullService` audio double, asserting deterministic samples and no
data race — nested's descent is on the frame/worker thread, only leaf audio pulls
dispatch further, and it holds only immutable-after-attach state.

**CI gates.** ≥90% diff coverage on changed lines; `scripts/check_levels.py`
(kind_nested still names only `contract`; no new edge); `scripts/check_claims.py`
both directions (the two new claims registered + enforced; the re-asserted claims
stay enforced).

**Deferred follow-ups (closer registers into the WBS / wires edges):**

- **`kinds.nested_audio_resampling`** (~2d) — replace this task's baseline
  nearest/hold cross-rate fill with a higher-order deterministic resampler (e.g.
  polyphase / windowed-sinc) for children that report `achieved_rate <
  request.sample_rate` (recorded-audio plugins, heterogeneous-rate nesting
  boundaries), each pinned by byte-exact per-filter goldens; keeps the mix seam,
  swaps the kernel — the audio analog of `kinds.raster_resampling_quality`.
  `depends kinds.nested_audio`. Milestone: `m6_audio` (closer registers in WBS).
- **No new runtime-binding leaf.** The audio facet reuses the same injected
  `PullService`/resolver/`DocRoot`; `kinds.nested_runtime_binding` already wires
  that injection. No `*_runtime_binding` successor is spawned.
- **Spatial-mode sub-audible cull** is already owned by `audio.spatial_policy`
  (`tasks/45-audio.tji:42-46`, "Spatial … sub-audible cull terminating
  recursion"); this task ships only the Flat-mode mix + depth-budget termination.
  No new leaf.

## Decisions

- **Reuse the whole visual scaffold; add one facet.** The synthetic descent,
  `PullService` reuse, aggregate-revision memo, attach seam, cycle backstop, and
  snapshot discipline are all landed by `kinds.nested`; the audio facet is the
  1D-signal twin over the identical scaffold (doc 12:202-208, "nothing new to
  invent"). `render_audio` re-expresses the per-layer descent for audio exactly as
  `render` does for pixels; `ensure_memo()` grows two audio fields. *Alternative
  rejected:* a separate audio-nesting component — nesting is one kind with two
  facets by design (doc 12:37-41, one content object), and the metadata memo /
  attach seam / child walk are shared state that a second component would have to
  duplicate.
- **Pull audio through `PullService::pull_audio`, never `render_audio` directly.**
  Doc 13's operator rule ("pulls go through the core `PullService`, never
  `input->render()`") has an explicit audio twin (`content.hpp:494-503`, doc
  12/13:80): `pull_audio` is the seam. This inherits block-cache-first serve,
  worker dispatch, and the recursion-depth backstop from the service, so nested
  rebuilds none of it. *Alternative rejected:* calling `child->audio()->
  render_audio()` directly — bypasses the block cache and the backstop, and would
  force nested to re-implement cycle termination.
- **The concrete `pull_audio` override is the mix engine's, not nested's.** The
  injected `PullService`'s `pull_audio` is defaulted to `ResourceUnavailable`
  today; the real override (block cache + backstop) lands with `audio.mix_engine`,
  a sibling L4 `kind_nested` may not name (doc 17:59). Nested_audio proves the
  kind against the abstract seam with an audio-routing `PullService` test double,
  exactly as `kinds.nested` proved the visual facet before the runtime wired the
  real `PullServiceImpl`, and as `kinds.tone` proved the facet with no mix engine
  at all. *Alternative rejected:* depending on `audio.mix_engine` — inverts
  levelization (a kind may not name a peer L4) and would serialize the two audio
  proofs that are designed to land independently.
- **Land `gain`/`audible` as first consumer.** Doc 12:89-92 mandates the audio
  placement fields, but `LayerRecord` does not carry them and no landed task adds
  them (`audio.mix_engine`'s note *assumes* "gain/audible placement"). They are
  two-field additive twins of `opacity`/`visible` — `records.hpp:40` already
  anticipates "remaining placement flags … the timeline tasks add" — with setters
  mirroring `set_opacity`/`set_span` and the same damage semantics. Nested_audio
  is their first and (until the mix engine) only consumer, so landing them here
  unblocks the reference proof without a scheduling round-trip, exactly as
  `kinds.raster` drove its `Editable`/`StateRefSink` model seam. *Alternative
  rejected:* a unit-gain-only mix that defers the fields — contradicts the task's
  named "gain remix" scope and disables the flat-mode `gain < 1` convergence
  termination story (doc 12:139-143), which is load-bearing. *Alternative
  rejected:* a separate model/timeline leaf for the fields — real, but it would be
  an orphaned two-field task blocking a leaf already scheduled ready; the fields
  ride their first consumer as placement twins, and the closer notes the mix
  engine consumes them. No design-doc delta: doc 12:89-92 already specifies them.
- **Flat mode only; Spatial sub-audible cull is monitor policy.** Doc 12:124-148
  makes spatialization monitor policy, not model — the Spatial attenuation and its
  sub-audible cull belong to `audio.spatial_policy`. In the default Flat mode
  (`contribution = gain × mix`), termination is the depth budget (all cycles) plus
  the natural decay of `gain < 1` (audibly correct echo). The task's deferral line
  names "sub-audible/depth-budget termination"; the sub-audible half resolves to
  the Spatial monitor cull (already owned by `spatial_policy`), the depth-budget
  half to this task's reuse of the doc-05 backstop. *Alternative rejected:* a
  nested-local audibility-floor cull — audio placement carries no per-request gain
  field (`AudioRequest` has none), so composed gain cannot thread down the pull
  the way composed scale threads the visual sub-pixel cull; the depth budget is
  the reliable, already-built terminator, and a second limiter is exactly what
  `kinds.nested` rejected.
- **Varispeed by requesting children at the composed rate; baseline resample for
  below-rate children; quality deferred.** The layer time map is applied by
  requesting each child at the composed rational rate over the mapped window
  (doc 12:107-118) — so a procedural child (tone) returns exactly the frames
  nested needs, placed 1:1, byte-exact at any depth (composed rates are exact
  rationals, doc 11:216-234). Only a child that *bottoms out* below the request
  rate needs a boundary resample; the core has no such kind today (tone honors any
  rate; codec content is a plugin, doc 10). A **baseline nearest/hold** fill keeps
  such a child correct-length and honest (aggregate `achieved_rate` drops),
  deferring the high-quality kernel to `kinds.nested_audio_resampling` — the audio
  analog of `kinds.raster_resampling_quality` deferring higher-order filters.
  *Alternative rejected:* shipping a windowed-sinc resampler here — real DSP with
  per-filter goldens that would swell the 2d facet task and is unexercised by the
  core reference scenes; deferring quality (not correctness) matches the raster
  precedent.
- **Reuse the aggregate-revision memo for audio metadata.** The aggregate revision
  bumps on any reachable change including audio damage (doc 12:208, same revision
  space), so it is the exact key for the audio metadata too; `ensure_memo()` gains
  `audio_stability`/`audio_extent` fields recomputed only on a revision change.
  *Alternative rejected:* recompute per query — violates doc 05's "Memoized"
  invariant and the static-hierarchies-are-cheap promise.

## Open questions

(none — all decided.) Two non-blocking WBS-shape observations are surfaced to the
closer in the return summary rather than encoded here: (1) the concrete
`PullService::pull_audio` override is assumed by `audio.mix_engine`'s note but not
explicitly scoped in its `.tji` — the closer should confirm the audio stream owns
it (nested_audio proves the kind against the abstract seam regardless); (2) the
`gain`/`audible` placement fields are a shared prerequisite of nested_audio and
`audio.mix_engine` — nested_audio lands them as first consumer, so the closer
should note the mix engine consumes rather than re-adds them.

## Status

**Done** — 2026-07-07.

- Implemented `NestedAudioFacet` in `src/kind_nested/arbc/kind_nested/nested_content.hpp` and `src/kind_nested/nested_content.cpp`: per-layer descent via the injected `PullService::pull_audio`, additive Flat-mode mix scaled by `gain`, varispeed via composed rational rate, depth-budget termination reused from doc-05 scaffold.
- Added `gain`/`audible` placement fields (`double gain{1.0}`, `k_layer_audible` bit) to `LayerRecord` in `src/model/arbc/model/records.hpp`, with `Transaction::set_gain`/`set_audible` setters in `src/model/arbc/model/model.hpp` and `src/model/model.cpp`, plus unit coverage in `src/model/t/records.t.cpp` and `src/model/t/temporal_placement.t.cpp`.
- Extended `ensure_memo()` with `audio_stability`/`audio_extent` fields memoized on the child's aggregate revision; both aggregate across reachable child-layer audio facets.
- Added unit tests in `src/kind_nested/t/nested_audio_meta.t.cpp`: metadata aggregation, mix culls, below-rate honesty, `set_gain`/`set_audible` round-trip.
- Added byte-exact golden suite `tests/nested_audio_goldens.t.cpp`: recursion-identity, gain/mono→stereo remix, varispeed, two-level nesting, Droste ×2 (gain<1 decay, gain≥1 silent+diagnostic).
- Added behavioral-counter and TSan concurrency suite `tests/nested_audio_concurrency.t.cpp`: one `pull_audio` per audible child, `snapshot`/`exactness` carried verbatim, no data race under multi-worker audio double.
- Activated `check_audio_facet_consistency` + `check_audio_async` conformance families over a nested-of-tones scene in `tests/nested_conformance.t.cpp`.
- Registered new claims `12-audio#nested-mixes-child-audio-through-pull` and `12-audio#nested-audio-metadata-aggregates` in `tests/claims/registry.tsv`; re-asserted `03-layer-plugin-interface#audio-facet-consistent`, `#audio-facet-optional`, `05-recursive-composition#graph-walk-bounds-cycles`, `#nested-boundary-budget-flows-through`.
- Updated `src/kind_nested/CMakeLists.txt` and `tests/CMakeLists.txt` for the new translation units and test targets.
- All 468 tests green; `check_claims` (145 claims) and `check_levels` pass; `kind_nested` still declares only `DEPENDS contract`.
- Tech-debt follow-up registered: `kinds.nested_audio_resampling` (~2d, replaces baseline nearest/hold cross-rate fill with higher-order deterministic resampler; depends `kinds.nested_audio`, milestone `m6_audio`).
