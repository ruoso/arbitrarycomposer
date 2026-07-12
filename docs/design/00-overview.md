# 00 — Overview

## Vision

Arbitrary Composer is a library (not an application) for building 2D composed
views out of heterogeneous layers. The core bet is a single, small contract
between the compositor and its layers:

> A layer, given a region of its own local coordinate space and a target
> resolution, produces pixels for that region at that resolution.

Everything else follows from taking that contract seriously:

- **Arbitrary layer kinds.** Because the compositor only ever asks for
  "pixels for this region at this scale", it does not care whether the layer
  is backed by a decoded raster, a vector document tessellated on demand, a 3D
  scene rendered through a camera, a procedural fractal, or another entire
  composition evaluated recursively.
- **Arbitrary per-layer transforms.** Each layer is placed in its parent's
  space by a full 2D affine transform (translate, rotate, scale, shear).
  Transforms compose down the tree; the compositor resolves the final
  viewport-to-layer mapping and derives the resolution each layer must
  produce.
- **Resolution independence / infinite zoom.** Layers that can re-render at
  any scale (vector, 3D, procedural, nested compositions) never pixelate.
  Zooming in simply raises the requested resolution for the layers in view.
  The limits are per-layer (a raster layer bottoms out at its native pixels)
  rather than global.
- **Two rendering modes, one scene model.** The same composition renders
  interactively (deadline-driven, progressive refinement, caching) and
  offline (exact, unbounded budget, arbitrary output resolution). This is a
  day-one requirement, and it shapes the layer contract: layers must support
  both a "best effort by deadline" and an "exact, take your time" request.

## Goals

1. A clean C++ core API: composition graph, transforms, viewport, and the
   layer interface.
2. A layer plugin mechanism so layer kinds can be implemented outside the
   core library, including by third parties.
