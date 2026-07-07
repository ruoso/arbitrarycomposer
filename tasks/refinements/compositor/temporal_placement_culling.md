# compositor.temporal_placement_culling — Span culling and time_map evaluation in the compositor

## TaskJuggler entry

`tasks/35-compositor.tji:77-82` → `compositor.temporal_placement_culling`
("Span culling and time_map evaluation in the compositor"), the ninth and
last leaf under `task compositor`. It carries
`depends !temporal_coalescing, timeline.temporal_placement`
(`35-compositor.tji:80`) and, through the parent `task compositor`, inherits
`depends contract.async_render, cache.key_shapes, color.resampling`
(`35-compositor.tji:7`). Note line:

> "Wire present_in_span span-culling and time_map evaluation into
> render_frame_interactive/plan_layer at composition_time, consuming the
> landed LayerRecord.span/time_map fields (timeline.temporal_placement).
> Unblocks imageseq's deferred end-to-end span/time_map compositor
> assertions. Docs 01/11. Source-of-debt:
> tasks/refinements/kinds/imageseq_plugin.md"

## Effort estimate

**1d.** The production change is small and localized: the compositor already
threads a real `composition_time` value into `render_frame_interactive` /
`plan_layer` (landed by `temporal_coalescing`), and the two primitives this
task consumes — `present_in_span` and `TimeMap::evaluate` — already exist in
`arbc::base` (landed by `timeline.temporal_placement` /
`timeline.rational_time`). The whole deliverable is **two gates inside one
existing loop body** (`render_frame_interactive`'s `for_each_layer` walk,
`src/compositor/tile_planning.cpp:259-316`): a span-cull `return` and a
time_map evaluation whose result replaces the raw `composition_time` passed
to `plan_layer`. No signature changes, no new frame path, no new seam. The
bulk of the work is tests: an end-to-end span-cull assertion, a time_map
retiming assertion, a behavioral-counter zero-render assertion, and the
imageseq reverse-rate through-compositor golden that this task's debt owner
(`imageseq_plugin`) deferred to it.

## Inherited dependencies

**Settled:**

- **`compositor.temporal_coalescing`** (DONE 2026-07-06,
  [`temporal_coalescing.md`](temporal_coalescing.md)) — the direct
  predecessor (`depends !temporal_coalescing`). It replaced the hard-wired
  `Time::zero()` in `render_frame_interactive` with a caller-supplied
  `Time composition_time` value parameter
  (`tile_planning.hpp:286-292`), threaded into `plan_layer`'s `Time time`
  argument (`tile_planning.cpp:314-316`), which is then keyed at
  `content->quantize_time(time).value_or(time)` (`tile_planning.cpp:160-163`).
  This task feeds the **time_map-evaluated local time** into that same
  `time` argument, so the pipeline order becomes
  *evaluate time_map → quantize_time → tile key* — exactly the compositor
  ordering doc 11:186-191 specifies. Its Decision routed per-layer
  local-time-via-time-map to a future task and noted "until time maps land,
  local time == composition time (identity), which is what this task
  passes" — this task is that landing.

- **`timeline.temporal_placement`** (DONE 2026-07-07,
  [`../timeline/temporal_placement.md`](../timeline/temporal_placement.md))
  — landed the two fields this task reads:
  - `LayerRecord.span` (`TimeRange`, default `TimeRange::all()`) and
    `LayerRecord.time_map` (`TimeMap`, default identity) at
    `src/model/arbc/model/records.hpp:74-75`.
  - It explicitly scoped span culling and time-map evaluation **out** — a
    store-and-mutate-only task — deferring them to the transport / pull
    pipeline. This task closes that boundary on the compositor side.
  - The consuming primitives it built on
    (`timeline.rational_time`): `present_in_span(const TimeRange&, Time)`
    (`src/base/arbc/base/rational_time.hpp:163-165`) and `TimeMap::evaluate`
    / `ComposedTimeMap` (`rational_time.hpp:108-156`).

**Pending:** none — both dependencies are settled.

## What this task is

