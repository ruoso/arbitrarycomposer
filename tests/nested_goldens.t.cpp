#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/compositor/compositor.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/kind_nested/nested_content.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/model/model.hpp>
#include <arbc/surface/surface_pool.hpp>

#include <catch2/catch_test_macros.hpp>

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

  void build() {
    auto tx = model.transact("scene");
    comp = tx.add_composition(8.0, 8.0);
    const ObjectId ca = tx.add_content(1);
    const ObjectId cb = tx.add_content(1);
    const ObjectId la = tx.add_layer(ca, Affine::identity());
    const ObjectId lb = tx.add_layer(cb, Affine::translation(1.0, 1.0));
    tx.attach_layer(comp, la);
    tx.attach_layer(comp, lb);
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
std::vector<std::byte> render_nested(Scene& scene, PullService& pull, Backend& backend, int dim,
                                     double scale) {
  const DocStatePtr doc = scene.model.current();
  NestedContent nested(scene.comp);
  nested.attach(pull, backend, scene.resolver(), *doc);
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

// Render the same layers as top-level compositor layers under viewport
// camera=scaling(scale) -- the flat-scene oracle (doc 05:24, "rendering is
// recursion").
std::vector<std::byte> render_flat(Scene& scene, Backend& backend, int dim, double scale) {
  const DocStatePtr doc = scene.model.current();
  SurfacePool pool(backend);
  auto target = backend.make_surface(dim, dim, k_working_rgba32f);
  REQUIRE(target.has_value());
  const Viewport viewport{dim, dim, Affine::scaling(scale, scale)};
  render_frame(*doc, scene.resolver(), viewport, backend, pool, **target);
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
  const RenderRequest req{Rect{0.0, 0.0, 8.0, 8.0}, 1.0,          Time::zero(), snap,
                          **target,                 Exactness::Exact, dl};
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
  const RenderRequest req{Rect{0.0, 0.0, 8.0, 8.0}, 1.0,           Time::zero(), StateHandle{},
                          **target,                 Exactness::BestEffort, Deadline::none()};
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
  const RenderRequest req2{Rect{0.0, 0.0, 8.0, 8.0}, 1.0,           Time::zero(), StateHandle{},
                           **target2,                Exactness::BestEffort, Deadline::none()};
  auto done2 = std::make_shared<RenderCompletion>();
  (void)nested.render(req2, done2);
  REQUIRE(bytes_equal(first, bytes_of(**target2)));
}
