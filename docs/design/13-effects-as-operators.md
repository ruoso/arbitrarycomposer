# 13 — Effects as Content Operators

Effects enter the model as **content that consumes content**: an operator
wraps one or more inputs, transforms their output, and presents itself as
ordinary content. A fade-in on a video clip is not a property of the layer —
the layer's content slot holds a fade operator whose input is the clip. The
layer stack therefore holds, in general, a DAG of content. Status: designed;
scheduling recorded at the end.

## Why operators, not effect lists

The alternative — a per-layer effect *chain* as a placement property — is
strictly less expressive and adds a second concept to the core. Operators
need no new rendering concept at all: an operator implements the doc-03
contract, so the compositor composes, caches, schedules, and recurses
through effects without knowing they exist. What falls out for free:

- **Chains** are just nesting: `color-grade(blur(clip))`.
- **Multi-input effects** are natural: crossfade(a, b), matte(source,
  mask), displacement(source, map) — things an effect-list model handles
  awkwardly or not at all. Blend modes and masks, listed as extension
  points since doc 00, become *operator kinds*; layer placement keeps only
  source-over + opacity.
- **Both facets** (doc 12): a fade operator attenuates alpha *and* gain, so
  a video fade-in fades sound and picture coherently through one node.
  Operators may be facet-selective (a blur passes audio through untouched;
  a reverb passes pixels through untouched).
- **Sharing**: an operator's output is content — several layers can
  reference the same graded clip, with one cache.
- Editors that want an AE-style "effect stack on a layer" UI build it as
  authoring sugar over a linear operator chain.

**Doc 05 is retroactively a special case**: the nested-composition kind is
an operator whose single input is a composition. The machinery invented
there — aggregate revision, snapshot propagation through pulls, cycle
handling, budget flow — is exactly the operator machinery; this doc
generalizes it rather than duplicating it.

## The operator contract

Three additions to the doc-03 interface make operators first-class, and the
first two are things the nested kind needed anyway:

```cpp
class Content {
  // … as in docs 03/11/12 …

  // --- graph structure (NEW) ---
  // Operator inputs, visible to the core. The core uses this for aggregate
  // revisions, snapshot consistency, cycle detection, and damage routing —
  // the doc-05 machinery, generalized. Leaf content returns empty.
  virtual std::span<const ContentRef> inputs() const { return {}; }

  // Map damage on an input into damage on this content's output.
  // Default: identity. A blur inflates by its radius; a warp maps through
  // its distortion. The core calls this to propagate invalidation.
  virtual Rect map_input_damage(size_t input, const Rect&) const;

  // --- pass-through (NEW) ---
  // If, for this request, the output is exactly input N's output (a fade
  // at envelope == 1, a disabled effect), say so: the compositor serves
  // the input's cached tiles directly — no render, no copy, no new cache
  // entry. The OpenFX "identity action", and what makes wrapping an
  // entire clip in a fade cost nothing outside the fade window.
  virtual std::optional<size_t> identity(const RenderRequest&) const;
};
```

**Pulling inputs goes through the core.** Operators do not call
`input->render()` directly — that would bypass caching, scheduling,
snapshots, and budgets. At attach, content receives a `PullService`:

```cpp
class PullService {
public:
  // Same machinery as a compositor-issued request: cache lookup first,
  // worker scheduling, snapshot token respected, deadline/budget inherited.
  void pull(ContentRef, const RenderRequest&,
            std::shared_ptr<RenderCompletion>);
  void pull_audio(ContentRef, const AudioRequest&,
                  std::shared_ptr<RenderCompletion>);
};
```

This is the one genuinely new core API, and it is the doc-05 synthetic
viewport generalized: the request/cache machinery, exposed as a service to
any content. The nested kind reimplements on top of it; third-party
operators get the proof (doc 05's closing argument) that plugins can do
anything the core can.

**A pull delivers into the caller's target.** `pull` writes the pulled
input's pixels into the request's `target` surface — the visual analog of
`pull_audio` writing samples into `AudioRequest.target` — on every path
where the pixels are available synchronously: a resident cache hit
composites the input's tile(s) into `target`, and a miss that settles
inline composites the freshly-rendered tile(s) into `target`, honoring the
request's region and scale (doc 09's `provided`-surface contract, same
reasoning). A request whose region spans more than one tile is served
across every covering tile of `tiles_covering(rung, region)` — each
independently cache-probed and delivered into its own sub-rect of `target`;
the caller's completion settles once from the aggregate, exact iff every
covering tile is exact at the selected rung. The operator then composites a
`target` that actually holds its input's pixels. A pull whose input answers
*asynchronously* — any covering tile — delivers nothing usable this pass:
the completion is left unsettled, the operator degrades for this frame, and
each async tile's arrival re-drives it (below).

Metadata composes synchronously: an operator computes `bounds()`,
`time_extent()`, `scale_range()`, `stability()` by querying its inputs and
applying its own contribution — a blur inflates bounds by its radius; a
fade is `Timed` even over `Static` input (its envelope depends on time); a
crossfade's extent is the union of its inputs'.

## Region, scale, and time dependencies

The pull contract makes dependency negotiation implicit — the operator
simply asks for what it needs (where OpenFX needs explicit RoI/RoD
actions, and its `renderScale` discipline is the precedent for ours):

