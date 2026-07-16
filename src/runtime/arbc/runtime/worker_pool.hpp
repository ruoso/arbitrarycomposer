#pragma once

#include <arbc/arbc_api.h>
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
// pool before dropping pending surfaces -- `drain_owner(owner)`, below, is that
// drain, and `owner` is what makes it addressable.
//
// `owner` is an OPAQUE submitter tag (`runtime.shared_worker_pool` Decision 3),
// stamped by `worker_backed_dispatch` with the submitting driver's `this`. The
// pool never dereferences it; it only compares and hashes it, exactly as it
// already keys its serialization gate on a raw `const Content*`. It exists
// because a pool may be SHARED by several drivers: when one of them dies, the
// pool has to know which of its outstanding tasks belong to the corpse and which
// belong to a sibling that is still rendering.
struct RenderTask {
  Content* content{nullptr};
  RenderRequest request;
  std::shared_ptr<RenderCompletion> done;
  const void* owner{nullptr};
};

// The SECOND lane on the pool: a generic, non-render unit of work
// (serialize.tile_store_parallel_save Decision 1). The runtime pool's only render seam
// takes a `RenderTask` -- a `Content*` rendering into a caller `Surface&`, guarded
// leaf-only by `check_worker_dispatch.py`. A tile-encode job is neither a `Content` nor
// a render, so it cannot ride the render lane; this lane carries it instead. A `WorkTask`
// is a plain movable-callable that the pool runs on a worker (or inline at
// `worker_count == 0`) and whose caller-owned `done` it settles when the callable
// returns.
//
// It shares the render lane's threads, its `poke`/`wait_completions`/`CompletionCursor`
// park machinery, its `drain_owner` teardown, and its counters -- but NOT the render
// lane's leaf-only policy or its per-content serialization gate: a work job is pure by
// construction, so every one is free to run on any worker with no gate. The leaf-only
// rule governs the RENDER lane specifically; this lane is cache-free because its jobs
// touch only their OWN caller-owned buffers and the immutable pinned document version
// (never the `TileCache`), so doc 02's "workers never touch the cache" holds for it by
// construction. Errors are values: the callable converts any failure into its own
// caller-owned result and never throws across the worker boundary (a body that threw
// would terminate, exactly as a `RenderTask` body that throws does).
//
// A work job carries no result payload -- its output goes into the caller-owned buffer the
// job writes, exactly as the audio pump's fills go into pump-owned staging -- so
// `WorkCompletion` is a bare settle-once flag rather than a `Completion<Result>`: the pool
// settles it when the job returns, and a waiter reads `settled()`. (It is deliberately NOT
// `Completion<T>`, whose out-of-line template body is explicitly instantiated in `contract`
// (L4) for the render/audio result types only; a runtime-defined result cannot be
// instantiated there without an L4->L5 edge.)
class WorkCompletion {
public:
  bool settled() const noexcept { return d_settled.load(std::memory_order_acquire); }
  void settle() noexcept { d_settled.store(true, std::memory_order_release); }

private:
  std::atomic<bool> d_settled{false};
};

struct WorkTask {
  std::function<void()> job;            // pure, movable; settles nothing itself
  std::shared_ptr<WorkCompletion> done; // caller-owned; the pool settles it post-run
  const void* owner{nullptr};           // the opaque submitter tag, as `RenderTask`
};

// A caller-owned drain cursor (`runtime.shared_worker_pool` Decision 1). The
// POOL holds the settle counter; the CURSOR holds how much of it one waiter has
// already seen.
//
// The cursor is caller-owned because the drain is not a pool-global fact. With a
// single pool-wide cursor, a settle is consumed by whichever thread parks first:
// renderer A's park would swallow the generation bump produced by renderer B's
// tile settling, and B would re-park to its deadline believing nothing landed --
// spuriously expiring, and cancelling or degrading tiles that are sitting
// finished in its own `RefinementQueue`. That is a wrong-output bug, and it fires
// the moment a second renderer touches the pool. Each parking thread owning its
// own cursor is what makes a settle observable to everyone who has not yet seen
// it and consumable by no one else.
//
// This is the pattern doc 17:114-128 already blesses: the stateless library takes
// the persistent value from the caller, so the value lives with the caller that
// has an identity and the library stays identity-free.
struct CompletionCursor {
  std::uint64_t drained_gen = 0;
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
class ARBC_API WorkerPool {
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

