# contract.audio_facet — Audio facet interface

## TaskJuggler entry

Back-link: `tasks/25-contract.tji:27-31`, `task audio_facet "Audio facet
interface"` under `task contract`. The verbatim note:

> AudioFacet: audio_extent, audio_stability, latency, render_audio over
> blocks; facet discovery via null-default virtual. Doc 12.

(The `Refinement:` back-link and `complete 100` are appended by the closer per
`tasks/refinements/README.md`.)

## Effort estimate

**1d** (`tasks/25-contract.tji:28`). An interface-surface task in the same
weight class as `snapshot_pins` (1d) and `operator_members` (1d): it adds one
abstract facet class, two by-value request/result structs, two small
`arbc::media` value types, one null-default discovery virtual on `Content`, and
one deferred-audio member on `PullService` — plus self-contained unit tests. No
samples are mixed and no engine machinery lands; the *use* of the facet
(pull-based mix, lookahead, clock mastering) is the `arbc::audio-engine` (L4)
stream that already exists in the WBS (`tasks/45-audio.tji`). The one non-trivial
edit is generalizing the shipped completion primitive to a template so audio
reuses it (Decision 3); the day covers the facet surface and its default-behaviour
tests, not the mix engine.

## Inherited dependencies

**Settled** (parent `task contract` deps, `tasks/25-contract.tji:7`):

- `model.editable_facet` (`6a799ae`, DONE 2026-07-05,
  `tasks/refinements/model/editable_facet.md`) — `StateHandle` and the
  content-state resolver a render/audio request pins.
- `surfaces.capabilities` (`62ff4df`, DONE 2026-07-05) — the backend surface
  contract; unrelated to audio directly but the parent gate.

**Sibling substrate (not a formal `depends`, but landed and relied on):**

- `contract.async_render` (`92c3d3b`, DONE 2026-07-05,
  `tasks/refinements/contract/async_render.md`) — `RenderCompletion` (the
  thread-safe one-shot settlement primitive this task generalizes to a template),
  `Exactness`, `RenderError`, and the one-code-path "return inline or settle via
  the completion" discipline `render_audio` mirrors verbatim.
- `contract.snapshot_pins` (`1da702a`, DONE 2026-07-05,
  `tasks/refinements/contract/snapshot_pins.md`) — `RenderRequest.snapshot`
  (`StateHandle`, index-only, trivially copyable) and the cheap-by-value request
  invariant `AudioRequest` must preserve. `AudioRequest.snapshot` resolves to the
  same `StateHandle`, exactly as `snapshot_pins` resolved the provisional
  `SnapshotToken*`.
- `contract.temporal_fields` (`957fd12`, DONE 2026-07-05,
  `tasks/refinements/contract/temporal_fields.md`) — `Stability`
  (`Static`/`Timed`/`Live`), `TimeRange`, and the `time_extent()` precedent
  `audio_extent()`/`audio_stability()` are the 1D-signal twins of.
- `contract.operator_members` (DONE 2026-07-05,
  `tasks/refinements/contract/operator_members.md`) — the `PullService` abstract
  interface and `ContentRef`. **Its Decision 3 explicitly deferred
  `PullService::pull_audio(ContentRef, const AudioRequest&, …)` to this task**,
  because `pull_audio` names `AudioRequest`, which this task owns
  (`operator_members.md:316-331`).

**Downstream (this task unblocks them):**

- `contract.audio_conformance` (`tasks/25-contract.tji:50-55`,
  `depends !audio_facet, !conformance_suite`) — extends `arbc-testing`'s
  facet-consistency and async families to `AudioFacet` and registers the
  *separate* claim `03-layer-plugin-interface#audio-facet-consistent`
  (`conformance_suite.md`, deferred there as an existing leaf).
