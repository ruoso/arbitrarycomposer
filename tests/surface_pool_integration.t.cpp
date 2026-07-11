#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/compositor/compositor.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/runtime/document.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <utility>

namespace {

// A counting decorator over a real Backend: it forwards every call unchanged
// but tallies make_surface, so a test can assert allocation *behavior* (a
// behavioral counter, doc 16 -- never a wall-clock timing).
class CountingBackend final : public arbc::Backend {
public:
  explicit CountingBackend(arbc::Backend& inner) : d_inner(inner) {}

  arbc::BackendCaps capabilities() const override { return d_inner.capabilities(); }

  arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError>
  make_surface(int width, int height, arbc::SurfaceFormat format) override {
    ++make_surface_calls;
    return d_inner.make_surface(width, height, format);
  }

  void clear(arbc::Surface& surface, float r, float g, float b, float a) override {
    d_inner.clear(surface, r, g, b, a);
  }
  void composite(arbc::Surface& dst, const arbc::Surface& src, const arbc::Affine& src_to_dst,
                 double opacity) override {
    d_inner.composite(dst, src, src_to_dst, opacity);
  }
  void downsample(arbc::Surface& dst, const arbc::Surface& src) override {
    d_inner.downsample(dst, src);
  }
  void convert(arbc::Surface& dst, const arbc::Surface& src) override { d_inner.convert(dst, src); }

  int make_surface_calls = 0;

private:
  arbc::Backend& d_inner;
};

// enforces: 09-surfaces-and-backends#surface-pool-recycles
TEST_CASE("a persistent pool reuses temps within and across frames") {
  // Scene on an 8x8 identity-camera viewport:
  //   two unit squares scaled 4x  -> two 4x4 temps (same key)
  //   one unit square  scaled 2x  -> one 2x2 temp  (distinct key)
  // Two distinct temp sizes across three layers.
  arbc::Document document;
  const arbc::Rect unit{0.0, 0.0, 1.0, 1.0};
  const arbc::ObjectId a = document.add_content(
      std::make_shared<arbc::SolidContent>(arbc::Rgba{1.0F, 0.0F, 0.0F, 1.0F}, unit));
  document.add_layer(a,
                     compose(arbc::Affine::translation(0.0, 0.0), arbc::Affine::scaling(4.0, 4.0)));
  const arbc::ObjectId b = document.add_content(
      std::make_shared<arbc::SolidContent>(arbc::Rgba{0.0F, 1.0F, 0.0F, 1.0F}, unit));
  document.add_layer(b,
                     compose(arbc::Affine::translation(4.0, 0.0), arbc::Affine::scaling(4.0, 4.0)));
  const arbc::ObjectId c = document.add_content(
      std::make_shared<arbc::SolidContent>(arbc::Rgba{0.0F, 0.0F, 1.0F, 1.0F}, unit));
  document.add_layer(c,
                     compose(arbc::Affine::translation(0.0, 5.0), arbc::Affine::scaling(2.0, 2.0)));

  arbc::CpuBackend cpu;
  CountingBackend backend(cpu);
  const arbc::Viewport viewport{8, 8, arbc::Affine::identity()};
  const arbc::DocStatePtr state = document.pin();
  const auto resolve = [&document](arbc::ObjectId id) { return document.resolve(id); };

  // The frame target is allocated once, up front; reset the counter so only
  // per-layer temp allocations are measured.
  arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError> target =
      backend.make_surface(viewport.width, viewport.height, arbc::k_working_rgba32f);
  REQUIRE(target.has_value());

  arbc::SurfacePool pool(backend); // persistent across both frames

  backend.make_surface_calls = 0;
  render_frame(*state, resolve, viewport, backend, pool, **target);
  // Within-frame recycle: three layers, two distinct temp sizes -> exactly two
  // allocations. The two 4x4 temps share one surface (the first releases before
  // the second acquires), so it is not three.
  REQUIRE(backend.make_surface_calls == 2);

  render_frame(*state, resolve, viewport, backend, pool, **target);
  // Cross-frame recycle: the identical scene reuses every temp from the free
  // list -- zero additional allocations.
  REQUIRE(backend.make_surface_calls == 2);
}

} // namespace
