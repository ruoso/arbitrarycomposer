# timeline.temporal_placement — Temporal placement in the model

## TaskJuggler entry

Defined as `task temporal_placement` in
[`tasks/40-time.tji`](../../40-time.tji) (`tasks/40-time.tji:12-17`), under
the `timeline` parent (`tasks/40-time.tji:3`). One-line note verbatim
(`tasks/40-time.tji:16`):

> "span + time_map on layer records beside the spatial transform; default
> (-inf, +inf) keeps stills the degenerate case. Docs 01/11."

## Effort estimate

**1d** (`tasks/40-time.tji:14`). This is a small, additive model-layer task:
two POD fields plus their transactional setters and defaults, on the seam the
predecessor `timeline.rational_time` already opened and the model-record
`flags` comment already reserved. No new component, no new dependency edge, no
new base type — the base machinery (`TimeRange`, `TimeMap`) already exists.

## Inherited dependencies

- **`timeline.rational_time`** — **settled** (landed `3f271f7`; refinement
  `tasks/refinements/timeline/rational_time.md`, Status Done). It is the
  immediate predecessor sibling and the direct `depends !rational_time`
  (`tasks/40-time.tji:15`). Supplies, in the L0 `arbc::base` component, the
  exact types this task stores on layer records:
  - `TimeMap` — per-edge affine time map `{ Time in; Rational rate; Time
    offset; }` (`src/base/arbc/base/rational_time.hpp:108-118`), the concrete
    realization of doc 11's `local_time = (parent_time − in) × rate + offset`.
  - `Rational` — exact reduced rational rate, default `{1,1}`
    (`src/base/arbc/base/rational_time.hpp:36-100`).
  - `present_in_span(const TimeRange& span, Time parent_time)` — the half-open
    span-culling predicate (`src/base/arbc/base/rational_time.hpp:163-165`),
    with the settled convention that the default all-present span is
    `TimeRange::all()` and a degenerate span (`out <= in`) is present at no
    instant (comment `src/base/arbc/base/rational_time.hpp:158-162`).
  - `Time` (integer flicks) and `TimeRange` (half-open `[start, end)`, the
    span type — there is deliberately **no** separate `Span` type;
    `TimeRange::all()` is `[int64_min, int64_max)`)
    (`src/base/arbc/base/time.hpp:11-20`, `:29-54`, `all()` at `:38-41`).
- **`model.transactions`** — **settled transitively** (the versioned-model
  substrate; refinements under `tasks/refinements/model/`, all landed on
  `main`). Provides `Model::Transaction`, the path-copy record mutators
  (`set_transform`, `set_opacity`, `set_working_space`), commit/coalesce,
  atomic publish, and the reserved placement-flag room on `LayerRecord`
  (`src/model/arbc/model/records.hpp:37-39`). It is the `timeline` parent's
  `depends model.transactions` (`tasks/40-time.tji:4`) reached transitively.

No **pending** inherited dependencies — both edges are landed.

## What this task is

Give a **layer instance** its temporal placement, as doc 01/11 promise it
sits: two fields on `LayerRecord` beside the spatial `Affine transform` —

1. a `TimeRange span` (half-open `[in, out)` in parent-composition time,
   default `TimeRange::all()` = `(−∞, +∞)`), and
2. a `TimeMap time_map` (the 1D affine map from parent time to content-local
   time, default identity),

plus the two transactional setters (`set_span`, `set_time_map`) that mutate
them through `Model::Transaction` by the existing path-copy idiom, and the
default-member-initializer / aggregate-init wiring that makes a layer created
with no temporal placement a **still**: always-present span, identity time
map. This is the model-storage half of doc 11's symmetry principle — the
temporal sibling of the already-stored spatial `transform`. It does **not**
compose time maps down the tree or cull by span at frame time — those live in
`timeline.transport` and the pull-service pipeline downstream.

## Why it needs to be done

Doc 11's entire time design is "add *when* to the question, additively"
(`docs/design/11-time-and-video.md:27-30`): every downstream temporal feature
reads a layer's `span` and `time_map`. `timeline.transport`
(`tasks/40-time.tji:18-23`, `depends !temporal_placement`) samples a
per-viewport clock and needs the per-layer placement to compose local times;
`timeline.temporal_cache` keys entries on `achieved_time`; frame planning
"computes each layer's local time by composing time maps down the tree"
(`docs/design/11-time-and-video.md:151-156`). None of that has a place to read
from until the fields exist on the record. This task lands the storage and the
edit surface; it is the pivot between the base time arithmetic (predecessor)
and the transport/pipeline consumers (successors).

