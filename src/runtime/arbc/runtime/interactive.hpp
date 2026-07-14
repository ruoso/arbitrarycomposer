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
#include <cstddef>
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
// The `WorkerPool` is both the async-completion park/wake substrate
// (`wait_completions`/`poke`) and -- since `runtime.worker_dispatch_leaf_only` --
// the frame's real miss executor: the pull service's dispatch is
// `worker_backed_dispatch(d_pool)`, so a `worker_count > 0` host genuinely fans
// leaf misses out. Since `runtime.interactive_worker_count_default` the SHIPPED
// interactive default is non-zero (`default_interactive_pool_config()`, below):
// this is the first arbitrarycomposer configuration with real threads under the
// frame loop, and it is a CORRECTNESS choice, not a throughput one -- at
// `worker_count == 0` the pool IS the render (`worker_pool.cpp:66`), so the frame
// thread sits inside a slow leaf's `render` while the deadline passes and doc
// 02:61-65's "when the deadline nears, the frame proceeds with what it has" cannot
// be kept. An explicit `WorkerPoolConfig{}` still selects the degenerate inline
// executor, byte-identically, for debuggability and determinism.
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
// per-frame. The dispatch is `worker_backed_dispatch(d_pool)`: an OPERATOR miss
// (fade, crossfade, nested) renders inline on the frame thread and only LEAF misses
// reach a worker, so no worker ever touches the cache (doc 02 § Threading model,
// "Worker dispatch is leaf-only").
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
//
// BINDING (`runtime.interactive_binder_wiring`, doc 13 § "Binding is the render
// driver's obligation, and every driver discharges it"). Serving the pull service was
// only half the operator contract: a non-endpoint operator layer -- a fade at envelope
// 0.5, a crossfade at w 0.5, a nesting -- runs its OWN `render`, which pulls through a
// service it can only have received at ATTACH. So a frame that actually renders binds
// the document's whole content graph (`bind_operators`) against the frame-local
// `PullServiceImpl` before it plans, and tears the binding down with the frame. The
// scope is function-local and declared AFTER `pulls` (so it detaches before the service
// it borrows dies) and it survives the deadline park and the arrival composite, which
// re-drive operator layers whose inputs settled late. Caching it across frames is a
// use-after-free, not an optimization: it holds a `PullService&` into the frame's stack.
//
// The binding is OPT-IN, through the trailing `FrameBinding` (a `const Document*` plus
// the caller's pin). `bind_operators` needs `Document::for_each_content` to reach
// contents-table contents no `DocRoot` layer walk sees, and the frame signature carries
// only the `DocRoot`. A default `FrameBinding{}` binds nothing and is today's behavior
// exactly, so a `Model&`-constructed `HostViewport` (and every direct-renderer test)
// is untouched. A still scene never gets that far: it early-outs ahead of the pull
// service, so it binds ZERO times (`operator_binds()`).