- `audio.audio_types` (`tasks/45-audio.tji:6-10`, `depends
  contract.audio_facet`) and the whole `arbc::audio-engine` stream
  (`mix_engine`, `lookahead`, `device_monitor`, `export_monitor`, `latency`,
  `spatial_policy`) — the pull-based mix that *drives* this facet.
- `kinds.tone` (`tasks/55-kinds.tji:6-9`, `org.arbc.tone`, the audio-only
  "hello world" reference implementer) and `kinds.nested_audio`
  (`tasks/55-kinds.tji:43-47`, `depends … contract.audio_facet`, the recursion
  reference proof).

## What this task is

Audio is the pull contract on a 1D signal: the same *where/what-resolution/when*
request, over a time window at a sample rate instead of a region at a scale,
answering with a sample block instead of pixels (`docs/design/12-audio.md:7-34`).
A layer's audio comes from the **same content object** as its pixels — a video
clip is one `Content` with both facets, so its audio and video can never drift
under editing (`12:37-41`). This task lands the **contract surface** of that
design and nothing above it:

1. `class AudioFacet` — the optional audio facet: `audio_extent()`,
   `audio_stability()`, `latency()` (null-defaulted to zero), and
   `render_audio(request, done)` (`12:63-70`).
2. `struct AudioRequest` / `struct AudioResult` — the by-value request/result
   descriptors: window + working rate + working layout + target block + exactness
   + pinned snapshot in, achieved rate + exact out (`12:49-61`).
3. `Content::audio()` — the **facet-discovery null-default virtual**
   (`virtual AudioFacet* audio() { return nullptr; }`, `12:46`), the exact twin
   of the shipped `Content::editable()` (`content.hpp:330`). Purely visual content
   keeps the `nullptr` default and costs the audio engine nothing (`12:73-77`).
4. `ChannelLayout` and `AudioBlock` — the minimal `arbc::media` (L1) value types
   `AudioRequest` names: a channel layout (default stereo) and a non-owning
   float32 sample-block view, the audio analog of the media component's typed
   pixel-span views (`docs/design/17-internal-components.md:50`).
5. `PullService::pull_audio(ContentRef, const AudioRequest&, …)` — the audio pull
   `operator_members` deferred here, defaulted so existing `PullService`
   implementers are unchanged (Decision 5).

Every seam is optional/null-defaulted, so every existing (visual-only) kind is
behaviourally byte-identical. The mix engine, monitors, working-format
configuration, and conversions are **not** in scope — they are L4/L5 tasks that
depend on this surface.

## Why it needs to be done

The audio half of the compositor is gated on this interface existing.
`contract.audio_conformance` is hard-blocked (`!audio_facet`); the entire
`arbc::audio-engine` stream (`tasks/45-audio.tji`) roots at
`audio.audio_types → contract.audio_facet`; the reference kinds `org.arbc.tone`
and `org.arbc.nested`'s audio facet subclass this surface; and
`operator_members` left a documented hole (`pull_audio`) that only this task can
fill, since it owns `AudioRequest`. Landing the contract now — ahead of any mix
implementation — lets all of that proceed against a stable, testable surface,
exactly as `async_render`/`snapshot_pins`/`temporal_fields` landed the render
contract ahead of the compositor that drives it. Doc 12's scheduling decision is
**full audio in v1** (`12:207-221`): v1 is a complete A/V compositor, and this
facet is its foundation.

## Inputs / context

**Design docs (normative, doc 16):**

- `docs/design/12-audio.md:37-81` — "Audio as a content facet". The verbatim
  sketches this task implements:
  - `12:44-47` — `class Content { … virtual AudioFacet* audio() { return
    nullptr; } };` — the null-default discovery hook.
  - `12:49-56` — `struct AudioRequest { TimeRange window; uint32_t sample_rate;
    ChannelLayout layout; AudioBlock& target; Exactness exactness; const
    SnapshotToken* snapshot; };` (snapshot resolves to `StateHandle`, Decision 2).
  - `12:58-61` — `struct AudioResult { uint32_t achieved_rate; bool exact; };`.
  - `12:63-70` — `class AudioFacet` with `audio_extent()`, `audio_stability()`,
    `latency()` (defaulted `Time::zero()`), `render_audio(const AudioRequest&,
    std::shared_ptr<…>)`.
