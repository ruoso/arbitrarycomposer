#pragma once

#include <algorithm>

namespace arbc {

struct Vec2 {
  double x{0.0};
  double y{0.0};
};

// Axis-aligned rectangle, half-open in spirit: a rect is empty unless
// x0 < x1 and y0 < y1.
struct Rect {
  double x0{0.0};
  double y0{0.0};
  double x1{0.0};
  double y1{0.0};

  static constexpr Rect from_size(double width, double height) { return {0.0, 0.0, width, height}; }

  constexpr double width() const { return x1 - x0; }
  constexpr double height() const { return y1 - y0; }
  constexpr bool empty() const { return !(x0 < x1 && y0 < y1); }

  constexpr Rect intersect(const Rect& other) const {
    return {std::max(x0, other.x0), std::max(y0, other.y0), std::min(x1, other.x1),
            std::min(y1, other.y1)};
  }

  friend constexpr bool operator==(const Rect&, const Rect&) = default;
};

} // namespace arbc
