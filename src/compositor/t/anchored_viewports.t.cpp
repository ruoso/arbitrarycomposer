#include <arbc/base/ids.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/compositor/anchored_viewports.hpp>
#include <arbc/model/model.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <vector>

// Numeric property tests for anchored viewports + rebasing (doc 04:49-86). The
// scenes are built against the real `Model` because rebasing and the outward
// walk descend the composition graph (`find_composition`/`for_each_layer_in`) --
// stub content is unnecessary; the geometry is what is under test.

namespace {

using arbc::Affine;
using arbc::compose;
using arbc::ObjectId;
using arbc::Vec2;
using arbc::Viewport;

// Every composition in the synthetic chains is a 1000x1000 canvas that nests its
// child centered at scale 1/1000 -- a ~1000x/level zoom, the doc 04:35-42
// pathological ratio. `edge` maps CHILD-local -> PARENT-local.
constexpr double k_canvas = 1000.0;
constexpr double k_center = 500.0;
constexpr double k_level_scale = 0.001; // child 1000 units -> 1 parent unit

Affine level_edge() {
  return compose(Affine::translation(k_center - 0.5, k_center - 0.5),
                 Affine::scaling(k_level_scale, k_level_scale));
}

// A camera framing a composition's center at scale `s`: maps local (500,500) to
// device (500,500) with `max_scale() == s`.
Affine frame_camera(double s) {
  return compose(Affine::translation(k_center, k_center),
                 compose(Affine::scaling(s, s), Affine::translation(-k_center, -k_center)));
}

// Build a chain of `levels + 1` nested compositions comps[0..levels]: comps[i]
// holds one layer placed by `level_edge()` whose content is comps[i+1]; the
// deepest holds a leaf-content layer. Returns the ordered composition ids.
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
  // Doc 04:66's "exact to within one double rounding": a few ULPs of the
  // coordinate magnitude -- the justified tolerance (doc 16:47-53) for a rebase,
  // which a byte-exact golden would over-specify (Decision 4).
  const double eps = std::numeric_limits<double>::epsilon();
  const double tol_x = 8.0 * eps * std::max(1.0, std::abs(a.x));
  const double tol_y = 8.0 * eps * std::max(1.0, std::abs(a.y));
  return std::abs(a.x - b.x) <= tol_x && std::abs(a.y - b.y) <= tol_y;
}

} // namespace

// enforces: 04-transforms-and-infinite-zoom#rebase-preserves-composed-appearance
TEST_CASE("re-anchoring across the threshold preserves the composed appearance") {
  arbc::Model model;
  const std::vector<ObjectId> comps = build_chain(model, /*levels=*/2);
  const arbc::DocStatePtr state = model.current();

  const double s = arbc::k_reanchor_scale_threshold * 2.0; // above the band -> zoom-in
  const Viewport before{1000, 1000, frame_camera(s), comps[0]};
  REQUIRE(arbc::rebase_need(before.camera.max_scale()) == arbc::RebaseNeed::zoom_in);

  const arbc::RebaseResult r = arbc::rebase(*state, before);
  REQUIRE(r.event.occurred);
  CHECK(r.event.from == comps[0]);
  CHECK(r.event.to == comps[1]);

  // A probe point in the NEW anchor's local space: its device position computed
  // through the old anchor (camera . edge) must equal the position through the
  // rebased camera to within one double rounding.
  const Vec2 probe{k_center, k_center};
  const Vec2 via_old = before.camera.apply(level_edge().apply(probe));
  const Vec2 via_new = r.viewport.camera.apply(probe);
  CHECK(within_one_rounding(via_old, via_new));

  // The pure re-anchor step is exactly the on-demand composition (doc 04:44-47).
  CHECK(r.viewport.camera == arbc::reanchor_camera(before.camera, level_edge()));
}

