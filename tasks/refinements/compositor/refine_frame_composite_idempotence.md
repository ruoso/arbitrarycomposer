# compositor.refine_frame_composite_idempotence

Clear (and clip) the target region before re-compositing in a damage-gated
refine frame.

## TaskJuggler entry

[`tasks/35-compositor.tji:104-109`](../../35-compositor.tji)

```
  task refine_frame_composite_idempotence "Clear target region before re-compositing in a damage-gated refine frame" {
    effort 1d
    allocate team
    depends !refinement
    note "... Docs 02/04. Refinement: tasks/refinements/compositor/refine_frame_composite_idempotence.md"
  }
```

## Effort estimate

**1d as scheduled.** The scoping below expands it: a plain rect clear is
*not* sufficient (Decision 1 — tile overhang), so the fix carries two new
clip-scoped `Backend` operations across four backend implementations plus a
design-doc delta. **Realistic estimate: 2d**; the closer should bump the WBS
effort when this lands.

## Inherited dependencies

**Settled:**

- `compositor.refinement` (`tasks/refinements/compositor/refinement.md`,
  Done 2026-07-05) — the `RefinementQueue`, `PendingTile`,
  `poll_refinements` → `std::vector<Damage>`, and the rule that frame-to-
  frame state (the queue, the counters, **the target surface**) belongs to
  the runtime loop, not the planner. This is the task whose async-arrival →
  damage → follow-up-frame loop *is* the refine path being fixed.
- `compositor.damage_planning`
  (`tasks/refinements/compositor/damage_planning.md`, Done 2026-07-06) — the
  damage gate: `map_damage_to_device`, `invalidate_damage`, `DirtyRegion`,
  and the `const DirtyRegion* dirty = nullptr` seam on
  `render_frame_interactive`. Its implementation added the **null →
  whole-viewport clear guard** (`damage_planning.md:602`) that is the
  structural origin of this bug.
- `compositor.tile_planning`, `compositor.counters`,
  `compositor.expose_visible_plan`, `compositor.operator_graph` — the tiled
  planner, the behavioral counters, the plan sink, and the identity
  short-circuit whose delivery branch also composites onto the target.
- `runtime.worker_dispatch_leaf_only`
  (`tasks/refinements/runtime/worker_dispatch_leaf_only.md`, Done
  2026-07-12) — made `worker_count > 0` correct and TSan-clean, which is what
  made this latent bug reachable.

**Pending (downstream, not blocking):**

- `runtime.interactive_worker_count_default` — **blocked on this task**
  (re-deferred in `ffbece7`; `tasks/65-runtime.tji:156` lists this task as an
  explicit `depends`). Its implementer found the bug and stashed the work.
- `compositor.in_flight_tile_dedup` (`tasks/35-compositor.tji:110-115`) —
  already in the WBS, `depends !refine_frame_composite_idempotence`.

## What this task is

`render_frame_interactive` clears the target surface **only** when it is
handed a null `DirtyRegion` (`src/compositor/tile_planning.cpp:238-244`).
Every damage-gated frame — which after the first frame is *every* frame
(`src/runtime/interactive.cpp:310`) — re-composites the damaged tiles
source-over onto the caller-persisted target, on top of the pixels the
previous frame already painted there. Source-over is not idempotent for
anything but fully opaque content, so a translucent layer's contribution
lands twice.

This task makes a damage-gated frame repaint a well-defined **device repaint
region**: clear it, then re-composite every layer that intersects it, with
every composite onto the target **clipped to that region** so a tile that
straddles the edge cannot spill onto un-cleared pixels. The invariant it buys
is the one the task is named for: a gated frame's repaint region is
byte-identical to what a single full pass would have put there, the rest of
the target is untouched, and compositing the same frame twice is a no-op.

## Why it needs to be done

The bug is silent and it converges on wrong pixels. From
`tasks/refinements/runtime/interactive_worker_count_default.md:49-66`:

> 2/60 runs with `worker_count=1` and CPU oversubscription on a nested
> semi-transparent scene produce `composites=6` where the single-pass oracle
> does `composites=3`. The loop quiesces cleanly (`schedule_follow_up=false`,
> pending empty, zero degraded composites, nothing cancelled) — it converges
> on wrong pixels silently.

At `worker_count == 0` (the shipped default) every miss settles inline inside
the frame, so the refine path essentially never fires and the bug stays
latent. `runtime.interactive_worker_count_default` exists to ship a non-zero
default — which turns refine frames into the *normal* interactive path and
this bug into routine behavior. That task cannot proceed until this one
lands; it is the sole reason it was re-deferred.

