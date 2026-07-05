#pragma once

// Seeded schedule-perturbation helper (design doc 16:66-73, tier-6 concurrency
// tests). The single shared primitive the cross-component stress/litmus tests
// consume, generalizing the inline seed+yield pattern in
// `src/contract/t/async_render.t.cpp:220-263` into one header-only utility.
//
// The whole point of a stress test is to explore *interleavings*, not to run
// for a duration. A `Perturber` wraps a per-thread `std::mt19937` seeded from an
// explicit loop counter and fires `std::this_thread::yield()` on a random bit,
// widening the race window under a REPRODUCIBLE schedule: a red CI run replays
// byte-for-byte once the seed (logged via Catch2 `INFO(seed)` at the call site)
// is known. No `std::random_device`, no time-based seeding, and the yield is
// never timed -- it only perturbs ordering (doc 16:54-62, no wall-clock
// assertions).

#include <cstdint>
#include <random>
#include <thread>

namespace arbc::test {

class Perturber {
public:
  explicit Perturber(std::uint32_t seed) noexcept : d_rng(seed) {}

  // Yield the CPU on a random bit so different seeds walk different
  // interleavings. Never timed: it widens the window, it does not pace the
  // test.
  void maybe_yield() noexcept {
    if ((d_rng() & 1U) != 0U) {
      std::this_thread::yield();
    }
  }

  // A raw draw, for call sites that gate a perturbation choice on more than one
  // bit of entropy (e.g. picking which slot to touch next under the seed).
  std::uint32_t next() noexcept { return d_rng(); }

private:
  std::mt19937 d_rng;
};

// The canonical per-thread seed derivation (async_render.t.cpp:239 mixes the
// loop seed with the golden ratio for its second thread). `salt` distinguishes
// each thread in a multi-producer run so every thread walks a different but
// fully reproducible sub-sequence off the one logged loop seed.
constexpr std::uint32_t derive_seed(std::uint32_t seed, std::uint32_t salt) noexcept {
  return seed ^ (0x9e3779b9U * (salt + 1U));
}

} // namespace arbc::test
