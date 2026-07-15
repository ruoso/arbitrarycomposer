#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/compositor/compositor.hpp>
#include <arbc/compositor/counters.hpp>
#include <arbc/compositor/pull_service.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/kind_nested/nested_content.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/model/model.hpp>
#include <arbc/surface/surface_pool.hpp>
#include <arbc/surface/testing/counting_backend.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

// Byte-exact goldens for org.arbc.nested (doc 16 tier-3). These are SELF-CHECKING
// equalities -- nested's output is compared against the compositor's own
// per-layer loop (`render_frame`) over the same layers, so there are no frozen
// tables to regenerate: the "rendering is recursion" identity (doc 05:24) is the
// oracle. Cross-component (compositor + backend-cpu + solid), so it lives here.

namespace {

using namespace arbc;

// A child composition of two org.arbc.solid layers over an explicit canvas, plus
// the id->Content resolver both the nested kind and the compositor consume.
struct Scene {
  Model model;
  std::unordered_map<ObjectId, Content*> binding;
  SolidContent solid_a{Rgba{0.60F, 0.20F, 0.10F, 0.80F}, Rect{0.0, 0.0, 8.0, 8.0}};
  SolidContent solid_b{Rgba{0.10F, 0.40F, 0.30F, 0.50F}, Rect{0.0, 0.0, 8.0, 8.0}};
  ObjectId comp{};

  // `working_space` is the child composition's DECLARED working space (doc 07 rule
  // 2, `CompositionRecord::working_space`). The default is the parent's, i.e. the
  // homogeneous tree every pre-existing case here drives; a different value makes
  // the nesting boundary a conversion point (rule 4).
  void build(SurfaceFormat working_space = k_working_rgba32f) {
    auto tx = model.transact("scene");
    comp = tx.add_composition(8.0, 8.0);
    const ObjectId ca = tx.add_content(1);
    const ObjectId cb = tx.add_content(1);
    const ObjectId la = tx.add_layer(ca, Affine::identity());
    const ObjectId lb = tx.add_layer(cb, Affine::translation(1.0, 1.0));
    tx.attach_layer(comp, la);
    tx.attach_layer(comp, lb);
    tx.set_working_space(comp, working_space);
    tx.commit();
    binding[ca] = &solid_a;
    binding[cb] = &solid_b;
  }

