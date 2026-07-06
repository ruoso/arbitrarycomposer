#include <arbc/base/geometry.hpp>
#include <arbc/base/time.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/media/pixel_traits.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/model/records.hpp>
#include <arbc/runtime/worker_pool.hpp>
#include <arbc/surface/surface.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <vector>

// Unit + stress tests for the runtime worker pool (doc 16:46,54-73). Every
// assertion is on a behavioral counter, a `RenderCompletion` settlement, or a
// pool counter; synchronization is via `wait_completions` (a completion-count
// condition) and atomic `go`/`stop` flags -- no test reads a wall clock to
// synchronize (the one `steady_clock::now()` use is a negative-test park bound,
// asserted only via the return value, doc 16:54-62).

namespace {

// Self-contained in-memory `Surface` with real rgba32f storage so a `render`
// fill is byte-observable without linking a backend (mirrors async_render.t.cpp).
class MemSurface : public arbc::Surface {
public:
  MemSurface(int w, int h)
      : d_w(w), d_h(h),
        d_pixels(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4U, 0.0F) {}

  int width() const override { return d_w; }
  int height() const override { return d_h; }
  arbc::SurfaceFormat format() const override { return arbc::k_working_rgba32f; }
  std::span<std::byte> cpu_bytes() override {
    return {reinterpret_cast<std::byte*>(d_pixels.data()), d_pixels.size() * sizeof(float)};
  }
  std::span<const std::byte> cpu_bytes() const override {
    return {reinterpret_cast<const std::byte*>(d_pixels.data()), d_pixels.size() * sizeof(float)};
  }
  const std::vector<float>& pixels() const { return d_pixels; }

private:
  int d_w;
  int d_h;
  std::vector<float> d_pixels;
};

// The shared deterministic fill: a function of the request only, so the pool's
// output for a request is byte-identical to calling `render` directly on it.
arbc::RenderResult paint(const arbc::RenderRequest& request) {
  const std::span<float> px = request.target.span<arbc::PixelFormat::Rgba32fLinearPremul>();
  const float seed = static_cast<float>(request.region.x0) + static_cast<float>(request.scale);
  for (std::size_t i = 0; i < px.size(); ++i) {
    px[i] = seed + static_cast<float>(i);
  }
  return arbc::RenderResult{request.scale, true};
}

// A synchronous stub content: solid-fills its target, records the thread it ran
// on and a per-content concurrency high-water, and settles INLINE by returning a
// `RenderResult`. `render_thread_safe()` is configurable so a test drives the
// serialization gate against a real override. `wait_for_overlap` forces observed
// concurrency for thread-safe contents (bounded spin, no sleep, no hang).
class StubContent : public arbc::Content {
public:
  StubContent(bool thread_safe, bool wait_for_overlap)
      : d_thread_safe(thread_safe), d_wait_for_overlap(wait_for_overlap) {}

  std::optional<arbc::Rect> bounds() const override { return std::nullopt; }
  arbc::Stability stability() const override { return arbc::Stability::Static; }
  std::optional<arbc::TimeRange> time_extent() const override { return std::nullopt; }
  bool render_thread_safe() const override { return d_thread_safe; }

  std::optional<arbc::RenderResult> render(const arbc::RenderRequest& request,
                                           std::shared_ptr<arbc::RenderCompletion>) override {
    const int now = d_in_flight.fetch_add(1, std::memory_order_acq_rel) + 1;
    int prev = d_max_in_flight.load(std::memory_order_relaxed);
    while (now > prev && !d_max_in_flight.compare_exchange_weak(prev, now)) {
    }
    d_render_calls.fetch_add(1, std::memory_order_acq_rel);
    d_last_thread.store(std::this_thread::get_id(), std::memory_order_release);

    // Widen the race window: yield so a second concurrent render (for thread-safe
    // content) overlaps this one. Bounded, so a serialized content never hangs.
    if (d_wait_for_overlap) {
      for (int spin = 0; spin < 100000 && d_in_flight.load(std::memory_order_acquire) < 2; ++spin) {
        std::this_thread::yield();
      }
    } else {
      std::this_thread::yield();
    }

    paint(request);
    d_in_flight.fetch_sub(1, std::memory_order_acq_rel);
    return arbc::RenderResult{request.scale, true};
  }

  int max_in_flight() const { return d_max_in_flight.load(std::memory_order_acquire); }
  std::uint64_t render_calls() const { return d_render_calls.load(std::memory_order_acquire); }
  std::thread::id last_thread() const { return d_last_thread.load(std::memory_order_acquire); }

private:
  bool d_thread_safe;
  bool d_wait_for_overlap;
  std::atomic<int> d_in_flight{0};
  std::atomic<int> d_max_in_flight{0};
  std::atomic<std::uint64_t> d_render_calls{0};
  std::atomic<std::thread::id> d_last_thread{};
};

// An async stub: stores `done`, returns nullopt, settles later via `deliver()`.
class AsyncStub : public arbc::Content {
public:
  std::optional<arbc::Rect> bounds() const override { return std::nullopt; }
  arbc::Stability stability() const override { return arbc::Stability::Static; }
  std::optional<arbc::TimeRange> time_extent() const override { return std::nullopt; }

