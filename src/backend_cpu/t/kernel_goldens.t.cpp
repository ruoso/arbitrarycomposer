#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/media/pixel_format.hpp>
#include <arbc/media/pixel_traits.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/surface/typed_span.hpp>

#include <catch2/catch_test_macros.hpp>

#include "../kernels.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <span>
#include <utility>
#include <vector>

// Byte-exact reference-vector golden suite for the color kernels and codecs
// (color.kernel_goldens; doc 16:47-53 tier-3 "Golden rendering tests" applied
// at kernel granularity). For each supported PixelFormat and each kernel family
// (fill, source-over/bilinear, box 2:1 downsample, cross-format convert) and for
// the underlying pixel_traits codecs, a fixed representative input is driven
// through the *real* kernel/codec and its output is asserted equal to a frozen
// expected table with NO tolerance -- raw storage bytes (memcmp) for the
// kernels, exact bit patterns for the float-valued codecs. This pins the
// absolute output the predecessors' property/round-trip tests leave free: a
// codec-constant, tap-weight, reduction-order, or FP-flag drift that perturbs a
// single output byte becomes a diff (doc 16 determinism contract, 16:47-49).
//
// The software f16 codec is deterministic and portable (kernels.md decision:
// software f16 over _Float16 precisely so output bytes agree bit-for-bit across
// the three compilers), so Rgba16fLinearPremul is byte-exact here too. The
// 1-ULP f16 figure (kernels.md:86) is an accuracy-vs-ideal bound, not a
// reproducibility one, and is deliberately NOT a tolerance in this suite.
//
// enforces: 16-sdlc-and-quality#byte-exact-goldens
// enforces: 07-color-and-pixel-formats#kernels-byte-exact-per-format
//
// REGENERATE PROCEDURE (doc 16 tier-3 "regenerate with an audited script"):
// an intended kernel/codec change deliberately re-freezes these tables; they
// never regenerate silently. Build this target and run only the hidden dump
// case, which prints paste-ready literals for every frozen table:
//
//     cmake --build --preset dev --target arbc_backend_cpu_t
//     ./build/dev/src/backend_cpu/arbc_backend_cpu_t "[regen]"
//
// then replace the "FROZEN EXPECTED TABLES" block below with its output. The
// dump case is hidden (`[.regen]`) so it never runs in the default suite; it is
// GCOV-excluded because it is a maintenance tool, not a covered assertion (the
// kernels it would dump are already covered by the golden cases, which drive the
// identical builder functions).

