// Production runtime binding for org.arbc.crossfade
// (operators.crossfade_runtime_binding). Before this seam, no production code
// called CrossfadeContent::attach -- every attach lived in a test double -- so a
// loaded crossfade aborted its render/render_audio attach assertions
// (crossfade_content.cpp:163,272). This test builds a Document with a crossfade over
// two inputs, binds it through register_builtin_operator_binders() + bind_operators
// (the seam this task registers into), and drives it through the REAL export
// (ExportMonitor) audio path and the offline (SequenceRenderer) identity path,
// asserting it renders with no manual attach and tears the binding down on release.
//
// SCOPE NOTE (byte-exact VISUAL end-to-end deferred): the visual byte-exact offline
// acceptance (interior dissolve reproduced through the SequenceRenderer, inline +
// parallel/TSan) is BLOCKED by a latent offline-driver gap this task surfaced -- the
// driver's cache-identity map (offline_sequence.cpp:87-104 / export_monitor.cpp:122)
// assigns an ObjectId only to LAYER-root content, so a multi-input operator's two
// same-stability child inputs both key on ObjectId{} and collide on one visual
// TileKey (pull_service.cpp:175-194); input 1's pull is served input 0's tile, so an
// interior crossfade renders input-0-only. FadeContent (single input) never collides.
// The audio path is unaffected (pull_audio is cache-gated on config.blocks, which the
// export monitor leaves null, so each tone renders fresh -- pull_service.cpp:372). The
// fix (distinct cache identity for operator input children) is a driver change outside
// this 0.5d wiring task's scope (Constraint 7) -- see follow-up
// runtime.operator_input_cache_identity. The visual byte-exact / parallel-TSan
// acceptance lands with that task.
//
// CROSS-COMPONENT: it drives the real CpuBackend / kind_crossfade / runtime drivers,
// so per doc 17 levelization it lives in tests/ and links the umbrella `arbc` rather
// than in src/runtime/t/ -- exactly as crossfade_goldens.t.cpp does.

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/compositor/compositor.hpp>
#include <arbc/compositor/counters.hpp>
#include <arbc/compositor/pull_service.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/kind_crossfade/crossfade_content.hpp>
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

// The visual crossfade params (identical scene to tests/crossfade_goldens.t.cpp): a
// dissolve over the window [0, 1000), so t = 500 is w == 0.5.
CrossfadeParams visual_crossfade_params() {
  return CrossfadeParams{CrossfadeShape::Linear, Time{0}, Time{1000}};
}

// The audio golden crossfade params: the window spans [0, 2s), so a block starting at
// 1s sits at w == 0.5 exactly -- the complementary-weight crossover.
CrossfadeParams audio_crossfade_params() {
  return CrossfadeParams{CrossfadeShape::Linear, Time{0}, Time{2 * Time::flicks_per_second}};
}

std::function<ObjectId(const Content*)>
id_map(const std::unordered_map<const Content*, ObjectId>& ids) {
  return [ids](const Content* c) {
    const auto it = ids.find(c);
    return it != ids.end() ? it->second : ObjectId{};
  };
}