The existing gated-path golden does not catch it, deliberately:
`tests/damage_planning_golden.t.cpp:35-38` explains that its scene is built
from opaque solids with a full-viewport opaque background precisely so that
"the un-cleared gated composite reproduce[s] a cleared full re-render
byte-for-byte (opaque source-over is a replace)". The masking assumption is
written down in the test; this task removes the need for it.

## Inputs / context

**The defect, in code:**

- `src/compositor/tile_planning.cpp:238-244` — the entire clear policy:
  `if (dirty == nullptr) { backend.clear(target, 0,0,0,0); }`. All-or-nothing
  on the whole target; there is no rect-scoped clear anywhere.
- `src/compositor/tile_planning.cpp:317-330` — the damage gate: each device
  dirty rect is clipped to the viewport, mapped through the layer's inverse
  into local space, `rect_union`'d (i.e. **bounding-boxed**) into
  `dirty_local`, and `region = region.intersect(dirty_local)`.
- `src/compositor/tile_planning.cpp:522-565` — the composite switch
  (`Fresh` / `Stale` / `Coarser` / `Placeholder`), each `backend.composite`
  landing source-over on `target`. Its `Placeholder` comment ("the target was
  cleared to transparent, so 'nothing yet' is a no-op paint") is **false on
  the gated path** — nothing was cleared.
- `src/compositor/tile_planning.cpp:500-505` — the operator identity-delivery
  branch, a fourth composite onto `target`.
- `src/compositor/tile_planning.cpp:63-101` — `composite_coarser`: composites
  the coarse tile into a pool temp (`:89`) and the temp onto `target` (`:95`).
  Only the second lands on the frame target.
- `src/compositor/tile_planning.cpp:130-131`, `tile_planning.hpp:89-98` —
  `tile_local_rect(rung, coord)` is the **whole tile cell**, never clipped to
  the plan region. This is why the clear alone is not enough (Decision 1).
- `src/runtime/interactive.cpp:225-229` — the still-frame early-out (empty
  dirty + empty pending ⇒ zero work, target untouched). Must survive.
- `src/runtime/interactive.cpp:306-318` — `dirty_ptr = first_frame ? nullptr
  : &dirty`: only the very first frame is un-gated.
- `src/compositor/damage_planning.cpp:82-99` — `invalidate_damage` drops
  damaged tiles "across all rungs/revisions/achieved-times" (doc 02:94-95),
  so a content-damaged tile has **no** stale or coarser fallback left; it
  plans as a miss and, if the render does not settle in budget, displays the
  transparent placeholder (Decision 3).

**The backend seam:**

- `src/surface/arbc/surface/backend.hpp:35` — `clear(Surface&, r,g,b,a)`:
  whole surface, no rect.
- `src/surface/arbc/surface/backend.hpp:37-40` — `composite(dst, src,
  src_to_dst, opacity)`: source-over of the **whole** `src` surface under the
  affine; no clip, no replace mode.
- `src/backend_cpu/cpu_backend.cpp:91-98` — `CpuBackend::clear` fills the
  entire surface via `visit_surface` + `fill_kernel`.
- Implementations to update: `src/backend_cpu/arbc/backend_cpu/cpu_backend.hpp`
  and the three test doubles under
  `src/surface/arbc/surface/testing/` (`stub_backend.hpp`,
  `counting_backend.hpp`, `forwarding_backend.hpp`).

**Design docs (normative):**

- **doc 02 § "The frame, interactively"** (`docs/design/02-architecture.md`,
  steps 1–6 and the new clauses below) — step 5 "Draw tiles bottom-to-top
  onto the target surface", step 6 "Async results that arrive later produce
  damage for their region, scheduling a follow-up frame". The composition of
  those two sentences is exactly the promise this bug violates, and doc 02
  did **not** say who clears the target or that a refined frame equals a full
  pass. **This task lands the delta** (see Decisions → *Design-doc delta*).
- **doc 02:94-95** — "Damage invalidates by `(content id, region)` across all
  rungs" — the sentence `invalidate_damage` implements, and the reason a
  damaged tile falls through to the transparent placeholder.
- **doc 04 § "Scale ladders and tile geometry"**
  (`docs/design/04-transforms-and-infinite-zoom.md:103-106`) — "tiles are
  axis-aligned in *local* space, and the composite pass applies the full
  affine". A tile's device footprint is therefore a rotated quad, which rules
  out expressing the clip as a source-side sub-rect (Decision 1, alternative
  (c)).
- **doc 09 § "Backend contract"** (`docs/design/09-surfaces-and-backends.md`)
  — "all composite operations route through the backend (the core never loops
  over pixels itself)", and per-frame work "expressible as a command list".
  **This task lands the clip-scoped-operations delta here.**
