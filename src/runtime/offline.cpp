#include <arbc/compositor/pull_service.hpp>  // PullServiceImpl, PullConfig, direct_dispatch
#include <arbc/compositor/tile_planning.hpp> // render_frame_interactive
#include <arbc/runtime/offline.hpp>
#include <arbc/runtime/operator_binding.hpp> // bind_operators, register_builtin_operator_binders
#include <arbc/runtime/pull_identity.hpp>    // build_pull_identity_map, pull_identity_of

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
  const ContentResolver resolve = [&document](ObjectId id) { return document.resolve(id); };
  // One-shot renders still need a live pull service: nested/operator contents pull
  // their inputs through the service injected at attach time.
  TileCache cache(std::size_t{64} * 1024 * 1024);
  const auto identity_map = build_pull_identity_map(*state, resolve);
  const auto stamp_map = build_pull_stamp_map(*state, *identity_map);
  auto contribution = pull_contribution_of(identity_map, stamp_map);
  PullConfig pull_config;
  pull_config.id_of = pull_identity_of(identity_map);
  pull_config.contribution = contribution;
  PullServiceImpl pull(cache, backend, direct_dispatch(), std::move(pull_config));
  register_builtin_operator_binders();
  const OperatorBindingScope binding = bind_operators(document, pull, backend, state);
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
  // ONE render path (`compositor.render_path_unification`): the same tiled driver the
  // interactive loop and the sequence exporter run, differing only in EXACTNESS --
  // `Exact` + `Deadline::none()`, so every miss is rendered to completion before the
  // composite and no tile is ever served degraded. This is field-for-field the inline
  // branch of `SequenceRenderer::render_frame_at` (`offline_sequence.cpp:142-164`) for
  // a single frame; the untiled `render_frame` it replaces was a walking-skeleton relic
  // that had fallen behind on scale quantization, composition time, content state and
  // async content (see the design note in doc 02).
  //
  // `prior_revision == nullopt` disables the stale probe, so a miss is never served a
  // prior-revision tile; `pending == nullptr` means an async content is driven to
  // completion inline rather than deferred.
  render_frame_interactive(*state, resolve, anchored, cache, backend, pool, **target,
                           Deadline::none(), /*prior_revision=*/std::nullopt,
                           /*pending=*/nullptr, /*counters=*/nullptr, /*dirty=*/nullptr,
                           Time::zero(), /*visible_plans=*/nullptr, /*diagnostics=*/nullptr, &pull,
                           Exactness::Exact, /*wanted=*/nullptr, &contribution);
  return std::move(*target);
}

} // namespace arbc
