#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/compositor/scale_ladder.hpp>
#include <arbc/media/pixel_format.hpp>
#include <arbc/media/surface_format.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <span>

// Byte-exact cross-component golden for the scale ladder (doc 16:47-53). The
// pure quantization arithmetic is unit-tested in
// `src/compositor/t/scale_ladder.t.cpp`; here we pin the one pixel-producing
// operation the ladder owns -- `reduce_rung`, the exact 2:1 box reduction in
// linear premultiplied working space -- and the prefer-downsample collapse:
// that a power-of-two rung request (remainder == 1.0, integer alignment) pays
// no resampling cost through `Backend::composite`.
//
// All fill values are small dyadic rationals whose 2x2 means are exactly
// representable in float regardless of the reducer's summation order, so the
// `==` comparisons are genuinely byte-exact, not near-equal.

namespace {

using arbc::PixelFormat;

constexpr auto k_fmt = PixelFormat::Rgba32fLinearPremul;

// Write pixel `idx` (row-major) as four raw working-space floats. For
// Rgba32fLinearPremul the stored bytes are the decoded premultiplied linear
// working sample verbatim, so a raw-float write is a working-space write.
void write_px(arbc::Surface& surface, int idx, std::array<float, 4> rgba) {
  const std::span<float> px = surface.span<k_fmt>();
  const auto base = static_cast<std::size_t>(idx) * 4;
  px[base + 0] = rgba[0];
  px[base + 1] = rgba[1];
  px[base + 2] = rgba[2];
  px[base + 3] = rgba[3];
}

std::array<float, 4> read_px(const arbc::Surface& surface, int idx) {
  const std::span<const float> px = surface.span<k_fmt>();
  const auto base = static_cast<std::size_t>(idx) * 4;
  return {px[base + 0], px[base + 1], px[base + 2], px[base + 3]};
}

void require_px_eq(std::array<float, 4> got, std::array<float, 4> want) {
  // Byte-exact: the chosen dyadic values make every 2x2 mean exactly
  // representable, so the reducer must reproduce the reference bit-for-bit.
  REQUIRE(got[0] == want[0]);
  REQUIRE(got[1] == want[1]);
  REQUIRE(got[2] == want[2]);
  REQUIRE(got[3] == want[3]);
}

} // namespace

// enforces: 04-transforms-and-infinite-zoom#scale-quantized-to-power-of-two-rung-prefer-downsample
TEST_CASE("reduce_rung: 2x2 box reduction is the byte-exact 2x2 mean, rung index drops by one") {
  arbc::CpuBackend backend;

  SECTION("non-uniform 2x2 -> 1x1 equals the hand-computed 2x2 mean") {
    arbc::CpuSurface src(2, 2, arbc::k_working_rgba32f);
    write_px(src, 0, {0.25F, 0.50F, 0.75F, 1.0F});
    write_px(src, 1, {0.75F, 0.50F, 0.25F, 1.0F});
    write_px(src, 2, {0.50F, 0.25F, 0.00F, 1.0F});
    write_px(src, 3, {0.00F, 0.75F, 1.00F, 1.0F});
    arbc::CpuSurface dst(1, 1, arbc::k_working_rgba32f);

    const arbc::ScaleRung coarser = arbc::reduce_rung(backend, dst, src, arbc::ScaleRung{3});

    // Coarser rung is exactly one octave down.
    CHECK(coarser == arbc::ScaleRung{2});
    // r: (0.25+0.75+0.50+0.00)/4 = 0.375; g: 2.0/4 = 0.5; b: 2.0/4 = 0.5.
    require_px_eq(read_px(dst, 0), {0.375F, 0.5F, 0.5F, 1.0F});
  }

  SECTION("multiple output pixels: 4x2 -> 2x1 reduces each 2x2 block independently") {
    arbc::CpuSurface src(4, 2, arbc::k_working_rgba32f);
    // Left 2x2 block (cols 0-1): the non-uniform pattern above.
    write_px(src, 0, {0.25F, 0.50F, 0.75F, 1.0F}); // (0,0)
    write_px(src, 1, {0.75F, 0.50F, 0.25F, 1.0F}); // (1,0)
    write_px(src, 4, {0.50F, 0.25F, 0.00F, 1.0F}); // (0,1)
    write_px(src, 5, {0.00F, 0.75F, 1.00F, 1.0F}); // (1,1)
    // Right 2x2 block (cols 2-3): uniform.
    write_px(src, 2, {0.5F, 0.5F, 0.5F, 1.0F});
    write_px(src, 3, {0.5F, 0.5F, 0.5F, 1.0F});
    write_px(src, 6, {0.5F, 0.5F, 0.5F, 1.0F});
    write_px(src, 7, {0.5F, 0.5F, 0.5F, 1.0F});
    arbc::CpuSurface dst(2, 1, arbc::k_working_rgba32f);

    const arbc::ScaleRung coarser = arbc::reduce_rung(backend, dst, src, arbc::ScaleRung{0});

    CHECK(coarser == arbc::ScaleRung{-1});
    require_px_eq(read_px(dst, 0), {0.375F, 0.5F, 0.5F, 1.0F}); // left block mean
    require_px_eq(read_px(dst, 1), {0.5F, 0.5F, 0.5F, 1.0F});   // uniform block mean
  }
}

