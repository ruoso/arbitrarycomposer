#pragma once

#include <arbc/base/expected.hpp>      // expected, unexpected
#include <arbc/base/rational_time.hpp> // Rational, TimeMap, TimeError
#include <arbc/base/time.hpp>          // Time
#include <arbc/cache/key_shapes.hpp>   // TileKey, TileCache
#include <arbc/contract/content.hpp>   // Content, PlaybackHint
#include <arbc/runtime/transport.hpp>  // Transport

#include <span>
#include <vector>

// The runtime playback-hint path (doc 11:160-178, doc 17:60): the L5 derivation
// and drive that wire the per-viewport `Transport` (the producer) to the temporal
// prefetch ring in `arbc::cache` (the consumer). Both ends already exist -- the
// transport exposes `rate()`/`is_paused()`, `cache::temporal_prefetch_ring`
// already accepts `direction`/`step`/`horizon` -- so this is the missing wire, not
// new machinery.
//
// `derive_playback_hint` turns transport state into the advisory
// `(direction, rate, horizon)` triple `Content::playback_hint` carries -- a pure
// function of the transport and a runtime real-time lookahead window, with the
// paused/reverse/zero-rate edge cases and the exact rational rate-scaled horizon
// settled here (the design-doc delta at doc 11:166-174). `drive_playback_prefetch`
// fans that hint out to the frame's participating `Timed` content and unpacks it
// into the `base`-typed `(direction, step, horizon)` the temporal ring consumes,
// priming the cache from the visible `Timed` anchor key(s) and returning the
// absent want-list for the pull service to render opportunistically.
//
// Levelization (doc 17:54-60): the derivation/drive are `arbc::runtime` (L5) --
// the transport lives here, and the compositor reserves the temporal ring for
// "the runtime transport path" (`compositor/refinement.hpp:52-53`). The
// `PlaybackHint` value and the `Content::playback_hint` method are `arbc::contract`
// (L3, on the `Content` vtable). The cache (L3, deps base+surface) is never handed
// a `PlaybackHint`: the drive UNPACKS it into `base` scalars at the
// `temporal_prefetch_ring` call site. Runtime's already-permitted `cache` dep is
// the only build-graph change (`check_levels.py`); no new levelization edge.
//
// The hint is advisory end to end (doc 11:175-178): the default
// `Content::playback_hint` is a no-op, so hint-ignoring content is byte-identical
// whether or not a hint is issued; priming is residency-only, so a drive leaves
// `resident_bytes()`/`evictions()` unchanged. Determinism stays owned by
// `quantize_time`/`achieved_time`, not by whether a hint was issued or honored.

namespace arbc {

// Derive the advisory playback hint from transport state and a runtime real-time
// lookahead window (doc 11:166-174). `direction` is the SIGN of `rate` (`+1`
// forward, `-1` reverse); `horizon` is `|rate| * real_lookahead` scaled from real
// time into content time in exact rational arithmetic with one ties-to-even leaf
// rounding, reusing the transport's `TimeMap`-based scaling so reverse playback
// gives the same magnitude as forward. A PAUSED or zero-rate transport derives the
// EMPTY hint (`direction 0`, `horizon 0`): no motion, no pre-roll, an empty
// temporal ring. A pathological rate faults as a `TimeError` value (never wraps),
// matching the transport's advance contract. `real_lookahead` is a non-negative
// real-time window the caller supplies.
expected<PlaybackHint, TimeError> derive_playback_hint(const Transport& transport,
                                                       Time real_lookahead);

// Drive the temporal prefetch from a derived hint over a warm `TileCache`
// (doc 11:141-149, 175-178). Two effects:
//   (a) fan `hint` out to each participating `Timed` content via
//       `Content::playback_hint` so a decoder can pre-roll sequentially (a no-op
//       for hint-ignoring content); and
//   (b) for each visible Timed anchor key (one carrying an `achieved_time`), build
//       the ring `temporal_prefetch_ring(anchor, hint.direction, step,
//       hint.horizon)`, `prime_ring(..., Temporal)` it, and collect the absent
//       members as the want-list.
// `step` (the native frame period / quantization grid of the visible content) is
// supplied by the plan-aware caller, exactly as `cache/prefetch.hpp:27-33`
// specifies -- the drive does not probe content cadence. A `Static` anchor
// (`achieved_time == nullopt`) builds no ring, so an all-Static scene under a
// playing transport (and an empty/paused hint, `horizon 0`) returns an empty
// want-list. Priming is residency-only: `resident_bytes()` and `evictions()` are
// unchanged across the call. Returns the absent temporal-neighbour keys, in anchor
// then ring order (the pull service renders and inserts them under `Temporal`).
std::vector<TileKey> drive_playback_prefetch(const PlaybackHint& hint,
                                             std::span<Content* const> participating,
                                             TileCache& cache, std::span<const TileKey> anchors,
                                             Time step);

} // namespace arbc
