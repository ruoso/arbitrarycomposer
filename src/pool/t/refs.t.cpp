#include <arbc/pool/chunk_source.hpp>
#include <arbc/pool/refs.hpp>
#include <arbc/pool/slot_store.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <thread>
#include <type_traits>
#include <vector>

#if defined(__linux__)
#include <sys/mman.h>
#endif

namespace {

// A record-embeddable payload that reports its own destruction, so tests can
// witness that reclamation actually runs `~T` (the cpioo release-does-not-run-
// the-destructor gap this layer's sink closes).
struct Tracked {
  int value;
  int* destructions;
  Tracked(int v, int* d) : value(v), destructions(d) {}
  ~Tracked() {
    if (destructions != nullptr) {
      ++*destructions;
    }
  }
};

TEST_CASE("create yields an owning Ref with a live count and a one-indirection deref") {
  arbc::Arena arena;
  arbc::RefStore<Tracked> store(arena);
  int destructions = 0;

  auto created = store.create(5, &destructions);
  REQUIRE(created.has_value());
  arbc::Ref<Tracked>& r = *created;
  REQUIRE(static_cast<bool>(r));
  REQUIRE(r->value == 5);
  REQUIRE((*r).value == 5);
  REQUIRE(store.count(r.slot()) == 1);
  REQUIRE(store.slots_live() == 1);
}

TEST_CASE("copy and copy-assign bump the count; the default sink reclaims on the last drop") {
  arbc::Arena arena;
  arbc::RefStore<Tracked> store(arena);
  int destructions = 0;
  arbc::SlotIndex idx = 0;

  {
    arbc::Ref<Tracked> a = *store.create(5, &destructions);
    idx = a.index();
    arbc::Ref<Tracked> b = a; // copy -> count 2
    REQUIRE(store.count(a.slot()) == 2);
    arbc::Ref<Tracked> c;
    c = a; // copy-assign -> count 3
    REQUIRE(store.count(a.slot()) == 3);
    REQUIRE(a->value == 5);
    REQUIRE(b->value == 5);
    REQUIRE(c->value == 5);
    REQUIRE(destructions == 0);
    // a, b, c all drop at scope exit -> count 0 -> immediate reclaim.
  }
  REQUIRE(destructions == 1);

  // The reclaimed slot is a perfect hole for the next same-class allocation.
  arbc::Ref<Tracked> d = *store.create(6, &destructions);
  REQUIRE(d.index() == idx);
  REQUIRE(d->value == 6);
}

TEST_CASE("move transfers ownership without any count traffic") {
  arbc::Arena arena;
  arbc::RefStore<Tracked> store(arena);
  int destructions = 0;

  arbc::Ref<Tracked> a = *store.create(1, &destructions);
  arbc::SlotRef<Tracked> s = a.slot();
  REQUIRE(store.count(s) == 1);

  arbc::Ref<Tracked> b = std::move(a); // move-construct: no bump
  REQUIRE(store.count(s) == 1);
  REQUIRE_FALSE(static_cast<bool>(a)); // moved-from is empty
  REQUIRE(static_cast<bool>(b));
  REQUIRE(b->value == 1);

  arbc::Ref<Tracked> c;
  c = std::move(b); // move-assign: no bump
  REQUIRE(store.count(s) == 1);
  REQUIRE_FALSE(static_cast<bool>(b));
  REQUIRE(c->value == 1);
  REQUIRE(destructions == 0); // still exactly one owner alive
}

TEST_CASE("the zero-count sink is a seam: an installed sink defers reclamation") {
  arbc::Arena arena;
  arbc::RefStore<Tracked> store(arena);
  int destructions = 0;

  struct RecordingSink final : arbc::ZeroCountSink {
    std::vector<arbc::SlotIndex> zeroed;
    void on_zero(arbc::SlotIndex index) override { zeroed.push_back(index); }
  } sink;
  store.set_zero_sink(&sink);

  {
    arbc::Ref<Tracked> r = *store.create(7, &destructions);
    // r drops -> count 0 -> sink::on_zero, but this sink does NOT reclaim.
  }
  REQUIRE(sink.zeroed.size() == 1);
  REQUIRE(destructions == 0); // release did not destroy: the seam works

  // A deferred queue (pool.reclamation) reclaims when it pops; do that now.
  store.reclaim(sink.zeroed.front());
  REQUIRE(destructions == 1);
}

TEST_CASE("a SlotRef stored in a record round-trips back to the object") {
  arbc::Arena arena;
  arbc::RefStore<Tracked> store(arena);
  int destructions = 0;

  struct Record { // stand-in for a doc-15 record: only index-only refs inside
    arbc::SlotRef<Tracked> child;
  };
  static_assert(std::is_trivially_copyable_v<Record>,
                "a record holding SlotRefs must stay trivially copyable");

  Record rec;
  {
    arbc::Ref<Tracked> owner = *store.create(77, &destructions);
    rec.child = owner.slot();
    // Holder-holds-a-count: the record takes its own count on the target.
    REQUIRE(store.retain(rec.child).has_value());
    REQUIRE(store.count(rec.child) == 2);
    // owner drops -> count 2 -> 1; the record's count keeps the object alive.
  }
  REQUIRE(destructions == 0);
  REQUIRE(store.count(rec.child) == 1);

  {
    auto resolved = store.resolve(rec.child); // pin for use
    REQUIRE(resolved.has_value());
    arbc::Ref<Tracked>& handle = *resolved;
    REQUIRE(handle->value == 77);
    // Non-owning peek reaches the same object with zero refcount traffic.
    REQUIRE(store.peek(rec.child)->value == 77);
    REQUIRE(store.count(rec.child) == 2); // resolve pinned it
  }
  REQUIRE(store.count(rec.child) == 1); // the transient pin released
  REQUIRE(destructions == 0);

  store.release(rec.child); // tear the record down -> object reclaimed
  REQUIRE(destructions == 1);
}

TEST_CASE("retain and resolve surface a loud error at the count ceiling, never wrapping") {
  arbc::Arena arena;
  arbc::RefStore<std::uint64_t> store(arena);

  arbc::Ref<std::uint64_t> r = *store.create(std::uint64_t{0});
  arbc::SlotRef<std::uint64_t> s = r.slot();

  // Saturate the count through the reconstruction seam.
  store.set_count(s, std::numeric_limits<std::uint32_t>::max());

  auto pinned = store.retain(s);
  REQUIRE_FALSE(pinned.has_value());
  REQUIRE(pinned.error() == arbc::RefError::CountOverflow);

  auto resolved = store.resolve(s);
  REQUIRE_FALSE(resolved.has_value());
  REQUIRE(resolved.error() == arbc::RefError::CountOverflow);

  // The count did not silently wrap to zero.
  REQUIRE(store.count(s) == std::numeric_limits<std::uint32_t>::max());

  // Restore a sane count so scope-exit reclamation is clean.
  store.set_count(s, 1);
}

TEST_CASE("SlotRef is an embeddable, position-independent, trivially copyable value") {
  static_assert(std::is_standard_layout_v<arbc::SlotRef<int>>);
  static_assert(std::is_trivially_copyable_v<arbc::SlotRef<int>>);
#ifdef NDEBUG
  // The persistent-map node size is written against this; the guarantee is
  // release-only (debug carries a generation tag, so the header static_assert
  // that mirrors this is `#ifdef NDEBUG`-gated too).
  static_assert(sizeof(arbc::SlotRef<int>) == 4);
#endif
  SUCCEED();
}

#ifndef NDEBUG
// Generation tags are a debug-build discipline (doc 15): a slot recycled out
// from under a stale reference must fault rather than silently read the
// live-again slot. We witness the trap through the predicate the resolution
// asserts are built on, so the test observes the condition without aborting.
TEST_CASE("a stale SlotRef to a recycled slot is caught by the generation tag") {
  arbc::Arena arena;
  arbc::RefStore<std::uint64_t> store(arena);

  arbc::SlotRef<std::uint64_t> stale;
  {
    arbc::Ref<std::uint64_t> ref = *store.create(std::uint64_t{42});
    stale = ref.slot();
    REQUIRE(store.generation_matches(stale)); // fresh reference matches
    // ref drops -> count 0 -> reclaim -> the slot's generation is bumped.
  }
  REQUIRE_FALSE(store.generation_matches(stale)); // the reference is now stale

  // Recycle the slot: the next allocation reuses the same index with a fresh
  // generation. The stale reference must still not match the live slot.
  arbc::Ref<std::uint64_t> reused = *store.create(std::uint64_t{99});
  REQUIRE(reused.index() == stale.index()); // perfect-hole reuse
  REQUIRE_FALSE(store.generation_matches(stale));
  REQUIRE(store.generation_matches(reused.slot()));
}
#endif

TEST_CASE("concurrent pin/unpin churns the count from many threads without corruption") {
  arbc::Arena arena;
  arbc::RefStore<std::uint64_t> store(arena);

  arbc::Ref<std::uint64_t> base = *store.create(std::uint64_t{0xABCD});
  arbc::SlotRef<std::uint64_t> s = base.slot(); // base holds one count throughout

  constexpr int thread_count = 8;
  constexpr int iterations = 2000;
  std::atomic<bool> go{false};
  std::atomic<bool> bad{false};

  std::vector<std::thread> threads;
  for (int t = 0; t < thread_count; ++t) {
    threads.emplace_back([&] {
      while (!go.load(std::memory_order_acquire)) {
      }
      for (int i = 0; i < iterations; ++i) {
        auto pinned = store.resolve(s); // pin from a non-writer thread
        if (!pinned.has_value()) {
          bad.store(true, std::memory_order_relaxed);
          continue;
        }
        arbc::Ref<std::uint64_t>& handle = *pinned;
        if (*handle != 0xABCD) {
          bad.store(true, std::memory_order_relaxed);
        }
        arbc::Ref<std::uint64_t> copy = handle; // extra pin traffic
        if (*copy != 0xABCD) {
          bad.store(true, std::memory_order_relaxed);
        }
        // Standalone retain/release round-trip (record store/teardown shape).
        if (store.retain(s).has_value()) {
          store.release(s);
        } else {
          bad.store(true, std::memory_order_relaxed);
        }
        // pinned and copy drop here -> unpin.
      }
    });
  }

  go.store(true, std::memory_order_release);
  for (std::thread& th : threads) {
    th.join();
  }

  REQUIRE_FALSE(bad.load());
  // No worker ever reached zero (base held a count), so nothing was reclaimed
  // off-thread and the count is back to the base one.
  REQUIRE(store.count(s) == 1);
  REQUIRE(*base == 0xABCD);
}

#if defined(__linux__)

// A ChunkSource that mmaps page-granular anonymous chunks and can flip their
// protection, so the test can freeze the DATA pages read-only and prove pin/
// unpin traffic never writes them.
class MmapRecordingSource final : public arbc::ChunkSource {
public:
  arbc::expected<arbc::ChunkSpan, arbc::PoolError> acquire(std::size_t size,
                                                           std::size_t /*alignment*/) override {
    const std::size_t rounded = (size + 4095) & ~std::size_t{4095};
    void* base =
        ::mmap(nullptr, rounded, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
      return arbc::unexpected(arbc::PoolError::OutOfMemory);
    }
    d_spans.push_back(arbc::ChunkSpan{base, rounded});
    return arbc::ChunkSpan{base, rounded};
  }
  void release(arbc::ChunkSpan span) noexcept override { ::munmap(span.base, span.size); }

