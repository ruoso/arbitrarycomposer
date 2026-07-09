# Refinement — `operators.crossfade`

## TaskJuggler entry

`tasks/50-operators.tji:21-26` — `task crossfade "org.arbc.crossfade"`, under
`task operators`. `effort 1d`, `allocate team`, `depends !fade`. Note:

> "Two-input operator: extent union, per-input time handling — the temporal
> transition primitive; with spans this is cuts-and-dissolves. Doc 13."

Design one-liner this expands: `docs/design/13-effects-as-operators.md:168`
(the `org.arbc.crossfade` reference-kind row).

## Effort estimate

1 day. This is the two-input generalization of `org.arbc.fade` (2d): the
operator seam, both-facet structure, `identity()` contract, conformance
harness, and golden/counter test shapes were all built by `operators.fade`
and `operators.operator_conformance` and are reused here rather than
re-derived. The net-new work is (a) a second input and the union extent,
(b) the over-based visual dissolve + complementary-weight audio mix in place
of fade's single-input attenuation, and (c) endpoint (`{0}`/`{1}`) identity.

## Inherited dependencies

**Settled predecessors this task builds on (all `complete 100`):**

- `operators.fade` (`tasks/refinements/operators/fade.md`, DONE 2026-07-08)
  — the both-facet operator template. Header shape
  (`src/kind_fade/arbc/kind_fade/fade_content.hpp`): a `Content` subclass with
  `static constexpr const char* kind_id` (`:74`), a `FadeParams` struct, an
  `attach(PullService&, Backend&)` seam (`:53`), `std::array<ContentRef, N>
  d_inputs` stable storage backing `inputs()` (`:95`), an inner `AudioFacet`
  member (`:79-91,98`), and borrowed `d_pull`/`d_backend` pointers (`:96-97`).
  Behavioral idioms reused verbatim: fully-open path pulls the input straight
  into the caller's target (`fade_content.cpp:91-116`); partial path clears a
  temp and `backend.composite(target, temp, temp_to_dst, e)`
  (`fade_content.cpp:133-158`); per-frame audio gain multiply
  (`fade_content.cpp:239-245`); `Stability::Timed` returned unconditionally on
  both facets (`fade_content.cpp:37,201`); `identity()` sound only where
  `render()` reproduces the claimed input bit-for-bit (`fade_content.cpp:50-52`).
- `operators.operator_conformance` (`tasks/refinements/operators/operator_conformance.md`,
  DONE 2026-07-09) — the canonical operator-agnostic conformance driver
  `tests/operator_conformance.t.cpp` and its arity-generic helper
  `tests/operator_conformance.hpp`. `check_operator_pulls_via_service(factory,
  {.input_count = N})` (`operator_conformance.hpp:176`) is already written to
  accept `N` inputs (`OperatorPullFactory` takes `std::span<const ContentRef>
  inputs`, `:143-144`; `PullRoutingCase::input_count`, `:147`); the header
  comment (`:16`) names "a future crossfade (2 inputs)" as reusing it verbatim.
  Crossfade appends one factory to the driver's list with `input_count = 2`
  and re-opens nothing.
- `compositor.pull_service` / `contract.operator_members` (transitively via
  `operators`, `tasks/50-operators.tji:7`) — the `PullService` interface
  (`src/contract/arbc/contract/content.hpp:617-645`), the operator-graph
  virtuals on `Content` (`inputs()` `:579`, `map_input_damage()` `:591`,
  `identity()` `:598`), and the `operator_graph` machinery
  (`src/compositor/arbc/compositor/operator_graph.hpp`) that already
  generalizes over input arity (`aggregate_revision` folds over `inputs()`
  `:105-107`; `resolve_identity` `:159-160`).

**Pending (must not be assumed at implementation time):**

- Runtime attach-injection wiring. Like fade, the built-in reference kinds
  are not registered into any `Registry` and are constructed directly in
  tests today; live-service injection of `PullService`/`Backend` at
  instantiation (and teardown on release) is out of scope here and deferred
  to a named follow-up (see Acceptance criteria → Deferred follow-up),
  mirroring `operators.fade_runtime_binding` (`tasks/50-operators.tji:15-19`).
- JSON serialization of `org.arbc.crossfade` params **and its two-input
  DAG** — rides with the serialize stream (`serialize.kind_params` +
  `serialize.sharing`, M7). Crossfade's second input is the first reference
  kind that genuinely needs the shared-content `"$ref"` table
  (`docs/design/13-effects-as-operators.md:155-161`); that is the serialize
  stream's concern, not this task's.

