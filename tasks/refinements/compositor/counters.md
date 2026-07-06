# compositor.counters — Behavioral counters

## TaskJuggler entry

`task counters "Behavioral counters"` in
[`tasks/35-compositor.tji`](../../35-compositor.tji) (lines 53–58):

```
task counters "Behavioral counters" {
  effort 1d
  allocate team
  depends !tile_planning
  note "Debug counters (requests, cache hits, composites) exposed for claims tests: still-scene playback issues zero renders, identity fades issue zero operator renders, etc. Doc 16."
}
```

## Effort estimate

**1d.** A small, self-contained surface: one plain-old-data counters struct,
an optional out-parameter threaded into the interactive driver and the
refinement poll (the exact precedent already exists — `RefinementQueue*
pending = nullptr`), and increments at three already-identified seams. No
new algorithm; cache hit/miss counts are *read* from the existing keyed
store, not re-implemented. The weight is in the tests, not the code.

## Inherited dependencies

**Settled:**

- **`compositor.tile_planning`** (`depends !tile_planning`,
  [`tile_planning.md`](tile_planning.md)) — landed the interactive pipeline
  this task instruments: `plan_layer`
  (`src/compositor/arbc/compositor/tile_planning.hpp:127`) with its
  cache-probe order (`src/compositor/tile_planning.cpp:139/156/173`) and the
  `is_miss` flag (`:149`), and the driver `render_frame_interactive`
  (`tile_planning.hpp:155`) whose inline miss-render call
  (`src/compositor/tile_planning.cpp:263`) and per-tile composite
  (`:297`, plus `composite_coarser` at `:40/:66/:69/:303`) are the exact
  counter seams. `tile_planning` **explicitly deferred the counter surface
  to this task** (its Not-this-task block) and proved its behaviors by
  direct assertion on the returned `LayerTilePlan` (`miss_count`,
  `src/compositor/t/tile_planning.t.cpp:60`); this task wires the same
  behaviors to the exposed counters.
- **`compositor.refinement`** (`refinement.md`) — added the async-arrival /
  follow-up-frame stage (`poll_refinements`,
  `src/compositor/refinement.cpp:67`, inserting arrivals at `:84`) and the
  optional `RefinementQueue* pending` trailing parameter on
  `render_frame_interactive`. That trailing-optional-pointer shape is the
  template this task copies for the counters out-parameter. It, too,
  deferred counter exposure here.
- **`arbc::cache`** — the keyed store already publishes the behavioral
  cache counters this task surfaces: `hits()/misses()/evictions()`
  (`src/cache/arbc/cache/keyed_store.hpp:193–195`), with the contract that a
  qualifying `lookup` bumps `hits()` and a miss bumps `misses()`
  (`:165–167`). No new cache counting is written.

**Pending:** none — every predecessor is landed.

## What this task is

Expose the compositor's wall-clock-free **behavioral counters** so the
efficiency claims doc 16 promises are asserted on *counts*, not timings.
Concretely:

1. A caller-owned **`CompositorCounters`** POD struct
   (`src/compositor/arbc/compositor/counters.hpp`, new): plain
   `std::uint64_t` fields — **`requests_issued`** (inline miss renders
   driven), **`composites`** (display-source composite calls, coarser
   upscales included), and **`follow_up_frames`** (refinement arrivals that
   settled into the cache and emitted damage) — each with a `noexcept`
   accessor, matching the established per-component counter style
   (`keyed_store.hpp:193`, `pool/arbc/pool/checkpoint.hpp:209–216`).
2. An **optional out-parameter** `CompositorCounters* counters = nullptr`
   appended to `render_frame_interactive`
   (`tile_planning.hpp:155`) and to `poll_refinements`
   (`refinement.hpp`), incremented at the seams named above. Null (the
   default) preserves the exact current behavior, byte-for-byte — the same
   opt-in discipline `RefinementQueue* pending` uses.
3. A **snapshot view** — `CompositorStats counters_snapshot(const
   CompositorCounters&, const TileCache&)` — that composes the compositor's
   own counts with the cache's `hits()/misses()/evictions()` into the
   observability record a host debug panel reads, mirroring
   `HousekeepingStats` (`src/runtime/arbc/runtime/housekeeping.hpp:50–62`):
   "no new counting mechanism, just a composed view of numbers the
   mechanisms publish."

**Not this task:**

- **Operator renders / the "identity fades issue zero operator renders"
  counter.** The `identity()` short-circuit that this counter would prove
  lives in `compositor.operator_graph` (`tasks/35-compositor.tji`,
  `task operator_graph`), which is not yet built — there is no seam to
  increment today. That counter and its behavioral claim are scoped to
  `operator_graph`'s own refinement, which owns the short-circuit seam. This
  is an **existing WBS task, not a new leaf** (see Acceptance criteria).
