// org.arbc.imageseq through the compositor: the reference Timed content
// end-to-end re-assertion of achieved-time coalescing (doc 11:138-159). Drives a
// real imageseq layer through render_frame_interactive at fixed composition
// times against a live TileCache and reads the behavioral counters and cache
// hits/misses (doc 16:54-62, never wall-clock). Also pins the insert-site
// temporal linkage (timed_insert_key_consistent) directly against imageseq's
// render/quantize_time pair.
//
// The interactive compositor now consumes the layer's temporal_placement fields
// (compositor.temporal_placement_culling): it culls a layer whose half-open span
// does not contain the frame's composition time and evaluates the layer time_map
// at composition time to the content-local time it requests. This file discharges
// the reverse-rate through-compositor golden imageseq_plugin deferred to that
// task: a placed imageseq layer with a negative-rate time_map, driven at ASCENDING
// composition times, requests DESCENDING content-local frames and composites the
// fixture sequence in reverse, byte-for-byte (doc 11:66-71, 122-124).

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/rational_time.hpp>
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

#include <catch2/catch_test_macros.hpp>

#include "support/imageseq_fixtures.hpp"

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
  ObjectId layer_id{};

  Scene() {
    Model::Transaction txn = model.transact();
    content_id = txn.add_content(0);
    layer_id = txn.add_layer(content_id, Affine::identity());
    REQUIRE(txn.commit().has_value());
  }

  // Place the layer under a time_map (reverse playback is a negative-rate map on
  // the layer, not a content-rate change -- imageseq_fixtures.hpp:50-52).
  void set_time_map(const TimeMap& map) {
    Model::Transaction txn = model.transact();
    txn.set_time_map(layer_id, map);
    REQUIRE(txn.commit().has_value());
  }

  ContentResolver resolver() {
    return [this](ObjectId id) -> Content* { return id == content_id ? content.get() : nullptr; };
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
  CHECK(counters.requests_issued() == 1);     // unchanged: zero renders
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

// enforces: 11-time-and-video#compositor-retimes-request-through-time-map
// enforces: 16-sdlc-and-quality#byte-exact-goldens
TEST_CASE("imageseq: a negative-rate layer time_map plays the fixtures in reverse "
          "through the compositor, byte-for-byte") {
  const Viewport viewport{256, 256, Affine::identity()};

  auto pixels_at = [&](Scene& scene, TileCache& cache, Time time) {
    CpuBackend backend;
    SurfacePool pool(backend);
    CompositorCounters counters;
    const DocStatePtr state = scene.model.current();
    const ContentResolver resolve = scene.resolver();
    auto target = backend.make_surface(viewport.width, viewport.height, k_working_rgba32f);
    REQUIRE(target.has_value());
    render_frame_interactive(*state, resolve, viewport, cache, backend, pool, **target,
                             Deadline::none(), std::nullopt, nullptr, &counters, nullptr, time);
    return target_pixels(**target);
  };

  // Forward reference: an identity-map layer composited at each native frame's
  // instant is the through-compositor image of that fixture frame. The four
  // fixtures are distinct solid colors, so the frames are genuinely different.
  Scene forward_scene;
  TileCache forward_cache(64u * 1024 * 1024);
  std::vector<std::vector<float>> forward;
  for (int f = 0; f < fix::k_frame_count; ++f) {
    forward.push_back(pixels_at(forward_scene, forward_cache, instant(f)));
  }
  for (int f = 1; f < fix::k_frame_count; ++f) {
    REQUIRE(forward[static_cast<std::size_t>(f)] != forward[static_cast<std::size_t>(f - 1)]);
  }

  // Reverse playback: a negative-rate time_map maps composition frame `f` to the
  // content-local instant `(k_frame_count-1 - f) * period`, so advancing the
  // composition clock walks the fixtures backwards. With `in = 0`, `rate = -1/1`,
  // `offset = (N-1)*period`: local = -(composition_time) + (N-1)*period.
  Scene reverse_scene;
  reverse_scene.set_time_map(TimeMap{Time{0}, Rational{-1, 1}, instant(fix::k_frame_count - 1)});
  TileCache reverse_cache(64u * 1024 * 1024);
  for (int f = 0; f < fix::k_frame_count; ++f) {
    const std::vector<float> got = pixels_at(reverse_scene, reverse_cache, instant(f));
    // Advancing composition time yields a DECREASING content-local frame: the
    // frame composited at composition instant f is fixture frame (N-1 - f),
    // byte-for-byte identical to the forward reference for that fixture.
    REQUIRE(got == forward[static_cast<std::size_t>(fix::k_frame_count - 1 - f)]);
  }
}