  ContentResolver resolver() {
    return [this](ObjectId id) -> Content* {
      const auto it = binding.find(id);
      return it != binding.end() ? it->second : nullptr;
    };
  }
};

// The abstract PullService contract (content.hpp:333): render `input` into the
// request's target and settle `done` exactly as Content::render does -- the
// inline (synchronous) honoring, threading the request verbatim. Also carries the
// recursion-depth backstop the real PullServiceImpl provides (doc 05:66-70), so a
// divergent Droste descent bottoms out on the placeholder rather than recursing
// forever.
class InlinePull final : public PullService {
public:
  explicit InlinePull(unsigned max_depth = 64) : d_max_depth(max_depth) {}
  void pull(ContentRef input, const RenderRequest& request,
            std::shared_ptr<RenderCompletion> done) override {
    if (input == nullptr) {
      done->fail(RenderError::ContentFailed);
      return;
    }
    if (d_depth >= d_max_depth) {
      ++d_backstops; // the >=1x cycle backstop (doc 05:66-70): placeholder
      done->fail(RenderError::ResourceUnavailable);
      return;
    }
    ++d_depth;
    const std::optional<RenderResult> r = input->render(request, done);
    --d_depth;
    if (r.has_value()) {
      done->complete(*r);
    }
  }
  unsigned backstops() const { return d_backstops; }

private:
  unsigned d_max_depth;
  unsigned d_depth{0};
  unsigned d_backstops{0};
};

std::vector<std::byte> bytes_of(const Surface& s) {
  const std::span<const std::byte> b = s.cpu_bytes();
  return {b.begin(), b.end()};
}

bool bytes_equal(const std::vector<std::byte>& a, const std::vector<std::byte>& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0;
}

// Render the child through NestedContent at `scale` (region-to-surface mapping
// region=(0,0,dim/scale,dim/scale), so the synthetic camera equals scaling(scale)).
// `parent_space` is the tag of the render target, i.e. the PARENT composition's
// working space: equal to the child's it is the homogeneous path, different it is
// the doc 07 rule 4 conversion boundary.
std::vector<std::byte> render_nested(Scene& scene, PullService& pull, Backend& backend, int dim,
                                     double scale, SurfaceFormat parent_space = k_working_rgba32f) {
  const DocStatePtr doc = scene.model.current();
  NestedContent nested(scene.comp);
  nested.attach(pull, backend, scene.resolver(), *doc);
  auto target = backend.make_surface(dim, dim, parent_space);
  REQUIRE(target.has_value());
  const RenderRequest req{Rect{0.0, 0.0, dim / scale, dim / scale},
                          scale,
                          Time::zero(),
                          StateHandle{},
                          **target,
                          Exactness::Exact,
                          Deadline::none()};
  auto done = std::make_shared<RenderCompletion>();
  const std::optional<RenderResult> r = nested.render(req, done);
  REQUIRE(r.has_value());
  return bytes_of(**target);
}

// Render the same layers as top-level compositor layers under viewport
// camera=scaling(scale) -- the flat-scene oracle (doc 05:24, "rendering is
// recursion").
std::vector<std::byte> render_flat(Scene& scene, Backend& backend, int dim, double scale) {
  const DocStatePtr doc = scene.model.current();
  SurfacePool pool(backend);
  auto target = backend.make_surface(dim, dim, k_working_rgba32f);
  REQUIRE(target.has_value());
  // Anchor the flat compositor at the child composition -- the same members
  // nested walks -- now that the frame walk is composition-scoped
  // (compositor.root_composition_frame_walk, doc 05:28-36).
  const Viewport viewport{dim, dim, Affine::scaling(scale, scale), scene.comp};
  render_frame(*doc, scene.resolver(), viewport, backend, pool, **target);
  return bytes_of(**target);
}

// The HETEROGENEOUS oracle (doc 07 rule 4), the same self-checking identity one
// conversion further on. Render the child's layers flat through the COMPOSITOR
// into a surface tagged with the CHILD's working space -- so they blend in the
// child's space, which is what rule 2 requires of the child composition's own
// layers, and the compositor inherits the tag from the target it is handed -- then
// convert that composed output ONCE into the parent's working space. That is rule
// 4 spelled out; nested must reproduce it byte-for-byte. Still no frozen table.
std::vector<std::byte> render_flat_then_convert(Scene& scene, Backend& backend, int dim,
                                                double scale, SurfaceFormat child_space,
                                                SurfaceFormat parent_space) {
  const DocStatePtr doc = scene.model.current();
  SurfacePool pool(backend);
  auto child_composed = backend.make_surface(dim, dim, child_space);
  REQUIRE(child_composed.has_value());
  const Viewport viewport{dim, dim, Affine::scaling(scale, scale), scene.comp};
  render_frame(*doc, scene.resolver(), viewport, backend, pool, **child_composed);

  auto parent_target = backend.make_surface(dim, dim, parent_space);
  REQUIRE(parent_target.has_value());
  backend.convert(**parent_target, **child_composed);
  return bytes_of(**parent_target);
}

// A backend that stores every format EXCEPT one -- the doc 09:55-60
// errors-as-values edge, standing exactly where the nesting boundary needs an
// allocation. When the child's DECLARED working space is a tag this backend cannot
// store, nested has nowhere to compose the child, and there is no honest fallback:
// composing into the parent's surface instead would blend the child's layers in the
// wrong space, which is the silent lie rule 4's boundary exists to prevent.
//
// Fault injection (doc 16:227-229) layered on `testing::CountingBackend`: it
// overrides only the allocation it refuses, and inherits the counters and the real
// forward for everything else -- so `convert_calls` below is the base's.
class RefusingBackend final : public arbc::testing::CountingBackend {
public:
  RefusingBackend(Backend& inner, SurfaceFormat refused)
      : arbc::testing::CountingBackend(inner), d_refused(refused) {}

