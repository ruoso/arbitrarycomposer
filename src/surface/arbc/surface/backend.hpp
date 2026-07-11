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

  // Format/space conversion (doc 07 rule 4; doc 09 "the conversion operation").
  // Rewrite `src`'s pixels into `dst`'s tag triple: same geometry,
  // position-for-position, REPLACING every destination pixel. It blends nothing
  // and takes neither a transform nor an opacity -- a transcode, kept separate
  // from the composite operation set above. Conversion routes format ->
  // premultiplied linear working float -> format (doc 07:104-108), so a backend
  // needs 2N codecs rather than N*N kernels, and equal tags are an exact copy,
  // never a decode/encode round-trip.
  //
  // Infallible: the format set is closed and core-owned (doc 07:110-115), so the
  // dispatch over the directed format pairs is total, and the only real failure
  // -- can this backend store that tag at all? -- is already a value out of
  // `make_surface`, which the caller must have gotten past to hold two live
  // surfaces. `dst` dims must equal `src` dims; a mismatch is a caller error
  // (debug assert, release cull), like the conventions above.
  //
  // This is the operation doc 07 rule 4's nesting boundary uses (the child's
  // composed output converts into the parent's working space), and the one the
  // import and display-out edges reuse -- so it carries no caller-specific
  // parameters.
  virtual void convert(Surface& dst, const Surface& src) = 0;

protected:
  Backend() = default;
};

} // namespace arbc
