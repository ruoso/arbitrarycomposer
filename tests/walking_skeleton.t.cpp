#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/media/pixel_format.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/offline.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstring>
#include <memory>
#include <utility>

namespace {

std::array<float, 4> pixel(const arbc::Surface& surface, int x, int y) {
  const std::span<const float> data = surface.span<arbc::PixelFormat::Rgba32fLinearPremul>();
  const std::size_t at =
      4 * (static_cast<std::size_t>(y) * static_cast<std::size_t>(surface.width()) +
           static_cast<std::size_t>(x));
  return {data[at], data[at + 1], data[at + 2], data[at + 3]};
}

// The walking skeleton (doc 16): a document with solid layers flows
// through model -> compositor -> CPU backend and produces exactly the
// pixels the design promises.
//
// Scene, on an 8x8 viewport with an identity camera:
//   bottom: opaque red unit square scaled 4x, placed at (2, 2) -> [2,6)^2
//   top:    half-transparent green unit square scaled (8, 4) -> x [0,8), y [0,4)
//
// enforces: 07-color-and-pixel-formats#premultiplied-source-over
TEST_CASE("walking skeleton: solid layers compose to exact pixels") {
  arbc::Document document;
  const arbc::ObjectId red = document.add_content(std::make_shared<arbc::SolidContent>(
      arbc::Rgba{1.0F, 0.0F, 0.0F, 1.0F}, arbc::Rect{0.0, 0.0, 1.0, 1.0}));
  document.add_layer(red,
                     compose(arbc::Affine::translation(2.0, 2.0), arbc::Affine::scaling(4.0, 4.0)));
  const arbc::ObjectId green = document.add_content(std::make_shared<arbc::SolidContent>(
      arbc::Rgba{0.0F, 0.5F, 0.0F, 0.5F}, arbc::Rect{0.0, 0.0, 1.0, 1.0}));
  document.add_layer(green, arbc::Affine::scaling(8.0, 4.0));

  arbc::CpuBackend backend;
  const arbc::Viewport viewport{8, 8, arbc::Affine::identity()};
  const std::unique_ptr<arbc::Surface> out = render_offline(document, viewport, backend);

  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 8; ++x) {
      const bool in_red = x >= 2 && x < 6 && y >= 2 && y < 6;
      const bool in_green = y < 4;
      std::array<float, 4> expected{0.0F, 0.0F, 0.0F, 0.0F};
      if (in_red) {
        expected = {1.0F, 0.0F, 0.0F, 1.0F};
      }
      if (in_green) {
        // Source-over on premultiplied alpha: out = s + (1 - a_s) * d.
        for (std::size_t k = 0; k < 4; ++k) {
          const std::array<float, 4> src{0.0F, 0.5F, 0.0F, 0.5F};
          expected[k] = src[k] + 0.5F * expected[k];
        }
      }
      CAPTURE(x, y);
      REQUIRE(pixel(*out, x, y) == expected);
    }
  }
}

// enforces: 16-sdlc-and-quality#byte-exact-goldens
TEST_CASE("walking skeleton: rendering is byte-exact deterministic") {
  arbc::Document document;
  const arbc::ObjectId content = document.add_content(std::make_shared<arbc::SolidContent>(
      arbc::Rgba{0.25F, 0.5F, 0.75F, 1.0F}, arbc::Rect{0.0, 0.0, 3.0, 3.0}));
  document.add_layer(
      content, compose(arbc::Affine::translation(1.0, 0.5), arbc::Affine::scaling(1.5, 2.0)), 0.75);

  arbc::CpuBackend backend;
  const arbc::Viewport viewport{16, 16, arbc::Affine::scaling(2.0, 2.0)};
  const std::unique_ptr<arbc::Surface> first = render_offline(document, viewport, backend);
  const std::unique_ptr<arbc::Surface> second = render_offline(document, viewport, backend);

  const std::span<const float> a =
      std::as_const(*first).span<arbc::PixelFormat::Rgba32fLinearPremul>();
  const std::span<const float> b =
      std::as_const(*second).span<arbc::PixelFormat::Rgba32fLinearPremul>();
  REQUIRE(a.size() == b.size());
  REQUIRE(std::memcmp(a.data(), b.data(), a.size_bytes()) == 0);
}

} // namespace