  expected<std::unique_ptr<Surface>, SurfaceError> make_surface(int width, int height,
                                                                SurfaceFormat format) override {
    if (format == d_refused) {
      return unexpected(SurfaceError::UnsupportedFormat);
    }
    return arbc::testing::CountingBackend::make_surface(width, height, format);
  }

private:
  SurfaceFormat d_refused;
};

// Render the child through NestedContent driven by the LIVE `PullServiceImpl`
// (synchronous `direct_dispatch`), exercising delivery-to-target
// (`pull-delivers-to-caller-target`): each child-layer pull now flows through the
// real tile-cache / scheduling engine and the pulled child's pixels are delivered
// into the nested temp. The "rendering is recursion" equality against the flat
// oracle must still hold byte-exact -- proving delivery lands the right pixels for
// a multi-layer nested composition.
std::vector<std::byte> render_nested_live(Scene& scene, CpuBackend& backend, int dim,
                                          double scale) {
  const DocStatePtr doc = scene.model.current();
  NestedContent nested(scene.comp);
  TileCache cache(64u * 1024 * 1024);
  std::unordered_map<const Content*, ObjectId> ids;
  for (const auto& entry : scene.binding) {
    ids.emplace(entry.second, entry.first);
  }
  PullConfig config;
  config.id_of = [ids](const Content* c) {
    const auto it = ids.find(c);
    return it != ids.end() ? it->second : ObjectId{};
  };
  config.contribution = [](const Content*) { return std::uint64_t{1}; };
  PullServiceImpl service(cache, backend, direct_dispatch(), config);
  nested.attach(service, backend, scene.resolver(), *doc);
  auto target = backend.make_surface(dim, dim, k_working_rgba32f);
  REQUIRE(target.has_value());
  const RenderRequest req{Rect{0.0, 0.0, dim / scale, dim / scale},
                          scale,
                          Time::zero(),
                          StateHandle{},
                          **target,
                          Exactness::Exact,
                          Deadline::none()};
  auto done = std::make_shared<RenderCompletion>();
  const std::optional<RenderResult> r = nested.render(req, done);
  REQUIRE(r.has_value());
  return bytes_of(**target);
}

} // namespace

// enforces: 05-recursive-composition#nested-renders-through-synthetic-viewport
TEST_CASE("nested renders byte-identically to compositing the child's layers flat") {
  Scene scene;
  scene.build();
  CpuBackend backend;
  InlinePull pull;

  SECTION("native scale") {
    REQUIRE(bytes_equal(render_nested(scene, pull, backend, 8, 1.0),
                        render_flat(scene, backend, 8, 1.0)));
  }
  SECTION("0.5x") {
    REQUIRE(bytes_equal(render_nested(scene, pull, backend, 8, 0.5),
                        render_flat(scene, backend, 8, 0.5)));
  }
  SECTION("0.25x") {
    REQUIRE(bytes_equal(render_nested(scene, pull, backend, 8, 0.25),
                        render_flat(scene, backend, 8, 0.25)));
  }
}

// The doc 07 rule 4 identity, self-checking against the compositor exactly as the
// homogeneous case above: nested's heterogeneous output IS "compose the child's
// layers in the CHILD's working space, then convert the composed result once into
// the parent's". No frozen table -- the oracle is the compositor plus one convert.
// enforces: 07-color-and-pixel-formats#nesting-boundary-converts-composed-output
// enforces: 16-sdlc-and-quality#byte-exact-goldens
TEST_CASE("nested converts the child's composed output across a heterogeneous boundary") {
  CpuBackend backend;
  InlinePull pull;

  SECTION("an 8-bit sRGB fast-mode child inside a linear rgba32f parent") {
    Scene scene;
    scene.build(k_fast_rgba8srgb);
    REQUIRE(bytes_equal(
        render_nested(scene, pull, backend, 8, 1.0, k_working_rgba32f),
        render_flat_then_convert(scene, backend, 8, 1.0, k_fast_rgba8srgb, k_working_rgba32f)));
  }
  SECTION("an rgba16f child inside a linear rgba32f parent") {
    Scene scene;
    scene.build(k_working_rgba16f);
    REQUIRE(bytes_equal(
        render_nested(scene, pull, backend, 8, 1.0, k_working_rgba32f),
        render_flat_then_convert(scene, backend, 8, 1.0, k_working_rgba16f, k_working_rgba32f)));
  }
  SECTION("an rgba32f child inside an 8-bit sRGB fast-mode parent") {
    Scene scene;
    scene.build(k_working_rgba32f);
    REQUIRE(bytes_equal(
        render_nested(scene, pull, backend, 8, 1.0, k_fast_rgba8srgb),
        render_flat_then_convert(scene, backend, 8, 1.0, k_working_rgba32f, k_fast_rgba8srgb)));
  }
  SECTION("the boundary holds at a reduced scale too") {
    Scene scene;
    scene.build(k_fast_rgba8srgb);
    REQUIRE(bytes_equal(
        render_nested(scene, pull, backend, 8, 0.5, k_working_rgba32f),
        render_flat_then_convert(scene, backend, 8, 0.5, k_fast_rgba8srgb, k_working_rgba32f)));
  }
}

