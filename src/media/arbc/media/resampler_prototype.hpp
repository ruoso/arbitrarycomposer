#pragma once

// INTERNAL header (deliberately absent from the component's PUBLIC_HEADERS, like
// serialize's `codec.hpp`): the ONE windowed-sinc polyphase prototype behind BOTH
// audio banks. Not part of libarbc's public surface -- `arbc::media` sources and the
// component's own tests include it; nothing else may.
//
// Why it exists: `audio_resampler.cpp` ships a FROZEN 32-phase upsampling table and
// GENERATES the ratio-scaled widened decimation bank at configure time. Those are two
// banks off one prototype ("one generator, not a second algorithm"), and that claim is
// only true if there is literally one generator. Before this header the frozen table
// had no live regeneration path at all: its REGENERATE PROCEDURE pointed at a `[.regen]`
// case that dumped only the OUTPUT golden, so the table was un-regenerable and the
// prototype's second copy lived in a comment. Re-deriving it by hand from the image
// resampler's dumper reproduces 497 of 512 coefficients and silently corrupts 15 -- the
// frozen table forces an integer sinc argument to exact zero (below) and a naive mirror
// does not.
//
// Generation runs OFF the RT thread only (construction / the whole-stream oracle / the
// regen dumper); it is the sole place libm's `std::sin`/`std::cos` are evaluated. The RT
// inner loop MACs over the resident float32 table, no-libm (Constraint 1, doc 16:48-53).

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace arbc {

struct PolyphaseBank {
  std::vector<float> coeffs; // phase-major, `taps` per phase
  std::int64_t taps{0};
};

namespace resampler_prototype {

// Normalized sinc. `force_integer_zero` snaps an exact-integer argument to exact zero
// rather than letting `std::sin(pi*t)`'s residual leak in: that is what makes phase 0 of
// the fixed-cutoff bank a true Kronecker delta, so an aligned integer-ratio sample
// reproduces its native input byte-for-byte instead of being re-quantized by a
// near-but-not-quite-zero tap. The generated decimation bank must NOT do this -- an
// aligned sample there has to be genuinely lowpassed or the anti-aliasing is defeated.
//
// The test is exact, not epsilon'd: `t` is `(k - half) - p/phases` with a power-of-two
// `phases`, so `p/phases` is a dyadic rational and `t` is integral exactly when p == 0.
inline double sinc_pi(double t, bool force_integer_zero) {
  if (t == 0.0) {
    return 1.0;
  }
  if (force_integer_zero && t == std::floor(t)) {
    return 0.0;
  }
  const double pit = 3.14159265358979323846 * t;
  return std::sin(pit) / pit;
}

// Blackman-Harris window over [-half_span, half_span]. The generator only ever evaluates
// `x` inside that support by construction (the widest tap lands at exactly +half_span,
// the narrowest above -half_span), so `t` is always in [0, 1]. Note the window follows
// the CONTINUOUS sinc argument `x` -- which shifts with the phase -- not the tap index.
inline double bh_window(double x, double half_span) {
  const double t = (x + half_span) / (2.0 * half_span); // in [0, 1]
  constexpr double a0 = 0.35875, a1 = 0.48829, a2 = 0.14128, a3 = 0.01168;
  constexpr double two_pi = 6.28318530717958647692;
  return a0 - a1 * std::cos(two_pi * t) + a2 * std::cos(2.0 * two_pi * t) -
         a3 * std::cos(3.0 * two_pi * t);
}

// The prototype. `fc` is the cutoff as a fraction of the input Nyquist: 1.0 for the fixed
// input-Nyquist bank (the frozen upsampling table), `dst_rate/src_rate < 1` for the
// widened device-Nyquist bank (decimation, where the impulse response widens by the
// decimation ratio and `taps` scales with it to preserve lobe count / stopband quality).
//
// Per-phase DC normalization (unity passband gain), then a single freeze to float32.
// The normalization is a reciprocal-multiply, matching what generated the checked-in
// table byte-for-byte -- do not "simplify" it to a division.
//
// Called with (32, 16, 1.0, true) this reproduces `k_resampler_coeffs` in
// `audio_resampler.cpp` exactly, all 512 coefficients; that is what the `[.regen]` dump
// in `t/audio_resampler.t.cpp` prints, and it is the table's only regeneration path.
inline PolyphaseBank generate(std::uint64_t phases, std::int64_t taps, double fc,
                              bool force_integer_zero) {
  // Tap `k` of phase `p` sits at offset `(k - half) - p/phases` from the output instant,
  // in input-sample units (mirrors `mac_frame`'s idx = center - half + k against
  // center + phase/phases).
  const std::int64_t half = taps / 2 - 1;
  const double half_span = static_cast<double>(taps) / 2.0;

  PolyphaseBank bank;
  bank.taps = taps;
  bank.coeffs.assign(static_cast<std::size_t>(phases) * static_cast<std::size_t>(taps), 0.0F);
  std::vector<double> row(static_cast<std::size_t>(taps), 0.0);
  for (std::uint64_t p = 0; p < phases; ++p) {
    double sum = 0.0;
    for (std::int64_t k = 0; k < taps; ++k) {
      const double x =
          static_cast<double>(k - half) - static_cast<double>(p) / static_cast<double>(phases);
      const double c = fc * sinc_pi(fc * x, force_integer_zero) * bh_window(x, half_span);
      row[static_cast<std::size_t>(k)] = c;
      sum += c;
    }
    const double inv = sum != 0.0 ? 1.0 / sum : 1.0;
    for (std::int64_t k = 0; k < taps; ++k) {
      bank.coeffs[static_cast<std::size_t>(p) * static_cast<std::size_t>(taps) +
                  static_cast<std::size_t>(k)] =
          static_cast<float>(row[static_cast<std::size_t>(k)] * inv);
    }
  }
  return bank;
}

// The checked-in frozen bank and its shape, defined in `audio_resampler.cpp` (the table
// stays where its design commentary lives). Exposed through this INTERNAL header purely
// so the component's own tests can (a) assert `generate(phases, taps, 1.0, true)` still
// reproduces it byte-for-byte -- the guard against the generator and the table drifting
// apart -- and (b) dump it in the `[.regen]` case.
struct FrozenBank {
  const float* coeffs;
  std::size_t count;
  std::uint64_t phases;
  std::int64_t taps;
};

FrozenBank frozen_bank();

} // namespace resampler_prototype
} // namespace arbc
