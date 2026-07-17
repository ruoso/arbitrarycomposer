# compositor.bounded_content_tile_clip — Clip sub-tile bounded content to its declared extent

The interactive tile path clips content bounds at *planning* only (tile-level
cull). A content whose declared extent is smaller than — or not aligned to —
its tile cell renders into the whole tile buffer and composites the whole tile,
so the overhang bleeds past `bounds()`. This task adds a composite-time clip to
the content's device-space bounds so sub-tile bounded content no longer bleeds.

## TaskJuggler entry

`tasks/35-compositor.tji:139-144` — `task bounded_content_tile_clip`,
`effort 2d`, `allocate team`, `depends !tile_planning`. The `note` names docs
02/04 and cites source-of-debt `tasks/refinements/packaging/examples.md`.

## Effort estimate

**2d as scheduled.** No new capability is built: the clip rides the existing
`Backend::composite_clipped` seam that `refine_frame_composite_idempotence`
landed, so this is one localized change to `composite_onto_target`
(`src/compositor/tile_planning.cpp:94-107`), a device-bounds value threaded to
its five call sites plus `composite_coarser`, one byte-exact golden, one
behavioral-counter case, and a doc-02 clause. Its shape mirrors
`compositor.disjoint_dirty_repaint` (also 2d, also a new consumer of the
already-shipped clip primitive, "No new backend virtuals are needed").

## Inherited dependencies

**Settled (formal `depends`):**

- `compositor.tile_planning` (`35-compositor.tji:139` → `!tile_planning`).
  Established the interactive tiled path this task extends: whole-cell tiles
  (`k_tile_size = 256`), the `plan_layer` pure planner, `tiles_covering`, and
  the `render_frame_interactive` driver. Decision: **tiles are whole cache
  cells** — a partially-filled tile poisons the cache, so the tile buffer must
  stay whole-cell and any narrowing is a *composite-time* operation, never a
  render-time one. This task honors that: it clips at composite, not render.

**Settled (informal — seams this builds on, already shipped):**

- `compositor.refine_frame_composite_idempotence` — introduced
  `Backend::composite_clipped(dst, src, src_to_dst, opacity, device_clip)`
  (`src/surface/arbc/surface/backend.hpp:66`) and its Decision 1(e): a
  device-space *clip rect* is the right parameter (not a source sub-rect),
  because tiles are axis-aligned in *local* space while the device footprint is
  the full affine's image (doc 04:102-106). Same reasoning applies here.
- `compositor.disjoint_dirty_repaint` — restructured the composite chokepoint
  into `composite_onto_target(..., std::span<const Rect* const> clips, ...)`
  (`tile_planning.cpp:94-107`), the parallel `tile_clips` per-tile clip lists
  (`tile_planning.cpp:512-554`), and the integer-aligned/disjoint repaint-rect
  contract this clip composes with.

**Pending (downstream, not blocking):** none. No task `depends` on this one;
it is a leaf correctness fix surfaced during `packaging.examples`.

## What this task is

After a tile is rendered, composite only the intersection of the tile's device
footprint with the content's device-space bounding rect, so a `SolidContent`
(or any content declaring finite `bounds()`) whose extent is smaller than its
tile cell does not paint the tile's overhang beyond that extent. The clip is
computed once per layer from `composed.map_rect(*content->bounds())`, rounded
out to whole device pixels, and applied at the single composite chokepoint
`composite_onto_target`, composing with the existing repaint-region clip.

**Not this task:**

- Making `SolidContent` self-clip. A solid is `infinite` extent (doc 01:68-77)
  and deliberately trusts the pipeline to request only in-bounds regions
  (`examples/host-interactive/main.cpp:76-80`); enforcement belongs in the
  compositor, which is the layer that already does it offline. See Decision 2.
- Exact clipping of a *rotated/sheared* extent to its device quad. This task
  clips to the conservative device AABB (round-out); a pixel-exact polygon
  clip would need a new backend primitive and is out of scope. See Decision 3.
- The camera-change and placement-damage repaint gaps surfaced alongside this
  one → `runtime.camera_change_damage`, `runtime.placement_damage_maps_to_device`
  (already registered, shipped).

## Why it needs to be done

