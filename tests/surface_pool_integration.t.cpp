#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/cache/keyed_store.hpp>
#include <arbc/compositor/compositor.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/surface/testing/counting_backend.hpp>

#include <catch2/catch_test_macros.hpp>

#include "tiled_render.hpp"

#include <memory>
#include <utility>

namespace {

// enforces: 09-surfaces-and-backends#surface-pool-recycles
TEST_CASE("a persistent pool reuses temps within and across frames") {
  // Scene on an 8x8 identity-camera viewport:
  //   two unit squares scaled 4x  -> two 4x4 temps (same key)
  //   one unit square  scaled 2x  -> one 2x2 temp  (distinct key)
  // Two distinct temp sizes across three layers.
  arbc::Document document;
  const arbc::Rect unit{0.0, 0.0, 1.0, 1.0};
  const arbc::ObjectId a = document.add_content(
      std::make_shared<arbc::SolidContent>(arbc::Rgba{1.0F, 0.0F, 0.0F, 1.0F}, unit));
  const arbc::ObjectId la = document.add_layer(
      a, compose(arbc::Affine::translation(0.0, 0.0), arbc::Affine::scaling(4.0, 4.0)));
  const arbc::ObjectId b = document.add_content(
      std::make_shared<arbc::SolidContent>(arbc::Rgba{0.0F, 1.0F, 0.0F, 1.0F}, unit));
  const arbc::ObjectId lb = document.add_layer(
      b, compose(arbc::Affine::translation(4.0, 0.0), arbc::Affine::scaling(4.0, 4.0)));
  const arbc::ObjectId c = document.add_content(
      std::make_shared<arbc::SolidContent>(arbc::Rgba{0.0F, 0.0F, 1.0F, 1.0F}, unit));
  const arbc::ObjectId lc = document.add_layer(
      c, compose(arbc::Affine::translation(0.0, 5.0), arbc::Affine::scaling(2.0, 2.0)));
  // Attach all three layers (creation/bottom-to-top order) to a composition so
  // the composition-scoped walk draws them (doc 05:28-36).
  const arbc::ObjectId comp = document.add_composition(8.0, 8.0);
  document.attach_layer(comp, la);
  document.attach_layer(comp, lb);
  document.attach_layer(comp, lc);

  arbc::CpuBackend cpu;
  // A counting decorator over the real backend: it forwards every call unchanged
  // but tallies make_surface, so this asserts allocation *behavior* (a behavioral
  // counter, doc 16 -- never a wall-clock timing).
  arbc::testing::CountingBackend backend(cpu);
  const arbc::Viewport viewport{8, 8, arbc::Affine::identity(), comp};
  const arbc::DocStatePtr state = document.pin();
  const auto resolve = [&document](arbc::ObjectId id) { return document.resolve(id); };

  arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError> target =
      backend.make_surface(viewport.width, viewport.height, arbc::k_working_rgba32f);
  REQUIRE(target.has_value());

  arbc::SurfacePool pool(backend);

  // --- the claim, on the pool itself -------------------------------------------
  //
  // Asserted DIRECTLY rather than inferred from a frame's allocation count, because the
  // tiled driver renders each miss straight into the cache-owned surface it will insert
  // and reaches `SurfacePool` only from `compose_coarser` (`tile_planning.cpp:156`), the
  // coarser-rung rescale. A frame over a cold cache therefore exercises the CACHE's
  // allocation path, not the pool's, and counting `make_surface` across frames cannot
  // tell the two apart. (The retired untiled driver did acquire a per-layer temp for
  // every layer, which is what this test used to measure.)
  backend.make_surface_calls = 0;
  {
    const auto first = pool.acquire(64, 64, arbc::k_working_rgba32f);
    REQUIRE(first.has_value());
    CHECK(backend.make_surface_calls == 1); // cold: the backend allocates
  } // released back to the free list here
  {
    const auto again = pool.acquire(64, 64, arbc::k_working_rgba32f);
    REQUIRE(again.has_value());
    CHECK(backend.make_surface_calls == 1); // SAME (size, format): recycled, not reallocated
    const auto other = pool.acquire(32, 64, arbc::k_working_rgba32f);
    REQUIRE(other.has_value());
    CHECK(backend.make_surface_calls == 2); // a DISTINCT key: one allocation, once
  }

  // --- and what the driver itself owes: a warm frame allocates nothing ----------
  arbc::TileCache warm(64U * 1024 * 1024);
  backend.make_surface_calls = 0;
  arbc::testing::render_once_exact(*state, resolve, viewport, warm, backend, pool, **target);
  const std::size_t cold_frame = backend.make_surface_calls;
  INFO("cold frame allocations: " << cold_frame);
  REQUIRE(cold_frame > 0); // a cold cache must allocate its tile surfaces

  backend.make_surface_calls = 0;
  arbc::testing::render_once_exact(*state, resolve, viewport, warm, backend, pool, **target);
  CHECK(backend.make_surface_calls ==
        0); // every tile resident: nothing rendered, nothing allocated
}

} // namespace
