#include <arbc/pool/checkpoint.hpp>
#include <arbc/pool/refs.hpp>
#include <arbc/pool/slot_store.hpp>
#include <arbc/pool/workspace_file.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

// The crash-recovery sweep rides the file-backed workspace. Its body (fork/kill
// injection, RLIMIT disk-full) is POSIX-specific; the Windows port is the separate
// pool.crash_tests_win32 leaf, so this stays gated off `_WIN32` even though Windows
// now has workspace files. On platforms without workspace files (or on Windows)
// this compiles to just the runtime-query sanity check.
TEST_CASE("crash-test harness tracks workspace-file support") {
  REQUIRE(arbc::workspace_files_supported() == (ARBC_HAS_WORKSPACE_FILES != 0));
}

#if ARBC_HAS_WORKSPACE_FILES && !defined(_WIN32)

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

// A self-cleaning unique workspace-file path under the temp dir (mirrors the
// checkpoint / workspace_file tests).
class TempPath {
public:
  TempPath() {
    char tmpl[] = "/tmp/arbc_crash_XXXXXX";
    const int fd = ::mkstemp(tmpl);
    if (fd >= 0) {
      ::close(fd);
    }
    d_path = tmpl;
  }
  ~TempPath() {
    if (!d_path.empty()) {
      ::unlink(d_path.c_str());
    }
  }
  TempPath(const TempPath&) = delete;
  TempPath& operator=(const TempPath&) = delete;
  TempPath(TempPath&& other) noexcept : d_path(std::move(other.d_path)) { other.d_path.clear(); }
  const std::string& str() const { return d_path; }

private:
  std::string d_path;
};

// A test-local record graph standing in for the model layer (doc 14 node types
// are L2; pool must not depend up). Trivially destructible, standard-layout, and
// pointer-free: children are index-only SlotRefs (doc 15 position independence).
struct GraphNode {
  std::uint32_t value = 0;
  arbc::SlotRef<GraphNode> left{};
  arbc::SlotRef<GraphNode> right{};
  bool has_left = false;
  bool has_right = false;
};

// Build the canonical little graph: root(100) -> {left(1), right(2)}. Each child
// is retained by the record; the transient child Refs drop so every node ends at
// count 1 (the root's count is the external pin the caller keeps).
arbc::Ref<GraphNode> build_graph(arbc::RefStore<GraphNode>& store) {
  arbc::Ref<GraphNode> left = *store.create();
  left->value = 1;
  arbc::Ref<GraphNode> right = *store.create();
  right->value = 2;
  arbc::Ref<GraphNode> root = *store.create();
  root->value = 100;
  root->left = left.slot();
  root->has_left = true;
  REQUIRE(store.retain(root->left).has_value());
  root->right = right.slot();
  root->has_right = true;
  REQUIRE(store.retain(root->right).has_value());
  return root;
}

// A reachability walk over the reopened store (the mechanism the model drives on
// document open). Reads records by raw storage index and rebuilds counts.
struct WalkResult {
  std::vector<arbc::SlotIndex> live;
};

WalkResult walk(arbc::RefStore<GraphNode>& store, arbc::SlotIndex root_index) {
  std::vector<int> counts;
  std::vector<char> seen;
  const auto ensure = [&](arbc::SlotIndex i) {
    if (i >= counts.size()) {
      counts.resize(i + 1, 0);
      seen.resize(i + 1, 0);
    }
  };

  WalkResult result;
  std::vector<arbc::SlotIndex> stack{root_index};
  ensure(root_index);
  counts[root_index] += 1;
  while (!stack.empty()) {
    const arbc::SlotIndex i = stack.back();
    stack.pop_back();
    if (seen[i]) {
      continue;
    }
    seen[i] = 1;
    result.live.push_back(i);
    const GraphNode* node = store.peek_index(i);
    if (node->has_left) {
      const arbc::SlotIndex c = node->left.index();
      ensure(c);
      counts[c] += 1;
      stack.push_back(c);
    }
    if (node->has_right) {
      const arbc::SlotIndex c = node->right.index();
      ensure(c);
      counts[c] += 1;
      stack.push_back(c);
    }
  }
  for (arbc::SlotIndex i : result.live) {
    store.set_count_index(i, static_cast<std::uint32_t>(counts[i]));
  }
  return result;
}

