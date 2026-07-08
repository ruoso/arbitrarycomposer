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
#include <thread>
#include <unordered_map>
#include <vector>

// doc 16 tier-6, tsan lane; refinement Acceptance "Concurrency / TSan" -- the new
// cross-thread surface the transport reserved (`transport.md:338-339`). A REAL fake
// device thread bumps the delivered-frame counter through the monitor's RT fill
// callback while the monitor's single owner thread masters the transport (advancing
// it + republishing the lock-free `Time` snapshot) and both the lookahead pump and a
// simulated video viewport read that snapshot concurrently -- "video chases audio".
// It asserts: no data race on the published snapshot / delivered counter / direction
// (the TSan lane flags any); the `Transport` is mutated on the single owner thread
// only (the RT device thread never touches it); each AudioCompletion settles exactly
// once (pool tasks_completed == tasks_submitted); and the ring/cache survive the race
// so a reprimed drain is byte-identical to the single-threaded inline goldens.
// Cross-component, so it lives in top-level `tests/` (not level-checked) and links
// the umbrella `arbc`. Local Content / PullService doubles keep it deterministic.

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

// A fake device sink whose `start` spawns a REAL device thread -- the RT-thread
// analog. It invokes the monitor's fill callback in a tight loop (bumping the
// delivered-frame counter) until quiesced. `stop()` is idempotent and, per the
// `DeviceSink` contract, guarantees the callback is not invoked after it returns.
class ThreadedDeviceSink final : public DeviceSink {
public:
  explicit ThreadedDeviceSink(DeviceFormat fmt) : d_format(fmt) {}
  ~ThreadedDeviceSink() override { stop(); }

  DeviceFormat format() const override { return d_format; }

  void start(DeviceFillCallback fill) override {
    d_fill = std::move(fill);
    d_stop.store(false, std::memory_order_release);
    d_thread = std::thread([this] {
      std::vector<float> buf(static_cast<std::size_t>(k_block_frames) * channel_count(d_format.layout),
                             0.0F);
      while (!d_stop.load(std::memory_order_acquire)) {
        d_fill(buf.data(), k_block_frames);
        std::this_thread::yield();
      }
    });
  }

  void stop() override {
    d_stop.store(true, std::memory_order_release);
    if (d_thread.joinable()) {
      d_thread.join();
    }
  }

private:
  DeviceFormat d_format;
  DeviceFillCallback d_fill;
  std::atomic<bool> d_stop{false};
  std::thread d_thread;
};

} // namespace

// enforces: 12-audio#device-clock-masters-transport
// enforces: 12-audio#device-callback-consumes-prepared-blocks-only
TEST_CASE("TSan: mastered-clock publish races the pump + a viewport; goldens survive the race") {
  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(k_rate);
  const std::int64_t span = static_cast<std::int64_t>(k_block_frames) * fpf;

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
  poolcfg.worker_count = 8; // multiple workers race the master + drain
  AudioWorkerPool pool(poolcfg);

  Transport transport{Time::zero()};
  std::atomic<DeviceMonitor*> monptr{nullptr};

  std::atomic<std::uint64_t> fake_tick{0};
  LookaheadPumpConfig pumpcfg;
  pumpcfg.horizon = Time{(k_blocks - 1) * span};
  pumpcfg.resolve = scene.resolver();
  pumpcfg.sample_rate = k_rate;
  pumpcfg.layout = ChannelLayout::Stereo;
  pumpcfg.block_frames = k_block_frames;
  pumpcfg.tick_period = std::chrono::milliseconds(1);
  pumpcfg.tick_source = [&fake_tick] { return fake_tick.fetch_add(1, std::memory_order_relaxed); };
  // Video chases audio: the pump samples the mastered snapshot concurrently.
  pumpcfg.playhead_source = [&monptr] {
    DeviceMonitor* m = monptr.load(std::memory_order_acquire);
    return m != nullptr ? m->playhead_snapshot() : Time::zero();
  };
  pumpcfg.direction_source = [&monptr] {
    DeviceMonitor* m = monptr.load(std::memory_order_acquire);
    return m != nullptr ? m->direction_snapshot() : 1;
  };
  LookaheadPump pump(ring, blocks, pool, pumpcfg);

  ThreadedDeviceSink sink{DeviceFormat{k_rate, ChannelLayout::Stereo}};
  DeviceMonitorConfig moncfg;
  moncfg.working_rate = k_rate;
  moncfg.working_layout = ChannelLayout::Stereo;
  moncfg.block_frames = k_block_frames;
  moncfg.master_period = std::chrono::milliseconds(1); // owner thread masters continuously
  DeviceMonitor monitor(transport, pump, sink, moncfg);
  monptr.store(&monitor, std::memory_order_release);

  // A simulated video viewport chasing the audio clock: it reads the lock-free
  // snapshot + direction concurrently with the owner thread's republish.
  std::atomic<bool> reader_stop{false};
  std::atomic<std::uint64_t> reads{0};
  std::thread viewport([&] {
    while (!reader_stop.load(std::memory_order_acquire)) {
      const Time seen = monitor.playhead_snapshot();
      (void)monitor.direction_snapshot();
      (void)seen;
      reads.fetch_add(1, std::memory_order_relaxed);
      std::this_thread::yield();
    }
  });

  // Drive the mastering concurrently: repeatedly force a master step (each advances
  // the transport from the delivered-frame delta and republishes the snapshot) while
  // the device thread delivers frames and the viewport reads the snapshot -- the
  // genuine three-way race on the published surface. Bounded by delivered frames, no
  // wall clock in the assertion path.
  const std::uint64_t target = static_cast<std::uint64_t>(k_blocks) * k_block_frames * 16;
  while (monitor.delivered_frames() < target) {
    monitor.flush_master(); // forces one owner-thread mastering step, races the device
  }
  monitor.flush_master(); // one more to master the final delivered frames

  // Quiesce the RT device thread (the DeviceSink contract: fill not invoked after),
  // then stop the viewport reader so the assertions run on a settled state.
  sink.stop();
  reader_stop.store(true, std::memory_order_release);
  viewport.join();

  // The device delivered concurrently and the single owner thread mastered the
  // transport forward from it (video chased a genuinely advancing clock).
  REQUIRE(monitor.delivered_frames() >= target);
  REQUIRE(reads.load(std::memory_order_relaxed) > 0);
  REQUIRE(monitor.master_steps() > 0);
  REQUIRE(transport.position() != Time::zero());

  // The ring + BlockCache survived the concurrent mastering race: reprime around 0
  // and drain blocks 0..k_blocks-1 -- byte-identical to the single-threaded oracle.
  monitor.seek(Time::zero());
  monitor.flush_master();
  pump.flush();
  pump.flush();
  for (std::int64_t i = 0; i < k_blocks; ++i) {
    std::vector<float> got(static_cast<std::size_t>(k_block_frames) * 2, 0.0F);
    AudioBlock out{got.data(), k_block_frames, ChannelLayout::Stereo, k_rate};
    AudioResult meta{};
    REQUIRE(pump.drain(i, out, meta));
    REQUIRE(bytes_equal(got, direct_mix(*doc, scene.comp, scene.resolver(), i)));
  }

  // Stop the pump (no more submits), await pool quiescence (bounded: every submitted
  // fill completes), then settle-once: each dispatched fill settled exactly once, so
  // completed == submitted with none lost or double-run.
  pump.request_stop();
  while (pool.tasks_completed() < pool.tasks_submitted()) {
    std::this_thread::yield();
  }
  REQUIRE(pool.tasks_submitted() > 0);
  REQUIRE(pool.tasks_completed() == pool.tasks_submitted());
}
