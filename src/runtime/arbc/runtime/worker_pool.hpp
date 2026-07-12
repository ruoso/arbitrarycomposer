#pragma once

#include <arbc/contract/content.hpp> // Content, RenderRequest, RenderCompletion (L5->contract, doc 17:60)

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

// The concurrency substrate for layer rendering (doc 02:118-137, doc 17:60,
// `runtime.threading`). It turns "call `content->render` inline on the render
// thread" into "dispatch the render to a worker, serialize the ones that must
// not run concurrently, and wake the render thread when a result settles."
//
// This is the runtime-owned mechanism the contract deliberately left as policy:
// `content.hpp:117-118` defers the completion *wake* ("condvar/eventfd") to the
// runtime, and doc 02:126-130 promises the core provides a per-content queue for
// content that declares `render_thread_safe() == false`. The pool supplies both,
// mirroring the proven park/wake/stop/join lifecycle of `HousekeepingThread`
// (the engine's first owned background thread) generalized from one thread to N.
//
// Deliberate non-responsibilities (see the refinement's Decisions):
//   * The pool is CLOCK-FREE. It carries each task's `Deadline` value untouched
//     and reads no clock; deadline enforcement (cancelling expired `BestEffort`
//     work) is `runtime.interactive` (doc 17:78-80, `content.hpp:50-55`).
//   * Workers never touch the cache. They render only into caller-owned target
//     surfaces and settle caller-owned completions; cache inserts stay on the
//     render thread via the compositor's `poll_refinements`. This keeps
//     `KeyedStore` single-thread-confined (parking-lot.md:38-41 untriggered).
//   * The `PullService` dispatch seam is injected downward at wiring time (a
//     `std::function` over `submit`), so no L4->L5 edge is introduced.

// The unit of dispatched work. The `RenderRequest` (holding `Surface& target`)
// and the `done` completion are CALLER-owned -- the compositor's `PendingTile`
// owns the surface and the completion (`refinement.hpp:65-79`); the task merely
// references them and the pool stores no surface across the render. A submitted
// task must not outlive its backing `PendingTile`; the frame loop drains the
// pool before dropping pending surfaces.
struct RenderTask {
  Content* content{nullptr};
  RenderRequest request;
  std::shared_ptr<RenderCompletion> done;
};

// Thread-lifecycle knobs, all defaulted.
struct WorkerPoolConfig {
  // Number of worker threads. `0` selects the DEGENERATE inline executor (doc
  // 02:135-137): `submit` runs the render on the calling thread and returns,
  // preserving the request/completion structure with no thread. Default `0`, so
  // the walking-skeleton path is byte-identical to `render_frame_interactive`'s
  // inline fill until a caller opts into real threads.
  std::size_t worker_count = 0;

  // The serialization signal. Default reads the contract declaration
  // `Content::render_thread_safe()` (doc 02:126-130): a content that returns
  // false renders one-at-a-time through its per-content queue. Injectable so a
  // test can drive the gate with a stub content without a real non-thread-safe
  // kind. The *source of truth* is the contract method; this is the routing hook.
  std::function<bool(const Content*)> serialize_predicate = [](const Content* c) {
    return c != nullptr && !c->render_thread_safe();
  };
};

// A pool of worker threads with a shared FIFO work queue, a per-content
// serialization gate, and a condition-variable completion wake. Non-copyable and
// non-movable: it owns threads, a mutex, and condition variables.
class WorkerPool {
public:
  explicit WorkerPool(WorkerPoolConfig config);
  ~WorkerPool();

  WorkerPool(const WorkerPool&) = delete;
  WorkerPool& operator=(const WorkerPool&) = delete;
  WorkerPool(WorkerPool&&) = delete;
  WorkerPool& operator=(WorkerPool&&) = delete;

