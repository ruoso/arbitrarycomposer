# runtime.interactive_pull_wiring — Wire PullService into the interactive frame path

## TaskJuggler entry

[`tasks/65-runtime.tji:118-123`](../../65-runtime.tji) — `task interactive_pull_wiring`,
in milestone `m9_release` ([`tasks/99-milestones.tji:72`](../../99-milestones.tji)).

> Pass a `PullServiceImpl` with a production `PullConfig::id_of` into
> `render_frame_interactive` (interactive.cpp passes a null `pulls` today), so
> interactive operator layers get cache-identity-correct input pulls and the
> identity-delivery endpoint fix (the w-at-0/w-at-1 case) fires on the
> interactive path instead of rendering blank. Source:
> `tasks/parking-lot.md` 2026-07-10 (interactive frame endpoint delivery gap).
> Docs 02/13.

## Effort estimate

**1d.** The `PullServiceImpl` construction itself is ~15 lines that mirror
`offline_sequence.cpp:80-138` verbatim. The bulk of the day is the arrival-damage
routing (Constraint 6 / Decision 4) that wiring `PullConfig::pending` forces, plus
the interactive-driver operator tests, which do not exist today in any form.

## Inherited dependencies

**Settled:**

- `runtime.operator_identity_offline_delivery` (Done 2026-07-10) — landed the
  identity-delivery branch in the frame driver
  ([`src/compositor/tile_planning.cpp:472-516`](../../../src/compositor/tile_planning.cpp)),
  **gated on `pulls != nullptr`**, and the claim
  `13-effects-as-operators#identity-layer-delivers-input-to-frame`
  ([`tests/claims/registry.tsv:178`](../../../tests/claims/registry.tsv)). Its
  Decision 3 (never `cache.insert` under the operator's `tile.key`) and Decision 4
  (an unsettled `done` keeps the planned fallback; reap and re-composite) are the
  two rules this task must honor on the interactive path.
- `runtime.interactive` (Done) — the six-step deadline-bounded frame loop
  ([`src/runtime/interactive.cpp`](../../../src/runtime/interactive.cpp)), the
  `RefinementQueue`/`CompositorCounters` frame-to-frame state, and the
  still-scene early-out (`interactive.cpp:90-94`).
- `runtime.operator_input_cache_identity` (Done) — `make_pull_identity_of` /
  `build_pull_identity_map`
  ([`src/runtime/arbc/runtime/pull_identity.hpp:33-42`](../../../src/runtime/arbc/runtime/pull_identity.hpp)),
  whose header note already declares this task the intended second consumer:
  *"a future interactive-audio `id_of` wiring reuses it verbatim."*
- `compositor.operator_graph` (Done) — `is_operator`, `resolve_identity`, and
  `route_operator_damage`
  ([`src/compositor/arbc/compositor/operator_graph.hpp:114-134`](../../../src/compositor/arbc/compositor/operator_graph.hpp)).
  `route_operator_damage` has **no production caller today**; this task is its
  first (Decision 4).
- `kinds.nested_runtime_binding` (Done) — the leaf-only worker-dispatch rule and
  the TSan-confirmed `TileCache`/`KeyedStore` race it exists to prevent
  ([`src/runtime/offline_sequence.cpp:142-170`](../../../src/runtime/offline_sequence.cpp)).

**Pending (deliberately downstream — this task must not pre-empt them):**

- `runtime.interactive_binder_wiring` (`tasks/65-runtime.tji:124-129`, depends on
  *this* task) — calls `bind_operators` per interactive frame. Until it lands, an
  interactive operator layer at a **non-endpoint** weight is still unattached
  (Constraint 5).
- `runtime.worker_dispatch_leaf_only` (`tasks/65-runtime.tji:130-135`, depends on
  *this* task) — hoists the leaf-only rule into a shared
  `worker_backed_dispatch(pool)` helper. This task therefore ships
  `direct_dispatch()` and leaves the worker-backed swap to it (Decision 3).
- `audio.interactive_pull_identity` — the audio arm of the same `id_of` gap.
  Out of scope here (Constraint 8).

## What this task is

`InteractiveRenderer::render_frame` calls `render_frame_interactive` with only
fourteen of its sixteen arguments
([`src/runtime/interactive.cpp:115-117`](../../../src/runtime/interactive.cpp)),
so `pulls` takes its `nullptr` default. Every operator behavior the frame driver
grew for `pulls` is therefore dead on the interactive path. This task builds a
per-frame `PullServiceImpl` inside `render_frame` — over the `TileCache&` and
`Backend&` the frame already receives, with a `PullConfig` whose `id_of` is
`make_pull_identity_of(state, resolve)` and whose `pending` is the loop's existing
`d_pending` — and passes it through. Wiring `pending` makes the driver capable of
an *async operator-input* arrival for the first time, whose damage names a content
that is not a layer root; so the task also routes that arrival damage up to the
operator layers that show it, through the compositor's already-built (and so far
uncalled) `route_operator_damage`.

