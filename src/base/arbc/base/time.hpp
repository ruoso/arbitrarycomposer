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

} // namespace arbc
