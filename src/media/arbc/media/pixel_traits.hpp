#pragma once

#include <arbc/media/pixel_format.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace arbc {

// The compositor's working sample (doc 07): premultiplied linear-light RGBA in
// four 32-bit floats. Every kernel operates on this; each PixelTraits knows how
// to decode its storage into a WorkingPixel and encode one back, so kernels are
// written once and monomorphized per format (doc 07's "templates inside").
using WorkingPixel = std::array<float, 4>;

// --- Premultiplication (doc 07 rule 2) -------------------------------------
//
// The working space is premultiplied; some storage formats (8-bit sRGB) hold
// straight alpha, so their codecs cross the boundary here. Factored out so the
// premultiply step is one tested definition rather than inlined per codec.

inline WorkingPixel premultiply(const WorkingPixel& straight) {
  const float a = straight[3];
  return {straight[0] * a, straight[1] * a, straight[2] * a, a};
}

inline WorkingPixel unpremultiply(const WorkingPixel& premul) {
  const float a = premul[3];
  const float inv = a > 0.0F ? 1.0F / a : 0.0F;
  return {premul[0] * inv, premul[1] * inv, premul[2] * inv, a};
}

// --- sRGB transfer (doc 07 rule 3) -----------------------------------------
//
// The 8-bit fast mode stores sRGB-encoded color; decode applies the EOTF to
// linear working light, encode the inverse with round-to-nearest. The pair
// round-trips exactly across all 256 codes per channel (exhaustive test) --
// the "fast mode" trades memory, never blends in gamma space.

inline float srgb8_to_linear(std::uint8_t sample) {
  const float c = static_cast<float>(sample) / 255.0F;
  return c <= 0.04045F ? c / 12.92F : std::pow((c + 0.055F) / 1.055F, 2.4F);
}

inline std::uint8_t linear_to_srgb8(float value) {
  const float clamped = std::clamp(value, 0.0F, 1.0F);
  const float s = clamped <= 0.0031308F ? clamped * 12.92F
                                        : 1.055F * std::pow(clamped, 1.0F / 2.4F) - 0.055F;
  return static_cast<std::uint8_t>(std::lround(s * 255.0F));
}

// Linear 8-bit alpha: a plain 0..1 quantization, round-to-nearest.
inline std::uint8_t unorm8_encode(float value) {
  return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
}

inline float unorm8_decode(std::uint8_t sample) { return static_cast<float>(sample) / 255.0F; }

// --- Software half-float (doc 07 decision: portable f16) --------------------
//
// Storage is std::uint16_t; conversion is branch-light bit manipulation with
// round-to-nearest-even, no reliance on _Float16/std::float16_t so the three
// target compilers agree bit-for-bit (doc 16 byte-exact discipline). SIMD is
// out of scope but the contiguous typed spans the kernels use do not preclude
// it later.

inline float f16_to_float(std::uint16_t half) {
  const std::uint32_t sign = static_cast<std::uint32_t>(half & 0x8000U) << 16;
  const std::uint32_t exp = (half >> 10) & 0x1FU;
  const std::uint32_t mant = half & 0x3FFU;
  std::uint32_t bits = 0;
  if (exp == 0) {
    if (mant != 0) {
      // Subnormal half: renormalize into a float normal.
      std::uint32_t m = mant;
      int shift = 0;
      while ((m & 0x400U) == 0) {
        m <<= 1;
        ++shift;
      }
      m &= 0x3FFU;
      // A half subnormal is mant * 2^-24; renormalizing to 1.f * 2^E gives
      // E = -14 - shift, so the biased float exponent is (127 - 14) - shift.
      const std::uint32_t e = 127U - 14U - static_cast<std::uint32_t>(shift);
      bits = sign | (e << 23) | (m << 13);
    } else {
      bits = sign; // signed zero
    }
  } else if (exp == 0x1FU) {
    bits = sign | 0x7F800000U | (mant << 13); // inf / NaN, payload preserved
  } else {
    bits = sign | ((exp + (127U - 15U)) << 23) | (mant << 13);
  }
  float out = 0.0F;
  std::memcpy(&out, &bits, sizeof out);
  return out;
}

