# contract.operator_members — Operator graph members

## TaskJuggler entry

Back-link: `tasks/25-contract.tji:31-35`, `task operator_members "Operator
graph members"` under `task contract`. The verbatim note:

> inputs(), map_input_damage(), identity(); PullService interface
> (implementation lives in the compositor). Doc 13.

## Effort estimate

**1d** (`tasks/25-contract.tji:32`). This is an interface-surface task in the
same weight class as `snapshot_pins` (1d) and `audio_facet` (1d): it adds a
handful of null/identity-default virtuals to one existing class plus one small
abstract interface, with a self-contained unit test. No pixels are rendered and
no scheduling machinery lands — the *use* of these members (aggregate revision,
damage routing, `identity()` short-circuit, actual pulling) belongs to L4
tasks that already exist in the WBS. The day covers writing the members and the
default-behaviour tests, not the graph algorithms.

## Inherited dependencies

**Settled** (parent `task contract` deps, `tasks/25-contract.tji:7`):

- `model.editable_facet` (`6a799ae`, DONE 2026-07-05,
  `tasks/refinements/model/editable_facet.md`) — `StateHandle` and the
  content-state resolver the render request already pins.
- `surfaces.capabilities` (`62ff4df`, DONE 2026-07-05) — the backend surface
  contract the render path targets.

**Sibling substrate (not a formal `depends`, but landed and relied on):**

- `contract.async_render` (`92c3d3b`, DONE 2026-07-05,
  `tasks/refinements/contract/async_render.md`) — `RenderRequest`,
  `RenderResult`, `RenderCompletion`, and the one-code-path `render(request,
  done)` entry point. `PullService::pull` is issued with *these same types*: a
  pull is "the same machinery as a compositor-issued request"
  (`docs/design/13-effects-as-operators.md:74-76`).
- `contract.snapshot_pins` (`1da702a`, DONE 2026-07-05,
  `tasks/refinements/contract/snapshot_pins.md`) — `RenderRequest.snapshot`
  (`StateHandle`), the "snapshot token respected" clause a pull inherits
  (`13:76`), and the cheap-by-value `RenderRequest` invariant this task must
  preserve for the pull seam.

**Downstream (this task unblocks them):**

- `contract.conformance_suite` (`tasks/25-contract.tji:41-46`) declares
  `depends !async_render, !temporal_fields, !operator_members` — the
  property-based public suite is a hard-blocked consumer.
- `compositor.operator_graph` (`tasks/35-compositor.tji:28-31`) — the core's
  *use* of `inputs()`/`map_input_damage()`/`identity()`.
- `compositor.pull_service` (`tasks/35-compositor.tji:33-37`) — the concrete
  `PullService` implementation and the attach-time injection.
- `operators.fade` / `operators.crossfade` (`tasks/50-operators.tji:9-18`,
  `depends compositor.pull_service, contract.operator_members`) and
  `operators.operator_conformance` (`tasks/50-operators.tji:20-24`).
- `kinds.nested` (`tasks/55-kinds.tji:16-20`) — recursive composition built on
  the `PullService` interface.
- `serialize.sharing` (`tasks/60-serialize.tji:33`) — core-owned operator input
  edges beside `params`.

## What this task is

Effects enter the model as content that consumes content: an operator wraps one
or more inputs, transforms their output, and presents itself as ordinary
`Content` (`docs/design/13-effects-as-operators.md:1-8`). This task lands the
**contract surface** that makes that graph visible to the core, as null/identity
-default virtuals on the existing `Content` class plus one abstract service
interface:

1. `inputs()` — the operator's input edges, visible to the core (default: none;
   leaf content is a graph leaf).
2. `map_input_damage(input, rect)` — forward propagation of an input's damage
   into this content's output damage (default: identity).
3. `identity(request)` — the OpenFX-style pass-through action: for this request,
   is the output exactly some input's output? (default: `nullopt`).
4. `PullService` — the abstract interface through which an operator asks the
   core to render an input (cache-first, scheduled, snapshot- and
   budget-inheriting), rather than calling `input->render()` directly. This
   task lands the **interface**; the implementation is a separate L4 task.

Every member is a null/identity default so existing (walking-skeleton, leaf)
content is behaviourally unchanged — the seam `content.hpp:134-135` already
reserved ("the operator-graph members land with their systems").

## Why it needs to be done

