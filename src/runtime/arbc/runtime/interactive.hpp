#pragma once

#include <arbc/base/time.hpp>             // Time
#include <arbc/compositor/compositor.hpp> // Viewport, ContentResolver, Backend, SurfacePool, Surface
#include <arbc/compositor/counters.hpp>   // CompositorCounters, CompositorStats, TileCache
#include <arbc/compositor/damage_planning.hpp> // DirtyRegion + the map/clock/invalidate free functions
#include <arbc/compositor/operator_graph.hpp> // OperatorLayer, route_operator_damage
#include <arbc/compositor/refinement.hpp>     // RefinementQueue, poll_refinements
#include <arbc/compositor/tile_planning.hpp>  // render_frame_interactive
#include <arbc/contract/content.hpp>          // Deadline
#include <arbc/model/damage.hpp>              // Damage
#include <arbc/model/model.hpp>               // DocRoot
#include <arbc/runtime/pull_identity.hpp>     // PullIdentityMap, pull_identity_of
#include <arbc/runtime/worker_pool.hpp>       // WorkerPool, WorkerPoolConfig

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
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
//
// Operators (`runtime.interactive_pull_wiring`). Each working frame builds a
// frame-local `PullServiceImpl` over the `TileCache&`/`Backend&` it is handed and
// passes it to `render_frame_interactive`, so the interactive driver serves the
// same operator contract the offline one does (doc 02:40-41, "two drivers over the
// same core"): an identity endpoint delivers its terminal input's pixels instead of
// compositing blank, and every operator input caches under its OWN identity rather
// than colliding on `ObjectId{}`. The `id_of` is `make_pull_identity_of`'s -- the
// same seam the export driver calls -- memoized on `state.revision()` (the pinned
// graph is immutable within a revision, doc 02:121-124, and a revision bump changes
// every tile key anyway, doc 02:94-95), so the O(graph) walk is per-edit, not
// per-frame. The dispatch is `direct_dispatch()`: every render still runs inline on
// the frame thread and no worker touches the cache (`runtime.worker_dispatch_leaf_only`
// owns the worker-backed swap).
//
// Because `PullConfig::pending` is wired, an operator's input can now answer
// asynchronously, and its arrival damage names the input's SYNTHESIZED id -- which
// is not a layer root, so `map_damage_to_device` (which matches damage against layer
// roots) would map it to zero device rects and never schedule the follow-up frame
// doc 02:69-71 promises. Step 6 therefore routes every arrival up through
// `route_operator_damage` to the operator layers that show it, and carries the
// ROUTED set, so frame N+1 re-plans the operator's footprint and re-enters the
// identity delivery branch -- the interactive analog of the export driver's
// re-composite pass.
//
// MODEL damage is routed the same way and for the same reason
// (`runtime.operator_model_damage_routing`), but through a sibling entry point: an
// edit's `Damage.object` is the edited object's own MODEL id, so it resolves through
// the `ContentResolver` rather than the inverse identity map (the two are different id
// spaces), and unlike an arrival it INVALIDATES as well as re-plans. Without it, an
// edit to a content that an operator consumes by `$ref` and that is not itself a layer
// matches no layer root, maps to an empty dirty region, early-outs, and never repaints
// -- the under-approximation doc 13:124-128 calls a correctness bug. The routed set
// additionally carries the damaged input's PULL identity, because its tiles are cached
// under that, not under its model id (doc 13:145-149). Clock-advance damage is NOT
// routed: an operator over a moving input is itself non-`Static` and already carries
// whole-footprint damage under its own object (doc 13:108-112).

namespace arbc {

// The zoom-gesture sign the interactive loop feeds `prime_prefetch` (Decision 5,
// doc 04:99-101): the sign of the frame-over-frame camera scale-magnitude delta.
// `prime_prefetch` takes a caller-supplied sign because "the compositor infers no
// gesture" (`refinement.hpp:95`), and the loop is the only place that sees
// successive viewports. Returns `+1` when the camera magnified since the previous
// frame, `-1` when it shrank, and `0` when unchanged or there is no prior frame
// (`prev_scale <= 0`, the pre-first-frame sentinel) -- "no gesture -> no zoom
// speculation". A free function so the derivation is directly unit-testable.
int zoom_direction_from_scale_delta(double prev_scale, double scale) noexcept;

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
  // How many times the per-revision pull-identity memo has been (re)built: the
  // behavioral counter that pins the wiring's per-frame cost at O(1) rather than
  // O(graph) (doc 16:54-62 -- never a wall-clock assertion). Bumps once per
  // revision that CARRIES MODEL DAMAGE (model-damage routing needs the operator-layer
  // set before the early-out) or that actually renders a frame; a still frame carries
  // neither, early-outs, and builds nothing.
  std::uint64_t identity_map_builds() const noexcept { return d_identity_map_builds; }