Everything happens inside `InteractiveRenderer::render_frame`. **No public
signature changes, no `HostViewport` changes.** The cache, the backend, the pinned
`DocRoot`, and the `ContentResolver` are all already parameters
(`interactive.hpp:97-101`).

## Why it needs to be done

Three concrete failures on the interactive path today, all of which the offline
path has already fixed:

1. **Identity endpoints composite blank.** `tile_planning.cpp:472` reads
   `else if (identity_terminal != nullptr && pulls != nullptr)`. A crossfade at
   `w == 0`/`w == 1` (or a fade at `envelope == 1`) short-circuits, so
   `issue_render` is false (`tile_planning.cpp:386`) — and with `pulls == nullptr`
   *neither* arm runs. The tile keeps its planned `TileSource::Placeholder` and
   composites as a transparent no-op (`tile_planning.cpp:555-562`). The same
   document exports correctly through `SequenceRenderer`. That divergence is the
   parking-lot entry (2026-07-10) this task was minted from.
2. **Operator inputs alias on one cache key.** `PullServiceImpl::pull` derives the
   input's identity as `d_config.id_of ? d_config.id_of(input) : ObjectId{}`
   ([`src/compositor/pull_service.cpp:175`](../../../src/compositor/pull_service.cpp)).
   Any interactive pull built without a production `id_of` keys **every** input
   under the root `ObjectId{}` — so a crossfade's two same-stability inputs
   collapse onto one tile key. This is exactly the bug
   `runtime.operator_input_cache_identity` fixed offline; it must not be
   reintroduced by wiring a half-configured service.
3. **It blocks two successor tasks.** `runtime.interactive_binder_wiring` and
   `runtime.worker_dispatch_leaf_only` both `depends !interactive_pull_wiring`;
   neither has a `PullService` to bind against or a dispatch to swap until this
   lands.

Downstream, this is what makes doc 02:40-41's "two drivers over the same core"
true for operators: after this task the interactive and offline drivers agree on
input cache identity and on identity delivery, and after `interactive_binder_wiring`
they agree on operator rendering outright.

## Inputs / context

### Design docs (normative)

- **`docs/design/02-architecture.md`**
  - 02:40-41 — "**Renderers**: two drivers over the same core." The premise: the
    interactive driver is not allowed to have a *different* operator contract.
  - 02:62-66 — step 4, misses become deadline-bounded requests; the degraded
    fallback preference order.
  - **02:69-71 — step 6: "Async results that arrive later produce damage for
    their region, scheduling a follow-up frame."** This is the sentence
    Constraint 6 / Decision 4 exist to keep true once `PullConfig::pending` is
    wired.
  - 02:94-95 — damage invalidates by `(content id, region)`; revision bumps
    invalidate wholesale by making old keys unreachable.
  - 02:121-124 — the compositor plans on the render thread under a pinned version;
    planning never races edits and never takes a lock.
- **`docs/design/13-effects-as-operators.md`**
  - 13:58-62 — `identity()`: "the compositor serves the input's cached tiles
    directly — no render, no copy, no new cache entry."
  - 13:69-72 — "**Pulling inputs goes through the core.** Operators do not call
    `input->render()` directly… At attach, content receives a `PullService`."
  - 13:91-106 — the delivery contract: a pull writes into the caller's `target`;
    "A pull whose input answers *asynchronously* — any covering tile — **delivers
    nothing usable this pass**: the completion is left unsettled, the operator
    degrades for this frame, and **each async tile's arrival re-drives it**."
    The re-drive is what Decision 4 implements interactively.
  - 13:124-128 — `map_input_damage`'s **covering** contract:
    "over-approximation is sound…, **under-approximation is a correctness bug**."
  - 13:144-149 — "**input tiles cache under the input's identity (shared by every
    consumer), operator output under the operator's.** `identity()` short-circuits
    both levels."
