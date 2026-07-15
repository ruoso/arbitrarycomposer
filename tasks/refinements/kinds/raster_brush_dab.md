# Refinement — `kinds.raster_brush_dab`

## TaskJuggler entry

`task raster_brush_dab` in [`tasks/55-kinds.tji`](../../55-kinds.tji) (line 60),
under `task kinds`. Back-link on completion: the `note` line ends with
`Refinement: tasks/refinements/kinds/raster_brush_dab.md`.

## Effort estimate

`effort 2d`. The expensive machinery underneath the dab — the persistent CoW
tile table, the incremental mip recompute, gesture coalescing, undo through
the `Editable` capture/restore facet, and touched-tile damage — is already
built, shipped, and claim-pinned. This task changes only the innermost
per-pixel write and the seam that carries a coverage value into it, plus its
goldens and the doc-14 delta. The 2-day estimate is goldens + delta, not
plumbing.

## Inherited dependencies

**Settled predecessors this task builds on (all `complete 100`):**

- `kinds.raster` — the `org.arbc.raster` kind: the persistent CoW tile table,
  the pyramid, `RasterStore::paint`, the `Editable` capture/restore facet, and
  the touched-tile damage discipline. Reference proof of doc 14's capture
  discipline (claim `14-data-model-and-editing#raster-paint-copies-only-touched-tiles`).
- `kinds.raster_pool_backing` — tile-pixel payloads live in a per-content
  `BigBlockPool`; untouched tiles are shared across CoW versions by the pool
  per-slot refcount (`Level::tiles` is a `std::vector<BlockSlotRef>`, read on
  render workers through `peek`, retain/release confined to the writer/drain
  thread). Claim `15-memory-model#raster-tile-pixels-pool-backed`.

Both are inherited settled — this task does not re-open the CoW copy path, the
pool backing, or the mip recompute; it composites into the fresh touched blobs
those paths already allocate.

**Pending (must not be assumed at implementation time):** none. Everything this
task touches is landed at HEAD `2e43cee`.

## What this task is

Give `RasterStore::paint` a **coverage mask** and a **source-over blend**, so
the `org.arbc.raster` kind can back a real brush. Today the dab is a hard
REPLACE of a flat colour inside an axis-aligned rect: `raster_content.cpp:415-423`
loops the touched tiles' pixels and does
`if (gx < w && gy < h && center_inside(region, gx, gy)) put(px, edge, ix, iy, color);`
— no coverage falloff, no antialiasing, no opacity, no blend against the
destination. That yields hard-edged rectangular stamps of an opaque colour,
which is not a brush.

This task changes **only** the innermost per-pixel write: the seam takes a
caller-supplied coverage source alongside the region and colour, and composites
`color` **over** the destination pixel at that coverage in premultiplied linear
working floats. A hard/soft round dab is the reference coverage generator; an
explicit alpha-mask span is the general form. Everything else — CoW, mip
recompute, coalescing, undo, damage — is unchanged and must stay byte-for-byte
identical for the special case that reproduces today's behaviour (a full-coverage
opaque dab).

## Why it needs to be done

`org.arbc.raster` is the paintable surface of the whole editor: doc 03:298-310
makes it the kind you retouch a photograph *on* (you stack an editable raster
over a referenced `org.arbc.image`). The v0.1 driving consumer is "an image
editor with simple brushes" (M9 note, `tasks/99-milestones.tji:73`), and the M9
note names "the masked/blended brush dab (the raster kind could only stamp
hard-edged flat-colour rects)" as one of the correctness gaps M9 closes, not
polish. The rect stamp is a placeholder that predates the brush; this task is
the promised replacement. It has no WBS predecessor for the brush semantics
themselves — the origin is `tasks/parking-lot.md` (2026-07-12), which observed
there was no WBS task for the brush itself; this refinement is that task.

