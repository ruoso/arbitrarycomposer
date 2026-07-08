#include <arbc/audio_engine/lookahead.hpp>
#include <arbc/audio_engine/mix.hpp>
#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/compositor/pull_service.hpp> // BlockCache, AudioBlockValue
#include <arbc/contract/content.hpp>
#include <arbc/kind_nested/nested_content.hpp>
#include <arbc/kind_tone/tone_content.hpp>
#include <arbc/media/audio_block.hpp>
#include <arbc/model/model.hpp>
#include <arbc/runtime/audio_worker_pool.hpp>
#include <arbc/runtime/lookahead_pump.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

// doc 16 tier-6, tsan lane; refinement Acceptance "Concurrency / TSan" -- the audio
// twin of `pull_audio_concurrency.t.cpp` / the render `WorkerPool` TSan case. The
// audio worker pool + lookahead ring run under concurrent fill (multiple workers
// rendering distinct contributor blocks) and drain: it asserts the pool settles
// every fill exactly once (tasks_completed == tasks_submitted), the ring and
// `BlockCache` see no data race (the TSan lane flags any), and the drained samples
// equal the single-threaded inline goldens. Cross-component, so it lives in
// top-level `tests/` (not level-checked) and links the umbrella `arbc`.

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

// A tone leaf declaring a constant processing latency() (a Live pipeline). Its
// render_audio is byte-identical to SineLeaf's -- latency is a residency concern, not a
// signal transform (doc 12:200-212) -- so a latent scene drains byte-identical to the
// same tones with zero latency; the ring only warms MORE blocks ahead.
class LatentSine final : public Content {
public:
  LatentSine(std::uint32_t freq_hz, float amp, Time latency) : d_facet(freq_hz, amp, latency) {}
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

std::int64_t block_index_of(const AudioRequest& request) {
  if (request.sample_rate == 0) {
    return 0;
  }
  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(request.sample_rate);
  return fpf != 0 ? request.window.start.flicks / fpf : 0;
}

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
  BlockCache* d_blocks;
  std::function<ObjectId(const Content*)> d_id_of;
  std::uint64_t d_revision;
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
constexpr std::uint32_t k_block_frames = 16;
constexpr std::int64_t k_blocks = 8;

std::vector<float> direct_mix(const DocRoot& doc, ObjectId comp, const MixResolver& resolve,
                              std::int64_t index) {
  CachingPull inline_pull(nullptr, {}, 0);
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
TEST_CASE("TSan: concurrent worker fill + drain equals the inline goldens, settle-once holds") {
  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(k_rate);
  const std::int64_t span = static_cast<std::int64_t>(k_block_frames) * fpf;

  SineLeaf a(300, 0.6F);
  SineLeaf b(700, 0.4F);
  SineLeaf c(1100, 0.3F);
  Scene scene;
  scene.add(&a);
  scene.add(&b);
  scene.add(&c);
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
  poolcfg.worker_count = 8; // multiple workers render distinct contributor blocks
  AudioWorkerPool pool(poolcfg);

  std::atomic<std::uint64_t> fake_tick{0};
  LookaheadPumpConfig pumpcfg;
  pumpcfg.horizon = Time{(k_blocks - 1) * span}; // blocks 0..k_blocks-1
  pumpcfg.resolve = scene.resolver();
  pumpcfg.sample_rate = k_rate;
  pumpcfg.layout = ChannelLayout::Stereo;
  pumpcfg.block_frames = k_block_frames;
  pumpcfg.tick_period = std::chrono::hours(1);
  pumpcfg.tick_source = [&fake_tick] { return fake_tick.fetch_add(1, std::memory_order_relaxed); };
  pumpcfg.playhead_source = [] { return Time::zero(); };
  LookaheadPump pump(ring, blocks, pool, pumpcfg);

  // A consumer thread drains concurrently with the pump's fill/mix ticks. Drain is
  // serialized with the ring under the pump mutex and never mixes; it races the
  // worker fills and the pump insert, which the TSan lane validates.
  std::atomic<bool> stop{false};
  std::thread consumer([&] {
    while (!stop.load(std::memory_order_acquire)) {
      for (std::int64_t i = 0; i < k_blocks; ++i) {
        std::vector<float> got(static_cast<std::size_t>(k_block_frames) * 2, 0.0F);
        AudioBlock out{got.data(), k_block_frames, ChannelLayout::Stereo, k_rate};
        AudioResult meta{};
        (void)pump.drain(i, out, meta); // ready or silence; never a mix
      }
      std::this_thread::yield();
    }
  });

  // Drive several ticks so the whole horizon is filled + mixed on the workers.
  for (int t = 0; t < 4; ++t) {
    pump.flush();
  }

  stop.store(true, std::memory_order_release);
  consumer.join();
  pump.request_stop();

  // Every dispatched fill settled exactly once (counted once).
  REQUIRE(pool.tasks_completed() == pool.tasks_submitted());
  REQUIRE(pool.tasks_completed() > 0);

  // The concurrently-filled ring drains byte-identical to the single-threaded
  // inline goldens.
  for (std::int64_t i = 0; i < k_blocks; ++i) {
    std::vector<float> got(static_cast<std::size_t>(k_block_frames) * 2, 0.0F);
    AudioBlock out{got.data(), k_block_frames, ChannelLayout::Stereo, k_rate};
    AudioResult meta{};
    REQUIRE(pump.drain(i, out, meta));
    REQUIRE(bytes_equal(got, direct_mix(*doc, scene.comp, scene.resolver(), i)));
  }
}

// enforces: 12-audio#latency-prerolls-declared-content
TEST_CASE("TSan: a declared-latency scene drains the zero-latency inline goldens threaded") {
  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(k_rate);
  const std::int64_t span = static_cast<std::int64_t>(k_block_frames) * fpf;

  // A latent contributor (latency = 3 blocks) mixed with a zero-latency tone. The ring
  // extends its fill lead by the declared latency; the drained output blocks 0..k_blocks
  // are unchanged, so the threaded drain equals the inline goldens (which ignore latency,
  // rendering the same samples) -- and equals the zero-latency oracle.
  LatentSine a(300, 0.6F, Time{3 * span});
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
  // The ring lifts its fill lead by the declared latency: it warms blocks past the base
  // horizon (the residency the latent contributor needs).
  REQUIRE(ring.effective_preroll(Time::zero()) == Time{3 * span});

  AudioWorkerPoolConfig poolcfg;
  poolcfg.worker_count = 8;
  AudioWorkerPool pool(poolcfg);

  std::atomic<std::uint64_t> fake_tick{0};
  LookaheadPumpConfig pumpcfg;
  pumpcfg.horizon = Time{(k_blocks - 1) * span}; // base horizon; the ring lifts it
  pumpcfg.resolve = scene.resolver();
  pumpcfg.sample_rate = k_rate;
  pumpcfg.layout = ChannelLayout::Stereo;
  pumpcfg.block_frames = k_block_frames;
  pumpcfg.tick_period = std::chrono::hours(1);
  pumpcfg.tick_source = [&fake_tick] { return fake_tick.fetch_add(1, std::memory_order_relaxed); };
  pumpcfg.playhead_source = [] { return Time::zero(); };
  LookaheadPump pump(ring, blocks, pool, pumpcfg);

  std::atomic<bool> stop{false};
  std::thread consumer([&] {
    while (!stop.load(std::memory_order_acquire)) {
      for (std::int64_t i = 0; i < k_blocks; ++i) {
        std::vector<float> got(static_cast<std::size_t>(k_block_frames) * 2, 0.0F);
        AudioBlock out{got.data(), k_block_frames, ChannelLayout::Stereo, k_rate};
        AudioResult meta{};
        (void)pump.drain(i, out, meta);
      }
      std::this_thread::yield();
    }
  });

  for (int t = 0; t < 4; ++t) {
    pump.flush();
  }

  stop.store(true, std::memory_order_release);
  consumer.join();
  pump.request_stop();

  REQUIRE(pool.tasks_completed() == pool.tasks_submitted());
  REQUIRE(pool.tasks_completed() > 0);
  REQUIRE(ring.silence_mixed() == 0);

  // The latency-primed threaded ring drains byte-identical to the single-threaded inline
  // goldens (the zero-latency oracle: latency does not change any sample).
  for (std::int64_t i = 0; i < k_blocks; ++i) {
    std::vector<float> got(static_cast<std::size_t>(k_block_frames) * 2, 0.0F);
    AudioBlock out{got.data(), k_block_frames, ChannelLayout::Stereo, k_rate};
    AudioResult meta{};
    REQUIRE(pump.drain(i, out, meta));
    REQUIRE(bytes_equal(got, direct_mix(*doc, scene.comp, scene.resolver(), i)));
  }
}

namespace {

// A below-rate native source for the recursive TSan case: honors at/below its native
// rate, reports achieved==native / exact==false above it. Stateless -> race-free.
class BelowRateSource final : public Content {
public:
  explicit BelowRateSource(std::uint32_t native_rate) : d_native_rate(native_rate), d_facet(this) {}
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
    explicit Facet(BelowRateSource* owner) : d_owner(owner) {}
    std::optional<TimeRange> audio_extent() const override { return std::nullopt; }
    Stability audio_stability() const override { return Stability::Static; }
    std::optional<AudioResult> render_audio(const AudioRequest& request,
                                            std::shared_ptr<AudioCompletion>) override {
      const std::uint32_t ch = channel_count(request.layout);
      const std::int64_t fpf =
          Time::flicks_per_second / static_cast<std::int64_t>(request.sample_rate);
      for (std::uint32_t f = 0; f < request.target.frames; ++f) {
        const float v = parab_sine(request.window.start.flicks + static_cast<std::int64_t>(f) * fpf,
                                   3000, 0.6F);
        for (std::uint32_t c = 0; c < ch; ++c) {
          request.target.samples[static_cast<std::size_t>(f) * ch + c] = v;
        }
      }
      const std::uint32_t achieved = std::min(request.sample_rate, d_owner->d_native_rate);
      return AudioResult{achieved, request.sample_rate <= d_owner->d_native_rate};
    }

  private:
    BelowRateSource* d_owner;
  };

  std::uint32_t d_native_rate;
  Facet d_facet;
};

std::vector<float> tone_block(std::uint32_t freq, float amp, std::int64_t index) {
  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(k_rate);
  const std::int64_t span = static_cast<std::int64_t>(k_block_frames) * fpf;
  const std::int64_t t0 = index * span;
  std::vector<float> buf(static_cast<std::size_t>(k_block_frames) * 2, 0.0F);
  ToneContent tone(freq, amp);
  AudioBlock block{buf.data(), k_block_frames, ChannelLayout::Stereo, k_rate};
  const AudioRequest req{TimeRange{Time{t0}, Time{t0 + span}},
                         k_rate,
                         ChannelLayout::Stereo,
                         block,
                         Exactness::Exact,
                         StateHandle{}};
  auto done = std::make_shared<AudioCompletion>();
  (void)tone.audio()->render_audio(req, done);
  return buf;
}

std::vector<float> belowrate_block(const DocRoot& doc, ObjectId comp, const MixResolver& resolve,
                                   std::int64_t index) {
  CachingPull inline_pull(nullptr, {}, 0); // no cache -> discovery + native re-request inline
  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(k_rate);
  const std::int64_t span = static_cast<std::int64_t>(k_block_frames) * fpf;
  const std::int64_t t0 = index * span;
  std::vector<float> buf(static_cast<std::size_t>(k_block_frames) * 2, 0.0F);
  AudioBlock block{buf.data(), k_block_frames, ChannelLayout::Stereo, k_rate};
  const AudioRequest req{TimeRange{Time{t0}, Time{t0 + span}},
                         k_rate,
                         ChannelLayout::Stereo,
                         block,
                         Exactness::BestEffort,
                         StateHandle{}};
  mix_composition(doc, comp, resolve, inline_pull, req);
  return buf;
}

} // namespace

// enforces: 12-audio#lookahead-warms-recursive-contributor-closure
TEST_CASE("TSan: threaded recursive fill of nested + below-rate drains the inline goldens") {
  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(k_rate);
  const std::int64_t span = static_cast<std::int64_t>(k_block_frames) * fpf;

  ToneContent tone_a(440, 0.5F);
  ToneContent tone_b(660, 0.25F);
  BelowRateSource src(24'000);

  Model model;
  std::unordered_map<ObjectId, Content*> binding;
  std::unordered_map<const Content*, ObjectId> ids;
  ObjectId c_inner{};
  ObjectId c_root{};
  ObjectId c_nest{};
  ObjectId c_br_only{}; // a below-rate-only composition, for the inline oracle
  {
    auto tx = model.transact("nested + below-rate scene");
    c_inner = tx.add_composition(0.0, 0.0);
    const ObjectId ca = tx.add_content(1);
    const ObjectId cb = tx.add_content(1);
    tx.attach_layer(c_inner, tx.add_layer(ca, Affine::identity()));
    tx.attach_layer(c_inner, tx.add_layer(cb, Affine::identity()));
    c_nest = tx.add_content(1);
    const ObjectId csrc = tx.add_content(1);
    c_root = tx.add_composition(0.0, 0.0);
    tx.attach_layer(c_root, tx.add_layer(c_nest, Affine::identity()));
    tx.attach_layer(c_root, tx.add_layer(csrc, Affine::identity()));
    c_br_only = tx.add_composition(0.0, 0.0);
    tx.attach_layer(c_br_only, tx.add_layer(csrc, Affine::identity()));
    tx.commit();
    binding[ca] = &tone_a;
    binding[cb] = &tone_b;
    binding[csrc] = &src;
    ids[&tone_a] = ca;
    ids[&tone_b] = cb;
    ids[&src] = csrc;
  }
  const DocStatePtr doc = model.current();

  auto resolver = [&binding](ObjectId id) -> Content* {
    const auto it = binding.find(id);
    return it != binding.end() ? it->second : nullptr;
  };
  auto id_of = [&ids](const Content* c) {
    const auto it = ids.find(c);
    return it != ids.end() ? it->second : ObjectId{};
  };

  BlockCache blocks{64u * 1024 * 1024};
  CachingPull pull(&blocks, id_of, doc->revision());

  CpuBackend backend;
  NestedContent nested(c_inner);
  nested.attach(pull, backend, resolver, *doc);
  binding[c_nest] = &nested;
  ids[&nested] = c_nest;

  LookaheadRingConfig ringcfg;
  ringcfg.composition = c_root;
  ringcfg.resolve = resolver;
  ringcfg.sample_rate = k_rate;
  ringcfg.layout = ChannelLayout::Stereo;
  ringcfg.block_frames = k_block_frames;
  ringcfg.revision = doc->revision();
  ringcfg.nested_composition = [c_nest, c_inner](ObjectId content) -> std::optional<ObjectId> {
    return content == c_nest ? std::optional<ObjectId>(c_inner) : std::nullopt;
  };
  LookaheadRing ring(*doc, pull, ringcfg);

  AudioWorkerPoolConfig poolcfg;
  poolcfg.worker_count = 8;
  // Serialize the cache-reading nested contributor renders so at most one worker
  // touches the single-writer BlockCache at a time (the per-content serialization gate
  // under recursive re-entry); the leaf tone / below-rate renders never touch it.
  poolcfg.serialize_predicate = [&nested](const Content* c) { return c == &nested; };
  AudioWorkerPool pool(poolcfg);

  std::atomic<std::uint64_t> fake_tick{0};
  LookaheadPumpConfig pumpcfg;
  pumpcfg.horizon = Time{(k_blocks - 1) * span};
  pumpcfg.resolve = resolver;
  pumpcfg.sample_rate = k_rate;
  pumpcfg.layout = ChannelLayout::Stereo;
  pumpcfg.block_frames = k_block_frames;
  pumpcfg.tick_period = std::chrono::hours(1);
  pumpcfg.tick_source = [&fake_tick] { return fake_tick.fetch_add(1, std::memory_order_relaxed); };
  pumpcfg.playhead_source = [] { return Time::zero(); };
  LookaheadPump pump(ring, blocks, pool, pumpcfg);

  // A consumer drains concurrently with the recursive fill/mix ticks.
  std::atomic<bool> stop{false};
  std::thread consumer([&] {
    while (!stop.load(std::memory_order_acquire)) {
      for (std::int64_t i = 0; i < k_blocks; ++i) {
        std::vector<float> got(static_cast<std::size_t>(k_block_frames) * 2, 0.0F);
        AudioBlock out{got.data(), k_block_frames, ChannelLayout::Stereo, k_rate};
        AudioResult meta{};
        (void)pump.drain(i, out, meta);
      }
      std::this_thread::yield();
    }
  });

  for (int t = 0; t < 5; ++t) {
    pump.flush();
  }

  stop.store(true, std::memory_order_release);
  consumer.join();
  pump.request_stop();

  // Every dispatched fill (each tone leaf, the nested contributor, each below-rate
  // discovery + native re-request) settled exactly once.
  REQUIRE(pool.tasks_completed() == pool.tasks_submitted());
  REQUIRE(pool.tasks_completed() > 0);
  REQUIRE(ring.silence_mixed() == 0);

  // The concurrently-filled recursive ring drains byte-identical to the inline
  // oracle: (tone_a + tone_b) [nested] + the below-rate reconstruction.
  for (std::int64_t i = 0; i < k_blocks; ++i) {
    std::vector<float> got(static_cast<std::size_t>(k_block_frames) * 2, 0.0F);
    AudioBlock out{got.data(), k_block_frames, ChannelLayout::Stereo, k_rate};
    AudioResult meta{};
    REQUIRE(pump.drain(i, out, meta));
    std::vector<float> want = tone_block(440, 0.5F, i);
    const std::vector<float> tb = tone_block(660, 0.25F, i);
    const std::vector<float> br = belowrate_block(*doc, c_br_only, resolver, i);
    for (std::size_t k = 0; k < want.size(); ++k) {
      want[k] += tb[k];
      want[k] += br[k];
    }
    REQUIRE(bytes_equal(got, want));
  }
}

namespace {

constexpr double k_view = 100.0;

// The direct Spatial mix oracle for one output block (audio.spatial_nested_warm_context):
// mix the root composition inline with the monitor's `Spatialization` context (so the
// nested contributor renders Spatial), then apply the camera post-scale by `accum_atten`
// exactly as `LookaheadRing::mix_block` does.
std::vector<float> spatial_direct_mix(const DocRoot& doc, ObjectId root, const MixResolver& resolve,
                                      std::int64_t index, const Spatialization& seed) {
  CachingPull inline_pull(nullptr, {}, 0); // no cache -> nested renders Spatial inline
  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(k_rate);
  const std::int64_t span = static_cast<std::int64_t>(k_block_frames) * fpf;
  const std::int64_t t0 = index * span;
  std::vector<float> buf(static_cast<std::size_t>(k_block_frames) * 2, 0.0F);
  AudioBlock block{buf.data(), k_block_frames, ChannelLayout::Stereo, k_rate};
  const AudioRequest req{TimeRange{Time{t0}, Time{t0 + span}},
                         k_rate,
                         ChannelLayout::Stereo,
                         block,
                         Exactness::BestEffort,
                         StateHandle{},
                         seed};
  mix_composition(doc, root, resolve, inline_pull, req, MixPolicy::Spatial);
  for (float& s : buf) {
    s *= seed.accum_atten;
  }
  return buf;
}

} // namespace

// enforces: 12-audio#spatial-warms-nested-with-pull-context
TEST_CASE("TSan: threaded Spatial nested fill drains the inline Spatial oracle, settle-once") {
  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(k_rate);
  const std::int64_t span = static_cast<std::int64_t>(k_block_frames) * fpf;

  // A Spatial monitor over a nested composition holding one off-center child tone -- the
  // internal Spatial mix differs from Flat, so warming the nested contributor Flat would
  // diverge from the inline Spatial oracle (the pre-fix bug this test guards).
  const Spatialization seed{Affine::identity(), k_view, k_view, 1.0F, k_sub_audible_atten};
  ToneContent child(770, 0.5F);

  Model model;
  std::unordered_map<ObjectId, Content*> binding;
  std::unordered_map<const Content*, ObjectId> ids;
  ObjectId c_inner{};
  ObjectId c_root{};
  ObjectId c_nest{};
  {
    auto tx = model.transact("spatial nested scene");
    c_inner = tx.add_composition(0.0, 0.0);
    const ObjectId c_child = tx.add_content(1);
    tx.attach_layer(c_inner, tx.add_layer(c_child, Affine::translation(k_view / 4.0, 0.0)));
    c_nest = tx.add_content(1);
    c_root = tx.add_composition(0.0, 0.0);
    tx.attach_layer(c_root, tx.add_layer(c_nest, Affine::translation(k_view / 2.0, 0.0)));
    tx.commit();
    binding[c_child] = &child;
    ids[&child] = c_child;
  }
  const DocStatePtr doc = model.current();

  auto resolver = [&binding](ObjectId id) -> Content* {
    const auto it = binding.find(id);
    return it != binding.end() ? it->second : nullptr;
  };
  auto id_of = [&ids](const Content* c) {
    const auto it = ids.find(c);
    return it != ids.end() ? it->second : ObjectId{};
  };

  BlockCache blocks{64u * 1024 * 1024};
  CachingPull pull(&blocks, id_of, doc->revision());

  CpuBackend backend;
  NestedContent nested(c_inner);
  nested.attach(pull, backend, resolver, *doc);
  binding[c_nest] = &nested;
  ids[&nested] = c_nest;

  LookaheadRingConfig ringcfg;
  ringcfg.composition = c_root;
  ringcfg.resolve = resolver;
  ringcfg.sample_rate = k_rate;
  ringcfg.layout = ChannelLayout::Stereo;
  ringcfg.block_frames = k_block_frames;
  ringcfg.revision = doc->revision();
  ringcfg.policy = MixPolicy::Spatial;
  ringcfg.spatial = seed;
  ringcfg.nested_composition = [c_nest, c_inner](ObjectId content) -> std::optional<ObjectId> {
    return content == c_nest ? std::optional<ObjectId>(c_inner) : std::nullopt;
  };
  LookaheadRing ring(*doc, pull, ringcfg);

  AudioWorkerPoolConfig poolcfg;
  poolcfg.worker_count = 8;
  poolcfg.serialize_predicate = [&nested](const Content* c) { return c == &nested; };
  AudioWorkerPool pool(poolcfg);

  std::atomic<std::uint64_t> fake_tick{0};
  LookaheadPumpConfig pumpcfg;
  pumpcfg.horizon = Time{(k_blocks - 1) * span};
  pumpcfg.resolve = resolver;
  pumpcfg.sample_rate = k_rate;
  pumpcfg.layout = ChannelLayout::Stereo;
  pumpcfg.block_frames = k_block_frames;
  pumpcfg.tick_period = std::chrono::hours(1);
  pumpcfg.tick_source = [&fake_tick] { return fake_tick.fetch_add(1, std::memory_order_relaxed); };
  pumpcfg.playhead_source = [] { return Time::zero(); };
  LookaheadPump pump(ring, blocks, pool, pumpcfg);

  std::atomic<bool> stop{false};
  std::thread consumer([&] {
    while (!stop.load(std::memory_order_acquire)) {
      for (std::int64_t i = 0; i < k_blocks; ++i) {
        std::vector<float> got(static_cast<std::size_t>(k_block_frames) * 2, 0.0F);
        AudioBlock out{got.data(), k_block_frames, ChannelLayout::Stereo, k_rate};
        AudioResult meta{};
        (void)pump.drain(i, out, meta);
      }
      std::this_thread::yield();
    }
  });

  for (int t = 0; t < 5; ++t) {
    pump.flush();
  }

  stop.store(true, std::memory_order_release);
  consumer.join();
  pump.request_stop();

  REQUIRE(pool.tasks_completed() == pool.tasks_submitted());
  REQUIRE(pool.tasks_completed() > 0);
  REQUIRE(ring.silence_mixed() == 0);

  // The concurrently-filled Spatial ring drains byte-identical to the inline Spatial
  // oracle: each nested contributor block was warmed under the per-edge context the
  // mixer pulls it with, so the threaded fill never substituted a Flat-warmed block.
  for (std::int64_t i = 0; i < k_blocks; ++i) {
    std::vector<float> got(static_cast<std::size_t>(k_block_frames) * 2, 0.0F);
    AudioBlock out{got.data(), k_block_frames, ChannelLayout::Stereo, k_rate};
    AudioResult meta{};
    REQUIRE(pump.drain(i, out, meta));
    REQUIRE(bytes_equal(got, spatial_direct_mix(*doc, c_root, resolver, i, seed)));
  }
}