- **`docs/design/17-internal-components.md`**
  - 17:46-61 — the levelization table. `PullService` *interface* in `contract`
    (L3); `PullServiceImpl` in `compositor` (L4); the two render drivers in
    `runtime` (L5), which may depend on everything below.
  - 17:110-112 — "**The two render drivers live in `runtime`, not the engines.**"
  - 17:114-127 — "pure per-frame libraries (the compositor) … take a caller-owned
    counters struct by pointer … the persistent value lives in `runtime`."
- **`docs/design/16-sdlc-and-quality.md`** — 16:14-18 (claims register), the test
  taxonomy (tier 3 byte-exact goldens, tier 4 behavioral counters —
  "**Wall-clock tests lie in CI; counters don't**"), 16:112-118 (≥90% diff-coverage
  hard gate).

### Source seams

| What | Where |
| --- | --- |
| The null-`pulls` call site (the whole bug) | `src/runtime/interactive.cpp:115-117` |
| The driver's `pulls` contract | `src/compositor/arbc/compositor/tile_planning.hpp:281-312` |
| Identity capture + the gated delivery branch | `src/compositor/tile_planning.cpp:381-397`, `:472-516` |
| The null path's inline `content->render` | `src/compositor/tile_planning.cpp:419-428` |
| The model to mirror (offline inline arm) | `src/runtime/offline_sequence.cpp:80-139` |
| `PullConfig` fields | `src/compositor/arbc/compositor/pull_service.hpp:107-142` |
| `id_of` consumption + `ObjectId{}` fallback | `src/compositor/pull_service.cpp:175`, `:413` |
| `direct_dispatch()` — "byte-for-byte identical to today" | `src/compositor/arbc/compositor/pull_service.hpp:44-52`; `PullServiceImpl::dispatch` at `:195-201`, impl `src/compositor/pull_service.cpp:67-69` |
| `pull` records an async miss under the **input's** id | `src/compositor/pull_service.cpp:321-336` |
| `poll_refinements` emits `Damage{pending.content, …}` | `src/compositor/refinement.cpp:110-120` |
| **`map_damage_to_device` matches damage against layer roots only** | `src/compositor/damage_planning.cpp:38-54` (`layer.content == d.object`) |
| `route_operator_damage` + `OperatorLayer` (no production caller yet) | `src/compositor/arbc/compositor/operator_graph.hpp:114-134` |
| `make_pull_identity_of` / `PullIdentityMap` | `src/runtime/arbc/runtime/pull_identity.hpp:20-42`, `src/runtime/pull_identity.cpp:12-72` |
| `std::hash<arbc::ObjectId>` (the inverse map is legal) | `src/base/arbc/base/ids.hpp:21` |
| The frame-to-frame state + read-only accessors seam | `src/runtime/arbc/runtime/interactive.hpp:110-118`, `:121-138` |
| The still-scene early-out (precedes all new work) | `src/runtime/interactive.cpp:90-94` |
| `FadeContent::identity` / `CrossfadeContent::identity` | `src/kind_fade/fade_content.cpp:57-59`; `src/kind_crossfade/crossfade_content.cpp:110-119` |
| The `assert(d_pull != nullptr)` an unattached operator's `render` trips | `src/kind_fade/fade_content.cpp:94`; `src/kind_crossfade/crossfade_content.cpp:172`; `src/kind_nested/nested_content.cpp:427-428` |

### Predecessor decisions this task inherits verbatim

- `operator_identity_offline_delivery` Decision 3 — the identity branch **never**
  `cache.insert`s under the operator's `tile.key`; the terminal's own tile is
  cached under the terminal's identity by `pull`, shared by every consumer.
- `operator_identity_offline_delivery` Decision 4 — an unsettled `done` on the
  identity pull leaves the planned fallback in place; the arrival re-drives the
  composite. Offline discharges this with a re-composite pass
  (`offline_sequence.cpp:196-200`); interactively the **follow-up frame is the
  re-composite pass** (Decision 4 below).
