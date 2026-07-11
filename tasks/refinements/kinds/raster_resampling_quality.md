# Refinement — kinds.raster_resampling_quality

## TaskJuggler entry

[`tasks/55-kinds.tji:20-25`](../../55-kinds.tji) —
`task raster_resampling_quality "org.arbc.raster resampling quality"`:

> Replace baseline box-downsample/bilinear resampling with higher-order
> filters (e.g. Lanczos-3 downsample, bicubic upsample), each pinned by
> byte-exact per-filter goldens; keeps the mip-generation seam, swaps the
> kernel. Docs 03/14. Source-of-debt:
> `tasks/refinements/kinds/raster.md`

Milestone: `m9_release` (`tasks/99-milestones.tji:71`).

## Effort estimate

`effort 2d`, `allocate team`.

This is a **kernel swap behind a landed seam**, not new machinery.
`kinds.raster` (3d) already shipped the pyramid, the rung selection, the
CoW paint path, the honesty math, and the frozen golden harness; it
deliberately left the filters as a documented baseline and named this task
as the quality follow-up (`tasks/refinements/kinds/raster.md:328-332`).
`kinds.raster_pool_backing` then moved tile pixels onto `BigBlockPool`
without touching a single float (`raster_pool_backing.md:364-371`).

The 2d covers: one new `arbc::media` header carrying the filter weights and
tap combiners; rewiring the three call sites that own the two kernels;
correcting the **paint-time mip propagation footprint** for the wider
kernel support (the one genuine correctness hazard here — see Constraint 4);
regenerating exactly four frozen golden tables; a second, larger multi-tile
golden fixture; two claims-register entries; and a direct kernel unit test.
The tile-table structure, rung selection, pool allocation, `achieved_scale`
/`exact` honesty math, and the whole `Editable` surface are untouched.

## Inherited dependencies

**Settled predecessor this task builds on (`complete 100`):**

- **`kinds.raster`** (`tasks/refinements/kinds/raster.md`, Done 2026-07-06)
  — shipped the mip/tile pyramid, `level_for_scale` rung selection, the CoW
  tile table, the render path, and the frozen golden suite. Its Decision
  *"Box-filter mip downsample is simple, deterministic, and byte-exact — a
  correct baseline; higher-order filters are deferred, not required for the
  reference proof"* (`raster.md:341-386`) is precisely the debt this task
  pays.
- **`kinds.raster_pool_backing`** (Done 2026-07-10) — **this is the state
  you inherit, not `raster.md`'s.** Tile pixels are `BlockSlotRef`s in a
  `BigBlockPool`, *not* `shared_ptr<TileBlob>`. Mip blobs are allocated via
  `new_blob(pool, edge)` (`raster_content.cpp:80-84`) and taps are read via
  `level_pixel(child, pool, edge, x, y)` (`:60-74`). Any kernel work must go
  through those accessors.
- **`kinds.raster_runtime_binding`** (Done 2026-07-10) — irrelevant to the
  pixels, but it removed the bespoke sink classes from
  `raster_content.hpp`; do not resurrect them.

**Structural precedent (settled; mirrored for shape — not a dependency):**

- **`kinds.nested_audio_resampling`**
  (`tasks/refinements/kinds/nested_audio_resampling.md`, Done 2026-07-07) —
  **the audio analog of this task**, and its closest template. It (a) put a
  deterministic, table-driven resampler in `arbc::media` rather than
  privately in the L4 kind, explicitly so a second consumer below an
  un-crossable level boundary could reuse it (`:420-443` — and it paid off:
  `audio.mix_engine` now reuses `resample_audio` at
  `tests/claims/registry.tsv:186-191`); (b) chose a convention that
  **collapses byte-for-byte to the incumbent baseline at the trivial case**,
  so pre-existing goldens survived untouched; (c) added exactly one
  claims-register row per genuinely new normative surface. **This
  refinement follows all three moves.**
- **`color.resampling`** (`tasks/refinements/color/resampling.md`, Done
  2026-07-05) — shipped `downsample_box_kernel` + the bilinear `fetch_texel`
  tap into `src/backend_cpu/kernels.hpp`, i.e. the *compositor-side* copy of
  exactly the same two baseline filters this task replaces on the
  *kind* side. It is **not** a dependency, but it is the second consumer
  that motivates the placement decision below, and it is the named
  follow-up (`color.resample_filter_quality`).

**Pending (must not be assumed at implementation time):**

- Nothing. Both kernels, both seams, the pool backing, and the golden
  harness are all landed and green at HEAD. This task has no pending
  inbound edge — it is unblocked the moment it is picked.

## What this task is

`org.arbc.raster` currently resamples with two placeholder kernels: mip
rungs are built by a **2×2 box average** (`raster_content.cpp:152-165`,
duplicated verbatim in the paint path at `:392-405`), and every rendered
pixel is fetched with a **4-tap bilinear** (`bilerp`, `:203-212`, called at
`:535-536`). Box decimation aliases (it is a poor low-pass: a half-band box
has enormous stopband leakage, so a downscaled detailed image shimmers and
moirés), and bilinear magnification is visibly soft. This task replaces both
with higher-order filters — a **6-tap Lanczos-3 half-band decimation** for
the pyramid and a **Catmull–Rom bicubic** tap for the sampler — implemented
as a format-agnostic, table-driven, byte-exact filter bank in `arbc::media`,
and pins each with byte-exact goldens.

