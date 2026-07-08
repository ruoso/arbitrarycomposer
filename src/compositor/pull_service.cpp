#include <arbc/base/expected.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/compositor/operator_graph.hpp>
#include <arbc/compositor/provided_surface.hpp>
#include <arbc/compositor/pull_service.hpp>
#include <arbc/compositor/scale_ladder.hpp>
#include <arbc/compositor/tile_planning.hpp> // k_tile_size, tiles_covering
#include <arbc/media/audio_block.hpp>        // channel_count

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace arbc {

RenderDispatch direct_dispatch() {
  // The single settle path (doc 03:117-121), exactly as `render_frame`'s inline
  // fill: drive `render` inline and fold a returned-inline result through `done`.
  // A `nullopt` return leaves `done` live for a later off-thread settle (the
  // async path), so the caller detects inline-vs-async via `done->settled()`.
  return
      [](Content* content, const RenderRequest& request, std::shared_ptr<RenderCompletion> done) {
        const std::optional<RenderResult> inline_result = content->render(request, done);
        if (inline_result.has_value()) {
          done->complete(*inline_result);
        }
      };
}

AudioDispatch direct_audio_dispatch() {
  // The audio twin of `direct_dispatch`: drive `render_audio` inline and fold a
  // returned-inline result through `done`; a content with no audio facet fails
  // once, a `nullopt` return leaves `done` live for a later off-thread settle.
  return [](Content* content, const AudioRequest& request, std::shared_ptr<AudioCompletion> done) {
    AudioFacet* facet = content != nullptr ? content->audio() : nullptr;
    if (facet == nullptr) {
      done->fail(RenderError::ResourceUnavailable);
      return;
    }
    const std::optional<AudioResult> inline_result = facet->render_audio(request, done);
    if (inline_result.has_value()) {
      done->complete(*inline_result);
    }
  };
}

std::int64_t audio_block_index(const AudioRequest& request) {
  if (request.sample_rate == 0) {
    return 0;
  }
  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(request.sample_rate);
  if (fpf <= 0) {
    return 0;
  }
  return request.window.start.flicks / fpf;
}

PullServiceImpl::PullServiceImpl(TileCache& cache, Backend& backend, RenderDispatch dispatch,
                                 PullConfig config)
    : d_cache(cache), d_backend(backend), d_dispatch(std::move(dispatch)),
      d_config(std::move(config)) {}

void PullServiceImpl::dispatch(Content* content, const RenderRequest& request,
                               std::shared_ptr<RenderCompletion> done) {
  d_dispatch(content, request, std::move(done));
}

namespace {

// Settle `done` with the placeholder policy (doc 05:66-70, doc 13:134-138): a
// budget-exceeded / null descent produces no pixels, signalled as an
// unavailable-resource failure the caller degrades to its placeholder display.
void settle_placeholder(const std::shared_ptr<RenderCompletion>& done) {
  if (done) {
    done->fail(RenderError::ResourceUnavailable);
  }
}

// The audio twin: a budget-exceeded / unavailable audio pull produces no samples,
// signalled as an unavailable-resource failure the mix engine mixes as silence
// for the layer this pass (doc 05:66-70, doc 12:31-34).
void settle_placeholder_audio(const std::shared_ptr<AudioCompletion>& done) {
  if (done) {
    done->fail(RenderError::ResourceUnavailable);
  }
}

} // namespace

void PullServiceImpl::pull(ContentRef input, const RenderRequest& request,
                           std::shared_ptr<RenderCompletion> done) {
  if (input == nullptr) {
    if (done) {
      done->fail(RenderError::ContentFailed);
    }
    return;
  }

  // Recursion-depth backstop (doc 05:66-70). Checked at entry so a divergent
  // operator-over-operator descent -- each hop re-entering `pull` under an
  // incremented `d_depth` -- terminates on the budget with the placeholder plus
  // exactly one diagnostic naming the offending content, never unbounded
  // recursion (`graph-walk-bounds-cycles`). The budget is inherited, never reset
  // per pull (doc 05:96-100).
  if (d_depth >= d_config.budget.max_depth) {
    if (d_config.diagnostics != nullptr) {
      d_config.diagnostics->entries.push_back(GraphDiagnostic{input, d_depth, {input}});
    }
    settle_placeholder(done);
    return;
  }

  const bool op = is_operator(input);

  // Operator identity short-circuit (doc 13:59-65,128): a pass-through operator
  // (a fade at envelope == 1) serves the terminal input's cached tiles directly
  // -- no operator render, no operator-output cache entry -- so the descent
  // resolves to the terminal and pulls *it* instead. A divergent identity cycle
  // exceeds the budget inside `resolve_identity`, which reports the one
  // diagnostic; we degrade to the placeholder without a second report.
  if (op) {
    const IdentityResolution ident =
        resolve_identity(input, request, d_config.budget, d_config.diagnostics);
    if (ident.budget_exceeded) {
      settle_placeholder(done);
      return;
    }
    if (ident.short_circuited && ident.terminal != nullptr) {
      ++d_depth;
      pull(const_cast<Content*>(ident.terminal), request, std::move(done));
      --d_depth;
      return;
    }
  }

  // Derive the input's fresh tile key from the request (doc 13:124-126). An
  // operator input keys on its whole reachable graph via the landed
  // `aggregate_revision` fold; a leaf degenerates to its own contribution
  // (`operator_graph.hpp:105-107`, Decision 6). Rung/coord come from the request
  // scale/region through the same ladder the planner uses (`select_rung`,
  // `tiles_covering`); achieved_time follows the input's stability +
  // `quantize_time`, mirroring `plan_layer` so the pull keys the tile the planner
  // would (`tile_planning.cpp:139-142`).
  const ObjectId id = d_config.id_of ? d_config.id_of(input) : ObjectId{};
  const std::uint64_t revision =
      op ? aggregate_revision(
               input,
               [this](const Content* node) {
                 return d_config.contribution ? d_config.contribution(node) : std::uint64_t{0};
               },
               d_config.budget)
         : (d_config.contribution ? d_config.contribution(input) : std::uint64_t{0});

  const RungSelection selection = select_rung(request.scale);
  const double rung_px = rung_scale(selection.rung);
  const std::vector<TileCoord> coords = tiles_covering(selection.rung, request.region);
  const TileCoord coord = coords.empty() ? TileCoord{} : coords.front();

  const Stability stability = input->stability();
  const Time key_time = input->quantize_time(request.time).value_or(request.time);
  const std::optional<Time> achieved_time =
      (stability == Stability::Static) ? std::nullopt : std::optional<Time>(key_time);
  const TileKey key{id, revision, selection.rung, coord, achieved_time};

  // Cache lookup first (doc 13:76-77, `pull-is-cache-first`). A resident exact
  // fresh hit -- exact at this rung, the planner's qualification -- completes
  // `done` synchronously from the cached metadata and dispatches NO render: a
  // warm pull issues zero work.
  if (std::optional<CacheHold<TileValue>> hit = d_cache.lookup(key);
      hit.has_value() && hit->get().meta.exact && hit->get().meta.achieved_scale == rung_px) {
    if (done) {
      done->complete(
          RenderResult{hit->get().meta.achieved_scale, hit->get().meta.exact, achieved_time});
    }
    return;
  }

  // Miss: build the render descriptor and dispatch it. The tile pixels are owned
  // by the cache (`TileValue` owns its `Surface`), so the render targets a
  // freshly-allocated cache-destined surface -- not the caller's `request.target`
  // -- which travels into the cache (inline) or the pending queue (async). If the
  // allocation fails this pass, degrade to the placeholder.
  arbc::expected<std::unique_ptr<Surface>, SurfaceError> owned =
      d_backend.make_surface(k_tile_size, k_tile_size, request.target.format());
  if (!owned.has_value()) {
    settle_placeholder(done);
    return;
  }
  Surface& tile_surface = **owned;
  d_backend.clear(tile_surface, 0.0F, 0.0F, 0.0F, 0.0F);

  // Snapshot and deadline carried verbatim (doc 05:96-100,
  // `pull-inherits-snapshot-and-deadline`): the dispatched render's `snapshot`
  // `StateHandle` and `deadline` value EQUAL the pull request's -- neither reset
  // nor recomputed, no per-pull sub-budget. Only the target is swapped to the
  // cache surface.
  const RenderRequest render_request{request.region,   request.scale, request.time,
                                     request.snapshot, tile_surface,  request.exactness,
                                     request.deadline};

  // A dispatched render is counted whether it answers inline or defers (both
  // issued the render, Decision 5); an operator render also bumps
  // `operator_renders` (the counter `operator_graph` established).
  if (d_config.counters != nullptr) {
    d_config.counters->note_request_issued();
    if (op) {
      d_config.counters->note_operator_render();
    }
  }

  // The descent depth is in effect across the dispatch so an operator whose
  // `render` recursively pulls its own input re-enters `pull` at `d_depth + 1`.
  ++d_depth;
  d_dispatch(input, render_request, done);
  --d_depth;

  if (done && done->settled()) {
    const std::optional<expected<RenderResult, RenderError>> settled = done->take();
    if (settled.has_value() && settled->has_value()) {
      RenderResult result = **settled;
      // Insert-key temporal linkage (doc 11:134-137): the pull keyed this tile at
      // `quantize_time(request.time)` before dispatching; assert the render landed
      // on that same instant so the cached tile serves the frame it is keyed under
      // (a no-op for conformant content, fires only on a doc-11 MUST violation).
      assert(timed_insert_key_consistent(key, result, stability));
      // Honor a content-provided surface (doc 09:87-100): copy it into the
      // cache-owned `tile_surface` (cleared to transparent above, so a source-over
      // copy is exact) and release it -- the cache never learns the surface was
      // provided (doc 09:109-112,328-340). Absent, the content filled
      // `tile_surface` and there is nothing to copy.
      consume_render_result(result, tile_surface, [&](const Surface& src) {
        if (&src != &tile_surface) {
          d_backend.composite(tile_surface, src, Affine::identity(), 1.0);
        }
      });
      const std::size_t bytes = tile_byte_cost(tile_surface);
      d_cache.insert(key, TileValue{std::move(*owned), {result.achieved_scale, result.exact}},
                     bytes, PriorityClass::Visible);
    }
    // A settled-via-fail inline render is dropped: no insert, no pending -- the
    // caller sees the failure on `done` and degrades to the placeholder.
  } else if (d_config.pending != nullptr) {
    // Async (doc 02:69-71 step 6, doc 13:128-130): the render answered
    // asynchronously (the completion is still live). Record it -- the cache-owned
    // surface and the completion travel with it -- so a later `poll_refinements`
    // inserts it under `Visible` and emits damage; the render occupies no worker
    // while pending. The cache stays single-writer on the draining frame thread
    // (Decision 4): only `content->render` into the thread-confined target ran on
    // a worker. `done` was NOT taken above, so it is the live completion the
    // worker settles and `poll_refinements` drains.
    const std::size_t bytes = tile_byte_cost(tile_surface);
    d_config.pending->tiles.push_back(
        PendingTile{key, request.region, id, stability, bytes, std::move(*owned), std::move(done)});
  }
}

void PullServiceImpl::pull_audio(ContentRef input, const AudioRequest& request,
                                 std::shared_ptr<AudioCompletion> done) {
  if (input == nullptr) {
    if (done) {
      done->fail(RenderError::ContentFailed);
    }
    return;
  }

  // Recursion-depth backstop (doc 05:66-70), shared `d_depth`/`budget` with `pull`
  // so a gain>=1 self-embedding audio cycle terminates on the budget one dimension
  // over (doc 12:143): a nested composition's `render_audio`, dispatched below,
  // re-enters `pull_audio` under an incremented `d_depth`. The budget is inherited,
  // never reset per pull (doc 05:96-100).
  if (d_depth >= d_config.budget.max_depth) {
    if (d_config.diagnostics != nullptr) {
      d_config.diagnostics->entries.push_back(GraphDiagnostic{input, d_depth, {input}});
    }
    settle_placeholder_audio(done);
    return;
  }

  // Cache-first probe on the 1D block key `(content id, revision, block index,
  // rate)` (doc 12:169-185, `12-audio#block-cache-is-tile-cache-1d`). The audio
  // revision space is the visual one (doc 12:208), so the revision folds exactly
  // as `pull`'s: an operator input over its reachable graph via `aggregate_revision`,
  // a leaf on its own contribution. A resident exact-fresh block (exact + at the
  // request rate, matching the request's frame count and layout) serves
  // synchronously with ZERO dispatch.
  if (d_config.blocks != nullptr) {
    const ObjectId id = d_config.id_of ? d_config.id_of(input) : ObjectId{};
    const bool op = is_operator(input);
    const std::uint64_t revision =
        op ? aggregate_revision(
                 input,
                 [this](const Content* node) {
                   return d_config.contribution ? d_config.contribution(node) : std::uint64_t{0};
                 },
                 d_config.budget)
           : (d_config.contribution ? d_config.contribution(input) : std::uint64_t{0});
    const BlockKey key{id, revision, audio_block_index(request), request.sample_rate};

    if (std::optional<CacheHold<AudioBlockValue>> hit = d_config.blocks->lookup(key);
        hit.has_value() && hit->get().meta.exact &&
        hit->get().meta.achieved_rate == request.sample_rate &&
        hit->get().frames == request.target.frames && hit->get().layout == request.target.layout) {
      const AudioBlockValue& value = hit->get();
      const std::size_t n = static_cast<std::size_t>(value.frames) * channel_count(value.layout);
      if (request.target.samples != nullptr && value.samples.size() >= n) {
        for (std::size_t i = 0; i < n; ++i) {
          request.target.samples[i] = value.samples[i];
        }
      }
      if (done) {
        done->complete(value.meta);
      }
      return;
    }
  }

  // Miss: dispatch exactly once onto the audio worker seam, carrying the request's
  // snapshot / exactness / rate verbatim (doc 12:31-34). No block-cache *fill*
  // here -- the prefetch-ring fill is `audio.lookahead`'s: `runtime::LookaheadPump`
  // renders the ring's want-list on the audio worker pool and inserts the blocks so
  // primed pulls hit above (doc 12:183-190). An unconfigured audio worker has no
  // audio pull, so it settles the placeholder, exactly as the base `PullService`
  // stub does (`content.cpp:19-26`).
  if (!d_config.audio_dispatch) {
    settle_placeholder_audio(done);
    return;
  }
  if (d_config.counters != nullptr) {
    d_config.counters->note_audio_dispatch();
  }

  // The descent depth is in effect across the dispatch so a nested composition
  // whose `render_audio` recursively pulls its child layers re-enters `pull_audio`
  // at `d_depth + 1`.
  ++d_depth;
  d_config.audio_dispatch(input, request, std::move(done));
  --d_depth;
}

} // namespace arbc
