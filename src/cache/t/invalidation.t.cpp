#include <arbc/cache/invalidation.hpp>

#include <arbc/cache/key_shapes.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace {

using arbc::ObjectId;
using arbc::PriorityClass;
using arbc::Rect;
using arbc::ScaleRung;
using arbc::TileCache;
using arbc::TileCoord;
using arbc::TileKey;
using arbc::TileMeta;
using arbc::TileValue;

using arbc::cache::drop_superseded;
using arbc::cache::invalidate_content;
using arbc::cache::invalidate_region;

// A lightweight in-test Surface reporting fixed width/height/format -- no
// CpuBackend, keeping the test at the cache's `base`+`surface` surface (the
// store never touches pixels). Mirrors the key_shapes test's fixture.
class FixedSurface : public arbc::Surface {
public:
  FixedSurface(int w, int h, arbc::PixelFormat pf) : d_w(w), d_h(h), d_pf(pf) {}

  int width() const override { return d_w; }
  int height() const override { return d_h; }
  arbc::SurfaceFormat format() const override {
    arbc::SurfaceFormat sf;
    sf.pixel_format = d_pf;
    return sf;
  }
  std::span<std::byte> cpu_bytes() override { return {}; }
  std::span<const std::byte> cpu_bytes() const override { return {}; }

private:
  int d_w;
  int d_h;
  arbc::PixelFormat d_pf;
};

constexpr arbc::PixelFormat k_pf = arbc::PixelFormat::Rgba32fLinearPremul;

TileValue make_tile(int w, int h) {
  return TileValue{std::make_unique<FixedSurface>(w, h, k_pf), TileMeta{1.0, true}};
}

std::size_t tile_bytes(int w, int h) { return arbc::tile_byte_cost(FixedSurface{w, h, k_pf}); }

// A tile grid whose tile side doubles per coarser rung: rung 0 -> 16 units,
// rung 1 -> 32, rung 2 -> 64. The compositor-owned scale ladder injected into
// `invalidate_region` -- kept in the test, never in the L3 cache.
Rect tile_rect(ScaleRung rung, TileCoord coord) {
  const double size = 16.0 * static_cast<double>(1 << rung.index);
  return {static_cast<double>(coord.col) * size, static_cast<double>(coord.row) * size,
          (static_cast<double>(coord.col) + 1.0) * size,
          (static_cast<double>(coord.row) + 1.0) * size};
}

// Convenience key builders. `content`, `rung`, `coord`, and optionally revision
// / achieved_time vary per case; a Static tile (nullopt time) is the default.
TileKey key(ObjectId content, std::int32_t rung, std::int32_t col, std::int32_t row,
            std::uint64_t revision = 1) {
  return TileKey{content, revision, ScaleRung{rung}, TileCoord{col, row}, std::nullopt};
}

void put(TileCache& cache, const TileKey& k) {
  cache.insert(k, make_tile(8, 8), tile_bytes(8, 8), PriorityClass::Visible);
}

bool resident(TileCache& cache, const TileKey& k) { return cache.lookup(k).has_value(); }

} // namespace

// enforces: 02-architecture#damage-invalidates-by-content-region-across-rungs
TEST_CASE("invalidate_region drops content C's intersecting tiles at every rung, "
          "spares non-intersecting C tiles and all of D") {
  TileCache cache(1 << 20);
  const ObjectId c{1};
  const ObjectId d{2};

  // C tiles at rungs {0,1,2} whose coord (0,0) footprint intersects the region.
  const TileKey c_r0 = key(c, 0, 0, 0);
  const TileKey c_r1 = key(c, 1, 0, 0);
  const TileKey c_r2 = key(c, 2, 0, 0);
  // A C tile at rung 0, coord (2,2): footprint {32,32,48,48}, outside the region.
  const TileKey c_far = key(c, 0, 2, 2);
  // A D tile inside the region -- different content, must be spared.
  const TileKey d_in = key(d, 0, 0, 0);

  put(cache, c_r0);
  put(cache, c_r1);
  put(cache, c_r2);
  put(cache, c_far);
  put(cache, d_in);

  const Rect region{0.0, 0.0, 20.0, 20.0};
  const std::size_t removed = invalidate_region(cache, c, region, tile_rect);

  // Every intersecting C key at every rung was removed; count == tiles removed.
  CHECK(removed == 3);
  CHECK_FALSE(resident(cache, c_r0));
  CHECK_FALSE(resident(cache, c_r1));
  CHECK_FALSE(resident(cache, c_r2));
  // Non-intersecting C tile and all of D survive.
  CHECK(resident(cache, c_far));
  CHECK(resident(cache, d_in));
}

