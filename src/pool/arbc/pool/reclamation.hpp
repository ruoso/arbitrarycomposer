#pragma once

#include <arbc/pool/refs.hpp>

#include <cstddef>
#include <vector>

namespace arbc {

// Deferred reclamation (design doc 15, "Cascades are deferred, never inline").
//
// When a reference count hits zero, release must ENQUEUE the slot rather than
// destroy it inline, so dropping the last reference to a large subtree never
// makes the releasing thread (potentially a render or audio-engine thread) run
// thousands of destructors. A later drain pass pops the queue, runs each
// object's `~T` -- whose child-reference releases enqueue THEIR targets onto the
// same queue -- and continues the cascade ITERATIVELY (a loop, not recursion):
// bounded stack, bounded latency, slots warm back to the free list. Running the
// destructors is exactly what fixes cpioo's missing-destructor gap (doc 15
// "gap 1"): child refcounts that cpioo never decremented on slot reuse now get
// decremented, so nested subtrees no longer leak.
//
// Two pieces, both header-only in arbc::pool over `base`:
//   DeferredReclaimSink<T> -- a ZeroCountSink installed on a RefStore<T> via the
//                             existing set_zero_sink seam; on_zero enqueues.
//   ReclamationQueue       -- the registry of stores a drain pass sweeps to
//                             quiescence, driving the iterative cascade.

// A ZeroCountSink that DEFERS reclamation. Its on_zero performs a lock-free,
// allocation-free push of the slot onto the store's anonymous reclaim-link
// stack (RefStore::enqueue_reclaim) -- touching no data page, taking no lock, no
// `~T` run here. Install with `store.set_zero_sink(&sink)`; the ReclamationQueue
// does this for you in `register_store`. RT-thread-safe.
template <class T> class DeferredReclaimSink final : public ZeroCountSink {
public:
  explicit DeferredReclaimSink(RefStore<T>& store) noexcept : d_store(&store) {}

  DeferredReclaimSink(const DeferredReclaimSink&) = delete;
  DeferredReclaimSink& operator=(const DeferredReclaimSink&) = delete;

  void on_zero(SlotIndex index) override { d_store->enqueue_reclaim(index); }

private:
  RefStore<T>* d_store;
};

// The central drain mechanism (NOT the policy: when to run is
// runtime.housekeeping's job). Registered stores drain through it until global
// quiescence, driving the iterative destructor cascade across size classes.
//
// Type erasure is PER STORE, not per slot: each entry is a
// `{ void* store, bool (*drain)(void*) }` thunk that static_casts back to the
// concrete RefStore<T>* and drains one batch. No per-slot type tag -- type is
// recovered from the (already type-segregated) store, keeping the hot enqueue
// path free of any tag write.
//
// Drain is WRITER-THREAD-ONLY (it bottoms out in SlotStore::release, which
// arena_core binds to the writer thread). RT threads only enqueue.
class ReclamationQueue {
public:
  ReclamationQueue() = default;
  ReclamationQueue(const ReclamationQueue&) = delete;
  ReclamationQueue& operator=(const ReclamationQueue&) = delete;

  // Install `sink` as `store`'s deferred zero-count sink and record its drain
  // thunk. Writer-only setup; `sink` and `store` must outlive this queue.
  template <class T> void register_store(RefStore<T>& store, DeferredReclaimSink<T>& sink) {
    store.set_zero_sink(&sink);
    d_entries.push_back(Entry{&store, &drain_thunk<T>});
  }

  // Run drain batches across every registered store until a full pass reclaims
  // nothing (global quiescence). A batch's destructors enqueue children onto
  // this or another store's fresh stack; the outer loop keeps sweeping until
  // every stack is empty. The recursion is unrolled through the stacks, so the
  // C++ call stack stays O(1) in subtree depth. Empty- and double-drain are
  // no-ops. Writer-thread-only.
  void drain() {
    bool progressed = true;
    while (progressed) {
      progressed = false;
      for (const Entry& entry : d_entries) {
        if (entry.drain(entry.store)) {
          progressed = true;
        }
      }
    }
  }

  std::size_t store_count() const noexcept { return d_entries.size(); }

private:
  struct Entry {
    void* store;
    bool (*drain)(void*);
  };

  template <class T> static bool drain_thunk(void* store) {
    return static_cast<RefStore<T>*>(store)->drain_pending();
  }

  std::vector<Entry> d_entries;
};

} // namespace arbc
