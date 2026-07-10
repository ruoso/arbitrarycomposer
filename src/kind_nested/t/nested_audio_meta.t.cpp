#include <arbc/base/geometry.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/kind_nested/nested_content.hpp>
#include <arbc/media/audio_block.hpp>
#include <arbc/model/model.hpp>
#include <arbc/surface/backend.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <unordered_map>

// Behavioral unit tests for org.arbc.nested's aggregated + memoized AUDIO metadata
// (doc 12:202-208, constraint 4). Lives in the kind's own `t/` (links only
// `contract`), so it uses local Content / AudioFacet doubles and drives the attach
// seam directly -- the byte-exact goldens, behavioral counters, and conformance
// families that need `arbc-testing` / `kind_tone` live in top-level `tests/`.

namespace {

using namespace arbc;

// A leaf content exposing a configurable AudioFacet (Static/Timed + optional
// extent). Its `render_audio` is a trivial silent fill -- the metadata tests never
// mix, only aggregate the description methods.
class StubAudioLeaf final : public Content {
public:
  StubAudioLeaf(Stability audio_stability, std::optional<TimeRange> audio_extent)
      : d_facet(audio_stability, audio_extent) {}

  std::optional<Rect> bounds() const override { return Rect{}; }
  Stability stability() const override { return Stability::Static; }
  std::optional<TimeRange> time_extent() const override { return std::nullopt; }
  std::optional<RenderResult> render(const RenderRequest& request,
                                     std::shared_ptr<RenderCompletion>) override {
    return RenderResult{request.scale, true};
  }
  AudioFacet* audio() override { return &d_facet; }

private:
  class Facet final : public AudioFacet {
  public:
    Facet(Stability stability, std::optional<TimeRange> extent)
        : d_stability(stability), d_extent(extent) {}
    std::optional<TimeRange> audio_extent() const override { return d_extent; }
    Stability audio_stability() const override { return d_stability; }
    std::optional<AudioResult> render_audio(const AudioRequest& request,
                                            std::shared_ptr<AudioCompletion>) override {
      const std::uint32_t ch = channel_count(request.layout);
      const std::size_t n = static_cast<std::size_t>(request.target.frames) * ch;
      for (std::size_t i = 0; i < n; ++i) {
        request.target.samples[i] = 0.0F;
      }
      return AudioResult{request.sample_rate, true};
    }

  private:
    Stability d_stability;
    std::optional<TimeRange> d_extent;
  };

  Facet d_facet;
};

// A purely visual leaf (no audio facet), to prove audio-less layers do not
// contribute to the audio metadata.
class VisualLeaf final : public Content {
public:
  std::optional<Rect> bounds() const override { return Rect{0.0, 0.0, 2.0, 2.0}; }
  Stability stability() const override { return Stability::Static; }
  std::optional<TimeRange> time_extent() const override { return std::nullopt; }
  std::optional<RenderResult> render(const RenderRequest& request,
                                     std::shared_ptr<RenderCompletion>) override {
    return RenderResult{request.scale, true};
  }
};

// A leaf whose audio facet fills every sample with a constant value, optionally
// reporting a forced below-request achieved rate (marked inexact) -- to exercise
// the mix, the layout remix, and the baseline below-rate honesty branch.
class ConstAudioLeaf final : public Content {
public:
  explicit ConstAudioLeaf(float value, std::optional<std::uint32_t> forced_rate = std::nullopt)
      : d_facet(value, forced_rate) {}
  std::optional<Rect> bounds() const override { return Rect{}; }
  Stability stability() const override { return Stability::Static; }
  std::optional<TimeRange> time_extent() const override { return std::nullopt; }
  std::optional<RenderResult> render(const RenderRequest& request,
                                     std::shared_ptr<RenderCompletion>) override {
    return RenderResult{request.scale, true};
  }
  AudioFacet* audio() override { return &d_facet; }

private:
  class Facet final : public AudioFacet {
  public:
    Facet(float value, std::optional<std::uint32_t> forced_rate)
        : d_value(value), d_forced_rate(forced_rate) {}
    std::optional<TimeRange> audio_extent() const override { return std::nullopt; }
    Stability audio_stability() const override { return Stability::Static; }
    std::optional<AudioResult> render_audio(const AudioRequest& request,
                                            std::shared_ptr<AudioCompletion>) override {
      const std::uint32_t ch = channel_count(request.layout);
      const std::size_t n = static_cast<std::size_t>(request.target.frames) * ch;
      for (std::size_t i = 0; i < n; ++i) {
        request.target.samples[i] = d_value;
      }
      const std::uint32_t achieved = d_forced_rate.value_or(request.sample_rate);
      return AudioResult{achieved, !d_forced_rate.has_value()};
    }

