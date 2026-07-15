// org.arbc.image is the reference kind for doc 09:157-160 -- the half of the
// content-provided-surface contract that imageseq does not exercise.
//
// `ImageSeqContent::render` hands back the WHOLE decoded frame and calls it a day. That is
// harmless for a 2x2 fixture and catastrophic for a photograph: the compositor tiles, so each
// of the ~100+ tile pulls over a 24 MP image would hand over a full-frame surface that the
// cache then COPIES -- a ~384 MB copy per cache insert at rgba32f. Doc 09:157-160 already
// says the right thing ("covering the requested region at the achieved scale"), and
// kinds/image.md Decision 1 takes it: `render()` returns a surface sized to the REQUEST, not
// to the image. That bounds every cache copy to one tile.
//
// The fixture is 384x320 -- materially larger than the 256-px tile -- so "the provided
// surface is not the whole frame" is a distinction the assertions can actually see.

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/cache/keyed_store.hpp>
#include <arbc/compositor/compositor.hpp>
#include <arbc/compositor/tile_planning.hpp> // k_tile_size
#include <arbc/contract/content.hpp>
#include <arbc/kind_image/image_content.hpp>
#include <arbc/media/pixel_format.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/model/model.hpp>
#include <arbc/surface/surface_pool.hpp>

#include <catch2/catch_test_macros.hpp>

#include "support/image_fixtures.hpp"

#include <array>
#include <memory>
#include <vector>

using namespace arbc;
namespace fix = arbc::image::testfix;

namespace {

std::array<float, 4> device_pixel(const Surface& surface, int x, int y) {
  const std::span<const float> px = surface.span<PixelFormat::Rgba32fLinearPremul>();
  const std::size_t base = (static_cast<std::size_t>(y) * surface.width() + x) * 4U;
  return {px[base + 0], px[base + 1], px[base + 2], px[base + 3]};
}

} // namespace

// enforces: 09-surfaces-and-backends#image-provided-surface-covers-requested-region
TEST_CASE("org.arbc.image provides the requested region at the achieved scale, never the frame") {
  CpuBackend backend;
  auto content = fix::make_content();
  REQUIRE(content->bounds() == Rect{0.0, 0.0, fix::k_width, fix::k_height});

  // Several rungs, and a NON-ORIGIN region at each -- the region offset is exactly what a
  // whole-frame handoff would silently ignore.
  struct Case {
    double scale;
    Rect region;
    int width;
    int height;
  };
  const std::array<Case, 3> cases{{
      // A full tile-sized pull at native scale, offset off the origin.
      {1.0, Rect{128.0, 64.0, 128.0 + k_tile_size, 64.0 + k_tile_size}, k_tile_size, k_tile_size},
      // Half scale: the same tile of DEVICE pixels covers twice the local extent.
      {0.5, Rect{64.0, 32.0, 64.0 + 2 * k_tile_size, 32.0 + 2 * k_tile_size}, k_tile_size,
       k_tile_size},
      // A small off-origin patch: the surface tracks the REQUEST, not a tile and not the image.
      {0.25, Rect{200.0, 160.0, 328.0, 288.0}, 32, 32},
  }};

  for (const Case& c : cases) {
    auto target = backend.make_surface(c.width, c.height, k_working_rgba32f);
    REQUIRE(target.has_value());
    auto done = std::make_shared<RenderCompletion>();
    const RenderRequest request{c.region, c.scale,          Time::zero(),    StateHandle{},
                                **target, Exactness::Exact, Deadline::none()};
    const std::optional<RenderResult> r = content->render(request, done);
    REQUIRE(r.has_value());
    REQUIRE(r->provided.has_value());

    const Surface& provided = r->provided->surface();
    // THE CLAIM: the provided surface's extent is the requested region at the achieved
    // scale -- i.e. the target's extent -- so a cache insert copies one tile, not one image.
    CHECK(provided.width() == c.width);
    CHECK(provided.height() == c.height);
    // ...and it is emphatically NOT the whole decoded frame, which is what the fixture being
    // materially larger than a tile lets us say out loud.
    CHECK(provided.width() < fix::k_width);
    CHECK(provided.height() < fix::k_height);

    // Non-transient (doc 09:176-182): the compositor may cache from it, not merely consume it
    // within the frame. And it carries the composition working-space tag (doc 09:219-230).
    CHECK_FALSE(r->provided->transient());
    CHECK(provided.format() == k_working_rgba32f);
    CHECK(r->achieved_scale == c.scale);
  }
}

TEST_CASE("org.arbc.image recycles its provided surfaces through the release callback") {
  CpuBackend backend;
  auto content = fix::make_content();
  auto target = backend.make_surface(64, 64, k_working_rgba32f);
  REQUIRE(target.has_value());

  const auto pull = [&]() {
    auto done = std::make_shared<RenderCompletion>();
    const RenderRequest request{Rect{0.0, 0.0, 64.0, 64.0},
                                1.0,
                                Time::zero(),
                                StateHandle{},
                                **target,
                                Exactness::Exact,
                                Deadline::none()};
    const std::optional<RenderResult> r = content->render(request, done);
    REQUIRE(r.has_value());
    REQUIRE(r->provided.has_value());
    // `r` dies here: the last SurfaceRef drops, firing the release callback, which returns
    // the surface to the plugin-owned free list (Decision 1).
  };

  pull();
  CHECK(content->tile_allocations() == 1);
  pull();
  pull();
  // Same extent, so the free list serves every subsequent pull: no further allocation. That
  // is the release callback firing, observed as a counter rather than as a timing.
  CHECK(content->tile_allocations() == 1);
}

