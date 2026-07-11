#pragma once

#include <arbc/base/expected.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/surface/backend.hpp>
#include <arbc/surface/surface.hpp>
#include <arbc/surface/surface_error.hpp>
#include <arbc/surface/testing/forwarding_backend.hpp>

#include <memory>

namespace arbc::testing {

// A `ForwardingBackend` that tallies one call per `Backend` operation and forwards
// each through the base, so a test can assert allocation/conversion *behavior* --
// the behavioral counters doc 16:54-62 asks for in place of a wall-clock timing.
// Counting is the overwhelmingly common decorator, so the component ships it rather
// than letting each test re-open-code it.
//
// The counters are strictly additive: a test asserts the ones it cares about and
// ignores the rest. They are plain `int`s with no synchronization -- a double is
// constructed and read on the test thread, and nothing here is thread-safe. A test
// that drives a counting double from the worker pool owns that problem itself.
class CountingBackend : public ForwardingBackend {
public:
  explicit CountingBackend(Backend& inner) : ForwardingBackend(inner) {}

  BackendCaps capabilities() const override {
    ++capabilities_calls;
    return ForwardingBackend::capabilities();
  }

  expected<std::unique_ptr<Surface>, SurfaceError> make_surface(int width, int height,
                                                                SurfaceFormat format) override {
    ++make_surface_calls;
    return ForwardingBackend::make_surface(width, height, format);
  }

  void clear(Surface& surface, float r, float g, float b, float a) override {
    ++clear_calls;
    ForwardingBackend::clear(surface, r, g, b, a);
  }

  void composite(Surface& dst, const Surface& src, const Affine& src_to_dst,
                 double opacity) override {
    ++composite_calls;
    ForwardingBackend::composite(dst, src, src_to_dst, opacity);
  }

  void downsample(Surface& dst, const Surface& src) override {
    ++downsample_calls;
    ForwardingBackend::downsample(dst, src);
  }

  void convert(Surface& dst, const Surface& src) override {
    ++convert_calls;
    ForwardingBackend::convert(dst, src);
  }

  void reset() {
    capabilities_calls = 0;
    make_surface_calls = 0;
    clear_calls = 0;
    composite_calls = 0;
    downsample_calls = 0;
    convert_calls = 0;
  }

  mutable int capabilities_calls = 0;
  int make_surface_calls = 0;
  int clear_calls = 0;
  int composite_calls = 0;
  int downsample_calls = 0;
  int convert_calls = 0;
};

} // namespace arbc::testing
