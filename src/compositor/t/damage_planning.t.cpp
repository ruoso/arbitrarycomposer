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
#include <arbc/compositor/tile_planning.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/model/damage.hpp>
#include <arbc/model/model.hpp>
#include <arbc/surface/backend.hpp>
#include <arbc/surface/surface.hpp>
#include <arbc/surface/surface_error.hpp>
#include <arbc/surface/surface_pool.hpp>
#include <arbc/surface/testing/counting_backend.hpp>
#include <arbc/surface/testing/stub_backend.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

// Unit tests for the pure damage planner (doc 16:46). `map_damage_to_device`
// and `clock_advance_damage` run against a hand-built scene with no backend;
// `invalidate_damage` against a hand-populated `TileCache`; the driver-gating
// seam against a stub backend/content pinning counts (doc 16:54-62), never
// timings. The end-to-end byte-exact composite is golden'd in
// `tests/damage_planning_golden.t.cpp`.

namespace {

using arbc::CompositorCounters;
using arbc::Damage;
using arbc::DirtyRegion;
using arbc::PriorityClass;
using arbc::Rect;
using arbc::RenderResult;
using arbc::ScaleRung;
using arbc::Stability;
using arbc::TileCache;
using arbc::TileCoord;
using arbc::TileKey;
using arbc::TileValue;

// A do-nothing Surface: the planner/invalidator reads only tile geometry and
// metadata, never pixels.
class StubSurface : public arbc::Surface {
public:
  int width() const override { return arbc::k_tile_size; }
  int height() const override { return arbc::k_tile_size; }
  arbc::SurfaceFormat format() const override { return arbc::k_working_rgba32f; }
  std::span<std::byte> cpu_bytes() override { return {}; }
  std::span<const std::byte> cpu_bytes() const override { return {}; }
};

// A CPU-buffer surface so the driver-gating counter tests exercise a real
// composite path (the counts, not the bytes, are under test here).
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

// Allocates real-buffer surfaces; composite/clear touch bytes but count nothing
// themselves -- `CompositorCounters` is the counter under test.
class MarkBackend : public arbc::testing::StubBackend {
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
};

// Content of a fixed stability that answers synchronously, exact, at scale.
class StubContent : public arbc::Content {
public:
  explicit StubContent(Stability stability) : d_stability(stability) {}
  std::optional<Rect> bounds() const override { return Rect{0.0, 0.0, 512.0, 512.0}; }
  Stability stability() const override { return d_stability; }
  std::optional<arbc::TimeRange> time_extent() const override {
    return d_stability == Stability::Static
               ? std::optional<arbc::TimeRange>(std::nullopt)
               : std::optional<arbc::TimeRange>(arbc::TimeRange::all());
  }
  std::optional<RenderResult> render(const arbc::RenderRequest& request,
                                     std::shared_ptr<arbc::RenderCompletion> /*done*/) override {
    return RenderResult{request.scale, /*exact=*/true};
  }

private:
  Stability d_stability;
};

// A single identity-transform layer bound to a fresh content id.
arbc::ObjectId add_layer(arbc::Model& model, const arbc::Affine& transform, arbc::ObjectId& comp) {
  auto txn = model.transact();
  const arbc::ObjectId content_id = txn.add_content(0);
  const arbc::ObjectId layer = txn.add_layer(content_id, transform);
  comp = txn.add_composition(512, 512);
  txn.attach_layer(comp, layer);
  REQUIRE(txn.commit().has_value());
  return content_id;
}

// Resident-tile helper: insert returns a pinned hold; discarding it unpins so
// the entry stays resident and lookup-able (a hand-populated cache).
void put_tile(TileCache& cache, arbc::ObjectId content, ScaleRung rung, TileCoord coord) {
  const TileKey key{content, /*revision=*/1, rung, coord, /*achieved_time=*/std::nullopt};
  cache.insert(key, TileValue{std::make_unique<StubSurface>(), {arbc::rung_scale(rung), true}},
               /*bytes=*/1, PriorityClass::Visible);
}

bool resident(TileCache& cache, arbc::ObjectId content, ScaleRung rung, TileCoord coord) {
  const TileKey key{content, /*revision=*/1, rung, coord, /*achieved_time=*/std::nullopt};
  return cache.lookup(key).has_value();
}

// Rasterize a rect set into a per-device-pixel coverage COUNT over the viewport: how
// many rects of the set contain each pixel. Disjointness is then "no cell above 1",
// and union-equality is "the same cells are non-zero".
//
// Asserting on the cover rather than on a particular rect list is deliberate. The
// contract `repaint_regions` makes is about the set of PIXELS, not about the band
// sweep's particular seams -- a test that pinned the seams would over-fit the
// decomposition and fail the next time it is improved without anything having gone
// wrong.
std::vector<int> coverage(const std::vector<Rect>& rects, int dim) {
  const Rect device_rect = Rect::from_size(static_cast<double>(dim), static_cast<double>(dim));
  std::vector<int> grid(static_cast<std::size_t>(dim) * static_cast<std::size_t>(dim), 0);
  for (const Rect& r : rects) {
    const Rect clipped = r.intersect(device_rect);
    if (clipped.empty()) {
      continue;
    }
    for (int y = static_cast<int>(clipped.y0); y < static_cast<int>(clipped.y1); ++y) {
      for (int x = static_cast<int>(clipped.x0); x < static_cast<int>(clipped.x1); ++x) {
        ++grid[(static_cast<std::size_t>(y) * static_cast<std::size_t>(dim)) +
               static_cast<std::size_t>(x)];
      }
    }
  }
  return grid;
}

// The reference cover: the pixels the (viewport-clipped, rounded-out) INPUT rects
// cover. `repaint_regions`' union must equal this EXACTLY -- not a superset (that is
// the waste the task is paying down) and emphatically not a subset (that is a stale
// seam: a pixel that looks undamaged and is not).
std::vector<int> input_coverage(const DirtyRegion& dirty, int dim) {
  const Rect device_rect = Rect::from_size(static_cast<double>(dim), static_cast<double>(dim));
  std::vector<Rect> rounded;
  for (const Rect& r : dirty.device_rects) {
    const Rect clipped = r.intersect(device_rect);
    if (clipped.empty()) {
      continue;
    }
    rounded.push_back(Rect{std::floor(clipped.x0), std::floor(clipped.y0), std::ceil(clipped.x1),
                           std::ceil(clipped.y1)});
  }
  return coverage(rounded, dim);
}

// Every promise `repaint_regions` makes, asserted at once on whatever it returned
// (`compositor.disjoint_dirty_repaint` Constraints 1-3): the rects are pairwise
// disjoint, integer-aligned and inside the viewport; their union is exactly the input
// union; and their bounding box is `repaint_region`, which is what ties the new
// function to the old one and makes the cap fallback coherent rather than a special
// case.
std::vector<Rect> checked_regions(const DirtyRegion& dirty, const arbc::Viewport& viewport) {
  const std::vector<Rect> regions = arbc::repaint_regions(dirty, viewport);
  const int dim = viewport.width;
  const Rect device_rect = Rect::from_size(static_cast<double>(dim), static_cast<double>(dim));

  Rect box{}; // empty accumulator (empty = identity under rect_union)
  for (const Rect& r : regions) {
    CHECK_FALSE(r.empty());
    CHECK(r.x0 == std::floor(r.x0));
    CHECK(r.y0 == std::floor(r.y0));
    CHECK(r.x1 == std::floor(r.x1));
    CHECK(r.y1 == std::floor(r.y1));
    CHECK(r == r.intersect(device_rect)); // inside the viewport
    box = arbc::rect_union(box, r);
  }
  CHECK(box == arbc::repaint_region(dirty, viewport));

  const std::vector<int> got = coverage(regions, dim);
  const std::vector<int> want = input_coverage(dirty, dim);
  std::size_t overlapped = 0;
  std::size_t mismatched = 0;
  for (std::size_t at = 0; at < got.size(); ++at) {
    if (got[at] > 1) {
      ++overlapped;
    }
    if ((got[at] > 0) != (want[at] > 0)) {
      ++mismatched;
    }
  }
  // Disjoint: no device pixel is cleared twice or composited twice.
  CHECK(overlapped == 0);
  // Union-exact: every damaged pixel is in exactly one repaint rect, and no undamaged
  // pixel is in any of them.
  CHECK(mismatched == 0);
  return regions;
}

} // namespace