**Scope:** the two kernels and the one structural consequence of widening
them (the paint-time mip propagation footprint, Constraint 4). Explicitly
**out of scope**: the tile-table/pool/CoW machinery, `level_for_scale`'s
rung ladder, the `achieved_scale`/`exact` honesty math
(`raster_content.cpp:511-513`), the `Editable` surface, `RenderRequest`'s
shape (**no filter-selection knob is added — see Decision 3**), and
`backend_cpu`'s own compositing kernels (deferred to
`color.resample_filter_quality`).

## Why it needs to be done

Raster is the reference kind for bounded scale (doc 03:217): it is the one
content whose whole purpose is to demonstrate honest degradation across a
mip pyramid, and it is the kind an embedder's photo/paint layer actually
runs on. Shipping it on a box filter means every zoomed-out raster layer in
the product aliases, and every zoomed-in one is soft — the two most visible
image-quality defects a compositor can have, on the most-used kind. Doc
04:95-99 ("the ladder is chosen so tiles are *downsampled* … since
minification looks better than magnification") makes the pyramid's
decimation quality the *load-bearing* one: the ladder deliberately routes as
much work as possible through it.

Downstream, the filter bank this task lands in `media` is the shared floor
that `backend_cpu`'s compositing kernels need for the same upgrade to the
≤1-octave remainder resample (doc 02:68) — the named follow-up
`color.resample_filter_quality`. Landing the bank here, once, is what stops
that task from duplicating a frozen coefficient table across a level
boundary it cannot cross.

## Inputs / context

### Governing design-doc sections (normative, doc 16)

The task note cites docs 03/14. **Both are thinner than they look, and the
gap is itself a finding**: the design corpus is *normatively silent* on
filter choice — "Lanczos", "bicubic", "bilinear", and "box" appear **nowhere**
in `docs/design/`. What the docs *do* bind:

- **`docs/design/03-layer-plugin-interface.md:145-148`** — the
  degradation-honesty clause: *"A content implementation that cannot honor
  exactness at the requested scale reports `achieved_scale`/`exact=false`
  honestly."* This is what the render path already satisfies at
  `raster_content.cpp:511-513` and what this task must not perturb.
- **`docs/design/03-layer-plugin-interface.md:217`** — the entire normative
  statement about raster's pyramid: *"Exercises finite bounds, bounded scale
  range, tiling/mip pyramid, `achieved_scale < requested`."* The pyramid is
  the kind's; its filter is unspecified.
- **`docs/design/01-core-concepts.md:71, :117-131`** — the resolution model;
  raster is "up to native", "tiles + mip pyramid from source".
- **`docs/design/04-transforms-and-infinite-zoom.md:95-99`** — the ladder is
  biased toward downsampling *"since minification looks better than
  magnification"*, and the compositor applies the ≤1-octave remainder. This
  is the doc that makes decimation quality the priority.
- **`docs/design/07-color-and-pixel-formats.md:25-31`** — resampling happens
  in premultiplied linear-light working floats, never in a nonlinear space.
  Already satisfied (both kernels operate on `WorkingPixel`); the new
  kernels inherit it, and it constrains the alpha handling in Constraint 3.
- **`docs/design/14-data-model-and-editing.md:175-178, :196`** — raster's
  state is a persistent CoW tile table; *"rendering a pure function of
  (state, region, scale, time)"*. Doc 14 says nothing about resampling; its
  bearing on this task is the **paint path**, whose incremental mip
  recompute must stay consistent with a full rebuild (Constraint 4).
- **`docs/design/16-sdlc-and-quality.md:48-53`** — the golden recipe:
  *"fixed FP flags, no FMA variance in kernels, ordered reductions, so
  goldens are byte-exact … Perceptual-tolerance comparison exists only where
  platform libm variance is unavoidable — tolerances are the exception."*
  **A Lanczos kernel evaluated with runtime `std::sin` would land squarely in
  that carve-out and force a tolerance. Tabulation is what keeps this task
  byte-exact** (Constraint 2).
- **`docs/design/16-sdlc-and-quality.md:15-25`** — the claims register and
  the same-commit doc-delta rule.
- **`docs/design/17-internal-components.md:46-64, :81-98`** — the
  levelization table and the "format-templated kernels are not `media`"
  note. **This task amends that note — see Decision 2 and the delta below.**

### Design-doc deltas

**Landed with this refinement** (they are placement *policy*, true the
moment the decision is made, and they name no unshipped file):

- **`docs/design/17-internal-components.md:81-98`** — narrows *"Kernels are
  not `media`"* to *"**Format-templated** kernels are not `media`"*, and
  states the mechanical split: needs `PixelTraits<F>` or a `Surface` ⇒
  `backend-cpu`; arithmetic on decoded working values ⇒ `media`. The L4
  kinds can reach `media` (transitively via `contract`) but never
  `backend-cpu`, so `media` is the only floor on which `kind-raster` and
  `backend-cpu` can share one filter. The `media` row's Contents cell
  (`:50`) gains the resampling-filter entry.
- **`docs/design/00-overview.md:159-168`** — a decision-record bullet
  ("Where a kernel lives"), since this placement rule governs every future
  kernel, not just this one.

**Landing with the implementation commit** (doc 16:15-25's same-commit rule
attaches a *behavior* delta to the *behavior* change; these describe pixels
that do not exist yet, so they must not land ahead of them). The implementer
writes both, verbatim in substance:

- **`docs/design/07-color-and-pixel-formats.md`**, appended to the working-space
  rules (after `:31`) — the project's resampling-filter policy, which is
  currently unwritten anywhere:

  > **Resampling filters.** Minification uses a windowed-sinc low-pass
  > (Lanczos-3); magnification uses an interpolating cubic (Catmull–Rom).
  > Both operate on decoded premultiplied linear working values (rule 3),
  > with tabulated float32 weights, a fixed tap order, and no runtime
  > `libm` — so every resample is byte-exact and portable (doc 16). Both
  > kernels' negative lobes may ring below zero; the result is clamped to
  > non-negative per channel before use, which removes the unphysical
  > undershoot (and the negative alpha that would break unpremultiplication)
  > while leaving the float working space's HDR headroom above alpha intact.
  > The filters live in `arbc::media` (doc 17) and are shared by the kinds'
  > mip pyramids and the backend's compositing kernels.

- **`docs/design/14-data-model-and-editing.md`**, appended near `:175-178` —
  the pyramid-consistency invariant that the widened kernel makes non-trivial:

  > A paint transaction's incremental mip recompute is **indistinguishable
  > from a full rebuild**: because a decimation kernel's support is wider
  > than the 2×1 pixels it reduces, the propagated region is dilated by the
  > kernel's radius at each rung, so no stale filtered pixel survives near a
  > painted tile's boundary. Copy-on-write economy applies to level 0's
  > touched tiles (only those are copied); the rungs above necessarily touch
  > a slightly wider set.

### Real source seams (paths + lines, at HEAD `d40263f`)

**The two kernels this task replaces — three call sites, not two:**

1. **Pyramid build** — `src/kind_raster/raster_content.cpp:135-174`,
   `build_levels`'s mip loop. The kernel proper is `:152-165`: four
   `level_pixel` taps at `2*cpx{,+1}`/`2*cpy{,+1}` with `std::min(...)`
   edge-clamping, then
   `avg{(a[k]+b[k]+c[k]+d[k]) * 0.25F}`, then `put(px, edge, ix, iy, avg)`.
2. **Paint-time mip recompute** — `src/kind_raster/raster_content.cpp:370-413`,
   inside `RasterStore::paint`. **The identical box kernel is duplicated
   verbatim at `:392-405`** (only `pool` → `d_pool`). There is no shared
   helper today — a kernel swap that misses this copy leaves the pyramid
   inconsistent between built and painted states. The propagation rect is
   computed at `:374-375`
   (`parent_rect{propagate.x0 * 0.5, …, propagate.y1 * 0.5}`) and advanced at
   `:412` (`propagate = parent_rect`) — **this is what Constraint 4 corrects.**
3. **Render sampler** — `src/kind_raster/raster_content.cpp:203-212`
   (`bilerp`), called at `:535-536` from the `visit_surface` lambda
   (`:516-540`). The surrounding machinery is the seam and stays: the
   dest→source map at `:522-523`, the level-space rescale
   `u = lx / ls - 0.5` at `:528-529`, the transparent-black out-of-bounds
   guard at `:525`, and `Traits::encode(sample, &typed.data[i])` at `:538`.

**Supporting surfaces:**

- `src/kind_raster/raster_content.cpp:60-74` — `level_pixel(level, pool,
  edge, x, y)`: the pool-backed tap fetch. Its callers are the kernels; note
  `TileTable::pixel` (`:218-223`) **clamps** x/y to the level bounds, so taps
  outside a level see clamp-to-edge, not a zero border (unlike
  `backend_cpu`'s `fetch_texel`). Preserve that convention (Constraint 3).
- `src/kind_raster/raster_content.cpp:190-201` — `level_for_scale`. Untouched.
- `src/kind_raster/raster_content.cpp:511-513` — the honesty math. Untouched.
- `src/kind_raster/arbc/kind_raster/raster_content.hpp:4` — already includes
  `<arbc/media/pixel_traits.hpp>`; `k_tile_channels = 4` at `:38`;
  `WorkingPixel = std::array<float,4>` is `src/media/arbc/media/pixel_traits.hpp:17`.
- `src/media/CMakeLists.txt` — the component to extend (`DEPENDS base`);
  `audio_resampler.hpp`/`.cpp` are the shape to copy, including the frozen
  coefficient banner and REGENERATE PROCEDURE at `src/media/audio_resampler.cpp:15-41`.
- `src/backend_cpu/kernels.hpp:39-49` (`fetch_texel`), `:65-111`
  (`source_over_kernel`'s bilinear tap), `:121-145` (`downsample_box_kernel`)
  — the *second* consumer. **Not touched by this task**; it is the
  motivation for the `media` placement and the body of the follow-up.
- `scripts/check_levels.py:17-41` — the `ALLOWED` map. `media: {"base"}`,
  `kind_raster: {"contract"}`. Include-checking validates against the
  **transitive closure** (`:51-60`, `:93-103`), and `contract` pulls in
  `media` — so `kind_raster` including `<arbc/media/…>` is already legal and
  **no edit to `check_levels.py` or any `CMakeLists.txt` DEPENDS line is
  required.** (`backend_cpu` already declares `media` directly.)

**Test surfaces:**

- `src/kind_raster/t/raster_goldens.t.cpp` — `k_edge = 4` (`:72`), the 4×4
  input fixture `kInput` (`:65-70`), nine frozen tables (`:127-222`), four
  `TEST_CASE`s (`:227`, `:235`, `:252`, `:261`), and the hidden
  `[.regen]` dumper (`:283-294`, `GCOV_EXCL`-wrapped).
- `src/media/t/audio_resampler.t.cpp:184` — the `[.regen]` convention. **Note
  the known gap** (`raster_pool_backing`/audio Explore finding): the audio
  coefficient *table* has no live regeneration path — its `[.regen]` case
  dumps only the output golden. This task does better (Constraint 2).
- `tests/raster_conformance.t.cpp`, `src/kind_raster/t/raster_paint.t.cpp`,
  `tests/raster_concurrency_stress.t.cpp`, `tests/pull_multitile_golden.t.cpp`
  — the regression envelope.
- `tests/claims/registry.tsv` — `:39`
  (`07-color-and-pixel-formats#resampling-in-linear-premultiplied-space`, the
  row whose "box 2:1 rung downsample" language is enforced by *backend_cpu's*
  goldens, not raster's), `:136`
  (`14-data-model-and-editing#raster-paint-copies-only-touched-tiles`), `:77`
  (the audio analog's row, for claim-writing style).

## Constraints / requirements

1. **Keep the seams; swap only the kernels.** The tile-table structure, the
   pool-backed blob allocation, `level_for_scale`'s rung ladder, the
   dest→source mapping, the out-of-bounds guard, `PixelTraits::encode`, and
   the `achieved`/`exact` honesty math (`raster_content.cpp:511-513`) are
   **not** modified in behavior. This is the "keeps the mip-generation seam,
   swaps the kernel" the task note prescribes and the precedent
   (`nested_audio_resampling.md:366-377`) established.

2. **Byte-exact, no runtime `libm`, no tolerance** (doc 16:48-53). The
   Lanczos-3 weights are **tabulated as `constexpr` float32 constants**,
   generated once by an audited procedure and frozen with a banner + a
   REGENERATE PROCEDURE comment, exactly as
   `src/media/audio_resampler.cpp:15-41` does. Runtime does only ordered,
   fixed-tap-order float32 multiply-accumulates. Catmull–Rom needs **no
   table** — its basis is an exact rational cubic (`-0.5, 1.5, 2.0, …`)
   evaluated in closed form, which is why it is the chosen magnifier
   (Decision 3). **Improving on the audio precedent's known gap: the
   `[.regen]` case must dump the coefficient table itself, not only the
   output goldens**, so the frozen bank has a live in-repo regeneration path.

3. **Ringing is clamped to non-negative, and only that.** Both kernels have
   negative lobes, so a hard edge produces undershoot. Negative alpha is
   unrepresentable and would break unpremultiplication in
   `PixelTraits<Rgba8Srgb>::encode`; negative light is unphysical. Clamp each
   of the four channels at `std::max(0.0F, v)` after the tap reduction —
   deterministic, byte-exact, one line. **Do not clamp RGB to ≤ alpha**: in a
   float linear-premultiplied working space, values above alpha are
   legitimate HDR headroom (doc 07:25-31) and each format's `encode` already
   clamps on the way to storage. Preserve the **clamp-to-edge** tap
   convention that `TileTable::pixel` (`:218-223`) already provides — do not
   switch to `backend_cpu`'s zero border, which would darken level borders.

4. **The paint-time mip propagation footprint must dilate by the kernel
   radius.** This is the one real correctness hazard. Today's box kernel
   reduces child pixels `2x, 2x+1` into parent `x`, so `parent_rect =
   touched * 0.5` (`raster_content.cpp:374-375`) is an exact footprint. A
   6-tap Lanczos-3 half-band reads child pixels `2x-2 … 2x+3`, so parent `x`
   is dirtied by any child `cx` with `2x-2 ≤ cx ≤ 2x+3` — i.e. the parent
   rect must be **dilated outward by the filter radius** before selecting
   which parent tiles to recompute (conservatively:
   `{(p.x0 - 3) * 0.5, …, (p.x1 + 3) * 0.5}`, floor/ceil to whole pixels).
   **Left uncorrected, a paint leaves stale filtered mip pixels in a band
   around the touched region — a silent, zoom-dependent corruption that no
   existing test would catch.** Pinned by a claim and a full-rebuild
   equivalence test (Acceptance).
   Note the consequence for `RasterStore::blobs_allocated`: the *rungs above*
   level 0 now CoW a slightly wider tile set. The claim
   `14-data-model-and-editing#raster-paint-copies-only-touched-tiles`
   (`registry.tsv:136`) is about **level 0**, which is unchanged — but any
   behavioral-counter assertion in `src/kind_raster/t/raster_paint.t.cpp`
   that pins a total blob count across all levels **will move, legitimately**;
   update it and say why in the commit.

5. **De-duplicate the box kernel as you replace it.** The kernel body is
   currently copy-pasted between `build_levels` (`:152-165`) and
   `RasterStore::paint` (`:392-405`). The replacement lands **once** (in
   `media`, invoked through one `kind_raster`-local helper that both call
   sites share). Two divergent copies of a byte-exact filter is exactly the
   defect this task exists to not create.

6. **`media` placement, no new dependency edges** (doc 17 delta, Decision 2).
   The filter bank is header-mostly in `src/media/arbc/media/image_resampler.hpp`
   (+ `image_resampler.cpp` for the frozen table), templated on a caller-supplied
   `Fetch` callable returning `WorkingPixel` — **it must not name
   `PixelFormat`, `PixelTraits<F>`, `Surface`, or `BigBlockPool`** (`media`
   depends only on `base`; `pool` is a *sibling* L1 and is unreachable). The
   tiled, pool-backed tap fetch stays in `kind_raster` and is passed in as the
   callable. `scripts/check_levels.py` and every `CMakeLists.txt` `DEPENDS`
   line stay **unchanged** — verify, don't edit.

7. **Fixed, documented filter characteristics — no runtime quality knob.**
   One decimation filter, one magnification filter, both compiled in. **No
   field is added to `RenderRequest`, no ctor parameter to `RasterContent`, no
   `FilterKind` enum** (doc 03 defines none; doc 16 favors one deterministic
   path; `nested_audio_resampling.md:270-272` settled the identical question
   for audio). The tap count, window, and phase convention are recorded beside
   the coefficient table and referenced from the golden regeneration procedure.

8. **`render_thread_safe() == true` survives.** Render stays a pure read of an
   immutable pinned tile table; the new sampler adds no shared mutable state.
   Paint's wider propagation stays under the existing `d_mutex`.

## Acceptance criteria

### The exact golden-movement prediction (the primary correctness gate)

Catmull–Rom is an *interpolating* spline: at integer phase its basis is
exactly `(0, 1, 0, 0)` in IEEE float, so it reproduces the source sample
bit-for-bit — as bilinear does. Every existing render golden samples at
integer phase (at `achieved = 1.0`: `u = lx - 0.5 = dx`; at `achieved = 0.5`
on level 1: `u = (2dx+1)/2 - 0.5 = dx`). Therefore the magnifier swap moves
**zero** existing bytes, and only the decimation swap moves any. The
implementer must confirm exactly this split in
`src/kind_raster/t/raster_goldens.t.cpp`:

| Frozen table | Expected |
| --- | --- |
| `kMip0` (`:130`) | **unchanged** — level 0 is the decoded buffer, never filtered |
| `kNative32` (`:157`), `kNative16` (`:204`), `kNative8` (`:214`) | **unchanged** — integer phase; the collapse-to-baseline guard |
| `kClamp32` (`:185`) | **unchanged** — BestEffort past native clamps to `achieved = 1.0`, integer phase |
| `kMip1` (`:149`), `kMip2` (`:155`) | **regenerated** — the decimation kernel changed |
| `kHalf32` (`:176`), `kQuarter32` (`:182`) | **regenerated** — they sample the changed rungs at integer phase |

**Any other golden movement means the magnifier is not collapsing at integer
phase and the implementation is wrong.** This is the `color.resampling` move
(b) that `nested_audio_resampling.md:57-69` names, made checkable.

### New golden coverage (the filters must actually be exercised)

The 4×4 / `k_edge = 4` fixture is almost all edge-clamp: it cannot show a
6-tap kernel's interior behavior, and no existing case samples a fractional
phase. Add, in `src/kind_raster/t/raster_goldens.t.cpp`:

- **A second, larger fixture** — a 16×16 synthetic pattern at `k_edge = 8`
  (so level 0 is 2×2 tiles), with a high-frequency component (a 1-px
  checkerboard or chirp) that a box filter aliases and Lanczos-3 does not.
  Freeze its per-rung mip bytes. This exercises **interior taps and
  cross-tile tap reads**, both of which the 4×4 fixture cannot.
- **A fractional-phase magnification golden** — an `Exact` render at
  `scale = 2.0` (achieved 2.0, exact; phases 0.25/0.75), and one at a
  non-power-of-two scale (e.g. `0.75`), where Catmull–Rom and bilinear
  genuinely differ. These are the only cases that pin the magnifier at all.
- **A hard-alpha-edge case** — a fixture with an opaque/transparent step, to
  force ringing, asserting the frozen bytes and (via a direct assertion on
  the `WorkingPixel` output) that **no channel is negative** (Constraint 3).
- **A discriminator assertion** — the new mip bytes are **not equal** to the
  box-filtered bytes for the high-frequency fixture. A golden alone cannot
  distinguish "Lanczos landed" from "someone re-froze the box output"; this
  assertion can, and it is what makes the claim below honest.

### Direct kernel unit test

`src/media/t/image_resampler.t.cpp` — the kernel's own anchor, independent of
the render path (mirroring `src/media/t/audio_resampler.t.cpp`):
identity/integer-phase reproduction (Catmull–Rom at `t = 0` returns the
sample bit-exactly); **not-a-bilinear** and **not-a-box** discriminators;
weight-sum normalization (the frozen Lanczos-3 6-tap sums to exactly `1.0F`
in float32); determinism (same input → same bytes, twice); clamp-to-edge tap
behavior; the non-negative clamp on a ringing input; and a byte-exact frozen
golden with a `[.regen]` dumper **that also dumps the coefficient table**
(Constraint 2). Wire into `src/media/CMakeLists.txt`'s `arbc_component_test`.

### Claims-register entries (two new rows in `tests/claims/registry.tsv`)

1. **`07-color-and-pixel-formats#resampling-uses-higher-order-filters`** —
   *"Minification through a mip pyramid uses a fixed 6-tap Lanczos-3
   half-band decimation and magnification uses an interpolating Catmull–Rom
   bicubic tap, both over decoded premultiplied linear working floats —
   decisively not the 2×2 box average and 4-tap bilinear they replace: a
   high-frequency input decimates to different bytes than a box filter
   produces, and a fractional-phase magnification differs from a bilinear
   tap, while at integer phase the interpolating cubic reproduces the source
   sample bit-exactly. Weights are tabulated float32 with a fixed tap order
   and no runtime libm, so every rung and every fractional phase is a
   byte-exact deterministic function of the decoded buffer; negative-lobe
   undershoot is clamped to non-negative per channel."*
   Enforced by the discriminator assertions + the new goldens above.
   (The "decisively not X" construction is the house style —
   `registry.tsv:39`, `:77` — and it is what stops a degenerate
   implementation from satisfying the claim.)

2. **`14-data-model-and-editing#paint-mip-recompute-matches-full-rebuild`** —
   *"After `org.arbc.raster::paint`, every mip rung of the resulting tile
   table is byte-identical to a full rebuild of the pyramid from the painted
   level-0 pixels: the incremental recompute dilates the propagated region by
   the decimation kernel's support at each rung, so no stale filtered pixel
   survives near a painted tile's boundary."*
   Enforced by a new case in `src/kind_raster/t/raster_paint.t.cpp`: paint a
   region deliberately straddling a tile boundary (and one landing a few
   pixels *inside* one, so the dilation crosses into an untouched tile), then
   assert every level's `level_pixels(l)` equals a freshly-built
   `RasterStore` over the painted level-0 bytes, **byte for byte**. This test
   fails on today's `parent_rect = touched * 0.5` the moment the kernel
   widens — it is the guard for Constraint 4.

### Re-asserted (a second `enforces:` test each, no new registry row)

- `03-layer-plugin-interface#render-scale-honest` — the honesty math is
  untouched, and the new goldens re-assert `achieved_scale`/`exact` on every
  render case (including the `Exact` past-native magnification, where the
  bicubic makes the "faithful at `achieved_scale`" self-report more
  defensible than bilinear did).
- `14-data-model-and-editing#raster-paint-copies-only-touched-tiles` — still
  true at level 0; the paint test re-asserts it alongside the new
  full-rebuild equivalence (Constraint 4).
- `16-sdlc-and-quality#byte-exact-goldens` — all new goldens carry the tag.

### Regression envelope (must stay green, unmodified)

`tests/raster_conformance.t.cpp` (the kind's contract conformance run — scale
honesty and stability properties), `tests/raster_concurrency_stress.t.cpp`
(**TSan**: render is still a pure read of an immutable table; paint's wider
propagation stays under `d_mutex` — Constraint 8),
`src/kind_raster/t/raster_pool_backing.t.cpp`, and
`src/backend_cpu/t/kernel_goldens.t.cpp` (untouched — `backend_cpu`'s kernels
are out of scope). **`tests/pull_multitile_golden.t.cpp` may legitimately
move** if it goldens raster pixels off a mip rung; the implementer checks,
regenerates if so, and states why in the commit message.

### CI gates

≥90% diff coverage on changed lines; `scripts/check_levels.py` green **with
no edit to its `ALLOWED` map or to any `DEPENDS` line** (Constraint 6);
`scripts/check_claims.py` green in both directions (both new claims
registered *and* enforced; re-asserted claims still enforced);
`tj3 project.tjp` silent.

### Deferred follow-up (closer registers in WBS)

- **`color.resample_filter_quality`** (~2d, area `color`, `tasks/15-color.tji`,
  milestone **m9_release**) — *"Swap `backend_cpu`'s bilinear composite tap
  (`kernels.hpp:65-111`) and `downsample_box_kernel` (`:121-145`) onto the
  `arbc::media` filter bank landed by `kinds.raster_resampling_quality`, so
  the compositor's ≤1-octave remainder resample (doc 02:68, 04:95-99) matches
  the kinds' pyramid quality; per-format byte-exact goldens in
  `src/backend_cpu/t/kernel_goldens.t.cpp`; re-assert claim
  `07-color-and-pixel-formats#resampling-in-linear-premultiplied-space`
  (`registry.tsv:39`), whose 'box 2:1 rung downsample' wording this
  supersedes."* `depends kinds.raster_resampling_quality, color.resampling`.
  Source-of-debt: this refinement. **This is the second consumer that
  justifies the `media` placement** (Decision 2) — it is concrete,
  agent-implementable, and deliberately *not* bundled here (it would
  contaminate this task's golden gate with backend-side golden churn, and
  `color.resampling`'s goldens are a separate regression surface).

## Decisions

- **Lanczos-3 (6-tap, fixed half-phase) for decimation.** At a 2:1 rung the
  destination center maps to source `u = 2x + 0.5` — **the fractional phase
  is constant at 0.5 for every output pixel**, so a windowed-sinc decimator
  collapses to a *single fixed 6-tap FIR* (taps at offsets ±0.5, ±1.5, ±2.5),
  not a polyphase bank. That is six frozen float32 constants: trivially
  tabulated, no runtime `libm`, no phase math, byte-exact. Lanczos-3 is the
  standard band-limited minifier and doc 04:95-99 makes decimation the
  quality-critical direction. *Alternative rejected:* a Mitchell–Netravali
  (B=C=1/3) decimator — softer and lobe-free, so no clamping needed, but it
  is a deliberately *blurring* filter and the defect being fixed is aliasing,
  not sharpness. *Alternative rejected:* a general polyphase bank like the
  audio resampler's — pure over-engineering here: the pyramid only ever
  decimates by exactly 2, so 31 of 32 phases would be dead weight and dead
  code.

- **Place the filter bank in `arbc::media` (L1), not privately in
  `kind_raster` (L4).** `kind_raster` **cannot** reach `backend_cpu`
  (`check_levels.py:17-41`: `kind_raster: {"contract"}`; `backend_cpu` is not
  in its closure), so it cannot reuse the box/bilinear kernels
  `color.resampling` already shipped — and `backend_cpu` equally cannot reach
  up into a kind. The two therefore *already* carry duplicate baseline
  filters, and a private higher-order kernel would duplicate a **frozen
  coefficient table** across an un-crossable boundary within weeks (the named
  `color.resample_filter_quality` follow-up is exactly that consumer).
  `media` is the only floor below both. This is the argument
  `nested_audio_resampling.md:420-443` made for `resample_audio`, and it paid
  off literally — `audio.mix_engine` now reuses that same kernel
  (`registry.tsv:186-191`). The bank is templated on a caller-supplied `Fetch`
  callable, so it names no `PixelFormat` and no `BigBlockPool`, and needs **no
  new dependency edge anywhere**. *Alternative rejected:* a
  `kind_raster`-private kernel — simplest for the single call site today, and
  it needs no doc delta, but it knowingly buys a duplicated byte-exact DSP
  table and a second golden regeneration path. The predecessor faced this
  exact fork and chose sharing; consistency plus the imminent second consumer
  settles it. *Alternative rejected:* hoisting the kernel into `backend_cpu`
  and letting the compositor pre-resample for raster — that inverts the
  design (doc 03:217 and 01:71 put the pyramid *in the kind*) and would strand
  the CoW paint path's mip recompute with no filter at all.

  **This required a design-doc delta**, because doc 17:81-83 read as a bright
  line ("Kernels are not `media`") that `media`'s own `audio_resampler` and
  `streaming_resampler` already crossed. Rather than quietly widen it a second
  time, the delta states the real, mechanical rule — *format-templated*
  storage kernels are `backend-cpu`; format-agnostic DSP over decoded working
  samples is `media` — and doc 00 records it, since it governs where every
  future kernel lands. See "Design-doc deltas" above.

- **Catmull–Rom bicubic (B=0, C=½) for magnification — and it needs no
  table.** Its basis is an exact rational cubic evaluated in closed form, so
  it is byte-exact with zero coefficients to freeze and zero `libm` calls.
  Decisively, it is **interpolating**: at integer phase its weights are
  exactly `(0, 1, 0, 0)` in IEEE float, so it reproduces the source sample
  bit-for-bit — which is precisely why every existing native-scale and
  on-rung golden survives the swap untouched (see the golden-movement table).
  That is the `color.resampling` "collapse to the baseline at the trivial
  case" move, and it makes the surviving goldens a *guard* on the new sampler
  rather than churn. *Alternative rejected:* Lanczos-3 for magnification too —
  symmetric and sharper, but it needs a real phase table, rings harder on the
  magnification path where ringing is most visible, and it is **not**
  interpolating, so it would move every existing golden and forfeit the
  collapse guard. *Alternative rejected:* Mitchell–Netravali (B=C=1/3), the
  usual "best general-purpose" cubic — it is *approximating*, not
  interpolating (it does not reproduce the source at integer phase), so a
  native-scale render would no longer return the source pixels, which would be
  a real regression in the exactness self-report at `achieved == native`.

- **No runtime filter-selection knob.** One decimator, one magnifier,
  compiled in. Doc 03's `RenderRequest` defines no quality field, doc 16
  favors one deterministic path, and the audio analog settled the identical
  question (`nested_audio_resampling.md:270-272`). *Alternative rejected:* a
  `FilterKind` enum on `RenderRequest` — a contract-wide change touching every
  kind, to serve zero present consumers, and it would multiply the golden
  matrix by the number of filters. (Read the task note's "each pinned by
  byte-exact **per-filter** goldens" as *one golden set per kernel* —
  decimation goldens and magnification goldens — not as goldens for N
  selectable filters; the audio analog resolved the same wording the same way.)

- **Clamp ringing to non-negative; do not clamp RGB to alpha.** Both filters
  undershoot at hard edges. Negative alpha would break unpremultiplication in
  `PixelTraits<Rgba8Srgb>::encode`, and negative light is unphysical — so a
  per-channel `std::max(0.0F, v)` after the reduction is required, and it is
  deterministic. But an RGB value *above* alpha is legitimate in a float
  linear-premultiplied working space (doc 07:25-31's HDR-capable working
  space), and each format's `encode` already clamps on the way to storage —
  so clamping RGB to ≤ alpha in the filter would destroy HDR headroom to fix
  a problem that does not exist. *Alternative rejected:* no clamp at all —
  cheapest, and fine in float, but it lets a negative alpha reach
  `encode`'s unpremultiply divide.

- **Correct the paint propagation footprint in this task, not a follow-up.**
  The widened kernel *creates* the stale-mip-band bug (Constraint 4); shipping
  the kernel without the dilation would knowingly land a silent, zoom-dependent
  corruption. It is a few lines and one equivalence test, and it is what the
  second claim pins. *Alternative rejected:* recomputing the full pyramid on
  every paint — correct and trivial, but it discards the CoW economy that
  `14:175-178` and `registry.tsv:136` promise, turning an O(touched) paint into
  O(image) at every stroke.

## Open questions

(none — all decided.)

Two non-blocking observations for the closer, surfaced here rather than
encoded as WBS work:

- **`media`'s audio coefficient table has no live regeneration path.**
  `src/media/audio_resampler.cpp:31-41` documents a REGENERATE PROCEDURE via
  the `[.regen]` case, but the only `[.regen]` case in `arbc_media_t` dumps
  the *output* golden (`t/audio_resampler.t.cpp:184`), not the 512-float bank.
  This task closes the gap for its own table (Constraint 2) and does not touch
  audio's. Worth a parking-lot line, not a task — the fix is a two-line dumper
  that whoever next opens that file should just make.
- **The design corpus never names a resampling filter** — "Lanczos",
  "bicubic", "bilinear", and "box" appear nowhere in `docs/design/`, and doc
  12:106-109 gestures at "a single raster resampler" as though one were
  already specified. The doc-07 delta this task lands (see "Design-doc
  deltas") gives that policy its first normative home.

## Status

**Done** — 2026-07-11.

- `src/media/arbc/media/image_resampler.hpp` and `src/media/image_resampler.cpp` created: table-driven Lanczos-3 half-band decimator (symmetric-pair fold for exact 1.0F DC gain) and Catmull–Rom bicubic magnifier templated on a caller-supplied `Fetch` callable, with frozen coefficient banner and REGENERATE PROCEDURE comment.
- `src/media/t/image_resampler.t.cpp` created: 8 unit cases (interpolating-at-integer-phase, not-a-bilinear at phase (0.25,0.25), not-a-box, symmetry + exactly-1.0F DC gain + Lanczos provenance, ringing/clamp, edge-convention, determinism) plus 2 output goldens (`kDecimated`/`kMagnified2x`) and a `[.regen]` that dumps the coefficient table itself (Constraint 2 gap closed).
- `src/media/CMakeLists.txt` extended to build `image_resampler.cpp` and wire the `arbc_component_test`.
- `src/kind_raster/raster_content.cpp` and `src/kind_raster/arbc/kind_raster/raster_content.hpp` updated: box/bilinear placeholders replaced by `arbc::media` Lanczos-3 decimator and Catmull–Rom magnifier; paint-time mip propagation dilated by kernel radius (Constraint 4, latent stale-band bug fixed).
- `src/kind_raster/t/raster_goldens.t.cpp` updated: 8 new raster goldens (`kHfMip1-4` 16×16 cross-tile fixture, `kStepMip1`/`kStepMag` hard-alpha-edge, `kMag2x`/`kMag075` fractional-phase), box-discriminator assertion, and non-negativity assertion. Golden-movement prediction held exactly: `kMip1`, `kMip2`, `kHalf32`, `kQuarter32` regenerated; `kMip0`, `kNative32`, `kNative16`, `kNative8`, `kClamp32` survived byte-for-byte (Catmull–Rom collapse at integer phase confirmed).
- `src/kind_raster/t/raster_paint.t.cpp` updated: paint→full-rebuild equivalence case (2 sections) confirmed real guard (verified by temporarily reverting dilation — full-rebuild memcmp fails, guard is non-vacuous).
- `docs/design/07-color-and-pixel-formats.md` and `docs/design/14-data-model-and-editing.md` updated with resampling-filter policy and pyramid-consistency invariant per same-commit rule (doc 16:15-25).
- `tests/claims/registry.tsv` extended: claims `07-color-and-pixel-formats#resampling-uses-higher-order-filters` and `14-data-model-and-editing#paint-mip-recompute-matches-full-rebuild` registered and enforced.
