#include <arbc/base/ids.hpp>
#include <arbc/base/rational_time.hpp>
#include <arbc/base/time.hpp>
#include <arbc/cache/key_shapes.hpp>
#include <arbc/cache/keyed_store.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/runtime/playback_hints.hpp>
#include <arbc/runtime/transport.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <vector>

// Unit + behavioral-counter tests for the runtime playback-hint path
// (`derive_playback_hint` / `drive_playback_prefetch`, doc 11:160-178, 17:60).
// The derivation is wall-clock-free and exact, so its assertions are tier-2
// exact-equality checks; the drive is a tier-4 behavioral-counter test over a warm
// `TileCache` (residency-only priming: `resident_bytes()`/`evictions()` never
// move). No golden applies -- the path produces advisory hints, not pixels.

namespace {

using arbc::PlaybackHint;
using arbc::PriorityClass;
using arbc::Rational;
using arbc::TileCache;
using arbc::TileKey;
using arbc::TileValue;
using arbc::Time;
using arbc::TimeError;
using arbc::Transport;

// A Timed tile key for content 1 (rung 0, coord 0,0) at the given achieved_time --
// the visible anchor the temporal ring walks along its achieved_time axis.
TileKey timed_tile(std::int64_t at) {
  return TileKey{arbc::ObjectId{1}, 0, arbc::ScaleRung{0}, arbc::TileCoord{0, 0}, Time{at}};
}

// A Static tile key (no achieved_time): it carries no temporal axis, so the drive
// builds no ring for it.
TileKey static_tile(std::int32_t col, std::int32_t row) {
  return TileKey{arbc::ObjectId{1}, 0, arbc::ScaleRung{0}, arbc::TileCoord{col, row}, std::nullopt};
}

// The store never inspects the value, so a null-surface `TileValue` at an explicit
// byte cost is a faithful resident tile without linking a backend surface.
void warm(TileCache& cache, const TileKey& key, PriorityClass klass) {
  cache.insert(key, TileValue{nullptr, {}}, 40, klass);
}

// A hint-recording Timed content double: it records every delivered hint and
// counts renders (which the drive must never trigger). No real decoder -- that
// arrives with `kinds.imageseq_plugin`.
class RecordingContent : public arbc::Content {
public:
  std::optional<arbc::Rect> bounds() const override { return std::nullopt; }
  arbc::Stability stability() const override { return arbc::Stability::Timed; }
  std::optional<arbc::TimeRange> time_extent() const override {
    return arbc::TimeRange{Time::zero(), Time{1000}};
  }
  std::optional<arbc::RenderResult> render(const arbc::RenderRequest&,
                                           std::shared_ptr<arbc::RenderCompletion>) override {
    ++renders;
    return arbc::RenderResult{};
  }
  void playback_hint(const PlaybackHint& h) override { hints.push_back(h); }

  std::vector<PlaybackHint> hints;
  int renders = 0;
};

} // namespace

// enforces: 11-time-and-video#playback-hint-derives-direction-from-rate-sign
TEST_CASE("derive_playback_hint takes direction from the rate sign; paused/zero -> empty hint") {
  // Forward rate -> direction +1.
  {
    Transport t;
    t.set_rate(Rational(3, 2));
    const auto h = arbc::derive_playback_hint(t, Time{100});
    REQUIRE(h.has_value());
    CHECK(h->direction == +1);
  }
  // Reverse rate -> direction -1.
  {
    Transport t;
    t.set_rate(Rational(-3, 2));
    const auto h = arbc::derive_playback_hint(t, Time{100});
    REQUIRE(h.has_value());
    CHECK(h->direction == -1);
  }
  // Paused (with a retained non-zero rate) -> the empty hint: direction 0,
  // horizon 0, regardless of the rate the transport still carries.
  {
    Transport t;
    t.set_rate(Rational(2, 1));
    t.pause();
    const auto h = arbc::derive_playback_hint(t, Time{100});
    REQUIRE(h.has_value());
    CHECK(h->direction == 0);
    CHECK(h->horizon == Time::zero());
  }
  // Zero rate (playing-but-frozen, distinct from paused) -> the empty hint.
  {
    Transport t;
    t.set_rate(Rational(0, 1));
    const auto h = arbc::derive_playback_hint(t, Time{100});
    REQUIRE(h.has_value());
    CHECK(h->direction == 0);
    CHECK(h->horizon == Time::zero());
  }
}