## What this task is

`org.arbc.crossfade` is the reference two-input temporal-transition operator:
it wraps two content inputs (`from`, `to`) and, driven by a single
time-evaluated **crossfade position** `w(t) ∈ [0,1]`, presents a blend of the
two as ordinary content across both facets. Before the transition window it is
input 0; after it, input 1; during it, a dissolve. Its output extent is the
**union** of its inputs' extents. Combined with layer spans and time maps
(doc 11), it is the primitive an NLE builds cuts-and-dissolves from
(`docs/design/13-effects-as-operators.md:168,204-206`).

The central design question — "what does *crossfade position* mean in terms of
the existing gain/attenuation primitives?" — is resolved **without a new
primitive** (Decision 1): the visual facet realizes the position as a single
source-over dissolve of input 1 over input 0 at opacity `w` (the exact
`Backend::composite` seam fade uses), and the audio facet realizes it as a
per-frame complementary-weight additive mix `s0·(1−w) + s1·w` (audio's native
additive combination). At `w == 0` and `w == 1` the operator renders by pulling
the corresponding input straight through, making its `identity()` claims sound
at both endpoints.

## Why it needs to be done

- **Doc 13 promises it as a v1 reference kind** (`13:168`) — the second, and
  only *multi-input*, reference operator. It proves the operator mechanism on
  the multi-input path the way fade proved it on the single-input path:
  `inputs()`-driven aggregate revisions, per-input damage routing, and
  two independent pulls that each hit the time-keyed cache
  (`13:91-96,112-116`).
- **It is the last piece the editing story needs.** "With spans (doc 11),
  audio (doc 12), and crossfades, v1 is capable of real cuts-and-dissolves
  editing end to end" (`13:204-206`). Doc 14 treats a crossfade insert as one
  atomic placement/graph transaction (`14:99-101,226-227`); this task supplies
  the operator that transaction inserts.
- **Downstream:** the serialize stream's shared-content `"$ref"` support
  (`13:155-161`) and any NLE-facing tooling both need a concrete two-input
  operator to exercise. The conformance driver already reserved the arity-2
  slot for it (`operator_conformance.hpp:16`).

## Inputs / context

