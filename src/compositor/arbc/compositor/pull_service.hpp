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
#include <vector>

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

// The audio worker-dispatch seam (doc 12:31-34,154-164): the audio twin of
// `RenderDispatch`. A dispatch renders `content`'s audio for `request` and
// settles `done` exactly as `AudioFacet::render_audio` does -- inline
// (synchronously) or off-thread. Over `contract` types only, so the compositor
// still names no `runtime` type; runtime binds it to the audio worker path when
// the block pipeline lands (doc 17:57). Because audio "renders ahead" and
// arbitrary `render_audio` plugin code must be dispatchable off the RT callback,
// the mix engine pulls every layer through this seam and never calls
// `render_audio` inline.
using AudioDispatch =
    std::function<void(Content*, const AudioRequest&, std::shared_ptr<AudioCompletion>)>;

// The default single-threaded audio dispatch: call `content->audio()->render_audio`
// inline and fold a returned-inline `AudioResult` through `done` (the audio twin
// of `direct_dispatch`). A content with no audio facet fails `done` once
// (`ResourceUnavailable`); a `nullopt` return leaves `done` live for a later
// off-thread settle. Lets the mix engine drive real per-content audio through
// `PullServiceImpl::pull_audio` synchronously in a test / single-threaded monitor.
AudioDispatch direct_audio_dispatch();

// The 1D audio block-cache value (doc 12:169-170, the block-cache-is-tile-cache-1d
// twin of `TileValue`): an owned interleaved-float32 sample block plus its shape
// and the `AudioResult` metadata a hit serves. Move-only via its `std::vector`
// (the `KeyedStore` `Value` requirement); the eventual owning home is
// `arbc::audio-engine`'s block pipeline (doc 12; `key_shapes.hpp:78-90`), but the
// concrete `pull_audio` that reads it lives here -- and an L4 peer cannot be a
// DEPENDS edge -- so the value + `KeyedStore<BlockKey, ...>` instantiation ride
// with the concrete service for now.
struct AudioBlockValue {
  std::vector<float> samples;                  // interleaved float32, block-cache-owned
  std::uint32_t frames{0};                     // number of sample frames
  ChannelLayout layout{ChannelLayout::Stereo}; // interleaving of `samples`
  std::uint32_t rate{0};                       // sample rate in Hz
  AudioResult meta{};                          // achieved_rate / exact of the block
};

// The concrete 1D block cache `pull_audio` probes -- the tile cache with a 1D key
// (doc 12:169-185, `12-audio#block-cache-is-tile-cache-1d`). Instantiated over the
// shared `KeyedStore` machinery, exactly as `TileCache` is (`key_shapes.hpp:129`).
using BlockCache = KeyedStore<BlockKey, AudioBlockValue>;

