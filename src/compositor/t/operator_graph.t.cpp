#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/cache/invalidation.hpp>
#include <arbc/cache/key_shapes.hpp>
#include <arbc/cache/keyed_store.hpp>
#include <arbc/compositor/compositor.hpp>
#include <arbc/compositor/counters.hpp>
#include <arbc/compositor/operator_graph.hpp>
#include <arbc/compositor/tile_planning.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/model/damage.hpp>
#include <arbc/model/model.hpp>
#include <arbc/surface/backend.hpp>
#include <arbc/surface/surface.hpp>
#include <arbc/surface/surface_error.hpp>
#include <arbc/surface/surface_pool.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

// Unit / behavioral tests for `compositor.operator_graph`: the compositor's
// first pass over the operator graph the core can see through `Content::inputs()`
// (doc 13:33-52). Exercised against synthetic operator `Content` doubles (the
// `operator_members.t.cpp` pattern) -- no real fade/crossfade kind exists yet
// (those are the `operators` stream) -- proving the four graph behaviors in
// isolation: aggregate-revision fold, damage routing, cycle/depth budgeting, and
// the `identity()` short-circuit plus its `operator_renders` counter.

namespace {

using arbc::Content;
using arbc::ContentRef;
using arbc::CompositorCounters;
using arbc::Damage;
using arbc::GraphBudget;
using arbc::GraphDiagnostics;
using arbc::IdentityResolution;
using arbc::OperatorLayer;
using arbc::Rect;
using arbc::RenderResult;
using arbc::ScaleRung;
using arbc::TileCache;
using arbc::TileCoord;
using arbc::TileKey;
using arbc::TileValue;

// A configurable operator/leaf `Content` double: N input edges (non-owning
// `Content*`, mutable via `set_inputs` so tests can close feedback cycles), an
// optional request-invariant `identity()` index, and a symmetric
// `map_input_damage` inflation (a blur-shaped covering map). Default construction
// is a leaf: empty inputs, `identity() == nullopt`, zero inflate -> `is_operator`
// false and the pass-through-shaped identity damage map.
class GraphContent : public arbc::Content {
public:
  explicit GraphContent(std::vector<ContentRef> inputs = {},
                        std::optional<std::size_t> identity_idx = std::nullopt, double inflate = 0.0)
      : d_inputs(std::move(inputs)), d_identity(identity_idx), d_inflate(inflate) {}

  void set_inputs(std::vector<ContentRef> inputs) { d_inputs = std::move(inputs); }

  std::optional<Rect> bounds() const override { return std::nullopt; }
  arbc::Stability stability() const override { return arbc::Stability::Static; }
  std::optional<arbc::TimeRange> time_extent() const override { return std::nullopt; }
  std::optional<RenderResult> render(const arbc::RenderRequest& request,
                                     std::shared_ptr<arbc::RenderCompletion> /*done*/) override {
    ++d_renders;
    return RenderResult{request.scale, /*exact=*/true};
  }

  std::span<const ContentRef> inputs() const override { return d_inputs; }
  std::optional<std::size_t> identity(const arbc::RenderRequest& /*request*/) const override {
    return d_identity;
  }
  arbc::Rect map_input_damage(std::size_t /*input*/, const arbc::Rect& rect) const override {
    return {rect.x0 - d_inflate, rect.y0 - d_inflate, rect.x1 + d_inflate, rect.y1 + d_inflate};
  }

