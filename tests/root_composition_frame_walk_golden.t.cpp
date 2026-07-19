#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/compositor/compositor.hpp>
#include <arbc/compositor/counters.hpp>
#include <arbc/compositor/pull_service.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/kind_nested/nested_content.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/model/model.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/host_viewport.hpp>
#include <arbc/runtime/interactive.hpp>
#include <arbc/runtime/offline.hpp>
#include <arbc/runtime/offline_sequence.hpp>
#include <arbc/surface/surface_pool.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <vector>

// Byte-exact + behavioral goldens for the composition-scoped visual frame walk
// (compositor.root_composition_frame_walk, doc 05:28-36). Before this task the
// compositor walked the document-global `for_each_layer`, so a document nesting a
// child composition drew the child's member layers TWICE: once at top level with
// child-local transforms, once (correctly) through `NestedContent::render`. These
// self-checking goldens pin that a frame now renders exactly ONE composition's
// members -- the nested child reached only through the enclosing layer's content,
// drawn once -- and that the drivers source the root composition themselves.
// Cross-component (compositor + backend-cpu + solid + nested + runtime), so it
// lives in tests/ linking the umbrella `arbc`.

using namespace arbc;

namespace {

constexpr int k_dim = 8;

// The abstract PullService contract honored inline (content.hpp:333), mirroring
// nested_goldens.t.cpp's InlinePull: render `input` into the request target and
// settle `done` exactly as Content::render does, so a nested child renders through
// the compositor's flat `render_frame` path with no cache/scheduler wiring.
class InlinePull final : public PullService {
public:
  void pull(ContentRef input, const RenderRequest& request,
            std::shared_ptr<RenderCompletion> done) override {
    if (input == nullptr) {
      done->fail(RenderError::ContentFailed);
      return;
    }
    const std::optional<RenderResult> r = input->render(request, done);
    if (r.has_value()) {
      done->complete(*r);
    }
  }
};

std::vector<std::byte> bytes_of(const Surface& s) {
  const std::span<const std::byte> b = s.cpu_bytes();
  return {b.begin(), b.end()};
}

bool bytes_equal(const std::vector<std::byte>& a, const std::vector<std::byte>& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0;
}

// A root composition R with (a) one direct OPAQUE full-canvas layer and (b) a
// `NestedContent` layer L embedding a child composition C, where C holds two
// TRANSLUCENT member layers over disjoint halves (left/right). The opaque layer is
// full-canvas so the flat and tiled drivers agree on it exactly; the two child
// layers are disjoint from each other so each blends over the opaque background
// once, making a doubled source-over a visible pixel change while keeping the
// nested-vs-flat composite associativity-free (each region is one translucent-over-
// opaque blend either way).
//
// R is created FIRST, so it wins the lowest-id root rule (`find_first_composition`,
// doc 08 Principle 7 -- the root is encountered first). Nothing attaches the
// nested layer to C: it is R's member, and C's members are C's.
struct NestedScene {
  Document doc;
  std::shared_ptr<SolidContent> opaque =
      std::make_shared<SolidContent>(Rgba{0.80F, 0.20F, 0.10F, 1.0F}, Rect{0.0, 0.0, 8.0, 8.0});
  std::shared_ptr<SolidContent> child_a =
      std::make_shared<SolidContent>(Rgba{0.10F, 0.40F, 0.30F, 0.50F}, Rect{0.0, 0.0, 4.0, 8.0});
  std::shared_ptr<SolidContent> child_b =
      std::make_shared<SolidContent>(Rgba{0.20F, 0.10F, 0.60F, 0.50F}, Rect{4.0, 0.0, 8.0, 8.0});
  std::shared_ptr<NestedContent> nested;
  ObjectId root{};
  ObjectId child{};

  NestedScene() {
    root = doc.add_composition(8.0, 8.0);  // R: created first -> lowest id -> the root
    child = doc.add_composition(8.0, 8.0); // C: the nested child
    const ObjectId l_a = doc.add_layer(doc.add_content(child_a), Affine::identity());
    const ObjectId l_b = doc.add_layer(doc.add_content(child_b), Affine::identity());
    doc.attach_layer(child, l_a);
    doc.attach_layer(child, l_b);
    const ObjectId l_op = doc.add_layer(doc.add_content(opaque), Affine::identity());
    nested = std::make_shared<NestedContent>(child);
    const ObjectId l_nested = doc.add_layer(doc.add_content(nested), Affine::identity());
    doc.attach_layer(root, l_op);
    doc.attach_layer(root, l_nested);
  }

  ContentResolver resolver() {
    return [this](ObjectId id) { return doc.resolve(id); };
  }
};

// The single-pass oracle: the SAME three layers -- the opaque layer plus C's two
// translucent layers -- as direct members of ONE flat composition, each composited
// exactly once, in the same bottom-to-top order (op, child_a, child_b). "Rendering
// is recursion" (doc 05:24): rendering the nested scene must reproduce this flat
// composite byte-for-byte. Fresh contents with identical parameters so the bytes
// are the same.
struct OracleScene {
  Document doc;
  ObjectId root{};

