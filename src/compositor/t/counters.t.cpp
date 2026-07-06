#include <arbc/base/expected.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/cache/key_shapes.hpp>
#include <arbc/cache/keyed_store.hpp>
#include <arbc/compositor/compositor.hpp>
#include <arbc/compositor/counters.hpp>
#include <arbc/compositor/refinement.hpp>
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
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <vector>

// Behavioral-counter unit tests for the interactive compositor path (doc
// 16:54-62, never wall-clock). The compositor is a pure per-frame library, so
// the counters live in a caller-owned `CompositorCounters` threaded by optional
// pointer; these tests assert *counts* (render requests issued, cache
// hits/misses, tiles composited, follow-up frames), not timings. A stub backend
// and stub content keep the tests inside the compositor's dependency closure --
// the byte-exact rendering path itself is golden'd in
// `tests/tile_planning_golden.t.cpp` / `tests/refinement_golden.t.cpp`.

namespace {

using arbc::CompositorCounters;
using arbc::CompositorStats;
using arbc::Damage;
using arbc::RefinementQueue;
using arbc::RenderResult;
using arbc::ScaleRung;
using arbc::TileCache;
using arbc::TileCoord;
using arbc::TileKey;

// A CPU-less surface carrying a real byte buffer, so a two-run byte comparison
// (the null-path side-effect-free property) is meaningful: the backend writes a
// deterministic marker into it per composite.
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

// A backend that allocates real-buffer surfaces and folds a deterministic
// per-call marker into the destination on composite, so an identical op
// sequence reproduces identical bytes and a dropped composite would not. It
// counts nothing itself -- the compositor's `CompositorCounters` is the counter
// under test.
class MarkBackend : public arbc::Backend {
public:
  arbc::BackendCaps capabilities() const override { return {}; }
  arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError>
  make_surface(int width, int height, arbc::SurfaceFormat /*format*/) override {
    return std::unique_ptr<arbc::Surface>(std::make_unique<BufferSurface>(width, height));
  }
  void clear(arbc::Surface& surface, float /*r*/, float /*g*/, float /*b*/, float /*a*/) override {
    std::span<std::byte> bytes = surface.cpu_bytes();
    std::memset(bytes.data(), 0, bytes.size_bytes());
  }
  void composite(arbc::Surface& dst, const arbc::Surface& /*src*/, const arbc::Affine& /*m*/,
                 double opacity) override {
    const auto mark = static_cast<unsigned>(opacity * 251.0) + 1u;
    for (std::byte& b : dst.cpu_bytes()) {
      b = static_cast<std::byte>((std::to_integer<unsigned>(b) + mark) & 0xFFu);
    }
  }
  void downsample(arbc::Surface& /*dst*/, const arbc::Surface& /*src*/) override {}
};

// Content that answers synchronously with an exact, at-scale result, so a
// warm-cache second frame plans every tile Fresh.
class SyncContent : public arbc::Content {
public:
  std::optional<arbc::Rect> bounds() const override { return arbc::Rect{0.0, 0.0, 512.0, 512.0}; }
  arbc::Stability stability() const override { return arbc::Stability::Static; }
  std::optional<arbc::TimeRange> time_extent() const override { return std::nullopt; }
  std::optional<RenderResult> render(const arbc::RenderRequest& request,
                                     std::shared_ptr<arbc::RenderCompletion> /*done*/) override {
    ++d_renders;
    return RenderResult{request.scale, /*exact=*/true};
  }
  int renders() const { return d_renders; }

private:
  int d_renders{0};
};

// Content that always defers (returns nullopt), leaving the driver's
// `RenderCompletion` live to be recorded into a `RefinementQueue`.
class AsyncContent : public arbc::Content {
public:
  std::optional<arbc::Rect> bounds() const override { return std::nullopt; }
  arbc::Stability stability() const override { return arbc::Stability::Static; }
  std::optional<arbc::TimeRange> time_extent() const override { return std::nullopt; }
  std::optional<RenderResult> render(const arbc::RenderRequest& /*request*/,
                                     std::shared_ptr<arbc::RenderCompletion> /*done*/) override {
    ++d_renders;
    return std::nullopt;
  }
  int renders() const { return d_renders; }

private:
  int d_renders{0};
};

// A single-layer document over `content_id`; the resolver binds the id to a
// concrete `Content` the caller owns (the runtime binding, kept out of L4).
arbc::ObjectId add_single_layer(arbc::Model& model) {
  auto txn = model.transact();
  const arbc::ObjectId content_id = txn.add_content(0);
  txn.add_layer(content_id, arbc::Affine::identity());
  REQUIRE(txn.commit().has_value());
  return content_id;
}

bool bytes_identical(const arbc::Surface& lhs, const arbc::Surface& rhs) {
  const std::span<const std::byte> a = lhs.cpu_bytes();
  const std::span<const std::byte> b = rhs.cpu_bytes();
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size_bytes()) == 0;
}

} // namespace

