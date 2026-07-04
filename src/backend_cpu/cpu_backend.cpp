#include <arbc/backend_cpu/cpu_backend.hpp>

#include <cassert>
#include <cmath>
#include <cstddef>
#include <utility>

namespace arbc {

CpuSurface::CpuSurface(int width, int height, PixelFormat format)
    : d_width(width), d_height(height), d_format(format),
      d_data(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) *
                 channels_per_pixel(format),
             0.0F) {}

std::unique_ptr<Surface> CpuBackend::make_surface(int width, int height, PixelFormat format) {
  return std::make_unique<CpuSurface>(width, height, format);
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
