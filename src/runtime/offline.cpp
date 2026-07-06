#include <arbc/runtime/offline.hpp>

#include <utility>

namespace arbc {

expected<std::unique_ptr<Surface>, SurfaceError>
render_offline(const Document& document, const Viewport& viewport, Backend& backend) {
  const DocStatePtr state = document.pin();
  // The frame target carries the composition's configured working space (doc 07
  // rule 2), read from the pinned version -- no longer a hardcode. A backend that
  // cannot store that format returns a SurfaceError (capability honesty, doc 07):
  // surface it as a value here rather than aborting, so a document configured for
  // an unstorable working space yields the error path, not a crash.
  expected<std::unique_ptr<Surface>, SurfaceError> target =
      backend.make_surface(viewport.width, viewport.height, state->working_space());
  if (!target.has_value()) {
    return unexpected(target.error());
  }
  // A local pool for this single frame: the offline driver renders one frame,
  // so it gets within-frame temp reuse now; a looping renderer would hold a
  // long-lived pool and reuse across frames (doc 09 / doc 02 still-scene). Temps
  // are acquired at the target's format, so they carry the working-space tags too.
  SurfacePool pool(backend);
  render_frame(
      *state, [&document](ObjectId id) { return document.resolve(id); }, viewport, backend, pool,
      **target);
  return std::move(*target);
}

} // namespace arbc
