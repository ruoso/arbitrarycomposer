#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/kind_nested/nested_content.hpp>
#include <arbc/model/model.hpp>
#include <arbc/surface/backend.hpp>
#include <arbc/surface/testing/stub_backend.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <span>
#include <unordered_map>

// Behavioral unit tests for org.arbc.nested's composed + memoized metadata and
// its operator-graph edges (doc 05:8-37, doc 13:39-67). Lives in the kind's own
// `t/` (links only `contract`), so it uses local `Content`/`Backend` doubles and
// drives the attach seam directly -- the conformance suite, byte-exact goldens,
// and the cache/budget behavioral counters that need `arbc-testing` /
// `backend-cpu` / the real `PullServiceImpl` live in top-level `tests/`.

namespace {

using namespace arbc;

// A leaf content double with configurable description metadata.
class StubLeaf final : public Content {
public:
  StubLeaf(std::optional<Rect> bounds, Stability stability,
           std::optional<TimeRange> extent = std::nullopt)
      : d_bounds(bounds), d_stability(stability), d_extent(extent) {}

  std::optional<Rect> bounds() const override { return d_bounds; }
  Stability stability() const override { return d_stability; }
  std::optional<TimeRange> time_extent() const override { return d_extent; }
  std::optional<RenderResult> render(const RenderRequest& request,
                                     std::shared_ptr<RenderCompletion>) override {
    return RenderResult{request.scale, true};
  }

private:
  std::optional<Rect> d_bounds;
  Stability d_stability;
  std::optional<TimeRange> d_extent;
};

// A backend that never allocates or composites: metadata queries touch no pixels.
class NullBackend final : public testing::StubBackend {
public:
  BackendCaps capabilities() const override { return {}; }
  expected<std::unique_ptr<Surface>, SurfaceError> make_surface(int, int, SurfaceFormat) override {
    return unexpected(SurfaceError::UnsupportedFormat);
  }
  void clear(Surface&, float, float, float, float) override {}
  void composite(Surface&, const Surface&, const Affine&, double) override {}
};

// A pull that never runs (metadata tests do not render).
class NullPull final : public PullService {
public:
  void pull(ContentRef, const RenderRequest&, std::shared_ptr<RenderCompletion>) override {}
};

// A two-solid child composition over a fixed canvas, plus its content resolver.
struct Scene {
  Model model;
  std::unordered_map<ObjectId, Content*> binding;
  ObjectId comp{};
  ObjectId layer_a{};
  ObjectId layer_b{};

  NestedResolver resolver() {
    return [this](ObjectId id) -> Content* {
      const auto it = binding.find(id);
      return it != binding.end() ? it->second : nullptr;
    };
  }
};

// Populate `s` with a child composition of two layers (identity, and
// translate(1,1)) bound to `a` / `b`. Canvas is 0 unless `declare_canvas`, so
// bounds exercises either the declared-canvas or the recursive-union path.
// (`Scene` owns a non-movable `Model`, so it is filled in place, never returned.)
void build_scene(Scene& s, Content* a, Content* b, bool declare_canvas) {
  auto tx = s.model.transact("scene");
  s.comp = tx.add_composition(declare_canvas ? 4.0 : 0.0, declare_canvas ? 4.0 : 0.0);
  const ObjectId ca = tx.add_content(1);
  const ObjectId cb = tx.add_content(1);
  s.layer_a = tx.add_layer(ca, Affine::identity());
  s.layer_b = tx.add_layer(cb, Affine::translation(1.0, 1.0));
  tx.attach_layer(s.comp, s.layer_a);
  tx.attach_layer(s.comp, s.layer_b);
  tx.commit();
  s.binding[ca] = a;
  s.binding[cb] = b;
}

} // namespace

TEST_CASE("nested exposes its child layers as operator inputs in membership order") {
  StubLeaf a(Rect{0.0, 0.0, 2.0, 2.0}, Stability::Static);
  StubLeaf b(Rect{0.0, 0.0, 2.0, 2.0}, Stability::Static);
  Scene scene;
  build_scene(scene, &a, &b, /*declare_canvas=*/true);
  const DocStatePtr doc = scene.model.current();

  NullPull pull;
  NullBackend backend;
  NestedContent nested(scene.comp);
  nested.attach(pull, backend, scene.resolver(), *doc);

  // A non-leaf operator: `inputs()` is non-empty (the conformance suite's
  // check_leaf_no_operator_graph is not applicable to nested).
  const std::span<const ContentRef> ins = nested.inputs();
  REQUIRE(ins.size() == 2);
  REQUIRE(ins[0] == &a);
  REQUIRE(ins[1] == &b);
}