- `operator_input_cache_identity` Constraint 8 / Decision 2 — both render drivers
  call `make_pull_identity_of` so the seam lands in one place. This task is the
  third caller; it must not hand-roll an `id_of`.
- `nested_runtime_binding` — operators render inline on the driver thread;
  workers never touch the cache (`worker_pool.hpp:37-40`).

## Constraints / requirements

1. **Mirror the offline config, do not invent one.** The `PullConfig` built in
   `render_frame` sets exactly `counters = &d_counters`, `pending = &d_pending`,
   `id_of = make_pull_identity_of(state, resolve)`, and
   `contribution = [revision](const Content*) { return revision; }` with
   `revision = state.revision()` (already computed at `interactive.cpp:44`) —
   field-for-field `offline_sequence.cpp:94-101`, differing only in that
   `pending` is non-null (interactive is `BestEffort`; offline reaps to
   quiescence instead).

2. **The service is a frame-local, and must be.** `PullServiceImpl` holds
   `TileCache&` and `Backend&` (`pull_service.hpp:204-206`), and both arrive as
   *parameters* of `render_frame` (`interactive.hpp:97-101`) — so it cannot be a
   member. Construct it after the early-out (`interactive.cpp:90-94`) and before
   `render_frame_interactive`; it dies at frame end.

3. **Byte-neutrality on every non-operator path is a hard requirement.** A
   `PullServiceImpl` driven with `direct_dispatch()` is documented to be
   "byte-for-byte identical to the pre-task inline fill" (`pull_service.hpp:48-52`,
   `:195-201`): `dispatch` is a straight `d_dispatch(content, request, done)`
   (`pull_service.cpp:67-69`) and `direct_dispatch` calls `content->render` +
   folds the inline result through `done` — exactly `tile_planning.cpp:422-428`.
   Therefore **no existing golden's bytes and no existing counter expectation may
   change.** Any golden churn is a bug in this task, not a rebaseline (see
   Acceptance A5).

4. **The still-scene early-out stays free.** `interactive.cpp:90-94` returns before
   any new work. A frame that does no work must build no identity map, construct
   no `PullServiceImpl`, and issue no pulls — the claims
   `02-architecture#interactive-still-scene-schedules-no-frame` and
   `#idle-viewport-issues-no-frames` (registry:145, :146) must still hold, and the
   per-revision identity memo (Decision 2) must not grow on those frames.

5. **Do not call `bind_operators`.** That is `runtime.interactive_binder_wiring`,
   which depends on this task and needs the `Document&` + `DocStatePtr` that
   `render_frame`'s signature does not carry. Consequence, which the tests must
   respect: an interactive operator layer at a **non-endpoint** weight still has a
   null `d_pull` and its `render` trips
   `assert(d_pull != nullptr && … "rendered before attach")`
   (`fade_content.cpp:94`) — exactly as it does today via
   `tile_planning.cpp:423`, so this task neither fixes nor regresses it. The
   **endpoint** case needs no attach: `identity()` reads params only, `inputs()`
   is core-owned structure (doc 13:167-170), and the *driver* — not the operator —
   issues the pull. Tests must confine interactive operator coverage to identity
   endpoints over **leaf** inputs (an operator input would itself need attach).

6. **An operator-input arrival must schedule a follow-up frame (doc 02:69-71).**
   `pull` records an async miss as `PendingTile{key, region, id, …}` where `id` is
   the *input's* identity (`pull_service.cpp:321-336`), so `poll_refinements`
   emits `Damage{input_id, …}` (`refinement.cpp:114-115`). But
   `map_damage_to_device` only matches damage against **layer roots**
   (`damage_planning.cpp:39`: `layer.content == d.object`), and an operator's
   input child is not a layer root — its id is *synthesized*, disjoint from every
   model id by construction (`pull_identity.hpp:26-30`). Left unrouted, that
   arrival maps to zero device rects: `schedule_follow_up` is false, the refined
   tile sits in the cache, and no frame is ever scheduled to composite it. Wiring
   `PullConfig::pending` **creates** this damage class, so this task must route it
   (Decision 4). Routing model damage through operators is a *separate*,
   pre-existing gap — see the deferred task under Acceptance criteria.

