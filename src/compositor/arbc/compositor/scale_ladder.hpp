#pragma once

#include <arbc/cache/key_shapes.hpp>
#include <arbc/surface/backend.hpp>
#include <arbc/surface/surface.hpp>

#include <cassert>
#include <cmath>
#include <cstdint>

// The scale-ladder rung algebra (doc 04:88-106) for the interactive path.
// Doc 04:90-91: "Interactive rendering quantizes requested scale to a ladder of
// powers of two (..., 1/2, 1, 2, 4, ...) in the layer's local space" -- rung 0
// is the native scale 1.0. Doc 04:95-98: the sub-octave remainder is applied as
// resampling during compositing, and by convention the ladder is chosen so
// tiles are *downsampled* (rung >= needed scale) since minification beats
// magnification. This header turns that convention into concrete, testable
// arithmetic plus the one pixel-producing operation the ladder owns -- building
// a coarser rung by exact 2:1 box reduction.
//
// The rung index *is* the cache-key discriminator: this code produces the
// `cache::ScaleRung` (`key_shapes.hpp:38-43`, "the compositor owns the ladder
// and hands the rung down") that `compositor.tile_planning` keys tiles by. It
// invents no parallel compositor-local rung type. The compositor is L4 and may
// depend on `cache`; the box reducer is reached only through the abstract L2
// `surface::Backend` seam, never a direct `backend-cpu` edge (doc 17:56,:75-77).
//
// The offline `render_frame` stays exact-scale by design (doc 02:74-85, "no
// quantization"); this ladder is interactive-only machinery consumed by
// `tile_planning` + the runtime interactive loop -- it does not rewire the
// offline frame.

namespace arbc {

// The result of quantizing a requested scale to the ladder: the rung to render
// at, and the sub-octave residual `remainder = scale / rung_scale(rung) in
// (0.5, 1.0]` the composite pass resamples (bilinear tap, `backend.hpp:39-40`),
// exactly `1.0` at power-of-two scales.
struct RungSelection {
  ScaleRung rung;
  double remainder{1.0};
};

// Quantize an interactive request scale -- the larger singular value of the
// composed mapping (`Affine::max_scale()`, doc 04:104) -- to the *smallest*
// power-of-two rung `2^k >= scale` (prefer-downsample, doc 04:96-98), leaving
// `remainder = scale / 2^k in (0.5, 1.0]` (<=1 octave, doc 04:95), bit-exactly
// `1.0` at power-of-two scales.
//
// Precondition: `scale > 0 && std::isfinite(scale)`. The caller culls a
// degenerate/non-finite composed scale upstream (`compositor.cpp:44`, doc
// 04:115-117); `select_rung` asserts the precondition (debug) rather than
// inventing a NaN/zero policy the ladder does not own.
//
// Implemented via `std::frexp` -- *not* `std::log2 + std::ceil`, which rounds
// unpredictably at powers of two (doc 16:47-53 wants bit-exact boundaries).
// `frexp(scale, &e)` decomposes `scale = m * 2^e` exactly with `m in [0.5, 1)`;
// `m == 0.5` means `scale` is the exact power `2^(e-1)` (rung index `e-1`),
// otherwise the smallest enclosing power is `2^e` (rung index `e`). Then
// `rung_scale`/`remainder` via `std::ldexp` keep the round-trip exact, so a
// power-of-two scale yields `remainder == 1.0` bit-exactly (division of equal
// exact doubles) and the composite tap collapses to the byte-exact nearest tap.
inline RungSelection select_rung(double scale) {
  assert(scale > 0.0 && std::isfinite(scale));

  int exponent = 0;
  const double mantissa = std::frexp(scale, &exponent); // scale = mantissa * 2^exponent
  const std::int32_t index =
      (mantissa == 0.5) ? (exponent - 1) : exponent; // exact power -> exponent-1
  const double rung = std::ldexp(1.0, index);
  const double remainder = scale / rung;

  // <=1-octave downsample invariant (doc 04:95): the rung is the smallest
  // 2^k >= scale, so the residual is always a downsample in (0.5, 1.0].
  assert(remainder > 0.5 && remainder <= 1.0);
  return RungSelection{ScaleRung{index}, remainder};
}

// The device-pixels-per-local-unit a rung renders at: `2^rung.index` (rung 0 =
// native scale 1.0; doc 04:90-91). No `2^16` clamp -- the raw `int32` index
// spans far beyond any representable zoom; extreme-scale precision is handled by
// viewport rebasing (`compositor.anchored_viewports`, doc 04:63-66), a separate
// mechanism the ladder does not conflate with per-octave tile geometry.
inline double rung_scale(ScaleRung rung) { return std::ldexp(1.0, rung.index); }

// Build the next-coarser rung (`index - 1`) from a finer-rung surface by exact
// 2:1 box reduction, returning the coarser rung. A thin wrapper over
// `Backend::downsample` (`backend.hpp:42-48`): the box mean is taken in decoded
// premultiplied linear working space by the delegated kernel (doc 07 rule 3) --
// `reduce_rung` is pixel-loop-free and re-implements no working-space math.
//
// Geometry contract (documented + debug-checked here, matching the backend's
// own asserts): `dst` dims must equal `src` dims / 2 with *even* source dims and
// identical format. That even-dims guarantee is a property of power-of-two tile
// geometry owned by `tile_planning`; this wrapper states the dependency rather
// than allocating or resizing. The box reducer was built *for* the scale ladder
// (`backend.hpp:46-47`); its production call site (populating coarser cache
// rungs on a miss / for the degradation fallback, doc 02:64-65) is
// `tile_planning`'s.
inline ScaleRung reduce_rung(Backend& backend, Surface& dst, const Surface& src,
                             ScaleRung src_rung) {
  assert(dst.format() == src.format());
  assert(src.width() % 2 == 0 && src.height() % 2 == 0);
  assert(dst.width() == src.width() / 2 && dst.height() == src.height() / 2);

  backend.downsample(dst, src);
  return ScaleRung{src_rung.index - 1};
}

} // namespace arbc