// enforces: 04-transforms-and-infinite-zoom#camera-well-conditioned-across-depth
TEST_CASE("the anchored camera stays well-conditioned across nesting depth") {
  constexpr int levels = 6;
  arbc::Model model;
  const std::vector<ObjectId> comps = build_chain(model, levels);
  const arbc::DocStatePtr state = model.current();

  const double s = arbc::k_reanchor_scale_threshold * 2.0;

  // Drive the viewport down the chain: at each level the user has zoomed to scale
  // `s` (above the band) on the current anchor, then rebases one level deeper.
  Viewport vp{1000, 1000, frame_camera(s), comps[0]};
  for (int i = 0; i < levels; ++i) {
    REQUIRE(arbc::rebase_need(vp.camera.max_scale()) == arbc::RebaseNeed::zoom_in);
    const arbc::RebaseResult r = arbc::rebase(*state, vp);
    REQUIRE(r.event.occurred);
    CHECK(r.event.to == comps[static_cast<std::size_t>(i + 1)]);
    vp = r.viewport;

    // Structure-bounded conditioning: the rebased anchor->device scale sits back
    // inside the well-conditioned band, independent of how deep we are.
    const double rebased = vp.camera.max_scale();
    CHECK(rebased <= arbc::k_reanchor_scale_threshold);
    CHECK(rebased >= 1.0 / arbc::k_reanchor_scale_threshold);

    if (i + 1 < levels) {
      vp.camera = frame_camera(s); // the user keeps zooming into the next child
    }
  }

  // The same deepest view through a SINGLE non-rebased root camera loses
  // conditioning: the composed comp[0]->comp[levels] mapping is 1000^levels, so
  // the flat camera's scale is astronomically outside the band.
  Affine chain = Affine::identity(); // comp[levels]-local -> comp[0]-local
  for (int i = 0; i < levels; ++i) {
    chain = compose(chain, level_edge());
  }
  const std::optional<Affine> chain_inv = chain.inverse();
  REQUIRE(chain_inv.has_value());
  const Affine flat = compose(frame_camera(s), *chain_inv);
  CHECK(flat.max_scale() > 1.0e12);
  CHECK(flat.max_scale() > arbc::k_reanchor_scale_threshold);
}

TEST_CASE("the conditioning test bands and the pure re-anchor step") {
  CHECK(arbc::rebase_need(1.0) == arbc::RebaseNeed::none);
  CHECK(arbc::rebase_need(arbc::k_reanchor_scale_threshold) == arbc::RebaseNeed::none);
  CHECK(arbc::rebase_need(arbc::k_reanchor_scale_threshold * 2.0) == arbc::RebaseNeed::zoom_in);
  CHECK(arbc::rebase_need(1.0 / (arbc::k_reanchor_scale_threshold * 2.0)) ==
        arbc::RebaseNeed::zoom_out);
  // A degenerate composed scale is a cull concern, not a rebase (doc 04:115-117).
  CHECK(arbc::rebase_need(0.0) == arbc::RebaseNeed::none);
  CHECK(arbc::rebase_need(std::numeric_limits<double>::quiet_NaN()) == arbc::RebaseNeed::none);

  // reanchor_camera is exactly on-demand composition: appearance-preserving by
  // construction (compose(C, E).apply(p) == C.apply(E.apply(p))).
  const Affine cam = frame_camera(4.0);
  const Affine edge = level_edge();
  const Vec2 p{123.0, 456.0};
  CHECK(within_one_rounding(arbc::reanchor_camera(cam, edge).apply(p), cam.apply(edge.apply(p))));
}

TEST_CASE("a viewport in-band or anchored at the root does not rebase") {
  arbc::Model model;
  const std::vector<ObjectId> comps = build_chain(model, /*levels=*/2);
  const arbc::DocStatePtr state = model.current();

  SECTION("in-band leaves the viewport untouched") {
    const Viewport vp{1000, 1000, frame_camera(2.0), comps[0]};
    const arbc::RebaseResult r = arbc::rebase(*state, vp);
    CHECK(r.need == arbc::RebaseNeed::none);
    CHECK_FALSE(r.event.occurred);
    CHECK(r.viewport.anchor == comps[0]);
  }
  SECTION("zoom-out reports the need but leaves the ancestor to the runtime path") {
    const Viewport vp{1000, 1000, frame_camera(1.0 / (arbc::k_reanchor_scale_threshold * 2.0)),
                      comps[1]};
    const arbc::RebaseResult r = arbc::rebase(*state, vp);
    CHECK(r.need == arbc::RebaseNeed::zoom_out);
    CHECK_FALSE(r.event.occurred);
    CHECK(r.viewport.anchor == comps[1]);
  }
  SECTION("the root sentinel has no anchored descendant to pick") {
    const Viewport vp{1000, 1000, frame_camera(arbc::k_reanchor_scale_threshold * 2.0),
                      arbc::k_root_anchor};
    const arbc::RebaseResult r = arbc::rebase(*state, vp);
    CHECK(r.need == arbc::RebaseNeed::zoom_in);
    CHECK_FALSE(r.event.occurred);
  }
}

