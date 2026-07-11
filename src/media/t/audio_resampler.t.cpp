#include <arbc/media/audio_block.hpp>
#include <arbc/media/audio_resampler.hpp>
#include <arbc/media/resampler_prototype.hpp> // INTERNAL: the shared windowed-sinc generator

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

// Component-level tests for the `arbc::media` windowed-sinc polyphase resampler
// (kinds.nested_audio_resampling). Two complementary layers, both tolerance-free:
//   - SEMANTIC properties that hold by construction of the coefficient table --
//     an integer-ratio phase-0 sample reproduces its native input byte-for-byte
//     (the filter is a genuine interpolator, not a re-quantizer), a fractional
//     phase is decisively NOT a nearest/hold, and the kernel is deterministic;
//   - a FROZEN byte golden pinning the exact reconstructed bytes for a fixed
//     input at a fixed non-integer ratio (doc 16 tier-3), so the mix engine's
//     future reuse has a component-level anchor independent of the nesting
//     descent.
//
// enforces: 12-audio#nested-boundary-resamples-below-rate-children
//
// REGENERATE PROCEDURE (doc 16 tier-3): an intended filter change deliberately
// re-freezes the golden below; it never regenerates silently. Build the target
// and run only the hidden dump case, which prints paste-ready literals:
//
//     cmake --build --preset dev --target arbc_media_t
//     ./build/dev/src/media/arbc_media_t "[.regen]"
//
// then replace the "FROZEN EXPECTED GOLDEN" block with its output.

namespace {

using namespace arbc;

// A fixed non-constant mono native signal (24 frames). Deliberately not a
// constant or a single ramp so the fractional-phase output is provably distinct
// from a hold and the band-limited taps all contribute.
constexpr std::array<float, 24> k_native = {
    0.0F,  0.50F, 0.90F, 1.00F, 0.80F,  0.30F,  -0.30F, -0.80F, -1.0F,  -0.80F, -0.30F, 0.30F,
    0.80F, 1.00F, 0.85F, 0.40F, -0.10F, -0.55F, -0.90F, -0.75F, -0.20F, 0.35F,  0.70F,  0.55F};

std::vector<float> resample_mono(const std::vector<float>& in, std::uint32_t src_rate,
                                 std::uint32_t dst_rate, std::uint32_t out_frames) {
  std::vector<float> out(out_frames, 0.0F);
  AudioBlock in_block{const_cast<float*>(in.data()), static_cast<std::uint32_t>(in.size()),
                      ChannelLayout::Mono, src_rate};
  AudioBlock out_block{out.data(), out_frames, ChannelLayout::Mono, dst_rate};
  resample_audio(in_block, out_block);
  return out;
}

} // namespace

// enforces: 12-audio#nested-boundary-resamples-below-rate-children
TEST_CASE("resample_audio reproduces native samples exactly at integer-ratio aligned phases") {
  const std::vector<float> in(k_native.begin(), k_native.end());
  const std::uint32_t frames = static_cast<std::uint32_t>(in.size());
  // 2:1 upsample (24000 -> 48000): even output frames land on phase 0, an exact
  // Kronecker-delta tap, so they reproduce the native sample byte-for-byte.
  const std::vector<float> out = resample_mono(in, 24'000, 48'000, 2 * frames);
  for (std::uint32_t k = 0; k < frames; ++k) {
    REQUIRE(out[2 * k] == in[k]); // exact, no tolerance -- the interpolator identity
  }
}

// enforces: 12-audio#nested-boundary-resamples-below-rate-children
TEST_CASE("resample_audio is a band-limited reconstruction, decisively not a nearest/hold") {
  const std::vector<float> in(k_native.begin(), k_native.end());
  const std::uint32_t frames = static_cast<std::uint32_t>(in.size());
  const std::vector<float> out = resample_mono(in, 24'000, 48'000, 2 * frames);
  // A nearest/hold fill would make each odd (phase-0.5) output equal one of its
  // integer neighbours. A real windowed-sinc reconstruction does not: assert the
  // interior fractional samples differ from BOTH neighbours (byte-level !=).
  bool any_fractional = false;
  for (std::uint32_t k = 8; k + 8 < frames; ++k) {
    const float mid = out[2 * k + 1];
    REQUIRE(mid != in[k]);
    REQUIRE(mid != in[k + 1]);
    any_fractional = true;
  }
  REQUIRE(any_fractional);
}