- **doc 16 §§ "Golden rendering tests", "Behavioral-counter tests"**
  (`docs/design/16-sdlc-and-quality.md:48-62`) and the claims register
  (`tests/claims/registry.tsv`, `scripts/check_claims.py`).

**Predecessor decisions this must honor:**

- The target surface is **caller-persisted and not owned by the compositor**
  (`tile_planning.md:167-168`, `damage_planning.md:204`).
- Every new capability on `render_frame_interactive` is a trailing optional
  parameter whose **null path is byte-identical** — no golden re-baselines.
- Behavioral counters, never wall clock (`counters.md`).
- Goldens are byte-exact; tolerances are the justified exception.

## Constraints / requirements

1. **The un-gated path is byte-identical.** `dirty == nullptr` (the first
   frame, and the offline `render_frame` in `compositor.cpp`, which this task
   does not touch) still clears the whole target and composites unclipped.
   Every existing golden — `tests/tile_planning_golden.t.cpp:65,96`,
   `tests/refinement_golden.t.cpp:100`,
   `tests/damage_planning_golden.t.cpp:72,124`,
   `tests/interactive_operator_identity_golden.t.cpp`,
   `tests/pull_multitile_golden.t.cpp`,
   `tests/anchored_viewports_golden.t.cpp` — must pass **unchanged**. A
   golden that needs a new baseline is a bug in this task, not a new
   baseline.
2. **Zero-work stays zero-work.** A non-null but empty `DirtyRegion` clears
   nothing, plans nothing, composites nothing, and leaves the target
   byte-identical (`tests/damage_planning_golden.t.cpp:124`;
   `damage_planning.md:204-206`). The clear is gated on a *non-empty* repaint
   region, not merely on `dirty != nullptr`.
3. **Pixel-neutral at the shipped configuration.** With `worker_count == 0`
   every leaf miss settles inline, so a gated frame's tiles are all `Fresh`
   and the cleared-then-repainted region reproduces the same pixels the
   un-cleared composite produced for the opaque scenes shipped today. The
   observable change is confined to translucent content and to
   `worker_count > 0`.
4. **No stateful clip on the `Backend`.** The backend is shared across the
   render worker pool (doc 02 § Threading model,
   `runtime.worker_dispatch_leaf_only`): a mutable scissor/`push_clip` state
   on the backend object is a data race, not an API convenience. The clip is
   a parameter.
5. **Levelization (doc 17).** No new component edges: `compositor` already
   depends on `surface` (it calls `Backend` on every path). The new
   operations live on the existing `Backend` seam.
6. **The counter contract is unchanged.** One `note_composite()` per
   composite call onto the target, in the same places
   (`tile_planning.cpp:92,98,504,528,540,548`). The clip does not add or
   remove composite calls.

## Acceptance criteria

