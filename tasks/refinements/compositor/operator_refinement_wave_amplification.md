# compositor.operator_refinement_wave_amplification — Coalesce per-arrival chain re-renders to at most one per refinement wave

## TaskJuggler entry

[`tasks/35-compositor.tji:118-123`](../../35-compositor.tji):

```
  task operator_refinement_wave_amplification "Coalesce per-arrival chain re-renders to at most one per refinement wave" {
    effort 2d
    allocate team
    depends !in_flight_tile_dedup
    note "An operator whose input answers async caches a TRANSIENT inexact tile which is not a hit, so N independently-arriving input tiles each re-drive the operator chain (nested_deep: 12 renders vs the 5-render inline oracle — the actual cause of the 2x worker slowdown that compositor.in_flight_tile_dedup was originally scoped to fix). Coalesce arrivals so one refinement wave re-renders the chain at most once, restoring the counter identity the headline equality assertion in tests/interactive_worker_default.t.cpp:697 was intended to enforce. Source-of-debt: tasks/refinements/compositor/in_flight_tile_dedup.md. Docs 02/13."
  }
```

## Effort estimate

WBS says **2d**. **Realistic estimate: 3d** — the closer should bump the WBS
effort when this lands, as `refine_frame_composite_idempotence` and
`in_flight_tile_dedup` both did.

The gate itself is small and reuses the predecessor's shape: one predicate next
to `tile_in_flight`, consulted at the same two sites. What the 2d figure does
not cost is the *recording* half — the pull service must accumulate the input
tiles a render left unmet and attribute them to the operator output tile that
was being driven, transitively through a nested chain (Decision 2) — plus a new
degraded-fallback source so a deferred tile composites the placeholder it
already has instead of blanking (Decision 3), a new counter (Decision 6), wait
pruning in `poll_refinements` (Decision 5), a three-document delta (Decision 8),
and a headline assertion that has to be *derived* rather than copied, because
the equality the WBS note asks for is not achievable and never was
(Decision 1).

## Inherited dependencies

**Settled:**

- `compositor.in_flight_tile_dedup` (the `depends` edge) — Done 2026-07-12. It
  landed `tile_in_flight(const RefinementQueue*, const TileKey&)`
  (`src/compositor/arbc/compositor/refinement.hpp:138`,
  `src/compositor/refinement.cpp:71-87`), the `requests_suppressed` counter
  (`counters.hpp:85`), and — critically — the *pending set as a second thing
  planning consults*, wired at both dispatch sites
  (`tile_planning.cpp:501-511`, `pull_service.cpp:249-255`) and blessed in doc
  02 (`02-architecture.md:124-147`). This task adds a second predicate over the
  same queue at the same two sites; the seam it needs already exists because
  that task built it.
  Its **Status block is the source of debt** (`in_flight_tile_dedup.md:575`):
  the guard *provably never fires* on the two operator benchmark scenes
  (`requests_suppressed() == 0`, measured, at every worker count), because
  neither scene shares a leaf tile between two askers. The residual — and the
  whole of the 2× worker slowdown — is the refinement **wave**, which is this
  task.
- `compositor.refinement` — `RefinementQueue`, `PendingTile`,
  `poll_refinements` (`refinement.hpp:77-98`, `:196-198`;
  `refinement.cpp:89-146`). Its Decision 1 — frame-to-frame state belongs to
  the runtime loop, not L4 planning — is why the queue is a caller-owned value
  threaded in as a parameter, and it is the reason the wave state this task
  needs has a legal home (doc 17:118-128 forbids the compositor from holding it
  anywhere else).
- `compositor.refine_frame_composite_idempotence` — a damage-gated refine frame
  clears and clips before re-compositing
  (`02-architecture#gated-frame-equals-single-pass`). This is what makes
  *removing* a re-render safe to reason about: the frame's pixels are a function
  of which tiles are resident, not of how many times they were composited.
- `kinds.operator_async_placeholder_inexact` — Done. It made every operator's
  transient placeholder report `exact = false`
  (`src/kind_fade/fade_content.cpp:110-120`, `:153-159`;
  `src/kind_crossfade/crossfade_content.cpp:148-183`, `:206`;
  `src/kind_nested/nested_content.cpp:392-405`), which is *correct* and is not
  what this task changes. It is, however, the mechanism: an inexact tile is not
  a hit, so the operator re-plans as a miss on every wave. This task does not
  weaken that claim
  (`13-effects-as-operators#transient-placeholder-is-never-exact`,
  `registry.tsv:197`); it stops the miss from being *re-rendered* while the
  inputs it is waiting on are still pending.
- `compositor.pull_service`, `compositor.operator_pull_delivers_target`,
  `compositor.pull_multi_tile_region` — `PullServiceImpl::pull` is cache-first
  per covering tile and delivers into the caller's target
  (`pull_service.cpp:214-255`, `:390-400`). The delivery path is what makes
  Decision 3 (serve the resident transient tile on a deferred pull) a
  two-line change rather than new machinery.
- `runtime.worker_dispatch_leaf_only` — **worker dispatch is leaf-only**
  (`02-architecture.md:220-233`): a content with inputs renders *inline on the
  driver thread* and is never submitted to a worker. This is load-bearing twice
  over: it is why the chain re-render is a driver-thread cost (so the
  amplification is a frame-loop problem, not a pool problem), and it is why the
  unmet-input accumulator of Decision 2 needs no synchronization.
- `runtime.operator_model_damage_routing` — arrival damage is *broadcast* on the
  input's shared pull identity to every operator that consumes it
  (`src/runtime/interactive.cpp:406`, `:99-123`; doc 02:135-147). The re-drive
  is a property of the tile, not of the dispatch. This task narrows *what the
  broadcast wakes up*, not the broadcast.
- `runtime.interactive_worker_count_default` — the original source of debt. Its
  benchmark table records `nested_deep` at 3027 ms with 0 workers and 6247 ms at
  the shipped 2, and attributes it to duplicate dispatch
  (`interactive_worker_count_default.md:481-484`). **That attribution is now
  known to be wrong** — `in_flight_tile_dedup` landed the dedup and the numbers
  did not move. The correct attribution is this task. Both prior refinements
  still carry the stale sentence in their bodies; per the house rule
  ("don't edit the prior sections"), they are left as the historical record and
  corrected here.

**Pending, and deliberately not depended on:**