7. **Levelization (doc 17:41-44, 46-61).** `runtime` (L5) may depend on
   `compositor` (L4), so including `arbc/compositor/pull_service.hpp` and
   `arbc/compositor/operator_graph.hpp` from `src/runtime/interactive.{hpp,cpp}`
   is legal on the existing edge — `offline_sequence.cpp` already includes both.
   **No new `DEPENDS` edge, and no widening of `src/runtime/CMakeLists.txt`'s
   dependency list.** `scripts/check_levels.py` must stay silent.

8. **Visual only.** `PullConfig::audio_dispatch` and `PullConfig::blocks` stay
   unset, and `pull_audio` is not reached from this path. Interactive audio's
   missing `id_of` is `audio.interactive_pull_identity`, already a WBS leaf in
   `m9_release`.

9. **No new concurrency surface.** With `direct_dispatch()` every render still runs
   inline on the frame thread; `d_pool` keeps its documented interactive role —
   the async-completion park/wake, not parallel miss dispatch
   (`interactive.hpp:47-50`). No worker touches the `TileCache` or the service's
   frame-thread-confined `d_depth` (`pull_service.hpp:208-213`). Doc 16's
   concurrency tier therefore adds no *new* scope here; the existing TSan lane
   must still pass unchanged (A9).

## Acceptance criteria

Tests live in `tests/` (cross-component: integration, claims, golden — doc
17:229-246) except the two pure-loop assertions, which extend
`src/runtime/t/interactive.t.cpp`.

**A1 — Interactive identity endpoint delivers the input's pixels (byte-exact
golden).** New `tests/interactive_operator_identity_golden.t.cpp`: a document with
one `org.arbc.crossfade` layer over two *distinct* solid leaves. Drive
`InteractiveRenderer::render_frame` with a budget large enough that every miss
settles inline, at `w == 0` and (fresh renderer/cache) at `w == 1`. Each frame must
be **byte-identical** to the `SequenceRenderer` frame of the same document at the
same time — the cross-driver equivalence doc 02:40-41 promises, and byte-exact
because the CPU backend is specified deterministic (doc 16 tier 3) and an
unpressured `BestEffort` frame degrades nowhere. Repeat for `org.arbc.fade` at
`envelope == 1`. Pre-task both frames are fully transparent.
`enforces: 13-effects-as-operators#identity-layer-delivers-input-to-frame`
(second enforcer on registry row 178 — no new row, see Decision 5).

**A2 — The endpoint issues zero operator renders and degrades nowhere
(behavioral counters, doc 16 tier 4).** Across the A1 endpoint frame:
`counters().operator_renders` delta `== 0` and `counters().degraded_composites`
delta `== 0`. Mirrors `tests/crossfade_identity_counter.t.cpp:67-71` on the
interactive driver.
`enforces: 13-effects-as-operators#identity-plan-issues-no-operator-render`.

**A3 — The two inputs do not alias one cache key (the `ObjectId{}` regression).**
Same crossfade over two distinct **Static** leaves, one renderer, one cache, one
revision: frame at `w == 0`, then frame at `w == 1`. Because the inputs are Static
their tile keys omit `achieved_time` and are clock-invariant, so the first frame's
cached input-0 tile is still resident for the second. Assert the second frame
matches **input 1**, and that the two frames differ. Under a missing/naive `id_of`
both inputs key under `ObjectId{}` at the same revision/rung/coord, the second
endpoint hits input 0's tile, and the two frames come out byte-identical — so this
test fails loudly if `PullConfig::id_of` is ever dropped or hand-rolled.
`enforces: 13-effects-as-operators#operator-input-children-have-distinct-cache-identity`.

**A4 — An async operator-input arrival schedules the follow-up frame that
re-drives the endpoint.** New `tests/interactive_operator_identity.t.cpp`: the same
crossfade at `w == 0` over an **async** leaf (a test content that defers, settling
on demand — the shape `tests/pull_service_async.t.cpp` and
`tests/async_external_load.t.cpp` already use). Assert, in order:
- Frame 1 returns `FrameOutcome::schedule_follow_up == true`, composites the
  placeholder, and bumps `operator_renders` by 0.
