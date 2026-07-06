#pragma once

#include <arbc/base/time.hpp>             // Time
#include <arbc/compositor/compositor.hpp> // Viewport, ContentResolver, Backend, SurfacePool, Surface
#include <arbc/compositor/counters.hpp>   // CompositorCounters, CompositorStats, TileCache
#include <arbc/compositor/damage_planning.hpp> // DirtyRegion + the map/clock/invalidate free functions
#include <arbc/compositor/refinement.hpp>      // RefinementQueue, poll_refinements
#include <arbc/compositor/tile_planning.hpp> // render_frame_interactive
#include <arbc/contract/content.hpp>         // Deadline
#include <arbc/model/damage.hpp>             // Damage
#include <arbc/model/model.hpp>              // DocRoot
#include <arbc/runtime/worker_pool.hpp>      // WorkerPool, WorkerPoolConfig

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

// The interactive render driver for `arbc::runtime` (L5, doc 17:60): doc
// 02:49-71 ("The frame, interactively") made concrete. It is the second render
// driver over the same compositor (doc 02:40-41) -- the interactive analog of
// the offline one-shot `render_offline` -- but a CLASS, not a free function,
// because the interactive frame carries state BETWEEN frames that the offline
// evaluation does not: the pending-refinement registry, the persistent
// behavioral counters, the last-completed revision (the stale probe), and the
// previous composition time (the clock-advance range).
//
// Every behavior doc 02's frame loop promises already ships as a caller-owned
// compositor free function (`render_frame_interactive`, `poll_refinements`,
// `map_damage_to_device`, `clock_advance_damage`, `invalidate_damage`) or the
// worker pool -- each of which "returns a value rather than scheduling" and
// holds no cross-frame state, deliberately deferring the frame loop, the
// deadline authority, and the persistent state to `runtime.interactive` (doc
// 17:78-80,88-95, `worker_pool.hpp:33-36`, `tile_planning.hpp:161,163-164`).
// This class is that closure: it owns the persistent state and the deadline
// policy and threads the stateless compositor free functions by pointer.
//
// Deadline enforcement lives here and reads the ONLY wall clock in the loop --
// an injected `steady_clock` source (default `steady_clock::now`). The frame
// samples it ONCE to get `d = now() + budget`, stamps the `Deadline` value from
// `d` onto miss requests, and bounds `wait_completions(d)` on the SAME `d` (one
// instant, two uses, no drift). Injecting the clock keeps the whole deadline
// path deterministic under a fake clock (doc 16:54-62's no-wall-clock rule).
//
// M3 runs the inline pool (`worker_count == 0`, the default): misses fill inline
// through `render_frame_interactive`, and the `WorkerPool`'s role here is the
// async-completion park/wake (`wait_completions`/`poke`) for externally-async
// content, not parallel miss dispatch -- that is `compositor.pull_service` (M4).

namespace arbc {

// The deadline-bounded, damage-driven interactive frame loop (doc 02:49-71).
// Owns the frame-to-frame state doc 17:88-95 assigns to runtime and threads it
// into the stateless compositor by pointer. Non-copyable and non-movable (it
// owns a `WorkerPool`, which owns threads).
class InteractiveRenderer {
public:
  // What one frame reports back to the host's event loop (host_objects' owns the
  // actual re-invocation): whether newly-settled arrivals owe a follow-up frame.
  struct FrameOutcome {
    bool schedule_follow_up{false};
  };

  // The injected wall-clock source -- the loop's only clock read. Empty selects
  // `steady_clock::now`; a test hands a fake clock so the deadline path is
  // deterministic and wall-clock-free (doc 16:54-62).
  using Clock = std::function<std::chrono::steady_clock::time_point()>;

  explicit InteractiveRenderer(WorkerPoolConfig pool_config = {}, Clock clock = {});

  InteractiveRenderer(const InteractiveRenderer&) = delete;
  InteractiveRenderer& operator=(const InteractiveRenderer&) = delete;
  InteractiveRenderer(InteractiveRenderer&&) = delete;
  InteractiveRenderer& operator=(InteractiveRenderer&&) = delete;

  // Run doc 02's six interactive-frame steps as one deadline-bounded pass over
  // `target` (the caller-persisted device surface). `model_damage` is the
  // already-accumulated model-side damage the host drained from its `DamageSink`
  // this frame (the sink lifecycle is host_objects', not this task);
  // `composition_time` is the transport-sampled content-local time; `budget` is
  // the per-frame compute budget (the deadline is `clock() + budget`). Returns
  // whether a follow-up frame is owed. Never blocks past the deadline; a failed
  // or cancelled render degrades to the placeholder policy, never thrown through
  // the loop.
  FrameOutcome render_frame(const DocRoot& state, const ContentResolver& resolve,
                            const Viewport& viewport, TileCache& cache, Backend& backend,
                            SurfacePool& pool, Surface& target,
                            std::span<const Damage> model_damage, Time composition_time,
                            std::chrono::steady_clock::duration budget);

  // The persistent behavioral counters accumulated across frames (doc 16:54-62).
  const CompositorCounters& counters() const noexcept { return d_counters; }
  // The composed observability snapshot (compositor counts beside the cache's).
  CompositorStats stats(const TileCache& cache) const {
    return counters_snapshot(d_counters, cache);
  }

  // Frame-to-frame state, exposed read-only for tests and host observability.
  const RefinementQueue& pending() const noexcept { return d_pending; }
  std::optional<std::uint64_t> prior_revision() const noexcept { return d_prior_revision; }
  std::optional<Time> previous_time() const noexcept { return d_prev_time; }

  // The async-completion park/wake substrate. Exposed so externally-async content
  // (or, in M4, `compositor.pull_service`'s dispatch) can `poke()` a render
  // thread parked in this loop's `wait_completions`.
  WorkerPool& worker_pool() noexcept { return d_pool; }

private:
  RefinementQueue d_pending;                     // the frame-to-frame registry of async renders
  CompositorCounters d_counters;                 // persistent behavioral counts across frames
  WorkerPool d_pool;                             // async-completion park/wake (inline by default)
  Clock d_clock;                                 // the loop's only wall-clock read
  std::optional<std::uint64_t> d_prior_revision; // last-completed revision (stale probe)
  std::optional<Time> d_prev_time;               // previous composition time (clock advance)
  // Arrival damage a poll produced this frame, owed to the NEXT frame's plan so
  // the just-inserted sharp tiles re-plan Fresh. Kept separate from the
  // invalidation set: it re-plans/re-composites the refined region WITHOUT
  // dropping the tiles the poll just inserted (a revision bump / model edit is
  // what invalidates; a refinement arrival is not).
  std::vector<Damage> d_carried_damage;
};

} // namespace arbc
