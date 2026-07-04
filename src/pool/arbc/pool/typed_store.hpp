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
  explicit TypedStore(Arena& arena) : d_store(&arena.store_for(sizeof(T), alignof(T))) {}
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

  // Run `~T` then release the slot (inline reclamation).
  void destroy(SlotIndex index) {
    resolve(index)->~T();
    d_store->release(index);
  }

  SlotStore& store() const noexcept { return *d_store; }

private:
  SlotStore* d_store;
};

} // namespace arbc
