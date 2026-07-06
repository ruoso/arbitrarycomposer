#pragma once

#include <arbc/base/ids.hpp>
#include <arbc/cache/key_shapes.hpp>
#include <arbc/cache/keyed_store.hpp>
#include <arbc/compositor/counters.hpp>
#include <arbc/compositor/operator_graph.hpp>
#include <arbc/compositor/refinement.hpp> // RefinementQueue / PendingTile (the async sink)
#include <arbc/contract/content.hpp>      // PullService, Content, RenderRequest, RenderCompletion
#include <arbc/surface/backend.hpp>

#include <cstdint>
#include <functional>
#include <memory>

// The concrete `PullService` implementation for `arbc::compositor` (L4, doc
// 17:56) -- "the one genuinely new core API" (doc 13:85). The abstract seam is
// contract's (`content.hpp:314-340`); this is the request/cache/snapshot/budget
// engine behind it (doc 13:69-89): probe the tile cache first, serve a resident
// hit synchronously with zero dispatch, dispatch a miss onto an injected worker
// seam carrying the request's snapshot and deadline unchanged, and settle the
// completion from the result -- exactly the machinery `render_frame_interactive`'s
// miss-fill does inline today, lifted into a service any content (an operator, a
// nested composition, or the frame driver itself) can call (doc 13:173-177,
// "Mostly *moves* the doc-05 synthetic-viewport internals behind a public seam").
//
// Levelization (doc 17:56): `arbc::compositor` may reach only `contract` and
// `cache` (+ transitive `surface`). The worker path enters ONLY through the
// injected `RenderDispatch` functor, whose signature is over `contract` types
// (`Content*`, `RenderRequest`, `RenderCompletion`) alone -- so this file names
// no `runtime` type (`WorkerPool`, `RenderTask`). Runtime (L5) binds that functor
// to `WorkerPool::submit` and performs the attach-time injection (Decision 2,
// `worker_pool.hpp:41`). No new DEPENDS edge; no upward edge.

namespace arbc {

// The worker-dispatch seam (doc 17:56 vs 60, `worker_pool.hpp:41` "a
// `std::function` over `submit`"). Over `contract` types only, so the compositor
// never names a `runtime` type. A dispatch renders `content` for `request` and
// settles `done` exactly as `Content::render` does: inline (synchronously, the
// completion is settled before the call returns) or off-thread (the completion
// is left live and settled later on a worker / by externally-async content).
using RenderDispatch =
    std::function<void(Content*, const RenderRequest&, std::shared_ptr<RenderCompletion>)>;

// The default single-threaded dispatch (Decision 3): call `content->render`
// inline and fold a returned-inline `RenderResult` through `done`, exactly as the
// pre-task driver's miss-fill did (`tile_planning.cpp:349-350`). This keeps the
// `render_frame_interactive` null path byte-for-byte identical to today; runtime
// swaps in a worker-backed dispatch opt-in.
RenderDispatch direct_dispatch();

// The per-frame hooks a `PullServiceImpl` threads, all caller-owned and defaulted
// null/identity so the engine mirrors the pure-seam posture of every compositor
// sibling (`CompositorCounters*` / `RefinementQueue*` / `GraphDiagnostics*` are
// the same optional-pointer discipline the driver already uses).
struct PullConfig {
  // Behavioral counters (doc 16:54-62): `requests_issued` bumps once per
  // dispatched render, `operator_renders` once per dispatched *operator* render.
  CompositorCounters* counters{nullptr};
  // The async sink (doc 02:69-71 step 6). A dispatched render that answers
  // asynchronously is recorded here (surface + completion travel with it) so a
  // later `poll_refinements` inserts it under `Visible` and emits damage. Null
  // drops the async miss exactly as the driver's null-`pending` path does.
  RefinementQueue* pending{nullptr};
  // The recursion-depth cycle backstop sink (doc 05:66-70). A descent exceeding
  // `budget.max_depth` reports one diagnostic naming the content path here.
  GraphDiagnostics* diagnostics{nullptr};
  // The per-request recursion-depth budget threaded through the descent, never
  // reset by it (doc 05:96-100). Bounds a divergent operator-over-operator pull.
  GraphBudget budget{};
  // `Content*` -> `ObjectId`: the input's cache identity (doc 13:126, "input
  // tiles cache under the input's identity"). The `pull` seam receives a raw
  // `Content*` (`content.hpp:161`) carrying no id, so the caller (the runtime
  // content binding; a test stub) supplies the reverse map. Empty -> the default
  // (root) id.
  std::function<ObjectId(const Content*)> id_of{};
  // Per-node revision contribution for `aggregate_revision` (doc 05:82-91): the
  // driver passes the document-global `state.revision()` for every node
  // (`operator_graph` Decision 3). Empty -> `0` (never stale; keys collapse).
  std::function<std::uint64_t(const Content*)> contribution{};
};

// The concrete L4 pull engine (Decision 1). Injected as a `PullService*` to
// content at attach; the frame driver uses its concrete `dispatch` seam directly
// (see `dispatch` below). Holds the frame's `TileCache&`, a `Backend&` (to
// allocate cache-destined tile surfaces), the injected `RenderDispatch`, and the
// per-frame `PullConfig` hooks. Not copyable (it references frame state).
class PullServiceImpl final : public PullService {
public:
  PullServiceImpl(TileCache& cache, Backend& backend, RenderDispatch dispatch, PullConfig config);

