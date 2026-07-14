#include <arbc/pool/checkpoint.hpp>
#include <arbc/pool/refs.hpp>
#include <arbc/pool/slot_store.hpp>
#include <arbc/pool/workspace_file.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

// The crash-recovery sweep rides the file-backed workspace. Its body drives the
// platform-neutral fault-injection seam on both platforms, with two death models:
// an in-process durable-snapshot sweep and a real process-kill sweep. The
// process-kill sweep uses fork/_exit on POSIX and a CreateProcess re-exec child
// that ExitProcess-es mid-syscall on Windows (pool.crash_tests_win32). The
// platform surgery lives in-file behind `_WIN32`, matching every sibling port. On
// platforms without workspace files this compiles to just the runtime-query
// sanity check.
TEST_CASE("crash-test harness tracks workspace-file support") {
  REQUIRE(arbc::workspace_files_supported() == (ARBC_HAS_WORKSPACE_FILES != 0));
}

#if ARBC_HAS_WORKSPACE_FILES

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#endif

namespace {

// A self-cleaning unique workspace-file path under the temp dir (mirrors the
// checkpoint / workspace_file tests).
class TempPath {
public:
  TempPath() {
#if defined(_WIN32)
    char dir[MAX_PATH];
    const DWORD n = ::GetTempPathA(MAX_PATH, dir);
    char buf[MAX_PATH];
    // GetTempFileNameA creates the file; create()/open() reopen with CREATE_ALWAYS.
    if (n != 0 && n < static_cast<DWORD>(MAX_PATH) && ::GetTempFileNameA(dir, "crs", 0, buf) != 0) {
      d_path = buf;
    }
#else
    char tmpl[] = "/tmp/arbc_crash_XXXXXX";
    const int fd = ::mkstemp(tmpl);
    if (fd >= 0) {
      ::close(fd);
    }
    d_path = tmpl;
#endif
  }
  ~TempPath() {
    if (!d_path.empty()) {
#if defined(_WIN32)
      ::DeleteFileA(d_path.c_str());
#else
      ::unlink(d_path.c_str());
#endif
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
#if defined(_WIN32)
  REQUIRE(::CopyFileA(src.c_str(), dst.c_str(), FALSE) != 0);
#else
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
#endif
}

// Overwrite `size` bytes at `offset` in an existing file -- the corruption/short-
// file surgery primitive (POSIX pwrite / Windows SetFilePointerEx+WriteFile).
// Runs in test-body context (REQUIRE is safe here, unlike inside the injector).
void write_at(const std::string& path, std::size_t offset, const void* data, std::size_t size) {
#if defined(_WIN32)
  HANDLE h = ::CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  REQUIRE(h != INVALID_HANDLE_VALUE);
  LARGE_INTEGER li;
  li.QuadPart = static_cast<LONGLONG>(offset);
  const BOOL sought = ::SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
  DWORD wrote = 0;
  const BOOL ok = sought != 0 && ::WriteFile(h, data, static_cast<DWORD>(size), &wrote, nullptr);
  ::CloseHandle(h);
  REQUIRE(ok != 0);
  REQUIRE(wrote == size);
#else
  const int fd = ::open(path.c_str(), O_RDWR);
  REQUIRE(fd >= 0);
  const ssize_t wrote = ::pwrite(fd, data, size, static_cast<off_t>(offset));
  ::close(fd);
  REQUIRE(wrote == static_cast<ssize_t>(size));
#endif
}

// Read `size` bytes at `offset` straight out of the file -- the on-disk truth, read
// without going through any mapping. The other half of the surgery kit: a page captured
// here is written back over itself later to model a writeback that never happened.
std::vector<std::uint8_t> read_bytes(const std::string& path, std::uint64_t offset,
                                     std::size_t size) {
  std::vector<std::uint8_t> out(size, 0);
#if defined(_WIN32)
  HANDLE h = ::CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  REQUIRE(h != INVALID_HANDLE_VALUE);
  LARGE_INTEGER li;
  li.QuadPart = static_cast<LONGLONG>(offset);
  const BOOL sought = ::SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
  DWORD got = 0;
  const BOOL ok = sought != 0 && ::ReadFile(h, out.data(), static_cast<DWORD>(size), &got, nullptr);
  ::CloseHandle(h);
  REQUIRE(ok != 0);
  REQUIRE(got == size);
#else
  const int fd = ::open(path.c_str(), O_RDONLY);
  REQUIRE(fd >= 0);
  const ssize_t got = ::pread(fd, out.data(), size, static_cast<off_t>(offset));
  ::close(fd);
  REQUIRE(got == static_cast<ssize_t>(size));
#endif
  return out;
}

// File offset of store-table snapshot `ab` (0 = A, 1 = B): the snapshots are one PAGE
// apart, each page-resident (pool.header_writeback_ordering).
std::uint64_t snapshot_offset(const arbc::WorkspaceHeader& hdr, int ab) {
  return hdr.store_table_offset + static_cast<std::uint64_t>(ab) * hdr.page_size;
}

// Truncate `path` to `length` bytes (POSIX truncate / Windows SetEndOfFile).
void truncate_file(const std::string& path, std::uint64_t length) {
#if defined(_WIN32)
  HANDLE h = ::CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  REQUIRE(h != INVALID_HANDLE_VALUE);
  LARGE_INTEGER li;
  li.QuadPart = static_cast<LONGLONG>(length);
  const BOOL sought = ::SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
  const BOOL ok = sought != 0 && ::SetEndOfFile(h);
  ::CloseHandle(h);
  REQUIRE(ok != 0);
#else
  REQUIRE(::truncate(path.c_str(), static_cast<off_t>(length)) == 0);
#endif
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

#if defined(_WIN32)
  // No-throw slot patch (runs inside the noexcept injector callback, so it cannot
  // use REQUIRE): seek + write one root slot, reporting success as a bool.
  static bool patch_slot(HANDLE h, std::size_t offset, std::uint64_t value) noexcept {
    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(offset);
    if (!::SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) {
      return false;
    }
    DWORD wrote = 0;
    return ::WriteFile(h, &value, sizeof(value), &wrote, nullptr) && wrote == sizeof(value);
  }
#endif

  void capture() {
    if (d_snapshot.empty()) {
      return;
    }
    copy_file(d_path, d_snapshot);
#if defined(_WIN32)
    HANDLE h = ::CreateFileA(d_snapshot.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
      const bool ok = patch_slot(h, offsetof(arbc::WorkspaceHeader, root_slot_a), d_durable_a) &&
                      patch_slot(h, offsetof(arbc::WorkspaceHeader, root_slot_b), d_durable_b);
      ::CloseHandle(h);
      if (!ok) {
        return;
      }
    }
#else
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
#endif
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

// Injects `err` on the `nth` (0-based) occurrence of `target`, passing every
// other syscall through -- the disk-full model. `err` is an errno on POSIX and a
// Win32 error code on Windows (the shim feeds it to SetLastError, so it round-
// trips into `last_error().sys_errno` via GetLastError).
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

// --- two-store variant (pool.workspace_store_directory) ----------------------
//
// The same kill sweep over a workspace backing TWO size-class stores, whose store
// table `commit` now writes into the header right before the root flip. Uses raw
// SlotStores (not RefStore) because what is under test is the storage-level pairing
// of root and store-table snapshot, not a typed graph.

// The colliding size classes: a 288-byte stride at 128 slots/chunk and a 144-byte
// stride at 256 slots/chunk both yield 36864-byte chunks, so chunk byte size cannot
// route them and the arena directory's owner tag must.
constexpr std::size_t k_two_align = alignof(std::max_align_t);
constexpr std::size_t k_node_stride = 288;
constexpr std::uint32_t k_node_bits = 7;
constexpr std::size_t k_record_stride = 144;
constexpr std::uint32_t k_record_bits = 8;

std::uint8_t two_pattern(std::uint32_t tag, std::uint32_t slot, std::size_t offset) {
  return static_cast<std::uint8_t>((tag * 131u) ^ (slot * 17u) ^ (offset * 7u));
}

void two_fill(void* base, std::size_t stride, std::uint32_t tag, std::uint32_t slot) {
  auto* bytes = static_cast<std::uint8_t*>(base);
  for (std::size_t i = 0; i < stride; ++i) {
    bytes[i] = two_pattern(tag, slot, i);
  }
}

// Reopen `snapshot` and assert the RECOVERED ROOT AND THE RECOVERED STORE TABLE ARE
// THE SAME COMMIT'S: every store's chunk set covers its high-water (adopt_snapshot
// refuses otherwise, so a successful open is itself that assertion), the restored
// high-waters are one of the two committed generations' -- never a mix -- and every
// slot below them resolves to its pre-crash bytes. `expected_slots` is the per-store
// high-water of generation 1; generation 2 doubles it.
void assert_two_store_recovers(const std::string& snapshot, std::uint32_t gen1_slots) {
  auto opened = arbc::Checkpointer::open(snapshot);
  if (!opened.has_value()) {
    return; // a clean error value is an acceptable recovery outcome
  }
  if (!opened->valid) {
    return; // died before the first root became durable
  }
  REQUIRE((opened->generation == 1 || opened->generation == 2));
  const std::uint32_t expected = opened->generation == 2 ? 2 * gen1_slots : gen1_slots;

  arbc::WorkspaceFileChunkSource& ws = *opened->source;
  arbc::Arena arena(ws.router());
  arbc::Checkpointer ckpt(ws, arena);
  REQUIRE(ckpt.reserve_restored_all(arena).has_value());

  arbc::SlotStore& nodes = arena.store_for(k_node_stride, k_two_align, k_node_bits);
  arbc::SlotStore& records = arena.store_for(k_record_stride, k_two_align, k_record_bits);
  // The high-water the recovery read belongs to the root it selected: the OLD root
  // pairs with the OLD snapshot, the new root with the new one -- never a mismatch.
  REQUIRE(nodes.high_water() == expected);
  REQUIRE(records.high_water() == expected);

  for (std::uint32_t i = 0; i < expected; ++i) {
    const auto* node = static_cast<const std::uint8_t*>(nodes.resolve(i));
    const auto* record = static_cast<const std::uint8_t*>(records.resolve(i));
    for (std::size_t b = 0; b < k_node_stride; ++b) {
      REQUIRE(node[b] == two_pattern(1, i, b));
    }
    for (std::size_t b = 0; b < k_record_stride; ++b) {
      REQUIRE(record[b] == two_pattern(2, i, b));
    }
  }
}

// Build a durable generation-1 state (gen1_slots per store), then a second batch, and
// commit it with the snapshot injector armed. Mirrors run_second_commit for the
// two-store case. Returns the number of enumerated injection points during commit #2.
long run_second_two_store_commit(long target, bool after, const std::string& snapshot,
                                 std::uint32_t gen1_slots) {
  TempPath path;
  auto src = arbc::WorkspaceFileChunkSource::create(path.str());
  REQUIRE(src.has_value());
  arbc::WorkspaceFileChunkSource& ws = **src;
  arbc::Arena arena(ws.router());
  arbc::SlotStore& nodes = arena.store_for(k_node_stride, k_two_align, k_node_bits);
  arbc::SlotStore& records = arena.store_for(k_record_stride, k_two_align, k_record_bits);
  arbc::Checkpointer ckpt(ws, arena);
  ckpt.register_store(nodes);
  ckpt.register_store(records);

  SnapshotInjector inj(ws, path.str());
  ws.set_syscall_injector(&inj);

  const auto grow = [&](std::uint32_t from, std::uint32_t to) {
    for (std::uint32_t i = from; i < to; ++i) {
      two_fill(nodes.resolve(*nodes.allocate()), k_node_stride, 1, i);
      two_fill(records.resolve(*records.allocate()), k_record_stride, 2, i);
    }
  };
  grow(0, gen1_slots);
  REQUIRE(ckpt.commit(0).has_value()); // generation 1, slot A
  grow(gen1_slots, 2 * gen1_slots);    // in flight: not yet published

  if (snapshot.empty()) {
    inj.count_only();
  } else {
    inj.arm(target, after, snapshot);
  }
  REQUIRE(ckpt.commit(0).has_value()); // generation 2, slot B
  ws.set_syscall_injector(nullptr);
  if (!snapshot.empty()) {
    REQUIRE(inj.captured());
  }
  return inj.points();
}

} // namespace

// enforces: 15-memory-model#store-high-water-durable-with-root
TEST_CASE("two-store kill sweep: the recovered high-water always belongs to the recovered root",
          "[pool]") {
  // 200 slots per store at generation 1, 400 at generation 2 -- so the two generations
  // need DIFFERENT chunk counts in both stores (nodes: 2 -> 4 chunks, records: 1 -> 2).
  // A commit that published the new high-water against the old root would therefore
  // claim chunks the old root's data msync never covered: exactly the corruption the
  // per-root store-table snapshot exists to prevent. Killing at every commit boundary
  // must land on old-root+old-high-water or new-root+new-high-water, never a mix.
  constexpr std::uint32_t gen1_slots = 200;
  const long num_points = run_second_two_store_commit(0, false, {}, gen1_slots);
  REQUIRE(num_points >= 3); // >=1 data msync + the root flip + the header msync

  for (long target = 0; target < num_points; ++target) {
    for (bool after : {false, true}) {
      TempPath snap;
      const long np = run_second_two_store_commit(target, after, snap.str(), gen1_slots);
      REQUIRE(np == num_points); // every re-run enumerates the same boundaries
      assert_two_store_recovers(snap.str(), gen1_slots);
    }
  }
}

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

  // The injected fault constant IS the value that round-trips into
  // `last_error().sys_errno`: an errno on POSIX, a Win32 error code on Windows
  // (the shim feeds `before()`'s return to SetLastError). Assert the exact code
  // both places by injecting and checking the same local constant.
#if defined(_WIN32)
  const int grow_err = ERROR_DISK_FULL;
  const int grow_err2 = ERROR_HANDLE_DISK_FULL;
  const int io_err = ERROR_IO_DEVICE;
#else
  const int grow_err = ENOSPC;
  const int grow_err2 = EFBIG;
  const int io_err = EIO;
#endif

  SECTION("ftruncate disk-full on chunk growth -> GrowFailed") {
    ErrnoInjector inj(arbc::WorkspaceSyscall::Ftruncate, grow_err);
    ws.set_syscall_injector(&inj);
    auto span = ws.acquire(chunk, alignof(std::max_align_t));
    ws.set_syscall_injector(nullptr);
    REQUIRE(inj.fired());
    REQUIRE_FALSE(span.has_value());
    REQUIRE(span.error() == arbc::PoolError::OutOfMemory);
    REQUIRE(ws.last_error().code == arbc::WorkspaceFileErrc::GrowFailed);
    REQUIRE(ws.last_error().sys_errno == grow_err);
  }

  SECTION("mmap disk-full on chunk growth -> GrowFailed") {
    ErrnoInjector inj(arbc::WorkspaceSyscall::Mmap, grow_err2);
    ws.set_syscall_injector(&inj);
    auto span = ws.acquire(chunk, alignof(std::max_align_t));
    ws.set_syscall_injector(nullptr);
    REQUIRE(inj.fired());
    REQUIRE_FALSE(span.has_value());
    REQUIRE(ws.last_error().code == arbc::WorkspaceFileErrc::GrowFailed);
    REQUIRE(ws.last_error().sys_errno == grow_err2);
  }

  SECTION("I/O error on the commit data msync -> HeaderIoFailed") {
    arbc::Ref<GraphNode> more = build_graph(store); // dirty the scene
    more->value = 200;
    ErrnoInjector inj(arbc::WorkspaceSyscall::Msync, io_err); // first msync == data msync
    ws.set_syscall_injector(&inj);
    auto result = ckpt.commit(more.index());
    ws.set_syscall_injector(nullptr);
    REQUIRE(inj.fired());
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == arbc::WorkspaceFileErrc::HeaderIoFailed);
    REQUIRE(result.error().sys_errno == io_err);
  }

#if defined(__linux__) || defined(_WIN32)
  SECTION("disk-full on the deferred hole-punch drain -> GrowFailed") {
    // The Checkpointer installs a chunk release fence, so `release` defers the
    // punch to the next commit's drain (punch_now -> fallocate on Linux,
    // FSCTL_SET_ZERO_DATA on Windows). The injector short-circuits BEFORE the real
    // call, so there is no NTFS/sparse-volume dependency (mmap_backing_win32
    // Decision 4's best-effort punch never runs). The punch failure is best-effort
    // -- it lands in `last_error()`, not the commit's return value.
    auto span = ws.acquire(chunk, alignof(std::max_align_t));
    REQUIRE(span.has_value());
    std::memset(span->base, 0xCD, span->size);
    ws.release(*span); // deferred to the durability fence
    ErrnoInjector inj(arbc::WorkspaceSyscall::Fallocate, grow_err);
    ws.set_syscall_injector(&inj);
    REQUIRE(ckpt.commit(root_index).has_value()); // drain -> punch_now -> fallocate injected
    ws.set_syscall_injector(nullptr);
    REQUIRE(inj.fired());
    REQUIRE(ws.last_error().code == arbc::WorkspaceFileErrc::GrowFailed);
    REQUIRE(ws.last_error().sys_errno == grow_err);
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
  // == header_bytes: one page of header + directory, then a page per store-table
  // snapshot (each is page-resident, pool.header_writeback_ordering).
  const std::uint64_t data_offset = header->data_offset;

  const auto truncate_copy = [&](std::uint64_t length) {
    TempPath dst;
    copy_file(path.str(), dst.str());
    truncate_file(dst.str(), length);
    return dst;
  };

  SECTION("mid-header truncation -> HeaderIoFailed, never OOB read") {
    TempPath dst = truncate_copy(sizeof(arbc::WorkspaceHeader) / 2);
    auto hdr = arbc::WorkspaceFileChunkSource::read_header(dst.str());
    REQUIRE_FALSE(hdr.has_value());
    REQUIRE(hdr.error().code == arbc::WorkspaceFileErrc::HeaderIoFailed);
    auto opened = arbc::Checkpointer::open(dst.str());
    REQUIRE_FALSE(opened.has_value());
    REQUIRE(opened.error().code == arbc::WorkspaceFileErrc::HeaderIoFailed);
  }

  SECTION("mid-directory truncation -> HeaderIoFailed, never mapped past EOF") {
    // File large enough for the fixed header read but shorter than the mapped
    // header/directory region.
    TempPath dst = truncate_copy(sizeof(arbc::WorkspaceHeader) + 32);
    auto opened = arbc::Checkpointer::open(dst.str());
    REQUIRE_FALSE(opened.has_value());
    REQUIRE(opened.error().code == arbc::WorkspaceFileErrc::HeaderIoFailed);
  }

  SECTION("mid-data-chunk truncation -> HeaderIoFailed, never mapped past EOF") {
    // Keep the whole header + directory but cut the live data chunk in half.
    TempPath dst = truncate_copy(data_offset + 128);
    auto opened = arbc::Checkpointer::open(dst.str());
    REQUIRE_FALSE(opened.has_value());
    REQUIRE(opened.error().code == arbc::WorkspaceFileErrc::HeaderIoFailed);
  }

  SECTION("clobbered magic -> BadMagic") {
    TempPath dst;
    copy_file(path.str(), dst.str());
    const std::uint64_t garbage = 0xDEAD'BEEF'DEAD'BEEFull;
    write_at(dst.str(), offsetof(arbc::WorkspaceHeader, magic), &garbage, sizeof(garbage));
    auto opened = arbc::Checkpointer::open(dst.str());
    REQUIRE_FALSE(opened.has_value());
    REQUIRE(opened.error().code == arbc::WorkspaceFileErrc::BadMagic);
  }

  SECTION("clobbered format-major -> UnsupportedFormat") {
    TempPath dst;
    copy_file(path.str(), dst.str());
    const std::uint32_t bad_major = arbc::k_workspace_format_major + 99;
    write_at(dst.str(), offsetof(arbc::WorkspaceHeader, format_major), &bad_major,
             sizeof(bad_major));
    auto opened = arbc::Checkpointer::open(dst.str());
    REQUIRE_FALSE(opened.has_value());
    REQUIRE(opened.error().code == arbc::WorkspaceFileErrc::UnsupportedFormat);
  }

  SECTION("truncation inside store-table snapshot B -> HeaderIoFailed, never a partial table") {
    // The snapshots sit between the chunk directory and the data region, one page each,
    // so a file cut inside snapshot B's page is short of its own header region. Refuse
    // rather than map a half-present table and read a torn row as geometry.
    const std::uint64_t snapshot_b = snapshot_offset(*header, 1);
    REQUIRE(snapshot_b < data_offset);
    TempPath dst = truncate_copy(snapshot_b + sizeof(arbc::WorkspaceStoreEntry) / 2);
    auto opened = arbc::Checkpointer::open(dst.str());
    REQUIRE_FALSE(opened.has_value());
    REQUIRE(opened.error().code == arbc::WorkspaceFileErrc::HeaderIoFailed);
  }

  SECTION("a store table overlapping the chunk directory -> HeaderIoFailed") {
    // A corrupt `store_table_offset` must never let a row read land inside (or past)
    // the mapping's bounds. Point the table at the header itself.
    TempPath dst;
    copy_file(path.str(), dst.str());
    const std::uint64_t overlapping = sizeof(arbc::WorkspaceHeader);
    write_at(dst.str(), offsetof(arbc::WorkspaceHeader, store_table_offset), &overlapping,
             sizeof(overlapping));
    auto opened = arbc::Checkpointer::open(dst.str());
    REQUIRE_FALSE(opened.has_value());
    REQUIRE(opened.error().code == arbc::WorkspaceFileErrc::HeaderIoFailed);
  }

  SECTION("a store table running past the data region -> HeaderIoFailed") {
    TempPath dst;
    copy_file(path.str(), dst.str());
    const std::uint64_t huge = 1u << 20;
    write_at(dst.str(), offsetof(arbc::WorkspaceHeader, max_stores), &huge, sizeof(huge));
    auto opened = arbc::Checkpointer::open(dst.str());
    REQUIRE_FALSE(opened.has_value());
    REQUIRE(opened.error().code == arbc::WorkspaceFileErrc::HeaderIoFailed);
  }

  SECTION("torn inactive root slot -> A/B redundancy still recovers the durable root") {
    // The active slot is written by a single naturally-aligned store (torn-write
    // free); a torn/garbage value can only appear in the INACTIVE slot, and the
    // ordered protocol never makes it durable-and-chosen. Corrupt the inactive
    // slot (B, currently 0) with garbage whose generation cannot outrank the
    // active slot, and confirm recovery still lands on the durable root.
    TempPath dst;
    copy_file(path.str(), dst.str());
    const std::uint64_t torn = arbc::encode_root(arbc::WorkspaceRoot{0, 0xFFFF'FFFFu});
    write_at(dst.str(), offsetof(arbc::WorkspaceHeader, root_slot_b), &torn, sizeof(torn));
    assert_recovers(dst.str(), 1u, 100u, high_water);
  }
}

// enforces: 15-memory-model#torn-header-falls-back-to-matching-root
TEST_CASE("a torn header pairs the root it recovers with the snapshot that root wrote", "[pool]") {
  // THE HOLE THIS TASK CLOSES. One `sync_header()` msyncs the whole header region, but
  // the root slots live on page 0 and each store-table snapshot on a page of its own,
  // and the kernel orders their writeback NOT AT ALL. A crash inside that msync can
  // therefore persist the NEW ROOT beside the STALE SNAPSHOT -- a state no kill sweep
  // can manufacture, because a kill only lands on whole-syscall boundaries. So the
  // partial writeback is modelled by file surgery on a copy: capture a snapshot page
  // before the commit that writes it, then paste it back over the committed file. That
  // is byte-exactly "the root page landed, the snapshot page did not".
  //
  // Against the pre-fix code the torn image below opens on the NEW root while restoring
  // the OLD high-water: every chunk above it is treated as post-checkpoint garbage and
  // hole-punched -- under the very root whose records live there. The generation stamp
  // makes the pairing checkable, so the new root is skipped and the older, coherent one
  // is resumed instead.
  constexpr std::uint32_t gen1_slots = 200; // nodes: 2 chunks, records: 1 -- gen 2 doubles both
  constexpr arbc::SlotIndex k_gen1_root = 7;
  constexpr arbc::SlotIndex k_gen2_root = 301; // above gen 1's high-water: a mis-pairing shows

  TempPath path;
  auto src = arbc::WorkspaceFileChunkSource::create(path.str());
  REQUIRE(src.has_value());
  arbc::WorkspaceFileChunkSource& ws = **src;
  arbc::Arena arena(ws.router());
  arbc::SlotStore& nodes = arena.store_for(k_node_stride, k_two_align, k_node_bits);
  arbc::SlotStore& records = arena.store_for(k_record_stride, k_two_align, k_record_bits);
  arbc::Checkpointer ckpt(ws, arena);
  ckpt.register_store(nodes);
  ckpt.register_store(records);

  const auto grow = [&](std::uint32_t from, std::uint32_t to) {
    for (std::uint32_t i = from; i < to; ++i) {
      two_fill(nodes.resolve(*nodes.allocate()), k_node_stride, 1, i);
      two_fill(records.resolve(*records.allocate()), k_record_stride, 2, i);
    }
  };

  const auto header = arbc::WorkspaceFileChunkSource::read_header(path.str());
  REQUIRE(header.has_value());
  const std::size_t page = static_cast<std::size_t>(header->page_size);
  // Both snapshot pages as they are BEFORE any commit writes them: stamp 0, identity
  // rows, zero high-waters. Snapshot A's next writer is commit 1 and snapshot B's is
  // commit 2, so this one capture is each page's pre-commit durable content.
  const std::vector<std::uint8_t> stale_a =
      read_bytes(path.str(), snapshot_offset(*header, 0), page);
  const std::vector<std::uint8_t> stale_b =
      read_bytes(path.str(), snapshot_offset(*header, 1), page);

  grow(0, gen1_slots);
  REQUIRE(ckpt.commit(k_gen1_root).has_value()); // generation 1: root slot A, snapshot A
  grow(gen1_slots, 2 * gen1_slots);
  REQUIRE(ckpt.commit(k_gen2_root).has_value()); // generation 2: root slot B, snapshot B

  // Reopen `image`, assert it resumes generation 1 on root A with generation 1's
  // high-waters, and that every record that root reaches still resolves -- i.e. the
  // recovery punched nothing root A references.
  const auto assert_recovers_generation_1 = [&](const std::string& image) {
    auto opened = arbc::Checkpointer::open(image);
    REQUIRE(opened.has_value());
    REQUIRE(opened->valid);
    REQUIRE(opened->generation == 1);
    REQUIRE(opened->root_index == k_gen1_root);

    arbc::WorkspaceFileChunkSource& ws2 = *opened->source;
    arbc::Arena arena2(ws2.router());
    arbc::Checkpointer ckpt2(ws2, arena2);
    REQUIRE(ckpt2.reserve_restored_all(arena2).has_value());
    arbc::SlotStore& nodes2 = arena2.store_for(k_node_stride, k_two_align, k_node_bits);
    arbc::SlotStore& records2 = arena2.store_for(k_record_stride, k_two_align, k_record_bits);
    REQUIRE(nodes2.high_water() == gen1_slots);
    REQUIRE(records2.high_water() == gen1_slots);
    for (std::uint32_t i = 0; i < gen1_slots; ++i) {
      const auto* node = static_cast<const std::uint8_t*>(nodes2.resolve(i));
      const auto* record = static_cast<const std::uint8_t*>(records2.resolve(i));
      for (std::size_t b = 0; b < k_node_stride; ++b) {
        REQUIRE(node[b] == two_pattern(1, i, b));
      }
      for (std::size_t b = 0; b < k_record_stride; ++b) {
        REQUIRE(record[b] == two_pattern(2, i, b));
      }
    }
  };

  SECTION("the untorn file recovers generation 2 -- the control") {
    // Without surgery the same file opens on the new root with the new high-waters, so
    // what the sections below observe is the tear, not a broken commit.
    TempPath dst;
    copy_file(path.str(), dst.str());
    auto opened = arbc::Checkpointer::open(dst.str());
    REQUIRE(opened.has_value());
    REQUIRE(opened->generation == 2);
    REQUIRE(opened->root_index == k_gen2_root);
  }

  SECTION("root page landed, snapshot page did not -> the older, matching root") {
    TempPath dst;
    copy_file(path.str(), dst.str());
    write_at(dst.str(), static_cast<std::size_t>(snapshot_offset(*header, 1)), stale_b.data(),
             stale_b.size());
    // Root B still says generation 2; snapshot B is stamped 0. The pair is refused and
    // root A -- generation 1, stamped 1 -- is resumed with ITS high-waters, intact.
    assert_recovers_generation_1(dst.str());
  }

  SECTION("snapshot page landed, root page did not -> the older root too") {
    // The mirror tear: already safe (the old root's own snapshot is untouched by a
    // commit that only ever writes the INACTIVE one), now pinned so it stays safe.
    TempPath dst;
    copy_file(path.str(), dst.str());
    const std::uint64_t never_written = 0;
    write_at(dst.str(), offsetof(arbc::WorkspaceHeader, root_slot_b), &never_written,
             sizeof(never_written));
    assert_recovers_generation_1(dst.str());
  }

  SECTION("neither snapshot page landed -> a refusal, never a silent mis-restore") {
    // Both roots durable, both snapshot pages rolled back: no root owns the snapshot
    // beside it, so A/B redundancy has nothing coherent left to fall back to. The one
    // thing recovery must not do is pick one anyway and under-restore it.
    TempPath dst;
    copy_file(path.str(), dst.str());
    write_at(dst.str(), static_cast<std::size_t>(snapshot_offset(*header, 0)), stale_a.data(),
             stale_a.size());
    write_at(dst.str(), static_cast<std::size_t>(snapshot_offset(*header, 1)), stale_b.data(),
             stale_b.size());
    auto opened = arbc::Checkpointer::open(dst.str());
    REQUIRE_FALSE(opened.has_value());
    REQUIRE(opened.error().code == arbc::WorkspaceFileErrc::StoreDirectoryInconsistent);
  }
}

// --- faithful process-kill sweep (decision (b) / Decision 3) -----------------

namespace {

// Terminate the current process at exactly the injected syscall, running no C++
// destructors -- the faithful mid-syscall death. POSIX: _exit(2). Windows:
// ExitProcess, the direct analog (likewise skipping atexit / stack unwinding), so
// the OS keeps the mapped-file page cache written up to the kill point.
// GCOV_EXCL_START -- runs only in the crash child (fork on POSIX, re-exec on
// Windows), which terminates without flushing gcov; the coverage lane cannot
// observe it even though every kill-and-recover case drives it.
[[noreturn]] void exit_child(int code) noexcept {
#if defined(_WIN32)
  ::ExitProcess(static_cast<UINT>(code));
#else
  ::_exit(code);
#endif
}
// GCOV_EXCL_STOP

// Kills the child at the `target`-th (0-based) intercepted syscall, in the given
// phase (before / after the real call). Counts every intercepted syscall kind --
// the exhaustive faithful model.
class KillInjector final : public arbc::SyscallInjector {
public:
  KillInjector(long target, bool after) : d_target(target), d_after(after) {}

  int before(arbc::WorkspaceSyscall, std::uint64_t, std::size_t) noexcept override {
    const long idx = d_point++;
    if (idx == d_target && !d_after) {
      exit_child(0); // GCOV_EXCL_LINE: crash-child only (see exit_child)
    }
    d_hit_after = (idx == d_target && d_after);
    return 0;
  }
  void after(arbc::WorkspaceSyscall, std::uint64_t, std::size_t) noexcept override {
    if (d_hit_after) {
      exit_child(0); // GCOV_EXCL_LINE: crash-child only (see exit_child)
    }
  }

private:
  long d_target;
  bool d_after;
  long d_point{0};
  bool d_hit_after{false};
};

// The child's deterministic workload: G1 (root value 100, 3 slots) committed,
// then G2 (root value 200, 3 slots) committed. Errors exit with a nonzero code;
// the parent's reopen is the real assertion. Runs no C++ destructors on the kill
// path (exit_child), so the crash is faithful.
// GCOV_EXCL_START -- executes only inside the crash child (see exit_child); its
// exit points self-kill without flushing gcov, so the coverage lane never sees
// them though the [pool] kill-and-recover sweep drives this workload.
[[noreturn]] void child_workload(const std::string& path, arbc::SyscallInjector& inj) {
  auto src = arbc::WorkspaceFileChunkSource::create(path);
  if (!src) {
    exit_child(10);
  }
  arbc::WorkspaceFileChunkSource& ws = **src;
  ws.set_syscall_injector(&inj);
  arbc::Arena arena(ws);
  arbc::RefStore<GraphNode> store(arena);
  arbc::Checkpointer ckpt(ws, arena);
  store.store().set_release_fence(&ckpt.slot_fence());

  arbc::Ref<GraphNode> root1 = build_graph(store);
  if (!ckpt.commit(root1.index())) {
    exit_child(11);
  }
  arbc::Ref<GraphNode> root2 = build_graph(store);
  root2->value = 200;
  if (!ckpt.commit(root2.index())) {
    exit_child(12);
  }
  ws.set_syscall_injector(nullptr);
  exit_child(0);
}
// GCOV_EXCL_STOP

#if defined(_WIN32)
// Re-exec this test binary in crash-child mode (Decision 3): run the hidden
// [.crashchild] test with (path,target,phase) passed through the environment, and
// wait for it to self-kill (or run to completion when N overshoots the syscall
// count). Windows has no fork, so the child cannot inherit the parent's in-memory
// workload -- it rebuilds it deterministically from scratch. The child's exit code
// is unused; the parent's reopen is the real assertion.
void spawn_crash_child(const std::string& path, long target, bool after) {
  char exe[MAX_PATH];
  const DWORD n = ::GetModuleFileNameA(nullptr, exe, static_cast<DWORD>(sizeof(exe)));
  REQUIRE(n > 0);
  REQUIRE(n < static_cast<DWORD>(sizeof(exe)));

  REQUIRE(::SetEnvironmentVariableA("CRASH_TESTS_CHILD", "1") != 0);
  REQUIRE(::SetEnvironmentVariableA("CRASH_TESTS_CHILD_PATH", path.c_str()) != 0);
  REQUIRE(::SetEnvironmentVariableA("CRASH_TESTS_CHILD_TARGET", std::to_string(target).c_str()) !=
          0);
  REQUIRE(::SetEnvironmentVariableA("CRASH_TESTS_CHILD_PHASE", after ? "1" : "0") != 0);

  // argv: "<exe>" "[.crashchild]" -- Catch2 runs only the hidden re-exec child.
  std::string cmd = "\"";
  cmd += exe;
  cmd += "\" \"[.crashchild]\"";
  std::vector<char> mutable_cmd(cmd.begin(), cmd.end());
  mutable_cmd.push_back('\0');

  STARTUPINFOA si;
  std::memset(&si, 0, sizeof(si));
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi;
  std::memset(&pi, 0, sizeof(pi));
  const BOOL ok = ::CreateProcessA(exe, mutable_cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                                   nullptr, &si, &pi);
  REQUIRE(ok != 0);
  ::WaitForSingleObject(pi.hProcess, INFINITE);
  ::CloseHandle(pi.hProcess);
  ::CloseHandle(pi.hThread);

  ::SetEnvironmentVariableA("CRASH_TESTS_CHILD", nullptr);
}
#endif

// Kill a child that dies at syscall (target, phase); reap it; reopen the real file
// in the parent and assert recovery lands on a consistent root -- never a crash or
// OOB read. POSIX forks the child in-process; Windows -- which has no fork --
// CreateProcess-es this binary in re-exec child mode (Decision 3). Either way,
// killing the writer mid-syscall leaves the OS page cache holding its writes up to
// that point (doc 15's "editor crash" model), so recovery lands on whichever root
// the ordered protocol had published: the OLD root (gen 1, value 100) until the
// second commit's flip, the NEW root (gen 2, value 200) after it -- both
// consistent.
void kill_and_check(long target, bool after) {
  TempPath path;
#if defined(_WIN32)
  spawn_crash_child(path.str(), target, after);
#else
  const pid_t pid = ::fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    KillInjector inj(target, after);
    child_workload(path.str(), inj); // [[noreturn]]
  }
  int status = 0;
  REQUIRE(::waitpid(pid, &status, 0) == pid);
#endif

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

#if defined(_WIN32)
// The re-exec crash child (Decision 3). Selected by the [.crashchild] tag when the
// CreateProcess parent spawns this binary; hidden from every normal run. It reads
// (path,target,phase) from the environment and runs the deterministic workload
// under a KillInjector that ExitProcess-es at syscall N -- the faithful mid-
// syscall death (no C++ unwinding), the direct analog of the POSIX fork child's
// _exit. Guards on the environment so an accidental direct invocation is a no-op.
TEST_CASE("crash-recovery re-exec child self-kills at a syscall boundary", "[.crashchild]") {
  const char* path = std::getenv("CRASH_TESTS_CHILD_PATH");
  if (path == nullptr) {
    return; // not a re-exec child invocation -- nothing to do
  }
  const char* target = std::getenv("CRASH_TESTS_CHILD_TARGET");
  const char* phase = std::getenv("CRASH_TESTS_CHILD_PHASE");
  REQUIRE(target != nullptr);
  REQUIRE(phase != nullptr);
  KillInjector inj(std::strtol(target, nullptr, 10), std::strcmp(phase, "1") == 0);
  child_workload(path, inj); // [[noreturn]]: exit_child at syscall N or on completion
}
#endif

// enforces: 15-memory-model#checkpoint-recovers-consistent-root
TEST_CASE("kill-and-recover: a bounded per-push subset recovers a consistent root", "[pool]") {
  // A handful of early syscall boundaries (chunk growth + the first commit's
  // ordering). The exhaustive sweep is the nightly job below.
  for (long target = 0; target < 8; ++target) {
    for (bool after : {false, true}) {
      kill_and_check(target, after);
    }
  }
}

// GCOV_EXCL_START -- [.nightly] hidden case, not selected by the fast coverage
// lane (ctest runs the default set only); the [pool] bounded subset above is the
// per-push coverage.
// enforces: 15-memory-model#checkpoint-recovers-consistent-root
TEST_CASE("kill-and-recover: exhaustive syscall sweep recovers a consistent root", "[.nightly]") {
  // Exhaustive over the syscall index N: overshooting the actual syscall count
  // simply lets the child run to completion (full recovery), so a generous fixed
  // upper bound needs no separate counting pass. Long-form job (doc 16:102-105).
  // Windows spawns one re-exec process per index (deliberately: no fork), so the
  // full breadth is nightly-only -- the bounded per-push subset above is the
  // fast-lane coverage.
  for (long target = 0; target < 40; ++target) {
    for (bool after : {false, true}) {
      kill_and_check(target, after);
    }
  }
}
// GCOV_EXCL_STOP

#endif // ARBC_HAS_WORKSPACE_FILES
