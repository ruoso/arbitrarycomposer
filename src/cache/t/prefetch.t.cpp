#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/cache/key_shapes.hpp>
#include <arbc/cache/keyed_store.hpp>
#include <arbc/cache/prefetch.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace {

using arbc::BlockKey;
using arbc::KeyedStore;
using arbc::ObjectId;
using arbc::PriorityClass;
using arbc::ScaleRung;
using arbc::TileCoord;
using arbc::TileKey;
using arbc::Time;

using arbc::cache::pan_prefetch_ring;
using arbc::cache::prime_ring;
using arbc::cache::temporal_prefetch_ring;

// A Timed tile key at grid (col,row) for content 1, revision 0, rung 0, and the
// given achieved time. The store never inspects the value, so these tests carry
// a plain `int` value -- keeping them at the cache's true base-only surface.
TileKey tile(std::int32_t col, std::int32_t row, std::int64_t at = 0) {
  return TileKey{ObjectId{1}, 0, ScaleRung{0}, TileCoord{col, row}, Time{at}};
}

BlockKey block(std::int64_t index) { return BlockKey{ObjectId{1}, 0, index, 48000}; }

bool contains_key(const std::vector<TileKey>& v, const TileKey& k) {
  return std::find(v.begin(), v.end(), k) != v.end();
}

} // namespace

// The spatial pan-prefetch ring is the Chebyshev-`radius` annulus around the
// visible set, excluding the visible set, deduplicated (doc 02:92-93).
TEST_CASE("pan_prefetch_ring is the annulus around visible, minus visible") {
  SECTION("single visible tile, radius 1 -> the 8 surrounding tiles") {
    const std::vector<TileKey> visible{tile(0, 0)};
    const std::vector<TileKey> ring = pan_prefetch_ring(visible, 1);

    CHECK(ring.size() == 8);
    CHECK_FALSE(contains_key(ring, tile(0, 0))); // visible tile excluded
    CHECK(contains_key(ring, tile(1, 0)));
    CHECK(contains_key(ring, tile(-1, -1)));
    // Every ring member shares the visible tile's non-coord fields.
    for (const TileKey& k : ring) {
      CHECK(k.content == ObjectId{1});
      CHECK(k.rung == ScaleRung{0});
      CHECK(k.achieved_time == std::optional<Time>{Time{0}});
    }
  }

  SECTION("adjacent visible tiles: the shared neighbour is deduped and the "
          "other visible tile is excluded") {
    const std::vector<TileKey> visible{tile(0, 0), tile(1, 0)};
    const std::vector<TileKey> ring = pan_prefetch_ring(visible, 1);

    // Neither visible tile appears; (0,0)'s radius covers (1,0) but it is
    // visible so excluded. The two 3x3 blocks overlap in 6 cells, so their
    // union is 9 + 9 - 6 = 12 cells; minus the 2 visible tiles leaves 10.
    CHECK_FALSE(contains_key(ring, tile(0, 0)));
    CHECK_FALSE(contains_key(ring, tile(1, 0)));
    CHECK(ring.size() == 10);
    // No duplicates: (1,1) is a neighbour of both visible tiles but appears once.
    CHECK(std::count(ring.begin(), ring.end(), tile(1, 1)) == 1);
  }

  SECTION("non-positive radius yields an empty ring") {
    const std::vector<TileKey> visible{tile(5, 5)};
    CHECK(pan_prefetch_ring(visible, 0).empty());
    CHECK(pan_prefetch_ring(visible, -3).empty());
  }
}