Make the interactive compositor honor each layer's **temporal placement**:
its `span` (the half-open `[in, out)` interval in composition time during
which the layer exists) and its `time_map` (the 1D affine map from
composition time to content-local time). Today the compositor's per-layer
walk (`render_frame_interactive` → `for_each_layer`,
`tile_planning.cpp:259-316`) reads `LayerRecord.transform`, `.opacity`,
`.visible()`, and `.content`, but **ignores `.span` and `.time_map`**: every
layer is treated as always-present and every request is issued at raw
composition time (identity time map). This task adds, inside that same loop
body, (1) a **span cull** — a layer whose span does not contain the current
`composition_time` is skipped entirely, before any content resolve or render
— and (2) a **time_map evaluation** — the layer's `time_map` is evaluated at
`composition_time` to produce the content-local time that is then passed to
`plan_layer` (and thence into the tile key and the miss `RenderRequest`) in
place of the raw composition time.

## Why it needs to be done

The `span`/`time_map` fields are already on `LayerRecord` and already
settable, but no code path reads them (`present_in_span` appears only in
`src/base/t/rational_time.t.cpp`; `.span`/`.time_map` are read only by model
tests). Temporal placement is therefore modeled but inert — a still and a
timed clip are indistinguishable to the compositor, and reverse playback,
trimmed spans, and retimed clips are unrepresentable in a rendered frame.
This is the last piece that makes the doc-11 temporal model *observable*:
doc 11:21/72-73 promises "outside its span → skip" and doc 11:122-124
promises `RenderRequest.time` is "content-local time, computed by the
compositor through the composed time map." Downstream, `imageseq_plugin`
(DONE 2026-07-07) explicitly **deferred its end-to-end span/time_map
compositor assertions to this task** — the reverse-rate through-compositor
golden and the end-to-end `#span-cull-is-half-open` re-assertion — because
the compositor did not yet consume the fields. This task discharges that
debt.

## Inputs / context

**The seam (production change lands here):**

- `src/compositor/tile_planning.cpp:259-316` — `render_frame_interactive`'s
  `state.for_each_layer([&](const LayerRecord& layer){ … })` walk. Current
  cull gate is only `if (!layer.visible() || layer.opacity <= 0.0) return;`
  (`:260-262`); content resolve at `:263-266`; degenerate-affine cull
  (`if (!inv.has_value()) return; // degenerate placement: cull`) at
  `:280-282` — **the established error-cull pattern** this task mirrors;
  `plan_layer` call passing `composition_time` at `:314-316`.
- `src/compositor/arbc/compositor/tile_planning.hpp:161-166` (`plan_layer`
  decl — already carries `Time time`), `:286-292`
  (`render_frame_interactive` decl — already carries
  `Time composition_time`). **Neither signature needs to change.**

**The fields consumed:**

- `src/model/arbc/model/records.hpp:62-78` — `LayerRecord`;
  `TimeRange span{TimeRange::all()}` (`:74`), `TimeMap time_map{}` (`:75`),
  `bool visible()` (`:77`).

**The base primitives (already landed, `arbc::base`):**

- `src/base/arbc/base/rational_time.hpp:163-165` — `present_in_span(span,
  parent_time)` = `span.contains(parent_time)`; the header comment
  (`:158-162`) states "Outside its span a layer is culled **before its time
  map is evaluated**" and that a degenerate span (`out <= in`) is present at
  no instant.
- `rational_time.hpp:108-118` — `TimeMap { Time in; Rational rate{1,1};
  Time offset; expected<Time,TimeError> evaluate(Time parent_time); }`;
  identity default evaluates `parent_time` unchanged. `:126-156` —
  `ComposedTimeMap` (the multi-edge fold; see Decision D2 for why this task
  uses single-edge `evaluate`, not the fold).

**Behavioral counters (for the zero-render assertion):**

- `src/compositor/arbc/compositor/counters.hpp:34-66` —
  `CompositorCounters::requests_issued()`, `composites()`,
  `operator_renders()`; `note_request_issued()` is bumped in the miss loop
  at `tile_planning.cpp:367`. A layer culled at the `for_each_layer` gate
  returns before reaching the miss loop, so a culled-layer frame is
  observable as `requests_issued() == 0 && composites() == 0`.

**Governing design docs (normative — doc 16):**

- `docs/design/11-time-and-video.md:15-25` — the spatial↔temporal symmetry
  table; `:21` "Culling: outside span → skip". `:34-40` — composition time
  is the root composition's time axis. `:60-73` — the `span` and `time_map`
  fields; the time_map formula `local_time = (parent_time − in) × rate +
  offset` (`:66-71`); `:72-73` "Outside its span a layer is culled, exactly
  like content outside the visible region." `:119-159` — `RenderRequest.time`
  is "content-local time, computed by the compositor through the composed
  time map" (`:122-124`), and the `quantize_time` pre-lookup (`:145-159`).
  `:185-213` — the pipeline: **"Frame planning samples the transport's
  current composition time, then computes each layer's local time by
  composing time maps down the tree … and then snaps each `Timed` layer's
  local time to that content's grid via `quantize_time` before the tile-cache
  lookup"** (`:186-191`) — the normative ordering this task realizes at the
  single-layer level.
