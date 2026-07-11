#include <arbc/media/image_resampler.hpp>
#include <arbc/media/pixel_traits.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <span>
#include <vector>

// The filter bank's own anchor, independent of any consumer's render path
// (mirrors src/media/t/audio_resampler.t.cpp). Two layers:
//
//   - semantic properties: the magnifier is INTERPOLATING (integer phase
//     reproduces the source bit-for-bit), the decimator's DC gain is exactly
//     1.0F, both are decisively NOT the bilinear/box baseline they replace, the
//     negative lobes ring and the ring is clamped, and both are deterministic;
//   - a byte-exact frozen golden for each kernel, with no tolerance.
//
// enforces: 07-color-and-pixel-formats#resampling-uses-higher-order-filters
// enforces: 16-sdlc-and-quality#byte-exact-goldens
//
// REGENERATE PROCEDURE (doc 16 tier-3): an intended filter change deliberately
// re-freezes the coefficient table AND these goldens; they never regenerate
// silently. The hidden dump case prints both, paste-ready:
//
//     cmake --build --preset dev --target arbc_media_t
//     ./build/dev/src/media/arbc_media_t "[.regen]"
//
// Unlike the audio bank (whose [.regen] dumps only the output golden), this one
// also recomputes and dumps the COEFFICIENT TABLE with libm, so the frozen bank in
// image_resampler.cpp has a live in-repo regeneration path.

namespace {

using namespace arbc;

constexpr int k_src = 8;

// The fixed 8x8 working field: a 1-pixel checkerboard -- the high-frequency
// component a box filter aliases into mush and Lanczos-3 does not -- riding a
// coarse ramp, with a full-alpha channel. Every constant is an exactly
// representable binary fraction, so the fixture itself is byte-stable.
WorkingPixel src_pixel(int x, int y) {
  const float chk = ((x + y) % 2 == 0) ? 0.75F : 0.25F;
  const float ramp = static_cast<float>(x) * 0.0625F + static_cast<float>(y) * 0.03125F;
  return {chk, ramp, chk * 0.5F, 1.0F};
}

// Clamp-to-edge tap fetch (a mip pyramid's convention: taps outside the level read
// the border pixel, never a zero surround).
WorkingPixel fetch_clamped(int x, int y) {
  return src_pixel(std::clamp(x, 0, k_src - 1), std::clamp(y, 0, k_src - 1));
}

// Zero-border tap fetch (a compositor texel fetch's convention) -- used only to
// show the two conventions genuinely diverge at a border.
WorkingPixel fetch_zero_border(int x, int y) {
  if (x < 0 || x >= k_src || y < 0 || y >= k_src) {
    return WorkingPixel{0.0F, 0.0F, 0.0F, 0.0F};
  }
  return src_pixel(x, y);
}

// The baselines this task replaces, written out locally so the discriminators can
// prove the new kernels are decisively not them.
WorkingPixel box_2x2(int dst_x, int dst_y) {
  const WorkingPixel a = fetch_clamped(2 * dst_x, 2 * dst_y);
  const WorkingPixel b = fetch_clamped(2 * dst_x + 1, 2 * dst_y);
  const WorkingPixel c = fetch_clamped(2 * dst_x, 2 * dst_y + 1);
  const WorkingPixel d = fetch_clamped(2 * dst_x + 1, 2 * dst_y + 1);
  WorkingPixel out{};
  for (std::size_t k = 0; k < out.size(); ++k) {
    out[k] = (a[k] + b[k] + c[k] + d[k]) * 0.25F;
  }
  return out;
}

WorkingPixel bilerp(int x0, int y0, float fx, float fy) {
  const WorkingPixel a = fetch_clamped(x0, y0);
  const WorkingPixel b = fetch_clamped(x0 + 1, y0);
  const WorkingPixel c = fetch_clamped(x0, y0 + 1);
  const WorkingPixel d = fetch_clamped(x0 + 1, y0 + 1);
  WorkingPixel out{};
  for (std::size_t k = 0; k < out.size(); ++k) {
    const float top = a[k] + (b[k] - a[k]) * fx;
    const float bot = c[k] + (d[k] - c[k]) * fx;
    out[k] = top + (bot - top) * fy;
  }
  return out;
}

} // namespace

