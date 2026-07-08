#include <arbc/audio_engine/mix.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/media/audio_block.hpp>
#include <arbc/model/model.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

// Behavioral + byte-exact unit tests for `arbc::audio-engine`'s composition mixer
// (`mix_composition`, doc 12:11-21,150-208). Lives in the component's own `t/`, so
// its includes stay inside the audio-engine dependency closure (contract / cache /
// model / media / base -- doc 17:41,57): it drives the mixer with LOCAL Content /
// AudioFacet / PullService doubles, never `kind_tone` / `compositor`. The
// byte-exact goldens through the REAL `PullServiceImpl` + `org.arbc.tone` live in
// top-level `tests/` (which is not level-checked).

namespace {

using namespace arbc;

// A deterministic procedural audio leaf: sample = amp * parabolic-sine over an
// EXACT integer flick phase (never std::sin), so the block is byte-exact and
// portable, mirroring tone's discipline. A pure function of absolute content-local
// time, so it is stateless and any rate/window is reproducible. Reports the
// requested rate / exact -- a rate-honoring child.
float parab_sine(std::int64_t t_flicks, std::uint32_t freq_hz, float amp) {
  const std::int64_t fps = Time::flicks_per_second;
  std::int64_t t = t_flicks % fps;
  if (t < 0) {
    t += fps;
  }
  const std::int64_t r = (static_cast<std::int64_t>(freq_hz) * t) % fps;
  double p = 2.0 * (static_cast<double>(r) / static_cast<double>(fps));
  if (p > 1.0) {
    p -= 2.0;
  }
  const double abs_p = p < 0.0 ? -p : p;
  return static_cast<float>(static_cast<double>(amp) * (4.0 * p * (1.0 - abs_p)));
}

class SineLeaf final : public Content {
public:
  SineLeaf(std::uint32_t freq_hz, float amp) : d_facet(freq_hz, amp) {}
  std::optional<Rect> bounds() const override { return std::nullopt; }
  Stability stability() const override { return Stability::Static; }
  std::optional<TimeRange> time_extent() const override { return std::nullopt; }
  std::optional<RenderResult> render(const RenderRequest&,
                                     std::shared_ptr<RenderCompletion> done) override {
    done->fail(RenderError::ContentFailed);
    return std::nullopt;
  }
  AudioFacet* audio() override { return &d_facet; }

private:
  class Facet final : public AudioFacet {
  public:
    Facet(std::uint32_t freq_hz, float amp) : d_freq(freq_hz), d_amp(amp) {}
    std::optional<TimeRange> audio_extent() const override { return std::nullopt; }
    Stability audio_stability() const override { return Stability::Static; }
    std::optional<AudioResult> render_audio(const AudioRequest& request,
                                            std::shared_ptr<AudioCompletion>) override {
      const std::uint32_t ch = channel_count(request.layout);
      const std::int64_t fpf =
          Time::flicks_per_second / static_cast<std::int64_t>(request.sample_rate);
      for (std::uint32_t f = 0; f < request.target.frames; ++f) {
        const float v = parab_sine(request.window.start.flicks + static_cast<std::int64_t>(f) * fpf,
                                   d_freq, d_amp);
        for (std::uint32_t c = 0; c < ch; ++c) {
          request.target.samples[static_cast<std::size_t>(f) * ch + c] = v;
        }
      }
      return AudioResult{request.sample_rate, true};
    }