// A 512x512 identity scene splits into a 2x2 grid of 256^2 rung-0 tiles.
constexpr std::uint64_t k_tiles_covered = 4;

// enforces: 02-architecture#damage-maps-to-device-dirty-regions
TEST_CASE("map_damage_to_device projects content damage through the camera to device rects") {
  arbc::Model model;
  arbc::ObjectId comp{};
  const arbc::ObjectId content = add_layer(model, arbc::Affine::identity(), comp);
  const arbc::DocStatePtr state = model.current();
  const arbc::Time now = arbc::Time::zero();

  SECTION("identity camera maps the content rect one-to-one") {
    const arbc::Viewport viewport{512, 512, arbc::Affine::identity(), comp};
    const Damage d{content, Rect{10.0, 20.0, 110.0, 120.0}, arbc::TimeRange::all()};
    const std::vector<Rect> rects =
        arbc::map_damage_to_device(*state, viewport, std::span(&d, 1), now);
    REQUIRE(rects.size() == 1);
    CHECK(rects[0] == Rect{10.0, 20.0, 110.0, 120.0});
  }

  SECTION("a scaled camera maps the rect correspondingly") {
    const arbc::Viewport viewport{512, 512, arbc::Affine::scaling(2.0, 2.0), comp};
    const Damage d{content, Rect{10.0, 20.0, 110.0, 120.0}, arbc::TimeRange::all()};
    const std::vector<Rect> rects =
        arbc::map_damage_to_device(*state, viewport, std::span(&d, 1), now);
    REQUIRE(rects.size() == 1);
    CHECK(rects[0] == Rect{20.0, 40.0, 220.0, 240.0});
  }

  SECTION("damage wholly outside the viewport clips to empty and is dropped") {
    const arbc::Viewport viewport{512, 512, arbc::Affine::identity(), comp};
    const Damage d{content, Rect{600.0, 600.0, 700.0, 700.0}, arbc::TimeRange::all()};
    CHECK(arbc::map_damage_to_device(*state, viewport, std::span(&d, 1), now).empty());
  }

  SECTION("structural infinite damage maps to the full viewport rect") {
    const arbc::Viewport viewport{512, 512, arbc::Affine::identity(), comp};
    const Damage d{content, Rect::infinite(), arbc::TimeRange::all()};
    const std::vector<Rect> rects =
        arbc::map_damage_to_device(*state, viewport, std::span(&d, 1), now);
    REQUIRE(rects.size() == 1);
    CHECK(rects[0] == Rect{0.0, 0.0, 512.0, 512.0});
  }

  SECTION("empty input yields an empty dirty region -- no damage, no work") {
    const arbc::Viewport viewport{512, 512, arbc::Affine::identity(), comp};
    CHECK(arbc::map_damage_to_device(*state, viewport, std::span<const Damage>(), now).empty());
  }
}

