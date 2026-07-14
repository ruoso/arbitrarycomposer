#include <arbc/audio_engine/lookahead.hpp>
#include <arbc/audio_engine/mix.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/cache/key_shapes.hpp>
#include <arbc/cache/keyed_store.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/media/audio_block.hpp>
#include <arbc/model/model.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

// Behavioral + byte-exact unit tests for `arbc::audio-engine`'s lookahead ring
// (`LookaheadRing`, doc 12:31-34,155-190). Lives in the component's own `t/`, so its
// includes stay inside the audio-engine dependency closure (contract / cache /
// model / media / base): it drives the ring with LOCAL Content / AudioFacet /
// PullService doubles and a LOCAL block-cache value (the compositor's
// `AudioBlockValue`/`PullServiceImpl` are an L4 peer, exercised in `tests/`). The
// ring's cache-touching methods are templated on the block value, so this test
// instantiates them over a local `KeyedStore<BlockKey, LocalBlock>`.

namespace {

using namespace arbc;

// Byte-exact procedural sine over an EXACT integer flick phase (never std::sin) --
// the same discipline as `mix.t.cpp` / `org.arbc.tone`.
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

// A tone leaf that additionally declares a constant processing `latency()` (a live
// pipeline / lookahead limiter). Its `render_audio` is byte-identical to `SineLeaf`'s
// (latency is a residency/scheduling quantity, not a signal transform, doc 12:200-212),
// so a latent scene drains byte-identical to the same scene with `latency() == zero`.
class LatentSineLeaf final : public Content {
public:
  LatentSineLeaf(std::uint32_t freq_hz, float amp, Time latency) : d_facet(freq_hz, amp, latency) {}
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
    Facet(std::uint32_t freq_hz, float amp, Time latency)
        : d_freq(freq_hz), d_amp(amp), d_latency(latency) {}
    std::optional<TimeRange> audio_extent() const override { return std::nullopt; }
    Stability audio_stability() const override { return Stability::Live; }
    Time latency() const override { return d_latency; }
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
    Time d_latency;
  };
  Facet d_facet;
};

// A local 1D block-cache value mirroring `compositor::AudioBlockValue`, so the ring's
// templated `prime`/`reprime`/`invalidate` instantiate here without naming the peer.
struct LocalBlock {
  std::vector<float> samples;
  std::uint32_t frames{0};
  ChannelLayout layout{ChannelLayout::Stereo};
  std::uint32_t rate{0};
  AudioResult meta{};
};
using LocalCache = KeyedStore<BlockKey, LocalBlock>;

std::int64_t local_block_index(const AudioRequest& request) {
  if (request.sample_rate == 0) {
    return 0;
  }
  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(request.sample_rate);
  return fpf != 0 ? request.window.start.flicks / fpf : 0;
}

// A local `PullServiceImpl` twin: cache-first single-settle over `LocalCache`, an
// injected `id_of`, and a doc-global revision. Counts dispatches (misses).
class CachingPull final : public PullService {
public:
  CachingPull(LocalCache* blocks, std::function<ObjectId(const Content*)> id_of,
              std::uint64_t revision)
      : d_blocks(blocks), d_id_of(std::move(id_of)), d_revision(revision) {}
  void pull(ContentRef, const RenderRequest&, std::shared_ptr<RenderCompletion> done) override {
    done->fail(RenderError::ContentFailed);
  }
  void pull_audio(ContentRef input, const AudioRequest& request,
                  std::shared_ptr<AudioCompletion> done) override {
    if (d_blocks != nullptr) {
      const BlockKey key{d_id_of ? d_id_of(input) : ObjectId{}, d_revision,
                         local_block_index(request), request.sample_rate};
      if (std::optional<CacheHold<LocalBlock>> hit = d_blocks->lookup(key);
          hit.has_value() && hit->get().meta.exact &&
          hit->get().meta.achieved_rate == request.sample_rate &&
          hit->get().frames == request.target.frames &&
          hit->get().layout == request.target.layout) {
        const LocalBlock& value = hit->get();
        const std::size_t n = static_cast<std::size_t>(value.frames) * channel_count(value.layout);
        if (request.target.samples != nullptr && value.samples.size() >= n) {
          for (std::size_t i = 0; i < n; ++i) {
            request.target.samples[i] = value.samples[i];
          }
        }
        done->complete(value.meta);
        return; // ZERO dispatch on a resident exact-fresh hit
      }
    }
    ++d_dispatches;
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
  int dispatches() const { return d_dispatches; }

private:
  LocalCache* d_blocks;
  std::function<ObjectId(const Content*)> d_id_of;
  std::uint64_t d_revision;
  int d_dispatches{0};
};

// A composition of caller-owned tone layers (as `mix.t.cpp`'s Scene), tracking the
// content->id and id->content maps the ring/pull need.
struct Scene {
  Model model;
  std::unordered_map<ObjectId, Content*> binding;
  std::unordered_map<const Content*, ObjectId> ids;
  ObjectId comp{};

  void add(Content* content) {
    auto tx = model.transact("add");
    const ObjectId cid = tx.add_content(1);
    const ObjectId layer = tx.add_layer(cid, Affine::identity());
    if (comp == ObjectId{}) {
      comp = tx.add_composition(0.0, 0.0);
    }
    tx.attach_layer(comp, layer);
    tx.commit();
    binding[cid] = content;
    ids[content] = cid;
  }

