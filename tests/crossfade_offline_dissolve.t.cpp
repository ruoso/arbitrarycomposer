// The visual byte-exact + parallel/TSan + distinct-input audio acceptance that
// operators.crossfade_runtime_binding DEFERRED to runtime.operator_input_cache_identity.
// crossfade_runtime_binding could bind a crossfade to the live driver services but
// could NOT freeze a visual golden through the real offline driver, because the
// offline/export `id_of` gave both same-stability input children the default
// `ObjectId{}` -- they collided on one visual TileKey (and audio BlockKey), so the
// interior dissolve rendered (1-w)*a + w*a = a instead of (1-w)*a + w*b. With the
// child-distinct `id_of` (src/runtime/pull_identity.cpp) in place, a multi-input
// operator renders byte-exact end to end. This file drives the REAL SequenceRenderer
// (inline + parallel) and ExportMonitor over two DISTINCT inputs and pins the blend
// byte-exact against a manually-attached reference.
//
// CROSS-COMPONENT: it drives the real CpuBackend composite, so -- like
// crossfade_goldens.t.cpp / fade_runtime_binding.t.cpp -- it lives in tests/ and
// links the umbrella `arbc` rather than in src/runtime/t/ (a runtime-component test
// may not include backend_cpu, doc 17 / check_levels.py). The same registrations run
// under the tsan preset, which is the parallel/TSan lane (Constraint 7): the identity
// map is written once on the driver thread before any worker dispatch and read-only
// on workers, so the parallel dissolve is race-free.

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
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

// The visual dissolve scene (identical inputs to tests/crossfade_goldens.t.cpp): a
// red-ish over a blue-ish 2x2 solid, window [0, 1000), so t = 500 is w == 0.5.
CrossfadeParams visual_params() {
  return CrossfadeParams{CrossfadeShape::Linear, Time{0}, Time{1000}};
}

Rgba from_color() { return Rgba{0.5F, 0.25F, 0.125F, 1.0F}; }
Rgba to_color() { return Rgba{0.125F, 0.375F, 0.75F, 1.0F}; }

std::function<ObjectId(const Content*)>
id_map(const std::unordered_map<const Content*, ObjectId>& ids) {
  return [&ids](const Content* c) {
    const auto it = ids.find(c);
    return it != ids.end() ? it->second : ObjectId{};
  };
}

std::vector<std::byte> to_bytes(std::span<const std::byte> s) { return {s.begin(), s.end()}; }

void require_equal(const std::vector<std::byte>& got, const std::vector<std::byte>& want) {
  REQUIRE(got.size() == want.size());
  REQUIRE(std::memcmp(got.data(), want.data(), want.size()) == 0);
}

// The MANUALLY-attached visual reference: render the crossfade DIRECTLY through a
// live PullServiceImpl with the two inputs mapped to DISTINCT ids (the correct
// dissolve, tests/crossfade_goldens.t.cpp render_visual_golden_live, pinned byte-exact
// to kVisualMid at the interior; the endpoint w==0/w==1 pass-through pins byte-exact to
// input 0 / input 1 there). The driver render must reproduce these bytes exactly once
// the driver's own `id_of` gives the children distinct ids too -- so this reference is
// the oracle for both the interior dissolve and the endpoint short-circuit.
std::vector<std::byte> render_crossfade_reference(const CrossfadeParams& params, Time time) {
  CpuBackend backend;
  SolidContent from{from_color(), Rect{0.0, 0.0, 2.0, 2.0}};
  SolidContent to{to_color(), Rect{0.0, 0.0, 2.0, 2.0}};
  CrossfadeContent xf{&from, &to, params};
  TileCache cache(64u * 1024 * 1024);
  const std::unordered_map<const Content*, ObjectId> ids{{&from, ObjectId{1}}, {&to, ObjectId{2}}};
  PullConfig config;
  config.id_of = id_map(ids);
  config.contribution = [](const Content*) { return std::uint64_t{1}; };
  PullServiceImpl service(cache, backend, direct_dispatch(), config);
  xf.attach(service, backend);

  const auto target = backend.make_surface(2, 2, k_working_rgba32f);
  REQUIRE(target.has_value());
  const RenderRequest req{
      Rect::from_size(2.0, 2.0), 1.0, time, StateHandle{}, **target, Exactness::Exact,
      Deadline::none()};
  auto done = std::make_shared<RenderCompletion>();
  const std::optional<RenderResult> r = xf.render(req, done);
  REQUIRE(r.has_value());
  return to_bytes((**target).cpu_bytes());
}

