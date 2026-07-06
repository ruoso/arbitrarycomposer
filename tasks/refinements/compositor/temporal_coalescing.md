# compositor.temporal_coalescing — Temporal achieved_time coalescing

## TaskJuggler entry

`tasks/35-compositor.tji:29-33` → `compositor.temporal_coalescing` ("Temporal
achieved_time coalescing"), the fourth leaf under `task compositor`. It carries
`depends !damage_planning` (`35-compositor.tji:32`) and, through the parent
`task compositor`, inherits
`depends contract.async_render, cache.key_shapes, color.resampling`
(`35-compositor.tji:7`). Note line:

> "Quantize a Timed layer's requested Time to the content's reported temporal
> grid (achieved_time bucketing, doc 11:111-114) so a clock advance within one
> frame period reuses the cached tile and issues zero renders — the temporal
> analog of no-damage-no-work. Docs 11. Source of debt:
> tasks/refinements/compositor/damage_planning.md."

Registered as tech debt by `damage_planning` (Decision 7,
[`damage_planning.md`](damage_planning.md):561-576): that task delivered the
temporal *axis* (which layers a clock advance dirties) and deferred the sub-frame
*coalescing optimization* — the content-grid bucketing — to this leaf.

## Effort estimate

**2d.** This task adds no rendering pipeline and no new frame path. Its whole
deliverable is a **one-input keying change** — replacing the identity time-bucket
that `tile_planning` provisionally shipped (`tile_planning.cpp:133-134`, raw
requested time for `Timed`/`Live`) with a quantized one — plus the single seam
that makes that quantization *sound and render-free*: a defaulted, pure
`Content::quantize_time(Time)` query (contract, doc 03/11 delta below). Concretely:
(1) the **contract delta** — one defaulted virtual on `Content`
(`content.hpp`), `nullopt` = today's identity behaviour, so no landed kind or
golden changes; (2) the **planner change** — `plan_layer` keys `Timed`/`Live`
tiles at `content->quantize_time(time).value_or(time)` instead of the raw `time`
(`tile_planning.cpp:133-134`), and the driver threads a real composition time into
`plan_layer` in place of the hard-wired `Time::zero()` (`tile_planning.cpp:266`,
`:288`) through a new defaulted `Time` value parameter; (3) a **test `Timed`
content kind** in the compositor tests that snaps to a 24 fps grid; (4) **unit +
behavioral-counter tests** proving a within-frame-period advance re-plans to an
all-fresh-hit and issues zero renders, and a cross-frame-period advance renders
once; (5) **one contract-conformance case** pinning `quantize_time`↔`achieved_time`
consistency and idempotence; (6) two claims-register rows and the CMake wiring.
Everything else — the cache key's `std::optional<Time> achieved_time` field, the
clock-advance damage axis, the tile grid, the counters — is already landed; this
task composes them.

## Inherited dependencies

**Settled:**

- **`compositor.damage_planning`** (DONE 2026-07-06,
  [`damage_planning.md`](damage_planning.md)) — the direct predecessor
  (`depends !damage_planning`) and source of this debt. It landed the temporal
  damage *axis* this task completes:
  - **`clock_advance_damage`** (`src/compositor/arbc/compositor/damage_planning.hpp`)
    emits `Damage` for visible **non-`Static`** layers on a clock advance, so a
    clock tick re-plans exactly the `Timed`/`Live` layers. This task makes that
    re-plan *cheap*: within one native frame period the re-plan resolves to a
    cached tile rather than a fresh render.
  - Its Decision 7 named this task crisply ("a quantization kernel keyed off the
    content's `RenderResult::achieved_time`, a keying change, and a behavioral
    test that a sub-frame-period advance bumps `requests_issued` by 0"). It also
    noted (`damage_planning.hpp:74`) that `Static` keys "omit `achieved_time` and
    are clock-invariant" — the invariant this task must leave untouched.
- **`cache.key_shapes`** (DONE 2026-07-05,
  [`../cache/key_shapes.md`](../cache/key_shapes.md)) — the key this task keys
  into. Live source `src/cache/arbc/cache/key_shapes.hpp`:
  - **`struct TileKey { ObjectId content; std::uint64_t revision{0}; ScaleRung
    rung; TileCoord coord; std::optional<Time> achieved_time; }`** (`:64-76`).
    `achieved_time` is `std::optional<Time>`, **absent (`nullopt`) for `Static`**
    content and present for `Timed` (`:56-63`); the hash mixes a distinct constant
    for present-vs-absent so a `Static` key never collides with a `Timed` key at
    `flicks == 0` (`:153-158`). The field **already exists** — this task changes
    *what value* fills it for `Timed` content, not the key shape. `key_shapes.md`
    explicitly deferred "achieved-time coalescing / quantization (mapping a
    requested time to an `achieved_time` bucket)" to the compositor.
- **`compositor.tile_planning`** (DONE 2026-07-05,
  [`tile_planning.md`](tile_planning.md)) — the planner this task's keying change
  edits. Live source `src/compositor/tile_planning.cpp`:
  - **`plan_layer(TileCache&, ObjectId content, std::uint64_t revision,
    std::optional<std::uint64_t> prior_revision, const RungSelection&, const Rect&
    local_region, const Affine& local_to_device, Stability stability, Time time,
    StateHandle, Deadline)`** (`tile_planning.hpp:128`, impl `:115`). The
    load-bearing line is **`:133-134`**:
    `const std::optional<Time> achieved_time = (stability == Stability::Static) ?
    std::nullopt : std::optional<Time>(time);` — the identity bucket
    (`Timed`/`Live` carry the **raw** requested `time`). Its own comment already
    flags the debt: *"bucketing is damage_planning's"* (`:130-132`). The same
    `achieved_time` fills the fresh key (`:140`), the stale key (`:162`), and the
    coarser key (`:179`). This task replaces the raw `time` with the quantized one.
  - **The driver hard-wires `Time::zero()`.** `render_frame_interactive` calls
    `plan_layer(..., content->stability(), Time::zero(), ...)` (`:266-267`) and
    builds the miss `RenderRequest` with `Time::zero()` (`:288-290`) — a
    walking-skeleton stand-in. To exercise coalescing the driver must plan at a
    **real** composition time; this task threads one in (Decision 4).
  - **The fill path drops `RenderResult::achieved_time`.** On a miss the tile is
    stored under the plan-built key (`:305-306`), copying only `achieved_scale` and
    `exact` into `TileMeta`; `result.achieved_time` is discarded. With the planner
    keying by `quantize_time(t)` **before** the render, the stored key already
    equals the native instant — consistent with the render's own report (the
    conformance contract guarantees they match), so no post-render re-key is needed
    (Decision 3). The async-arrival insert (`refinement.cpp:85-91`) likewise stores
    under the plan-built key and derives damage time from
    `pending.key.achieved_time.value_or(Time::zero())` — automatically correct once
    the plan key is quantized; **`poll_refinements` is unchanged**.
