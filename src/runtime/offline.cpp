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
  // Source the document's root composition (lowest-id wins, the same rule the
  // serializer and working_space()/working_audio_format() use) and anchor the
  // frame walk to it, so the compositor draws exactly the root composition's
  // members and not the document-global leaf set
  // (compositor.root_composition_frame_walk, doc 05:28-36). The compositor never
  // re-derives the root -- the driver hands it the id (Decision 2). A caller that
  // already pinned a specific composition keeps it.
  Viewport anchored = viewport;
  if (!anchored.anchor.valid()) {
    ObjectId root_id{};
    const CompositionRecord* root_rec = nullptr;
    if (state->find_first_composition(root_id, root_rec)) {
      anchored.anchor = root_id;
    }
  }
  render_frame(
      *state, [&document](ObjectId id) { return document.resolve(id); }, anchored, backend, pool,
      **target);
  return std::move(*target);
}

} // namespace arbc