- `docs/design/12-audio.md:73-77` — "Purely visual content returns no facet and
  costs the audio engine nothing" — the observable invariant the primary claim
  pins.
- `docs/design/12-audio.md:26-29` — `Static`/`Timed`/`Live` stability "applies
  verbatim" to audio (a tone is `Static`, most audio `Timed`, a microphone
  `Live`); `audio_stability()` returns the existing `Stability` enum.
- `docs/design/12-audio.md:7-34` — the visual↔audio symmetry table:
  `achieved_rate` is the temporal-resolution analog of `achieved_scale`;
  `sample_rate` is the temporal "scale"; a block is the 1D surface.
- `docs/design/12-audio.md:182-188` — "Sync and latency": constant `latency()` is
  honored by the lookahead scheduler (an L4 concern); the contract carries the
  *value only* (default zero), exactly as `Deadline` carries a value the runtime
  enforces.
- `docs/design/12-audio.md:139-159` — the async discipline: audio never renders
  against a deadline, it renders *ahead* — so `render_audio`'s one-code-path
  (inline or settle-later) is the same primitive as `render`, and `Exact`
  (offline export) vs `BestEffort` (interactive lookahead) reuses `Exactness`.
- `docs/design/12-audio.md:207-221` — "Scheduling decision: full audio in v1";
  the reference implementers ride on `org.arbc.tone` (audio-only),
  `org.arbc.imageseq` (visual-only, proving facet-less costs nothing), and
  `org.arbc.nested` (both facets).
- `docs/design/13-effects-as-operators.md:69-89` — `PullService`; `13:80` names
  the audio pull `pull_audio` on the same interface.
- `docs/design/17-internal-components.md:53` — `arbc::contract` (L3) contents
  explicitly list **`AudioFacet`** and design doc **12**: declaring the facet here
  needs **no new component**.
- `docs/design/17-internal-components.md:50` — `arbc::media` (L1) contents:
  "channel layouts, typed pixel/**sample** span views", design docs 07 **and
  12** — the authoritative home for `ChannelLayout`/`AudioBlock`.
- `docs/design/17-internal-components.md:57` — `arbc::audio-engine` (L4) "pull-
  based mix, lookahead scheduler, block pipeline, clock mastering, latency
  pre-roll" — everything this task defers.

**Source seams:**

- `src/contract/arbc/contract/content.hpp:325-330` — `virtual Editable*
  editable() { return nullptr; }` and its doc-comment: the exact facet-discovery
  pattern `audio()` mirrors. `AudioFacet` sits beside `class Editable`
  (`content.hpp:213-225`) as a second optional-facet interface (pure virtuals,
  virtual dtor, non-copyable, protected default ctor).
- `src/contract/arbc/contract/content.hpp:230-360` — `class Content`;
  `Content::audio()` is added in the `--- optional facets ---` neighbourhood,
  before `editable()` (grouping the two facet hooks) or immediately after it.
- `src/contract/arbc/contract/content.hpp:153-185` + `src/contract/
  render_completion.cpp` — `class RenderCompletion`, the shipped one-shot
  settlement primitive this task generalizes to a `Completion<Result>` template
  (Decision 3).
- `src/contract/arbc/contract/content.hpp:27-46` — `Stability` (27-32),
  `Exactness` (37), `RenderError` (43-46): reused by the audio facet unchanged.
- `src/contract/arbc/contract/content.hpp:78-86` — `struct RenderRequest`
  (`StateHandle snapshot{}` at :82): the by-value descriptor `AudioRequest`
  mirrors, snapshot field included.