namespace {

// Build a two-child anchor comp: one in-view leaf layer, plus one nested
// composition placed by `tiny_transform` (sub-pixel or singular). Returns
// {anchor, big_leaf_content, nested_leaf_content}.
struct SubtreeScene {
  ObjectId anchor;
  ObjectId visible_leaf;
  ObjectId nested_leaf;
};

SubtreeScene build_subtree_scene(arbc::Model& model, const Affine& tiny_transform) {
  auto txn = model.transact();
  const ObjectId big_leaf = txn.add_content(0);
  const ObjectId nested_leaf = txn.add_content(0);
  const ObjectId anchor = txn.add_composition(k_canvas, k_canvas);
  const ObjectId nested = txn.add_composition(k_canvas, k_canvas);

  const ObjectId big_layer = txn.add_layer(big_leaf, Affine::identity());
  txn.attach_layer(anchor, big_layer);
  const ObjectId nested_layer = txn.add_layer(nested, tiny_transform);
  txn.attach_layer(anchor, nested_layer);
  const ObjectId nested_leaf_layer = txn.add_layer(nested_leaf, Affine::identity());
  txn.attach_layer(nested, nested_leaf_layer);

  REQUIRE(txn.commit().has_value());
  return {anchor, big_leaf, nested_leaf};
}

std::vector<ObjectId> visited_contents(const arbc::DocRoot& state, const Viewport& vp) {
  std::vector<ObjectId> visited;
  arbc::cull_walk(state, vp, [&](const arbc::LayerRecord& layer, const Affine& composed) {
    (void)composed;
    visited.push_back(layer.content);
  });
  return visited;
}

bool contains(const std::vector<ObjectId>& v, ObjectId id) {
  for (const ObjectId x : v) {
    if (x == id) {
      return true;
    }
  }
  return false;
}

} // namespace

// enforces: 04-transforms-and-infinite-zoom#subpixel-subtree-culled
TEST_CASE("a sub-pixel subtree is culled without descending and emits no request") {
  arbc::Model model;
  // The nested composition is placed at 1e-6x: its 1000-unit canvas maps to
  // 0.001 device px -- far below one pixel.
  const SubtreeScene scene = build_subtree_scene(model, Affine::scaling(1.0e-6, 1.0e-6));
  const arbc::DocStatePtr state = model.current();

  const Viewport vp{1000, 1000, Affine::identity(), scene.anchor};
  const std::vector<ObjectId> visited = visited_contents(*state, vp);

  // The in-view leaf is visited (its content would be requested); the sub-pixel
  // subtree's leaf is never reached -> zero requests from it.
  CHECK(contains(visited, scene.visible_leaf));
  CHECK_FALSE(contains(visited, scene.nested_leaf));
}

TEST_CASE("a degenerate subtree placement culls without descending or NaNs") {
  arbc::Model model;
  // A singular authored transform (zero linear part): inverse() is nullopt, so
  // the subtree culls rather than propagating NaNs (doc 04:115-117).
  const SubtreeScene scene = build_subtree_scene(model, Affine{0.0, 0.0, 0.0, 0.0, 0.0, 0.0});
  const arbc::DocStatePtr state = model.current();

  const Viewport vp{1000, 1000, Affine::identity(), scene.anchor};
  const std::vector<ObjectId> visited = visited_contents(*state, vp);

  CHECK(contains(visited, scene.visible_leaf));
  CHECK_FALSE(contains(visited, scene.nested_leaf));
}

TEST_CASE(
    "the root sentinel binds no composition (empty walk); a real anchor composes the camera") {
  // compositor.root_composition_frame_walk, Decision 3: the flat fallback is
  // composition-scoped (`for_each_layer_in`), so the `k_root_anchor` sentinel --
  // "no composition bound" -- resolves nothing and visits NOTHING rather than
  // reviving the document-global walk that double-drew nested children (doc
  // 05:28-36). A real anchor visits that composition's members, composing the
  // camera with each layer's transform exactly as `render_frame`.
  arbc::Model model;
  ObjectId comp{};
  {
    auto txn = model.transact();
    const ObjectId c1 = txn.add_content(0);
    const ObjectId c2 = txn.add_content(0);
    const ObjectId a = txn.add_layer(c1, Affine::translation(10.0, 20.0));
    const ObjectId b = txn.add_layer(c2, Affine::translation(30.0, 40.0));
    comp = txn.add_composition(k_canvas, k_canvas);
    txn.attach_layer(comp, a);
    txn.attach_layer(comp, b);
    REQUIRE(txn.commit().has_value());
  }
  const arbc::DocStatePtr state = model.current();
  const Affine camera = Affine::scaling(2.0, 2.0);

  // The sentinel resolves no composition: an empty walk, no layers visited.
  std::vector<Affine> sentinel_composed;
  arbc::cull_walk(
      *state, Viewport{100, 100, camera, arbc::k_root_anchor},
      [&](const arbc::LayerRecord&, const Affine& m) { sentinel_composed.push_back(m); });
  CHECK(sentinel_composed.empty());

  // Anchored at the real composition: both members are visited, camera-composed.
  std::vector<Affine> composed;
  arbc::cull_walk(*state, Viewport{100, 100, camera, comp},
                  [&](const arbc::LayerRecord& layer, const Affine& m) {
                    composed.push_back(m);
                    CHECK(m == compose(camera, layer.transform));
                  });
  CHECK(composed.size() == 2);
}
