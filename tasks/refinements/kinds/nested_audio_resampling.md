# Refinement — `kinds.nested_audio_resampling`

## TaskJuggler entry

`tasks/55-kinds.tji:52-57` — `task nested_audio_resampling "org.arbc.nested
audio cross-rate resampling quality"`, under `task kinds "Reference kinds"`
(55 — Reference kinds, docs 03/05/11/12/17).

> note "Replace the baseline nearest/hold cross-rate fill introduced in
> kinds.nested_audio with a higher-order deterministic resampler (e.g.
> polyphase / windowed-sinc) for children that report achieved_rate <
> request.sample_rate; each filter pinned by byte-exact per-filter goldens;
> keeps the mix seam, swaps the kernel — the audio analog of
> kinds.raster_resampling_quality. Docs 12/05.
> Source-of-debt: tasks/refinements/kinds/nested_audio.md"

## Effort estimate

`effort 2d`, `allocate team`. This is a **kernel swap behind a landed seam**,
not new machinery. `kinds.nested_audio` (2d) already shipped the whole audio
nesting descent — the synthetic-monitor per-layer walk, the injected
`pull_audio`, the additive Flat-mode mix with `gain`/layout remix, varispeed
via the composed rational rate, aggregate-revision metadata, and the depth-
budget termination. It deliberately left the **below-rate cross-rate fill** as
a baseline (the child's block read 1:1, honesty downgraded), naming this task
as the quality follow-up. The 2d covers: a deterministic windowed-sinc /
polyphase resampler as an `arbc::media` primitive (with a checked-in,
audited-script-generated coefficient table so it carries no runtime libm
variance), wiring it into `mix_child_layer`'s below-rate branch, and the
per-ratio byte-exact goldens + one new claims-register entry. The mix seam,
honesty math, and metadata memo are untouched.

## Inherited dependencies

Own `depends`: `!nested_audio` (`tasks/55-kinds.tji:55`), plus
`contract.conformance_suite` from the parent `task kinds` (`tasks/55-kinds.tji:4`).

**Settled predecessor this task builds on (`complete 100`):**

- **`kinds.nested_audio`** — the shipped audio facet this task refines.
  `NestedContent::NestedAudioFacet` (`src/kind_nested/arbc/kind_nested/nested_content.hpp:172-183`,
  stable instance `d_audio_facet{this}` at `:203`, handed out by
  `NestedContent::audio()` at `src/kind_nested/nested_content.cpp:379`);
  `render_audio` (`nested_content.cpp:515-557` — zeroes target `:528-530`,
  seeds `achieved = request.sample_rate; exact = true` `:541-542`, walks
  members bottom-to-top calling `mix_child_layer` `:548-554`); the per-layer
  descent `mix_child_layer` (`nested_content.cpp` — composed rate
  `child_rate = request.sample_rate * den/num` `:428-429`, child request at the
  composed rate carrying `snapshot`/`exactness` verbatim `:452-462`, the pull
  through the injected service `:469-480`, the **1:1 additive mix + layout
  remix** `:485-500`, the **achieved_rate / exact honesty downgrade** on the
  below-rate branch `:502-512`). The `gain`/`audible` placement fields and the
  `set_gain`/`set_audible` setters landed with this task. Refinement:
  `tasks/refinements/kinds/nested_audio.md` (baseline fill decision at `:494-508`,
  this follow-up named at `:420-426`).

**Structural precedent (settled, mirrored for shape — not a dependency):**

- **`color.resampling`** (`tasks/refinements/color/resampling.md`, Done
  2026-07-05) — the visual "higher-order filter behind an existing seam" that
  this task is the audio analog of. It (a) added deterministic filtered
  resampling as a **kernel** with byte-exact goldens (fixed tap order, float32,
  ordered reduction — doc 16's determinism recipe); (b) chose a **sampling
  convention that collapses byte-for-byte to the incumbent baseline at the
  trivial/aligned case**, so pre-existing goldens survived unchanged and only
  genuinely-filtered cases grew new goldens; (c) added exactly **one** new
  claims-register entry (`registry.tsv:35`) and deferred the full per-format
  golden matrix to a separate goldens task. This refinement follows all three
  moves.
- **`kinds.tone`** (`tasks/refinements/kinds/tone.md:213-217,328-339`) — the
  reference audio source. Its waveform **avoids `std::sin`** (an exact integer
  flick phase reduced to a fractional cycle, evaluated with a fixed parabolic-
  sine polynomial in pure IEEE-754 float32), so its samples are byte-exact and
  portable across toolchains. Tone is a **rate-honoring** child
  (`registry.tsv:69`: reports `achieved_rate == request.sample_rate` at every
  rate), so it exercises the pay-nothing path and **never triggers this task's
  resampler** — it is the regression-guard source, and the below-rate goldens
  need a distinct below-rate test source (see Constraints).

**Pending (must not be assumed at implementation time):**

- **`audio.mix_engine`** (`tasks/45-audio.tji:12-17`) — the concrete L4 pull-
  based mix engine that lands the real `PullService::pull_audio` override. It
  converts at its own edges (working → device) and will need the same cross-rate
  reconstruction the nesting boundary needs; it is a **sibling L4**
  `kind_nested` may not name (doc 17:59). Placing this task's resampler in the
  shared L1 `arbc::media` (below both) lets the mix engine consume the same
  primitive later **without** a cross-L4 edge (see Decisions). Not a dependency:
  this task drives its tests with a below-rate `PullService` audio double,
  exactly as `kinds.nested_audio` drove its pulls before the mix engine existed.

## What this task is

Replace the **baseline below-rate cross-rate fill** that `kinds.nested_audio`
left in `mix_child_layer` with a **higher-order deterministic resampler**, for
the case where a nested child bottoms out **below** the composed request rate.

Today (`nested_content.cpp:452-512`), every audible child is requested at the
composed rational rate `child_rate` over exactly `frames` frames, and the
returned block is read **1:1** into the additive mix. A child that honors that
rate (tone, nested-of-tones — every core reference scene) returns exactly the
frames the mix needs, placed 1:1, **byte-exact at any nesting depth** because
composed rates are exact rationals (doc 11:216-234). Only a child that reports
`achieved_rate < child_rate` — a native-below-request source such as a future
recorded-audio / codec plugin, or a heterogeneous-rate nesting boundary — is
served by the current baseline: its short block is reinterpreted 1:1 (a
nearest/hold reconstruction) and the aggregate `achieved_rate`/`exact` is
honestly downgraded (`nested_content.cpp:502-512`).

This task swaps that hold for a proper band-limited reconstruction. On the
below-rate branch, `mix_child_layer` obtains the child's genuine **native-rate**
samples over the same child-local window and runs a **fixed-tap windowed-sinc
polyphase** kernel — a deterministic `arbc::media` primitive over interleaved
float32 `AudioBlock`s — to produce exactly `frames` samples at `child_rate`,
which then feed the **unchanged** 1:1 additive mix and layout remix
(`nested_content.cpp:485-500`). The `achieved_rate`/`exact` honesty math
(`:502-512`) is **unchanged**: reconstruction improves the sample *values*, it
never fabricates a higher rate or exactness. This is doc 12's raster-upscaling
analog for audio (`12-audio.md:24-25`: "recorded audio bottoms out at its native
rate and reports it, and the engine resamples — the exact analog of upscaling a
raster past native"), giving the nesting-boundary "converts (resample + remix)"
promise (`12-audio.md:100-104`) a real filter.

**Scope:** the **below-rate / upsampling** reconstruction only (child native
rate < composed request rate — the sole shape the task note names and the only
one the honesty branch flags in practice, since a child that *can* honor a lower
request downsamples internally and reports `achieved_rate == request.sample_rate`).
Time-stretch (pitch-preserving) is separately deferred with the effects stack
(doc 12:116-118) — out of scope. Spatial-mode policy is `audio.spatial_policy`,
untouched. No model, contract, or levelization change: this is a kernel + a
media primitive + goldens.

## Why it needs to be done

`kinds.nested_audio` shipped the recursion proof of the audio facet with a
**correct-but-baseline** cross-rate fill: a below-rate child is length-correct
and honestly reported, but its held reconstruction is not the band-limited
result doc 12 promises for the nesting boundary. `kinds.nested_audio_resampling`
is wired into **M6** — `m6_audio` `depends … kinds.nested_audio,
kinds.nested_audio_resampling` (`tasks/99-milestones.tji:51`) — so M6's audio
story is not complete until the boundary reconstructs at quality. Downstream,
`audio.mix_engine` performs the same working→device and inter-composition rate
conversion; landing the deterministic resampler as a shared `arbc::media`
primitive gives the mix engine a byte-exact reconstruction to reuse rather than
re-derive, and gives the whole audio stack a permanent per-ratio golden anchor
for resample quality — the audio twin of what `color.resampling` did for the
compositor.

## Inputs / context

**Governing design-doc sections (normative, doc 16):**

- `docs/design/12-audio.md:24-25` — the framing analog: "recorded audio bottoms
  out at its native rate and reports it, and **the engine resamples** — the
  exact analog of upscaling a raster past native." This task is the engine-side
  resample for the nesting boundary.
- `docs/design/12-audio.md:100-104` — working format / edges: "Nested
  compositions may declare different working formats; the **nesting boundary
  converts (resample + remix)**, homogeneous trees pay nothing." This task
  supplies the quality of the "resample" half; the "remix" (layout) half and the
  pay-nothing homogeneous path already shipped.
- `docs/design/12-audio.md:107-118` — rate maps / **Varispeed** (default):
  "resample through the composed rational rate … exact, cheap, always correct as
  a *signal* interpretation of the time map, and well-defined at any nesting
  depth because composed rates are exact rationals." Varispeed of a **honoring**
  child stays byte-exact and resampler-free; varispeed of a **below-rate** child
  composes the exact-rational rate first (doc 11), then this kernel reconstructs.
  Time-stretch is separately deferred (`:116-118`).
- `docs/design/11-time-and-video.md:44-48, 216-234` — exact-rational rate
  composition, **one rounding at the leaf, never accumulate**. The output-frame →
  native-sample position is computed from the composed per-edge rational and
  rounded once (to the polyphase phase index), never accumulated across depth.
- `docs/design/16-sdlc-and-quality.md:48-53` — **byte-exact goldens** are the
  default (CPU determinism: fixed FP flags, no FMA, ordered reductions);
  "perceptual-tolerance comparison exists only where platform libm variance is
  unavoidable — tolerances are the exception and each carries a justifying
  comment." This task must be **byte-exact** (no tolerance): the resampler avoids
  runtime libm exactly as tone's waveform does (checked-in coefficient table),
  so there is no unavoidable-libm exception to invoke.
- `docs/design/05-recursive-composition.md:55-91` — the machinery the resampler
  inherits unchanged: budgets flow through nesting; the composed result is keyed
  on the aggregate revision. The resampler adds no state to the descent.
- `docs/design/17-internal-components.md:48-59` — levelization. `arbc::media`
  (L1) already owns "channel layouts, typed pixel/sample span views" and cites
  doc 12; `arbc::kind-*` (L4) `nested` "uses only the `PullService` interface";
  `arbc::audio-engine` (L4) is a sibling `kind_nested` may not name. The
  resampler is a media primitive both L4 consumers reach through their existing
  closures.

**Real source seams (paths + lines):**

- `src/kind_nested/nested_content.cpp` — `mix_child_layer`: composed rate
  `:428-432`; child request at composed rate (window, layout, `exactness`/
  `snapshot` verbatim) `:452-462`; pull through injected service `:469-480`; the
  **1:1 additive mix + layout remix** (unchanged) `:485-500`; the **below-rate
  honesty branch** `:502-512` — the exact seam the kernel plugs into. The
  guiding comment already names this task at `:503-505`.
- `src/kind_nested/arbc/kind_nested/nested_content.hpp:172-183,203` —
  `NestedAudioFacet` and its stable `d_audio_facet` instance; `mix_child_layer`
  declaration. No signature change is required here.
- `src/media/arbc/media/audio_block.hpp:14-47` — `ChannelLayout {Mono, Stereo}`,
  `channel_count()`, the non-owning interleaved float32 `AudioBlock
  {samples, frames, layout, rate}`. The resampler's input and output types.
  `src/media/arbc/media/audio_format.hpp` — `AudioFormat`/`k_working_audio`.
  `arbc::base` (L0) owns the rational-rate arithmetic
  (`src/base/arbc/base/rational_time.hpp`) for the exact phase computation.
- `src/media/CMakeLists.txt` — where the new `media` translation unit / header
  and its coefficient table are wired (media is L1, `DEPENDS base`).
- `tests/nested_audio_goldens.t.cpp` + `tests/CMakeLists.txt:60-63` — the
  existing byte-exact audio golden suite (`arbc_nested_audio_goldens_t`,
  linking the `arbc` umbrella) and its `catch_discover_tests` wiring; the
  regression-guard goldens live here or in a sibling
  `tests/nested_audio_resampling_goldens.t.cpp`.
- `tests/nested_audio_concurrency.t.cpp` + `tests/CMakeLists.txt:68-71` — the
  TSan lane to extend with a below-rate child.
- `src/kind_nested/CMakeLists.txt:1-7` — `kind_nested` (`DEPENDS contract`
  only) and its `arbc_component_test` unit list (`t/nested_meta.t.cpp
  t/nested_audio_meta.t.cpp`); a direct kernel unit test attaches here.
- `tests/claims/registry.tsv:69-71` — the audio claims to re-assert / extend:
  `#tone-renders-at-any-requested-rate` (:69, honoring source untouched),
  `#nested-mixes-child-audio-through-pull` (:70, the recursion identity whose
  honoring path must stay byte-exact), `#nested-audio-metadata-aggregates`
  (:71, untouched).

## Constraints / requirements

1. **Levelization unchanged (doc 17, CI-enforced).** The resampler is a
   deterministic free function in **`arbc::media`** (L1) over `AudioBlock`s; it
   names no `compositor`, `audio-engine`, or `backend-cpu` type and adds no
   component edge. `kind_nested` keeps its sole declared edge `DEPENDS contract`
   — media is already in contract's closure and `mix_child_layer` already
   consumes media symbols (`AudioBlock`, `channel_count`). `scripts/check_levels.py`
   must pass unchanged.
2. **Kernel swaps behind the landed mix seam.** The additive mix + layout remix
   (`nested_content.cpp:485-500`) and the `achieved_rate`/`exact` honesty math
   (`:502-512`) are **not** modified in behavior. The resampler is inserted
   between the child pull and the mix, producing exactly `frames` samples at
   `child_rate` from the child's native-rate samples, so the downstream 1:1 mix
   is unchanged. No change to `NestedAudioFacet`'s public shape, to `AudioRequest`/
   `AudioResult`, or to the model.
3. **Below-rate branch only; honoring path is byte-exact and untouched.** The
   resampler runs **only** when a child reports `achieved_rate < child_rate`.
   When `achieved_rate == child_rate` (tone, nested-of-tones, every homogeneous
   reference scene) the pay-nothing 1:1 path is taken verbatim, so **every
   existing `tests/nested_audio_goldens.t.cpp` golden reproduces byte-for-byte**
   (the analog of `color.resampling`'s collapse-to-baseline-at-aligned-case).
4. **Obtain genuine native samples, one rounding at the leaf (doc 11).** On the
   below-rate branch, resolve the child's native samples over the same child-
   local window at the reported native rate (a second `pull_audio` over the same
   window at `cr.achieved_rate` — block-cache-served, cheap — see Decisions),
   then map each output frame's position through the exact composed rational and
   round **once** to the polyphase phase index. Never accumulate rate across
   depth (`11:216-234`); two-level nesting composes the rational first, rounds at
   the leaf.
5. **Deterministic, byte-exact, no runtime libm (doc 16).** The polyphase filter
   coefficients are a **checked-in table generated by an audited script** (doc
   16:50-53's "regenerate with an audited script"); runtime does only fixed-order
   float32 multiply-accumulate with **ordered reduction, no FMA**. No `std::sin`/
   libm at render time — mirroring tone's portability discipline — so goldens are
   byte-exact with **no tolerance** and identical across toolchains. A tolerance
   is not permitted here (there is no unavoidable-libm variance to justify one).
6. **Honesty preserved, never fabricated.** After reconstruction the aggregate
   `achieved_rate` still reports the child's true native rate mapped to parent
   (the existing `eff` computation, `nested_content.cpp:507-512`) and `exact`
   stays `false` for that contributor. A higher-order reconstruction improves the
   samples' fidelity but creates no information: it must **not** raise
   `achieved_rate` toward the request rate nor report `exact`.
7. **Fixed, documented filter characteristics.** A single fixed tap count (e.g.
   2N taps) and window (e.g. Kaiser/Blackman-Harris) — no runtime-selectable
   quality knob (doc 12 defines none, doc 16 favors one deterministic path). The
   chosen tap count / window / polyphase-branch count are recorded beside the
   coefficient table and referenced from the golden regeneration script.
8. **Thread-safety unchanged.** The resampler is a pure function over caller-
   owned local buffers with no shared state; `render_audio` keeps
   `render_thread_safe()` `true` (`nested_content.hpp:84`). It runs on the
   worker/frame thread as the rest of the descent does (audio renders ahead on
   workers, never the device callback — doc 12:154-164).
9. **Snapshot / budget discipline unchanged.** The second (native-rate) pull
   carries the same `snapshot`/`exactness` verbatim and is subject to the same
   depth budget; the resampler adds no new limiter and no new recursion path (a
   Droste below-rate child still terminates by the doc-05 depth budget).

## Acceptance criteria

**New claims-register entry (doc 12 nesting boundary, resample-specific).** Add
to `tests/claims/registry.tsv`, pinned by an `enforces:`-tagged test:

- `12-audio#nested-boundary-resamples-below-rate-children` — *At the nesting
  boundary, `org.arbc.nested` reconstructs a child that reports `achieved_rate <
  the composed request rate` with a deterministic fixed-tap windowed-sinc /
  polyphase filter over decoded float32 samples — a band-limited reconstruction,
  decisively not a nearest/hold — producing exactly `frames` samples at the
  composed rate before the additive mix; the reconstruction is a byte-exact
  deterministic function of `(native samples, native rate, composed rate,
  window)`, and it preserves honesty: the aggregate `achieved_rate` still reports
  the child's native rate and `exact` stays `false`.* Pinned by the **per-ratio
  byte-exact goldens** below, a **behavioral-counter** assertion (exactly one
  pull for a honoring child; exactly two — discovery + native — for a below-rate
  child, the second block-cache-served), and an **honesty** assertion (a
  resampled below-rate child reports native `achieved_rate`, `exact == false`).

**Re-asserted (a second `enforces:` test, not re-registered):**

- `12-audio#nested-mixes-child-audio-through-pull` (`registry.tsv:70`) — the
  honoring / homogeneous recursion-identity path stays byte-exact and resampler-
  free (the regression-guard golden below).
- `12-audio#tone-renders-at-any-requested-rate` (`registry.tsv:69`) — tone still
  honors every rate and never triggers the resampler (behavioral-counter: one
  pull, no native re-request) — untouched.

**Byte-exact per-ratio goldens (doc 16 default; portable — no runtime libm).**
Under `tests/nested_audio_resampling_goldens.t.cpp` (new, mirroring
`tests/CMakeLists.txt:60-63`), or extended into `tests/nested_audio_goldens.t.cpp`,
driven by a **below-rate audio test source** (a `PullService` audio double / test
content that returns a deterministic band-limited signal at a chosen native rate
below the composed rate and reports that `achieved_rate` — tone cannot serve, it
always honors):

- **Integer-ratio upsample** — a native-24000 child mixed into a 48 kHz
  composition (2:1), byte-exact.
- **Non-integer ratio** — a native-44100 child into 48 kHz, byte-exact (the
  polyphase fractional-phase path).
- **Varispeed × below-rate** — a rate-½ layer (child_rate = 96 kHz) with a
  native-32000 child (composed 3:1), byte-exact — proving exact-rational
  composition then one rounding at the leaf (doc 11).
- **Two-level nesting with a below-rate leaf** — the reconstruction composes
  through depth, byte-exact.
- **Regression guard** — a honoring child (tone / nested-of-tones) reproduces the
  **existing** `nested_audio_goldens.t.cpp` bytes exactly (resampler not
  invoked). This is the collapse-to-baseline guard.

**Contract conformance (the kind's own `arbc-testing` run).** `audio()` is
already non-null, so `tests/nested_conformance.t.cpp` continues to auto-run
`check_audio_facet_consistency` + `check_audio_async`; both must stay green with
the resampler wired (block-continuity — a window split at an interior frame
boundary concatenates bit-identically — must hold through the reconstruction).

**Async / concurrency (doc 16 — concurrency-touching lane).** Extend
`tests/nested_audio_concurrency.t.cpp` (`tests/CMakeLists.txt:68-71`) with a TSan/
stress case rendering a nested **below-rate** audio scene through a multi-worker
`PullService` audio double: deterministic samples, no data race (the resampler is
a pure media function over local buffers).

**Direct kernel unit test.** A component unit test for the `arbc::media`
resampler (attached via `arbc_component_test` in `src/media/CMakeLists.txt` or
`src/kind_nested/CMakeLists.txt:7`) asserting byte-exact output for a fixed input
at a fixed ratio — the kernel's own golden, independent of the nesting descent —
so the mix engine's future reuse has a component-level anchor.

**CI gates.** ≥90% diff coverage on changed lines; `scripts/check_levels.py`
(media gains a function, no new edge; `kind_nested` still names only `contract`);
`scripts/check_claims.py` both directions (the new claim registered + enforced;
the re-asserted claims stay enforced).

**Deferred follow-ups:** none. This task fully closes the below-rate /
upsampling reconstruction gap for the nesting boundary. Time-stretch is already
deferred with the effects stack (doc 12:116-118); Spatial-mode policy is already
`audio.spatial_policy`; a boundary **downsample** (child native *above* the
composed rate) has no core or plugin consumer today (a child able to honor a
lower request downsamples internally and reports `achieved_rate ==
request.sample_rate`), so no speculative leaf is spawned (surfaced to the closer
in the return summary, not encoded as a WBS task).

## Decisions

- **Swap the kernel behind the landed mix seam; do not touch the descent.** The
  synthetic-monitor walk, injected `pull_audio`, additive mix, layout remix,
  metadata memo, and depth-budget termination all shipped correct in
  `kinds.nested_audio`; only the *below-rate reconstruction quality* is
  deficient. The resampler slots between the child pull and the 1:1 mix
  (`nested_content.cpp:480→485`), producing `frames` samples at `child_rate`, so
  the mix and honesty math (`:485-512`) are behaviorally unchanged — precisely
  the "keeps the mix seam, swaps the kernel" the task note and the
  `color.resampling` precedent prescribe. *Alternative rejected:* reworking
  `render_audio`/`mix_child_layer` around a resample-first pipeline — needless
  churn on a correct descent, and it would risk the byte-exactness of the
  honoring path that all existing goldens pin.
- **Run the resampler only on the below-rate branch; honoring path stays 1:1 and
  byte-exact.** Gating on `cr.achieved_rate < child_rate` means every core
  reference scene (tone, nested-of-tones, homogeneous 48 kHz) keeps its exact
  1:1 placement and reproduces its existing goldens byte-for-byte — the audio
  analog of `color.resampling`'s texel-center convention that collapses the
  higher-order filter to the incumbent tap at the aligned case
  (`tasks/refinements/color/resampling.md`). Here the collapse is a branch, not a
  numeric coincidence, so it is exact by construction. *Alternative rejected:*
  running the resampler unconditionally (identity ratio) — would perturb the
  honoring path's bytes (a unity-ratio polyphase pass is not bit-identical to a
  raw copy), breaking the regression guard and taxing the pay-nothing path doc
  12:104 promises.
- **Reconstruct from genuine native samples via a second native-rate pull.** A
  below-rate child, requested at the composed `child_rate`, cannot convey more
  than `achieved_rate` of genuine information in the caller-sized block — the
  baseline hold. To reconstruct properly the boundary needs the child's real
  native samples, so on the below-rate branch nested re-requests the **same
  child over the same child-local window at `cr.achieved_rate`** (a genuine
  native block), then filters. The second pull is block-cache-served (same
  content, revision, window) so it is cheap, and it is issued only for the rare
  below-rate child. *Alternative rejected:* extending `AudioResult` to carry a
  native frame count so a single pull could return native samples inline — a
  contract change owned by `contract.audio_facet`, out of `kind_nested`'s level,
  and it would ripple into the mix engine; the two-phase discovery keeps the
  contract stable and the change local. *Alternative rejected:* trusting the
  child's own resample (drop nested's) — contradicts doc 12:24-25 ("**the
  engine** resamples") and would scatter resample quality across every plugin.
- **Windowed-sinc polyphase with a checked-in coefficient table; byte-exact, no
  runtime libm.** A fixed-tap windowed-sinc (Kaiser / Blackman-Harris) polyphase
  bank is the standard band-limited reconstruction and composes cleanly with the
  exact-rational phase from doc 11. Coefficients are generated **once by an
  audited script** and checked in (doc 16:50-53); runtime does only ordered,
  no-FMA float32 MACs — so output is **byte-exact with no tolerance**, portable
  across toolchains exactly as tone's parabolic-sine waveform is
  (`tasks/refinements/kinds/tone.md:328-339`). *Alternative rejected:* computing
  sinc/window at runtime with `std::sin` — introduces platform libm ULP variance,
  which doc 16 admits only where "unavoidable" and each with a justifying
  comment; here it is entirely avoidable by tabulation, so a tolerance is not
  warranted. *Alternative rejected:* linear/cubic interpolation — cheaper but a
  demonstrably poorer band-limiter; the task's whole purpose is quality above the
  baseline hold, so it targets the windowed-sinc tier, matching the raster
  higher-order-filter follow-up (`tasks/refinements/kinds/raster.md:328-331`).
- **Place the resampler in `arbc::media` (L1), not privately in `kind_nested`.**
  `arbc::media` already owns the audio-buffer vocabulary this operates on
  (`AudioBlock`, `ChannelLayout`, `AudioFormat`; doc 17:50 lists "channel
  layouts, typed pixel/sample span views" and cites doc 12), and `kind_nested`
  already consumes those media symbols through contract's closure — so the
  function is reachable with **no new declared edge** and `check_levels` is
  unchanged. Critically, `audio.mix_engine` (a sibling L4 that `kind_nested` may
  not name, doc 17:59) performs the same working→device / inter-composition rate
  conversion and can reuse the identical primitive **only** if it lives below
  both — media is that shared floor. This is the reuse-favoring call and the
  second consumer is imminent (mix engine is the next audio leaf). *Alternative
  rejected:* a `kind_nested`-private kernel — simplest for the single call site
  today, but it forces the mix engine to duplicate a byte-exact DSP kernel (and
  its golden table) weeks later across an un-crossable L4 boundary; a media
  primitive is the same reuse move `color.resampling` made by placing its box
  reducer where both consumers reach it. *No design-doc delta:* per the brief's
  criteria this is not a new dependency, new seam, or behavior deviation — media
  is an existing component whose charter (audio sample-span primitives, doc 12
  already cited) covers the function; the resample-happens rule is already
  normative (doc 12:24-25,100-104) and byte-exact-quality follows from doc 16's
  default. The single new normative surface — the reconstruction is deterministic
  and honesty-preserving — is pinned by the new claims-register entry, exactly as
  `color.resampling` filled an existing doc rule with one claim rather than new
  doc language.
- **Honesty is preserved, not improved.** The reconstruction raises fidelity but
  not information, so the aggregate `achieved_rate` still reports the child's
  native rate and `exact` stays `false` (`nested_content.cpp:507-512` unchanged).
  *Alternative rejected:* reporting `achieved_rate == request.sample_rate` after
  a "good enough" resample — dishonest per doc 12's rate-honesty symmetry
  (`registry.tsv:69,87`); a resampled signal is not a natively-rated one.

## Open questions

(none — all decided.) One non-blocking observation is surfaced to the closer in
the return summary rather than encoded here: the deterministic resampler lands
in `arbc::media` as a shared primitive that `audio.mix_engine` will also consume
for its working→device / inter-composition rate conversion — the closer should
note the mix engine reuses rather than re-adds it (no new WBS leaf; the mix
engine is already scoped).

## Status

**Done** — 2026-07-07.

- Windowed-sinc polyphase resampler landed as `arbc::media` primitive: `src/media/arbc/media/audio_resampler.hpp` (header) and `src/media/audio_resampler.cpp` (frozen 32-phase × 16-tap Blackman-Harris coefficient table; no runtime libm).
- `src/kind_nested/nested_content.cpp` below-rate branch updated: second native-rate pull → `resample_audio` → unchanged 1:1 additive mix; honesty math (`achieved_rate`/`exact`) untouched.
- Kernel unit tests: `src/media/t/audio_resampler.t.cpp` (exact 2:1, identity, not-a-hold, determinism, no-op, byte-exact frozen golden with `[.regen]` dumper).
- Integration/golden tests: `tests/nested_audio_resampling_goldens.t.cpp` — six cases (integer 24k→48k, non-integer 44.1k→48k, varispeed×below-rate 32k→96k, two-level nesting, honoring-guard regression, deferred-native fallback); behavioral-counter (1 pull honoring / 2 below-rate) and honesty assertions.
- Concurrency: `tests/nested_audio_concurrency.t.cpp` extended with TSan below-rate lane.
- New claim `12-audio#nested-boundary-resamples-below-rate-children` registered in `tests/claims/registry.tsv` and enforced (re-asserts `#nested-mixes-child-audio-through-pull`, `#tone-renders-at-any-requested-rate`).
- `src/media/CMakeLists.txt` and `tests/CMakeLists.txt` updated to wire new translation units and test targets.
- Levelization unchanged: `kind_nested` still declares only `contract`; resampler is reachable through existing media closure; `check_levels` green.
- Non-blocking note: `audio.mix_engine` will reuse `arbc::media::resample_audio` for working→device / inter-composition conversion — no new WBS leaf required.