// enforces: 02-architecture#damage-maps-to-device-dirty-regions
TEST_CASE("map_damage_to_device gates on the displayed instant") {
  arbc::Model model;
  arbc::ObjectId comp{};
  const arbc::ObjectId content = add_layer(model, arbc::Affine::identity(), comp);
  const arbc::DocStatePtr state = model.current();
  const arbc::Viewport viewport{512, 512, arbc::Affine::identity(), comp};
  const Rect rect{10.0, 20.0, 110.0, 120.0};

  SECTION("a range excluding now drops the damage") {
    const Damage d{content, rect, arbc::TimeRange{arbc::Time{10}, arbc::Time{20}}};
    CHECK(arbc::map_damage_to_device(*state, viewport, std::span(&d, 1), arbc::Time{5}).empty());
  }

  SECTION("a range containing now keeps the damage") {
    const Damage d{content, rect, arbc::TimeRange{arbc::Time{10}, arbc::Time{20}}};
    CHECK(arbc::map_damage_to_device(*state, viewport, std::span(&d, 1), arbc::Time{15}).size() ==
          1);
  }

  SECTION("a degenerate instant range (refinement-arrival shape) is present-frame damage") {
    // TimeRange{when, when} is empty under TimeRange::empty(); the gate reads it
    // as this instant, not no-time, so an async arrival is never dropped.
    const Damage d{content, rect, arbc::TimeRange{arbc::Time{7}, arbc::Time{7}}};
    CHECK(arbc::map_damage_to_device(*state, viewport, std::span(&d, 1), arbc::Time{999}).size() ==
          1);
  }
}

namespace {

// A two-level displayed tree plus an undisplayed sibling, exposing every id the
// walk-path match domain covers (`runtime.placement_damage_maps_to_device`):
//
//   anchor comp ── leaf_layer  (content = leaf_content)
//              └── group_layer (content = child_comp)
//                    child comp ── inner_layer (content = inner_content)
//   other comp  ── other_layer (content = other_content)   [not displayed]
struct WalkPathScene {
  arbc::ObjectId anchor;
  arbc::ObjectId leaf_layer;
  arbc::ObjectId leaf_content;
  arbc::ObjectId group_layer;
  arbc::ObjectId child_comp;
  arbc::ObjectId inner_layer;
  arbc::ObjectId inner_content;
  arbc::ObjectId other_comp;
  arbc::ObjectId other_layer;
};

WalkPathScene build_walk_path_scene(arbc::Model& model) {
  auto txn = model.transact();
  WalkPathScene s{};
  s.leaf_content = txn.add_content(0);
  s.inner_content = txn.add_content(0);
  const arbc::ObjectId other_content = txn.add_content(0);

  s.anchor = txn.add_composition(512, 512);
  s.child_comp = txn.add_composition(512, 512);
  s.other_comp = txn.add_composition(512, 512);

  s.leaf_layer = txn.add_layer(s.leaf_content, arbc::Affine::identity());
  txn.attach_layer(s.anchor, s.leaf_layer);
  s.group_layer = txn.add_layer(s.child_comp, arbc::Affine::identity());
  txn.attach_layer(s.anchor, s.group_layer);
  s.inner_layer = txn.add_layer(s.inner_content, arbc::Affine::identity());
  txn.attach_layer(s.child_comp, s.inner_layer);
  s.other_layer = txn.add_layer(other_content, arbc::Affine::identity());
  txn.attach_layer(s.other_comp, s.other_layer);

  REQUIRE(txn.commit().has_value());
  return s;
}

// The shape every placement/membership mutator flushes (model.cpp: `Damage{id,
// Rect::infinite(), TimeRange::all()}`, claim rows 22/29).
Damage structural(arbc::ObjectId object) {
  return Damage{object, Rect::infinite(), arbc::TimeRange::all()};
}

std::vector<Rect> map_one(const arbc::DocRoot& state, const arbc::Viewport& viewport,
                          const Damage& d) {
  return arbc::map_damage_to_device(state, viewport, std::span(&d, 1), arbc::Time::zero());
}

} // namespace