  std::optional<arbc::RenderResult> render(const arbc::RenderRequest& request,
                                           std::shared_ptr<arbc::RenderCompletion> done) override {
    // Copy the request by value: it is a transient descriptor that does not
    // outlive the `render` call (the pool moves the `RenderTask` away once
    // render returns). The copied `Surface&` still refers to the caller-owned
    // target, which outlives the frame (`refinement.hpp:65-79`).
    d_request.emplace(request);
    d_done = std::move(done);
    return std::nullopt;
  }
  void deliver() { d_done->complete(paint(*d_request)); }

private:
  std::optional<arbc::RenderRequest> d_request;
  std::shared_ptr<arbc::RenderCompletion> d_done;
};

arbc::RenderRequest make_request(MemSurface& target, double scale) {
  return arbc::RenderRequest{arbc::Rect::from_size(2.0, 2.0), scale, arbc::Time::zero(),
                             arbc::StateHandle{}, target};
}

} // namespace

// enforces: 02-architecture#worker-pool-degenerates-to-inline
// enforces: 03-layer-plugin-interface#render-inline-or-async
TEST_CASE("inline degenerate mode settles on the calling thread, byte-identical to direct render") {
  const std::thread::id test_tid = std::this_thread::get_id();

  arbc::WorkerPool pool(arbc::WorkerPoolConfig{}); // worker_count == 0 (default)

  // (a) sync content: submit runs the render inline and settles before returning.
  StubContent content(/*thread_safe=*/true, /*wait_for_overlap=*/false);
  MemSurface pool_target(2, 2);
  auto done = std::make_shared<arbc::RenderCompletion>();
  pool.submit(arbc::RenderTask{&content, make_request(pool_target, 1.5), done});

  REQUIRE(done->settled());                        // settled synchronously on this thread
  REQUIRE(content.last_thread() == test_tid);      // render ran on the calling thread
  REQUIRE(pool.tasks_submitted() == 1);
  REQUIRE(pool.tasks_completed() == 1);

  std::optional<arbc::expected<arbc::RenderResult, arbc::RenderError>> settled = done->take();
  REQUIRE(settled.has_value());
  REQUIRE(settled->has_value());
  REQUIRE_FALSE(done->take().has_value()); // settled exactly once

  // The pool's pixels are byte-identical to calling render() directly.
  MemSurface direct_target(2, 2);
  StubContent direct(/*thread_safe=*/true, /*wait_for_overlap=*/false);
  arbc::RenderRequest direct_req = make_request(direct_target, 1.5);
  (void)direct.render(direct_req, std::make_shared<arbc::RenderCompletion>());
  REQUIRE(pool_target.pixels() == direct_target.pixels());

  // (b) async content through the same submit -> wait_completions -> take path.
  AsyncStub async;
  MemSurface async_target(2, 2);
  auto async_done = std::make_shared<arbc::RenderCompletion>();
  pool.submit(arbc::RenderTask{&async, make_request(async_target, 1.5), async_done});
  REQUIRE_FALSE(async_done->settled()); // async: not settled by the render call
  async.deliver();                      // external settle...
  pool.poke();                          // ...wakes the parked render thread
  REQUIRE(pool.wait_completions(std::nullopt));
  std::optional<arbc::expected<arbc::RenderResult, arbc::RenderError>> a = async_done->take();
  REQUIRE(a.has_value());
  REQUIRE(a->has_value());
  REQUIRE(async_target.pixels() == pool_target.pixels()); // same paint, same bytes
}

