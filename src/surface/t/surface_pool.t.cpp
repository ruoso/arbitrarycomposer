#include <arbc/surface/surface_pool.hpp>

#include <arbc/media/surface_format.hpp>
#include <arbc/surface/backend.hpp>
#include <arbc/surface/surface.hpp>
#include <arbc/surface/surface_error.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <span>
#include <utility>

namespace {

// A minimal L2-clean surface: it carries a size + format tag and bumps a
// shared live-count on construction/destruction so a test can prove a
// released surface is recycled (not freed) and a moved handle does not
// double-release. No CPU storage -- the pool never touches pixels.
class StubSurface final : public arbc::Surface {
public:
  StubSurface(int width, int height, arbc::SurfaceFormat format, int& live)
      : d_width(width), d_height(height), d_format(format), d_live(live) {
    ++d_live;
  }
  ~StubSurface() override { --d_live; }

  StubSurface(const StubSurface&) = delete;
  StubSurface& operator=(const StubSurface&) = delete;

  int width() const override { return d_width; }
  int height() const override { return d_height; }
  arbc::SurfaceFormat format() const override { return d_format; }
  std::span<float> cpu_pixels() override { return {}; }
  std::span<const float> cpu_pixels() const override { return {}; }

private:
  int d_width;
  int d_height;
  arbc::SurfaceFormat d_format;
  int& d_live;
};

// In-test backend (not CpuBackend, which is L3): counts make_surface calls,
// stores only k_working_rgba32f, and yields SurfaceError for anything else so
// the pool's miss-path forwarding is exercised without an L3 dependency.
class StubBackend final : public arbc::Backend {
public:
  arbc::BackendCaps capabilities() const override { return {}; }

  arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError>
  make_surface(int width, int height, arbc::SurfaceFormat format) override {
    if (format != arbc::k_working_rgba32f) {
      return arbc::unexpected(arbc::SurfaceError::UnsupportedFormat);
    }
    ++make_surface_calls;
    std::unique_ptr<arbc::Surface> surface =
        std::make_unique<StubSurface>(width, height, format, live);
    return surface;
  }

  void clear(arbc::Surface&, float, float, float, float) override {}
  void composite(arbc::Surface&, const arbc::Surface&, const arbc::Affine&, double) override {}

