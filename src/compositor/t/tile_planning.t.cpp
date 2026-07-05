#include <arbc/compositor/tile_planning.hpp>

#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/cache/key_shapes.hpp>
#include <arbc/cache/keyed_store.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/model/records.hpp>
#include <arbc/surface/surface.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>

// Exhaustive unit tests for the pure tile planner (doc 16:46). The planner is
// exercised against a hand-populated `TileCache` with no backend and no pool --
// `plan_layer` only reads and pins, so a minimal stub `Surface` (the planner
// never touches its pixels) is all the value needs. The end-to-end tiled
// composite is byte-exact-golden'd in `tests/tile_planning_golden.t.cpp`.

namespace {

using arbc::LayerTilePlan;
using arbc::PlannedTile;
using arbc::PriorityClass;
using arbc::Rect;
using arbc::RungSelection;
using arbc::ScaleRung;
using arbc::Stability;
using arbc::TileCache;
using arbc::TileCoord;
using arbc::TileKey;
using arbc::TileSource;
using arbc::TileValue;

// A do-nothing Surface: the planner reads only `TileValue.meta`, never the
// surface, so width/height/format are enough to satisfy `TileValue` ownership.
class StubSurface : public arbc::Surface {
public:
  int width() const override { return arbc::k_tile_size; }
  int height() const override { return arbc::k_tile_size; }
  arbc::SurfaceFormat format() const override { return arbc::k_working_rgba32f; }
  std::span<std::byte> cpu_bytes() override { return {}; }
  std::span<const std::byte> cpu_bytes() const override { return {}; }
};

// Resident-tile helper: insert returns a pinned hold; discarding it unpins so
// the entry stays resident and evictable, exactly a hand-populated cache.
void put_tile(TileCache& cache, const TileKey& key, double achieved_scale, bool exact) {
  cache.insert(key, TileValue{std::make_unique<StubSurface>(), {achieved_scale, exact}},
               /*bytes=*/1, PriorityClass::Visible);
}

int miss_count(const LayerTilePlan& plan) {
  int misses = 0;
  for (const PlannedTile& tile : plan.tiles) {
    if (tile.is_miss) {
      ++misses;
    }
  }
  return misses;
}

const arbc::ObjectId k_content{7};
constexpr std::uint64_t k_revision = 100;

// The rung-0 selection (scale 1.0): rung_scale 1, cell edge 256 local units.
RungSelection rung0() { return arbc::select_rung(1.0); }

LayerTilePlan plan_static(TileCache& cache, const RungSelection& sel, const Rect& region,
                          std::optional<std::uint64_t> prior = std::nullopt,
                          arbc::Time time = arbc::Time::zero(),
                          arbc::StateHandle snapshot = arbc::StateHandle{}) {
  return arbc::plan_layer(cache, k_content, k_revision, prior, sel, region,
                          arbc::Affine::identity(), Stability::Static, time, snapshot,
                          arbc::Deadline::none());
}

} // namespace

// --- Grid geometry -----------------------------------------------------------

TEST_CASE("tiles_covering: a region spanning 2.5 cells covers 3 columns") {
  // rung 0 -> cell edge 256 local units. x in [0, 640) = 2.5 cells.
  const std::vector<TileCoord> tiles = arbc::tiles_covering(ScaleRung{0}, Rect{0.0, 0.0, 640.0, 256.0});
  REQUIRE(tiles.size() == 3);
  CHECK(tiles[0] == TileCoord{0, 0});
  CHECK(tiles[1] == TileCoord{1, 0});
  CHECK(tiles[2] == TileCoord{2, 0});
}

TEST_CASE("tiles_covering: a region exactly on cell boundaries does not double-cover") {
  // [0, 512) x [0, 512) is exactly a 2x2 block of rung-0 cells, not 3x3.
  const std::vector<TileCoord> tiles = arbc::tiles_covering(ScaleRung{0}, Rect{0.0, 0.0, 512.0, 512.0});
  CHECK(tiles.size() == 4);
  for (const TileCoord coord : tiles) {
    CHECK(coord.col >= 0);
    CHECK(coord.col <= 1);
    CHECK(coord.row >= 0);
    CHECK(coord.row <= 1);
  }
}

