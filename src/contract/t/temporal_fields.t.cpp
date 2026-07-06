#include <arbc/base/geometry.hpp>
#include <arbc/base/time.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/model/records.hpp>
#include <arbc/surface/surface.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace {

// A self-contained in-memory `Surface` with real rgba32f storage, so `render()`
// output is byte-observable via the checked typed span without linking a
// backend (mirrors the double in snapshot_pins.t.cpp / async_render.t.cpp).
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

// A concrete `Timed` test content: a 24 fps clip. It quantizes the requested
// local time DOWN to its native frame grid, paints each target pixel as a pure
// function of that quantized instant, and reports the quantized instant via
// `achieved_time` -- the temporal analog of `achieved_scale` (doc 11:110-114).
// Its temporal extent is a fixed half-open duration `[0, k_frame * k_frames)`.
class TimedContent : public arbc::Content {
public:
  static constexpr std::int64_t k_frame = arbc::Time::flicks_per_second / 24; // 24 fps
  static constexpr std::int64_t k_frames = 48;                                // two seconds

  std::optional<arbc::Rect> bounds() const override { return std::nullopt; }
  arbc::Stability stability() const override { return arbc::Stability::Timed; }
  std::optional<arbc::TimeRange> time_extent() const override {
    return arbc::TimeRange{arbc::Time::zero(), arbc::Time{k_frame * k_frames}};
  }

  std::optional<arbc::RenderResult> render(const arbc::RenderRequest& request,
                                           std::shared_ptr<arbc::RenderCompletion>) override {
    // Quantize down to the native frame (request.time is non-negative here).
    const std::int64_t quantized = (request.time.flicks / k_frame) * k_frame;
    const std::span<float> px = request.target.span<arbc::PixelFormat::Rgba32fLinearPremul>();
    for (std::size_t i = 0; i < px.size(); ++i) {
      px[i] = static_cast<float>(quantized) + static_cast<float>(i);
    }
    arbc::RenderResult result{};
    result.achieved_time = arbc::Time{quantized};
    return result;
  }
};

// A concrete `Static` test content: time-invariant. It paints a fixed pattern
// regardless of `request.time`, reports no `achieved_time` (the requested time
// is honored trivially), and declares no temporal extent (doc 11:69-71).
class StaticContent : public arbc::Content {
public:
  std::optional<arbc::Rect> bounds() const override { return std::nullopt; }
  arbc::Stability stability() const override { return arbc::Stability::Static; }
  std::optional<arbc::TimeRange> time_extent() const override { return std::nullopt; }

  std::optional<arbc::RenderResult> render(const arbc::RenderRequest& request,
                                           std::shared_ptr<arbc::RenderCompletion>) override {
    const std::span<float> px = request.target.span<arbc::PixelFormat::Rgba32fLinearPremul>();
    for (std::size_t i = 0; i < px.size(); ++i) {
      px[i] = 7.0F + static_cast<float>(i);
    }
    return arbc::RenderResult{}; // achieved_time defaults to nullopt
  }
};

// Render `content` against a fresh 2x2 target at `time` and return the target
// pixels plus the settled `RenderResult` (by out-param).
std::vector<float> render_at(arbc::Content& content, arbc::Time time,
                             arbc::RenderResult& out_result) {
  MemSurface target(2, 2, k_fmt);
  const arbc::RenderRequest request{arbc::Rect::from_size(2.0, 2.0), 1.0, time, arbc::StateHandle{},
                                    target};
  const std::optional<arbc::RenderResult> result =
      content.render(request, std::make_shared<arbc::RenderCompletion>());
  REQUIRE(result.has_value());
  out_result = *result;
  return target.pixels();
}

} // namespace