namespace {

using arbc::PixelFormat;

// --- byte / bit helpers -----------------------------------------------------

std::uint32_t f32_bits(float f) {
  std::uint32_t b = 0;
  std::memcpy(&b, &f, sizeof b);
  return b;
}

// Copy a surface's raw storage bytes out for a byte-exact compare against a
// frozen table (the surface itself is not movable -- Surface deletes copy).
std::vector<std::byte> bytes_of(const arbc::Surface& s) {
  const std::span<const std::byte> b = s.cpu_bytes();
  return {b.begin(), b.end()};
}

void require_bytes(const std::vector<std::byte>& got, std::span<const unsigned char> want) {
  REQUIRE(got.size() == want.size());
  REQUIRE(std::memcmp(got.data(), want.data(), want.size()) == 0);
}

// Encode a fixed working-space sample into pixel `idx` -- deterministic input
// setup through the format's own encode codec (idx is row-major, y*width + x).
template <PixelFormat F> void wr(arbc::Surface& s, int idx, const arbc::WorkingPixel& w) {
  const std::span<typename arbc::PixelTraits<F>::Storage> px = s.span<F>();
  arbc::PixelTraits<F>::encode(w, &px[static_cast<std::size_t>(idx) * 4]);
}

// --- fixed representative inputs (no RNG, no clock; committed constants) -----

// Fill: one valid premultiplied (rgb <= a) non-opaque working color.
constexpr arbc::WorkingPixel kFillColor{0.1F, 0.2F, 0.3F, 0.5F};

// Source-over integer-aligned: half-alpha src over an opaque background, so the
// nearest-tap blend `out = s + (1 - a_s) d` is genuinely exercised (not a copy).
constexpr std::array<arbc::WorkingPixel, 4> kSoDst{{{0.5F, 0.1F, 0.1F, 1.0F},
                                                    {0.1F, 0.5F, 0.1F, 1.0F},
                                                    {0.1F, 0.1F, 0.5F, 1.0F},
                                                    {0.4F, 0.4F, 0.1F, 1.0F}}};
constexpr std::array<arbc::WorkingPixel, 4> kSoSrc{{{0.15F, 0.30F, 0.10F, 0.5F},
                                                    {0.20F, 0.10F, 0.25F, 0.5F},
                                                    {0.05F, 0.20F, 0.30F, 0.5F},
                                                    {0.25F, 0.10F, 0.15F, 0.5F}}};

// Source-over fractional: an opaque red x-ramp on a 2x2 source, half-texel
// shifted, so an interior destination pixel is the two-tap bilinear mean and an
// edge pixel falls off toward the transparent border.
constexpr std::array<arbc::WorkingPixel, 4> kRamp{{{0.0F, 0.0F, 0.0F, 1.0F},
                                                   {1.0F, 0.0F, 0.0F, 1.0F},
                                                   {0.0F, 0.0F, 0.0F, 1.0F},
                                                   {1.0F, 0.0F, 0.0F, 1.0F}}};

// Downsample: four distinct opaque colors, so the 2:1 box mean is non-trivial.
constexpr std::array<arbc::WorkingPixel, 4> kDown{{{0.1F, 0.2F, 0.3F, 1.0F},
                                                   {0.5F, 0.6F, 0.7F, 1.0F},
                                                   {0.9F, 0.1F, 0.2F, 1.0F},
                                                   {0.3F, 0.4F, 0.5F, 1.0F}}};

// Convert: one opaque and one non-opaque (straight->premultiplied) sample, so
// every directed pair exercises a distinct src-decode/dst-encode combination.
constexpr arbc::WorkingPixel kConvA{0.20F, 0.40F, 0.60F, 1.0F};
constexpr arbc::WorkingPixel kConvB{0.30F, 0.15F, 0.05F, 0.6F};

// --- kernel output builders (the real shipped kernels / CpuBackend path) -----

template <PixelFormat F> std::vector<std::byte> fill_bytes(arbc::SurfaceFormat fmt) {
  arbc::CpuSurface dst(2, 1, fmt);
  arbc::fill_kernel<F>(arbc::TypedSpan<F>{dst.span<F>()}, kFillColor);
  return bytes_of(dst);
}

template <PixelFormat F> std::vector<std::byte> src_over_integer_bytes(arbc::SurfaceFormat fmt) {
  arbc::CpuBackend backend;
  arbc::CpuSurface src(2, 2, fmt);
  arbc::CpuSurface dst(2, 2, fmt);
  for (int i = 0; i < 4; ++i) {
    wr<F>(src, i, kSoSrc[static_cast<std::size_t>(i)]);
    wr<F>(dst, i, kSoDst[static_cast<std::size_t>(i)]);
  }
  backend.composite(dst, src, arbc::Affine::identity(), 1.0);
  return bytes_of(dst);
}

template <PixelFormat F> std::vector<std::byte> src_over_fractional_bytes(arbc::SurfaceFormat fmt) {
  arbc::CpuBackend backend;
  arbc::CpuSurface src(2, 2, fmt);
  for (int i = 0; i < 4; ++i) {
    wr<F>(src, i, kRamp[static_cast<std::size_t>(i)]);
  }
  arbc::CpuSurface dst(2, 2, fmt); // fresh: transparent zero
  backend.composite(dst, src, arbc::Affine::translation(0.5, 0.0), 1.0);
  return bytes_of(dst);
}

template <PixelFormat F> std::vector<std::byte> downsample_bytes(arbc::SurfaceFormat fmt) {
  arbc::CpuBackend backend;
  arbc::CpuSurface src(2, 2, fmt);
  for (int i = 0; i < 4; ++i) {
    wr<F>(src, i, kDown[static_cast<std::size_t>(i)]);
  }
  arbc::CpuSurface dst(1, 1, fmt);
  backend.downsample(dst, src);
  return bytes_of(dst);
}

template <PixelFormat SrcF, PixelFormat DstF>
std::vector<std::byte> convert_bytes(arbc::SurfaceFormat src_fmt, arbc::SurfaceFormat dst_fmt) {
  arbc::CpuSurface src(2, 1, src_fmt);
  wr<SrcF>(src, 0, kConvA);
  wr<SrcF>(src, 1, kConvB);
  arbc::CpuSurface dst(2, 1, dst_fmt);
  arbc::convert_kernel<SrcF, DstF>(std::as_const(src).span<SrcF>(),
                                   arbc::TypedSpan<DstF>{dst.span<DstF>()}, 2);
  return bytes_of(dst);
}

// The SURFACE-level operation over the SAME fixed input. `CpuBackend::convert` is
// the L2 `Backend` seam the nesting boundary (doc 07 rule 4) and the later import
// / display-out edges call; it must be a faithful wrapper of `convert_kernel`, not
// a second, divergent implementation -- so it is asserted against the very same
// frozen tables below, and adds no new regeneration surface.
template <PixelFormat SrcF, PixelFormat DstF>
std::vector<std::byte> backend_convert_bytes(arbc::SurfaceFormat src_fmt,
                                             arbc::SurfaceFormat dst_fmt) {
  arbc::CpuBackend backend;
  arbc::CpuSurface src(2, 1, src_fmt);
  wr<SrcF>(src, 0, kConvA);
  wr<SrcF>(src, 1, kConvB);
  // Pre-dirty the destination: `convert` REPLACES every destination pixel, so a
  // partial write must not be able to hide behind a zero-initialized buffer.
  arbc::CpuSurface dst(2, 1, dst_fmt);
  backend.clear(dst, 0.9F, 0.1F, 0.7F, 1.0F);
  backend.convert(dst, src);
  return bytes_of(dst);
}

// A 256-pixel rgba8-sRGB sweep written as RAW STORAGE bytes rather than through
// the encode codec, so it covers straight-alpha samples a decode/encode
// round-trip cannot reproduce -- notably alpha 0 carrying nonzero chroma (i == 0
// below), which unpremultiply destroys. Deterministic: a fixed affine sweep over
// the code space, no RNG and no clock (doc 16).
void write_srgb8_sweep(arbc::Surface& s) {
  const std::span<std::uint8_t> px = s.span<PixelFormat::Rgba8Srgb>();
  for (std::size_t i = 0; i < 256; ++i) {
    px[i * 4 + 0] = static_cast<std::uint8_t>((i * 7 + 31) % 256);
    px[i * 4 + 1] = static_cast<std::uint8_t>((i * 13 + 17) % 256);
    px[i * 4 + 2] = static_cast<std::uint8_t>((i * 29 + 3) % 256);
    px[i * 4 + 3] = static_cast<std::uint8_t>(i);
  }
}

// One equal-tag `convert` over the two fixed convert samples: the destination is
// pre-dirtied, so the returned bytes are the copy the operation actually made.
template <PixelFormat F>
std::pair<std::vector<std::byte>, std::vector<std::byte>>
identity_convert_bytes(arbc::SurfaceFormat fmt) {
  arbc::CpuBackend backend;
  arbc::CpuSurface src(2, 1, fmt);
  wr<F>(src, 0, kConvA);
  wr<F>(src, 1, kConvB);
  arbc::CpuSurface dst(2, 1, fmt);
  backend.clear(dst, 0.9F, 0.1F, 0.7F, 1.0F);
  backend.convert(dst, src);
  return {bytes_of(dst), bytes_of(src)};
}

// ===========================================================================
// FROZEN EXPECTED TABLES -- regenerate deliberately (see procedure at top).
// ===========================================================================

// fill_kernel, 2x1 tile of kFillColor, per format.
constexpr std::array<unsigned char, 32> kFillExp32{
    {0xCD, 0xCC, 0xCC, 0x3D, 0xCD, 0xCC, 0x4C, 0x3E, 0x9A, 0x99, 0x99,
     0x3E, 0x00, 0x00, 0x00, 0x3F, 0xCD, 0xCC, 0xCC, 0x3D, 0xCD, 0xCC,
     0x4C, 0x3E, 0x9A, 0x99, 0x99, 0x3E, 0x00, 0x00, 0x00, 0x3F}};
constexpr std::array<unsigned char, 16> kFillExp16{{0x66, 0x2E, 0x66, 0x32, 0xCD, 0x34, 0x00, 0x38,
                                                    0x66, 0x2E, 0x66, 0x32, 0xCD, 0x34, 0x00,
                                                    0x38}};
constexpr std::array<unsigned char, 8> kFillExp8{{0x7C, 0xAA, 0xCB, 0x80, 0x7C, 0xAA, 0xCB, 0x80}};

// source_over_kernel integer-aligned (identity), 2x2, per format.
constexpr std::array<unsigned char, 64> kSoIntExp32{
    {0xCD, 0xCC, 0xCC, 0x3E, 0x34, 0x33, 0xB3, 0x3E, 0x9A, 0x99, 0x19, 0x3E, 0x00,
     0x00, 0x80, 0x3F, 0x00, 0x00, 0x80, 0x3E, 0x33, 0x33, 0xB3, 0x3E, 0x9A, 0x99,
     0x99, 0x3E, 0x00, 0x00, 0x80, 0x3F, 0xCD, 0xCC, 0xCC, 0x3D, 0x00, 0x00, 0x80,
     0x3E, 0xCD, 0xCC, 0x0C, 0x3F, 0x00, 0x00, 0x80, 0x3F, 0x66, 0x66, 0xE6, 0x3E,
     0x9A, 0x99, 0x99, 0x3E, 0xCD, 0xCC, 0x4C, 0x3E, 0x00, 0x00, 0x80, 0x3F}};
constexpr std::array<unsigned char, 32> kSoIntExp16{
    {0x66, 0x36, 0x9A, 0x35, 0xCC, 0x30, 0x00, 0x3C, 0x00, 0x34, 0x9A,
     0x35, 0xCD, 0x34, 0x00, 0x3C, 0x66, 0x2E, 0x00, 0x34, 0x66, 0x38,
     0x00, 0x3C, 0x33, 0x37, 0xCC, 0x34, 0x66, 0x32, 0x00, 0x3C}};
constexpr std::array<unsigned char, 16> kSoIntExp8{{0xAA, 0xA0, 0x6C, 0xFF, 0x89, 0xA0, 0x95, 0xFF,
                                                    0x59, 0x89, 0xC4, 0xFF, 0xB3, 0x95, 0x7C,
                                                    0xFF}};

// source_over_kernel fractional (translation 0.5,0), 2x2, per format.
constexpr std::array<unsigned char, 64> kSoFracExp32{
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x3F, 0x00, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x00, 0x3F,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3F}};
constexpr std::array<unsigned char, 32> kSoFracExp16{
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x38, 0x00, 0x38, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
     0x00, 0x38, 0x00, 0x38, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3C}};