  void protect(int prot) {
    for (const arbc::ChunkSpan& span : d_spans) {
      ::mprotect(span.base, span.size, prot);
    }
  }

private:
  std::vector<arbc::ChunkSpan> d_spans;
};

// enforces: 15-memory-model#refcounts-outside-data-pages
TEST_CASE("pin/unpin traffic never faults with the data pages mprotected read-only") {
  MmapRecordingSource source;
  arbc::Arena arena(source);
  arbc::RefStore<std::uint64_t> store(arena);

  std::vector<arbc::SlotRef<std::uint64_t>> slots;
  {
    std::vector<arbc::Ref<std::uint64_t>> keep;
    for (std::uint64_t i = 0; i < 64; ++i) {
      auto created = store.create(0xD000u + i);
      REQUIRE(created.has_value());
      keep.push_back(*created);
    }
    for (arbc::Ref<std::uint64_t>& r : keep) {
      arbc::SlotRef<std::uint64_t> s = r.slot();
      REQUIRE(store.retain(s).has_value()); // a standalone count survives `keep`
      slots.push_back(s);
    }
    // keep drops here (on this writer thread): every count goes 2 -> 1.
  }

  // Freeze the data pages. From here every reference operation must touch only
  // the anonymous count table, never a data chunk.
  source.protect(PROT_READ);

  for (int round = 0; round < 500; ++round) {
    for (arbc::SlotRef<std::uint64_t> s : slots) {
      auto pinned = store.resolve(s); // increments count (heap), reads data (RO)
      REQUIRE(pinned.has_value());
      const volatile std::uint64_t observed = **pinned; // read a read-only data page
      (void)observed;
      arbc::Ref<std::uint64_t> copy = *pinned; // more pin traffic
      (void)copy;
    }
    for (arbc::SlotRef<std::uint64_t> s : slots) {
      REQUIRE(store.retain(s).has_value());
    }
    for (arbc::SlotRef<std::uint64_t> s : slots) {
      store.release(s);
    }
  }

  // Restore writability before teardown so reclamation and munmap are safe.
  source.protect(PROT_READ | PROT_WRITE);
  for (arbc::SlotRef<std::uint64_t> s : slots) {
    store.release(s); // drop the surviving counts -> reclaim on this thread
  }
}

#endif // __linux__

} // namespace