namespace arbc {

class Document;

// The zoom-gesture sign the interactive loop feeds `prime_prefetch` (Decision 5,
// doc 04:99-101): the sign of the frame-over-frame camera scale-magnitude delta.
// `prime_prefetch` takes a caller-supplied sign because "the compositor infers no
// gesture" (`refinement.hpp:95`), and the loop is the only place that sees
// successive viewports. Returns `+1` when the camera magnified since the previous
// frame, `-1` when it shrank, and `0` when unchanged or there is no prior frame
// (`prev_scale <= 0`, the pre-first-frame sentinel) -- "no gesture -> no zoom
// speculation". A free function so the derivation is directly unit-testable.
int zoom_direction_from_scale_delta(double prev_scale, double scale) noexcept;

// The cap on the interactive driver's default worker count
// (`runtime.interactive_worker_count_default` Decision 2). The DEFAULT pool is
// per-renderer -- the renderer builds and owns it -- so an uncapped
// `hardware_concurrency() - 1` would spawn 63 threads per viewport on a 64-core
// workstation, and a second viewport would double that. The cap governs exactly
// that default: a host that opens several viewports now builds ONE pool and passes
// it to every one of them (`InteractiveRenderer(WorkerPool&, Clock)`,
// `runtime.shared_worker_pool`), sizes it however it likes, and is not bound by this
// number at all. The cap is not repealed, because it still protects the host that
// does not share -- which is the default, and the common case. `2` is the value the
// task's Google Benchmark table selected: across the leaf-heavy, operator-heavy and
// nested-deep scenes the counters at 4 workers were identical to the counters at 2
// (same renders, same composites, no duplicate in-flight), so under the refinement's
// stated tie-break -- "if the counters do not separate 2 from 4, ship 2" -- the
// smaller thread footprint wins. A host that wants more passes its own
// `WorkerPoolConfig`.
inline constexpr std::size_t k_max_interactive_workers = 2;

// The interactive driver's default worker count: `hardware_concurrency() - 1`,
// clamped to `[1, k_max_interactive_workers]`.
//
// `n - 1` because the frame thread is a PARTICIPANT, not an observer -- it plans,
// composites and parks in `wait_completions` -- so leaving it a core is the point.
// `>= 1` even on a single-core box, because the reason for a worker is that the frame
// thread can REACH the deadline park and degrade (doc 02:61-65), which is a latency
// property, not a throughput one: on one core the worker still gets scheduled while
// the frame thread parks. A `hardware_concurrency()` of `0` means "unknown" and reads
// as one core.
//
// A formula rather than a constant because a fixed `4` oversubscribes a 2-core CI
// runner by 3x and undersubscribes a workstation: a constant that ignores the machine
// is a measurement of the author's machine.
std::size_t default_interactive_worker_count();

// The pool config `InteractiveRenderer`'s constructor defaults to: the policy above
// in `worker_count`, every other knob at its `WorkerPoolConfig` default.
//
// The default is installed HERE, on the interactive driver, and NOT on
// `WorkerPoolConfig::worker_count` (which keeps its `0`, claim
// `02-architecture#worker-pool-degenerates-to-inline`): the struct is shared with
// `SequenceRenderer`, which caches `worker_count != 0` as its inline-vs-parallel
// switch (`offline_sequence.cpp:58`), so flipping it there would silently move the
// OFFLINE driver off the byte-deterministic exact path doc 02:73-85 specifies -- and
// "how many threads should an interactive host use?" is render-driver policy (doc
// 17:110-112), not a property of a config struct that does not know which driver it
// is about to configure.
WorkerPoolConfig default_interactive_pool_config();

// What a frame needs to BIND its operators, and the only thing the frame signature does
// not already carry (`runtime.interactive_binder_wiring` Decision 2): the `Document` whose
// content graph `bind_operators` walks, and the pin the frame is compositing -- the SAME
// snapshot, so a nested content reads its child's membership from the version being
// rendered (doc 05:71-75).
//
// One struct rather than two loose parameters because a document without a pin (or a pin
// without a document) is not a meaningful state: the pair is atomic, and one assert in
// `render_frame` covers it. Trailing and defaulted because the default -- bind nothing --
// is what every `Model&`-constructed viewport and every direct-driver test already does,
// so none of them move. At namespace scope rather than nested in `InteractiveRenderer`
// because a nested class's default member initializers are not available to a default
// argument of the enclosing class, which is still incomplete there.
struct FrameBinding {
  const Document* document{nullptr};
  DocStatePtr pin{nullptr};
};

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

  // OWN a pool. The default `pool_config` is the SHIPPED interactive configuration:
  // real workers (`default_interactive_pool_config()`). Passing an explicit
  // `WorkerPoolConfig{}` opts back out to the thread-free degenerate inline executor,
  // byte-identically -- the spelling every deterministic unit test and golden in the
  // tree uses.
  explicit InteractiveRenderer(WorkerPoolConfig pool_config = default_interactive_pool_config(),
                               Clock clock = {});

  // BORROW a pool (`runtime.shared_worker_pool`). A host with K viewports builds ONE
  // pool and hands it to every viewport, so K viewports cost N threads and not K x N.
  //
  // K renderers over 1 pool is the only correct multi-viewport shape, and it is the
  // cheap one. It is not interchangeable with K viewports over 1 RENDERER: the wanted-
  // tile set, the carried damage, the previous time and camera scale and the pull-
  // identity memo below are all PER-VIEWPORT state, so sharing a renderer
  // cross-contaminates one viewport's frame into another's -- silently, with no crash.
  //
  // LIFETIME, and it is on the host: `pool` MUST outlive every renderer borrowing it.
  // `~InteractiveRenderer` calls into the pool to drain its own submissions, so a pool
  // that died first is a use-after-free. Declare the pool BEFORE the renderers that
  // borrow it, exactly as the member block below declares `d_pending` before its own
  // pool and for the same reason.
  explicit InteractiveRenderer(WorkerPool& pool, Clock clock = {});

