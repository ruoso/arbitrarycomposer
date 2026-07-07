#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/kind_nested/nested_content.hpp>
#include <arbc/kind_tone/tone_content.hpp>
#include <arbc/media/audio_block.hpp>
#include <arbc/media/audio_format.hpp>
#include <arbc/model/model.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

// Byte-exact goldens for org.arbc.nested's AUDIO facet (doc 16 tier-3, doc
// 12:202-208). Like the visual nested goldens, these are SELF-CHECKING
// equalities -- nested's mixed samples are compared against directly mixing the
// same tones under the equivalent monitor -- so there are no frozen tables: the
// "rendering is recursion" identity is the oracle for audio too. Cross-component
// (kind_nested + kind_tone + backend-cpu), so it lives here.

namespace {

using namespace arbc;

// An inline (synchronous) honoring of the audio pull contract, routing
// `pull_audio` to the input's `AudioFacet` and threading the per-request
// recursion-depth backstop the real mix engine provides (doc 05:66-70), so a
// divergent Droste descent bottoms out on silence rather than recursing forever.
// The visual `pull` is unused by the audio goldens.
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
      ++d_backstops; // the >=1x cycle backstop (doc 05:66-70): silence
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
  unsigned backstops() const { return d_backstops; }

private:
  unsigned d_max_depth;
  unsigned d_depth{0};
  unsigned d_pulls{0};
  unsigned d_backstops{0};
};

bool bytes_equal(const std::vector<float>& a, const std::vector<float>& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

// Render a fixed tone directly into an interleaved block -- the flat-monitor
// oracle (a leaf of the recursion identity).
std::vector<float> render_tone(std::uint32_t freq, float amp, const TimeRange& window,
                               std::uint32_t rate, ChannelLayout layout, std::uint32_t frames) {
  ToneContent tone(freq, amp);
  std::vector<float> buf(static_cast<std::size_t>(frames) * channel_count(layout), 0.0F);
  AudioBlock block{buf.data(), frames, layout, rate};
  const AudioRequest req{window, rate, layout, block, Exactness::Exact, StateHandle{}};
  auto done = std::make_shared<AudioCompletion>();
  const std::optional<AudioResult> r = tone.audio()->render_audio(req, done);
  REQUIRE(r.has_value());
  return buf;
}

// Render an already-built + attached nested content's audio into a block.
std::vector<float> render_nested(NestedContent& nested, const TimeRange& window, std::uint32_t rate,
                                 ChannelLayout layout, std::uint32_t frames) {
  std::vector<float> buf(static_cast<std::size_t>(frames) * channel_count(layout), 0.0F);
  AudioBlock block{buf.data(), frames, layout, rate};
  const AudioRequest req{window, rate, layout, block, Exactness::Exact, StateHandle{}};
  auto done = std::make_shared<AudioCompletion>();
  const std::optional<AudioResult> r = nested.audio()->render_audio(req, done);
  REQUIRE(r.has_value());
  return buf;
}

NestedResolver map_resolver(std::unordered_map<ObjectId, Content*>& binding) {
  return [&binding](ObjectId id) -> Content* {
    const auto it = binding.find(id);
    return it != binding.end() ? it->second : nullptr;
  };
}

constexpr std::uint32_t k_rate = 48'000;
constexpr std::uint32_t k_frames = 16;

TimeRange frame_window(std::uint32_t rate, std::uint32_t frames) {
  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(rate);
  return TimeRange{Time::zero(), Time{static_cast<std::int64_t>(frames) * fpf}};
}

} // namespace