Downstream: the disjoint-damage repaint (`compositor.disjoint_dirty_repaint`)
and per-object revision (`model.per_object_revision`) work in M9 assume a real
brush stroke as their exercising workload; a coverage-masked dab is what makes
that workload real rather than a rectangle.

## Inputs / context

**Governing design-doc sections (normative, doc 16):**

- `docs/design/07-color-and-pixel-formats.md:18-31` (rules 2 and 3) — **the
  blend-space authority.** Rule 2: "All compositing happens in the composition's
  working space on premultiplied alpha." Rule 3: "Blending and resampling in a
  nonlinear space is mathematically wrong … linear f16 is correct." Blending in
  premultiplied linear working floats is *already normative here* — this task
  applies it to the dab, it does not amend it. Lines 37-47 give the clamp
  discipline the resampling filters use (clamp to non-negative per channel; do
  not clamp RGB to ≤ alpha — HDR headroom above alpha is legitimate), which the
  blend inherits.
- `docs/design/14-data-model-and-editing.md:184-211` — **the delta target.** The
  paint-discipline block: capture-once-per-entry / mutate / damage (`:184-195`),
  the structural-sharing tile-table proof (`:197-204`), and the incremental-mip /
  dilation invariant (`:206-211`). This block states the paint's CoW, capture,
  coalesce, mip, and damage discipline in full but is **entirely silent on what
  the mutation writes** — no "coverage", no "blend", no "source-over" anywhere in
  doc 14. The delta extends this block (see Decisions).
- `docs/design/01-core-concepts.md:29` — "`blend`: blend operation (v1:
  source-over only)". The blend op v1 scope is source-over; the dab is its
  content-side instance.
- `docs/design/02-architecture.md:95-100` — "Compositing is source-over, which is
  not idempotent for anything but fully-opaque content." The property that
  distinguishes a translucent blend from a replace, and the reason the
  full-coverage-opaque special case is the *only* one that reduces to a replace.
- `docs/design/03-layer-plugin-interface.md:215-221,277,298-310` — the render
  contract only demands mutation be visible (damage + revision) and rendering be
  pure over pinned state; raster is "codec-free and editable", the paintable
  document-state surface.

**Real source seams (paths + lines, at HEAD `2e43cee`):**

- `src/kind_raster/raster_content.cpp:377-488` — `RasterStore::paint`. The
  level-0 CoW clone + per-pixel write is `:397-432`; the innermost write to
  change is `:415-423` (the `center_inside` rect test + `put(px, …, color)`).
  The higher-level mip recompute (`:435-484`) is **untouched** — it reads the
  already-updated child level and is agnostic to how level 0's touched pixels
  got their values.
- `src/kind_raster/raster_content.cpp:87-95` — `put(float* px, …, const
  WorkingPixel& c)`, the raw premultiplied-float store the blend replaces at the
  covered pixels.
- `src/kind_raster/raster_content.cpp:231-235` — `center_inside(const Rect&, x,
  y)`, the pixel-center-at-`+0.5` sampling convention (`cx = x + 0.5`) the
  coverage sampler must match.
- `src/kind_raster/arbc/kind_raster/raster_content.hpp:204-210` — the
  `RasterStore::paint` declaration and doc comment; `:295` — the content-level
  `RasterContent::paint(Model::Transaction&, ObjectId, const Rect&, const
  WorkingPixel&)`.
