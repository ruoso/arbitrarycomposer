#include <arbc/audio_engine/lookahead.hpp>
#include <arbc/audio_engine/mix.hpp>
#include <arbc/base/rt_safety.hpp> // RtScope (the RT-safety enforcement guard)
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/compositor/pull_service.hpp> // BlockCache, AudioBlockValue
#include <arbc/contract/content.hpp>
#include <arbc/media/audio_block.hpp>
#include <arbc/media/audio_resampler.hpp>
#include <arbc/model/model.hpp>
#include <arbc/runtime/audio_worker_pool.hpp>
#include <arbc/runtime/device_monitor.hpp>
#include <arbc/runtime/device_sink.hpp>
#include <arbc/runtime/lookahead_pump.hpp>
#include <arbc/runtime/transport.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <vector>

// Integration tests for the device monitor + clock mastering (doc 12:155-178),
// the thin runtime adapter turning a device callback into `pump.drain`
// consumption and mastering the transport from delivered samples. A test-driven
// FAKE `DeviceSink` is the clock: it captures the monitor's RT fill callback and
// the test invokes it with a scripted frame count -- no hardware, no wall clock.
// It witnesses: the drained device bytes equal a direct `mix_composition` oracle
// byte-identically and worker-count-independently (the callback runs NO plugin
// code); after the sink delivers K frames at rate R the transport position and
// the lock-free published snapshot both advance by exactly K/R; the RT callback
// never touches the transport (position stays put until the owner thread
// masters); a starved block yields silence + an underrun, never an inline mix; a
// second monitor on one transport is rejected; and a transport with no monitor
// free-runs unchanged. Local Content / PullService doubles keep the includes
// inside runtime's dependency closure (no `kind_tone` / `backend_cpu`).

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

