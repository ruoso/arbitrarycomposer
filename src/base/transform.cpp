#include <arbc/base/transform.hpp>

#include <cmath>

namespace arbc {

std::optional<Affine> Affine::inverse() const {
  const double determinant = det();
  if (determinant == 0.0 || !std::isfinite(determinant)) {
    return std::nullopt;
  }
  const double inv_det = 1.0 / determinant;
  Affine result;
  result.a = d * inv_det;
  result.b = -b * inv_det;
  result.c = -c * inv_det;
  result.d = a * inv_det;
  result.tx = -(result.a * tx + result.c * ty);
  result.ty = -(result.b * tx + result.d * ty);
  return result;
}

double Affine::max_scale() const {
  // Larger singular value of [a c; b d]: from the eigenvalues of M^T M.
  const double trace = a * a + b * b + c * c + d * d;
  const double determinant = det();
  const double discriminant = std::max(0.0, trace * trace - 4.0 * determinant * determinant);
  return std::sqrt((trace + std::sqrt(discriminant)) / 2.0);
}

Rect Affine::map_rect(const Rect& r) const {
  const Vec2 corners[4] = {apply({r.x0, r.y0}), apply({r.x1, r.y0}), apply({r.x0, r.y1}),
                           apply({r.x1, r.y1})};
  Rect out{corners[0].x, corners[0].y, corners[0].x, corners[0].y};
  for (const Vec2& p : corners) {
    out.x0 = std::min(out.x0, p.x);
    out.y0 = std::min(out.y0, p.y);
    out.x1 = std::max(out.x1, p.x);
    out.y1 = std::max(out.y1, p.y);
  }
  return out;
}

Affine compose(const Affine& outer, const Affine& inner) {
  Affine result;
  result.a = outer.a * inner.a + outer.c * inner.b;
  result.b = outer.b * inner.a + outer.d * inner.b;
  result.c = outer.a * inner.c + outer.c * inner.d;
  result.d = outer.b * inner.c + outer.d * inner.d;
  result.tx = outer.a * inner.tx + outer.c * inner.ty + outer.tx;
  result.ty = outer.b * inner.tx + outer.d * inner.ty + outer.ty;
  return result;
}

} // namespace arbc
