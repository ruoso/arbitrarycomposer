// Seeded schedule-perturbation stress for the deferred reclamation queue
// (design doc 16:66-73). Multiple RT-role producers release (lock-free enqueue)
// into a single drainer, with yields fired on random bits from a per-thread
// seeded `std::mt19937`; the drain must run every subtree's destructor cascade
// exactly once and return the slots to the free pools. This is the seeded
// reclaim stress `pool.reclamation` (`reclamation.t.cpp:248`) and
// `pool.free_pools`/`pool.refcounts_in_store` parked here; the smokes those left
// behind ran under a benign schedule (no seeded yields).
//
// Single-drainer discipline (doc 15:137-143, reclamation.hpp:57-62): many
// producers may release/enqueue from any thread, but exactly one thread drains
// at a time. Here the writer/main thread is the sole drainer (it loops
// `queue.drain()` while the producers release, then drains once more), and
// `create` stays writer-only (every slot is built up front on this thread).
//
// enforces: 15-memory-model#release-enqueues-never-destroys-inline
// enforces: 15-memory-model#deferred-cascade-reclaims-whole-subtree
// enforces: 15-memory-model#thread-local-free-pools-spill-to-global
// enforces: 15-memory-model#one-count-column-per-size-class

#include <arbc/pool/reclamation.hpp>
#include <arbc/pool/refs.hpp>
#include <arbc/pool/slot_store.hpp>

#include <catch2/catch_test_macros.hpp>

#include "support/schedule_perturb.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

// A reclamation-cascade record: holds one counted child edge (released on
// destruction, so a chain reclaims as a whole subtree) and bumps a destruction
// counter. `~Tracked` runs ONLY on the single drainer (here the main thread),
// so the plain `int` counter needs no atomic -- it is written and read on one
// thread, ordered by the producers' join and the final drain.
struct Tracked {
  arbc::RefStore<Tracked>* store{nullptr};
  arbc::SlotRef<Tracked> child{};
  bool has_child{false};
  int* destructions{nullptr};
  ~Tracked() {
    if (has_child) {
      store->release(child);
    }
    if (destructions != nullptr) {
      ++*destructions;
    }
  }
};

// Build a `depth`-node retained chain; the returned `Ref` owns the root. Each
// node points at the shared destruction counter.
arbc::Ref<Tracked> build_chain(arbc::RefStore<Tracked>& store, int depth, int* destructions) {
  arbc::Ref<Tracked> current = *store.create();
  current->store = &store;
  current->destructions = destructions;
  for (int i = 1; i < depth; ++i) {
    arbc::Ref<Tracked> parent = *store.create();
    parent->store = &store;
    parent->destructions = destructions;
    parent->child = current.slot();
    REQUIRE(store.retain(parent->child).has_value());
    parent->has_child = true;
    current = std::move(parent);
  }
  return current;
}

void run_reclamation_stress(std::uint32_t seed_begin, std::uint32_t seed_end, int producer_count,
                            int chains_per_producer, int chain_depth) {
  for (std::uint32_t seed = seed_begin; seed < seed_end; ++seed) {
    INFO("seed = " << seed);

    arbc::Arena arena;
    arbc::RefStore<Tracked> store(arena);
    arbc::DeferredReclaimSink<Tracked> sink(store);
    arbc::ReclamationQueue queue;
    queue.register_store(store, sink);

    int destructions = 0;
    const int nodes_per_chain = chain_depth;
    const int total_chains = producer_count * chains_per_producer;
    const int total_nodes = total_chains * nodes_per_chain;

    // The writer creates every chain up front (create is writer-only) and hands
    // each producer a disjoint block of standalone-counted chain roots.
    std::vector<std::vector<arbc::SlotRef<Tracked>>> blocks(producer_count);
    for (int p = 0; p < producer_count; ++p) {
      for (int c = 0; c < chains_per_producer; ++c) {
        arbc::Ref<Tracked> root = build_chain(store, chain_depth, &destructions);
        arbc::SlotRef<Tracked> s = root.slot();
        REQUIRE(store.retain(s).has_value()); // survive the owning Ref's drop
        blocks[p].push_back(s);
      }
    }
    const std::size_t built = store.slots_live();
    REQUIRE(built == static_cast<std::size_t>(total_nodes));

    // Deterministic litmus for release-enqueues-never-destroys-inline: releasing
    // one chain root only enqueues -- no destructor has run and every slot is
    // still live -- until the drain pops it and cascades.
    {
      const int before = destructions;
      store.release(blocks[0].back());
      blocks[0].pop_back();
      REQUIRE(destructions == before);               // ~T has NOT run yet
      REQUIRE(store.slots_live() == built);          // the slot is still live
      queue.drain();                                 // now the cascade runs
      REQUIRE(destructions == before + chain_depth); // whole subtree, once each
      REQUIRE(store.slots_live() == built - static_cast<std::size_t>(chain_depth));
    }

    std::atomic<bool> go{false};
    std::atomic<int> done{0};

    std::vector<std::thread> producers;
    for (int p = 0; p < producer_count; ++p) {
      producers.emplace_back([&, p] {
        arbc::test::Perturber perturb(arbc::test::derive_seed(seed, static_cast<std::uint32_t>(p)));
        while (!go.load(std::memory_order_acquire)) {
        }
        for (arbc::SlotRef<Tracked> s : blocks[p]) {
          store.release(s); // count 1 -> 0 -> deferred enqueue (RT-safe path)
          perturb.maybe_yield();
        }
        done.fetch_add(1, std::memory_order_release);
      });
    }

    go.store(true, std::memory_order_release);

    // Single drainer: the main thread sweeps to quiescence concurrently with the
    // producers, then once more after they finish to catch the last enqueues.
    // Producers release/enqueue from any thread; only this thread drains.
    arbc::test::Perturber dperturb(arbc::test::derive_seed(seed, 0xD1A12U));
    while (done.load(std::memory_order_acquire) < producer_count) {
      queue.drain();
      dperturb.maybe_yield();
    }
    queue.drain();

    for (std::thread& th : producers) {
      th.join();
    }
    queue.drain(); // final sweep after the join's happens-before

    // Outcome only, no timing: every node's destructor fired exactly once (the
    // whole-subtree cascade completed for every chain) and every slot returned
    // to the free pools -- none lost, none double-freed.
    REQUIRE(destructions == total_nodes);
    REQUIRE(store.slots_live() == 0);
    // The released slots round-tripped through the shared GLOBAL free pool at
    // least once (the per-thread pools spilled a batch), witnessing the
    // thread-local-free-pools-spill-to-global path under a cross-thread churn.
    REQUIRE((store.store().spill_count() > 0 || store.store().refill_count() > 0));
  }
}

