#pragma once

#include <arbc/media/surface_format.hpp>
#include <arbc/surface/surface.hpp>

#include <cstddef>
#include <span>

namespace arbc::testing {

// A minimal `Surface`: it carries a size + format tag and no pixel storage at all
// (both `cpu_bytes()` overloads yield an empty span, which `Surface` already
// specifies as the honest answer for a surface without CPU access).
//
// It exists so a double can hand back a surface without a real backend behind it --
// which is what lets an L2 test exercise the `Backend`/`SurfacePool` contracts
// without reaching for `CpuBackend` (L3), an edge the levelization table forbids.
//
// The optional `live` pointer is a shared refcount, bumped on construction and
// dropped on destruction, so a test can prove a released surface was recycled (not
// freed) and that a moved handle does not double-release.
class StubSurface final : public Surface {
public:
  StubSurface(int width, int height, SurfaceFormat format, int* live = nullptr)
      : d_width(width), d_height(height), d_format(format), d_live(live) {
    if (d_live != nullptr) {
      ++*d_live;
    }
  }

  ~StubSurface() override {
    if (d_live != nullptr) {
      --*d_live;
    }
  }

  StubSurface(const StubSurface&) = delete;
  StubSurface& operator=(const StubSurface&) = delete;

  int width() const override { return d_width; }
  int height() const override { return d_height; }
  SurfaceFormat format() const override { return d_format; }
  std::span<std::byte> cpu_bytes() override { return {}; }
  std::span<const std::byte> cpu_bytes() const override { return {}; }

private:
  int d_width;
  int d_height;
  SurfaceFormat d_format;
  int* d_live;
};

} // namespace arbc::testing
