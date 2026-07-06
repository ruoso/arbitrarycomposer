#pragma once

#include <arbc/base/time.hpp>
#include <arbc/cache/key_shapes.hpp>
#include <arbc/cache/keyed_store.hpp>

#include <cstdint>
#include <span>
#include <unordered_set>
#include <vector>

// The tile/block-cache prefetch layer (doc 02:87-116, doc 11:138-149). It turns
// the store's flat priority-class LRU into a *predictive residency policy* with
// two prefetch rings: the spatial pan ring (doc 02:92-93 "adjacent (pan
// prefetch ring)") and the temporal ring (doc 11:141-149 "upcoming times in
// playback direction ... the playback-hint horizon bounds it").
//
// Like `cache.invalidation`, everything here is a header-only free-function
// layer in `arbc::cache` over the store's public surface (`lookup` /
// `reclassify`); the store stays key-agnostic and all key-shape knowledge lives
// here. The cache is passive: prefetch is a *survival + want-list* operation --
// it may `reclassify` already-resident entries onto a ring's class and it
// reports the *absent* keys for the caller to render + `insert`, but it renders
// nothing and inserts nothing. A prime_ring call therefore leaves
// `resident_bytes()` and `evictions()` unchanged.
//
// Levelization (doc 17:54): `cache` is Level 3, deps `base`+`surface` only. The
// transport's `playback_hint` and the compositor's achieved-time quantization
// live at/above L3, so this layer may not name them: the playback `direction`,
// the achieved-time `step`, and the `horizon` all arrive as plain `base` data
// (`Time` flicks / a direction sign), and the visible tile set arrives as
// `TileKey` values (doc 11:115-121, key_shapes.md:144-146). The compositor
// reads the hint and passes the resulting values in; the cache never calls up.

namespace arbc {

// Temporal-axis customization point (ADL-found in `arbc`, the namespace of the
// key shapes). Returns the key `buckets` steps forward along its temporal axis
// (`buckets` carries the playback direction: negative walks backward). Each key
// shape defines its own axis so the generic temporal ring (below) stays
// engine-agnostic (doc 17:73-74). `TileKey`'s axis is `achieved_time` (a
// `Time`, advanced by `buckets * step`); `BlockKey`'s is `block_index` (one
// bucket == one block, so `step` -- an audio quantum -- only sizes the horizon).
inline TileKey prefetch_temporal_step(const TileKey& base, std::int64_t buckets, Time step) {
  // Precondition: a temporal ring is only meaningful for a Timed key (one that
  // carries an achieved_time); the compositor only builds rings for Timed
  // content. Static keys (nullopt) are never passed here.
  TileKey key = base;
  key.achieved_time = Time{base.achieved_time->flicks + buckets * step.flicks};
  return key;
}

inline BlockKey prefetch_temporal_step(const BlockKey& base, std::int64_t buckets, Time /*step*/) {
  BlockKey key = base;
  key.block_index = base.block_index + buckets;
  return key;
}

} // namespace arbc

namespace arbc::cache {

// The spatial pan-prefetch ring (doc 02:92-93): every tile within Chebyshev
// `radius` tiles of the visible set, *excluding* the visible set itself -- the
// annulus a pan is about to reveal at the leading edge. Pure tile-coordinate
// arithmetic on the injected visible keys (same content / revision / rung /
// achieved_time, neighbouring `coord`); the result is deduplicated and stable
// in enumeration order. A non-positive `radius` yields an empty ring. This is
// inherently 2D and so `TileKey`-specific (doc 17:73-74: audio prefetch is
// purely temporal, with no pan analogue).
inline std::vector<TileKey> pan_prefetch_ring(std::span<const TileKey> visible,
                                              std::int32_t radius) {
  std::unordered_set<TileKey> visible_set(visible.begin(), visible.end());
  std::unordered_set<TileKey> emitted;
  std::vector<TileKey> ring;
  for (const TileKey& v : visible) {
    for (std::int32_t dr = -radius; dr <= radius; ++dr) {
      for (std::int32_t dc = -radius; dc <= radius; ++dc) {
        if (dc == 0 && dr == 0) {
          continue; // the visible tile itself, never a ring member
        }
        TileKey neighbour = v;
        neighbour.coord.col = v.coord.col + dc;
        neighbour.coord.row = v.coord.row + dr;
        if (visible_set.count(neighbour) != 0) {
          continue; // covered by the visible set -- excluded from the annulus
        }
        if (emitted.insert(neighbour).second) {
          ring.push_back(neighbour);
        }
      }
    }
  }
  return ring;
}

// The temporal prefetch ring (doc 11:141-149): the keys at each upcoming
// temporal bucket `base (+/-) step*k`, `k = 1..`, walked in the playback
// `direction` (+1 forward / -1 reverse) while `|step*k| <= horizon`. The
// horizon bounds it to `K = horizon / step` buckets; no bucket beyond the
// horizon and none in the reverse direction is ever enumerated. All time
// arithmetic is on the injected `base::Time` values the compositor supplies
// (the cache neither quantizes time nor knows frame cadence, doc 11:110-121).
// Templated over the key so `audio-engine` reuses it for `BlockKey` (the axis
// itself is `prefetch_temporal_step`, specialized per key shape).
//
// A non-positive `step` or `horizon` yields an empty ring (nothing is
// enumerable). `direction` is taken by sign, so only its orientation matters.
template <class Key>
std::vector<Key> temporal_prefetch_ring(const Key& base, int direction, Time step, Time horizon) {
  std::vector<Key> ring;
  if (step.flicks <= 0 || horizon.flicks <= 0) {
    return ring;
  }
  const std::int64_t max_k = horizon.flicks / step.flicks; // |step*k| <= horizon
  const std::int64_t sign = direction < 0 ? -1 : 1;
  ring.reserve(static_cast<std::size_t>(max_k));
  for (std::int64_t k = 1; k <= max_k; ++k) {
    ring.push_back(prefetch_temporal_step(base, sign * k, step));
  }
  return ring;
}

// Classify-resident / report-absent driver (doc 02, doc 11). For a ring's key
// set and its target `klass`, `reclassify` every already-resident member onto
// `klass` (pan ring -> `Adjacent`, zoom rung -> `Speculative`, temporal ring ->
// `Temporal`) and return the *absent* members -- the want-list the caller
// (compositor / audio-engine) renders and inserts under the same class. Renders
// nothing, inserts nothing: `resident_bytes()` and `evictions()` are unchanged
// across the call. Templated over key/value so both `TileCache` and a future
// `BlockCache` drive it (doc 17:73-74).
template <class Key, class Value>
std::vector<Key> prime_ring(KeyedStore<Key, Value>& store, std::span<const Key> ring,
                            PriorityClass klass) {
  std::vector<Key> absent;
  for (const Key& key : ring) {
    // lookup is the only residency probe the store exposes; a hit yields a
    // transient hold (dropped at the end of the iteration) and we retag it,
    // a miss lands the key on the want-list.
    if (store.lookup(key).has_value()) {
      store.reclassify(key, klass);
    } else {
      absent.push_back(key);
    }
  }
  return absent;
}

} // namespace arbc::cache
