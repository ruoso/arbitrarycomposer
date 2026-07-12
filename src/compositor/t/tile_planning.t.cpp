#include <arbc/base/expected.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/cache/key_shapes.hpp>
#include <arbc/cache/keyed_store.hpp>
#include <arbc/compositor/compositor.hpp>
#include <arbc/compositor/refinement.hpp>
#include <arbc/compositor/tile_planning.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/model/model.hpp>
#include <arbc/model/records.hpp>
#include <arbc/surface/backend.hpp>
#include <arbc/surface/surface.hpp>
#include <arbc/surface/surface_error.hpp>
#include <arbc/surface/surface_pool.hpp>
#include <arbc/surface/testing/stub_backend.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <vector>

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

// --- Driver-seam fixtures (the `visible_plans` surfacing test) ----------------
// A CPU-buffer surface + a backend that folds a deterministic marker per
// composite, so the two-drive byte comparison in the surfacing test is
// meaningful (a dropped composite would not reproduce the bytes). Mirrors the
// runtime loop test's `BufferSurface`/`MarkBackend`.
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

class MarkBackend : public arbc::testing::StubBackend {
public:
  arbc::BackendCaps capabilities() const override { return {}; }
  arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError>
  make_surface(int width, int height, arbc::SurfaceFormat /*format*/) override {
    return std::unique_ptr<arbc::Surface>(std::make_unique<BufferSurface>(width, height));
  }
  void clear(arbc::Surface& surface, float /*r*/, float /*g*/, float /*b*/, float /*a*/) override {
    std::span<std::byte> bytes = surface.cpu_bytes();
    std::memset(bytes.data(), 0, bytes.size_bytes());
  }
  void composite(arbc::Surface& dst, const arbc::Surface& src, const arbc::Affine& /*m*/,
                 double opacity) override {
    const std::span<const std::byte> s = src.cpu_bytes();
    const unsigned seed = s.empty() ? 0u : std::to_integer<unsigned>(s[0]);
    const auto mark = (static_cast<unsigned>(opacity * 251.0) + 1u + seed) & 0xFFu;
    for (std::byte& b : dst.cpu_bytes()) {
      b = static_cast<std::byte>((std::to_integer<unsigned>(b) + mark) & 0xFFu);
    }
  }
};

// A Content that answers synchronously with an exact, at-scale result and fills
// its tile deterministically, so a cold-cache drive renders + inserts inline.
class SyncSolid : public arbc::Content {
public:
  std::optional<arbc::Rect> bounds() const override { return arbc::Rect{0.0, 0.0, 512.0, 512.0}; }
  Stability stability() const override { return Stability::Static; }
  std::optional<arbc::TimeRange> time_extent() const override { return std::nullopt; }
  std::optional<arbc::RenderResult>
  render(const arbc::RenderRequest& request,
         std::shared_ptr<arbc::RenderCompletion> /*done*/) override {
    const std::span<std::byte> bytes = request.target.cpu_bytes();
    for (std::size_t i = 0; i < bytes.size(); ++i) {
      bytes[i] = static_cast<std::byte>((i * 31u + 7u) & 0xFFu);
    }
    return arbc::RenderResult{request.scale, /*exact=*/true};
  }
};

} // namespace

// --- Grid geometry -----------------------------------------------------------

TEST_CASE("tiles_covering: a region spanning 2.5 cells covers 3 columns") {
  // rung 0 -> cell edge 256 local units. x in [0, 640) = 2.5 cells.
  const std::vector<TileCoord> tiles =
      arbc::tiles_covering(ScaleRung{0}, Rect{0.0, 0.0, 640.0, 256.0});
  REQUIRE(tiles.size() == 3);
  CHECK(tiles[0] == TileCoord{0, 0});
  CHECK(tiles[1] == TileCoord{1, 0});
  CHECK(tiles[2] == TileCoord{2, 0});
}

TEST_CASE("tiles_covering: a region exactly on cell boundaries does not double-cover") {
  // [0, 512) x [0, 512) is exactly a 2x2 block of rung-0 cells, not 3x3.
  const std::vector<TileCoord> tiles =
      arbc::tiles_covering(ScaleRung{0}, Rect{0.0, 0.0, 512.0, 512.0});
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
    for (const TileCoord coord :
         {TileCoord{0, 0}, TileCoord{3, 5}, TileCoord{-2, -4}, TileCoord{-1, 2}}) {
      const Rect rect = arbc::tile_local_rect(rung, coord);
      const std::vector<TileCoord> covering = arbc::tiles_covering(rung, rect);
      // A single cell's own rect covers exactly that cell.
      REQUIRE(covering.size() == 1);
      CHECK(covering[0] == coord);
    }
  }
}

