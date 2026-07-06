#pragma once

#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/cache/key_shapes.hpp>

#include <cstddef>
#include <cstdint>

// The tile-cache invalidation layer (doc 02:87-116, doc 05:78-91). The tile
// cache is inert without it: once a leaf is edited or a sub-region is damaged,
// stale tiles must stop being served or the compositor renders wrong pixels
// (doc 02:62-65). Doc 02:94-95 names two distinct triggers -- *"Damage
// invalidates by `(content id, region)` across all rungs; revision bumps
// invalidate wholesale by making old keys unreachable."* -- and this header
// turns each into a concrete cache operation.
//
// Everything here is a thin, key-shape-aware wrapper over the generic
// `KeyedStore::remove_if(Pred)` seam (`cache.keyed_store`): the store stays
// key-agnostic and all knowledge that a `TileKey` has a `.content`/`.revision`
// field lives here. Routing through `remove_if` means every match inherits the
// store's pin-defer discipline -- a pinned tile hit by invalidation goes
// immediately unreachable to new lookups while its surface is held until the
// last in-flight composite unpins it (doc 02:100-104).
//
// Levelization (doc 17:54): `cache` is Level 3, deps `base`+`surface` only.
// The scale ladder / tile geometry is compositor-owned (L4, doc 17:56; base
// geometry supplies no integer grid, `key_shapes.hpp:37-38`), so region damage
// injects a caller-supplied `Geom: Rect(ScaleRung, TileCoord)` rather than
// embedding a ladder here -- keeping the cache out of `contract`/`model`.

namespace arbc::cache {

// (content, region) damage across ALL resident rungs (doc 02:94-95). Drops
// every tile of `content` whose content-space footprint intersects `region`,
// at *every* scale rung it is cached at -- the reason this must enumerate live
// keys (only the cache knows which rungs hold resident tiles for a content)
// rather than have the compositor pre-compute a tile set. `tile_rect` maps a
// tile's (rung, coord) to its content-space `Rect`; the compositor supplies it
// from its scale ladder.
//
// Match is revision- and achieved_time-agnostic: spatial damage to a content
// supersedes that region at *every* revision and playback time. This is a
// sound over-approximation (under-approximation would "drop repaint",
// `content.hpp:228`), so dropping matching tiles at all revisions/times is the
// conservative, correct choice. Returns the number of tiles removed.
template <class Geom> // Geom: Rect(ScaleRung, TileCoord)
std::size_t invalidate_region(TileCache& cache, ObjectId content, const Rect& region,
                              Geom&& tile_rect) {
  return cache.remove_if([&](const TileKey& key) {
    if (!(key.content == content)) {
      return false;
    }
    const Rect footprint = tile_rect(key.rung, key.coord);
    return !footprint.intersect(region).empty();
  });
}

// Wholesale: drop every resident tile of `content` -- all revisions, rungs, and
// achieved times. For content deletion or coarse structural damage where no
// tile of the content survives. Returns the number of tiles removed.
inline std::size_t invalidate_content(TileCache& cache, ObjectId content) {
  return cache.remove_if([&](const TileKey& key) { return key.content == content; });
}

// Opt-in eager reclaim of superseded revisions: drop every tile of `content`
// whose `revision < live_revision`. A revision bump is otherwise lazy -- a
// bumped revision simply produces fresh keys, and the prior-revision tiles stay
// lookup-able as the "stale-revision" fallback tier (doc 02:62-65,:84-85) until
// LRU reclaims them. This is the compositor's explicit choice to reclaim those
// bytes once the stale tiles are no longer wanted as fallback; it is NOT fired
// on the bump hot path. Returns the number of tiles removed.
//
// Because an aggregate revision is just a `std::uint64_t` in the same
// `TileKey::revision` slot (doc 05:78-86), a composed-result tile is reclaimed
// by this exact machinery when its child bumps the aggregate -- no
// cache-specific composite mechanism is needed.
inline std::size_t drop_superseded(TileCache& cache, ObjectId content,
                                   std::uint64_t live_revision) {
  return cache.remove_if(
      [&](const TileKey& key) { return key.content == content && key.revision < live_revision; });
}

} // namespace arbc::cache
