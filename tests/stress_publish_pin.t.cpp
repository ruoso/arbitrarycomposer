// Seeded schedule-perturbation stress for the publish/pin protocol (design doc
// 16:66-73). A background pinner loop (pin -> peek-traverse -> unpin) races a
// committing writer and a `HousekeepingThread` drainer, with yields fired on
// random bits from a per-thread seeded `std::mt19937`. This is the tier-6
// concurrency test every publish/pin predecessor parked here
// (`persistent_state.t.cpp:170` parks the seeded stress; the smoke there runs a
// writer-thread `model.drain()` under a benign schedule only).
//
// The pinnable substrate is the publish/pin protocol doc 14/15 describe, minus
// the HAMT weight: a `Version` is a heap control object owning one `Ref` into an
// arena chain -- exactly `model::DocRoot`'s shape (model.hpp:36-89, "Document
// holds the current DocState in an atomic shared pointer; readers pin a version
// by acquiring a shared_ptr<const DocRoot>, granting the version handle and a
// transitive count in one lock-free step"). While any pin is held the whole
// chain is memory-live, so a `peek` traversal is safe and touches no refcount
// page. Building the substrate at the pool level (rather than on `Model`, whose
// arena/queue are private and not yet `HousekeepingThread`-wired -- see
// parking-lot 2026-07-05 "Document->slab-arena rewire") is what lets a real
// `HousekeepingThread` be the single background drainer and lets the test assert
// on `Arena::total_slots_live()`.
//
// enforces: 14-data-model-and-editing#pinned-version-never-observes-later-edit
// enforces: 15-memory-model#const-ref-traversal-touches-no-refcount-page
// enforces: 15-memory-model#housekeeping-thread-single-drainer

#include <arbc/pool/chunk_source.hpp>
#include <arbc/pool/reclamation.hpp>
#include <arbc/pool/refs.hpp>
#include <arbc/pool/slot_store.hpp>
#include <arbc/runtime/housekeeping_thread.hpp>

#include <catch2/catch_test_macros.hpp>

#include "support/schedule_perturb.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <sys/mman.h>
#endif

