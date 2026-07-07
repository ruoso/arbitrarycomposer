# Refinement — `kinds.tone`

## TaskJuggler entry

`tasks/55-kinds.tji:6-9` — `task tone "org.arbc.tone"`, under
`task kinds "Reference kinds"` (55 — Reference kinds, docs 03/05/11/12/17).

> note "Audio-only content — the hello world of the audio facet: empty
> visual bounds, procedural tone at any requested rate. Doc 12."

## Effort estimate

`effort 1d`, `allocate team`. The smallest reference kind: a stateless leaf
with no editable state, no cache, no operator graph, and no mip/tile
machinery. The work is the `AudioFacet` implementation, a visual stub, the
conformance driver, and two goldens/claims.

## Inherited dependencies

From the parent `task kinds`: `depends contract.conformance_suite`
(`tasks/55-kinds.tji:4`). The `tone` leaf declares no additional `depends` of
its own today. The audio-facet surface it implements is already landed and
`complete 100` (below), so scheduling is unaffected — but the missing
`depends contract.audio_facet` edge on the leaf is flagged for the closer in
the return summary (a WBS-shape note, not encoded here).

**Settled predecessors this task builds on (all `complete 100`):**

- `contract.conformance_suite` — the public `arbc-testing` property suite
  (`arbc::contract_tests(factory, options)`,
  `testing/arbc/testing/contract_tests.hpp:201`), tone's conformance gate.
  Decision of that refinement: *each reference kind wires its own
  `arbc::contract_tests` run — that is the kind's task*, so the run is scoped
  here (`tasks/25-contract.tji:44-47`).
- `contract.audio_facet` — the L3 `AudioFacet` vtable and its value types in
  `src/contract/arbc/contract/content.hpp`: `AudioFacet`
  (`content.hpp:261-293`, pure `audio_extent()`/`audio_stability()`/
  `render_audio`, defaulted `latency()`), `AudioRequest`
  (`content.hpp:242-249`), `AudioResult` (`content.hpp:217-220`),
  `AudioCompletion = Completion<AudioResult>` (`content.hpp:226`), and the
  discovery virtual `Content::audio()` (`content.hpp:438`, `nullptr`
  default). (`tasks/25-contract.tji:27-30`.)
- `contract.audio_conformance` — the two audio families the umbrella suite
  runs when `factory()->audio() != nullptr`:
  `check_audio_facet_consistency` (`contract_tests.hpp:140`) and
  `check_audio_async` (`contract_tests.hpp:148`), plus the `Options`
  toggles `audio_consistency`/`audio_async` (`contract_tests.hpp:76-77`).
  Registered claim `03-layer-plugin-interface#audio-facet-consistent`
  (`tests/claims/registry.tsv:143`). (`tasks/25-contract.tji:51-54`.)
- `audio.audio_types` — the L1 `arbc::media` audio vocabulary:
  `ChannelLayout { Mono, Stereo }` + `channel_count()`
  (`src/media/arbc/media/audio_block.hpp:14-29`), the non-owning
  interleaved-float32 `AudioBlock` render target
  (`audio_block.hpp:42-47`, interleaving `samples[f * channel_count + c]`,
  `audio_block.hpp:36`), and the per-composition working format
  `AudioFormat` / `k_working_audio` (48 kHz stereo,
  `src/media/arbc/media/audio_format.hpp:17-28`). (`tasks/45-audio.tji:6-9`.)
- `contract.async_render` / `contract.snapshot_pins` /
  `contract.temporal_fields` / `contract.operator_members` — the visual
  `Content` vtable subset tone must still satisfy
  (`bounds`/`stability`/`time_extent`/`render`/`render_thread_safe`, the
  null/identity-default operator members), `src/contract/arbc/contract/content.hpp:328-468`.
- `contract.conformance_suite` also landed the L3 seam boundary tone honors:
  its own component TU may not `#include <arbc/testing/...>`; the conformance
  driver lives under `tests/` (exempt from the doc-17 include-hygiene scan),
  linking `arbc-testing` before `arbc`.

**Pending (must not be assumed at implementation time):**