// The discriminator. The equality above would still pass if the child's layers were
// blended in the PARENT's space and the golden frozen from that pipeline -- unless
// the two pipelines are known to disagree. They do: two overlapping semi-transparent
// layers blended in nonlinear 8-bit sRGB are not the same image as those layers
// blended in linear rgba32f (doc 07 rule 3 is precisely that statement, and a child
// declaring the fast mode is ASKING for its artifacts). Asserting the inequality is
// what gives the equality teeth.
// enforces: 07-color-and-pixel-formats#nesting-boundary-converts-composed-output
TEST_CASE("nested blends the child's layers in the child's working space, not the parent's") {
  CpuBackend backend;
  InlinePull pull;

  Scene fast_child;
  fast_child.build(k_fast_rgba8srgb);
  const std::vector<std::byte> blended_in_child_space =
      render_nested(fast_child, pull, backend, 8, 1.0, k_working_rgba32f);

  // The same two overlapping semi-transparent layers, blended in the parent's linear
  // working space instead -- nested's homogeneous path, and what a compose-in-parent
  // implementation would produce for the fast-mode child above.
  Scene linear_child;
  linear_child.build(k_working_rgba32f);
  const std::vector<std::byte> blended_in_parent_space =
      render_nested(linear_child, pull, backend, 8, 1.0, k_working_rgba32f);

  REQUIRE(blended_in_child_space.size() == blended_in_parent_space.size());
  REQUIRE_FALSE(bytes_equal(blended_in_child_space, blended_in_parent_space));
}

// "Homogeneous trees pay nothing" (doc 07:34-35) and "one conversion per nested
// render, never one per layer" (rule 4's "composed output") are performance-shaped
// promises, so they get behavioral counters rather than a wall-clock timing (doc
// 16:54-62). The child has TWO layers, so "exactly one conversion and exactly one
// extra surface" is a real assertion: a per-layer conversion would show two of each.
// enforces: 07-color-and-pixel-formats#homogeneous-trees-pay-nothing
// enforces: 07-color-and-pixel-formats#nesting-boundary-converts-composed-output
TEST_CASE("a homogeneous nested render pays nothing; a heterogeneous one converts once") {
  // A counting decorator over the real CpuBackend: it forwards every operation and
  // tallies the boundary conversions and surface allocations one nested render
  // issues, which is how the two promises above get pinned.
  CpuBackend cpu;
  arbc::testing::CountingBackend backend(cpu);
  InlinePull pull;

  Scene homogeneous;
  homogeneous.build(k_working_rgba32f);
  backend.reset();
  render_nested(homogeneous, pull, backend, 8, 1.0, k_working_rgba32f);
  const int homogeneous_allocs = backend.make_surface_calls;
  const int homogeneous_converts = backend.convert_calls;

  // The render target plus one temp per composed child layer -- exactly what nested
  // allocated before the conversion path existed.
  REQUIRE(homogeneous_allocs == 3);
  REQUIRE(homogeneous_converts == 0);

  Scene heterogeneous;
  heterogeneous.build(k_fast_rgba8srgb);
  backend.reset();
  render_nested(heterogeneous, pull, backend, 8, 1.0, k_working_rgba32f);

  // Exactly ONE additional surface (the child-tagged boundary intermediate, not one
  // per layer) and exactly ONE conversion (the composed output, not one per layer).
  REQUIRE(backend.make_surface_calls == homogeneous_allocs + 1);
  REQUIRE(backend.convert_calls == 1);
}

