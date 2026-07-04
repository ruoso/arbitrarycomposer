#include <arbc/backend_cpu/cpu_backend.hpp>

#include <catch2/catch_test_macros.hpp>

#include <utility>

namespace {

void fill(arbc::Surface& surface, float r, float g, float b, float a) {
  arbc::CpuBackend backend;
  backend.clear(surface, r, g, b, a);
}

TEST_CASE("identity composite over transparent copies the source") {
  arbc::CpuBackend backend;
  arbc::CpuSurface src(2, 2, arbc::PixelFormat::Rgba32fLinearPremul);
  fill(src, 1.0F, 0.0F, 0.0F, 1.0F);
  arbc::CpuSurface dst(2, 2, arbc::PixelFormat::Rgba32fLinearPremul);

  backend.composite(dst, src, arbc::Affine::identity(), 1.0);

  for (std::size_t i = 0; i + 3 < dst.cpu_pixels().size(); i += 4) {
    REQUIRE(dst.cpu_pixels()[i + 0] == 1.0F);
    REQUIRE(dst.cpu_pixels()[i + 1] == 0.0F);
    REQUIRE(dst.cpu_pixels()[i + 2] == 0.0F);
    REQUIRE(dst.cpu_pixels()[i + 3] == 1.0F);
  }
}

// enforces: 07-color-and-pixel-formats#premultiplied-source-over
TEST_CASE("composite is source-over on premultiplied alpha") {
  arbc::CpuBackend backend;
  // Premultiplied half-transparent green over opaque red.
  arbc::CpuSurface src(1, 1, arbc::PixelFormat::Rgba32fLinearPremul);
  fill(src, 0.0F, 0.5F, 0.0F, 0.5F);
  arbc::CpuSurface dst(1, 1, arbc::PixelFormat::Rgba32fLinearPremul);
  fill(dst, 1.0F, 0.0F, 0.0F, 1.0F);

  backend.composite(dst, src, arbc::Affine::identity(), 1.0);

  const std::span<const float> out = std::as_const(dst).cpu_pixels();
  REQUIRE(out[0] == 0.5F); // 0 + (1 - 0.5) * 1
  REQUIRE(out[1] == 0.5F); // 0.5 + (1 - 0.5) * 0
  REQUIRE(out[2] == 0.0F);
  REQUIRE(out[3] == 1.0F); // 0.5 + (1 - 0.5) * 1
}

TEST_CASE("degenerate mappings composite nothing") {
  arbc::CpuBackend backend;
  arbc::CpuSurface src(2, 2, arbc::PixelFormat::Rgba32fLinearPremul);
  fill(src, 1.0F, 1.0F, 1.0F, 1.0F);
  arbc::CpuSurface dst(2, 2, arbc::PixelFormat::Rgba32fLinearPremul);

  backend.composite(dst, src, arbc::Affine::scaling(0.0, 1.0), 1.0);

  for (const float value : std::as_const(dst).cpu_pixels()) {
    REQUIRE(value == 0.0F);
  }
}

} // namespace