- `src/kind_raster/arbc/kind_raster/raster_content.hpp:75` — `using TileFill =
  std::function<bool(std::size_t index, std::span<float> dst)>`. The abstraction
  precedent for handing the kind a caller-supplied callable; the coverage sampler
  mirrors its shape and its L4-boundary rationale ("hand the kind nothing but
  pixels/scalars").
- `src/media/arbc/media/pixel_traits.hpp:17,25-38` — `WorkingPixel =
  std::array<float,4>`, and `premultiply`/`unpremultiply` (the blend operates on
  premultiplied working floats; no unpremultiply is needed in the blend itself).
- `src/kind_raster/raster_content.cpp:660-672` — `RasterContent::paint`, the
  content-level forwarder that sets content state, emits touched-tile damage, and
  rebases the live base. Unchanged in shape.

**Test surfaces:**

- `src/kind_raster/t/raster_paint.t.cpp` — the CoW touched-tile allocation
  witness (`:180-230`), the paint→full-rebuild mip-equivalence oracle
  (`:246-293`), and the L2 end-to-end sink / coalescing / capture-restore cases.
- `src/kind_raster/t/raster_goldens.t.cpp` — the byte-exact golden suite
  (`enforces: 16-sdlc-and-quality#byte-exact-goldens`); frozen tables embedded
  inline as `k`-prefixed `constexpr` arrays, regenerated through the hidden
  `[.regen]` dumper (`:706`).
- `tests/raster_conformance.t.cpp` — the `arbc::contract_tests(factory)`
  conformance run over the kind.

**Out-of-scope boundaries (do not touch):** the CoW copy path, the pool backing,
the mip recompute + dilation, the coalescing/undo/damage plumbing, and the
`org.arbc.raster` serialization (a dab writes touched tiles; the tile store
already persists them). Per-stroke coverage accumulation (the opacity-vs-flow
overlap problem) is a named follow-up, below — not this task.

## Constraints / requirements

1. **The seam gains a coverage source, not a new store.** Add a coverage
   parameter to `RasterStore::paint` (mirror `TileFill`: a
   `std::function<float(int gx, int gy)>` over level-0 pixel coordinates,
   returning per-pixel coverage in `[0,1]`). The existing flat-fill signature is
   retained as a forwarding overload passing a constant `1.0f` coverage, so every
   current caller and golden is the full-coverage case unchanged. `gx,gy` are
   global level-0 pixel coordinates; the sampler is evaluated at the same
   pixel-center convention `center_inside` uses (`gx + 0.5`, `gy + 0.5`).

2. **The blend is premultiplied-linear source-over.** At each covered pixel, with
   destination `dst` and per-pixel coverage `a = clamp(coverage(gx,gy), 0, 1)`:

   ```
   src' = color * a                         // premultiplied colour scaled by coverage
   out  = src' + dst * (1 - color[3] * a)   // premultiplied source-over
   ```

   Computed in float32 working space (the tile format), fixed operation order, no
   `libm`, so the result is a byte-exact deterministic function of `dst`,
   `color`, and the coverage value (doc 07:37-47 discipline, matching the
   resampling kernels). Clamp each output channel to non-negative
   (`std::max(0.0f, v)`); do **not** clamp RGB to ≤ alpha (HDR headroom is
   legitimate, doc 07:44-47). No unpremultiply/premultiply round-trip: the tile
   format is already premultiplied, so the blend is pure premultiplied algebra.

3. **The full-coverage opaque dab is byte-identical to today.** For `a = 1` and
   an opaque colour (`color[3] = 1`), the blend reduces to `out = color` — exactly
   `put(px, …, color)`. This is the load-bearing preservation: the existing
   CoW/mip/coalescing/damage claims and every shipped golden must hold
   byte-for-byte. (A *translucent* flat colour under the retained overload now
   source-over blends instead of replacing — the correct brush semantics; no
   shipped golden exercises a translucent flat fill, and the special case the
   task preserves is explicitly the *opaque* full-coverage one — see Decision 3.)

4. **A single dab composites each covered pixel exactly once.** The per-pixel
   write visits each touched pixel once (the existing single-pass tile loop), so
   a partial-opacity dab applies its coverage once, never twice — no
   double-darkening *within one dab*. (Double-darkening where *consecutive dabs
   in one stroke* overlap is the per-stroke accumulation problem, deferred below.)

5. **Coverage falloff is deterministic and `libm`-free.** The reference round-dab
   generator must produce coverage as a byte-exact deterministic function so its
   goldens are byte-exact. Drive the falloff by normalized *squared* radial
   distance (avoiding `sqrtf` and all `libm`), remapped by a fixed-order
   polynomial between an inner hard radius and the outer radius, scaled by the
   brush opacity, clamped to `[0,1]`. A hard dab is binary coverage
   (pixel-center inside the circle); a soft dab is the polynomial falloff. The
   exact profile is an implementation choice pinned by golden — not a designed
   invariant — and the explicit alpha-mask span form sidesteps it entirely for
   callers wanting arbitrary shapes.

6. **The mip recompute and damage are unchanged.** The dab writes level 0's
   touched pixels; the incremental mip recompute (`:435-484`) and its
   dilation-by-kernel-radius run exactly as today over the updated child level,
   so `14-data-model-and-editing#paint-mip-recompute-matches-full-rebuild` holds
   unchanged. Damage stays the touched-tile set: a soft dab whose falloff reaches
   the far edge of a tile still touches that tile (it wrote a blob for it), so
   the touched-tile bounding rect is reported as before.

7. **Levelization (doc 17).** The coverage sampler takes plain ints and returns a
   plain float; the reference round-dab generator lives in `kind_raster` (its one
   call site), not in `media`. `media` stays format/kernel primitives; a brush
   dab is an editing tool with no second consumer (doc 17:61, `arbc::media` L1).

## Acceptance criteria

**The opaque full-coverage regression envelope holds byte-for-byte.** The entire
existing `raster_paint.t.cpp` and `raster_goldens.t.cpp` suites pass unmodified
(the retained flat overload routes an opaque colour at coverage `1` to
`out = color`), witnessing that claims
`14-data-model-and-editing#raster-paint-copies-only-touched-tiles` and
`#paint-mip-recompute-matches-full-rebuild` and every shipped golden survive the
seam change. This is the primary correctness gate.

**New golden coverage (the coverage + blend must actually be exercised):** add
byte-exact goldens in `raster_goldens.t.cpp` (regenerated through the `[.regen]`
dumper) for:

- a **soft-edged round dab** over a known destination — pins the polynomial
  falloff and the source-over result at partial coverage, at level 0 and on the
  mip rungs above it (the dab's falloff must decimate through the pyramid
  byte-exactly);
- a **partial-opacity dab** (`opacity < 1`) over a known non-transparent
  destination — pins `out = color·a + dst·(1 − color[3]·a)` per pixel, and by
  hand-computed comparison witnesses that each covered pixel is composited
  **exactly once** (Constraint 4), not twice;
- an **explicit alpha-mask span** dab reproducing an arbitrary coverage field —
  pins the general form independent of the round-dab generator.

**New claim (claims-register growth, doc 16).** Add one row to
`tests/claims/registry.tsv`:
`14-data-model-and-editing#raster-dab-is-coverage-masked-source-over` — "*A
raster dab composites `color` over the destination at a caller-supplied per-pixel
coverage in premultiplied linear working floats
(`out = color·a + dst·(1 − color[3]·a)`, per-channel non-negative-clamped, no
libm), not a replace; a full-coverage opaque dab reduces to the prior REPLACE
byte-for-byte, so the CoW/mip/coalescing/damage claims hold unchanged, while a
partial-coverage or partial-opacity dab blends against the destination exactly
once per covered pixel.*" Enforced by an `enforces:`-tagged test in
`raster_paint.t.cpp` driving a partial-opacity dab through the content-level API
against a real `Model`/`Journal` and asserting the per-pixel source-over result
plus the byte-for-byte opaque-full-coverage reduction.

**Re-asserted (a second `enforces:` line each, no new registry row):**
`07-color-and-pixel-formats#resampling-in-linear-premultiplied-space` (the blend,
like the resampling it sits beside, is premultiplied linear working floats) on
the soft-dab golden; `16-sdlc-and-quality#byte-exact-goldens` on every new
golden.

**Conformance suite.** `tests/raster_conformance.t.cpp`
(`arbc::contract_tests(factory)`) stays green — a dab is still a visible,
capture/restore-round-tripping, pure-over-pinned-state mutation; nothing in the
facet contract changes.

**CI gates.** ≥90% diff coverage on the changed lines (the blend, the coverage
seam, the reference dab generator, and their goldens are the task, not a
follow-up); `tj3 project.tjp` silent after the closer's `complete 100`.

**Deferred follow-up (closer registers in WBS):** per-stroke coverage
accumulation — deferred to `kinds.raster_stroke_coverage_buffer` (closer
registers in WBS; milestone **M10** — a polish enhancement, not a v0.1 release
gap, since v0.1 ships "simple brushes"). Scope: accumulate each gesture's dab
coverage in a per-stroke mask (max/union, not additive) so overlapping dabs
*within one stroke* composite against the destination exactly once at gesture
end — the GIMP opacity-vs-flow distinction. Effort ~2d, `depends
!raster_brush_dab`, Docs 07/14.

## Decisions

- **Add a coverage sampler to the seam; keep the flat overload as a forwarding
  special case.** `RasterStore::paint` gains
  `const std::function<float(int,int)>& coverage`; the existing
  `paint(base, region, color, touched_out)` forwards with a constant-`1.0f`
  sampler. This is the minimal, one-seam change and makes the entire existing
  golden/claim suite the coverage-≡1 special case automatically.
  *Alternative rejected:* a separate `paint_dab` method — it would duplicate the
  CoW clone / mip recompute / damage plumbing or force both methods through a
  shared private core anyway, for no gain; the coverage callback *is* the
  generalization.
  *Alternative rejected:* a dense coverage buffer materialized over the region
  bounding box — O(region) transient the callback avoids, and the explicit
  alpha-mask span is expressible as a callback wrapping the span, so the callback
  is strictly more general.

- **Blend in premultiplied linear working floats, coverage folded into effective
  source alpha.** `out = color·a + dst·(1 − color[3]·a)`, `a = coverage·` (the
  sampler's value). This is standard premultiplied source-over; scaling a
  premultiplied colour by the scalar `a` scales RGB and alpha together, which is
  exactly a coverage-attenuated source. No unpremultiply is needed because the
  tile format is already premultiplied (doc 07 rule 2). *Alternative rejected:*
  straight-alpha compositing with an unpremultiply/premultiply round-trip — doc
  07 rule 2 makes the working space premultiplied; a round-trip would add divide
  error and contradict the format for zero benefit.

- **Opacity is the dab generator's business, not the seam's.** The coverage
  sampler returns *final* per-pixel coverage in `[0,1]`; brush opacity, hardness,
  and radius are parameters of the reference round-dab generator that folds them
  into the sampler's output. The seam stays minimal and fully general (an
  arbitrary alpha mask is just another sampler). *Alternative rejected:* a
  separate `opacity` scalar on `paint` — redundant with the sampler's `[0,1]`
  range, and it pushes brush semantics into the store that belong to the brush.

- **The preserved special case is opaque + full-coverage; translucent flat fill
  now blends.** The retained flat overload routes through the same source-over
  blend. For an opaque colour at coverage 1 the result is byte-identical to
  today's REPLACE (the load-bearing preservation). A *translucent* flat colour
  now source-over blends against the destination instead of replacing it — the
  correct brush semantics, and no shipped golden or test exercises a translucent
  flat fill, so nothing regresses. The task note scopes the preservation
  explicitly to "a full-coverage opaque dab", so this is intended.
  *Alternative rejected:* branching the overload to keep literal REPLACE for
  translucent flat colours — it would preserve a semantics nothing wants
  (paint-that-erases-alpha) and fork the write path.

- **The reference round-dab falloff is a `libm`-free polynomial over squared
  distance.** Coverage is driven by normalized squared radial distance remapped
  by a fixed-order polynomial between the inner hard radius and outer radius,
  scaled by opacity, clamped to `[0,1]` — no `sqrtf`, no `exp`, byte-exact and
  portable, matching the resampling kernels' `libm`-free discipline (doc
  07:41-42). *Alternative rejected:* a Gaussian / `exp`-based falloff — `exp`
  is not correctly-rounded across platforms, so it cannot carry a byte-exact
  golden (doc 16); the polynomial gives an equally acceptable soft edge and is
  deterministic. The exact falloff shape is an implementation choice pinned by
  golden, not a designed invariant.