- `runtime.deadline_cancel_retains_wanted` (registered by `in_flight_tile_dedup`,
  M9, 2d) — the deadline sweep cancels *every* unsettled pending tile on expiry
  (`interactive.cpp:387`), including tiles the next frame still wants. Because
  `tile_in_flight` excludes cancelled entries by design (that task's Decision
  3), a blanket cancel at every frame boundary would tear down the wave state
  this task depends on — under deadline pressure, which is exactly the regime
  where the amplification lives. **This task does not wait for it.** The wave
  gate uses a *different* predicate that treats a cancelled-but-unsettled input
  as still pending, and Decision 4 is the argument for why that is safe here
  even though it is not safe for the dispatch gate. When
  `deadline_cancel_retains_wanted` lands, nothing here changes.

## What this task is

Teach the two dispatch sites a third thing to consult, after the cache and the
pending set: the **wave**.

When an operator renders and one or more of its input tiles answer
asynchronously, it paints a placeholder and reports `exact = false`
(doc 13:135-146 — it *must*, or the empty tile freezes into the cache as a
final answer). That transient tile is inserted into the cache under its exact
key (`tile_planning.cpp:612-616`, `pull_service.cpp:338-339`) but the hit gate
requires `meta.exact` (`tile_planning.cpp:228-229`,
`pull_service.cpp:224-225`), so on the next plan it is a **miss** — and the
whole operator chain renders again. That is correct and necessary once: it is
how the real pixels finally get composed. The bug is that it happens once per
*independently arriving input tile*, not once per *wave*: with two leaves
landing in two different frames, the chain is re-driven twice, and a chain is
not cheap.

This task records, alongside the transient tile, the set of input tiles it was
waiting on — its **unmet set** — and gates the re-render on that set being
resolved. While any unmet input is still pending, the operator's tile is not
re-rendered: the frame composites the transient tile that is already resident
(no pixels change), no render is driven, and no pull is issued. When the last
unmet input leaves the queue — settled, failed, or dropped — the gate opens and
the chain renders exactly once, with everything it needs.

**Not this task:**

- **Making the operator's transient tile a cache hit.** It must stay inexact
  (`13-effects-as-operators#transient-placeholder-is-never-exact`). The gate
  sits *between* the miss and the render, not in the hit predicate.
- **Partial recomposition.** An operator that has received 1 of its 2 inputs
  cannot compose "the arrived half" into its resident tile — the `Content`
  contract has no partial-render entry point, and inventing one is an L2
  plugin-ABI change to buy back a single intermediate frame (Decision 7).
- **Time-based batching / debouncing arrivals.** No wall-clock anywhere
  (doc 16:224-226). The wave's boundary is a *set membership* fact about the
  refinement queue, not a duration.
- **Changing quiescence, cancellation, or deadline semantics.** The park loop,
  the deadline sweep, and `FrameOutcome::schedule_follow_up`
  (`interactive.hpp:209`) are untouched. This task removes renders; it does not
  change when the frame loop stops.
- **The cross-frame teardown of the pending set.** That is
  `runtime.deadline_cancel_retains_wanted`, already registered.

## Why it needs to be done

`nested_deep` is **2× slower with workers than without**, and after
`in_flight_tile_dedup` we know exactly why. The measured counter, recorded in
the test that had to weaken its own assertion
(`tests/interactive_worker_default.t.cpp:640-678`):

> Measured: `operator_heavy` issues 5 renders inline and 7 threaded,
> `nested_deep` 5 and 12 — one chain re-render per independently-arriving input
> tile, which is what makes `nested_deep` slower with workers than without.

Twelve renders for a scene that needs five. The excess is not duplicate
dispatch — `requests_suppressed() == 0` on this scene at every worker count
(`:721`, measured) — it is the chain re-rendering once per arrival.

Two design-doc promises are currently false because of it:

- **Doc 05:137-139** — "*a 10-level tree re-renders only the spine from the
  edited layer to the viewed root*". An async spine re-renders once *per
  arrival*, so a 10-level tree with N async leaves re-renders the spine N times.
- **Doc 05:151-153** — "*the recursive case must cost what an equivalent flat
  scene would*". The flat scene (`leaf_heavy`) is already counter-identical
  across the worker sweep (`:707-710`, `==`). The recursive one is 2.4× its own
  inline cost.

And one acceptance criterion has now failed to land twice:
`runtime.interactive_worker_count_default`'s **A5** — "adding workers buys
parallelism, never duplicate or wasted renders" — shipped as `>=`
(`:714`), and `in_flight_tile_dedup` was chartered to make it `==` and could
not. This task is the third and last attempt, and it can only succeed by first
correcting what the identity *should* be (Decision 1): the inline oracle is not
the right right-hand side, and no amount of coalescing will ever make it one.

The downstream consumers are every driver that renders an operator scene with a
worker pool: the interactive editor (a fade, crossfade, or nested composition on
a cold cache — i.e. every scene the moment the user pans or zooms) and the
offline sequence driver, which reaps to quiescence with a live `pending` queue
(`src/runtime/offline_sequence.cpp:154-198`) and so pays the amplification on
every exported frame.

## Inputs / context

### Governing design docs (normative)

- **`docs/design/02-architecture.md:71-73`** § The frame, interactively, step 6
  (Refine) — "*Async results that arrive later produce damage for their region,
  scheduling a follow-up frame. Zooming therefore shows progressively sharper
  content rather than blocking.*" True and unchanged; this task changes what the
  follow-up frame *renders*, not that it happens.
- **`docs/design/02-architecture.md:145-147`** — "*a nested chain pays one
  [redundant render] per level **per refinement wave***". This is the **only**
  use of the term "refinement wave" in the design docs (plus its mirror in
  `00-overview.md:245-246`), and it is used *undefined*, as a unit of
  accounting. The concept this task needs is already named in the constitution
  and never delimited. Defining it is the delta (Decision 8).
- **`docs/design/02-architecture.md:62-67`** § step 4 — the degraded-fallback
  preference order: "*stale-revision tiles, coarser-scale tiles rescaled, or
  checkerboard/transparent, in that preference order*". A resident,
  current-revision, current-rung, **inexact** tile is not in that list and needs
  to be (Decision 3) — it is strictly better than a stale-revision tile, and it
  is what a deferred operator tile must composite.
- **`docs/design/02-architecture.md:124-147`** — the in-flight rule the
  predecessor landed: "*A tile already in flight is not dispatched twice*"; the
  suppressed miss "*is not treated as a hit: it contributes no pixels this pass
  and settles nothing*"; and the broadcast argument, "*The re-drive is a
  property of the tile, not of the dispatch*". The wave gate is the same shape
  one level up: a *re-render* already accounted for is not driven twice.