// enforces: 02-architecture#placement-damage-maps-to-device
// enforces: 02-architecture#damage-maps-to-device-dirty-regions
TEST_CASE("map_damage_to_device matches structural damage against every walk-path node id") {
  arbc::Model model;
  const WalkPathScene s = build_walk_path_scene(model);
  const arbc::Viewport viewport{512, 512, arbc::Affine::identity(), s.anchor};
  const Rect full{0.0, 0.0, 512.0, 512.0};

  SECTION("a leaf layer's own id (placement damage) maps to the full viewport") {
    const arbc::DocStatePtr state = model.current();
    const std::vector<Rect> rects = map_one(*state, viewport, structural(s.leaf_layer));
    REQUIRE(rects.size() == 1);
    CHECK(rects[0] == full);
  }

  SECTION("a descended group layer's id maps to the full viewport") {
    const arbc::DocStatePtr state = model.current();
    const std::vector<Rect> rects = map_one(*state, viewport, structural(s.group_layer));
    REQUIRE(rects.size() == 1);
    CHECK(rects[0] == full);
  }

  SECTION("a descended child composition's id (membership damage inside it) maps to the full "
          "viewport") {
    const arbc::DocStatePtr state = model.current();
    const std::vector<Rect> rects = map_one(*state, viewport, structural(s.child_comp));
    REQUIRE(rects.size() == 1);
    CHECK(rects[0] == full);
  }

  SECTION("the anchor composition's id (membership damage on the anchor) maps to the full "
          "viewport") {
    const arbc::DocStatePtr state = model.current();
    const std::vector<Rect> rects = map_one(*state, viewport, structural(s.anchor));
    REQUIRE(rects.size() == 1);
    CHECK(rects[0] == full);
  }

  SECTION("a leaf layer inside the descended child composition maps too") {
    const arbc::DocStatePtr state = model.current();
    const std::vector<Rect> rects = map_one(*state, viewport, structural(s.inner_layer));
    REQUIRE(rects.size() == 1);
    CHECK(rects[0] == full);
  }

  SECTION("a node this viewport does not display contributes nothing -- per-viewport isolation") {
    const arbc::DocStatePtr state = model.current();
    // Damage in the displayed tree maps to nothing on a viewport anchored elsewhere...
    const arbc::Viewport other_viewport{512, 512, arbc::Affine::identity(), s.other_comp};
    CHECK(map_one(*state, other_viewport, structural(s.leaf_layer)).empty());
    CHECK(map_one(*state, other_viewport, structural(s.group_layer)).empty());
    CHECK(map_one(*state, other_viewport, structural(s.child_comp)).empty());
    CHECK(map_one(*state, other_viewport, structural(s.anchor)).empty());
    // ...and the undisplayed sibling's ids map to nothing on THIS viewport.
    CHECK(map_one(*state, viewport, structural(s.other_comp)).empty());
    CHECK(map_one(*state, viewport, structural(s.other_layer)).empty());
  }

  SECTION("a layer-id match bypasses the visible gate -- the edit that hid the layer still "
          "repaints; its content edit does not") {
    {
      auto txn = model.transact();
      txn.set_visible(s.leaf_layer, false);
      REQUIRE(txn.commit().has_value());
    }
    const arbc::DocStatePtr state = model.current();
    const std::vector<Rect> rects = map_one(*state, viewport, structural(s.leaf_layer));
    REQUIRE(rects.size() == 1);
    CHECK(rects[0] == full);
    // A CONTENT edit on the now-invisible layer keeps the gate: no pixels shown,
    // nothing contributed -- exactly the pre-existing behavior.
    CHECK(map_one(*state, viewport, structural(s.leaf_content)).empty());
  }

  SECTION("a layer-id match bypasses the opacity gate the same way") {
    {
      auto txn = model.transact();
      txn.set_opacity(s.leaf_layer, 0.0);
      REQUIRE(txn.commit().has_value());
    }
    const arbc::DocStatePtr state = model.current();
    const std::vector<Rect> rects = map_one(*state, viewport, structural(s.leaf_layer));
    REQUIRE(rects.size() == 1);
    CHECK(rects[0] == full);
    CHECK(map_one(*state, viewport, structural(s.leaf_content)).empty());
  }

  SECTION("the descent hook fires before pruning: a hidden group still matches; nodes strictly "
          "inside its pruned subtree do not") {
    {
      auto txn = model.transact();
      txn.set_visible(s.group_layer, false);
      REQUIRE(txn.commit().has_value());
    }
    const arbc::DocStatePtr state = model.current();
    // The edit that hid the group must repaint the pixels the subtree occupied.
    REQUIRE(map_one(*state, viewport, structural(s.group_layer)).size() == 1);
    // The child composition id is reported by the same pre-pruning hook.
    REQUIRE(map_one(*state, viewport, structural(s.child_comp)).size() == 1);
    // An edit STRICTLY inside the hidden subtree changed nothing that was drawn
    // (refinement Decision 2's soundness note): unreported, correctly.
    CHECK(map_one(*state, viewport, structural(s.inner_layer)).empty());
  }

  SECTION("a finite rect on a layer-keyed record maps through the composed transform -- one "
          "projection rule, no dead special case") {
    const arbc::DocStatePtr state = model.current();
    // No producer emits this today (placement mutators flush Rect::infinite());
    // Decision 3 pins the uniform rule regardless.
    const Damage d{s.leaf_layer, Rect{10.0, 20.0, 110.0, 120.0}, arbc::TimeRange::all()};
    const std::vector<Rect> rects = map_one(*state, viewport, d);
    REQUIRE(rects.size() == 1);
    CHECK(rects[0] == Rect{10.0, 20.0, 110.0, 120.0});
  }
}

// enforces: 11-time-and-video#clock-advance-damages-only-moving-layers
// enforces: 11-time-and-video#static-tiles-survive-clock
TEST_CASE("clock_advance_damage damages only the moving layers") {
  arbc::Model model;
  arbc::ObjectId static_content;
  arbc::ObjectId timed_content;
  arbc::ObjectId comp{};
  {
    auto txn = model.transact();
    static_content = txn.add_content(0);
    timed_content = txn.add_content(0);
    const arbc::ObjectId static_layer = txn.add_layer(static_content, arbc::Affine::identity());
    const arbc::ObjectId timed_layer = txn.add_layer(timed_content, arbc::Affine::identity());
    comp = txn.add_composition(512, 512);
    txn.attach_layer(comp, static_layer);
    txn.attach_layer(comp, timed_layer);
    REQUIRE(txn.commit().has_value());
  }
  const arbc::DocStatePtr state = model.current();
  StubContent still(Stability::Static);
  StubContent moving(Stability::Timed);
  const auto resolver = [&](arbc::ObjectId id) -> arbc::Content* {
    if (id == static_content) {
      return &still;
    }
    if (id == timed_content) {
      return &moving;
    }
    return nullptr;
  };
  const arbc::Viewport viewport{512, 512, arbc::Affine::identity(), comp};
  const arbc::TimeRange advanced{arbc::Time{0}, arbc::Time{100}};

  SECTION("a Static+Timed scene emits exactly one damage for the Timed layer") {
    const std::vector<Damage> damage =
        arbc::clock_advance_damage(*state, resolver, viewport, advanced);
    REQUIRE(damage.size() == 1);
    CHECK(damage[0].object == timed_content);
    CHECK(damage[0].rect == Rect::infinite());
    CHECK(damage[0].range == advanced);
  }

  SECTION("an all-Static scene emits nothing -- still tiles survive the clock") {
    const auto static_only = [&](arbc::ObjectId) -> arbc::Content* { return &still; };
    CHECK(arbc::clock_advance_damage(*state, static_only, viewport, advanced).empty());
  }

  SECTION("an empty advance emits nothing") {
    const arbc::TimeRange none{arbc::Time{5}, arbc::Time{5}};
    CHECK(arbc::clock_advance_damage(*state, resolver, viewport, none).empty());
  }
}