The offline path (`render_layer`, `src/compositor/compositor.cpp:28-34`) sizes
its temp surface to `region = inv->map_rect(device_rect).intersect(*bounds)` and
composites only that region, so bounded content never spills there. The
interactive tiled path intersects bounds at tile *selection* only
(`tile_planning.cpp:442-448`) and then renders and composites whole 256px
cells, clipped only to repaint rects (`composite_onto_target`,
`tile_planning.cpp:94-107`). The two paths therefore disagree on a bounded
content that is smaller than / not aligned to a tile: offline clips it, tiled
bleeds it.

The gap is latent because every existing interactive test uses
`bounds == viewport`, and the host-interactive example dodges it with a
tile-aligned `Rect{0,0,256,256}` panel and a comment naming the defect
verbatim: *"the tile path plans (but does not clip) whole tiles; tile-aligned
bounds keep the CI artifact hand-computable"* (`main.cpp:76-80`,
`101-102`). Source-of-debt: `tasks/refinements/packaging/examples.md` (Status,
registered this leaf). Closing it lets interactive scenes use arbitrary bounds
and makes the tiled path's pixels match the offline path's for bounded content.

## Inputs / context

**Design docs (normative):**

- `doc 02:76-78` — step 5 **Composite**: the pass this clip attaches to. (This
  task lands a delta here; see Decision 4.)
- `doc 02:126-131` — *Clip to the region*: the existing composite clip (to the
  repaint region) the content-bounds clip composes with; both funnel through
  the doc 09 backend clip primitive.
- `doc 02:57-61` — step 3 **Plan requests**: tiles are whole local-space-aligned
  cache cells, the granularity this clip sub-divides below.
- `doc 04:102-106` — tiles axis-aligned in *local* space, full affine applied at
  composite; so the device-space bound is the AABB of the transformed extent,
  computed through `composed`, and the clip is applied in the composite pass.
- `doc 01:50-54, 68-77` — `bounds()` is the local-space region where content may
  produce non-transparent pixels; Solid/gradient/procedural are `infinite`,
  Raster is its image rect. The `nullopt` case is unclipped.
- `doc 03:74` — `virtual std::optional<Rect> bounds() const = 0; // nullopt =
  unbounded`: the declared-extent API this task reads.

**The defect, in code:**

- `src/compositor/tile_planning.cpp:442-448` — bounds intersected into `region`
  at cull/plan time only (`region = region.intersect(*bounds)`).
- `src/compositor/tile_planning.cpp:94-107` — `composite_onto_target`: the single
  chokepoint. Iterates `clips`, calling `backend.composite_clipped(...)` for a
  non-null entry or `backend.composite(...)` for the un-gated null entry. **No
  entry carries the content's bounds**, so the whole tile is painted.
- `src/compositor/tile_planning.cpp:512-554` — `tile_clips` build: one `{nullptr}`
  per tile on the un-gated path (`:519-521`), one `{&rect}` per repaint rect on
  the gated path. The null entry (`:521`) is exactly the path that spills.
- `src/compositor/tile_planning.cpp:117-153` — `composite_coarser` routes its
  final paint through `composite_onto_target` (`:150`), so it inherits the clip.

**The seam (production change lands here):**

- Compute the device bound once per layer where local `bounds` is already
  fetched (`tile_planning.cpp:443`): `std::optional<Rect> device_bounds =
  bounds ? round_out(composed.map_rect(*bounds)) : nullopt`. Round out with the
  `{floor(x0), floor(y0), ceil(x1), ceil(y1)}` convention `damage_planning.cpp`
  already uses (`src/compositor/damage_planning.cpp:39-40, 192`).
- Thread `const Rect* device_bounds` into `composite_onto_target` (and
  `composite_coarser`), and inside it intersect each clip: a non-null clip
  becomes `clip->intersect(*device_bounds)` (`Rect::intersect`,
  `src/base/arbc/base/geometry.hpp:36`); the null entry, when bounds exist,
  becomes `composite_clipped(..., *device_bounds)` instead of the unclipped
  `composite`. `nullopt` bounds → unchanged behavior. Counter bumps stay one
  per clip entry (`tile_planning.cpp:103-104`) — counter-neutral.

**The backend seam (reused, no new virtual):**

- `Backend::composite_clipped` — `src/surface/arbc/surface/backend.hpp:66`, CPU
  impl in `src/backend_cpu/cpu_backend.cpp`. Its contract (claim
  `09-surfaces-and-backends#clip-scoped-ops-honor-the-clip`,
  `tests/claims/registry.tsv:54`): writes no pixel outside the device clip; the
  clip is intersected with destination bounds; **a clip covering the whole
  destination is byte-identical to the unclipped composite** — which is why
  `bounds == viewport` cases stay byte-for-byte unchanged.