- `serialize.kind_params` (`tasks/60-serialize.tji:23-27`) — the central
  `serialize()`/`deserialize()` kind-param hooks. Tone exposes a small param
  surface (frequency, amplitude) but does **not** wire its JSON codec here:
  serialization is the `serialize` stream's concern (L4, which names tone's
  `kind_id`; the reverse edge is banned), exactly as the sibling kinds leave
  it. No new leaf — `serialize.kind_params` consumes tone's `kind_id` and
  param surface when it lands.
- `model.content_binding` — not `complete 100`. Tone is a pure leaf: it
  registers **no** model-side sinks (no `Editable`, no `StateRefSink`/
  `RestoreSink`/`StateCostFn`), so unlike `kinds.raster`/`kinds.nested` it
  needs **no** `*_runtime_binding` follow-up. The conformance driver
  constructs the content directly, as solid/raster/nested drivers do.

## What this task is

Ship `org.arbc.solid`'s audio sibling: `org.arbc.tone`, an audio-only
reference content kind and the "hello world" of the audio facet (doc
12:226-232). Tone subclasses `Content`, reports **empty visual bounds**
(present-but-zero-area — culled from every visual pass), and returns a
non-null `AudioFacet` from `audio()`. The facet is a `Static` procedural
generator: it synthesizes a deterministic tone into the caller's `AudioBlock`
at **any requested sample rate**, always reporting `achieved_rate ==
request.sample_rate` and `exact == true` — the audio analog of raster's
"native res or below" scale honesty, one dimension down, where a procedural
source never bottoms out (doc 12:23-28). Its samples are a pure function of
absolute content-local time, so the facet is stateless, block-continuous, and
trivially thread-safe.

The reference `ToneFacet`/`AudioFixture` test double already exercised in
`tests/contract_conformance.t.cpp:234-297` (constructed as
`AudioFixture(std::nullopt, Stability::Static, async=false)`,
`:387-391`) is the exact behavioral shape this task formalizes into a shipped,
registrable in-lib kind.

## Why it needs to be done

Doc 12's scheduling decision (doc 12:219-232) makes full audio v1 scope and
names three reference proofs of the facet contract: `org.arbc.solid` gains an
audio sibling `org.arbc.tone` (this task), the image-sequence kind stays
visual-only (proving facet-less content costs nothing — landed by
`kinds.imageseq_plugin`), and `org.arbc.nested` implements both facets
(`kinds.nested_audio`). Tone is the minimal end-to-end existence proof that a
content with **only** an audio facet participates fully in the mix while
costing every visual pass nothing — the audio twin of solid. Downstream, the
audio engine (`arbc::audio-engine`), the export monitor, and the offline
sequence renderer all need a concrete audio-producing kind to drive; tone is
that fixture, and it is the permanent regression anchor for
`Content::audio()` discovery, rate-independent procedural synthesis, and the
audio conformance families.

## Inputs / context

**Governing design-doc sections (normative, doc 16):**

- `docs/design/12-audio.md:7-34` — the visual↔audio symmetry table:
  `region+scale` ↔ `window+sample_rate`, `achieved_scale` ↔
  `achieved_rate`, and the key line "procedural audio (a synth, a tone)
  renders at any requested rate; recorded audio bottoms out at its native
  rate" — tone is the procedural side. `Static`/`Timed`/`Live` "applies
  verbatim (a tone is `Static`)" (`12-audio.md:26-29`).
- `docs/design/12-audio.md:37-93` — audio as a content facet: the `Content`/
  `AudioFacet`/`AudioRequest`/`AudioResult` shapes, and 12:84-87 "Audio-only
  content … implements the audio facet and reports empty visual bounds; it is
  culled from every visual pass and participates fully in the mix" — the
  normative definition of tone's dual surface.
- `docs/design/12-audio.md:95-105` — working format: 48 kHz stereo default,
  samples always float32, conversions at the edges. Tone honors whatever
  `(rate, layout)` the `AudioRequest` names; the working format lives in the
  model and reaches tone only through the request.
