#include <arbc/base/expected.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/cache/key_shapes.hpp>
#include <arbc/cache/keyed_store.hpp>
#include <arbc/compositor/compositor.hpp>
#include <arbc/compositor/counters.hpp>
#include <arbc/compositor/pull_service.hpp>
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
#include <arbc/surface/testing/stub_backend.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
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
class MarkBackend : public arbc::testing::StubBackend {
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

// An operator content over one input (`inputs()` non-empty is the whole
// leaf/operator test, `operator_graph.hpp`). Its render pulls that input over the
// tile it was asked for -- the descent the real fade/nested kinds make -- and, when
// the pull answers asynchronously, reports the TRANSIENT, INEXACT placeholder the
// contract requires (doc 13:117-120): flagging it exact would freeze this pass's
// empty tile into the cache as a fresh hit, and the arrival would never re-drive it.
// Two of these over ONE leaf is the duplicate-dispatch shape the dedup guard exists
// for: both pull the same covering tile, in the same frame, while it is in flight.
class PullOperator : public arbc::Content {
public:
  arbc::PullServiceImpl* service{nullptr};

  explicit PullOperator(arbc::Content* input) : d_inputs{input} {}

  std::optional<arbc::Rect> bounds() const override { return arbc::Rect{0.0, 0.0, 512.0, 512.0}; }
  arbc::Stability stability() const override { return arbc::Stability::Static; }
  std::optional<arbc::TimeRange> time_extent() const override { return std::nullopt; }
  std::span<const arbc::ContentRef> inputs() const override { return d_inputs; }
  std::optional<RenderResult> render(const arbc::RenderRequest& request,
                                     std::shared_ptr<arbc::RenderCompletion> /*done*/) override {
    auto inner = std::make_shared<arbc::RenderCompletion>();
    service->pull(d_inputs.front(), request, inner);
    if (!inner->settled()) {
      inner->cancel(); // the completion this pass will not drain
      return RenderResult{request.scale, /*exact=*/false};
    }
    return RenderResult{request.scale, /*exact=*/true};
  }

private:
  std::vector<arbc::ContentRef> d_inputs;
};

// An operator over N inputs -- the nested-chain shape, which `PullOperator` (one
// input) cannot express. It pulls every input over the tile it was asked for and,
// when any of them answers asynchronously, reports the TRANSIENT, INEXACT placeholder
// the contract requires (doc 13:117-120). Stacked -- a top operator over two
// mid operators, each over one async leaf -- this is `nested_deep` in miniature: three
// operators, two leaves, and the chain that used to re-render once per independently
// arriving leaf.
class ChainOperator : public arbc::Content {
public:
  arbc::PullServiceImpl* service{nullptr};

  explicit ChainOperator(std::vector<arbc::ContentRef> inputs) : d_inputs(std::move(inputs)) {}