3. Built-in reference layer kinds: solid color, raster image, and nested
   composition. These exercise the contract and serve as plugin examples.
   Two further reference kinds — a still image and an image sequence — ship
   *outside* the core library because they carry decoders (doc 17's "codec
   line"). The split between the editable, codec-free `raster` and the
   read-only, file-referencing `image` is load-bearing rather than
   cosmetic: it is what lets a document reference the photographs it was
   built from instead of copying them (doc 08 Principles 3 and 8), and it
   makes non-destructive editing structural — you retouch by stacking an
   editable raster over a referenced image.
4. An interactive render loop with tile caching, damage tracking, and
   progressive refinement.
5. An offline renderer producing exact frames at arbitrary resolution.
6. Numeric design that survives deep zoom (see
   [04-transforms-and-infinite-zoom](04-transforms-and-infinite-zoom.md)).
7. A temporal model: compositions have a timeline, layers occupy time spans
   with rate-mapped local time, viewports have a transport, and timed
   content (video-like) renders through the same pull contract as stills
   (see [11-time-and-video](11-time-and-video.md)).
8. Audio as a content facet through the same pull contract: additive
   mixing with per-layer gain, working sample format per composition, a
   glitch-free lookahead engine with audio-clock-mastered transports, and
   sample-exact export (see [12-audio](12-audio.md)).
9. Effects as content operators: content that consumes content through the
   core's pull machinery, with pass-through optimization and damage
   routing; fade and crossfade ship as reference kinds (see
   [13-effects-as-operators](13-effects-as-operators.md)).
10. An editing data model fit for a multi-disciplinary editor: immutable
    document versions with structural sharing, transactions with gesture
    coalescing, a document-wide undo journal spanning all content kinds,
    and lock-free consistent reads for live outputs (see
    [14-data-model-and-editing](14-data-model-and-editing.md)).

## Non-goals (for now)

- **Not a GUI framework.** The library produces composed pixels into a
  surface the host provides; windowing, input handling, and widgets belong to
  the host application. An embeddable viewport *widget* for a specific
  toolkit could be a separate downstream package.
- **Not an editor.** Scene construction is programmatic API first. A file
  format for compositions is desirable but comes after the in-memory model is
  settled.
- **Not a keyframe animation system.** Timed *content* and the timeline are
  in scope (doc 11); keyframed *property* animation (animating transforms,
  opacity, params over time, with interpolators and curves) is not — it
  stages cleanly later because placement is already sampled every frame.
- **No exhaustive effect library in v1.** The effect *mechanism* is core
  (operators, doc 13) and fade/crossfade ship as reference kinds, but the
  library — grades, keys, stylizes, blend-mode and matte operators, audio
  DSP — is plugin territory. Layer placement itself stays source-over plus
  opacity/gain; richer combination lives in the operator graph.

## Guiding decisions

Decisions made so far, and their rationale:

| Decision | Rationale |
| --- | --- |
| C++ implementation | Control over rendering backends, embeddability into native applications, feasible bindings to other languages later. |
| Interactive *and* offline rendering from day one | Designing the layer contract for only one mode would bake in assumptions (deadlines vs exactness) that are hard to retrofit. |
| Layer kinds are plugins | The whole point of the library; the core must not enumerate layer kinds. |
| Compositor pulls from layers, layers never push pixels | Pull-based rendering is what makes resolution independence work: the request carries the resolution. Layers push *invalidation*, never pixels. |
| No global coordinate space requirement | All coordinates are relative (layer-local, parent-relative transforms, viewport anchored to a node). This is what makes deep zoom numerically viable. |
| CPU reference backend first, GPU as a backend behind the same surface abstraction | Correctness and the contract come first; the design must not preclude GPU compositing, but must not depend on it either. |

## Resolved questions

Initially-open questions, now decided in their own docs:

- **Plugin ABI**: two stages — C++ interface + `dlopen` registration for v1,
  stable C ABI shim once the interface stops moving; the v1 structs are
  shaped for that future. Decided in doc 03.
- **Color management**: surfaces tagged with pixel format + color space from
  day one; per-composition working space, default premultiplied linear-light
  RGBA16F; format-templated kernels behind a type-erased plugin boundary.
  Decided in doc 07.
- **Serialization**: v1 deliverable. Canonical JSON documents, kind-owned
  `params`, lossless round-trip of unknown kinds *and* of unknown fields at
  every document tier, in-document child compositions in a core-owned
  `compositions` table (Droste cycles serialize), external references by
  URI. Decided in doc 08.
- **Content-provided surfaces / texture adoption**: `RenderResult` may carry
  the content's own surface (3D engine framebuffer) instead of filling the
  target; backend import with sync primitives. Decided in doc 09.
- **Language & tooling**: C++20, minimal-vetted-deps core (codecs and GPU
  APIs live in plugins/backends), CMake presets, plugins as shared libraries
  with a one-line build helper. Decided in doc 10.
- **Time and video**: full video in v1 — timeline, layer time spans with
  rational rate maps, `time` in the render contract, transport, time-keyed
  caching with temporal prefetch and a render-free `quantize_time` grid query
  the compositor uses to snap requested times to native frames (achieved-time
  coalescing), and an image-sequence reference kind. Keyframed property
  animation and motion blur remain deferred. Decided in doc 11.
- **Audio**: full audio in v1 — audio as an optional content facet sharing
  the layer's placement, per-layer gain, working sample rate/layout per
  composition, spatialization as monitor policy (flat by default), device
  monitors mastering the transport clock via a lookahead engine, and
  sample-exact export. DSP chains, time-stretch, and full PDC deferred.
  Decided in doc 12.
- **Effects**: operators in v1 — effects are content consuming content
  through a public `PullService`, with core-visible input graphs (damage
  routing, aggregate revisions, cycle handling generalized from doc 05)
  and `identity()` pass-through; `org.arbc.fade` and `org.arbc.crossfade`
  ship as reference kinds; the effect library stays in plugins. Decided in
  doc 13.
- **Editing model**: full in v1 — immutable versioned `DocState` (the
  concrete form of the snapshot token), transactions with coalescing, a
  document-wide journal for cross-kind undo/redo, and the `Editable`
  content facet with the cheap-capture/structural-sharing discipline.
  Collaboration and delta-based journals deferred. Decided in doc 14.
- **SDLC and quality**: GitHub-hosted, trunk-based with a mechanized local
  gate and a red-main protocol; hard diff-coverage gate (≥90% changed
  lines); the design docs as an executable specification via a
  claims register; contract conformance suite shipped as public API;
  levelization enforced in CI; configs-as-style-guide formatting/linting.
  Decided in doc 16.
- **Internal components**: one shipped `libarbc` composed of levelized
  CMake object libraries (base → pool/media → surface/model → contract/
  cache/backend-cpu → engines/serialize/kinds → runtime → umbrella), with
  CI-enforced dependency edges; built-in kinds are codec-free and
  dual-built as dlopen plugins in CI (against the *static* `libarbc` — the
  public surface is not export-annotated yet, so every plugin still carries a
  private copy of the core objects it references; the shared-`libarbc` link is
  deferred to `packaging.shared_library_build`); imageseq ships as a separate
  plugin artifact carrying the codec dependency, and — by the same "codec line" —
  the audio device backend stays off `libarbc`'s link line: the `DeviceSink`
  interface and transport-mastering `DeviceMonitor` are dependency-free
  `runtime` objects, while the concrete OS-audio backend ships as a separate
  `arbc-plugin-<device>` artifact. Decided in doc 17.
- **Where a kernel lives**: split by *format templating*, not by "is it
  arithmetic". A loop parameterised on `(PixelFormat, ColorTransfer)` that
  reads or writes surface storage is `backend-cpu` (L3); format-agnostic
  DSP over already-decoded working samples — a filter's coefficient bank
  and tap combiner, taking `WorkingPixel`/float in and out — is `media`
  (L1). The L4 kinds can reach `media` but never `backend-cpu`, so `media`
  is the only floor where `kind-raster`'s mip pyramid and `backend-cpu`'s
  compositing kernels can share one byte-exact resampling filter instead of
  duplicating its frozen coefficient table across an un-crossable level
  boundary. Decided in doc 17 (`kinds.raster_resampling_quality`).
- **Async external loading**: a not-yet-loaded external child is a *model*
  state, not a render-completion state. The loader mints the child
  composition's id *before* fetching, so the embedding content binds a valid
  child id immediately and the parent load never blocks; until the bytes land
  that id names no record, which is already the placeholder state an
  unavailable reference leaves behind. Arrival installs the child under that
  same id in one ordinary transaction — a new revision plus damage on the
  embedding content, which doc 02's *Refine* step turns into a follow-up
  frame. The fetch may run off-thread; the install is marshalled onto the
  single model writer. So "async" costs a revision bump and a damage route,
  not a second placeholder type or a new content state. Decided in doc 05
  (`runtime.async_external_load`).
- **Memory model**: inside-out slab arenas (refcounts stored apart from
  immutable data, per the `poc-inside-out-objects` prototype) for document
  records and content state nodes, reimplemented inside arbc core and
  offered to plugins; version reclamation is pure refcount reachability
  with deferred cascades, so "GC policy" reduces to the existing journal
  and cache budgets. Arenas are mmapped from a per-document workspace file
  by default (crash recovery, demand paging, hole-punch release, a path to
  shared-mapping process isolation); records are index-only /
  position-independent; JSON (doc 08) remains the interchange format.
  Decided in doc 15.
- **Object identity namespaces**: the 64-bit `ObjectId` space is split by
  its top bit. The model allocator issues only bit-63-clear ids; the
  reserved half is the runtime's *synthesized-identity* namespace, used for
  graph nodes the model never named (an operator's inline input children,
  doc 13). Disjointness is therefore structural, not incidental: a
  synthesized cache identity can never alias a model object's id, so damage
  and cache invalidation on one namespace cannot evict the other's entries.
  Synthesized ids never persist — they are render-time state, never
  journaled or serialized. Decided in doc 14 (§ Identity).
- **Worker dispatch is leaf-only.** The render worker pool executes *leaf*
  renders only. An operator's `render` re-enters the `PullService`, whose
  cache probe/insert and descent-depth accounting are render-thread-
  confined, so dispatching one to a worker is a data race on the tile
  cache — not a performance trade-off. Operators therefore render inline on
  the driver thread and only leaves fan out. This bounds what the
  concurrency story can ever be (the parallelism lives at the leaves, and
  an operator-heavy scene parallelizes through its leaves, not its spine),
  so it is structural rather than advisory: the rule lives in one runtime
  helper that every worker-backed dispatch is obtained from, and a
  grep-lint keeps it the only one. Decided in doc 02 (§ Threading model).
- **A damage-gated frame clears and clips its repaint region, and that
  region is a disjoint rect set.** The target surface is the caller's and
  persists across frames, so a gated frame re-composites onto pixels a
  previous frame already painted — and source-over is not idempotent for
  translucent content. The frame therefore *clears* its device repaint
  region before re-compositing it, and *clips* every composite to that
  region so a tile straddling the edge cannot spill onto un-cleared pixels.
  The region is a set of **pairwise-disjoint** rects, normalized from the
  raw (overlapping) per-(damage, layer) dirty rects: disjointness is what
  makes "cleared once, repainted once" true per pixel, and it is what keeps
  two far-apart damages from repainting everything between them. This buys
  the frame loop its central invariant — a gated frame's repaint region
  equals what one full pass would have put there — at the cost of two
  clip-scoped operations (`clear_rect`, `composite_clipped`) on the backend
  contract that every backend, CPU and GPU, must implement. It is a scissor
  rect; GPU command lists already have one. Decided in doc 02 (§ The frame,
  interactively) and doc 09 (§ Backend contract).
- **The pending set is a suppression key, exactly like the cache.** A tile
  whose render is in flight is absent from the cache, so a cache-only miss
  test cannot tell it apart from a tile nobody has ever asked for — and
  re-dispatches it. Planning and pulling therefore consult the in-flight set
  as well, and a miss already in flight issues no render: an operator whose
  output spans several tiles stops re-rendering its shared input once per
  output tile, and a nested chain stops paying one render per level per
  refinement wave. What makes the join sound is that an arrival's damage is
  *broadcast* to every consumer of the tile rather than delivered to the
  caller that dispatched it, so collapsing N dispatches to one changes the
  work done and not the wake-ups delivered — no join primitive, no
  multi-waiter completion, no new contract type. The one carve-out is a
  *cancelled* render: cancellation is advisory and a cancelled render may
  fail, and a failed arrival is dropped with no damage, so suppressing
  against one would strand its tile behind a placeholder forever. Decided in
  doc 02 (§ The frame, interactively) and doc 13
  (`compositor.in_flight_tile_dedup`).
- **An operator chain renders at most twice on a cold cache: once to request
  its inputs and paint the placeholder, once when the wave lands.** Two is
  the *floor*, not the waste, and saying so is what finally bounds a cost doc
  02 had explicitly declined to bound. The driver does not know an operator's
  input tiles; it discovers them by rendering the operator and watching it
  pull. So with a worker pool an operator *must* render once to dispatch its
  inputs — and that render necessarily produces a placeholder, because the
  inputs it just dispatched have not landed — and must render again, once they
  have, to compose the real pixels. (At zero workers the floor is one, because
  `submit` *is* the render and the first render is already exact. An inline
  oracle is therefore the wrong thing to demand equality against: it measures
  a regime that structurally cannot have a placeholder pass.) What is *not* on
  the floor is re-rendering the chain once per **independently arriving input
  tile**, which is what a nested composition used to do — and it is why the
  recursive case cost more with workers than without, making doc 05's "only the
  spine re-renders" and "the recursive case must cost what an equivalent flat
  scene would" false in the async case. So an operator's transient tile records
  the inputs its render left unmet, and the chain is not re-driven until the
  last of them lands. The alternative that would reach the inline oracle's count
  — enumerate an operator's input tiles *without* rendering it — deadlocks: the
  input tiles are not knowable without the pull, and the pull does not happen
  without the render. Making them knowable means a new plugin-ABI entry point
  imposed on every operator author, to save the placeholder pass that *is* the
  first paint of the scene — which doc 02 sells as a feature ("progressively
  sharper content rather than blocking"). Decided in doc 02 (§ The frame,
  interactively) and doc 13
  (`compositor.operator_refinement_wave_amplification`).
- **The interactive driver ships with real threads, and the deadline is
  what buys them.** This is the first shipped configuration with a worker
  pool under the frame loop, and the argument for it is *correctness*, not
  throughput: with zero workers the pool is the degenerate inline executor,
  so `submit` is the render, the frame thread is inside a slow leaf when the
  deadline passes, and the "proceed with what you have — stale, coarser, or
  transparent" promise has nothing to proceed *with*. A degrade policy needs
  the render to be off the thread that has to give up on it. The count is a
  formula (hardware concurrency, less the frame thread, capped), not a
  constant: a constant is a measurement of the author's machine, and the cap
  exists because the pool is per-viewport. The offline driver keeps its
  inline-exact default — exactness has no deadline to miss. Decided in doc 02
  (§ Threading model).
- **The housekeeping thread drains; the writer checkpoints.** Deferred
  reclamation and durable checkpointing are both "housekeeping", but they
  do not share a thread. A drain may run on the low-priority thread
  concurrently with a writer transaction — which makes every seam a
  record's destructor reaches through (the runtime's content-state routing
  table, a kind's state store) a concurrent-read surface that must be
  synchronized. A `Checkpointer::commit` may not: it reads the allocator's
  high-water, seals full chunks read-only, and compacts the durability
  quarantine, all of which the writer mutates lock-free. Checkpoint cadence
  therefore decides *when* a commit happens, never *where* — every trigger,
  timer included, is evaluated on the writer thread. Decided in doc 15
  (§ Version reclamation, § File-backed arenas) and doc 14 (§ Editable).
- **A frame cancels the renders it no longer wants, not the renders it could
  not wait for.** The deadline bounds *the frame*, not *the work*. It is
  enforced by the frame refusing to park past it; the cancel that follows is a
  courtesy to the renderer, not the mechanism, so narrowing it costs nothing —
  the frame still returns on time with the best pixels it has. An expired frame
  therefore cancels only what it has stopped wanting (superseded by a revision
  bump, or no longer visible at the current camera and time) and leaves every
  still-wanted render **in flight**. That is what makes the in-flight join a
  *cross-frame* mechanism: cancel everything on expiry and every render that
  survives a frame boundary is disqualified from being joined — precisely under
  the load that made it worth joining. And the blanket sweep is not merely
  wasteful, it is unsound: cancellation is advisory, a conformant content may
  honor it and fail, and a failed render is dropped with no damage — so a tile
  the frame still wanted ends up in neither the cache nor the pending set, with
  nothing left to re-plan it, stranded behind a placeholder until an unrelated
  edit happens to repaint its region. Decided in doc 02 (§ The frame,
  interactively).
- **Imported images are referenced; painted pixels are stored — and the two
  are different kinds.** A document is a `.arbc` file plus a sibling asset
  directory. An *imported* photograph has a file it came from, so it
  serializes as a URI and nothing else: that is `org.arbc.image`, which
  carries a decoder and therefore lives outside `libarbc`, is read-only, and
  stores not one pixel in the project. *Painted* pixels have no such file —
  they are irreplaceable document state — so `org.arbc.raster` stays
  codec-free and editable and persists its copy-on-write tile table as
  content-addressed blobs in the asset directory, mips rebuilt on load. The
  split is not taxonomy. Storing an imported photograph as raster tiles costs
  roughly 490 MB where referencing it costs 32 MB (30-layer, 24 MP
  composition), and no compressor closes that gap: after content-addressed
  dedup, photographic tiles are 93% of the bytes and compress about 2.1x,
  because sensor noise is incompressible. Compression is the *weakest* lever
  here — dedup beats it — and the only lever that works is not copying the
  photograph in at all. The split also makes non-destructive editing
  structural rather than conventional: an `image` has no `Editable` facet, so
  retouching *must* stack a raster over it. Decided in doc 08 (§ The asset
  directory, Principles 3 and 8), doc 03 (§ Reference kinds) and doc 17
  (§ The codec line).

## Open questions

- HDR output transforms / tone mapping and OCIO-grade color: structurally
  accommodated (doc 07), unscheduled.
- Out-of-process plugin isolation: deferred; the async render path is the
  seam (doc 03).
- Projective per-layer transforms: deliberately excluded from v1 with the
  door open (doc 04).
