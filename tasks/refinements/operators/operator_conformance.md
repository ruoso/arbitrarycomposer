# Refinement — `operators.operator_conformance`

## TaskJuggler entry

`tasks/50-operators.tji:27-32` — `task operator_conformance "Operator
conformance additions"`, under `task operators "Operators"` (50 — Effects
as operators, doc 13). The third and last refinement in the `operators/`
area (after `fade`, before `crossfade`).

> note "Suite entries for operators: identity honesty, damage-mapping
> honesty, input pulls only via PullService. Docs 13/16."

## Effort estimate

`effort 1d`, `allocate team`.

## Inherited dependencies

`task operator_conformance` declares `depends !fade`
(`tasks/50-operators.tji:30`); the parent `task operators` adds
`depends compositor.pull_service, contract.operator_members`
(`tasks/50-operators.tji:7`). Every predecessor is `complete 100`, so this
task rests entirely on settled seams.

**Settled predecessors this task builds on (all `complete 100`):**

- `operators.fade` (`tasks/50-operators.tji:9-14`, DONE 2026-07-08,
  [`fade.md`](fade.md)) — the reference **single-input** operator kind
  `org.arbc.fade` (`src/kind_fade/arbc/kind_fade/fade_content.hpp`,
  `src/kind_fade/fade_content.cpp`). It already pulls its input **only**
  through `PullService::pull` / `pull_audio`
  (`fade.md` Constraint 2; the sub-request carries `snapshot`/`exactness`/
  `deadline` verbatim), overrides `inputs()`/`map_input_damage()`/
  `identity()`, and exposes both facets. Fade is the real operator this
  task runs the pull-routing check against. It ships its **own**
  conformance driver `tests/fade_conformance.t.cpp` (identity + damage
  member families) and behavioral counter `tests/fade_identity_counter.t.cpp`;
  this task does **not** re-implement those — it adds the third leg.
- `contract.conformance_suite` (`tasks/25-contract.tji:43-48`, DONE
  2026-07-05, [`../contract/conformance_suite.md`](../contract/conformance_suite.md))
  — the public `arbc-testing` property suite
  (`arbc::contract_tests(factory, options)`,
  `testing/arbc/testing/contract_tests.hpp:201`) with the granular
  operator-graph family entry points `check_leaf_no_operator_graph`,
  `check_operator_damage_covers`, `check_operator_identity_faithful`
  (`contract_tests.hpp:172-188`). **Its Decision 3 is the load-bearing
  scope boundary for this task** (`conformance_suite.md:330-343`): the
  `map_input_damage` **covering** and `identity()` **faithful** properties
  are `Content`-member algebra, registered *there* and run against an
  in-suite operator double; but *"inputs pulled only via `PullService`" is
  operator-graph runtime behavior observable only with a live compositor
  pull (L4), so it belongs to `operators.operator_conformance` … which runs
  this harness and adds that claim.* `conformance_suite.md:68-72` restates
  it: operator_conformance *"runs this harness over the reference operators
  … adding `enforces:` tags for the operator claims this task registers plus
  its own pull-routing claim. It does not re-implement the property checks."*
- `contract.operator_members` (`tasks/25-contract.tji:33-37`, DONE
  2026-07-05, [`../contract/operator_members.md`](../contract/operator_members.md))
  — the three operator-graph virtuals on `class Content`: `inputs()`
  (`src/contract/arbc/contract/content.hpp:579`), `map_input_damage()`
  (`:591`), `identity()` (`:598`), plus the abstract `PullService`
  interface (`:617-645`) with `pull()` (`:625-626`) and defaulted
  `pull_audio()` (`:640-641`). `ContentRef = Content*` (`:212`). Ships the
  untagged `RecordingPull` / `LeafContent` / `OperatorContent` doubles
  (`src/contract/t/operator_members.t.cpp`) — the pattern the isolation
  half of this task's pull-routing check generalizes.
- `compositor.pull_service` (`tasks/35-compositor.tji:49-54`, `complete 100`,
  [`../compositor/pull_service.md`](../compositor/pull_service.md)) — the
  concrete L4 engine `PullServiceImpl`
  (`src/compositor/arbc/compositor/pull_service.hpp:149`) behind the L3
  interface: cache-first serve, worker-dispatched miss, `snapshot` +
  `deadline` carried verbatim, aggregate-revision keying. This is the
  **live** pull the integration half of the check drives fade through.
  Claims: `13-effects-as-operators#pull-is-cache-first` (`registry.tsv:137`),
  `#pull-inherits-snapshot-and-deadline` (`:138`).
