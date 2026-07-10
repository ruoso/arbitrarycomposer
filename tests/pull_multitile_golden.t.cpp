// Byte-exact "pull tiled == whole" golden for compositor.pull_service (doc 16
// tier-3; refinement compositor.pull_multi_tile_region Constraint 6, Acceptance
// "Golden"). A region spanning a 2x2 block of `k_tile_size` (256 px) tiles, pulled
// through the live `PullServiceImpl` with a synchronous `direct_dispatch`, walks
// the four covering tiles -- each rendered over its own footprint into a cache-
// owned surface and `deliver_tile`d into its disjoint sub-rect of the caller's
// target -- and must reconstruct byte-for-byte what the same content produces
// rendered WHOLE into a region-sized surface (the rendering-is-recursion identity,
// doc 05 "tiled == whole"): no seam, no double-blend at tile boundaries.
//
// The oracle is a position-DEPENDENT leaf (`RasterContent` over a gradient), so a
// per-tile mistake -- wrong footprint, a shared top-left corner, an overlapping
// sub-rect -- diverges the bytes (a uniform solid would pass trivially). Rendering
// the raw leaf whole (a direct `render`, no pull) is a true independent oracle:
// unlike an operator whose own whole render recurses back through the multi-tile
// pull, it exercises the tiling path exactly once. Cross-component (CpuBackend +
// kind_raster + compositor), so it links the umbrella `arbc` and lives here.

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/expected.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/cache/key_shapes.hpp>
#include <arbc/cache/keyed_store.hpp>
#include <arbc/compositor/counters.hpp>
#include <arbc/compositor/pull_service.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/kind_raster/raster_content.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/surface/surface.hpp>
#include <arbc/surface/surface_error.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace {

using namespace arbc;

// A `dim x dim` premultiplied working-linear gradient (rgb <= a), fully
// deterministic (no RNG, no clock). Every pixel differs from its neighbours, so a
// per-tile footprint error is visible in the bytes.
DecodedImage gradient_image(int dim) {
  DecodedImage img;
  img.width = dim;
  img.height = dim;
  img.format = k_working_rgba32f;
  std::vector<float> f(static_cast<std::size_t>(dim) * static_cast<std::size_t>(dim) * 4U);
  const float span = static_cast<float>(dim - 1);
  for (int y = 0; y < dim; ++y) {
    for (int x = 0; x < dim; ++x) {
      const std::size_t o =
          (static_cast<std::size_t>(y) * static_cast<std::size_t>(dim) + static_cast<std::size_t>(x)) *
          4U;
      const float a = 0.5F + 0.5F * static_cast<float>(x) / span;
      f[o] = a * static_cast<float>(x) / span;
      f[o + 1] = a * static_cast<float>(y) / span;
      f[o + 2] = a * 0.25F;
      f[o + 3] = a;
    }
  }
  img.bytes.resize(f.size() * sizeof(float));
  std::memcpy(img.bytes.data(), f.data(), img.bytes.size());
  return img;
}

// A 2x2-tile region at native (power-of-two) scale: at rung 0 the grid cell is
// `k_tile_size` (256) local units, so `from_size(512,512)` covers exactly
// (0,0),(1,0),(0,1),(1,1).
RenderRequest region_request(Surface& target) {
  return RenderRequest{Rect::from_size(512.0, 512.0),
                       1.0,
                       Time::zero(),
                       StateHandle{},
                       target,
                       Exactness::BestEffort,
                       Deadline::none()};
}

std::vector<std::byte> bytes_of(const Surface& s) {
  const std::span<const std::byte> b = s.cpu_bytes();
  return {b.begin(), b.end()};
}

} // namespace

// enforces: 13-effects-as-operators#pull-fills-multi-tile-region
// enforces: 16-sdlc-and-quality#byte-exact-goldens
TEST_CASE("pull tiled == whole: a 2x2-tile region pulled through the live PullServiceImpl equals "
          "the content rendered whole, byte-exact") {
  CpuBackend backend;

  // Oracle: the content rendered WHOLE into a 512x512 (= 2x2 tile) surface at
  // native scale -- one direct `render`, no tiling, no pull.
  RasterContent whole_content(gradient_image(512));
  expected<std::unique_ptr<Surface>, SurfaceError> whole_surf =
      backend.make_surface(512, 512, k_working_rgba32f);
  REQUIRE(whole_surf.has_value());
  backend.clear(**whole_surf, 0.0F, 0.0F, 0.0F, 0.0F);
  {
    auto done = std::make_shared<RenderCompletion>();
    const std::optional<RenderResult> r = whole_content.render(region_request(**whole_surf), done);
    REQUIRE(r.has_value());
    CHECK(r->exact); // native, exact
  }
  const std::vector<std::byte> whole_bytes = bytes_of(**whole_surf);

  // Tiled: the SAME content pulled through the live `PullServiceImpl` over the 2x2
  // covering set (synchronous `direct_dispatch`). Each covering tile is rendered
  // over its own footprint into a cache-owned surface and delivered into its
  // disjoint sub-rect of the caller's target (cleared transparent, so a source-over
  // deliver onto zero reproduces the tile exactly).
  RasterContent tiled_content(gradient_image(512));
  const std::unordered_map<const Content*, ObjectId> ids{{&tiled_content, ObjectId{1}}};
  TileCache cache(256u * 1024 * 1024);
  CompositorCounters counters;
  PullConfig config;
  config.counters = &counters;
  config.id_of = [&ids](const Content* c) {
    const auto it = ids.find(c);
    return it != ids.end() ? it->second : ObjectId{};
  };
  config.contribution = [](const Content*) { return std::uint64_t{1}; };
  PullServiceImpl service(cache, backend, direct_dispatch(), config);

  expected<std::unique_ptr<Surface>, SurfaceError> tiled_surf =
      backend.make_surface(512, 512, k_working_rgba32f);
  REQUIRE(tiled_surf.has_value());
  backend.clear(**tiled_surf, 0.0F, 0.0F, 0.0F, 0.0F);

  auto done = std::make_shared<RenderCompletion>();
  service.pull(&tiled_content, region_request(**tiled_surf), done);

  // The aggregate settled once, exact (every covering tile exact at the rung), and
  // exactly the four covering tiles were rendered (cold cache).
  REQUIRE(done->settled());
  const auto settled = done->take();
  REQUIRE(settled.has_value());
  REQUIRE(settled->has_value());
  CHECK((*settled)->exact);
  CHECK(counters.requests_issued() == 4);

  // Byte-exact: tiled reconstruction equals the whole render, no seam, no
  // double-blend at the tile boundaries (Constraint 6).
  const std::vector<std::byte> tiled_bytes = bytes_of(**tiled_surf);
  REQUIRE(tiled_bytes.size() == whole_bytes.size());
  CHECK(std::memcmp(tiled_bytes.data(), whole_bytes.data(), whole_bytes.size()) == 0);
}