// Byte-copy the workspace file so recovery runs against an INDEPENDENT file (its
// own fd + mappings), mirroring the landed checkpoint.t.cpp idiom.
void copy_file(const std::string& src, const std::string& dst) {
  const int in = ::open(src.c_str(), O_RDONLY);
  REQUIRE(in >= 0);
  const int out = ::open(dst.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
  REQUIRE(out >= 0);
  char buf[65536];
  ssize_t n = 0;
  while ((n = ::read(in, buf, sizeof(buf))) > 0) {
    ssize_t off = 0;
    while (off < n) {
      const ssize_t w = ::write(out, buf + off, static_cast<std::size_t>(n - off));
      REQUIRE(w > 0);
      off += w;
    }
  }
  REQUIRE(n == 0);
  ::close(in);
  ::close(out);
}

// --- in-process durable-snapshot injector (decision (a)) ---------------------
//
// Models a crash in-process: the two header A/B root slots become durable only
// when a header msync completes, so this injector pins them to their last-
// durable values (advancing them only in the header-msync `after`). At a chosen
// injection point it byte-copies the file and patches those durable slots in --
// exactly the state a crash at that point recovers from. Data/directory pages
// that are "ahead" of the durable root are unreachable from it, so copying the
// live page cache for them is harmless. This is a faithful generalization of the
// landed `copy_file` clean-boundary reopen to every mid-commit boundary.
class SnapshotInjector final : public arbc::SyscallInjector {
public:
  SnapshotInjector(arbc::WorkspaceFileChunkSource& source, std::string path)
      : d_source(&source), d_path(std::move(path)) {}

  // Enumerate injection points (Msync + RootFlip) without capturing.
  void count_only() { reset(-1, false, false, {}); }
  // Capture at the `target`-th (0-based) enumerated point, in the given phase.
  void arm(long target, bool after_phase, std::string snapshot) {
    reset(target, after_phase, false, std::move(snapshot));
  }
  // Capture in the `before` phase of the header msync (kill just before the
  // publish becomes durable), regardless of the point index.
  void arm_before_header(std::string snapshot) { reset(-1, false, true, std::move(snapshot)); }

  long points() const { return d_point; }
  bool captured() const { return d_captured; }

  int before(arbc::WorkspaceSyscall kind, std::uint64_t file_offset,
             std::size_t) noexcept override {
    if (kind == arbc::WorkspaceSyscall::Msync || kind == arbc::WorkspaceSyscall::RootFlip) {
      const long idx = d_point++;
      d_pending_after = false;
      if (d_before_header && kind == arbc::WorkspaceSyscall::Msync && file_offset == 0) {
        capture();
      } else if (idx == d_target && !d_after) {
        capture();
      } else if (idx == d_target && d_after) {
        d_pending_after = true;
      }
    }
    return 0;
  }

  void after(arbc::WorkspaceSyscall kind, std::uint64_t file_offset,
             std::size_t) noexcept override {
    if (kind == arbc::WorkspaceSyscall::Msync && file_offset == 0) {
      // Header msync completed: the flipped root is now durable.
      d_durable_a = d_source->root_slot(0);
      d_durable_b = d_source->root_slot(1);
    }
    if ((kind == arbc::WorkspaceSyscall::Msync || kind == arbc::WorkspaceSyscall::RootFlip) &&
        d_pending_after) {
      capture();
      d_pending_after = false;
    }
  }

private:
  void reset(long target, bool after_phase, bool before_header, std::string snapshot) {
    d_point = 0;
    d_target = target;
    d_after = after_phase;
    d_before_header = before_header;
    d_snapshot = std::move(snapshot);
    d_captured = false;
    d_pending_after = false;
  }

  void capture() {
    if (d_snapshot.empty()) {
      return;
    }
    copy_file(d_path, d_snapshot);
    const int fd = ::open(d_snapshot.c_str(), O_RDWR);
    if (fd >= 0) {
      const ssize_t wrote_a = ::pwrite(fd, &d_durable_a, sizeof(d_durable_a),
                                       offsetof(arbc::WorkspaceHeader, root_slot_a));
      const ssize_t wrote_b = ::pwrite(fd, &d_durable_b, sizeof(d_durable_b),
                                       offsetof(arbc::WorkspaceHeader, root_slot_b));
      if (wrote_a != static_cast<ssize_t>(sizeof(d_durable_a)) ||
          wrote_b != static_cast<ssize_t>(sizeof(d_durable_b))) {
        ::close(fd);
        return;
      }
      ::close(fd);
    }
    d_captured = true;
  }

  arbc::WorkspaceFileChunkSource* d_source;
  std::string d_path;
  std::string d_snapshot;
  std::uint64_t d_durable_a{0};
  std::uint64_t d_durable_b{0};
  long d_point{0};
  long d_target{-1};
  bool d_after{false};
  bool d_before_header{false};
  bool d_pending_after{false};
  bool d_captured{false};
};

// Injects `err` (an errno) on the `nth` (0-based) occurrence of `target`,
// passing every other syscall through -- the disk-full model.
class ErrnoInjector final : public arbc::SyscallInjector {
public:
  ErrnoInjector(arbc::WorkspaceSyscall target, int err, long nth = 0)
      : d_target(target), d_err(err), d_nth(nth) {}

  int before(arbc::WorkspaceSyscall kind, std::uint64_t, std::size_t) noexcept override {
    if (kind == d_target && d_seen++ == d_nth) {
      d_fired = true;
      return d_err;
    }
    return 0;
  }
  void after(arbc::WorkspaceSyscall, std::uint64_t, std::size_t) noexcept override {}
  bool fired() const { return d_fired; }

private:
  arbc::WorkspaceSyscall d_target;
  int d_err;
  long d_nth;
  long d_seen{0};
  bool d_fired{false};
};

// Reopen `snapshot`, assert the durable root resolves to the expected graph and
// the walk is consistent (generation valid, reachable slots consistent). An
// `expected_gen` of 0 accepts any valid (non-zero) generation.
void assert_recovers(const std::string& snapshot, std::uint32_t expected_gen,
                     std::uint32_t expected_root_value, std::uint32_t high_water) {
  auto opened = arbc::Checkpointer::open(snapshot);
  REQUIRE(opened.has_value());
  REQUIRE(opened->valid);
  if (expected_gen != 0) {
    REQUIRE(opened->generation == expected_gen);
  } else {
    REQUIRE(opened->generation >= 1);
  }
  arbc::Arena a(*opened->source);
  arbc::RefStore<GraphNode> s(a);
  REQUIRE(s.restore(high_water).has_value());
  const WalkResult wr = walk(s, opened->root_index);
  REQUIRE(wr.live.size() == 3);
  REQUIRE(s.peek_index(opened->root_index)->value == expected_root_value);
}

// Build a durable G1 (root value 100) then a distinct in-flight G2 (root value
// 200) in the same arena, and commit G2 with the snapshot injector configured.
// Returns the number of enumerated injection points during commit #2. When
// `snapshot` is non-empty the injector captures at (target, after); when empty
// it only counts (and asserts the count matches the behavioral counters).
long run_second_commit(long target, bool after, const std::string& snapshot) {
  TempPath path;
  auto src = arbc::WorkspaceFileChunkSource::create(path.str());
  REQUIRE(src.has_value());
  arbc::WorkspaceFileChunkSource& ws = **src;
  arbc::Arena arena(ws);
  arbc::RefStore<GraphNode> store(arena);
  arbc::Checkpointer ckpt(ws, arena);

  SnapshotInjector inj(ws, path.str());
  ws.set_syscall_injector(&inj);

  arbc::Ref<GraphNode> root1 = build_graph(store);
  REQUIRE(ckpt.commit(root1.index()).has_value()); // G1 durable (gen 1, slot A)

  arbc::Ref<GraphNode> root2 = build_graph(store);
  root2->value = 200;

  if (snapshot.empty()) {
    inj.count_only();
  } else {
    inj.arm(target, after, snapshot);
  }
  const std::uint64_t data_before = ckpt.data_msyncs();
  REQUIRE(ckpt.commit(root2.index()).has_value());
  const std::uint64_t data_delta = ckpt.data_msyncs() - data_before;

  ws.set_syscall_injector(nullptr); // clear before teardown (inj is a local)
  const long np = inj.points();
  if (snapshot.empty()) {
    // Coverage-completeness: the enumerated points equal what the behavioral
    // counters predict (data msyncs + one flip + one header msync) -- no
    // silently skipped syscall.
    REQUIRE(np == static_cast<long>(data_delta) + 2);
    REQUIRE_FALSE(inj.captured());
  } else {
    REQUIRE(inj.captured());
  }
  return np;
}

} // namespace

