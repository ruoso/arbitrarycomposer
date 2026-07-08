#include <arbc/audio_engine/mix.hpp>

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/cache/key_shapes.hpp>
#include <arbc/compositor/counters.hpp>
#include <arbc/compositor/pull_service.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/kind_nested/nested_content.hpp>
#include <arbc/kind_tone/tone_content.hpp>
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

// Byte-exact goldens + behavioral counters for `arbc::audio-engine`'s composition
// mixer driven through the REAL machinery -- `compositor`'s `PullServiceImpl::
// pull_audio` and the `org.arbc.tone` / `org.arbc.nested` reference kinds (doc
// 12:11-21,150-208). Cross-component, so it lives in top-level `tests/` (which is
// not level-checked): the mix engine (L4) + the concrete pull (L4) + the kinds
// (L3) are only assembled here, exactly as `runtime` will assemble them. The
// pure-unit mix behavior with local doubles lives in `src/audio_engine/t/mix.t.cpp`.

namespace {

using namespace arbc;

// A stateless below-native-rate audio source: asked above its native rate it
// reports achieved_rate == native / exact == false, driving the mixer's
// windowed-sinc reconstruction through the real `resample_audio`. A byte-exact
// parabolic sine over an exact integer flick phase (never std::sin).
class BelowRateSource final : public Content {
public:
  explicit BelowRateSource(std::uint32_t native_rate) : d_facet(native_rate) {}
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
      const std::int64_t fps = Time::flicks_per_second;
      const std::int64_t fpf = fps / static_cast<std::int64_t>(request.sample_rate);
      for (std::uint32_t f = 0; f < request.target.frames; ++f) {
        std::int64_t t = (request.window.start.flicks + static_cast<std::int64_t>(f) * fpf) % fps;
        if (t < 0) {
          t += fps;
        }
        const std::int64_t r = (static_cast<std::int64_t>(3000) * t) % fps;
        double p = 2.0 * (static_cast<double>(r) / static_cast<double>(fps));
        if (p > 1.0) {
          p -= 2.0;
        }
        const double abs_p = p < 0.0 ? -p : p;
        const float v = static_cast<float>(0.6 * (4.0 * p * (1.0 - abs_p)));
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

// The real per-content pull, wired to render inline through `direct_audio_dispatch`
// (a single-threaded monitor / test), counting dispatches via `CompositorCounters`.
struct RealPull {
  CpuBackend backend;
  TileCache cache{64u * 1024 * 1024};
  CompositorCounters counters;
  std::unique_ptr<PullServiceImpl> service;

  explicit RealPull(const std::unordered_map<const Content*, ObjectId>& ids) {
    PullConfig config;
    config.counters = &counters;
    config.id_of = [&ids](const Content* c) {
      const auto it = ids.find(c);
      return it != ids.end() ? it->second : ObjectId{};
    };
    config.contribution = [](const Content*) { return std::uint64_t{0}; };
    config.audio_dispatch = direct_audio_dispatch();
    service = std::make_unique<PullServiceImpl>(cache, backend, direct_dispatch(), config);
  }
};

MixResolver map_resolver(const std::unordered_map<ObjectId, Content*>& binding) {
  return [&binding](ObjectId id) -> Content* {
    const auto it = binding.find(id);
    return it != binding.end() ? it->second : nullptr;
  };
}

std::vector<float> mix(const DocRoot& doc, ObjectId comp, const MixResolver& resolve,
                       PullService& pull, std::uint32_t rate, std::uint32_t frames,
                       AudioResult& out_result) {
  std::vector<float> buf(static_cast<std::size_t>(frames) * 2, 0.0F);
  AudioBlock block{buf.data(), frames, ChannelLayout::Stereo, rate};
  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(rate);
  const AudioRequest req{TimeRange{Time::zero(), Time{static_cast<std::int64_t>(frames) * fpf}},
                         rate,
                         ChannelLayout::Stereo,
                         block,
                         Exactness::Exact,
                         StateHandle{}};
  out_result = mix_composition(doc, comp, resolve, pull, req);
  return buf;
}

std::vector<float> render_tone(ToneContent& tone, std::uint32_t rate, std::uint32_t frames) {
  std::vector<float> buf(static_cast<std::size_t>(frames) * 2, 0.0F);
  AudioBlock block{buf.data(), frames, ChannelLayout::Stereo, rate};
  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(rate);
  const AudioRequest req{TimeRange{Time::zero(), Time{static_cast<std::int64_t>(frames) * fpf}},
                         rate,
                         ChannelLayout::Stereo,
                         block,
                         Exactness::Exact,
                         StateHandle{}};
  auto done = std::make_shared<AudioCompletion>();
  (void)tone.audio()->render_audio(req, done);
  return buf;
}

bool bytes_equal(const std::vector<float>& a, const std::vector<float>& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

constexpr std::uint32_t k_rate = 48'000;
constexpr std::uint32_t k_frames = 32;

} // namespace

// enforces: 12-audio#mix-engine-mixes-layers-additively
TEST_CASE("mix engine sums tones additively through the real PullServiceImpl, byte-exact") {
  Model model;
  ToneContent tone_a(440, 0.5F);
  ToneContent tone_b(660, 0.25F);
  std::unordered_map<ObjectId, Content*> binding;
  std::unordered_map<const Content*, ObjectId> ids;
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
    tx.set_gain(lb, 0.5);
    tx.commit();
    binding[ca] = &tone_a;
    binding[cb] = &tone_b;
    ids[&tone_a] = ca;
    ids[&tone_b] = cb;
  }
  const DocStatePtr doc = model.current();
  RealPull pull(ids);

  AudioResult result{};
  const std::vector<float> got =
      mix(*doc, comp, map_resolver(binding), *pull.service, k_rate, k_frames, result);

  // Hand-summed reference: tone_a at gain 1, tone_b at gain 0.5, summed sample-wise.
  const std::vector<float> ra = render_tone(tone_a, k_rate, k_frames);
  const std::vector<float> rb = render_tone(tone_b, k_rate, k_frames);
  std::vector<float> want(static_cast<std::size_t>(k_frames) * 2, 0.0F);
  for (std::size_t i = 0; i < want.size(); ++i) {
    want[i] = ra[i] + 0.5F * rb[i];
  }

  REQUIRE(bytes_equal(got, want));
  REQUIRE(result.achieved_rate == k_rate);
  REQUIRE(result.exact);
  REQUIRE(pull.counters.audio_dispatches() == 2); // one real dispatch per tone
}

TEST_CASE("mix engine: a nested-of-tones layer equals the same tones mixed flat, byte-exact") {
  // Flat: two tones as direct layers of a top composition.
  // Nested: one layer bound to org.arbc.nested wrapping a child composition of the
  // same two tones. Both are mixed through the SAME real PullServiceImpl -- the
  // audio "rendering is recursion" identity, now through real cache-first machinery.
  Model model;
  ToneContent tone_a(440, 0.5F);
  ToneContent tone_b(660, 0.25F);

  std::unordered_map<ObjectId, Content*> binding;
  std::unordered_map<const Content*, ObjectId> ids;
  ObjectId flat_comp{};
  ObjectId inner_comp{};
  ObjectId outer_comp{};
  ObjectId nested_content_id{};
  {
    auto tx = model.transact("flat + nested scenes");
    // Flat composition.
    flat_comp = tx.add_composition(0.0, 0.0);
    const ObjectId ca = tx.add_content(1);
    const ObjectId cb = tx.add_content(1);
    tx.attach_layer(flat_comp, tx.add_layer(ca, Affine::identity()));
    tx.attach_layer(flat_comp, tx.add_layer(cb, Affine::identity()));
    // Inner composition (the nested child) with the same two tones.
    inner_comp = tx.add_composition(0.0, 0.0);
    const ObjectId ia = tx.add_content(1);
    const ObjectId ib = tx.add_content(1);
    tx.attach_layer(inner_comp, tx.add_layer(ia, Affine::identity()));
    tx.attach_layer(inner_comp, tx.add_layer(ib, Affine::identity()));
    // Outer composition with a single layer bound to the nested content.
    outer_comp = tx.add_composition(0.0, 0.0);
    nested_content_id = tx.add_content(1);
    tx.attach_layer(outer_comp, tx.add_layer(nested_content_id, Affine::identity()));
    tx.commit();
    binding[ca] = &tone_a;
    binding[cb] = &tone_b;
    binding[ia] = &tone_a;
    binding[ib] = &tone_b;
    ids[&tone_a] = ca; // the id_of only needs to answer for cache-keying (blocks unused here)
    ids[&tone_b] = cb;
  }
  const DocStatePtr doc = model.current();
  RealPull pull(ids);

  // Attach the nested content to the same real service + resolver.
  NestedContent nested(inner_comp);
  nested.attach(*pull.service, pull.backend, map_resolver(binding), *doc);
  binding[nested_content_id] = &nested;
  ids[&nested] = nested_content_id;

  AudioResult flat_result{};
  const std::vector<float> flat =
      mix(*doc, flat_comp, map_resolver(binding), *pull.service, k_rate, k_frames, flat_result);

  AudioResult nested_result{};
  const std::vector<float> via_nested =
      mix(*doc, outer_comp, map_resolver(binding), *pull.service, k_rate, k_frames, nested_result);

  REQUIRE(bytes_equal(via_nested, flat));
  REQUIRE(nested_result.achieved_rate == k_rate);
  REQUIRE(nested_result.exact);
}

TEST_CASE("mix engine reconstructs a below-rate child through resample_audio, reporting honestly") {
  Model model;
  BelowRateSource slow(24'000); // native rate = half the working rate
  std::unordered_map<ObjectId, Content*> binding;
  std::unordered_map<const Content*, ObjectId> ids;
  ObjectId comp{};
  {
    auto tx = model.transact("below-rate");
    comp = tx.add_composition(0.0, 0.0);
    const ObjectId cs = tx.add_content(1);
    tx.attach_layer(comp, tx.add_layer(cs, Affine::identity()));
    tx.commit();
    binding[cs] = &slow;
    ids[&slow] = cs;
  }
  const DocStatePtr doc = model.current();
  RealPull pull(ids);

  AudioResult result{};
  const std::vector<float> got =
      mix(*doc, comp, map_resolver(binding), *pull.service, k_rate, k_frames, result);

  // Honesty: the aggregate reports the child's native rate and stays inexact even
  // though the samples are band-limit-reconstructed (not a hold).
  REQUIRE(result.achieved_rate == 24'000);
  REQUIRE_FALSE(result.exact);
  // Discovery pull (reports the native rate) + a native-rate re-request feeding the
  // reconstruction: two real dispatches for the one below-rate layer.
  REQUIRE(pull.counters.audio_dispatches() == 2);
  bool any_nonzero = false;
  for (const float v : got) {
    if (v != 0.0F) {
      any_nonzero = true;
      break;
    }
  }
  REQUIRE(any_nonzero);
}

// enforces: 12-audio#mix-engine-facetless-costs-nothing
TEST_CASE("mix engine dispatches zero for an all-culled composition and N for N audible tones") {
  Model model;
  ToneContent tone_a(440, 0.5F);
  ToneContent tone_b(660, 0.25F);
  ToneContent tone_c(880, 0.25F);

  std::unordered_map<ObjectId, Content*> binding;
  std::unordered_map<const Content*, ObjectId> ids;
  ObjectId audible_comp{};
  ObjectId culled_comp{};
  {
    auto tx = model.transact("audible + culled");
    // Three audible in-span tone layers.
    audible_comp = tx.add_composition(0.0, 0.0);
    const ObjectId ca = tx.add_content(1);
    const ObjectId cb = tx.add_content(1);
    const ObjectId cc = tx.add_content(1);
    tx.attach_layer(audible_comp, tx.add_layer(ca, Affine::identity()));
    tx.attach_layer(audible_comp, tx.add_layer(cb, Affine::identity()));
    tx.attach_layer(audible_comp, tx.add_layer(cc, Affine::identity()));
    // An all-culled composition: inaudible + zero-gain tone layers.
    culled_comp = tx.add_composition(0.0, 0.0);
    const ObjectId cd = tx.add_content(1);
    const ObjectId ce = tx.add_content(1);
    const ObjectId ld = tx.add_layer(cd, Affine::identity());
    const ObjectId le = tx.add_layer(ce, Affine::identity());
    tx.attach_layer(culled_comp, ld);
    tx.attach_layer(culled_comp, le);
    tx.set_audible(ld, false);
    tx.set_gain(le, 0.0);
    tx.commit();
    binding[ca] = &tone_a;
    binding[cb] = &tone_b;
    binding[cc] = &tone_c;
    binding[cd] = &tone_a;
    binding[ce] = &tone_b;
    ids[&tone_a] = ca;
    ids[&tone_b] = cb;
    ids[&tone_c] = cc;
  }
  const DocStatePtr doc = model.current();

  RealPull culled_pull(ids);
  AudioResult culled_result{};
  const std::vector<float> culled =
      mix(*doc, culled_comp, map_resolver(binding), *culled_pull.service, k_rate, k_frames,
          culled_result);
  REQUIRE(culled_pull.counters.audio_dispatches() == 0); // costs the audio engine nothing
  for (const float v : culled) {
    REQUIRE(v == 0.0F);
  }

  RealPull audible_pull(ids);
  AudioResult audible_result{};
  mix(*doc, audible_comp, map_resolver(binding), *audible_pull.service, k_rate, k_frames,
      audible_result);
  REQUIRE(audible_pull.counters.audio_dispatches() == 3); // one per audible in-span layer
}
