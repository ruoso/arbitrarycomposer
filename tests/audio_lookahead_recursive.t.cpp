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
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Pump-driven goldens + behavioral counters for the RECURSIVE lookahead fill
// (audio.lookahead_recursive_prefetch, doc 12:210-222). audio.lookahead proved the
// FLAT case (`tests/audio_lookahead_concurrency.t.cpp`, flat SineLeaf contributors);
// this file drives a NESTED composition and a BELOW-RATE native contributor through
// the real `LookaheadPump` + `AudioWorkerPool` at `worker_count > 0` and asserts the
// drained mixed blocks are byte-identical to (a) the inline recursion oracle and (b)
// the `worker_count == 0` drain -- the fill warmed the whole transitive contributor
// closure, so the threaded mix never mixed silence for a not-yet-rendered descendant.
// Cross-component (kind_nested + kind_tone + compositor + runtime), so it lives here
// and links the umbrella `arbc`.

namespace {

using namespace arbc;

// Byte-exact parabolic sine over an EXACT integer flick phase (never std::sin) -- the
// same discipline as `org.arbc.tone` / the flat lookahead doubles.
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

// A below-rate audio source (a tone always honors, so it cannot exercise the native
// re-request): asked at or below its native rate it honors; asked above it it reports
// `achieved_rate == native`, `exact == false` -- the below-rate contributor the
// resampling boundary provokes (reused fixture semantics from
// nested_audio_resampling_goldens.t.cpp). Stateless -> race-free under concurrent
// descent into thread-confined targets.
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
        const std::int64_t t = request.window.start.flicks + static_cast<std::int64_t>(f) * fpf;
        const float v = parab_sine(t, d_owner->d_freq_hz, d_owner->d_amp);
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
  std::uint32_t d_freq_hz;
  float d_amp;
  Facet d_facet;
};

std::int64_t block_index_of(const AudioRequest& request) {
  if (request.sample_rate == 0) {
    return 0;
  }
  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(request.sample_rate);
  return fpf != 0 ? request.window.start.flicks / fpf : 0;
}

// The mix-pass / inner-pull `PullService`: cache-first over the REAL compositor
// `BlockCache`, rendering inline on a miss (so the below-rate discovery block -- which
// the exact-fresh gate does not serve -- still settles and the mix stays byte-exact),
// counting dispatches (misses). One instance serves both the ring's top-level mix and
// each nested contributor's inner recursive pulls, so a warmed closure is all hits.
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
      // Mirror the real PullServiceImpl::pull_audio read key (pull_service.cpp:301): fold the
      // request's Spatial-context digest so this read key equals the write-side warm key
      // `contribution_key` builds under the same per-edge context (Flat digests to 0, a no-op).
      const BlockKey key{d_id_of ? d_id_of(input) : ObjectId{}, d_revision, block_index_of(request),
                         request.sample_rate, spatial_context_digest(request.spatial)};
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
        return; // resident exact-fresh hit: zero dispatch
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

