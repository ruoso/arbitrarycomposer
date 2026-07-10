// Production runtime binding for org.arbc.fade (operators.fade_runtime_binding).
// Before this seam, no production code called FadeContent::attach -- every attach
// lived in a test double -- so a loaded fade aborted its render/render_audio
// attach assertions (fade_content.cpp:87,209). This test builds a Document with a
// fade layer and drives it through the REAL offline (SequenceRenderer) and export
// (ExportMonitor) drivers, asserting it renders to completion (it would abort
// today), byte-exact to a manually-attached reference, with the identity/pull
// counters and teardown the seam promises.
//
// CROSS-COMPONENT: it drives the real CpuBackend composite to reproduce the frozen
// fade_goldens bytes (byte-exact acceptance), so -- like fade_goldens.t.cpp and
// fade_identity_counter.t.cpp -- it lives in tests/ and links the umbrella `arbc`
// rather than in src/runtime/t/ (a runtime-component test may not include
// backend_cpu, doc 17 / check_levels.py). The runtime driver wiring it exercises
// (SequenceRenderer / ExportMonitor / operator_binding) is the subject under test.

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/compositor/compositor.hpp>
#include <arbc/compositor/counters.hpp>
#include <arbc/compositor/pull_service.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/kind_fade/fade_content.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/kind_tone/tone_content.hpp>
#include <arbc/media/audio_block.hpp>
#include <arbc/media/audio_format.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/export_monitor.hpp>
#include <arbc/runtime/offline_sequence.hpp>
#include <arbc/runtime/operator_binding.hpp>
#include <arbc/surface/backend.hpp>
#include <arbc/surface/surface.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

using namespace arbc;

namespace {

constexpr std::uint32_t k_rate = 48000;
constexpr std::uint32_t k_frames = 16;

std::int64_t flicks_per_frame() {
  return Time::flicks_per_second / static_cast<std::int64_t>(k_rate);
}

// The visual golden fade params (identical scene to tests/fade_goldens.t.cpp): a 2x2
// solid faded in over [0, 1000), so t = 500 is E = 0.5 (every premultiplied channel
// halved).
FadeParams visual_fade_params() {
  return FadeParams{FadeShape::Linear, FadeWindow{Time{0}, Time{1000}}, std::nullopt};
}

// The audio golden fade params: a per-frame ramp over [0, 16*fpf).
FadeParams audio_fade_params() {
  return FadeParams{
      FadeShape::Linear,
      FadeWindow{Time{0}, Time{static_cast<std::int64_t>(k_frames) * flicks_per_frame()}},
      std::nullopt};
}

std::function<ObjectId(const Content*)>
id_map(const std::unordered_map<const Content*, ObjectId>& ids) {
  return [&ids](const Content* c) {
    const auto it = ids.find(c);
    return it != ids.end() ? it->second : ObjectId{};
  };
}

std::vector<std::byte> to_bytes(std::span<const std::byte> s) { return {s.begin(), s.end()}; }

// The MANUALLY-attached reference: render the fade DIRECTLY through a live
// PullServiceImpl (the golden computation, tests/fade_goldens.t.cpp
// render_visual_golden_live, which fade_goldens pins byte-exact to the frozen
// kVisualHalf table). The driver render must reproduce these bytes exactly
// (Constraint 6: binding changes WHO calls attach, never WHAT fade computes).
std::vector<std::byte> render_visual_reference() {
  CpuBackend backend;
  SolidContent solid{Rgba{0.5F, 0.25F, 0.125F, 1.0F}, Rect{0.0, 0.0, 2.0, 2.0}};
  FadeContent fade{&solid, visual_fade_params()};
  TileCache cache(64u * 1024 * 1024);
  const std::unordered_map<const Content*, ObjectId> ids{{&solid, ObjectId{1}}};
  PullConfig config;
  config.id_of = id_map(ids);
  config.contribution = [](const Content*) { return std::uint64_t{1}; };
  PullServiceImpl service(cache, backend, direct_dispatch(), config);
  fade.attach(service, backend);

  const auto target = backend.make_surface(2, 2, k_working_rgba32f);
  REQUIRE(target.has_value());
  const RenderRequest req{
      Rect::from_size(2.0, 2.0), 1.0, Time{500}, StateHandle{}, **target, Exactness::Exact,
      Deadline::none()};
  auto done = std::make_shared<RenderCompletion>();
  const std::optional<RenderResult> r = fade.render(req, done);
  REQUIRE(r.has_value());
  return to_bytes((**target).cpu_bytes());
}

// The manually-attached audio reference (tests/fade_goldens.t.cpp
// render_audio_golden_live, pinned byte-exact to kAudioRamp).
std::vector<std::byte> render_audio_reference() {
  CpuBackend backend;
  ToneContent tone{440, 0.5F};
  FadeContent fade{&tone, audio_fade_params()};
  TileCache cache(64u * 1024 * 1024);
  const std::unordered_map<const Content*, ObjectId> ids{{&tone, ObjectId{1}}};
  PullConfig config;
  config.id_of = id_map(ids);
  config.contribution = [](const Content*) { return std::uint64_t{1}; };
  config.audio_dispatch = direct_audio_dispatch();
  PullServiceImpl service(cache, backend, direct_dispatch(), config);
  fade.attach(service, backend);

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
  const auto* src = reinterpret_cast<const std::byte*>(samples.data());
  bytes.assign(src, src + bytes.size());
  return bytes;
}

// Populate `doc` with a single fade-over-solid layer at the global root (the offline
// driver's render walk). The caller owns both `doc` and `solid` (`Document` is non-
// movable, and the fade borrows `solid` non-owning, so both must outlive the render).
void visual_scene(Document& doc, SolidContent& solid, const FadeParams& params) {
  auto fade = std::make_shared<FadeContent>(&solid, params);
  const ObjectId cid = doc.add_content(fade);
  doc.add_layer(cid, Affine::identity());
}

void require_equal(const std::vector<std::byte>& got, const std::vector<std::byte>& want) {
  REQUIRE(got.size() == want.size());
  REQUIRE(std::memcmp(got.data(), want.data(), want.size()) == 0);
}

} // namespace

