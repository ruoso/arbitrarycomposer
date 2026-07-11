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

A frame therefore renders **exactly one composition's layers**: the compositor
walks the membership of the composition it was asked for, and a nested layer's
child is reached only by recursing through that layer's content. A child
composition's layers are never also drawn by the enclosing walk — they are not
the enclosing composition's members, and drawing them there would both
double-draw them and do it with child-local transforms, outside the nesting
layer's embedding. The audio mix already reads this way (it takes the
composition to mix); the visual frame walk must too.

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
an external one.

External loading is async by nature, and the not-yet-loaded state is a
**model** state, not a render-completion one. The loader mints the child
composition's `ObjectId` before it fetches a single byte (the
allocate-before-parse rule below), so the embedding content binds a *valid*
child id immediately and the parent document finishes loading without waiting
on the fetch. Until the bytes arrive that id names no composition record — and
a child id naming no record is exactly the state the nested kind already
renders as the placeholder. When the bytes do arrive, the loader installs the
child's composition graph under that same pre-allocated id in one ordinary
transaction (doc 14), which publishes a **new revision** and flushes damage
naming the **embedding content**; doc 02's *Refine* step turns that damage
into a follow-up frame, and the placeholder is replaced live. The arrival edge
therefore costs a revision bump and a damage route — ordinary model wiring —
and adds no second placeholder type, no new content state, and no change to
render, audio, aggregate revision, damage routing or tile caching.

*Pending* and *unavailable* thus differ in exactly one bit, and only for the
loader: a pending reference holds a valid child id whose record is not there
*yet*; an unavailable one holds a null child id and never will. Both render
the placeholder, both keep their `ref`, and both re-save byte-identically as
the authored URI. The fetch may run on any thread; the install and its damage
are marshalled onto the single writer thread that owns the model (docs 14,
15). Loading a file is async — mutating the document is not.

The loaded child is installed as an ordinary composition in the **host
document's model**, so render, aggregate revision, damage routing and tile
caching see no difference between an inline child and an external one — the
"plus a loader" is the whole of the difference. Load-time termination of an
external cycle (A embeds B embeds A; a document embedding *itself*) is
guaranteed the same way the in-document Droste knot is cut (doc 08 Principle
7): the loader records the child's composition id in its resolved-identity map
*before* parsing the child's bytes, so a back-edge resolves to the in-flight
composition instead of re-loading it. A non-cyclic external chain is bounded
by a load-time depth cap; exceeding it, like a missing or unreadable file,
makes the reference **unavailable** — the embedding content keeps its `ref`
and renders the placeholder.

Persistence follows the same split (doc 08): an in-document child
serializes into the document-level `compositions` table, the nested
content naming it by a core-owned id — the core reads the reference off
the content's `composition_ref()` accessor (doc 03), the exact mirror of
`inputs()` — cycles included, so a Droste scene round-trips as data (the
root's reserved ordinal `"0"` is what a back-edge names), while an
external child stays a kind-owned `params.ref` URI resolved through the
loader.

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
