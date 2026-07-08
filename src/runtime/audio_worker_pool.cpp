#include <arbc/runtime/audio_worker_pool.hpp>

#include <optional>
#include <utility>

namespace arbc {

AudioWorkerPool::AudioWorkerPool(AudioWorkerPoolConfig config) : d_config(std::move(config)) {
  // Threads started LAST, after all state is constructed (mirrors WorkerPool /
  // HousekeepingThread): a worker may run before the ctor body returns.
  d_workers.reserve(d_config.worker_count);
  for (std::size_t i = 0; i < d_config.worker_count; ++i) {
    d_workers.emplace_back([this] { run(); });
  }
}

AudioWorkerPool::~AudioWorkerPool() {
  request_stop();
  for (std::thread& worker : d_workers) {
    if (worker.joinable()) {
      worker.join();
    }
  }
}

void AudioWorkerPool::request_stop() noexcept {
  {
    std::lock_guard<std::mutex> lock(d_mutex);
    d_stop = true;
  }
  d_work_cv.notify_all();
  d_completion_cv.notify_all();
}

void AudioWorkerPool::poke() noexcept {
  {
    std::lock_guard<std::mutex> lock(d_mutex);
    ++d_settle_gen;
  }
  d_completion_cv.notify_all();
}

bool AudioWorkerPool::wait_completions(std::optional<std::chrono::steady_clock::time_point> until) {
  std::unique_lock<std::mutex> lock(d_mutex);
  const auto ready = [this] { return d_stop || d_settle_gen != d_drained_gen; };
  if (until.has_value()) {
    d_completion_cv.wait_until(lock, *until, ready);
  } else {
    d_completion_cv.wait(lock, ready);
  }
  const bool settled_since_drain = d_settle_gen != d_drained_gen;
  d_drained_gen = d_settle_gen;
  return settled_since_drain;
}

void AudioWorkerPool::submit(AudioTask task) {
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
        state.parked.push_back(std::move(task));
        return;
      }
      state.active = true;
    }
    d_ready.push_back(std::move(task));
  }
  d_work_cv.notify_one();
}

void AudioWorkerPool::submit_inline(AudioTask task) {
  Content* const content = task.content;
  const bool serialize = d_config.serialize_predicate(content);
  if (serialize) {
    std::unique_lock<std::mutex> lock(d_mutex);
    SerialState& state = d_serial[content];
    if (state.active) {
      state.parked.push_back(std::move(task));
      return;
    }
    state.active = true;
  }

  std::optional<AudioTask> current;
  current.emplace(std::move(task));
  for (;;) {
    run_task(std::move(*current), serialize);
    current.reset();
    if (!serialize) {
      break;
    }
    std::lock_guard<std::mutex> lock(d_mutex);
    SerialState& state = d_serial[content];
    --state.rendering;
    if (!state.parked.empty()) {
      current.emplace(std::move(state.parked.front()));
      state.parked.pop_front();
      continue;
    }
    state.active = false;
    d_serial.erase(content);
    break;
  }
}

void AudioWorkerPool::run_task(AudioTask task, bool serialize) {
  Content* const content = task.content;
  if (serialize) {
    std::lock_guard<std::mutex> lock(d_mutex);
    const std::uint64_t in_flight = static_cast<std::uint64_t>(++d_serial[content].rendering);
    if (in_flight > d_max_in_flight.load(std::memory_order_relaxed)) {
      d_max_in_flight.store(in_flight, std::memory_order_relaxed);
    }
  }

  // One settle path (doc 03:117-121): run the audio facet's `render_audio` and fold
  // a returned-inline result through `complete`; a `nullopt` return means the
  // content settles `done` off-thread. Content with no audio facet fails once,
  // exactly as `direct_audio_dispatch` does.
  AudioFacet* const facet = content != nullptr ? content->audio() : nullptr;
  if (facet == nullptr) {
    if (task.done) {
      task.done->fail(RenderError::ResourceUnavailable);
    }
  } else {
    const std::optional<AudioResult> inline_result = facet->render_audio(task.request, task.done);
    if (inline_result.has_value() && task.done) {
      task.done->complete(*inline_result);
    }
  }
  if (task.done && task.done->settled()) {
    d_completed.fetch_add(1, std::memory_order_release);
    poke();
  }
}

void AudioWorkerPool::run() {
  for (;;) {
    std::optional<AudioTask> task;
    {
      std::unique_lock<std::mutex> lock(d_mutex);
      d_work_cv.wait(lock, [this] { return d_stop || !d_ready.empty(); });
      if (d_stop) {
        return;
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
