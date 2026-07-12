#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/media/pixel_format.hpp>
#include <arbc/media/pixel_traits.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/surface/typed_span.hpp>

#include "kernels.hpp"

#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <utility>

namespace arbc {

namespace {

// A compile-time format as a value, for the SOURCE side of a two-format
// operation. `visit_surface` resolves the destination (it hands back a WRITABLE
// TypedSpan); a `const Surface&` source only needs its format lifted to a
// compile-time constant, and reads through the checked `Surface::span<F>() const`
// accessor. Composing the two gives ONE dispatch per operation over the (src,
// dst) pair -- total over the closed format set -- never a per-pixel branch
// (doc 07:79-95).
template <PixelFormat F> struct FormatTag {
  static constexpr PixelFormat format = F;
};

template <class Visitor> void visit_pixel_format(PixelFormat format, Visitor&& visitor) {
  switch (format) {
  case PixelFormat::Rgba32fLinearPremul:
    visitor(FormatTag<PixelFormat::Rgba32fLinearPremul>{});
    return;
  case PixelFormat::Rgba16fLinearPremul:
    visitor(FormatTag<PixelFormat::Rgba16fLinearPremul>{});
    return;
  case PixelFormat::Rgba8Srgb:
    visitor(FormatTag<PixelFormat::Rgba8Srgb>{});
    return;
  }
}

// Resolve a device-space clip rect against a destination's bounds into the
// half-open integer pixel box the kernels walk (doc 09 "The clip-scoped
// operations"). Two rules, both stated in doc 09 and both load-bearing:
//
//  * *Intersected with the destination's bounds* -- a clip reaching past the
//    edge (or `Rect::infinite()`) is legal and simply saturates, so the kernels
//    need no per-pixel bounds check and an unclipped operation is expressible as
//    the whole-destination clip.
//  * *Rounded OUT to whole pixels* -- a device dirty rect is a `map_rect` output,
//    i.e. arbitrary doubles. A pixel whose cell the clip touches at all is in.
//    Rounding IN would leave a sub-pixel fringe of the repaint region unpainted
//    (a stale seam); rounding out is conservative in the safe direction, and the
//    compositor gates its plan on the *same* rounded rect, so the extra pixels
//    are covered by every layer that covers them (refinement Decision 2).
//
// An empty (or NaN-poisoned, which `Rect::empty()` reports empty) clip yields an
// empty box: the kernels then walk zero pixels, which is doc 09's "an empty clip
// is a no-op".
PixelBox clip_box(const Rect& device_clip, int width, int height) {
  const Rect bounded = device_clip.intersect(
      Rect::from_size(static_cast<double>(width), static_cast<double>(height)));
  if (bounded.empty()) {
    return PixelBox{};
  }
  return PixelBox{static_cast<int>(std::floor(bounded.x0)),
                  static_cast<int>(std::floor(bounded.y0)), static_cast<int>(std::ceil(bounded.x1)),
                  static_cast<int>(std::ceil(bounded.y1))};
}

// The whole destination, as a clip: the box that makes a clipped operation the
// unclipped one (doc 09 -- which is how `clear`/`composite` are defined below).
PixelBox whole_surface(const Surface& surface) {
  return PixelBox{0, 0, surface.width(), surface.height()};
}

// The one fill: (r,g,b,a) is a premultiplied working-space sample (doc 07 rule
// 2). One variant dispatch per operation -- resolve the surface's runtime format
// tag to a compile-time typed span, then run the monomorphized fill that encodes
// the working color into the destination format once per pixel inside `box`.
void clear_in_box(Surface& surface, const PixelBox& box, float r, float g, float b, float a) {
  const WorkingPixel color{r, g, b, a};
  const int width = surface.width();
  visit_surface(surface, [&](auto typed) { fill_kernel(typed, width, box, color); });
}

// The one composite. Tag agreement (doc 07 rule 2): compositing happens within
// one working format; converting between differing tags routes through
// convert_kernel and is wired by the edge tasks (imports, nesting, display-out),
// not here. Until then a mismatch is a caller error, never a silent
// reinterpretation -- debug assert, no-op in release, mirroring the
// degenerate-transform cull below.
void composite_in_box(Surface& dst, const Surface& src, const Affine& src_to_dst, double opacity,
                      const PixelBox& box) {
  assert(dst.format() == src.format());
  if (!(dst.format() == src.format())) {
    return;
  }
  const std::optional<Affine> dst_to_src = src_to_dst.inverse();
  if (!dst_to_src.has_value()) {
    return; // degenerate mapping: cull, never propagate NaNs (doc 04)
  }
  // One dispatch per tile-sized operation (doc 07), never per pixel: the kernel
  // body is monomorphized on the shared format. src is read through the same
  // compile-time format, so the source-over math runs in linear working floats.
  visit_surface(dst, [&](auto dst_typed) {
    constexpr PixelFormat F = decltype(dst_typed)::format;
    const std::span<const typename PixelTraits<F>::Storage> src_span = src.span<F>();
    assert(!dst_typed.data.empty() && !src_span.empty());
    source_over_kernel<F>(dst_typed, dst.width(), src_span, src.width(), src.height(), *dst_to_src,
                          static_cast<float>(opacity), box);
  });
}

} // namespace

// Byte-backed storage sized by the pixel format (doc 07): 16 / 8 / 4 bytes per
// pixel for 32f / 16f / 8. Zero-filled bytes are transparent black in every
// format (float +0, integer 0), so a fresh surface starts clear -- and the 32f
// case is byte-identical to the walking skeleton's old float-count layout.
CpuSurface::CpuSurface(int width, int height, SurfaceFormat format)
    : d_width(width), d_height(height), d_format(format),
      d_data(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) *
                 bytes_per_pixel(format.pixel_format),
             std::byte{0}) {}