// A 512x512 identity scene splits into a 2x2 grid of 256^2 rung-0 tiles.
constexpr std::uint64_t k_tiles_covered = 4;

// enforces: 16-sdlc-and-quality#compositor-exposes-behavioral-counters
// enforces: 11-time-and-video#static-tiles-survive-clock
// enforces: 02-architecture#miss-becomes-deadline-request
TEST_CASE("counters: a still warm-cache scene issues zero renders through the counter surface") {
  MarkBackend backend;
  SyncContent content;
  arbc::Model model;
  const arbc::ObjectId content_id = add_single_layer(model);
  const arbc::DocStatePtr state = model.current();
  const auto resolver = [&](arbc::ObjectId id) -> arbc::Content* {
    return id == content_id ? &content : nullptr;
  };
  const arbc::Viewport viewport{512, 512, arbc::Affine::identity()};
  arbc::SurfacePool pool(backend);
  TileCache cache(64u * 1024 * 1024);

  arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError> frame1 =
      backend.make_surface(512, 512, arbc::k_working_rgba32f);
  arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError> frame2 =
      backend.make_surface(512, 512, arbc::k_working_rgba32f);
  REQUIRE(frame1.has_value());
  REQUIRE(frame2.has_value());

  CompositorCounters counters;

  // Frame 1: cold cache. Every covered tile is a fresh-key miss -> one render
  // request driven and one composite issued per tile; the cache saw only misses.
  arbc::render_frame_interactive(*state, resolver, viewport, cache, backend, pool, **frame1,
                                 arbc::Deadline::none(), std::nullopt, nullptr, &counters);
  CHECK(counters.requests_issued() == k_tiles_covered);
  CHECK(counters.composites() == k_tiles_covered);
  CHECK(cache.hits() == 0);
  // Each cold-cache miss also drives the coarser-fallback probes, so cache
  // misses advance by at least the fresh-key miss per tile (the counter surface
  // pins the render-request count exactly; the cache owns its own probe count).
  CHECK(cache.misses() >= k_tiles_covered);

  const std::uint64_t hits_before = cache.hits();
  const std::uint64_t misses_before = cache.misses();

  // Frame 2: warm, unchanged scene. Every tile plans Fresh -> zero new render
  // requests, all cache hits, no new misses, one composite per tile.
  arbc::render_frame_interactive(*state, resolver, viewport, cache, backend, pool, **frame2,
                                 arbc::Deadline::none(), std::nullopt, nullptr, &counters);
  CHECK(counters.requests_issued() == k_tiles_covered);          // delta == 0
  CHECK(counters.composites() == 2 * k_tiles_covered);           // + one per tile
  CHECK(cache.hits() - hits_before == k_tiles_covered);          // all hits
  CHECK(cache.misses() - misses_before == 0);                    // zero new misses
  CHECK(content.renders() == static_cast<int>(k_tiles_covered)); // no second render
}

// enforces: 02-architecture#async-arrival-emits-damage
TEST_CASE("counters: an async arrival records one follow-up frame on the counter surface") {
  arbc::Model model;
  const arbc::ObjectId content_id = add_single_layer(model);
  const arbc::DocStatePtr state = model.current();
  MarkBackend backend;
  AsyncContent content;
  const auto resolver = [&](arbc::ObjectId id) -> arbc::Content* {
    return id == content_id ? &content : nullptr;
  };
  const arbc::Viewport viewport{256, 256, arbc::Affine::identity()}; // 1 rung-0 tile
  arbc::SurfacePool pool(backend);
  TileCache cache(64u * 1024 * 1024);
  arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError> target =
      backend.make_surface(256, 256, arbc::k_working_rgba32f);
  REQUIRE(target.has_value());

  CompositorCounters counters;
  RefinementQueue queue;

  // The inline miss drives one render (counted) but answers async: it is
  // recorded into the queue, not composited, and follow_up_frames stays 0.
  arbc::render_frame_interactive(*state, resolver, viewport, cache, backend, pool, **target,
                                 arbc::Deadline::none(), std::nullopt, &queue, &counters);
  CHECK(counters.requests_issued() == 1);
  CHECK(counters.follow_up_frames() == 0);
  REQUIRE(queue.tiles.size() == 1);

  // An unsettled poll settles nothing -> no follow-up frame.
  CHECK(arbc::poll_refinements(queue, cache, &counters).empty());
  CHECK(counters.follow_up_frames() == 0);

  // On completion the poll inserts the arrival and emits damage: exactly one
  // follow-up frame.
  queue.tiles.front().done->complete(RenderResult{});
  const std::vector<Damage> damage = arbc::poll_refinements(queue, cache, &counters);
  REQUIRE(damage.size() == 1);
  CHECK(counters.follow_up_frames() == 1);

  // The follow-up frame re-plans the settled tile as a fresh hit: zero new
  // render requests, one more cache hit.
  const std::uint64_t hits_before = cache.hits();
  arbc::render_frame_interactive(*state, resolver, viewport, cache, backend, pool, **target,
                                 arbc::Deadline::none(), std::nullopt, &queue, &counters);
  CHECK(counters.requests_issued() == 1); // delta == 0
  CHECK(cache.hits() - hits_before == 1);
}

