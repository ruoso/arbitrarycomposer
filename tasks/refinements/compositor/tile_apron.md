# compositor.tile_apron — Sample past the tile edge, paint only the cell

The compositor resamples every tile surface against a **transparent border that is
a tiling artifact, not a content edge**. Two user-visible defects fall out of that
one fact: a seam grid at every internal tile boundary whenever the composite has
fractional phase, and a hard, un-antialiased edge (plus up to 1 device px of
bleed) wherever a content's declared `bounds()` ends. This task gives every tile
surface an **apron** of real neighbouring content to sample from, restricts each
tile's *paint* to its own cell through a new source-space window on the composite,
and zeroes the apron only where the content's declared extent genuinely ends.

## TaskJuggler entry

`tasks/35-compositor.tji` — `task tile_apron`, `effort 5d`, `allocate team`,
`depends !bounded_content_tile_clip`. Docs 02/04/07/09.

## Effort estimate

**5d.** One new backend virtual (`composite_windowed`) plus its CPU kernel arm and
the four testing-backend forwards; the tile-geometry change in `tile_planning.cpp`
(surface dims, render rect, plan growth, the apron zeroing, retiring the
device-bounds clip); the same two rules in `pull_service.cpp` (tile render +
`deliver_tile`); the damage-invalidation rect widening; the twin rule in
`kind_nested`; and the golden re-baseline. The pixel churn is the bulk of it: ~10
tests move, several of them byte-exact goldens whose new values must each be
derived, not regenerated.

## Inherited dependencies

**Settled (formal `depends`):**

- `compositor.bounded_content_tile_clip` — established the composite-time
  device-bounds clip (`composite_onto_target`'s `device_bounds` parameter,
  `tile_planning.cpp:116-133`) and its Constraint 2, *"a coverage gap is a worse
  defect than a bounded bleed"*, which is why the clip rounds **out** to whole
  device pixels. This task **retires that clip** — see Decision 4. Its Decision 2
  ("enforcement belongs in the compositor, not in `SolidContent`") is kept and in
  fact strengthened: the compositor now enforces the extent in the tile's pixels
  rather than in the destination's.
- `compositor.tile_planning` — whole-cell tiles (`k_tile_size = 256`), the
  `plan_layer` pure planner, `tiles_covering`, `render_frame_interactive`.
  Decision: **tiles are whole cache cells**; a partially-filled tile poisons the
  cache. Honored — the apron makes the cached tile *larger*, never partial.

**Settled (informal — seams this builds on):**

- `compositor.refine_frame_composite_idempotence` — `Backend::composite_clipped`
  and its Decision 1(e): a *device-space clip rect* is the right parameter because
  tiles are axis-aligned in local space while the device footprint is the full
  affine's image. This task adds the complementary half — a *source-space* window —
  for the same reason (Decision 2).
- `compositor.disjoint_dirty_repaint` — the `composite_onto_target(...,
  std::span<const Rect* const> clips, ...)` chokepoint the window threads through.
- `compositor.pull_multi_tile_region` — `deliver_tile`'s "each covering tile holds
  its own disjoint slice of the region and delivers seam-free" claim
  (`pull_service.cpp:337-346`). That claim is **false at fractional phase** for
  exactly the reason below; this task makes it true.

**Pending (downstream, not blocking):** none.

## What this task is

### The measurements this is built on

Both defects were measured directly, not inferred.

**Defect B — the seam.** One opaque `SolidContent`, local extent `600×600`, layer
transform `translation(0.5, 0.5)`, camera `scaling(1.0)`, rendered through
`render_frame_interactive`. `k_tile_size` is 256, so internal cell boundaries fall
at local 256 and 512. Alpha along a scanline:

```
x=254 a=1.000000     x=510 a=1.000000
x=255 a=1.062500     x=511 a=1.062500
x=256 a=0.750000  ←  x=512 a=0.750000  ←  cell boundary
x=257 a=1.062500     x=513 a=1.062500
```

The interior of an opaque solid is not opaque. Each tile's Catmull-Rom tap reads
its own surface's transparent border (`kernels.hpp:66-68`), so the boundary pixel
is painted twice at roughly half weight each and source-over gives `0.5 + 0.5·0.5
= 0.75`, with the cubic's negative lobe ringing to 1.0625 either side. It is
invisible only when the composite's phase is integral (weights collapse to
`(0,1,0,0)`, `kernels.hpp:88-91`) — i.e. it appears on any sub-pixel pan, any
off-rung scale, and any rotation, as a dark grid every 256 device pixels.

