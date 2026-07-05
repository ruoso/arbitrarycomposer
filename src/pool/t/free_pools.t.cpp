#include <arbc/pool/chunk_source.hpp>
#include <arbc/pool/reclamation.hpp>
#include <arbc/pool/refs.hpp>
#include <arbc/pool/slot_store.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <set>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

constexpr std::size_t k_batch = arbc::SlotStore::k_free_pool_batch;

// Cross-thread release is now legal: a slot allocated by the writer is freed from
// a different thread and does not abort (the previous writer-only assert would
// have fired on the release path).
TEST_CASE("a slot allocated by the writer can be released from another thread") {
  arbc::Arena arena;
  arbc::SlotStore& store = arena.store_for(48, alignof(std::max_align_t));

  const auto index = store.allocate(); // writer (this thread) binds + allocates
  REQUIRE(index.has_value());
  REQUIRE(store.slots_live() == 1);

  std::thread releaser([&] { store.release(*index); }); // NOT the writer thread
  releaser.join();

  REQUIRE(store.slots_live() == 0); // the cross-thread release was accepted
}

#if defined(__linux__) && !defined(NDEBUG)

// The asymmetry is pinned: release is any-thread but ALLOCATE stays writer-only.
// A debug-build allocate from a non-writer thread trips the writer assert, which
// aborts. Fork a child so the abort does not take the test runner down.
TEST_CASE("allocate from a non-writer thread still trips the writer assert (debug)") {
  const pid_t pid = ::fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    // Restore the default SIGABRT disposition so the assert's abort() actually
    // signals the child (Catch2's inherited handler would otherwise swallow it).
    ::signal(SIGABRT, SIG_DFL);
    arbc::Arena arena;
    arbc::SlotStore& store = arena.store_for(48, alignof(std::max_align_t));
    // Bind the writer to a helper thread, then join it so the child is
    // single-threaded again before the offending call (sanitizer-friendly).
    std::thread binder([&] { (void)store.allocate(); });
    binder.join();
    (void)store.allocate(); // child main thread != writer -> assert -> abort()
    ::_exit(0);             // reached only if the assert did NOT fire
  }
  int status = 0;
  REQUIRE(::waitpid(pid, &status, 0) == pid);
  REQUIRE(WIFSIGNALED(status));
  REQUIRE(WTERMSIG(status) == SIGABRT);
}

#endif // __linux__ && !NDEBUG

// enforces: 15-memory-model#thread-local-free-pools-spill-to-global
TEST_CASE("round-trip recycle: a second thread frees a batch multiple, the writer reuses it") {
  arbc::Arena arena;
  arbc::SlotStore& store = arena.store_for(64, alignof(std::max_align_t));

  // A whole number of batches so the releaser's local pool spills completely and
  // every freed slot reaches the shared global pool (no remainder stranded).
  constexpr std::size_t n = 4 * k_batch;
  std::vector<arbc::SlotIndex> first;
  for (std::size_t i = 0; i < n; ++i) {
    const auto idx = store.allocate();
    REQUIRE(idx.has_value());
    first.push_back(*idx);
  }
  const std::uint32_t high_water = store.high_water();
  const std::size_t capacity = store.slots_capacity();
  REQUIRE(store.slots_live() == n);

  // A second thread releases all n; each full batch spills to the global pool.
  std::thread releaser([&] {
    for (const arbc::SlotIndex idx : first) {
      store.release(idx);
    }
  });
  releaser.join();
  REQUIRE(store.slots_live() == 0);
  REQUIRE(store.spill_count() >= 4); // every batch spilled cross-thread

  // The writer reuses the spilled slots via global-pool refills -- no high-water
  // growth, no new capacity, and every slot comes back with no loss or dup.
  std::set<arbc::SlotIndex> second;
  for (std::size_t i = 0; i < n; ++i) {
    const auto idx = store.allocate();
    REQUIRE(idx.has_value());
    second.insert(*idx);
  }
  REQUIRE(store.refill_count() >= 4);            // reuse went through the global pool
  REQUIRE(store.high_water() == high_water);     // no growth beyond n
  REQUIRE(store.slots_capacity() == capacity);   // capacity unchanged
  REQUIRE(second.size() == n);                   // no duplication
  REQUIRE(second == std::set<arbc::SlotIndex>(first.begin(), first.end())); // no loss
}

// enforces: 15-memory-model#reuse-is-thread-affine
TEST_CASE("a thread's just-released slot is its next same-thread allocation, before any global "
          "round-trip") {
  arbc::Arena arena;
  arbc::SlotStore& store = arena.store_for(32, alignof(std::max_align_t));

  // Warm up with a few sub-batch allocations (pure growth touches neither the
  // spill nor the refill boundary).
  for (int i = 0; i < 3; ++i) {
    REQUIRE(store.allocate().has_value());
  }
  const auto victim = store.allocate();
  REQUIRE(victim.has_value());
  const std::size_t spills = store.spill_count();
  const std::size_t refills = store.refill_count();

  store.release(*victim); // pushes onto THIS (writer) thread's local pool
  const auto reused = store.allocate();
  REQUIRE(reused.has_value());

  REQUIRE(*reused == *victim);                  // thread-affine: the same slot came back
  REQUIRE(store.spill_count() == spills);       // no spill
  REQUIRE(store.refill_count() == refills);     // no global-pool round-trip
}

