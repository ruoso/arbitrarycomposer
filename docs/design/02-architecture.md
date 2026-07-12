# 02 — Architecture

## Components

```
┌─────────────────────────────────────────────────────────┐
│ Host application                                        │
│   owns surfaces, drives frames, handles input           │
└──────────────┬──────────────────────────┬───────────────┘
               │                          │
        ┌──────▼───────┐          ┌───────▼────────┐
        │ Interactive  │          │ Offline        │
        │ renderer     │          │ renderer       │
        └──────┬───────┘          └───────┬────────┘
               │      pull: render requests│
        ┌──────▼──────────────────────────▼────────┐
        │ Compositor core                          │
        │  scene model · transform resolution ·    │
        │  culling · tile cache · damage tracking  │
        └──────┬────────────────────────────────────┘
               │ layer interface (doc 03)
   ┌───────────┼───────────┬──────────────┬─────────┐
┌──▼───┐  ┌────▼────┐  ┌───▼────────┐ ┌───▼──────┐  …
│solid │  │ raster  │  │ nested     │ │ plugins  │
│color │  │ image   │  │ composition│ │ (vector, │
└──────┘  └─────────┘  └────────────┘ │ 3D, …)   │
                                      └──────────┘
               │ surface abstraction
        ┌──────▼───────────────────────────┐
        │ Backends: CPU (reference), GPU…  │
        └──────────────────────────────────┘
```

- **Scene model**: compositions, layer instances, transforms, revisions.
  Pure data plus change notification; no rendering knowledge.
- **Compositor core**: given a viewport, resolves transforms, culls layers
  against the visible region, decides what each layer must produce (region +
  scale), consults/fills the tile cache, and composites the results onto the
  target surface.
- **Renderers**: two drivers over the same core. Interactive owns a frame
  loop, deadlines, and progressive refinement. Offline owns exact evaluation.
- **Backends**: implement surfaces (pixel buffers the layers render into and
  the compositor composites between) and the composite operations. The
  reference backend is CPU memory + software compositing; the abstraction
  must map cleanly onto GPU textures + draw calls later, which mostly means:
  surfaces are opaque handles, and pixel readback is an explicit, avoidable
  operation.

## The frame, interactively

1. **Collect damage.** Since the last frame: content damage, placement
   changes, camera changes. No damage → no work.
2. **Resolve and cull.** Walk the composition from the viewport's anchor,
   composing transforms. Intersect each layer's bounds with the visible
   region; skip layers wholly outside or fully occluded (occlusion culling is
   an optimization, not v1).
3. **Plan requests.** For each visible layer: map the visible region into
   layer-local space, quantize scale to the ladder, split into **tiles**
   (fixed local-space-aligned tile grid per scale rung, e.g. 256² device
   pixels), and look each tile up in the cache — and, on a miss, in the
   **pending set** of renders already in flight (below).
4. **Render misses within budget.** Cache misses become render requests with
   a deadline, *unless the tile's render is already in flight* (below).
   Layers may answer synchronously, or asynchronously with a placeholder
   policy (see doc 03). When the deadline nears, the frame proceeds with what
   it has: a **resident but inexact tile** under the tile's own key,
   stale-revision tiles, coarser-scale tiles rescaled, or
   checkerboard/transparent, in that preference order. The first entry is an
   operator's *transient placeholder* — a tile it painted while its inputs
   were still in flight, and which is therefore resident under the current
   revision at the current rung but flagged inexact, so it is not a hit and a
   render is still owed. It is nonetheless strictly better than everything
   below it (same content, same revision, same rung, right geometry — simply
   not final), and it is what a tile whose re-render the refinement wave gate
   defers composites, which is why deferring changes no pixels.
5. **Composite.** Draw tiles bottom-to-top onto the target surface with each
   layer's composed transform and opacity. Tiles rendered at a ladder rung
   are resampled by the ≤1-octave remainder during this pass.
6. **Refine.** Async results that arrive later produce damage for their
   region, scheduling a follow-up frame. Zooming therefore shows
   progressively sharper content rather than blocking.

**The target surface is the caller's, and it persists across frames.** The
compositor never owns it and never reallocates it: the host hands the same
surface to every frame, and the pixels a frame does not repaint are the
pixels the previous frame left there. That is what makes step 1's "no damage
→ no work" a *visual* no-op and not a black screen.

**A damage-gated frame repaints a device repaint region — clearing it
first.** Step 1's damage maps to a device **repaint region**: a set of
**pairwise-disjoint**, integer-aligned device rects. The frame clears that
region, then re-composites *every* layer that intersects it, each layer's
tiles clipped to it. Three parts are load-bearing:

