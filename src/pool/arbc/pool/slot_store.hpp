#pragma once

#include <arbc/base/expected.hpp>
#include <arbc/pool/chunk_source.hpp>
#include <arbc/pool/slab_directory.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <utility>
#include <variant>
#include <vector>

#ifndef NDEBUG
#include <thread>
#endif

namespace arbc {

// Storage address of a slot within a store. Distinct from ObjectId: an index
// is a storage location that recycles, an ObjectId is a document identity.
using SlotIndex = std::uint32_t;

// Slots-per-chunk exponent k targeting ~64 KiB data chunks, capped at 12
// (the max the 10+10 root/table split keeps addressable in a 32-bit index).
// Larger slots get fewer slots per chunk; slots larger than a chunk target
// get one slot per chunk.
constexpr std::uint32_t default_chunk_bits(std::size_t slot_stride) {
  constexpr std::size_t target_bytes = std::size_t{64} * 1024;
  const std::size_t slots = target_bytes / (slot_stride == 0 ? 1 : slot_stride);
  std::uint32_t bits = 0;
  while (bits < 12 && (std::size_t{1} << (bits + 1)) <= slots) {
    ++bits;
  }
  return bits;
}

// Round `n` up to a multiple of `align` (a power of two).
constexpr std::size_t align_up(std::size_t n, std::size_t align) {
  return (n + align - 1) & ~(align - 1);
}

// Nullable interposition point between `SlotStore::release` and the free list
// (doc 15's durability-epoch quarantine). Default is `nullptr` — release pushes
// straight to the free list, so every anonymous/arena-only usage is byte-for-
// byte unchanged. When a fence is installed (pool.checkpoints' durability
// fence), `release` diverts the freed slot to the fence instead of the free
// list; the fence returns it through `free_now` only once a checkpoint has made
// the freeing durable. This mirrors the nullable zero-count sink of pool.refs.
class SlotStore;
class ReleaseFence {
public:
  virtual ~ReleaseFence() = default;
  // The store has decided `index` is free but a fence is installed: quarantine
  // it (stamped with the current durability epoch). The slot's data bytes are
  // NOT mutated here and stay resolvable until `free_now` returns it.
  virtual void on_release(SlotStore& store, SlotIndex index) = 0;
};

// Diagnostic backing seam for the REFCOUNT column (doc 15). Production leaves
// this null and refcount chunks are plain anonymous `new[]`/`delete[]` heap --
// the portable default. A harness installs a page-aligned, mprotectable backing
// to machine-check the inside-out invariant that a `const&`/`peek` traversal
// touches no refcount page (the honest-baseline premise,
// 15-memory-model#const-ref-traversal-touches-no-refcount-page): with the
// refcount chunks frozen read-only, a `peek` traversal must not fault, whereas a
// by-value shared_ptr traversal -- which dirties a count per node per visit --
// would. Only the refcount column routes here (that is the column the invariant
// is about); the generation column keeps its `new[]` backing. Because the
// column is owned by the size-class store, the backing is a store property
// supplied at store creation: one backing per size class (the first typed view
// to mint the store wins; later views over the same class share it).
class RefcountTableBacking {
public:
  virtual ~RefcountTableBacking() = default;
  RefcountTableBacking() = default;
  RefcountTableBacking(const RefcountTableBacking&) = delete;
  RefcountTableBacking& operator=(const RefcountTableBacking&) = delete;

