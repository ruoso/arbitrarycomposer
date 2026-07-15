# kinds.nested_temporal_mapping — org.arbc.nested visual span culling + time_map

## TaskJuggler entry

Back-link: `tasks/55-kinds.tji`, `task nested_temporal_mapping`
("org.arbc.nested visual span culling + time_map"), inside the
`kinds` container.

## Effort estimate

**2d.** A single-call-site retime plus a span-cull guard on the visual
nested descent, mirroring an already-landed audio twin. The weight is in
the golden matrix (retime, offset/in, two-level never-accumulate, span
half-open, reverse rate, overflow cull) and the not-descended behavioral
counter — not in new machinery.

## Inherited dependencies

- **`!nested_runtime_binding`** (kinds) — **settled** (Done 2026-07-11).
  Widened the operator-binder to a `BindContext` and made `attach` re-key
  the memo only on a genuinely-new pin. The visual descent this task edits
  runs under that binding; nothing new is needed from it.
- **`compositor.root_composition_frame_walk`** — **settled** (Done
  2026-07-15). Re-scoped the visual walk to a single composition's layers,
  so a nested child is reached *only* by recursing through its content.
  That is the walk shape (doc 05:28-35) whose temporal boundary this task
  completes.
- **`compositor.temporal_placement_culling`** — **settled** (Done
  2026-07-07). Landed the flat-walk temporal seam this task mirrors across
  the nested boundary: `present_in_span(layer.span, composition_time)` then
  single-edge `layer.time_map.evaluate(...)`
  (`src/compositor/tile_planning.cpp:380-409`). Registered
  `11-time-and-video#compositor-retimes-request-through-time-map`;
  re-asserted `#span-cull-is-half-open`.

All three predecessors are complete — no pending inheritance.

## What this task is

The visual nested descent currently drops time entirely. When
`NestedContent` composites a child composition, its per-layer loop reads
`visible()`, `opacity`, `transform`, and the resolved content's `bounds()`,
and builds the child sub-request forwarding `request.time` **verbatim**
(`src/kind_nested/nested_content.cpp:319-423`, sub-request at
`:380-384`). It never reads `layer.span` and never reads `layer.time_map`.
Consequently, across a nested visual boundary there is **no span cull** and
**no retime**: a child layer with a rate-2 `time_map`, an in/offset, or a
span that should hide it renders as if the map were identity and the layer
always present.

This task makes the visual descent apply the nesting edge's temporal
placement, exactly as the flat walk and the audio twin already do:

1. **Span cull** the child layer whose half-open span `[in, out)` does not
   contain the request time — before descending (drawing nothing, pulling
   nothing).
2. **Retime** the sub-request by remapping its time instant through the
   edge's `time_map` — `sub.time = layer.time_map.evaluate(request.time)`
   instead of forwarding `request.time` verbatim.

Both are per-edge re-derivations: each nested boundary evaluates its own
single edge; recursion composes boundaries naturally, in exact rational
arithmetic with one rounding at the leaf. The multi-edge flat fold
`ComposedTimeMap::compose` (`src/base/arbc/base/rational_time.hpp:163`,
**zero production callers**) is explicitly *not* the answer.

## Why it needs to be done

Nested visual recursion exists (`kinds.nested`, Done 2026-07-06) and the
flat walk retimes (`compositor.temporal_placement_culling`), but the
*boundary between them* is temporally blind. Any document that nests a clip
with a non-identity `time_map` — slow-motion, offset, reverse — or that
places a nested layer on a bounded span, renders wrong: the child plays at
rate 1, from time 0, and never hides. The audio facet already does this
correctly (`NestedContent::mix_child_layer`,
`src/kind_nested/nested_content.cpp:555-609`); the visual facet is the last
temporally-blind descent. Closing it makes the "time remapping falls out of
nesting, exactly as spatial remapping does" promise (doc 11:217-218) true
for the visual path, and lets multi-edge time maps compose across nested
edges — the property the `.tji` note flags as newly reachable now that a
recursion path exists.

Downstream: every nested-clip authoring workflow (retimed inserts, spanned
overlays) depends on this; the serialize/runtime round-trips of `span`/
`time_map` (already carried on `LayerRecord`) become observable in the
rendered frame rather than silently ignored on the visual side.

## Inputs / context

**Governing design docs** (normative — doc 16):

