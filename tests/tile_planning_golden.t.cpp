#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/compositor/tile_planning.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/media/pixel_format.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/offline.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <utility>

// Byte-exact cross-component goldens for the interactive tiled driver (doc
// 16:47-53). The pure planner classification is unit-tested in
// `src/compositor/t/tile_planning.t.cpp`; here we drive the end-to-end
// resolve+composite path through the CPU backend and pin two properties the
// tiling promises:
//   1. tiled == whole: splitting a layer into local-space-aligned tiles and
//      compositing each reconstructs the offline whole-region render
//      byte-for-byte (no seam, gap, or double-blend) at a power-of-two rung.
//   2. warm cache: a second frame over an unchanged scene issues zero renders
//      and reproduces the first frame byte-for-byte (doc 04:93-94 tile reuse).

namespace {

// A Content that counts its render calls and otherwise renders a solid fill,
// so a warm-cache second frame's render count is directly observable.
class CountingContent : public arbc::Content {
public:
  CountingContent(arbc::Rgba color, std::optional<arbc::Rect> bounds)
      : d_solid(color, std::move(bounds)) {}

  std::optional<arbc::Rect> bounds() const override { return d_solid.bounds(); }
  arbc::Stability stability() const override { return d_solid.stability(); }
  std::optional<arbc::TimeRange> time_extent() const override { return d_solid.time_extent(); }
  std::optional<arbc::RenderResult> render(const arbc::RenderRequest& request,
                                           std::shared_ptr<arbc::RenderCompletion> done) override {
    ++d_renders;
    return d_solid.render(request, std::move(done));
  }

  int renders() const { return d_renders; }

private:
  arbc::SolidContent d_solid;
  int d_renders{0};
};

bool byte_identical(const arbc::Surface& lhs, const arbc::Surface& rhs) {
  const std::span<const float> a =
      std::as_const(lhs).span<arbc::PixelFormat::Rgba32fLinearPremul>();
  const std::span<const float> b =
      std::as_const(rhs).span<arbc::PixelFormat::Rgba32fLinearPremul>();
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size_bytes()) == 0;
}

} // namespace

// enforces: 16-sdlc-and-quality#byte-exact-goldens
TEST_CASE(
    "render_frame_interactive: tiled composite equals the whole render at a power-of-two rung") {
  // Identity camera, unit-scale layer -> composed scale 1.0 -> rung 0,
  // remainder 1.0. A 512x512 content splits into a 2x2 grid of 256^2 tiles.
  arbc::Document document;
  const arbc::ObjectId content = document.add_content(std::make_shared<arbc::SolidContent>(
      arbc::Rgba{0.25F, 0.5F, 0.75F, 1.0F}, arbc::Rect{0.0, 0.0, 512.0, 512.0}));
  document.add_layer(content, arbc::Affine::identity());

  arbc::CpuBackend backend;
  const arbc::Viewport viewport{512, 512, arbc::Affine::identity()};

  // The offline whole-region reference.
  const auto whole_result = render_offline(document, viewport, backend);
  REQUIRE(whole_result.has_value());
  const std::unique_ptr<arbc::Surface>& whole = *whole_result;

  // The interactive tiled render of the same scene.
  const arbc::DocStatePtr state = document.pin();
  arbc::SurfacePool pool(backend);
  arbc::TileCache cache(64u * 1024 * 1024);
  arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError> tiled =
      backend.make_surface(whole->width(), whole->height(), whole->format());
  REQUIRE(tiled.has_value());
  arbc::render_frame_interactive(
      *state, [&document](arbc::ObjectId id) { return document.resolve(id); }, viewport, cache,
      backend, pool, **tiled, arbc::Deadline::none(), std::nullopt);

  REQUIRE(byte_identical(*whole, **tiled));
}

TEST_CASE(
    "render_frame_interactive: a warm second frame issues zero renders, byte-identical output") {
  auto counting = std::make_shared<CountingContent>(arbc::Rgba{0.5F, 0.25F, 0.125F, 1.0F},
                                                    arbc::Rect{0.0, 0.0, 512.0, 512.0});
  arbc::Document document;
  const arbc::ObjectId content = document.add_content(counting);
  document.add_layer(content, arbc::Affine::identity());

  arbc::CpuBackend backend;
  const arbc::Viewport viewport{512, 512, arbc::Affine::identity()};
  const arbc::DocStatePtr state = document.pin();
  const auto resolver = [&document](arbc::ObjectId id) { return document.resolve(id); };
  arbc::SurfacePool pool(backend);
  arbc::TileCache cache(64u * 1024 * 1024);

  arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError> frame1 =
      backend.make_surface(512, 512, arbc::k_working_rgba32f);
  arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError> frame2 =
      backend.make_surface(512, 512, arbc::k_working_rgba32f);
  REQUIRE(frame1.has_value());
  REQUIRE(frame2.has_value());

  // Frame 1 fills the cache: each of the 2x2 tiles is rendered exactly once.
  arbc::render_frame_interactive(*state, resolver, viewport, cache, backend, pool, **frame1,
                                 arbc::Deadline::none(), std::nullopt);
  const int after_first = counting->renders();
  CHECK(after_first == 4);

  // Frame 2 over the unchanged scene plans all-fresh: zero new renders.
  arbc::render_frame_interactive(*state, resolver, viewport, cache, backend, pool, **frame2,
                                 arbc::Deadline::none(), std::nullopt);
  CHECK(counting->renders() == after_first);

  REQUIRE(byte_identical(**frame1, **frame2));
}
