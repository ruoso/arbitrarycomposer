#include <arbc/base/rational_time.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <random>
#include <vector>

namespace {

using arbc::ComposedTimeMap;
using arbc::present_in_span;
using arbc::Rational;
using arbc::Time;
using arbc::TimeError;
using arbc::TimeMap;
using arbc::TimeRange;

constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();

// ---- Tier 2: Rational canonicalization -----------------------------------

TEST_CASE("Rational canonicalizes to lowest terms, positive denominator, zero as 0/1") {
  const Rational z;
  REQUIRE(z.num() == 0);
  REQUIRE(z.den() == 1);
  REQUIRE(z.is_zero());

  // Zero is 0/1 regardless of the incoming denominator sign.
  REQUIRE(Rational(0, 5) == Rational(0, 1));
  REQUIRE(Rational(0, -3) == Rational(0, 1));

  // gcd reduction.
  REQUIRE(Rational(2, 4) == Rational(1, 2));
  REQUIRE(Rational(6, 3) == Rational(2, 1));
  REQUIRE(Rational(2, 4).num() == 1);
  REQUIRE(Rational(2, 4).den() == 2);

  // A coprime realistic rate is stored verbatim.
  REQUIRE(Rational(24000, 1001).num() == 24000);
  REQUIRE(Rational(24000, 1001).den() == 1001);

  // Sign is normalized into the numerator, denominator stays positive.
  REQUIRE(Rational(1, -2).num() == -1);
  REQUIRE(Rational(1, -2).den() == 2);
  REQUIRE(Rational(-1, -2) == Rational(1, 2));
  REQUIRE(Rational(-3, 6) == Rational(-1, 2));

  // Equality is value-exact on the canonical form.
  REQUIRE(Rational(2, 4) == Rational(1, 2));
  REQUIRE_FALSE(Rational(1, 2) == Rational(1, 3));

  // Additive inverse stays canonical.
  REQUIRE(Rational(1, 2).negated() == Rational(-1, 2));
  REQUIRE(Rational(-1, 2).negated() == Rational(1, 2));
}

// ---- Tier 2: exact rate composition --------------------------------------

// enforces: 11-time-and-video#rational-rate-composition-is-exact
TEST_CASE("exact rational rate multiply and add stay reduced") {
  const auto half_times_ntsc = Rational(1, 2).mul(Rational(24000, 1001));
  REQUIRE(half_times_ntsc.has_value());
  REQUIRE(*half_times_ntsc == Rational(12000, 1001));

  const auto reverse = Rational(-1, 2).mul(Rational(1, 2));
  REQUIRE(reverse.has_value());
  REQUIRE(*reverse == Rational(-1, 4));

  const auto whole = Rational(2, 1).mul(Rational(3, 1));
  REQUIRE(whole.has_value());
  REQUIRE(*whole == Rational(6, 1));

  // Reciprocal composes back to identity, exactly.
  const auto identity = Rational(24000, 1001).mul(Rational(1001, 24000));
  REQUIRE(identity.has_value());
  REQUIRE(*identity == Rational(1, 1));

  const auto sum = Rational(1, 2).add(Rational(1, 3));
  REQUIRE(sum.has_value());
  REQUIRE(*sum == Rational(5, 6));

  const auto cancel = Rational(1, 2).add(Rational(-1, 2));
  REQUIRE(cancel.has_value());
  REQUIRE(*cancel == Rational(0, 1));
}

// enforces: 11-time-and-video#rational-rate-composition-is-exact
TEST_CASE("rational composition overflow surfaces as a TimeError value, never wraps") {
  const auto mul_ov = Rational(kMax, 1).mul(Rational(kMax, 1));
  REQUIRE_FALSE(mul_ov.has_value());
  REQUIRE(mul_ov.error().kind == TimeError::Kind::Overflow);

  // Coprime denominators whose product exceeds the width also fault.
  const auto den_ov = Rational(1, kMax).mul(Rational(1, kMax - 1));
  REQUIRE_FALSE(den_ov.has_value());

  const auto add_ov = Rational(kMax, 1).add(Rational(kMax, 1));
  REQUIRE_FALSE(add_ov.has_value());
  REQUIRE(add_ov.error().kind == TimeError::Kind::Overflow);
}

// ---- Tier 2: affine evaluation with a single leaf rounding ---------------

// enforces: 11-time-and-video#time-map-composes-in-exact-rational-with-one-rounding
TEST_CASE("time map evaluates the affine map exactly on representable inputs") {
  // Identity: local = parent.
  REQUIRE(TimeMap{Time{0}, Rational(1, 1), Time{0}}.evaluate(Time{42})->flicks == 42);

  // Pure rate.
  REQUIRE(TimeMap{Time{0}, Rational(2, 1), Time{0}}.evaluate(Time{10})->flicks == 20);

  // (parent - in) * rate + offset = (7 - 5) * 2 + 3 = 7.
  REQUIRE(TimeMap{Time{5}, Rational(2, 1), Time{3}}.evaluate(Time{7})->flicks == 7);

  // Negative rate: reverse playback. (30 - 0) * -1 + 100 = 70.
  REQUIRE(TimeMap{Time{0}, Rational(-1, 1), Time{100}}.evaluate(Time{30})->flicks == 70);

  // A zero rate is a frozen frame: every instant maps to the offset.
  REQUIRE(TimeMap{Time{5}, Rational(0, 1), Time{99}}.evaluate(Time{1234})->flicks == 99);
}

// enforces: 11-time-and-video#time-map-composes-in-exact-rational-with-one-rounding
TEST_CASE("leaf rounding is nearest flick, ties to even, sign-symmetric") {
  const TimeMap half{Time{0}, Rational(1, 2), Time{0}}; // local = parent/2

  // Half-flick ties round to the even neighbor, positive side.
  REQUIRE(half.evaluate(Time{1})->flicks == 0);  // 0.5 -> 0
  REQUIRE(half.evaluate(Time{3})->flicks == 2);  // 1.5 -> 2
  REQUIRE(half.evaluate(Time{5})->flicks == 2);  // 2.5 -> 2
  REQUIRE(half.evaluate(Time{7})->flicks == 4);  // 3.5 -> 4

  // Negative mirrors: symmetric, unbiased under reverse playback.
  REQUIRE(half.evaluate(Time{-1})->flicks == 0);  // -0.5 -> 0
  REQUIRE(half.evaluate(Time{-3})->flicks == -2); // -1.5 -> -2
  REQUIRE(half.evaluate(Time{-5})->flicks == -2); // -2.5 -> -2
  REQUIRE(half.evaluate(Time{-7})->flicks == -4); // -3.5 -> -4

  // Non-tie rounding to the nearer flick.
  const TimeMap third{Time{0}, Rational(1, 3), Time{0}};
  REQUIRE(third.evaluate(Time{1})->flicks == 0);   // 0.333 -> 0
  REQUIRE(third.evaluate(Time{2})->flicks == 1);   // 0.667 -> 1
  REQUIRE(third.evaluate(Time{-1})->flicks == 0);  // -0.333 -> 0
  REQUIRE(third.evaluate(Time{-2})->flicks == -1); // -0.667 -> -1
}

TEST_CASE("evaluate faults rather than wrapping when the instant overflows the width") {
  // Composed rate INT64_MAX at parent INT64_MAX overflows the int64 flick range.
  const ComposedTimeMap big{Rational(kMax, 1), Rational(0, 1)};
  const auto out = big.evaluate(Time{kMax});
  REQUIRE_FALSE(out.has_value());
  REQUIRE(out.error().kind == TimeError::Kind::Overflow);

  // A large denominator forces the intermediate product past the 128-bit width.
  const ComposedTimeMap wide{Rational(kMax, 1), Rational(1, kMax)};
  REQUIRE_FALSE(wide.evaluate(Time{kMax}).has_value());
}

// ---- Tier 2: half-open span culling --------------------------------------

// enforces: 11-time-and-video#span-cull-is-half-open
TEST_CASE("span culling is half-open: in included, out excluded") {
  const TimeRange span{Time{10}, Time{20}};
  REQUIRE(present_in_span(span, Time{10}));      // in included
  REQUIRE(present_in_span(span, Time{15}));      // interior
  REQUIRE(present_in_span(span, Time{19}));      // just below out
  REQUIRE_FALSE(present_in_span(span, Time{20})); // out excluded
  REQUIRE_FALSE(present_in_span(span, Time{9}));  // before in

  // The default all() span is always present -- a still is the degenerate case.
  REQUIRE(present_in_span(TimeRange::all(), Time{0}));
  REQUIRE(present_in_span(TimeRange::all(), Time{-1'000'000'000}));
  REQUIRE(present_in_span(TimeRange::all(), Time{1'000'000'000}));

  // A degenerate span (out <= in) is present at no instant.
  const TimeRange empty{Time{5}, Time{5}};
  REQUIRE_FALSE(present_in_span(empty, Time{5}));
  REQUIRE_FALSE(present_in_span(empty, Time{4}));
  const TimeRange reversed{Time{20}, Time{10}};
  REQUIRE_FALSE(present_in_span(reversed, Time{15}));
}

// ---- Tier 2: chain composition down an edge stack ------------------------

// enforces: 11-time-and-video#time-map-composes-in-exact-rational-with-one-rounding
TEST_CASE("composing an edge stack multiplies rates and rounds once at the leaf") {
  // rate 1/2 inside rate 1/2 plays at 1/4 (doc 11 recursion example).
  const std::array<TimeMap, 2> edges{TimeMap{Time{0}, Rational(1, 2), Time{0}},
                                     TimeMap{Time{0}, Rational(1, 2), Time{0}}};
  const auto composed = ComposedTimeMap::compose(edges.data(), edges.size());
  REQUIRE(composed.has_value());
  REQUIRE(composed->a == Rational(1, 4));
  REQUIRE(composed->b == Rational(0, 1));
  REQUIRE(composed->evaluate(Time{40})->flicks == 10);

  // An empty stack is the identity.
  const auto id = ComposedTimeMap::compose(nullptr, 0);
  REQUIRE(id.has_value());
  REQUIRE(*id == ComposedTimeMap::identity());
  REQUIRE(id->evaluate(Time{7})->flicks == 7);

  // A deep same-rate stack of a coprime rate eventually overflows the width and
  // faults rather than silently corrupting the composed rate.
  std::vector<TimeMap> deep(8, TimeMap{Time{0}, Rational(24000, 1001), Time{0}});
  REQUIRE_FALSE(ComposedTimeMap::compose(deep.data(), deep.size()).has_value());
}

TEST_CASE("composition faults propagate as values through every fold step") {
  // from(): the in*rate product overflows.
  REQUIRE_FALSE(ComposedTimeMap::from(TimeMap{Time{kMax}, Rational(kMax, 1), Time{0}}).has_value());
  // from(): in*rate is fine but offset - in*rate overflows.
  REQUIRE_FALSE(ComposedTimeMap::from(TimeMap{Time{-1}, Rational(kMax, 1), Time{kMax}}).has_value());
  // then() surfaces a faulting edge from identity.
  REQUIRE_FALSE(
      ComposedTimeMap::identity().then(TimeMap{Time{kMax}, Rational(kMax, 1), Time{0}}).has_value());

  // and_then(): each of the three rational ops can fault.
  const ComposedTimeMap max_rate{Rational(kMax, 1), Rational(0, 1)};
  const ComposedTimeMap max_off{Rational(1, 1), Rational(kMax, 1)};
  REQUIRE_FALSE(max_rate.and_then(max_rate).has_value());          // rate * rate
  REQUIRE_FALSE(max_off.and_then(max_rate).has_value());           // rate * offset
  REQUIRE_FALSE(max_off.and_then(max_off).has_value());            // offset + offset
}

// ---- Tier 5: numeric invariant / property test ---------------------------

using i128 = arbc::rational_i128;
using u128 = arbc::rational_u128;

struct RefRat {
  i128 n;
  i128 d;
};

u128 gcd_u(u128 a, u128 b) {
  while (b != 0) {
    const u128 t = a % b;
    a = b;
    b = t;
  }
  return a;
}

// Reduce to canonical form, mirroring the implementation's fixed-width discipline:
// nullopt marks the overflow the implementation reports as a TimeError.
std::optional<RefRat> ref_reduce(i128 n, i128 d) {
  const bool neg = (n < 0) != (d < 0);
  u128 an = n < 0 ? u128(0) - static_cast<u128>(n) : static_cast<u128>(n);
  u128 ad = d < 0 ? u128(0) - static_cast<u128>(d) : static_cast<u128>(d);
  const u128 g = gcd_u(an, ad);
  an /= g;
  ad /= g;
  const u128 lim = static_cast<u128>(kMax);
  if (an > lim || ad > lim) {
    return std::nullopt;
  }
  return RefRat{neg ? -static_cast<i128>(an) : static_cast<i128>(an), static_cast<i128>(ad)};
}

std::optional<RefRat> ref_mul(RefRat a, RefRat b) { return ref_reduce(a.n * b.n, a.d * b.d); }
std::optional<RefRat> ref_add(RefRat a, RefRat b) {
  return ref_reduce(a.n * b.d + b.n * a.d, a.d * b.d);
}

// enforces: 11-time-and-video#time-map-composes-in-exact-rational-with-one-rounding
// enforces: 11-time-and-video#rational-rate-composition-is-exact
TEST_CASE("pathological rate stacks compose exactly, depth-invariantly, and fault on overflow") {
  const std::array<Rational, 6> pool{Rational(24000, 1001), Rational(30000, 1001), Rational(1, 2),
                                     Rational(2, 1),         Rational(-1, 2),       Rational(1, 1)};
  std::mt19937 rng(0xC0FFEEu);
  std::uniform_int_distribution<int> depth_dist(1, 7);
  std::uniform_int_distribution<std::size_t> rate_dist(0, pool.size() - 1);
  std::uniform_int_distribution<std::int64_t> small(-1000, 1000);
  std::uniform_int_distribution<std::int64_t> parent_dist(-10000, 10000);

  int compared_values = 0;
  int checked_associativity = 0;
  int observed_overflow = 0;

  for (int iter = 0; iter < 4000; ++iter) {
    const int depth = depth_dist(rng);
    std::vector<TimeMap> edges;
    edges.reserve(static_cast<std::size_t>(depth));
    for (int e = 0; e < depth; ++e) {
      edges.push_back(TimeMap{Time{small(rng)}, pool[rate_dist(rng)], Time{small(rng)}});
    }

    // Independent higher-width reference fold, mirroring compose()'s per-edge
    // reduce-and-check discipline.
    std::optional<RefRat> A = RefRat{1, 1};
    std::optional<RefRat> B = RefRat{0, 1};
    bool ref_overflow = false;
    for (const TimeMap& m : edges) {
      const RefRat am{m.rate.num(), m.rate.den()};
      const auto bm = ref_reduce(static_cast<i128>(m.offset.flicks) * m.rate.den() -
                                     static_cast<i128>(m.in.flicks) * m.rate.num(),
                                 m.rate.den());
      if (!bm) {
        ref_overflow = true;
        break;
      }
      const auto na = ref_mul(am, *A);
      const auto sb = ref_mul(am, *B);
      if (!na || !sb) {
        ref_overflow = true;
        break;
      }
      const auto nb = ref_add(*sb, *bm);
      if (!nb) {
        ref_overflow = true;
        break;
      }
      A = na;
      B = nb;
    }

    const auto composed = ComposedTimeMap::compose(edges.data(), edges.size());

    if (ref_overflow) {
      // The width artifact must surface as a value, never wrap or abort.
      CHECK_FALSE(composed.has_value());
      ++observed_overflow;
      continue;
    }

    REQUIRE(composed.has_value());
    // Exact composed rate and offset -- no floating error, in lowest terms.
    CHECK(static_cast<i128>(composed->a.num()) == A->n);
    CHECK(static_cast<i128>(composed->a.den()) == A->d);
    CHECK(static_cast<i128>(composed->b.num()) == B->n);
    CHECK(static_cast<i128>(composed->b.den()) == B->d);
    ++compared_values;

    // Grouping / associativity independence: splitting the chain and combining
    // the halves yields the identical composed affine (depth invariance).
    const int cut = depth / 2;
    const auto left = ComposedTimeMap::compose(edges.data(), static_cast<std::size_t>(cut));
    const auto right = ComposedTimeMap::compose(edges.data() + cut,
                                                static_cast<std::size_t>(depth - cut));
    if (left.has_value() && right.has_value()) {
      const auto recombined = left->and_then(*right);
      if (recombined.has_value()) {
        CHECK(*recombined == *composed);
        ++checked_associativity;
      }
    }

    // Single leaf rounding matches the independent reference rounding.
    const std::int64_t p = parent_dist(rng);
    i128 term = A->n * static_cast<i128>(p);
    i128 nn = 0;
    const bool eval_overflow = __builtin_mul_overflow(term, B->d, &term) ||
                               __builtin_add_overflow(term, B->n * A->d, &nn);
    if (!eval_overflow) {
      const i128 dd = A->d * B->d;
      i128 q = nn / dd;
      i128 r = nn % dd;
      if (r < 0) {
        q -= 1;
        r += dd;
      }
      const i128 twice = 2 * r;
      if (twice > dd || (twice == dd && (q & 1) != 0)) {
        q += 1;
      }
      const auto evaluated = composed->evaluate(Time{p});
      if (q > static_cast<i128>(kMax) || q < static_cast<i128>(std::numeric_limits<std::int64_t>::min())) {
        CHECK_FALSE(evaluated.has_value());
      } else {
        REQUIRE(evaluated.has_value());
        CHECK(evaluated->flicks == static_cast<std::int64_t>(q));
      }
    }
  }

  // The seeded corpus actually exercises each behavior it claims to.
  CHECK(compared_values > 0);
  CHECK(checked_associativity > 0);
  CHECK(observed_overflow > 0);
}

} // namespace