BackendCaps CpuBackend::capabilities() const {
  // Honest current caps (doc 07/09): the reference backend serves typed CPU
  // access, imports no external handles, and offers no sync primitives. The
  // import bits flip on when surfaces.import lands wrap-or-copy; sync arrives
  // with GPU backends. Advertising them before the machinery exists would be
  // exactly the dishonesty capability honesty forbids.
  return BackendCaps{
      .cpu_access = true,
      .import_handles = {},
      .sync_primitives = false,
  };
}

expected<std::unique_ptr<Surface>, SurfaceError> CpuBackend::make_surface(int width, int height,
                                                                          SurfaceFormat format) {
  // Capability honesty (doc 07): with color.kernels the reference backend now
  // stores all three closed working formats -- premultiplied linear-light
  // rgba32f and rgba16f, and the straight-alpha 8-bit sRGB fast mode -- each at
  // the exact tag triple its codecs honor. Any other tag combination (a color
  // space or premultiplication no kernel implements, e.g. gamma-space rgba32f
  // or premultiplied rgba8) is honestly unsupported -> SurfaceError, surfaced
  // as a value (doc 10), never a null handle and never an abort. A configurable
  // working space beyond these arrives with color.working_space.
  if (format != k_working_rgba32f && format != k_working_rgba16f && format != k_fast_rgba8srgb) {
    return unexpected(SurfaceError::UnsupportedFormat);
  }
  // Convert to the base handle first: expected's value ctor takes exactly
  // unique_ptr<Surface>, and a direct unique_ptr<CpuSurface> argument would
  // need two user-defined conversions (derived->base, then into expected).
  std::unique_ptr<Surface> surface = std::make_unique<CpuSurface>(width, height, format);
  return surface;
}

void CpuBackend::clear(Surface& surface, float r, float g, float b, float a) {
  // The unclipped clear IS the whole-destination clip case (doc 09), so the
  // backend carries one fill kernel, not two.
  clear_in_box(surface, whole_surface(surface), r, g, b, a);
}

void CpuBackend::clear_rect(Surface& dst, const Rect& device_rect, float r, float g, float b,
                            float a) {
  clear_in_box(dst, clip_box(device_rect, dst.width(), dst.height()), r, g, b, a);
}

void CpuBackend::composite(Surface& dst, const Surface& src, const Affine& src_to_dst,
                           double opacity) {
  // The unclipped composite IS the whole-destination clip case (doc 09): same
  // kernel, same taps, every destination pixel in the box.
  composite_in_box(dst, src, src_to_dst, opacity, whole_surface(dst));
}