  OracleScene() {
    root = doc.add_composition(8.0, 8.0);
    const ObjectId l_op =
        doc.add_layer(doc.add_content(std::make_shared<SolidContent>(
                          Rgba{0.80F, 0.20F, 0.10F, 1.0F}, Rect{0.0, 0.0, 8.0, 8.0})),
                      Affine::identity());
    const ObjectId l_a =
        doc.add_layer(doc.add_content(std::make_shared<SolidContent>(
                          Rgba{0.10F, 0.40F, 0.30F, 0.50F}, Rect{0.0, 0.0, 4.0, 8.0})),
                      Affine::identity());
    const ObjectId l_b =
        doc.add_layer(doc.add_content(std::make_shared<SolidContent>(
                          Rgba{0.20F, 0.10F, 0.60F, 0.50F}, Rect{4.0, 0.0, 8.0, 8.0})),
                      Affine::identity());
    doc.attach_layer(root, l_op);
    doc.attach_layer(root, l_a);
    doc.attach_layer(root, l_b);
  }
};

// The flat single-pass reference, through the offline driver (which sources the
// root itself).
std::vector<std::byte> render_oracle(OracleScene& scene, Backend& backend) {
  const auto out = render_offline(scene.doc, Viewport{k_dim, k_dim, Affine::identity()}, backend);
  REQUIRE(out.has_value());
  return bytes_of(**out);
}

} // namespace

// The double-draw golden. Offline `render_frame` and the interactive tiled driver
// must each reproduce the single-pass oracle: the nested child's layers land once,
// through the nesting layer's embedding, never also at top level.
// enforces: 05-recursive-composition#frame-renders-one-compositions-layers
// enforces: 16-sdlc-and-quality#byte-exact-goldens
TEST_CASE("a frame renders one composition's layers: the nested child is drawn once") {
  CpuBackend backend;

  OracleScene oracle;
  const std::vector<std::byte> want = render_oracle(oracle, backend);

  SECTION("one-shot render_offline path") {
    NestedScene scene;
    const auto frame = render_offline(scene.doc, Viewport{k_dim, k_dim, Affine::identity()}, backend);
    REQUIRE(frame.has_value());
    REQUIRE(bytes_equal(bytes_of(**frame), want));
  }

  SECTION("offline render_frame path") {
    NestedScene scene;
    InlinePull pull;
    scene.nested->attach(pull, backend, scene.resolver(), *scene.doc.pin());
    SurfacePool pool(backend);
    const DocStatePtr pin = scene.doc.pin();
    auto target = backend.make_surface(k_dim, k_dim, pin->working_space());
    REQUIRE(target.has_value());
    // Anchor at the root composition R: the offline flat compositor draws R's two
    // direct members (op + the nested layer), and the nested layer owns the child.
    render_frame(*pin, scene.resolver(), Viewport{k_dim, k_dim, Affine::identity(), scene.root},
                 backend, pool, **target);
    REQUIRE(bytes_equal(bytes_of(**target), want));
    scene.nested->detach();
  }

  SECTION("interactive tiled driver, via the offline sequence driver") {
    NestedScene scene;
    // The SequenceRenderer binds the nested content to its live pull and sources
    // the root composition itself (no manual attach, no explicit anchor).
    SequenceRenderer renderer(scene.doc, Viewport{k_dim, k_dim, Affine::identity()}, backend);
    const auto frame = renderer.render_frame_at(Time::zero());
    REQUIRE(frame.has_value());
    REQUIRE(bytes_equal(bytes_of(**frame), want));
  }
}

// The performance-shaped proof (doc 16:54-62): the double draws are also extra
// WORK. A nested layer must contribute exactly ONE top-level composite (its own
// composed result), the same as any single direct member -- not one per child
// layer. Comparing the nested scene's `composites()` against a control scene of two
// plain direct members pins that C's members are not composited at top level, with
// no magic count to drift.
// enforces: 05-recursive-composition#frame-renders-one-compositions-layers
// enforces: 16-sdlc-and-quality#compositor-exposes-behavioral-counters
TEST_CASE("the nested child adds no top-level composites: no double-draw work") {
  CpuBackend backend;

  NestedScene nested_scene;
  SequenceRenderer nested_renderer(nested_scene.doc, Viewport{k_dim, k_dim, Affine::identity()},
                                   backend);
  REQUIRE(nested_renderer.render_frame_at(Time::zero()).has_value());
  const std::uint64_t nested_composites = nested_renderer.counters().composites();

  // Control: a root composition with the SAME opaque layer plus one plain solid over
  // the right half -- two direct members, one top-level composite each.
  Document control;
  const ObjectId control_root = control.add_composition(8.0, 8.0);
  const ObjectId c_op =
      control.add_layer(control.add_content(std::make_shared<SolidContent>(
                            Rgba{0.80F, 0.20F, 0.10F, 1.0F}, Rect{0.0, 0.0, 4.0, 8.0})),
                        Affine::identity());
  const ObjectId c_solid =
      control.add_layer(control.add_content(std::make_shared<SolidContent>(
                            Rgba{0.20F, 0.10F, 0.60F, 0.50F}, Rect{4.0, 0.0, 8.0, 8.0})),
                        Affine::identity());
  control.attach_layer(control_root, c_op);
  control.attach_layer(control_root, c_solid);
  SequenceRenderer control_renderer(control, Viewport{k_dim, k_dim, Affine::identity()}, backend);
  REQUIRE(control_renderer.render_frame_at(Time::zero()).has_value());

  // Equal: the nested layer is one composite, exactly like the plain solid. Under the
  // old document-global walk the nested scene would have composited C's two members
  // at top level too, so this count would exceed the control's.
  CHECK(nested_composites == control_renderer.counters().composites());
}