// enforces: 02-architecture#damage-invalidates-by-content-region-across-rungs
TEST_CASE("invalidate_damage drops the damaged content's tiles across rungs") {
  TileCache cache(64u * 1024 * 1024);
  const arbc::ObjectId damaged{1};
  const arbc::ObjectId other{2};
  const ScaleRung rung0{0}; // cell 256 local units
  const ScaleRung rung1{1}; // cell 128 local units (finer)

  // Two tiles of `damaged` intersecting region [0,0,100,100] at different rungs,
  // one `damaged` tile outside the region, and one `other`-content tile.
  put_tile(cache, damaged, rung0, TileCoord{0, 0});   // footprint [0,0,256,256]
  put_tile(cache, damaged, rung1, TileCoord{0, 0});   // footprint [0,0,128,128]
  put_tile(cache, damaged, rung0, TileCoord{10, 10}); // footprint [2560,..] -- outside
  put_tile(cache, other, rung0, TileCoord{0, 0});

  const std::uint64_t evictions_before = cache.evictions();

  const Damage region_damage{damaged, Rect{0.0, 0.0, 100.0, 100.0}, arbc::TimeRange::all()};
  const std::size_t dropped = arbc::invalidate_damage(cache, std::span(&region_damage, 1));

  CHECK(dropped == 2);
  CHECK_FALSE(resident(cache, damaged, rung0, TileCoord{0, 0}));
  CHECK_FALSE(resident(cache, damaged, rung1, TileCoord{0, 0}));
  CHECK(resident(cache, damaged, rung0, TileCoord{10, 10})); // outside region: survives
  CHECK(resident(cache, other, rung0, TileCoord{0, 0}));     // other content: survives
  CHECK(cache.evictions() == evictions_before);              // drops are not evictions

  // Structural infinite region routes to the wholesale drop: the last `damaged`
  // tile goes, `other` survives.
  const Damage structural{damaged, Rect::infinite(), arbc::TimeRange::all()};
  CHECK(arbc::invalidate_damage(cache, std::span(&structural, 1)) == 1);
  CHECK_FALSE(resident(cache, damaged, rung0, TileCoord{10, 10}));
  CHECK(resident(cache, other, rung0, TileCoord{0, 0}));
  CHECK(cache.evictions() == evictions_before);
}

// enforces: 02-architecture#damage-maps-to-device-dirty-regions
TEST_CASE("render_frame_interactive gates work to the dirty region (counter-backed)") {
  MarkBackend backend;
  StubContent content(Stability::Static);
  arbc::Model model;
  arbc::ObjectId comp{};
  const arbc::ObjectId content_id = add_layer(model, arbc::Affine::identity(), comp);
  const arbc::DocStatePtr state = model.current();
  const auto resolver = [&](arbc::ObjectId id) -> arbc::Content* {
    return id == content_id ? &content : nullptr;
  };
  const arbc::Viewport viewport{512, 512, arbc::Affine::identity(), comp};
  arbc::SurfacePool pool(backend);

  auto drive = [&](const DirtyRegion* dirty, CompositorCounters& counters) {
    TileCache cache(64u * 1024 * 1024); // cold: every planned tile is a miss
    arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError> target =
        backend.make_surface(512, 512, arbc::k_working_rgba32f);
    REQUIRE(target.has_value());
    arbc::render_frame_interactive(*state, resolver, viewport, cache, backend, pool, **target,
                                   arbc::Deadline::none(), std::nullopt, nullptr, &counters, dirty);
  };

  SECTION("a dirty region covering one tile plans only that tile") {
    DirtyRegion dirty{{Rect{0.0, 0.0, 256.0, 256.0}}};
    CompositorCounters counters;
    drive(&dirty, counters);
    CHECK(counters.requests_issued() == 1);
    CHECK(counters.composites() == 1);
  }

  SECTION("a non-null empty dirty region plans nothing -- zero renders, zero composites") {
    DirtyRegion dirty{};
    CompositorCounters counters;
    drive(&dirty, counters);
    CHECK(counters.requests_issued() == 0);
    CHECK(counters.composites() == 0);
  }

  SECTION("a null dirty pointer plans the whole viewport") {
    CompositorCounters counters;
    drive(nullptr, counters);
    CHECK(counters.requests_issued() == k_tiles_covered);
    CHECK(counters.composites() == k_tiles_covered);
  }
}

