#pragma once

#include <arbc/base/expected.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/surface/capabilities.hpp>
#include <arbc/surface/surface.hpp>
#include <arbc/surface/surface_error.hpp>

#include <memory>

namespace arbc {

// Backend contract (doc 09): surfaces and the composite operation set. The
// core never loops over pixels itself; all composite operations route
// through here so the abstraction maps onto GPU command lists later.
class Backend {
public:
  Backend(const Backend&) = delete;
  Backend& operator=(const Backend&) = delete;
  virtual ~Backend();

  // Advertise the backend's capability flags (doc 09): whether typed CPU
  // access is available, which external handle types it can import, and
  // whether it offers sync primitives. A backend reports only what it
  // currently implements (capability honesty, doc 07/09).
  virtual BackendCaps capabilities() const = 0;

  // Allocate a surface carrying `format`'s full tag triple. Errors as values
  // (doc 10): a backend that cannot store that format returns a SurfaceError,
  // never a null handle and never an abort (capability honesty, doc 07).
  virtual expected<std::unique_ptr<Surface>, SurfaceError> make_surface(int width, int height,
                                                                        SurfaceFormat format) = 0;

  // Premultiplied working-space color (doc 07).
  virtual void clear(Surface& surface, float r, float g, float b, float a) = 0;

  // Source-over composite of `src` onto `dst` under the src->dst mapping,
  // on premultiplied alpha, scaled by `opacity`.
  virtual void composite(Surface& dst, const Surface& src, const Affine& src_to_dst,
                         double opacity) = 0;

  // Box-filtered exact 2:1 downsample of `src` into `dst` (doc 09:18's
  // "backend-internal ... resample operation consumed by the compositor"):
  // `dst` dims are `src` dims / 2 (even source dims), same format, the mean
  // taken in the decoded premultiplied linear working space (doc 07 rule 3).
  // The scale ladder (compositor) uses this to build coarser rungs; rung
  // selection is not the backend's concern.
  virtual void downsample(Surface& dst, const Surface& src) = 0;

protected:
  Backend() = default;
};

} // namespace arbc
