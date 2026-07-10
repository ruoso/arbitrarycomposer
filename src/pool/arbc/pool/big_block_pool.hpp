#pragma once

#include <arbc/base/expected.hpp>
#include <arbc/pool/chunk_source.hpp>
#include <arbc/pool/refs.hpp>
#include <arbc/pool/slot_store.hpp>

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <type_traits>

namespace arbc {

// The big-block pool (design doc 15, the "bulk media data" memory population):
// a variable-size, page-aligned, refcount-managed bulk-payload allocator for
// raster tile pixels, decoded frames, and audio sample runs -- the page-scale
// blobs that are immutable once filled, as distinct from the small fixed-size
// document-record slabs (`15-memory-model.md:16-22,237-242`).
//
// It is a THIN size-classed facade over the existing Arena/SlotStore machinery,
// NOT a new allocator. Each size class is one `SlotStore` whose slot stride is
// the class's byte size, minted on demand through `Arena::store_for`.
// "Variable-size" is realized as round-up to a power-of-two page-multiple size
// class; "refcounted" reuses the store-owned inside-out count column
// (`SlotStore::count_ref`, pool.refcounts_in_store); "page-aligned" is the
// store's `slot_align`. Because byte blobs run no destructor and hold no child
// references, the zero-count handler is a single `SlotStore::release` -- there
// is no zero-count sink, no reclaim-link stack, and no deferred drain (the
// deliberate divergence from `RefStore<T>`).

class BlockSlotRef;
class BlockRef;
class BigBlockPool;

// The only in-record reference form (mirrors `pool.refs`' `SlotRef<T>`):
// standard-layout and trivially copyable so it can live inside an mmapped
// record. It carries the logical byte length, so the exact span is
// reconstructible from the ref alone and the owning size-class store is a pure
// function of `size()` (`BigBlockPool::class_stride(size())`) -- no per-slot
// side column is needed. It is 8 bytes in release (larger than `SlotRef`'s 4,
// which is fine: a record holds few big-block refs next to megabyte payloads).
// It does NOT own: whoever STORES a BlockSlotRef holds a count on the target
// until it releases it (BigBlockPool::retain / ::release).
class BlockSlotRef {
public:
  BlockSlotRef() = default;

  SlotIndex index() const noexcept { return d_index; }
  std::uint32_t size() const noexcept { return d_size; }

  bool operator==(const BlockSlotRef& other) const noexcept {
    return d_index == other.d_index && d_size == other.d_size;
  }
  bool operator!=(const BlockSlotRef& other) const noexcept { return !(*this == other); }

private:
  friend class BigBlockPool;
  friend class BlockRef;

#ifndef NDEBUG
  BlockSlotRef(SlotIndex index, std::uint32_t size, std::uint32_t generation) noexcept
      : d_index(index), d_size(size), d_generation(generation) {}
#else
  BlockSlotRef(SlotIndex index, std::uint32_t size) noexcept : d_index(index), d_size(size) {}
#endif

  SlotIndex d_index{0};
  std::uint32_t d_size{0};
#ifndef NDEBUG
  std::uint32_t d_generation{0};
#endif
};

// Owning transient handle (mirrors `pool.refs`' `Ref<T>`): pool pointer + slot +
// size + cached data pointer (+ debug generation). Copy retains, move steals,
// destructor releases (RAII, adopting-constructor pattern). `allocate` returns a
// WRITABLE BlockRef for the fill-once phase; after publish, holders read through
// `BigBlockPool::peek`, which touches no count.
class BlockRef {
public:
  BlockRef() noexcept = default;
  BlockRef(const BlockRef& other) noexcept;
  BlockRef(BlockRef&& other) noexcept;
  BlockRef& operator=(const BlockRef& other) noexcept;
  BlockRef& operator=(BlockRef&& other) noexcept;
  ~BlockRef();

  explicit operator bool() const noexcept { return d_store != nullptr; }

