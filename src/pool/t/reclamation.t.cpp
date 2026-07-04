#include <arbc/pool/chunk_source.hpp>
#include <arbc/pool/reclamation.hpp>
#include <arbc/pool/refs.hpp>
#include <arbc/pool/slot_store.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <sys/mman.h>
#endif

namespace {

// A record-embeddable payload that reports its own destruction, so tests can
// witness that DEFERRED reclamation runs `~T` only on drain (never inline at
// release-to-zero) -- the cpioo release-does-not-run-the-destructor gap.
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

// enforces: 15-memory-model#release-enqueues-never-destroys-inline
TEST_CASE("a deferred sink enqueues on release-to-zero: ~T runs only on drain") {
  arbc::Arena arena;
  arbc::RefStore<Tracked> store(arena);
  arbc::DeferredReclaimSink<Tracked> sink(store);
  arbc::ReclamationQueue queue;
  queue.register_store(store, sink);

  int destructions = 0;
  arbc::SlotIndex idx = 0;
  {
    arbc::Ref<Tracked> r = *store.create(5, &destructions);
    idx = r.index();
    // r drops here -> count 0 -> deferred sink enqueues, does NOT destroy.
  }
  // Release-to-zero left the object bit-live: the destructor has not run and
  // the slot is still counted live (the slot returns to the free list only on
  // drain, never on the release path).
  REQUIRE(destructions == 0);
  REQUIRE(store.slots_live() == 1);

  queue.drain();

  // Only drain advanced the destructor and returned the slot to the free list.
  REQUIRE(destructions == 1);
  REQUIRE(store.slots_live() == 0);

  // The reclaimed slot is a perfect hole for the next same-class allocation.
  arbc::Ref<Tracked> reused = *store.create(6, &destructions);
  REQUIRE(reused.index() == idx);
  REQUIRE(reused->value == 6);
}

TEST_CASE("set_zero_sink(nullptr) restores inline reclamation (the refs.md behavior)") {
  arbc::Arena arena;
  arbc::RefStore<Tracked> store(arena);
  arbc::DeferredReclaimSink<Tracked> sink(store);
  store.set_zero_sink(&sink);

  int destructions = 0;
  {
    arbc::Ref<Tracked> r = *store.create(7, &destructions);
  }
  REQUIRE(destructions == 0); // deferred: nothing ran inline
  REQUIRE(store.slots_live() == 1);

  // Restore the built-in immediate sink; the slot is still on the deferred
  // stack, so drain it first, then prove inline reclamation is back.
  store.set_zero_sink(nullptr);
  REQUIRE(store.drain_pending()); // the pending slot still reclaims cleanly
  REQUIRE(destructions == 1);
  REQUIRE(store.slots_live() == 0);

  {
    arbc::Ref<Tracked> r = *store.create(8, &destructions);
    // r drops -> count 0 -> immediate sink reclaims inline, right here.
  }
  REQUIRE(destructions == 2);
  REQUIRE(store.slots_live() == 0);
}

TEST_CASE("empty-drain, double-drain, and a store with no deferred sink are no-ops") {
  arbc::Arena arena;
  arbc::RefStore<Tracked> store(arena);
  arbc::DeferredReclaimSink<Tracked> sink(store);
  arbc::ReclamationQueue queue;
  queue.register_store(store, sink);

  int destructions = 0;
  arbc::Ref<Tracked> kept = *store.create(1, &destructions);

  // Nothing has hit zero: an empty drain reclaims nothing and keeps the object.
  queue.drain();
  REQUIRE(destructions == 0);
  REQUIRE(store.slots_live() == 1);

  // Drop to zero, drain once (reclaims), then drain again (no-op).
  kept = arbc::Ref<Tracked>{};
  queue.drain();
  REQUIRE(destructions == 1);
  REQUIRE(store.slots_live() == 0);
  queue.drain();
  REQUIRE(destructions == 1); // double-drain did not re-run ~T

  // A store carrying only the built-in immediate sink never enqueues, so a
  // direct drain_pending finds an empty stack -- a no-op.
  arbc::RefStore<Tracked> immediate_store(arena);
  int immediate_destructions = 0;
  { arbc::Ref<Tracked> r = *immediate_store.create(9, &immediate_destructions); }
  REQUIRE(immediate_destructions == 1); // immediate sink reclaimed inline
  REQUIRE_FALSE(immediate_store.drain_pending());
  REQUIRE(immediate_destructions == 1);
}

// A node that holds a RETAINED SlotRef to its child and releases that count in
// its destructor. Under a deferred sink, that release only enqueues the child,
// so ~Node never re-enters ~Node -- the cascade is a loop through the queue, not
// C++ recursion. A shared probe records destructor count and the maximum nesting
// depth of ~Node observed across the whole cascade.
struct CascadeProbe {
  int destructions = 0;
  int depth = 0;
  int max_depth = 0;
};

struct Node {
  arbc::SlotRef<Node> child{};
  bool has_child = false;
  arbc::RefStore<Node>* store = nullptr;
  CascadeProbe* probe = nullptr;