- **Region**: a fade pulls the same region; a blur pulls the region
  inflated by its radius (and declares radius units — content-local or
  output-pixel — resolving them against `scale`); a warp pulls through its
  inverse mapping. `map_input_damage` is the same mapping in reverse, so
  declaring it is not extra design work. Its contract is a **covering**
  one: the mapped output damage must cover every output pixel the input
  change can affect — over-approximation is sound (extra pixels merely
  re-render), under-approximation is a correctness bug (stale pixels
  survive an edit).
- **Scale**: passes through by default. An operator that is resolution-
  bounded (a stylize working at a fixed grain) reports `achieved_scale`
  honestly like any raster. Downsample-then-upsample tricks (cheap heavy
  blur) are internal choices, still reported honestly.
- **Time**: a fade evaluates its envelope at the request's time and pulls
  the input at that same time. Temporal operators pull *multiple* times
  (echo/trails pull t, t−δ, t−2δ; temporal blur integrates a shutter
  window before doc 11's motion-blur seam exists natively) — each an
  ordinary pull, each hitting the time-keyed cache.
- **Audio**: identical shape one dimension down — a reverb pulls the input
  window extended backward; a gain envelope multiplies in place; latency
  declaration (doc 12) covers operators that genuinely delay. The lookahead
  engine and block cache are unchanged.

## Caching and scheduling

An operator's output caches like any content: keyed by its id and its
*aggregate* revision (doc 05's mechanism, now driven by the core-visible
`inputs()` graph). Two-level caching applies as in doc 05 — input tiles
cache under the input's identity (shared by every consumer), operator
output under the operator's. `identity()` short-circuits both levels.
Async composes: an operator whose input answers asynchronously is itself
asynchronous via the same completion plumbing; deadlines and cache budgets
flow through pulls exactly as they flow through nesting (doc 05's budget
rule, same reasoning). The pull core retains a dispatched render's target
surface until that render settles, so a caller abandoning its completion
(`RenderCompletion::cancel`) never frees a surface an in-flight worker is
still writing; a pull configured without an async reap sink must be given a
synchronous dispatch, so its render settles before `pull` returns rather
than dangling the surface.

Cycles in the operator graph are feedback (video feedback, audio echo
loops) and get doc 05's rules verbatim: convergent cycles (spatial scale
< 1, or gain < 1, or time-offset with finite span) terminate by the
sub-pixel / sub-audible cull; divergent ones hit the recursion-depth
budget and the cycle diagnostic.

## Serialization (amends doc 08)

Input edges are core-owned structure, so they serialize outside `params`,
mirroring `inputs()`:

```jsonc
{
  "kind": "org.arbc.fade",
  "inputs": [ { "kind": "org.arbc.imageseq",
                "params": { "pattern": "shots/a-%04d.png" } } ],
  "params": { "shape": "linear", "in": [0, 1.0], "out": null },
  "transform": [1, 0, 0, 1, 0, 0]
}
```

Inline nesting covers chains; for *shared* content the document gains an
optional `"contents"` table of id → content description, referenced as
`{"$ref": "id"}` from any `inputs` slot or layer. Unknown-kind
round-tripping (doc 08 principle 2) extends naturally: a placeholder
operator preserves `kind`/`params` *and* its inputs — and may even render
input 0 as its pass-through, so a missing fade plugin degrades to an
unfaded clip rather than a hole.

## Reference kinds (added to doc 03's table)

| Kind id | Proves |
| --- | --- |
| `org.arbc.fade` | Single-input operator, both facets (alpha + gain envelope), `Timed`-over-`Static` aggregation, `identity()` pass-through outside the envelope window. |
| `org.arbc.crossfade` | Two-input operator, extent union, per-input time maps — the temporal transition primitive (with spans, enough to build an NLE's cuts-and-dissolves). |

A blur (region inflation + damage mapping + radius-vs-scale resolution)
is the natural third, as a non-core example plugin.

## New machinery (honest cost list)

1. `PullService` — the real one: request/cache/snapshot/budget machinery
   exposed to content. Mostly *moves* the doc-05 synthetic-viewport
   internals behind a public seam.
2. `inputs()` + core-side graph awareness (aggregate revision, damage
   routing via `map_input_damage`, cycle detection generalized from
   doc 05).
3. `identity()` short-circuit in request planning.
4. Serialization: core-owned `inputs` arrays + the shared-`contents`
   table.
5. Two reference operator kinds.

Nothing changes in: transforms/zoom (doc 04), color (doc 07 — operators
receive and produce working-space surfaces like everyone), surfaces
(doc 09 — provided surfaces flow through pulls), the audio engine (doc 12).

## Deferred

A rich effect library (grades, keys, stylizes), GPU effect kernels (the
doc 09 backend op set is the seam; v1 operators are CPU code over working-
space surfaces), non-causal audio DSP beyond declared latency, and
keyframed *parameters* on operators — which is the same deferral as
keyframed placement (doc 11): when property animation lands, operator
params get it through the same mechanism.

## Scheduling decision

**Decision: mechanism + reference operators in v1.** `PullService`,
`inputs()` with damage routing, `identity()`, the serialization support,
and the `org.arbc.fade` / `org.arbc.crossfade` kinds all ship in v1; the
nested-composition kind is built *on* `PullService` from the start (never
built twice). With spans (doc 11), audio (doc 12), and crossfades, v1 is
capable of real cuts-and-dissolves editing end to end. The effect
*library* — grades, keys, stylizes, blend-mode and matte operators, audio
DSP — remains plugin territory per doc 00's non-goals.
