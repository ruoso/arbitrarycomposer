// Byte-exact goldens for org.arbc.crossfade (refinement Acceptance "Byte-exact
// goldens"; doc 16 tier-3 deterministic rendering). Two frozen tables:
//   (a) a visual tile at w == 0.5 over two differently-colored solid inputs,
//       proving the source-over dissolve of input 1 over input 0 at opacity w
//       through the real CpuBackend composite (Decision 1);
//   (b) an audio block at w == 0.5 over two distinct tone inputs, proving the
//       per-frame complementary-weight additive mix s0*(1-w) + s1*w (Decision 1).
// Both are byte-exact with no tolerance (memcmp): the tone waveform is a
// parabolic sine over an exact integer flick phase (never std::sin) and the
// composite is the deterministic CPU path (doc 16: fixed FP flags).
//
// This file also holds the endpoint pass-through behavioral cases (w == 0 ->
// byte-identical to input 0; w == 1 -> byte-identical to input 1) that the
// endpoint identity claim rests on (Decision 2).
//
// It is a CROSS-COMPONENT test (it drives the real CpuBackend composite over
// solid / tone inputs), so per doc 17 levelization it lives in tests/ rather
// than src/kind_crossfade/t/ -- exactly as tests/fade_goldens.t.cpp does.
//
// REGENERATE PROCEDURE (doc 16 tier-3): an intended rendering change deliberately
// re-freezes these tables; they never regenerate silently. Build the target and
// run only the hidden dump case, which prints paste-ready literals:
//
//     cmake --build --preset dev --target arbc_crossfade_goldens_t
//     ./build/dev/tests/arbc_crossfade_goldens_t "[.regen]"

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/time.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/kind_crossfade/crossfade_content.hpp>
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

// Route both facets to the wrapped input (the inline PullService a crossfade
// needs at attach; production wiring is the deferred
// operators.crossfade_runtime_binding).
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

// Render `xf` into a `width`x`height` rgba32f target at scale 1 and time `time`,
// returning the raw target bytes.
std::vector<std::byte> render_crossfade_bytes(CrossfadeContent& xf, CpuBackend& backend, int width,
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
  const std::optional<RenderResult> r = xf.render(req, done);
  REQUIRE(r.has_value());
  const std::span<const std::byte> bytes = (**target).cpu_bytes();
  return std::vector<std::byte>(bytes.begin(), bytes.end());
}

// The visual golden: a 2x2 crossfade at w == 0.5 (window [0, 1000), rendered at
// t = 500) of a red-ish over a blue-ish solid -- one source-over dissolve.
std::vector<std::byte> render_visual_golden() {
  CpuBackend backend;
  InlinePull pull;
  SolidContent from{Rgba{0.5F, 0.25F, 0.125F, 1.0F}, Rect{0.0, 0.0, 2.0, 2.0}};
  SolidContent to{Rgba{0.125F, 0.375F, 0.75F, 1.0F}, Rect{0.0, 0.0, 2.0, 2.0}};
  CrossfadeContent xf{&from, &to, CrossfadeParams{CrossfadeShape::Linear, Time{0}, Time{1000}}};
  xf.attach(pull, backend);
  return render_crossfade_bytes(xf, backend, 2, 2, Time{500});
}