## Inputs / context

Design docs (normative — doc 16's executable-spec discipline):

- `docs/design/11-time-and-video.md`
  - § *The symmetry principle* (lines 8–30) — the placement correspondence is
    load-bearing: "Placement: 2D affine transform | Temporal placement: span +
    1D affine time map" (`:18`) and "Culling: outside visible region → skip |
    Culling: outside span → skip" (`:21`). Framing: adding *when* is additive,
    "Nothing structural breaks" (`:27-30`).
  - § *Model changes* (lines 59–93) — the normative field definitions.
    Lead (`:60`): "**Layer instance** (doc 01) gains temporal placement beside
    the spatial one." `span` (`:61-64`): "half-open interval [in, out) in
    parent-composition time during which the layer exists. Default (−∞, +∞): a
    still image is simply a layer that is always present — stills are the
    degenerate case, not a special one." `time_map` (`:65-71`):
    "`local_time = (parent_time − in) × rate + offset`, with `rate` a rational,
    negative allowed (reverse playback)." Culling (`:73-74`): "Outside its span
    a layer is culled, exactly like content outside the visible region."
  - § *Time model* (lines 32–56) — instants are integer flicks, rates exact
    rationals, composed once and rounded at the leaf; the model stores unrounded
    exact operands (this task stores them; it does not evaluate).
  - § *Pipeline changes* (lines 150–162) and § *Recursion* (lines 182–199) —
    downstream consumers: frame planning composes time maps down the tree; a
    nested composition's temporal placement is carried by the **referencing
    layer** (time remapping "falls out of nesting"), which is why the field
    lives on `LayerRecord`, not `CompositionRecord`.
- `docs/design/01-core-concepts.md`
  - § *Layer instance* (lines 21–42) — a layer instance is content + placement;
    the placement list carries `transform` (`:26-27`, the 2D affine) and,
    beside it, "**temporal placement**: a time `span` on the parent's timeline
    and a rate map from parent time to content-local time (doc 11) — shared by
    the visual and audio facets" (`:33-36`). This pins *where* the fields sit:
    the same placement bag as `transform`, on the layer instance.
  - § *Content and layer kinds* (lines 44–77) — content-side stability
    (`Static`/`Timed`/`Live`) and `time_extent()` are a **content/contract**
    concern, explicitly out of this task's scope (see Decisions).
- `docs/design/17-internal-components.md` — levelization. `arbc::base` is L0
  and owns "`Time`/rational rates/time maps" (`:48`); `arbc::model` is L2 with
  dependencies "base, pool, media" (`:52`); "A component may depend only on
  strictly lower levels" (`:41`). The `model → base` edge already exists.
- `docs/design/16-sdlc-and-quality.md` — testing taxonomy: byte-exact where
  deterministic; behavioral counters, never wall-clock (`:54-62`); claims
  register with `enforces:` tags gated both directions by
  `scripts/check_claims.py`.

Source seams this task extends (all current at `HEAD`):

- `src/model/arbc/model/records.hpp:61-68` — `LayerRecord`; the spatial
  `Affine transform{}` field is `:63`. The new `span`/`time_map` fields sit
  beside it. `#include <arbc/base/transform.hpp>` is already present (`:4`);
  add `#include <arbc/base/rational_time.hpp>` (which transitively pulls
  `time.hpp`) here.
- `src/model/arbc/model/records.hpp:37-39` — the reserved flag comment
  ("Room is left for the remaining placement flags (span/time-map presence,
  etc.) the timeline tasks add"). See Decision 4 — this task does **not**
  consume a presence bit; the degenerate defaults encode "no placement".
- `src/model/arbc/model/records.hpp:138-152` — the `LayerRecord` /
  `ObjectRecord` standard-layout / trivially-destructible / trivially-copyable
  `static_assert`s the new fields must keep passing (`TimeRange` and `TimeMap`
  are POD, so they do). The union-arm size invariant `sizeof(LayerOrderChunk)
  <= sizeof(CompositionRecord)` (`:147`) is unaffected (it does not touch the
  layer arm), but note growing `LayerRecord` may enlarge the `ObjectRecord`
  size class — accounted, not a violation.
- `src/model/model.cpp:518-543` — `add_layer`; line 531 aggregate-inits
  `LayerRecord{content, transform, opacity, k_layer_visible}` (see Decision 3 —
  prefer default member initializers over extending this brace list).
- `src/model/model.cpp:633-663` — `set_transform`; the path-copy mutator
  template (`nr = *old;` then override one field; `hamt_insert`; `touch`;
  `add_damage`) that `set_span`/`set_time_map` mirror exactly.
  `set_working_space` (`src/model/model.cpp:601-631`) is the closest precedent
  for a newly-added field.
- `src/model/arbc/model/model.hpp:209-340` — `Model::Transaction`;
  `set_transform` decl at `:238`, `set_opacity` at `:244`,
  `set_working_space` at `:234`, `coalesce()` at `:285`, `add_damage()` at
  `:292`, `commit()` at `:300`. The new setters are declared alongside.
- `src/model/CMakeLists.txt:6` (`DEPENDS base pool media`) and
  `scripts/check_levels.py:22` (`"model": {"base", "pool", "media"}`) — the
  `model → base` edge is already allowed; no config change.

Predecessor / sibling conventions followed: `rational_time.md` (immediate
predecessor — inherits its `TimeRange::all()`-is-the-default and
stills-are-degenerate decisions verbatim); the model-area refinements under
`tasks/refinements/model/` (records stay standard-layout / trivially
destructible with `static_assert`s per type; index-only fields never STL;
levelization stated + CI-enforced; errors as values on the writer path;
behavioral counters, never wall-clock).

## Constraints / requirements

- **Levelization (doc 17, CI-gated).** `arbc::model` is L2 and may include L0
  `arbc::base` headers (`docs/design/17-internal-components.md:41,48,52`;
  `scripts/check_levels.py:22` checks the `#include` closure too). Including
  `arbc/base/rational_time.hpp` from `records.hpp` adds no dependency edge —
  the edge already exists (`transform.hpp`/`ids.hpp` are already included). No
  edge onto contract/surface; `base` must never depend back on `model`.
- **Records stay POD.** `LayerRecord` must remain standard-layout, trivially
  destructible, and trivially copyable
  (`src/model/arbc/model/records.hpp:138-140,149-152`). `TimeRange` and
  `TimeMap` are pointer-free POD (`Time` is an `int64` wrapper, `Rational` two
  `int64`s), so the `static_assert`s hold; the new fields carry no owned edge
  and no count — the layer stays trivially destructible.
- **Default = still.** A `LayerRecord` created without explicit temporal
  placement must have `span == TimeRange::all()` and an identity `time_map`
  (`in=0`, `rate=1/1`, `offset=0`, i.e. `local_time == parent_time`). This is
  the doc 11 promise "stills are the degenerate case, not a special one"
  (`docs/design/11-time-and-video.md:61-64`). `TimeRange::all()` is
  `constexpr`, so it is usable as a default member initializer.
- **Mutation is transactional and publishes once.** `set_span`/`set_time_map`
  go through `Model::Transaction`, path-copy the record (`nr = *old;` +
  single-field override), `hamt_insert`, `touch`, and `add_damage`, exactly
  like `set_transform` (`src/model/model.cpp:633-663`). A committed
  single-field edit increments the model revision by exactly 1 and is visible
  atomically (observers see v(n) or v(n+1), never a half-edit). Repeated edits
  to the same layer within a transaction coalesce as the existing per-field
  setters do (`coalesce()`, `src/model/arbc/model/model.hpp:285`).
- **Errors as values.** Setters targeting a non-existent or non-layer
  `ObjectId` follow the existing setter contract (no throw/abort — mirror
  `set_transform`'s handling of a missing/mismatched target).
- **Scope boundary.** Store and mutate only. No span culling, no time-map
  composition, no rounding — those are `timeline.transport` / pipeline work and
  must not leak into this task.

## Acceptance criteria

- **Unit tests** (`src/model/t/temporal_placement.t.cpp`, wired via
  `arbc_component_test(COMPONENT model …)` in `src/model/CMakeLists.txt`;
  Catch2, matching `src/model/t/` style):
  - A layer added with no temporal placement reports `span ==
    TimeRange::all()` and an identity `time_map` — the still/degenerate default
    (byte-exact field comparison, no tolerance).
  - `set_span` round-trips an arbitrary half-open `[in, out)` (including a
    single-output-frame-long flash span) into the committed record;
    `set_time_map` round-trips an arbitrary `{in, rate, offset}` including a
    **negative rate** (reverse playback is first-class,
    `docs/design/11-time-and-video.md:65-67`).
  - Setting placement on one layer leaves sibling layers' placement and the
    layer's own `transform`/`opacity`/`flags` untouched (structural sharing;
    `SlotRef`-identity of the unedited records preserved).
  - Targeting a missing / non-layer `ObjectId` behaves as `set_transform` does
    (errors-as-values, no throw/abort).
- **Behavioral-counter assertions** (doc 16:54-62 — counters, never
  wall-clock): a single committed `set_span` (or `set_time_map`) bumps
  `Model::revision()` by exactly 1; two coalesced edits to the same layer in
  one transaction publish once; the edit is the executable witness of
  structural sharing (unedited records share slots — assert on the model's
  live-slot / arena counters, matching the `model.transactions` style).
- **Claims (register in `tests/claims/registry.tsv`; enforce with an
  `// enforces: <claim-id>` tagged test — `scripts/check_claims.py` gates both
  directions):**
  - New: `11-time-and-video#temporal-placement-lives-on-layer-defaults-all-present`
    — a layer instance carries `span` + `time_map` beside its spatial
    transform; a layer created with no temporal placement has `span =
    TimeRange::all()` and an identity time map, so a still is the degenerate
    case of a timed layer, not a special record. Enforced by the default-value
    unit test above.
  - Preserve (no change): `11-time-and-video#span-cull-is-half-open` (already
    registered by `timeline.rational_time`, enforced from
    `src/base/t/rational_time.t.cpp`) stays green — this task stores the `span`
    the predicate consumes and must not alter its semantics.
- **Concurrency: none added.** This task adds two fields and two writer-side
  setters that ride the existing single-writer/atomic-publish substrate; it
  introduces no new synchronization. Stated explicitly so the closer does not
  scope TSan/stress work here — the seeded schedule-perturbation stress remains
  `quality.stress_harness` (already `depends model.persistent_state`), not
  duplicated.
- **Coverage / gate.** ≥90% diff coverage on changed lines; `scripts/gate`
  green including asan, `scripts/check_levels.py`, `scripts/check_claims.py`,
  clang-format `-Werror`; `tj3 project.tjp 2>&1 | grep -iE "error|warning"`
  silent.
- **Deferred follow-up (closer registers nothing new).** Every downstream
  consumer of these fields is already a WBS leaf: span culling and time-map
  composition at frame time are `timeline.transport`
  (`tasks/40-time.tji:18-23`, already `depends !temporal_placement`);
  achieved-time keying is `timeline.temporal_cache` (`tasks/40-time.tji:24-29`).
  Content-side temporal metadata (`time_extent()`, the
  `Static`/`Timed`/`Live` stability refinement,
  `docs/design/11-time-and-video.md:75-87`) is a **content/contract** concern
  on the `Content` vtable, out of this model-record task and owned by the
  contract/kinds stream — **no new leaf**. No under-registered follow-ups.

## Decisions

1. **Two fields on `LayerRecord` — `TimeRange span{TimeRange::all()}` and
   `TimeMap time_map{}` — beside `Affine transform{}`
   (`src/model/arbc/model/records.hpp:63`).** This is doc 01/11's literal
   placement: "temporal placement beside the spatial one"
   (`docs/design/11-time-and-video.md:60`), in the same placement bag as
   `transform` (`docs/design/01-core-concepts.md:33-36`). Reuses the exact
   predecessor types (`TimeRange` for the span — *no* new `Span` type — and
   `TimeMap` for the affine map), so the fields are the record-shaped mirror of
   the base arithmetic. *Rejected:* a single fused `TemporalPlacement` struct
   wrapping both — adds a type for no gain, and doc 01 lists `span` and the rate
   map as distinct placement fields; two POD members keep the aggregate init
   and per-field setters uniform with the spatial side.

2. **Temporal placement lives on `LayerRecord` only, never on
   `CompositionRecord`.** A nested composition is placed in time by the
   **layer that references it**, exactly as it is placed in space by that
   layer's `transform`; "time remapping falls out of nesting"
   (`docs/design/11-time-and-video.md:182-186`) precisely because each
   referencing layer carries its own placement. *Rejected:* adding
   `span`/`time_map` to `CompositionRecord` — redundant with the referencing
   layer's placement, would double-count a nested composition's retime, and
   breaks the transform/temporal symmetry (the spatial `transform` is not on
   `CompositionRecord` either).

3. **Defaults via struct default member initializers, not by extending the
   `add_layer` brace list.** `TimeRange::all()` is `constexpr`
   (`src/base/arbc/base/time.hpp:38-41`) and `TimeMap`'s identity is its
   member-initialized default (`in{}`, `rate{1,1}`, `offset{}`,
   `src/base/arbc/base/rational_time.hpp:108-118`), so `TimeRange
   span{TimeRange::all()}; TimeMap time_map{};` on the struct gives the
   still-default for free and survives every construction path
   (`add_layer`'s aggregate init at `src/model/model.cpp:531`, and
   `ObjectRecord`'s value-init at `src/model/arbc/model/records.hpp:130`).
   *Rejected:* threading the defaults through `add_layer`'s positional brace
   list — brittle (every new field re-touches the call site) and the setter
   path would still need the struct default; the member initializer is the one
   source of truth. `add_layer`'s existing brace init stays valid (trailing
   defaulted members).

4. **The degenerate default *is* the "absent" encoding — no presence flag
   consumed.** A layer with `span == all()` and an identity `time_map` is
   exactly doc 11's still ("the degenerate case, not a special one",
   `docs/design/11-time-and-video.md:61-64`); there is nothing to flag as
   present/absent. The reserved flag room
   (`src/model/arbc/model/records.hpp:37-39`) is left untouched for a future
   optimization, not consumed here. *Rejected:* a `k_layer_has_span` /
   `k_layer_has_time_map` presence bit gating field validity — it would
   re-introduce the special-case still the design explicitly removes, and adds
   a branch every reader must honor; the values are self-describing.

