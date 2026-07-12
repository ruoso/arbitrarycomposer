#include <arbc/base/expected.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/cache/key_shapes.hpp>
#include <arbc/cache/keyed_store.hpp>
#include <arbc/compositor/compositor.hpp>
#include <arbc/compositor/counters.hpp>
#include <arbc/compositor/damage_planning.hpp>
#include <arbc/compositor/tile_planning.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/model/damage.hpp>
#include <arbc/model/model.hpp>
#include <arbc/surface/backend.hpp>
#include <arbc/surface/surface.hpp>
#include <arbc/surface/surface_error.hpp>
#include <arbc/surface/surface_pool.hpp>
#include <arbc/surface/testing/stub_backend.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

// Unit tests for the pure damage planner (doc 16:46). `map_damage_to_device`
// and `clock_advance_damage` run against a hand-built scene with no backend;
// `invalidate_damage` against a hand-populated `TileCache`; the driver-gating
// seam against a stub backend/content pinning counts (doc 16:54-62), never
// timings. The end-to-end byte-exact composite is golden'd in
// `tests/damage_planning_golden.t.cpp`.

namespace {

using arbc::CompositorCounters;
using arbc::Damage;
using arbc::DirtyRegion;
using arbc::PriorityClass;
using arbc::Rect;
using arbc::RenderResult;
using arbc::ScaleRung;
using arbc::Stability;
using arbc::TileCache;
using arbc::TileCoord;
using arbc::TileKey;
using arbc::TileValue;

// A do-nothing Surface: the planner/invalidator reads only tile geometry and
// metadata, never pixels.
class StubSurface : public arbc::Surface {
public:
  int width() const override { return arbc::k_tile_size; }
  int height() const override { return arbc::k_tile_size; }
  arbc::SurfaceFormat format() const override { return arbc::k_working_rgba32f; }
  std::span<std::byte> cpu_bytes() override { return {}; }
  std::span<const std::byte> cpu_bytes() const override { return {}; }
};

// A CPU-buffer surface so the driver-gating counter tests exercise a real
// composite path (the counts, not the bytes, are under test here).
class BufferSurface : public arbc::Surface {
public:
  BufferSurface(int width, int height)
      : d_width(width), d_height(height),
        d_bytes(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 16,
                std::byte{0}) {}
  int width() const override { return d_width; }
  int height() const override { return d_height; }
  arbc::SurfaceFormat format() const override { return arbc::k_working_rgba32f; }
  std::span<std::byte> cpu_bytes() override { return d_bytes; }
  std::span<const std::byte> cpu_bytes() const override { return d_bytes; }

private:
  int d_width;
  int d_height;
  std::vector<std::byte> d_bytes;
};

// Allocates real-buffer surfaces; composite/clear touch bytes but count nothing
// themselves -- `CompositorCounters` is the counter under test.
class MarkBackend : public arbc::testing::StubBackend {
public:
  arbc::BackendCaps capabilities() const override { return {}; }
  arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError>
  make_surface(int width, int height, arbc::SurfaceFormat /*format*/) override {
    return std::unique_ptr<arbc::Surface>(std::make_unique<BufferSurface>(width, height));
  }
  void clear(arbc::Surface& /*surface*/, float /*r*/, float /*g*/, float /*b*/,
             float /*a*/) override {}
  void composite(arbc::Surface& /*dst*/, const arbc::Surface& /*src*/, const arbc::Affine& /*m*/,
                 double /*opacity*/) override {}
};

// Content of a fixed stability that answers synchronously, exact, at scale.
class StubContent : public arbc::Content {
public:
  explicit StubContent(Stability stability) : d_stability(stability) {}
  std::optional<Rect> bounds() const override { return Rect{0.0, 0.0, 512.0, 512.0}; }
  Stability stability() const override { return d_stability; }
  std::optional<arbc::TimeRange> time_extent() const override {
    return d_stability == Stability::Static
               ? std::optional<arbc::TimeRange>(std::nullopt)
               : std::optional<arbc::TimeRange>(arbc::TimeRange::all());
  }
  std::optional<RenderResult> render(const arbc::RenderRequest& request,
                                     std::shared_ptr<arbc::RenderCompletion> /*done*/) override {
    return RenderResult{request.scale, /*exact=*/true};
  }

private:
  Stability d_stability;
};

// A single identity-transform layer bound to a fresh content id.
arbc::ObjectId add_layer(arbc::Model& model, const arbc::Affine& transform) {
  auto txn = model.transact();
  const arbc::ObjectId content_id = txn.add_content(0);
  txn.add_layer(content_id, transform);
  REQUIRE(txn.commit().has_value());
  return content_id;
}

// Resident-tile helper: insert returns a pinned hold; discarding it unpins so
// the entry stays resident and lookup-able (a hand-populated cache).
void put_tile(TileCache& cache, arbc::ObjectId content, ScaleRung rung, TileCoord coord) {
  const TileKey key{content, /*revision=*/1, rung, coord, /*achieved_time=*/std::nullopt};
  cache.insert(key, TileValue{std::make_unique<StubSurface>(), {arbc::rung_scale(rung), true}},
               /*bytes=*/1, PriorityClass::Visible);
}

bool resident(TileCache& cache, arbc::ObjectId content, ScaleRung rung, TileCoord coord) {
  const TileKey key{content, /*revision=*/1, rung, coord, /*achieved_time=*/std::nullopt};
  return cache.lookup(key).has_value();
}

} // namespace

