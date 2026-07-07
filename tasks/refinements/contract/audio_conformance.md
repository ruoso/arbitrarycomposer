# contract.audio_conformance — Audio facet conformance (`arbc-testing` extension)

## TaskJuggler entry

Back-link: [`tasks/25-contract.tji:51-56`](../../25-contract.tji).

```
task audio_conformance "Audio facet conformance (arbc-testing extension)" {
  effort 1d
  allocate team
  depends !audio_facet, !conformance_suite
  note "Extend arbc-testing facet-consistency and async families to
        AudioFacet; register 03-layer-plugin-interface#audio-facet-consistent.
        Source: tasks/refinements/contract/conformance_suite.md. Docs 12/16."
}
```

## Effort estimate

**1d** (`tasks/25-contract.tji:52`). A focused extension, not a new artifact:
`arbc-testing` already exists (shipped by `conformance_suite`), the audio
surface it tests already exists (shipped by `audio_facet`), and the
thread-safe settle/cancel/`take` machinery is the shared `Completion<Result>`
template already TSan-covered for the render side. The work is two new
property families over the existing suite (`check_audio_facet_consistency`,
`check_audio_async`), the `Options` knobs and umbrella dispatch to reach them,
audio-bearing fixtures in the existing conformance driver, and one new
claims-register row. Sized like the 1d sibling contract surfaces
(`audio_facet`, `temporal_fields`), well below the 4d `conformance_suite` that
stood the harness up.

## Inherited dependencies

**Settled (formal `depends`):**

- `contract.audio_facet` (DONE 2026-07-06,
  [`audio_facet.md`](audio_facet.md)) — shipped the entire audio surface this
  task drives:
  - `class AudioFacet` (`src/contract/arbc/contract/content.hpp:261-293`) with
    `audio_extent()` (`:271`), `audio_stability()` (`:275`), `latency()`
    (`:280`, defaulted `Time::zero()`), and `render_audio(request, done)`
    (`:288-289`).
  - `AudioRequest` (`:242-249`: `window`, `sample_rate`, `layout`,
    `AudioBlock& target`, `exactness`, `snapshot` — no `Deadline`, audio
    renders ahead), `AudioResult` (`:217-220`: `achieved_rate`, `exact`),
    `AudioCompletion = Completion<AudioResult>` (`:226`).
  - Facet discovery via the null-default virtual `Content::audio()`
    (`:438`) — the exact twin of `editable()`.
  - The one claim it registered, `03-layer-plugin-interface#audio-facet-optional`
    (`tests/claims/registry.tsv:142`, enforced in
    `src/contract/t/audio_facet.t.cpp:133`), which already pins pointer
    identity across calls, the `latency()` default, and `render_audio`
    settling exactly once inline-or-async. This task re-runs that invariant
    over arbitrary generated content but does **not** re-register it.
- `contract.conformance_suite` (DONE 2026-07-05,
  [`conformance_suite.md`](conformance_suite.md)) — shipped `arbc-testing`
  itself: the `STATIC` library (sources under `testing/`, public header
  `testing/arbc/testing/contract_tests.hpp`), the seeded suite-owned PRNG,
  the umbrella `arbc::contract_tests(factory, options)` plus granular
  per-family entry points, the `Options` seed/toggle struct, and the
  cross-component conformance driver `tests/contract_conformance.t.cpp`. This
  task extends exactly two of its seven families — facet consistency
  (`testing/contract_tests.cpp:399-412`, decl `contract_tests.hpp:115`) and
  async completion/cancellation (`contract_tests.cpp:321-397`, decl
  `contract_tests.hpp:110`) — and the deferral that spawned this task is
  recorded at [`conformance_suite.md:294-300`](conformance_suite.md).

**Sibling substrate (landed, relied on, not a formal `depends`):**

- `contract.temporal_fields` — the three-way `Stability { Static, Timed, Live }`
  enum and `arbc::base` half-open `TimeRange`. `audio_stability()` returns the
  *same* `Stability` enum the visual facet reports (`content.hpp:272-275`), so
  the audio consistency check is the 1D twin of the visual
  `check_facet_consistency`.
