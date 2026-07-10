#include <arbc/pool/big_block_pool.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace arbc {

BigBlockPool::BigBlockPool()
    : d_owned_source(std::make_unique<AnonymousChunkSource>()), d_arena(*d_owned_source) {}

BigBlockPool::BigBlockPool(ChunkSource& source) : d_arena(source) {}

BigBlockPool::~BigBlockPool() = default;

SlotStore& BigBlockPool::class_store_for_stride(std::size_t stride) {
  // chunk_bits == 0 lets the store derive its chunk size from the slot stride;
  // slot_align == k_page makes every slot page-aligned.
  SlotStore& store = d_arena.store_for(stride, k_page, /*chunk_bits=*/0);
  d_class[class_index(stride)].store(&store, std::memory_order_release);
  return store;
}

SlotStore& BigBlockPool::class_store(std::size_t size) {
  return class_store_for_stride(class_stride(size));
}

expected<BlockRef, PoolError> BigBlockPool::allocate(std::size_t size) {
  SlotStore& store = class_store_for_stride(class_stride(size));
  expected<SlotIndex, PoolError> index = store.allocate();
  if (!index) {
    return unexpected(index.error());
  }
  store.count_ref(*index).store(1, std::memory_order_release);
  auto* data = static_cast<std::byte*>(store.resolve(*index));
  d_blobs_allocated.fetch_add(1, std::memory_order_relaxed);
  const auto logical = static_cast<std::uint32_t>(size);
#ifndef NDEBUG
  const std::uint32_t generation = store.generation_ref(*index).load(std::memory_order_acquire);
  return BlockRef(BlockRef::AdoptTag{}, &store, *index, logical, data, generation);
#else
  return BlockRef(BlockRef::AdoptTag{}, &store, *index, logical, data);
#endif
}

expected<std::uint32_t, RefError> BigBlockPool::retain(BlockSlotRef ref) {
  SlotStore& store = store_ref(ref.size());
  assert_generation(store, ref);
  std::atomic<std::uint32_t>& counter = store.count_ref(ref.index());
  std::uint32_t current = counter.load(std::memory_order_relaxed);
  do {
    if (current == k_max_count) {
      return unexpected(RefError::CountOverflow);
    }
  } while (!counter.compare_exchange_weak(current, current + 1, std::memory_order_acq_rel,
                                          std::memory_order_relaxed));
  return current + 1;
}

void BigBlockPool::release(BlockSlotRef ref) {
  SlotStore& store = store_ref(ref.size());
  assert_generation(store, ref);
  release_slot(store, ref.index());
}

expected<BlockRef, RefError> BigBlockPool::resolve(BlockSlotRef ref) {
  SlotStore& store = store_ref(ref.size());
  assert_generation(store, ref);
  if (!try_retain_slot(store, ref.index())) {
    return unexpected(RefError::CountOverflow);
  }
  auto* data = static_cast<std::byte*>(store.resolve(ref.index()));
#ifndef NDEBUG
  return BlockRef(BlockRef::AdoptTag{}, &store, ref.index(), ref.size(), data, ref.d_generation);
#else
  return BlockRef(BlockRef::AdoptTag{}, &store, ref.index(), ref.size(), data);
#endif
}

std::span<const std::byte> BigBlockPool::peek(BlockSlotRef ref) const noexcept {
  SlotStore& store = store_ref(ref.size());
  assert_generation(store, ref);
  const auto* data = static_cast<const std::byte*>(store.resolve(ref.index()));
  return {data, ref.size()};
}

std::uint32_t BigBlockPool::count(BlockSlotRef ref) const noexcept {
  SlotStore& store = store_ref(ref.size());
  return store.count_ref(ref.index()).load(std::memory_order_acquire);
}

#ifndef NDEBUG
bool BigBlockPool::generation_matches(BlockSlotRef ref) const noexcept {
  SlotStore& store = store_ref(ref.size());
  return ref.d_generation == store.generation_ref(ref.index()).load(std::memory_order_acquire);
}
#endif

} // namespace arbc
