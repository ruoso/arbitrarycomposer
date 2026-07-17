#pragma once

#include <arbc/arbc_api.h>
#include <arbc/base/ids.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/model/model.hpp>
#include <arbc/surface/backend.hpp>
#include <arbc/surface/surface_pool.hpp>

#include <functional>

namespace arbc {

// The consumer side (doc 01): a device-pixel rectangle plus a camera
// mapping composition space to device pixels.
struct Viewport {
  int width{0};
  int height{0};
  Affine camera;
  // The node the camera is pinned to (doc 04:81-84): `camera` maps this
  // anchor's LOCAL space -> device pixels, not root space, so the transforms the
  // pipeline computes with stay well-conditioned at any zoom depth (doc
  // 04:49-69). The frame walk is composition-scoped (doc 05:28-36): it draws
  // exactly this composition's direct members, reaching a nested child only
  // through the enclosing layer's content. The default (invalid) id means "no
  // composition bound" -- `for_each_layer_in` resolves nothing and the frame is
  // empty; the driver sources the root composition and sets it (Decision 2/3).
  // Rebasing re-picks this as the user zooms; the persistent value lives in
  // runtime (doc 17), the compositor stays a pure per-frame library.
  ObjectId anchor{};

  // Value equality over the whole device mapping `(width, height, camera, anchor)`:
  // what the interactive loop compares against its previous frame to detect
  // camera-change damage (doc 02 § "A camera change is device damage").
  friend constexpr bool operator==(const Viewport&, const Viewport&) = default;
};

// Resolves a content id from the pinned state to its implementation; the
// binding lives in runtime (doc 17), keeping the compositor free of it.
using ContentResolver = std::function<Content*(ObjectId)>;

// One frame under the offline discipline (doc 02): exact, synchronous,
// bottom-to-top. The tile cache, culling refinements, deadlines, and
// progressive refinement land with the interactive renderer.
//
// Per-layer temp targets are acquired from `pool` (doc 09): a caller-owned
// SurfacePool so a looping renderer can reuse temps across frames with no
// per-frame allocator churn. The pool composes over `backend`.
ARBC_API void render_frame(const DocRoot& state, const ContentResolver& resolve,
                           const Viewport& viewport, Backend& backend, SurfacePool& pool,
                           Surface& target);

// Render one layer whose composed local->device transform is `composed` into
// `target`, applying the pull contract's exact cull/compose/region/sub-pixel
// predicates (doc 03/04): degenerate `inverse()` cull, bounds intersect,
// `max_scale()` positive-finite cull, zero-pixel cull, then the single settle
// path. `render_frame` calls it per global layer; the anchored viewport-outward
// walk (`anchored_viewports.hpp`) calls it per surviving leaf -- ONE shared body,
// so the `anchor == root` case stays byte-identical (refinement Decision 4).
// Internal to the compositor; declared here so the anchored walk reuses it
// verbatim rather than duplicating the predicates.
ARBC_API void render_layer(const ContentResolver& resolve, const LayerRecord& layer,
                           const Affine& composed, const Rect& device_rect, Backend& backend,
                           SurfacePool& pool, Surface& target);

} // namespace arbc