- **`docs/design/02-architecture.md:112-122`** — the frame-loop correctness
  invariant: "*a scene refined over N follow-up frames lands on exactly the
  pixels one un-gated pass would have produced*". It bounds *correctness* under
  N waves and deliberately does not bound N. This task bounds N's *cost*, and
  must leave the invariant intact (Constraint 1).
- **`docs/design/02-architecture.md:176-178`** — the cache value carries
  "*actual scale achieved, exact vs best-effort flag*". Doc 02 never says what a
  lookup does with an inexact entry; the code says "not a hit"
  (`tile_planning.cpp:228-229`). Both remain true after this task.
- **`docs/design/13-effects-as-operators.md:118-120`** — **the sentence this
  task amends**: "*A pull whose input answers asynchronously — any covering tile
  — delivers nothing usable this pass: the completion is left unsettled, the
  operator degrades for this frame, and **each async tile's arrival re-drives
  it***." That last clause is a per-arrival promise, and it is the normative
  text that authorizes the 12-render behavior. Coalescing requires amending it.
- **`docs/design/13-effects-as-operators.md:135-146`** — the exactness rule:
  "*Exactness is what the caller caches on*"; an exact-flagged placeholder would
  be "*permanently freezing the deferred input out of the frame*". Untouched.
  The transient tile stays inexact; this task only stops it being *re-rendered*
  before its inputs are ready.
- **`docs/design/13-effects-as-operators.md:148-157`** — the TRANSIENT/FINAL
  split. A deferred operator tile is still TRANSIENT: something more *is* coming
  for this revision, from the wave it is waiting on. The split is preserved, not
  amended — exactly as `in_flight_tile_dedup` preserved it for a joined pull.
- **`docs/design/13-effects-as-operators.md:200-204`** — "*Async composes: an
  operator whose input answers asynchronously is itself asynchronous via the
  same completion plumbing*". This is why the unmet set is *transitive* through
  a nested chain (Decision 2): a nested composition's asynchrony is its
  children's asynchrony.