// Render `xf`'s audio for a 16-frame stereo block at 48 kHz over
// [window_start, window_start + 16*fpf), returning the raw interleaved float32
// bytes.
std::vector<std::byte> render_crossfade_audio_bytes(CrossfadeContent& xf, Time window_start) {
  AudioFacet* af = xf.audio();
  REQUIRE(af != nullptr);
  std::vector<float> samples(static_cast<std::size_t>(k_frames) * 2U, 0.0F);
  AudioBlock block{samples.data(), k_frames, ChannelLayout::Stereo, k_rate};
  const AudioRequest req{
      TimeRange{window_start, Time{window_start.flicks +
                                   static_cast<std::int64_t>(k_frames) * flicks_per_frame()}},
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

// The audio golden: two distinct tones crossfaded at w == 0.5. The window spans
// [0, 2s); the block starts at 1s (= duration/2), so frame 0 is w == 0.5 exactly
// and the block sits right at the crossover -- the complementary-weight mix.
std::vector<std::byte> render_audio_golden() {
  CpuBackend backend;
  InlinePull pull;
  ToneContent from{440, 0.5F};
  ToneContent to{660, 0.25F};
  const std::int64_t two_seconds = 2 * Time::flicks_per_second;
  CrossfadeContent xf{&from, &to,
                      CrossfadeParams{CrossfadeShape::Linear, Time{0}, Time{two_seconds}}};
  xf.attach(pull, backend);
  return render_crossfade_audio_bytes(xf, Time{Time::flicks_per_second}); // 1s == w 0.5
}

void require_bytes(const std::vector<std::byte>& got, std::span<const unsigned char> want) {
  REQUIRE(got.size() == want.size());
  REQUIRE(std::memcmp(got.data(), want.data(), want.size()) == 0);
}

// ===========================================================================
// FROZEN EXPECTED TABLES -- regenerate deliberately (see procedure at top).
// ===========================================================================

constexpr std::array<unsigned char, 64> kVisualMid = {
    0x00, 0x00, 0xA0, 0x3E, 0x00, 0x00, 0xA0, 0x3E, 0x00, 0x00, 0xE0, 0x3E, 0x00, 0x00, 0x80, 0x3F,
    0x00, 0x00, 0xA0, 0x3E, 0x00, 0x00, 0xA0, 0x3E, 0x00, 0x00, 0xE0, 0x3E, 0x00, 0x00, 0x80, 0x3F,
    0x00, 0x00, 0xA0, 0x3E, 0x00, 0x00, 0xA0, 0x3E, 0x00, 0x00, 0xE0, 0x3E, 0x00, 0x00, 0x80, 0x3F,
    0x00, 0x00, 0xA0, 0x3E, 0x00, 0x00, 0xA0, 0x3E, 0x00, 0x00, 0xE0, 0x3E, 0x00, 0x00, 0x80, 0x3F};

constexpr std::array<unsigned char, 128> kAudioMid = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC8, 0x7C, 0x00, 0x3D, 0xC8, 0x7C, 0x00, 0x3D,
    0x80, 0x1F, 0x7B, 0x3D, 0x80, 0x1F, 0x7B, 0x3D, 0x12, 0xF4, 0xB7, 0x3D, 0x12, 0xF4, 0xB7, 0x3D,
    0x5A, 0x6B, 0xEF, 0x3D, 0x5A, 0x6B, 0xEF, 0x3D, 0xCC, 0xFA, 0x11, 0x3E, 0xCC, 0xFA, 0x11, 0x3E,
    0x65, 0xC9, 0x2A, 0x3E, 0x65, 0xC9, 0x2A, 0x3E, 0x76, 0x21, 0x42, 0x3E, 0x76, 0x21, 0x42, 0x3E,
    0x03, 0x03, 0x58, 0x3E, 0x03, 0x03, 0x58, 0x3E, 0x09, 0x6E, 0x6C, 0x3E, 0x09, 0x6E, 0x6C, 0x3E,
    0x88, 0x62, 0x7F, 0x3E, 0x88, 0x62, 0x7F, 0x3E, 0x3F, 0x70, 0x88, 0x3E, 0x3F, 0x70, 0x88, 0x3E,
    0xF7, 0x73, 0x90, 0x3E, 0xF7, 0x73, 0x90, 0x3E, 0x6A, 0xBC, 0x97, 0x3E, 0x6A, 0xBC, 0x97, 0x3E,
    0x9A, 0x49, 0x9E, 0x3E, 0x9A, 0x49, 0x9E, 0x3E, 0x84, 0x1B, 0xA4, 0x3E, 0x84, 0x1B, 0xA4, 0x3E};

// ===========================================================================
// END FROZEN EXPECTED TABLES
// ===========================================================================

} // namespace