  // Storage for `count` live (constructed) `std::atomic<std::uint32_t>`, each
  // holding 0 on return -- matching the value-initialized `new[]` default.
  virtual std::atomic<std::uint32_t>* allocate(std::size_t count) = 0;
  virtual void deallocate(std::atomic<std::uint32_t>* base, std::size_t count) noexcept = 0;
};

// Instance-owned fixed-slot storage for one size class (doc 15). Slots have
// stable addresses for the life of the store; growth appends chunks through
// the ChunkSource and never reallocates. Fragmentation is structurally
// impossible: a released slot is a perfect hole reused by the next same-class
// allocation.
//
// The store owns the inside-out parallel columns keyed by PHYSICAL slot index:
// one `std::atomic<std::uint32_t>` refcount column and (debug only) one
// generation column, grown in lock-step with the data chunks (writer-only, at
// chunk-mint time) so a column can never be short of the data directory. These
// columns are anonymous runtime state (never persisted, rebuilt on open) and
// are read/written by the `pool.refs` typed views over this store. Because they
// are keyed by physical slot -- not by the typed view -- several `RefStore<T>` /
// `RefStore<U>` views that share one size class share ONE count column and ONE
// generation column: a slot has exactly one logical reference count wherever it
// is viewed from, and a slot recycled from `T` to `U` reuses the same count
// entry and carries its generation bump across the views.
//
// Threading (doc 15): `allocate` and `release` are WRITER-THREAD-ONLY in this
// task (a debug-build assert enforces it; thread-local free pools and
// cross-thread release arrive with pool.reclamation). `resolve` is safe from
// any thread concurrently with growth.
//
// `release` marks a slot reusable but DOES NOT run the object's destructor —
// running or deferring destruction is the caller's obligation (doc 15:
// release enqueues, never destroys inline). This is the seam
// pool.reclamation's deferred queue plugs into.
class SlotStore {
public:
  // `refcount_backing` is null in production (portable `new[]` refcount chunks);
  // a diagnostic harness passes a page-aligned, mprotectable backing to freeze
  // the count column read-only and witness the zero-refcount-traffic traversal.
  SlotStore(std::size_t slot_size, std::size_t slot_align, std::uint32_t chunk_bits,
            ChunkSource& source, RefcountTableBacking* refcount_backing = nullptr);
  ~SlotStore();
  SlotStore(const SlotStore&) = delete;
  SlotStore& operator=(const SlotStore&) = delete;

  // Reserve a slot and return its index. Writer-only. Returns an error
  // (never throws, never aborts) when the ChunkSource refuses to grow or the
  // index space is exhausted.
  expected<SlotIndex, PoolError> allocate();

  // Mark a slot reusable. Does NOT destroy the slot's object (caller's
  // obligation). Writer-only. With a release fence installed the slot is
  // diverted to the fence instead of the free list.
  void release(SlotIndex index);

  // Install (or clear, with nullptr) the durability-epoch release fence. Default
  // is no fence (direct free-list push). Writer-only setup.
  void set_release_fence(ReleaseFence* fence) noexcept { d_release_fence = fence; }

  // Un-fenced return of a slot to the free list, called by a ReleaseFence once
  // the freeing is durable. Does NOT touch the live count (release already
  // decremented it) and never runs a destructor. Writer-only.
  void free_now(SlotIndex index);

  // Resolve a slot index to its stable address. Any thread.
  void* resolve(SlotIndex index) const noexcept;

  // Recovery (doc 15, "map → validate → select → rebuild"). Bind existing
  // file-backed chunks covering `[0, high_water)` from the (reopened) source and
  // set the high-water mark WITHOUT constructing anything: the records already
  // live in the file. Counts and free list are rebuilt by the caller
  // (RefStore::set_count on the reachability walk, then `finalize_restore`).
  // Writer-only. The reopened source returns the already-mapped file chunks in
  // order, so this never grows the file.
  expected<std::monostate, PoolError> reserve_restored(std::uint32_t high_water);

  // Recovery finalize: repopulate the free list with the below-high-water
  // complement of `live` (the reachable slots the walk found) and set the live
  // count. Writer-only.
  void finalize_restore(const std::vector<SlotIndex>& live);

  std::uint32_t high_water() const noexcept { return d_high_water; }

  // Debug checkpoint seal (doc 15): visit every FULLY-published data chunk base
  // (all chunks below the frontier chunk still being filled), so the caller can
  // mprotect published records read-only while continued allocation into the
  // frontier chunk stays writable. `fn(base, chunk_bytes)`.
  template <class Fn> void for_each_sealable_chunk(Fn&& fn) const {
    const std::uint32_t frontier = d_high_water >> d_chunk_bits;
    for (std::uint32_t chunk_number = 0; chunk_number < frontier; ++chunk_number) {
      std::byte* base = d_directory.chunk(chunk_number);
      if (base != nullptr) {
        fn(static_cast<void*>(base), d_chunk_bytes);
      }
    }
  }

  std::size_t slot_size() const noexcept { return d_slot_size; }
  std::size_t slot_stride() const noexcept { return d_slot_stride; }
  std::size_t slot_align() const noexcept { return d_slot_align; }
  std::uint32_t chunk_bits() const noexcept { return d_chunk_bits; }