**Design docs (normative — doc 16's constitution rule):**

- `docs/design/13-effects-as-operators.md:168` — the crossfade reference-kind
  row: "Two-input operator, extent union, per-input time maps — the temporal
  transition primitive (with spans, enough to build an NLE's
  cuts-and-dissolves)."
- `13:91-96` — synchronous metadata composition: an operator computes
  `bounds()`/`time_extent()`/`scale_range()`/`stability()` from its inputs plus
  its own contribution; "a crossfade's extent is the union of its inputs'";
  a fade "is `Timed` even over `Static` input (its envelope depends on time)"
  — the same rule makes a time-varying crossfade `Timed` over two `Static`
  inputs.
- `13:59-65` — the `identity()` contract: "If, for this request, the output is
  exactly input N's output … say so: the compositor serves the input's cached
  tiles directly — no render, no copy, no new cache entry."
- `13:69-72` — operators pull inputs only through the `PullService`, never
  `input->render()` directly.
- `13:112-120` — per-input, per-facet pull shape: each input is "an ordinary
  pull, each hitting the time-keyed cache"; audio is "identical shape one
  dimension down".
- `13:24-27` — both facets: "a fade operator attenuates alpha *and* gain …
  Operators may be facet-selective." Crossfade drives both.
- `docs/design/00-overview.md:52-53` — "additive mixing with per-layer gain"
  (the audio combination model the audio facet uses).
- `docs/design/11-time-and-video.md:62-73` — `span` and `time_map`
  (`local_time = (parent_time − in) × rate + offset`); the per-clip retiming
  that rides on the *inputs*, not on the crossfade (Decision 4).
- `docs/design/07-color-and-pixel-formats.md` — premultiplied linear working
  space (claim `07-color-and-pixel-formats#compositing-in-working-space`),
  which is what makes source-over-at-opacity a correct attenuation.
- `docs/design/17-internal-components.md:28-31,59` — `kind-crossfade` is a
  built-in L4 `arbc::kind-*` target beside `kind-fade`; the `PullService`
  *interface* is L3 (`contract`, `:53`), its *implementation* + counters are
  L4 (`compositor`, `:56`).

**Source seams (real paths + current lines):**

- Operator/base contract — `src/contract/arbc/contract/content.hpp`:
  `inputs()` (`:579`), `map_input_damage()` (`:591`), `identity()` (`:598`),
  `render()` (`:525-526`); `ContentRef = Content*` (`:212`); `AudioFacet`
  (`:395-427`) with `render_audio()`/`audio_extent()`/`audio_stability()`;
  `PullService` (`:617-645`) with `pull()` (`:625-626`) and defaulted
  `pull_audio()` (`:640-641`).
- Template to mirror — `src/kind_fade/arbc/kind_fade/fade_content.hpp` +
  `src/kind_fade/fade_content.cpp` (structure enumerated under Inherited
  dependencies). CMake: `src/kind_fade/CMakeLists.txt` (`NAME kind_fade`,
  `DEPENDS contract`).
- Backend seam — `src/surface/arbc/surface/backend.hpp`: `composite(dst, src,
  src_to_dst, opacity)` source-over on premultiplied alpha (`:37-40`),
  `clear()` (`:35`). **No additive op exists** (see Decision 1).
- Audio block layout — interleaved float32, `samples[f * channels + c]`
  (`src/media/arbc/media/audio_block.hpp:42-47`); additive-mix precedent
  `samples[...] += gain * s` (`src/kind_nested/nested_content.cpp:603`).
- Compositor operator graph — `src/compositor/arbc/compositor/operator_graph.hpp`
  (`is_operator()` `:83`, `aggregate_revision` `:105-107`,
  `route_operator_damage` `:131-133`, `resolve_identity` `:159-160`) —
  arity-generic, no change needed. `PullServiceImpl` at
  `src/compositor/arbc/compositor/pull_service.hpp:149`.
- Counters — `src/compositor/arbc/compositor/counters.hpp`: `operator_renders()`
  (`:54`), `requests_issued()` (`:40`), `audio_dispatches()` (`:73`).
- Conformance — `tests/operator_conformance.hpp`
  (`check_operator_pulls_via_service` `:176`, `OperatorPullFactory` `:143-144`,
  `PullRoutingCase::input_count` `:147`) and driver `tests/operator_conformance.t.cpp`
  (fade factory `:103-110`, the three legs `:115-143`). Shipped property suite:
  `testing/arbc/testing/contract_tests.hpp` (families `:172-188`,
  `arbc::contract_tests` `:201`).
- Fade test precedents — `tests/fade_conformance.t.cpp`, `tests/fade_goldens.t.cpp`
  (inline `constexpr` byte tables + `[.regen]` dumper), `tests/fade_identity_counter.t.cpp`;
  wiring in `tests/CMakeLists.txt:48-89`. Claims: `tests/claims/registry.tsv:139-141`.

## Constraints / requirements

1. **Levelization (doc 17, CI-enforced).** New component `src/kind_crossfade/`
   (`NAME kind_crossfade`, `DEPENDS contract`), an L4 `arbc::kind-*` target
   mirroring `kind_fade`. Its include closure is `contract`-only: it must not
   include `surface`, `compositor`, or `media` backends directly. Any test that
   needs `CpuBackend` or `PullServiceImpl` (goldens, identity counter,
   conformance) therefore lives in `tests/`, not `src/kind_crossfade/t/` —
   exactly the placement fade used (`tests/fade_goldens.t.cpp` etc.).
2. **Pull discipline (doc 13:69-72).** Both inputs are pulled only through the
   injected `PullService` (`d_pull->pull` / `d_pull->pull_audio`), once per
   input, never `input->render()`. Each pull carries the request's snapshot,
   exactness, and deadline verbatim (fade precedent, `fade_content.cpp:102,218`).
3. **Extent = union (doc 13:91-96, task note).** `bounds()`, `time_extent()`,
   and `AudioFacet::audio_extent()` each return the **union** of the two
   inputs' respective extents — not a pass-through (fade's single-input
   pass-through does not generalize) and never an intersection.
4. **`Timed`-over-`Static` (doc 13:93).** `stability()` and `audio_stability()`
   return `Stability::Timed` unconditionally, because `w(t)` depends on time —
   even when both inputs are `Static`. As with fade, a `Timed` visual facet
   must report `achieved_time = request.time`, and a `Timed` audio facet must
   declare a bounded `audio_extent` (here the union of the inputs' extents,
   already bounded when both inputs are).
