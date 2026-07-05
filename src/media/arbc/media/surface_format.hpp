#pragma once

#include <arbc/media/color_space.hpp>
#include <arbc/media/pixel_format.hpp>

namespace arbc {

// Whether color channels are stored premultiplied by alpha (doc 07). A tag,
// not a convention: compositing happens on premultiplied alpha in the
// working space, and making it explicit lets that invariant be asserted at
// the composite boundary instead of living as an implicit assumption.
enum class Premultiplied {
  No,
  Yes,
};

// The full surface tag triple (doc 09): memory layout + interpretation +
// alpha convention, carried as one value so signatures don't accrete
// parameters. Every surface carries a SurfaceFormat from creation (doc 07
// rule 1 -- tags are the part that is prohibitively expensive to retrofit).
//
// `pixel_format` names its storage transfer as part of format identity (doc
// 07); `color_space` (primaries + transfer) and `premultiplied` stay
// orthogonal tags. Equality is member-wise so downstream code that keys off
// the tags -- convert, cache, import -- can compare a whole format at once.
struct SurfaceFormat {
  PixelFormat pixel_format = PixelFormat::Rgba32fLinearPremul;
  ColorSpace color_space = k_linear_srgb;
  Premultiplied premultiplied = Premultiplied::Yes;

  friend constexpr bool operator==(const SurfaceFormat&, const SurfaceFormat&) = default;
};

// The doc 07 default working format at float32 -- premultiplied linear-light
// sRGB, the walking-skeleton storage.
inline constexpr SurfaceFormat k_working_rgba32f{PixelFormat::Rgba32fLinearPremul, k_linear_srgb,
                                                 Premultiplied::Yes};

// The doc 07 *designed* default working format: premultiplied linear-light
// sRGB at f16. Storable with color.kernels; the working-space default flips
// onto it with color.working_space.
inline constexpr SurfaceFormat k_working_rgba16f{PixelFormat::Rgba16fLinearPremul, k_linear_srgb,
                                                 Premultiplied::Yes};

// The doc 07 rule-3 fast mode: 8-bit sRGB, *straight* alpha. Premultiplying
// gamma-encoded 8-bit samples is doubly wrong (gamma-space multiply + precision
// loss), so the storable pairing is non-premultiplied; the codec crosses into
// the premultiplied working space at decode time.
inline constexpr SurfaceFormat k_fast_rgba8srgb{PixelFormat::Rgba8Srgb, k_srgb, Premultiplied::No};

} // namespace arbc