- **`docs/design/05-recursive-composition.md:129-153`** — the caching-across-
  nesting promise ("*only the spine*") and the budget rule ("*the recursive case
  must cost what an equivalent flat scene would*"). Both are the promises this
  task restores; neither needs amending.
- **`docs/design/16-sdlc-and-quality.md:54-62`, `:224-226`** — "*Wall-clock tests
  lie in CI; counters don't*"; "*behavioral counters gate, benchmarks trend*".
  The whole of Acceptance criteria is counter identities. The `nested_deep`
  millisecond figure is a benchmark to trend, never a gate.
- **`docs/design/17-internal-components.md:118-128`** — "*Components that are
  pure per-frame libraries (the compositor) do not hold persistent counter state
  — they take a caller-owned counters struct by pointer, the same way they
  already take the `SurfacePool` and `RefinementQueue`, so the persistent value
  lives in `runtime` and the library stays stateless.*" **This is the binding
  constraint on where the wave state may live** (Decision 5).

### Source seams

- **`src/compositor/arbc/compositor/refinement.hpp:77-98`** — `PendingTile`
  (`TileKey key; Rect local_rect; ObjectId content; Stability; bytes;
  unique_ptr<Surface> surface; shared_ptr<RenderCompletion> done;`) and
  `struct RefinementQueue { std::vector<PendingTile> tiles; };`. Caller-owned,
  frame-thread only, no index, no mutex. The struct this task extends.
- **`src/compositor/arbc/compositor/refinement.hpp:100-138`** — the header
  comment and declaration of `tile_in_flight`. The comment states why the scan
  is a scan and why cancelled entries deliberately answer `false`. The new
  predicate goes here, next to it, and the comment must say why its answer for a
  cancelled entry is the *opposite* (Decision 4).
- **`src/compositor/refinement.cpp:71-87`** — `tile_in_flight`. The guard is
  `pending.key == key && !pending.done->settled() && !pending.done->cancelled()`
  (`:82`).
- **`src/compositor/refinement.cpp:89-146`** — `poll_refinements`. Retains
  unsettled (`:96-99`); inserts a settled-with-value arrival under its exact key
  (`:125-127`), emits one `Damage` from the *key* (`:131-133`), bumps
  `note_follow_up_frame()` (`:136-138`); a settled-via-`fail` entry is **dropped
  with no insert and no damage** (`:139-140`). This is where wait pruning lands
  (Decision 5) and where the fail-drop's interaction with liveness is decided
  (Decision 4).
- **`src/compositor/tile_planning.cpp:226-232`** — the fresh-probe hit gate:
  `hit->get().meta.exact && hit->get().meta.achieved_scale == rung_px`. An
  inexact tile fails it and sets `tile.is_miss = true`. **This is the
  amplification's proximate cause and it stays exactly as it is.**
- **`src/compositor/tile_planning.cpp:501-511`** — the driver's dispatch site
  after `in_flight_tile_dedup`: `const bool in_flight = tile.is_miss &&
  tile_in_flight(pending, tile.key);` → `note_request_suppressed()` → `if
  (tile.is_miss && !in_flight)` renders. The wave gate is a third term in that
  same condition.
- **`src/compositor/tile_planning.cpp:612-616`** (inline-settle insert) and
  **`:618-631`** (async record, `pending->tiles.push_back(PendingTile{...})`) —
  where a `RenderResult` becomes a cache entry or a pending tile. The
  wait-recording hook sits immediately after the render returns, where
  `result.exact` is in hand.
- **`src/compositor/pull_service.cpp:214-255`** — the per-covering-tile loop:
  cache probe (`:224-229`), then the in-flight join (`:249-255`, which flips
  `any_async` and delivers nothing). The wave gate goes here as a fourth arm —
  and unlike the join, it **delivers** (Decision 3).
- **`src/compositor/pull_service.cpp:348-365`** (async record) and **`:390-400`**
  (aggregate settle: `region_exact`, `any_async` → the caller's `done` left
  unsettled). These two sites, plus the join at `:249-255`, are the three places
  a pull leaves an input tile *unmet* — i.e. the three places the accumulator of
  Decision 2 appends.
- **`src/cache/arbc/cache/key_shapes.hpp:105-115`** — `TileMeta{achieved_scale,
  exact}`, `bool exact{true}` at `:114`. Value metadata, **not part of the
  key** — which is why the transient tile is *resident and findable* under the
  exact key even though it is not a hit. That is what makes Decision 3 possible
  with no cache change.
- **`src/compositor/arbc/compositor/counters.hpp:34-103`** —
  `CompositorCounters`: `requests_issued()` `:40`, `operator_renders()` `:54`,
  `degraded_composites()` `:65`, `requests_suppressed()` `:85`, `note_*` at
  `:87-93`. `CompositorStats` at `:110-121` with `requests_suppressed` appended
  **last** (`:118-120`) so positional aggregate inits keep meaning — the new
  counter appends after it, same rule.
- **`src/runtime/interactive.cpp:299-304`** — the frame-local `PullServiceImpl`
  is built with `config.pending = &d_pending` and `config.counters = &d_counters`
  — the same queue the driver plans against. This is why one wave state, held in
  the queue, is visible to both gate sites with **no new plumbing**.
- **`src/runtime/interactive.cpp:369-391`** — the park loop, and the deadline
  sweep that `cancel()`s every unsettled tile on expiry (`:387`). The reason the
  wave gate cannot use `tile_in_flight` (Decision 4).
- **`src/runtime/interactive.cpp:397-414`** — `poll_refinements` →
  `route_arrival_damage` → `schedule_follow_up = !arrival_device.empty()`.
  Unchanged.
- **`tests/interactive_worker_default.t.cpp:575-584`** (`build_operator_heavy`:
  fade@0.5 over `under`; crossfade@0.5 over `from`/`to` — **2 operators,
  3 leaves**) and **`:586-598`** (`build_nested_deep`: two solids → two
  `FadeContent`@0.5 → a child composition → one `NestedContent` layer —
  **3 operators, 2 leaves**). Both are 5 renders inline. These two shapes are
  what Decision 1's identity is derived against, and they are why the identity
  is checkable.
- **`tests/interactive_worker_default.t.cpp:704-722`** — the assertions:
  byte-identity (`:704`) and `max_in_flight_per_content() <= 1` (`:705`) hold at
  every worker count; the flat scene gets `==` (`:709-710`); the operator scenes
  get `>=` (`:714`) plus `requests_suppressed() == 0` (`:721`). **`:714` is the
  line this task exists to tighten** — the WBS note's `:697` citation has
  drifted; `:697` is now the sweep-loop head.

## Constraints / requirements

1. **Quiesced pixels are byte-identical.** Every golden must pass **unchanged,
   with no re-baselining**: `tests/refinement_golden.t.cpp`,
   `tests/refine_idempotence_golden.t.cpp`, and the `byte_identical` sweep at
   `tests/interactive_worker_default.t.cpp:704`. All of these compare at
   *quiescence*, and at quiescence the gate has opened for every tile by
   construction (Decision 4's liveness argument). If a golden moves, the gate is
   stranding a tile.
2. **A deferred tile composites the transient tile it already has** — not the
   transparent/checkerboard fallback, and not a stale-revision tile. The
   resident inexact entry under the tile's *exact* key is the correct fallback
   and it is already in the cache. Skipping the render must not blank the layer
   for a frame (Decision 3). This is what keeps the *intermediate* frames
   pixel-stable too, and it is a strictly better fallback than doc 02:62-67's
   current preference order offers.
3. **Never strand an operator tile.** The gate must open whenever every unmet
   input has left the pending queue — by settling, by failing, or by being
   dropped — and it must open on the very next plan of that tile. A gate that
   can stay shut after its wave is over is a permanent placeholder, which is
   strictly worse than the waste it replaces (Decision 4).
4. **Null-tolerant.** `operator_wave_pending(nullptr, key)` is `false`, and a
   null `pulls` service records no waits. The offline driver's exact first pass
   plans with `pending == nullptr` (`src/runtime/offline_sequence.cpp:136`) and
   must stay byte-identical, per the house rule that every optional compositor
   parameter has a behavior-identical null path.
5. **No new thread-safety surface.** The wave state lives in the
   frame-thread-only `RefinementQueue`; the unmet accumulator lives in the
   frame-local `PullServiceImpl`, which by `worker_dispatch_leaf_only`
   (doc 02:220-233) is only ever re-entered on the driver thread — operators
   render inline, never on a worker. The predicate reads `done->settled()`,
   the same cross-thread atomic read the driver's park loop already makes
   (`interactive.cpp:369-372`). No lock, no new atomic, no new shared state, and
   nothing a worker writes.
6. **Levelization (doc 17:42-62).** Everything lands in `arbc::compositor` (L4):
   `refinement.hpp/.cpp`, `tile_planning.cpp`, `pull_service.hpp/.cpp`,
   `counters.hpp`. It consults `cache`'s `TileKey` (L3) and `contract`'s
   `Completion` (L3), both already direct edges. **No new component edge, no new
   third-party dependency** (doc 10). Per doc 17:118-128 the persistent wave
   value is held by `runtime` inside the caller-owned `RefinementQueue` — the
   compositor stays a stateless per-frame library.
7. **The transient placeholder stays inexact.**
   `13-effects-as-operators#transient-placeholder-is-never-exact`
   (`registry.tsv:197`) must keep passing unchanged, and
   `tests/operator_async_placeholder_inexact.t.cpp`'s seven cases must stay
   green. The gate is *not* implemented by making the tile a hit.
8. **Counters stay honest.** A deferred render bumps neither `requests_issued`
   nor `operator_renders` — that is the entire observable point. It bumps the new
   `renders_coalesced` instead (Decision 6), so the coalescing is provable by
   *presence*, not only by absence. `requests_suppressed` keeps its existing
   meaning and must remain `0` on these scenes.
9. **The wave is a set, not a duration.** No timers, no wall-clock, no frame
   counters in the gate. Its only input is which `TileKey`s are still in
   `queue.tiles` unsettled.

## Acceptance criteria

### The headline assertion — `tests/interactive_worker_default.t.cpp`

Replace the `>=` at `:714` with the **coalescing identity**, for the operator
scenes (`operator_heavy`, `nested_deep`), across the full `{0, 1, 2, 4, hw-1}`
worker sweep:

```
oracle_operator_renders = oracle.renderer().counters().operator_renders();

workers == 0:  requests_issued() == oracle_requests
               operator_renders() == oracle_operator_renders
               renders_coalesced() == 0          // nothing is ever in flight inline

workers >= 1:  requests_issued()  == oracle_requests + oracle_operator_renders
               operator_renders() == 2 * oracle_operator_renders
               renders_coalesced() >  0          // the gate actually fired
               requests_suppressed() == 0        // and it is still not dedup
```

**Every leaf renders exactly once; every operator renders exactly twice** — once
to *request* its inputs and paint the placeholder, once when the wave lands.
That is what "at most one chain re-render per wave" means when the wave is
singular, which on a cold-cache scene it is (Decision 1).

The identity is checkable against the two shipped scenes, and one of them
already validates it: `operator_heavy` has 2 operators and 3 leaves
(`:575-584`), so it predicts `5 + 2 = 7` — **the measured threaded value today**
(`:662`). It is therefore already at the coalesced floor and this assertion is a
*regression guard* for it. `nested_deep` has 3 operators and 2 leaves
(`:586-598`), so it predicts `5 + 3 = 8` against **12 measured** — that scene is
the one this task fixes, and 12 → 8 is the deliverable.

The 45-line comment at `:640-678` explaining why `==` is false is replaced by
the derivation of why *this* identity is true — including the part the WBS note
gets wrong (Decision 1).

### Claims-register entries

(`tests/claims/registry.tsv`, each with an `// enforces:` tagged test; each
requires the design-doc delta of Decision 8, per the standing house rule from
`interactive_pull_wiring` Decision 5 — *do not mint a claim id for a sentence no
design doc contains*.)

- **`02-architecture#refinement-wave-coalesces-chain-rerender`** — an operator
  tile whose recorded unmet inputs are still pending is not re-rendered by a
  partial arrival: no `content->render`, no pull, no surface allocation. The
  chain is re-driven at most once per wave.
- **`13-effects-as-operators#operator-defers-to-its-pending-inputs`** — a pull
  for an operator tile still waiting on its wave delivers the **resident
  transient tile** into the caller's target, dispatches nothing, and leaves the
  region inexact (TRANSIENT), so the parent degrades this pass exactly as it did
  before.
- **`02-architecture#coalescing-never-strands-an-operator-tile`** — when the last
  unmet input leaves the pending queue (settled, failed, or dropped), the gate
  opens on the next plan of that tile and the operator renders. Liveness: a wave
  that ends always releases what it gated.

### Unit tests

- **`src/compositor/t/refinement.t.cpp`** — `operator_wave_pending`, one case per
  arm: null queue → false; no wait recorded for the key → false; wait recorded,
  one unmet input still pending-and-unsettled → **true**; wait recorded, that
  input **cancelled but unsettled** → **still true** (Decision 4 — the whole
  point, and the arm that differs from `tile_in_flight`); wait recorded, input
  settled → false; wait recorded, input no longer in the queue at all (the
  fail-drop) → false; wait with two unmet inputs, one resolved → still true; both
  resolved → false. Plus: `poll_refinements` erases a wait whose unmet set is
  fully drained (Decision 5), and a wait keyed on a superseded revision is
  unmatchable and pruned.
- **`src/compositor/t/pull_service.t.cpp`** — the deferred pull. Render an
  operator tile whose input goes async (worker-backed), so a wait is recorded;
  pull that same operator tile again in the next frame while the input is still
  pending → `operator_renders()` unchanged, `renders_coalesced() == 1`, the
  caller's `done` **unsettled** (TRANSIENT, not a placeholder-settled FINAL),
  the region marked inexact, and **the caller's target holds the transient
  tile's pixels** (byte-compare against the resident cache entry — Constraint 2).
  Then settle the input, `poll_refinements`, pull again → the gate is open, the
  operator renders once, the tile is exact. Enforces
  `#operator-defers-to-its-pending-inputs`.
- **`src/compositor/t/counters.t.cpp`** — a driver scene with a nested chain over
  two async leaves: across the whole quiesced run, `operator_renders()` is
  exactly twice the chain depth and `renders_coalesced()` is positive (the test
  fails if the gate silently never fires — the failure mode `in_flight_tile_dedup`
  was bitten by). Enforces `#refinement-wave-coalesces-chain-rerender`.
- **`src/compositor/t/refinement.t.cpp`** (liveness) — record a wait on an input
  that then settles via `fail`; `poll_refinements` drops it with no insert and no
  damage (`refinement.cpp:139-140`); the very next plan of the operator tile
  **renders** it (gate open) rather than deferring forever. Enforces
  `#coalescing-never-strands-an-operator-tile`.

### Goldens (byte-exact — doc 16:48-53)

`tests/refinement_golden.t.cpp` and `tests/refine_idempotence_golden.t.cpp` pass
**byte-unchanged, no re-baselining**. If either moves, Constraint 2 is violated
(the deferred tile is compositing the wrong fallback) or Constraint 3 is (a tile
is stranded). A moved golden is a bug signal here, not a baseline update.

### Behavioral-counter framing, never wall-clock (doc 16:54-62, :224-226)

The promise is "one chain re-render per wave", which is a counter identity. The
`nested_deep` millisecond figure (3027 ms inline vs 6247 ms at 2 workers) is a
benchmark to **trend** — the expected outcome is that the threaded number falls
below the inline one, and that is a chart, not a gate. No test asserts a
millisecond figure.

### Concurrency coverage

`tests/refine_idempotence_stress.t.cpp` (60 iters × `{1,2,4}` workers,
`[.nightly]`, TSan-full lane) must stay green unchanged, and gains one
assertion: across the whole quiesced multi-worker run, `operator_renders()`
equals the single-worker oracle's — the coalescing holds under real thread
interleaving and real arrival races, not just in the unit harness. The gate's
cross-thread read (`done->settled()`) is exercised there against a live worker
pool, which is where TSan would catch a mis-ordered read. `tests/
interactive_worker_default.t.cpp` runs in the standard suite and is therefore on
the per-push `gcc-tsan` lane: a counter bumped from a worker fails there.

### Coverage

≥ 90 % diff coverage on changed lines (CI gate). Each arm of the predicate, the
accumulator, the fallback source, and the pruning have a test above.

### Deferred follow-ups

**None.** Everything this task surfaces is either decided here or already
registered: the cross-frame teardown of the pending set is
`runtime.deadline_cancel_retains_wanted` (M9, 2d, registered by
`in_flight_tile_dedup`), and this task is deliberately independent of it
(Decision 4). Partial recomposition is rejected outright rather than deferred
(Decision 7) — it needs a `Content`-contract entry point that does not exist and
should not.

## Decisions

### 1. The inline oracle is the wrong right-hand side — the identity is `oracle + oracle_operator_renders`

The WBS note asks this task to restore "*the counter identity the headline
equality assertion ... was intended to enforce*", i.e.
`requests_issued() == oracle_requests`. **That equality is unachievable, and no
coalescing scheme can achieve it.** Stating this plainly is the first
deliverable, because two prior tasks have now been chartered against it.

The reason is causal, not incidental: **the operator's first render is how its
inputs get requested at all.** The driver does not know an operator's input
tiles; it discovers them by rendering the operator and watching it pull
(`13:69-83` — pulling inputs goes through the core). So on a cold cache with a
worker pool, an operator *must* render once to dispatch its inputs, and that
render necessarily produces a placeholder, because the inputs it just dispatched
have not landed. It must then render a second time, once they have, to compose
the real pixels. Two renders per operator is the **floor**, not the waste.

At `worker_count == 0` the floor is one, because `submit` *is* the render: the
dispatched leaf settles inline into the cache before the pull returns, so the
operator's first and only render is already exact. The inline oracle measures a
regime that structurally cannot have a placeholder pass. Comparing a threaded
run against it and demanding equality is asking the threaded run to be
synchronous.

So the honest identity, derived from the mechanism:

> every **leaf** renders exactly once (the cache and the in-flight guard between
> them ensure that), and every **operator** renders exactly twice — once to
> request and placeholder, once to compose.

`requests_issued() == oracle_requests + oracle_operator_renders`, and
`operator_renders() == 2 * oracle_operator_renders`.

This is not a rationalization fitted after the fact. It is *falsifiable*, and it
is *already confirmed* on one of the two shipped scenes: `operator_heavy` has
exactly 2 operators, so the identity predicts 7, and 7 is what the threaded run
measures today (`:662`). The formula predicting an unrelated scene's measured
number, on the nose, is the evidence that it is the real floor.
`nested_deep` has 3 operators, so it predicts 8 against 12 measured — and the
gap of 4 is precisely the per-arrival re-drives of a 3-deep chain that this task
coalesces.

**Alternative rejected:** *skip the operator's first render entirely — plan its
input tiles without rendering it, dispatch them, and render the chain once when
they land.* This would genuinely reach `== oracle_requests`, and it is the only
thing that could. It is rejected because **it deadlocks**: the input tiles are
not knowable without the pull, and the pull does not happen without the render.
Making them knowable means a new `Content` entry point that enumerates a render's
input tiles *before* rendering — an L2, plugin-ABI-visible addition, imposed on
every operator author, to save one placeholder pass that the user sees as the
first paint of the scene. The first paint is a feature; doc 02:71-73 sells it
("*progressively sharper content rather than blocking*"). Not worth it, and not
this task.

### 2. The unmet set is recorded by the pull service, drained by the render site, and transitive through the chain

The gate needs to know, for an operator's transient output tile, *which input
tiles it is waiting on*. Only the pull service knows: they are exactly the tiles
it left unmet during that render.

`PullServiceImpl` grows a frame-scoped accumulator, `std::vector<TileKey>
d_unmet`, appended at the **three** places a pull fails to deliver a covering
tile: the async dispatch record (`pull_service.cpp:348-365`), the in-flight join
(`:249-255`), and the new wave-deferral arm (Decision 3). It exposes two calls —
`unmet_mark()` (the current size) and `unmet_since(mark)` (a copy of the tail) —
and the render site brackets `content->render` with them:

```
const std::size_t mark = pulls ? pulls->unmet_mark() : 0;
const RenderResult result = content->render(request, target, done);
if (pending && pulls && !result.exact) {
  std::vector<TileKey> unmet = pulls->unmet_since(mark);
  if (!unmet.empty()) { record_wait(*pending, tile.key, std::move(unmet)); }
}
```

`unmet_since` **copies without erasing**, and that is deliberate: it is what
makes the set *transitive*. When `NestedContent` renders, it pulls its child
fades; each fade renders inline (worker dispatch is leaf-only, doc 02:220-233)
and pulls its leaf, which goes async and appends. The fade's own wait, recorded
by the pull site's render, gets the tail since *its* mark — just its leaf. The
nested tile's wait, recorded by the driver, gets the tail since *its* mark — the
union of the whole subtree. Each level waits on exactly the leaves beneath it,
and the top waits on all of them. That is "one chain re-render per wave" falling
out of the recording, with no graph walk and no `inputs()` traversal.

The accumulator is cleared at the top of each driver-driven output tile, so it
never grows across the frame's tiles.

**Alternative rejected:** *derive the unmet set from `inputs()` and
`map_input_damage` at plan time, without recording anything.* The compositor
already has operator-graph awareness (`compositor.operator_graph`), so it can
walk `inputs()` and, for each input content, ask which of *its* tiles cover the
operator's output tile. Rejected because the mapping from an operator's output
tile to its input **tiles** is not `inputs()`' to give — it depends on the
operator's internal geometry (a nested composition's `time_map` and transform, a
fade's identity short-circuit, a crossfade's per-input rung selection), which is
exactly the knowledge the `PullService` seam exists to keep *inside* the
operator. Reconstructing it in the compositor means duplicating every operator's
sampling logic in L4 and keeping the duplicate correct forever. The pull service
already observes the ground truth for free; observe it.

### 3. A deferred tile composites the **resident transient tile**, and that becomes the first degraded fallback

Skipping the re-render is only sound if the frame still paints the right pixels.
The tile is `is_miss` (the hit gate rejects it for `!meta.exact`), so today's
degraded-fallback order — stale-revision, then coarser, then
checkerboard/transparent (doc 02:62-67) — would step *over* the perfectly good
transient tile sitting resident under the tile's own exact key and paint a stale
or transparent one. The operator would blink.

So the fallback order gains a new first entry: **a resident, current-revision,
current-rung, inexact tile**. It is strictly better than everything below it
(same revision, same rung, right geometry — it is simply not *final*), it costs
one `cache.lookup` on a path that is already doing one, and it needs **no cache
change at all**, because `TileMeta` is *value* metadata and not part of the key
(`key_shapes.hpp:105-115`) — the entry is findable, it is just not a hit.

On the pull side (`pull_service.cpp:214-255`) the same rule means the deferred
arm **delivers**: `deliver_tile(...)` from the resident entry into
`request.target`, then `region_exact = false; any_async = true; continue;`. This
is the one place the wave gate differs from the in-flight join at `:249-255`,
which delivers nothing — and it must, because the parent operator is going to
composite this tile *now*, and an undelivered target is a transparent hole in
the parent's placeholder.

The payoff is that intermediate frames are pixel-stable too, not merely quiesced
ones: the frame that defers paints exactly what the previous frame painted for
that tile. Constraint 1's "no golden moves" is a consequence of this decision,
not an independent hope.

**Alternative rejected:** *let the deferred tile fall through to the existing
stale/coarser/transparent order.* It is zero new code and it is wrong: on a cold
cache there *is* no stale tile and no coarser tile, so the operator paints
transparent for every frame of the wave and then pops in. That trades a
performance bug for a visual one — and it would move the goldens, which is how
we would find out.

### 4. The wave gate treats a **cancelled-but-unsettled** input as still pending — the opposite of `tile_in_flight`

Two predicates over the same queue, differing in one clause, and the difference
is deliberate. It needs to be stated precisely because getting it backwards
breaks the task in one direction and strands tiles in the other.

- **`tile_in_flight`** (the *dispatch* gate, `refinement.cpp:82`) —
  `!settled() && !cancelled()`. It **excludes** cancelled entries because
  suppressing a *dispatch* against one risks a permanent hole: `cancel()` is
  advisory (`content.hpp:161-163`), a conformant content may honor it and settle
  via `fail`, and `poll_refinements` drops a failed arrival with **no cache
  insert and no damage** (`refinement.cpp:139-140`). Suppress against it and the
  tile is in neither the cache nor the queue, with no damage ever emitted —
  nothing re-plans it. That is `in_flight_tile_dedup`'s Decision 3, and it
  stands.
- **`operator_wave_pending`** (the *re-render* gate, new) — an unmet key that is
  still in `queue.tiles` and `!settled()`, **regardless of `cancelled()`**.

The asymmetry is sound because the two gates fail differently.

Suppressing a *dispatch* against a cancelled tile means nobody is rendering it
and nobody will notice. Deferring a *re-render* against a cancelled tile means
we decline to re-render an operator whose tile is **already resident** and
**already composited** (Decision 3) — the user sees the placeholder, which is
what they were going to see anyway. And it cannot last, because a cancelled
entry has only two futures, and both open the gate:

- **it completes anyway** (the common case — most content ignores the advisory
  flag): `poll_refinements` inserts it, emits damage, the router broadcasts to
  every consumer, the operator is re-planned, the gate sees a settled input and
  opens. One chain re-render, on the wave, exactly as designed.
- **it honors the cancel and fails**: `poll_refinements` drops it — it leaves
  `queue.tiles`, so `operator_wave_pending` stops matching it and the gate opens
  on the next plan of that tile. No damage was emitted, so there may be no next
  plan — but that is precisely today's behavior for a failed leaf, unchanged:
  the operator keeps its placeholder until something else damages the region.
  The gate adds no new stall; it inherits the existing one.

If the wave gate used `tile_in_flight` instead, the deadline sweep
(`interactive.cpp:387` — cancel *every* unsettled tile on expiry) would open
every gate at every frame boundary, and the coalescing would evaporate in
exactly the regime it exists for: a scene under deadline pressure, whose arrivals
land across several frames. The whole task would pass its unit tests and do
nothing in production. This clause is what makes it work, and it is what makes
this task **independent of `runtime.deadline_cancel_retains_wanted`** rather
than blocked on it.

**Alternative rejected:** *block on `runtime.deadline_cancel_retains_wanted`,
land it first, then use `tile_in_flight` for both gates.* Cleaner — one
predicate, one meaning. Rejected because it buys uniformity at the cost of an
extra dependency edge and, more importantly, of *correctness under a
still-cancelling deadline*: even with the sweep narrowed to "no longer wanted"
tiles, a tile genuinely dropped from the viewport can still be cancelled while an
operator's recorded wait names it, and the wave gate would open early again. The
two predicates answer two different questions — "is someone rendering this?" and
"is more coming for this?" — and a cancelled render answers *yes* to the second
even when it answers *no* to the first.

### 5. The wave state lives in `RefinementQueue`, as a second vector, pruned by `poll_refinements`

```cpp
struct OperatorWait {
  TileKey              output;  // the transient tile whose render left inputs unmet
  std::vector<TileKey> unmet;   // the input tiles it is waiting on
};

struct RefinementQueue {
  std::vector<PendingTile>  tiles;
  std::vector<OperatorWait> waits;   // NEW
};

bool operator_wave_pending(const RefinementQueue* queue, const TileKey& output) noexcept;
```

Doc 17:118-128 settles *where* this can live and leaves no choice: the compositor
is a pure per-frame library and "*does not hold persistent counter state — [it
takes] a caller-owned struct by pointer, the same way [it] already [takes] the
`SurfacePool` and `RefinementQueue`*". A wave spans frames, so it is persistent
state, so it belongs to `runtime` — and `RefinementQueue` is the caller-owned
struct `runtime` already holds and already threads to **both** gate sites
(`interactive.cpp:299-304`, `:346-348`). Putting the waits beside the tiles they
name costs zero new plumbing and zero new levelization edges.

