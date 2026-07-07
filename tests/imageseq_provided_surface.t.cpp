// org.arbc.imageseq is the reference decoder for RenderResult.provided (doc 03
// "video decoder" case, doc 09). Asserts the decoded frame returned via
// `provided` is honored by the compositor's consume path and that its lifecycle
// is sound (released after consume -- a re-render is a cache hit with no
// re-decode, and the whole path runs clean under ASan/UBSan). Also pins the
// advisory-hint safety invariant (constraint §6): rendered pixels are
// byte-identical whether or not a playback_hint was issued.

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/cache/key_shapes.hpp>
#include <arbc/cache/keyed_store.hpp>
#include <arbc/compositor/compositor.hpp>
#include <arbc/compositor/tile_planning.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/media/pixel_format.hpp>
#include <arbc/model/model.hpp>
#include <arbc/surface/surface_pool.hpp>

#include "support/imageseq_fixtures.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <memory>
#include <vector>

using namespace arbc;
namespace fix = arbc::imageseq::testfix;

namespace {

Time instant(int frame) { return Time{frame * fix::k_period_flicks}; }

std::array<float, 4> device_pixel(const Surface& surface, int x, int y) {
  const std::span<const float> px = surface.span<PixelFormat::Rgba32fLinearPremul>();
  const std::size_t base = (static_cast<std::size_t>(y) * surface.width() + x) * 4U;
  return {px[base + 0], px[base + 1], px[base + 2], px[base + 3]};
}

std::vector<float> direct_frame_pixels(arbc::imageseq::ImageSeqContent& content, Backend& backend,
                                       Time t) {
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
  const std::optional<RenderResult> r = content.render(request, done);
  REQUIRE(r.has_value());
  REQUIRE(r->provided.has_value());
  const std::span<const float> span =
      r->provided->surface().span<PixelFormat::Rgba32fLinearPremul>();
  return {span.begin(), span.end()};
}

} // namespace

// enforces: 09-surfaces-and-backends#content-provided-surface-honored
// enforces: 09-surfaces-and-backends#provided-surface-released-after-consume
TEST_CASE("imageseq's provided frame is honored and released through the compositor") {
  CpuBackend backend;

  // The pixels the decoder provides for frame 1, sampled directly.
  auto probe = fix::make_content();
  const std::vector<float> frame1 = direct_frame_pixels(*probe, backend, instant(1));

  // Drive the same frame through the compositor's cache path.
  Model model;
  ObjectId content_id{};
  auto content = fix::make_content();
  {
    Model::Transaction txn = model.transact();
    content_id = txn.add_content(0);
    txn.add_layer(content_id, Affine::identity());
    REQUIRE(txn.commit().has_value());
  }
  const ContentResolver resolve = [&](ObjectId id) -> Content* {
    return id == content_id ? content.get() : nullptr;
  };
  const DocStatePtr state = model.current();
  const Viewport viewport{16, 16, Affine::identity()};
  SurfacePool pool(backend);
  TileCache cache(64u * 1024 * 1024);

  auto drive = [&]() {
    auto target = backend.make_surface(viewport.width, viewport.height, k_working_rgba32f);
    REQUIRE(target.has_value());
    render_frame_interactive(*state, resolve, viewport, cache, backend, pool, **target,
                             Deadline::none(), std::nullopt, nullptr, nullptr, nullptr, instant(1));
    return std::move(*target);
  };

  // Honored: with an identity camera the 2x2 provided frame lands 1:1 at the
  // device top-left; the composited pixels there are the decoder's own pixels
  // (premultiplied over transparent == the frame), not the transparent target.
  const std::unique_ptr<Surface> out = drive();
  for (int y = 0; y < fix::k_height; ++y) {
    for (int x = 0; x < fix::k_width; ++x) {
      const std::array<float, 4> got = device_pixel(*out, x, y);
      const std::size_t i = (static_cast<std::size_t>(y) * fix::k_width + x) * 4U;
      REQUIRE(got[0] == frame1[i + 0]);
      REQUIRE(got[1] == frame1[i + 1]);
      REQUIRE(got[2] == frame1[i + 2]);
      REQUIRE(got[3] == frame1[i + 3]);
    }
  }
  REQUIRE(content->decodes_issued() == 1);

  // Released after consume: a second frame at the same instant is served from
  // the coalesced cache entry (a hit) with no re-decode -- the provided surface
  // was consumed and released without forcing the decoder to re-run, and the
  // whole consume/release path is memory-clean (ASan/UBSan lane).
  const std::uint64_t hits_before = cache.hits();
  drive();
  CHECK(cache.hits() > hits_before);
  CHECK(content->decodes_issued() == 1);
}

TEST_CASE("imageseq renders byte-identically with and without a playback hint (constraint 6)") {
  CpuBackend backend;

  auto no_hint = fix::make_content();
  const std::vector<float> baseline = direct_frame_pixels(*no_hint, backend, instant(2));

  auto with_hint = fix::make_content();
  // Prime the pre-roll anchor, then issue a forward hint -- pre-roll warms the
  // decoded-frame cache but must not change any rendered pixel.
  direct_frame_pixels(*with_hint, backend, instant(0));
  with_hint->playback_hint(PlaybackHint{+1, Rational{1, 1}, Time{3 * fix::k_period_flicks}});
  const std::vector<float> hinted = direct_frame_pixels(*with_hint, backend, instant(2));

  REQUIRE(hinted == baseline);
}