constexpr std::array<unsigned char, 16> kSoFracExp8{{0x00, 0x00, 0x00, 0x80, 0xBC, 0x00, 0x00, 0xFF,
                                                     0x00, 0x00, 0x00, 0x80, 0xBC, 0x00, 0x00,
                                                     0xFF}};

// downsample_box_kernel 2x2 -> 1x1, per format.
constexpr std::array<unsigned char, 16> kDownExp32{{0x66, 0x66, 0xE6, 0x3E, 0x67, 0x66, 0xA6, 0x3E,
                                                    0x9A, 0x99, 0xD9, 0x3E, 0x00, 0x00, 0x80,
                                                    0x3F}};
constexpr std::array<unsigned char, 8> kDownExp16{{0x33, 0x37, 0x33, 0x35, 0xCD, 0x36, 0x00, 0x3C}};
constexpr std::array<unsigned char, 4> kDownExp8{{0xB3, 0x9A, 0xAF, 0xFF}};

// convert_kernel, 2 px, the six directed format pairs (output in DstF).
constexpr std::array<unsigned char, 16> kConv_32_16{{0x66, 0x32, 0x66, 0x36, 0xCD, 0x38, 0x00, 0x3C,
                                                     0xCD, 0x34, 0xCD, 0x30, 0x66, 0x2A, 0xCD,
                                                     0x38}};
