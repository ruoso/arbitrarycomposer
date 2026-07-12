# compositor.disjoint_dirty_repaint

Normalize the frame's device dirty rects into a **pairwise-disjoint** rect set
and clear/clip/repaint **per rect**, replacing the single bounding-box repaint
region that `compositor.refine_frame_composite_idempotence` shipped as its
deliberate, registered simplification.

## TaskJuggler entry

[`tasks/35-compositor.tji:124-129`](../../35-compositor.tji):

```tji
  task disjoint_dirty_repaint "Normalize damage rects to a disjoint set before clear/clip/repaint" {
    effort 2d
    allocate team
    depends !refine_frame_composite_idempotence
    note "The repaint region is currently the bounding box of the frame's device dirty rects (Decision 2 of refine_frame_composite_idempotence): two small damages at opposite corners of the viewport repaint everything between them. Normalize the DirtyRegion into a disjoint rect set and clear/clip/repaint per rect instead. Waste, not incorrectness -- the bbox repaint is byte-exact and re-composites from cache. Source-of-debt: tasks/refinements/compositor/refine_frame_composite_idempotence.md. Docs 02/09."
  }
```

## Effort estimate

**2d as scheduled.** The work splits roughly evenly: ~0.75d for the rect
normalizer (`repaint_regions`, a band-sweep decomposition — self-contained,
heavily unit-testable, no dependencies on the rest of the compositor), ~0.75d
for the `render_frame_interactive` restructure (a `Rect` becomes a
`std::vector<Rect>` at three use sites, and the per-layer walk grows a
tile-dedup step), ~0.5d for goldens, counter assertions and the claims rows.
No new backend virtuals are needed — `clear_rect` and `composite_clipped`
already take exactly the per-rect arguments this task wants — which is what
keeps it at 2d rather than the 1d+backend-surface shape of its predecessor.

## Inherited dependencies

**Settled:**

- `compositor.refine_frame_composite_idempotence`
  ([refinement](refine_frame_composite_idempotence.md)) — the direct
  predecessor and the source of this debt. It landed the clip-scoped backend
  ops (`Backend::clear_rect`, `Backend::composite_clipped`,
  `src/surface/arbc/surface/backend.hpp:63,66`), the
  `repaint_region()` bbox helper (`src/compositor/damage_planning.cpp:84-106`),
  and the clear-first/clip-to-region frame discipline this task generalizes.
  Its Decision 2 *is* the thing being revised — see "Why it needs to be done".