- **`contract.async_render` / `contract.temporal_fields`** (landed) — the render
  contract this task extends by one defaulted method. Live source
  `src/contract/arbc/contract/content.hpp`:
  - **`enum class Stability { Static, Timed, Live }`** (`:25-30`) — `Static`
    "ignores `request.time`, reports no `achieved_time`"; `Timed` "reports the
    quantized `achieved_time` it rendered, cacheable per achieved_time"; `Live`
    non-deterministic. Resolved on the plan path via the virtual
    `Content::stability()` (`:173`).
  - **`struct RenderResult { double achieved_scale; bool exact;
    std::optional<Time> achieved_time; }`** (`:86-99`) — `achieved_time` (`:98`) is
    "the local media time actually rendered, if the content quantized the requested
    `time` … a 24 fps clip asked for `t=0.31 s` renders `7/24 s` and says so"
    (`:89-97`). This task's `quantize_time` is the **render-free** counterpart of
    this field, contractually equal to it.
  - **`struct RenderRequest { Rect region; double scale; Time time; … }`**
    (`:76-84`) — the requested content-local `time` (`:79`) the compositor
    quantizes.
  - Existing conformance coverage: `src/contract/t/temporal_fields.t.cpp` already
    pins that `Timed` content reports `achieved_time == frame7` for `t` in
    `[7/24, 8/24)` and byte-identical pixels across those times (`:116-151`), and
    that `Static` reports `nullopt` (`:156-171`). This is the fixture the new
    `quantize_time` conformance case extends.
- **`compositor.counters`** (DONE 2026-07-06, [`counters.md`](counters.md)) — the
  behavioral surface that pins the zero-render claim. `CompositorCounters`
  (`counters.hpp:34`, `requests_issued`/`composites`/`follow_up_frames`),
  `counters_snapshot` (`:74`). A within-frame-period advance asserts
  `requests_issued == 0` through this surface.