// enforces: 11-time-and-video#temporal-prefetch-ring-bounded-by-horizon
TEST_CASE("temporal_prefetch_ring walks upcoming buckets bounded by the horizon") {
  const TileKey base = tile(0, 0, 1000);

  SECTION("forward: buckets base+step*k while step*k <= horizon, none beyond") {
    // step 10, horizon 35 -> k = 1,2,3 (30 <= 35); k=4 (40 > 35) excluded.
    const std::vector<TileKey> ring = temporal_prefetch_ring(base, +1, Time{10}, Time{35});
    REQUIRE(ring.size() == 3);
    CHECK(ring[0].achieved_time == std::optional<Time>{Time{1010}});
    CHECK(ring[1].achieved_time == std::optional<Time>{Time{1020}});
    CHECK(ring[2].achieved_time == std::optional<Time>{Time{1030}});
    // The reverse-direction bucket is never enumerated.
    for (const TileKey& k : ring) {
      CHECK(k.achieved_time->flicks > 1000);
    }
  }

  SECTION("reverse: buckets walk backward, never into the forward direction") {
    const std::vector<TileKey> ring = temporal_prefetch_ring(base, -1, Time{10}, Time{25});
    REQUIRE(ring.size() == 2); // 25/10 = 2 buckets
    CHECK(ring[0].achieved_time == std::optional<Time>{Time{990}});
    CHECK(ring[1].achieved_time == std::optional<Time>{Time{980}});
    for (const TileKey& k : ring) {
      CHECK(k.achieved_time->flicks < 1000);
    }
  }

  SECTION("exact horizon boundary is included; one past is not") {
    const std::vector<TileKey> ring = temporal_prefetch_ring(base, +1, Time{10}, Time{30});
    REQUIRE(ring.size() == 3); // step*3 == 30 == horizon, included
    CHECK(ring.back().achieved_time == std::optional<Time>{Time{1030}});
  }

  SECTION("degenerate step/horizon yield an empty ring") {
    CHECK(temporal_prefetch_ring(base, +1, Time{0}, Time{100}).empty());
    CHECK(temporal_prefetch_ring(base, +1, Time{10}, Time{0}).empty());
    CHECK(temporal_prefetch_ring(base, +1, Time{10}, Time{5}).empty()); // horizon < step
  }
}

// enforces: 11-time-and-video#temporal-ring-evicts-between-recent-and-adjacent
TEST_CASE("the priority ladder has five classes with Temporal between Recent "
          "and Adjacent") {
  SECTION("eviction-order array lists all five, victim-first") {
    REQUIRE(arbc::detail::k_priority_class_count == 5);
    const auto& order = arbc::detail::cache_eviction_order();
    REQUIRE(order.size() == 5);
    CHECK(order[0] == PriorityClass::Speculative);
    CHECK(order[1] == PriorityClass::Recent);
    CHECK(order[2] == PriorityClass::Temporal);
    CHECK(order[3] == PriorityClass::Adjacent);
    CHECK(order[4] == PriorityClass::Visible);
  }

  SECTION("a Recent entry is evicted before a Temporal one") {
    KeyedStore<int, int> store(100);
    store.insert(1, 1, 40, PriorityClass::Recent);
    store.insert(2, 2, 40, PriorityClass::Temporal);

    // Inserting past budget must evict the lowest class with a victim: Recent.
    store.insert(3, 3, 40, PriorityClass::Visible);
    CHECK(store.evictions() == 1);
    CHECK_FALSE(store.lookup(1).has_value()); // Recent evicted
    CHECK(store.lookup(2).has_value());       // Temporal spared
  }

  SECTION("a Temporal entry is evicted before an Adjacent one") {
    KeyedStore<int, int> store(100);
    store.insert(1, 1, 40, PriorityClass::Temporal);
    store.insert(2, 2, 40, PriorityClass::Adjacent);

    store.insert(3, 3, 40, PriorityClass::Visible);
    CHECK(store.evictions() == 1);
    CHECK_FALSE(store.lookup(1).has_value()); // Temporal evicted
    CHECK(store.lookup(2).has_value());       // Adjacent spared
  }
}