  // Drains THIS renderer's outstanding submissions out of the pool (`d_pool.drain_owner`)
  // before any member dies -- see the member block below for why that has to be a
  // destructor BODY and not a member's own destructor.
  ~InteractiveRenderer();

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
  //
  // `binding` is the document + pin this frame binds its operators against (see
  // `FrameBinding`); the default binds nothing. When set, its `pin` MUST be the very
  // snapshot `state` names -- asserted, because a valid-but-different pin would have the
  // compositor walk one snapshot while the operators pulled from another, a stale-pixel
  // bug with no crash.
  FrameOutcome render_frame(const DocRoot& state, const ContentResolver& resolve,
                            const Viewport& viewport, TileCache& cache, Backend& backend,
                            SurfacePool& pool, Surface& target,
                            std::span<const Damage> model_damage, Time composition_time,
                            std::chrono::steady_clock::duration budget,
                            const FrameBinding& binding = {});

  // The persistent behavioral counters accumulated across frames (doc 16:54-62).
  const CompositorCounters& counters() const noexcept { return d_counters; }
  // The composed observability snapshot (compositor counts beside the cache's).
  CompositorStats stats(const TileCache& cache) const {
    return counters_snapshot(d_counters, cache);
  }

  // Frame-to-frame state, exposed read-only for tests and host observability.
  const RefinementQueue& pending() const noexcept { return d_pending; }
  // The stamp the last frame that composited `content` keyed it under -- the stale
  // probe's per-content prior (`model.per_object_revision` Decision 7,
  // `02-architecture#stale-probe-is-per-content`). `nullopt` for a content this renderer
  // has never composited, which is exactly the case with no stale tier. This replaces
  // the document-global `prior_revision()` scalar, which under per-object stamps named
  // no content's prior stamp and would have made the probe miss unconditionally --
  // silently costing the degradation ladder a tier.
  std::optional<std::uint64_t> prior_stamp(ObjectId content) const noexcept {
    const auto it = d_prior_stamps.find(content);
    return it != d_prior_stamps.end() ? std::optional<std::uint64_t>(it->second) : std::nullopt;
  }
  std::optional<Time> previous_time() const noexcept { return d_prev_time; }
  // How many times the per-revision pull-identity memo has been (re)built: the
  // behavioral counter that pins the wiring's per-frame cost at O(1) rather than
  // O(graph) (doc 16:54-62 -- never a wall-clock assertion). Bumps once per
  // revision that CARRIES MODEL DAMAGE (model-damage routing needs the operator-layer
  // set before the early-out) or that actually renders a frame; a still frame carries
  // neither, early-outs, and builds nothing.
  std::uint64_t identity_map_builds() const noexcept { return d_identity_map_builds; }
  // How many times this driver has bound the document's content graph: once per frame
  // that actually RENDERS with a `FrameBinding` set, and never on a frame that takes the
  // still-scene early-out (which precedes the pull service the binding borrows). The
  // behavioral counter (doc 16:54-62, never wall-clock) that pins "a still scene binds
  // nothing" -- the case that runs at 60Hz doing nothing.
  std::uint64_t operator_binds() const noexcept { return d_operator_binds; }

  // The three counters the non-zero default has to be ARGUED from
  // (`runtime.interactive_worker_count_default`, doc 16:54-62 -- "wall-clock tests lie
  // in CI; counters don't"). Frame-thread-only, plain `std::uint64_t` like
  // `CompositorCounters` (`counters.hpp:22-24`): nothing on a worker may bump one --
  // that would be exactly the data race the leaf-only rule exists to prevent, and TSan
  // would say so.