- **`cache.prefetch`** (landed) — `src/cache/arbc/cache/prefetch.hpp`. Its
  `prefetch_temporal_step` (`:49`) advances `key.achieved_time` by whole buckets
  and its doc is explicit that **"the cache neither quantizes time nor knows frame
  cadence"** (`:98-103`) — the quantization is the *compositor's* job, which is
  exactly this task. No change to prefetch; this task supplies the cadence the
  temporal ring assumed a producer for.

**Pending:** none — every predecessor is landed.

## What this task is

Doc 11:110-114 promises the payoff that makes playback cheap: *"A 24 fps clip
asked for t=0.31 s renders its frame at 7/24 s and says so. The compositor then
serves every request in [7/24, 8/24) from that cached entry — on a 60 fps output,
more than half of all playback requests against 24 fps content become cache hits
by achieved-time coalescing, with zero decoder work."* Today the compositor
delivers **none** of that coalescing: `plan_layer` keys `Timed` tiles by the raw
requested time (`tile_planning.cpp:133-134`), so two distinct requested instants
in the same native frame (output frames at 1/60 apart, both inside one 1/24 native
period) form two different keys and each misses → each renders. `damage_planning`
made a clock advance re-plan the moving layers; this task makes that re-plan reuse
the cached native frame.

The core realization (Decision 1): the compositor **cannot** learn the bucket
`[7/24, 8/24)` from the post-render `achieved_time` alone. One render at `t=0.31`
reveals the bucket *floor* (`7/24`) but never its *span*; a floor-probe over the
cached instants over-serves a skipped native frame under a seek (cache holds
frames 6 and 8, a scrub to `7.5/24` would wrongly serve frame 6). Sound,
render-free, zero-render coalescing therefore requires the compositor to snap the
requested time to the native grid **before** the cache lookup — and only the
content knows its cadence. So this task adds exactly one seam:

1. **Contract — `std::optional<Time> Content::quantize_time(Time) const`**
   (defaulted `{ return std::nullopt; }`, `content.hpp`). Returns the native grid
   instant a render at `t` would resolve to, **without rendering**. `nullopt` means
   "does not quantize" (honors any time exactly, or is `Static`) → the requested
   time is used as-is, byte-identical to today. A 24 fps `Timed` clip returns
   `floor(t·24)/24`. **Contract, conformance-tested:** when `quantize_time(t)` has
   a value it MUST equal `render(time = t).achieved_time`, and `quantize_time` is
   idempotent. This is the design-doc delta (doc 11 §Contract changes; doc 00
   decision record).

2. **Planner — quantized keying.** In `plan_layer`, replace the identity bucket at
   `tile_planning.cpp:133-134` with
   `const std::optional<Time> key_time = (stability == Stability::Static) ?
   std::nullopt : std::optional<Time>(content_quantize(time));` where
   `content_quantize(time) = content->quantize_time(time).value_or(time)`. `Static`
   still keys `nullopt` (clock-invariant, unchanged); `Timed`/`Live` key the
   snapped instant. `plan_layer` reaches the `Content*` the driver already resolved
   — the quantization is a single query on the layer's content, evaluated once per
   layer, not per tile. Fresh/stale/coarser keys (`:140,:162,:179`) all use the
   same snapped instant, so a within-period advance produces byte-identical keys and
   re-plans to all-fresh hits (zero misses, zero renders).

3. **Driver — real composition time.** `render_frame_interactive` gains a trailing
   defaulted `Time composition_time = Time::zero()` value parameter, threaded to
   `plan_layer`'s `time` argument (`:266-267`) and the miss `RenderRequest`
   (`:288-290`) in place of the hard-wired `Time::zero()`. Defaulted, so every
   landed caller and golden is byte-unchanged; a `Time` value, not a clock — the
   compositor stays stateless and the transport that *samples* the clock is
   `runtime.interactive`'s (doc 11:129, doc 17:60). Per-layer local-time
   composition through time maps (doc 11:130) is `operator_graph`'s; until it lands
   local time == composition time (identity), which is what this task passes.

**Not this task:**

- **The transport / clock / the frame loop that advances time and re-invokes the
  driver** → `runtime.interactive` (doc 17:60). This task consumes a composition
  time by value and quantizes it; it owns no clock and no loop. Same stateless,
  caller-owned boundary `damage_planning` and `refinement` drew.
- **Per-layer local-time computation by composing time maps down the tree**
  (doc 11:129-131) → the time-map walk is `operator_graph`'s
  (`35-compositor.tji:35-39`); this task snaps the *content-local* time the caller
  supplies. Until time maps land, local time == composition time.