  int renders() const { return d_renders; }

private:
  std::vector<ContentRef> d_inputs;
  std::optional<std::size_t> d_identity;
  double d_inflate;
  int d_renders{0};
};

// A CPU-buffer surface (cache-value construction and driver frames).
class BufferSurface : public arbc::Surface {
public:
  BufferSurface(int width, int height)
      : d_width(width), d_height(height),
        d_bytes(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 16,
                std::byte{0}) {}
  int width() const override { return d_width; }
  int height() const override { return d_height; }
  arbc::SurfaceFormat format() const override { return arbc::k_working_rgba32f; }
  std::span<std::byte> cpu_bytes() override { return d_bytes; }
  std::span<const std::byte> cpu_bytes() const override { return d_bytes; }

private:
  int d_width;
  int d_height;
  std::vector<std::byte> d_bytes;
};

// A request holder: `identity()` ignores the request, but the contract needs a
// concrete `RenderRequest` (with a bound target) to call it. Declaration order
// binds `request.target` to the constructed `target`.
struct StubReq {
  BufferSurface target{2, 2};
  arbc::RenderRequest request{arbc::Rect::from_size(2.0, 2.0), 1.0, arbc::Time::zero(),
                              arbc::StateHandle{}, target};
};

// A backend that allocates real-buffer surfaces and folds a deterministic marker
// on composite (mirrors `counters.t.cpp`'s `MarkBackend`).
class MarkBackend : public arbc::Backend {
public:
  arbc::BackendCaps capabilities() const override { return {}; }
  arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError>
  make_surface(int width, int height, arbc::SurfaceFormat /*format*/) override {
    return std::unique_ptr<arbc::Surface>(std::make_unique<BufferSurface>(width, height));
  }
  void clear(arbc::Surface& surface, float /*r*/, float /*g*/, float /*b*/, float /*a*/) override {
    std::span<std::byte> bytes = surface.cpu_bytes();
    std::memset(bytes.data(), 0, bytes.size_bytes());
  }
  void composite(arbc::Surface& dst, const arbc::Surface& /*src*/, const arbc::Affine& /*m*/,
                 double opacity) override {
    const auto mark = static_cast<unsigned>(opacity * 251.0) + 1u;
    for (std::byte& b : dst.cpu_bytes()) {
      b = static_cast<std::byte>((std::to_integer<unsigned>(b) + mark) & 0xFFu);
    }
  }
  void downsample(arbc::Surface& /*dst*/, const arbc::Surface& /*src*/) override {}
};

// A single-layer document over `content_id`; the resolver binds it to a
// caller-owned `Content` (the runtime binding, kept out of L4).
arbc::ObjectId add_single_layer(arbc::Model& model) {
  auto txn = model.transact();
  const arbc::ObjectId content_id = txn.add_content(0);
  txn.add_layer(content_id, arbc::Affine::identity());
  REQUIRE(txn.commit().has_value());
  return content_id;
}

// A per-node contribution over an explicit revision map (the fold's callback).
std::function<std::uint64_t(const Content*)>
contrib_of(const std::unordered_map<const Content*, std::uint64_t>& revs) {
  return [&revs](const Content* c) {
    const auto it = revs.find(c);
    return it != revs.end() ? it->second : 0U;
  };
}

} // namespace

// enforces: 05-recursive-composition#aggregate-revision-folds-reachable-inputs
// enforces: 03-layer-plugin-interface#leaf-content-has-no-operator-graph
TEST_CASE("operator_graph: aggregate revision folds each reachable input exactly once") {
  SECTION("a leaf (empty inputs) degenerates to contribution(root) -- the flat path") {
    GraphContent leaf;
    REQUIRE_FALSE(arbc::is_operator(&leaf));
    // A leaf is neutral under every graph-aware query (re-asserting the contract
    // leaf claim through the graph-aware planner's own functions).
    CHECK(leaf.map_input_damage(0, Rect{1.0, 2.0, 5.0, 9.0}) == Rect{1.0, 2.0, 5.0, 9.0});
    const std::unordered_map<const Content*, std::uint64_t> revs{{&leaf, 4242U}};
    CHECK(arbc::aggregate_revision(&leaf, contrib_of(revs)) == 4242U);
  }

  SECTION("a chain folds every node's contribution") {
    GraphContent b;
    GraphContent a({&b});
    GraphContent op({&a});
    REQUIRE(arbc::is_operator(&op));
    const std::unordered_map<const Content*, std::uint64_t> revs{{&op, 3U}, {&a, 30U}, {&b, 300U}};
    CHECK(arbc::aggregate_revision(&op, contrib_of(revs)) == 333U);
  }

  SECTION("a diamond folds the shared input once and is order-independent") {
    GraphContent c;
    GraphContent a({&c});
    GraphContent b({&c});
    GraphContent op({&a, &b});
    GraphContent op_perm({&b, &a}); // same reachable set, permuted input order
    const std::unordered_map<const Content*, std::uint64_t> revs{
        {&op, 1U}, {&op_perm, 1U}, {&a, 10U}, {&b, 100U}, {&c, 1000U}};
    // c folded once, not twice: 1 + 10 + 100 + 1000.
    CHECK(arbc::aggregate_revision(&op, contrib_of(revs)) == 1111U);
    CHECK(arbc::aggregate_revision(&op_perm, contrib_of(revs)) ==
          arbc::aggregate_revision(&op, contrib_of(revs)));
  }

  SECTION("the aggregate changes iff a reachable contribution changes") {
    GraphContent b;
    GraphContent a({&b});
    GraphContent op({&a});
    std::unordered_map<const Content*, std::uint64_t> revs{{&op, 3U}, {&a, 30U}, {&b, 300U}};
    const std::uint64_t before = arbc::aggregate_revision(&op, contrib_of(revs));
    revs[&b] = 301U; // bump a reachable input's contribution
    CHECK(arbc::aggregate_revision(&op, contrib_of(revs)) != before);
    const std::uint64_t after = arbc::aggregate_revision(&op, contrib_of(revs));
    GraphContent unrelated;
    revs[&unrelated] = 9999U; // an unreachable node's contribution never moves it
    CHECK(arbc::aggregate_revision(&op, contrib_of(revs)) == after);
  }

  SECTION("a cyclic inputs() graph still terminates, each node folded once") {
    GraphContent op;
    GraphContent a;
    op.set_inputs({&a});
    a.set_inputs({&op}); // op -> a -> op (feedback)
    const std::unordered_map<const Content*, std::uint64_t> revs{{&op, 7U}, {&a, 70U}};
    CHECK(arbc::aggregate_revision(&op, contrib_of(revs)) == 77U);
  }
}

