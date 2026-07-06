# contract.conformance_suite — Contract conformance suite (`arbc-testing`)

## TaskJuggler entry

Back-link: [`tasks/25-contract.tji:43-48`](../../25-contract.tji).

```
task conformance_suite "Contract conformance suite (arbc-testing)" {
  effort 4d
  allocate team
  depends !async_render, !temporal_fields, !operator_members
  note "Property-based public suite: render purity, scale/time honesty,
        bounds honesty, damage soundness, capture/restore round-trips,
        async completion + cancellation, facet consistency. Shipped
        artifact. Doc 16."
}
```

## Effort estimate

**4d** (`tasks/25-contract.tji:44`). The heaviest task in the contract
stream — 3–4× the 1d sibling surfaces (`temporal_fields`,
`operator_members`, `snapshot_pins`) and above the 3d `async_render`. The
weight is real: this task ships a *new standalone artifact* (`arbc-testing`,
a static library with a public header surface — the first non-OBJECT-library
build target in the tree) plus a property-based harness that exercises every
branch of the `Content` contract the three sibling tasks just completed, plus
a seeded generator, plus the core-repo conformance driver that pins the new
claims.

## Inherited dependencies

**Settled (formal `depends`):**

- `contract.async_render` (`92c3d3b`, DONE 2026-07-05,
  [`async_render.md`](async_render.md)) — the unified sync/async `render()`
  entry point, `RenderCompletion` (one-shot settle, `cancelled()`/`cancel()`,
  `take()`, `settled()`), `RenderError`, and the `Exactness`/`Deadline`
  request disciplines. The suite's "async completion and cancellation"
  property family drives exactly this surface.
- `contract.temporal_fields` (`957fd12`, DONE 2026-07-05,
  [`temporal_fields.md`](temporal_fields.md)) — `RenderRequest.time`,
  `RenderResult.achieved_time`, the pure-virtual `time_extent()`, the
  three-way `Stability { Static, Timed, Live }` enum, and `arbc::base`'s
  half-open `TimeRange`. The "scale/time honesty" and "bounds/extent honesty"
  properties read these.
- `contract.operator_members` (`2106b3f`, DONE 2026-07-05,
  [`operator_members.md`](operator_members.md)) — the null/identity-default
  operator virtuals `inputs()`, `map_input_damage()`, `identity()`,
  `ContentRef`, and the abstract `PullService` interface. The operator
  covering/identity properties drive these; `operator_members.t.cpp` already
  ships the untagged `RecordingPull`, `LeafContent`, and `OperatorContent`
  test doubles this suite generalizes.

**Sibling substrate (not a formal `depends`, but landed and relied on):**

- `contract.snapshot_pins` (DONE 2026-07-05, [`snapshot_pins.md`](snapshot_pins.md))
  — `RenderRequest.snapshot` (`StateHandle`) makes render a pure function of
  `(state, region, scale, time)`. The "render purity" property pins content
  under a fixed snapshot; the "capture/restore round-trip" property exercises
  the `Editable` facet against it.
