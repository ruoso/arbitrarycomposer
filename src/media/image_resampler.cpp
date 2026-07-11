#include <arbc/media/image_resampler.hpp>

#include <array>

namespace arbc {

namespace {

// ===========================================================================
// FROZEN LANCZOS-3 HALF-BAND TABLE -- regenerate deliberately (doc 16:50-53).
// ===========================================================================
//
// TAPS=6  WINDOW=Lanczos (a=3)  PHASE=0.5 (fixed)
//
// A 2:1 rung maps the destination center to source `u = 2x + 0.5`, so the
// fractional phase is CONSTANT for every output pixel and the decimator collapses
// to a single fixed 6-tap FIR -- taps at offsets -2.5, -1.5, -0.5, +0.5, +1.5,
// +2.5, i.e. child pixels `2x-2 .. 2x+3`. Six frozen float32 constants: no
// polyphase bank, no phase math, no runtime `libm`, byte-exact (a Lanczos kernel
// evaluated with runtime `std::sin` would land in doc 16's platform-libm carve-out
// and force a tolerance; tabulation is what keeps this exact).
//
// The values are `sinc(t) * sinc(t/3)` at those six offsets, normalized by their
// double-precision sum, then rounded once to float32. The table is exactly
// symmetric (`w[i] == w[5-i]`), which the kernel's symmetric-pair-folded tap order
// relies on for its exactly-1.0F float32 DC gain (see `decimate_line`).
//
// Window/tap/phase choices are fixed (Constraint 7): no runtime quality knob.
//
// REGENERATE PROCEDURE (doc 16 tier-3): an intended filter change (different
// window or tap count) deliberately re-freezes this table; it never regenerates
// silently. Build the media unit test and run only the hidden dump case, which
// recomputes the coefficients with libm and prints this paste-ready block:
//
//     cmake --build --preset dev --target arbc_media_t
//     ./build/dev/src/media/arbc_media_t "[.regen]"
//
// then replace the block below with its output AND re-freeze the dependent output
// goldens (src/media/t/image_resampler.t.cpp and
// src/kind_raster/t/raster_goldens.t.cpp).
constexpr std::array<float, k_decimate_taps> k_lanczos3_half_band{{
    0x1.90b216p-6f,   // child 2x-2  (offset -2.5)  = +0.024456521
    -0x1.1642c8p-3f,  // child 2x-1  (offset -1.5)  = -0.135869563
    0x1.390b22p-1f,   // child 2x+0  (offset -0.5)  = +0.611413062
    0x1.390b22p-1f,   // child 2x+1  (offset +0.5)  = +0.611413062
    -0x1.1642c8p-3f,  // child 2x+2  (offset +1.5)  = -0.135869563
    0x1.90b216p-6f,   // child 2x+3  (offset +2.5)  = +0.024456521
}};
// ===========================================================================
// END FROZEN LANCZOS-3 HALF-BAND TABLE
// ===========================================================================

} // namespace

const std::array<float, k_decimate_taps>& lanczos3_half_band() { return k_lanczos3_half_band; }

float lanczos3_half_band_dc_gain() {
  // Reduced through the kernel itself, so this is literally the gain the pyramid
  // sees -- not an independently-summed approximation of it.
  const WorkingPixel unit{1.0F, 1.0F, 1.0F, 1.0F};
  const std::array<WorkingPixel, k_decimate_taps> flat{{unit, unit, unit, unit, unit, unit}};
  return decimate_line(flat)[0];
}

} // namespace arbc