- `src/contract/arbc/contract/content.hpp:375-388` — `class PullService`; `pull`
  at :383-384. `pull_audio` is added here as a defaulted virtual (Decision 5).
- `src/contract/CMakeLists.txt` — `arbc_add_component(NAME contract … DEPENDS
  base media surface model)`: the `media` edge already exists, so
  `#include <arbc/media/audio_block.hpp>` needs no CMake change; the new test TU
  `t/audio_facet.t.cpp` is appended to `arbc_component_test`.
- `src/media/arbc/media/` (`pixel_format.hpp`, `color_space.hpp`,
  `surface_format.hpp`) and `src/media/CMakeLists.txt` — the new
  `arbc/media/audio_block.hpp` (`ChannelLayout` + `AudioBlock`) joins these;
  header-only (constexpr `channel_count`), no new `.cpp` required.
- `src/base/arbc/base/time.hpp:16,29` — `Time::zero()` (the `latency()` default)
  and `struct TimeRange` (the `audio_extent()` return and `AudioRequest.window`).

**Predecessor / sibling refinements:** `operator_members.md` (PullService,
`ContentRef`, the `pull_audio` deferral to this task, the L3-interface /
L4-implementation split, claims-anchor-to-doc-03 convention), `async_render.md`
(the completion primitive, the one-code-path discipline, TSan/stress scope for a
concurrency primitive), `snapshot_pins.md` (resolving a provisional type name —
`SnapshotToken*` → `StateHandle` — with no design-doc delta; cheap-by-value
request invariant), `temporal_fields.md` (`Stability`/`TimeRange`/`time_extent`
as the visual twins of the audio description methods).

## Constraints / requirements

