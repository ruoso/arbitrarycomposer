// Byte-exact goldens for org.arbc.tone (refinement Acceptance "Byte-exact
// golden"; doc 16 tier-3 deterministic rendering). A fixed tone (440 Hz,
// amplitude 0.5, window starting at flick 0, 16 stereo frames) is rendered and
// its interleaved float32 block compared byte-for-byte against a frozen table.
// The table is portable across toolchains because the waveform is a parabolic
// sine over an exact integer flick phase, not std::sin (see tone_content.cpp).
// A second golden at 44100 Hz doubles as visible evidence of rate independence.
//
// REGENERATE PROCEDURE (doc 16 tier-3): an intended waveform change deliberately
// re-freezes these tables; they never regenerate silently. Build the target and
// run only the hidden dump case, which prints paste-ready literals:
//
//     cmake --build --preset dev --target arbc_kind_tone_t
//     ./build/dev/src/kind_tone/arbc_kind_tone_t "[.regen]"

#include <arbc/contract/content.hpp>
#include <arbc/kind_tone/tone_content.hpp>
#include <arbc/media/audio_block.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

using namespace arbc;

namespace {

constexpr std::uint32_t k_frequency_hz = 440;
constexpr float k_amplitude = 0.5F;
constexpr std::uint32_t k_frames = 16;

// Render the fixed tone at `rate` and return the raw bytes of the interleaved
// float32 stereo block (the exact bytes the caller-owned AudioBlock receives).
std::vector<std::byte> render_tone_bytes(std::uint32_t rate) {
  ToneContent tone(k_frequency_hz, k_amplitude);
  AudioFacet* facet = tone.audio();
  REQUIRE(facet != nullptr);

  std::vector<float> samples(static_cast<std::size_t>(k_frames) * 2U, 0.0F);
  AudioBlock block{samples.data(), k_frames, ChannelLayout::Stereo, rate};

  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(rate);
  const AudioRequest request{
      TimeRange{Time::zero(), Time{static_cast<std::int64_t>(k_frames) * fpf}}, rate,
      ChannelLayout::Stereo, block, Exactness::Exact};

  auto done = std::make_shared<AudioCompletion>();
  const std::optional<AudioResult> result = facet->render_audio(request, done);
  REQUIRE(result.has_value());

  std::vector<std::byte> bytes(samples.size() * sizeof(float));
  std::memcpy(bytes.data(), samples.data(), bytes.size());
  return bytes;
}

void require_bytes(const std::vector<std::byte>& got, std::span<const unsigned char> want) {
  REQUIRE(got.size() == want.size());
  REQUIRE(std::memcmp(got.data(), want.data(), want.size()) == 0);
}

// ===========================================================================
// FROZEN EXPECTED TABLES -- regenerate deliberately (see procedure at top).
// ===========================================================================

constexpr std::array<unsigned char, 128> kTone48k = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xE9, 0x6E, 0x13, 0x3D, 0xE9, 0x6E, 0x13, 0x3D,
    0x09, 0xAE, 0x90, 0x3D, 0x09, 0xAE, 0x90, 0x3D, 0xBD, 0xE3, 0xD4, 0x3D, 0xBD, 0xE3, 0xD4, 0x3D,
    0x48, 0x2C, 0x0B, 0x3E, 0x48, 0x2C, 0x0B, 0x3E, 0x42, 0x86, 0x2A, 0x3E, 0x42, 0x86, 0x2A, 0x3E,
    0xCC, 0x7F, 0x48, 0x3E, 0xCC, 0x7F, 0x48, 0x3E, 0xE5, 0x18, 0x65, 0x3E, 0xE5, 0x18, 0x65, 0x3E,
    0xC7, 0x28, 0x80, 0x3E, 0xC7, 0x28, 0x80, 0x3E, 0xE4, 0x14, 0x8D, 0x3E, 0xE4, 0x14, 0x8D, 0x3E,
    0xC8, 0x50, 0x99, 0x3E, 0xC8, 0x50, 0x99, 0x3E, 0x75, 0xDC, 0xA4, 0x3E, 0x75, 0xDC, 0xA4, 0x3E,
    0xE9, 0xB7, 0xAF, 0x3E, 0xE9, 0xB7, 0xAF, 0x3E, 0x25, 0xE3, 0xB9, 0x3E, 0x25, 0xE3, 0xB9, 0x3E,
    0x2A, 0x5E, 0xC3, 0x3E, 0x2A, 0x5E, 0xC3, 0x3E, 0xF6, 0x28, 0xCC, 0x3E, 0xF6, 0x28, 0xCC, 0x3E};

constexpr std::array<unsigned char, 128> kTone44k = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xDF, 0x34, 0x20, 0x3D, 0xDF, 0x34, 0x20, 0x3D,
    0xCF, 0xF1, 0x9C, 0x3D, 0xCF, 0xF1, 0x9C, 0x3D, 0x20, 0x86, 0xE6, 0x3D, 0x20, 0x86, 0xE6, 0x3D,
    0xB0, 0x6B, 0x16, 0x3E, 0xB0, 0x6B, 0x16, 0x3E, 0xC9, 0xF2, 0x37, 0x3E, 0xC9, 0xF2, 0x37, 0x3E,
    0x5A, 0xD8, 0x57, 0x3E, 0x5A, 0xD8, 0x57, 0x3E, 0x63, 0x1C, 0x76, 0x3E, 0x63, 0x1C, 0x76, 0x3E,
    0x72, 0x5F, 0x89, 0x3E, 0x72, 0x5F, 0x89, 0x3E, 0xEF, 0xDF, 0x96, 0x3E, 0xEF, 0xDF, 0x96, 0x3E,
    0xA8, 0x8F, 0xA3, 0x3E, 0xA8, 0x8F, 0xA3, 0x3E, 0x9C, 0x6E, 0xAF, 0x3E, 0x9C, 0x6E, 0xAF, 0x3E,
    0xCE, 0x7C, 0xBA, 0x3E, 0xCE, 0x7C, 0xBA, 0x3E, 0x3B, 0xBA, 0xC4, 0x3E, 0x3B, 0xBA, 0xC4, 0x3E,
    0xE4, 0x26, 0xCE, 0x3E, 0xE4, 0x26, 0xCE, 0x3E, 0xC9, 0xC2, 0xD6, 0x3E, 0xC9, 0xC2, 0xD6, 0x3E};

// ===========================================================================
// END FROZEN EXPECTED TABLES
// ===========================================================================

} // namespace

// enforces: 12-audio#tone-renders-at-any-requested-rate
TEST_CASE("org.arbc.tone renders a byte-exact golden at 48 kHz") {
  require_bytes(render_tone_bytes(48000), kTone48k);
}

// enforces: 12-audio#tone-renders-at-any-requested-rate
TEST_CASE("org.arbc.tone renders a byte-exact golden at 44100 Hz") {
  require_bytes(render_tone_bytes(44100), kTone44k);
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

} // namespace

TEST_CASE("dump tone goldens", "[.regen]") {
  dump("kTone48k", render_tone_bytes(48000));
  dump("kTone44k", render_tone_bytes(44100));
}
// GCOV_EXCL_STOP