- Settle the deferred render; frame 2 (driven by the carried damage, no other
  input) re-plans the operator layer's footprint, `pull` hits the now-resident
  terminal tile, and the frame is **byte-identical to A1's synchronous frame**.

  This is the regression test for Constraint 6: **without** the arrival routing of
  Decision 4, frame 1 returns `schedule_follow_up == false` and frame 2's dirty
  region is empty, so the assertion fails on the first line.
`enforces: 02-architecture#async-arrival-emits-damage` (second enforcer, registry
row 127).

**A5 — Byte-neutrality of every non-operator path.** The landed interactive
goldens and counter tests must pass **with zero edits to golden bytes and zero
edits to expected counter values**:
`tests/tile_planning_golden.t.cpp`, `tests/refinement_golden.t.cpp`,
`tests/damage_planning_golden.t.cpp`, `tests/temporal_coalescing_golden.t.cpp`,
`tests/provided_surfaces.t.cpp`, `tests/imageseq_temporal.t.cpp`,
`src/runtime/t/interactive.t.cpp`, `src/runtime/t/host_viewport.t.cpp`,
`tests/host_viewport_reanchor_golden.t.cpp`. This is Constraint 3 made
checkable — a changed golden means `direct_dispatch()` is not in fact the inline
fill, which is a bug, not a rebaseline.

**A6 — The identity map is built once per revision (behavioral counter).** Extend
`src/runtime/t/interactive.t.cpp`: five working frames at one revision leave
`renderer.identity_map_builds() == 1`; a revision bump then a sixth frame leaves
`== 2`. Never a wall-clock assertion (doc 16 tier 4). This pins Decision 2 — the
per-frame cost of the wiring is O(1), not O(graph).

**A7 — The still scene stays free.** Extend `src/runtime/t/interactive.t.cpp`: after
one working frame, N still frames (no damage, empty `pending`) leave
`identity_map_builds()`, `requests_issued`, and `composites` unchanged and return
`schedule_follow_up == false`. Re-asserts `02-architecture#interactive-still-scene-schedules-no-frame`
(registry:145) against Constraint 4.

**A8 — Coverage.** ≥90% diff coverage on the changed lines (doc 16:112-118). The
changed surface is small and entirely reachable from A1–A7; there is no excuse for
a `GCOV_EXCL` here.

**A9 — Concurrency, unchanged.** No new TSan scope is added (Constraint 9). The
existing TSan lane must pass unchanged; the acceptance is the *absence* of a new
worker-visible seam, not a new stress test. When
`runtime.worker_dispatch_leaf_only` swaps in `worker_backed_dispatch(pool)`, the
leaf-only rule becomes load-bearing here and **that** task owns the TSan coverage
for it.

**A10 — Gate.** `scripts/gate` clean: `check_levels.py` silent (Constraint 7),
`check_claims.py` silent (three second-enforcer tags naming already-registered
claims; no new registry rows — Decision 5).

### Deferred follow-up (closer registers in WBS)

- **`runtime.operator_model_damage_routing`** — *"Route **model** damage through
  `route_operator_damage` in the interactive and export drivers, so an edit to a
  content that an operator consumes by `$ref` (and is not itself a visible layer)
  re-plans and invalidates the operator layers that reach it. Today
  `map_damage_to_device` matches damage against layer roots only
  (`damage_planning.cpp:39`), so such an edit is silently dropped — an
  under-approximation, which doc 13:124-128 calls a correctness bug. This task
  routes only the *refinement-arrival* damage class that `PullConfig::pending`
  creates (Constraint 6); the model-damage arm is the same
  `route_operator_damage` call applied to `content_damage` before
  `invalidate_damage`, plus the invalidation semantics for a still-exact operator
  output tile. Source-of-debt: `tasks/refinements/runtime/interactive_pull_wiring.md`.
  Docs 02/13."* — effort **1d**, `depends !interactive_pull_wiring`, milestone
  **`m9_release`**.

  This is a pre-existing gap on both drivers, not one this task introduces, which
  is why it is split out rather than folded in: this task is responsible for the
  damage class it *creates*, and no more.

## Decisions

