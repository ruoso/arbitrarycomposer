#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/media/pixel_format.hpp>
#include <arbc/media/pixel_traits.hpp>

#include <catch2/catch_test_macros.hpp>

#include "../kernels.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace {

using arbc::PixelFormat;

void fill(arbc::Surface& surface, float r, float g, float b, float a) {
  arbc::CpuBackend backend;
  backend.clear(surface, r, g, b, a);
}

// Decode pixel 0 of a same-format surface back into the linear working sample,
// so per-format blends can be compared against the 32f reference.
template <PixelFormat F> arbc::WorkingPixel decode0(const arbc::Surface& surface) {
  const std::span<const typename arbc::PixelTraits<F>::Storage> px = surface.span<F>();
  return arbc::PixelTraits<F>::decode(&px[0]);
}

void require_close(const arbc::WorkingPixel& got, const arbc::WorkingPixel& want, float tol) {
  for (std::size_t k = 0; k < 4; ++k) {
    CAPTURE(k, got[k], want[k], tol);
    REQUIRE(std::fabs(got[k] - want[k]) <= tol);
  }
}

// Encode a premultiplied linear working sample into pixel `idx` of `surface`,
// and decode it back -- so a resampling test can lay out a known source and
// read the interpolated destination in the same working coordinates for every
// format (idx is the row-major pixel index, y*width + x).
template <PixelFormat F>
void write_px(arbc::Surface& surface, int idx, const arbc::WorkingPixel& w) {
  const std::span<typename arbc::PixelTraits<F>::Storage> px = surface.span<F>();
  arbc::PixelTraits<F>::encode(w, &px[static_cast<std::size_t>(idx) * 4]);
}

template <PixelFormat F> arbc::WorkingPixel read_px(const arbc::Surface& surface, int idx) {
  const std::span<const typename arbc::PixelTraits<F>::Storage> px = surface.span<F>();
  return arbc::PixelTraits<F>::decode(&px[static_cast<std::size_t>(idx) * 4]);
}

// A red ramp in x (columns 0 and 1), opaque, laid out on a 2x2 source: the
// canonical fixture for exercising the bilinear tap's x interpolation with a
// per-channel value the four taps make analytic.
template <PixelFormat F> void fill_red_ramp_2x2(arbc::Surface& src) {
  write_px<F>(src, 0, {0.0F, 0.0F, 0.0F, 1.0F}); // (0,0)
  write_px<F>(src, 1, {1.0F, 0.0F, 0.0F, 1.0F}); // (1,0)
  write_px<F>(src, 2, {0.0F, 0.0F, 0.0F, 1.0F}); // (0,1)
  write_px<F>(src, 3, {1.0F, 0.0F, 0.0F, 1.0F}); // (1,1)
}

// Bilinear magnification: a 2x upscale of the red ramp resolves interior
// destination pixels to interpolated reds a nearest tap could never produce
// (dst(1,1) samples texel-space x=0.25, dst(2,1) samples x=0.75).
template <PixelFormat F> void check_magnification(arbc::SurfaceFormat fmt, float tol) {
  arbc::CpuBackend backend;
  arbc::CpuSurface src(2, 2, fmt);
  fill_red_ramp_2x2<F>(src);
  arbc::CpuSurface dst(4, 4, fmt);

  backend.composite(dst, src, arbc::Affine::scaling(2.0, 2.0), 1.0);

  const arbc::WorkingPixel p11 = read_px<F>(dst, 1 * 4 + 1);
  const arbc::WorkingPixel p21 = read_px<F>(dst, 1 * 4 + 2);
  CAPTURE(p11[0], p21[0], tol);
  REQUIRE(std::fabs(p11[0] - 0.25F) <= tol); // (1 - 0.25) * 0 + 0.25 * 1 in x
  REQUIRE(std::fabs(p21[0] - 0.75F) <= tol);
  REQUIRE(std::fabs(p11[3] - 1.0F) <= tol); // opaque throughout the interior
  // A genuine blend, not a snapped texel: nearest would land exactly on 0 or 1.
  REQUIRE(p11[0] > tol);
  REQUIRE(p11[0] < 1.0F - tol);
}

// Fractional offset: a half-texel shift makes an interior destination pixel the
// exact two-tap average (frac == 0.5), not a snapped source texel.
template <PixelFormat F> void check_fractional_offset(arbc::SurfaceFormat fmt, float tol) {
  arbc::CpuBackend backend;
  arbc::CpuSurface src(2, 2, fmt);
  fill_red_ramp_2x2<F>(src);
  arbc::CpuSurface dst(2, 2, fmt);

  backend.composite(dst, src, arbc::Affine::translation(0.5, 0.0), 1.0);

  // dst(1,0) maps to texel-space x = 0.5: the exact mean of texels 0 and 1.
  const arbc::WorkingPixel p10 = read_px<F>(dst, 1);
  CAPTURE(p10[0], tol);
  REQUIRE(std::fabs(p10[0] - 0.5F) <= tol);
}