`poll_refinements` prunes: after the retain/insert pass, erase every wait whose
`unmet` set contains no key still present in `queue.tiles`. One `erase_if`. It
handles every expiry route at once — settled inputs, failed-and-dropped inputs,
and stale-revision waits (a revision bump makes the wait's `output` key
unmatchable, and its inputs drain out of the queue like any others). The vector
is bounded by the number of distinct operator output tiles with work in flight —
tens, in the same order as `tiles` itself.

The predicate is a linear scan, for the same reason `tile_in_flight` is
(`in_flight_tile_dedup` Decision 1, restated here because it is a *correctness*
argument and not a performance one): membership is not a function of the queue's
mutations. It depends on `done->settled()`, an atomic a **renderer thread** flips
with no notification to the queue, so any precomputed index would be stale the
instant a worker settled and would have to be re-validated against the
completions on every query — which is the scan, plus an invariant to get wrong.
It is paid only on the miss path; a warm frame never calls it.

### 6. A new counter, `renders_coalesced`, proves the gate positively

`CompositorCounters` gains `renders_coalesced()` / `note_render_coalesced()`,
bumped once per chain re-render the gate defers. It appends **last** in
`CompositorStats` (after `requests_suppressed`, `counters.hpp:118-120`), keeping
positional aggregate inits meaningful.

