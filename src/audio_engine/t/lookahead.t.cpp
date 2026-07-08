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
  cfg.revision = 0;
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
  REQUIRE(wants.size() == 4);        // 2 tones x 2 blocks
  REQUIRE(ring.blocks_mixed() == 0); // nothing mixed against a cold cache

  // Fill the wants (the pump's worker path), then re-prime: now resident -> mixed.
  for (const PrefetchWant& w : wants) {
    fill_want(cache, scene.resolver(), w);
  }
  const int dispatches_before = pull.dispatches();
  const std::vector<PrefetchWant> wants2 = ring.prime<LocalBlock>(&cache, Time::zero(), Time{span}, +1);
  REQUIRE(wants2.empty());          // fully warm
  REQUIRE(ring.blocks_mixed() == 2); // both blocks mixed
  // The mix pass hit the warmed cache for every contributor: ZERO new dispatch.
  REQUIRE(pull.dispatches() == dispatches_before);

  // A subsequent direct pull_audio for a filled key hits with zero dispatch.
  const int dispatches_probe = pull.dispatches();
  std::vector<float> probe(static_cast<std::size_t>(k_block_frames) * 2, 0.0F);
  AudioBlock pblock{probe.data(), k_block_frames, ChannelLayout::Stereo, k_rate};
  const AudioRequest preq{TimeRange{Time::zero(), Time{span}}, k_rate, ChannelLayout::Stereo, pblock,
                          Exactness::BestEffort, StateHandle{}};
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
  REQUIRE(ring.blocks_mixed() == 5); // 4 + exactly one new block
  REQUIRE_FALSE(ring.is_prepared(0)); // flushed
  REQUIRE(ring.is_prepared(4));       // freshly mixed
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