**Defect A — the bounded-content edge.** `tests/nested_goldens.t.cpp`'s scene
(two `SolidContent`s, extent `8×8`, layers at identity and `translation(1,1)`, an
`8×8` composition) at 0.5×. `SolidContent` fills whatever target it is handed
(`solid_content.cpp:26-30`, "the compositor only requests regions within declared
bounds"), and the tiled path hands it the whole cell — so the tile carries solid
colour well past the content's extent, and the extent is re-imposed at composite
time by a clip rounded out to whole device pixels. At device pixel `(4,2)` the
content covers exactly half the pixel; the flat path paints `a=0.5` (full), while
`NestedContent`, whose temp is sized to the content region and therefore falls off
against its own border, paints `a=0.25` (correct). **The flat path is the wrong
one**, and its wrongness is `bounded_content_tile_clip` Constraint 2's accepted
cost, now paid down.

The two defects have one root: *the compositor resamples a tile surface whose
border is a property of the tiling, not of the content.*

### The rules

**Rule 1 — every tile surface carries an apron.** A tile's **cell** is unchanged:
`k_tile_size` device px at its rung, aligned to the local origin
(`tile_local_rect`). It remains the tile's identity, its cache-key geometry, and
the unit of arrival damage. Its **surface** becomes `k_tile_size + 2·k_tile_apron`
px covering `tile_render_rect(rung, coord)` — the cell grown by `k_tile_apron /
rung_scale(rung)` local units per side — and the tile's `RenderRequest` covers that
aproned rect. Neighbouring tiles overlap inside the apron and, renders being
deterministic (doc 16), hold identical pixels there: the apron *is* the neighbour.

**Rule 2 — the apron is sampled, never painted.** Each tile composites through a
**source-space window** equal to its own cell: in tile-surface pixel coordinates
that is the constant `[k_tile_apron, k_tile_apron + k_tile_size)²`. A destination
pixel is painted iff its sample position falls inside the window. Adjacent cells
abut exactly in source space and the affine is a bijection, so every device pixel
is painted by **exactly one** tile — no gap, no overlap — while its 4×4 tap still
reads the apron's real colour. That is the seam fix, and it is exact under
rotation and shear, which no destination-side scissor can be (Decision 2).

**Rule 3 — the apron is zeroed where the content's extent ends.** After a tile
render, if the content declares a finite `bounds()`, every pixel of the tile
surface outside `bounds()` (mapped to surface-pixel space) is cleared to
transparent. Inside the content the apron is real neighbour colour, so Rule 2's
tap sees no border; at the content's own edge the apron is genuinely empty, so the
tap falls off smoothly — the antialiased extent edge `NestedContent` already
produces. This *is* the bounds enforcement, moved from the destination to the
tile, and it makes `bounded_content_tile_clip`'s round-out clip redundant
(Decision 4).

**Rule 4 — plan one tap past the region.** `tiles_covering` is called on the
layer's `region` grown by `k_composite_tap_reach / rung_scale(rung)` local units,
and the `wanted` footprint is grown identically so the deadline sweep does not
cancel the added ring. Without this, a content whose extent lands *exactly on* a
cell boundary — the common case, since most content's `bounds()` start at the
local origin and so does the grid — loses its falloff: the pixels that should
receive it sit in the neighbouring cell, which Rule 2 assigns to a tile that was
never planned. The added tiles are outside the content's extent and Rule 3 zeroes
almost all of them; they exist to carry the ≤2px falloff.

**Rule 5 — the coarser fallback inherits both.** `composite_coarser`'s pool temp
becomes aproned and is filled over the fine tile's aproned rect from the coarser
tile — which has its own apron, so the fill reads real colour — and the final
paint uses the same cell window.

**Rule 6 — the pull service takes the same two rules.** `deliver_tile` paints
through the cell window (making `pull_multi_tile_region`'s seam-free claim true),
and the pull service's own tile render is aproned and zeroed outside `bounds()`.

**Rule 7 — `NestedContent` takes Rules 3 and 4, not 1 and 2.** Nested composes each
child layer through **one** temp, so it has no internal boundaries to seam and
needs no window. But its `region` is `inv(device_rect) ∩ bounds`, and the two
clips want opposite treatment: a border at the **bounds** edge is correct falloff,
a border at the **viewport** edge is ringing. Growing the region by
`k_composite_tap_reach / scale` and then zeroing outside `bounds()` distinguishes
them exactly — real content appears where the clip was the viewport, zeros where it
was the extent. The sub-pixel/Droste-floor cull (`nested_content.cpp:373-379`)
stays measured on the **ungrown** footprint, or a `<1×` cycle loses its floor
(doc 05:61-65).

**Not this task:**

- Wiring `reduce_rung` into production. It is unreferenced outside tests, and its
  even-source-dims contract predates the apron: halving a 264px aproned tile gives
  a 4px apron at the coarser rung's scale, i.e. 2 coarser px — below the tap reach.
  Registered as tech debt (`compositor.reduce_rung_apron_geometry`).
- A pixel-exact polygon clip of a rotated extent. Rule 3 clears the extent's
  conservative AABB in tile-surface space, inheriting `bounded_content_tile_clip`
  Decision 3 unchanged.
- Retiring the destination-side `composite_clipped`. The repaint-rect clip is
  orthogonal to the source window and both are threaded together (Decision 3).

## Why it needs to be done

Defect B is a correctness bug on the interactive path's most ordinary gesture:
pan by a sub-pixel amount and an opaque fill grows a grid of translucent lines.
Nothing in the suite caught it because every landed golden composites at integral
phase, where the tap collapses to the nearest texel.

Defect A is what blocks the three `[!mayfail]` goldens in
`tests/nested_goldens.t.cpp` — the enforcement of doc 05:24, *"rendering is
recursion"*. They pin the identity that a nested composition renders
byte-identically to compositing the child's layers flat. It held while both paths
shared the retired untiled `render_layer`; `6a3e30a` moved the flat oracle onto the
tiled driver and the identity broke, because the two paths disagree about what
happens at a content's edge. Rule 3 makes the tiled path agree with the recursive
one — verified: with `nested_content.cpp` untouched, zeroing the tile outside
`bounds()` and dropping the round-out clip turns all three green at 1.0×, 0.5× and
0.25×, with a per-pixel probe reporting zero differences.

## Inputs / context

- **doc 02** `§ The frame, interactively` (02:57-71) — the tile grid, the
  degradation ladder, the composite step the window scopes.
- **doc 04:95-98,102-106** — the ≤1-octave remainder "applied as resampling during
  compositing", and the local-space tile axis-alignment that forces a device-space
  (or, now, source-space) parameter rather than a source sub-rect.
- **doc 05:24** — "rendering is recursion", the identity Rule 3 restores; **05:61-65**
  — the `<1×` Droste floor Rule 7 must not break.
- **doc 07 § Resampling filters** — the Catmull-Rom bank (`k_magnify_taps = 4`,
  `k_magnify_first_tap = -1`, `image_resampler.hpp:129-133`) that sets the tap
  reach, and rule 3's decoded-premultiplied-linear working space.
- **doc 09** — `Backend`'s clip-scoped operations (`backend.hpp:60-70`), the family
  `composite_windowed` joins; the errors-as-values allocation posture.
- `src/backend_cpu/kernels.hpp:103-147` — `source_over_kernel`, where the window
  test lands; `:59-73` `fetch_texel`'s zero border, the artifact's mechanism.
- `src/compositor/tile_planning.cpp:116-133` (`composite_onto_target`), `:143-179`
  (`composite_coarser`), `:209-214` (`tile_local_rect`), `:463-521` (the per-layer
  cull, `device_bounds`, the `wanted` footprint), `:645-733` (the miss fill).
- `src/compositor/pull_service.cpp:105-130` (`deliver_tile`), `:325-350` (the tile
  render descriptor).
- `src/compositor/refinement.cpp:267` (arrival damage over the **cell** — unchanged)
  and `src/compositor/damage_planning.cpp:273` (`invalidate_region` over
  `tile_local_rect` — must widen to the aproned rect, or a damaged neighbour leaves
  a stale apron behind).
- `src/kind_nested/nested_content.cpp:319-450` (`compose_child_layer`).

## Constraints / requirements

1. **The cell stays the tile's identity.** `TileKey`, `tiles_covering`,
   `tile_local_rect`, arrival damage and the `wanted` footprint keep talking about
   cells. The apron is surface geometry only, never key geometry — a tile must not
   become un-findable because a neighbour was planned.
2. **Every device pixel is painted exactly once per layer.** Rule 2's window and
   the repaint-rect clips compose by intersection; the repaint rects are already
   pairwise disjoint (`disjoint_dirty_repaint`), so the composite count stays one
   bump per clip entry and `composites` is counter-neutral on the un-gated path.
3. **The apron never leaks into the cache's notion of a tile.** `tile_byte_cost`
   reads the surface, so the +6.3% (`(264/256)² = 1.0634`) is accounted
   automatically; no cache change is needed.
4. **Levelization (doc 17:56).** `composite_windowed` is added to the L2
   `surface::Backend` seam; the compositor (L4) and `kind_nested` reach it only
   through that virtual. No new `backend-cpu` edge.
5. **`k_tile_apron` must survive the coarser fallback.** A `k`-octave fallback fills
   a fine temp spanning `(k_tile_size + 2a)/2^k` coarser px whose own tap adds 2
   coarser px, so the coarser surface must extend `a/2^k + 2` past its cell:
   `a(1 − 2^-k) ≥ 2`, tightest at `k = 1`, giving **`a ≥ 4`**. `k_tile_apron = 4`,
   and `k_tile_size + 2a = 264` stays even.
6. **Unbounded content is untouched by Rule 3.** `bounds() == nullopt` (doc
   01:68-77) zeroes nothing; the apron is always real content, which is exactly
   right for content with no extent.
7. **RT-safety and determinism.** No clock reads, no allocation in the kernel, fixed
   tap order (doc 16).

## Acceptance criteria

- **The seam is gone.** A new byte-exact golden renders the 600×600 opaque solid at
  `translation(0.5, 0.5)` and asserts a uniform interior across both cell
  boundaries — the measurement above, inverted into a regression guard.
- **The recursion identity holds.** The three `[!mayfail]` tags in
  `tests/nested_goldens.t.cpp` are removed and the cases pass at 1.0×, 0.5× and
  0.25×, homogeneous and across the heterogeneous working-space boundary.
- **The extent edge is antialiased.** A golden pins the half-covered pixel at a
  bounded content's edge at its coverage-weighted value, not the whole-pixel clip.
- **Counter neutrality.** `composites`, `requests_issued` and `degraded_composites`
  are unchanged on the un-gated path for a scene whose region does not gain a Rule-4
  ring; the ring's extra tiles are asserted explicitly where it does.
- **Existing goldens re-derived, not regenerated.** Each moved value is justified in
  the test's comment against the coverage it now expresses.
- Full `./scripts/gate` green (its exit code, not a `tail` of its log).

## Decisions

1. **Apron, not overlap-free re-rendering.** The alternative — resolve each layer's
   tiles into one layer-sized surface by exact copy, then composite once — also kills
   the seam and makes the recursion identity structural. Rejected for this task: it
   allocates a full visible-region surface per layer per frame and rewrites the
   damage-gated incremental path, where the apron is a bounded 6.3% memory cost and a
   local change. Recorded as the eventual endpoint if per-layer resolve is ever wanted
   for other reasons.
2. **A source-space window, not a destination-space scissor.** Partitioning device
   pixels between tiles with rects works only while the composed affine is
   axis-aligned; under rotation or shear adjacent cells' device AABBs overlap, so a
   round-out scissor double-blends a band and a round-in one opens a gap. Testing the
   *sample position* against the cell in source space is exact for any affine, costs
   one rect test per destination pixel, and needs no case analysis — so
   `composite_windowed` carries both a destination clip and a source window, and
   `composite_clipped` stays the whole-source case of it.
3. **Both clips, composed.** The destination clip (repaint rects) and the source
   window (the cell) answer different questions — *which pixels may this frame
   touch?* versus *which pixels does this tile own?* — and are threaded together
   rather than fused.
4. **Retire the device-bounds composite clip.** Rule 3 supersedes it and is strictly
   better: antialiased instead of hard-edged, and with no ≤1px bleed. Keeping both
   would be actively wrong — the round-out clip is only 1px wider than the extent
   while the falloff reaches 2px, so it would truncate exactly the antialiasing this
   task adds. `bounded_content_tile_clip`'s Constraint 2 ("a coverage gap is worse
   than a bounded bleed") is honored, not reversed: Rule 4 is what guarantees no
   coverage gap now that the bleed is gone.
5. **`k_tile_apron = 4`, derived not chosen.** Constraint 5's inequality; 2 would
   serve a tile's own composite but starve the coarser fallback.
6. **Nested keeps its single temp.** Rule 7 rather than making `NestedContent` drive
   the tiled planner. The temp has no internal boundaries, so Rules 1-2 buy it
   nothing, and routing a kind through the compositor would add an L5→L4 edge the
   levelization does not have.

## Open questions

(none — all decided)

## Status

**Done — 2026-07-25.**

Artifacts:

- **Backend seam.** `Backend::composite_windowed` (`src/surface/arbc/surface/backend.hpp`),
  its CPU arm (`cpu_backend.cpp`, the window test in `kernels.hpp`'s
  `source_over_kernel`), and the forwarding / counting / stub / export-monitor
  overrides. `CountingBackend` counts it apart from `composite_clipped`.
- **Tile geometry.** `k_composite_tap_reach`, `k_tile_apron = 4`,
  `k_tile_surface_size = 264`, `k_tile_cell_window`, `tile_block_window`,
  `tile_render_rect`, `tiles_covering_render` and `clear_tile_outside_bounds`
  (`tile_planning.hpp` / `.cpp`); the aproned tile surface and request region at all
  three render sites (`tile_planning.cpp`, `pull_service.cpp`, and the arrival drain in
  `refinement.cpp`, which carries the extent on `PendingTile::bounds`);
  `damage_planning.cpp` invalidating by render rect.
- **Retired.** `composite_onto_target`'s `device_bounds` parameter and the `round_out`
  helper behind it — `bounded_content_tile_clip`'s composite-time clip.
- **`NestedContent`.** `grow_within_bounds` in `nested_content.cpp`: Rule 7, and no
  zeroing pass, since clamping the growth to `bounds` means the temp never covers
  anything outside the extent in the first place.
- **Docs.** Doc 02 step 5 rewritten (apron + window + tile-side extent enforcement);
  doc 09 gained "The source-space paint window" and the `provided_origin` placement
  clause. Claims `02-architecture#tile-composite-clipped-to-content-bounds` retired,
  replaced by `#bounded-content-extent-zeroed-in-tile` and
  `#tile-paints-its-cell-samples-its-apron`.
- **Goldens.** `tests/tile_apron_golden.t.cpp` (the seam repro inverted into a guard,
  and the coverage-weighted extent edge); the three `[!mayfail]` tags removed from
  `tests/nested_goldens.t.cpp`; `tests/walking_skeleton.t.cpp`'s green rows now
  antialias symmetrically at both edges rather than at the tile-grid-aligned one.

Two things surfaced during implementation that were not in the plan above:

- **Rule 4 was replaced by `tile_block_window`.** Growing the planned region by a tap
  reach did place the extent-edge falloff correctly, but for the very common case of
  content whose `bounds()` start at the local origin it planned a whole extra ring of
  tiles — measured 4x the renders on the interactive tests, 9x once nested's own growth
  straddled a cell boundary and the pull service covered it by cells. Opening the
  window outward at the plan's coord extremes catches the same falloff, from the edge
  tile's own (already zeroed) apron, for nothing. Same for `tiles_covering_render`,
  which is what stops an operator's aproned request from naming its input's tile plus
  both neighbours in each axis.
- **`RenderResult::provided_origin` (doc 09).** The zero-copy provided-surface path
  copied at the identity, which silently assumed the provided pixels start at the
  request region's origin. That held only for the tile at the local origin — any other
  tile would already have mislaid a decoder's frame — and the apron made every tile
  request start one apron earlier, so `org.arbc.imageseq` composited a frame four
  pixels up and to the left. A latent bug the apron exposed rather than caused; stating
  the origin fixes it for any region.
- **`BenchBackend` had to model `clear_rect` and `composite_windowed`.** It inherited
  `StubBackend`'s delegate-to-the-unclipped-op defaults, which are safe when
  `clear_rect` clears a repaint *region* and actively wrong when it clears a *complement*
  — every bench scene rendered fully transparent. The hazard `StubBackend`'s own header
  warns about, landing exactly as described.
