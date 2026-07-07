# Refinement — `kinds.nested`

## TaskJuggler entry

`tasks/55-kinds.tji:36-41` — `task nested "org.arbc.nested"`, under
`task kinds "Reference kinds"` (55 — Reference kinds, docs 03/05/11/12/17).

> note "Recursive composition on PullService: synthetic viewport/monitor,
> both facets, bounds/extent/stability aggregation memoized on aggregate
> revision, two-level caching, snapshot consistency for Droste scenes. Doc 05."

## Effort estimate

`effort 4d`, `allocate team`.

## Inherited dependencies

From the parent `task kinds`: `depends contract.conformance_suite`
(`tasks/55-kinds.tji:4`). The nested leaf adds two `depends` of its own:
`compositor.pull_service, color.working_space` (`tasks/55-kinds.tji:39`).

**Settled predecessors this task builds on (all `complete 100`):**

- `compositor.pull_service` — the concrete L4 pull engine
  `PullServiceImpl` (`src/compositor/arbc/compositor/pull_service.hpp:89`)
  behind the L3 `PullService` interface
  (`src/contract/arbc/contract/content.hpp:327-340`). Nested reuses its
  cache-first serve, worker-dispatched miss, snapshot+deadline **verbatim**
  inheritance, aggregate-revision keying, and the recursion-depth budget +
  cycle diagnostic backstop. Claims already landed and re-usable:
  `13-effects-as-operators#pull-is-cache-first`,
  `#pull-inherits-snapshot-and-deadline` (`registry.tsv:112-113`),
  `05-recursive-composition#graph-walk-bounds-cycles` (`:110`).
  Refinement: `tasks/refinements/compositor/pull_service.md`.
- `color.working_space` — the per-composition `SurfaceFormat working_space`
  record (`src/model/arbc/model/records.hpp:104-111`),
  `Transaction::set_working_space`, and `DocRoot::working_space()`
  (`src/model/arbc/model/model.hpp:53-60`). That refinement explicitly
  handed the nesting-boundary conversion (doc 07 rule 4) to `kinds.nested`;
  see **Decisions** for the split. `DocRoot::working_space()` already notes
  "true multi-composition selection lands with `kinds.nested`"
  (`model.hpp:57-58`). Claim landed:
  `07-color-and-pixel-formats#compositing-in-working-space` (`:28`).
  Refinement: `tasks/refinements/color/working_space.md`.
- `contract.conformance_suite` — the public `arbc-testing` property suite
  (`arbc::contract_tests(factory, options)`,
  `testing/arbc/testing/contract_tests.hpp`), including the operator-graph
  family entry points (`check_operator_damage_covers`,
  `check_operator_identity_faithful`, `check_leaf_no_operator_graph`,
  `contract_tests.hpp:117-147`). Nested's conformance gate.
- `contract.operator_members` — `inputs()`, `map_input_damage()`,
  `identity()` on `Content` (`content.hpp:284-308`), and the `PullService`
  interface. Nested is the reference multi-input operator over these seams.
- `color.kernels` — the format/transfer `convert_kernel`
  (`src/backend_cpu/kernels.hpp:153-164`), routing through the working
  space, byte-exact per directed pair. Landed but not yet exposed through a
  `Backend` operation; see the deferred conversion follow-up.

**Pending (must not be assumed at implementation time):**

- `contract.audio_facet` — **not** `complete 100`
  (`tasks/25-contract.tji:27-31`). No `AudioFacet` / `AudioRequest` /
  `render_audio` type exists in source (only the forward-reference comment
  at `content.hpp:325-326`). Nested therefore cannot implement the audio
  facet today; the audio half of "both facets" is deferred to
  `kinds.nested_audio` (see Acceptance criteria).
- `model.content_binding` — not `complete 100`. Production runtime
  auto-wiring of nested's attach-time injection (`PullService`, `Backend`,
  resolver, pinned model) rides it; see the deferred follow-up
  `kinds.nested_runtime_binding`. Nested's own tests drive the attach seam
  manually, exactly as `kinds.raster` drives its sink registration.

## What this task is