  // The store-owned refcount cell for physical slot `index` (the inside-out
  // parallel column shared by every typed view over this size class). Backed by
  // the chunk minted in lock-step with the slot's data chunk, so it is always
  // published for any live index; never touches a data page. Any thread (a
  // per-slot atomic). This is the hot-path accessor `pool.refs` retain/release
  // read and write.
  std::atomic<std::uint32_t>& count_ref(SlotIndex index) const noexcept {
    const std::uint32_t chunk_number = index >> d_chunk_bits;
    const std::uint32_t slot_in_chunk = index & d_slot_mask;
    return d_refcounts.chunk(chunk_number)[slot_in_chunk];
  }

#ifndef NDEBUG
  // The store-owned generation tag for physical slot `index` (debug only). Bumped
  // when the slot is reclaimed so a stale typed reference faults on resolution --
  // and because the column is store-owned, a bump made through one typed view is
  // visible to a stale reference held through another view of the same slot.
  std::atomic<std::uint32_t>& generation_ref(SlotIndex index) const noexcept {
    const std::uint32_t chunk_number = index >> d_chunk_bits;
    const std::uint32_t slot_in_chunk = index & d_slot_mask;
    return d_generations.chunk(chunk_number)[slot_in_chunk];
  }
#endif

  // Per-store accounting (doc 15 debug discipline; hosts want a memory panel).
  std::size_t slots_live() const noexcept { return d_slots_live; }
  std::size_t slots_capacity() const noexcept { return d_slots_capacity; }
  std::size_t free_slots() const noexcept { return d_free.size(); }
  std::size_t bytes_reserved() const noexcept { return d_bytes_reserved; }

private:
  void assert_writer_thread() noexcept;

  // Publish the refcount (and, in debug, generation) chunk covering
  // `chunk_number` if it is not yet backed. Idempotent. Called in lock-step with
  // data-chunk growth (allocate / reserve_restored) so the columns can never be
  // short of the data directory. Writer-only. The refcount chunk routes through
  // the optional `d_refcount_backing`; the generation chunk is always `new[]`.
  void publish_parallel_columns(std::uint32_t chunk_number);

  std::size_t d_slot_size;
  std::size_t d_slot_align;
  std::size_t d_slot_stride;
  std::uint32_t d_chunk_bits;
  std::uint32_t d_slot_mask;
  std::size_t d_chunk_slots;
  std::size_t d_chunk_bytes;
  ChunkSource* d_source;

  SlabDirectory<std::byte> d_directory;
  // Inside-out parallel columns, physical-slot indexed and grown in lock-step
  // with the data directory (see class comment). Anonymous, never persisted.
  SlabDirectory<std::atomic<std::uint32_t>> d_refcounts;
#ifndef NDEBUG
  SlabDirectory<std::atomic<std::uint32_t>> d_generations;
#endif
  // Null in production (portable `new[]` refcount chunks); non-null only for the
  // page-aligned, mprotectable count-column diagnostic harness.
  RefcountTableBacking* d_refcount_backing{nullptr};
  std::vector<SlotIndex> d_free; // LIFO free list, kept OUTSIDE data pages
  ReleaseFence* d_release_fence{nullptr};
  SlotIndex d_high_water{0};
  std::size_t d_slots_live{0};
  std::size_t d_slots_capacity{0};
  std::size_t d_bytes_reserved{0};

#ifndef NDEBUG
  std::thread::id d_writer{};
  bool d_writer_bound{false};
#endif
};

// Instance arena owning the per-size-class stores (doc 15). Documents own
// arenas: this gives per-arena accounting, O(live buffers) teardown, and
// multi-document / plugin-lifetime ownership that cpioo's `inline static`
// storage cannot. Same-sized record types share a store (free memory
// efficiency).
class Arena {
public:
  Arena();                             // owns a default AnonymousChunkSource
  explicit Arena(ChunkSource& source); // borrows an external source
  ~Arena();
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  // The store for a size class, created on first use. `chunk_bits == 0`
  // derives the chunk size from the slot size; pass nonzero to override.
  // `refcount_backing` is consulted ONLY when the store is first minted (it
  // becomes the size-class store's count-column allocator); a later call for an
  // already-existing size class ignores it -- one backing per size class.
  SlotStore& store_for(std::size_t slot_size, std::size_t slot_align, std::uint32_t chunk_bits = 0,
                       RefcountTableBacking* refcount_backing = nullptr);

  std::size_t store_count() const noexcept { return d_stores.size(); }
  std::size_t total_slots_live() const noexcept;
  std::size_t total_high_water() const noexcept;
  std::size_t total_bytes_reserved() const noexcept;

  // Visit every store in the arena (the checkpoint seal walks these). Writer-only.
  template <class Fn> void for_each_store(Fn&& fn) {
    for (auto& entry : d_stores) {
      fn(*entry.second);
    }
  }

private:
  std::unique_ptr<ChunkSource> d_owned_source; // non-null only for the default
  ChunkSource* d_source;
  std::map<std::pair<std::size_t, std::size_t>, std::unique_ptr<SlotStore>> d_stores;
};

} // namespace arbc