constexpr std::array<unsigned char, 8> kConv_32_8{{0x7C, 0xAA, 0xCB, 0xFF, 0xBC, 0x89, 0x52, 0x99}};
constexpr std::array<unsigned char, 32> kConv_16_32{
    {0x00, 0xC0, 0x4C, 0x3E, 0x00, 0xC0, 0xCC, 0x3E, 0x00, 0xA0, 0x19,
     0x3F, 0x00, 0x00, 0x80, 0x3F, 0x00, 0xA0, 0x99, 0x3E, 0x00, 0xA0,
     0x19, 0x3E, 0x00, 0xC0, 0x4C, 0x3D, 0x00, 0xA0, 0x19, 0x3F}};
constexpr std::array<unsigned char, 8> kConv_16_8{{0x7C, 0xAA, 0xCB, 0xFF, 0xBC, 0x89, 0x51, 0x99}};
constexpr std::array<unsigned char, 32> kConv_8_32{
    {0xC2, 0x64, 0x4E, 0x3E, 0x0B, 0xD0, 0xCD, 0x3E, 0x39, 0xE2, 0x18,
     0x3F, 0x00, 0x00, 0x80, 0x3F, 0x9D, 0x7C, 0x9A, 0x3E, 0x83, 0xB2,
     0x19, 0x3E, 0xED, 0x5C, 0x4F, 0x3D, 0x9A, 0x99, 0x19, 0x3F}};
constexpr std::array<unsigned char, 16> kConv_8_16{{0x73, 0x32, 0x6F, 0x36, 0xC7, 0x38, 0x00, 0x3C,
                                                    0xD4, 0x34, 0xCE, 0x30, 0x7B, 0x2A, 0xCD,
                                                    0x38}};

// Codec reference vectors (exact bit patterns / codes).
constexpr std::array<std::uint32_t, 4> kSrgb8ToLinear{
    {0x0, 0x399F22B4, 0x3E5D0A8B, 0x3F800000}}; // codes 0,1,128,255
constexpr std::array<unsigned char, 4> kLinearToSrgb8{
    {0x0, 0x8, 0xBC, 0xFF}}; // inputs 0.0,0.0025,0.5,1.0
constexpr std::array<unsigned char, 3> kUnorm8Encode{{0x0, 0x80, 0xFF}}; // inputs 0.0,0.5,1.0
constexpr std::array<std::uint32_t, 3> kUnorm8Decode{
    {0x0, 0x3F008081, 0x3F800000}}; // codes 0,128,255
// halves 0x0000,0x8000(-0),0x3800(0.5),0x3C00(1.0),0x0001(subnormal),0x7C00(inf)
constexpr std::array<std::uint32_t, 6> kF16ToFloat{
    {0x0, 0x80000000, 0x3F000000, 0x3F800000, 0x33800000, 0x7F800000}};
// values +0,-0,0.5,1.0,2^-24 subnormal,inf
constexpr std::array<std::uint16_t, 6> kF16FromFloat{{0x0, 0x8000, 0x3800, 0x3C00, 0x1, 0x7C00}};
constexpr std::array<std::uint32_t, 4> kPremultiply{
    {0x3EF5C290, 0x3E75C290, 0x3DF5C290, 0x3F19999A}}; // straight {0.8,0.4,0.2,0.6}
constexpr std::array<std::uint32_t, 4> kUnpremultiply{
    {0x3F4CCCCC, 0x3ECCCCCC, 0x3E4CCCCC, 0x3F19999A}}; // premul  {0.48,0.24,0.12,0.6}

// ===========================================================================
// END FROZEN EXPECTED TABLES
// ===========================================================================

// Codec input landmarks (shared by the golden and the regen dump).
constexpr arbc::WorkingPixel kPremulStraight{0.8F, 0.4F, 0.2F, 0.6F};
constexpr arbc::WorkingPixel kUnpremulIn{0.48F, 0.24F, 0.12F, 0.6F};
const float kInf = std::numeric_limits<float>::infinity();
constexpr float kSubnormal = 5.9604644775390625e-08F; // 2^-24, smallest f16 subnormal

} // namespace

// --- per-format kernel goldens ----------------------------------------------

