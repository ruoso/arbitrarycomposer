# compositor.operator_graph — Operator graph awareness

## TaskJuggler entry

Back-link: `tasks/35-compositor.tji:36-40` (`task operator_graph`, effort **3d**,
`allocate team`, no `complete 100` yet). The note reads, verbatim:

> Core-visible inputs(): aggregate revisions, damage routing via
> map_input_damage, cycle detection + recursion-depth budget, identity()
> short-circuit in planning. Docs 05/13.

The task carries no own `depends` line, so it inherits only the parent
`compositor` block's edges (`tasks/35-compositor.tji:7`):
`contract.async_render, cache.key_shapes, color.resampling`. It is a **hard
predecessor of `compositor.pull_service`** (`tasks/35-compositor.tji:48-53`,
`depends !tile_planning, !operator_graph`): the async pull machinery is built
on the core-visible graph this task lands. It is also the sibling every other
compositor leaf routed its operator-graph concerns to (see Inherited
dependencies).

## Effort estimate

**3d.** Larger than the 2d damage_planning / counters leaves because it lands
four distinct graph behaviors (aggregate-revision fold, `map_input_damage`
routing, cycle/depth budgeting, `identity()` short-circuit) plus a counter
field, each with its own claim and unit coverage; smaller than the 4d
tile_planning leaf because it introduces no new cache/ladder machinery — it
walks the already-landed `Content` operator members and threads their results
into the already-landed planner, damage, and counter seams. The bulk is the
pure `operator_graph.{hpp,cpp}` walk module and its exhaustive synthetic-graph
tests; the driver wiring is the established add-a-defaulted-parameter edit.

## Inherited dependencies

**Settled:**

- **The operator-graph contract members** (`contract`, L3) landed as
  null/identity defaults in `src/contract/arbc/contract/content.hpp:284-308`
  (commit `1c7184b`-era contract): `inputs()` → empty span
  (`content.hpp:289`), `map_input_damage(input, rect)` → identity
  (`content.hpp:301`, default body `src/contract/content.cpp:11`),
  `identity(request)` → `nullopt` (`content.hpp:308`, default body
  `content.cpp:15`). `ContentRef = Content*` (`content.hpp:161`) — input edges
  are **raw non-owning `Content*`**, not `ObjectId`s. Their isolated behavior
  is proven by `src/contract/t/operator_members.t.cpp` (`LeafContent`,
  `OperatorContent`, `RecordingPull`; enforces
  `03-layer-plugin-interface#leaf-content-has-no-operator-graph`). **This task
  is their first L4 consumer** — nothing in the compositor reads any of them
  today.
- **`compositor.tile_planning`** (commit `5d3f6c2`, DONE 2026-07-05) delivered
  the per-layer planner `plan_layer` and the synchronous driver
  `render_frame_interactive`
  (`src/compositor/arbc/compositor/tile_planning.hpp:145-150,218-223`; impl
  `src/compositor/tile_planning.cpp`). `plan_layer` already takes the layer's
  `const Content* content_ptr` (`tile_planning.hpp:150`), consulted today only
  for `quantize_time`; the driver resolves each layer's `Content*` at
  `tile_planning.cpp:240` (`Content* content = resolve(layer.content)`) — the
  natural seam to become graph-aware. Planning is **pure, allocation-free,
  lock-free** (`tile_planning.md:378-383`, Decision 1): it reads and pins via
  `lookup`, never inserts or renders; the driver fills misses. `tile_planning`
  explicitly deferred **"`inputs()`, aggregate revisions, damage routing, cycle
  detection, and the `identity()` short-circuit in planning"** to this task
  (`tile_planning.md:219-221`).
- **`compositor.damage_planning`** (commit `2bcacc6`, DONE 2026-07-06)
  delivered `map_damage_to_device`, `clock_advance_damage`, and
  `invalidate_damage` plus the `const DirtyRegion* dirty` frame gate
  (`src/compositor/arbc/compositor/damage_planning.hpp:64-92,47-49`). It keys
  damage flatly on `Damage.object` as the content id
  (`damage_planning.hpp:31-33`) and explicitly deferred **"mapping a *child's*
  damage up through a containing composite via `inputs()` / `map_input_damage`"**
  to this task (`damage_planning.md:219-222`, "Existing WBS leaf, no new task").
