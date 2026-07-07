#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/kind_nested/nested_content.hpp>
#include <arbc/kind_tone/tone_content.hpp>
#include <arbc/media/audio_block.hpp>
#include <arbc/media/audio_format.hpp>
#include <arbc/media/audio_resampler.hpp>
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

// Byte-exact goldens for org.arbc.nested's BELOW-RATE cross-rate reconstruction
// (kinds.nested_audio_resampling). A rate-honoring child keeps the byte-exact 1:1
// path pinned by nested_audio_goldens.t.cpp; a child that bottoms out BELOW the
// composed rate is instead band-limit-reconstructed by the shared `arbc::media`
// windowed-sinc kernel. Like the honoring goldens these are SELF-CHECKING
// equalities -- nested's reconstructed mix is compared, byte-for-byte, against
// applying the SAME media kernel to the child's genuine native block (the "engine
// resamples" identity, doc 12:24-25), and against the baseline hold to prove the
// reconstruction is decisively NOT a nearest/hold. The kernel's own byte-exact
// correctness is pinned independently in src/media/t/audio_resampler.t.cpp.

namespace {

using namespace arbc;

// --- A below-rate audio source (tone cannot serve: it always honors) ---------
//
// A procedural source that reports a fixed NATIVE rate: asked at or below its
// native rate it honors (fills genuine samples, achieved == request); asked ABOVE
// it (the composed request rate) it can convey only native-rate information, so it
// reports `achieved_rate == native_rate`, `exact == false` -- the below-rate
// contributor this task reconstructs. The waveform is a byte-exact parabolic sine
// over an exact integer flick phase (never std::sin), portable exactly as tone's
// (kinds.tone), so the samples are byte-exact across toolchains.
float bl_sample(std::uint32_t freq_hz, float amp, std::int64_t t) {
  constexpr std::int64_t fps = Time::flicks_per_second;
  std::int64_t tm = t % fps;
  if (tm < 0) {
    tm += fps;
  }
  const std::int64_t num = static_cast<std::int64_t>(freq_hz) * tm;
  const std::int64_t r = num % fps;
  const double frac = static_cast<double>(r) / static_cast<double>(fps);
  double p = 2.0 * frac;
  if (p > 1.0) {
    p -= 2.0;
  }
  const double abs_p = p < 0.0 ? -p : p;
  const double s = 4.0 * p * (1.0 - abs_p); // factored parabolic sine (no FMA a*b+c)
  return static_cast<float>(static_cast<double>(amp) * s);
}

class BelowRateSource final : public Content {
public:
  explicit BelowRateSource(std::uint32_t native_rate, std::uint32_t freq_hz = 3000,
                           float amp = 0.6F)
      : d_native_rate(native_rate), d_freq_hz(freq_hz), d_amp(amp), d_facet(this) {}

  std::optional<Rect> bounds() const override { return std::nullopt; }
  Stability stability() const override { return Stability::Static; }
  std::optional<TimeRange> time_extent() const override { return std::nullopt; }
  std::optional<RenderResult> render(const RenderRequest&,
                                     std::shared_ptr<RenderCompletion> done) override {
    done->fail(RenderError::ContentFailed); // audio-only test source
    return std::nullopt;
  }
  AudioFacet* audio() override { return &d_facet; }

  std::uint32_t native_rate() const { return d_native_rate; }

private:
  class Facet final : public AudioFacet {
  public:
    explicit Facet(BelowRateSource* owner) : d_owner(owner) {}
    std::optional<TimeRange> audio_extent() const override { return std::nullopt; }
    Stability audio_stability() const override { return Stability::Static; }
    std::optional<AudioResult> render_audio(const AudioRequest& request,
                                            std::shared_ptr<AudioCompletion>) override {
      const std::uint32_t ch = channel_count(request.layout);
      const std::int64_t fpf =
          Time::flicks_per_second / static_cast<std::int64_t>(request.sample_rate);
      for (std::uint32_t f = 0; f < request.target.frames; ++f) {
        const std::int64_t t = request.window.start.flicks + static_cast<std::int64_t>(f) * fpf;
        const float v = bl_sample(d_owner->d_freq_hz, d_owner->d_amp, t);
        for (std::uint32_t c = 0; c < ch; ++c) {
          request.target.samples[static_cast<std::size_t>(f) * ch + c] = v;
        }
      }
      // Honors at or below native; reports native (inexact) above it -- the
      // below-rate contributor.
      const std::uint32_t achieved = std::min(request.sample_rate, d_owner->d_native_rate);
      return AudioResult{achieved, request.sample_rate <= d_owner->d_native_rate};
    }