  // Frames that got past the still-scene early-out and actually did work. The
  // DENOMINATOR for "renders per frame"; the numerator is `counters().requests_issued()`.
  // A still frame bumps nothing (claim
  // `02-architecture#interactive-still-scene-schedules-no-frame`).
  std::uint64_t frames_rendered() const noexcept { return d_frames_rendered; }
  // Frames whose deadline park (Step 5) reached the deadline with renders still
  // unsettled -- `wait_completions` returned `false`. This is the counter that makes doc
  // 02:61-65's deadline promise OBSERVABLE: at `worker_count == 0` it can never bump for
  // a slow synchronous leaf, because `submit` IS the render and the frame thread is
  // inside it when the deadline passes. Derived from `wait_completions`' return value,
  // never from a second clock read (the loop samples `d_clock()` exactly once).
  std::uint64_t deadline_expiries() const noexcept { return d_deadline_expiries; }
  // Still-unsettled BestEffort pending renders the expired frame cancelled (advisory,
  // `content.hpp:122-123`) because it NO LONGER WANTS THEM -- superseded by a revision
  // bump, or no longer visible at the current camera and time
  // (`runtime.deadline_cancel_retains_wanted`). Not "everything that was in flight": that
  // blanket sweep is what this counter's sibling below exists to prove is gone. It stays
  // a live path -- during playback `achieved_time` supersedes every non-Static tile each
  // frame -- so a scene whose cancels went to zero is a scene whose tiles are all still
  // wanted, not a sweep that stopped running.
  std::uint64_t tiles_cancelled() const noexcept { return d_tiles_cancelled; }
  // Still-unsettled pending renders the expired frame deliberately LEFT IN FLIGHT because
  // it still wants them (`runtime.deadline_cancel_retains_wanted`, doc 02 § The frame,
  // interactively). Together with `tiles_cancelled` it PARTITIONS the unsettled entries at
  // expiry, which is the point: without it, "the sweep cancelled nothing" is
  // indistinguishable from "the deadline never expired", "the tile settled in time", and
  // "someone deleted the sweep" -- and `in_flight_tile_dedup` shipped a guard that
  // provably never fired precisely because every assertion on it was of the "a number did
  // not grow" kind (its Decision 5). This is the positive witness that the sweep looked at
  // the queue and made a decision.
  std::uint64_t tiles_retained() const noexcept { return d_tiles_retained; }

  // The frame's leaf-miss executor and async-completion park/wake substrate.
  // Exposed so externally-async content can `poke()` a render thread parked in this
  // loop's `wait_completions`.
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