  // Enqueue a generic work job on the pool's SECOND lane (Decision 1). Deliberately NOT
  // named `submit`: the render-dispatch grep-lint (`check_worker_dispatch.py`) anchors on
  // the token `submit(` (and on `RenderTask`), and a work job is neither. A work job is
  // pure, so there is no per-content serialization gate and no `serialize_predicate`
  // call: every one is free to run on any worker. In inline mode (`worker_count == 0`) it
  // runs immediately on the calling thread. Post-stop `submit_work` is a no-op that leaves
  // the completion unsettled, exactly as `submit`. The job is counted in the SAME
  // accounting identity as renders (`tasks_submitted`/`tasks_completed`/`tasks_dropped`),
  // so a mix of `submit` and `submit_work` still closes `tasks_submitted() ==
  // tasks_completed() + tasks_dropped() + <outstanding>` at quiescence.
  void submit_work(WorkTask task);

  // The async-completion wake handle (`content.hpp:117-118`): any settler -- a
  // worker after an inline-in-worker settle, or externally-async content on its
  // own thread -- calls this to wake a render thread parked in
  // `wait_completions`. Bumps the settle generation and notifies; never blocks.
  //
  // Owner-BLIND, deliberately and permanently. This is the contract-facing wake
  // handle: content reaches it through `InteractiveRenderer::worker_pool()` and
  // holds a `WorkerPool&` and nothing else -- it knows nothing about renderers, so
  // it could not name a waiter to wake even if the pool asked it to. So the wake
  // stays a BROADCAST and each waiter filters it against its own
  // `CompletionCursor`. A sibling's settle may therefore wake a parked renderer
  // that has nothing of its own to reap; that spurious wake is bounded (one per
  // sibling settle), cannot spin (the frame loop re-tests its own pending queue
  // and re-parks against the SAME absolute deadline) and is paid only when a pool
  // is actually shared. A STOLEN wake -- the thing a pool-global cursor produces --
  // is what is not permitted.
  void poke() noexcept;

  // Block the calling (render) thread until the settle generation advances past
  // `cursor`, or `until` elapses (nullopt = no timeout); returns whether a
  // completion settled (`false` only on timeout). Waits on a CONDITION (a
  // settle-count advance), never a fixed sleep. The frame loop parks here between
  // `submit`-ing misses and calling `poll_refinements`. The optional timeout is a
  // park bound, never a deadline decision.
  //
  // `cursor` is the caller's own (`CompletionCursor`, above) and is advanced to
  // the pool's current generation on return: consuming a settle advances the
  // CALLER's view of the counter, never anybody else's.
  bool wait_completions(CompletionCursor& cursor,
                        std::optional<std::chrono::steady_clock::time_point> until);

  // The pool's current settle generation. A caller joining a long-lived, already-
  // busy pool seeds its cursor from this so its first park is not handed a free
  // return for settles that happened before it existed. A nicety, not a
  // correctness requirement -- a stale cursor costs one extra predicate
  // evaluation in the park loop, never a missed settle.
  std::uint64_t settle_generation() const noexcept;