// A 512x512 identity scene splits into a 2x2 grid of 256^2 rung-0 tiles.
constexpr std::uint64_t k_tiles_covered = 4;

// enforces: 02-architecture#damage-maps-to-device-dirty-regions
TEST_CASE("map_damage_to_device projects content damage through the camera to device rects") {
  arbc::Model model;
  const arbc::ObjectId content = add_layer(model, arbc::Affine::identity());
  const arbc::DocStatePtr state = model.current();
  const arbc::Time now = arbc::Time::zero();

  SECTION("identity camera maps the content rect one-to-one") {
    const arbc::Viewport viewport{512, 512, arbc::Affine::identity()};
    const Damage d{content, Rect{10.0, 20.0, 110.0, 120.0}, arbc::TimeRange::all()};
    const std::vector<Rect> rects =
        arbc::map_damage_to_device(*state, viewport, std::span(&d, 1), now);
    REQUIRE(rects.size() == 1);
    CHECK(rects[0] == Rect{10.0, 20.0, 110.0, 120.0});
  }

  SECTION("a scaled camera maps the rect correspondingly") {
    const arbc::Viewport viewport{512, 512, arbc::Affine::scaling(2.0, 2.0)};
    const Damage d{content, Rect{10.0, 20.0, 110.0, 120.0}, arbc::TimeRange::all()};
    const std::vector<Rect> rects =
        arbc::map_damage_to_device(*state, viewport, std::span(&d, 1), now);
    REQUIRE(rects.size() == 1);
    CHECK(rects[0] == Rect{20.0, 40.0, 220.0, 240.0});
  }

  SECTION("damage wholly outside the viewport clips to empty and is dropped") {
    const arbc::Viewport viewport{512, 512, arbc::Affine::identity()};
    const Damage d{content, Rect{600.0, 600.0, 700.0, 700.0}, arbc::TimeRange::all()};
    CHECK(arbc::map_damage_to_device(*state, viewport, std::span(&d, 1), now).empty());
  }

  SECTION("structural infinite damage maps to the full viewport rect") {
    const arbc::Viewport viewport{512, 512, arbc::Affine::identity()};
    const Damage d{content, Rect::infinite(), arbc::TimeRange::all()};
    const std::vector<Rect> rects =
        arbc::map_damage_to_device(*state, viewport, std::span(&d, 1), now);
    REQUIRE(rects.size() == 1);
    CHECK(rects[0] == Rect{0.0, 0.0, 512.0, 512.0});
  }

  SECTION("empty input yields an empty dirty region -- no damage, no work") {
    const arbc::Viewport viewport{512, 512, arbc::Affine::identity()};
    CHECK(arbc::map_damage_to_device(*state, viewport, std::span<const Damage>(), now).empty());
  }
}

// enforces: 02-architecture#damage-maps-to-device-dirty-regions
TEST_CASE("map_damage_to_device gates on the displayed instant") {
  arbc::Model model;
  const arbc::ObjectId content = add_layer(model, arbc::Affine::identity());
  const arbc::DocStatePtr state = model.current();
  const arbc::Viewport viewport{512, 512, arbc::Affine::identity()};
  const Rect rect{10.0, 20.0, 110.0, 120.0};

  SECTION("a range excluding now drops the damage") {
    const Damage d{content, rect, arbc::TimeRange{arbc::Time{10}, arbc::Time{20}}};
    CHECK(arbc::map_damage_to_device(*state, viewport, std::span(&d, 1), arbc::Time{5}).empty());
  }

  SECTION("a range containing now keeps the damage") {
    const Damage d{content, rect, arbc::TimeRange{arbc::Time{10}, arbc::Time{20}}};
    CHECK(arbc::map_damage_to_device(*state, viewport, std::span(&d, 1), arbc::Time{15}).size() ==
          1);
  }

  SECTION("a degenerate instant range (refinement-arrival shape) is present-frame damage") {
    // TimeRange{when, when} is empty under TimeRange::empty(); the gate reads it
    // as this instant, not no-time, so an async arrival is never dropped.
    const Damage d{content, rect, arbc::TimeRange{arbc::Time{7}, arbc::Time{7}}};
    CHECK(arbc::map_damage_to_device(*state, viewport, std::span(&d, 1), arbc::Time{999}).size() ==
          1);
  }
}