  std::byte* data() const noexcept { return d_data; }
  std::uint32_t size() const noexcept { return d_size; }
  std::span<std::byte> bytes() const noexcept { return {d_data, d_size}; }

  SlotIndex index() const noexcept { return d_index; }

  // Non-owning projection to the in-record reference form. Does NOT touch the
  // count -- the count stays with THIS BlockRef. To have a record hold its own
  // count, follow with `BigBlockPool::retain(ref.slot())`.
  BlockSlotRef slot() const noexcept {
#ifndef NDEBUG
    return BlockSlotRef(d_index, d_size, d_generation);
#else
    return BlockSlotRef(d_index, d_size);
#endif
  }

private:
  friend class BigBlockPool;

  // Adopting constructor: takes ownership of a count the caller has ALREADY
  // accounted for (allocate sets it to 1; resolve incremented it).
  struct AdoptTag {};
#ifndef NDEBUG
  BlockRef(AdoptTag, SlotStore* store, SlotIndex index, std::uint32_t size, std::byte* data,
           std::uint32_t generation) noexcept
      : d_store(store), d_index(index), d_size(size), d_data(data), d_generation(generation) {}
#else
  BlockRef(AdoptTag, SlotStore* store, SlotIndex index, std::uint32_t size,
           std::byte* data) noexcept
      : d_store(store), d_index(index), d_size(size), d_data(data) {}
#endif

  void reset() noexcept;

  SlotStore* d_store{nullptr};
  SlotIndex d_index{0};
  std::uint32_t d_size{0};
  std::byte* d_data{nullptr};
#ifndef NDEBUG
  std::uint32_t d_generation{0};
#endif
};

class BigBlockPool {
public:
  // The page floor (and slot alignment) for every size class. Every rung is a
  // power-of-two >= k_page, so each blob address is page-aligned by
  // construction (chunk bases from the ChunkSource are page-aligned and a
  // page-multiple stride keeps every slot aligned).
  static constexpr std::size_t k_page = 4096;
  // Loud, non-silent overflow ceiling (doc 15): a uint32 count that hits its max
  // is an error, never a silent wrap. Mirrors `RefStore`'s `k_max_count`.
  static constexpr std::uint32_t k_max_count = std::numeric_limits<std::uint32_t>::max();

  BigBlockPool();                             // owns a default AnonymousChunkSource
  explicit BigBlockPool(ChunkSource& source); // borrows an external source
  ~BigBlockPool();
  BigBlockPool(const BigBlockPool&) = delete;
  BigBlockPool& operator=(const BigBlockPool&) = delete;

  // Round `size` up to its size-class stride: powers of two with a one-page
  // floor. `class_stride(size) = size <= k_page ? k_page : next_pow2(size)`.
  static constexpr std::size_t next_pow2(std::size_t n) noexcept {
    std::size_t p = k_page;
    while (p < n) {
      p <<= 1;
    }
    return p;
  }
  static constexpr std::size_t class_stride(std::size_t size) noexcept {
    return size <= k_page ? k_page : next_pow2(size);
  }

  // Allocate a WRITABLE blob of `size` bytes: rounds up to `class_stride(size)`,
  // mints/finds the class store, reserves a slot, and returns an owning BlockRef
  // with a count of 1 whose `data()` is page-aligned. WRITER-ONLY (arena growth
  // is single-threaded -- same rule as `SlotStore::allocate`). Propagates the
  // SlotStore error path (never throws, never aborts).
  expected<BlockRef, PoolError> allocate(std::size_t size);

  // Acquire a standalone count for a stored BlockSlotRef (holder-holds-a-count).
  // Overflow-checked; returns the new count. Any thread.
  expected<std::uint32_t, RefError> retain(BlockSlotRef ref);

  // Release a standalone count. On the last count the slot returns to its class
  // free pool (`SlotStore::release`, made any-thread by pool.free_pools). Any
  // thread -- but driving to zero bottoms out in `SlotStore::release`, so it
  // runs on the writer or the housekeeping drain thread, never an RT thread.
  void release(BlockSlotRef ref);