This is the lesson of `in_flight_tile_dedup`, learned the hard way. That task
argued (its Decision 5) that a dedup provable only by a number *not growing* is
indistinguishable from a dedup that never fires — and then its `requests_issued`
assertion passed vacuously while `requests_suppressed() == 0` revealed the guard
had never fired on either benchmark scene. Without `renders_coalesced`, every
assertion here is again "a number did not grow", and a refactor that quietly
disconnects the gate passes silently. With it, `renders_coalesced() > 0` at every
non-zero worker count is a *positive witness* that the gate is on the live path,
and `== 0` at `worker_count == 0` is a positive witness that it does not fire
where nothing is in flight.

**Alternative rejected:** *reuse `requests_suppressed`.* Both count "a render we
declined to drive", so one counter looks like enough. Rejected because it would
destroy the one assertion that made the previous task's failure *legible*:
`requests_suppressed() == 0` on these scenes is what proved the residual was the
wave and not duplicate dispatch. Merging the two would have hidden that, and it
would hide the next such surprise. The counters are disjoint because the
mechanisms are.

### 7. Partial recomposition is rejected, not deferred

Coalescing has one real cost, and it should be named: an operator whose output
tile waits on two inputs, one fast and one slow, used to composite the fast one
against a placeholder as soon as it arrived, and now shows the all-placeholder
tile until both land. One intermediate refinement step, per output tile, is lost.