// enforces: 12-audio#nested-mixes-child-audio-through-pull
TEST_CASE("nested mixes a two-tone child byte-identically to mixing those tones directly") {
  Model model;
  ToneContent tone_a(440, 0.5F);
  ToneContent tone_b(660, 0.25F);
  std::unordered_map<ObjectId, Content*> binding;
  ObjectId comp{};
  {
    auto tx = model.transact("two tones");
    comp = tx.add_composition(0.0, 0.0);
    const ObjectId ca = tx.add_content(1);
    const ObjectId cb = tx.add_content(1);
    const ObjectId la = tx.add_layer(ca, Affine::identity());
    const ObjectId lb = tx.add_layer(cb, Affine::identity());
    tx.attach_layer(comp, la);
    tx.attach_layer(comp, lb);
    tx.commit();
    binding[ca] = &tone_a;
    binding[cb] = &tone_b;
  }
  const DocStatePtr doc = model.current();
  const TimeRange window = frame_window(k_rate, k_frames);

  CpuBackend backend;
  InlineAudioPull pull;
  NestedContent nested(comp);
  nested.attach(pull, backend, map_resolver(binding), *doc);

  const std::vector<float> got =
      render_nested(nested, window, k_rate, ChannelLayout::Stereo, k_frames);

  // Oracle: the same two tones mixed directly at top level (homogeneous 48 kHz
  // stereo, unit gain), in bottom-to-top order.
  const std::vector<float> a =
      render_tone(440, 0.5F, window, k_rate, ChannelLayout::Stereo, k_frames);
  const std::vector<float> b =
      render_tone(660, 0.25F, window, k_rate, ChannelLayout::Stereo, k_frames);
  std::vector<float> want(a.size(), 0.0F);
  for (std::size_t i = 0; i < a.size(); ++i) {
    want[i] = a[i] + b[i];
  }

  REQUIRE(bytes_equal(got, want));
  // Exactly one pull_audio per audible child layer.
  REQUIRE(pull.pulls() == 2);
}

// enforces: 12-audio#nested-mixes-child-audio-through-pull
TEST_CASE("nested scales by the layer gain and remixes a mono child up to stereo") {
  Model model;
  ToneContent tone(440, 0.5F);
  std::unordered_map<ObjectId, Content*> binding;
  ObjectId comp{};
  ObjectId layer{};
  {
    auto tx = model.transact("mono child");
    comp = tx.add_composition(0.0, 0.0);
    const ObjectId ct = tx.add_content(1);
    layer = tx.add_layer(ct, Affine::identity());
    tx.attach_layer(comp, layer);
    tx.commit();
    binding[ct] = &tone;
  }
  {
    auto tx = model.transact("mono + gain");
    tx.set_working_audio_format(comp, AudioFormat{k_rate, ChannelLayout::Mono});
    tx.set_gain(layer, 0.5);
    tx.commit();
  }
  const DocStatePtr doc = model.current();
  const TimeRange window = frame_window(k_rate, k_frames);

  CpuBackend backend;
  InlineAudioPull pull;
  NestedContent nested(comp);
  nested.attach(pull, backend, map_resolver(binding), *doc);

  const std::vector<float> got =
      render_nested(nested, window, k_rate, ChannelLayout::Stereo, k_frames);

  // Oracle: the mono tone at the child working layout, scaled 0.5 and spread to
  // both stereo channels (the weighted additive mix + mono->stereo remix).
  const std::vector<float> mono =
      render_tone(440, 0.5F, window, k_rate, ChannelLayout::Mono, k_frames);
  std::vector<float> want(static_cast<std::size_t>(k_frames) * 2, 0.0F);
  for (std::uint32_t f = 0; f < k_frames; ++f) {
    const float s = 0.5F * mono[f];
    want[static_cast<std::size_t>(f) * 2] = s;
    want[static_cast<std::size_t>(f) * 2 + 1] = s;
  }
  REQUIRE(bytes_equal(got, want));
}

// enforces: 12-audio#nested-mixes-child-audio-through-pull
TEST_CASE("nested varispeeds a rate-1/2 child by requesting it at the composed rate") {
  Model model;
  ToneContent tone(440, 0.5F);
  std::unordered_map<ObjectId, Content*> binding;
  ObjectId comp{};
  ObjectId layer{};
  {
    auto tx = model.transact("varispeed");
    comp = tx.add_composition(0.0, 0.0);
    const ObjectId ct = tx.add_content(1);
    layer = tx.add_layer(ct, Affine::identity());
    tx.attach_layer(comp, layer);
    tx.commit();
  }
  {
    auto tx = model.transact("rate 1/2");
    tx.set_time_map(layer, TimeMap{Time::zero(), Rational{1, 2}, Time::zero()});
    tx.commit();
  }
  binding[model.current()->find_layer(layer)->content] = &tone;
  const DocStatePtr doc = model.current();
  const TimeRange window = frame_window(k_rate, k_frames);

  CpuBackend backend;
  InlineAudioPull pull;
  NestedContent nested(comp);
  nested.attach(pull, backend, map_resolver(binding), *doc);

  const std::vector<float> got =
      render_nested(nested, window, k_rate, ChannelLayout::Stereo, k_frames);

  // Oracle: a rate-1/2 layer plays its audio at half speed == the tone requested
  // at TWICE the rate (the composed rational rate), placed 1:1 -- one octave down.
  const std::uint32_t child_rate = 2 * k_rate;
  const TimeRange child_window = frame_window(child_rate, k_frames);
  const std::vector<float> want =
      render_tone(440, 0.5F, child_window, child_rate, ChannelLayout::Stereo, k_frames);
  REQUIRE(bytes_equal(got, want));
}

