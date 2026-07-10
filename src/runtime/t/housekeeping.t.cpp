#include <arbc/pool/checkpoint.hpp>
#include <arbc/pool/reclamation.hpp>
#include <arbc/pool/refs.hpp>
#include <arbc/pool/slot_store.hpp>
#include <arbc/pool/workspace_file.hpp>
#include <arbc/runtime/housekeeping.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

// The housekeeping suite drives the file-backed workspace on both POSIX (mkstemp/
// unlink) and Windows (GetTempFileNameA/DeleteFileA). The platform leaf branches on
// `_WIN32`; everything above the capability gate runs unconditionally. The Windows
// housekeeping port landed with runtime.housekeeping_win32.
#if ARBC_HAS_WORKSPACE_FILES
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <cerrno>
#include <cstdlib>
#include <memory>
#include <string>
#endif

namespace {

// A test record in the shape the reclamation cascade expects: it may hold one
// child SlotRef on which it keeps its own count (the holder-holds-a-count
// convention), releasing it in its destructor so a drain unrolls a subtree.
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

// Build a `depth`-node retained-SlotRef chain and return an owning Ref to its
// root. Each parent holds its own count on its child, so the whole chain is kept
// alive solely by the returned root Ref; dropping that Ref enqueues the root and
// a single drain cascades the rest.
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

// enforces: 15-memory-model#housekeeping-drains-between-transactions
TEST_CASE("housekeeper drains the reclamation queue between transactions") {
  arbc::Arena arena;
  arbc::RefStore<Rec> store(arena);
  arbc::DeferredReclaimSink<Rec> sink(store);
  arbc::ReclamationQueue queue;
  queue.register_store(store, sink);

  const std::size_t baseline = store.slots_live();

  SECTION("drain_between_transactions = true reclaims to the no-garbage baseline") {
    arbc::HousekeepingConfig config; // drain_between_transactions defaults true
    arbc::Housekeeper hk(queue, nullptr, &arena, config);

    arbc::Ref<Rec> chain = build_chain(store, 5);
    const arbc::SlotIndex root = chain.index();
    REQUIRE(store.slots_live() == baseline + 5);
    chain = arbc::Ref<Rec>{}; // drop the root: enqueued, nothing destroyed yet
    REQUIRE(store.slots_live() == baseline + 5);

    REQUIRE(hk.after_commit(root).has_value());
    REQUIRE(store.slots_live() == baseline); // drained to quiescence
    REQUIRE(hk.stats().drains_run == 1);
  }

  SECTION("drain_between_transactions = false defers to an explicit quiesce") {
    arbc::HousekeepingConfig config;
    config.drain_between_transactions = false;
    arbc::Housekeeper hk(queue, nullptr, &arena, config);

    arbc::Ref<Rec> chain = build_chain(store, 5);
    const arbc::SlotIndex root = chain.index();
    chain = arbc::Ref<Rec>{};

    REQUIRE(hk.after_commit(root).has_value());
    REQUIRE(store.slots_live() == baseline + 5); // left un-drained on the queue
    REQUIRE(hk.stats().drains_run == 0);

    hk.drain_and_quiesce();
    REQUIRE(store.slots_live() == baseline);
    REQUIRE(hk.stats().drains_run == 1);
  }
}

// enforces: 15-memory-model#checkpoint-cadence-is-policy
TEST_CASE("housekeeper checkpoint triggers are inert on an anonymous arena") {
  arbc::Arena arena;
  arbc::RefStore<Rec> store(arena);
  arbc::DeferredReclaimSink<Rec> sink(store);
  arbc::ReclamationQueue queue;
  queue.register_store(store, sink);

  arbc::HousekeepingConfig config;
  config.checkpoint_every_n_transactions = 1;
  config.checkpoint_tick_interval = 10;
  arbc::Housekeeper hk(queue, nullptr, &arena, config); // no checkpointer

  arbc::Ref<Rec> chain = build_chain(store, 3);
  const arbc::SlotIndex root = chain.index();
  chain = arbc::Ref<Rec>{};

  REQUIRE(hk.after_commit(root).has_value());
  REQUIRE(hk.tick(1000).has_value());
  REQUIRE(hk.request_checkpoint().has_value());

  const arbc::HousekeepingStats stats = hk.stats();
  REQUIRE(stats.checkpoints_committed == 0);
  REQUIRE(stats.checkpoints_skipped_clean == 0);
  REQUIRE(stats.live_slots == arena.total_slots_live());
  REQUIRE(stats.live_slots == 0); // reclamation cadence still ran
  REQUIRE(stats.slots_freed_to_list == 0);
  REQUIRE(stats.durable_epoch == 0);
}

#if ARBC_HAS_WORKSPACE_FILES

// A temp workspace-file path, cleaned up on teardown. The platform leaf mirrors
// checkpoint.t.cpp: GetTempFileNameA/DeleteFileA on Windows, mkstemp/unlink on POSIX.
class TempPath {
public:
  TempPath() {
#if defined(_WIN32)
    char dir[MAX_PATH];
    const DWORD n = ::GetTempPathA(MAX_PATH, dir);
    char buf[MAX_PATH];
    // GetTempFileNameA creates the file; create() reopens it with CREATE_ALWAYS.
    if (n != 0 && n < static_cast<DWORD>(MAX_PATH) && ::GetTempFileNameA(dir, "hk", 0, buf) != 0) {
      d_path = buf;
    }
#else
    char tmpl[] = "/tmp/arbc_hk_XXXXXX";
    const int fd = ::mkstemp(tmpl);
    if (fd >= 0) {
      ::close(fd);
    }
    d_path = tmpl;
#endif
  }
  ~TempPath() {
#if defined(_WIN32)
    ::DeleteFileA(d_path.c_str());
#else
    ::unlink(d_path.c_str());
#endif
  }
  TempPath(const TempPath&) = delete;
  TempPath& operator=(const TempPath&) = delete;
  const std::string& str() const noexcept { return d_path; }

private:
  std::string d_path;
};

// Fault-injection shim (doc 16's sanctioned mocking exception): fail the first
// matching syscall with `err`. Used to force a header msync (sync_header)
// failure on a clean-scene commit, whose only Msync is the header sync.
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

// A workspace-file-backed fixture: source, arena, store, checkpointer (with its
// slot fence installed), and a reclamation queue over the store's deferred sink.
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
TEST_CASE("housekeeper commits on the transaction-count trigger") {
  WsFixture fx;
  arbc::HousekeepingConfig config;
  config.checkpoint_every_n_transactions = 3;
  arbc::Housekeeper hk(fx.queue, &fx.ckpt, &fx.arena, config);

  std::vector<arbc::Ref<Rec>> live; // kept alive so each transaction dirties the arena
  for (int i = 0; i < 7; ++i) {
    arbc::Ref<Rec> node = *fx.store.create();
    node->value = static_cast<std::uint32_t>(i);
    const arbc::SlotIndex root = node.index();
    live.push_back(std::move(node));
    REQUIRE(hk.after_commit(root).has_value());
  }

  REQUIRE(fx.ckpt.commit_count() == 2); // fired at the 3rd and 6th transaction
  REQUIRE(hk.stats().checkpoints_committed == 2);
  REQUIRE(hk.stats().transactions_seen == 7);
}

// enforces: 15-memory-model#checkpoint-cadence-is-policy
TEST_CASE("housekeeper commits on the tick-interval trigger and skips a still scene") {
  WsFixture fx;
  arbc::HousekeepingConfig config;
  config.checkpoint_tick_interval = 100;
  arbc::Housekeeper hk(fx.queue, &fx.ckpt, &fx.arena, config);

  arbc::Ref<Rec> node = *fx.store.create();
  REQUIRE(hk.after_commit(node.index()).has_value()); // one transaction since checkpoint

  REQUIRE(hk.tick(50).has_value()); // interval not yet elapsed
  REQUIRE(fx.ckpt.commit_count() == 0);

  REQUIRE(hk.tick(100).has_value()); // elapsed + a dirty transaction -> commit
  REQUIRE(fx.ckpt.commit_count() == 1);
  REQUIRE(hk.stats().checkpoints_committed == 1);

  REQUIRE(hk.tick(250).has_value()); // elapsed but no transaction since -> skip
  REQUIRE(fx.ckpt.commit_count() == 1);
  REQUIRE(hk.stats().checkpoints_skipped_clean == 1);
}

// enforces: 15-memory-model#checkpoint-cadence-is-policy
TEST_CASE("explicit request_checkpoint is unconditional and skips the data msync when clean") {
  WsFixture fx;
  arbc::HousekeepingConfig config; // no automatic triggers
  arbc::Housekeeper hk(fx.queue, &fx.ckpt, &fx.arena, config);

  arbc::Ref<Rec> node = *fx.store.create();
  REQUIRE(hk.after_commit(node.index()).has_value());

  REQUIRE(hk.request_checkpoint().has_value());
  REQUIRE(fx.ckpt.commit_count() == 1);
  const std::uint64_t data_after_first = fx.ckpt.data_msyncs();

  // A clean scene still commits (advances commit_count) but skips the data msync.
  REQUIRE(hk.request_checkpoint().has_value());
  REQUIRE(fx.ckpt.commit_count() == 2);
  REQUIRE(fx.ckpt.data_msyncs() == data_after_first);
  REQUIRE(hk.stats().checkpoints_committed == 2);
}

// enforces: 15-memory-model#checkpoint-cadence-is-policy
TEST_CASE("a workspace sync failure surfaces from request_checkpoint as a value") {
  WsFixture fx;
  arbc::HousekeepingConfig config;
  arbc::Housekeeper hk(fx.queue, &fx.ckpt, &fx.arena, config);

  arbc::Ref<Rec> node = *fx.store.create();
  REQUIRE(hk.after_commit(node.index()).has_value());
  REQUIRE(hk.request_checkpoint().has_value()); // reach a clean, durable state

  ErrnoInjector inj(arbc::WorkspaceSyscall::Msync, EIO);
  fx.source->set_syscall_injector(&inj);
  const auto result = hk.request_checkpoint(); // clean -> only the header msync -> fails
  fx.source->set_syscall_injector(nullptr);

  REQUIRE(inj.fired());
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().code == arbc::WorkspaceFileErrc::HeaderIoFailed);
  REQUIRE(result.error().sys_errno == EIO);
  REQUIRE(hk.stats().checkpoints_committed == 1); // the failed commit was not counted
}

