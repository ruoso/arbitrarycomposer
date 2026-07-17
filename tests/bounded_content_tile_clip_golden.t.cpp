#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/compositor/damage_planning.hpp>
#include <arbc/compositor/tile_planning.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/media/pixel_format.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/model/damage.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/offline.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <memory>
#include <optional>
#include <span>

// Byte-exact golden for `compositor.bounded_content_tile_clip` (doc 16:48-53):
// a layer whose content declares a finite `bounds()` smaller than -- or not
// aligned to -- its 256px tile cell must not paint the cell's overhang past that
// extent. The oracle is the OFFLINE path (`render_offline`,
// `compositor.cpp:28-34`), which already sizes its temp to `region ∩ bounds` and
// so never bleeds: the fix makes the tiled path's pixels match it byte-for-byte.
// This file qualifies everything with `arbc::` (no `using namespace`), matching
// `tests/tile_planning_golden.t.cpp`, the sibling tiled==whole oracle.

namespace {

bool byte_identical(const arbc::Surface& lhs, const arbc::Surface& rhs) {
  const std::span<const float> a =
      std::as_const(lhs).span<arbc::PixelFormat::Rgba32fLinearPremul>();
  const std::span<const float> b =
      std::as_const(rhs).span<arbc::PixelFormat::Rgba32fLinearPremul>();
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size_bytes()) == 0;
}

// An opaque backdrop covering the whole viewport, premultiplied (each channel <=
// alpha). Source-over of an opaque layer within its clip fully replaces the
// backdrop there, so any overhang that leaks past the clip is trivially visible
// as a byte difference against the offline reference.
arbc::ObjectId add_backdrop(arbc::Document& document, double width, double height) {
  return document.add_content(std::make_shared<arbc::SolidContent>(
      arbc::Rgba{0.2F, 0.2F, 0.2F, 1.0F}, arbc::Rect{0.0, 0.0, width, height}));
}

// A resolver over a `Document`, the shape every driver call below takes.
auto resolver_for(const arbc::Document& document) {
  return [&document](arbc::ObjectId id) { return document.resolve(id); };
}

// Drive the tiled path over a fresh cache/target and return the composited
// surface, so a test can compare it against the offline reference.
arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError>
render_tiled(arbc::Document& document, const arbc::Viewport& viewport, arbc::CpuBackend& backend,
             int width, int height) {
  const arbc::DocStatePtr state = document.pin();
  arbc::SurfacePool pool(backend);
  arbc::TileCache cache(64u * 1024 * 1024);
  arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError> tiled =
      backend.make_surface(width, height, arbc::k_working_rgba32f);
  if (!tiled.has_value()) {
    return tiled;
  }
  arbc::render_frame_interactive(*state, resolver_for(document), viewport, cache, backend, pool,
                                 **tiled, arbc::Deadline::none(), std::nullopt);
  return tiled;
}

} // namespace

// enforces: 02-architecture#tile-composite-clipped-to-content-bounds
// enforces: 03-layer-plugin-interface#render-within-declared-bounds
// enforces: 16-sdlc-and-quality#byte-exact-goldens
TEST_CASE("bounded clip golden: a sub-tile bounded solid does not paint past its declared extent") {
  arbc::Document document;
  const arbc::ObjectId backdrop = add_backdrop(document, 256.0, 256.0);
  // A 64x64 extent inside the single 256px rung-0 cell: the tile buffer fills
  // whole-cell (the solid is infinite extent and ignores bounds), so only the
  // composite-time clip keeps the overhang [64,256) off the target.
  const arbc::ObjectId fg = document.add_content(std::make_shared<arbc::SolidContent>(
      arbc::Rgba{0.6F, 0.1F, 0.1F, 1.0F}, arbc::Rect{0.0, 0.0, 64.0, 64.0}));
  const arbc::ObjectId comp = document.add_composition(256.0, 256.0);
  document.attach_layer(comp, document.add_layer(backdrop, arbc::Affine::identity()));
  document.attach_layer(comp, document.add_layer(fg, arbc::Affine::identity()));

  arbc::CpuBackend backend;
  const arbc::Viewport viewport{256, 256, arbc::Affine::identity(), comp};

  const auto whole = render_offline(document, viewport, backend);
  REQUIRE(whole.has_value());
  const auto tiled =
      render_tiled(document, viewport, backend, (*whole)->width(), (*whole)->height());
  REQUIRE(tiled.has_value());

  // The overhang pixels (extent edge -> tile edge) equal the backdrop, byte for
  // byte -- which is exactly what the offline path composited.
  REQUIRE(byte_identical(**whole, **tiled));
}

// enforces: 02-architecture#tile-composite-clipped-to-content-bounds
// enforces: 16-sdlc-and-quality#byte-exact-goldens
TEST_CASE("bounded clip golden: a bounded solid straddling a tile boundary is clipped in both "
          "cells") {
  arbc::Document document;
  const arbc::ObjectId backdrop = add_backdrop(document, 512.0, 512.0);
  // [192,320) x [192,320) crosses the 256px cell edge on both axes, so four
  // rung-0 cells are planned and every one of them must clip its overhang.
  const arbc::ObjectId fg = document.add_content(std::make_shared<arbc::SolidContent>(
      arbc::Rgba{0.1F, 0.5F, 0.2F, 1.0F}, arbc::Rect{192.0, 192.0, 320.0, 320.0}));
  const arbc::ObjectId comp = document.add_composition(512.0, 512.0);
  document.attach_layer(comp, document.add_layer(backdrop, arbc::Affine::identity()));
  document.attach_layer(comp, document.add_layer(fg, arbc::Affine::identity()));

  arbc::CpuBackend backend;
  const arbc::Viewport viewport{512, 512, arbc::Affine::identity(), comp};

  const auto whole = render_offline(document, viewport, backend);
  REQUIRE(whole.has_value());
  const auto tiled =
      render_tiled(document, viewport, backend, (*whole)->width(), (*whole)->height());
  REQUIRE(tiled.has_value());
  REQUIRE(byte_identical(**whole, **tiled));
}