// Identity and pure-integer composites map every destination center to frac 0,
// where the bilinear weights collapse to the single incumbent texel -- the
// output reproduces the pre-filter nearest tap byte-for-byte (the mechanism
// that keeps the walking-skeleton golden intact).
template <PixelFormat F> void check_integer_is_byte_exact(arbc::SurfaceFormat fmt) {
  arbc::CpuBackend backend;
  // Distinct opaque per-pixel colors so a snapped identity is a byte compare.
  arbc::CpuSurface src(3, 1, fmt);
  write_px<F>(src, 0, {0.20F, 0.40F, 0.60F, 1.0F});
  write_px<F>(src, 1, {0.80F, 0.10F, 0.30F, 1.0F});
  write_px<F>(src, 2, {0.05F, 0.90F, 0.50F, 1.0F});

  using Storage = typename arbc::PixelTraits<F>::Storage;
  const std::span<const Storage> src_px = std::as_const(src).span<F>();

  SECTION("identity reproduces the source bytes") {
    arbc::CpuSurface dst(3, 1, fmt);
    backend.composite(dst, src, arbc::Affine::identity(), 1.0);
    const std::span<const Storage> dst_px = std::as_const(dst).span<F>();
    for (std::size_t i = 0; i < src_px.size(); ++i) {
      CAPTURE(i);
      REQUIRE(dst_px[i] == src_px[i]);
    }
  }

  SECTION("integer translation snaps to the shifted texel, byte-exact") {
    arbc::CpuSurface dst(3, 1, fmt);
    backend.composite(dst, src, arbc::Affine::translation(1.0, 0.0), 1.0);
    const std::span<const Storage> dst_px = std::as_const(dst).span<F>();
    // dst(0) is fully off the shifted source (transparent, zero bytes); dst(x>=1)
    // reproduces src(x-1) byte-for-byte.
    for (std::size_t k = 0; k < 4; ++k) {
      CAPTURE(k);
      REQUIRE(dst_px[k] == Storage{0});
      REQUIRE(dst_px[1 * 4 + k] == src_px[0 * 4 + k]);
      REQUIRE(dst_px[2 * 4 + k] == src_px[1 * 4 + k]);
    }
  }
}

TEST_CASE("identity composite over transparent copies the source") {
  arbc::CpuBackend backend;
  arbc::CpuSurface src(2, 2, arbc::k_working_rgba32f);
  fill(src, 1.0F, 0.0F, 0.0F, 1.0F);
  arbc::CpuSurface dst(2, 2, arbc::k_working_rgba32f);

  backend.composite(dst, src, arbc::Affine::identity(), 1.0);

  const std::span<const float> out = std::as_const(dst).span<PixelFormat::Rgba32fLinearPremul>();
  for (std::size_t i = 0; i + 3 < out.size(); i += 4) {
    REQUIRE(out[i + 0] == 1.0F);
    REQUIRE(out[i + 1] == 0.0F);
    REQUIRE(out[i + 2] == 0.0F);
    REQUIRE(out[i + 3] == 1.0F);
  }
}

// enforces: 07-color-and-pixel-formats#premultiplied-source-over
TEST_CASE("composite is source-over on premultiplied alpha") {
  arbc::CpuBackend backend;
  // Premultiplied half-transparent green over opaque red.
  arbc::CpuSurface src(1, 1, arbc::k_working_rgba32f);
  fill(src, 0.0F, 0.5F, 0.0F, 0.5F);
  arbc::CpuSurface dst(1, 1, arbc::k_working_rgba32f);
  fill(dst, 1.0F, 0.0F, 0.0F, 1.0F);

  backend.composite(dst, src, arbc::Affine::identity(), 1.0);

  const std::span<const float> out = std::as_const(dst).span<PixelFormat::Rgba32fLinearPremul>();
  REQUIRE(out[0] == 0.5F); // 0 + (1 - 0.5) * 1
  REQUIRE(out[1] == 0.5F); // 0.5 + (1 - 0.5) * 0
  REQUIRE(out[2] == 0.0F);
  REQUIRE(out[3] == 1.0F); // 0.5 + (1 - 0.5) * 1
}

TEST_CASE("degenerate mappings composite nothing") {
  arbc::CpuBackend backend;
  arbc::CpuSurface src(2, 2, arbc::k_working_rgba32f);
  fill(src, 1.0F, 1.0F, 1.0F, 1.0F);
  arbc::CpuSurface dst(2, 2, arbc::k_working_rgba32f);

  backend.composite(dst, src, arbc::Affine::scaling(0.0, 1.0), 1.0);

  for (const float value : std::as_const(dst).span<PixelFormat::Rgba32fLinearPremul>()) {
    REQUIRE(value == 0.0F);
  }
}

// enforces: 07-color-and-pixel-formats#surfaces-carry-tags
// enforces: 09-surfaces-and-backends#make-surface-faults-as-value
TEST_CASE("a created surface echoes its full tag triple") {
  arbc::CpuBackend backend;
  const arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError> s =
      backend.make_surface(4, 3, arbc::k_working_rgba32f);
  REQUIRE(s.has_value());
  REQUIRE(*s != nullptr);
  REQUIRE((*s)->width() == 4);
  REQUIRE((*s)->height() == 3);

  const arbc::SurfaceFormat f = (*s)->format();
  REQUIRE(f == arbc::k_working_rgba32f);
  REQUIRE(f.pixel_format == PixelFormat::Rgba32fLinearPremul);
  REQUIRE(f.color_space == arbc::k_linear_srgb);
  REQUIRE(f.premultiplied == arbc::Premultiplied::Yes);
}

// enforces: 09-surfaces-and-backends#make-surface-faults-as-value
TEST_CASE("make_surface stores the three working formats and faults on the rest") {
  arbc::CpuBackend backend;
  // Storable with color.kernels: the three closed working formats -- rgba32f,
  // the rgba16f designed default, and the rgba8 sRGB fast mode -- each returns
  // a live surface carrying its exact tag triple.
  const arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError> f32 =
      backend.make_surface(2, 2, arbc::k_working_rgba32f);
  REQUIRE(f32.has_value());
  REQUIRE((*f32)->format() == arbc::k_working_rgba32f);
  const arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError> f16 =
      backend.make_surface(2, 2, arbc::k_working_rgba16f);
  REQUIRE(f16.has_value());
  REQUIRE((*f16)->format() == arbc::k_working_rgba16f);
  const arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError> u8 =
      backend.make_surface(2, 2, arbc::k_fast_rgba8srgb);
  REQUIRE(u8.has_value());
  REQUIRE((*u8)->format() == arbc::k_fast_rgba8srgb);

  // Unsupported: a describable tag triple no kernel honors -- surfaced as a
  // value, never null, never abort.
  const arbc::SurfaceFormat nonlinear{PixelFormat::Rgba32fLinearPremul, arbc::k_srgb,
                                      arbc::Premultiplied::Yes};
  REQUIRE(backend.make_surface(2, 2, nonlinear).error() == arbc::SurfaceError::UnsupportedFormat);
  const arbc::SurfaceFormat straight{PixelFormat::Rgba32fLinearPremul, arbc::k_linear_srgb,
                                     arbc::Premultiplied::No};
  REQUIRE(backend.make_surface(2, 2, straight).error() == arbc::SurfaceError::UnsupportedFormat);
  // rgba8 is a fast-mode format only in straight-alpha sRGB; a premultiplied
  // rgba8 tag has no codec and faults.
  const arbc::SurfaceFormat premul8{PixelFormat::Rgba8Srgb, arbc::k_srgb, arbc::Premultiplied::Yes};
  REQUIRE(backend.make_surface(2, 2, premul8).error() == arbc::SurfaceError::UnsupportedFormat);
}

