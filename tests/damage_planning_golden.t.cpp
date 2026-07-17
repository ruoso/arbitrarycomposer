#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/compositor/compositor.hpp>
#include <arbc/compositor/counters.hpp>
#include <arbc/compositor/damage_planning.hpp>
#include <arbc/compositor/tile_planning.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/media/pixel_format.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/runtime/document.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <vector>

// Byte-exact cross-component golden for the damage planner (doc 16:47-53, doc
// 02:51 "no damage -> no work"). The pure mapping/gating logic is unit-tested in
// `src/compositor/t/damage_planning.t.cpp`; here we drive the end-to-end
// resolve+composite path through the CPU backend and pin two promises:
//   1. A damaged-region re-render, gated to the damaged layer's device dirty
//      rect (from `map_damage_to_device`), is BYTE-IDENTICAL to a full re-render
//      of the same scene -- the gated frame composites the damaged tiles
//      identically while the untouched region survives from the persisted target
//      and the warm cache, with no seam or double-blend.
//   2. A quiescent frame (empty `DirtyRegion`) leaves the target unchanged
//      byte-for-byte and issues zero renders / zero composites (the behavioral
//      class, doc 16:54-62).

namespace {

// An opaque solid layer. The bottom (background) layer covers the whole
// viewport, so any dirty region it is gated to is fully, opaquely re-covered --
// which is what makes the un-cleared gated composite reproduce a cleared full
// re-render byte-for-byte (opaque source-over is a replace).
struct Scene {
  arbc::Document document;
  arbc::ObjectId background;  // bottom: opaque, full-viewport
  arbc::ObjectId foreground;  // top: opaque, top-left tile only
  arbc::ObjectId front_layer; // the foreground's layer: the placement-edit target
  arbc::ObjectId comp;        // the composition the frame walk is anchored at

  Scene() {
    background = document.add_content(std::make_shared<arbc::SolidContent>(
        arbc::Rgba{0.75F, 0.1F, 0.1F, 1.0F}, arbc::Rect{0.0, 0.0, 512.0, 512.0}));
    const arbc::ObjectId back_layer = document.add_layer(background, arbc::Affine::identity());
    foreground = document.add_content(std::make_shared<arbc::SolidContent>(
        arbc::Rgba{0.1F, 0.2F, 0.75F, 1.0F}, arbc::Rect{0.0, 0.0, 256.0, 256.0}));
    front_layer = document.add_layer(foreground, arbc::Affine::identity());
    // Attach both layers (creation/bottom-to-top order) to a composition so the
    // composition-scoped frame walk draws them (doc 05:28-36).
    comp = document.add_composition(512.0, 512.0);
    document.attach_layer(comp, back_layer);
    document.attach_layer(comp, front_layer);
  }
};

// Captures the model-flushed damage batches, so the placement golden below is
// gated by the AUTO-EMITTED damage (claim row 29) -- nothing hand-forged.
struct CollectSink final : arbc::DamageSink {
  std::vector<arbc::Damage> records;
  void flush(const std::vector<arbc::Damage>& damage) override {
    records.insert(records.end(), damage.begin(), damage.end());
  }
};

bool byte_identical(const arbc::Surface& lhs, const arbc::Surface& rhs) {
  const std::span<const float> a =
      std::as_const(lhs).span<arbc::PixelFormat::Rgba32fLinearPremul>();
  const std::span<const float> b =
      std::as_const(rhs).span<arbc::PixelFormat::Rgba32fLinearPremul>();
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size_bytes()) == 0;
}

std::vector<float> snapshot(const arbc::Surface& surface) {
  const std::span<const float> px =
      std::as_const(surface).span<arbc::PixelFormat::Rgba32fLinearPremul>();
  return {px.begin(), px.end()};
}

} // namespace