// Two distinct record types with identical layout so the arena maps them onto
// ONE size-class store (and therefore one shared per-slot refcount column).
struct CellA {
  std::uint64_t value;
};
struct CellB {
  std::uint64_t value;
};
static_assert(sizeof(CellA) == sizeof(CellB), "CellA and CellB must share a size class");
static_assert(alignof(CellA) == alignof(CellB), "CellA and CellB must share a size class");

void run_shared_column_stress(std::uint32_t seed_begin, std::uint32_t seed_end, int iterations) {
  for (std::uint32_t seed = seed_begin; seed < seed_end; ++seed) {
    INFO("seed = " << seed);

    arbc::Arena arena;
    arbc::RefStore<CellA> a(arena);
    arbc::RefStore<CellB> b(arena);
    REQUIRE(&a.store() == &b.store()); // one store => one shared count column
    REQUIRE(arena.store_count() == 1);

    arbc::Ref<CellA> ra = *a.create(CellA{0xA1A1A1A1u});
    arbc::Ref<CellB> rb = *b.create(CellB{0xB2B2B2B2u});
    const arbc::SlotRef<CellA> sa = ra.slot();
    const arbc::SlotRef<CellB> sb = rb.slot();
    REQUIRE(sa.index() != sb.index()); // disjoint slots on the shared store
    // Both typed views name the very same atomic count cell for a given slot.
    REQUIRE(&a.store().count_ref(sa.index()) == &b.store().count_ref(sa.index()));

    std::atomic<bool> go{false};
    std::atomic<bool> bad{false};

    // Each thread churns retain/resolve/release on its own view's slot under the
    // seed; the shared count column serves both concurrently. Counts must return
    // to the base of 1 (the owning `Ref` each thread never drops).
    std::thread ta([&] {
      arbc::test::Perturber perturb(arbc::test::derive_seed(seed, 0));
      while (!go.load(std::memory_order_acquire)) {
      }
      for (int i = 0; i < iterations; ++i) {
        auto pinned = a.resolve(sa);
        if (!pinned.has_value() || (*pinned)->value != 0xA1A1A1A1u) {
          bad.store(true, std::memory_order_relaxed);
        }
        perturb.maybe_yield();
      }
    });
    std::thread tb([&] {
      arbc::test::Perturber perturb(arbc::test::derive_seed(seed, 1));
      while (!go.load(std::memory_order_acquire)) {
      }
      for (int i = 0; i < iterations; ++i) {
        auto pinned = b.resolve(sb);
        if (!pinned.has_value() || (*pinned)->value != 0xB2B2B2B2u) {
          bad.store(true, std::memory_order_relaxed);
        }
        perturb.maybe_yield();
      }
    });

    go.store(true, std::memory_order_release);
    ta.join();
    tb.join();

    REQUIRE_FALSE(bad.load());
    REQUIRE(a.count(sa) == 1); // back to the base count; nothing reclaimed
    REQUIRE(b.count(sb) == 1);
  }
}

} // namespace

TEST_CASE("stress: seeded multi-producer enqueue while a single thread drains the cascade") {
  run_reclamation_stress(/*seed_begin=*/0, /*seed_end=*/5, /*producer_count=*/8,
                         /*chains_per_producer=*/50, /*chain_depth=*/4);
}

TEST_CASE("stress: two typed views share one count column under seeded churn") {
  run_shared_column_stress(/*seed_begin=*/0, /*seed_end=*/5, /*iterations=*/2000);
}

TEST_CASE("stress: reclamation-queue seeded schedule sweep (long-form)", "[.nightly]") {
  run_reclamation_stress(/*seed_begin=*/0, /*seed_end=*/32, /*producer_count=*/8,
                         /*chains_per_producer=*/400, /*chain_depth=*/6);
}

TEST_CASE("stress: shared count column seeded sweep (long-form)", "[.nightly]") {
  run_shared_column_stress(/*seed_begin=*/0, /*seed_end=*/32, /*iterations=*/20000);
}
