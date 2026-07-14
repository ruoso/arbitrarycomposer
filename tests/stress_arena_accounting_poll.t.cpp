// pool.stats_counter_race: the accounting a host memory panel reads is read from the
// HOST's thread, while the writer allocates. Doc 15 `:186-198` makes that legal; these
// two stresses are what hold it legal. Both run under the per-push `gcc-tsan` lane.
//
// They exist because `stress_document_housekeeping.t.cpp` cannot witness this race: its
// `memory_stats()` calls (`:234`, `:257`, `:259`) all happen AFTER `stop.store(true)` and
// `th.join()`, so its poll never overlaps the writer. Here the poll overlaps on purpose.
//
// Two hazards, two tests, because one test cannot reach both:
//
//   1. THE COUNTER TEAR -- `d_bytes_reserved` / `d_slots_capacity` / `d_high_water`, plain
//      scalars read-modify-written by the writer's grow path with no lock. The
//      Document-level test is the witness: it drives the SHIPPED chain end to end
//      (`Document::memory_stats` -> `HousekeepingThread::stats` -> `Housekeeper::stats` ->
//      `ArenaHousekeepingTarget` -> `Arena::total_bytes_reserved`), which is the race the
//      `.tji` names. Pre-fix, TSan reports the write in `SlotStore::allocate`'s grow branch
//      against the poller's load.
//
//   2. THE STORE-MAP WALK -- the aggregators traverse `Arena::d_stores` (a `std::map`) while
//      the writer EMPLACES into it, so a red-black rebalance rewrites the link words the
//      traversal is reading. No per-counter atomic can fix that; the `d_stores_mutex` does.
//      A Document CANNOT witness this: a Model mints both of its stores (`RefStore<HamtNode>`,
//      `RefStore<ObjectRecord>`) in its constructor and never calls `store_for` again, so its
//      map is frozen before any thread starts. `BigBlockPool` is the shipped allocator that
//      DOES mint stores mid-run -- one per size class, on demand -- so the second test polls
//      its arena while the writer walks up the size classes.
//
// Both pollers assert on behavioral counters, never a wall clock (doc 16:54-62): reserved
// bytes are monotonically non-decreasing across samples, and the poller's final sample --
// taken after the writer has stopped -- equals the same accessor read single-threaded, so a
// concurrent poll neither invented a value the counter never held nor cost the counter an
// update.
//
// enforces: 15-memory-model#accounting-reads-concurrent-with-allocation

#include <arbc/pool/big_block_pool.hpp>
#include <arbc/pool/slot_store.hpp>
#include <arbc/runtime/document.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <thread>
#include <vector>

namespace {

using arbc::BigBlockPool;
using arbc::Document;
using arbc::DocumentHousekeepingConfig;

// An ACTIVE background drainer for the whole run, so the poll contends with the
// housekeeping thread's mutex as well as with the writer. Never asserted on (doc 16:54-62).
constexpr std::chrono::microseconds kActiveTick{50};

// Enough compositions that the model arena mints data chunks well past the first --
// the writes to `d_bytes_reserved`/`d_slots_capacity` only happen on a chunk mint, so a
// run that grows only one chunk would give the poller nothing to race with. The
// `CHECK(final > initial)` below is what holds this number honest.
constexpr int kWriterRounds = 3000;

// Size classes the big-block writer walks up: 4 KiB, 8 KiB, ... Each is a store minted
// mid-run through `Arena::store_for` -- i.e. one `std::map` insert under a live poller.
constexpr int kSizeClasses = 8;
constexpr int kBlobsPerClass = 4;

} // namespace

