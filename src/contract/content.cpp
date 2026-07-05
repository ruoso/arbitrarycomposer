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

} // namespace arbc
