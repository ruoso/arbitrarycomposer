#include <arbc/base/expected.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/cache/key_shapes.hpp>
#include <arbc/cache/keyed_store.hpp>
#include <arbc/compositor/compositor.hpp>
#include <arbc/compositor/counters.hpp>
#include <arbc/compositor/damage_planning.hpp>
#include <arbc/compositor/scale_ladder.hpp>
#include <arbc/compositor/tile_planning.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/model/model.hpp>
#include <arbc/surface/backend.hpp>
#include <arbc/surface/surface.hpp>
#include <arbc/surface/surface_error.hpp>
#include <arbc/surface/surface_pool.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

// Unit + behavioral-counter tests for achieved-time coalescing (doc 11:110-129,
// doc 16:54-62). The pure keying change is exercised on `plan_layer` against a
// hand-populated `TileCache` (no backend); the zero-render behavioral property
// is pinned on the interactive driver through `CompositorCounters` (never
// wall-clock). The end-to-end byte-exact reuse is golden'd in
// `tests/temporal_coalescing_golden.t.cpp`.

namespace {

using arbc::CompositorCounters;
using arbc::DirtyRegion;
using arbc::LayerTilePlan;
using arbc::PriorityClass;
using arbc::Rect;
using arbc::RenderResult;
using arbc::RungSelection;
using arbc::Stability;
using arbc::TileCache;
using arbc::TileCoord;
using arbc::TileKey;
using arbc::TileSource;
using arbc::TileValue;

// The 24 fps native frame period in flicks (exact: flicks_per_second % 24 == 0).
constexpr std::int64_t k_frame = arbc::Time::flicks_per_second / 24; // 29'400'000
// One 1/60 s output-frame step -- shorter than one native period, so an advance
// by this amount stays inside the same native frame (the coalescing case).
constexpr std::int64_t k_output_step = arbc::Time::flicks_per_second / 60; // 11'760'000

// Two requested instants inside native frame 7 ([7/24, 8/24)), one output step
// apart, plus an instant in the next native frame (frame 8).
constexpr arbc::Time k_t0{210'000'000};                    // interior of frame 7
constexpr arbc::Time k_t_same{210'000'000 + k_output_step}; // 221'760'000, still frame 7
constexpr arbc::Time k_t_next{240'000'000};                 // interior of frame 8
constexpr arbc::Time k_frame7{7 * k_frame};                 // 205'800'000
constexpr arbc::Time k_frame8{8 * k_frame};                 // 235'200'000

// A `Timed` test content: a 24 fps clip. `quantize_time` snaps a requested time
// DOWN to its native frame grid render-free; `render` reports the same instant
// via `achieved_time` (the contract equality the compositor keys on).
class TimedContent : public arbc::Content {
public:
  std::optional<Rect> bounds() const override { return Rect{0.0, 0.0, 512.0, 512.0}; }
  Stability stability() const override { return Stability::Timed; }
  std::optional<arbc::TimeRange> time_extent() const override {
    return arbc::TimeRange{arbc::Time::zero(), arbc::Time{k_frame * 48}};
  }
  std::optional<arbc::Time> quantize_time(arbc::Time t) const override {
    return arbc::Time{(t.flicks / k_frame) * k_frame};
  }
  std::optional<RenderResult> render(const arbc::RenderRequest& request,
                                     std::shared_ptr<arbc::RenderCompletion> /*done*/) override {
    RenderResult result{request.scale, /*exact=*/true};
    result.achieved_time = arbc::Time{(request.time.flicks / k_frame) * k_frame};
    return result;
  }
};

// A `Timed` content that does NOT quantize (the defaulted `quantize_time` ->
// nullopt): it honors any requested time exactly, so keying falls back to the
// raw requested time -- the identity default, byte-identical to pre-coalescing.
class IdentityTimedContent : public arbc::Content {
public:
  std::optional<Rect> bounds() const override { return Rect{0.0, 0.0, 512.0, 512.0}; }
  Stability stability() const override { return Stability::Timed; }
  std::optional<arbc::TimeRange> time_extent() const override {
    return arbc::TimeRange{arbc::Time::zero(), arbc::Time{k_frame * 48}};
  }
  // quantize_time inherits the defaulted nullopt.
  std::optional<RenderResult> render(const arbc::RenderRequest& request,
                                     std::shared_ptr<arbc::RenderCompletion> /*done*/) override {
    return RenderResult{request.scale, /*exact=*/true};
  }
};

// A `Static` content: time-invariant, keys omit achieved_time regardless.
class StaticContent : public arbc::Content {
public:
  std::optional<Rect> bounds() const override { return Rect{0.0, 0.0, 512.0, 512.0}; }
  Stability stability() const override { return Stability::Static; }
  std::optional<arbc::TimeRange> time_extent() const override { return std::nullopt; }
  std::optional<RenderResult> render(const arbc::RenderRequest& request,
                                     std::shared_ptr<arbc::RenderCompletion> /*done*/) override {
    return RenderResult{request.scale, /*exact=*/true};
  }
};

// A minimal Surface for the hand-populated cache in the plan-only tests: the
// planner reads only `TileValue.meta`, never pixels.
class StubSurface : public arbc::Surface {
public:
  int width() const override { return arbc::k_tile_size; }
  int height() const override { return arbc::k_tile_size; }
  arbc::SurfaceFormat format() const override { return arbc::k_working_rgba32f; }
  std::span<std::byte> cpu_bytes() override { return {}; }
  std::span<const std::byte> cpu_bytes() const override { return {}; }
};

// A CPU-buffer surface + a byte-touching backend so the driver's composite path
// is real; the counts, not the bytes, are under test here.
class BufferSurface : public arbc::Surface {
public:
  BufferSurface(int width, int height)
      : d_width(width), d_height(height),
        d_bytes(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 16,
                std::byte{0}) {}
  int width() const override { return d_width; }
  int height() const override { return d_height; }
  arbc::SurfaceFormat format() const override { return arbc::k_working_rgba32f; }
  std::span<std::byte> cpu_bytes() override { return d_bytes; }
  std::span<const std::byte> cpu_bytes() const override { return d_bytes; }

private:
  int d_width;
  int d_height;
  std::vector<std::byte> d_bytes;
};

class MarkBackend : public arbc::Backend {
public:
  arbc::BackendCaps capabilities() const override { return {}; }
  arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError>
  make_surface(int width, int height, arbc::SurfaceFormat /*format*/) override {
    return std::unique_ptr<arbc::Surface>(std::make_unique<BufferSurface>(width, height));
  }
  void clear(arbc::Surface& /*surface*/, float /*r*/, float /*g*/, float /*b*/,
             float /*a*/) override {}
  void composite(arbc::Surface& /*dst*/, const arbc::Surface& /*src*/, const arbc::Affine& /*m*/,
                 double /*opacity*/) override {}
  void downsample(arbc::Surface& /*dst*/, const arbc::Surface& /*src*/) override {}
};

const arbc::ObjectId k_content{7};
constexpr std::uint64_t k_revision = 100;

// A single-tile region so counts read as "exactly one render per native frame".
Rect one_tile_region() { return Rect{0.0, 0.0, 256.0, 256.0}; }

LayerTilePlan plan_timed(TileCache& cache, const arbc::Content* content, Stability stability,
                         arbc::Time time) {
  return arbc::plan_layer(cache, k_content, k_revision, std::nullopt, arbc::select_rung(1.0),
                          one_tile_region(), arbc::Affine::identity(), stability, time,
                          arbc::StateHandle{}, arbc::Deadline::none(), content);
}

void put_tile(TileCache& cache, const TileKey& key) {
  cache.insert(key, TileValue{std::make_unique<StubSurface>(), {arbc::rung_scale(key.rung), true}},
               /*bytes=*/1, PriorityClass::Visible);
}

// A single identity-transform Timed layer bound to a fresh content id.
arbc::ObjectId add_layer(arbc::Model& model) {
  auto txn = model.transact();
  const arbc::ObjectId content_id = txn.add_content(0);
  txn.add_layer(content_id, arbc::Affine::identity());
  REQUIRE(txn.commit().has_value());
  return content_id;
}

} // namespace

