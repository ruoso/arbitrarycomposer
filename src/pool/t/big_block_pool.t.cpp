#include <arbc/pool/big_block_pool.hpp>
#include <arbc/pool/chunk_source.hpp>
#include <arbc/pool/slot_store.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <thread>
#include <vector>

namespace {

constexpr std::size_t k_page = arbc::BigBlockPool::k_page;

bool page_aligned(const std::byte* p) {
  return (reinterpret_cast<std::uintptr_t>(p) & (k_page - 1)) == 0;
}

// enforces: 15-memory-model#bulk-payloads-are-page-aligned-size-classed
TEST_CASE("allocate returns a page-aligned blob from a power-of-two size class") {
  arbc::BigBlockPool pool;

  // A page-floor allocation and a same-size sibling share one store.
  arbc::BlockRef a = *pool.allocate(k_page);
  REQUIRE(static_cast<bool>(a));
  REQUIRE(page_aligned(a.data()));
  REQUIRE(a.size() == k_page);
  REQUIRE(pool.class_store(k_page).slot_stride() == k_page);

  arbc::BlockRef a2 = *pool.allocate(k_page);
  REQUIRE(page_aligned(a2.data()));
  REQUIRE(pool.arena().store_count() == 1); // same class -> one store

  // A page+1 request rounds up to the 2*page rung: a distinct class, a distinct
  // store, a distinct stride.
  arbc::BlockRef b = *pool.allocate(k_page + 1);
  REQUIRE(page_aligned(b.data()));
  REQUIRE(b.size() == k_page + 1);
  REQUIRE(pool.class_store(k_page + 1).slot_stride() == 2 * k_page);
  REQUIRE(pool.arena().store_count() == 2); // distinct class -> distinct store

  // The 1 MiB default raster tile hits an exact power-of-two rung: zero waste.
  constexpr std::size_t mib = std::size_t{1} << 20;
  REQUIRE(arbc::BigBlockPool::class_stride(mib) == mib);
  arbc::BlockRef c = *pool.allocate(mib);
  REQUIRE(page_aligned(c.data()));
  REQUIRE(c.size() == mib);
  REQUIRE(pool.class_store(mib).slot_stride() == mib);
  REQUIRE(pool.arena().store_count() == 3);
}

TEST_CASE("class_stride rounds up to a power-of-two page multiple") {
  using P = arbc::BigBlockPool;
  REQUIRE(P::class_stride(0) == k_page);
  REQUIRE(P::class_stride(1) == k_page);
  REQUIRE(P::class_stride(k_page) == k_page);
  REQUIRE(P::class_stride(k_page + 1) == 2 * k_page);
  REQUIRE(P::class_stride(2 * k_page) == 2 * k_page);
  REQUIRE(P::class_stride(2 * k_page + 1) == 4 * k_page);
  REQUIRE(P::class_stride(std::size_t{1} << 20) == (std::size_t{1} << 20));
}

TEST_CASE("a blob filled through bytes() reads back identically via peek and resolve") {
  arbc::BigBlockPool pool;
  constexpr std::size_t n = k_page + 123; // rounds to the 2*page rung
  arbc::BlockRef blob = *pool.allocate(n);

  std::span<std::byte> fill = blob.bytes();
  REQUIRE(fill.size() == n);
  for (std::size_t i = 0; i < n; ++i) {
    fill[i] = static_cast<std::byte>((i * 31 + 7) & 0xFF);
  }

  const arbc::BlockSlotRef ref = blob.slot();

  // peek: zero-count read, exactly size() bytes.
  std::span<const std::byte> seen = pool.peek(ref);
  REQUIRE(seen.size() == n);
  for (std::size_t i = 0; i < n; ++i) {
    REQUIRE(seen[i] == static_cast<std::byte>((i * 31 + 7) & 0xFF));
  }

  // resolve: pin and read back through the returned handle's data pointer.
  auto pinned = pool.resolve(ref);
  REQUIRE(pinned.has_value());
  REQUIRE(pinned->size() == n);
  REQUIRE(std::equal(seen.begin(), seen.end(), pinned->data()));
}

// enforces: 15-memory-model#bulk-payloads-shared-by-refcount
TEST_CASE("a blob is shared by refcount and reclaimed to its class free pool on the last release") {
  arbc::BigBlockPool pool;
  arbc::BlockRef owner = *pool.allocate(k_page);
  std::fill(owner.bytes().begin(), owner.bytes().end(), std::byte{0x5A});

  const arbc::BlockSlotRef ref = owner.slot();
  const arbc::SlotIndex idx = owner.index();
  REQUIRE(pool.count(ref) == 1);

  const std::size_t reserved0 = pool.arena().total_bytes_reserved();
  const std::size_t live0 = pool.arena().total_slots_live();
  REQUIRE(live0 == 1);

  // A second owner retains the same blob (holder-holds-a-count).
  REQUIRE(pool.retain(ref).has_value());
  REQUIRE(pool.count(ref) == 2);

  // The first owner releases; the second keeps the blob live -- peek still sees
  // the identical bytes (shared by refcount, no data-page write on retain).
  owner = arbc::BlockRef{};
  REQUIRE(pool.count(ref) == 1);
  std::span<const std::byte> shared = pool.peek(ref);
  REQUIRE(shared.size() == k_page);
  REQUIRE(std::all_of(shared.begin(), shared.end(), [](std::byte b) { return b == std::byte{0x5A}; }));

  // The final release drops the count to zero and returns the slot to its class
  // free pool.
  pool.release(ref);
  REQUIRE(pool.arena().total_slots_live() == live0 - 1);

  // The next same-size allocate reuses the reclaimed slot with no growth.
  arbc::BlockRef reused = *pool.allocate(k_page);
  REQUIRE(reused.index() == idx);
  REQUIRE(pool.arena().total_bytes_reserved() == reserved0);
  REQUIRE(pool.arena().total_slots_live() == live0);
}

// enforces: 15-memory-model#bulk-payloads-shared-by-refcount
TEST_CASE("blobs_allocated counts distinct allocations only; reuse does not grow reserved bytes") {
  arbc::BigBlockPool pool;
  REQUIRE(pool.blobs_allocated() == 0);

  arbc::BlockRef blob = *pool.allocate(k_page);
  REQUIRE(pool.blobs_allocated() == 1);

  // Non-allocating operations never advance the counter.
  const arbc::BlockSlotRef ref = blob.slot();
  REQUIRE(pool.retain(ref).has_value());
  {
    auto pinned = pool.resolve(ref);
    REQUIRE(pinned.has_value());
    (void)pool.peek(ref);
    (void)pool.count(ref);
  }
  pool.release(ref); // drop the standalone retain
  REQUIRE(pool.blobs_allocated() == 1);

  // A churn-and-reuse burst (release-to-zero -> allocate same size) recycles the
  // one slot: reserved bytes stay flat after the first allocation.
  const std::size_t reserved0 = pool.arena().total_bytes_reserved();
  for (int i = 0; i < 32; ++i) {
    blob = arbc::BlockRef{};      // release to zero
    blob = *pool.allocate(k_page); // reuse the freed slot
  }
  REQUIRE(pool.blobs_allocated() == 1 + 32);
  REQUIRE(pool.arena().total_bytes_reserved() == reserved0);
}

TEST_CASE("retain at the count ceiling surfaces a loud overflow, never wrapping") {
  arbc::BigBlockPool pool;
  arbc::BlockRef blob = *pool.allocate(k_page);
  const arbc::BlockSlotRef ref = blob.slot();

  // Saturate the count directly on the store-owned column.
  arbc::SlotStore& store = pool.class_store(k_page);
  store.count_ref(blob.index()).store(std::numeric_limits<std::uint32_t>::max(),
                                      std::memory_order_release);

  auto retained = pool.retain(ref);
  REQUIRE_FALSE(retained.has_value());
  REQUIRE(retained.error() == arbc::RefError::CountOverflow);

  auto resolved = pool.resolve(ref);
  REQUIRE_FALSE(resolved.has_value());
  REQUIRE(resolved.error() == arbc::RefError::CountOverflow);

  // The count did not silently wrap to zero.
  REQUIRE(pool.count(ref) == std::numeric_limits<std::uint32_t>::max());

  // Restore a sane count so scope-exit reclamation is clean.
  store.count_ref(blob.index()).store(1, std::memory_order_release);
}

// A ReleaseFence that records diverted slots and returns them on demand -- the
// pool-only analogue of pool.checkpoints' durability fence (mirrors the one in
// free_pools.t.cpp).
class RecordingFence final : public arbc::ReleaseFence {
public:
  void on_release(arbc::SlotStore& /*store*/, arbc::SlotIndex index) override {
    d_quarantined.push_back(index);
  }
  void drain_to(arbc::SlotStore& store) {
    for (const arbc::SlotIndex idx : d_quarantined) {
      store.free_now(idx);
    }
    d_quarantined.clear();
  }
  std::size_t pending() const noexcept { return d_quarantined.size(); }

private:
  std::vector<arbc::SlotIndex> d_quarantined;
};

TEST_CASE("a durability fence on a class store quarantines a freed blob until free_now") {
  arbc::BigBlockPool pool;
  arbc::SlotStore& store = pool.class_store(k_page);
  RecordingFence fence;
  store.set_release_fence(&fence);

  arbc::BlockRef blob = *pool.allocate(k_page);
  const arbc::SlotIndex idx = blob.index();
  std::fill(blob.bytes().begin(), blob.bytes().end(), std::byte{0xAB});

  // Release to zero: with the fence installed the slot diverts to the fence
  // instead of the free pool.
  blob = arbc::BlockRef{};
  REQUIRE(fence.pending() == 1);

  // The blob's bytes stay resolvable while quarantined (the fence does not mutate
  // the data page).
  const auto* raw = static_cast<const std::byte*>(store.resolve(idx));
  REQUIRE(raw[0] == std::byte{0xAB});

  // A fresh allocation does NOT reuse the quarantined slot -- the writer grows.
  arbc::BlockRef grown = *pool.allocate(k_page);
  REQUIRE(grown.index() != idx);

  // free_now returns the quarantined slot; now the next allocation reuses it.
  fence.drain_to(store);
  REQUIRE(fence.pending() == 0);
  arbc::BlockRef reused = *pool.allocate(k_page);
  REQUIRE(reused.index() == idx);

  store.set_release_fence(nullptr);
}

#ifndef NDEBUG
// Generation tags are a debug-build discipline: a slot recycled out from under a
// stale BlockSlotRef must fault rather than silently read the live-again blob.
TEST_CASE("a stale BlockSlotRef to a recycled slot is caught by the generation tag (debug)") {
  arbc::BigBlockPool pool;

  arbc::BlockSlotRef stale;
  {
    arbc::BlockRef blob = *pool.allocate(k_page);
    stale = blob.slot();
    REQUIRE(pool.generation_matches(stale)); // fresh reference matches
    // blob drops -> count 0 -> release -> the slot's generation is bumped.
  }
  REQUIRE_FALSE(pool.generation_matches(stale)); // now stale

  // Recycle the slot: the next allocation reuses the same index with a fresh
  // generation; the stale reference must still not match.
  arbc::BlockRef reused = *pool.allocate(k_page);
  REQUIRE(reused.index() == stale.index()); // perfect-hole reuse
  REQUIRE_FALSE(pool.generation_matches(stale));
  REQUIRE(pool.generation_matches(reused.slot()));
}
#endif

// Concurrency smoke (doc 16 `:66-73`, TSan + asan): the writer allocates and
// fills blobs while a second thread releases blobs cross-thread (driving some to
// zero off the writer) and reader threads peek published blobs concurrently.
TEST_CASE("concurrent: writer allocates/fills while a thread releases and readers peek") {
  arbc::BigBlockPool pool;
  constexpr std::size_t blob = k_page; // one size class

  // Published blobs kept alive for the readers, each filled with a per-blob byte.
  constexpr int published = 16;
  std::vector<arbc::BlockRef> keep;
  std::vector<arbc::BlockSlotRef> pub_refs;
  for (int i = 0; i < published; ++i) {
    arbc::BlockRef r = *pool.allocate(blob);
    std::fill(r.bytes().begin(), r.bytes().end(), static_cast<std::byte>(i));
    pub_refs.push_back(r.slot());
    keep.push_back(std::move(r));
  }

  // Disposable blobs: each retained once so a standalone count survives the
  // BlockRef drop, then released cross-thread below (count 1 -> 0 off the writer).
  constexpr int disposable = 500;
  std::vector<arbc::BlockSlotRef> to_release;
  for (int i = 0; i < disposable; ++i) {
    arbc::BlockRef r = *pool.allocate(blob);
    const arbc::BlockSlotRef s = r.slot();
    REQUIRE(pool.retain(s).has_value());
    to_release.push_back(s);
    // r drops here -> count 1 (the standalone retain).
  }

  std::atomic<bool> go{false};
  std::atomic<bool> bad{false};

  std::thread releaser([&] {
    while (!go.load(std::memory_order_acquire)) {
    }
    for (const arbc::BlockSlotRef s : to_release) {
      pool.release(s); // count 1 -> 0 -> SlotStore::release, off the writer
    }
  });

  std::vector<std::thread> readers;
  for (int t = 0; t < 4; ++t) {
    readers.emplace_back([&] {
      while (!go.load(std::memory_order_acquire)) {
      }
      for (int round = 0; round < 500; ++round) {
        for (int i = 0; i < published; ++i) {
          std::span<const std::byte> span = pool.peek(pub_refs[i]);
          if (span.size() != blob) {
            bad.store(true, std::memory_order_relaxed);
            continue;
          }
          for (std::byte byte : span) {
            if (byte != static_cast<std::byte>(i)) {
              bad.store(true, std::memory_order_relaxed);
              break;
            }
          }
        }
      }
    });
  }

  go.store(true, std::memory_order_release);

  // The writer keeps allocating fresh blobs during the release window (writer
  // allocate racing the releaser's cross-thread release); each drops to zero and
  // frees on the writer thread.
  for (int i = 0; i < disposable; ++i) {
    arbc::BlockRef churn = *pool.allocate(blob);
    (void)churn; // drops immediately -> count 0 -> freed on the writer
  }

  releaser.join();
  for (std::thread& th : readers) {
    th.join();
  }

  REQUIRE_FALSE(bad.load());

  // Drop the published blobs: the arena returns to baseline, every blob accounted.
  keep.clear();
  REQUIRE(pool.arena().total_slots_live() == 0);
}

} // namespace