- **Reference dab generator lives in `kind_raster`, not `media`.** It has one
  call site and is an editing tool, not a shared format/kernel primitive.
  *Alternative rejected:* placing it in `arbc::media` (L1) beside the resampling
  filters — no second consumer, and doc 17:61 scopes `media` to format-agnostic
  descriptors and resampling kernels over working samples, not brush shapes.

- **Doc-14 delta (same-commit, doc 16:15-25).** Append a normative paragraph to
  the paint-discipline block after `docs/design/14-data-model-and-editing.md:211`
  stating that the dab's per-pixel write is a coverage-masked source-over
  composite in premultiplied linear working floats (citing doc 07 rules 2-3 as
  the blend-space authority, and doc 01:29 / doc 02:95-100 as the source-over-only
  / non-idempotence anchors), spelling out `out = color·a + dst·(1 − color[3]·a)`,
  and noting that a fully-opaque full-coverage dab reduces to a replace as the
  special case. Doc 14 today describes the paint's CoW/capture/coalesce/mip/damage
  discipline but is silent on what the mutation writes; this delta closes that
  gap. **No doc-07 delta:** rule 2 ("compositing … on premultiplied alpha") and
  rule 3 ("blending … in a nonlinear space is mathematically wrong") already make
  the blend space normative — the source-over algebra is *implied* there and
  *stated* in the doc-14 paint block where paint's semantics live, which is the
  right home. This is not a project-shaping decision, so no doc 00 decision-record
  bullet is required (the blend op scope was already decided: doc 01:29 v1
  source-over only).

