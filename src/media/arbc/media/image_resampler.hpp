#pragma once

#include <arbc/media/pixel_traits.hpp> // WorkingPixel

#include <algorithm>
#include <array>
#include <cstddef>

namespace arbc {

// Deterministic, format-agnostic image resampling filters (doc 07 "Resampling
// filters"; doc 16:48-53) -- the raster twin of `audio_resampler`'s windowed-sinc
// polyphase bank, and for the same reason: the filters live in `media` (doc 17)
// because `kind_raster`'s mip pyramid and `backend_cpu`'s compositing kernels sit
// on opposite sides of an un-crossable level boundary, and `media` is the only
// floor below both. Duplicating a frozen coefficient table across that boundary
// is exactly what this placement prevents.
//
// MINIFICATION uses a fixed 6-tap Lanczos-3 half-band decimator; MAGNIFICATION
// uses an interpolating Catmull-Rom bicubic tap. Both operate on decoded
// premultiplied linear working floats (doc 07 rule 3) with tabulated float32
// weights, a fixed tap order, and no runtime `libm` -- so every resample is a
// byte-exact deterministic function of its taps, portable across toolchains.
// Both kernels' negative lobes ring below zero at a hard edge; the result is
// clamped to non-negative per channel, which removes the unphysical undershoot
// (and the negative alpha that would break unpremultiplication) while leaving the
// working space's HDR headroom above alpha intact.
//
// The tap FETCH is the caller's: these kernels name no `PixelFormat`, no
// `Surface`, and no pool -- they take a `Fetch` callable returning a
// `WorkingPixel` for a source coordinate, so the edge convention (clamp-to-edge
// for a kind's mip pyramid, zero-border for a compositor texel fetch) is decided
// by the consumer, not baked in here.
//
// No runtime quality knob (Constraint 7): one decimator, one magnifier, both
// compiled in. Tap count, window, and phase convention are recorded beside the
// coefficient table in `image_resampler.cpp`.

// --- Lanczos-3 half-band 2:1 decimator ---------------------------------------
//
// At a 2:1 rung the destination center maps to source `u = 2x + 0.5`, so the
// fractional phase is CONSTANT at 0.5 for every output pixel and a windowed-sinc
// decimator collapses to a single fixed 6-tap FIR (taps at offsets +-0.5, +-1.5,
// +-2.5) rather than a polyphase bank. Tap `i` of parent `x` reads child pixel
// `2x + k_decimate_first_tap + i`, i.e. the support spans child `2x-2 .. 2x+3`.

inline constexpr int k_decimate_taps = 6;
inline constexpr int k_decimate_first_tap = -2;

// The conservative outward dilation, in child pixels, that covers the support on
// both sides (the support reaches 2 left and 3 right of the 2:1 footprint). A
// consumer doing an INCREMENTAL rung recompute must dilate its dirty region by
// this before selecting which parent pixels to rebuild, or it leaves a stale
// filtered band around the changed region (doc 14).
inline constexpr int k_decimate_radius = 3;

// The frozen half-band weight table, in tap order (child `2x-2` .. `2x+3`).
// Symmetric: `w[i] == w[5 - i]` bit-for-bit.
const std::array<float, k_decimate_taps>& lanczos3_half_band();

// The float32 DC gain of the frozen bank, reduced in the SAME fixed tap order the
// kernel uses. Exactly `1.0F` -- so a constant child field decimates to that
// constant (pinned in the unit test).
float lanczos3_half_band_dc_gain();

// Clamp each channel to non-negative -- the negative-lobe undershoot guard. RGB is
// deliberately NOT clamped to <= alpha: above-alpha values are legitimate HDR
// headroom in a float linear-premultiplied working space (doc 07:25-31), and each
// format's `encode` already clamps on the way to storage.
inline WorkingPixel clamp_non_negative(const WorkingPixel& c) {
  WorkingPixel out{};
  for (std::size_t k = 0; k < out.size(); ++k) {
    out[k] = std::max(0.0F, c[k]);
  }
  return out;
}

// Reduce one line of six half-band taps (child `2x-2` .. `2x+3`, in that order)
// into one parent sample. UNCLAMPED -- the separable 2D kernel clamps once, after
// both passes; clamping an intermediate row would change the result.
//
// The tap order is a symmetric-pair fold, centre-out. It is fixed and load-bearing,
// not an optimization: because `w[i] == w[5-i]` exactly, folding the tap PAIRS
// before the multiply is what makes the float32 DC gain land on exactly 1.0 (a
// plain left-to-right 6-tap MAC lands one ulp short, so a flat field would drift).
inline WorkingPixel decimate_line(const std::array<WorkingPixel, k_decimate_taps>& p) {
  const std::array<float, k_decimate_taps>& w = lanczos3_half_band();
  WorkingPixel out{};
  for (std::size_t k = 0; k < out.size(); ++k) {
    float acc = w[2] * (p[2][k] + p[3][k]);
    acc += w[1] * (p[1][k] + p[4][k]);
    acc += w[0] * (p[0][k] + p[5][k]);
    out[k] = acc;
  }
  return out;
}

// Separable 2:1 decimation: the parent pixel `(dst_x, dst_y)` of the rung above
// the child level `fetch` reads. `fetch(x, y)` returns the child `WorkingPixel` at
// child coordinates (x, y) and OWNS THE EDGE CONVENTION -- the kernel will ask for
// coordinates outside the child level (the support reaches 2 left / 3 right of the
// footprint), and it is the caller that decides whether those clamp to the border
// or read a zero surround.
template <class Fetch> WorkingPixel decimate_half_band(int dst_x, int dst_y, Fetch&& fetch) {
  std::array<WorkingPixel, k_decimate_taps> rows{};
  for (int j = 0; j < k_decimate_taps; ++j) {
    const int cy = 2 * dst_y + k_decimate_first_tap + j;
    std::array<WorkingPixel, k_decimate_taps> cols{};
    for (int i = 0; i < k_decimate_taps; ++i) {
      cols[static_cast<std::size_t>(i)] = fetch(2 * dst_x + k_decimate_first_tap + i, cy);
    }
    rows[static_cast<std::size_t>(j)] = decimate_line(cols);
  }
  return clamp_non_negative(decimate_line(rows));
}

// --- Catmull-Rom bicubic magnifier -------------------------------------------
//
// B=0, C=1/2. Its basis is an exact rational cubic evaluated in closed form, so it
// needs NO table and NO libm. Decisively, it is INTERPOLATING: at integer phase
// (`t == 0`) the weights are exactly `(0, 1, 0, 0)` in IEEE float, so the filter
// reproduces the source sample BIT-FOR-BIT -- which is why swapping bilinear for
// it moves no native-scale and no on-rung golden byte. That collapse-to-the-source
// property is the guard, not a coincidence: an approximating cubic
// (Mitchell-Netravali) would fail it and a native-scale render would stop
// returning the source pixels.

inline constexpr int k_magnify_taps = 4;

// Tap `i` of `x0 = floor(u)` reads source pixel `x0 + k_magnify_first_tap + i`,
// i.e. `x0-1 .. x0+2`.
inline constexpr int k_magnify_first_tap = -1;

inline std::array<float, k_magnify_taps> catmull_rom_weights(float t) {
  const float t2 = t * t;
  const float t3 = t2 * t;
  return {-0.5F * t3 + t2 - 0.5F * t, 1.5F * t3 - 2.5F * t2 + 1.0F,
          -1.5F * t3 + 2.0F * t2 + 0.5F * t, 0.5F * t3 - 0.5F * t2};
}

// Reduce one line of four taps (source `x0-1` .. `x0+2`, in that order) with `w`.
// UNCLAMPED, for the same reason `decimate_line` is. The MAC runs in tap order, so
// at `w == (0, 1, 0, 0)` it returns `p[1]` exactly.
inline WorkingPixel magnify_line(const std::array<WorkingPixel, k_magnify_taps>& p,
                                 const std::array<float, k_magnify_taps>& w) {
  WorkingPixel out{};
  for (std::size_t k = 0; k < out.size(); ++k) {
    float acc = w[0] * p[0][k];
    acc += w[1] * p[1][k];
    acc += w[2] * p[2][k];
    acc += w[3] * p[3][k];
    out[k] = acc;
  }
  return out;
}

// Separable Catmull-Rom tap at source position `(x0 + fx, y0 + fy)`, where
// `x0 = floor(u)` and `fx = u - x0` in `[0, 1)`. `fetch(x, y)` returns the source
// `WorkingPixel` and owns the edge convention (see `decimate_half_band`).
template <class Fetch>
WorkingPixel sample_bicubic(int x0, int y0, float fx, float fy, Fetch&& fetch) {
  const std::array<float, k_magnify_taps> wx = catmull_rom_weights(fx);
  const std::array<float, k_magnify_taps> wy = catmull_rom_weights(fy);
  std::array<WorkingPixel, k_magnify_taps> rows{};
  for (int j = 0; j < k_magnify_taps; ++j) {
    const int sy = y0 + k_magnify_first_tap + j;
    std::array<WorkingPixel, k_magnify_taps> cols{};
    for (int i = 0; i < k_magnify_taps; ++i) {
      cols[static_cast<std::size_t>(i)] = fetch(x0 + k_magnify_first_tap + i, sy);
    }
    rows[static_cast<std::size_t>(j)] = magnify_line(cols, wx);
  }
  return clamp_non_negative(magnify_line(rows, wy));
}

} // namespace arbc
