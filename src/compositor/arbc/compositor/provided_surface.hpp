#pragma once

#include <arbc/base/transform.hpp>
#include <arbc/contract/content.hpp> // RenderResult, SurfaceRef
#include <arbc/surface/surface.hpp>

#include <utility>

namespace arbc {

// Honor a settled `RenderResult`'s optional content-provided surface at a
// `RenderResult`-consumption site (doc 09 §Content-provided surfaces). One
// helper, invoked at every consumption site (the inline composite in
// `compositor.cpp`, and the copy-to-cache in `tile_planning.cpp` /
// `pull_service.cpp` / `refinement.cpp`), so the branch and the release live in
// one testable place (doc 03:126-130's "one render entry point").
//
//   - `fallback` is the surface the content was asked to fill: the pooled temp
//     on the inline path, the cache-destined tile surface on the cache path.
//   - `consume(src)` is invoked EXACTLY ONCE with the surface the pixels
//     actually live in -- the content's `provided` surface when it supplied one
//     (the caller composites directly from it inline -- zero copy -- or copies
//     it into `fallback` on the cache path), otherwise `fallback` itself (the
//     content filled the target the ordinary way). The caller distinguishes the
//     two by identity (`&src == &fallback`): a cache-path caller copies only
//     when they differ, since a filled `fallback` is already the cached pixels.
//
// After `consume` returns, the provided `SurfaceRef` is released -- our
// reference dropped, firing the content's release callback when it was the last
// holder (doc 09:106-108) -- and NEVER before the composite/copy. The `provided`
// surface is never stored in `TileValue` or any structure that outlives the
// frame (v1 always copies into cache, never adopts, doc 09:109-112). The
// `fallback` target is left untouched whenever a provided surface was consumed
// (doc 09:80,97-98). `result` is taken by mutable reference so the release
// (`provided.reset()`) is visible; its other fields (`achieved_scale`, `exact`,
// `achieved_time`) are untouched and remain valid for the caller's cache insert.
// Where a consumed provided surface must be placed relative to the surface it is
// copied/composited into: the affine from the provided surface's pixel space to
// the target's. `RenderResult::provided_origin` names the local-space point the
// provided surface's pixel (0, 0) covers; absent it is the request region's own
// origin and this is the identity, which is the placement every landed caller
// used before the origin became statable. `region` and `scale` are the request's
// (the target covers `region` at `scale`), so the offset is the two origins'
// separation taken into target pixels.
//
// Takes the ORIGIN rather than the whole `RenderResult` deliberately. Passing the
// result would alias its `std::optional<SurfaceRef>` into this function, and gcc-13
// at `-O2` then loses track of that optional's shared-count across the consume below
// and reports a `-Wmaybe-uninitialized` on the result's own destructor. The helper
// needs two scalars; taking two scalars is both the smaller interface and the one
// that does not hand an optimizer a reason to guess.
inline Affine provided_placement(const std::optional<SurfaceRef>& provided_surface,
                                 const Rect& region, double scale) {
  if (!provided_surface.has_value() || !provided_surface->origin().has_value()) {
    return Affine::identity();
  }
  const Vec2& origin = *provided_surface->origin();
  return Affine::translation((origin.x - region.x0) * scale, (origin.y - region.y0) * scale);
}

//
// Takes the `provided` OPTIONAL rather than the whole `RenderResult`, for the same
// reason `provided_placement` takes the origin: it touches nothing else, and handing
// the result in aliases its refcounted optional into a function whose reset gcc-13 at
// `-O2` cannot then reconcile with the result's own destructor (a
// `-Wmaybe-uninitialized` on the shared count). The smaller interface is also the
// truer one -- the helper's whole job is this one member.
template <class Consume>
void consume_render_result(std::optional<SurfaceRef>& provided_surface, Surface& fallback,
                           Consume&& consume) {
  if (provided_surface.has_value()) {
    const Surface& provided = provided_surface->surface();
    consume(provided);
    provided_surface.reset(); // release after composite/copy, never before
  } else {
    const Surface& src = fallback;
    consume(src);
  }
}

} // namespace arbc