// enforces: 16-sdlc-and-quality#byte-exact-goldens
// enforces: 13-effects-as-operators#operator-bound-to-live-services-at-instantiation
// enforces: 13-effects-as-operators#operator-pulls-only-via-pull-service
// enforces: 13-effects-as-operators#fade-attenuates-both-facets
TEST_CASE("org.arbc.fade renders byte-exact through the inline offline driver with production "
          "binding") {
  const std::vector<std::byte> expected = render_visual_reference();

  CpuBackend backend;
  SolidContent solid{Rgba{0.5F, 0.25F, 0.125F, 1.0F}, Rect{0.0, 0.0, 2.0, 2.0}};
  Document doc;
  visual_scene(doc, solid, visual_fade_params());
  // Inline (worker_count == 0): the fade is attached to the driver's live service and
  // renders to completion -- this REQUIRE would fire the attach assertion today.
  SequenceRenderer renderer(doc, Viewport{2, 2, Affine::identity()}, backend);
  const auto frame = renderer.render_frame_at(Time{500});
  REQUIRE(frame.has_value());
  require_equal(to_bytes((**frame).cpu_bytes()), expected);
  // The fade obtained its input through the live PullServiceImpl (a bypass would
  // abort or yield different bytes): the service issued at least the input render.
  CHECK(renderer.counters().requests_issued() >= 1U);
}

// enforces: 16-sdlc-and-quality#byte-exact-goldens
// enforces: 13-effects-as-operators#operator-bound-to-live-services-at-instantiation
TEST_CASE("org.arbc.fade renders byte-exact through the parallel offline driver, race-free") {
  const std::vector<std::byte> expected = render_visual_reference();

  CpuBackend backend;
  SolidContent solid{Rgba{0.5F, 0.25F, 0.125F, 1.0F}, Rect{0.0, 0.0, 2.0, 2.0}};
  Document doc;
  visual_scene(doc, solid, visual_fade_params());
  WorkerPoolConfig pool;
  pool.worker_count = 4;
  // Parallel: the fade is bound once on the driver thread before any worker dispatch
  // and read-only on workers (Constraint 8). Stress a handful of frames so the TSan
  // lane exercises the bind/dispatch/reap path, then assert byte-exactness at t=500.
  SequenceRenderer renderer(doc, Viewport{2, 2, Affine::identity()}, backend, pool);
  for (const std::int64_t t : {std::int64_t{0}, std::int64_t{250}, std::int64_t{750}}) {
    const auto stress = renderer.render_frame_at(Time{t});
    REQUIRE(stress.has_value());
  }
  const auto frame = renderer.render_frame_at(Time{500});
  REQUIRE(frame.has_value());
  require_equal(to_bytes((**frame).cpu_bytes()), expected);
}