1. **Optional / null-default facet — visual-only content is unchanged.**
   `Content::audio()` defaults to `nullptr`; a content that does not override it
   is audio-less, and the audio engine descends no audio path into it (`12:76`).
   Every existing kind (`org.arbc.solid`, `org.arbc.raster`, `org.arbc.imageseq`,
   `org.arbc.nested`'s visual facet) keeps the default and is byte-identical. This
   is the observable invariant the primary claim pins.
2. **Verbatim facet shape (`12:63-70`).** `AudioFacet` declares pure virtuals
   `std::optional<TimeRange> audio_extent() const`, `Stability audio_stability()
   const`, and `std::optional<AudioResult> render_audio(const AudioRequest&,
   std::shared_ptr<AudioCompletion>)`, plus a defaulted `Time latency() const {
   return Time::zero(); }`. Non-copyable, `virtual ~AudioFacet() = default`,
   protected default ctor — the `Editable` shape (`content.hpp:213-225`).
3. **`AudioRequest`/`AudioResult` stay cheap by-value descriptors.** As
   `RenderRequest`/`RenderResult`: no allocation, no refcount, no atomic on the
   request path. `AudioRequest.snapshot` is `StateHandle` (index-only, trivially
   copyable); `target` is `AudioBlock&` (caller-owned, zero-initialized, `12:54`);
   `layout` is a by-value `ChannelLayout`. `AudioResult` is `{ uint32_t
   achieved_rate; bool exact; }`, trivially copyable.
4. **One code path, sync and async (`12:139-159`, `03:80-84`).** `render_audio`
   returns a `RenderResult`-shaped `AudioResult` inline, **or** returns `nullopt`
   and settles later via `done` — the identical discipline as `render`. `Exact`
   (offline export) requests must be faithful; `BestEffort` (interactive
   lookahead) may report `achieved_rate < request.sample_rate` / `exact == false`.
   Audio never carries a `Deadline` (it renders ahead, not on a deadline,
   `12:33-34`), so the facet has no deadline field.
5. **`arbc::media` (L1) owns `ChannelLayout`/`AudioBlock`.** They are pure
   value/view types over `base` scalars (an enum + `float*`/`uint32_t` fields):
   they belong at the lowest honest level, which `17:50` names explicitly. Keep
   them minimal — `ChannelLayout` is `{ Mono, Stereo }` with a `constexpr
   channel_count()`; `AudioBlock` is a non-owning mutable float32 view (`samples`,
   `frames`, `layout`, `rate`) with a documented interleaved layout. Pooled/owned
   blocks and the working-format *configuration* are `audio.audio_types` /
   `arbc::cache` (Decision 6).
6. **Levelization — no new component edge.** `arbc::contract` is L3 and `17:53`
   already lists `AudioFacet`; the `media` edge already exists in
   `src/contract/CMakeLists.txt`. `AudioFacet`/`AudioRequest`/`AudioResult`/
   `pull_audio` reference only contract-local + `media` + `base` types. The mix
   engine (L4 `arbc::audio-engine`, `17:57`) is where the facet is *driven*;
   nothing in this task reaches above L3. `scripts/check_levels.py` gates.
7. **`RenderCompletion` behavior is preserved (Decision 3).** Generalizing the
   primitive to `Completion<Result>` with `using RenderCompletion =
   Completion<RenderResult>;` must leave every existing `RenderCompletion` call
   site (`content.hpp`, compositor, kinds) and `src/contract/t/async_render.t.cpp`
   compiling and passing byte-unchanged — the alias is the same type.
8. **Diff coverage ≥90% (doc 16).** Tests exercise the default path (a
   visual-only test `Content` whose `audio()` is `nullptr`) and the override path
   (an audio test `Content` exposing an `AudioFacet` double), covering every added
   line, including the templated completion's `AudioResult` instantiation and the
   defaulted `pull_audio`.

## Acceptance criteria

Test TU: `src/contract/t/audio_facet.t.cpp` (Catch2), self-contained test
`Content`s and an `AudioFacet` double — links no higher component.

**Claims-register growth.** Register in `tests/claims/registry.tsv` (TAB
`<claim-id>\t<description>`, gated both directions by `scripts/check_claims.py`),
enforced by a `// enforces:` tagged test:

- `03-layer-plugin-interface#audio-facet-optional` — A `Content` that does not
  override `audio()` returns `nullptr` (audio-less: facet discovery finds no
  audio and it costs the audio engine nothing), and a `Content` that exposes an
  `AudioFacet` returns it by pointer identity and answers `audio_extent()` /
  `audio_stability()` / `latency()`, with `render_audio` settling exactly once —
  inline or via the shared completion. (Anchored to doc 03, the `Content`
  interface where the discovery hook lives, per the sibling convention; doc 12
  motivates it. No such claim exists yet.)

**Deterministic unit assertions** (no golden bytes — nothing is mixed):

- *Default path*: a visual-only test `Content` (overrides none of the audio
  members) returns `nullptr` from `audio()` — the claim-enforcing case.
- *Override path — discovery*: a test `Content` returning a non-null
  `AudioFacet*` yields the same pointer on repeated calls (identity preserved).
- *Override path — description methods*: the facet double reports a configured
  `audio_extent()` (a `TimeRange`, or `nullopt` for a `Static` tone),
  `audio_stability()` (`Static`/`Timed`/`Live`), and a `latency()` (default
  `Time::zero()` when unoverridden).
- *`render_audio` one code path*: drive a **synchronous** facet (returns an
  `AudioResult` inline) and an **asynchronous** facet (returns `nullopt`, later
  `done->complete(result)`) through the identical render→settle→`take()` path and
  assert equivalent settlements; assert a `done->fail(RenderError::…)` surfaces as
  the expected `unexpected`. This is the audio twin of
  `03-layer-plugin-interface#render-inline-or-async`.
- *`AudioBlock`/`Audio`Request cheapness*: `static_assert(std::is_trivially_copyable_v<AudioRequest>)`
  and `…<AudioResult>` / `…<ChannelLayout>`; `channel_count(ChannelLayout::Stereo)
  == 2`.
- *`PullService::pull_audio` shape*: `static_assert(std::is_abstract_v<PullService>)`
  still holds; a test `PullService` that overrides only `pull` inherits the
  defaulted `pull_audio`, whose default settles the passed completion as
  `unexpected(RenderError::ResourceUnavailable)` exactly once (a service predating
  audio-engine answers "no audio pull" safely, never hangs the caller).

**Concurrency (explicit, so the closer scopes it here).** This task **touches a
concurrency primitive**: generalizing `RenderCompletion` to `Completion<Result>`.
TSan/stress coverage is required for the **new `AudioResult` instantiation**
(`AudioCompletion`) — a settle/`take` race asserting exactly-one settlement and no
torn payload, the audio twin of `async_render`'s completion stress test — and the
existing `RenderCompletion` stress test must remain green unchanged (the template
body is shared, so both instantiations are covered by structurally identical
tests). No other concurrency is added: the facet is a stateless interface; the mix
engine's threading and its TSan/stress coverage land with `audio.mix_engine` /
`audio.lookahead` / `audio.rt_safety` (`tasks/45-audio.tji`).

**No goldens / behavioral counters in this task.** No samples are mixed, so
byte-exact goldens do not apply (they land with `audio.export_monitor`'s
sample-exact mix). "Facet-less content issues zero audio renders" is a
behavioral-counter promise requiring the mix engine that counts — deferred to
`audio.mix_engine`.

**Deferred (owners already WBS leaves — no new task):**

- The **property-based public verification** of the facet — audio-extent honesty,
  `audio_stability` coherence, `render_audio` block continuity, and facet
  consistency over arbitrary content — and the claim
  `03-layer-plugin-interface#audio-facet-consistent`: `contract.audio_conformance`
  (`tasks/25-contract.tji:50-55`), which extends `arbc-testing`'s facet-consistency
  and async families. **No new leaf.**
- The **working audio format** (per-composition rate/layout config), **format
  conversions at edges**, and **nesting-boundary resample/remix**:
  `audio.audio_types` (`tasks/45-audio.tji:6-10`). This task lands only the
  minimal `ChannelLayout`/`AudioBlock` *vocabulary* the facet signature names.
- The **pull-based mix**, **lookahead scheduler**, **clock mastering**,
  **latency pre-roll**, **spatialization**, and the concrete `pull_audio`
  implementation: `audio.mix_engine` / `lookahead` / `device_monitor` /
  `latency` / `spatial_policy` (`tasks/45-audio.tji`). **No new leaf.**
- The **reference implementers**: `org.arbc.tone` (`kinds.tone`,
  `tasks/55-kinds.tji:6-9`), the "hello world" audio kind, and
  `org.arbc.nested`'s audio facet (`kinds.nested_audio`,
  `tasks/55-kinds.tji:43-47`). **No new leaf.**