- **The offline `render_frame` path.** It has no cache and always renders
  every tile; no efficiency claim rides on it, so it stays uninstrumented
  (Decision 4).
- **A `base`-level global counter registry.** Rejected by design — see the
  doc 17 delta and Decision 1.

## Why it needs to be done

Doc 16 makes behavioral counters the *non-flaky* substitute for wall-clock
performance gates: "Wall-clock tests lie in CI; counters don't"
(doc 16:54–62). The compositor's efficiency promises — a still scene
playing back issues **zero** render requests, a warm-cache re-plan is
**all** cache hits — are exactly the claims that must gate on counts. Today
those behaviors are proven only through test-local proxies: a counting
`Content` double's `renders()` (`tests/tile_planning_golden.t.cpp:41–51`)
and the plan's `miss_count` (`src/compositor/t/tile_planning.t.cpp:60`).
That works for a test but is not the **exposed** surface a host debug panel
or a downstream `pull_service` consumer can read. This task promotes the
scattered proxies into the compositor's public counter surface, closing the
last piece of the interactive pipeline's observability that doc 16 names.

Downstream: `compositor.pull_service` and `runtime`'s interactive frame loop
read these counters for their own behavioral assertions and for the host
memory/telemetry panel; `operator_graph` extends the same struct with the
operator-render count when it lands.

## Inputs / context

- **`docs/design/16-sdlc-and-quality.md:54–62`** — the behavioral-counter
  tier (normative): "The core exposes debug counters (render requests
  issued, cache hits/misses, tiles composited, blocks mixed, slots
  allocated/reclaimed) and tests assert *behavioral* claims: playback of a
  still scene issues zero visual renders …". Also `:15–21` (claims
  register), `:112–118` (≥90% diff-coverage hard gate).
- **`docs/design/17-internal-components.md:56`** — compositor is **Level 4**
  (`depends on contract, cache (+ below)`); `:41` — a component may depend
  only on strictly lower levels. `:48` — `arbc::base`'s "debug counters"
  content line, clarified by this task's delta (see Decisions).
- **`src/compositor/arbc/compositor/compositor.hpp:26`** — "the compositor
  stays a pure per-frame library"; persistent state lives in runtime. This
  is the constraint that forces caller-owned counters (Decision 1).
- **`src/compositor/arbc/compositor/tile_planning.hpp:155–159`** —
  `render_frame_interactive` signature, including the
  `RefinementQueue* pending = nullptr` trailing optional (the shape to
  copy). Seams: `src/compositor/tile_planning.cpp:263` (inline miss render →
  `requests_issued`), `:297` + `composite_coarser` `:40/:66/:69/:303`
  (composite → `composites`).
- **`src/compositor/arbc/compositor/refinement.hpp`** /
  **`refinement.cpp:67,:84`** — `poll_refinements`; a settled arrival
  inserted into the cache and emitting damage → `follow_up_frames`.
- **`src/cache/arbc/cache/keyed_store.hpp:193–195`** — `hits()/misses()/
  evictions()` the snapshot reads (contract at `:165–167`).
- **`src/runtime/arbc/runtime/housekeeping.hpp:50–62`** — `HousekeepingStats`,
  the composed-snapshot precedent.
- **`tests/claims/registry.tsv`** — 2-column TAB-separated
  `<claim-id>\t<description>`, `<doc-file-stem>#<slug>`; enforced by
  `scripts/check_claims.py` (bidirectional: every claim has a live
  `enforces:` test and every tag names a registered claim). Existing rows to
  re-assert-not-re-register: `:65` `02-architecture#miss-becomes-deadline-request`,
  `:67` `11-time-and-video#static-tiles-survive-clock`.
- **Test conventions:** unit tests `src/compositor/t/<name>.t.cpp` (Catch2,
  `arbc_component_test`); cross-component goldens `tests/<name>_golden.t.cpp`
  (`arbc_<name>_golden_t`, links `arbc` + `Catch2::Catch2WithMain`). The
  current counting `Content` double: `tests/tile_planning_golden.t.cpp:41–51`.

## Constraints / requirements

- **Levelization (doc 17:41/56):** the counters surface lives in
  `arbc::compositor` (L4) and may reference only `contract`/`cache` (+ below).
  `CompositorCounters` uses `<cstdint>` only — no new dependency edge, no
  edge into `runtime` (L5). CI's levelization check must stay green.
- **Pure per-frame library (compositor.hpp:26):** the compositor holds **no**
  persistent counter state. `CompositorCounters` is caller-owned and passed
  by pointer; persistence (accumulate-across-frames) is the caller's
  business, exactly as `SurfacePool`/`TileCache`/`RefinementQueue` already
  are.