// The other side of the boundary: the backend cannot store the child's declared
// working space at all. Errors as values (doc 09:55-60) -- the allocation fails, and
// nested answers honest EMPTY pixels over the already-cleared target rather than
// composing the child's layers in the parent's space, which would be a silent lie in
// exactly the direction rule 4 forbids. The inequality below is what gives the
// all-zero assertion teeth: this scene renders decisively non-empty when the same
// working space IS storable, so "empty" is a real answer here, not a vacuous one.
// enforces: 07-color-and-pixel-formats#nesting-boundary-converts-composed-output
TEST_CASE("nested answers honest empty pixels when the child's working space is unstorable") {
  InlinePull pull;

  Scene scene;
  scene.build(k_fast_rgba8srgb); // the child declares the 8-bit sRGB fast mode ...
  CpuBackend cpu;
  RefusingBackend refusing(cpu, k_fast_rgba8srgb); // ... which this backend cannot store.
  const std::vector<std::byte> unstorable =
      render_nested(scene, pull, refusing, 8, 1.0, k_working_rgba32f);

  REQUIRE(std::all_of(unstorable.begin(), unstorable.end(),
                      [](std::byte b) { return b == std::byte{0}; }));
  // Nothing was composed, so nothing was there to convert.
  REQUIRE(refusing.convert_calls == 0);

  // The same child through a backend that CAN store the fast mode: non-empty, so the
  // empty answer above is the unstorable path talking, not an empty scene.
  CpuBackend storing;
  Scene storable;
  storable.build(k_fast_rgba8srgb);
  const std::vector<std::byte> stored =
      render_nested(storable, pull, storing, 8, 1.0, k_working_rgba32f);
  REQUIRE_FALSE(bytes_equal(unstorable, stored));
}

// enforces: 16-sdlc-and-quality#byte-exact-goldens
// enforces: 13-effects-as-operators#operator-pulls-only-via-pull-service
TEST_CASE("nested re-runs the flat-scene equality byte-exact through the live pull service") {
  Scene scene;
  scene.build();
  CpuBackend backend;

  SECTION("native scale") {
    REQUIRE(bytes_equal(render_nested_live(scene, backend, 8, 1.0),
                        render_flat(scene, backend, 8, 1.0)));
  }
  SECTION("0.5x") {
    REQUIRE(bytes_equal(render_nested_live(scene, backend, 8, 0.5),
                        render_flat(scene, backend, 8, 0.5)));
  }
  SECTION("0.25x") {
    REQUIRE(bytes_equal(render_nested_live(scene, backend, 8, 0.25),
                        render_flat(scene, backend, 8, 0.25)));
  }
}

// enforces: 05-recursive-composition#nested-renders-through-synthetic-viewport
TEST_CASE("two visits to the same nested child within one snapshot are byte-identical") {
  Scene scene;
  scene.build();
  CpuBackend backend;
  InlinePull pull;
  // Snapshot consistency (doc 05:71-75): repeated visits at the same pinned state
  // render identical output -- the diamond-embedding invariant, folded to a
  // dedicated determinism case.
  REQUIRE(bytes_equal(render_nested(scene, pull, backend, 8, 1.0),
                      render_nested(scene, pull, backend, 8, 1.0)));
}

