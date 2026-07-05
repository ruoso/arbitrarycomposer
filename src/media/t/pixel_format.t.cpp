#include <arbc/media/color_space.hpp>
#include <arbc/media/pixel_format.hpp>
#include <arbc/media/surface_format.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string_view>

namespace {

// The closed core-owned format set (doc 07); every descriptor helper is
// exercised over all of it.
constexpr arbc::PixelFormat k_all_formats[] = {
    arbc::PixelFormat::Rgba32fLinearPremul,
    arbc::PixelFormat::Rgba16fLinearPremul,
    arbc::PixelFormat::Rgba8Srgb,
};

TEST_CASE("bytes_per_pixel is 16 / 8 / 4 across the closed set") {
  REQUIRE(arbc::bytes_per_pixel(arbc::PixelFormat::Rgba32fLinearPremul) == 16);
  REQUIRE(arbc::bytes_per_pixel(arbc::PixelFormat::Rgba16fLinearPremul) == 8);
  REQUIRE(arbc::bytes_per_pixel(arbc::PixelFormat::Rgba8Srgb) == 4);

  // Constexpr-evaluable (descriptors are compile-time value queries).
  static_assert(arbc::bytes_per_pixel(arbc::PixelFormat::Rgba32fLinearPremul) == 16);
  static_assert(arbc::bytes_per_pixel(arbc::PixelFormat::Rgba8Srgb) == 4);
}

TEST_CASE("every format is 4-channel RGBA") {
  for (const arbc::PixelFormat format : k_all_formats) {
    REQUIRE(arbc::channels_per_pixel(format) == 4);
  }
  static_assert(arbc::channels_per_pixel(arbc::PixelFormat::Rgba16fLinearPremul) == 4);
}

TEST_CASE("is_float distinguishes the float working formats from the 8-bit mode") {
  REQUIRE(arbc::is_float(arbc::PixelFormat::Rgba32fLinearPremul));
  REQUIRE(arbc::is_float(arbc::PixelFormat::Rgba16fLinearPremul));
  REQUIRE_FALSE(arbc::is_float(arbc::PixelFormat::Rgba8Srgb));
  static_assert(arbc::is_float(arbc::PixelFormat::Rgba16fLinearPremul));
  static_assert(!arbc::is_float(arbc::PixelFormat::Rgba8Srgb));
}

TEST_CASE("bytes_per_pixel agrees with channel count for the float formats") {
  // 32f: 4 channels x 4 bytes; 16f: 4 channels x 2 bytes.
  REQUIRE(arbc::bytes_per_pixel(arbc::PixelFormat::Rgba32fLinearPremul) ==
          arbc::channels_per_pixel(arbc::PixelFormat::Rgba32fLinearPremul) * 4);
  REQUIRE(arbc::bytes_per_pixel(arbc::PixelFormat::Rgba16fLinearPremul) ==
          arbc::channels_per_pixel(arbc::PixelFormat::Rgba16fLinearPremul) * 2);
}

TEST_CASE("to_string names every format in the closed set") {
  REQUIRE(std::string_view(arbc::to_string(arbc::PixelFormat::Rgba32fLinearPremul)) ==
          "rgba32f-linear-premul");
  REQUIRE(std::string_view(arbc::to_string(arbc::PixelFormat::Rgba16fLinearPremul)) ==
          "rgba16f-linear-premul");
  REQUIRE(std::string_view(arbc::to_string(arbc::PixelFormat::Rgba8Srgb)) == "rgba8-srgb");

  // No member falls through to the "unknown" sentinel.
  for (const arbc::PixelFormat format : k_all_formats) {
    REQUIRE(std::string_view(arbc::to_string(format)) != "unknown");
  }
}

TEST_CASE("ColorSpace equality is member-wise") {
  REQUIRE(arbc::k_linear_srgb == arbc::ColorSpace{arbc::Primaries::Srgb,
                                                  arbc::TransferFunction::Linear});
  REQUIRE_FALSE(arbc::k_linear_srgb == arbc::k_srgb);
  REQUIRE(arbc::k_srgb.transfer == arbc::TransferFunction::Srgb);
  static_assert(arbc::k_linear_srgb == arbc::ColorSpace{});
}

TEST_CASE("SurfaceFormat has value equality and copy semantics") {
  constexpr arbc::SurfaceFormat a = arbc::k_working_rgba32f;
  arbc::SurfaceFormat b = a; // copy
  REQUIRE(a == b);
  REQUIRE(b.pixel_format == arbc::PixelFormat::Rgba32fLinearPremul);
  REQUIRE(b.color_space == arbc::k_linear_srgb);
  REQUIRE(b.premultiplied == arbc::Premultiplied::Yes);

  // Any single tag differing breaks equality -- each tag is load-bearing.
  b.premultiplied = arbc::Premultiplied::No;
  REQUIRE_FALSE(a == b);

  arbc::SurfaceFormat c = a;
  c.color_space = arbc::k_srgb;
  REQUIRE_FALSE(a == c);

  arbc::SurfaceFormat d = a;
  d.pixel_format = arbc::PixelFormat::Rgba16fLinearPremul;
  REQUIRE_FALSE(a == d);

  static_assert(arbc::k_working_rgba32f == arbc::SurfaceFormat{});
}

} // namespace
