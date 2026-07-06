#include <arbc/base/expected.hpp>
#include <arbc/compositor/refinement.hpp>

#include <span>
#include <utility>

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

std::vector<Damage> poll_refinements(RefinementQueue& queue, TileCache& cache) {
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
      const RenderResult result = **settled;
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
                              when, when});
    }
    // A settled-via-fail (or already-taken) arrival is dropped: no insert, no
    // damage, no retain.
  }

  queue.tiles = std::move(retained);
  return damage;
}

} // namespace arbc
