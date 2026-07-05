# contract.temporal_fields — Temporal Contract Fields

## TaskJuggler entry

`tasks/25-contract.tji:21-25`:

```
task temporal_fields "Temporal contract fields" {
  effort 1d
  allocate team
  note "time honored end to end, achieved_time reporting, time_extent(), three-way stability driving cacheability. Doc 11."
}
```

The `Refinement:` note back-link and `complete 100` are added by the closer
when the task lands (see `tasks/refinements/README.md` task-completion
ritual).

## Effort estimate

**1d** (`tasks/25-contract.tji:22`). Same weight class as
`contract.snapshot_pins` (1d) and `contract.operator_members` (1d): a
contract-surface task that adds one `RenderResult` field, one pure-virtual
description method on `Content` (plus a minimal L0 value type it needs),
migrates the handful of existing `Content` implementors, and pins the
behavior with self-contained deterministic unit tests. Lighter than
`contract.async_render` (3d), which changed the load-bearing `render`
signature and added the async completion protocol.

## Inherited dependencies

**Settled:**

- `model.editable_facet` and `surfaces.capabilities` — the parent
  `task contract` edges (`tasks/25-contract.tji:7`). No direct `depends` of
  its own.

**Sibling substrate (not a formal `depends`, but landed and relied on):**

- `contract.async_render` (`92c3d3b`, DONE 2026-07-05,
  `tasks/refinements/contract/async_render.md`) — established the single
  load-bearing `render` virtual and the "every task migrates all
  implementors + the sole producer" invariant. It **explicitly reserved**
  `achieved_time` and `time_extent()` for this task and kept `RenderResult`
  at `{achieved_scale, exact}` (`async_render.md:282-286`, Constraint 6;
  `async_render.md:128-129,149-154`).
- `contract.snapshot_pins` (`1da702a`, DONE 2026-07-05,
  `tasks/refinements/contract/snapshot_pins.md`) — added the
  `StateHandle snapshot` field to `RenderRequest`; establishes the
  "cheap by-value descriptor" invariant and the claim-anchoring convention.
- `contract.operator_members` (DONE 2026-07-05,
  `tasks/refinements/contract/operator_members.md`) — added the
  operator-graph virtuals with **defaults**, and confirmed the three-way
  `Stability` enum already lives in `content.hpp`.

**Downstream (this task unblocks them):**

- `contract.conformance_suite` (`tasks/25-contract.tji:42-47`,
  `depends !async_render, !temporal_fields, !operator_members`) — the
  property-based public suite ships the "scale/time honesty" and
  "bounds/extent honesty" properties over arbitrary content.
- `time.timeline` and all its children (`tasks/40-time.tji:3`,
  `depends model.transactions, contract.temporal_fields`) — the whole time
  and video stream (`rational_time`, `temporal_placement`, `transport`,
  `temporal_cache`, `playback_hints`) sits on top of the contract fields
  this task lands. In particular `time.temporal_cache`
  (`tasks/40-time.tji`, achieved_time coalescing) consumes the reported
  `achieved_time`.

## What this task is

Land the temporal surface of the layer contract that the walking-skeleton
subset deferred: (1) add the `std::optional<Time> achieved_time` field to
`RenderResult` so content reports the local time it actually rendered; (2)
add the pure-virtual `std::optional<TimeRange> time_extent()` description
method to `Content` and migrate every existing implementor; (3) introduce
the minimal `TimeRange` half-open interval value type in `arbc::base` that
`time_extent()` returns; and (4) formalize, in the contract surface and its
doc-comments, that the already-present three-way `Stability` enum drives
cacheability — `Static` ignores `time` and reports no `achieved_time`
(no cache-time dimension), `Timed` reports the quantized `achieved_time`
(cacheable per time), `Live` is cacheable only within a frame/snapshot.
The `RenderRequest.time` field already exists (`content.hpp:72`); this task
makes the reporting-and-extent half of the contract concrete and pins the
time-honesty behavior with deterministic unit tests over concrete test
`Content`.

## Why it needs to be done

`RenderRequest.time` has existed since the walking skeleton, but the
contract has had no way for content to *report* what time it rendered
(`achieved_time`) or *declare* its temporal validity (`time_extent()`),
and no contract-level statement that stability governs cacheability. Doc 11
sequences the whole time-and-video effort as "**contract fields and model
first**, transport and temporal cache next, reference kind last"
(`docs/design/11-time-and-video.md:212-232`); this task is the "contract
fields" stage. It gates `contract.conformance_suite` (the "scale/time
honesty" property) and the entire `time.timeline` stream, which cannot
compose time maps into `RenderRequest.time`, coalesce on `achieved_time`,
or cull on temporal extent until the contract carries these fields.