- **`compositor.counters`** (commit `9183dcd`, DONE 2026-07-06) delivered the
  caller-owned `CompositorCounters` POD
  (`src/compositor/arbc/compositor/counters.hpp:34-56`:
  `requests_issued`/`composites`/`follow_up_frames`) and its
  `counters_snapshot` view. It explicitly deferred the **operator-render
  counter and the claim "identity fades issue zero operator renders"** to this
  task (`counters.md:89-95,244-250`: "Its refinement extends
  `CompositorCounters` with an `operator_renders` field and registers that
  claim when the seam lands").
- **`compositor.expose_visible_plan`** (commit `b1679b6`, DONE 2026-07-06)
  established the seam-growth discipline this task follows: the driver signature
  grows by **one trailing defaulted parameter per sibling**
  (`RefinementQueue*`, `CompositorCounters*`, `const DirtyRegion*`, `Time`,
  `std::vector<LayerTilePlan>*` today — `tile_planning.hpp:218-223`), with the
  null/default path held **byte-for-byte** against the landed goldens.
- **`cache.invalidation`** delivered `invalidate_region(cache, content, region,
  geom)` and `invalidate_content(cache, content)`
  (`src/cache/arbc/cache/invalidation.hpp:48,62`), the drops routed operator
  output damage feeds. `TileKey.revision` is a bare opaque `std::uint64_t`
  (`src/cache/arbc/cache/key_shapes.hpp:64-76`, comment 24-31), so an aggregate
  revision projects into the same slot with no schema change.

**Pending:** none — every predecessor is landed. (`compositor.pull_service` is a
*successor*, not a dependency.)

## What this task is

The compositor's first pass over the **operator graph the core can see through
`Content::inputs()`** (doc 13:48-52). Today every visible layer is planned,
keyed, and damaged as a flat leaf; this task teaches the planner, the damage
router, and the counter surface to walk an operator's `inputs()` DAG and act on
what they find — without yet pulling or caching input outputs (that is
`pull_service`'s job, doc 13:69-89). Four deliverables, all in a new pure
compositor-internal module `src/compositor/arbc/compositor/operator_graph.{hpp,cpp}`
plus thin wiring into the landed seams:

1. **Aggregate revision fold.**
   `std::uint64_t aggregate_revision(const Content* root, const
   std::function<std::uint64_t(const Content*)>& contribution, GraphBudget)` —
   a depth-first fold over the reachable `inputs()` DAG that combines each
   reachable node's `contribution` into a single opaque `uint64`, so an
   operator's output key changes iff any reachable input changes (doc 05:82-91,
   doc 13:124-125). Order-independent (a commutative mix), each reachable node
   folded exactly once (cycle-safe via a `const Content*` visited set). The
   operator layer's output `TileKey.revision` is set from the fold instead of
   the flat `state.revision()`. The `contribution` callback is caller-supplied;
   the driver passes the document-global `state.revision()` for every node today
   (Decision 3).

2. **Damage routing via `map_input_damage`.** Given a `Damage{object, rect}` on
   an input, propagate it to every **visible operator** that reaches `object`
   through `inputs()`, mapping the rect forward through the composed chain of
   `map_input_damage` calls to the operator's output footprint (doc 13:54-57,
   104-107; doc 05:88-91). The compositor resolves `Damage.object → Content*`
   via the existing resolver, then does a pointer-reachability walk from each
   visible operator layer; on a hit it folds the rect up through
   `map_input_damage` at each edge (over-approximating, covering) and emits an
   operator-output `Damage{operatorObject, mappedRect}` that feeds the landed
   `map_damage_to_device` / `invalidate_damage`. Cycle-safe and depth-budgeted.

3. **Cycle detection + recursion-depth budget.** A `GraphBudget { unsigned
   max_depth; }` (default `k_max_recursion_depth`) carried through every graph
   traversal as the compositor-internal backstop doc 05:66-70 mandates. The
   pure structural walks (fold, damage route) terminate on any `inputs()` cycle
   by the visited set (each node once). The planning descent (identity-chain
   resolution, and future nested-composition descent) that exceeds `max_depth`
   makes the layer render the **placeholder** and reports a **diagnostic naming
   the offending content path** through a caller-owned optional
   `GraphDiagnostics*` sink (doc 05:66-70, doc 13:134-138). Convergent cycles
   (composed scale < 1) bottom out earlier by the sub-pixel cull
   (`anchored_viewports`, doc 04) before reaching the budget — this task does
   not re-implement the cull; it guarantees the budget is a sound backstop.

