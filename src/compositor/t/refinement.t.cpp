#include <arbc/base/expected.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/cache/key_shapes.hpp>
#include <arbc/cache/keyed_store.hpp>
#include <arbc/compositor/compositor.hpp>
#include <arbc/compositor/refinement.hpp>
#include <arbc/compositor/scale_ladder.hpp>
#include <arbc/compositor/tile_planning.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/model/model.hpp>
#include <arbc/model/records.hpp>
#include <arbc/surface/backend.hpp>
#include <arbc/surface/surface.hpp>
#include <arbc/surface/surface_error.hpp>
#include <arbc/surface/surface_pool.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

// Unit tests for the progressive-refinement + zoom-speculation layer
// (doc 16:46). The pure ring/queue logic is exercised against hand-built plans,
// a hand-populated `TileCache`, and a test `Content` that answers async -- no
// backend for the ring/queue tests; a minimal stub backend only where the driver
// seam is driven. The end-to-end coarse-then-refine composite is byte-exact
// golden'd in `tests/refinement_golden.t.cpp`.

namespace {

using arbc::Damage;
using arbc::LayerTilePlan;
using arbc::PendingTile;
using arbc::PlannedTile;
using arbc::PriorityClass;
using arbc::Rect;
using arbc::RefinementQueue;
using arbc::RenderCompletion;
using arbc::RenderError;
using arbc::RenderResult;
using arbc::RungSelection;
using arbc::ScaleRung;
using arbc::TileCache;
using arbc::TileCoord;
using arbc::TileKey;
using arbc::TileValue;

const arbc::ObjectId k_content{7};
constexpr std::uint64_t k_revision = 100;

// A do-nothing Surface: nothing in these tests reads its pixels; width/height/
// format are enough for `TileValue` ownership and `tile_byte_cost`.
class StubSurface : public arbc::Surface {
public:
  int width() const override { return arbc::k_tile_size; }
  int height() const override { return arbc::k_tile_size; }
  arbc::SurfaceFormat format() const override { return arbc::k_working_rgba32f; }
  std::span<std::byte> cpu_bytes() override { return {}; }
  std::span<const std::byte> cpu_bytes() const override { return {}; }
};

// A stub backend that hands back format-agnostic stub surfaces and no-ops the
// composite ops -- the driver-seam tests only care that misses are recorded, not
// the pixels (byte-exactness is the golden's job).
class StubBackend : public arbc::Backend {
public:
  arbc::BackendCaps capabilities() const override { return {}; }
  arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError>
  make_surface(int /*width*/, int /*height*/, arbc::SurfaceFormat /*format*/) override {
    return std::unique_ptr<arbc::Surface>(std::make_unique<StubSurface>());
  }
  void clear(arbc::Surface& /*surface*/, float /*r*/, float /*g*/, float /*b*/,
             float /*a*/) override {}
  void composite(arbc::Surface& /*dst*/, const arbc::Surface& /*src*/,
                 const arbc::Affine& /*src_to_dst*/, double /*opacity*/) override {}
  void downsample(arbc::Surface& /*dst*/, const arbc::Surface& /*src*/) override {}
};

// A Content that always answers async: it returns `nullopt` (leaving the
// driver's `RenderCompletion` live to be recorded) and counts its render calls,
// so a quiescent frame's zero-miss property is observable.
class AsyncContent : public arbc::Content {
public:
  std::optional<arbc::Rect> bounds() const override { return std::nullopt; }
  arbc::Stability stability() const override { return arbc::Stability::Static; }
  std::optional<arbc::TimeRange> time_extent() const override { return std::nullopt; }
  std::optional<arbc::RenderResult> render(const arbc::RenderRequest& /*request*/,
                                           std::shared_ptr<arbc::RenderCompletion> /*done*/)
      override {
    ++d_renders;
    return std::nullopt;
  }
  int renders() const { return d_renders; }

private:
  int d_renders{0};
};

TileKey tile_key(ScaleRung rung, TileCoord coord, std::optional<arbc::Time> time = std::nullopt) {
  return TileKey{k_content, k_revision, rung, coord, time};
}

// Insert a resident entry of a given class/bytes and drop the pinned hold so the
// entry stays resident and evictable (a hand-populated cache).
void put(TileCache& cache, const TileKey& key, std::size_t bytes, PriorityClass klass) {
  cache.insert(key, TileValue{std::make_unique<StubSurface>(), {1.0, true}}, bytes, klass);
}

bool contains(const std::vector<TileKey>& keys, const TileKey& key) {
  for (const TileKey& k : keys) {
    if (k == key) {
      return true;
    }
  }
  return false;
}

// A single-visible-tile plan at rung 0 covering [0,0,256,256], hand-built so the
// prime tests do not touch the planner or pin anything.
LayerTilePlan single_tile_plan() {
  LayerTilePlan plan;
  plan.content = k_content;
  plan.rung = ScaleRung{0};
  plan.remainder = 1.0;
  PlannedTile tile;
  tile.coord = TileCoord{0, 0};
  tile.local_rect = arbc::tile_local_rect(ScaleRung{0}, TileCoord{0, 0});
  tile.key = tile_key(ScaleRung{0}, TileCoord{0, 0});
  plan.tiles.push_back(std::move(tile));
  return plan;
}

} // namespace