// enforces: 05-recursive-composition#operator-damage-routes-through-map-input-damage
// enforces: 03-layer-plugin-interface#operator-damage-covers
TEST_CASE("operator_graph: input damage routes to reaching operators through map_input_damage") {
  const Rect in{10.0, 10.0, 20.0, 20.0};
  const arbc::TimeRange range{}; // routing preserves the caller's range verbatim

  SECTION("a single edge maps the rect forward, covering (over-approximating)") {
    GraphContent damaged;
    GraphContent op({&damaged}, std::nullopt, /*inflate=*/2.0);
    const OperatorLayer ops[] = {{arbc::ObjectId{}, &op}};
    const std::vector<Damage> out = arbc::route_operator_damage(ops, &damaged, in, range);
    REQUIRE(out.size() == 1U);
    // Covering: out ∩ in == in iff out ⊇ in; and it strictly inflates.
    CHECK(out[0].rect.intersect(in) == in);
    CHECK(out[0].rect.width() > in.width());
  }

  SECTION("a multi-hop chain composes each edge's map_input_damage") {
    GraphContent damaged;
    GraphContent mid({&damaged}, std::nullopt, /*inflate=*/1.0);
    GraphContent op({&mid}, std::nullopt, /*inflate=*/1.0);
    const OperatorLayer ops[] = {{arbc::ObjectId{}, &op}};
    const std::vector<Damage> out = arbc::route_operator_damage(ops, &damaged, in, range);
    REQUIRE(out.size() == 1U);
    CHECK(out[0].rect == Rect{in.x0 - 2.0, in.y0 - 2.0, in.x1 + 2.0, in.y1 + 2.0});
  }

  SECTION("a diamond unions both paths' mapped rects") {
    GraphContent damaged;
    GraphContent a({&damaged}, std::nullopt, /*inflate=*/1.0);
    GraphContent b({&damaged}, std::nullopt, /*inflate=*/5.0);
    GraphContent op({&a, &b}, std::nullopt, /*inflate=*/0.0);
    const OperatorLayer ops[] = {{arbc::ObjectId{}, &op}};
    const std::vector<Damage> out = arbc::route_operator_damage(ops, &damaged, in, range);
    REQUIRE(out.size() == 1U);
    // The wider path (inflate 5) dominates the union.
    CHECK(out[0].rect == Rect{in.x0 - 5.0, in.y0 - 5.0, in.x1 + 5.0, in.y1 + 5.0});
  }

  SECTION("an operator that does not reach the damaged input receives none") {
    GraphContent damaged;
    GraphContent other;
    GraphContent reaching({&damaged}, std::nullopt, 1.0);
    GraphContent sparing({&other}, std::nullopt, 1.0);
    const OperatorLayer ops[] = {{arbc::ObjectId{1}, &reaching}, {arbc::ObjectId{2}, &sparing}};
    const std::vector<Damage> out = arbc::route_operator_damage(ops, &damaged, in, range);
    REQUIRE(out.size() == 1U); // only `reaching` emitted
    CHECK(out[0].object == arbc::ObjectId{1});
  }

  SECTION("a cyclic inputs() graph terminates during routing, covering preserved") {
    GraphContent damaged;
    GraphContent a({&damaged}, std::nullopt, 1.0);
    GraphContent op({&a}, std::nullopt, 1.0);
    a.set_inputs({&damaged, &op}); // close the cycle op -> a -> op via a's back-edge
    const OperatorLayer ops[] = {{arbc::ObjectId{}, &op}};
    const std::vector<Damage> out = arbc::route_operator_damage(ops, &damaged, in, range);
    REQUIRE(out.size() == 1U);
    CHECK(out[0].rect.intersect(in) == in);
  }
}

