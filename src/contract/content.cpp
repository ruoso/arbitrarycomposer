#include <arbc/contract/content.hpp>

namespace arbc {

Content::~Content() = default;

// Default: identity. A pass-through-shaped content's output damage is its
// input's damage unchanged; operators that inflate (a blur by its radius, a
// warp through its distortion) override this. See the covering requirement in
// the header (doc 13:104-107).
Rect Content::map_input_damage(std::size_t /*input*/, const Rect& rect) const { return rect; }

// Default: never a pass-through. Operators that can serve an input's output
// verbatim for some request (a fade at envelope == 1) override this.
std::optional<std::size_t> Content::identity(const RenderRequest& /*request*/) const {
  return std::nullopt;
}

// Default audio pull (doc 12 Decision 5): a `PullService` that predates
// `arbc::audio-engine` genuinely has no audio pull, so it settles `done` --
// once, never leaving the caller hung -- as "no audio available". The real
// override lands with the L4 mix engine.
void PullService::pull_audio(ContentRef /*input*/, const AudioRequest& /*request*/,
                             std::shared_ptr<AudioCompletion> done) {
  done->fail(RenderError::ResourceUnavailable);
}

} // namespace arbc
