// org.arbc.imageseq through the compositor: the reference Timed content
// end-to-end re-assertion of achieved-time coalescing (doc 11:138-159). Drives a
// real imageseq layer through render_frame_interactive at fixed composition
// times against a live TileCache and reads the behavioral counters and cache
// hits/misses (doc 16:54-62, never wall-clock). Also pins the insert-site
// temporal linkage (timed_insert_key_consistent) directly against imageseq's
// render/quantize_time pair.
//
// NOTE: the interactive compositor does not yet apply layer span culling or the
// layer time_map at composition_time (it passes composition_time through as the
// content-local time); those consume the temporal_placement model fields and are
// a later compositor task. The end-to-end span-cull / time_map re-assertions are
// therefore deferred to that task (see kinds.imageseq_plugin return summary);
// #span-cull-is-half-open stays enforced at the value level by
// src/base/t/rational_time.t.cpp.

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/cache/key_shapes.hpp>
#include <arbc/cache/keyed_store.hpp>
#include <arbc/compositor/compositor.hpp>
#include <arbc/compositor/counters.hpp>
#include <arbc/compositor/tile_planning.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/model/model.hpp>
#include <arbc/surface/surface_pool.hpp>

#include "support/imageseq_fixtures.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <vector>

using namespace arbc;
namespace fix = arbc::imageseq::testfix;

namespace {

Time instant(int frame) { return Time{frame * fix::k_period_flicks}; }

std::vector<float> target_pixels(const Surface& surface) {
  const std::span<const float> span = surface.span<PixelFormat::Rgba32fLinearPremul>();
  return {span.begin(), span.end()};
}

// A minimal scene: one imageseq layer over the fixtures. The content binding
// lives in the resolver (runtime's job, doc 17), exactly as the temporal-cache
// tests wire their doubles.
struct Scene {
  Model model;
  std::unique_ptr<arbc::imageseq::ImageSeqContent> content = fix::make_content();
  ObjectId content_id{};

  Scene() {
    Model::Transaction txn = model.transact();
    content_id = txn.add_content(0);
    txn.add_layer(content_id, Affine::identity());
    REQUIRE(txn.commit().has_value());
  }

  ContentResolver resolver() {
    return [this](ObjectId id) -> Content* {
      return id == content_id ? content.get() : nullptr;
    };
  }
};

} // namespace

// enforces: 11-time-and-video#achieved-time-coalescing-issues-zero-renders
// enforces: 16-sdlc-and-quality#byte-exact-goldens
TEST_CASE("imageseq: a sub-frame clock advance issues zero renders and serves identical pixels") {
  Scene scene;
  const ContentResolver resolve = scene.resolver();
  const DocStatePtr state = scene.model.current();
  const Viewport viewport{256, 256, Affine::identity()};

  CpuBackend backend;
  SurfacePool pool(backend);
  TileCache cache(64u * 1024 * 1024);
  CompositorCounters counters;

  auto drive = [&](Time time) {
    auto target = backend.make_surface(viewport.width, viewport.height, k_working_rgba32f);
    REQUIRE(target.has_value());
    render_frame_interactive(*state, resolve, viewport, cache, backend, pool, **target,
                             Deadline::none(), std::nullopt, nullptr, &counters, nullptr, time);
    return target_pixels(**target);
  };

  // Frame at frame-1's instant: one cold miss render.
  const std::vector<float> on_grid = drive(instant(1));
  CHECK(counters.requests_issued() == 1);
  const std::uint64_t misses_after_cold = cache.misses();

  // A clock advance staying inside frame 1's native interval re-plans to an
  // all-fresh cache hit: zero new renders, and the composited pixels are
  // byte-identical to the on-grid frame (coalescing serves the same decode).
  const std::uint64_t hits_before = cache.hits();
  const std::vector<float> sub_frame = drive(Time{instant(1).flicks + 7});
  CHECK(counters.requests_issued() == 1);   // unchanged: zero renders
  CHECK(cache.misses() == misses_after_cold); // no new miss
  CHECK(cache.hits() > hits_before);          // served from the coalesced entry
  CHECK(sub_frame == on_grid);                // byte-exact identical output

  // Advancing across the native-frame boundary keys a distinct entry: exactly
  // one more render.
  drive(instant(2));
  CHECK(counters.requests_issued() == 2);
}

// enforces: 11-time-and-video#coalesced-timed-tile-round-trips-through-cache
TEST_CASE("imageseq render lands on its own quantize_time grid at the insert site") {
  CpuBackend backend;
  auto content = fix::make_content();

  auto render_at = [&](Time t) {
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
    const std::optional<RenderResult> r = content->render(request, done);
    REQUIRE(r.has_value());
    return *r;
  };

  // A sub-frame time: the compositor keys the tile at quantize_time(t) BEFORE it
  // renders; the render must land on that same instant (the doc-11 MUST). The
  // insert-site predicate holds for the pre-quantized key...
  const Time t{instant(2).flicks + 123};
  const std::optional<Time> keyed = content->quantize_time(t);
  REQUIRE(keyed.has_value());
  const TileKey key{ObjectId{}, 0, ScaleRung{0}, TileCoord{0, 0}, keyed};
  const RenderResult result = render_at(t);
  REQUIRE(timed_insert_key_consistent(key, result, Stability::Timed));

  // ...and the tripwire bites a key at the wrong instant (a wrong-frame-under-
  // seek bug would be caught here rather than served).
  const TileKey wrong_key{ObjectId{}, 0, ScaleRung{0}, TileCoord{0, 0}, Time{keyed->flicks + 1}};
  REQUIRE_FALSE(timed_insert_key_consistent(wrong_key, result, Stability::Timed));
}
