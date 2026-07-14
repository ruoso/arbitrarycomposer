# 01 — Core Concepts

This document defines the vocabulary and the object model. Everything here is
renderer-agnostic; the pipeline that evaluates it is doc 02.

## Composition

A **composition** is an ordered list (bottom to top) of **layer instances**,
plus composition-wide properties (background, working color space). It is the
unit of authoring and the unit of recursion: a composition can appear as a
layer inside another composition.

A composition has its own 2D coordinate space, the **composition space**.
It has no intrinsic size or resolution — it is an unbounded plane. A
composition *may* declare an authored **canvas rectangle** (a region of
interest in composition space) as a hint for viewers and for embedding
("fit this composition into that rectangle"), but nothing clips to it by
default.

## Layer instance

A **layer instance** is the pairing of:

- a **content** object (the pluggable part — see doc 03), and
- **placement** in the parent composition:
  - `transform`: a 2D affine transform from the content's local space to the
    parent composition space,
  - `opacity`: scalar 0..1,
  - `blend`: blend operation (v1: source-over only),
  - `visible`: flag,
  - `gain` and `audible`: the audio siblings of opacity and visible
    (doc 12),
  - **temporal placement**: a time `span` on the parent's timeline and a
    rate map from parent time to content-local time (doc 11) — shared by
    the visual and audio facets, so a layer's sound can never drift from
    its pixels.

The same content object may be referenced by multiple layer instances (in the
same or different compositions) — content is shared, placement is per
instance. This falls out naturally from the pull-based contract and makes
things like "the same live 3D scene visible in two places" free.

## Content and layer kinds

**Content** is anything implementing the layer interface. Its defining
property is its **local coordinate space**: an unbounded 2D plane in which
the content knows how to render any requested region at any requested scale
(subject to its own limits).

Content declares:

- **bounds**: the region of local space where it may produce non-transparent
  pixels. May be finite (a raster image), infinite (a procedural background),
  or unknown-until-asked. Bounds are how the compositor culls.
- **scale range**: the range of scales (device pixels per local unit) at
  which it can produce faithful output. A raster image is faithful up to its
  native resolution and degrades above it; vector or procedural content
  reports an unbounded range. This drives zoom-limit UX and lets the
  compositor know when upscaling a cached tile is lossless vs lossy.
- **stability**: whether repeated identical requests produce identical
  pixels — `Static` (time-invariant), `Timed` (deterministic function of
  request time, e.g. a video clip), or `Live` (non-deterministic, e.g. a
  camera feed). This gates caching; see doc 11 for the temporal cases.

Anticipated kinds (only the first three are core deliverables; the rest are
plugin proofs of the design):

| Kind | Local space | Scale range | Notes |
| --- | --- | --- | --- |
| Solid / gradient | infinite | unbounded | trivial reference implementation |
| Raster image | image rect, 1 unit = 1 source pixel | up to native | tiles + mip pyramid from source |
| Nested composition | the child's composition space | unbounded* | recursion; see doc 05 |
| Vector document | document units | unbounded | tessellate/rasterize at requested scale |
| 3D scene view | camera image plane | unbounded | request scale sets render resolution |
| Procedural | infinite | unbounded | fractals, noise, tilings |

\* bounded by the scale ranges of what's inside, recursively.

## Transforms

Layer transforms are full 2D affine (translate, rotate, scale — including
non-uniform — and shear), represented as the standard 2×3 matrix. Design
notes, including why affine and not projective in v1 and how precision is
managed under deep zoom, are in doc 04.

Transforms compose multiplicatively down the nesting hierarchy. For any layer
in view, the compositor resolves a single affine mapping between viewport
device pixels and that layer's local space; the scale factor extracted from
that mapping is the resolution the layer is asked to render at.

## Viewport

A **viewport** is the consumer side: a device-pixel rectangle (the output
surface) plus a mapping into a composition. It is defined by:

- an **anchor**: the composition (or, for deep zoom, a specific nested layer
  — see doc 04) whose space the camera coordinates live in,
- a **camera transform**: affine mapping from anchor space to device pixels,
- a **transport**: the viewport's clock on the composition's timeline —
  play/pause/seek/rate (doc 11). Per-viewport, like the camera: two views
  may observe the same composition at different times.

The audio-side consumer is the **monitor** (doc 12): attached to a
transport, it pulls the mixed audio the way a viewport pulls composed
pixels; a device monitor's hardware clock masters the transport, and video
chases it.

Pan/zoom/rotate of the view are edits to the camera transform. Multiple
viewports may observe the same composition simultaneously (an editor with an
overview and a detail view), which is again free under pull-based rendering.

A viewport observes a **document**, and binding it to one is the host's single
wiring step. The document is what holds the things a frame needs, so it is
the document that supplies them — not the host, by hand: the id→content
resolution the compositor's walk performs, the damage seam that wakes the frame
(doc 02 step 1), the settle point at which a late external child (doc 05)
is installed on a new revision, and the content graph each frame binds to give
its operators their render-time services (doc 13). A host that owns a document
therefore constructs a viewport directly against it; it never assembles those
seams itself, never attaches an operator by hand, and never needs a reference to
the versioned model underneath. Several viewports bound to
one document each observe every damage batch it publishes (above).

The offline renderer is just a viewport with no deadline: "this rectangle of
composition space onto this W×H image, exact."

## Resolution model

The central quantity everywhere is **scale**: device pixels per local unit,
derived per layer from the composed transform. (Under non-uniform scale or
rotation the mapping has two singular values; the *requested* scale is the
larger, so quality is sufficient along the most-magnified axis.)

Rules:

- The compositor **requests**; content **renders at the requested scale**.
  Content must not assume any global resolution.
- Requested scales are quantized to a ladder (powers of two around 1.0) for
  cache efficiency; the compositor resamples the ≤1-octave remainder. Exact
  (offline) requests are not quantized.
- Content that cannot reach the requested scale (raster beyond native)
  renders at its maximum and reports it; the compositor upscales and can
  surface "beyond native resolution" to the host.

## Invalidation

Content pushes **damage**, never pixels: "region R of my local space changed"
(R may be everything). Placement changes (transform, opacity, order) generate
damage in the parent automatically. Damage propagates up through nesting to
every viewport observing an affected composition; each viewport maps it to a
dirty device region and schedules re-rendering. This is the only path by
which anything re-renders in interactive mode; offline mode ignores damage
(each frame is evaluated fresh or against explicit caches).

## Identity and versioning

Every content object and every layer instance carries its own **revision**,
bumped on any change and increasing across successive edits. Cache keys are
`(content identity, revision, region, quantized scale)`. This keeps caching
correct without the cache needing to understand what changed — and it keeps
caching *selective*: an edit to one object leaves every other object's keys
untouched, so a static sibling's tiles survive an unrelated edit.

Identity is a stable per-object `ObjectId`, and "any change" is concretely
a transaction publishing a new immutable document version — the editing
data model, including undo/redo and the `Editable` content facet, is
doc 14. Undo is the one case where a revision does not increase: it
republishes the object's previous record verbatim, so the revision is
*restored* rather than advanced (doc 14, § Per-object revisions), which is
what makes the pre-edit tiles reachable again instead of merely stale.
