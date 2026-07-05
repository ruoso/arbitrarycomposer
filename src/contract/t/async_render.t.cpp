#include <arbc/base/geometry.hpp>
#include <arbc/base/time.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/model/records.hpp>
#include <arbc/surface/surface.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <thread>
#include <vector>

namespace {

// Self-contained in-memory `Surface` with real rgba32f storage so `render()`
// output is byte-observable without linking a backend -- keeps the test at L3
// (mirrors the double in snapshot_pins.t.cpp).
class MemSurface : public arbc::Surface {
public:
  MemSurface(int w, int h, arbc::SurfaceFormat fmt)
      : d_w(w), d_h(h), d_fmt(fmt),
        d_pixels(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4U, 0.0F) {}

  int width() const override { return d_w; }
  int height() const override { return d_h; }
  arbc::SurfaceFormat format() const override { return d_fmt; }
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
  arbc::SurfaceFormat d_fmt;
  std::vector<float> d_pixels;
};

constexpr arbc::SurfaceFormat k_fmt = arbc::k_working_rgba32f;

// The shared, deterministic pixel-writing logic. Both the sync and the async
// test content settle by calling this against the *same* request, so their
// outputs must be byte-identical -- the whole point of the one-code-path
// golden.
arbc::RenderResult paint(const arbc::RenderRequest& request) {
  const std::span<float> px = request.target.span<arbc::PixelFormat::Rgba32fLinearPremul>();
  const float seed = static_cast<float>(request.region.x0) + static_cast<float>(request.region.y0) +
                     static_cast<float>(request.scale) + static_cast<float>(request.time.flicks);
  for (std::size_t i = 0; i < px.size(); ++i) {
    px[i] = seed + static_cast<float>(i);
  }
  return arbc::RenderResult{request.scale, true};
}

// Synchronous content: settles INLINE, returning the result and never storing
// `done` (returns non-nullopt).
class SyncContent : public arbc::Content {
public:
  std::optional<arbc::Rect> bounds() const override { return std::nullopt; }
  arbc::Stability stability() const override { return arbc::Stability::Static; }

  std::optional<arbc::RenderResult> render(const arbc::RenderRequest& request,
                                           std::shared_ptr<arbc::RenderCompletion>) override {
    return paint(request);
  }
};

// Asynchronous content: stores `done` and the request, returns `nullopt`, and
// settles later when the test calls `deliver()` with the identical `paint`
// logic. The request must outlive `deliver()` (the test keeps it alive), as a
// real async content would copy what it needs; a raw pointer keeps the double
// minimal.
class AsyncContent : public arbc::Content {
public:
  std::optional<arbc::Rect> bounds() const override { return std::nullopt; }
  arbc::Stability stability() const override { return arbc::Stability::Static; }

  std::optional<arbc::RenderResult> render(const arbc::RenderRequest& request,
                                           std::shared_ptr<arbc::RenderCompletion> done) override {
    d_request = &request;
    d_done = std::move(done);
    return std::nullopt;
  }

  void deliver() { d_done->complete(paint(*d_request)); }

private:
  const arbc::RenderRequest* d_request{nullptr};
  std::shared_ptr<arbc::RenderCompletion> d_done;
};

// Drive `content` through the one settle path: render -> (fold an inline result
// through `complete`, else let the caller settle async) -> `take()`. Returns
// the target pixels and the settled `RenderResult` by out-param.
template <class SettleAsync>
std::vector<float> drive(arbc::Content& content, const arbc::RenderRequest& request,
                         SettleAsync settle_async, arbc::RenderResult& out_result) {
  auto done = std::make_shared<arbc::RenderCompletion>();
  const std::optional<arbc::RenderResult> inline_result = content.render(request, done);
  if (inline_result.has_value()) {
    done->complete(*inline_result); // returned-inline == immediately-completed async
  } else {
    settle_async(); // async content settles off the render call
  }
  std::optional<arbc::expected<arbc::RenderResult, arbc::RenderError>> settled = done->take();
  REQUIRE(settled.has_value());
  REQUIRE(settled->has_value());
  out_result = **settled;
  return static_cast<const MemSurface&>(request.target).pixels();
}

} // namespace

// enforces: 03-layer-plugin-interface#render-inline-or-async
TEST_CASE("one code path: sync-inline and async-deferred renders settle byte-identically") {
  const arbc::Rect region = arbc::Rect::from_size(2.0, 2.0);

  MemSurface sync_target(2, 2, k_fmt);
  const arbc::RenderRequest sync_request{region, 1.5, arbc::Time::zero(), arbc::StateHandle{},
                                         sync_target};
  SyncContent sync_content;
  arbc::RenderResult sync_result{};
  const std::vector<float> sync_pixels = drive(sync_content, sync_request, [] {}, sync_result);

  MemSurface async_target(2, 2, k_fmt);
  const arbc::RenderRequest async_request{region, 1.5, arbc::Time::zero(), arbc::StateHandle{},
                                          async_target};
  AsyncContent async_content;
  arbc::RenderResult async_result{};
  const std::vector<float> async_pixels =
      drive(async_content, async_request, [&] { async_content.deliver(); }, async_result);

  // Byte-identical target buffers and equal RenderResult (doc 16:48-53).
  REQUIRE(sync_pixels == async_pixels);
  REQUIRE(sync_result.achieved_scale == async_result.achieved_scale);
  REQUIRE(sync_result.exact == async_result.exact);
}