**Goldens (byte-exact — doc 16:48-53).** New file
`tests/refine_idempotence_golden.t.cpp`, built on a **translucent** scene
(the inverse of `damage_planning_golden.t.cpp`'s deliberately opaque one):
a semi-transparent layer over a background, nested one level, so that a
double source-over is observable.

- *"a gated frame composited twice is byte-identical to composited once"* —
  the deterministic core regression test, and the one the task is named for.
  Warm the cache, run one gated `render_frame_interactive` onto `target`,
  snapshot; run the **same** gated frame again onto the same `target`;
  `byte_identical`. Fails on `main` (the second pass doubles the translucent
  contribution), passes after. No worker pool, no race, no flake.
  → **claim `02-architecture#gated-frame-repaint-is-idempotent`**
- *"a refined interactive sequence quiesces byte-identical to a single full
  pass"* — drive `InteractiveRenderer::render_frame` to quiescence
  (`schedule_follow_up == false`, empty pending) at `worker_count` 0 and 2,
  and compare the target to a single null-dirty pass over the same scene from
  the same warm cache. This is the oracle the stashed
  `interactive_worker_count_default` work found the bug with.
  → **claim `02-architecture#gated-frame-equals-single-pass`**
- *"a gated frame touches no pixel outside its repaint region"* — damage a
  sub-rect; assert every pixel outside the repaint region is byte-identical
  to the previous frame's target (this is what forbids "just repaint
  everything" as the fix).
  → **claim `02-architecture#gated-frame-touches-only-its-repaint-region`**

**Backend unit tests** (`src/backend_cpu/t/`): `clear_rect` and
`composite_clipped` write no pixel outside the clip; the clip is intersected
with the destination bounds (a clip extending past the edge is legal); an
empty clip is a no-op; a clip covering the whole destination is byte-identical
to the unclipped `clear` / `composite`.
→ **claim `09-surfaces-and-backends#clip-scoped-ops-honor-the-clip`**

**Test doubles.** `stub_backend.hpp`, `counting_backend.hpp` and
`forwarding_backend.hpp` implement the two new operations; the forwarding
double's existing claim
`09-surfaces-and-backends#forwarding-double-delegates-every-op`
(`tests/claims/registry.tsv:51`) means its test must cover them — "an
operation the double does not explicitly override is forwarded" is now a
claim about six operations, not four.

**Behavioral counters (doc 16:54-62), never wall clock.** A second identical
gated frame over a warm cache issues **zero renders** (`requests_issued`
delta 0) and re-composites the same tile count (`composites` delta equal to
the first frame's) — the fix must not turn idempotence into "skip the work",
and must not double it either.

**Concurrency (the original repro).** `tests/refine_idempotence_stress.t.cpp`:
60 iterations × `worker_count ∈ {1, 2, 4}` driving the interactive loop to
quiescence on the translucent nested scene, each asserting byte-exactness
against the single-pass oracle. Registered in the nightly TSan sweep lane
that `runtime.worker_dispatch_leaf_only` added, so the arrival/refine path is
TSan-covered under a real pool.

**Coverage.** ≥90% diff coverage on changed lines (CI gate). The clip kernels
and the empty/degenerate clip branches are part of the task, not a follow-up.

**Claims register.** Four rows added to `tests/claims/registry.tsv`, each with
an `// enforces: <claim-id>` tag in the test body; `scripts/check_claims.py`
green in the `lint` job.

**Deferred to `compositor.disjoint_dirty_repaint`** (closer registers in the
WBS; same milestone as `compositor.in_flight_tile_dedup`, i.e. the M9 list at
`tasks/99-milestones.tji:72`): the repaint region is the **bounding box** of
the frame's device dirty rects (Decision 2), so two small damages at opposite
corners of the viewport repaint everything between them. Normalize the
`DirtyRegion` into a disjoint rect set and clear/clip/repaint per rect
instead. Effort 2d, `depends !refine_frame_composite_idempotence`. Waste, not
incorrectness — the bbox repaint is byte-exact, just wider than it needs to
be, and it re-composites from cache (no extra renders).

## Decisions

### 1. The fix is a *clip-scoped* clear + composite, not a bare rect clear

The WBS note prescribes "clear the target region before re-compositing". That
is necessary but **not sufficient**, and the reason is worth stating because
it is the whole shape of the task:

`tile_local_rect` (`tile_planning.cpp:130-131`) is the **whole tile cell** —
a tile is never clipped to the plan region, because a partially-filled tile
would poison the cache for every other frame that reads it. So a tile that
straddles the edge of the dirty region has a device footprint that extends
*beyond* it. Clear the dirty rect, composite the whole tile, and the overhang
lands source-over on pixels that were **not** cleared: the same double
contribution, narrowed to a tile-boundary fringe. Byte-exactness against a
single full pass is the acceptance bar, so the fringe is not acceptable.

Making the painted set equal the cleared set requires clipping the composite.
`Backend` therefore gains two operations:

```cpp
virtual void clear_rect(Surface& dst, const Rect& device_rect,
                        float r, float g, float b, float a) = 0;
virtual void composite_clipped(Surface& dst, const Surface& src,
                               const Affine& src_to_dst, double opacity,
                               const Rect& device_clip) = 0;
```

Distinct names rather than an extra parameter on `clear` / `composite`: an
overloaded virtual is hidden by any override in a derived backend unless every
one of them writes `using Backend::clear;`, and a default argument on a
virtual binds statically. Existing call sites — the kinds, `pull_service`'s
`deliver_tile`, the offline `render_frame` — keep compiling untouched, and
the CPU backend defines the unclipped forms as the whole-surface-clip case of
the clipped kernels, so it carries one kernel per operation, not two. The
clipped composite has exactly one call-site family (the four target composites
in `tile_planning.cpp`); that is a narrow seam by design.

**Alternatives rejected:**

- *(a) Clear the dirty rects, composite unclipped.* The tile-overhang fringe
  above. Fixes the reported repro's gross artifact while leaving a smaller
  one; fails the byte-exact oracle.
- *(b) Expand the repaint region to the tile-closure of the damage* (union in
  the planned tiles' device footprints, re-plan against the expanded region,
  iterate to a fixed point). Backend-API-free and byte-exact at the fixed
  point, but the iteration is only bounded by the viewport: each round unions
  in bounding boxes of rotated tile quads at each layer's own rung, so in
  practice it grows toward a full-viewport repaint — and it replaces a
  one-line clear with a fixed-point loop, the least testable structure of the
  three.
- *(c) Composite the region into a pool scratch surface and copy it back.*
  The copy-back is a *replace-in-rect*, which the backend does not have
  either (`convert` replaces, but only whole-surface, same-geometry —
  `backend.hpp:70`), so it is the same class of API addition, plus a
  viewport-sized surface and a viewport-sized clear on every refine frame.
  Nothing is bought.
- *(d) A stateful scissor* (`push_clip`/`pop_clip` on the backend). The
  backend is shared across the worker pool; mutable clip state on it is a
  data race (Constraint 4).
- *(e) Shrink the source: composite a sub-rect of the tile surface.* Doc
  04:103-106 — tiles are axis-aligned in *local* space and the composite pass
  applies the full affine, so a device-space clip is a rotated quad in tile
  space. An axis-aligned source sub-rect cannot express it.

### 2. One repaint rect: three uses, one value

The gated frame computes a single device **repaint rect** — the bounding box
of `DirtyRegion::device_rects`, rounded **out** to integer device pixels and
intersected with the viewport — and uses that one value three times: as the
region mapped back through each layer's inverse to gate its plan (replacing
the `rect_union` loop at `tile_planning.cpp:317-327`), as the `clear_rect`
argument, and as the `device_clip` on every composite onto the target. A
helper `Rect repaint_region(const DirtyRegion&, const Viewport&)` lands next
to `DirtyRegion` in `damage_planning.hpp`.

Deriving all three from one value is what makes the proof trivial: the
planned set, the cleared set and the painted set are the same set, so within
the region every layer that covers a pixel repaints it exactly once onto
transparent — a single full pass, restricted to the region — and outside it
nothing is written.

*Why the bounding box and not the individual rects:* the per-layer gate
**already** bounding-boxes (`rect_union` of the mapped dirty rects,
`tile_planning.cpp:324`), so the planner is bbox-granular today; and
`map_damage_to_device` (`damage_planning.cpp:21-56`) emits one rect per
(damage, layer) pair, which may **overlap** — clipping each tile once per
dirty rect would composite it twice in the overlap, re-introducing the very
bug being fixed. Making the rects disjoint first is real geometry (and it
changes the per-tile composite count, hence the counters). The bbox is one
rect, one composite per tile, and unambiguously correct. `compositor.
disjoint_dirty_repaint` (registered above) is where the precision goes.

*Why round out:* the dirty rects are `map_rect` outputs — arbitrary doubles.
Rounding in would leave a sub-pixel fringe of the true damage unpainted (a
stale seam); rounding out is conservative in the safe direction, and because
the *same* rounded rect gates the plan, the extra pixels are covered by every
layer that covers them.

### 3. A placeholder in the cleared region paints transparent, not the previous frame's pixels

Today, a damaged tile that misses and does not settle in budget composites
*nothing* (`tile_planning.cpp:555-564`), so the previous frame's pixels show
through — which looks like a stale-frame fallback but is really the absence of
a clear. After this task the region is cleared, so that tile shows the
transparent placeholder: **a hole, for one frame, until the arrival refines
it**.

That is doc 02 step 4's stated degradation ("stale-revision tiles,
coarser-scale tiles rescaled, or checkerboard/transparent, in that preference
order") and it is what a full pass would have shown. It is not reachable at
the shipped `worker_count == 0`, where every miss settles inline. Note that
the stale/coarser rungs of that ladder are *not* available for a
content-damaged tile: `invalidate_damage` drops it across all rungs and
revisions (`damage_planning.cpp:82-99`, doc 02:94-95), by design — the stale
fallback serves a *revision bump*, not a *damaged region*.

The alternative — suppress the repaint of any sub-region whose tiles are all
placeholders, so the old pixels survive — is sound but needs a
plan-all-then-composite restructure of `render_frame_interactive` and trades
progressive refinement for all-or-nothing regions. Whether the interactive
path should prefer a transparent hole or a stale frame while a tile is in
flight is a product judgment on top of a design doc that already answers it;
it is **not** re-litigated here, and it is raised for the parking lot rather
than encoded as a task.

### 4. Design-doc delta (rides in the closer's commit, doc 16's same-commit rule)

Doc 02 never said who clears the target, that the target persists across
frames, or that a refined frame equals a full pass — the promise the bug
violates was *unwritten*, and the house rule
(`interactive_pull_wiring` Decision 5) is: do not mint a claim id for a
sentence no design doc contains. So the doc says it first:

- **`docs/design/02-architecture.md`** § "The frame, interactively" — two new
  clauses after the six steps: the target surface is the caller's and
  persists across frames; a damage-gated frame repaints exactly one device
  region, clearing it first and clipping every composite to it, with the
  *clear first* and *clip to the region* rationale spelled out, and the
  resulting invariant (gated repaint ≡ single full pass; compositing the same
  frame twice is a no-op).
- **`docs/design/09-surfaces-and-backends.md`** § "Backend contract" — the
  clip-scoped operations: device-space, half-open, intersected with the
  destination's bounds; empty clip is a no-op; a whole-destination clip is
  byte-identical to the unclipped operation (which is how the unclipped forms
  are defined, so one kernel per operation). Framed as a scissor rect — the
  shape a GPU command list already has.
- **`docs/design/00-overview.md`** § "Guiding decisions" — a decision-record
  bullet ("A damage-gated frame clears and clips its repaint region"). It is
  project-shaping: it puts two operations on the backend contract that every
  backend, CPU and GPU, must implement.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-12.

- Added `clear_rect` and `composite_clipped` to `Backend` seam (`src/surface/arbc/surface/backend.hpp`) so a damage-gated frame can clear exactly its repaint region and clip every composite to it.
- `StubBackend` defaults delegate clip-scoped ops to unclipped virtuals (whole-destination clip ≡ unclipped op), so pixel-modelling doubles (`MarkBackend` et al.) inherit correct behavior without patching eight files (`src/surface/arbc/surface/testing/{stub,forwarding,counting}_backend.hpp`).
- `CpuBackend` implements both ops with clipped kernels; unclipped forms are the whole-surface-clip case (`src/backend_cpu/{cpu_backend.cpp,kernels.hpp,arbc/backend_cpu/cpu_backend.hpp}`).
- `tile_planning.cpp` computes `repaint_region` (bounding-box of dirty device rects, rounded out, intersected with viewport), calls `clear_rect` on it, and clips every composite-onto-target with `composite_clipped`; `damage_planning.hpp/.cpp` expose the helper (`src/compositor/{tile_planning.cpp,damage_planning.cpp,arbc/compositor/{tile_planning,damage_planning}.hpp}`).
- Golden test `tests/refine_idempotence_golden.t.cpp`: gated-frame-twice-is-idempotent; quiesces-equals-single-pass at workers 0/2; touches-only-its-repaint-region — claims `02-architecture#gated-frame-{repaint-is-idempotent,equals-single-pass,touches-only-its-repaint-region}`.
- Stress test `tests/refine_idempotence_stress.t.cpp`: 60 iters × `{1,2,4}` workers, deterministic gated late-arrival, `[.nightly]` seeded sweep registered in TSan-full lane (`tests/CMakeLists.txt`, `.github/workflows/nightly.yml`).
- Backend unit tests cover `clear_rect`/`composite_clipped` clip-honor, bounds-intersect, empty-clip no-op, whole-clip ≡ unclipped, round-out (`src/backend_cpu/t/cpu_backend.t.cpp`); forwarding-double claim extended from 6 to 8 ops (`src/surface/t/backend_doubles.t.cpp`); claim `09-surfaces-and-backends#clip-scoped-ops-honor-the-clip` added.
- Design-doc delta landed in same commit: `docs/design/00-overview.md`, `docs/design/02-architecture.md`, `docs/design/09-surfaces-and-backends.md`.
- `compositor.disjoint_dirty_repaint` registered in the WBS (effort 2d, depends `!refine_frame_composite_idempotence`, wired to M9) for the bbox→disjoint-rect precision follow-up.