bool bytes_equal(const std::vector<float>& a, const std::vector<float>& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

NestedResolver map_resolver(const std::unordered_map<ObjectId, Content*>& binding) {
  return [&binding](ObjectId id) -> Content* {
    const auto it = binding.find(id);
    return it != binding.end() ? it->second : nullptr;
  };
}

constexpr std::uint32_t k_rate = 48'000;
constexpr std::uint32_t k_block_frames = 32;

std::vector<float> render_tone(std::uint32_t freq, float amp, std::int64_t index) {
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

// The inline recursion oracle for one output block: mix the root composition with a
// no-cache inline pull (renders every contributor -- including a below-rate native
// re-request -- synchronously), byte-identical to the `worker_count == 0` fill.
std::vector<float> inline_mix(const DocRoot& doc, ObjectId root, const MixResolver& resolve,
                              PullService& pull, std::int64_t index) {
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
  mix_composition(doc, root, resolve, pull, req);
  return buf;
}

} // namespace

// enforces: 12-audio#lookahead-warms-recursive-contributor-closure
// enforces: 12-audio#lookahead-prepares-ahead-of-playhead
TEST_CASE("the threaded pump drains a nested-of-tones scene byte-identical to the inline oracle") {
  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(k_rate);
  const std::int64_t span = static_cast<std::int64_t>(k_block_frames) * fpf;

  for (const std::size_t worker_count : {std::size_t{0}, std::size_t{4}}) {
    ToneContent tone_a(440, 0.5F);
    ToneContent tone_b(660, 0.25F);

    Model model;
    std::unordered_map<ObjectId, Content*> binding;
    std::unordered_map<const Content*, ObjectId> ids;
    ObjectId c_inner{};
    ObjectId c_root{};
    ObjectId c_nest{};
    {
      auto tx = model.transact("nested of tones");
      c_inner = tx.add_composition(0.0, 0.0);
      const ObjectId ca = tx.add_content(1);
      const ObjectId cb = tx.add_content(1);
      tx.attach_layer(c_inner, tx.add_layer(ca, Affine::identity()));
      tx.attach_layer(c_inner, tx.add_layer(cb, Affine::identity()));
      c_root = tx.add_composition(0.0, 0.0);
      c_nest = tx.add_content(1);
      tx.attach_layer(c_root, tx.add_layer(c_nest, Affine::identity()));
      tx.commit();
      binding[ca] = &tone_a;
      binding[cb] = &tone_b;
      ids[&tone_a] = ca;
      ids[&tone_b] = cb;
    }
    const DocStatePtr doc = model.current();

    BlockCache blocks{64u * 1024 * 1024};
    CachingPull pull(
        &blocks,
        [&ids](const Content* c) {
          const auto it = ids.find(c);
          return it != ids.end() ? it->second : ObjectId{};
        },
        doc->revision());

    CpuBackend backend;
    NestedContent nested(c_inner);
    nested.attach(pull, backend, map_resolver(binding), *doc);
    binding[c_nest] = &nested;
    ids[&nested] = c_nest;

    LookaheadRingConfig ringcfg;
    ringcfg.composition = c_root;
    ringcfg.resolve = map_resolver(binding);
    ringcfg.sample_rate = k_rate;
    ringcfg.layout = ChannelLayout::Stereo;
    ringcfg.block_frames = k_block_frames;
    ringcfg.contribution = [rev = doc->revision()](ObjectId) { return rev; };
    ringcfg.nested_composition = [c_nest, c_inner](ObjectId content) -> std::optional<ObjectId> {
      return content == c_nest ? std::optional<ObjectId>(c_inner) : std::nullopt;
    };
    LookaheadRing ring(*doc, pull, ringcfg);

    AudioWorkerPoolConfig poolcfg;
    poolcfg.worker_count = worker_count;
    // Serialize the (cache-reading) nested contributor renders so at most one worker
    // touches the single-writer BlockCache at a time (the per-content serialization
    // gate, Constraint 3): leaf tone renders never touch the cache and stay parallel.
    poolcfg.serialize_predicate = [&nested](const Content* c) { return c == &nested; };
    AudioWorkerPool pool(poolcfg);

    std::atomic<std::uint64_t> fake_tick{0};
    LookaheadPumpConfig pumpcfg;
    pumpcfg.horizon = Time{2 * span}; // blocks 0..2
    pumpcfg.resolve = map_resolver(binding);
    pumpcfg.sample_rate = k_rate;
    pumpcfg.layout = ChannelLayout::Stereo;
    pumpcfg.block_frames = k_block_frames;
    pumpcfg.tick_period = std::chrono::hours(1);
    pumpcfg.tick_source = [&fake_tick] {
      return fake_tick.fetch_add(1, std::memory_order_relaxed);
    };
    pumpcfg.playhead_source = [] { return Time::zero(); };
    LookaheadPump pump(ring, blocks, pool, pumpcfg);

    pump.flush();
    pump.flush();

    const std::uint64_t dispatches_after_prime = pull.dispatches();

    for (std::int64_t i = 0; i < 3; ++i) {
      std::vector<float> got(static_cast<std::size_t>(k_block_frames) * 2, 0.0F);
      AudioBlock out{got.data(), k_block_frames, ChannelLayout::Stereo, k_rate};
      AudioResult meta{};
      REQUIRE(pump.drain(i, out, meta));
      // Oracle: the rendering-is-recursion identity -- the nested mix equals summing
      // the two tones directly at top level (homogeneous, unit gain, identity map).
      std::vector<float> want = render_tone(440, 0.5F, i);
      const std::vector<float> b = render_tone(660, 0.25F, i);
      for (std::size_t k = 0; k < want.size(); ++k) {
        want[k] += b[k];
      }
      REQUIRE(bytes_equal(got, want));
    }

    // The gate never mixed silence for an unwarmed descendant, and the drain consumed
    // prepared blocks without running any plugin code (0 new dispatch on the consumer).
    REQUIRE(ring.silence_mixed() == 0);
    REQUIRE(pull.dispatches() == dispatches_after_prime);

    pump.request_stop();
  }
}

// enforces: 12-audio#lookahead-warms-recursive-contributor-closure
TEST_CASE("nested-of-tones counters: closure warmed once, inner pulls all hit, drain is pure") {
  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(k_rate);
  const std::int64_t span = static_cast<std::int64_t>(k_block_frames) * fpf;

  ToneContent tone_a(440, 0.5F);
  ToneContent tone_b(660, 0.25F);

  Model model;
  std::unordered_map<ObjectId, Content*> binding;
  std::unordered_map<const Content*, ObjectId> ids;
  ObjectId c_inner{};
  ObjectId c_root{};
  ObjectId c_nest{};
  {
    auto tx = model.transact("nested of tones");
    c_inner = tx.add_composition(0.0, 0.0);
    const ObjectId ca = tx.add_content(1);
    const ObjectId cb = tx.add_content(1);
    tx.attach_layer(c_inner, tx.add_layer(ca, Affine::identity()));
    tx.attach_layer(c_inner, tx.add_layer(cb, Affine::identity()));
    c_root = tx.add_composition(0.0, 0.0);
    c_nest = tx.add_content(1);
    tx.attach_layer(c_root, tx.add_layer(c_nest, Affine::identity()));
    tx.commit();
    binding[ca] = &tone_a;
    binding[cb] = &tone_b;
    ids[&tone_a] = ca;
    ids[&tone_b] = cb;
  }
  const DocStatePtr doc = model.current();

  BlockCache blocks{64u * 1024 * 1024};
  CachingPull pull(
      &blocks,
      [&ids](const Content* c) {
        const auto it = ids.find(c);
        return it != ids.end() ? it->second : ObjectId{};
      },
      doc->revision());

  CpuBackend backend;
  NestedContent nested(c_inner);
  nested.attach(pull, backend, map_resolver(binding), *doc);
  binding[c_nest] = &nested;
  ids[&nested] = c_nest;

  LookaheadRingConfig ringcfg;
  ringcfg.composition = c_root;
  ringcfg.resolve = map_resolver(binding);
  ringcfg.sample_rate = k_rate;
  ringcfg.layout = ChannelLayout::Stereo;
  ringcfg.block_frames = k_block_frames;
  ringcfg.contribution = [rev = doc->revision()](ObjectId) { return rev; };
  ringcfg.nested_composition = [c_nest, c_inner](ObjectId content) -> std::optional<ObjectId> {
    return content == c_nest ? std::optional<ObjectId>(c_inner) : std::nullopt;
  };
  LookaheadRing ring(*doc, pull, ringcfg);

  AudioWorkerPoolConfig poolcfg;
  poolcfg.worker_count = 4;
  poolcfg.serialize_predicate = [&nested](const Content* c) { return c == &nested; };
  AudioWorkerPool pool(poolcfg);

  std::atomic<std::uint64_t> fake_tick{0};
  LookaheadPumpConfig pumpcfg;
  pumpcfg.horizon = Time{2 * span}; // blocks 0..2 (3 output blocks)
  pumpcfg.resolve = map_resolver(binding);
  pumpcfg.sample_rate = k_rate;
  pumpcfg.layout = ChannelLayout::Stereo;
  pumpcfg.block_frames = k_block_frames;
  pumpcfg.tick_period = std::chrono::hours(1);
  pumpcfg.tick_source = [&fake_tick] { return fake_tick.fetch_add(1, std::memory_order_relaxed); };
  pumpcfg.playhead_source = [] { return Time::zero(); };
  LookaheadPump pump(ring, blocks, pool, pumpcfg);

  pump.flush();
  pump.flush();

  constexpr std::int64_t k_blocks = 3;
  // The fill enumerated the WHOLE tree (not one level): per output block, each tone
  // leaf and the nested contributor were submitted exactly once -- 3 contributors x 3
  // blocks -- demonstrating recursive descent + bottom-up dispatch gating.
  REQUIRE(pool.tasks_submitted() == 3 * static_cast<std::uint64_t>(k_blocks));
  REQUIRE(pool.tasks_completed() == pool.tasks_submitted());
  // Every worker's nested `render_audio` found its descendants resident: its inner
  // pull_audio issued 0 dispatches (all cache hits), and the top-level mix hit too --
  // a homogeneous exact scene warms to zero dispatch through the whole fill.
  REQUIRE(pull.dispatches() == 0);
  // No output block mixed while a transitive contributor was absent.
  REQUIRE(ring.silence_mixed() == 0);
  REQUIRE(ring.blocks_mixed() == static_cast<std::uint64_t>(k_blocks));

  // Drain issues no render/pull dispatch (pure-consume).
  const std::uint64_t before_drain = pull.dispatches();
  for (std::int64_t i = 0; i < k_blocks; ++i) {
    std::vector<float> got(static_cast<std::size_t>(k_block_frames) * 2, 0.0F);
    AudioBlock out{got.data(), k_block_frames, ChannelLayout::Stereo, k_rate};
    AudioResult meta{};
    REQUIRE(pump.drain(i, out, meta));
  }
  REQUIRE(pull.dispatches() == before_drain);

  pump.request_stop();
}

// enforces: 12-audio#lookahead-warms-recursive-contributor-closure
// enforces: 12-audio#nested-boundary-resamples-below-rate-children
TEST_CASE("the threaded pump drains a below-rate native contributor byte-identical to inline") {
  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(k_rate);
  const std::int64_t span = static_cast<std::int64_t>(k_block_frames) * fpf;

  for (const std::size_t worker_count : {std::size_t{0}, std::size_t{4}}) {
    BelowRateSource src(24'000); // native 24 kHz < 48 kHz working rate -> below-rate

    Model model;
    std::unordered_map<ObjectId, Content*> binding;
    std::unordered_map<const Content*, ObjectId> ids;
    ObjectId c_root{};
    {
      auto tx = model.transact("below-rate leaf");
      c_root = tx.add_composition(0.0, 0.0);
      const ObjectId cs = tx.add_content(1);
      tx.attach_layer(c_root, tx.add_layer(cs, Affine::identity()));
      tx.commit();
      binding[cs] = &src;
      ids[&src] = cs;
    }
    const DocStatePtr doc = model.current();

    BlockCache blocks{64u * 1024 * 1024};
    CachingPull pull(
        &blocks,
        [&ids](const Content* c) {
          const auto it = ids.find(c);
          return it != ids.end() ? it->second : ObjectId{};
        },
        doc->revision());

    LookaheadRingConfig ringcfg;
    ringcfg.composition = c_root;
    ringcfg.resolve = map_resolver(binding);
    ringcfg.sample_rate = k_rate;
    ringcfg.layout = ChannelLayout::Stereo;
    ringcfg.block_frames = k_block_frames;
    ringcfg.contribution = [rev = doc->revision()](ObjectId) { return rev; };
    // No nesting here: the below-rate contributor is a native leaf, so the closure is
    // its discovery block + its native re-request block (both leaf renders).
    LookaheadRing ring(*doc, pull, ringcfg);

    AudioWorkerPoolConfig poolcfg;
    poolcfg.worker_count = worker_count;
    AudioWorkerPool pool(poolcfg);

    std::atomic<std::uint64_t> fake_tick{0};
    LookaheadPumpConfig pumpcfg;
    pumpcfg.horizon = Time{2 * span}; // blocks 0..2
    pumpcfg.resolve = map_resolver(binding);
    pumpcfg.sample_rate = k_rate;
    pumpcfg.layout = ChannelLayout::Stereo;
    pumpcfg.block_frames = k_block_frames;
    pumpcfg.tick_period = std::chrono::hours(1);
    pumpcfg.tick_source = [&fake_tick] {
      return fake_tick.fetch_add(1, std::memory_order_relaxed);
    };
    pumpcfg.playhead_source = [] { return Time::zero(); };
    LookaheadPump pump(ring, blocks, pool, pumpcfg);

    pump.flush();
    pump.flush();

    // The fill discovered the native re-request lazily (from the resident
    // achieved_rate) and warmed it: 2 tasks per block (discovery + native).
    REQUIRE(pool.tasks_submitted() == 2 * 3);
    REQUIRE(pool.tasks_completed() == pool.tasks_submitted());
    REQUIRE(ring.silence_mixed() == 0);

    CachingPull inline_pull(nullptr, {}, 0); // no cache: render every pull inline
    for (std::int64_t i = 0; i < 3; ++i) {
      std::vector<float> got(static_cast<std::size_t>(k_block_frames) * 2, 0.0F);
      AudioBlock out{got.data(), k_block_frames, ChannelLayout::Stereo, k_rate};
      AudioResult meta{};
      REQUIRE(pump.drain(i, out, meta));
      // The below-rate boundary was band-limit-reconstructed identically to the inline
      // path (which resamples via the 16-tap Blackman-Harris / 32-phase kernel).
      const std::vector<float> want =
          inline_mix(*doc, c_root, map_resolver(binding), inline_pull, i);
      REQUIRE(bytes_equal(got, want));
      // Honesty preserved: the aggregate still reports the child's native rate, inexact.
      REQUIRE(meta.achieved_rate == 24'000);
      REQUIRE(meta.exact == false);
    }

    pump.request_stop();
  }
}