  // Enqueue a render. Thread-safe content goes on the shared queue for any
  // worker; serialized content (per `serialize_predicate`) that already has an
  // in-flight render is parked on its per-content FIFO and released when the
  // in-flight one settles, guaranteeing at most one concurrent `render` call for
  // that content. In inline mode (`worker_count == 0`) it runs immediately on
  // the calling thread, still honoring the per-content gate. Post-stop `submit`
  // is a no-op: it does not enqueue and leaves the completion unsettled (never
  // lost to UB).
  void submit(RenderTask task);

  // The async-completion wake handle (`content.hpp:117-118`): any settler -- a
  // worker after an inline-in-worker settle, or externally-async content on its
  // own thread -- calls this to wake a render thread parked in
  // `wait_completions`. Bumps the settle generation and notifies; never blocks.
  void poke() noexcept;

  // Block the calling (render) thread until at least one dispatched completion
  // has settled since the last drain, or `until` elapses (nullopt = no timeout);
  // returns whether a completion is ready. Waits on a CONDITION (a settle-count
  // advance), never a fixed sleep. The frame loop parks here between `submit`-ing
  // misses and calling `poll_refinements`. The optional timeout is a park bound,
  // never a deadline decision.
  bool wait_completions(std::optional<std::chrono::steady_clock::time_point> until);

  // Idempotent: sets the stop flag and wakes all workers (join is in the dtor).
  void request_stop() noexcept;

  // The configured worker count -- `0` is the degenerate inline executor. Exposed so a
  // caller (and `runtime.interactive_worker_count_default`'s unit test) can observe the
  // pool a driver actually built, without re-deriving it from the config it passed: the
  // interactive driver's default config is a MACHINE-DEPENDENT policy
  // (`default_interactive_worker_count()`), so the only honest assertion about it is a
  // property of what the pool reports, never a literal.
  std::size_t worker_count() const noexcept { return d_config.worker_count; }

  // Behavioral counters (doc 16:54-62), caller-readable and wall-clock-free.
  std::uint64_t tasks_submitted() const noexcept {
    return d_submitted.load(std::memory_order_acquire);
  }
  std::uint64_t tasks_completed() const noexcept {
    return d_completed.load(std::memory_order_acquire);
  }
  // The high-water mark of concurrent `render` calls for any single serialized
  // content -- the observable that pins "<= 1" for serialized content. Measured
  // at the render call site independently of the admission gate, so a broken
  // gate would surface as a value > 1.
  std::uint64_t max_in_flight_per_content() const noexcept {
    return d_max_in_flight.load(std::memory_order_acquire);
  }

private:
  // Per serialized content: `active` (a task is admitted+outstanding, gating
  // dispatch from admission until the render call returns) and a parked FIFO.
  // `rendering` is the actual concurrent-render count observed at the call site.
  struct SerialState {
    bool active = false;
    int rendering = 0;
    std::deque<RenderTask> parked;
  };

  void run();                                     // worker loop (started per thread)
  void run_task(RenderTask task, bool serialize); // render + settle + counters
  void submit_inline(RenderTask task);            // worker_count == 0 executor

  WorkerPoolConfig d_config;

  mutable std::mutex d_mutex;              // guards the queues + serialization map + gens
  std::condition_variable d_work_cv;       // parks idle workers
  std::condition_variable d_completion_cv; // parks a render thread in wait_completions
  std::deque<RenderTask> d_ready;          // shared FIFO of runnable tasks
  std::unordered_map<const Content*, SerialState> d_serial; // per-content gate
  bool d_stop = false;

  // The completion-wake condition: `poke()` bumps `d_settle_gen`;
  // `wait_completions` returns when it advances past `d_drained_gen`.
  std::uint64_t d_settle_gen = 0;
  std::uint64_t d_drained_gen = 0;

  std::atomic<std::uint64_t> d_submitted{0};
  std::atomic<std::uint64_t> d_completed{0};
  std::atomic<std::uint64_t> d_max_in_flight{0};

  std::vector<std::thread> d_workers; // started last in the ctor body
};

} // namespace arbc