**Decision 1 — Build the `PullServiceImpl` inside `InteractiveRenderer::render_frame`,
not in `HostViewport`.**
Everything the service needs is already a parameter of `render_frame`: the pinned
`DocRoot&`, the `ContentResolver&`, the `TileCache&`, and the `Backend&`
(`interactive.hpp:97-101`). Building it there means **no signature change anywhere**
and keeps the service's lifetime exactly the frame's, which is what
`PullServiceImpl`'s borrowed `TileCache&`/`Backend&` demand (Constraint 2).
*Alternative rejected:* hoist the service into `HostViewport` beside the pinned
`DocStatePtr`. That is where `bind_operators` will eventually have to live (it needs
the `Document&` and the pin), but doing it now would drag a `Document&` into
`InteractiveRenderer` for a task that does not bind — pre-empting
`runtime.interactive_binder_wiring`'s design and forcing the ownership question in
the wrong task. The binder task owns that plumbing; this one deliberately requires
none of it.

**Decision 2 — Memoize the identity map on `state.revision()`; expose
`identity_map_builds()` for the counter test.**
`make_pull_identity_of` walks the whole reachable content graph
(`pull_identity.cpp:12-72`). Offline pays that once per exported frame under no
deadline; the interactive loop is deadline-bounded and its whole ethos is per-frame
cost discipline (doc 02:51 "no damage → no work"; `runtime.interactive`'s Step 7 is
render-free and probe-free by design). Keep one frame-to-frame memo — the
`shared_ptr<const PullIdentityMap>`, the `id_of` closure over it, and the inverse
`ObjectId -> const Content*` map Decision 4 needs — rebuilt only when
`state.revision()` differs from the memo's. Within one revision the pinned graph is
immutable (doc 02:121-124, doc 14), so the memo is exact; across a revision bump
every tile key changes anyway (doc 02:94-95), so a shifted synthesized id can never
serve a stale hit. The memo keys on revision alone, which is sound under precisely
the same single-document-per-renderer assumption `d_prior_revision` (the stale probe,
`interactive.hpp:125`) already relies on. Expose the build count read-only, matching
the existing "frame-to-frame state, exposed read-only for tests and host
observability" accessors at `interactive.hpp:110-113`.
*Alternative rejected:* rebuild per frame, mirroring `offline_sequence.cpp:92`
verbatim. Simpler, and the walk is deterministic so it is *correct* — but it puts an
O(graph) walk (three of them, once Decision 4's inverse map and operator-layer list
are counted) on every frame that does any work at all, in the one loop the project
holds to a frame budget. The memo is ~6 lines and turns that into O(graph) per edit.

**Decision 3 — Ship `direct_dispatch()`; leave the worker-backed dispatch to
`runtime.worker_dispatch_leaf_only`.**
`direct_dispatch()` is specified byte-for-byte identical to the inline fill
(`pull_service.hpp:48-52`), which is what makes Constraint 3 (zero golden churn)
achievable and what keeps this task free of new concurrency scope (Constraint 9).
*Alternative rejected:* build the leaf-only worker dispatch here, copying
`offline_sequence.cpp:162-170`. That would hand-copy the exact invariant whose
*whole reason for existing* is that copying it is dangerous: dispatching an operator
render to a worker re-opens the TSan-confirmed `TileCache`/`KeyedStore` race
(`offline_sequence.cpp:148-161`). `runtime.worker_dispatch_leaf_only` exists to hoist
that rule into `worker_backed_dispatch(pool)` and it `depends !interactive_pull_wiring`
— so the WBS's own ordering says the helper comes *after* this task and the interactive
driver adopts it there. This task leaves the swap a one-line change at the dispatch
argument.

