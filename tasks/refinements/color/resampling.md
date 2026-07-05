# color.resampling — Filtered resampling

## TaskJuggler entry

`tasks/15-color.tji` → `color.resampling` ("Filtered resampling").

## Effort estimate

2d.

## Inherited dependencies

- `color.kernels` — settled (commit `86a6312`): provides the
  `source_over_kernel` whose **nearest** tap this task upgrades
  (`src/backend_cpu/kernels.hpp:34-69`), the `PixelTraits<F>`
  decode/encode codecs and `WorkingPixel` type
  (`src/media/arbc/media/pixel_traits.hpp`), the `TypedSpan<F>` +
  `visit_surface` one-dispatch-per-operation seam
  (`src/surface/arbc/surface/typed_span.hpp`), and the `Backend::composite`
  virtual (`src/surface/arbc/surface/backend.hpp:39-40`) alongside which the
  new downsample seam is declared.
- NOT inherited: `color.working_space`, `compositor.scale_ladder`. This
  task supplies the two resampling *tools* (a filtered composite tap and a
  rung-generation downsample); the *policy* that quantizes scale to rungs
  and chooses which rung to sample is `compositor.scale_ladder`
  (`tasks/35-compositor.tji:9`, which `depends … color.resampling`).

## What this task is

Replace the walking-skeleton **nearest-neighbor** sampling in the composite
pass with **bilinear** filtering, and add the **box-filtered 2:1
downsample** that generates coarser scale-ladder rungs — both operating on
decoded premultiplied linear working floats so resampling is
mathematically correct (doc 07 rule 3). The bilinear tap folds into the
existing `source_over_kernel` (one resample-and-blend pass); the box
reducer lands as a new `downsample_box_kernel<F>` exposed through a new
`Backend::downsample` virtual — the seam doc 09 already names as a
"backend-internal … resample operation consumed by the compositor" — so
`compositor.scale_ladder` has a ready capability to wire. No compositor
change lands here (bilinear is transparent to the composite call; the
downsample's caller is the later ladder task). The per-format byte-exact
golden matrix stays in `color.kernel_goldens`.

## Why it needs to be done

Doc 07 rule 3 ("Blending and **resampling** in a nonlinear space is
mathematically wrong … linear f16 is correct") is unclaimable while the
composite pass point-samples one texel: any non-integer scale or rotation
produces jaggies and, worse, would fringe if it ever interpolated in gamma
or straight-alpha space. Doc 04's scale-ladder scheme
(`docs/design/04-transforms-and-infinite-zoom.md:88-106`) is built on two
primitives this task provides: the sub-octave remainder "applied as
resampling during compositing" (the bilinear tap) and downsampled rungs
("the ladder is chosen so tiles are *downsampled* … since minification
looks better than magnification" — the box reducer). Every downstream
compositor task (`compositor.scale_ladder`, `compositor.tile_planning`,
`compositor.refinement`) and the M3 "Deep zoom" milestone build on both.

## Inputs / context

- `docs/design/04-transforms-and-infinite-zoom.md:88-106` — "Scale ladders
  and tile geometry": the sub-octave remainder is "applied as resampling
  during compositing" (`:95`), the prefer-downsample convention and
  "minification looks better than magnification" (`:95-98`), and "the
  composite pass applies the full affine" (`:103-106`). The ladder is
  powers of two (`:90`).
- `docs/design/07-color-and-pixel-formats.md` — rule 2 (`:19-24`,
  compositing/resampling in the working space on premultiplied alpha) and
  rule 3 (`:25-31`, resampling in nonlinear space is wrong; linear-light is
  correct); "Templates and variants" lists `resample` among the hot loops
  templated over format/transfer (`:68`).
- `docs/design/09-surfaces-and-backends.md:18` — "backend-internal
  composite/blit/**resample** operations consumed by the compositor" — and
  `:24-26` the Backend contract. The downsample virtual is a member of this
  already-named operation set (no doc delta — see Decisions).
- `docs/design/16-sdlc-and-quality.md` — byte-exact-golden determinism
  discipline the resample kernels inherit.
- `docs/design/17-internal-components.md:53,56,76-80` — the L3/L4 split:
  templated kernel bodies are `backend_cpu` (L3); the scale ladder / rung
  selection is `compositor` (L4).
- `src/backend_cpu/kernels.hpp:34-69` — `source_over_kernel`; the nearest
  tap is `:47-49` (`q = dst_to_src.apply({x+0.5, y+0.5})`, `i = floor(q.x)`,
  `j = floor(q.y)`), with a single-texel fetch and out-of-range **cull**
  (`:50-52`). `:77-88` `convert_kernel` is the precedent for a kernel
  landing ahead of its call sites.
- `src/backend_cpu/cpu_backend.cpp:70-95` — `CpuBackend::composite`: the
  `visit_surface` dispatch (`:88-94`) that recovers `constexpr PixelFormat
  F` and calls `source_over_kernel<F>`. The downsample dispatch mirrors it.
- `src/surface/arbc/surface/backend.hpp:39-40` — the `composite` pure
  virtual; the new `downsample` virtual sits beside it.
- `src/compositor/compositor.cpp:95-99` — `temp_to_dst` (from
  `result.achieved_scale`) and the `backend.composite(target, temp,
  temp_to_dst, opacity)` call — the composite pass whose filter this task
  upgrades. **Unchanged by this task.**
- `src/media/arbc/media/pixel_traits.hpp` — `WorkingPixel`, `decode`/
  `encode`, `premultiply`/`unpremultiply`; the interpolation runs on
  `decode`d premultiplied working floats.
- `src/cache/arbc/cache/key_shapes.hpp:39-43,99-113` — `ScaleRung`,
  `TileMeta`, `TileValue`: the rung tiles the box downsample generates
  (surfaces keyed by `TileKey`); "the compositor owns the ladder and hands
  the rung down" (`:38`).
- Consumers: `tasks/35-compositor.tji:9` (`scale_ladder`, depends
  `color.resampling`); `tasks/15-color.tji:34-38` (`kernel_goldens`, note
  lists "blend/convert/**resample**" goldens).

## Constraints / requirements

- **Bilinear folds into `source_over_kernel`** (one resample-and-blend
  pass, no intermediate resampled temp). Per destination pixel: map the
  pixel center through `dst_to_src`, take the 2×2 source neighborhood,
  interpolate the four **`decode`d premultiplied linear `WorkingPixel`s**
  (doc 07 rule 3 — never the encoded bytes, never straight alpha), then run
  the existing premultiplied source-over on the interpolated sample. No new
  composite kernel; no `Backend::composite` signature change.
- **Texel-center sampling convention.** Sample position in texel-index
  space is `p = dst_to_src(center) − (0.5, 0.5)`; `i0 = floor(p.x)`,
  `frac = p.x − i0`, taps `i0` (weight `1−frac`) and `i0+1` (weight
  `frac`), likewise in y. At integer alignment `frac == 0` and the bilinear
  weights collapse to the single incumbent texel — so identity/integer
  composites reproduce the current nearest tap **byte-for-byte** (see
  Decisions; this is what keeps the walking-skeleton golden intact).
- **Transparent (premultiplied-zero) border.** Taps outside
  `[0,src_width) × [0,src_height)` contribute `WorkingPixel{0,0,0,0}`, not a
  clamped edge texel — correct antialiased falloff for a temp that exactly
  covers its content region. A sample whose whole 2×2 footprint is outside
  contributes nothing (transparent), matching today's cull.
- **Box downsample.** New `downsample_box_kernel<F>`: each destination
  pixel is the mean of the corresponding 2×2 source block, averaged in
  `decode`d premultiplied linear working floats and `encode`d once. Exact
  2:1; destination dims are `src_dims / 2` with **even** source dims (rung
  tiles are power-of-two device pixels, doc 02:59). Dispatched via a new
  `Backend::downsample(Surface& dst, const Surface& src)` pure virtual
  (declared in `surface`, L2), `CpuBackend::downsample` resolving the tag to
  a `constexpr PixelFormat F` through `visit_surface` — **one visit per
  operation, never per pixel** (doc 07). Same-format only (assert tag
  agreement, mirroring `composite`); cross-format rung generation is not a
  thing (a rung shares its source's working format).
- **Determinism (doc 16).** Fixed tap-evaluation order, `float32` working
  math, `encode` through the existing codecs → byte-exact and
  platform-stable. Goldens are regenerated only where a composite genuinely
  samples fractionally (the task's intended behavior change), never
  silently.
- **Levelization unchanged.** Kernel bodies stay in `backend_cpu` (L3); the
  `Backend::downsample` declaration rides in `surface` (L2) beside
  `composite`; rung *selection* stays in `compositor` (L4, doc 17:56,76-80).
  No `scripts/check_levels.py` edge changes.

## Acceptance criteria

- Unit tests (`src/backend_cpu/t/cpu_backend.t.cpp`):
  - **Bilinear magnification:** a 2× upscale of a known 2×2 source yields
    the analytic per-channel midpoint lerps (in premultiplied linear
    floats), for all three formats within their stated precision.
  - **Fractional offset:** a half-texel-shifted composite yields the exact
    two-tap average, not a snapped texel.
  - **Identity reduces to nearest, byte-exact:** an identity composite and a
    pure-integer translation reproduce the pre-change nearest output
    bit-for-bit for all three formats (the golden-preservation mechanism).
  - **Edge taps:** a sample straddling the source border blends toward
    transparent (premultiplied zero) — no opaque-edge smear; a fully-outside
    center contributes nothing.
  - **Box downsample:** a 2×2→1×1 equals the four-tap mean; a uniform
    surface downsamples to the same uniform value (mean preserved); a
    decisive case shows the average is taken in **linear light**, not on the
    encoded sRGB bytes.
- Claim (register in `tests/claims/registry.tsv` + `enforces:` tag):
  `07-color-and-pixel-formats#resampling-in-linear-premultiplied-space` —
  filtered resampling (the bilinear composite tap **and** the box rung
  downsample) interpolates `decode`d premultiplied linear working floats, so
  an antialiased `Rgba8Srgb` edge resampled shows the linear-light result,
  decisively **not** the gamma-space / straight-alpha resample; the test
  constructs a case where the two differ visibly and asserts the linear
  result.
- The walking-skeleton golden (`tests/walking_skeleton.t.cpp`) stays
  byte-identical — its composite is integer-aligned, so bilinear collapses
  to the incumbent nearest tap; `16-sdlc-and-quality#byte-exact-goldens`
  stays green. Should any existing golden exercise a fractional composite,
  it is regenerated once with bytes hand-verified against the bilinear
  reference, called out in the Status block as the intended behavior change.
- Gate green including asan/ubsan; diff coverage ≥90% on the changed kernel
  and dispatch lines.
- Per-format byte-exact **resample/downsample golden matrix** is deferred to
  `color.kernel_goldens` — whose note already lists "resample" goldens
  (`tasks/15-color.tji:38`) but which currently `depends !kernels` only; it
  must gain a `depends !resampling` edge so the resample goldens can exist
  (closer registers the edge; no new task — see return summary).

## Decisions

- **Fuse the bilinear tap into `source_over_kernel`** — one
  resample-and-blend pass. Rejected: a standalone resample-to-temp kernel
  followed by a nearest composite — it adds an allocation and a full pixel
  pass, and doc 04:95 states the remainder is "applied as resampling
  *during* compositing" (fused); the compositor already composites through
  an affine (`compositor.cpp:95-99`), so the transform is right there.
- **Texel-center convention (`p = mapped_center − 0.5`)** chosen so bilinear
  reduces *exactly* to the incumbent nearest tap at integer alignment. This
  makes the walking-skeleton golden a regression guard the new filter
  passes, not a casualty it rewrites, and leaves exact-scale content
  pixel-crisp. Rejected: a corner-grid convention (`frac = 0.5` at identity
  → averages four texels → blurs every exact-scale composite and forces a
  rewrite of every existing composite golden for no correctness gain).
- **Transparent (premultiplied-zero) border** for out-of-range taps.
  Rejected: clamp-to-edge — it smears opaque edge content outward past the
  content region, which is wrong for a temp sized to exactly its region;
  premultiplied-zero is the correct neutral element for premultiplied
  source-over and gives correct antialiased falloff.
- **Box (2:1 average) as the rung filter, exposed through a new
  `Backend::downsample` virtual now** rather than left a private
  `backend_cpu/kernels.hpp` helper. Doc 04:90 makes the ladder powers of
  two and doc 04:95-98 prefers downsampled rungs; the box mean is the
  canonical energy-preserving exact-2:1 reducer; and doc 09:18/24 already
  names "resample operations consumed by the compositor" as backend
  charter, so the seam needs **no design-doc delta**. Rejected: a bare
  private kernel — the L4 compositor (`scale_ladder`) cannot include a
  private L3 header, so the capability would be unreachable by its only
  consumer. Rejected: a wider rung filter (tent/Lanczos) — box is exact and
  deterministic for 2:1, and quality on the ≤1-octave remainder is already
  the bilinear tap's job (doc 04:95).
- **Filter policy stays in the compositor, not the kernel.** The kernel does
  not detect minify-vs-magnify or select a rung; the ladder
  (`compositor.scale_ladder`, doc 04) keeps the composite-pass remainder
  ≤1 octave — where a single bilinear tap is adequate — and calls
  `Backend::downsample` to build coarser rungs. This task supplies both
  tools; the octave-splitting policy is L4's. Rejected: trilinear/mip
  blending inside the composite kernel now — it needs a ladder and a mip
  chain that don't exist yet (premature, and it belongs to the compositor
  stream).

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-05.

- `src/backend_cpu/kernels.hpp`: nearest tap replaced with bilinear via new `fetch_texel` helper; new `downsample_box_kernel<F>` for 2:1 box reduction.
- `src/surface/arbc/surface/backend.hpp`: new `downsample(Surface& dst, const Surface& src)` pure virtual beside `composite`.
- `src/backend_cpu/arbc/backend_cpu/cpu_backend.hpp` + `src/backend_cpu/cpu_backend.cpp`: `CpuBackend::downsample` one-visit dispatch via `visit_surface`, mirroring the `composite` dispatch.
- `src/media/arbc/media/pixel_traits.hpp` + `src/surface/arbc/surface/typed_span.hpp`: minor adjustments supporting the new kernel bodies.
- `src/backend_cpu/t/cpu_backend.t.cpp`: unit tests — bilinear magnification, fractional offset, integer/identity byte-exact-reduces-to-nearest, edge/transparent-border taps, box downsample four-tap mean, uniform-preserved, linear-light decisive case (all formats where applicable).
- `src/surface/t/surface_pool.t.cpp` + `tests/surface_pool_integration.t.cpp`: test-double `Backend::downsample` overrides.
- `tests/claims/registry.tsv`: new claim `07-color-and-pixel-formats#resampling-in-linear-premultiplied-space` enforced by two linear-light tests (box downsample and bilinear tap).
- Walking-skeleton golden stays byte-identical (integer-aligned composite → bilinear collapses to nearest; no golden rewrite needed).
