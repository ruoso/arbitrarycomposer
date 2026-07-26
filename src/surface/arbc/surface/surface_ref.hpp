#pragma once

#include <arbc/base/geometry.hpp>
#include <arbc/surface/surface.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <utility>

namespace arbc {

// A copyable, refcounted handle to a content-provided Surface (doc 09
// §Content-provided surfaces). When content answers a render request by
// returning its OWN surface -- a 3D engine's framebuffer, a video decoder's
// output -- instead of filling the compositor's target, it hands back a
// `SurfaceRef`; the compositor composites/caches from it and releases it the
// instant it is done (doc 09:87-100,106-112).
//
// Modelled as a `std::shared_ptr<Surface>` plus a `transient` bit:
//   - the shared_ptr control block IS the thread-safe atomic refcount the
//     doc-09 Threading note requires (release callbacks "may arrive from
//     content threads", 09:131-134) -- no hand-rolled atomics to get wrong;
//   - the shared_ptr's deleter IS the content-supplied release callback: the
//     handle does NOT own or free the Surface (the content does), so when the
//     last SurfaceRef drops, the deleter fires the callback handing the surface
//     back to the content rather than deleting it (doc 09:106-108);
//   - `transient` marks a surface the content reuses every frame (a live
//     engine's framebuffer): the compositor must consume it within the frame
//     and copy -- never adopt -- it into cache (doc 09:109-112);
//   - `origin` says WHERE the surface's pixel (0, 0) sits in content-local
//     space (issue #19). Absent means the request region's own origin, i.e.
//     the surface answers the request in place -- which is what the compositor
//     assumed unconditionally before, and which held only for a request that
//     happened to start where the content does. It lives on the HANDLE for the
//     same reason `transient` does: it is a property of the SURFACE, not of the
//     result that carries it.
class SurfaceRef {
public:
  // Adopt `surface` (owned by the content, NOT by this handle) with `release`
  // fired when the last reference drops. `transient` marks consume-within-frame
  // content (doc 09:109-112). A null `release` is permitted (a surface with no
  // teardown obligation): the deleter simply does nothing.
  SurfaceRef(Surface& surface, std::function<void()> release, bool transient = false,
             std::optional<Vec2> origin = std::nullopt)
      : d_surface(&surface,
                  [cb = std::move(release)](Surface*) {
                    if (cb) {
                      cb();
                    }
                  }),
        d_transient(transient), d_origin(origin) {}

  // The wrapped surface, reachable while any reference is live. Non-const even
  // through a const handle: the shared pointee is not const (the compositor may
  // need a mutable Surface& for a backend op), mirroring shared_ptr semantics.
  Surface& surface() const noexcept { return *d_surface; }

  // Whether the content marked this surface transient (doc 09:109-112).
  bool transient() const noexcept { return d_transient; }

  // Where this surface's pixel (0, 0) sits in CONTENT-LOCAL space, or `nullopt`
  // for "the request region's origin" -- the surface answers the request in
  // place (issue #19).
  const std::optional<Vec2>& origin() const noexcept { return d_origin; }

private:
  std::shared_ptr<Surface> d_surface;
  bool d_transient{false};
  std::optional<Vec2> d_origin{};
};

} // namespace arbc