std::vector<std::byte> render_visual_reference() {
  return render_crossfade_reference(visual_params(), Time{500});
}

// Populate `doc` with a single crossfade-over-two-solids layer at the global root (the
// offline driver's render walk). The caller owns `doc` and both solids (the crossfade
// borrows them non-owning, so all must outlive the render).
void visual_scene(Document& doc, SolidContent& from, SolidContent& to,
                  const CrossfadeParams& params) {
  auto xf = std::make_shared<CrossfadeContent>(&from, &to, params);
  const ObjectId cid = doc.add_content(xf);
  doc.add_layer(cid, Affine::identity());
}

// The MANUALLY-attached audio reference: render the crossfade's audio DIRECTLY through
// an inline pull (each input rendered on demand, no shared block cache, so no possible
// collision) -- the correct complementary-weight mix of two DISTINCT tones.
class InlineAudioPull final : public PullService {
public:
  void pull(ContentRef, const RenderRequest&, std::shared_ptr<RenderCompletion> done) override {
    done->fail(RenderError::ContentFailed);
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

std::vector<std::byte> render_audio_reference(Time window_start) {
  CpuBackend backend;
  InlineAudioPull pull;
  ToneContent from{440, 0.5F};
  ToneContent to{660, 0.25F};
  const std::int64_t two_seconds = 2 * Time::flicks_per_second;
  CrossfadeContent xf{&from, &to,
                      CrossfadeParams{CrossfadeShape::Linear, Time{0}, Time{two_seconds}}};
  xf.attach(pull, backend);

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

} // namespace

// enforces: 16-sdlc-and-quality#byte-exact-goldens
// enforces: 13-effects-as-operators#crossfade-bound-to-live-services-at-instantiation
// enforces: 13-effects-as-operators#operator-pulls-only-via-pull-service
// enforces: 13-effects-as-operators#crossfade-mixes-both-facets
// enforces: 13-effects-as-operators#operator-input-children-have-distinct-cache-identity
TEST_CASE("org.arbc.crossfade renders a byte-exact interior dissolve through the inline offline "
          "driver") {
  const std::vector<std::byte> expected = render_visual_reference();

  CpuBackend backend;
  SolidContent from{from_color(), Rect{0.0, 0.0, 2.0, 2.0}};
  SolidContent to{to_color(), Rect{0.0, 0.0, 2.0, 2.0}};
  Document doc;
  visual_scene(doc, from, to, visual_params());
  // Inline (worker_count == 0): the crossfade is attached to the driver's live service
  // and both inputs are keyed distinctly by the driver's `id_of`, so the dissolve is
  // (1-w)*a + w*b -- byte-exact to the reference. Pre-fix the two inputs aliased and
  // this produced input 0 duplicated.
  SequenceRenderer renderer(doc, Viewport{2, 2, Affine::identity()}, backend);
  const auto frame = renderer.render_frame_at(Time{500});
  REQUIRE(frame.has_value());
  require_equal(to_bytes((**frame).cpu_bytes()), expected);
  // Both inputs pulled cold through the live service: two input renders plus the one
  // operator render (a collision would serve input 1 from input 0's tile -> one).
  CHECK(renderer.counters().requests_issued() - renderer.counters().operator_renders() == 2U);
}

// enforces: 16-sdlc-and-quality#byte-exact-goldens
// enforces: 13-effects-as-operators#crossfade-bound-to-live-services-at-instantiation
// enforces: 13-effects-as-operators#operator-input-children-have-distinct-cache-identity
TEST_CASE("org.arbc.crossfade renders a byte-exact interior dissolve through the parallel offline "
          "driver, race-free") {
  const std::vector<std::byte> expected = render_visual_reference();

  CpuBackend backend;
  SolidContent from{from_color(), Rect{0.0, 0.0, 2.0, 2.0}};
  SolidContent to{to_color(), Rect{0.0, 0.0, 2.0, 2.0}};
  Document doc;
  visual_scene(doc, from, to, visual_params());
  WorkerPoolConfig pool;
  pool.worker_count = 4;
  // Parallel: the identity map is built once on the driver thread before any worker
  // dispatch and read-only on workers (Constraint 7). Stress a handful of frames so
  // the TSan lane exercises the bind/dispatch/reap path, then assert byte-exactness.
  SequenceRenderer renderer(doc, Viewport{2, 2, Affine::identity()}, backend, pool);
  for (const std::int64_t t : {std::int64_t{0}, std::int64_t{250}, std::int64_t{750}}) {
    const auto stress = renderer.render_frame_at(Time{t});
    REQUIRE(stress.has_value());
  }
  const auto frame = renderer.render_frame_at(Time{500});
  REQUIRE(frame.has_value());
  require_equal(to_bytes((**frame).cpu_bytes()), expected);
}

// enforces: 13-effects-as-operators#crossfade-identity-at-endpoints
// enforces: 13-effects-as-operators#identity-plan-issues-no-operator-render
// enforces: 13-effects-as-operators#operator-input-children-have-distinct-cache-identity
TEST_CASE("org.arbc.crossfade endpoint pass-through hits the identity short-circuit through the "
          "offline driver") {
  CpuBackend backend;
  // Window [1000, 2000): at t == 0 the position is w == 0 (serve input 0); at t == 3000
  // it is w == 1 (serve input 1). The identity short-circuit serves an input directly
  // with no new cache entry (doc 13:60-65) and allocates NO child key, so the
  // child-distinct `id_of` must not perturb it: the observable proof through the driver
  // is that the endpoint issues zero operator renders (Constraint 6). The endpoint
  // pixel bytes are pinned at the content level by tests/crossfade_goldens.t.cpp;
  // delivering the identity-served input child to the offline frame is a separate
  // compositor delivery concern (see runtime.operator_identity_offline_delivery
  // follow-up), out of this cache-identity task's scope.
  const CrossfadeParams endpoints{CrossfadeShape::Linear, Time{1000}, Time{1000}};

  SECTION("w == 0 serves input 0: zero operator renders") {
    SolidContent from{from_color(), Rect{0.0, 0.0, 2.0, 2.0}};
    SolidContent to{to_color(), Rect{0.0, 0.0, 2.0, 2.0}};
    Document doc;
    visual_scene(doc, from, to, endpoints);
    SequenceRenderer renderer(doc, Viewport{2, 2, Affine::identity()}, backend);
    const auto frame = renderer.render_frame_at(Time{0});
    REQUIRE(frame.has_value());
    CHECK(renderer.counters().operator_renders() == 0U);
  }

  SECTION("w == 1 serves input 1: zero operator renders") {
    SolidContent from{from_color(), Rect{0.0, 0.0, 2.0, 2.0}};
    SolidContent to{to_color(), Rect{0.0, 0.0, 2.0, 2.0}};
    Document doc;
    visual_scene(doc, from, to, endpoints);
    SequenceRenderer renderer(doc, Viewport{2, 2, Affine::identity()}, backend);
    const auto frame = renderer.render_frame_at(Time{3000});
    REQUIRE(frame.has_value());
    CHECK(renderer.counters().operator_renders() == 0U);
  }
}

// enforces: 11-time-and-video#static-tiles-survive-clock
// enforces: 11-time-and-video#achieved-time-coalescing-issues-zero-renders
// enforces: 13-effects-as-operators#operator-input-children-have-distinct-cache-identity
TEST_CASE("a crossfade over two static solids re-renders neither input after the first frame") {
  CpuBackend backend;
  SolidContent from{from_color(), Rect{0.0, 0.0, 2.0, 2.0}};
  SolidContent to{to_color(), Rect{0.0, 0.0, 2.0, 2.0}};
  Document doc;
  visual_scene(doc, from, to, visual_params());
  SequenceRenderer renderer(doc, Viewport{2, 2, Affine::identity()}, backend);

  // First interior frame: each Static input renders exactly once (input-render count =
  // requests_issued - operator_renders). Two distinct ids => 2; a collision would be 1.
  const auto f0 = renderer.render_frame_at(Time{400}); // w == 0.4
  REQUIRE(f0.has_value());
  const std::uint64_t inputs_after_first =
      renderer.counters().requests_issued() - renderer.counters().operator_renders();
  CHECK(inputs_after_first == 2U);

  // The playback clock advances but the Static inputs' achieved time coalesces, so
  // their tiles survive the clock: subsequent interior frames re-render zero inputs
  // (the synthesized child ids are stable across frames, so the input keys hit).
  for (const std::int64_t t : {std::int64_t{600}, std::int64_t{800}}) {
    const auto f = renderer.render_frame_at(Time{t});
    REQUIRE(f.has_value());
  }
  const std::uint64_t inputs_after_more =
      renderer.counters().requests_issued() - renderer.counters().operator_renders();
  CHECK(inputs_after_more == inputs_after_first);
}

// enforces: 16-sdlc-and-quality#byte-exact-goldens
// enforces: 13-effects-as-operators#crossfade-mixes-both-facets
// enforces: 13-effects-as-operators#operator-input-children-have-distinct-cache-identity
TEST_CASE("org.arbc.crossfade audio mixes two distinct tones byte-exact through the export "
          "monitor") {
  // The block at 1s over a [0, 2s) crossfade window is w == 0.5: the complementary
  // mix s0*(1-w) + s1*w of two DISTINCT tones. Pre-fix both tones keyed on `ObjectId{}`
  // and the second tone's block was served the first's -- input 0 duplicated. The
  // reference renders the same block inline (no shared cache), so the export mix must
  // match it byte-exact once the BlockKey ids are distinct.
  const Time block_start{Time::flicks_per_second};
  const std::vector<std::byte> expected = render_audio_reference(block_start);

  auto from = std::make_shared<ToneContent>(440, 0.5F);
  auto to = std::make_shared<ToneContent>(660, 0.25F);
  const std::int64_t two_seconds = 2 * Time::flicks_per_second;
  auto xf = std::make_shared<CrossfadeContent>(
      from.get(), to.get(), CrossfadeParams{CrossfadeShape::Linear, Time{0}, Time{two_seconds}});
  Document doc;
  const ObjectId comp = doc.add_composition(0.0, 0.0);
  const ObjectId cid = doc.add_content(xf);
  const ObjectId layer = doc.add_layer(cid, Affine::identity());
  doc.attach_layer(comp, layer);

  ExportMonitor monitor(doc, comp, AudioFormat{k_rate, ChannelLayout::Stereo});
  std::vector<float> samples(static_cast<std::size_t>(k_frames) * 2U, 0.0F);
  AudioBlock block{samples.data(), k_frames, ChannelLayout::Stereo, k_rate};
  (void)monitor.render_block_at(
      TimeRange{block_start, Time{block_start.flicks +
                                  static_cast<std::int64_t>(k_frames) * flicks_per_frame()}},
      block);
  std::vector<std::byte> got(samples.size() * sizeof(float));
  std::memcpy(got.data(), samples.data(), got.size());
  require_equal(got, expected);
}
