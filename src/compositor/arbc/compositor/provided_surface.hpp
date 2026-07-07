#pragma once

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
template <class Consume>
void consume_render_result(RenderResult& result, Surface& fallback, Consume&& consume) {
  if (result.provided.has_value()) {
    const Surface& provided = result.provided->surface();
    consume(provided);
    result.provided.reset(); // release after composite/copy, never before
  } else {
    const Surface& src = fallback;
    consume(src);
  }
}

} // namespace arbc
