#pragma once

#include <arbc/audio_engine/lookahead.hpp>    // LookaheadRing, PrefetchWant
#include <arbc/base/time.hpp>                 // Time, TimeRange
#include <arbc/compositor/pull_service.hpp>   // BlockCache, AudioBlockValue
#include <arbc/media/audio_block.hpp>         // AudioBlock, ChannelLayout
#include <arbc/runtime/audio_worker_pool.hpp> // AudioWorkerPool

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace arbc {

// The headless lookahead pump for `arbc::runtime` (L5, doc 17:60,84-86 "monitor
// objects / frame loop / drivers"): the owned background thread that turns the pure
// L4 `LookaheadRing` into a running, deadline-free pipeline -- "audio renders
// ahead," fully testable with a fake clock and no audio device (Constraint 2).
//
// It follows the `HousekeepingThread` template exactly (the engine's proven owned
// background thread, `housekeeping_thread.md`): it parks on a condvar with a
// timeout, wakes on the timeout OR an explicit poke, drives one tick under a mutex,
// and exposes a `flush()` barrier that synchronizes on the tick counter, never on a
// wall clock. Its clock is an INJECTABLE monotonic `tick_source` (a fake clock in
// tests); its playhead is sampled from an injected `playhead_source` (a `Transport`
// in production) -- it reads the transport, it does not master it (device-clock
// mastering is `audio.device_monitor`).
//
// Each tick: sample the playhead + direction, apply pending damage (`invalidate`),
// call the ring's `prime` (or `reprime` after a transport change) to enumerate the
// per-content fill want-list and mix every already-warm output block, dispatch the
// absent contributor blocks to the audio worker pool (so ALL `render_audio` plugin
// code runs OFF this thread and the consumer thread), insert the rendered blocks
// into the single-thread-confined `BlockCache`, and re-`prime` so the now-warm
// blocks mix with zero dispatch. A consumer calls `drain` to consume a prepared,
// already-mixed block -- never mixing inline.
//
// The pump REFERENCES (does not own) the `LookaheadRing`, the `BlockCache`, and the
// `AudioWorkerPool`, so the caller wires `PullConfig::audio_dispatch` to that same
// pool's `submit` and `PullConfig::blocks` to that same cache (closing the two
// mix-engine pull gaps) before constructing the pump.
struct LookaheadPumpConfig {
  Time horizon{};      // lookahead budget (doc 12:157, e.g. 100-500ms)
  MixResolver resolve; // ObjectId -> Content* (fill target lookup)
  std::uint32_t sample_rate{0};
  ChannelLayout layout{ChannelLayout::Stereo};
  std::uint32_t block_frames{0};
  // Idle park interval between automatic wake-ups; never appears in a test assertion.
  std::chrono::steady_clock::duration tick_period{std::chrono::milliseconds(5)};
  // Injectable monotonic tick provider (empty -> a default steady_clock counter);
  // present only so no test reads a wall clock, exactly as `HousekeepingThread`.
  std::function<std::uint64_t()> tick_source;
  // Samples the transport playhead as an immutable `Time` (no clock, no mastering).
  std::function<Time()> playhead_source;
  // Samples the playback direction (sign of `Transport::rate()`); empty -> forward.
  std::function<int()> direction_source;
};

class LookaheadPump {
public:
  LookaheadPump(LookaheadRing& ring, BlockCache& blocks, AudioWorkerPool& pool,
                LookaheadPumpConfig config);
  ~LookaheadPump();

  LookaheadPump(const LookaheadPump&) = delete;
  LookaheadPump& operator=(const LookaheadPump&) = delete;

  // Consume prepared block `index` (already mixed on a worker, never on this call).
  // Delegates to `LookaheadRing::drain`, which reads the prepared-block ring
  // LOCK-FREE via a per-slot seqlock (audio.rt_safety, Decision D2); returns true +
  // copies the mixed samples if ready, else silence + an underrun. It NEVER mixes,
  // allocates, or takes a lock -- `ARBC_RT_NONBLOCKING` puts the whole RT callback
  // chain from here down under RealtimeSanitizer.
  bool drain(std::int64_t index, AudioBlock& out, AudioResult& meta) ARBC_RT_NONBLOCKING;

  // Flag a transport change (`seek`/`set_rate`/direction): the next tick calls the
  // ring's `reprime` from the freshly-sampled playhead (flush + re-enumerate).
  void notify_transport_change() noexcept;

  // Queue a local-time damage range: the next tick calls the ring's `invalidate`.
  void notify_damage(TimeRange range);

  // Wake the loop for an immediate tick (non-blocking).
  void poke() noexcept;

  // Poke and BLOCK until the loop completes one further tick, returning the new
  // tick count. Waits on the tick counter (a condition), never a wall clock.
  std::uint64_t flush();

  // Stop the loop and BLOCK until it has quiesced (the loop thread is joined). Once this
  // returns, no further `d_pool.submit` can occur, so a caller may drain the pool to a
  // settle-once equilibrium (completed == submitted) without a late fill racing in.
  // Idempotent and safe to call before the dtor.
  void request_stop() noexcept;
  std::uint64_t ticks() const noexcept { return d_ticks.load(std::memory_order_acquire); }

private:
  void run();
  void tick_once();
  void fill_and_insert(const std::vector<PrefetchWant>& wants);

  LookaheadRing& d_ring;
  BlockCache& d_blocks;
  AudioWorkerPool& d_pool;
  LookaheadPumpConfig d_config;

  mutable std::mutex d_mutex;            // guards the ring, the cache, and the flags
  std::condition_variable d_wake_cv;     // parks the loop (poke / stop / timeout)
  std::condition_variable d_progress_cv; // wakes a blocked flush()
  bool d_stop{false};
  bool d_poke{false};
  bool d_transport_changed{false};
  std::vector<TimeRange> d_pending_damage;
  std::atomic<std::uint64_t> d_ticks{0};

  std::thread d_thread; // started last in the ctor body
};

} // namespace arbc