// enforces: 09-surfaces-and-backends#capabilities-are-honest
TEST_CASE("the reference backend advertises its honest current capabilities") {
  arbc::CpuBackend backend;
  const arbc::BackendCaps caps = backend.capabilities();

  // CPU access is advertised, and a stored surface honors it with a non-empty
  // CPU byte span consistent with the advertised bit.
  REQUIRE(caps.cpu_access == true);
  const arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError> s =
      backend.make_surface(2, 2, arbc::k_working_rgba32f);
  REQUIRE(s.has_value());
  REQUIRE_FALSE((*s)->cpu_bytes().empty());

  // No import or sync machinery exists yet, so neither is advertised until
  // surfaces.import / GPU backends land it.
  REQUIRE(caps.import_handles.empty());
  REQUIRE_FALSE(caps.import_handles.test(arbc::ImportHandle::CpuMemory));
  REQUIRE(caps.sync_primitives == false);
}

// enforces: 07-color-and-pixel-formats#surfaces-carry-tags
TEST_CASE("composite refuses mismatched surface tags") {
  arbc::CpuBackend backend;
  arbc::CpuSurface dst(1, 1, arbc::k_working_rgba32f);
  fill(dst, 1.0F, 0.0F, 0.0F, 1.0F);

  // Same rgba32f storage but a different color-space tag: a working-space
  // mismatch conversions (later edge tasks) would resolve and that this backend
  // must never silently reinterpret.
  const arbc::SurfaceFormat srgb_tagged{PixelFormat::Rgba32fLinearPremul, arbc::k_srgb,
                                        arbc::Premultiplied::Yes};
  arbc::CpuSurface src(1, 1, srgb_tagged);
  fill(src, 0.0F, 1.0F, 0.0F, 1.0F);

  REQUIRE_FALSE(dst.format() == src.format()); // the rejection precondition

#ifdef NDEBUG
  // Release: the mismatch is a no-op cull (asserts compiled out), so dst is
  // left byte-for-byte unchanged -- mixed-tag compositing never happens.
  backend.composite(dst, src, arbc::Affine::identity(), 1.0);
  const std::span<const float> out = std::as_const(dst).span<PixelFormat::Rgba32fLinearPremul>();
  REQUIRE(out[0] == 1.0F);
  REQUIRE(out[1] == 0.0F);
  REQUIRE(out[2] == 0.0F);
  REQUIRE(out[3] == 1.0F);
#endif
  // Debug: the same predicate the composite asserts on is witnessed above
  // without tripping the abort (mirrors the generation-tag convention in
  // pool/t/refs.t.cpp).
}

// enforces: 07-color-and-pixel-formats#storage-sized-by-pixel-format
TEST_CASE("cpu surface storage is width*height*bytes_per_pixel with checked typed views") {
  const arbc::CpuSurface f32(4, 3, arbc::k_working_rgba32f);
  REQUIRE(f32.cpu_bytes().size() == static_cast<std::size_t>(4) * 3 * 16);
  const arbc::CpuSurface f16(4, 3, arbc::k_working_rgba16f);
  REQUIRE(f16.cpu_bytes().size() == static_cast<std::size_t>(4) * 3 * 8);
  const arbc::CpuSurface u8(4, 3, arbc::k_fast_rgba8srgb);
  REQUIRE(u8.cpu_bytes().size() == static_cast<std::size_t>(4) * 3 * 4);

  // A typed span reinterprets the bytes at the format's storage width (4
  // channels per pixel), and a mismatched request yields an empty span -- never
  // a silent cross-format reinterpretation.
  REQUIRE(f16.span<PixelFormat::Rgba16fLinearPremul>().size() ==
          static_cast<std::size_t>(4) * 3 * 4);
  REQUIRE(u8.span<PixelFormat::Rgba8Srgb>().size() == static_cast<std::size_t>(4) * 3 * 4);
  REQUIRE(f16.span<PixelFormat::Rgba8Srgb>().empty());
  REQUIRE(u8.span<PixelFormat::Rgba32fLinearPremul>().empty());
}

