#include <arbc/pool/refs.hpp>
#include <arbc/pool/slot_store.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <thread>
#include <type_traits>

// Witnesses for pool.refcounts_in_store: the inside-out refcount column and
// (debug) generation column are owned by the size-class SlotStore and indexed by
// PHYSICAL slot, so several typed views over one size class share ONE count
// column and ONE generation column. A slot has exactly one logical reference
// count wherever it is viewed from; a slot recycled from one type to another
// reuses the same count entry and carries its generation bump across the views.

namespace {

// Two DISTINCT record types with identical (sizeof, alignof): they deliberately
// land in the same size class, so `Arena::store_for` hands both typed views the
// SAME untyped SlotStore (the sharing this task makes coherent). Each reports its
// own destruction so the tests can see reclamation run the right `~T`.
struct Alpha {
  std::uint32_t value;
  int* destructions;
  Alpha(std::uint32_t v, int* d) : value(v), destructions(d) {}
  ~Alpha() {
    if (destructions != nullptr) {
      ++*destructions;
    }
  }
};
struct Beta {
  std::uint32_t value;
  int* destructions;
  Beta(std::uint32_t v, int* d) : value(v), destructions(d) {}
  ~Beta() {
    if (destructions != nullptr) {
      ++*destructions;
    }
  }
};

static_assert(sizeof(Alpha) == sizeof(Beta), "Alpha and Beta must share a size class");
static_assert(alignof(Alpha) == alignof(Beta), "Alpha and Beta must share a size class");

// enforces: 15-memory-model#one-count-column-per-size-class
TEST_CASE("a slot recycled from one type to another reuses the shared count-column entry") {
  arbc::Arena arena;
  arbc::RefStore<Alpha> alpha(arena);
  arbc::RefStore<Beta> beta(arena);

  // Same (sizeof, alignof) => one untyped store => one count column.
  REQUIRE(&alpha.store() == &beta.store());
  REQUIRE(arena.store_count() == 1);

  int a_dtor = 0;
  int b_dtor = 0;
  arbc::SlotIndex idx = 0;
  {
    arbc::Ref<Alpha> a = *alpha.create(0xA1u, &a_dtor);
    idx = a.index();
    REQUIRE(alpha.count(a.slot()) == 1);
    // a drops -> count 0 -> the immediate sink reclaims: ~Alpha runs, slot freed.
  }
  REQUIRE(a_dtor == 1);
  REQUIRE(alpha.store().slots_live() == 0);

  // Recycle the freed slot AS BETA. Perfect-hole reuse returns the same physical
  // index, and its shared count-column entry is reset to 1 by create -- not a
  // second, per-view table.
  arbc::Ref<Beta> b = *beta.create(0xB2u, &b_dtor);
  REQUIRE(b.index() == idx); // same physical slot
  REQUIRE(beta.store().slots_live() == 1);
  REQUIRE(beta.count(b.slot()) == 1); // one logical count, reset on reuse
  REQUIRE(b->value == 0xB2u);
}

// enforces: 15-memory-model#one-count-column-per-size-class
TEST_CASE("typed views over one size class read and write a single per-slot count cell") {
  arbc::Arena arena;
  arbc::RefStore<Alpha> alpha(arena);
  arbc::RefStore<Beta> beta(arena);
  REQUIRE(&alpha.store() == &beta.store());
  REQUIRE(arena.store_count() == 1); // one store => structurally one count column

  int a_dtor = 0;
  arbc::Ref<Alpha> a = *alpha.create(7u, &a_dtor);
  const arbc::SlotIndex idx = a.index();

  // A standalone retain through the Alpha view bumps the STORE-owned cell; the
  // store accessor (the single source of truth) observes it -- no duplicate view
  // table exists to disagree. Both views name the very same atomic.
  REQUIRE(&alpha.store().count_ref(idx) == &beta.store().count_ref(idx));
  REQUIRE(alpha.retain(a.slot()).has_value()); // 1 -> 2
  REQUIRE(alpha.count(a.slot()) == 2);
  REQUIRE(alpha.store().count_ref(idx).load(std::memory_order_acquire) == 2);
  alpha.release(a.slot()); // 2 -> 1
  REQUIRE(alpha.store().count_ref(idx).load(std::memory_order_acquire) == 1);
}

#ifndef NDEBUG
// Generation tags are a debug-build discipline (doc 15). Because the column is
// store-owned and physical-slot indexed, a bump made through ANY typed view is
// visible to a stale reference held through ANOTHER view of the same slot.
// enforces: 15-memory-model#one-count-column-per-size-class
TEST_CASE("the shared generation column faults a stale cross-type reference") {
  arbc::Arena arena;
  arbc::RefStore<Alpha> alpha(arena);
  arbc::RefStore<Beta> beta(arena);
  REQUIRE(&alpha.store() == &beta.store());

  int a_dtor = 0;
  int b_dtor = 0;
  arbc::SlotRef<Alpha> stale;
  arbc::SlotIndex idx = 0;
  {
    arbc::Ref<Alpha> a = *alpha.create(0xA1u, &a_dtor);
    idx = a.index();
    stale = a.slot();
    REQUIRE(alpha.generation_matches(stale)); // fresh reference matches
    // a drops -> reclaim -> the STORE-owned generation for idx is bumped.
  }
  REQUIRE(a_dtor == 1);
  REQUIRE_FALSE(alpha.generation_matches(stale));

  // A single shared cell: both views resolve the generation to one address.
  REQUIRE(&alpha.store().generation_ref(idx) == &beta.store().generation_ref(idx));

  // Recycle the slot AS BETA. The stale SlotRef<Alpha> still faults on the
  // recycled-as-Beta slot -- the T-view reference is invalidated across views.
  arbc::Ref<Beta> b = *beta.create(0xB2u, &b_dtor);
  REQUIRE(b.index() == idx);
  REQUIRE_FALSE(alpha.generation_matches(stale)); // T-view ref stale after recycle as U
  REQUIRE(beta.generation_matches(b.slot()));     // the fresh U-view ref matches

  // A bump made through the BETA (U) view is visible through the ALPHA (T) view's
  // store accessor: the two views share the one generation cell.
  const std::uint32_t before = alpha.store().generation_ref(idx).load(std::memory_order_acquire);
  b = arbc::Ref<Beta>{}; // drop -> reclaim via the Beta view -> bump
  REQUIRE(b_dtor == 1);
  const std::uint32_t after = alpha.store().generation_ref(idx).load(std::memory_order_acquire);
  REQUIRE(after == before + 1); // the U-view bump is seen through the T view
}
#endif

// TSan/asan smoke (doc 16): two typed views over one size-class store, each
// pinning/unpinning its OWN disjoint slot concurrently, must race-clean on the
// now-shared per-slot count column (per-slot atomics; growth is writer-only). The
// seeded cross-type churn stress lives in quality.stress_harness, not here.
TEST_CASE("two typed views over one store pin their disjoint slots race-clean") {
  arbc::Arena arena;
  arbc::RefStore<Alpha> alpha(arena);
  arbc::RefStore<Beta> beta(arena);
  REQUIRE(&alpha.store() == &beta.store());

  int a_dtor = 0;
  int b_dtor = 0;
  arbc::Ref<Alpha> a = *alpha.create(0xA1u, &a_dtor); // base count keeps each slot
  arbc::Ref<Beta> b = *beta.create(0xB2u, &b_dtor);   // alive for the whole test
  const arbc::SlotRef<Alpha> sa = a.slot();
  const arbc::SlotRef<Beta> sb = b.slot();
  REQUIRE(sa.index() != sb.index()); // disjoint slots on the shared store

  constexpr int iterations = 5000;
  std::atomic<bool> go{false};
  std::atomic<bool> bad{false};

  std::thread ta([&] {
    while (!go.load(std::memory_order_acquire)) {
    }
    for (int i = 0; i < iterations; ++i) {
      auto pinned = alpha.resolve(sa); // count op + read, any thread
      if (!pinned.has_value() || (*pinned)->value != 0xA1u) {
        bad.store(true, std::memory_order_relaxed);
      }
      // pinned drops (2 -> 1); the base count keeps the slot off the sink.
    }
  });
  std::thread tb([&] {
    while (!go.load(std::memory_order_acquire)) {
    }
    for (int i = 0; i < iterations; ++i) {
      auto pinned = beta.resolve(sb);
      if (!pinned.has_value() || (*pinned)->value != 0xB2u) {
        bad.store(true, std::memory_order_relaxed);
      }
    }
  });

  go.store(true, std::memory_order_release);
  ta.join();
  tb.join();

  REQUIRE_FALSE(bad.load());
  REQUIRE(alpha.count(sa) == 1); // back to the base count, nothing reclaimed
  REQUIRE(beta.count(sb) == 1);
}

} // namespace