  MixResolver resolver() {
    return [this](ObjectId id) -> Content* {
      const auto it = binding.find(id);
      return it != binding.end() ? it->second : nullptr;
    };
  }
  std::function<ObjectId(const Content*)> id_of() {
    return [this](const Content* c) {
      const auto it = ids.find(c);
      return it != ids.end() ? it->second : ObjectId{};
    };
  }
};

bool bytes_equal(const std::vector<float>& a, const std::vector<float>& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

constexpr std::uint32_t k_rate = 48'000;
constexpr std::uint32_t k_block_frames = 32;

// The direct mix_composition reference for one output block index.
std::vector<float> direct_mix(const DocRoot& doc, ObjectId comp, const MixResolver& resolve,
                              PullService& pull, std::int64_t index) {
  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(k_rate);
  const std::int64_t span = static_cast<std::int64_t>(k_block_frames) * fpf;
  std::vector<float> buf(static_cast<std::size_t>(k_block_frames) * 2, 0.0F);
  AudioBlock block{buf.data(), k_block_frames, ChannelLayout::Stereo, k_rate};
  const AudioRequest req{TimeRange{Time{index * span}, Time{index * span + span}},
                         k_rate,
                         ChannelLayout::Stereo,
                         block,
                         Exactness::BestEffort,
                         StateHandle{}};
  mix_composition(doc, comp, resolve, pull, req);
  return buf;
}

LookaheadRingConfig ring_config(Scene& scene) {
  LookaheadRingConfig cfg;
  cfg.composition = scene.comp;
  cfg.resolve = scene.resolver();
  cfg.sample_rate = k_rate;
  cfg.layout = ChannelLayout::Stereo;
  cfg.block_frames = k_block_frames;
  cfg.contribution = [rev = std::uint64_t{0}](ObjectId) { return rev; };
  return cfg;
}

// Render + insert a want into the cache exactly as the pump's worker path would.
void fill_want(LocalCache& cache, const MixResolver& resolve, const PrefetchWant& w) {
  Content* content = resolve(w.content);
  std::vector<float> buf(static_cast<std::size_t>(w.frames) * channel_count(w.layout), 0.0F);
  AudioBlock block{buf.data(), w.frames, w.layout, w.rate};
  const AudioRequest req{w.window, w.rate, w.layout, block, Exactness::BestEffort, StateHandle{}};
  auto done = std::make_shared<AudioCompletion>();
  const std::optional<AudioResult> r = content->audio()->render_audio(req, done);
  const AudioResult meta = r.has_value() ? *r : AudioResult{w.rate, false};
  const std::size_t bytes = buf.size() * sizeof(float);
  cache.insert(w.key, LocalBlock{std::move(buf), w.frames, w.layout, w.rate, meta}, bytes,
               PriorityClass::Temporal);
}

} // namespace

// enforces: 12-audio#lookahead-prepares-ahead-of-playhead
TEST_CASE("a primed ring drained in order is byte-identical to a direct per-window mix") {
  SineLeaf a(300, 0.6F);
  SineLeaf b(700, 0.4F);
  Scene scene;
  scene.add(&a);
  scene.add(&b);
  const DocStatePtr doc = scene.model.current();

  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(k_rate);
  const std::int64_t span = static_cast<std::int64_t>(k_block_frames) * fpf;

  // Inline path (no cache): prime the horizon, then drain each block.
  CachingPull pull(nullptr, scene.id_of(), 0);
  LookaheadRing ring(*doc, pull, ring_config(scene));
  ring.prime<LocalBlock>(nullptr, Time::zero(), Time{3 * span}, +1); // blocks 0..3

  REQUIRE(ring.blocks_mixed() == 4);
  REQUIRE(ring.prepared_count() == 4);

  CachingPull ref_pull(nullptr, scene.id_of(), 0);
  for (std::int64_t i = 0; i < 4; ++i) {
    std::vector<float> got(static_cast<std::size_t>(k_block_frames) * 2, 0.0F);
    AudioBlock out{got.data(), k_block_frames, ChannelLayout::Stereo, k_rate};
    AudioResult meta{};
    REQUIRE(ring.drain(i, out, meta));
    const std::vector<float> want = direct_mix(*doc, scene.comp, scene.resolver(), ref_pull, i);
    REQUIRE(bytes_equal(got, want));
  }
  // The drain path mixed nothing: every mix was on the prime path.
  REQUIRE(ring.blocks_mixed() == 4);
}

// The direct Spatial reference for one output block: mix_composition in Spatial mode
// with the seed on the request, post-scaled by the camera's uniform scale-attenuation
// -- exactly what `LookaheadRing::mix_block` does when `config.spatial` is set.
std::vector<float> spatial_direct_mix(const DocRoot& doc, ObjectId comp, const MixResolver& resolve,
                                      PullService& pull, std::int64_t index,
                                      const Spatialization& seed) {
  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(k_rate);
  const std::int64_t span = static_cast<std::int64_t>(k_block_frames) * fpf;
  std::vector<float> buf(static_cast<std::size_t>(k_block_frames) * 2, 0.0F);
  AudioBlock block{buf.data(), k_block_frames, ChannelLayout::Stereo, k_rate};
  const AudioRequest req{TimeRange{Time{index * span}, Time{index * span + span}},
                         k_rate,
                         ChannelLayout::Stereo,
                         block,
                         Exactness::BestEffort,
                         StateHandle{},
                         seed};
  mix_composition(doc, comp, resolve, pull, req, MixPolicy::Spatial);
  for (float& v : buf) {
    v *= seed.accum_atten; // the camera post-scale
  }
  return buf;
}

// enforces: 12-audio#spatial-attenuates-by-composed-scale
TEST_CASE("the ring threads a Spatial seed: threaded fill == inline Spatial mix, silence_mixed 0") {
  SineLeaf a(300, 0.6F);
  SineLeaf b(700, 0.4F);
  Scene scene;
  scene.add(&a);
  scene.add(&b);
  const DocStatePtr doc = scene.model.current();

  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(k_rate);
  const std::int64_t span = static_cast<std::int64_t>(k_block_frames) * fpf;

  // A camera at half scale (uniform attenuation 0.5) seeds the accumulated attenuation
  // AND the post-scale (as a monitor would compute from `max_scale(listener)`).
  const Affine listener = Affine::scaling(0.5, 0.5);
  const Spatialization seed{listener, 100.0, 100.0, spatial_edge_atten(listener),
                            k_sub_audible_atten};

  // Inline fill (no cache): `mix_block` mixes each block Spatially and post-scales.
  CachingPull inline_pull(nullptr, scene.id_of(), 0);
  LookaheadRingConfig icfg = ring_config(scene);
  icfg.policy = MixPolicy::Spatial;
  icfg.spatial = seed;
  LookaheadRing iring(*doc, inline_pull, icfg);
  iring.prime<LocalBlock>(nullptr, Time::zero(), Time{3 * span}, +1);
  REQUIRE(iring.blocks_mixed() == 4);
  REQUIRE(iring.silence_mixed() == 0);

  // Threaded fill (cache-warmed): prime in rounds, filling each want, until the whole
  // closure is resident and the blocks mix at full transitive residency.
  LocalCache cache{64u * 1024 * 1024};
  CachingPull cache_pull(&cache, scene.id_of(), 0);
  LookaheadRingConfig ccfg = ring_config(scene);
  ccfg.policy = MixPolicy::Spatial;
  ccfg.spatial = seed;
  LookaheadRing cring(*doc, cache_pull, ccfg);
  std::vector<PrefetchWant> wants;
  do {
    wants = cring.prime<LocalBlock>(&cache, Time::zero(), Time{3 * span}, +1);
    for (const PrefetchWant& w : wants) {
      fill_want(cache, ccfg.resolve, w);
    }
  } while (!wants.empty());
  REQUIRE(cring.blocks_mixed() == 4);
  REQUIRE(cring.silence_mixed() == 0); // never mixed silence for an absent descendant

  // The threaded drain is byte-identical to the inline drain AND to the direct Spatial
  // oracle (the warmed superset covers exactly what the culling mixer pulls).
  CachingPull ref_pull(nullptr, scene.id_of(), 0);
  for (std::int64_t i = 0; i < 4; ++i) {
    std::vector<float> got_inline(static_cast<std::size_t>(k_block_frames) * 2, 0.0F);
    std::vector<float> got_threaded(static_cast<std::size_t>(k_block_frames) * 2, 0.0F);
    AudioBlock oi{got_inline.data(), k_block_frames, ChannelLayout::Stereo, k_rate};
    AudioBlock ot{got_threaded.data(), k_block_frames, ChannelLayout::Stereo, k_rate};
    AudioResult mi{};
    AudioResult mt{};
    REQUIRE(iring.drain(i, oi, mi));
    REQUIRE(cring.drain(i, ot, mt));
    REQUIRE(bytes_equal(got_inline, got_threaded));
    const std::vector<float> want =
        spatial_direct_mix(*doc, scene.comp, scene.resolver(), ref_pull, i, seed);
    REQUIRE(bytes_equal(got_inline, want));
  }
}

// enforces: 12-audio#lookahead-prepares-ahead-of-playhead
TEST_CASE("draining a not-yet-prepared block yields silence and an underrun, never a mix") {
  SineLeaf a(300, 0.6F);
  Scene scene;
  scene.add(&a);
  const DocStatePtr doc = scene.model.current();
  CachingPull pull(nullptr, scene.id_of(), 0);
  LookaheadRing ring(*doc, pull, ring_config(scene));
  // No prime: nothing is prepared.
  std::vector<float> got(static_cast<std::size_t>(k_block_frames) * 2, 1.0F);
  AudioBlock out{got.data(), k_block_frames, ChannelLayout::Stereo, k_rate};
  AudioResult meta{};
  REQUIRE_FALSE(ring.drain(7, out, meta));
  for (const float v : got) {
    REQUIRE(v == 0.0F); // silence
  }
  REQUIRE(ring.underruns() == 1);
  REQUIRE(ring.blocks_mixed() == 0); // drain never mixed
}

// enforces: 12-audio#lookahead-fills-block-cache-through-prefetch-ring
TEST_CASE("priming classifies contributors onto the prefetch ring and fills the block cache") {
  SineLeaf a(300, 0.6F);
  SineLeaf b(700, 0.4F);
  Scene scene;
  scene.add(&a);
  scene.add(&b);
  const DocStatePtr doc = scene.model.current();

  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(k_rate);
  const std::int64_t span = static_cast<std::int64_t>(k_block_frames) * fpf;

  LocalCache cache{64u * 1024 * 1024};
  CachingPull pull(&cache, scene.id_of(), 0);
  LookaheadRing ring(*doc, pull, ring_config(scene));

  // First prime over a cold cache: every contributor block is absent (a want), and
  // no block is mixed yet (the mixer would miss).
  std::vector<PrefetchWant> wants =
      ring.prime<LocalBlock>(&cache, Time::zero(), Time{span}, +1); // blocks 0..1
  REQUIRE(wants.size() == 4);                                       // 2 tones x 2 blocks
  REQUIRE(ring.blocks_mixed() == 0); // nothing mixed against a cold cache

  // Fill the wants (the pump's worker path), then re-prime: now resident -> mixed.
  for (const PrefetchWant& w : wants) {
    fill_want(cache, scene.resolver(), w);
  }
  const int dispatches_before = pull.dispatches();
  const std::vector<PrefetchWant> wants2 =
      ring.prime<LocalBlock>(&cache, Time::zero(), Time{span}, +1);
  REQUIRE(wants2.empty());           // fully warm
  REQUIRE(ring.blocks_mixed() == 2); // both blocks mixed
  // The mix pass hit the warmed cache for every contributor: ZERO new dispatch.
  REQUIRE(pull.dispatches() == dispatches_before);

  // A subsequent direct pull_audio for a filled key hits with zero dispatch.
  const int dispatches_probe = pull.dispatches();
  std::vector<float> probe(static_cast<std::size_t>(k_block_frames) * 2, 0.0F);
  AudioBlock pblock{probe.data(), k_block_frames, ChannelLayout::Stereo, k_rate};
  const AudioRequest preq{TimeRange{Time::zero(), Time{span}},
                          k_rate,
                          ChannelLayout::Stereo,
                          pblock,
                          Exactness::BestEffort,
                          StateHandle{}};
  auto pdone = std::make_shared<AudioCompletion>();
  pull.pull_audio(&a, preq, pdone);
  REQUIRE(pdone->settled());
  REQUIRE(pull.dispatches() == dispatches_probe); // 0 dispatch: a resident hit
}

// enforces: 12-audio#lookahead-transport-change-flushes-and-reprimes
TEST_CASE("reprime retains overlapping blocks and re-mixes only the newly-needed ones") {
  SineLeaf a(440, 0.5F);
  Scene scene;
  scene.add(&a);
  const DocStatePtr doc = scene.model.current();

  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(k_rate);
  const std::int64_t span = static_cast<std::int64_t>(k_block_frames) * fpf;

  CachingPull pull(nullptr, scene.id_of(), 0);
  LookaheadRing ring(*doc, pull, ring_config(scene));
  ring.prime<LocalBlock>(nullptr, Time::zero(), Time{3 * span}, +1); // blocks 0,1,2,3
  REQUIRE(ring.blocks_mixed() == 4);

  // Nudge the playhead forward by one block: window is now blocks 1..4. Blocks 1,2,3
  // are RETAINED (not re-mixed); block 0 is flushed; only block 4 is newly mixed.
  ring.reprime<LocalBlock>(nullptr, Time{span}, Time{3 * span}, +1); // blocks 1,2,3,4
  REQUIRE(ring.blocks_mixed() == 5);                                 // 4 + exactly one new block
  REQUIRE_FALSE(ring.is_prepared(0));                                // flushed
  REQUIRE(ring.is_prepared(4));                                      // freshly mixed
  REQUIRE(ring.prepared_count() == 4);
}

// enforces: 12-audio#lookahead-damage-remixes-prepared-blocks
TEST_CASE("invalidate drops exactly the overlapped prepared blocks and re-mixes them") {
  SineLeaf a(440, 0.5F);
  Scene scene;
  scene.add(&a);
  const DocStatePtr doc = scene.model.current();

  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(k_rate);
  const std::int64_t span = static_cast<std::int64_t>(k_block_frames) * fpf;

  CachingPull pull(nullptr, scene.id_of(), 0);
  LookaheadRing ring(*doc, pull, ring_config(scene));
  ring.prime<LocalBlock>(nullptr, Time::zero(), Time{3 * span}, +1); // blocks 0,1,2,3
  REQUIRE(ring.blocks_mixed() == 4);

  // Damage a range strictly inside block 2's window: only block 2 is invalidated.
  const TimeRange dmg{Time{2 * span + 10}, Time{2 * span + 20}};
  ring.invalidate<LocalBlock>(nullptr, dmg);
  REQUIRE_FALSE(ring.is_prepared(2));
  REQUIRE(ring.is_prepared(0));
  REQUIRE(ring.is_prepared(1));
  REQUIRE(ring.is_prepared(3));
  REQUIRE(ring.prepared_count() == 3);

  // The next prime re-mixes exactly the one dropped block.
  ring.prime<LocalBlock>(nullptr, Time::zero(), Time{3 * span}, +1);
  REQUIRE(ring.blocks_mixed() == 5); // 4 + the one re-mixed block
  REQUIRE(ring.is_prepared(2));
}

namespace {

// A below-rate leaf: honors at/below its native rate, reports achieved==native /
// exact==false above it -- the below-rate contributor the ring's native re-request
// warming targets (a tone always honors, so it cannot exercise this path).
class BelowRateLeaf final : public Content {
public:
  explicit BelowRateLeaf(std::uint32_t native_rate) : d_native_rate(native_rate), d_facet(this) {}
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
    explicit Facet(BelowRateLeaf* owner) : d_owner(owner) {}
    std::optional<TimeRange> audio_extent() const override { return std::nullopt; }
    Stability audio_stability() const override { return Stability::Static; }
    std::optional<AudioResult> render_audio(const AudioRequest& request,
                                            std::shared_ptr<AudioCompletion>) override {
      const std::uint32_t ch = channel_count(request.layout);
      for (std::uint32_t f = 0; f < request.target.frames; ++f) {
        for (std::uint32_t c = 0; c < ch; ++c) {
          request.target.samples[static_cast<std::size_t>(f) * ch + c] = 0.25F;
        }
      }
      const std::uint32_t achieved = std::min(request.sample_rate, d_owner->d_native_rate);
      return AudioResult{achieved, request.sample_rate <= d_owner->d_native_rate};
    }