- `docs/design/17-internal-components.md:28-59` — levelization: `kind-tone`
  is L4 (`17:28`), in the `arbc::kind-*` row (`17:59`, "solid, tone, …"),
  depending on `contract` (+ transitive closure) only.
- `docs/design/03-layer-plugin-interface.md` — reverse-DNS `kind_id` as
  persistent contract; the `Content` render one-code-path (inline vs async
  settlement) tone's `render_audio` reuses.

**Real source seams (paths + lines):**

- `src/contract/arbc/contract/content.hpp:261-293` — `AudioFacet` the kind
  implements; `:288-289` `render_audio` (inline-return-`AudioResult` OR
  `nullopt`+settle-`done` one-code-path); `:271` `audio_extent()`; `:275`
  `audio_stability()`; `:280` defaulted `latency()`.
- `src/contract/arbc/contract/content.hpp:242-249` `AudioRequest`
  (`window`/`sample_rate`/`layout`/`target`/`exactness`/`snapshot`),
  `:217-220` `AudioResult` (`achieved_rate`/`exact`), `:226`
  `AudioCompletion`, `:438` `Content::audio()`.
- `src/contract/arbc/contract/content.hpp:334-346` — the visual pure virtuals
  tone stubs: `bounds()` (`std::optional<Rect>`), `stability()`,
  `time_extent()`, and `:391-392` `render()`.
- `src/base/arbc/base/geometry.hpp:15-34` — `Rect`; `empty()` is
  `!(x0 < x1 && y0 < y1)` (`geometry.hpp:34`), so a default `Rect{}` is a
  present-but-empty rect. This is tone's `bounds()` — **not** `std::nullopt`,
  which means *unbounded* (the opposite signal).
- `src/media/arbc/media/audio_block.hpp:42-47` — `AudioBlock` target
  (`samples`/`frames`/`layout`/`rate`); interleaving `f * ch + c`
  (`audio_block.hpp:36`); `channel_count()` (`:21-29`).
- `src/kind_solid/arbc/kind_solid/solid_content.hpp` +
  `src/kind_solid/solid_content.cpp` + `src/kind_solid/CMakeLists.txt` — the
  reference-kind template tone mirrors: constructor taking kind params,
  overridden description + `render` virtuals, `static constexpr const char*
  kind_id`, and `arbc_add_component(NAME kind_solid … DEPENDS contract)`.
- `tests/contract_conformance.t.cpp:234-297,385-391` — the `ToneFacet`/
  `AudioFixture` double and the passing `Static` tone conformance case tone
  formalizes. `fill_block` (`:254-264`) shows the exact per-frame instant
  math `t = window.start.flicks + f * (flicks_per_second / sample_rate)` and
  the "silent outside extent" rule — the block-continuity discipline tone
  must satisfy.
- `tests/raster_conformance.t.cpp` / `tests/imageseq_conformance.t.cpp` +
  `tests/CMakeLists.txt` — the driver pattern (factory → `contract_tests`,
  `// enforces:` tags, `arbc-testing` before `arbc` on the link line).

## Constraints / requirements

1. **Dual surface.** `ToneContent : public Content` overrides `audio()` to
   return its `AudioFacet` and implements the visual pure virtuals as a
   culled stub: `bounds()` → `Rect{}` (empty, doc 12:85-87), `stability()` →
   `Stability::Static`, `time_extent()` → `std::nullopt`, `render()` fills
   `request.target` transparent (all-zero working-space pixels) and returns
   `RenderResult{request.scale, true, std::nullopt}`. Because bounds are
   empty the compositor culls tone from visual passes; the transparent render
   only has to satisfy the visual conformance families (any region is outside
   empty bounds → transparent output).
2. **Static procedural audio.** The facet reports `audio_extent()` →
   `std::nullopt` (defined for all time — a tone never starts or stops),
   `audio_stability()` → `Stability::Static`, `latency()` → the default
   `Time::zero()`.