// --- Zoom-ring geometry ------------------------------------------------------

TEST_CASE("zoom_prefetch_ring re-tiles the visible region at the neighbouring rung") {
  const RungSelection current = arbc::select_rung(1.0); // rung 0, cell 256
  const Rect region{0.0, 0.0, 256.0, 256.0};

  SECTION("zoom-in (direction < 0) -> the finer rung+1: more, smaller tiles") {
    const std::vector<TileKey> ring =
        arbc::zoom_prefetch_ring(current, region, k_content, k_revision, std::nullopt, -1);
    // rung 1 cell = 256/2 = 128 -> [0,256) covers 2 cols x 2 rows = 4 tiles.
    REQUIRE(ring.size() == 4);
    for (const TileKey& key : ring) {
      CHECK(key.rung.index == 1);       // the finer, next rung
      CHECK(key.revision == k_revision); // carries the current revision
      CHECK_FALSE(key.achieved_time.has_value()); // Static: clock-invariant key
      CHECK(key.content == k_content);
    }
  }

  SECTION("zoom-out (direction > 0) -> the coarser rung-1: fewer tiles") {
    const std::vector<TileKey> ring =
        arbc::zoom_prefetch_ring(current, region, k_content, k_revision, std::nullopt, 1);
    // rung -1 cell = 256/0.5 = 512 -> [0,256) covers 1 col x 1 row = 1 tile.
    REQUIRE(ring.size() == 1);
    CHECK(ring.front().rung.index == -1);
  }

  SECTION("no gesture (direction == 0) -> empty ring") {
    CHECK(arbc::zoom_prefetch_ring(current, region, k_content, k_revision, std::nullopt, 0).empty());
  }

  SECTION("Timed content carries the requested time into every key") {
    const arbc::Time when{500};
    const std::vector<TileKey> ring =
        arbc::zoom_prefetch_ring(current, region, k_content, k_revision, when, -1);
    REQUIRE_FALSE(ring.empty());
    for (const TileKey& key : ring) {
      REQUIRE(key.achieved_time.has_value());
      CHECK(*key.achieved_time == when);
    }
  }
}

// --- Priming invariant -------------------------------------------------------