- `docs/design/01-core-concepts.md:33-37` — temporal placement (a `span` and
  a rate map from parent time to content-local time) is a per-instance
  property shared by the visual and audio facets; `:60-63` — `Stability`
  (`Static`/`Timed`/`Live`) gates caching.

**The debt this task discharges:**

- `tasks/refinements/kinds/imageseq_plugin.md` (Status, `:475`) — deferred:
  "compositor does not yet consume `LayerRecord.span`/`time_map` at
  `composition_time`; reverse-via-`time_map` through-compositor golden
  deferred; `#span-cull-is-half-open` stays enforced by `rational_time.t.cpp`.
  Follow-up registered as `compositor.temporal_placement_culling`." Existing
  scaffolding to reuse: the imageseq temporal test + `tests/support/
  imageseq_fixtures.hpp` and fixtures `plugins/imageseq/t/fixtures/
  frame_000{0..3}.ppm`; `ImageseqContent` is `Stability::Timed` with exact
  `quantize_time`.

## Constraints / requirements

1. **Consume `span` and `time_map` inside the existing `for_each_layer`
   body** (`tile_planning.cpp:259-316`) — no new function, no signature
   change to `plan_layer` or `render_frame_interactive` (both already carry
   the needed `Time`). The compositor stays the pure, caller-state-only
   per-frame library it is at every sibling leaf.
2. **Order (doc 11:186-191, and the `:158-162` header comment):** span cull
   **first**, then time_map evaluation only for present layers, then the
   existing `quantize_time` keying inside `plan_layer`. Concretely the loop
   becomes: visible/opacity gate → `present_in_span(layer.span,
   composition_time)` cull → resolve content → evaluate `layer.time_map` at
   `composition_time` → pass the resulting local time as `plan_layer`'s
   `time`.
3. **Span cull is a full skip:** an out-of-span layer must resolve no
   content, issue no render request, and emit no composite — it `return`s at
   the gate exactly like the `!visible()` and degenerate-affine culls.
4. **Half-open semantics:** present iff `in <= composition_time < out`
   (`out` excluded); default `TimeRange::all()` is always present (a still is
   the degenerate case); a degenerate span (`out <= in`) is present at no
   instant. These follow directly from `present_in_span` — do not
   re-implement the predicate.
5. **time_map evaluation error → cull.** `TimeMap::evaluate` returns
   `expected<Time, TimeError>` (overflow of the fixed flick width). On error,
   skip the layer, mirroring the degenerate-affine cull at `:280-282` — a
   layer that cannot be temporally placed is culled, never rendered at a
   wrong or clamped time (Decision D3).
6. **Identity/default path is byte-exact.** A layer with the default
   `span == TimeRange::all()` and identity `time_map{}` (in=0, rate=1/1,
   offset=0) must produce byte-for-byte the same plan, keys, requests, and
   composites as today — guarded by the landed golden set (tile_planning,
   refinement, anchored_viewports, damage_planning, counters,
   temporal_coalescing goldens). This is the invariant every compositor
   sibling holds.
7. **Levelization (doc 17, CI-enforced):** no new edge. `present_in_span` /
   `TimeMap` are `arbc::base` (universally depended-on); `LayerRecord` is
   `arbc::model`, already read by the compositor via `DocRoot::for_each_layer`.
   `arbc::compositor` (L4) `DEPENDS contract cache` is unchanged.
8. **Diff coverage ≥ 90%** on changed lines (doc 16 CI gate).

## Acceptance criteria

1. **End-to-end half-open span culling — re-assert
   `11-time-and-video#span-cull-is-half-open`** (registry.tsv:124, today
   enforced only at the base-arithmetic level by `src/base/t/rational_time.t.cpp`).
   A new `// enforces: 11-time-and-video#span-cull-is-half-open`–tagged
   compositor test (`src/compositor/t/temporal_placement_culling.t.cpp`)
   places a layer with `span = [in, out)` and drives
   `render_frame_interactive` at composition times `in-1` (absent), `in`
   (present), `out-1` (present), `out` (absent, half-open) — asserting the
   layer is composited iff present. Re-assert; **do not re-register** the
   claim.
