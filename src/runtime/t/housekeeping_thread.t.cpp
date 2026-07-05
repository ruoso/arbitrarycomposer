#include <arbc/pool/checkpoint.hpp>
#include <arbc/pool/reclamation.hpp>
#include <arbc/pool/refs.hpp>
#include <arbc/pool/slot_store.hpp>
#include <arbc/pool/workspace_file.hpp>
#include <arbc/runtime/housekeeping_thread.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#if ARBC_HAS_WORKSPACE_FILES
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <memory>
#include <string>
#endif

namespace {

// The reclamation-cascade test record and chain builder, reused verbatim from
// the runtime.housekeeping fixtures (housekeeping.t.cpp): a record that holds one
// counted child and releases it on destruction, so dropping the chain root and
// draining once cascades the whole subtree.
struct Rec {
  arbc::RefStore<Rec>* store{nullptr};
  arbc::SlotRef<Rec> child{};
  bool has_child{false};
  std::uint32_t value{0};
  ~Rec() {
    if (has_child) {
      store->release(child);
    }
  }
};

arbc::Ref<Rec> build_chain(arbc::RefStore<Rec>& store, int depth) {
  arbc::Ref<Rec> current = *store.create();
  current->store = &store;
  for (int i = 1; i < depth; ++i) {
    arbc::Ref<Rec> parent = *store.create();
    parent->store = &store;
    parent->child = current.slot();
    REQUIRE(store.retain(parent->child).has_value());
    parent->has_child = true;
    current = std::move(parent);
  }
  return current;
}

// A tick period long enough that no automatic timeout fires during a
// deterministic unit test: the ONLY ticks are the ones flush() pokes. It never
// appears in an assertion (doc 16:54-62) -- it just keeps the test poke-driven.
constexpr std::chrono::steady_clock::duration kNoTimeout = std::chrono::hours(1);

TEST_CASE("background tick drains the reclamation queue") {
  arbc::Arena arena;
  arbc::RefStore<Rec> store(arena);
  arbc::DeferredReclaimSink<Rec> sink(store);
  arbc::ReclamationQueue queue;
  queue.register_store(store, sink);

  const std::size_t baseline = arena.total_slots_live();

  arbc::HousekeepingThreadConfig tc;
  tc.tick_period = kNoTimeout;
  arbc::HousekeepingThread hkt(queue, nullptr, &arena, arbc::HousekeepingConfig{}, std::move(tc));

  arbc::Ref<Rec> chain = build_chain(store, 5);
  chain = arbc::Ref<Rec>{}; // drop the root: enqueued, nothing destroyed yet
  REQUIRE(arena.total_slots_live() == baseline + 5);

  const std::uint64_t before = hkt.background_ticks();
  hkt.flush(); // poke + block until one background tick completes (no sleep)

  REQUIRE(hkt.background_ticks() >= before + 1); // the loop actually ran a tick
  REQUIRE(arena.total_slots_live() == baseline); // ...and it drained to quiescence
}

// enforces: 15-memory-model#housekeeping-thread-stops-gracefully
TEST_CASE("stop wakes the parked loop, runs a final drain, and joins cleanly") {
  arbc::Arena arena;
  arbc::RefStore<Rec> store(arena);
  arbc::DeferredReclaimSink<Rec> sink(store);
  arbc::ReclamationQueue queue;
  queue.register_store(store, sink);

  const std::size_t baseline = arena.total_slots_live();

  SECTION("the stop path's final drain reclaims a non-empty queue") {
    arbc::Ref<Rec> chain = build_chain(store, 5);
    chain = arbc::Ref<Rec>{};
    REQUIRE(arena.total_slots_live() == baseline + 5);

    {
      arbc::HousekeepingThreadConfig tc;
      tc.tick_period = kNoTimeout; // no timeout tick can drain first
      arbc::HousekeepingThread hkt(queue, nullptr, &arena, arbc::HousekeepingConfig{},
                                   std::move(tc));
      hkt.request_stop(); // stop WITHOUT a prior flush
    } // destructor joins -> the stop path's final drain_and_quiesce ran

    REQUIRE(arena.total_slots_live() == baseline);
  }

  SECTION("construct and immediately destroy does not hang") {
    arbc::HousekeepingThreadConfig tc;
    tc.tick_period = kNoTimeout;
    {
      arbc::HousekeepingThread hkt(queue, nullptr, &arena, arbc::HousekeepingConfig{},
                                   std::move(tc));
    }
    SUCCEED("destructor returned -- the join did not hang");
  }
}

TEST_CASE("the writer's after_commit drains through the wrapper mutex") {
  arbc::Arena arena;
  arbc::RefStore<Rec> store(arena);
  arbc::DeferredReclaimSink<Rec> sink(store);
  arbc::ReclamationQueue queue;
  queue.register_store(store, sink);

  const std::size_t baseline = arena.total_slots_live();

  arbc::HousekeepingThreadConfig tc;
  tc.tick_period = kNoTimeout; // loop parked; the writer path does the draining
  arbc::HousekeepingThread hkt(queue, nullptr, &arena, arbc::HousekeepingConfig{}, std::move(tc));

  arbc::Ref<Rec> chain = build_chain(store, 5);
  const arbc::SlotIndex root = chain.index();
  chain = arbc::Ref<Rec>{};
  REQUIRE(arena.total_slots_live() == baseline + 5);

  REQUIRE(hkt.after_commit(root).has_value()); // synchronized writer entry
  REQUIRE(arena.total_slots_live() == baseline);
}

#if ARBC_HAS_WORKSPACE_FILES

// A temp workspace-file path, unlinked on teardown (housekeeping.t.cpp recipe).
class TempPath {
public:
  TempPath() {
    char tmpl[] = "/tmp/arbc_hkt_XXXXXX";
    const int fd = ::mkstemp(tmpl);
    if (fd >= 0) {
      ::close(fd);
    }
    d_path = tmpl;
  }
  ~TempPath() { ::unlink(d_path.c_str()); }
  TempPath(const TempPath&) = delete;
  TempPath& operator=(const TempPath&) = delete;
  const std::string& str() const noexcept { return d_path; }

private:
  std::string d_path;
};

// Fault-injection shim (doc 16's sanctioned mocking exception): fail the first
// matching syscall with `err`.
class ErrnoInjector final : public arbc::SyscallInjector {
public:
  ErrnoInjector(arbc::WorkspaceSyscall target, int err) noexcept : d_target(target), d_err(err) {}
  int before(arbc::WorkspaceSyscall kind, std::uint64_t, std::size_t) noexcept override {
    if (kind == d_target) {
      d_fired = true;
      return d_err;
    }
    return 0;
  }
  void after(arbc::WorkspaceSyscall, std::uint64_t, std::size_t) noexcept override {}
  bool fired() const noexcept { return d_fired; }

private:
  arbc::WorkspaceSyscall d_target;
  int d_err;
  bool d_fired{false};
};

// Workspace-file-backed fixture (housekeeping.t.cpp recipe): source, arena,
// store, checkpointer with its slot fence installed, and a reclamation queue.
struct WsFixture {
  TempPath path;
  std::unique_ptr<arbc::WorkspaceFileChunkSource> source;
  arbc::Arena arena;
  arbc::RefStore<Rec> store;
  arbc::Checkpointer ckpt;
  arbc::DeferredReclaimSink<Rec> sink;
  arbc::ReclamationQueue queue;