// enforces: 05-recursive-composition#nested-boundary-budget-flows-through
// enforces: 13-effects-as-operators#pull-inherits-snapshot-and-deadline
TEST_CASE("nested threads the outer snapshot/deadline/exactness through every pull") {
  Scene scene;
  scene.build();
  CpuBackend backend;

  // A pull that records the request fields it receives, then renders inline so
  // nested still composites (the fields it must carry verbatim, doc 05:93-101).
  struct Rec {
    StateHandle snapshot;
    Deadline deadline;
    Exactness exactness;
  };
  class RecordingPull final : public PullService {
  public:
    std::vector<Rec> seen;
    void pull(ContentRef input, const RenderRequest& request,
              std::shared_ptr<RenderCompletion> done) override {
      seen.push_back(Rec{request.snapshot, request.deadline, request.exactness});
      const std::optional<RenderResult> r = input ? input->render(request, done) : std::nullopt;
      if (r.has_value()) {
        done->complete(*r);
      } else if (!done->settled()) {
        done->fail(RenderError::ContentFailed);
      }
    }
  } pull;

  const DocStatePtr doc = scene.model.current();
  NestedContent nested(scene.comp);
  nested.attach(pull, backend, scene.resolver(), *doc);

  auto target = backend.make_surface(8, 8, k_working_rgba32f);
  REQUIRE(target.has_value());
  StateHandle snap{};
  snap.slot = 7;
  const Deadline dl{std::chrono::steady_clock::time_point::max() - std::chrono::seconds(1)};
  const RenderRequest req{Rect{0.0, 0.0, 8.0, 8.0}, 1.0, Time::zero(), snap, **target,
                          Exactness::Exact,         dl};
  auto done = std::make_shared<RenderCompletion>();
  (void)nested.render(req, done);

  // A depth-1 nested scene issues one pull per visible child layer (two here) --
  // exactly what the equivalent flat scene renders -- and each carries the outer
  // request's snapshot/deadline/exactness verbatim, never reset per level.
  REQUIRE(pull.seen.size() == 2);
  for (const Rec& r : pull.seen) {
    REQUIRE(r.snapshot == snap);
    REQUIRE(r.deadline == dl);
    REQUIRE(r.exactness == Exactness::Exact);
  }
}

// enforces: 05-recursive-composition#graph-walk-bounds-cycles
TEST_CASE("a Droste self-cycle terminates on the depth budget with a placeholder") {
  // A composition whose only layer references a nested content that embeds the
  // SAME composition (A embeds C, C's layer resolves back to the nested-A) with a
  // scale-down transform: a legitimate infinite-zoom object (doc 05:54-65).
  Model model;
  std::unordered_map<ObjectId, Content*> binding;
  SolidContent backdrop{Rgba{0.2F, 0.2F, 0.2F, 1.0F}, Rect{0.0, 0.0, 8.0, 8.0}};

  auto tx = model.transact("droste");
  const ObjectId comp = tx.add_composition(8.0, 8.0);
  const ObjectId c_back = tx.add_content(1);
  const ObjectId c_self = tx.add_content(1); // resolves back to the nested content
  const ObjectId l_back = tx.add_layer(c_back, Affine::identity());
  const ObjectId l_self = tx.add_layer(c_self, Affine::scaling(0.4, 0.4));
  tx.attach_layer(comp, l_back);
  tx.attach_layer(comp, l_self);
  tx.commit();
  const DocStatePtr doc = model.current();

  CpuBackend backend;
  InlinePull pull(/*max_depth=*/8);
  NestedContent nested(comp);
  binding[c_back] = &backdrop;
  binding[c_self] = &nested; // the cycle edge
  nested.attach(
      pull, backend,
      [&binding](ObjectId id) -> Content* {
        const auto it = binding.find(id);
        return it != binding.end() ? it->second : nullptr;
      },
      *doc);

  auto target = backend.make_surface(8, 8, k_working_rgba32f);
  REQUIRE(target.has_value());
  const RenderRequest req{
      Rect{0.0, 0.0, 8.0, 8.0}, 1.0, Time::zero(), StateHandle{}, **target, Exactness::BestEffort,
      Deadline::none()};
  auto done = std::make_shared<RenderCompletion>();
  // Terminates (the test completing is the liveness assertion) ...
  const std::optional<RenderResult> r = nested.render(req, done);
  REQUIRE(r.has_value());
  // ... and the depth budget fired, so the deepest self-reference degraded to the
  // placeholder rather than recursing unboundedly.
  REQUIRE(pull.backstops() > 0);

  // The composed output is deterministic across repeated visits (stable pixels).
  const std::vector<std::byte> first = bytes_of(**target);
  auto target2 = backend.make_surface(8, 8, k_working_rgba32f);
  REQUIRE(target2.has_value());
  const RenderRequest req2{
      Rect{0.0, 0.0, 8.0, 8.0}, 1.0, Time::zero(), StateHandle{}, **target2, Exactness::BestEffort,
      Deadline::none()};
  auto done2 = std::make_shared<RenderCompletion>();
  (void)nested.render(req2, done2);
  REQUIRE(bytes_equal(first, bytes_of(**target2)));
}