  // Resolve a stored BlockSlotRef to an owning transient BlockRef, pinning the
  // target for the duration of use (increments the count, checked). Any thread.
  expected<BlockRef, RefError> resolve(BlockSlotRef ref);

  // Zero-refcount-traffic read: the blob's bytes without touching the count.
  // Valid only while the target is kept alive by a count the caller holds. The
  // hot-path read primitive (immutable-after-fill). Any thread.
  std::span<const std::byte> peek(BlockSlotRef ref) const noexcept;

  // Current count -- accounting / host memory panel. Any thread.
  std::uint32_t count(BlockSlotRef ref) const noexcept;

  // The size-class store for `size` (minted on first use). WRITER-ONLY (it may
  // grow the arena's store map). Exposed for durability-fence install and
  // per-class accounting / stride inspection.
  SlotStore& class_store(std::size_t size);

  Arena& arena() noexcept { return d_arena; }
  const Arena& arena() const noexcept { return d_arena; }

  // Behavioral counter (doc 16 `:54-62`): advances by exactly one per distinct
  // `allocate` call, and NOT on retain/release/resolve/peek/count.
  std::uint64_t blobs_allocated() const noexcept {
    return d_blobs_allocated.load(std::memory_order_relaxed);
  }

#ifndef NDEBUG
  // Debug-only predicate the resolution asserts are built on: does `ref` carry
  // the generation the slot currently holds? A stale reference to a recycled
  // slot returns false. Exposed so tests can witness the trap without aborting.
  bool generation_matches(BlockSlotRef ref) const noexcept;
#endif

private:
  friend class BlockRef;

  // The class array index for a power-of-two `stride` (log2). Strides run from
  // 2^12 (k_page) up, so the index is >= 12 and bounded by the 32-bit index
  // space; `k_class_count` covers up to 2^39-byte blobs, far beyond reach.
  static constexpr int k_class_count = 40;
  static int class_index(std::size_t stride) noexcept {
    int i = 0;
    while ((std::size_t{1} << i) < stride) {
      ++i;
    }
    return i;
  }

  // Mint (writer-only) or find the store for `stride` and publish its pointer
  // into the lock-free class array so any-thread ops resolve it without touching
  // the arena's store map.
  SlotStore& class_store_for_stride(std::size_t stride);

  // The already-published store for a ref's size class. A valid BlockSlotRef
  // implies its class was minted on the writer (with a release store), so the
  // acquire load here never sees null.
  SlotStore& store_ref(std::uint32_t size) const noexcept {
    SlotStore* store = d_class[class_index(class_stride(size))].load(std::memory_order_acquire);
    assert(store != nullptr && "BlockSlotRef for an unminted size class");
    return *store;
  }

  // The retain/release discipline copied verbatim from `refs.hpp` (uint32, loud
  // overflow, assert underflow) so the whole component shares one overflow rule.
  static bool try_retain_slot(SlotStore& store, SlotIndex index) noexcept {
    std::atomic<std::uint32_t>& counter = store.count_ref(index);
    std::uint32_t current = counter.load(std::memory_order_relaxed);
    do {
      if (current == k_max_count) {
        return false;
      }
    } while (!counter.compare_exchange_weak(current, current + 1, std::memory_order_acq_rel,
                                            std::memory_order_relaxed));
    return true;
  }

