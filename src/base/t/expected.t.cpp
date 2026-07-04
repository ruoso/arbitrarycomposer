#include <arbc/base/expected.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>

namespace {

enum class Err { Boom, Other };

TEST_CASE("expected holds a value") {
  arbc::expected<int, Err> e = 42;
  REQUIRE(e.has_value());
  REQUIRE(static_cast<bool>(e));
  REQUIRE(*e == 42);
  REQUIRE(e.value() == 42);
}

TEST_CASE("expected holds an error") {
  arbc::expected<int, Err> e = arbc::unexpected<Err>(Err::Boom);
  REQUIRE_FALSE(e.has_value());
  REQUIRE_FALSE(static_cast<bool>(e));
  REQUIRE(e.error() == Err::Boom);
}

TEST_CASE("expected copies and moves preserve state") {
  arbc::expected<std::string, Err> value = std::string("hello");
  arbc::expected<std::string, Err> copy = value;
  REQUIRE(copy.has_value());
  REQUIRE(*copy == "hello");

  arbc::expected<std::string, Err> moved = std::move(value);
  REQUIRE(moved.has_value());
  REQUIRE(*moved == "hello");

  arbc::expected<std::string, Err> err = arbc::unexpected<Err>(Err::Other);
  arbc::expected<std::string, Err> err_copy = err;
  REQUIRE_FALSE(err_copy.has_value());
  REQUIRE(err_copy.error() == Err::Other);
}

TEST_CASE("expected assignment switches the active alternative") {
  arbc::expected<int, Err> e = 1;
  e = arbc::unexpected<Err>(Err::Boom);
  REQUIRE_FALSE(e.has_value());
  REQUIRE(e.error() == Err::Boom);
  e = 7;
  REQUIRE(e.has_value());
  REQUIRE(*e == 7);
}

namespace {
int g_live = 0;
struct Tracked {
  Tracked() { ++g_live; }
  Tracked(const Tracked&) { ++g_live; }
  Tracked(Tracked&&) noexcept { ++g_live; }
  Tracked& operator=(const Tracked&) = default;
  Tracked& operator=(Tracked&&) noexcept = default;
  ~Tracked() { --g_live; }
};
} // namespace

TEST_CASE("expected destroys its non-trivial value exactly once") {
  g_live = 0;
  {
    arbc::expected<Tracked, Err> e = Tracked{};
    REQUIRE(e.has_value());
    REQUIRE(g_live == 1);
  }
  REQUIRE(g_live == 0);
}

} // namespace
