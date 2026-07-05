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
template <PixelFormat F> void fill_kernel(TypedSpan<F> dst, const WorkingPixel& color) {
  using Traits = PixelTraits<F>;
  for (std::size_t i = 0; i + Traits::channels <= dst.data.size(); i += Traits::channels) {
    Traits::encode(color, &dst.data[i]);
  }
}

// Fetch source texel (i, j) as a decoded premultiplied linear working sample,
// with a transparent (premultiplied-zero) border for out-of-range indices
// (doc 07 rule 3 -- resampling runs on decoded premultiplied linear floats;
// the neutral element for premultiplied source-over is `{0,0,0,0}`, giving
// correct antialiased falloff for a temp sized to exactly its content region
// rather than a clamped-edge smear). Fixed for determinism (doc 16).
template <PixelFormat F>
inline WorkingPixel fetch_texel(std::span<const typename PixelTraits<F>::Storage> src,
                                int src_width, int src_height, int i, int j) {
  if (i < 0 || i >= src_width || j < 0 || j >= src_height) {
    return WorkingPixel{0.0F, 0.0F, 0.0F, 0.0F};
  }
  const std::size_t at = PixelTraits<F>::channels *
                         (static_cast<std::size_t>(j) * static_cast<std::size_t>(src_width) +
                          static_cast<std::size_t>(i));
  return PixelTraits<F>::decode(&src[at]);
}

// Source-over composite of `src` onto `dst`, same format (doc 07 rule 2 --
// premultiplied source-over `out = s + (1 - a_s) d`, evaluated in linear
// working floats). Bilinear resampling at the destination pixel center: the
// resample folds into the blend as one pass (doc 04:95 -- the sub-octave
// remainder is "applied as resampling during compositing"). `dst_to_src` maps
// destination pixel space into source pixel space.
//
// Texel-center convention (doc 07 rule 3): the sample position in texel-index
// space is `p = dst_to_src(center) - (0.5, 0.5)`, so at integer alignment
// `frac == 0` and the two-tap weights collapse to the single incumbent texel
// -- an identity or pure-integer composite reproduces the pre-filter nearest
// tap byte-for-byte (the walking-skeleton golden is a regression guard, not a
// casualty). The four taps are interpolated in decoded premultiplied linear
// working floats, never the encoded bytes and never straight alpha.
template <PixelFormat F>
void source_over_kernel(TypedSpan<F> dst, int dst_width, int dst_height,
                        std::span<const typename PixelTraits<F>::Storage> src, int src_width,
                        int src_height, const Affine& dst_to_src, float opacity) {
  using Traits = PixelTraits<F>;
  const std::size_t stride = Traits::channels;
  for (int y = 0; y < dst_height; ++y) {
    for (int x = 0; x < dst_width; ++x) {
      const Vec2 q = dst_to_src.apply({x + 0.5, y + 0.5});
      const double sx = q.x - 0.5;
      const double sy = q.y - 0.5;
      const int i0 = static_cast<int>(std::floor(sx));
      const int j0 = static_cast<int>(std::floor(sy));
      // Cull only when the whole 2x2 footprint is out of range -- a sample with
      // no in-range tap contributes nothing (transparent), matching the old
      // nearest cull and leaving `dst` untouched. A straddling footprint blends
      // toward the transparent border instead (antialiased falloff).
      if (i0 + 1 < 0 || i0 >= src_width || j0 + 1 < 0 || j0 >= src_height) {
        continue;
      }
      const float fx = static_cast<float>(sx - i0);
      const float fy = static_cast<float>(sy - j0);
      const float wx0 = 1.0F - fx;
      const float wy0 = 1.0F - fy;
      const WorkingPixel s00 = fetch_texel<F>(src, src_width, src_height, i0, j0);
      const WorkingPixel s10 = fetch_texel<F>(src, src_width, src_height, i0 + 1, j0);
      const WorkingPixel s01 = fetch_texel<F>(src, src_width, src_height, i0, j0 + 1);
      const WorkingPixel s11 = fetch_texel<F>(src, src_width, src_height, i0 + 1, j0 + 1);
      WorkingPixel s{};
      for (std::size_t k = 0; k < Traits::channels; ++k) {
        const float top = s00[k] * wx0 + s10[k] * fx;
        const float bot = s01[k] * wx0 + s11[k] * fx;
        s[k] = top * wy0 + bot * fy;
      }
      const std::size_t dst_at =
          stride * (static_cast<std::size_t>(y) * static_cast<std::size_t>(dst_width) +
                    static_cast<std::size_t>(x));
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

// Box-filtered exact 2:1 downsample (doc 09:18 backend-internal resample op):
// each destination pixel is the mean of its 2x2 source block, averaged in
// decoded premultiplied linear working floats and encoded once (doc 07 rule 3
// -- averaging encoded sRGB bytes would be gamma-space-wrong). Exact 2:1, so
// `dst` dims are `src` dims / 2 with even source dims (rung tiles are
// power-of-two device pixels, doc 02:59). Same-format; the caller (the later
// scale-ladder task) generates coarser rungs with it. Fixed tap order for
// determinism (doc 16).
template <PixelFormat F>
void downsample_box_kernel(TypedSpan<F> dst, int dst_width, int dst_height,
                           std::span<const typename PixelTraits<F>::Storage> src, int src_width,
                           int src_height) {
  using Traits = PixelTraits<F>;
  const std::size_t stride = Traits::channels;
  for (int y = 0; y < dst_height; ++y) {
    for (int x = 0; x < dst_width; ++x) {
      const int i = 2 * x;
      const int j = 2 * y;
      const WorkingPixel a = fetch_texel<F>(src, src_width, src_height, i, j);
      const WorkingPixel b = fetch_texel<F>(src, src_width, src_height, i + 1, j);
      const WorkingPixel c = fetch_texel<F>(src, src_width, src_height, i, j + 1);
      const WorkingPixel e = fetch_texel<F>(src, src_width, src_height, i + 1, j + 1);
      WorkingPixel m{};
      for (std::size_t k = 0; k < Traits::channels; ++k) {
        m[k] = (a[k] + b[k] + c[k] + e[k]) * 0.25F;
      }
      const std::size_t dst_at =
          stride * (static_cast<std::size_t>(y) * static_cast<std::size_t>(dst_width) +
                    static_cast<std::size_t>(x));
      Traits::encode(m, &dst.data[dst_at]);
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