// enforces: 05-recursive-composition#graph-walk-bounds-cycles
TEST_CASE("operator_graph: the recursion budget bounds a divergent identity cycle") {
  StubReq req;
  const arbc::RenderRequest& request = req.request;

  SECTION("an identity chain within budget resolves to its terminal, no diagnostic") {
    GraphContent terminal; // identity() == nullopt
    GraphContent mid({&terminal}, std::size_t{0});
    GraphContent head({&mid}, std::size_t{0});
    GraphDiagnostics diag;
    const IdentityResolution res =
        arbc::resolve_identity(&head, request, GraphBudget{/*max_depth=*/8}, &diag);
    CHECK(res.terminal == &terminal);
    CHECK(res.short_circuited);
    CHECK_FALSE(res.budget_exceeded);
    CHECK(diag.entries.empty());
  }

  SECTION("a divergent identity cycle exceeds the budget: placeholder + one diagnostic") {
    GraphContent opA({}, std::size_t{0});
    GraphContent opB({}, std::size_t{0});
    opA.set_inputs({&opB});
    opB.set_inputs({&opA}); // opA identity -> opB identity -> opA ...
    GraphDiagnostics diag;
    const IdentityResolution res =
        arbc::resolve_identity(&opA, request, GraphBudget{/*max_depth=*/4}, &diag);
    CHECK(res.terminal == nullptr); // renders the placeholder
    CHECK(res.budget_exceeded);
    REQUIRE(diag.entries.size() == 1U); // exactly one diagnostic
    REQUIRE_FALSE(diag.entries[0].path.empty());
    // The diagnostic names the offending content at the end of the recorded path.
    CHECK(diag.entries[0].content == diag.entries[0].path.back());
    CHECK((diag.entries[0].content == &opA || diag.entries[0].content == &opB));
  }

  SECTION("a broken identity index is not a budget failure -- render the node itself") {
    GraphContent op({}, std::size_t{0}); // identity index 0 with no input edge
    const IdentityResolution res = arbc::resolve_identity(&op, request, GraphBudget{4}, nullptr);
    CHECK(res.terminal == &op);
    CHECK_FALSE(res.budget_exceeded);
  }
}

// enforces: 03-layer-plugin-interface#operator-identity-faithful
TEST_CASE("operator_graph: identity resolution selects exactly input N") {
  StubReq req;
  const arbc::RenderRequest& request = req.request;

  GraphContent input0;
  GraphContent input1;

  SECTION("identity(request) == N resolves to input N") {
    GraphContent op0({&input0, &input1}, std::size_t{0});
    GraphContent op1({&input0, &input1}, std::size_t{1});
    const IdentityResolution r0 = arbc::resolve_identity(&op0, request);
    CHECK(r0.terminal == &input0);
    CHECK(r0.short_circuited);
    const IdentityResolution r1 = arbc::resolve_identity(&op1, request);
    CHECK(r1.terminal == &input1);
    CHECK(r1.short_circuited);
  }

  SECTION("a non-identity operator resolves to itself, not short-circuited") {
    GraphContent op({&input0}, std::nullopt);
    const IdentityResolution r = arbc::resolve_identity(&op, request);
    CHECK(r.terminal == &op);
    CHECK_FALSE(r.short_circuited);
    CHECK_FALSE(r.budget_exceeded);
  }
}