// The sub-batch hot path takes no global lock: spill/refill counters stay put for
// a release-then-reallocate burst smaller than one batch, and advance only when a
// local pool crosses the batch threshold (doc 16 behavioral-counter discipline).
TEST_CASE("a sub-batch churn burst takes no global lock; crossing the threshold does") {
  arbc::Arena arena;
  arbc::SlotStore& store = arena.store_for(32, alignof(std::max_align_t));

  std::vector<arbc::SlotIndex> slots;
  for (std::size_t i = 0; i < k_batch - 1; ++i) { // fewer than one batch
    const auto idx = store.allocate();
    REQUIRE(idx.has_value());
    slots.push_back(*idx);
  }
  const std::size_t spills = store.spill_count();
  const std::size_t refills = store.refill_count();

  for (const arbc::SlotIndex idx : slots) {
    store.release(idx); // all stay in the writer's local pool: no spill
  }
  for (std::size_t i = 0; i < slots.size(); ++i) {
    REQUIRE(store.allocate().has_value()); // all served locally: no refill
  }
  REQUIRE(store.spill_count() == spills);   // hot path took no global lock
  REQUIRE(store.refill_count() == refills);

  // Now push the local pool across the batch threshold: exactly one spill fires.
  std::vector<arbc::SlotIndex> full;
  for (std::size_t i = 0; i < k_batch; ++i) {
    const auto idx = store.allocate();
    REQUIRE(idx.has_value());
    full.push_back(*idx);
  }
  for (const arbc::SlotIndex idx : full) {
    store.release(idx);
  }
  REQUIRE(store.spill_count() == spills + 1); // advanced only at the threshold
}

// A ReleaseFence that records diverted slots and returns them on demand -- the
// pool-only analogue of pool.checkpoints' durability fence.
class RecordingFence final : public arbc::ReleaseFence {
public:
  void on_release(arbc::SlotStore& /*store*/, arbc::SlotIndex index) override {
    d_quarantined.push_back(index);
  }
  void drain_to(arbc::SlotStore& store) {
    for (const arbc::SlotIndex idx : d_quarantined) {
      store.free_now(idx); // returns onto the caller's local pool
    }
    d_quarantined.clear();
  }
  std::size_t pending() const noexcept { return d_quarantined.size(); }

private:
  std::vector<arbc::SlotIndex> d_quarantined;
};

TEST_CASE("with a fence installed release diverts to the fence; only free_now returns the slot") {
  arbc::Arena arena;
  arbc::SlotStore& store = arena.store_for(48, alignof(std::max_align_t));
  RecordingFence fence;
  store.set_release_fence(&fence);

  const auto idx = store.allocate();
  REQUIRE(idx.has_value());
  store.release(*idx); // fence intercepts: NOT returned to any pool yet
  REQUIRE(fence.pending() == 1);

  // The next allocation does not reuse the quarantined slot (it is still with the
  // fence), so the writer grows instead.
  const auto grown = store.allocate();
  REQUIRE(grown.has_value());
  REQUIRE(*grown != *idx);

  // free_now returns the quarantined slot to the writer's local pool; now it
  // reuses.
  fence.drain_to(store);
  REQUIRE(fence.pending() == 0);
  const auto reused = store.allocate();
  REQUIRE(reused.has_value());
  REQUIRE(*reused == *idx);
}

// A record that reports its own destruction, so the concurrent smoke can prove
// every ~T fires exactly once.
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

// enforces: 15-memory-model#thread-local-free-pools-spill-to-global
TEST_CASE("concurrent: the writer allocates while a low-priority thread drains cross-thread") {
  arbc::Arena arena;
  arbc::RefStore<Tracked> store(arena);
  arbc::DeferredReclaimSink<Tracked> sink(store);
  arbc::ReclamationQueue queue;
  queue.register_store(store, sink);

  constexpr int total = 2000;
  int destructions = 0;

  // The writer (this thread) creates every initial slot up front (create is
  // writer-only) and keeps a standalone count on each so it survives the Ref drop.
  std::vector<arbc::SlotRef<Tracked>> pending;
  for (int i = 0; i < total; ++i) {
    arbc::Ref<Tracked> r = *store.create(i, &destructions);
    arbc::SlotRef<Tracked> s = r.slot();
    REQUIRE(store.retain(s).has_value());
    pending.push_back(s);
  }
  REQUIRE(store.slots_live() == total);

  std::atomic<bool> go{false};
  std::atomic<bool> stop{false};

  // The low-priority drainer is the SINGLE drainer while it runs: drain bottoms
  // out in SlotStore::release, freeing slots cross-thread into the drainer's own
  // local pool -- concurrently with the writer's allocate below.
  std::thread drainer([&] {
    while (!go.load(std::memory_order_acquire)) {
    }
    while (!stop.load(std::memory_order_acquire)) {
      queue.drain();
    }
  });

  go.store(true, std::memory_order_release);

  // Writer releases the pending block (each count 1 -> 0 -> enqueue) and, in the
  // same window, churns fresh allocations (writer allocate racing the drainer's
  // cross-thread release), each dropped straight to the reclamation queue.
  for (arbc::SlotRef<Tracked> s : pending) {
    store.release(s);
  }
  for (int i = 0; i < total; ++i) {
    arbc::Ref<Tracked> r = *store.create(total + i, &destructions);
    // r drops here -> count 0 -> enqueued -> reclaimed by the drainer thread.
  }

  stop.store(true, std::memory_order_release);
  drainer.join();

  // The writer is now the single drainer: sweep whatever was enqueued last.
  queue.drain();

  REQUIRE(destructions == 2 * total); // every ~T fired exactly once
  REQUIRE(store.slots_live() == 0);   // no loss, no duplication: back to baseline
}

} // namespace
