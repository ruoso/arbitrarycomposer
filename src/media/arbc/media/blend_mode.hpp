#pragma once

// The per-layer BLEND MODE (issue #36): the function that decides how a layer's colour
// combines with what is already under it, beside the `opacity` that decides how much of it
// lands. Level-1 vocabulary, beside `SurfaceFormat` and `AudioFormat` and for the same reason:
// the model records it, the compositor threads it, the backend's composite kernel evaluates
// it, and the serializer writes it -- four components, so it belongs to none of them.
//
// WHICH SET, AND WHY NOT THE OTHER ONE. Compositing has two independent axes and this is only
// one of them. The PORTER-DUFF operators (over, in, out, atop, xor, ...) change the COVERAGE
// arithmetic -- which parts of the source and backdrop survive at all -- and the SEPARABLE
// BLEND MODES change the COLOUR function applied where both are present. What a compositing
// editor means by "multiply shadow pass", "screen glow", "add for light" is the second axis;
// only the second is here. The vocabulary is the one PDF 1.4 specified and CSS Compositing and
// Blending Level 1 restates, taken as a reference rather than invented, so a document's
// `multiply` means what every other tool means by it.
//
// The Porter-Duff axis is deliberately NOT added with it. It is a different question (coverage,
// not colour), it interacts with the tiled compositor's apron and paint-once partition in ways
// the colour axis does not, and no host has asked for it -- adding both at once would ship one
// of them untested by any real use. If it lands later it lands as its own field, not as more
// enumerators here, because an operator and a blend mode compose (a `multiply` layer drawn
// `atop`) rather than exclude.
//
// The NON-SEPARABLE modes (hue, saturation, color, luminosity) are also not here yet, and for a
// sharper reason: they are defined in terms of a colour's luminosity, which is a function of
// the RGB primaries, so their result depends on the composition's working space in a way the
// separable modes' does not. They want the same answer the blend-space question wants generally
// (below), and shipping them before it is settled would bake a guess into documents.
//
// WHICH SPACE. The composition's configured `working_space` (doc 07 rule 2) -- the space the
// compositor already blends in, because it is the space the pixels are in when the composite
// kernel runs. This is a real choice with a visible consequence, not a formality: a working
// space that is LINEAR (the default) makes `multiply` a light-transport multiply, while the
// same mode evaluated on sRGB-encoded values -- which is what a canvas or a PDF viewer does --
// darkens differently. arbc blends in the composition's working space, so a document says which
// answer it wants by saying what it works in, and one document is internally consistent.
//
// HOW A KIND OPTS OUT: it does not, because a blend mode is not a property of a kind. It rides
// the LAYER, which is placement -- exactly where `opacity` and its audio twin `gain` already
// live, and the model already has this shape: "a visual-only layer carries the harmless default
// and never reads it" (`records.hpp`, on `gain`). An audio-only layer carries `BlendMode::Normal`
// and the mix engine never looks at it, precisely as a visual-only layer carries `gain` and the
// compositor never looks at that. No facet, no capability virtual, and no error: a question that
// does not apply to a layer is answered by nobody asking it.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace arbc {

// The separable blend modes, in the reference vocabulary's order. `Normal` is 0 and is
// SOURCE-OVER exactly -- the composite every caller has always gotten -- so a default-
// constructed placement, a zeroed record field and a document written before this existed all
// mean the same thing.
enum class BlendMode : std::uint8_t {
  Normal = 0,
  Multiply,
  Screen,
  Overlay,
  Darken,
  Lighten,
  ColorDodge,
  ColorBurn,
  HardLight,
  SoftLight,
  Difference,
  Exclusion,
};

inline constexpr std::size_t k_blend_mode_count = 12;