// enforces: 03-layer-plugin-interface#render-completion-settles-once
TEST_CASE("take() yields the single settlement once and reports settled state") {
  arbc::RenderCompletion c;

  // (a) before settle: take() is nullopt, settled() is false.
  REQUIRE_FALSE(c.settled());
  REQUIRE_FALSE(c.take().has_value());

  c.complete(arbc::RenderResult{0.5, false});
  REQUIRE(c.settled());

  // after complete: take() yields the value once, then nullopt.
  std::optional<arbc::expected<arbc::RenderResult, arbc::RenderError>> first = c.take();
  REQUIRE(first.has_value());
  REQUIRE(first->has_value());
  REQUIRE((**first).achieved_scale == 0.5);
  REQUIRE_FALSE((**first).exact);
  REQUIRE_FALSE(c.take().has_value());
  REQUIRE(c.settled()); // still records that a settlement occurred
}

// enforces: 03-layer-plugin-interface#render-completion-settles-once
TEST_CASE("fail() surfaces the error through take()") {
  arbc::RenderCompletion c;
  c.fail(arbc::RenderError::ContentFailed);

  std::optional<arbc::expected<arbc::RenderResult, arbc::RenderError>> settled = c.take();
  REQUIRE(settled.has_value());
  REQUIRE_FALSE(settled->has_value());
  REQUIRE(settled->error() == arbc::RenderError::ContentFailed);
}

// enforces: 03-layer-plugin-interface#render-completion-settles-once
TEST_CASE("a second settle is ignored: the first settlement stands") {
  SECTION("complete then complete") {
    arbc::RenderCompletion c;
    c.complete(arbc::RenderResult{0.25, true});
    c.complete(arbc::RenderResult{0.99, false}); // ignored
    std::optional<arbc::expected<arbc::RenderResult, arbc::RenderError>> settled = c.take();
    REQUIRE(settled.has_value());
    REQUIRE(settled->has_value());
    REQUIRE((**settled).achieved_scale == 0.25);
    REQUIRE((**settled).exact);
  }
  SECTION("complete then fail") {
    arbc::RenderCompletion c;
    c.complete(arbc::RenderResult{0.25, true});
    c.fail(arbc::RenderError::ResourceUnavailable); // ignored
    std::optional<arbc::expected<arbc::RenderResult, arbc::RenderError>> settled = c.take();
    REQUIRE(settled.has_value());
    REQUIRE(settled->has_value());
    REQUIRE((**settled).achieved_scale == 0.25);
  }
}

// enforces: 03-layer-plugin-interface#render-completion-settles-once
TEST_CASE("cancel() is advisory: cancelled() observes true yet a later complete still settles") {
  arbc::RenderCompletion c;
  REQUIRE_FALSE(c.cancelled());
  c.cancel();
  REQUIRE(c.cancelled());

  // Cancellation does not prevent settlement (doc 03:66,122-123).
  c.complete(arbc::RenderResult{0.75, true});
  std::optional<arbc::expected<arbc::RenderResult, arbc::RenderError>> settled = c.take();
  REQUIRE(settled.has_value());
  REQUIRE(settled->has_value());
  REQUIRE((**settled).achieved_scale == 0.75);
  REQUIRE(c.cancelled()); // still observably cancelled after settlement
}

// enforces: 03-layer-plugin-interface#render-completion-settles-once
TEST_CASE("concurrent complete and cancel/take: exactly one settlement, no torn payload") {
  // TSan/stress case (doc 16:66-73): a renderer thread settles while a consumer
  // thread races cancel()/cancelled()/take(), iterated under seeded schedule
  // perturbation (randomized yields). Asserts exactly-one settlement and an
  // intact payload -- the achieved_scale must equal the value the renderer
  // wrote, never a tear.
  for (unsigned seed = 0; seed < 256U; ++seed) {
    auto c = std::make_shared<arbc::RenderCompletion>();
    const double expected_scale = 0.5 + static_cast<double>(seed);

    std::thread renderer([&c, seed, expected_scale] {
      std::mt19937 rng(seed);
      if ((rng() & 1U) != 0U) {
        std::this_thread::yield();
      }
      c->complete(arbc::RenderResult{expected_scale, false});
    });

    std::mt19937 rng(seed ^ 0x9e3779b9U);
    std::optional<arbc::expected<arbc::RenderResult, arbc::RenderError>> taken;
    for (int spin = 0; spin < 4096 && !taken.has_value(); ++spin) {
      if ((rng() & 1U) != 0U) {
        c->cancel();
      }
      (void)c->cancelled();
      taken = c->take();
      if ((rng() & 2U) != 0U) {
        std::this_thread::yield();
      }
    }

    renderer.join();
    if (!taken.has_value()) {
      taken = c->take(); // drain the settlement that landed after the loop
    }

    REQUIRE(taken.has_value());
    REQUIRE(taken->has_value());
    REQUIRE((**taken).achieved_scale == expected_scale); // no torn payload
    REQUIRE_FALSE((**taken).exact);
    REQUIRE_FALSE(c->take().has_value()); // settled exactly once
  }
}

// enforces: 03-layer-plugin-interface#render-inline-or-async
TEST_CASE("RenderRequest defaults exactness to BestEffort and deadline to none()") {
  MemSurface target(1, 1, k_fmt);
  const arbc::RenderRequest request{arbc::Rect::from_size(1.0, 1.0), 1.0, arbc::Time::zero(),
                                    arbc::StateHandle{}, target};
  REQUIRE(request.exactness == arbc::Exactness::BestEffort);
  REQUIRE(request.deadline == arbc::Deadline::none());
  REQUIRE(request.deadline.is_none());
}
