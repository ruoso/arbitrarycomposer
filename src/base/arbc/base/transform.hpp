#pragma once

#include <arbc/base/geometry.hpp>

#include <optional>

namespace arbc {

// 2D affine transform mapping (x, y) -> (a*x + c*y + tx, b*x + d*y + ty).
// Transforms are stored per edge and composed on demand, never accumulated
// (doc 04).
struct Affine {
  double a{1.0};
  double b{0.0};
  double c{0.0};
  double d{1.0};
  double tx{0.0};
  double ty{0.0};

  static constexpr Affine identity() { return {}; }
  static constexpr Affine translation(double dx, double dy) { return {1.0, 0.0, 0.0, 1.0, dx, dy}; }
  static constexpr Affine scaling(double sx, double sy) { return {sx, 0.0, 0.0, sy, 0.0, 0.0}; }

  constexpr Vec2 apply(Vec2 p) const { return {a * p.x + c * p.y + tx, b * p.x + d * p.y + ty}; }
  constexpr double det() const { return a * d - b * c; }

  // nullopt when the transform is degenerate or non-finite; callers cull
  // rather than propagate NaNs (doc 04).
  std::optional<Affine> inverse() const;

  // The larger singular value of the linear part: the resolution a layer
  // must render at so quality suffices along the most-magnified axis
  // (doc 01).
  double max_scale() const;

  // Axis-aligned bounding box of the four mapped corners.
  Rect map_rect(const Rect& r) const;

  friend constexpr bool operator==(const Affine&, const Affine&) = default;
};

// compose(outer, inner).apply(p) == outer.apply(inner.apply(p))
Affine compose(const Affine& outer, const Affine& inner);

} // namespace arbc