Implement `org.arbc.nested`, the reference content kind that embeds one
composition inside another as ordinary content — the design's keystone
feature (doc 05:1-7), the proof that the layer contract is truly arbitrary
(the compositor itself lives behind it) and that deep zoom is structurally
infinite. A nested content wraps a reference to a **child composition**; its
local space is the child's composition space identically (the parent layer's
transform is the only mapping). It renders by running the child composition
through a **synthetic viewport** (device rect = the request's target,
camera = the request's region-to-surface mapping) — "rendering *is*
recursion" (doc 05:24-31) — pulling each child layer's content through the
injected `PullService` (so cache-first serve, worker dispatch, snapshot and
deadline inheritance, aggregate revision, cycle budget, and async all come
from that one service, never rebuilt) and source-over compositing the
results via the `Backend`. It aggregates `bounds()`, `time_extent()`, and
`stability()` from the child's reachable layers, memoized on the child's
**aggregate revision**, and exposes the child layers as its `inputs()` so the
core's operator machinery folds them.

Scope for this task: the **visual** facet, **homogeneous** working-space
trees (the common case — "homogeneous trees pay nothing", doc 07:34-35),
two-level caching, cycle/Droste termination, and snapshot consistency. The
**audio** facet and **heterogeneous** working-space conversion are deferred
to named follow-ups (see Acceptance criteria) because their prerequisites
(`contract.audio_facet`; a `Backend` conversion operation) are not yet
landed.

## Why it needs to be done

`kinds.nested` is the sole remaining leaf of the M4 milestone
`m4_recursion` ("M4: Recursive composition"), which
`depends kinds.nested, compositor.operator_graph,
compositor.expose_visible_plan` (`tasks/99-milestones.tji:37`) — the other
two are `complete 100`. M4's promise: "Compositions embed compositions
through PullService with two-level caching and cycle termination. Doc 05
realized." (`:38`). Nested is that realization. It is also doc 05's closing
argument made executable: a kind built purely on public interfaces (the
layer contract downward, `PullService` inward) proves a third-party plugin
can do anything core can (doc 05:29-31, doc 13:87-89). Downstream, doc 12
designates `org.arbc.nested` as the reference proof of the audio facet's
recursion (`12-audio.md:216-218`) and doc 08 as the loader target for
external nested projects — both build on this kind.

## Inputs / context

**Governing design docs (normative, doc 16).**

- **doc 05 — Recursive composition** (`docs/design/05-recursive-composition.md`),
  the whole doc:
  - Semantics (`:8-37`): local space = child's identically; **bounds** =
    child canvas rect if declared, else recursive union of child layer
    bounds (unbounded if any layer unbounded), memoized, invalidated by
    child structural damage; **scale range** unbounded at the boundary
    (child layers keep their own limits); **stability** = static iff every
    reachable child layer is static, memoized the same way; **rendering** =
    run the compositor over the child with a synthetic viewport (device
    rect = target, camera = region-to-surface), same exactness/deadline/
    snapshot — "Rendering *is* recursion."
  - Sharing and instancing (`:39-52`): child layer tile caches keyed by the
    child's content identities, shared by all embeddings at matching scale
    rungs; each embedding's *composed*-result cache is its own.
  - Cycles (`:54-75`): a cycle is a legitimate Droste/infinite-zoom object;
    termination guaranteed by the sub-pixel cull (doc 04) for `<1×` cycles;
    a per-request recursion-depth budget backstops `≥1×` cycles (placeholder
    + diagnostic naming the cycle); snapshot correctness — every visit to a
    composition within one frame sees the same revisions.
  - Caching across the nesting boundary (`:77-91`): two cooperating layers;
    composed result keyed by the child's **aggregate revision** (a
    composition-level revision bumped by any reachable change); "static
    children make deep hierarchies cheap: a 10-level tree re-renders only
    the spine from the edited layer to the viewed root."
  - Budgets across recursion (`:93-101`): deadline and cache budget flow
    *through* nesting, priority-classed by *outermost* visibility — "the
    recursive case must cost what an equivalent flat scene would."