// enforces: 16-sdlc-and-quality#byte-exact-goldens
// enforces: 13-effects-as-operators#operator-bound-to-live-services-at-instantiation
// enforces: 13-effects-as-operators#operator-pulls-only-via-pull-service
// enforces: 13-effects-as-operators#fade-attenuates-both-facets
TEST_CASE("org.arbc.fade audio renders byte-exact through the export monitor with production "
          "binding") {
  const std::vector<std::byte> expected = render_audio_reference();

  ToneContent tone{440, 0.5F};
  auto fade = std::make_shared<FadeContent>(&tone, audio_fade_params());
  Document doc;
  const ObjectId comp = doc.add_composition(0.0, 0.0);
  const ObjectId cid = doc.add_content(fade);
  const ObjectId layer = doc.add_layer(cid, Affine::identity());
  doc.attach_layer(comp, layer);

  // The ExportMonitor binds the fade to its live audio pull at construction; mixing
  // one block would abort (fade_content.cpp:209) without the production wiring.
  ExportMonitor monitor(doc, comp, AudioFormat{k_rate, ChannelLayout::Stereo});
  std::vector<float> samples(static_cast<std::size_t>(k_frames) * 2U, 0.0F);
  AudioBlock block{samples.data(), k_frames, ChannelLayout::Stereo, k_rate};
  (void)monitor.render_block_at(
      TimeRange{Time::zero(), Time{static_cast<std::int64_t>(k_frames) * flicks_per_frame()}},
      block);
  std::vector<std::byte> got(samples.size() * sizeof(float));
  std::memcpy(got.data(), samples.data(), got.size());
  require_equal(got, expected);
  CHECK(monitor.counters().audio_dispatches() >= 1U);
}

// enforces: 13-effects-as-operators#identity-plan-issues-no-operator-render
// enforces: 13-effects-as-operators#fade-identity-at-open-envelope
TEST_CASE("through the offline driver a fully-open fade issues zero operator renders and a "
          "mid-fade exactly one") {
  CpuBackend backend;
  SolidContent solid{Rgba{0.5F, 0.25F, 0.125F, 1.0F}, Rect{0.0, 0.0, 2.0, 2.0}};

  SECTION("fully-open envelope: identity short-circuit, zero operator renders") {
    Document doc;
    visual_scene(doc, solid, FadeParams{}); // no windows -> E == 1 everywhere
    SequenceRenderer renderer(doc, Viewport{2, 2, Affine::identity()}, backend);
    const auto frame = renderer.render_frame_at(Time{500});
    REQUIRE(frame.has_value());
    CHECK(renderer.counters().operator_renders() == 0U);
  }

  SECTION("mid-fade: non-identity, exactly one operator render") {
    Document doc;
    visual_scene(doc, solid, visual_fade_params());
    SequenceRenderer renderer(doc, Viewport{2, 2, Affine::identity()}, backend);
    const auto frame = renderer.render_frame_at(Time{500});
    REQUIRE(frame.has_value());
    CHECK(renderer.counters().operator_renders() == 1U);
  }
}

// enforces: 13-effects-as-operators#fade-timed-over-static
TEST_CASE("org.arbc.fade renders differently at different times over a static solid through the "
          "driver") {
  CpuBackend backend;
  SolidContent solid{Rgba{0.5F, 0.25F, 0.125F, 1.0F}, Rect{0.0, 0.0, 2.0, 2.0}};
  Document doc;
  visual_scene(doc, solid, visual_fade_params());
  SequenceRenderer renderer(doc, Viewport{2, 2, Affine::identity()}, backend);
  const auto early = renderer.render_frame_at(Time{250}); // E = 0.25
  const auto late = renderer.render_frame_at(Time{750});  // E = 0.75
  REQUIRE(early.has_value());
  REQUIRE(late.has_value());
  const std::span<const std::byte> be = (**early).cpu_bytes();
  const std::span<const std::byte> bl = (**late).cpu_bytes();
  REQUIRE(be.size() == bl.size());
  // Timed even over a static input: the time-varying envelope yields distinct bytes.
  CHECK(std::memcmp(be.data(), bl.data(), be.size()) != 0);
}

// enforces: 13-effects-as-operators#operator-bound-to-live-services-at-instantiation
TEST_CASE("the runtime binder clears a fade's borrowed services on release") {
  CpuBackend backend;
  TileCache cache(64u * 1024 * 1024);
  PullServiceImpl service(cache, backend, direct_dispatch(), PullConfig{});
  SolidContent solid{Rgba{0.5F, 0.25F, 0.125F, 1.0F}, Rect{0.0, 0.0, 2.0, 2.0}};
  auto fade = std::make_shared<FadeContent>(&solid, FadeParams{});
  Document doc;
  doc.add_content(fade);
  register_builtin_operator_binders();

  CHECK_FALSE(fade->attached());
  {
    const OperatorBindingScope scope = bind_operators(doc, service, backend);
    CHECK(scope.size() == 1U);
    CHECK(fade->attached());
  }
  // Released: the borrowed pointers are cleared so no render after release
  // dereferences the (now out-of-scope) service (Constraint 3).
  CHECK_FALSE(fade->attached());
  // Re-bindable through the same seam after release (Constraint 4).
  {
    const OperatorBindingScope scope = bind_operators(doc, service, backend);
    CHECK(fade->attached());
  }
  CHECK_FALSE(fade->attached());
}