// enforces: 07-color-and-pixel-formats#kernels-byte-exact-per-format
TEST_CASE("fill_kernel is byte-exact per format") {
  require_bytes(fill_bytes<PixelFormat::Rgba32fLinearPremul>(arbc::k_working_rgba32f), kFillExp32);
  require_bytes(fill_bytes<PixelFormat::Rgba16fLinearPremul>(arbc::k_working_rgba16f), kFillExp16);
  require_bytes(fill_bytes<PixelFormat::Rgba8Srgb>(arbc::k_fast_rgba8srgb), kFillExp8);
}

// enforces: 07-color-and-pixel-formats#premultiplied-source-over
// enforces: 07-color-and-pixel-formats#blending-in-linear-working-space
TEST_CASE("source_over_kernel integer-aligned blend is byte-exact per format") {
  require_bytes(src_over_integer_bytes<PixelFormat::Rgba32fLinearPremul>(arbc::k_working_rgba32f),
                kSoIntExp32);
  require_bytes(src_over_integer_bytes<PixelFormat::Rgba16fLinearPremul>(arbc::k_working_rgba16f),
                kSoIntExp16);
  require_bytes(src_over_integer_bytes<PixelFormat::Rgba8Srgb>(arbc::k_fast_rgba8srgb), kSoIntExp8);
}

// enforces: 07-color-and-pixel-formats#resampling-in-linear-premultiplied-space
TEST_CASE("source_over_kernel fractional bilinear tap is byte-exact per format") {
  require_bytes(
      src_over_fractional_bytes<PixelFormat::Rgba32fLinearPremul>(arbc::k_working_rgba32f),
      kSoFracExp32);
  require_bytes(
      src_over_fractional_bytes<PixelFormat::Rgba16fLinearPremul>(arbc::k_working_rgba16f),
      kSoFracExp16);
  require_bytes(src_over_fractional_bytes<PixelFormat::Rgba8Srgb>(arbc::k_fast_rgba8srgb),
                kSoFracExp8);
}

// enforces: 07-color-and-pixel-formats#resampling-in-linear-premultiplied-space
TEST_CASE("downsample_box_kernel 2:1 mean is byte-exact per format") {
  require_bytes(downsample_bytes<PixelFormat::Rgba32fLinearPremul>(arbc::k_working_rgba32f),
                kDownExp32);
  require_bytes(downsample_bytes<PixelFormat::Rgba16fLinearPremul>(arbc::k_working_rgba16f),
                kDownExp16);
  require_bytes(downsample_bytes<PixelFormat::Rgba8Srgb>(arbc::k_fast_rgba8srgb), kDownExp8);
}

// enforces: 07-color-and-pixel-formats#conversions-route-through-working-space
TEST_CASE("convert_kernel is byte-exact for every directed format pair") {
  require_bytes(convert_bytes<PixelFormat::Rgba32fLinearPremul, PixelFormat::Rgba16fLinearPremul>(
                    arbc::k_working_rgba32f, arbc::k_working_rgba16f),
                kConv_32_16);
  require_bytes(convert_bytes<PixelFormat::Rgba32fLinearPremul, PixelFormat::Rgba8Srgb>(
                    arbc::k_working_rgba32f, arbc::k_fast_rgba8srgb),
                kConv_32_8);
  require_bytes(convert_bytes<PixelFormat::Rgba16fLinearPremul, PixelFormat::Rgba32fLinearPremul>(
                    arbc::k_working_rgba16f, arbc::k_working_rgba32f),
                kConv_16_32);
  require_bytes(convert_bytes<PixelFormat::Rgba16fLinearPremul, PixelFormat::Rgba8Srgb>(
                    arbc::k_working_rgba16f, arbc::k_fast_rgba8srgb),
                kConv_16_8);
  require_bytes(convert_bytes<PixelFormat::Rgba8Srgb, PixelFormat::Rgba32fLinearPremul>(
                    arbc::k_fast_rgba8srgb, arbc::k_working_rgba32f),
                kConv_8_32);
  require_bytes(convert_bytes<PixelFormat::Rgba8Srgb, PixelFormat::Rgba16fLinearPremul>(
                    arbc::k_fast_rgba8srgb, arbc::k_working_rgba16f),
                kConv_8_16);
}