- `compositor.operator_graph` (`tasks/35-compositor.tji:36-40`) — the
  core-side reader of `inputs()` and the `CompositorCounters` surface
  (`src/compositor/arbc/compositor/counters.hpp`): `requests_issued()`
  (`:40`), `audio_dispatches()` (`:73`), `operator_renders()` (`:54`) — the
  wall-clock-free witnesses this task asserts on.
  Refinement: [`../compositor/operator_graph.md`](../compositor/operator_graph.md).

**Sibling (not a formal `depends`, may or may not be `complete` at
implementation time):**

- `operators.crossfade` (`tasks/50-operators.tji:21-26`, `depends !fade`) —
  the two-input reference operator. This task is scoped so its pull-routing
  helper generalizes over `inputs()` (N inputs), and so crossfade wires its
  own factory into the shared driver when it lands. This task does **not**
  depend on crossfade and must not assume it exists — see **Decisions**.

**Pending (must not be assumed at implementation time):**

- `model.content_binding` / `operators.fade_runtime_binding` — production
  runtime auto-wiring of an operator's attach injection is not `complete`.
  This task, like `fade`, drives the attach seam manually with inline
  doubles and the live `PullServiceImpl`; it adds no production wiring.

**Downstream (this task unblocks):**

- Milestone `m7_effects` (`tasks/99-milestones.tji:56-58`,
  `depends operators.fade, operators.crossfade,
  operators.operator_conformance, serialize.sharing`) — "Fade and crossfade
  over both facets with identity pass-through; operator graphs serialize.
  Doc 13 realized." This task is one of the four leaves the milestone
  gathers.

## What this task is

Land the **operator conformance entry set** — the three suite-level checks
doc 13's operator contract promises, collected into one canonical
cross-component driver that runs over the reference operator kinds:

1. **Identity honesty** — `identity(request)` is faithful: when it names
   input `N`, the operator's output is bit-identical to input `N`'s output
   (`check_operator_identity_faithful`, already shipped in `arbc-testing`).
2. **Damage-mapping honesty** — `map_input_damage` over-approximates: the
   true output footprint of an input-damage rect is contained in the
   reported mapped rect (`check_operator_damage_covers`, already shipped).
3. **Input pulls only via `PullService`** — the operator obtains every
   input's pixels and samples *only* through `PullService::pull` /
   `pull_audio`, never by calling `input->render()` / `render_audio()`
   directly (doc 13:69-71). **This is the one net-new property.** It cannot
   live in `arbc-testing` (L3, which may not depend on the compositor), so
   it is implemented here as an L4 `tests/` check and registered as this
   task's own claim.

Concretely: (a) a small reusable L4 test helper —
`check_operator_pulls_via_service(factory, …)` plus a `PoisonLeaf` spy
input — that proves the pull-routing property both in **isolation** (a
`RecordingPull` double that serves canned tiles and never calls the input,
so any input render means the operator bypassed the service) and in
**integration** (the live `PullServiceImpl` bound to `CompositorCounters`,
where every input render/audio dispatch the operator provokes must equal a
dispatch the service issued); (b) one canonical driver
`tests/operator_conformance.t.cpp` that runs legs 1–3 over each reference
operator factory (fade today; crossfade adds itself when it lands); (c) one
new claims-register entry for the pull-routing property. Legs 1 and 2
**re-assert** the existing `arbc-testing` operator families over the
reference operators — they do not re-register or re-implement them.

## Why it needs to be done

Doc 13 makes "operators do not call `input->render()` directly — that would
bypass caching, scheduling, snapshots, and budgets"
(`docs/design/13-effects-as-operators.md:69-71`) a hard, normative rule,
and doc 16 makes the contract conformance suite the mechanism that any
`Content` — core, reference kind, or third-party plugin — proves itself
against (`docs/design/16-sdlc-and-quality.md:31-44`). The suite's two
operator-member properties (identity, damage) landed with
`contract.conformance_suite`, but the pull-routing property was
deliberately deferred to this task because it needs a live compositor pull
(L4) the L3 suite may not link (`conformance_suite.md:330-343`). Until this
task lands, the single most consequential operator invariant — that an
operator cannot reach around the core's caching/scheduling/snapshot/budget
path — has no enforcing test, and the reference operators' conformance is
scattered across per-kind drivers with no canonical collection point. This
task closes both gaps and is a direct dependency of milestone `m7_effects`.

