# Refinement — `runtime.operator_identity_offline_delivery`

## TaskJuggler entry

`tasks/65-runtime.tji:74-79`:

```
task operator_identity_offline_delivery "Deliver identity-served operator input pixels to offline/export frame" {
  effort 1d
  allocate team
  depends !operator_input_cache_identity
  note "Wire the compositor delivery seam so an identity-served (w==0/w==1) operator-input child's pixels reach the offline/export frame buffer; unblocks endpoint byte-exact-frame acceptance deferred by runtime.operator_input_cache_identity. Source-of-debt: tasks/refinements/runtime/operator_input_cache_identity.md. Doc 13."
}
```

This task is the deferred follow-up minted by `runtime.operator_input_cache_identity`
(`tasks/refinements/runtime/operator_input_cache_identity.md:454`, closer note: *"endpoint
`w==0`/`w==1` byte-exact-frame delivery through the offline driver remains blank
(pre-existing compositor delivery gap); registered as
`runtime.operator_identity_offline_delivery`"*). It belongs to milestone `m9_release`
(`tasks/99-milestones.tji`), the same milestone that gathers the runtime-binding and
identity work — the reference operators are not shippable end-to-end until a crossfade
layer at its **endpoints** (`w==0`/`w==1`) renders byte-exact through the real offline
driver, not just at its interior dissolve.

## Effort estimate

`effort 1d`, `allocate team`. One production seam (a delivery branch on the existing
operator-layer identity short-circuit in `render_frame_interactive`), a one-line driver
wiring change (the inline offline path passes its already-built `PullServiceImpl` as
`pulls`), plus the carried-in endpoint acceptance surface (byte-exact endpoint frame
goldens inline + parallel, a zero-operator-render counter assertion, and one new claim).
No new kernel, no new math, no new `PullConfig`/`Backend`/`RenderRequest` field, no
design-doc delta. The delivery machinery (`deliver_tile`, multi-tile coverage, cache-first
serve) already exists in `PullServiceImpl::pull`; this task routes the frame driver's
identity short-circuit through it.

## Inherited dependencies

**Settled predecessors this task builds on (all `complete 100`):**

- `runtime.operator_input_cache_identity` (`tasks/65-runtime.tji:67`, refinement
  `tasks/refinements/runtime/operator_input_cache_identity.md`, **Done 2026-07-10**).
  Landed `src/runtime/pull_identity.cpp` (`make_pull_identity_of`), so the offline/export
  `PullConfig::id_of` now gives **every** operator input child a distinct, stable cache
  identity (claim `13-effects-as-operators#operator-input-children-have-distinct-cache-identity`,
  registry row 154). It pinned the crossfade **interior** dissolve (`w==0.5`) byte-exact
  through the real `SequenceRenderer` (inline + parallel/TSan), but **explicitly deferred
  the endpoint `w==0`/`w==1` byte-exact frame to this task** because the driver's frame
  path short-circuits the operator render at the endpoint and never delivers input N's
  pixels into the frame — the blank-frame gap this task closes.
- `compositor.operator_graph` (refinement
  `tasks/refinements/compositor/operator_graph.md`, **Done 2026-07-06**). **Decision 5**
  (`operator_graph.md:430-442`): the identity short-circuit landed as a **planning
  decision only** — suppress the operator render, zero `operator_renders`, and create
  **no operator-output cache entry** (explicitly rejecting caching the input's pixels
  under the operator's `TileKey`, `:437`) — while *"serving input N's cached tiles is
  `pull_service`'s job."* This task is precisely that serving half, for the frame-driver
  path. `resolve_identity` / `IdentityResolution` are the seam it consumes, unchanged.
- `compositor.operator_pull_delivers_target` (refinement
  `tasks/refinements/compositor/operator_pull_delivers_target.md`, **Done 2026-07-09**).
  Landed the `deliver_tile` helper (`pull_service.cpp:106-117`) that composites a
  resident/just-rendered tile into `request.target` under a region/scale-honoring affine —
  the delivery primitive this task reuses. Its Decision 1 (`:337`) fixed *deliver by
  compositing the covering tile into `request.target`*, and its Constraint 2 (`:234`)
  *no dispatched render and no cache entry to deliver* — both inherited here.
- `compositor.pull_multi_tile_region` (refinement
  `tasks/refinements/compositor/pull_multi_tile_region.md`, **Done 2026-07-10**). Widened
  `pull` to serve **every** covering tile of `tiles_covering(rung, region)` into its own
  sub-rect of `target` (claim row 142). So a `pull` that serves the endpoint terminal
  already covers a multi-tile operator layer footprint — no extra work here.
- `compositor.pull_service` (refinement `tasks/refinements/compositor/pull_service.md`,
  **Done 2026-07-06**). Landed `PullServiceImpl`, `PullConfig`, and the two-seam split:
  `pull` (full cache-first serve + delivery, including the identity recursion) vs
  `dispatch` (render-call-only). Its Decision 3 (`:476`) is that `render_frame_interactive`
  takes a trailing `PullServiceImpl* pulls` and that the null path is byte-for-byte the
  pre-task inline fill; `pull_service.hpp:195-199` states the delegated `dispatch` seam
  "stays byte-for-byte identical to the pre-task inline fill." This equivalence is what
  lets the inline driver pass a non-null `pulls` here (Decision 2).
- `operators.crossfade_runtime_binding` / `operators.crossfade` (**Done 2026-07-10 /
  2026-07-09**). Shipped `CrossfadeContent::identity()` (returns 0 at `w==0`, 1 at `w==1`,
  `crossfade_content.cpp:110-119`) and its endpoint `render()` that pulls the identity
  input straight through (`:184-192`) — the content-level faithfulness this task makes the
  frame driver honor. Claim `13-effects-as-operators#crossfade-identity-at-endpoints`
  (row 153) rests on *"the compositor serving that input's cached tiles is bit-identical
  to a render."*

**Pending — nothing.** All predecessors are `complete 100`. This task introduces no new
dependency and (per Decisions) no design-doc delta.

## What this task is

The offline sequence driver renders each frame through `render_frame_interactive`
(`tile_planning.cpp:230`). For a visible **operator** layer (`op_layer`, e.g. a crossfade
layer) the frame path resolves the operator's identity chain before issuing a render
(`tile_planning.cpp:381-386`):

```cpp
bool issue_render = true;
if (op_layer) {
  const IdentityResolution ident =
      resolve_identity(content, request, GraphBudget{}, diagnostics);
  issue_render = !(ident.short_circuited || ident.budget_exceeded);
}
if (issue_render) { /* dispatch/render the operator, insert into cache, composite */ }
```

At a crossfade endpoint (`w==0`/`w==1`) `identity()` fires, `resolve_identity` reports
`short_circuited == true`, and `issue_render` becomes `false`. That correctly suppresses
the operator render and the operator-output cache entry (claim
`13-effects-as-operators#identity-plan-issues-no-operator-render`, row 138, and
`operator_graph.md` Decision 5) — **but the branch then delivers nothing.** No render
runs, `tile.display_source` keeps its planned fallback, `tile.hold` stays invalid, and the
composite step (`tile_planning.cpp:467-475`) composites nothing. The operator layer's
footprint in the offline frame comes out **blank** at its endpoints. That is the
"pre-existing compositor delivery gap" the predecessor named: the plan-time short-circuit
is implemented, but the *serving* of input N's pixels into the frame — which
`operator_graph.md` Decision 5 deferred to `pull_service` — was never wired for the frame
driver.

The **pull path already handles this**: when an operator *pulls* an input that itself
short-circuits, `PullServiceImpl::pull` recurses on the terminal
(`pull_service.cpp:152-165`) and delivers the terminal's tiles into `request.target` via
`deliver_tile`. The gap is only the *frame-driver* path, which reaches the short-circuit
through `resolve_identity` at plan time, not through a `pull`.

This task closes the gap by routing the frame driver's identity short-circuit through the
same serving machinery: on `short_circuited`, `render_frame_interactive` serves the
resolved terminal (`ident.terminal`) into the operator layer's tile surface via
`pulls->pull(ident.terminal, request, done)` — the existing cache-first, multi-tile,
`deliver_tile`-based serve — then composites that tile surface at the operator layer's
placement and opacity. No operator render, no operator-output cache entry (the terminal's
own tile is cached under **its** identity by `pull`, shared by every consumer); the
frame's operator-layer footprint now holds input N's pixels, byte-identical to input N
rendered directly. The inline offline driver, which today passes `pulls == nullptr`,
passes its already-built `inline_pull` (whose `direct_dispatch` makes the delegated render
byte-for-byte identical to the prior inline fill, `pull_service.hpp:195-199`,
`pull_service.cpp:20-32`) so `render_frame_interactive` has a `PullServiceImpl` to serve
through.

## Not this task

- **No new operator render / operator-output cache entry.** The suppression half
  (`issue_render == false`, zero `operator_renders`, no operator-keyed `TileKey`) stays
  exactly as `operator_graph.md` Decision 5 and claim row 138 fixed it. This task adds
  only *delivery* of the already-decided identity input.
- **No compositor-core key/config change.** `PullConfig`, `TileKey`/`BlockKey`,
  `aggregate_revision`, `deliver_tile`, and the multi-tile coverage are frozen and reused
  verbatim. No new `PullConfig` field (`operator_pull_delivers_target.md:114-115`), no new
  `Backend` op (`composite` is the sole delivery primitive, `backend.hpp:39`), no new
  public `render_frame_interactive` parameter beyond using the existing `pulls`.
- **No audio change.** The audio endpoint is already correct: a crossfade's `render_audio`
  at the endpoint pulls the identity input through `pull_audio`, and
  `runtime.operator_input_cache_identity` pinned the distinct-tone export-audio mix
  byte-exact. This task is the **visual** frame endpoint only.
- **Not the interactive frame path.** `interactive.cpp:115` calls
  `render_frame_interactive` with `pulls == nullptr` and builds no production
  `PullConfig::id_of` (per `operator_input_cache_identity.md` Open questions) — so the
  interactive endpoint would not be served by this task's gated branch. Interactive
  endpoint delivery is the interactive stream's concern (see Open questions); the fix in
  `render_frame_interactive` is inherited there for free once interactive wires a
  `PullServiceImpl`.
- **Not the legacy single-frame `render_offline`.** `offline.cpp:25` uses the older
  `render_frame` (no operator identity handling); the sequence driver
  (`offline_sequence.cpp`) is the operator-aware offline path this task targets.

## Why it needs to be done

- **It closes the endpoint debt and unblocks `m9_release`.** The crossfade cannot render
  byte-exact end-to-end at its endpoints — the last correctness gap the predecessor left
  open — until the frame driver serves input N at `w==0`/`w==1` instead of a blank tile.
- **It is a general identity-delivery fix, not crossfade-specific.** Any operator layer
  whose `identity()` fires for a frame (a fade at open envelope, a disabled effect, a
  placeholder passing input 0) mis-renders blank through the offline driver today. Wiring
  the frame-path serve fixes them all.
- **It makes the observable behavior match the design.** Doc 13:59-65 promises that at an
  identity request "the compositor serves the input's cached tiles directly"; doc 13:145
  says `identity()` "short-circuits both levels" while still producing input N's output;
  claims `03-layer-plugin-interface#operator-identity-faithful` (row 108) and
  `13-effects-as-operators#crossfade-identity-at-endpoints` (row 153) assert the served
  output is bit-identical to input N. For the offline frame that promise is silently
  broken (blank) until this fix.

## Inputs / context

**Design docs (normative — doc 16's constitution rule):**

- `docs/design/13-effects-as-operators.md`:
  - `:59-66` — the `identity()` pass-through: at an identity request "the compositor
    serves the input's cached tiles directly — no render, no copy, no new cache entry."
    The frame driver must *serve* input N (this task), while still creating no
    operator-output entry (already landed).
  - `:91-107` — "A pull delivers into the caller's target": a resident cache hit
    composites the input's tile(s) into `target`, and a request spanning more than one
    tile is "served across every covering tile … each … delivered into its own sub-rect
    of `target`." The delivery contract this task routes the frame short-circuit through.
  - `:141-154` — "input tiles cache under the input's identity (shared by every consumer),
    operator output under the operator's. `identity()` short-circuits both levels." The
    terminal served here is cached under **its** identity, never the operator's.
- `docs/design/17-internal-components.md`:
  - `:41-44`, `:53-60` — the levelization edges: `compositor` (L4) owns the `PullService`
    implementation and the frame driver `render_frame_interactive`; `arbc::runtime` (L5)
    is "everything below" and owns the two render drivers. This task's compositor change
    stays inside L4 (uses `contract`/`cache`/`backend`), and the driver wiring stays in
    L5 — **no new component edge.**

**Source seams (real paths + current lines):**

- **The edit surface (frame driver):** `src/compositor/tile_planning.cpp:230`
  (`render_frame_interactive`), `:236` (the trailing `PullServiceImpl* pulls`), `:299`
  (`op_layer = is_operator(content)`), `:381-386` (the identity short-circuit that sets
  `issue_render = false` and delivers nothing — **the gap**), `:387-461` (the
  `issue_render` body: allocate `tile_surface`, dispatch/render, insert, set
  `display_source = Fresh`), `:408-417` (`pulls->dispatch` vs inline `content->render`),
  `:467-475` (composite from `tile.hold->surface` at `layer.opacity` through the
  layer→device affine).
- **The delivery machinery (frozen — reuse, do not edit):**
  `PullServiceImpl::pull` `src/compositor/pull_service.cpp:152-165` (identity recursion to
  terminal), `:213` (per-covering-tile loop), `:223-228` (cache-hit `deliver_tile`),
  `:305-312` (inline-miss `deliver_tile` + insert under the input's key); `deliver_tile`
  `:106-117` (`Backend::composite` into `request.target` under the tile→region→target
  affine); `PullServiceImpl::dispatch` `:67-70` (render-call-only seam);
  `direct_dispatch` `:20-32` (drives `content->render` inline + `done->complete` — the
  byte-for-byte equivalence to the null path).
- **The short-circuit resolver (frozen):** `resolve_identity`
  `src/compositor/arbc/compositor/operator_graph.hpp:159` returning `IdentityResolution`
  (`:136-148`: `terminal`, `short_circuited`, `budget_exceeded`); definition
  `src/compositor/operator_graph.cpp:117-155`.
- **The driver wiring (edit):** `src/runtime/offline_sequence.cpp:119` (inline path builds
  `inline_pull` with the full `make_config`), `:120` (`bind_operators`), `:121-125`
  (calls `render_frame_interactive` with **`/*pulls=*/nullptr`** — the one-line change:
  pass `&inline_pull`); `:140` (parallel path builds `pulls`), `:145-149` (already passes
  `&pulls`).
- **The interface (frozen):** `PullServiceImpl` ctor
  `src/compositor/arbc/compositor/pull_service.hpp:151`, `pull` `:177-178`, `dispatch`
  `:200-201` (with the `:195-199` "byte-for-byte identical to the pre-task inline fill"
  guarantee); `PullConfig::id_of` `:127`, `contribution` `:131`. `RenderRequest::target`
  is a `Surface&` `src/contract/arbc/contract/content.hpp:88`; `Content::identity`
  `:598`; `PullService` `:617-640`. `Backend::composite`
  `src/surface/arbc/surface/backend.hpp:39-40`.
- **The operator whose endpoints this pins:** `CrossfadeContent::identity`
  `src/kind_crossfade/crossfade_content.cpp:110-119`, endpoint `render` (pull-through)
  `:151-166, :184-192`.
- **Behavioral-counter surface:** `src/compositor/arbc/compositor/counters.hpp` —
  `operator_renders()`, `requests_issued()`.
- **The predecessor test to extend:** `tests/crossfade_offline_dissolve.t.cpp` — its
  `render_crossfade_reference(params, time)` oracle (`:94-116`) is already parameterized by
  time and produces the correct endpoint reference (input 0 at `w==0`, input 1 at `w==1`),
  and the interior dissolve is pinned byte-exact through `SequenceRenderer` inline (`:193`)
  and parallel (`:218`); the parallel test already renders the `w==0` endpoint frame at
  `t==0` as TSan stress (`:233`) but does **not** assert it byte-exact. This task adds the
  endpoint byte-exact frame assertions.

**Predecessor decisions carried in:**

- `operator_input_cache_identity.md:454` — the exact deferral of endpoint byte-exact frame
  delivery to this task; the child-distinct `id_of` (`make_pull_identity_of`) it landed is
  what lets `pulls->pull(terminal)` key the terminal correctly.
- `operator_graph.md:430-442` (Decision 5) — suppression only, serving is `pull_service`'s;
  no operator-output cache entry. Honored: this task serves without an operator-keyed entry.
- `operator_pull_delivers_target.md:337` (Decision 1) + `pull_multi_tile_region.md:375`
  (Decision 1) — deliver by compositing each covering tile into `target`; reused verbatim.
- `pull_service.md:476` (Decision 3) + `pull_service.hpp:195-199` — the `pulls` seam and
  its byte-for-byte `direct_dispatch` equivalence; the basis for Decision 2 below.
- Test-placement rule `crossfade_runtime_binding.md:285-287` (Deviation 1): byte-exact
  acceptance needs `backend_cpu`, so the endpoint frame goldens live in top-level `tests/`
  (extending `tests/crossfade_offline_dissolve.t.cpp`), not `src/compositor/t/`.

## Constraints / requirements

1. **Endpoint delivery (correctness).** For a visible operator layer whose
   `identity(request)` fires, the offline-frame footprint of that layer must hold input
   N's pixels — byte-identical to input N rendered directly for the same request — placed
   through the operator layer's device affine at the layer's opacity. No blank tile. This
   is the invariant whose violation is the bug.
2. **Suppression preserved (behavioral).** The delivery must add **zero** operator renders
   and create **no** operator-output cache entry (claim row 138 and `operator_graph.md`
   Decision 5). The terminal's own tile is cached under **its** identity by
   `pulls->pull` — shared by every consumer (doc 13:141-154), not keyed under the
   operator's `TileKey`.
3. **Reuse the delivery chain, add no surface area.** Serving goes through the existing
   `PullServiceImpl::pull` → cache-first probe → `deliver_tile` → `Backend::composite`
   path. No new `PullConfig`/`RenderRequest` field, no new `Backend` op, no new public
   `render_frame_interactive` parameter, no separate pull cycle beyond the single
   `pull(terminal)` the short-circuit implies.
4. **Non-identity path byte-for-byte.** The inline offline driver now passes a non-null
   `pulls`; because that `PullServiceImpl` uses `direct_dispatch` (`content->render` inline
   + `done->complete`, `pull_service.cpp:20-32`), the `issue_render` branch's
   `pulls->dispatch(content, …)` is byte-identical to the prior inline `content->render`
   (`pull_service.hpp:195-199`). Every non-endpoint tile — leaf layers and the interior
   dissolve — must reproduce the existing goldens unchanged.
5. **Multi-tile coverage.** An operator layer footprint spanning more than one tile at an
   endpoint must be served across **every** covering tile (reusing `pull`'s
   `tiles_covering` loop, claim row 142) — no single-tile shortcut, no seams.
6. **Async endpoint degrades-and-redrives.** On the parallel offline path, if a covering
   terminal tile answers asynchronously, the endpoint tile is left unfilled this frame and
   re-driven on arrival — identical to the non-identity async path (doc 13:104-107 "async
   composes"). No special-casing; the offline no-degrade discipline reaps to quiescence as
   for any tile.
7. **Concurrency.** The terminal serve runs on the driver/frame thread (single-writer
   cache, `runtime.threading`'s rule); only each leaf `render` runs on a worker. Race-free
   under TSan, reusing the reap-to-quiescence discipline the predecessor's parallel lane
   proved.
8. **Levelization (doc 17).** The delivery branch lives in `compositor`
   (`render_frame_interactive`, L4, using `contract`/`cache`/`backend`); the one-line
   `pulls` wiring lives in `arbc::runtime` (L5). **No new component edge**; the CI
   dependency check stays green.

## Acceptance criteria

**New claim (the promise this task lands):** register
`13-effects-as-operators#identity-layer-delivers-input-to-frame` in
`tests/claims/registry.tsv`, enforced by a test tagged `enforces:`. Claim text:
*A visible operator layer whose identity(request) returns input N is composited into the
offline frame with input N's pixels: the frame driver serves input N's cached tiles into
the layer's tile footprint through PullService::pull (every covering tile, cache-first,
delivered via deliver_tile) and composites them at the layer's device affine and opacity —
byte-identical to input N rendered directly — while issuing zero operator renders and
creating no operator-output cache entry (the delivery counterpart of
13-effects-as-operators#identity-plan-issues-no-operator-render).* Pins the serving half
of doc 13:59-65 for the frame driver.

**Endpoint byte-exact frame goldens (the carried-in headline, inline + parallel):** extend
`tests/crossfade_offline_dissolve.t.cpp` — render the crossfade-over-two-distinct-solids
scene through the real `SequenceRenderer` at `w==0` (frame == input 0) and `w==1` (frame
== input 1), on **both** the inline and the parallel exact paths, and assert byte-exact
against `render_crossfade_reference(params, endpoint_time)`. This fails on today's code
(the endpoint frame is blank). No tolerance; re-asserts
`16-sdlc-and-quality#byte-exact-goldens`. Lives in top-level `tests/` (needs
`backend_cpu`; `crossfade_runtime_binding.md` Deviation 1). This is the acceptance
`runtime.operator_input_cache_identity` deferred here.

**Suppression-preserved counter assertion (behavioral):** the endpoint frame render issues
**zero** operator renders (`CompositorCounters::operator_renders()` delta 0 — the operator
never renders at the endpoint) and no operator-output cache entry, while `requests_issued`
reflects only the terminal input's own render (exactly one when the terminal is cold, zero
when it is warm). Re-enforces (second `enforces:` tag, no new row)
`13-effects-as-operators#identity-plan-issues-no-operator-render` (row 138) for the
delivery path, proving the fix delivers *without* re-introducing an operator render.

**Cross-frame endpoint stability (behavioral counter):** the crossfade endpoint rendered
across a sequence whose clock advances but whose achieved time coalesces issues **zero**
input re-renders after the first frame — the terminal's `Static` tile, cached under its
own stable identity by `pulls->pull`, survives the clock. Re-asserts (second `enforces:`
tag, no new row) `11-time-and-video#static-tiles-survive-clock` and
`#achieved-time-coalescing-issues-zero-renders` for the identity-served path.

**Re-enforced end-to-end (no new rows):** with endpoint delivery landed,
`03-layer-plugin-interface#operator-identity-faithful` (row 108) and
`13-effects-as-operators#crossfade-identity-at-endpoints` (row 153) now hold **through the
offline driver** (the frame output is bit-identical to input N), and the delivery reuses
`13-effects-as-operators#pull-delivers-to-caller-target` (row 141) /
`#pull-fills-multi-tile-region` (row 142). Add second `enforces:` tags on the new/extended
tests; add no new registry rows for these.

**Concurrency (TSan lane, carried in):** the endpoint frame rendered through the parallel
offline path under TSan asserts no data race — the terminal serve runs on the driver
thread, leaf renders on workers, reaped to quiescence (Constraint 7). Reuses the parallel
offline path the predecessor's dissolve TSan lane already drives (`tests/CMakeLists.txt`
tsan preset).

**Coverage:** ≥90% diff coverage on changed lines (CI gate). Tests ship in this task.

**Deferred follow-ups:** none new. This task **consumes** the endpoint byte-exact frame
debt `runtime.operator_input_cache_identity` minted; it spawns no new WBS leaf. (The
interactive endpoint / interactive-`id_of` wiring observation is surfaced under Open
questions for the parking lot, not minted as a task — see there.)

## Decisions

1. **Serve the resolved terminal through the existing `PullServiceImpl::pull`, delivering
   into the operator layer's tile surface — not by calling the operator's `render()` and
   not by a bespoke copy.** On the frame-path identity short-circuit
   (`tile_planning.cpp:381-386`), when `ident.short_circuited && ident.terminal != nullptr`,
   `render_frame_interactive` calls `pulls->pull(const_cast<Content*>(ident.terminal),
   request, done)` with `request.target` set to the layer's freshly-allocated tile
   surface, then composites that surface at the layer's affine/opacity as a fresh display
   source. *Rationale:* `pull` already does the correct cache-first, multi-tile,
   `deliver_tile`-based serve that keys the terminal under **its** identity (via
   `PullConfig::id_of`, which the predecessor populated) and creates **no** operator-output
   entry — exactly the serving half `operator_graph.md` Decision 5 deferred to
   `pull_service`. It is the same mechanism the pull path already uses
   (`pull_service.cpp:159-164`), now reached from the frame driver. *Rejected:* (a) drop
   the short-circuit and call the operator's `render()` at the endpoint — that would count
   an `operator_render` and insert an operator-output cache entry under the operator's
   `TileKey`, violating claim row 138 and `operator_graph.md` Decision 5 (`:437`); (b) a
   bespoke `Backend::composite` of the terminal's tile directly in `tile_planning` —
   reinvents `deliver_tile`, bypasses the cache-first probe and multi-tile coverage, and
   duplicates a path `pull` already owns.

2. **The inline offline driver passes its already-built `PullServiceImpl` (`inline_pull`)
   as `pulls`, replacing the `nullptr` argument.** *Rationale:* `render_frame_interactive`
   needs a `PullServiceImpl` (which carries `PullConfig::id_of`, so it can key the
   terminal) to serve the short-circuit; the driver already constructs and binds one
   (`offline_sequence.cpp:119-120`). Passing it is byte-for-byte safe on the non-identity
   path because `inline_pull` uses `direct_dispatch`, so `pulls->dispatch(content, …)` in
   the `issue_render` branch is exactly `content->render(request, done)` +
   `done->complete` — the prior inline fill (`pull_service.cpp:20-32`,
   `pull_service.hpp:195-199`); the existing interior-dissolve and leaf goldens pin it.
   The parallel path already passes `&pulls` (`:145-149`), so it needs no wiring change —
   only the new short-circuit branch. *Rejected:* adding a second dedicated
   `render_frame_interactive` parameter for identity-serving — unnecessary once the
   `direct_dispatch` equivalence lets the existing `pulls` seam carry the serve, and a new
   parameter widens the interface against Constraint 3.

3. **No operator-output cache entry; the frame composites the delivered tile surface as a
   fresh, uncached-under-operator-key display source.** The short-circuit branch allocates
   the layer's tile surface, has `pull` deliver the terminal into it, and composites it at
   `layer.opacity` through the layer→device affine (as the `issue_render` path does for a
   fresh tile) — but does **not** `cache.insert` under the operator's `tile.key`.
   *Rationale:* the served pixels *are* the terminal's, already cached under the terminal's
   own identity by `pull`; a second entry under the operator key is the exact duplication
   `operator_graph.md` Decision 5 (`:437`) and doc 13:59-65 ("no new cache entry") forbid,
   and would silently double the crossfade's endpoint memory. *Rejected:* caching the
   served tile under the operator's key for "warm reuse" — reintroduces the forbidden
   operator-output entry; the terminal's own cache entry already gives cross-frame reuse.

4. **Async endpoint follows the non-identity async path — no special-casing.** If a
   covering terminal tile answers asynchronously (parallel path), `pull` leaves `done`
   unsettled; the endpoint tile is left unfilled this frame and re-driven on arrival,
   exactly as a non-identity operator tile whose input is async (doc 13:104-107). The
   offline sequence's reap-to-quiescence loop already settles it before the exact
   composite. *Rationale:* the identity-served tile is just another tile; threading its
   async through the same completion plumbing keeps one code path. *Rejected:* forcing a
   synchronous terminal serve at the endpoint — needless divergence from the async
   discipline the driver already runs.

5. **No design-doc delta.** *Rationale:* doc 13:59-65 already promises the compositor
   serves input N's cached tiles at an identity request, doc 13:145 that `identity()`
   short-circuits both levels while yielding input N's output, and claims rows 108/153
   assert the served output is bit-identical to input N. This task *implements* that
   promise for the frame driver — a bug fix toward designed behavior (the frame was blank),
   not a behavior change. It creates no new component edge (doc 17 respected). The delivery
   mechanism (`deliver_tile`) was already amended into doc 13 by
   `operator_pull_delivers_target`. The frame-driver wiring is an internal compositor/driver
   seam whose home is this refinement, following the `operator_graph.md` / `pull_service.md`
   precedent (internal driver-seam shape stays in refinements, not docs). *Rejected:* a
   doc-13 clarifying sentence that the frame driver serves the short-circuit — no promise
   or edge changes, and the serving is already stated at :59-65/:145.

## Open questions

The interactive frame path (`interactive.cpp:115`) calls `render_frame_interactive` with
`pulls == nullptr` and builds no production `PullConfig::id_of`
(`operator_input_cache_identity.md` Open questions), so this task's gated short-circuit
serve does not fire there — an interactive operator layer at its endpoint would still
render blank. That is the same "interactive not yet fully pull-wired" gap the predecessor
flagged, owned by the interactive stream, not an offline-frame delivery this task can
close; minting a WBS leaf for it risks duplicating an existing interactive-stream task. It
is **not** deferred as a named task here; it is flagged for the parking lot so a human can
confirm whether the interactive-wiring task already covers passing a `PullServiceImpl`
(with `id_of`) into `render_frame_interactive` to inherit this fix. (All in-scope
questions are decided.)

## Status

**Done** — 2026-07-10.

- `src/compositor/tile_planning.cpp` — captured `identity_terminal` from the short-circuit; added an identity-delivery `else if` branch that serves the resolved terminal through `pulls->pull()` into the layer's tile surface and composites it at the layer affine/opacity (no operator render, no operator-output cache entry; drops any planned fallback hold, marks `Fresh`).
- `src/runtime/offline_sequence.cpp` — inline path now passes `&inline_pull` (was `nullptr`); the parallel re-composite pass now passes `&pulls` (was `nullptr`) so the endpoint is served synchronously after reap-to-quiescence.
- `tests/claims/registry.tsv` — new claim `13-effects-as-operators#identity-layer-delivers-input-to-frame`.
- `tests/crossfade_offline_dissolve.t.cpp` — endpoint test rewritten for byte-exact delivery; unit/golden endpoint byte-exact frame (`w==0`/`w==1`, inline + parallel) vs `render_crossfade_reference`; counter assertions (zero operator renders, one input render cold); new cross-frame endpoint-stability test (zero input re-renders after first frame).
- Claim `13-effects-as-operators#identity-layer-delivers-input-to-frame` landed; re-enforced (second tags, no new rows): rows 108/153/138/141, 94/121.
- Deviation from refinement's literal driver-wiring note: the parallel re-composite pass (`offline_sequence.cpp:159-163`) also required `nullptr→&pulls` — the identity op tile is always a miss (no operator-output entry), so it must deliver via `pull` on the final pass too; this is Constraint 6's "re-driven on arrival" and confirmed by the passing parallel byte-exact endpoint tests.
- No new WBS tech-debt tasks minted; interactive endpoint delivery gap flagged in parking lot.
