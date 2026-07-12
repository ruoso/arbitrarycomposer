// Bench-smoke test (design doc 16 tier 4/9), the runtime twin of
// `src/pool/t/bench_smoke.t.cpp`.
//
// Drives every interactive benchmark workload body
// (src/runtime/bench/interactive_bench_workloads.hpp) ONCE, at a minimal size, under the
// normal dev/asan test build -- NOT through the Google Benchmark runner. This gives the
// benchmark bodies diff coverage and rot protection, and asserts the BEHAVIORAL facts the
// benchmarks are shaped around, without any wall-clock assertion entering the merge path
// (doc 16:54-62, 225-226: counters gate, benchmarks trend).
//
// The facts it pins are the ones that would make the benchmark a LIE if they broke:
// every scene really renders (a benchmark over a scene that composites nothing is fast
// and meaningless), the operator scenes really drive their operators' own `render` (an
// identity endpoint would measure the driver's delivery path instead), and the loop
// really reaches quiescence (a benchmark that silently exhausts its frame bound would
// report the bound, not the work).

#include <arbc/runtime/interactive.hpp>
#include <arbc/runtime/worker_pool.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "../bench/interactive_bench_workloads.hpp"

#include <chrono>
#include <cstddef>
#include <span>
#include <utility>

using namespace arbc::runtime_bench;

namespace {

// Minimal: one 256px tile per full-canvas layer, the cheapest leaf, a scene of two.
constexpr int k_dim = 256;
constexpr int k_size = 2;
constexpr int k_work = 1;
constexpr std::size_t k_cache_bytes = 64U * 1024 * 1024;
// A park bound, never an assertion: a frame with work in flight returns the instant a
// completion settles, and one with none returns when the bound elapses. Small, because a
// frame carrying arrival damage but dispatching nothing parks for the WHOLE bound.
constexpr auto k_budget = std::chrono::milliseconds(20);

bool all_transparent(const arbc::Surface& surface) {
  const std::span<const float> px =
      std::as_const(surface).span<arbc::PixelFormat::Rgba32fLinearPremul>();
  for (const float v : px) {
    if (v != 0.0F) {
      return false;
    }
  }
  return true;
}

arbc::WorkerPoolConfig inline_pool() { return arbc::WorkerPoolConfig{}; }

} // namespace

TEST_CASE("bench smoke: every interactive bench scene renders, and reaches quiescence") {
  const SceneKind kind =
      GENERATE(SceneKind::LeafHeavy, SceneKind::OperatorHeavy, SceneKind::NestedDeep);
  INFO("scene = " << scene_name(kind));

  BenchScene scene(kind, k_dim, k_size, k_work);
  REQUIRE(scene.leaf_count() > 0);
  REQUIRE_FALSE(scene.whole_scene_damage().empty());

  BenchHarness harness(scene, inline_pool(), k_cache_bytes);
  REQUIRE(harness.usable());

  const SceneCounters pass = harness.run_pass(k_budget);

  // The scene really did work: a benchmark over a scene that renders nothing measures
  // nothing. `frames_rendered` is the new denominator counter, and the pass got past the
  // still-scene early-out at least once.
  CHECK(pass.frames_rendered >= 1);
  CHECK(pass.requests_issued >= 1);
  CHECK(pass.composites >= 1);
  CHECK_FALSE(all_transparent(harness.target()));

  // Inline (`WorkerPoolConfig{}`): `submit` IS the render, so every miss settled on the
  // frame thread within the frame -- nothing was left in flight to cancel.
  CHECK(pass.tiles_cancelled == 0);
  CHECK(harness.usable());

  // The operator scenes really drive their operators' own `render` -- neither the fade's
  // envelope nor the crossfade's w is an endpoint at `k_scene_time`, so `identity()`
  // declines. Without this the "operator-heavy" and "nested-deep" benchmarks would be
  // measuring the driver's identity-delivery path and calling it an operator render.
  if (kind != SceneKind::LeafHeavy) {
    CHECK(pass.operator_renders >= 1);
  } else {
    CHECK(pass.operator_renders == 0); // a flat leaf scene has no operator layer at all
  }
}