// enforces: 07-color-and-pixel-formats#resampling-uses-higher-order-filters
TEST_CASE("Catmull-Rom is interpolating: integer phase reproduces the source bit-for-bit") {
  const std::array<float, k_magnify_taps> w = catmull_rom_weights(0.0F);
  REQUIRE(w[0] == 0.0F);
  REQUIRE(w[1] == 1.0F);
  REQUIRE(w[2] == 0.0F);
  REQUIRE(w[3] == 0.0F);

  // The whole-field consequence: at t == 0 the tap reduction returns p[1] exactly,
  // so a native-scale / on-rung fetch is bit-identical to the stored sample. This
  // is the "collapse to the baseline at the trivial case" guard that lets every
  // pre-existing integer-phase golden survive the bilinear -> bicubic swap.
  for (int y = 0; y < k_src; ++y) {
    for (int x = 0; x < k_src; ++x) {
      REQUIRE(sample_bicubic(x, y, 0.0F, 0.0F, fetch_clamped) == src_pixel(x, y));
    }
  }
}

// enforces: 07-color-and-pixel-formats#resampling-uses-higher-order-filters
TEST_CASE("the magnifier is decisively not a bilinear tap at fractional phase") {
  // A cubic with negative lobes cannot agree with a 4-tap bilinear on a field that
  // is not locally affine; the checkerboard guarantees it is not.
  bool any_differs = false;
  for (int y = 1; y < k_src - 2; ++y) {
    for (int x = 1; x < k_src - 2; ++x) {
      const WorkingPixel cubic = sample_bicubic(x, y, 0.25F, 0.75F, fetch_clamped);
      const WorkingPixel linear = bilerp(x, y, 0.25F, 0.75F);
      if (cubic != linear) {
        any_differs = true;
      }
    }
  }
  REQUIRE(any_differs);
  // And it is not merely different somewhere -- it differs at a named interior
  // sample, so a degenerate "return the bilinear result" cannot satisfy this.
  // The phase matters: at exactly (0.5, 0.5) a 1-px checkerboard is symmetric under
  // both kernels and they legitimately COINCIDE, so the discriminator is pinned at
  // an asymmetric phase (cubic 0.618164 vs bilinear 0.562500 on the chroma channel).
  REQUIRE(sample_bicubic(3, 3, 0.25F, 0.25F, fetch_clamped) != bilerp(3, 3, 0.25F, 0.25F));
}

// enforces: 07-color-and-pixel-formats#resampling-uses-higher-order-filters
TEST_CASE("the decimator is decisively not a 2x2 box average on a high-frequency field") {
  bool any_differs = false;
  for (int y = 0; y < k_src / 2; ++y) {
    for (int x = 0; x < k_src / 2; ++x) {
      if (decimate_half_band(x, y, fetch_clamped) != box_2x2(x, y)) {
        any_differs = true;
      }
    }
  }
  REQUIRE(any_differs);
  REQUIRE(decimate_half_band(1, 1, fetch_clamped) != box_2x2(1, 1));
}

