#include <arbc/base/expected.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/cache/key_shapes.hpp>
#include <arbc/cache/keyed_store.hpp>
#include <arbc/compositor/scale_ladder.hpp>
#include <arbc/compositor/tile_planning.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/model/records.hpp>
#include <arbc/surface/backend.hpp>
#include <arbc/surface/surface.hpp>
#include <arbc/surface/surface_error.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

// Live-cache-boundary soundness for achieved-time coalescing (doc 11:130-148,
// doc 16:54-62). The predecessor tasks proved coalescing only *indirectly*
// (composited-output byte-identity + a render counter, in
// `temporal_coalescing_golden.t.cpp`) or *abstractly* (a contract-unit double, in
// `contract/t/temporal_fields.t.cpp`). This file pins the two remaining soundness
// facets at the `TileCache` insert->lookup API the compositor actually keys a
// Timed tile under:
//   1. a live-cache round-trip: an insert-on-miss then a *direct* `cache.lookup`
//      returns the very stored surface across a sub-frame advance (hit) and a
//      distinct entry across a native-frame boundary (miss);
//   2. the insert-key linkage: `timed_insert_key_consistent` catches a Timed
//      content whose render lands off its own `quantize_time` grid, called
//      directly so the claim holds regardless of `NDEBUG`.

namespace {

using arbc::CacheHold;
using arbc::LayerTilePlan;
using arbc::PriorityClass;
using arbc::Rect;
using arbc::RenderResult;
using arbc::Stability;
using arbc::TileCache;
using arbc::TileKey;
using arbc::TileValue;

// The 24 fps native frame period in flicks (exact: flicks_per_second % 24 == 0).
constexpr std::int64_t k_frame = arbc::Time::flicks_per_second / 24; // 29'400'000
// One 1/60 s output-frame step -- shorter than one native period, so an advance
// by this amount stays inside the same native frame (the coalescing case).
constexpr std::int64_t k_output_step = arbc::Time::flicks_per_second / 60; // 11'760'000

// Two requested instants inside native frame 7 ([7/24, 8/24)), one output step
// apart, plus an instant in the next native frame (frame 8).
constexpr arbc::Time k_t0{210'000'000};                     // interior of frame 7
constexpr arbc::Time k_t_same{210'000'000 + k_output_step}; // 221'760'000, still frame 7
constexpr arbc::Time k_t_next{240'000'000};                 // interior of frame 8
constexpr arbc::Time k_frame7{7 * k_frame};                 // 205'800'000 == 7/24 s
constexpr arbc::Time k_frame8{8 * k_frame};                 // 235'200'000 == 8/24 s

// A conformant 24 fps `Timed` clip: `quantize_time` snaps a requested instant
// DOWN to its native grid render-free, and `render` reports the SAME instant via
// `achieved_time` -- the doc-11 MUST the compositor keys on.
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

// A deliberately-misbehaving `Timed` clip: it quantizes to the same 24 fps grid,
// but its `render` reports the RAW requested time as `achieved_time` -- off its
// own `quantize_time` grid, a violation of the doc-11 MUST (11:134-137). Keyed at
// the grid instant but rendering a different one, it would serve a wrong frame
// under seek if the linkage went unchecked.
class DriftingTimedContent : public arbc::Content {
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
    result.achieved_time = request.time; // off-grid: does NOT equal quantize_time
    return result;
  }
};

// A CPU-buffer surface backing the real cache tiles the round-trip inserts and
// looks up -- the round-trip asserts surface *identity* through the store, so the
// tiles must be real owning surfaces (not the metadata-only stub the plan-only
// tests hand-populate with).
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

