#include <arbc/runtime/transport.hpp>

#include <cstdint>
#include <limits>

namespace arbc {

expected<Time, TimeError> Transport::advance(Time real_elapsed) {
  // A paused advance moves zero flicks (doc 11:108). The playhead is unchanged and
  // the delta is zero regardless of rate or real elapsed.
  if (d_paused) {
    return d_playhead;
  }

  // Scale the real elapsed duration by the rate through the SAME exact-rational +
  // single-leaf-rounding + overflow-as-value path the time-map math uses
  // (`TimeMap{in=0, rate, offset=0}.evaluate(real_elapsed) == round_ties_even(
  // real_elapsed * rate)`), so reverse playback is sign-symmetric and a
  // pathological rate faults as a `TimeError` rather than wrapping (doc 11:102-108).
  const expected<Time, TimeError> delta = TimeMap{Time{0}, d_rate, Time{0}}.evaluate(real_elapsed);
  if (!delta) {
    return unexpected(delta.error()); // playhead untouched: advance is all-or-nothing
  }

  // Compute the advanced instant in a 128-bit intermediate so the playhead + delta
  // sum never wraps; the loop-wrap result lands back in `int64` range and the
  // unbounded result is range-checked below.
  const rational_i128 advanced =
      static_cast<rational_i128>(d_playhead.flicks) + static_cast<rational_i128>(delta->flicks);

  // Loop wrap: true modulo into the half-open `[in, out)` window. A degenerate loop
  // (`out <= in`, i.e. `empty()`) is ignored -- no wrap, as if unbounded.
  if (d_loop && !d_loop->empty()) {
    const rational_i128 in = d_loop->start.flicks;
    const rational_i128 out = d_loop->end.flicks;
    const rational_i128 len = out - in; // > 0 (non-degenerate); fits in i128
    rational_i128 off = advanced - in;
    rational_i128 m = off % len;
    if (m < 0) {
      m += len; // true modulo: forward past `out` and reverse past `in` both land in range
    }
    const rational_i128 wrapped = in + m; // in <= wrapped < out, so it fits int64
    d_playhead = Time{static_cast<std::int64_t>(wrapped)};
    return d_playhead;
  }

  // Unbounded advance: an instant beyond the flick width faults rather than wrapping.
  constexpr rational_i128 kMax = std::numeric_limits<std::int64_t>::max();
  constexpr rational_i128 kMin = std::numeric_limits<std::int64_t>::min();
  if (advanced > kMax || advanced < kMin) {
    return unexpected(TimeError{TimeError::Kind::Overflow}); // playhead untouched
  }
  d_playhead = Time{static_cast<std::int64_t>(advanced)};
  return d_playhead;
}

} // namespace arbc
