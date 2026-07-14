#pragma once

#include <arbc/base/expected.hpp>
#include <arbc/pool/chunk_source.hpp>
#include <arbc/pool/slot_store.hpp>

#include <cstdint>
#include <new>
#include <utility>

namespace arbc {

// Header-only typed veneer over a SlotStore (doc 15). `T` maps to its size
// class; `allocate` placement-news a `T` into the reserved slot and `resolve`
// hands back a typed pointer. The untyped SlotStore does the storage work so
// same-sized record types share a store and the future C ABI stays simple.
//
// `release` marks the slot reusable WITHOUT running `~T` — honoring the
// SlotStore contract, destruction is the caller's obligation and the seam
// pool.reclamation's deferred queue plugs into. `destroy` is the explicit
// run-the-destructor-then-release helper for callers that reclaim inline.
template <class T> class TypedStore {
public:
  // `refcount_backing` is forwarded to the size-class store on first creation
  // (it becomes the store's count-column allocator); later views over the same
  // size class share the store already minted, so their backing arg is ignored.
  explicit TypedStore(Arena& arena, RefcountTableBacking* refcount_backing = nullptr)
      : d_store(&arena.store_for(sizeof(T), alignof(T), 0, refcount_backing)) {}
  explicit TypedStore(SlotStore& store) : d_store(&store) {}

  // Reserve a slot and construct a `T` in it, forwarding the arguments.
  // Propagates the SlotStore error path (never throws, never aborts) — on
  // failure no object is constructed.
  template <class... Args> expected<SlotIndex, PoolError> allocate(Args&&... args) {
    expected<SlotIndex, PoolError> index = d_store->allocate();
    if (!index) {
      return index;
    }
    ::new (d_store->resolve(*index)) T(std::forward<Args>(args)...);
    return index;
  }

  // Typed address of a live slot. Any thread.
  T* resolve(SlotIndex index) const noexcept { return static_cast<T*>(d_store->resolve(index)); }

  // Mark the slot reusable WITHOUT destroying the object (caller's obligation).
  void release(SlotIndex index) { d_store->release(index); }

  // Run `~T` WITHOUT releasing the slot. Split out of `destroy` so a caller that
  // must interpose between destruction and the slot becoming REUSABLE can do so
  // — `RefStore::reclaim` arms the debug generation tripwire in that gap, and a
  // slot handed to the free pool is re-allocatable by the writer immediately.
  void destruct(SlotIndex index) { resolve(index)->~T(); }

  // Run `~T` then release the slot (inline reclamation).
  void destroy(SlotIndex index) {
    destruct(index);
    d_store->release(index);
  }

  SlotStore& store() const noexcept { return *d_store; }

private:
  SlotStore* d_store;
};

} // namespace arbc
