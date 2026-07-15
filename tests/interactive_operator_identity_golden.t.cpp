// The interactive driver's operator contract, pinned against the export driver
// (`runtime.interactive_pull_wiring`). Doc 02:40-41 promises "two drivers over the
// same core"; before this task `InteractiveRenderer::render_frame` passed a null
// `pulls` to `render_frame_interactive`, so every operator behavior the frame
// driver had grown was dead interactively:
//
//   * an identity endpoint (a crossfade at w == 0 / w == 1, a fade at envelope == 1)
//     short-circuited its render AND had no pull service to deliver the terminal
//     input's tiles through, so the tile kept its planned `Placeholder` and
//     composited as a transparent no-op -- a blank layer, where the SAME document
//     exported correctly through `SequenceRenderer`;
//   * any pull built without a production `id_of` keys every operator input under
//     the root `ObjectId{}`, collapsing a crossfade's two same-stability inputs onto
//     one tile key.
//
// Both are asserted here on the interactive driver, byte-exact against the export
// driver's frame of the same document at the same instant (the CPU backend is
// specified deterministic, doc 16 tier 3, and an unpressured `BestEffort` frame
// degrades nowhere). Pre-task every interactive frame below is fully transparent.
//
// This file stays confined to identity ENDPOINTS over leaf inputs, and deliberately so:
// an endpoint needs no attach at all (`identity()` reads params only, `inputs()` is
// core-owned structure, and the DRIVER, not the operator, issues the pull), so every
// frame below calls `render_frame` with the default -- UNBOUND -- `FrameBinding`. That is
// exactly what makes it the regression test for the driver-side delivery path in
// isolation: it would still pass if the binder were reverted. Interior weights, which run
// the operator's OWN `render` and can only pull through a service they received at attach,
// belong to `runtime.interactive_binder_wiring` and are covered in
// `interactive_operator_binding_golden.t.cpp`.

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/cache/keyed_store.hpp>
#include <arbc/compositor/compositor.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/kind_crossfade/crossfade_content.hpp>
#include <arbc/kind_fade/fade_content.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/media/pixel_format.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/interactive.hpp>
#include <arbc/runtime/offline_sequence.hpp>
#include <arbc/surface/surface.hpp>
#include <arbc/surface/surface_pool.hpp>

#include <catch2/catch_test_macros.hpp>

#include "support/root_anchor.hpp"

#include <chrono>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

using namespace arbc;

