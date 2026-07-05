#pragma once

#include <arbc/model/records.hpp>
#include <arbc/pool/refs.hpp>

#include <bit>
#include <cstdint>
#include <type_traits>

namespace arbc {

// The persistent (path-copying) map from `ObjectId` to object records (doc 14,
// § "versions, not mutations") is a bitmapped trie -- a HAMT -- over the 64-bit
// key, with FIXED-ARITY interior nodes so nodes land in a single slab size class
// (doc 15:225-227; refinement Decisions). Key digits are consumed LOW bits first
// (sequentially assigned `ObjectId`s spread across the top levels rather than
// collapsing under a shared high-bit prefix), 5 bits per level.
//
// A commit path-copies only the nodes from the root to the touched key -- every
// untouched subtree is shared by `SlotRef` identity between the pre- and
// post-commit versions (14-data-model-and-editing#commit-shares-untouched-
// structure). Nodes refer to one another by 4-byte `SlotRef` edges only; each
// stored edge holds one count on its target (the holder-holds-a-count
// convention), released when the node is reclaimed.

inline constexpr std::uint32_t k_hamt_bits = 5;
inline constexpr std::uint32_t k_hamt_arity = std::uint32_t{1} << k_hamt_bits; // 32
inline constexpr std::uint32_t k_hamt_mask = k_hamt_arity - 1;
// 64-bit key / 5 bits per level, rounded up: the maximum trie depth, so build
// recursion is bounded (reclamation is separately iterative via the queue).
inline constexpr std::uint32_t k_hamt_max_depth = (64 + k_hamt_bits - 1) / k_hamt_bits;

struct HamtNode; // forward: children are SlotRef<HamtNode> (index-only, self-referential)

// The two typed slab stores a DocState lives in: the HAMT nodes and the object
// records they bind. Passed by pointer into the non-record control objects
// (DocRoot, Transaction) -- these are heap objects, not mmapped records, so they
// may hold pointers.
struct StoreBundle {
  RefStore<HamtNode>* nodes{nullptr};
  RefStore<ObjectRecord>* records{nullptr};
};

// Ambient context a `~HamtNode` reaches to release its child edges. Records must
// be POINTER-FREE (doc 15:192-197), so a node cannot store a pointer to its
// store; instead the single drainer publishes this context (thread-local) around
// `ReclamationQueue::drain`, and `~HamtNode` -- which only ever runs inside that
// drain (the deferred sink defers every release) -- reads it. This keeps the
// pool's mandated deferred, iterative cascade while nodes stay pointer-free.
struct ReclaimContext {
  RefStore<HamtNode>* nodes{nullptr};
  RefStore<ObjectRecord>* records{nullptr};
};

inline ReclaimContext*& active_reclaim_context() noexcept {
  static thread_local ReclaimContext* ctx = nullptr;
  return ctx;
}

// RAII: publish `ctx` as the active reclamation context for the current thread
// (the writer, or the low-priority housekeeping thread) for the duration of a
// drain. Nested/re-entrant safe.
class ReclaimContextGuard {
public:
  explicit ReclaimContextGuard(ReclaimContext& ctx) noexcept : d_prev(active_reclaim_context()) {
    active_reclaim_context() = &ctx;
  }
  ~ReclaimContextGuard() { active_reclaim_context() = d_prev; }
  ReclaimContextGuard(const ReclaimContextGuard&) = delete;
  ReclaimContextGuard& operator=(const ReclaimContextGuard&) = delete;

private:
  ReclaimContext* d_prev;
};

// One persistent-map node. A LEAF binds one `key -> record` edge; a BRANCH holds
// up to `k_hamt_arity` child edges selected by a 5-bit key digit (`bitmap` marks
// which are present). Standard-layout, fixed-size, and pointer-free -- the
// mmap/checkpoint-compatible slab shape.
//
// NOTE ON DESTRUCTIBILITY: unlike the object records, a HamtNode is NOT trivially
// destructible. It OWNS its child/record `SlotRef` edges (holder-holds-a-count),
// so reclaiming it must decrement those counts to continue the structural-sharing
// cascade -- and the pool's deferred cascade drives exactly that through `~T`
// (pool/reclamation.hpp, driven by `ReclamationQueue::drain`). The destructor
// reaches its stores through the ambient `ReclaimContext`, so the node itself
// still stores no pointer.
struct HamtNode {
  std::uint8_t is_leaf{0};

  // Leaf payload (valid iff is_leaf): the object-record edge this node owns.
  std::uint64_t key{0};
  SlotRef<ObjectRecord> record{};

  // Branch payload (valid iff !is_leaf): the owned child edges.
  std::uint32_t bitmap{0};
  SlotRef<HamtNode> children[k_hamt_arity]{};

  HamtNode() = default;
  HamtNode(const HamtNode&) = delete; // nodes are built in place, never copied
  HamtNode& operator=(const HamtNode&) = delete;

  ~HamtNode() {
    ReclaimContext* ctx = active_reclaim_context();
    // Only ever reached during a guarded drain; outside one (e.g. arena drop at
    // teardown) the node's memory is freed without a per-node walk (doc 15:144).
    if (ctx == nullptr) {
      return;
    }
    if (is_leaf != 0) {
      ctx->records->release(record);
      return;
    }
    std::uint32_t remaining = bitmap;
    while (remaining != 0) {
      const std::uint32_t digit = static_cast<std::uint32_t>(std::countr_zero(remaining));
      ctx->nodes->release(children[digit]);
      remaining &= remaining - 1;
    }
  }
};

static_assert(std::is_standard_layout_v<HamtNode>,
              "HamtNode must be standard-layout to live in a mmapped arena");
static_assert(std::is_standard_layout_v<SlotRef<HamtNode>>,
              "self-referential node edges must be index-only");

// Build the next version by inserting/replacing `key -> record` into `root`
// (empty `root` == empty map). Returns an owning `Ref` to the NEW root; every
// untouched subtree is shared (retained) rather than copied. The caller must
// hold a live count on `record` for the duration; the new leaf takes its own.
// Writer-only (allocates). Errors surface as values (never throws).
expected<Ref<HamtNode>, PoolError> hamt_insert(StoreBundle& sb, const Ref<HamtNode>& root,
                                               std::uint64_t key, SlotRef<ObjectRecord> record);

// Resolve `key` to its bound object-record edge in `root` (a pinned version).
// Zero-refcount-traffic: follows `SlotRef` edges via `peek` (doc 15:42-44). Any
// thread. Returns false (leaving `out` untouched) when the key is absent.
bool hamt_lookup(const StoreBundle& sb, const Ref<HamtNode>& root, std::uint64_t key,
                 SlotRef<ObjectRecord>& out);

} // namespace arbc