inline std::uint16_t f16_from_float(float value) {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof bits);
  const std::uint16_t sign = static_cast<std::uint16_t>((bits >> 16) & 0x8000U);
  const std::uint32_t biased = (bits >> 23) & 0xFFU;
  const std::uint32_t mant = bits & 0x7FFFFFU;

  if (biased == 0xFFU) {
    // Inf or NaN. A nonzero mantissa stays NaN (quieted); Inf stays Inf.
    const std::uint16_t payload = mant != 0 ? 0x200U : 0U;
    return static_cast<std::uint16_t>(sign | 0x7C00U | payload);
  }

  const std::int32_t exp = static_cast<std::int32_t>(biased) - 127 + 15;
  if (exp >= 0x1F) {
    return static_cast<std::uint16_t>(sign | 0x7C00U); // overflow to infinity
  }
  if (exp <= 0) {
    if (exp < -10) {
      return sign; // magnitude rounds to zero
    }
    // Subnormal half: shift the implicit-1 mantissa, round-to-nearest-even.
    const std::uint32_t full = mant | 0x800000U;
    const int shift = 14 - exp;
    const std::uint32_t quotient = full >> shift;
    const std::uint32_t remainder = full & ((1U << shift) - 1U);
    const std::uint32_t halfway = 1U << (shift - 1);
    std::uint32_t rounded = quotient;
    if (remainder > halfway || (remainder == halfway && (quotient & 1U) != 0)) {
      ++rounded;
    }
    return static_cast<std::uint16_t>(sign | rounded);
  }
  // Normalized: keep the top 10 mantissa bits, round-to-nearest-even. A carry
  // out of the mantissa flows into the exponent, which is exactly correct
  // (and if it reaches 0x1F the result is Inf, also correct).
  const std::uint32_t half_mant = mant >> 13;
  const std::uint32_t remainder = mant & 0x1FFFU;
  std::uint32_t out = (static_cast<std::uint32_t>(exp) << 10) | half_mant;
  if (remainder > 0x1000U || (remainder == 0x1000U && (half_mant & 1U) != 0)) {
    ++out;
  }
  return static_cast<std::uint16_t>(sign | out);
}

// --- Per-format storage + codec descriptors (doc 07) ------------------------
//
// PixelTraits<F> is the compile-time descriptor a kernel template is
// monomorphized over: the storage type of one sample, and the codec that
// routes storage <-> the premultiplied linear working space. Conversions go
// format -> working -> format, so N formats need 2N codecs, not N*N pairs
// (doc 07's "conversions route through the working space"). Primary template
// is left undefined so an unhandled format is a hard compile error, never a
// runtime hole.
template <PixelFormat F>
struct PixelTraits;

template <>
struct PixelTraits<PixelFormat::Rgba32fLinearPremul> {
  using Storage = float;
  static constexpr PixelFormat format = PixelFormat::Rgba32fLinearPremul;
  static constexpr std::size_t channels = 4;

  // Storage already *is* the working sample: the codec is identity, which is
  // why 32f goldens stay byte-identical through the kernel path.
  static WorkingPixel decode(const Storage* px) { return {px[0], px[1], px[2], px[3]}; }
  static void encode(const WorkingPixel& c, Storage* px) {
    px[0] = c[0];
    px[1] = c[1];
    px[2] = c[2];
    px[3] = c[3];
  }
};

template <>
struct PixelTraits<PixelFormat::Rgba16fLinearPremul> {
  using Storage = std::uint16_t;
  static constexpr PixelFormat format = PixelFormat::Rgba16fLinearPremul;
  static constexpr std::size_t channels = 4;

  // Premultiplied linear half: only the storage width changes, so the codec is
  // pure f16 <-> f32 conversion, no premultiply/transfer step.
  static WorkingPixel decode(const Storage* px) {
    return {f16_to_float(px[0]), f16_to_float(px[1]), f16_to_float(px[2]), f16_to_float(px[3])};
  }
  static void encode(const WorkingPixel& c, Storage* px) {
    for (std::size_t k = 0; k < channels; ++k) {
      px[k] = f16_from_float(c[k]);
    }
  }
};

template <>
struct PixelTraits<PixelFormat::Rgba8Srgb> {
  using Storage = std::uint8_t;
  static constexpr PixelFormat format = PixelFormat::Rgba8Srgb;
  static constexpr std::size_t channels = 4;

  // Straight-alpha sRGB storage: decode un-gammas the color to linear, reads
  // alpha linearly, then premultiplies into the working space; encode is the
  // exact inverse. Premultiplying 8-bit gamma samples would be doubly wrong
  // (gamma-space multiply + precision loss), so the fast mode stores straight.
  static WorkingPixel decode(const Storage* px) {
    const WorkingPixel straight{srgb8_to_linear(px[0]), srgb8_to_linear(px[1]),
                                srgb8_to_linear(px[2]), unorm8_decode(px[3])};
    return premultiply(straight);
  }
  static void encode(const WorkingPixel& c, Storage* px) {
    const WorkingPixel straight = unpremultiply(c);
    px[0] = linear_to_srgb8(straight[0]);
    px[1] = linear_to_srgb8(straight[1]);
    px[2] = linear_to_srgb8(straight[2]);
    px[3] = unorm8_encode(straight[3]);
  }
};

} // namespace arbc
