# compositor.operator_pull_delivers_target — Visual pull delivers to caller target

## TaskJuggler entry

Back-link: [`tasks/35-compositor.tji`](../../35-compositor.tji) —
`task operator_pull_delivers_target` (lines 84-89):

```
task operator_pull_delivers_target "Visual pull delivers to caller target" {
  effort 2d
  allocate team
  depends !pull_service
  note "Make PullServiceImpl::pull deliver pulled input pixels to the caller's request.target (visual parity with pull_audio) and give an operator's nested async pull a safe reap/settle path, so FadeContent/NestedContent render correctly through the live service. Source of debt: tasks/refinements/operators/fade_runtime_binding.md. Doc 13."
}
```

## Effort estimate

**2d.** Two localized changes to `PullServiceImpl` (a delivery step on the
synchronous visual-pull paths; a surface-retention guarantee on the async
path) plus their unit / golden / TSan coverage. No new component, no new
public interface — the seams (`RenderRequest.target`, `RenderResult.provided`,
`d_backend.composite`, the `pending` reap sink) all already exist.

## Inherited dependencies

**Settled:**

- `compositor.pull_service` (Done 2026-07-06,
  [`pull_service.md`](pull_service.md)) — landed
  `PullServiceImpl final : public arbc::PullService` in
  `src/compositor/pull_service.{hpp,cpp}` with the cache-first hit path, the
  injected `RenderDispatch` worker seam, the `pending` reap queue +
  `poll_refinements` async path, the recursion-depth `GraphBudget`, and the
  `CompositorCounters` wiring. This task extends that impl in place; it does
  **not** revisit key derivation, rung selection, snapshot/deadline
  inheritance, or the dispatch injection shape.
- `compositor.operator_graph` — `aggregate_revision` /
  `resolve_identity` (the `op` short-circuit at `pull_service.cpp:125-138`).
- `compositor.counters` — the caller-owned `CompositorCounters` POD and its
  `note_request_issued` / `note_operator_render` mutators.