- `contract.snapshot_pins` — `AudioRequest.snapshot` is the same index-only
  `model::StateHandle` a `RenderRequest` pins (`content.hpp:234-241`, doc
  12:79-82), which makes `render_audio` a pure function of
  `(snapshot, window, rate, layout)` for non-`Live` content — the audio twin
  of render purity, the property the block-continuity check exercises.
- `audio.audio_types` (`0767140`, DONE) — the `arbc::media` (L1) vocabulary the
  facet signature names: `AudioBlock` (`src/media/arbc/media/audio_block.hpp:42-47`,
  non-owning interleaved float32 view), `ChannelLayout` (`:14`), and
  `channel_count(layout)` (`:21`). The suite allocates its own caller-owned
  block from these to hand `render_audio` a target.

**Downstream (this task unblocks / feeds):**

- `kinds.tone` (`org.arbc.tone`, the audio "hello world", doc 03:203) and
  `kinds.nested_audio` — each links `arbc-testing` and calls
  `arbc::contract_tests(factory)` as its conformance gate; with this task the
  umbrella run also drives their audio facet, no extra call site.
- The `m6_audio` milestone (`tasks/99-milestones.tji:50-53`) formally depends
  on `contract.audio_conformance`.

## What this task is

Extend the shipped `arbc-testing` conformance suite so a `Content` that
exposes an `AudioFacet` is driven through property-based conformance for its
audio, exactly as its pixels already are. Two new families:

1. **Audio facet consistency** — the 1D twin of the visual
   `check_facet_consistency`: the audio description methods
   (`audio_extent`/`audio_stability`/`latency`) are idempotent and mutually
   coherent, a `render_audio` window lying entirely outside `audio_extent()`
   yields a silent block (audio-extent honesty), and a `Static`/`Timed` facet's
   samples are a deterministic, block-continuous function of the pinned request
   (splitting a window at an interior boundary and concatenating the two
   sub-renders is bit-identical to the single-window render).
2. **Audio async** — re-runs the one-code-path settle discipline over
   `render_audio`/`AudioCompletion`: an inline `AudioResult` and a
   `nullopt`+`AudioCompletion` settlement drain equivalently through `take()`
   exactly once, verified under concurrent `complete`/`cancel`/`take`.

Facet discovery is via the null-default `Content::audio()` virtual: content
that returns `nullptr` (every visual-only kind) skips the audio families at
zero cost, and `Live` audio opts out of the determinism-dependent checks, the
audio analog of the visual suite's `Live`-skips-purity rule. The task
registers exactly one new claim,
`03-layer-plugin-interface#audio-facet-consistent`, and re-runs the existing
`#audio-facet-optional` invariant over generated content without
re-registering it.

## Why it needs to be done

`audio_facet` shipped the `AudioFacet` interface but explicitly deferred its
*property-based, arbitrary-content* verification, keeping its own tests to one
hand-written `FacetDouble` in `src/contract/t/audio_facet.t.cpp`.
`conformance_suite` stood up the harness but explicitly deferred audio,
naming this task as the closer of the gap
([`conformance_suite.md:294-300`](conformance_suite.md)). Until this lands,
the audio facet is the one part of the layer contract with no reusable
conformance gate: `org.arbc.tone`, `kinds.nested_audio`, and every future
audio-bearing plugin would have to hand-roll their own audio invariant tests
or ship none. It is a formal dependency of the `m6_audio` milestone
(doc 12 realized). The suite is doc 16's "crown jewel"
(`docs/design/16-sdlc-and-quality.md:31`) precisely because plugin quality
scales without review capacity; leaving audio uncovered is a hole in that
story.

## Inputs / context

### Design docs (normative, doc 16)

