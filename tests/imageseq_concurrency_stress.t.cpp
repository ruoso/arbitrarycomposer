// org.arbc.imageseq concurrency (doc 16 tier-6, tsan lane). Because imageseq is
// a stateful decoder it declares render_thread_safe() == false (Decision 5), so
// the core serializes its requests through the per-content queue
// (runtime.threading / WorkerPool). This drives concurrent interactive renders
// through the pool while a separate thread races the decoder with
// temporal-prefetch playback_hint calls, and asserts: at most one in-flight
// render for the content, every submission settles exactly once with the correct
// achieved_time, and no data race (the shared decoded-frame cache + pre-roll
// state are mutex-guarded). Outcome-only assertions, no wall-clock; runs green
// under dev, ASan/UBSan, and `ctest --preset tsan`.

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/rational_time.hpp>
#include <arbc/base/time.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/runtime/worker_pool.hpp>

#include <catch2/catch_test_macros.hpp>

#include "support/imageseq_fixtures.hpp"

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

using namespace arbc;
namespace fix = arbc::imageseq::testfix;

namespace {

Time instant(int frame) { return Time{frame * fix::k_period_flicks}; }

} // namespace

// enforces: 02-architecture#worker-pool-serializes-non-thread-safe-content
TEST_CASE("imageseq renders serialize through the per-content queue race-free") {
  CpuBackend backend;
  auto content = fix::make_content();
  REQUIRE_FALSE(content->render_thread_safe()); // the stateful decoder opts in to serialization

  constexpr int k_tasks = 240;

  // Caller-owned targets + completions (the pool references, never owns them).
  std::vector<std::unique_ptr<Surface>> targets;
  std::vector<std::shared_ptr<RenderCompletion>> dones;
  std::vector<Time> times;
  targets.reserve(k_tasks);
  dones.reserve(k_tasks);
  times.reserve(k_tasks);
  for (int i = 0; i < k_tasks; ++i) {
    auto target = backend.make_surface(fix::k_width, fix::k_height, k_working_rgba32f);
    REQUIRE(target.has_value());
    targets.push_back(std::move(*target));
    dones.push_back(std::make_shared<RenderCompletion>());
    times.push_back(instant(i % fix::k_frame_count));
  }

  WorkerPool pool(WorkerPoolConfig{/*worker_count=*/4});

  // A background thread races the decoder with temporal-prefetch hints while
  // renders are in flight (drive_playback_prefetch fans hints to content off the
  // pool's threads, so render vs playback_hint is a genuine concurrent pair).
  std::atomic<bool> stop{false};
  std::thread hinter([&] {
    while (!stop.load(std::memory_order_acquire)) {
      content->playback_hint(PlaybackHint{+1, Rational{1, 1}, Time{2 * fix::k_period_flicks}});
    }
  });

  // Producers submit renders concurrently.
  std::atomic<bool> go{false};
  std::vector<std::thread> producers;
  constexpr int k_producers = 4;
  for (int p = 0; p < k_producers; ++p) {
    producers.emplace_back([&, p] {
      while (!go.load(std::memory_order_acquire)) {
      }
      for (int i = p; i < k_tasks; i += k_producers) {
        // RenderRequest holds a Surface& (reference member), so RenderTask is
        // aggregate-initialized in one shot -- it is not default-constructible
        // or assignable.
        RenderTask task{content.get(),
                        RenderRequest{Rect{0.0, 0.0, fix::k_width, fix::k_height}, 1.0,
                                      times[static_cast<std::size_t>(i)], StateHandle{},
                                      *targets[static_cast<std::size_t>(i)], Exactness::Exact,
                                      Deadline::none()},
                        dones[static_cast<std::size_t>(i)]};
        pool.submit(std::move(task));
      }
    });
  }

  go.store(true, std::memory_order_release);
  for (std::thread& t : producers) {
    t.join();
  }

  // Drain every completion (park on the settle condition, never a fixed sleep).
  int settled = 0;
  while (settled < k_tasks) {
    pool.wait_completions(std::nullopt);
    settled = 0;
    for (int i = 0; i < k_tasks; ++i) {
      settled += dones[static_cast<std::size_t>(i)]->settled() ? 1 : 0;
    }
  }

  stop.store(true, std::memory_order_release);
  hinter.join();

  // Every submission settled exactly once with the correct achieved_time, and
  // the serialized decoder never had more than one concurrent render.
  for (int i = 0; i < k_tasks; ++i) {
    const std::optional<expected<RenderResult, RenderError>> taken =
        dones[static_cast<std::size_t>(i)]->take();
    REQUIRE(taken.has_value());
    REQUIRE(taken->has_value());
    REQUIRE((*taken)->achieved_time == instant(i % fix::k_frame_count));
    REQUIRE(dones[static_cast<std::size_t>(i)]->take() == std::nullopt); // settles at most once
  }
  CHECK(pool.tasks_completed() == static_cast<std::uint64_t>(k_tasks));
  CHECK(pool.max_in_flight_per_content() == 1);
}