- **Zero behavior change when null:** with `counters == nullptr` (the
  default), `render_frame_interactive` and `poll_refinements` produce
  byte-identical output and identical cache state to today. The existing
  `tile_planning`/`refinement` goldens must pass unchanged.
- **No wall-clock, no atomics required:** counts are plain `std::uint64_t`
  incremented on the single-threaded driver path (doc 02:135–137,
  reaffirmed at `tile_planning.hpp:144`). No `std::atomic` — the driver is
  single-threaded; the async worker pool is `compositor.pull_service`, which
  will own any threading concerns when it wraps this seam.
- **Counter semantics must match the seam exactly:** `requests_issued`
  increments once per inline miss render actually driven
  (`tile_planning.cpp:263`), not per planned miss (a coarser/placeholder
  fallback that is not inline-filled issues no request). `composites`
  counts every `Backend::composite` call the tiled driver makes, coarser
  upscales included. `follow_up_frames` counts arrivals that settled into
  the cache and produced damage in a `poll_refinements` call.
- **Diff coverage ≥90% (doc 16:112–118)** on the changed lines — tests ship
  in this task.

## Acceptance criteria

**Claims (register + `enforces:` tags)** — one new row appended to
`tests/claims/registry.tsv`, enforced from the counters test:

- **`16-sdlc-and-quality#compositor-exposes-behavioral-counters`** — "The
  compositor exposes wall-clock-free debug counters — render requests
  issued, cache hits/misses, tiles composited — as caller-owned
  `std::uint64_t` fields, so a still-scene warm-cache re-plan is observable
  as `requests_issued == 0` with all cache hits, not as a timing." (doc
  16:54–62.) Enforced by `src/compositor/t/counters.t.cpp` via a
  `// enforces:` comment.

**Behavioral-counter unit test — `src/compositor/t/counters.t.cpp`** (new,
Catch2, registered via `arbc_component_test`; doc 16:54–62, never
wall-clock):

- **Still scene issues zero renders through the counter surface:** drive
  `render_frame_interactive` twice over an unchanged scene with a
  `CompositorCounters` bound. Frame 1: `requests_issued == tiles_covered`,
  `cache.misses()` advanced by the same, `composites == tiles_covered`.
  Frame 2 (warm cache, unchanged): `requests_issued` delta `== 0`,
  `cache.hits()` delta `== tiles_covered`, `cache.misses()` delta `== 0`,
  `composites` delta `== tiles_covered`. This is the counter-surface
  restatement of the `miss_count(frame2) == 0` assertion at
  `src/compositor/t/tile_planning.t.cpp:340–341`, and it additionally
  re-asserts (does **not** re-register) the landed claims
  `11-time-and-video#static-tiles-survive-clock` (`registry.tsv:67`) and
  `02-architecture#miss-becomes-deadline-request` (`registry.tsv:65`) via
  their `enforces:` tags — a claim may carry more than one enforcing test.
- **Async arrival counts one follow-up frame:** with a `RefinementQueue` and
  a `Content` double that answers asynchronously, an inline miss records
  pending and does not bump `requests_issued` beyond the drive; a subsequent
  `poll_refinements` that settles the arrival bumps `follow_up_frames` by 1
  and, on the follow-up frame, plans it as a fresh hit (`cache.hits()`
  advances, `requests_issued` delta `== 0`). Mirrors
  `src/compositor/t/refinement.t.cpp`'s async assertions, now on the counter
  surface.
- **Null out-parameter is byte-identical:** the same two-frame drive with
  `counters == nullptr` produces the identical `target` bytes and identical
  `cache.hits()/misses()` deltas as the instrumented run — proving the
  counter path is side-effect-free on rendering.
- **Snapshot composition:** `counters_snapshot(counters, cache)` returns a
  `CompositorStats` whose fields equal the live `counters` fields and the
  live `cache.hits()/misses()/evictions()` — the `HousekeepingStats`
  composed-view property.

**Golden regression (no new golden):** the existing
`tests/tile_planning_golden.t.cpp` and `tests/refinement_golden.t.cpp` pass
unchanged, confirming the null-default path is byte-exact
(`16-sdlc-and-quality#byte-exact-goldens`, `registry.tsv:35` — a meta-claim
this task does not re-register).

**Deferred follow-up (existing task, no new WBS leaf):** the **operator-render
counter** and the claim **"identity fades issue zero operator renders"** are
scoped to **`compositor.operator_graph`** (`tasks/35-compositor.tji`), which
owns the `identity()` short-circuit seam and is not yet built. Its refinement
extends `CompositorCounters` with an `operator_renders` field and registers
that claim when the seam lands. No closer action beyond noting the
cross-reference — `operator_graph` already exists in the WBS.