namespace {

// One version's chain node: the retained-SlotRef record the reclamation cascade
// walks (same shape as the runtime fixtures' `Rec`), carrying the `payload` a
// pinner reads back to witness that a pinned version never observes a later
// edit. Its destructor releases its counted child edge, so dropping the chain
// root and draining cascades the whole subtree.
struct VerNode {
  arbc::RefStore<VerNode>* store{nullptr};
  arbc::SlotRef<VerNode> child{};
  bool has_child{false};
  std::uint32_t payload{0};
  ~VerNode() {
    if (has_child) {
      store->release(child);
    }
  }
};

// The pinnable version root -- `model::DocRoot`'s heap control object without
// the HAMT: one owning `Ref` into the chain (which transitively pins the whole
// subtree memory-live) plus the revision and the payload every node in this
// version carries.
struct Version {
  arbc::Ref<VerNode> root;
  std::uint32_t revision{0};
  std::uint32_t payload{0};
};
using VersionPtr = std::shared_ptr<const Version>;

// Build a `depth`-node chain whose every node carries `payload`. WRITER-ONLY
// (`create` allocates slots). Each parent takes a standalone count on its child
// so the chain stays live until reclaimed.
arbc::Ref<VerNode> build_chain(arbc::RefStore<VerNode>& store, int depth, std::uint32_t payload) {
  arbc::Ref<VerNode> current = *store.create();
  current->store = &store;
  current->payload = payload;
  for (int i = 1; i < depth; ++i) {
    arbc::Ref<VerNode> parent = *store.create();
    parent->store = &store;
    parent->payload = payload;
    parent->child = current.slot();
    REQUIRE(store.retain(parent->child).has_value());
    parent->has_child = true;
    current = std::move(parent);
  }
  return current;
}

// Publish a fresh version (writer thread). `make_shared` gives the version its
// control block; the `Ref` root moves in.
VersionPtr make_version(arbc::RefStore<VerNode>& store, int depth, std::uint32_t revision) {
  return std::make_shared<const Version>(
      Version{build_chain(store, depth, revision), revision, revision});
}

// A tick period short enough that the background thread is an active
// low-priority drainer (it never appears in an assertion, doc 16:54-62).
constexpr std::chrono::microseconds kActiveTick{50};

// Seed salt for the writer thread, disjoint from the small pinner salts (0..N).
constexpr std::uint32_t kWriterSalt = 0x5A5AU;

void run_publish_pin_stress(std::uint32_t seed_begin, std::uint32_t seed_end, int publishes,
                            int chain_depth, int pinner_count) {
  for (std::uint32_t seed = seed_begin; seed < seed_end; ++seed) {
    INFO("seed = " << seed);

    arbc::Arena arena;
    arbc::RefStore<VerNode> store(arena);
    arbc::DeferredReclaimSink<VerNode> sink(store);
    arbc::ReclamationQueue queue;
    queue.register_store(store, sink);

    const std::size_t baseline = arena.total_slots_live();

    // The writer's `after_commit` names a stable root slot (mirrors the
    // housekeeping stress fixture); it is dropped last, at teardown.
    arbc::Ref<VerNode> sentinel = *store.create();
    sentinel->store = &store;

    // Publish the initial version so a pinner always loads a live root.
    std::atomic<VersionPtr> current{make_version(store, chain_depth, 0)};

    std::atomic<bool> go{false};
    std::atomic<bool> stop{false};
    std::atomic<bool> bad{false};

    std::vector<std::thread> pinners;
    for (int t = 0; t < pinner_count; ++t) {
      pinners.emplace_back([&, t] {
        arbc::test::Perturber perturb(arbc::test::derive_seed(seed, static_cast<std::uint32_t>(t)));
        while (!go.load(std::memory_order_acquire)) {
        }
        while (!stop.load(std::memory_order_acquire)) {
          // Pin: acquire a shared_ptr<const Version> -- the version handle and a
          // transitive count in one lock-free step (doc 14:27-32).
          const VersionPtr pin = current.load(std::memory_order_acquire);
          perturb.maybe_yield();
          const std::uint32_t expected = pin->payload;

          // Refcount-free peek traversal of the pinned chain: the pin keeps the
          // whole subtree memory-live, so following in-record `SlotRef` edges
          // through `peek` touches no count page. A pinned version is immutable,
          // so every node still carries the payload it was published with --
          // even as the writer publishes newer versions concurrently. A torn
          // read or a slot reused out from under the pin would break it.
          const VerNode* node = store.peek(pin->root.slot());
          while (node != nullptr) {
            if (node->payload != expected) {
              bad.store(true, std::memory_order_relaxed);
            }
            perturb.maybe_yield();
            if (!node->has_child) {
              break;
            }
            node = store.peek(node->child);
          }
          // Unpin: dropping `pin` here only ENQUEUES if this was the last
          // reference to a now-superseded version (RT-safe, no drain).
        }
      });
    }

    {
      arbc::HousekeepingThreadConfig tc;
      tc.tick_period = kActiveTick;
      arbc::HousekeepingThread hkt(queue, nullptr, &arena, arbc::HousekeepingConfig{},
                                   std::move(tc));

      go.store(true, std::memory_order_release);
      arbc::test::Perturber wperturb(arbc::test::derive_seed(seed, kWriterSalt));
      for (int i = 1; i <= publishes; ++i) {
        // Publish: atomic swap of the current-version handle. The store drops
        // the superseded version; its last reference (here or in a pinner)
        // enqueues the old chain for deferred reclamation.
        current.store(make_version(store, chain_depth, static_cast<std::uint32_t>(i)),
                      std::memory_order_release);
        // The writer's between-transaction drain, serialized with the
        // background thread's tick through the wrapper mutex -> exactly one
        // drainer at a time (doc 15:129-136).
        REQUIRE(hkt.after_commit(sentinel.index()).has_value());
        wperturb.maybe_yield();
      }

      stop.store(true, std::memory_order_release);
      for (std::thread& th : pinners) {
        th.join();
      }

      current.store(VersionPtr{}, std::memory_order_release); // drop the last version
      sentinel = arbc::Ref<VerNode>{};                        // drop the after_commit root
    } // hkt destructor: request_stop + join + a final drain_and_quiesce

    // Outcome only, no timing: every version's chain was reclaimed exactly once
    // and the arena is back to its no-garbage baseline; no torn read or
    // use-after-free was ever observed under any interleaving.
    REQUIRE_FALSE(bad.load());
    REQUIRE(arena.total_slots_live() == baseline);
    REQUIRE(store.slots_live() == baseline);
  }
}

#if defined(__linux__)

// A ChunkSource that mmaps page-granular anonymous DATA chunks and can flip their
// protection, so the concurrent claim test can freeze the data pages read-only
// and prove that a producer's pin churn (count-only writes to the anonymous
// parallel column) never faults them, even under concurrent traversal. This is
// the concurrent extension of refs.t.cpp:311's single-threaded pattern.
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

#endif // __linux__

} // namespace