## Inputs / context

**Design docs (`docs/design/16-sdlc-and-quality.md` normative, doc 16):**

- `docs/design/11-time-and-video.md` — the primary source (WBS note cites
  it):
  - Symmetry mapping (`:15-25`): `RenderRequest.time` is the temporal analog
    of `region`/`scale`; `achieved_time` is the analog of `achieved_scale`
    ("video bottoms out at native frame times", `:22`).
  - Time model (`:32-48`): composition time is continuous; frame rate is a
    property of the render, not the model. Instants are integer flicks
    (`1/705,600,000 s`), rates are exact rationals composed per edge.
  - `Content` temporal metadata (`:67-79`): `time_extent()` is the temporal
    `bounds()` — "optional local-time range where the content varies /
    exists ... static content reports 'none'."
  - Three-way `Stability` (`:72-79`): `Static` (time-invariant), `Timed`
    (deterministic function of request time — cacheable per time), `Live`
    (non-deterministic — cacheable only within a frame/snapshot).
  - Contract changes (`:87-124`): `RenderRequest.time` is content-local time
    composed through the time map; `RenderResult` gains
    `std::optional<Time> achieved_time` — "the local time actually rendered,
    if quantized." `achieved_time` **is** the temporal `achieved_scale`
    (`:110`); a 24 fps clip asked for `t=0.31 s` renders `7/24 s` and says
    so (`:111-112`); the compositor then serves every request in
    `[7/24, 8/24)` from that entry (`:112-114`).
  - Pipeline changes (`:128-148`): clock advance is temporal damage; the
    tile-cache key gains a time component **for `Timed` content only** —
    `Static` keys are unchanged (`:138-143`); the key carries
    **achieved_time**, not requested time.
- `docs/design/03-layer-plugin-interface.md` — the contract surface:
  - `Stability` enum sketch (`:33-38`).
  - `RenderRequest.time` (`:45-46`): "content-local time ...; Static content
    ignores it."
  - `RenderResult.achieved_time` (`:56-58`): "local time actually rendered,
    if quantized ... the temporal achieved_scale." `RenderResult.provided`
    (`:59`) is a **separate** field, surfaces scope (see Decisions).
  - `Content::time_extent()` (`:77-78`): shown as a **pure virtual** `= 0`,
    deliberately grouped with the other pure-virtual description methods
    `bounds()`/`scale_range()`/`stability()` (`:74-77`), *not* with the
    defaulted operator-graph methods (`:98-102`).
  - Hot-struct rule (`:159-162`): the by-value descriptors stay plain data.
- `docs/design/15-memory-model.md:21` — the time-keyed tile cache is a
  backend-owned, budgeted, LRU cache **value**; time is *not* part of the
  version-structured arenas. "revisions track edits, time tracks playback"
  (doc 11) — this task adds no arena/pin obligation.
- `docs/design/16-sdlc-and-quality.md` — claims register (`:9-26`), contract
  conformance suite properties (`:31-44`, incl. "scale/time honesty"), unit
  tier ("rational time", `:45-46`), behavioral-counter tier (`:54-62`),
  definition of done (`:138-142`).
- `docs/design/17-internal-components.md` — levelization: `arbc::base` (L0)
  owns `Time`/`TimeRange`/rational rates (`:48`); `arbc::contract` (L3) owns
  `RenderRequest`/`RenderResult`/`Stability`/`Content` (`:53`); `arbc::cache`
  (L3) owns the time-keyed cache (`:54`).

**Source seams:**

- `src/base/arbc/base/time.hpp:10-19` — `struct Time { std::int64_t flicks; }`
  with `flicks_per_second = 705'600'000`, `zero()`, `==`/`<=>`. **No
  `TimeRange` type exists yet** (the only reference is the doc 03 sketch,
  `03-layer-plugin-interface.md:77`). This task adds it here.
- `src/contract/arbc/contract/content.hpp:19-23` — `enum class Stability {
  Static, Timed, Live }` already present, `// Cacheability ... (docs 01/11)`.
- `src/contract/arbc/contract/content.hpp:69-77` — `RenderRequest`; the
  `Time time;` field is `content.hpp:72`.
- `src/contract/arbc/contract/content.hpp:79-82` — `RenderResult` is
  currently `{ double achieved_scale{1.0}; bool exact{true}; }`. **No time
  field yet** — `achieved_time` is added here.