2. **Culled layer issues zero renders and zero composites** — behavioral
   counters (doc 16, no wall-clock). Within the same span-cull test, at an
   out-of-span composition time assert `counters.requests_issued() == 0`,
   `counters.composites() == 0`, and `counters.operator_renders() == 0`; at
   an in-span time assert they are non-zero. Folded into the
   `#span-cull-is-half-open` enforcing test (Decision D5), pinning the
   performance face of "outside span → skip" (doc 11:21,72).
3. **Compositor retimes the request through `time_map` — register new claim
   `11-time-and-video#compositor-retimes-request-through-time-map`.** A layer
   with `time_map{in, rate, offset}` placed and rendered at
   `composition_time = t` causes the content to be requested at the
   content-local time `local = (t − in) × rate + offset` (doc 11:66-71,
   122-124). Enforced by a tagged test using a test `Timed` content kind that
   records the request `time` it received (or asserts on the tile key's
   `achieved_time` derived from that time), covering forward rate ≠ 1,
   non-zero `in`/`offset`, and a negative rate (reverse playback). New
   registry.tsv row + `enforces:` tag in
   `src/compositor/t/temporal_placement_culling.t.cpp`.
4. **Reverse-rate through-compositor golden (byte-exact) — discharges the
   imageseq debt.** Enable/add the imageseq temporal test's deferred
   variant (c): a placed `ImageseqContent` layer with `time_map.rate < 0`;
   advancing `composition_time` yields decreasing content-local time →
   earlier fixture frames; a byte-exact golden PPM matches the reversed
   frame order (reusing `plugins/imageseq/t/fixtures/frame_000{0..3}.ppm`).
   This closes `imageseq_plugin`'s deferred through-compositor assertions;
   the closer notes the debt discharged in that refinement's follow-up.
5. **Identity/default path unchanged** — the full landed golden set
   (tile_planning, refinement, anchored_viewports, damage_planning, counters,
   temporal_coalescing) passes byte-unchanged, proving a default-placement
   layer is byte-identical to today. No new golden needed; assert by running
   the existing suite.
6. **time_map evaluation error culls** — a test constructs a `time_map` whose
   `evaluate` at the driven `composition_time` overflows (`TimeError`) and
   asserts the layer is culled (zero requests/composites), mirroring the
   degenerate-affine cull.
7. **Conformance:** the change is compositor-internal (consumes existing
   contract fields); no content-kind or operator contract surface changes, so
   no new `arbc-testing` conformance case is required beyond the imageseq
   golden of criterion 4. Diff coverage ≥ 90%.

## Decisions