## Open questions

(none — all decided.) The one item deliberately *not* encoded as a WBS decision:
whether v0.1's "simple brushes" want per-stroke coverage accumulation
(opacity-vs-flow) in the release or as post-v0.1 polish. The defensible call is
polish → M10 (`kinds.raster_stroke_coverage_buffer`, above), since v0.1 scopes
"simple brushes" and a single-dab source-over is a complete, correct brush; the
milestone assignment is surfaced to the closer in the return summary.

## Status

**Done** — 2026-07-14.

- Added `CoverageSampler` alias and `round_dab` declaration to
  `src/kind_raster/arbc/kind_raster/raster_content.hpp`; added
  coverage-taking overloads of `RasterStore::paint` and
  `RasterContent::paint`.
- Implemented `blend_over` (premultiplied-linear source-over, non-negative
  clamp, libm-free), coverage-taking `paint` core, and constant-`1.0f`
  forwarding overloads in `src/kind_raster/raster_content.cpp`; implemented
  `round_dab` polynomial falloff.
- Extended `docs/design/14-data-model-and-editing.md` with a normative
  dab paragraph (coverage-masked source-over) after line 211.
- Added claim row `14-data-model-and-editing#raster-dab-is-coverage-masked-source-over`
  to `tests/claims/registry.tsv`.
- Added byte-exact goldens (soft round dab L0/L1/L2, partial-opacity dab,
  alpha-mask dab) to `src/kind_raster/t/raster_goldens.t.cpp` and an
  `enforces:`-tagged claim test to `src/kind_raster/t/raster_paint.t.cpp`.
- All 24 kind_raster tests + 383 conformance assertions pass; claims and
  levelization checkers green; full CI replay (gcc/clang × debug/release/
  asan/tsan/rtsan, coverage + diff-coverage gate) green.
- Tech-debt follow-up registered: `kinds.raster_stroke_coverage_buffer`
  (M10) — per-stroke coverage accumulation (max/union mask) so overlapping
  dabs within one stroke composite once at gesture end.
