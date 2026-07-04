#pragma once

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
class ChunkSource {
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

// Default backing: page-aligned anonymous process memory. `pool.mmap_backing`
// generalizes this to mmap/workspace-file sources through the same interface.
class AnonymousChunkSource final : public ChunkSource {
public:
  expected<ChunkSpan, PoolError> acquire(std::size_t size, std::size_t alignment) override;
  void release(ChunkSpan span) noexcept override;
};

} // namespace arbc
