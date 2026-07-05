#include <arbc/base/geometry.hpp>
#include <arbc/base/time.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/model/records.hpp>
#include <arbc/surface/surface.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

// A minimal `Surface` used only to bind `RenderRequest::target` -- nothing is
// rendered in this task, so it stores no pixels and reports empty CPU access.
class StubSurface : public arbc::Surface {
public:
  int width() const override { return 0; }
  int height() const override { return 0; }
  arbc::SurfaceFormat format() const override { return arbc::k_working_rgba32f; }
  std::span<std::byte> cpu_bytes() override { return {}; }
  std::span<const std::byte> cpu_bytes() const override { return {}; }
};

// A leaf `Content` that overrides NONE of the operator-graph members, so it
// exercises their null/identity defaults exactly as today's walking-skeleton
// content does. `render` is a no-op settle -- nothing depends on its pixels.
class LeafContent : public arbc::Content {
public:
  std::optional<arbc::Rect> bounds() const override { return std::nullopt; }
  arbc::Stability stability() const override { return arbc::Stability::Static; }
  std::optional<arbc::TimeRange> time_extent() const override { return std::nullopt; }
  std::optional<arbc::RenderResult> render(const arbc::RenderRequest&,
                                           std::shared_ptr<arbc::RenderCompletion>) override {
    return arbc::RenderResult{};
  }
};

// An operator-shaped `Content` overriding all three graph members: it holds N
// input edges (non-owning `Content*`), is a pass-through onto `d_identity_idx`
// exactly when the request time matches `d_passthrough_at`, and inflates input
// damage by `d_inflate` on every side (a blur-shaped covering map).
class OperatorContent : public arbc::Content {
public:
  OperatorContent(std::vector<arbc::ContentRef> inputs, std::size_t identity_idx,
                  arbc::Time passthrough_at, double inflate)
      : d_inputs(std::move(inputs)), d_identity_idx(identity_idx), d_passthrough_at(passthrough_at),
        d_inflate(inflate) {}

  std::optional<arbc::Rect> bounds() const override { return std::nullopt; }
  arbc::Stability stability() const override { return arbc::Stability::Timed; }
  // The union of its (Static, nullopt) inputs' extents: temporally unbounded.
  std::optional<arbc::TimeRange> time_extent() const override { return std::nullopt; }
  std::optional<arbc::RenderResult> render(const arbc::RenderRequest&,
                                           std::shared_ptr<arbc::RenderCompletion>) override {
    return arbc::RenderResult{};
  }

  std::span<const arbc::ContentRef> inputs() const override { return d_inputs; }

  std::optional<std::size_t> identity(const arbc::RenderRequest& request) const override {
    if (request.time == d_passthrough_at) {
      return d_identity_idx;
    }
    return std::nullopt;
  }

  arbc::Rect map_input_damage(std::size_t /*input*/, const arbc::Rect& rect) const override {
    return {rect.x0 - d_inflate, rect.y0 - d_inflate, rect.x1 + d_inflate, rect.y1 + d_inflate};
  }

private:
  std::vector<arbc::ContentRef> d_inputs;
  std::size_t d_identity_idx;
  arbc::Time d_passthrough_at;
  double d_inflate;
};

// A `PullService` test double that records the exact arguments of the single
// `pull` it receives, so a test can witness that the pull carries the render
// contract's own request/completion types unchanged (no new settlement path).
class RecordingPull : public arbc::PullService {
public:
  void pull(arbc::ContentRef input, const arbc::RenderRequest& request,
            std::shared_ptr<arbc::RenderCompletion> done) override {
    d_input = input;
    d_region = request.region;
    d_scale = request.scale;
    d_time = request.time;
    d_snapshot = request.snapshot;
    d_target = &request.target;
    d_exactness = request.exactness;
    d_deadline = request.deadline;
    d_done = std::move(done);
    ++d_calls;
  }

  int calls() const { return d_calls; }
  arbc::ContentRef input() const { return d_input; }
  arbc::Rect region() const { return d_region; }
  double scale() const { return d_scale; }
  arbc::Time time() const { return d_time; }
  arbc::StateHandle snapshot() const { return d_snapshot; }
  const arbc::Surface* target() const { return d_target; }
  arbc::Exactness exactness() const { return d_exactness; }
  arbc::Deadline deadline() const { return d_deadline; }
  const std::shared_ptr<arbc::RenderCompletion>& completion() const { return d_done; }

private:
  int d_calls{0};
  arbc::ContentRef d_input{nullptr};
  arbc::Rect d_region{};
  double d_scale{0.0};
  arbc::Time d_time{};
  arbc::StateHandle d_snapshot{};
  const arbc::Surface* d_target{nullptr};
  arbc::Exactness d_exactness{arbc::Exactness::Exact};
  arbc::Deadline d_deadline{};
  std::shared_ptr<arbc::RenderCompletion> d_done;
};