// A backend that allocates real `BufferSurface` tiles; composite/clear are
// no-ops (this file asserts cache identity and counters, never pixels).
class BufferBackend : public arbc::Backend {
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

// A single-tile region so the plan holds exactly one tile, keyed at one instant.
Rect one_tile_region() { return Rect{0.0, 0.0, 256.0, 256.0}; }

LayerTilePlan plan_timed(TileCache& cache, const arbc::Content* content, Stability stability,
                         arbc::Time time) {
  return arbc::plan_layer(cache, k_content, k_revision, std::nullopt, arbc::select_rung(1.0),
                          one_tile_region(), arbc::Affine::identity(), stability, time,
                          arbc::StateHandle{}, arbc::Deadline::none(), content);
}

// Drive the compositor render-on-miss sequence exactly as the interactive driver
// does (`tile_planning.cpp`): render the content into a freshly allocated
// cache-owned surface, validate the insert-key linkage, and insert under the
// plan-time key. Returns the raw surface pointer inserted so the caller can assert
// the store returns that same object.
arbc::Surface* render_and_insert(TileCache& cache, arbc::Backend& backend, arbc::Content& content,
                                 const TileKey& key, arbc::Time time) {
  arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError> owned =
      backend.make_surface(arbc::k_tile_size, arbc::k_tile_size, arbc::k_working_rgba32f);
  REQUIRE(owned.has_value());
  arbc::Surface* const inserted = owned->get();

  const arbc::RenderRequest request{
      one_tile_region(),           arbc::rung_scale(key.rung), time, arbc::StateHandle{}, **owned,
      arbc::Exactness::BestEffort, arbc::Deadline::none()};
  auto done = std::make_shared<arbc::RenderCompletion>();
  const std::optional<RenderResult> result = content.render(request, done);
  REQUIRE(result.has_value());
  // The linkage the insert site asserts under `assert`; called here directly so
  // it is exercised regardless of `NDEBUG`.
  CHECK(arbc::timed_insert_key_consistent(key, *result, content.stability()));

  const std::size_t bytes = arbc::tile_byte_cost(**owned);
  cache.insert(key, TileValue{std::move(*owned), {result->achieved_scale, result->exact}}, bytes,
               PriorityClass::Visible);
  return inserted;
}

} // namespace

// enforces: 11-time-and-video#coalesced-timed-tile-round-trips-through-cache
TEST_CASE("live-cache round-trip: a coalesced Timed tile is reused at the KeyedStore API") {
  TimedContent content;
  BufferBackend backend;
  TileCache cache(64u * 1024 * 1024);
  // A cold scratch cache used only to compute the keys the compositor would form
  // at other instants -- key construction is independent of cache contents, so
  // this keeps the live cache's hit/miss counters clean for the direct-lookup
  // assertions below (a plan on the live cache would itself lookup and pollute them).
  TileCache scratch(64u * 1024 * 1024);

  // Plan at t0 (interior of frame 7): a cold miss keyed at the coalesced 7/24 instant.
  const LayerTilePlan p0 = plan_timed(cache, &content, Stability::Timed, k_t0);
  REQUIRE(p0.tiles.size() == 1);
  const TileKey key0 = p0.tiles[0].key;
  REQUIRE(key0.achieved_time == std::optional<arbc::Time>{k_frame7});
  REQUIRE(p0.tiles[0].is_miss);

  // Render-on-miss inserts the tile under key0.
  arbc::Surface* const inserted = render_and_insert(cache, backend, content, key0, k_t0);

  // A sub-frame-later instant (t0 + 1/60 s, still frame 7) forms the IDENTICAL
  // coalesced key: distinct requested instant, same native-grid key.
  const LayerTilePlan p_same = plan_timed(scratch, &content, Stability::Timed, k_t_same);
  const TileKey key_same = p_same.tiles[0].key;
  CHECK(key_same == key0);

  // Direct KeyedStore round-trip: a lookup at that key HITS and returns the very
  // surface the render inserted -- reuse proven at the cache API, not via
  // composited-output byte-identity.
  const std::uint64_t hits_before = cache.hits();
  const std::uint64_t misses_before = cache.misses();
  std::optional<CacheHold<TileValue>> hit = cache.lookup(key_same);
  REQUIRE(hit.has_value());
  CHECK(cache.hits() == hits_before + 1);
  CHECK(cache.misses() == misses_before); // a hit: no miss
  CHECK(hit->get().surface.get() == inserted);

  // An instant across the native-frame boundary (8/24) keys a DISTINCT entry, so a
  // direct lookup on the frame-7-warm cache misses -- coalescing is frame-specific.
  const LayerTilePlan p_next = plan_timed(scratch, &content, Stability::Timed, k_t_next);
  const TileKey key_next = p_next.tiles[0].key;
  CHECK(key_next.achieved_time == std::optional<arbc::Time>{k_frame8});
  CHECK(key_next != key0);
  const std::uint64_t misses_before2 = cache.misses();
  CHECK_FALSE(cache.lookup(key_next).has_value());
  CHECK(cache.misses() == misses_before2 + 1); // a distinct native frame: a miss

  // Rendering that boundary instant inserts a SECOND, distinct entry: both native
  // frames now coexist in the cache, each returning its own stored surface.
  arbc::Surface* const inserted_next =
      render_and_insert(cache, backend, content, key_next, k_t_next);
  std::optional<CacheHold<TileValue>> h0 = cache.lookup(key0);
  std::optional<CacheHold<TileValue>> hn = cache.lookup(key_next);
  REQUIRE(h0.has_value());
  REQUIRE(hn.has_value());
  CHECK(h0->get().surface.get() == inserted);
  CHECK(hn->get().surface.get() == inserted_next);
  CHECK(inserted != inserted_next);
}

