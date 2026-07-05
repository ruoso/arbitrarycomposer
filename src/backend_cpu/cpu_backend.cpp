#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/media/pixel_format.hpp>
#include <arbc/media/pixel_traits.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/surface/typed_span.hpp>

#include "kernels.hpp"

#include <cassert>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <utility>

namespace arbc {

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
  // (r,g,b,a) is a premultiplied working-space sample (doc 07 rule 2). One
  // variant dispatch per operation: resolve the surface's runtime format tag to
  // a compile-time typed span, then run the monomorphized fill that encodes the
  // working color into the destination format once per pixel.
  const WorkingPixel color{r, g, b, a};
  visit_surface(surface, [&](auto typed) { fill_kernel(typed, color); });
}

void CpuBackend::composite(Surface& dst, const Surface& src, const Affine& src_to_dst,
                           double opacity) {
  // Tag agreement (doc 07 rule 2): compositing happens within one working
  // format; converting between differing tags routes through convert_kernel and
  // is wired by the edge tasks (imports, nesting, display-out), not here. Until
  // then a mismatch is a caller error, never a silent reinterpretation -- debug
  // assert, no-op in release, mirroring the degenerate-transform cull below.
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
    source_over_kernel<F>(dst_typed, dst.width(), dst.height(), src_span, src.width(), src.height(),
                          *dst_to_src, static_cast<float>(opacity));
  });
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

} // namespace arbc