- **doc 05 (recursive composition)** — 05:28-35: "A frame therefore renders
  exactly one composition's layers … a nested layer's child is reached only
  by recursing through that layer's content … The audio mix already reads
  this way … *the visual frame walk must too*." 05:114-118: termination is
  guaranteed by the sub-pixel cull.
- **doc 11 (time and video)** — 11:21 & 73-74: "Outside its span a layer is
  culled, exactly like content outside the visible region." 11:45-48:
  composed time maps are evaluated in rational arithmetic and rounded to
  the timebase "once, at the leaf … recompute from per-edge matrices, never
  accumulate." 11:66-67: `local_time = (parent_time − in) × rate + offset`,
  `rate` rational, negative allowed. 11:185-191: compose time maps down the
  tree "one rational multiply-add per edge." 11:217-218: "time remapping
  falls out of nesting, exactly as spatial remapping does." 11:226-231:
  temporal cycles terminate by the sub-pixel cull / depth budget.
- **doc 04** — 04:44-47, 113-114: the spatial rule this mirrors —
  "recomputed from per-edge matrices on demand … never incrementally
  accumulated."
- **doc 12 (audio)** — 12:179-182, 200-206: the audio descent spells the
  per-edge/never-accumulate mechanic out concretely (compose per edge; a
  culled contributor "is *not descended*"). This is the template.

**Source seams:**

- `src/kind_nested/nested_content.cpp:319-423` — `compose_child_layer`
  (visual descent). Guard `if (!layer.visible() || layer.opacity <= 0.0)`
  at `:327`; transform compose at `:338`; **sub-request built at
  `:380-384` forwarding `request.time` verbatim**; pull at `:391`;
  unsettled pull returns `false` at `:404-405`; composite at `:418`.
- `src/kind_nested/nested_content.cpp:555-609` — `mix_child_layer` (audio
  twin, the template). Span cull at `:555-560`; per-edge rate re-derivation
  at `:585-600` (recomputes `child_rate` from `layer.time_map.rate.num()/
  .den()` every descent, "never accumulated (doc 11:216-234)");
  `child_start = layer.time_map.evaluate(request.window.start)` with
  overflow → cull at `:602-609`.
- `src/model/arbc/model/records.hpp:87-88` — `LayerRecord` carries
  `TimeRange span{TimeRange::all()}` and `TimeMap time_map{}`. A nested
  "edge" is exactly a `LayerRecord`; there is no separate edge struct.
- `src/base/arbc/base/rational_time.hpp:118-128` — `TimeMap { Time in;
  Rational rate{1,1}; Time offset; evaluate(Time) -> expected<Time,
  TimeError>; }`. `present_in_span(span, t)` helper at `:173-175`
  (`span.contains(t)`).
- `src/base/rational_time.cpp:180-186` — `TimeMap::evaluate` routes the
  single edge through `ComposedTimeMap::from(*this)` then `evaluate`
  (exact-rational, one leaf rounding). Multi-edge
  `ComposedTimeMap::compose` at `:122-133` — **grep-confirmed zero
  production callers** (only unit tests in `src/base/t/rational_time.t.cpp`
  reference it).
- `src/compositor/tile_planning.cpp:380-409` — the flat-walk temporal seam
  to mirror: `present_in_span(layer.span, composition_time)` cull at
  `:390`, then single-edge `layer.time_map.evaluate(composition_time)` →
  `local_time` at `:405`, culling on `!has_value`.

**Note-provenance caveat:** the `.tji` note cites
`tasks/parking-lot.md 2026-07-07 (multi-edge time_map across nested
edges)`. No such entry exists in the current `parking-lot.md` (verified —
the 2026-07-07 entries there concern surfaces, not temporal mapping). The
parked item was evidently promoted into this WBS leaf and removed; no
parking-lot reference is invented here. The governing authority is the
design docs above.

## Constraints / requirements

- **Mirror the audio twin, not `ComposedTimeMap::compose`.** Evaluate the
  single edge per descent; let recursion compose boundaries. Never
  pre-accumulate a `ComposedTimeMap` down the tree (doc 11:45-48, 185-191;
  doc 04:113-114).
- **Retime the instant, don't varispeed.** A visual request carries a time
  instant; the visual analog of audio's varispeed is remapping that
  instant: `sub.time = layer.time_map.evaluate(request.time)`. Only
  `region`, `scale`, `target`, and now `time` are "the layer's own"; the
  sub-request still carries `snapshot`, `exactness`, and `deadline`
  verbatim (doc 05:93-101, constraint 2 — unchanged). Update the
  `:380-384` comment to add `time` to the layer's-own list.
