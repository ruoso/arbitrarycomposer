// Byte-exact rendering goldens for org.arbc.image (doc 16 "byte-exact goldens"; the
// tolerance is the justified exception, and none is used here).
//
// COMPUTED-REFERENCE, IN-TU -- the imageseq precedent. The only checked-in binary is the
// fixture itself (`plugins/image/t/fixtures/photo.ppm`); every expected pixel is derived in
// this TU from those bytes by an INDEPENDENT path: `support/image_fixtures.hpp` decodes the
// P6 with its own reader, converts to the working space, and builds the reference rung with
// `media`'s frozen half-band bank. The kind's own pyramid must reproduce that bit-for-bit.
//
// Four cases, and the last two are the honesty pair (`raster_content.cpp:508-514`):
//   * native scale        -- achieved == requested, exact == true;
//   * a downscale rung    -- level 1 of the pyramid, pinning the `decimate_half_band` path;
//   * an upscale, Exact       -- bicubic magnify past native, achieved == requested;
//   * an upscale, BestEffort  -- achieved_scale CLAMPS AT NATIVE and exact == false.

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/time.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/kind_image/image_content.hpp>
#include <arbc/media/image_resampler.hpp>
#include <arbc/media/pixel_format.hpp>
#include <arbc/media/surface_format.hpp>

#include <catch2/catch_test_macros.hpp>

#include "support/image_fixtures.hpp"

#include <cmath>
#include <cstddef>
#include <memory>
#include <vector>

using namespace arbc;
namespace fix = arbc::image::testfix;

namespace {

// Render `region` at `scale` into a `w` x `h` target and return the pixels the content
// PROVIDED (never the target: this kind always answers with its own surface, Decision 1).
struct Rendered {
  std::vector<float> px;
  double achieved_scale{0.0};
  bool exact{false};
  int width{0};
  int height{0};
};

Rendered render_region(arbc::image::ImageContent& content, Backend& backend, const Rect& region,
                       double scale, int w, int h, Exactness exactness) {
  auto target = backend.make_surface(w, h, k_working_rgba32f);
  REQUIRE(target.has_value());
  auto done = std::make_shared<RenderCompletion>();
  const RenderRequest request{region,   scale,     Time::zero(),    StateHandle{},
                              **target, exactness, Deadline::none()};
  const std::optional<RenderResult> r = content.render(request, done);
  REQUIRE(r.has_value());
  REQUIRE(r->provided.has_value());
  // Static content reports no achieved_time: it adds no time dimension to the cache key.
  REQUIRE(r->achieved_time == std::nullopt);
  const Surface& src = r->provided->surface();
  REQUIRE(src.format() == k_working_rgba32f); // the composition working-space tag (doc 09:219-230)
  const std::span<const float> span = src.span<PixelFormat::Rgba32fLinearPremul>();
  return Rendered{std::vector<float>(span.begin(), span.end()), r->achieved_scale, r->exact,
                  src.width(), src.height()};
}

// The reference resample, driven by this TU's own sampling geometry (the same geometry
// `render` derives, written out independently here rather than shared with it).
std::vector<float> reference_resample(const fix::RefImage& level, const Rect& region,
                                      double achieved, double level_scale, int w, int h) {
  std::vector<float> out(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4U);
  for (int dy = 0; dy < h; ++dy) {
    for (int dx = 0; dx < w; ++dx) {
      const double lx = region.x0 + (static_cast<double>(dx) + 0.5) / achieved;
      const double ly = region.y0 + (static_cast<double>(dy) + 0.5) / achieved;
      WorkingPixel sample{0.0F, 0.0F, 0.0F, 0.0F};
      // Outside the master's bounds reads transparent -- the render's own guard, in the
      // MASTER's coordinates regardless of which rung answers.
      if (lx >= 0.0 && lx < static_cast<double>(fix::k_width) && ly >= 0.0 &&
          ly < static_cast<double>(fix::k_height)) {
        const double u = lx / level_scale - 0.5;
        const double v = ly / level_scale - 0.5;
        const int x0 = static_cast<int>(std::floor(u));
        const int y0 = static_cast<int>(std::floor(v));
        const auto fx = static_cast<float>(u - static_cast<double>(x0));
        const auto fy = static_cast<float>(v - static_cast<double>(y0));
        sample = sample_bicubic(x0, y0, fx, fy, [&](int sx, int sy) { return level.at(sx, sy); });
      }
      const std::size_t o = (static_cast<std::size_t>(dy) * static_cast<std::size_t>(w) +
                             static_cast<std::size_t>(dx)) *
                            4U;
      out[o] = sample[0];
      out[o + 1] = sample[1];
      out[o + 2] = sample[2];
      out[o + 3] = sample[3];
    }
  }
  return out;
}

} // namespace