TEST_CASE("tiles_covering: an empty region covers no cells") {
  CHECK(arbc::tiles_covering(ScaleRung{0}, Rect{5.0, 5.0, 5.0, 5.0}).empty());
  CHECK(arbc::tiles_covering(ScaleRung{0}, Rect{10.0, 0.0, 0.0, 10.0}).empty());
}

TEST_CASE("tile_local_rect round-trips through tiles_covering, incl. negative coords") {
  for (const ScaleRung rung : {ScaleRung{-2}, ScaleRung{0}, ScaleRung{3}}) {
    for (const TileCoord coord : {TileCoord{0, 0}, TileCoord{3, 5}, TileCoord{-2, -4}, TileCoord{-1, 2}}) {
      const Rect rect = arbc::tile_local_rect(rung, coord);
      const std::vector<TileCoord> covering = arbc::tiles_covering(rung, rect);
      // A single cell's own rect covers exactly that cell.
      REQUIRE(covering.size() == 1);
      CHECK(covering[0] == coord);
    }
  }
}

TEST_CASE("tile_local_rect: cell edge is k_tile_size / rung_scale across rungs") {
  for (const ScaleRung rung : {ScaleRung{-2}, ScaleRung{-1}, ScaleRung{0}, ScaleRung{1}, ScaleRung{3}}) {
    const Rect rect = arbc::tile_local_rect(rung, TileCoord{0, 0});
    const double expected_edge = static_cast<double>(arbc::k_tile_size) / arbc::rung_scale(rung);
    CHECK(rect.width() == expected_edge);
    CHECK(rect.height() == expected_edge);
  }
}

// --- Fresh hits, misses, and the deadline-carrying descriptor ----------------

// enforces: 02-architecture#miss-becomes-deadline-request
TEST_CASE("plan_layer: a warm cache of qualifying tiles plans every tile Fresh, zero misses") {
  TileCache cache(64u * 1024 * 1024);
  const RungSelection sel = rung0();
  const Rect region{0.0, 0.0, 512.0, 512.0}; // 2x2 rung-0 tiles

  // Pre-populate every covering tile with a qualifying fresh entry.
  for (const TileCoord coord : arbc::tiles_covering(sel.rung, region)) {
    put_tile(cache, TileKey{k_content, k_revision, sel.rung, coord, std::nullopt},
             arbc::rung_scale(sel.rung), /*exact=*/true);
  }

  const LayerTilePlan plan = plan_static(cache, sel, region);
  REQUIRE(plan.tiles.size() == 4);
  CHECK(miss_count(plan) == 0);
  for (const PlannedTile& tile : plan.tiles) {
    CHECK(tile.display_source == TileSource::Fresh);
    CHECK_FALSE(tile.is_miss);
    CHECK(tile.hold.valid());
    CHECK(tile.source_rung == sel.rung);
  }
}

// enforces: 02-architecture#miss-becomes-deadline-request
TEST_CASE("plan_layer: an empty cache marks every tile a miss owing a deadline request") {
  TileCache cache(64u * 1024 * 1024);
  const RungSelection sel = rung0();
  const Rect region{0.0, 0.0, 512.0, 256.0}; // 2 rung-0 tiles

  arbc::StateHandle snapshot;
  snapshot.slot = 5; // a non-default pinned handle to prove it is carried
  arbc::Deadline deadline;
  deadline.at = std::chrono::steady_clock::time_point{std::chrono::seconds{42}};

  const LayerTilePlan plan = arbc::plan_layer(cache, k_content, k_revision, std::nullopt, sel,
                                              region, arbc::Affine::identity(), Stability::Static,
                                              arbc::Time::zero(), snapshot, deadline);

  REQUIRE(plan.tiles.size() == 2);
  CHECK(miss_count(plan) == 2);
  // The plan carries the frame deadline and pinned snapshot verbatim.
  CHECK(plan.deadline == deadline);
  CHECK(plan.snapshot == snapshot);

  StubSurface target; // a target reference for the harness-materialized request
  for (const PlannedTile& tile : plan.tiles) {
    CHECK(tile.is_miss);
    CHECK(tile.display_source == TileSource::Placeholder);
    CHECK_FALSE(tile.hold.valid());

    // A miss materializes into exactly the tile's footprint at rung_scale, a
    // BestEffort request stamped with the frame deadline and pinned snapshot.
    const arbc::RenderRequest request{tile.local_rect,
                                      arbc::rung_scale(plan.rung),
                                      plan.time,
                                      plan.snapshot,
                                      target,
                                      arbc::Exactness::BestEffort,
                                      plan.deadline};
    CHECK(request.exactness == arbc::Exactness::BestEffort);
    CHECK(request.deadline == deadline);
    CHECK(request.snapshot == snapshot);
    CHECK(request.scale == arbc::rung_scale(sel.rung));
    CHECK(request.region == arbc::tile_local_rect(sel.rung, tile.coord));
  }
}

