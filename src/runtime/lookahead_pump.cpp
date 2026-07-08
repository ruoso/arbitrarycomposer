#include <arbc/base/time.hpp>
#include <arbc/contract/content.hpp> // AudioRequest, AudioCompletion, Exactness
#include <arbc/runtime/lookahead_pump.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <utility>

namespace arbc {

LookaheadPump::LookaheadPump(LookaheadRing& ring, BlockCache& blocks, AudioWorkerPool& pool,
                             LookaheadPumpConfig config)
    : d_ring(ring), d_blocks(blocks), d_pool(pool), d_config(std::move(config)) {
  if (!d_config.tick_source) {
    const auto start = std::chrono::steady_clock::now();
    const std::chrono::steady_clock::duration period = d_config.tick_period;
    d_config.tick_source = [start, period]() -> std::uint64_t {
      if (period.count() <= 0) {
        return 0;
      }
      return static_cast<std::uint64_t>((std::chrono::steady_clock::now() - start) / period);
    };
  }
  d_thread = std::thread(&LookaheadPump::run, this); // launched last
}

LookaheadPump::~LookaheadPump() {
  request_stop();
  if (d_thread.joinable()) {
    d_thread.join();
  }
}

bool LookaheadPump::drain(std::int64_t index, AudioBlock& out, AudioResult& meta) {
  std::lock_guard<std::mutex> lock(d_mutex);
  // Serialized with the tick's ring mutation; `LookaheadRing::drain` itself never
  // mixes / allocates / blocks (the RT-safety invariant). A lock-free double-buffer
  // for a true RT device thread is `audio.device_monitor`'s concern.
  return d_ring.drain(index, out, meta);
}

void LookaheadPump::notify_transport_change() noexcept {
  {
    std::lock_guard<std::mutex> lock(d_mutex);
    d_transport_changed = true;
    d_poke = true;
  }
  d_wake_cv.notify_one();
}

void LookaheadPump::notify_damage(TimeRange range) {
  {
    std::lock_guard<std::mutex> lock(d_mutex);
    d_pending_damage.push_back(range);
    d_poke = true;
  }
  d_wake_cv.notify_one();
}

void LookaheadPump::poke() noexcept {
  {
    std::lock_guard<std::mutex> lock(d_mutex);
    d_poke = true;
  }
  d_wake_cv.notify_one();
}

std::uint64_t LookaheadPump::flush() {
  std::unique_lock<std::mutex> lock(d_mutex);
  const std::uint64_t target = d_ticks.load(std::memory_order_relaxed) + 1;
  d_poke = true;
  d_wake_cv.notify_one();
  d_progress_cv.wait(
      lock, [this, target] { return d_stop || d_ticks.load(std::memory_order_acquire) >= target; });
  return d_ticks.load(std::memory_order_relaxed);
}

void LookaheadPump::request_stop() noexcept {
  {
    std::lock_guard<std::mutex> lock(d_mutex);
    d_stop = true;
  }
  d_wake_cv.notify_one();
  d_progress_cv.notify_all(); // wake a flush() waiting for a tick that will never come
}

void LookaheadPump::fill_and_insert(const std::vector<PrefetchWant>& wants) {
  // Render every absent contributor block on the audio worker pool -- ALL
  // `render_audio` plugin code runs here, OFF the pump/consumer thread (doc
  // 12:31-34,155-164). Each renders into a pump-owned staging buffer; the pump (this
  // thread) inserts into the single-thread-confined `BlockCache`, so no worker ever
  // touches the `KeyedStore` (WorkerPool's cache discipline).
  const std::size_t count = wants.size();
  std::vector<std::vector<float>> bufs(count);
  std::vector<AudioBlock> targets(count);
  std::vector<std::shared_ptr<AudioCompletion>> dones(count);
  for (std::size_t i = 0; i < count; ++i) {
    const PrefetchWant& w = wants[i];
    Content* content = d_config.resolve ? d_config.resolve(w.content) : nullptr;
    bufs[i].assign(static_cast<std::size_t>(w.frames) * channel_count(w.layout), 0.0F);
    targets[i] = AudioBlock{bufs[i].data(), w.frames, w.layout, w.rate};
    dones[i] = std::make_shared<AudioCompletion>();
    const AudioRequest req{w.window,     w.rate, w.layout, targets[i], Exactness::BestEffort,
                           StateHandle{}};
    d_pool.submit(AudioTask{content, req, dones[i]});
  }

  // Wait until every fill has settled (inline mode settles synchronously; threaded
  // mode parks on the completion condition, never a fixed sleep). Done OFF the ring
  // lock so a concurrent `drain` stays responsive.
  for (;;) {
    bool all_settled = true;
    for (const std::shared_ptr<AudioCompletion>& done : dones) {
      if (!done->settled()) {
        all_settled = false;
        break;
      }
    }
    if (all_settled) {
      break;
    }
    d_pool.wait_completions(std::nullopt);
  }

  std::lock_guard<std::mutex> lock(d_mutex);
  for (std::size_t i = 0; i < count; ++i) {
    const PrefetchWant& w = wants[i];
    const std::optional<expected<AudioResult, RenderError>> settled = dones[i]->take();
    const AudioResult meta =
        (settled.has_value() && settled->has_value()) ? **settled : AudioResult{w.rate, false};
    const std::size_t bytes = bufs[i].size() * sizeof(float);
    d_blocks.insert(w.key, AudioBlockValue{std::move(bufs[i]), w.frames, w.layout, w.rate, meta},
                    bytes, PriorityClass::Temporal);
  }
}

void LookaheadPump::tick_once() {
  Time playhead;
  int direction = 1;
  bool transport_changed = false;
  std::vector<TimeRange> damage;
  {
    std::lock_guard<std::mutex> lock(d_mutex);
    playhead = d_config.playhead_source ? d_config.playhead_source() : Time::zero();
    direction = d_config.direction_source ? d_config.direction_source() : 1;
    transport_changed = d_transport_changed;
    d_transport_changed = false;
    damage.swap(d_pending_damage);
  }

  bool first = true;
  // Prime, fill, re-prime until the horizon is fully mixed (no absent contributor
  // remains). Bounded so a persistently-unrenderable contributor cannot spin.
  for (int round = 0; round < 8; ++round) {
    std::vector<PrefetchWant> wants;
    {
      std::lock_guard<std::mutex> lock(d_mutex);
      if (first) {
        for (const TimeRange& range : damage) {
          d_ring.invalidate(&d_blocks, range);
        }
      }
      wants = (first && transport_changed)
                  ? d_ring.reprime(&d_blocks, playhead, d_config.horizon, direction)
                  : d_ring.prime(&d_blocks, playhead, d_config.horizon, direction);
    }
    first = false;
    if (wants.empty()) {
      break; // horizon fully warm and mixed
    }
    fill_and_insert(wants);
  }
}

void LookaheadPump::run() {
  for (;;) {
    {
      std::unique_lock<std::mutex> lock(d_mutex);
      d_wake_cv.wait_for(lock, d_config.tick_period, [this] { return d_stop || d_poke; });
      if (d_stop) {
        break;
      }
      d_poke = false;
    }
    (void)d_config.tick_source(); // sample the monotonic clock (no wall-clock assertion)
    tick_once();
    {
      // Publish the tick under the mutex so a `flush()` cannot miss it between its
      // predicate check and its wait (mirrors HousekeepingThread).
      std::lock_guard<std::mutex> lock(d_mutex);
      d_ticks.fetch_add(1, std::memory_order_release);
    }
    d_progress_cv.notify_all();
  }
}

} // namespace arbc
