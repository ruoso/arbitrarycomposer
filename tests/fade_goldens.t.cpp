// Byte-exact goldens for org.arbc.fade (refinement Acceptance "Byte-exact
// goldens"; doc 16 tier-3 deterministic rendering). Two frozen tables:
//   (a) a visual tile at a partial envelope (E = 0.5) over a known solid input,
//       proving premultiplied-RGBA attenuation through the real CpuBackend
//       composite (Decision 4);
//   (b) an audio block at a per-frame envelope ramp over a known 440 Hz tone,
//       proving per-frame gain attenuation (Decision 5).
// Both are byte-exact with no tolerance (memcmp): the tone waveform is a
// parabolic sine over an exact integer flick phase (never std::sin) and the
// composite is the deterministic CPU path (doc 16: fixed FP flags).
//
// This file also holds the both-facets behavioral cases (fully-closed ->
// transparent / silent; fully-open -> byte-identical no-op) that the goldens'
// claim rests on.
//
// It is a CROSS-COMPONENT test (it drives the real CpuBackend composite over a
// solid / tone input), so per doc 17 levelization it lives in tests/ rather than
// src/kind_fade/t/ -- exactly as tests/nested_goldens.t.cpp does for the nested
// kind, whose byte-exact golden also needs the CPU backend.
//
// REGENERATE PROCEDURE (doc 16 tier-3): an intended rendering change deliberately
// re-freezes these tables; they never regenerate silently. Build the target and
// run only the hidden dump case, which prints paste-ready literals:
//
//     cmake --build --preset dev --target arbc_fade_goldens_t
//     ./build/dev/tests/arbc_fade_goldens_t "[.regen]"

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/time.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/kind_fade/fade_content.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/kind_tone/tone_content.hpp>
#include <arbc/media/audio_block.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/surface/surface.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <vector>

using namespace arbc;