**No under-registered follow-ups.** Every deferral above maps to an existing WBS
leaf; this task registers no new task.

## Decisions

1. **`Content::audio()` is a null-default discovery virtual, twinning
   `editable()`.** Add `virtual AudioFacet* audio() { return nullptr; }` beside
   `Content::editable()` (`content.hpp:330`).
   *Rationale:* the `note` mandates "facet discovery via null-default virtual" and
   doc 12's own sketch (`12:46`) writes exactly this, matching the shipped,
   conformance-suite-discovered `editable()` pattern (`conformance_suite`
   discovers facets through these virtuals and skips families a content does not
   expose). One idiom for all optional facets keeps the contract uniform and the
   suite's facet-skip logic unchanged.
   *Rejected — a capability-enum / `dynamic_cast`:* the codebase's established
   optional-capability idiom is the null-default virtual (`editable()`,
   `quantize_time()`); a second mechanism would be gratuitous and defeats the
   allocation-free discovery the suite relies on.

2. **`AudioRequest.snapshot` resolves the sketch's `SnapshotToken*` to
   `StateHandle` — no design-doc delta for the snapshot field.** The request pins
   the same index-only `model::StateHandle` a `RenderRequest` pins
   (`content.hpp:82`), defaulting to `k_state_none`.
   *Rationale:* doc 12 predates doc 14's purity refinement; `snapshot_pins`
   already resolved the visual side's provisional `SnapshotToken*` to `StateHandle`
   and made rendering a pure function of `(state, region, scale, time)` — audio is
   "the same contract on a 1D signal" (`12:9`), so `(state, window, rate, layout)`
   pins identically. Reusing `StateHandle` keeps `AudioRequest` trivially copyable
   and lets the export monitor's snapshot-consistent offline mix (`12:154-158`)
   work with the model's existing pin machinery.
   *Rejected — a distinct audio snapshot token:* audio and video share one content
   object and one revision space (`12:37-41,196-197`), so they must share one
   snapshot handle or a video clip could mix audio from a different frozen state
   than it renders pixels from — the exact drift the shared-facet design forbids.