// enforces: 02-architecture#gated-frame-touches-only-its-repaint-region
TEST_CASE("repaint_region is the rounded-out bbox of the dirty rects, viewport-clipped") {
  const arbc::Viewport viewport{512, 512, arbc::Affine::identity()};

  SECTION("an empty region repaints nothing") {
    CHECK(arbc::repaint_region(DirtyRegion{}, viewport).empty());
  }

  SECTION("a single rect rounds OUT to whole device pixels") {
    // Rounding IN would leave a sub-pixel fringe of the true damage unpainted --
    // a stale seam. Out is conservative in the safe direction.
    const DirtyRegion dirty{{arbc::Rect{10.25, 20.75, 30.5, 40.1}}};
    CHECK(arbc::repaint_region(dirty, viewport) == arbc::Rect{10.0, 20.0, 31.0, 41.0});
  }

  SECTION("several rects collapse to their bounding box") {
    // Two disjoint corners repaint everything between them: waste, not
    // incorrectness (`compositor.disjoint_dirty_repaint` is where the precision
    // goes). One rect means one composite per tile, which is what keeps a tile in
    // an overlap of two dirty rects from being composited twice.
    const DirtyRegion dirty{{arbc::Rect{0.0, 0.0, 16.0, 16.0},
                             arbc::Rect{100.0, 200.0, 110.0, 210.0},
                             arbc::Rect{50.0, 50.0, 60.0, 60.0}}};
    CHECK(arbc::repaint_region(dirty, viewport) == arbc::Rect{0.0, 0.0, 110.0, 210.0});
  }

  SECTION("rects are clipped to the viewport, and a structural infinite rect saturates") {
    const DirtyRegion out_of_view{{arbc::Rect{600.0, 600.0, 700.0, 700.0}}};
    CHECK(arbc::repaint_region(out_of_view, viewport).empty());

    const DirtyRegion straddling{{arbc::Rect{480.0, 480.0, 900.0, 900.0}}};
    CHECK(arbc::repaint_region(straddling, viewport) == arbc::Rect{480.0, 480.0, 512.0, 512.0});

    // The whole-plane rect must clip to the viewport, not take the box to
    // infinity (it is absorbing under `rect_union`).
    const DirtyRegion structural{{arbc::Rect{4.0, 4.0, 8.0, 8.0}, arbc::Rect::infinite()}};
    CHECK(arbc::repaint_region(structural, viewport) == arbc::Rect{0.0, 0.0, 512.0, 512.0});
  }
}

// enforces: 02-architecture#disjoint-repaint-covers-damage-exactly-once
TEST_CASE("repaint_regions normalizes the dirty rects into a disjoint integer cover") {
  const arbc::Viewport viewport{64, 64, arbc::Affine::identity()};

  SECTION("an empty DirtyRegion yields an empty vector -- the no-damage-no-work root") {
    CHECK(arbc::repaint_regions(DirtyRegion{}, viewport).empty());
  }

  SECTION("rects outside the viewport contribute nothing") {
    const DirtyRegion dirty{{Rect{600.0, 600.0, 700.0, 700.0}}};
    CHECK(checked_regions(dirty, viewport).empty());
  }

  SECTION("disjoint rects pass through unchanged (up to round-out)") {
    // The common case must not pay for the band machinery: two separated damages come
    // back out as two rects, not as the three bands the sweep passes through (the
    // vertical coalesce is what collapses the middle gap band away).
    const DirtyRegion dirty{{Rect{2.0, 2.0, 6.0, 6.0}, Rect{10.0, 10.0, 14.0, 14.0}}};
    const std::vector<Rect> regions = checked_regions(dirty, viewport);
    REQUIRE(regions.size() == 2);
    CHECK(regions[0] == Rect{2.0, 2.0, 6.0, 6.0});
    CHECK(regions[1] == Rect{10.0, 10.0, 14.0, 14.0});
  }

  SECTION("overlapping rects are split into a disjoint cover") {
    // The classic L-shape. The overlap belongs to exactly ONE output rect, which is
    // what keeps a translucent tile covering it from being composited -- and blended
    // -- twice. `checked_regions` asserts the coverage grid, not a particular set of
    // cuts.
    const DirtyRegion dirty{{Rect{0.0, 0.0, 10.0, 10.0}, Rect{5.0, 5.0, 15.0, 15.0}}};
    const std::vector<Rect> regions = checked_regions(dirty, viewport);
    CHECK(regions.size() > 1); // it really decomposed rather than boxing
  }

  SECTION("a contained rect is absorbed") {
    const DirtyRegion dirty{{Rect{0.0, 0.0, 20.0, 20.0}, Rect{5.0, 5.0, 10.0, 10.0}}};
    const std::vector<Rect> regions = checked_regions(dirty, viewport);
    REQUIRE(regions.size() == 1);
    CHECK(regions[0] == Rect{0.0, 0.0, 20.0, 20.0});
  }

  SECTION("identical rects collapse -- the (damage, layer) duplicate") {
    // `map_damage_to_device` emits one rect per (damage, layer) pair, so two layers
    // showing the same damaged content emit the SAME device rect. This is the most
    // common real overlap, and it must not become two repaint rects (which would clear
    // and composite those pixels twice).
    const DirtyRegion dirty{{Rect{4.0, 4.0, 12.0, 12.0}, Rect{4.0, 4.0, 12.0, 12.0}}};
    const std::vector<Rect> regions = checked_regions(dirty, viewport);
    REQUIRE(regions.size() == 1);
    CHECK(regions[0] == Rect{4.0, 4.0, 12.0, 12.0});
  }

  SECTION("a structural infinite rect saturates to the viewport") {
    // Clipping to the viewport BEFORE the sweep is what keeps `Rect::infinite()` from
    // taking the round-out to a non-representable integer.
    const DirtyRegion structural{{Rect::infinite()}};
    const std::vector<Rect> saturated = checked_regions(structural, viewport);
    REQUIRE(saturated.size() == 1);
    CHECK(saturated[0] == Rect{0.0, 0.0, 64.0, 64.0});

    // ... and mixed with a small rect it SUBSUMES it, rather than being cut by it.
    const DirtyRegion mixed{{Rect{4.0, 4.0, 8.0, 8.0}, Rect::infinite()}};
    const std::vector<Rect> regions = checked_regions(mixed, viewport);
    REQUIRE(regions.size() == 1);
    CHECK(regions[0] == Rect{0.0, 0.0, 64.0, 64.0});
  }

  SECTION("rects are rounded OUT, not in") {
    // The dirty rects are `Affine::map_rect` outputs -- arbitrary doubles. Rounding in
    // would leave a sub-pixel fringe of the true damage unpainted: a stale seam.
    const DirtyRegion dirty{{Rect{10.25, 20.75, 30.5, 40.1}}};
    const std::vector<Rect> regions = checked_regions(dirty, viewport);
    REQUIRE(regions.size() == 1);
    CHECK(regions[0] == Rect{10.0, 20.0, 31.0, 41.0}); // strictly contains the input
  }
}

