#pragma once

#include <cstdint>

namespace arbc {

// An instant on a local time axis, counted in integer flicks
// (1/705'600'000 s — exactly divisible by all common video and audio
// rates). Rates are exact rationals composed per edge (doc 11).
struct Time {
  std::int64_t flicks{0};

  static constexpr std::int64_t flicks_per_second = 705'600'000;

  static constexpr Time zero() { return {}; }

  friend constexpr bool operator==(const Time&, const Time&) = default;
  friend constexpr auto operator<=>(const Time&, const Time&) = default;
};

// A half-open interval [start, end) on a local time axis (doc 11:67-79,
// doc 17:48): the temporal analog of a `Rect`. `Content::time_extent()` returns
// the range of local media time over which content varies or exists, or
// `nullopt` for time-invariant (`Static`) content. Trivially copyable and
// STL-free, so it stays a cheap by-value descriptor; the rational rate /
// time-map arithmetic that composes ranges across edges is `time.rational_time`
// (doc 11), which builds on this minimal type, not part of it.
struct TimeRange {
  Time start{};
  Time end{};

  // Empty iff it contains no instant -- `end <= start`. A degenerate range
  // (`end == start`) is empty; a well-formed extent has `start < end`.
  constexpr bool empty() const { return end.flicks <= start.flicks; }

  // Whether `t` lies in the half-open interval: `start <= t < end`. Always
  // false for an empty range (the upper bound is exclusive).
  constexpr bool contains(Time t) const {
    return start.flicks <= t.flicks && t.flicks < end.flicks;
  }

  friend constexpr bool operator==(const TimeRange&, const TimeRange&) = default;
};

} // namespace arbc
