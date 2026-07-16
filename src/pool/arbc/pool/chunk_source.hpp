#pragma once

#include <arbc/arbc_api.h>
#include <arbc/base/expected.hpp>

#include <cstddef>

namespace arbc {

// Failure modes surfaced as values (doc 10: never thrown, never aborting).
enum class PoolError {
  OutOfMemory,       // the backing ChunkSource refused to grow
  CapacityExhausted, // the 32-bit slot index space for a store is full
};

// A page-aligned span of raw bytes handed out by a ChunkSource.
struct ChunkSpan {
  void* base{nullptr};
  std::size_t size{0};
};

// Backing seam for slab chunks (doc 15). A store acquires fixed-size,
// page-aligned spans and releases them at teardown; only immutable data
// chunks flow through this interface, so `pool.mmap_backing` can swap in
// file-backed spans without touching store logic. Bookkeeping tables
// (directory, free list) never go through a ChunkSource.
class ARBC_API ChunkSource {
public:
  ChunkSource() = default;
  virtual ~ChunkSource() = default;
  ChunkSource(const ChunkSource&) = delete;
  ChunkSource& operator=(const ChunkSource&) = delete;

  // Acquire a span of at least `size` bytes aligned to at least `alignment`.
  virtual expected<ChunkSpan, PoolError> acquire(std::size_t size, std::size_t alignment) = 0;

  // Return a span previously handed out by `acquire`.
  virtual void release(ChunkSpan span) noexcept = 0;
};

// Per-store routing seam (doc 15's arena directory). An Arena bound to a router
// asks it which ChunkSource should back each size-class store, instead of
// handing every store the one source. `pool.workspace_store_directory`
// implements it over the workspace file's store table so each store gets a
// facade serving only its own chunks; the anonymous path leaves the router null
// and is byte-for-byte unchanged.
//
// The interface is deliberately NOT `ChunkSource::acquire(..., store_id)`: the
// ChunkSource seam is the generic backing interface that AnonymousChunkSource
// and big_block_pool also speak, and neither has any notion of a store
// directory. Routing at bind time instead of at acquire time makes mis-routing
// structurally impossible rather than one wrong argument away.
class ARBC_API ChunkSourceRouter {
public:
  ChunkSourceRouter() = default;
  virtual ~ChunkSourceRouter() = default;
  ChunkSourceRouter(const ChunkSourceRouter&) = delete;
  ChunkSourceRouter& operator=(const ChunkSourceRouter&) = delete;

  // The source that should back the store for this size class. Returns nullptr
  // when the router REFUSES to bind it (the workspace store table is full, or the
  // file's recorded geometry disagrees with this build); the router carries the
  // reason. A refused store is backed by a RefusingChunkSource, never by a
  // fallback that could serve another store's chunks.
  virtual ChunkSource* source_for(std::size_t slot_stride, std::size_t slot_align,
                                  std::size_t chunk_slots) = 0;
};

// Default backing: page-aligned anonymous process memory. `pool.mmap_backing`
// generalizes this to mmap/workspace-file sources through the same interface.
class ARBC_API AnonymousChunkSource final : public ChunkSource {
public:
  expected<ChunkSpan, PoolError> acquire(std::size_t size, std::size_t alignment) override;
  void release(ChunkSpan span) noexcept override;
};

// Backing for a store whose router refused to bind it. It hands out nothing, so
// a mis-bound store physically cannot serve another store's chunks — the refusal
// is enforced structurally, not by convention (the same move mmap_backing made
// for the anonymous-bookkeeping split). The store's `allocate` then fails as a
// value; the router reports why.
class RefusingChunkSource final : public ChunkSource {
public:
  expected<ChunkSpan, PoolError> acquire(std::size_t, std::size_t) override {
    return unexpected(PoolError::OutOfMemory);
  }
  void release(ChunkSpan) noexcept override {}
};

} // namespace arbc