  std::optional<arbc::Rect> bounds() const override { return arbc::Rect{0.0, 0.0, 512.0, 512.0}; }
  arbc::Stability stability() const override { return arbc::Stability::Static; }
  std::optional<arbc::TimeRange> time_extent() const override { return std::nullopt; }
  std::span<const arbc::ContentRef> inputs() const override { return d_inputs; }
  std::optional<RenderResult> render(const arbc::RenderRequest& request,
                                     std::shared_ptr<arbc::RenderCompletion> /*done*/) override {
    ++d_renders;
    bool exact = true;
    for (const arbc::ContentRef input : d_inputs) {
      auto inner = std::make_shared<arbc::RenderCompletion>();
      service->pull(input, request, inner);
      const std::optional<arbc::expected<RenderResult, arbc::RenderError>> settled = inner->take();
      if (!(settled.has_value() && settled->has_value() && (*settled)->exact)) {
        exact = false; // this input is still coming: degrade to the placeholder
      }
    }
    return RenderResult{request.scale, exact};
  }
  int renders() const { return d_renders; }

private:
  std::vector<arbc::ContentRef> d_inputs;
  int d_renders{0};
};

// A single-layer document over `content_id`; the resolver binds the id to a
// concrete `Content` the caller owns (the runtime binding, kept out of L4).
arbc::ObjectId add_single_layer(arbc::Model& model, arbc::ObjectId& comp) {
  auto txn = model.transact();
  const arbc::ObjectId content_id = txn.add_content(0);
  const arbc::ObjectId layer = txn.add_layer(content_id, arbc::Affine::identity());
  comp = txn.add_composition(512, 512);
  txn.attach_layer(comp, layer);
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
  arbc::ObjectId comp{};
  const arbc::ObjectId content_id = add_single_layer(model, comp);
  const arbc::DocStatePtr state = model.current();
  const auto resolver = [&](arbc::ObjectId id) -> arbc::Content* {
    return id == content_id ? &content : nullptr;
  };
  const arbc::Viewport viewport{512, 512, arbc::Affine::identity(), comp};
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
  arbc::ObjectId comp{};
  const arbc::ObjectId content_id = add_single_layer(model, comp);
  const arbc::DocStatePtr state = model.current();
  MarkBackend backend;
  AsyncContent content;
  const auto resolver = [&](arbc::ObjectId id) -> arbc::Content* {
    return id == content_id ? &content : nullptr;
  };
  const arbc::Viewport viewport{256, 256, arbc::Affine::identity(), comp}; // 1 rung-0 tile
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
  arbc::ObjectId comp{};
  const arbc::ObjectId content_id = add_single_layer(model, comp);
  const arbc::DocStatePtr state = model.current();
  const arbc::Viewport viewport{512, 512, arbc::Affine::identity(), comp};
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
  arbc::ObjectId comp{};
  const arbc::ObjectId content_id = add_single_layer(model, comp);
  const arbc::DocStatePtr state = model.current();
  const auto resolver = [&](arbc::ObjectId id) -> arbc::Content* {
    return id == content_id ? &content : nullptr;
  };
  const arbc::Viewport viewport{512, 512, arbc::Affine::identity(), comp};
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
  CHECK(stats.requests_suppressed == counters.requests_suppressed());
  // Appended last, after the cache block, so every existing positional aggregate init
  // keeps its meaning -- and asserted, because a mis-ordered positional init is exactly
  // the kind of mistake that compiles clean and reports the wrong number forever.
  CHECK(stats.renders_coalesced == counters.renders_coalesced());
}

// --- In-flight dedup, at the driver seam (compositor.in_flight_tile_dedup) ----

// The duplicate this task removes, in the shape the tree actually produces it: two
// operator layers SHARING one input leaf, over a viewport wider than one tile. Each
// operator's per-tile render pulls the leaf's covering tile; the first pull
// dispatches it to a "worker" (the deferring dispatch below) and the second finds it
// already in flight -- absent from the cache, and so, on the cache alone,
// indistinguishable from a tile nobody has ever asked for.
//
// The identity is the point: `requests_issued` counts exactly the DISTINCT tile keys
// the frame needed, and `requests_suppressed` counts the duplicates -- a POSITIVE
// number, so the test fails if the dedup silently stops firing rather than passing
// vacuously on a number that did not grow.
//
// enforces: 02-architecture#in-flight-tile-is-not-redispatched
// enforces: 16-sdlc-and-quality#compositor-exposes-behavioral-counters
TEST_CASE("counters: two operator layers sharing an input dispatch its tiles once, not twice") {
  MarkBackend backend;

  // The shared leaf: it defers (a worker-backed miss), so its render is IN FLIGHT
  // for the whole frame -- the state the guard exists to see.
  AsyncContent leaf;
  PullOperator op_a(&leaf);
  PullOperator op_b(&leaf);

  arbc::Model model;
  arbc::ObjectId a_id{};
  arbc::ObjectId b_id{};
  arbc::ObjectId comp{};
  {
    auto txn = model.transact();
    a_id = txn.add_content(0);
    const arbc::ObjectId a_layer = txn.add_layer(a_id, arbc::Affine::identity());
    b_id = txn.add_content(0);
    const arbc::ObjectId b_layer = txn.add_layer(b_id, arbc::Affine::identity());
    comp = txn.add_composition(512, 512);
    txn.attach_layer(comp, a_layer);
    txn.attach_layer(comp, b_layer);
    REQUIRE(txn.commit().has_value());
  }
  const arbc::ObjectId leaf_id{99};
  const arbc::DocStatePtr state = model.current();
  const auto resolver = [&](arbc::ObjectId id) -> arbc::Content* {
    if (id == a_id) {
      return &op_a;
    }
    return id == b_id ? &op_b : nullptr;
  };

  const arbc::Viewport viewport{512, 512, arbc::Affine::identity(), comp};
  arbc::SurfacePool pool(backend);
  TileCache cache(64u * 1024 * 1024);
  auto target = backend.make_surface(512, 512, arbc::k_working_rgba32f);
  REQUIRE(target.has_value());

  CompositorCounters counters;
  RefinementQueue queue;

  // `direct_dispatch` renders every content on the frame thread, which reproduces the
  // shipped leaf-only rule exactly for this scene: an operator renders inline (and so
  // pulls its input from inside its own render), while `AsyncContent` -- the leaf --
  // returns nullopt and leaves its completion live, which is precisely the state a
  // worker-dispatched leaf is in, and the state the guard exists to see.
  const std::unordered_map<const arbc::Content*, arbc::ObjectId> ids{{&leaf, leaf_id}};
  arbc::PullConfig config;
  config.counters = &counters;
  config.pending = &queue;
  config.id_of = [&ids](const arbc::Content* c) {
    const auto it = ids.find(c);
    return it != ids.end() ? it->second : arbc::ObjectId{};
  };
  config.contribution = [rev = state->revision()](const arbc::Content*) { return rev; };
  arbc::PullServiceImpl pulls(cache, backend, arbc::direct_dispatch(), config);
  op_a.service = &pulls;
  op_b.service = &pulls;

  arbc::render_frame_interactive(*state, resolver, viewport, cache, backend, pool, **target,
                                 arbc::Deadline::none(), std::nullopt, &queue, &counters, nullptr,
                                 arbc::Time::zero(), nullptr, nullptr, &pulls);

  // The frame needed 12 distinct tile keys: one per output tile for each of the two
  // operator layers (4 + 4), plus the leaf's four covering tiles -- pulled by BOTH
  // operators, dispatched once.
  constexpr std::uint64_t k_operator_tiles = 2 * k_tiles_covered;
  constexpr std::uint64_t k_distinct_keys = k_operator_tiles + k_tiles_covered;
  CHECK(counters.requests_issued() == k_distinct_keys);
  CHECK(counters.operator_renders() == k_operator_tiles);

  // ...and suppressed exactly the duplicates: the second operator's four pulls of the
  // leaf tiles the first operator already had in flight. Without the guard these are
  // four more `content->render` calls, four more surfaces, four more PendingTiles --
  // for pixels that were already being rendered.
  CHECK(counters.requests_suppressed() == k_tiles_covered);
  CHECK(leaf.renders() == static_cast<int>(k_tiles_covered));

  // One PendingTile per distinct leaf tile -- the suppressed pulls recorded nothing.
  // (The operators settled inexact transient placeholders inline, so they queue
  // nothing.)
  CHECK(queue.tiles.size() == k_tiles_covered);
}

// --- The refinement wave, at the driver seam ---------------------------------
// (compositor.operator_refinement_wave_amplification)

// `nested_deep` in miniature, and the whole of the 2x worker slowdown: a 3-deep operator
// chain (a top operator over two mids, each over one leaf) whose two leaves are dispatched
// to workers and arrive in DIFFERENT frames. Without the gate the chain is re-driven once
// per arrival -- twice here, and N times for N leaves, which is what makes the recursive
// case cost more than the flat one it is supposed to match (doc 05:151-153).
//
// The identity is derived, not copied (Decision 1): the inline oracle is the WRONG
// right-hand side and no coalescing scheme can reach it, because the operator's first
// render is HOW its inputs get requested at all -- the driver does not know an operator's
// input tiles, it discovers them by rendering it and watching it pull. So on a cold cache
// with a worker pool, every operator renders exactly TWICE (once to request its inputs and
// paint the placeholder, once to compose when the wave lands) and every leaf exactly once.
// Two is the floor, not the waste.
//
// And it is asserted POSITIVELY: `renders_coalesced() > 0` is the witness that the gate is
// on the live path at all. Without it every assertion here is "a number did not grow",
// which is equally what you observe when the gate never fires -- the exact failure mode
// `compositor.in_flight_tile_dedup` shipped and was bitten by.
//
// enforces: 02-architecture#refinement-wave-coalesces-chain-rerender
// enforces: 16-sdlc-and-quality#compositor-exposes-behavioral-counters
TEST_CASE("counters: a nested chain over two async leaves re-renders once per wave, not once "
          "per arrival") {
  MarkBackend backend;

  AsyncContent leaf_a;
  AsyncContent leaf_b;
  ChainOperator mid_a({&leaf_a});
  ChainOperator mid_b({&leaf_b});
  ChainOperator top({&mid_a, &mid_b});

  arbc::Model model;
  arbc::ObjectId comp{};
  const arbc::ObjectId top_id = add_single_layer(model, comp);
  const arbc::DocStatePtr state = model.current();
  const auto resolver = [&](arbc::ObjectId id) -> arbc::Content* {
    return id == top_id ? &top : nullptr;
  };

  const arbc::Viewport viewport{512, 512, arbc::Affine::identity(), comp};
  arbc::SurfacePool pool(backend);
  TileCache cache(64u * 1024 * 1024);
  auto target = backend.make_surface(512, 512, arbc::k_working_rgba32f);
  REQUIRE(target.has_value());

  CompositorCounters counters;
  RefinementQueue queue;

  // `direct_dispatch` reproduces the shipped leaf-only rule exactly for this scene: the
  // operators render inline on the frame thread (so each pulls its inputs from inside its
  // own render), while `AsyncContent` -- the leaves -- return nullopt and leave their
  // completions live, which is precisely the state a worker-dispatched leaf is in.
  const std::unordered_map<const arbc::Content*, arbc::ObjectId> ids{{&top, top_id},
                                                                     {&mid_a, arbc::ObjectId{51}},
                                                                     {&mid_b, arbc::ObjectId{52}},
                                                                     {&leaf_a, arbc::ObjectId{53}},
                                                                     {&leaf_b, arbc::ObjectId{54}}};
  arbc::PullConfig config;
  config.counters = &counters;
  config.pending = &queue;
  config.id_of = [&ids](const arbc::Content* c) {
    const auto it = ids.find(c);
    return it != ids.end() ? it->second : arbc::ObjectId{};
  };
  config.contribution = [rev = state->revision()](const arbc::Content*) { return rev; };
  arbc::PullServiceImpl pulls(cache, backend, arbc::direct_dispatch(), config);
  top.service = &pulls;
  mid_a.service = &pulls;
  mid_b.service = &pulls;

  const auto frame = [&] {
    arbc::render_frame_interactive(*state, resolver, viewport, cache, backend, pool, **target,
                                   arbc::Deadline::none(), std::nullopt, &queue, &counters, nullptr,
                                   arbc::Time::zero(), nullptr, nullptr, &pulls);
  };
  // Settle exactly the pending tiles of one leaf -- the independently-arriving input.
  const auto settle = [&](const arbc::ObjectId leaf) {
    for (arbc::PendingTile& pending : queue.tiles) {
      if (pending.key.content == leaf && !pending.done->settled()) {
        pending.done->complete(RenderResult{1.0, /*exact=*/true});
      }
    }
  };

  // Frame 1: the whole chain renders once, top-down, and bottoms out on two async leaves.
  // Three operators over four tiles each, plus one dispatch per leaf tile -- exactly the
  // five renders per tile the inline oracle needs, and no more.
  constexpr std::uint64_t k_chain_depth = 3; // top, mid_a, mid_b
  constexpr std::uint64_t k_oracle_operator_renders = k_chain_depth * k_tiles_covered;
  constexpr std::uint64_t k_oracle_requests = k_oracle_operator_renders + 2 * k_tiles_covered;
  frame();
  REQUIRE(counters.requests_issued() == k_oracle_requests);
  REQUIRE(counters.operator_renders() == k_oracle_operator_renders);
  REQUIRE(counters.renders_coalesced() == 0); // nothing has arrived yet -- nothing to coalesce
  REQUIRE(queue.tiles.size() == 2 * k_tiles_covered); // both leaves, all tiles, in flight
  REQUIRE(queue.waits.size() == k_chain_depth * k_tiles_covered); // one wave per operator tile

  // The FIRST leaf lands. Its damage would re-drive the chain -- and used to. The top's
  // recorded wave still names leaf_b's tiles, so the gate holds: the chain is not
  // re-rendered, and the frame composites the transient placeholder it already has.
  settle(arbc::ObjectId{53});
  arbc::poll_refinements(queue, cache, &counters);
  const std::uint64_t issued_after_first = counters.requests_issued();
  frame();
  CHECK(counters.requests_issued() == issued_after_first); // the partial arrival drove NOTHING
  CHECK(counters.operator_renders() == k_oracle_operator_renders);
  CHECK(counters.renders_coalesced() == k_tiles_covered); // one per deferred top tile
  CHECK(top.renders() == static_cast<int>(k_tiles_covered));

  // The LAST leaf lands: the wave is over, and the chain renders exactly once more.
  settle(arbc::ObjectId{54});
  arbc::poll_refinements(queue, cache, &counters);
  frame();
  CHECK(queue.tiles.empty()); // quiesced: nothing left in flight
  CHECK(queue.waits.empty()); // ...and every wave pruned

  // The coalescing identity, at quiescence. Every leaf rendered exactly once; every
  // operator exactly twice -- once to request, once to compose.
  CHECK(counters.operator_renders() == 2 * k_oracle_operator_renders);
  CHECK(counters.requests_issued() == k_oracle_requests + k_oracle_operator_renders);
  CHECK(leaf_a.renders() == static_cast<int>(k_tiles_covered));
  CHECK(leaf_b.renders() == static_cast<int>(k_tiles_covered));
  // The gate fired, and it is not the dedup: these operators share no input tile, so
  // there is nothing in flight for the in-flight guard to suppress.
  CHECK(counters.renders_coalesced() > 0);
  CHECK(counters.requests_suppressed() == 0);
}