// HAZARD 1, through the shipped chain: a host thread polls `Document::memory_stats()` while
// the writer grows the model arena.
TEST_CASE("stress: a host polls Document::memory_stats() while the writer grows the arena") {
  DocumentHousekeepingConfig config;
  config.thread.tick_period = kActiveTick;
  Document doc(config);

  std::atomic<bool> stop{false};
  std::atomic<bool> not_monotonic{false};
  std::atomic<std::size_t> samples{0};
  std::size_t final_bytes = 0;

  // The host memory panel. It runs for the WHOLE writer loop -- this is the overlap the
  // existing Document stress does not have.
  std::thread panel([&] {
    std::size_t last = 0;
    while (!stop.load(std::memory_order_acquire)) {
      const std::size_t seen = doc.memory_stats().bytes_reserved;
      if (seen < last) {
        // Reserved bytes are monotonic: capacity is never handed back. A decrease means the
        // poll read a value the counter never held (a tear), not a stale one.
        not_monotonic.store(true, std::memory_order_relaxed);
      }
      last = seen;
      samples.fetch_add(1, std::memory_order_relaxed);
    }
    // One last sample with the writer already stopped -- the value the post-join
    // single-threaded read must agree with exactly.
    final_bytes = doc.memory_stats().bytes_reserved;
  });

  const std::size_t initial_bytes = doc.memory_stats().bytes_reserved;

  // The writer (this thread). Each composition commits its own version: a record that stays
  // live plus a HAMT path copy, so the arena's high-water climbs and mints chunk after chunk.
  for (int i = 0; i < kWriterRounds; ++i) {
    doc.add_composition(8.0, 8.0);
  }

  stop.store(true, std::memory_order_release);
  panel.join();

  CHECK_FALSE(not_monotonic.load());
  CHECK(samples.load() > 0);

  // The writer really did grow the arena under the poll -- otherwise there was no write to
  // race the poller's loads and this test witnesses nothing.
  const std::size_t settled = doc.memory_stats().bytes_reserved;
  CHECK(settled > initial_bytes);

  // The concurrent poll saw the counter's real final value, and cost it no update.
  CHECK(final_bytes == settled);
}

// HAZARD 2: the writer MINTS size-class stores (inserting into the arena's `std::map`) while
// a host thread walks that same map in the aggregate accessors.
TEST_CASE("stress: a host polls arena aggregates while the writer mints new size-class stores") {
  BigBlockPool pool;
  const arbc::Arena& arena = pool.arena();

  std::atomic<bool> stop{false};
  std::atomic<bool> not_monotonic{false};
  std::atomic<std::size_t> samples{0};
  std::size_t final_bytes = 0;

  std::thread panel([&] {
    std::size_t last_bytes = 0;
    std::size_t last_stores = 0;
    while (!stop.load(std::memory_order_acquire)) {
      // All four map-walking accessors, so the mutex covers every one of them: a walk that
      // raced the writer's `emplace` would follow a link word mid-rebalance.
      const std::size_t bytes = arena.total_bytes_reserved();
      const std::size_t stores = arena.store_count();
      (void)arena.total_slots_live();
      (void)arena.total_high_water();
      if (bytes < last_bytes || stores < last_stores) {
        not_monotonic.store(true, std::memory_order_relaxed);
      }
      last_bytes = bytes;
      last_stores = stores;
      samples.fetch_add(1, std::memory_order_relaxed);
    }
    final_bytes = arena.total_bytes_reserved();
  });

  // The writer (this thread): walk UP the size classes, so each outer round mints a store the
  // poller's walk has never seen, and each blob within a class grows that store's chunks.
  std::vector<arbc::BlockRef> held;
  for (int c = 0; c < kSizeClasses; ++c) {
    const std::size_t size = BigBlockPool::k_page << c;
    for (int b = 0; b < kBlobsPerClass; ++b) {
      auto blob = pool.allocate(size);
      REQUIRE(blob.has_value());
      held.push_back(std::move(*blob)); // keep them live: reserved bytes stay monotonic
    }
  }

  stop.store(true, std::memory_order_release);
  panel.join();

  CHECK_FALSE(not_monotonic.load());
  CHECK(samples.load() > 0);

  // Every size class minted exactly one store, and the poller's concurrent walks never
  // dropped or double-counted one.
  CHECK(arena.store_count() == static_cast<std::size_t>(kSizeClasses));
  CHECK(final_bytes == arena.total_bytes_reserved());
  CHECK(arena.total_slots_live() == static_cast<std::size_t>(kSizeClasses * kBlobsPerClass));
}
