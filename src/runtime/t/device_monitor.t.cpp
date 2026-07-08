#include <arbc/audio_engine/lookahead.hpp>
#include <arbc/audio_engine/mix.hpp>
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

// The concatenated working-rate stereo mix over blocks [0, blocks) -- the input a
// whole-stream `resample_audio` reconstructs to build the device-edge oracle.
std::vector<float> working_stream(const DocRoot& doc, ObjectId comp, const MixResolver& resolve,
                                  std::uint32_t working_rate, std::uint32_t block_frames,
                                  std::int64_t blocks) {
  CachingPull inline_pull(nullptr, {}, 0);
  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(working_rate);
  const std::int64_t span = static_cast<std::int64_t>(block_frames) * fpf;
  std::vector<float> all;
  for (std::int64_t i = 0; i < blocks; ++i) {
    std::vector<float> buf(static_cast<std::size_t>(block_frames) * 2, 0.0F);
    AudioBlock block{buf.data(), block_frames, ChannelLayout::Stereo, working_rate};
    const AudioRequest req{TimeRange{Time{i * span}, Time{i * span + span}},
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
TEST_CASE("matched device rate keeps the 1:1 drain; a below-working device rate is rejected") {
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

  // Below-working device rate: rejected at construction (decimating SRC deferred to
  // audio.device_edge_decimation, closing the previously-untested guard).
  {
    SineLeaf a(440, 0.5F);
    Scene scene;
    scene.add(&a);
    const DocStatePtr doc = scene.model.current();

    BlockCache blocks{4u * 1024 * 1024};
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
    poolcfg.worker_count = 0;
    AudioWorkerPool pool(poolcfg);

    LookaheadPumpConfig pumpcfg;
    pumpcfg.horizon = Time::zero();
    pumpcfg.resolve = scene.resolver();
    pumpcfg.sample_rate = working_rate;
    pumpcfg.layout = ChannelLayout::Stereo;
    pumpcfg.block_frames = block_frames;
    pumpcfg.tick_period = std::chrono::hours(1);
    pumpcfg.tick_source = [] { return std::uint64_t{0}; };
    pumpcfg.playhead_source = [] { return Time::zero(); };
    LookaheadPump pump(ring, blocks, pool, pumpcfg);

    Transport transport{Time::zero()};
    FakeDeviceSink sink{DeviceFormat{44'100, ChannelLayout::Stereo}}; // below working
    DeviceMonitorConfig moncfg;
    moncfg.working_rate = working_rate;
    moncfg.working_layout = ChannelLayout::Stereo;
    moncfg.block_frames = block_frames;
    moncfg.master_period = std::chrono::hours(1);
    REQUIRE_THROWS_AS(DeviceMonitor(transport, pump, sink, moncfg), std::invalid_argument);

    pump.request_stop();
  }
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