// The companion assertion: region damage is revision- and time-agnostic. Tiles
// at the same intersecting coord but differing revision and achieved_time are
// all dropped (spatial damage supersedes the region at every edit/playback
// axis, a sound over-approximation).
// enforces: 02-architecture#damage-invalidates-by-content-region-across-rungs
TEST_CASE("invalidate_region is revision- and achieved_time-agnostic") {
  TileCache cache(1 << 20);
  const ObjectId c{5};

  TileKey rev_a = key(c, 0, 0, 0, /*revision=*/3);
  TileKey rev_b = key(c, 0, 0, 0, /*revision=*/9);
  TileKey timed = key(c, 0, 0, 0, /*revision=*/3);
  timed.achieved_time = arbc::Time{100};
  TileKey timed_other = key(c, 0, 0, 0, /*revision=*/3);
  timed_other.achieved_time = arbc::Time{200};

  put(cache, rev_a);
  put(cache, rev_b);
  put(cache, timed);
  put(cache, timed_other);

  const Rect region{0.0, 0.0, 20.0, 20.0};
  const std::size_t removed = invalidate_region(cache, c, region, tile_rect);

  CHECK(removed == 4);
  CHECK_FALSE(resident(cache, rev_a));
  CHECK_FALSE(resident(cache, rev_b));
  CHECK_FALSE(resident(cache, timed));
  CHECK_FALSE(resident(cache, timed_other));
}

// enforces: 02-architecture#revision-bump-preserves-stale-tiles-as-fallback
TEST_CASE("a revision bump leaves the prior-revision tile as resident fallback; "
          "drop_superseded is the explicit reclaim") {
  TileCache cache(1 << 20);
  const ObjectId c{7};

  const TileKey k_n = key(c, 0, 0, 0, /*revision=*/4);
  put(cache, k_n);
  const std::uint64_t evictions_before = cache.evictions();

  // The compositor advances to revision N+1: no cache op fires on the bump. The
  // fresh key was never inserted (misses); the stale key remains resident.
  const TileKey k_n1 = key(c, 0, 0, 0, /*revision=*/5);
  CHECK_FALSE(resident(cache, k_n1)); // fresh key: miss (nothing was evicted for it)
  CHECK(resident(cache, k_n));        // stale-revision fallback still hits
  CHECK(cache.evictions() == evictions_before);

  // The compositor later reclaims the superseded tile explicitly.
  const std::size_t removed = drop_superseded(cache, c, /*live_revision=*/5);
  CHECK(removed == 1);
  CHECK_FALSE(resident(cache, k_n));
  // The reclaim is a removal, not an eviction.
  CHECK(cache.evictions() == evictions_before);
}

// enforces: 02-architecture#invalidation-defers-drop-of-pinned-key
TEST_CASE("invalidating a pinned tile makes it miss immediately but defers the "
          "byte drop to last unpin") {
  TileCache cache(1 << 20);
  const ObjectId c{9};
  const TileKey k = key(c, 0, 0, 0);
  const std::size_t bytes = tile_bytes(8, 8);

  // Insert (its transient hold drops at the end of this statement), then pin via
  // lookup so exactly one hold is outstanding.
  put(cache, k);
  auto hold = cache.lookup(k);
  REQUIRE(hold.has_value());
  CHECK(cache.resident_bytes() == bytes);

  const Rect region{0.0, 0.0, 20.0, 20.0};
  const std::size_t removed = invalidate_region(cache, c, region, tile_rect);
  CHECK(removed == 1);

  // Immediately unreachable to a *new* lookup while the surface is orphaned:
  // bytes stay counted until the pin releases.
  CHECK_FALSE(cache.lookup(k).has_value());
  CHECK(cache.resident_bytes() == bytes);

  // Destroying the hold fires the deferred drop and reclaims the bytes.
  hold = std::nullopt;
  CHECK(cache.resident_bytes() == 0);
}

// enforces: 05-recursive-composition#composed-result-invalidated-like-leaf
TEST_CASE("a composed-result tile keyed by an aggregate revision is invalidated "
          "by the exact same machinery as a leaf") {
  TileCache cache(1 << 20);
  // A composed-result content id; its revision slot carries an aggregate
  // revision (a composition-level revision bumped by any reachable change,
  // doc 05:78-86) -- a plain std::uint64_t, no composite-specific type.
  const ObjectId composed{42};

  const TileKey agg_old = key(composed, 0, 0, 0, /*revision=*/1000);
  put(cache, agg_old);

  // A reachable child change bumps the aggregate; the new composed result is
  // cached under the higher aggregate revision.
  const TileKey agg_new = key(composed, 0, 0, 0, /*revision=*/1001);
  put(cache, agg_new);

  // drop_superseded reclaims the pre-bump aggregate exactly as it would a leaf's
  // superseded revision -- one tile, the higher aggregate spared.
  CHECK(drop_superseded(cache, composed, /*live_revision=*/1001) == 1);
  CHECK_FALSE(resident(cache, agg_old));
  CHECK(resident(cache, agg_new));

  // invalidate_content wholesale-drops the composed result like any content.
  CHECK(invalidate_content(cache, composed) == 1);
  CHECK_FALSE(resident(cache, agg_new));
}
