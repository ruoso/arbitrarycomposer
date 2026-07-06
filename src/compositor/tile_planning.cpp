#include <arbc/compositor/damage_planning.hpp>
#include <arbc/compositor/operator_graph.hpp>
#include <arbc/compositor/pull_service.hpp>
#include <arbc/compositor/refinement.hpp>
#include <arbc/compositor/tile_planning.hpp>

#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

namespace arbc {

namespace {

// Floor division for a positive divisor (std integer division truncates toward
// zero, which is wrong for negative tile coordinates). Maps a fine tile column
// to the coarser-grid column that contains it.
std::int32_t floor_div(std::int32_t numerator, std::int32_t divisor) {
  std::int32_t quotient = numerator / divisor;
  if (numerator % divisor != 0 && numerator < 0) {
    --quotient;
  }
  return quotient;
}

// Map a tile surface (whose pixel (i, j) covers `local_rect.origin + (i, j) /
// scale`) into device space through `local_to_device`, mirroring
// `compositor.cpp:95-99`. At an integer-aligned power-of-two rung the composite
// tap collapses to the byte-exact nearest tap (doc 02:66-68).
Affine surface_to_device(const Affine& local_to_device, const Rect& local_rect, double scale) {
  return compose(local_to_device, compose(Affine::translation(local_rect.x0, local_rect.y0),
                                          Affine::scaling(1.0 / scale, 1.0 / scale)));
}

// Composite a `Coarser` fallback (doc 02:64). The coarser source covers a
// larger local rect (2^octave x this tile), so painting it directly would
// overpaint abutting tiles. Instead upscale just this fine tile's footprint
// into a transient pool temp (never cached), then composite that temp through
// the fine per-tile affine -- bounding the paint to the tile so neighbouring
// fallbacks never double-blend.
void composite_coarser(Backend& backend, SurfacePool& pool, Surface& target,
                       const PlannedTile& tile, const Affine& local_to_device, ScaleRung rung,
                       double opacity, CompositorCounters* counters) {
  const double rung_px = rung_scale(rung);
  const int octave = rung.index - tile.source_rung.index;
  const std::int32_t factor = std::int32_t{1} << octave;
  const TileCoord coarser_coord{floor_div(tile.coord.col, factor),
                                floor_div(tile.coord.row, factor)};
  const Rect coarser_rect = tile_local_rect(tile.source_rung, coarser_coord);
  const double coarser_px = rung_scale(tile.source_rung);

  expected<PooledSurface, SurfaceError> temp =
      pool.acquire(k_tile_size, k_tile_size, target.format());
  if (!temp.has_value()) {
    return; // cannot upscale this fallback this pass: leave transparent
  }
  Surface& temp_surface = temp->get();
  backend.clear(temp_surface, 0.0F, 0.0F, 0.0F, 0.0F);

  // coarser-surface px -> local -> fine-temp px, so the whole temp is filled
  // from the coarser tile's covering sub-region.
  const Affine coarser_to_local = compose(Affine::translation(coarser_rect.x0, coarser_rect.y0),
                                          Affine::scaling(1.0 / coarser_px, 1.0 / coarser_px));
  const Affine local_to_temp =
      compose(Affine::scaling(rung_px, rung_px),
              Affine::translation(-tile.local_rect.x0, -tile.local_rect.y0));
  backend.composite(temp_surface, *tile.hold->surface, compose(local_to_temp, coarser_to_local),
                    1.0);
  if (counters != nullptr) {
    counters->note_composite();
  }

  backend.composite(target, temp_surface,
                    surface_to_device(local_to_device, tile.local_rect, rung_px), opacity);
  if (counters != nullptr) {
    counters->note_composite();
  }
}

} // namespace

std::vector<TileCoord> tiles_covering(ScaleRung rung, const Rect& local_region) {
  std::vector<TileCoord> tiles;
  if (local_region.empty()) {
    return tiles;
  }

  // Cell edge in local units: k_tile_size device pixels / (device px per local
  // unit at this rung). A cell is exactly k_tile_size^2 device pixels here.
  const double cell = static_cast<double>(k_tile_size) / rung_scale(rung);

  // Half-open coverage: cell c overlaps [x0, x1) iff c*cell < x1 and
  // (c+1)*cell > x0, so col in [floor(x0/cell), ceil(x1/cell) - 1]. A region
  // exactly on a boundary contributes no extra cell (no double-cover).
  const auto col_min = static_cast<std::int32_t>(std::floor(local_region.x0 / cell));
  const auto col_max = static_cast<std::int32_t>(std::ceil(local_region.x1 / cell)) - 1;
  const auto row_min = static_cast<std::int32_t>(std::floor(local_region.y0 / cell));
  const auto row_max = static_cast<std::int32_t>(std::ceil(local_region.y1 / cell)) - 1;

  for (std::int32_t row = row_min; row <= row_max; ++row) {
    for (std::int32_t col = col_min; col <= col_max; ++col) {
      tiles.push_back(TileCoord{col, row});
    }
  }
  return tiles;
}

Rect tile_local_rect(ScaleRung rung, TileCoord coord) {
  const double cell = static_cast<double>(k_tile_size) / rung_scale(rung);
  const double x0 = static_cast<double>(coord.col) * cell;
  const double y0 = static_cast<double>(coord.row) * cell;
  return Rect{x0, y0, x0 + cell, y0 + cell};
}

LayerTilePlan plan_layer(TileCache& cache, ObjectId content, std::uint64_t revision,
                         std::optional<std::uint64_t> prior_revision,
                         const RungSelection& selection, const Rect& local_region,
                         const Affine& local_to_device, Stability stability, Time time,
                         StateHandle snapshot, Deadline deadline, const Content* content_ptr) {
  LayerTilePlan plan;
  plan.content = content;
  plan.rung = selection.rung;
  plan.remainder = selection.remainder;
  plan.local_to_device = local_to_device;
  plan.time = time;
  plan.snapshot = snapshot;
  plan.deadline = deadline;

  const double rung_px = rung_scale(selection.rung);
  // Static content omits achieved_time so its keys are clock-invariant (doc 11:
  // 139-140), unchanged by the quantization branch. Timed/Live content keys the
  // time snapped to the content's native grid via `quantize_time` (achieved-time
  // coalescing, doc 11:115-129): every requested instant in one native frame
  // period collapses to a single key, so a sub-frame clock advance re-plans to
  // all-fresh hits and issues zero renders. A null `content_ptr` or a `nullopt`
  // from `quantize_time` keeps the raw requested time (today's identity
  // behaviour) -- the default path is byte-identical. Evaluated once per layer.
  const Time key_time =
      (content_ptr != nullptr) ? content_ptr->quantize_time(time).value_or(time) : time;
  const std::optional<Time> achieved_time =
      (stability == Stability::Static) ? std::nullopt : std::optional<Time>(key_time);

  for (const TileCoord coord : tiles_covering(selection.rung, local_region)) {
    PlannedTile tile;
    tile.coord = coord;
    tile.local_rect = tile_local_rect(selection.rung, coord);
    tile.key = TileKey{content, revision, selection.rung, coord, achieved_time};
    tile.klass = PriorityClass::Visible;
    tile.source_rung = selection.rung;

    // Fresh probe (doc 02:63): a hit qualifies only if exact at this rung (the
    // consumer's read of the value metadata, keyed_store contract).
    if (std::optional<CacheHold<TileValue>> hit = cache.lookup(tile.key);
        hit.has_value() && hit->get().meta.exact && hit->get().meta.achieved_scale == rung_px) {
      tile.display_source = TileSource::Fresh;
      tile.hold = std::move(*hit);
      tile.is_miss = false;
      plan.tiles.push_back(std::move(tile));
      continue;
    }

    // Fresh exact absent -> a render is owed regardless of the fallback shown.
    tile.is_miss = true;

    // Stale probe: a deliberate prior-revision lookup (doc 02:64,:94). Once
    // cache.invalidation drops the prior tile this simply misses and falls
    // through to Coarser -- forward-compatible with no cache change.
    if (prior_revision.has_value()) {
      const TileKey stale_key{content, *prior_revision, selection.rung, coord, achieved_time};
      if (std::optional<CacheHold<TileValue>> stale = cache.lookup(stale_key); stale.has_value()) {
        tile.display_source = TileSource::Stale;
        tile.hold = std::move(*stale);
        plan.tiles.push_back(std::move(tile));
        continue;
      }
    }

    // Coarser probe: successively coarser rungs at the current revision, bounded
    // to k_max_fallback_octaves (a cold cache would otherwise scan to the
    // coarsest); the first covering hit wins.
    bool found_coarser = false;
    for (int octave = 1; octave <= k_max_fallback_octaves; ++octave) {
      const ScaleRung coarser{selection.rung.index - octave};
      const std::int32_t factor = std::int32_t{1} << octave;
      const TileCoord coarser_coord{floor_div(coord.col, factor), floor_div(coord.row, factor)};
      const TileKey coarser_key{content, revision, coarser, coarser_coord, achieved_time};
      if (std::optional<CacheHold<TileValue>> hit = cache.lookup(coarser_key); hit.has_value()) {
        tile.display_source = TileSource::Coarser;
        tile.source_rung = coarser;
        tile.hold = std::move(*hit);
        found_coarser = true;
        break;
      }
    }
    if (found_coarser) {
      plan.tiles.push_back(std::move(tile));
      continue;
    }

    // Nothing available -> transparent/checkerboard placeholder, no pin.
    tile.display_source = TileSource::Placeholder;
    plan.tiles.push_back(std::move(tile));
  }

  return plan;
}

void render_frame_interactive(const DocRoot& state, const ContentResolver& resolve,
                              const Viewport& viewport, TileCache& cache, Backend& backend,
                              SurfacePool& pool, Surface& target, Deadline deadline,
                              std::optional<std::uint64_t> prior_revision, RefinementQueue* pending,
                              CompositorCounters* counters, const DirtyRegion* dirty,
                              Time composition_time, std::vector<LayerTilePlan>* visible_plans,
                              GraphDiagnostics* diagnostics, PullServiceImpl* pulls) {
  // The null-`dirty` path clears and re-plans the whole viewport (byte-identical
  // to today). A gated frame composites only the damaged tiles onto the
  // caller-persisted `target`, so it must NOT clear -- the untouched region
  // survives from the previous frame (doc 02:51, refinement Decision 2).
  if (dirty == nullptr) {
    backend.clear(target, 0.0F, 0.0F, 0.0F, 0.0F);
  }

  // The opt-in plan sink is emptied at entry so it holds exactly this frame's
  // planned layers, in composite order (see the move-out at layer-scope exit).
  // Null (the default) surfaces nothing -- byte-for-byte the current behavior.
  if (visible_plans != nullptr) {
    visible_plans->clear();
  }

  const std::uint64_t revision = state.revision();
  const Rect device_rect =
      Rect::from_size(static_cast<double>(viewport.width), static_cast<double>(viewport.height));

  // The per-layer cull/compose/region walk is `render_frame`'s front half
  // (compositor.cpp:16-46), reused verbatim; the interactive path forks after
  // it into quantize -> tile-split -> lookup -> tiled-composite.
  state.for_each_layer([&](const LayerRecord& layer) {
    if (!layer.visible() || layer.opacity <= 0.0) {
      return;
    }
    Content* content = resolve(layer.content);
    if (content == nullptr) {
      return;
    }
    // Operator-graph awareness (`compositor.operator_graph`): a layer whose
    // content is an operator (`inputs()` non-empty) keys its tiles by the
    // aggregate revision folded over the reachable `inputs()` DAG (doc
    // 05:82-91), so the operator's key changes on any reachable input change.
    // A leaf keeps the flat `revision` -- the fold degenerates to
    // `contribution(root)` and this branch is skipped, so the leaf path is
    // byte-identical. The driver supplies the document-global `revision` as
    // every node's contribution today (Decision 3): correct and never stale.
    const bool op_layer = is_operator(content);
    const std::uint64_t layer_revision =
        op_layer ? aggregate_revision(content, [&](const Content*) { return revision; }) : revision;
    const Affine composed = compose(viewport.camera, layer.transform);
    const std::optional<Affine> inv = composed.inverse();
    if (!inv.has_value()) {
      return; // degenerate placement: cull (doc 04)
    }
    Rect region = inv->map_rect(device_rect);
    if (const std::optional<Rect> bounds = content->bounds(); bounds.has_value()) {
      region = region.intersect(*bounds);
    }
    // Damage gating (Decision 2): narrow this layer's plan region to the dirty
    // region mapped into layer-local space (reusing the inverse the cull already
    // computed), so only tiles intersecting a device dirty rect are planned. A
    // non-null empty `DirtyRegion` leaves `dirty_local` empty -> `region` empty
    // -> this layer is skipped, realizing "no damage -> no work" as zero renders
    // and zero composites.
    if (dirty != nullptr) {
      Rect dirty_local{}; // empty accumulator (empty=identity under rect_union)
      for (const Rect& device_dirty : dirty->device_rects) {
        const Rect clipped = device_dirty.intersect(device_rect);
        if (clipped.empty()) {
          continue;
        }
        dirty_local = rect_union(dirty_local, inv->map_rect(clipped));
      }
      region = region.intersect(dirty_local);
    }
    if (region.empty()) {
      return;
    }
    const double scale = composed.max_scale();
    if (!(scale > 0.0) || !std::isfinite(scale)) {
      return; // sub-pixel / degenerate: cull (doc 04)
    }

    // Doc 02 step 3: quantize to the ladder, split into tiles, look each up.
    const RungSelection selection = select_rung(scale);
    LayerTilePlan plan = plan_layer(cache, layer.content, layer_revision, prior_revision, selection,
                                    region, composed, content->stability(), composition_time,
                                    StateHandle{}, deadline, content);

    const double rung_px = rung_scale(selection.rung);

    for (PlannedTile& tile : plan.tiles) {
      // Doc 02 step 4: a miss becomes a BestEffort deadline-carrying request
      // targeting exactly the tile footprint, driven inline this pass. The
      // rendered pixels are owned by the cache (TileValue owns its Surface), so
      // the tile target is a cache-owned surface (not a transient pool temp);
      // the pool serves the coarser-fallback upscale. On success the tile
      // becomes its own fresh display source.
      if (tile.is_miss) {
        expected<std::unique_ptr<Surface>, SurfaceError> owned =
            backend.make_surface(k_tile_size, k_tile_size, target.format());
        if (owned.has_value()) {
          Surface& tile_surface = **owned;
          backend.clear(tile_surface, 0.0F, 0.0F, 0.0F, 0.0F);

          // The requested content-local time is the caller-supplied
          // `composition_time` (Decision 4); the content quantizes it internally
          // and reports `achieved_time == quantize_time(composition_time)`, which
          // by contract equals the plan-built key, so the fill stores under the
          // key the render lands on -- no post-render re-key (Decision 3). The
          // request carries the layer plan's `snapshot` pin verbatim (closing the
          // inert `StateHandle{}` gap, doc 02:124,
          // `02-architecture#miss-becomes-deadline-request`); runtime populates a
          // non-none handle once content-state binding lands, and it flows onto
          // every dispatched render unchanged. `deadline` is stamped as a value.
          const RenderRequest request{tile.local_rect, rung_px,      composition_time,
                                      plan.snapshot,   tile_surface, Exactness::BestEffort,
                                      deadline};
          // Operator identity short-circuit (`compositor.operator_graph`, doc
          // 13:59-65,128). An operator layer resolves its identity chain before
          // issuing a render: an identity pass-through (a fade at envelope == 1)
          // serves input N's cached tiles -- `pull_service`'s job -- so this task
          // issues NO operator render and creates NO operator-output cache entry,
          // and a budget-exceeded descent (a divergent cycle) renders the
          // placeholder plus one diagnostic. Only a genuine non-identity operator
          // render is driven and counted (`operator_renders`). A leaf never
          // enters this branch (`op_layer` false), so its path is byte-identical.
          bool issue_render = true;
          if (op_layer) {
            const IdentityResolution ident =
                resolve_identity(content, request, GraphBudget{}, diagnostics);
            issue_render = !(ident.short_circuited || ident.budget_exceeded);
          }
          if (issue_render) {
            // The single settle path (doc 03:117-121), exactly as `render_frame`.
            // A request is *driven* here (Decision 5): count it whether the
            // content answers inline or defers -- both issued the render.
            auto done = std::make_shared<RenderCompletion>();
            if (counters != nullptr) {
              counters->note_request_issued();
              if (op_layer) {
                counters->note_operator_render();
              }
            }
            // Inline-vs-async detection. The NULL path is byte-for-byte the
            // pre-task inline fill: it drives `content->render` directly and keys
            // off the RETURN value (a content may return `nullopt` yet pre-settle
            // `done` -- that stays an async record reaped by the next poll, doc
            // 02:69-71). The ACTIVE path (`pulls` non-null, runtime's worker-backed
            // dispatch) hands the render to `pulls->dispatch` and detects async via
            // `done->settled()` -- a worker leaves the completion live to settle
            // off-thread. Only the render call differs; the insert and async
            // recording below are shared.
            bool inline_settled = false;
            if (pulls != nullptr) {
              pulls->dispatch(content, request, done);
              inline_settled = done->settled();
            } else {
              const std::optional<RenderResult> inline_result = content->render(request, done);
              if (inline_result.has_value()) {
                done->complete(*inline_result);
              }
              inline_settled = inline_result.has_value();
            }
            if (inline_settled) {
              const std::optional<expected<RenderResult, RenderError>> settled = done->take();
              if (settled.has_value() && settled->has_value()) {
                const RenderResult result = **settled;
                const std::size_t bytes = tile_byte_cost(tile_surface);
                tile.hold = cache.insert(
                    tile.key, TileValue{std::move(*owned), {result.achieved_scale, result.exact}},
                    bytes, PriorityClass::Visible);
                tile.display_source = TileSource::Fresh;
                tile.source_rung = selection.rung;
              }
            } else if (pending != nullptr) {
              // Doc 02:69-71 step 6: the content answered asynchronously (the
              // `RenderCompletion` is still live). Record the deferred render into
              // the caller-owned queue instead of dropping it -- the target
              // surface travels with it so the arrival can be inserted, and the
              // tile keeps its planned fallback `display_source` so it still
              // composites the coarse-then-refine display this frame. When
              // `pending` is null this branch is skipped and the tile is dropped
              // exactly as before.
              const std::size_t bytes = tile_byte_cost(tile_surface);
              pending->tiles.push_back(PendingTile{tile.key, tile.local_rect, layer.content, bytes,
                                                   std::move(*owned), std::move(done)});
            }
          }
        }
      }

      // Doc 02 steps 5-6: composite the display source through the per-tile
      // affine, the <=1-octave remainder folded into the bilinear tap.
      switch (tile.display_source) {
      case TileSource::Fresh:
      case TileSource::Stale:
        if (tile.hold.valid()) {
          backend.composite(target, *tile.hold->surface,
                            surface_to_device(composed, tile.local_rect, rung_px), layer.opacity);
          if (counters != nullptr) {
            counters->note_composite();
          }
        }
        break;
      case TileSource::Coarser:
        if (tile.hold.valid()) {
          composite_coarser(backend, pool, target, tile, composed, selection.rung, layer.opacity,
                            counters);
        }
        break;
      case TileSource::Placeholder:
        // Transparent placeholder: the target was cleared to transparent, so
        // "nothing yet" is a no-op (doc 02:64 checkerboard/transparent).
        break;
      }
    }

    // Surface the plan the frame just composited from (Decision 1/3): move it
    // into the caller's sink -- retaining its tiles' cache holds -- so the
    // interactive loop drives `prime_prefetch` from exactly these keys without a
    // re-plan (claim `02-architecture#speculation-drives-from-exposed-plan`).
    // Reached only for a layer that survived every cull above and was planned, so
    // the sink holds one entry per composited layer in bottom-to-top order. Null
    // (the default) drops the plan here exactly as before.
    if (visible_plans != nullptr) {
      visible_plans->push_back(std::move(plan));
    }
  });
}

} // namespace arbc
