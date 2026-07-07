#include <arbc/base/rational_time.hpp>

#include <cassert>
#include <cstdint>
#include <limits>

namespace arbc {

namespace {

using i128 = rational_i128;
using u128 = rational_u128;

constexpr u128 gcd_u128(u128 a, u128 b) {
  while (b != 0) {
    const u128 t = a % b;
    a = b;
    b = t;
  }
  return a;
}

constexpr u128 magnitude_i128(i128 v) {
  return v < 0 ? u128(0) - static_cast<u128>(v) : static_cast<u128>(v);
}

TimeError overflow() { return TimeError{TimeError::Kind::Overflow}; }

} // namespace

expected<Rational, TimeError> Rational::from_i128(i128 num, i128 den) {
  assert(den != 0 && "Rational denominator must be non-zero");
  const bool negative = (num < 0) != (den < 0);
  u128 an = magnitude_i128(num);
  u128 ad = magnitude_i128(den);
  const u128 g = gcd_u128(an, ad); // ad != 0 so g >= 1
  an /= g;
  ad /= g;
  constexpr u128 kMaxMag = static_cast<u128>(std::numeric_limits<std::int64_t>::max());
  if (an > kMaxMag || ad > kMaxMag) {
    return unexpected(overflow());
  }
  const std::int64_t rn = negative ? -static_cast<std::int64_t>(an) : static_cast<std::int64_t>(an);
  const std::int64_t rd = static_cast<std::int64_t>(ad);
  return Rational(raw_tag{}, rn, rd);
}

expected<Rational, TimeError> Rational::mul(const Rational& other) const {
  // Products of two int64 fit in 128 bits (< 2^127), so the only failure is the
  // reduced result exceeding int64.
  return from_i128(static_cast<i128>(d_num) * other.d_num,
                   static_cast<i128>(d_den) * other.d_den);
}

expected<Rational, TimeError> Rational::add(const Rational& other) const {
  // Each cross-product is < 2^126 and their sum < 2^127, so the 128-bit
  // intermediates never overflow for canonical int64 operands.
  const i128 num = static_cast<i128>(d_num) * other.d_den +
                   static_cast<i128>(other.d_num) * d_den;
  const i128 den = static_cast<i128>(d_den) * other.d_den;
  return from_i128(num, den);
}

expected<ComposedTimeMap, TimeError> ComposedTimeMap::from(const TimeMap& edge) {
  // a = rate; b = offset - in * rate.
  const Rational in_r(edge.in.flicks, 1);
  const Rational off_r(edge.offset.flicks, 1);
  const auto in_scaled = in_r.mul(edge.rate);
  if (!in_scaled) {
    return unexpected(in_scaled.error());
  }
  const auto b = off_r.add(in_scaled->negated());
  if (!b) {
    return unexpected(b.error());
  }
  return ComposedTimeMap{edge.rate, *b};
}

expected<ComposedTimeMap, TimeError> ComposedTimeMap::and_then(const ComposedTimeMap& other) const {
  // other applied after this: y = other.a * (a*p + b) + other.b
  //                             = (other.a * a) * p + (other.a * b + other.b)
  const auto na = other.a.mul(a);
  if (!na) {
    return unexpected(na.error());
  }
  const auto scaled_b = other.a.mul(b);
  if (!scaled_b) {
    return unexpected(scaled_b.error());
  }
  const auto nb = scaled_b->add(other.b);
  if (!nb) {
    return unexpected(nb.error());
  }
  return ComposedTimeMap{*na, *nb};
}

expected<ComposedTimeMap, TimeError> ComposedTimeMap::then(const TimeMap& edge) const {
  const auto sub = from(edge);
  if (!sub) {
    return unexpected(sub.error());
  }
  return and_then(*sub);
}

expected<ComposedTimeMap, TimeError> ComposedTimeMap::compose(const TimeMap* edges,
                                                             std::size_t count) {
  ComposedTimeMap acc = identity();
  for (std::size_t i = 0; i < count; ++i) {
    const auto next = acc.then(edges[i]);
    if (!next) {
      return unexpected(next.error());
    }
    acc = *next;
  }
  return acc;
}

expected<Time, TimeError> ComposedTimeMap::evaluate(Time parent_time) const {
  // local = a*parent + b = (a.num*parent)/a.den + b.num/b.den
  //       = n / d  with  n = a.num*parent*b.den + b.num*a.den,  d = a.den*b.den.
  // a.num*parent is a product of two int64, so it fits 128 bits; the remaining
  // multiply/add are overflow-checked so an adversarial instant faults rather
  // than wrapping.
  const i128 an = a.num();
  const i128 ad = a.den();
  const i128 bn = b.num();
  const i128 bd = b.den();

  i128 term = an * static_cast<i128>(parent_time.flicks); // fits: |.| < 2^126
  if (__builtin_mul_overflow(term, bd, &term)) {
    return unexpected(overflow());
  }
  i128 n = 0;
  if (__builtin_add_overflow(term, bn * ad, &n)) { // bn*ad fits (< 2^126)
    return unexpected(overflow());
  }
  // d > 0 and fits: canonical denominators are <= INT64_MAX, so ad*bd < 2^126.
  const i128 d = ad * bd;

  // Floor division: q = floor(n/d), 0 <= r < d.
  i128 q = n / d;
  i128 r = n % d;
  if (r < 0) {
    q -= 1;
    r += d;
  }
  // Round to nearest, ties to even. Candidates are q (floor) and q+1; compare
  // 2r against d. This is sign-symmetric because it operates on the true
  // rational value, so reverse playback is unbiased.
  const i128 twice = 2 * r; // r < d, so 2r < 2^127: no overflow
  if (twice > d || (twice == d && (q & 1) != 0)) {
    q += 1;
  }

  constexpr i128 kMax = std::numeric_limits<std::int64_t>::max();
  constexpr i128 kMinV = std::numeric_limits<std::int64_t>::min();
  if (q > kMax || q < kMinV) {
    return unexpected(overflow());
  }
  return Time{static_cast<std::int64_t>(q)};
}

expected<Time, TimeError> TimeMap::evaluate(Time parent_time) const {
  const auto composed = ComposedTimeMap::from(*this);
  if (!composed) {
    return unexpected(composed.error());
  }
  return composed->evaluate(parent_time);
}

} // namespace arbc
