#include <arbc/cache/key_shapes.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>

namespace {

using arbc::BlockKey;
using arbc::PriorityClass;
using arbc::ScaleRung;
using arbc::TileCache;
using arbc::TileCoord;
using arbc::TileKey;
using arbc::TileMeta;
using arbc::TileValue;

// A lightweight in-test Surface reporting fixed width/height/format. No
// CpuBackend -- keeps the test at the cache's `base`+`surface` surface (the
// store never touches pixels).
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

// A representative fully-populated Timed tile key the field-discrimination
// cases perturb one field at a time.
TileKey timed_key() {
  return TileKey{arbc::ObjectId{7}, 3, ScaleRung{2}, TileCoord{5, 9}, arbc::Time{100}};
}

TileValue make_tile(int w, int h, arbc::PixelFormat pf) {
  return TileValue{std::make_unique<FixedSurface>(w, h, pf), TileMeta{1.0, true}};
}

std::size_t hash_of(const TileKey& k) { return std::hash<TileKey>{}(k); }
std::size_t hash_of(const BlockKey& k) { return std::hash<BlockKey>{}(k); }

} // namespace

// enforces: 11-time-and-video#tile-key-carries-time-and-revision
// enforces: 02-architecture#tile-cache-key-and-value-shape
TEST_CASE("TileKey: identical fields compare == and hash equal") {
  const TileKey a = timed_key();
  const TileKey b = timed_key();
  CHECK(a == b);
  CHECK(hash_of(a) == hash_of(b));
}

// enforces: 11-time-and-video#tile-key-carries-time-and-revision
TEST_CASE("TileKey: each field independently discriminates") {
  const TileKey base = timed_key();

  SECTION("content") {
    TileKey k = base;
    k.content = arbc::ObjectId{8};
    CHECK_FALSE(k == base);
  }
  SECTION("revision") {
    TileKey k = base;
    k.revision = 4;
    CHECK_FALSE(k == base);
  }
  SECTION("scale rung") {
    TileKey k = base;
    k.rung = ScaleRung{3};
    CHECK_FALSE(k == base);
  }
  SECTION("tile coord col") {
    TileKey k = base;
    k.coord.col = 6;
    CHECK_FALSE(k == base);
  }
  SECTION("tile coord row") {
    TileKey k = base;
    k.coord.row = 10;
    CHECK_FALSE(k == base);
  }
  SECTION("achieved_time value") {
    TileKey k = base;
    k.achieved_time = arbc::Time{101};
    CHECK_FALSE(k == base);
  }
  SECTION("achieved_time present-vs-absent") {
    TileKey k = base;
    k.achieved_time = std::nullopt;
    CHECK_FALSE(k == base);
  }
}

// enforces: 11-time-and-video#tile-key-carries-time-and-revision
TEST_CASE("TileKey: a Static key never equals a Timed key, including at flicks 0") {
  const TileKey static_key{arbc::ObjectId{7}, 3, ScaleRung{2}, TileCoord{5, 9}, std::nullopt};

  // Static content omits the time axis -- no still grows the cache.
  TileKey timed_zero = static_key;
  timed_zero.achieved_time = arbc::Time::zero(); // flicks == 0

  CHECK_FALSE(static_key == timed_zero);
  // The hash must not collide either (the nullopt-vs-present combine guard).
  CHECK(hash_of(static_key) != hash_of(timed_zero));

  // Two Static keys differing only in an earlier field are still distinct.
  TileKey other_static = static_key;
  other_static.revision = 4;
  CHECK_FALSE(static_key == other_static);
}

// enforces: 12-audio#block-cache-is-tile-cache-1d
TEST_CASE("BlockKey: identical fields == and hash-equal; each field discriminates") {
  const BlockKey base{arbc::ObjectId{7}, 3, 42, 48000};
  const BlockKey same{arbc::ObjectId{7}, 3, 42, 48000};
  CHECK(base == same);
  CHECK(hash_of(base) == hash_of(same));

  SECTION("content") {
    BlockKey k = base;
    k.content = arbc::ObjectId{8};
    CHECK_FALSE(k == base);
  }
  SECTION("revision") {
    BlockKey k = base;
    k.revision = 4;
    CHECK_FALSE(k == base);
  }
  SECTION("block index") {
    BlockKey k = base;
    k.block_index = 43;
    CHECK_FALSE(k == base);
  }
  SECTION("rate") {
    BlockKey k = base;
    k.rate = 44100;
    CHECK_FALSE(k == base);
  }
}

