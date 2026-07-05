#include <arbc/backend_cpu/cpu_backend.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <span>
#include <utility>

namespace {

void fill(arbc::Surface& surface, float r, float g, float b, float a) {
  arbc::CpuBackend backend;
  backend.clear(surface, r, g, b, a);
}

TEST_CASE("identity composite over transparent copies the source") {
  arbc::CpuBackend backend;
  arbc::CpuSurface src(2, 2, arbc::k_working_rgba32f);
  fill(src, 1.0F, 0.0F, 0.0F, 1.0F);
  arbc::CpuSurface dst(2, 2, arbc::k_working_rgba32f);

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
  arbc::CpuSurface src(1, 1, arbc::k_working_rgba32f);
  fill(src, 0.0F, 0.5F, 0.0F, 0.5F);
  arbc::CpuSurface dst(1, 1, arbc::k_working_rgba32f);
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
  arbc::CpuSurface src(2, 2, arbc::k_working_rgba32f);
  fill(src, 1.0F, 1.0F, 1.0F, 1.0F);
  arbc::CpuSurface dst(2, 2, arbc::k_working_rgba32f);

  backend.composite(dst, src, arbc::Affine::scaling(0.0, 1.0), 1.0);

  for (const float value : std::as_const(dst).cpu_pixels()) {
    REQUIRE(value == 0.0F);
  }
}

// enforces: 07-color-and-pixel-formats#surfaces-carry-tags
TEST_CASE("a created surface echoes its full tag triple") {
  arbc::CpuBackend backend;
  const std::unique_ptr<arbc::Surface> s = backend.make_surface(4, 3, arbc::k_working_rgba32f);
  REQUIRE(s != nullptr);
  REQUIRE(s->width() == 4);
  REQUIRE(s->height() == 3);

  const arbc::SurfaceFormat f = s->format();
  REQUIRE(f == arbc::k_working_rgba32f);
  REQUIRE(f.pixel_format == arbc::PixelFormat::Rgba32fLinearPremul);
  REQUIRE(f.color_space == arbc::k_linear_srgb);
  REQUIRE(f.premultiplied == arbc::Premultiplied::Yes);
}

TEST_CASE("make_surface reports formats it cannot store as null") {
  arbc::CpuBackend backend;
  // Supported: the premultiplied linear-light rgba32f working format.
  REQUIRE(backend.make_surface(2, 2, arbc::k_working_rgba32f) != nullptr);

  // Unsupported storage until color.kernels: f16 and 8-bit are describable
  // but creatable nowhere yet.
  const arbc::SurfaceFormat f16{arbc::PixelFormat::Rgba16fLinearPremul, arbc::k_linear_srgb,
                                arbc::Premultiplied::Yes};
  REQUIRE(backend.make_surface(2, 2, f16) == nullptr);
  const arbc::SurfaceFormat srgb8{arbc::PixelFormat::Rgba8Srgb, arbc::k_srgb,
                                  arbc::Premultiplied::Yes};
  REQUIRE(backend.make_surface(2, 2, srgb8) == nullptr);

  // Unsupported working space until color.working_space: rgba32f storage but
  // a non-default color-space or premultiplication tag the backend has no
  // kernel to honor.
  const arbc::SurfaceFormat nonlinear{arbc::PixelFormat::Rgba32fLinearPremul, arbc::k_srgb,
                                      arbc::Premultiplied::Yes};
  REQUIRE(backend.make_surface(2, 2, nonlinear) == nullptr);
  const arbc::SurfaceFormat straight{arbc::PixelFormat::Rgba32fLinearPremul, arbc::k_linear_srgb,
                                     arbc::Premultiplied::No};
  REQUIRE(backend.make_surface(2, 2, straight) == nullptr);
}

// enforces: 07-color-and-pixel-formats#surfaces-carry-tags
TEST_CASE("composite refuses mismatched surface tags") {
  arbc::CpuBackend backend;
  arbc::CpuSurface dst(1, 1, arbc::k_working_rgba32f);
  fill(dst, 1.0F, 0.0F, 0.0F, 1.0F);

  // Same rgba32f storage but a different color-space tag: a working-space
  // mismatch conversions (color.kernels) would resolve and that this backend
  // must never silently reinterpret.
  const arbc::SurfaceFormat srgb_tagged{arbc::PixelFormat::Rgba32fLinearPremul, arbc::k_srgb,
                                        arbc::Premultiplied::Yes};
  arbc::CpuSurface src(1, 1, srgb_tagged);
  fill(src, 0.0F, 1.0F, 0.0F, 1.0F);

  REQUIRE_FALSE(dst.format() == src.format()); // the rejection precondition

#ifdef NDEBUG
  // Release: the mismatch is a no-op cull (asserts compiled out), so dst is
  // left byte-for-byte unchanged -- mixed-tag compositing never happens.
  backend.composite(dst, src, arbc::Affine::identity(), 1.0);
  const std::span<const float> out = std::as_const(dst).cpu_pixels();
  REQUIRE(out[0] == 1.0F);
  REQUIRE(out[1] == 0.0F);
  REQUIRE(out[2] == 0.0F);
  REQUIRE(out[3] == 1.0F);
#endif
  // Debug: the same predicate the composite asserts on is witnessed above
  // without tripping the abort (mirrors the generation-tag convention in
  // pool/t/refs.t.cpp).
}

} // namespace
