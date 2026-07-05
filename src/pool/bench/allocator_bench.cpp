// Allocator benchmarks (design doc 15; doc 16 tier 9).
//
// The honest cpioo-vs-shared_ptr rerun and the arena churn/reclaim
// microbenchmarks, built with Google Benchmark against the SHIPPED `arbc::pool`
// implementation (RefStore / Ref / SlotRef / peek + the deferred reclamation
// surface) -- not the external cpioo prototype (doc 15:222).
//
// This TU is compiled ONLY under `-DARBC_BENCHMARKS=ON` (the `bench` preset,
// Release). The workload bodies live in pool_bench_workloads.hpp and are also
// driven -- and behaviorally asserted -- by src/pool/t/bench_smoke.t.cpp under
// the normal test build, which is what gives that logic diff coverage. Nothing
// here asserts a duration or a ratio: benchmarks TREND via the tracked history,
// they do not gate (doc 16:225-226).
//
// COVERAGE EXCLUSION (doc 16:114-116, justified): this file holds only the
// Google Benchmark registration/harness wrappers and is not compiled in the
// coverage build (ARBC_BENCHMARKS is off there), so it never appears in the
// coverage report; its measurable logic is the shared header, covered by the
// bench-smoke CTest. These wrappers are trend-only, exercised by the nightly
// bench run -- not gamed around the diff gate.
//
// Run with
//   --benchmark_format=json --benchmark_out=<file>
// so quality.benchmark_history can upload the results unreshaped.

#include "pool_bench_workloads.hpp"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <memory>

namespace {

using namespace arbc::pool_bench;

// Tree depth for the producer/consumer topology (2^depth - 1 nodes).
constexpr int k_tree_depth = 12; // 4095 nodes

// --- Version production: producer publishes one immutable tree version. -------

void BM_VersionProduction_Managed(benchmark::State& state) {
  arbc::Arena arena;
  arbc::RefStore<BenchNode> store(arena);
  arbc::DeferredReclaimSink<BenchNode> sink(store);
  arbc::ReclamationQueue queue;
  queue.register_store(store, sink);
  for (auto _ : state) {
    std::uint64_t counter = 0;
    arbc::Ref<BenchNode> root = build_managed_tree(store, k_tree_depth, counter);
    benchmark::DoNotOptimize(root.get());
    root = arbc::Ref<BenchNode>{}; // drop the version -> enqueue
    queue.drain();                 // reclaim it before the next iteration
  }
}
BENCHMARK(BM_VersionProduction_Managed);

void BM_VersionProduction_Shared(benchmark::State& state) {
  for (auto _ : state) {
    std::uint64_t counter = 0;
    std::shared_ptr<SharedNode> root = build_shared_tree(k_tree_depth, counter);
    benchmark::DoNotOptimize(root.get());
    root.reset(); // recursive teardown
  }
}
BENCHMARK(BM_VersionProduction_Shared);

// --- Traversal: consumer walks a pinned version. -----------------------------
// Headline pair: managed `peek` vs shared_ptr `const&` (the FAIR baseline).

void BM_Traversal_Managed_Peek(benchmark::State& state) {
  arbc::Arena arena;
  arbc::RefStore<BenchNode> store(arena);
  std::uint64_t counter = 0;
  arbc::Ref<BenchNode> root = build_managed_tree(store, k_tree_depth, counter);
  for (auto _ : state) {
    std::uint64_t sum = traverse_managed_peek(store, root.get());
    benchmark::DoNotOptimize(sum);
  }
}
BENCHMARK(BM_Traversal_Managed_Peek);

void BM_Traversal_Shared_ConstRef(benchmark::State& state) {
  std::uint64_t counter = 0;
  std::shared_ptr<SharedNode> root = build_shared_tree(k_tree_depth, counter);
  for (auto _ : state) {
    std::uint64_t sum = traverse_shared_constref(root);
    benchmark::DoNotOptimize(sum);
  }
}
BENCHMARK(BM_Traversal_Shared_ConstRef);

// The dishonest by-value baseline, explicitly labeled so the flattery doc
// 15:75-80 calls out is visible in the output, never quoted as the headline.
void BM_Traversal_Shared_ByValue_baseline(benchmark::State& state) {
  std::uint64_t counter = 0;
  std::shared_ptr<SharedNode> root = build_shared_tree(k_tree_depth, counter);
  for (auto _ : state) {
    std::uint64_t sum = traverse_shared_byvalue(root);
    benchmark::DoNotOptimize(sum);
  }
}
BENCHMARK(BM_Traversal_Shared_ByValue_baseline);

// --- Steady-state allocation: warm pop-from-freelist vs new/make_shared. ------

void BM_Alloc_WarmCreate(benchmark::State& state) {
  arbc::Arena arena;
  arbc::RefStore<std::uint64_t> store(arena);
  warm_store(store, 1024);
  std::uint64_t v = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(warm_create_once(store, v++));
  }
}
BENCHMARK(BM_Alloc_WarmCreate);

void BM_Alloc_New(benchmark::State& state) {
  std::uint64_t v = 0;
  for (auto _ : state) {
    auto* p = new std::uint64_t(v++);
    benchmark::DoNotOptimize(p);
    delete p;
  }
}
BENCHMARK(BM_Alloc_New);

void BM_Alloc_MakeShared(benchmark::State& state) {
  std::uint64_t v = 0;
  for (auto _ : state) {
    auto p = std::make_shared<std::uint64_t>(v++);
    benchmark::DoNotOptimize(p.get());
  }
}
BENCHMARK(BM_Alloc_MakeShared);

// --- Churn cycle: allocate N -> release all -> drain, repeated. ---------------

void BM_Churn_Managed(benchmark::State& state) {
  arbc::Arena arena;
  arbc::RefStore<std::uint64_t> store(arena);
  arbc::DeferredReclaimSink<std::uint64_t> sink(store);
  arbc::ReclamationQueue queue;
  queue.register_store(store, sink);
  const int n = static_cast<int>(state.range(0));
  for (auto _ : state) {
    churn_cycle(store, queue, n);
  }
}
BENCHMARK(BM_Churn_Managed)->Arg(1024);

// --- Deep-subtree reclaim: drop the root, time the cascade vs recursion. ------

void BM_DeepReclaim_Managed_Drain(benchmark::State& state) {
  arbc::Arena arena;
  arbc::RefStore<BenchNode> store(arena);
  arbc::DeferredReclaimSink<BenchNode> sink(store);
  arbc::ReclamationQueue queue;
  queue.register_store(store, sink);
  for (auto _ : state) {
    state.PauseTiming();
    std::uint64_t counter = 0;
    arbc::Ref<BenchNode> root = build_managed_tree(store, k_tree_depth, counter);
    state.ResumeTiming();
    root = arbc::Ref<BenchNode>{}; // drop the root -> whole subtree enqueues
    queue.drain();                 // iterative cascade
  }
}
BENCHMARK(BM_DeepReclaim_Managed_Drain);

void BM_DeepReclaim_Shared_Recursive(benchmark::State& state) {
  for (auto _ : state) {
    state.PauseTiming();
    std::uint64_t counter = 0;
    std::shared_ptr<SharedNode> root = build_shared_tree(k_tree_depth, counter);
    state.ResumeTiming();
    root.reset(); // recursive destructor storm
  }
}
BENCHMARK(BM_DeepReclaim_Shared_Recursive);

} // namespace

BENCHMARK_MAIN();