// enforces: 02-architecture#worker-pool-degenerates-to-inline
TEST_CASE("real pool renders on workers and wakes the render thread") {
  const std::thread::id test_tid = std::this_thread::get_id();
  constexpr int k = 32;

  arbc::WorkerPoolConfig cfg;
  cfg.worker_count = 4;
  arbc::WorkerPool pool(cfg);

  StubContent content(/*thread_safe=*/true, /*wait_for_overlap=*/false);
  std::vector<std::unique_ptr<MemSurface>> targets;
  std::vector<std::shared_ptr<arbc::RenderCompletion>> dones;
  for (int i = 0; i < k; ++i) {
    targets.push_back(std::make_unique<MemSurface>(2, 2));
    dones.push_back(std::make_shared<arbc::RenderCompletion>());
    pool.submit(arbc::RenderTask{&content, make_request(*targets[i], 1.5), dones[i]});
  }

  std::vector<bool> taken(k, false);
  int got = 0;
  while (got < k) {
    pool.wait_completions(std::nullopt); // park until a completion advances
    for (int i = 0; i < k; ++i) {
      if (!taken[i]) {
        std::optional<arbc::expected<arbc::RenderResult, arbc::RenderError>> s = dones[i]->take();
        if (s.has_value()) {
          REQUIRE(s->has_value());
          taken[i] = true;
          ++got;
        }
      }
    }
  }

  REQUIRE(pool.tasks_submitted() == k);
  REQUIRE(pool.tasks_completed() == k);
  REQUIRE(content.render_calls() == static_cast<std::uint64_t>(k));
  REQUIRE(content.last_thread() != test_tid); // renders ran off the test thread
  for (int i = 0; i < k; ++i) {
    REQUIRE_FALSE(dones[i]->take().has_value()); // each settled exactly once
  }
}

// enforces: 02-architecture#worker-pool-serializes-non-thread-safe-content
TEST_CASE("per-content serialization holds at most one in-flight render") {
  constexpr int serial_count = 24;
  constexpr int safe_count = 24;

  arbc::WorkerPoolConfig cfg;
  cfg.worker_count = 8;
  arbc::WorkerPool pool(cfg);

  StubContent serialized(/*thread_safe=*/false, /*wait_for_overlap=*/false);
  StubContent safe(/*thread_safe=*/true, /*wait_for_overlap=*/true);
  std::vector<std::shared_ptr<arbc::RenderCompletion>> dones;
  std::vector<std::unique_ptr<MemSurface>> targets;

  // Interleave serialized and thread-safe submissions.
  for (int i = 0; i < serial_count + safe_count; ++i) {
    targets.push_back(std::make_unique<MemSurface>(2, 2));
    dones.push_back(std::make_shared<arbc::RenderCompletion>());
    arbc::Content* c = (i % 2 == 0) ? static_cast<arbc::Content*>(&serialized)
                                    : static_cast<arbc::Content*>(&safe);
    pool.submit(arbc::RenderTask{c, make_request(*targets.back(), 1.5), dones.back()});
  }

  const int total = serial_count + safe_count;
  std::vector<bool> taken(total, false);
  int got = 0;
  while (got < total) {
    pool.wait_completions(std::nullopt);
    for (int i = 0; i < total; ++i) {
      if (!taken[i] && dones[i]->take().has_value()) {
        taken[i] = true;
        ++got;
      }
    }
  }

  REQUIRE(pool.tasks_completed() == static_cast<std::uint64_t>(total));
  REQUIRE(pool.max_in_flight_per_content() == 1); // the gate: serialized never > 1
  REQUIRE(serialized.max_in_flight() == 1);       // observed at the stub too
  REQUIRE(safe.max_in_flight() > 1);              // thread-safe content ran concurrently
  for (int i = 0; i < total; ++i) {
    REQUIRE_FALSE(dones[i]->take().has_value()); // each settled exactly once
  }
}

// enforces: 02-architecture#worker-pool-stops-gracefully
TEST_CASE("graceful stop joins without hang and refuses new work") {
  SECTION("submitted work drains, then post-stop submit is refused") {
    arbc::WorkerPoolConfig cfg;
    cfg.worker_count = 4;
    arbc::WorkerPool pool(cfg);

    constexpr int k = 16;
    StubContent content(/*thread_safe=*/true, /*wait_for_overlap=*/false);
    std::vector<std::shared_ptr<arbc::RenderCompletion>> dones;
    std::vector<std::unique_ptr<MemSurface>> targets;
    for (int i = 0; i < k; ++i) {
      targets.push_back(std::make_unique<MemSurface>(2, 2));
      dones.push_back(std::make_shared<arbc::RenderCompletion>());
      pool.submit(arbc::RenderTask{&content, make_request(*targets[i], 1.5), dones[i]});
    }
    int got = 0;
    while (got < k) {
      pool.wait_completions(std::nullopt);
      for (int i = 0; i < k; ++i) {
        if (dones[i]->settled()) {
          std::optional<arbc::expected<arbc::RenderResult, arbc::RenderError>> s = dones[i]->take();
          if (s.has_value()) {
            REQUIRE(s->has_value()); // intact, exactly once
            ++got;
          }
        }
      }
    }

    pool.request_stop();
    const std::uint64_t submitted_before = pool.tasks_submitted();

    // Post-stop submit does not enqueue and leaves the completion unsettled.
    StubContent late(/*thread_safe=*/true, /*wait_for_overlap=*/false);
    MemSurface late_target(2, 2);
    auto late_done = std::make_shared<arbc::RenderCompletion>();
    pool.submit(arbc::RenderTask{&late, make_request(late_target, 1.5), late_done});
    REQUIRE(pool.tasks_submitted() == submitted_before); // not counted, not enqueued
    REQUIRE_FALSE(late_done->settled());                 // never lost to UB
    REQUIRE(late.render_calls() == 0);
  }

  SECTION("construct and immediately destroy a pool with no work: join does not hang") {
    {
      arbc::WorkerPoolConfig cfg;
      cfg.worker_count = 4;
      arbc::WorkerPool pool(cfg);
    } // dtor request_stop + join -- reaching the next line means no hang
    REQUIRE(true);
  }
}