- `pull_audio` (landed alongside pull_service) — the delivery template this
  task mirrors: `pull_service.cpp:310-325` (hit copies samples into
  `request.target.samples`) and `:335-348` (miss dispatches with the
  caller's `target` intact).

**Pending (this task unblocks, does not depend on):**

- `operators.fade_runtime_binding` (Re-deferred 2026-07-09,
  [`fade_runtime_binding.md`](../operators/fade_runtime_binding.md)) — its
  `depends` was amended to include this task
  (`tasks/50-operators.tji:18`). It re-picks once this is `complete 100`.
- The sibling operator/nested runtime-binding tasks share the identical
  blocker (parking lot, `tasks/parking-lot.md:158-162`).

## What this task is

Make the **visual** `PullServiceImpl::pull` actually hand the pulled input's
pixels back to the caller, closing the asymmetry with `pull_audio` that
re-deferred `fade_runtime_binding`. Two deliverables:

1. **Deliver to `request.target` on the synchronous paths.** Today the
   visual `pull`:
   - on a **cache hit** (`pull_service.cpp:173-180`) settles `done` from the
     tile's *metadata only* and never touches `request.target`;
   - on an **inline miss** (`:186-243`) renders into a cache-owned
     `tile_surface`, inserts it into the cache, and returns — again leaving
     `request.target` untouched.

   The `pull_audio` arm delivers on both (`:317-319` copies the resident
   block into `request.target.samples`; the miss dispatches with
   `request.target` intact so the facet writes the caller's block directly).
   This task adds the visual analog: after the covering tile is resident
   (hit) or freshly rendered (inline miss), composite that tile into
   `request.target`, honoring the request's region and scale via the same
   tile→region mapping `plan_layer` uses to composite tiles into a frame
   (`tile_planning.cpp`). One `d_backend.composite` per pull; no new
   dispatched render.

2. **A safe reap/settle path for an operator's nested async pull.** The
   async branch (`:246-258`) retains the render's cache-owned surface +
   completion in the `pending` queue only when `d_config.pending != nullptr`.
   When a nested operator pull is issued against a service with
   `pending == nullptr` **and** the injected `d_dispatch` answers
   asynchronously, the render is dispatched, `done` is left unsettled, and
   `owned` (the surface the worker is still writing into) is freed at scope
   exit — the use-after-free / SIGSEGV that re-deferred `fade_runtime_binding`
   (parking lot `:158-162`). This task guarantees a dispatched render's
   target surface outlives the render on **every** path: an async-capable
   dispatch requires the `pending` reap sink (the surface + completion are
   retained there until the worker settles, exactly as the top-level path
   already does); a service configured without that sink must be given a
   synchronous dispatch, and `pull` asserts `done->settled()` after such a
   dispatch, degrading to the placeholder + one `GraphDiagnostic` on
   violation rather than freeing a live surface. A caller's
   `RenderCompletion::cancel()` abandons the caller's interest but never
   frees the surface a worker is mid-write on.

Together these make FadeContent / CrossfadeContent / NestedContent render
correctly through the live `PullServiceImpl`: a warm input hits and delivers
real pixels the operator composites; a cold input renders inline (synchronous
driver) and delivers in one pass, or — under an async driver — degrades to the
placeholder this frame while the render reaps safely and the async arrival's
damage re-drives a later frame that hits and delivers (doc 13's "async
composes").

**Wiring:** none beyond `PullServiceImpl`. The delivery step and the
retention guarantee live entirely inside `pull` (and its inline/async
branches); no signature changes to `render_frame_interactive`, no new
`PullConfig` field beyond what pull_service landed. The null/default
single-threaded path stays byte-for-byte against the landed goldens (a
top-level pull that fills a frame surface was already correct because the
compositor composited from the cache; the new delivery step is a no-op for a
caller whose `target` is the frame it will read from the cache anyway — see
Decision 4).

**Not this task:**

- Wiring the live service onto `FadeContent` / `NestedContent` at
  instantiation — that is `operators.fade_runtime_binding` and its siblings
  (they re-pick after this lands).
- `pull_audio` behavior — already delivers; untouched.
- Fanning operator *input* pulls onto workers concurrently (a
  concurrent/sharded input-tile cache) — parked by pull_service Decision 4
  and its parking-lot entry; not resurrected here.
- Multi-tile pull regions — `pull` keys and renders a single covering tile
  (`coords.front()`, `pull_service.cpp:160-161`); delivery stays symmetric
  with that. Widening a pull to a multi-tile region is a pre-existing
  limitation, deferred (see Acceptance criteria).

## Why it needs to be done

`fade_runtime_binding` was implemented and fully reverted on 2026-07-09: the
manual `fade->attach(pull, backend)` that only tests do cannot be promoted to
the live driver because the live visual `pull` returns transparent pixels
(byte-exact golden unachievable) and crashes on the parallel path
(`worker_pool.cpp:139` → `typed_span.hpp:34`, UAF). All three operator
runtime-binding tasks (`fade`, `crossfade`, `nested`) block on this single
compositor-contract gap. Closing it is the prerequisite for any operator to
run under the real runtime rather than an inline test double — the last seam
between the pull service and shipping operators (pull_service Done note: the
attach-injection was deferred to the M4 streams precisely here).

## Inputs / context

### Design docs (normative)

- **doc 13 — Effects as Content Operators**, `## The operator contract`
  (`13-effects-as-operators.md:69-95`): "Pulling inputs goes through the
  core"; the `PullService` "Same machinery as a compositor-issued request".
  **This task's delta** adds the explicit delivery-to-`target` paragraph
  (after the `PullService` block) and the surface-retention sentence in
  `## Caching and scheduling` (`:129-138`) — see Decisions 1 & 3.
- **doc 13**, `## Region, scale, and time dependencies` (`:97-120`): a fade
  "pulls the same region" and "evaluates its envelope at the request's time
  and pulls the input at that same time" — establishes that the operator
  expects its input's pixels *for the request's region/scale/time* to arrive
  in the surface it passed.
- **doc 13**, `## Caching and scheduling` (`:122-138`): two-level caching
  ("input tiles cache under the input's identity"), `identity()`
  short-circuits both levels, and "Async composes".
- **doc 09 — Surfaces and backends**, the content-provided-surface contract
  (cited from `content.hpp:106-118`): a `RenderResult.provided` surface is
  composited/cached "honoring the request's region and scale" and the target
  returned to the pool — the same region/scale-honoring copy the delivery
  step performs, in reverse (cache/tile → `target`).
- **doc 12 — Audio**, `12-audio#pull-audio-is-cache-first-single-settle`
  (`registry.tsv:76`): the audio arm's delivery-and-single-settle contract
  this task brings the visual arm to parity with.
- **doc 16 — SDLC and quality**, `## Test taxonomy` (`16-…:27-62`):
  byte-exact goldens (tier 3), behavioral-counter tests (tier 4),
  concurrency/TSan (tier 6); the ≥90% diff-coverage hard gate (`:112-118`).
- **doc 17 — Internal components** (`17-…:20-61`): `PullService` *interface*
  at L3 `contract`; `PullService` *implementation* at L4 `compositor`
  (depends `contract`, `cache` + below); `kind-*` at L4 reach it only through
  the L3 interface. This task stays inside L4 `compositor`.

### Source seams (real paths + lines)

- `src/compositor/pull_service.cpp:169-180` — visual **hit**: settles
  metadata, delivers nothing. **Add:** composite the resident tile into
  `request.target`.
- `src/compositor/pull_service.cpp:182-243` — visual **inline miss**:
  renders into `tile_surface`, inserts to cache. **Add:** composite
  `tile_surface` into `request.target` after the successful settle.
- `src/compositor/pull_service.cpp:246-258` — visual **async** branch: the
  `pending != nullptr` retention; the missing `pending == nullptr` case is
  the UAF. **Add:** the retention guarantee (Deliverable 2).
- `src/compositor/pull_service.cpp:310-325` / `:335-348` — `pull_audio` hit
  copy-loop and miss target-intact dispatch: the parity template.
- `src/compositor/pull_service.cpp:235-239` — `consume_render_result` +
  `d_backend.composite`: the existing tile-compositing seam the delivery
  reuses (backend is already a compositor dependency).
- `src/contract/arbc/contract/content.hpp:83-91` — `RenderRequest`
  (`Surface& target`); `:106-118` — `RenderResult.provided`; `:370-383` —
  `AudioRequest` (`AudioBlock& target`, the delivered-into audio analog);
  `:617-645` — the `PullService` interface.
- `src/kind_fade/fade_content.cpp:101-118,135-158,209-233` — FadeContent
  passes `temp` / `request.target` into `pull`/`pull_audio` and composites
  it; the un-delivered `temp` is why visual fade renders transparent.
- `src/kind_nested/nested_content.cpp:304-326` — the canonical operator pull
  idiom (`pull` → `if (!done->settled()) { done->cancel(); … }`); the
  `cancel` that must not free a live surface.
- `worker_pool.cpp:139` → `typed_span.hpp:34` — the async-into-freed-surface
  write site the retention guarantee closes.

### Test / registry conventions

- Claim id is `<doc-file-stem>#<slug>` (`registry.tsv` header); each
  registered claim is referenced by a test comment `// enforces: <claim-id>`;
  `scripts/check_claims.py` enforces the register↔test correspondence
  bidirectionally. **Re-assert, don't re-register**: an existing claim
  exercised through a new path gets a *second* `enforces:` test, not a new
  row.
- Existing operator/pull rows: `13-effects-as-operators#pull-is-cache-first`
  (`registry.tsv:139`), `#pull-inherits-snapshot-and-deadline` (`:140`),
  `#operator-pulls-only-via-pull-service` (`:144`);
  `16-sdlc-and-quality#byte-exact-goldens` (`:44`);
  `09-surfaces-and-backends#content-provided-surface-honored` (`:41`).
- Pull unit tests: `src/compositor/t/pull_service.t.cpp` (visual hit
  `:264-283` asserts settle only — **no** pixel-into-target check today;
  `pull_audio` hit `:745-800` **does** — that assertion is the model). Async
  TSan: `tests/pull_service_async.t.cpp`.

## Constraints / requirements

1. **Delivery is region/scale-honoring, not a raw blit.** The covering tile
   is at a rung-quantized scale and tile-grid coord; `request.target` holds
   the request's region at the request's scale. Delivery composites the tile
   into `target` under the tile→region affine `plan_layer` /
   `tile_planning.cpp` already computes — the same mapping doc 09 mandates
   for a `provided` surface. It must **not** assume `target` and the tile
   share dimensions or origin.
2. **Delivery adds no dispatched render and no cache entry.** It is a
   composite, not a render: a warm operator-input pull's `requests_issued`
   delta stays **0** and it creates no new cache tile (the `pull-is-cache-first`
   contract). `operator_renders` follows the existing rules unchanged.
3. **The cache stays single-writer on the frame/drain thread** (pull_service
   Decision 4). Delivery reads the resident/just-rendered tile and writes the
   caller-owned `target`; it introduces no new cache write and no new
   cross-thread cache access.
4. **A dispatched render's target surface outlives the render on every
   path.** No branch may free `owned` (or any render target) while `done` is
   unsettled and a worker may still be writing it. Async-capable dispatch ⇒
   `pending` retention; no reap sink ⇒ synchronous dispatch (asserted).
   `RenderCompletion::cancel()` never frees a live surface.
5. **The null/default single-threaded path is byte-for-byte** against the
   landed tile_planning / refinement / anchored_viewports goldens. A
   top-level frame-fill pull's delivery must not perturb pixels the
   compositor already composites from the cache (Decision 4).
6. **Levelization (doc 17): no new edge.** All work is in L4 `compositor`
   (`pull_service.{hpp,cpp}`), using only `contract` (L3: requests/results,
   `Surface`, `PullService`), `cache` (L3), and `backend` (L3,
   `d_backend.composite` — already a dependency). No L4 `kind-*` type is
   named; the CI component-graph check stays green; headers compile
   standalone.

## Acceptance criteria

**New claims** (implementer registers in `tests/claims/registry.tsv`; each
enforced by an `enforces:`-tagged test in `src/compositor/t/pull_service.t.cpp`
unless noted):

- `13-effects-as-operators#pull-delivers-to-caller-target` — "PullService::pull
  writes the pulled input's pixels into the caller's `request.target` — the
  visual analog of pull_audio delivering into AudioRequest.target — on both
  the resident cache-hit path and the synchronous miss path, honoring the
  request's region and scale, and dispatches no additional render to do so."
  Test: a pull whose input tile is resident, and a pull whose input renders
  inline, each leave the expected pixels in a caller-owned `target` (mirrors
  the `pull_audio` hit assertion at `pull_service.t.cpp:798`), with
  `requests_issued` delta 0 on the warm case.
- `13-effects-as-operators#pull-retains-render-surface-until-settle` — "A
  render dispatched by PullService::pull has its target surface retained
  until the render settles; a caller's RenderCompletion::cancel() never frees
  a surface an in-flight worker is still writing, and a pull with no async
  reap sink dispatches synchronously so its surface is consumed before pull
  returns." Test: a cross-component TSan/stress test in
  `tests/pull_service_async.t.cpp` links a real multi-worker `WorkerPool`,
  issues a nested pull whose input misses async, cancels the caller
  completion mid-flight, drains to quiescence, and asserts **no data race**,
  no use-after-free, and consistent resident-bytes/eviction after drain
  (behavioral, never wall-clock). A companion unit test asserts the
  `pending == nullptr` + async-dispatch misconfiguration degrades to the
  placeholder + one `GraphDiagnostic` (no crash).

**Re-asserted claims** (second `enforces:` test through the delivery path — do
**not** re-register):

- `13-effects-as-operators#pull-is-cache-first` (`registry.tsv:139`) — the
  hit now additionally *delivers* pixels; still zero dispatch.
- `13-effects-as-operators#operator-pulls-only-via-pull-service`
  (`registry.tsv:144`) — a FadeContent/CrossfadeContent/NestedContent driven
  through the live `PullServiceImpl` bound to `CompositorCounters` now renders
  correct pixels, and every input render/audio dispatch it provokes still
  equals one the service issued.
- `16-sdlc-and-quality#byte-exact-goldens` (`registry.tsv:44`) — the landed
  fade / crossfade / nested visual goldens re-run **through the live pull
  service** (synchronous DirectDispatch) produce byte-exact output, proving
  delivery lands the right pixels. No tolerance.

**Behavioral counters:**

- Warm operator-input pull: `requests_issued` delta **0**, no new cache tile,
  correct delivered pixels (delivery ≠ render).
- Cold operator-input pull (synchronous driver): exactly **1** input render
  dispatched, pixels delivered in the same pass; `operator_renders` per the
  existing identity/non-identity rules.

**Goldens:** no **new** golden — this task changes no pixel-producing kind;
it makes the *existing* fade/crossfade/nested goldens reachable through the
live service. New operator goldens land with their kinds, already done.

**Concurrency:** the TSan/stress coverage above is mandatory (this task
touches surface lifetime across the worker boundary).

**Coverage:** ≥90% diff coverage on the changed `pull_service.cpp` lines —
tests ship with the task.

**Deferred (named future tasks — closer registers in WBS, milestone M4
compositor):**

- `compositor.pull_multi_tile_region` — effort **1d** — widen `pull` (key,
  render, and deliver) from the single covering tile (`coords.front()`) to
  the full set of tiles covering `request.region`, so an operator pulling a
  region larger than one tile receives a fully-filled `target`. Pre-existing
  single-tile limitation surfaced (not introduced) by delivery. `depends
  !operator_pull_delivers_target`. Note cites this refinement.

The concurrent/sharded input-tile cache (fanning operator input pulls onto
workers) is **not** re-registered — it remains parked (pull_service Decision
4 + its parking-lot entry) as a pure optimization; correctness here holds
because operator recursion runs synchronously on the cache-owning thread.

## Decisions

1. **Deliver by compositing the covering tile into `request.target`, reusing
   the existing tile→region compositing path — not by rendering into
   `request.target` directly.**
   *Rationale:* the tile must be cached at tile granularity under the input's
   identity for two-level caching (doc 13:126-128), so the render targets a
   tile-sized cache surface regardless. Compositing that tile into `target`
   afterward is one `d_backend.composite` (a seam already present at
   `pull_service.cpp:237`), keeps the cache-fill contract identical to what
   pull_service landed, and honors region/scale exactly as doc 09's
   `provided`-surface copy does. It is the direct visual analog of
   `pull_audio`'s post-lookup copy loop (`:317-319`).
   *Rejected:* rendering into `request.target` and copying *into* the cache
   afterward (pull_audio-miss-style, `:347`). Symmetric in cost but forces
   the async path to still allocate a cache-owned surface (target is
   caller-owned and transient), splitting the sync/async surface handling —
   more code for no benefit. The chosen direction uses one surface (the cache
   tile) on both branches.

2. **Only the top-level pull (descent depth 0, on the frame/drain thread) may
   defer to the `pending` reap sink; every dispatched render's surface is
   retained until settle regardless of caller cancellation.**
   *Rationale:* this makes pull_service Decision 4 ("operator recursion
   synchronous on the frame thread; cache single-writer; only the top-level
   leaf render is async") true in code and closes the UAF *by construction* —
   a caller's `cancel()` abandons interest but the surface lives in the reap
   sink (top-level) or is consumed inline (synchronous driver) until the
   worker is done. Aligns with doc 13's "async composes": the operator
   degrades this frame and the async arrival's damage re-drives it, rather
   than the pull blocking the frame thread.
   *Rejected:* (a) making a nested miss render *synchronously inline on a
   worker* and write the shared cache — would contend the single-writer cache
   under the offline parallel path (a race pull_service Decision 4 explicitly
   defers). (b) A `RenderCompletion::cancel()` that also *cancels the
   in-flight worker render* — a far larger cooperative-cancellation
   machinery, unnecessary once the surface simply outlives the render.

3. **A pull configured with no reap sink (`pending == nullptr`) must be given
   a synchronous dispatch; `pull` asserts `done->settled()` after such a
   dispatch and degrades to placeholder + one `GraphDiagnostic` on
   violation.**
   *Rationale:* the pull service cannot force an injected `d_dispatch` to
   answer synchronously, so "async dispatch requires a reap sink" is a driver
   precondition. Asserting it (debug) and degrading safely (release) converts
   the silent UAF into a caught, diagnosable misconfiguration — the runtime
   binds a synchronous dispatch for any sink-less path (offline goldens use
   DirectDispatch already), and the interactive/offline-parallel paths that
   dispatch async always supply `pending`.
   *Rejected:* silently leaking the surface (unbounded memory), or blocking
   the pull thread to drain the render (no drain hook exists at the pull
   service level, and blocking the frame thread violates the deadline model).

4. **The top-level frame-fill pull's delivery is a no-op-equivalent for the
   existing single-threaded goldens.** *Rationale:* when the compositor fills
   a frame it composites from the cache anyway; delivering the same tile into
   a `target` the caller then reads changes no pixels. Guarding delivery so
   the null/default path stays byte-for-byte (Constraint 5) keeps the landed
   goldens the regression oracle — no golden regen.
   *Rejected:* an unconditional delivery that would risk perturbing the
   frame-fill path and force a golden regen for a behavior that only
   operators observe.

5. **Doc-13 delta (this commit), no doc-00 bullet.** *Rationale:* the
   delivery-to-`target` behavior was *implied* ("same machinery as a
   compositor request") but never stated, which is exactly why the impl
   diverged from `pull_audio`; the surface-retention guarantee is a
   memory-safety invariant of the async plumbing. Both are clarifications
   within doc 13's existing scope (the "Effects" resolved-question in doc 00
   already covers `PullService`), so they land as two paragraphs in doc 13
   without a new doc-00 resolved-question row.
   *Rejected:* landing the fix with no doc amendment (leaves the register's
   new claims ungrounded in normative text, violating doc 16's same-commit
   rule); a doc-00 bullet (over-weights a correctness clarification as a
   project-shaping decision).

## Open questions

(none — all decided.)

## Status

**Done** — 2026-07-09.

- `src/compositor/pull_service.cpp` — `deliver_tile` helper added; delivery
  composited into `request.target` on both the cache-hit path and the
  inline-miss path; internal-completion dispatch so the caller's `done` is
  settled via `complete` (symmetric with hit), never `take`; no-sink
  async-dispatch retention/degrade branch implemented.
- `src/compositor/t/pull_service.t.cpp` — `CaptureBackend` fixture plus three
  new tests: hit-delivery (affine-verified), inline-miss delivery, and no-sink
  degrade (NDEBUG-guarded per `cpu_backend.t.cpp` convention).
- `tests/pull_service_async.t.cpp` — cancel-mid-flight TSan/stress test against
  a real `WorkerPool`; validates no data race, no UAF, consistent resident-bytes
  after drain.
- `tests/fade_goldens.t.cpp`, `tests/crossfade_goldens.t.cpp`,
  `tests/nested_goldens.t.cpp` — byte-exact re-runs through the live
  `PullServiceImpl` (synchronous `DirectDispatch`).
- `tests/claims/registry.tsv` — two new claim rows:
  `13-effects-as-operators#pull-delivers-to-caller-target` and
  `#pull-retains-render-surface-until-settle`.
- `docs/design/13-effects-as-operators.md` — delivery-to-`target` paragraph
  and surface-retention sentence added per Decision 5 (clarification within
  doc 13's scope, no doc-00 bullet).
