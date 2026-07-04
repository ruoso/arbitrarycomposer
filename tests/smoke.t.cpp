#include <catch2/catch_test_macros.hpp>

// The doc 16 bootstrap sequence, commit 1: the harness exists and passes
// before any feature code does.
TEST_CASE("test harness is alive") { REQUIRE(true); }
