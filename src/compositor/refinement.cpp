#include <arbc/base/expected.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/compositor/provided_surface.hpp>
#include <arbc/compositor/refinement.hpp>
#include <arbc/surface/backend.hpp>

#include <cassert>
#include <span>
#include <utility>
#include <vector>

namespace arbc {

std::vector<TileKey> zoom_prefetch_ring(const RungSelection& current, const Rect& local_region,
                                        ObjectId content, std::uint64_t revision,
                                        std::optional<Time> achieved_time, int zoom_direction) {
  if (zoom_direction == 0) {
    return {}; // no gesture -> no speculation (doc 04:99-101)
  }

  // A higher rung index is finer (`rung_scale = 2^index`, scale_ladder.hpp:79-84):
  // zoom-in (direction < 0) heads toward the finer `index + 1` rung -- the
  // downsample-preferred "next" rung once available (doc 04:95-98), more/smaller
  // tiles over the same region; zoom-out (direction > 0) toward the coarser
  // `index - 1` rung, fewer tiles.
  const ScaleRung target_rung{current.rung.index + (zoom_direction < 0 ? 1 : -1)};

  std::vector<TileKey> keys;
  for (const TileCoord coord : tiles_covering(target_rung, local_region)) {
    keys.push_back(TileKey{content, revision, target_rung, coord, achieved_time});
  }
  return keys;
}

std::vector<TileKey> prime_prefetch(TileCache& cache, const LayerTilePlan& plan, int zoom_direction,
                                    std::int32_t pan_radius) {
  // Assemble the visible tile keys and their covered local region straight from
  // the pure plan: the pan ring is the annulus around these keys, the zoom ring
  // re-tiles this region at the neighbouring rung.
  std::vector<TileKey> visible;
  visible.reserve(plan.tiles.size());
  Rect region{};
  for (const PlannedTile& tile : plan.tiles) {
    visible.push_back(tile.key);
    region = rect_union(region, tile.local_rect);
  }

  std::vector<TileKey> want;

  // Pan ring -> Adjacent. `cache::pan_prefetch_ring` owns the pure neighbour-coord
  // annulus; we only feed it the visible set and classify the result.
  const std::vector<TileKey> pan = cache::pan_prefetch_ring(visible, pan_radius);
  const std::vector<TileKey> pan_absent =
      cache::prime_ring(cache, std::span<const TileKey>(pan), PriorityClass::Adjacent);
  want.insert(want.end(), pan_absent.begin(), pan_absent.end());

  // Zoom ring -> Speculative. Revision and `achieved_time` come from the plan's
  // keys (all tiles at a rung share both); an empty plan speculates nothing.
  if (!plan.tiles.empty()) {
    const TileKey& sample = plan.tiles.front().key;
    const std::vector<TileKey> zoom =
        zoom_prefetch_ring(RungSelection{plan.rung, plan.remainder}, region, plan.content,
                           sample.revision, sample.achieved_time, zoom_direction);
    const std::vector<TileKey> zoom_absent =
        cache::prime_ring(cache, std::span<const TileKey>(zoom), PriorityClass::Speculative);
    want.insert(want.end(), zoom_absent.begin(), zoom_absent.end());
  }

  return want;
}

bool tile_in_flight(const RefinementQueue* queue, const TileKey& key) noexcept {
  if (queue == nullptr) {
    return false;
  }
  for (const PendingTile& pending : queue->tiles) {
    // Keyed on the `TileKey` alone -- the full five-field equality, so a revision
    // bump, a rung change, a different coord or a different achieved_time is a
    // DIFFERENT tile and is dispatched. `PendingTile::local_rect` is deliberately
    // not consulted: the two record sites disagree about it (the driver stores the
    // tile cell, `pull` stores the whole pull region) and nothing reads it --
    // `poll_refinements` derives its damage rect from the key.
    if (pending.key == key && !pending.done->settled() && !pending.done->cancelled()) {
      return true;
    }
  }
  return false;
}

bool tile_wanted(const RefinementQueue& queue, const WantedTiles& wanted, const TileKey& key) {
  if (wanted.contains(key)) {
    return true; // in the frame's visible footprint, or named by one of its pulls
  }
  // Not visible in its own right -- but a live wave may still owe it. An operator whose
  // output the frame wants, and whose last render left `key` unmet, is waiting on this
  // tile even though it did not (and, wave-deferred, could not) re-pull it this frame.
  // Cancelling it would end nothing and strand the operator on its placeholder.
  for (const OperatorWait& wait : queue.waits) {
    if (!wanted.contains(wait.output)) {
      continue; // the frame does not want the output either: nothing to strand
    }
    for (const TileKey& unmet : wait.unmet) {
      if (unmet == key) {
        return true;
      }
    }
  }
  return false;
}

namespace {

// Is `key` an input tile the queue still owes? PRESENCE in `queue.tiles`, and nothing
// else -- not `settled()`, not `cancelled()`. The question the wave gate asks is "is
// more coming for this tile?", and everything still in this queue answers yes:
//
//  * UNSETTLED -- a worker is rendering it. Obviously more is coming.
//  * CANCELLED but unsettled -- `cancel` is advisory (`content.hpp:161-163`) and the
//    render usually lands anyway. Nobody is GUARANTEED to be rendering it, which is
//    why `tile_in_flight` (the DISPATCH gate) excludes it, but something is still
//    coming for it, which is what this gate asks.
//  * SETTLED but not yet drained -- its pixels exist, but they are not in the CACHE:
//    the insert happens in `poll_refinements`, not on the worker. So a chain
//    re-rendered against it right now would miss it in the cache, re-dispatch a render
//    that has already completed, and throw the result away. More is genuinely coming
//    for this tile -- from the very next poll -- and that poll also emits the damage
//    that re-drives the chain properly.
//
// So a tile leaves the wave exactly when it leaves the QUEUE, which is what Constraint
// 3 says and what `poll_refinements`' prune already keys on: settled-and-inserted,
// failed-and-dropped, or already-taken. `poll_refinements` runs unconditionally every
// frame, before any re-plan, so a settled entry is never held for more than the poll
// it is about to be drained by -- the gate cannot stall on one.
bool input_pending(const RefinementQueue& queue, const TileKey& key) noexcept {
  for (const PendingTile& pending : queue.tiles) {
    if (pending.key == key) {
      return true;
    }
  }
  return false;
}

} // namespace

void record_operator_wait(RefinementQueue& queue, const TileKey& output,
                          std::vector<TileKey> unmet) {
  if (unmet.empty()) {
    return; // nothing was left unmet -> no wave, and nothing to gate on
  }
  for (OperatorWait& wait : queue.waits) {
    if (wait.output == output) {
      wait.unmet = std::move(unmet); // the fresh render's unmet set supersedes
      return;
    }
  }
  queue.waits.push_back(OperatorWait{output, std::move(unmet)});
}

bool operator_wave_pending(const RefinementQueue* queue, const TileKey& output) noexcept {
  if (queue == nullptr) {
    return false;
  }
  for (const OperatorWait& wait : queue->waits) {
    if (!(wait.output == output)) {
      continue;
    }
    for (const TileKey& key : wait.unmet) {
      if (input_pending(*queue, key)) {
        return true;
      }
    }
  }
  return false;
}

std::vector<TileKey> operator_wave_unmet(const RefinementQueue* queue, const TileKey& output) {
  std::vector<TileKey> still;
  if (queue == nullptr) {
    return still;
  }
  for (const OperatorWait& wait : queue->waits) {
    if (!(wait.output == output)) {
      continue;
    }
    for (const TileKey& key : wait.unmet) {
      if (input_pending(*queue, key)) {
        still.push_back(key);
      }
    }
  }
  return still;
}

std::vector<Damage> poll_refinements(RefinementQueue& queue, TileCache& cache,
                                     CompositorCounters* counters, Backend* backend) {
  std::vector<Damage> damage;
  std::vector<PendingTile> retained;
  retained.reserve(queue.tiles.size());

  for (PendingTile& pending : queue.tiles) {
    if (!pending.done->settled()) {
      retained.push_back(std::move(pending)); // still in flight -- keep it
      continue;
    }

    const std::optional<expected<RenderResult, RenderError>> settled = pending.done->take();
    if (settled.has_value() && settled->has_value()) {
      RenderResult result = **settled;
      // Insert-key temporal linkage (doc 11:134-137): this async arrival is keyed
      // at the pre-quantized plan instant; assert the render landed on it before it
      // is cached, exactly as the inline insert sites do (a no-op for conformant
      // content, fires only on a doc-11 MUST violation).
      assert(timed_insert_key_consistent(pending.key, result, pending.stability));
      // Honor a content-provided surface arriving async (doc 09:87-100): copy it
      // into the cache-owned `pending.surface` (cleared to transparent at record
      // time, so a source-over copy is exact) and release it, exactly as the inline
      // sites do -- the cache never learns the surface was provided
      // (doc 09:109-112,328-340). Absent, the worker filled `pending.surface` and
      // there is nothing to copy. `backend` is non-null on the production drain
      // (`runtime.interactive`); a null backend only reaches here in a test with no
      // provided surface, so the copy branch is never taken without one.
      consume_render_result(result, *pending.surface, [&](const Surface& src) {
        if (&src != pending.surface.get() && backend != nullptr) {
          backend->composite(*pending.surface, src, Affine::identity(), 1.0);
        }
      });
      // The arrival is placed under its exact request key so a follow-up frame at
      // the same revision plans it Fresh (doc 02:100-104 pin), then dropped from
      // the queue by simply not retaining it.
      cache.insert(pending.key,
                   TileValue{std::move(pending.surface), {result.achieved_scale, result.exact}},
                   pending.bytes, PriorityClass::Visible);
      // Content-local damage keyed by (content, region) (doc 02:94). Time mirrors
      // model damage: the tile's requested instant for Timed content, Time::zero()
      // for the clock-invariant Static key.
      const Time when = pending.key.achieved_time.value_or(Time::zero());
      damage.push_back(Damage{pending.content, tile_local_rect(pending.key.rung, pending.key.coord),
                              TimeRange{when, when}});
      // A settled arrival that landed in the cache and emitted damage is one
      // follow-up frame (doc 02:69-71 step 6, doc 16:54-62).
      if (counters != nullptr) {
        counters->note_follow_up_frame();
      }
    }
    // A settled-via-fail (or already-taken) arrival is dropped: no insert, no
    // damage, no retain.
  }

  queue.tiles = std::move(retained);

  // Prune the waves this drain ended. A wait whose unmet set names no tile still
  // pending in the (just-rebuilt) queue is over: its inputs settled and landed in
  // the cache, or failed and were dropped, or its output key was superseded by a
  // revision bump and its inputs drained out like any others. Erasing it is
  // bookkeeping, not liveness -- `operator_wave_pending` already answers `false`
  // for a drained set, so the gate is open either way; this is what keeps `waits`
  // bounded by the operator tiles with work genuinely outstanding.
  std::erase_if(queue.waits, [&queue](const OperatorWait& wait) {
    for (const TileKey& key : wait.unmet) {
      if (input_pending(queue, key)) {
        return false;
      }
    }
    return true;
  });

  return damage;
}

} // namespace arbc
