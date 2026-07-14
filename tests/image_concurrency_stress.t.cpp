// org.arbc.image concurrency (doc 16 tier-6, tsan lane). The mirror image of
// `imageseq_concurrency_stress.t.cpp`, and deliberately so: imageseq declares
// `render_thread_safe() == false` because its decoder and LRU are stateful, so the core
// SERIALIZES its renders through the per-content queue. `org.arbc.image` declares TRUE --
// its pyramid is immutable after construction, so a render is a pure read -- and therefore it
// genuinely runs CONCURRENTLY on workers (doc 00:203's leaf-only worker dispatch). That is
// the win Decision 4 is claiming, so it is the thing this test has to hold to.
//
// Two races, both real:
//
//   * Concurrent RENDERS of one content across the worker pool. Every render allocates from
//     (and releases back to) the plugin-owned tile free list, so the free list is genuinely
//     shared mutable state on the hot path even though the pyramid is not.
//   * Concurrent CONSTRUCTION of several contents resolving to ONE URI. The pyramid cache is
//     the state at risk here (a `weak_ptr` map keyed by resolved URI); in production
//     construction is writer-thread-only, but the counter claim -- one decode per resolved
//     identity -- must hold under a race, not merely under a convention.
//
// Outcome-only assertions, no wall-clock. Runs green under dev, ASan/UBSan and
// `ctest --preset tsan`.

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/time.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/kind_image/image_content.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/runtime/worker_pool.hpp>

#include <catch2/catch_test_macros.hpp>

#include "support/image_fixtures.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace arbc;
namespace fix = arbc::image::testfix;

TEST_CASE("concurrent renders of one org.arbc.image are race-free across the worker pool") {
  CpuBackend backend;
  auto content = fix::make_content();
  REQUIRE(content->render_thread_safe()); // the immutable pyramid opts IN to worker dispatch

  constexpr int k_tasks = 200;
  constexpr int k_edge = 32;

  std::vector<std::unique_ptr<Surface>> targets;
  std::vector<std::shared_ptr<RenderCompletion>> dones;
  targets.reserve(k_tasks);
  dones.reserve(k_tasks);
  for (int i = 0; i < k_tasks; ++i) {
    auto target = backend.make_surface(k_edge, k_edge, k_working_rgba32f);
    REQUIRE(target.has_value());
    targets.push_back(std::move(*target));
    dones.push_back(std::make_shared<RenderCompletion>());
  }

  WorkerPool pool(WorkerPoolConfig{/*worker_count=*/4});
  CompletionCursor cursor;

  std::atomic<bool> go{false};
  std::vector<std::thread> producers;
  constexpr int k_producers = 4;
  for (int p = 0; p < k_producers; ++p) {
    producers.emplace_back([&, p] {
      while (!go.load(std::memory_order_acquire)) {
      }
      for (int i = p; i < k_tasks; i += k_producers) {
        // Walk the image in tile-sized strides so different workers hit different regions --
        // and, at 0.5 scale, different pyramid rungs -- all reading the one immutable pyramid.
        const auto idx = static_cast<std::size_t>(i);
        const double x0 = static_cast<double>((i % 8) * 32);
        const double y0 = static_cast<double>((i % 5) * 32);
        const double scale = ((i % 2) == 0) ? 1.0 : 0.5;
        const double extent = static_cast<double>(k_edge) / scale;
        RenderTask task{content.get(),
                        RenderRequest{Rect{x0, y0, x0 + extent, y0 + extent}, scale, Time::zero(),
                                      StateHandle{}, *targets[idx], Exactness::Exact,
                                      Deadline::none()},
                        dones[idx]};
        pool.submit(std::move(task));
      }
    });
  }

  go.store(true, std::memory_order_release);
  for (std::thread& t : producers) {
    t.join();
  }

  int settled = 0;
  while (settled < k_tasks) {
    pool.wait_completions(cursor, std::nullopt);
    settled = 0;
    for (int i = 0; i < k_tasks; ++i) {
      settled += dones[static_cast<std::size_t>(i)]->settled() ? 1 : 0;
    }
  }

  // Every submission settled exactly once, with pixels -- and, because the content is
  // thread-safe, several were genuinely in flight at once (the pool does NOT throttle it to
  // one, which is exactly what distinguishes this kind from imageseq).
  for (int i = 0; i < k_tasks; ++i) {
    const auto idx = static_cast<std::size_t>(i);
    const std::optional<expected<RenderResult, RenderError>> taken = dones[idx]->take();
    REQUIRE(taken.has_value());
    REQUIRE(taken->has_value());
    REQUIRE((*taken)->provided.has_value());
    // Static content: no achieved_time, so it adds no time dimension to the cache key.
    CHECK((*taken)->achieved_time == std::nullopt);
    CHECK(dones[idx]->take() == std::nullopt); // settles at most once
  }
  CHECK(pool.tasks_completed() == static_cast<std::uint64_t>(k_tasks));
}

TEST_CASE("concurrent construction against one resolved URI issues exactly one decode") {
  const std::string bytes = fix::fixture_bytes();
  REQUIRE_FALSE(bytes.empty());

  // A cache LOCAL to this test, so the counter is isolated from every other decode in the
  // binary and the assertion is an exact equality rather than a delta.
  arbc::image::PyramidCache cache;

  constexpr int k_threads = 8;
  std::atomic<bool> go{false};
  std::vector<std::thread> racers;
  std::vector<arbc::image::PyramidPtr> got(k_threads);
  racers.reserve(k_threads);
  for (int i = 0; i < k_threads; ++i) {
    racers.emplace_back([&, i] {
      while (!go.load(std::memory_order_acquire)) {
      }
      // Every thread resolves the SAME URI. Whichever wins the race decodes; the rest must
      // observe its pyramid, not decode a second one.
      got[static_cast<std::size_t>(i)] =
          cache.resolve("project/assets/bg.ppm",
                        std::span<const unsigned char>(
                            reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()));
    });
  }
  go.store(true, std::memory_order_release);
  for (std::thread& t : racers) {
    t.join();
  }

  // Exactly ONE decode, and every racer holds the very same pyramid -- no data race on the
  // cache, and dedup by resolved identity (doc 08:116-122) survives concurrency.
  CHECK(cache.decodes_issued() == 1);
  for (const arbc::image::PyramidPtr& p : got) {
    REQUIRE(p != nullptr);
    CHECK(p.get() == got[0].get());
  }
}
