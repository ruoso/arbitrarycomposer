#include <arbc/pool/chunk_source.hpp>
#include <arbc/pool/slot_store.hpp>
#include <arbc/pool/typed_store.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <set>
#include <thread>
#include <vector>

namespace {

// A ChunkSource that refuses to grow after `budget` chunks, to exercise the
// allocation error path as a value.
class CappedChunkSource final : public arbc::ChunkSource {
public:
  explicit CappedChunkSource(int budget) : d_budget(budget) {}

  arbc::expected<arbc::ChunkSpan, arbc::PoolError> acquire(std::size_t size,
                                                           std::size_t alignment) override {
    if (d_granted >= d_budget) {
      return arbc::unexpected(arbc::PoolError::OutOfMemory);
    }
    ++d_granted;
    return d_delegate.acquire(size, alignment);
  }
  void release(arbc::ChunkSpan span) noexcept override { d_delegate.release(span); }

private:
  arbc::AnonymousChunkSource d_delegate;
  int d_budget;
  int d_granted{0};
};

TEST_CASE("distinct allocations yield distinct slots") {
  arbc::Arena arena;
  arbc::SlotStore& store = arena.store_for(32, alignof(std::max_align_t));

  std::set<arbc::SlotIndex> indices;
  std::set<void*> addresses;
  for (int i = 0; i < 64; ++i) {
    auto slot = store.allocate();
    REQUIRE(slot.has_value());
    REQUIRE(indices.insert(*slot).second);
    REQUIRE(addresses.insert(store.resolve(*slot)).second);
  }
}

// enforces: 15-memory-model#slots-recycle-in-place
TEST_CASE("a released slot is the next same-class allocation's perfect hole") {
  arbc::Arena arena;
  arbc::SlotStore& store = arena.store_for(48, alignof(std::max_align_t));

  auto a = store.allocate();
  auto b = store.allocate();
  auto c = store.allocate();
  REQUIRE(a.has_value());
  REQUIRE(b.has_value());
  REQUIRE(c.has_value());
  void* b_address = store.resolve(*b);

  store.release(*b);
  auto reused = store.allocate();
  REQUIRE(reused.has_value());
  REQUIRE(*reused == *b);
  REQUIRE(store.resolve(*reused) == b_address);
  REQUIRE(*reused != *a);
  REQUIRE(*reused != *c);
}

TEST_CASE("accounting tracks alloc and release exactly") {
  arbc::Arena arena;
  // Four slots per chunk to make growth observable.
  arbc::SlotStore& store = arena.store_for(64, alignof(std::max_align_t), /*chunk_bits=*/2);

  REQUIRE(store.slots_live() == 0);
  REQUIRE(store.slots_capacity() == 0);
  REQUIRE(store.bytes_reserved() == 0);

  std::vector<arbc::SlotIndex> live;
  for (int i = 0; i < 5; ++i) {
    auto slot = store.allocate();
    REQUIRE(slot.has_value());
    live.push_back(*slot);
  }
  // Five slots crossed the four-slot chunk boundary: two chunks reserved.
  REQUIRE(store.slots_live() == 5);
  REQUIRE(store.slots_capacity() == 8);
  REQUIRE(store.bytes_reserved() >= 8 * store.slot_stride());

  store.release(live.back());
  live.pop_back();
  REQUIRE(store.slots_live() == 4);
  REQUIRE(store.slots_capacity() == 8); // capacity is monotonic; release keeps chunks
}

TEST_CASE("multiple stores of different slot sizes coexist in one arena") {
  arbc::Arena arena;
  arbc::SlotStore& small = arena.store_for(16, alignof(std::max_align_t));
  arbc::SlotStore& medium = arena.store_for(64, alignof(std::max_align_t));
  arbc::SlotStore& large = arena.store_for(256, alignof(std::max_align_t));

  REQUIRE(arena.store_count() == 3);
  REQUIRE(small.slot_stride() == 16);
  REQUIRE(medium.slot_stride() == 64);
  REQUIRE(large.slot_stride() == 256);

  // Same size class resolves to the same store (free sharing between
  // same-sized record types).
  REQUIRE(&arena.store_for(64, alignof(std::max_align_t)) == &medium);
  REQUIRE(arena.store_count() == 3);

  REQUIRE(small.allocate().has_value());
  REQUIRE(medium.allocate().has_value());
  REQUIRE(large.allocate().has_value());
  REQUIRE(arena.total_slots_live() == 3);
}

TEST_CASE("allocation surfaces an error when the ChunkSource refuses to grow") {
  CappedChunkSource source(/*budget=*/1);
  arbc::Arena arena(source);
  // One chunk of two slots is all the source will grant.
  arbc::SlotStore& store = arena.store_for(32, alignof(std::max_align_t), /*chunk_bits=*/1);

  REQUIRE(store.allocate().has_value());
  REQUIRE(store.allocate().has_value());

  auto denied = store.allocate(); // needs a second chunk; source refuses
  REQUIRE_FALSE(denied.has_value());
  REQUIRE(denied.error() == arbc::PoolError::OutOfMemory);

  // A refused growth leaves accounting untouched — the store stays usable.
  REQUIRE(store.slots_live() == 2);
  REQUIRE(store.slots_capacity() == 2);
}

// enforces: 15-memory-model#chunk-growth-preserves-addresses
TEST_CASE("pointers taken before growth remain valid and unchanged after growth") {
  arbc::Arena arena;
  // Two slots per chunk forces many growths over the run.
  arbc::SlotStore& store = arena.store_for(sizeof(std::uint64_t), alignof(std::uint64_t),
                                           /*chunk_bits=*/1);

  struct Recorded {
    arbc::SlotIndex index;
    std::uint64_t* address;
  };
  std::vector<Recorded> recorded;
  for (std::uint64_t i = 0; i < 8; ++i) {
    auto slot = store.allocate();
    REQUIRE(slot.has_value());
    auto* p = static_cast<std::uint64_t*>(store.resolve(*slot));
    *p = 0xA5A5'0000u + i; // sentinel written before growth
    recorded.push_back({*slot, p});
  }

  // Force substantial growth (many new chunks appended).
  for (int i = 0; i < 4096; ++i) {
    REQUIRE(store.allocate().has_value());
  }

  // Every pre-growth pointer is byte-identical and still readable.
  for (std::uint64_t i = 0; i < recorded.size(); ++i) {
    REQUIRE(store.resolve(recorded[i].index) == recorded[i].address);
    REQUIRE(*recorded[i].address == 0xA5A5'0000u + i);
  }
}

TEST_CASE("TypedStore constructs, resolves, and reuses slots") {
  arbc::Arena arena;
  arbc::TypedStore<std::uint64_t> store(arena);

  auto a = store.allocate(std::uint64_t{111});
  auto b = store.allocate(std::uint64_t{222});
  REQUIRE(a.has_value());
  REQUIRE(b.has_value());
  REQUIRE(*store.resolve(*a) == 111);
  REQUIRE(*store.resolve(*b) == 222);

  store.release(*b);
  auto c = store.allocate(std::uint64_t{333});
  REQUIRE(c.has_value());
  REQUIRE(*c == *b); // perfect-hole reuse survives the typed veneer
  REQUIRE(*store.resolve(*c) == 333);
}

// Workers resolve indices to pointers while the writer grows the directory:
// the acquire/release directory protocol must make this race-free without any
// reader lock (TSan lanes in CI observe the race; here we assert stability).
TEST_CASE("readers resolve indices concurrently with writer growth") {
  arbc::Arena arena;
  arbc::SlotStore& store = arena.store_for(sizeof(std::uint32_t), alignof(std::uint32_t),
                                           /*chunk_bits=*/1);

  constexpr int tracked_count = 256;
  std::vector<arbc::SlotIndex> tracked;
  for (int i = 0; i < tracked_count; ++i) {
    auto slot = store.allocate(); // all allocation stays on this (writer) thread
    REQUIRE(slot.has_value());
    *static_cast<std::uint32_t*>(store.resolve(*slot)) = static_cast<std::uint32_t>(*slot);
    tracked.push_back(*slot);
  }

  std::atomic<bool> go{false};
  std::atomic<bool> stop{false};
  std::atomic<bool> mismatch{false};

  std::vector<std::thread> readers;
  for (int r = 0; r < 4; ++r) {
    readers.emplace_back([&] {
      while (!go.load(std::memory_order_acquire)) {
      }
      while (!stop.load(std::memory_order_acquire)) {
        for (arbc::SlotIndex index : tracked) {
          const auto* p = static_cast<const std::uint32_t*>(store.resolve(index));
          if (*p != index) {
            mismatch.store(true, std::memory_order_relaxed);
          }
        }
      }
    });
  }

  go.store(true, std::memory_order_release);
  // Writer keeps growing the directory while readers resolve the stable set.
  for (int i = 0; i < 20000; ++i) {
    REQUIRE(store.allocate().has_value());
  }
  stop.store(true, std::memory_order_release);
  for (std::thread& reader : readers) {
    reader.join();
  }

  REQUIRE_FALSE(mismatch.load());
}

} // namespace