  static void release_slot(SlotStore& store, SlotIndex index) noexcept {
    const std::uint32_t previous = store.count_ref(index).fetch_sub(1, std::memory_order_acq_rel);
    assert(previous >= 1 && "big-block refcount underflow: released a slot with no live count");
    if (previous == 1) {
#ifndef NDEBUG
      // Bump the store-owned generation so any surviving stale BlockSlotRef
      // faults (the byte-blob analogue of `RefStore::reclaim`'s bump). Byte
      // blobs run no destructor, so this is the whole reclamation step before
      // the slot returns to the free pool (or the durability fence).
      store.generation_ref(index).fetch_add(1, std::memory_order_acq_rel);
#endif
      store.release(index);
    }
  }

#ifndef NDEBUG
  void assert_generation(SlotStore& store, BlockSlotRef ref) const noexcept {
    assert(ref.d_generation == store.generation_ref(ref.index()).load(std::memory_order_acquire) &&
           "stale BlockSlotRef: slot was recycled since this reference");
  }
#else
  void assert_generation(SlotStore&, BlockSlotRef) const noexcept {}
#endif

  std::unique_ptr<ChunkSource> d_owned_source; // non-null only for the default
  Arena d_arena;
  // Lock-free size-class store cache, indexed by log2(stride). Filled by the
  // writer at `allocate` (release store); read any-thread (acquire load), so a
  // reader's `peek`/`retain`/`release` never touches the arena's store map even
  // while the writer mints a new class concurrently.
  std::atomic<SlotStore*> d_class[k_class_count]{};
  std::atomic<std::uint64_t> d_blobs_allocated{0};
};

// --- BlockRef special members (out-of-line so they can reach BigBlockPool's
// static count helpers) ---

inline BlockRef::BlockRef(const BlockRef& other) noexcept
    : d_store(other.d_store), d_index(other.d_index), d_size(other.d_size), d_data(other.d_data)
#ifndef NDEBUG
      ,
      d_generation(other.d_generation)
#endif
{
  if (d_store != nullptr) {
    const bool ok = BigBlockPool::try_retain_slot(*d_store, d_index);
    assert(ok && "refcount overflow on BlockRef copy");
    (void)ok;
  }
}

inline BlockRef::BlockRef(BlockRef&& other) noexcept
    : d_store(other.d_store), d_index(other.d_index), d_size(other.d_size), d_data(other.d_data)
#ifndef NDEBUG
      ,
      d_generation(other.d_generation)
#endif
{
  other.d_store = nullptr;
  other.d_data = nullptr;
}

inline BlockRef& BlockRef::operator=(const BlockRef& other) noexcept {
  if (this != &other) {
    // Retain the source first so self-referential aliasing can never drop the
    // count to zero mid-assignment.
    if (other.d_store != nullptr) {
      const bool ok = BigBlockPool::try_retain_slot(*other.d_store, other.d_index);
      assert(ok && "refcount overflow on BlockRef assignment");
      (void)ok;
    }
    reset();
    d_store = other.d_store;
    d_index = other.d_index;
    d_size = other.d_size;
    d_data = other.d_data;
#ifndef NDEBUG
    d_generation = other.d_generation;
#endif
  }
  return *this;
}

inline BlockRef& BlockRef::operator=(BlockRef&& other) noexcept {
  if (this != &other) {
    reset();
    d_store = other.d_store;
    d_index = other.d_index;
    d_size = other.d_size;
    d_data = other.d_data;
#ifndef NDEBUG
    d_generation = other.d_generation;
#endif
    other.d_store = nullptr;
    other.d_data = nullptr;
  }
  return *this;
}

inline BlockRef::~BlockRef() { reset(); }

inline void BlockRef::reset() noexcept {
  if (d_store != nullptr) {
    BigBlockPool::release_slot(*d_store, d_index);
    d_store = nullptr;
    d_data = nullptr;
  }
}

// BlockSlotRef is the in-record reference form: standard-layout and trivially
// copyable so it can be memcpy'd inside mmapped records.
static_assert(std::is_standard_layout_v<BlockSlotRef>,
              "BlockSlotRef must be standard-layout to live inside a record");
static_assert(std::is_trivially_copyable_v<BlockSlotRef>,
              "BlockSlotRef must be trivially copyable to live inside a mmapped record");
#ifdef NDEBUG
static_assert(sizeof(BlockSlotRef) == 8, "BlockSlotRef must be exactly 8 bytes in release builds");
#endif

} // namespace arbc