// enforces: 15-memory-model#checkpoint-recovers-consistent-root
TEST_CASE("commit-ordering kill sweep: old root before the header sync, new root after", "[pool]") {
  const long num_points = run_second_commit(0, false, {}); // count-only pass
  REQUIRE(num_points >= 3);                                // >=1 data msync + flip + header msync

  for (long target = 0; target < num_points; ++target) {
    for (bool after : {false, true}) {
      TempPath snap;
      const long np = run_second_commit(target, after, snap.str());
      REQUIRE(np == num_points); // every re-run enumerates the same boundaries

      // The publish becomes durable only at the header msync (the last point).
      // Every earlier boundary -- and the header msync's `before` -- recovers
      // the OLD root; only the header msync's `after` recovers the NEW root.
      const bool is_new = (target == num_points - 1) && after;
      assert_recovers(snap.str(), is_new ? 2u : 1u, is_new ? 200u : 100u, /*high_water=*/6);
    }
  }
}

// enforces: 15-memory-model#freed-slot-quarantined-until-durable
TEST_CASE("freed slot survives a crash before the freeing is durable", "[pool]") {
  TempPath path;
  auto src = arbc::WorkspaceFileChunkSource::create(path.str());
  REQUIRE(src.has_value());
  arbc::WorkspaceFileChunkSource& ws = **src;
  arbc::Arena arena(ws);
  arbc::RefStore<GraphNode> store(arena);
  arbc::Checkpointer ckpt(ws, arena);
  store.store().set_release_fence(&ckpt.slot_fence());

  SnapshotInjector inj(ws, path.str());
  ws.set_syscall_injector(&inj);

  // G1: root1 references child Y. Commit -> the durable root references Y.
  arbc::Ref<GraphNode> child_y = *store.create();
  child_y->value = 42;
  arbc::Ref<GraphNode> root1 = *store.create();
  root1->value = 100;
  root1->left = child_y.slot();
  root1->has_left = true;
  REQUIRE(store.retain(root1->left).has_value());
  const arbc::SlotIndex y_index = child_y.index();
  child_y = arbc::Ref<GraphNode>{}; // Y now held only by root1 (count 1)
  REQUIRE(ckpt.commit(root1.index()).has_value());

  // Transition to a new version that drops Y: release root1's hold on Y (count
  // 1 -> 0 -> quarantined by the fence) and allocate Z, which must NOT reuse the
  // quarantined slot. Without the durability-epoch fence Z would land on Y's
  // slot and overwrite the record the durable root still points at.
  arbc::SlotRef<GraphNode> y_ref = root1->left;
  store.release(y_ref);
  REQUIRE(ckpt.slot_fence().pending() == 1);
  arbc::Ref<GraphNode> z = *store.create();
  z->value = 999;
  REQUIRE(z.index() != y_index);

  // Commit the new version (root Z) but crash just before its header msync makes
  // the freeing durable. Recovery must land on the OLD root, which still
  // references Y, and Y must be intact -- not reused.
  TempPath snap;
  inj.arm_before_header(snap.str());
  REQUIRE(ckpt.commit(z.index()).has_value());
  REQUIRE(inj.captured());
  ws.set_syscall_injector(nullptr);

  auto opened = arbc::Checkpointer::open(snap.str());
  REQUIRE(opened.has_value());
  REQUIRE(opened->valid);
  REQUIRE(opened->generation == 1); // the OLD root, not Z's not-yet-durable one
  arbc::Arena a2(*opened->source);
  arbc::RefStore<GraphNode> s2(a2);
  REQUIRE(s2.restore(static_cast<std::uint32_t>(store.store().high_water())).has_value());
  const WalkResult wr = walk(s2, opened->root_index);
  // The durable root still references Y, and Y's record survived (value 42, not
  // overwritten by Z at 999).
  const GraphNode* recovered_root = s2.peek_index(opened->root_index);
  REQUIRE(recovered_root->has_left);
  REQUIRE(recovered_root->left.index() == y_index);
  REQUIRE(s2.peek_index(y_index)->value == 42);
  bool y_reachable = false;
  for (arbc::SlotIndex i : wr.live) {
    if (i == y_index) {
      y_reachable = true;
    }
  }
  REQUIRE(y_reachable);
}

