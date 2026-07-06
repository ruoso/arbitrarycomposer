# compositor.pull_service — PullService implementation

## TaskJuggler entry

Back-link: `tasks/35-compositor.tji:49-54` (`task pull_service`, effort **3d**,
`allocate team`, no `complete 100` yet). The note reads, verbatim:

> The request/cache/snapshot/budget machinery behind a public seam: pulls hit
> the cache first, schedule on workers, inherit deadline + snapshot. Doc 13.

It carries its own `depends !tile_planning, !operator_graph`
(`tasks/35-compositor.tji:52`) and, through the parent `task compositor`,
inherits `depends contract.async_render, cache.key_shapes, color.resampling`
(`tasks/35-compositor.tji:7`). The two `!`-predecessors are the reason this task
is last-but-one in the compositor stream: it wraps the flat per-layer fill path
`tile_planning` built and the core-visible operator graph `operator_graph`
landed into the one reusable pull engine. It is in turn a **hard predecessor of
`kinds.nested`** (`tasks/55-kinds.tji:39`, `depends compositor.pull_service,
color.working_space`) and of the whole **`operators` stream**
(`tasks/50-operators.tji:7`, `depends compositor.pull_service,
contract.operator_members`) — nested composition and the reference fade /
crossfade kinds are all built *on* this seam (doc 13:201-205, "never built
twice").

## Effort estimate

**3d.** On par with the 3d `operator_graph` leaf and above the 2d
damage/counters leaves, because it lands one concrete L4 service
(`PullServiceImpl`) with three behaviors — cache-first serve, worker dispatch,
snapshot+deadline+budget inheritance — each with its own claim and unit
coverage, *plus* a genuine concurrency surface (off-thread completion → frame-
thread drain) that carries a TSan obligation the pure-library siblings did not.
Smaller than the 4d `tile_planning` leaf: it introduces no new cache, ladder, or
tile-geometry machinery — it **moves** the doc-05 synthetic-viewport internals
already living inline in `render_frame_interactive`'s miss-fill
(`src/compositor/tile_planning.cpp:299-403`) behind the `PullService` seam
(doc 13:175-177, "Mostly *moves* the doc-05 synthetic-viewport internals behind
a public seam"), re-uses `plan_layer`'s probe order and `operator_graph`'s
`aggregate_revision` / `GraphBudget` verbatim, and threads the result through the
runtime dispatch/attach points already **reserved for it by name**
(`src/runtime/arbc/runtime/worker_pool.hpp:41`,
`src/runtime/arbc/runtime/interactive.hpp:116`). No new component, no new
levelization edge, no design-doc delta.

## Inherited dependencies

**Settled:**

- **The `PullService` interface** (`contract`, L3) already exists as pure
  virtuals: `src/contract/arbc/contract/content.hpp:314-340`. The sole visual
  method is
  `virtual void pull(ContentRef input, const RenderRequest& request,
  std::shared_ptr<RenderCompletion> done) = 0;` (`content.hpp:335-336`); its
  doc comment (`content.hpp:314-326`) states this task's contract literally —
  "cache lookup first, worker scheduling, the request's snapshot token
  respected, its deadline and budget inherited" — and scopes it: "This is the L3
  interface only … The concrete implementation and the attach-time injection
  that hands a service to content are the L4 concern (`compositor.pull_service`,
  doc 17:56)." **This task is that concrete implementation.** `ContentRef =
  Content*` (`content.hpp:160-161`) — inputs are raw non-owning `Content*`.
- **`compositor.tile_planning`** (commit `5d3f6c2`, DONE 2026-07-05) delivered
  the per-layer probe `plan_layer` (`src/compositor/arbc/compositor/
  tile_planning.hpp:153-158`, impl `tile_planning.cpp:118+`) and the synchronous
  driver `render_frame_interactive` (`tile_planning.hpp:241-247`, impl
  `tile_planning.cpp:210-416`). The **miss-fill this task wraps** is
  `tile_planning.cpp:299-403`: build a tile surface (`:307-311`), materialize a
  `BestEffort` `RenderRequest{tile.local_rect, rung_px, composition_time,
  StateHandle{}, tile_surface, Exactness::BestEffort, deadline}` (`:320-322`),
  call `content->render(request, done)` (`:349`), insert an inline result under
  `PriorityClass::Visible` (`:356-358`), or push an async (`nullopt`) result to
  the `RefinementQueue` (`:362-373`). Note `tile_planning.cpp:321` passes an
  **inert `StateHandle{}`** — wiring the real snapshot through is explicitly this
  task's "inherit … snapshot" (`35-compositor.tji:53`). `plan_layer` is pure,
  allocation-free, lock-free (reads/pins via `lookup`, never inserts or renders,
  `tile_planning.md` Decision 1); this task keeps that split — plan probes, the
  service fills.
- **`compositor.operator_graph`** (commit `f762773`, DONE 2026-07-06) delivered
  `src/compositor/arbc/compositor/operator_graph.{hpp,cpp}`: `aggregate_revision`
  (`operator_graph.hpp:105-107`) that keys an operator's output tile,
  `is_operator` (`:83-85`), `resolve_identity` (`:159-160`, impl
  `operator_graph.cpp:117-155`) returning `IdentityResolution` (`:136-149`),
  `route_operator_damage` (`:131-133`), and the `GraphBudget { unsigned
  max_depth; }` (`:58-60`, default `k_max_recursion_depth = 64`, `:56`) +
  `GraphDiagnostics` (`:76-78`) backstop. It landed the *planning decision*
  (identity detected, operator render suppressed, aggregate revision computed,
  damage routed) and **explicitly deferred the serve/execution to this task**:
  "`PullService::pull` … owns the request/cache/snapshot/budget machinery that
  actually renders and caches inputs → `compositor.pull_service`"
  (`operator_graph.md:157-164`, Decision 5). The `operator_renders` counter it
  added (`counters.hpp:54`, `note_operator_render`) is the surface this task
  bumps once per dispatched operator render.
- **`compositor.expose_visible_plan` / `compositor.counters`** established the
  seam-growth discipline and the caller-owned `CompositorCounters` POD
  (`src/compositor/arbc/compositor/counters.hpp:34-66`:
  `requests_issued`/`composites`/`follow_up_frames`/`operator_renders`, snapshot
  via `counters_snapshot`, `:85-94`). `render_frame_interactive` grows by **one
  trailing defaulted parameter per sibling**, the null/default path held
  **byte-for-byte** against landed goldens — the template this task follows.
- **`cache.key_shapes` / `cache.invalidation`** (inherited) delivered the tile
  cache: `using TileCache = KeyedStore<TileKey, TileValue>`
  (`src/cache/arbc/cache/key_shapes.hpp:129`), `TileKey{content, revision, rung,
  coord, achieved_time}` (`:64-76`), `KeyedStore::lookup → optional<CacheHold>`
  (`src/cache/arbc/cache/keyed_store.hpp:169`, impl `:287-299`, a miss does
  `++d_misses; return nullopt`), `insert(key, value, bytes, klass)` (`:163`),
  `CacheHold` RAII pin (`:78-126`), `PriorityClass` (`:26-32`), counters
  `hits()/misses()/evictions()` (`:191-195`).
- **`contract.async_render`** (inherited) delivered the settlement plumbing:
  `Content::render(request, done) → optional<RenderResult>` (inline via return,
  async via `nullopt` + later settle, `content.hpp:259-260`),
  `render_thread_safe()` (`:275`), the one-shot thread-safe `RenderCompletion`
  (`content.hpp:119-151`, impl `src/contract/render_completion.cpp`),
  `RenderRequest{region, scale, time, snapshot, target, exactness, deadline}`
  (`:76-84`), `Deadline{at}` value-only (`:56-63`, no `now()`/`expired()`), and
  the `Exactness` / `Stability` enums (`:25-35`).

**Pending:** none — every predecessor is landed. (`kinds.nested` and the
`operators` stream are *successors*, not dependencies.)

## What this task is

The concrete L4 `PullService` — the one request/cache/snapshot/budget engine
behind the public seam (doc 13:69-89, "the one genuinely new core API"). A new
service `class PullServiceImpl final : public arbc::PullService` in
`src/compositor/arbc/compositor/pull_service.{hpp,cpp}` implements
`pull(input, request, done)` as: **probe the cache first, serve a hit
synchronously, dispatch a miss onto the worker path carrying the request's
snapshot and deadline unchanged, and settle `done` from the result** — exactly
the machinery `render_frame_interactive`'s miss-fill does inline today, lifted
into a service any content (an operator, a nested composition, or the frame
driver itself) can call. Three deliverables plus wiring:

1. **Cache-first serve.** On `pull`, derive the input's tile key(s) from
   `request` (content id via `input`, revision via `aggregate_revision(input, …,
   budget)` so an operator input keys on its whole reachable graph — doc 13:124-126,
   `operator_graph.hpp:105-107`; rung from `request.scale`; coords from
   `request.region`) and probe `TileCache::lookup` in `plan_layer`'s established
   order (fresh → stale → coarser). A resident, exact hit completes `done`
   synchronously from the cached `TileValue` and **dispatches no render**
   (`content.hpp:314-317`, "cache lookup first"). This is the operator analog of
   no-damage-no-work: a warm pull issues zero work.

2. **Worker dispatch on miss.** A miss builds the render descriptor and hands it
   to an injected **`RenderDispatch`** functor —
   `std::function<void(Content*, const RenderRequest&, std::shared_ptr<RenderCompletion>)>`
   — which runtime binds to `WorkerPool::submit(RenderTask{content, request,
   done})` (`worker_pool.hpp:41,50-54,96`) at wiring, and which defaults to a
   compositor-provided **`DirectDispatch`** (call `content->render` inline,
   settle the inline `RenderResult` into `done`) so the single-threaded degenerate
   path — today's behavior — is preserved byte-for-byte. On completion the
   service inserts the tile under the **input's own identity** and aggregate
   revision at `PriorityClass::Visible` (doc 13:124-126, two-level caching) and
   settles `done`; an async (`nullopt`) render composes through the same
   `RenderCompletion`, occupying no worker while pending and routing to the
   `RefinementQueue` so a follow-up frame re-plans it as a fresh hit and emits
   damage (doc 13:128-130). `note_request_issued` fires once per dispatched
   render, `note_operator_render` once per dispatched **operator** render —
   preserving the counter semantics `tile_planning`/`operator_graph` established.

3. **Snapshot, deadline, and budget inheritance.** Every dispatched render
   carries `request.snapshot` (the `StateHandle` pin, replacing the inert
   `StateHandle{}` at `tile_planning.cpp:321`) and `request.deadline`
   **verbatim** — neither reset nor recomputed, no per-pull sub-budgeting
   (doc 05:96-100, "budgets flow *through* recursion … the recursive case must
   cost what an equivalent flat scene would"). Recursive pulls (an operator whose
   input is itself an operator) descend under one `GraphBudget`; a descent
   exceeding `max_depth` — a divergent cycle at scale ≥1× — renders the
   **placeholder** and reports one diagnostic naming the content path through the
   caller-owned `GraphDiagnostics*` (doc 05:66-70, doc 13:134-138), the same
   backstop `operator_graph` installed, now driving real recursive evaluation.
   Every visit within one frame sees one revision set: the snapshot token rides
   in `request.snapshot` unchanged (doc 05:71-74).

**Wiring.** `render_frame_interactive` grows one trailing defaulted parameter
`PullService* pulls = nullptr` and **delegates its miss-fill** to it (when null,
it constructs an internal `PullServiceImpl` over the frame's `TileCache` with a
`DirectDispatch`, so the null path is bit-identical to today). Runtime binds the
worker-backed dispatch and injects the service at the reserved seams
(`worker_pool.hpp:41`, `interactive.hpp:116`), so operators attached to the
document receive a live `PullService`.

**Not this task:**

- **`pull_audio` / the audio facet.** The interface comment
  (`content.hpp:324-326`) assigns it: "The audio pull (`pull_audio`, doc 13:80)
  joins this interface with `contract.audio_facet`, which owns `AudioRequest`."
  The pull-based audio path is the `arbc::audio-engine` component (doc 17:57) and
  the `audio` stream (`tasks/45-audio.tji`, `mix_engine`), which owns
  `AudioRequest` and the block cache. `PullServiceImpl` implements only the visual
  `pull`; when the audio facet's request type lands, `pull_audio` is added to the
  interface and implemented there. **Existing streams own it — no new WBS leaf.**
- **Real operator / nested kinds.** `org.arbc.fade` / `crossfade` are the
  `operators` stream (`tasks/50-operators.tji`, doc 13:163-171); the nested-
  composition kind is `kinds.nested` (`tasks/55-kinds.tji:39`, doc 17:59, "nested
  uses only the `PullService` interface"). This task delivers and tests the
  service against **synthetic operator `Content` doubles** (the
  `src/contract/t/operator_members.t.cpp` / `RecordingPull` pattern), not real
  kinds; the contract conformance suite (`arbc-testing`, doc 16) runs against the
  real kinds when they land, and the operators stream's suite adds "input pulls
  only via PullService" (`tasks/50-operators.tji:24`).
- **Per-content revision granularity** (doc 05:84's "static children make deep
  hierarchies cheap"). `aggregate_revision` remains conservative under the
  document-global `DocRoot::revision()` (`operator_graph.md` Decision 3, parked to
  `tasks/parking-lot.md`); this task does **not** reopen it (aggregate revision is
  correct-but-conservative and generalizes with no compositor change).
- **Speculative next-rung priming, progressive refinement, damage mapping.**
  Those are `compositor.refinement` / `compositor.damage_planning` /
  `operator_graph`, already landed; this task consumes their outputs (the
  `RefinementQueue`, aggregate revision, routed damage), it does not re-implement
  them.

## Why it needs to be done

Doc 13's central claim is that a plugin "can do anything the core can"
(doc 13:88-89): an operator pulls its inputs through the *same* request/cache/
snapshot/budget machinery the compositor uses for a top-level layer, so nesting,
effects, and the frame itself are one recursion — "Rendering *is* recursion"
(doc 05:26). `operator_graph` made the graph *visible* (the core can read
`inputs()`, compute an aggregate revision, detect a cycle, spot an identity), but
nothing yet **pulls**: an operator has no way to render its input's tiles without
calling `input->render()` directly, which "would bypass caching, scheduling,
snapshots, and budgets" (doc 13:69-71). This task lands the seam that closes that
gap — the "one genuinely new core API" (doc 13:85). Concretely it is the gate for
two whole streams: `kinds.nested` (M4) is "recursive composition on PullService"
(`tasks/55-kinds.tji:40`) and cannot exist without it, and every reference
operator (`operators` stream) proves itself by pulling only through this service
(`tasks/50-operators.tji:24`). Until it lands, doc 13 and doc 05 are contract
without engine.

## Inputs / context

**Design docs (normative, doc 16):**

- **doc 13 — effects as operators.** The seam: "At attach, content receives a
  `PullService`" whose `pull` uses the "Same machinery as a compositor-issued
  request: cache lookup first, worker scheduling, snapshot token respected,
  deadline/budget inherited" (`13-effects-as-operators.md:69-83`); "the doc-05
  synthetic viewport generalized … the request/cache machinery, exposed as a
  service to any content" and "the one genuinely new core API" (`13:85-89`).
  Region/scale/time dependencies: "a fade pulls the same region; a blur pulls the
  region inflated by its radius … a fade evaluates its envelope at the request's
  time and pulls the input at that same time … each an ordinary pull, each
  hitting the time-keyed cache" (`13:97-116`). Caching: an operator's output is
  "keyed by its id and its *aggregate* revision"; "input tiles cache under the
  input's identity (shared by every consumer), operator output under the
  operator's"; "Async composes … deadlines and cache budgets flow through pulls
  exactly as they flow through nesting" (`13:122-132`). Cycles "get doc 05's rules
  verbatim … divergent ones hit the recursion-depth budget and the cycle
  diagnostic" (`13:133-138`). New machinery item 1 = "`PullService` — the real
  one … Mostly *moves* the doc-05 synthetic-viewport internals behind a public
  seam" (`13:173-177`). v1 scope: PullService ships, and "the nested-composition
  kind is built *on* `PullService` from the start (never built twice)"
  (`13:201-205`).
- **doc 05 — recursive composition.** "the synthetic viewport inherits the outer
  request's deadline, and tiles allocated inside a child count against the same
  global cache budget (priority-classed by *outermost* visibility) … the recursive
  case must cost what an equivalent flat scene would" (`05-recursive-composition.md:93-100`).
  "the compositor carries a recursion-depth budget per request as a backstop;
  exceeding it renders the placeholder and reports a diagnostic naming the cycle"
  (`05:66-70`); "within one frame/snapshot, every visit to the same composition
  sees the same revisions (doc 02's snapshot token rides along in `RenderRequest`)"
  (`05:71-74`); two-level caching keyed by the child's aggregate revision
  (`05:76-91`).
- **doc 02 — architecture.** Layer rendering "runs on a worker pool. Requests
  carry everything the layer needs (region, scale, deadline, target surface)"
  and async settlement "occupying no worker" (`02-architecture.md:126-132`); the
  scene renders "under a snapshot — concretely, a pinned document version"
  (`02:124`). The byte budget "is a soft target … correctness … outranks the
  budget" (`02:110-116`), the budget model a pull inherits.
- **doc 17 — components.** `arbc::compositor` (Level 4) scopes this task
  literally: "… cycle handling, `PullService` implementation"; allowed deps
  **`contract`, `cache`** only (line 56). The `PullService` *interface* is
  contract's (L3, line 53); `arbc::runtime` (L5) "Depends on: everything below"
  (line 60) — so runtime, not the compositor, binds the worker pool and injects
  the service downward. `arbc::kind-*` "nested uses only the `PullService`
  interface" (line 59).

**Source seams (real paths + lines):**

- `src/contract/arbc/contract/content.hpp:314-340` — the `PullService` interface
  this task implements (`pull` at `:335-336`); `:76-84` `RenderRequest`
  (region, scale, time, **snapshot**, target, exactness, **deadline**); `:86-99`
  `RenderResult`; `:119-151` `RenderCompletion`; `:56-63` `Deadline`;
  `:259-260` `Content::render`, `:275` `render_thread_safe`; `:179-191`
  `Editable` facet (`capture`/`restore` — the `StateHandle` source);
  `:160-161` `ContentRef = Content*`.
- `src/compositor/arbc/compositor/tile_planning.hpp:153-158` `plan_layer`,
  `:241-247` `render_frame_interactive` (the delegation site + new `pulls`
  param); impl `src/compositor/tile_planning.cpp:299-403` — the inline miss-fill
  this task lifts into the service (`:320-322` request build with inert
  `StateHandle{}`; `:349` render call; `:356-358` insert; `:362-373` async →
  `RefinementQueue`).
- `src/compositor/arbc/compositor/operator_graph.hpp:105-107` `aggregate_revision`
  (operator-input keying), `:159-160` `resolve_identity`, `:58-60` `GraphBudget`,
  `:76-78` `GraphDiagnostics`, `:83-85` `is_operator`.
- `src/compositor/arbc/compositor/counters.hpp:34-66` `CompositorCounters`
  (`note_request_issued`/`note_operator_render`/`note_composite`, `:56-59`),
  `:85-94` `counters_snapshot`.
- `src/cache/arbc/cache/key_shapes.hpp:64-76` `TileKey`, `:110-113` `TileValue`,
  `:129` `TileCache`; `src/cache/arbc/cache/keyed_store.hpp:163` `insert`,
  `:169` `lookup`, `:78-126` `CacheHold`, `:26-32` `PriorityClass`.
- `src/runtime/arbc/runtime/worker_pool.hpp:41` — "The `PullService` dispatch
  seam is injected downward at wiring time (a `std::function` over `submit`)";
  `:50-54` `RenderTask`, `:57-73` `WorkerPoolConfig` (`worker_count == 0` =
  inline degenerate executor), `:78-163` `WorkerPool` (`submit` `:96`, `poke`
  `:102`, `wait_completions` `:110`), `:34-40` clock-free.
- `src/runtime/arbc/runtime/interactive.hpp:116` — `worker_pool()` exposed "so
  … `compositor.pull_service`'s dispatch can `poke()` a render thread parked in
  this loop's `wait_completions`"; `:68-139` `InteractiveRenderer`, `:97-101`
  `render_frame`, `:104-108` persistent `d_counters`.
- `src/contract/t/operator_members.t.cpp:85-133` — `RecordingPull : public
  arbc::PullService` and the abstract-interface witness; `:208-228` forwards ref,
  request, completion unchanged — the synthetic-double pattern this task's tests
  mirror. `src/contract/t/snapshot_pins.t.cpp` — render purity over the pinned
  snapshot.

**Test / registry conventions:** claim id is `<doc-file-stem>#<kebab-slug>`
(`tests/claims/registry.tsv`, TAB-separated 2-column), enforced bidirectionally
by `scripts/check_claims.py`; each registered claim needs a live test tagged
`// enforces: <claim-id>`. Compositor unit tests live in
`src/compositor/t/<name>.t.cpp` (Catch2, `arbc_component_test`); cross-component
tests that link a real `WorkerPool` + `CpuBackend` live in `tests/`.

## Constraints / requirements

- **Levelization (doc 17:56).** `arbc::compositor` (L4) may reach only `contract`
  and `cache` (+ transitive). `PullServiceImpl` **must not** name any `runtime`
  type (`WorkerPool`, `RenderTask`) — the worker path enters only through the
  injected `RenderDispatch` functor, whose signature is over `contract` types
  (`Content*`, `RenderRequest`, `RenderCompletion`) alone. Runtime (L5) binds that
  functor to `WorkerPool::submit` and performs the attach-time injection at the
  seams it already reserved (`worker_pool.hpp:41`, `interactive.hpp:116`). **No
  new DEPENDS edge; no same-level edge; no upward edge.** The CI component-graph
  check stays green; `pull_service.hpp` compiles standalone.
- **Zero behavior change on the null path (byte-exact).** `render_frame_interactive`
  with `pulls == nullptr` must plan, key, fill, and composite **byte-for-byte** as
  today: the internal `DirectDispatch` reproduces `content->render` inline
  settlement, the same `note_request_issued`/`note_operator_render`/`note_composite`
  deltas, the same `RefinementQueue` routing for async. All landed
  tile_planning / refinement / anchored_viewports / damage / operator-graph
  goldens stay green unchanged. The new parameter is trailing and defaulted.
- **Cache-first ordering is normative.** `pull` **must** consult `TileCache`
  before any dispatch; a qualifying hit issues zero dispatches and zero renders
  (doc 13:76-77). A miss dispatches exactly one render (no duplicate per
  consumer of a shared input — the input's identity keys one entry, doc 13:126).
- **Snapshot & deadline carried verbatim.** The dispatched render's `snapshot`
  and `deadline` fields **must equal** the pull request's — no reset, no
  per-pull recompute, no sub-budget (doc 05:96-100). This includes fixing the
  top-level driver's inert `StateHandle{}` (`tile_planning.cpp:321`) to carry the
  layer plan's real pin.
- **Budget backstop soundness.** Recursive pulls thread one `GraphBudget`;
  exceeding `max_depth` selects the placeholder and emits exactly one
  `GraphDiagnostic` naming the content path — never unbounded recursion, never a
  crash (doc 05:66-70). Convergent cycles bottom out earlier by the sub-pixel
  cull (`anchored_viewports`); this task guarantees the budget is a sound
  backstop, it does not re-implement the cull.
- **Concurrency (doc 02:126-132) — TSan obligation in scope.** The worker-backed
  path renders on a pool and settles `RenderCompletion` **off-thread**, drained
  and inserted into the cache on the frame thread (single-writer cache, matching
  the current driver). `PullServiceImpl` **must not** introduce concurrent cache
  mutation: probe and `insert` stay on the calling/draining frame thread; only
  `content->render` into its thread-confined `target` runs on a worker
  (governed by `render_thread_safe`). This is concurrency-touching, so a
  **TSan/stress test over the async dispatch path with a real multi-worker
  `WorkerPool` is scoped explicitly** (doc 16).
- **CI diff coverage ≥90%** on changed lines (doc 16) — tests ship with the task.

## Acceptance criteria

**Claims — new (registered in `tests/claims/registry.tsv`, each with an
`enforces:`-tagged test in `src/compositor/t/pull_service.t.cpp`):**

- `13-effects-as-operators#pull-is-cache-first` — "A `PullService::pull` whose
  input tile is resident under the input's identity and aggregate revision
  completes `done` synchronously from the cache and dispatches no render
  (dispatch-count delta 0, `requests_issued` delta 0); a `pull` whose fresh key
  misses dispatches exactly one render carrying the request unchanged, inserts the
  result tile under the input's identity, and settles `done` from it."
- `13-effects-as-operators#pull-inherits-snapshot-and-deadline` — "The render a
  `pull` dispatches for a cache miss carries the pull request's `snapshot`
  `StateHandle` and `deadline` value **verbatim** — neither reset nor recomputed —
  and each recursive pull propagates the same snapshot token, so every visit to a
  node within one frame sees one revision set."

**Claims — re-asserted (add a second `enforces:` test; do *not* re-register —
these are existing claims this task now drives through the pull engine):**

- `05-recursive-composition#graph-walk-bounds-cycles` — a **planning descent
  driven by real recursive pulls** (operator-over-operator) exceeding
  `GraphBudget::max_depth` selects the placeholder and reports one diagnostic,
  extending the pure-walk coverage `operator_graph` registered.
- `02-architecture#miss-becomes-deadline-request` (`registry.tsv:65`) — the
  service now builds the miss request, carrying the frame `Deadline` **and the
  real pinned `StateHandle`** (closing the inert-`StateHandle{}` gap), so the
  claim's "pinned StateHandle" clause is exercised end-to-end.
- `02-architecture#async-arrival-emits-damage` — an async pull (dispatched render
  returns `nullopt`) records pending, occupies no worker, and on completion
  inserts under `Visible` and emits damage so a follow-up frame re-plans it as a
  fresh hit — now proven through `pull`, not only the inline driver.

**Unit / behavioral test** — `src/compositor/t/pull_service.t.cpp` (new, Catch2,
`arbc_component_test`; synthetic operator `Content` doubles and a **recording /
deferrable `RenderDispatch`** à la `operator_members.t.cpp`):
warm-hit (resident tile → synchronous completion, 0 dispatches, 0
`requests_issued`); cold-miss (1 dispatch, request `snapshot`+`deadline` equal,
inline result inserted, `done` settled); async-compose (deferred dispatch settled
later → `done` settles, no dispatch blocked, damage emitted, `RefinementQueue`
grows one); snapshot fidelity (a nested operator→operator→leaf pull carries the
same `StateHandle` at every hop); budget (depth-N recursive pull within budget
succeeds; depth-(N+1) or a cycle → placeholder + one `GraphDiagnostic`); operator
counter (dispatched operator render bumps `operator_renders` once; identity pull
short-circuits to input tiles with `operator_renders == 0` and no operator cache
entry); and **null-path neutrality** (`render_frame_interactive` with
`pulls == nullptr` reproduces every counter delta and cache op of the pre-task
inline fill).

**No new golden.** This task changes *where* the render/cache machinery lives, not
*what pixels* it produces: the leaf-path pixels are guarded byte-exact by the
landed tile_planning / refinement / anchored_viewports goldens, re-run **through
the `PullServiceImpl`-delegated fill** (the default `DirectDispatch` path) to
prove the move is bit-identical. Operator-output pixels arrive with the real
fade / crossfade / nested kinds (operators / kinds streams) and get their goldens
there.

**TSan / stress** — `tests/pull_service_async.t.cpp` (new, cross-component: links
`arbc` + `CpuBackend` + a real multi-worker `WorkerPool`): many concurrent
async pulls of thread-safe leaf content, asserting under ThreadSanitizer no data
race on the completion plumbing or the cache (probe/insert stay single-writer on
the draining thread), and that resident bytes / eviction counts are consistent
after drain (doc 16 — behavioral, never wall-clock).

**No new WBS leaf.** Every deferral routes to an **existing** stream:
`pull_audio` → the `audio` stream / `arbc::audio-engine`, which owns
`AudioRequest` (`content.hpp:324-326`, doc 17:57); real operator kinds →
`operators` (`tasks/50-operators.tji`); nested composition → `kinds.nested`
(`tasks/55-kinds.tji`); per-content revision granularity remains the parked
model-stream question (`operator_graph.md` Decision 3). The runtime dispatch
binding + attach injection ride the seams runtime **already reserved by name**
(`worker_pool.hpp:41`, `interactive.hpp:116`) and land in this task's commit — no
separate leaf. **The closer registers no new task for this refinement.**

**Component wiring & CI dependency check.** `arbc::compositor` `DEPENDS` stays
`contract cache`; `pull_service.{hpp,cpp}` are wired into
`src/compositor/CMakeLists.txt`; `pull_service.hpp` compiles standalone; the
doc-17 component-graph check is green; no `runtime`, `backend-cpu`, or same-level
edge is added.

**Gate green.** Build + tiers 1–5 in Debug + ASan/UBSan pass; the scoped **TSan**
tier passes; `check_claims.py` is bidirectionally satisfied; CI diff coverage
≥90%.

## Decisions

1. **A concrete `PullServiceImpl` in a new compositor-internal
   `pull_service.{hpp,cpp}`, injected as `PullService*`.** The service holds the
   frame's `TileCache&`, the operator-graph hooks (`aggregate_revision`,
   `GraphBudget`, `GraphDiagnostics*`), the `CompositorCounters*`, and a
   `RenderDispatch` functor. *Rationale:* mirrors the pure-seam posture of every
   compositor sibling and keeps the engine unit-testable against synthetic
   `Content` doubles + a recording dispatch, with no runtime, no real pool. It
   implements the interface the contract already fixed (`content.hpp:314-340`).
   *Rejected:* leaving the fill inline in `render_frame_interactive` and giving
   operators a separate ad-hoc path — that is doc 13:204's "built twice" and
   would let operator pulls bypass the driver's cache/budget accounting.

2. **The worker path enters only through an injected `RenderDispatch`
   `std::function`; runtime binds it to `WorkerPool::submit`.** The functor's
   signature is over `contract` types only, so the compositor never names a
   `runtime` type. *Rationale:* the sole levelization-legal shape (doc 17:56 vs
   60), and it is exactly what `worker_pool.hpp:41` pre-declared ("a
   `std::function` over `submit`"). *Rejected:* a `WorkerPool&` member on
   `PullServiceImpl` — an illegal L4→L5 edge; *rejected:* moving the whole
   `PullService` implementation up into `runtime` — contradicts doc 17:56, which
   places the implementation in the compositor, and would strand `kinds.nested`
   (L4) without an L4 pull engine.

3. **`render_frame_interactive` delegates its miss-fill to the service via a
   trailing defaulted `PullService* pulls = nullptr`; the null path constructs an
   internal `PullServiceImpl` with `DirectDispatch`.** *Rationale:* one fill path
   (doc 13:176-177, "moves … behind a public seam"), while the seam-growth
   discipline + `DirectDispatch` keep the default single-threaded path
   byte-for-byte against every landed golden and counter delta. The worker-backed
   dispatch is opt-in from runtime. *Rejected:* a hard cut-over to the worker pool
   as the default — it would perturb goldens/counter timing and force the whole
   compositor test tier onto a concurrent executor with no behavioral gain in the
   still-scene case.

4. **Operator recursion is evaluated synchronously on the frame thread within
   `GraphBudget`; only leaf `render` is dispatched to workers; the cache is
   single-writer on the frame/drain thread.** *Rationale:* matches the current
   driver's single-threaded cache discipline (doc 02:135-137) and `WorkerPool`'s
   `wait_completions` drain model (`worker_pool.hpp:110`), so the task adds no
   concurrent cache mutation — the only cross-thread surface is the already
   thread-safe `RenderCompletion`. It makes the recursion-depth budget a clean
   bound on synchronous descent. *Rejected:* fanning input pulls out onto workers
   concurrently in v1 — it would demand a concurrent/sharded cache and a far
   larger TSan surface for a throughput optimization no v1 milestone needs; if it
   is ever wanted it is a self-contained future optimization, surfaced to the
   parking lot, not encoded now.

5. **The recursion-depth budget, snapshot, and deadline are inherited from the
   incoming `RenderRequest` / walk state, never reset per pull.** The dispatched
   render copies `request.snapshot` and `request.deadline` unchanged; `GraphBudget`
   decrements by depth, not by reset. *Rationale:* doc 05:96-100's invariant "the
   recursive case must cost what an equivalent flat scene would" — resetting any
   of the three would multiply a deep scene's entitlement by nesting. *Rejected:*
   giving each pull a fresh deadline/budget slice — the precise resource-inflation
   bug doc 05 forbids.

6. **`aggregate_revision` keys an operator input's tiles; a leaf input keys on its
   own revision (the fold degenerates to `contribution(root)`).** *Rationale:*
   reuses `operator_graph`'s landed fold verbatim (`operator_graph.hpp:105-107`),
   so an operator's cache entry invalidates iff a reachable input changes
   (doc 13:124-126) with no new keying machinery. *Rejected:* a pull-local
   revision scheme — it would duplicate the fold and diverge from the planner's
   keys, risking stale-tile / double-cache bugs.

- **No design-doc delta.** `PullService` (`content.hpp:314-340`), cache-first +
  worker scheduling + snapshot + deadline/budget inheritance (doc 13:69-89,
  122-132; doc 05:66-74, 93-100), two-level caching by aggregate revision
  (doc 13:124-126), and the recursion-depth budget + cycle diagnostic
  (doc 05:66-70, doc 13:134-138) are all settled doc text and existing contract.
  This task concretizes them into C++ without altering designed behavior; the
  runtime injection seams were reserved for it by the earlier runtime work.

## Open questions

(none — all decided.) Two items are routed to the return summary for the parking
lot rather than resolved as WBS work: (a) whether v1 should ever evaluate
operator input pulls **concurrently** (Decision 4 defers it — a self-contained
throughput optimization needing a concurrent cache, no milestone requires it);
and (b) the exact `audio` stream home and effort for `pull_audio` once
`contract.audio_facet` fixes `AudioRequest` (the interface comment assigns
ownership to that stream, `content.hpp:324-326`, but the leaf is that stream's to
scope, not the compositor's). Neither is a compositor task.

## Status

**Done** — 2026-07-06.

- `src/compositor/arbc/compositor/pull_service.hpp` + `src/compositor/pull_service.cpp` — `PullServiceImpl` (concrete L4 `PullService`), `RenderDispatch` functor, `direct_dispatch()`, `PullConfig`.
- `src/compositor/t/pull_service.t.cpp` — 8 Catch2 unit/behavioral cases: warm-hit, cold-miss, snapshot fidelity, async-compose, operator-counter/identity, recursion budget, identity-cycle, direct_dispatch+null-input, null-path neutrality.
- `tests/pull_service_async.t.cpp` — TSan/stress test with real 4-worker `WorkerPool` + `CpuBackend`; no data race on completion plumbing or cache.
- `src/compositor/arbc/compositor/tile_planning.hpp` + `src/compositor/tile_planning.cpp` — added trailing `PullServiceImpl* pulls` param to `render_frame_interactive`; miss-fill delegates to `PullServiceImpl`; request now carries `plan.snapshot`.
- `src/compositor/CMakeLists.txt` + `tests/CMakeLists.txt` — wired new source/headers/tests.
- `tests/claims/registry.tsv` — registered two new claims: `13-effects-as-operators#pull-is-cache-first`, `13-effects-as-operators#pull-inherits-snapshot-and-deadline`; re-asserted existing claims `05-recursive-composition#graph-walk-bounds-cycles`, `02-architecture#miss-becomes-deadline-request`, `02-architecture#async-arrival-emits-damage` via new `enforces:` tags.
- Null path remains byte-exact against all landed tile_planning / refinement / anchored_viewports goldens and counter deltas; `interactive.cpp` not re-wired (worker-backed dispatch binding proven in TSan test; operators/kinds attach-injection deferred to M4 streams).
