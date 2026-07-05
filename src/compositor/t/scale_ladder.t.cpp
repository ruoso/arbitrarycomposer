#include <arbc/compositor/scale_ladder.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <limits>

// Exhaustive edge-case unit tests for the scale-ladder rung algebra (doc 16:46
// lists the scale ladder as a fast, exhaustive unit-test item). The box-reducer
// `reduce_rung` needs a backend and is exercised byte-exactly by the
// cross-component golden `tests/scale_ladder_golden.t.cpp`; these tests cover
// the pure quantization arithmetic.

namespace {

using arbc::rung_scale;
using arbc::RungSelection;
using arbc::ScaleRung;
using arbc::select_rung;

} // namespace

// enforces: 04-transforms-and-infinite-zoom#scale-quantized-to-power-of-two-rung-prefer-downsample
TEST_CASE("select_rung: power-of-two scales quantize exactly, remainder is bit-exact 1.0") {
  // At every representable power of two, the requested scale already sits on a
  // rung: the index is exactly k and the residual is bit-exactly 1.0 so the
  // composite pays no resampling cost (the tap collapses to the nearest tap).
  for (std::int32_t k = -20; k <= 20; ++k) {
    const double scale = std::ldexp(1.0, k);
    const RungSelection sel = select_rung(scale);

    CHECK(sel.rung == ScaleRung{k});
    // Bit-exact comparison, not near: division of equal exact doubles is 1.0.
    CHECK(sel.remainder == 1.0);
    // rung_scale round-trips the power of two bit-exactly.
    CHECK(rung_scale(ScaleRung{k}) == scale);
  }
}

// enforces: 04-transforms-and-infinite-zoom#scale-quantized-to-power-of-two-rung-prefer-downsample
TEST_CASE("select_rung: prefer-downsample rounds the rung up in scale at boundaries") {
  for (std::int32_t k = -20; k <= 20; ++k) {
    const double power = std::ldexp(1.0, k);

    SECTION("just above 2^k selects the next-coarser rung k+1") {
      const double scale = std::nextafter(power, std::numeric_limits<double>::infinity());
      const RungSelection sel = select_rung(scale);
      CHECK(sel.rung == ScaleRung{k + 1});
      // Residual just above 0.5: at most one octave of composite downsample.
      CHECK(sel.remainder > 0.5);
      CHECK(sel.remainder < 0.51);
    }

    SECTION("just below 2^k stays on the finer rung k") {
      const double scale = std::nextafter(power, 0.0);
      const RungSelection sel = select_rung(scale);
      CHECK(sel.rung == ScaleRung{k});
      // Residual just below 1.0.
      CHECK(sel.remainder < 1.0);
      CHECK(sel.remainder > 0.999);
    }
  }
}

// enforces: 04-transforms-and-infinite-zoom#scale-quantized-to-power-of-two-rung-prefer-downsample
TEST_CASE("select_rung: remainder invariant holds across a dense multi-octave sweep") {
  // Sweep scales spanning several octaves below and above 1.0. For each, the
  // rung brackets the scale from above by <1 octave and the residual sits in
  // (0.5, 1.0], and remainder * rung_scale reconstructs the scale.
  for (int i = 1; i <= 4000; ++i) {
    const double scale = std::ldexp(static_cast<double>(i) * 0.013, -6); // ~2^-13 .. ~2^0.7
    const RungSelection sel = select_rung(scale);
    const double rs = rung_scale(sel.rung);

    // The rung is at or above the requested scale (prefer-downsample) and no
    // more than one octave above it.
    CHECK(rs >= scale);
    CHECK(rs < 2.0 * scale);
    // <=1-octave residual bound.
    CHECK(sel.remainder > 0.5);
    CHECK(sel.remainder <= 1.0);

    // Reconstruction is a float round-trip (not a byte-exact render), so a
    // 1-ulp tolerance on the reconstructed scale is the justified bound.
    const double reconstructed = sel.remainder * rs;
    const double tol = std::nextafter(scale, std::numeric_limits<double>::infinity()) - scale;
    CHECK(std::abs(reconstructed - scale) <= tol);
  }
}

// enforces: 04-transforms-and-infinite-zoom#scale-quantized-to-power-of-two-rung-prefer-downsample
TEST_CASE("select_rung: rung index is non-decreasing in scale") {
  // A larger scale never selects a coarser rung (monotone quantization).
  std::int32_t prev = std::numeric_limits<std::int32_t>::min();
  for (int i = 1; i <= 5000; ++i) {
    const double scale = std::ldexp(static_cast<double>(i) * 0.017, -4);
    const std::int32_t index = select_rung(scale).rung.index;
    CHECK(index >= prev);
    prev = index;
  }
}

// The precondition `scale > 0 && std::isfinite(scale)` is asserted as a
// contract (doc 04:115-117: callers cull degenerate/non-finite composed scale
// upstream), not returned as a value -- so there is no return-value behavior to
// assert here. Documented rather than death-tested to keep the suite portable.
TEST_CASE("rung_scale: matches select_rung's inverse at the rungs") {
  for (std::int32_t k = -20; k <= 20; ++k) {
    CHECK(rung_scale(ScaleRung{k}) == std::ldexp(1.0, k));
  }
}
