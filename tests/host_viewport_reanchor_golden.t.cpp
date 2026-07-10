#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/compositor/anchored_viewports.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/model/model.hpp>
#include <arbc/runtime/host_viewport.hpp>
#include <arbc/runtime/interactive.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

// ULP-exact probe golden for the runtime-held anchor-path zoom-out
// (`arbc::HostViewport`, doc 04:62-69). Mirrors `anchored_viewports_golden.t.cpp`
// and the tolerance of the landed `#rebase-preserves-composed-appearance` claim:
// re-anchoring only RE-EXPRESSES the same camera in a new anchor frame, so the
// composed device-space image of a probe point is preserved across the switch to
// within one double rounding (doc 04:66), and a zoom-in then zoom-out of the same
// descent edge restores the original `(anchor, matrix)`. Doc 04:66's continuity is
// a few-ULP property, not byte-exact, so the assertion is a ULP bound (the
// anchored-viewports header, lines 14-19, draws exactly this distinction).

namespace {

using arbc::Affine;
using arbc::compose;
using arbc::ObjectId;
using arbc::Vec2;
using arbc::Viewport;

constexpr double k_canvas = 1000.0;
constexpr double k_center = 500.0;
constexpr double k_level_scale = 0.001;

Affine level_edge() {
  return compose(Affine::translation(k_center - 0.5, k_center - 0.5),
                 Affine::scaling(k_level_scale, k_level_scale));
}

Affine frame_camera(double s) {
  return compose(Affine::translation(k_center, k_center),
                 compose(Affine::scaling(s, s), Affine::translation(-k_center, -k_center)));
}

std::vector<ObjectId> build_chain(arbc::Model& model, int levels) {
  std::vector<ObjectId> comps;
  auto txn = model.transact();
  const ObjectId leaf = txn.add_content(0);
  for (int i = 0; i <= levels; ++i) {
    comps.push_back(txn.add_composition(k_canvas, k_canvas));
  }
  const ObjectId leaf_layer = txn.add_layer(leaf, Affine::identity());
  txn.attach_layer(comps[static_cast<std::size_t>(levels)], leaf_layer);
  for (int i = 0; i < levels; ++i) {
    const ObjectId l = txn.add_layer(comps[static_cast<std::size_t>(i + 1)], level_edge());
    txn.attach_layer(comps[static_cast<std::size_t>(i)], l);
  }
  REQUIRE(txn.commit().has_value());
  return comps;
}

bool within_one_rounding(Vec2 a, Vec2 b) {
  const double eps = std::numeric_limits<double>::epsilon();
  const double tol_x = 8.0 * eps * std::max(1.0, std::abs(a.x));
  const double tol_y = 8.0 * eps * std::max(1.0, std::abs(a.y));
  return std::abs(a.x - b.x) <= tol_x && std::abs(a.y - b.y) <= tol_y;
}

arbc::HostViewport::Clock epoch_clock() {
  return [] { return std::chrono::steady_clock::time_point{}; };
}

} // namespace

// enforces: 04-transforms-and-infinite-zoom#zoom-out-reanchors-along-anchor-path
TEST_CASE("host_viewport zoom-out re-anchor preserves the composed probe to within one rounding") {
  arbc::CpuBackend backend;
  arbc::Model model;
  const std::vector<ObjectId> comps = build_chain(model, /*levels=*/1);
  const auto resolve = [](ObjectId) -> arbc::Content* { return nullptr; };
  arbc::SurfacePool pool(backend);
  arbc::TileCache cache(64u * 1024 * 1024);
  auto target = backend.make_surface(1000, 1000, arbc::k_working_rgba32f);
  REQUIRE(target.has_value());
  arbc::InteractiveRenderer renderer({}, epoch_clock());

  const double s = arbc::k_reanchor_scale_threshold * 2.0;
  const Affine c0 = frame_camera(s);
  arbc::HostViewport::Config cfg;
  cfg.viewport = Viewport{1000, 1000, c0, comps[0]};
  arbc::HostViewport viewport(renderer, model, resolve, backend, pool, cache, **target,
                              epoch_clock(), cfg);

  const Vec2 probe{k_center, k_center};

  // Zoom in: the probe's device position through the OLD anchor (camera . edge)
  // equals its position through the rebased camera, to within one double rounding.
  viewport.step();
  REQUIRE(viewport.anchor() == comps[1]);
  const Vec2 in_old = c0.apply(level_edge().apply(probe));
  const Vec2 in_new = viewport.camera().apply(probe);
  CHECK(within_one_rounding(in_old, in_new));

  // Zoom out: drive the camera below the band and step. The pop re-anchors upward
  // by inverting the stored descent edge; the probe's device position is preserved
  // across the switch (the same camera re-expressed in the ancestor frame).
  const Affine c_low = frame_camera(1.0 / (arbc::k_reanchor_scale_threshold * 2.0));
  viewport.set_camera(c_low);
  viewport.step();
  REQUIRE(viewport.anchor() == comps[0]);
  const std::optional<Affine> inv = level_edge().inverse();
  REQUIRE(inv.has_value());
  const Vec2 out_before = c_low.apply(inv->apply(probe));
  const Vec2 out_after = viewport.camera().apply(probe);
  CHECK(within_one_rounding(out_before, out_after));

  // Round-trip identity: zoom-in then zoom-out of the same descent edge restores
  // the original (anchor, matrix) image of the probe to within one double rounding.
  const Vec2 round_trip =
      arbc::reanchor_camera(arbc::reanchor_camera(c0, level_edge()), *inv).apply(probe);
  CHECK(within_one_rounding(round_trip, c0.apply(probe)));
}
