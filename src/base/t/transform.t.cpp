#include <arbc/base/transform.hpp>

#include <catch2/catch_test_macros.hpp>

namespace {

TEST_CASE("compose applies inner first") {
  const arbc::Affine t =
      arbc::compose(arbc::Affine::translation(2.0, 2.0), arbc::Affine::scaling(4.0, 4.0));
  const arbc::Vec2 p = t.apply({1.0, 1.0});
  REQUIRE(p.x == 6.0);
  REQUIRE(p.y == 6.0);
}

TEST_CASE("inverse round-trips exactly for power-of-two scales") {
  const arbc::Affine t =
      arbc::compose(arbc::Affine::translation(3.0, -7.0), arbc::Affine::scaling(4.0, 0.5));
  const auto inv = t.inverse();
  REQUIRE(inv.has_value());
  const arbc::Vec2 p = inv->apply(t.apply({5.0, 9.0}));
  REQUIRE(p.x == 5.0);
  REQUIRE(p.y == 9.0);
}

TEST_CASE("degenerate transforms have no inverse") {
  REQUIRE(!arbc::Affine::scaling(0.0, 1.0).inverse().has_value());
}

TEST_CASE("max_scale is the larger singular value") {
  REQUIRE(arbc::Affine::scaling(3.0, 2.0).max_scale() == 3.0);
  REQUIRE(arbc::Affine::scaling(1.0, -5.0).max_scale() == 5.0);
}

TEST_CASE("map_rect bounds the mapped corners") {
  const arbc::Rect r =
      arbc::compose(arbc::Affine::translation(1.0, 2.0), arbc::Affine::scaling(2.0, 3.0))
          .map_rect({0.0, 0.0, 1.0, 1.0});
  REQUIRE(r == arbc::Rect{1.0, 2.0, 3.0, 5.0});
}

} // namespace