// The L2 seam over the same six directed pairs, asserted against the SAME frozen
// tables: `Backend::convert` routes to `convert_kernel` and is not a divergent
// second implementation. No new table, so no new regeneration surface.
// enforces: 07-color-and-pixel-formats#conversions-route-through-working-space
// enforces: 07-color-and-pixel-formats#kernels-byte-exact-per-format
// enforces: 09-surfaces-and-backends#convert-is-same-geometry-replace
TEST_CASE("Backend::convert is byte-exact for every directed cross-format pair") {
  require_bytes(
      backend_convert_bytes<PixelFormat::Rgba32fLinearPremul, PixelFormat::Rgba16fLinearPremul>(
          arbc::k_working_rgba32f, arbc::k_working_rgba16f),
      kConv_32_16);
  require_bytes(backend_convert_bytes<PixelFormat::Rgba32fLinearPremul, PixelFormat::Rgba8Srgb>(
                    arbc::k_working_rgba32f, arbc::k_fast_rgba8srgb),
                kConv_32_8);
  require_bytes(
      backend_convert_bytes<PixelFormat::Rgba16fLinearPremul, PixelFormat::Rgba32fLinearPremul>(
          arbc::k_working_rgba16f, arbc::k_working_rgba32f),
      kConv_16_32);
  require_bytes(backend_convert_bytes<PixelFormat::Rgba16fLinearPremul, PixelFormat::Rgba8Srgb>(
                    arbc::k_working_rgba16f, arbc::k_fast_rgba8srgb),
                kConv_16_8);
  require_bytes(backend_convert_bytes<PixelFormat::Rgba8Srgb, PixelFormat::Rgba32fLinearPremul>(
                    arbc::k_fast_rgba8srgb, arbc::k_working_rgba32f),
                kConv_8_32);
  require_bytes(backend_convert_bytes<PixelFormat::Rgba8Srgb, PixelFormat::Rgba16fLinearPremul>(
                    arbc::k_fast_rgba8srgb, arbc::k_working_rgba16f),
                kConv_8_16);
}

// enforces: 09-surfaces-and-backends#convert-is-same-geometry-replace
TEST_CASE("Backend::convert with equal tags is an exact copy, never a re-encode") {
  SECTION("the float formats copy byte-for-byte over a pre-dirtied destination") {
    const auto f32 =
        identity_convert_bytes<PixelFormat::Rgba32fLinearPremul>(arbc::k_working_rgba32f);
    REQUIRE(f32.first == f32.second);
    const auto f16 =
        identity_convert_bytes<PixelFormat::Rgba16fLinearPremul>(arbc::k_working_rgba16f);
    REQUIRE(f16.first == f16.second);
  }

  SECTION("rgba8-sRGB copies exactly where the working-space round-trip does not") {
    arbc::CpuBackend backend;
    arbc::CpuSurface src(256, 1, arbc::k_fast_rgba8srgb);
    write_srgb8_sweep(src);
    const std::vector<std::byte> want = bytes_of(src);

    arbc::CpuSurface dst(256, 1, arbc::k_fast_rgba8srgb);
    backend.clear(dst, 0.9F, 0.1F, 0.7F, 1.0F); // pre-dirty: convert must replace
    backend.convert(dst, src);
    REQUIRE(bytes_of(dst) == want);

    // Decisively NOT a decode/encode round-trip: routing the identity pair through
    // the kernel (decode to premultiplied linear working floats, re-encode) does
    // not reproduce these bytes -- the straight-alpha unpremultiply drops the
    // chroma of the zero-alpha sample outright. `convert` copies instead, so the
    // public operation never silently quantizes on a no-op, which is the trap the
    // import and display-out callers would otherwise inherit.
    arbc::CpuSurface roundtrip(256, 1, arbc::k_fast_rgba8srgb);
    arbc::convert_kernel<PixelFormat::Rgba8Srgb, PixelFormat::Rgba8Srgb>(
        std::as_const(src).span<PixelFormat::Rgba8Srgb>(),
        arbc::TypedSpan<PixelFormat::Rgba8Srgb>{roundtrip.span<PixelFormat::Rgba8Srgb>()}, 256);
    REQUIRE(bytes_of(roundtrip) != want);
  }
}

// enforces: 09-surfaces-and-backends#convert-is-same-geometry-replace
TEST_CASE("convert culls a dimension mismatch rather than resampling") {
  arbc::CpuBackend backend;
  arbc::CpuSurface src(2, 1, arbc::k_working_rgba32f);
  wr<PixelFormat::Rgba32fLinearPremul>(src, 0, kConvA);
  wr<PixelFormat::Rgba32fLinearPremul>(src, 1, kConvB);

  arbc::CpuSurface dst(1, 1, arbc::k_working_rgba16f);
  backend.clear(dst, 0.9F, 0.1F, 0.7F, 1.0F);
  const std::vector<std::byte> before = bytes_of(dst);
  REQUIRE(before.size() == 8); // 1 px rgba16f

  REQUIRE_FALSE(dst.width() == src.width()); // the rejection precondition

#ifdef NDEBUG
  // Release: `convert` is position-for-position and same-geometry by contract, so
  // a dimension mismatch is a caller error and the operation culls (asserts
  // compiled out) -- it never resamples to fit and never writes a partial
  // destination. Debug asserts instead; the predicate is witnessed above without
  // tripping the abort, mirroring the composite tag-mismatch convention in
  // cpu_backend.t.cpp.
  backend.convert(dst, src);
  REQUIRE(bytes_of(dst) == before);
#endif
}

// --- codec reference vectors ------------------------------------------------

