#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace arbc {

// Two-level chunk directory with monotonic, address-stable growth (doc 15).
//
// A 32-bit slot index splits as
//     index        = chunk_number << chunk_bits | slot_in_chunk
//     chunk_number = root_index    << table_bits | table_index
// giving root(10) | table(10) | slot(chunk_bits) at the default chunk size.
// A fixed root array of 2^10 second-level-table pointers each points at an
// on-demand table of 2^10 chunk-base pointers. All pointers are atomic:
// the writer publishes a chunk base with a release store; any thread
// resolves it with two acquire loads and an add. Growth only ever appends
// chunks — a published base never moves — which is what makes lock-free
// pinned reads possible with no RCU and no reallocation.
//
// The directory is parameterized over the chunk element `Slot` so the same
// indexing drives PARALLEL tables: `SlotStore` instantiates it over
// `std::byte` for raw slot storage, and `pool.refs` will instantiate it over
// refcount / generation-tag elements against the identical index space — the
// inside-out layout falls out of reuse, not duplication.
template <class Slot> class SlabDirectory {
public:
  static constexpr std::uint32_t root_bits = 10;
  static constexpr std::uint32_t table_bits = 10;
  static constexpr std::uint32_t root_size = std::uint32_t{1} << root_bits;
  static constexpr std::uint32_t table_size = std::uint32_t{1} << table_bits;
  static constexpr std::uint32_t max_chunks = std::uint32_t{1} << (root_bits + table_bits);

  SlabDirectory() = default;
  ~SlabDirectory() { clear(); }
  SlabDirectory(const SlabDirectory&) = delete;
  SlabDirectory& operator=(const SlabDirectory&) = delete;

  // Base of the chunk holding `chunk_number`, or nullptr if it has not been
  // published. Safe from any thread concurrently with growth (acquire loads).
  Slot* chunk(std::uint32_t chunk_number) const noexcept {
    const std::uint32_t root_index = chunk_number >> table_bits;
    const std::uint32_t table_index = chunk_number & (table_size - 1);
    const Table* table = d_root[root_index].load(std::memory_order_acquire);
    if (table == nullptr) {
      return nullptr;
    }
    return table->slots[table_index].load(std::memory_order_acquire);
  }

  // Publish `base` as the chunk for `chunk_number`, allocating its
  // second-level table on demand. Writer-thread-only; the release stores make
  // the table and base visible to concurrent READERS. Note the load-check-`new`-
  // store is atomic for readers but NOT between publishers: two concurrent
  // publishers into one root slot would double-`new` and lose a table. Correct
  // only under a single publisher -- the SlotStore single-writer-identity
  // contract (slot_store.hpp Threading) is what guarantees that.
  void publish(std::uint32_t chunk_number, Slot* base) {
    const std::uint32_t root_index = chunk_number >> table_bits;
    const std::uint32_t table_index = chunk_number & (table_size - 1);
    Table* table = d_root[root_index].load(std::memory_order_acquire);
    if (table == nullptr) {
      table = new Table();
      d_root[root_index].store(table, std::memory_order_release);
    }
    table->slots[table_index].store(base, std::memory_order_release);
  }

  // Visit every published chunk base once (teardown / accounting). Must not
  // race with growth.
  template <class Fn> void for_each_chunk(Fn&& fn) const {
    for (std::uint32_t r = 0; r < root_size; ++r) {
      const Table* table = d_root[r].load(std::memory_order_relaxed);
      if (table == nullptr) {
        continue;
      }
      for (std::uint32_t t = 0; t < table_size; ++t) {
        Slot* base = table->slots[t].load(std::memory_order_relaxed);
        if (base != nullptr) {
          fn(base);
        }
      }
    }
  }

private:
  struct Table {
    std::array<std::atomic<Slot*>, table_size> slots{};
  };

  void clear() noexcept {
    for (std::uint32_t r = 0; r < root_size; ++r) {
      delete d_root[r].load(std::memory_order_relaxed);
      d_root[r].store(nullptr, std::memory_order_relaxed);
    }
  }

  std::array<std::atomic<Table*>, root_size> d_root{};
};

} // namespace arbc
