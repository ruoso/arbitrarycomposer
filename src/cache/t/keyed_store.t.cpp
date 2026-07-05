#include <arbc/cache/keyed_store.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <utility>

namespace {

using arbc::CacheHold;
using arbc::KeyedStore;
using arbc::PriorityClass;

// A minimal base-only move-only Value: it carries an id and a shared "released"
// counter its destructor bumps exactly once for the live instance. Moving
// transfers the counter (the moved-from husk holds none), so the counter proves
// exactly-once release and lets a test see a deferred drop fire on last unpin.
// No Surface / CpuBackend -- keeps the test at the store's true base-only
// dependency surface.
struct Tracked {
  int id;
  std::shared_ptr<int> released;

  Tracked(int the_id, std::shared_ptr<int> counter)
      : id(the_id), released(std::move(counter)) {}

  Tracked(const Tracked&) = delete;
  Tracked& operator=(const Tracked&) = delete;

  Tracked(Tracked&& other) noexcept
      : id(other.id), released(std::move(other.released)) {}
  Tracked& operator=(Tracked&& other) noexcept {
    id = other.id;
    released = std::move(other.released);
    return *this;
  }

  ~Tracked() {
    if (released) {
      ++(*released);
    }
  }
};

using Store = KeyedStore<int, Tracked>;

Tracked make(int id, const std::shared_ptr<int>& released) {
  return Tracked(id, released);
}

} // namespace

TEST_CASE("insert then lookup hits; absent key misses") {
  auto released = std::make_shared<int>(0);
  Store store(1000);

  store.insert(1, make(1, released), 10, PriorityClass::Visible);

  auto hit = store.lookup(1);
  REQUIRE(hit.has_value());
  CHECK(hit->get().id == 1);
  CHECK(store.hits() == 1);
  CHECK(store.misses() == 0);

  auto miss = store.lookup(42);
  CHECK_FALSE(miss.has_value());
  CHECK(store.misses() == 1);
}

// enforces: 15-memory-model#cache-budget-is-eviction-policy
// enforces: 02-architecture#cache-evicts-lru-within-priority-class
TEST_CASE("insert past budget evicts Speculative before higher classes") {
  auto released = std::make_shared<int>(0);
  Store store(100);

  // Fill to budget with one entry per lower class (fire-and-forget: the
  // returned holds drop at end of statement, so every entry is unpinned).
  store.insert(1, make(1, released), 40, PriorityClass::Visible);
  store.insert(2, make(2, released), 40, PriorityClass::Speculative);
  CHECK(store.resident_bytes() == 80);

  // This insert needs 40 more (would reach 120 > 100): the Speculative entry is
  // the victim, the Visible one is spared.
  store.insert(3, make(3, released), 40, PriorityClass::Recent);

  CHECK(store.evictions() == 1);
  CHECK(store.resident_bytes() <= store.budget());
  CHECK(store.lookup(2).has_value() == false); // Speculative evicted
  CHECK(store.lookup(1).has_value() == true);  // Visible survives
  CHECK(store.lookup(3).has_value() == true);
}

// enforces: 02-architecture#cache-evicts-lru-within-priority-class
TEST_CASE("a higher class is not evicted while a lower class has a victim") {
  auto released = std::make_shared<int>(0);
  Store store(100);

  store.insert(1, make(1, released), 40, PriorityClass::Visible);
  store.insert(2, make(2, released), 40, PriorityClass::Adjacent);

  // Adjacent outranks nothing lower here; it is the only evictable non-Visible
  // entry, so it -- not the Visible one -- is dropped.
  store.insert(3, make(3, released), 40, PriorityClass::Visible);

  CHECK(store.lookup(2).has_value() == false); // Adjacent evicted
  CHECK(store.lookup(1).has_value() == true);  // Visible spared
}

// enforces: 02-architecture#cache-evicts-lru-within-priority-class
TEST_CASE("LRU within a class: least-recently-used goes first, lookup flips it") {
  auto released = std::make_shared<int>(0);
  Store store(100);

  store.insert(1, make(1, released), 40, PriorityClass::Recent);
  store.insert(2, make(2, released), 40, PriorityClass::Recent);

  // Touch key 1 so key 2 is now least-recently-used.
  { auto touch = store.lookup(1); }

  store.insert(3, make(3, released), 40, PriorityClass::Recent);

  CHECK(store.lookup(2).has_value() == false); // LRU victim
  CHECK(store.lookup(1).has_value() == true);  // refreshed, spared
}

// enforces: 02-architecture#cache-evicts-lru-within-priority-class
TEST_CASE("reclassify moves an entry between classes: demote -> victim, "
          "promote -> spared") {
  auto released = std::make_shared<int>(0);

  SECTION("demote makes an otherwise-spared entry the next victim") {
    Store store(100);
    store.insert(1, make(1, released), 40, PriorityClass::Visible);
    store.insert(2, make(2, released), 40, PriorityClass::Adjacent);

    // Demote the Visible entry below the Adjacent one: it becomes the victim.
    store.reclassify(1, PriorityClass::Speculative);
    store.insert(3, make(3, released), 40, PriorityClass::Visible);
    CHECK(store.lookup(1).has_value() == false); // demoted, evicted
    CHECK(store.lookup(2).has_value() == true);  // Adjacent, spared
  }

  SECTION("promote spares an entry that would otherwise be the victim") {
    Store store(100);
    store.insert(1, make(1, released), 40, PriorityClass::Speculative);
    store.insert(2, make(2, released), 40, PriorityClass::Speculative);

    // Promote key 1 out of the eviction line; key 2 remains the lowest victim.
    store.reclassify(1, PriorityClass::Visible);
    store.insert(3, make(3, released), 40, PriorityClass::Recent);
    CHECK(store.lookup(1).has_value() == true);  // promoted, spared
    CHECK(store.lookup(2).has_value() == false); // still Speculative, evicted
  }

  Store store(100);
  store.reclassify(99, PriorityClass::Visible); // absent -> no-op, no crash
}