// enforces: 15-memory-model#workspace-io-faults-surface-as-values
TEST_CASE("disk-full syscall failures surface as values and stay recoverable", "[pool]") {
  TempPath path;
  auto src = arbc::WorkspaceFileChunkSource::create(path.str());
  REQUIRE(src.has_value());
  arbc::WorkspaceFileChunkSource& ws = **src;
  arbc::Arena arena(ws);
  arbc::RefStore<GraphNode> store(arena);
  arbc::Checkpointer ckpt(ws, arena);

  // A durable root to recover to after each injected fault.
  arbc::Ref<GraphNode> root = build_graph(store);
  const arbc::SlotIndex root_index = root.index();
  REQUIRE(ckpt.commit(root_index).has_value());

  constexpr std::size_t chunk = 64 * 1024;

  SECTION("ftruncate ENOSPC on chunk growth") {
    ErrnoInjector inj(arbc::WorkspaceSyscall::Ftruncate, ENOSPC);
    ws.set_syscall_injector(&inj);
    auto span = ws.acquire(chunk, alignof(std::max_align_t));
    ws.set_syscall_injector(nullptr);
    REQUIRE(inj.fired());
    REQUIRE_FALSE(span.has_value());
    REQUIRE(span.error() == arbc::PoolError::OutOfMemory);
    REQUIRE(ws.last_error().code == arbc::WorkspaceFileErrc::GrowFailed);
    REQUIRE(ws.last_error().sys_errno == ENOSPC);
  }

  SECTION("mmap EFBIG on chunk growth") {
    ErrnoInjector inj(arbc::WorkspaceSyscall::Mmap, EFBIG);
    ws.set_syscall_injector(&inj);
    auto span = ws.acquire(chunk, alignof(std::max_align_t));
    ws.set_syscall_injector(nullptr);
    REQUIRE(inj.fired());
    REQUIRE_FALSE(span.has_value());
    REQUIRE(ws.last_error().code == arbc::WorkspaceFileErrc::GrowFailed);
    REQUIRE(ws.last_error().sys_errno == EFBIG);
  }

  SECTION("EIO on the commit data msync") {
    arbc::Ref<GraphNode> more = build_graph(store); // dirty the scene
    more->value = 200;
    ErrnoInjector inj(arbc::WorkspaceSyscall::Msync, EIO); // first msync == data msync
    ws.set_syscall_injector(&inj);
    auto result = ckpt.commit(more.index());
    ws.set_syscall_injector(nullptr);
    REQUIRE(inj.fired());
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == arbc::WorkspaceFileErrc::HeaderIoFailed);
    REQUIRE(result.error().sys_errno == EIO);
  }

