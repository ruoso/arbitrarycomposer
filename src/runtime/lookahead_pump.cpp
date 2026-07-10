#include <arbc/base/time.hpp>
#include <arbc/contract/content.hpp> // AudioRequest, AudioCompletion, Exactness
#include <arbc/runtime/lookahead_pump.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <thread>
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
  request_stop(); // joins the loop thread; the join below is then a no-op
  if (d_thread.joinable()) {
    d_thread.join();
  }
}

bool LookaheadPump::drain(std::int64_t index, AudioBlock& out,
                          AudioResult& meta) noexcept ARBC_RT_NONBLOCKING {
  // Lock-free (audio.rt_safety, Decision D2): `LookaheadRing::drain` reads the
  // target slot through a per-slot seqlock, so the RT consumer takes NO mutex and
  // runs concurrently with the pump tick's ring mutation. The `d_mutex` that used
  // to guard this call was the one blocking op left on the device RT callback chain
  // (the reserved double-buffer, device_monitor.hpp:34-36); it is gone now, not
  // annotated around a still-blocking op (Constraint 1). It never mixes/allocates.
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

void LookaheadPump::set_spatial(std::optional<Spatialization> spatial) {
  {
    std::lock_guard<std::mutex> lock(d_mutex);
    d_pending_spatial = std::move(spatial);
    d_spatial_pending = true;
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

  // Quiesce the loop before returning: once it has exited, no further `d_pool.submit`
  // can race in. This is what makes "no more submits after request_stop" a true
  // precondition for a settle-once drain (completed == submitted) -- otherwise a tick
  // already past its `d_stop` check could submit one last fill after the caller began
  // draining, leaving submitted transiently ahead with the loop gone. Guarded against a
  // self-join in case a tick-thread callback ever calls this (join-from-self would throw
  // through this noexcept). Idempotent: the dtor's own request_stop then no-ops.
  if (d_thread.joinable() && d_thread.get_id() != std::this_thread::get_id()) {
    d_thread.join();
  }
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
    // Thread the per-edge Spatial context the ring reconstructed onto the request
    // (audio.spatial_nested_warm_context, Decision D1/D3): a spatial-context-consuming
    // contributor (a nested composition) is then rendered under the identical context
    // the mixer's `mix_layer` pulls it with (`mix.cpp:113-122`), so the warmed block is
    // Spatial, not Flat, and the threaded drain is byte-identical to the inline oracle.
    // `w.spatial` is nullopt for Flat / leaf-only scenes => a byte-exact Flat request.
    const AudioRequest req{w.window,      w.rate,   w.layout, targets[i], Exactness::BestEffort,
                           StateHandle{}, w.spatial};
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
    // Apply a staged live-camera listener re-seed before this tick's reprime
    // (audio.spatial_camera_follow, Decision D5). Done under `d_mutex`, which
    // serializes with the ring's `prime`/`reprime`/`drain`, so the seed swap and the
    // ring-slot flush it triggers never race a fill. The master thread stages this
    // before it calls `notify_transport_change`, so `transport_changed` is set on the
    // same tick and the reprime below re-warms under the new listener.
    if (d_spatial_pending) {
      d_ring.set_spatial(std::move(d_pending_spatial));
      d_pending_spatial.reset();
      d_spatial_pending = false;
    }
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