// enforces: 16-sdlc-and-quality#byte-exact-goldens
// enforces: 02-architecture#damage-maps-to-device-dirty-regions
TEST_CASE("damage golden: a gated damaged-region re-render is byte-identical to a full re-render") {
  arbc::CpuBackend backend;

  // The reference: a full render of the (post-damage) scene into its own target.
  Scene ref_scene;
  // Anchor the direct frame walk at the scene's composition (identical for both
  // scenes here, doc 05:28-36).
  const arbc::Viewport viewport{512, 512, arbc::Affine::identity(), ref_scene.comp};
  const arbc::DocStatePtr ref_state = ref_scene.document.pin();
  const auto ref_resolver = [&](arbc::ObjectId id) { return ref_scene.document.resolve(id); };
  arbc::SurfacePool ref_pool(backend);
  arbc::TileCache ref_cache(64u * 1024 * 1024);
  arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError> reference =
      backend.make_surface(512, 512, arbc::k_working_rgba32f);
  REQUIRE(reference.has_value());
  arbc::render_frame_interactive(*ref_state, ref_resolver, viewport, ref_cache, backend, ref_pool,
                                 **reference, arbc::Deadline::none(), std::nullopt);

  // The gated scene: full render into the persisted target, then damage the
  // foreground and re-render gated to its device dirty rect.
  Scene scene;
  const arbc::DocStatePtr state = scene.document.pin();
  const auto resolver = [&](arbc::ObjectId id) { return scene.document.resolve(id); };
  arbc::SurfacePool pool(backend);
  arbc::TileCache cache(64u * 1024 * 1024);
  arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError> target =
      backend.make_surface(512, 512, arbc::k_working_rgba32f);
  REQUIRE(target.has_value());

  // Frame 1: full render -> persisted target, warm cache. Matches the reference.
  arbc::render_frame_interactive(*state, resolver, viewport, cache, backend, pool, **target,
                                 arbc::Deadline::none(), std::nullopt);
  REQUIRE(byte_identical(**reference, **target));

  // Damage the foreground layer's region: drop its tiles and map the damage to a
  // device dirty rect through the same camera.
  const std::vector<arbc::Damage> damage{
      arbc::Damage{scene.foreground, arbc::Rect{0.0, 0.0, 256.0, 256.0}, arbc::TimeRange::all()}};
  CHECK(arbc::invalidate_damage(cache, damage) >= 1);
  const std::vector<arbc::Rect> device_rects =
      arbc::map_damage_to_device(*state, viewport, damage, arbc::Time::zero());
  REQUIRE(device_rects.size() == 1);
  const arbc::DirtyRegion dirty{device_rects};

  // Frame 2: gated re-render onto the persisted target. Only tiles intersecting
  // the dirty rect are re-planned/composited; the rest survive. Byte-identical
  // to the full re-render -- no seam, no double-blend.
  arbc::render_frame_interactive(*state, resolver, viewport, cache, backend, pool, **target,
                                 arbc::Deadline::none(), std::nullopt, nullptr, nullptr, &dirty);
  REQUIRE(byte_identical(**reference, **target));
}