  private:
    BelowRateLeaf* d_owner;
  };
  std::uint32_t d_native_rate;
  Facet d_facet;
};

} // namespace

// enforces: 12-audio#lookahead-warms-recursive-contributor-closure
TEST_CASE("the fill descends recursively and gates a nested contributor on its child closure") {
  // A root composition embedding a (structurally simulated) nested composition of two
  // leaves. The ring descends via the injected `nested_composition` enumerator (an L3
  // NestedContent is exercised in tests/); here a leaf stands in for the nested
  // content object, and the enumerator supplies the child-composition edge.
  SineLeaf nested_stub(0, 0.0F); // the nested contributor placeholder (has an audio facet)
  SineLeaf a(300, 0.6F);
  SineLeaf b(700, 0.4F);

  Model model;
  std::unordered_map<ObjectId, Content*> binding;
  std::unordered_map<const Content*, ObjectId> ids;
  ObjectId c_inner{};
  ObjectId c_root{};
  ObjectId c_nest{};
  {
    auto tx = model.transact("nested scene");
    c_inner = tx.add_composition(0.0, 0.0);
    const ObjectId ca = tx.add_content(1);
    const ObjectId cb = tx.add_content(1);
    tx.attach_layer(c_inner, tx.add_layer(ca, Affine::identity()));
    tx.attach_layer(c_inner, tx.add_layer(cb, Affine::identity()));
    c_root = tx.add_composition(0.0, 0.0);
    c_nest = tx.add_content(1);
    tx.attach_layer(c_root, tx.add_layer(c_nest, Affine::identity()));
    tx.commit();
    binding[ca] = &a;
    binding[cb] = &b;
    binding[c_nest] = &nested_stub;
    ids[&a] = ca;
    ids[&b] = cb;
    ids[&nested_stub] = c_nest;
  }
  const DocStatePtr doc = model.current();

  LocalCache cache{64u * 1024 * 1024};
  CachingPull pull(
      &cache,
      [&ids](const Content* c) {
        const auto it = ids.find(c);
        return it != ids.end() ? it->second : ObjectId{};
      },
      0);

  LookaheadRingConfig cfg;
  cfg.composition = c_root;
  cfg.resolve = [&binding](ObjectId id) -> Content* {
    const auto it = binding.find(id);
    return it != binding.end() ? it->second : nullptr;
  };
  cfg.sample_rate = k_rate;
  cfg.layout = ChannelLayout::Stereo;
  cfg.block_frames = k_block_frames;
  cfg.contribution = [rev = std::uint64_t{0}](ObjectId) { return rev; };
  cfg.nested_composition = [c_nest, c_inner](ObjectId content) -> std::optional<ObjectId> {
    return content == c_nest ? std::optional<ObjectId>(c_inner) : std::nullopt;
  };
  LookaheadRing ring(*doc, pull, cfg);

  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(k_rate);
  const std::int64_t span = static_cast<std::int64_t>(k_block_frames) * fpf;

  // Round 1: only the two GRANDCHILD leaves are dispatched (the nested contributor is
  // gated on its child closure, warming bottom-up); nothing mixes yet.
  std::vector<PrefetchWant> w1 = ring.prime<LocalBlock>(&cache, Time::zero(), Time{span}, +1);
  REQUIRE(w1.size() == 4); // 2 leaves x 2 blocks
  REQUIRE(ring.blocks_mixed() == 0);
  for (const PrefetchWant& w : w1) {
    fill_want(cache, cfg.resolve, w);
  }

  // Round 2: the grandchildren are resident, so the nested contributor is now
  // dispatched (one per block); still no mix (the nested block is not yet resident).
  std::vector<PrefetchWant> w2 = ring.prime<LocalBlock>(&cache, Time::zero(), Time{span}, +1);
  REQUIRE(w2.size() == 2); // the nested contributor at blocks 0..1
  REQUIRE(ring.blocks_mixed() == 0);
  for (const PrefetchWant& w : w2) {
    fill_want(cache, cfg.resolve, w);
  }

  // Round 3: the whole closure is resident -> both output blocks mix, no more wants,
  // and the gate never mixed silence for an absent descendant.
  std::vector<PrefetchWant> w3 = ring.prime<LocalBlock>(&cache, Time::zero(), Time{span}, +1);
  REQUIRE(w3.empty());
  REQUIRE(ring.blocks_mixed() == 2);
  REQUIRE(ring.silence_mixed() == 0);
}