The obvious fix — let the operator composite the newly-arrived input into its
resident transient tile — is rejected outright rather than registered as a
follow-up, because it is not implementable within the contract and should not be
made so. It needs a `Content` entry point meaning "re-compose with input *k*
now available, reusing your previous output", which is (a) an L2, plugin-ABI
addition imposed on every operator author, (b) a stateful obligation on content
the contract deliberately keeps stateless, and (c) a correctness minefield for
any operator whose composition is not incremental (a crossfade's per-input rung
selection, a nested composition's whole child frame). Doc 13's cost list
(`13:252-261`) is an honest accounting of what operators are allowed to cost, and
this is not on it.

The cost is also smaller than it first looks. The granularity that matters to the
user is the *output tile*: each output tile carries its own wait set keyed on its
own `TileKey`, so tile T₁ refines the instant *its* inputs land, independently of
T₂. A large operator region still sharpens progressively, tile by tile. What is
lost is sub-tile progressiveness *within* one output tile whose inputs land at
different times — where the operator was already painting a full placeholder
composite anyway. Trading that for a 3-deep chain not re-rendering N times is not
a close call.

### 8. Design-doc delta (rides in the closer's commit, doc 16's same-commit rule)

Doc 13 currently *authorizes* the behavior this task removes, in so many words,
so the claims of the Acceptance criteria cannot be minted until the docs say
otherwise.

- **`docs/design/13-effects-as-operators.md:118-120`** — amend "*and **each async
  tile's arrival re-drives it***" to the coalesced rule: the operator is re-driven
  when the **last** of the input tiles it is waiting on arrives; a partial arrival
  does not re-drive it, and a pull for an operator tile still waiting on its wave
  delivers the resident transient tile, dispatches nothing, and stays TRANSIENT.
  This *narrows* the promise; it does not contradict `pull-is-cache-first`
  (`13:74-82`) or the TRANSIENT/FINAL split (`13:148-157`), both of which are
  preserved verbatim.
- **`docs/design/02-architecture.md`** § The frame, interactively — **define the
  refinement wave**. The term is already used, twice, undefined (`02:145-147`,
  `00-overview.md:245-246`); this is where it acquires a boundary: *the set of
  input tiles an operator's transient output tile is waiting on; the wave ends
  when the last of them leaves the pending queue*. State the coalescing rule
  after the in-flight-dedup paragraph (`02:124-147`), where its sibling already
  lives, including the cancelled-input asymmetry (Decision 4) and why deferring a
  re-render cannot strand a tile.
- **`docs/design/02-architecture.md:62-67`** — extend the degraded-fallback
  preference order with its new first entry: a resident, current-revision,
  current-rung **inexact** tile, ahead of stale-revision (Decision 3).
- **`docs/design/00-overview.md`** § Resolved questions — a decision-record
  bullet in the `:211-234` style: *an operator chain renders at most twice on a
  cold cache — once to request its inputs and paint the placeholder, once when
  the wave lands.* This is a project-shaping economy rule (it is what makes doc
  05's "only the spine" and "the recursive case must cost what an equivalent flat
  scene would" true in the async case), and it is the sentence that finally
  bounds a cost doc 02:112-122 explicitly declined to bound.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-12.

- Added `OperatorWait` struct and `waits` vector to `RefinementQueue` in `src/compositor/arbc/compositor/refinement.hpp`; new `operator_wave_pending()` predicate gates re-renders while unmet inputs remain in the pending queue.
- Wave state recorded by `PullServiceImpl` via bracketed `unmet_mark()`/`unmet_since()` calls around `content->render` in `src/compositor/tile_planning.cpp` and `src/compositor/pull_service.cpp`; deferred arm in `pull_service.cpp` delivers the resident transient tile (Decision 3) instead of blanking.
- `poll_refinements` in `src/compositor/refinement.cpp` prunes waits whose unmet sets are fully drained; handles settled, failed-and-dropped, and stale-revision entries.
- New `renders_coalesced` counter added to `src/compositor/arbc/compositor/counters.hpp` and `CompositorStats`; bumped at both dispatch sites when the wave gate fires.
- Design docs updated: `docs/design/02-architecture.md` defines refinement wave and its coalescing rule (Decision 8); `docs/design/13-effects-as-operators.md` amends per-arrival re-drive text; `docs/design/00-overview.md` adds resolved-question bullet bounding the two-render-per-operator cost.
- Unit tests added: `operator_wave_pending` (8 arms) + poll-prune/liveness in `src/compositor/t/refinement.t.cpp`; deferred-pull delivery in `src/compositor/t/pull_service.t.cpp`; nested-chain quiesced identity in `src/compositor/t/counters.t.cpp`; `tile_planning` inexact-tile source in `src/compositor/t/tile_planning.t.cpp`.
- Counter tests added: `tests/interactive_worker_default.t.cpp` (`nested_deep` 12→8 requests, 8→6 operator renders, coalescing identity enforced); `tests/refine_idempotence_stress.t.cpp` gains `operator_renders()` cross-worker oracle assertion.
- Claims registered in `tests/claims/registry.tsv`: `02-architecture#refinement-wave-coalesces-chain-rerender`, `02-architecture#coalescing-never-strands-an-operator-tile`, `13-effects-as-operators#operator-defers-to-its-pending-inputs`.
