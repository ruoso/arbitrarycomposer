#include <arbc/runtime/worker_pool.hpp>

#include <cstdint>
#include <deque>
#include <optional>
#include <utility>
#include <vector>

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
  // BROADCAST, not a targeted wake (Decision 1): `poke()` is the contract-facing
  // handle and its caller cannot name a waiter. Every parked waiter re-tests the
  // generation against its OWN cursor and decides for itself whether it saw
  // something new.
  d_completion_cv.notify_all();
}

std::uint64_t WorkerPool::settle_generation() const noexcept {
  std::lock_guard<std::mutex> lock(d_mutex);
  return d_settle_gen;
}

bool WorkerPool::wait_completions(CompletionCursor& cursor,
                                  std::optional<std::chrono::steady_clock::time_point> until) {
  std::unique_lock<std::mutex> lock(d_mutex);
  const auto ready = [this, &cursor] { return d_stop || d_settle_gen != cursor.drained_gen; };
  if (until.has_value()) {
    d_completion_cv.wait_until(lock, *until, ready);
  } else {
    d_completion_cv.wait(lock, ready);
  }
  const bool settled_since_drain = d_settle_gen != cursor.drained_gen;
  // Consume into the CALLER's cursor: the next park by THIS waiter waits for a
  // fresh advance, and every other waiter's view of the counter is untouched.
  cursor.drained_gen = d_settle_gen;
  return settled_since_drain;
}

void WorkerPool::claim_outstanding(const void* owner) { ++d_outstanding[owner]; }

void WorkerPool::release_outstanding(const void* owner) {
  const auto it = d_outstanding.find(owner);
  if (it == d_outstanding.end()) {
    return; // never claimed (a task the pool refused): nothing to release
  }
  if (--it->second == 0) {
    d_outstanding.erase(it); // an idle owner holds no entry
  }
}

void WorkerPool::drop_queued(const RenderTask& task) {
  d_dropped.fetch_add(1, std::memory_order_release);
  release_outstanding(task.owner);
}

void WorkerPool::drop_queued_work(const WorkTask& task) {
  d_dropped.fetch_add(1, std::memory_order_release);
  release_outstanding(task.owner);
}

void WorkerPool::abandon_queued() {
  for (const RenderTask& task : d_ready) {
    drop_queued(task);
  }
  d_ready.clear();
  for (auto& [content, state] : d_serial) {
    for (const RenderTask& task : state.parked) {
      drop_queued(task);
    }
    state.parked.clear();
  }
  // The generic lane too: queued work jobs are abandoned on stop, counted so the
  // accounting identity closes over BOTH lanes (Decision 1 / Constraint 6). No parked
  // FIFO here -- work jobs have no per-content gate.
  for (const WorkTask& task : d_work_ready) {
    drop_queued_work(task);
  }
  d_work_ready.clear();
}

void WorkerPool::drain_owner(const void* owner) {
  std::unique_lock<std::mutex> lock(d_mutex);

  // (1) The owner's PARKED tasks, first -- so the gate-release in (2) can only hand
  //     the gate to a task that survives this purge.
  for (auto& [content, state] : d_serial) {
    std::deque<RenderTask> kept;
    while (!state.parked.empty()) {
      RenderTask task = std::move(state.parked.front());
      state.parked.pop_front();
      if (task.owner == owner) {
        drop_queued(task);
      } else {
        kept.push_back(std::move(task));
      }
    }
    state.parked = std::move(kept);
  }

  // (2) The owner's READY tasks. A serialized task sitting in `d_ready` is BY
  //     CONSTRUCTION its content's gate holder: `submit` pushes it there only after
  //     setting `active`, and `run` hands the gate on by pushing the next parked task
  //     there -- so at most one admitted task per serialized content exists at a time,
  //     and while it is queued nothing of that content is rendering. Purging it
  //     therefore has to RELEASE the gate, or the content's `active` flag stays set
  //     forever and every later submit of it parks behind a task that will never run.
  //     Having a `SerialState` entry at all is exactly the "is serialized" test here
  //     (an unserialized content never gets one), so the purge never calls back into
  //     `serialize_predicate` under the lock.
  std::deque<RenderTask> kept_ready;
  std::vector<const Content*> orphaned_gates;
  while (!d_ready.empty()) {
    RenderTask task = std::move(d_ready.front());
    d_ready.pop_front();
    if (task.owner != owner) {
      kept_ready.push_back(std::move(task));
      continue;
    }
    if (d_serial.find(task.content) != d_serial.end()) {
      orphaned_gates.push_back(task.content);
    }
    drop_queued(task);
  }
  d_ready = std::move(kept_ready);

  bool handed_on = false;
  for (const Content* const content : orphaned_gates) {
    const auto it = d_serial.find(content);
    if (it == d_serial.end()) {
      continue;
    }
    SerialState& state = it->second;
    if (!state.parked.empty()) {
      d_ready.push_back(std::move(state.parked.front())); // a sibling's task inherits the gate
      state.parked.pop_front();
      handed_on = true;
    } else {
      state.active = false;
      d_serial.erase(it);
    }
  }
  if (handed_on) {
    d_work_cv.notify_all();
  }

  // (2b) The owner's queued WORK jobs. No gate and no parked FIFO, so a straight filter
  //      of the work-ready queue; a STARTED work job is waited out by the same
  //      `d_outstanding` condition below, exactly as a started render.
  std::deque<WorkTask> kept_work;
  while (!d_work_ready.empty()) {
    WorkTask task = std::move(d_work_ready.front());
    d_work_ready.pop_front();
    if (task.owner == owner) {
      drop_queued_work(task);
    } else {
      kept_work.push_back(std::move(task));
    }
  }
  d_work_ready = std::move(kept_work);

  // (3) Wait out the owner's STARTED tasks. Nothing of this owner is queued any more,
  //     so what is left is exactly the renders a worker is inside right now -- and
  //     every one of those returns, stop or no stop. A sibling's tasks are not waited
  //     on and were not touched.
  d_drain_cv.wait(lock, [this, owner] { return d_outstanding.find(owner) == d_outstanding.end(); });
}