3. **Rate independence (the doc 12:23-28 promise).** `render_audio` honors
   `request.sample_rate` for **any** rate and reports `achieved_rate ==
   request.sample_rate`, `exact == true` — under both `BestEffort` and
   `Exact`. A procedural source never degrades: there is no native rate to
   bottom out at, so `achieved_rate` never falls below the request. It
   settles **inline** (returns an `AudioResult`; the completion is unused),
   the trivial-content path.
4. **Sample purity / block-continuity.** Each frame's sample is a pure
   function of its **absolute content-local time**
   `t_f = request.window.start.flicks + f * (Time::flicks_per_second /
   request.sample_rate)` and the construction params — never of the frame's
   position within the block and never of accumulated phase. This is what
   makes the facet stateless, makes two identical requests bit-identical, and
   makes a window split at a frame boundary concatenate bit-identically to
   the single-window render (the `check_audio_facet_consistency` contract).
   All `channel_count(request.layout)` channels of a frame carry the same
   value (a mono tone spread across the layout).
5. **Byte-exact, platform-independent waveform.** Sample values are computed
   with pure IEEE-754 float arithmetic from an exact integer/rational phase —
   **not** `std::sin` (whose libm ULP result varies by platform and would
   force a golden tolerance). See Decisions. This keeps the golden byte-exact
   with no tolerance (doc 16 default) and portable across toolchains.
6. **Statelessness ⇒ thread-safety.** `ToneContent` holds only its immutable
   construction params; `render`/`render_audio` are pure. It keeps the
   `render_thread_safe()` default (`true`), so the worker pool renders its
   requests concurrently with no serialization queue.
7. **Levelization (doc 17, CI-enforced).** `kind-tone` is L4:
   `arbc_add_component(NAME kind_tone SOURCES tone_content.cpp PUBLIC_HEADERS
   arbc/kind_tone/tone_content.hpp DEPENDS contract)`, folded into `libarbc`
   as an OBJECT library. It may **not** include or link `arbc::audio-engine`
   or `arbc::compositor` (peer L4), nor `arbc::cache`/`arbc::backend-cpu`
   (L3) — the mix, block cache, and rate conversion are the engine's concern.
   Its only edge is `contract` (+ transitive base/pool/media/surface/model).
8. **Identity.** `static constexpr const char* kind_id = "org.arbc.tone";`
   (reverse-DNS persistent contract, doc 03).

## Acceptance criteria

