# compositor.anchored_viewports — Anchored viewports + rebasing

## TaskJuggler entry

`tasks/35-compositor.tji:39-44` → `compositor.anchored_viewports` ("Anchored
viewports + rebasing"), the sixth leaf under `task compositor`. It carries its
own `depends !tile_planning` (`35-compositor.tji:42`) and, through the parent
`task compositor`, inherits `depends contract.async_render, cache.key_shapes,
color.resampling` (`35-compositor.tji:7`). No sibling chains off it — it is a
leaf consumer, with `runtime.host_objects` (`65-runtime.tji:25-29`, "anchored
cameras as (anchor, matrix) pairs with re-anchor events surfaced") the
downstream host-facing consumer of what it lands. Note line:

> "Camera = (anchor node, matrix); re-anchor past the 2^16 scale threshold;
> culling walks viewport-outward with sub-pixel cull; deep-nesting numeric
> property tests across rebase thresholds. Doc 04."

## Effort estimate

**4d.** Twice the scale-ladder leaf and equal to tile_planning, and rightly:
this task is not one arithmetic module but three coupled pieces of the
deep-zoom precision story that doc 04 makes normative — (1) promoting the
camera from a bare `Affine` to the `(anchor node, matrix)` pair doc 04:81-84
requires, keeping today's flat single-composition render byte-identical when
the anchor is the root; (2) a pure `rebase` decision function that, when the
composed anchor→device scale crosses the well-conditioned band, re-picks the
anchor node (descendant on zoom-in, ancestor on zoom-out) and rebuilds the
matrix relative to it, composed appearance preserved to within one double
rounding, emitting a host-visible re-anchor event; (3) generalizing
`render_frame`'s per-layer cull into the viewport-outward walk that composes
transforms from the anchor and culls a subtree the moment its composed scale
drops sub-pixel. It reuses `Affine::max_scale()`/`inverse()`
(`src/base/arbc/base/transform.hpp:29,34`), `rung_scale`
(`scale_ladder.hpp:84`), and the exact cull/compose predicates
`render_frame` already uses (`compositor.cpp:16-46`); it builds new the anchor
field, the rebase function, and the numeric property-test harness. The
persistent cross-frame anchor state and the host-API surfacing of the
re-anchor event are runtime (L5) policy, not built here (see Decisions).

## Inherited dependencies

**Settled:**

- `compositor.tile_planning` (commit `5d3f6c2`, DONE 2026-07-05) — the direct
  predecessor via `depends !tile_planning`. Delivered the interactive planner
  and the synchronous tiled driver `render_frame_interactive`
  (`src/compositor/arbc/compositor/tile_planning.hpp:155-159`), and — load
  bearing here — **explicitly deferred the anchor-walk / viewport-outward cull
  and the `2^16` re-anchor threshold to this task**, reusing `render_frame`'s
  flat bounds-intersect cull in the meantime
  (`tasks/refinements/compositor/tile_planning.md`, "Not this task"). Its
  `LayerTilePlan` (`tile_planning.hpp:105-114`) and pure/allocation-free/
  lock-free planning rule (doc 02:123-125) are the contract this task's walk
  must not break.
- `compositor.scale_ladder` (commit `2b74aa6`, DONE 2026-07-05) — the transitive
  dep (tile_planning's `!scale_ladder`). Its `rung_scale(ScaleRung) = 2^index`
  (`scale_ladder.hpp:84`; orientation: **higher rung index = finer**) is the
  scale algebra the threshold math shares, and it pre-cited this task as the
  owner of the `2^16` figure: "handled by viewport rebasing
  (`compositor.anchored_viewports`, doc 04:63-66), a separate mechanism the
  ladder does not conflate" (`scale_ladder.hpp:82-83`).
- `cache.key_shapes` (commit `a198981`, DONE 2026-07-05) — parent-inherited.
  Supplies `ScaleRung` (`src/cache/arbc/cache/key_shapes.hpp:39`) and the
  `TileCache` the generalized walk still feeds; this task adds no new cache key.
- `contract.async_render` (commit `92c3d3b`, DONE 2026-07-05) — parent-inherited.
  The `RenderRequest`/`RenderCompletion` settle path the walk keeps driving
  unchanged; a sub-pixel-culled subtree issues **no** request against it.
- `color.resampling` (commit `fa895d5`, DONE 2026-07-05) — parent-inherited.
  The composite/downsample tap; unchanged by this task (rebasing touches
  geometry, not resampling).

**Pending:** none — every predecessor is landed.

## What this task is

Doc 04's thesis is that ArbitraryComposer has **no global coordinate space**:
the camera maps *anchor space* — not root space — to device pixels, and as the
user zooms the viewport re-anchors to a node in view so the transform the
pipeline actually computes with stays permanently well-conditioned
(`04-transforms-and-infinite-zoom.md:49-86`). This task concretizes that thesis
into `arbc::compositor` in three coupled pieces:

1. **Camera = (anchor node, matrix).** Extend `Viewport`
   (`src/compositor/arbc/compositor/compositor.hpp:16-20`) with an
   `ObjectId anchor` — the composition node, or for deep zoom a specific nested
   layer, the camera is pinned to (doc 01:96, doc 04:56-61). `Viewport::camera`
   remains the `Affine` but now maps **anchor-local space → device pixels**, not
   root→device. The invariant with `anchor == <root composition id>` reproduces
   today's semantics exactly (the walk still composes `camera` with each layer's
   transform).

2. **Rebasing.** A pure function that, given the current `(anchor, camera)` and
   the transform chain reachable through the resolver, tests whether the
   composed anchor→device scale (`camera.max_scale()`, the larger singular value,
   `transform.hpp:31-34`) has left the well-conditioned band — above the
   `2^16` re-anchor threshold on zoom-in, or below its reciprocal on zoom-out.
   When it has, it picks a new anchor node in view (a descendant on zoom-in, an
   ancestor on zoom-out), rebuilds the camera matrix relative to it so the
   composed on-screen mapping is unchanged to within one double rounding, and
   reports the re-anchor as a host-visible event (doc 04:62-69, 81-84). The
   camera transform thereby stays well-conditioned at any zoom depth; depth is
   bounded by *structure* (how deep the scene nests), not by float
   representation.

3. **Viewport-outward culling.** Generalize `render_frame`'s per-layer walk
   (`compositor.cpp:16-46`) so it walks from the anchor node outward, composing
   viewport-relative transforms on demand, and culls a subtree the moment its
   composed scale drops below a sub-pixel threshold — never descending further
   (doc 04:70-75). This simultaneously bounds the numeric range every computed
   number sees and makes infinitely deep scenes renderable, since only a few
   nesting levels are ever visible through a given viewport.

The deliverable is the extended `Viewport`, the pure rebase function and its
re-anchor event value, the generalized cull walk, and a numeric property-test
harness that pins the precision guarantees across rebase thresholds. It is a
**pure per-frame library** contribution: no persistent state, no host-API
surface (Decision 4).

## Why it needs to be done

Deep zoom is a headline capability (doc 00:95, "what makes deep zoom
numerically viable"). Without rebasing, a scene nested ~10 levels at 1000×/level
needs ~30 significant decimal digits to resolve one on-screen pixel; `double`
carries ~15.9, so the screen "jitters, then freezes into quantized steps"
(`04-transforms-and-infinite-zoom.md:35-42`). tile_planning deliberately left
this gap — it reuses `render_frame`'s flat bounds cull and pinned every
`2^16`/rebasing concern here (`tasks/refinements/compositor/tile_planning.md`,
"Not this task"). The downstream consumer `runtime.host_objects`
(`65-runtime.tji:25-29`) already promises the host "anchored cameras as
(anchor, matrix) pairs with re-anchor events surfaced" — it cannot deliver that
pair or those events until this task lands the compositor-side representation
and the rebase function that produces them. The generalized cull walk is also
the precondition doc 04:70-75 makes for rendering arbitrarily deep scenes at
all: without the sub-pixel cutoff a nested scene descends without bound.

## Inputs / context

Governing normative text (doc 04, cited by the note line):

- `docs/design/04-transforms-and-infinite-zoom.md:3-29` — the transform model:
  a 2×3 affine (`| a c tx / b d ty |`); decomposition/pivots never stored. Matches
  `struct Affine` (`transform.hpp:12-40`).
- `:31-47` — the precision problem: the ~30-digit failure mode (`:35-42`) and
  the rule "composed transforms should always be recomputed from the stored
  per-edge matrices, never incrementally updated" (`:44-47`), already honored by
  `compose()` on demand (`transform.hpp:42-43`, `compositor.cpp:25-26`).
- `:49-61` — anchored viewports: the camera maps *anchor space* to device
  pixels; everything visible is within ~10⁷ scale of the screen.
- `:62-69` — **rebasing**: "when it exceeds a threshold (say 2¹⁶), the viewport
  re-anchors to a descendant node in view — new anchor, camera rebuilt relative
  to it, composed appearance unchanged (the switch is exact to within one double
  rounding, invisible at sub-pixel level). Zooming out re-anchors upward." This
  is *the* governing paragraph.
- `:70-75` — **culling from the viewport outward**: top-down from the anchor
  composing viewport-relative transforms; a subtree whose composed scale drops
  below a sub-pixel threshold is culled without descending.
- `:77-79`, `:108-117` — numeric conventions: `double` everywhere is sufficient;
  all public geometry is `double`, float32 only inside backends on
  guaranteed-small viewport-relative values; composed transforms memoized
  per-frame, never accumulated across frames; a degenerate composed transform
  culls the layer rather than propagating NaNs.
- `:81-84` — **anchors are part of viewport state**: "'Camera position' is
  `(anchor node, matrix)`, not a matrix alone … re-anchoring must be a
  host-visible event." The representation mandate.

Anchor definition and cross-refs:

- `docs/design/01-core-concepts.md:96,98` — the anchor is "the composition (or,
  for deep zoom, a specific nested layer)"; the camera transform is the "affine
  mapping from anchor space to device pixels."
- `docs/design/02-architecture.md:53` — "Resolve and cull. Walk the composition
  from the viewport's anchor" — the architecture-level counterpart of the
  outward cull walk.
- `docs/design/16-sdlc-and-quality.md:64` — mandates "rebase-continuity checks"
  among the required quality checks; `:14-25` the claims register form; `:47-53`
  byte-exact goldens; `:54-62` behavioral-counter tests.
- `docs/design/10-tooling-and-packaging.md:74` — testing note requiring
  "stability across rebase thresholds," which this task's property tests satisfy.

Source seams this task extends:

- `src/compositor/arbc/compositor/compositor.hpp:16-20` — `struct Viewport
  { int width, height; Affine camera; }`; the header comment at `:13-15` already
  flags "Anchoring and rebasing (doc 04) land with deep-zoom support" as the
  extension point. `:22-24` `ContentResolver` (`std::function<Content*(ObjectId)>`)
  — the graph handle the walk resolves anchors through.
- `src/compositor/compositor.cpp:16-46` — the per-layer cull/compose/region walk
  reused as the outward walk's kernel: `compose(viewport.camera, layer.transform)`
  (`:26`), degenerate `inverse()` cull (`:27-30`, the doc 04:115-117 no-NaN
  guarantee — reused verbatim), `map_rect` region intersect (`:35-41`),
  `max_scale()` positive-finite cull (`:43-46`), sub-pixel `temp_width/height <=
  0` cull (`:47-50`).
- `src/base/arbc/base/transform.hpp:12-43` — `Affine` (six `double`),
  `max_scale()` (`:31-34`, the conditioning quantity rebasing protects),
  `inverse()` (`:27-29`, `nullopt` on degenerate/non-finite), `compose(outer,
  inner)` (`:42-43`).
- `src/base/arbc/base/ids.hpp:11` — `struct ObjectId` (a `std::uint64_t`, zero
  invalid): the node-handle type an anchor points at (model records reference
  each other by `ObjectId`, `src/model/arbc/model/records.hpp:60-102`).
- `src/compositor/arbc/compositor/scale_ladder.hpp:82-84` — `rung_scale` and the
  reservation note pointing the `2^16` figure here.
- `src/compositor/arbc/compositor/tile_planning.hpp:155-159` — the interactive
  driver whose front-half cull this task generalizes; it takes the `Viewport` by
  const-ref, so the anchor field flows in without a signature change.
- Test siblings: `src/compositor/t/{scale_ladder,tile_planning,refinement}.t.cpp`
  (Catch2, `.t.cpp`); goldens `tests/*_golden.t.cpp`; claims
  `tests/claims/registry.tsv` (TAB-separated `<id>\t<description>`), enforced by
  `scripts/check_claims.py` and `// enforces: <id>` test comments.

## Constraints / requirements

- **Levelization: `arbc::compositor` is Level 4 (doc 17:56).** Depends only on
  `contract`, `cache`, and their lower closure (`base`, `model`, `surface`, …).
  No direct `backend-cpu` edge (kernels reached only through the abstract L2
  `surface::Backend` seam); no same-level edge. `src/compositor/CMakeLists.txt`
  keeps `DEPENDS contract cache` unchanged — this task adds no dependency.
- **Pure per-frame library; inter-frame state is runtime (L5).** The compositor
  never holds the `Viewport` across frames. The rebase function is pure: it
  takes the current `(anchor, camera)` and returns the new pair plus the
  re-anchor event as a **value**; the caller (runtime) owns the persistent
  camera and applies the result — the same "caller owns the state, planning
  stays pure" posture as `refinement.hpp:59-62` and `render_frame_interactive`'s
  value-stamped deadline (doc 17:60,78-80). No wall-clock read, no global.
- **`double` everywhere in the public geometry (doc 04:108-112).** The anchor
  field, the rebase math, and the walk operate in `double`; float32 stays inside
  backends on guaranteed-small viewport-relative values, untouched here.
- **Composed transforms recomputed, never accumulated (doc 04:44-47,113-114).**
  The walk composes per-edge matrices on demand via `compose()`; rebasing
  rebuilds the camera from the composed matrices, it does not incrementally
  patch a running product across frames.
- **Degenerate composed transform culls, never NaNs (doc 04:115-117).** The walk
  reuses `inverse()`'s `nullopt` cull (`compositor.cpp:27-30`) verbatim; a
  singular authored matrix or scale-~0 subtree is culled, not propagated.
- **Byte-exact backward compatibility.** With `anchor` set to the root
  composition and no rebase, the generalized walk must reproduce
  `render_frame`'s current output **byte-for-byte** — the existing
  `tests/tile_planning_golden.t.cpp` and `render_frame` goldens must stay green
  unchanged. The generalization is additive, not a behavior change to the
  one-level case.
- **The `2^16` threshold is a named tunable, not designed behavior.** Doc
  04:63-66 writes "(say 2¹⁶)" — illustrative. Introduce it as a named constant
  (`k_reanchor_scale_threshold`, mirroring `k_tile_size`/`k_max_fallback_octaves`
  "a later task can tune"); tests pin the **invariants** (appearance preserved,
  conditioning bounded, sub-pixel culled), never the literal 65536.
- **Sub-pixel cutoff, no request emitted.** A subtree culled by the sub-pixel
  rule must not descend and must issue no `RenderRequest` — a behavioral claim
  proven by direct assertion on the walk's emitted request set (Acceptance).
- **Re-anchor is host-visible (doc 04:81-84).** The rebase function's return
  carries an explicit re-anchor event (old anchor → new anchor); the compositor
  produces it, `runtime.host_objects` surfaces it. This task does **not** build
  the host API — it produces the value the host layer consumes.

## Acceptance criteria

Tests are part of this task (doc 16, ≥90% diff coverage on changed lines).

- **Claims-register growth** — three new rows appended to
  `tests/claims/registry.tsv` under the `04-transforms-and-infinite-zoom` stem
  (descriptive slugs, no new doc anchor, matching the sibling convention), each
  with a live `// enforces:` test:
  - `04-transforms-and-infinite-zoom#rebase-preserves-composed-appearance` —
    re-anchoring across the threshold leaves the device-space image of a probe
    point unchanged to within one double rounding (a few ULPs), enforced by a
    numeric property test in `src/compositor/t/anchored_viewports.t.cpp`. This is
    doc 04:66's "exact to within one double rounding" — a **justified tolerance**
    (doc 16:47-53's exception, the guarantee is within-one-rounding, not
    byte-exact), documented as such.
  - `04-transforms-and-infinite-zoom#camera-well-conditioned-across-depth` —
    driving a synthetic chain nested N levels at ~1000×/level through repeated
    rebases keeps the composed anchor→device `max_scale()` inside the
    well-conditioned band (structure-bounded, never diverging with depth); the
    same chain rendered with a single non-rebased camera loses conditioning.
    Enforced by the deep-nesting property test the note calls for.
  - `04-transforms-and-infinite-zoom#subpixel-subtree-culled` — a subtree whose
    composed scale drops below the sub-pixel threshold is culled without
    descending and emits **zero** `RenderRequest`s, asserted directly on the
    walk's request set.
- **Byte-exact golden** — `tests/anchored_viewports_golden.t.cpp` (Catch2, links
  `arbc` + `CpuBackend`, added to `tests/CMakeLists.txt`): a one-level scene
  rendered through the generalized walk with `anchor == root` is byte-identical
  (`memcmp`) to `render_frame`'s output, pinning the backward-compatibility
  constraint. Enforces `16-sdlc-and-quality#byte-exact-goldens`. (Rebase
  continuity itself is *not* a byte-exact golden — doc 04:66 promises
  within-one-rounding, so it lands as the property test above.)
- **Degenerate/NaN cull** — reuses the existing `inverse()`-nullopt cull
  (`compositor.cpp:27-30`); a property-test case feeds a singular authored
  matrix and asserts the subtree culls with no NaN in the output rather than
  registering a redundant claim (the doc 04:115-117 behavior is already live).
- **Behavioral-counter posture (deferred surface).** This task proves its
  zero-request claim by direct assertion on the walk's emitted requests, not
  through a counter surface; exposing requests/cache-hits/composites counters is
  `compositor.counters` (`35-compositor.tji:52-56`, doc 16:54-62). Noted so the
  absence is a scoped decision, not a gap — same posture as
  `scale_ladder.md`/`tile_planning.md`.
- **No new WBS leaf.** Every deferral below lands on an **existing** task:
  persistent cross-frame anchor state + host-API surfacing of the re-anchor
  event → `runtime.host_objects` (`65-runtime.tji:25-29`, already scoped for it);
  async worker dispatch of walk-emitted misses → `compositor.pull_service`;
  re-anchor / clock advance as damage → `compositor.damage_planning`;
  multi-level descent into nested *compositions* as recursive composition lands →
  `compositor.operator_graph` (the rebase/walk functions are written
  depth-agnostic, so they need no change when nesting deepens); counter-surface
  exposure → `compositor.counters`. The closer registers **no** new task for this
  refinement.

## Decisions

- **1. Anchor is an `ObjectId` field on `Viewport`, not a new camera struct.**
  Doc 04:81-84 mandates the `(anchor node, matrix)` pair; the model already
  identifies every node by `ObjectId` (`ids.hpp:11`, `records.hpp:60-102`) and
  the compositor already anchors tile keys on `ObjectId` content. Adding
  `ObjectId anchor` beside the existing `Affine camera` in `Viewport`
  (`compositor.hpp:16-20`) is the minimal seam: the driver signatures take
  `const Viewport&` and flow it through unchanged, and `anchor == <root>`
  reproduces today's behavior with zero migration. *Rejected:* a separate
  `AnchoredCamera` type — it would duplicate `Viewport`'s width/height and force
  a signature churn through `render_frame`/`render_frame_interactive` for no
  representational gain. *Not a design-doc delta:* the pair is already normative
  in doc 04:81-84; this concretizes it in C++.

- **2. Rebase is a pure value-returning function; state lives in runtime.** Doc
  17:78-80 places frame loops, viewport objects, and inter-frame state in runtime
  (L5); the compositor (L4) is a pure per-frame library. So `rebase` takes the
  current `(anchor, camera)` + resolver and returns `{ Viewport rebased; bool
  reanchored; ObjectId from, to; }` (or equivalent) as a value — runtime holds
  the camera across frames and applies it, exactly as `refinement.hpp:59-62` has
  the caller own the queue and `render_frame_interactive` stamps the deadline as
  a value with no wall-clock read (doc 17:60). *Rejected:* stashing the anchor in
  a compositor-owned session object — it would pull L5 frame-loop policy down
  into L4 and break the "planning stays pure" invariant the whole stream is
  built on.

- **3. The `2^16` threshold is a named tunable; claims pin invariants, not the
  constant.** Doc 04:63-66's "(say 2¹⁶)" is explicitly illustrative, and
  `scale_ladder.hpp:82-83` already reserved the figure here "as a separate
  mechanism the ladder does not conflate." So it becomes
  `k_reanchor_scale_threshold` (a tunable constant in the sibling mold of
  `k_tile_size`/`k_max_fallback_octaves`), and the tests assert the *observable
  invariants* — appearance preserved to one rounding, conditioning bounded across
  depth, sub-pixel subtrees culled — never the literal 65536. *Rejected:* a claim
  asserting rebase fires exactly at scale 65536 — it would over-specify past what
  doc 04 promises and freeze a tunable into a contract.

- **4. Rebase continuity is a numeric property test (justified tolerance), the
  backward-compat case is the byte-exact golden.** Doc 04:66 states the guarantee
  as "exact to within one double rounding, invisible at sub-pixel level" — a
  double rounding can flip a boundary pixel, so a byte-exact golden *across* a
  rebase would be over-strong and flaky. The within-one-rounding guarantee is
  the justified tolerance doc 16:47-53 permits as the exception. The strong
  byte-exact golden instead pins the *no-rebase* equivalence (generalized walk
  with `anchor == root` == `render_frame`), which regression-guards the
  generalization without over-claiming. *Rejected:* byte-exact goldens on rebased
  frames — they contradict doc 04's own stated precision bound.

- **5. The walk is written depth-agnostic; multi-level nested-composition descent
  rides on recursive composition, not a new task.** The cull walk and the rebase
  anchor-selection descend whatever transform chain the resolver exposes; over
  today's single composition→layer nesting that is one level, and it composes
  cleanly with deeper structure as recursive composition
  (`compositor.operator_graph`, doc 05) lands, with no change to these functions.
  *Rejected:* deferring a dedicated "deep-nesting rebase" follow-up task — it
  would be redundant work against depth-agnostic functions and violate the
  no-audit/no-revisit rule; the composition point is a note, not a WBS leaf.

- **No design-doc delta.** Every rule here is settled doc text: the
  `(anchor, matrix)` representation and host-visible re-anchor event
  (04:81-84), the `2^16`/rebasing mechanism (04:62-69), the viewport-outward
  sub-pixel cull (04:70-75), the `double`-everywhere / recompute-never-accumulate
  / degenerate-culls conventions (04:108-117). This task *concretizes* those
  promises into C++ without altering designed behavior — no doc edit, no doc-00
  bullet. `runtime.host_objects` is already scoped (docs 01/04) to surface the
  pair and events this task produces.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-06.

- Extended `Viewport` with `ObjectId anchor` field in `src/compositor/arbc/compositor/compositor.hpp`; shared `render_layer` helper extracted to `compositor.cpp` (byte-neutral refactor, existing goldens still green).
- New module `src/compositor/arbc/compositor/anchored_viewports.hpp` + `src/compositor/anchored_viewports.cpp`: `k_reanchor_scale_threshold`/`k_subpixel_cull_extent`/`k_root_anchor` tunables, pure `rebase_need`, `reanchor_camera`, `rebase` → `RebaseResult`+`Reanchor`, depth-agnostic `cull_walk`, and `render_frame_anchored`.
- Unit/property tests in `src/compositor/t/anchored_viewports.t.cpp` enforcing claims `04-transforms-and-infinite-zoom#{rebase-preserves-composed-appearance, camera-well-conditioned-across-depth, subpixel-subtree-culled}` (within-one-rounding tolerance per Decision 4 / doc 16:47-53).
- Byte-exact golden in `tests/anchored_viewports_golden.t.cpp`: one-level scene via generalized walk with `anchor == root` is byte-identical to `render_frame` output, enforcing `16-sdlc-and-quality#byte-exact-goldens`.
- Three new rows added to `tests/claims/registry.tsv` under `04-transforms-and-infinite-zoom`.
- Build wiring in `src/compositor/CMakeLists.txt` and `tests/CMakeLists.txt`; no new CMake dependency (levelization unchanged, still `contract cache`).
- All deferred concerns (persistent cross-frame anchor, host-API re-anchor events, async worker dispatch) ride existing tasks: `runtime.host_objects`, `compositor.pull_service`, `compositor.damage_planning`, `compositor.operator_graph`, `compositor.counters`; no new WBS leaf.