void CpuBackend::composite_clipped(Surface& dst, const Surface& src, const Affine& src_to_dst,
                                   double opacity, const Rect& device_clip) {
  composite_in_box(dst, src, src_to_dst, opacity, clip_box(device_clip, dst.width(), dst.height()));
}

void CpuBackend::downsample(Surface& dst, const Surface& src) {
  // Same-format rung generation (doc 07 rule 2): a rung shares its source's
  // working format, so cross-format downsampling is not a thing -- a tag
  // mismatch is a caller error, debug-asserted and culled in release, mirroring
  // composite. The 2:1 geometry is exact: dst dims are src dims / 2 with even
  // source dims (rung tiles are power-of-two device pixels, doc 02:59).
  assert(dst.format() == src.format());
  if (!(dst.format() == src.format())) {
    return;
  }
  assert(src.width() % 2 == 0 && src.height() % 2 == 0);
  assert(dst.width() == src.width() / 2 && dst.height() == src.height() / 2);
  if (dst.width() != src.width() / 2 || dst.height() != src.height() / 2) {
    return;
  }
  // One dispatch per operation (doc 07), never per pixel: resolve the shared
  // runtime tag to a compile-time format once, then run the monomorphized box
  // mean in decoded premultiplied linear working floats.
  visit_surface(dst, [&](auto dst_typed) {
    constexpr PixelFormat F = decltype(dst_typed)::format;
    const std::span<const typename PixelTraits<F>::Storage> src_span = src.span<F>();
    assert(!dst_typed.data.empty() && !src_span.empty());
    downsample_box_kernel<F>(dst_typed, dst.width(), dst.height(), src_span, src.width(),
                             src.height());
  });
}

void CpuBackend::convert(Surface& dst, const Surface& src) {
  // Same-geometry, position-for-position (doc 09, the conversion operation): the
  // kernel transcodes pixel (i, j) into pixel (i, j) and does not resample, so
  // differing dimensions are a caller error, never a silent reinterpretation --
  // debug assert, cull in release, the convention composite and downsample use.
  assert(dst.width() == src.width() && dst.height() == src.height());
  if (dst.width() != src.width() || dst.height() != src.height()) {
    return;
  }

  // One dispatch per operation (doc 07:79-95), never per pixel: resolve the
  // (source, destination) runtime tag PAIR to compile-time formats once, then run
  // the monomorphized kernel, which routes each sample through the premultiplied
  // linear working space (2N codecs, not N*N -- doc 07:104-108). The format set
  // is closed and core-owned, so the pair dispatch is total: no default-case hole.
  const std::size_t pixel_count =
      static_cast<std::size_t>(dst.width()) * static_cast<std::size_t>(dst.height());
  visit_surface(dst, [&](auto dst_typed) {
    constexpr PixelFormat DstF = decltype(dst_typed)::format;
    visit_pixel_format(src.format().pixel_format, [&](auto src_tag) {
      constexpr PixelFormat SrcF = decltype(src_tag)::format;
      if constexpr (SrcF == DstF) {
        // The diagonal of the pair dispatch is an EXACT byte copy, decisively not
        // a decode/encode round-trip. Routing it through convert_kernel<F, F>
        // would decode to the linear working space and re-encode -- byte-identical
        // for the float formats, but NOT for Rgba8Srgb, where the straight-alpha
        // unpremultiply and the sRGB encode's rounding can move a low bit (and a
        // zero-alpha pixel loses its color outright). A public conversion never
        // perturbs pixels it has no reason to perturb.
        //
        // Equal pixel format IS equal tag triple here: `make_surface` stores only
        // the three closed working formats, and each pins a distinct pixel format,
        // so no surface this backend hands out can share a pixel format while
        // differing in color space or alpha convention. The assert pins that.
        assert(dst.format() == src.format());
        const std::span<const std::byte> in = src.cpu_bytes();
        const std::span<std::byte> out = dst.cpu_bytes();
        assert(in.size() == out.size());
        std::memcpy(out.data(), in.data(), out.size());
      } else {
        const std::span<const typename PixelTraits<SrcF>::Storage> src_span = src.span<SrcF>();
        assert(!dst_typed.data.empty() && !src_span.empty());
        convert_kernel<SrcF, DstF>(src_span, dst_typed, pixel_count);
      }
    });
  });
}

} // namespace arbc