- *Clear first.* Compositing is source-over, which is not idempotent for
  anything but fully-opaque content. Re-compositing a translucent layer onto
  the pixels a previous frame left in place lands its contribution twice. A
  refine frame (step 6) re-composites precisely such a region, so without the
  clear the loop converges — quietly, with no degraded tiles and no pending
  work — on wrong pixels.
- *Clip to the region.* Tiles are whole cache cells, so a tile that
  straddles the region's edge extends beyond it. Painting the overhang would
  land source-over on pixels that were *not* cleared — the same double
  contribution, narrowed to a fringe. Every composite onto the target is
  therefore clipped to the repaint region (doc 09 § Backend contract), which
  makes the painted set and the cleared set the same set.
- *Disjoint rects, not a bounding box.* Damage maps to one device rect per
  (damage, layer) pair, and those rects may overlap. Clearing and clipping
  per raw rect would clear a pixel in an overlap twice — harmless — but also
  composite the tiles covering it **once per overlapping rect**, landing a
  translucent layer's contribution more than once: exactly the double-blend
  the clear exists to prevent. The rects are therefore **normalized into a
  disjoint set** before any of them is cleared or clipped to, so every pixel
  of the region belongs to exactly one repaint rect and is cleared once and
  repainted once. The bounding box of the damage is the degenerate
  one-rect normalization: always *correct*, but it repaints everything
  between two far-apart damages, so it is the fallback (for a pathological
  rect count), not the rule.

Together these give the frame loop its correctness invariant: **a gated
frame's repaint region is byte-identical to what a single full pass would
have put there, and the rest of the target is untouched** — so compositing
the same frame twice is a no-op, and a scene refined over N follow-up frames
lands on exactly the pixels one un-gated pass would have produced. The
invariant is per-rect and the rects are disjoint, so it composes: it holds
of the region as a whole exactly because it holds of each rect and no pixel
is in two. A tile that is still un-rendered when the deadline arrives paints
step 4's fallback (stale → coarser → transparent) into the cleared region,
as it would in a full pass; it does not leave the previous frame's pixels
showing.

