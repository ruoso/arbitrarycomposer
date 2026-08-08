#pragma once

#include <arbc/arbc_api.h>
#include <arbc/base/expected.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/media/blend_mode.hpp>
#include <arbc/surface/capabilities.hpp>
#include <arbc/surface/import.hpp>
#include <arbc/surface/surface.hpp>
#include <arbc/surface/surface_error.hpp>

#include <memory>

namespace arbc {

// Backend contract (doc 09): surfaces and the composite operation set. The
// core never loops over pixels itself; all composite operations route
// through here so the abstraction maps onto GPU command lists later.
class ARBC_API Backend {
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

  // Composite of `src` onto `dst` under the src->dst mapping, on premultiplied alpha,
  // scaled by `opacity` and combined by `blend`.
  //
  // `blend` is the per-layer BLEND MODE (issue #36, `arbc/media/blend_mode.hpp`), and
  // `BlendMode::Normal` -- the default every caller had before it existed -- is
  // source-over exactly. It is a REQUIRED parameter rather than a defaulted one because a
  // default argument on a virtual binds statically, which would let a backend and its
  // caller disagree about what an omitted mode meant; the same reason the clip-scoped
  // operations below are distinctly named rather than overloaded.
  //
  // Every backend implements every mode. This is not a capability: the modes are pure
  // arithmetic on the premultiplied working floats a backend must already be able to
  // composite, so there is nothing for `capabilities()` to be honest about, exactly as
  // there is nothing for it to say about `opacity`.
  virtual void composite(Surface& dst, const Surface& src, const Affine& src_to_dst, double opacity,
                         BlendMode blend) = 0;

  // The clip-scoped operations (doc 09 "The clip-scoped operations"): `clear`
  // and `composite` in a second form carrying a device-space (destination-space)
  // clip rect, writing NO pixel outside it. Damage-gated rendering repaints a
  // *region* of a caller-persisted target (doc 02 § The frame, interactively):
  // the region must be cleared before it is re-composited, and the composites
  // must not spill past it, or source-over lands twice on the pixels beyond the
  // clear -- a tile is a whole cache cell, so a tile straddling the region's
  // edge overhangs it.
  //
  // The clip is intersected with the destination's bounds (a clip reaching past
  // the edge is legal, not an error) and is half-open. An empty clip is a no-op;
  // a clip covering the whole destination is byte-identical to the unclipped
  // operation above -- which is how `clear`/`composite` are *defined*, so a
  // backend carries one kernel per operation rather than two. It is a scissor
  // rect: the shape a GPU backend's command list already has.
  //
  // Distinct names rather than a defaulted/overloaded parameter: an overloaded
  // virtual is hidden by any override in a derived backend unless every one of
  // them writes `using Backend::clear;`, and a default argument on a virtual
  // binds statically.
  virtual void clear_rect(Surface& dst, const Rect& device_rect, float r, float g, float b,
                          float a) = 0;
  virtual void composite_clipped(Surface& dst, const Surface& src, const Affine& src_to_dst,
                                 double opacity, BlendMode blend, const Rect& device_clip) = 0;

  // The composite with a SOURCE-space paint window (`compositor.tile_apron`).
  // `device_clip` scopes it exactly as `composite_clipped` does; `src_window` is a
  // half-open rect in `src`'s own PIXEL space, and a destination pixel is painted
  // only if its sample position falls inside it. The tap still reads the whole of
  // `src` -- the window narrows what is PAINTED, never what is SAMPLED.
  //
  // That distinction is the whole point. A tiled compositor must let a tile's
  // resampling tap reach past the tile's edge (or the tap lands on the surface's
  // transparent border and every tile boundary rings) while still painting each
  // destination pixel exactly once (or the overlapping paints double-blend). A
  // destination-space scissor cannot express it: tiles are axis-aligned in LOCAL
  // space, so under rotation or shear adjacent tiles' device footprints are
  // overlapping quads, and no rect partition of the destination exists. The
  // sample position, though, lands in exactly one tile's cell for any invertible
  // affine -- so the partition is exact in source space and nowhere else.
  //
  // Half-open, in the same texel-index convention the resampling tap uses, and
  // intersected with nothing: a window covering all of `src` IS
  // `composite_clipped`, which is how that operation is defined -- one kernel,
  // not two. An empty window is a no-op.
  virtual void composite_windowed(Surface& dst, const Surface& src, const Affine& src_to_dst,
                                  double opacity, BlendMode blend, const Rect& device_clip,
                                  const Rect& src_window) = 0;

  // Exact 2:1 minifying downsample of `src` into `dst` (doc 09:18's
  // "backend-internal ... resample operation consumed by the compositor"):
  // `dst` dims are `src` dims / 2 (even source dims), same format, the reduction
  // taken in the decoded premultiplied linear working space (doc 07 rule 3). The
  // reference CPU backend uses the shared arbc::media Lanczos-3 half-band
  // decimator (doc 07 § Resampling filters); the filter is the backend's choice,
  // not part of this seam. The scale ladder (compositor) uses this to build
  // coarser rungs; rung selection is not the backend's concern.
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

  // Import caller-owned CPU memory as a surface (doc 09:59-61,114-120: "import is
  // wrap-or-copy of caller memory"), gated on the `ImportHandle::CpuMemory`
  // capability bit. Errors as values (doc 10), symmetric with `make_surface`: a
  // backend with no CPU-memory import, or an unstorable source tag, or a `memory`
  // span inconsistent with the declared (width, height, source_format), yields a
  // SurfaceError -- never a null handle, never an abort.
  //
  // The wrap/copy fork is deterministic on the tag (`CpuImport`): equal source
  // and target tags WRAP `import.memory` zero-copy; unequal tags COPY, converting
  // the source into a fresh `target_format` surface at import time so no foreign
  // tag ever reaches the compositor (doc 09:220-230). The returned surface always
  // carries `target_format`. `import.release` fires when the backend is done with
  // `import.memory` -- at the wrapped surface's destruction, or before return on
  // the copy path -- and never at all if the import faults (the caller keeps its
  // memory). CPU memory is the one handle type every backend can accept; a GPU
  // backend adds its own handle-typed import for GL/Vulkan/DMA-BUF.
  virtual expected<std::unique_ptr<Surface>, SurfaceError>
  import_cpu_memory(const CpuImport& import) = 0;

protected:
  Backend() = default;
};

} // namespace arbc