// enforces: 07-color-and-pixel-formats#resampling-uses-higher-order-filters
TEST_CASE("the frozen half-band bank is symmetric and has exactly unity DC gain") {
  const std::array<float, k_decimate_taps>& w = lanczos3_half_band();

  // Bit-exact symmetry -- the kernel's symmetric-pair-folded tap order depends on
  // it, and it is what keeps the filter phase-linear.
  REQUIRE(w[0] == w[5]);
  REQUIRE(w[1] == w[4]);
  REQUIRE(w[2] == w[3]);

  // The taps sum to exactly 1.0F in float32, reduced in the kernel's own tap order.
  REQUIRE(lanczos3_half_band_dc_gain() == 1.0F);

  // The consequence, and the reason the exact sum matters: a constant child field
  // decimates to that constant, with no drift.
  const auto flat = [](int, int) { return WorkingPixel{0.5F, 0.25F, 0.125F, 1.0F}; };
  REQUIRE(decimate_half_band(2, 2, flat) == WorkingPixel{0.5F, 0.25F, 0.125F, 1.0F});

  // Provenance: the six frozen constants really are Lanczos-3 (sinc(t)*sinc(t/3) at
  // the fixed half-band phases, normalized), not arbitrary numbers that happen to
  // sum to one. This compares the TABLE against its generator, so it is a
  // transcription guard -- not a tolerance on any rendered output (doc 16:48-53).
  constexpr double k_pi = 3.14159265358979323846;
  const auto sinc = [&](double t) { return t == 0.0 ? 1.0 : std::sin(k_pi * t) / (k_pi * t); };
  constexpr std::array<double, k_decimate_taps> offs{{-2.5, -1.5, -0.5, 0.5, 1.5, 2.5}};
  double sum = 0.0;
  std::array<double, k_decimate_taps> raw{};
  for (std::size_t i = 0; i < raw.size(); ++i) {
    raw[i] = sinc(offs[i]) * sinc(offs[i] / 3.0);
    sum += raw[i];
  }
  for (std::size_t i = 0; i < raw.size(); ++i) {
    REQUIRE(std::abs(static_cast<double>(w[i]) - raw[i] / sum) < 1e-6);
  }

  // The negative lobes are real: a windowed-sinc low-pass is not a blur kernel.
  REQUIRE(w[1] < 0.0F);
  REQUIRE(w[4] < 0.0F);
}

// enforces: 07-color-and-pixel-formats#resampling-uses-higher-order-filters
TEST_CASE("negative-lobe ringing at a hard edge is clamped to non-negative") {
  // A hard opaque/transparent step in x: the left half is transparent black, the
  // right half opaque white. This is the case that makes the clamp load-bearing --
  // an unclamped negative ALPHA would break unpremultiplication downstream.
  const auto step = [](int x, int) {
    return x < 4 ? WorkingPixel{0.0F, 0.0F, 0.0F, 0.0F} : WorkingPixel{1.0F, 1.0F, 1.0F, 1.0F};
  };

  // Parent pixel x = 1 reads child 0..5 = (0,0,0,0,1,1): the trailing lobe pulls
  // the reduction below zero. `decimate_line` is the UNCLAMPED reduction, so it
  // witnesses the undershoot directly -- the clamp is removing a real negative, not
  // guarding a case that never happens.
  std::array<WorkingPixel, k_decimate_taps> taps{};
  for (int i = 0; i < k_decimate_taps; ++i) {
    taps[static_cast<std::size_t>(i)] = step(2 * 1 + k_decimate_first_tap + i, 0);
  }
  const WorkingPixel unclamped = decimate_line(taps);
  REQUIRE(unclamped[0] < 0.0F); // colour undershoots
  REQUIRE(unclamped[3] < 0.0F); // ... and so does alpha

  // The shipped kernel clamps it away, per channel, to exactly zero.
  const WorkingPixel clamped = decimate_half_band(1, 2, step);
  for (std::size_t k = 0; k < clamped.size(); ++k) {
    REQUIRE(clamped[k] == 0.0F);
  }

  // The magnifier rings too, and is clamped the same way; no channel of any sample
  // across the step is ever negative.
  for (int dx = 0; dx < 16; ++dx) {
    const WorkingPixel s = sample_bicubic(dx / 2, 2, (dx % 2) == 0 ? 0.0F : 0.5F, 0.5F, step);
    for (std::size_t k = 0; k < s.size(); ++k) {
      REQUIRE(s[k] >= 0.0F);
    }
  }
}

