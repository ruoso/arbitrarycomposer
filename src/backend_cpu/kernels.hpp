#pragma once

// Format-templated CPU kernels (doc 07: "yes, inside the kernels"). Each is a
// function template over a compile-time PixelFormat, monomorphized so the hot
// loop carries no per-pixel format branch. The single runtime format decision
// happens once per operation at the variant visit in cpu_backend.cpp, never
// here. Private to backend_cpu -- plugins never see these; they reach typed
// pixels through the checked accessors in <arbc/surface/typed_span.hpp>.
//
// SIMD is out of scope, but the kernels operate on contiguous typed spans, so
// vectorizing them later is a body change, not a structural one.

#include <arbc/base/geometry.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/media/pixel_traits.hpp>
#include <arbc/surface/typed_span.hpp>

#include <cmath>
#include <cstddef>
#include <span>

namespace arbc {

// Fill every pixel of `dst` with one working-space color, encoding it into the
// destination format once per pixel.
template <PixelFormat F>
void fill_kernel(TypedSpan<F> dst, const WorkingPixel& color) {
  using Traits = PixelTraits<F>;
  for (std::size_t i = 0; i + Traits::channels <= dst.data.size(); i += Traits::channels) {
    Traits::encode(color, &dst.data[i]);
  }
}

// Source-over composite of `src` onto `dst`, same format (doc 07 rule 2 --
// premultiplied source-over `out = s + (1 - a_s) d`, evaluated in linear
// working floats). Nearest sampling at the destination pixel center; filtered
// resampling is color.resampling. `dst_to_src` maps destination pixel space
// into source pixel space.
template <PixelFormat F>
void source_over_kernel(TypedSpan<F> dst, int dst_width, int dst_height,
                        std::span<const typename PixelTraits<F>::Storage> src, int src_width,
                        int src_height, const Affine& dst_to_src, float opacity) {
  using Traits = PixelTraits<F>;
  const std::size_t stride = Traits::channels;
  for (int y = 0; y < dst_height; ++y) {
    for (int x = 0; x < dst_width; ++x) {
      const Vec2 q = dst_to_src.apply({x + 0.5, y + 0.5});
      const int i = static_cast<int>(std::floor(q.x));
      const int j = static_cast<int>(std::floor(q.y));
      if (i < 0 || i >= src_width || j < 0 || j >= src_height) {
        continue;
      }
      const std::size_t src_at =
          stride * (static_cast<std::size_t>(j) * static_cast<std::size_t>(src_width) +
                    static_cast<std::size_t>(i));
      const std::size_t dst_at =
          stride * (static_cast<std::size_t>(y) * static_cast<std::size_t>(dst_width) +
                    static_cast<std::size_t>(x));
      const WorkingPixel s = Traits::decode(&src[src_at]);
      const WorkingPixel d = Traits::decode(&dst.data[dst_at]);
      const float alpha = s[3] * opacity;
      WorkingPixel out{};
      for (std::size_t k = 0; k < Traits::channels; ++k) {
        out[k] = s[k] * opacity + (1.0F - alpha) * d[k];
      }
      Traits::encode(out, &dst.data[dst_at]);
    }
  }
}

// Format/transfer conversion (doc 07: "conversions route through the working
// space"). Decode each source sample to the premultiplied linear working
// space, then encode into the destination format -- 2N codecs, not N*N pairs.
// `pixel_count` is the number of 4-channel pixels to convert. This is the
// kernel the later edge tasks (imports, nesting boundary, display-out) wire
// into their call sites; it is same-geometry, position-for-position.
template <PixelFormat SrcF, PixelFormat DstF>
void convert_kernel(std::span<const typename PixelTraits<SrcF>::Storage> src, TypedSpan<DstF> dst,
                    std::size_t pixel_count) {
  using SrcTraits = PixelTraits<SrcF>;
  using DstTraits = PixelTraits<DstF>;
  static_assert(SrcTraits::channels == DstTraits::channels);
  const std::size_t stride = SrcTraits::channels;
  for (std::size_t p = 0; p < pixel_count; ++p) {
    const WorkingPixel working = SrcTraits::decode(&src[p * stride]);
    DstTraits::encode(working, &dst.data[p * stride]);
  }
}

} // namespace arbc