  private:
    BelowRateSource* d_owner;
  };

  std::uint32_t d_native_rate;
  std::uint32_t d_freq_hz;
  float d_amp;
  Facet d_facet;
};

// The pull double: routes pull_audio to the input's facet and counts pulls
// (discovery + native re-request), threading a recursion-depth backstop.
class InlineAudioPull final : public PullService {
public:
  explicit InlineAudioPull(unsigned max_depth = 64) : d_max_depth(max_depth) {}
  void pull(ContentRef, const RenderRequest&, std::shared_ptr<RenderCompletion> done) override {
    done->fail(RenderError::ContentFailed);
  }
  void pull_audio(ContentRef input, const AudioRequest& request,
                  std::shared_ptr<AudioCompletion> done) override {
    ++d_pulls;
    if (input == nullptr) {
      done->fail(RenderError::ContentFailed);
      return;
    }
    if (d_depth >= d_max_depth) {
      done->fail(RenderError::ResourceUnavailable);
      return;
    }
    AudioFacet* af = input->audio();
    if (af == nullptr) {
      done->fail(RenderError::ResourceUnavailable);
      return;
    }
    ++d_depth;
    const std::optional<AudioResult> r = af->render_audio(request, done);
    --d_depth;
    if (r.has_value()) {
      done->complete(*r);
    } else if (!done->settled()) {
      done->fail(RenderError::ContentFailed);
    }
  }
  unsigned pulls() const { return d_pulls; }

private:
  unsigned d_max_depth;
  unsigned d_depth{0};
  unsigned d_pulls{0};
};

// A pull double that settles the first (discovery) pull inline but DEFERS the
// second (native re-request) by leaving its completion unsettled -- the worker-miss
// path. Nested must cancel the deferred native pull and fall back to the baseline
// discovery block rather than fabricate samples.
class DeferNativePull final : public PullService {
public:
  void pull(ContentRef, const RenderRequest&, std::shared_ptr<RenderCompletion> done) override {
    done->fail(RenderError::ContentFailed);
  }
  void pull_audio(ContentRef input, const AudioRequest& request,
                  std::shared_ptr<AudioCompletion> done) override {
    ++d_pulls;
    if (d_pulls >= 2) {
      return; // defer the native re-request: leave `done` unsettled (a worker miss)
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
  unsigned pulls() const { return d_pulls; }

private:
  unsigned d_pulls{0};
};

bool bytes_equal(const std::vector<float>& a, const std::vector<float>& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

NestedResolver map_resolver(std::unordered_map<ObjectId, Content*>& binding) {
  return [&binding](ObjectId id) -> Content* {
    const auto it = binding.find(id);
    return it != binding.end() ? it->second : nullptr;
  };
}

constexpr std::uint32_t k_rate = 48'000;
constexpr std::uint32_t k_frames = 32;

TimeRange frame_window(std::uint32_t rate, std::uint32_t frames) {
  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(rate);
  return TimeRange{Time::zero(), Time{static_cast<std::int64_t>(frames) * fpf}};
}

struct MixResult {
  std::vector<float> samples;
  AudioResult meta{0, true};
};

MixResult render_nested(NestedContent& nested, const TimeRange& window, std::uint32_t rate,
                        ChannelLayout layout, std::uint32_t frames) {
  std::vector<float> buf(static_cast<std::size_t>(frames) * channel_count(layout), 0.0F);
  AudioBlock block{buf.data(), frames, layout, rate};
  const AudioRequest req{window, rate, layout, block, Exactness::Exact, StateHandle{}};
  auto done = std::make_shared<AudioCompletion>();
  const std::optional<AudioResult> r = nested.audio()->render_audio(req, done);
  REQUIRE(r.has_value());
  return MixResult{buf, *r};
}

// The oracle: mirror nested's below-rate path -- pull the source's GENUINE native
// samples over the same child-local window at the native rate, band-limit them to
// `child_rate` with the media kernel, then apply the layer gain + layout remix.
std::vector<float> reconstructed_oracle(BelowRateSource& src, Time child_start,
                                        std::uint32_t child_rate, ChannelLayout child_layout,
                                        std::uint32_t frames, double gain, ChannelLayout out_layout) {
  const std::uint32_t native_rate = src.native_rate();
  const std::uint32_t in_ch = channel_count(child_layout);
  const std::uint32_t out_ch = channel_count(out_layout);
  const std::int64_t fpf_native = Time::flicks_per_second / static_cast<std::int64_t>(native_rate);
  const std::uint32_t native_frames = static_cast<std::uint32_t>(
      static_cast<std::uint64_t>(frames) * native_rate / child_rate + 1);
  std::vector<float> native_buf(static_cast<std::size_t>(native_frames) * in_ch, 0.0F);
  AudioBlock native_block{native_buf.data(), native_frames, child_layout, native_rate};
  const AudioRequest nreq{
      TimeRange{child_start,
                Time{child_start.flicks + static_cast<std::int64_t>(native_frames) * fpf_native}},
      native_rate, child_layout, native_block, Exactness::Exact, StateHandle{}};
  auto done = std::make_shared<AudioCompletion>();
  src.audio()->render_audio(nreq, done);

  std::vector<float> child_buf(static_cast<std::size_t>(frames) * in_ch, 0.0F);
  AudioBlock child_block{child_buf.data(), frames, child_layout, child_rate};
  resample_audio(native_block, child_block);

  std::vector<float> out(static_cast<std::size_t>(frames) * out_ch, 0.0F);
  const float g = static_cast<float>(gain);
  for (std::uint32_t f = 0; f < frames; ++f) {
    for (std::uint32_t c = 0; c < out_ch; ++c) {
      float s = 0.0F;
      if (in_ch == out_ch) {
        s = child_buf[static_cast<std::size_t>(f) * in_ch + c];
      } else if (in_ch == 1) {
        s = child_buf[f];
      } else {
        s = 0.5F * (child_buf[static_cast<std::size_t>(f) * 2] +
                    child_buf[static_cast<std::size_t>(f) * 2 + 1]);
      }
      out[static_cast<std::size_t>(f) * out_ch + c] = g * s;
    }
  }
  return out;
}

TimeRange frame_window_at(Time start, std::uint32_t rate, std::uint32_t frames) {
  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(rate);
  return TimeRange{start, Time{start.flicks + static_cast<std::int64_t>(frames) * fpf}};
}

// The baseline nearest/hold this task replaces: the source's own above-rate block
// read 1:1 (what the discovery pull returns), gain-scaled and remixed.
std::vector<float> baseline_hold(BelowRateSource& src, Time child_start, std::uint32_t child_rate,
                                 ChannelLayout child_layout, std::uint32_t frames, double gain) {
  const std::uint32_t ch = channel_count(child_layout);
  std::vector<float> buf(static_cast<std::size_t>(frames) * ch, 0.0F);
  AudioBlock block{buf.data(), frames, child_layout, child_rate};
  const AudioRequest req{
      frame_window_at(child_start, child_rate, frames), child_rate, child_layout, block,
      Exactness::Exact, StateHandle{}};
  auto done = std::make_shared<AudioCompletion>();
  src.audio()->render_audio(req, done);
  const float g = static_cast<float>(gain);
  for (float& v : buf) {
    v = g * v;
  }
  return buf;
}

} // namespace

// enforces: 12-audio#nested-boundary-resamples-below-rate-children
TEST_CASE("nested reconstructs an integer-ratio (24000->48k) below-rate child, not a hold") {
  Model model;
  BelowRateSource src(24'000);
  std::unordered_map<ObjectId, Content*> binding;
  ObjectId comp{};
  {
    auto tx = model.transact("integer ratio");
    comp = tx.add_composition(0.0, 0.0);
    const ObjectId cs = tx.add_content(1);
    const ObjectId ls = tx.add_layer(cs, Affine::identity());
    tx.attach_layer(comp, ls);
    tx.commit();
    binding[cs] = &src;
  }
  const DocStatePtr doc = model.current();
  const TimeRange window = frame_window(k_rate, k_frames);

  CpuBackend backend;
  InlineAudioPull pull;
  NestedContent nested(comp);
  nested.attach(pull, backend, map_resolver(binding), *doc);

  const MixResult got = render_nested(nested, window, k_rate, ChannelLayout::Stereo, k_frames);
  const std::vector<float> want =
      reconstructed_oracle(src, Time::zero(), k_rate, ChannelLayout::Stereo, k_frames, 1.0,
                           ChannelLayout::Stereo);
  REQUIRE(bytes_equal(got.samples, want));

  // Decisively not the baseline hold the discovery block would have produced.
  const std::vector<float> hold =
      baseline_hold(src, Time::zero(), k_rate, ChannelLayout::Stereo, k_frames, 1.0);
  REQUIRE_FALSE(bytes_equal(got.samples, hold));

  // Exactly two pulls: discovery (above-rate) + one native re-request.
  REQUIRE(pull.pulls() == 2);
  // Honesty preserved: the aggregate reports the child's native rate, exact false.
  REQUIRE(got.meta.achieved_rate == 24'000);
  REQUIRE(got.meta.exact == false);
}

// enforces: 12-audio#nested-boundary-resamples-below-rate-children
TEST_CASE("nested reconstructs a non-integer-ratio (44100->48k) below-rate child") {
  Model model;
  BelowRateSource src(44'100);
  std::unordered_map<ObjectId, Content*> binding;
  ObjectId comp{};
  {
    auto tx = model.transact("non-integer ratio");
    comp = tx.add_composition(0.0, 0.0);
    const ObjectId cs = tx.add_content(1);
    const ObjectId ls = tx.add_layer(cs, Affine::identity());
    tx.attach_layer(comp, ls);
    tx.commit();
    binding[cs] = &src;
  }
  const DocStatePtr doc = model.current();
  const TimeRange window = frame_window(k_rate, k_frames);

  CpuBackend backend;
  InlineAudioPull pull;
  NestedContent nested(comp);
  nested.attach(pull, backend, map_resolver(binding), *doc);

  const MixResult got = render_nested(nested, window, k_rate, ChannelLayout::Stereo, k_frames);
  const std::vector<float> want =
      reconstructed_oracle(src, Time::zero(), k_rate, ChannelLayout::Stereo, k_frames, 1.0,
                           ChannelLayout::Stereo);
  REQUIRE(bytes_equal(got.samples, want));
  REQUIRE(pull.pulls() == 2);
  REQUIRE(got.meta.achieved_rate == 44'100);
  REQUIRE(got.meta.exact == false);
}

// enforces: 12-audio#nested-boundary-resamples-below-rate-children
TEST_CASE("nested composes varispeed then reconstructs a below-rate child (composed 3:1)") {
  // A rate-1/2 layer (child_rate = 96 kHz) over a native-32000 child: the
  // exact-rational rate composes FIRST (doc 11), then the leaf rounds once.
  Model model;
  BelowRateSource src(32'000);
  std::unordered_map<ObjectId, Content*> binding;
  ObjectId comp{};
  ObjectId layer{};
  {
    auto tx = model.transact("varispeed below-rate");
    comp = tx.add_composition(0.0, 0.0);
    const ObjectId cs = tx.add_content(1);
    layer = tx.add_layer(cs, Affine::identity());
    tx.attach_layer(comp, layer);
    tx.commit();
    binding[cs] = &src;
  }
  {
    auto tx = model.transact("rate 1/2");
    tx.set_time_map(layer, TimeMap{Time::zero(), Rational{1, 2}, Time::zero()});
    tx.commit();
  }
  const DocStatePtr doc = model.current();
  const TimeRange window = frame_window(k_rate, k_frames);

  CpuBackend backend;
  InlineAudioPull pull;
  NestedContent nested(comp);
  nested.attach(pull, backend, map_resolver(binding), *doc);

  const MixResult got = render_nested(nested, window, k_rate, ChannelLayout::Stereo, k_frames);
  // child_rate = request rate * den/num = 48000 * 2 = 96000; child_start = 0.
  const std::vector<float> want =
      reconstructed_oracle(src, Time::zero(), 96'000, ChannelLayout::Stereo, k_frames, 1.0,
                           ChannelLayout::Stereo);
  REQUIRE(bytes_equal(got.samples, want));
  REQUIRE(pull.pulls() == 2);
  // eff = achieved_rate * request_rate / child_rate = 32000 * 48000 / 96000 = 16000.
  REQUIRE(got.meta.achieved_rate == 16'000);
  REQUIRE(got.meta.exact == false);
}

// enforces: 12-audio#nested-boundary-resamples-below-rate-children
TEST_CASE("nested reconstructs a below-rate leaf through two levels of nesting") {
  Model model;
  BelowRateSource src(24'000);
  std::unordered_map<ObjectId, Content*> binding;
  ObjectId c1{}; // inner: the below-rate leaf
  ObjectId c2{}; // outer: embeds the inner nested
  ObjectId c_src{};
  ObjectId c_inner{};
  {
    auto tx = model.transact("two levels below-rate");
    c1 = tx.add_composition(0.0, 0.0);
    c_src = tx.add_content(1);
    const ObjectId l_src = tx.add_layer(c_src, Affine::identity());
    tx.attach_layer(c1, l_src);
    c2 = tx.add_composition(0.0, 0.0);
    c_inner = tx.add_content(1);
    const ObjectId l_inner = tx.add_layer(c_inner, Affine::identity());
    tx.attach_layer(c2, l_inner);
    tx.commit();
    binding[c_src] = &src;
  }
  const DocStatePtr doc = model.current();
  const TimeRange window = frame_window(k_rate, k_frames);

  CpuBackend backend;
  InlineAudioPull pull;
  NestedContent inner(c1);
  inner.attach(pull, backend, map_resolver(binding), *doc);
  binding[c_inner] = &inner;
  NestedContent outer(c2);
  outer.attach(pull, backend, map_resolver(binding), *doc);

  const MixResult got = render_nested(outer, window, k_rate, ChannelLayout::Stereo, k_frames);
  // Identity maps at both levels: the inner nested reconstructs at 48k, and the
  // outer 1:1-mixes that (rate-honoring) block -- so the outer result equals the
  // inner reconstruction.
  const std::vector<float> want =
      reconstructed_oracle(src, Time::zero(), k_rate, ChannelLayout::Stereo, k_frames, 1.0,
                           ChannelLayout::Stereo);
  REQUIRE(bytes_equal(got.samples, want));
  REQUIRE(got.meta.achieved_rate == 24'000);
  REQUIRE(got.meta.exact == false);
}

// enforces: 12-audio#nested-mixes-child-audio-through-pull
// enforces: 12-audio#tone-renders-at-any-requested-rate
TEST_CASE("a rate-honoring tone child stays byte-exact 1:1 and never triggers the resampler") {
  // The collapse-to-baseline regression guard: a honoring child reproduces the
  // 1:1 recursion identity byte-for-byte and issues exactly ONE pull (no native
  // re-request), so every pre-existing honoring golden is unaffected.
  Model model;
  ToneContent tone(440, 0.5F);
  std::unordered_map<ObjectId, Content*> binding;
  ObjectId comp{};
  {
    auto tx = model.transact("honoring tone");
    comp = tx.add_composition(0.0, 0.0);
    const ObjectId ct = tx.add_content(1);
    const ObjectId lt = tx.add_layer(ct, Affine::identity());
    tx.attach_layer(comp, lt);
    tx.commit();
    binding[ct] = &tone;
  }
  const DocStatePtr doc = model.current();
  const TimeRange window = frame_window(k_rate, k_frames);

  CpuBackend backend;
  InlineAudioPull pull;
  NestedContent nested(comp);
  nested.attach(pull, backend, map_resolver(binding), *doc);

  const MixResult got = render_nested(nested, window, k_rate, ChannelLayout::Stereo, k_frames);

  // Oracle: the tone rendered directly at the request rate (1:1 placement).
  std::vector<float> want(static_cast<std::size_t>(k_frames) * 2, 0.0F);
  {
    AudioBlock block{want.data(), k_frames, ChannelLayout::Stereo, k_rate};
    const AudioRequest req{window, k_rate, ChannelLayout::Stereo, block, Exactness::Exact,
                           StateHandle{}};
    auto done = std::make_shared<AudioCompletion>();
    const std::optional<AudioResult> r = tone.audio()->render_audio(req, done);
    REQUIRE(r.has_value());
  }
  REQUIRE(bytes_equal(got.samples, want));
  REQUIRE(pull.pulls() == 1); // no native re-request for a honoring child
  REQUIRE(got.meta.achieved_rate == k_rate);
  REQUIRE(got.meta.exact == true);
}

// enforces: 12-audio#nested-boundary-resamples-below-rate-children
TEST_CASE("a deferred native re-request falls back to the baseline block, honestly") {
  Model model;
  BelowRateSource src(24'000);
  std::unordered_map<ObjectId, Content*> binding;
  ObjectId comp{};
  {
    auto tx = model.transact("deferred native");
    comp = tx.add_composition(0.0, 0.0);
    const ObjectId cs = tx.add_content(1);
    const ObjectId ls = tx.add_layer(cs, Affine::identity());
    tx.attach_layer(comp, ls);
    tx.commit();
    binding[cs] = &src;
  }
  const DocStatePtr doc = model.current();
  const TimeRange window = frame_window(k_rate, k_frames);

  CpuBackend backend;
  DeferNativePull pull;
  NestedContent nested(comp);
  nested.attach(pull, backend, map_resolver(binding), *doc);

  const MixResult got = render_nested(nested, window, k_rate, ChannelLayout::Stereo, k_frames);
  // The native pull was deferred, so the mix falls back to the discovery block (the
  // baseline hold), NOT silence and NOT a fabricated reconstruction.
  const std::vector<float> hold =
      baseline_hold(src, Time::zero(), k_rate, ChannelLayout::Stereo, k_frames, 1.0);
  REQUIRE(bytes_equal(got.samples, hold));
  REQUIRE(pull.pulls() == 2); // discovery + the (deferred) native re-request
  // Honesty is unchanged by the fallback: still the native rate, still inexact.
  REQUIRE(got.meta.achieved_rate == 24'000);
  REQUIRE(got.meta.exact == false);
}
