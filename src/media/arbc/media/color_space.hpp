#pragma once

namespace arbc {

// Color-space descriptors (doc 07): the *interpretation* of a surface's
// samples, kept deliberately orthogonal to the pixel format's memory layout.
// Closed, core-owned enums; wide-gamut / HDR members (Display-P3, PQ, ...)
// are later enum additions, not a redesign.

// Chromaticities of the RGB primaries.
enum class Primaries {
  Srgb,
};

// Opto-electronic transfer function relating stored samples to light.
enum class TransferFunction {
  Linear, // linear-light (the correct space to blend and resample in)
  Srgb,   // the sRGB nonlinear encoding (the 8-bit fast-mode interpretation)
};

// A color space is primaries + transfer function (doc 07). A constexpr value
// type carried as a surface tag; equality is member-wise.
struct ColorSpace {
  Primaries primaries = Primaries::Srgb;
  TransferFunction transfer = TransferFunction::Linear;

  friend constexpr bool operator==(const ColorSpace&, const ColorSpace&) = default;
};

// The doc 07 default working color space: linear-light sRGB primaries.
inline constexpr ColorSpace k_linear_srgb{Primaries::Srgb, TransferFunction::Linear};

// Nonlinear sRGB: the interpretation of the 8-bit fast mode (doc 07 rule 3).
inline constexpr ColorSpace k_srgb{Primaries::Srgb, TransferFunction::Srgb};

} // namespace arbc