- `src/contract/arbc/contract/content.hpp:149-207` — `class Content`;
  pure-virtual description methods `bounds()` (`:155`), `stability()`
  (`:156`); `render()` (`:180-181`); defaulted operator-graph virtuals
  (`:188-207`). `time_extent()` is net-new, added among the description
  methods.
- `src/contract/content.cpp:5,11,15` — out-of-line `~Content` and the
  defaulted operator-graph bodies (no default for the new pure virtual).
- `src/kind_solid/arbc/kind_solid/solid_content.hpp:20` — `SolidContent`
  (the only production `Content`; `Static`). Implementors to migrate:
  `SolidContent`, plus the test doubles in
  `src/contract/t/snapshot_pins.t.cpp:51,72`,
  `src/contract/t/operator_members.t.cpp:34,48`,
  `src/contract/t/async_render.t.cpp:67,83`, and
  `tests/tile_planning_golden.t.cpp:33`.
- `src/cache/arbc/cache/key_shapes.hpp:64-76` — `struct TileKey` already
  carries `std::optional<Time> achieved_time;` (`:69`), present for `Timed`,
  absent for `Static`.
- `src/compositor/tile_planning.cpp:122-132` — the compositor already
  *derives* `achieved_time` from `stability()` when building the `TileKey`.
  Reconciling this with the newly *reported* `RenderResult.achieved_time` is
  downstream (see Decisions / Acceptance criteria).
- `src/compositor/t/tile_planning.t.cpp:308-325` — existing "Static keys omit
  achieved_time and survive a clock advance" test (cache-side; not this
  task's).
- `src/contract/CMakeLists.txt:1-8` — component definition
  (`DEPENDS base media surface model`) and the `arbc_component_test` list the
  new `t/temporal_fields.t.cpp` joins.
- `tests/claims/registry.tsv` — TAB-separated `<claim-id>\t<description>`;
  ids are `<doc-file-stem>#<slug>`; tests tag `// enforces: <claim-id>`;
  gated both directions by `scripts/check_claims.py`.

**Predecessor decisions carried forward:** claims anchor to doc 03 (the
render contract), citing the motivating doc for the *why*
(`snapshot_pins.md:280-284`, `async_render.md:457-461`,
`operator_members.md:368-374`); the task's own tests cover concrete
deterministic `Content` only, deferring the property-based version to the
conformance suite; `RenderResult` and the requests stay cheap by-value
descriptors — no allocation, refcount, or atomic on the render path.

## Constraints / requirements

1. **`RenderResult` gains exactly `std::optional<Time> achieved_time`**
   (`content.hpp:79-82`), matching the doc 03 sketch (`:56-58`).
   `RenderResult.provided`/`SurfaceRef` (doc 03 `:59`) stays **out** — it is
   surfaces scope, deferred by `async_render` (`async_render.md:282-286`).
   `nullopt` means "honored the requested time exactly / time-invariant"
   (the parallel of `achieved_scale == request.scale`).
2. **`Content::time_extent()` is a pure virtual** `virtual
   std::optional<TimeRange> time_extent() const = 0;`, placed among the
   description methods beside `bounds()`/`scale_range()`/`stability()`
   (`content.hpp:155-156`), matching doc 03 `:77-78`. It is **not**
   null-defaulted like the operator-graph members (see Decisions). Every
   existing implementor is migrated; `Static` content returns `std::nullopt`,
   a `Timed` test double returns its duration as a `TimeRange`.
3. **`TimeRange` is a minimal half-open `[start, end)` interval of `Time`,
   added to `arbc::base`** (`src/base/arbc/base/time.hpp`), per doc 17 `:48`.
   Trivially copyable, no STL members, defaulted `==`; the rational
   rate/time-map arithmetic is *not* part of it (that is `time.rational_time`,
   `tasks/40-time.tji`).
4. **The three-way `Stability` → cacheability linkage is documented on the
   contract surface**, not re-implemented: `Static` ignores `time` and
   reports no `achieved_time`; `Timed` reports the quantized `achieved_time`
   (cacheable per achieved_time); `Live` is cacheable only within a
   frame/snapshot. The enum and its cache-key wiring already exist
   (`content.hpp:19-23`, `key_shapes.hpp:69`,
   `tile_planning.cpp:122-132`) — this task pins the contract-level invariant
   with doc-comments and tests, and does not change the compositor.
5. **Cheap by-value descriptor preserved** (`snapshot_pins.md:166-169`,
   `async_render.md:253-255`): `std::optional<Time>` over the trivially
   copyable `Time` adds no allocation, refcount, or atomic to `RenderResult`;
   `TimeRange` is likewise a plain value.