  private:
    float d_value;
    std::optional<std::uint32_t> d_forced_rate;
  };
  Facet d_facet;
};

// A pull double that either routes `pull_audio` to the input's facet (Inline) or
// leaves the completion unsettled (Miss, an async dispatch) so nested shows the
// silent placeholder for the layer.
class RoutingAudioPull final : public PullService {
public:
  enum class Mode { Inline, Miss };
  explicit RoutingAudioPull(Mode mode = Mode::Inline) : d_mode(mode) {}
  void pull(ContentRef, const RenderRequest&, std::shared_ptr<RenderCompletion> done) override {
    done->fail(RenderError::ContentFailed);
  }
  void pull_audio(ContentRef input, const AudioRequest& request,
                  std::shared_ptr<AudioCompletion> done) override {
    if (d_mode == Mode::Miss) {
      return; // dispatched to a worker: not settled inline
    }
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

private:
  Mode d_mode;
};

class NullBackend final : public Backend {
public:
  BackendCaps capabilities() const override { return {}; }
  expected<std::unique_ptr<Surface>, SurfaceError> make_surface(int, int, SurfaceFormat) override {
    return arbc::unexpected<SurfaceError>(SurfaceError::UnsupportedFormat);
  }
  void clear(Surface&, float, float, float, float) override {}
  void composite(Surface&, const Surface&, const Affine&, double) override {}
  void downsample(Surface&, const Surface&) override {}
};

class NullPull final : public PullService {
public:
  void pull(ContentRef, const RenderRequest&, std::shared_ptr<RenderCompletion>) override {}
};

struct Scene {
  Model model;
  std::unordered_map<ObjectId, Content*> binding;
  ObjectId comp{};
  ObjectId layer_a{};
  ObjectId layer_b{};