- `model.editable_facet` (transitive via the `contract` parent's `depends`)
  — the `Editable` facet the round-trip property discovers and, when absent
  (`Live` content), skips.

**Downstream (this task unblocks them):**

- `operators.operator_conformance` (`tasks/50-operators.tji:20-24`) — runs
  *this* harness over the reference operators (`org.arbc.fade`,
  `org.arbc.crossfade`), adding `enforces:` tags for the operator claims this
  task registers plus its own pull-routing claim. It does not re-implement
  the property checks.
- Every reference kind (`kinds` stream) and third-party plugin — each links
  `arbc-testing` and calls `arbc::contract_tests(factory)` as its conformance
  gate. This is the "plugin quality scales without review capacity" story
  (`docs/design/16-sdlc-and-quality.md:41-44`).

## What this task is

Build **`arbc-testing`** — a reusable, property-based conformance suite that
any `Content` implementation runs against, shipped as public API
(`arbc::contract_tests(factory)`). The suite generates randomized requests
(regions, scales, times) and edit sequences under a caller-supplied seed and
asserts the algebraic invariants the `Content` contract promises: render
purity, scale honesty, bounds/extent honesty, damage soundness,
capture/restore round-trips, async completion + cancellation, operator
covering/identity, and facet consistency. This task delivers the library, its
public header, the seeded generator, and a core-repo conformance driver that
runs the suite over purpose-built test doubles (one per contract branch) and
pins the new invariants as claims-register entries.

## Why it needs to be done

Doc 16 calls the conformance suite "the crown jewel"
(`docs/design/16-sdlc-and-quality.md:31`) and doc 00 lists it as a shipped
public API in the decision record (`docs/design/00-overview.md:143`). The
three sibling tasks (`async_render`, `temporal_fields`, `operator_members`)
each deliberately deferred their *arbitrary-content, property-based*
verification to this task, keeping their own tests to concrete deterministic
doubles (see the deferrals at `async_render.md:348-350`,
`temporal_fields.md:287-290`, `operator_members.md:251-257`,
`snapshot_pins.md:219-223`). Those deferred properties have nowhere to live
until this harness exists. It is a hard downstream blocker for
`operators.operator_conformance` and every reference-kind conformance gate,
and it is the integration coverage the design leans on: "the contract suite
plus real in-memory implementations cover integration seams; mocks are
reserved for fault injection" (`docs/design/16-sdlc-and-quality.md:227`).

## Inputs / context

### Design docs (normative, doc 16)

- **`docs/design/16-sdlc-and-quality.md:31-44`** — the authoritative
  definition. The seven property families the suite MUST enforce (render
  purity; scale/time honesty; bounds/extent honesty; damage soundness;
  capture/restore round-trips; async completion + cancellation; facet
  consistency), the mandate for *property-based generation under a seed*
  ("the contract is algebraic, so test it algebraically", `16:39-40`), and
  the public-API shape `arbc::contract_tests(my_content_factory)` (`16:41-44`).
- **`docs/design/16-sdlc-and-quality.md:15-21`** — the claims-register
  mechanism the suite plugs into: each normative claim gets an id and a test
  tagged `// enforces: <claim-id>`; CI fails on an unenforced claim.
- **`docs/design/03-layer-plugin-interface.md:69-133`** — the contract being
  conformed to. Description facet `bounds()`/`scale_range()`/`stability()`/
  `time_extent()` (`03:74-78`); the single `render()` entry point, sync inline
  vs async `nullopt`+`done->complete()` (`03:83-84`, `03:117-121`);
  `RenderResult` honesty fields (`03:53-60`); operator members (`03:98-102`);
  the damage sink (`03:104-109`); "exact requests must be faithful or report
  `exact=false` honestly" (`03:124-127`); reference-kind coverage table
  (`03:180-191`, the fixtures the suite is ultimately validated against).
- **`docs/design/11-time-and-video.md:88-125,134-137`** — the temporal
  contract tests the suite adds: time-honesty ("`Timed` content returns
  identical pixels for identical times"), `achieved_time` correctness,
  span/extent consistency (`11:123-125`); `Static` content ignores `time`
  (`11:108-109`); "`Static` layers' cached tiles remain valid" across clock
  advance (`11:134-137`).
- **`docs/design/13-effects-as-operators.md:39-95,104-120`** — the operator
  member invariants: `map_input_damage` over-approximates (default identity,
  `13:54-57`); `identity(request)` short-circuit is faithful (`13:59-65`);
  synchronous metadata composition (`13:91-95`); pulls go through the core
  `PullService`, never `input->render()` directly (`13:69-83`).
- **`docs/design/14-data-model-and-editing.md:165-174,265`** — the
  capture/restore round-trip and damage-honesty discipline the contract
  imposes; `Live` content opts out of `Editable` (`14:173-174`), so
  facet-consistency treats `Editable` as optional.
- **`docs/design/17-internal-components.md:14,53,151`** — where the artifact
  lives and what it may depend on: `arbc-testing` (static) is a *separate
  shipped artifact, never linked by `libarbc`* (`17:14`); its sources live in
  `testing/` (`17:151`); it drives `arbc::contract` (L3, `17:53`) which holds
  the `PullService` *interface* only.

### Source seams

- `src/contract/arbc/contract/content.hpp` — the surface under test.
  `Content` pure virtuals `bounds()` (`:172`), `stability()` (`:173`),
  `time_extent()` (`:184`), `render(request, done)` (`:208-209`); operator
  members `inputs()` (`:216`), `map_input_damage()` (`:228`), `identity()`
  (`:235`); `RenderRequest` (`:76-84`), `RenderResult` (`:86-99`),
  `RenderCompletion` (`:119-151`), `Exactness`/`Deadline` (`:35`,`:56-63`),
  `Stability` (`:25`), `RenderError` (`:41`), `ContentRef` (`:160-161`),
  `PullService::pull` (`:262-263`). The covering-requirement note at
  `content.hpp:219-227` already says the operator members are "enforced over
  arbitrary operators by the public conformance suite" — this task makes good
  on that promise.
- `src/base/arbc/base/time.hpp` — `Time` (flicks, `:10-19`) and half-open
  `TimeRange` (`:28-43`) the temporal properties consume.
- `src/contract/t/{async_render,temporal_fields,operator_members,snapshot_pins}.t.cpp`
  — the existing deterministic doubles this suite generalizes into reusable
  fixtures. `operator_members.t.cpp` (`RecordingPull`, `LeafContent`,
  `OperatorContent`) is the closest existing analog; its operator-property
  `TEST_CASE`s are currently *untagged* — this task formalizes them as claims.
- `src/kind_solid/arbc/kind_solid/solid_content.hpp` — a concrete `Content`
  (`org.arbc.solid`, minimal sync/`Static`) available now as a real fixture.
- `cmake/ArbcComponent.cmake` — `arbc_add_component` (OBJECT libs folded into
  the `arbc` umbrella, `:18-40`), `arbc_component_test` → `arbc_<name>_t`
  (`:46-56`). There is **no** helper for a standalone static library that
  exports a public header set; this task adds one.
- `tests/claims/registry.tsv` — the claims register (`<claim-id>\t<desc>`,
  ids `<doc-file-stem>#<slug>`). Existing contract claims: `#render-pure-over-pinned-state`
  (`:50`), `#render-inline-or-async` (`:51`), `#leaf-content-has-no-operator-graph`
  (`:52`), `#render-completion-settles-once` (`:53`), `#render-time-honest`
  (`:69`), `#static-time-invariant` (`:70`).
- `scripts/check_claims.py` — bidirectional claims gate; scans `src/` and
  `tests/` only (`:32-37`), regex `enforces:\s*([A-Za-z0-9#_./-]+)` (`:15`).
  Does **not** scan `testing/` — a gap this task closes (Decision 4).

## Constraints / requirements

1. **New standalone artifact `arbc-testing`.** A `STATIC` library, sources
   under `testing/` per `17:151`, public header exported under
   `testing/arbc/testing/contract_tests.hpp` (installed for plugin authors).
   Depends only on `arbc::contract` (L3) and its transitive lowers
   (`base`, `media`, `surface`, `model`) plus `Catch2` — **never** on the
   `arbc` umbrella library, and nothing in `libarbc` may depend on it
   (`17:14`, a CI-enforced directional edge). Add an
   `arbc_add_testing_library` (or equivalent) helper to
   `cmake/ArbcComponent.cmake`; do not fold it into `arbc_finalize_library`.
2. **Public API.** The umbrella entry point is `arbc::contract_tests(factory,
   options)` matching the name doc 16 fixed (`16:41`). `factory` produces a
   fresh `Content` per invocation (`std::function<std::unique_ptr<Content>()>`);
   `options` carries the seed and coverage toggles. Also expose granular
   per-family entry points (e.g. `arbc::testing::check_render_purity(...)`)
   so downstream tasks can run one family in isolation. The suite discovers a
   content's facets through the contract's facet-discovery virtuals (Editable,
   audio) and skips families a content does not expose — a `Static` solid
   content passes without an `Editable` facet.
3. **Property-based, seeded, deterministic.** Generation of regions, scales,
   times, and edit sequences is driven by a caller-supplied seed through a
   suite-owned PRNG. No wall-clock, no `Date`/system randomness (the base ban
   on ambient `now()`/random still applies). A fixed seed reproduces an exact
   case set — this is what makes the suite CI-stable and what the quality
   stream's stress harness perturbs.
4. **All seven doc-16 property families implemented**, plus the two operator
   member properties (doc 03:98-102 / doc 13). Families that map to
   already-registered claims (render purity, async completion+cancellation,
   time honesty, leaf operator graph) re-run those checks over arbitrary
   generated content but do **not** re-register the claim; the five genuinely
   new invariants (scale honesty, bounds/extent honesty, damage soundness,
   capture/restore round-trip, facet consistency) plus the two operator
   properties get new claims (Acceptance criteria).
5. **Byte-exact assertions where the contract is deterministic.** Render
   purity, damage soundness, capture/restore round-trip, and operator
   identity are *bit-identical* comparisons of rendered output — no
   tolerances (doc 16 deterministic-rendering rule). Honesty properties
   (scale/time/bounds) are inequality/containment assertions, not
   pixel-equality.
6. **Concurrency.** The async completion+cancellation family drives
   `RenderCompletion` under concurrent `complete`/`cancel`/`take` from
   multiple threads; this family MUST be covered under TSan (the contract
   stream's TSan lane already exists from `quality.stress_harness`). The
   suite must not itself introduce data races in its generator or fixtures.
7. **Levelization respected** (`17:41-44`, CI-enforced): `arbc-testing`
   depends only on strictly-lower components. The suite drives the
   `PullService` *interface* but the concrete implementation lives in
   `arbc::compositor` (L4). Operator properties that need a live pull are
   satisfied with an in-suite deterministic `PullService` double (generalized
   from `RecordingPull`), not by depending on the compositor — keeping
   `arbc-testing` at the contract level.
8. **≥90% diff coverage** on changed lines (doc 16 CI gate); the conformance
   driver's fixtures and the suite's own property code are the coverage
   surface.

## Acceptance criteria

Concrete, testable checks that say "done":

- **Library target.** `arbc-testing` builds as a `STATIC` library, exports
  `testing/arbc/testing/contract_tests.hpp`, and links into a downstream test
  binary. The CI component/levelization check (doc 17) passes with the
  `never-by-libarbc` edge intact.
- **Conformance driver.** A new `tests/contract_conformance.t.cpp`
  (cross-component, so it may pull `kind_solid` and purpose-built doubles)
  instantiates the suite over one fixture per contract branch — a `Static`
  sync content (`org.arbc.solid`), a `Timed` content, a resolution-bounded
  content (`achieved_scale < requested`), an async-completing content, an
  `Editable` content, a single-input operator, and a leaf — under a fixed
  seed, and every property family passes.
- **New claims-register entries** in `tests/claims/registry.tsv`, each paired
  to a `// enforces:` tag in `tests/contract_conformance.t.cpp`:
  - `03-layer-plugin-interface#render-scale-honest` — `RenderResult.achieved_scale`
    never exceeds `request.scale`, and `exact` is never `true` when the
    content could not render faithfully at `achieved_scale`.
  - `03-layer-plugin-interface#render-within-declared-bounds` — render
    produces no coverage outside `bounds()`; a request whose region lies
    entirely outside `bounds()` (or, for `Timed` content, outside
    `time_extent()`) yields empty/transparent output.
  - `03-layer-plugin-interface#undamaged-regions-stable` — after an edit that
    reports damage `D`, re-rendering any region disjoint from `D` returns
    bit-identical pixels (damage soundness).
  - `03-layer-plugin-interface#capture-restore-roundtrip` — for content
    exposing the `Editable` facet, rendering after `restore(capture(s))` is
    bit-identical to rendering at state `s`.
  - `03-layer-plugin-interface#facet-consistency` — declared facets are
    self-consistent: description methods (`bounds`/`scale_range`/`stability`/
    `time_extent`) are idempotent and mutually coherent, and the `Editable`
    facet is optional (`Live` content need not expose it).
  - `03-layer-plugin-interface#operator-damage-covers` — an operator's
    `map_input_damage` over-approximates: the true output footprint of any
    input-damage rect is contained in the reported mapped rect (never
    under-reports).
  - `03-layer-plugin-interface#operator-identity-faithful` — when
    `identity(request)` returns input index `N`, the operator's rendered
    output for that request is bit-identical to input `N`'s output.
- **Re-run without re-registration.** The suite exercises the six existing
  contract claims (`#render-pure-over-pinned-state`, `#render-inline-or-async`,
  `#render-completion-settles-once`, `#render-time-honest`,
  `#static-time-invariant`, `#leaf-content-has-no-operator-graph`) over
  generated content; the driver may add `enforces:` tags for these but does
  not duplicate the registry rows.
- **`scripts/check_claims.py` passes** in both directions after the
  registry additions and the scan-root extension (Decision 4).
- **TSan lane green** for the async completion+cancellation family
  (concurrent `complete`/`cancel`/`take`).
- **Deferred follow-up:** audio-facet conformance is deferred to
  `contract.audio_conformance` (effort ~1d) — extend `arbc-testing`'s
  facet-consistency and async families to the `AudioFacet` (audio-extent
  honesty, `audio_stability`, `render_audio` block continuity) and register
  `03-layer-plugin-interface#audio-facet-consistent`; depends on
  `contract.audio_facet` and this task, wired under the contract milestone.
  The closer registers it in the WBS.

## Decisions

1. **`arbc-testing` uses Catch2 directly and ships as a Catch2-based suite.**
   *Rationale:* doc 16 fixes the entry point `arbc::contract_tests(factory)`
   and the repo already standardizes on Catch2 v3 (`CMakeLists.txt:57-68`);
   plugin authors call the suite from inside their own `TEST_CASE`, so a
   Catch2 dependency is the path of least surprise and gives per-property
   pass/fail reporting for free. *Rejected — a framework-agnostic assertion
   callback interface:* it would let plugins use GoogleTest/doctest, but doc
   16 names one API and one framework is already the house standard;
   abstracting the assertion sink is speculative generality for zero current
   call sites. *No design-doc delta:* doc 16 already specifies the API name
   and property list; this only refines the header shape (adding a seed/
   options struct and granular entry points), which is the refinement's job.

2. **The suite is validated against purpose-built contract-level doubles now,
   not the full reference-kind table.** *Rationale:* only `org.arbc.solid`
   exists today; `raster`/`imageseq`/`fade`/`crossfade` land in the `kinds`
   and `operators` streams later. The harness is *complete* (all families
   implemented) and validated against one deterministic double per contract
   branch (generalized from the existing `.t.cpp` doubles). Each reference
   kind wires its own `arbc::contract_tests` run as it ships — that is the
   kind's task, not a new deferred task here. *Rejected — block this task on
   the reference kinds:* would invert the dependency (the kinds depend on the
   suite to gate themselves) and stall the crown-jewel artifact behind
   unrelated work. *No design-doc delta:* doc 03:190-191 already frames the
   reference kinds as the suite's eventual validation set, growing over time.

3. **Operator covering/identity properties are registered here; the
   pull-routing property stays with `operators.operator_conformance`.**
   *Rationale:* `map_input_damage` and `identity()` are `Content` *members*
   (doc 03:98-102), so their invariants belong to the contract-level suite
   and are exercised on an in-suite operator double. "Inputs pulled only via
   `PullService`" is operator-graph *runtime* behavior observable only with a
   live compositor pull (L4), so it belongs to `operators.operator_conformance`
   (`tasks/50-operators.tji:20-24`), which runs this harness and adds that
   claim. *Rejected — register everything operator-related here:* the suite
   sits at L3 and must not depend on the compositor; the pull-routing check
   needs L4 machinery. *Rejected — defer the two member properties to the
   operators stream:* they are pure contract-member algebra, testable now
   against a double, and the `content.hpp:219-227` note already promises the
   public suite enforces them.

4. **`scripts/check_claims.py` scan roots extend to include `testing/`.**
   *Rationale:* doc 17:151 puts the suite sources under `testing/`, but the
   claims gate currently scans only `src/` and `tests/` (`check_claims.py:32-37`).
   The enforcing `enforces:` tags for this task's claims live in the
   cross-component driver under `tests/` (already scanned), so the gate passes
   today — but any future `enforces:` tag placed in suite source under
   `testing/` would be silently invisible, a latent trap. Adding `testing/` to
   the scan roots now (a one-line, mechanical change) keeps the claims tooling
   consistent with the doc-17 layout. *Rejected — leave the scanner and forbid
   tags under `testing/`:* an undocumented placement rule that the next author
   trips over; the scanner change is cheaper and durable. *No design-doc
   delta:* this is tooling hygiene, not a design change — doc 17's layout is
   unchanged; the scanner is being brought into line with it.

5. **Property generation is seeded and suite-owned; no ambient randomness.**
   *Rationale:* doc 16:39-40 mandates seeded property generation, and the
   repo bans `Date.now()`/system randomness for determinism/resume. A caller-
   supplied seed threaded through a suite-owned PRNG makes every run
   reproducible and makes the quality stream's stress harness able to perturb
   the seed deterministically. *Rejected — hand-picked case tables:* doc 16 is
   explicit that "the contract is algebraic, so test it algebraically";
   enumerated cases miss the tail the sibling tasks deferred here precisely to
   catch. *No design-doc delta:* directly implements doc 16:39-40.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-05.

- Shipped `arbc-testing` as a `STATIC` library with public header `testing/arbc/testing/contract_tests.hpp` and umbrella entry point `arbc::contract_tests(factory, options)` plus per-family granular entry points (`testing/contract_tests.cpp`, `testing/CMakeLists.txt`).
- Added `arbc_add_testing_library` CMake helper to `cmake/ArbcComponent.cmake` with headers-only dependency propagation; the library depends only on `arbc::contract` (L3) and Catch2, never on the `arbc` umbrella.
- Wired conformance driver `tests/contract_conformance.t.cpp` (9 TEST_CASEs, one fixture per contract branch: Static sync, Timed, resolution-bounded, async-completing, Editable, single-input operator, leaf) under a fixed seed; added its CMake entry to `tests/CMakeLists.txt`.
- Registered 7 new claims in `tests/claims/registry.tsv`: `#render-scale-honest`, `#render-within-declared-bounds`, `#undamaged-regions-stable`, `#capture-restore-roundtrip`, `#facet-consistency`, `#operator-damage-covers`, `#operator-identity-faithful`; 6 existing claims re-tagged with `enforces:` in the driver.
- Extended `scripts/check_claims.py` scan roots to include `testing/` (Decision 4); levelization passes with `arbc-testing` never linked by `libarbc`.
- All 7 families implemented and property-based (seeded PRNG, caller-supplied seed); async family green under TSan.
- Tech-debt follow-up deferred: `contract.audio_conformance` (~1d) — extend facet-consistency/async families to `AudioFacet`, register `03-layer-plugin-interface#audio-facet-consistent`; depends on `contract.audio_facet` + this task; wired to m6_audio in the WBS.