6. **Media time vs wall clock kept distinct** (`async_render.md:396-421`):
   `Time`/`TimeRange`/`achieved_time` are content-local media time in flicks
   — never a wall clock. `Deadline` (`content.hpp:49-56`) remains the only
   `steady_clock` budget; the contract carries values only, reads no clock.
7. **Levelization** (`docs/design/17-internal-components.md:48,53`;
   `scripts/check_levels.py`): `TimeRange` in `base` (L0), the contract
   fields in `contract` (L3, `contract → base`). No new edge to `cache`,
   `compositor`, or any kind.
8. **Gates green** (doc 16): `scripts/gate` (build + asan + `check_levels` +
   `check_claims`) passes; ≥90% diff coverage on changed lines.

## Acceptance criteria

- **`RenderResult::achieved_time` and `Content::time_extent()` land**, with
  `TimeRange` in `arbc::base`, and every `Content` implementor
  (`SolidContent` + the six test doubles listed under Inputs) compiles
  against the new pure virtual. `scripts/gate` green.
- **Unit tests** in a new `src/contract/t/temporal_fields.t.cpp` (Catch2,
  registered in `src/contract/CMakeLists.txt`) over a concrete `Static` test
  `Content` and a concrete `Timed` test `Content`:
  - `Timed` content returns identical pixels/result for identical
    `request.time`, renders a deterministic function of time, and reports the
    quantized local time via `achieved_time`.
  - `Static` content produces identical output regardless of `request.time`
    and reports `achieved_time == std::nullopt` and
    `time_extent() == std::nullopt`.
  - `time_extent()` on `Timed` content returns its half-open duration; the
    `TimeRange` interval semantics (`[start, end)`, emptiness, ordering) are
    exercised directly.
- **Claims-register growth** (`tests/claims/registry.tsv`,
  `scripts/check_claims.py`), anchored to doc 03 and cited from
  `temporal_fields.t.cpp` via `// enforces:` (descriptions single-line,
  present tense, ASCII):
  - `03-layer-plugin-interface#render-time-honest` — "Timed content renders a
    deterministic function of request time and reports the achieved_time it
    rendered"
  - `03-layer-plugin-interface#static-time-invariant` — "Static content
    ignores request time and reports nullopt achieved_time and nullopt
    time_extent so it adds no time dimension to the cache key"
  - Do **not** re-register the cache/architecture-owned claims
    `11-time-and-video#tile-key-carries-time-and-revision` and
    `02-architecture#tile-cache-key-and-value-shape` — this task's
    contract-surface claims feed them, not duplicate them.
- **Concurrency: none added.** The work is value types plus a description
  virtual; no shared mutable state, no new threading. Stated explicitly so
  the closer does not scope TSan/stress work here.
- **Deferred (owners already WBS leaves — no new task):**
  - The **property-based "scale/time honesty" over arbitrary content** is
    `contract.conformance_suite` (`tasks/25-contract.tji:42-47`, already
    `depends !temporal_fields`). This task ships concrete-content unit tests
    only. No new leaf.
  - **Compositor consumption of the reported `achieved_time`** — reconciling
    `RenderResult.achieved_time` with the value the compositor currently
    *derives* into the `TileKey` (`tile_planning.cpp:122-132`), so
    achieved-time coalescing keys on what content actually rendered — is
    `time.temporal_cache` (`tasks/40-time.tji`, already `depends
    !rational_time, cache.key_shapes`; `timeline` already `depends
    contract.temporal_fields`). Out of this L3 task's scope. No new leaf.
  - The **`Timed` reference kind** (`org.arbc.imageseq`, doc 03 `:184`) that
    exercises real quantization is a later kinds-stream leaf; this task
    exercises `Timed` behavior via a test double. Out of scope. No new leaf.
  - The **model `span`/`time_map` and per-edge rational time maps** that
    compute `RenderRequest.time` are `time.temporal_placement` and
    `time.rational_time` (`tasks/40-time.tji`), downstream of this task. No
    new leaf.
- **No under-registered follow-ups.** Every deferral above maps to an
  existing WBS leaf; this task registers no new task and needs no
  design-doc delta.

## Decisions