TEST_CASE("tile_local_rect: cell edge is k_tile_size / rung_scale across rungs") {
  for (const ScaleRung rung :
       {ScaleRung{-2}, ScaleRung{-1}, ScaleRung{0}, ScaleRung{1}, ScaleRung{3}}) {
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
    const arbc::RenderRequest request{
        tile.local_rect, arbc::rung_scale(plan.rung), plan.time,    plan.snapshot,
        target,          arbc::Exactness::BestEffort, plan.deadline};
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

  SECTION("exact == false is not a hit -- it is the TRANSIENT fallback, and a render is owed") {
    TileCache cache(64u * 1024 * 1024);
    put_tile(cache, fresh_key, arbc::rung_scale(sel.rung), /*exact=*/false);
    const LayerTilePlan plan = plan_static(cache, sel, region);
    REQUIRE(plan.tiles.size() == 1);
    // The load-bearing half is unchanged and is what the hit gate is for: an inexact
    // entry is NOT a hit, so the render is still owed
    // (`13-effects-as-operators#transient-placeholder-is-never-exact`). What
    // `compositor.operator_refinement_wave_amplification` (Decision 3) changed is only
    // what gets SHOWN meanwhile: doc 02:62-67's degraded-fallback order gained a new
    // first entry, and a resident, current-revision, current-rung, merely-inexact tile
    // -- an operator's transient placeholder -- is strictly better than the transparent
    // one this used to fall through to. It is the same content at the same revision and
    // the same rung, simply not final; painting transparent over it would blink the
    // layer for every frame of the refinement wave and then pop in.
    CHECK(plan.tiles[0].display_source == TileSource::Transient);
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
TEST_CASE("plan_layer: the degraded fallback preference order is fresh -> stale -> coarser -> "
          "placeholder") {
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
    const LayerTilePlan timed = arbc::plan_layer(cache, k_content, k_revision, std::nullopt, sel,
                                                 region, arbc::Affine::identity(), Stability::Timed,
                                                 t1, arbc::StateHandle{}, arbc::Deadline::none());
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

// --- Surfacing the composited plan (the `visible_plans` seam) -----------------

// enforces: 02-architecture#speculation-drives-from-exposed-plan
TEST_CASE("render_frame_interactive: the surfaced plan equals the composited plan; null is inert") {
  arbc::Model model;
  arbc::ObjectId content_id{};
  {
    auto txn = model.transact();
    content_id = txn.add_content(0);
    txn.add_layer(content_id, arbc::Affine::identity());
    REQUIRE(txn.commit().has_value());
  }
  const arbc::DocStatePtr state = model.current();
  SyncSolid content;
  const auto resolver = [&](arbc::ObjectId id) -> arbc::Content* {
    return id == content_id ? &content : nullptr;
  };
  const arbc::Viewport viewport{256, 256, arbc::Affine::identity()}; // one rung-0 tile
  const TileKey visible_key{content_id, state->revision(), ScaleRung{0}, TileCoord{0, 0},
                            std::nullopt};

  // Drive A: with a plan sink. A cold cache renders the tile inline (Fresh) and
  // surfaces its plan.
  MarkBackend backend_a;
  arbc::SurfacePool pool_a(backend_a);
  TileCache cache_a(64u * 1024 * 1024);
  auto target_a = backend_a.make_surface(256, 256, arbc::k_working_rgba32f);
  REQUIRE(target_a.has_value());
  std::vector<LayerTilePlan> plans;
  arbc::render_frame_interactive(*state, resolver, viewport, cache_a, backend_a, pool_a, **target_a,
                                 arbc::Deadline::none(), std::nullopt, nullptr, nullptr, nullptr,
                                 arbc::Time::zero(), &plans);
  const std::uint64_t hits_after_plan = cache_a.hits();
  const std::uint64_t misses_after_plan = cache_a.misses();

  // One entry per visible layer, in composite order, carrying exactly the tile the
  // frame composited (same key, same post-composite display source).
  REQUIRE(plans.size() == 1);
  CHECK(plans[0].content == content_id);
  CHECK(plans[0].rung == ScaleRung{0});
  REQUIRE(plans[0].tiles.size() == 1);
  CHECK(plans[0].tiles[0].key == visible_key);
  CHECK(plans[0].tiles[0].display_source == TileSource::Fresh);

  // The surfaced plan drives prime_prefetch with no re-plan: the pan annulus of the
  // single visible rung-0 tile is the 8 absent Chebyshev neighbours, and the prime
  // probes the cache but composites/renders nothing.
  const std::vector<arbc::TileKey> want =
      arbc::prime_prefetch(cache_a, plans[0], /*zoom_direction=*/0, /*pan_radius=*/1);
  CHECK(want.size() == 8);
  CHECK(cache_a.misses() > misses_after_plan); // the prime probed the ring keys

  // Drive B: identical drive with a null sink. Byte-identical target and identical
  // plan-pass probe counts -- the seam is inert when unused.
  MarkBackend backend_b;
  arbc::SurfacePool pool_b(backend_b);
  TileCache cache_b(64u * 1024 * 1024);
  auto target_b = backend_b.make_surface(256, 256, arbc::k_working_rgba32f);
  REQUIRE(target_b.has_value());
  arbc::render_frame_interactive(*state, resolver, viewport, cache_b, backend_b, pool_b, **target_b,
                                 arbc::Deadline::none(), std::nullopt, nullptr, nullptr, nullptr,
                                 arbc::Time::zero(), nullptr);

  CHECK(hits_after_plan == cache_b.hits());
  CHECK(misses_after_plan == cache_b.misses()); // surfacing added no probe over the plan pass
  const std::span<const std::byte> bytes_a = (*target_a)->cpu_bytes();
  const std::span<const std::byte> bytes_b = (*target_b)->cpu_bytes();
  REQUIRE(bytes_a.size() == bytes_b.size());
  CHECK(std::memcmp(bytes_a.data(), bytes_b.data(), bytes_a.size_bytes()) == 0);
}