// The second pass over the same warm harness re-damages the whole scene, so it re-renders
// rather than taking the still-scene early-out. This is what makes a multi-iteration
// benchmark measure the same work every iteration instead of measuring an empty frame
// from iteration 2 onward -- the single most plausible way an interactive benchmark
// silently becomes a no-op.
TEST_CASE("bench smoke: a second pass re-damages the scene and does the work again") {
  BenchScene scene(SceneKind::OperatorHeavy, k_dim, k_size, k_work);
  BenchHarness harness(scene, inline_pool(), k_cache_bytes);
  REQUIRE(harness.usable());

  const SceneCounters first = harness.run_pass(k_budget);
  REQUIRE(first.requests_issued >= 1);

  const SceneCounters second = harness.run_pass(k_budget);
  CHECK(second.frames_rendered >= 1);
  CHECK(second.requests_issued == first.requests_issued);   // the same misses, re-driven
  CHECK(second.operator_renders == first.operator_renders); // and the same operator work
}

// The workloads run at the SHIPPED DEFAULT too -- the configuration the benchmark exists
// to sweep. Driven to quiescence, the fan-out reaches the same place: the loop settles,
// the pool never runs two renders of one content at once, and the scene composites.
//
// It does NOT issue the same number of renders, and that is a real property of the
// shipped compositor, not an artifact of this harness: `PullServiceImpl::pull` is
// cache-first with no check against the refinement queue (`pull_service.cpp:219-243`), so
// a tile whose render is still IN FLIGHT is a cache miss and is dispatched again. An
// operator whose input answered asynchronously degrades this frame and re-drives on the
// arrival (`pull_service.cpp:205-209`, doc 13's "async composes"), and each re-drive
// re-pulls -- so an operator scene at `worker_count > 0` pays one operator render per
// refinement wave, and a nested chain pays one per level per wave. The pixels are
// unaffected (renders are deterministic and each targets its own surface, which is why
// `02-architecture#worker-dispatch-is-leaf-only`'s byte-identity clause still holds); the
// cost is redundant work on a cold cache. Dedup-ing planned misses against the
// `RefinementQueue` is `compositor.in_flight_tile_dedup`.
//
// So the invariant asserted here is the one that holds unconditionally -- a scene with NO
// operators has no re-drive path at all, and its renders ARE invariant across worker
// counts. That is the anti-waste guard, stated where it is true.
//
// enforces: 02-architecture#worker-dispatch-is-leaf-only
TEST_CASE("bench smoke: the workloads settle on a real worker pool, without duplicate "
          "in-flight renders") {
  const SceneKind kind =
      GENERATE(SceneKind::LeafHeavy, SceneKind::OperatorHeavy, SceneKind::NestedDeep);
  INFO("scene = " << scene_name(kind));

  BenchScene inline_scene(kind, k_dim, k_size, k_work);
  BenchHarness inline_harness(inline_scene, inline_pool(), k_cache_bytes);
  REQUIRE(inline_harness.usable());
  const SceneCounters oracle = inline_harness.run_pass(k_budget);
  REQUIRE(oracle.requests_issued >= 1);

  BenchScene parallel_scene(kind, k_dim, k_size, k_work);
  BenchHarness parallel_harness(parallel_scene, arbc::default_interactive_pool_config(),
                                k_cache_bytes);
  REQUIRE(parallel_harness.usable());
  const SceneCounters parallel = parallel_harness.run_pass(k_budget);

  // The pool never rendered one content twice at once, at any worker count
  // (`worker_pool.hpp:126-128`'s high-water mark, measured at the render call site).
  CHECK(parallel.max_in_flight <= 1);
  // The loop settled, and it settled having composited something.
  CHECK(parallel.frames_rendered >= 1);
  CHECK_FALSE(all_transparent(parallel_harness.target()));

  if (kind == SceneKind::LeafHeavy) {
    // No operator layer => no degrade-and-re-drive path => exactly the inline renders.
    CHECK(parallel.requests_issued == oracle.requests_issued);
    CHECK(parallel.operator_renders == 0);
  } else {
    // The operator re-drive, pinned rather than hidden: the fan-out costs AT LEAST the
    // inline renders and never fewer (a run issuing fewer would be dropping work, not
    // saving it). `compositor.in_flight_tile_dedup` is what would collapse this back to
    // equality; until it lands, this asserts the direction and the bound.
    CHECK(parallel.requests_issued >= oracle.requests_issued);
    CHECK(parallel.operator_renders >= oracle.operator_renders);
  }
}
