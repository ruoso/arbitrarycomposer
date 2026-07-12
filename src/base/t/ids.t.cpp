// The `ObjectId` namespace split (doc 14 § Identity, `runtime.pull_identity_disjoint_ids`):
// the 64-bit id space is halved by its top bit. The model allocator issues only
// bit-63-clear ids; the reserved half is the runtime's synthesized-identity namespace.
// Disjointness is STRUCTURAL -- `synthetic()` is a pure predicate on one id's bit
// pattern, decidable in isolation, with no dependence on allocation order or on the
// model's high-water mark. These cases pin the predicate at its boundaries, which is
// what makes "a synthesized id never equals a model id" checkable rather than merely
// asserted.

#include <arbc/base/ids.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

using namespace arbc;

TEST_CASE("synthetic() splits the ObjectId space at bit 63") {
  // The model half: zero (the invalid/fallback id), the first allocated id, and the
  // largest id the model could ever issue. None is synthetic -- in particular the
  // fallback `ObjectId{}` must not be mistaken for a reserved id (Constraint 3).
  CHECK(!synthetic(ObjectId{}));
  CHECK(!synthetic(ObjectId{1}));
  CHECK(!synthetic(ObjectId{2}));
  CHECK(!synthetic(ObjectId{(1ULL << 63) - 1}));

  // The reserved half: the bare bit, the bit with a payload, and all-ones.
  CHECK(synthetic(ObjectId{1ULL << 63}));
  CHECK(synthetic(ObjectId{(1ULL << 63) | 1}));
  CHECK(synthetic(ObjectId{~0ULL}));

  // The predicate is exactly "top bit set" -- constant and predicate agree.
  CHECK(kSyntheticIdBit == (1ULL << 63));
}

TEST_CASE("synthetic_id() mints valid, injective ids inside the reserved half") {
  // Counting from 1 *within* the half, so the bare `kSyntheticIdBit` is never minted
  // and every synthesized id is `valid()` -- no synthesized id can be confused with
  // the `ObjectId{}` fallback an unmapped `Content*` falls back to (Decision 3).
  CHECK(synthetic_id(1) == ObjectId{(1ULL << 63) | 1});
  CHECK(synthetic_id(1).valid());
  CHECK(synthetic(synthetic_id(1)));

  // Injective across the counter: distinct n yield distinct ids, all synthetic.
  for (std::uint64_t n = 1; n <= 8; ++n) {
    CHECK(synthetic(synthetic_id(n)));
    CHECK(synthetic_id(n).valid());
    CHECK(synthetic_id(n) != synthetic_id(n + 1));
    // ...and none of them collides with the model-half id of the same ordinal, which
    // is the whole point of the split.
    CHECK(synthetic_id(n) != ObjectId{n});
  }
}

// The split is a compile-time property of the id type, not a runtime convention: a
// consumer keying on `ObjectId` can decide the namespace in a constant expression.
static_assert(!synthetic(ObjectId{}));
static_assert(!synthetic(ObjectId{(1ULL << 63) - 1}));
static_assert(synthetic(synthetic_id(1)));
static_assert(synthetic_id(1).valid());