// enforces: 11-time-and-video#clock-advance-damages-only-moving-layers
// enforces: 11-time-and-video#static-tiles-survive-clock
TEST_CASE("clock_advance_damage damages only the moving layers") {
  arbc::Model model;
  arbc::ObjectId static_content;
  arbc::ObjectId timed_content;
  {
    auto txn = model.transact();
    static_content = txn.add_content(0);
    timed_content = txn.add_content(0);
    txn.add_layer(static_content, arbc::Affine::identity());
    txn.add_layer(timed_content, arbc::Affine::identity());
    REQUIRE(txn.commit().has_value());
  }
  const arbc::DocStatePtr state = model.current();
  StubContent still(Stability::Static);
  StubContent moving(Stability::Timed);
  const auto resolver = [&](arbc::ObjectId id) -> arbc::Content* {
    if (id == static_content) {
      return &still;
    }
    if (id == timed_content) {
      return &moving;
    }
    return nullptr;
  };
  const arbc::Viewport viewport{512, 512, arbc::Affine::identity()};
  const arbc::TimeRange advanced{arbc::Time{0}, arbc::Time{100}};

  SECTION("a Static+Timed scene emits exactly one damage for the Timed layer") {
    const std::vector<Damage> damage =
        arbc::clock_advance_damage(*state, resolver, viewport, advanced);
    REQUIRE(damage.size() == 1);
    CHECK(damage[0].object == timed_content);
    CHECK(damage[0].rect == Rect::infinite());
    CHECK(damage[0].range == advanced);
  }

  SECTION("an all-Static scene emits nothing -- still tiles survive the clock") {
    const auto static_only = [&](arbc::ObjectId) -> arbc::Content* { return &still; };
    CHECK(arbc::clock_advance_damage(*state, static_only, viewport, advanced).empty());
  }

  SECTION("an empty advance emits nothing") {
    const arbc::TimeRange none{arbc::Time{5}, arbc::Time{5}};
    CHECK(arbc::clock_advance_damage(*state, resolver, viewport, none).empty());
  }
}

// enforces: 02-architecture#damage-invalidates-by-content-region-across-rungs
TEST_CASE("invalidate_damage drops the damaged content's tiles across rungs") {
  TileCache cache(64u * 1024 * 1024);
  const arbc::ObjectId damaged{1};
  const arbc::ObjectId other{2};
  const ScaleRung rung0{0}; // cell 256 local units
  const ScaleRung rung1{1}; // cell 128 local units (finer)

  // Two tiles of `damaged` intersecting region [0,0,100,100] at different rungs,
  // one `damaged` tile outside the region, and one `other`-content tile.
  put_tile(cache, damaged, rung0, TileCoord{0, 0});   // footprint [0,0,256,256]
  put_tile(cache, damaged, rung1, TileCoord{0, 0});   // footprint [0,0,128,128]
  put_tile(cache, damaged, rung0, TileCoord{10, 10}); // footprint [2560,..] -- outside
  put_tile(cache, other, rung0, TileCoord{0, 0});

  const std::uint64_t evictions_before = cache.evictions();

  const Damage region_damage{damaged, Rect{0.0, 0.0, 100.0, 100.0}, arbc::TimeRange::all()};
  const std::size_t dropped = arbc::invalidate_damage(cache, std::span(&region_damage, 1));

  CHECK(dropped == 2);
  CHECK_FALSE(resident(cache, damaged, rung0, TileCoord{0, 0}));
  CHECK_FALSE(resident(cache, damaged, rung1, TileCoord{0, 0}));
  CHECK(resident(cache, damaged, rung0, TileCoord{10, 10})); // outside region: survives
  CHECK(resident(cache, other, rung0, TileCoord{0, 0}));     // other content: survives
  CHECK(cache.evictions() == evictions_before);              // drops are not evictions

  // Structural infinite region routes to the wholesale drop: the last `damaged`
  // tile goes, `other` survives.
  const Damage structural{damaged, Rect::infinite(), arbc::TimeRange::all()};
  CHECK(arbc::invalidate_damage(cache, std::span(&structural, 1)) == 1);
  CHECK_FALSE(resident(cache, damaged, rung0, TileCoord{10, 10}));
  CHECK(resident(cache, other, rung0, TileCoord{0, 0}));
  CHECK(cache.evictions() == evictions_before);
}