- **`docs/design/12-audio.md:6-34`** (`## The symmetry, continued`) — the
  visual/audio parallel: `region`+`scale` ↔ time `window`+`sample_rate`,
  `Surface` ↔ `Block`, `achieved_scale` ↔ `achieved_rate`, and
  "`Static`/`Timed`/`Live` stability … applies verbatim" (`:26-29`, a tone is
  `Static`, most audio `Timed`, a microphone `Live`). The one asymmetry
  (`:31-34`): audio renders *ahead*, never on a deadline (hence no `Deadline`
  field, hence the audio families never assert deadline behavior).
- **`docs/design/12-audio.md:36-82`** (`## Audio as a content facet`) — the
  facet's normative shape: `render_audio` answers on the *identical*
  inline-or-settle-later code path as `Content::render` (`:73-78`);
  `AudioRequest.snapshot` is the same `StateHandle` a `RenderRequest` pins
  (`:79-82`), so audio is a pure function of the pinned state — the source of
  the block-continuity/determinism property.
- **`docs/design/12-audio.md:94-104`** (`## Working format`) — samples are
  always float32; the working default is 48 kHz stereo. The suite's generated
  requests draw rate/layout around these defaults.
- **`docs/design/16-sdlc-and-quality.md:31-44`** — the conformance suite
  definition; *facet consistency* and *async completion + cancellation* are
  named families this task extends. `:39-40` mandates seeded property-based
  generation ("the contract is algebraic, so test it algebraically").
- **`docs/design/16-sdlc-and-quality.md:15-17`** — the claims-register
  mechanism: each normative claim gets an id, enforced by a
  `// enforces: <claim-id>` test tag; CI fails on an unenforced claim.
- **`docs/design/16-sdlc-and-quality.md:66-73`** — concurrency tier: TSan on
  the suite; the async settle/cancel/take family is the TSan-covered one.
- **`docs/design/17-internal-components.md:14,53,41-44,151`** — levelization:
  `arbc-testing` is a separate shipped artifact, never linked by `libarbc`
  (`:14`); the `AudioFacet` lives in `arbc::contract` **L3** (`:53`); a
  component depends only on strictly-lower levels (`:41-44`, CI-enforced); the
  audio conformance families must therefore drive the **L3** contract surface
  only and must not reach into the **L4** `arbc::audio-engine` (mix/lookahead).

### Source seams

- `src/contract/arbc/contract/content.hpp` — the surface under test:
  `AudioFacet` (`:261-293`), `audio_extent()` (`:271`), `audio_stability()`
  (`:275`), `latency()` (`:280`), `render_audio()` (`:288-289`); `AudioRequest`
  (`:242-249`), `AudioResult` (`:217-220`), `AudioCompletion` (`:226`);
  `Content::audio()` null-default discovery virtual (`:438`).
- `src/media/arbc/media/audio_block.hpp` — `AudioBlock` (`:42-47`),
  `ChannelLayout` (`:14`), `channel_count()` (`:21`): the vocabulary the suite
  uses to allocate a caller-owned target block (a `std::vector<float>` of
  `frames * channel_count(layout)`) and inspect the samples `render_audio`
  writes.
- `testing/arbc/testing/contract_tests.hpp` — the header to extend: the
  `Options` struct (`:48-71`) with its per-family toggles (`facet_consistency`
  `:69`, `async_cancellation` `:68`), the granular family decls
  (`check_facet_consistency` `:115`, `check_async_cancellation` `:110`), and
  the umbrella `arbc::contract_tests` decl (`:168`).
- `testing/contract_tests.cpp` — the impl to extend: `check_facet_consistency`
  (`:399-412`, the pattern the audio consistency check mirrors),
  `check_async_cancellation` (`:321-397`, whose `Completion` drive and
  concurrent settle/cancel/take loop the audio async family reuses over
  `AudioCompletion`), and the umbrella dispatch (`:547-573`).
- `tests/contract_conformance.t.cpp` — the cross-component driver (may pull
  `kind_solid` and purpose-built doubles) where the new audio fixtures and
  `// enforces:` tags land.