- **D1 — Cull and evaluate in the driver loop body, not in `plan_layer`.**
  Both the `present_in_span` gate and the `time_map` evaluation land inside
  `render_frame_interactive`'s `for_each_layer` body
  (`tile_planning.cpp:259-316`); the evaluated local time is passed as
  `plan_layer`'s existing `Time time` argument. *Rationale:* the driver
  already owns the per-layer cull/compose walk (the "front half reused
  verbatim" comment at `:256-258`), while `plan_layer` is the pure,
  keying-only tail. Culling and time remapping are placement concerns, so
  they belong with the transform compose and the affine/region culls, not in
  the keying tail. This needs no signature change and keeps the identity path
  byte-exact. *Rejected:* adding a parameter to `plan_layer` or evaluating
  inside it — spreads temporal placement into the pure keying layer and adds
  a parameter for no gain.
- **D2 — Single-edge `TimeMap::evaluate`, not multi-edge
  `ComposedTimeMap`.** The compositor's `for_each_layer` is a flat walk over
  the root composition's direct layers; each layer contributes exactly one
  time_map edge, so `layer.time_map.evaluate(composition_time)` is the
  complete evaluation. *Rationale:* `evaluate` already routes through the
  same rational-compose-then-round-once path as a chain
  (`rational_time.hpp:113-115, 145-149`), so precision is identical to the
  fold; there is no second edge to compose because the compositor does not
  yet recurse into nested compositions. *Rejected:* wiring
  `ComposedTimeMap::compose` over an edge stack now — there is no edge stack
  until nested-composition rendering exists; that multi-edge composition is
  subsumed by the future nested-composition-rendering axis (see Open
  questions), not this task.
- **D3 — time_map evaluation error → cull.** On `TimeError` from `evaluate`,
  skip the layer. *Rationale:* consistent with the existing
  degenerate-placement cull (`:280-282`); a layer that cannot be temporally
  placed is honestly culled. *Rejected:* clamping the local time to the
  Time bounds — silently renders a wrong frame; a cull is the honest
  failure and cannot mislead a golden.
- **D4 — Span cull strictly before time_map evaluation.** *Rationale:* the
  `rational_time.hpp:158-162` header comment and doc 11:72 both state the
  cull precedes evaluation; it is also cheaper (an absent layer never
  evaluates its map or resolves content).
- **D5 — Fold the zero-render counter assertion into the
  `#span-cull-is-half-open` test; register only one new claim
  (`#compositor-retimes-request-through-time-map`).** *Rationale:* the
  zero-render-on-cull behavior is the performance face of the same half-open
  cull promise and is naturally asserted in the same test case; a separate
  `#culled-layer-issues-zero-renders` row would be redundant. The time_map
  retiming is a genuinely distinct promised behavior (doc 11:122-124) with no
  existing claim, so it earns one row.
- **D6 — Discharge the imageseq reverse-rate golden here, not as a fresh
  follow-up.** *Rationale:* this task is the registered debt owner; the
  fixtures and test harness already exist; landing the golden here closes the
  loop rather than perpetuating a follow-up chain. *Rejected:* spawning
  another `kinds`-area leaf for the golden — needless indirection now that
  the compositor honors the fields.
- **No design-doc delta.** The behavior this task lands is exactly what
  docs 11 (60-73, 122-124, 185-191) and 01 (33-37) already specify, using
  primitives (`present_in_span`, `TimeMap::evaluate`) and fields
  (`LayerRecord.span/time_map`) already landed by predecessors. No new seam,
  no new dependency, no deviation — so no `docs/design/` edit is required.

## Open questions

(none — all decided.)

Note for the parking lot (not a WBS leaf): **multi-edge time_map composition
across nested-composition edges** (`ComposedTimeMap::compose` over an edge
stack) becomes necessary only when the compositor gains nested-composition
recursion — a spatial+temporal axis that is not yet built and is blocked on
unbuilt nested-composition rendering. It is therefore not pickup-ready
implementable work today and is deliberately *not* registered as a WBS leaf
(per doc-16 anti-pattern: no task whose only content is "revisit later"); the
future nested-composition-rendering task will fold time maps down the tree as
a natural part of recursing, replacing this task's single-edge `evaluate`
with the fold. Surfaced here for the human-review queue.

## Status

**Done** — 2026-07-07.

- Span cull gate added inside `render_frame_interactive`'s `for_each_layer` body (`src/compositor/tile_planning.cpp`) — `present_in_span(layer.span, composition_time)` returns before content resolve or render; mirrors the established degenerate-affine cull pattern.
- `layer.time_map.evaluate(composition_time)` threaded into `plan_layer` as `local_time` (replacing the raw `composition_time` pass-through); evaluation error → cull, consistent with Decision D3.
- Doc comment on `composition_time` updated in `src/compositor/arbc/compositor/tile_planning.hpp` (no signature change).
- New unit/behavioral test `src/compositor/t/temporal_placement_culling.t.cpp` (4 cases): half-open span cull with zero-render/composite counters (re-asserts `#span-cull-is-half-open`), time_map forward-rate retiming, time_map reverse-rate retiming, time_map overflow cull — registered in `src/compositor/CMakeLists.txt`.
- New computed-reference golden in `tests/imageseq_temporal.t.cpp`: reverse-rate `ImageseqContent` layer through the compositor discharges `imageseq_plugin`'s deferred end-to-end assertion; test harness extended with `Scene` time_map support.
- New claim `11-time-and-video#compositor-retimes-request-through-time-map` registered in `tests/claims/registry.tsv`; `#span-cull-is-half-open` re-asserted (not re-registered).
- `src/contract/arbc/contract/registry.hpp` and `src/contract/t/registry.t.cpp` updated as needed by registry changes; existing imageseq test suite (`imageseq_achieved_time`, `imageseq_concurrency_stress`, `imageseq_conformance`, `imageseq_plugin_path`, `imageseq_prefetch`, `imageseq_provided_surface`) passes unchanged — identity/default path is byte-exact.
