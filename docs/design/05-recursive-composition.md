# 05 — Recursive Composition

A composition embedded as a layer inside another composition is the design's
keystone feature: it proves the layer contract is truly arbitrary (the
compositor itself can live behind it), and it is what turns "deep zoom" into
"structurally infinite zoom".

## Semantics

The `org.arbc.nested` content kind wraps a reference to a child composition:

- **Local space** = the child's composition space, identically. The layer
  instance's transform in the parent is the only mapping between the two
  worlds; the nested content itself adds no scaling or cropping.
- **Bounds** = the child's canvas rectangle if declared, else the union of
  the child's layer bounds (recursively; unbounded if any layer is
  unbounded). Memoized, invalidated by child structural damage.
- **Scale range** = unbounded at the nesting boundary. The child's own
  layers each have their own limits, which take effect naturally when *they*
  are asked to render — nesting does not need to aggregate them.
- **Stability** = static iff every reachable child layer is static
  (memoized the same way as bounds).
- **Rendering** a request means: run the compositor over the child
  composition with a synthetic viewport — device rect = the request's target
  surface, camera = the request's region-to-surface mapping — and the same
  exactness/deadline/snapshot. Rendering *is* recursion.

Because the nested kind is implemented purely against public interfaces
(the layer contract downward, the compositor API inward), it doubles as the
proof that third-party plugins have enough API surface to do anything the
core can do.

Doc 13 later generalizes this shape: the nested kind is an *operator* —
content that consumes content — with the child composition as its single
input, built on the public `PullService`. The machinery this doc introduces
(aggregate revision, snapshot propagation, cycle handling, budget flow) is
the operator machinery; it applies to every effect kind, not just nesting.

## Sharing and instancing

A child composition may be embedded by many parents, or several times in one
parent (with different transforms) — content is shared, placement is
per-instance (doc 01). Layer-level tile caches inside the child are keyed by
the child's content identities, so all embeddings share them at matching
scale rungs; each embedding's *composed*-result cache is its own.

"Recursion into an entirely independent compose project" (a child loaded
from a separate file/project) is the same mechanism plus a loader; from the
compositor's perspective there is no difference between an inline child and
an external one. External loading is async by nature, and the nested kind's
async render path plus placeholder policy already cover the not-yet-loaded
state.

## Cycles

Composition references form a graph; cycles are representable (A embeds B
embeds A) and even *meaningful* — a cycle is a Droste-effect scene, and
combined with a scale-down transform it is a legitimate infinite-zoom object.
So the design does not forbid cycles at the model level. Instead:

- **Termination is guaranteed by the sub-pixel cull** (doc 04): recursion
  descends only while the composed scale keeps content above the visibility
  threshold. A cycle through a <1× scale factor bottoms out naturally after
  finitely many turns. This is the same rule that makes any deep nesting
  renderable, not a special case.
- **Cycles at scale ≥1×** (each turn as large or larger than the last) would
  genuinely diverge. The compositor carries a recursion-depth budget per
  request as a backstop; exceeding it renders the placeholder and reports a
  diagnostic naming the cycle. Editors can additionally warn at authoring
  time via a cheap reachability check.
- **Snapshot correctness**: within one frame/snapshot, every visit to the
  same composition sees the same revisions (doc 02's snapshot token rides
  along in `RenderRequest`), so even a Droste scene is self-consistent
  within a frame.

## Caching across the nesting boundary

Two cache layers cooperate (doc 02):

1. **Inside the child**: the child's layers cache their tiles as usual —
   shared by all embeddings.
2. **The composed result**: the nested layer's rendered output is cached by
   the parent like any other content's tiles, keyed by the child's
   *aggregate revision* (a composition-level revision bumped by any reachable
   change). Static children make deep hierarchies cheap: a 10-level tree
   re-renders only the spine from the edited layer to the viewed root.

The aggregate revision is also what damage propagation uses: child damage
maps through each embedding's transform into parent-space damage, which is
how an edit deep inside a shared component invalidates every place it
appears, at every viewport, at the right screen rectangles.

## Budgets across recursion

Deadline and cache-budget accounting flow *through* nesting rather than
being reset by it: the synthetic viewport inherits the outer request's
deadline, and tiles allocated inside a child count against the same global
cache budget (priority-classed by *outermost* visibility). Otherwise a
deep scene could multiply its resource entitlement by nesting — the
recursive case must cost what an equivalent flat scene would.