5. **Endpoint identity soundness (doc 13:59-65).** `identity(request)` returns
   `{0}` iff `w(request.time) == 0.0` exactly and `{1}` iff `w == 1.0` exactly,
   else `std::nullopt`. This is sound **only because** `render()` at each
   endpoint pulls the corresponding input straight through (Constraint 6), so
   the compositor serving that input's cached tiles is bit-identical to what
   `render()` would produce. The operator must never claim identity in the
   interior.
6. **Endpoint render is a direct pass-through.** `render()` at `w == 0` pulls
   input 0 directly into the caller's target (no temp, no composite); at
   `w == 1` pulls input 1 directly; in the interior it pulls input 0 into the
   target, then `backend.composite(target, temp1, temp1_to_dst, w)` — one
   source-over of input 1 over input 0 at opacity `w`. This keeps `render()`
   bit-faithful to `identity()` at both ends (mirrors `fade_content.cpp:91-158`).
7. **Damage mapping is identity (doc 13:54-57).** Crossfade neither moves nor
   scales pixels; both inputs blend in the shared output coordinate space, so
   `map_input_damage(input, rect)` returns `rect` for either input (the
   contract default). `check_operator_damage_covers` verifies the
   over-approximation.
8. **Deterministic rendering → byte-exact goldens (doc 16).** The visual
   dissolve and audio mix are pure functions of inputs and `w`; goldens are
   frozen `constexpr` byte tables compared by `memcmp`, no tolerance
   (fade precedent).

## Acceptance criteria

**Conformance (property suite).** `tests/crossfade_conformance.t.cpp` builds a
`testing::ContentFactory` over `CrossfadeContent` fronted by an inline
`PullService` + `CpuBackend`, runs `arbc::contract_tests(factory,
{.operator_graph = true, .snapshot_sensitive = false})`
(`contract_tests.hpp:201`) plus granular `check_operator_damage_covers` and
`check_operator_identity_faithful(factory, /*time where w==0*/)`; audio
families auto-run because `factory()->audio() != nullptr`.

**Pull-routing (reuse the canonical driver).** Append a `crossfade_pull_factory`
wrapping `inputs[0]`/`inputs[1]` to `tests/operator_conformance.t.cpp` and call
`check_operator_pulls_via_service(factory, {.input_count = 2})`
(`operator_conformance.hpp:176`). The helper injects two `PoisonLeaf` inputs and
asserts each input is pulled ≥1 time via the service and never rendered
directly — pinning Constraint 2 for both inputs. Re-asserts (tagged, not
re-registered) `13-effects-as-operators#operator-pulls-only-via-pull-service`
(`registry.tsv`).

**Behavioral counter (identity).** `tests/crossfade_identity_counter.t.cpp`
binds `CompositorCounters` and asserts `operator_renders()` delta **0** at
`w == 0` and at `w == 1` (identity short-circuit, no operator render) and delta
**1** for an interior `w` (`counters.hpp:54`). Re-asserts
`13-effects-as-operators#identity-plan-issues-no-operator-render`.

**Byte-exact goldens.** `tests/crossfade_goldens.t.cpp`, inline `constexpr`
byte tables + `[.regen]` dumper (fade shape): (a) a visual tile at `w == 0.5`
over two differently-colored solid inputs; (b) an audio block at `w == 0.5`
over two distinct tone inputs; plus behavioral cases asserting the `w == 0`
tile equals input 0's and the `w == 1` tile equals input 1's (bit-exact).
Re-asserts `16-sdlc-and-quality#byte-exact-goldens`.

**New claims register entries** (`tests/claims/registry.tsv`, namespace
`13-effects-as-operators#`), each with an `// enforces:`-tagged test:

- `crossfade-mixes-both-facets` — combines two inputs across both facets by a
  single time-evaluated position `w(t)`: visual via source-over of input 1
  over input 0 at opacity `w`, audio via per-frame `s0·(1−w) + s1·w`; enforced
  by the goldens + the `w==0`/`w==1` pass-through cases.
- `crossfade-extent-union` — `bounds()`, `time_extent()`, and `audio_extent()`
  are the union of the two inputs'; enforced by an extent unit test over two
  inputs with disjoint bounds/extents.
- `crossfade-timed-over-static` — reports `Timed` on both facets even over two
  `Static` inputs; enforced by a stability test.