**Decision 4 — Route refinement-arrival damage through `route_operator_damage`
before mapping it to device; carry the routed set, not the raw set.**
In Step 6, after `poll_refinements` returns `arrival`, fold each arrival forward:
resolve its `ObjectId` back to a `const Content*` through the inverse identity map
(Decision 2), then call
`route_operator_damage(operator_layers, damaged, d.rect, d.range)` and union the
result into the damage set that feeds `map_damage_to_device` **and** into
`d_carried_damage`. `operator_layers` is built from `state.for_each_layer` filtered
by `is_operator(resolve(layer.content))` — every layer, not the culled set;
`map_damage_to_device` culls afterwards and over-approximation is sound (doc
13:124-128). Route *every* arrival, not just the non-layer ones:
`route_operator_damage` skips an operator equal to `damaged` and emits nothing for
operators that do not reach it (`operator_graph.hpp:126-131`), so a plain leaf-layer
arrival costs one empty walk, and a content that is *both* a layer and an operator's
`$ref`'d input correctly damages both footprints. Carrying the **routed** set is what
makes frame N+1 re-plan the operator layer's device rects and re-enter the identity
branch — so **the interactive follow-up frame is the offline re-composite pass**
(`offline_sequence.cpp:196-200`), discharging `operator_identity_offline_delivery`'s
Decision 4 without a second `render_frame_interactive` call. Note carried damage does
not invalidate (`interactive.cpp:96-99`), which is correct: at an identity endpoint
there is no operator-output cache entry to drop (that predecessor's Decision 3).
*Alternative rejected:* leave `PullConfig::pending` null, so an async input pull is
simply dropped. That is a one-word change and it makes the routing problem vanish —
but `pull_service.hpp:171-176` specifies that a null `pending` **drops the async
miss**, so an operator over any async or external content would never refine
interactively at all. It trades a doc 02:69-71 violation for a doc 13:104-107
violation. *Alternative also rejected:* teach `map_damage_to_device` to route
internally. It takes no `ContentResolver` by design (`damage_planning.hpp:60-63`) and
is a pure L4 per-frame function; the operator graph and the identity map are the
*driver's* knowledge. `route_operator_damage` was built for exactly this call and has
sat without a production caller since `compositor.operator_graph` landed — this is
the seam, used as designed.

**Decision 5 — Re-enforce three existing claims; add no new registry rows.**
The behaviors this task lands are already registered and their doc language is
**driver-agnostic**: doc 13 says operators pull through the core and inputs cache
under the input's identity; it never says "offline". Rows 177
(`#operator-input-children-have-distinct-cache-identity`), 178
(`#identity-layer-delivers-input-to-frame`), 161
(`#identity-plan-issues-no-operator-render`), and 127
(`02-architecture#async-arrival-emits-damage`) already carry those promises — the
offline enforcers merely happened to be the only ones. Adding
`…#identity-layer-delivers-input-to-frame-interactively` would mint a claim id for a
sentence no design doc contains, inflating the register with an implementation axis
rather than a normative one. Instead, tag the new interactive tests with second
`enforces:` tags on those rows — the pattern `runtime.interactive`,
`kinds.nested_runtime_binding`, and `runtime.damage_router` all used. This is also why
this task needs **no design-doc delta**: doc 02:40-41 already says the two drivers sit
over one core, and doc 13's contract is stated for operators, not for a driver. This
task makes the docs true; it does not amend them.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-11.

- Constructed a frame-local `PullServiceImpl` inside `InteractiveRenderer::render_frame` (after the still-scene early-out) with a full `PullConfig`: `id_of = make_pull_identity_of(state, resolve)`, `pending = &d_pending`, `counters = &d_counters`, `contribution` keyed on the current revision — mirroring `offline_sequence.cpp:94-101` field-for-field (`src/runtime/interactive.cpp`).
- Memoized the `PullIdentityMap` per `state.revision()` in new `d_identity_map_revision` / `d_identity_map` / `d_identity_map_builds` fields; exposed `identity_map_builds()` read-only counter (`src/runtime/arbc/runtime/interactive.hpp`).
- Added `route_operator_damage` call in Step 6 of the frame loop (first production caller): async operator-input arrivals now resolve back through the inverse identity map and fan out to all operator layers that reach them, so the follow-up frame re-plans and re-drives the identity endpoint (`src/runtime/interactive.cpp`).
- Extended `src/runtime/arbc/runtime/pull_identity.hpp` and `src/runtime/pull_identity.cpp` with the inverse `ObjectId → const Content*` map needed by Decision 4's routing.
- Added counter tests A6/A7 (identity map built once per revision; still scene builds nothing) in `src/runtime/t/interactive.t.cpp`.
- Created `tests/interactive_operator_identity_golden.t.cpp`: byte-exact goldens for crossfade `w==0`/`w==1` and fade `envelope==1` vs `SequenceRenderer` (A1), plus counter assertions (A2) and cache-identity regression (A3).
- Created `tests/interactive_operator_identity.t.cpp`: async operator-input arrival → damage routing → follow-up frame → sharp byte-exact output (A4); wired in `tests/CMakeLists.txt`.