TEST_CASE("rgba16f composite: blend properties match the 32f reference within f16 precision") {
  arbc::CpuBackend backend;

  SECTION("a transparent source is byte-exact identity") {
    arbc::CpuSurface dst(1, 1, arbc::k_working_rgba16f);
    fill(dst, 0.6F, 0.2F, 0.1F, 1.0F);
    const std::span<const std::uint16_t> before =
        std::as_const(dst).span<PixelFormat::Rgba16fLinearPremul>();
    const std::array<std::uint16_t, 4> saved{before[0], before[1], before[2], before[3]};

    arbc::CpuSurface src(1, 1, arbc::k_working_rgba16f);
    fill(src, 0.0F, 0.0F, 0.0F, 0.0F);
    backend.composite(dst, src, arbc::Affine::identity(), 1.0);

    const std::span<const std::uint16_t> after =
        std::as_const(dst).span<PixelFormat::Rgba16fLinearPremul>();
    REQUIRE(after[0] == saved[0]);
    REQUIRE(after[1] == saved[1]);
    REQUIRE(after[2] == saved[2]);
    REQUIRE(after[3] == saved[3]);
  }

  SECTION("an opaque source replaces the destination byte-exactly") {
    arbc::CpuSurface dst(1, 1, arbc::k_working_rgba16f);
    fill(dst, 0.6F, 0.2F, 0.1F, 1.0F);
    arbc::CpuSurface src(1, 1, arbc::k_working_rgba16f);
    fill(src, 0.3F, 0.15F, 0.05F, 1.0F);
    backend.composite(dst, src, arbc::Affine::identity(), 1.0);

    const std::span<const std::uint16_t> d =
        std::as_const(dst).span<PixelFormat::Rgba16fLinearPremul>();
    const std::span<const std::uint16_t> s =
        std::as_const(src).span<PixelFormat::Rgba16fLinearPremul>();
    REQUIRE(d[0] == s[0]);
    REQUIRE(d[1] == s[1]);
    REQUIRE(d[2] == s[2]);
    REQUIRE(d[3] == s[3]);
  }

  SECTION("premultiplied source-over matches the 32f reference within f16 precision") {
    arbc::CpuSurface dst(1, 1, arbc::k_working_rgba16f);
    fill(dst, 0.6F, 0.2F, 0.1F, 1.0F);
    arbc::CpuSurface src(1, 1, arbc::k_working_rgba16f);
    fill(src, 0.3F, 0.15F, 0.05F, 0.5F);
    backend.composite(dst, src, arbc::Affine::identity(), 1.0);

    // out = s + (1 - a_s) d, evaluated in linear working floats.
    const arbc::WorkingPixel reference{0.6F, 0.25F, 0.1F, 1.0F};
    require_close(decode0<PixelFormat::Rgba16fLinearPremul>(dst), reference, 0.005F);
  }
}

TEST_CASE("rgba8 sRGB composite: blend properties hold in linear working light") {
  arbc::CpuBackend backend;

  SECTION("a transparent source is byte-exact identity") {
    arbc::CpuSurface dst(1, 1, arbc::k_fast_rgba8srgb);
    fill(dst, 0.6F, 0.2F, 0.1F, 1.0F);
    const std::span<const std::uint8_t> before = std::as_const(dst).span<PixelFormat::Rgba8Srgb>();
    const std::array<std::uint8_t, 4> saved{before[0], before[1], before[2], before[3]};

    arbc::CpuSurface src(1, 1, arbc::k_fast_rgba8srgb);
    fill(src, 0.0F, 0.0F, 0.0F, 0.0F);
    backend.composite(dst, src, arbc::Affine::identity(), 1.0);

    const std::span<const std::uint8_t> after = std::as_const(dst).span<PixelFormat::Rgba8Srgb>();
    REQUIRE(after[0] == saved[0]);
    REQUIRE(after[1] == saved[1]);
    REQUIRE(after[2] == saved[2]);
    REQUIRE(after[3] == saved[3]);
  }

  SECTION("an opaque source replaces the destination byte-exactly") {
    arbc::CpuSurface dst(1, 1, arbc::k_fast_rgba8srgb);
    fill(dst, 0.6F, 0.2F, 0.1F, 1.0F);
    arbc::CpuSurface src(1, 1, arbc::k_fast_rgba8srgb);
    fill(src, 0.3F, 0.15F, 0.05F, 1.0F);
    backend.composite(dst, src, arbc::Affine::identity(), 1.0);

    const std::span<const std::uint8_t> d = std::as_const(dst).span<PixelFormat::Rgba8Srgb>();
    const std::span<const std::uint8_t> s = std::as_const(src).span<PixelFormat::Rgba8Srgb>();
    REQUIRE(d[0] == s[0]);
    REQUIRE(d[1] == s[1]);
    REQUIRE(d[2] == s[2]);
    REQUIRE(d[3] == s[3]);
  }

  SECTION("premultiplied source-over matches the 32f reference within 8-bit precision") {
    arbc::CpuSurface dst(1, 1, arbc::k_fast_rgba8srgb);
    fill(dst, 0.6F, 0.2F, 0.1F, 1.0F);
    arbc::CpuSurface src(1, 1, arbc::k_fast_rgba8srgb);
    fill(src, 0.3F, 0.15F, 0.05F, 0.5F);
    backend.composite(dst, src, arbc::Affine::identity(), 1.0);

    const arbc::WorkingPixel reference{0.6F, 0.25F, 0.1F, 1.0F};
    require_close(decode0<PixelFormat::Rgba8Srgb>(dst), reference, 0.03F);
  }
}