// enforces: 11-time-and-video#coalesced-timed-tile-round-trips-through-cache
TEST_CASE("insert-key linkage: a render off its own quantize grid is caught at the insert site") {
  BufferBackend backend;
  TileCache scratch(64u * 1024 * 1024);

  // The compositor forms the same 7/24 key for either content (both quantize to
  // the grid); the difference is only what their render reports it landed on.
  DriftingTimedContent drifting;
  const TileKey key = plan_timed(scratch, &drifting, Stability::Timed, k_t0).tiles[0].key;
  REQUIRE(key.achieved_time == std::optional<arbc::Time>{k_frame7});

  BufferSurface target(arbc::k_tile_size, arbc::k_tile_size);
  const arbc::RenderRequest request{
      one_tile_region(),           arbc::rung_scale(key.rung), k_t0, arbc::StateHandle{}, target,
      arbc::Exactness::BestEffort, arbc::Deadline::none()};
  auto done = std::make_shared<arbc::RenderCompletion>();

  // The misbehaving render reports the raw t0, OFF the frame-7 grid the tile is
  // keyed under -- the predicate reports inconsistent (this is what the insert
  // site's assert fires on in a debug build).
  const RenderResult drifted = *drifting.render(request, done);
  CHECK(drifted.achieved_time == std::optional<arbc::Time>{k_t0});
  CHECK(drifted.achieved_time != key.achieved_time);
  CHECK_FALSE(arbc::timed_insert_key_consistent(key, drifted, Stability::Timed));

  // Conformant content on the SAME key reports the grid instant -- consistent.
  TimedContent good;
  const RenderResult ok = *good.render(request, done);
  CHECK(ok.achieved_time == std::optional<arbc::Time>{k_frame7});
  CHECK(arbc::timed_insert_key_consistent(key, ok, Stability::Timed));

  // Exemptions: a Timed render reporting nullopt achieved_time (time honored
  // exactly / un-migrated quantize default) is consistent -- the key then carries
  // the raw time and coalesces nothing, still sound.
  const RenderResult honored{ok.achieved_scale, ok.exact, std::nullopt};
  CHECK(arbc::timed_insert_key_consistent(key, honored, Stability::Timed));

  // Static/Live content owns no achieved-time grid: exempt even against the
  // off-grid drifted result.
  CHECK(arbc::timed_insert_key_consistent(key, drifted, Stability::Live));
  const TileKey static_key{k_content, k_revision, key.rung, key.coord, std::nullopt};
  CHECK(arbc::timed_insert_key_consistent(static_key, honored, Stability::Static));
}