void WorkerPool::submit(RenderTask task) {
  {
    std::lock_guard<std::mutex> lock(d_mutex);
    if (d_stop) {
      return; // refuse new work after stop; the completion is left unsettled
    }
    d_submitted.fetch_add(1, std::memory_order_release);
    // Outstanding from ADMISSION, not from dispatch: a task queued behind the
    // serialization gate is work the pool is holding on this owner's behalf, and a
    // drain that ignored it would return while it was still poised to run against
    // surfaces the owner is about to destroy.
    claim_outstanding(task.owner);
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

void WorkerPool::submit_work(WorkTask task) {
  {
    std::lock_guard<std::mutex> lock(d_mutex);
    if (d_stop) {
      return; // refuse new work after stop; the completion is left unsettled
    }
    d_submitted.fetch_add(1, std::memory_order_release);
    claim_outstanding(task.owner);
  }

  if (d_config.worker_count == 0) {
    submit_work_inline(std::move(task));
    return;
  }

  {
    std::lock_guard<std::mutex> lock(d_mutex);
    d_work_ready.push_back(std::move(task)); // no gate: a pure job runs on any worker
  }
  d_work_cv.notify_one();
}

void WorkerPool::submit_work_inline(WorkTask task) { run_work_task(std::move(task)); }

void WorkerPool::run_work_task(WorkTask task) {
  // Run the pure job, then settle the caller's completion. Unlike a render, there is no
  // off-thread settle path: a work job is synchronous by construction, so it always
  // settles here. Errors are values inside the job's own caller-owned buffer -- the job
  // returns `void` and never throws across this boundary (Constraint 7).
  if (task.job) {
    task.job();
  }
  if (task.done) {
    task.done->settle();
  }
  d_completed.fetch_add(1, std::memory_order_release);
  poke(); // bump the settle generation and broadcast, exactly as a settled render does

  // Outstanding until the job returns, not until anything downstream reaps it -- the same
  // termination argument `drain_owner` relies on for renders.
  {
    std::lock_guard<std::mutex> lock(d_mutex);
    release_outstanding(task.owner);
  }
  d_drain_cv.notify_all();
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

  // The task stops being OUTSTANDING here -- when the render returns, not when the
  // completion settles. This is the whole termination argument for `drain_owner`: a
  // content that settles off-thread may never settle, and a cancelled render settles
  // however it likes, but every `content->render` call that started has now returned,
  // so the pool is provably no longer touching this task's target surface.
  {
    std::lock_guard<std::mutex> lock(d_mutex);
    release_outstanding(task.owner);
  }
  d_drain_cv.notify_all();

  // For serialized content the `rendering` decrement + gate release happens in
  // the caller (`run`/`submit_inline`) so the two release paths stay explicit.
}

void WorkerPool::run() {
  for (;;) {
    std::optional<RenderTask> task;
    {
      std::unique_lock<std::mutex> lock(d_mutex);
      d_work_cv.wait(lock, [this] { return d_stop || !d_ready.empty() || !d_work_ready.empty(); });
      if (d_stop) {
        // In-flight renders already finished; queued work is abandoned. It always
        // was -- `tasks_dropped()` is what makes that visible, so the accounting
        // identity closes instead of leaving the abandoned tasks looking like
        // submissions that merely never settled. The first worker here empties the
        // queues and counts them; the rest find nothing left to abandon.
        abandon_queued();
        d_drain_cv.notify_all(); // a drain parked on abandoned work is now satisfied
        return;
      }
      // Render lane first: the generic lane is background encode work, and starving an
      // interactive frame behind a save's fan-out is exactly what the bounded window
      // (the fan-out's own concern) exists to prevent. When no render is ready, take a
      // work job and run it unlocked. Holding it by value (not `std::optional`) keeps
      // its lifetime inside this branch -- gcc 13's -Wmaybe-uninitialized otherwise
      // false-positives on the reaped `WorkTask`'s `shared_ptr` at end-of-scope.
      if (d_ready.empty()) {
        WorkTask work = std::move(d_work_ready.front());
        d_work_ready.pop_front();
        lock.unlock();
        run_work_task(std::move(work)); // no gate, no per-content bookkeeping
        continue;
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
