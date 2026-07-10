# compositor.pull_multi_tile_region — Widen pull to full multi-tile region coverage

## TaskJuggler entry

Back-link: [`tasks/35-compositor.tji`](../../35-compositor.tji) —
`task pull_multi_tile_region` (lines 91-96):

```
task pull_multi_tile_region "Widen pull to full multi-tile region coverage" {
  effort 1d
  allocate team
  depends !operator_pull_delivers_target
  note "Widen PullServiceImpl::pull from the single covering tile (coords.front()) to the full set of tiles covering request.region, so an operator pulling a region larger than one tile receives a fully-filled target. Pre-existing single-tile limitation surfaced (not introduced) by delivery. Source of debt: tasks/refinements/compositor/operator_pull_delivers_target.md. Doc 13."
}
```

## Effort estimate

**1d.** One localized restructuring of `PullServiceImpl::pull` in place: the
already-computed `coords` vector (`pull_service.cpp:187`) becomes a loop, and
the caller's `done` settles once from the aggregate after the loop. No new
component, no new public interface, no signature change — `deliver_tile`
already takes a per-`coord` affine (`pull_service.cpp:106-117`), the
`tiles_covering` enumeration already runs, and `render_frame_interactive`'s
per-tile miss-fill loop (`tile_planning.cpp:344`) is the exact template. The
1d weight is the loop + aggregate-settle logic plus its unit / golden /
counter / TSan coverage.

## Inherited dependencies

**Settled:**

- `compositor.operator_pull_delivers_target` (Done 2026-07-09,
  [`operator_pull_delivers_target.md`](operator_pull_delivers_target.md)) —
  landed the `deliver_tile` helper and delivery-to-`request.target` on both the
  cache-hit path (`pull_service.cpp:206`) and the inline-miss path
  (`pull_service.cpp:289`), plus the internal-completion dispatch
  (`inner`) so the caller's `done` settles via `complete`, and the
  surface-retention / no-sink-degrade async guarantee
  (`pull_service.cpp:304-340`). **This task extends that impl in place**: it
  wraps the single-`coord` key/lookup/render/deliver body in a per-tile loop
  and does **not** revisit key derivation, rung selection, snapshot/deadline
  inheritance, the delivery affine, or the async reap/retention machinery —
  each of those simply runs once per covering tile instead of once.
- `compositor.pull_service` (Done 2026-07-06,
  [`pull_service.md`](pull_service.md)) — the `PullServiceImpl` impl, the
  injected `RenderDispatch` worker seam, the `pending` reap queue +
  `poll_refinements` async path, the `GraphBudget` depth backstop, and the
  `CompositorCounters` wiring, all reused per tile.
- `compositor.tile_planning` (Done) — `tiles_covering` /
  `tile_local_rect` / `k_tile_size` (`tile_planning.hpp:78,92,98`) and the
  canonical multi-tile miss-fill loop `render_frame_interactive`
  (`tile_planning.cpp:344`) this task mirrors.
- `compositor.counters` — the caller-owned `CompositorCounters` and its
  `note_request_issued` / `note_operator_render` mutators, incremented per
  dispatched tile render.

**Pending (this task unblocks, does not depend on):**

- Nothing re-picks specifically on this task. It closes a correctness gap for
  operators whose pulled region exceeds one `k_tile_size` (256 px) tile — a
  case the operator runtime-binding tasks (`operators.fade_runtime_binding`
  and siblings, already unblocked by `operator_pull_delivers_target`) will hit
  once real viewports drive a fade/crossfade/nested over a large region.

## What this task is

Widen the **visual** `PullServiceImpl::pull` from serving the **single**
covering tile (`coords.front()`, `pull_service.cpp:188`) to serving **every**
tile in `tiles_covering(rung, request.region)`, so an operator that pulls a
region larger than one 256 px tile receives a **fully-filled** `target` rather
than a `target` in which only the first covering tile carries pixels.