  // Wait until the pool is no longer TOUCHING `owner`'s tasks, then return. Purges
  // the owner's not-yet-started tasks from the ready queue and from every
  // per-content parked FIFO, then blocks until every one of its STARTED tasks has
  // returned from `content->render`.
  //
  // This is the drain the lifetime contract above names, made addressable. Until
  // `runtime.shared_worker_pool` it was `~WorkerPool` -- which works only for a
  // driver that OWNS its pool. A driver that BORROWS one destroys its pending
  // surfaces with the pool still alive and its workers still writing into them, so
  // it must say so explicitly, and it must say so about its OWN work only: a
  // sibling driver's renders keep running and the pool stays usable.
  //
  // It waits on "no longer outstanding", NOT on "settled", and the difference is
  // what makes it terminate unconditionally. A cancelled task still runs (cancel is
  // advisory, `content.hpp:161-165`), and content that settles off-thread may never
  // settle at all -- so waiting for settlement can hang. Every STARTED task's render
  // returns even under stop (`02-architecture#worker-pool-stops-gracefully`), and
  // every UNSTARTED task of this owner is purged before the wait begins, so waiting
  // for outstanding-ness cannot.
  //
  // A purged task is dropped with its completion left UNSETTLED -- the same
  // disposition a post-stop `submit` gives it, and safe for the same reason: the
  // completion is a `shared_ptr` its caller owns, and it is about to die with the
  // caller. Purged tasks are counted by `tasks_dropped()`.
  //
  // Does NOT stop the pool: stop is terminal and pool-global, so a borrowing
  // renderer calling it would strand every sibling's renders unsettled forever.
  void drain_owner(const void* owner);

  // Idempotent: sets the stop flag and wakes all workers (join is in the dtor).
  // The POOL OWNER's prerogative -- never a borrowing driver's (see `drain_owner`).
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
  // Tasks removed from the queues WITHOUT running: `drain_owner`'s purge, and the
  // queued work `run()` abandons on stop (which the pool has always done silently --
  // this counter is what makes it observable). The counter exists so the accounting
  // identity `tasks_submitted() == tasks_completed() + tasks_dropped() + <still
  // outstanding>` can be ASSERTED at quiescence: without it, a purge that dropped the
  // wrong owner's work and a purge that dropped nothing at all are the same
  // observation -- a submitted task that simply never settled.
  std::uint64_t tasks_dropped() const noexcept { return d_dropped.load(std::memory_order_acquire); }
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
  void run_work_task(WorkTask task);              // generic-lane job + settle + counters
  void submit_work_inline(WorkTask task);         // worker_count == 0 work executor

  // --- owner bookkeeping (all called with `d_mutex` held) ---
  // A task is OUTSTANDING from the moment `submit` admits it until its `run_task`
  // returns -- not until its completion settles, which may never happen
  // (`drain_owner`).
  void claim_outstanding(const void* owner);
  void release_outstanding(const void* owner);
  // Drop a queued task without running it: count it and release its outstanding
  // claim. The completion is left unsettled.
  void drop_queued(const RenderTask& task);
  void drop_queued_work(const WorkTask& task); // the work-lane mirror of drop_queued
  // Drop every queued (never-started) task, whatever its owner: what `run()` does
  // to the ready queue and the parked FIFOs when it observes stop.
  void abandon_queued();

  WorkerPoolConfig d_config;

  mutable std::mutex d_mutex;              // guards the queues + serialization map + gens
  std::condition_variable d_work_cv;       // parks idle workers
  std::condition_variable d_completion_cv; // parks a render thread in wait_completions
  std::condition_variable d_drain_cv;      // parks a dying driver in drain_owner
  std::deque<RenderTask> d_ready;          // shared FIFO of runnable render tasks
  std::deque<WorkTask> d_work_ready;       // shared FIFO of runnable generic-lane jobs
  std::unordered_map<const Content*, SerialState> d_serial; // per-content gate
  // Outstanding tasks per submitter (`RenderTask::owner`) -- the whole mechanism
  // behind `drain_owner`. Keyed on the opaque tag exactly as `d_serial` is keyed on
  // a raw `const Content*`; entries are erased at zero, so an idle owner costs
  // nothing and a pool with one driver holds one entry.
  std::unordered_map<const void*, std::size_t> d_outstanding;
  bool d_stop = false;

  // The completion-wake condition: `poke()` bumps `d_settle_gen`; a
  // `wait_completions` returns when it advances past the CALLER's `CompletionCursor`.
  // The pool holds no drain cursor of its own -- that is the point (Decision 1).
  std::uint64_t d_settle_gen = 0;

  std::atomic<std::uint64_t> d_submitted{0};
  std::atomic<std::uint64_t> d_completed{0};
  std::atomic<std::uint64_t> d_dropped{0};
  std::atomic<std::uint64_t> d_max_in_flight{0};

  std::vector<std::thread> d_workers; // started last in the ctor body
};

} // namespace arbc