// enforces: 16-sdlc-and-quality#byte-exact-goldens
// enforces: 13-effects-as-operators#crossfade-mixes-both-facets
TEST_CASE("org.arbc.crossfade renders a byte-exact visual dissolve golden at w == 0.5") {
  require_bytes(render_visual_golden(), kVisualMid);
}

// enforces: 16-sdlc-and-quality#byte-exact-goldens
// enforces: 13-effects-as-operators#crossfade-mixes-both-facets
TEST_CASE("org.arbc.crossfade renders a byte-exact audio mix golden at w == 0.5") {
  require_bytes(render_audio_golden(), kAudioMid);
}

// enforces: 13-effects-as-operators#crossfade-mixes-both-facets
TEST_CASE("org.arbc.crossfade w == 0 tile is byte-identical to input 0") {
  CpuBackend backend;
  InlinePull pull;
  SolidContent from{Rgba{0.5F, 0.25F, 0.125F, 1.0F}, Rect{0.0, 0.0, 2.0, 2.0}};
  SolidContent to{Rgba{0.125F, 0.375F, 0.75F, 1.0F}, Rect{0.0, 0.0, 2.0, 2.0}};
  // Window [1000, 2000): at t = 0 the position is w == 0 -> the endpoint
  // pass-through pulls input 0 straight through.
  CrossfadeContent xf{&from, &to, CrossfadeParams{CrossfadeShape::Linear, Time{1000}, Time{1000}}};
  xf.attach(pull, backend);
  const std::vector<std::byte> at_zero = render_crossfade_bytes(xf, backend, 2, 2, Time{0});

  const auto direct = backend.make_surface(2, 2, k_working_rgba32f);
  REQUIRE(direct.has_value());
  const RenderRequest req{
      Rect::from_size(2.0, 2.0), 1.0, Time{0}, StateHandle{}, **direct, Exactness::Exact,
      Deadline::none()};
  auto done = std::make_shared<RenderCompletion>();
  REQUIRE(from.render(req, done).has_value());
  const std::span<const std::byte> direct_bytes = (**direct).cpu_bytes();
  CHECK(std::equal(at_zero.begin(), at_zero.end(), direct_bytes.begin(), direct_bytes.end()));
}

// enforces: 13-effects-as-operators#crossfade-mixes-both-facets
TEST_CASE("org.arbc.crossfade w == 1 tile is byte-identical to input 1") {
  CpuBackend backend;
  InlinePull pull;
  SolidContent from{Rgba{0.5F, 0.25F, 0.125F, 1.0F}, Rect{0.0, 0.0, 2.0, 2.0}};
  SolidContent to{Rgba{0.125F, 0.375F, 0.75F, 1.0F}, Rect{0.0, 0.0, 2.0, 2.0}};
  // Window [1000, 2000): at t = 3000 the position is w == 1 -> the endpoint
  // pass-through pulls input 1 straight through.
  CrossfadeContent xf{&from, &to, CrossfadeParams{CrossfadeShape::Linear, Time{1000}, Time{1000}}};
  xf.attach(pull, backend);
  const std::vector<std::byte> at_one = render_crossfade_bytes(xf, backend, 2, 2, Time{3000});

  const auto direct = backend.make_surface(2, 2, k_working_rgba32f);
  REQUIRE(direct.has_value());
  const RenderRequest req{
      Rect::from_size(2.0, 2.0), 1.0, Time{3000}, StateHandle{}, **direct, Exactness::Exact,
      Deadline::none()};
  auto done = std::make_shared<RenderCompletion>();
  REQUIRE(to.render(req, done).has_value());
  const std::span<const std::byte> direct_bytes = (**direct).cpu_bytes();
  CHECK(std::equal(at_one.begin(), at_one.end(), direct_bytes.begin(), direct_bytes.end()));
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

TEST_CASE("dump crossfade goldens", "[.regen]") {
  dump("kVisualMid", render_visual_golden());
  dump("kAudioMid", render_audio_golden());
}
// GCOV_EXCL_STOP
