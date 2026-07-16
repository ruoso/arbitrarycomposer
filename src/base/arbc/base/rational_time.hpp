#pragma once

#include <arbc/arbc_api.h>
#include <arbc/base/expected.hpp>
#include <arbc/base/time.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>

// The MSVC fallback for 128-bit integers must be included before opening
// namespace arbc, because int128_msvc.hpp itself declares namespace arbc.
// Including it inside namespace arbc would create a nested arbc::arbc scope.
#ifndef __SIZEOF_INT128__
#include <arbc/base/detail/int128_msvc.hpp>
#endif

namespace arbc {

// The 128-bit intermediate the rate math reduces through (doc 11 Decision 2).
// `__int128` is a GCC/Clang extension; MSVC uses a portable struct fallback
// (included above, outside this namespace).
#ifdef __SIZEOF_INT128__
// GCC and Clang: use the native type directly.
__extension__ typedef __int128 rational_i128;
__extension__ typedef unsigned __int128 rational_u128;
#endif

// Error value for rational-time arithmetic. A compose or evaluate that would
// exceed the fixed integer width even after reduction surfaces as this value
// rather than wrapping silently or aborting (faults-as-values, doc 10;
// doc 11:52-56). Pathological rate stacks fail honestly; realistic ones
// reduce and never reach it.
struct TimeError {
  enum class Kind { Overflow };
  Kind kind{Kind::Overflow};

  friend constexpr bool operator==(const TimeError&, const TimeError&) = default;
};

// An exact rational rate, kept canonical: reduced to lowest terms, `den > 0`,
// sign carried in the numerator, and `0` stored as `0/1`. Rates in time maps
// are exact rationals (e.g. 24000/1001) that compose without precision loss at
// any nesting depth (doc 11:41-56). Trivially copyable and STL-free like the
// `Time`/`TimeRange` value types it sits beside; equality is value-exact on the
// canonical form.
class ARBC_API Rational {
public:
  constexpr Rational() = default;

  // Canonicalizing constructor. Precondition: `den != 0` (an infinite rate is
  // not a representable value) and neither argument is `INT64_MIN` (its
  // magnitude is not representable as a positive `int64`). Both are checked in
  // debug builds; the data-dependent overflow of a *composition* is a
  // faults-as-values path (`mul`/`add`), distinct from these construction
  // preconditions.
  constexpr Rational(std::int64_t num, std::int64_t den) {
    assert(den != 0 && "Rational denominator must be non-zero");
    assert(num != kMin && den != kMin && "Rational argument INT64_MIN not representable");
    std::uint64_t an = magnitude(num);
    std::uint64_t ad = magnitude(den);
    const std::uint64_t g = gcd_u64(an, ad);
    an /= g;
    ad /= g;
    const bool negative = (num < 0) != (den < 0);
    d_num = negative ? -static_cast<std::int64_t>(an) : static_cast<std::int64_t>(an);
    d_den = static_cast<std::int64_t>(ad);
  }

  constexpr std::int64_t num() const { return d_num; }
  constexpr std::int64_t den() const { return d_den; }
  constexpr bool is_zero() const { return d_num == 0; }

  // Additive inverse; stays canonical (a canonical numerator is never
  // INT64_MIN, so the negation never overflows).
  constexpr Rational negated() const { return Rational(raw_tag{}, -d_num, d_den); }

  // Exact rational multiply (rate composition) and add, each reduced back to
  // canonical form via 128-bit intermediates. Returns `TimeError` if the
  // reduced result exceeds the `int64` width -- never a silent wrap (doc
  // 11:52-56).
  expected<Rational, TimeError> mul(const Rational& other) const;
  expected<Rational, TimeError> add(const Rational& other) const;

  friend constexpr bool operator==(const Rational&, const Rational&) = default;

private:
  struct raw_tag {};
  constexpr Rational(raw_tag, std::int64_t num, std::int64_t den) : d_num(num), d_den(den) {}

