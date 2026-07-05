#pragma once

// Reusable benchmark workloads for the pool allocator (design doc 15).
//
// The workload BODIES live here, free of any harness dependency, so two very
// different drivers can share them:
//   * src/pool/bench/allocator_bench.cpp wraps each in a Google Benchmark
//     `for (auto _ : state)` loop (built only under ARBC_BENCHMARKS), and
//   * src/pool/t/bench_smoke.t.cpp drives each once with a tiny size under the
//     normal dev/asan test build, asserting the BEHAVIORAL facts (leak-to-
//     baseline, flat chunk publication, exactly-once destruction, topology
//     equality) so the benchmark code carries diff coverage and rot protection
//     without any wall-clock assertion entering the merge path (doc 16:82-87,
//     225-226).
//
// The topology mirrors doc 15's evaluation (15-memory-model.md:47-49): a
// producer publishing immutable tree versions and a consumer traversing them.
// The managed side uses the shipped `arbc::pool` surface (RefStore / Ref /
// SlotRef / peek); the `std::shared_ptr` side is a pure `std` baseline in this
// translation unit (levelization: Level 1, reaches nothing above pool).

#include <arbc/pool/reclamation.hpp>
#include <arbc/pool/refs.hpp>
#include <arbc/pool/slot_store.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace arbc::pool_bench {

// ---------------------------------------------------------------------------
// Producer/consumer tree node types (managed vs shared_ptr), same shape.
// ---------------------------------------------------------------------------

// Managed record: a binary-tree node whose child edges are in-record SlotRefs
// (the only reference form allowed inside a record, doc 15). It holds a
// RETAINED count on each child (holder-holds-a-count) and drops those counts in
// its destructor, so dropping the root cascades the whole subtree. `probe` is
// null on the benchmark path (a single teardown-time null check, never in the
// timed traversal); the smoke test points it at a counter to witness that the
// deferred cascade runs every `~BenchNode` exactly once.
struct BenchNode {
  std::uint64_t value{0};
  arbc::SlotRef<BenchNode> left{};
  arbc::SlotRef<BenchNode> right{};
  bool has_left{false};
  bool has_right{false};
  arbc::RefStore<BenchNode>* store{nullptr};
  int* probe{nullptr};

  BenchNode() = default;

  ~BenchNode() {
    if (probe != nullptr) {
      ++*probe;
    }
    // Holder-holds-a-count: drop the counts this record held on its children.
    // With a deferred sink installed this only ENQUEUES each child (no
    // re-entrant ~BenchNode); the next drain batch continues the cascade.
    if (has_left) {
      store->release(left);
    }
    if (has_right) {
      store->release(right);
    }
  }
};

struct SharedNode {
  std::uint64_t value{0};
  std::shared_ptr<SharedNode> left;
  std::shared_ptr<SharedNode> right;
};

// ---------------------------------------------------------------------------
// Producers: build one immutable tree version. `counter` labels nodes so the
// managed and shared trees built from the same seed are value-identical, which
// the smoke test asserts to catch a mis-shaped topology.
// ---------------------------------------------------------------------------

// Build a full binary tree of `depth` levels ((2^depth - 1) nodes); returns an
// owning Ref to the root that pins the whole subtree. Each internal node retains
// a count on each child; the child owning Refs drop as the recursion unwinds, so
// every node ends at count 1 (pinned only by the record edge above it, and the
// root by the returned Ref).
inline arbc::Ref<BenchNode> build_managed_tree(arbc::RefStore<BenchNode>& store, int depth,
                                               std::uint64_t& counter, int* probe = nullptr) {
  arbc::Ref<BenchNode> node = *store.create();
  node->store = &store;
  node->probe = probe;
  node->value = counter++;
  if (depth > 1) {
    arbc::Ref<BenchNode> l = build_managed_tree(store, depth - 1, counter, probe);
    arbc::Ref<BenchNode> r = build_managed_tree(store, depth - 1, counter, probe);
    node->left = l.slot();
    node->right = r.slot();
    (void)store.retain(node->left);
    (void)store.retain(node->right);
    node->has_left = true;
    node->has_right = true;
    // l, r drop here: children go 2 -> 1, still pinned by the record's count.
  }
  return node;
}

inline std::shared_ptr<SharedNode> build_shared_tree(int depth, std::uint64_t& counter) {
  auto node = std::make_shared<SharedNode>();
  node->value = counter++;
  if (depth > 1) {
    node->left = build_shared_tree(depth - 1, counter);
    node->right = build_shared_tree(depth - 1, counter);
  }
  return node;
}

