#pragma once

#include <cstdint>

// The 64-bit bijective mixer every revision fold runs its contributions through
// (`model.per_object_revision` Decision 3). `base` is L0, so `model` (L2),
// `compositor` (L4) and `runtime` all reach it with no new levelization edge --
// `cache`'s `detail::key_hash_combine` (`key_shapes.hpp`) is L3 and therefore
// unusable from `model`, which is why this lives here rather than being reused
// from there.

namespace arbc {

// splitmix64's finalizer: a BIJECTION on the full 64-bit range (each step --
// xor-shift, odd multiply -- is invertible), which is the whole property the
// revision folds rest on.
//
// A fold that sums raw per-object revision stamps cancels: stamps are small
// monotone integers, so two reachable inputs at stamps 7 and 3 sum to the same
// 10 as a later configuration at 6 and 4 -- reachable through an ordinary
// undo/redo interleaving, or a membership edit that swaps a high-stamp layer for
// a low-stamp one -- and a collision there serves another configuration's
// composed tile: wrong pixels, silently, from a cache hit. Summing a bijective
// mix of each contribution instead keeps every property the fold needs
// (commutative and associative, hence order-independent; each reachable node
// folded exactly once) while destroying the structured cancellation, because the
// mixed images of 7,3 and 6,4 bear no arithmetic relation to one another.
//
// It is NOT a cryptographic hash and makes no claim to be: it removes structured
// cancellation between small integers, which is exactly the failure mode
// (05-recursive-composition#aggregate-fold-mixes-before-summing).
inline constexpr std::uint64_t mix64(std::uint64_t x) noexcept {
  x ^= x >> 30;
  x *= 0xBF58476D1CE4E5B9ULL;
  x ^= x >> 27;
  x *= 0x94D049BB133111EBULL;
  x ^= x >> 31;
  return x;
}

} // namespace arbc