TEST_CASE("the tap fetch owns the edge convention: clamp-to-edge is not a zero border") {
  // The kernels name no edge policy; the caller's `Fetch` decides. A zero surround
  // would darken every level border, which is why a mip pyramid passes a clamping
  // fetch -- and the two must genuinely differ, or the convention is not being
  // honoured.
  REQUIRE(decimate_half_band(0, 0, fetch_clamped) != decimate_half_band(0, 0, fetch_zero_border));
  REQUIRE(sample_bicubic(0, 0, 0.5F, 0.5F, fetch_clamped) !=
          sample_bicubic(0, 0, 0.5F, 0.5F, fetch_zero_border));

  // Interior samples, whose support never leaves the field, are identical under
  // both -- so the difference above is the edge policy and nothing else.
  REQUIRE(decimate_half_band(2, 2, fetch_clamped) == decimate_half_band(2, 2, fetch_zero_border));
}

TEST_CASE("both kernels are deterministic: the same taps produce the same bytes twice") {
  REQUIRE(decimate_half_band(1, 2, fetch_clamped) == decimate_half_band(1, 2, fetch_clamped));
  REQUIRE(sample_bicubic(3, 2, 0.25F, 0.75F, fetch_clamped) ==
          sample_bicubic(3, 2, 0.25F, 0.75F, fetch_clamped));
}