4. **`identity()` short-circuit in planning.** When a visible operator layer
   reports `identity(request) == N`, the planner resolves the identity chain to
   its terminal non-pass-through content (depth-budgeted) and issues **no
   operator render and creates no operator-output cache entry** for the
   short-circuited operators (doc 13:59-65,128). `CompositorCounters` gains an
   `operator_renders` field (`note_operator_render`) bumped once per `render`
   the driver issues on an operator-typed content (`inputs()` non-empty); an
   identity operator leaves it at zero — the behavioral proof of "identity fades
   issue zero operator renders."

**Not this task:**

- **Pulling and caching input outputs / serving input N's cached tiles as the
  operator's output.** `PullService::pull` takes a `ContentRef`
  (`content.hpp:335`) and owns the request/cache/snapshot/budget machinery that
  actually renders and caches inputs → `compositor.pull_service`
  (`tasks/35-compositor.tji:48-53`, doc 13:69-89). This task lands the *planning
  decision* (identity detected, operator render suppressed, aggregate revision
  computed, damage routed); `pull_service` completes the serve-the-input-tiles
  execution.
- **Real operator kinds (fade / crossfade / nested).** No operator kind exists
  on disk (only `kind_solid`, `kind_raster`). `org.arbc.fade` / `crossfade` are
  the `operators` stream (`tasks/50-operators.tji`); the nested-composition kind
  is the `kinds` stream (`tasks/55-kinds.tji`, doc 17:59). This task is tested
  against **synthetic operator `Content` doubles** (the `operator_members.t.cpp`
  pattern), not real kinds; the contract conformance suite runs against the real
  kinds when they land.