Today the pull computes the full covering set at `pull_service.cpp:187` and
then immediately discards everything past `.front()` (`:188`); the whole rest
of the body — key derivation (`:194`), the cache-hit lookup + deliver + settle
(`:200-212`), and the miss allocate/dispatch/consume/deliver/insert-or-pending
(`:219-340`) — runs for that one tile. `deliver_tile` is already per-`coord`
correct (`:106-117`: its tile→local→target affine is parameterized by `coord`),
so a region that fits in one tile renders correctly, but a region spanning ≥2
tiles leaves every tile after the first untouched in `target`. The header
already flags this as a temporary contract (`pull_service.hpp:170-171`: "the
single-tile pull seam"). This task makes the seam internally multi-tile.

Two deliverables:

1. **Loop the per-tile body over all covering tiles.** Restructure
   `pull_service.cpp:194-340` so each `TileCoord` in `coords` gets its own
   `TileKey` (identical `{id, revision, rung, achieved_time}`, differing only
   in `coord`), its own cache probe, and — on a resident exact-fresh hit — its
   own `deliver_tile`, or — on a miss — its own cache-destined surface,
   dispatched render, `consume_render_result`, `deliver_tile`, and
   `d_cache.insert` (inline) / `PendingTile` push (async). This is the exact
   sequence `render_frame_interactive` already runs per planned tile
   (`tile_planning.cpp:344-460`); `pull` mirrors it, with delivery into the
   caller's `target` (which the frame path does not have) added per tile.

2. **Settle the caller's `done` once, from the aggregate across all tiles.**
   The single-tile path settled `done` inline per branch; the multi-tile path
   settles it exactly once *after* the loop:
   - **All tiles resolved synchronously (hit or inline-settle):**
     `done->complete(RenderResult{rung_px, region_exact, achieved_time})`,
     where `region_exact` is the conjunction (AND) of every tile's `exact`
     (a region is exact only if every covering tile is exact at this rung),
     `achieved_scale` is the uniform `rung_px`, and `achieved_time` is the
     uniform per-input value — both identical across tiles by construction
     (same input, same rung, same request time).
   - **Any tile answers asynchronously (its `inner` unsettled, pushed to the
     reap sink):** leave `done` **unsettled**. The operator sees
     `!done->settled()`, degrades to its placeholder this frame, and ignores
     `target`; each async tile's `PendingTile` reaps and emits damage
     independently, so the follow-up frame re-plans, the operator re-pulls,
     and now every covering tile hits and delivers (doc 13's "async
     composes"). Tiles already delivered into `target` this pass are harmless:
     an unsettled `done` means the operator never reads `target`.
   - **Any tile fails inline** (settled-via-fail, or a per-tile
     `make_surface` allocation failure): fail the whole region —
     `settle_placeholder(done)` / `done->fail(...)` — and stop; a partially
     filled region is not a correct operator input.

Together these make an operator pulling a region wider than one tile receive a
seam-free, fully-filled `target` (the visual analog of `pull_audio` already
filling a multi-block request), reconstructing exactly what the same content
rendered whole would produce — the rendering-is-recursion identity across the
tile grid.

**Not this task:**

- **Fanning the covering tiles' renders onto workers concurrently** (a sharded
  / concurrent input-tile cache). The tiles are dispatched **sequentially on
  the frame/drain thread**, exactly as the single-tile path dispatched its one
  render and as `render_frame_interactive` walks its planned tiles — the cache
  stays single-writer (pull_service Decision 4). Parallelizing input-tile
  renders is a pure optimization, parked (pull_service Decision 4 +
  `tasks/parking-lot.md`); correctness here holds because operator recursion
  runs synchronously on the cache-owning thread. **Not re-registered.**
- `pull_audio` — already serves multi-block requests; untouched.
- The delivery affine, key derivation, snapshot/deadline inheritance, budget,
  identity short-circuit, and the async retention/no-sink-degrade guarantee —
  all landed by predecessors; each runs per tile unchanged.

## Why it needs to be done

`operator_pull_delivers_target` closed the pull↔pull_audio *delivery* gap but
explicitly scoped delivery to stay symmetric with the single covering tile it
served, deferring the multi-tile widening to this task
(`operator_pull_delivers_target.md:126-129, 320-328`). Delivery **surfaced**
the pre-existing single-tile limitation: before delivery the frame-fill path
composited from the cache and a top-level pull that filled a whole frame was
correct regardless, so nobody read `target`; now that operators read `target`,
a region spanning more than one 256 px tile visibly loses every tile after the
first. Any operator (fade / crossfade / nested / blur / warp) driven over a
real viewport region larger than a tile — the common case at interactive
scale — needs its input's full region delivered. This is the last correctness
gap between the pull service and operators running over non-trivial regions.

## Inputs / context

### Design docs (normative)

- **doc 13 — Effects as Content Operators**
  (`docs/design/13-effects-as-operators.md`), `## The operator contract`,
  the **"A pull delivers into the caller's target."** paragraph (`:91-101`):
  "a resident cache hit composites the input's **tile(s)** into `target`, and
  a miss that settles inline composites the **freshly-rendered tile** into
  `target`, honoring the request's region and scale". The hit path already
  reads "tile(s)" (plural); the inline-miss path reads "the freshly-rendered
  tile" (singular). **This task's delta** makes the inline-miss path plural
  and adds one explicit multi-tile-region sentence, grounding the new claim in
  normative text — see Decision 4.
- **doc 13**, `## Region, scale, and time dependencies` (`:109-123`): the
  operator chooses *what region* to pull (a fade pulls the same region, a blur
  the inflated region); it says nothing about how that region decomposes into
  tiles — that mapping is the compositor's, via `tiles_covering`.
- **doc 13**, `## Caching and scheduling` (`:135-149`): "input tiles cache
  under the input's identity" — the tile is the cache unit, so a region larger
  than a tile is inherently a *set* of independently-keyed tiles; and "Async
  composes" — the async-arrival re-drive that lets a partially-async region
  settle over frames.
- **doc 09 — Surfaces and backends**, the content-provided-surface contract
  (`registry.tsv:41`, `content.hpp:106-118`): a surface is composited
  "honoring the request's region and scale" — the same region/scale-honoring
  copy `deliver_tile` performs per tile.
- **doc 05 — Recursive composition**: rendering-is-recursion — an operator's
  view of its input equals the input rendered directly; the multi-tile fill
  must reconstruct byte-for-byte what the whole-region render produces (the
  "tiled == whole" golden the tile_planning stream established).
- **doc 16 — SDLC and quality**, `## Test taxonomy` (`:27-62`): byte-exact
  goldens (tier 3), behavioral-counter tests (tier 4), concurrency/TSan
  (tier 6); the ≥90% diff-coverage hard gate (`:112-118`).
- **doc 17 — Internal components** (`:53,56`): `PullService` *interface* at L3
  `contract`; `PullService` *implementation* at L4 `compositor` (`Depends on:
  contract, cache (+ below)`). This task stays inside L4 `compositor`, using
  only `contract`, `cache`, `backend`, and its own `tile_planning` sibling.

### Source seams (real paths + lines)

- `src/compositor/pull_service.cpp:187` — `tiles_covering(selection.rung,
  request.region)` already returns **all** covering tiles into `coords`.
  **The whole task hangs off this line already existing.**
- `src/compositor/pull_service.cpp:188` — `coords.front()` — the single-tile
  discard this task removes.
- `src/compositor/pull_service.cpp:194` — the `TileKey` construction; becomes
  per-tile (only `coord` varies).
- `src/compositor/pull_service.cpp:200-212` — the cache-hit lookup + deliver +
  `done->complete`; the lookup + `deliver_tile` move into the loop, the
  settle moves out (aggregate).
- `src/compositor/pull_service.cpp:219-302` — the miss allocate / dispatch /
  `consume_render_result` / `deliver_tile` / `insert`; runs per missing tile.
- `src/compositor/pull_service.cpp:304-340` — the async `pending`-retention
  and no-sink-degrade branch; runs per async tile (each pushes its own
  `PendingTile{key, request.region, id, stability, bytes, owned, inner}`,
  `:317-318`).
- `src/compositor/pull_service.cpp:106-117` — `deliver_tile`: **unchanged**;
  its affine is already parameterized by `coord`, so it delivers any covering
  tile into the correct sub-rect of `target`.
- `src/compositor/pull_service.hpp:170-171` — the header comment naming this
  "the single-tile pull seam" that "a caller pulling a multi-tile region
  decomposes … per tile"; **update** to state the seam now covers the full
  region internally.
- `src/compositor/tile_planning.cpp:344-460` — `render_frame_interactive`'s
  `for (PlannedTile& tile : plan.tiles)` miss-fill loop: the canonical
  per-tile allocate → dispatch → consume/insert-or-pending sequence to mirror.
- `src/compositor/tile_planning.cpp:165` — `plan_layer`'s per-`coord`
  `TileKey` derivation, the single-tile key-per-coord `pull` already does once.

### Test / registry conventions

- Claim id form `<doc-file-stem>#<kebab-slug>` (a slug, not a markdown
  anchor); each claim carries ≥1 `// enforces: <claim-id>` test;
  `scripts/check_claims.py` enforces register↔test correspondence
  bidirectionally. **Re-assert, don't re-register**: an existing claim
  exercised through the multi-tile path gets a second `enforces:` test.
- Existing pull/tile rows: `13-effects-as-operators#pull-delivers-to-caller-target`
  (`registry.tsv:141`), `#pull-is-cache-first` (`:139`),
  `#operator-pulls-only-via-pull-service` (`:146`),
  `#pull-retains-render-surface-until-settle` (`:142`);
  `02-architecture#tile-cache-key-and-value-shape` (`:68`);
  `16-sdlc-and-quality#byte-exact-goldens` (`:44`),
  `#compositor-exposes-behavioral-counters` (`:118`).
- Pull unit tests: `src/compositor/t/pull_service.t.cpp` (the `CaptureBackend`
  fixture + delivery assertions landed by `operator_pull_delivers_target`).
  Async TSan: `tests/pull_service_async.t.cpp`.

## Constraints / requirements

1. **Every covering tile is served; none is dropped.** For a `request.region`
   spanning N tiles at the selected rung, the loop visits all N `coords` and
   delivers each into its correct sub-rect of `target` under `deliver_tile`'s
   per-`coord` affine. The single-tile case (N == 1) stays byte-identical to
   today.
2. **`done` settles exactly once, from the aggregate.** No branch settles
   `done` mid-loop. `region_exact = AND(tile.exact)`; `achieved_scale =
   rung_px`; `achieved_time` the uniform per-input value. Any async tile ⇒
   `done` left unsettled (operator degrades this frame). Any inline failure ⇒
   whole region degrades to the placeholder.
3. **Delivery adds no render and no cache entry beyond the per-tile ones.**
   A warm N-tile region that hits every tile issues **zero** dispatches
   (`requests_issued` delta 0) and inserts no tile — the `pull-is-cache-first`
   contract, per tile. A cold N-tile region dispatches **exactly N** input
   renders (one per covering tile), each counted once.
4. **The cache stays single-writer on the frame/drain thread.** Tiles are
   probed, rendered, delivered, and inserted **sequentially**; no tile is
   fanned onto a worker concurrently with another tile's cache write. Only the
   per-tile leaf `render` runs on a worker (via `d_dispatch`), exactly as the
   single-tile path; the async retention guarantee holds per tile (each
   `PendingTile` owns its surface + `inner` until its worker settles).
5. **A dispatched render's target surface outlives the render on every tile.**
   The predecessor's per-branch retention invariant is preserved per tile: an
   async-capable dispatch requires the `pending` reap sink; a sink-less path
   is given a synchronous dispatch and the per-tile `inner->settled()`
   assertion still fires; no tile frees `owned` while its `inner` is unsettled.
6. **Multi-tile fill == whole-region render, byte-exact.** At a power-of-two
   scale a region spanning ≥2×2 tiles delivered tile-by-tile into `target`
   equals the same content rendered whole into a region-sized surface — no
   seam, no double-blend at tile boundaries (each tile composites into a
   disjoint half-open sub-rect). This is the rendering-is-recursion oracle
   (doc 05, "tiled == whole").
7. **The empty-region degenerate is handled cleanly.** When `coords` is empty
   (an empty/degenerate `request.region`), the loop runs zero times, delivers
   nothing, and settles `done->complete(RenderResult{rung_px, /*exact*/true,
   achieved_time})` — replacing today's degenerate `coords.empty() ?
   TileCoord{}` probe (`:188`) that would key and possibly dispatch a render
   for a `{}` coord. This is strictly a correctness improvement and affects
   only operator pulls (`PullServiceImpl::pull` is never the frame-fill path).
8. **Levelization (doc 17): no new edge.** All work is in L4 `compositor`
   (`pull_service.{hpp,cpp}`), using `contract` (L3: requests/results,
   `Surface`, `PullService`), `cache` (L3), `backend` (L3, `d_backend.composite`
   / `make_surface`), and the `tile_planning` sibling (`tiles_covering`,
   `k_tile_size`, `TileCoord`) — all already dependencies. The CI
   component-graph check stays green (`DEPENDS contract cache` unchanged);
   headers compile standalone.

## Acceptance criteria

**New claim** (implementer registers in `tests/claims/registry.tsv`; enforced
by an `enforces:`-tagged test in `src/compositor/t/pull_service.t.cpp` unless
noted):

- `13-effects-as-operators#pull-fills-multi-tile-region` — "A
  `PullService::pull` whose `request.region` spans more than one tile fills the
  caller's `request.target` across **every** covering tile of
  `tiles_covering(rung, region)`: each covering tile is independently
  cache-probed and, on a resident exact-fresh hit, composited into `target`
  under its own tile→region affine, or, on a miss, rendered into a
  cache-owned surface, composited into `target`, and inserted — the same
  per-tile sequence `render_frame_interactive` runs, with delivery added. The
  caller's `done` settles exactly once from the aggregate: exact iff every
  covering tile is exact at the selected rung, with the uniform rung scale and
  achieved_time; any covering tile answering asynchronously leaves `done`
  unsettled so the operator degrades this frame and each async tile's arrival
  re-drives it; any covering tile failing inline degrades the whole region to
  the placeholder." Test: a pull whose region spans a 2×2 tile block, with a
  deliberate mix of resident and missing input tiles, leaves the expected
  pixels in a caller-owned `target` across all four sub-rects, with
  `requests_issued` delta equal to the number of missing tiles.

**Re-asserted claims** (second `enforces:` test through the multi-tile path —
do **not** re-register):

- `13-effects-as-operators#pull-delivers-to-caller-target` (`registry.tsv:141`)
  — delivery now spans the full covering set, not just the first tile.
- `13-effects-as-operators#pull-is-cache-first` (`registry.tsv:139`) — a warm
  N-tile region issues zero dispatch and inserts no tile.
- `13-effects-as-operators#operator-pulls-only-via-pull-service`
  (`registry.tsv:146`) — a FadeContent/CrossfadeContent/NestedContent driven
  through the live `PullServiceImpl` over a multi-tile region provokes exactly
  one input render per covering missing tile, each equal to one the service
  issued.
- `13-effects-as-operators#pull-retains-render-surface-until-settle`
  (`registry.tsv:142`) — the per-tile async retention / no-sink-degrade
  guarantee holds when several covering tiles miss async at once.
- `16-sdlc-and-quality#byte-exact-goldens` (`registry.tsv:44`) — the new
  multi-tile golden (below) is byte-exact through the live pull service.

**Behavioral counters:**

- Warm N-tile region (every covering tile resident): `requests_issued` delta
  **0**, no new cache tile, correct delivered pixels across all N sub-rects.
- Cold N-tile region (synchronous driver): exactly **N** input renders
  dispatched (one per covering tile), all pixels delivered in the same pass;
  `operator_renders` bumped once per dispatched operator-input tile render
  (Decision 3).
- Partially-warm region (M of N tiles resident): exactly **N − M** dispatches;
  the M resident tiles deliver with zero dispatch.

**Golden (new — byte-exact, no tolerance):** a "pull tiled == whole" golden in
`tests/` — an operator (fade or nested) pulling a region spanning ≥2×2
`k_tile_size` tiles at a power-of-two scale, driven through the live
`PullServiceImpl` (synchronous `DirectDispatch`), produces output byte-exact
to the same content composited whole into a region-sized surface. This pins
Constraint 6 (no seam, no double-blend). The landed single-tile
fade/crossfade/nested goldens re-run unchanged (N == 1 path byte-for-byte).

**Concurrency (TSan/stress, mandatory):** extend `tests/pull_service_async.t.cpp`
with a multi-tile variant — a region where some covering tiles hit and others
miss **async** against a real multi-worker `WorkerPool`; cancel the caller
completion mid-flight; drain to quiescence; assert **no data race**, no
use-after-free, `done` correctly unsettled until every async tile arrives, and
consistent resident-bytes/eviction after drain (behavioral, never wall-clock).
This task widens the surface-lifetime-across-the-worker-boundary path from one
tile to many, so the concurrency coverage widens with it.

**Coverage:** ≥90% diff coverage on the changed `pull_service.cpp` lines —
tests ship with the task.

**Deferred:** **none new.** The concurrent/sharded input-tile cache (fanning
the covering tiles' renders onto workers in parallel) remains **parked**
(pull_service Decision 4 + its parking-lot entry) as a pure optimization —
correctness here holds because the per-tile loop runs synchronously on the
cache-owning thread. No "audit/revisit" leaf is created.

## Decisions

1. **Loop the existing single-tile body over `coords`, mirroring
   `render_frame_interactive` — do not introduce a new region-render path.**
   *Rationale:* `coords = tiles_covering(rung, request.region)` is already
   computed (`pull_service.cpp:187`); `deliver_tile` is already per-`coord`
   correct (`:106-117`); `render_frame_interactive` already runs the exact
   per-tile allocate/dispatch/consume/insert-or-pending loop the frame path
   needs (`tile_planning.cpp:344`). Wrapping `pull`'s existing body in that
   same loop reuses every landed seam, keeps the tile as the cache unit (doc
   13:135-149), and yields the tiled==whole identity for free (each tile
   composites into a disjoint half-open sub-rect). It is a 1d restructuring,
   not a redesign.
   *Rejected:* rendering the whole region into a single region-sized surface
   and caching *that*. Breaks the tile-granular two-level cache (input tiles
   share across consumers by tile identity, doc 13:135-149), defeats reuse
   when a neighboring pull overlaps only part of the region, and diverges the
   pull path from the frame path — more code, worse caching, no benefit.

2. **Settle `done` once from the aggregate after the loop; any async tile
   leaves the whole region unsettled (degrade this frame), any inline failure
   degrades to the placeholder.**
   *Rationale:* an operator consumes its input as one region — a partially
   filled `target` is not a correct input, so the region is all-or-nothing per
   frame. `region_exact = AND(tile.exact)` because a region is exact only if
   every tile is (doc 09's honest-exactness folding, matching the audio mixer's
   min/conjunction fold, `registry.tsv:75`). Leaving `done` unsettled on any
   async tile — rather than settling a partial region — makes the operator
   degrade to its placeholder and re-pull after the async arrivals' damage
   (doc 13 "Async composes", `:135-149`); tiles delivered into `target` this
   pass are harmless because an unsettled `done` means `target` is never read.
   *Rejected:* (a) settling `done` per tile — an operator has one completion
   for one region; N settles is a contract violation. (b) Settling a *partial*
   exact region when some tiles are async — would hand the operator a
   half-filled `target` it reads as complete, a visible correctness bug. (c)
   Delivering only after confirming *all* tiles resolve synchronously (buffer
   then commit) — needless since an unsettled `done` already suppresses the
   read; deliver-as-you-go is simpler and matches the single-tile path.

3. **Count `requests_issued` (and `operator_renders` for an operator input)
   once per dispatched covering tile, not once per pull.**
   *Rationale:* each missing covering tile is a genuine, separate `render`
   invocation over that tile's footprint — the same per-tile counting
   `render_frame_interactive` does (one request per planned tile miss). A warm
   region hits every tile and issues zero, preserving the `pull-is-cache-first`
   zero-work property (`registry.tsv:139`); a cold N-tile region issues N,
   which is the true dispatch count. Per-dispatch counting keeps
   `requests_issued` meaning "renders dispatched" consistent between the pull
   path and the frame path, so the behavioral-counter oracle stays uniform.
   *Rejected:* counting one `operator_render` per pull regardless of tile
   count. Would under-report the actual render work and desynchronize the
   `operator-pulls-only-via-pull-service` invariant (each input render the
   operator provokes equals one the service *issued* — N tiles ⇒ N issued),
   making the counter equality fail for multi-tile regions.

4. **Doc-13 delta (lands same-commit at close), no doc-00 bullet.**
   *Rationale:* doc 13's delivery paragraph (`:91-101`) already reads "tile(s)"
   (plural) on the cache-hit path but "the freshly-rendered tile" (singular) on
   the inline-miss path, and states no explicit multi-tile-region promise. The
   delta makes the inline-miss wording plural and adds one sentence — "a
   request whose region spans more than one tile is served across every
   covering tile; the caller's completion settles once from the aggregate,
   exact iff every covering tile is exact" — grounding the new
   `#pull-fills-multi-tile-region` claim in normative text (doc 16 same-commit
   rule). This is a clarification within doc 13's existing scope, exactly like
   `operator_pull_delivers_target`'s doc-13 delta, so it carries no doc-00
   resolved-question bullet.
   *Rejected:* landing the code with no doc amendment — leaves the new
   registered claim ungrounded in normative text, violating doc 16's
   same-commit rule; a doc-00 bullet — over-weights a per-tile-loop
   clarification as a project-shaping decision.

5. **Keep the covering tiles sequential on the frame/drain thread; do not fan
   them onto workers.** *Rationale:* the single-writer-cache invariant
   (pull_service Decision 4) requires all cache writes on the draining thread;
   the frame path already renders its planned tiles sequentially. Parallelizing
   input-tile renders is a latency optimization that would contend the cache
   and is explicitly parked. Correctness holds without it because operator
   recursion runs synchronously on the cache-owning thread.
   *Rejected:* dispatching all covering tiles concurrently now — resurrects the
   parked sharded-cache race for a performance gain this task does not need,
   and would require the very concurrent-cache machinery pull_service Decision
   4 defers.

## Open questions

(none — all decided.)

## Status

**Done** — 2026-07-10.

- Wrapped the single-tile body in a per-`coord` loop over `tiles_covering` in
  `src/compositor/pull_service.cpp`; each tile is keyed, cache-probed,
  rendered, and delivered independently; `done` settled once from the aggregate
  (`region_exact = AND`, any async ⇒ unsettled, inline-fail ⇒ placeholder).
- Fixed the latent single-tile shortcut so each tile renders its own
  `tile_local_rect`/`rung_px` (matching `render_frame_interactive`) rather than
  `request.region`/`request.scale`.
- Updated the "single-tile pull seam" header comment in
  `src/compositor/arbc/compositor/pull_service.hpp` to state full-region
  coverage.
- Added Decision 4 doc-13 delta (inline-miss now plural + multi-tile-region
  sentence) in `docs/design/13-effects-as-operators.md`.
- Registered new claim `13-effects-as-operators#pull-fills-multi-tile-region` in
  `tests/claims/registry.tsv`.
- Added 2 unit TEST_CASEs (2×2 warm/cold/partial with counter+affine assertions;
  empty-region degenerate) and extended `CaptureBackend` to record all
  composites in `src/compositor/t/pull_service.t.cpp`.
- Added multi-tile TSan variant (mixed hits/async-misses, cancel mid-flight,
  drain, re-pull composes) in `tests/pull_service_async.t.cpp`.
- New byte-exact "tiled == whole" golden (position-dependent `RasterContent`)
  in `tests/pull_multitile_golden.t.cpp` with `tests/CMakeLists.txt` stanza.
