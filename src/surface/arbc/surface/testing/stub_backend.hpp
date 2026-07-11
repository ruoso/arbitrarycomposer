#pragma once

#include <arbc/base/expected.hpp>
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

  void downsample(Surface& /*dst*/, const Surface& /*src*/) override {}

  void convert(Surface& /*dst*/, const Surface& /*src*/) override {}
};

} // namespace arbc::testing