// enforces: 11-time-and-video#tile-key-carries-time-and-revision
TEST_CASE("plan_layer coalesces Timed keys within one native frame period") {
  TimedContent content;
  TileCache cache(64u * 1024 * 1024);

  // (a) Two requested instants one output step apart, both inside native frame 7,
  // produce byte-identical keys snapped to 7/24 -- coalesced.
  const LayerTilePlan p0 = plan_timed(cache, &content, Stability::Timed, k_t0);
  const LayerTilePlan p_same = plan_timed(cache, &content, Stability::Timed, k_t_same);
  REQUIRE(p0.tiles.size() == 1);
  REQUIRE(p_same.tiles.size() == 1);
  CHECK(p0.tiles[0].key.achieved_time == std::optional<arbc::Time>{k_frame7});
  CHECK(p0.tiles[0].key == p_same.tiles[0].key);
  CHECK(p0.tiles[0].is_miss);      // cold cache: a render is owed
  CHECK(p_same.tiles[0].is_miss);

  // Warm the cache under the coalesced (frame-7) key: BOTH instants now plan
  // Fresh with zero misses -- the second request issues no render.
  put_tile(cache, p0.tiles[0].key);
  const LayerTilePlan w0 = plan_timed(cache, &content, Stability::Timed, k_t0);
  const LayerTilePlan w_same = plan_timed(cache, &content, Stability::Timed, k_t_same);
  CHECK(w0.tiles[0].display_source == TileSource::Fresh);
  CHECK_FALSE(w0.tiles[0].is_miss);
  CHECK(w_same.tiles[0].display_source == TileSource::Fresh);
  CHECK_FALSE(w_same.tiles[0].is_miss);

  // (b) An instant in the NEXT native frame keys a distinct achieved_time (8/24)
  // and misses on the warm cache -- coalescing is native-frame-specific, sound
  // under seek (the content, not the compositor, owns the grid).
  const LayerTilePlan p_next = plan_timed(cache, &content, Stability::Timed, k_t_next);
  CHECK(p_next.tiles[0].key.achieved_time == std::optional<arbc::Time>{k_frame8});
  CHECK(p_next.tiles[0].key != p0.tiles[0].key);
  CHECK(p_next.tiles[0].is_miss);
}