// --- Hit qualification -------------------------------------------------------

TEST_CASE("plan_layer: a best-effort or wrong-scale hit at the fresh key is not Fresh") {
  const RungSelection sel = rung0();
  const Rect region{0.0, 0.0, 256.0, 256.0}; // 1 rung-0 tile
  const TileCoord coord{0, 0};
  const TileKey fresh_key{k_content, k_revision, sel.rung, coord, std::nullopt};

  SECTION("exact == false falls through to placeholder") {
    TileCache cache(64u * 1024 * 1024);
    put_tile(cache, fresh_key, arbc::rung_scale(sel.rung), /*exact=*/false);
    const LayerTilePlan plan = plan_static(cache, sel, region);
    REQUIRE(plan.tiles.size() == 1);
    CHECK(plan.tiles[0].display_source == TileSource::Placeholder);
    CHECK(plan.tiles[0].is_miss);
  }

  SECTION("achieved_scale != rung_scale falls through to placeholder") {
    TileCache cache(64u * 1024 * 1024);
    put_tile(cache, fresh_key, arbc::rung_scale(sel.rung) * 2.0, /*exact=*/true);
    const LayerTilePlan plan = plan_static(cache, sel, region);
    REQUIRE(plan.tiles.size() == 1);
    CHECK(plan.tiles[0].display_source == TileSource::Placeholder);
    CHECK(plan.tiles[0].is_miss);
  }

  SECTION("a qualifying entry is Fresh") {
    TileCache cache(64u * 1024 * 1024);
    put_tile(cache, fresh_key, arbc::rung_scale(sel.rung), /*exact=*/true);
    const LayerTilePlan plan = plan_static(cache, sel, region);
    REQUIRE(plan.tiles.size() == 1);
    CHECK(plan.tiles[0].display_source == TileSource::Fresh);
    CHECK_FALSE(plan.tiles[0].is_miss);
  }
}

// --- Degradation preference order -------------------------------------------

// enforces: 02-architecture#degraded-fallback-preference-order
TEST_CASE("plan_layer: the degraded fallback preference order is fresh -> stale -> coarser -> placeholder") {
  const RungSelection sel = rung0();
  const Rect region{0.0, 0.0, 256.0, 256.0}; // 1 rung-0 tile
  const TileCoord coord{0, 0};
  constexpr std::uint64_t k_prior = 99;

  const TileKey stale_key{k_content, k_prior, sel.rung, coord, std::nullopt};
  // Octave-1 coarser tile at the current revision: coarser coord = floor(0/2) = 0.
  const TileKey coarser_key{k_content, k_revision, ScaleRung{sel.rung.index - 1}, TileCoord{0, 0},
                            std::nullopt};

  SECTION("stale present, no fresh -> Stale") {
    TileCache cache(64u * 1024 * 1024);
    put_tile(cache, stale_key, arbc::rung_scale(sel.rung), true);
    const LayerTilePlan plan = plan_static(cache, sel, region, k_prior);
    REQUIRE(plan.tiles.size() == 1);
    CHECK(plan.tiles[0].display_source == TileSource::Stale);
    CHECK(plan.tiles[0].is_miss); // fresh exact still absent -> render owed
    CHECK(plan.tiles[0].hold.valid());
  }

  SECTION("only a coarser tile -> Coarser, coarser rung recorded") {
    TileCache cache(64u * 1024 * 1024);
    put_tile(cache, coarser_key, arbc::rung_scale(coarser_key.rung), true);
    const LayerTilePlan plan = plan_static(cache, sel, region, k_prior);
    REQUIRE(plan.tiles.size() == 1);
    CHECK(plan.tiles[0].display_source == TileSource::Coarser);
    CHECK(plan.tiles[0].source_rung == ScaleRung{sel.rung.index - 1});
    CHECK(plan.tiles[0].is_miss);
    CHECK(plan.tiles[0].hold.valid());
  }

  SECTION("nothing resident -> Placeholder, no pin") {
    TileCache cache(64u * 1024 * 1024);
    const LayerTilePlan plan = plan_static(cache, sel, region, k_prior);
    REQUIRE(plan.tiles.size() == 1);
    CHECK(plan.tiles[0].display_source == TileSource::Placeholder);
    CHECK(plan.tiles[0].is_miss);
    CHECK_FALSE(plan.tiles[0].hold.valid());
  }

  SECTION("stale AND coarser both present -> Stale wins the order") {
    TileCache cache(64u * 1024 * 1024);
    put_tile(cache, stale_key, arbc::rung_scale(sel.rung), true);
    put_tile(cache, coarser_key, arbc::rung_scale(coarser_key.rung), true);
    const LayerTilePlan plan = plan_static(cache, sel, region, k_prior);
    REQUIRE(plan.tiles.size() == 1);
    CHECK(plan.tiles[0].display_source == TileSource::Stale);
  }

  SECTION("no prior_revision supplied -> stale is unreachable, coarser wins") {
    TileCache cache(64u * 1024 * 1024);
    put_tile(cache, stale_key, arbc::rung_scale(sel.rung), true);
    put_tile(cache, coarser_key, arbc::rung_scale(coarser_key.rung), true);
    const LayerTilePlan plan = plan_static(cache, sel, region, std::nullopt);
    REQUIRE(plan.tiles.size() == 1);
    CHECK(plan.tiles[0].display_source == TileSource::Coarser);
  }
}

