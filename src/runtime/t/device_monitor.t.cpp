#include <arbc/audio_engine/lookahead.hpp>
#include <arbc/audio_engine/mix.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/compositor/pull_service.hpp> // BlockCache, AudioBlockValue
#include <arbc/contract/content.hpp>
#include <arbc/media/audio_block.hpp>
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
    REQUIRE(monitor.delivered_frames() ==
            static_cast<std::uint64_t>(k_blocks) * k_block_frames);

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
  REQUIRE(bytes_equal(b1, silence));   // silence, not an inline mix
  REQUIRE(monitor.underruns() == 1);   // exactly one starved block counted
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

// enforces: 12-audio#device-clock-masters-transport
TEST_CASE("free-run: a transport with no device monitor advances byte-identically (no regression)") {
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
