// The blend-mode vocabulary itself (issue #36): the per-channel colour functions, their limit
// cases, and the persistent names. Unit coverage of `arbc/media/blend_mode.hpp` alone -- the
// composite that consumes it is `backend_cpu`'s kernel, and the end-to-end layer behaviour is
// `tests/layer_blend_mode.t.cpp`.
//
// The expected values here are computed from the reference formulas (PDF 1.4 / CSS Compositing
// and Blending Level 1) BY HAND, not by re-running the implementation: a test that restated the
// code would pass for a transcription error in either direction.

#include <arbc/media/blend_mode.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string_view>

using arbc::blend_channel;
using arbc::blend_mode_from_name;
using arbc::blend_mode_name;
using arbc::BlendMode;
using Catch::Approx;

TEST_CASE("the separable blend modes evaluate their reference formulas") {
  // Backdrop 0.6, source 0.4 -- neither at a boundary, so every branch is exercised on its
  // ordinary arm rather than on a limit case.
  const float cb = 0.6F;
  const float cs = 0.4F;

  CHECK(blend_channel(BlendMode::Normal, cb, cs) == 0.4F);             // Cs
  CHECK(blend_channel(BlendMode::Multiply, cb, cs) == Approx(0.24F));  // Cb*Cs
  CHECK(blend_channel(BlendMode::Screen, cb, cs) == Approx(0.76F));    // Cb+Cs-Cb*Cs
  CHECK(blend_channel(BlendMode::Darken, cb, cs) == 0.4F);             // min
  CHECK(blend_channel(BlendMode::Lighten, cb, cs) == 0.6F);            // max
  CHECK(blend_channel(BlendMode::Difference, cb, cs) == Approx(0.2F)); // |Cb-Cs|
  CHECK(blend_channel(BlendMode::Exclusion, cb, cs) == Approx(0.52F)); // Cb+Cs-2CbCs
  CHECK(blend_channel(BlendMode::ColorDodge, cb, cs) == Approx(1.0F)); // 0.6/0.6 = 1
  // 1 - min((1-Cb)/Cs, 1) = 1 - min(1, 1); a margin, not a ratio, because the exact answer is
  // zero and float32's 0.4 is not exactly 0.4.
  CHECK(blend_channel(BlendMode::ColorBurn, cb, cs) == Approx(0.0F).margin(1e-6));
  CHECK(blend_channel(BlendMode::HardLight, cb, cs) == Approx(0.48F)); // Cs<=.5: 2*Cs*Cb
  // Cs <= .5 arm: Cb - (1-2Cs)*Cb*(1-Cb) = 0.6 - 0.2*0.6*0.4
  CHECK(blend_channel(BlendMode::SoftLight, cb, cs) == Approx(0.552F));

  // Overlay IS HardLight with the operands swapped -- the reference's own definition, so the
  // relation is the assertion rather than a second hand-computed number.
  CHECK(blend_channel(BlendMode::Overlay, cb, cs) == blend_channel(BlendMode::HardLight, cs, cb));
  // Swapped, the backdrop 0.6 lands on HardLight's upper arm: Screen(0.4, 2*0.6-1) = 0.52.
  CHECK(blend_channel(BlendMode::Overlay, cb, cs) == Approx(0.52F));

  // Normal is the identity on the source, which is what makes it source-over when the
  // composite re-assembles it -- and why the kernel can keep a separate arm for it.
  for (float v = 0.0F; v <= 1.0F; v += 0.125F) {
    CHECK(blend_channel(BlendMode::Normal, 0.3F, v) == v);
  }
}

TEST_CASE("the dividing modes stay finite at their limits") {
  // Without the reference's limit cases these divide by zero, and an infinity or NaN in a
  // premultiplied working buffer does not stay local: it propagates through every later
  // composite in the frame.
  CHECK(blend_channel(BlendMode::ColorDodge, 0.0F, 0.9F) == 0.0F); // black backdrop stays black
  CHECK(blend_channel(BlendMode::ColorDodge, 0.5F, 1.0F) == 1.0F); // full source blows out
  CHECK(blend_channel(BlendMode::ColorBurn, 1.0F, 0.1F) == 1.0F);  // white backdrop stays white
  CHECK(blend_channel(BlendMode::ColorBurn, 0.5F, 0.0F) == 0.0F);  // full-dark source burns out

  // And they stay finite for out-of-unit values too, which a linear float working space can
  // legitimately hold (an HDR highlight). Finiteness is the promise; the values are unclamped.
  for (const float v : {-0.5F, 1.5F, 4.0F}) {
    CHECK(std::isfinite(blend_channel(BlendMode::ColorDodge, 0.5F, v)));
    CHECK(std::isfinite(blend_channel(BlendMode::ColorDodge, v, 0.5F)));
    CHECK(std::isfinite(blend_channel(BlendMode::ColorBurn, 0.5F, v)));
    CHECK(std::isfinite(blend_channel(BlendMode::ColorBurn, v, 0.5F)));
  }
}

TEST_CASE("SoftLight's piecewise D(Cb) is continuous where the arms meet") {
  // The `Cb <= 0.25` polynomial exists so the curve does not kink at 0.25, where a bare sqrt
  // would. Straddle the seam with a source on the upper arm (the only arm D is reached from).
  const float below = blend_channel(BlendMode::SoftLight, 0.25F - 1e-4F, 0.75F);
  const float above = blend_channel(BlendMode::SoftLight, 0.25F + 1e-4F, 0.75F);
  CHECK(std::fabs(above - below) < 1e-3F);
  // At Cb == 0.25 both definitions agree exactly: the polynomial gives 0.5, and so does
  // sqrt(0.25).
  CHECK(blend_channel(BlendMode::SoftLight, 0.25F, 1.0F) == Approx(0.5F));
}

TEST_CASE("a blend mode's persistent name round-trips, and an unknown one is refused") {
  // The NAME is what a document carries, never the enumerator's number: the enum's order is an
  // implementation detail and a document outlives it.
  for (std::size_t i = 0; i < arbc::k_blend_mode_count; ++i) {
    const auto mode = static_cast<BlendMode>(i);
    BlendMode parsed = BlendMode::Overlay; // a non-default seed, so a no-op "parse" is caught
    INFO(blend_mode_name(mode));
    REQUIRE(blend_mode_from_name(blend_mode_name(mode), parsed));
    CHECK(parsed == mode);
  }
  CHECK(blend_mode_name(BlendMode::Normal) == std::string_view("normal"));
  CHECK(blend_mode_name(BlendMode::ColorDodge) == std::string_view("color-dodge"));

  // An out-of-range enumerator -- which only a corrupt record or a cast can produce -- reads
  // as `normal` rather than off the end of the table, in both directions.
  const auto bogus = static_cast<BlendMode>(200);
  CHECK(blend_mode_name(bogus) == std::string_view("normal"));
  CHECK(blend_channel(bogus, 0.6F, 0.4F) == 0.4F);

  BlendMode out = BlendMode::Multiply;
  CHECK_FALSE(blend_mode_from_name("hue", out)); // a real mode this build does not have
  CHECK_FALSE(blend_mode_from_name("", out));
  CHECK_FALSE(blend_mode_from_name("Multiply", out)); // the spelling is exact
  CHECK(out == BlendMode::Multiply);                  // a refused parse writes nothing
}