**Test / registry conventions:**

- Test doubles: `CountingBackend` (`composite_clipped_calls`,
  `src/surface/arbc/surface/testing/counting_backend.hpp:61-93`) and
  `ForwardingBackend` — record/forward `composite_clipped` for unit assertions.
- Tile goldens: `tests/tile_planning_golden.t.cpp` (the "tiled == whole"
  byte-exact oracle), `tests/refine_idempotence_golden.t.cpp`. Naming
  `tests/<name>_golden.t.cpp`, links `arbc` + `CpuBackend`.
- Counters: `CompositorCounters` (`src/compositor/arbc/compositor/counters.hpp`)
  — `composites()`, `requests_issued()`; unit tests in
  `src/compositor/t/counters.t.cpp`, `src/compositor/t/tile_planning.t.cpp`.
- Claims: `tests/claims/registry.tsv` (TAB-separated `<claim-id>\t<desc>`,
  `<doc-stem>#<slug>`), `enforces: <claim-id>` test comment,
  `scripts/check_claims.py` enforces register↔test bidirectionally.

## Constraints / requirements

1. **The clip is composite-time, never render-time.** The tile buffer stays a
   whole cache cell (tile_planning's decision); narrowing the render would
   poison the cache. Intersect only at composite.
2. **The device bound is rounded *out* to whole device pixels.** It must never
   remove a pixel the content legitimately covers (a coverage gap is worse than
   a bounded bleed). Round-out gives an exact clip for axis-aligned placements
   and a conservative AABB for rotated/sheared ones. Reuse the
   `floor/floor/ceil/ceil` convention from `damage_planning.cpp:39-40`.
3. **Counter-neutral.** `composites()` bumps once per clip entry exactly as
   today; a bounded tile that overlaps its repaint rect (it was only planned
   because `region ∩ bounds` was non-empty) yields a non-empty intersection, so
   no composite is added or dropped. `requests_issued` is untouched — planning
   is unchanged.
4. **`nullopt` bounds → byte-identical.** Unbounded content stays on the plain
   unclipped `composite`; the un-gated path for unbounded content does not
   change. `bounds == viewport` stays byte-identical via the whole-destination
   clip identity (claim `09…#clip-scoped-ops-honor-the-clip`).
5. **Compose with the repaint-region clip.** On the gated path the effective
   clip is `repaint_rect ∩ device_bounds`; the disjointness/idempotence
   invariants of `disjoint_dirty_repaint` (each pixel painted once) are
   preserved because intersecting a disjoint set with one rect stays disjoint.
6. **Levelization (doc 17): no new edges.** `arbc::compositor` (L4) reaches
   `Rect::intersect`/`Affine::map_rect` (L0 `base`) and the `Backend` interface
   (L2 `surface`, transitively via `contract`) that it already depends on. No
   `compositor → backend-cpu` edge; the CI dependency check must stay green.

## Acceptance criteria

- **Golden — `tests/bounded_content_tile_clip_golden.t.cpp` (new,
  cross-component, links `arbc` + `CpuBackend`). Byte-exact (doc 16:48-53):**
  - *"a sub-tile bounded solid does not paint past its declared extent"* — a
    `SolidContent` with `bounds` occupying a sub-region of one tile cell (e.g.
    `Rect{0,0,64,64}` on a 256px cell), composited over an opaque backdrop; the
    overhang pixels (extent edge → tile edge) equal the backdrop, byte-for-byte.
    → **claim `02-architecture#tile-composite-clipped-to-content-bounds`.**
  - *"a bounded solid straddling a tile boundary is clipped in both cells"* —
    bounds crossing a cell edge so two tiles are planned; both cells clip.
    → same claim.
  - *"gated repaint of sub-tile bounds does not bleed"* — the same scene under a
    partial `DirtyRegion`; the content-bounds clip composes with the repaint
    clip and the overhang stays backdrop. → same claim.
  - *"content whose bounds cover the tile composites byte-identically"* — a
    bounds-covers-viewport case producing the pre-task bytes; **re-asserts**
    (does not re-register) the tiled==whole property and
    `09…#clip-scoped-ops-honor-the-clip`.
- **Behavioral counters (doc 16:54-62), never wall clock** — in
  `src/compositor/t/tile_planning.t.cpp` (or `counters.t.cpp`): rendering a
  sub-tile bounded layer issues the *same* `composites()` and `requests_issued()`
  as the unclipped path — the clip changes pixels, not counts.
- **Unit — `src/compositor/t/tile_planning.t.cpp`:** with a `CountingBackend`,
  `render_frame_interactive` on a sub-tile bounded layer routes every composite
  through `composite_clipped` (never plain `composite`) and the clip passed
  equals `repaint_rect ∩ device_bounds` (un-gated: `device_bounds`); an
  unbounded layer still routes through plain `composite`.
- **Claims register.** One new row in `tests/claims/registry.tsv`:
  `02-architecture#tile-composite-clipped-to-content-bounds` — "Every composite
  of a layer with finite bounds() is clipped to those bounds mapped (rounded
  out) into device space, so a tile cell extending past the declared extent
  paints no pixel beyond it; an unbounded layer composites unclipped." Enforced
  by an `// enforces:` tag in the golden. **Re-assert, do not re-register**
  `03-layer-plugin-interface#render-within-declared-bounds` (registry:129) and
  `09-surfaces-and-backends#clip-scoped-ops-honor-the-clip` (registry:54).
- **Design-doc delta** (doc 02 step 5) rides in the closer's commit (Decision 4).
- **Coverage.** ≥90% diff coverage on changed lines (CI gate, doc 16:112-118).
- **No conformance-suite run, no TSan/stress scope.** No new content-kind or
  operator contract surface changes (the fix is compositor-internal, reading an
  existing `bounds()`), and no concurrency seam is touched (the clip is a pure
  per-composite value, no shared state) — so neither tier applies.
- **No new WBS leaf.** Every deferral above routes to an already-registered
  sibling (`runtime.camera_change_damage`,
  `runtime.placement_damage_maps_to_device`) or is a decided limitation
  (Decision 3, rotated exactness), not future work. The closer registers no new
  task for this refinement.

## Decisions

### 1. Clip at composite time to a device-space bounds rect, at the single `composite_onto_target` chokepoint

Compute `device_bounds = round_out(composed.map_rect(*bounds))` once per layer
and intersect it into every clip inside `composite_onto_target`
(`tile_planning.cpp:94-107`): non-null clip → `clip->intersect(device_bounds)`;
null clip (un-gated) with bounds → `composite_clipped(device_bounds)`. One
function, one threaded value, five call sites plus `composite_coarser`.

*Rationale:* `composite_onto_target` is the sole composite-onto-target seam
(post-`disjoint_dirty_repaint`), and it already carries the counter bump, so the
clip lands exactly where the repaint-region clip does and composes with it for
free. Reuses `composite_clipped` — the same primitive, no new backend virtual
(parallel to `disjoint_dirty_repaint`).

**Alternatives rejected:**
- *(a) Clip at render time* (size the tile render to `region ∩ bounds`, like the
  offline `render_layer`): a partial tile poisons the cache — its key claims a
  whole cell it did not fill, so a later frame requesting the covered part reads
  a hole. tile_planning's whole-cell decision forbids it. A composite-time clip
  keeps the cached tile whole and reusable across repaint regions.
- *(b) Make `SolidContent` self-clip:* wrong layer. Solid is `infinite` extent
  and trusts the pipeline (the memory note and `main.cpp:77-78`); every content
  kind would independently re-implement the clip, and the offline path already
  enforces it compositor-side. See Decision 2.
- *(c) Substitute the clip when building `tile_clips`* (`:512-554`) instead of
  inside `composite_onto_target`: the clip lists hold `const Rect*` pointing at
  caller-owned `repaint` rects; an intersected rect needs owned storage per
  tile, threading a lifetime through the plan. Intersecting inside the composite
  helper needs only one per-layer `const Rect*` and a stack temporary.

### 2. Enforcement is the compositor's job, not the content's

The compositor owns "request/composite only in-bounds"; the offline path already
does (`compositor.cpp:28-34`), and the interactive path must match it. Content
kinds declare `bounds()` and may or may not self-clip; the compositor makes the
guarantee observable regardless.

*Rationale:* keeps the invariant in one place, symmetric with offline, and does
not depend on every current and future kind honoring bounds internally. It also
means the content-side claim `03…#render-within-declared-bounds` (a content
promise) and this compositor claim are distinct invariants — the compositor clip
holds even for a kind that renders sloppily.

*Rejected:* pushing the clip into each kind's `render` (see Decision 1(b)) —
duplicated, and it cannot fix a whole-cell tile buffer the content filled on
request.

### 3. Clip to the conservative device AABB, rounded *out*; axis-aligned is exact, rotated is conservative

`composite_clipped` takes an axis-aligned `Rect`. Under a rotating/shearing
`composed`, the content's local extent images to a device *quad*; we clip to its
axis-aligned bounding box, rounded out to whole pixels.

*Rationale:* round-out guarantees the clip never removes a pixel the content
legitimately covers — a coverage gap (a visible seam) is a worse defect than a
bounded bleed. For the reported bug and the example (axis-aligned integer
bounds) the AABB *is* the exact extent, so the golden is byte-exact and the bleed
is fully removed. For rotated/sheared placements the residual is at most a
sub-pixel fringe at the extent's diagonal edges — the same tolerance class the
≤1-octave resampler already introduces at rung boundaries — and it errs toward
over-painting inside a conservative box, never toward clipping real content.

**Alternatives rejected:**
- *(a) Round in:* would clip pixels the content legitimately painted at the
  extent edge, converting a bleed into a coverage gap. Never round a clip
  inward.
- *(b) Pixel-exact polygon/quad clip:* needs a new backend primitive
  (`composite_clipped` is rect-only) — a real feature with its own contract and
  goldens, disproportionate to a sub-pixel rotated fringe. Not registered as a
  follow-up: it would be an "add it if it ever matters" task with no concrete
  trigger, which doc 16 forbids as a WBS leaf. If a rotated-extent use case ever
  demands exactness, that is a new, separately-motivated feature.

### 4. Design-doc delta (rides in the closer's commit, doc 16's same-commit rule)

No design doc currently states that the composite is clipped to content bounds
(doc 02:126-131 clips only to the *repaint region*). Per the house rule
(`interactive_pull_wiring` D5, re-cited by `disjoint_dirty_repaint` D5) — *do not
mint a claim id for a sentence no design doc contains* — the new claim
`02-architecture#tile-composite-clipped-to-content-bounds` requires the doc to
say it first. The delta adds a clause to doc 02 step 5 (Composite): every
composite of a layer with finite `bounds()` is clipped to those bounds mapped
(rounded out) into device space; unbounded content is composited unclipped.

*Not a `00-overview` decision-record bullet:* this pays down a known gap between
two existing render paths (it makes tiled match offline), not a project-shaping
architectural choice — same weight as `disjoint_dirty_repaint`'s doc-02 clause.

## Open questions

(none — all decided.)

## Status

**Done** — 2026-07-17.

- Edited `src/compositor/tile_planning.cpp`: added `round_out` helper; `device_bounds` computed once per layer from `composed.map_rect(*bounds)` and threaded through `composite_onto_target` and `composite_coarser` at 5 call sites; null-clip un-gated path routes through `composite_clipped` when bounds exist.
- Edited `src/compositor/t/tile_planning.t.cpp`: added `BoundedSolid` and `ClipRecordingBackend` test doubles; 3 new cases — routing to `composite_clipped` for bounded layer, gated `repaint∩bounds` intersection, unbounded layer still uses plain `composite`; counter-neutral assertion (same `composites()` and `requests_issued()`).
- Created `tests/bounded_content_tile_clip_golden.t.cpp`: 4 byte-exact golden cases — sub-tile bounded solid does not bleed, bounded solid straddling tile boundary clips in both cells, gated repaint of sub-tile bounds does not bleed, bounds-covers-tile produces pre-task bytes.
- Edited `tests/CMakeLists.txt`: wired `bounded_content_tile_clip_golden` test target linking `arbc` + `CpuBackend`.
- Edited `tests/claims/registry.tsv`: registered new claim `02-architecture#tile-composite-clipped-to-content-bounds`; golden enforces it plus re-asserts `03-layer-plugin-interface#render-within-declared-bounds` and `09-surfaces-and-backends#clip-scoped-ops-honor-the-clip`.
- Edited `docs/design/02-architecture.md`: added doc-02 step-5 clause (Decision 4) — every composite of a layer with finite `bounds()` is clipped to those bounds mapped (rounded out) into device space; unbounded content composited unclipped.