// enforces: 11-time-and-video#playback-hint-horizon-scales-with-rate
TEST_CASE("derive_playback_hint scales the horizon by |rate| with one ties-to-even rounding") {
  // 1x rate: horizon == the real-time lookahead itself.
  {
    Transport t;
    t.set_rate(Rational(1, 1));
    const auto h = arbc::derive_playback_hint(t, Time{1000});
    REQUIRE(h.has_value());
    CHECK(h->horizon == Time{1000});
  }
  // Doubling the rate doubles the content-time horizon.
  {
    Transport t;
    t.set_rate(Rational(2, 1));
    const auto h = arbc::derive_playback_hint(t, Time{1000});
    REQUIRE(h.has_value());
    CHECK(h->horizon == Time{2000});
  }
  // Reverse (rate < 0) gives the SAME horizon magnitude as forward.
  {
    Transport fwd;
    fwd.set_rate(Rational(2, 1));
    Transport rev;
    rev.set_rate(Rational(-2, 1));
    const auto hf = arbc::derive_playback_hint(fwd, Time{1000});
    const auto hr = arbc::derive_playback_hint(rev, Time{1000});
    REQUIRE(hf.has_value());
    REQUIRE(hr.has_value());
    CHECK(hr->direction == -1);
    CHECK(hr->horizon == hf->horizon); // magnitude unbiased by sign
    CHECK(hr->horizon == Time{2000});
  }
  // A realistic NTSC-ish rate lands exact when the lookahead clears the denominator.
  {
    Transport t;
    t.set_rate(Rational(24000, 1001));
    const auto h = arbc::derive_playback_hint(t, Time{1001});
    REQUIRE(h.has_value());
    CHECK(h->horizon == Time{24000});
  }
  // Ties-to-even leaf rounding on a sub-flick remainder: |rate| 1/2 rounds
  // 0.5 -> 0 and 1.5 -> 2 (both to even), just as the transport advance does.
  {
    Transport half;
    half.set_rate(Rational(1, 2));
    const auto h1 = arbc::derive_playback_hint(half, Time{1});
    REQUIRE(h1.has_value());
    CHECK(h1->horizon == Time{0}); // 0.5 -> 0
    Transport half3;
    half3.set_rate(Rational(1, 2));
    const auto h3 = arbc::derive_playback_hint(half3, Time{3});
    REQUIRE(h3.has_value());
    CHECK(h3->horizon == Time{2}); // 1.5 -> 2
  }
  // A pathological rate that overflows the flick width faults as a value, never
  // wraps -- matching the transport advance contract.
  {
    constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    Transport t;
    t.set_rate(Rational(kMax, 1));
    const auto h = arbc::derive_playback_hint(t, Time{kMax});
    REQUIRE_FALSE(h.has_value());
    CHECK(h.error() == TimeError{TimeError::Kind::Overflow});
  }
}