namespace {

// --- golden builders (the real shipped kernels) -----------------------------

// The 8x8 fixture decimated one rung, to 4x4.
std::vector<float> decimated_rung() {
  std::vector<float> out;
  for (int y = 0; y < k_src / 2; ++y) {
    for (int x = 0; x < k_src / 2; ++x) {
      const WorkingPixel p = decimate_half_band(x, y, fetch_clamped);
      out.insert(out.end(), p.begin(), p.end());
    }
  }
  return out;
}

// The fixture magnified 2x over its top-left 2x2 source pixels -- destination
// centres land at fractional phases 0.25/0.75 and the support reaches past the
// border, so this pins the magnifier at exactly the phases no pre-existing golden
// reaches.
std::vector<float> magnified_2x() {
  std::vector<float> out;
  for (int dy = 0; dy < 4; ++dy) {
    for (int dx = 0; dx < 4; ++dx) {
      const double u = (static_cast<double>(dx) + 0.5) / 2.0 - 0.5;
      const double v = (static_cast<double>(dy) + 0.5) / 2.0 - 0.5;
      const int x0 = static_cast<int>(std::floor(u));
      const int y0 = static_cast<int>(std::floor(v));
      const WorkingPixel p =
          sample_bicubic(x0, y0, static_cast<float>(u - x0), static_cast<float>(v - y0),
                         fetch_clamped);
      out.insert(out.end(), p.begin(), p.end());
    }
  }
  return out;
}

std::vector<std::byte> as_bytes(const std::vector<float>& v) {
  std::vector<std::byte> out(v.size() * sizeof(float));
  std::memcpy(out.data(), v.data(), out.size());
  return out;
}

void require_bytes(const std::vector<std::byte>& got, std::span<const unsigned char> want) {
  REQUIRE(got.size() == want.size());
  REQUIRE(std::memcmp(got.data(), want.data(), want.size()) == 0);
}

// ===========================================================================
// FROZEN EXPECTED GOLDENS -- regenerate deliberately (see procedure at top).
// ===========================================================================
constexpr std::array<unsigned char, 256> kDecimated{{0xD4, 0xB9, 0x04, 0x3F, 0xD4, 0x9B, 0x1E, 0x3D, 0xD4, 0xB9, 0x84, 0x3E, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x3F, 0xA8, 0x37, 0x2D, 0x3E, 0x00, 0x00, 0x80, 0x3E, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x3F, 0xD4, 0x9B, 0x96, 0x3E, 0x00, 0x00, 0x80, 0x3E, 0x00, 0x00, 0x80, 0x3F, 0x59, 0x8C, 0xF6, 0x3E, 0x2E, 0x64, 0xD9, 0x3E, 0x59, 0x8C, 0x76, 0x3E, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x3F, 0x9D, 0xDE, 0xD4, 0x3D, 0x00, 0x00, 0x80, 0x3E, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x3F, 0x01, 0x00, 0x70, 0x3E, 0x00, 0x00, 0x80, 0x3E, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x3F, 0x00, 0x00, 0xB8, 0x3E, 0x00, 0x00, 0x80, 0x3E, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x3F, 0x5A, 0xC8, 0xFA, 0x3E, 0x00, 0x00, 0x80, 0x3E, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x3F, 0x50, 0x6F, 0x2A, 0x3E, 0x00, 0x00, 0x80, 0x3E, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x98, 0x3E, 0x00, 0x00, 0x80, 0x3E, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x3F, 0x04, 0x00, 0xD8, 0x3E, 0x00, 0x00, 0x80, 0x3E, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x3F, 0x2C, 0x64, 0x0D, 0x3F, 0x00, 0x00, 0x80, 0x3E, 0x00, 0x00, 0x80, 0x3F, 0x59, 0x8C, 0xF6, 0x3E, 0xA9, 0x37, 0x6D, 0x3E, 0x59, 0x8C, 0x76, 0x3E, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x3F, 0x2D, 0x64, 0xB9, 0x3E, 0x00, 0x00, 0x80, 0x3E, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x3F, 0x2C, 0x64, 0xF9, 0x3E, 0x00, 0x00, 0x80, 0x3E, 0x00, 0x00, 0x80, 0x3F, 0xD4, 0xB9, 0x04, 0x3F, 0x43, 0x16, 0x1E, 0x3F, 0xD4, 0xB9, 0x84, 0x3E, 0x00, 0x00, 0x80, 0x3F}};
constexpr std::array<unsigned char, 256> kMagnified2x{{0x00, 0x44, 0x53, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x44, 0xD3, 0x3E, 0x00, 0x00, 0x80, 0x3F, 0x00, 0xEC, 0x27, 0x3F, 0x00, 0x00, 0x14, 0x3C, 0x00, 0xEC, 0xA7, 0x3E, 0x00, 0x00, 0x80, 0x3F, 0x00, 0xC8, 0x94, 0x3E, 0x00, 0x00, 0x31, 0x3D, 0x00, 0xC8, 0x14, 0x3E, 0x00, 0x00, 0x80, 0x3F, 0x00, 0xA0, 0x9B, 0x3E, 0x00, 0x80, 0x9B, 0x3D, 0x00, 0xA0, 0x1B, 0x3E, 0x00, 0x00, 0x80, 0x3F, 0x00, 0xEC, 0x27, 0x3F, 0x00, 0x00, 0xA0, 0x3A, 0x00, 0xEC, 0xA7, 0x3E, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x24, 0x13, 0x3F, 0x00, 0x00, 0x8A, 0x3C, 0x00, 0x24, 0x93, 0x3E, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x98, 0xCC, 0x3E, 0x00, 0x00, 0x51, 0x3D, 0x00, 0x98, 0x4C, 0x3E, 0x00, 0x00, 0x80, 0x3F, 0x00, 0xE0, 0xCF, 0x3E, 0x00, 0x80, 0xAB, 0x3D, 0x00, 0xE0, 0x4F, 0x3E, 0x00, 0x00, 0x80, 0x3F, 0x00, 0xC8, 0x94, 0x3E, 0x00, 0x00, 0x96, 0x3C, 0x00, 0xC8, 0x14, 0x3E, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x98, 0xCC, 0x3E, 0x00, 0x00, 0x0B, 0x3D, 0x00, 0x98, 0x4C, 0x3E, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x84, 0x22, 0x3F, 0x00, 0x80, 0x8B, 0x3D, 0x00, 0x84, 0xA2, 0x3E, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x50, 0x20, 0x3F, 0x00, 0x80, 0xCE, 0x3D, 0x00, 0x50, 0xA0, 0x3E, 0x00, 0x00, 0x80, 0x3F, 0x00, 0xA0, 0x9B, 0x3E, 0x00, 0x00, 0x0E, 0x3D, 0x00, 0xA0, 0x1B, 0x3E, 0x00, 0x00, 0x80, 0x3F, 0x00, 0xE0, 0xCF, 0x3E, 0x00, 0x00, 0x4E, 0x3D, 0x00, 0xE0, 0x4F, 0x3E, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x50, 0x20, 0x3F, 0x00, 0x00, 0xAD, 0x3D, 0x00, 0x50, 0xA0, 0x3E, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x40, 0x1E, 0x3F, 0x00, 0x00, 0xF0, 0x3D, 0x00, 0x40, 0x9E, 0x3E, 0x00, 0x00, 0x80, 0x3F}};
// ===========================================================================
// END FROZEN EXPECTED GOLDENS
// ===========================================================================

} // namespace

