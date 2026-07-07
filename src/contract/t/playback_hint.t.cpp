#include <arbc/base/geometry.hpp>
#include <arbc/base/rational_time.hpp>
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

// Contract-level tests for the advisory playback-hint seam (doc 11:160-178): the
// `PlaybackHint` value and the null-defaulted `Content::playback_hint`. The hint
// is advisory -- it changes no pixels and no cache correctness -- so the default
// is a no-op and hint-ignoring content is byte-identical whether or not a hint is
// issued (the runtime drive test reinforces this with a behavioral counter). No
// golden applies: the seam issues advisories, not pixels.

namespace {

// A self-contained in-memory rgba32f `Surface` so `render()` output is
// byte-observable without linking a backend (mirrors temporal_fields.t.cpp).
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

// Hint-ignoring content: it renders a fixed pattern and counts its renders, and
// it deliberately does NOT override `playback_hint` -- so delivering a hint
// exercises the contract's default no-op.
class HintIgnoringContent : public arbc::Content {
public:
  std::optional<arbc::Rect> bounds() const override { return std::nullopt; }
  arbc::Stability stability() const override { return arbc::Stability::Static; }
  std::optional<arbc::TimeRange> time_extent() const override { return std::nullopt; }

  std::optional<arbc::RenderResult> render(const arbc::RenderRequest& request,
                                           std::shared_ptr<arbc::RenderCompletion>) override {
    ++renders;
    const std::span<float> px = request.target.span<arbc::PixelFormat::Rgba32fLinearPremul>();
    for (std::size_t i = 0; i < px.size(); ++i) {
      px[i] = 3.0F + static_cast<float>(i);
    }
    return arbc::RenderResult{};
  }

  int renders = 0;
};

std::vector<float> render_once(arbc::Content& content) {
  MemSurface target(2, 2, k_fmt);
  const arbc::RenderRequest request{arbc::Rect::from_size(2.0, 2.0), 1.0, arbc::Time::zero(),
                                    arbc::StateHandle{}, target};
  const std::optional<arbc::RenderResult> result =
      content.render(request, std::make_shared<arbc::RenderCompletion>());
  REQUIRE(result.has_value());
  return target.pixels();
}

} // namespace

// enforces: 11-time-and-video#playback-hint-is-advisory-no-op-by-default
TEST_CASE("default Content::playback_hint is an advisory no-op; content is byte-identical") {
  // The `PlaybackHint` value bundles the derived (direction, rate, horizon) triple.
  const arbc::PlaybackHint hint{+1, arbc::Rational(24000, 1001), arbc::Time{1234}};
  CHECK(hint.direction == +1);
  CHECK(hint.rate == arbc::Rational(24000, 1001));
  CHECK(hint.horizon == arbc::Time{1234});

  HintIgnoringContent content;
  const std::vector<float> before = render_once(content);

  // Deliver hints through the default no-op: neither may change any state.
  content.playback_hint(hint);
  content.playback_hint(arbc::PlaybackHint{-1, arbc::Rational(1, 2), arbc::Time{99}});
  content.playback_hint(arbc::PlaybackHint{}); // the empty (paused) hint

  const std::vector<float> after = render_once(content);

  // Byte-identical pixels: the advisory hint changed no rendered output.
  CHECK(after == before);
  // Exactly two renders ran -- the three hints issued zero extra renders.
  CHECK(content.renders == 2);
}

// enforces: 11-time-and-video#playback-hint-is-advisory-no-op-by-default
TEST_CASE("overriding content receives the PlaybackHint value through the Content vtable") {
  // A decoder-backed content overrides the seam to record the hint; the call
  // dispatches virtually through a `Content&`, proving the triple is delivered.
  class Recorder : public HintIgnoringContent {
  public:
    void playback_hint(const arbc::PlaybackHint& h) override {
      last = h;
      ++count;
    }
    arbc::PlaybackHint last{};
    int count = 0;
  } rec;

  arbc::Content& as_content = rec;
  as_content.playback_hint(arbc::PlaybackHint{-1, arbc::Rational(2, 1), arbc::Time{500}});

  REQUIRE(rec.count == 1);
  CHECK(rec.last.direction == -1);
  CHECK(rec.last.rate == arbc::Rational(2, 1));
  CHECK(rec.last.horizon == arbc::Time{500});
  CHECK(rec.renders == 0); // the advisory triggered no render
}