- **Span-cull before descending.** Guard on
  `present_in_span(layer.span, request.time)` alongside the existing
  `visible()/opacity` guard at `:327`, returning the same "settled, nothing
  composited" result. A span-culled child must issue **zero** pulls (not
  descended — the audio twin's discipline, doc 12:200-206).
- **Cull on `evaluate()` overflow.** If `layer.time_map.evaluate(...)`
  returns `!has_value`, draw nothing (unrepresentable local time), matching
  the flat walk (`tile_planning.cpp:405-407`) and audio twin (`:602-609`).
- **Support reverse (negative) rate.** `evaluate` accepts negative rate
  (header: "negative allowed for reverse"); a visual instant remaps cleanly
  under reverse. Do **not** copy the audio twin's `num <= 0` cull
  (`:585-600`) — that cull exists because audio streams have direction and
  reverse is deferred with time-stretch (doc 12:118); a visual instant has
  no such constraint, and culling reverse would wrongly hide valid
  reverse-playback nested clips.
- **Identity honoring path unchanged.** A default `time_map{}` (rate 1/1,
  in/offset 0) and `span == TimeRange::all()` must produce byte-identical
  output to today's descent — the retime/cull is a pure addition on the
  non-identity branch.
- **Levelization (doc 17).** `compose_child_layer` stays in `kind_nested`
  (L4) and uses only `arbc::base` (L1) helpers — `present_in_span`,
  `TimeMap::evaluate` — which the audio twin already consumes. No new
  cross-component edge; the descent still must not link the compositor
  (the L4→L4 ban `kinds.nested` established).

## Acceptance criteria

Tests are part of this task (CI gates ≥90% diff coverage on changed lines).

**Claims register (`tests/claims/registry.tsv`):**

- **Register one new claim** (doc-stem `11-time-and-video`, matching the
  sibling `#compositor-retimes-request-through-time-map`):
  `11-time-and-video#nested-boundary-retimes-child-through-time-map` —
  "Across a nested visual boundary the compositor remaps the child
  sub-request's time through that edge's `time_map` (per edge, never
  accumulated), so a non-identity rate/offset selects a decisively
  different child instant — not the parent time forwarded as identity."
- **Re-assert** (second `// enforces:` tag, no new registration):
  `11-time-and-video#span-cull-is-half-open` on the half-open nested-span
  test, and
  `11-time-and-video#time-map-composes-in-exact-rational-with-one-rounding`
  on the two-level never-accumulate golden.

**Byte-exact goldens** (`tests/nested_goldens.t.cpp`, self-checking against
the flat-compositor oracle, per the nested-family house style):

1. **Retime** — a nested child with a rate-2 `time_map` rendered at parent
   time `T` is byte-identical to that child rendered directly at local time
   `2T` via the flat walk. *(pins the new claim.)*
2. **In/offset** — non-zero `in`/`offset` selects
   `(T − in) × rate + offset`, byte-exact vs oracle.
3. **Identity regression** — default `time_map{}` / `span::all()` is
   byte-identical to the current descent output.
4. **Two-level never-accumulate** — a rate-½ child inside a rate-½ parent
   plays at ¼, byte-exact, proving per-edge re-derivation composes in exact
   rational with one leaf rounding. *(re-asserts
   `#time-map-composes-in-exact-rational-with-one-rounding`.)*
5. **Reverse rate** — a rate `-1` child samples the reversed instant,
   byte-exact vs the flat oracle (no cull).
6. **Overflow cull** — a `time_map` whose `evaluate` overflows draws
   nothing.

**Span cull — golden + behavioral counter:**

7. **Half-open span** — a child whose `[in, out)` span excludes
   `request.time` renders nothing (transparent target unchanged); at
   exactly `out` it is culled, at exactly `in` it is present. *(re-asserts
   `#span-cull-is-half-open`.)*
8. **Not-descended counter** — using a counting `PullService` double, a
   span-culled child issues **zero** `pull` calls (behavioral counter, not
   wall-clock; doc 16:54-62), mirroring the audio "is not descended"
   discipline.

**Conformance:** `tests/nested_conformance.t.cpp` (the `arbc::contract_tests`
driver over `NestedContent`) continues to pass — a regression guard that
the temporal addition breaks no doc-03 layer-contract invariant. The new
temporal-boundary behavior is nested-specific and is pinned by the goldens
+ counter above, not by the generic contract suite.