// enforces: 11-time-and-video#static-tiles-survive-clock
TEST_CASE("plan_layer: a Static layer keys nullopt at any time, unchanged by quantization") {
  StaticContent content;
  TileCache cache(64u * 1024 * 1024);

  const LayerTilePlan p0 = plan_timed(cache, &content, Stability::Static, k_t0);
  const LayerTilePlan p_next = plan_timed(cache, &content, Stability::Static, k_t_next);
  REQUIRE(p0.tiles.size() == 1);
  CHECK(p0.tiles[0].key.achieved_time == std::nullopt);
  CHECK(p0.tiles[0].key == p_next.tiles[0].key); // clock-invariant: identical keys
}

// enforces: 11-time-and-video#tile-key-carries-time-and-revision
TEST_CASE("plan_layer: a nullopt quantize_time keys the raw requested time (identity default)") {
  IdentityTimedContent content;
  TileCache cache(64u * 1024 * 1024);

  // No quantization: each requested instant keys itself, so two sub-frame times
  // do NOT coalesce -- the default is a byte-identical no-op.
  const LayerTilePlan p0 = plan_timed(cache, &content, Stability::Timed, k_t0);
  const LayerTilePlan p_same = plan_timed(cache, &content, Stability::Timed, k_t_same);
  CHECK(p0.tiles[0].key.achieved_time == std::optional<arbc::Time>{k_t0});
  CHECK(p_same.tiles[0].key.achieved_time == std::optional<arbc::Time>{k_t_same});
  CHECK(p0.tiles[0].key != p_same.tiles[0].key);

  // A null content pointer takes the same raw-time path (the landed default).
  const LayerTilePlan p_null = plan_timed(cache, /*content=*/nullptr, Stability::Timed, k_t0);
  CHECK(p_null.tiles[0].key == p0.tiles[0].key);
}