- **doc 13 — Effects as operators** (`docs/design/13-effects-as-operators.md`):
  nested is retroactively an operator whose single input is a composition
  (`:33-37`); the operator contract (`inputs()`/`map_input_damage()`/
  `identity()`, `:44-66`); pulls go through the core `PullService`, never
  `input->render()` (`:69-89`); metadata composes synchronously by querying
  inputs (`:91-92`).
- **doc 07 rule 4** (`docs/design/07-color-and-pixel-formats.md:32-35`):
  "Nested compositions may declare different working spaces; the nesting
  boundary is a conversion point like any other content (the child's
  composed output converts into the parent's working space). Homogeneous
  trees pay nothing." Rule 2 (`:19-24`): all compositing happens in the
  composition's working space.
- **doc 17 — Internal components** (`docs/design/17-internal-components.md`):
  `arbc::kind-*` is **L4** (`:59`), `kind-nested` depends only on
  `contract` (+ its transitive closure: base, pool, media, surface, model)
  and "uses only the `PullService` interface"; a component may depend only
  on strictly lower levels, no same-level edges (`:41-44`, CI-enforced by
  `scripts/check_levels.py`). Dual-build (`:108-111`). `arbc::surface` is
  **L2** and owns the `Backend` contract (`:51`); `arbc::compositor` is
  **L4** (`:56`), a sibling nested may not name.

**Existing seams the implementation extends (real paths + lines).**

- `src/contract/arbc/contract/content.hpp` — the `Content` base and its
  facets nested implements: `bounds()`/`stability()`/`time_extent()`
  (`:202-214`), `quantize_time()` (`:235`), `render()` (`:259-260`),
  `render_thread_safe()` (`:275`), and the operator-graph members
  `inputs()` (`:289`), `map_input_damage()` (`:301`), `identity()` (`:308`).
  `PullService::pull` (`:335-336`), `RenderRequest{region, scale, time,
  snapshot, target, exactness, deadline}` (`:76-84`), `RenderResult`
  (`:86-99`), `RenderCompletion` (`:119-151`), `Stability`/`Exactness`
  (`:25-35`), `ContentRef = Content*` (`:160-161`).
- `src/compositor/arbc/compositor/pull_service.hpp` — `PullServiceImpl`
  (`:89-136`); `PullConfig` fields nested's request travels through:
  `budget` (the `GraphBudget` threaded, never reset, `:69-71`),
  `diagnostics` (`GraphDiagnostics*` cycle-backstop sink, `:66-68`),
  `contribution` / `id_of` (aggregate-revision + cache identity, `:72-81`).
- `src/compositor/arbc/compositor/operator_graph.hpp` — `aggregate_revision`
  (`:105-107`), `GraphBudget { unsigned max_depth; }` (`k_max_recursion_depth
  = 64`), `GraphDiagnostics` — the machinery nested's inputs fold through.
- `src/compositor/arbc/compositor/compositor.hpp` — `struct Viewport`
  (`:14-26`) and the per-layer predicate shape (`render_layer`, `:44-56`)
  nested re-expresses (it may not link the compositor; see Decisions).
- `src/surface/arbc/surface/backend.hpp` — `Backend::composite(dst, src,
  src_to_dst, opacity)` (source-over, premultiplied, `:38-40`),
  `clear` (`:35`), `make_surface` (`:31-32`); the L2 seam nested composites
  through.
- `src/model/arbc/model/model.hpp` — `DocRoot::find_composition(ObjectId)`
  (`:50`), `for_each_layer_in(composition, fn)` (`:74`, bottom-to-top
  membership), `working_space()` (`:53-60`); `records.hpp` —
  `CompositionRecord { canvas_w, canvas_h, working_space, layer_count, … }`
  (`:104-111`), `LayerRecord` (transform + opacity + content id).
- `src/kind_solid/arbc/kind_solid/solid_content.hpp` +
  `src/kind_solid/CMakeLists.txt` — the reference-kind template
  (`static constexpr const char* kind_id`, `arbc_add_component(NAME kind_solid
  … DEPENDS contract)`) nested's component mirrors.
- `testing/arbc/testing/contract_tests.hpp` — `contract_tests(factory,
  options)` and the operator-graph family entry points (`:117-147`),
  `Options` (`:57-71`, incl. `operator_graph`, `snapshot_sensitive`).

## Constraints / requirements

1. **New component `arbc::kind-nested`** at `src/kind_nested/`, public header
   `arbc/kind_nested/nested_content.hpp`, CMake `arbc_add_component(NAME
   kind_nested … DEPENDS contract)` — the *only* declared dependency edge
   (doc 17:59). No `compositor`, no `backend-cpu`, no `runtime` type is
   named. The include-hygiene and dependency checks (`scripts/check_levels.py`,
   doc 17:41-44) must pass.
2. **Reuse `PullService`, never `input->render()`** (doc 13:69-71). Every
   child-layer render goes through the injected `PullService::pull`, carrying
   the outer request's `snapshot`, `deadline`, `exactness`, and `GraphBudget`
   **verbatim** — never reset, recomputed, or sub-budgeted per level
   (doc 05:93-101). Nested rebuilds none of cache lookup, worker dispatch,
   aggregate revision, cycle budget, identity, or async — those are the
   service's.
3. **Synthetic viewport** (doc 05:24). Per child layer, compose the request's
   region-to-surface mapping with the layer's transform; the device target is
   the request's `target`. Same-exactness/deadline/snapshot sub-requests.
4. **Metadata composed and memoized** (doc 05:8-37, doc 13:91-92).
   `bounds()` = child canvas rect if declared else recursive union of
   reachable child-layer bounds (unbounded if any is); `time_extent()` = union
   of child-layer time extents; `stability()` = `Static` iff every reachable
   child layer is `Static`. All three memoized keyed on the child's aggregate
   revision (recompute only when it changes); invalidated by child structural
   damage.
5. **`inputs()` exposes the child layers' contents** so the core's
   `aggregate_revision` fold, cycle walk, and damage routing operate over the
   graph (`operator-damage-routes-through-map-input-damage`,
   `aggregate-revision-folds-reachable-inputs` already pin the fold). Nested's
   `map_input_damage` maps each child layer's damage through that layer's
   embedding transform (covering / over-approximating, `content.hpp:293-301`).
6. **Cycle / Droste termination** (doc 05:54-75). Thread the request's
   `GraphBudget` through pulls; the sub-pixel cull terminates `<1×` cycles,
   the depth budget backstops `≥1×` cycles — a descent past `max_depth`
   renders the placeholder and reports exactly one diagnostic naming the
   content path (reuse the `PullService` backstop; do not add a second one).
7. **Snapshot consistency** (doc 05:71-75). The request's `snapshot`
   `StateHandle` rides every pull unchanged; nested reads child composition
   membership at the pinned model version, so a Droste scene is self-consistent
   within a frame.
8. **Homogeneous working space** (doc 07:34-35). When
   `child.working_space() == parent working_space`, composite directly (pay
   nothing). When they differ, degrade honestly: render the placeholder and
   report one `GraphDiagnostic` naming the boundary mismatch (no wrong pixels,
   no crash) — real conversion is the deferred follow-up.
9. **Attach seam driven by tests** (mirrors `kinds.raster`). Nested defines an
   attach/injection seam (`PullService&`, `Backend&`, a `Content*(ObjectId)`
   resolver over model ids, pinned `DocRoot` access) its own tests drive
   manually; production runtime auto-wiring is deferred.
10. **`render_thread_safe()`** — nested evaluates its per-layer descent
    synchronously on the calling frame thread (only leaf renders are
    dispatched to workers by the `PullService`), so it declares whatever the
    contract requires for a synchronous compositor over a thread-confined
    target; document the choice against `content.hpp:262-275`.
11. **Dual-build honesty** (doc 17:108-111): the kind also links into the
    CI-only shared library via the extern-C entry point (the `kinds.dual_build`
    proof), so it must stay codec-free and self-contained.

## Acceptance criteria

**Conformance run (doc 16 — content kinds run the contract suite).** A Catch2
TU under `src/kind_nested/t/` calls `arbc::contract_tests(factory)` with a
factory producing a fresh `NestedContent` wired (via the attach seam) to a
fixed small child composition — e.g. two `org.arbc.solid` layers over a stub
`PullService` + `CpuBackend`. With `operator_graph = true` it enforces:

- `03-layer-plugin-interface#render-scale-honest`,
  `#render-within-declared-bounds`, `#undamaged-regions-stable`,
  `#render-pure-over-pinned-state`, `#facet-consistency`,
  `#static-time-invariant` / `#render-time-honest` — the description-method
  and purity families over the aggregated metadata.
- The operator-graph families over nested's real inputs:
  `check_operator_damage_covers` (`05-recursive-composition#operator-damage-\
routes-through-map-input-damage`) and `check_leaf_no_operator_graph` is
  *not* applicable (nested is a non-leaf; assert non-empty `inputs()`
  instead). `snapshot_sensitive` is set true when the fixed child contains an
  editable layer.

**New claims-register entries (nested-specific, doc 05).** Add to
`tests/claims/registry.tsv` and pin each with an `enforces:`-tagged test:

- `05-recursive-composition#nested-renders-through-synthetic-viewport` —
  *rendering a nested content over a child composition produces pixels
  byte-identical to compositing that child's layers directly at top level
  under the equivalent viewport* (the "rendering is recursion" identity).
  Pinned by a **byte-exact golden**: a two-layer child rendered through
  `NestedContent` versus the same two layers as top-level compositor layers.
- `05-recursive-composition#static-subtree-served-from-cache` — *a static
  child's composed-result tiles survive a clock advance and an unrelated
  edit: only the spine from an edited layer to the viewed root re-renders*
  (doc 05:87-89). Pinned by a **behavioral-counter** test on dispatched
  renders (`requests_issued` / `operator_renders` deltas), never wall-clock:
  a clock advance over a static subtree issues zero renders; an edit deep in
  a shared child re-renders exactly the spine.
- `05-recursive-composition#nested-metadata-memoized-on-aggregate-revision` —
  *`bounds()`/`stability()`/`time_extent()` recompute only when the child's
  aggregate revision changes* — a **behavioral-counter** assertion on a
  recompute counter (zero recomputes across repeated queries at a stable
  aggregate revision).
- `05-recursive-composition#nested-boundary-budget-flows-through` — *a nested
  scene's pulls carry the outer `snapshot`/`deadline`/`GraphBudget` verbatim;
  a depth-`N` nesting costs what the equivalent flat scene costs* (re-asserts
  `13-effects-as-operators#pull-inherits-snapshot-and-deadline` through the
  nested path).

Re-asserted (a second `enforces:` test, not re-registered):
`05-recursive-composition#graph-walk-bounds-cycles` (a `≥1×` Droste cycle
terminates at the depth budget with one diagnostic + placeholder; a `<1×`
Droste bottoms out via the sub-pixel cull after finitely many turns) and
`05-recursive-composition#composed-result-invalidated-like-leaf` (nested's
composed output is invalidated as an ordinary leaf tile — no
composite-specific cache mechanism).

**Byte-exact goldens (deterministic rendering, doc 16 — goldens are the
default).** A golden suite under `src/kind_nested/t/` (following the
`kinds.raster` / `color.kernel_goldens` pattern): the synthetic-viewport
identity golden above; a two-level nesting golden (composition embedding a
composition) at native, `0.5×`, and `0.25×`; a Droste-at-`<0.5×` golden
proving finite termination and stable pixels.

**Snapshot consistency test.** Within one frame/snapshot, two visits to the
same shared child composition (a diamond embedding) render byte-identical
output (`snapshot-consistent-within-frame` behavior, folded into the
synthetic-viewport golden or a dedicated case).

**Concurrency (doc 16 — concurrency-touching tasks scope their coverage).** A
TSan/stress test rendering a nested scene through a real multi-worker
`WorkerPool`-backed `PullService` (child leaf renders dispatched
concurrently, nested's descent on the frame thread), asserting no data race
and deterministic output — reusing the `pull_service` async test harness.

**Dual-build honesty** (doc 17:108-111): the CI-only shared-library link of
`kind_nested` via the extern-C entry point loads and renders. **CI**: ≥90%
diff coverage on changed lines; the doc-17 dependency + include-hygiene
checks pass.

**Deferred follow-ups (closer registers each as a real WBS leaf):**

- `kinds.nested_audio` (~2d) — implement `org.arbc.nested`'s **audio facet**:
  aggregate the child composition's `AudioFacet` through the synthetic
  monitor, time-map + gain remix, sub-audible/depth-budget termination — the
  recursion reference proof for the audio facet (doc 12:191-197, 216-218).
  `depends kinds.nested, contract.audio_facet`. Milestone: `m6_audio`.
- `kinds.nested_working_space_conversion` (~2d) — wire the **heterogeneous**
  nesting-boundary conversion (doc 07 rule 4): add a `convert` operation to
  the `Backend` contract (surface L2 — doc 17:51 already scopes "format
  conversion interfaces" there; carries a doc 09 delta), implement it in
  `CpuBackend` over the landed `convert_kernel`, and invoke it when the
  child's `working_space()` differs from the parent's — replacing this task's
  placeholder-on-mismatch. Pin per-format byte-exact goldens.
  `depends kinds.nested`. Milestone: `m4_recursion` (heterogeneous nesting
  completes doc 05 × doc 07 rule 4).
- `kinds.nested_runtime_binding` (~1d) — when the runtime instantiates an
  `org.arbc.nested` content, wire its attach injection (`PullService`,
  `Backend`, the id→`Content*` resolver, the pinned `DocRoot`) onto the live
  services and tear down on release — closing the production wiring nested's
  tests drive manually. `depends kinds.nested, model.content_binding`.
  Milestone: the runtime/kinds integration milestone (as
  `kinds.raster_runtime_binding`).

## Decisions

- **Nested composites its child's layers itself, re-expressing the per-layer
  cull/compose loop; only the `PullService` machinery is reused.** Doc 05:24
  says "run the compositor over the child," but doc 17:44/56/59 forbids a
  `kind-nested → compositor` edge (both are L4; no same-level edges). So
  nested walks the pinned child composition's members
  (`DocRoot::for_each_layer_in`), pulls each layer's content through the
  injected `PullService` (inheriting cache/worker/snapshot/deadline/budget),
  and source-over composites via `Backend::composite` (surface L2, in
  `contract`'s closure). The *heavy* machinery is never duplicated (doc
  13:33-37, "generalizes it rather than duplicating it"); only the thin
  per-layer predicate loop is re-expressed. *Alternative rejected:* linking
  the compositor's `render_layer` — a same-level edge doc 17 bans. *Also
  rejected:* moving the composite loop into a shared lower-level component —
  premature; nested is its only caller today (doc 05's "compositor API
  inward" resolves to the `PullService` interface, doc 13:85-89).
- **`inputs()` returns the child composition's member-layer contents.** Doc
  13:34 frames nested as "an operator whose single input is a composition";
  concretely a composition is its member layers, and the core needs those
  edges for the aggregate-revision fold, cycle detection, and damage routing
  (`operator_graph.hpp:105`). *Alternative rejected:* synthesizing a single
  "composition-output" `Content` as the lone input — that Content would have
  to composite the layers somewhere, and building it is compositor-level work
  nested cannot own; exposing the real member edges is the levelization-clean
  realization.
- **Homogeneous working space now; heterogeneous conversion deferred.** The
  common case ("homogeneous trees pay nothing", doc 07:35) needs only the
  existing `Backend::composite`. The heterogeneous boundary needs a `Backend`
  *conversion* operation that does not yet exist (the `convert_kernel` is
  landed in `backend-cpu` but unexposed through the L2 contract), plus a doc
  09 delta and per-format goldens — a distinct chunk that would swell an
  already-large 4d keystone task. Deferring it to
  `kinds.nested_working_space_conversion` keeps `kinds.nested` focused on the
  doc-05 machinery and matches M4's stated scope (`:38`, no color mention),
  while the in-task placeholder+diagnostic on mismatch keeps behavior honest
  (no wrong pixels, no crash). *Alternative rejected:* adding `Backend::convert`
  here — real, but out of M4's scope and best carried with its own goldens by
  the follow-up.
- **Audio facet deferred.** `contract.audio_facet` is not landed; no
  `AudioFacet`/`AudioRequest`/`render_audio` type exists
  (`content.hpp:325-326` is a forward comment). Nested cannot implement a
  facet whose interface is absent. Deferred to `kinds.nested_audio` (doc 12
  still names nested the audio-facet recursion proof, `:216-218`).
  *Alternative rejected:* landing the `AudioFacet` interface here — that is
  `contract.audio_facet`'s job (doc 17:53, contract owns the facet), not a
  kind's.
- **Metadata memoized on the child's aggregate revision.** The aggregate
  revision bumps on any reachable change (doc 05:85), so it is the exact key
  for "invalidated by child structural damage" (doc 05:15-16); a stable
  aggregate revision means bounds/stability/extent are unchanged and the
  cached values are returned. *Alternative rejected:* recompute every query —
  correct but violates doc 05's "Memoized" invariant and the "static
  hierarchies are cheap" promise.
- **Reuse the `PullService` recursion-depth backstop; add no second one.** The
  budget + diagnostic already live in `PullServiceImpl` (`pull_service.hpp:66-71`,
  `05-recursive-composition#graph-walk-bounds-cycles`); nested threads the
  request's `GraphBudget` unchanged and lets the service backstop divergent
  `≥1×` cycles. *Alternative rejected:* a nested-local depth counter —
  duplicates the backstop and risks a second, inconsistent limit.

## Open questions

(none — all decided.) One non-blocking WBS-shape observation is surfaced to
the closer in the return summary rather than encoded here: the milestone
placement of `kinds.nested_working_space_conversion` (M4 vs. a later color
milestone) is the closer's call when wiring the deferred leaf.

## Status

**Done** — 2026-07-06.

- `src/kind_nested/arbc/kind_nested/nested_content.hpp` + `src/kind_nested/nested_content.cpp` — `NestedContent` (`org.arbc.nested`): synthetic-viewport render re-expressing the per-layer cull/compose loop via the injected `PullService` + `Backend`; composed+memoized `bounds`/`stability`/`time_extent` keyed on pinned aggregate revision; `inputs()`/`map_input_damage()`/`identity()`; attach seam; `recursive_mutex` on metadata memo for Droste self-query termination.
- `src/kind_nested/CMakeLists.txt` — `arbc_add_component(NAME kind_nested … DEPENDS contract)`, sole declared dependency edge (doc 17:59); levelization-clean.
- `src/kind_nested/t/nested_meta.t.cpp` — in-component conformance suite entry (`contract_tests` + `check_operator_damage_covers` + non-empty `inputs()` assertion).
- `tests/nested_conformance.t.cpp` — top-level conformance: `contract_tests` + operator-graph family, `snapshot_sensitive = true`.
- `tests/nested_goldens.t.cpp` — byte-exact golden suite: synthetic-viewport identity vs. direct composite; two-level nesting at 1×/0.5×/0.25×; Droste-at-<0.5× finite termination + stable pixels; snapshot-consistency (diamond embedding, same pixels per frame).
- `tests/nested_cache.t.cpp` — behavioral-counter claims: `static-subtree-served-from-cache` (zero re-renders over a static child on clock advance); `nested-metadata-memoized-on-aggregate-revision` (zero recomputes at stable aggregate revision); `nested-boundary-budget-flows-through` (outer snapshot/deadline/GraphBudget verbatim through pulls).
- `tests/nested_concurrency.t.cpp` — TSan/stress: real multi-worker `WorkerPool`-backed `PullService`, deterministic output, no data race.
- `tests/claims/registry.tsv` — 4 new claims: `nested-renders-through-synthetic-viewport`, `static-subtree-served-from-cache`, `nested-metadata-memoized-on-aggregate-revision`, `nested-boundary-budget-flows-through`.
- `src/CMakeLists.txt` + `tests/CMakeLists.txt` — wired `kind_nested` into the build and test targets.
- Design calls: homogeneous working-space is a **precondition assert** (GraphDiagnostics off-limits at L4→L4); nested codes against the abstract `PullService` contract; `recursive_mutex` on metadata memo makes Droste self-metadata-queries visit each node once. `Backend::convert` intentionally absent — deferred to `kinds.nested_working_space_conversion`.
- Deferred follow-ups registered as WBS tasks in this commit: `kinds.nested_audio`, `kinds.nested_working_space_conversion`, `kinds.nested_runtime_binding`.
