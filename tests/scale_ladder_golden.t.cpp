#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/compositor/scale_ladder.hpp>
#include <arbc/media/image_resampler.hpp>
#include <arbc/media/pixel_format.hpp>
#include <arbc/media/surface_format.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <span>

// Byte-exact cross-component golden for the scale ladder (doc 16:47-53). The
// pure quantization arithmetic is unit-tested in
// `src/compositor/t/scale_ladder.t.cpp`; here we pin the one pixel-producing
// operation the ladder owns -- `reduce_rung`, the exact 2:1 Lanczos-3 half-band
// reduction in linear premultiplied working space (doc 07 § Resampling filters;
// color.resample_filter_quality swapped it off the walking skeleton's box mean) --
// and the prefer-downsample collapse: that a power-of-two rung request
// (remainder == 1.0, integer alignment) pays no resampling cost through
// `Backend::composite`.
//
// The per-pixel FILTER bytes themselves are owned byte-exactly by
// `src/backend_cpu/t/kernel_goldens.t.cpp` (kDownExp*); here the ladder's job is to
// apply that reduction faithfully to the whole surface and drop the rung index, so
// a 2x2 case is pinned against the frozen coefficient table directly and a
// multi-tile case is pinned against a direct `Backend::downsample` (the operation
// `reduce_rung` wraps), which also carries the 6-tap support's cross-block reach
// the retired box mean did not have.

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
  REQUIRE(got[0] == want[0]);
  REQUIRE(got[1] == want[1]);
  REQUIRE(got[2] == want[2]);
  REQUIRE(got[3] == want[3]);
}

// The exact 2:1 half-band value of a 2x2 source (its only four taps in range, the
// zero border everywhere else), folded in the identical order the shipped kernel
// uses so the `==` is genuinely byte-exact. `(a,b)` is the top row's tap pair,
// `(c,d)` the bottom row's. Valid ONLY for a 2x2 source -- a wider source lets the
// 6-tap support reach neighbouring taps (exercised against `downsample` below).
float half_band_2x2(float a, float b, float c, float d) {
  const float w = arbc::lanczos3_half_band()[2];
  return w * (w * (a + b) + w * (c + d));
}

} // namespace

// enforces: 04-transforms-and-infinite-zoom#scale-quantized-to-power-of-two-rung-prefer-downsample
TEST_CASE("reduce_rung: 2:1 Lanczos-3 half-band reduction, rung index drops by one") {
  arbc::CpuBackend backend;

  SECTION("non-uniform 2x2 -> 1x1 equals the half-band decimation of the four taps") {
    arbc::CpuSurface src(2, 2, arbc::k_working_rgba32f);
    write_px(src, 0, {0.25F, 0.50F, 0.75F, 1.0F}); // (0,0)
    write_px(src, 1, {0.75F, 0.50F, 0.25F, 1.0F}); // (1,0)
    write_px(src, 2, {0.50F, 0.25F, 0.00F, 1.0F}); // (0,1)
    write_px(src, 3, {0.00F, 0.75F, 1.00F, 1.0F}); // (1,1)
    arbc::CpuSurface dst(1, 1, arbc::k_working_rgba32f);

    const arbc::ScaleRung coarser = arbc::reduce_rung(backend, dst, src, arbc::ScaleRung{3});

    // Coarser rung is exactly one octave down.
    CHECK(coarser == arbc::ScaleRung{2});
    // The half-band fold of the four in-range taps, per channel (decisively not the
    // retired box mean {0.375, 0.5, 0.5, 1.0}).
    require_px_eq(read_px(dst, 0), {half_band_2x2(0.25F, 0.75F, 0.50F, 0.00F),
                                    half_band_2x2(0.50F, 0.50F, 0.25F, 0.75F),
                                    half_band_2x2(0.75F, 0.25F, 0.00F, 1.00F),
                                    half_band_2x2(1.0F, 1.0F, 1.0F, 1.0F)});
  }

  SECTION("multiple output pixels: 4x2 -> 2x1 reduces the whole surface, byte-for-byte") {
    arbc::CpuSurface src(4, 2, arbc::k_working_rgba32f);
    write_px(src, 0, {0.25F, 0.50F, 0.75F, 1.0F}); // (0,0)
    write_px(src, 1, {0.75F, 0.50F, 0.25F, 1.0F}); // (1,0)
    write_px(src, 4, {0.50F, 0.25F, 0.00F, 1.0F}); // (0,1)
    write_px(src, 5, {0.00F, 0.75F, 1.00F, 1.0F}); // (1,1)
    write_px(src, 2, {0.5F, 0.5F, 0.5F, 1.0F});
    write_px(src, 3, {0.5F, 0.5F, 0.5F, 1.0F});
    write_px(src, 6, {0.5F, 0.5F, 0.5F, 1.0F});
    write_px(src, 7, {0.5F, 0.5F, 0.5F, 1.0F});
    arbc::CpuSurface dst(2, 1, arbc::k_working_rgba32f);

    const arbc::ScaleRung coarser = arbc::reduce_rung(backend, dst, src, arbc::ScaleRung{0});

    CHECK(coarser == arbc::ScaleRung{-1});
    // reduce_rung is the ladder's thin wrapper over Backend::downsample: its output
    // is byte-identical to the operation it wraps (whose per-pixel filter bytes are
    // owned by kernel_goldens.t.cpp). The 6-tap support at output x=1 reaches back
    // into the left block -- a cross-block reach the box mean never had -- so a
    // direct downsample is the faithful reference for the multi-pixel case.
    arbc::CpuSurface ref(2, 1, arbc::k_working_rgba32f);
    backend.downsample(ref, src);
    require_px_eq(read_px(dst, 0), read_px(ref, 0));
    require_px_eq(read_px(dst, 1), read_px(ref, 1));
    // Genuinely two independent output pixels, not a fill: the two blocks differ.
    REQUIRE(read_px(dst, 0)[0] != read_px(dst, 1)[0]);
  }
}