  private:
    std::uint32_t d_freq;
    float d_amp;
  };
  Facet d_facet;
};

// A leaf that bottoms out at a native rate: asked above it, reports
// achieved_rate == native / exact == false (driving the mixer's below-rate
// reconstruction), and synthesizes the same deterministic parabolic sine so the
// samples are reproducible.
class BelowRateLeaf final : public Content {
public:
  explicit BelowRateLeaf(std::uint32_t native_rate) : d_facet(native_rate) {}
  std::optional<Rect> bounds() const override { return std::nullopt; }
  Stability stability() const override { return Stability::Static; }
  std::optional<TimeRange> time_extent() const override { return std::nullopt; }
  std::optional<RenderResult> render(const RenderRequest&,
                                     std::shared_ptr<RenderCompletion> done) override {
    done->fail(RenderError::ContentFailed);
    return std::nullopt;
  }
  AudioFacet* audio() override { return &d_facet; }

private:
  class Facet final : public AudioFacet {
  public:
    explicit Facet(std::uint32_t native_rate) : d_native(native_rate) {}
    std::optional<TimeRange> audio_extent() const override { return std::nullopt; }
    Stability audio_stability() const override { return Stability::Static; }
    std::optional<AudioResult> render_audio(const AudioRequest& request,
                                            std::shared_ptr<AudioCompletion>) override {
      const std::uint32_t ch = channel_count(request.layout);
      const std::int64_t fpf =
          Time::flicks_per_second / static_cast<std::int64_t>(request.sample_rate);
      for (std::uint32_t f = 0; f < request.target.frames; ++f) {
        const float v =
            parab_sine(request.window.start.flicks + static_cast<std::int64_t>(f) * fpf, 400, 0.5F);
        for (std::uint32_t c = 0; c < ch; ++c) {
          request.target.samples[static_cast<std::size_t>(f) * ch + c] = v;
        }
      }
      const std::uint32_t achieved = std::min(request.sample_rate, d_native);
      return AudioResult{achieved, request.sample_rate <= d_native};
    }

  private:
    std::uint32_t d_native;
  };
  Facet d_facet;
};

// A purely visual leaf: no audio facet, so it must cost the mixer nothing.
class VisualLeaf final : public Content {
public:
  std::optional<Rect> bounds() const override { return Rect{0.0, 0.0, 1.0, 1.0}; }
  Stability stability() const override { return Stability::Static; }
  std::optional<TimeRange> time_extent() const override { return std::nullopt; }
  std::optional<RenderResult> render(const RenderRequest& request,
                                     std::shared_ptr<RenderCompletion>) override {
    return RenderResult{request.scale, true};
  }
};

// Routes `pull_audio` to the input's facet inline (the single-threaded monitor
// path) and COUNTS every dispatch, so a test can witness "one pull per audible
// in-span audio layer / zero for a culled one" (12-audio#mix-engine-facetless-
// costs-nothing). A `Miss` mode leaves the completion unsettled (a worker
// deferral) so the mixer shows the silent placeholder.
class CountingAudioPull final : public PullService {
public:
  enum class Mode { Inline, Miss };
  explicit CountingAudioPull(Mode mode = Mode::Inline) : d_mode(mode) {}
  void pull(ContentRef, const RenderRequest&, std::shared_ptr<RenderCompletion> done) override {
    done->fail(RenderError::ContentFailed);
  }
  void pull_audio(ContentRef input, const AudioRequest& request,
                  std::shared_ptr<AudioCompletion> done) override {
    ++d_pulls;
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
  int pulls() const { return d_pulls; }

private:
  Mode d_mode;
  int d_pulls{0};
};

// A composition holding a caller-chosen set of layers, each bound to a
// caller-owned Content. `add` appends a layer and returns its id so a test can
// tweak gain / span / time map / audible before pinning.
struct Scene {
  Model model;
  std::unordered_map<ObjectId, Content*> binding;
  ObjectId comp{};

  ObjectId add(Content* content) {
    ObjectId layer{};
    {
      auto tx = model.transact("add");
      const ObjectId cid = tx.add_content(1);
      layer = tx.add_layer(cid, Affine::identity());
      if (comp == ObjectId{}) {
        comp = tx.add_composition(0.0, 0.0);
      }
      tx.attach_layer(comp, layer);
      tx.commit();
      binding[cid] = content;
    }
    return layer;
  }