// enforces: 05-recursive-composition#composed-result-invalidated-like-leaf
TEST_CASE("operator_graph: an aggregate-revision-keyed tile invalidates like a leaf tile") {
  MarkBackend backend;
  TileCache cache(64u * 1024 * 1024);

  GraphContent leaf;
  GraphContent op({&leaf});
  const std::unordered_map<const Content*, std::uint64_t> revs{{&op, 5U}, {&leaf, 5U}};
  const std::uint64_t aggregate = arbc::aggregate_revision(&op, contrib_of(revs));

  const arbc::ObjectId op_id{7};
  const TileKey op_key{op_id, aggregate, ScaleRung{0}, TileCoord{0, 0}, std::nullopt};

  auto insert_tile = [&](const TileKey& key) {
    auto surf = backend.make_surface(256, 256, arbc::k_working_rgba32f);
    REQUIRE(surf.has_value());
    const std::size_t bytes = arbc::tile_byte_cost(**surf);
    cache.insert(key, TileValue{std::move(*surf), {1.0, true}}, bytes, arbc::PriorityClass::Visible);
  };

  SECTION("invalidate_content drops the aggregate-keyed tile (a plain uint64 slot)") {
    insert_tile(op_key);
    REQUIRE(cache.lookup(op_key).has_value());
    CHECK(arbc::cache::invalidate_content(cache, op_id) == 1U);
    CHECK_FALSE(cache.lookup(op_key).has_value());
  }

  SECTION("invalidate_region drops it by (content, region), revision-agnostic like a leaf") {
    insert_tile(op_key);
    const auto tile_rect = [](ScaleRung rung, TileCoord coord) {
      return arbc::tile_local_rect(rung, coord);
    };
    CHECK(arbc::cache::invalidate_region(cache, op_id, Rect{0.0, 0.0, 8.0, 8.0}, tile_rect) == 1U);
    CHECK_FALSE(cache.lookup(op_key).has_value());
  }
}

// enforces: 13-effects-as-operators#identity-plan-issues-no-operator-render
TEST_CASE("operator_graph: an identity operator layer issues zero operator renders") {
  MarkBackend backend;
  arbc::Model model;
  const arbc::ObjectId content_id = add_single_layer(model);
  const arbc::DocStatePtr state = model.current();
  const arbc::Viewport viewport{256, 256, arbc::Affine::identity()}; // one rung-0 tile
  arbc::SurfacePool pool(backend);

  GraphContent leaf; // the operator's single input

  auto tile_key_for = [&](const Content* op) {
    const std::unordered_map<const Content*, std::uint64_t> revs{{op, state->revision()},
                                                                 {&leaf, state->revision()}};
    const std::uint64_t aggregate = arbc::aggregate_revision(op, contrib_of(revs));
    return TileKey{content_id, aggregate, ScaleRung{0}, TileCoord{0, 0}, std::nullopt};
  };

  SECTION("a non-identity operator inline-renders exactly once and caches its output") {
    GraphContent op({&leaf}, std::nullopt); // operator, never a pass-through
    const auto resolver = [&](arbc::ObjectId id) -> arbc::Content* {
      return id == content_id ? &op : nullptr;
    };
    TileCache cache(64u * 1024 * 1024);
    auto frame = backend.make_surface(256, 256, arbc::k_working_rgba32f);
    REQUIRE(frame.has_value());
    CompositorCounters counters;
    arbc::render_frame_interactive(*state, resolver, viewport, cache, backend, pool, **frame,
                                   arbc::Deadline::none(), std::nullopt, nullptr, &counters);
    CHECK(counters.operator_renders() == 1U);
    CHECK(counters.requests_issued() == 1U);
    CHECK(op.renders() == 1);
    CHECK(cache.lookup(tile_key_for(&op)).has_value()); // keyed by its aggregate revision
  }

  SECTION("an identity operator issues no render and creates no operator-output entry") {
    GraphContent op({&leaf}, std::size_t{0}); // identity(request) == input 0
    const auto resolver = [&](arbc::ObjectId id) -> arbc::Content* {
      return id == content_id ? &op : nullptr;
    };
    TileCache cache(64u * 1024 * 1024);
    auto frame = backend.make_surface(256, 256, arbc::k_working_rgba32f);
    REQUIRE(frame.has_value());
    CompositorCounters counters;
    arbc::render_frame_interactive(*state, resolver, viewport, cache, backend, pool, **frame,
                                   arbc::Deadline::none(), std::nullopt, nullptr, &counters);
    CHECK(counters.operator_renders() == 0U); // zero operator renders
    CHECK(counters.requests_issued() == 0U);  // no render request either
    CHECK(op.renders() == 0);
    CHECK_FALSE(cache.lookup(tile_key_for(&op)).has_value()); // no operator-output entry
  }
}