- **`time_extent()` is a pure virtual, not a null-default.** Doc 03
  deliberately groups it with the pure-virtual description methods
  `bounds()`/`scale_range()`/`stability()` (`03-layer-plugin-interface.md:74-78`),
  separate from the defaulted operator-graph methods (`:98-102`). Every
  `Content` consciously declares its temporal extent, exactly as it declares
  its spatial bounds. *Rationale:* a null-default returning `nullopt` would
  silently classify an un-migrated `Timed` content as time-invariant — a
  correctness hazard (its tiles would never carry an achieved-time key and
  would be served stale across a clock advance). The compiler forcing every
  author to answer is the safer contract, and doc 11's "static reports none"
  (`11-time-and-video.md:69-71`) is a one-line `return std::nullopt;` for the
  common `Static` case. *Rejected — null-default virtual:* matches the
  `operator_members` pattern but that pattern exists for members whose
  *absence* is the correct behavior for leaf content; temporal extent's
  correct answer depends on the content's kind and must be declared.
  *No design-doc delta:* doc 03's sketch already shows both `achieved_time`
  (`:56-58`) and the pure-virtual `time_extent()` (`:77-78`) — the code is
  catching up to the doc, altering no designed behavior, so doc 16's
  same-commit amend rule is not triggered.
- **`TimeRange` lands in `arbc::base` (L0), minimally.** Doc 17 places
  `Time`/`TimeRange` in `base` (`:48`), and `time_extent()` needs the type
  *now* while the entire `time` stream (`time.rational_time` included)
  *depends on* this task (`tasks/40-time.tji:3`) — so the type cannot come
  from `rational_time`. *Rationale:* a half-open `[start, end)` pair of
  `Time` is the honest minimal slice; the rational rate/time-map arithmetic
  stays with `time.rational_time`, which builds on this type. *Rejected —
  defining `TimeRange` in `contract`:* violates levelization (time types are
  L0 `base`, not L3 `contract`) and would force `time.rational_time` to
  depend on `contract` for a value type.
- **Only `achieved_time` is added to `RenderResult`; `provided`/`SurfaceRef`
  stays out.** *Rationale:* `async_render` explicitly reserved
  `achieved_time` for this task and `provided` for surfaces scope
  (`async_render.md:282-286`); keeping the split honors the established
  ownership and keeps `RenderResult` a cheap by-value descriptor.
- **`nullopt achieved_time` is the "honored exactly / time-invariant"
  signal**, mirroring `achieved_scale == request.scale`. *Rationale:*
  reusing `std::optional`'s empty state avoids a sentinel `Time` and matches
  the temporal-`achieved_scale` framing of doc 11 (`:110`); it is also what
  the existing `TileKey.achieved_time` optional (`key_shapes.hpp:69`) already
  encodes for `Static` content.
- **Claims anchored to doc 03, not doc 11.** Following the convention in all
  three sibling refinements (`snapshot_pins.md:280-284`,
  `async_render.md:457-461`, `operator_members.md:368-374`): the claims
  constrain observable `Content::render`/description behavior, which lives in
  the layer-plugin-interface doc; doc 11 supplies the *why* and is cited as
  motivation. This task makes the "11" that `operator_members.md:373`
  anticipated concrete.
- **The compositor is not touched.** The `Stability` → cacheability wiring
  already exists in `cache`/`compositor`; this task lands the contract-surface
  *reporting* channel and *description* method and documents the invariant.
  Reconciling the reported vs derived `achieved_time` is `time.temporal_cache`
  (see Acceptance criteria). *Rationale:* keeps the L3 contract task within
  its component boundary and its 1d weight; the coalescing behavior is an L4
  concern with its own leaf.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-05.

- `src/base/arbc/base/time.hpp` — added `TimeRange` half-open `[start,end)` value type (L0) alongside the existing `Time` struct.
- `src/contract/arbc/contract/content.hpp` — added `RenderResult::achieved_time` (`std::optional<Time>`), pure-virtual `Content::time_extent()`, and `Stability`→cacheability doc-comments.
- `src/kind_solid/arbc/kind_solid/solid_content.hpp` + `src/kind_solid/solid_content.cpp` — migrated `SolidContent` to implement `time_extent()` returning `std::nullopt` (Static).
- `src/contract/t/snapshot_pins.t.cpp`, `src/contract/t/async_render.t.cpp`, `src/contract/t/operator_members.t.cpp`, `tests/tile_planning_golden.t.cpp` — migrated the six test doubles to implement `time_extent()`.
- `src/contract/t/temporal_fields.t.cpp` (new) — 4 unit test cases: Timed time-honesty, Static time-invariance, Timed extent, TimeRange interval semantics; Catch2 `[` in title fixed to avoid tag-parser collision.
- `src/contract/CMakeLists.txt` — registered `temporal_fields.t.cpp` in the component test list.
- `tests/claims/registry.tsv` — added `03-layer-plugin-interface#render-time-honest` and `03-layer-plugin-interface#static-time-invariant`.
