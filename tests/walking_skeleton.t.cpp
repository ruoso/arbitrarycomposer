#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/media/image_resampler.hpp>
#include <arbc/media/pixel_format.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/offline.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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

// The green layer (premultiplied {0,0.5,0,0.5}) is rendered into an 8x8 temp at
// its max device scale (8), then composited with temp_to_dst = scaling(1, 0.5) --
// a 2:1 MINIFICATION in y through the composite tap. So device row y samples temp
// rows 2y-1 .. 2y+2 at Catmull-Rom phase 0.5 (x stays integer-phase, exact). At an
// interior row all four taps are green and the weights sum to 1, so the value is
// the flat 0.5; at the green region's top (y=0) and bottom (y=3) edges one outer
// tap lands on the zero border, dropping a negative-weight lobe, so the
// premultiplied value rings UP to 0.5 * (w1 + w2 + w3) = 0.53125 (color.resample_
// filter_quality's Catmull-Rom tap; the incumbent bilinear read a flat 0.5 here).
// The result is clamped non-negative (no effect at these positive rows). The red
// layer maps to device [2,6)^2 by an integer translation, so its tap is
// integer-phase (weights (0,1,0,0)) and stays byte-exact.
std::array<float, 4> green_src_row(int y) {
  const std::array<float, 4> w = arbc::catmull_rom_weights(0.5F);
  float s = 0.0F;
  const int j0 = 2 * y; // floor(2y + 0.5)
  for (int t = 0; t < 4; ++t) {
    const int row = j0 - 1 + t;                              // Catmull-Rom taps 2y-1..2y+2
    const float cover = (row >= 0 && row < 8) ? 1.0F : 0.0F; // temp is 8 rows of green
    s += w[static_cast<std::size_t>(t)] * cover;
  }
  const float g = std::max(0.0F, 0.5F * s); // premul green & alpha both scale by 0.5
  return {0.0F, g, 0.0F, g};
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
  // The frame walk is composition-scoped, so the offline driver sources the root
  // composition and the layers must be its members, bottom-to-top in creation order
  // (compositor.root_composition_frame_walk, doc 05:28-36).
  const arbc::ObjectId comp = document.add_composition(8.0, 8.0);
  const arbc::ObjectId red = document.add_content(std::make_shared<arbc::SolidContent>(
      arbc::Rgba{1.0F, 0.0F, 0.0F, 1.0F}, arbc::Rect{0.0, 0.0, 1.0, 1.0}));
  document.attach_layer(comp, document.add_layer(red, compose(arbc::Affine::translation(2.0, 2.0),
                                                              arbc::Affine::scaling(4.0, 4.0))));
  const arbc::ObjectId green = document.add_content(std::make_shared<arbc::SolidContent>(
      arbc::Rgba{0.0F, 0.5F, 0.0F, 0.5F}, arbc::Rect{0.0, 0.0, 1.0, 1.0}));
  document.attach_layer(comp, document.add_layer(green, arbc::Affine::scaling(8.0, 4.0)));

  arbc::CpuBackend backend;
  const arbc::Viewport viewport{8, 8, arbc::Affine::identity()};
  const auto out_result = render_offline(document, viewport, backend);
  REQUIRE(out_result.has_value());
  const std::unique_ptr<arbc::Surface>& out = *out_result;

  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 8; ++x) {
      const bool in_red = x >= 2 && x < 6 && y >= 2 && y < 6;
      const bool in_green = y < 4;
      std::array<float, 4> expected{0.0F, 0.0F, 0.0F, 0.0F};
      if (in_red) {
        expected = {1.0F, 0.0F, 0.0F, 1.0F};
      }
      if (in_green) {
        // Source-over on premultiplied alpha: out = s + (1 - a_s) * d, with the
        // resampled green source (flat 0.5 interior, ringing to 0.53125 at the
        // green region's top/bottom edges -- see green_src_row).
        const std::array<float, 4> src = green_src_row(y);
        for (std::size_t k = 0; k < 4; ++k) {
          expected[k] = src[k] + (1.0F - src[3]) * expected[k];
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
  const arbc::ObjectId comp = document.add_composition(16.0, 16.0);
  const arbc::ObjectId content = document.add_content(std::make_shared<arbc::SolidContent>(
      arbc::Rgba{0.25F, 0.5F, 0.75F, 1.0F}, arbc::Rect{0.0, 0.0, 3.0, 3.0}));
  document.attach_layer(comp, document.add_layer(content,
                                                 compose(arbc::Affine::translation(1.0, 0.5),
                                                         arbc::Affine::scaling(1.5, 2.0)),
                                                 0.75));

  arbc::CpuBackend backend;
  const arbc::Viewport viewport{16, 16, arbc::Affine::scaling(2.0, 2.0)};
  const auto first_result = render_offline(document, viewport, backend);
  const auto second_result = render_offline(document, viewport, backend);
  REQUIRE(first_result.has_value());
  REQUIRE(second_result.has_value());
  const std::unique_ptr<arbc::Surface>& first = *first_result;
  const std::unique_ptr<arbc::Surface>& second = *second_result;

  const std::span<const float> a =
      std::as_const(*first).span<arbc::PixelFormat::Rgba32fLinearPremul>();
  const std::span<const float> b =
      std::as_const(*second).span<arbc::PixelFormat::Rgba32fLinearPremul>();
  REQUIRE(a.size() == b.size());
  REQUIRE(std::memcmp(a.data(), b.data(), a.size_bytes()) == 0);
}

} // namespace
