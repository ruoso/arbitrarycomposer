#include <arbc/kind_solid/solid_content.hpp>

#include <cstddef>
#include <span>
#include <utility>

namespace arbc {

SolidContent::SolidContent(Rgba premultiplied_color, std::optional<Rect> bounds)
    : d_color(premultiplied_color), d_bounds(std::move(bounds)) {}

std::optional<Rect> SolidContent::bounds() const { return d_bounds; }

Stability SolidContent::stability() const { return Stability::Static; }

// Trivial content settles INLINE (doc 03:14,117-121): it fills the target and
// returns a `RenderResult`, never `nullopt`, so it pays no async ceremony and
// ignores the supplied completion.
std::optional<RenderResult> SolidContent::render(const RenderRequest& request,
                                                 std::shared_ptr<RenderCompletion>) {
  // The compositor only requests regions within declared bounds (doc 03),
  // so the whole target is ours to fill.
  std::span<float> pixels = request.target.cpu_pixels();
  for (std::size_t i = 0; i + 3 < pixels.size(); i += 4) {
    pixels[i + 0] = d_color.r;
    pixels[i + 1] = d_color.g;
    pixels[i + 2] = d_color.b;
    pixels[i + 3] = d_color.a;
  }
  return RenderResult{request.scale, true};
}

} // namespace arbc