// enforces: 02-architecture#tile-composite-clipped-to-content-bounds
// enforces: 16-sdlc-and-quality#byte-exact-goldens
TEST_CASE("bounded clip golden: gated repaint of sub-tile bounds does not bleed") {
  arbc::Document document;
  const arbc::ObjectId backdrop = add_backdrop(document, 256.0, 256.0);
  const arbc::ObjectId fg = document.add_content(std::make_shared<arbc::SolidContent>(
      arbc::Rgba{0.6F, 0.1F, 0.1F, 1.0F}, arbc::Rect{0.0, 0.0, 64.0, 64.0}));
  const arbc::ObjectId comp = document.add_composition(256.0, 256.0);
  document.attach_layer(comp, document.add_layer(backdrop, arbc::Affine::identity()));
  document.attach_layer(comp, document.add_layer(fg, arbc::Affine::identity()));

  arbc::CpuBackend backend;
  const arbc::Viewport viewport{256, 256, arbc::Affine::identity(), comp};

  const auto whole = render_offline(document, viewport, backend);
  REQUIRE(whole.has_value());

  const arbc::DocStatePtr state = document.pin();
  const auto resolver = resolver_for(document);
  arbc::SurfacePool pool(backend);
  arbc::TileCache cache(64u * 1024 * 1024);
  auto target =
      backend.make_surface((*whole)->width(), (*whole)->height(), arbc::k_working_rgba32f);
  REQUIRE(target.has_value());

  // A full (un-gated) pass warms the cache and fills the whole target -- equal to
  // offline, the sub-tile-clip case above.
  arbc::render_frame_interactive(*state, resolver, viewport, cache, backend, pool, **target,
                                 arbc::Deadline::none(), std::nullopt);
  REQUIRE(byte_identical(**whole, **target));

  // A gated pass over a rect that spans BOTH the bounded extent and its overhang.
  // The frame clears the rect to transparent then re-composites: the effective
  // clip is `repaint_rect ∩ device_bounds`, so the overhang inside the dirty rect
  // is repainted to backdrop, not to the foreground -- byte-identical to offline.
  arbc::DirtyRegion dirty;
  dirty.device_rects.push_back(arbc::Rect{0.0, 0.0, 128.0, 256.0});
  arbc::render_frame_interactive(*state, resolver, viewport, cache, backend, pool, **target,
                                 arbc::Deadline::none(), std::nullopt, /*pending=*/nullptr,
                                 /*counters=*/nullptr, &dirty);
  REQUIRE(byte_identical(**whole, **target));
}

// enforces: 09-surfaces-and-backends#clip-scoped-ops-honor-the-clip
// enforces: 16-sdlc-and-quality#byte-exact-goldens
TEST_CASE("bounded clip golden: content whose bounds cover the tile composites byte-identically") {
  // Two identical scenes differing ONLY in the foreground's bounds: one covering
  // the whole viewport (composited via `composite_clipped` with a whole-destination
  // clip), one unbounded (composited via the plain `composite`). Doc 09's clip
  // identity -- a clip covering the whole destination is byte-identical to the
  // unclipped composite -- makes the two byte-for-byte equal, which is why every
  // `bounds == viewport` scene stays un-rebaselined by this task.
  const auto build = [](std::optional<arbc::Rect> fg_bounds, arbc::CpuBackend& backend) {
    arbc::Document document;
    const arbc::ObjectId backdrop = add_backdrop(document, 256.0, 256.0);
    const arbc::ObjectId fg = document.add_content(
        std::make_shared<arbc::SolidContent>(arbc::Rgba{0.6F, 0.1F, 0.1F, 1.0F}, fg_bounds));
    const arbc::ObjectId comp = document.add_composition(256.0, 256.0);
    document.attach_layer(comp, document.add_layer(backdrop, arbc::Affine::identity()));
    document.attach_layer(comp, document.add_layer(fg, arbc::Affine::identity()));
    const arbc::Viewport viewport{256, 256, arbc::Affine::identity(), comp};
    const auto whole = render_offline(document, viewport, backend);
    REQUIRE(whole.has_value());
    auto tiled = render_tiled(document, viewport, backend, (*whole)->width(), (*whole)->height());
    REQUIRE(tiled.has_value());
    // The tiled path still matches offline for a bounds-covers-viewport layer:
    // the tiled==whole property, re-asserted.
    REQUIRE(byte_identical(**whole, **tiled));
    return std::move(*tiled);
  };

  arbc::CpuBackend backend;
  const std::unique_ptr<arbc::Surface> bounded = build(arbc::Rect{0.0, 0.0, 256.0, 256.0}, backend);
  const std::unique_ptr<arbc::Surface> unbounded = build(std::nullopt, backend);
  REQUIRE(byte_identical(*bounded, *unbounded));
}
