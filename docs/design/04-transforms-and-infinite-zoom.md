# 04 — Transforms and Infinite Zoom

## Transform model

Per-layer placement is a full 2D affine transform: the 2×3 matrix

```
| a c tx |
| b d ty |
```

covering translate, rotate, uniform and non-uniform scale, and shear.

**Why affine, not projective, in v1.** Projective (3×3 with perspective)
would let a layer be "tilted into the page". The cost: scale is no longer
even locally two singular values — required resolution varies *across* the
layer, so tiles at one scale rung are wrong, culling and bounds mapping get
harder, and every layer implementation inherits the complexity. The design
keeps the door open (nothing in the layer contract mentions the transform,
only region + scale — a projective compositor could issue per-tile scales),
but affine covers the motivating use cases. A "3D scene view" layer kind
gets true perspective *inside* the layer anyway, which is where it usually
belongs.

**Decomposition is never stored.** Authoring tools like to think in
translate/rotate/scale components; the model stores only the matrix.
Component-wise editing is a UI concern with well-known decompositions, and
storing components invites ambiguity (order, pivot). Pivot points likewise:
an authoring convenience computed into the matrix, not model state.

## The precision problem

Infinite zoom breaks naive numerics. Two distinct failure modes:

**1. Catastrophic magnitude in composed transforms.** Nest compositions,
each scaling its child down by 1000×. Ten levels deep, the child-to-root
scale factor is 10⁻³⁰. A viewport anchored at the root that zooms onto that
child composes a camera scale of 10³⁰ against a layer scale of 10⁻³⁰ —
each representable in double, but their *translations* are the killer: the
layer's position in root space needs ~30 significant decimal digits to
resolve one on-screen pixel. Double has ~15.9. The screen jitters, then
freezes into quantized steps.

**2. Accumulated error.** Long chains of matrix multiplication accumulate
rounding error even at benign magnitudes; mostly harmless for display, but
it means composed transforms should always be recomputed from the stored
per-edge matrices, never incrementally updated.

## The solution: no global space, anchored viewports, rebasing

The design never requires representing "position in root space":

- **Transforms are stored per edge** (layer→parent), each individually
  well-conditioned. The pathological quantity only appears if you *compose to
  the root*, so: don't.
- **Viewports anchor to a node, not the root** (doc 01). The camera maps
  *anchor space* to device pixels. All rendering math composes transforms
  along the path between the anchor and the layers in view — a path that is
  short and numerically tame precisely *because* the user is looking at it
  (everything visible is within ~10⁷ scale of the screen, or it would be
  sub-pixel or astronomically clipped).
- **Rebasing**: as the user zooms in, the camera's scale relative to its
  anchor grows. When it exceeds a threshold (say 2¹⁶), the viewport
  re-anchors to a descendant node in view — new anchor, camera rebuilt
  relative to it, composed appearance unchanged (the switch is exact to
  within one double rounding, invisible at sub-pixel level). Zooming out
  re-anchors upward. The camera transform thus stays permanently
  well-conditioned; zoom depth becomes limited by *structure* (how deep the
  scene nests), not by float representation.
- **Culling from the viewport outward.** Culling walks top-down from the
  anchor composing viewport-relative transforms. A subtree whose composed
  scale drops below a sub-pixel threshold is culled without descending
  further — this simultaneously bounds numeric range and makes infinitely
  deep scenes renderable (only ~a few nesting levels are ever *visible*
  through a given viewport).

With this scheme, `double` everywhere is sufficient: every number the
pipeline actually computes with is viewport-relative and bounded. No
arbitrary-precision arithmetic, no fixed-point world coordinates.

**Consequence — anchors are part of viewport state.** "Camera position" is
`(anchor node, matrix)`, not a matrix alone. Serialized camera bookmarks and
host-visible camera APIs must speak that pair, and re-anchoring must be a
host-visible event (a host drawing overlays in anchor space needs to know).
This is the one place the precision strategy leaks into the public API, and
it is deliberate.

## Scale ladders and tile geometry

Interactive rendering quantizes requested scale to a ladder of powers of two
(…, ½, 1, 2, 4, …) in the layer's local space:

- Cache tiles are keyed by rung, so a smooth pinch-zoom reuses one rung's
  tiles across an octave instead of thrashing re-renders every frame.
- The remainder (≤1 octave) is applied as resampling during compositing —
  and by convention the ladder is chosen so tiles are *downsampled* (rung ≥
  needed scale) once the next rung is available, since minification looks
  better than magnification.
- During a zoom gesture, the compositor speculatively requests the next rung
  while displaying the current one — this is the progressive-refinement path
  from doc 02, and it is what makes "infinite zoom" *feel* continuous.

Rotation and non-uniform scale don't change the scheme: the request scale is
the larger singular value of the composed mapping (doc 01), tiles are
axis-aligned in *local* space, and the composite pass applies the full
affine.

## Numeric conventions

- All geometry in public APIs is `double`. (`float` halves cache traffic on
  GPU paths, but the conversion belongs inside backends, at the last moment,
  on viewport-relative values that are guaranteed small.)
- Composed transforms are recomputed from per-edge matrices on demand
  (memoized per frame), never incrementally accumulated across frames.
- Inverses are computed from the composed matrix with a conditioning check;
  a degenerate composed transform (scale ~0, or a singular authored matrix)
  culls the layer rather than propagating NaNs.
