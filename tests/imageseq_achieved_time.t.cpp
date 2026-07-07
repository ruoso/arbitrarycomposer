// The reference-kind proof of the doc-11 achieved_time contract (imageseq_plugin
// §acceptance). Content-local: drives ImageSeqContent::render directly and reads
// the decoded pixels back out of RenderResult.provided (imageseq never fills the
// target). Byte-exact comparisons are computed-reference goldens (doc 16): the
// determinism they assert leans on the landed sRGB8 round-trip / kernel
// byte-exactness guarantees (doc 07), so no frozen byte table is needed.

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/time.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/media/pixel_format.hpp>
#include <arbc/media/pixel_traits.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/surface/surface.hpp>

#include "support/imageseq_fixtures.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <vector>

using namespace arbc;
using arbc::imageseq::ImageSeqContent;
namespace fix = arbc::imageseq::testfix;

namespace {

RenderResult render_at(ImageSeqContent& content, Backend& backend, Time t) {
  auto target = backend.make_surface(fix::k_width, fix::k_height, k_working_rgba32f);
  REQUIRE(target.has_value());
  auto done = std::make_shared<RenderCompletion>();
  const RenderRequest request{Rect{0.0, 0.0, fix::k_width, fix::k_height},
                              1.0,
                              t,
                              StateHandle{},
                              **target,
                              Exactness::Exact,
                              Deadline::none()};
  const std::optional<RenderResult> result = content.render(request, done);
  REQUIRE(result.has_value()); // imageseq settles a valid frame inline
  return *result;
}

std::vector<float> provided_pixels(const RenderResult& result) {
  REQUIRE(result.provided.has_value());
  const std::span<const float> span =
      result.provided->surface().span<PixelFormat::Rgba32fLinearPremul>();
  REQUIRE(!span.empty());
  return {span.begin(), span.end()};
}

Time instant(int frame) { return Time{frame * fix::k_period_flicks}; }

} // namespace

// enforces: 11-time-and-video#imageseq-achieved-time-is-nearest-source-frame
// enforces: 11-time-and-video#quantize-time-matches-achieved-time
TEST_CASE("imageseq achieved_time is the nearest native source-frame instant") {
  CpuBackend backend;
  auto content = fix::make_content();

  SECTION("quantize_time floors to the native grid and is clamped, half-open") {
    // A sub-frame time inside frame 1's interval floors to frame 1's instant.
    REQUIRE(content->quantize_time(Time{fix::k_period_flicks + 5}) == instant(1));
    // Half-open: the instant one flick before a boundary is still the prior frame.
    REQUIRE(content->quantize_time(Time{fix::k_period_flicks - 1}) == instant(0));
    REQUIRE(content->quantize_time(instant(1)) == instant(1));
    // A time past the clip clamps to the last frame; a negative time clamps to 0.
    REQUIRE(content->quantize_time(Time{100 * fix::k_period_flicks}) ==
            instant(fix::k_frame_count - 1));
    REQUIRE(content->quantize_time(Time{-1000}) == instant(0));
  }

  SECTION("quantize_time is idempotent") {
    for (std::int64_t t : {std::int64_t{0}, std::int64_t{5}, fix::k_period_flicks + 7,
                           3 * fix::k_period_flicks - 1, 100 * fix::k_period_flicks}) {
      const std::optional<Time> q = content->quantize_time(Time{t});
      REQUIRE(q.has_value());
      REQUIRE(content->quantize_time(*q) == q);
    }
  }

  SECTION("render(time=t).achieved_time equals quantize_time(t)") {
    for (std::int64_t t : {std::int64_t{5}, fix::k_period_flicks + 12,
                           2 * fix::k_period_flicks + 3, 100 * fix::k_period_flicks}) {
      const RenderResult r = render_at(*content, backend, Time{t});
      REQUIRE(r.achieved_time == content->quantize_time(Time{t}));
    }
  }

  SECTION("two distinct sub-frame times in one interval render byte-identical pixels") {
    const RenderResult on_grid = render_at(*content, backend, instant(1));
    const RenderResult sub_a = render_at(*content, backend, Time{fix::k_period_flicks + 7});
    const RenderResult sub_b = render_at(*content, backend, Time{2 * fix::k_period_flicks - 3});
    const std::vector<float> px = provided_pixels(on_grid);
    REQUIRE(provided_pixels(sub_a) == px);
    REQUIRE(provided_pixels(sub_b) == px);
    // Sanity: a different native frame is genuinely different pixels (temporal
    // variation is real, not a constant).
    REQUIRE(provided_pixels(render_at(*content, backend, instant(0))) != px);
  }
}

// enforces: 16-sdlc-and-quality#byte-exact-goldens
TEST_CASE("imageseq renders each native frame byte-identically forward and reverse") {
  CpuBackend backend;
  auto content = fix::make_content();

  // (a) On-grid instants: each native frame is a distinct, byte-exact image.
  std::vector<std::vector<float>> forward;
  for (int f = 0; f < fix::k_frame_count; ++f) {
    forward.push_back(provided_pixels(render_at(*content, backend, instant(f))));
  }
  for (int f = 1; f < fix::k_frame_count; ++f) {
    REQUIRE(forward[static_cast<std::size_t>(f)] != forward[static_cast<std::size_t>(f - 1)]);
  }

  // (c) Reverse-rate playback: driving the on-grid instants in DESCENDING order
  // reproduces each frame's pixels byte-for-byte -- reverse playback is not a
  // different image, and the stateful decoder is order-independent (determinism
  // is owned by quantize_time/achieved_time, not by playback direction).
  auto reverse_content = fix::make_content();
  for (int f = fix::k_frame_count - 1; f >= 0; --f) {
    const RenderResult r = render_at(*reverse_content, backend, instant(f));
    REQUIRE(r.achieved_time == instant(f));
    REQUIRE(provided_pixels(r) == forward[static_cast<std::size_t>(f)]);
  }
}

// enforces: 16-sdlc-and-quality#byte-exact-goldens
TEST_CASE("imageseq decodes each native frame at most once across sub-frame times") {
  CpuBackend backend;
  auto content = fix::make_content();
  REQUIRE(content->decodes_issued() == 0);

  // Five distinct sub-frame times inside frame 2's interval issue exactly one
  // decode (achieved-time coalescing at the content's decoded-frame cache).
  const std::int64_t base = 2 * fix::k_period_flicks;
  for (std::int64_t offset : {std::int64_t{0}, std::int64_t{1}, std::int64_t{100},
                              std::int64_t{9000}, fix::k_period_flicks - 1}) {
    render_at(*content, backend, Time{base + offset});
  }
  REQUIRE(content->decodes_issued() == 1);

  // Crossing into frame 3 issues exactly one more decode.
  render_at(*content, backend, Time{3 * fix::k_period_flicks});
  REQUIRE(content->decodes_issued() == 2);
}