- `compositor.damage_planning` ([refinement](damage_planning.md)) — owns
  `struct DirtyRegion { std::vector<Rect> device_rects; }`
  (`src/compositor/arbc/compositor/damage_planning.hpp:47-49`) and
  `map_damage_to_device`, the producer of the overlapping rects this task
  normalizes. Its D2 (the dirty region is device-space, mapped into each
  layer's local space at plan time) is the seam this task keeps.
- `compositor.counters` ([refinement](counters.md)) — `CompositorCounters`
  (`src/compositor/arbc/compositor/counters.hpp:34-103`), the caller-owned
  counter struct whose `composites` and `requests_issued` fields are how this
  task's efficiency claim is asserted without a wall clock.
- `compositor.in_flight_tile_dedup` ([refinement](in_flight_tile_dedup.md)) —
  the pending-set guard at both dispatch sites (`tile_planning.cpp:402`,
  `pull_service.cpp:251`, both bumping `note_request_suppressed`). Load-bearing
  here: a tile straddling two disjoint repaint rects is *planned* twice, and
  this guard is part of why it is *rendered* once. (Not a `depends` edge in the
  `.tji` — it is already landed on `main`, `fef70f9`.)
- `compositor.expose_visible_plan` ([refinement](expose_visible_plan.md)) — the
  opt-in `visible_plans` sink (`tile_planning.cpp:294-296`), whose contract
  ("this frame's planned layers, in composite order") constrains how the
  per-rect plans are merged. See Constraint 5.

**Pending (downstream, not blocking):** none. This task is a leaf refinement of
an already-shipped seam; nothing in the WBS waits on it.

## What this task is

Today a damage-gated interactive frame computes **one** device repaint rect —
the bounding box of the frame's dirty rects — and uses it three times: to gate
each layer's plan region, as the `clear_rect` argument, and as the `device_clip`
on every composite onto the target. This task replaces that single rect with a
**pairwise-disjoint set** of integer-aligned rects covering the same damage, and
runs the same three uses per rect: clear each rect, plan each layer against each
rect, and clip each tile's composite to the rect that planned it. The correctness
argument is unchanged and merely re-quantified — it was already "the planned set,
the cleared set and the painted set are the same set"; disjointness is what makes
that statement still true when the set is more than one rect, because it is what
guarantees no pixel belongs to two rects and therefore no pixel is cleared twice
or composited twice.

The deliverable is one new free function in the `compositor` component —

```cpp
std::vector<Rect> repaint_regions(const DirtyRegion& dirty, const Viewport& viewport);
```

— landing next to `repaint_region` in
`src/compositor/arbc/compositor/damage_planning.hpp`, plus the
`render_frame_interactive` restructure that consumes it, plus the tests that pin
both. No backend change, no new component, no new levelization edge.

## Why it needs to be done

`refine_frame_composite_idempotence` had to make the gated frame *correct* — its
whole subject was the double-blend that un-cleared source-over produces on a
persisted target. It bought that correctness with a bounding box, and it said so
in the code, in the refinement, and in the WBS note it spawned. The reason is
worth restating precisely, because it is the reason this task is not trivial:

> `map_damage_to_device` emits **one rect per (damage, layer) pair, which may
> overlap** — clipping each tile once per dirty rect would composite it **twice
> in the overlap**, re-introducing the very double-blend this clears.
> (`src/compositor/arbc/compositor/damage_planning.hpp:68-75`)

So "just clip per dirty rect" is not the fix — it is a *reintroduction of the
bug*. The bbox dodged the overlap problem by collapsing the rects to one. This
task solves it properly: make the rects genuinely disjoint first, and then
per-rect clear/clip is not only safe but is the thing that makes "cleared once,
repainted once" true per pixel rather than true by the accident of there being
only one rect.

The cost being paid down is real, if bounded. The bbox is byte-exact and mostly
re-composites from cache, so this is **waste, not incorrectness** — which is why
it shipped. But the waste is unbounded in the wrong dimension: two 32×32 damages
at opposite corners of a 1920×1080 viewport produce a 1920×1080 repaint region.
The frame then clears ~2M pixels, and re-plans and re-composites *every tile of
every visible layer* — hundreds of `composite_clipped` calls, each a full tile
blend — to refresh 2048 pixels of actual damage. That is the common interactive
shape, not a pathological one: two cursors, a selection handle and a toolbar
badge, an async tile arriving in one corner while a caret blinks in the other.
The refine loop (doc 02 step 6) makes it worse by construction, since each
async arrival damages its own tile region and schedules another frame.

The downstream consumer is the interactive driver (`runtime.interactive`,
`src/runtime/interactive.cpp:239,340,346-348`), which already builds the
`DirtyRegion` and passes it by pointer; it needs no change at all. The benefit
lands entirely inside `render_frame_interactive`.

## Inputs / context

**The seam, in code:**

- `src/compositor/arbc/compositor/damage_planning.hpp:47-49` — `struct
  DirtyRegion { std::vector<Rect> device_rects; };`. A bare aggregate; no
  add/merge/iterate surface, and **no disjointness invariant** — the vector is
  whatever `map_damage_to_device` pushed plus whatever the driver appended.
- `src/compositor/arbc/compositor/damage_planning.hpp:51-82` — `repaint_region`,
  the bbox helper, and the ~30-line rationale comment that names this task at
  line 73 as the precision follow-up. **That comment is now stale and is part of
  this task's diff.**
- `src/compositor/damage_planning.cpp:84-106` — the bbox implementation. Note the
  two invariants it already establishes, which `repaint_regions` must preserve:
  intersect each rect with the viewport **before** combining (because
  `Rect::infinite()` is absorbing under `rect_union`, and a structural damage rect
  *is* infinite), and round **out** to whole device pixels (rounding in would
  leave a sub-pixel stale seam).
- `src/compositor/damage_planning.cpp:21-56` — `map_damage_to_device`, the
  producer: one `device_rects.push_back(dev)` per (damage, layer) pair. This is
  the source of the overlap, and it is *not* changing — normalizing at the
  producer would be wrong (see Decision 4).
- `src/compositor/tile_planning.cpp:280-289` — the three-uses-of-one-value block:
  `repaint`, `repaint_clip`, and the `clear` / `clear_rect` fork on
  `dirty == nullptr`.
- `src/compositor/tile_planning.cpp:361-369` — the per-layer plan gate:
  `region = region.intersect(inv->map_rect(repaint))`.
- `src/compositor/tile_planning.cpp:66-73` — `composite_onto_target(...,
  const Rect* clip)`, the helper that routes to `composite_clipped` when the clip
  is non-null. Its call sites: `:559`, `:584`, `:597`, and `:609` (via
  `composite_coarser`, `:81-119`).
- `src/compositor/tile_planning.cpp:294-296` — the `visible_plans` sink clear.
- `src/base/arbc/base/geometry.hpp:15-42` — `Rect` (`empty()`, `intersect()`,
  `from_size()`, `infinite()`); `rect_union` is a free function in
  `src/model/arbc/model/damage.hpp:33-73` (bbox union; empty is the identity,
  `Rect::infinite()` is absorbing).

**The backend seam (unchanged, already sufficient):**

- `src/surface/arbc/surface/backend.hpp:63,66` — `clear_rect(dst, device_rect,
  rgba)` and `composite_clipped(dst, src, src_to_dst, opacity, device_clip)`. Both
  already take a single device rect and write no pixel outside it; calling them N
  times with N disjoint rects is exactly the required semantics, and needs no new
  virtual. CPU impl: `src/backend_cpu/cpu_backend.cpp:65-66,171-185`.

**Design docs (normative):**

- `docs/design/02-architecture.md` § "The frame, interactively" (lines 49-107) —
  step 1 "No damage → no work"; the persisted-target paragraph (75-79); the
  repaint-region paragraph (81-97); the correctness invariant (99-106). **This
  section says "exactly one device region" today and is amended by this task —
  see Decision 5.**
- `docs/design/09-surfaces-and-backends.md` § "Backend contract", "The
  clip-scoped operations" (lines 44-57) — the clip is device-space, half-open,
  intersected with the destination's bounds; an empty clip is a no-op; a
  whole-destination clip is byte-identical to the unclipped op. Nothing here
  changes; this task is purely a consumer.
- `docs/design/16-sdlc-and-quality.md` § "Test taxonomy" item 4 (lines 54-62) —
  behavioral-counter tests are how efficiency claims are asserted ("tiles
  composited"), never wall clock. Item 1 (36-37) names damage soundness
  ("undamaged regions bit-identical across edits") as a conformance property.
- `docs/design/17-internal-components.md` — `compositor` (L4) already sees
  `base` (geometry), `model` (damage), `cache`, `surface`. No new edge.

**Predecessor decisions this must honor:**

- `refine_frame_composite_idempotence` D1 — the clip must be *clip-scoped ops*,
  not a stateful scissor: the backend is shared across the worker pool, so
  `push_clip`/`pop_clip` would be a data race. Per-rect clipping must therefore
  stay in the per-call argument, which `composite_clipped` already is.
- `refine_frame_composite_idempotence` D2 — "planned set == cleared set == painted
  set". Preserved verbatim, now per rect.
- `refine_frame_composite_idempotence` D3 — a damaged tile that misses and does not
  settle in budget paints the transparent placeholder into the cleared region (doc
  02 step 4's degradation order), not the previous frame's pixels. Unchanged: the
  cleared region is just narrower now.
- `damage_planning` D2 — one device-space dirty region serves all layers, mapped
  into each layer's local space at plan time using the inverse the cull already
  computed. Preserved: each *rect* is mapped through the same inverse.
- `counters` D5 — `requests_issued` counts requests *driven*, not misses *planned*.
  Load-bearing for Acceptance criterion 4: planning a straddling tile twice must
  not bump it twice.

## Constraints / requirements

1. **The output set must be pairwise disjoint, integer-aligned, and inside the
   viewport.** These three are the whole contract. Disjointness is the
   correctness-critical one: it is what makes "cleared once, repainted once"
   hold per pixel. Integer alignment is inherited from `repaint_region`'s
   round-out (dirty rects are `Affine::map_rect` outputs — arbitrary doubles);
   the round-out must happen **per input rect, before** decomposition, so the
   decomposition operates on integer rects and its outputs are integer by
   construction. Viewport containment is inherited from the intersect-before-
   combine rule, and is what keeps `Rect::infinite()` structural damage from
   poisoning the sweep.

2. **The union of the output must equal the union of the (viewport-clipped,
   rounded-out) inputs — exactly.** Not a superset (that re-introduces waste)
   and emphatically not a subset (that leaves a stale seam — an undamaged-looking
   pixel that is in fact damaged). This is the property the golden oracle checks
   at the pixel; it is also checkable directly as a set identity in a unit test
   by rasterizing both sides into a coverage grid.

3. **The bounding box of the output must equal `repaint_region(dirty, viewport)`.**
   This ties the new function to the old one and is what makes the cap fallback
   (Decision 3) coherent rather than a special case: the fallback returns
   `{repaint_region(...)}`, whose union is a *superset* of the disjoint set's
   union but is still byte-exact for the reason the predecessor gave. Holds
   because `floor`/`ceil` distribute over `min`/`max`, so rounding out each rect
   and then taking the bbox equals taking the bbox and then rounding out.

4. **Every pixel of the target is written at most once per frame, by exactly one
   repaint rect.** The failure mode this rules out is the one the predecessor's
   header comment warns about: composite a tile once per overlapping dirty rect
   and a translucent layer lands twice in the overlap. Disjointness is the
   mechanism; the golden on a **translucent** scene is the proof. Reuse
   `tests/refine_idempotence_golden.t.cpp`'s deliberately-translucent scene, not
   `damage_planning_golden.t.cpp`'s deliberately-opaque one — opaque source-over
   is a replace and would mask exactly this bug.

5. **A tile straddling two repaint rects is planned twice but rendered once and
   composited once *per rect*.** Planning is idempotent and cheap (a cache/pending
   lookup). Rendering must not double: the second plan's lookup hits the cache the
   first plan's inline fill populated, or is suppressed by the pending set
   (`tile_planning.cpp:402`). Compositing *must* happen once per rect — that is
   how the tile's contribution reaches both rects — and each such composite is
   clipped to its own rect, so the pixels are still written once each. Therefore:
   `requests_issued` must not grow relative to the bbox frame; `composites` may
   grow by the straddle count while dropping by far more from the skipped gap.

6. **The `visible_plans` sink keeps its contract: one entry per planned layer, in
   composite order.** Per-rect planning naturally produces N `LayerTilePlan`s for
   one layer. They must be merged — deduped by `TileKey` — into a single entry
   before being pushed to the sink, or `expose_visible_plan`'s consumers see a
   layer listed N times.

7. **The null-`dirty` path stays byte-identical.** `dirty == nullptr` means
   un-gated: `backend.clear(target, ...)` on the whole surface, unclipped
   composites, no plan gate. Every landed golden depends on this; a golden that
   needs a new baseline is a bug in this task, not a new baseline.

8. **The empty-`DirtyRegion` path stays "no damage → no work".** A non-null but
   empty region (or one that maps entirely outside the viewport) yields an empty
   rect vector, which must plan nothing, clear nothing, composite nothing, and
   leave the target byte-identical — zero renders, zero composites (doc 02:51).
   The natural implementation gets this for free (an empty vector makes every
   per-rect loop body run zero times), but it must be *asserted*, because it is
   the invariant the whole idle-viewport story rests on.

9. **Levelization (doc 17): no new edges.** `repaint_regions` is pure geometry
   over `base::Rect` inside the existing `compositor` component. No new
   dependency, no `backend-cpu` edge, nothing for the CI levelization check to
   notice.

## Acceptance criteria

**Unit tests — the normalizer** (`src/compositor/t/damage_planning.t.cpp`, next to
the existing `repaint_region` cases at `:356-386`):

- *"disjoint rects pass through unchanged (up to round-out)"* — two separated
  rects in, two rects out.
- *"overlapping rects are split into a disjoint cover"* — two overlapping rects
  in; output is pairwise disjoint and its union covers exactly the input union.
  The classic L-shape: assert the coverage grid, not a specific rect
  decomposition (the test must not over-fit the band-sweep's particular cuts).
- *"a contained rect is absorbed"* — rect B inside rect A yields a cover of A.
- *"identical rects collapse"* — the (damage, layer) duplicate case, which is the
  most common real overlap.
- *"a structural infinite rect saturates to the viewport"* — `Rect::infinite()`
  in, one viewport-sized rect out; and mixed with a small rect, still one
  viewport rect (the infinite rect subsumes it).
- *"rects are rounded out, not in"* — a rect with fractional edges yields integer
  edges strictly containing it.
- *"rects outside the viewport contribute nothing"* — empty vector out.
- *"an empty DirtyRegion yields an empty vector"* — the "no damage → no work" root.
- *"the bbox of the output equals repaint_region"* — Constraint 3, asserted as a
  property across the cases above.
- *"a rect count over the cap falls back to the bbox"* — feed > `kMaxRepaintRects`
  worth of mutually-overlapping rects; assert the output is exactly
  `{repaint_region(dirty, viewport)}` (Decision 3).

→ **claim `02-architecture#disjoint-repaint-covers-damage-exactly-once`**: *"The
frame's device repaint region is a set of pairwise-disjoint, integer-aligned rects
inside the viewport whose union is exactly the union of the (viewport-clipped,
rounded-out) device dirty rects: every damaged device pixel lies in exactly one
repaint rect — so it is cleared once and repainted once — and no undamaged pixel
outside that union lies in any of them."*

**Golden tests (byte-exact, doc 16:48-53)** — extend
`tests/refine_idempotence_golden.t.cpp` (its scene is already deliberately
translucent, which is the scene that can *fail* this):

- *"a disjoint-rect gated frame is byte-identical to a bbox gated frame"* — the
  oracle. Drive the same translucent scene twice onto two separately-persisted
  targets from the same warm cache: once with the disjoint repaint set, once
  forced through the bbox (the cap fallback gives a clean way to force it — a
  `kMaxRepaintRects` of 1 is exactly the old behavior). Assert the two targets are
  byte-identical. This is the strongest statement available and it is not a
  tautology: it holds *because* the gap pixels the bbox repaints are undamaged, so
  by the persisted-target contract they already hold precisely what the bbox
  repaints into them. It also means **no existing golden needs a new baseline** —
  which is itself an assertion this task makes.
  → **claim `02-architecture#disjoint-repaint-equals-bbox-repaint`**
- *"a translucent layer under two overlapping damages lands exactly once"* — the
  regression guard for the bug the predecessor's bbox was dodging. Two *overlapping*
  dirty rects over a translucent layer; assert the result is byte-identical to a
  single un-gated full pass over the same scene. Without normalization the overlap
  region is double-blended and this fails.
  → re-asserts (does not re-register) `02-architecture#gated-frame-equals-single-pass`
- The three existing gated-frame claims must pass **unchanged**, on the same
  baselines: `02-architecture#gated-frame-repaint-is-idempotent`,
  `02-architecture#gated-frame-equals-single-pass`,
  `02-architecture#gated-frame-touches-only-its-repaint-region` (registry lines
  158-160). They are the regression net; their proofs now read "the disjoint set"
  where they read "the bbox", but their *assertions* are untouched.

**Behavioral-counter test (doc 16:54-62 — counters, never wall clock)**, in
`src/compositor/t/damage_planning.t.cpp` or alongside the golden:

- *"two far-apart damages do not repaint the gap"* — the headline scenario from the
  WBS note. A viewport with two small damages at opposite corners over a
  multi-tile scene. Assert `composites` delta equals only the tiles overlapping the
  two corner rects, and is **strictly less than** the same frame's `composites`
  under the bbox fallback. Assert `clear_rect` call count is 2, not 1, via
  `CountingBackend` (`src/surface/arbc/surface/testing/counting_backend.hpp:57,92`
  already tallies `clear_rect_calls`).
- *"a straddling tile issues one render, two composites"* — Constraint 5. One damage
  region straddling a tile boundary such that the tile falls in two repaint rects;
  assert `requests_issued` delta is 1 (not 2 — the second plan hits the cache or is
  suppressed by the pending set, bumping `requests_suppressed`) and `composites`
  delta is 2, one per rect.
- *"an empty DirtyRegion still issues zero renders and zero composites"* —
  Constraint 8; re-asserts `02-architecture#interactive-still-scene-schedules-no-frame`
  (registry line 155) through the new code path.

→ **claim `02-architecture#disjoint-repaint-skips-the-undamaged-gap`**: *"A gated
frame whose damage is two far-apart rects composites only the tiles its disjoint
repaint rects overlap — strictly fewer than the bounding-box repaint of the same
damage, which composites every tile between them — while issuing no additional
render requests: a tile straddling two repaint rects is planned twice, rendered
once (cache hit or pending-set suppression) and composited once per rect, each
clipped to its own rect."*

**Concurrency:** `tests/refine_idempotence_stress.t.cpp` (the existing
`[.nightly]` TSan/stress lane, 60 iterations × `worker_count ∈ {1,2,4}`) must be
extended with a multi-damage case, not just re-run. The new code plans the same
tile from two rects, which is a *new* way for two plans of the same `TileKey` to
race within one frame — precisely what the pending-set guard exists for, and
precisely what a single-damage stress case cannot exercise. `repaint_regions`
itself is a pure function over caller-owned data and adds no shared state.

**Coverage:** ≥90% diff coverage on changed lines (the CI gate). The band-sweep
has genuine branch structure (empty input, infinite rect, cap overflow, the
band/run loops) — the unit cases above are enumerated to cover it, not to
decorate it.

**Claims register:** three new rows in `tests/claims/registry.tsv`, each with an
`// enforces: <claim-id>` tag on its test, `scripts/check_claims.py` green in the
`lint` job.

**Design-doc delta (rides in the closer's commit — doc 16's same-commit rule):**
`docs/design/02-architecture.md` and `docs/design/00-overview.md`, already written
— see Decision 5.

**Deferred:** nothing. This task closes the debt it was created for and opens
none; there is no follow-up leaf to register.

## Decisions

### 1. A `repaint_regions()` free function returning `std::vector<Rect>`, not a `DirtyRegion` method and not a stateful region type

`repaint_regions(const DirtyRegion&, const Viewport&) -> std::vector<Rect>` lands
next to `repaint_region` in `damage_planning.hpp` and mirrors it exactly: pure,
caller-owned, no state, plain values in and out. `repaint_region` **stays** — it is
still the cap fallback (Decision 3), it is still what pins Constraint 3, and it is
already under test.

**Alternatives rejected:**

- *(a) A real `Region` class (X11/Cairo-style) with `add`/`subtract`/`intersect`
  and a maintained disjoint-band invariant.* This is the industrial answer and it
  is over-built for one call site. The compositor needs exactly one operation —
  "normalize this vector once, at frame start" — and never mutates the region
  afterward. A stateful region type would add a class, an invariant to maintain,
  and an API surface, to serve a single pure function call. `damage_planning`'s
  D2 rationale (plain values, no cross-frame state at L4) points the same way.
- *(b) Make `DirtyRegion` self-normalizing — maintain disjointness on every
  `push_back`.* Wrong layer, and it changes the meaning of the type. `DirtyRegion`
  is the *driver's* accumulator; `map_damage_to_device` and the clock-advance path
  both append to it (`interactive.cpp:237,239`), and per-append normalization is
  both O(n²)-by-construction and a lie about what the caller handed over. The
  compositor should normalize once, when it is about to consume.
- *(c) Normalize at the producer (`map_damage_to_device`).* Rejected for a sharper
  reason: the producer's one-rect-per-(damage, layer) output is *information*.
  Something downstream may want to know which layer a dirty rect came from
  (per-layer plan gating is an obvious future use). Collapsing that structure at
  the producer to serve the consumer's needs discards it for everyone.

### 2. Band-sweep decomposition (y-bands × merged x-runs), and the frame loops layer-outer / rect-inner

The normalizer: (1) clip each input rect to the viewport, round out, drop empties;
(2) collect the distinct y-edges and sort them into y-bands; (3) within each band,
collect the x-intervals of every rect spanning it, sort and merge the overlapping
ones into disjoint x-runs; (4) emit one rect per (band, x-run); (5) coalesce
vertically-adjacent bands with identical x-runs. This is the classic scanline
region decomposition. Output is disjoint and integer by construction, and its union
is exactly the input union — both properties fall out of the construction rather
than needing to be checked. Step (5) is a pure rect-count reduction (it turns a
plain non-overlapping pair of rects back into two rects instead of three bands) and
is what keeps the common case from paying for the machinery.

In `render_frame_interactive`, the loop nesting is **layer outer, rect inner**:

```
for each layer:                          # composite order, unchanged
  for each repaint rect r:
    region_r = layer_region ∩ inv->map_rect(r)
    plan_layer(region_r) -> tiles        # narrow plan, not the bbox
  dedup the per-rect tiles by TileKey    # -> one visible_plans entry (Constraint 6)
  for each deduped tile:
    render/lookup ONCE                   # requests_issued does not double
    for each rect r the tile overlaps:
      composite_onto_target(clip = r)    # each pixel written once (disjointness)
```

Layer-outer is what preserves bottom-to-top composite order (doc 02 step 5) and the
`visible_plans` ordering. Rect-inner-for-plan is what actually buys the win — tiles
in the gap between two damages are never planned, never looked up, never composited.
Rendering once per deduped tile, then compositing once per overlapping rect, is what
splits Constraint 5's two halves cleanly.

**Alternatives rejected:**

- *(a) Rect-outer, layer-inner — run the entire existing per-layer walk once per
  repaint rect.* The minimal diff (bind `repaint` to each rect and loop), and it is
  *correct*: within a rect, layer order is preserved, and rects are disjoint, so each
  pixel's source-over sequence is still bottom-to-top. But it re-plans and re-renders
  per rect (a straddling tile's render is deduped only by the cache/pending set, which
  is a guard, not a plan), it emits `visible_plans` entries per rect rather than per
  layer (breaking `expose_visible_plan`'s contract), and it interleaves the frame's
  work in an order that makes the counter story much harder to state. Layer-outer
  keeps every existing contract intact for the same amount of work.
- *(b) Keep the bbox plan gate; make only the clear and the clip per-rect.* A much
  smaller diff, and it fixes the clear (2 small clears, not one huge one). But it
  keeps planning, looking up and *rendering* every tile in the bbox and only skips
  the final blend — which forgoes most of the win, since the plan/lookup walk over
  every tile of every layer in a full-viewport bbox is the bulk of the cost the WBS
  note is complaining about. Half a fix.
- *(c) Extend `Backend::composite_clipped` to take a *list* of clip rects.* Pushes
  the disjointness contract into every backend (CPU and every future GPU one) and
  changes a contract doc 09 just settled, to save an outer loop in one caller. The
  existing single-rect op called N times is already the right primitive — doc 09
  explicitly frames it as a scissor rect, and a GPU command list sets a scissor per
  draw anyway.

### 3. Cap the rect count at 64; over the cap, fall back to the bbox

A band sweep over *n* input rects can emit O(n²) output rects in the worst case
(n rects in a diagonal staircase → ~n bands × ~n runs). The input is
one-rect-per-(damage, layer), so a 30-damage commit over 10 visible layers is 300
input rects — a plausible number, and one whose worst-case decomposition is far
worse than the bbox it would replace. So: if the decomposition exceeds
`kMaxRepaintRects = 64`, return `{repaint_region(dirty, viewport)}` — the bbox,
today's exact behavior.

This is a *safe* degradation in the strongest sense: the fallback is not an
approximation, it is the shipped, byte-exact, currently-correct behavior, and
Constraint 3 (bbox of the disjoint set == `repaint_region`) means the fallback's
union is a superset of the disjoint set's, so no damage is ever missed. The cap
converts a pathological-input performance cliff into a graceful return to the
status quo, and it gives the golden a clean way to A/B the two paths.

64 is chosen as comfortably above any realistic interactive frame (a handful of
damages × a handful of layers, most of which overlap and collapse) and comfortably
below the point where per-rect frame overhead — N plan-gate `map_rect`s per layer,
N `clear_rect` calls — would exceed the blend work it saves.

**Alternatives rejected:**

- *(a) No cap.* Leaves an unbounded worst case in the frame path, reachable from
  ordinary model damage. Unacceptable in a loop that has a deadline.
- *(b) Cap by merging the smallest rects pairwise until under the cap.* Produces a
  set that is still disjoint but no longer a minimal cover — a hybrid whose extra
  repainted area is hard to reason about and whose merge policy is one more thing to
  test. The bbox fallback is one line and needs no new correctness argument.
- *(c) An area heuristic instead of a count cap ("if the disjoint set's area is
  ≥90% of the bbox's, just use the bbox").* Genuinely appealing — it targets the
  actual break-even — but it is a second knob with a tuned constant, and it does not
  address the failure the cap exists for (a *many-rect* decomposition can have small
  total area, which is exactly the case the area heuristic would *not* catch and the
  count cap would). Count cap only; the area case is a micro-optimization, not a
  correctness or robustness matter, and no further work is scheduled for it.

### 4. Counters keep their current meanings; no new counter field

`composites` continues to count every `Backend::composite*` call onto the target —
which now means a tile straddling two repaint rects counts **2**. That is honest:
two clipped blends really do happen, into disjoint pixel sets. `requests_issued`
continues to count renders *driven* (`counters` D5), which is why Constraint 5's
"planned twice, rendered once" is observable as `requests_issued` not growing while
`requests_suppressed` may.

No new field (e.g. a `repaint_rects` gauge) is added. The one thing it would buy —
observing the cap fallback from frame level — is already directly unit-testable on
`repaint_regions` itself, and `CountingBackend::clear_rect_calls` already reveals the
rect count from the frame's outside. Adding a counter field to
`CompositorCounters` for a fact two existing observations already pin is surface for
its own sake.

### 5. Design-doc delta: doc 02 says "exactly one device region" today, so the doc changes with the code

Doc 02 § "The frame, interactively" currently reads *"A damage-gated frame repaints
**exactly one** device region — clearing it first."* That is a normative sentence
and this task falsifies it, so under doc 16's same-commit rule the doc is amended in
the same commit as the code (and the house rule from `interactive_pull_wiring` D5 —
*do not mint a claim id for a sentence no design doc contains* — means the doc must
say it first, since this task registers three claims).

The delta, already written into this task's working tree:

- **`docs/design/02-architecture.md`** § "The frame, interactively" — the repaint
  paragraph is generalized from one region to *a set of pairwise-disjoint,
  integer-aligned device rects*, and gains a third load-bearing bullet (*"Disjoint
  rects, not a bounding box"*) explaining why the raw per-(damage, layer) rects
  cannot be clipped to directly — the overlap double-blend — and demoting the
  bounding box to the degenerate one-rect normalization used as a fallback. The
  correctness invariant paragraph gains the composition argument: the invariant is
  per-rect, and disjointness is what lets it compose to the region as a whole.
- **`docs/design/00-overview.md`** § "Guiding decisions" — the existing bullet *"A
  damage-gated frame clears and clips its repaint region"* is amended in place
  (rather than a new bullet added) to say the region is a disjoint rect set and why:
  disjointness is what makes "cleared once, repainted once" true per pixel, and what
  keeps two far-apart damages from repainting everything between them. This is a
  refinement of a decision already recorded, not a new decision, so it belongs in the
  existing bullet.

Doc 09's § "Backend contract" needs **no** change: the clip-scoped ops were specified
per-rect from the start, and N disjoint calls is already within their stated
semantics.

**Alternative rejected:** *leave doc 02 alone and treat "one region" as loose
phrasing.* It is not loose — the predecessor deliberately wrote "exactly one" and
built its idempotence proof on the uniqueness of the region. Generalizing the proof
is the substance of this task, and the doc is where that proof lives.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-12.

- `repaint_regions()` band-sweep normalizer landed in `src/compositor/damage_planning.cpp` (next to the existing `repaint_region` bbox helper); declaration in `src/compositor/arbc/compositor/damage_planning.hpp`. Produces a pairwise-disjoint, integer-aligned, viewport-clipped cover with bbox-fallback over `k_max_repaint_rects = 64`.
- `render_frame_interactive` restructured in `src/compositor/tile_planning.cpp` to layer-outer / rect-inner: per-rect clear, per-rect plan gate, per-rect clipped composite; per-rect plans merged by tile coord so a straddling tile renders once and `visible_plans` keeps one entry per layer.
- Unit tests in `src/compositor/t/damage_planning.t.cpp`: `repaint_regions normalizes the dirty rects into a disjoint integer cover` (8 sections, coverage-grid oracle) + `repaint_regions falls back to the bbox over the rect-count cap`.
- Counter tests (also `damage_planning.t.cpp`): `a disjoint repaint set skips the undamaged gap (counter-backed)` (3 sections, `CountingBackend::clear_rect_calls`) + `a layer lying in the undamaged gap is not planned and not exposed`.
- Golden tests in `tests/refine_idempotence_golden.t.cpp`: `a disjoint-rect gated frame is byte-identical to a bbox gated frame` + `a translucent layer under two OVERLAPPING damages lands exactly once`.
- Stress test in `tests/refine_idempotence_stress.t.cpp`: `a refine sequence whose damage is two far-apart rects is byte-exact under a worker pool`.
- Three new claims rows in `tests/claims/registry.tsv`: `02-architecture#disjoint-repaint-covers-damage-exactly-once`, `#disjoint-repaint-equals-bbox-repaint`, `#disjoint-repaint-skips-the-undamaged-gap`.
- Design-doc delta pre-landed: `docs/design/02-architecture.md` and `docs/design/00-overview.md` generalize "exactly one device region" to the disjoint-rect-set formulation (Decision 5). Deviation noted: `requests_suppressed` stays 0 because the duplicate plan is merged by tile coord before dispatch (layer-outer design); claim text updated to match the real mechanism.
