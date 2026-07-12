#include <arbc/compositor/damage_planning.hpp>
#include <arbc/compositor/operator_graph.hpp>
#include <arbc/compositor/provided_surface.hpp>
#include <arbc/compositor/pull_service.hpp>
#include <arbc/compositor/refinement.hpp>
#include <arbc/compositor/tile_planning.hpp>

#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace arbc {

bool timed_insert_key_consistent(const TileKey& key, const RenderResult& result,
                                 Stability stability) {
  // Only `Timed` content owns an achieved-time grid the key must land on (doc
  // 11:139-143); `Static` keys carry no achieved_time and `Live` owns no grid.
  if (stability != Stability::Timed) {
    return true;
  }
  // A `Timed` render that reports no achieved_time honored the requested time
  // exactly (or its `quantize_time` defaulted to nullopt): the key then carries
  // the raw requested time and coalesces nothing, still sound (content.hpp:227-234).
  if (!result.achieved_time.has_value()) {
    return true;
  }
  // The doc-11 MUST: the instant the render landed on IS the instant the tile was
  // keyed under, so the coalesced tile is exactly the frame the transport asked for.
  return key.achieved_time == result.achieved_time;
}

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

// Pack a tile coord into a hash key. Every per-rect plan of ONE layer shares the
// layer's content, revision, rung and achieved_time -- only the covered coords
// differ, since the rung is selected from the layer's scale, not from its plan
// region -- so within a layer the coord IS the `TileKey` identity, and this is the
// dedup key the per-rect plans merge on.
std::uint64_t coord_key(TileCoord coord) {
  return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(coord.col)) << 32U) |
         static_cast<std::uint64_t>(static_cast<std::uint32_t>(coord.row));
}

// Composite onto the frame target, once per repaint rect the tile was planned from
// (`refine_frame_composite_idempotence`, generalized by
// `compositor.disjoint_dirty_repaint`).
//
// `clips` holds exactly one NULL entry on the un-gated (null-`dirty`) path, which
// routes to the plain `Backend::composite` -- so that path is one unclipped call,
// byte-identical, and the existing goldens are untouched. On the gated path it holds
// one non-null entry per repaint rect whose plan covered this tile: every composite
// onto the target carries a rect the frame CLEARED and PLANNED against, so the
// planned set, the cleared set and the painted set are the same set, and a tile
// straddling a rect's edge cannot spill its overhang source-over onto un-cleared
// pixels.
//
// A tile straddling two repaint rects therefore composites TWICE -- and that is not
// a double-blend, because the repaint rects are pairwise DISJOINT: each blend is
// scissored to its own rect, so each device pixel is still written exactly once, by
// exactly one of them. That disjointness is `repaint_regions`' whole contract, and
// it is why the raw (overlapping) dirty rects could not be clipped to directly.
//
// The composite count follows the clip list, so it is bumped here rather than at the
// call sites: `composites` honestly reports two clipped blends for a straddling tile
// (Decision 4). At one clip -- every un-gated frame, every single-rect gated frame --
// it is one bump, exactly as before.
void composite_onto_target(Backend& backend, Surface& target, const Surface& src,
                           const Affine& src_to_dst, double opacity,
                           std::span<const Rect* const> clips, CompositorCounters* counters) {
  for (const Rect* clip : clips) {
    if (clip != nullptr) {
      backend.composite_clipped(target, src, src_to_dst, opacity, *clip);
    } else {
      backend.composite(target, src, src_to_dst, opacity);
    }
    if (counters != nullptr) {
      counters->note_composite();
    }
  }
}