// enforces: 12-audio#lookahead-warms-recursive-contributor-closure
TEST_CASE("the fill discovers a below-rate native re-request lazily from resident achieved_rate") {
  BelowRateLeaf src(24'000); // native 24 kHz < 48 kHz working rate
  Scene scene;
  scene.add(&src);
  const DocStatePtr doc = scene.model.current();

  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(k_rate);
  const std::int64_t span = static_cast<std::int64_t>(k_block_frames) * fpf;

  LocalCache cache{64u * 1024 * 1024};
  CachingPull pull(&cache, scene.id_of(), 0);
  LookaheadRing ring(*doc, pull, ring_config(scene));

  // Round 1: the working-rate discovery block is the only want (the native rate is a
  // render RESULT, unknowable yet); no mix.
  std::vector<PrefetchWant> w1 = ring.prime<LocalBlock>(&cache, Time::zero(), Time{span}, +1);
  REQUIRE(w1.size() == 2); // discovery block at blocks 0..1
  for (const PrefetchWant& w : w1) {
    REQUIRE(w.rate == k_rate);
    fill_want(cache, scene.resolver(), w);
  }
  REQUIRE(ring.blocks_mixed() == 0);

  // Round 2: the discovery block is resident and reports achieved_rate 24 kHz < 48 kHz,
  // so the native re-request (a DISTINCT BlockKey at the native rate) is now emitted.
  std::vector<PrefetchWant> w2 = ring.prime<LocalBlock>(&cache, Time::zero(), Time{span}, +1);
  REQUIRE(w2.size() == 2); // one native re-request per block
  for (const PrefetchWant& w : w2) {
    REQUIRE(w.rate == 24'000); // the native rate, not the working rate
    fill_want(cache, scene.resolver(), w);
  }
  REQUIRE(ring.blocks_mixed() == 0);

  // Round 3: discovery + native both resident -> the blocks mix at full residency.
  std::vector<PrefetchWant> w3 = ring.prime<LocalBlock>(&cache, Time::zero(), Time{span}, +1);
  REQUIRE(w3.empty());
  REQUIRE(ring.blocks_mixed() == 2);
  REQUIRE(ring.silence_mixed() == 0);
}

// enforces: 12-audio#lookahead-warms-recursive-contributor-closure
TEST_CASE("the recursive descent honors the Flat-mode culls: an inaudible child warms nothing") {
  SineLeaf nested_stub(0, 0.0F);
  SineLeaf a(300, 0.6F);
  SineLeaf b(700, 0.4F);

  Model model;
  std::unordered_map<ObjectId, Content*> binding;
  std::unordered_map<const Content*, ObjectId> ids;
  ObjectId c_inner{};
  ObjectId c_root{};
  ObjectId c_nest{};
  ObjectId lb{};
  {
    auto tx = model.transact("nested with a muted child");
    c_inner = tx.add_composition(0.0, 0.0);
    const ObjectId ca = tx.add_content(1);
    const ObjectId cb = tx.add_content(1);
    tx.attach_layer(c_inner, tx.add_layer(ca, Affine::identity()));
    lb = tx.add_layer(cb, Affine::identity());
    tx.attach_layer(c_inner, lb);
    c_root = tx.add_composition(0.0, 0.0);
    c_nest = tx.add_content(1);
    tx.attach_layer(c_root, tx.add_layer(c_nest, Affine::identity()));
    tx.commit();
    binding[ca] = &a;
    binding[cb] = &b;
    binding[c_nest] = &nested_stub;
    ids[&a] = ca;
    ids[&b] = cb;
    ids[&nested_stub] = c_nest;
  }
  {
    auto tx = model.transact("mute the second child");
    tx.set_audible(lb, false); // the inaudible cull (doc 12:86-87)
    tx.commit();
  }
  const DocStatePtr doc = model.current();

  LocalCache cache{64u * 1024 * 1024};
  CachingPull pull(
      &cache,
      [&ids](const Content* c) {
        const auto it = ids.find(c);
        return it != ids.end() ? it->second : ObjectId{};
      },
      0);

  LookaheadRingConfig cfg;
  cfg.composition = c_root;
  cfg.resolve = [&binding](ObjectId id) -> Content* {
    const auto it = binding.find(id);
    return it != binding.end() ? it->second : nullptr;
  };
  cfg.sample_rate = k_rate;
  cfg.layout = ChannelLayout::Stereo;
  cfg.block_frames = k_block_frames;
  cfg.contribution = [rev = std::uint64_t{0}](ObjectId) { return rev; };
  cfg.nested_composition = [c_nest, c_inner](ObjectId content) -> std::optional<ObjectId> {
    return content == c_nest ? std::optional<ObjectId>(c_inner) : std::nullopt;
  };
  LookaheadRing ring(*doc, pull, cfg);

  // Only the ONE audible grandchild is warmed (the muted child pulls nothing), so a
  // single leaf want appears for block 0 -- the descent honors the mixer's cull.
  std::vector<PrefetchWant> w1 = ring.prime<LocalBlock>(&cache, Time::zero(), Time{0}, +1);
  REQUIRE(w1.size() == 1);
}

namespace {

// An N-deep nesting CHAIN of DISTINCT compositions (audio.spatial_fill_cull): comp[k]
// holds a tone leaf (identity, edge 1.0) plus -- for k < N-1 -- a nesting layer at HALF
// scale (edge 0.5) whose injected child composition is comp[k+1]. Distinct content ids
// per level give distinct BlockKeys, so the warmed-contributor count reflects the
// descent DEPTH (a self-embedding Droste collapses every level to one key and so cannot
// expose depth via the want-list). In Spatial mode the accumulated attenuation halves
// each edge, so the sub-audible cull terminates the WARMING descent at a finite depth
// (13 for 2^-12); in Flat mode the transform is ignored and the descent warms the whole
// chain -- the exact Spatial-vs-Flat contrast the mix walk shows (audio_mix_goldens
// "sub-audible cull terminates a Droste recursion", ported to the ring).
struct SpatialChain {
  Model model;
  std::vector<std::unique_ptr<SineLeaf>> leaves;
  std::vector<std::unique_ptr<SineLeaf>> nests;
  std::unordered_map<ObjectId, Content*> binding;
  std::unordered_map<const Content*, ObjectId> ids;
  std::vector<ObjectId> comps;
  std::unordered_map<ObjectId, ObjectId> nest_edge; // nest content id -> child composition

  explicit SpatialChain(std::size_t depth) {
    const Affine half = Affine::scaling(0.5, 0.5);
    auto tx = model.transact("spatial chain");
    comps.resize(depth);
    for (std::size_t k = 0; k < depth; ++k) {
      comps[k] = tx.add_composition(0.0, 0.0);
    }
    for (std::size_t k = 0; k < depth; ++k) {
      leaves.push_back(std::make_unique<SineLeaf>(300, 0.5F));
      const ObjectId leaf_id = tx.add_content(1);
      tx.attach_layer(comps[k], tx.add_layer(leaf_id, Affine::identity()));
      binding[leaf_id] = leaves.back().get();
      ids[leaves.back().get()] = leaf_id;
      if (k + 1 < depth) {
        nests.push_back(std::make_unique<SineLeaf>(0, 0.0F)); // a nested-composition stub
        const ObjectId nest_id = tx.add_content(1);
        tx.attach_layer(comps[k], tx.add_layer(nest_id, half));
        binding[nest_id] = nests.back().get();
        ids[nests.back().get()] = nest_id;
        nest_edge[nest_id] = comps[k + 1];
      }
    }
    tx.commit();
  }

  MixResolver resolver() {
    return [this](ObjectId id) -> Content* {
      const auto it = binding.find(id);
      return it != binding.end() ? it->second : nullptr;
    };
  }
  std::function<ObjectId(const Content*)> id_of() {
    return [this](const Content* c) {
      const auto it = ids.find(c);
      return it != ids.end() ? it->second : ObjectId{};
    };
  }
  std::function<std::optional<ObjectId>(ObjectId)> nesting() {
    return [this](ObjectId content) -> std::optional<ObjectId> {
      const auto it = nest_edge.find(content);
      return it != nest_edge.end() ? std::optional<ObjectId>(it->second) : std::nullopt;
    };
  }
};

// Warm the whole transitive closure of output block 0 to residency, filling each want as
// the pump's worker path would, and return the total number of DISTINCT warmed
// contributors (the sum of the want-list sizes over the bottom-up rounds). A finite
// return proves the descent terminated.
std::size_t warm_closure_count(SpatialChain& chain, bool spatial) {
  const DocStatePtr doc = chain.model.current();
  LocalCache cache{64u * 1024 * 1024};
  CachingPull pull(&cache, chain.id_of(), 0);
  LookaheadRingConfig cfg;
  cfg.composition = chain.comps.front();
  cfg.resolve = chain.resolver();
  cfg.sample_rate = k_rate;
  cfg.layout = ChannelLayout::Stereo;
  cfg.block_frames = k_block_frames;
  cfg.contribution = [rev = std::uint64_t{0}](ObjectId) { return rev; };
  cfg.nested_composition = chain.nesting();
  if (spatial) {
    cfg.policy = MixPolicy::Spatial;
    // An identity listener (accum 1.0): the cull is driven purely by the per-edge
    // half-scale transforms, so its depth is a clean function of the threshold.
    cfg.spatial = Spatialization{Affine::identity(), 100.0, 100.0, 1.0F, k_sub_audible_atten};
  }
  LookaheadRing ring(*doc, pull, cfg);
  std::size_t total = 0;
  std::vector<PrefetchWant> wants;
  do {
    wants = ring.prime<LocalBlock>(&cache, Time::zero(), Time{0}, +1); // one output block
    total += wants.size();
    for (const PrefetchWant& w : wants) {
      fill_want(cache, cfg.resolve, w);
    }
  } while (!wants.empty());
  return total;
}

} // namespace

// enforces: 12-audio#spatial-fill-cull-terminates-warming
TEST_CASE("the Spatial warming descent culls a sub-audible Droste chain finite, below Flat") {
  // A 20-deep half-scale nesting chain: deeper than the 2^-12 cull depth (13) but well
  // inside max_depth (64), so Flat warms the WHOLE chain while Spatial terminates on the
  // sub-audible cull. The warmed set the mixer would pull is the same predicate, so the
  // ring's warmed closure equals the mix walk's culled tree (Constraint 1/3, D3).
  constexpr std::size_t k_depth = 20;

  SpatialChain flat_chain(k_depth);
  SpatialChain spatial_chain(k_depth);
  const std::size_t flat = warm_closure_count(flat_chain, /*spatial=*/false);
  const std::size_t spatial = warm_closure_count(spatial_chain, /*spatial=*/true);

  // Flat: no cull -> the whole 20-level chain warms (20 leaves + 19 nested contributors).
  REQUIRE(flat == 2 * k_depth - 1);
  // Spatial: half-scale edges cross 2^-12 at depth 13, so the descent bottoms out there
  // (13 leaves at depths 1..13 + 12 nested contributors at depths 1..12; the depth-13
  // edge, 2^-13, is culled). Finite, and strictly smaller than the Flat closure.
  REQUIRE(spatial == 13 + 12);
  REQUIRE(spatial < flat);
}

// enforces: 12-audio#spatial-fill-cull-terminates-warming
TEST_CASE("the Spatial cull keeps the primed-ring drain byte-exact: cache == inline, "
          "silence_mixed 0") {
  // The determinism invariant (Constraint 3, D5): after the warming descent culls the
  // Droste chain, the cache-warmed drain stays byte-identical to the inline drain and
  // never mixes silence. The mix walk pulls only the root's DIRECT contributors (leaf[0]
  // + the depth-1 nested stub, both above threshold and warmed either way), so the ring's
  // now-culled warmed set still covers the mixer's pulls exactly -- the cull removed only
  // deeper blocks the top mix never pulls. Mirrors the shape of the shipped Spatial ring
  // golden (threaded == inline, silence_mixed == 0) for a Droste scene.
  constexpr std::size_t k_depth = 20;

  const Spatialization seed{Affine::identity(), 100.0, 100.0, 1.0F, k_sub_audible_atten};

  // Inline fill (no cache): mix_block mixes block 0 Spatially, settling each pull inline.
  SpatialChain ichain(k_depth);
  const DocStatePtr idoc = ichain.model.current();
  CachingPull inline_pull(nullptr, ichain.id_of(), 0);
  LookaheadRingConfig icfg;
  icfg.composition = ichain.comps.front();
  icfg.resolve = ichain.resolver();
  icfg.sample_rate = k_rate;
  icfg.layout = ChannelLayout::Stereo;
  icfg.block_frames = k_block_frames;
  icfg.contribution = [rev = std::uint64_t{0}](ObjectId) { return rev; };
  icfg.nested_composition = ichain.nesting();
  icfg.policy = MixPolicy::Spatial;
  icfg.spatial = seed;
  LookaheadRing iring(*idoc, inline_pull, icfg);
  iring.prime<LocalBlock>(nullptr, Time::zero(), Time{0}, +1);
  REQUIRE(iring.blocks_mixed() == 1);
  REQUIRE(iring.silence_mixed() == 0);

  // Threaded fill (cache-warmed): prime in rounds, filling each want, until the culled
  // closure is resident and block 0 mixes at full transitive residency.
  SpatialChain cchain(k_depth);
  const DocStatePtr cdoc = cchain.model.current();
  LocalCache cache{64u * 1024 * 1024};
  CachingPull cache_pull(&cache, cchain.id_of(), 0);
  LookaheadRingConfig ccfg;
  ccfg.composition = cchain.comps.front();
  ccfg.resolve = cchain.resolver();
  ccfg.sample_rate = k_rate;
  ccfg.layout = ChannelLayout::Stereo;
  ccfg.block_frames = k_block_frames;
  ccfg.contribution = [rev = std::uint64_t{0}](ObjectId) { return rev; };
  ccfg.nested_composition = cchain.nesting();
  ccfg.policy = MixPolicy::Spatial;
  ccfg.spatial = seed;
  LookaheadRing cring(*cdoc, cache_pull, ccfg);
  std::vector<PrefetchWant> wants;
  do {
    wants = cring.prime<LocalBlock>(&cache, Time::zero(), Time{0}, +1);
    for (const PrefetchWant& w : wants) {
      fill_want(cache, ccfg.resolve, w);
    }
  } while (!wants.empty());
  REQUIRE(cring.blocks_mixed() == 1);
  REQUIRE(cring.silence_mixed() == 0); // never mixed silence for an absent contributor

  std::vector<float> got_inline(static_cast<std::size_t>(k_block_frames) * 2, 0.0F);
  std::vector<float> got_cache(static_cast<std::size_t>(k_block_frames) * 2, 0.0F);
  AudioBlock oi{got_inline.data(), k_block_frames, ChannelLayout::Stereo, k_rate};
  AudioBlock oc{got_cache.data(), k_block_frames, ChannelLayout::Stereo, k_rate};
  AudioResult mi{};
  AudioResult mc{};
  REQUIRE(iring.drain(0, oi, mi));
  REQUIRE(cring.drain(0, oc, mc));
  REQUIRE(bytes_equal(got_inline, got_cache));
}

// enforces: 12-audio#latency-prerolls-declared-content
TEST_CASE("a declared latency extends the fill lead, drain stays byte-identical to zero") {
  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(k_rate);
  const std::int64_t span = static_cast<std::int64_t>(k_block_frames) * fpf;
  const Time k_latency{2 * span}; // two output-block spans of declared latency

  // Two structurally identical scenes: one zero-latency tone, one declaring latency=k.
  // Their `render_audio` is byte-identical, so the drain must match block-for-block.
  SineLeaf zero_a(300, 0.6F);
  Scene zero;
  zero.add(&zero_a);
  const DocStatePtr zero_doc = zero.model.current();

  LatentSineLeaf latent_a(300, 0.6F, k_latency);
  Scene latent;
  latent.add(&latent_a);
  const DocStatePtr latent_doc = latent.model.current();

  CachingPull zero_pull(nullptr, zero.id_of(), 0);
  CachingPull latent_pull(nullptr, latent.id_of(), 0);
  LookaheadRing zero_ring(*zero_doc, zero_pull, ring_config(zero));
  LookaheadRing latent_ring(*latent_doc, latent_pull, ring_config(latent));

  // effective_preroll: zero for the no-latency scene, exactly k for the latent one.
  REQUIRE(zero_ring.effective_preroll(Time::zero()) == Time::zero());
  REQUIRE(latent_ring.effective_preroll(Time::zero()) == k_latency);

  // Prime both over the same base horizon (blocks 0..3). The latent ring lifts the
  // horizon by ceil(k/block_span) == 2, so it warms exactly two MORE blocks (0..5).
  zero_ring.prime<LocalBlock>(nullptr, Time::zero(), Time{3 * span}, +1);
  latent_ring.prime<LocalBlock>(nullptr, Time::zero(), Time{3 * span}, +1);
  REQUIRE(zero_ring.prepared_count() == 4);
  REQUIRE(latent_ring.prepared_count() == 6); // 4 + ceil(k / block_span)
  REQUIRE(latent_ring.is_prepared(5));
  REQUIRE_FALSE(zero_ring.is_prepared(5));

  // Byte-exact golden (no tolerance): every commonly-warmed block drains identically.
  for (std::int64_t i = 0; i < 6; ++i) {
    std::vector<float> got_latent(static_cast<std::size_t>(k_block_frames) * 2, 0.0F);
    AudioBlock out_latent{got_latent.data(), k_block_frames, ChannelLayout::Stereo, k_rate};
    AudioResult meta_latent{};
    REQUIRE(latent_ring.drain(i, out_latent, meta_latent));
    // The oracle is the zero-latency drain per output window (no hand-sum).
    CachingPull ref_pull(nullptr, latent.id_of(), 0);
    const std::vector<float> want =
        direct_mix(*latent_doc, latent.comp, latent.resolver(), ref_pull, i);
    REQUIRE(bytes_equal(got_latent, want));
    if (i < 4) {
      std::vector<float> got_zero(static_cast<std::size_t>(k_block_frames) * 2, 0.0F);
      AudioBlock out_zero{got_zero.data(), k_block_frames, ChannelLayout::Stereo, k_rate};
      AudioResult meta_zero{};
      REQUIRE(zero_ring.drain(i, out_zero, meta_zero));
      REQUIRE(bytes_equal(got_latent, got_zero)); // latent == zero-latency, byte-for-byte
    }
  }
}

// enforces: 12-audio#latency-prerolls-declared-content
TEST_CASE("effective pre-roll maxes declared latency across direct contributors, floored") {
  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(k_rate);
  const std::int64_t span = static_cast<std::int64_t>(k_block_frames) * fpf;

  // Two latent contributors: the effective pre-roll is the MAX of their declared
  // latencies (global max over direct contributors, doc 12:200-212 / Decision D2).
  LatentSineLeaf small(300, 0.6F, Time{span});
  LatentSineLeaf big(700, 0.4F, Time{3 * span});
  Scene scene;
  scene.add(&small);
  scene.add(&big);
  const DocStatePtr doc = scene.model.current();

  SECTION("max over contributors when the config floor is lower") {
    LookaheadRingConfig cfg = ring_config(scene);
    cfg.preroll = Time{span}; // below the max declared latency
    CachingPull pull(nullptr, scene.id_of(), 0);
    LookaheadRing ring(*doc, pull, cfg);
    REQUIRE(ring.effective_preroll(Time::zero()) == Time{3 * span}); // the bigger latency wins
    // The lifted window warms ceil(3*span / span) == 3 more blocks than the base.
    ring.prime<LocalBlock>(nullptr, Time::zero(), Time{2 * span}, +1); // base blocks 0..2
    REQUIRE(ring.prepared_count() == 6);                               // 3 + 3
  }

  SECTION("config.preroll floors the pre-roll when it exceeds every declared latency") {
    LookaheadRingConfig cfg = ring_config(scene);
    cfg.preroll = Time{5 * span}; // above the max declared latency
    CachingPull pull(nullptr, scene.id_of(), 0);
    LookaheadRing ring(*doc, pull, cfg);
    REQUIRE(ring.effective_preroll(Time::zero()) == Time{5 * span}); // the floor wins
  }
}
