#include <arbc/pool/checkpoint.hpp>
#include <arbc/pool/reclamation.hpp>
#include <arbc/pool/refs.hpp>
#include <arbc/pool/slot_store.hpp>
#include <arbc/pool/workspace_file.hpp>

#include <cstring>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// The checkpoint protocol rides the file-backed workspace, which is POSIX-only.
// On other platforms it compiles out with just the runtime-query check.
TEST_CASE("checkpoint support tracks workspace-file support") {
  REQUIRE(arbc::workspace_files_supported() == (ARBC_HAS_WORKSPACE_FILES != 0));
}

#if ARBC_HAS_WORKSPACE_FILES

#include <csetjmp>
#include <csignal>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <thread>

namespace {

// A self-cleaning unique workspace-file path under the temp dir (mirrors the
// workspace_file tests).
class TempPath {
public:
  TempPath() {
    char tmpl[] = "/tmp/arbc_ckpt_XXXXXX";
    const int fd = ::mkstemp(tmpl);
    if (fd >= 0) {
      ::close(fd);
    }
    d_path = tmpl;
  }
  ~TempPath() { ::unlink(d_path.c_str()); }
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

blkcnt_t block_count(const std::string& path) {
  struct stat st{};
  REQUIRE(::stat(path.c_str(), &st) == 0);
  return st.st_blocks;
}

// Byte-copy the workspace file so recovery runs against an INDEPENDENT file, as
// a real recovery process would (its own fd + mappings). This mirrors a crash:
// the copy captures the on-disk state the last commit msync'd, and the recovery
// arena's teardown (which hole-punches its chunks) never touches the writer's
// live file. Copying the msync'd bytes is exactly what a post-crash reopen sees.
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
  REQUIRE(n == 0); // clean EOF
  ::close(in);
  ::close(out);
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
  { arbc::Ref<GraphNode> scratch = *store.create(); }
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
  REQUIRE(a.generation == 1);         // two successive commits wrote different
  REQUIRE(b.generation == 2);         // slots
  REQUIRE(a.root_index == first.index());
  REQUIRE(b.root_index == second.index());

  auto opened = arbc::Checkpointer::open(path.str());
  REQUIRE(opened.has_value());
  REQUIRE(opened->generation == 2);   // the newer generation wins
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

  // A commit that makes the freeing durable releases the slot.
  REQUIRE(ckpt.commit(keep_index).has_value());
  REQUIRE(ckpt.slot_fence().pending() == 0);
  REQUIRE(store.store().free_slots() == 1);

  // Now it is reusable.
  arbc::Ref<GraphNode> reused = *store.create();
  REQUIRE(reused.index() == freed);
}

// enforces: 15-memory-model#freed-slot-quarantined-until-durable
TEST_CASE("an emptied chunk is not hole-punched until the emptying is durable") {
#if defined(__linux__)
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
  REQUIRE(::msync(span->base, span->size, MS_SYNC) == 0);
  const blkcnt_t allocated = block_count(path.str());

  // Release it: with the chunk fence installed the punch is DEFERRED, so the
  // block count is unchanged (the chunk may still back the on-disk root).
  ws.release(*span);
  REQUIRE(block_count(path.str()) == allocated);

  // A commit makes the emptying durable and drains the deferred punch.
  REQUIRE(ckpt.commit(0).has_value());
  REQUIRE(block_count(path.str()) < allocated);
#else
  SUCCEED("hole-punch storage return is a Linux-only guarantee");
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
  REQUIRE(store.store().free_slots() == 1); // straight to the free list

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
  scratch = arbc::Ref<GraphNode>{}; // free -> quarantined
  REQUIRE(ckpt.slots_freed_to_list() == 0); // free path advances nothing
  (void)scratch_index;

  REQUIRE(ckpt.commit(root_index).has_value());
  REQUIRE(ckpt.slots_freed_to_list() == 1); // the commit drained the fence
  REQUIRE(ckpt.epoch() == 4);               // exactly one epoch step per commit
}

#ifndef NDEBUG

namespace {
sigjmp_buf g_jump;
volatile sig_atomic_t g_faulted = 0;
void segv_handler(int) {
  g_faulted = 1;
  siglongjmp(g_jump, 1);
}
} // namespace

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

  // Un-seal so teardown (munmap / directory writes) proceeds normally.
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

  REQUIRE(destructions.load() == total);       // every ~Tracked fired exactly once
  REQUIRE(store.slots_live() == baseline);     // only the permanent root remains

  // The resulting file still recovers the permanent root.
  auto header = arbc::WorkspaceFileChunkSource::read_header(path.str());
  REQUIRE(header.has_value());
  const arbc::WorkspaceRoot a = arbc::decode_root(header->root_slot_a);
  const arbc::WorkspaceRoot b = arbc::decode_root(header->root_slot_b);
  const std::uint32_t newest = a.generation >= b.generation ? a.root_index : b.root_index;
  REQUIRE(newest == root_index);
}

#endif // ARBC_HAS_WORKSPACE_FILES