namespace {

// A SolidContent that records whether it was rendered, so a driver test can assert
// WHICH composition's members the frame walked.
class CountingSolid final : public Content {
public:
  explicit CountingSolid(Rgba color) : d_solid(color, Rect{0.0, 0.0, 8.0, 8.0}) {}
  std::optional<Rect> bounds() const override { return d_solid.bounds(); }
  Stability stability() const override { return d_solid.stability(); }
  std::optional<TimeRange> time_extent() const override { return d_solid.time_extent(); }
  std::optional<RenderResult> render(const RenderRequest& request,
                                     std::shared_ptr<RenderCompletion> done) override {
    ++d_renders;
    return d_solid.render(request, std::move(done));
  }
  int renders() const { return d_renders; }

private:
  SolidContent d_solid;
  int d_renders{0};
};

// A two-composition document: root R (lowest id, created first) holding one member
// backed by `root_solid`, and a second composition S holding one member backed by
// `other_solid`. A composition-scoped, root-anchored frame walk renders R's member
// and never S's.
struct TwoCompositionScene {
  Document doc;
  std::shared_ptr<CountingSolid> root_solid =
      std::make_shared<CountingSolid>(Rgba{0.9F, 0.1F, 0.1F, 1.0F});
  std::shared_ptr<CountingSolid> other_solid =
      std::make_shared<CountingSolid>(Rgba{0.1F, 0.1F, 0.9F, 1.0F});
  ObjectId root{};
  ObjectId second{};

  TwoCompositionScene() {
    root = doc.add_composition(8.0, 8.0);
    second = doc.add_composition(8.0, 8.0);
    const ObjectId l_root = doc.add_layer(doc.add_content(root_solid), Affine::identity());
    const ObjectId l_other = doc.add_layer(doc.add_content(other_solid), Affine::identity());
    doc.attach_layer(root, l_root);
    doc.attach_layer(second, l_other);
  }
};

} // namespace

// Each visual driver must source the root composition (lowest-id wins,
// `find_first_composition`) and render its members -- not the second composition's.
// enforces: 05-recursive-composition#frame-renders-one-compositions-layers
TEST_CASE("the visual drivers source the root composition and render only its members") {
  SECTION("render_offline") {
    TwoCompositionScene scene;
    CpuBackend backend;
    const auto out = render_offline(scene.doc, Viewport{k_dim, k_dim, Affine::identity()}, backend);
    REQUIRE(out.has_value());
    CHECK(scene.root_solid->renders() > 0);
    CHECK(scene.other_solid->renders() == 0);
  }

  SECTION("SequenceRenderer") {
    TwoCompositionScene scene;
    CpuBackend backend;
    SequenceRenderer renderer(scene.doc, Viewport{k_dim, k_dim, Affine::identity()}, backend);
    REQUIRE(renderer.render_frame_at(Time::zero()).has_value());
    CHECK(scene.root_solid->renders() > 0);
    CHECK(scene.other_solid->renders() == 0);
  }

  SECTION("HostViewport seeds its anchor to the root composition") {
    // The host constructor sources the root and seeds `Viewport::anchor` when the
    // host did not pin one, so a default-anchor host renders the root's members
    // through the same scoped walk the two cases above exercise.
    TwoCompositionScene scene;
    CpuBackend backend;
    SurfacePool pool(backend);
    TileCache cache(64u * 1024 * 1024);
    auto target = backend.make_surface(k_dim, k_dim, k_working_rgba32f);
    REQUIRE(target.has_value());
    InteractiveRenderer renderer({}, [] { return std::chrono::steady_clock::time_point{}; });
    HostViewport::Config cfg;
    cfg.viewport = Viewport{k_dim, k_dim, Affine::identity()}; // default (invalid) anchor
    HostViewport viewport(
        renderer, scene.doc, HostViewport::DocumentBinding{}, backend, pool, cache, **target,
        [] { return std::chrono::steady_clock::time_point{}; }, cfg);
    CHECK(viewport.anchor() == scene.root);
    CHECK(viewport.anchor() != scene.second);
  }
}
