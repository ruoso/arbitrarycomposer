#include <arbc/base/time.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/media/audio_block.hpp>
#include <arbc/runtime/audio_worker_pool.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

// Unit + stress tests for the runtime audio worker pool (doc 12:31-34,155-164, doc
// 16:46,54-73): the audio twin of `worker_pool.t.cpp`. Every assertion is on a
// behavioral counter, an `AudioCompletion` settlement, or a pool counter;
// synchronization is via `wait_completions` (a completion-count condition) and
// atomic flags -- no test reads a wall clock to synchronize (the one
// `steady_clock::now()` use is a park bound asserted only via its return value).

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

// A stub audio content: its facet fills the target with a deterministic parabolic
// sine (a pure function of the request) and settles INLINE, recording the thread it
// ran on and a per-content concurrency high-water. `render_thread_safe()` is
// configurable so a test drives the serialization gate.
class AudioStub final : public Content {
public:
  explicit AudioStub(bool thread_safe) : d_thread_safe(thread_safe) {}
  std::optional<Rect> bounds() const override { return std::nullopt; }
  Stability stability() const override { return Stability::Static; }
  std::optional<TimeRange> time_extent() const override { return std::nullopt; }
  bool render_thread_safe() const override { return d_thread_safe; }
  std::optional<RenderResult> render(const RenderRequest&,
                                     std::shared_ptr<RenderCompletion> done) override {
    done->fail(RenderError::ContentFailed);
    return std::nullopt;
  }
  AudioFacet* audio() override { return &d_facet; }

  int max_in_flight() const { return d_facet.max_in_flight(); }
  std::uint64_t render_calls() const { return d_facet.render_calls(); }
  std::thread::id last_thread() const { return d_facet.last_thread(); }

private:
  class Facet final : public AudioFacet {
  public:
    std::optional<TimeRange> audio_extent() const override { return std::nullopt; }
    Stability audio_stability() const override { return Stability::Static; }
    std::optional<AudioResult> render_audio(const AudioRequest& request,
                                            std::shared_ptr<AudioCompletion>) override {
      const int now = d_in_flight.fetch_add(1, std::memory_order_acq_rel) + 1;
      int prev = d_max_in_flight.load(std::memory_order_relaxed);
      while (now > prev && !d_max_in_flight.compare_exchange_weak(prev, now)) {
      }
      d_render_calls.fetch_add(1, std::memory_order_acq_rel);
      d_last_thread.store(std::this_thread::get_id(), std::memory_order_release);
      std::this_thread::yield();

      const std::uint32_t ch = channel_count(request.layout);
      const std::int64_t fpf =
          Time::flicks_per_second / static_cast<std::int64_t>(request.sample_rate);
      for (std::uint32_t f = 0; f < request.target.frames; ++f) {
        const float v =
            parab_sine(request.window.start.flicks + static_cast<std::int64_t>(f) * fpf, 440, 0.5F);
        for (std::uint32_t c = 0; c < ch; ++c) {
          request.target.samples[static_cast<std::size_t>(f) * ch + c] = v;
        }
      }
      d_in_flight.fetch_sub(1, std::memory_order_acq_rel);
      return AudioResult{request.sample_rate, true};
    }
    int max_in_flight() const { return d_max_in_flight.load(std::memory_order_acquire); }
    std::uint64_t render_calls() const { return d_render_calls.load(std::memory_order_acquire); }
    std::thread::id last_thread() const { return d_last_thread.load(std::memory_order_acquire); }

  private:
    std::atomic<int> d_in_flight{0};
    std::atomic<int> d_max_in_flight{0};
    std::atomic<std::uint64_t> d_render_calls{0};
    std::atomic<std::thread::id> d_last_thread{};
  };
  bool d_thread_safe;
  Facet d_facet;
};

constexpr std::uint32_t k_rate = 48'000;
constexpr std::uint32_t k_frames = 32;

struct Target {
  std::vector<float> buf;
  std::unique_ptr<AudioBlock> block; // stable address: AudioRequest holds AudioBlock&
  explicit Target(std::int64_t start_frame = 0)
      : buf(static_cast<std::size_t>(k_frames) * 2, 0.0F) {
    block = std::make_unique<AudioBlock>(AudioBlock{buf.data(), k_frames, ChannelLayout::Stereo,
                                                    k_rate});
    (void)start_frame;
  }
  AudioRequest request(std::int64_t start = 0) const {
    const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(k_rate);
    return AudioRequest{TimeRange{Time{start}, Time{start + static_cast<std::int64_t>(k_frames) * fpf}},
                        k_rate, ChannelLayout::Stereo, *block, Exactness::BestEffort, StateHandle{}};
  }
};

} // namespace

