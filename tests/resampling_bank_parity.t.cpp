#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/kind_raster/raster_content.hpp>
#include <arbc/media/pixel_traits.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/surface/typed_span.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

// The doc-07 promise -- the resampling filters are "shared by the kinds' mip
// pyramids and the backend's compositing kernels" -- made byte-testable. A
// scale-ladder rung `CpuBackend::downsample` (backend_cpu, L3) builds equals,
// pixel-for-pixel, the mip level `kind_raster` (L4) builds from the same source,
// because both now run the one `arbc::media` (L1) Lanczos-3 half-band bank
// instead of duplicating a coefficient table across an un-crossable level
// boundary (doc 17). This is the raison d'etre of color.resample_filter_quality:
// before the swap the compositor point-interpolated with a box mean, so a
// compositor rung did NOT match a kind's mip; now it does.
//
// Parity holds at every INTERIOR rung pixel whose 6-tap support (2 left / 3 right
// of the 2:1 footprint) stays in range. The surface border is exactly where the
// two consumers' deliberately-different edge conventions diverge -- the compositor
// fetches a transparent zero border (antialiased falloff for a content-sized temp)
// while a kind clamps to edge (a zero surround would darken every level border) --
// so the border is excluded here and asserted to DIVERGE, which proves the
// interior parity is the shared filter and not a trivially-equal empty rung
// (refinement Constraint 4 / Decisions; doc 07 Sec Resampling filters).

namespace {

using arbc::PixelFormat;

// A deterministic gradient source (opaque): distinct enough per pixel that a 2:1
// decimation is non-trivial and the bank's taps are actually exercised. rgba32f
// `decode` is identity, so both consumers' level-0 buffers are bit-identical to
// these floats and the only remaining variable is the edge convention.
arbc::WorkingPixel src_pixel(int x, int y) {
  const auto fx = static_cast<float>(x);
  const auto fy = static_cast<float>(y);
  return {0.02F * fx + 0.01F * fy, 0.05F + 0.03F * fy, 0.4F - 0.02F * fx, 1.0F};
}

} // namespace

// enforces: 07-color-and-pixel-formats#shared-resampling-bank-parity
TEST_CASE("a compositor rung is byte-identical to a kind_raster mip through the shared bank") {
  constexpr int kW = 8;
  constexpr int kH = 8;
  constexpr int kRW = kW / 2;
  constexpr int kRH = kH / 2;

  // The one shared source, row-major rgba32f floats.
  std::vector<float> src_floats(static_cast<std::size_t>(kW) * static_cast<std::size_t>(kH) * 4);
  for (int y = 0; y < kH; ++y) {
    for (int x = 0; x < kW; ++x) {
      const arbc::WorkingPixel p = src_pixel(x, y);
      const std::size_t at = (static_cast<std::size_t>(y) * kW + static_cast<std::size_t>(x)) * 4;
      for (std::size_t k = 0; k < 4; ++k) {
        src_floats[at + k] = p[k];
      }
    }
  }

  // Backend side: CpuBackend::downsample over the 8x8 surface (zero-border fetch).
  arbc::CpuBackend backend;
  arbc::CpuSurface src(kW, kH, arbc::k_working_rgba32f);
  {
    const std::span<float> px = src.span<PixelFormat::Rgba32fLinearPremul>();
    for (std::size_t i = 0; i < src_floats.size(); ++i) {
      px[i] = src_floats[i];
    }
  }
  arbc::CpuSurface dst(kRW, kRH, arbc::k_working_rgba32f);
  backend.downsample(dst, src);
  const std::span<const float> rung = std::as_const(dst).span<PixelFormat::Rgba32fLinearPremul>();

  // Kind side: kind_raster's level-1 mip from the identical floats (clamp-to-edge
  // fetch). One tile per level (edge == kW), so level 1 is the 4x4 rung.
  arbc::DecodedImage image;
  image.width = kW;
  image.height = kH;
  image.format = arbc::k_working_rgba32f;
  image.bytes.resize(src_floats.size() * sizeof(float));
  std::memcpy(image.bytes.data(), src_floats.data(), image.bytes.size());
  const arbc::RasterContent content(std::move(image), /*tile_edge=*/kW);
  const std::vector<float> mip = content.store().base_table()->level_pixels(1);
  REQUIRE(mip.size() == rung.size());

  // Compare only interior rung pixels whose 6-tap support stays in range on both
  // axes: 2x-2 >= 0 and 2x+3 <= kW-1 -> x in {1, 2} for kW == 8. There the
  // zero-border and clamp-to-edge fetches read the identical in-range taps, so the
  // one shared bank yields byte-identical bytes.
  bool compared_interior = false;
  for (int y = 0; y < kRH; ++y) {
    for (int x = 0; x < kRW; ++x) {
      const bool interior =
          (2 * x - 2 >= 0) && (2 * x + 3 <= kW - 1) && (2 * y - 2 >= 0) && (2 * y + 3 <= kH - 1);
      if (!interior) {
        continue;
      }
      compared_interior = true;
      const std::size_t at = (static_cast<std::size_t>(y) * kRW + static_cast<std::size_t>(x)) * 4;
      for (std::size_t k = 0; k < 4; ++k) {
        CAPTURE(x, y, k);
        REQUIRE(rung[at + k] == mip[at + k]);
      }
    }
  }
  REQUIRE(compared_interior); // the interior comparison set is non-empty

  // The border legitimately DIVERGES: at rung pixel (0,0) the support reaches the
  // negative surround, where the compositor reads zero and the kind clamps to
  // edge. The opaque source has alpha 1 everywhere, so the kind's clamped support
  // decimates alpha to the bank's unit DC gain (1.0), while the compositor's
  // zero-border support drops the half-band's outer taps (whose weights net
  // negative) and so overshoots ABOVE 1.0 -- distinct bytes, proving the interior
  // parity above is the shared filter, not an all-zero coincidence.
  REQUIRE(rung[3] != mip[3]);
  REQUIRE(rung[3] > mip[3]);
}