  // The async-completion park/wake substrate. Exposed so externally-async content
  // (or, in M4, `compositor.pull_service`'s dispatch) can `poke()` a render
  // thread parked in this loop's `wait_completions`.
  WorkerPool& worker_pool() noexcept { return d_pool; }

private:
  // Rebuild the per-revision pull-identity memo when `revision` differs from the
  // memo's (a no-op otherwise): the `Content* -> ObjectId` map, the `id_of` functor
  // over it, its inverse (which arrival routing resolves a damaged `ObjectId` back
  // through), and the visible operator-layer set `route_operator_damage` walks.
  // Called from a frame that carries model damage (Step 1 -- routing needs the
  // operator-layer set BEFORE the no-damage early-out) or that does work (Step 3), so a
  // still frame's memo never grows.
  void refresh_identity_memo(const DocRoot& state, const ContentResolver& resolve,
                             std::uint64_t revision);

  // Fold each refinement arrival forward through the operator layers that show it
  // (doc 02:69-71, doc 13:104-107): the returned set is the arrivals themselves
  // PLUS, for any arrival naming a content an operator consumes, the operator's own
  // damaged footprint. Over-approximation is sound and under-approximation is a
  // correctness bug (doc 13:124-128), so every arrival is routed -- an operator that
  // does not reach the damaged content emits nothing.
  std::vector<Damage> route_arrival_damage(std::span<const Damage> arrival) const;

  // Fold each MODEL-damage record forward through the operator layers that reach it
  // (doc 13:124-128, doc 05:141-144): the returned set is the edits themselves PLUS, for
  // each damaged content, a record under its PULL identity (the key its shared input
  // tiles cache under, doc 13:145-149) and one under every operator layer that consumes
  // it. Sibling of `route_arrival_damage`, deliberately not merged with it (Decision 1):
  // the two classes differ in how a `Damage.object` becomes a `const Content*` -- an
  // arrival names a pull identity and inverts through `d_content_by_id`, an edit names a
  // MODEL id and inverts through the `ContentResolver` -- and in what they do to the
  // cache: an arrival re-plans without invalidating, an edit invalidates.
  //
  // One set, two consumers (Decision 3): `map_damage_to_device` contributes zero rects
  // for the pull-identity records (a synthesized id matches no layer root), so the same
  // set feeds the device mapping and the invalidation without a second loop.
  std::vector<Damage> route_model_damage(std::span<const Damage> model_damage,
                                         const ContentResolver& resolve) const;

  RefinementQueue d_pending;                     // the frame-to-frame registry of async renders
  CompositorCounters d_counters;                 // persistent behavioral counts across frames
  WorkerPool d_pool;                             // async-completion park/wake (inline by default)
  Clock d_clock;                                 // the loop's only wall-clock read
  std::optional<std::uint64_t> d_prior_revision; // last-completed revision (stale probe)
  std::optional<Time> d_prev_time;               // previous composition time (clock advance)
  // The previous frame's viewport camera scale magnitude (`camera.max_scale()`),
  // the only inter-frame camera state the loop keeps -- Step 7 compares it with
  // this frame's to pick a `zoom_direction` sign for `prime_prefetch` (Decision
  // 5). `0` before the first rendered frame (and while unchanged) means "no
  // gesture", so the zoom ring stays cold until the camera actually scales.
  double d_prev_camera_scale{0.0};
  // Arrival damage a poll produced this frame, owed to the NEXT frame's plan so
  // the just-inserted sharp tiles re-plan Fresh. Kept separate from the
  // invalidation set: it re-plans/re-composites the refined region WITHOUT
  // dropping the tiles the poll just inserted (a revision bump / model edit is
  // what invalidates; a refinement arrival is not).
  std::vector<Damage> d_carried_damage;

  // The per-revision pull-identity memo. `make_pull_identity_of` walks the whole
  // reachable content graph, which the deadline-bounded loop must not pay every
  // frame; within one revision the pinned graph is immutable, so the memo is exact,
  // and across a bump every tile key changes anyway, so a shifted synthesized id can
  // never serve a stale hit. Keying on the revision alone is sound under exactly the
  // single-document-per-renderer assumption `d_prior_revision` already relies on.
  std::optional<std::uint64_t> d_identity_revision;      // the revision the memo was built at
  std::shared_ptr<const PullIdentityMap> d_identity_map; // Content* -> ObjectId
  std::function<ObjectId(const Content*)> d_id_of;       // PullConfig::id_of over it
  std::unordered_map<ObjectId, const Content*> d_content_by_id; // the inverse, for routing
  std::vector<OperatorLayer> d_operator_layers;                 // the visible operator layers
  std::uint64_t d_identity_map_builds{0};                       // memo (re)builds, a counter
};

} // namespace arbc