// enforces: 02-architecture#cache-pin-survives-eviction
// enforces: 15-memory-model#cache-budget-is-eviction-policy
TEST_CASE("pinned entry survives eviction and overshoots budget; dropping the "
          "hold reclaims it") {
  auto released = std::make_shared<int>(0);
  Store store(100);

  auto pinned = store.insert(1, make(1, released), 80, PriorityClass::Speculative);
  REQUIRE(pinned.valid());

  // Even though key 1 is the lowest class, its hold excludes it from eviction:
  // the store overshoots budget rather than dropping an in-use entry.
  store.insert(2, make(2, released), 80, PriorityClass::Visible);
  CHECK(store.lookup(1).has_value() == true);
  CHECK(store.resident_bytes() > store.budget());

  // Release the hold (and the lookup's transient hold) -> key 1 is now
  // evictable and the next insert reclaims it.
  pinned = CacheHold<Tracked>{};
  store.insert(3, make(3, released), 80, PriorityClass::Recent);
  CHECK(store.lookup(1).has_value() == false);
  CHECK(store.resident_bytes() <= store.budget());
}

// enforces: 02-architecture#cache-pin-survives-eviction
TEST_CASE("remove of a pinned key: immediate miss, value drop deferred to last "
          "unpin") {
  auto released = std::make_shared<int>(0);
  Store store(1000);

  auto hold = store.insert(7, make(7, released), 10, PriorityClass::Visible);
  const int released_before = *released;

  store.remove(7);
  // Immediately unreachable by lookup...
  CHECK_FALSE(store.lookup(7).has_value());
  // ...but the value is still held: its released counter has not fired.
  CHECK(*released == released_before);

  // Dropping the last hold fires the deferred drop exactly once.
  hold = CacheHold<Tracked>{};
  CHECK(*released == released_before + 1);

  store.remove(7); // already gone -> no-op
}

TEST_CASE("remove of an unpinned key releases inline and frees its bytes") {
  auto released = std::make_shared<int>(0);
  Store store(1000);

  store.insert(5, make(5, released), 30, PriorityClass::Recent);
  CHECK(store.resident_bytes() == 30);
  const int released_before = *released;

  store.remove(5);
  CHECK(*released == released_before + 1); // dropped inline
  CHECK(store.resident_bytes() == 0);
  CHECK_FALSE(store.lookup(5).has_value());
}

// enforces: 02-architecture#cache-pin-survives-eviction
TEST_CASE("CacheHold move transfers the unpin obligation: exactly-once release, "
          "no double-unpin") {
  auto released = std::make_shared<int>(0);
  Store store(1000);

  {
    CacheHold<Tracked> a = store.insert(1, make(1, released), 10,
                                        PriorityClass::Visible);
    CacheHold<Tracked> b = std::move(a); // move-construct: a is now empty
    CHECK_FALSE(a.valid());
    CHECK(b.valid());
    CHECK(b->id == 1);

    CacheHold<Tracked> c;
    c = std::move(b); // move-assign: b is now empty
    CHECK_FALSE(b.valid());
    CHECK(c.valid());

    // Only `c` holds the pin now; while pinned the entry is not evictable.
    store.remove(1); // pinned -> deferred drop
    CHECK(*released == 0);
  } // a, b are empty (no unpin); c releases exactly once -> deferred drop fires

  CHECK(*released == 1);
}

// enforces: 15-memory-model#cache-budget-is-eviction-policy
TEST_CASE("re-inserting a key replaces its value and re-accounts bytes") {
  auto released = std::make_shared<int>(0);
  Store store(1000);

  store.insert(1, make(1, released), 30, PriorityClass::Recent);
  CHECK(store.resident_bytes() == 30);
  const int released_before = *released;

  // Replace: the old (unpinned) value drops inline, new cost accounted.
  store.insert(1, make(11, released), 50, PriorityClass::Recent);
  CHECK(*released == released_before + 1);
  CHECK(store.resident_bytes() == 50);

  auto hit = store.lookup(1);
  REQUIRE(hit.has_value());
  CHECK(hit->get().id == 11);
}

// The behavioral-counter tier for a cache (doc 16:54-62): a scripted sequence
// asserting exact hits / misses / evictions deltas. Never wall-clock.
// enforces: 15-memory-model#cache-budget-is-eviction-policy
TEST_CASE("behavioral counters: exact hits/misses/evictions over a script") {
  auto released = std::make_shared<int>(0);
  Store store(100);

  store.insert(1, make(1, released), 40, PriorityClass::Speculative);
  store.insert(2, make(2, released), 40, PriorityClass::Speculative);
  CHECK(store.hits() == 0);
  CHECK(store.misses() == 0);
  CHECK(store.evictions() == 0);

  { auto h = store.lookup(1); } // hit
  { auto m = store.lookup(9); } // miss
  CHECK(store.hits() == 1);
  CHECK(store.misses() == 1);

  // Forces one eviction (2 is now LRU in Speculative).
  store.insert(3, make(3, released), 40, PriorityClass::Speculative);
  CHECK(store.evictions() == 1);

  { auto m2 = store.lookup(2); } // evicted -> miss
  CHECK(store.misses() == 2);
  CHECK(store.hits() == 1);
}