// enforces: 02-architecture#tile-cache-key-and-value-shape
// enforces: 12-audio#block-cache-is-tile-cache-1d
TEST_CASE("TileKey and BlockKey behave as std::unordered_map keys") {
  std::unordered_map<TileKey, int> tiles;
  tiles[timed_key()] = 1;
  TileKey other = timed_key();
  other.coord.col = 99;
  tiles[other] = 2;
  CHECK(tiles.size() == 2);
  CHECK(tiles.at(timed_key()) == 1);

  std::unordered_map<BlockKey, int> blocks;
  blocks[BlockKey{arbc::ObjectId{7}, 3, 42, 48000}] = 1;
  blocks[BlockKey{arbc::ObjectId{7}, 3, 43, 48000}] = 2;
  CHECK(blocks.size() == 2);
  CHECK(blocks.at(BlockKey{arbc::ObjectId{7}, 3, 42, 48000}) == 1);
}

// enforces: 02-architecture#tile-cache-key-and-value-shape
TEST_CASE("TileCache round-trip: insert, hit on equal key, miss on a differing one") {
  TileCache cache(1 << 20);
  const TileKey key = timed_key();
  const std::size_t bytes =
      arbc::tile_byte_cost(FixedSurface{16, 16, arbc::PixelFormat::Rgba32fLinearPremul});

  cache.insert(key, make_tile(16, 16, arbc::PixelFormat::Rgba32fLinearPremul), bytes,
               PriorityClass::Visible);
  CHECK(cache.resident_bytes() == bytes);

  // Lookup with an equal-valued (distinct object) key hits and returns the value.
  auto hit = cache.lookup(timed_key());
  REQUIRE(hit.has_value());
  CHECK(hit->get().surface != nullptr);
  CHECK(hit->get().meta.exact);

  // A key differing in exactly one field misses.
  TileKey miss_key = key;
  miss_key.coord.row += 1;
  CHECK_FALSE(cache.lookup(miss_key).has_value());
}

// enforces: 02-architecture#tile-cache-key-and-value-shape
TEST_CASE("tile_byte_cost is width*height*bytes_per_pixel for each PixelFormat") {
  CHECK(arbc::tile_byte_cost(FixedSurface{16, 8, arbc::PixelFormat::Rgba32fLinearPremul}) ==
        static_cast<std::size_t>(16) * 8 * 16);
  CHECK(arbc::tile_byte_cost(FixedSurface{16, 8, arbc::PixelFormat::Rgba16fLinearPremul}) ==
        static_cast<std::size_t>(16) * 8 * 8);
  CHECK(arbc::tile_byte_cost(FixedSurface{16, 8, arbc::PixelFormat::Rgba8Srgb}) ==
        static_cast<std::size_t>(16) * 8 * 4);
}

// The behavioral-counter tier for the tile cache (doc 16:54-62): a scripted
// insert/lookup/miss sequence asserting exact hits()/misses() deltas across
// distinct tile keys. Never wall-clock.
// enforces: 02-architecture#tile-cache-key-and-value-shape
TEST_CASE("behavioral counters: exact tile-cache hits/misses over a script") {
  TileCache cache(1 << 20);
  const auto pf = arbc::PixelFormat::Rgba32fLinearPremul;
  const std::size_t bytes = arbc::tile_byte_cost(FixedSurface{8, 8, pf});

  TileKey k1 = timed_key();
  TileKey k2 = timed_key();
  k2.coord.col += 1; // a distinct tile

  cache.insert(k1, make_tile(8, 8, pf), bytes, PriorityClass::Visible);
  cache.insert(k2, make_tile(8, 8, pf), bytes, PriorityClass::Visible);
  CHECK(cache.hits() == 0);
  CHECK(cache.misses() == 0);

  {
    auto h = cache.lookup(k1);
  } // hit
  {
    auto h = cache.lookup(k2);
  } // hit
  TileKey absent = timed_key();
  absent.revision += 100;
  {
    auto m = cache.lookup(absent);
  } // miss (different revision)

  CHECK(cache.hits() == 2);
  CHECK(cache.misses() == 1);
}
