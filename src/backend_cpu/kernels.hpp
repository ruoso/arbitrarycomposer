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
#include <arbc/media/blend_mode.hpp>
#include <arbc/media/image_resampler.hpp>
#include <arbc/media/pixel_traits.hpp>
#include <arbc/surface/typed_span.hpp>

#include <cmath>
#include <cstddef>
#include <span>

namespace arbc {

// A half-open integer pixel box: the clip every write-side kernel is scoped to
// (doc 09 "The clip-scoped operations" -- a scissor rect). Already resolved
// against the destination's bounds by `clip_box` in cpu_backend.cpp, so a kernel
// may index [x0, x1) x [y0, y1) without a further bounds check. Empty means "no
// pixel", which is how a kernel realizes the doc-09 "an empty clip is a no-op".
struct PixelBox {
  int x0{0};
  int y0{0};
  int x1{0};
  int y1{0};

  constexpr bool empty() const { return !(x0 < x1 && y0 < y1); }
};

// Fill the pixels of `dst` inside `clip` with one working-space color, encoding
// it into the destination format once per pixel. The whole-surface clip is the
// unclipped `Backend::clear` (doc 09: the unclipped op *is* the whole-destination
// clip case), so this is the one fill kernel, not two.
template <PixelFormat F>
void fill_kernel(TypedSpan<F> dst, int dst_width, const PixelBox& clip, const WorkingPixel& color) {
  using Traits = PixelTraits<F>;
  const std::size_t stride = Traits::channels;
  for (int y = clip.y0; y < clip.y1; ++y) {
    for (int x = clip.x0; x < clip.x1; ++x) {
      const std::size_t at =
          stride * (static_cast<std::size_t>(y) * static_cast<std::size_t>(dst_width) +
                    static_cast<std::size_t>(x));
      Traits::encode(color, &dst.data[at]);
    }
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
// working floats). The ≤1-octave remainder resample folds into the blend as one
// pass (doc 04:95-98 -- the sub-octave remainder is "applied as resampling
// during compositing") through the shared `arbc::media` Catmull-Rom bicubic tap
// (`sample_bicubic`), so a tile the compositor rescales matches, byte-for-byte,
// the same content the kinds' mip pyramid reconstructs off the one filter bank
// (doc 07 § Resampling filters). `dst_to_src` maps destination pixel space into
// source pixel space.
//
// Texel-center convention (doc 07 rule 3): the sample position in texel-index
// space is `p = dst_to_src(center) - (0.5, 0.5)`, split into an integer base
// `(i0, j0) = floor(p)` and a fractional phase `(fx, fy)`. Catmull-Rom is
// INTERPOLATING: at integer alignment `frac == 0` its weights are exactly
// `(0, 1, 0, 0)` in float32, so the tap collapses to the single incumbent texel
// and an identity or pure-integer composite reproduces the pre-filter nearest
// tap byte-for-byte (the walking-skeleton golden is a regression guard, not a
// casualty). The 4x4 taps are reconstructed in decoded premultiplied linear
// working floats, never the encoded bytes and never straight alpha; the bank
// clamps the negative-lobe undershoot to non-negative once before the blend so a
// ringing tap can never feed a negative alpha into unpremultiplication.
//
// The destination walk is scoped to `clip` (doc 09 "The clip-scoped operations"):
// no pixel outside it is read or written, and the sample position of a pixel
// inside it does not depend on the clip -- so a clipped composite paints exactly
// the clip-restricted subset of what the unclipped one would. The whole-surface
// clip IS the unclipped `Backend::composite`, so this is the one composite
// kernel, not two.
//
// `window` is the SOURCE-space paint window (`compositor.tile_apron`): a pixel is
// painted only if its sample position `q` lands inside it, half-open, in the same
// source pixel coordinates the tap indexes. Note where the test sits -- AFTER `q`
// is computed and BEFORE the tap: the window narrows what is painted, never what
// is sampled, so the 4x4 footprint still reaches past the window into a tile's
// apron and reads real colour there. `Rect::infinite()` is the unwindowed case
// (`composite` / `composite_clipped`), where the test can never fail, so this
// stays the one composite kernel.
// `blend` is the layer's BLEND MODE (issue #36), and `BlendMode::Normal` takes a SEPARATE
// ARM of the inner loop on purpose. Normal is algebraically the general formula with
// `B(Cb, Cs) = Cs`, so one expression would serve both -- but "algebraically equal" is not
// "bit-identical" in float32, and every composite golden in the tree pins the bytes the
// two-term source-over expression produces. So Normal keeps that exact expression, evaluated
// in that exact order, and the general arm is reached only by a layer that asked for a mode.
// The default path is therefore provably unchanged rather than argued to be.
template <PixelFormat F>
void composite_kernel(TypedSpan<F> dst, int dst_width,
                      std::span<const typename PixelTraits<F>::Storage> src, int src_width,
                      int src_height, const Affine& dst_to_src, float opacity, const PixelBox& clip,
                      const Rect& window, BlendMode blend = BlendMode::Normal) {
  using Traits = PixelTraits<F>;
  const std::size_t stride = Traits::channels;
  for (int y = clip.y0; y < clip.y1; ++y) {
    for (int x = clip.x0; x < clip.x1; ++x) {
      const Vec2 q = dst_to_src.apply({x + 0.5, y + 0.5});
      // Half-open in source pixel space, so two windows abutting on a shared edge
      // partition the destination exactly: the pixel sampling that edge belongs to
      // the window on its >= side, and to no other. That is what makes every device
      // pixel painted by exactly ONE tile under any invertible affine.
      if (!(q.x >= window.x0 && q.x < window.x1 && q.y >= window.y0 && q.y < window.y1)) {
        continue;
      }
      const double sx = q.x - 0.5;
      const double sy = q.y - 0.5;
      const int i0 = static_cast<int>(std::floor(sx));
      const int j0 = static_cast<int>(std::floor(sy));
      // Cull only when the whole 4x4 Catmull-Rom footprint (`i0-1 .. i0+2`,
      // `j0-1 .. j0+2`) is out of range -- a sample with no in-range tap
      // contributes nothing (transparent), matching the old nearest cull and
      // leaving `dst` untouched. A straddling footprint blends toward the
      // transparent border instead (antialiased falloff). The window widened
      // with the tap (bilinear's 2x2 -> the cubic's 4x4), but the cull rule is
      // the same: skip a fully-transparent sample, paint every straddling one.
      if (i0 + 2 < 0 || i0 - 1 >= src_width || j0 + 2 < 0 || j0 - 1 >= src_height) {
        continue;
      }
      const float fx = static_cast<float>(sx - i0);
      const float fy = static_cast<float>(sy - j0);
      // Catmull-Rom reconstruction over the zero-border fetch (the compositor's
      // edge convention, doc 07 -- transparent premultiplied zero, NOT the kind's
      // clamp-to-edge). `sample_bicubic` clamps its own negative-lobe undershoot.
      const WorkingPixel s = arbc::sample_bicubic(i0, j0, fx, fy, [&](int i, int j) {
        return fetch_texel<F>(src, src_width, src_height, i, j);
      });
      const std::size_t dst_at =
          stride * (static_cast<std::size_t>(y) * static_cast<std::size_t>(dst_width) +
                    static_cast<std::size_t>(x));
      const WorkingPixel d = Traits::decode(&dst.data[dst_at]);
      const float alpha = s[3] * opacity;
      WorkingPixel out{};
      if (blend == BlendMode::Normal) {
        for (std::size_t k = 0; k < Traits::channels; ++k) {
          out[k] = s[k] * opacity + (1.0F - alpha) * d[k];
        }
      } else {
        // The reference composite with a blend function (PDF 1.4 / CSS Compositing and
        // Blending Level 1), on premultiplied working floats:
        //
        //   co = (1 - ab)*as*cs  +  ab*as*B(cb, cs)  +  (1 - as)*ab*cb
        //   ao = as + ab*(1 - as)
        //
        // The three terms are the three coverage regions -- source only, both, backdrop
        // only -- and only the middle one consults the mode, which is why Normal (B = cs)
        // collapses the first two back into plain source-over.
        //
        // `B` is defined on UNPREMULTIPLIED colour, so each channel is divided out first.
        // OPACITY CANCELS in that division (`s[k]*opacity / (s[3]*opacity)`), which is the
        // correct reading and not an accident of the algebra: opacity scales how much of
        // the layer lands, never what colour it is, so turning a multiply layer down must
        // fade it toward the backdrop rather than toward a different multiply.
        const float ab = d[3];
        const float inv_as = s[3] > 0.0F ? 1.0F / s[3] : 0.0F;
        const float inv_ab = ab > 0.0F ? 1.0F / ab : 0.0F;
        for (std::size_t k = 0; k + 1 < Traits::channels; ++k) {
          const float cs = s[k] * inv_as;
          const float cb = d[k] * inv_ab;
          const float mixed = blend_channel(blend, cb, cs);
          out[k] = ((1.0F - ab) * alpha * cs) + (ab * alpha * mixed) + ((1.0F - alpha) * ab * cb);
        }
        out[Traits::channels - 1] = alpha + (ab * (1.0F - alpha));
      }
      Traits::encode(out, &dst.data[dst_at]);
    }
  }
}

// Lanczos-3 exact 2:1 downsample (doc 09:18 backend-internal resample op): each
// destination pixel is the shared `arbc::media` half-band decimation of its 6x6
// source support, reduced in decoded premultiplied linear working floats and
// encoded once (doc 07 rule 3 -- averaging encoded sRGB bytes would be
// gamma-space-wrong). This is the same `decimate_half_band` the kinds' mip
// pyramid runs, so a scale-ladder rung the compositor builds is byte-identical
// to the mip a kind builds from the same source (doc 07 § Resampling filters).
// Exact 2:1, so `dst` dims are `src` dims / 2 with even source dims (rung tiles
// are power-of-two device pixels, doc 02:59). Same-format; the caller (the
// scale-ladder) generates coarser rungs with it. The zero-border `fetch` keeps
// the compositor's edge convention; the bank clamps the negative lobe once at
// the end. Fixed tap order for determinism (doc 16).
template <PixelFormat F>
void downsample_kernel(TypedSpan<F> dst, int dst_width, int dst_height,
                       std::span<const typename PixelTraits<F>::Storage> src, int src_width,
                       int src_height) {
  using Traits = PixelTraits<F>;
  const std::size_t stride = Traits::channels;
  for (int y = 0; y < dst_height; ++y) {
    for (int x = 0; x < dst_width; ++x) {
      const WorkingPixel m = arbc::decimate_half_band(
          x, y, [&](int i, int j) { return fetch_texel<F>(src, src_width, src_height, i, j); });
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