// enforces: 03-layer-plugin-interface#render-time-honest
TEST_CASE(
    "Timed content renders a deterministic function of request time and reports achieved_time") {
  TimedContent content;

  // t = 0.31 s in flicks (doc 11:111-112 worked example): it falls in native
  // frame 7, so the content renders 7/24 s and reports that as achieved_time.
  const arbc::Time asked{218'736'000};                // 0.31 s
  const arbc::Time frame7{7 * TimedContent::k_frame}; // 7/24 s
  REQUIRE(frame7.flicks == 205'800'000);

  // (a) Reports the quantized local time it actually rendered.
  arbc::RenderResult r_asked{};
  const std::vector<float> px_asked = render_at(content, asked, r_asked);
  REQUIRE(r_asked.achieved_time == std::optional<arbc::Time>{frame7});

  // (b) Deterministic in request time: an identical request yields identical
  // pixels and the identical achieved_time.
  arbc::RenderResult r_again{};
  const std::vector<float> px_again = render_at(content, asked, r_again);
  REQUIRE(px_again == px_asked);
  REQUIRE(r_again.achieved_time == r_asked.achieved_time);

  // (c) A different request time WITHIN the same native frame quantizes to the
  // same achieved_time and renders byte-identical pixels -- the entry the
  // compositor serves across `[7/24, 8/24)` is a function of achieved_time.
  const arbc::Time same_frame{210'000'000}; // still in frame 7 ([205.8M, 235.2M))
  arbc::RenderResult r_same{};
  const std::vector<float> px_same = render_at(content, same_frame, r_same);
  REQUIRE(r_same.achieved_time == std::optional<arbc::Time>{frame7});
  REQUIRE(px_same == px_asked);

  // (d) A request time in the NEXT frame renders a different function value and
  // a different achieved_time -- time is a genuine input.
  const arbc::Time frame8{8 * TimedContent::k_frame};
  arbc::RenderResult r_next{};
  const std::vector<float> px_next = render_at(content, frame8, r_next);
  REQUIRE(r_next.achieved_time == std::optional<arbc::Time>{frame8});
  REQUIRE(px_next != px_asked);
}

// enforces: 03-layer-plugin-interface#static-time-invariant
TEST_CASE("Static content ignores request time and reports nullopt achieved_time and time_extent") {
  StaticContent content;

  // (a) Time-invariant output: two different request times render byte-identical
  // pixels.
  arbc::RenderResult r_zero{};
  const std::vector<float> px_zero = render_at(content, arbc::Time::zero(), r_zero);
  arbc::RenderResult r_late{};
  const std::vector<float> px_late = render_at(content, arbc::Time{999'999'999}, r_late);
  REQUIRE(px_late == px_zero);

  // (b) Reports no achieved_time regardless of the requested time (the parallel
  // of `achieved_scale == request.scale`), so it adds no time dimension to the
  // tile-cache key (doc 11:138-143).
  REQUIRE(r_zero.achieved_time == std::nullopt);
  REQUIRE(r_late.achieved_time == std::nullopt);

  // (c) Declares no temporal extent: static content varies over no local-time
  // range.
  REQUIRE(content.time_extent() == std::nullopt);
}

TEST_CASE("Timed content declares its half-open temporal extent") {
  TimedContent content;
  const std::optional<arbc::TimeRange> extent = content.time_extent();
  REQUIRE(extent.has_value());
  REQUIRE(extent->start == arbc::Time::zero());
  REQUIRE(extent->end == arbc::Time{TimedContent::k_frame * TimedContent::k_frames});
}

TEST_CASE("TimeRange is a half-open interval with inclusive start and exclusive end - emptiness "
          "membership equality") {
  const arbc::TimeRange r{arbc::Time{10}, arbc::Time{20}};

  // Half-open membership: start is included, end is excluded.
  REQUIRE_FALSE(r.empty());
  REQUIRE(r.contains(arbc::Time{10}));       // lower bound inclusive
  REQUIRE(r.contains(arbc::Time{15}));       // interior
  REQUIRE_FALSE(r.contains(arbc::Time{20})); // upper bound EXCLUSIVE
  REQUIRE_FALSE(r.contains(arbc::Time{9}));  // below
  REQUIRE_FALSE(r.contains(arbc::Time{21})); // above

  // Emptiness: a degenerate (end == start) and an inverted (end < start) range
  // are both empty and contain no instant.
  const arbc::TimeRange degenerate{arbc::Time{5}, arbc::Time{5}};
  const arbc::TimeRange inverted{arbc::Time{7}, arbc::Time{3}};
  REQUIRE(degenerate.empty());
  REQUIRE_FALSE(degenerate.contains(arbc::Time{5}));
  REQUIRE(inverted.empty());
  REQUIRE_FALSE(inverted.contains(arbc::Time{5}));

  // Value equality is member-wise: two ranges are equal iff both endpoints are.
  REQUIRE(r == arbc::TimeRange{arbc::Time{10}, arbc::Time{20}});
  REQUIRE(r != arbc::TimeRange{arbc::Time{10}, arbc::Time{21}});
  REQUIRE(r != arbc::TimeRange{arbc::Time{11}, arbc::Time{20}});
}