5. **Two granular setters (`set_span`, `set_time_map`), each on the
   `set_transform` path-copy template.** Mirrors the existing per-field
   granularity (`set_transform`, `set_opacity`, `set_working_space`) so
   coalescing, damage routing, and revision bumps behave identically and the
   editor can retime without disturbing the span (and vice versa). *Rejected:*
   a single `set_temporal_placement(span, time_map)` — coarser than the spatial
   siblings, forces callers to read-modify-write the field they don't intend to
   change, and diverges from the one-field-per-setter idiom the transaction
   layer is built around.

**No design-doc delta.** Doc 01 (`:33-36`) and doc 11 (`:59-93`) already name
the fields (`span`, `time_map`), fix their placement beside `transform`, pin
the half-open semantics and the `(−∞, +∞)` still-default, and state the affine
formula. This task realizes those normative statements in the model record; it
neither amends nor deviates from them, so no `docs/design/` edit is required.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-07.

- Added `#include <arbc/base/rational_time.hpp>` to `src/model/arbc/model/records.hpp`; two trailing POD fields on `LayerRecord`: `TimeRange span{TimeRange::all()}` and `TimeMap time_map{}`.
- Added `set_span` / `set_time_map` transaction declarations to `src/model/arbc/model/model.hpp` alongside `set_transform` / `set_opacity`.
- Implemented `set_span` / `set_time_map` definitions in `src/model/model.cpp` following the `set_transform` path-copy template.
- Wired `src/model/t/temporal_placement.t.cpp` via `arbc_component_test` in `src/model/CMakeLists.txt`.
- New test file `src/model/t/temporal_placement.t.cpp`: still-default, span/time-map round-trip (incl. negative rate), structural-sharing/SlotRef identity, absent/non-layer no-op; behavioral-counter assertions (revision +1, two-edits-one-publish, damage dedup).
- Registered claim `11-time-and-video#temporal-placement-lives-on-layer-defaults-all-present` in `tests/claims/registry.tsv`; enforced by the still-default test case.
- No design-doc delta required — docs 01/11 already name and specify the fields verbatim; no downstream consumers added (transport, temporal_cache already WBS leaves).
