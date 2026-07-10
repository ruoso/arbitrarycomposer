// Bench-smoke test (design doc 16 tier 4/9).
//
// Drives every benchmark workload body (src/pool/bench/pool_bench_workloads.hpp)
// ONCE, at a minimal size, under the normal dev/asan test build -- NOT through
// the Google Benchmark runner. This gives the benchmark bodies diff coverage and
// rot protection, and asserts the BEHAVIORAL facts the benchmarks are shaped
// around, without any wall-clock assertion entering the merge path
// (doc 16:54-62, 225-226: counters gate, benchmarks trend).
//
// It also lands the new machine-checked witness for the honest baseline's
// premise: a const-ref/peek traversal touches no refcount page. With the
// refcount table frozen read-only, a full peek traversal must not fault --
// whereas the by-value shared_ptr traversal the old numbers used would.

#include <arbc/pool/reclamation.hpp>
#include <arbc/pool/refs.hpp>
#include <arbc/pool/slot_store.hpp>

#include <catch2/catch_test_macros.hpp>

#include "../bench/pool_bench_workloads.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <sys/mman.h>
#endif

using namespace arbc::pool_bench;

namespace {

// Sum of node values 0..n-1 for a full binary tree of `depth` levels, built by
// the pre-order `counter++` labeling the producers use.
std::uint64_t full_tree_sum(int depth) {
  const std::uint64_t nodes = (std::uint64_t{1} << depth) - 1;
  return nodes * (nodes - 1) / 2;
}

} // namespace

TEST_CASE("version production and traversal build value-identical managed/shared topologies") {
  constexpr int depth = 6; // 63 nodes
  const std::uint64_t expected = full_tree_sum(depth);

  arbc::Arena arena;
  arbc::RefStore<BenchNode> store(arena);
  arbc::DeferredReclaimSink<BenchNode> sink(store);
  arbc::ReclamationQueue queue;
  queue.register_store(store, sink);

  std::uint64_t managed_counter = 0;
  arbc::Ref<BenchNode> managed_root = build_managed_tree(store, depth, managed_counter);

  std::uint64_t shared_counter = 0;
  std::shared_ptr<SharedNode> shared_root = build_shared_tree(depth, shared_counter);

  // The producers built the same shape; every consumer walks it to the same sum.
  REQUIRE(traverse_managed_peek(store, managed_root.get()) == expected);
  REQUIRE(traverse_shared_constref(shared_root) == expected);
  REQUIRE(traverse_shared_byvalue(shared_root) == expected);

  // Drop the managed version; the deferred cascade reclaims it before teardown.
  managed_root = arbc::Ref<BenchNode>{};
  queue.drain();
  REQUIRE(store.slots_live() == 0);
}

TEST_CASE("warm allocation microbench body runs and yields the constructed value") {
  arbc::Arena arena;
  arbc::RefStore<std::uint64_t> store(arena);
  warm_store(store, 32);
  REQUIRE(store.slots_live() == 0); // warming left the free list, not live slots
  REQUIRE(warm_create_once(store, 7) == 7);
  REQUIRE(store.slots_live() == 0); // the warm create+drop returned to zero
}

// enforces: 15-memory-model#slots-recycle-in-place
TEST_CASE("the churn cycle recycles in place: back to baseline, flat chunk publication") {
  arbc::Arena arena;
  arbc::RefStore<std::uint64_t> store(arena);
  arbc::DeferredReclaimSink<std::uint64_t> sink(store);
  arbc::ReclamationQueue queue;
  queue.register_store(store, sink);

  const std::size_t baseline = arena.total_slots_live();

  churn_cycle(store, queue, 256);
  REQUIRE(arena.total_slots_live() == baseline); // structural leak check
  const std::size_t chunks_after_first = store.reclaim_chunks_published();

  // A second cycle reuses the freed holes: no high-water growth, so no new
  // parallel chunk is published -- fragmentation-free reuse.
  churn_cycle(store, queue, 256);
  REQUIRE(arena.total_slots_live() == baseline);
  REQUIRE(store.reclaim_chunks_published() == chunks_after_first);
}

// enforces: 15-memory-model#deferred-cascade-reclaims-whole-subtree
TEST_CASE("dropping a deep managed subtree and draining once runs every ~T exactly once") {
  constexpr int depth = 7; // 127 nodes
  const std::size_t nodes = (std::size_t{1} << depth) - 1;

  arbc::Arena arena;
  arbc::RefStore<BenchNode> store(arena);
  arbc::DeferredReclaimSink<BenchNode> sink(store);
  arbc::ReclamationQueue queue;
  queue.register_store(store, sink);

  const std::size_t baseline = store.slots_live();
  int destructions = 0;
  std::uint64_t counter = 0;
  arbc::Ref<BenchNode> root = build_managed_tree(store, depth, counter, &destructions);
  REQUIRE(store.slots_live() == baseline + nodes);

  root = arbc::Ref<BenchNode>{}; // drop the root -> the whole subtree enqueues
  REQUIRE(destructions == 0);    // deferred: nothing destroyed inline

  queue.drain();
  REQUIRE(destructions == static_cast<int>(nodes)); // each ~BenchNode ran once
  REQUIRE(store.slots_live() == baseline);          // leak check: back to baseline
}