**No deferred follow-ups.** This task is self-contained; nothing is handed
to a future WBS leaf.

## Decisions

- **D1 — Per-edge re-derivation, not `ComposedTimeMap::compose`.** Each
  boundary evaluates its own single edge; nested recursion composes them,
  in exact rational arithmetic rounded once at the leaf (doc 11:45-48,
  185-191; doc 04:113-114). *Alternative rejected:* pre-accumulate a
  `ComposedTimeMap` down the tree — violates never-accumulate, and
  duplicates the composition recursion already provides. The multi-edge
  flat fold `ComposedTimeMap::compose` has zero production callers and is
  the wrong tool; the `.tji` note names this explicitly.
- **D2 — Retime the sub-request instant, not varispeed.**
  `sub.time = layer.time_map.evaluate(request.time)`. Visual requests carry
  an instant, not a stream, so instant remap is the visual analog of the
  audio twin's rate scaling — which is *why* the visual side needs no
  resampler and is markedly lighter than `nested_audio_resampling`.
- **D3 — Span-cull before descending, reusing the flat-walk helper.**
  `present_in_span(layer.span, request.time)`, at the existing
  `visible()/opacity` guard, zero pulls when culled — the flat walk
  (`tile_planning.cpp:390`) and audio twin (`:555-560`) already do exactly
  this; reuse, don't reinvent.
- **D4 — Support reverse (negative) rate on the visual side.** Cull only on
  `evaluate` overflow, never on `num <= 0`. A visual instant remaps cleanly
  under reverse; the audio twin's `num <= 0` cull is an audio-stream
  concern (direction; reverse deferred with time-stretch, doc 12:118) that
  does not apply. *Alternative rejected:* copy the audio cull — it would
  silently hide legitimate reverse-playback nested clips.
- **D5 — Cull on unrepresentable local time.** `!has_value` from `evaluate`
  → draw nothing, matching `tile_planning.cpp:405-407` and the audio twin.
- **D6 — One new claim + two re-assertions.** Retime across a nested visual
  edge is genuinely new behavior (the task's raison d'être) → new claim.
  Span half-openness and exact-rational composition are re-assertions of
  existing claims in a new location → second `// enforces:` tag, no
  re-registration (registry house style).
- **D7 — Doc stem `11-time-and-video`, not `05`.** The claim asserts
  temporal correctness (retime), which doc 11 governs; it parallels the
  sibling `11-time-and-video#compositor-retimes-request-through-time-map`.
- **D8 — No design-doc delta.** The behavior is already normative: the
  conjunction of "the visual walk must mirror audio" (05:34-35), "outside
  its span a layer is culled" (11:73-74), "time remapping falls out of
  nesting" (11:217-218), and per-edge/never-accumulate (11:45-48) fully
  specifies it. Synthesizing normative promises into a work order is the
  refinement's job, not a design change. This matches the `nested_audio`
  and `temporal_placement_culling` precedent (both implemented
  already-normative behavior with no delta; only
  `nested_working_space_conversion` needed one, for a genuinely new
  `Backend::convert` seam). *Considered and rejected:* a clarifying doc-11
  sentence tying the three pieces together for the visual boundary — the
  design survey flagged that synthesis as "implicit," but implicit-by-
  conjunction is still decided; adding prose that restates existing
  normative text is scope creep, not a delta.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-15.

- `src/kind_nested/nested_content.cpp` — `compose_child_layer`: added half-open span cull (`present_in_span(layer.span, request.time)`) before descending, and retimed the sub-request via `layer.time_map.evaluate(request.time)` (cull on overflow, reverse rate first-class), replacing verbatim `request.time` forwarding.
- `tests/nested_goldens.t.cpp` — added `TimeProbe` time-encoding content + helpers and 8 temporal test cases: retime, in/offset, identity-regression-vs-flat, two-level never-accumulate, reverse-rate, overflow-cull, half-open-span, and not-descended behavioral counter.
- `tests/claims/registry.tsv` — registered `11-time-and-video#nested-boundary-retimes-child-through-time-map`; re-asserted `#span-cull-is-half-open` and `#time-map-composes-in-exact-rational-with-one-rounding` via second `// enforces:` tags.
- Conformance: `tests/nested_conformance.t.cpp` (6 cases) and `tests/nested_goldens.t.cpp` (17 total cases) pass; identity path byte-exact against pre-change output.
