#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/media/pixel_traits.hpp>
#include <arbc/surface/typed_span.hpp>

#include <cstddef>
#include <utility>

namespace arbc {

SolidContent::SolidContent(Rgba premultiplied_color, std::optional<Rect> bounds)
    : d_color(premultiplied_color), d_bounds(std::move(bounds)) {}

std::optional<Rect> SolidContent::bounds() const { return d_bounds; }

Stability SolidContent::stability() const { return Stability::Static; }

// Time-invariant: a solid fill varies over no local-time range, so it declares
// no temporal extent (doc 11:69-71), exactly as `Static` content reports no
// `achieved_time`.
std::optional<TimeRange> SolidContent::time_extent() const { return std::nullopt; }

// Trivial content settles INLINE (doc 03:14,117-121): it fills the target and
// returns a `RenderResult`, never `nullopt`, so it pays no async ceremony and
// ignores the supplied completion.
std::optional<RenderResult> SolidContent::render(const RenderRequest& request,
                                                 std::shared_ptr<RenderCompletion>) {
  // The compositor only requests regions within declared bounds (doc 03),
  // so the whole target is ours to fill. The color is a premultiplied
  // working-space sample; one variant dispatch per render resolves the target's
  // format tag (doc 07) and encodes it per pixel through the checked typed span,
  // so a solid fills any working format, not just rgba32f.
  const WorkingPixel color{d_color.r, d_color.g, d_color.b, d_color.a};
  visit_surface(request.target, [&](auto typed) {
    using Traits = PixelTraits<decltype(typed)::format>;
    for (std::size_t i = 0; i + Traits::channels <= typed.data.size(); i += Traits::channels) {
      Traits::encode(color, &typed.data[i]);
    }
  });
  return RenderResult{request.scale, true};
}

} // namespace arbc
