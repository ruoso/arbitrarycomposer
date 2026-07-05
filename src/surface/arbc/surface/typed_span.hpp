#pragma once

#include <arbc/media/pixel_format.hpp>
#include <arbc/media/pixel_traits.hpp>
#include <arbc/surface/surface.hpp>

#include <span>
#include <utility>
#include <variant>

namespace arbc {

// A surface's CPU storage as a span carrying its pixel format at compile time
// (doc 07's `TypedSpan<F>` sketch). The kernel a visitor selects is
// monomorphized on `TypedSpan::format`, so the per-pixel loop never branches on
// format -- the one runtime decision is the visit below, once per operation.
template <PixelFormat F> struct TypedSpan {
  static constexpr PixelFormat format = F;
  std::span<typename PixelTraits<F>::Storage> data;
};

// The closed set as a total variant (doc 07's `AnySurfaceRef`). It is total by
// construction: adding a PixelFormat without a TypedSpan alternative here, or a
// case in `typed()` below, is a compile error (-Wswitch on the exhaustive
// switch, and std::visit rejecting an unhandled alternative) -- never a silent
// runtime hole.
using AnySurfaceRef =
    std::variant<TypedSpan<PixelFormat::Rgba32fLinearPremul>,
                 TypedSpan<PixelFormat::Rgba16fLinearPremul>, TypedSpan<PixelFormat::Rgba8Srgb>>;

// Resolve a surface's runtime format tag to its compile-time typed span. The
// switch is exhaustive with no default arm, so a new format forces a case here.
inline AnySurfaceRef typed(Surface& surface) {
  switch (surface.format().pixel_format) {
  case PixelFormat::Rgba32fLinearPremul:
    return TypedSpan<PixelFormat::Rgba32fLinearPremul>{
        surface.span<PixelFormat::Rgba32fLinearPremul>()};
  case PixelFormat::Rgba16fLinearPremul:
    return TypedSpan<PixelFormat::Rgba16fLinearPremul>{
        surface.span<PixelFormat::Rgba16fLinearPremul>()};
  case PixelFormat::Rgba8Srgb:
    return TypedSpan<PixelFormat::Rgba8Srgb>{surface.span<PixelFormat::Rgba8Srgb>()};
  }
  // Unreachable: the switch covers the closed set. Present only so the function
  // has a return on every control path under -Werror.
  return TypedSpan<PixelFormat::Rgba32fLinearPremul>{
      surface.span<PixelFormat::Rgba32fLinearPremul>()};
}

// One dispatch per operation (doc 07): visit the surface as its typed span. The
// visitor is a generic lambda `[](auto typed) { ... }` whose body is
// instantiated per format; `decltype(typed)::format` recovers the compile-time
// format inside.
template <class Visitor> decltype(auto) visit_surface(Surface& surface, Visitor&& visitor) {
  return std::visit(std::forward<Visitor>(visitor), typed(surface));
}

} // namespace arbc
