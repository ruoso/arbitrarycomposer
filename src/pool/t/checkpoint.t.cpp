#include <arbc/pool/checkpoint.hpp>
#include <arbc/pool/reclamation.hpp>
#include <arbc/pool/refs.hpp>
#include <arbc/pool/slot_store.hpp>
#include <arbc/pool/workspace_file.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// The checkpoint protocol rides the file-backed workspace and is validated on both
// POSIX (mmap/msync/mprotect + signal/setjmp) and Windows (MapViewOfFile/
// FlushViewOfFile/VirtualProtect + SEH). The platform leaves branch on `_WIN32`;
// everything above the capability gate runs unconditionally. On platforms without
// workspace files the body compiles out with just the runtime-query check.
TEST_CASE("checkpoint support tracks workspace-file support") {
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
#include <unistd.h>

#include <csetjmp>
#include <csignal>
#endif

#include <atomic>
#include <thread>

namespace {

// A self-cleaning unique workspace-file path under the temp dir (mirrors the
// workspace_file tests).
class TempPath {
public:
  TempPath() {
#if defined(_WIN32)
    char dir[MAX_PATH];
    const DWORD n = ::GetTempPathA(MAX_PATH, dir);
    char buf[MAX_PATH];
    // GetTempFileNameA creates the file; create()/open() reopen with CREATE_ALWAYS.
    if (n != 0 && n < static_cast<DWORD>(MAX_PATH) && ::GetTempFileNameA(dir, "ckp", 0, buf) != 0) {
      d_path = buf;
    }
#else
    char tmpl[] = "/tmp/arbc_ckpt_XXXXXX";
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
  const std::string& str() const { return d_path; }

private:
  std::string d_path;
};

// A test-local record graph standing in for the model layer (doc 14 node types
// are L2; pool must not depend up). Trivially destructible, standard-layout, and
// pointer-free: children are index-only SlotRefs, the only in-record reference
// form (doc 15 position independence).
struct GraphNode {
  std::uint32_t value = 0;
  arbc::SlotRef<GraphNode> left{};
  arbc::SlotRef<GraphNode> right{};
  bool has_left = false;
  bool has_right = false;
};

// Build the canonical little graph: root(100) -> {left(1), right(2)}. Each child
// is retained by the record (holder-holds-a-count), and the transient child Refs
// are dropped so every node ends at count 1 (the root's count is the external
// pin the caller keeps). Returns the root's owning Ref.
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
  return root; // left/right transient Refs drop here: children settle at count 1
}

// A reachability walk over the reopened store (the mechanism the model drives on
// document open). Reads records by raw storage index -- generations are anonymous
// and reset on open, so the walk must not assert them. Rebuilds counts (each
// in-record edge is one count, plus one for the durable root) and returns the
// live set in discovery order.
struct WalkResult {
  std::vector<arbc::SlotIndex> live;
  std::vector<std::uint32_t> values; // parallel to `live`
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
  counts[root_index] += 1; // the external/root pin
  while (!stack.empty()) {
    const arbc::SlotIndex i = stack.back();
    stack.pop_back();
    if (seen[i]) {
      continue;
    }
    seen[i] = 1;
    result.live.push_back(i);
    result.values.push_back(store.peek_index(i)->value);
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

// On-disk allocated size in bytes: st_blocks*512 on POSIX, GetCompressedFileSize on
// Windows (both drop when a sparse chunk is hole-punched). Mirrors the sibling
// workspace_file.t.cpp helper.
std::uint64_t allocated_size(const std::string& path) {
#if defined(_WIN32)
  DWORD high = 0;
  const DWORD low = ::GetCompressedFileSizeA(path.c_str(), &high);
  REQUIRE(low != INVALID_FILE_SIZE);
  return (static_cast<std::uint64_t>(high) << 32) | low;
#else
  struct stat st{};
  REQUIRE(::stat(path.c_str(), &st) == 0);
  return static_cast<std::uint64_t>(st.st_blocks) * 512u;
#endif
}

// Byte-copy the workspace file so recovery runs against an INDEPENDENT file, as
// a real recovery process would (its own fd + mappings). This mirrors a crash:
// the copy captures the on-disk state the last commit msync'd, and the recovery
// arena's teardown (which hole-punches its chunks) never touches the writer's
// live file. Copying the msync'd bytes is exactly what a post-crash reopen sees.
void copy_file(const std::string& src, const std::string& dst) {
#if defined(_WIN32)
  // A single-call independent-file copy: recovery runs against its own file with
  // its own handle + mappings, exactly as the POSIX byte-copy arm below.
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
  REQUIRE(n == 0); // clean EOF
  ::close(in);
  ::close(out);
#endif
}

} // namespace

TEST_CASE("commit round-trips: open + a test-local walk resolves the same graph") {
  TempPath path;
  // The writer stays alive across the reopen: a crash leaves the file's msync'd
  // state intact, so recovery reads the committed data (a clean teardown would
  // hole-punch the live data chunks — mmap_backing's release).
  auto source = arbc::WorkspaceFileChunkSource::create(path.str());
  REQUIRE(source.has_value());
  arbc::Arena arena(**source);
  arbc::RefStore<GraphNode> store(arena);
  arbc::Checkpointer ckpt(**source, arena);

  arbc::Ref<GraphNode> root = build_graph(store);
  const arbc::SlotIndex root_index = root.index();
  const std::uint32_t high_water = static_cast<std::uint32_t>(store.store().high_water());

  REQUIRE(ckpt.commit(root_index).has_value());
  REQUIRE(ckpt.generation() == 1);

  // read_header sees the active root slot advanced, the other still zero.
  {
    auto header = arbc::WorkspaceFileChunkSource::read_header(path.str());
    REQUIRE(header.has_value());
    const arbc::WorkspaceRoot a = arbc::decode_root(header->root_slot_a);
    const arbc::WorkspaceRoot b = arbc::decode_root(header->root_slot_b);
    REQUIRE(a.generation == 1);
    REQUIRE(a.root_index == root_index);
    REQUIRE(b.generation == 0); // the other slot is untouched
  }

  // Recovery in a fresh arena against an independent copy (a crash reopen).
  TempPath recovered;
  copy_file(path.str(), recovered.str());
  auto opened = arbc::Checkpointer::open(recovered.str());
  REQUIRE(opened.has_value());
  REQUIRE(opened->valid);
  REQUIRE(opened->generation == 1);
  REQUIRE(opened->root_index == root_index);

  arbc::Arena arena2(*opened->source);
  arbc::RefStore<GraphNode> store2(arena2);
  arbc::Checkpointer ckpt2(*opened->source, arena2);
  REQUIRE(store2.restore(high_water).has_value());

  const WalkResult result = walk(store2, opened->root_index);
  REQUIRE(result.live.size() == 3);
  REQUIRE(store2.peek_index(opened->root_index)->value == 100);
  const GraphNode* root_node = store2.peek_index(opened->root_index);
  REQUIRE(store2.peek_index(root_node->left.index())->value == 1);
  REQUIRE(store2.peek_index(root_node->right.index())->value == 2);
}

// enforces: 15-memory-model#checkpoint-recovers-consistent-root
TEST_CASE("recovery lands on the old root before the header sync and the new root after") {
  TempPath path;
  auto source = arbc::WorkspaceFileChunkSource::create(path.str());
  REQUIRE(source.has_value());
  arbc::Arena arena(**source);
  arbc::RefStore<GraphNode> store(arena);
  arbc::Checkpointer ckpt(**source, arena);

  // Publish graph G1 and make it durable (generation 1, slot A).
  arbc::Ref<GraphNode> root1 = build_graph(store);
  const arbc::SlotIndex root1_index = root1.index();
  REQUIRE(ckpt.commit(root1_index).has_value());

  // Build a second, DISTINCT graph G2 in the same arena. Immutability means G1's
  // records are never overwritten, so the durable root still resolves G1 until a
  // new commit publishes G2. Before that commit's header sync, recovery of the
  // file lands on the OLD root (G1).
  arbc::Ref<GraphNode> root2 = build_graph(store);
  root2->value = 200;
  const arbc::SlotIndex root2_index = root2.index();

  {
    TempPath pre_copy;
    copy_file(path.str(), pre_copy.str());
    auto pre = arbc::Checkpointer::open(pre_copy.str());
    REQUIRE(pre.has_value());
    REQUIRE(pre->generation == 1);
    REQUIRE(pre->root_index == root1_index); // old root: consistent G1
    arbc::Arena a(*pre->source);
    arbc::RefStore<GraphNode> s(a);
    REQUIRE(s.restore(static_cast<std::uint32_t>(store.store().high_water())).has_value());
    walk(s, pre->root_index);
    REQUIRE(s.peek_index(pre->root_index)->value == 100);
  }

  // Publish G2 (generation 2, slot B) and make it durable. After the header sync,
  // recovery lands on the NEW root (G2). The A/B slots alternate: A=gen1, B=gen2.
  REQUIRE(ckpt.commit(root2_index).has_value());
  REQUIRE(ckpt.generation() == 2);

  auto header = arbc::WorkspaceFileChunkSource::read_header(path.str());
  REQUIRE(header.has_value());
  REQUIRE(arbc::decode_root(header->root_slot_a).generation == 1);
  REQUIRE(arbc::decode_root(header->root_slot_b).generation == 2);

  TempPath post_copy;
  copy_file(path.str(), post_copy.str());
  auto post = arbc::Checkpointer::open(post_copy.str());
  REQUIRE(post.has_value());
  REQUIRE(post->generation == 2);
  REQUIRE(post->root_index == root2_index); // new root: consistent G2
  arbc::Arena a(*post->source);
  arbc::RefStore<GraphNode> s(a);
  REQUIRE(s.restore(static_cast<std::uint32_t>(store.store().high_water())).has_value());
  walk(s, post->root_index);
  REQUIRE(s.peek_index(post->root_index)->value == 200);
}

TEST_CASE("recovery yields counts-at-zero + empty free lists, then finalize rebuilds them") {
  TempPath path;
  auto source = arbc::WorkspaceFileChunkSource::create(path.str());
  REQUIRE(source.has_value());
  arbc::Arena arena(**source);
  arbc::RefStore<GraphNode> store(arena);
  arbc::Checkpointer ckpt(**source, arena);
  arbc::Ref<GraphNode> root = build_graph(store);
  const arbc::SlotIndex root_index = root.index();
  const arbc::SlotIndex left_index = root->left.index();
  const arbc::SlotIndex right_index = root->right.index();
  const std::uint32_t high_water = static_cast<std::uint32_t>(store.store().high_water());
  REQUIRE(ckpt.commit(root_index).has_value());

  TempPath recovered;
  copy_file(path.str(), recovered.str());
  auto opened = arbc::Checkpointer::open(recovered.str());
  REQUIRE(opened.has_value());
  arbc::Arena arena2(*opened->source);
  arbc::RefStore<GraphNode> store2(arena2);
  arbc::Checkpointer ckpt2(*opened->source, arena2);
  REQUIRE(store2.restore(high_water).has_value());

  // Rebuild-in-progress: every count is zero and the free list is empty.
  REQUIRE(store2.count_index(root_index) == 0);
  REQUIRE(store2.count_index(left_index) == 0);
  REQUIRE(store2.count_index(right_index) == 0);
  REQUIRE(store2.store().slots_live() == 0);
  REQUIRE(store2.store().free_slots() == 0);

  const WalkResult result = walk(store2, opened->root_index);
  ckpt2.finalize_open(store2.store(), result.live);

  // Live counts match the pre-crash graph (each node at count 1) and the free
  // list is the below-high-water complement of the live set (here: empty, all
  // three slots are live).
  REQUIRE(store2.count_index(root_index) == 1);
  REQUIRE(store2.count_index(left_index) == 1);
  REQUIRE(store2.count_index(right_index) == 1);
  REQUIRE(store2.store().slots_live() == 3);
  REQUIRE(store2.store().free_slots() == 0);
}

TEST_CASE("recovery free list is the below-high-water complement when a slot was freed") {
  TempPath path;
  auto source = arbc::WorkspaceFileChunkSource::create(path.str());
  REQUIRE(source.has_value());
  arbc::Arena arena(**source);
  arbc::RefStore<GraphNode> store(arena);
  arbc::Checkpointer ckpt(**source, arena);
  store.store().set_release_fence(&ckpt.slot_fence());

  // Allocate a throwaway node (grows the high-water) that is NOT part of the
  // committed graph, then build + commit the real graph. The freed scratch slot
  // is quarantined, so the graph keeps building above it.
  {
    arbc::Ref<GraphNode> scratch = *store.create();
  }
  arbc::Ref<GraphNode> root = build_graph(store);
  const arbc::SlotIndex root_index = root.index();
  const std::uint32_t high_water = static_cast<std::uint32_t>(store.store().high_water());
  REQUIRE(high_water == 4); // scratch(0) + left(1) + right(2) + root(3)
  REQUIRE(ckpt.commit(root_index).has_value());

  TempPath recovered;
  copy_file(path.str(), recovered.str());
  auto opened = arbc::Checkpointer::open(recovered.str());
  REQUIRE(opened.has_value());
  arbc::Arena arena2(*opened->source);
  arbc::RefStore<GraphNode> store2(arena2);
  arbc::Checkpointer ckpt2(*opened->source, arena2);
  REQUIRE(store2.restore(high_water).has_value());

  const WalkResult result = walk(store2, opened->root_index);
  REQUIRE(result.live.size() == 3); // scratch is unreachable from the root
  ckpt2.finalize_open(store2.store(), result.live);

  // The one below-high-water hole (the unreachable scratch slot) is on the free
  // list, so the next allocation reuses it rather than growing.
  REQUIRE(store2.store().slots_live() == 3);
  REQUIRE(store2.store().free_slots() == 1);
}

TEST_CASE("A/B root slots alternate and the newer generation wins on open") {
  TempPath path;
  auto source = arbc::WorkspaceFileChunkSource::create(path.str());
  REQUIRE(source.has_value());
  arbc::Arena arena(**source);
  arbc::RefStore<GraphNode> store(arena);
  arbc::Checkpointer ckpt(**source, arena);

  arbc::Ref<GraphNode> first = *store.create();
  first->value = 11;
  arbc::Ref<GraphNode> second = *store.create();
  second->value = 22;

  REQUIRE(ckpt.commit(first.index()).has_value());  // gen 1 -> slot A
  REQUIRE(ckpt.commit(second.index()).has_value()); // gen 2 -> slot B

  auto header = arbc::WorkspaceFileChunkSource::read_header(path.str());
  REQUIRE(header.has_value());
  const arbc::WorkspaceRoot a = arbc::decode_root(header->root_slot_a);
  const arbc::WorkspaceRoot b = arbc::decode_root(header->root_slot_b);
  REQUIRE(a.generation == 1); // two successive commits wrote different
  REQUIRE(b.generation == 2); // slots
  REQUIRE(a.root_index == first.index());
  REQUIRE(b.root_index == second.index());

  auto opened = arbc::Checkpointer::open(path.str());
  REQUIRE(opened.has_value());
  REQUIRE(opened->generation == 2); // the newer generation wins
  REQUIRE(opened->root_index == second.index());
}

// enforces: 15-memory-model#freed-slot-quarantined-until-durable
TEST_CASE("a freed slot is quarantined until a commit makes the freeing durable") {
  TempPath path;
  auto source = arbc::WorkspaceFileChunkSource::create(path.str());
  REQUIRE(source.has_value());
  arbc::Arena arena(**source);
  arbc::RefStore<GraphNode> store(arena);
  arbc::Checkpointer ckpt(**source, arena);
  store.store().set_release_fence(&ckpt.slot_fence());

  // Allocate a slot, capture its index, then free it. With the fence installed
  // the slot does NOT return to the free list.
  arbc::SlotIndex freed = 0;
  {
    arbc::Ref<GraphNode> a = *store.create();
    a->value = 7;
    freed = a.index();
  }
  REQUIRE(store.store().free_slots() == 0); // quarantined, not free
  REQUIRE(ckpt.slot_fence().pending() == 1);

  // The next allocation does NOT reuse the quarantined slot.
  arbc::Ref<GraphNode> keep = *store.create();
  REQUIRE(keep.index() != freed);
  const arbc::SlotIndex keep_index = keep.index();

  // A commit that makes the freeing durable releases the slot. free_now returns
  // it to the writer's THREAD-LOCAL pool (pool.free_pools), so the global-only
  // free_slots() stays 0; reuse below proves the slot is back and usable.
  REQUIRE(ckpt.commit(keep_index).has_value());
  REQUIRE(ckpt.slot_fence().pending() == 0);
  REQUIRE(store.store().free_slots() == 0); // in the writer's local pool, not global

  // Now it is reusable.
  arbc::Ref<GraphNode> reused = *store.create();
  REQUIRE(reused.index() == freed);
}

// enforces: 15-memory-model#freed-slot-quarantined-until-durable
TEST_CASE("an emptied chunk is not hole-punched until the emptying is durable") {
#if defined(__linux__) || defined(_WIN32)
  // Deferred hole-punch on a sparse-capable volume: Linux `fallocate(PUNCH_HOLE)`
  // and Windows `FSCTL_SET_ZERO_DATA` on an NTFS sparse file. Guarded to those
  // sparse-capable volumes, as macOS / non-NTFS keeps the logical bytes.
  TempPath path;
  auto source = arbc::WorkspaceFileChunkSource::create(path.str());
  REQUIRE(source.has_value());
  arbc::WorkspaceFileChunkSource& ws = **source;
  arbc::Arena arena(ws);
  arbc::Checkpointer ckpt(ws, arena); // installs the chunk release fence

  // Acquire a chunk directly, dirty + flush it so blocks are allocated.
  constexpr std::size_t chunk = 256 * 1024;
  auto span = ws.acquire(chunk, alignof(std::max_align_t));
  REQUIRE(span.has_value());
  std::memset(span->base, 0xCD, span->size);
#if defined(_WIN32)
  REQUIRE(::FlushViewOfFile(span->base, span->size) != 0);
#else
  REQUIRE(::msync(span->base, span->size, MS_SYNC) == 0);
#endif
  const std::uint64_t allocated = allocated_size(path.str());

  // Release it: with the chunk fence installed the punch is DEFERRED, so the
  // allocated size is unchanged (the chunk may still back the on-disk root).
  ws.release(*span);
  REQUIRE(allocated_size(path.str()) == allocated);

  // A commit makes the emptying durable and drains the deferred punch.
  REQUIRE(ckpt.commit(0).has_value());
  REQUIRE(allocated_size(path.str()) < allocated);
#else
  SUCCEED("hole-punch storage return is a sparse-file (Linux/Windows) guarantee");
#endif
}

TEST_CASE("without a workspace fence a freed slot returns to the free list immediately") {
  // Anonymous arena, no Checkpointer, no fence: arena_core behavior is unchanged.
  arbc::Arena arena;
  arbc::RefStore<GraphNode> store(arena);

  arbc::SlotIndex freed = 0;
  {
    arbc::Ref<GraphNode> a = *store.create();
    freed = a.index();
  }
  // No fence: release goes straight to the writer's thread-local pool. The
  // global-only free_slots() stays 0; the immediate reuse below is the proof the
  // slot returned (pool.free_pools: sub-batch churn never touches the global pool).
  REQUIRE(store.store().free_slots() == 0);

  arbc::Ref<GraphNode> reused = *store.create();
  REQUIRE(reused.index() == freed); // reused with no commit in sight
}

TEST_CASE("behavioral counters: msyncs, fence releases, and epoch advance as specified") {
  TempPath path;
  auto source = arbc::WorkspaceFileChunkSource::create(path.str());
  REQUIRE(source.has_value());
  arbc::Arena arena(**source);
  arbc::RefStore<GraphNode> store(arena);
  arbc::Checkpointer ckpt(**source, arena);
  store.store().set_release_fence(&ckpt.slot_fence());

  arbc::Ref<GraphNode> root = build_graph(store);
  const arbc::SlotIndex root_index = root.index();

  // First commit flushes the live data chunk(s) and the header once, advances
  // the epoch exactly once.
  REQUIRE(ckpt.commit(root_index).has_value());
  REQUIRE(ckpt.commit_count() == 1);
  REQUIRE(ckpt.header_msyncs() == 1);
  REQUIRE(ckpt.data_msyncs() >= 1);
  REQUIRE(ckpt.epoch() == 2);
  REQUIRE(ckpt.durable_epoch() == 1);

  // An unchanged scene issues ZERO data-chunk msyncs beyond the header flush and
  // frees zero slots, but still advances the epoch exactly once.
  const std::uint64_t data_before = ckpt.data_msyncs();
  REQUIRE(ckpt.commit(root_index).has_value());
  REQUIRE(ckpt.data_msyncs() == data_before); // no new/freed slots -> no data sync
  REQUIRE(ckpt.header_msyncs() == 2);
  REQUIRE(ckpt.commit_count() == 2);
  REQUIRE(ckpt.epoch() == 3);

  // The fence's release counter advances only at commit, never on the free path.
  arbc::Ref<GraphNode> scratch = *store.create();
  const arbc::SlotIndex scratch_index = scratch.index();
  scratch = arbc::Ref<GraphNode>{};         // free -> quarantined
  REQUIRE(ckpt.slots_freed_to_list() == 0); // free path advances nothing
  (void)scratch_index;

  REQUIRE(ckpt.commit(root_index).has_value());
  REQUIRE(ckpt.slots_freed_to_list() == 1); // the commit drained the fence
  REQUIRE(ckpt.epoch() == 4);               // exactly one epoch step per commit
}

#ifndef NDEBUG

#if defined(_WIN32)
namespace {
// The faulting store is isolated in its own frame: MSVC forbids __try/__except in a
// function that also requires C++ object unwinding (error C2712), so this helper
// carries no unwind-requiring locals -- just the raw record pointer and a bool.
// Windows delivers the write to a VirtualProtect(PAGE_READONLY) page as a structured
// exception (EXCEPTION_ACCESS_VIOLATION), the native analog of the POSIX SIGSEGV.
bool published_store_faults(GraphNode* record) {
  bool faulted = false;
  __try {
    record->value = 999; // write into the published (sealed) chunk
  } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                               : EXCEPTION_CONTINUE_SEARCH) {
    faulted = true;
  }
  return faulted;
}
} // namespace
#else
namespace {
sigjmp_buf g_jump;
volatile sig_atomic_t g_faulted = 0;
void segv_handler(int) {
  g_faulted = 1;
  siglongjmp(g_jump, 1);
}
} // namespace
#endif

// enforces: 15-memory-model#checkpoint-published-chunks-read-only
TEST_CASE("a write into a published data chunk faults after commit (debug)") {
  TempPath path;
  auto source = arbc::WorkspaceFileChunkSource::create(path.str());
  REQUIRE(source.has_value());
  arbc::Arena arena(**source);
  // Force two slots per chunk so chunk 0 fills and is FULLY published (sealable),
  // while chunk 1 (the frontier) stays writable for continued allocation.
  arena.store_for(sizeof(GraphNode), alignof(GraphNode), /*chunk_bits=*/1);
  arbc::RefStore<GraphNode> store(arena);
  arbc::Checkpointer ckpt(**source, arena);

  // build_graph creates left(0), right(1) — filling chunk 0 — then root(2) in the
  // frontier chunk 1. The left child lives in the published (sealed) chunk 0.
  arbc::Ref<GraphNode> root = build_graph(store);
  const arbc::SlotIndex sealed_index = root->left.index();
  REQUIRE(sealed_index < 2); // chunk 0
  GraphNode* sealed_record = store.peek_index(sealed_index);
  REQUIRE(ckpt.commit(root.index()).has_value()); // seals chunk 0 read-only

#if defined(_WIN32)
  // The write is caught by an SEH __try/__except on EXCEPTION_ACCESS_VIOLATION,
  // isolated in a no-unwind helper (C2712).
  REQUIRE(published_store_faults(sealed_record));
#else
  struct sigaction old_action{};
  struct sigaction action{};
  action.sa_handler = segv_handler;
  ::sigemptyset(&action.sa_mask);
  REQUIRE(::sigaction(SIGSEGV, &action, &old_action) == 0);

  if (sigsetjmp(g_jump, 1) == 0) {
    sealed_record->value = 999; // write into the published chunk 0
    REQUIRE(false);             // unreachable: the store faulted
  }
  REQUIRE(g_faulted == 1);

  REQUIRE(::sigaction(SIGSEGV, &old_action, nullptr) == 0);
#endif

  // Un-seal so teardown (munmap / directory writes) proceeds normally.
  REQUIRE((*source)->protect_data(false).has_value());
}

// enforces: 15-memory-model#freed-slot-quarantined-until-durable
TEST_CASE("a slot recycled from an EARLIER epoch is writable after a later commit re-seals "
          "its chunk (debug)") {
  // Regression. `commit`'s debug seal re-mprotects whole published chunks read-only on
  // EVERY commit, but the reopen after `drain_fences` used to cover only the slots
  // quarantined in the CURRENT epoch. A slot freed and drained in an earlier epoch is on
  // the free list and belongs to nobody's quarantine any more -- so the next commit's
  // seal re-sealed its page and left it that way, and the first allocation to recycle it
  // died on SEGV_ACCERR inside the placement-new. Only reachable when the recycled slot
  // sits in a chunk that is sealable at all (fully published, strictly below the
  // frontier), which is why the free/commit/reuse test above -- single frontier chunk,
  // nothing sealed -- never caught it. Two commits are the crux: the first drains, the
  // second re-seals.
  //
  // Fixed by reopening every reusable slot's page after the drain, not just the epoch's.
  // Model's asan lane covered this only indirectly; this pins it at the pool layer.
  TempPath path;
  auto source = arbc::WorkspaceFileChunkSource::create(path.str());
  REQUIRE(source.has_value());
  arbc::Arena arena(**source);
  // Two slots per chunk: slots 0,1 fill chunk 0 (sealable once the frontier moves on),
  // slot 2 opens chunk 1 and keeps it the writable frontier.
  arena.store_for(sizeof(GraphNode), alignof(GraphNode), /*chunk_bits=*/1);
  arbc::RefStore<GraphNode> store(arena);
  arbc::Checkpointer ckpt(**source, arena);
  store.store().set_release_fence(&ckpt.slot_fence());

  arbc::SlotIndex freed = 0;
  {
    arbc::Ref<GraphNode> a = *store.create(); // slot 0, chunk 0
    a->value = 7;
    freed = a.index();
  }
  arbc::Ref<GraphNode> b = *store.create(); // slot 1, chunk 0 -- now full
  b->value = 8;
  arbc::Ref<GraphNode> frontier = *store.create(); // slot 2, chunk 1 -- the frontier
  frontier->value = 9;
  REQUIRE(freed < 2); // the freed slot really is in the chunk that gets sealed

  // Epoch 1: seals chunk 0 read-only, then drains -- `freed` returns to the free list
  // and its page is reopened for writing.
  REQUIRE(ckpt.commit(frontier.index()).has_value());

  // Epoch 2: seals chunk 0 again. `freed` is on the free list, in nobody's quarantine.
  // This is the commit that used to leave its page read-only.
  REQUIRE(ckpt.commit(frontier.index()).has_value());

  // The payload write inside create() is what faulted. Reaching the assert at all is the
  // regression check; that it is the recycled slot proves we exercised the intended path.
  arbc::Ref<GraphNode> recycled = *store.create();
  recycled->value = 42;
  REQUIRE(recycled.index() == freed);
  REQUIRE(recycled->value == 42);

  REQUIRE((*source)->protect_data(false).has_value());
}

#endif // NDEBUG

// enforces: 15-memory-model#freed-slot-quarantined-until-durable
TEST_CASE("TSan smoke: RT producers enqueue while the writer drains and checkpoints") {
  TempPath path;
  auto source = arbc::WorkspaceFileChunkSource::create(path.str());
  REQUIRE(source.has_value());
  arbc::Arena arena(**source);

  struct Tracked {
    int value;
    std::atomic<int>* destructions;
    Tracked(int v, std::atomic<int>* d) : value(v), destructions(d) {}
    ~Tracked() {
      if (destructions != nullptr) {
        destructions->fetch_add(1, std::memory_order_relaxed);
      }
    }
  };

  arbc::RefStore<Tracked> store(arena);
  arbc::DeferredReclaimSink<Tracked> sink(store);
  arbc::ReclamationQueue queue;
  queue.register_store(store, sink);
  arbc::Checkpointer ckpt(**source, arena);
  store.store().set_release_fence(&ckpt.slot_fence());

  // A permanent root the checkpoint publishes and that is never freed.
  std::atomic<int> destructions{0};
  arbc::Ref<Tracked> root = *store.create(-1, nullptr);
  const arbc::SlotIndex root_index = root.index();
  const std::size_t baseline = store.slots_live();

  constexpr int producer_count = 4;
  constexpr int per_producer = 250;
  constexpr int total = producer_count * per_producer;

  std::vector<std::vector<arbc::SlotRef<Tracked>>> blocks(producer_count);
  for (int p = 0; p < producer_count; ++p) {
    for (int i = 0; i < per_producer; ++i) {
      arbc::Ref<Tracked> r = *store.create(p * per_producer + i, &destructions);
      arbc::SlotRef<Tracked> s = r.slot();
      REQUIRE(store.retain(s).has_value());
      blocks[p].push_back(s);
    }
  }
  REQUIRE(store.slots_live() == baseline + total);

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
  // The writer drains (reclaim -> SlotStore::release -> fence quarantine) and
  // checkpoints (fence drain -> free_now) concurrently with the producers.
  while (done.load(std::memory_order_acquire) < producer_count) {
    queue.drain();
    REQUIRE(ckpt.commit(root_index).has_value());
  }
  queue.drain();
  REQUIRE(ckpt.commit(root_index).has_value());
  queue.drain();
  REQUIRE(ckpt.commit(root_index).has_value());

  for (std::thread& th : producers) {
    th.join();
  }

  REQUIRE(destructions.load() == total);   // every ~Tracked fired exactly once
  REQUIRE(store.slots_live() == baseline); // only the permanent root remains

  // The resulting file still recovers the permanent root.
  auto header = arbc::WorkspaceFileChunkSource::read_header(path.str());
  REQUIRE(header.has_value());
  const arbc::WorkspaceRoot a = arbc::decode_root(header->root_slot_a);
  const arbc::WorkspaceRoot b = arbc::decode_root(header->root_slot_b);
  const std::uint32_t newest = a.generation >= b.generation ? a.root_index : b.root_index;
  REQUIRE(newest == root_index);
}

#endif // ARBC_HAS_WORKSPACE_FILES