`contract.conformance_suite` cannot ship without it (`!operator_members`), and
the entire operator/compositor-graph stream is gated on this surface existing:
`compositor.operator_graph` reads `inputs()` for aggregate revisions and routes
damage through `map_input_damage()`; `compositor.pull_service` implements the
interface declared here; `operators.fade`/`crossfade` and `kinds.nested`
subclass `Content` overriding these members and pull their inputs through
`PullService`; `serialize.sharing` persists the `inputs()` edges. Landing the
contract now — ahead of any implementation — lets all of those proceed against a
stable, testable surface, exactly as `async_render` and `snapshot_pins` landed
the render contract ahead of the compositor that drives it.

## Inputs / context

**Design docs (normative, doc 16):**

- `docs/design/13-effects-as-operators.md:39-89` — "The operator contract".
  Verbatim signatures this task implements:
  - `13:52` — `virtual std::span<const ContentRef> inputs() const { return {}; }`
    ("visible to the core … aggregate revisions, snapshot consistency, cycle
    detection, and damage routing"; "Leaf content returns empty").
  - `13:57` — `virtual Rect map_input_damage(size_t input, const Rect&) const;`
    ("Map damage on an input into damage on this content's output. Default:
    identity. A blur inflates by its radius; a warp maps through its
    distortion").
  - `13:65` — `virtual std::optional<size_t> identity(const RenderRequest&)
    const;` ("If, for this request, the output is exactly input N's output …
    say so"; "the compositor serves the input's cached tiles directly — no
    render, no copy, no new cache entry"; the OpenFX identity action).
  - `13:69-85` — `PullService` with `pull(ContentRef, const RenderRequest&,
    std::shared_ptr<RenderCompletion>)` ("Operators do not call
    `input->render()` directly … At attach, content receives a `PullService`";
    "the one genuinely new core API … the doc-05 synthetic viewport
    generalized").
- `docs/design/13-effects-as-operators.md:104-111` — `map_input_damage` "is the
  same mapping in reverse" of the region pull (a blur pulls the inflated
  region; damage inflates by the same radius). This is the soundness anchor
  (see Decision 5).
- `docs/design/13-effects-as-operators.md:140-161` — input edges are core-owned
  structure that serialize *outside* `params`, mirroring `inputs()`; the basis
  for `ContentRef` being a non-owning edge (Decision 1) and for
  `serialize.sharing`'s scope.
- `docs/design/17-internal-components.md:53` — `arbc::contract` (Level 3)
  "Contents … `PullService` *interface*, damage sinks".
- `docs/design/17-internal-components.md:56` — `arbc::compositor` (Level 4)
  "damage routing over `inputs()`, aggregate revisions, cycle handling,
  `PullService` implementation".
- `docs/design/17-internal-components.md:59` — `arbc::kind-*` "nested uses only
  the `PullService` interface".
- `docs/design/16-sdlc-and-quality.md:31-44` — the property-based public suite
  `arbc::contract_tests(factory)` (deferred: `contract.conformance_suite`).

**Source seams:**

- `src/contract/arbc/contract/content.hpp:136-172` — `class Content`. The three
  members are added after `render` (`:167-168`); `content.hpp:134-135`
  ("Walking-skeleton subset … the operator-graph members land with their
  systems") is the reserved insertion point.
- `src/contract/arbc/contract/content.hpp:67-75` — `struct RenderRequest`
  (cheap, trivially copyable by-value; `identity()` and `PullService::pull`
  take it by `const&`).
- `src/contract/arbc/contract/content.hpp:100-132` — `class RenderCompletion`
  (the one-shot settlement handle a pull carries, unchanged).
- `src/base/arbc/base/geometry.hpp:14-32` — `struct Rect` (the damage-region
  type `map_input_damage` consumes and produces; `intersect`/`empty`/`width`/
  `height` available for tests).
- `src/contract/CMakeLists.txt:1-8` — component wiring: no new `SOURCES` TU is
  required (all members are header-declared, defaults inline; any out-of-line
  bodies go in the existing `content.cpp`). The new test TU
  `t/operator_members.t.cpp` is registered on the `arbc_component_test` line
  beside `snapshot_pins`/`async_render`.
- `src/compositor/tile_planning.cpp:210,261` and
  `src/compositor/compositor.cpp:20,78` — the compositor already names content
  as `Content* content = resolve(layer.content)` and calls
  `content->render(request, done)`; this is the `Content*` idiom `ContentRef`
  resolves to (Decision 1), and the call site a future `PullService`
  implementation generalizes.

**Predecessor / sibling refinements:** `tasks/refinements/contract/
async_render.md` (RenderCompletion/RenderRequest; the L3-interface / L5-driver
split; the doc-03 claim-anchor convention), `tasks/refinements/contract/
snapshot_pins.md` (StateHandle resolution of a provisional type name with no
design-doc delta; cheap-by-value request invariant; "Concurrency: none added").

## Constraints / requirements

1. **Null/identity defaults — leaf content is unchanged.** `inputs()` defaults
   to an empty span, `identity()` to `nullopt`, `map_input_damage()` to the
   identity map. A `Content` subclass that overrides none of them is a graph
   leaf indistinguishable from today's walking-skeleton content. This is the
   observable invariant the primary claim pins.
2. **Verbatim signatures.** Match `docs/design/13:52,57,65` exactly:
   `std::span<const ContentRef> inputs() const`, `Rect map_input_damage(size_t
   input, const Rect&) const`, `std::optional<size_t> identity(const
   RenderRequest&) const`. `<span>` is added to the header includes.
3. **`RenderRequest` stays cheap by-value.** `identity(const RenderRequest&)`
   and `PullService::pull(ContentRef, const RenderRequest&, …)` take the request
   by `const&`; nothing on this path allocates, refcounts, or adds an atomic to
   the request (preserving the `snapshot_pins`/`async_render` invariant).
4. **`PullService` is an abstract L3 interface only.** It declares pure virtuals
   and a virtual destructor; it holds no state and contains no cache/worker/
   scheduling logic (those are L4 `compositor.pull_service`). The interface
   references only types already in `content.hpp` (`RenderRequest`,
   `RenderCompletion`) plus `ContentRef` — no new component edge.
5. **Damage soundness (normative, from `13:104-107`).** `map_input_damage` must
   return output damage that *covers* every output pixel whose value can change
   when the named input's given rect changes: over-approximation is sound,
   under-approximation drops repaint and is a bug. The default identity map
   satisfies this for pass-through-shaped content; the header doc-comment states
   the covering requirement explicitly (Decision 5).
6. **Levelization.** No new component edge. `arbc::contract` is Level 3
   (`17:53`); `ContentRef` is intra-contract, `PullService` references only
   contract-local types, and `Rect`/`StateHandle` edges already exist
   (`content.hpp:4,6`). `scripts/check_levels.py` gates.
7. **Diff coverage ≥90% (doc 16).** The test exercises both the default path
   (a leaf test `Content`) and an override path (an operator-shaped test
   `Content` and a test `PullService` double), covering every added line.

## Acceptance criteria

Test TU: `src/contract/t/operator_members.t.cpp` (Catch2), self-contained test
`Content`s and a test `PullService` — links no higher component.

**Claims-register growth.** Register in `tests/claims/registry.tsv` (TAB
`<claim-id>\t<description>`, gated both directions by
`scripts/check_claims.py`), enforced by a `// enforces:` tagged test:

- `03-layer-plugin-interface#leaf-content-has-no-operator-graph` — A `Content`
  that overrides none of the operator-graph members is a graph leaf: `inputs()`
  returns an empty span, `identity()` returns `nullopt` for every request, and
  `map_input_damage(i, r)` returns `r` unchanged. (Anchored to doc 03, the
  interface where the members live, per the sibling convention; doc 13
  motivates them.) No such claim exists yet.

**Deterministic unit assertions** (no golden bytes — nothing is rendered):

- *Override path — `inputs()`*: an operator-shaped test `Content` holding N
  input `Content*` edges returns them via `inputs()` in declared order with
  pointer identity preserved (the span views the operator's storage; no copy).
- *Override path — `identity()`*: the test operator returns the configured input
  index for a pass-through request and `nullopt` otherwise (request-scoped).
- *Override path — `map_input_damage()` soundness*: a test operator with an
  inflating map returns an output rect that `covers` the input rect
  (`out.intersect(in) == in`), pinning the over-approximation direction of
  Constraint 5 for a known operator.
- *`PullService` shape*: `static_assert(std::is_abstract_v<PullService>)`; a
  test double implementing `pull` records that it received the exact
  `ContentRef` and a `RenderRequest` equal by value to the one passed, and that
  the operator's `std::shared_ptr<RenderCompletion>` is forwarded unchanged —
  demonstrating the pull carries the render contract's own request/completion
  types with no new settlement path.

**No goldens / behavioral counters / concurrency in this task.** No pixels are
rendered, so byte-exact goldens (`16:48-53`) do not apply. Behavioral-counter
assertions ("identity fades issue zero operator renders") require the compositor
that counts renders — deferred to `compositor.counters`
(`tasks/35-compositor.tji:51-55`). **Concurrency: none added** — `PullService`
is a stateless interface; its thread-safe implementation and TSan/stress
coverage land with `compositor.pull_service`. This note is explicit so the
closer does not scope concurrency work here.

**Deferred (owners already WBS leaves — no new task):**

- The **general damage-soundness property** over arbitrary operators, the
  `identity()` **short-circuit** ("no render, no copy, no new cache entry"), and
  **facet consistency** — these are the property-based public suite's job:
  `contract.conformance_suite` (`tasks/25-contract.tji:41-46`) and
  `operators.operator_conformance` (`tasks/50-operators.tji:20-24`, "identity
  honesty, damage-mapping honesty, input pulls only via PullService"). **No new
  leaf.**
- The **`PullService` implementation** (cache-first lookup, worker scheduling,
  snapshot/budget inheritance) and the **attach-time injection** of the service
  into content: `compositor.pull_service` (`tasks/35-compositor.tji:33-37`).
  **No new leaf.**
- The **core's use of the graph members** (aggregate revisions, damage routing,
  cycle detection + recursion-depth budget, `identity()` short-circuit in
  planning): `compositor.operator_graph` (`tasks/35-compositor.tji:28-31`).
  **No new leaf.**
- The **audio pull** (`pull_audio(ContentRef, const AudioRequest&, …)`): lands
  with `contract.audio_facet` (`tasks/25-contract.tji:26-29`), which owns
  `AudioRequest` (Decision 3). **No new leaf.**

**No under-registered follow-ups.** Every deferral above maps to an existing WBS
leaf; this task registers no new task.

## Decisions

1. **`ContentRef` is a non-owning `Content*` graph edge.** Define
   `using ContentRef = Content*;` (with a forward `class Content;`) so
   `inputs()` returns `std::span<const ContentRef>` and `PullService::pull`
   takes a `ContentRef` by value.
   *Rationale:* it matches how the compositor already names content after
   resolution (`Content* content = resolve(layer.content)`,
   `src/compositor/tile_planning.cpp:210`, `src/compositor/compositor.cpp:20`);
   it is trivially copyable, keeping the `inputs()` span and the pull seam
   allocation-free and preserving the cheap-by-value request invariant; and
   input edges are explicitly *core-owned structure* (`13:142`), so the operator
   is a non-owning observer of its inputs — a raw non-owning pointer states
   exactly that lifetime relationship.
   *Rejected — `ObjectId`:* operators may have inline (unshared) inputs with no
   stable model id at the contract level, and `ObjectId`↔`Content` resolution is
   model/runtime concern; the *core-visible* graph (`13:48`) is of live
   `Content`, while ids are a serialization projection (`13:145-158`,
   `serialize.sharing`'s scope).
   *Rejected — an owning smart-pointer edge:* would give the operator co-
   ownership of input lifetime, contradicting "core-owned structure".
   *No design-doc delta:* this resolves the provisional doc-13 type name
   `ContentRef` to the project's existing `Content*` idiom, altering no designed
   behavior — the same move `snapshot_pins` made for `SnapshotToken*`→
   `StateHandle` and `async_render` for `Error`→`RenderError`, so doc 16's
   same-commit amend rule is not triggered.

2. **`PullService` is a pure-virtual abstract interface at L3; the
   implementation is deferred to `compositor.pull_service` (L4).** The contract
   declares `class PullService { public: virtual ~PullService() = default;
   virtual void pull(ContentRef, const RenderRequest&,
   std::shared_ptr<RenderCompletion>) = 0; };`.
   *Rationale:* `17:53` places the `PullService` *interface* in `arbc::contract`
   (L3) and `17:56` places the *implementation* in `arbc::compositor` (L4);
   `17:59` has the nested kind depend on the interface only. The cache-first /
   worker-scheduling machinery a pull performs (`13:75-76`) lives above L3
   (workers/scheduling are compositor/runtime), so a concrete implementation
   here would invert the levelization. An abstract interface at the lower level
   with the implementation above mirrors the established surface-backend pattern
   (`17:51`, the backend contract at L2; `backend-cpu` implements at L3).
   *Rejected — concrete `PullService` in contract:* pulls request/cache/worker
   machinery below its level; `scripts/check_levels.py` would reject the edge.

3. **This task lands `pull` (render) only; `pull_audio` is deferred to
   `contract.audio_facet`.** `PullService` gains `pull_audio(ContentRef, const
   AudioRequest&, std::shared_ptr<RenderCompletion>)` when the audio facet
   lands.
   *Rationale:* `pull_audio` takes `AudioRequest`, an audio-facet type owned by
   `contract.audio_facet` (`tasks/25-contract.tji:26-29`), which has not landed
   and is not a dependency of `operator_members` — so this task cannot reference
   it. The audio pull is implemented by `arbc::audio-engine` ("pull-based mix",
   `17:57`), a different component from the compositor that implements the video
   pull, so the two halves of `PullService` split cleanly along the facet
   boundary the rest of the contract already uses.
   *Rejected — forward-declare `AudioRequest` now:* creates an undefined-type
   placeholder and a hidden forward dependency on an unlanded task; splitting
   the interface along the facet seam is cleaner and matches how the contract
   grows facet-by-facet. Deferral maps to the existing `contract.audio_facet`
   leaf — no new task.

4. **No attach/injection hook is added to `Content` in this task.** How content
   receives a `PullService` "at attach" (`13:72`) is not implemented here.
   *Rationale:* the three graph members need no `PullService`, and no operator
   kind lands in this task to consume one, so an injection hook would be
   speculative, unconsumed L3 surface. The concrete service and the mechanism
   that hands it to content are the same L4 concern —
   `compositor.pull_service`'s implementation and wiring.
   *Rejected — add `virtual void attach(PullService&)` now:* injection policy
   belongs with the implementation, not ahead of it; adding it here would be an
   untested virtual with no caller. Deferral maps to the existing
   `compositor.pull_service` leaf — no new task.

5. **`map_input_damage`'s covering (over-approximation) requirement is stated
   normatively in the header doc-comment, without a design-doc delta.** Doc 13
   says "Default: identity … A blur inflates by its radius; a warp maps through
   its distortion" (`13:54-56`) and that the map "is the same mapping in
   reverse" of the region pull (`13:106-107`), but does not use the words
   "conservative"/"cover". The header comment makes the entailed covering
   direction explicit: mapped output damage must cover every affected output
   pixel; over-approximation is sound.
   *Rationale:* the covering relation is already *entailed* by `13:106-107` —
   the region pull for a blur is "the region inflated by its radius", and the
   forward damage map being its reverse is inherently a covering (inflating)
   relation. Making an entailed requirement explicit in the work order and the
   header comment is a clarification, not a behavior change, so doc 16's
   same-commit amend rule is not triggered (the same reasoning `snapshot_pins`
   used for resolving a provisional name). The *general* property is enforced by
   `contract.conformance_suite`/`operators.operator_conformance` over arbitrary
   operators; this task pins the direction for its concrete test operator only.
   *Rejected — a doc-13 delta adding "conservative over-approximation":* would
   amend the doc for a requirement it already entails; if a human reviewer reads
   the sharpening as new normative content rather than clarification, it is
   surfaced for the parking lot (see return summary) rather than encoded as a
   WBS task.

6. **Claims anchor to doc 03, not doc 13.** The single registered claim id is
   `03-layer-plugin-interface#leaf-content-has-no-operator-graph`.
   *Rationale:* the operator-graph members are additions to the doc-03 `Content`
   interface; behaviour lives where the contract is stated, per the explicit
   convention in both sibling refinements ("Claims anchored to doc 03 (the
   render contract), not docs 11/14/16"). Doc 13 is the motivating doc, cited in
   Inputs.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-05.

- `src/contract/arbc/contract/content.hpp` — added `<cstddef>`/`<span>` includes, `using ContentRef = Content*;` alias (non-owning graph edge), three virtual graph members (`inputs()` inline empty-span default, `map_input_damage`/`identity` declared), and abstract `PullService` class (pure-virtual `pull` + virtual dtor, non-copyable).
- `src/contract/content.cpp` — out-of-line identity defaults for `Content::map_input_damage` (returns `rect` unchanged) and `Content::identity` (returns `std::nullopt`).
- `src/contract/t/operator_members.t.cpp` — new Catch2 TU (5 cases, 26 assertions): leaf-default path (claim-enforcing), `inputs()` order/pointer identity, request-scoped `identity()`, `map_input_damage()` over-approximation covering, `PullService` abstractness + exact ref/request/completion forwarding via `RecordingPull`.
- `src/contract/CMakeLists.txt` — `t/operator_members.t.cpp` appended to `arbc_component_test` source list.
- `tests/claims/registry.tsv` — registered claim `03-layer-plugin-interface#leaf-content-has-no-operator-graph` (A `Content` overriding none of the operator-graph members is a graph leaf: `inputs()` empty span, `identity()` nullopt, `map_input_damage` identity map).
- No tech-debt follow-ups: all deferrals (`pull_audio`, injection hook, implementation, graph use, conformance property) map to existing WBS leaves.