// enforces: 12-audio#nested-mixes-child-audio-through-pull
TEST_CASE("a composition embedding a composition mixes its child audio through two levels") {
  Model model;
  ToneContent tone(440, 0.5F);
  std::unordered_map<ObjectId, Content*> binding;
  ObjectId c1{}; // inner: one tone
  ObjectId c2{}; // outer: one layer embedding a nested-of-c1
  ObjectId c_tone{};
  ObjectId c_inner{};
  {
    auto tx = model.transact("two levels");
    c1 = tx.add_composition(0.0, 0.0);
    c_tone = tx.add_content(1);
    const ObjectId l_tone = tx.add_layer(c_tone, Affine::identity());
    tx.attach_layer(c1, l_tone);
    c2 = tx.add_composition(0.0, 0.0);
    c_inner = tx.add_content(1);
    const ObjectId l_inner = tx.add_layer(c_inner, Affine::identity());
    tx.attach_layer(c2, l_inner);
    tx.commit();
    binding[c_tone] = &tone;
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

  const std::vector<float> got =
      render_nested(outer, window, k_rate, ChannelLayout::Stereo, k_frames);

  // Homogeneous, unit-gain, identity-map two-level nesting == the leaf tone.
  const std::vector<float> want =
      render_tone(440, 0.5F, window, k_rate, ChannelLayout::Stereo, k_frames);
  REQUIRE(bytes_equal(got, want));
}

// enforces: 05-recursive-composition#graph-walk-bounds-cycles
TEST_CASE("a gain<1 Droste audio cycle decays to a finite, stable, deterministic mix") {
  // A composition with a backdrop tone plus a self-embedding layer at gain 0.5: a
  // feedback echo that converges (each turn quieter) and truncates at the budget.
  Model model;
  ToneContent backdrop(440, 0.5F);
  std::unordered_map<ObjectId, Content*> binding;
  ObjectId comp{};
  ObjectId c_self{};
  ObjectId l_self{};
  {
    auto tx = model.transact("droste decay");
    comp = tx.add_composition(0.0, 0.0);
    const ObjectId c_back = tx.add_content(1);
    c_self = tx.add_content(1);
    const ObjectId l_back = tx.add_layer(c_back, Affine::identity());
    l_self = tx.add_layer(c_self, Affine::identity());
    tx.attach_layer(comp, l_back);
    tx.attach_layer(comp, l_self);
    tx.commit();
    binding[c_back] = &backdrop;
  }
  {
    auto tx = model.transact("gain 0.5 self");
    tx.set_gain(l_self, 0.5);
    tx.commit();
  }
  const DocStatePtr doc = model.current();
  const TimeRange window = frame_window(k_rate, k_frames);

  CpuBackend backend;
  InlineAudioPull pull(/*max_depth=*/8);
  NestedContent nested(comp);
  binding[c_self] = &nested; // the cycle edge
  nested.attach(pull, backend, map_resolver(binding), *doc);

  const std::vector<float> first =
      render_nested(nested, window, k_rate, ChannelLayout::Stereo, k_frames);
  // The depth budget fired (the divergent self-reference degraded to silence),
  // yet the mix is finite and bounded (a converging geometric echo).
  REQUIRE(pull.backstops() > 0);
  for (const float v : first) {
    REQUIRE(std::isfinite(v));
    REQUIRE(std::fabs(v) < 4.0F);
  }
  // Deterministic across repeated visits (stable samples).
  const std::vector<float> second =
      render_nested(nested, window, k_rate, ChannelLayout::Stereo, k_frames);
  REQUIRE(bytes_equal(first, second));
}

// enforces: 05-recursive-composition#graph-walk-bounds-cycles
TEST_CASE("a gain>=1 Droste audio cycle terminates on the budget with a silent block") {
  // A composition whose only layer embeds itself at unit gain: a divergent cycle
  // with no independent source -- caught by the depth budget, yielding silence.
  Model model;
  std::unordered_map<ObjectId, Content*> binding;
  ObjectId comp{};
  ObjectId c_self{};
  {
    auto tx = model.transact("droste silent");
    comp = tx.add_composition(0.0, 0.0);
    c_self = tx.add_content(1);
    const ObjectId l_self = tx.add_layer(c_self, Affine::identity()); // gain default 1.0
    tx.attach_layer(comp, l_self);
    tx.commit();
  }
  const DocStatePtr doc = model.current();
  const TimeRange window = frame_window(k_rate, k_frames);

  CpuBackend backend;
  InlineAudioPull pull(/*max_depth=*/8);
  NestedContent nested(comp);
  binding[c_self] = &nested;
  nested.attach(pull, backend, map_resolver(binding), *doc);

  const std::vector<float> got =
      render_nested(nested, window, k_rate, ChannelLayout::Stereo, k_frames);
  REQUIRE(pull.backstops() > 0);
  for (const float v : got) {
    REQUIRE(v == 0.0F); // silent (all-zero) block
  }
}

// enforces: 12-audio#nested-mixes-child-audio-through-pull
// enforces: 05-recursive-composition#nested-boundary-budget-flows-through
TEST_CASE("nested threads the outer snapshot/exactness through every audio pull") {
  Model model;
  ToneContent tone_a(440, 0.5F);
  ToneContent tone_b(660, 0.25F);
  std::unordered_map<ObjectId, Content*> binding;
  ObjectId comp{};
  {
    auto tx = model.transact("two tones");
    comp = tx.add_composition(0.0, 0.0);
    const ObjectId ca = tx.add_content(1);
    const ObjectId cb = tx.add_content(1);
    const ObjectId la = tx.add_layer(ca, Affine::identity());
    const ObjectId lb = tx.add_layer(cb, Affine::identity());
    tx.attach_layer(comp, la);
    tx.attach_layer(comp, lb);
    tx.commit();
    binding[ca] = &tone_a;
    binding[cb] = &tone_b;
  }
  const DocStatePtr doc = model.current();

  struct ARec {
    StateHandle snapshot;
    Exactness exactness;
  };
  class RecordingAudioPull final : public PullService {
  public:
    std::vector<ARec> seen;
    void pull(ContentRef, const RenderRequest&, std::shared_ptr<RenderCompletion> done) override {
      done->fail(RenderError::ContentFailed);
    }
    void pull_audio(ContentRef input, const AudioRequest& request,
                    std::shared_ptr<AudioCompletion> done) override {
      seen.push_back(ARec{request.snapshot, request.exactness});
      AudioFacet* af = input != nullptr ? input->audio() : nullptr;
      const std::optional<AudioResult> r =
          af != nullptr ? af->render_audio(request, done) : std::nullopt;
      if (r.has_value()) {
        done->complete(*r);
      } else if (!done->settled()) {
        done->fail(RenderError::ContentFailed);
      }
    }
  } pull;

  CpuBackend backend;
  NestedContent nested(comp);
  nested.attach(pull, backend, map_resolver(binding), *doc);

  StateHandle snap{};
  snap.slot = 7;
  const TimeRange window = frame_window(k_rate, k_frames);
  std::vector<float> buf(static_cast<std::size_t>(k_frames) * 2, 0.0F);
  AudioBlock block{buf.data(), k_frames, ChannelLayout::Stereo, k_rate};
  const AudioRequest req{window, k_rate, ChannelLayout::Stereo, block, Exactness::Exact, snap};
  auto done = std::make_shared<AudioCompletion>();
  (void)nested.audio()->render_audio(req, done);

  // A depth-1 nested audio scene issues one pull per audible child layer (two),
  // each carrying the outer request's snapshot + exactness verbatim.
  REQUIRE(pull.seen.size() == 2);
  for (const ARec& r : pull.seen) {
    REQUIRE(r.snapshot == snap);
    REQUIRE(r.exactness == Exactness::Exact);
  }
}
