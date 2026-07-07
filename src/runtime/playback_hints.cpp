#include <arbc/runtime/playback_hints.hpp>

#include <arbc/cache/keyed_store.hpp> // PriorityClass
#include <arbc/cache/prefetch.hpp>    // temporal_prefetch_ring, prime_ring

namespace arbc {

expected<PlaybackHint, TimeError> derive_playback_hint(const Transport& transport,
                                                       Time real_lookahead) {
  const Rational rate = transport.rate();

  // A paused or zero-rate transport consumes no upcoming frames: derive the empty
  // hint (direction 0, horizon 0) -- no pre-roll, an empty temporal ring. Mirrors
  // "a paused advance moves the playhead zero flicks" (doc 11:108,173). `pause` is
  // a distinct fact from `rate == 0` (doc 11:97-101), so either alone suffices.
  if (transport.is_paused() || rate.is_zero()) {
    return PlaybackHint{0, rate, Time::zero()};
  }

  // Direction is the SIGN of the rate; there is no stored direction (doc 11:167).
  const int direction = rate.num() < 0 ? -1 : 1;

  // Horizon = |rate| * real_lookahead: the real-time lookahead window scaled into
  // content time, so faster playback looks proportionally further ahead. Reuse the
  // transport's `TimeMap`-based scaling (`TimeMap{in=0, |rate|, offset=0}.evaluate(
  // real_lookahead) == round_ties_even(real_lookahead * |rate|)`) for one
  // ties-to-even leaf rounding -- sign-symmetric so reverse gives the same
  // magnitude as forward -- and an overflowing rate faults as a value rather than
  // wrapping (doc 11:170-172).
  const Rational magnitude = rate.num() < 0 ? rate.negated() : rate;
  const expected<Time, TimeError> horizon =
      TimeMap{Time{0}, magnitude, Time{0}}.evaluate(real_lookahead);
  if (!horizon) {
    return unexpected(horizon.error());
  }
  return PlaybackHint{direction, rate, horizon.value()};
}

std::vector<TileKey> drive_playback_prefetch(const PlaybackHint& hint,
                                             std::span<Content* const> participating,
                                             TileCache& cache, std::span<const TileKey> anchors,
                                             Time step) {
  // (a) Fan the hint out to the frame's participating Timed content so a decoder
  // can pre-roll sequentially. The default `Content::playback_hint` is a no-op, so
  // hint-ignoring content is byte-identical whether or not a hint is issued.
  for (Content* content : participating) {
    if (content != nullptr) {
      content->playback_hint(hint);
    }
  }

  // (b) Unpack the hint into the base-typed (direction, step, horizon) the temporal
  // ring consumes and prime the cache from each visible Timed anchor. A Static
  // anchor (no achieved_time) builds no ring; an empty hint (horizon 0) yields an
  // empty ring, so a paused transport or an all-Static scene primes nothing.
  std::vector<TileKey> want_list;
  for (const TileKey& anchor : anchors) {
    if (!anchor.achieved_time.has_value()) {
      continue; // Static key: no temporal axis, no ring (doc 11:139-140,218-219)
    }
    const std::vector<TileKey> ring =
        cache::temporal_prefetch_ring(anchor, hint.direction, step, hint.horizon);
    const std::vector<TileKey> absent =
        cache::prime_ring(cache, std::span<const TileKey>(ring), PriorityClass::Temporal);
    want_list.insert(want_list.end(), absent.begin(), absent.end());
  }
  return want_list;
}

} // namespace arbc