// enforces: 02-architecture#disjoint-repaint-covers-damage-exactly-once
TEST_CASE("repaint_regions falls back to the bbox over the rect-count cap") {
  const arbc::Viewport viewport{512, 512, arbc::Affine::identity()};

  // A diagonal staircase of mutually-overlapping rects: every y-band's merged x-run is
  // shifted one pixel from the band below it, so nothing coalesces and the sweep emits
  // one rect per band -- the O(n^2) shape Decision 3's cap exists for. The input is not
  // exotic: one rect per (damage, layer) pair over a many-damage, many-layer commit
  // reaches these counts.
  DirtyRegion dirty;
  for (int at = 0; at < 70; ++at) {
    const double edge = static_cast<double>(at);
    dirty.device_rects.push_back(Rect{edge, edge, edge + 2.0, edge + 2.0});
  }

  // The fallback is not an approximation: it is the shipped, byte-exact bounding-box
  // behavior, whose union is a SUPERSET of the disjoint set's -- so a pathological
  // input degrades to the status quo, never to missed damage.
  const std::vector<Rect> regions = arbc::repaint_regions(dirty, viewport);
  REQUIRE(regions.size() == 1);
  CHECK(regions[0] == arbc::repaint_region(dirty, viewport));
  CHECK(regions[0] == Rect{0.0, 0.0, 71.0, 71.0});
}

