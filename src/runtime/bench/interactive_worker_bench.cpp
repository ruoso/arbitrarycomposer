// Interactive worker-count benchmarks (`runtime.interactive_worker_count_default`;
// doc 16 tier 9).
//
// The measurement the shipped `default_interactive_worker_count()` was chosen from:
// three representative scenes (leaf-heavy, operator-heavy, nested-deep) swept over the
// worker counts `0 / 1 / 2 / 4 / hardware_concurrency() - 1`, each driven to quiescence
// with a realistic per-frame budget over the real `steady_clock`.
//
// Nothing here asserts ANYTHING -- not a duration, not a ratio, not a counter. The
// SIGN of the decision ("the interactive default is non-zero") comes from doc 02:61-65:
// at `worker_count == 0` the pool IS the render, so the frame thread is inside a slow
// leaf's `render` when the deadline passes and the degrade promise cannot be kept. Only
// the MAGNITUDE -- `k_max_interactive_workers` -- comes from this sweep, and it comes
// from the COUNTERS below (which are machine-independent), not from the timings (which
// are not). Benchmarks trend; counters gate (doc 16:225-226).
//
// This TU is compiled ONLY under `-DARBC_BENCHMARKS=ON` (the `bench` preset). The
// workload bodies live in interactive_bench_workloads.hpp and are also driven -- and
// behaviorally asserted -- by src/runtime/t/bench_smoke.t.cpp under the normal test
// build, which is what gives that logic diff coverage.
//
// COVERAGE EXCLUSION (doc 16:114-116, justified, the same one allocator_bench.cpp
// carries): this file holds only the Google Benchmark registration/harness wrappers and
// is not compiled in the coverage build, so it never appears in the coverage report; its
// measurable logic is the shared header, covered by the bench-smoke CTest.
//
// Run with
//   --benchmark_format=json --benchmark_out=<file>
// so quality.benchmark_history can upload the results unreshaped.

#include "interactive_bench_workloads.hpp"
#include <benchmark/benchmark.h>

#include <chrono>
#include <cstddef>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace arbc::runtime_bench;

// A 1024x1024 viewport: a 4x4 grid of 256px rung-0 tiles per full-canvas layer, so a
// scene really does plan enough tiles for a fan-out to have something to fan out.
constexpr int k_dim = 1024;
// The scene size knob (leaf strips / operator layers / nesting depth) and the leaf's
// per-pixel cost. Sized so one pass is milliseconds, not microseconds -- a leaf cheaper
// than the queue push + condvar wake would measure the pool's overhead, which is a real
// effect (see the crossover note in the task's `## Status`) but not the headline.
constexpr int k_scene_size = 8;
constexpr int k_leaf_work = 24;
// A realistic per-frame compute budget: one 60Hz frame. It is a PARK BOUND, not an
// assertion -- a frame with nothing in flight parks on it, which is exactly what a host
// frame does, and is included in the reported wall-clock on purpose.
constexpr auto k_budget = std::chrono::milliseconds(16);
constexpr std::size_t k_cache_bytes = 512U * 1024 * 1024;

// The swept worker counts. `hardware_concurrency() - 1` is the unclamped formula
// `default_interactive_worker_count()` clamps, included so the sweep shows what the
// clamp is protecting against on a big machine.
std::vector<std::size_t> worker_counts() {
  const unsigned hw = std::thread::hardware_concurrency();
  std::vector<std::size_t> counts{0, 1, 2, 4};
  if (hw > 5) {
    counts.push_back(static_cast<std::size_t>(hw) - 1);
  }
  return counts;
}

arbc::WorkerPoolConfig pool_of(std::size_t workers) {
  arbc::WorkerPoolConfig config;
  config.worker_count = workers;
  return config;
}

// Emit the pass's behavioral counters as Google Benchmark counters, so the JSON the
// future `quality.benchmark_history` uploads carries the numbers the DECISION rests on
// beside the timing that merely trends. `kAvgIterations` reports the per-pass mean.
void report(benchmark::State& state, const SceneCounters& c) {
  const auto avg = benchmark::Counter::kAvgIterations;
  state.counters["frames_rendered"] =
      benchmark::Counter(static_cast<double>(c.frames_rendered), avg);
  state.counters["requests_issued"] =
      benchmark::Counter(static_cast<double>(c.requests_issued), avg);
  state.counters["operator_renders"] =
      benchmark::Counter(static_cast<double>(c.operator_renders), avg);
  state.counters["composites"] = benchmark::Counter(static_cast<double>(c.composites), avg);
  state.counters["degraded_composites"] =
      benchmark::Counter(static_cast<double>(c.degraded_composites), avg);
  state.counters["follow_up_frames"] =
      benchmark::Counter(static_cast<double>(c.follow_up_frames), avg);
  state.counters["deadline_expiries"] =
      benchmark::Counter(static_cast<double>(c.deadline_expiries), avg);
  state.counters["tiles_cancelled"] =
      benchmark::Counter(static_cast<double>(c.tiles_cancelled), avg);
  // A high-water MARK, not a per-pass count: reported as-is, and `<= 1` is the
  // no-duplicate-render invariant `tests/interactive_worker_default.t.cpp` gates on.
  state.counters["max_in_flight"] = benchmark::Counter(static_cast<double>(c.max_in_flight));
}

// One scene at one worker count: build the scene and the harness ONCE (so the per-
// iteration cost is the frame loop's, not `std::thread`'s), then time one full
// damage-everything -> drive-to-quiescence pass per iteration.
void run(benchmark::State& state, SceneKind kind, std::size_t workers) {
  BenchScene scene(kind, k_dim, k_scene_size, k_leaf_work);
  BenchHarness harness(scene, pool_of(workers), k_cache_bytes);
  if (!harness.usable()) {
    state.SkipWithError("bench backend could not allocate the device target");
    return;
  }
  SceneCounters last{};
  for (auto _ : state) {
    last = harness.run_pass(k_budget);
    benchmark::DoNotOptimize(last.requests_issued);
  }
  report(state, last);
}

// Google Benchmark's registration runs at static-init, so the sweep is registered from a
// static initializer rather than through the BENCHMARK macro (which cannot take a
// runtime-computed worker list).
const int k_registered = [] {
  for (const SceneKind kind :
       {SceneKind::LeafHeavy, SceneKind::OperatorHeavy, SceneKind::NestedDeep}) {
    for (const std::size_t workers : worker_counts()) {
      const std::string name = std::string("BM_InteractiveFrame/") + scene_name(kind) +
                               "/workers:" + std::to_string(workers);
      benchmark::RegisterBenchmark(name.c_str(), [kind, workers](benchmark::State& state) {
        run(state, kind, workers);
      })->Unit(benchmark::kMillisecond);
    }
  }
  return 0;
}();

} // namespace

BENCHMARK_MAIN();