3. **Generalize the shipped `RenderCompletion` to a `Completion<Result>` template;
   audio reuses it as `AudioCompletion = Completion<AudioResult>`.** Retrofit
   `class RenderCompletion` into `template <class Result> class Completion` with
   `using RenderCompletion = Completion<RenderResult>;` (byte-identical to today,
   every call site and `async_render.t.cpp` unchanged) and add `using
   AudioCompletion = Completion<AudioResult>;`. `render_audio`/`pull_audio` take
   `std::shared_ptr<AudioCompletion>`. `RenderError` is reused unchanged.
   *Rationale:* doc 12 (`12:69`) and `operator_members` both wrote the audio
   completion as the *same* completion machinery — but the concrete
   `RenderCompletion` carries a `RenderResult` payload and cannot carry
   `AudioResult`. Templatizing is the honest expression of "one shared completion
   primitive": the subtle release/acquire + single-settle CAS logic
   (`async_render`'s hard-won, TSan-verified core) is written and covered *once*,
   and both facets instantiate it. The change to shipped code is a mechanical,
   behavior-preserving class→template+alias refactor (the payload is already the
   only type parameter, stored as `expected<Result, RenderError>`).
   *Rejected — a standalone `AudioCompletion` class duplicating the atomics:*
   duplicates ~50 lines of concurrency-critical code and doubles the TSan surface
   doc 16 mandates for a concurrency primitive; a single template body TSan-covered
   for both instantiations is strictly better.
   *Rejected — keeping the literal `RenderCompletion` for audio:* a type error
   (payload mismatch); it cannot deliver an `AudioResult`.
   *Design-doc delta:* doc 12's sketch signature is updated to `AudioCompletion`
   with a one-line note (see delta below) — the sketch as written names a type
   that cannot carry `AudioResult`, so this is a correctness fix to the
   constitution, not merely a provisional-name resolution.

4. **`ChannelLayout` and `AudioBlock` land in `arbc::media` (L1), minimal.**
   `arbc/media/audio_block.hpp`: `enum class ChannelLayout { Mono, Stereo }` with
   `constexpr uint32_t channel_count(ChannelLayout)`, and `struct AudioBlock` — a
   non-owning mutable interleaved float32 view (`float* samples; uint32_t frames;
   ChannelLayout layout; uint32_t rate;`).
   *Rationale:* `17:50` names `arbc::media` the home of "channel layouts, typed
   … sample span views" and cites doc 12; `media` is an existing, allowed contract
   dependency, so this needs no new edge. `AudioBlock` is the audio analog of the
   media typed pixel-span views and of `Surface&` as a caller-owned render target
   (`12:54`, "zero-initialized"). Keeping the layout set to `{Mono, Stereo}`
   (doc 12's default is stereo) and the block a non-owning view is the smallest
   surface that types the facet honestly.
   *Rejected — `arbc::base` (L0):* audio format vocabulary is media's remit by
   `17:50`, mirroring how pixel formats live in `media`, not `base`.
   *Rejected — richer layouts / a pooled owned block now:* 5.1/ambisonic layouts
   and pooled block storage are engine concerns; adding them here is speculative
   surface with no v1 caller. The `enum` extends losslessly when `audio.audio_types`
   / the mix engine need more.

5. **`PullService::pull_audio` is a *defaulted* virtual, not pure.** Add
   `virtual void pull_audio(ContentRef input, const AudioRequest& request,
   std::shared_ptr<AudioCompletion> done)` with a default that settles `done` as
   `unexpected(RenderError::ResourceUnavailable)`.
   *Rationale:* two concrete `PullService` implementers already exist —
   `PullServiceImpl` (`src/compositor/arbc/compositor/pull_service.hpp:89`, L4) and
   the `NullPull` test double (`src/kind_nested/t/nested_meta.t.cpp:61`). A *pure*
   `pull_audio` would force edits to both (a compositor L4 file and a kind test),
   coupling this L3 interface task to code it should not touch and inverting the
   levelization. A defaulted virtual lands the stable signature now (so
   `audio.mix_engine` can override it) while leaving every existing implementer
   byte-identical — the same "extend the interface without breaking implementers"
   move the whole facet design makes. The default settles (never leaves `done`
   unsettled) so a caller never hangs; it fails safely because a service predating
   `arbc::audio-engine` genuinely has no audio pull.
   *Rejected — pure virtual:* forces out-of-scope edits and premature audio-pull
   implementation; the real override belongs to `audio.mix_engine` (L4).
   *Rejected — omit `pull_audio` entirely:* `operator_members` Decision 3
   explicitly deferred it here because this task owns `AudioRequest`; deferring it
   again would strand the audio operator/nested-mix stream with no pull seam.

6. **Claims anchor to doc 03; this task registers `audio-facet-optional`, not
   `audio-facet-consistent`.** The single new claim is
   `03-layer-plugin-interface#audio-facet-optional`.
   *Rationale:* the audio discovery hook is an addition to the doc-03 `Content`
   interface, so behaviour anchors to doc 03 per the explicit sibling convention
   (`operator_members` Decision 6, `03-layer-plugin-interface#facet-consistency`).
   The *property-based* consistency claim `…#audio-facet-consistent` is already
   owned by `contract.audio_conformance` (`conformance_suite.md`); this task pins
   only the interface-landing invariant (optional facet, discovery, one-code-path
   settle) for concrete doubles.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-07.

- `src/media/arbc/media/audio_block.hpp` — new: `ChannelLayout` (Mono/Stereo + `channel_count`) and `AudioBlock` non-owning float32 view (L1 arbc::media)
- `src/contract/arbc/contract/content.hpp` — templatized `RenderCompletion`→`Completion<Result>`; added `RenderCompletion`/`AudioCompletion` aliases, `AudioResult`, `AudioRequest`, `AudioFacet`, `Content::audio()` null-default virtual, `PullService::pull_audio` defaulted virtual
- `src/contract/render_completion.cpp` — added template definitions + explicit instantiations for `RenderResult`/`AudioResult`
- `src/contract/content.cpp` — `PullService::pull_audio` default implementation (settles `unexpected(ResourceUnavailable)`)
- `src/contract/t/audio_facet.t.cpp` — new: unit + TSan/stress test TU (claim `03-layer-plugin-interface#audio-facet-optional` enforced)
- `src/media/CMakeLists.txt`, `src/contract/CMakeLists.txt` — new header and test TU wired in
- `tests/claims/registry.tsv` — registered `03-layer-plugin-interface#audio-facet-optional`
- `docs/design/12-audio.md` — design-doc delta: `AudioCompletion` name fix per Decision 3