  // A layer whose content is deliberately left unbound, so the resolver returns
  // nullptr for it (an unresolved / not-yet-loaded layer, doc 05:50).
  ObjectId add_unbound() {
    ObjectId layer{};
    {
      auto tx = model.transact("unbound");
      const ObjectId cid = tx.add_content(1);
      layer = tx.add_layer(cid, Affine::identity());
      if (comp == ObjectId{}) {
        comp = tx.add_composition(0.0, 0.0);
      }
      tx.attach_layer(comp, layer);
      tx.commit();
    }
    return layer;
  }

  MixResolver resolver() {
    return [this](ObjectId id) -> Content* {
      const auto it = binding.find(id);
      return it != binding.end() ? it->second : nullptr;
    };
  }
};

AudioRequest make_request(AudioBlock& block, std::vector<float>& buf, std::uint32_t rate,
                          ChannelLayout layout, std::uint32_t frames, Time start = Time::zero()) {
  buf.assign(static_cast<std::size_t>(frames) * channel_count(layout), 0.0F);
  block = AudioBlock{buf.data(), frames, layout, rate};
  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(rate);
  return AudioRequest{
      TimeRange{start, Time{start.flicks + static_cast<std::int64_t>(frames) * fpf}},
      rate,
      layout,
      block,
      Exactness::Exact,
      StateHandle{},
  };
}

bool bytes_equal(const std::vector<float>& a, const std::vector<float>& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

constexpr std::uint32_t k_rate = 48'000;
constexpr std::uint32_t k_frames = 32;

} // namespace

// enforces: 12-audio#mix-engine-mixes-layers-additively
TEST_CASE("mix_composition additively sums audible layers, scaled by gain, byte-exact") {
  SineLeaf a(300, 0.6F);
  SineLeaf b(700, 0.4F);
  Scene scene;
  const ObjectId la = scene.add(&a);
  const ObjectId lb = scene.add(&b);
  {
    auto tx = scene.model.transact("gains");
    tx.set_gain(la, 1.0);
    tx.set_gain(lb, 0.5);
    tx.commit();
  }
  const DocStatePtr doc = scene.model.current();
  CountingAudioPull pull;

  std::vector<float> got;
  AudioBlock got_block{};
  const AudioRequest req = make_request(got_block, got, k_rate, ChannelLayout::Stereo, k_frames);
  const AudioResult r = mix_composition(*doc, scene.comp, scene.resolver(), pull, req);

  // Hand-summed reference: each identity-placed, same-layout layer contributes its
  // render at the request window/rate, scaled by its gain -- the additive walk.
  std::vector<float> want(static_cast<std::size_t>(k_frames) * 2, 0.0F);
  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(k_rate);
  for (std::uint32_t f = 0; f < k_frames; ++f) {
    const std::int64_t t = static_cast<std::int64_t>(f) * fpf;
    const float va = parab_sine(t, 300, 0.6F);
    const float vb = parab_sine(t, 700, 0.4F);
    const float mixed = 1.0F * va + 0.5F * vb;
    want[static_cast<std::size_t>(f) * 2] = mixed;
    want[static_cast<std::size_t>(f) * 2 + 1] = mixed;
  }
  REQUIRE(bytes_equal(got, want));
  // Two rate-honoring contributors -> the aggregate stays exact at the request rate.
  REQUIRE(r.achieved_rate == k_rate);
  REQUIRE(r.exact);
  REQUIRE(pull.pulls() == 2); // one dispatch per audible in-span audio layer
}

// enforces: 12-audio#mix-engine-mixes-layers-additively
TEST_CASE("mix_composition requests a varispeed layer at the composed rational rate") {
  // A rate-1/2 layer plays its child an octave down: the mixer requests the child
  // at 2x the working rate over the time-mapped window, and mixes those samples 1:1.
  SineLeaf a(400, 0.5F);
  Scene scene;
  const ObjectId la = scene.add(&a);
  {
    auto tx = scene.model.transact("rate 1/2");
    tx.set_time_map(la, TimeMap{Time::zero(), Rational{1, 2}, Time::zero()});
    tx.commit();
  }
  const DocStatePtr doc = scene.model.current();
  CountingAudioPull pull;

  std::vector<float> got;
  AudioBlock got_block{};
  const AudioRequest req = make_request(got_block, got, k_rate, ChannelLayout::Stereo, k_frames);
  mix_composition(*doc, scene.comp, scene.resolver(), pull, req);

  // Reference: child_rate = rate * den/num = 2*rate; child window start = start/2
  // (local = parent * 1/2); samples read at the child rate over that window.
  const std::uint32_t child_rate = k_rate * 2;
  const std::int64_t fpf_child = Time::flicks_per_second / static_cast<std::int64_t>(child_rate);
  std::vector<float> want(static_cast<std::size_t>(k_frames) * 2, 0.0F);
  for (std::uint32_t f = 0; f < k_frames; ++f) {
    const std::int64_t t = 0 + static_cast<std::int64_t>(f) * fpf_child; // start/2 == 0
    const float v = parab_sine(t, 400, 0.5F); // gain 1.0 default * facet amp 0.5
    want[static_cast<std::size_t>(f) * 2] = v;
    want[static_cast<std::size_t>(f) * 2 + 1] = v;
  }
  REQUIRE(bytes_equal(got, want));
}

// enforces: 12-audio#mix-engine-mixes-layers-additively
TEST_CASE("mix_composition remixes a mono child up to a stereo target") {
  SineLeaf mono(500, 0.5F);
  Scene scene;
  scene.add(&mono);
  {
    auto tx = scene.model.transact("mono working format");
    tx.set_working_audio_format(scene.comp, AudioFormat{k_rate, ChannelLayout::Mono});
    tx.commit();
  }
  const DocStatePtr doc = scene.model.current();
  CountingAudioPull pull;

  std::vector<float> got;
  AudioBlock got_block{};
  const AudioRequest req = make_request(got_block, got, k_rate, ChannelLayout::Stereo, k_frames);
  mix_composition(*doc, scene.comp, scene.resolver(), pull, req);

  // A mono child fans out to every stereo channel: L == R == the mono sample.
  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(k_rate);
  for (std::uint32_t f = 0; f < k_frames; ++f) {
    const float v = parab_sine(static_cast<std::int64_t>(f) * fpf, 500, 0.5F);
    REQUIRE(got[static_cast<std::size_t>(f) * 2] == v);
    REQUIRE(got[static_cast<std::size_t>(f) * 2 + 1] == v);
  }
}

// enforces: 12-audio#mix-engine-facetless-costs-nothing
TEST_CASE("culled layers contribute exactly zero and dispatch nothing") {
  SineLeaf audible_a(300, 0.5F);
  SineLeaf audible_b(600, 0.5F);
  VisualLeaf visual; // no audio facet
  SineLeaf muted(440, 0.5F);
  SineLeaf zero_gain(440, 0.5F);
  SineLeaf out_of_span(440, 0.5F);
  SineLeaf reversed(440, 0.5F);

  Scene scene;
  scene.add(&audible_a);
  scene.add(&visual);
  const ObjectId lm = scene.add(&muted);
  const ObjectId lz = scene.add(&zero_gain);
  const ObjectId ls = scene.add(&out_of_span);
  const ObjectId lr = scene.add(&reversed);
  scene.add(&audible_b);
  {
    auto tx = scene.model.transact("cull");
    tx.set_audible(lm, false);
    tx.set_gain(lz, 0.0);
    tx.set_span(ls, TimeRange{Time{1'000'000'000}, Time{2'000'000'000}});
    tx.set_time_map(lr, TimeMap{Time::zero(), Rational{-1, 1}, Time::zero()});
    tx.commit();
  }
  const DocStatePtr doc = scene.model.current();
  CountingAudioPull pull;

  std::vector<float> got;
  AudioBlock got_block{};
  const AudioRequest req = make_request(got_block, got, k_rate, ChannelLayout::Stereo, k_frames);
  mix_composition(*doc, scene.comp, scene.resolver(), pull, req);

  // Only the two genuinely audible in-span layers dispatch (and contribute); the
  // facet-less / inaudible / zero-gain / out-of-span / reverse-rate layers cost
  // nothing.
  REQUIRE(pull.pulls() == 2);

  // Reference: only audible_a + audible_b at unit gain.
  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(k_rate);
  for (std::uint32_t f = 0; f < k_frames; ++f) {
    const std::int64_t t = static_cast<std::int64_t>(f) * fpf;
    const float v = parab_sine(t, 300, 0.5F) + parab_sine(t, 600, 0.5F);
    REQUIRE(got[static_cast<std::size_t>(f) * 2] == v);
  }
}

// enforces: 12-audio#mix-engine-facetless-costs-nothing
TEST_CASE("an all-culled composition dispatches zero and mixes silence") {
  VisualLeaf visual;
  SineLeaf muted(440, 0.5F);
  Scene scene;
  scene.add(&visual);
  const ObjectId lm = scene.add(&muted);
  {
    auto tx = scene.model.transact("mute");
    tx.set_audible(lm, false);
    tx.commit();
  }
  const DocStatePtr doc = scene.model.current();
  CountingAudioPull pull;

  std::vector<float> got;
  AudioBlock got_block{};
  const AudioRequest req = make_request(got_block, got, k_rate, ChannelLayout::Stereo, k_frames);
  const AudioResult r = mix_composition(*doc, scene.comp, scene.resolver(), pull, req);

  REQUIRE(pull.pulls() == 0); // costs the audio engine nothing
  for (const float v : got) {
    REQUIRE(v == 0.0F); // silence
  }
  REQUIRE(r.achieved_rate == k_rate); // no contributor -> a faithful silent block
  REQUIRE(r.exact);
}

// enforces: 12-audio#mix-engine-mixes-layers-additively
TEST_CASE("mix_composition folds achieved_rate/exact honestly over a below-rate child") {
  BelowRateLeaf slow(24'000); // bottoms out at half the working rate
  Scene scene;
  scene.add(&slow);
  const DocStatePtr doc = scene.model.current();
  CountingAudioPull pull;

  std::vector<float> got;
  AudioBlock got_block{};
  const AudioRequest req = make_request(got_block, got, k_rate, ChannelLayout::Stereo, k_frames);
  const AudioResult r = mix_composition(*doc, scene.comp, scene.resolver(), pull, req);

  // The below-rate child lowers the aggregate and marks it inexact; the mixer
  // issues a second (native-rate) pull to band-limit-reconstruct it.
  REQUIRE_FALSE(r.exact);
  REQUIRE(r.achieved_rate == 24'000);
  REQUIRE(pull.pulls() == 2); // discovery + native re-request

  // The reconstructed block is not silence (the resampler produced real samples).
  bool any_nonzero = false;
  for (const float v : got) {
    if (v != 0.0F) {
      any_nonzero = true;
      break;
    }
  }
  REQUIRE(any_nonzero);
}

TEST_CASE("mix_composition on an absent composition mixes a faithful silent block") {
  Scene scene;
  SineLeaf a(300, 0.5F);
  scene.add(&a); // one real composition, but we mix a DIFFERENT (absent) id
  const DocStatePtr doc = scene.model.current();
  CountingAudioPull pull;

  std::vector<float> got;
  AudioBlock got_block{};
  const AudioRequest req = make_request(got_block, got, k_rate, ChannelLayout::Stereo, k_frames);
  const AudioResult r = mix_composition(*doc, ObjectId{9'999}, scene.resolver(), pull, req);

  REQUIRE(pull.pulls() == 0);
  for (const float v : got) {
    REQUIRE(v == 0.0F);
  }
  REQUIRE(r.achieved_rate == k_rate); // silent block is honest at the request rate
  REQUIRE(r.exact);
}

TEST_CASE("mix_composition skips an unresolved layer at zero cost") {
  SineLeaf audible(300, 0.5F);
  Scene scene;
  scene.add_unbound(); // the resolver returns nullptr for this layer
  scene.add(&audible);
  const DocStatePtr doc = scene.model.current();
  CountingAudioPull pull;

  std::vector<float> got;
  AudioBlock got_block{};
  const AudioRequest req = make_request(got_block, got, k_rate, ChannelLayout::Stereo, k_frames);
  mix_composition(*doc, scene.comp, scene.resolver(), pull, req);

  REQUIRE(pull.pulls() == 1); // only the resolved audible layer dispatches
  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(k_rate);
  for (std::uint32_t f = 0; f < k_frames; ++f) {
    const float v = parab_sine(static_cast<std::int64_t>(f) * fpf, 300, 0.5F);
    REQUIRE(got[static_cast<std::size_t>(f) * 2] == v);
  }
}

TEST_CASE("mix_composition mixes silence for a layer whose pull defers to a worker") {
  SineLeaf a(300, 0.5F);
  Scene scene;
  scene.add(&a);
  const DocStatePtr doc = scene.model.current();
  CountingAudioPull miss(CountingAudioPull::Mode::Miss); // never settles inline

  std::vector<float> got;
  AudioBlock got_block{};
  const AudioRequest req = make_request(got_block, got, k_rate, ChannelLayout::Stereo, k_frames);
  const AudioResult r = mix_composition(*doc, scene.comp, scene.resolver(), miss, req);

  REQUIRE(miss.pulls() == 1); // the dispatch was issued...
  for (const float v : got) {
    REQUIRE(v == 0.0F); // ...but the unsettled miss contributes silence this pass
  }
  REQUIRE(r.achieved_rate == k_rate);
  REQUIRE(r.exact);
}

TEST_CASE("mix_composition downmixes a stereo child into a mono target") {
  SineLeaf stereo(500, 0.5F); // a stereo working-format child
  Scene scene;
  scene.add(&stereo); // composition working format defaults to stereo
  const DocStatePtr doc = scene.model.current();
  CountingAudioPull pull;

  std::vector<float> got;
  AudioBlock got_block{};
  const AudioRequest req = make_request(got_block, got, k_rate, ChannelLayout::Mono, k_frames);
  mix_composition(*doc, scene.comp, scene.resolver(), pull, req);

  // The stereo child's channels are averaged into the single mono channel; since
  // both channels carry the same sample, the mono result equals that sample.
  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(k_rate);
  for (std::uint32_t f = 0; f < k_frames; ++f) {
    const float v = parab_sine(static_cast<std::int64_t>(f) * fpf, 500, 0.5F);
    REQUIRE(got[f] == v);
  }
}

TEST_CASE("mix_composition is deterministic: two calls settle bit-identical") {
  SineLeaf a(300, 0.6F);
  SineLeaf b(700, 0.4F);
  Scene scene;
  scene.add(&a);
  scene.add(&b);
  const DocStatePtr doc = scene.model.current();
  CountingAudioPull pull;

  std::vector<float> first;
  AudioBlock first_block{};
  const AudioRequest req1 =
      make_request(first_block, first, k_rate, ChannelLayout::Stereo, k_frames);
  mix_composition(*doc, scene.comp, scene.resolver(), pull, req1);

  std::vector<float> second;
  AudioBlock second_block{};
  const AudioRequest req2 =
      make_request(second_block, second, k_rate, ChannelLayout::Stereo, k_frames);
  mix_composition(*doc, scene.comp, scene.resolver(), pull, req2);

  REQUIRE(bytes_equal(first, second));
}