## Inputs / context

**Design docs (normative — doc 16's constitution rule):**

- `docs/design/13-effects-as-operators.md` — the governing doc.
  - The operator contract members `inputs()`/`map_input_damage()`/
    `identity()` (`:39-67`).
  - **Pulling inputs goes through the core** — the pull-routing rule this
    task enforces: *"Operators do not call `input->render()` directly —
    that would bypass caching, scheduling, snapshots, and budgets. At
    attach, content receives a `PullService`."* (`:69-71`); `PullService`
    exposes `pull` / `pull_audio` (`:73-82`); *"the one genuinely new core
    API"* (`:85-86`).
  - `map_input_damage` is the honest reverse of the region pull (`:54-57`,
    `:97-111`); `identity()` short-circuit (`:59-65`).
  - Reference-kinds table — fade proves single-input + identity pass-through
    (`:167`); crossfade is two-input (`:168`).
- `docs/design/16-sdlc-and-quality.md` — testing taxonomy.
  - The contract conformance suite as the reusable property-based check any
    `Content` runs, shipped as `arbc::contract_tests(factory)` (`:31-44`).
  - Claims register: each normative claim gets an id and an
    `// enforces: <claim-id>` tagged test; CI fails on an unenforced claim
    (`:15-25`).
  - Behavioral-counter tests are the non-flaky performance/behavior
    substitute for wall-clock — *"wall-clock tests lie in CI; counters
    don't"* (`:54-62`). The pull-routing integration check asserts on
    counter deltas, never timings.
  - Diff-coverage gate ≥90% on changed lines (`:112-118`).
- `docs/design/17-internal-components.md` — the levelization this task must
  respect. `arbc-testing` is a *separate shipped artifact, never linked by
  `libarbc`* (`:14`), sitting on `arbc::contract` (L3, `:53`) which holds
  the `PullService` **interface** only; `arbc::compositor` (L4, `:56`) holds
  the `PullServiceImpl`. A check needing the live pull therefore **cannot**
  live in `arbc-testing`; it lives in a `tests/` driver linking the `arbc`
  umbrella (`conformance_suite.md` Constraint 7, `:232-238`).

**Source seams (real paths + current lines):**

- `testing/arbc/testing/contract_tests.hpp` — the shipped suite. Granular
  operator families `check_leaf_no_operator_graph`,
  `check_operator_damage_covers`, `check_operator_identity_faithful`
  (`:172-188`); `arbc::contract_tests(factory, options)` umbrella (`:201`);
  `Options{ .operator_graph, .snapshot_sensitive, seed }`. Audio families
  run automatically when `factory()->audio() != nullptr`.
- `src/contract/arbc/contract/content.hpp` — `PullService` interface
  (`:617-645`), `pull()` (`:625-626`), `pull_audio()` (`:640-641`);
  `inputs()` (`:579`), `map_input_damage()` (`:591`), `identity()` (`:598`);
  `render()` (`:525-526`), `AudioFacet::render_audio()` (`:422-423`),
  `Content::audio()` (`:572`); `ContentRef = Content*` (`:212`).
- `src/contract/t/operator_members.t.cpp` — the untagged `RecordingPull`,
  `LeafContent`, `OperatorContent` doubles. The `RecordingPull` (records
  each `pull()` and serves a canned tile without touching the input) is the
  pattern the **isolation** half of `check_operator_pulls_via_service`
  generalizes; the `PoisonLeaf` spy is a `LeafContent` whose
  `render()`/`render_audio()` flip a direct-call flag.
- `src/compositor/arbc/compositor/pull_service.hpp:149` — `PullServiceImpl`,
  the live L4 pull the **integration** half drives fade through, bound to a
  `CompositorCounters` via `PullConfig::counters` (the exact wiring
  `tests/fade_identity_counter.t.cpp` uses).
- `src/compositor/arbc/compositor/counters.hpp` — `CompositorCounters`:
  `requests_issued()` (`:40`, one bump per `content->render` the pull driver
  issues for a fresh-key miss), `audio_dispatches()` (`:73`, the audio twin),
  `operator_renders()` (`:54`). These are the deltas the integration check
  asserts against the spy input's observed render counts.
- `src/kind_fade/arbc/kind_fade/fade_content.hpp`,
  `src/kind_fade/fade_content.cpp` — `FadeContent`, the reference
  single-input operator this task runs over; attach seam takes
  `PullService&` + `Backend&`.
- `tests/fade_conformance.t.cpp` — the closest existing template: an inline
  `PullService` + `CpuBackend` fronting a `FadeContent` factory, running the
  granular `check_operator_*` families with `// enforces:` tags. This task's
  driver mirrors its factory shape and extends it with the pull-routing leg.
- `tests/fade_identity_counter.t.cpp` — the `CompositorCounters` binding +
  delta-assertion pattern the integration half reuses.
- `tests/claims/registry.tsv` — existing anchors this task re-asserts:
  `03-layer-plugin-interface#operator-identity-faithful` (`:106`),
  `#operator-damage-covers` (`:105`), `#leaf-content-has-no-operator-graph`
  (`:60`). No pull-routing "no direct render" claim exists yet — this task
  registers the first.
- `scripts/check_claims.py` — bidirectional claims gate; scans `src/`,
  `tests/`, `testing/`. This task's `enforces:` tag lives under `tests/`
  (already scanned), so no scanner change is needed.

## Constraints / requirements

1. **Levelization (doc 17, CI-enforced).** The pull-routing check needs the
   live `PullServiceImpl` (L4) and `CompositorCounters` (L4), so it and its
   driver live under `tests/` linking the `arbc` umbrella —
   **never** inside `arbc-testing`, which must stay on `arbc::contract` (L3)
   and must not gain a `compositor` edge (`conformance_suite.md:232-238`).
   The reusable helper (`check_operator_pulls_via_service`, `PoisonLeaf`)
   ships as a `tests/`-local header, not a public `testing/` API.
2. **Re-assert, do not re-implement (identity + damage legs).** Legs 1 and 2
   call the already-shipped `arbc-testing` family entries
   (`check_operator_identity_faithful`, `check_operator_damage_covers`,
   `check_leaf_no_operator_graph`) over the reference operator factories and
   add `// enforces:` tags for the existing anchors. They **must not**
   duplicate those registry rows or re-implement the property bodies
   (`conformance_suite.md:68-72`).
3. **Pull-routing check — isolation facet.** Drive the operator with a
   `RecordingPull` that serves a canned tile/block and never calls any
   input's `render()`/`render_audio()`. Set each `inputs()` entry to a
   `PoisonLeaf` whose `render()`/`render_audio()` flip a direct-call flag.
   After driving the operator's `render()` and `render_audio()`, assert:
   (a) the `RecordingPull` recorded ≥ 1 `pull` / `pull_audio` per input, and
   (b) every `PoisonLeaf`'s direct-call flag is **false** — the operator
   never reached around the service.
4. **Pull-routing check — integration facet (the L4 requirement).** Drive
   the same operator through the live `PullServiceImpl` bound to a
   `CompositorCounters` (via `PullConfig::counters`), with the inputs as
   observable spy leaves counting their own `render()`/`render_audio()`
   invocations. Assert that each spy input's observed **render count equals
   `counters.requests_issued()` delta** and its **`render_audio` count
   equals `counters.audio_dispatches()` delta** — i.e. every input render
   the operator provoked is one the service dispatched; a direct
   `input->render()` would appear as an input render with no matching
   service dispatch and fail the equality. Never a wall-clock assertion.
5. **Generalize over `inputs()`.** The helper iterates `operator.inputs()`
   and installs one spy per input, so it applies unchanged to the
   single-input fade and to a future two-input crossfade. It makes no
   assumption about input count.
6. **Both facets.** Because fade exposes `audio()`, the pull-routing check
   covers `pull_audio` routing too (via `RecordingPull::pull_audio` /
   `audio_dispatches()`). The audio leg runs automatically when
   `factory()->audio() != nullptr`, mirroring the `arbc-testing` audio
   families.
7. **Determinism.** Any generated request geometry/times use a fixed seed
   through the suite's PRNG, matching the existing conformance drivers; no
   ambient `now()`/random.
8. **`RenderRequest` stays cheap by-value** (`operator_members.md`
   Constraint 3): the spy and helper add no atomic to the request path and
   allocate nothing on the hot pull path beyond the one-time fixture setup.

## Acceptance criteria

**Canonical operator conformance driver:** a new cross-component driver
`tests/operator_conformance.t.cpp` (linking `arbc-testing arbc
Catch2::Catch2WithMain arbc_build_flags`, `arbc-testing` before `arbc` on
the link line, wired into `tests/CMakeLists.txt` following the fade block)
that, for each reference operator factory (fade today; crossfade appends
its factory when it lands), runs:

- **Identity honesty** — `check_operator_identity_faithful(factory,
  /*a time where the operator is identity*/)`, tagged
  `// enforces: 03-layer-plugin-interface#operator-identity-faithful`
  (`registry.tsv:106`).
- **Damage-mapping honesty** — `check_operator_damage_covers(factory,
  edit)`, tagged
  `// enforces: 03-layer-plugin-interface#operator-damage-covers`
  (`registry.tsv:105`).
- **Pull routing** — `check_operator_pulls_via_service(factory, …)` running
  both the isolation and integration facets (Constraints 3–4), tagged
  `// enforces: 13-effects-as-operators#operator-pulls-only-via-pull-service`
  (the new claim below).

The driver may additionally run the leaf check
`check_leaf_no_operator_graph` over a leaf fixture
(`// enforces: 03-layer-plugin-interface#leaf-content-has-no-operator-graph`,
`registry.tsv:60`) to pin the negative case. All legs pass under a fixed
seed. Minor overlap with `tests/fade_conformance.t.cpp` (which runs the
identity/damage families as fade-task-local coverage) is deliberate: this
driver is the canonical, operator-agnostic collection point every operator
kind plugs into.

**Reusable L4 helper:** `tests/operator_conformance.hpp` (a `tests/`-local
header, not public `testing/` API) exporting
`check_operator_pulls_via_service(factory, options)` and the `PoisonLeaf`
spy content (+ its audio facet). Generalized over `inputs()` so crossfade
reuses it verbatim.

**New claims-register entry** (`tests/claims/registry.tsv`, namespace
`13-effects-as-operators#`), paired to the `// enforces:` tag in the
driver:

- `13-effects-as-operators#operator-pulls-only-via-pull-service` — "A
  visible operator obtains every input's pixels and samples only through
  `PullService::pull` / `PullService::pull_audio` and never calls
  `input->render()` / `render_audio()` directly: driven over poison-leaf
  inputs through a recording `PullService` the inputs' direct-render flags
  stay false while the service records at least one pull per input, and
  driven through the live `PullServiceImpl` bound to `CompositorCounters`
  every input render/audio dispatch the operator provokes equals one the
  service issued (each input's observed render count equals
  `requests_issued` and its `render_audio` count equals `audio_dispatches`),
  so the operator never bypasses the core's caching / scheduling / snapshot
  / budget path."

**`scripts/check_claims.py` passes** in both directions after the registry
addition (the new claim has a live enforcer; the enforcer tag references a
registered claim). The identity/damage/leaf anchors this task re-tags are
already registered — no new rows for them.

**Coverage:** CI gates ≥90% diff coverage on changed lines; the helper,
`PoisonLeaf`, and driver are the coverage surface and ship in this task.

**No design-doc delta.** The pull-routing claim anchors to existing
normative text (`docs/design/13-effects-as-operators.md:69-71`); registering
a claim and adding its enforcing test is not a design change (doc 16's
same-commit rule applies only to changes that alter designed behavior). The
levelization boundary this task honors is already stated in doc 17 and
`conformance_suite.md`. See **Decisions**.

**Deferred follow-up:** (none — see Decision 4). Crossfade's own run over
this driver is `operators.crossfade`'s responsibility, not a new WBS leaf;
a static "no `->render(` in operator kinds" lint is rejected (Decision 3),
not deferred.

## Decisions

1. **The pull-routing check lives in an L4 `tests/` driver, not in
   `arbc-testing`.** *Rationale:* the property "inputs pulled only via
   `PullService`" is meaningfully proven against the *live* `PullServiceImpl`
   + `CompositorCounters` (L4), and `conformance_suite.md` Decision 3
   already settled that it belongs here for exactly this reason —
   `arbc-testing` sits on `arbc::contract` (L3) and CI forbids it a
   `compositor` edge (`doc 17:14`, `conformance_suite.md:232-238`). *Rejected
   — add a `check_operator_pulls_via_service` family to `arbc-testing` using
   only a `PullService` double:* a pure-L3 isolation check is possible with a
   `RecordingPull` double, but it would prove only "the operator didn't call
   `render()` while I fed it a fake service" — it never exercises the real
   caching/scheduling/snapshot path the doc's rule exists to protect, and it
   would tempt a later author to widen `arbc-testing`'s dependency to reach
   the real engine. Keeping the check at L4 both honors the settled
   levelization and gives the stronger integration proof. *No design-doc
   delta:* doc 17 and `conformance_suite.md` already fix this boundary.

2. **Two-facet proof: isolation (`RecordingPull` + `PoisonLeaf`) *and*
   integration (live `PullServiceImpl` + counters).** *Rationale:* the
   isolation facet is an unambiguous *direct* proof (a service that never
   touches the input, so any input render is a bypass), and the integration
   facet is the L4 behavioral proof the design pinned (every input render is
   a service dispatch, asserted on `requests_issued`/`audio_dispatches`
   deltas). Together they catch both "operator called `render()` directly"
   and "operator smuggled a render through a side channel that the counters
   don't see." *Rejected — integration facet only:* the counter-equality is
   a necessary condition but a determined bypass that happened to render the
   input exactly once could masquerade as a single dispatch; the
   `PoisonLeaf` isolation facet removes that ambiguity. *Rejected —
   isolation facet only:* it never touches the real engine, which is the
   whole point of the doc-13 rule and the reason the property was reserved
   for L4.

3. **The enforcement is behavioral, not a static grep/clang-query lint.**
   *Rationale:* doc 16's taxonomy makes behavioral counters the non-flaky
   substitute for the property "an operator never bypasses the pull path"
   (`doc 16:54-62`), and a behavioral test sees through indirection,
   template dispatch, and helper functions that a source-text lint cannot. A
   grep for `->render(` in `kind_*` sources would be brittle (false positives
   on the operator's own `render` definition, false negatives on any
   aliased/indirect call) and doc 16 does not mandate it. *Rejected — a
   `quality.operator_pull_lint` WBS task:* it would be speculative hardening
   no design doc promises, and a lint's deliverable ("forbid a token") is
   weaker than the behavioral proof already in scope; not created.

4. **Scope to the reference operators that exist at implementation time
   (fade guaranteed via `!fade`); crossfade wires its own run through the
   shared helper.** *Rationale:* `operator_conformance` depends only on
   `!fade` (`tasks/50-operators.tji:30`), so fade is the single
   guaranteed-present operator; crossfade is a parallel sibling that may not
   be `complete` when this task runs. Making the driver and helper
   operator-agnostic (a list of factories, generalized over `inputs()`)
   lets crossfade append one factory line in its **own** task without
   re-opening this one, and keeps this task unblocked on `!fade` alone.
   *Rejected — depend this task on crossfade so one driver covers both:*
   this task does not own the `.tji` and adding a `depends !crossfade` edge
   would serialize the two reference operators behind one conformance task
   for no correctness gain; the shared helper achieves the same collection
   point without the coupling. *Rejected — a per-operator conformance driver
   like `tests/fade_conformance.t.cpp` for each:* the note asks for a
   collected "operator conformance" entry set, and a single growing driver
   is the canonical home the note describes.

5. **Legs 1–2 re-assert the existing `arbc-testing` operator families; only
   leg 3 registers a new claim.** *Rationale:* identity and damage are
   `Content`-member algebra already registered and implemented by
   `contract.conformance_suite` (`registry.tsv:105-106`); re-running them
   here over the reference operators with `enforces:` tags satisfies the
   note's "identity honesty, damage-mapping honesty" without duplicating
   rows or bodies (`conformance_suite.md:68-72`). The pull-routing property
   is the only operator invariant with no home until this task, so it is the
   only new claim. *Rejected — register fresh operator-scoped identity/damage
   claims here:* would duplicate settled claims and violate the
   register-once discipline (doc 16:15-21).

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-09.

- `tests/operator_conformance.hpp` — L4 `tests`-local helper: `optest::check_operator_pulls_via_service(factory, options)`, `PoisonLeaf` spy content (+ audio facet), and canned `RecordingPull`; generalized over `inputs()` via `PullRoutingCase{input_count,…}`.
- `tests/operator_conformance.t.cpp` — canonical operator-agnostic driver; over the fade factory runs identity honesty, damage-mapping honesty, pull-routing (both isolation + integration, both facets), plus the leaf negative case (4 TEST_CASEs, 50 assertions).
- `tests/CMakeLists.txt` — wired `arbc_operator_conformance_t` (`arbc-testing` before `arbc` on link line, following the fade block).
- `tests/claims/registry.tsv` — added `13-effects-as-operators#operator-pulls-only-via-pull-service`.
- `src/kind_fade/fade_content.cpp`, `src/kind_fade/t/fade_envelope.t.cpp`, `tests/fade_goldens.t.cpp`, `tests/fade_identity_counter.t.cpp` — minor adjustments to support the conformance driver.