// enforces: 02-architecture#disjoint-repaint-skips-the-undamaged-gap
// enforces: 02-architecture#interactive-still-scene-schedules-no-frame
TEST_CASE("a disjoint repaint set skips the undamaged gap (counter-backed)") {
  MarkBackend mark;
  arbc::testing::CountingBackend backend(mark); // tallies the clip-scoped ops
  StubContent content(Stability::Static);
  arbc::Model model;
  arbc::ObjectId comp{};
  const arbc::ObjectId content_id = add_layer(model, arbc::Affine::identity(), comp);
  const arbc::DocStatePtr state = model.current();
  const auto resolver = [&](arbc::ObjectId id) -> arbc::Content* {
    return id == content_id ? &content : nullptr;
  };
  const arbc::Viewport viewport{512, 512, arbc::Affine::identity(), comp};
  arbc::SurfacePool pool(backend);

  // The cache is the CALLER's: a `LayerTilePlan` surfaced through `visible_plans`
  // retains its tiles' cache holds, so a sink outliving the cache it was planned
  // against would dangle. Each call gets a cold cache -- every planned tile a miss --
  // but the caller owns its lifetime.
  auto drive = [&](TileCache& cache, const DirtyRegion* dirty, CompositorCounters& counters,
                   std::vector<arbc::LayerTilePlan>* plans = nullptr) {
    backend.reset();
    arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError> target =
        backend.make_surface(512, 512, arbc::k_working_rgba32f);
    REQUIRE(target.has_value());
    arbc::render_frame_interactive(*state, resolver, viewport, cache, backend, pool, **target,
                                   arbc::Deadline::none(), std::nullopt, nullptr, &counters, dirty,
                                   arbc::Time::zero(), plans);
  };

  SECTION("two far-apart damages do not repaint the gap between them") {
    // The headline scenario from the WBS note: two small damages at opposite corners of
    // the viewport. Their bounding box is the WHOLE viewport, so the pre-task gate
    // re-planned and re-composited every tile between them to refresh 512 pixels of
    // actual damage.
    const DirtyRegion dirty{{Rect{0.0, 0.0, 16.0, 16.0}, Rect{496.0, 496.0, 512.0, 512.0}}};
    TileCache split_cache(64u * 1024 * 1024);
    CompositorCounters split;
    drive(split_cache, &dirty, split);
    const int split_clears = backend.clear_rect_calls;

    // The same damage forced through the bounding box: a one-rect `DirtyRegion` holding
    // exactly `repaint_region`'s box normalizes to itself, so this frame IS the pre-task
    // behavior -- which is also what `repaint_regions` returns over the cap.
    const DirtyRegion boxed{{arbc::repaint_region(dirty, viewport)}};
    REQUIRE(arbc::repaint_regions(boxed, viewport).size() == 1);
    TileCache box_cache(64u * 1024 * 1024);
    CompositorCounters box;
    drive(box_cache, &boxed, box);
    const int box_clears = backend.clear_rect_calls;

    CHECK(split.composites() == 2);               // the two corner tiles, nothing between
    CHECK(box.composites() == k_tiles_covered);   // every tile of the 2x2 grid
    CHECK(split.composites() < box.composites()); // strictly less work, same pixels
    CHECK(split.requests_issued() == 2);          // and it renders only what it paints
    CHECK(box.requests_issued() == k_tiles_covered);
    CHECK(split_clears == 2); // one clear_rect per repaint rect ...
    CHECK(box_clears == 1);   // ... where the bbox clears the whole viewport, once
  }

  SECTION("a tile straddling two repaint rects renders once and composites once per rect") {
    // Two damages inside ONE tile: the repaint set is two disjoint rects and the tile is
    // planned from both. Planning is idempotent and cheap (a cache and pending-set
    // lookup); the merge by tile coord is what makes it RENDER once. Compositing must
    // happen once per rect -- that is how the tile's pixels reach both -- and each
    // composite is scissored to its own rect, so no pixel is written twice.
    const DirtyRegion dirty{{Rect{10.0, 10.0, 20.0, 20.0}, Rect{10.0, 100.0, 20.0, 110.0}}};
    REQUIRE(arbc::repaint_regions(dirty, viewport).size() == 2);

    TileCache cache(64u * 1024 * 1024);
    CompositorCounters counters;
    std::vector<arbc::LayerTilePlan> plans; // destroyed before `cache`: it holds cache pins
    drive(cache, &dirty, counters, &plans);

    CHECK(counters.requests_issued() == 1); // planned twice, rendered ONCE
    CHECK(counters.composites() == 2);      // ... and composited once per repaint rect
    // Not by the pending-set guard: the per-rect plans are merged by tile coord BEFORE
    // dispatch, so the duplicate plan never reaches a dispatch site to be suppressed.
    // (The guard is still the backstop for a tile two LAYERS want in one frame.)
    CHECK(counters.requests_suppressed() == 0);
    CHECK(backend.clear_rect_calls == 2);
    // Per-rect planning naturally produces N plans for one layer. They are merged, so
    // `visible_plans` keeps its contract: one entry per planned layer, in composite
    // order -- not one per (layer, rect) (`compositor.expose_visible_plan`).
    REQUIRE(plans.size() == 1);
    CHECK(plans[0].tiles.size() == 1);
  }

  SECTION("an empty DirtyRegion clears nothing, plans nothing, composites nothing") {
    // The invariant the whole idle-viewport story rests on (doc 02:51). The empty rect
    // vector makes every per-rect loop in the frame run zero times -- including the
    // clear, which must not degenerate into a whole-target one.
    const DirtyRegion dirty{};
    TileCache cache(64u * 1024 * 1024);
    CompositorCounters counters;
    drive(cache, &dirty, counters);
    CHECK(counters.requests_issued() == 0);
    CHECK(counters.composites() == 0);
    CHECK(backend.clear_rect_calls == 0);
    CHECK(backend.clear_calls == 0);
  }
}

// enforces: 02-architecture#disjoint-repaint-skips-the-undamaged-gap
TEST_CASE("a layer lying in the undamaged gap is not planned and not exposed") {
  MarkBackend backend;
  StubContent content(Stability::Static);
  arbc::Model model;
  arbc::ObjectId near_id{};
  arbc::ObjectId far_id{};
  arbc::ObjectId comp{};
  {
    auto txn = model.transact();
    near_id = txn.add_content(0);
    const arbc::ObjectId near_layer = txn.add_layer(near_id, arbc::Affine::identity());
    far_id = txn.add_content(0);
    const arbc::ObjectId far_layer = txn.add_layer(far_id, arbc::Affine::translation(300.0, 300.0));
    comp = txn.add_composition(512, 512);
    txn.attach_layer(comp, near_layer);
    txn.attach_layer(comp, far_layer);
    REQUIRE(txn.commit().has_value());
  }
  const arbc::DocStatePtr state = model.current();
  const auto resolver = [&](arbc::ObjectId id) -> arbc::Content* {
    return (id == near_id || id == far_id) ? &content : nullptr;
  };
  const arbc::Viewport viewport{512, 512, arbc::Affine::identity(), comp};
  arbc::SurfacePool pool(backend);

  TileCache cache(64u * 1024 * 1024);
  arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError> target =
      backend.make_surface(512, 512, arbc::k_working_rgba32f);
  REQUIRE(target.has_value());

  // Damage the top-left corner only. The near layer covers it; the far layer, offset by
  // 300 device pixels, meets NO repaint rect -- so it is not planned, not composited,
  // and not pushed to `visible_plans`, which holds this frame's *planned* layers. Under
  // the bounding-box gate a layer could only ever miss the one rect; now it must miss
  // every rect, and that is the branch under test.
  const DirtyRegion dirty{{Rect{0.0, 0.0, 16.0, 16.0}}};
  CompositorCounters counters;
  std::vector<arbc::LayerTilePlan> plans;
  arbc::render_frame_interactive(*state, resolver, viewport, cache, backend, pool, **target,
                                 arbc::Deadline::none(), std::nullopt, nullptr, &counters, &dirty,
                                 arbc::Time::zero(), &plans);

  CHECK(counters.requests_issued() == 1);
  CHECK(counters.composites() == 1);
  REQUIRE(plans.size() == 1);
  CHECK(plans[0].content == near_id);
}