// enforces: 16-sdlc-and-quality#byte-exact-goldens
// enforces: 02-architecture#placement-damage-maps-to-device
TEST_CASE("damage golden: a placement-edit-gated repaint is byte-identical to a full re-render "
          "of the edited scene") {
  arbc::CpuBackend backend;
  const arbc::Affine moved = arbc::Affine::translation(128.0, 64.0);

  // The reference: a full render of the POST-EDIT scene (foreground already moved).
  Scene ref_scene;
  ref_scene.document.set_layer_transform(ref_scene.front_layer, moved);
  const arbc::Viewport viewport{512, 512, arbc::Affine::identity(), ref_scene.comp};
  const arbc::DocStatePtr ref_state = ref_scene.document.pin();
  const auto ref_resolver = [&](arbc::ObjectId id) { return ref_scene.document.resolve(id); };
  arbc::SurfacePool ref_pool(backend);
  arbc::TileCache ref_cache(64u * 1024 * 1024);
  arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError> reference =
      backend.make_surface(512, 512, arbc::k_working_rgba32f);
  REQUIRE(reference.has_value());
  arbc::render_frame_interactive(*ref_state, ref_resolver, viewport, ref_cache, backend, ref_pool,
                                 **reference, arbc::Deadline::none(), std::nullopt);

  // The gated scene: full render at the ORIGINAL placement into the persisted
  // target, warm cache -- then the placement edit, gated by its own auto-damage.
  Scene scene;
  const auto resolver = [&](arbc::ObjectId id) { return scene.document.resolve(id); };
  arbc::SurfacePool pool(backend);
  arbc::TileCache cache(64u * 1024 * 1024);
  arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError> target =
      backend.make_surface(512, 512, arbc::k_working_rgba32f);
  REQUIRE(target.has_value());
  {
    const arbc::DocStatePtr before = scene.document.pin();
    arbc::render_frame_interactive(*before, resolver, viewport, cache, backend, pool, **target,
                                   arbc::Deadline::none(), std::nullopt);
  }

  // The edit. The model flushes `Damage{front_layer, Rect::infinite(), all()}`
  // (claim row 29); the sink captures it verbatim -- nothing is hand-forged.
  CollectSink sink;
  scene.document.set_damage_sink(&sink);
  scene.document.set_layer_transform(scene.front_layer, moved);
  scene.document.set_damage_sink(nullptr);
  REQUIRE(sink.records.size() == 1);
  CHECK(sink.records[0].object == scene.front_layer);

  // Placement damage repaints, never invalidates: the layer-keyed record matches
  // no TileKey.content, so the warm tiles all stay resident.
  CHECK(arbc::invalidate_damage(cache, sink.records) == 0);

  // The layer-keyed record maps to the full viewport of the displaying viewport.
  const arbc::DocStatePtr state = scene.document.pin();
  const std::vector<arbc::Rect> device_rects =
      arbc::map_damage_to_device(*state, viewport, sink.records, arbc::Time::zero());
  REQUIRE(device_rects.size() == 1);
  CHECK(device_rects[0] == arbc::Rect{0.0, 0.0, 512.0, 512.0});
  const arbc::DirtyRegion dirty{device_rects};

  // The gated repaint re-composites the resident tiles through the new placement,
  // byte-identical to the full re-render of the edited scene (doc 16: goldens,
  // no tolerances).
  arbc::render_frame_interactive(*state, resolver, viewport, cache, backend, pool, **target,
                                 arbc::Deadline::none(), std::nullopt, nullptr, nullptr, &dirty);
  REQUIRE(byte_identical(**reference, **target));
}

// enforces: 16-sdlc-and-quality#byte-exact-goldens
// enforces: 11-time-and-video#clock-advance-damages-only-moving-layers
TEST_CASE("damage golden: a quiescent frame does nothing") {
  arbc::CpuBackend backend;

  Scene scene;
  const arbc::Viewport viewport{512, 512, arbc::Affine::identity(), scene.comp};
  const arbc::DocStatePtr state = scene.document.pin();
  const auto resolver = [&](arbc::ObjectId id) { return scene.document.resolve(id); };
  arbc::SurfacePool pool(backend);
  arbc::TileCache cache(64u * 1024 * 1024);
  arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError> target =
      backend.make_surface(512, 512, arbc::k_working_rgba32f);
  REQUIRE(target.has_value());

  // Full render -> persisted target, then snapshot its bytes.
  arbc::render_frame_interactive(*state, resolver, viewport, cache, backend, pool, **target,
                                 arbc::Deadline::none(), std::nullopt);
  const std::vector<float> before = snapshot(**target);

  // No damage: a non-null empty dirty region plans nothing.
  const arbc::DirtyRegion empty{};
  arbc::CompositorCounters counters;
  arbc::render_frame_interactive(*state, resolver, viewport, cache, backend, pool, **target,
                                 arbc::Deadline::none(), std::nullopt, nullptr, &counters, &empty);

  const std::vector<float> after = snapshot(**target);
  CHECK(before == after); // target unchanged byte-for-byte
  CHECK(counters.requests_issued() == 0);
  CHECK(counters.composites() == 0);
}
