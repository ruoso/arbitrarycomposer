#include <arbc/base/geometry.hpp>
#include <arbc/base/time.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/model/records.hpp>
#include <arbc/surface/surface.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace {

// A self-contained in-memory `Surface` with real rgba32f storage, so
// `render()` output is byte-observable via `cpu_pixels()` without linking a
// backend. Keeps the contract unit test at L3.
class MemSurface : public arbc::Surface {
public:
  MemSurface(int w, int h, arbc::SurfaceFormat fmt)
      : d_w(w), d_h(h), d_fmt(fmt),
        d_pixels(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4U, 0.0F) {}

  int width() const override { return d_w; }
  int height() const override { return d_h; }
  arbc::SurfaceFormat format() const override { return d_fmt; }
  std::span<float> cpu_pixels() override { return d_pixels; }
  std::span<const float> cpu_pixels() const override { return d_pixels; }

  const std::vector<float>& pixels() const { return d_pixels; }

private:
  int d_w;
  int d_h;
  arbc::SurfaceFormat d_fmt;
  std::vector<float> d_pixels;
};

// A deterministic `Content` whose `render()` writes each target pixel as a
// pure function of `(request.snapshot.slot, region, scale, time)` -- the exact
// obligation the render contract states (doc 03:138-140, doc 14:181-187). No
// hidden state: the same request always yields the same bytes, and the
// `snapshot` handle is a genuine input (it seeds the pixels).
class DeterministicContent : public arbc::Content {
public:
  std::optional<arbc::Rect> bounds() const override { return std::nullopt; }
  arbc::Stability stability() const override { return arbc::Stability::Static; }

  std::optional<arbc::RenderResult> render(const arbc::RenderRequest& request,
                                           std::shared_ptr<arbc::RenderCompletion>) override {
    const std::span<float> px = request.target.cpu_pixels();
    const float seed = static_cast<float>(request.snapshot.slot) +
                       static_cast<float>(request.region.x0) +
                       static_cast<float>(request.region.y0) + static_cast<float>(request.scale) +
                       static_cast<float>(request.time.flicks);
    for (std::size_t i = 0; i < px.size(); ++i) {
      px[i] = seed + static_cast<float>(i);
    }
    return arbc::RenderResult{};
  }
};

// A recording `Content` that copies the `snapshot` it received, so a test can
// witness that the pinned handle reaches `render()` unchanged.
class RecordingContent : public arbc::Content {
public:
  std::optional<arbc::Rect> bounds() const override { return std::nullopt; }
  arbc::Stability stability() const override { return arbc::Stability::Static; }

  std::optional<arbc::RenderResult> render(const arbc::RenderRequest& request,
                                           std::shared_ptr<arbc::RenderCompletion>) override {
    d_received = request.snapshot;
    return arbc::RenderResult{};
  }

  arbc::StateHandle received() const { return d_received; }

private:
  arbc::StateHandle d_received{};
};

constexpr arbc::SurfaceFormat k_fmt = arbc::k_working_rgba32f;

// Render `content` against a fresh 2x2 target with the given pinned handle and
// return the resulting target pixels by value.
std::vector<float> render_pixels(arbc::Content& content, arbc::StateHandle snapshot) {
  MemSurface target(2, 2, k_fmt);
  const arbc::Rect region = arbc::Rect::from_size(2.0, 2.0);
  const arbc::RenderRequest request{region, 1.0, arbc::Time::zero(), snapshot, target};
  content.render(request, std::make_shared<arbc::RenderCompletion>());
  return target.pixels();
}

} // namespace

// enforces: 03-layer-plugin-interface#render-pure-over-pinned-state
TEST_CASE("render is a pure function of the pinned snapshot: identical requests match, "
          "differing-only-in-snapshot requests differ") {
  DeterministicContent content;

  // (a) Purity: the same request rendered twice yields byte-identical pixels.
  const std::vector<float> first = render_pixels(content, arbc::StateHandle{7U});
  const std::vector<float> second = render_pixels(content, arbc::StateHandle{7U});
  REQUIRE(first == second);

  // (b) The snapshot is a genuine input: two requests differing ONLY in
  // `snapshot.slot` yield different pixels.
  const std::vector<float> other = render_pixels(content, arbc::StateHandle{8U});
  REQUIRE(first != other);
}

TEST_CASE("the pinned snapshot handle reaches render() unchanged") {
  RecordingContent content;
  const arbc::StateHandle pinned{42U};
  render_pixels(content, pinned);
  REQUIRE(content.received() == pinned);
}

TEST_CASE("RenderRequest defaults its snapshot to the no-state sentinel") {
  MemSurface target(1, 1, k_fmt);
  const arbc::RenderRequest request{arbc::Rect::from_size(1.0, 1.0), 1.0, arbc::Time::zero(),
                                    arbc::StateHandle{}, target};
  REQUIRE_FALSE(request.snapshot.has_state());
  REQUIRE(request.snapshot == arbc::StateHandle{arbc::k_state_none});
}
