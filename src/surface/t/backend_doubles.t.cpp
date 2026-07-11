#include <arbc/base/transform.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/surface/capabilities.hpp>
#include <arbc/surface/surface.hpp>
#include <arbc/surface/surface_error.hpp>
#include <arbc/surface/testing/counting_backend.hpp>
#include <arbc/surface/testing/forwarding_backend.hpp>
#include <arbc/surface/testing/stub_backend.hpp>
#include <arbc/surface/testing/stub_surface.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>

namespace {

// The inner backend the decorators under test delegate to. It is a StubBackend
// subclass rather than CpuBackend because `surface` is L2 and `backend_cpu` is L3
// (doc 17's levelization table), and because the point here is not what the pixels
// become -- it is that the operation ARRIVED at all. Every operation records that
// it was received, so a decorator that quietly swallowed one is caught.
//
// `make_surface` hands back a `testing::StubSurface` for the working format and the
// stub's own `UnsupportedFormat` for anything else, which is what lets the
// error-propagation case below be checked without a real allocator.
class RecordingBackend final : public arbc::testing::StubBackend {
public:
  // The distinctive capability report the forward must carry back verbatim: an
  // inherited `StubBackend::capabilities()` would answer an empty `{}` instead.
  static constexpr arbc::BackendCaps k_caps{/*cpu_access=*/true, arbc::ImportHandle::CpuMemory,
                                            /*sync_primitives=*/true};

  arbc::BackendCaps capabilities() const override {
    ++capabilities_seen;
    return k_caps;
  }

  arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError>
  make_surface(int width, int height, arbc::SurfaceFormat format) override {
    if (format != arbc::k_working_rgba32f) {
      return arbc::unexpected(arbc::SurfaceError::UnsupportedFormat);
    }
    ++make_surface_seen;
    std::unique_ptr<arbc::Surface> surface =
        std::make_unique<arbc::testing::StubSurface>(width, height, format, &live);
    return surface;
  }

  void clear(arbc::Surface&, float r, float, float, float) override {
    ++clear_seen;
    clear_red = r;
  }

  void composite(arbc::Surface&, const arbc::Surface&, const arbc::Affine&,
                 double opacity) override {
    ++composite_seen;
    composite_opacity = opacity;
  }

  void downsample(arbc::Surface&, const arbc::Surface&) override { ++downsample_seen; }

  void convert(arbc::Surface&, const arbc::Surface&) override { ++convert_seen; }

  mutable int capabilities_seen = 0;
  int make_surface_seen = 0;
  int clear_seen = 0;
  int composite_seen = 0;
  int downsample_seen = 0;
  int convert_seen = 0;
  int live = 0;
  float clear_red = 0.0F;
  double composite_opacity = 0.0;
};

// An unsupported key (it differs from the working format on the premul tag).
constexpr arbc::SurfaceFormat k_unstorable{arbc::PixelFormat::Rgba32fLinearPremul,
                                           arbc::k_linear_srgb, arbc::Premultiplied::No};

// enforces: 09-surfaces-and-backends#forwarding-double-delegates-every-op
TEST_CASE("a counting decorator forwards every Backend operation to its inner backend") {
  RecordingBackend inner;
  arbc::testing::CountingBackend backend(inner);

  // Drive each of the six operations exactly once through the decorator.
  const arbc::BackendCaps caps = backend.capabilities();

  arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError> surface =
      backend.make_surface(16, 8, arbc::k_working_rgba32f);
  REQUIRE(surface.has_value());
  arbc::testing::StubSurface other(16, 8, arbc::k_working_rgba32f);

  backend.clear(**surface, 0.25F, 0.5F, 0.75F, 1.0F);
  backend.composite(**surface, other, arbc::Affine::identity(), 0.5);
  backend.downsample(**surface, other);
  backend.convert(**surface, other);

  // Each operation was counted exactly once ...
  CHECK(backend.capabilities_calls == 1);
  CHECK(backend.make_surface_calls == 1);
  CHECK(backend.clear_calls == 1);
  CHECK(backend.composite_calls == 1);
  CHECK(backend.downsample_calls == 1);
  CHECK(backend.convert_calls == 1);

  // ... and, decisively, each one ARRIVED at the inner backend. This is the whole
  // promise: a decorator observes an operation, it does not absorb it. A base with
  // no-op defaults would leave every one of these at zero while the counters above
  // still read 1.
  CHECK(inner.capabilities_seen == 1);
  CHECK(inner.make_surface_seen == 1);
  CHECK(inner.clear_seen == 1);
  CHECK(inner.composite_seen == 1);
  CHECK(inner.downsample_seen == 1);
  CHECK(inner.convert_seen == 1);

  // The forwards carry their arguments and their results through unmangled.
  CHECK(caps == RecordingBackend::k_caps);
  CHECK(inner.clear_red == 0.25F);
  CHECK(inner.composite_opacity == 0.5);
  CHECK((*surface)->width() == 16);
  CHECK((*surface)->height() == 8);
  CHECK((*surface)->format() == arbc::k_working_rgba32f);
  CHECK(inner.live == 1); // the inner's StubSurface, handed back by the decorator

  backend.reset();
  CHECK(backend.capabilities_calls == 0);
  CHECK(backend.make_surface_calls == 0);
  CHECK(backend.clear_calls == 0);
  CHECK(backend.composite_calls == 0);
  CHECK(backend.downsample_calls == 0);
  CHECK(backend.convert_calls == 0);
}

// enforces: 09-surfaces-and-backends#forwarding-double-delegates-every-op
TEST_CASE("a forwarding decorator propagates the inner backend's allocation fault as a value") {
  RecordingBackend inner;
  arbc::testing::ForwardingBackend backend(inner);

  arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError> refused =
      backend.make_surface(16, 8, k_unstorable);
  REQUIRE_FALSE(refused.has_value());
  CHECK(refused.error() == arbc::SurfaceError::UnsupportedFormat);
  CHECK(inner.make_surface_seen == 0); // the inner refused before allocating
  CHECK(inner.live == 0);
}

// The stub surface's own contract: it carries its tag triple, holds no pixels, and
// its live-count knob is what lets a pool test prove a surface was recycled rather
// than freed. The `live` pointer is optional -- a double that does not care omits it.
TEST_CASE("StubSurface carries its tag triple, holds no pixels, and tracks liveness") {
  int live = 0;
  {
    const arbc::testing::StubSurface surface(32, 16, arbc::k_working_rgba32f, &live);
    CHECK(live == 1);
    CHECK(surface.width() == 32);
    CHECK(surface.height() == 16);
    CHECK(surface.format() == arbc::k_working_rgba32f);
    CHECK(surface.cpu_bytes().empty());

    arbc::testing::StubSurface mutable_surface(4, 4, arbc::k_working_rgba32f, &live);
    CHECK(live == 2);
    CHECK(mutable_surface.cpu_bytes().empty());
  }
  CHECK(live == 0);

  const arbc::testing::StubSurface untracked(8, 8, arbc::k_working_rgba32f);
  CHECK(untracked.width() == 8);
  CHECK(live == 0); // no knob passed, nothing counted
}

} // namespace
