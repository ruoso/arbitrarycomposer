#pragma once

#include <arbc/base/ids.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/model/model.hpp>
#include <arbc/surface/backend.hpp>
#include <arbc/surface/surface_pool.hpp>

#include <functional>

namespace arbc {

// The consumer side (doc 01): a device-pixel rectangle plus a camera
// mapping composition space to device pixels. Anchoring and rebasing
// (doc 04) land with deep-zoom support.
struct Viewport {
  int width{0};
  int height{0};
  Affine camera;
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
void render_frame(const DocRoot& state, const ContentResolver& resolve, const Viewport& viewport,
                  Backend& backend, SurfacePool& pool, Surface& target);

} // namespace arbc
