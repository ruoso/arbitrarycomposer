#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace arbc {

// Stable, document-unique object identity (doc 14 § Identity). Zero is never a
// valid id.
struct ObjectId {
  std::uint64_t value{0};

  constexpr bool valid() const { return value != 0; }

  friend constexpr bool operator==(const ObjectId&, const ObjectId&) = default;
};

// The `ObjectId` space is split in two halves by its top bit (doc 14 § Identity).
// The model allocator issues only bit-63-CLEAR ids (`Model::allocate_id` counts up
// from 1, so the reserved half is 2^63 allocations away); the reserved half --
// bit 63 SET -- is the runtime's SYNTHESIZED-IDENTITY namespace, minted for graph
// nodes the model never named (an operator's inline input children, doc 13). The
// two halves are disjoint by construction, so a synthesized cache identity can
// never equal any model `ObjectId` in the document: damage or cache invalidation
// naming an id in one namespace cannot evict the other's entries. Synthesized ids
// are render-time state -- never journaled, never serialized.
inline constexpr std::uint64_t kSyntheticIdBit = 1ull << 63;

// Pure predicate on the bit pattern: is this id from the reserved half? Checkable
// from a single id in isolation -- no allocation-order or high-water-mark
// assumption. `ObjectId{}` (the invalid/fallback id) is NOT synthetic.
constexpr bool synthetic(ObjectId id) { return (id.value & kSyntheticIdBit) != 0; }

// Mint the `n`-th synthesized identity, `n` counting from 1. The bare
// `kSyntheticIdBit` (n == 0) is never minted, so every synthesized id is `valid()`
// and the mapping from `n` is injective (doc 14 § Identity; Decision 3).
constexpr ObjectId synthetic_id(std::uint64_t n) { return ObjectId{kSyntheticIdBit | n}; }

} // namespace arbc

template <> struct std::hash<arbc::ObjectId> {
  std::size_t operator()(const arbc::ObjectId& id) const noexcept {
    return std::hash<std::uint64_t>{}(id.value);
  }
};
