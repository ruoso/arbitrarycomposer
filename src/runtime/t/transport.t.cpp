#include <arbc/runtime/transport.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <random>

// Unit tests for the per-viewport playback transport (`arbc::Transport`, doc
// 11:88-115, 17:60). Deterministic and wall-clock-free: `advance` is driven by a
// host-supplied elapsed `Time`, never a real clock read, so every assertion is an
// exact-equality (tier-2) check (doc 16:54-62). No golden applies -- the transport
// produces time, not pixels (mirrors rational_time).

namespace {

using arbc::Rational;
using arbc::Time;
using arbc::TimeError;
using arbc::TimeRange;
using arbc::Transport;

// Independent numeric oracle for `round_ties_even(elapsed * num / den)`, `den > 0`
// -- written from scratch in the test so it is not the code under test. Mirrors the
// leaf rounding rational_time proved (floor-divide, then compare 2r against d, ties
// to even), sign-symmetric on the true rational value.
std::int64_t ref_scale(std::int64_t elapsed, std::int64_t num, std::int64_t den) {
  using i128 = arbc::rational_i128; // exported __int128 alias -- keeps the pedantic build clean
  i128 n = static_cast<i128>(elapsed) * num;
  const i128 d = den; // > 0
  i128 q = n / d;
  i128 r = n % d;
  if (r < 0) {
    q -= 1;
    r += d;
  }
  const i128 twice = 2 * r;
  if (twice > d || (twice == d && (q & 1) != 0)) {
    q += 1;
  }
  return static_cast<std::int64_t>(q);
}

} // namespace

// enforces: 11-time-and-video#transport-advance-scales-real-time-by-rate
TEST_CASE("transport: advance scales real elapsed by the rate, sign-symmetric, exact") {
  // Hand-picked: identity rate is a pure real-time clock.
  {
    Transport t;
    REQUIRE(t.advance(Time{1000}).has_value());
    CHECK(t.position() == Time{1000});
  }
  // Half rate exercises the ties-to-even leaf rounding on the sub-flick remainder.
  {
    Transport t;
    t.set_rate(Rational(1, 2));
    CHECK(t.advance(Time{1}).value() == Time{0}); // 0.5 -> 0 (tie to even)
    Transport u;
    u.set_rate(Rational(1, 2));
    CHECK(u.advance(Time{3}).value() == Time{2}); // 1.5 -> 2 (tie to even)
  }
  // A realistic NTSC-ish rate lands exact when the elapsed clears the denominator.
  {
    Transport t;
    t.set_rate(Rational(24000, 1001));
    CHECK(t.advance(Time{1001}).value() == Time{24000});
  }
  // Sign symmetry: rate r and rate -r step by equal magnitude, opposite sign, from
  // the same start.
  {
    Transport fwd(Time{0});
    fwd.set_rate(Rational(3, 2));
    Transport rev(Time{0});
    rev.set_rate(Rational(-3, 2));
    const Time f = fwd.advance(Time{7}).value();
    const Time r = rev.advance(Time{7}).value();
    CHECK(f.flicks == -r.flicks);
    CHECK(f == Time{ref_scale(7, 3, 2)});
  }

  // Seeded numeric sweep over rate/delta pairs against the independent oracle.
  {
    const std::array<Rational, 8> rates{Rational(1, 1),  Rational(1, 2),        Rational(2, 1),
                                        Rational(-1, 2), Rational(24000, 1001), Rational(-3, 7),
                                        Rational(5, 4),  Rational(-1, 1)};
    std::mt19937 rng(0xB19B0A7u);
    std::uniform_int_distribution<std::int64_t> elapsed_dist(-5'000'000, 5'000'000);
    for (int i = 0; i < 4096; ++i) {
      const Rational rate = rates[static_cast<std::size_t>(rng() % rates.size())];
      const std::int64_t elapsed = elapsed_dist(rng);
      Transport t(Time{0});
      t.set_rate(rate);
      const auto out = t.advance(Time{elapsed});
      REQUIRE(out.has_value());
      CHECK(out.value() == Time{ref_scale(elapsed, rate.num(), rate.den())});
      CHECK(t.position() == out.value());
    }
  }

  // A pathological rate that overflows the flick width faults as a value and leaves
  // the playhead untouched -- never a silent wrap.
  {
    constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    Transport t(Time{42});
    t.set_rate(Rational(kMax, 1));
    const auto out = t.advance(Time{kMax});
    REQUIRE_FALSE(out.has_value());
    CHECK(out.error() == TimeError{TimeError::Kind::Overflow});
    CHECK(t.position() == Time{42}); // unchanged: advance is all-or-nothing
  }
}