namespace {

// Route both facets to the wrapped input (the inline PullService a fade needs
// at attach; production wiring is the deferred operators.fade_runtime_binding).
class InlinePull final : public PullService {
public:
  void pull(ContentRef input, const RenderRequest& request,
            std::shared_ptr<RenderCompletion> done) override {
    if (input == nullptr) {
      done->fail(RenderError::ContentFailed);
      return;
    }
    const std::optional<RenderResult> r = input->render(request, done);
    if (r.has_value()) {
      done->complete(*r);
    }
  }
  void pull_audio(ContentRef input, const AudioRequest& request,
                  std::shared_ptr<AudioCompletion> done) override {
    AudioFacet* af = input != nullptr ? input->audio() : nullptr;
    if (af == nullptr) {
      done->fail(RenderError::ResourceUnavailable);
      return;
    }
    const std::optional<AudioResult> r = af->render_audio(request, done);
    if (r.has_value()) {
      done->complete(*r);
    } else if (!done->settled()) {
      done->fail(RenderError::ContentFailed);
    }
  }
};

constexpr std::uint32_t k_rate = 48000;
constexpr std::uint32_t k_frames = 16;

std::int64_t flicks_per_frame() {
  return Time::flicks_per_second / static_cast<std::int64_t>(k_rate);
}

// Render `fade` into a `width`x`height` rgba32f target at scale 1 and time
// `time`, returning the raw target bytes.
std::vector<std::byte> render_fade_bytes(FadeContent& fade, CpuBackend& backend, int width,
                                         int height, Time time) {
  const auto target = backend.make_surface(width, height, k_working_rgba32f);
  REQUIRE(target.has_value());
  const RenderRequest req{Rect::from_size(static_cast<double>(width), static_cast<double>(height)),
                          1.0,
                          time,
                          StateHandle{},
                          **target,
                          Exactness::Exact,
                          Deadline::none()};
  auto done = std::make_shared<RenderCompletion>();
  const std::optional<RenderResult> r = fade.render(req, done);
  REQUIRE(r.has_value());
  const std::span<const std::byte> bytes = (**target).cpu_bytes();
  return std::vector<std::byte>(bytes.begin(), bytes.end());
}

// The visual golden: a 2x2 solid at E = 0.5 (fade-in [0, 1000), rendered at
// t = 500). Every channel of the premultiplied source is halved.
std::vector<std::byte> render_visual_golden() {
  CpuBackend backend;
  InlinePull pull;
  SolidContent solid{Rgba{0.5F, 0.25F, 0.125F, 1.0F}, Rect{0.0, 0.0, 2.0, 2.0}};
  FadeContent fade{&solid,
                   FadeParams{FadeShape::Linear, FadeWindow{Time{0}, Time{1000}}, std::nullopt}};
  fade.attach(pull, backend);
  return render_fade_bytes(fade, backend, 2, 2, Time{500});
}

// Render `fade`'s audio for a 16-frame stereo block at 48 kHz over [0, 16*fpf),
// returning the raw interleaved float32 bytes.
std::vector<std::byte> render_fade_audio_bytes(FadeContent& fade) {
  AudioFacet* af = fade.audio();
  REQUIRE(af != nullptr);
  std::vector<float> samples(static_cast<std::size_t>(k_frames) * 2U, 0.0F);
  AudioBlock block{samples.data(), k_frames, ChannelLayout::Stereo, k_rate};
  const AudioRequest req{
      TimeRange{Time::zero(), Time{static_cast<std::int64_t>(k_frames) * flicks_per_frame()}},
      k_rate,
      ChannelLayout::Stereo,
      block,
      Exactness::Exact,
      StateHandle{}};
  auto done = std::make_shared<AudioCompletion>();
  const std::optional<AudioResult> r = af->render_audio(req, done);
  REQUIRE(r.has_value());
  std::vector<std::byte> bytes(samples.size() * sizeof(float));
  std::memcpy(bytes.data(), samples.data(), bytes.size());
  return bytes;
}

// The audio golden: a 440 Hz tone under a per-frame ramp (fade-in [0, 16*fpf)),
// so frame f is scaled by f/16 -- a clean per-frame attenuation.
std::vector<std::byte> render_audio_golden() {
  CpuBackend backend;
  InlinePull pull;
  ToneContent tone{440, 0.5F};
  FadeContent fade{&tone, FadeParams{FadeShape::Linear,
                                     FadeWindow{Time{0}, Time{static_cast<std::int64_t>(k_frames) *
                                                              flicks_per_frame()}},
                                     std::nullopt}};
  fade.attach(pull, backend);
  return render_fade_audio_bytes(fade);
}

void require_bytes(const std::vector<std::byte>& got, std::span<const unsigned char> want) {
  REQUIRE(got.size() == want.size());
  REQUIRE(std::memcmp(got.data(), want.data(), want.size()) == 0);
}

// ===========================================================================
// FROZEN EXPECTED TABLES -- regenerate deliberately (see procedure at top).
// ===========================================================================

constexpr std::array<unsigned char, 64> kVisualHalf = {
    0x00, 0x00, 0x80, 0x3E, 0x00, 0x00, 0x00, 0x3E, 0x00, 0x00, 0x80, 0x3D, 0x00, 0x00, 0x00, 0x3F,
    0x00, 0x00, 0x80, 0x3E, 0x00, 0x00, 0x00, 0x3E, 0x00, 0x00, 0x80, 0x3D, 0x00, 0x00, 0x00, 0x3F,
    0x00, 0x00, 0x80, 0x3E, 0x00, 0x00, 0x00, 0x3E, 0x00, 0x00, 0x80, 0x3D, 0x00, 0x00, 0x00, 0x3F,
    0x00, 0x00, 0x80, 0x3E, 0x00, 0x00, 0x00, 0x3E, 0x00, 0x00, 0x80, 0x3D, 0x00, 0x00, 0x00, 0x3F};

constexpr std::array<unsigned char, 128> kAudioRamp = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xE9, 0x6E, 0x13, 0x3B, 0xE9, 0x6E, 0x13, 0x3B,
    0x09, 0xAE, 0x10, 0x3C, 0x09, 0xAE, 0x10, 0x3C, 0xCE, 0xAA, 0x9F, 0x3C, 0xCE, 0xAA, 0x9F, 0x3C,
    0x48, 0x2C, 0x0B, 0x3D, 0x48, 0x2C, 0x0B, 0x3D, 0xD2, 0x27, 0x55, 0x3D, 0xD2, 0x27, 0x55, 0x3D,
    0xD9, 0x5F, 0x96, 0x3D, 0xD9, 0x5F, 0x96, 0x3D, 0xC8, 0x75, 0xC8, 0x3D, 0xC8, 0x75, 0xC8, 0x3D,
    0xC7, 0x28, 0x00, 0x3E, 0xC7, 0x28, 0x00, 0x3E, 0x80, 0xB7, 0x1E, 0x3E, 0x80, 0xB7, 0x1E, 0x3E,
    0xFA, 0xA4, 0x3F, 0x3E, 0xFA, 0xA4, 0x3F, 0x3E, 0x21, 0xAF, 0x62, 0x3E, 0x21, 0xAF, 0x62, 0x3E,
    0xEF, 0xC9, 0x83, 0x3E, 0xEF, 0xC9, 0x83, 0x3E, 0x8E, 0x08, 0x97, 0x3E, 0x8E, 0x08, 0x97, 0x3E,
    0x65, 0xF2, 0xAA, 0x3E, 0x65, 0xF2, 0xAA, 0x3E, 0x67, 0x66, 0xBF, 0x3E, 0x67, 0x66, 0xBF, 0x3E};