#if defined(__linux__)
  SECTION("ENOSPC on the deferred hole-punch drain") {
    // The Checkpointer installs a chunk release fence, so `release` defers the
    // punch to the next commit's drain (punch_now -> fallocate).
    auto span = ws.acquire(chunk, alignof(std::max_align_t));
    REQUIRE(span.has_value());
    std::memset(span->base, 0xCD, span->size);
    ws.release(*span); // deferred to the durability fence
    ErrnoInjector inj(arbc::WorkspaceSyscall::Fallocate, ENOSPC);
    ws.set_syscall_injector(&inj);
    REQUIRE(ckpt.commit(root_index).has_value()); // drain -> punch_now -> fallocate injected
    ws.set_syscall_injector(nullptr);
    REQUIRE(inj.fired());
    REQUIRE(ws.last_error().code == arbc::WorkspaceFileErrc::GrowFailed);
    REQUIRE(ws.last_error().sys_errno == ENOSPC);
  }
#endif

  // After any injected fault, an independent reopen still recovers the last
  // durable root (no abort, no corruption). The generation is section-dependent
  // (the hole-punch section commits again), so accept any valid generation and
  // pin the recovered graph (root value 100).
  TempPath recovered;
  copy_file(path.str(), recovered.str());
  assert_recovers(recovered.str(), 0u, 100u,
                  static_cast<std::uint32_t>(store.store().high_water()));
}

