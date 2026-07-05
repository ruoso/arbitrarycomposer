#include <arbc/media/pixel_traits.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <limits>

namespace {

using arbc::f16_from_float;
using arbc::f16_to_float;

// One f16 ULP at `value`: the gap between adjacent representable halves in
// `value`'s binade. Used to bound the float -> half quantization error.
float half_ulp(float value) {
  const std::uint16_t h = f16_from_float(value);
  const auto neighbor = static_cast<std::uint16_t>(h + 1U);
  return std::fabs(f16_to_float(neighbor) - f16_to_float(h));
}

} // namespace

// The 8-bit fast mode trades memory, never precision on the codes it stores:
// the sRGB transfer and the linear-alpha quantizer both round-trip every one of
// the 256 codes exactly, so encode(decode(x)) == x per channel.
// enforces: 07-color-and-pixel-formats#srgb8-round-trips-exactly
TEST_CASE("the 8-bit sRGB codec round-trips all 256 codes per channel exactly") {
  for (int code = 0; code < 256; ++code) {
    const auto x = static_cast<std::uint8_t>(code);
    CAPTURE(code);
    // Color channels: sRGB EOTF then its inverse returns the same code.
    REQUIRE(arbc::linear_to_srgb8(arbc::srgb8_to_linear(x)) == x);
    // Alpha: plain unorm quantization also round-trips every code.
    REQUIRE(arbc::unorm8_encode(arbc::unorm8_decode(x)) == x);
  }
}

// Software half-float conversion is portable (no _Float16 reliance) and correct
// on the IEEE edge cases the reference table names, and every representable half
// round-trips half -> float -> half exactly.
// enforces: 07-color-and-pixel-formats#f16-conversion-portable-and-exact
TEST_CASE("software f16 conversion is exact on the edge cases and round-trips") {
  SECTION("signed zero") {
    REQUIRE(f16_from_float(0.0F) == 0x0000U);
    REQUIRE(f16_from_float(-0.0F) == 0x8000U);
    REQUIRE(f16_to_float(0x0000U) == 0.0F);
    REQUIRE_FALSE(std::signbit(f16_to_float(0x0000U)));
    REQUIRE(f16_to_float(0x8000U) == 0.0F);
    REQUIRE(std::signbit(f16_to_float(0x8000U))); // negative zero preserved
  }

  SECTION("exact powers: 1.0 and 0.5") {
    REQUIRE(f16_from_float(1.0F) == 0x3C00U);
    REQUIRE(f16_from_float(0.5F) == 0x3800U);
    REQUIRE(f16_to_float(0x3C00U) == 1.0F);
    REQUIRE(f16_to_float(0x3800U) == 0.5F);
  }

  SECTION("infinities") {
    const float inf = std::numeric_limits<float>::infinity();
    REQUIRE(f16_from_float(inf) == 0x7C00U);
    REQUIRE(f16_from_float(-inf) == 0xFC00U);
    REQUIRE(f16_to_float(0x7C00U) == inf);
    REQUIRE(f16_to_float(0xFC00U) == -inf);
  }

  SECTION("NaN maps to a NaN half and back, payload nonzero") {
    const std::uint16_t nan_half = f16_from_float(std::numeric_limits<float>::quiet_NaN());
    REQUIRE((nan_half & 0x7C00U) == 0x7C00U); // exponent all ones
    REQUIRE((nan_half & 0x03FFU) != 0U);      // nonzero mantissa -> NaN, not inf
    REQUIRE(std::isnan(f16_to_float(nan_half)));
  }

  SECTION("subnormals: smallest, largest, and a spread all round-trip") {
    // The smallest positive half subnormal is exactly 2^-24.
    REQUIRE(f16_to_float(0x0001U) == std::ldexp(1.0F, -24));
    // Largest subnormal is 1023 * 2^-24, just below the smallest normal 2^-14.
    REQUIRE(f16_to_float(0x03FFU) == 1023.0F * std::ldexp(1.0F, -24));
    REQUIRE(f16_to_float(0x0400U) == std::ldexp(1.0F, -14)); // smallest normal

    for (const std::uint16_t h : {0x0001U, 0x0002U, 0x0100U, 0x0200U, 0x03FFU, 0x8001U, 0x83FFU}) {
      CAPTURE(h);
      REQUIRE(f16_from_float(f16_to_float(h)) == h);
    }
  }

  SECTION("every non-NaN representable half round-trips half -> float -> half") {
    for (int code = 0; code <= 0xFFFF; ++code) {
      const auto h = static_cast<std::uint16_t>(code);
      const bool is_nan = (h & 0x7C00U) == 0x7C00U && (h & 0x03FFU) != 0U;
      if (is_nan) {
        continue; // NaN payloads quiet, not required to round-trip bit-exactly
      }
      CAPTURE(code);
      REQUIRE(f16_from_float(f16_to_float(h)) == h);
    }
  }

  SECTION("float -> half quantization stays within one f16 ULP") {
    for (const float v : {0.1F, 0.3F, 0.6F, 1.0F / 3.0F, 123.456F, 0.001F}) {
      CAPTURE(v);
      const float round_tripped = f16_to_float(f16_from_float(v));
      REQUIRE(std::fabs(round_tripped - v) <= half_ulp(v));
    }
  }
}