// enforces: 07-color-and-pixel-formats#blending-in-linear-working-space
TEST_CASE("rgba8 sRGB composite blends in linear light, not gamma space") {
  arbc::CpuBackend backend;
  // Opaque black under a 50%-alpha white, both 8-bit sRGB straight-alpha -- the
  // classic case where a linear-light over and a gamma-space over diverge.
  arbc::CpuSurface dst(1, 1, arbc::k_fast_rgba8srgb);
  {
    const std::span<std::uint8_t> px = dst.span<PixelFormat::Rgba8Srgb>();
    px[0] = 0;
    px[1] = 0;
    px[2] = 0;
    px[3] = 255;
  }
  arbc::CpuSurface src(1, 1, arbc::k_fast_rgba8srgb);
  {
    const std::span<std::uint8_t> px = src.span<PixelFormat::Rgba8Srgb>();
    px[0] = 255;
    px[1] = 255;
    px[2] = 255;
    px[3] = 128;
  }

  backend.composite(dst, src, arbc::Affine::identity(), 1.0);
  const std::span<const std::uint8_t> out = std::as_const(dst).span<PixelFormat::Rgba8Srgb>();

  // The linear-light expectation, derived through the very codec the kernel
  // uses: white decodes to linear 1.0, premultiplied by a = 128/255 over linear
  // black, then re-encoded to straight sRGB (alpha is fully opaque after the
  // over, so unpremultiply is identity here).
  const float a = 128.0F / 255.0F;
  const std::uint8_t linear_expected = arbc::linear_to_srgb8(arbc::srgb8_to_linear(255) * a);
  // The gamma-space blend (the WRONG result) would just lerp the stored sRGB
  // bytes: round(255 * a) ~ 128, far below the linear-light value.
  const auto gamma_wrong = static_cast<std::uint8_t>(std::lround(255.0 * static_cast<double>(a)));

  REQUIRE(out[0] == linear_expected);
  REQUIRE(out[1] == linear_expected);
  REQUIRE(out[2] == linear_expected);
  REQUIRE(out[0] != gamma_wrong);         // decisively not the gamma-space over
  REQUIRE(linear_expected > gamma_wrong); // linear light is markedly brighter
  REQUIRE(out[3] == 255);                 // fully opaque after the over
}

// enforces: 07-color-and-pixel-formats#conversions-route-through-working-space
TEST_CASE("convert routes format -> working -> format (rgba8 <-> rgba32f round-trip)") {
  // An opaque rgba8 sample converted to the linear working space and back is
  // byte-exact: the sRGB codec round-trips every code, so 2N codecs through the
  // working space suffice -- no N*N direct pair is needed.
  arbc::CpuSurface a8(1, 1, arbc::k_fast_rgba8srgb);
  {
    const std::span<std::uint8_t> px = a8.span<PixelFormat::Rgba8Srgb>();
    px[0] = 200;
    px[1] = 100;
    px[2] = 50;
    px[3] = 255;
  }

  arbc::CpuSurface f32(1, 1, arbc::k_working_rgba32f);
  arbc::convert_kernel<PixelFormat::Rgba8Srgb, PixelFormat::Rgba32fLinearPremul>(
      std::as_const(a8).span<PixelFormat::Rgba8Srgb>(),
      arbc::TypedSpan<PixelFormat::Rgba32fLinearPremul>{
          f32.span<PixelFormat::Rgba32fLinearPremul>()},
      1);

  arbc::CpuSurface back(1, 1, arbc::k_fast_rgba8srgb);
  arbc::convert_kernel<PixelFormat::Rgba32fLinearPremul, PixelFormat::Rgba8Srgb>(
      std::as_const(f32).span<PixelFormat::Rgba32fLinearPremul>(),
      arbc::TypedSpan<PixelFormat::Rgba8Srgb>{back.span<PixelFormat::Rgba8Srgb>()}, 1);

  const std::span<const std::uint8_t> out = std::as_const(back).span<PixelFormat::Rgba8Srgb>();
  REQUIRE(out[0] == 200);
  REQUIRE(out[1] == 100);
  REQUIRE(out[2] == 50);
  REQUIRE(out[3] == 255);
}

TEST_CASE("bilinear magnification interpolates in premultiplied linear floats") {
  check_magnification<PixelFormat::Rgba32fLinearPremul>(arbc::k_working_rgba32f, 1e-6F);
  check_magnification<PixelFormat::Rgba16fLinearPremul>(arbc::k_working_rgba16f, 0.005F);
  check_magnification<PixelFormat::Rgba8Srgb>(arbc::k_fast_rgba8srgb, 0.02F);
}

TEST_CASE("a fractional composite offset yields the two-tap average, not a snap") {
  check_fractional_offset<PixelFormat::Rgba32fLinearPremul>(arbc::k_working_rgba32f, 1e-6F);
  check_fractional_offset<PixelFormat::Rgba16fLinearPremul>(arbc::k_working_rgba16f, 0.005F);
  check_fractional_offset<PixelFormat::Rgba8Srgb>(arbc::k_fast_rgba8srgb, 0.02F);
}

// enforces: 16-sdlc-and-quality#byte-exact-goldens
TEST_CASE("integer-aligned composites reduce to the nearest tap byte-for-byte") {
  check_integer_is_byte_exact<PixelFormat::Rgba32fLinearPremul>(arbc::k_working_rgba32f);
  check_integer_is_byte_exact<PixelFormat::Rgba16fLinearPremul>(arbc::k_working_rgba16f);
  check_integer_is_byte_exact<PixelFormat::Rgba8Srgb>(arbc::k_fast_rgba8srgb);
}

TEST_CASE("edge taps blend toward the transparent border, no clamp smear") {
  arbc::CpuBackend backend;
  // Opaque white 2x2 shifted half a texel into a larger destination: a corner
  // pixel straddles the source border, an interior pixel is fully covered, and a
  // far pixel's whole footprint is outside.
  arbc::CpuSurface src(2, 2, arbc::k_working_rgba32f);
  for (int i = 0; i < 4; ++i) {
    write_px<PixelFormat::Rgba32fLinearPremul>(src, i, {1.0F, 1.0F, 1.0F, 1.0F});
  }
  arbc::CpuSurface dst(4, 4, arbc::k_working_rgba32f);

  backend.composite(dst, src, arbc::Affine::translation(0.5, 0.5), 1.0);

  // dst(1,1) sits at texel-space (0.5,0.5): all four taps in range -> fully opaque.
  const arbc::WorkingPixel interior = read_px<PixelFormat::Rgba32fLinearPremul>(dst, 1 * 4 + 1);
  require_close(interior, {1.0F, 1.0F, 1.0F, 1.0F}, 1e-6F);

  // dst(0,0) straddles the corner: only the (0,0) tap is in range with weight
  // 0.25, so the premultiplied sample falls off toward transparent (0.25), never
  // a clamped opaque edge (which would read 1.0).
  const arbc::WorkingPixel corner = read_px<PixelFormat::Rgba32fLinearPremul>(dst, 0);
  require_close(corner, {0.25F, 0.25F, 0.25F, 0.25F}, 1e-6F);
  REQUIRE(corner[3] < 0.9F); // decisively not clamp-to-edge

  // dst(3,3) is fully outside the shifted source: it contributes nothing and the
  // cleared destination stays transparent (matching the old nearest cull).
  const arbc::WorkingPixel outside = read_px<PixelFormat::Rgba32fLinearPremul>(dst, 3 * 4 + 3);
  require_close(outside, {0.0F, 0.0F, 0.0F, 0.0F}, 0.0F);
}