// enforces: 02-architecture#damage-maps-to-device-dirty-regions
TEST_CASE("render_frame_interactive gates work to the dirty region (counter-backed)") {
  MarkBackend backend;
  StubContent content(Stability::Static);
  arbc::Model model;
  const arbc::ObjectId content_id = add_layer(model, arbc::Affine::identity());
  const arbc::DocStatePtr state = model.current();
  const auto resolver = [&](arbc::ObjectId id) -> arbc::Content* {
    return id == content_id ? &content : nullptr;
  };
  const arbc::Viewport viewport{512, 512, arbc::Affine::identity()};
  arbc::SurfacePool pool(backend);

  auto drive = [&](const DirtyRegion* dirty, CompositorCounters& counters) {
    TileCache cache(64u * 1024 * 1024); // cold: every planned tile is a miss
    arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError> target =
        backend.make_surface(512, 512, arbc::k_working_rgba32f);
    REQUIRE(target.has_value());
    arbc::render_frame_interactive(*state, resolver, viewport, cache, backend, pool, **target,
                                   arbc::Deadline::none(), std::nullopt, nullptr, &counters, dirty);
  };

  SECTION("a dirty region covering one tile plans only that tile") {
    DirtyRegion dirty{{Rect{0.0, 0.0, 256.0, 256.0}}};
    CompositorCounters counters;
    drive(&dirty, counters);
    CHECK(counters.requests_issued() == 1);
    CHECK(counters.composites() == 1);
  }

  SECTION("a non-null empty dirty region plans nothing -- zero renders, zero composites") {
    DirtyRegion dirty{};
    CompositorCounters counters;
    drive(&dirty, counters);
    CHECK(counters.requests_issued() == 0);
    CHECK(counters.composites() == 0);
  }

  SECTION("a null dirty pointer plans the whole viewport") {
    CompositorCounters counters;
    drive(nullptr, counters);
    CHECK(counters.requests_issued() == k_tiles_covered);
    CHECK(counters.composites() == k_tiles_covered);
  }
}

// enforces: 02-architecture#gated-frame-touches-only-its-repaint-region
TEST_CASE("repaint_region is the rounded-out bbox of the dirty rects, viewport-clipped") {
  const arbc::Viewport viewport{512, 512, arbc::Affine::identity()};

  SECTION("an empty region repaints nothing") {
    CHECK(arbc::repaint_region(DirtyRegion{}, viewport).empty());
  }

  SECTION("a single rect rounds OUT to whole device pixels") {
    // Rounding IN would leave a sub-pixel fringe of the true damage unpainted --
    // a stale seam. Out is conservative in the safe direction.
    const DirtyRegion dirty{{arbc::Rect{10.25, 20.75, 30.5, 40.1}}};
    CHECK(arbc::repaint_region(dirty, viewport) == arbc::Rect{10.0, 20.0, 31.0, 41.0});
  }

  SECTION("several rects collapse to their bounding box") {
    // Two disjoint corners repaint everything between them: waste, not
    // incorrectness (`compositor.disjoint_dirty_repaint` is where the precision
    // goes). One rect means one composite per tile, which is what keeps a tile in
    // an overlap of two dirty rects from being composited twice.
    const DirtyRegion dirty{{arbc::Rect{0.0, 0.0, 16.0, 16.0},
                             arbc::Rect{100.0, 200.0, 110.0, 210.0},
                             arbc::Rect{50.0, 50.0, 60.0, 60.0}}};
    CHECK(arbc::repaint_region(dirty, viewport) == arbc::Rect{0.0, 0.0, 110.0, 210.0});
  }

  SECTION("rects are clipped to the viewport, and a structural infinite rect saturates") {
    const DirtyRegion out_of_view{{arbc::Rect{600.0, 600.0, 700.0, 700.0}}};
    CHECK(arbc::repaint_region(out_of_view, viewport).empty());

    const DirtyRegion straddling{{arbc::Rect{480.0, 480.0, 900.0, 900.0}}};
    CHECK(arbc::repaint_region(straddling, viewport) == arbc::Rect{480.0, 480.0, 512.0, 512.0});

    // The whole-plane rect must clip to the viewport, not take the box to
    // infinity (it is absorbing under `rect_union`).
    const DirtyRegion structural{{arbc::Rect{4.0, 4.0, 8.0, 8.0}, arbc::Rect::infinite()}};
    CHECK(arbc::repaint_region(structural, viewport) == arbc::Rect{0.0, 0.0, 512.0, 512.0});
  }
}