  static constexpr std::int64_t kMin = -0x7fff'ffff'ffff'ffffLL - 1; // INT64_MIN

  static constexpr std::uint64_t magnitude(std::int64_t v) {
    return v < 0 ? 0u - static_cast<std::uint64_t>(v) : static_cast<std::uint64_t>(v);
  }
  static constexpr std::uint64_t gcd_u64(std::uint64_t a, std::uint64_t b) {
    while (b != 0) {
      const std::uint64_t t = a % b;
      a = b;
      b = t;
    }
    return a;
  }

  // Reduce a 128-bit ratio to a canonical `Rational` or `TimeError` on
  // overflow. Precondition `den != 0`. Defined in the TU.
  static expected<Rational, TimeError> from_i128(rational_i128 num, rational_i128 den);

  std::int64_t d_num{0};
  std::int64_t d_den{1};
};

// The per-edge 1D affine time map (doc 11:66-71):
// `local_time = (parent_time - in) * rate + offset`, with `rate` a rational
// (negative allowed for reverse playback). A chain of these composes down a
// graph edge stack into a single `ComposedTimeMap`, so the whole composition is
// evaluated in rational arithmetic and rounded to an integer flick exactly once,
// at the leaf.
struct ARBC_API TimeMap {
  Time in{};
  Rational rate{1, 1};
  Time offset{};

  // Convenience single-edge evaluation, routed through the same rational
  // composition + single leaf rounding as a chain.
  expected<Time, TimeError> evaluate(Time parent_time) const;

  friend constexpr bool operator==(const TimeMap&, const TimeMap&) = default;
};

// The composed affine accumulator `local = a * parent + b`, held entirely in
// exact rational arithmetic (unrounded) across a whole edge stack. `a` is the
// composed rate; `b` the composed offset in flicks. Rounding to the integer
// flick timebase happens exactly once, in `evaluate`, at the leaf -- never per
// edge (doc 11:44-48, doc 04's never-accumulate rule). The default value is the
// identity map.
struct ARBC_API ComposedTimeMap {
  Rational a{1, 1};
  Rational b{0, 1};

  static constexpr ComposedTimeMap identity() { return {}; }

  // Lift a single edge to its affine form: `a = rate`,
  // `b = offset - in * rate`.
  static expected<ComposedTimeMap, TimeError> from(const TimeMap& edge);

  // Fold one more edge onto the leaf side of this accumulator (the edge's map
  // is applied *after* this one, as when descending one level deeper).
  expected<ComposedTimeMap, TimeError> then(const TimeMap& edge) const;

  // Compose two accumulators: `other` applied after `this`. Associative, so the
  // composed result is independent of how a chain is grouped -- precision does
  // not degrade with depth.
  expected<ComposedTimeMap, TimeError> and_then(const ComposedTimeMap& other) const;

  // The single leaf rounding: convert the composed rational instant at
  // `parent_time` to an integer-flick `Time`, rounding to the nearest flick with
  // ties to even (sign-symmetric, so reverse playback is unbiased; doc 11:48-51).
  // `TimeError` on overflow of the fixed width.
  expected<Time, TimeError> evaluate(Time parent_time) const;

  // Compose a whole edge stack, `edges[0]` root-most (applied first) through
  // `edges[count-1]` leaf-most. One rational multiply-add per edge (doc 11:144).
  static expected<ComposedTimeMap, TimeError> compose(const TimeMap* edges, std::size_t count);

  friend constexpr bool operator==(const ComposedTimeMap&, const ComposedTimeMap&) = default;
};

// Half-open span culling (doc 11:62-73): a layer with span `[in, out)` in
// parent time is present at `parent_time` iff `span.contains(parent_time)`. The
// default all-present span is `TimeRange::all()` (a still is the degenerate,
// always-present case); a degenerate span (`out <= in`) is present at no
// instant. Outside its span a layer is culled before its time map is evaluated.
constexpr bool present_in_span(const TimeRange& span, Time parent_time) {
  return span.contains(parent_time);
}

} // namespace arbc