// The 1D block index a request's window start maps to at its working rate: the
// sample-frame index `floor(window.start / (flicks_per_second / rate))`. The
// block key's temporal axis (doc 12:169-170); `audio.lookahead`'s
// `LookaheadRing`/`LookaheadPump` own the fixed block *size* and the prepared-
// block ring above this per-window-start key. `rate == 0` yields index 0 (a
// degenerate request).
std::int64_t audio_block_index(const AudioRequest& request);

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
  // The frame's wanted-tile sink (`runtime.deadline_cancel_retains_wanted`, doc 02
  // § The frame, interactively). Every covering tile key a `pull` NAMES is inserted
  // here -- hit, in-flight join, wave deferral or fresh dispatch alike -- because an
  // operator's input leaves are not layers and so appear in no plan and in no visible
  // footprint. Without this the frame's wanted set would omit exactly the population the
  // deadline sweep most needs to retain. Sharing the driver's sink is what makes the two
  // producers one set. Null (the offline driver, every one-shot renderer, neither of
  // which sweeps) records nothing and is behavior-identical.
  WantedTiles* wanted{nullptr};
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
  // The audio worker-dispatch seam `pull_audio` dispatches a block-cache miss onto
  // (doc 12:31-34). Empty -> a miss has no audio worker, so `pull_audio` settles
  // the placeholder (`ResourceUnavailable`), exactly as the base `PullService`
  // stub does for a service that predates audio (`content.cpp:19-26`).
  AudioDispatch audio_dispatch{};
  // The 1D block cache `pull_audio` probes cache-first (doc 12:169-185). Null ->
  // every `pull_audio` is a miss (no fill here -- the prefetch-ring fill is
  // `audio.lookahead`'s: `runtime::LookaheadPump` renders the ring's want-list on
  // the audio worker pool and inserts the blocks, so a primed pull hits here).
  BlockCache* blocks{nullptr};
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
  // selects the placeholder (`done->fail`) and reports one diagnostic. A request
  // whose `region` spans more than one tile is served across EVERY covering tile
  // of `tiles_covering(rung, region)` -- each independently keyed, probed,
  // rendered, and delivered into its own sub-rect of `target` -- and `done`
  // settles once from the aggregate: exact iff every covering tile is exact, with
  // the uniform rung scale and achieved_time; any covering tile answering
  // asynchronously leaves `done` unsettled so the operator degrades this frame
  // (`pull-fills-multi-tile-region`, doc 13:91-101).
  void pull(ContentRef input, const RenderRequest& request,
            std::shared_ptr<RenderCompletion> done) override;

  // Render `input`'s audio for `request`, cache-first (doc 12:169-185, the audio
  // arm of `pull` and the concrete home the `content.cpp:19-26` stub named). Probe
  // the 1D block cache on `(content id, revision, block index, rate)`: a resident
  // exact-fresh block completes `done` synchronously with ZERO dispatch; a miss
  // dispatches exactly one audio render onto `PullConfig::audio_dispatch` carrying
  // the request's `snapshot`/`exactness`/`rate` verbatim and settles `done` from
  // it (no block-cache *fill* here -- that is `audio.lookahead`'s). The shared
  // recursion-depth budget backstop applies (a depth-exceeded pull settles the
  // placeholder, doc 05:61-67). `done` settles EXACTLY ONCE on every path. A null
  // input fails once; an unconfigured audio worker settles the placeholder.
  void pull_audio(ContentRef input, const AudioRequest& request,
                  std::shared_ptr<AudioCompletion> done) override;

  // The frame driver's dispatch-only seam (Decision 3). Invokes the injected
  // `RenderDispatch` to render `content` for `request`, settling `done` inline or
  // off-thread. The driver keeps its own plan-time cache probe, key, tile-surface
  // ownership, insert, and async recording -- delegating ONLY the render call --
  // so the null path (an internal `direct_dispatch` engine) stays byte-for-byte
  // identical to the pre-task inline fill (no re-probe, no double insert). Does
  // NOT touch the cache or bump counters (the driver owns those on this path).
  void dispatch(Content* content, const RenderRequest& request,
                std::shared_ptr<RenderCompletion> done);

  // The UNMET-INPUT accumulator (`compositor.operator_refinement_wave_amplification`,
  // Decision 2). Only the pull service knows which input tiles an operator's render
  // is waiting on: they are exactly the tiles the pull left unmet. Each `pull`
  // appends to `d_unmet` at the three places it fails to deliver a covering tile --
  // the async dispatch record, the in-flight join, and the wave-deferral arm -- and
  // a RENDER site brackets its `render` call with these:
  //
  //     const std::size_t mark = pulls->unmet_mark();
  //     ... drive content->render ...
  //     if (!result.exact) record_operator_wait(*pending, key, pulls->unmet_since(mark));
  //
  // `unmet_since` COPIES without erasing, and that is what makes the set transitive
  // through a nested chain with no graph walk and no `inputs()` traversal. Because
  // worker dispatch is leaf-only (doc 02:220-233) an operator renders inline on this
  // thread, so a nested composition's render pulls its child fades, each of which
  // renders inline and pulls its own leaf: the fade's own wait -- recorded by the
  // pull's render site -- gets the tail since ITS mark (just its leaf), while the
  // nested tile's wait -- recorded by the driver -- gets the tail since ITS mark (the
  // union of the whole subtree). Each level waits on exactly the leaves beneath it.
  //
  // Frame-thread-confined by that same leaf-only rule (Constraint 5): a worker only
  // ever runs a leaf `render` into a thread-confined target and never re-enters
  // `pull`, so this needs no lock, no atomic, and no new shared state.
  std::size_t unmet_mark() const noexcept;
  std::vector<TileKey> unmet_since(std::size_t mark) const;
  // Drop the accumulated tail. The driver calls this at the top of each output tile
  // it drives, so the accumulator never grows across the frame's tiles.
  void unmet_clear() noexcept;

private:
  TileCache& d_cache;
  Backend& d_backend;
  RenderDispatch d_dispatch;
  PullConfig d_config;
  // The frame-scoped unmet-input tail (see `unmet_mark`). Frame-thread-only.
  std::vector<TileKey> d_unmet;
  // The synchronous operator-descent depth (Decision 4/5): operator recursion is
  // evaluated on the frame/calling thread within `budget`, so this is frame-
  // thread-confined (never touched by a worker -- workers only run a leaf
  // `render` into a thread-confined target). Incremented across a nested pull so
  // a divergent operator-over-operator descent hits the budget backstop.
  unsigned d_depth{0};
};

} // namespace arbc