// Drives the concurrent-pin workload body (a background producer churns version
// pins on the shared interior nodes while the consumer traverses that version)
// for a bounded op count on both substrates, asserting the BEHAVIORAL facts the
// BM_ConcurrentPin_* benches are shaped around -- no torn read, arena back to
// baseline -- with no timing. Exercises the cross-thread release path of
// 15-memory-model#thread-local-free-pools-spill-to-global (the producer's
// retain/release run off the writer thread) and, on teardown+drain,
// 15-memory-model#slots-recycle-in-place (both existing claims -- no new claim).
TEST_CASE(
    "concurrent pin churn: the consumer sees no torn value and the arena returns to baseline") {
  constexpr int depth = 8; // 255 nodes
  const std::uint64_t expected = full_tree_sum(depth);

  SECTION("managed: peek traversal under a pin-churning producer") {
    arbc::Arena arena;
    arbc::RefStore<BenchNode> store(arena);
    arbc::DeferredReclaimSink<BenchNode> sink(store);
    arbc::ReclamationQueue queue;
    queue.register_store(store, sink);

    const std::size_t baseline = arena.total_slots_live();

    std::uint64_t counter = 0;
    arbc::Ref<BenchNode> root = build_managed_tree(store, depth, counter);

    std::vector<arbc::SlotRef<BenchNode>> interior;
    collect_interior_slots_managed(store, root.slot(), interior);
    REQUIRE_FALSE(interior.empty());

    std::atomic<bool> stop{false};
    std::atomic<bool> torn{false};
    std::thread producer([&] {
      while (!stop.load(std::memory_order_acquire)) {
        churn_pins_managed(store, interior, 32);
      }
    });

    for (int i = 0; i < 200; ++i) {
      if (traverse_managed_peek(store, root.get()) != expected) {
        torn.store(true, std::memory_order_relaxed);
      }
    }
    stop.store(true, std::memory_order_release);
    producer.join();

    REQUIRE_FALSE(torn.load()); // the pin churn never dirtied the read data

    root = arbc::Ref<BenchNode>{}; // drop the version -> deferred cascade
    queue.drain();
    REQUIRE(arena.total_slots_live() == baseline); // back to pre-run baseline
  }

  SECTION("make_shared: const& traversal under a pin-churning producer") {
    std::uint64_t counter = 0;
    std::shared_ptr<SharedNode> root = build_shared_tree(depth, counter);

    std::vector<std::shared_ptr<SharedNode>> interior;
    collect_interior_shared(root, interior);
    REQUIRE_FALSE(interior.empty());

    std::atomic<bool> stop{false};
    std::atomic<bool> torn{false};
    std::thread producer([&] {
      while (!stop.load(std::memory_order_acquire)) {
        churn_pins_shared(interior, 32);
      }
    });

    for (int i = 0; i < 200; ++i) {
      if (traverse_shared_constref(root) != expected) {
        torn.store(true, std::memory_order_relaxed);
      }
    }
    stop.store(true, std::memory_order_release);
    producer.join();

    REQUIRE_FALSE(torn.load());

    interior.clear();
    root.reset(); // recursive teardown returns every node
  }
}

#if defined(__linux__)

// A page-aligned, mprotectable backing for the REFCOUNT table (the count-table
// analog of refs.t.cpp's data-side MmapRecordingSource). Installed on a RefStore
// so the test can freeze the count table read-only and prove a peek traversal
// never writes it.
class MmapRefcountBacking final : public arbc::RefcountTableBacking {
public:
  std::atomic<std::uint32_t>* allocate(std::size_t count) override {
    const std::size_t bytes = count * sizeof(std::atomic<std::uint32_t>);
    const std::size_t rounded = (bytes + 4095) & ~std::size_t{4095};
    void* base =
        ::mmap(nullptr, rounded, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    REQUIRE(base != MAP_FAILED);
    auto* arr = static_cast<std::atomic<std::uint32_t>*>(base);
    for (std::size_t i = 0; i < count; ++i) {
      ::new (&arr[i]) std::atomic<std::uint32_t>(0); // zeroed pages, made explicit
    }
    d_spans.push_back(Span{base, rounded});
    return arr;
  }

  void deallocate(std::atomic<std::uint32_t>* base, std::size_t /*count*/) noexcept override {
    for (Span& span : d_spans) {
      if (span.base == static_cast<void*>(base)) {
        ::munmap(span.base, span.size);
        span.base = nullptr;
        return;
      }
    }
  }

  // Flip every published refcount page to `prot`.
  void protect(int prot) {
    for (const Span& span : d_spans) {
      if (span.base != nullptr) {
        REQUIRE(::mprotect(span.base, span.size, prot) == 0);
      }
    }
  }

private:
  struct Span {
    void* base;
    std::size_t size;
  };
  std::vector<Span> d_spans;
};

// enforces: 15-memory-model#const-ref-traversal-touches-no-refcount-page
TEST_CASE("a peek traversal of a pinned version never touches a refcount page") {
  constexpr int depth = 8; // 255 nodes, all within one refcount chunk
  const std::uint64_t expected = full_tree_sum(depth);

  MmapRefcountBacking backing; // declared before the store: outlives it
  arbc::Arena arena;
  arbc::RefStore<BenchNode> store(arena, &backing);

  std::uint64_t counter = 0;
  arbc::Ref<BenchNode> root = build_managed_tree(store, depth, counter);

  // Freeze the refcount table. From here a traversal that touched any count --
  // as a by-value shared_ptr visit would (bump/drop per node) -- would fault.
  backing.protect(PROT_READ);

  // A full peek traversal follows in-record SlotRef edges straight to the data,
  // touching no count page: it completes without faulting and reads the whole
  // pinned version.
  const std::uint64_t sum = traverse_managed_peek(store, root.get());
  REQUIRE(sum == expected);

  // Restore writability so reclamation (which decrements counts) and munmap are
  // safe at teardown.
  backing.protect(PROT_READ | PROT_WRITE);
  root = arbc::Ref<BenchNode>{}; // drop -> immediate cascade reclaim
  REQUIRE(store.slots_live() == 0);
}

#endif // __linux__
