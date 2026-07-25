#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/compositor/tile_planning.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/media/pixel_format.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/surface/surface_pool.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <span>

// Byte-exact goldens for `compositor.tile_apron` (doc 16 tier-3): the two defects
// the apron exists to remove, each pinned as the measurement that found it.
//
// Both are stated as EXACT pixel values derived from the geometry, not as frozen
// tables -- an opaque fill is opaque, and a half-covered edge pixel carries the
// coverage its geometry implies. Neither needs regenerating if the filter changes;
// they would need re-deriving, which is the point.
//
// CROSS-COMPONENT (tests/, linking the umbrella `arbc`): the real `CpuBackend`
// beside the compositor and `org.arbc.solid` (doc 17).

namespace {

float alpha_at(const arbc::Surface& surface, int x, int y) {
  const std::span<const float> px =
      std::as_const(surface).span<arbc::PixelFormat::Rgba32fLinearPremul>();
  const std::size_t at = 4 * (static_cast<std::size_t>(y) *
                                  static_cast<std::size_t>(surface.width()) +
                              static_cast<std::size_t>(x));
  return px[at + 3];
}

// Drive the tiled interactive path over a fresh cache and target.
arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError>
render_tiled(arbc::Document& document, const arbc::Viewport& viewport, arbc::CpuBackend& backend,
             int dim) {
  const arbc::DocStatePtr state = document.pin();
  arbc::SurfacePool pool(backend);
  arbc::TileCache cache(64U * 1024 * 1024);
  arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError> target =
      backend.make_surface(dim, dim, arbc::k_working_rgba32f);
  REQUIRE(target.has_value());
  const auto resolve = [&document](arbc::ObjectId id) { return document.resolve(id); };
  arbc::render_frame_interactive(*state, resolve, viewport, cache, backend, pool, **target,
                                 arbc::Deadline::none(), std::nullopt);
  return target;
}

} // namespace

// enforces: 02-architecture#tile-paints-its-cell-samples-its-apron
TEST_CASE("an opaque fill is opaque across every tile boundary at a fractional phase") {
  // The measurement that found the defect, inverted into a guard. One opaque solid,
  // wider than three tile cells, placed at a HALF-PIXEL offset so the composite has a
  // fractional phase -- which is the only condition under which the resampling tap
  // straddles two source texels at all. At an integral phase Catmull-Rom collapses to
  // `(0, 1, 0, 0)` and reads a single texel, which is why every pre-existing golden in
  // the suite missed this entirely.
  //
  // Before the apron, each tile's outer tap fell on its own surface's transparent
  // border, so the two tiles abutting a boundary each painted the boundary pixel at
  // roughly half weight and premultiplied source-over of two halves gave 0.75, with the
  // cubic's negative lobe ringing to 1.0625 either side. On a pan of half a pixel an
  // opaque fill grew a translucent grid every `k_tile_size` device pixels.
  constexpr int k_dim = 5 * arbc::k_tile_size / 2; // spans two internal cell boundaries
  arbc::Document document;
  const double extent = static_cast<double>(k_dim);
  const arbc::ObjectId comp = document.add_composition(extent, extent);
  const arbc::ObjectId solid = document.add_content(std::make_shared<arbc::SolidContent>(
      arbc::Rgba{0.5F, 0.5F, 0.5F, 1.0F}, arbc::Rect{0.0, 0.0, extent, extent}));
  document.attach_layer(comp, document.add_layer(solid, arbc::Affine::translation(0.5, 0.5)));

  arbc::CpuBackend backend;
  const arbc::Viewport viewport{k_dim, k_dim, arbc::Affine::scaling(1.0, 1.0), comp};
  const auto out = render_tiled(document, viewport, backend, k_dim);
  REQUIRE(out.has_value());

  // Every pixel strictly inside the fill is fully opaque -- most sharply the ones ON
  // the two internal cell boundaries and their immediate neighbours, which is exactly
  // where the seam appeared. The sweep starts `k_composite_tap_reach` pixels in: the
  // fill's OWN edges are antialiased (it begins half a pixel inside the viewport), and
  // the cubic's negative lobe carries that falloff one further pixel, so the outer two
  // rows and columns are the content's edge rather than a tile boundary.
  constexpr int k_edge = arbc::k_composite_tap_reach;
  for (int y = k_edge; y < k_dim - k_edge; ++y) {
    for (int x = k_edge; x < k_dim - k_edge; ++x) {
      CAPTURE(x, y);
      REQUIRE(alpha_at(**out, x, y) == 1.0F);
    }
  }
}

// enforces: 02-architecture#bounded-content-extent-zeroed-in-tile
TEST_CASE("a bounded content's extent edge carries its coverage, not a whole-pixel clip") {
  // A content whose declared extent ends on a HALF pixel. An opaque solid over local
  // [0, 8), offset by one local unit and rendered at 0.5x, so its extent maps to device
  // [0.5, 4.5): device pixel 4 spans [4, 5) and the content covers exactly half of it.
  //
  // The old composite-time clip rounded the extent OUT to whole device pixels, so that
  // pixel was painted at FULL strength -- a hard edge plus up to a pixel of bleed, the
  // cost `bounded_content_tile_clip` Constraint 2 knowingly accepted. Enforcing the
  // extent in the tile's own pixels instead lets the composite's tap fall off across
  // the boundary, so the pixel carries its coverage.
  constexpr int k_dim = 8;
  arbc::Document document;
  const arbc::ObjectId comp = document.add_composition(8.0, 8.0);
  const arbc::ObjectId solid = document.add_content(std::make_shared<arbc::SolidContent>(
      arbc::Rgba{1.0F, 1.0F, 1.0F, 1.0F}, arbc::Rect{0.0, 0.0, 8.0, 8.0}));
  document.attach_layer(comp, document.add_layer(solid, arbc::Affine::translation(1.0, 1.0)));

  arbc::CpuBackend backend;
  const arbc::Viewport viewport{k_dim, k_dim, arbc::Affine::scaling(0.5, 0.5), comp};
  const auto out = render_tiled(document, viewport, backend, k_dim);
  REQUIRE(out.has_value());

  // Device column 4 spans [4, 5); the content reaches 4.5, so it covers half of it.
  // Interior column 2 is fully covered and stays opaque -- the pair is what makes this
  // a statement about the EDGE rather than about the whole layer being dimmed.
  CHECK(alpha_at(**out, 2, 2) == 1.0F);
  CHECK(alpha_at(**out, 4, 2) == 0.5F);
  CHECK(alpha_at(**out, 2, 4) == 0.5F);
  // Nothing at all past the extent: column 5 begins at device 5.0, half a pixel beyond
  // the content's last coverage, so no falloff reaches it.
  CHECK(alpha_at(**out, 5, 2) == 0.0F);
}