TEST_CASE("box downsample is the four-tap mean in premultiplied linear floats") {
  arbc::CpuBackend backend;

  SECTION("2x2 -> 1x1 equals the mean of the four source taps") {
    arbc::CpuSurface src(2, 2, arbc::k_working_rgba32f);
    write_px<PixelFormat::Rgba32fLinearPremul>(src, 0, {0.1F, 0.2F, 0.3F, 1.0F});
    write_px<PixelFormat::Rgba32fLinearPremul>(src, 1, {0.5F, 0.6F, 0.7F, 1.0F});
    write_px<PixelFormat::Rgba32fLinearPremul>(src, 2, {0.9F, 0.1F, 0.2F, 1.0F});
    write_px<PixelFormat::Rgba32fLinearPremul>(src, 3, {0.3F, 0.4F, 0.5F, 1.0F});
    arbc::CpuSurface dst(1, 1, arbc::k_working_rgba32f);

    backend.downsample(dst, src);

    const arbc::WorkingPixel mean{(0.1F + 0.5F + 0.9F + 0.3F) * 0.25F,
                                  (0.2F + 0.6F + 0.1F + 0.4F) * 0.25F,
                                  (0.3F + 0.7F + 0.2F + 0.5F) * 0.25F, 1.0F};
    require_close(read_px<PixelFormat::Rgba32fLinearPremul>(dst, 0), mean, 1e-6F);
  }

  SECTION("a uniform surface downsamples to the same uniform value") {
    arbc::CpuSurface src(2, 2, arbc::k_working_rgba32f);
    const arbc::WorkingPixel uniform{0.4F, 0.2F, 0.1F, 0.8F};
    for (int i = 0; i < 4; ++i) {
      write_px<PixelFormat::Rgba32fLinearPremul>(src, i, uniform);
    }
    arbc::CpuSurface dst(1, 1, arbc::k_working_rgba32f);

    backend.downsample(dst, src);

    require_close(read_px<PixelFormat::Rgba32fLinearPremul>(dst, 0), uniform, 1e-6F);
  }
}

// enforces: 07-color-and-pixel-formats#resampling-in-linear-premultiplied-space
TEST_CASE("box downsample averages in linear light, not on the sRGB bytes") {
  arbc::CpuBackend backend;
  // A 2x2 sRGB block, half black, half white, all opaque: the linear-light mean
  // of two 0.0 and two 1.0 samples is 0.5, decisively brighter than the naive
  // mean of the stored sRGB bytes (0/255).
  arbc::CpuSurface src(2, 2, arbc::k_fast_rgba8srgb);
  {
    const std::span<std::uint8_t> px = src.span<PixelFormat::Rgba8Srgb>();
    const std::array<std::uint8_t, 4> black{0, 0, 0, 255};
    const std::array<std::uint8_t, 4> white{255, 255, 255, 255};
    for (std::size_t k = 0; k < 4; ++k) {
      px[0 * 4 + k] = black[k]; // (0,0)
      px[1 * 4 + k] = white[k]; // (1,0)
      px[2 * 4 + k] = white[k]; // (0,1)
      px[3 * 4 + k] = black[k]; // (1,1)
    }
  }
  arbc::CpuSurface dst(1, 1, arbc::k_fast_rgba8srgb);

  backend.downsample(dst, src);
  const std::span<const std::uint8_t> out = std::as_const(dst).span<PixelFormat::Rgba8Srgb>();

  const float linear_mean =
      (arbc::srgb8_to_linear(0) + arbc::srgb8_to_linear(255)) * 0.5F; // two of each -> 0.5
  const std::uint8_t linear_expected = arbc::linear_to_srgb8(linear_mean);
  // The gamma-space wrong answer just averages the stored bytes: (0+255)/2.
  const auto gamma_wrong = static_cast<std::uint8_t>(std::lround((0.0 + 255.0) / 2.0));

  REQUIRE(out[0] == linear_expected);
  REQUIRE(out[0] != gamma_wrong);         // decisively not a byte-space average
  REQUIRE(linear_expected > gamma_wrong); // linear light is markedly brighter
  REQUIRE(out[3] == 255);                 // opaque preserved
}

