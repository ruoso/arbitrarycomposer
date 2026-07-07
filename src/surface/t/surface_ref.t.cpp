#include <arbc/media/surface_format.hpp>
#include <arbc/surface/surface.hpp>
#include <arbc/surface/surface_ref.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <span>
#include <utility>

namespace {

// A minimal L2-clean surface carrying only a size + format tag -- SurfaceRef
// never touches pixels, it only refcounts the handle and fires the release
// callback. No CPU storage.
class StubSurface final : public arbc::Surface {
public:
  StubSurface(int width, int height, arbc::SurfaceFormat format)
      : d_width(width), d_height(height), d_format(format) {}

  int width() const override { return d_width; }
  int height() const override { return d_height; }
  arbc::SurfaceFormat format() const override { return d_format; }
  std::span<std::byte> cpu_bytes() override { return {}; }
  std::span<const std::byte> cpu_bytes() const override { return {}; }

private:
  int d_width;
  int d_height;
  arbc::SurfaceFormat d_format;
};

constexpr arbc::SurfaceFormat k_fmt = arbc::k_working_rgba32f;

// enforces: 09-surfaces-and-backends#provided-surface-released-after-consume
TEST_CASE("a SurfaceRef fires its release callback exactly once when the last reference drops") {
  StubSurface surface(16, 8, k_fmt);
  int releases = 0;

  {
    arbc::SurfaceRef ref(surface, [&releases] { ++releases; });
    REQUIRE(releases == 0);
    {
      // Copy: now two references share one control block.
      arbc::SurfaceRef copy = ref; // NOLINT(performance-unnecessary-copy-initialization)
      REQUIRE(releases == 0);      // first ref still live -> no release
    }
    REQUIRE(releases == 0); // one of two dropped -> still not the last
  }
  REQUIRE(releases == 1); // last reference dropped -> released exactly once
}

// enforces: 09-surfaces-and-backends#provided-surface-released-after-consume
TEST_CASE("moving a SurfaceRef transfers the reference without double-release or leak") {
  StubSurface surface(16, 8, k_fmt);
  int releases = 0;

  {
    arbc::SurfaceRef ref(surface, [&releases] { ++releases; });
    arbc::SurfaceRef moved = std::move(ref); // the moved-from handle holds no reference
    REQUIRE(releases == 0);                  // exactly one live reference across the move
    // `ref` (moved-from) must NOT release when it dies at scope end.
  }
  REQUIRE(releases == 1); // released once by the surviving handle, never double-dropped
}

TEST_CASE("the transient flag round-trips and is queryable on the handle") {
  StubSurface surface(16, 8, k_fmt);

  const arbc::SurfaceRef opaque(surface, nullptr, /*transient=*/false);
  const arbc::SurfaceRef live(surface, nullptr, /*transient=*/true);
  REQUIRE_FALSE(opaque.transient());
  REQUIRE(live.transient());

  // Default is non-transient (the common, cacheable-directly case).
  const arbc::SurfaceRef defaulted(surface, nullptr);
  REQUIRE_FALSE(defaulted.transient());
}

TEST_CASE("the wrapped Surface is reachable while any reference is live") {
  StubSurface surface(24, 12, k_fmt);
  const arbc::SurfaceRef ref(surface, nullptr);

  REQUIRE(&ref.surface() == &surface);
  REQUIRE(ref.surface().width() == 24);
  REQUIRE(ref.surface().height() == 12);
  REQUIRE(ref.surface().format() == k_fmt);
}

TEST_CASE("a null release callback is permitted and fires nothing") {
  StubSurface surface(4, 4, k_fmt);
  {
    arbc::SurfaceRef ref(surface, nullptr);
  }
  SUCCEED("no teardown obligation: destruction is a no-op, never UB");
}

} // namespace