  WsFixture()
      : source(make_source(path)), arena(*source), store(arena), ckpt(*source, arena), sink(store) {
    store.store().set_release_fence(&ckpt.slot_fence());
    queue.register_store(store, sink);
  }

  static std::unique_ptr<arbc::WorkspaceFileChunkSource> make_source(const TempPath& p) {
    auto created = arbc::WorkspaceFileChunkSource::create(p.str());
    REQUIRE(created.has_value());
    return std::move(*created);
  }
};

// enforces: 15-memory-model#checkpoint-cadence-is-policy
TEST_CASE("the background tick drives the tick-interval checkpoint trigger deterministically") {
  WsFixture fx;
  std::atomic<std::uint64_t> tick_value{0};

  arbc::HousekeepingConfig policy;
  policy.checkpoint_tick_interval = 100;
  arbc::HousekeepingThreadConfig tc;
  tc.tick_period = kNoTimeout; // ticks come only from flush(), never a timeout
  tc.tick_source = [&tick_value] { return tick_value.load(std::memory_order_acquire); };
  arbc::HousekeepingThread hkt(fx.queue, &fx.ckpt, &fx.arena, policy, std::move(tc));

  arbc::Ref<Rec> node = *fx.store.create();
  REQUIRE(hkt.after_commit(node.index()).has_value()); // one dirty transaction

  tick_value.store(50, std::memory_order_release);
  hkt.flush(); // tick(50): interval not yet elapsed
  REQUIRE(fx.ckpt.commit_count() == 0);

  tick_value.store(100, std::memory_order_release);
  hkt.flush(); // tick(100): elapsed + a dirty transaction -> commit
  REQUIRE(fx.ckpt.commit_count() == 1);
  REQUIRE(hkt.stats().checkpoints_committed == 1);

  tick_value.store(250, std::memory_order_release);
  hkt.flush(); // tick(250): elapsed but no transaction since -> skip clean
  REQUIRE(fx.ckpt.commit_count() == 1);
  REQUIRE(hkt.stats().checkpoints_skipped_clean == 1);
}

TEST_CASE("a background checkpoint failure is captured and surfaced, never aborts") {
  WsFixture fx;
  std::atomic<std::uint64_t> tick_value{0};
  std::atomic<int> callback_hits{0};

  arbc::HousekeepingConfig policy;
  policy.checkpoint_tick_interval = 100;
  arbc::HousekeepingThreadConfig tc;
  tc.tick_period = kNoTimeout;
  tc.tick_source = [&tick_value] { return tick_value.load(std::memory_order_acquire); };
  tc.on_checkpoint_error = [&callback_hits](const arbc::WorkspaceFileError&) {
    callback_hits.fetch_add(1, std::memory_order_release);
  };
  arbc::HousekeepingThread hkt(fx.queue, &fx.ckpt, &fx.arena, policy, std::move(tc));

  // Reach a clean, durable state so the tick's failing commit does ONLY the
  // header msync (a clean scene skips the data msync) -> HeaderIoFailed.
  arbc::Ref<Rec> node = *fx.store.create();
  REQUIRE(hkt.after_commit(node.index()).has_value());
  REQUIRE(hkt.request_checkpoint().has_value());       // commit -> clean, durable
  REQUIRE(hkt.after_commit(node.index()).has_value()); // a dirty-count txn, scene still clean

  ErrnoInjector inj(arbc::WorkspaceSyscall::Msync, EIO);
  fx.source->set_syscall_injector(&inj);
  tick_value.store(100, std::memory_order_release);
  hkt.flush(); // tick(100): commits at the tip, the clean-scene header msync fails
  fx.source->set_syscall_injector(nullptr);

  REQUIRE(inj.fired());
  const std::optional<arbc::WorkspaceFileError> err = hkt.last_checkpoint_error();
  REQUIRE(err.has_value());
  REQUIRE(err->code == arbc::WorkspaceFileErrc::HeaderIoFailed);
  REQUIRE(err->sys_errno == EIO);
  REQUIRE(callback_hits.load(std::memory_order_acquire) == 1);
  REQUIRE(fx.ckpt.commit_count() == 1);            // only the earlier good commit
  REQUIRE(hkt.stats().checkpoints_committed == 1); // the failed commit was not counted
}

#endif // ARBC_HAS_WORKSPACE_FILES

// enforces: 15-memory-model#housekeeping-thread-single-drainer
TEST_CASE("stress: RT producers enqueue while the background thread and writer both drain") {
  arbc::Arena arena;
  arbc::RefStore<Rec> store(arena);
  arbc::DeferredReclaimSink<Rec> sink(store);
  arbc::ReclamationQueue queue;
  queue.register_store(store, sink);

  const std::size_t baseline = arena.total_slots_live();

  constexpr int producer_count = 8;
  constexpr int per_producer = 500;
  constexpr int total = producer_count * per_producer;

  // The writer creates every slot up front (create is writer-only) and hands each
  // producer (an RT surrogate) a disjoint block of standalone-counted SlotRefs.
  std::vector<std::vector<arbc::SlotRef<Rec>>> blocks(producer_count);
  for (int p = 0; p < producer_count; ++p) {
    for (int i = 0; i < per_producer; ++i) {
      arbc::Ref<Rec> r = *store.create();
      arbc::SlotRef<Rec> s = r.slot();
      REQUIRE(store.retain(s).has_value()); // survive the owning Ref's drop
      blocks[p].push_back(s);
    }
  }
  arbc::Ref<Rec> sentinel = *store.create(); // the writer's after_commit root
  REQUIRE(arena.total_slots_live() == baseline + total + 1);

  std::atomic<bool> go{false};
  std::atomic<int> done{0};
  std::vector<std::thread> producers;

  {
    arbc::HousekeepingThreadConfig tc;
    tc.tick_period = std::chrono::microseconds(50); // an active low-priority drainer
    arbc::HousekeepingThread hkt(queue, nullptr, &arena, arbc::HousekeepingConfig{}, std::move(tc));

    for (int p = 0; p < producer_count; ++p) {
      producers.emplace_back([&, p] {
        while (!go.load(std::memory_order_acquire)) {
        }
        int n = 0;
        for (arbc::SlotRef<Rec> s : blocks[p]) {
          store.release(s); // count 1 -> 0 -> deferred enqueue (RT-safe, no wrapper)
          if ((++n % 32) == 0) {
            std::this_thread::yield(); // schedule perturbation widens the race window
          }
        }
        done.fetch_add(1, std::memory_order_release);
      });
    }

    go.store(true, std::memory_order_release);

    // The writer thread (this one) runs its between-transaction drain in a loop,
    // serialized against the background thread's tick by the wrapper mutex.
    int spins = 0;
    while (done.load(std::memory_order_acquire) < producer_count) {
      REQUIRE(hkt.after_commit(sentinel.index()).has_value());
      if ((++spins % 8) == 0) {
        std::this_thread::yield();
      }
    }

    for (std::thread& th : producers) {
      th.join();
    }
    sentinel = arbc::Ref<Rec>{}; // drop the last root: enqueued for the final drain
  } // hkt destructor: request_stop + join + a final drain_and_quiesce

  // Outcome only, no timing: every released slot reclaimed exactly once, none
  // lost or double-freed -- the arena is back to its no-garbage baseline.
  REQUIRE(arena.total_slots_live() == baseline);
  REQUIRE(store.slots_live() == baseline);
}

} // namespace