// enforces: 07-color-and-pixel-formats#resampling-in-linear-premultiplied-space
TEST_CASE("the bilinear tap resamples in linear light, not on the sRGB bytes") {
  arbc::CpuBackend backend;
  // A black|white sRGB column pair; a half-texel-shifted composite makes an
  // interior destination pixel the 50/50 blend. In linear light that is 0.5
  // (~188 sRGB), decisively not the gamma-space byte average (~128).
  arbc::CpuSurface src(2, 2, arbc::k_fast_rgba8srgb);
  {
    const std::span<std::uint8_t> px = src.span<PixelFormat::Rgba8Srgb>();
    const std::array<std::uint8_t, 4> black{0, 0, 0, 255};
    const std::array<std::uint8_t, 4> white{255, 255, 255, 255};
    for (std::size_t k = 0; k < 4; ++k) {
      px[0 * 4 + k] = black[k]; // (0,0)
      px[1 * 4 + k] = white[k]; // (1,0)
      px[2 * 4 + k] = black[k]; // (0,1)
      px[3 * 4 + k] = white[k]; // (1,1)
    }
  }
  arbc::CpuSurface dst(2, 2, arbc::k_fast_rgba8srgb);

  backend.composite(dst, src, arbc::Affine::translation(0.5, 0.5), 1.0);
  const std::span<const std::uint8_t> out = std::as_const(dst).span<PixelFormat::Rgba8Srgb>();

  // dst(1,1) sits at texel-space (0.5,0.5): a 50/50 blend of black and white.
  const std::size_t at = (1 * 2 + 1) * 4;
  const float linear_mean = (arbc::srgb8_to_linear(0) + arbc::srgb8_to_linear(255)) * 0.5F;
  const std::uint8_t linear_expected = arbc::linear_to_srgb8(linear_mean);
  const auto gamma_wrong = static_cast<std::uint8_t>(std::lround((0.0 + 255.0) / 2.0));

  REQUIRE(out[at + 0] == linear_expected);
  REQUIRE(out[at + 0] != gamma_wrong);
  REQUIRE(linear_expected > gamma_wrong);
  REQUIRE(out[at + 3] == 255); // opaque after the blend of two opaque taps
}

// --- the clip-scoped operations (doc 09) ------------------------------------

namespace {

// Every pixel of a 32f surface, as raw floats: the byte-exact comparison the
// clip tests are made of ("wrote no pixel outside the clip" is a statement about
// *bytes*, not about a tolerance).
std::vector<float> pixels(const arbc::Surface& surface) {
  const std::span<const float> px = std::as_const(surface).span<PixelFormat::Rgba32fLinearPremul>();
  return {px.begin(), px.end()};
}

// Pre-paint a 32f surface opaque red, so any pixel a clipped operation touches is
// unmistakable against what it must leave alone. (`Surface` is neither copyable
// nor movable, so this paints in place rather than handing one back.)
void paint_red(arbc::Surface& surface) {
  arbc::CpuBackend backend;
  backend.clear(surface, 1.0F, 0.0F, 0.0F, 1.0F);
}

} // namespace

// enforces: 09-surfaces-and-backends#clip-scoped-ops-honor-the-clip
TEST_CASE("clear_rect writes no pixel outside its clip") {
  arbc::CpuBackend backend;
  arbc::CpuSurface dst(4, 4, arbc::k_working_rgba32f);
  paint_red(dst);
  const std::vector<float> before = pixels(dst);

  // Clear the interior 2x2 to transparent; the surrounding ring must survive.
  backend.clear_rect(dst, arbc::Rect{1.0, 1.0, 3.0, 3.0}, 0.0F, 0.0F, 0.0F, 0.0F);

  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 4; ++x) {
      const int idx = y * 4 + x;
      const bool inside = x >= 1 && x < 3 && y >= 1 && y < 3;
      CAPTURE(x, y);
      const arbc::WorkingPixel got = read_px<PixelFormat::Rgba32fLinearPremul>(dst, idx);
      if (inside) {
        REQUIRE(got == arbc::WorkingPixel{0.0F, 0.0F, 0.0F, 0.0F});
      } else {
        // Byte-identical to what was there before: the clip is a scissor, not a
        // hint.
        for (std::size_t k = 0; k < 4; ++k) {
          REQUIRE(got[k] == before[static_cast<std::size_t>(idx) * 4 + k]);
        }
      }
    }
  }
}

// enforces: 09-surfaces-and-backends#clip-scoped-ops-honor-the-clip
TEST_CASE("composite_clipped writes no pixel outside its clip") {
  arbc::CpuBackend backend;
  arbc::CpuSurface dst(4, 4, arbc::k_working_rgba32f);
  paint_red(dst);
  const std::vector<float> before = pixels(dst);

  // An opaque green source covering the WHOLE destination: unclipped it would
  // replace every pixel, so anything red left standing is the clip doing its job.
  arbc::CpuSurface src(4, 4, arbc::k_working_rgba32f);
  backend.clear(src, 0.0F, 1.0F, 0.0F, 1.0F);

  backend.composite_clipped(dst, src, arbc::Affine::identity(), 1.0,
                            arbc::Rect{1.0, 1.0, 3.0, 3.0});

  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 4; ++x) {
      const int idx = y * 4 + x;
      const bool inside = x >= 1 && x < 3 && y >= 1 && y < 3;
      CAPTURE(x, y);
      const arbc::WorkingPixel got = read_px<PixelFormat::Rgba32fLinearPremul>(dst, idx);
      if (inside) {
        REQUIRE(got == arbc::WorkingPixel{0.0F, 1.0F, 0.0F, 1.0F}); // opaque green
      } else {
        for (std::size_t k = 0; k < 4; ++k) {
          REQUIRE(got[k] == before[static_cast<std::size_t>(idx) * 4 + k]); // still red
        }
      }
    }
  }
}

// enforces: 09-surfaces-and-backends#clip-scoped-ops-honor-the-clip
TEST_CASE("a clip is intersected with the destination bounds") {
  arbc::CpuBackend backend;
  arbc::CpuSurface dst(4, 4, arbc::k_working_rgba32f);
  paint_red(dst);

  // A clip reaching far past every edge (and the whole-plane clip) is LEGAL, not
  // an error: it saturates to the destination. Both must reproduce the unclipped
  // clear byte-for-byte.
  arbc::CpuSurface reference(4, 4, arbc::k_working_rgba32f);
  paint_red(reference);
  backend.clear(reference, 0.0F, 0.0F, 1.0F, 1.0F);

  backend.clear_rect(dst, arbc::Rect{-100.0, -100.0, 100.0, 100.0}, 0.0F, 0.0F, 1.0F, 1.0F);
  REQUIRE(pixels(dst) == pixels(reference));

  arbc::CpuSurface infinite_clipped(4, 4, arbc::k_working_rgba32f);
  paint_red(infinite_clipped);
  backend.clear_rect(infinite_clipped, arbc::Rect::infinite(), 0.0F, 0.0F, 1.0F, 1.0F);
  REQUIRE(pixels(infinite_clipped) == pixels(reference));

  // A clip entirely outside the destination intersects to nothing: a no-op.
  arbc::CpuSurface outside(4, 4, arbc::k_working_rgba32f);
  paint_red(outside);
  const std::vector<float> untouched = pixels(outside);
  backend.clear_rect(outside, arbc::Rect{10.0, 10.0, 20.0, 20.0}, 0.0F, 0.0F, 1.0F, 1.0F);
  REQUIRE(pixels(outside) == untouched);
}

