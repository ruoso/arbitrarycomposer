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

// A `Backend` with every operation stubbed out: no capabilities, no storage, and
// three no-op pixel operations. Test doubles derive from it and override ONLY the
// operations the code under test actually drives.
//
// This exists so the `Backend` contract can grow an operation without rippling a
// dead stub into every double in the tree. A double that spells out a no-op for
// an operation its test never calls is not documenting anything -- it is
// answering the compiler -- and each such line is unreachable by construction,
// which is exactly the code the diff-coverage gate (doc 16) is right to reject.
// Inheriting the no-op says the same thing once, in the place that owns the
// contract.
//
// `make_surface` defaults to `UnsupportedFormat` rather than to a fabricated
// surface: errors as values (doc 09/10), and a stub that cannot store anything is
// the honest default for a double that has not been told how to allocate.
class StubBackend : public Backend {
public:
  BackendCaps capabilities() const override { return {}; }

  expected<std::unique_ptr<Surface>, SurfaceError> make_surface(int /*width*/, int /*height*/,
                                                                SurfaceFormat /*format*/) override {
    return unexpected(SurfaceError::UnsupportedFormat);
  }

  void clear(Surface& /*surface*/, float /*r*/, float /*g*/, float /*b*/, float /*a*/) override {}

  void composite(Surface& /*dst*/, const Surface& /*src*/, const Affine& /*src_to_dst*/,
                 double /*opacity*/) override {}

  // The clip-scoped operations (doc 09) DELEGATE to the unclipped ones rather than
  // no-oping like the operations above -- the one place this base departs from "stub
  // it out", and deliberately.
  //
  // Doc 09 defines a whole-destination clip as byte-identical to the unclipped
  // operation, so delegation is a faithful reading of the contract for a double that
  // does not model geometry. And it is the only SAFE default here: a subclass that
  // overrides `clear`/`composite` to model pixels (the `MarkBackend` shape, all over
  // the tree) but inherits a no-op `clear_rect`/`composite_clipped` would silently
  // DROP every paint a damage-gated frame makes -- green tests asserting nothing,
  // which is precisely the failure mode `ForwardingBackend`'s header calls out for
  // decorators. A subclass that no-ops `clear`/`composite` (the pure stub) still
  // no-ops these, because the delegation lands on its own no-ops.
  //
  // A double that must observe the CLIP itself -- rather than the paint -- overrides
  // these explicitly (`CountingBackend` does). Clip fidelity proper is asserted
  // against the real `CpuBackend`
  // (`09-surfaces-and-backends#clip-scoped-ops-honor-the-clip`), never against a
  // double, so nothing here can make that claim vacuous.
  void clear_rect(Surface& dst, const Rect& /*device_rect*/, float r, float g, float b,
                  float a) override {
    clear(dst, r, g, b, a);
  }

  void composite_clipped(Surface& dst, const Surface& src, const Affine& src_to_dst, double opacity,
                         const Rect& /*device_clip*/) override {
    composite(dst, src, src_to_dst, opacity);
  }

  void downsample(Surface& /*dst*/, const Surface& /*src*/) override {}

  void convert(Surface& /*dst*/, const Surface& /*src*/) override {}
};

} // namespace arbc::testing