TEST_CASE("counters: a null out-parameter is byte-identical to an instrumented run") {
  MarkBackend backend;
  arbc::Model model;
  const arbc::ObjectId content_id = add_single_layer(model);
  const arbc::DocStatePtr state = model.current();
  const arbc::Viewport viewport{512, 512, arbc::Affine::identity()};
  arbc::SurfacePool pool(backend);

  // Two independent two-frame drives over identical scenes: one instrumented,
  // one with a null counters pointer.
  auto drive = [&](CompositorCounters* counters, TileCache& cache, arbc::Surface& f1,
                   arbc::Surface& f2) {
    SyncContent content;
    const auto resolver = [&](arbc::ObjectId id) -> arbc::Content* {
      return id == content_id ? &content : nullptr;
    };
    arbc::render_frame_interactive(*state, resolver, viewport, cache, backend, pool, f1,
                                   arbc::Deadline::none(), std::nullopt, nullptr, counters);
    arbc::render_frame_interactive(*state, resolver, viewport, cache, backend, pool, f2,
                                   arbc::Deadline::none(), std::nullopt, nullptr, counters);
  };

  CompositorCounters counters;
  TileCache cache_a(64u * 1024 * 1024);
  TileCache cache_b(64u * 1024 * 1024);
  auto a1 = backend.make_surface(512, 512, arbc::k_working_rgba32f);
  auto a2 = backend.make_surface(512, 512, arbc::k_working_rgba32f);
  auto b1 = backend.make_surface(512, 512, arbc::k_working_rgba32f);
  auto b2 = backend.make_surface(512, 512, arbc::k_working_rgba32f);
  REQUIRE(a1.has_value());
  REQUIRE(a2.has_value());
  REQUIRE(b1.has_value());
  REQUIRE(b2.has_value());

  drive(&counters, cache_a, **a1, **a2);
  drive(nullptr, cache_b, **b1, **b2);

  // The counter path is side-effect-free on rendering: identical target bytes...
  CHECK(bytes_identical(**a1, **b1));
  CHECK(bytes_identical(**a2, **b2));
  // ...and identical cache hit/miss deltas.
  CHECK(cache_a.hits() == cache_b.hits());
  CHECK(cache_a.misses() == cache_b.misses());
}

TEST_CASE("counters: counters_snapshot composes the compositor and cache counts") {
  MarkBackend backend;
  SyncContent content;
  arbc::Model model;
  const arbc::ObjectId content_id = add_single_layer(model);
  const arbc::DocStatePtr state = model.current();
  const auto resolver = [&](arbc::ObjectId id) -> arbc::Content* {
    return id == content_id ? &content : nullptr;
  };
  const arbc::Viewport viewport{512, 512, arbc::Affine::identity()};
  arbc::SurfacePool pool(backend);
  TileCache cache(64u * 1024 * 1024);
  auto target = backend.make_surface(512, 512, arbc::k_working_rgba32f);
  REQUIRE(target.has_value());

  CompositorCounters counters;
  arbc::render_frame_interactive(*state, resolver, viewport, cache, backend, pool, **target,
                                 arbc::Deadline::none(), std::nullopt, nullptr, &counters);

  const CompositorStats stats = arbc::counters_snapshot(counters, cache);
  CHECK(stats.requests_issued == counters.requests_issued());
  CHECK(stats.composites == counters.composites());
  CHECK(stats.follow_up_frames == counters.follow_up_frames());
  CHECK(stats.cache_hits == cache.hits());
  CHECK(stats.cache_misses == cache.misses());
  CHECK(stats.cache_evictions == cache.evictions());
}