- **Contract conformance (the kind's own `arbc-testing` run).** A new
  cross-component driver `tests/tone_conformance.t.cpp` defines a
  `testing::ContentFactory` returning a fresh `ToneContent`, and a `TEST_CASE`
  calling `arbc::contract_tests(factory)`. Registered in `tests/CMakeLists.txt`
  linking **`arbc-testing` before `arbc`** (`… Catch2::Catch2WithMain`). Since
  `factory()->audio() != nullptr`, the umbrella auto-runs both audio families
  — `check_audio_facet_consistency` and `check_audio_async` — in addition to
  the visual families (render purity, scale/time/bounds honesty, facet
  consistency, leaf-no-operator-graph). `// enforces:` tags re-assert the
  already-registered claims over tone without re-registering them:
  - `03-layer-plugin-interface#audio-facet-consistent`
    (`tests/claims/registry.tsv:143`) — the Static-tone determinism +
    block-continuity + silent-outside-extent contract.
  - `03-layer-plugin-interface#audio-facet-optional`
    (`tests/claims/registry.tsv:142`) — `audio()` returns the facet by
    stable pointer identity; `render_audio` settles exactly once.
  - `03-layer-plugin-interface#static-time-invariant` /
    `03-layer-plugin-interface#facet-consistency` — the visual stub's Static
    coherence.
- **New claim — rate independence.** Register
  `12-audio#tone-renders-at-any-requested-rate` in `tests/claims/registry.tsv`
  ("`org.arbc.tone` renders a procedural tone at any requested sample rate:
  `render_audio` at rates {8000, 22050, 44100, 48000, 96000} reports
  `achieved_rate == request.sample_rate` and `exact == true` under both
  `BestEffort` and `Exact`, never degrading below a native rate — the
  procedural side of doc 12's resolution/rate-independence symmetry"). Pin it
  with an `enforces:`-tagged test in `src/kind_tone/t/` (or the conformance
  driver) that renders tone across those rates and asserts the invariant. This
  is behavior the generic conformance suite does **not** cover (it runs at a
  single working rate), so it is a genuine new claim.
- **Byte-exact golden.** `src/kind_tone/t/tone_goldens.t.cpp` renders a fixed
  tone (fixed frequency/amplitude, a fixed `window`, `frames`, stereo,
  48 kHz) and asserts the produced float block byte-for-byte against a
  checked-in golden — deterministic rendering gets byte-exact goldens (doc
  16), portable because the waveform avoids `std::sin` (Constraint 5),
  following the `color.kernel_goldens` pattern. A second golden at a second
  rate (e.g. 44100) doubles as visible evidence of rate independence.
- **Async settle path coverage.** `check_audio_async` (TSan-covered family,
  run by the umbrella) exercises tone's inline settlement through the
  `Completion<AudioResult>` machinery under concurrent `complete`/`cancel`/
  `take`. Tone adds no new concurrency surface of its own (stateless,
  `render_thread_safe()` default), so no additional TSan target is scoped —
  the conformance family is the coverage.
- **CI gates.** ≥90% diff coverage on changed lines; `scripts/check_levels.py`
  (doc-17 dependency/include hygiene — `kind_tone` names only `contract`);
  `scripts/check_claims.py` both directions (the new claim is registered and
  enforced, the re-asserted claims stay enforced).

**Deferred follow-ups (closer registers into the WBS / wires edges):**

- **No new leaf.** Tone is a pure leaf and needs no `*_runtime_binding`
  successor (it registers no model sinks — see Pending). Its JSON param codec
  is consumed by the existing `serialize.kind_params`
  (`tasks/60-serialize.tji:23-27`), which names tone's `kind_id` and param
  surface when it lands — no edge is added here (levelization forbids
  `kind-tone → serialize`).
- **`kinds.dual_build`** (`tasks/55-kinds.tji:68-72`, "each in-lib kind also
  links into a CI-only shared library loaded via the extern C entry point")
  gains tone as one more in-lib kind it must cover; it already spans all
  in-lib kinds, so the closer wires no new leaf — this is a note that tone
  joins its set.
- **Built-in registry registration** of `org.arbc.tone` into the umbrella
  `arbc` target (doc 17:61, "built-in kind registration") is the umbrella's
  concern, exactly as for solid/raster/nested; tone provides `kind_id` + a
  factory and no new task is spawned.

## Decisions

- **Empty `Rect{}` bounds, not `std::nullopt`.** Doc 12:85-87 says audio-only
  content "reports empty visual bounds"; `Rect::empty()`
  (`geometry.hpp:34`) makes a zero-area rect the culled signal, whereas
  `std::nullopt` means *unbounded* (infinite visual extent — the exact wrong
  answer for audio-only content, which would then never be culled). *Alternative
  rejected:* `nullopt` bounds — it reads as "paints everywhere" and defeats the
  "costs every visual pass nothing" guarantee.
- **`Static` stability, `nullopt` audio_extent.** A tone plays identically for
  all time with no quantization and no hidden time-varying state, so it needs
  no `achieved_time`/extent dimension in the block-cache key — precisely
  doc 12:26-29's classification ("a tone is `Static`") and the passing
  `AudioFixture(nullopt, Static, …)` conformance case. `Static` does **not**
  mean "constant samples": the samples still vary with absolute time; it means
  the facet is a fixed, deterministic, unquantized function of `(window, rate,
  layout)`, which is what `check_audio_facet_consistency` verifies. *Alternative
  rejected:* `Timed` with a bounded `audio_extent` — a tone has no natural
  start/end, and a bounded extent would falsely silence it outside a window.
- **Procedural synthesis, `achieved_rate == request.sample_rate` always.** The
  tone is the "renders at any requested rate" side of the doc-12 symmetry: it
  synthesizes directly at the requested rate and never reports a lower
  achieved rate, the audio analog of raster reporting `achieved_scale <
  request.scale` past native — except a procedural source has no native
  ceiling. *Alternative rejected:* pinning a "native" rate and resampling —
  that is recorded-audio behavior and the mix engine's job (rate conversion
  toward the working format lives in `arbc::audio-engine`), not a procedural
  generator's.
- **Byte-portable waveform, not `std::sin`.** Samples are computed from an
  exact integer flick phase reduced to a fractional cycle, then evaluated with
  pure IEEE-754 float arithmetic (e.g. a fixed parabolic sine approximation
  `y = amplitude · (B·x + C·x·|x|)` over the reduced phase, or an equivalent
  project-owned deterministic evaluator) — so the golden is byte-exact with no
  tolerance and identical across toolchains. *Alternative rejected:*
  `std::sin` — libm results differ by ULPs across platforms/versions, which
  would force a golden tolerance; doc 16 makes tolerances the justified
  exception, and there is no justification here when a portable closed form
  exists. *Alternative rejected:* the double's integer-derived non-sinusoidal
  ramp — it satisfies determinism but is not audibly a tone; the shipped
  reference kind should sound like one.
- **Inline settlement, stateless, thread-safe by default.** `render_audio`
  returns its `AudioResult` inline (the trivial-content path, mirroring
  `SolidContent::render`) and holds no state, so it keeps
  `render_thread_safe() == true` and adds no serialization queue. *Alternative
  rejected:* async settlement — there is no work to defer; inline is the
  honest, simplest path and still flows through the same `Completion`
  primitive the async family verifies.
- **In-lib OBJECT library, `DEPENDS contract` only.** Tone carries no codec or
  external dependency, so it folds into `libarbc` like solid/raster/nested
  (contrast `kinds.imageseq_plugin`, out-of-lib for its decode dep). *Alternative
  rejected:* naming `audio-engine`/`cache` to "produce blocks" — a banned
  same-level (L4→L4) / upward edge; the kind produces samples into the
  caller-owned `AudioBlock` and knows nothing of the mix.

## Open questions

(none — all decided.) One non-blocking WBS-shape observation is surfaced to
the closer in the return summary rather than encoded here: the `tone` leaf's
`.tji` declares no `depends contract.audio_facet` (it inherits only
`contract.conformance_suite` from the parent), though the audio-facet surface
is what it implements. All the real predecessors are already `complete 100`,
so scheduling is unaffected; the closer may add the explicit edge for
provenance.

## Status

**Done** — 2026-07-07.

- Shipped `org.arbc.tone` as an L4 OBJECT library in `src/kind_tone/` (header `arbc/kind_tone/tone_content.hpp`, implementation `tone_content.cpp`, build `CMakeLists.txt`) folded into `libarbc` via `src/CMakeLists.txt`.
- `ToneContent` implements both surfaces: `bounds()` → empty `Rect{}` (visual cull), `stability()`/`audio_stability()` → `Static`, `audio()` returns a `ToneFacet` that synthesizes a byte-portable parabolic-sine tone at any requested rate with `achieved_rate == request.sample_rate` and `exact == true`.
- New conformance driver `tests/tone_conformance.t.cpp` wires `arbc::contract_tests(factory)` over a `ToneContent` factory, exercising all visual and audio families (linked `arbc-testing` before `arbc` per doc-17 boundary rule); wired into `tests/CMakeLists.txt`.
- New claim `12-audio#tone-renders-at-any-requested-rate` registered in `tests/claims/registry.tsv` and enforced by `src/kind_tone/t/tone_rates.t.cpp` (renders across {8000, 22050, 44100, 48000, 96000} Hz under `BestEffort`/`Exact`).
- Byte-exact goldens frozen at 48 kHz and 44100 Hz in `src/kind_tone/t/tone_goldens.t.cpp` (parabolic-sine waveform, no `std::sin`, platform-independent per doc-16).
- `depends contract.audio_facet` edge added to the `tone` leaf in `tasks/55-kinds.tji` for provenance (all predecessors already `complete 100`, scheduling unaffected).
- Gate: `scripts/gate` + `ARBC_GATE_PRESET=asan scripts/gate` — both green.