// enforces: 04-transforms-and-infinite-zoom#zoom-speculates-next-rung
TEST_CASE("prime_prefetch primes the next rung, reports the absent want-list, evicts nothing") {
  const LayerTilePlan plan = single_tile_plan();

  SECTION("want-list is exactly the absent zoom-ring members; residency unchanged") {
    TileCache cache(64u * 1024 * 1024);
    // Zoom-in ring at rung 1 is coords (0,0),(1,0),(0,1),(1,1). Two resident.
    put(cache, tile_key(ScaleRung{1}, TileCoord{0, 0}), 1, PriorityClass::Visible);
    put(cache, tile_key(ScaleRung{1}, TileCoord{1, 1}), 1, PriorityClass::Visible);

    const std::size_t bytes_before = cache.resident_bytes();
    const std::uint64_t evictions_before = cache.evictions();

    // pan_radius 0 -> the pan ring is empty, isolating the zoom ring.
    const std::vector<TileKey> want =
        arbc::prime_prefetch(cache, plan, /*zoom_direction=*/-1, /*pan_radius=*/0);

    // Only the two absent ring members are reported; the resident ones are not.
    REQUIRE(want.size() == 2);
    CHECK(contains(want, tile_key(ScaleRung{1}, TileCoord{1, 0})));
    CHECK(contains(want, tile_key(ScaleRung{1}, TileCoord{0, 1})));
    CHECK_FALSE(contains(want, tile_key(ScaleRung{1}, TileCoord{0, 0})));
    CHECK_FALSE(contains(want, tile_key(ScaleRung{1}, TileCoord{1, 1})));

    // Rendered nothing, inserted nothing, evicted nothing.
    CHECK(cache.resident_bytes() == bytes_before);
    CHECK(cache.evictions() == evictions_before);
  }

  SECTION("resident zoom-ring tiles are reclassified Speculative") {
    // Tight budget holding exactly three 40-byte tiles. The filler is inserted
    // FIRST (so it is LRU): if the ring tiles stayed Visible it would be the
    // eviction victim; reclassifying them to Speculative (a lower class) makes a
    // ring tile the victim instead and spares the Visible filler.
    TileCache tight(120);
    put(tight, tile_key(ScaleRung{1}, TileCoord{9, 9}), 40, PriorityClass::Visible); // filler
    put(tight, tile_key(ScaleRung{1}, TileCoord{0, 0}), 40, PriorityClass::Visible);
    put(tight, tile_key(ScaleRung{1}, TileCoord{1, 1}), 40, PriorityClass::Visible);

    arbc::prime_prefetch(tight, plan, /*zoom_direction=*/-1, /*pan_radius=*/0);

    // Force one eviction: the lowest class with a victim is now Speculative.
    put(tight, tile_key(ScaleRung{1}, TileCoord{5, 5}), 40, PriorityClass::Visible);
    CHECK(tight.evictions() == 1);
    CHECK(tight.lookup(tile_key(ScaleRung{1}, TileCoord{9, 9})).has_value()); // Visible spared
    const int resident_ring =
        (tight.lookup(tile_key(ScaleRung{1}, TileCoord{0, 0})).has_value() ? 1 : 0) +
        (tight.lookup(tile_key(ScaleRung{1}, TileCoord{1, 1})).has_value() ? 1 : 0);
    CHECK(resident_ring == 1); // exactly one reclassified-Speculative ring tile evicted
  }

  SECTION("the pan ring is primed under Adjacent and contributes to the want-list") {
    TileCache cache(64u * 1024 * 1024);
    const std::vector<TileKey> want =
        arbc::prime_prefetch(cache, plan, /*zoom_direction=*/0, /*pan_radius=*/1);
    // zoom_direction 0 -> no zoom ring; the whole want-list is the absent pan
    // annulus around the single visible rung-0 tile (the 8 Chebyshev neighbours).
    CHECK(want.size() == 8);
    for (const TileKey& key : want) {
      CHECK(key.rung.index == 0); // pan ring stays at the visible rung
    }
  }
}

// --- Refinement queue: poll semantics ---------------------------------------