**A tile already in flight is not dispatched twice.** The cache is not the
only thing a miss must be checked against: a tile whose render was dispatched
and has not yet landed is absent from the cache, so on the cache alone it is
indistinguishable from a tile nobody has ever asked for. Planning therefore
consults a second suppression key — the **pending set** of in-flight renders
(step 6's queue) — and a miss whose tile is already there issues no request,
allocates no target, and drives no render. It is *not* treated as a hit: it
contributes no pixels this pass and settles nothing, taking exactly the path
the dispatch it deferred to takes — the frame composites step 4's fallback,
and an operator pulling it degrades for this frame (doc 13).

This is safe because an arrival's damage is **broadcast, not delivered**. When
the in-flight render settles, step 6 emits damage for the tile's region keyed
on its content, and the router maps that to *every* consumer of that content —
so one arrival re-plans everyone who wanted the tile, whether they dispatched
the render, deferred to someone else's dispatch, or had not been planned yet
when it was issued. The re-drive is a property of the tile, not of the
dispatch, which is exactly what lets N dispatches collapse to one without
losing a wake-up. Without the check, a frame pays one redundant render per
duplicate ask — an operator whose output spans several tiles re-pulls its
shared input once per output tile, and a nested chain pays one per level per
refinement wave. It costs work, never correctness (every render is
deterministic and targets its own surface), which is why it stayed invisible
until the worker pool made in-flight state routine.

**One carve-out: a *cancelled* pending render does not suppress.**
Cancellation is advisory (doc 03) — it does not settle the completion, and it
leaves the render free to abandon its work and fail. A failed arrival is
dropped with no cache insert and *no damage*, which is what keeps a
persistently-failing tile from spinning the refinement loop forever. So a
suppression that trusted a cancelled entry would strand its tile: absent from
the cache, gone from the pending set, and never damaged, it would show a
placeholder until some unrelated edit happened to repaint the region. A
cancelled entry is therefore re-dispatched, and only a live, uncancelled
in-flight render is joined.

**A frame cancels the renders it no longer wants, not the renders it could
not wait for.** On deadline expiry the frame cancels only the pending renders
it has stopped wanting — a tile superseded by a revision bump, or no longer
visible at the current camera and time — and leaves the rest **in flight**,
uncancelled. So a still-wanted render survives the frame boundary, and the
next frame that re-plans it *joins* the render already running rather than
dispatching a second one. What the frame wants is its **visible footprint**:
every tile a surviving layer covers over the whole viewport at the chosen
rung, plus every tile its pulls named, plus the unmet inputs of any live wave
whose output it still wants. Deliberately not "the tiles it planned" —
planning is repaint-scoped, so on a partial repaint a tile that is plainly
visible and plainly still missing is simply not planned, and nothing
re-dirties a tile merely because its render is late.

Narrowing the sweep costs nothing in enforcement, because the deadline is
enforced by the frame **not parking past it** — the cancel that follows is a
courtesy to the renderer, not the mechanism. The frame still returns at the
deadline, still composites step 4's fallback for every tile that has not
landed. And the blanket alternative is not merely wasteful but *unsound*:
cancellation is advisory (doc 03), so a conformant content is free to honor it
and *fail* the render — which is dropped with no cache insert and no damage.
Cancel a tile the frame still wants, and it is then in neither the cache nor
the pending set, nothing ever damaged it, and nothing re-plans it: it is
stranded behind a placeholder, which is precisely the failure the carve-out
above refuses to introduce via suppression. Retaining what the frame wants
removes the cause — a tile it still wants is never told to abandon its render
— and it is what makes the in-flight join a **cross-frame** mechanism rather
than an intra-frame one. Under a blanket sweep, every entry that survived a
frame boundary was cancelled, and so was disqualified from suppression before
the next frame planned: the carve-out above was reachable only in theory.

**A refinement wave re-drives an operator chain at most once.** The term is
used above as a unit of accounting; here is its boundary. When an operator
renders and one or more of its input tiles answer asynchronously, it paints a
placeholder and must report it *inexact* (doc 13) — flagging it exact would
freeze the empty tile into the cache as the final answer. That transient tile
is resident under its exact key but is not a hit, so the next plan is a miss
and the whole chain renders again. Once, that is correct and necessary: the
re-render is how the real pixels finally get composed. The **refinement wave**
of an operator's transient output tile is the *set of input tiles that render
left unmet*, and it ends when the last of them leaves the pending set. So a
miss is checked against a third thing, after the cache and the pending set:
while its wave is still running, the tile is **not re-rendered**. The frame
composites the transient tile it already has (step 4's new first fallback), no
render is driven and no pull is issued, and when the wave ends the chain
renders exactly once, with everything it needs.

Without this, the chain is re-driven once per *independently arriving input
tile* rather than once per wave, and a chain is not cheap: it is what made a
nested composition cost more with a worker pool than without one, and it is
what made doc 05's "only the spine re-renders" and "the recursive case must
cost what an equivalent flat scene would" false in the async case. The wave is
a **set**, never a duration — no timer, no wall clock, no frame counter; its
only input is which tiles are still pending.

Two asymmetries with the in-flight rule above are deliberate, because the two
gates answer different questions — *"is someone rendering this?"* versus *"is
more coming for this?"* An input still in the pending set holds the wave open
whether or not it has been **cancelled** (cancellation is advisory, and the
render usually lands anyway; and a tile genuinely dropped from the viewport
can still be cancelled by the deadline sweep — which cancels the unsettled
tiles it no longer wants — while an operator's recorded wait names it, so a
gate that trusted the flag would open on a wave that is still running), and
whether or not it has **settled** (a settled tile's
pixels are not in the *cache* until step 6 drains it, so a chain re-rendered
against it would re-dispatch a render that has already finished). Neither can
strand a tile, which is the failure the in-flight carve-out guards against: a
deferred operator tile is *already resident and already composited*, so the
user sees the placeholder they were going to see anyway, and every route out of
the pending set — settled and drained, failed and dropped — opens the gate on
the very next plan. Step 6 runs unconditionally every frame, before any
re-plan, so no wave is held past the drain it is about to get.

## The frame, offline

Same steps without deadlines, quantization, or placeholders: exact scale,
every request rendered to completion, output guaranteed to reflect exactly
revision-consistent content. A **snapshot** mechanism (freeze revisions
during a frame) keeps a frame consistent even if the scene is being mutated
concurrently — needed for "export while editing" and for video where frame N
must not see frame N+1's edits.

The offline path still uses the tile cache when content stability allows —
rendering 4K video of a mostly-static scene should not re-rasterize every
layer every frame — but correctness rules are strict: only exact-scale,
current-revision entries qualify.

## Tile cache

- Key: `(content id, revision, scale rung, tile coords)`.
- Value: a backend surface plus metadata (actual scale achieved, exact vs
  best-effort flag).
- Budgeted (bytes), LRU within priority classes: visible > adjacent (pan
  prefetch ring) > recently visible > speculative (next/previous zoom rung).
- Damage invalidates by `(content id, region)` across all rungs; revision
  bumps invalidate wholesale by making old keys unreachable.
- Nested compositions cache at two levels: the child composition's layers
  cache their own tiles, and the nested-composition layer may cache its
  *composed* result as ordinary tiles. The composed cache is what makes deep
  recursion affordable; doc 05 covers when it's safe.
- **Residency pin vs. payload refcount.** An entry a frame is about to
  composite from must not be evicted mid-frame, so a lookup (or a
  just-completed insert) yields a *pinned* hold on the entry; eviction skips
  pinned entries, and removing a pinned key (invalidation) defers the drop to
  its last unpin. This residency pin is internal to the cache store — it is
  distinct from, and layered above, the backend-pool refcount that owns the
  value's pixel/sample payload (doc 15): the cache component depends only on
  `base` + `surface` (doc 17), so it reconciles "cache values are refcounted
  from the owning nodes" (doc 15) by owning its entries and holding those
  values for as long as they are cached, releasing on eviction.
- **The byte budget is a soft target, not a hard allocator cap.** Budgets are
  the eviction *policy* (doc 15): an insert past budget evicts LRU within the
  lowest priority class first and keeps climbing classes until it fits or only
  pinned entries remain. The pinned working set is never dropped to honor the
  budget — correctness (serving the tiles the current frame needs) outranks the
  budget — so resident bytes may transiently exceed the budget when the pinned
  set alone does.

## Threading model

- The **scene model** is single-writer. The host mutates it from one thread
  (typically the UI thread), through transactions that publish immutable
  document versions (doc 14).
- The **compositor** runs frame planning on the render thread. It reads the
  scene under a snapshot — concretely, a pinned document version (doc 14) —
  so planning never races edits and never takes a lock.
- **Layer rendering** runs on a worker pool. Requests carry everything the
  layer needs (region, scale, deadline, target surface); layer
  implementations declare whether they are internally thread-safe or need
  serialization (the core provides a per-content queue for the latter).
  Layers whose rendering is inherently external (a 3D engine with its own
  thread/GPU context) integrate through the async completion path rather
  than occupying a worker.
- **Worker dispatch is leaf-only.** The pool is a *leaf-render* executor,
  not a general one. A content with inputs — an operator (doc 13): a fade,
  a crossfade, a nested composition — renders inline on the driver thread
  and is never submitted to a worker, because its `render` re-enters the
  `PullService` to fetch those inputs, and a pull probes and inserts into
  the tile cache and walks the service's own descent depth, both of which
  are render-thread-confined. Only *leaf* content, whose render touches
  nothing but its own caller-owned target surface, fans out. This is what
  makes "workers never touch the cache" true rather than aspirational, and
  it costs no parallelism worth having: the leaves are where the pixels are
  made. The rule is not a convention each driver re-derives — it is
  structural, hoisted into runtime's single worker-backed dispatch helper,
  so a driver obtains a worker-backed dispatch only by obtaining one that
  already enforces it.
- **The interactive driver ships with a non-zero worker count.** Not as a
  performance tuning, but because Step 4's deadline promise is otherwise
  unkeepable: with zero workers the pool is the degenerate inline executor,
  where `submit` *is* the render, so the frame thread sits inside a slow
  leaf's `render` while the deadline passes and reaches the deadline park
  only once every miss has been rendered to completion — there is nothing
  left to degrade to and nothing to cancel. "The frame proceeds with what it
  has" needs the render to be somewhere the frame is not. The count is
  *runtime policy*, derived from the machine's hardware concurrency (less
  one, because the frame thread plans, composites and parks, so it is a
  participant) and capped, because the pool is per-viewport — never a fixed
  number, which would oversubscribe a small machine and undersubscribe a
  large one. A host that wants a different pool passes one, and a host that
  wants no threads at all still gets the inline executor by asking for it.
  The **offline** driver keeps inline-exact as *its* default (§ The frame,
  offline): exactness has no deadline to miss, and byte-determinism is the
  whole point of an export.
- Compositing itself happens on the render thread (or GPU queue).

This is the *model*. The interactive driver has graduated out of the
degenerate "everything on one thread" mode it was first built in — and the
fact that it could, by changing a default rather than a design, is what the
request/completion structure was for: concurrency arrived as an optimization,
not a redesign. Drivers with no deadline to enforce (the offline one) stay
there on purpose.

## Error handling

Layers fail; the composition must not. A failed render request yields a
diagnostic and the placeholder policy (hold stale tile, or transparent).
Failures are reported to the host per-layer, not thrown through the frame
loop. A plugin that crashes is a process-level problem out of scope for the
core (isolation options belong to the eventual plugin-ABI design, doc 03).
