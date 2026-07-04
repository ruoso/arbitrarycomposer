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
  `params`, lossless round-trip of unknown kinds, external references by
  URI. Decided in doc 08.
- **Content-provided surfaces / texture adoption**: `RenderResult` may carry
  the content's own surface (3D engine framebuffer) instead of filling the
  target; backend import with sync primitives. Decided in doc 09.
- **Language & tooling**: C++20, minimal-vetted-deps core (codecs and GPU
  APIs live in plugins/backends), CMake presets, plugins as shared libraries
  with a one-line build helper. Decided in doc 10.
- **Time and video**: full video in v1 — timeline, layer time spans with
  rational rate maps, `time` in the render contract, transport, time-keyed
  caching with temporal prefetch, and an image-sequence reference kind.
  Keyframed property animation and motion blur remain deferred. Decided in
  doc 11.
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
  dual-built as dlopen plugins in CI; imageseq ships as a separate plugin
  artifact carrying the codec dependency. Decided in doc 17.
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

## Open questions

- HDR output transforms / tone mapping and OCIO-grade color: structurally
  accommodated (doc 07), unscheduled.
- Out-of-process plugin isolation: deferred; the async render path is the
  seam (doc 03).
- Projective per-layer transforms: deliberately excluded from v1 with the
  door open (doc 04).
