#include <arbc/media/surface_format.hpp>
#include <arbc/runtime/offline.hpp>

namespace arbc {

std::unique_ptr<Surface> render_offline(const Document& document, const Viewport& viewport,
                                        Backend& backend) {
  const DocStatePtr state = document.pin();
  // The frame target carries the doc 07 default working tags (premultiplied
  // linear-light rgba32f -- the reference backend's stored format today).
  std::unique_ptr<Surface> target =
      backend.make_surface(viewport.width, viewport.height, k_working_rgba32f);
  render_frame(
      *state, [&document](ObjectId id) { return document.resolve(id); }, viewport, backend,
      *target);
  return target;
}

} // namespace arbc
