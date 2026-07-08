#pragma once

#include <arbc/contract/content.hpp> // Content, AudioRequest, AudioCompletion (L5->contract)

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

namespace arbc {

// The audio sibling of `WorkerPool` (`worker_pool.hpp:78`): the `AudioRequest` /
// `AudioCompletion` analog that runs arbitrary `render_audio` plugin code OFF the
// consumer/callback thread -- the load-bearing promise of the lookahead note
// ("workers run all plugin code", doc 12:31-34,155-164). `threading.md` already
// anticipated "reuse this same `WorkerPool` or an analogous one against
// `AudioRequest`"; per the refinement's Decision this is a SIBLING, not a
// templatization of `WorkerPool` -- `WorkerPool` carries `RenderTask`
// (`RenderRequest`/`RenderCompletion`) only, so duplicating the thin shared-FIFO +
// per-content-gate + condvar-wake shell keeps the TSan-verified render pool
// byte-identical and untouched.
//
// Same deliberate non-responsibilities as `WorkerPool`: clock-free (audio never
// carries a deadline at all, doc 12:33-34); workers never touch the `BlockCache`
// (they render into caller-owned `AudioBlock` targets and settle caller-owned
// completions -- the pump inserts into the single-thread-confined `KeyedStore`);
// and the `PullConfig::audio_dispatch` seam is injected downward as a
// `std::function` over `submit`, so no L4->L5 edge.

// The unit of dispatched audio work. The `AudioRequest` (holding `AudioBlock&
// target`) and the `done` completion are CALLER-owned; the pool stores no buffer
// across the render and a submitted task must not outlive its backing target.
struct AudioTask {
  Content* content{nullptr};
  AudioRequest request;
  std::shared_ptr<AudioCompletion> done;
};

// Thread-lifecycle knobs, all defaulted (mirrors `WorkerPoolConfig`).
struct AudioWorkerPoolConfig {
  // `0` selects the DEGENERATE inline executor: `submit` runs the render on the
  // calling thread and returns, byte-identical to a single-threaded monitor.
  std::size_t worker_count = 0;
  // The serialization signal; a content returning `render_thread_safe() == false`
  // renders one-at-a-time through its per-content queue. Injectable for tests.
  std::function<bool(const Content*)> serialize_predicate = [](const Content* c) {
    return c != nullptr && !c->render_thread_safe();
  };
};

class AudioWorkerPool {
public:
  explicit AudioWorkerPool(AudioWorkerPoolConfig config);
  ~AudioWorkerPool();

  AudioWorkerPool(const AudioWorkerPool&) = delete;
  AudioWorkerPool& operator=(const AudioWorkerPool&) = delete;
  AudioWorkerPool(AudioWorkerPool&&) = delete;
  AudioWorkerPool& operator=(AudioWorkerPool&&) = delete;

  // Enqueue an audio render. Thread-safe content goes on the shared queue for any
  // worker; serialized content with an in-flight render is parked on its per-content
  // FIFO and released when the in-flight one settles, guaranteeing at most one
  // concurrent `render_audio` per content. In inline mode it runs immediately.
  // Post-stop `submit` is a no-op leaving the completion unsettled.
  void submit(AudioTask task);

  // The async-completion wake handle: any settler wakes a thread parked in
  // `wait_completions`. Bumps the settle generation and notifies; never blocks.
  void poke() noexcept;

  // Block until at least one completion has settled since the last drain, or `until`
  // elapses (nullopt = no timeout); returns whether a completion is ready. Waits on
  // a CONDITION (a settle-count advance), never a fixed sleep.
  bool wait_completions(std::optional<std::chrono::steady_clock::time_point> until);

  // Idempotent: sets the stop flag and wakes all workers (join is in the dtor).
  void request_stop() noexcept;

  // Behavioral counters (doc 16:54-62), wall-clock-free.
  std::uint64_t tasks_submitted() const noexcept {
    return d_submitted.load(std::memory_order_acquire);
  }
  std::uint64_t tasks_completed() const noexcept {
    return d_completed.load(std::memory_order_acquire);
  }
  std::uint64_t max_in_flight_per_content() const noexcept {
    return d_max_in_flight.load(std::memory_order_acquire);
  }

private:
  struct SerialState {
    bool active = false;
    int rendering = 0;
    std::deque<AudioTask> parked;
  };

  void run();
  void run_task(AudioTask task, bool serialize);
  void submit_inline(AudioTask task);

  AudioWorkerPoolConfig d_config;

  mutable std::mutex d_mutex;
  std::condition_variable d_work_cv;
  std::condition_variable d_completion_cv;
  std::deque<AudioTask> d_ready;
  std::unordered_map<const Content*, SerialState> d_serial;
  bool d_stop = false;

  std::uint64_t d_settle_gen = 0;
  std::uint64_t d_drained_gen = 0;

  std::atomic<std::uint64_t> d_submitted{0};
  std::atomic<std::uint64_t> d_completed{0};
  std::atomic<std::uint64_t> d_max_in_flight{0};

  std::vector<std::thread> d_workers; // started last in the ctor body
};

} // namespace arbc
