#pragma once

#include <arbc/arbc_api.h>
#include <arbc/base/expected.hpp>      // expected
#include <arbc/base/rational_time.hpp> // Rational, TimeMap, TimeError
#include <arbc/base/time.hpp>          // Time, TimeRange

#include <optional>

// The per-viewport playback transport for `arbc::runtime` (L5, doc 17:60,84-86):
// doc 11:88-115 ("Viewport gains a transport") made concrete. It is the temporal
// sibling of the viewport camera -- per-viewport host-owned value state that owns
// the playhead (an instant on the composition time axis), a rational playback
// `rate` (negative allowed for reverse), a `pause` flag, and optional half-open
// loop bounds. Its sampled `position()` is the `composition_time` the interactive
// render driver already threads into `render_frame` (`interactive.hpp:97-101`);
// the transport is the missing producer that turns "here is a time" into "here is
// a clock the host plays, pauses, scrubs, and reverses".
//
// The `Time`/`Rational` time vocabulary is `base` (doc 17:48); a clock that PLAYS
// that vocabulary is runtime POLICY (the same vocabulary-vs-policy line that keeps
// the render drivers in runtime, not the engines). The transport therefore lives
// in runtime and depends only on `base` -- a subset of the runtime component's
// already-allowed deps, so `scripts/check_levels.py` needs no new edge.
//
// Concurrency: this is per-viewport host-owned value state mutated only by the
// driving thread, exactly like the camera (doc 02:120-137); only the immutable
// sampled `Time` crosses by value to the render thread. It holds no shared mutable
// state, owns no thread, is copyable, and reads NO wall clock (doc 16:54-62) --
// advance is driven by the elapsed REAL duration the host samples from its own
// injected clock. It therefore carries no TSan/stress obligation by design.

namespace arbc {

class ARBC_API Transport {
public:
  // A transport starts playing (not paused) at `start`, rate 1/1, no loop bounds.
  explicit Transport(Time start = Time::zero()) : d_playhead(start) {}

  // --- Sampling (pure reads) ------------------------------------------------
  // The playhead: the instant the compositor renders this viewport at. A pure
  // read that never mutates state and never reads a clock.
  Time position() const noexcept { return d_playhead; }

  Rational rate() const noexcept { return d_rate; }
  bool is_paused() const noexcept { return d_paused; }
  const std::optional<TimeRange>& loop() const noexcept { return d_loop; }

  // --- Controls -------------------------------------------------------------
  // Resume playing at the retained (pre-pause) rate; `pause()` freezes the clock
  // without discarding the rate, so `play()` after `pause()` restores it. `pause`
  // is a distinct fact from `rate == 0`: a paused transport can still be
  // seeked/scrubbed, and a `rate == 0` transport is legitimately playing-but-frozen
  // (doc 11:97-101).
  void play() noexcept { d_paused = false; }
  void pause() noexcept { d_paused = true; }

  // Set the playback rate (negative = reverse). Retained across pause. Independent
  // of the pause flag -- setting a rate does not resume a paused transport.
  void set_rate(Rational rate) noexcept { d_rate = rate; }

  // Seek/scrub: set the playhead to EXACTLY `t`, unconstrained by the loop bounds
  // or the pause state (doc 11:112-114). A filmstrip thumbnail may sit anywhere,
  // including outside the loop window -- scrub must reach any instant so two
  // viewports can observe one composition at arbitrary different times.
  void seek(Time t) noexcept { d_playhead = t; }

  // Set (or clear, with `nullopt`) the half-open `[in, out)` loop bounds. A
  // degenerate range (`out <= in`) is not a valid loop and applies no wrap; it is
  // stored as given but ignored by `advance` (see `advance`).
  void set_loop(std::optional<TimeRange> loop) noexcept { d_loop = loop; }

  // Advance the playhead by the host-supplied elapsed REAL duration scaled by the
  // rate, wrapped into the loop. The delta is `round_ties_even(real_elapsed * rate)`
  // flicks, computed in exact rational arithmetic with one ties-to-even leaf
  // rounding via `TimeMap{in=0, rate, offset=0}.evaluate` -- inheriting its
  // sign-symmetric reverse-playback rounding and its overflow-as-`TimeError`
  // (never a silent wrap). A PAUSED advance moves the playhead zero flicks and
  // returns the unchanged position. On any `TimeError` the playhead is left
  // untouched (advance is all-or-nothing). Returns the new (or unchanged) playhead.
  //
  // With loop `[in, out)` set and non-degenerate, the advanced instant re-enters
  // the window by true modulo at `in` -- a forward advance reaching/passing `out`
  // wraps to `in`, a reverse advance past `in` wraps symmetrically, and an advance
  // longer than one loop length still lands in `[in, out)`. With no loop (or a
  // degenerate one) advance is unbounded (span culling handles emptiness).
  expected<Time, TimeError> advance(Time real_elapsed);

private:
  Time d_playhead{};               // the sampled instant (pure-read via position())
  Rational d_rate{1, 1};           // playback speed, negative = reverse, kept across pause
  bool d_paused{false};            // distinct from rate == 0 (doc 11:97-101)
  std::optional<TimeRange> d_loop; // half-open loop bounds, or none (unbounded)
};

} // namespace arbc