// enforces: 05-recursive-composition#operator-damage-routes-through-map-input-damage
TEST_CASE("nested maps input damage through the embedding transform (covering)") {
  StubLeaf a(Rect{0.0, 0.0, 2.0, 2.0}, Stability::Static);
  StubLeaf b(Rect{0.0, 0.0, 2.0, 2.0}, Stability::Static);
  Scene scene;
  build_scene(scene, &a, &b, /*declare_canvas=*/true);
  const DocStatePtr doc = scene.model.current();

  NullPull pull;
  NullBackend backend;
  NestedContent nested(scene.comp);
  nested.attach(pull, backend, scene.resolver(), *doc);

  const Rect dmg{0.0, 0.0, 1.0, 1.0};
  // Input 0's layer transform is identity -> damage passes through unchanged.
  REQUIRE(nested.map_input_damage(0, dmg) == dmg);
  // Input 1's layer transform is translate(1,1) -> damage shifts by (1,1).
  REQUIRE(nested.map_input_damage(1, dmg) == Rect{1.0, 1.0, 2.0, 2.0});
  // Out-of-range input is a safe identity over-approximation, never a crash.
  REQUIRE(nested.map_input_damage(7, dmg) == dmg);
}

TEST_CASE("nested composes bounds and stability from the reachable child layers") {
  SECTION("declared child canvas is the bounds") {
    StubLeaf a(std::nullopt, Stability::Static); // unbounded leaf...
    StubLeaf b(Rect{0.0, 0.0, 2.0, 2.0}, Stability::Static);
    Scene scene;
    build_scene(scene, &a, &b, /*declare_canvas=*/true);
    const DocStatePtr doc = scene.model.current();
    NullPull pull;
    NullBackend backend;
    NestedContent nested(scene.comp);
    nested.attach(pull, backend, scene.resolver(), *doc);
    // ...but a declared canvas fixes the bounds regardless of the layers.
    REQUIRE(nested.bounds() == Rect{0.0, 0.0, 4.0, 4.0});
  }

  SECTION("undeclared canvas unions the transformed layer bounds") {
    StubLeaf a(Rect{0.0, 0.0, 2.0, 2.0}, Stability::Static); // -> (0,0,2,2)
    StubLeaf b(Rect{0.0, 0.0, 2.0, 2.0}, Stability::Static); // +(1,1) -> (1,1,3,3)
    Scene scene;
    build_scene(scene, &a, &b, /*declare_canvas=*/false);
    const DocStatePtr doc = scene.model.current();
    NullPull pull;
    NullBackend backend;
    NestedContent nested(scene.comp);
    nested.attach(pull, backend, scene.resolver(), *doc);
    REQUIRE(nested.bounds() == Rect{0.0, 0.0, 3.0, 3.0});
  }

  SECTION("an unbounded reachable layer makes nested unbounded") {
    StubLeaf a(std::nullopt, Stability::Static);
    StubLeaf b(Rect{0.0, 0.0, 2.0, 2.0}, Stability::Static);
    Scene scene;
    build_scene(scene, &a, &b, /*declare_canvas=*/false);
    const DocStatePtr doc = scene.model.current();
    NullPull pull;
    NullBackend backend;
    NestedContent nested(scene.comp);
    nested.attach(pull, backend, scene.resolver(), *doc);
    REQUIRE_FALSE(nested.bounds().has_value());
  }

  SECTION("stability is static iff every reachable layer is static") {
    StubLeaf a(Rect{0.0, 0.0, 2.0, 2.0}, Stability::Static);
    StubLeaf timed(Rect{0.0, 0.0, 2.0, 2.0}, Stability::Timed);
    Scene scene;
    build_scene(scene, &a, &timed, /*declare_canvas=*/true);
    const DocStatePtr doc = scene.model.current();
    NullPull pull;
    NullBackend backend;
    NestedContent nested(scene.comp);
    nested.attach(pull, backend, scene.resolver(), *doc);
    REQUIRE(nested.stability() == Stability::Timed);
  }
}

// enforces: 05-recursive-composition#nested-metadata-memoized-on-aggregate-revision
TEST_CASE("nested metadata recomputes only when the aggregate revision changes") {
  StubLeaf a(Rect{0.0, 0.0, 2.0, 2.0}, Stability::Static);
  StubLeaf b(Rect{0.0, 0.0, 2.0, 2.0}, Stability::Static);
  Scene scene;
  build_scene(scene, &a, &b, /*declare_canvas=*/true);
  const DocStatePtr doc_v1 = scene.model.current();

  NullPull pull;
  NullBackend backend;
  NestedContent nested(scene.comp);
  nested.attach(pull, backend, scene.resolver(), *doc_v1);

  // First query computes once; repeated queries across all three description
  // methods and inputs() at a STABLE revision recompute zero more times.
  (void)nested.bounds();
  REQUIRE(nested.metadata_recomputes() == 1);
  for (int i = 0; i < 8; ++i) {
    (void)nested.bounds();
    (void)nested.stability();
    (void)nested.time_extent();
    (void)nested.inputs();
  }
  REQUIRE(nested.metadata_recomputes() == 1);

  // An edit bumps the document revision; re-pinning + re-attaching the newer
  // version re-keys the memo (invalidated by child structural damage, doc 05:16).
  {
    auto tx = scene.model.transact("edit");
    tx.set_opacity(scene.layer_a, 0.5);
    tx.commit();
  }
  const DocStatePtr doc_v2 = scene.model.current();
  REQUIRE(doc_v2->revision() != doc_v1->revision());
  nested.attach(pull, backend, scene.resolver(), *doc_v2);
  (void)nested.bounds();
  REQUIRE(nested.metadata_recomputes() == 2);
}
