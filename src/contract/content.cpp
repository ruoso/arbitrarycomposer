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

// Default audio pull (doc 12 Decision 5): a `PullService` that predates audio
// genuinely has no audio pull, so it settles `done` -- once, never leaving the
// caller hung -- as "no audio available". The real override is `compositor`'s
// `PullServiceImpl::pull_audio` (doc 17:56), the sole concrete `PullService`;
// the L4 mix engine (`arbc::audio-engine`) drives that concrete service through
// this seam rather than subclassing it (an L4-peer edge doc 17:41 forbids).
//
// `done` stays BY VALUE: the by-value handle is the seam's ownership transfer
// (`content.hpp:745`) -- an implementation may keep the completion live past the
// call and settle it off-thread, which is the whole async path. This body only
// happens to settle inline; narrowing the interface to a reference for it would
// re-shape the declaration every implementation inherits.
void PullService::pull_audio(ContentRef /*input*/, const AudioRequest& /*request*/,
                             // NOLINTNEXTLINE(performance-unnecessary-value-param): seam contract
                             std::shared_ptr<AudioCompletion> done) {
  done->fail(RenderError::ResourceUnavailable);
}

} // namespace arbc