- `crossfade-identity-at-endpoints` — `identity()` returns `{0}` at `w==0` and
  `{1}` at `w==1`, `nullopt` in between; enforced by the identity counter +
  conformance identity leg.

Re-asserted (via `// enforces:` tags, not re-registered):
`03-layer-plugin-interface#operator-damage-covers`,
`#operator-identity-faithful`,
`13-effects-as-operators#operator-pulls-only-via-pull-service`,
`#identity-plan-issues-no-operator-render`,
`16-sdlc-and-quality#byte-exact-goldens`.

**Coverage.** CI gates ≥90% diff coverage on changed lines — the tests above
are part of this task, not a follow-up.

**Deferred follow-up** (closer registers in WBS, milestone `m9_release`,
mirroring `operators.fade_runtime_binding`):

- `operators.crossfade_runtime_binding` — "Wire `CrossfadeContent` attach
  injection (`PullService`, `Backend`) onto live services at runtime
  instantiation, tear down on release." `effort 0.5d`, `allocate team`,
  `depends !crossfade, model.content_binding`, note citing this refinement and
  Doc 13.

## Decisions

1. **Crossfade position `w(t)` is realized on existing gain/attenuation
   primitives — no new primitive, no design-doc delta.** Visual: one
   `backend.composite(target, temp1, temp1_to_dst, w)` — source-over of input 1
   over input 0 at opacity `w`, the exact seam fade uses
   (`backend.hpp:37-40`). For opaque inputs this yields the textbook linear
   crossfade `in0·(1−w) + in1·w` with correct full alpha (source-over of an
   opaque source at opacity `w` gives `in1·w + in0·(1−w)`, alpha 1). Audio:
   per-frame complementary-weight additive mix `s0·(1−w) + s1·w`, audio's
   native combination (`00:52-53`, nested's `+= gain·s` at
   `nested_content.cpp:603`).
   *Rationale:* the orchestrator's brief and doc 13 both frame crossfade as
   reusing the gain/attenuation primitives; the `Backend` exposes only
   source-over `composite` (no additive op), and this realization needs
   nothing more. Doc 13:167-168 already promises "both facets" and "extent
   union" — this turns the promise concrete without deviating, so no doc delta.
   *Rejected:* (a) **adding an additive `Backend` op** (e.g.
   `accumulate(dst, src, weight)`) to do a symmetric `in0·(1−w)+in1·w` in
   premultiplied space — a new L3/L4 backend seam every backend (CPU + future
   GPU) must implement, i.e. a doc-09 delta, for a v1 whose inputs are
   overwhelmingly opaque clip frames; deferred to a real need (see Open
   questions / parking lot). (b) **Two stacked source-over composites** (input 0
   at opacity `1−w`, then input 1 at opacity `w`) — mathematically wrong for
   opaque overlap: alpha comes out `1−w+w²` (0.75 at `w=0.5`), an under-full
   result. Rejected on correctness.
2. **Endpoints render by direct pass-through so identity is sound at both
   ends.** `render()` at `w==0` pulls input 0 straight into the target, at
   `w==1` pulls input 1 straight in; `identity()` returns `{0}`/`{1}`
   accordingly.
   *Rationale:* fade's soundness rule — claim identity only where `render()`
   reproduces the input bit-for-bit (`fade_content.cpp:50-52,91-116`).
   Pulling the endpoint input directly makes the compositor's tile-serving
   short-circuit bit-identical to a render, so both endpoints cost nothing
   outside the transition window — which is exactly the cuts-and-dissolves
   economy doc 13 wants (a clip crossfaded in costs nothing before and after
   the transition).
   *Rejected:* claiming `identity(1)` at `w==1` while rendering `w==1` via the
   interior over-composite — unsound for a non-opaque input 1 (the over leaves
   `in0·(1−a1)` showing through), the same trap fade avoids by never claiming
   identity at `E==0`.
3. **Position envelope is a linear ramp over a `[start, start+duration)`
   window; `w==0` before, `w==1` after.** `CrossfadeParams { Time start; Time
   duration; }`, `w(t) = clamp((t − start)/duration, 0, 1)`.
   *Rationale:* matches fade's linear-only envelope decision (other shapes were
   YAGNI there); before/after the window collapse to the sound endpoint
   identities, which is precisely a cut framed by a dissolve.
   *Rejected:* shipping smoothstep/equal-power/other curves now — YAGNI; the
   `shape` space stays closed, added later only against a real need.