namespace {

// One rung-0 tile at scale 1.0, so an endpoint frame is a single planned tile.
constexpr int k_dim = 256;
constexpr auto k_budget = std::chrono::hours(1); // no deadline pressure: nothing degrades

Rect canvas() { return Rect{0.0, 0.0, static_cast<double>(k_dim), static_cast<double>(k_dim)}; }
Viewport viewport() { return Viewport{k_dim, k_dim, Affine::identity()}; }

// The loop's only clock is injected; a fake clock fixed at the steady_clock epoch
// puts every deadline instant in the real past, so no frame below reads a wall
// clock or blocks (doc 16:54-62). Every miss here answers inline anyway.
InteractiveRenderer::Clock epoch_clock() {
  return [] { return std::chrono::steady_clock::time_point{}; };
}

// A crossfade over [1000, 2000): w == 0 strictly before it, w == 1 at/after its end.
CrossfadeParams window_params() {
  return CrossfadeParams{CrossfadeShape::Linear, Time{1000}, Time{1000}};
}

std::vector<float> snapshot(const Surface& surface) {
  const std::span<const float> px = std::as_const(surface).span<PixelFormat::Rgba32fLinearPremul>();
  return {px.begin(), px.end()};
}

bool byte_identical(const std::vector<float>& a, const std::vector<float>& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

// The export driver's frame of `doc` at `time` -- the oracle every interactive frame
// below is compared against. `SequenceRenderer` binds the operators itself.
std::vector<float> exported_frame(Document& doc, Backend& backend, Time time) {
  SequenceRenderer renderer(doc, viewport(), backend);
  const expected<std::unique_ptr<Surface>, SurfaceError> frame = renderer.render_frame_at(time);
  REQUIRE(frame.has_value());
  CHECK(renderer.counters().operator_renders() == 0U); // the oracle is itself an endpoint
  return snapshot(**frame);
}

// One interactive frame of `doc` at `time` on a fresh renderer/cache, plus the
// counters that frame accumulated. A cold first frame plans the whole viewport.
struct InteractiveFrame {
  std::vector<float> pixels;
  std::uint64_t operator_renders{0};
  std::uint64_t degraded_composites{0};
  std::uint64_t composites{0};
};

InteractiveFrame interactive_frame(Document& doc, Backend& backend, Time time) {
  const DocStatePtr pin = doc.pin();
  const ContentResolver resolve = [&doc](ObjectId id) { return doc.resolve(id); };
  TileCache cache(64U * 1024 * 1024);
  SurfacePool pool(backend);
  const expected<std::unique_ptr<Surface>, SurfaceError> target =
      backend.make_surface(k_dim, k_dim, pin->working_space());
  REQUIRE(target.has_value());

  InteractiveRenderer renderer({}, epoch_clock());
  // `InteractiveRenderer::render_frame` renders the composition the Viewport anchors and
  // does not source the root itself, so anchor at the document's root composition.
  const Viewport vp{k_dim, k_dim, Affine::identity(), arbc::test::root_composition_of(*pin)};
  renderer.render_frame(*pin, resolve, vp, cache, backend, pool, **target, {}, time, k_budget);
  return InteractiveFrame{snapshot(**target), renderer.counters().operator_renders(),
                          renderer.counters().degraded_composites(),
                          renderer.counters().composites()};
}

} // namespace

// enforces: 13-effects-as-operators#identity-layer-delivers-input-to-frame
// enforces: 13-effects-as-operators#identity-plan-issues-no-operator-render
TEST_CASE("interactive: a crossfade identity endpoint delivers its input's pixels (byte-exact "
          "against the export driver)") {
  CpuBackend backend;
  SolidContent from{Rgba{0.5F, 0.25F, 0.125F, 1.0F}, canvas()};
  SolidContent to{Rgba{0.125F, 0.375F, 0.75F, 1.0F}, canvas()};

  // `Document` is non-movable and the crossfade borrows both solids non-owningly, so
  // all three outlive every render below.
  Document doc;
  const ObjectId comp = doc.add_composition(static_cast<double>(k_dim), static_cast<double>(k_dim));
  doc.attach_layer(comp, doc.add_layer(doc.add_content(std::make_shared<CrossfadeContent>(
                                           &from, &to, window_params())),
                                       Affine::identity()));

  SECTION("w == 0 serves input 0") {
    const InteractiveFrame frame = interactive_frame(doc, backend, Time{500});
    CHECK(byte_identical(frame.pixels, exported_frame(doc, backend, Time{500})));
    // The endpoint painted the layer: pre-task the tile kept its planned placeholder
    // and this frame was a fully transparent no-op.
    CHECK(frame.composites == 1U);
  }

  SECTION("w == 1 serves input 1") {
    const InteractiveFrame frame = interactive_frame(doc, backend, Time{2500});
    CHECK(byte_identical(frame.pixels, exported_frame(doc, backend, Time{2500})));
    CHECK(frame.composites == 1U);
  }
}

// enforces: 13-effects-as-operators#identity-layer-delivers-input-to-frame
// enforces: 13-effects-as-operators#identity-plan-issues-no-operator-render
TEST_CASE("interactive: a fade at envelope == 1 delivers its input's pixels (byte-exact against "
          "the export driver)") {
  CpuBackend backend;
  SolidContent solid{Rgba{0.25F, 0.5F, 0.75F, 1.0F}, canvas()};

  // No fade window at all: the envelope is 1 everywhere, so `identity()` returns
  // input 0 at every instant.
  Document doc;
  const ObjectId comp = doc.add_composition(static_cast<double>(k_dim), static_cast<double>(k_dim));
  doc.attach_layer(
      comp, doc.add_layer(doc.add_content(std::make_shared<FadeContent>(&solid, FadeParams{})),
                          Affine::identity()));

  const InteractiveFrame frame = interactive_frame(doc, backend, Time{500});
  CHECK(byte_identical(frame.pixels, exported_frame(doc, backend, Time{500})));
  CHECK(frame.composites == 1U);
}

// enforces: 13-effects-as-operators#identity-plan-issues-no-operator-render
TEST_CASE("interactive: an identity endpoint issues zero operator renders and degrades nowhere") {
  CpuBackend backend;
  SolidContent from{Rgba{0.5F, 0.25F, 0.125F, 1.0F}, canvas()};
  SolidContent to{Rgba{0.125F, 0.375F, 0.75F, 1.0F}, canvas()};

  Document doc;
  const ObjectId comp = doc.add_composition(static_cast<double>(k_dim), static_cast<double>(k_dim));
  doc.attach_layer(comp, doc.add_layer(doc.add_content(std::make_shared<CrossfadeContent>(
                                           &from, &to, window_params())),
                                       Affine::identity()));

  // The interactive twin of `tests/crossfade_identity_counter.t.cpp`: the identity
  // plan short-circuits, so the operator is never rendered, and the delivered tile is
  // the sole paint -- an unpressured BestEffort frame degrades nowhere (doc 16 tier 4:
  // behavioral counters, never a wall-clock assertion).
  const InteractiveFrame at_w0 = interactive_frame(doc, backend, Time{500});
  CHECK(at_w0.operator_renders == 0U);
  CHECK(at_w0.degraded_composites == 0U);

  const InteractiveFrame at_w1 = interactive_frame(doc, backend, Time{2500});
  CHECK(at_w1.operator_renders == 0U);
  CHECK(at_w1.degraded_composites == 0U);
}

// enforces: 13-effects-as-operators#operator-input-children-have-distinct-cache-identity
TEST_CASE("interactive: a crossfade's two inputs do not alias one cache key") {
  CpuBackend backend;
  SolidContent from{Rgba{0.5F, 0.25F, 0.125F, 1.0F}, canvas()};
  SolidContent to{Rgba{0.125F, 0.375F, 0.75F, 1.0F}, canvas()};

  Document doc;
  const ObjectId comp = doc.add_composition(static_cast<double>(k_dim), static_cast<double>(k_dim));
  doc.attach_layer(comp, doc.add_layer(doc.add_content(std::make_shared<CrossfadeContent>(
                                           &from, &to, window_params())),
                                       Affine::identity()));

  // ONE renderer, ONE cache, ONE revision. The inputs are Static, so their tile keys
  // omit `achieved_time` and are clock-invariant: frame 1's cached input-0 tile is
  // still resident when frame 2 pulls input 1. (The clock advance damages the
  // crossfade LAYER, which invalidates tiles keyed under the crossfade's own id --
  // of which an identity endpoint creates none -- and never the inputs', which are
  // keyed under their synthesized identities.)
  const DocStatePtr pin = doc.pin();
  const ContentResolver resolve = [&doc](ObjectId id) { return doc.resolve(id); };
  TileCache cache(64U * 1024 * 1024);
  SurfacePool pool(backend);
  const expected<std::unique_ptr<Surface>, SurfaceError> target =
      backend.make_surface(k_dim, k_dim, pin->working_space());
  REQUIRE(target.has_value());
  InteractiveRenderer renderer({}, epoch_clock());
  // The direct interactive driver renders the anchored composition; anchor at `comp`.
  const Viewport vp{k_dim, k_dim, Affine::identity(), comp};

  renderer.render_frame(*pin, resolve, vp, cache, backend, pool, **target, {}, Time{500}, k_budget);
  const std::vector<float> at_w0 = snapshot(**target);

  renderer.render_frame(*pin, resolve, vp, cache, backend, pool, **target, {}, Time{2500},
                        k_budget);
  const std::vector<float> at_w1 = snapshot(**target);

  // Under a missing or hand-rolled `PullConfig::id_of` both inputs key under the root
  // `ObjectId{}` at the same revision/rung/coord, so the w == 1 endpoint HITS input
  // 0's resident tile and the two frames come out byte-identical. Both checks below
  // fail loudly if the production `id_of` is ever dropped.
  CHECK_FALSE(byte_identical(at_w0, at_w1));
  CHECK(byte_identical(at_w1, exported_frame(doc, backend, Time{2500})));
  CHECK(renderer.counters().operator_renders() == 0U);
}