// enforces: 12-audio#lookahead-prepares-ahead-of-playhead
TEST_CASE("audio pool inline mode settles on the calling thread, byte-identical to direct render") {
  const std::thread::id test_tid = std::this_thread::get_id();
  AudioWorkerPool pool(AudioWorkerPoolConfig{}); // worker_count == 0

  AudioStub content(/*thread_safe=*/true);
  Target target;
  auto done = std::make_shared<AudioCompletion>();
  pool.submit(AudioTask{&content, target.request(), done});

  REQUIRE(done->settled());
  REQUIRE(content.last_thread() == test_tid); // ran on the calling thread
  REQUIRE(pool.tasks_submitted() == 1);
  REQUIRE(pool.tasks_completed() == 1);
  auto settled = done->take();
  REQUIRE(settled.has_value());
  REQUIRE(settled->has_value());
  REQUIRE_FALSE(done->take().has_value()); // settled exactly once

  // Byte-identical to calling render_audio directly.
  Target direct;
  AudioStub direct_content(true);
  auto d2 = std::make_shared<AudioCompletion>();
  (void)direct_content.audio()->render_audio(direct.request(), d2);
  REQUIRE(target.buf == direct.buf);
}

// enforces: 12-audio#lookahead-prepares-ahead-of-playhead
TEST_CASE("audio pool renders on workers off the consumer thread and wakes it") {
  const std::thread::id test_tid = std::this_thread::get_id();
  constexpr int k = 32;
  AudioWorkerPoolConfig cfg;
  cfg.worker_count = 4;
  AudioWorkerPool pool(cfg);

  AudioStub content(/*thread_safe=*/true);
  std::vector<Target> targets;
  targets.reserve(k);
  std::vector<std::shared_ptr<AudioCompletion>> dones;
  for (int i = 0; i < k; ++i) {
    targets.emplace_back();
    dones.push_back(std::make_shared<AudioCompletion>());
    pool.submit(AudioTask{&content, targets[i].request(), dones[i]});
  }

  int got = 0;
  std::vector<bool> taken(k, false);
  while (got < k) {
    pool.wait_completions(std::nullopt);
    for (int i = 0; i < k; ++i) {
      if (!taken[i] && dones[i]->take().has_value()) {
        taken[i] = true;
        ++got;
      }
    }
  }
  REQUIRE(pool.tasks_completed() == static_cast<std::uint64_t>(k));
  REQUIRE(content.render_calls() == static_cast<std::uint64_t>(k));
  REQUIRE(content.last_thread() != test_tid); // plugin code ran OFF the consumer thread
}

// enforces: 12-audio#lookahead-prepares-ahead-of-playhead
TEST_CASE("audio pool per-content serialization holds at most one in-flight render") {
  constexpr int total = 48;
  AudioWorkerPoolConfig cfg;
  cfg.worker_count = 8;
  AudioWorkerPool pool(cfg);

  AudioStub serialized(/*thread_safe=*/false);
  std::vector<Target> targets;
  targets.reserve(total);
  std::vector<std::shared_ptr<AudioCompletion>> dones;
  for (int i = 0; i < total; ++i) {
    targets.emplace_back();
    dones.push_back(std::make_shared<AudioCompletion>());
    pool.submit(AudioTask{&serialized, targets[i].request(), dones[i]});
  }
  int got = 0;
  std::vector<bool> taken(total, false);
  while (got < total) {
    pool.wait_completions(std::nullopt);
    for (int i = 0; i < total; ++i) {
      if (!taken[i] && dones[i]->take().has_value()) {
        taken[i] = true;
        ++got;
      }
    }
  }
  REQUIRE(pool.tasks_completed() == static_cast<std::uint64_t>(total));
  REQUIRE(pool.max_in_flight_per_content() == 1); // the serialization gate
  REQUIRE(serialized.max_in_flight() == 1);
}

// enforces: 12-audio#lookahead-prepares-ahead-of-playhead
TEST_CASE("audio pool stops gracefully: join without hang, post-stop submit refused") {
  SECTION("post-stop submit is refused") {
    AudioWorkerPoolConfig cfg;
    cfg.worker_count = 4;
    AudioWorkerPool pool(cfg);
    pool.request_stop();
    AudioStub late(true);
    Target target;
    auto done = std::make_shared<AudioCompletion>();
    pool.submit(AudioTask{&late, target.request(), done});
    REQUIRE(pool.tasks_submitted() == 0);
    REQUIRE_FALSE(done->settled());
  }
  SECTION("construct and immediately destroy an idle pool: join does not hang") {
    {
      AudioWorkerPoolConfig cfg;
      cfg.worker_count = 4;
      AudioWorkerPool pool(cfg);
    }
    REQUIRE(true);
  }
}