// enforces: 12-audio#nested-boundary-resamples-below-rate-children
TEST_CASE("resample_audio is deterministic and stereo-channel-independent") {
  // Interleave the native signal into a stereo block with a distinct right channel
  // (negated), then confirm each channel resamples independently and identically
  // across two runs.
  const std::uint32_t frames = static_cast<std::uint32_t>(k_native.size());
  std::vector<float> stereo(static_cast<std::size_t>(frames) * 2, 0.0F);
  for (std::uint32_t f = 0; f < frames; ++f) {
    stereo[static_cast<std::size_t>(f) * 2] = k_native[f];
    stereo[static_cast<std::size_t>(f) * 2 + 1] = -k_native[f];
  }
  const std::uint32_t out_frames = 2 * frames;
  std::vector<float> a(static_cast<std::size_t>(out_frames) * 2, 0.0F);
  std::vector<float> b(static_cast<std::size_t>(out_frames) * 2, 0.0F);
  AudioBlock in_block{stereo.data(), frames, ChannelLayout::Stereo, 24'000};
  AudioBlock out_a{a.data(), out_frames, ChannelLayout::Stereo, 48'000};
  AudioBlock out_b{b.data(), out_frames, ChannelLayout::Stereo, 48'000};
  resample_audio(in_block, out_a);
  resample_audio(in_block, out_b);
  REQUIRE(a == b); // deterministic
  for (std::uint32_t f = 0; f < out_frames; ++f) {
    REQUIRE(a[static_cast<std::size_t>(f) * 2 + 1] == -a[static_cast<std::size_t>(f) * 2]);
  }
}

// enforces: 12-audio#nested-boundary-resamples-below-rate-children
TEST_CASE("resample_audio leaves the output untouched on an equal-rate shape") {
  const std::vector<float> in(k_native.begin(), k_native.end());
  // Equal rate is the caller's 1:1 path -- the kernel is a no-op. (Downsampling is no
  // longer a no-op: it decimates through the widened bank, audio.device_edge_decimation.)
  std::vector<float> out(4, 7.0F);
  AudioBlock in_block{const_cast<float*>(in.data()), static_cast<std::uint32_t>(in.size()),
                      ChannelLayout::Mono, 48'000};
  AudioBlock out_block{out.data(), 4, ChannelLayout::Mono, 48'000}; // equal rate
  resample_audio(in_block, out_block);
  REQUIRE(std::all_of(out.begin(), out.end(), [](float v) { return v == 7.0F; }));
}

namespace {

// Byte-compare helper over the interleaved float buffer.
std::vector<std::byte> as_bytes(const std::vector<float>& v) {
  std::vector<std::byte> out(v.size() * sizeof(float));
  std::memcpy(out.data(), v.data(), out.size());
  return out;
}

// The fixed golden ratio: 32000 -> 48000 (2:3, a non-integer fractional-phase
// path exercising the polyphase bank), 30 output frames.
constexpr std::uint32_t k_golden_src = 32'000;
constexpr std::uint32_t k_golden_dst = 48'000;
constexpr std::uint32_t k_golden_out_frames = 30;

std::vector<float> golden_output() {
  const std::vector<float> in(k_native.begin(), k_native.end());
  return resample_mono(in, k_golden_src, k_golden_dst, k_golden_out_frames);
}

// ===========================================================================
// FROZEN EXPECTED GOLDEN -- regenerate deliberately (see procedure at top).
// ===========================================================================
constexpr std::array<unsigned char, 120> k_golden_32k_to_48k = {
    0x00, 0x00, 0x00, 0x00, 0xA4, 0xEC, 0x97, 0x3E, 0x20, 0xCA, 0x2D, 0x3F, 0x66, 0x66, 0x66,
    0x3F, 0x09, 0xBA, 0x7E, 0x3F, 0x68, 0x15, 0x78, 0x3F, 0xCD, 0xCC, 0x4C, 0x3F, 0xD1, 0xCA,
    0xFD, 0x3E, 0x7F, 0x8F, 0xBF, 0x3D, 0x9A, 0x99, 0x99, 0xBE, 0x9A, 0xDF, 0x26, 0xBF, 0x60,
    0x5F, 0x69, 0xBF, 0x00, 0x00, 0x80, 0xBF, 0x52, 0x58, 0x69, 0xBF, 0xF8, 0xEC, 0x26, 0xBF,
    0x9A, 0x99, 0x99, 0xBE, 0x0E, 0x84, 0xC0, 0x3D, 0x28, 0x81, 0xFD, 0x3E, 0xCD, 0xCC, 0x4C,
    0x3F, 0x9F, 0x59, 0x78, 0x3F, 0x98, 0x2A, 0x7D, 0x3F, 0x9A, 0x99, 0x59, 0x3F, 0x9C, 0x28,
    0x13, 0x3F, 0xB1, 0x67, 0x62, 0x3E, 0xCD, 0xCC, 0xCC, 0xBD, 0xF2, 0x2F, 0xCA, 0xBE, 0xED,
    0x46, 0x33, 0xBF, 0x66, 0x66, 0x66, 0xBF, 0xB2, 0x10, 0x5E, 0xBF, 0x54, 0x35, 0x15, 0xBF};
// ===========================================================================
// END FROZEN EXPECTED GOLDEN
// ===========================================================================

} // namespace

// enforces: 12-audio#nested-boundary-resamples-below-rate-children
// enforces: 16-sdlc-and-quality#byte-exact-goldens
TEST_CASE("resample_audio pins byte-exact reconstructed output at a fixed non-integer ratio") {
  const std::vector<std::byte> got = as_bytes(golden_output());
  REQUIRE(got.size() == k_golden_32k_to_48k.size());
  REQUIRE(std::memcmp(got.data(), k_golden_32k_to_48k.data(), got.size()) == 0);
}

// The frozen table in `audio_resampler.cpp` is checked-in DATA, and until now nothing in
// the repo could regenerate it: its REGENERATE PROCEDURE pointed at the `[.regen]` case
// below, which dumped only the output golden. The table and the "one generator, not a
// second algorithm" claim it rests on were both unverifiable.
//
// So pin the generator against the table. This is the guard that lets the dumper be
// trusted: if someone edits `resampler_prototype::generate` -- the SAME function that
// builds the widened decimation bank at every device-edge configure -- and the frozen
// upsampling table no longer falls out of it, this fails loudly here rather than
// silently emitting a subtly wrong table the next time someone runs `[.regen]`.
//
// enforces: 16-sdlc-and-quality#byte-exact-goldens
TEST_CASE("the shared prototype still reproduces the frozen upsampling bank byte-for-byte") {
  const arbc::resampler_prototype::FrozenBank frozen = arbc::resampler_prototype::frozen_bank();
  const arbc::PolyphaseBank regenerated = arbc::resampler_prototype::generate(
      frozen.phases, frozen.taps, /*fc=*/1.0, /*force_integer_zero=*/true);

  REQUIRE(regenerated.taps == frozen.taps);
  REQUIRE(regenerated.coeffs.size() == frozen.count);
  // Bit-for-bit, not approximately: the table is frozen bytes (doc 16 tier-3).
  REQUIRE(std::memcmp(regenerated.coeffs.data(), frozen.coeffs, frozen.count * sizeof(float)) == 0);
}

// GCOV_EXCL_START -- maintenance dumper, not shipped behavior.
namespace {
void dump(const char* name, const std::vector<std::byte>& bytes) {
  std::printf("constexpr std::array<unsigned char, %zu> %s = {\n    ", bytes.size(), name);
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    std::printf("0x%02X%s", static_cast<unsigned>(std::to_integer<unsigned char>(bytes[i])),
                i + 1 == bytes.size() ? "};\n" : (i % 16 == 15 ? ",\n    " : ", "));
  }
}

// Print the frozen polyphase bank paste-ready for `audio_resampler.cpp`, as hex floats
// (an exact round-trip -- a decimal literal is not). Phase-major, one tap per line, with
// the phase index marked on each phase's last tap exactly as the checked-in block has it.
void dump_coefficients() {
  const arbc::resampler_prototype::FrozenBank frozen = arbc::resampler_prototype::frozen_bank();
  const arbc::PolyphaseBank bank = arbc::resampler_prototype::generate(
      frozen.phases, frozen.taps, /*fc=*/1.0, /*force_integer_zero=*/true);

  std::printf("// PHASES=%llu TAPS=%lld (half N=%lld), Blackman-Harris windowed sinc, "
              "per-phase normalized\n",
              static_cast<unsigned long long>(frozen.phases), static_cast<long long>(frozen.taps),
              static_cast<long long>(frozen.taps / 2));
  std::printf("constexpr std::array<float, %zu> k_resampler_coeffs = {\n", bank.coeffs.size());
  for (std::size_t i = 0; i < bank.coeffs.size(); ++i) {
    const bool last_of_phase = (i + 1) % static_cast<std::size_t>(frozen.taps) == 0;
    std::printf("    %af,", static_cast<double>(bank.coeffs[i]));
    if (last_of_phase) {
      std::printf(" // phase %zu", i / static_cast<std::size_t>(frozen.taps));
    }
    std::printf("\n");
  }
  std::printf("};\n");
}
} // namespace

TEST_CASE("dump resampler goldens", "[.regen]") {
  dump_coefficients();
  dump("k_golden_32k_to_48k", as_bytes(golden_output()));
}
// GCOV_EXCL_STOP