// enforces: 15-memory-model#housekeeping-reports-memory-panel-stats
TEST_CASE("housekeeper stats aggregate the driven counts and underlying counters") {
  WsFixture fx;
  arbc::HousekeepingConfig config;
  config.checkpoint_every_n_transactions = 2;
  arbc::Housekeeper hk(fx.queue, &fx.ckpt, &fx.arena, config);

  // Two transactions whose records stay live (grow the arena)...
  arbc::Ref<Rec> a = *fx.store.create();
  REQUIRE(hk.after_commit(a.index()).has_value());
  arbc::Ref<Rec> b = *fx.store.create();
  REQUIRE(hk.after_commit(b.index()).has_value()); // 2nd transaction -> commit

  // ...plus a released chain the between-transaction drain reclaims.
  arbc::Ref<Rec> chain = build_chain(fx.store, 4);
  const arbc::SlotIndex root = chain.index();
  chain = arbc::Ref<Rec>{};
  REQUIRE(hk.after_commit(root).has_value());

  const arbc::HousekeepingStats stats = hk.stats();
  REQUIRE(stats.transactions_seen == 3);
  REQUIRE(stats.drains_run == 3); // one per after_commit (drain defaults on)
  REQUIRE(stats.checkpoints_committed == 1);
  REQUIRE(stats.checkpoints_skipped_clean == 0);
  REQUIRE(stats.live_slots == fx.arena.total_slots_live());
  REQUIRE(stats.live_slots == 2); // a and b remain; the chain was reclaimed
  REQUIRE(stats.slots_freed_to_list == fx.ckpt.slots_freed_to_list());
  REQUIRE(stats.durable_epoch == fx.ckpt.durable_epoch());
}

#endif // ARBC_HAS_WORKSPACE_FILES

} // namespace
