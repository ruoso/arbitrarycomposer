#include <arbc/media/surface_format.hpp>
#include <arbc/runtime/offline.hpp>

#include <cassert>
#include <utility>

namespace arbc {

std::unique_ptr<Surface> render_offline(const Document& document, const Viewport& viewport,
                                        Backend& backend) {
  const DocStatePtr state = document.pin();
  // The frame target carries the doc 07 default working tags (premultiplied
  // linear-light rgba32f -- the reference backend's stored format today), which
  // every backend must store, so creation succeeds; assert the value per the
  // former unconditional-deref posture (make_surface is now errors-as-values).
  expected<std::unique_ptr<Surface>, SurfaceError> target =
      backend.make_surface(viewport.width, viewport.height, k_working_rgba32f);
  assert(target.has_value());
  render_frame(
      *state, [&document](ObjectId id) { return document.resolve(id); }, viewport, backend,
      **target);
  return std::move(*target);
}

} // namespace arbc
