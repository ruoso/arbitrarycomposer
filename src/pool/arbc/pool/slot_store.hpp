#pragma once

#include <arbc/base/expected.hpp>
#include <arbc/pool/chunk_source.hpp>
#include <arbc/pool/slab_directory.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <utility>
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

// Instance-owned fixed-slot storage for one size class (doc 15). Slots have
// stable addresses for the life of the store; growth appends chunks through
// the ChunkSource and never reallocates. Fragmentation is structurally
// impossible: a released slot is a perfect hole reused by the next same-class
// allocation.
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
  SlotStore(std::size_t slot_size, std::size_t slot_align, std::uint32_t chunk_bits,
            ChunkSource& source);
  ~SlotStore();
  SlotStore(const SlotStore&) = delete;
  SlotStore& operator=(const SlotStore&) = delete;

  // Reserve a slot and return its index. Writer-only. Returns an error
  // (never throws, never aborts) when the ChunkSource refuses to grow or the
  // index space is exhausted.
  expected<SlotIndex, PoolError> allocate();

  // Mark a slot reusable. Does NOT destroy the slot's object (caller's
  // obligation). Writer-only.
  void release(SlotIndex index);

  // Resolve a slot index to its stable address. Any thread.
  void* resolve(SlotIndex index) const noexcept;

  std::size_t slot_size() const noexcept { return d_slot_size; }
  std::size_t slot_stride() const noexcept { return d_slot_stride; }
  std::size_t slot_align() const noexcept { return d_slot_align; }
  std::uint32_t chunk_bits() const noexcept { return d_chunk_bits; }

  // Per-store accounting (doc 15 debug discipline; hosts want a memory panel).
  std::size_t slots_live() const noexcept { return d_slots_live; }
  std::size_t slots_capacity() const noexcept { return d_slots_capacity; }
  std::size_t bytes_reserved() const noexcept { return d_bytes_reserved; }

private:
  void assert_writer_thread() noexcept;

  std::size_t d_slot_size;
  std::size_t d_slot_align;
  std::size_t d_slot_stride;
  std::uint32_t d_chunk_bits;
  std::uint32_t d_slot_mask;
  std::size_t d_chunk_slots;
  std::size_t d_chunk_bytes;
  ChunkSource* d_source;

  SlabDirectory<std::byte> d_directory;
  std::vector<SlotIndex> d_free; // LIFO free list, kept OUTSIDE data pages
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
  Arena();                          // owns a default AnonymousChunkSource
  explicit Arena(ChunkSource& source); // borrows an external source
  ~Arena();
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  // The store for a size class, created on first use. `chunk_bits == 0`
  // derives the chunk size from the slot size; pass nonzero to override.
  SlotStore& store_for(std::size_t slot_size, std::size_t slot_align,
                       std::uint32_t chunk_bits = 0);

  std::size_t store_count() const noexcept { return d_stores.size(); }
  std::size_t total_slots_live() const noexcept;
  std::size_t total_bytes_reserved() const noexcept;

private:
  std::unique_ptr<ChunkSource> d_owned_source; // non-null only for the default
  ChunkSource* d_source;
  std::map<std::pair<std::size_t, std::size_t>, std::unique_ptr<SlotStore>> d_stores;
};

} // namespace arbc
