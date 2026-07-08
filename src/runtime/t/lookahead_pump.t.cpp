#include <arbc/runtime/lookahead_pump.hpp>

#include <arbc/audio_engine/lookahead.hpp>
#include <arbc/audio_engine/mix.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/compositor/pull_service.hpp> // BlockCache, AudioBlockValue
#include <arbc/contract/content.hpp>
#include <arbc/media/audio_block.hpp>
#include <arbc/model/model.hpp>
#include <arbc/runtime/audio_worker_pool.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

// Integration tests for the headless lookahead pump (doc 12:31-34,155-190): a fake
// injected clock and no audio device, driving the L4 ring + the L5 audio worker
// pool over the REAL compositor `BlockCache`. It witnesses "audio renders ahead" --
// the pump fills + mixes ahead of the playhead on a worker, a consumer `drain`
// consumes prepared mixed blocks and runs NO plugin code, and threaded and inline
// fills produce byte-identical samples. Local Content / PullService doubles keep the
// includes inside runtime's dependency closure (no `kind_tone` / `backend_cpu`).

namespace {

using namespace arbc;

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

std::int64_t block_index_of(const AudioRequest& request) {
  if (request.sample_rate == 0) {
    return 0;
  }
  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(request.sample_rate);
  return fpf != 0 ? request.window.start.flicks / fpf : 0;
}

// A local cache-first `PullService` over the REAL compositor `BlockCache`, counting
// dispatches (misses). It is the pump's mix-pass pull; the pump's own worker fill
// populates the same cache.
class CachingPull final : public PullService {
public:
  CachingPull(BlockCache* blocks, std::function<ObjectId(const Content*)> id_of,
              std::uint64_t revision)
      : d_blocks(blocks), d_id_of(std::move(id_of)), d_revision(revision) {}
  void pull(ContentRef, const RenderRequest&, std::shared_ptr<RenderCompletion> done) override {
    done->fail(RenderError::ContentFailed);
  }
  void pull_audio(ContentRef input, const AudioRequest& request,
                  std::shared_ptr<AudioCompletion> done) override {
    if (d_blocks != nullptr) {
      const BlockKey key{d_id_of ? d_id_of(input) : ObjectId{}, d_revision, block_index_of(request),
                         request.sample_rate};
      if (std::optional<CacheHold<AudioBlockValue>> hit = d_blocks->lookup(key);
          hit.has_value() && hit->get().meta.exact &&
          hit->get().meta.achieved_rate == request.sample_rate &&
          hit->get().frames == request.target.frames &&
          hit->get().layout == request.target.layout) {
        const AudioBlockValue& value = hit->get();
        const std::size_t n = static_cast<std::size_t>(value.frames) * channel_count(value.layout);
        if (request.target.samples != nullptr && value.samples.size() >= n) {
          for (std::size_t i = 0; i < n; ++i) {
            request.target.samples[i] = value.samples[i];
          }
        }
        done->complete(value.meta);
        return;
      }
    }
    d_dispatches.fetch_add(1, std::memory_order_acq_rel);
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
  std::uint64_t dispatches() const { return d_dispatches.load(std::memory_order_acquire); }

private:
  BlockCache* d_blocks;
  std::function<ObjectId(const Content*)> d_id_of;
  std::uint64_t d_revision;
  std::atomic<std::uint64_t> d_dispatches{0};
};

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

constexpr std::uint32_t k_rate = 48'000;
constexpr std::uint32_t k_block_frames = 32;

std::vector<float> direct_mix(const DocRoot& doc, ObjectId comp, const MixResolver& resolve,
                              std::int64_t index) {
  CachingPull inline_pull(nullptr, {}, 0); // no cache: render inline, deterministic
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
  mix_composition(doc, comp, resolve, inline_pull, req);
  return buf;
}

bool bytes_equal(const std::vector<float>& a, const std::vector<float>& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

} // namespace

// enforces: 12-audio#lookahead-prepares-ahead-of-playhead
// enforces: 12-audio#lookahead-fills-block-cache-through-prefetch-ring
TEST_CASE("the pump renders ahead: drain consumes prepared blocks, worker-count-independent") {
  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(k_rate);
  const std::int64_t span = static_cast<std::int64_t>(k_block_frames) * fpf;

  for (const std::size_t worker_count : {std::size_t{0}, std::size_t{4}}) {
    SineLeaf a(300, 0.6F);
    SineLeaf b(700, 0.4F);
    Scene scene;
    scene.add(&a);
    scene.add(&b);
    const DocStatePtr doc = scene.model.current();

    BlockCache blocks{64u * 1024 * 1024};
    CachingPull pull(&blocks, scene.id_of(), doc->revision());

    LookaheadRingConfig ringcfg;
    ringcfg.composition = scene.comp;
    ringcfg.resolve = scene.resolver();
    ringcfg.sample_rate = k_rate;
    ringcfg.layout = ChannelLayout::Stereo;
    ringcfg.block_frames = k_block_frames;
    ringcfg.revision = doc->revision();
    LookaheadRing ring(*doc, pull, ringcfg);

    AudioWorkerPoolConfig poolcfg;
    poolcfg.worker_count = worker_count;
    AudioWorkerPool pool(poolcfg);

    std::atomic<std::uint64_t> fake_tick{0};
    LookaheadPumpConfig pumpcfg;
    pumpcfg.horizon = Time{3 * span}; // blocks 0..3
    pumpcfg.resolve = scene.resolver();
    pumpcfg.sample_rate = k_rate;
    pumpcfg.layout = ChannelLayout::Stereo;
    pumpcfg.block_frames = k_block_frames;
    pumpcfg.tick_period = std::chrono::hours(1); // only ticks on poke/flush
    pumpcfg.tick_source = [&fake_tick] {
      return fake_tick.fetch_add(1, std::memory_order_relaxed);
    };
    pumpcfg.playhead_source = [] { return Time::zero(); };
    LookaheadPump pump(ring, blocks, pool, pumpcfg);

    // One tick fully primes the horizon (fill + mix). Flush twice for good measure.
    pump.flush();
    pump.flush();

    const std::uint64_t dispatches_after_prime = pull.dispatches();

    // Drain each prepared block: byte-identical to a direct per-window mix, and the
    // drain thread issues NO new dispatch (no plugin code runs on the consumer).
    for (std::int64_t i = 0; i < 4; ++i) {
      std::vector<float> got(static_cast<std::size_t>(k_block_frames) * 2, 0.0F);
      AudioBlock out{got.data(), k_block_frames, ChannelLayout::Stereo, k_rate};
      AudioResult meta{};
      REQUIRE(pump.drain(i, out, meta));
      const std::vector<float> want = direct_mix(*doc, scene.comp, scene.resolver(), i);
      REQUIRE(bytes_equal(got, want));
    }
    // The mix pass hit the warmed cache; the drain pass ran no plugin code.
    REQUIRE(pull.dispatches() == dispatches_after_prime);

    // enforces the cache-fill claim: a post-prime pull_audio for a primed key hits
    // with zero dispatch (the mixer never dispatched during the warm mix).
    const std::uint64_t before_probe = pull.dispatches();
    std::vector<float> probe(static_cast<std::size_t>(k_block_frames) * 2, 0.0F);
    AudioBlock pblock{probe.data(), k_block_frames, ChannelLayout::Stereo, k_rate};
    const AudioRequest preq{TimeRange{Time::zero(), Time{span}}, k_rate, ChannelLayout::Stereo,
                            pblock, Exactness::BestEffort, StateHandle{}};
    auto pdone = std::make_shared<AudioCompletion>();
    pull.pull_audio(&a, preq, pdone);
    REQUIRE(pdone->settled());
    REQUIRE(pull.dispatches() == before_probe); // 0 new dispatch: a resident hit

    pump.request_stop();
  }
}
