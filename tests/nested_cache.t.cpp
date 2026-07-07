#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/cache/key_shapes.hpp>
#include <arbc/compositor/counters.hpp>
#include <arbc/compositor/pull_service.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/kind_nested/nested_content.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/model/model.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>

// Behavioral-counter tests for nested's two-level caching (doc 05:77-91): the
// composed result is cached by the PARENT like any leaf tile, keyed by the
// child's aggregate revision. A clock advance over a static subtree issues zero
// renders; an edit deep in the shared child re-renders exactly the spine. Asserts
// on dispatched-render counts (`requests_issued`), never wall-clock. Nested is
// pulled AS AN INPUT through a real `PullServiceImpl` (the parent's cache); its
// own child renders travel through a separate inline pull, so the parent counter
// isolates nested's composed-result renders.

namespace {

using namespace arbc;

// Inline honoring of the abstract PullService contract, for nested's own child
// renders (kept off the parent counter).
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

struct Fixture {
  Model model;
  CpuBackend backend;
  InlinePull inner_pull;
  SolidContent solid_a{Rgba{0.5F, 0.25F, 0.125F, 1.0F}, Rect{0.0, 0.0, 8.0, 8.0}};
  SolidContent solid_b{Rgba{0.2F, 0.4F, 0.1F, 0.75F}, Rect{0.0, 0.0, 8.0, 8.0}};
  SolidContent unrelated{Rgba{0.9F, 0.9F, 0.9F, 1.0F}, Rect{0.0, 0.0, 8.0, 8.0}};
  std::unordered_map<ObjectId, Content*> binding;
  ObjectId comp{};
  DocStatePtr doc;

  Fixture() {
    auto tx = model.transact("child");
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
    doc = model.current();
  }

  NestedResolver resolver() {
    return [this](ObjectId id) -> Content* {
      const auto it = binding.find(id);
      return it != binding.end() ? it->second : nullptr;
    };
  }
};

} // namespace

// enforces: 05-recursive-composition#static-subtree-served-from-cache
// enforces: 05-recursive-composition#composed-result-invalidated-like-leaf
TEST_CASE("a static nested subtree is served from the parent cache; only the spine re-renders") {
  Fixture fx;
  NestedContent nested(fx.comp);
  nested.attach(fx.inner_pull, fx.backend, fx.resolver(), *fx.doc);

  // The parent's cache + a per-node revision contribution (the aggregate-revision
  // key, doc 05:82-91). A stable contribution means a stable composed-result key.
  std::unordered_map<const Content*, std::uint64_t> revision;
  revision[&nested] = 1;
  revision[&fx.solid_a] = 1;
  revision[&fx.solid_b] = 1;
  revision[&fx.unrelated] = 1;

  TileCache cache(64u * 1024 * 1024);
  CompositorCounters counters;
  PullConfig config;
  config.counters = &counters;
  config.id_of = [&nested](const Content* c) -> ObjectId {
    return c == &nested ? ObjectId{100} : ObjectId{};
  };
  config.contribution = [&revision](const Content* c) -> std::uint64_t {
    const auto it = revision.find(c);
    return it != revision.end() ? it->second : 0U;
  };
  PullServiceImpl parent(cache, fx.backend, direct_dispatch(), config);

  auto target = fx.backend.make_surface(8, 8, k_working_rgba32f);
  REQUIRE(target.has_value());
  const Rect region = Rect::from_size(4.0, 4.0);

  auto pull_nested = [&](std::int64_t at_time) {
    const RenderRequest req{region,   1.0,      Time{at_time}, StateHandle{},
                            **target, Exactness::Exact, Deadline::none()};
    auto done = std::make_shared<RenderCompletion>();
    parent.pull(&nested, req, done);
  };

  // First pull: a miss dispatches exactly one composed-result render.
  pull_nested(0);
  REQUIRE(counters.requests_issued() == 1);

  // A clock advance over the STATIC subtree (aggregate revision unchanged, and a
  // Static content keys with a nullopt achieved_time) is served from the cache:
  // zero new renders.
  pull_nested(1'000'000);
  pull_nested(2'000'000);
  REQUIRE(counters.requests_issued() == 1);

  // An UNRELATED edit (a content the nested subtree does not reach) does not
  // change nested's aggregate revision -- still a cache hit, zero new renders.
  revision[&fx.unrelated] = 2;
  pull_nested(3'000'000);
  REQUIRE(counters.requests_issued() == 1);

  // An edit deep in the SHARED child bumps a reachable input's contribution, so
  // nested's aggregate revision changes: the composed-result tile is invalidated
  // exactly like a leaf tile keyed by revision, and the spine re-renders once.
  revision[&fx.solid_a] = 2;
  pull_nested(4'000'000);
  REQUIRE(counters.requests_issued() == 2);

  // ... and it is now cached again at the new revision.
  pull_nested(5'000'000);
  REQUIRE(counters.requests_issued() == 2);
}
