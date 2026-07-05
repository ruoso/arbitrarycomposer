#pragma once

#include <arbc/runtime/housekeeping.hpp>

#include <arbc/base/expected.hpp>
#include <arbc/pool/checkpoint.hpp>
#include <arbc/pool/reclamation.hpp>
#include <arbc/pool/slot_store.hpp>
#include <arbc/pool/workspace_file.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <variant>

namespace arbc {

// The owned background housekeeping thread for arbc::runtime (doc 17: L5, the
// listed "housekeeping thread" runtime content). It turns the passive
// `Housekeeper` cadence object (runtime.housekeeping) into a live low-priority
// drain/checkpoint loop and -- crucially -- SERIALIZES that loop against the
// writer's between-transaction drain so the reclamation queue always has exactly
// one drainer (doc 15:129-136, reclamation.hpp:57-62).
//
// The serialization is enforced by CONSTRUCTION: this class OWNS the Housekeeper
// by value, so it is the sole access path, and every draining entry point --
// the background `tick`, the writer's `after_commit`, `request_checkpoint`, and
// `drain_and_quiesce` -- runs under one mutex. There is no unsynchronized
// backdoor to the drainer, so "writer-between-transactions OR low-priority
// thread" is a single-drainer discipline rather than a data race. RT threads
// never touch this object: they only enqueue via `store.release()`
// (lock-free, doc 15:137-143).
//
// This is the engine's first owned background worker thread and its first
// std::condition_variable. It parks on the cv with a timeout, wakes on the
// timeout OR an explicit poke, drives one tick under the mutex, and on stop runs
// one final drain-to-quiescence before joining (doc 15:144-147). No test asserts
// on a wall clock: the tick source is injectable (deterministic sequences) and
// `flush()` synchronizes on the tick counter, never on time (doc 16:54-62).

// The thread-lifecycle knobs, all defaulted.
struct HousekeepingThreadConfig {
  // The idle park interval between automatic wake-ups -- the
  // `condition_variable::wait_for` timeout. A modest low-priority cadence; it
  // never appears in a test assertion (doc 16:54-62).
  std::chrono::steady_clock::duration tick_period = std::chrono::milliseconds(50);

  // The monotonic tick provider handed to `Housekeeper::tick()`. Empty means the
  // constructor installs a default `steady_clock`-derived monotonic counter
  // (elapsed `tick_period`s since construction). Injectable so tests hand a
  // deterministic, controlled sequence -- no test reads a wall clock (doc 16;
  // consistent with `Housekeeper::tick` taking a HANDED value).
  std::function<std::uint64_t()> tick_source;

  // Nullable. A background `tick`'s checkpoint I/O failure has no synchronous
  // caller to return the value to; this callback surfaces it (errors as values,
  // never abort -- doc 15). When null, the error is only recorded (see
  // `last_checkpoint_error()`). It fires under the internal mutex, so it MUST NOT
  // re-enter this object.
  std::function<void(const WorkspaceFileError&)> on_checkpoint_error;
};

class HousekeepingThread {
public:
  // Constructs the owned `Housekeeper{queue, checkpointer, arena, policy}` and
  // launches the background loop (started last). `checkpointer`/`arena` follow
  // the Housekeeper contract (null for anonymous, live-only arenas). Non-copyable
  // and non-movable: it owns a thread, a mutex, and condition variables.
  HousekeepingThread(ReclamationQueue& queue, Checkpointer* checkpointer, Arena* arena,
                     HousekeepingConfig policy, HousekeepingThreadConfig thread_config);

  // Requests stop then joins; the loop's stop path runs a final
  // `drain_and_quiesce()` so nothing is left on the queue at teardown.
  ~HousekeepingThread();

  HousekeepingThread(const HousekeepingThread&) = delete;
  HousekeepingThread& operator=(const HousekeepingThread&) = delete;
  HousekeepingThread(HousekeepingThread&&) = delete;
  HousekeepingThread& operator=(HousekeepingThread&&) = delete;

  // The synchronized writer entry: holds the same mutex the background loop holds
  // while it ticks, which is what serializes the two drainers into one.
  expected<std::monostate, WorkspaceFileError> after_commit(SlotIndex root);

  // The explicit host-call checkpoint trigger, synchronized.
  expected<std::monostate, WorkspaceFileError> request_checkpoint();

  // The explicit teardown / bulk-release drain, synchronized.
  void drain_and_quiesce();

  // Wake the loop for an immediate tick (non-blocking): a host "there was
  // activity, drain soon" hint and the test driver's wake.
  void poke() noexcept;

  // Poke and BLOCK until the loop completes one further tick, returning the new
  // background-tick count. Waits on a CONDITION (the tick counter advanced),
  // never a wall clock -- a deterministic synchronization point for tests and a
  // host "drain now and wait" call. Returns promptly if the loop has stopped.
  std::uint64_t flush();

  // Idempotent: sets the stop flag and wakes the loop (the join is in the dtor).
  void request_stop() noexcept;

  // The synchronized observability snapshot, delegating to `Housekeeper::stats()`.
  HousekeepingStats stats() const;

  // Loop iterations completed -- the thread-level counter the policy object
  // cannot expose.
  std::uint64_t background_ticks() const noexcept;

  // The last background checkpoint error, if any (never surfaced synchronously
  // because a background tick has no caller).
  std::optional<WorkspaceFileError> last_checkpoint_error() const;

private:
  void run();

  Housekeeper d_housekeeper;         // owned -- the sole, serialized access path
  HousekeepingThreadConfig d_config; // tick_source is non-empty after the ctor

  mutable std::mutex d_mutex;            // guards the Housekeeper + all state below
  std::condition_variable d_wake_cv;     // parks the loop (poke / stop / timeout)
  std::condition_variable d_progress_cv; // wakes a blocked flush()
  bool d_stop{false};
  bool d_poke{false};
  std::optional<WorkspaceFileError> d_last_checkpoint_error;
  // Atomic so the noexcept accessor reads it without locking; written under
  // d_mutex and published to a waiting flush() via d_progress_cv.
  std::atomic<std::uint64_t> d_background_ticks{0};

  std::thread d_thread; // started last in the ctor body
};

} // namespace arbc
