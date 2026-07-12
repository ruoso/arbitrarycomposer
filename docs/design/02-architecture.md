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
   pixels), and look each tile up in the cache.
4. **Render misses within budget.** Cache misses become render requests with
   a deadline. Layers may answer synchronously, or asynchronously with a
   placeholder policy (see doc 03). When the deadline nears, the frame
   proceeds with what it has: stale-revision tiles, coarser-scale tiles
   rescaled, or checkerboard/transparent, in that preference order.
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

**A damage-gated frame repaints exactly one device region — clearing it
first.** Step 1's damage maps to a device **repaint region**; the frame
clears that region, then re-composites *every* layer that intersects it,
each layer's tiles clipped to it. Both halves are load-bearing:

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

Together these give the frame loop its correctness invariant: **a gated
frame's repaint region is byte-identical to what a single full pass would
have put there, and the rest of the target is untouched** — so compositing
the same frame twice is a no-op, and a scene refined over N follow-up frames
lands on exactly the pixels one un-gated pass would have produced. A tile
that is still un-rendered when the deadline arrives paints step 4's fallback
(stale → coarser → transparent) into the cleared region, as it would in a
full pass; it does not leave the previous frame's pixels showing.

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
- Compositing itself happens on the render thread (or GPU queue).

This is the *model*; v1 may degenerate to "everything on one thread" while
keeping the request/completion structure, so concurrency arrives as an
optimization rather than a redesign.

## Error handling

Layers fail; the composition must not. A failed render request yields a
diagnostic and the placeholder policy (hold stale tile, or transparent).
Failures are reported to the host per-layer, not thrown through the frame
loop. A plugin that crashes is a process-level problem out of scope for the
core (isolation options belong to the eventual plugin-ABI design, doc 03).