// Composite a `Coarser` fallback (doc 02:64). The coarser source covers a
// larger local rect (2^octave x this tile), so painting it directly would
// overpaint abutting tiles. Instead upscale just this fine tile's footprint
// into a transient pool temp (never cached), then composite that temp through
// the fine per-tile affine -- bounding the paint to the tile so neighbouring
// fallbacks never double-blend. The upscale into the temp happens ONCE even for a
// tile straddling several repaint rects: only the final paint onto the target is
// per-rect, since that is the only step the clip scopes.
void composite_coarser(Backend& backend, SurfacePool& pool, Surface& target,
                       const PlannedTile& tile, const Affine& local_to_device, ScaleRung rung,
                       double opacity, CompositorCounters* counters,
                       std::span<const Rect* const> clips) {
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

  composite_onto_target(backend, target, temp_surface,
                        surface_to_device(local_to_device, tile.local_rect, rung_px), opacity,
                        clips, counters);
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
    std::optional<CacheHold<TileValue>> resident = cache.lookup(tile.key);
    if (resident.has_value() && resident->get().meta.exact &&
        resident->get().meta.achieved_scale == rung_px) {
      tile.display_source = TileSource::Fresh;
      tile.hold = std::move(*resident);
      tile.is_miss = false;
      plan.tiles.push_back(std::move(tile));
      continue;
    }

    // Fresh exact absent -> a render is owed regardless of the fallback shown.
    tile.is_miss = true;

    // Transient probe (doc 02:62-67's new first degraded entry,
    // `compositor.operator_refinement_wave_amplification` Decision 3): the SAME probe
    // already done above may have found the tile resident and merely INEXACT -- an
    // operator's transient placeholder, painted while its inputs are still in flight.
    // Same content, same revision, same rung, same coord: strictly better than a
    // stale-revision tile and better still than transparent, and it is what a tile
    // whose re-render the wave gate defers must composite so the layer does not blink.
    // No second lookup: `resident` is the fresh probe's own hold, re-read for its
    // value metadata rather than discarded.
    if (resident.has_value() && resident->get().meta.achieved_scale == rung_px) {
      tile.display_source = TileSource::Transient;
      tile.hold = std::move(*resident);
      plan.tiles.push_back(std::move(tile));
      continue;
    }

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
                              GraphDiagnostics* diagnostics, PullServiceImpl* pulls,
                              Exactness exactness) {
  const std::uint64_t revision = state.revision();
  const Rect device_rect =
      Rect::from_size(static_cast<double>(viewport.width), static_cast<double>(viewport.height));

  // The frame's device **repaint region** (doc 02 § The frame, interactively;
  // `refine_frame_composite_idempotence` Decision 2, generalized by
  // `compositor.disjoint_dirty_repaint`): a set of PAIRWISE-DISJOINT, integer-aligned
  // device rects whose union is exactly the union of the (viewport-clipped,
  // rounded-out) dirty rects. The null-`dirty` path clears and re-plans the WHOLE
  // viewport, unclipped -- byte-identical to today, which is what keeps every landed
  // golden un-rebaselined.
  //
  // That ONE set is used three times below, PER RECT -- each rect gates each layer's
  // plan, each rect is cleared, and each rect is the clip on every composite of a
  // tile it planned -- so the planned set, the cleared set and the painted set are
  // the same set. That is the whole idempotence proof, and DISJOINTNESS is what keeps
  // it true now that the set is more than one rect: no pixel is in two rects, so no
  // pixel is cleared twice or composited twice. Within the region every layer that
  // covers a pixel repaints it exactly once onto transparent (a single full pass,
  // restricted to the region), and outside it nothing is written. The invariant is
  // per-rect and it composes precisely because the rects do not overlap.
  //
  // An empty set -- a non-null but empty `DirtyRegion`, or one mapping entirely
  // outside the viewport -- clears nothing, plans nothing, composites nothing, and
  // leaves `target` byte-identical: "no damage -> no work" (doc 02:51) falls out of
  // every per-rect loop below running zero times.
  const std::vector<Rect> repaint =
      (dirty != nullptr) ? repaint_regions(*dirty, viewport) : std::vector<Rect>{};
  if (dirty == nullptr) {
    backend.clear(target, 0.0F, 0.0F, 0.0F, 0.0F);
  } else {
    // Clear FIRST: source-over is not idempotent for anything but fully-opaque
    // content, so re-compositing a translucent layer onto the pixels the previous
    // frame left in place would land its contribution twice (doc 02, "Clear first").
    // One `clear_rect` per repaint rect -- and because the rects are disjoint, no
    // pixel is cleared twice, and the gap between two far-apart damages is not
    // cleared at all.
    for (const Rect& rect : repaint) {
      backend.clear_rect(target, rect, 0.0F, 0.0F, 0.0F, 0.0F);
    }
  }

  // The opt-in plan sink is emptied at entry so it holds exactly this frame's
  // planned layers, in composite order (see the move-out at layer-scope exit).
  // Null (the default) surfaces nothing -- byte-for-byte the current behavior.
  if (visible_plans != nullptr) {
    visible_plans->clear();
  }

  // The per-layer cull/compose/region walk is `render_frame`'s front half
  // (compositor.cpp:16-46), reused verbatim; the interactive path forks after
  // it into quantize -> tile-split -> lookup -> tiled-composite.
  state.for_each_layer([&](const LayerRecord& layer) {
    if (!layer.visible() || layer.opacity <= 0.0) {
      return;
    }
    // Temporal placement (doc 11:60-73, 185-191): cull a layer whose half-open
    // span `[in, out)` does not contain the current `composition_time` BEFORE
    // resolving content or evaluating its time map (the cull precedes evaluation
    // per doc 11:72 and the `rational_time.hpp:158-162` header comment, and is
    // cheaper -- an absent layer resolves nothing). The default still-layer span
    // is `TimeRange::all()`, always present, so this gate is byte-neutral for a
    // layer with no temporal placement.
    if (!present_in_span(layer.span, composition_time)) {
      return; // outside span: cull (doc 11:21,72)
    }
    Content* content = resolve(layer.content);
    if (content == nullptr) {
      return;
    }
    // The 1D affine map from composition time to content-local time
    // (`local = (composition_time - in) * rate + offset`, doc 11:66-71): the
    // time the request is issued at, replacing the raw composition time. The
    // identity default map (in=0, rate=1/1, offset=0) returns `composition_time`
    // unchanged, so a still layer is byte-exact. On `TimeError` (overflow of the
    // fixed flick width) the layer is culled, mirroring the degenerate-affine
    // cull below -- a layer that cannot be temporally placed is honestly skipped,
    // never rendered at a wrong/clamped instant (Decision D3).
    const expected<Time, TimeError> local = layer.time_map.evaluate(composition_time);
    if (!local.has_value()) {
      return; // unrepresentable local time: cull (doc 11:66-71, Decision D3)
    }
    const Time local_time = *local;
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
    // An empty repaint set skips every layer, realizing "no damage -> no work" as
    // zero renders and zero composites (doc 02:51).
    if (dirty != nullptr && repaint.empty()) {
      return;
    }
    if (region.empty()) {
      return;
    }
    const double scale = composed.max_scale();
    if (!(scale > 0.0) || !std::isfinite(scale)) {
      return; // sub-pixel / degenerate: cull (doc 04)
    }

    // Doc 02 step 3: quantize to the ladder, split into tiles, look each up. The rung
    // is selected from the layer's SCALE, not from its plan region, so every per-rect
    // plan below shares it -- which is what makes the coord the tile's identity here.
    const RungSelection selection = select_rung(scale);
    const double rung_px = rung_scale(selection.rung);

    // Damage gating (damage_planning Decision 2; `disjoint_dirty_repaint` Decision 2):
    // plan this layer ONCE PER REPAINT RECT, each rect mapped into layer-local space
    // through the inverse the cull already computed, and merge the per-rect plans by
    // tile coord. Rect-inner planning is what actually buys the win -- a tile in the
    // gap between two far-apart damages is never planned, never looked up, never
    // composited, where the bounding-box gate planned every tile between them.
    //
    // The merge is what keeps the rest of the frame's contracts intact. A tile
    // straddling two repaint rects is PLANNED twice (idempotent and cheap -- a cache
    // and pending-set lookup) but appears ONCE in the merged plan, so it is rendered
    // once: `requests_issued` does not grow. It carries the list of rects that planned
    // it, and composites once per rect, each scissored to that rect -- so its pixels
    // still land exactly once (the rects are disjoint). And one merged plan per layer
    // is what keeps `visible_plans` at one entry per planned layer, in composite order
    // (`compositor.expose_visible_plan`), rather than one entry per (layer, rect).
    //
    // `tile_clips` is parallel to `plan.tiles`: the clip rects each tile composites
    // under. On the un-gated path it is a single null per tile -- one unclipped
    // composite, byte-for-byte the pre-task path.
    LayerTilePlan plan;
    std::vector<std::vector<const Rect*>> tile_clips;
    const auto plan_region = [&](const Rect& local_region) {
      return plan_layer(cache, layer.content, layer_revision, prior_revision, selection,
                        local_region, composed, content->stability(), local_time, StateHandle{},
                        deadline, content);
    };
    if (dirty == nullptr) {
      plan = plan_region(region);
      tile_clips.assign(plan.tiles.size(), std::vector<const Rect*>{nullptr});
    } else {
      std::unordered_map<std::uint64_t, std::size_t> tile_at;
      for (const Rect& rect : repaint) {
        const Rect rect_region = region.intersect(inv->map_rect(rect));
        if (rect_region.empty()) {
          continue; // this layer does not meet this repaint rect
        }
        LayerTilePlan part = plan_region(rect_region);
        if (tile_at.empty()) {
          // The per-rect plans of one layer agree on every scalar field (same content,
          // rung, remainder, affine, key time, snapshot, deadline) and differ only in
          // which tiles they cover, so the first non-empty part seeds the merged plan
          // wholesale and later parts merge their tiles into it.
          plan = std::move(part);
          tile_clips.assign(plan.tiles.size(), std::vector<const Rect*>{&rect});
          for (std::size_t at = 0; at < plan.tiles.size(); ++at) {
            tile_at.emplace(coord_key(plan.tiles[at].coord), at);
          }
          continue;
        }
        for (PlannedTile& tile : part.tiles) {
          const auto [entry, fresh] = tile_at.emplace(coord_key(tile.coord), plan.tiles.size());
          if (fresh) {
            plan.tiles.push_back(std::move(tile));
            tile_clips.push_back(std::vector<const Rect*>{&rect});
          } else {
            // The straddle: already planned from an earlier rect, so it renders once
            // and gains one more clip to composite under.
            tile_clips[entry->second].push_back(&rect);
          }
        }
      }
    }
    if (plan.tiles.empty()) {
      // A layer lying wholly in the gap between the repaint rects: not planned, not
      // composited, and -- decisively -- not pushed to `visible_plans`, which holds
      // this frame's PLANNED layers.
      return;
    }

    for (std::size_t at = 0; at < plan.tiles.size(); ++at) {
      PlannedTile& tile = plan.tiles[at];
      const std::span<const Rect* const> clips{tile_clips[at]};
      // Doc 02 step 3/4: a miss is checked against the PENDING SET as well as the
      // cache. A tile whose render is already in flight -- this frame dispatched it
      // for another layer, or an operator's pull did -- is absent from the cache and
      // so looks exactly like a tile nobody has ever asked for. Dispatching it again
      // buys nothing: both renders target their own surface, both are deterministic,
      // and the arrival's damage is BROADCAST on the content's id to every consumer
      // of the tile rather than delivered to whoever dispatched it
      // (`refinement.cpp`'s damage + `interactive.cpp`'s `route_arrival_damage`), so
      // the render already in flight re-drives this layer too. A suppressed tile is
      // NOT a hit: it takes exactly the path a freshly-dispatched async tile takes --
      // it keeps its planned fallback `display_source` (stale -> coarser ->
      // transparent) and composites that below -- minus the second surface, the
      // second `content->render`, and the second `PendingTile`. A cancelled entry
      // does not suppress (`tile_in_flight`); an identity operator issues no render
      // and so is never recorded pending, which is why this guard cannot swallow the
      // identity-delivery branch below.
      const bool in_flight = tile.is_miss && tile_in_flight(pending, tile.key);
      if (in_flight && counters != nullptr) {
        counters->note_request_suppressed();
      }
      // Doc 02 step 3/4, the THIRD thing a miss is checked against: the refinement
      // WAVE (`compositor.operator_refinement_wave_amplification`,
      // `02-architecture#refinement-wave-coalesces-chain-rerender`). An operator tile
      // whose last render came back inexact recorded the input tiles it was waiting
      // on; while ANY of them is still pending, a partial arrival does not re-drive
      // the chain. The tile keeps its `Transient` display source -- the placeholder
      // it already painted, resident under this very key -- so the frame composites
      // exactly what the previous frame composited for it and no pixels move; and no
      // `content->render`, no pull, and no surface allocation happen. When the LAST
      // unmet input leaves the queue (settled, failed, or dropped) the gate opens on
      // the very next plan and the chain renders once, with everything it needs.
      //
      // This is not the dedup one line up, and the counters are disjoint so the two
      // stay distinguishable: `in_flight` is a DUPLICATE of a render someone else is
      // already driving, while this is a SECOND render of a tile whose inputs have not
      // all landed. A leaf never enters it -- a leaf's render pulls nothing, leaves
      // nothing unmet, and so records no wait.
      const bool wave_pending =
          tile.is_miss && !in_flight && operator_wave_pending(pending, tile.key);
      if (wave_pending && counters != nullptr) {
        counters->note_render_coalesced();
      }
      // Doc 02 step 4: a miss becomes a BestEffort deadline-carrying request
      // targeting exactly the tile footprint, driven inline this pass. The
      // rendered pixels are owned by the cache (TileValue owns its Surface), so
      // the tile target is a cache-owned surface (not a transient pool temp);
      // the pool serves the coarser-fallback upscale. On success the tile
      // becomes its own fresh display source.
      if (tile.is_miss && !in_flight && !wave_pending) {
        expected<std::unique_ptr<Surface>, SurfaceError> owned =
            backend.make_surface(k_tile_size, k_tile_size, target.format());
        if (owned.has_value()) {
          Surface& tile_surface = **owned;
          backend.clear(tile_surface, 0.0F, 0.0F, 0.0F, 0.0F);

          // The requested content-local time is `local_time` -- the composition
          // time mapped through this layer's `time_map` above (doc 11:122-124);
          // for the identity map it equals the caller-supplied `composition_time`
          // (Decision 4). The content quantizes it internally and reports
          // `achieved_time == quantize_time(local_time)`, which by contract equals
          // the plan-built key (`plan_layer` keyed the same `local_time`), so the
          // fill stores under the key the render lands on -- no post-render re-key
          // (Decision 3). The request carries the layer plan's `snapshot` pin
          // verbatim (closing the inert `StateHandle{}` gap, doc 02:124,
          // `02-architecture#miss-becomes-deadline-request`); runtime populates a
          // non-none handle once content-state binding lands, and it flows onto
          // every dispatched render unchanged. `deadline` is stamped as a value.
          const RenderRequest request{tile.local_rect, rung_px,   local_time, plan.snapshot,
                                      tile_surface,    exactness, deadline};
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
          const Content* identity_terminal = nullptr;
          if (op_layer) {
            const IdentityResolution ident =
                resolve_identity(content, request, GraphBudget{}, diagnostics);
            issue_render = !(ident.short_circuited || ident.budget_exceeded);
            // An identity pass-through resolves to a terminal input whose pixels
            // the frame must SERVE (runtime.operator_identity_offline_delivery,
            // doc 13:59-65). Capture it so the delivery branch below routes input
            // N through `pulls->pull` instead of the suppressed operator render:
            // the serving half `operator_graph.md` Decision 5 deferred to
            // `pull_service`. A budget-exceeded descent leaves `terminal` null and
            // renders the placeholder, so it stays out of the delivery branch.
            if (ident.short_circuited) {
              identity_terminal = ident.terminal;
            }
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
            //
            // The render is bracketed by the pull service's unmet MARK
            // (`compositor.operator_refinement_wave_amplification` Decision 2): an
            // operator renders INLINE on this thread (worker dispatch is leaf-only,
            // doc 02:220-233), so whatever its own -- possibly deeply recursive --
            // pulls leave unmet accumulates past this mark, and the tail is exactly
            // the wave this output tile is waiting on. The accumulator is cleared per
            // driven output tile so it never grows across the frame.
            bool inline_settled = false;
            std::size_t unmet_mark = 0;
            if (pulls != nullptr) {
              pulls->unmet_clear();
              unmet_mark = pulls->unmet_mark();
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
                RenderResult result = **settled;
                // Insert-key temporal linkage (doc 11:134-137): the tile was keyed
                // at `quantize_time(time)` before this render; assert the render
                // landed on that same instant so the coalesced entry serves the
                // frame it is keyed under (a no-op for conformant content, fires
                // only on a doc-11 MUST violation).
                assert(timed_insert_key_consistent(tile.key, result, content->stability()));
                // Honor a content-provided surface (doc 09:87-100): the cache path
                // always COPIES it into the cache-owned `tile_surface` (never
                // adopts, v1 -- so `TileValue` and the cache stay oblivious,
                // doc 09:109-112,328-340), then releases it. `tile_surface` was
                // cleared to transparent above, so a source-over copy yields exactly
                // the provided pixels. Absent, the content already filled
                // `tile_surface` and there is nothing to copy.
                consume_render_result(result, tile_surface, [&](const Surface& src) {
                  if (&src != &tile_surface) {
                    backend.composite(tile_surface, src, Affine::identity(), 1.0);
                  }
                });
                const std::size_t bytes = tile_byte_cost(tile_surface);
                tile.hold = cache.insert(
                    tile.key, TileValue{std::move(*owned), {result.achieved_scale, result.exact}},
                    bytes, PriorityClass::Visible);
                tile.display_source = TileSource::Fresh;
                tile.source_rung = selection.rung;
                // Record the wave this render opened (Decision 2). An INEXACT operator
                // render is a transient placeholder painted while its inputs are still
                // in flight -- the tile just inserted above. Naming the inputs it is
                // waiting on is what lets the next plan DEFER its re-render rather than
                // re-drive the whole chain per arrival. An inexact leaf pulled nothing,
                // so its tail is empty and `record_operator_wait` records nothing.
                if (!result.exact && pending != nullptr && pulls != nullptr) {
                  record_operator_wait(*pending, tile.key, pulls->unmet_since(unmet_mark));
                }
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
              pending->tiles.push_back(PendingTile{tile.key, tile.local_rect, layer.content,
                                                   content->stability(), bytes, std::move(*owned),
                                                   std::move(done)});
            }
          } else if (identity_terminal != nullptr && pulls != nullptr) {
            // Identity delivery (runtime.operator_identity_offline_delivery): the
            // operator layer short-circuited to `identity_terminal`, so SERVE that
            // input's cached tiles into this layer's tile surface through the
            // existing pull machinery -- cache-first, every covering tile,
            // delivered via `deliver_tile` -- keyed under the terminal's own
            // identity (via `PullConfig::id_of`), NOT the operator's (doc
            // 13:59-65,141-154; `operator_graph.md` Decision 5). `request.target`
            // is `tile_surface`, so `pull` fills exactly the pixels a direct input
            // render would (Decision 1). This adds ZERO operator renders
            // (`issue_render` is false, so `note_operator_render` never fired) and
            // NO operator-output cache entry (we composite the delivered surface
            // directly and never `cache.insert` under the operator's `tile.key`,
            // Decision 3); the terminal's own tile is cached under its identity by
            // `pull`, shared by every consumer.
            auto done = std::make_shared<RenderCompletion>();
            pulls->pull(const_cast<Content*>(identity_terminal), request, done);
            if (done->settled()) {
              const std::optional<expected<RenderResult, RenderError>> settled = done->take();
              if (settled.has_value() && settled->has_value()) {
                // `pull` delivered input N's pixels into `tile_surface`; composite
                // it at the layer's device affine/opacity, the same placement the
                // `issue_render` path uses -- but read directly from `tile_surface`
                // (there is no operator-keyed cache hold to read from). Drop any
                // planned coarser/stale fallback hold and mark the tile Fresh so
                // the composite switch below is a no-op (`tile.hold` invalid):
                // this direct composite is the sole paint, so the frame is not
                // counted degraded (offline no-degrade guarantee).
                composite_onto_target(backend, target, tile_surface,
                                      surface_to_device(composed, tile.local_rect, rung_px),
                                      layer.opacity, clips, counters);
                tile.hold = CacheHold<TileValue>{};
                tile.display_source = TileSource::Fresh;
              }
            }
            // A `done` left unsettled means a covering terminal tile answered
            // asynchronously (parallel path, Constraint 6, Decision 4): `pull`
            // recorded that terminal tile in its own pending sink, so the driver
            // reaps it to quiescence and the re-composite pass delivers it
            // synchronously. The operator tile keeps its planned fallback this
            // frame -- no special-casing, exactly the non-identity async path.
          }
        }
      }

      // Doc 02 steps 5-6: composite the display source through the per-tile
      // affine, the <=1-octave remainder folded into the bilinear tap.
      switch (tile.display_source) {
      case TileSource::Fresh:
        if (tile.hold.valid()) {
          composite_onto_target(backend, target, *tile.hold->surface,
                                surface_to_device(composed, tile.local_rect, rung_px),
                                layer.opacity, clips, counters);
        }
        break;
      case TileSource::Transient:
        // A degraded display: the tile's OWN resident-but-inexact entry -- an
        // operator's transient placeholder -- shown while the final render is still
        // owed (doc 02:62-67's first degraded entry,
        // `compositor.operator_refinement_wave_amplification` Decision 3). Shown
        // instead of a stale/coarser/transparent fallback because it is strictly
        // better: same content, same revision, same rung, right geometry, merely not
        // final. It is what a tile whose re-render the wave gate deferred composites,
        // which is why deferring changes no pixels. Still NOT final, so it is counted
        // degraded, exactly as `Stale` and `Coarser` are -- an offline frame that
        // composited one left the final render undone, which the no-degrade guarantee
        // forbids.
        if (tile.hold.valid()) {
          composite_onto_target(backend, target, *tile.hold->surface,
                                surface_to_device(composed, tile.local_rect, rung_px),
                                layer.opacity, clips, counters);
          if (counters != nullptr) {
            counters->note_degraded_composite();
          }
        }
        break;
      case TileSource::Stale:
        // A degraded display: a prior-revision tile shown while the fresh render is
        // owed (doc 02:64). Counted as degraded so the offline no-degrade guarantee
        // is a behavioral zero (`02-architecture#offline-frame-renders-exactly-no-degrade`).
        // Degradation is a property of the tile's DISPLAY SOURCE, not of how many
        // repaint rects it lands in, so it is counted once per tile even when the tile
        // straddles several -- exactly as the `Placeholder` arm below counts one
        // degraded display against zero composites.
        if (tile.hold.valid()) {
          composite_onto_target(backend, target, *tile.hold->surface,
                                surface_to_device(composed, tile.local_rect, rung_px),
                                layer.opacity, clips, counters);
          if (counters != nullptr) {
            counters->note_degraded_composite();
          }
        }
        break;
      case TileSource::Coarser:
        // A degraded display: a coarser-rung tile rescaled up (doc 02:64).
        if (tile.hold.valid()) {
          composite_coarser(backend, pool, target, tile, composed, selection.rung, layer.opacity,
                            counters, clips);
          if (counters != nullptr) {
            counters->note_degraded_composite();
          }
        }
        break;
      case TileSource::Placeholder:
        // Transparent placeholder: the pixels this tile covers were cleared to
        // transparent -- the whole target on the un-gated path, the repaint rects on
        // the gated one, and a planned tile intersects every rect that planned it by
        // construction -- so "nothing yet" is a no-op paint (doc 02:64
        // checkerboard/transparent). On the gated path this is a HOLE for one frame
        // until the arrival refines it, not the previous frame's pixels showing
        // through: that is what a full pass would have shown (doc 02 step 4's
        // degradation ladder), and a content-damaged tile has no stale or coarser
        // rung left to fall back to -- `invalidate_damage` drops it across all rungs
        // and revisions (doc 02:94-95). Unreachable at the shipped `worker_count ==
        // 0`, where every miss settles inline (`refine_frame_composite_idempotence`
        // Decision 3). It is still a degraded (non-fresh) display, so it is counted
        // as such: an offline frame that composited a placeholder left a hole, which
        // the no-degrade guarantee forbids.
        if (counters != nullptr) {
          counters->note_degraded_composite();
        }
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
