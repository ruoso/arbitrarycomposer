#include <arbc/runtime/offline.hpp>

namespace arbc {

std::unique_ptr<Surface> render_offline(const Document& document, const Viewport& viewport,
                                        Backend& backend) {
  const DocStatePtr state = document.pin();
  std::unique_ptr<Surface> target =
      backend.make_surface(viewport.width, viewport.height, PixelFormat::Rgba32fLinearPremul);
  render_frame(
      *state, [&document](ObjectId id) { return document.resolve(id); }, viewport, backend,
      *target);
  return target;
}

} // namespace arbc