- **The temporal prefetch ring** (doc 11:141-149, `cache/prefetch.hpp`) — a
  distinct priority class the runtime primes; landed on the cache side, driven by
  `runtime`. This task supplies the per-frame cadence snapping the ring assumed,
  not the ring's scheduling.
- **`Live` coalescing.** `Live` content is non-deterministic (cacheable only within
  a snapshot); it returns `nullopt` from `quantize_time` (identity bucket, raw
  time) exactly as today — no cross-time reuse. Only `Timed` content benefits
  (Decision 5).
- **Widening the offline `render_frame`** (exact, renders every tile) — no
  coalescing on the exact path; an `Exact` request renders the requested instant
  faithfully.
- **Implementing `quantize_time` on real decoder-backed video content** — no such
  kind exists yet (`kind_solid` is `Static`). This task ships the contract method
  (defaulted) and a test `Timed` kind; the video kind implements it when it lands
  (`55-kinds` / the image-sequence reference kind, doc 11).

## Why it needs to be done

The time axis is load-bearing in doc 11: *"on a 60 fps output, more than half of
all playback requests against 24 fps content become cache hits by achieved-time
coalescing"* (`11:112-114`). That is the temporal analog of "no damage → no work":
a clock advance that stays inside one native frame period is *quiescent* for the
moving layer's pixels and must issue zero renders. Today it issues a full render
per output frame, because the raw-time keying (`tile_planning.cpp:133-134`) never
lets two requested instants share a cache entry. `damage_planning` made clock
advance a first-class damage source that re-plans the moving layers — but without
coalescing, "re-plan the moving layers" means "re-render them every output frame,"
so playback of a mostly-still 24 fps scene on a 60 fps display does 2.5× the
decoder work it should. `key_shapes` shipped the `achieved_time` key field for
exactly this and `cache.prefetch` built a temporal ring on top of it, both assuming
a producer of quantized keys that does not yet exist. This task is that producer.
Until it lands, the `achieved_time` field is inert for coalescing (every `Timed`
tile keys a unique raw instant), the temporal prefetch ring has no cadence to
step, and doc 11's headline efficiency promise is unrealized.

## Inputs / context