// --- Static vs Timed keying and clock invariance -----------------------------

// enforces: 11-time-and-video#static-tiles-survive-clock
TEST_CASE("plan_layer: Static keys omit achieved_time and survive a clock advance") {
  const RungSelection sel = rung0();
  const Rect region{0.0, 0.0, 512.0, 256.0}; // 2 rung-0 tiles
  const arbc::Time t1{1000};
  const arbc::Time t2{5000};

  SECTION("Static content omits achieved_time; Timed content carries the raw time") {
    TileCache cache(64u * 1024 * 1024);
    const LayerTilePlan still = plan_static(cache, sel, region, std::nullopt, t1);
    for (const PlannedTile& tile : still.tiles) {
      CHECK_FALSE(tile.key.achieved_time.has_value());
    }
    const LayerTilePlan timed =
        arbc::plan_layer(cache, k_content, k_revision, std::nullopt, sel, region,
                         arbc::Affine::identity(), Stability::Timed, t1, arbc::StateHandle{},
                         arbc::Deadline::none());
    for (const PlannedTile& tile : timed.tiles) {
      REQUIRE(tile.key.achieved_time.has_value());
      CHECK(*tile.key.achieved_time == t1);
    }
  }

  SECTION("a warm all-Static scene re-plans to all fresh hits after only the clock advances") {
    TileCache cache(64u * 1024 * 1024);
    for (const TileCoord coord : arbc::tiles_covering(sel.rung, region)) {
      put_tile(cache, TileKey{k_content, k_revision, sel.rung, coord, std::nullopt},
               arbc::rung_scale(sel.rung), true);
    }

    const LayerTilePlan frame1 = plan_static(cache, sel, region, std::nullopt, t1);
    const LayerTilePlan frame2 = plan_static(cache, sel, region, std::nullopt, t2);

    REQUIRE(frame1.tiles.size() == frame2.tiles.size());
    CHECK(miss_count(frame1) == 0);
    CHECK(miss_count(frame2) == 0); // clock advance issued zero render requests
    for (std::size_t i = 0; i < frame1.tiles.size(); ++i) {
      // Clock-invariant: identical keys and all fresh across the advance.
      CHECK(frame1.tiles[i].key == frame2.tiles[i].key);
      CHECK(frame1.tiles[i].display_source == TileSource::Fresh);
      CHECK(frame2.tiles[i].display_source == TileSource::Fresh);
    }
  }
}

// --- Priority class ----------------------------------------------------------

TEST_CASE("plan_layer: every planned tile carries PriorityClass::Visible") {
  TileCache cache(64u * 1024 * 1024);
  const RungSelection sel = rung0();
  const LayerTilePlan plan = plan_static(cache, sel, Rect{0.0, 0.0, 512.0, 512.0});
  REQUIRE(plan.tiles.size() == 4);
  for (const PlannedTile& tile : plan.tiles) {
    CHECK(tile.klass == PriorityClass::Visible);
  }
}
