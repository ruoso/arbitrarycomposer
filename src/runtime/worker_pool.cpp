#include <arbc/runtime/worker_pool.hpp>

#include <optional>
#include <utility>

namespace arbc {

WorkerPool::WorkerPool(WorkerPoolConfig config) : d_config(std::move(config)) {
  // Threads started LAST, after all state is constructed (mirrors
  // HousekeepingThread): a worker may run before the ctor body returns, so every
  // member it touches must already be live.
  d_workers.reserve(d_config.worker_count);
  for (std::size_t i = 0; i < d_config.worker_count; ++i) {
    d_workers.emplace_back([this] { run(); });
  }
}

WorkerPool::~WorkerPool() {
  request_stop();
  for (std::thread& worker : d_workers) {
    if (worker.joinable()) {
      worker.join();
    }
  }
}

void WorkerPool::request_stop() noexcept {
  {
    std::lock_guard<std::mutex> lock(d_mutex);
    d_stop = true;
  }
  d_work_cv.notify_all();
  d_completion_cv.notify_all();
}

void WorkerPool::poke() noexcept {
  {
    std::lock_guard<std::mutex> lock(d_mutex);
    ++d_settle_gen;
  }
  d_completion_cv.notify_all();
}

bool WorkerPool::wait_completions(std::optional<std::chrono::steady_clock::time_point> until) {
  std::unique_lock<std::mutex> lock(d_mutex);
  const auto ready = [this] { return d_stop || d_settle_gen != d_drained_gen; };
  if (until.has_value()) {
    d_completion_cv.wait_until(lock, *until, ready);
  } else {
    d_completion_cv.wait(lock, ready);
  }
  const bool settled_since_drain = d_settle_gen != d_drained_gen;
  d_drained_gen = d_settle_gen; // consume: the next park waits for a fresh advance
  return settled_since_drain;
}

void WorkerPool::submit(RenderTask task) {
  {
    std::lock_guard<std::mutex> lock(d_mutex);
    if (d_stop) {
      return; // refuse new work after stop; the completion is left unsettled
    }
    d_submitted.fetch_add(1, std::memory_order_release);
  }

  if (d_config.worker_count == 0) {
    submit_inline(std::move(task));
    return;
  }

  Content* const content = task.content;
  const bool serialize = d_config.serialize_predicate(content);
  {
    std::lock_guard<std::mutex> lock(d_mutex);
    if (serialize) {
      SerialState& state = d_serial[content];
      if (state.active) {
        state.parked.push_back(std::move(task)); // one in-flight already: park FIFO
        return;
      }
      state.active = true; // admit this one; hold the gate until its render returns
    }
    d_ready.push_back(std::move(task));
  }
  d_work_cv.notify_one();
}

void WorkerPool::submit_inline(RenderTask task) {
  Content* const content = task.content;
  const bool serialize = d_config.serialize_predicate(content);
  if (serialize) {
    std::unique_lock<std::mutex> lock(d_mutex);
    SerialState& state = d_serial[content];
    if (state.active) {
      // Re-entrant submit for an already-in-flight serialized content (the outer
      // render is still on the stack): park and let the outer drain run it after.
      state.parked.push_back(std::move(task));
      return;
    }
    state.active = true;
  }

  std::optional<RenderTask> current;
  current.emplace(std::move(task));
  for (;;) {
    run_task(std::move(*current), serialize);
    current.reset();
    if (!serialize) {
      break;
    }
    // Release the next parked task for this content and run it inline too, so a
    // re-entrant chain drains before submit_inline returns.
    std::lock_guard<std::mutex> lock(d_mutex);
    SerialState& state = d_serial[content];
    --state.rendering;
    if (!state.parked.empty()) {
      current.emplace(std::move(state.parked.front()));
      state.parked.pop_front();
      continue; // gate stays active for the handed-off task
    }
    state.active = false;
    d_serial.erase(content);
    break;
  }
}

void WorkerPool::run_task(RenderTask task, bool serialize) {
  Content* const content = task.content;
  if (serialize) {
    std::lock_guard<std::mutex> lock(d_mutex);
    const std::uint64_t in_flight = static_cast<std::uint64_t>(++d_serial[content].rendering);
    if (in_flight > d_max_in_flight.load(std::memory_order_relaxed)) {
      d_max_in_flight.store(in_flight, std::memory_order_relaxed);
    }
  }

  // One settle path (doc 03:117-121): fold a returned-inline result through
  // `complete`; a `nullopt` return means the content settles `done` off-thread.
  const std::optional<RenderResult> inline_result = content->render(task.request, task.done);
  if (inline_result.has_value()) {
    task.done->complete(*inline_result);
  }
  if (task.done->settled()) {
    // Observed synchronously (inline or inline-in-worker): count it and wake a
    // parked render thread. A genuinely-async settle is counted+woken by the
    // external settler's own `poke()`.
    d_completed.fetch_add(1, std::memory_order_release);
    poke();
  }
  // For serialized content the `rendering` decrement + gate release happens in
  // the caller (`run`/`submit_inline`) so the two release paths stay explicit.
}

void WorkerPool::run() {
  for (;;) {
    std::optional<RenderTask> task;
    {
      std::unique_lock<std::mutex> lock(d_mutex);
      d_work_cv.wait(lock, [this] { return d_stop || !d_ready.empty(); });
      if (d_stop) {
        return; // in-flight renders already finished; queued work is abandoned
      }
      task.emplace(std::move(d_ready.front()));
      d_ready.pop_front();
    }

    Content* const content = task->content;
    const bool serialize = d_config.serialize_predicate(content);
    run_task(std::move(*task), serialize);
    task.reset();

    if (serialize) {
      std::lock_guard<std::mutex> lock(d_mutex);
      SerialState& state = d_serial[content];
      --state.rendering;
      if (!state.parked.empty()) {
        // Hand the gate to the next parked task; it becomes runnable now.
        d_ready.push_back(std::move(state.parked.front()));
        state.parked.pop_front();
        d_work_cv.notify_one();
      } else {
        state.active = false;
        d_serial.erase(content);
      }
    }
  }
}

} // namespace arbc