// enforces: 15-memory-model#workspace-io-faults-surface-as-values
TEST_CASE("short / truncated / corrupt workspace files surface as values", "[pool]") {
  // A small directory keeps the header region a single page, so truncation
  // boundaries are easy to hit exactly.
  arbc::WorkspaceLayout layout;
  layout.max_chunks = 8;

  TempPath path;
  auto src = arbc::WorkspaceFileChunkSource::create(path.str(), layout);
  REQUIRE(src.has_value());
  arbc::WorkspaceFileChunkSource& ws = **src;
  arbc::Arena arena(ws);
  arbc::RefStore<GraphNode> store(arena);
  arbc::Checkpointer ckpt(ws, arena);
  arbc::Ref<GraphNode> root = build_graph(store);
  const arbc::SlotIndex root_index = root.index();
  const std::uint32_t high_water = static_cast<std::uint32_t>(store.store().high_water());
  REQUIRE(ckpt.commit(root_index).has_value());

  const auto header = arbc::WorkspaceFileChunkSource::read_header(path.str());
  REQUIRE(header.has_value());
  const std::uint64_t data_offset = header->data_offset; // == header_bytes (one page)

  const auto truncate_copy = [&](off_t length) {
    TempPath dst;
    copy_file(path.str(), dst.str());
    REQUIRE(::truncate(dst.str().c_str(), length) == 0);
    return dst;
  };

  SECTION("mid-header truncation -> HeaderIoFailed, never OOB read") {
    TempPath dst = truncate_copy(static_cast<off_t>(sizeof(arbc::WorkspaceHeader)) / 2);
    auto hdr = arbc::WorkspaceFileChunkSource::read_header(dst.str());
    REQUIRE_FALSE(hdr.has_value());
    REQUIRE(hdr.error().code == arbc::WorkspaceFileErrc::HeaderIoFailed);
    auto opened = arbc::Checkpointer::open(dst.str());
    REQUIRE_FALSE(opened.has_value());
    REQUIRE(opened.error().code == arbc::WorkspaceFileErrc::HeaderIoFailed);
  }

  SECTION("mid-directory truncation -> HeaderIoFailed, never mapped past EOF") {
    // File large enough for the fixed header pread but shorter than the mapped
    // header/directory region.
    TempPath dst = truncate_copy(static_cast<off_t>(sizeof(arbc::WorkspaceHeader)) + 32);
    auto opened = arbc::Checkpointer::open(dst.str());
    REQUIRE_FALSE(opened.has_value());
    REQUIRE(opened.error().code == arbc::WorkspaceFileErrc::HeaderIoFailed);
  }

  SECTION("mid-data-chunk truncation -> HeaderIoFailed, never mapped past EOF") {
    // Keep the whole header + directory but cut the live data chunk in half.
    TempPath dst = truncate_copy(static_cast<off_t>(data_offset) + 128);
    auto opened = arbc::Checkpointer::open(dst.str());
    REQUIRE_FALSE(opened.has_value());
    REQUIRE(opened.error().code == arbc::WorkspaceFileErrc::HeaderIoFailed);
  }

  SECTION("clobbered magic -> BadMagic") {
    TempPath dst;
    copy_file(path.str(), dst.str());
    const int fd = ::open(dst.str().c_str(), O_RDWR);
    REQUIRE(fd >= 0);
    const std::uint64_t garbage = 0xDEAD'BEEF'DEAD'BEEFull;
    REQUIRE(::pwrite(fd, &garbage, sizeof(garbage), offsetof(arbc::WorkspaceHeader, magic)) ==
            sizeof(garbage));
    ::close(fd);
    auto opened = arbc::Checkpointer::open(dst.str());
    REQUIRE_FALSE(opened.has_value());
    REQUIRE(opened.error().code == arbc::WorkspaceFileErrc::BadMagic);
  }

  SECTION("clobbered format-major -> UnsupportedFormat") {
    TempPath dst;
    copy_file(path.str(), dst.str());
    const int fd = ::open(dst.str().c_str(), O_RDWR);
    REQUIRE(fd >= 0);
    const std::uint32_t bad_major = arbc::k_workspace_format_major + 99;
    REQUIRE(::pwrite(fd, &bad_major, sizeof(bad_major),
                     offsetof(arbc::WorkspaceHeader, format_major)) == sizeof(bad_major));
    ::close(fd);
    auto opened = arbc::Checkpointer::open(dst.str());
    REQUIRE_FALSE(opened.has_value());
    REQUIRE(opened.error().code == arbc::WorkspaceFileErrc::UnsupportedFormat);
  }

  SECTION("torn inactive root slot -> A/B redundancy still recovers the durable root") {
    // The active slot is written by a single naturally-aligned store (torn-write
    // free); a torn/garbage value can only appear in the INACTIVE slot, and the
    // ordered protocol never makes it durable-and-chosen. Corrupt the inactive
    // slot (B, currently 0) with garbage whose generation cannot outrank the
    // active slot, and confirm recovery still lands on the durable root.
    TempPath dst;
    copy_file(path.str(), dst.str());
    const int fd = ::open(dst.str().c_str(), O_RDWR);
    REQUIRE(fd >= 0);
    const std::uint64_t torn = arbc::encode_root(arbc::WorkspaceRoot{0, 0xFFFF'FFFFu});
    REQUIRE(::pwrite(fd, &torn, sizeof(torn), offsetof(arbc::WorkspaceHeader, root_slot_b)) ==
            sizeof(torn));
    ::close(fd);
    assert_recovers(dst.str(), 1u, 100u, high_water);
  }
}

// --- fork-and-kill faithful sweep (decision (b)) -----------------------------

namespace {

// _exit(0)s the child at the `target`-th (0-based) intercepted syscall, in the
// given phase (before / after the real call). Counts every intercepted syscall
// kind -- the exhaustive faithful model.
class KillInjector final : public arbc::SyscallInjector {
public:
  KillInjector(long target, bool after) : d_target(target), d_after(after) {}

  int before(arbc::WorkspaceSyscall, std::uint64_t, std::size_t) noexcept override {
    const long idx = d_point++;
    if (idx == d_target && !d_after) {
      ::_exit(0);
    }
    d_hit_after = (idx == d_target && d_after);
    return 0;
  }
  void after(arbc::WorkspaceSyscall, std::uint64_t, std::size_t) noexcept override {
    if (d_hit_after) {
      ::_exit(0);
    }
  }

private:
  long d_target;
  bool d_after;
  long d_point{0};
  bool d_hit_after{false};
};

// The child's deterministic workload: G1 (root value 100, 3 slots) committed,
// then G2 (root value 200, 3 slots) committed. Errors _exit with a nonzero code;
// the parent's reopen is the real assertion.
[[noreturn]] void child_workload(const std::string& path, arbc::SyscallInjector& inj) {
  auto src = arbc::WorkspaceFileChunkSource::create(path);
  if (!src) {
    ::_exit(10);
  }
  arbc::WorkspaceFileChunkSource& ws = **src;
  ws.set_syscall_injector(&inj);
  arbc::Arena arena(ws);
  arbc::RefStore<GraphNode> store(arena);
  arbc::Checkpointer ckpt(ws, arena);
  store.store().set_release_fence(&ckpt.slot_fence());

  arbc::Ref<GraphNode> root1 = build_graph(store);
  if (!ckpt.commit(root1.index())) {
    ::_exit(11);
  }
  arbc::Ref<GraphNode> root2 = build_graph(store);
  root2->value = 200;
  if (!ckpt.commit(root2.index())) {
    ::_exit(12);
  }
  ws.set_syscall_injector(nullptr);
  ::_exit(0);
}

// Fork a child that dies at syscall (target, phase); reap it; reopen the real
// file in the parent and assert recovery lands on a consistent root -- never a
// crash or OOB read. Killing the writer mid-syscall leaves the shared page cache
// holding its writes up to that point (doc 15's "editor crash" model), so
// recovery lands on whichever root the ordered protocol had published: the OLD
// root (gen 1, value 100) until the second commit's flip, the NEW root (gen 2,
// value 200) after it -- both consistent.
void fork_kill_and_check(long target, bool after) {
  TempPath path;
  const pid_t pid = ::fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    KillInjector inj(target, after);
    child_workload(path.str(), inj); // [[noreturn]]
  }
  int status = 0;
  REQUIRE(::waitpid(pid, &status, 0) == pid);

  auto opened = arbc::Checkpointer::open(path.str());
  if (!opened.has_value()) {
    return; // a clean error value is an acceptable recovery outcome
  }
  if (!opened->valid) {
    return; // died before the first root became durable -- no durable root yet
  }
  REQUIRE((opened->generation == 1 || opened->generation == 2));
  const std::uint32_t high_water = opened->generation >= 2 ? 6u : 3u;
  const std::uint32_t expected_value = opened->generation >= 2 ? 200u : 100u;
  arbc::Arena a(*opened->source);
  arbc::RefStore<GraphNode> s(a);
  REQUIRE(s.restore(high_water).has_value());
  const WalkResult wr = walk(s, opened->root_index);
  REQUIRE(wr.live.size() == 3);
  REQUIRE(s.peek_index(opened->root_index)->value == expected_value);
}

} // namespace

// enforces: 15-memory-model#checkpoint-recovers-consistent-root
TEST_CASE("fork-and-kill: a bounded per-push subset recovers a consistent root", "[pool]") {
  // A handful of early syscall boundaries (chunk growth + the first commit's
  // ordering). The exhaustive sweep is the nightly job below.
  for (long target = 0; target < 8; ++target) {
    for (bool after : {false, true}) {
      fork_kill_and_check(target, after);
    }
  }
}

// enforces: 15-memory-model#checkpoint-recovers-consistent-root
TEST_CASE("fork-and-kill: exhaustive syscall sweep recovers a consistent root", "[.nightly]") {
  // Exhaustive over the syscall index N: overshooting the actual syscall count
  // simply lets the child run to completion (full recovery), so a generous fixed
  // upper bound needs no separate counting pass. Long-form job (doc 16:102-105).
  for (long target = 0; target < 40; ++target) {
    for (bool after : {false, true}) {
      fork_kill_and_check(target, after);
    }
  }
}

#endif // ARBC_HAS_WORKSPACE_FILES