// ===========================================================================
// END FROZEN EXPECTED TABLES
// ===========================================================================

} // namespace

// enforces: 16-sdlc-and-quality#byte-exact-goldens
TEST_CASE("org.arbc.fade renders a byte-exact visual golden at a partial envelope") {
  require_bytes(render_visual_golden(), kVisualHalf);
}

// enforces: 16-sdlc-and-quality#byte-exact-goldens
TEST_CASE("org.arbc.fade renders a byte-exact audio golden under a per-frame ramp") {
  require_bytes(render_audio_golden(), kAudioRamp);
}

// enforces: 13-effects-as-operators#fade-attenuates-both-facets
TEST_CASE("org.arbc.fade fully-closed envelope yields transparent and silent output") {
  CpuBackend backend;
  InlinePull pull;

  // Visual: a fade-in window in the future, rendered before it opens -> E = 0.
  SolidContent solid{Rgba{0.5F, 0.25F, 0.125F, 1.0F}, Rect{0.0, 0.0, 2.0, 2.0}};
  FadeContent vfade{
      &solid, FadeParams{FadeShape::Linear, FadeWindow{Time{1000}, Time{2000}}, std::nullopt}};
  vfade.attach(pull, backend);
  const std::vector<std::byte> pixels = render_fade_bytes(vfade, backend, 2, 2, Time{0});
  CHECK(std::all_of(pixels.begin(), pixels.end(), [](std::byte b) { return b == std::byte{0}; }));

  // Audio: the same closed envelope over a tone -> all-zero samples.
  ToneContent tone{440, 0.5F};
  FadeContent afade{&tone,
                    FadeParams{FadeShape::Linear,
                               FadeWindow{Time{1'000'000'000}, Time{2'000'000'000}}, std::nullopt}};
  afade.attach(pull, backend);
  const std::vector<std::byte> silence = render_fade_audio_bytes(afade);
  CHECK(std::all_of(silence.begin(), silence.end(), [](std::byte b) { return b == std::byte{0}; }));
}

// enforces: 13-effects-as-operators#fade-attenuates-both-facets
TEST_CASE("org.arbc.fade fully-open envelope is a byte-identical no-op") {
  CpuBackend backend;
  InlinePull pull;

  // Visual: a fade with no windows (E == 1) renders byte-identically to the
  // solid rendered directly.
  SolidContent solid{Rgba{0.5F, 0.25F, 0.125F, 1.0F}, Rect{0.0, 0.0, 2.0, 2.0}};
  FadeContent vfade{&solid, FadeParams{}};
  vfade.attach(pull, backend);
  const std::vector<std::byte> faded = render_fade_bytes(vfade, backend, 2, 2, Time{0});

  const auto direct = backend.make_surface(2, 2, k_working_rgba32f);
  REQUIRE(direct.has_value());
  const RenderRequest req{
      Rect::from_size(2.0, 2.0), 1.0, Time{0}, StateHandle{}, **direct, Exactness::Exact,
      Deadline::none()};
  auto done = std::make_shared<RenderCompletion>();
  REQUIRE(solid.render(req, done).has_value());
  const std::span<const std::byte> direct_bytes = (**direct).cpu_bytes();
  CHECK(std::equal(faded.begin(), faded.end(), direct_bytes.begin(), direct_bytes.end()));

  // Audio: a no-window fade over a tone equals the tone rendered directly.
  ToneContent tone{440, 0.5F};
  FadeContent afade{&tone, FadeParams{}};
  afade.attach(pull, backend);
  const std::vector<std::byte> afaded = render_fade_audio_bytes(afade);

  std::vector<float> tsamples(static_cast<std::size_t>(k_frames) * 2U, 0.0F);
  AudioBlock tblock{tsamples.data(), k_frames, ChannelLayout::Stereo, k_rate};
  const AudioRequest treq{
      TimeRange{Time::zero(), Time{static_cast<std::int64_t>(k_frames) * flicks_per_frame()}},
      k_rate,
      ChannelLayout::Stereo,
      tblock,
      Exactness::Exact,
      StateHandle{}};
  auto tdone = std::make_shared<AudioCompletion>();
  REQUIRE(tone.audio()->render_audio(treq, tdone).has_value());
  std::vector<std::byte> tbytes(tsamples.size() * sizeof(float));
  std::memcpy(tbytes.data(), tsamples.data(), tbytes.size());
  CHECK(afaded == tbytes);
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

TEST_CASE("dump fade goldens", "[.regen]") {
  dump("kVisualHalf", render_visual_golden());
  dump("kAudioRamp", render_audio_golden());
}
// GCOV_EXCL_STOP
