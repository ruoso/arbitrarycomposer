#pragma once

#include <arbc/base/expected.hpp>
#include <arbc/pool/slab_directory.hpp>
#include <arbc/pool/slot_store.hpp>
#include <arbc/pool/typed_store.hpp>

#include <atomic>
#include <cassert>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>

namespace arbc {

// The ownership layer over the arenas (design doc 15): atomically counted
// references whose counts live in PARALLEL tables, not next to the data (the
// inside-out property). Two reference forms share one ownership model:
//
//   Ref<T>     -- pointer + index, fully copy/move/assignable, for stack and
//                 API use; deref is one indirection. Owns a count.
//   SlotRef<T> -- 4-byte index-only, position-independent, trivially copyable,
//                 standard-layout: the ONLY reference form allowed inside
//                 records (doc 15's mmap requirement). Does not own.
//
// Refcounts are std::atomic<std::uint32_t> in a table indexed identically to
// the slots and allocated as ANONYMOUS runtime state (never through the data
// ChunkSource): increment/decrement never write a data chunk, so data pages
// stay clean for shared/read-only mappings (doc 15/17 isolation path). In
// debug builds a parallel generation table lets a stale reference (a slot
// recycled out from under it) fault loudly instead of reading a live-again
// slot; the generation members are `#ifdef`-gated to zero release overhead.

template <class T> class Ref;
template <class T> class RefStore;

// Loud, non-silent overflow surface for the checked pin path (doc 15: a
// uint32 count that hits its max is an error, never a silent wrap).
enum class RefError { CountOverflow };

// Zero-count sink (doc 15). The store calls `on_zero(index)` when a slot's
// count reaches zero; the sink decides WHEN and HOW to reclaim. This task
// ships an immediate sink (run the destructor, release the slot); the deferred
// reclamation queue (pool.reclamation) swaps in via `set_zero_sink`.
class ZeroCountSink {
public:
  virtual ~ZeroCountSink() = default;
  virtual void on_zero(SlotIndex index) = 0;
};

// 4-byte index-only reference. Position-independent and trivially copyable, so
// it is the only reference form that may live inside a record (records are
// mmapped and have no stable base address across runs, doc 15). It does NOT
// own: the convention is that whoever STORES a SlotRef holds a count on the
// target until it releases it (see RefStore::retain / RefStore::release). A
// SlotRef is resolved back to a usable object only through its RefStore.
template <class T> class SlotRef {
public:
  SlotRef() = default;

  SlotIndex index() const noexcept { return d_index; }

  bool operator==(const SlotRef& other) const noexcept { return d_index == other.d_index; }
  bool operator!=(const SlotRef& other) const noexcept { return d_index != other.d_index; }

private:
  friend class RefStore<T>;
  friend class Ref<T>;

#ifndef NDEBUG
  SlotRef(SlotIndex index, std::uint32_t generation) noexcept
      : d_index(index), d_generation(generation) {}
#else
  explicit SlotRef(SlotIndex index) noexcept : d_index(index) {}
#endif

  SlotIndex d_index{0};
#ifndef NDEBUG
  std::uint32_t d_generation{0};
#endif
};

// Pointer + index owning reference for stack and API use. Copy/move/assign are
// all supported (fixing the cpioo `reference`'s deleted assignment) so refs
// live in containers. Deref is a single indirection through the cached data
// pointer.
//
// TRAVERSAL CONVENTION (doc 15, the cpioo benchmark lesson): pass
// `const Ref<T>&`, never copy a Ref in a read loop -- reads must do ZERO
// refcount traffic. To follow an in-record `SlotRef` edge during traversal use
// `RefStore::peek` (returns a raw pointer, no count touched); the enclosing
// pinned structure keeps the target alive.
template <class T> class Ref {
public:
  Ref() noexcept = default;

  Ref(const Ref& other) noexcept
      : d_store(other.d_store), d_index(other.d_index), d_ptr(other.d_ptr)
#ifndef NDEBUG
        ,
        d_generation(other.d_generation)
#endif
  {
    if (d_store != nullptr) {
      const bool ok = d_store->try_retain(d_index);
      assert(ok && "refcount overflow on Ref copy");
      (void)ok;
    }
  }

  Ref(Ref&& other) noexcept
      : d_store(other.d_store), d_index(other.d_index), d_ptr(other.d_ptr)
#ifndef NDEBUG
        ,
        d_generation(other.d_generation)
#endif
  {
    other.d_store = nullptr;
    other.d_ptr = nullptr;
  }

  Ref& operator=(const Ref& other) noexcept {
    if (this != &other) {
      // Retain the source first so self-referential aliasing (two refs to the
      // same slot) can never drop the count to zero mid-assignment.
      if (other.d_store != nullptr) {
        const bool ok = other.d_store->try_retain(other.d_index);
        assert(ok && "refcount overflow on Ref assignment");
        (void)ok;
      }
      reset();
      d_store = other.d_store;
      d_index = other.d_index;
      d_ptr = other.d_ptr;
#ifndef NDEBUG
      d_generation = other.d_generation;
#endif
    }
    return *this;
  }

  Ref& operator=(Ref&& other) noexcept {
    if (this != &other) {
      reset();
      d_store = other.d_store;
      d_index = other.d_index;
      d_ptr = other.d_ptr;
#ifndef NDEBUG
      d_generation = other.d_generation;
#endif
      other.d_store = nullptr;
      other.d_ptr = nullptr;
    }
    return *this;
  }

  ~Ref() { reset(); }

  explicit operator bool() const noexcept { return d_store != nullptr; }

  T& operator*() const noexcept {
    assert(d_store != nullptr && "deref of an empty Ref");
    return *d_ptr;
  }
  T* operator->() const noexcept {
    assert(d_store != nullptr && "deref of an empty Ref");
    return d_ptr;
  }
  T* get() const noexcept { return d_ptr; }

  SlotIndex index() const noexcept { return d_index; }

  // Non-owning projection to the in-record reference form. Does NOT touch the
  // count -- the count stays with THIS Ref. To have a record hold its own
  // count, follow with `RefStore::retain(ref.slot())`.
  SlotRef<T> slot() const noexcept {
#ifndef NDEBUG
    return SlotRef<T>(d_index, d_generation);
#else
    return SlotRef<T>(d_index);
#endif
  }

  bool operator==(const Ref& other) const noexcept {
    return d_store == other.d_store && d_index == other.d_index;
  }
  bool operator!=(const Ref& other) const noexcept { return !(*this == other); }

private:
  friend class RefStore<T>;

  // Adopting constructor: takes ownership of a count that the caller has
  // ALREADY accounted for (create sets it to 1; resolve/retain incremented
  // it). No increment happens here.
  struct AdoptTag {};
#ifndef NDEBUG
  Ref(AdoptTag, RefStore<T>* store, SlotIndex index, T* ptr, std::uint32_t generation) noexcept
      : d_store(store), d_index(index), d_ptr(ptr), d_generation(generation) {}
#else
  Ref(AdoptTag, RefStore<T>* store, SlotIndex index, T* ptr) noexcept
      : d_store(store), d_index(index), d_ptr(ptr) {}
#endif

  void reset() noexcept {
    if (d_store != nullptr) {
      d_store->release_index(d_index);
      d_store = nullptr;
      d_ptr = nullptr;
    }
  }

  RefStore<T>* d_store{nullptr};
  SlotIndex d_index{0};
  T* d_ptr{nullptr};
#ifndef NDEBUG
  std::uint32_t d_generation{0};
#endif
};

// Owns the objects of one size class AND their inside-out parallel tables. The
// refcount table (and, in debug, the generation table) are indexed by the same
// SlotIndex as the data but allocated as anonymous heap chunks grown in
// lock-step with the store at allocation time (writer-only). Because they are
// separate from the data pages, arbitrary pin/unpin traffic never dirties a
// data chunk.
//
// Threading (doc 15): `create` and `reclaim` are WRITER-THREAD-ONLY (they
// allocate/release slots through the SlotStore, and grow the parallel tables).
// Count operations -- `retain`, `release`, `resolve`, `peek`, `count`, and all
// Ref copy/move/destroy -- are safe from ANY thread. The one caveat: a
// `release` that drops the count to zero dispatches to the sink, and the
// default immediate sink reclaims inline (writer-only). Drive counts to zero on
// the writer thread, or install a deferred sink (pool.reclamation) whose
// on_zero merely enqueues.
template <class T> class RefStore {
public:
  explicit RefStore(Arena& arena) : d_typed(arena) {
    const SlotStore& store = d_typed.store();
    d_chunk_bits = store.chunk_bits();
    d_slot_mask = (std::uint32_t{1} << d_chunk_bits) - 1;
    d_chunk_slots = std::size_t{1} << d_chunk_bits;
    d_immediate.store = this;
    d_sink = &d_immediate;
  }

  ~RefStore() {
    d_refcounts.for_each_chunk([](std::atomic<std::uint32_t>* base) { delete[] base; });
    d_reclaim_links.for_each_chunk([](std::atomic<SlotIndex>* base) { delete[] base; });
#ifndef NDEBUG
    d_generations.for_each_chunk([](std::atomic<std::uint32_t>* base) { delete[] base; });
#endif
  }

  RefStore(const RefStore&) = delete;
  RefStore& operator=(const RefStore&) = delete;

  // Allocate a slot, construct a `T` in it, and return an owning Ref with a
  // count of 1. Writer-only. Propagates the SlotStore error path (never throws,
  // never aborts) -- on failure no object is constructed.
  template <class... Args> expected<Ref<T>, PoolError> create(Args&&... args) {
    expected<SlotIndex, PoolError> index = d_typed.allocate(std::forward<Args>(args)...);
    if (!index) {
      return unexpected(index.error());
    }
    ensure_parallel_chunks(*index);
    count_ref(*index).store(1, std::memory_order_release);
    T* ptr = d_typed.resolve(*index);
#ifndef NDEBUG
    const std::uint32_t generation = generation_ref(*index).load(std::memory_order_acquire);
    return Ref<T>(typename Ref<T>::AdoptTag{}, this, *index, ptr, generation);
#else
    return Ref<T>(typename Ref<T>::AdoptTag{}, this, *index, ptr);
#endif
  }

  // Resolve a stored SlotRef to an owning transient Ref, pinning the target for
  // the duration of use. Increments the count (checked); the returned Ref drops
  // it on destruction. Any thread. This is the "extract a durable handle" path;
  // for zero-traffic traversal use `peek`.
  expected<Ref<T>, RefError> resolve(SlotRef<T> ref) {
    assert_generation(ref);
    if (!try_retain(ref.d_index)) {
      return unexpected(RefError::CountOverflow);
    }
    T* ptr = d_typed.resolve(ref.d_index);
#ifndef NDEBUG
    return Ref<T>(typename Ref<T>::AdoptTag{}, this, ref.d_index, ptr, ref.d_generation);
#else
    return Ref<T>(typename Ref<T>::AdoptTag{}, this, ref.d_index, ptr);
#endif
  }

  // Zero-refcount-traffic read: resolve a SlotRef straight to its object
  // pointer without touching the count. Valid only while the target is kept
  // alive by a count the caller (or an enclosing pinned structure) holds. This
  // is the hot-path traversal primitive. Any thread.
  T* peek(SlotRef<T> ref) const noexcept {
    assert_generation(ref);
    return d_typed.resolve(ref.d_index);
  }

  // Acquire a standalone count for a stored SlotRef (the holder-holds-a-count
  // convention: a record that stores a SlotRef retains here and releases when
  // the record is torn down). Overflow-checked; returns the new count. Any
  // thread.
  expected<std::uint32_t, RefError> retain(SlotRef<T> ref) {
    assert_generation(ref);
    std::atomic<std::uint32_t>& counter = count_ref(ref.d_index);
    std::uint32_t current = counter.load(std::memory_order_relaxed);
    do {
      if (current == k_max_count) {
        return unexpected(RefError::CountOverflow);
      }
    } while (!counter.compare_exchange_weak(current, current + 1, std::memory_order_acq_rel,
                                            std::memory_order_relaxed));
    return current + 1;
  }

  // Release a standalone count held on a stored SlotRef. On the last count the
  // sink is invoked. Any thread (but see the zero-count caveat above).
  void release(SlotRef<T> ref) {
    assert_generation(ref);
    release_index(ref.d_index);
  }

  // Current count for a SlotRef -- accounting/host memory panel (doc 15) and
  // the readout a reachability walk rebuilds against on workspace open.
  std::uint32_t count(SlotRef<T> ref) const noexcept {
    return count_ref(ref.d_index).load(std::memory_order_acquire);
  }

  // Reconstruction seam (doc 15: counts are rebuilt by a reachability walk when
  // a workspace file is mapped -- they never hit disk). Overwrites the count
  // outright. Writer-only.
  void set_count(SlotRef<T> ref, std::uint32_t value) noexcept {
    count_ref(ref.d_index).store(value, std::memory_order_release);
  }

  // Recovery (doc 15, "map → validate → select → rebuild"). Re-bind the store's
  // existing file-backed chunks covering `[0, high_water)` and publish the
  // parallel refcount/reclaim-link tables for them, WITHOUT constructing any
  // object (the records already live in the file). Counts start at zero; the
  // caller's reachability walk sets them via `set_count_index`, then
  // `SlotStore::finalize_restore` builds the free list. Writer-only.
  expected<std::monostate, PoolError> restore(std::uint32_t high_water) {
    expected<std::monostate, PoolError> reserved = d_typed.store().reserve_restored(high_water);
    if (!reserved) {
      return unexpected(reserved.error());
    }
    if (high_water != 0) {
      const std::uint32_t last_chunk = (high_water - 1) >> d_chunk_bits;
      for (std::uint32_t chunk_number = 0; chunk_number <= last_chunk; ++chunk_number) {
        ensure_parallel_chunks(chunk_number << d_chunk_bits);
      }
    }
    return std::monostate{};
  }

  // Raw-index reachability-walk primitives (recovery). The walk reads records by
  // storage index -- SlotRef generations are anonymous and reset on open, so the
  // walk must not assert them. `peek_index` resolves the record, and the child
  // edges it follows are the records' in-place `SlotRef::index()` values.
  T* peek_index(SlotIndex index) const noexcept { return d_typed.resolve(index); }
  void set_count_index(SlotIndex index, std::uint32_t value) noexcept {
    count_ref(index).store(value, std::memory_order_release);
  }
  std::uint32_t count_index(SlotIndex index) const noexcept {
    return count_ref(index).load(std::memory_order_acquire);
  }

  // Immediate reclamation: run `~T`, return the slot to the free list, and (in
  // debug) bump the slot's generation so any surviving stale reference faults.
  // This is what the default sink does and what a deferred queue calls when it
  // pops. Writer-only.
  void reclaim(SlotIndex index) {
    d_typed.destroy(index);
#ifndef NDEBUG
    generation_ref(index).fetch_add(1, std::memory_order_acq_rel);
#endif
  }

  // Swap the zero-count sink (pool.reclamation installs a deferred queue).
  // Passing nullptr restores the built-in immediate sink.
  void set_zero_sink(ZeroCountSink* sink) noexcept {
    d_sink = sink != nullptr ? sink : &d_immediate;
  }

  // Lock-free, allocation-free push of a slot onto this store's anonymous
  // reclaim-link stack (pool.reclamation). This is what a deferred sink's
  // `on_zero` calls: a single CAS onto the parallel link table (published at
  // create), touching no data page and taking no lock -- RT-thread-safe. The
  // slot is NOT destroyed here; a later drain runs `~T`. Any thread.
  void enqueue_reclaim(SlotIndex index) noexcept {
    std::atomic<SlotIndex>& link = link_ref(index);
    SlotIndex head = d_reclaim_head.load(std::memory_order_relaxed);
    do {
      link.store(head, std::memory_order_relaxed);
    } while (!d_reclaim_head.compare_exchange_weak(head, index, std::memory_order_acq_rel,
                                                   std::memory_order_relaxed));
  }

  // Detach this store's reclaim stack with a single `exchange` (single-consumer
  // pop -- no CAS-pop loop, so no Treiber-stack ABA hazard) and `reclaim` every
  // slot on it. Each `reclaim` runs `~T`, whose member Ref/SlotRef releases
  // decrement child counts and re-enter the sink, pushing children onto this (or
  // another registered store's) fresh stack -- the iterative cascade, unrolled
  // through the queue so the C++ stack stays O(1) in subtree depth. Reclaims one
  // detached batch; the ReclamationQueue loops across stores to quiescence.
  // Returns whether it reclaimed anything. Writer-thread-only.
  bool drain_pending() {
    SlotIndex head = d_reclaim_head.exchange(k_reclaim_nil, std::memory_order_acquire);
    if (head == k_reclaim_nil) {
      return false;
    }
    while (head != k_reclaim_nil) {
      const SlotIndex next = link_ref(head).load(std::memory_order_relaxed);
      reclaim(head);
      head = next;
    }
    return true;
  }

#ifndef NDEBUG
  // Debug-only predicate the resolution asserts are built on: does `ref` carry
  // the generation the slot currently holds? A stale reference to a recycled
  // slot returns false. Exposed so tests can witness the stale-reference trap
  // without tripping an abort (an assert-hook style check).
  bool generation_matches(SlotRef<T> ref) const noexcept {
    return ref.d_generation == generation_ref(ref.d_index).load(std::memory_order_acquire);
  }
#endif

  // Accounting passthrough (doc 15 debug discipline).
  std::size_t slots_live() const noexcept { return d_typed.store().slots_live(); }

  // The underlying untyped store -- where recovery installs the durability
  // release fence and where `finalize_open` rebuilds the free list.
  SlotStore& store() noexcept { return d_typed.store(); }

  // How many times a parallel chunk (refcount + reclaim-link, and in debug the
  // generation table) has been published. Advances only on the writer-only
  // `create` path, never on the RT enqueue path -- the counter a behavioral
  // assertion checks to prove enqueue allocates no table storage.
  std::size_t reclaim_chunks_published() const noexcept { return d_reclaim_chunks; }

private:
  friend class Ref<T>;

  static constexpr std::uint32_t k_max_count = std::numeric_limits<std::uint32_t>::max();
  // Empty-stack sentinel for the reclaim-link table. Slot 0 is a valid index, so
  // the sentinel is the max SlotIndex (0xFFFFFFFF), never a real slot.
  static constexpr SlotIndex k_reclaim_nil = std::numeric_limits<SlotIndex>::max();

  struct ImmediateSink final : ZeroCountSink {
    RefStore* store{nullptr};
    void on_zero(SlotIndex index) override { store->reclaim(index); }
  };

  // Publish the refcount (and, in debug, generation) chunk covering `index` if
  // it is not yet backed. These chunks are ANONYMOUS heap, never the data
  // ChunkSource -- the inside-out split that keeps data pages clean. Writer-only
  // (called from create, in lock-step with the store's own growth).
  void ensure_parallel_chunks(SlotIndex index) {
    const std::uint32_t chunk_number = index >> d_chunk_bits;
    if (d_refcounts.chunk(chunk_number) == nullptr) {
      d_refcounts.publish(chunk_number, new std::atomic<std::uint32_t>[d_chunk_slots]());
      // The reclaim-link table is published in lock-step with the refcount
      // table (release builds included -- deferred reclamation is production
      // behavior), so the RT enqueue path never publishes a chunk or allocates.
      d_reclaim_links.publish(chunk_number, new std::atomic<SlotIndex>[d_chunk_slots]());
#ifndef NDEBUG
      d_generations.publish(chunk_number, new std::atomic<std::uint32_t>[d_chunk_slots]());
#endif
      ++d_reclaim_chunks;
    }
  }

  std::atomic<std::uint32_t>& count_ref(SlotIndex index) const noexcept {
    const std::uint32_t chunk_number = index >> d_chunk_bits;
    const std::uint32_t slot_in_chunk = index & d_slot_mask;
    return d_refcounts.chunk(chunk_number)[slot_in_chunk];
  }

  // The reclaim-link "next" slot for `index` in the anonymous parallel table
  // (the inside-out sibling of the refcount/generation tables). Backed by the
  // chunk published at create; never touches a data page.
  std::atomic<SlotIndex>& link_ref(SlotIndex index) const noexcept {
    const std::uint32_t chunk_number = index >> d_chunk_bits;
    const std::uint32_t slot_in_chunk = index & d_slot_mask;
    return d_reclaim_links.chunk(chunk_number)[slot_in_chunk];
  }

  // Unchecked-return atomic increment used by Ref copy/assign. Never silently
  // wraps: on saturation it leaves the count at max and returns false (the
  // caller asserts in debug; a release build harmlessly declines the pin rather
  // than wrapping to zero -- the 4-billion-pin bug class the debug lane and
  // asan catch).
  bool try_retain(SlotIndex index) noexcept {
    std::atomic<std::uint32_t>& counter = count_ref(index);
    std::uint32_t current = counter.load(std::memory_order_relaxed);
    do {
      if (current == k_max_count) {
        return false;
      }
    } while (!counter.compare_exchange_weak(current, current + 1, std::memory_order_acq_rel,
                                            std::memory_order_relaxed));
    return true;
  }

  void release_index(SlotIndex index) noexcept {
    const std::uint32_t previous = count_ref(index).fetch_sub(1, std::memory_order_acq_rel);
    assert(previous >= 1 && "refcount underflow: released a slot with no live count");
    if (previous == 1) {
      d_sink->on_zero(index);
    }
  }

#ifndef NDEBUG
  std::atomic<std::uint32_t>& generation_ref(SlotIndex index) const noexcept {
    const std::uint32_t chunk_number = index >> d_chunk_bits;
    const std::uint32_t slot_in_chunk = index & d_slot_mask;
    return d_generations.chunk(chunk_number)[slot_in_chunk];
  }
  void assert_generation(SlotRef<T> ref) const noexcept {
    assert(generation_matches(ref) && "stale SlotRef: slot was recycled since this reference");
  }
#else
  void assert_generation(SlotRef<T>) const noexcept {}
#endif

  TypedStore<T> d_typed;
  SlabDirectory<std::atomic<std::uint32_t>> d_refcounts;
  // Anonymous per-store reclaim-link stack: a parallel table of "next" indices
  // plus one head. Producers CAS-push (enqueue_reclaim); the writer detaches and
  // walks it (drain_pending). Empty is k_reclaim_nil.
  SlabDirectory<std::atomic<SlotIndex>> d_reclaim_links;
  std::atomic<SlotIndex> d_reclaim_head{k_reclaim_nil};
#ifndef NDEBUG
  SlabDirectory<std::atomic<std::uint32_t>> d_generations;
#endif
  std::uint32_t d_chunk_bits{0};
  std::uint32_t d_slot_mask{0};
  std::size_t d_chunk_slots{0};
  std::size_t d_reclaim_chunks{0};
  ImmediateSink d_immediate{};
  ZeroCountSink* d_sink{nullptr};
};

// SlotRef is the in-record reference form: standard-layout and trivially
// copyable so it can be memcpy'd inside mmapped records, and (release build)
// exactly 4 bytes -- the size promise persistent-map nodes are written against.
static_assert(std::is_standard_layout_v<SlotRef<int>>,
              "SlotRef must be standard-layout to live inside a record");
static_assert(std::is_trivially_copyable_v<SlotRef<int>>,
              "SlotRef must be trivially copyable to live inside a mmapped record");
#ifdef NDEBUG
static_assert(sizeof(SlotRef<int>) == 4, "SlotRef must be exactly 4 bytes in release builds");
#endif

} // namespace arbc
