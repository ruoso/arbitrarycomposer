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

} // namespace