## Decisions

1. **Caller-owned counters struct, not a `base` global registry.** The
   compositor is a pure per-frame library (`compositor.hpp:26`); a mutable
   `base`-level singleton would reintroduce persistent cross-frame state into
   a stateless engine and make behavioral assertions order-dependent under
   Catch2 parallel execution. The caller-owned struct threaded by optional
   pointer matches the already-shipped `SurfacePool`/`TileCache`/
   `RefinementQueue` ownership and the per-component counter reality
   (`keyed_store` owns its `hits()/misses()`, `runtime` composes
   `HousekeepingStats`, `pool` counts on arenas). Doc 17:48 lists "debug
   counters" as `base` content, which read literally suggests a shared
   facility; that gap is a genuine ambiguity, so this task lands a **doc 17
   delta** (a "Notes on placements" bullet) clarifying that "debug counters"
   names the *primitive style*, that components own their own counters, and
   that pure-per-frame libraries take a caller-owned struct — no central
   registry. *Alternative rejected:* a `base` `CounterRegistry` singleton —
   simpler call sites (increment anywhere) but breaks the stateless-library
   posture and test determinism, and no existing counter in the tree uses
   one. The clarification (not a reversal) doesn't rise to a doc-00
   decision-record change; the existing "Internal components" bullet
   (doc 00:146–151) already covers levelization.

2. **Reuse the cache's own hit/miss counters; don't shadow them.** `cache`
   already counts `hits()/misses()/evictions()` with a documented bump
   contract (`keyed_store.hpp:165–167,193–195`). `CompositorCounters` counts
   only what the compositor uniquely knows — requests inline-driven,
   composites issued, follow-up frames — and the snapshot *composes* the two.
   *Alternative rejected:* a compositor-side hit/miss mirror — double
   counting, and it could silently diverge from the store's own numbers.

3. **Optional trailing `CompositorCounters*` default null, mirroring
   `RefinementQueue* pending`.** Keeps the null path byte-identical (the
   `tile_planning`/`refinement` goldens are the guard) and avoids a signature
   churn across the many existing call sites. *Alternative rejected:* a
   required parameter — forces every caller and test to construct a struct
   they may not want and perturbs the goldens.

4. **Instrument only the interactive/refinement path, not offline
   `render_frame`.** The behavioral claims ("still-scene playback issues zero
   renders") are properties of the *cached, interactive* pipeline; offline
   `render_frame` has no cache and renders every tile by definition, so a
   counter there measures nothing a claim asserts. *Alternative rejected:*
   instrumenting both — adds surface with no claim to pin it, against doc
   16's "test coverage that pins observable behavior" bias.

5. **`requests_issued` counts requests *driven*, not misses *planned*.** A
   planned miss shown via a coarser/placeholder fallback that is not
   inline-filled issues no `RenderRequest`; the counter must match the actual
   `content->render` call site (`tile_planning.cpp:263`) so it stays faithful
   to the doc-16 wording "render requests issued". This also keeps it
   distinct from the plan-level `miss_count` proxy, which counts fresh-key
   misses regardless of fallback.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-06.

- `src/compositor/arbc/compositor/counters.hpp` (new): `CompositorCounters` POD struct with `requests_issued`, `composites`, `follow_up_frames` fields (`uint64_t`, private `d_`, `noexcept` accessors, `note_*` bumps); `CompositorStats`; inline `counters_snapshot`.
- `src/compositor/arbc/compositor/tile_planning.hpp` / `src/compositor/tile_planning.cpp`: optional trailing `CompositorCounters* counters = nullptr` on `render_frame_interactive`; `requests_issued` bumped at the `content->render` call site; `composites` bumped at every `Backend::composite` call (Fresh/Stale + both `composite_coarser` paths).
- `src/compositor/arbc/compositor/refinement.hpp` / `src/compositor/refinement.cpp`: optional trailing `CompositorCounters* counters = nullptr` on `poll_refinements`; `follow_up_frames` bumped per settled+damaged arrival.
- `src/compositor/CMakeLists.txt`: registered `counters.hpp` header and `t/counters.t.cpp` test target.
- `src/compositor/t/counters.t.cpp` (new): 4 Catch2 cases — still-scene zero-renders, async-arrival follow-up-frame, null-param byte-identical, snapshot composition.
- `tests/claims/registry.tsv`: one new claim row `16-sdlc-and-quality#compositor-exposes-behavioral-counters`.
- `docs/design/17-internal-components.md`: delta clarifying "debug counters" names the primitive style (components own their counters; pure-per-frame libraries use caller-owned structs — no central registry).
