// `mix64` (`model.per_object_revision` Decision 3): the bijective 64-bit finalizer the
// aggregate revision fold runs each contribution through BEFORE summing. Two properties
// carry the whole decision, and both are checked here rather than assumed: the map is
// injective (so folding a mixed contribution loses no information a raw stamp carried),
// and it destroys the structured cancellation a raw sum of small monotone stamps suffers
// -- which is the wrong-pixel failure mode the fold's "iff" promise depends on.

#include <arbc/base/hash_mix.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <unordered_set>

using namespace arbc;

TEST_CASE("mix64 is injective over the small-integer range revision stamps live in") {
  // Revision stamps are small monotone integers minted from a commit counter, so the
  // range that actually matters is the low one. An injective map over it means no two
  // distinct stamps can ever mix to the same value.
  std::unordered_set<std::uint64_t> images;
  for (std::uint64_t i = 0; i < 20000; ++i) {
    REQUIRE(images.insert(mix64(i)).second);
  }
  REQUIRE(images.size() == 20000);
}

TEST_CASE("mix64 removes the additive cancellation a raw stamp sum suffers") {
  // The exact collision Decision 3 names: two reachable inputs at stamps 7 and 3, and a
  // later configuration of the same graph at 6 and 4. The raw sums are equal -- so a raw
  // fold would serve the first configuration's composed tile for the second. Mixing
  // before summing separates them.
  REQUIRE(7 + 3 == 6 + 4);
  REQUIRE(mix64(7) + mix64(3) != mix64(6) + mix64(4));

  // The same cancellation at a larger opposite-direction move (an undo/redo interleaving
  // that walks one input's stamp down by exactly what another's walks up).
  REQUIRE(mix64(100) + mix64(1) != mix64(51) + mix64(50));
}

TEST_CASE("mix64 is a pure constexpr function of its input") {
  // Order-independence of the fold rests on the mix being a pure function -- the same
  // stamp mixes to the same image every time, with no state and no allocation. Pinned as
  // a constant expression so a regression to anything stateful fails to compile.
  static_assert(mix64(42) == mix64(42), "mix64 must be a pure function");
  static_assert(mix64(0) == 0, "the splitmix64 finalizer fixes zero");
  REQUIRE(mix64(42) == mix64(42));
}