  // DECLARATION ORDER IS LOAD-BEARING: `d_pending` before `d_owned_pool`, so an OWNED
  // pool destructs FIRST and `~WorkerPool` stops and joins its threads while the pending
  // surfaces those workers may still be writing are all alive. Since
  // `runtime.worker_dispatch_leaf_only` wired `worker_backed_dispatch(d_pool, this)` into
  // the frame, a `worker_count > 0` renderer can be destroyed with real renders in
  // flight; reversing these two is a use-after-free, not a style choice.
  //
  // That argument covers only the OWNED case. A BORROWED pool is not destructed when this
  // renderer dies, so no join happens here at all and a worker can still be inside
  // `content->render`, writing into a `PendingTile`'s surface, when `d_pending` goes. What
  // covers the borrowed case is `~InteractiveRenderer`'s BODY, which calls
  // `d_pool.drain_owner(this)`: a destructor body runs before ANY member is destroyed, so
  // it drains ahead of `d_pending` unconditionally -- for the borrowed case, where nothing
  // else would, and harmlessly for the owned case, where `~WorkerPool` would have done it a
  // moment later. It is a statement in the body rather than an RAII lease member precisely
  // because a lease member would have to be declared BEFORE `d_pending` to drain in time,
  // inverting the rule above: two opposite member-order rules in one class, each a
  // use-after-free if broken (`runtime.shared_worker_pool` Decision 4).
  RefinementQueue d_pending;     // the frame-to-frame registry of async renders
  CompositorCounters d_counters; // persistent behavioral counts across frames
  // The pool, owned or borrowed. `d_owned_pool` is engaged exactly when this renderer
  // built its own; `d_pool` binds to it or to the host's, so every use site just says
  // `d_pool` and branches on nothing. A reference member rather than a variant because
  // `WorkerPool` is non-movable, and because the only thing any call site wants is *the
  // pool* (Decision 2).
  std::optional<WorkerPool> d_owned_pool;
  WorkerPool& d_pool; // leaf-miss executor + async-completion park/wake
  // THIS renderer's drain cursor into the pool's settle counter (Decision 1). Caller-owned
  // so that a sibling renderer parked on the same pool cannot consume the settle produced
  // by one of OUR tiles -- and cannot have one of its own consumed by us. Seeded from the
  // pool's current generation at construction, so joining a busy pool does not hand the
  // first frame a free spurious return.
  CompletionCursor d_cursor;
  Clock d_clock; // the loop's only wall-clock read
  // The stale probe's PER-CONTENT priors (Decision 7): for each content this renderer has
  // composited, the `layer_revision` the last frame that planned it keyed it under --
  // the aggregate for an operator layer, the leaf stamp otherwise, taken straight off the
  // plan the frame composited (`LayerTilePlan::revision`), which is what keeps the two
  // cases uniform. Updated, never cleared: a layer culled this frame keeps the stamp it
  // was last composited under, which is a legitimate prior rendering of that content and
  // therefore a legitimate stale tier. It replaces the single document-global scalar,
  // which per-object stamps make meaningless.
  PriorStamps d_prior_stamps;
  std::optional<Time> d_prev_time; // previous composition time (clock advance)
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
  // This frame's WANTED TILE SET (`runtime.deadline_cancel_retains_wanted`): the visible
  // footprint the compositor fills at Step 4, plus every tile the frame's pulls named,
  // and the only thing Step 5's deadline sweep tests a pending tile against before
  // cancelling it. Frame-scoped -- `clear()`ed at the top of Step 4, read at Step 5, and
  // never carried -- but held as a MEMBER rather than a frame local so its hash buckets
  // survive and a 60Hz loop does not build a table per frame. It is a pure function of
  // one frame's plan inputs (viewport, camera, revision, composition time, layer culls),
  // so nothing can invalidate it while it is alive, and it holds keys only -- no surface,
  // no completion -- so it takes no part in the destruction order below.
  WantedTiles d_wanted_tiles;

  // The per-revision pull-identity memo. `make_pull_identity_of` walks the whole
  // reachable content graph, which the deadline-bounded loop must not pay every
  // frame; within one revision the pinned graph is immutable, so the memo is exact,
  // and across a bump every tile key changes anyway, so a shifted synthesized id can
  // never serve a stale hit. Keying on the revision alone is sound under exactly the
  // single-document-per-renderer assumption this loop has always relied on.
  //
  // The per-object revision CONTRIBUTION column rides the same memo
  // (`model.per_object_revision` Decision 4): one more walk-free pass over the identity
  // map on the frame thread, per revision, handed to the workers as a read-only
  // snapshot. Eager-and-immutable rather than lazily filled, because workers pull
  // concurrently and a lazy memo would need mutable state and a lock on exactly the
  // hot path.
  std::optional<std::uint64_t> d_identity_revision;      // the revision the memo was built at
  std::shared_ptr<const PullIdentityMap> d_identity_map; // Content* -> ObjectId
  std::shared_ptr<const PullStampMap> d_stamp_map;       // ObjectId -> revision contribution
  std::function<ObjectId(const Content*)> d_id_of;       // PullConfig::id_of over it
  RevisionContribution d_contribution;                   // PullConfig::contribution over both
  std::unordered_map<ObjectId, const Content*> d_content_by_id; // the inverse, for routing
  std::vector<OperatorLayer> d_operator_layers;                 // the visible operator layers
  std::uint64_t d_identity_map_builds{0};                       // memo (re)builds, a counter
  std::uint64_t d_operator_binds{0};                            // per-rendering-frame binds
  std::uint64_t d_frames_rendered{0};   // frames past the still-scene early-out
  std::uint64_t d_deadline_expiries{0}; // parks that reached the deadline
  std::uint64_t d_tiles_cancelled{0};   // unwanted pendings the expired frame cancelled
  std::uint64_t d_tiles_retained{0};    // still-wanted pendings it left in flight
};

} // namespace arbc