4. **Per-input time handling = two independent same-time pulls; clip retiming
   rides on the inputs' spans/time maps (doc 11), not on the crossfade.** Each
   input receives its own `RenderRequest`/`AudioRequest` at the request's time
   and is pulled independently, each hitting the time-keyed cache on its own
   (`13:112-116`). The crossfade carries no time-remap of its own.
   *Rationale:* doc 11 already places affine timeline retiming
   (`local_time = (parent_time − in)×rate + offset`) on layer placement
   (`11:62-73`); an NLE crossfade sits over two already-retimed input layers.
   Putting a redundant remap on the operator would duplicate that seam and
   contradict "with spans this is cuts-and-dissolves" (the spans *are* the
   retiming). "Per-input time maps" (doc 13:168) is satisfied by the two pulls
   being independent RenderRequests, each independently time-keyed — not by a
   crossfade-owned remap.
   *Rejected:* a crossfade-carried per-input `time_map` parameter — duplicates
   doc 11's placement retiming for no v1 need; if a genuine operator-local
   offset ever surfaces it is an additive parameter defaulting to zero, not a
   structural requirement.
5. **Fixed arity 2 via `std::array<ContentRef, 2>`; damage mapping is the
   identity default.** `CrossfadeContent(ContentRef from, ContentRef to,
   CrossfadeParams)`, `std::array<ContentRef, 2> d_inputs{from, to}` backing
   `inputs()`; `map_input_damage` returns `rect` for both inputs.
   *Rationale:* crossfade's arity is genuinely fixed at 2 (unlike nested's
   variadic `std::vector<ContentRef>`), so the array mirrors fade's arity-1
   array exactly and keeps storage stable for the `inputs()` span; both inputs
   share the output coordinate space, so damage is identity — no override
   needed beyond confirming the default under `check_operator_damage_covers`.

## Open questions

(none — all decided). One item is deferred to the human-review parking lot
(not a WBS task, since it is a design judgment, not implementable work): if a
future need arises for a *symmetric additive* crossfade of **semi-transparent**
layers (where the over-based visual dissolve of Decision 1 is asymmetric
between inputs), that requires an additive `Backend` op and a doc-09 delta.
Surfaced in the return summary for the parking lot.

## Status

**Done** — 2026-07-09.

- `src/kind_crossfade/arbc/kind_crossfade/crossfade_content.hpp` — `CrossfadeContent` header: `kind_id`, `CrossfadeParams`, `attach(PullService&, Backend&)`, `std::array<ContentRef, 2> d_inputs`, inner `AudioFacet` member, borrowed `d_pull`/`d_backend` pointers.
- `src/kind_crossfade/crossfade_content.cpp` — full implementation: endpoint pass-through identity, interior source-over dissolve via `backend.composite`, per-frame complementary-weight audio mix, `Stability::Timed` unconditionally on both facets, extent-union for `bounds()`/`time_extent()`/`audio_extent()`.
- `src/kind_crossfade/CMakeLists.txt` — L4 `kind_crossfade` component, `DEPENDS contract`.
- `src/kind_crossfade/t/crossfade_position.t.cpp` — unit tests: position/identity, Timed-over-Static, extent-union, structure.
- `src/CMakeLists.txt` — `add_subdirectory(kind_crossfade)`.
- `tests/crossfade_conformance.t.cpp` — `contract_tests` + `check_operator_damage_covers` + `check_operator_identity_faithful`; uses `BoundedAudioLeaf` (not tones) to satisfy Timed `audio_extent()` requirement.
- `tests/crossfade_goldens.t.cpp` — inline `constexpr` byte tables: visual dissolve @ `w=0.5`, audio mix @ `w=0.5`, `w==0`/`w==1` pass-through identity; `[.regen]` dumper.
- `tests/crossfade_identity_counter.t.cpp` — `operator_renders()` delta 0/0/1 at `w==0`/`w==1`/interior.
- `tests/CMakeLists.txt` — three new executables wired in.
- `tests/operator_conformance.hpp` / `tests/operator_conformance.t.cpp` — arity-2 `crossfade_pull_factory` + `check_operator_pulls_via_service({.input_count = 2})`.
- `tests/claims/registry.tsv` — four new claims: `crossfade-mixes-both-facets`, `crossfade-extent-union`, `crossfade-timed-over-static`, `crossfade-identity-at-endpoints`.