// enforces: 16-sdlc-and-quality#byte-exact-goldens
TEST_CASE("org.arbc.image renders byte-exact against the computed reference at native scale") {
  CpuBackend backend;
  auto content = fix::make_content();
  const fix::RefImage master = fix::reference_master();
  REQUIRE(master.width == fix::k_width);
  REQUIRE(master.height == fix::k_height);
  REQUIRE(content->bounds() == Rect{0.0, 0.0, fix::k_width, fix::k_height});

  // A non-origin region, so the golden pins the region offset and not just the origin tile.
  const Rect region{64.0, 96.0, 80.0, 108.0};
  const Rendered got = render_region(*content, backend, region, 1.0, 16, 12, Exactness::Exact);

  CHECK(got.achieved_scale == 1.0);
  CHECK(got.exact);
  // At native scale the phase is integral and Catmull-Rom is interpolating -- weights are
  // exactly (0, 1, 0, 0) -- so the render reproduces the master's pixels BIT-FOR-BIT.
  const std::vector<float> want = reference_resample(master, region, 1.0, 1.0, 16, 12);
  CHECK(got.px == want);
}

// enforces: 16-sdlc-and-quality#byte-exact-goldens
TEST_CASE("org.arbc.image renders byte-exact on the level-1 downscale rung") {
  CpuBackend backend;
  auto content = fix::make_content();

  // Level 1 of the reference pyramid: ONE half-band decimation of the master through media's
  // frozen bank. This is the golden that pins the `decimate_half_band` path -- the kind's own
  // pyramid must equal a direct decimation of the decoded master.
  const fix::RefImage level1 = fix::decimate(fix::reference_master());
  REQUIRE(level1.width == (fix::k_width + 1) / 2);
  REQUIRE(level1.height == (fix::k_height + 1) / 2);

  const Rect region{64.0, 96.0, 96.0, 120.0};
  const Rendered got = render_region(*content, backend, region, 0.5, 16, 12, Exactness::Exact);

  CHECK(got.achieved_scale == 0.5);
  CHECK(got.exact); // an Exact request at 0.5 IS honored exactly: the rung exists
  const std::vector<float> want = reference_resample(level1, region, 0.5, 2.0, 16, 12);
  CHECK(got.px == want);
}

// enforces: 16-sdlc-and-quality#byte-exact-goldens
// enforces: 03-layer-plugin-interface#render-scale-honest
TEST_CASE(
    "org.arbc.image magnifies past native on an Exact request and says achieved == requested") {
  CpuBackend backend;
  auto content = fix::make_content();
  const fix::RefImage master = fix::reference_master();

  const Rect region{64.0, 96.0, 72.0, 102.0};
  const Rendered got = render_region(*content, backend, region, 2.0, 16, 12, Exactness::Exact);

  // An OFFLINE (Exact) render must be faithful and may take unbounded time: it magnifies
  // past native with the Catmull-Rom tap rather than degrading, and reports the scale it was
  // asked for (`raster_content.cpp:508-512`).
  CHECK(got.achieved_scale == 2.0);
  CHECK(got.exact);
  const std::vector<float> want = reference_resample(master, region, 2.0, 1.0, 16, 12);
  CHECK(got.px == want);
}

// enforces: 16-sdlc-and-quality#byte-exact-goldens
// enforces: 03-layer-plugin-interface#render-scale-honest
TEST_CASE("org.arbc.image clamps a BestEffort upscale at native and reports it honestly") {
  CpuBackend backend;
  auto content = fix::make_content();
  const fix::RefImage master = fix::reference_master();

  const Rect region{64.0, 96.0, 80.0, 108.0};
  const Rendered got = render_region(*content, backend, region, 2.0, 16, 12, Exactness::BestEffort);

  // The honesty rule (`raster_content.cpp:512-514`): an INTERACTIVE request may degrade, so
  // it clamps at native -- and `achieved < requested` is NEVER `exact`. A content that
  // clamped silently while claiming exactness would let the compositor cache a degraded tile
  // as if it were faithful.
  CHECK(got.achieved_scale == 1.0);
  CHECK(got.achieved_scale < 2.0);
  CHECK_FALSE(got.exact);
  const std::vector<float> want = reference_resample(master, region, 1.0, 1.0, 16, 12);
  CHECK(got.px == want);
}