- `src/contract/t/audio_facet.t.cpp` — the existing component unit test with
  its `FacetDouble` (inline-vs-async `render_audio`) and the
  `// enforces: 03-layer-plugin-interface#audio-facet-optional` tag (`:133`);
  the model for the driver's audio doubles (generalized into the suite's
  factory form, not copied).
- `tests/claims/registry.tsv` — the register; the new row sits alongside the
  visual `03-layer-plugin-interface#facet-consistency` (`:88`) it parallels and
  `03-layer-plugin-interface#audio-facet-optional` (`:142`) it complements.
- `scripts/check_claims.py` — the bidirectional claims gate; already scans
  `tests/` (where the enforcing tag lands) and, per
  [`conformance_suite.md` Decision 4](conformance_suite.md), `testing/` too.

## Constraints / requirements

1. **Extension only — no new artifact, no new dependency.** The work lands
   inside the existing `arbc-testing` library and the existing conformance
   driver. `arbc-testing` continues to depend only on `arbc::contract` (L3)
   and its transitive lowers (`base`, `media`, `surface`, `model`) plus
   Catch2, and is never linked by `libarbc` (`17:14`). No new third-party
   dependency (doc 10 policy) — `AudioBlock` storage is a plain
   `std::vector<float>`.
2. **Levelization respected (L3 ceiling, CI-enforced, `17:41-44`).** The audio
   families drive only the `AudioFacet`/`AudioRequest`/`AudioResult`/
   `AudioCompletion` contract surface. They must **not** reach into the L4
   `arbc::audio-engine`. The audio *pull-routing* property (inputs pulled only
   via `PullService::pull_audio`, `content.hpp:502-503`) is L4/operator-runtime
   and is explicitly out of scope here (see Decisions — it is the audio twin of
   the visual pull-routing already owned by `operators.operator_conformance`).
3. **Facet discovery via `Content::audio()`.** The umbrella entry point probes
   `content->audio()`; when it is `nullptr`, the audio families are skipped
   entirely (a visual-only content passes without paying an audio path). This
   mirrors the visual suite's "content that does not expose the facet is
   skipped." `Live` audio (`audio_stability() == Stability::Live`) additionally
   opts out of the determinism-dependent checks (block continuity,
   extent-outside silence for a live source), the audio analog of the
   `Live`-skips-purity rule (doc 14:173-174 in spirit).