// The manually-attached audio reference (tests/crossfade_goldens.t.cpp
// render_audio_golden, pinned byte-exact to kAudioMid) re-run through a live
// PullServiceImpl: two distinct tones crossfaded at w == 0.5.
std::vector<std::byte> render_audio_reference() {
  CpuBackend backend;
  ToneContent from{440, 0.5F};
  ToneContent to{660, 0.25F};
  CrossfadeContent xf{&from, &to, audio_crossfade_params()};
  TileCache cache(64u * 1024 * 1024);
  const std::unordered_map<const Content*, ObjectId> ids{{&from, ObjectId{1}}, {&to, ObjectId{2}}};
  PullConfig config;
  config.id_of = id_map(ids);
  config.contribution = [](const Content*) { return std::uint64_t{1}; };
  config.audio_dispatch = direct_audio_dispatch();
  PullServiceImpl service(cache, backend, direct_dispatch(), config);
  xf.attach(service, backend);

  AudioFacet* af = xf.audio();
  REQUIRE(af != nullptr);
  std::vector<float> samples(static_cast<std::size_t>(k_frames) * 2U, 0.0F);
  AudioBlock block{samples.data(), k_frames, ChannelLayout::Stereo, k_rate};
  const AudioRequest req{TimeRange{Time{Time::flicks_per_second},
                                   Time{Time::flicks_per_second +
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
  const auto* src = reinterpret_cast<const std::byte*>(samples.data());
  bytes.assign(src, src + bytes.size());
  return bytes;
}

// Populate `doc` with a single crossfade-over-two-solids layer at the global root (the
// offline driver's render walk). The caller owns `doc` and both solids (`Document` is
// non-movable, and the crossfade borrows both non-owning, so all three must outlive the
// render).
void visual_scene(Document& doc, SolidContent& from, SolidContent& to,
                  const CrossfadeParams& params) {
  auto xf = std::make_shared<CrossfadeContent>(&from, &to, params);
  const ObjectId cid = doc.add_content(xf);
  doc.add_layer(cid, Affine::identity());
}

void require_equal(const std::vector<std::byte>& got, const std::vector<std::byte>& want) {
  REQUIRE(got.size() == want.size());
  REQUIRE(std::memcmp(got.data(), want.data(), want.size()) == 0);
}

} // namespace

// enforces: 16-sdlc-and-quality#byte-exact-goldens
// enforces: 13-effects-as-operators#crossfade-bound-to-live-services-at-instantiation
// enforces: 13-effects-as-operators#operator-pulls-only-via-pull-service
// enforces: 13-effects-as-operators#crossfade-mixes-both-facets
TEST_CASE("org.arbc.crossfade audio renders byte-exact through the export monitor with "
          "production binding") {
  const std::vector<std::byte> expected = render_audio_reference();

  ToneContent from{440, 0.5F};
  ToneContent to{660, 0.25F};
  auto xf = std::make_shared<CrossfadeContent>(&from, &to, audio_crossfade_params());
  Document doc;
  const ObjectId comp = doc.add_composition(0.0, 0.0);
  const ObjectId cid = doc.add_content(xf);
  const ObjectId layer = doc.add_layer(cid, Affine::identity());
  doc.attach_layer(comp, layer);

  // The ExportMonitor binds the crossfade to its live audio pull at construction via the
  // runtime binder this task registers; mixing one block would abort
  // (crossfade_content.cpp:272) without the production wiring. The block starts at 1s so
  // it sits at w == 0.5 (the crossover), matching the golden.
  ExportMonitor monitor(doc, comp, AudioFormat{k_rate, ChannelLayout::Stereo});
  std::vector<float> samples(static_cast<std::size_t>(k_frames) * 2U, 0.0F);
  AudioBlock block{samples.data(), k_frames, ChannelLayout::Stereo, k_rate};
  (void)monitor.render_block_at(
      TimeRange{
          Time{Time::flicks_per_second},
          Time{Time::flicks_per_second + static_cast<std::int64_t>(k_frames) * flicks_per_frame()}},
      block);
  std::vector<std::byte> got(samples.size() * sizeof(float));
  std::memcpy(got.data(), samples.data(), got.size());
  require_equal(got, expected);
  // Both tones were dispatched through the live audio pull (two inputs, cold).
  CHECK(monitor.counters().audio_dispatches() >= 2U);
}

// enforces: 13-effects-as-operators#identity-plan-issues-no-operator-render
// enforces: 13-effects-as-operators#crossfade-identity-at-endpoints
TEST_CASE("through the offline driver a crossfade at each endpoint issues zero operator renders "
          "and an interior one exactly one") {
  CpuBackend backend;
  SolidContent from{Rgba{0.5F, 0.25F, 0.125F, 1.0F}, Rect{0.0, 0.0, 2.0, 2.0}};
  SolidContent to{Rgba{0.125F, 0.375F, 0.75F, 1.0F}, Rect{0.0, 0.0, 2.0, 2.0}};
  // Window [1000, 2000): t = 500 is w == 0 (input 0), t = 3000 is w == 1 (input 1), and
  // t = 1500 is w == 0.5 (interior). The crossfade is bound to the driver's live service
  // by the runtime binder; without it the interior render at t=1500 would abort.
  const CrossfadeParams params{CrossfadeShape::Linear, Time{1000}, Time{1000}};

  SECTION("w == 0 endpoint: identity short-circuit, zero operator renders") {
    Document doc;
    visual_scene(doc, from, to, params);
    SequenceRenderer renderer(doc, Viewport{2, 2, Affine::identity()}, backend);
    const auto frame = renderer.render_frame_at(Time{500});
    REQUIRE(frame.has_value());
    CHECK(renderer.counters().operator_renders() == 0U);
  }

  SECTION("w == 1 endpoint: identity short-circuit, zero operator renders") {
    Document doc;
    visual_scene(doc, from, to, params);
    SequenceRenderer renderer(doc, Viewport{2, 2, Affine::identity()}, backend);
    const auto frame = renderer.render_frame_at(Time{3000});
    REQUIRE(frame.has_value());
    CHECK(renderer.counters().operator_renders() == 0U);
  }

  SECTION("interior w: non-identity, exactly one operator render") {
    Document doc;
    visual_scene(doc, from, to, params);
    SequenceRenderer renderer(doc, Viewport{2, 2, Affine::identity()}, backend);
    const auto frame = renderer.render_frame_at(Time{1500});
    REQUIRE(frame.has_value());
    CHECK(renderer.counters().operator_renders() == 1U);
  }
}

// enforces: 13-effects-as-operators#crossfade-bound-to-live-services-at-instantiation
TEST_CASE("the runtime binder clears a crossfade's borrowed services on release") {
  CpuBackend backend;
  TileCache cache(64u * 1024 * 1024);
  PullServiceImpl service(cache, backend, direct_dispatch(), PullConfig{});
  SolidContent from{Rgba{0.5F, 0.25F, 0.125F, 1.0F}, Rect{0.0, 0.0, 2.0, 2.0}};
  SolidContent to{Rgba{0.125F, 0.375F, 0.75F, 1.0F}, Rect{0.0, 0.0, 2.0, 2.0}};
  auto xf = std::make_shared<CrossfadeContent>(&from, &to, visual_crossfade_params());
  Document doc;
  doc.add_content(xf);
  register_builtin_operator_binders();

  CHECK_FALSE(xf->attached());
  {
    const OperatorBindingScope scope = bind_operators(doc, service, backend);
    CHECK(scope.size() == 1U);
    CHECK(xf->attached());
  }
  // Released: the borrowed pointers are cleared so no render after release
  // dereferences the (now out-of-scope) service (Constraint 3).
  CHECK_FALSE(xf->attached());
  // Re-bindable through the same seam after release (Constraint 4).
  {
    const OperatorBindingScope scope = bind_operators(doc, service, backend);
    CHECK(xf->attached());
  }
  CHECK_FALSE(xf->attached());
}