// enforces: 11-time-and-video#transport-loop-wraps-half-open
TEST_CASE("transport: loop bounds wrap half-open via true modulo, seek is unclamped") {
  const TimeRange loop{Time{100}, Time{200}}; // [in=100, out=200), length 100

  // Forward advance staying inside the window does not wrap.
  {
    Transport t(Time{150});
    t.set_loop(loop);
    CHECK(t.advance(Time{30}).value() == Time{180});
  }
  // Reaching `out` exactly wraps to `in` (half-open: out excluded).
  {
    Transport t(Time{150});
    t.set_loop(loop);
    CHECK(t.advance(Time{50}).value() == Time{100});
  }
  // Passing `out` re-enters via true modulo.
  {
    Transport t(Time{150});
    t.set_loop(loop);
    CHECK(t.advance(Time{60}).value() == Time{110});
  }
  // An advance longer than one loop length still lands in [in, out).
  {
    Transport t(Time{150});
    t.set_loop(loop);
    const Time p = t.advance(Time{250}).value(); // 400 -> modulo -> 100
    CHECK(p == Time{100});
    CHECK(p.flicks >= 100);
    CHECK(p.flicks < 200);
  }
  // Reverse advance past `in` wraps symmetrically at `in`.
  {
    Transport t(Time{150});
    t.set_loop(loop);
    t.set_rate(Rational(-1, 1));
    CHECK(t.advance(Time{60}).value() == Time{190}); // 90 -> wrap -> 190
  }
  // No loop: advance is unbounded (no wrap).
  {
    Transport t(Time{0});
    CHECK(t.advance(Time{1'000'000}).value() == Time{1'000'000});
  }
  // A degenerate loop (out <= in) applies no wrap -- treated as unbounded.
  {
    Transport t(Time{150});
    t.set_loop(TimeRange{Time{200}, Time{100}});
    CHECK(t.advance(Time{100}).value() == Time{250});
  }
}

// enforces: 11-time-and-video#transport-pause-and-seek-are-exact
TEST_CASE("transport: pause is a no-op advance, seek is exact and unclamped") {
  // A paused advance moves zero flicks; resume plays at the pre-pause rate.
  {
    Transport t(Time{500});
    t.set_rate(Rational(1, 2));
    CHECK(t.advance(Time{10}).value() == Time{505}); // playing: +5
    t.pause();
    CHECK(t.advance(Time{1000}).value() == Time{505}); // no-op: unchanged
    CHECK(t.position() == Time{505});
    t.play();
    CHECK(t.advance(Time{10}).value() == Time{510}); // resumes at retained 1/2 rate
  }
  // Seek reaches any instant regardless of loop bounds or pause state.
  {
    Transport t(Time{0});
    t.set_loop(TimeRange{Time{0}, Time{100}});
    t.seek(Time{9999}); // outside the loop window
    CHECK(t.position() == Time{9999});
    t.pause();
    t.seek(Time{-4321}); // paused, still scrubbable, unclamped
    CHECK(t.position() == Time{-4321});
  }
}

// enforces: 11-time-and-video#transports-observe-composition-independently
TEST_CASE("transport: two transports over one document observe it independently") {
  // Two per-viewport clocks: mutating one never touches the other, and each samples
  // its own instant -- the per-viewport premise the pull design gives for free.
  Transport preview(Time{0});
  Transport filmstrip(Time{7'000'000});

  preview.set_rate(Rational(1, 1));
  REQUIRE(preview.advance(Time{1'000'000}).has_value());
  filmstrip.seek(Time{42});

  CHECK(preview.position() == Time{1'000'000});
  CHECK(filmstrip.position() == Time{42}); // preview's advance did not move it

  // Copies are independent value state, not aliases.
  Transport a(Time{100});
  Transport b = a;
  b.seek(Time{999});
  CHECK(a.position() == Time{100});
  CHECK(b.position() == Time{999});
}
