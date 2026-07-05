#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/media/surface_format.hpp>

#include <cassert>
#include <cmath>
#include <cstddef>
#include <utility>

namespace arbc {

CpuSurface::CpuSurface(int width, int height, SurfaceFormat format)
    : d_width(width), d_height(height), d_format(format),
      d_data(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) *
                 channels_per_pixel(format.pixel_format),
             0.0F) {}

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
  // Capability honesty (doc 07): the reference backend stores and composites
  // only the premultiplied linear-light rgba32f working format today. f16 /
  // 8-bit storage and the tag conversions arrive with color.kernels; a
  // configurable working space with color.working_space. Anything else is
  // honestly unsupported -> SurfaceError, surfaced as a value (doc 10), never
  // a null handle and never an abort.
  if (format != k_working_rgba32f) {
    return unexpected(SurfaceError::UnsupportedFormat);
  }
  // Convert to the base handle first: expected's value ctor takes exactly
  // unique_ptr<Surface>, and a direct unique_ptr<CpuSurface> argument would
  // need two user-defined conversions (derived->base, then into expected).
  std::unique_ptr<Surface> surface = std::make_unique<CpuSurface>(width, height, format);
  return surface;
}

void CpuBackend::clear(Surface& surface, float r, float g, float b, float a) {
  std::span<float> pixels = surface.cpu_pixels();
  assert(!pixels.empty() || surface.width() == 0 || surface.height() == 0);
  for (std::size_t i = 0; i + 3 < pixels.size(); i += 4) {
    pixels[i + 0] = r;
    pixels[i + 1] = g;
    pixels[i + 2] = b;
    pixels[i + 3] = a;
  }
}

void CpuBackend::composite(Surface& dst, const Surface& src, const Affine& src_to_dst,
                           double opacity) {
  // Tag agreement (doc 07 rule 2): compositing happens within one working
  // format; converting between differing tags is color.kernels' job. Until
  // then a mismatch is a caller error, never a silent reinterpretation --
  // debug assert, no-op in release, mirroring the degenerate-transform cull
  // below.
  assert(dst.format() == src.format());
  if (!(dst.format() == src.format())) {
    return;
  }
  const std::optional<Affine> dst_to_src = src_to_dst.inverse();
  if (!dst_to_src.has_value()) {
    return; // degenerate mapping: cull, never propagate NaNs (doc 04)
  }
  std::span<float> d = dst.cpu_pixels();
  const std::span<const float> s = std::as_const(src).cpu_pixels();
  assert(!d.empty() && !s.empty());

  const int dst_width = dst.width();
  const int dst_height = dst.height();
  const int src_width = src.width();
  const int src_height = src.height();
  const auto op = static_cast<float>(opacity);

  for (int y = 0; y < dst_height; ++y) {
    for (int x = 0; x < dst_width; ++x) {
      // Nearest sampling at the destination pixel center. Filtered
      // resampling arrives with the real kernel set (doc 07).
      const Vec2 q = dst_to_src->apply({x + 0.5, y + 0.5});
      const int i = static_cast<int>(std::floor(q.x));
      const int j = static_cast<int>(std::floor(q.y));
      if (i < 0 || i >= src_width || j < 0 || j >= src_height) {
        continue;
      }
      const std::size_t src_at =
          4 * (static_cast<std::size_t>(j) * static_cast<std::size_t>(src_width) +
               static_cast<std::size_t>(i));
      const std::size_t dst_at =
          4 * (static_cast<std::size_t>(y) * static_cast<std::size_t>(dst_width) +
               static_cast<std::size_t>(x));
      // Source-over on premultiplied alpha (doc 07): out = s + (1 - a_s) d,
      // uniformly across all four channels.
      const float alpha = s[src_at + 3] * op;
      for (std::size_t k = 0; k < 4; ++k) {
        d[dst_at + k] = s[src_at + k] * op + (1.0F - alpha) * d[dst_at + k];
      }
    }
  }
}

} // namespace arbc
