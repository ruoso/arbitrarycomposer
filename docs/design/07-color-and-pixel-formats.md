# 07 — Color and Pixel Formats

## The model

Three distinct concepts, deliberately separated:

- **Pixel format**: memory layout of a surface — channel type and order
  (`RGBA8`, `RGBA16F`, …). A *closed set defined by the core*.
- **Color space**: interpretation — primaries + transfer function
  (sRGB, linear-sRGB, Display-P3, …). Metadata carried by every surface.
- **Working space**: the color space + format the compositor blends in,
  configured **per composition**.

Rules:

1. **Every surface is tagged** with pixel format and color space from day
   one. Tags are the part that is prohibitively expensive to retrofit; the
   defaults are cheap to change later.
2. **All compositing happens in the composition's working space** on
   premultiplied alpha. Layers are asked to render *into the working space*
   (the `RenderRequest`'s target surface carries the tag); content that
   sources other spaces (an sRGB JPEG, a P3 photo) converts at decode/render
   time. The viewport applies the output transform (working space → display)
   as the final step.
3. **Default working space: premultiplied linear-light sRGB primaries,
   `RGBA16F`.** Blending and resampling in a nonlinear space is
   mathematically wrong (dark fringing on antialiased edges, hue shifts in
   gradients); linear f16 is correct, HDR-capable, and the standard practice
   everywhere quality matters. `RGBA8` in nonlinear sRGB remains available
   as a configured fast mode for memory-constrained embedders who accept
   the artifacts.
4. Nested compositions may declare different working spaces; the nesting
   boundary is a conversion point like any other content (the child's
   composed output converts into the parent's working space). Homogeneous
   trees pay nothing.

**Resampling filters.** Minification uses a windowed-sinc low-pass
(Lanczos-3); magnification uses an interpolating cubic (Catmull–Rom). Both
operate on decoded premultiplied linear working values (rule 3), with
tabulated float32 weights, a fixed tap order, and no runtime `libm` — so
every resample is byte-exact and portable (doc 16). Both kernels' negative
lobes may ring below zero; the result is clamped to non-negative per channel
before use, which removes the unphysical undershoot (and the negative alpha
that would break unpremultiplication) while leaving the float working space's
HDR headroom above alpha intact. The filters live in `arbc::media` (doc 17)
and are shared by the kinds' mip pyramids and the backend's compositing
kernels.

In the CPU backend this division of labour is explicit. Scale-ladder rung
generation (`CpuBackend::downsample`) is the Lanczos-3 minification path — an
exact 2:1 half-band step, one octave at a time. The composite tap
(`source_over_kernel`) is the Catmull-Rom reconstruction of the ≤1-octave
remainder (doc 04) from the *single* rung the compositor selected. That rung
is already band-limited by the Lanczos decimation that built it, so the tap
needs no second low-pass: it is single-rung reconstruction, deliberately not
cross-level (trilinear) blending, which would defeat the one-rung-serves-an-
octave cache reuse (doc 04). Because Catmull-Rom is interpolating — its
integer-phase weights are exactly `(0, 1, 0, 0)` in float32 — an
integer-aligned composite reproduces the source sample bit-for-bit, so the
mild sub-octave aliasing the single-rung remainder can carry is the accepted
cost of that cache reuse, not a filter defect.

Out of scope for v1 but kept structurally possible: full OCIO-style
management, HDR output transforms/tone mapping, CMYK. These all slot in as
"more color spaces + better edge conversions" without touching the model.

## Templates and variants: where compile-time formats live

Design question raised during review: *can the working format be a template
parameter, dispatched via variants?* Yes and no — it splits by layer of the
system, and the split is load-bearing:

**No, at the public API and plugin boundary.** `Content`, `RenderRequest`,
`Surface`-as-seen-by-plugins, and the compositor's public types are *not*
templated on pixel format:

- Templates don't cross ABI boundaries. A plugin compiled against
  `Content<RGBA16F>` cannot serve a host configured for `RGBA8` without
  recompilation; supporting N formats would mean every plugin ships N
  instantiations, and the planned stable C ABI (doc 03) becomes impossible
  to state. A `std::variant` in the plugin signature doesn't help — the
  plugin then handles all alternatives at runtime anyway, which is exactly
  the type-erased design with extra steps.
- Most content never touches pixels directly: vector, 3D, and nested layers
  render through backend drawing APIs that consume the surface tag
  themselves. Making every plugin author confront the format parameter
  taxes the common case to benefit the rare one.
- The working space is *per composition* and chosen at runtime
  (configuration, or a loaded file — doc 08); the host binary cannot know
  it at compile time, so somewhere there is unavoidably a runtime dispatch.
  The design's job is to put that dispatch in the right place.

**Yes, inside the core's compositing kernels — and this is exactly where
variants earn their keep.** The CPU backend's hot loops (blend, resample,
premultiply, space conversion) are function templates over
`(PixelFormat, ColorTransfer)`:

```cpp
// Internal to the CPU backend — never visible to plugins.
template <PixelFormatDesc F>
void composite_tile(TypedSpan<F> dst, TypedSpan<F> src,
                    const AffineDevice&, float opacity);

// One dispatch per *operation*, never per pixel:
using AnySurfaceRef = std::variant<TypedSpan<Rgba8Srgb>,
                                   TypedSpan<Rgba16fLinear> /*…*/>;
std::visit([&](auto dst, auto src) { composite_tile(dst, src, m, o); },
           dst.typed(), src.typed());
```

The closed format set makes the variant total; the compiler monomorphizes
each kernel (vectorizable tight loops, no per-pixel branching); the erased
`Surface` boundary costs one `visit` per tile-sized operation, which is
noise. Plugins that *do* want raw CPU pixel access get the same deal via a
typed accessor: `surface.span<Rgba16fLinear>()` (checked against the tag,
error on mismatch) or a visit-style helper for format-generic plugin code.

This mirrors the doc-04 numeric philosophy: strong guarantees inside the
core, plain erased types at the boundary. Cross-format compositing
(mixed-working-space nesting, rule 4) instantiates conversion kernels over
format *pairs*; the set stays small because the format list is closed and
conversions route through the working space rather than N×N.

**Guardrail.** The format list is core-owned precisely so the variant stays
total; plugins cannot add pixel formats. A plugin with an exotic native
format converts to a core format when rendering — same as any other
source-space conversion. If a genuinely new format class shows up (e.g.
10-bit packed video), it is added to the core enum and kernels, a minor
version bump, not a plugin.
