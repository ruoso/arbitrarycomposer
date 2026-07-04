#include <arbc/pool/chunk_source.hpp>
#include <arbc/pool/slab_directory.hpp>
#include <arbc/pool/slot_store.hpp>
#include <arbc/pool/workspace_file.hpp> // ARBC_HAS_WORKSPACE_FILES

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <new>

#if ARBC_HAS_WORKSPACE_FILES
#include <sys/mman.h>
#endif

namespace arbc {

namespace {

// AnonymousChunkSource always over-aligns to a page; every slot alignment we
// support is far smaller, so acquire/release use a single constant and never
// need to remember a per-span alignment.
constexpr std::size_t k_page_alignment = 4096;

} // namespace

expected<ChunkSpan, PoolError> AnonymousChunkSource::acquire(std::size_t size,
                                                             std::size_t alignment) {
  assert(alignment <= k_page_alignment);
  (void)alignment;
  const std::size_t rounded = align_up(size, k_page_alignment);
#if ARBC_HAS_WORKSPACE_FILES
  // Real anonymous mapping, not heap: MAP_NORESERVE keeps larger-than-RAM
  // reservations from pre-committing swap (doc 15's demand-paging framing), and
  // pages return to the OS on munmap — the universal fallback that mirrors the
  // file-backed source without a file.
  void* base = ::mmap(nullptr, rounded, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (base == MAP_FAILED) {
    return unexpected(PoolError::OutOfMemory);
  }
  return ChunkSpan{base, rounded};
#else
  void* base = ::operator new(rounded, std::align_val_t{k_page_alignment}, std::nothrow);
  if (base == nullptr) {
    return unexpected(PoolError::OutOfMemory);
  }
  return ChunkSpan{base, rounded};
#endif
}

void AnonymousChunkSource::release(ChunkSpan span) noexcept {
#if ARBC_HAS_WORKSPACE_FILES
  // munmap rounds the length up to a page multiple, so passing the store's
  // chunk bytes (which acquire rounded up) unmaps the whole region.
  ::munmap(span.base, span.size);
#else
  ::operator delete(span.base, std::align_val_t{k_page_alignment});
#endif
}

SlotStore::SlotStore(std::size_t slot_size, std::size_t slot_align, std::uint32_t chunk_bits,
                     ChunkSource& source)
    : d_slot_size(slot_size), d_slot_align(slot_align),
      d_slot_stride(align_up(std::max<std::size_t>(slot_size, 1), slot_align)),
      d_chunk_bits(chunk_bits), d_slot_mask((std::uint32_t{1} << chunk_bits) - 1),
      d_chunk_slots(std::size_t{1} << chunk_bits), d_chunk_bytes(d_chunk_slots * d_slot_stride),
      d_source(&source) {}

SlotStore::~SlotStore() {
  d_directory.for_each_chunk(
      [this](std::byte* base) { d_source->release(ChunkSpan{base, d_chunk_bytes}); });
}

void SlotStore::assert_writer_thread() noexcept {
#ifndef NDEBUG
  const std::thread::id self = std::this_thread::get_id();
  if (!d_writer_bound) {
    d_writer = self;
    d_writer_bound = true;
  } else {
    assert(self == d_writer && "SlotStore allocate/release is writer-thread-only");
  }
#endif
}

expected<SlotIndex, PoolError> SlotStore::allocate() {
  assert_writer_thread();

  if (!d_free.empty()) {
    // Perfect-hole reuse: the most recently released slot is reused first.
    const SlotIndex index = d_free.back();
    d_free.pop_back();
    ++d_slots_live;
    return index;
  }

  const SlotIndex index = d_high_water;
  const std::uint32_t chunk_number = index >> d_chunk_bits;
  const std::uint32_t slot_in_chunk = index & d_slot_mask;

  if (slot_in_chunk == 0) {
    // `index` is the first slot of a not-yet-backed chunk: grow.
    if (chunk_number >= SlabDirectory<std::byte>::max_chunks) {
      return unexpected(PoolError::CapacityExhausted);
    }
    expected<ChunkSpan, PoolError> span = d_source->acquire(d_chunk_bytes, d_slot_align);
    if (!span) {
      return unexpected(span.error());
    }
    d_directory.publish(chunk_number, static_cast<std::byte*>(span->base));
    d_slots_capacity += d_chunk_slots;
    d_bytes_reserved += span->size;
    // Reserve free-list headroom up front so release() never allocates.
    d_free.reserve(d_slots_capacity);
  }

  ++d_high_water;
  ++d_slots_live;
  return index;
}

void SlotStore::release(SlotIndex index) {
  assert_writer_thread();
  d_free.push_back(index);
  --d_slots_live;
}

void* SlotStore::resolve(SlotIndex index) const noexcept {
  const std::uint32_t chunk_number = index >> d_chunk_bits;
  const std::uint32_t slot_in_chunk = index & d_slot_mask;
  std::byte* base = d_directory.chunk(chunk_number);
  assert(base != nullptr && "resolve of an unbacked slot index");
  return base + static_cast<std::size_t>(slot_in_chunk) * d_slot_stride;
}

Arena::Arena()
    : d_owned_source(std::make_unique<AnonymousChunkSource>()), d_source(d_owned_source.get()) {}

Arena::Arena(ChunkSource& source) : d_source(&source) {}

Arena::~Arena() = default;

SlotStore& Arena::store_for(std::size_t slot_size, std::size_t slot_align,
                            std::uint32_t chunk_bits) {
  const std::size_t align = std::max<std::size_t>(slot_align, alignof(std::max_align_t));
  const std::size_t stride = align_up(std::max<std::size_t>(slot_size, 1), align);
  const std::uint32_t bits = chunk_bits != 0 ? chunk_bits : default_chunk_bits(stride);

  const std::pair<std::size_t, std::size_t> key{stride, align};
  auto it = d_stores.find(key);
  if (it == d_stores.end()) {
    it = d_stores.emplace(key, std::make_unique<SlotStore>(stride, align, bits, *d_source)).first;
  }
  return *it->second;
}

std::size_t Arena::total_slots_live() const noexcept {
  std::size_t total = 0;
  for (const auto& entry : d_stores) {
    total += entry.second->slots_live();
  }
  return total;
}

std::size_t Arena::total_bytes_reserved() const noexcept {
  std::size_t total = 0;
  for (const auto& entry : d_stores) {
    total += entry.second->bytes_reserved();
  }
  return total;
}

} // namespace arbc