// enforces: 11-time-and-video#playback-prefetch-drives-temporal-ring
TEST_CASE("drive_playback_prefetch primes the temporal ring from a transport-derived hint") {
  // A transport-derived hint: forward, horizon 30 (rate 2x over a 15-flick real
  // lookahead), so with step 10 the ring is exactly K = 30/10 = 3 buckets forward.
  Transport transport;
  transport.set_rate(Rational(2, 1));
  const auto hint = arbc::derive_playback_hint(transport, Time{15});
  REQUIRE(hint.has_value());
  REQUIRE(hint->direction == +1);
  REQUIRE(hint->horizon == Time{30});
  const Time step{10};

  // The visible Timed anchor at achieved_time 1000; its forward temporal
  // neighbours are 1010, 1020, 1030 (none reverse, none beyond the horizon).
  const TileKey anchor = timed_tile(1000);
  const std::array<TileKey, 1> anchors{anchor};

  RecordingContent content;
  const std::array<arbc::Content*, 1> participating{&content};

  TileCache cache(10'000);
  // Warm: neighbours 1010 and 1030 resident (Speculative), 1020 absent; plus a
  // non-ring Speculative filler to prove reclassification under pressure below.
  warm(cache, timed_tile(1010), PriorityClass::Speculative);
  warm(cache, timed_tile(1030), PriorityClass::Speculative);
  warm(cache, timed_tile(7777), PriorityClass::Speculative);

  const std::size_t bytes_before = cache.resident_bytes();
  const std::uint64_t evictions_before = cache.evictions();

  const std::vector<TileKey> want = arbc::drive_playback_prefetch(
      *hint, participating, cache, std::span<const TileKey>(anchors), step);

  // (a) The want-list is exactly the absent ring member 1020; 1010/1030 were
  // resident and reclassified, not reported. The ring is bounded by the derived
  // horizon (nothing reverse of 1000, nothing beyond 1030).
  REQUIRE(want.size() == 1);
  CHECK(want[0] == timed_tile(1020));
  CHECK(want[0].achieved_time->flicks > 1000);  // forward of the anchor
  CHECK(want[0].achieved_time->flicks <= 1030); // within the horizon

  // (b) Priming is residency-only: nothing rendered, inserted, or evicted.
  CHECK(cache.resident_bytes() == bytes_before);
  CHECK(cache.evictions() == evictions_before);
  CHECK(content.renders == 0); // the advisory hint issued zero renders

  // (c) The hint was fanned out to the participating content verbatim.
  REQUIRE(content.hints.size() == 1);
  CHECK(content.hints[0].direction == +1);
  CHECK(content.hints[0].horizon == Time{30});

  // (d) The resident ring members were reclassified onto Temporal: under budget
  // pressure the still-Speculative filler is the victim, the retagged neighbours
  // survive (Temporal outranks Speculative).
  TileCache tight(120); // holds exactly 3 * 40
  warm(tight, timed_tile(1010), PriorityClass::Speculative);
  warm(tight, timed_tile(1030), PriorityClass::Speculative);
  warm(tight, timed_tile(7777), PriorityClass::Speculative);
  arbc::drive_playback_prefetch(*hint, participating, tight, std::span<const TileKey>(anchors),
                                step);
  warm(tight, timed_tile(5555), PriorityClass::Visible); // forces one eviction
  CHECK(tight.evictions() == 1);
  CHECK_FALSE(tight.lookup(timed_tile(7777)).has_value()); // Speculative filler evicted
  CHECK(tight.lookup(timed_tile(1010)).has_value());       // retagged Temporal, spared
  CHECK(tight.lookup(timed_tile(1030)).has_value());       // retagged Temporal, spared
}

// enforces: 11-time-and-video#playback-prefetch-drives-temporal-ring
TEST_CASE("drive_playback_prefetch over a Static-only scene builds no ring (still-scene)") {
  // A PLAYING transport over an all-Static warm scene builds an empty temporal
  // ring: zero want-list entries, nothing reclassified, zero renders (doc 16:60).
  Transport transport; // playing, rate 1/1
  const auto hint = arbc::derive_playback_hint(transport, Time{1000});
  REQUIRE(hint.has_value());
  REQUIRE(hint->direction == +1);

  RecordingContent content;
  const std::array<arbc::Content*, 1> participating{&content};
  // A Static anchor carries no achieved_time -- no temporal axis to walk.
  const std::array<TileKey, 1> anchors{static_tile(0, 0)};

  TileCache cache(10'000);
  warm(cache, static_tile(0, 0), PriorityClass::Visible);
  const std::size_t bytes_before = cache.resident_bytes();

  const std::vector<TileKey> want = arbc::drive_playback_prefetch(
      *hint, participating, cache, std::span<const TileKey>(anchors), Time{10});

  CHECK(want.empty());                           // Static anchor -> no ring
  CHECK(cache.resident_bytes() == bytes_before); // nothing touched
  CHECK(cache.evictions() == 0);
  CHECK(content.renders == 0);        // zero renders
  REQUIRE(content.hints.size() == 1); // participating content still notified once
}

// enforces: 11-time-and-video#playback-prefetch-drives-temporal-ring
TEST_CASE("drive_playback_prefetch under a paused (empty) hint primes nothing") {
  // A paused transport derives the empty hint (horizon 0) -> an empty ring even
  // over a Timed anchor: no want-list, no reclassification, no renders.
  Transport transport;
  transport.set_rate(Rational(2, 1));
  transport.pause();
  const auto hint = arbc::derive_playback_hint(transport, Time{1000});
  REQUIRE(hint.has_value());
  REQUIRE(hint->direction == 0);
  REQUIRE(hint->horizon == Time::zero());

  RecordingContent content;
  const std::array<arbc::Content*, 1> participating{&content};
  const std::array<TileKey, 1> anchors{timed_tile(1000)};

  TileCache cache(10'000);
  warm(cache, timed_tile(1010), PriorityClass::Speculative);
  const std::size_t bytes_before = cache.resident_bytes();

  const std::vector<TileKey> want = arbc::drive_playback_prefetch(
      *hint, participating, cache, std::span<const TileKey>(anchors), Time{10});

  CHECK(want.empty()); // empty hint -> empty ring, even for a Timed anchor
  CHECK(cache.resident_bytes() == bytes_before);
  CHECK(cache.evictions() == 0);
  CHECK(content.renders == 0);
  REQUIRE(content.hints.size() == 1); // still notified: a decoder may stop pre-roll
}
