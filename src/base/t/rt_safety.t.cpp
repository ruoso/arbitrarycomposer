#include <arbc/base/rt_safety.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

using namespace arbc;

namespace {

// A function carrying the portable RT annotation. On Clang it expands to
// `[[clang::nonblocking]]` (RealtimeSanitizer, Layer A); on GCC/MSVC to nothing.
// Either way it must compile under `-Wall -Wextra -Wpedantic -Werror` and run --
// this is the portability guarantee (Constraint 2).
int annotated_add(int a, int b) noexcept ARBC_RT_NONBLOCKING { return a + b; }

} // namespace

TEST_CASE("RtScope arms and disarms the calling thread, nesting correctly") {
  REQUIRE_FALSE(RtScope::armed());
  bool inner_armed = false;
  bool mid_armed = false;
  {
    RtScope outer;
    mid_armed = RtScope::armed();
    {
      RtScope inner;
      inner_armed = RtScope::armed();
    }
    // Still armed after the inner scope leaves (nestable).
    REQUIRE(RtScope::armed());
  }
  REQUIRE(mid_armed);
  REQUIRE(inner_armed);
  REQUIRE_FALSE(RtScope::armed());
}

TEST_CASE("an armed RtScope over a clean, allocation-free body records zero forbidden ops") {
  RtScope::reset_counts();
  int sum = 0;
  {
    RtScope guard;
    // A pure, allocation-free body -- exactly what the RT callback chain must be.
    sum = annotated_add(2, 3);
  }
  REQUIRE(sum == 5);
  REQUIRE(RtScope::allocations() == 0);
  REQUIRE(RtScope::locks() == 0);
  REQUIRE(RtScope::refcounts() == 0);
}

TEST_CASE("the portable RT annotation expands to a compiling, callable function") {
  REQUIRE(annotated_add(40, 2) == 42);
}

#if !defined(ARBC_RT_HARDENED)
// In a NON-hardened build the notes count without aborting, so the counting
// discipline is directly observable (the debug-hardened build turns each of
// these into a build-failing `std::abort`, so it cannot be exercised there).
TEST_CASE("forbidden-operation notes increment the per-thread counters (non-hardened)") {
  RtScope::reset_counts();
  RtScope::note_allocation();
  RtScope::note_allocation();
  RtScope::note_lock();
  RtScope::note_refcount();
  RtScope::note_refcount();
  RtScope::note_refcount();
  REQUIRE(RtScope::allocations() == 2);
  REQUIRE(RtScope::locks() == 1);
  REQUIRE(RtScope::refcounts() == 3);
  RtScope::reset_counts();
  REQUIRE(RtScope::allocations() == 0);
  REQUIRE(RtScope::locks() == 0);
  REQUIRE(RtScope::refcounts() == 0);
}
#endif