// enforces: 04-transforms-and-infinite-zoom#scale-quantized-to-power-of-two-rung-prefer-downsample
TEST_CASE("reduce_rung: the reduction is taken in linear-light premultiplied working floats") {
  arbc::CpuBackend backend;

  SECTION("a flat 2x2 patch decimates by the half-band fold, not a DC-preserving mean") {
    arbc::CpuSurface src(2, 2, arbc::k_working_rgba32f);
    const std::array<float, 4> value{0.25F, 0.5F, 0.75F, 1.0F};
    for (int i = 0; i < 4; ++i) {
      write_px(src, i, value);
    }
    arbc::CpuSurface dst(1, 1, arbc::k_working_rgba32f);

    arbc::reduce_rung(backend, dst, src, arbc::ScaleRung{4});

    // The zero border strips the half-band's negative side-lobes, so a flat patch
    // does not reduce to itself (the box mean did): each channel is the half-band
    // fold of four equal taps, which over-counts the positive centre lobe.
    require_px_eq(read_px(dst, 0),
                  {half_band_2x2(0.25F, 0.25F, 0.25F, 0.25F), half_band_2x2(0.5F, 0.5F, 0.5F, 0.5F),
                   half_band_2x2(0.75F, 0.75F, 0.75F, 0.75F),
                   half_band_2x2(1.0F, 1.0F, 1.0F, 1.0F)});
    REQUIRE(read_px(dst, 0)[0] > value[0]); // decisively not DC-preserving
  }

  SECTION("a two-value checker decimates in linear light, per channel") {
    arbc::CpuSurface src(2, 2, arbc::k_working_rgba32f);
    const std::array<float, 4> lo{0.25F, 0.25F, 0.25F, 1.0F};
    const std::array<float, 4> hi{0.75F, 0.75F, 0.75F, 1.0F};
    write_px(src, 0, lo);
    write_px(src, 1, hi);
    write_px(src, 2, hi);
    write_px(src, 3, lo);
    arbc::CpuSurface dst(1, 1, arbc::k_working_rgba32f);

    arbc::reduce_rung(backend, dst, src, arbc::ScaleRung{1});

    // lo,hi (top row) and hi,lo (bottom row): the symmetric half-band fold in the
    // linear working space, decisively not a gamma-space or box average.
    require_px_eq(read_px(dst, 0), {half_band_2x2(0.25F, 0.75F, 0.75F, 0.25F),
                                    half_band_2x2(0.25F, 0.75F, 0.75F, 0.25F),
                                    half_band_2x2(0.25F, 0.75F, 0.75F, 0.25F),
                                    half_band_2x2(1.0F, 1.0F, 1.0F, 1.0F)});
  }
}

// enforces: 04-transforms-and-infinite-zoom#scale-quantized-to-power-of-two-rung-prefer-downsample
TEST_CASE("prefer-downsample collapse: a power-of-two rung request pays no resampling cost") {
  arbc::CpuBackend backend;

  // A power-of-two request scale lands exactly on a rung: remainder == 1.0, so the
  // composite is integer-aligned and the Catmull-Rom tap's integer-phase weights
  // (0,1,0,0) collapse to the byte-exact nearest tap -- viewed from the ladder's
  // seam, no resampling.
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

  // Integer-aligned (identity) source-over onto transparent black: the result is
  // byte-identical to `src`, the un-resampled source-over.
  backend.composite(dst, src, arbc::Affine::identity(), 1.0);

  for (int i = 0; i < 4; ++i) {
    require_px_eq(read_px(dst, i), read_px(src, i));
  }
}