4. **Property-based, seeded, suite-owned PRNG — no ambient randomness.** Audio
   windows, sample rates, and layouts are generated from the same
   `Options::seed`-threaded PRNG the visual families use (fresh salt per
   family, e.g. `seed ^ 0x0A` / `seed ^ 0x0B`, matching the existing
   `check_async_cancellation`'s `seed ^ 0x06`). No wall-clock, no system
   randomness — a fixed seed reproduces an exact case set (the surface the
   quality stream's stress harness perturbs).
5. **Byte-exact where the contract is deterministic.** Block continuity and
   `render_audio` determinism are *bit-identical* float-sample comparisons of
   the target block — no tolerances (doc 16 deterministic-rendering rule).
   Extent-honesty is an all-zero-samples assertion. `achieved_rate` honesty
   (`achieved_rate <= request.sample_rate`; a degraded render never reports
   `exact`) is an inequality/flag assertion, not sample-equality.
6. **Concurrency / TSan.** The audio async family drives `AudioCompletion`
   under concurrent `complete`/`cancel`/`take` from multiple threads, exactly
   as `check_async_cancellation` does for `RenderCompletion` — the same
   `Completion<Result>` template, instantiated over `AudioResult`. This family
   MUST be exercised under the contract stream's existing TSan lane
   (`16:66-73`). The suite must introduce no data races in its own generator or
   audio doubles.
7. **≥90% diff coverage** on changed lines (doc 16 CI gate): the new family
   code in `testing/contract_tests.cpp`, the new `Options` handling, and the
   driver's audio fixtures are the coverage surface.

## Acceptance criteria

Concrete, testable checks that say "done":

- **Granular audio entry points exist and are dispatched.**
  `arbc::testing::check_audio_facet_consistency(factory, options)` and
  `arbc::testing::check_audio_async(factory, options)` are declared in
  `testing/arbc/testing/contract_tests.hpp` and defined in
  `testing/contract_tests.cpp`; `Options` gains `audio_consistency{true}` and
  `audio_async{true}` toggles; and `arbc::contract_tests` (dispatch at
  `contract_tests.cpp:547-573`) probes `factory()->audio()` and runs the two
  audio families when it is non-null, skipping them when `nullptr`.
- **New claims-register entry** in `tests/claims/registry.tsv`, paired to a
  `// enforces:` tag in `tests/contract_conformance.t.cpp`:
  - `03-layer-plugin-interface#audio-facet-consistent` — a `Content`'s
    `AudioFacet` is self-consistent: `audio_extent()`/`audio_stability()`/
    `latency()` are idempotent and mutually coherent (`Static` audio ⇔ `nullopt`
    `audio_extent`, `Timed` ⇒ a present `audio_extent`, `latency()` stable and
    defaulting to `Time::zero()`); a `render_audio` window lying entirely
    outside `audio_extent()` yields an all-zero (silent) block; and for
    `Static`/`Timed` audio the target samples are a deterministic,
    block-continuous function of `(snapshot, window, rate, layout)` — two
    requests with equal inputs settle to bit-identical samples, and splitting a
    window at an interior boundary then concatenating the two sub-renders is
    bit-identical to the single-window render. Discovered via `Content::audio()`
    and skipped for audio-less (`nullptr`) and `Live` content.
- **Re-run without re-registration.** The audio async family exercises the
  existing `03-layer-plugin-interface#audio-facet-optional` invariant
  (`render_audio` settles exactly once, inline or via `AudioCompletion`) over
  generated audio content; the driver may add an `enforces:` tag for it but
  does **not** duplicate the registry row.
- **Conformance driver fixtures.** `tests/contract_conformance.t.cpp` gains,
  under a fixed seed, at least: a `Static` audio (tone-like, `nullopt`
  `audio_extent`, deterministic samples), a `Timed` audio (present
  `audio_extent`, deterministic function of `window`), an async-settling audio
  facet, and a re-use of the existing visual-only content to prove `audio()`
  skip. Both audio families pass over every audio-bearing fixture.
- **`scripts/check_claims.py` passes** in both directions after the registry
  addition (forward: the new claim has an enforcing tag; reverse: no tag names
  an unregistered claim).
- **TSan lane green** for the audio async family (concurrent
  `complete`/`cancel`/`take` over `AudioCompletion`).
- **No new WBS follow-up and no design-doc delta** (see Decisions): the audio
  pull-routing property is declared out of scope (owned by the operator
  conformance stream), and the invariants pinned here are already normative in
  doc 12 and `content.hpp`.

## Decisions

1. **One bundled new claim, `#audio-facet-consistent`, not three audio twins.**
   The visual side splits its facet invariants across three claims
   (`#facet-consistency`, `#render-within-declared-bounds`,
   `#render-pure-over-pinned-state`). This task registers a single claim
   bundling the audio analogs (description-method coherence + extent honesty +
   block-continuous determinism). *Rationale:* the deferral that scoped this
   task named exactly one claim, `03-layer-plugin-interface#audio-facet-consistent`
   ([`conformance_suite.md:294-300`](conformance_suite.md)), and the existing
   audio claim `#audio-facet-optional` is itself a bundle (pointer identity +
   `latency` default + settle-once). The three audio sub-invariants share a
   single generated-content drive over the same facet, so one bundled claim
   keeps the register proportional to a 1d task and matches the precedent that
   `#facet-consistency` bundles idempotence-plus-coherence. *Rejected — three
   separate audio claims:* register growth out of proportion to the effort and
   to what the deferral scoped; the sub-invariants are not independently
   consumed.
2. **Claim anchors to doc 03's stem (`03-layer-plugin-interface#…`), not doc
   12's.** *Rationale:* the sibling convention — `audio_facet` Decision 6 and
   all seven `conformance_suite` claims anchor audio/contract invariants to the
   doc-03 stem even when the normative prose lives in doc 11/12/13, because the
   claim id is a conceptual identifier, not a markdown heading anchor. The
   task's own `note` names the id `03-layer-plugin-interface#audio-facet-consistent`.
   *Rejected — `12-audio#…`:* would break the established anchoring convention
   for facet-consistency claims and split the audio facet's invariants across
   two doc stems (`#audio-facet-optional` already lives under doc 03).
3. **No design-doc delta.** *Rationale:* every invariant pinned here is already
   normative — `Static`/`Timed`/`Live` stability "applies verbatim" and a
   `Static` tone reports `nullopt` extent (doc 12:26-29); `render_audio` is a
   pure function of the pinned `AudioRequest.snapshot` (doc 12:79-82,
   `content.hpp:234-241`); the audio description methods and settle discipline
   are fixed in `content.hpp:261-293`. This task turns those promises into a
   testable work order (doc 16's job for a refinement), not new architecture.
   This matches `conformance_suite`'s "No design-doc delta" posture for its
   seven claims. *Rejected — add an audio-facet-consistency section to doc 03:*
   the promise already exists; a new doc section would duplicate doc 12 and
   `content.hpp` rather than constrain anything new.
4. **Block continuity is the load-bearing audio-specific property.** Beyond the
   direct twin of render purity (identical requests ⇒ identical samples), the
   consistency family asserts that splitting a `Static`/`Timed` window at an
   arbitrary interior boundary and concatenating the two sub-block renders is
   bit-identical to rendering the whole window at once. *Rationale:* the mix
   engine renders in fixed blocks (doc 12's block cache, `12:21`), so
   seam-freedom across block boundaries is exactly the property a real audio
   pipeline depends on and the one with no clean visual analog; "render_audio
   block continuity" is what the deferral text named. *Rejected — determinism
   only:* would miss the boundary-seam class of bugs that motivates a
   *block*-based conformance check.
5. **Audio pull-routing is out of scope, not a new deferred task.**
   `PullService::pull_audio` routing (an operator pulls its inputs' audio only
   through the pull seam, never `input->render_audio()` directly) is
   operator-graph *runtime* behavior observable only with a live L4 pull —
   the audio twin of the visual pull-routing that `conformance_suite`
   Decision 3 assigned to `operators.operator_conformance`
   (`tasks/50-operators.tji`). *Rationale:* keeping `arbc-testing` at L3
   forbids depending on the audio engine, so the check cannot live here; it
   belongs with the operator conformance work when operators gain audio (m7),
   under the existing "inputs pulled only via `PullService`" ownership. Naming a
   fresh WBS leaf for it now would duplicate that ownership and risk a task the
   operator stream will absorb. *Rejected — defer `audio.audio_pull_conformance`
   as a new leaf:* it has no stable home until both the audio engine and audio
   operators exist and would overlap `operators.operator_conformance`; recorded
   here as a scope boundary instead.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-07.

- Added `Options::audio_consistency`/`audio_async` toggles and decls for `check_audio_facet_consistency`/`check_audio_async` to `testing/arbc/testing/contract_tests.hpp`.
- Implemented audio generators (`gen_sample_rate`/`gen_layout`/`flicks_per_frame`) + `drive_audio` helper and both family impls (idempotence/coherence, extent-outside silence, determinism, block-continuity; and one-settle-path + concurrent complete/cancel/take over `AudioCompletion`) in `testing/contract_tests.cpp`.
- Added `ToneFacet`/`AudioFixture` doubles (absolute-time-keyed, byte-exact, inline+async) and 4 driver test cases to `tests/contract_conformance.t.cpp`; umbrella dispatch probes `factory()->audio()`.
- Registered new claim `03-layer-plugin-interface#audio-facet-consistent` in `tests/claims/registry.tsv`; `// enforces:` tag in driver exercises it; existing `#audio-facet-optional` re-run with `enforces:` tag (not re-registered).
- 4908 assertions across 13 cases — all green; `check_claims`/`check_levels` clean.