// enforces: 11-time-and-video#achieved-time-coalescing-issues-zero-renders
TEST_CASE("driver: an advance within one native frame issues zero renders (counter-backed)") {
  MarkBackend backend;
  TimedContent content;
  arbc::Model model;
  const arbc::ObjectId content_id = add_layer(model);
  const arbc::DocStatePtr state = model.current();
  const auto resolver = [&](arbc::ObjectId id) -> arbc::Content* {
    return id == content_id ? &content : nullptr;
  };
  // A 256x256 viewport -> exactly one rung-0 tile, so "renders exactly once".
  const arbc::Viewport viewport{256, 256, arbc::Affine::identity()};
  arbc::SurfacePool pool(backend);
  TileCache cache(64u * 1024 * 1024);
  CompositorCounters counters;

  auto drive = [&](arbc::Time composition_time) {
    arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError> target =
        backend.make_surface(256, 256, arbc::k_working_rgba32f);
    REQUIRE(target.has_value());
    arbc::render_frame_interactive(*state, resolver, viewport, cache, backend, pool, **target,
                                   arbc::Deadline::none(), std::nullopt, nullptr, &counters, nullptr,
                                   composition_time);
  };

  // Frame 1 at t0: cold cache -> exactly one render, one composite.
  drive(k_t0);
  CHECK(counters.requests_issued() == 1);
  CHECK(counters.composites() == 1);

  // Frame 2 at t0 + 1/60 s (same native frame): the coalesced key is warm ->
  // zero NEW renders. The full re-plan re-composites the reused tile.
  drive(k_t_same);
  CHECK(counters.requests_issued() == 1); // delta 0: zero renders
  CHECK(counters.composites() == 2);      // one re-composite of the cached tile

  // Frame 3 at t2 in the next native frame: a distinct key, cold -> exactly one
  // more render.
  drive(k_t_next);
  CHECK(counters.requests_issued() == 2); // +1: an advance across a boundary renders once
  CHECK(counters.composites() == 3);
}

// enforces: 11-time-and-video#achieved-time-coalescing-issues-zero-renders
TEST_CASE("driver: a coalesced sub-frame advance is a quiescent temporal frame (zero composites)") {
  MarkBackend backend;
  TimedContent content;
  arbc::Model model;
  const arbc::ObjectId content_id = add_layer(model);
  const arbc::DocStatePtr state = model.current();
  const auto resolver = [&](arbc::ObjectId id) -> arbc::Content* {
    return id == content_id ? &content : nullptr;
  };
  const arbc::Viewport viewport{256, 256, arbc::Affine::identity()};
  arbc::SurfacePool pool(backend);
  TileCache cache(64u * 1024 * 1024);

  arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError> target =
      backend.make_surface(256, 256, arbc::k_working_rgba32f);
  REQUIRE(target.has_value());

  // Warm the cache at t0.
  CompositorCounters warmup;
  arbc::render_frame_interactive(*state, resolver, viewport, cache, backend, pool, **target,
                                 arbc::Deadline::none(), std::nullopt, nullptr, &warmup, nullptr,
                                 k_t0);

  // The advance to t0 + 1/60 s stays within one native frame -- `quantize_time`
  // reports the SAME grid instant, so the moving layer's pixels are quiescent and
  // the runtime schedules no temporal damage (the temporal "no damage -> no
  // work"). That empty dirty region drives zero renders AND zero composites.
  REQUIRE(content.quantize_time(k_t0) == content.quantize_time(k_t_same));
  const DirtyRegion empty{};
  CompositorCounters counters;
  arbc::render_frame_interactive(*state, resolver, viewport, cache, backend, pool, **target,
                                 arbc::Deadline::none(), std::nullopt, nullptr, &counters, &empty,
                                 k_t_same);
  CHECK(counters.requests_issued() == 0);
  CHECK(counters.composites() == 0);
}
