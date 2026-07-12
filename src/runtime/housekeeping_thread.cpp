#include <arbc/runtime/housekeeping_thread.hpp>

#include <utility>

namespace arbc {

HousekeepingThread::HousekeepingThread(HousekeepingTarget& target, HousekeepingConfig policy,
                                       HousekeepingThreadConfig thread_config)
    : d_housekeeper(target, policy), d_config(std::move(thread_config)) {
  // Default tick source: a monotonic counter of elapsed park-periods since
  // construction. Production reads a real clock here (clocks are runtime policy,
  // doc 17:80; a timer cadence is sanctioned, doc 15:213); tests inject their own
  // deterministic source so no TEST reads a wall clock (doc 16:54-62).
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
  // Launch the loop last: every member it touches is now initialized.
  d_thread = std::thread(&HousekeepingThread::run, this);
}

HousekeepingThread::~HousekeepingThread() {
  request_stop();
  if (d_thread.joinable()) {
    d_thread.join();
  }
}

expected<std::monostate, WorkspaceFileError> HousekeepingThread::after_commit() {
  std::lock_guard<std::mutex> lock(d_mutex);
  return d_housekeeper.after_commit();
}

expected<std::monostate, WorkspaceFileError> HousekeepingThread::request_checkpoint() {
  std::lock_guard<std::mutex> lock(d_mutex);
  return d_housekeeper.request_checkpoint();
}

void HousekeepingThread::drain_and_quiesce() {
  std::lock_guard<std::mutex> lock(d_mutex);
  d_housekeeper.drain_and_quiesce();
}

void HousekeepingThread::poke() noexcept {
  {
    std::lock_guard<std::mutex> lock(d_mutex);
    d_poke = true;
  }
  d_wake_cv.notify_one();
}

std::uint64_t HousekeepingThread::flush() {
  std::unique_lock<std::mutex> lock(d_mutex);
  // Wait for one tick strictly after this call. Bail if the loop has stopped
  // (it will no longer advance the counter) so flush() never hangs at teardown.
  const std::uint64_t target = d_background_ticks.load(std::memory_order_relaxed) + 1;
  d_poke = true;
  d_wake_cv.notify_one();
  d_progress_cv.wait(lock, [this, target] {
    return d_stop || d_background_ticks.load(std::memory_order_acquire) >= target;
  });
  return d_background_ticks.load(std::memory_order_relaxed);
}

void HousekeepingThread::request_stop() noexcept {
  {
    std::lock_guard<std::mutex> lock(d_mutex);
    d_stop = true;
  }
  d_wake_cv.notify_one();
  // Wake a flush() that is waiting for a tick that will now never come.
  d_progress_cv.notify_all();
}

HousekeepingStats HousekeepingThread::stats() const {
  std::lock_guard<std::mutex> lock(d_mutex);
  return d_housekeeper.stats();
}

std::uint64_t HousekeepingThread::background_ticks() const noexcept {
  return d_background_ticks.load(std::memory_order_acquire);
}

std::optional<WorkspaceFileError> HousekeepingThread::last_checkpoint_error() const {
  std::lock_guard<std::mutex> lock(d_mutex);
  return d_last_checkpoint_error;
}

void HousekeepingThread::run() {
  std::unique_lock<std::mutex> lock(d_mutex);
  for (;;) {
    // Park until the period elapses OR a poke/stop arrives. A timeout return
    // still means "run a tick" (the automatic cadence); only stop breaks.
    d_wake_cv.wait_for(lock, d_config.tick_period, [this] { return d_stop || d_poke; });
    if (d_stop) {
      break;
    }
    d_poke = false;

    // Drive one tick under the mutex -- the same mutex the writer's after_commit
    // holds, so the two never drain the reclamation queue concurrently.
    const std::uint64_t monotonic_tick = d_config.tick_source();
    expected<std::monostate, WorkspaceFileError> result = d_housekeeper.tick(monotonic_tick);
    if (!result) {
      // A background tick has no synchronous caller: capture the error and hand
      // it to the optional callback (never abort, never throw off the thread).
      d_last_checkpoint_error = result.error();
      if (d_config.on_checkpoint_error) {
        d_config.on_checkpoint_error(result.error());
      }
    }
    d_background_ticks.fetch_add(1, std::memory_order_release);
    d_progress_cv.notify_all(); // release any flush() blocked on this tick
  }
  // Stop path: one final drain to quiescence so teardown leaves the reclamation
  // queue empty (doc 15:144-147). Still under the mutex -- single-drainer holds.
  d_housekeeper.drain_and_quiesce();
}

} // namespace arbc