- **Per-content revision *granularity*** (the doc 05:84 optimization that lets an
  unchanged sibling's cached tiles survive an edit elsewhere). The model exposes
  only a document-global `DocRoot::revision()`
  (`src/model/arbc/model/model.hpp:44`); the fold is conservative under it
  (correct, never stale) and generalizes with no compositor change — see
  Decision 3 and Open questions.
- **Anchor-walk cull / convergent-cycle termination by sub-pixel cull** →
  `compositor.anchored_viewports` (its walk is depth-agnostic,
  `anchored_viewports.md:355-363`); this task only guarantees the budget
  backstop.

## Why it needs to be done

Doc 13's whole thesis is that effects are operators — "content that consumes
content" — and that the machinery doc 05 built for nested composition is the
*general* operator machinery, made core-visible through `inputs()` (doc
13:33-37,179-181). Until the compositor actually reads `inputs()`, an operator
is invisible to the core: its output cannot key on its inputs' revisions (stale
renders across an input edit), an edit to a shared input cannot invalidate the
operators that show it (doc 05:88-91's "edit deep inside a shared component
invalidates every place it appears"), a feedback cycle would loop forever, and a
fully-open fade would needlessly re-render its input (doc 13:63-64 "cost nothing
outside the fade window"). This task closes all four gaps at the planning /
invalidation layer. It is also the gate for `compositor.pull_service`
(`35-compositor.tji:51`): the async pull machinery renders inputs *through* the
graph this task makes visible — aggregate revision keys the operator's cache
entry, the depth budget bounds recursive pulls, and `identity()` lets a pull
short-circuit — so pull_service cannot land until the core-visible graph exists.

## Inputs / context

**Design docs (normative, doc 16):**

- **doc 05 — recursive composition.** Aggregate revision = "a composition-level
  revision bumped by any reachable change" (`05-recursive-composition.md:82-86`);
  it is what damage propagation uses — "child damage maps through each
  embedding's transform into parent-space damage … invalidates every place it
  appears, at every viewport, at the right screen rectangles" (`05:88-91`).
  Cycles are representable and meaningful (`05:55-59`); "termination is
  guaranteed by the sub-pixel cull" for convergent cycles (`05:61-65`); "the
  compositor carries a recursion-depth budget per request as a backstop;
  exceeding it renders the placeholder and reports a diagnostic naming the
  cycle" (`05:66-70`); within one snapshot every visit to a composition sees the
  same revisions (`05:71-74`); budgets flow *through* recursion, never reset by
  it (`05:96-100`).
- **doc 13 — effects as operators.** The operator contract:
  `inputs()` "visible to the core … for aggregate revisions, snapshot
  consistency, cycle detection, and damage routing … Leaf content returns empty"
  (`13-effects-as-operators.md:48-52`); `map_input_damage` "Map damage on an
  input into damage on this content's output. Default: identity … The core calls
  this to propagate invalidation" (`13:54-57`); `identity()` "the compositor
  serves the input's cached tiles directly — no render, no copy, no new cache
  entry" (`13:59-65`). Caching: an operator's output is "keyed by its id and its
  *aggregate* revision … now driven by the core-visible `inputs()` graph",
  "`identity()` short-circuits both levels" (`13:124-128`). Cycles "get doc 05's
  rules verbatim … divergent ones hit the recursion-depth budget and the cycle
  diagnostic" (`13:134-138`). New machinery enumerated (`13:179-181`). The pull
  seam is "the one genuinely new core API" (`13:69-89`) — **not this task**.
- **doc 17 — components.** The compositor row (line 56) scopes this task
  literally: `arbc::compositor`, Level 4, "… damage routing over `inputs()`,
  aggregate revisions, cycle handling, `PullService` implementation"; allowed
  deps **`contract`, `cache`** only. The `PullService` *interface* is contract's
  (L3, line 53); the *implementation* is compositor's (line 56) — but that
  implementation is `pull_service`, not this task.

**Source seams (real paths + lines):**

- `src/contract/arbc/contract/content.hpp:284-308` — the three operator members
  this task consumes; `:76-84` — `RenderRequest` (region, scale, time, snapshot,
  target, exactness, deadline — **no depth field**, Decision 2); `:161`
  `ContentRef = Content*`; `:327-340` `PullService` interface (successor's).
- `src/compositor/arbc/compositor/tile_planning.hpp:113-122` `LayerTilePlan`,
  `:145-150` `plan_layer`, `:218-223` `render_frame_interactive`; impl
  `src/compositor/tile_planning.cpp:240` (resolve seam), `:147` (`TileKey`
  build), `:315`/`:354` (`note_request_issued`/`note_composite` bump seams).
- `src/compositor/arbc/compositor/damage_planning.hpp:64-92` — the three damage
  functions operator-output damage feeds; `:31-33` — the flat-keying comment
  this task generalizes.
- `src/compositor/arbc/compositor/counters.hpp:34-56` `CompositorCounters`,
  `:63-82` `CompositorStats` / `counters_snapshot` — the `operator_renders`
  field lands here.
- `src/cache/arbc/cache/invalidation.hpp:48,62` `invalidate_region` /
  `invalidate_content`; `src/cache/arbc/cache/key_shapes.hpp:64-76` `TileKey`
  (opaque `uint64` revision slot).
- `src/model/arbc/model/model.hpp:44` `DocRoot::revision()` (document-global);
  `src/model/arbc/model/records.hpp:60-61` `LayerRecord{ObjectId content; …}`;
  `src/model/arbc/model/damage.hpp:19-25` `Damage{object, rect, range}`.
- `src/contract/t/operator_members.t.cpp` — the synthetic-double test pattern
  (`LeafContent`, `OperatorContent`) this task's unit tests mirror.

**Test / registry conventions:** claim id is `<doc-file-stem>#<kebab-slug>`
(`tests/claims/registry.tsv`, TAB-separated 2-column), enforced bidirectionally
by `scripts/check_claims.py` — each registered claim needs a live test tagged
`// enforces: <claim-id>`. Compositor unit tests live in
`src/compositor/t/<name>.t.cpp` (Catch2, `arbc_component_test`).

## Constraints / requirements

- **Levelization (doc 17:56).** `arbc::compositor` (L4) may reach only
  `contract` and `cache` (+ transitive). `model::Damage` / `DocRoot` /
  `for_each_layer` are reached through the same transitive `model` visibility
  `render_frame_interactive` already uses (`damage_planning.hpp:34-37`). **No
  new DEPENDS edge; no `backend-cpu` edge; no same-level edge.** The new
  `operator_graph.{hpp,cpp}` header must compile standalone; the CI
  component-graph check must stay green.
- **Pure per-frame library; caller-owned state.** The graph module is pure
  functions over `const Content*` — no persistent state, no allocation on the
  leaf path, no lock (doc 02:123-125,135-137, `compositor.hpp:26`). `GraphBudget`
  and `GraphDiagnostics` are caller-owned, threaded by value / optional pointer
  exactly as `DirtyRegion` / `CompositorCounters` are.
- **Zero behavior change on the leaf path (byte-exact).** A `Content` with empty
  `inputs()` must plan, key, damage, and composite **byte-for-byte** as today —
  the fold degenerates to `contribution(root)`, no identity check fires, no
  routing occurs. All landed tile_planning / refinement / anchored_viewports /
  damage goldens must stay green unchanged. New driver parameters are trailing
  and defaulted; the null path is the current path.
- **Covering / soundness.** Damage routing must **over-approximate**, never
  under-report (doc 13 covering requirement, `content.hpp:294-300`): the routed
  operator-output rect must contain every output pixel the input rect can change.
  Aggregate revision must change whenever a reachable input's contribution
  changes (no stale operator tile).
- **Snapshot self-consistency.** Every graph walk runs under one snapshot
  (`StateHandle`); each visit to the same node within a frame sees the same
  revision contribution (doc 05:71-74). The walks take no wall clock and issue no
  render.
- **Single-threaded; no TSan obligation.** The driver path is single-threaded
  (doc 02:135-137); counts are plain `uint64` (no atomics). Threading is
  `pull_service`'s when it wraps this seam. No TSan/stress coverage is in scope.
- **CI diff coverage ≥90%** on changed lines (doc 16:112-118) — tests ship with
  the task.

## Acceptance criteria

**Claims — new (registered in `tests/claims/registry.tsv`, each with an
`enforces:`-tagged test in `src/compositor/t/operator_graph.t.cpp`):**

- `13-effects-as-operators#identity-plan-issues-no-operator-render` — "A visible
  operator layer whose `identity(request)` returns an input index is planned with
  zero operator renders and creates no operator-output cache entry; the
  `operator_renders` counter delta is 0, while a non-identity operator layer that
  is inline-rendered records exactly one." (The counters.md/`35-compositor.tji:73`
  pre-committed "identity fades issue zero operator renders".)
- `05-recursive-composition#aggregate-revision-folds-reachable-inputs` — "An
  operator's aggregate revision changes iff some reachable input's revision
  contribution changes; the fold is order-independent (permuting `inputs()`
  yields the same value) and visits each reachable node exactly once, so a shared
  diamond input is folded once and a cyclic `inputs()` graph still terminates."
- `05-recursive-composition#operator-damage-routes-through-map-input-damage` —
  "Damage on an input propagates to every visible operator that reaches it
  through `inputs()`, the output rect being the composed `map_input_damage`
  mapping (over-approximating / covering); an operator that does not reach the
  damaged input receives none."
- `05-recursive-composition#graph-walk-bounds-cycles` — "A structural walk
  (aggregate-revision fold, damage route) over a cyclic `inputs()` graph
  terminates with each node visited once; a planning descent (identity chain)
  that exceeds the per-request recursion-depth budget selects the placeholder and
  reports one diagnostic naming the offending content, without unbounded
  recursion."

**Claims — re-asserted (add a second `enforces:` test; do *not* re-register —
these are `03`/`05` contract/recursion claims this task consumes):**

- `03-layer-plugin-interface#leaf-content-has-no-operator-graph`
  (`registry.tsv:56`) — a leaf layer walked by the graph-aware planner behaves
  byte-identically to the pre-task leaf path.
- `03-layer-plugin-interface#operator-damage-covers` (`registry.tsv:84`) — the
  routing relies on and exercises `map_input_damage`'s covering property.
- `03-layer-plugin-interface#operator-identity-faithful` (`registry.tsv:85`) —
  the short-circuit relies on identity fidelity (output == input N).
- `05-recursive-composition#composed-result-invalidated-like-leaf`
  (`registry.tsv:67`) — an operator-output tile keyed by the aggregate revision
  invalidates via `invalidate_content` / `invalidate_region` identically to a
  leaf.

**Unit / behavioral test** — `src/compositor/t/operator_graph.t.cpp` (Catch2,
synthetic operator `Content` doubles à la `operator_members.t.cpp`): fold
correctness (single input, diamond, chain, order-permutation, cycle-terminates);
damage routing (single edge, multi-hop chain composing `map_input_damage`,
diamond union, unreachable operator spared, covering over-approximation); budget
(depth-N chain within budget succeeds, depth-(N+1) or a cycle hits the budget →
placeholder + one diagnostic); identity (`identity()==0` → `operator_renders==0`
and no operator cache insert; `nullopt` → exactly one operator render; identity
chain resolves to the terminal input, budget-guarded); and the **leaf-neutrality**
case (empty `inputs()` → all landed behavior unchanged).

**No new golden.** This task adds no new deterministic *pixel* output — it plans,
keys, and invalidates; the leaf-path pixels are guarded byte-exact by the landed
tile_planning / refinement / anchored_viewports goldens (which must stay green
unchanged). Operator-output pixels arrive with the real fade/crossfade kinds
(operators stream) and get goldens there.

**No new WBS leaf.** Every deferral routes to an **existing** leaf:
end-to-end operator rendering / serving input tiles → `compositor.pull_service`
(`35-compositor.tji:48-53`) and the real kinds → `operators` /`kinds` streams;
convergent-cycle cull → `compositor.anchored_viewports`. Per-content revision
granularity (doc 05:84) is a **model-stream design question**, surfaced to the
parking lot, not a compositor task (Open questions).

**Component wiring & CI dependency check.** `arbc::compositor` `DEPENDS` stays
`contract cache`; `operator_graph.hpp` compiles standalone; the doc-17
component-graph check is green; no `backend-cpu` or same-level edge is added.

**Gate green.** Build + tiers 1–5 in Debug + ASan/UBSan pass; `check_claims.py`
is bidirectionally satisfied. **No TSan obligation** (single-threaded pure
library, doc 02:135-137).

## Decisions

1. **A dedicated pure `operator_graph.{hpp,cpp}` module, walked from the driver's
   resolve seam.** The four behaviors are pure functions over `const Content*`
   (fold, route, budget, identity-resolve), consumed by
   `render_frame_interactive` / `plan_layer` at the existing
   `resolve(layer.content)` point (`tile_planning.cpp:240`) and by the damage
   functions. *Rationale:* mirrors the pure-library posture of every compositor
   sibling (tile_planning Decision 1, damage_planning) and keeps the graph logic
   unit-testable against synthetic doubles with no driver, no cache, no surface.
   *Rejected:* folding the walk inline into `plan_layer` — it would entangle
   graph traversal with tile probing, obstruct isolated testing, and bloat the
   already-large driver.

2. **The recursion-depth budget is compositor-internal walk state, not a
   `RenderRequest` field.** `GraphBudget` is threaded through the graph functions;
   `RenderRequest` (`content.hpp:76-84`) is unchanged. *Rationale:* doc 05:66-70
   says "*the compositor* carries a recursion-depth budget per request" — the
   budget is a core policy the operator never sees or self-limits against;
   putting it in the L3 `RenderRequest` descriptor would be a contract change for
   state that belongs to the L4 walker. *Rejected:* adding `unsigned depth` to
   `RenderRequest` — an unnecessary contract delta that leaks compositor policy
   into every content's render signature.

3. **The aggregate-revision fold takes a caller-supplied per-node `contribution`
   callback; the driver supplies the document-global `state.revision()` today.**
   *Rationale:* the fold *mechanism* (walk `inputs()`, combine contributions,
   visit-once, cycle-safe) is fully in scope and testable now; the *selectivity*
   that lets a static subtree keep its old key depends on per-content revision
   numbers the model does not yet expose (`DocRoot::revision()` is global,
   `model.hpp:44`). Feeding the global revision as every node's contribution is
   **correct and never stale** (any change bumps every operator's key —
   conservative, exactly today's flat behavior for the operator's own key) and
   the callback lets per-node contributions drop in with **no compositor change**
   when the model exposes them. *Rejected:* hard-coding `state.revision()` inside
   the fold — it would bake in the conservative behavior and force a compositor
   edit later; the callback costs nothing and is the honest seam. *Rejected:*
   blocking this task on model per-content revisions — out of the compositor's
   levelization and not required for any of the four behaviors to be correct.

4. **Damage routing walks forward from the bounded set of visible operator
   layers, resolving `Damage.object → Content*` and testing pointer
   reachability** — no global `Content*→ObjectId` reverse map. *Rationale:*
   `inputs()` yields raw `Content*` (`content.hpp:161`) while `Damage` carries an
   `ObjectId`; the resolver already maps `ObjectId→Content*` forward, so
   resolving the damaged object once and walking each *visible* operator's
   `inputs()` subtree by pointer matches the two representations without a new
   reverse index, and the visible-operator set is naturally bounded by
   `for_each_layer`. *Rejected:* building/maintaining a document-wide
   `Content*→ObjectId` map — new persistent state in a pure library, and
   unnecessary when only visible operators can produce on-screen damage.

5. **Identity short-circuit lands the planning *decision* (suppress operator
   render, zero `operator_renders`, no operator-output cache entry); serving input
   N's cached tiles is `pull_service`'s.** *Rationale:* actually serving an
   input's tiles requires caching inputs by `Content*` identity, which is exactly
   `PullService::pull`'s machinery (`content.hpp:335`, doc 13:69-89) and the
   successor task's charter; the counter + no-cache-entry proof is the observable
   contract this task can and must deliver now (counters.md pre-committed it).
   *Rejected:* caching the input's pixels under the operator's `TileKey` here — it
   would create the very operator cache entry doc 13:63 forbids and duplicate
   pull_service's caching. *Rejected:* deferring the whole identity behavior to
   pull_service — the counters refinement already scoped the counter and claim to
   *this* task; the planning-time suppression is independently testable with
   synthetic doubles.

6. **Cycle diagnostics via a caller-owned optional `GraphDiagnostics*` sink;
   budget-exceeded selects the placeholder.** *Rationale:* doc 05:66-70 wants a
   "diagnostic naming the cycle" plus placeholder render; a caller-owned optional
   sink matches the `CompositorCounters*` / `DirtyRegion*` seam discipline and
   keeps the pure library stateless, while defaulting null keeps the path
   byte-neutral. *Rejected:* throwing / aborting on cycle — the design says the
   frame degrades gracefully (placeholder), not crashes; *rejected:* logging to a
   global — breaks the caller-owned, testable-in-isolation posture.

- **No design-doc delta.** `inputs()`, `map_input_damage`, `identity()`, and
  `PullService` are already in the contract (`content.hpp:284-340`); aggregate
  revision (doc 05:82-91), damage routing (doc 13:54-57), the recursion-depth
  budget + cycle diagnostic (doc 05:66-70, doc 13:134-138), and the identity
  short-circuit (doc 13:59-65,128) are all settled doc text. This task
  concretizes them into C++ without altering designed behavior.

## Open questions

(none — all decided.) One item is routed to the parking lot rather than resolved
here: **per-content revision granularity** (doc 05:84's "static children make
deep hierarchies cheap"). Realizing the cache-survival optimization needs the
model to expose and bump per-object revisions rather than a single document-wide
counter — a model-stream design judgment about where per-object revisions live
and how coalescing/undo affect them. This task's fold is correct and conservative
without it (Decision 3) and generalizes with no compositor change; the closer
records the model question in `tasks/parking-lot.md`.

## Status

**Done** — 2026-07-06.

- New module `src/compositor/arbc/compositor/operator_graph.hpp` + `src/compositor/operator_graph.cpp`: pure graph-walk functions (`aggregate_revision`, `route_operator_damage`, identity-chain resolution) with `GraphBudget` and `GraphDiagnostics` as caller-owned sinks.
- New test `src/compositor/t/operator_graph.t.cpp`: 6 behavioral cases covering fold correctness, damage routing, cycle termination, identity short-circuit, and leaf-neutrality.
- `src/compositor/arbc/compositor/counters.hpp`: added `operator_renders` field and `note_operator_render()` to `CompositorCounters`; extended `CompositorStats` snapshot.
- `src/compositor/arbc/compositor/tile_planning.hpp`: forward-declared `GraphDiagnostics`; added trailing `GraphDiagnostics*` parameter to `plan_layer`/`render_frame_interactive`.
- `src/compositor/tile_planning.cpp`: aggregate-revision keying via fold, identity short-circuit gate, `operator_renders` bump on non-identity operator render.
- `src/compositor/CMakeLists.txt`: wired `operator_graph.cpp` into the compositor component.
- `tests/claims/registry.tsv`: registered 4 new claims (`05…#aggregate-revision-folds-reachable-inputs`, `05…#operator-damage-routes-through-map-input-damage`, `05…#graph-walk-bounds-cycles`, `13-effects-as-operators#identity-plan-issues-no-operator-render`).
- Per-content revision granularity (doc 05:84 optimization) routed to `tasks/parking-lot.md` as a model-stream design question; no new WBS leaf.