TEST_CASE("stress: seeded pin/peek/unpin races a committing writer and a housekeeping drainer") {
  // Per-push brief: a small seed range and modest op counts keep the lane fast
  // (doc 16:101-103, short-form). The nightly case below sweeps wide.
  run_publish_pin_stress(/*seed_begin=*/0, /*seed_end=*/5, /*publishes=*/150,
                         /*chain_depth=*/5, /*pinner_count=*/2);
}

TEST_CASE("stress: publish/pin seeded schedule sweep (long-form)", "[.nightly]") {
  // Nightly long-form: a wide seed sweep with deeper chains and more pinners
  // (doc 16:104-105). Same body as the brief case, parameterized by range.
  run_publish_pin_stress(/*seed_begin=*/0, /*seed_end=*/48, /*publishes=*/1500,
                         /*chain_depth=*/8, /*pinner_count=*/3);
}

#if defined(__linux__)

// enforces: 15-memory-model#interference-free-concurrent-pin
//
// The concurrent composition of the two single-threaded page-cleanliness claims
// (#const-ref-traversal-touches-no-refcount-page,
// #refcounts-outside-data-pages): a producer thread churns version pins
// (retain/release) on the shared interior nodes of a published version while a
// consumer thread `peek`-traverses that same version, with the DATA chunks
// mprotected read-only. In `arbc::pool` the producer's count-only writes land in
// the anonymous parallel column, disjoint from the read-only data pages, so
// neither thread faults and the consumer observes no torn value under real
// contention -- the two threads' page-write and page-read sets are provably
// disjoint. The concurrency, the shared read/write set, and the no-torn-read are
// the new facts neither single-threaded claim individually witnesses. Runs clean
// on the tsan lane. (mprotect faults would SIGSEGV, so completion is itself the
// no-fault witness; the atomics carry the torn-read verdict.)
TEST_CASE(
    "interference-free concurrent pin: pin churn and peek traversal touch disjoint data pages") {
  MmapRecordingSource source;
  arbc::Arena arena(source);
  arbc::RefStore<VerNode> store(arena);

  constexpr int chain_depth = 12;
  constexpr std::uint32_t payload = 0x00C0FFEEU;
  arbc::Ref<VerNode> root = build_chain(store, chain_depth, payload);

  // The shared interior nodes: every node carrying a child edge, reached by
  // following in-record SlotRef edges from the root. The producer pins THESE (the
  // subtree a pinned reader is reading, doc 15:58-62), not merely the root.
  std::vector<arbc::SlotRef<VerNode>> interior;
  {
    arbc::SlotRef<VerNode> s = root.slot();
    for (const VerNode* n = store.peek(s); n->has_child; n = store.peek(s)) {
      interior.push_back(s);
      s = n->child;
    }
  }
  REQUIRE_FALSE(interior.empty());

  // Freeze the DATA pages read-only. From here the producer's count-only writes
  // must never fault a data page, and the consumer reads only immutable data.
  source.protect(PROT_READ);

  std::atomic<bool> go{false};
  std::atomic<bool> stop{false};
  std::atomic<bool> bad{false};
  std::atomic<bool> torn{false};

  constexpr int producer_rounds = 4000;
  std::thread producer([&] {
    while (!go.load(std::memory_order_acquire)) {
    }
    for (int i = 0; i < producer_rounds; ++i) {
      for (arbc::SlotRef<VerNode> s : interior) {
        // retain/release write only the anonymous count column; the interior
        // node is pinned by the record edge above it, so a count never reaches
        // zero and no reclamation (which would write a data page) is triggered.
        if (!store.retain(s).has_value()) {
          bad.store(true, std::memory_order_relaxed);
        } else {
          store.release(s);
        }
      }
    }
    stop.store(true, std::memory_order_release);
  });

  go.store(true, std::memory_order_release);
  // Consumer (main thread): peek-traverse the pinned version while the producer
  // churns pins. A pinned version is immutable, so every node still carries its
  // published payload; a torn read would mean the pin traffic dirtied the data.
  while (!stop.load(std::memory_order_acquire)) {
    const VerNode* node = store.peek(root.slot());
    while (node != nullptr) {
      if (node->payload != payload) {
        torn.store(true, std::memory_order_relaxed);
      }
      if (!node->has_child) {
        break;
      }
      node = store.peek(node->child);
    }
  }
  producer.join();

  REQUIRE_FALSE(bad.load());  // no overflow on the checked pin path
  REQUIRE_FALSE(torn.load()); // consumer saw no torn value under real contention

  // Restore writability so the immediate-sink reclaim cascade (which writes data
  // pages) and munmap are safe at teardown.
  source.protect(PROT_READ | PROT_WRITE);
  root = arbc::Ref<VerNode>{}; // drop the version -> immediate cascade reclaim
  REQUIRE(store.slots_live() == 0);
}

#endif // __linux__