// ---------------------------------------------------------------------------
// Consumers: traverse a pinned version, summing values.
// ---------------------------------------------------------------------------

// Managed traversal via `peek`: follows in-record SlotRef edges straight to the
// object, touching NO refcount page. The enclosing pinned root keeps every node
// alive for the walk. This is the zero-refcount-traffic analog the honest
// baseline is measured against.
inline std::uint64_t traverse_managed_peek(const arbc::RefStore<BenchNode>& store,
                                           const BenchNode* node) {
  std::uint64_t sum = node->value;
  if (node->has_left) {
    sum += traverse_managed_peek(store, store.peek(node->left));
  }
  if (node->has_right) {
    sum += traverse_managed_peek(store, store.peek(node->right));
  }
  return sum;
}

// FAIR baseline (the doc 15:75-80 fix): the shared_ptr visitor takes `const&`,
// matching the managed consumer's `const Ref&`/`peek` -- no per-node-per-visit
// refcount traffic. This is the honest headline comparison.
inline std::uint64_t traverse_shared_constref(const std::shared_ptr<SharedNode>& node) {
  std::uint64_t sum = node->value;
  if (node->left) {
    sum += traverse_shared_constref(node->left);
  }
  if (node->right) {
    sum += traverse_shared_constref(node->right);
  }
  return sum;
}

// DISHONEST baseline, kept only as an explicitly labeled datapoint: the visitor
// takes its argument BY VALUE and passes children by value, so every visit bumps
// and drops a refcount (dirtying a control block per node). This is exactly the
// inflation doc 15:75-80 calls out; the benchmark reports it as
// `_byvalue_baseline` so the honesty is visible in the output, not just prose.
inline std::uint64_t traverse_shared_byvalue(std::shared_ptr<SharedNode> node) { // NOLINT
  std::uint64_t sum = node->value;
  if (node->left) {
    sum += traverse_shared_byvalue(node->left);
  }
  if (node->right) {
    sum += traverse_shared_byvalue(node->right);
  }
  return sum;
}

// ---------------------------------------------------------------------------
// Allocation microbenchmarks.
// ---------------------------------------------------------------------------

// Warm a store's free list so `create` is the pop-from-freelist fast path:
// allocate `n` owning Refs, then drop them (immediate reclaim returns `n` warm
// slots to the free list). Returns the store to zero live slots.
inline void warm_store(arbc::RefStore<std::uint64_t>& store, int n) {
  std::vector<arbc::Ref<std::uint64_t>> refs;
  refs.reserve(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) {
    refs.push_back(*store.create(static_cast<std::uint64_t>(i)));
  }
  // refs drop here -> every slot reclaims -> free list holds n warm slots.
}

// One warm create + immediate drop (the steady-state allocation unit measured
// against `new`/`make_shared`). Returns the constructed value so the caller can
// keep it from being optimized away.
inline std::uint64_t warm_create_once(arbc::RefStore<std::uint64_t>& store, std::uint64_t v) {
  arbc::Ref<std::uint64_t> r = *store.create(v);
  return *r;
  // r drops -> slot reclaims back to the (still warm) free list.
}

// ---------------------------------------------------------------------------
// Churn / reclaim workloads.
// ---------------------------------------------------------------------------

// One churn cycle: allocate `n` objects each carrying a standalone retained
// count, release every one to zero (which, under the deferred sink, only
// enqueues), then drain to quiescence. The fixed-slot slab reuses the freed
// holes, so the arena returns to its pre-cycle live count and no new reclaim
// chunk is published across cycles (fragmentation-free reuse).
inline void churn_cycle(arbc::RefStore<std::uint64_t>& store, arbc::ReclamationQueue& queue,
                        int n) {
  std::vector<arbc::SlotRef<std::uint64_t>> slots;
  slots.reserve(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) {
    arbc::Ref<std::uint64_t> r = *store.create(static_cast<std::uint64_t>(i));
    arbc::SlotRef<std::uint64_t> s = r.slot();
    (void)store.retain(s); // a standalone count survives the owning Ref
    slots.push_back(s);
    // r drops here: count 2 -> 1 (never zero, so nothing is enqueued yet).
  }
  for (arbc::SlotRef<std::uint64_t> s : slots) {
    store.release(s); // -> 0 -> deferred sink enqueues
  }
  queue.drain(); // iterative cascade to quiescence
}

} // namespace arbc::pool_bench
