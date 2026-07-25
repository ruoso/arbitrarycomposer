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

// The green layer's composited premultiplied value, per device row it covers.
//
// Rows 1-3 are the FLAT {0, 0.5, 0, 0.5} the geometry calls for: a uniformly
// half-transparent rect, minified 2:1 in y, whose four Catmull-Rom taps all land on
// green and whose weights sum to 1.
//
// Row 0 rings UP to `0.5 * (w1 + w2 + w3) == 0.53125`, because its outer tap falls on
// the source surface's ZERO BORDER and drops a negative-weight lobe. That is the same
// zero-border mechanism (`kernels.hpp:66-68`) that makes compositor-side MAGNIFICATION
// unusable, showing up here on a plain minification.
//
// It is asymmetric -- row 0 rings and row 3 does not -- and the asymmetry is the tile
// grid: the content's local origin coincides with tile (0,0)'s local origin, so the
// tap at the TOP edge reaches past the tile surface's first row, while the bottom edge
// sits deep inside a 256px-tall surface with real green above and below it. A content
// edge that lands ON a tile boundary rings; one that lands mid-tile does not.
//
// The retired UNTILED driver rang at BOTH edges: it sized its temp to exactly the
// content's device footprint (8x8 here), so both the top and the bottom tap hit the
// border. Moving to the tiled driver fixed row 3 and left row 0 as it was.
//
// The red layer was never affected: it maps to device [2,6)^2 by an integer
// translation, so its tap is integer-phase (weights (0,1,0,0)) and byte-exact.
std::array<float, 4> green_src_row(int y) {
  const float g = (y == 0) ? 0.53125F : 0.5F;
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
        // green source per-row (flat 0.5, ringing at row 0 only -- see above).
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
