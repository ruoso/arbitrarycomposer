#include <arbc/base/expected.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/compositor/operator_graph.hpp>
#include <arbc/compositor/provided_surface.hpp>
#include <arbc/compositor/pull_service.hpp>
#include <arbc/compositor/refinement.hpp> // tile_in_flight (the pending-set suppression key)
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

std::size_t PullServiceImpl::unmet_mark() const noexcept { return d_unmet.size(); }

std::vector<TileKey> PullServiceImpl::unmet_since(std::size_t mark) const {
  if (mark >= d_unmet.size()) {
    return {};
  }
  return std::vector<TileKey>(d_unmet.begin() + static_cast<std::ptrdiff_t>(mark), d_unmet.end());
}

void PullServiceImpl::unmet_clear() noexcept { d_unmet.clear(); }

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

// Deliver the covering tile's pixels into the caller's `request.target`
// (doc 13, `pull-delivers-to-caller-target`): the visual analog of `pull_audio`
// copying the resident block into `request.target.samples` (`:317-319`). The
// tile is cached at tile granularity (`k_tile_size` px) at rung scale `rung_px`
// and grid `coord`; `request.target` holds `request.region` at `request.scale`.
// Delivery composites the tile into `target` under the tile->region affine
// `plan_layer` uses to composite tiles into a frame (`tile_planning.cpp:52-55,
// 104-135`): tile pixel -> local (`translate(tile origin) . scale(1/rung_px)`)
// chained with local -> target pixel (`scale(request.scale) .
// translate(-region origin)`). So delivery honors region and scale and never
// assumes `target` and the tile share dimensions or origin (Constraint 1). It is
// one `Backend::composite`, no dispatched render and no cache write
// (Constraints 2, 3) -- exactly the region/scale-honoring copy doc 09 mandates
// for a provided surface, in reverse (cache tile -> `target`).
void deliver_tile(Backend& backend, const Surface& tile, TileCoord coord, double rung_px,
                  const RenderRequest& request) {
  const double cell = static_cast<double>(k_tile_size) / rung_px;
  const double local_x0 = static_cast<double>(coord.col) * cell;
  const double local_y0 = static_cast<double>(coord.row) * cell;
  const Affine local_from_tile = compose(Affine::translation(local_x0, local_y0),
                                         Affine::scaling(1.0 / rung_px, 1.0 / rung_px));
  const Affine target_from_local =
      compose(Affine::scaling(request.scale, request.scale),
              Affine::translation(-request.region.x0, -request.region.y0));
  backend.composite(request.target, tile, compose(target_from_local, local_from_tile), 1.0);
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

  const Stability stability = input->stability();
  const Time key_time = input->quantize_time(request.time).value_or(request.time);
  const std::optional<Time> achieved_time =
      (stability == Stability::Static) ? std::nullopt : std::optional<Time>(key_time);

  // Serve EVERY covering tile of `request.region`, not just the first
  // (`pull-fills-multi-tile-region`, Decision 1). `coords` is the full covering
  // set at the selected rung; each tile is independently keyed / probed / rendered
  // / delivered -- the exact per-tile sequence `render_frame_interactive` runs
  // (`tile_planning.cpp:344`), with delivery into the caller's `target` added. The
  // tiles are walked SEQUENTIALLY on this frame/drain thread (Decision 5): the
  // cache stays single-writer, only each leaf `render` runs on a worker.
  //
  // The caller's `done` settles EXACTLY ONCE from the aggregate after the loop
  // (Decision 2): a region is exact only if every covering tile is exact at this
  // rung (`region_exact`, the AND fold matching doc 09's honest-exactness
  // folding); any tile answering asynchronously leaves the whole region unsettled
  // (`any_async`) so the operator degrades this frame and each async arrival
  // re-drives (doc 13's "async composes"); any tile failing inline degrades the
  // whole region to the placeholder immediately (a partially filled region is not
  // a correct operator input).
  bool region_exact = true;
  bool any_async = false;

  for (const TileCoord coord : coords) {
    // The input's fresh tile key for this covering tile (doc 13:124-126): identical
    // `{id, revision, rung, achieved_time}` across the region, differing only in
    // `coord`.
    const TileKey key{id, revision, selection.rung, coord, achieved_time};

    // The frame WANTS every tile it pulls (`runtime.deadline_cancel_retains_wanted`).
    // Recorded before the gates below, unconditionally, because all four outcomes -- a
    // resident hit, an in-flight join, a wave deferral, a fresh dispatch -- are the same
    // statement: this frame asked for this tile. An operator's input leaf is not a layer,
    // so it appears in no plan and in no visible footprint; this is the only place it
    // enters the wanted set, and it is exactly the population the deadline sweep must not
    // cancel out from under the operator that is waiting on it.
    if (d_config.wanted != nullptr) {
      d_config.wanted->insert(key);
    }

    // Cache lookup first (doc 13:76-77, `pull-is-cache-first`), per tile. A
    // resident exact fresh hit -- exact at this rung -- delivers the resident tile
    // into the caller's `request.target` (`pull-delivers-to-caller-target`) and
    // dispatches NO render: a warm tile contributes zero work (Constraint 2/3).
    if (std::optional<CacheHold<TileValue>> hit = d_cache.lookup(key);
        hit.has_value() && hit->get().meta.exact && hit->get().meta.achieved_scale == rung_px) {
      deliver_tile(d_backend, *hit->get().surface, coord, rung_px, request);
      region_exact = region_exact && hit->get().meta.exact;
      continue;
    }

    // Cache-first has a SECOND suppression key behind it (`pull-joins-in-flight-tile`,
    // doc 13, `compositor.in_flight_tile_dedup`): a covering tile whose render is
    // already in flight is not dispatched again. The cache is still probed first and
    // the pending set only on a miss, so this narrows `pull-is-cache-first` rather
    // than replacing it. This is the dominant duplicate source in the tree: two
    // operators sharing an input each pull it, a leaf that is both a visible layer
    // and an operator's input is dispatched by the driver and then re-dispatched by
    // the pull, and a nested chain pays one per level per wave -- and the in-flight
    // tile is absent from the cache, so nothing above catches any of it.
    //
    // The joined pull delivers NOTHING and settles nothing: it is an async tile like
    // any other (`any_async`), so the aggregate below leaves the caller's `done`
    // unsettled, the region stays inexact, the operator degrades this pass
    // (TRANSIENT -- something more is genuinely coming, from the dispatch we deferred
    // to), and the in-flight arrival's damage re-drives every consumer of that input,
    // including this one, which never registered with it. That broadcast is what
    // makes the join sound with no join primitive: collapsing N dispatches to one
    // changes the work done, not the wake-ups delivered.
    if (tile_in_flight(d_config.pending, key)) {
      if (d_config.counters != nullptr) {
        d_config.counters->note_request_suppressed();
      }
      d_unmet.push_back(key);
      any_async = true;
      continue;
    }

    // And a THIRD thing behind the pending set: the refinement WAVE
    // (`compositor.operator_refinement_wave_amplification`,
    // `13-effects-as-operators#operator-defers-to-its-pending-inputs`). A covering tile
    // that is an OPERATOR's transient output -- resident under this exact key but
    // flagged inexact, so the hit gate above rejected it -- whose recorded unmet inputs
    // are still pending is NOT re-rendered by a partial arrival. Its chain is re-driven
    // at most once per wave, when the last input it is waiting on lands.
    //
    // Unlike the in-flight join above, this arm DELIVERS. It must: the caller is an
    // operator that is about to composite this tile NOW, and an undelivered target is a
    // transparent hole in its placeholder. The resident transient tile is exactly the
    // right thing to hand it -- same content, same revision, same rung, same coord, and
    // merely not final -- so the frame that defers paints exactly what the previous frame
    // painted for this tile and no pixels move. It is still TRANSIENT: `any_async` leaves
    // the caller's `done` unsettled and the region inexact, so the parent degrades this
    // pass exactly as it did before, and the wave's arrival re-drives it.
    //
    // The caller inherits the child's OUTSTANDING inputs (not the child's own output key,
    // which is an operator tile and so never appears in `queue.tiles`) -- that is what
    // keeps the parent's wait in leaf-key space and opens both gates on the same wave.
    if (operator_wave_pending(d_config.pending, key)) {
      if (std::optional<CacheHold<TileValue>> transient = d_cache.lookup(key);
          transient.has_value() && transient->get().meta.achieved_scale == rung_px) {
        deliver_tile(d_backend, *transient->get().surface, coord, rung_px, request);
        if (d_config.counters != nullptr) {
          d_config.counters->note_render_coalesced();
        }
        const std::vector<TileKey> still = operator_wave_unmet(d_config.pending, key);
        d_unmet.insert(d_unmet.end(), still.begin(), still.end());
        region_exact = false;
        any_async = true;
        continue;
      }
      // The transient tile was evicted under memory pressure, so there is nothing to
      // deliver and deferring would blank the caller's target. Fall through and render:
      // coalescing is an optimization, and stranding a tile behind a hole to buy it is
      // not a trade this gate is allowed to make (Constraint 3).
    }

    // Miss: build the render descriptor and dispatch it. The tile pixels are owned
    // by the cache (`TileValue` owns its `Surface`), so the render targets a
    // freshly-allocated cache-destined surface -- not the caller's `request.target`
    // -- which travels into the cache (inline) or the pending queue (async). A
    // per-tile allocation failure fails the WHOLE region to the placeholder
    // (Constraint 2): a partially filled region is not a correct operator input.
    arbc::expected<std::unique_ptr<Surface>, SurfaceError> owned =
        d_backend.make_surface(k_tile_size, k_tile_size, request.target.format());
    if (!owned.has_value()) {
      settle_placeholder(done);
      return;
    }
    Surface& tile_surface = **owned;
    d_backend.clear(tile_surface, 0.0F, 0.0F, 0.0F, 0.0F);

    // Render THIS tile's footprint at the rung scale into the cache surface --
    // exactly the per-tile descriptor `render_frame_interactive` builds
    // (`tile_planning.cpp:370`: `{tile.local_rect, rung_px, ...}`), not the whole
    // `request.region`/`request.scale`. A tile is cached over `tile_local_rect` at
    // `rung_px`, and `deliver_tile`'s affine assumes exactly that (its tile->local
    // map is `translate(coord origin) . scale(1/rung_px)`), so each covering tile
    // holds its own disjoint slice of the region and delivers seam-free (the
    // "tiled == whole" identity, Constraint 6). Using `request.region` for every
    // tile -- the pre-existing single-tile shortcut this task removes -- would fill
    // every tile with the region's top-left corner. Snapshot and deadline are
    // carried verbatim (doc 05:96-100, `pull-inherits-snapshot-and-deadline`):
    // neither reset nor recomputed, no per-pull sub-budget; only the target and the
    // per-tile region/scale differ.
    const Rect tile_region = tile_local_rect(selection.rung, coord);
    const RenderRequest render_request{tile_region,      rung_px,      request.time,
                                       request.snapshot, tile_surface, request.exactness,
                                       request.deadline};

    // A dispatched render is counted per covering tile (Decision 3): whether it
    // answers inline or defers (both issued the render); an operator render also
    // bumps `operator_renders`. A warm N-tile region issues zero; a cold N-tile
    // region issues exactly N.
    if (d_config.counters != nullptr) {
      d_config.counters->note_request_issued();
      if (op) {
        d_config.counters->note_operator_render();
      }
    }

    // Dispatch into an INTERNAL completion so the caller's `done` is never consumed
    // by the render -- the caller's `done` settles once from the aggregate after
    // the loop. An operator owns the `done` it passed and observes its input's
    // result through it (`FadeContent::render` pulls into `temp` then reads `done`).
    auto inner = std::make_shared<RenderCompletion>();

    // The descent depth is in effect across the dispatch so an operator whose
    // `render` recursively pulls its own input re-enters `pull` at `d_depth + 1`.
    // The unmet MARK is taken across it too (Decision 2): whatever this render's own
    // (possibly recursive) pulls leave unmet is the tail past this point, and it is
    // the wave the tile below is waiting on.
    const std::size_t mark = unmet_mark();
    ++d_depth;
    d_dispatch(input, render_request, inner);
    --d_depth;

    if (inner->settled()) {
      const std::optional<expected<RenderResult, RenderError>> settled = inner->take();
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
        // Deliver the freshly-rendered tile into the caller's `request.target`
        // (`pull-delivers-to-caller-target`): the synchronous-miss analog of the
        // hit delivery above. One composite into this tile's disjoint sub-rect; the
        // render was already dispatched (Constraint 2).
        deliver_tile(d_backend, tile_surface, coord, rung_px, request);
        const std::size_t bytes = tile_byte_cost(tile_surface);
        d_cache.insert(key, TileValue{std::move(*owned), {result.achieved_scale, result.exact}},
                       bytes, PriorityClass::Visible);
        region_exact = region_exact && result.exact;
        // Record the wave this render opened (Decision 2). An INEXACT result whose
        // render left input tiles unmet is an operator that painted a transient
        // placeholder while its inputs are still in flight -- the tile just inserted
        // above. Naming the inputs it is waiting on is what lets the next plan defer
        // its re-render instead of driving the whole chain again. An inexact LEAF
        // pulled nothing, so its tail is empty and `record_operator_wait` records
        // nothing: it has no wave and keeps re-rendering exactly as it does today.
        if (!result.exact && d_config.pending != nullptr) {
          record_operator_wait(*d_config.pending, key, unmet_since(mark));
        }
      } else {
        // A settled-via-fail inline render: no insert, no pending. Fail the WHOLE
        // region to the placeholder and stop -- a partially filled region is not a
        // correct operator input.
        settle_placeholder(done);
        return;
      }
    } else if (d_config.pending != nullptr) {
      // Async (doc 02:69-71 step 6, doc 13:128-130): the render answered
      // asynchronously (`inner` is still live). Record it -- the cache-owned surface
      // and the render's completion `inner` travel with it -- so a later
      // `poll_refinements` inserts it under `Visible` and emits damage; the render
      // occupies no worker while pending. The cache stays single-writer on the
      // draining frame thread (Decision 4/5): only `content->render` into the
      // thread-confined target ran on a worker. The reap sink, not the caller, owns
      // the surface + `inner` until the worker settles, so a caller `cancel()` never
      // frees a surface an in-flight worker is still writing
      // (`pull-retains-render-surface-until-settle`). The caller's `done` is left
      // unsettled because THIS tile is async -- flagged so the aggregate settle
      // below leaves the whole region unsettled and the operator degrades this
      // frame; each async tile re-drives independently.
      const std::size_t bytes = tile_byte_cost(tile_surface);
      d_config.pending->tiles.push_back(PendingTile{key, request.region, id, stability, bytes,
                                                    std::move(*owned), std::move(inner)});
      d_unmet.push_back(key);
      any_async = true;
    } else {
      // Async dispatch with no reap sink (Decision 3, `pull-retains-render-surface-
      // until-settle`): the injected `d_dispatch` answered asynchronously (`inner`
      // unsettled) against a service configured without a `pending` sink -- a
      // driver-precondition violation. There is nowhere to retain `owned` (the
      // surface a worker may still be writing) + `inner` until the render settles, so
      // we cannot proceed as the top-level async path does. Assert the precondition
      // in debug (a sink-less path MUST be given a synchronous dispatch, which settles
      // inline above); in release, degrade the WHOLE region to the placeholder plus
      // exactly one diagnostic naming the offending input rather than the silent
      // use-after-free this branch used to fall through to. The runtime binds a
      // synchronous dispatch for every sink-less path (offline goldens use
      // `direct_dispatch`), so this fires only on a misconfiguration.
      assert(inner->settled() &&
             "PullServiceImpl::pull: an async-capable dispatch requires a `pending` reap sink; a "
             "service configured without one must be given a synchronous dispatch (Decision 3)");
      if (d_config.diagnostics != nullptr) {
        d_config.diagnostics->entries.push_back(GraphDiagnostic{input, d_depth, {input}});
      }
      settle_placeholder(done);
      return;
    }
  }

  // Settle the caller's completion once from the aggregate across all covering
  // tiles (Decision 2). Any tile answering asynchronously leaves `done` unsettled
  // -- the operator sees `!done->settled()`, degrades to its placeholder this
  // frame, and ignores `target`; tiles already delivered are harmless (an
  // unsettled `done` means `target` is never read), and each async tile's arrival
  // re-drives a later frame that hits and delivers (doc 13's "async composes").
  // Otherwise every covering tile resolved synchronously (hit or inline-settle):
  // complete with the uniform rung scale, the region's folded exactness
  // (`region_exact = AND(tile.exact)`), and the uniform per-input achieved_time.
  // An empty region (no covering tiles) runs the loop zero times, delivers
  // nothing, and completes exact -- replacing the old degenerate `{}`-coord probe
  // (Constraint 7).
  if (any_async) {
    return;
  }
  if (done) {
    done->complete(RenderResult{rung_px, region_exact, achieved_time});
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
    // Fold the request's Spatial-context digest (doc 12:249-254,
    // spatial_blockkey_disambiguation D1/D4): this read-side pull key must equal the
    // write-side warm key `contribution_key` built from the same per-edge
    // `Spatialization`, so residency holds; two distinct contexts over the same
    // `(content, revision, block index, rate)` probe distinct slots. Flat digests to 0,
    // leaving the key byte-identical to the pre-task one (Constraint 1).
    const BlockKey key{id, revision, audio_block_index(request), request.sample_rate,
                       spatial_context_digest(request.spatial)};

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