// enforces: 09-surfaces-and-backends#content-provided-surface-honored
// enforces: 09-surfaces-and-backends#provided-surface-released-after-consume
TEST_CASE("org.arbc.image's provided surface is honored and released through the compositor") {
  CpuBackend backend;

  // The pixels the kind provides for the top-left of the image, sampled directly.
  auto probe = fix::make_content();
  auto probe_target = backend.make_surface(16, 16, k_working_rgba32f);
  REQUIRE(probe_target.has_value());
  std::vector<float> expected;
  {
    auto done = std::make_shared<RenderCompletion>();
    const RenderRequest request{Rect{0.0, 0.0, 16.0, 16.0},
                                1.0,
                                Time::zero(),
                                StateHandle{},
                                **probe_target,
                                Exactness::Exact,
                                Deadline::none()};
    const std::optional<RenderResult> r = probe->render(request, done);
    REQUIRE(r.has_value());
    REQUIRE(r->provided.has_value());
    const std::span<const float> span =
        r->provided->surface().span<PixelFormat::Rgba32fLinearPremul>();
    expected.assign(span.begin(), span.end());
  }

  // Drive the same content through the compositor's tile/cache path.
  Model model;
  ObjectId content_id{};
  ObjectId comp_id{};
  auto content = fix::make_content();
  {
    Model::Transaction txn = model.transact();
    content_id = txn.add_content(0);
    const ObjectId layer = txn.add_layer(content_id, Affine::identity());
    // Attach to a composition so the composition-scoped walk draws it (doc 05:28-36).
    comp_id = txn.add_composition(16.0, 16.0);
    txn.attach_layer(comp_id, layer);
    REQUIRE(txn.commit().has_value());
  }
  const ContentResolver resolve = [&](ObjectId id) -> Content* {
    return id == content_id ? content.get() : nullptr;
  };
  const DocStatePtr state = model.current();
  const Viewport viewport{16, 16, Affine::identity(), comp_id};
  SurfacePool pool(backend);
  TileCache cache(64u * 1024 * 1024);

  const auto drive = [&]() {
    auto out = backend.make_surface(viewport.width, viewport.height, k_working_rgba32f);
    REQUIRE(out.has_value());
    render_frame_interactive(*state, resolve, viewport, cache, backend, pool, **out,
                             Deadline::none(), std::nullopt, nullptr, nullptr, nullptr,
                             Time::zero());
    return std::move(*out);
  };

  // HONORED: with an identity camera the image lands 1:1 at the device origin, and the
  // composited pixels there are the CONTENT'S OWN (premultiplied over a transparent target
  // == the provided pixels), not the untouched target the content never filled.
  const std::unique_ptr<Surface> out = drive();
  for (int y = 0; y < 16; ++y) {
    for (int x = 0; x < 16; ++x) {
      const std::array<float, 4> got = device_pixel(*out, x, y);
      const std::size_t i = (static_cast<std::size_t>(y) * 16U + static_cast<std::size_t>(x)) * 4U;
      REQUIRE(got[0] == expected[i + 0]);
      REQUIRE(got[1] == expected[i + 1]);
      REQUIRE(got[2] == expected[i + 2]);
      REQUIRE(got[3] == expected[i + 3]);
    }
  }

  // RELEASED AFTER CONSUME: the second frame is a cache hit, so the provided surface was
  // copied into cache and its reference dropped -- and the whole consume/release path runs
  // clean under ASan/UBSan. `Static` content reports no achieved_time, so an identical frame
  // re-pulls the very same cache entry.
  const std::uint64_t hits_before = cache.hits();
  drive();
  CHECK(cache.hits() > hits_before);
}

TEST_CASE("an unavailable org.arbc.image has empty bounds and renders nothing") {
  CpuBackend backend;
  // Decision 7 and the deviation it records: doc 08:130-131 says an unavailable reference
  // "renders the placeholder", but a placeholder needs an EXTENT to draw over, and this kind
  // has none -- its intrinsic size is knowable only by decoding, and Constraint 4 forbids
  // caching it in the document. Empty bounds is the honest reading; fabricating a rectangle
  // would let a MISSING file change the composition's geometry.
  auto content = fix::make_unavailable_content();
  REQUIRE_FALSE(content->available());

  const std::optional<Rect> bounds = content->bounds();
  REQUIRE(bounds.has_value()); // not nullopt -- nullopt would mean UNBOUNDED
  CHECK(bounds->empty());

  // The authored reference survives intact, which is what lets the layer re-save
  // byte-identically and reappear in full when the file returns.
  CHECK(content->external_asset_ref() == "assets/missing.png");

  auto target = backend.make_surface(16, 16, k_working_rgba32f);
  REQUIRE(target.has_value());
  auto done = std::make_shared<RenderCompletion>();
  const RenderRequest request{Rect{0.0, 0.0, 16.0, 16.0},
                              1.0,
                              Time::zero(),
                              StateHandle{},
                              **target,
                              Exactness::Exact,
                              Deadline::none()};
  // No pixels, reported as a VALUE -- never UB, never a throw (Constraint 7).
  CHECK(content->render(request, done) == std::nullopt);
  REQUIRE(done->settled());
  CHECK_FALSE(done->settled_ok());
}