  PullServiceImpl(const PullServiceImpl&) = delete;
  PullServiceImpl& operator=(const PullServiceImpl&) = delete;

  // Render `input`'s tile for `request`, cache-first (doc 13:69-89, the
  // `pull-is-cache-first` claim). Derive the input's tile key from the request
  // (id via `PullConfig::id_of`; revision via `aggregate_revision` for an
  // operator input, else its own contribution; rung/coord from
  // `request.scale`/`request.region`; achieved_time from the input's stability +
  // `quantize_time`). A resident exact fresh hit completes `done` synchronously
  // and dispatches NO render. A miss dispatches exactly one render carrying the
  // request's `snapshot` and `deadline` verbatim, inserts the result tile under
  // the input's identity at `Visible`, and settles `done`; an async render is
  // recorded into `PullConfig::pending` (occupying no worker) and inserted on a
  // later `poll_refinements`. An operator input resolves its `identity()` chain
  // first: a pass-through serves the terminal input's tiles (no operator render,
  // no operator-output cache entry); a descent exceeding the recursion budget
  // selects the placeholder (`done->fail`) and reports one diagnostic. This is
  // the single-tile pull seam -- a caller pulling a multi-tile region decomposes
  // it per tile exactly as the driver plans per tile.
  void pull(ContentRef input, const RenderRequest& request,
            std::shared_ptr<RenderCompletion> done) override;

  // The frame driver's dispatch-only seam (Decision 3). Invokes the injected
  // `RenderDispatch` to render `content` for `request`, settling `done` inline or
  // off-thread. The driver keeps its own plan-time cache probe, key, tile-surface
  // ownership, insert, and async recording -- delegating ONLY the render call --
  // so the null path (an internal `direct_dispatch` engine) stays byte-for-byte
  // identical to the pre-task inline fill (no re-probe, no double insert). Does
  // NOT touch the cache or bump counters (the driver owns those on this path).
  void dispatch(Content* content, const RenderRequest& request,
                std::shared_ptr<RenderCompletion> done);

private:
  TileCache& d_cache;
  Backend& d_backend;
  RenderDispatch d_dispatch;
  PullConfig d_config;
  // The synchronous operator-descent depth (Decision 4/5): operator recursion is
  // evaluated on the frame/calling thread within `budget`, so this is frame-
  // thread-confined (never touched by a worker -- workers only run a leaf
  // `render` into a thread-confined target). Incremented across a nested pull so
  // a divergent operator-over-operator descent hits the budget backstop.
  unsigned d_depth{0};
};

} // namespace arbc