  int make_surface_calls = 0; // total backend allocations
  int live = 0;               // StubSurfaces currently constructed
};

// A supported and an unsupported key (the latter differs on the premul tag).
constexpr arbc::SurfaceFormat k_ok = arbc::k_working_rgba32f;
constexpr arbc::SurfaceFormat k_bad{arbc::PixelFormat::Rgba32fLinearPremul, arbc::k_linear_srgb,
                                    arbc::Premultiplied::No};

TEST_CASE("acquire miss allocates exactly once and yields the requested surface") {
  StubBackend backend;
  arbc::SurfacePool pool(backend);

  arbc::expected<arbc::PooledSurface, arbc::SurfaceError> handle = pool.acquire(16, 8, k_ok);
  REQUIRE(handle.has_value());
  REQUIRE(backend.make_surface_calls == 1);
  REQUIRE(backend.live == 1);
  REQUIRE(handle->get().width() == 16);
  REQUIRE(handle->get().height() == 8);
  REQUIRE(handle->get().format() == k_ok);
}

// enforces: 09-surfaces-and-backends#surface-pool-recycles
TEST_CASE("a released surface is recycled for a same-key acquire") {
  StubBackend backend;
  arbc::SurfacePool pool(backend);

  arbc::Surface* first = nullptr;
  {
    arbc::expected<arbc::PooledSurface, arbc::SurfaceError> handle = pool.acquire(16, 8, k_ok);
    REQUIRE(handle.has_value());
    first = &handle->get();
  } // release: surface returns to the free list, not freed
  REQUIRE(backend.make_surface_calls == 1);
  REQUIRE(backend.live == 1); // still alive, parked in the pool

  arbc::expected<arbc::PooledSurface, arbc::SurfaceError> again = pool.acquire(16, 8, k_ok);
  REQUIRE(again.has_value());
  REQUIRE(backend.make_surface_calls == 1); // recycled, no new allocation
  REQUIRE(&again->get() == first);          // the very same surface
}

TEST_CASE("distinct keys allocate distinct surfaces") {
  StubBackend backend;
  arbc::SurfacePool pool(backend);

  arbc::expected<arbc::PooledSurface, arbc::SurfaceError> a = pool.acquire(16, 8, k_ok);
  arbc::expected<arbc::PooledSurface, arbc::SurfaceError> b = pool.acquire(32, 8, k_ok);
  REQUIRE(a.has_value());
  REQUIRE(b.has_value());
  REQUIRE(backend.make_surface_calls == 2);
  REQUIRE(&a->get() != &b->get());
}

TEST_CASE("two concurrent live handles of one key both allocate") {
  StubBackend backend;
  arbc::SurfacePool pool(backend);

  // The free list only recycles *released* surfaces, so a second acquire of a
  // still-live key must allocate rather than hand back the live one.
  arbc::expected<arbc::PooledSurface, arbc::SurfaceError> a = pool.acquire(16, 8, k_ok);
  arbc::expected<arbc::PooledSurface, arbc::SurfaceError> b = pool.acquire(16, 8, k_ok);
  REQUIRE(a.has_value());
  REQUIRE(b.has_value());
  REQUIRE(backend.make_surface_calls == 2);
  REQUIRE(&a->get() != &b->get());
}

TEST_CASE("acquire forwards the backend's SurfaceError on an unsupported format") {
  StubBackend backend;
  arbc::SurfacePool pool(backend);

  arbc::expected<arbc::PooledSurface, arbc::SurfaceError> handle = pool.acquire(16, 8, k_bad);
  REQUIRE_FALSE(handle.has_value());
  REQUIRE(handle.error() == arbc::SurfaceError::UnsupportedFormat);
  REQUIRE(backend.make_surface_calls == 0);
  REQUIRE(backend.live == 0);
}

TEST_CASE("moving a handle transfers the release obligation without double-release or leak") {
  StubBackend backend;
  arbc::SurfacePool pool(backend);

  {
    arbc::expected<arbc::PooledSurface, arbc::SurfaceError> handle = pool.acquire(16, 8, k_ok);
    REQUIRE(handle.has_value());
    REQUIRE(backend.live == 1);

    arbc::PooledSurface moved = std::move(*handle);
    REQUIRE(backend.live == 1); // exactly one live surface across the move
    // `handle`'s moved-from PooledSurface must not release again when it dies.
  }
  REQUIRE(backend.make_surface_calls == 1);
  REQUIRE(backend.live == 1); // released once into the pool, still alive

  arbc::expected<arbc::PooledSurface, arbc::SurfaceError> reacquired = pool.acquire(16, 8, k_ok);
  REQUIRE(reacquired.has_value());
  REQUIRE(backend.make_surface_calls == 1); // recycled the moved-then-released surface
}

TEST_CASE("move-assignment releases the overwritten surface to the pool") {
  StubBackend backend;
  arbc::SurfacePool pool(backend);

  arbc::expected<arbc::PooledSurface, arbc::SurfaceError> a = pool.acquire(16, 8, k_ok);
  arbc::expected<arbc::PooledSurface, arbc::SurfaceError> b = pool.acquire(32, 8, k_ok);
  REQUIRE(a.has_value());
  REQUIRE(b.has_value());
  REQUIRE(backend.make_surface_calls == 2);
  REQUIRE(backend.live == 2);

  *a = std::move(*b); // a's 16x8 surface returns to the pool; a now holds 32x8
  REQUIRE(backend.live == 2); // one live (32x8), one parked (16x8)
  REQUIRE(a->get().width() == 32);

  arbc::expected<arbc::PooledSurface, arbc::SurfaceError> c = pool.acquire(16, 8, k_ok);
  REQUIRE(c.has_value());
  REQUIRE(backend.make_surface_calls == 2); // recycled the released 16x8
}

} // namespace