// enforces: 07-color-and-pixel-formats#resampling-uses-higher-order-filters
// enforces: 16-sdlc-and-quality#byte-exact-goldens
TEST_CASE("the image filter bank pins byte-exact output for a fixed field") {
  require_bytes(as_bytes(decimated_rung()), kDecimated);
  require_bytes(as_bytes(magnified_2x()), kMagnified2x);
}

// --- regeneration dump (hidden; excluded from coverage) ---------------------
// GCOV_EXCL_START -- maintenance tool, run only by the regenerate procedure.
namespace {

void dump_bytes(const char* name, const std::vector<std::byte>& b) {
  std::printf("constexpr std::array<unsigned char, %zu> %s{{", b.size(), name);
  for (std::size_t i = 0; i < b.size(); ++i) {
    std::printf("%s0x%02X", i ? ", " : "",
                static_cast<unsigned>(std::to_integer<unsigned char>(b[i])));
  }
  std::printf("}};\n");
}

// Recompute the frozen Lanczos-3 half-band bank from its generator (this is the
// ONLY place libm touches the filter -- the shipped kernel is table-driven) and
// print it paste-ready for image_resampler.cpp.
void dump_coefficients() {
  constexpr double k_pi = 3.14159265358979323846;
  const auto sinc = [&](double t) { return t == 0.0 ? 1.0 : std::sin(k_pi * t) / (k_pi * t); };
  constexpr std::array<double, k_decimate_taps> offs{{-2.5, -1.5, -0.5, 0.5, 1.5, 2.5}};
  std::array<double, k_decimate_taps> raw{};
  double sum = 0.0;
  for (std::size_t i = 0; i < raw.size(); ++i) {
    raw[i] = sinc(offs[i]) * sinc(offs[i] / 3.0);
    sum += raw[i];
  }
  std::printf("constexpr std::array<float, k_decimate_taps> k_lanczos3_half_band{{\n");
  for (std::size_t i = 0; i < raw.size(); ++i) {
    const auto w = static_cast<float>(raw[i] / sum);
    std::printf("    %af, // child 2x%+d  (offset %+.1f)  = %+.9f\n", static_cast<double>(w),
                k_decimate_first_tap + static_cast<int>(i), offs[i], static_cast<double>(w));
  }
  std::printf("}};\n");
}

} // namespace

TEST_CASE("dump image resampler coefficients and goldens", "[.regen]") {
  dump_coefficients();
  dump_bytes("kDecimated", as_bytes(decimated_rung()));
  dump_bytes("kMagnified2x", as_bytes(magnified_2x()));
}
// GCOV_EXCL_STOP