// enforces: 02-architecture#prefetch-ring-classifies-resident-reports-absent
TEST_CASE("prime_ring reclassifies resident members and reports the absent set") {
  KeyedStore<TileKey, int> store(1000);

  // Two resident ring tiles (as Speculative), interleaved with two absent ones.
  store.insert(tile(0, 0), 10, 40, PriorityClass::Speculative);
  store.insert(tile(2, 0), 20, 40, PriorityClass::Speculative);
  // A resident non-ring filler, also Speculative, to prove reclassification.
  store.insert(tile(9, 9), 90, 40, PriorityClass::Speculative);

  const std::vector<TileKey> ring{tile(0, 0), tile(1, 0), tile(2, 0), tile(3, 0)};

  const std::size_t bytes_before = store.resident_bytes();
  const std::uint64_t evictions_before = store.evictions();

  const std::vector<TileKey> absent =
      prime_ring(store, std::span<const TileKey>(ring), PriorityClass::Adjacent);

  // Want-list is exactly the absent ring members, in ring order.
  REQUIRE(absent.size() == 2);
  CHECK(absent[0] == tile(1, 0));
  CHECK(absent[1] == tile(3, 0));

  // The cache inserted and evicted nothing across the call.
  CHECK(store.resident_bytes() == bytes_before);
  CHECK(store.evictions() == evictions_before);

  // The resident ring members were reclassified to Adjacent: under budget
  // pressure the still-Speculative filler is the victim, not the retagged tiles.
  KeyedStore<TileKey, int> tight(120); // holds exactly 3 * 40
  tight.insert(tile(0, 0), 10, 40, PriorityClass::Speculative);
  tight.insert(tile(2, 0), 20, 40, PriorityClass::Speculative);
  tight.insert(tile(9, 9), 90, 40, PriorityClass::Speculative);
  prime_ring(tight, std::span<const TileKey>(ring), PriorityClass::Adjacent);
  tight.insert(tile(5, 5), 55, 40, PriorityClass::Visible); // forces one eviction
  CHECK(tight.evictions() == 1);
  CHECK_FALSE(tight.lookup(tile(9, 9)).has_value()); // still-Speculative filler
  CHECK(tight.lookup(tile(0, 0)).has_value());       // retagged Adjacent, spared
  CHECK(tight.lookup(tile(2, 0)).has_value());       // retagged Adjacent, spared
}

// The temporal ring and the classify/report driver are engine-agnostic (doc
// 17:73-74): they instantiate over BlockKey too, so `audio-engine` reuses them
// when it instantiates a BlockCache. No production BlockCache exists yet, so a
// local KeyedStore<BlockKey, int> stands in.
TEST_CASE("temporal_prefetch_ring and prime_ring instantiate for BlockKey") {
  SECTION("temporal ring advances block_index one block per bucket") {
    const std::vector<BlockKey> ring = temporal_prefetch_ring(block(100), +1, Time{10}, Time{30});
    REQUIRE(ring.size() == 3);
    CHECK(ring[0].block_index == 101);
    CHECK(ring[1].block_index == 102);
    CHECK(ring[2].block_index == 103);

    const std::vector<BlockKey> back = temporal_prefetch_ring(block(100), -1, Time{10}, Time{20});
    REQUIRE(back.size() == 2);
    CHECK(back[0].block_index == 99);
    CHECK(back[1].block_index == 98);
  }

  SECTION("prime_ring drives a BlockCache-shaped store: reclassify + want-list") {
    KeyedStore<BlockKey, int> store(1000);
    store.insert(block(101), 1, 40, PriorityClass::Speculative);
    // block(102) absent.
    store.insert(block(103), 3, 40, PriorityClass::Speculative);

    const std::vector<BlockKey> ring{block(101), block(102), block(103)};
    const std::size_t bytes_before = store.resident_bytes();

    const std::vector<BlockKey> absent =
        prime_ring(store, std::span<const BlockKey>(ring), PriorityClass::Temporal);

    REQUIRE(absent.size() == 1);
    CHECK(absent[0] == block(102));
    CHECK(store.resident_bytes() == bytes_before);
    CHECK(store.evictions() == 0);
  }
}