// enforces: 02-architecture#async-arrival-emits-damage
TEST_CASE("poll_refinements drains settled arrivals into cache inserts and damage") {
  TileCache cache(64u * 1024 * 1024);
  RefinementQueue queue;

  auto make_pending = [](TileCoord coord, const std::shared_ptr<RenderCompletion>& done) {
    PendingTile pending;
    pending.key = tile_key(ScaleRung{0}, coord);
    pending.local_rect = arbc::tile_local_rect(ScaleRung{0}, coord);
    pending.content = k_content;
    pending.bytes = 1;
    pending.surface = std::make_unique<StubSurface>();
    pending.done = done;
    return pending;
  };

  SECTION("an unsettled arrival emits no damage and is retained") {
    auto done = std::make_shared<RenderCompletion>();
    queue.tiles.push_back(make_pending(TileCoord{0, 0}, done));
    CHECK(arbc::poll_refinements(queue, cache).empty());
    CHECK(queue.tiles.size() == 1);
  }

  SECTION("a completed arrival inserts under Visible, emits one damage, and drains") {
    auto done = std::make_shared<RenderCompletion>();
    queue.tiles.push_back(make_pending(TileCoord{0, 0}, done));
    done->complete(RenderResult{});

    const std::vector<Damage> damage = arbc::poll_refinements(queue, cache);
    REQUIRE(damage.size() == 1);
    CHECK(damage.front().object == k_content);
    CHECK(damage.front().rect == arbc::tile_local_rect(ScaleRung{0}, TileCoord{0, 0}));
    CHECK(queue.tiles.empty());

    // The arrival is now a fresh, exact cache entry at its key.
    std::optional<arbc::CacheHold<TileValue>> hit = cache.lookup(tile_key(ScaleRung{0}, TileCoord{0, 0}));
    REQUIRE(hit.has_value());
    CHECK(hit->get().meta.exact);
  }

  SECTION("a failed arrival is dropped with no insert and no damage") {
    auto done = std::make_shared<RenderCompletion>();
    queue.tiles.push_back(make_pending(TileCoord{0, 0}, done));
    done->fail(RenderError::ContentFailed);

    CHECK(arbc::poll_refinements(queue, cache).empty());
    CHECK(queue.tiles.empty());
    CHECK_FALSE(cache.lookup(tile_key(ScaleRung{0}, TileCoord{0, 0})).has_value());
  }
}

// enforces: 02-architecture#quiescent-refinement-schedules-no-frame
TEST_CASE("poll_refinements over an empty queue schedules no follow-up frame") {
  TileCache cache(64u * 1024 * 1024);
  RefinementQueue queue;
  CHECK(arbc::poll_refinements(queue, cache).empty());
  CHECK(queue.tiles.empty());
}

// --- Driver seam: record vs. drop -------------------------------------------

TEST_CASE("render_frame_interactive records async misses only when a queue is supplied") {
  StubBackend backend;
  AsyncContent content;

  arbc::Model model;
  arbc::ObjectId content_id{};
  {
    auto txn = model.transact();
    content_id = txn.add_content(0);
    txn.add_layer(content_id, arbc::Affine::identity());
    REQUIRE(txn.commit().has_value());
  }
  const arbc::DocStatePtr state = model.current();
  const auto resolver = [&](arbc::ObjectId id) -> arbc::Content* {
    return id == content_id ? &content : nullptr;
  };
  const arbc::Viewport viewport{256, 256, arbc::Affine::identity()};
  arbc::SurfacePool pool(backend);
  TileCache cache(64u * 1024 * 1024);
  arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError> target =
      backend.make_surface(256, 256, arbc::k_working_rgba32f);
  REQUIRE(target.has_value());

  SECTION("non-null pending: the async miss is recorded, then refined on completion") {
    RefinementQueue queue;
    arbc::render_frame_interactive(*state, resolver, viewport, cache, backend, pool, **target,
                                   arbc::Deadline::none(), std::nullopt, &queue);
    CHECK(content.renders() == 1);          // one tile, one async miss
    REQUIRE(queue.tiles.size() == 1);       // recorded, not dropped
    CHECK(queue.tiles.front().content == content_id);

    // Not settled yet -> the poll schedules no follow-up frame.
    CHECK(arbc::poll_refinements(queue, cache).empty());
    REQUIRE(queue.tiles.size() == 1);

    // On completion the poll inserts + emits one damage and drains.
    queue.tiles.front().done->complete(RenderResult{});
    const std::vector<Damage> damage = arbc::poll_refinements(queue, cache);
    REQUIRE(damage.size() == 1);
    CHECK(damage.front().object == content_id);
    CHECK(queue.tiles.empty());
    CHECK(cache.lookup(TileKey{content_id, state->revision(), ScaleRung{0}, TileCoord{0, 0},
                               std::nullopt})
              .has_value());
  }

  SECTION("null pending: the async miss is dropped exactly as before") {
    arbc::render_frame_interactive(*state, resolver, viewport, cache, backend, pool, **target,
                                   arbc::Deadline::none(), std::nullopt, nullptr);
    CHECK(content.renders() == 1); // rendered, answered async
    // Nothing recorded (no global state), nothing inserted -> the tile is dropped.
    CHECK_FALSE(cache.lookup(TileKey{content_id, state->revision(), ScaleRung{0}, TileCoord{0, 0},
                                     std::nullopt})
                    .has_value());
  }
}