// A `PullService` is an abstract interface: it cannot be instantiated, only
// implemented (doc 17:53). The implementation lands at L4.
static_assert(
    std::is_abstract_v<arbc::PullService>,
    "PullService is an abstract L3 interface (implementation is compositor.pull_service)");

} // namespace

// enforces: 03-layer-plugin-interface#leaf-content-has-no-operator-graph
TEST_CASE("a Content overriding none of the operator-graph members is a graph leaf") {
  LeafContent leaf;
  StubSurface target;

  // inputs(): an empty span -- a leaf has no graph edges.
  REQUIRE(leaf.inputs().empty());

  // map_input_damage(): the identity map -- output damage equals input damage.
  const arbc::Rect rect{1.0, 2.0, 5.0, 9.0};
  REQUIRE(leaf.map_input_damage(0, rect) == rect);
  REQUIRE(leaf.map_input_damage(7, arbc::Rect::from_size(3.0, 4.0)) ==
          arbc::Rect::from_size(3.0, 4.0));

  // identity(): nullopt for every request -- a leaf is never a pass-through.
  const arbc::RenderRequest req_a{arbc::Rect::from_size(2.0, 2.0), 1.0, arbc::Time::zero(),
                                  arbc::StateHandle{}, target};
  const arbc::RenderRequest req_b{arbc::Rect::from_size(4.0, 4.0), 2.0,    arbc::Time{999},
                                  arbc::StateHandle{3U},           target, arbc::Exactness::Exact};
  REQUIRE(leaf.identity(req_a) == std::nullopt);
  REQUIRE(leaf.identity(req_b) == std::nullopt);
}

TEST_CASE("inputs() returns the operator's input edges in order, viewing its own storage") {
  LeafContent a;
  LeafContent b;
  LeafContent c;
  OperatorContent op({&a, &b, &c}, 0U, arbc::Time::zero(), 0.0);

  const std::span<const arbc::ContentRef> edges = op.inputs();
  REQUIRE(edges.size() == 3U);
  REQUIRE(edges[0] == &a);
  REQUIRE(edges[1] == &b);
  REQUIRE(edges[2] == &c);

  // The span views the operator's storage (no copy): two calls return the same
  // backing pointer.
  REQUIRE(op.inputs().data() == op.inputs().data());
}

TEST_CASE(
    "identity() is request-scoped: the configured input for a pass-through request, else nullopt") {
  LeafContent a;
  LeafContent b;
  StubSurface target;
  const arbc::Time passthrough_at{4242};
  OperatorContent op({&a, &b}, 1U, passthrough_at, 0.0);

  const arbc::RenderRequest pass_through{arbc::Rect::from_size(2.0, 2.0), 1.0, passthrough_at,
                                         arbc::StateHandle{}, target};
  const arbc::RenderRequest other{arbc::Rect::from_size(2.0, 2.0), 1.0, arbc::Time::zero(),
                                  arbc::StateHandle{}, target};

  REQUIRE(op.identity(pass_through) == std::optional<std::size_t>{1U});
  REQUIRE(op.identity(other) == std::nullopt);
}

TEST_CASE("map_input_damage() over-approximates: the output rect covers the input rect") {
  LeafContent a;
  OperatorContent op({&a}, 0U, arbc::Time::zero(), /*inflate=*/2.0);

  const arbc::Rect in{10.0, 10.0, 20.0, 20.0};
  const arbc::Rect out = op.map_input_damage(0, in);

  // Covering (Constraint 5): the mapped output damage contains the input rect
  // in full -- over-approximation is sound. out ∩ in == in iff out ⊇ in.
  REQUIRE(out.intersect(in) == in);
  // ...and this operator strictly inflates, so it is a real over-approximation.
  REQUIRE(out.width() > in.width());
  REQUIRE(out.height() > in.height());
}

TEST_CASE("PullService::pull forwards the exact ref, request, and completion unchanged") {
  LeafContent leaf;
  StubSurface target;
  const arbc::RenderRequest request{arbc::Rect::from_size(4.0, 4.0), 2.0,    arbc::Time{123},
                                    arbc::StateHandle{5U},           target, arbc::Exactness::Exact,
                                    arbc::Deadline::none()};
  auto done = std::make_shared<arbc::RenderCompletion>();
  const arbc::ContentRef ref = &leaf;

  RecordingPull service;
  service.pull(ref, request, done);

  REQUIRE(service.calls() == 1);
  REQUIRE(service.input() == ref);

  // The request reached the service equal by value in every field (its target
  // is the same surface, by address).
  REQUIRE(service.region() == request.region);
  REQUIRE(service.scale() == request.scale);
  REQUIRE(service.time() == request.time);
  REQUIRE(service.snapshot() == request.snapshot);
  REQUIRE(service.target() == &request.target);
  REQUIRE(service.exactness() == request.exactness);
  REQUIRE(service.deadline() == request.deadline);

  // The completion is forwarded unchanged: the service holds the very same
  // one-shot handle (shared ownership of the same control block).
  REQUIRE(service.completion() == done);
  REQUIRE(service.completion().get() == done.get());
}
