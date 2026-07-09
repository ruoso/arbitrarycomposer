#include <arbc/media/pixel_traits.hpp>
#include <arbc/serialize/placeholder_content.hpp>
#include <arbc/surface/typed_span.hpp>

#include <cstddef>
#include <utility>

namespace arbc {

PlaceholderContent::PlaceholderContent(nlohmann::json body, bool kind_registered,
                                       std::vector<ContentRef> inputs)
    : d_body(std::move(body)), d_kind_registered(kind_registered), d_inputs(std::move(inputs)) {}

bool PlaceholderContent::has_passthrough_input() const {
  return !d_inputs.empty() && d_inputs[0] != nullptr;
}

// Pass-through the input's footprint when one is bound (the compositor serves input
// 0 under identity()); an input-less placeholder is a bounded diagnostic fill of
// whatever region the compositor requests, so it declares no intrinsic bounds --
// exactly as an unbounded solid does, never an infinite hole.
std::optional<Rect> PlaceholderContent::bounds() const {
  if (has_passthrough_input()) {
    return d_inputs[0]->bounds();
  }
  return std::nullopt;
}

// Static: the diagnostic fill is time-invariant. When an input is bound, identity()
// short-circuits to input 0, so the input's own stability governs the cache.
Stability PlaceholderContent::stability() const { return Stability::Static; }

std::optional<TimeRange> PlaceholderContent::time_extent() const { return std::nullopt; }

std::span<const ContentRef> PlaceholderContent::inputs() const { return d_inputs; }

std::optional<std::size_t> PlaceholderContent::identity(const RenderRequest& /*request*/) const {
  if (has_passthrough_input()) {
    return std::optional<std::size_t>{0};
  }
  return std::nullopt;
}

// A no-input placeholder settles INLINE (doc 03:14,117-121): it fills the requested
// target region with the diagnostic color and returns a `RenderResult`, never a
// fault, never `nullopt`. When an input is bound the compositor serves input 0 under
// identity() and never calls render() for pixels; if it is called anyway, the same
// harmless diagnostic fill keeps the contract total (never a crash, doc 08
// Principle 6). One variant dispatch per render resolves the target's working format
// (doc 07) and encodes the premultiplied diagnostic sample per pixel.
std::optional<RenderResult> PlaceholderContent::render(const RenderRequest& request,
                                                       std::shared_ptr<RenderCompletion>) {
  const WorkingPixel color{k_placeholder_diagnostic[0], k_placeholder_diagnostic[1],
                           k_placeholder_diagnostic[2], k_placeholder_diagnostic[3]};
  visit_surface(request.target, [&](auto typed) {
    using Traits = PixelTraits<decltype(typed)::format>;
    for (std::size_t i = 0; i + Traits::channels <= typed.data.size(); i += Traits::channels) {
      Traits::encode(color, &typed.data[i]);
    }
  });
  return RenderResult{request.scale, true};
}

} // namespace arbc