// enforces: 02-architecture#worker-pool-stops-gracefully
TEST_CASE("quiescent substrate is idle: no completions, wait times out to false") {
  arbc::WorkerPoolConfig cfg;
  cfg.worker_count = 4;
  arbc::WorkerPool pool(cfg);

  // No work submitted: nothing settles, and a bounded park returns false without
  // a busy loop. The timeout is a park bound; only the return value is asserted.
  const auto bound = std::chrono::steady_clock::now() + std::chrono::milliseconds(5);
  REQUIRE_FALSE(pool.wait_completions(bound));
  REQUIRE(pool.tasks_completed() == 0);
  REQUIRE(pool.tasks_submitted() == 0);
  REQUIRE(pool.max_in_flight_per_content() == 0);
}

// enforces: 02-architecture#worker-pool-serializes-non-thread-safe-content
TEST_CASE("stress: producers submit interleaved thread-safe and serialized work") {
  // Mirrors housekeeping_thread.t.cpp:285-330 / reclamation.t.cpp:246-303:
  // producer threads spin-wait on an atomic `go` then submit a fixed op count of
  // interleaved thread-safe + serialized work into a worker pool while a consumer
  // thread drains via wait_completions. Outcome assertions only (settled-count ==
  // submit-count, serialized never > 1 in flight); no timing assertion. Runs
  // green under dev, ASan/UBSan, and `ctest --preset tsan`.
  constexpr int producer_count = 6;
  constexpr int per_producer = 200;
  constexpr int total = producer_count * per_producer;

  arbc::WorkerPoolConfig cfg;
  cfg.worker_count = 6;
  arbc::WorkerPool pool(cfg);

  StubContent serialized(/*thread_safe=*/false, /*wait_for_overlap=*/false);
  StubContent safe(/*thread_safe=*/true, /*wait_for_overlap=*/false);

  // Caller-owned targets + completions, pre-allocated so producers never allocate
  // shared state under the race (the pool holds only references).
  std::vector<std::unique_ptr<MemSurface>> targets(total);
  std::vector<std::shared_ptr<arbc::RenderCompletion>> dones(total);
  for (int i = 0; i < total; ++i) {
    targets[i] = std::make_unique<MemSurface>(2, 2);
    dones[i] = std::make_shared<arbc::RenderCompletion>();
  }

  std::atomic<bool> go{false};
  std::vector<std::thread> producers;
  for (int p = 0; p < producer_count; ++p) {
    producers.emplace_back([&, p] {
      while (!go.load(std::memory_order_acquire)) {
      }
      for (int i = 0; i < per_producer; ++i) {
        const int idx = p * per_producer + i;
        // Every 4th op is the shared serialized content; the rest are thread-safe.
        arbc::Content* c = (i % 4 == 0) ? static_cast<arbc::Content*>(&serialized)
                                        : static_cast<arbc::Content*>(&safe);
        pool.submit(arbc::RenderTask{c, make_request(*targets[idx], 1.5), dones[idx]});
        if ((i % 16) == 0) {
          std::this_thread::yield(); // schedule perturbation widens the race window
        }
      }
    });
  }

  // Consumer: drain settled completions until every submission has settled once.
  std::thread consumer([&] {
    std::vector<bool> taken(total, false);
    int got = 0;
    while (got < total) {
      pool.wait_completions(std::chrono::steady_clock::now() + std::chrono::milliseconds(1));
      for (int i = 0; i < total; ++i) {
        if (!taken[i] && dones[i]->take().has_value()) {
          taken[i] = true;
          ++got;
        }
      }
    }
  });

  go.store(true, std::memory_order_release);
  for (std::thread& t : producers) {
    t.join();
  }
  consumer.join();

  REQUIRE(pool.tasks_submitted() == static_cast<std::uint64_t>(total));
  REQUIRE(pool.tasks_completed() == static_cast<std::uint64_t>(total)); // each settled once
  REQUIRE(pool.max_in_flight_per_content() == 1); // serialized gate held throughout
  REQUIRE(serialized.max_in_flight() == 1);
  for (int i = 0; i < total; ++i) {
    REQUIRE_FALSE(dones[i]->take().has_value()); // none double-settled
  }
}
