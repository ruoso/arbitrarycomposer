// Litmus tests for the arena growth handshake (design doc 16:66-73). Concurrent
// readers resolve a stable index set through the store's acquire loads while the
// writer keeps growing the `SlabDirectory`, under seeded schedule perturbation.
// Every resolved pointer must stay valid and unchanged across arbitrary growth
// (no torn read, no use-after-free), and TSan must report no data race on the
// capacity-growth handshake.
//
// This is doc 15 caveat 4's charter: the "racy capacity-growth handshake (works
// in practice, wants a clean acquire/release protocol)" that `pool.arena_core`
// hardened to an acquire/release resolve; this litmus is what pins it
// TSan-clean. It generalizes the benign-schedule concurrent-resolve smoke at
// `pool.t.cpp:192` by injecting seeded yields to widen the race window.
//
// Reads are any-thread; `allocate` (arena growth: chunk publish, capacity
// accounting) is WRITER-ONLY, so all growth stays on the main thread here.
//
// enforces: 15-memory-model#chunk-growth-preserves-addresses
// enforces: 15-memory-model#slots-recycle-in-place

#include <arbc/pool/slot_store.hpp>
#include <arbc/pool/typed_store.hpp>

#include "support/schedule_perturb.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

namespace {

void run_arena_growth_litmus(std::uint32_t seed_begin, std::uint32_t seed_end, int reader_count,
                             int grow_ops, int tracked_count) {
  for (std::uint32_t seed = seed_begin; seed < seed_end; ++seed) {
    INFO("seed = " << seed);

    arbc::Arena arena;
    // chunk_bits == 1 => two slots per chunk, so the directory grows constantly
    // under the writer's allocate loop -- maximal pressure on the handshake.
    arbc::SlotStore& store =
        arena.store_for(sizeof(std::uint32_t), alignof(std::uint32_t), /*chunk_bits=*/1);

    // A stable set: each slot stores its own index as a sentinel value, written
    // BEFORE any reader starts. Capture each slot's address to prove growth
    // never moves an already-published slot.
    std::vector<arbc::SlotIndex> tracked;
    std::vector<const void*> addrs;
    for (int i = 0; i < tracked_count; ++i) {
      const auto slot = store.allocate();
      REQUIRE(slot.has_value());
      *static_cast<std::uint32_t*>(store.resolve(*slot)) = static_cast<std::uint32_t>(*slot);
      tracked.push_back(*slot);
      addrs.push_back(store.resolve(*slot));
    }

    std::atomic<bool> go{false};
    std::atomic<bool> stop{false};
    std::atomic<bool> mismatch{false};

    std::vector<std::thread> readers;
    for (int r = 0; r < reader_count; ++r) {
      readers.emplace_back([&, r] {
        arbc::test::Perturber perturb(
            arbc::test::derive_seed(seed, static_cast<std::uint32_t>(r)));
        while (!go.load(std::memory_order_acquire)) {
        }
        while (!stop.load(std::memory_order_acquire)) {
          for (arbc::SlotIndex index : tracked) {
            // Resolve races the writer's directory growth. A torn (half-published)
            // directory read or a moved slot would corrupt the sentinel.
            const auto* p = static_cast<const std::uint32_t*>(store.resolve(index));
            if (*p != index) {
              mismatch.store(true, std::memory_order_relaxed);
            }
            perturb.maybe_yield();
          }
        }
      });
    }

    go.store(true, std::memory_order_release);
    // Writer grows the directory well past the tracked set while readers resolve.
    arbc::test::Perturber wperturb(arbc::test::derive_seed(seed, 0x770000u));
    for (int i = 0; i < grow_ops; ++i) {
      REQUIRE(store.allocate().has_value());
      wperturb.maybe_yield();
    }
    stop.store(true, std::memory_order_release);
    for (std::thread& reader : readers) {
      reader.join();
    }

    // No reader ever saw a torn/moved slot, and every tracked slot kept its
    // exact address and value across all the growth (chunk-growth-preserves-
    // addresses).
    REQUIRE_FALSE(mismatch.load());
    for (int i = 0; i < tracked_count; ++i) {
      REQUIRE(store.resolve(tracked[i]) == addrs[i]);
      REQUIRE(*static_cast<const std::uint32_t*>(addrs[i]) == tracked[i]);
    }
  }
}

} // namespace

TEST_CASE("litmus: concurrent seeded resolve stays valid across writer-driven arena growth") {
  run_arena_growth_litmus(/*seed_begin=*/0, /*seed_end=*/5, /*reader_count=*/4,
                          /*grow_ops=*/8000, /*tracked_count=*/256);
}

TEST_CASE("litmus: a released slot is the next allocation's perfect hole") {
  // Deterministic slots-recycle-in-place witness: fixed slabs make a released
  // slot the exact hole the next same-class allocation fills (doc 15).
  arbc::Arena arena;
  arbc::SlotStore& store =
      arena.store_for(sizeof(std::uint32_t), alignof(std::uint32_t), /*chunk_bits=*/1);

  const auto first = store.allocate();
  REQUIRE(first.has_value());
  const arbc::SlotIndex index = *first;
  const void* address = store.resolve(index);

  store.release(index); // mark the slot reusable (no destructor; raw storage)

  const auto reused = store.allocate();
  REQUIRE(reused.has_value());
  REQUIRE(*reused == index);                  // the same index, recycled in place
  REQUIRE(store.resolve(*reused) == address); // at the same stable address
}

TEST_CASE("litmus: arena growth handshake seeded sweep (long-form)", "[.nightly]") {
  run_arena_growth_litmus(/*seed_begin=*/0, /*seed_end=*/48, /*reader_count=*/4,
                          /*grow_ops=*/40000, /*tracked_count=*/512);
}