// enforces: 04-transforms-and-infinite-zoom#scale-quantized-to-power-of-two-rung-prefer-downsample
TEST_CASE("reduce_rung: energy/mean conservation in linear-light working space") {
  arbc::CpuBackend backend;

  SECTION("a uniform surface reduces to the same uniform value byte-exactly") {
    arbc::CpuSurface src(4, 4, arbc::k_working_rgba32f);
    const std::array<float, 4> value{0.25F, 0.5F, 0.75F, 1.0F};
    for (int i = 0; i < 16; ++i) {
      write_px(src, i, value);
    }
    arbc::CpuSurface dst(2, 2, arbc::k_working_rgba32f);

    arbc::reduce_rung(backend, dst, src, arbc::ScaleRung{4});

    for (int i = 0; i < 4; ++i) {
      require_px_eq(read_px(dst, i), value);
    }
  }

  SECTION("a two-value checker reduces to the exact average") {
    arbc::CpuSurface src(2, 2, arbc::k_working_rgba32f);
    const std::array<float, 4> lo{0.25F, 0.25F, 0.25F, 1.0F};
    const std::array<float, 4> hi{0.75F, 0.75F, 0.75F, 1.0F};
    write_px(src, 0, lo);
    write_px(src, 1, hi);
    write_px(src, 2, hi);
    write_px(src, 3, lo);
    arbc::CpuSurface dst(1, 1, arbc::k_working_rgba32f);

    arbc::reduce_rung(backend, dst, src, arbc::ScaleRung{1});

    // Two lo + two hi -> exact average 0.5 per color channel.
    require_px_eq(read_px(dst, 0), {0.5F, 0.5F, 0.5F, 1.0F});
  }
}

// enforces: 04-transforms-and-infinite-zoom#scale-quantized-to-power-of-two-rung-prefer-downsample
TEST_CASE("prefer-downsample collapse: a power-of-two rung request pays no resampling cost") {
  arbc::CpuBackend backend;

  // A power-of-two request scale lands exactly on a rung: remainder == 1.0,
  // so the composite is integer-aligned and the bilinear tap collapses to the
  // byte-exact nearest tap -- viewed from the ladder's seam, no resampling.
  const arbc::RungSelection sel = arbc::select_rung(4.0);
  CHECK(sel.rung == arbc::ScaleRung{2});
  REQUIRE(sel.remainder == 1.0);

  arbc::CpuSurface src(2, 2, arbc::k_working_rgba32f);
  write_px(src, 0, {0.25F, 0.50F, 0.75F, 1.0F});
  write_px(src, 1, {0.75F, 0.50F, 0.25F, 1.0F});
  write_px(src, 2, {0.50F, 0.25F, 0.00F, 1.0F});
  write_px(src, 3, {0.10F, 0.20F, 0.30F, 0.5F});

  arbc::CpuSurface dst(2, 2, arbc::k_working_rgba32f);
  backend.clear(dst, 0.0F, 0.0F, 0.0F, 0.0F);

  // Integer-aligned (identity) source-over onto transparent black: the result
  // is byte-identical to `src`, the un-resampled source-over.
  backend.composite(dst, src, arbc::Affine::identity(), 1.0);

  for (int i = 0; i < 4; ++i) {
    require_px_eq(read_px(dst, i), read_px(src, i));
  }
}