  NestedResolver resolver() {
    return [this](ObjectId id) -> Content* {
      const auto it = binding.find(id);
      return it != binding.end() ? it->second : nullptr;
    };
  }
};

// Two identity-placed layers bound to `a` / `b` (default identity time maps, so
// audio-extent mapping into parent time is the identity).
void build_scene(Scene& s, Content* a, Content* b) {
  auto tx = s.model.transact("scene");
  s.comp = tx.add_composition(4.0, 4.0);
  const ObjectId ca = tx.add_content(1);
  const ObjectId cb = tx.add_content(1);
  s.layer_a = tx.add_layer(ca, Affine::identity());
  s.layer_b = tx.add_layer(cb, Affine::identity());
  tx.attach_layer(s.comp, s.layer_a);
  tx.attach_layer(s.comp, s.layer_b);
  tx.commit();
  s.binding[ca] = a;
  s.binding[cb] = b;
}

AudioResult render_mix(NestedContent& nested, std::uint32_t rate, ChannelLayout layout,
                       std::uint32_t frames, std::vector<float>& out) {
  out.assign(static_cast<std::size_t>(frames) * channel_count(layout), 0.0F);
  AudioBlock block{out.data(), frames, layout, rate};
  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(rate);
  const AudioRequest req{TimeRange{Time::zero(), Time{static_cast<std::int64_t>(frames) * fpf}},
                         rate,
                         layout,
                         block,
                         Exactness::Exact,
                         StateHandle{}};
  auto done = std::make_shared<AudioCompletion>();
  const std::optional<AudioResult> r = nested.audio()->render_audio(req, done);
  REQUIRE(r.has_value());
  return *r;
}

} // namespace

TEST_CASE("nested audio mix culls inaudible, zero-gain, missing, and unresolved layers") {
  RoutingAudioPull pull;
  NullBackend backend;
  const std::uint32_t rate = 48'000;
  const std::uint32_t frames = 4;

  SECTION("an inaudible layer contributes nothing to the mix") {
    ConstAudioLeaf a(1.0F);
    ConstAudioLeaf b(1.0F);
    Scene scene;
    build_scene(scene, &a, &b);
    {
      auto tx = scene.model.transact("mute b");
      tx.set_audible(scene.layer_b, false);
      tx.commit();
    }
    const DocStatePtr doc = scene.model.current();
    NestedContent nested(scene.comp);
    nested.attach(pull, backend, scene.resolver(), *doc);
    std::vector<float> out;
    const AudioResult r = render_mix(nested, rate, ChannelLayout::Stereo, frames, out);
    // Only the audible layer contributes (1.0), not both (2.0).
    for (const float v : out) {
      REQUIRE(v == 1.0F);
    }
    REQUIRE(r.achieved_rate == rate);
    REQUIRE(r.exact);
  }

  SECTION("a zero-gain layer contributes nothing to the mix") {
    ConstAudioLeaf a(1.0F);
    ConstAudioLeaf b(1.0F);
    Scene scene;
    build_scene(scene, &a, &b);
    {
      auto tx = scene.model.transact("silence b");
      tx.set_gain(scene.layer_b, 0.0);
      tx.commit();
    }
    const DocStatePtr doc = scene.model.current();
    NestedContent nested(scene.comp);
    nested.attach(pull, backend, scene.resolver(), *doc);
    std::vector<float> out;
    render_mix(nested, rate, ChannelLayout::Stereo, frames, out);
    for (const float v : out) {
      REQUIRE(v == 1.0F);
    }
  }

  SECTION("a layer culled by its span contributes nothing") {
    ConstAudioLeaf a(1.0F);
    ConstAudioLeaf b(1.0F);
    Scene scene;
    build_scene(scene, &a, &b);
    {
      auto tx = scene.model.transact("span b away");
      // A span far past the (near-zero) request window: b is culled.
      tx.set_span(scene.layer_b, TimeRange{Time{1'000'000'000}, Time{2'000'000'000}});
      tx.commit();
    }
    const DocStatePtr doc = scene.model.current();
    NestedContent nested(scene.comp);
    nested.attach(pull, backend, scene.resolver(), *doc);
    std::vector<float> out;
    render_mix(nested, rate, ChannelLayout::Stereo, frames, out);
    for (const float v : out) {
      REQUIRE(v == 1.0F); // only the in-span layer a contributes
    }
  }

  SECTION("a reverse-rate (negative time-map) layer is culled from the mix (deferred scope)") {
    ConstAudioLeaf a(1.0F);
    ConstAudioLeaf b(1.0F);
    Scene scene;
    build_scene(scene, &a, &b);
    {
      auto tx = scene.model.transact("reverse b");
      tx.set_time_map(scene.layer_b, TimeMap{Time::zero(), Rational{-1, 1}, Time::zero()});
      tx.commit();
    }
    const DocStatePtr doc = scene.model.current();
    NestedContent nested(scene.comp);
    nested.attach(pull, backend, scene.resolver(), *doc);
    std::vector<float> out;
    render_mix(nested, rate, ChannelLayout::Stereo, frames, out);
    for (const float v : out) {
      REQUIRE(v == 1.0F); // only the forward-rate layer a contributes
    }
  }

  SECTION("a layer whose audio pull does not settle inline shows the silent placeholder") {
    ConstAudioLeaf a(1.0F);
    ConstAudioLeaf b(1.0F);
    Scene scene;
    build_scene(scene, &a, &b);
    const DocStatePtr doc = scene.model.current();
    RoutingAudioPull miss(RoutingAudioPull::Mode::Miss);
    NestedContent nested(scene.comp);
    nested.attach(miss, backend, scene.resolver(), *doc);
    std::vector<float> out;
    const AudioResult r = render_mix(nested, rate, ChannelLayout::Stereo, frames, out);
    for (const float v : out) {
      REQUIRE(v == 0.0F); // both layers missed -> silence
    }
    REQUIRE(r.achieved_rate == rate); // no contributor -> faithful silent block
  }
}

TEST_CASE("nested audio mix reports a below-rate child honestly (baseline fill)") {
  RoutingAudioPull pull;
  NullBackend backend;
  const std::uint32_t rate = 48'000;
  // A single child that bottoms out at half the request rate, marked inexact.
  ConstAudioLeaf slow(1.0F, /*forced_rate=*/24'000);
  ConstAudioLeaf silent(0.0F);
  Scene scene;
  build_scene(scene, &slow, &silent);
  const DocStatePtr doc = scene.model.current();
  NestedContent nested(scene.comp);
  nested.attach(pull, backend, scene.resolver(), *doc);
  std::vector<float> out;
  const AudioResult r = render_mix(nested, rate, ChannelLayout::Stereo, 4, out);
  // The below-rate child lowers the aggregate and marks it inexact (quality
  // deferred to kinds.nested_audio_resampling).
  REQUIRE_FALSE(r.exact);
  REQUIRE(r.achieved_rate == 24'000);
}

// enforces: 12-audio#nested-audio-metadata-aggregates
TEST_CASE("nested aggregates audio stability from the reachable child audio facets") {
  SECTION("static iff every reachable audible child audio facet is static") {
    StubAudioLeaf a(Stability::Static, std::nullopt);
    StubAudioLeaf b(Stability::Static, std::nullopt);
    Scene scene;
    build_scene(scene, &a, &b);
    const DocStatePtr doc = scene.model.current();
    NullPull pull;
    NullBackend backend;
    NestedContent nested(scene.comp);
    nested.attach(pull, backend, scene.resolver(), *doc);
    REQUIRE(nested.audio()->audio_stability() == Stability::Static);
  }

  SECTION("one timed child makes the aggregate timed") {
    StubAudioLeaf a(Stability::Static, std::nullopt);
    StubAudioLeaf timed(Stability::Timed, TimeRange{Time{10}, Time{20}});
    Scene scene;
    build_scene(scene, &a, &timed);
    const DocStatePtr doc = scene.model.current();
    NullPull pull;
    NullBackend backend;
    NestedContent nested(scene.comp);
    nested.attach(pull, backend, scene.resolver(), *doc);
    REQUIRE(nested.audio()->audio_stability() == Stability::Timed);
  }

  SECTION("a purely visual (audio-less) layer does not contribute to audio metadata") {
    VisualLeaf visual;
    StubAudioLeaf timed(Stability::Timed, TimeRange{Time{10}, Time{20}});
    Scene scene;
    build_scene(scene, &visual, &timed);
    const DocStatePtr doc = scene.model.current();
    NullPull pull;
    NullBackend backend;
    NestedContent nested(scene.comp);
    nested.attach(pull, backend, scene.resolver(), *doc);
    // Only the timed audio child counts: stability Timed, extent its own (mapped).
    REQUIRE(nested.audio()->audio_stability() == Stability::Timed);
    REQUIRE(nested.audio()->audio_extent() == TimeRange{Time{10}, Time{20}});
  }
}

// enforces: 12-audio#nested-audio-metadata-aggregates
TEST_CASE("nested aggregates audio extent as the time-mapped union of reachable extents") {
  SECTION("static-unbounded children make the aggregate extent unbounded") {
    StubAudioLeaf a(Stability::Static, std::nullopt);
    StubAudioLeaf b(Stability::Static, std::nullopt);
    Scene scene;
    build_scene(scene, &a, &b);
    const DocStatePtr doc = scene.model.current();
    NullPull pull;
    NullBackend backend;
    NestedContent nested(scene.comp);
    nested.attach(pull, backend, scene.resolver(), *doc);
    REQUIRE_FALSE(nested.audio()->audio_extent().has_value());
  }

  SECTION("two bounded timed children union their extents (identity time maps)") {
    StubAudioLeaf a(Stability::Timed, TimeRange{Time{10}, Time{20}});
    StubAudioLeaf b(Stability::Timed, TimeRange{Time{30}, Time{40}});
    Scene scene;
    build_scene(scene, &a, &b);
    const DocStatePtr doc = scene.model.current();
    NullPull pull;
    NullBackend backend;
    NestedContent nested(scene.comp);
    nested.attach(pull, backend, scene.resolver(), *doc);
    REQUIRE(nested.audio()->audio_extent() == TimeRange{Time{10}, Time{40}});
  }

  SECTION("a negative-rate time map inverts the child extent into parent time") {
    StubAudioLeaf a(Stability::Timed, TimeRange{Time{10}, Time{20}});
    StubAudioLeaf b(Stability::Timed, TimeRange{Time{30}, Time{40}});
    Scene scene;
    build_scene(scene, &a, &b);
    {
      auto tx = scene.model.transact("reverse a");
      // rate -1: parent = -local, so a's [10,20) maps to [-20,-10) (min/max order).
      tx.set_time_map(scene.layer_a, TimeMap{Time::zero(), Rational{-1, 1}, Time::zero()});
      tx.commit();
    }
    const DocStatePtr doc = scene.model.current();
    NullPull pull;
    NullBackend backend;
    NestedContent nested(scene.comp);
    nested.attach(pull, backend, scene.resolver(), *doc);
    // Union of a's inverted extent [-20,-10) and b's identity extent [30,40).
    REQUIRE(nested.audio()->audio_extent() == TimeRange{Time{-20}, Time{40}});
  }

  SECTION("any reachable static-unbounded child makes even a timed aggregate unbounded") {
    StubAudioLeaf bounded(Stability::Timed, TimeRange{Time{10}, Time{20}});
    StubAudioLeaf unbounded(Stability::Static, std::nullopt);
    Scene scene;
    build_scene(scene, &bounded, &unbounded);
    const DocStatePtr doc = scene.model.current();
    NullPull pull;
    NullBackend backend;
    NestedContent nested(scene.comp);
    nested.attach(pull, backend, scene.resolver(), *doc);
    REQUIRE(nested.audio()->audio_stability() == Stability::Timed);
    REQUIRE_FALSE(nested.audio()->audio_extent().has_value());
  }
}

// enforces: 12-audio#nested-audio-metadata-aggregates
TEST_CASE("nested audio metadata recomputes only when the aggregate revision changes") {
  StubAudioLeaf a(Stability::Timed, TimeRange{Time{10}, Time{20}});
  StubAudioLeaf b(Stability::Timed, TimeRange{Time{30}, Time{40}});
  Scene scene;
  build_scene(scene, &a, &b);
  const DocStatePtr doc_v1 = scene.model.current();

  NullPull pull;
  NullBackend backend;
  NestedContent nested(scene.comp);
  nested.attach(pull, backend, scene.resolver(), *doc_v1);

  // First audio query computes once; repeated audio (and visual) queries at a
  // STABLE revision recompute zero more times -- the audio metadata shares the
  // visual aggregate-revision memo (doc 12:208).
  (void)nested.audio()->audio_stability();
  REQUIRE(nested.metadata_recomputes() == 1);
  for (int i = 0; i < 8; ++i) {
    (void)nested.audio()->audio_stability();
    (void)nested.audio()->audio_extent();
    (void)nested.bounds();
  }
  REQUIRE(nested.metadata_recomputes() == 1);

  // An edit bumps the document revision; re-pinning + re-attaching re-keys the
  // shared memo exactly once.
  {
    auto tx = scene.model.transact("edit");
    tx.set_gain(scene.layer_a, 0.5);
    tx.commit();
  }
  const DocStatePtr doc_v2 = scene.model.current();
  REQUIRE(doc_v2->revision() != doc_v1->revision());
  nested.attach(pull, backend, scene.resolver(), *doc_v2);
  (void)nested.audio()->audio_extent();
  REQUIRE(nested.metadata_recomputes() == 2);
}