// enforces: 09-surfaces-and-backends#clip-scoped-ops-honor-the-clip
TEST_CASE("an empty clip is a no-op for both clip-scoped operations") {
  arbc::CpuBackend backend;
  arbc::CpuSurface src(4, 4, arbc::k_working_rgba32f);
  backend.clear(src, 0.0F, 1.0F, 0.0F, 1.0F);

  arbc::CpuSurface dst(4, 4, arbc::k_working_rgba32f);
  paint_red(dst);
  const std::vector<float> before = pixels(dst);

  // Degenerate (zero-area) and inverted rects are both `Rect::empty()`.
  const arbc::Rect degenerate{2.0, 2.0, 2.0, 2.0};
  const arbc::Rect inverted{3.0, 3.0, 1.0, 1.0};

  backend.clear_rect(dst, degenerate, 0.0F, 0.0F, 0.0F, 0.0F);
  REQUIRE(pixels(dst) == before);
  backend.clear_rect(dst, inverted, 0.0F, 0.0F, 0.0F, 0.0F);
  REQUIRE(pixels(dst) == before);
  backend.clear_rect(dst, arbc::Rect{}, 0.0F, 0.0F, 0.0F, 0.0F);
  REQUIRE(pixels(dst) == before);

  backend.composite_clipped(dst, src, arbc::Affine::identity(), 1.0, degenerate);
  REQUIRE(pixels(dst) == before);
  backend.composite_clipped(dst, src, arbc::Affine::identity(), 1.0, inverted);
  REQUIRE(pixels(dst) == before);
  backend.composite_clipped(dst, src, arbc::Affine::identity(), 1.0, arbc::Rect{});
  REQUIRE(pixels(dst) == before);
}

// enforces: 09-surfaces-and-backends#clip-scoped-ops-honor-the-clip
TEST_CASE("a whole-destination clip is byte-identical to the unclipped operation") {
  arbc::CpuBackend backend;
  const arbc::Rect whole = arbc::Rect::from_size(4.0, 4.0);

  // This is not a nicety -- it is how the unclipped ops are DEFINED (doc 09), so
  // the backend carries one kernel per operation rather than two. A drift here is
  // a second kernel that has started to disagree with the first.
  arbc::CpuSurface clipped_clear(4, 4, arbc::k_working_rgba32f);
  paint_red(clipped_clear);
  arbc::CpuSurface plain_clear(4, 4, arbc::k_working_rgba32f);
  paint_red(plain_clear);
  backend.clear_rect(clipped_clear, whole, 0.25F, 0.5F, 0.75F, 1.0F);
  backend.clear(plain_clear, 0.25F, 0.5F, 0.75F, 1.0F);
  REQUIRE(pixels(clipped_clear) == pixels(plain_clear));

  // A translucent source at a FRACTIONAL offset, so the bilinear tap is live: the
  // clipped walk must resolve each destination pixel to the same sample the
  // unclipped walk does -- the sample position may not depend on the clip.
  arbc::CpuSurface src(4, 4, arbc::k_working_rgba32f);
  fill_red_ramp_2x2<PixelFormat::Rgba32fLinearPremul>(src);
  write_px<PixelFormat::Rgba32fLinearPremul>(src, 5, {0.2F, 0.1F, 0.05F, 0.4F});

  arbc::CpuSurface clipped_composite(4, 4, arbc::k_working_rgba32f);
  paint_red(clipped_composite);
  arbc::CpuSurface plain_composite(4, 4, arbc::k_working_rgba32f);
  paint_red(plain_composite);
  const arbc::Affine placement = arbc::Affine::translation(0.5, 0.5);
  backend.composite_clipped(clipped_composite, src, placement, 0.6, whole);
  backend.composite(plain_composite, src, placement, 0.6);
  REQUIRE(pixels(clipped_composite) == pixels(plain_composite));
}

// enforces: 09-surfaces-and-backends#clip-scoped-ops-honor-the-clip
TEST_CASE("a clip is rounded OUT to whole device pixels") {
  arbc::CpuBackend backend;
  arbc::CpuSurface dst(4, 4, arbc::k_working_rgba32f);
  paint_red(dst);

  // Device dirty rects are `map_rect` outputs -- arbitrary doubles. A pixel whose
  // cell the clip touches at all is IN: rounding in would leave a sub-pixel fringe
  // of the repaint region unpainted (a stale seam), and the compositor gates its
  // plan on the same rounded rect, so the extra pixels are covered.
  backend.clear_rect(dst, arbc::Rect{0.5, 0.5, 2.5, 2.5}, 0.0F, 0.0F, 0.0F, 0.0F);

  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 4; ++x) {
      CAPTURE(x, y);
      const bool inside = x < 3 && y < 3; // floor(0.5)=0 .. ceil(2.5)=3
      const arbc::WorkingPixel got = read_px<PixelFormat::Rgba32fLinearPremul>(dst, y * 4 + x);
      REQUIRE(got == (inside ? arbc::WorkingPixel{0.0F, 0.0F, 0.0F, 0.0F}
                             : arbc::WorkingPixel{1.0F, 0.0F, 0.0F, 1.0F}));
    }
  }
}

} // namespace