  Node() = default;

  ~Node() {
    ++probe->destructions;
    ++probe->depth;
    if (probe->depth > probe->max_depth) {
      probe->max_depth = probe->depth;
    }
    if (has_child) {
      // Holder-holds-a-count: drop the count this node held on its child. With
      // a deferred sink this only ENQUEUES the child (no re-entrant ~Node); the
      // next drain batch reclaims it and continues the cascade.
      store->release(child);
    }
    --probe->depth;
  }
};

// enforces: 15-memory-model#deferred-cascade-reclaims-whole-subtree
TEST_CASE("draining a released deep chain reclaims the whole subtree, iteratively") {
  constexpr int depth = 4096; // deep enough that a recursive cascade is pathological

  arbc::Arena arena;
  arbc::RefStore<Node> store(arena);
  arbc::DeferredReclaimSink<Node> sink(store);
  arbc::ReclamationQueue queue;
  queue.register_store(store, sink);

  CascadeProbe probe;
  const std::size_t baseline = store.slots_live();

  // Build a `depth`-deep chain bottom-up. Each parent retains a SlotRef to its
  // child (the record-holds-a-count convention); the intermediate owning Refs
  // drop as we climb, so every node ends at count 1.
  arbc::Ref<Node> current = *store.create();
  {
    Node& leaf = *current;
    leaf.store = &store;
    leaf.probe = &probe;
    leaf.has_child = false;
  }
  for (int i = 1; i < depth; ++i) {
    arbc::Ref<Node> parent = *store.create();
    Node& p = *parent;
    p.store = &store;
    p.probe = &probe;
    p.child = current.slot();
    REQUIRE(store.retain(p.child).has_value()); // the record takes its own count
    p.has_child = true;
    current = std::move(parent); // old `current` Ref drops: child goes 2 -> 1
  }

  REQUIRE(store.slots_live() == baseline + static_cast<std::size_t>(depth));

  // Drop the only root reference: the root hits zero and is enqueued (nothing
  // destroyed yet -- the whole subtree is still bit-live).
  current = arbc::Ref<Node>{};
  REQUIRE(probe.destructions == 0);
  REQUIRE(store.slots_live() == baseline + static_cast<std::size_t>(depth));

  // A single drain-to-quiescence cascades the entire chain.
  queue.drain();

  REQUIRE(probe.destructions == depth);            // every ~Node ran exactly once
  REQUIRE(store.slots_live() == baseline);         // leak check: back to baseline
  REQUIRE(probe.max_depth == 1);                   // iterative: ~Node never nested
}

TEST_CASE("an enqueue burst allocates nothing and publishes no reclaim-link chunk") {
  arbc::Arena arena;
  arbc::RefStore<std::uint64_t> store(arena);
  arbc::DeferredReclaimSink<std::uint64_t> sink(store);
  arbc::ReclamationQueue queue;
  queue.register_store(store, sink);

  constexpr int count = 512;
  std::vector<arbc::SlotRef<std::uint64_t>> slots;
  for (int i = 0; i < count; ++i) {
    arbc::Ref<std::uint64_t> r = *store.create(static_cast<std::uint64_t>(i));
    arbc::SlotRef<std::uint64_t> s = r.slot();
    REQUIRE(store.retain(s).has_value()); // a standalone count survives `r`
    slots.push_back(s);
  }

  const std::size_t live_before = store.slots_live();
  const std::size_t chunks_before = store.reclaim_chunks_published();

  // Burst of release-to-zero enqueues: every count 1 -> 0, each slot pushed onto
  // the reclaim stack. Enqueue frees nothing and publishes no chunk.
  for (arbc::SlotRef<std::uint64_t> s : slots) {
    store.release(s);
  }

  REQUIRE(store.slots_live() == live_before);                     // enqueue frees no slot
  REQUIRE(store.reclaim_chunks_published() == chunks_before);     // no chunk published

  queue.drain();
  REQUIRE(store.slots_live() == live_before - count);             // drain returned them
}

TEST_CASE("multiple producers enqueue concurrently while the writer drains") {
  arbc::Arena arena;
  arbc::RefStore<Tracked> store(arena);
  arbc::DeferredReclaimSink<Tracked> sink(store);
  arbc::ReclamationQueue queue;
  queue.register_store(store, sink);

  constexpr int producer_count = 8;
  constexpr int per_producer = 500;
  constexpr int total = producer_count * per_producer;

  // The writer thread creates every slot up front (create is writer-only) and
  // hands each producer a disjoint block of standalone-counted SlotRefs.
  int destructions = 0;
  std::vector<std::vector<arbc::SlotRef<Tracked>>> blocks(producer_count);
  for (int p = 0; p < producer_count; ++p) {
    for (int i = 0; i < per_producer; ++i) {
      arbc::Ref<Tracked> r = *store.create(p * per_producer + i, &destructions);
      arbc::SlotRef<Tracked> s = r.slot();
      REQUIRE(store.retain(s).has_value()); // survive the owning Ref's drop
      blocks[p].push_back(s);
    }
  }
  REQUIRE(store.slots_live() == total);

  std::atomic<bool> go{false};
  std::atomic<int> done{0};

  std::vector<std::thread> producers;
  for (int p = 0; p < producer_count; ++p) {
    producers.emplace_back([&, p] {
      while (!go.load(std::memory_order_acquire)) {
      }
      for (arbc::SlotRef<Tracked> s : blocks[p]) {
        store.release(s); // count 1 -> 0 -> deferred enqueue (RT-safe path)
      }
      done.fetch_add(1, std::memory_order_release);
    });
  }

  go.store(true, std::memory_order_release);

  // The writer drains concurrently with the producers, then once more after
  // they have all finished to sweep whatever was enqueued last.
  while (done.load(std::memory_order_acquire) < producer_count) {
    queue.drain();
  }
  queue.drain();

  for (std::thread& th : producers) {
    th.join();
  }

  REQUIRE(destructions == total);       // every ~T fired exactly once
  REQUIRE(store.slots_live() == 0);     // the arena is back to baseline
}

#if defined(__linux__)

// A ChunkSource that mmaps page-granular anonymous chunks and can flip their
// protection, so the test can freeze the DATA pages read-only and prove the
// deferred enqueue path never writes them (it touches only the anonymous
// refcount and reclaim-link tables).
class MmapRecordingSource final : public arbc::ChunkSource {
public:
  arbc::expected<arbc::ChunkSpan, arbc::PoolError> acquire(std::size_t size,
                                                           std::size_t /*alignment*/) override {
    const std::size_t rounded = (size + 4095) & ~std::size_t{4095};
    void* base = ::mmap(nullptr, rounded, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
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
TEST_CASE("release-to-zero enqueues never fault with the data pages mprotected read-only") {
  MmapRecordingSource source;
  arbc::Arena arena(source);
  arbc::RefStore<std::uint64_t> store(arena);
  arbc::DeferredReclaimSink<std::uint64_t> sink(store);
  arbc::ReclamationQueue queue;
  queue.register_store(store, sink);

  std::vector<arbc::SlotRef<std::uint64_t>> slots;
  {
    std::vector<arbc::Ref<std::uint64_t>> keep;
    for (std::uint64_t i = 0; i < 128; ++i) {
      auto created = store.create(0xE000u + i);
      REQUIRE(created.has_value());
      keep.push_back(*created);
    }
    for (arbc::Ref<std::uint64_t>& r : keep) {
      arbc::SlotRef<std::uint64_t> s = r.slot();
      REQUIRE(store.retain(s).has_value()); // a standalone count survives `keep`
      slots.push_back(s);
    }
    // keep drops here (writer thread): every count goes 2 -> 1, no zero yet.
  }

  // Freeze the data pages. From here the release-to-zero enqueue path must touch
  // only the anonymous count and reclaim-link tables, never a data chunk.
  source.protect(PROT_READ);
  for (arbc::SlotRef<std::uint64_t> s : slots) {
    store.release(s); // count 1 -> 0 -> enqueue: no data-page write, no fault
  }

  // Restore writability before draining/teardown (reclaim runs ~T and munmap).
  source.protect(PROT_READ | PROT_WRITE);
  queue.drain();
  REQUIRE(store.slots_live() == 0);
}

#endif // __linux__

} // namespace