- `docs/design/11-time-and-video.md`:
  - **`:87-125` — Contract changes (doc 03).** `:98-105` the `RenderResult`
    with `std::optional<Time> achieved_time` (the post-render report). `:108-114`
    the achieved-time coalescing promise (`Static` ignores time; a 24 fps clip
    renders `7/24` and "the compositor then serves every request in `[7/24, 8/24)`
    from that cached entry"). **The `quantize_time` bullet added by this task's
    delta** (immediately after `:114`) — the render-free grid query, defaulted
    `nullopt`, contractually equal to `achieved_time`, idempotent; the mechanism by
    which the compositor forms the `7/24` key before rendering. `:123-125` contract
    tests extend for time-honesty and `achieved_time` correctness — the fixture the
    new conformance case joins.
  - **`:72-79` — the `Stability` enum.** `Static` (no time axis), `Timed`
    (deterministic per time, cacheable per time), `Live` (non-deterministic). The
    predicate `quantize_time`'s default and `plan_layer`'s branch key off.
  - **`:127-137` — Pipeline changes.** `:129-131` frame planning "samples the
    transport's current composition time, then computes each layer's local time"
    — **amended by this task's delta** to add "then snaps each `Timed` layer's
    local time to that content's grid via `quantize_time` before the tile-cache
    lookup." `:133-137` clock advance is the temporal damage, orthogonal axes
    (`damage_planning`'s text; this task makes the temporal re-plan hit the cache).
  - **`:138-149` — the time-keyed cache.** `:138-140` "the key gains a time
    component for `Timed` content … `Static` content's keys are unchanged (no time
    dimension, no cache growth for stills)." `:141-149` the temporal prefetch ring
    this task feeds cadence to.
- `docs/design/00-overview.md:118-122` — the "Time and video" resolved-questions
  bullet, **amended by this task's delta** to record the `quantize_time` grid query.
- `docs/design/16-sdlc-and-quality.md`: `:14-25` claims register
  (`<doc-file-stem>#<slug>`, `// enforces:` tag); `:47-53` byte-exact CPU goldens;
  `:54-62` behavioral-counter tests ("playback of a still scene issues zero visual
  renders … Most claims-register entries about efficiency land here") — the
  zero-render assertion is behavioral, never wall-clock; `:112-118` ≥90% diff
  coverage on changed lines.
- `docs/design/17-internal-components.md`: `:53` `arbc::contract` is **Level 3**;
  `:56` `arbc::compositor` is **Level 4**, `Depends on: contract, cache`. Adding a
  defaulted method to the L3 `Content` interface and calling it from L4 respects the
  existing edge — **no new `DEPENDS` edge**; the doc-17 dependency check stays green.
- `src/contract/arbc/contract/content.hpp` — `Stability` (`:25-30`), `RenderRequest`
  (`:76-84`), `RenderResult::achieved_time` (`:98`), the `Content` interface with
  `stability()` (`:173`), `time_extent()` (`:184`, the null-defaulting rationale for
  a "conscious declaration" method — the model `quantize_time` follows for its
  *defaulted* opposite), `render()` (`:208`); the operator-graph members
  (`:216-235`) are the null-default pattern the new query mirrors.
- `src/contract/t/temporal_fields.t.cpp:116-171` — the existing `Timed`/`Static`
  achieved_time fixture the conformance case extends.
- `src/compositor/tile_planning.cpp` — the identity-bucket keying `:133-134`, the
  key uses at `:140,:162,:179`, the hard-wired `Time::zero()` at `:266-267,:288-290`,
  the fill path dropping `result.achieved_time` at `:305-306`;
  `src/compositor/arbc/compositor/tile_planning.hpp:128` (`plan_layer`), `:162`
  (`render_frame_interactive`).
- `src/compositor/arbc/compositor/damage_planning.hpp:74` — the `Static`
  "clock-invariant" invariant this task preserves.
- `src/cache/arbc/cache/key_shapes.hpp:64-76` — `TileKey`, `:153-158` the hash's
  present-vs-absent guard.
- `src/base/arbc/base/time.hpp` — `Time { std::int64_t flicks; }` (`:12`),
  `flicks_per_second = 705'600'000` (`:14`, the grid arithmetic constant),
  `Time::zero()` (`:16`), defaulted `<=>` (`:19`); `TimeRange` (`:29-54`). **No
  quantize helper exists** — the 24 fps snap the test kind computes is
  `Time{ (t.flicks / (flicks_per_second/24)) * (flicks_per_second/24) }`, an exact
  integer floor (705'600'000 is divisible by 24).
- `src/cache/arbc/cache/prefetch.hpp:49,:98-103` — the temporal ring's per-bucket
  step and the "cache neither quantizes time nor knows frame cadence" note.
- `tests/claims/registry.tsv` — 2-column TAB-separated `<claim-id>\t<description>`,
  `<doc-file-stem>#<slug>`; enforced by `scripts/check_claims.py` (bidirectional).
  Existing row to **re-assert, not re-register**:
  `11-time-and-video#static-tiles-survive-clock` (registered by `tile_planning`).
- Test conventions: unit tests `src/compositor/t/<name>.t.cpp` (Catch2,
  `arbc_component_test`); contract conformance `src/contract/t/<name>.t.cpp`;
  cross-component goldens `tests/<name>_golden.t.cpp` (links `arbc` + `CpuBackend`).
  Enforce-tag example: `src/compositor/t/tile_planning.t.cpp:142`.

## Constraints / requirements

- **Levelization (doc 17:40-44,:53,:56).** `quantize_time` is a method on the L3
  `arbc::contract` `Content`; the L4 `arbc::compositor` calls it through the
  existing `contract` edge. **No new `DEPENDS` edge, no `backend-cpu` edge**; the CI
  dependency check stays green. The contract touch is additive (a defaulted
  virtual), so `arbc::contract`'s own dependents are unaffected.
- **Backward-compatible by default (byte-exact).** `quantize_time` defaults to
  `nullopt`; the driver's `composition_time` defaults to `Time::zero()`. With both
  defaults, `plan_layer` keys exactly as today (`Static` → `nullopt`, `Timed`/`Live`
  → raw time) and `render_frame_interactive` produces byte-identical output and
  cache state. The landed `tile_planning`, `refinement`, `anchored_viewports`, and
  `damage_planning` goldens must pass unchanged.
- **`Static` invariant untouched (doc 11:139-140).** `Static` content keys
  `achieved_time == nullopt` regardless of `quantize_time` (the branch short-circuits
  on `Stability::Static`), so a still scene grows no cache across a clock advance and
  `11-time-and-video#static-tiles-survive-clock` still holds.
- **Soundness under seek.** Quantization is the *content's* grid
  (`quantize_time`), never a compositor guess or a floor-probe over cached instants.
  A scrub to any `t` keys `quantize_time(t)` = the true native frame for `t` and
  hits only if that exact frame is cached — never over-serves a skipped frame
  (Decision 1). The zero-render property is an *optimization on top of a sound key*,
  not a heuristic relaxation of correctness.
- **`quantize_time`↔`achieved_time` consistency (contract).** When
  `quantize_time(t)` returns a value it equals `render(time = t).achieved_time`, and
  it is idempotent (`quantize_time(quantize_time(t)) == quantize_time(t)`). This is
  what lets the compositor form the native-instant key *before* rendering and trust
  the render to land on it — enforced by the conformance case.
- **Pure per-frame library; state caller-owned.** `quantize_time` is a pure query
  on immutable content; the composition time is a caller-supplied value. No
  cross-frame temporal state lives in the compositor.
- **Single-threaded; no concurrency surface.** All work runs on the frame thread
  against the pinned snapshot. **No TSan obligation.**
- **CI diff coverage ≥90% (doc 16:112-118)** on changed lines — tests ship in this
  task.

## Acceptance criteria

**Design-doc delta (same commit, doc 16):**

- `docs/design/11-time-and-video.md` — the `quantize_time` bullet in §Contract
  changes and the pre-lookup snap clause in §Pipeline changes (both landed with this
  refinement).
- `docs/design/00-overview.md:118-122` — the "Time and video" resolved-questions
  bullet records the `quantize_time` grid query (landed with this refinement).

**Claims (register + `enforces:` tags)** — two new rows appended to
`tests/claims/registry.tsv`, enforced from the tests below; one existing row
re-asserted:

- **`11-time-and-video#achieved-time-coalescing-issues-zero-renders`** (new) — "A
  `Timed` layer's requested time is quantized to the content's native grid via
  `quantize_time` before the cache lookup, so a clock advance that stays within one
  native frame period re-plans to all-fresh cache hits and issues zero renders and
  zero composites; an advance across a frame-period boundary renders exactly once."
  (doc 11:110-114; doc 16:54-62 — behavioral, never wall-clock.) Enforced by the
  driver counter test in `src/compositor/t/temporal_coalescing.t.cpp`.
- **`11-time-and-video#quantize-time-matches-achieved-time`** (new) — "A content's
  render-free `quantize_time(t)`, when present, equals the `achieved_time` a render
  at `t` reports, and is idempotent — so the compositor may key a tile at the native
  instant before rendering and the render lands on that key." (doc 11 §Contract
  changes.) Enforced by the conformance case in
  `src/contract/t/temporal_fields.t.cpp` (or a new `quantize_time.t.cpp`).
- **Re-asserted (no new row):** `11-time-and-video#static-tiles-survive-clock`
  (registered by `tile_planning`) gains a re-exercise — an all-`Static` scene still
  keys `nullopt` and re-plans to fresh hits after a clock advance, unchanged by the
  quantization branch — via a `// enforces:` tag on this task's still-scene case.

**Behavioral / unit test — `src/compositor/t/temporal_coalescing.t.cpp`** (new,
Catch2, `arbc_component_test`), using a **test `Timed` content kind** that
implements `stability() == Timed`, `quantize_time(t) = floor-to-24fps`, and a
`render` whose `RenderResult::achieved_time` equals `quantize_time(request.time)`:

- **`plan_layer` quantized keying:** planning the `Timed` layer at two requested
  times inside one native frame period (e.g. `t0`, `t0 + 1/60 s` both in
  `[7/24, 8/24)`) produces **byte-identical `TileKey`s** (both `achieved_time ==
  7/24`); the second plan against a warm cache reports **all-fresh hits, zero
  misses**. Planning at `t2` in the next period (`≥ 8/24`) produces a distinct key
  (`achieved_time == 8/24`) that misses on the warm cache.
- **`Static` unchanged:** a `Static` layer keys `achieved_time == nullopt` at any
  requested time; two plans at different times produce identical keys
  (`// enforces: 11-time-and-video#static-tiles-survive-clock`).
- **Driver zero-render (counter-backed, doc 16:54-62):** drive
  `render_frame_interactive` on the `Timed` scene once at `composition_time = t0`
  (warms the cache), then again at `t0 + 1/60 s` (same native frame): the second
  frame shows `requests_issued == 0` **and** `composites == 0` for that layer
  (`// enforces: 11-time-and-video#achieved-time-coalescing-issues-zero-renders`).
  A third frame at `t2 ≥ 8/24` bumps `requests_issued` by exactly one. With
  `composition_time` defaulted (`Time::zero()`), the frame plans exactly as the
  ungated `tile_planning` run (null-path parity).
- **Identity default:** a content returning `nullopt` from `quantize_time` keys the
  raw requested time (today's behaviour) — proving the default is a no-op.

**Contract conformance — `src/contract/t/temporal_fields.t.cpp`** (extend) or new
`src/contract/t/quantize_time.t.cpp`:

- For the `Timed` test content: `quantize_time(t).has_value()` for representative
  `t`, `*quantize_time(t) == render(request{time=t}).achieved_time`, and
  `quantize_time(*quantize_time(t)) == quantize_time(t)` (idempotence)
  (`// enforces: 11-time-and-video#quantize-time-matches-achieved-time`).
- For a `Static` content: `quantize_time(t) == nullopt` for all `t` (the default is
  correct for stills).
- This case is scoped to run under the **contract conformance suite** (the contract
  stream's test target today; migrates to `arbc-testing` when it exists, doc 16).

**Golden — `tests/temporal_coalescing_golden.t.cpp`** (new, cross-component, links
`arbc` + `CpuBackend`; byte-exact, doc 16:47-53):

- **Coalesced frame is byte-identical to the rendered frame.** Render the `Timed`
  scene at `t0` into a persisted `target`; advance `composition_time` to
  `t0 + 1/60 s` (same native frame) and re-render: the `target` is **byte-identical**
  to the first frame (the cached native-frame tiles are reused, no re-decode, no
  re-composite drift), and the counters show zero renders. This is the temporal
  analog of `damage_planning`'s quiescent-frame golden.

**Golden regression (no new golden):** the landed `tile_planning`, `refinement`,
`anchored_viewports`, and `damage_planning` goldens pass unchanged, confirming the
defaulted `quantize_time`/`composition_time` path is byte-exact.

**No deferred WBS follow-up.** This task closes the achieved-time coalescing debt in
full; nothing is deferred to a new leaf. The real video kind's own `quantize_time`
implementation rides with that kind (`55-kinds`, existing WBS), not a new task here.

## Decisions

1. **Coalescing requires a render-free content grid query; the existing seams
   provably cannot deliver zero-render on a sub-frame advance.** The compositor
   must key a `Timed` tile at the native instant `7/24` *before* it renders, so a
   second request in the same period hits. It cannot derive `7/24`'s bucket
   `[7/24, 8/24)` from the post-render `achieved_time`: one render at `t=0.31`
   reveals only the floor; a floor-probe over cached instants over-serves a skipped
   frame under seek (cache has frames 6 and 8, a scrub to `7.5/24` wrongly serves 6);
   insert-side re-keying by `RenderResult::achieved_time` gives cache *dedup* but
   not lookup-time hits (the lookup at the raw time still misses, so a render still
   fires — not zero renders). Only the content knows its cadence, so the sound,
   zero-render, seek-correct mechanism is a pure `Content::quantize_time(Time)`
   query the compositor applies at plan time. This *is* a new seam, in tension with
   "reuse existing seams" — but it is the **minimal** one (a single defaulted
   virtual, `nullopt` = today), and the promised behavioral property is
   unreachable without it. *Rejected:* floor-probe over cached achieved_times —
   unsound under seek (byte-exactness is the project's floor, not a heuristic).
   *Rejected:* insert-side re-keying alone — prevents cache growth but yields zero
   coalescing hits, since the lookup key is still raw-timed. *Rejected:* the
   compositor inferring cadence from two consecutive achieved_times — fragile
   (non-uniform cadence, single-sample bootstrap) and still needs a first miss per
   bucket.

2. **`quantize_time` is a defaulted virtual returning `nullopt`, not a required
   one.** `Content::time_extent()` is deliberately *non*-defaulted (doc 03:77,
   `content.hpp:174-184`) because a silent default would misclassify a `Timed`
   content as timeless and serve it stale. `quantize_time` is the opposite: its
   `nullopt` default is *safe* — it means "use the requested time as-is," which is
   today's exact behaviour, sound for every content (an un-migrated `Timed` content
   simply coalesces nothing, never renders wrong pixels). So it is null-defaulted
   like the operator-graph members (`content.hpp:216-235`), keeping every landed
   kind and golden byte-unchanged and letting only content that *can* quantize opt
   in. *Rejected:* a required pure virtual — forces churn on every kind and the
   whole conformance suite for an optimization only `Timed` decoder content uses,
   and offers no safety gain (the default is already sound).

3. **Key by `quantize_time` at plan time; do not re-key the fill by
   `RenderResult::achieved_time`.** The conformance contract guarantees
   `quantize_time(t) == render(time=t).achieved_time`, so the plan-built key
   *already* equals the instant the render will report — the fill can keep storing
   under the plan key (`tile_planning.cpp:305-306`) with no post-render re-key, and
   the async-arrival insert (`refinement.cpp:85-91`) needs no change. This keeps a
   single keying site (`plan_layer`) and leaves the landed fill/arrival paths
   untouched. *Rejected:* storing under the raw key then re-keying to `achieved_time`
   on fill — two keying sites that can disagree, an extra cache write, and it does
   nothing the pre-lookup quantization does not already do (and it would not produce
   lookup hits, per Decision 1).

4. **The driver threads a composition `Time` value; it does not own a clock.** To
   exercise coalescing the driver must plan at a real requested time, replacing the
   walking-skeleton `Time::zero()` (`tile_planning.cpp:266,:288`). It takes a
   trailing defaulted `Time composition_time = Time::zero()` **value** — the same
   caller-owned, stateless shape as `damage_planning`'s `DirtyRegion*` and
   `refinement`'s `RefinementQueue*`. Sampling the transport clock and advancing it
   is `runtime.interactive`'s (doc 11:129, doc 17:60); the compositor consumes the
   sampled value. The default keeps every landed caller byte-identical. *Rejected:*
   a `Transport`/clock object on the compositor — cross-frame mutable state at L4,
   duplicating the runtime's transport, and untestable without a loop. *Rejected:*
   leaving `Time::zero()` hard-wired and testing only `plan_layer` — the behavioral
   zero-render claim (doc 16:54-62) must be pinned at the driver with counters, which
   needs a real time in.

5. **`Live` content does not coalesce (returns `nullopt`).** `Live` is
   non-deterministic and cacheable only within a snapshot (doc 11:77-78); reusing a
   `Live` tile across two times would serve stale non-deterministic pixels. So `Live`
   keeps the identity bucket (raw time via the `nullopt` default), exactly as
   `tile_planning` shipped. Only `Timed` (deterministic per time) is safe to
   coalesce. *Rejected:* coalescing `Live` on a grid — unsound (a running simulation
   at `t0+ε` is not the frame at `t0`).

6. **24 fps floor-quantization in the test kind; no quantization helper on `Time`.**
   The test `Timed` kind computes `Time{ (t.flicks / step) * step }` with
   `step = flicks_per_second / 24` — an exact integer floor because
   `705'600'000 % 24 == 0` (the flicks base is chosen to divide common frame rates).
   No general quantize helper is added to `base/Time` — the cadence is content
   knowledge, not a base primitive, and a real video kind derives `step` from its
   stream's rate. *Rejected:* a `Time::quantize(rate)` in `base` — pushes
   frame-cadence policy into the base layer, which the prefetch doc already says
   "neither quantizes time nor knows frame cadence" belongs above base.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-06.

- Added `virtual std::optional<Time> quantize_time(Time) const` (defaulted `nullopt`) to `Content` interface in `src/contract/arbc/contract/content.hpp` — the render-free grid query seam.
- Replaced identity-bucket keying in `plan_layer` with `content->quantize_time(time).value_or(time)` in `src/compositor/tile_planning.cpp`; `Static` branch unchanged (`nullopt`).
- Added `const Content* content_ptr = nullptr` to `plan_layer` and `Time composition_time = Time::zero()` to `render_frame_interactive` in `src/compositor/arbc/compositor/tile_planning.hpp`; threaded real composition time through driver and miss request.
- New unit/counter test file `src/compositor/t/temporal_coalescing.t.cpp`: plan_layer coalescing, Static-invariant, identity-default, driver zero-render, quiescent-frame zero-composite cases.
- New cross-component golden `tests/temporal_coalescing_golden.t.cpp`: byte-identical coalesced frame, zero-render counters.
- Extended `src/contract/t/temporal_fields.t.cpp` with `quantize_time` on `TimedContent` plus two conformance cases (`quantize_time == achieved_time`, idempotence).
- Updated `src/compositor/CMakeLists.txt` and `tests/CMakeLists.txt` to wire the new test targets; appended 2 new claim rows to `tests/claims/registry.tsv`.
- Updated `docs/design/00-overview.md` and `docs/design/11-time-and-video.md` with the `quantize_time` grid-query delta and pre-lookup snap clause.