// A clean-tone leaf for the anti-alias spectral assertion: a genuine `std::sin`
// sinusoid over an exact flick phase (this is TEST code -- not the RT/portability
// path, so libm is fine here). Unlike `SineLeaf`'s parabolic approximation it has no
// harmonics, so the decimation stopband can be measured against a single alias bin.
class PureSineLeaf final : public Content {
public:
  PureSineLeaf(std::uint32_t freq_hz, float amp) : d_facet(freq_hz, amp) {}
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
      const double fps = static_cast<double>(Time::flicks_per_second);
      for (std::uint32_t f = 0; f < request.target.frames; ++f) {
        const std::int64_t t = request.window.start.flicks + static_cast<std::int64_t>(f) * fpf;
        const double v = static_cast<double>(d_amp) *
                         std::sin(2.0 * 3.14159265358979323846 * static_cast<double>(d_freq) *
                                  static_cast<double>(t) / fps);
        for (std::uint32_t c = 0; c < ch; ++c) {
          request.target.samples[static_cast<std::size_t>(f) * ch + c] = static_cast<float>(v);
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
// dispatches (misses) so the test can assert the device callback runs no plugin code.
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
constexpr std::int64_t k_blocks = 6;

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

// The test-driven fake device sink: it captures the monitor's RT fill callback in
// `start`, and `deliver(frames)` invokes it into a fresh device buffer and returns
// the produced interleaved Float32 -- the fake sink IS the clock (no hardware, no
// wall clock). `deliver` stands in for one device RT callback.
class FakeDeviceSink final : public DeviceSink {
public:
  explicit FakeDeviceSink(DeviceFormat fmt) : d_format(fmt) {}
  DeviceFormat format() const override { return d_format; }
  void start(DeviceFillCallback fill) override { d_fill = std::move(fill); }
  void stop() override { d_fill = nullptr; }

  std::vector<float> deliver(std::uint32_t frames) {
    std::vector<float> buf(static_cast<std::size_t>(frames) * channel_count(d_format.layout), 0.0F);
    d_fill(buf.data(), frames);
    return buf;
  }

private:
  DeviceFormat d_format;
  DeviceFillCallback d_fill;
};

// The concatenated working-rate stereo mix over blocks [start, start+blocks) -- the
// input a whole-stream `resample_audio` reconstructs, and (at a matched rate) the bytes
// the device drains directly. A post-seek/-rate realign resumes the drain at the block
// covering the reprimed playhead (start_block_index()), so the oracle for post-change
// output starts at THAT block, not at block 0 (audio.seek_drain_realign).
std::vector<float> working_stream_from(const DocRoot& doc, ObjectId comp,
                                       const MixResolver& resolve, std::uint32_t working_rate,
                                       std::uint32_t block_frames, std::int64_t start,
                                       std::int64_t blocks) {
  CachingPull inline_pull(nullptr, {}, 0);
  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(working_rate);
  const std::int64_t span = static_cast<std::int64_t>(block_frames) * fpf;
  std::vector<float> all;
  for (std::int64_t i = 0; i < blocks; ++i) {
    const std::int64_t bi = start + i;
    std::vector<float> buf(static_cast<std::size_t>(block_frames) * 2, 0.0F);
    AudioBlock block{buf.data(), block_frames, ChannelLayout::Stereo, working_rate};
    const AudioRequest req{TimeRange{Time{bi * span}, Time{bi * span + span}},
                           working_rate,
                           ChannelLayout::Stereo,
                           block,
                           Exactness::BestEffort,
                           StateHandle{}};
    mix_composition(doc, comp, resolve, inline_pull, req);
    all.insert(all.end(), buf.begin(), buf.end());
  }
  return all;
}

// The concatenated working-rate stereo mix over blocks [0, blocks) -- the input a
// whole-stream `resample_audio` reconstructs to build the device-edge oracle.
std::vector<float> working_stream(const DocRoot& doc, ObjectId comp, const MixResolver& resolve,
                                  std::uint32_t working_rate, std::uint32_t block_frames,
                                  std::int64_t blocks) {
  return working_stream_from(doc, comp, resolve, working_rate, block_frames, 0, blocks);
}

// A single whole-stream `resample_audio` of a working-layout stereo stream to the
// device rate -- the byte-exact oracle the streaming device edge must reproduce.
std::vector<float> whole_stream_resample(const std::vector<float>& working,
                                         std::uint32_t working_rate, std::uint32_t device_rate,
                                         std::uint32_t out_frames) {
  const std::uint32_t in_frames = static_cast<std::uint32_t>(working.size() / 2);
  std::vector<float> out(static_cast<std::size_t>(out_frames) * 2, 0.0F);
  AudioBlock in_block{const_cast<float*>(working.data()), in_frames, ChannelLayout::Stereo,
                      working_rate};
  AudioBlock out_block{out.data(), out_frames, ChannelLayout::Stereo, device_rate};
  resample_audio(in_block, out_block);
  return out;
}

// Build the full ring/pump/monitor pipeline around `scene` at the given working
// and device rates, prime the horizon, then deliver device frames in the scripted
// chunk sizes and return the concatenated device-layout output. Reused across the
// upsample / matched-rate goldens (both worker counts).
std::vector<float> drive_device(const DocStatePtr& doc, ObjectId comp, const MixResolver& resolve,
                                const std::function<ObjectId(const Content*)>& id_of,
                                std::uint32_t working_rate, std::uint32_t device_rate,
                                std::uint32_t block_frames, std::size_t worker_count,
                                std::int64_t horizon_blocks,
                                const std::vector<std::uint32_t>& chunks) {
  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(working_rate);
  const std::int64_t span = static_cast<std::int64_t>(block_frames) * fpf;

  BlockCache blocks{64u * 1024 * 1024};
  CachingPull pull(&blocks, id_of, doc->revision());

  LookaheadRingConfig ringcfg;
  ringcfg.composition = comp;
  ringcfg.resolve = resolve;
  ringcfg.sample_rate = working_rate;
  ringcfg.layout = ChannelLayout::Stereo;
  ringcfg.block_frames = block_frames;
  ringcfg.revision = doc->revision();
  LookaheadRing ring(*doc, pull, ringcfg);

  AudioWorkerPoolConfig poolcfg;
  poolcfg.worker_count = worker_count;
  AudioWorkerPool pool(poolcfg);

  Transport transport{Time::zero()};
  std::atomic<DeviceMonitor*> monptr{nullptr};

  LookaheadPumpConfig pumpcfg;
  pumpcfg.horizon = Time{(horizon_blocks - 1) * span};
  pumpcfg.resolve = resolve;
  pumpcfg.sample_rate = working_rate;
  pumpcfg.layout = ChannelLayout::Stereo;
  pumpcfg.block_frames = block_frames;
  pumpcfg.tick_period = std::chrono::hours(1);
  pumpcfg.tick_source = [] { return std::uint64_t{0}; };
  pumpcfg.playhead_source = [&monptr] {
    DeviceMonitor* m = monptr.load(std::memory_order_acquire);
    return m != nullptr ? m->playhead_snapshot() : Time::zero();
  };
  pumpcfg.direction_source = [&monptr] {
    DeviceMonitor* m = monptr.load(std::memory_order_acquire);
    return m != nullptr ? m->direction_snapshot() : 1;
  };
  LookaheadPump pump(ring, blocks, pool, pumpcfg);

  FakeDeviceSink sink{DeviceFormat{device_rate, ChannelLayout::Stereo}};
  DeviceMonitorConfig moncfg;
  moncfg.working_rate = working_rate;
  moncfg.working_layout = ChannelLayout::Stereo;
  moncfg.block_frames = block_frames;
  moncfg.master_period = std::chrono::hours(1);
  DeviceMonitor monitor(transport, pump, sink, moncfg);
  monptr.store(&monitor, std::memory_order_release);

  pump.flush();
  pump.flush();

  std::vector<float> out;
  for (const std::uint32_t n : chunks) {
    const std::vector<float> got = sink.deliver(n);
    out.insert(out.end(), got.begin(), got.end());
  }
  REQUIRE(monitor.underruns() == 0); // the horizon covers the delivered window
  pump.request_stop();
  return out;
}

// The post-seek-drain-realign fixture (audio.seek_drain_realign): build the pipeline,
// prime, deliver `pre_chunks` device frames, apply a transport change (a block-aligned
// `seek_to`, a `rate` change, both, or neither -> a plain advance), reprime, then
// deliver `post_chunks` and return ONLY the post-change device output plus the
// wall-clock-free counters the assertions pin. Mirrors `drive_device`.
struct SeekDrainResult {
  std::vector<float> post;          // device output drained AFTER the transport change
  std::int64_t start_block{0};      // block the drain re-seats to (floor(playhead/span))
  std::uint64_t drain_realigns{0};  // realign consumes counted by the RT callback
  std::uint64_t underruns_delta{0}; // underruns incurred by the post-change drain only
};

SeekDrainResult
drive_seek_realign(const DocStatePtr& doc, ObjectId comp, const MixResolver& resolve,
                   const std::function<ObjectId(const Content*)>& id_of, std::uint32_t working_rate,
                   std::uint32_t device_rate, std::uint32_t block_frames, std::size_t worker_count,
                   std::int64_t horizon_blocks, const std::vector<std::uint32_t>& pre_chunks,
                   std::optional<Time> seek_to, std::optional<Rational> rate,
                   const std::vector<std::uint32_t>& post_chunks) {
  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(working_rate);
  const std::int64_t span = static_cast<std::int64_t>(block_frames) * fpf;

  BlockCache blocks{64u * 1024 * 1024};
  CachingPull pull(&blocks, id_of, doc->revision());

  LookaheadRingConfig ringcfg;
  ringcfg.composition = comp;
  ringcfg.resolve = resolve;
  ringcfg.sample_rate = working_rate;
  ringcfg.layout = ChannelLayout::Stereo;
  ringcfg.block_frames = block_frames;
  ringcfg.revision = doc->revision();
  LookaheadRing ring(*doc, pull, ringcfg);

  AudioWorkerPoolConfig poolcfg;
  poolcfg.worker_count = worker_count;
  AudioWorkerPool pool(poolcfg);

  Transport transport{Time::zero()};
  std::atomic<DeviceMonitor*> monptr{nullptr};

  LookaheadPumpConfig pumpcfg;
  pumpcfg.horizon = Time{(horizon_blocks - 1) * span};
  pumpcfg.resolve = resolve;
  pumpcfg.sample_rate = working_rate;
  pumpcfg.layout = ChannelLayout::Stereo;
  pumpcfg.block_frames = block_frames;
  pumpcfg.tick_period = std::chrono::hours(1);
  pumpcfg.tick_source = [] { return std::uint64_t{0}; };
  pumpcfg.playhead_source = [&monptr] {
    DeviceMonitor* m = monptr.load(std::memory_order_acquire);
    return m != nullptr ? m->playhead_snapshot() : Time::zero();
  };
  pumpcfg.direction_source = [&monptr] {
    DeviceMonitor* m = monptr.load(std::memory_order_acquire);
    return m != nullptr ? m->direction_snapshot() : 1;
  };
  LookaheadPump pump(ring, blocks, pool, pumpcfg);

  FakeDeviceSink sink{DeviceFormat{device_rate, ChannelLayout::Stereo}};
  DeviceMonitorConfig moncfg;
  moncfg.working_rate = working_rate;
  moncfg.working_layout = ChannelLayout::Stereo;
  moncfg.block_frames = block_frames;
  moncfg.master_period = std::chrono::hours(1);
  DeviceMonitor monitor(transport, pump, sink, moncfg);
  monptr.store(&monitor, std::memory_order_release);

  pump.flush();
  pump.flush();

  for (const std::uint32_t n : pre_chunks) {
    sink.deliver(n);
  }

  // Apply the transport change on the owner thread, then reprime the ring around the
  // freshly-published playhead (mirrors the seek/resampler-flush tests).
  if (seek_to.has_value()) {
    monitor.seek(*seek_to);
  }
  if (rate.has_value()) {
    monitor.set_rate(*rate);
  }
  monitor.flush_master();
  pump.flush();
  pump.flush();

  SeekDrainResult res;
  const std::int64_t playhead = monitor.playhead_snapshot().flicks;
  res.start_block = span != 0 ? playhead / span : 0; // playhead >= 0 in these fixtures
  const std::uint64_t underruns_before = monitor.underruns();
  for (const std::uint32_t n : post_chunks) {
    const std::vector<float> got = sink.deliver(n);
    res.post.insert(res.post.end(), got.begin(), got.end());
  }
  res.drain_realigns = monitor.drain_realigns();
  res.underruns_delta = monitor.underruns() - underruns_before;
  pump.request_stop();
  return res;
}

} // namespace

// enforces: 12-audio#device-clock-masters-transport
// enforces: 12-audio#device-callback-consumes-prepared-blocks-only
TEST_CASE("device monitor masters the transport from samples; drained bytes equal the mix oracle") {
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

    Transport transport{Time::zero()};
    std::atomic<DeviceMonitor*> monptr{nullptr};

    std::atomic<std::uint64_t> fake_tick{0};
    LookaheadPumpConfig pumpcfg;
    pumpcfg.horizon = Time{(k_blocks - 1) * span}; // blocks 0..k_blocks-1
    pumpcfg.resolve = scene.resolver();
    pumpcfg.sample_rate = k_rate;
    pumpcfg.layout = ChannelLayout::Stereo;
    pumpcfg.block_frames = k_block_frames;
    pumpcfg.tick_period = std::chrono::hours(1); // only ticks on poke/flush
    pumpcfg.tick_source = [&fake_tick] {
      return fake_tick.fetch_add(1, std::memory_order_relaxed);
    };
    // Video chases audio: the pump samples the mastered snapshot, not the transport.
    pumpcfg.playhead_source = [&monptr] {
      DeviceMonitor* m = monptr.load(std::memory_order_acquire);
      return m != nullptr ? m->playhead_snapshot() : Time::zero();
    };
    pumpcfg.direction_source = [&monptr] {
      DeviceMonitor* m = monptr.load(std::memory_order_acquire);
      return m != nullptr ? m->direction_snapshot() : 1;
    };
    LookaheadPump pump(ring, blocks, pool, pumpcfg);

    FakeDeviceSink sink{DeviceFormat{k_rate, ChannelLayout::Stereo}};
    DeviceMonitorConfig moncfg;
    moncfg.working_rate = k_rate;
    moncfg.working_layout = ChannelLayout::Stereo;
    moncfg.block_frames = k_block_frames;
    moncfg.master_period = std::chrono::hours(1); // only masters on flush_master/seek/set_rate
    DeviceMonitor monitor(transport, pump, sink, moncfg);
    monptr.store(&monitor, std::memory_order_release);

    // Prime the horizon against the (still-zero) mastered playhead.
    pump.flush();
    pump.flush();
    const std::uint64_t dispatches_after_prime = pull.dispatches();

    // Drive the device callback block-by-block: each delivered block is byte-identical
    // to a direct per-window mix oracle, and the callback issues NO new dispatch.
    for (std::int64_t i = 0; i < k_blocks; ++i) {
      const std::vector<float> got = sink.deliver(k_block_frames);
      const std::vector<float> want = direct_mix(*doc, scene.comp, scene.resolver(), i);
      REQUIRE(bytes_equal(got, want));
    }
    REQUIRE(pull.dispatches() == dispatches_after_prime); // no plugin code on the callback
    REQUIRE(monitor.underruns() == 0);
    REQUIRE(monitor.delivered_frames() == static_cast<std::uint64_t>(k_blocks) * k_block_frames);

    // RT purity: the callback delivered K frames but the transport is untouched until
    // the single owner thread masters (the RT thread never mutates the transport).
    REQUIRE(transport.position() == Time::zero());

    // Master the clock: after K frames at rate R the transport position AND the
    // lock-free published snapshot both advance by exactly K/R.
    monitor.flush_master();
    const std::int64_t k_frames = k_blocks * k_block_frames;
    const Time expected{k_frames * fpf};
    REQUIRE(transport.position() == expected);
    REQUIRE(monitor.playhead_snapshot() == expected);

    pump.request_stop();
  }
}

// enforces: 12-audio#device-callback-consumes-prepared-blocks-only
TEST_CASE("a starved block yields silence + an underrun, never an inline mix") {
  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(k_rate);
  const std::int64_t span = static_cast<std::int64_t>(k_block_frames) * fpf;

  SineLeaf a(440, 0.5F);
  Scene scene;
  scene.add(&a);
  const DocStatePtr doc = scene.model.current();

  BlockCache blocks{16u * 1024 * 1024};
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
  poolcfg.worker_count = 0;
  AudioWorkerPool pool(poolcfg);

  Transport transport{Time::zero()};
  std::atomic<DeviceMonitor*> monptr{nullptr};

  LookaheadPumpConfig pumpcfg;
  pumpcfg.horizon = Time{span - 1}; // prime only the anchor block 0; block 1+ is starved
  pumpcfg.resolve = scene.resolver();
  pumpcfg.sample_rate = k_rate;
  pumpcfg.layout = ChannelLayout::Stereo;
  pumpcfg.block_frames = k_block_frames;
  pumpcfg.tick_period = std::chrono::hours(1);
  pumpcfg.tick_source = [] { return std::uint64_t{0}; };
  pumpcfg.playhead_source = [&monptr] {
    DeviceMonitor* m = monptr.load(std::memory_order_acquire);
    return m != nullptr ? m->playhead_snapshot() : Time::zero();
  };
  LookaheadPump pump(ring, blocks, pool, pumpcfg);

  FakeDeviceSink sink{DeviceFormat{k_rate, ChannelLayout::Stereo}};
  DeviceMonitorConfig moncfg;
  moncfg.working_rate = k_rate;
  moncfg.working_layout = ChannelLayout::Stereo;
  moncfg.block_frames = k_block_frames;
  moncfg.master_period = std::chrono::hours(1);
  DeviceMonitor monitor(transport, pump, sink, moncfg);
  monptr.store(&monitor, std::memory_order_release);

  pump.flush();
  const std::uint64_t dispatches_after_prime = pull.dispatches();

  // Block 0 is prepared; block 1 was never primed (beyond the horizon) -> starved.
  const std::vector<float> b0 = sink.deliver(k_block_frames);
  REQUIRE(bytes_equal(b0, direct_mix(*doc, scene.comp, scene.resolver(), 0)));
  REQUIRE(monitor.underruns() == 0);

  const std::vector<float> b1 = sink.deliver(k_block_frames);
  const std::vector<float> silence(static_cast<std::size_t>(k_block_frames) * 2, 0.0F);
  REQUIRE(bytes_equal(b1, silence));                    // silence, not an inline mix
  REQUIRE(monitor.underruns() == 1);                    // exactly one starved block counted
  REQUIRE(pull.dispatches() == dispatches_after_prime); // the callback dispatched nothing

  pump.request_stop();
}

// enforces: 12-audio#device-clock-masters-transport
TEST_CASE("host seek and set_rate rebase the mastered clock and reprime") {
  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(k_rate);
  const std::int64_t span = static_cast<std::int64_t>(k_block_frames) * fpf;

  SineLeaf a(440, 0.5F);
  Scene scene;
  scene.add(&a);
  const DocStatePtr doc = scene.model.current();

  BlockCache blocks{32u * 1024 * 1024};
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
  poolcfg.worker_count = 0;
  AudioWorkerPool pool(poolcfg);

  Transport transport{Time::zero()};
  std::atomic<DeviceMonitor*> monptr{nullptr};

  LookaheadPumpConfig pumpcfg;
  pumpcfg.horizon = Time{(k_blocks - 1) * span};
  pumpcfg.resolve = scene.resolver();
  pumpcfg.sample_rate = k_rate;
  pumpcfg.layout = ChannelLayout::Stereo;
  pumpcfg.block_frames = k_block_frames;
  pumpcfg.tick_period = std::chrono::hours(1);
  pumpcfg.tick_source = [] { return std::uint64_t{0}; };
  pumpcfg.playhead_source = [&monptr] {
    DeviceMonitor* m = monptr.load(std::memory_order_acquire);
    return m != nullptr ? m->playhead_snapshot() : Time::zero();
  };
  pumpcfg.direction_source = [&monptr] {
    DeviceMonitor* m = monptr.load(std::memory_order_acquire);
    return m != nullptr ? m->direction_snapshot() : 1;
  };
  LookaheadPump pump(ring, blocks, pool, pumpcfg);

  FakeDeviceSink sink{DeviceFormat{k_rate, ChannelLayout::Stereo}};
  DeviceMonitorConfig moncfg;
  moncfg.working_rate = k_rate;
  moncfg.working_layout = ChannelLayout::Stereo;
  moncfg.block_frames = k_block_frames;
  moncfg.master_period = std::chrono::hours(1);
  DeviceMonitor monitor(transport, pump, sink, moncfg);
  monptr.store(&monitor, std::memory_order_release);

  // Deliver two blocks and master: the clock advances by 2*block/rate.
  sink.deliver(k_block_frames);
  sink.deliver(k_block_frames);
  monitor.flush_master();
  REQUIRE(transport.position() == Time{2 * k_block_frames * fpf});

  // Seek rebases the master's sample origin: the pre-seek frames do NOT re-advance
  // past the target, and the published snapshot jumps to exactly the seek target.
  const Time target{123 * fpf};
  monitor.seek(target);
  monitor.flush_master();
  REQUIRE(transport.position() == target);
  REQUIRE(monitor.playhead_snapshot() == target);

  // Frames delivered AFTER the seek advance from the rebased origin (not double-counted).
  sink.deliver(k_block_frames);
  monitor.flush_master();
  REQUIRE(transport.position() == Time{target.flicks + k_block_frames * fpf});

  // A reverse rate is published to the direction snapshot the pump chases.
  monitor.set_rate(Rational{-1, 1});
  monitor.flush_master();
  REQUIRE(monitor.direction_snapshot() == -1);
  REQUIRE(transport.rate().num() < 0);

  pump.request_stop();
}

// enforces: 12-audio#device-clock-masters-transport
TEST_CASE("one device monitor per transport is rejected") {
  SineLeaf a(440, 0.5F);
  Scene scene;
  scene.add(&a);
  const DocStatePtr doc = scene.model.current();

  BlockCache blocks{4u * 1024 * 1024};
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
  poolcfg.worker_count = 0;
  AudioWorkerPool pool(poolcfg);

  LookaheadPumpConfig pumpcfg;
  pumpcfg.horizon = Time::zero();
  pumpcfg.resolve = scene.resolver();
  pumpcfg.sample_rate = k_rate;
  pumpcfg.layout = ChannelLayout::Stereo;
  pumpcfg.block_frames = k_block_frames;
  pumpcfg.tick_period = std::chrono::hours(1);
  pumpcfg.tick_source = [] { return std::uint64_t{0}; };
  pumpcfg.playhead_source = [] { return Time::zero(); };
  LookaheadPump pump(ring, blocks, pool, pumpcfg);

  Transport transport{Time::zero()};
  FakeDeviceSink sink_a{DeviceFormat{k_rate, ChannelLayout::Stereo}};
  FakeDeviceSink sink_b{DeviceFormat{k_rate, ChannelLayout::Stereo}};
  DeviceMonitorConfig moncfg;
  moncfg.working_rate = k_rate;
  moncfg.working_layout = ChannelLayout::Stereo;
  moncfg.block_frames = k_block_frames;
  moncfg.master_period = std::chrono::hours(1);

  DeviceMonitor first(transport, pump, sink_a, moncfg);
  REQUIRE_THROWS_AS(DeviceMonitor(transport, pump, sink_b, moncfg), std::logic_error);

  pump.request_stop();
}

// enforces: 12-audio#device-edge-resamples-working-to-device
TEST_CASE("device monitor upsamples working->device byte-exact vs a whole-stream oracle") {
  // Two ratios exercise the phase accumulator + history carry: an integer 1:2 and a
  // coprime 2:3. Many small deliver() chunks pin continuity across callback seams (no
  // per-block phase restart). Each is byte-identical between worker_count 0 and >0 via
  // the shared oracle. Device layout == working layout isolates SRC from layout remix.
  struct Config {
    std::uint32_t working_rate;
    std::uint32_t device_rate;
    std::uint32_t block_frames;
  };
  const std::vector<Config> configs = {{48'000, 96'000, 32}, {32'000, 48'000, 24}};
  const std::vector<std::uint32_t> chunks = {17, 31, 5, 40, 33, 22, 48, 11, 29, 39};
  constexpr std::int64_t horizon_blocks = 16;

  for (const Config& cfg : configs) {
    SineLeaf a(300, 0.6F);
    SineLeaf b(700, 0.4F);
    Scene scene;
    scene.add(&a);
    scene.add(&b);
    const DocStatePtr doc = scene.model.current();

    const std::vector<float> working = working_stream(
        *doc, scene.comp, scene.resolver(), cfg.working_rate, cfg.block_frames, horizon_blocks);

    for (const std::size_t worker_count : {std::size_t{0}, std::size_t{4}}) {
      const std::vector<float> got =
          drive_device(doc, scene.comp, scene.resolver(), scene.id_of(), cfg.working_rate,
                       cfg.device_rate, cfg.block_frames, worker_count, horizon_blocks, chunks);
      const std::uint32_t out_frames = static_cast<std::uint32_t>(got.size() / 2);
      const std::vector<float> want =
          whole_stream_resample(working, cfg.working_rate, cfg.device_rate, out_frames);
      REQUIRE(out_frames > 0);
      REQUIRE(bytes_equal(got, want)); // byte-exact, no tolerance
    }
  }
}

// enforces: 12-audio#device-edge-resamples-working-to-device
// enforces: 12-audio#device-edge-decimates-working-to-device
TEST_CASE("matched device rate keeps the 1:1 drain; a below-working device rate decimates") {
  constexpr std::uint32_t working_rate = 48'000;
  constexpr std::uint32_t block_frames = 32;
  constexpr std::int64_t horizon_blocks = 16;
  const std::vector<std::uint32_t> chunks = {32, 32, 32, 32};

  // Matched rate: zero resampler engagement, byte-for-byte the working mix (Constraint 5).
  {
    SineLeaf a(300, 0.6F);
    SineLeaf b(700, 0.4F);
    Scene scene;
    scene.add(&a);
    scene.add(&b);
    const DocStatePtr doc = scene.model.current();
    const std::vector<float> working = working_stream(*doc, scene.comp, scene.resolver(),
                                                      working_rate, block_frames, horizon_blocks);
    const std::vector<float> got =
        drive_device(doc, scene.comp, scene.resolver(), scene.id_of(), working_rate, working_rate,
                     block_frames, /*worker_count=*/0, horizon_blocks, chunks);
    REQUIRE(got.size() <= working.size());
    const std::vector<float> want(working.begin(),
                                  working.begin() + static_cast<std::ptrdiff_t>(got.size()));
    REQUIRE(bytes_equal(got, want));
  }

  // Below-working device rate: now decimated at the edge (audio.device_edge_decimation),
  // no longer rejected. The drained device bytes equal a whole-stream resample_audio
  // decimation of the working mix -- the same 44.1 kHz-under-48 kHz config the
  // predecessor rejected at construction, now succeeding and anti-aliased.
  {
    SineLeaf a(300, 0.6F);
    SineLeaf b(700, 0.4F);
    Scene scene;
    scene.add(&a);
    scene.add(&b);
    const DocStatePtr doc = scene.model.current();
    constexpr std::uint32_t device_rate = 44'100; // below working
    const std::vector<float> working = working_stream(*doc, scene.comp, scene.resolver(),
                                                      working_rate, block_frames, horizon_blocks);
    const std::vector<float> got =
        drive_device(doc, scene.comp, scene.resolver(), scene.id_of(), working_rate, device_rate,
                     block_frames, /*worker_count=*/0, horizon_blocks, chunks);
    const std::uint32_t out_frames = static_cast<std::uint32_t>(got.size() / 2);
    const std::vector<float> want =
        whole_stream_resample(working, working_rate, device_rate, out_frames);
    REQUIRE(out_frames > 0);
    REQUIRE(bytes_equal(got, want)); // byte-exact vs the whole-stream decimation oracle
  }
}

// enforces: 12-audio#device-edge-decimates-working-to-device
TEST_CASE("device monitor decimates working->device byte-exact vs a whole-stream oracle") {
  // Two ratios exercise the phase accumulator + widened history carry: the integer 2:1
  // (48k->24k) and the flagship coprime 160:147 (48k->44.1k). Many small deliver()
  // chunks pin continuity across callback seams (no per-block phase restart -- the
  // continuity golden of Constraint 3/4). Each is byte-identical between worker_count 0
  // and >0 via the shared whole-stream oracle. Decimation consumes MORE working frames
  // than it emits, so the horizon is sized well above the device-output span.
  struct Config {
    std::uint32_t working_rate;
    std::uint32_t device_rate;
    std::uint32_t block_frames;
  };
  const std::vector<Config> configs = {{48'000, 24'000, 32}, {48'000, 44'100, 32}};
  const std::vector<std::uint32_t> chunks = {17, 31, 5, 40, 33, 22, 48, 11, 29, 39};
  constexpr std::int64_t horizon_blocks = 40;

  for (const Config& cfg : configs) {
    SineLeaf a(300, 0.6F);
    SineLeaf b(700, 0.4F);
    Scene scene;
    scene.add(&a);
    scene.add(&b);
    const DocStatePtr doc = scene.model.current();

    const std::vector<float> working = working_stream(
        *doc, scene.comp, scene.resolver(), cfg.working_rate, cfg.block_frames, horizon_blocks);

    for (const std::size_t worker_count : {std::size_t{0}, std::size_t{4}}) {
      const std::vector<float> got =
          drive_device(doc, scene.comp, scene.resolver(), scene.id_of(), cfg.working_rate,
                       cfg.device_rate, cfg.block_frames, worker_count, horizon_blocks, chunks);
      const std::uint32_t out_frames = static_cast<std::uint32_t>(got.size() / 2);
      const std::vector<float> want =
          whole_stream_resample(working, cfg.working_rate, cfg.device_rate, out_frames);
      REQUIRE(out_frames > 0);
      REQUIRE(bytes_equal(got, want)); // byte-exact, no tolerance
    }
  }
}

// enforces: 12-audio#device-edge-decimates-working-to-device
TEST_CASE("device-edge decimation is anti-aliased: above-device-Nyquist content is suppressed") {
  // The one JUSTIFIED tolerance (doc 16): a stopband is inherently a threshold, not a
  // byte match. Decimating 48k -> 24k (device Nyquist 12 kHz), a 20 kHz working-rate
  // tone lies above the device Nyquist; without the ratio-scaled widened lowpass it
  // would fold to 24k-20k = 4 kHz at (near) full amplitude. The widened bank cut at the
  // device Nyquist attenuates it into the stopband, so the 4 kHz alias-band energy is
  // >= 60 dB below an in-band 4 kHz control tone driven through the same monitor. A
  // fixed-cutoff (un-widened, input-Nyquist) decimation passes 20 kHz unattenuated and
  // FAILS this -- the non-degenerate check distinguishing anti-aliasing from a naive
  // stride (D2).
  constexpr std::uint32_t working_rate = 48'000;
  constexpr std::uint32_t device_rate = 24'000;
  constexpr std::uint32_t block_frames = 32;
  constexpr std::int64_t horizon_blocks = 48;
  const std::vector<std::uint32_t> chunks = {512}; // integer cycles of 4 kHz at 24k

  // Left-channel single-bin magnitude at `freq` over the device output (device rate).
  const auto bin_mag = [](const std::vector<float>& out, double freq, std::uint32_t rate) {
    const std::size_t frames = out.size() / 2;
    double re = 0.0;
    double im = 0.0;
    for (std::size_t n = 0; n < frames; ++n) {
      const double s = static_cast<double>(out[n * 2]);
      const double ang =
          2.0 * 3.14159265358979323846 * freq * static_cast<double>(n) / static_cast<double>(rate);
      re += s * std::cos(ang);
      im += s * std::sin(ang);
    }
    return frames > 0 ? 2.0 * std::hypot(re, im) / static_cast<double>(frames) : 0.0;
  };

  const auto drive_tone = [&](std::uint32_t freq_hz) {
    PureSineLeaf tone(freq_hz, 0.5F);
    Scene scene;
    scene.add(&tone);
    const DocStatePtr doc = scene.model.current();
    return drive_device(doc, scene.comp, scene.resolver(), scene.id_of(), working_rate, device_rate,
                        block_frames, /*worker_count=*/0, horizon_blocks, chunks);
  };

  const std::vector<float> control = drive_tone(4'000);   // in-band, passes
  const std::vector<float> aliasing = drive_tone(20'000); // > device Nyquist, must be killed
  const double control_mag = bin_mag(control, 4'000.0, device_rate);
  const double alias_mag = bin_mag(aliasing, 4'000.0, device_rate);

  REQUIRE(control_mag > 0.1);                // the in-band control genuinely passes
  REQUIRE(alias_mag < control_mag * 1.0e-3); // >= 60 dB suppression of the folded tone
}

// enforces: 12-audio#device-edge-resamples-working-to-device
TEST_CASE(
    "a seek flushes the resampler filter state (post-flush restarts a fresh reconstruction)") {
  // A 1500 Hz tone is exactly one cycle per 32-frame block at 48 kHz, so every working
  // block is identical -- a fresh reconstruction is therefore independent of which block
  // the (deliberately un-realigned, peer-task) drain resumes from. Delivering an ODD
  // pre-flush device-frame total lands the un-flushed phase cursor on odd parity, so a
  // MISSED flush would diverge from the fresh oracle: the assertion is sensitive to it.
  constexpr std::uint32_t working_rate = 48'000;
  constexpr std::uint32_t device_rate = 96'000;
  constexpr std::uint32_t block_frames = 32;
  constexpr std::int64_t horizon_blocks = 16;
  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(working_rate);
  const std::int64_t span = static_cast<std::int64_t>(block_frames) * fpf;

  for (const std::size_t worker_count : {std::size_t{0}, std::size_t{4}}) {
    SineLeaf tone(1'500, 0.5F);
    Scene scene;
    scene.add(&tone);
    const DocStatePtr doc = scene.model.current();

    BlockCache blocks{64u * 1024 * 1024};
    CachingPull pull(&blocks, scene.id_of(), doc->revision());

    LookaheadRingConfig ringcfg;
    ringcfg.composition = scene.comp;
    ringcfg.resolve = scene.resolver();
    ringcfg.sample_rate = working_rate;
    ringcfg.layout = ChannelLayout::Stereo;
    ringcfg.block_frames = block_frames;
    ringcfg.revision = doc->revision();
    LookaheadRing ring(*doc, pull, ringcfg);

    AudioWorkerPoolConfig poolcfg;
    poolcfg.worker_count = worker_count;
    AudioWorkerPool pool(poolcfg);

    Transport transport{Time::zero()};
    std::atomic<DeviceMonitor*> monptr{nullptr};

    LookaheadPumpConfig pumpcfg;
    pumpcfg.horizon = Time{(horizon_blocks - 1) * span};
    pumpcfg.resolve = scene.resolver();
    pumpcfg.sample_rate = working_rate;
    pumpcfg.layout = ChannelLayout::Stereo;
    pumpcfg.block_frames = block_frames;
    pumpcfg.tick_period = std::chrono::hours(1);
    pumpcfg.tick_source = [] { return std::uint64_t{0}; };
    pumpcfg.playhead_source = [&monptr] {
      DeviceMonitor* m = monptr.load(std::memory_order_acquire);
      return m != nullptr ? m->playhead_snapshot() : Time::zero();
    };
    pumpcfg.direction_source = [&monptr] {
      DeviceMonitor* m = monptr.load(std::memory_order_acquire);
      return m != nullptr ? m->direction_snapshot() : 1;
    };
    LookaheadPump pump(ring, blocks, pool, pumpcfg);

    FakeDeviceSink sink{DeviceFormat{device_rate, ChannelLayout::Stereo}};
    DeviceMonitorConfig moncfg;
    moncfg.working_rate = working_rate;
    moncfg.working_layout = ChannelLayout::Stereo;
    moncfg.block_frames = block_frames;
    moncfg.master_period = std::chrono::hours(1);
    DeviceMonitor monitor(transport, pump, sink, moncfg);
    monptr.store(&monitor, std::memory_order_release);

    pump.flush();
    pump.flush();

    // Build non-trivial resampler state: 201 device frames (odd) mid-stream.
    sink.deliver(101);
    sink.deliver(100);

    // Seek rebases the master and asks the RT callback to flush the resampler's
    // phase + history; reprime around the (still-zero) playhead.
    monitor.seek(Time::zero());
    monitor.flush_master();
    pump.flush();
    pump.flush();

    // The first post-flush output must be a fresh whole-stream reconstruction.
    std::vector<float> got;
    for (const std::uint32_t n : {32u, 32u, 32u, 32u}) {
      const std::vector<float> chunk = sink.deliver(n);
      got.insert(got.end(), chunk.begin(), chunk.end());
    }
    const std::uint32_t out_frames = static_cast<std::uint32_t>(got.size() / 2);
    const std::vector<float> working = working_stream(*doc, scene.comp, scene.resolver(),
                                                      working_rate, block_frames, horizon_blocks);
    const std::vector<float> want =
        whole_stream_resample(working, working_rate, device_rate, out_frames);
    REQUIRE(out_frames > 0);
    REQUIRE(bytes_equal(got, want));
    REQUIRE(monitor.underruns() == 0);

    pump.request_stop();
  }
}

// enforces: 12-audio#device-drain-realigns-on-transport-change
// enforces: 12-audio#device-edge-decimates-working-to-device
TEST_CASE("post-seek/-rate device output realigns the drain to the reprimed window byte-exact") {
  // A NON-degenerate two-tone mix (300 Hz + 700 Hz, period not dividing the 32-frame
  // block) so consecutive working blocks differ and a mis-aligned drain cursor yields
  // observably wrong bytes -- unlike the degenerate 1500 Hz case above, where every
  // block is identical (D5). After a block-aligned seek OR a rate rebase the drain must
  // resume at the block covering the reprimed playhead: matched-rate bytes equal a fresh
  // `mix_composition` oracle at that block, device-edge SRC bytes equal a fresh
  // whole-stream `resample_audio` of it -- byte-identical between worker_count 0 and >0,
  // no tolerance (Constraint 7). A partial pre-change block (`pre = {50}`) exercises the
  // carry drop (Constraint 4 / D4).
  constexpr std::uint32_t block_frames = 32;
  constexpr std::int64_t horizon_blocks = 16;
  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(48'000);
  const std::int64_t span = static_cast<std::int64_t>(block_frames) * fpf;
  const std::vector<std::uint32_t> pre_chunks = {50}; // a partial pre-change block, dropped
  const std::vector<std::uint32_t> post_chunks = {32, 32, 32, 32};

  struct Case {
    std::uint32_t working_rate;
    std::uint32_t device_rate; // == working: matched 1:1; > working: upsample; < working: decimate
    bool use_rate;             // true: a set_rate rebase; false: a block-aligned seek
  };
  const std::vector<Case> cases = {
      {48'000, 48'000, false}, // matched-rate seek
      {48'000, 96'000, false}, // device-edge upsample seek
      {48'000, 24'000, false}, // device-edge decimation seek
      {48'000, 48'000, true},  // matched-rate set_rate
      {48'000, 96'000, true},  // device-edge upsample set_rate
      {48'000, 24'000, true},  // device-edge decimation set_rate
  };

  for (const Case& c : cases) {
    SineLeaf a(300, 0.6F);
    SineLeaf b(700, 0.4F);
    Scene scene;
    scene.add(&a);
    scene.add(&b);
    const DocStatePtr doc = scene.model.current();

    const std::optional<Time> seek_to =
        c.use_rate ? std::nullopt : std::optional<Time>{Time{5 * span}};
    const std::optional<Rational> rate =
        c.use_rate ? std::optional<Rational>{Rational{2, 1}} : std::nullopt;

    std::optional<std::vector<float>> golden; // pinned across worker counts
    for (const std::size_t worker_count : {std::size_t{0}, std::size_t{4}}) {
      const SeekDrainResult r = drive_seek_realign(
          doc, scene.comp, scene.resolver(), scene.id_of(), c.working_rate, c.device_rate,
          block_frames, worker_count, horizon_blocks, pre_chunks, seek_to, rate, post_chunks);
      REQUIRE(r.drain_realigns == 1);  // exactly one realign consumed (Constraint 6)
      REQUIRE(r.underruns_delta == 0); // the realigned cursor finds the resident window
      REQUIRE(!r.post.empty());

      const std::uint32_t out_frames = static_cast<std::uint32_t>(r.post.size() / 2);
      const std::vector<float> working =
          working_stream_from(*doc, scene.comp, scene.resolver(), c.working_rate, block_frames,
                              r.start_block, horizon_blocks);
      std::vector<float> want;
      if (c.device_rate == c.working_rate) {
        want.assign(working.begin(), working.begin() + static_cast<std::ptrdiff_t>(r.post.size()));
      } else {
        want = whole_stream_resample(working, c.working_rate, c.device_rate, out_frames);
      }
      REQUIRE(bytes_equal(r.post, want)); // byte-exact vs a fresh oracle at the new block

      if (!golden.has_value()) {
        golden = r.post;
      } else {
        REQUIRE(bytes_equal(r.post, *golden)); // worker_count 0 == worker_count 4
      }
    }
  }
}

// enforces: 12-audio#device-drain-realigns-on-transport-change
TEST_CASE("the RT drain-realign counter fires once per rebase and never on a plain advance") {
  constexpr std::uint32_t rate = 48'000;
  constexpr std::uint32_t block_frames = 32;
  constexpr std::int64_t horizon_blocks = 16;
  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(rate);
  const std::int64_t span = static_cast<std::int64_t>(block_frames) * fpf;

  SineLeaf a(300, 0.6F);
  SineLeaf b(700, 0.4F);
  Scene scene;
  scene.add(&a);
  scene.add(&b);
  const DocStatePtr doc = scene.model.current();

  // A plain advance (no seek/rate) publishes no realign request: the counter stays put.
  const SeekDrainResult plain =
      drive_seek_realign(doc, scene.comp, scene.resolver(), scene.id_of(), rate, rate, block_frames,
                         /*worker_count=*/0, horizon_blocks, /*pre=*/{50}, std::nullopt,
                         std::nullopt, /*post=*/{32, 32});
  REQUIRE(plain.drain_realigns == 0);

  // One seek + flush_master + fills consumes exactly one realign; the resident reprimed
  // window means the realigned cursor finds prepared blocks (zero extra underruns).
  const SeekDrainResult sought = drive_seek_realign(
      doc, scene.comp, scene.resolver(), scene.id_of(), rate, rate, block_frames,
      /*worker_count=*/0, horizon_blocks, /*pre=*/{50}, std::optional<Time>{Time{5 * span}},
      std::nullopt, /*post=*/{32, 32, 32, 32});
  REQUIRE(sought.drain_realigns == 1);
  REQUIRE(sought.underruns_delta == 0);

  // A set_rate rebase also realigns exactly once.
  const SeekDrainResult rated =
      drive_seek_realign(doc, scene.comp, scene.resolver(), scene.id_of(), rate, rate, block_frames,
                         /*worker_count=*/0, horizon_blocks, /*pre=*/{50}, std::nullopt,
                         std::optional<Rational>{Rational{2, 1}}, /*post=*/{32, 32, 32, 32});
  REQUIRE(rated.drain_realigns == 1);
}

// enforces: 12-audio#device-clock-masters-transport
TEST_CASE(
    "free-run: a transport with no device monitor advances byte-identically (no regression)") {
  // The device-mastering feature adds nothing to the non-device path: a transport
  // with no monitor advances exactly by the host-sampled elapsed duration, and is
  // mutated zero times by any device machinery.
  Transport bare{Time::zero()};
  Transport ref{Time::zero()};
  const Time elapsed{Time::flicks_per_second / 100}; // 10 ms of host time

  const auto a = bare.advance(elapsed);
  const auto b = ref.advance(elapsed);
  REQUIRE(a.has_value());
  REQUIRE(b.has_value());
  REQUIRE(*a == *b);
  REQUIRE(bare.position() == ref.position());
  REQUIRE(bare.position() != Time::zero()); // it genuinely advanced (non-vacuous)
}

// enforces: 12-audio#rt-callback-chain-is-nonblocking
TEST_CASE("the device RT callback chain is nonblocking under an armed RtScope") {
  // Structural upgrade of device_monitor's callback-purity counter (audio.rt_safety,
  // Decision D1/D2). `fill_rt` arms an `RtScope` for the whole callback body, so under
  // the debug-hardened build a heap allocation on the chain aborts build-failingly and
  // `RtScope::allocations()` counts them; the `[[clang::nonblocking]]` annotations put
  // the same chain under RealtimeSanitizer on the rtsan lane. Here we drive the real
  // callback across the shipped scenarios and assert zero allocation / lock / refcount
  // on the drain sweep -- lock-free by construction now that the pump-drain mutex is
  // gone (D2). `FakeDeviceSink::deliver` runs `fill_rt` on this thread, so the
  // thread-local counters read back what the callback did.
  auto run = [&](std::uint32_t working_rate, std::uint32_t device_rate, std::int64_t hb,
                 bool do_seek, bool starve) {
    const std::int64_t wfpf = Time::flicks_per_second / static_cast<std::int64_t>(working_rate);
    const std::int64_t wspan = static_cast<std::int64_t>(k_block_frames) * wfpf;

    SineLeaf a(440, 0.5F);
    Scene scene;
    scene.add(&a);
    const DocStatePtr doc = scene.model.current();

    BlockCache blocks{64u * 1024 * 1024};
    CachingPull pull(&blocks, scene.id_of(), doc->revision());
    LookaheadRingConfig ringcfg;
    ringcfg.composition = scene.comp;
    ringcfg.resolve = scene.resolver();
    ringcfg.sample_rate = working_rate;
    ringcfg.layout = ChannelLayout::Stereo;
    ringcfg.block_frames = k_block_frames;
    ringcfg.revision = doc->revision();
    LookaheadRing ring(*doc, pull, ringcfg);

    AudioWorkerPoolConfig poolcfg;
    poolcfg.worker_count = 0;
    AudioWorkerPool pool(poolcfg);

    Transport transport{Time::zero()};
    std::atomic<DeviceMonitor*> monptr{nullptr};
    LookaheadPumpConfig pumpcfg;
    pumpcfg.horizon = Time{(hb - 1) * wspan};
    pumpcfg.resolve = scene.resolver();
    pumpcfg.sample_rate = working_rate;
    pumpcfg.layout = ChannelLayout::Stereo;
    pumpcfg.block_frames = k_block_frames;
    pumpcfg.tick_period = std::chrono::hours(1);
    pumpcfg.tick_source = [] { return std::uint64_t{0}; };
    pumpcfg.playhead_source = [&monptr] {
      DeviceMonitor* m = monptr.load(std::memory_order_acquire);
      return m != nullptr ? m->playhead_snapshot() : Time::zero();
    };
    pumpcfg.direction_source = [&monptr] {
      DeviceMonitor* m = monptr.load(std::memory_order_acquire);
      return m != nullptr ? m->direction_snapshot() : 1;
    };
    LookaheadPump pump(ring, blocks, pool, pumpcfg);

    FakeDeviceSink sink{DeviceFormat{device_rate, ChannelLayout::Stereo}};
    DeviceMonitorConfig moncfg;
    moncfg.working_rate = working_rate;
    moncfg.working_layout = ChannelLayout::Stereo;
    moncfg.block_frames = k_block_frames;
    moncfg.master_period = std::chrono::hours(1);
    DeviceMonitor monitor(transport, pump, sink, moncfg);
    monptr.store(&monitor, std::memory_order_release);

    pump.flush();
    pump.flush();

    // Arm-and-measure: reset the thread-local RT counters, drive the real device
    // callback, and read back that the chain performed no forbidden operation.
    RtScope::reset_counts();
    const std::vector<std::uint32_t> chunks = {17, 31, 5, 40, 22};
    if (starve) {
      // Deliver well past the tiny primed horizon: the drain's silence + underrun
      // branch runs on the RT thread and must be allocation-free too.
      for (int i = 0; i < 8; ++i) {
        sink.deliver(k_block_frames);
      }
      REQUIRE(monitor.underruns() > 0);
    } else {
      for (const std::uint32_t n : chunks) {
        sink.deliver(n);
      }
      REQUIRE(monitor.underruns() == 0);
      if (do_seek) {
        // The post-seek realign consume is on the same armed callback: re-seat the
        // drain to a block boundary, reprime, and drive the realigned drain.
        monitor.seek(Time{3 * wspan});
        monitor.flush_master();
        pump.flush();
        for (const std::uint32_t n : chunks) {
          sink.deliver(n);
        }
        REQUIRE(monitor.drain_realigns() > 0);
        REQUIRE(monitor.underruns() == 0);
      }
    }
    // Zero heap allocations, zero lock acquisitions, zero refcount operations across
    // the whole RT callback chain -- the build-failing form of the purity check.
    REQUIRE(RtScope::allocations() == 0);
    REQUIRE(RtScope::locks() == 0);
    REQUIRE(RtScope::refcounts() == 0);

    pump.request_stop();
  };

  SECTION("matched-rate 1:1 drain, forward + post-seek realign") {
    run(k_rate, k_rate, 48, /*do_seek=*/true, /*starve=*/false);
  }
  SECTION("device-edge upsample 48k->96k") {
    run(k_rate, 96'000, 48, /*do_seek=*/false, /*starve=*/false);
  }
  SECTION("device-edge decimate 48k->24k") {
    run(k_rate, 24'000, 48, /*do_seek=*/false, /*starve=*/false);
  }
  SECTION("starved underrun path") { run(k_rate, k_rate, 2, /*do_seek=*/false, /*starve=*/true); }
}