// enforces: 07-color-and-pixel-formats#srgb8-round-trips-exactly
TEST_CASE("sRGB / unorm8 codecs pin their absolute curve values") {
  const std::array<std::uint8_t, 4> srgb_codes{0, 1, 128, 255};
  for (std::size_t i = 0; i < srgb_codes.size(); ++i) {
    CAPTURE(i);
    REQUIRE(f32_bits(arbc::srgb8_to_linear(srgb_codes[i])) == kSrgb8ToLinear[i]);
  }
  const std::array<float, 4> srgb_lin{0.0F, 0.0025F, 0.5F, 1.0F};
  for (std::size_t i = 0; i < srgb_lin.size(); ++i) {
    CAPTURE(i);
    REQUIRE(arbc::linear_to_srgb8(srgb_lin[i]) == kLinearToSrgb8[i]);
  }
  const std::array<float, 3> unorm_vals{0.0F, 0.5F, 1.0F};
  for (std::size_t i = 0; i < unorm_vals.size(); ++i) {
    CAPTURE(i);
    REQUIRE(arbc::unorm8_encode(unorm_vals[i]) == kUnorm8Encode[i]);
  }
  const std::array<std::uint8_t, 3> unorm_codes{0, 128, 255};
  for (std::size_t i = 0; i < unorm_codes.size(); ++i) {
    CAPTURE(i);
    REQUIRE(f32_bits(arbc::unorm8_decode(unorm_codes[i])) == kUnorm8Decode[i]);
  }
}

// enforces: 07-color-and-pixel-formats#f16-conversion-portable-and-exact
TEST_CASE("software f16 codec pins its absolute bytes at the IEEE landmarks") {
  const std::array<std::uint16_t, 6> halves{0x0000U, 0x8000U, 0x3800U, 0x3C00U, 0x0001U, 0x7C00U};
  for (std::size_t i = 0; i < halves.size(); ++i) {
    CAPTURE(i);
    REQUIRE(f32_bits(arbc::f16_to_float(halves[i])) == kF16ToFloat[i]);
  }
  const std::array<float, 6> vals{0.0F, -0.0F, 0.5F, 1.0F, kSubnormal, kInf};
  for (std::size_t i = 0; i < vals.size(); ++i) {
    CAPTURE(i);
    REQUIRE(arbc::f16_from_float(vals[i]) == kF16FromFloat[i]);
  }
}

// enforces: 07-color-and-pixel-formats#premultiplied-source-over
TEST_CASE("premultiply / unpremultiply pin their absolute bytes at alpha != {0,1}") {
  const arbc::WorkingPixel pm = arbc::premultiply(kPremulStraight);
  const arbc::WorkingPixel um = arbc::unpremultiply(kUnpremulIn);
  for (std::size_t k = 0; k < 4; ++k) {
    CAPTURE(k);
    REQUIRE(f32_bits(pm[k]) == kPremultiply[k]);
    REQUIRE(f32_bits(um[k]) == kUnpremultiply[k]);
  }
}

// --- regeneration dump (hidden; excluded from coverage) ---------------------
//
// GCOV_EXCL_START -- maintenance tool, run only by the regenerate procedure at
// the top of the file; the kernels it dumps are covered by the golden cases,
// which drive the identical builder functions above.
namespace {

void dump_bytes(const char* name, std::size_t n, const std::vector<std::byte>& b) {
  std::printf("constexpr std::array<unsigned char, %zu> %s{{", b.size(), name);
  (void)n;
  for (std::size_t i = 0; i < b.size(); ++i) {
    std::printf("%s0x%02X", i ? ", " : "",
                static_cast<unsigned>(std::to_integer<unsigned char>(b[i])));
  }
  std::printf("}};\n");
}

template <typename T> void dump_ints(const char* name, const char* type, std::span<const T> v) {
  std::printf("constexpr std::array<%s, %zu> %s{{", type, v.size(), name);
  for (std::size_t i = 0; i < v.size(); ++i) {
    std::printf("%s0x%llX", i ? ", " : "", static_cast<unsigned long long>(v[i]));
  }
  std::printf("}};\n");
}

} // namespace