// `B(Cb, Cs)` -- the mode's colour function on ONE channel of UNPREMULTIPLIED, working-space
// values, which is how the reference defines it. The compositor's kernel un-premultiplies,
// calls this, and re-composites; nothing here knows about alpha.
//
// Every formula is the reference's verbatim. The two DIVIDING modes (`ColorDodge`, `ColorBurn`)
// carry the reference's own limit cases, which are not decoration: without them a fully-lit
// dodge divides by zero and a fully-dark burn divides by zero, and an infinity or a NaN in a
// premultiplied working buffer propagates through every later composite in the frame.
//
// The reference states its formulas over [0, 1], and a linear float working space can hold
// values outside it (an HDR highlight). They are evaluated unclamped rather than pinned to the
// unit interval, because clamping would quietly destroy exactly the data such a space exists to
// carry -- and every mode stays FINITE for every finite input, the two dividing modes because
// their limit cases catch the out-of-range divisor before it is used.
//
// `inline`, not `constexpr`: `SoftLight` needs a square root, and `std::sqrt` is not a constant
// expression before C++26. Nothing here wants compile-time evaluation.
inline float blend_channel(BlendMode mode, float cb, float cs) noexcept {
  switch (mode) {
  case BlendMode::Normal:
    return cs;
  case BlendMode::Multiply:
    return cb * cs;
  case BlendMode::Screen:
    return cb + cs - (cb * cs);
  case BlendMode::Overlay:
    // HardLight with the operands swapped, which is the reference's own definition.
    return blend_channel(BlendMode::HardLight, cs, cb);
  case BlendMode::Darken:
    return cb < cs ? cb : cs;
  case BlendMode::Lighten:
    return cb > cs ? cb : cs;
  case BlendMode::ColorDodge:
    // A black backdrop stays black however bright the source: nothing to brighten.
    if (cb <= 0.0F) {
      return 0.0F;
    }
    // A fully-bright source blows the backdrop out rather than dividing by zero.
    if (cs >= 1.0F) {
      return 1.0F;
    }
    return (cb / (1.0F - cs)) < 1.0F ? (cb / (1.0F - cs)) : 1.0F;
  case BlendMode::ColorBurn:
    // A white backdrop stays white however dark the source: nothing to burn into.
    if (cb >= 1.0F) {
      return 1.0F;
    }
    // A fully-dark source burns to black rather than dividing by zero.
    if (cs <= 0.0F) {
      return 0.0F;
    }
    return 1.0F - (((1.0F - cb) / cs) < 1.0F ? ((1.0F - cb) / cs) : 1.0F);
  case BlendMode::HardLight:
    return cs <= 0.5F ? blend_channel(BlendMode::Multiply, cb, 2.0F * cs)
                      : blend_channel(BlendMode::Screen, cb, (2.0F * cs) - 1.0F);
  case BlendMode::SoftLight: {
    // The reference's piecewise D(Cb); the `cb <= 0.25` arm is the polynomial that keeps the
    // curve smooth where a bare sqrt would kink.
    const float d = cb <= 0.25F ? ((((16.0F * cb) - 12.0F) * cb) + 4.0F) * cb : std::sqrt(cb);
    return cs <= 0.5F ? cb - ((1.0F - (2.0F * cs)) * cb * (1.0F - cb))
                      : cb + (((2.0F * cs) - 1.0F) * (d - cb));
  }
  case BlendMode::Difference:
    return cb > cs ? cb - cs : cs - cb;
  case BlendMode::Exclusion:
    return cb + cs - (2.0F * cb * cs);
  }
  return cs; // an out-of-range value blends as Normal rather than reading past the table
}

// The mode's persistent name -- the reference vocabulary's spelling, which is what the document
// format writes (doc 08). Stable text, never the enumerator's numeric value: the enum's order is
// an implementation detail and a document outlives it.
constexpr std::string_view blend_mode_name(BlendMode mode) noexcept {
  switch (mode) {
  case BlendMode::Normal:
    return "normal";
  case BlendMode::Multiply:
    return "multiply";
  case BlendMode::Screen:
    return "screen";
  case BlendMode::Overlay:
    return "overlay";
  case BlendMode::Darken:
    return "darken";
  case BlendMode::Lighten:
    return "lighten";
  case BlendMode::ColorDodge:
    return "color-dodge";
  case BlendMode::ColorBurn:
    return "color-burn";
  case BlendMode::HardLight:
    return "hard-light";
  case BlendMode::SoftLight:
    return "soft-light";
  case BlendMode::Difference:
    return "difference";
  case BlendMode::Exclusion:
    return "exclusion";
  }
  return "normal";
}

// The inverse: `false` for a name this build does not know, which the reader answers as a
// malformed field rather than by guessing a mode (doc 08 -- a document that names a blend this
// build cannot perform must not silently render as something else).
constexpr bool blend_mode_from_name(std::string_view name, BlendMode& out) noexcept {
  for (std::size_t i = 0; i < k_blend_mode_count; ++i) {
    const auto mode = static_cast<BlendMode>(i);
    if (blend_mode_name(mode) == name) {
      out = mode;
      return true;
    }
  }
  return false;
}

} // namespace arbc
