#pragma once

#include <arbc/base/expected.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/surface/backend.hpp>
#include <arbc/surface/capabilities.hpp>
#include <arbc/surface/surface.hpp>
#include <arbc/surface/surface_error.hpp>

#include <memory>

namespace arbc::testing {

// A `Backend` decorator over an injected inner backend: it delegates EVERY
// operation and adds no pixel behavior of its own. Test doubles that observe or
// perturb a real backend -- counting allocations, refusing one format -- derive
// from it and override ONLY the operations they observe; the rest keep forwarding
// so a real render still happens (doc 09 "Test doubles are part of the contract").
//
// This is the sibling of `StubBackend` and its opposite shape, and the difference
// is load-bearing. Both exist so the `Backend` contract can grow an operation
// without rippling a dead stub into every double in the tree, but they absorb that
// new operation differently, and each is correct for its shape: a stub no-ops it
// (a test that drives no real pixels cannot notice), while a decorator MUST forward
// it. A decorator that inherited no-op defaults would silently swallow an operation
// its inner backend implements -- wrong pixels, quietly, with every test still
// green until a golden happens to notice. Delegation fails loudly instead.
//
// The inner backend is held by reference, never owned: base classes initialize
// before members, so a double owning its inner as a member could not pass it to
// this ctor without binding a reference into not-yet-constructed storage. Callers
// own the inner and outlive the decorator. The reference is also what keeps this
// L2 header free of any dependency on a concrete backend (`CpuBackend` is L3).
class ForwardingBackend : public Backend {
public:
  explicit ForwardingBackend(Backend& inner) : d_inner(inner) {}

  BackendCaps capabilities() const override { return d_inner.capabilities(); }

  expected<std::unique_ptr<Surface>, SurfaceError> make_surface(int width, int height,
                                                                SurfaceFormat format) override {
    return d_inner.make_surface(width, height, format);
  }

  void clear(Surface& surface, float r, float g, float b, float a) override {
    d_inner.clear(surface, r, g, b, a);
  }

  void composite(Surface& dst, const Surface& src, const Affine& src_to_dst,
                 double opacity) override {
    d_inner.composite(dst, src, src_to_dst, opacity);
  }

  void clear_rect(Surface& dst, const Rect& device_rect, float r, float g, float b,
                  float a) override {
    d_inner.clear_rect(dst, device_rect, r, g, b, a);
  }

  void composite_clipped(Surface& dst, const Surface& src, const Affine& src_to_dst, double opacity,
                         const Rect& device_clip) override {
    d_inner.composite_clipped(dst, src, src_to_dst, opacity, device_clip);
  }

  void downsample(Surface& dst, const Surface& src) override { d_inner.downsample(dst, src); }

  void convert(Surface& dst, const Surface& src) override { d_inner.convert(dst, src); }

  expected<std::unique_ptr<Surface>, SurfaceError>
  import_cpu_memory(const CpuImport& import) override {
    return d_inner.import_cpu_memory(import);
  }

protected:
  // The backend being decorated, for a subclass that overrides an operation and
  // still wants to pass it through after observing it.
  Backend& inner() const { return d_inner; }

private:
  Backend& d_inner;
};

} // namespace arbc::testing