TEST_CASE("regenerate frozen kernel/codec tables", "[.regen]") {
  dump_bytes("kFillExp32", 0,
             fill_bytes<PixelFormat::Rgba32fLinearPremul>(arbc::k_working_rgba32f));
  dump_bytes("kFillExp16", 0,
             fill_bytes<PixelFormat::Rgba16fLinearPremul>(arbc::k_working_rgba16f));
  dump_bytes("kFillExp8", 0, fill_bytes<PixelFormat::Rgba8Srgb>(arbc::k_fast_rgba8srgb));

  dump_bytes("kSoIntExp32", 0,
             src_over_integer_bytes<PixelFormat::Rgba32fLinearPremul>(arbc::k_working_rgba32f));
  dump_bytes("kSoIntExp16", 0,
             src_over_integer_bytes<PixelFormat::Rgba16fLinearPremul>(arbc::k_working_rgba16f));
  dump_bytes("kSoIntExp8", 0,
             src_over_integer_bytes<PixelFormat::Rgba8Srgb>(arbc::k_fast_rgba8srgb));

  dump_bytes("kSoFracExp32", 0,
             src_over_fractional_bytes<PixelFormat::Rgba32fLinearPremul>(arbc::k_working_rgba32f));
  dump_bytes("kSoFracExp16", 0,
             src_over_fractional_bytes<PixelFormat::Rgba16fLinearPremul>(arbc::k_working_rgba16f));
  dump_bytes("kSoFracExp8", 0,
             src_over_fractional_bytes<PixelFormat::Rgba8Srgb>(arbc::k_fast_rgba8srgb));

  dump_bytes("kDownExp32", 0,
             downsample_bytes<PixelFormat::Rgba32fLinearPremul>(arbc::k_working_rgba32f));
  dump_bytes("kDownExp16", 0,
             downsample_bytes<PixelFormat::Rgba16fLinearPremul>(arbc::k_working_rgba16f));
  dump_bytes("kDownExp8", 0, downsample_bytes<PixelFormat::Rgba8Srgb>(arbc::k_fast_rgba8srgb));

  dump_bytes("kConv_32_16", 0,
             convert_bytes<PixelFormat::Rgba32fLinearPremul, PixelFormat::Rgba16fLinearPremul>(
                 arbc::k_working_rgba32f, arbc::k_working_rgba16f));
  dump_bytes("kConv_32_8", 0,
             convert_bytes<PixelFormat::Rgba32fLinearPremul, PixelFormat::Rgba8Srgb>(
                 arbc::k_working_rgba32f, arbc::k_fast_rgba8srgb));
  dump_bytes("kConv_16_32", 0,
             convert_bytes<PixelFormat::Rgba16fLinearPremul, PixelFormat::Rgba32fLinearPremul>(
                 arbc::k_working_rgba16f, arbc::k_working_rgba32f));
  dump_bytes("kConv_16_8", 0,
             convert_bytes<PixelFormat::Rgba16fLinearPremul, PixelFormat::Rgba8Srgb>(
                 arbc::k_working_rgba16f, arbc::k_fast_rgba8srgb));
  dump_bytes("kConv_8_32", 0,
             convert_bytes<PixelFormat::Rgba8Srgb, PixelFormat::Rgba32fLinearPremul>(
                 arbc::k_fast_rgba8srgb, arbc::k_working_rgba32f));
  dump_bytes("kConv_8_16", 0,
             convert_bytes<PixelFormat::Rgba8Srgb, PixelFormat::Rgba16fLinearPremul>(
                 arbc::k_fast_rgba8srgb, arbc::k_working_rgba16f));

  const std::array<std::uint8_t, 4> srgb_codes{0, 1, 128, 255};
  std::array<std::uint32_t, 4> srgb_lin{};
  for (std::size_t i = 0; i < 4; ++i) {
    srgb_lin[i] = f32_bits(arbc::srgb8_to_linear(srgb_codes[i]));
  }
  dump_ints<std::uint32_t>("kSrgb8ToLinear", "std::uint32_t", srgb_lin);

  const std::array<float, 4> lin_in{0.0F, 0.0025F, 0.5F, 1.0F};
  std::array<unsigned char, 4> lin_to_srgb{};
  for (std::size_t i = 0; i < 4; ++i) {
    lin_to_srgb[i] = arbc::linear_to_srgb8(lin_in[i]);
  }
  dump_ints<unsigned char>("kLinearToSrgb8", "unsigned char", lin_to_srgb);

  const std::array<float, 3> unorm_vals{0.0F, 0.5F, 1.0F};
  std::array<unsigned char, 3> unorm_enc{};
  for (std::size_t i = 0; i < 3; ++i) {
    unorm_enc[i] = arbc::unorm8_encode(unorm_vals[i]);
  }
  dump_ints<unsigned char>("kUnorm8Encode", "unsigned char", unorm_enc);

  const std::array<std::uint8_t, 3> unorm_codes{0, 128, 255};
  std::array<std::uint32_t, 3> unorm_dec{};
  for (std::size_t i = 0; i < 3; ++i) {
    unorm_dec[i] = f32_bits(arbc::unorm8_decode(unorm_codes[i]));
  }
  dump_ints<std::uint32_t>("kUnorm8Decode", "std::uint32_t", unorm_dec);

  const std::array<std::uint16_t, 6> halves{0x0000U, 0x8000U, 0x3800U, 0x3C00U, 0x0001U, 0x7C00U};
  std::array<std::uint32_t, 6> f16_to{};
  for (std::size_t i = 0; i < 6; ++i) {
    f16_to[i] = f32_bits(arbc::f16_to_float(halves[i]));
  }
  dump_ints<std::uint32_t>("kF16ToFloat", "std::uint32_t", f16_to);

  const std::array<float, 6> f16_vals{0.0F, -0.0F, 0.5F, 1.0F, kSubnormal, kInf};
  std::array<std::uint16_t, 6> f16_from{};
  for (std::size_t i = 0; i < 6; ++i) {
    f16_from[i] = arbc::f16_from_float(f16_vals[i]);
  }
  dump_ints<std::uint16_t>("kF16FromFloat", "std::uint16_t", f16_from);

  const arbc::WorkingPixel pm = arbc::premultiply(kPremulStraight);
  const arbc::WorkingPixel um = arbc::unpremultiply(kUnpremulIn);
  std::array<std::uint32_t, 4> pm_bits{};
  std::array<std::uint32_t, 4> um_bits{};
  for (std::size_t k = 0; k < 4; ++k) {
    pm_bits[k] = f32_bits(pm[k]);
    um_bits[k] = f32_bits(um[k]);
  }
  dump_ints<std::uint32_t>("kPremultiply", "std::uint32_t", pm_bits);
  dump_ints<std::uint32_t>("kUnpremultiply", "std::uint32_t", um_bits);
}
// GCOV_EXCL_STOP
