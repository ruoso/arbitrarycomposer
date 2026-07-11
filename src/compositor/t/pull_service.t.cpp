#include <arbc/base/expected.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/cache/key_shapes.hpp>
#include <arbc/cache/keyed_store.hpp>
#include <arbc/compositor/compositor.hpp>
#include <arbc/compositor/counters.hpp>
#include <arbc/compositor/operator_graph.hpp>
#include <arbc/compositor/pull_service.hpp>
#include <arbc/compositor/refinement.hpp>
#include <arbc/compositor/scale_ladder.hpp>
#include <arbc/compositor/tile_planning.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/media/audio_block.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/model/damage.hpp>
#include <arbc/model/model.hpp>
#include <arbc/model/records.hpp>
#include <arbc/surface/backend.hpp>
#include <arbc/surface/surface.hpp>
#include <arbc/surface/surface_error.hpp>
#include <arbc/surface/surface_pool.hpp>
#include <arbc/surface/testing/stub_backend.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

// Unit / behavioral tests for `compositor.pull_service`: the concrete L4
// `PullServiceImpl` behind the abstract `PullService` seam (doc 13:69-89). Driven
// against synthetic operator/leaf `Content` doubles and a recording / deferrable
// `RenderDispatch` (the `operator_members.t.cpp` pattern) -- no real fade /
// crossfade / nested kind exists yet (those are the operators / kinds streams).
// Proves the three engine behaviors -- cache-first serve, worker dispatch on
// miss, snapshot+deadline+budget inheritance -- plus the null-path neutrality of
// `render_frame_interactive`'s delegated fill.

namespace {

using arbc::CompositorCounters;
using arbc::Content;
using arbc::ContentRef;
using arbc::Damage;
using arbc::Deadline;
using arbc::GraphBudget;
using arbc::GraphDiagnostics;
using arbc::PullConfig;
using arbc::PullServiceImpl;
using arbc::RefinementQueue;
using arbc::RenderCompletion;
using arbc::RenderRequest;
using arbc::RenderResult;
using arbc::ScaleRung;
using arbc::StateHandle;
using arbc::TileCache;
using arbc::TileCoord;
using arbc::TileKey;
using arbc::TileValue;

// A CPU-buffer surface (cache-value construction + dispatch targets).
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

// A backend that allocates real-buffer surfaces (mirrors `operator_graph.t.cpp`).
class MarkBackend : public arbc::testing::StubBackend {
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
};

// A `MarkBackend` that additionally records the last `composite` (dst / src /
// affine / opacity), so a delivery test can witness both THAT `pull` composited
// the covering tile into the caller's target and the tile->target affine it used
// (the region/scale-honoring mapping, Constraint 1) -- not just that some
// composite ran.
class CaptureBackend : public arbc::testing::StubBackend {
public:
  struct Composite {
    const arbc::Surface* dst{nullptr};
    const arbc::Surface* src{nullptr};
    arbc::Affine affine{};
    double opacity{0.0};
  };
  int composites{0};
  Composite last{};
  std::vector<Composite> all{}; // every composite, in order (multi-tile delivery)

  arbc::BackendCaps capabilities() const override { return {}; }
  arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError>
  make_surface(int width, int height, arbc::SurfaceFormat /*format*/) override {
    return std::unique_ptr<arbc::Surface>(std::make_unique<BufferSurface>(width, height));
  }
  void clear(arbc::Surface& surface, float /*r*/, float /*g*/, float /*b*/, float /*a*/) override {
    std::span<std::byte> bytes = surface.cpu_bytes();
    std::memset(bytes.data(), 0, bytes.size_bytes());
  }
  void composite(arbc::Surface& dst, const arbc::Surface& src, const arbc::Affine& m,
                 double opacity) override {
    ++composites;
    last = Composite{&dst, &src, m, opacity};
    all.push_back(last);
    const auto mark = static_cast<unsigned>(opacity * 251.0) + 1u;
    for (std::byte& b : dst.cpu_bytes()) {
      b = static_cast<std::byte>((std::to_integer<unsigned>(b) + mark) & 0xFFu);
    }
  }
};

// A configurable operator/leaf `Content` double. Empty `inputs` -> a leaf; an
// operator carries input edges, an optional request-invariant `identity()` index,
// and -- when a `service` is wired -- recursively pulls its first input inside
// `render` (a genuine operator-over-operator descent, for the budget backstop).
class GraphContent : public arbc::Content {
public:
  PullServiceImpl* service{nullptr};

  explicit GraphContent(std::vector<ContentRef> inputs = {},
                        std::optional<std::size_t> identity_idx = std::nullopt)
      : d_inputs(std::move(inputs)), d_identity(identity_idx) {}

  void set_inputs(std::vector<ContentRef> inputs) { d_inputs = std::move(inputs); }

  std::optional<arbc::Rect> bounds() const override { return std::nullopt; }
  arbc::Stability stability() const override { return arbc::Stability::Static; }
  std::optional<arbc::TimeRange> time_extent() const override { return std::nullopt; }
  std::optional<RenderResult> render(const RenderRequest& request,
                                     std::shared_ptr<RenderCompletion> done) override {
    ++d_renders;
    // A wired operator pulls its first input through the SAME service, threading
    // the request (snapshot/deadline) unchanged -- the recursive-pull descent. A
    // pull that bottomed out on the recursion budget (the placeholder) propagates
    // upward: this operator fails its own completion too, so a divergent descent
    // surfaces as a placeholder at the top, not a spurious success.
    if (service != nullptr && !d_inputs.empty() && d_inputs.front() != nullptr) {
      auto inner = std::make_shared<RenderCompletion>();
      service->pull(d_inputs.front(), request, inner);
      const std::optional<arbc::expected<RenderResult, arbc::RenderError>> settled = inner->take();
      if (!(settled.has_value() && settled->has_value())) {
        done->fail(arbc::RenderError::ResourceUnavailable);
        return std::nullopt;
      }
    }
    return RenderResult{request.scale, /*exact=*/true};
  }

  std::span<const ContentRef> inputs() const override { return d_inputs; }
  std::optional<std::size_t> identity(const RenderRequest& /*request*/) const override {
    return d_identity;
  }

  int renders() const { return d_renders; }

private:
  std::vector<ContentRef> d_inputs;
  std::optional<std::size_t> d_identity;
  int d_renders{0};
};

// A recording / deferrable `RenderDispatch`: records the exact request fields of
// every dispatch (so a test can witness snapshot/deadline carried verbatim), and
// either runs `content->render` inline (folding a returned result through `done`)
// or defers -- capturing the completion so the test settles it later (the async
// worker stand-in).
struct DispatchRecorder {
  int calls{0};
  bool defer{false};
  Content* last_content{nullptr};
  StateHandle last_snapshot{};
  Deadline last_deadline{};
  arbc::Rect last_region{};
  double last_scale{0.0};
  arbc::Time last_time{};

  void run(Content* content, const RenderRequest& request, std::shared_ptr<RenderCompletion> done) {
    ++calls;
    last_content = content;
    last_snapshot = request.snapshot;
    last_deadline = request.deadline;
    last_region = request.region;
    last_scale = request.scale;
    last_time = request.time;
    if (defer) {
      return; // leave `done` live: the recorded pending render settles later
    }
    const std::optional<RenderResult> result = content->render(request, done);
    if (result.has_value()) {
      done->complete(*result);
    }
  }
};

// Build a `RenderDispatch` over a shared recorder (so the test reads it back).
arbc::RenderDispatch recording_dispatch(const std::shared_ptr<DispatchRecorder>& rec) {
  return [rec](Content* content, const RenderRequest& request,
               std::shared_ptr<RenderCompletion> done) {
    rec->run(content, request, std::move(done));
  };
}

// A `Content* -> ObjectId` map over an explicit table (the runtime binding's
// reverse, supplied by the test) and a constant per-node revision contribution.
std::function<arbc::ObjectId(const Content*)>
id_map(const std::unordered_map<const Content*, arbc::ObjectId>& ids) {
  return [&ids](const Content* c) {
    const auto it = ids.find(c);
    return it != ids.end() ? it->second : arbc::ObjectId{};
  };
}

// The single rung-0 tile a `Rect::from_size(256, 256)` region at scale 1.0 covers.
TileKey tile_key(arbc::ObjectId id, std::uint64_t revision) {
  return TileKey{id, revision, ScaleRung{0}, TileCoord{0, 0}, std::nullopt};
}

// A rung-0 tile key at an explicit grid coordinate (the multi-tile covering set).
TileKey tile_key_at(arbc::ObjectId id, std::uint64_t revision, int col, int row) {
  return TileKey{id, revision, ScaleRung{0}, TileCoord{col, row}, std::nullopt};
}

// A 2x2-tile request over `from_size(512, 512)` at native scale: at rung 0 the
// grid cell is `k_tile_size` (256) local units, so this region covers coords
// (0,0),(1,0),(0,1),(1,1) -- exactly four tiles (Constraint 1/6).
RenderRequest four_tile_request(arbc::Surface& target) {
  return RenderRequest{arbc::Rect::from_size(512.0, 512.0),
                       1.0,
                       arbc::Time::zero(),
                       StateHandle{},
                       target,
                       arbc::Exactness::BestEffort,
                       Deadline::none()};
}

// A one-tile request over that footprint at native scale, carrying `snapshot` and
// `deadline` so a test can witness they ride through the dispatch verbatim.
RenderRequest one_tile_request(arbc::Surface& target, StateHandle snapshot, Deadline deadline) {
  return RenderRequest{
      arbc::Rect::from_size(256.0, 256.0), 1.0,     arbc::Time::zero(), snapshot, target,
      arbc::Exactness::BestEffort,         deadline};
}

// A single-layer document; the resolver binds it to a caller-owned `Content`.
arbc::ObjectId add_single_layer(arbc::Model& model) {
  auto txn = model.transact();
  const arbc::ObjectId content_id = txn.add_content(0);
  txn.add_layer(content_id, arbc::Affine::identity());
  REQUIRE(txn.commit().has_value());
  return content_id;
}

constexpr std::uint64_t k_rev = 1; // the constant contribution every node folds

std::function<std::uint64_t(const Content*)> const_contribution() {
  return [](const Content*) { return k_rev; };
}

} // namespace

// enforces: 13-effects-as-operators#pull-is-cache-first
// enforces: 02-architecture#miss-becomes-deadline-request
// enforces: 13-effects-as-operators#pull-inherits-snapshot-and-deadline
TEST_CASE(
    "pull_service: cache-first serve -- a warm hit issues no dispatch, a miss dispatches one") {
  MarkBackend backend;
  TileCache cache(64u * 1024 * 1024);
  auto rec = std::make_shared<DispatchRecorder>();
  BufferSurface caller_target(256, 256);

  GraphContent leaf; // a leaf input
  const arbc::ObjectId leaf_id{11};
  const std::unordered_map<const Content*, arbc::ObjectId> ids{{&leaf, leaf_id}};

  CompositorCounters counters;
  PullConfig config;
  config.counters = &counters;
  config.id_of = id_map(ids);
  config.contribution = const_contribution();
  PullServiceImpl service(cache, backend, recording_dispatch(rec), config);

  const TileKey key = tile_key(leaf_id, k_rev);

  SECTION("a resident exact fresh hit completes done synchronously and dispatches no render") {
    // Seed the input's tile under its identity + revision at exact rung scale.
    auto surf = backend.make_surface(256, 256, arbc::k_working_rgba32f);
    REQUIRE(surf.has_value());
    const std::size_t bytes = arbc::tile_byte_cost(**surf);
    cache.insert(key, TileValue{std::move(*surf), {1.0, true}}, bytes,
                 arbc::PriorityClass::Visible);

    auto done = std::make_shared<RenderCompletion>();
    service.pull(&leaf, one_tile_request(caller_target, StateHandle{}, Deadline::none()), done);

    CHECK(rec->calls == 0);                 // zero dispatches (cache-first)
    CHECK(counters.requests_issued() == 0); // zero renders issued
    CHECK(leaf.renders() == 0);
    REQUIRE(done->settled()); // completed synchronously from cache
    const auto settled = done->take();
    REQUIRE(settled.has_value());
    REQUIRE(settled->has_value());
    CHECK((*settled)->achieved_scale == 1.0);
  }

  SECTION("a fresh miss dispatches exactly one render carrying snapshot + deadline verbatim") {
    const StateHandle snapshot{7U};
    const Deadline deadline{std::chrono::steady_clock::time_point{std::chrono::seconds{42}}};

    auto done = std::make_shared<RenderCompletion>();
    service.pull(&leaf, one_tile_request(caller_target, snapshot, deadline), done);

    CHECK(rec->calls == 1); // exactly one dispatch on the miss
    CHECK(counters.requests_issued() == 1);
    CHECK(rec->last_content == &leaf);
    // Snapshot + deadline reach the dispatched render byte-equal (neither reset
    // nor recomputed), and the region/scale/time are the request's.
    CHECK(rec->last_snapshot == snapshot);
    CHECK(rec->last_deadline == deadline);
    CHECK(rec->last_region == arbc::Rect::from_size(256.0, 256.0));
    CHECK(rec->last_scale == 1.0);
    CHECK(rec->last_time == arbc::Time::zero());
    // The result tile is inserted under the input's identity; done is settled.
    REQUIRE(done->settled());
    CHECK(cache.lookup(key).has_value());
  }
}

// enforces: 13-effects-as-operators#pull-delivers-to-caller-target
// enforces: 13-effects-as-operators#pull-is-cache-first
TEST_CASE("pull_service: a warm hit delivers the resident tile's pixels into the caller's target, "
          "no dispatch") {
  CaptureBackend backend;
  TileCache cache(64u * 1024 * 1024);
  auto rec = std::make_shared<DispatchRecorder>();
  BufferSurface caller_target(256, 256);

  GraphContent leaf;
  const arbc::ObjectId leaf_id{31};
  const std::unordered_map<const Content*, arbc::ObjectId> ids{{&leaf, leaf_id}};

  CompositorCounters counters;
  PullConfig config;
  config.counters = &counters;
  config.id_of = id_map(ids);
  config.contribution = const_contribution();
  PullServiceImpl service(cache, backend, recording_dispatch(rec), config);
  const TileKey key = tile_key(leaf_id, k_rev);

  // Seed the input's covering tile under its identity + revision at exact rung
  // scale, and remember the resident surface so delivery's `src` can be checked.
  auto surf = backend.make_surface(256, 256, arbc::k_working_rgba32f);
  REQUIRE(surf.has_value());
  const arbc::Surface* const tile_ptr = surf->get();
  const std::size_t bytes = arbc::tile_byte_cost(**surf);
  cache.insert(key, TileValue{std::move(*surf), {1.0, true}}, bytes, arbc::PriorityClass::Visible);

  SECTION("an aligned region at native scale delivers under the identity affine") {
    // The caller's target starts transparent (`BufferSurface` zero-fills).
    for (const std::byte b : caller_target.cpu_bytes()) {
      REQUIRE(std::to_integer<unsigned>(b) == 0u);
    }

    auto done = std::make_shared<RenderCompletion>();
    service.pull(&leaf, one_tile_request(caller_target, StateHandle{}, Deadline::none()), done);

    // Cache-first: no render dispatched -- delivery is a composite, not a render.
    CHECK(rec->calls == 0);
    CHECK(counters.requests_issued() == 0);
    CHECK(leaf.renders() == 0);
    REQUIRE(done->settled()); // completed synchronously from the cache

    // Delivery composited the resident tile into the caller's target: for a
    // `from_size(256,256)` region at scale 1.0 over rung-0 tile (0,0) the
    // tile->target affine is the identity, src is the resident tile, dst the
    // caller's target, opacity 1.0 -- exactly one composite, no new cache tile.
    CHECK(backend.composites == 1);
    CHECK(backend.last.dst == static_cast<const arbc::Surface*>(&caller_target));
    CHECK(backend.last.src == tile_ptr);
    CHECK(backend.last.opacity == 1.0);
    const arbc::Affine m = backend.last.affine;
    CHECK(m.a == 1.0);
    CHECK(m.b == 0.0);
    CHECK(m.c == 0.0);
    CHECK(m.d == 1.0);
    CHECK(m.tx == 0.0);
    CHECK(m.ty == 0.0);
    for (const std::byte b : caller_target.cpu_bytes()) {
      CHECK(std::to_integer<unsigned>(b) == 252u); // opacity-1.0 mark folded in
    }
  }

  SECTION("an off-origin sub-tile region honors the request origin (not a raw blit)") {
    // A 128x128 region offset to local (40,24), still covered by the single rung-0
    // tile (0,0). Delivery must map the tile into `target` under the request's
    // region origin -- a `translate(-40,-24)` -- proving it does NOT assume the
    // target and the tile share an origin (Constraint 1).
    const arbc::RenderRequest req{arbc::Rect{40.0, 24.0, 168.0, 152.0},
                                  1.0,
                                  arbc::Time::zero(),
                                  StateHandle{},
                                  caller_target,
                                  arbc::Exactness::BestEffort,
                                  Deadline::none()};

    auto done = std::make_shared<RenderCompletion>();
    service.pull(&leaf, req, done);

    CHECK(rec->calls == 0); // still a warm hit (same covering tile / key)
    REQUIRE(done->settled());
    CHECK(backend.composites == 1);
    const arbc::Affine m = backend.last.affine;
    CHECK(m.a == 1.0);
    CHECK(m.d == 1.0);
    CHECK(m.tx == -40.0);
    CHECK(m.ty == -24.0);
  }
}

// enforces: 13-effects-as-operators#pull-delivers-to-caller-target
TEST_CASE("pull_service: a synchronous miss delivers the freshly-rendered tile into the caller's "
          "target in one pass") {
  CaptureBackend backend;
  TileCache cache(64u * 1024 * 1024);
  auto rec = std::make_shared<DispatchRecorder>(); // runs render inline (synchronous driver)
  BufferSurface caller_target(256, 256);

  GraphContent leaf;
  const arbc::ObjectId leaf_id{32};
  const std::unordered_map<const Content*, arbc::ObjectId> ids{{&leaf, leaf_id}};

  CompositorCounters counters;
  PullConfig config;
  config.counters = &counters;
  config.id_of = id_map(ids);
  config.contribution = const_contribution();
  PullServiceImpl service(cache, backend, recording_dispatch(rec), config);
  const TileKey key = tile_key(leaf_id, k_rev);

  auto done = std::make_shared<RenderCompletion>();
  service.pull(&leaf, one_tile_request(caller_target, StateHandle{}, Deadline::none()), done);

  // Exactly one render dispatched, and the tile was cached under the input's key.
  CHECK(rec->calls == 1);
  CHECK(counters.requests_issued() == 1);
  REQUIRE(done->settled());
  auto hit = cache.lookup(key);
  REQUIRE(hit.has_value());

  // Delivery composited the just-rendered cache tile into the caller's target in
  // the same pass: identity affine for this aligned request, opacity 1.0, and the
  // sole composite (the render filled the tile in-place -- no provided-surface
  // copy -- so delivery is the only composite the miss performs).
  CHECK(backend.composites == 1);
  CHECK(backend.last.dst == static_cast<const arbc::Surface*>(&caller_target));
  CHECK(backend.last.src == hit->get().surface.get());
  CHECK(backend.last.opacity == 1.0);
  const arbc::Affine m = backend.last.affine;
  CHECK(m.a == 1.0);
  CHECK(m.b == 0.0);
  CHECK(m.c == 0.0);
  CHECK(m.d == 1.0);
  CHECK(m.tx == 0.0);
  CHECK(m.ty == 0.0);
  for (const std::byte b : caller_target.cpu_bytes()) {
    CHECK(std::to_integer<unsigned>(b) == 252u);
  }
}

// enforces: 13-effects-as-operators#pull-fills-multi-tile-region
// enforces: 13-effects-as-operators#pull-delivers-to-caller-target
// enforces: 13-effects-as-operators#pull-is-cache-first
TEST_CASE("pull_service: a pull over a 2x2-tile region fills the caller's target across every "
          "covering tile, dispatching one render per missing tile") {
  CaptureBackend backend;
  TileCache cache(64u * 1024 * 1024);
  auto rec = std::make_shared<DispatchRecorder>(); // runs render inline (synchronous driver)
  BufferSurface caller_target(512, 512);           // holds the 2x2-tile region

  GraphContent leaf;
  const arbc::ObjectId leaf_id{51};
  const std::unordered_map<const Content*, arbc::ObjectId> ids{{&leaf, leaf_id}};

  CompositorCounters counters;
  PullConfig config;
  config.counters = &counters;
  config.id_of = id_map(ids);
  config.contribution = const_contribution();
  PullServiceImpl service(cache, backend, recording_dispatch(rec), config);

  // The four covering-tile keys of the `from_size(512,512)` region at rung 0.
  const TileKey k00 = tile_key_at(leaf_id, k_rev, 0, 0);
  const TileKey k10 = tile_key_at(leaf_id, k_rev, 1, 0);
  const TileKey k01 = tile_key_at(leaf_id, k_rev, 0, 1);
  const TileKey k11 = tile_key_at(leaf_id, k_rev, 1, 1);

  // The per-coord delivery affine `deliver_tile` uses for this region at scale 1.0
  // is a pure translation to the tile's origin (col*256, row*256): the four tiles
  // land in four disjoint sub-rects, no seam, no double-cover (Constraint 1/6).
  const auto delivered_translations = [&]() {
    std::vector<std::pair<double, double>> out;
    for (const CaptureBackend::Composite& c : backend.all) {
      CHECK(c.dst == static_cast<const arbc::Surface*>(&caller_target));
      CHECK(c.opacity == 1.0);
      CHECK(c.affine.a == 1.0);
      CHECK(c.affine.d == 1.0);
      CHECK(c.affine.b == 0.0);
      CHECK(c.affine.c == 0.0);
      out.emplace_back(c.affine.tx, c.affine.ty);
    }
    std::sort(out.begin(), out.end());
    return out;
  };
  const std::vector<std::pair<double, double>> expected_translations{
      {0.0, 0.0}, {0.0, 256.0}, {256.0, 0.0}, {256.0, 256.0}};

  auto seed = [&](const TileKey& key) {
    auto surf = backend.make_surface(256, 256, arbc::k_working_rgba32f);
    REQUIRE(surf.has_value());
    const std::size_t bytes = arbc::tile_byte_cost(**surf);
    cache.insert(key, TileValue{std::move(*surf), {1.0, true}}, bytes,
                 arbc::PriorityClass::Visible);
  };

  SECTION("a partially-warm region dispatches exactly one render per MISSING tile") {
    // Two of four tiles resident; the diagonal pair (1,0) and (0,1) miss.
    seed(k00);
    seed(k11);

    auto done = std::make_shared<RenderCompletion>();
    service.pull(&leaf, four_tile_request(caller_target), done);

    // Exactly N - M == 2 dispatches (one per missing tile); every covering tile is
    // delivered into its own sub-rect (4 composites), and the two missing tiles are
    // now cached.
    CHECK(rec->calls == 2);
    CHECK(counters.requests_issued() == 2);
    CHECK(backend.composites == 4);
    CHECK(delivered_translations() == expected_translations);
    CHECK(cache.lookup(k10).has_value());
    CHECK(cache.lookup(k01).has_value());
    // The aggregate settled once: exact (every covering tile exact) at the rung
    // scale.
    REQUIRE(done->settled());
    const auto settled = done->take();
    REQUIRE(settled.has_value());
    REQUIRE(settled->has_value());
    CHECK((*settled)->achieved_scale == 1.0);
    CHECK((*settled)->exact);
  }

  SECTION("a fully-warm region issues zero dispatch and inserts no tile (cache-first, per tile)") {
    seed(k00);
    seed(k10);
    seed(k01);
    seed(k11);

    auto done = std::make_shared<RenderCompletion>();
    service.pull(&leaf, four_tile_request(caller_target), done);

    CHECK(rec->calls == 0);                 // zero dispatch: every tile hit
    CHECK(counters.requests_issued() == 0); // requests_issued delta 0
    CHECK(backend.composites == 4);         // four deliveries, no render composite
    CHECK(delivered_translations() == expected_translations);
    REQUIRE(done->settled());
    const auto settled = done->take();
    REQUIRE(settled.has_value());
    REQUIRE(settled->has_value());
    CHECK((*settled)->exact);
  }

  SECTION("a fully-cold region dispatches exactly N renders, delivering all in one pass") {
    auto done = std::make_shared<RenderCompletion>();
    service.pull(&leaf, four_tile_request(caller_target), done);

    CHECK(rec->calls == 4); // one render per covering tile
    CHECK(counters.requests_issued() == 4);
    CHECK(backend.composites == 4); // each freshly-rendered tile delivered once
    CHECK(delivered_translations() == expected_translations);
    CHECK(cache.lookup(k00).has_value());
    CHECK(cache.lookup(k10).has_value());
    CHECK(cache.lookup(k01).has_value());
    CHECK(cache.lookup(k11).has_value());
    REQUIRE(done->settled());
  }
}

// enforces: 13-effects-as-operators#pull-fills-multi-tile-region
TEST_CASE("pull_service: an empty/degenerate region covers no tile, delivers nothing, and settles "
          "exact with no dispatch") {
  CaptureBackend backend;
  TileCache cache(64u * 1024 * 1024);
  auto rec = std::make_shared<DispatchRecorder>();
  BufferSurface caller_target(256, 256);

  GraphContent leaf;
  const arbc::ObjectId leaf_id{53};
  const std::unordered_map<const Content*, arbc::ObjectId> ids{{&leaf, leaf_id}};

  CompositorCounters counters;
  PullConfig config;
  config.counters = &counters;
  config.id_of = id_map(ids);
  config.contribution = const_contribution();
  PullServiceImpl service(cache, backend, recording_dispatch(rec), config);

  // A zero-extent region covers no tile (`tiles_covering` yields no cells), so the
  // loop runs zero times: no probe, no dispatch, no delivery, and `done` settles
  // exact from the (empty) aggregate at the rung scale (Constraint 7) -- never the
  // old degenerate `{}`-coord probe.
  const RenderRequest req{arbc::Rect::from_size(0.0, 0.0),
                          1.0,
                          arbc::Time::zero(),
                          StateHandle{},
                          caller_target,
                          arbc::Exactness::BestEffort,
                          Deadline::none()};

  auto done = std::make_shared<RenderCompletion>();
  service.pull(&leaf, req, done);

  CHECK(rec->calls == 0);
  CHECK(counters.requests_issued() == 0);
  CHECK(backend.composites == 0); // nothing delivered
  REQUIRE(done->settled());
  const auto settled = done->take();
  REQUIRE(settled.has_value());
  REQUIRE(settled->has_value());
  CHECK((*settled)->achieved_scale == 1.0);
  CHECK((*settled)->exact);
}

// enforces: 13-effects-as-operators#pull-retains-render-surface-until-settle
TEST_CASE("pull_service: an async dispatch with no reap sink degrades to the placeholder plus one "
          "diagnostic, freeing no live surface") {
  MarkBackend backend;
  TileCache cache(64u * 1024 * 1024);
  auto rec = std::make_shared<DispatchRecorder>();
  rec->defer = true; // the dispatch answers asynchronously: `done` is left unsettled
  BufferSurface caller_target(256, 256);

  GraphContent leaf;
  const arbc::ObjectId leaf_id{33};
  const std::unordered_map<const Content*, arbc::ObjectId> ids{{&leaf, leaf_id}};

  GraphDiagnostics diags;
  PullConfig config;
  config.id_of = id_map(ids);
  config.contribution = const_contribution();
  config.diagnostics = &diags;
  config.pending = nullptr; // NO reap sink: the Decision-3 misconfiguration
  PullServiceImpl service(cache, backend, recording_dispatch(rec), config);

  // Precondition of the misconfiguration: the injected dispatch answers async.
  REQUIRE(rec->defer);

#ifdef NDEBUG
  // Release: `pull`'s `assert(done->settled())` is compiled out, so a sink-less
  // async miss degrades safely instead of falling through to the use-after-free
  // this branch used to leave. The render was dispatched, `done` fails with the
  // placeholder, and exactly one diagnostic names the offending input -- no crash,
  // and no live surface is freed (the deferred dispatch wrote nothing and nothing
  // is retained).
  auto done = std::make_shared<RenderCompletion>();
  service.pull(&leaf, one_tile_request(caller_target, StateHandle{}, Deadline::none()), done);
  CHECK(rec->calls == 1);
  REQUIRE(done->settled());
  const auto settled = done->take();
  REQUIRE(settled.has_value());
  CHECK_FALSE(settled->has_value()); // placeholder failure
  CHECK(settled->error() == arbc::RenderError::ResourceUnavailable);
  CHECK(diags.entries.size() == 1);
#endif
  // Debug: `pull` asserts `done->settled()` after a sink-less dispatch; the same
  // precondition (an async-answering dispatch) is witnessed by `rec->defer` above
  // without tripping the abort (mirrors the NDEBUG convention in
  // backend_cpu/t/cpu_backend.t.cpp).
}

// enforces: 13-effects-as-operators#pull-inherits-snapshot-and-deadline
TEST_CASE(
    "pull_service: a nested operator->operator->leaf pull carries one snapshot at every hop") {
  MarkBackend backend;
  TileCache cache(64u * 1024 * 1024);
  auto rec = std::make_shared<DispatchRecorder>();
  BufferSurface caller_target(256, 256);

  GraphContent leaf;
  GraphContent inner_op({&leaf});
  GraphContent outer_op({&inner_op});
  const std::unordered_map<const Content*, arbc::ObjectId> ids{
      {&leaf, arbc::ObjectId{1}}, {&inner_op, arbc::ObjectId{2}}, {&outer_op, arbc::ObjectId{3}}};

  PullConfig config;
  config.id_of = id_map(ids);
  config.contribution = const_contribution();
  PullServiceImpl service(cache, backend, recording_dispatch(rec), config);
  inner_op.service = &service; // each operator recursively pulls its input
  outer_op.service = &service;

  const StateHandle snapshot{99U};
  const Deadline deadline{std::chrono::steady_clock::time_point{std::chrono::seconds{5}}};

  auto done = std::make_shared<RenderCompletion>();
  service.pull(&outer_op, one_tile_request(caller_target, snapshot, deadline), done);

  // Every hop dispatched (outer op, inner op, leaf); the LAST dispatch is the
  // leaf, and it carries the same snapshot token + deadline as the top request --
  // one revision set per node within one frame (doc 05:71-74).
  CHECK(rec->last_content == &leaf);
  CHECK(rec->last_snapshot == snapshot);
  CHECK(rec->last_deadline == deadline);
  CHECK(rec->calls == 3);
}

// enforces: 02-architecture#async-arrival-emits-damage
TEST_CASE("pull_service: an async pull records pending, then a poll inserts it and emits damage") {
  MarkBackend backend;
  TileCache cache(64u * 1024 * 1024);
  auto rec = std::make_shared<DispatchRecorder>();
  rec->defer = true; // the dispatch answers asynchronously (leaves done live)
  BufferSurface caller_target(256, 256);

  GraphContent leaf;
  const arbc::ObjectId leaf_id{21};
  const std::unordered_map<const Content*, arbc::ObjectId> ids{{&leaf, leaf_id}};

  CompositorCounters counters;
  RefinementQueue queue;
  PullConfig config;
  config.counters = &counters;
  config.pending = &queue;
  config.id_of = id_map(ids);
  config.contribution = const_contribution();
  PullServiceImpl service(cache, backend, recording_dispatch(rec), config);

  const TileKey key = tile_key(leaf_id, k_rev);

  auto done = std::make_shared<RenderCompletion>();
  service.pull(&leaf, one_tile_request(caller_target, StateHandle{}, Deadline::none()), done);

  // The render was dispatched but answers async: recorded pending, not blocked,
  // not yet in the cache.
  CHECK(rec->calls == 1);
  CHECK(counters.requests_issued() == 1);
  CHECK_FALSE(done->settled());
  REQUIRE(queue.tiles.size() == 1);
  CHECK_FALSE(cache.lookup(key).has_value());

  // An unsettled poll settles nothing.
  CHECK(arbc::poll_refinements(queue, cache, &counters).empty());

  // On completion the poll inserts the arrival under Visible and emits damage, so
  // a follow-up frame re-plans it as a fresh cache hit.
  queue.tiles.front().done->complete(RenderResult{1.0, true});
  const std::vector<Damage> damage = arbc::poll_refinements(queue, cache, &counters);
  REQUIRE(damage.size() == 1);
  CHECK(counters.follow_up_frames() == 1);
  CHECK(queue.tiles.empty());

  // A re-pull is now a warm hit -- zero further dispatch.
  const int calls_before = rec->calls;
  auto done2 = std::make_shared<RenderCompletion>();
  service.pull(&leaf, one_tile_request(caller_target, StateHandle{}, Deadline::none()), done2);
  CHECK(rec->calls == calls_before);
  CHECK(done2->settled());
}

// enforces: 13-effects-as-operators#pull-is-cache-first
TEST_CASE(
    "pull_service: an operator render bumps operator_renders; an identity pull short-circuits") {
  MarkBackend backend;
  TileCache cache(64u * 1024 * 1024);
  auto rec = std::make_shared<DispatchRecorder>();
  BufferSurface caller_target(256, 256);

  GraphContent leaf;
  const arbc::ObjectId leaf_id{31};
  const arbc::ObjectId op_id{32};

  SECTION("a non-identity operator inline-renders once and caches under its own identity") {
    GraphContent op({&leaf}, std::nullopt);
    const std::unordered_map<const Content*, arbc::ObjectId> ids{{&leaf, leaf_id}, {&op, op_id}};
    CompositorCounters counters;
    PullConfig config;
    config.counters = &counters;
    config.id_of = id_map(ids);
    config.contribution = const_contribution();
    PullServiceImpl service(cache, backend, recording_dispatch(rec), config);

    auto done = std::make_shared<RenderCompletion>();
    service.pull(&op, one_tile_request(caller_target, StateHandle{}, Deadline::none()), done);

    CHECK(counters.operator_renders() == 1);
    CHECK(counters.requests_issued() == 1);
    CHECK(op.renders() == 1);
    // Keyed by its aggregate revision (op + reachable leaf = 2 * k_rev).
    CHECK(cache.lookup(tile_key(op_id, 2 * k_rev)).has_value());
  }

  SECTION("an identity operator issues zero operator renders and no operator-output entry") {
    GraphContent op({&leaf}, std::size_t{0}); // identity(request) == input 0
    const std::unordered_map<const Content*, arbc::ObjectId> ids{{&leaf, leaf_id}, {&op, op_id}};
    CompositorCounters counters;
    PullConfig config;
    config.counters = &counters;
    config.id_of = id_map(ids);
    config.contribution = const_contribution();
    PullServiceImpl service(cache, backend, recording_dispatch(rec), config);

    auto done = std::make_shared<RenderCompletion>();
    service.pull(&op, one_tile_request(caller_target, StateHandle{}, Deadline::none()), done);

    CHECK(counters.operator_renders() == 0); // zero operator renders
    CHECK(counters.requests_issued() == 1);  // the leaf render only
    CHECK(op.renders() == 0);                // the operator never rendered
    CHECK_FALSE(cache.lookup(tile_key(op_id, 2 * k_rev)).has_value()); // no operator entry
    CHECK(cache.lookup(tile_key(leaf_id, k_rev)).has_value());         // the input tile
  }
}

// enforces: 05-recursive-composition#graph-walk-bounds-cycles
TEST_CASE("pull_service: the recursion budget bounds a divergent operator-over-operator pull") {
  MarkBackend backend;
  TileCache cache(64u * 1024 * 1024);
  auto rec = std::make_shared<DispatchRecorder>();
  BufferSurface caller_target(256, 256);

  SECTION("a within-budget chain resolves through to the leaf with no diagnostic") {
    GraphContent leaf;
    GraphContent inner_op({&leaf});
    GraphContent outer_op({&inner_op});
    const std::unordered_map<const Content*, arbc::ObjectId> ids{
        {&leaf, arbc::ObjectId{1}}, {&inner_op, arbc::ObjectId{2}}, {&outer_op, arbc::ObjectId{3}}};
    GraphDiagnostics diag;
    PullConfig config;
    config.id_of = id_map(ids);
    config.contribution = const_contribution();
    config.diagnostics = &diag;
    config.budget = GraphBudget{/*max_depth=*/8};
    PullServiceImpl service(cache, backend, recording_dispatch(rec), config);
    inner_op.service = &service;
    outer_op.service = &service;

    auto done = std::make_shared<RenderCompletion>();
    service.pull(&outer_op, one_tile_request(caller_target, StateHandle{}, Deadline::none()), done);
    CHECK(diag.entries.empty());
    CHECK(done->settled());
  }

  SECTION("a self-referential operator pull exceeds the budget: placeholder + one diagnostic") {
    GraphContent op;
    op.set_inputs({&op}); // op pulls op pulls op ... (a divergent feedback cycle)
    const std::unordered_map<const Content*, arbc::ObjectId> ids{{&op, arbc::ObjectId{7}}};
    GraphDiagnostics diag;
    PullConfig config;
    config.id_of = id_map(ids);
    config.contribution = const_contribution();
    config.diagnostics = &diag;
    config.budget = GraphBudget{/*max_depth=*/4};
    PullServiceImpl service(cache, backend, recording_dispatch(rec), config);
    op.service = &service;

    auto done = std::make_shared<RenderCompletion>();
    service.pull(&op, one_tile_request(caller_target, StateHandle{}, Deadline::none()), done);

    // The descent terminated on the budget (never unbounded), reporting exactly
    // one diagnostic naming the offending content. The completion is settled (the
    // placeholder was selected, not left hanging), and because every level's
    // render propagated the failure no tile was cached for the divergent operator.
    REQUIRE(diag.entries.size() == 1);
    CHECK(diag.entries[0].content == &op);
    CHECK(done->settled());
    CHECK(rec->calls <= 4); // bounded by the budget, no runaway dispatch
    CHECK_FALSE(cache.lookup(tile_key(arbc::ObjectId{7}, k_rev)).has_value());
  }
}

// enforces: 05-recursive-composition#graph-walk-bounds-cycles
TEST_CASE("pull_service: a divergent identity cycle through pull selects the placeholder") {
  MarkBackend backend;
  TileCache cache(64u * 1024 * 1024);
  auto rec = std::make_shared<DispatchRecorder>();
  BufferSurface caller_target(256, 256);

  GraphContent op_a({}, std::size_t{0}); // identity -> input 0
  GraphContent op_b({}, std::size_t{0});
  op_a.set_inputs({&op_b});
  op_b.set_inputs({&op_a}); // opA identity -> opB identity -> opA ...
  const std::unordered_map<const Content*, arbc::ObjectId> ids{{&op_a, arbc::ObjectId{1}},
                                                               {&op_b, arbc::ObjectId{2}}};
  GraphDiagnostics diag;
  PullConfig config;
  config.id_of = id_map(ids);
  config.contribution = const_contribution();
  config.diagnostics = &diag;
  config.budget = GraphBudget{/*max_depth=*/4};
  PullServiceImpl service(cache, backend, recording_dispatch(rec), config);

  auto done = std::make_shared<RenderCompletion>();
  service.pull(&op_a, one_tile_request(caller_target, StateHandle{}, Deadline::none()), done);

  // The identity chain diverged: `resolve_identity` reported one diagnostic and no
  // render was ever dispatched (the pass-through never renders its own output).
  REQUIRE(diag.entries.size() == 1);
  CHECK(rec->calls == 0);
  CHECK(done->settled());
}

TEST_CASE("pull_service: direct_dispatch drives content->render inline, and a null input fails") {
  MarkBackend backend;
  TileCache cache(64u * 1024 * 1024);
  BufferSurface caller_target(256, 256);

  GraphContent leaf;
  const arbc::ObjectId leaf_id{41};
  const std::unordered_map<const Content*, arbc::ObjectId> ids{{&leaf, leaf_id}};
  PullConfig config;
  config.id_of = id_map(ids);
  config.contribution = const_contribution();
  // The default single-threaded dispatch: `content->render` inline (Decision 3).
  PullServiceImpl service(cache, backend, arbc::direct_dispatch(), config);

  SECTION("a miss under direct_dispatch renders inline and caches the tile") {
    auto done = std::make_shared<RenderCompletion>();
    service.pull(&leaf, one_tile_request(caller_target, StateHandle{}, Deadline::none()), done);
    CHECK(leaf.renders() == 1);
    REQUIRE(done->settled());
    CHECK(cache.lookup(tile_key(leaf_id, k_rev)).has_value());
  }

  SECTION("a null input fails the completion, dispatching nothing") {
    auto done = std::make_shared<RenderCompletion>();
    service.pull(nullptr, one_tile_request(caller_target, StateHandle{}, Deadline::none()), done);
    REQUIRE(done->settled());
    const auto settled = done->take();
    REQUIRE(settled.has_value());
    CHECK_FALSE(settled->has_value());
  }
}

// enforces: 13-effects-as-operators#pull-is-cache-first
TEST_CASE(
    "pull_service: render_frame_interactive with pulls == nullptr reproduces the inline fill") {
  MarkBackend backend;
  arbc::Model model;
  const arbc::ObjectId content_id = add_single_layer(model);
  const arbc::DocStatePtr state = model.current();
  GraphContent leaf; // a plain leaf layer
  const auto resolver = [&](arbc::ObjectId id) -> arbc::Content* {
    return id == content_id ? &leaf : nullptr;
  };
  const arbc::Viewport viewport{256, 256, arbc::Affine::identity()}; // one rung-0 tile
  arbc::SurfacePool pool(backend);
  TileCache cache(64u * 1024 * 1024);
  CompositorCounters counters;

  auto frame1 = backend.make_surface(256, 256, arbc::k_working_rgba32f);
  REQUIRE(frame1.has_value());

  // Cold frame: the one covered tile is a fresh-key miss -> one render request and
  // one composite, exactly the pre-task inline fill (the null `pulls` path).
  arbc::render_frame_interactive(*state, resolver, viewport, cache, backend, pool, **frame1,
                                 Deadline::none(), std::nullopt, nullptr, &counters);
  CHECK(counters.requests_issued() == 1);
  CHECK(counters.composites() == 1);
  CHECK(leaf.renders() == 1);

  // Warm frame: the tile plans Fresh -> zero new renders (delta 0), one composite.
  auto frame2 = backend.make_surface(256, 256, arbc::k_working_rgba32f);
  REQUIRE(frame2.has_value());
  arbc::render_frame_interactive(*state, resolver, viewport, cache, backend, pool, **frame2,
                                 Deadline::none(), std::nullopt, nullptr, &counters);
  CHECK(counters.requests_issued() == 1); // delta == 0
  CHECK(counters.composites() == 2);
  CHECK(leaf.renders() == 1); // no second render
}

namespace {

using arbc::AudioBlock;
using arbc::AudioBlockValue;
using arbc::AudioCompletion;
using arbc::AudioDispatch;
using arbc::AudioFacet;
using arbc::AudioRequest;
using arbc::AudioResult;
using arbc::BlockCache;
using arbc::BlockKey;
using arbc::ChannelLayout;
using arbc::PriorityClass;

// A one-block audio request over `buf` (caller-owned, stereo). `start` selects the
// block index the `pull_audio` key derives.
AudioRequest audio_request(AudioBlock& block, std::vector<float>& buf, std::uint32_t rate,
                           std::uint32_t frames, StateHandle snapshot, arbc::Exactness exactness,
                           arbc::Time start) {
  buf.assign(static_cast<std::size_t>(frames) * arbc::channel_count(ChannelLayout::Stereo), 0.0F);
  block = AudioBlock{buf.data(), frames, ChannelLayout::Stereo, rate};
  const std::int64_t fpf = arbc::Time::flicks_per_second / static_cast<std::int64_t>(rate);
  return AudioRequest{
      arbc::TimeRange{start, arbc::Time{start.flicks + static_cast<std::int64_t>(frames) * fpf}},
      rate,
      ChannelLayout::Stereo,
      block,
      exactness,
      snapshot,
  };
}

// Records each dispatch's verbatim request fields and settles `done` once as an
// exact block at the requested rate -- the audio twin of `DispatchRecorder`.
struct AudioDispatchRecorder {
  int calls{0};
  Content* last_input{nullptr};
  std::uint32_t last_rate{0};
  arbc::Exactness last_exactness{arbc::Exactness::BestEffort};
  StateHandle last_snapshot{};

  void run(Content* content, const AudioRequest& request, std::shared_ptr<AudioCompletion> done) {
    ++calls;
    last_input = content;
    last_rate = request.sample_rate;
    last_exactness = request.exactness;
    last_snapshot = request.snapshot;
    done->complete(AudioResult{request.sample_rate, true});
  }
};

AudioDispatch recording_audio_dispatch(const std::shared_ptr<AudioDispatchRecorder>& rec) {
  return
      [rec](Content* content, const AudioRequest& request, std::shared_ptr<AudioCompletion> done) {
        rec->run(content, request, std::move(done));
      };
}

// A leaf whose audio facet either settles INLINE (returns an AudioResult) or defers
// (returns nullopt, leaving `done` live) -- to exercise both `direct_audio_dispatch`
// folding paths.
class InlineOrAsyncAudioLeaf final : public Content {
public:
  explicit InlineOrAsyncAudioLeaf(bool inline_settle) : d_facet(inline_settle) {}
  std::optional<arbc::Rect> bounds() const override { return std::nullopt; }
  arbc::Stability stability() const override { return arbc::Stability::Static; }
  std::optional<arbc::TimeRange> time_extent() const override { return std::nullopt; }
  std::optional<RenderResult> render(const RenderRequest&,
                                     std::shared_ptr<RenderCompletion> done) override {
    done->fail(arbc::RenderError::ContentFailed);
    return std::nullopt;
  }
  AudioFacet* audio() override { return &d_facet; }

private:
  class Facet final : public AudioFacet {
  public:
    explicit Facet(bool inline_settle) : d_inline(inline_settle) {}
    std::optional<arbc::TimeRange> audio_extent() const override { return std::nullopt; }
    arbc::Stability audio_stability() const override { return arbc::Stability::Static; }
    std::optional<AudioResult> render_audio(const AudioRequest& request,
                                            std::shared_ptr<AudioCompletion>) override {
      if (d_inline) {
        return AudioResult{request.sample_rate, true};
      }
      return std::nullopt; // defer: `done` stays live for a later off-thread settle
    }

  private:
    bool d_inline;
  };
  Facet d_facet;
};

} // namespace

TEST_CASE(
    "direct_audio_dispatch folds an inline result, fails a facet-less content, defers a miss") {
  std::vector<float> buf(16, 0.0F);
  AudioBlock block{buf.data(), 8, ChannelLayout::Stereo, 48'000};
  const AudioRequest req{arbc::TimeRange{arbc::Time::zero(), arbc::Time{100}},
                         48'000,
                         ChannelLayout::Stereo,
                         block,
                         arbc::Exactness::Exact,
                         StateHandle{}};
  const AudioDispatch dispatch = arbc::direct_audio_dispatch();

  SECTION("an inline-settling facet completes the completion once") {
    InlineOrAsyncAudioLeaf leaf(/*inline_settle=*/true);
    auto done = std::make_shared<AudioCompletion>();
    dispatch(&leaf, req, done);
    REQUIRE(done->settled());
    const auto settled = done->take();
    REQUIRE(settled.has_value());
    REQUIRE(settled->has_value());
    CHECK((*settled)->achieved_rate == 48'000);
  }

  SECTION("a facet-less content fails ResourceUnavailable once") {
    GraphContent visual; // no audio facet
    auto done = std::make_shared<AudioCompletion>();
    dispatch(&visual, req, done);
    REQUIRE(done->settled());
    const auto settled = done->take();
    REQUIRE(settled.has_value());
    CHECK_FALSE(settled->has_value());
  }

  SECTION("a deferring facet leaves the completion live for a later off-thread settle") {
    InlineOrAsyncAudioLeaf leaf(/*inline_settle=*/false);
    auto done = std::make_shared<AudioCompletion>();
    dispatch(&leaf, req, done);
    CHECK_FALSE(done->settled()); // the worker will settle it later
  }
}

// enforces: 12-audio#pull-audio-is-cache-first-single-settle
TEST_CASE("pull_audio serves a resident exact-fresh block cache-first with zero dispatch") {
  MarkBackend backend;
  TileCache cache(64u * 1024 * 1024);
  BlockCache blocks(64u * 1024 * 1024);

  GraphContent leaf; // a non-operator leaf -> revision == its own contribution
  const arbc::ObjectId leaf_id{71};
  const std::unordered_map<const Content*, arbc::ObjectId> ids{{&leaf, leaf_id}};

  CompositorCounters counters;
  auto rec = std::make_shared<AudioDispatchRecorder>();
  PullConfig config;
  config.counters = &counters;
  config.id_of = id_map(ids);
  config.contribution = const_contribution();
  config.audio_dispatch = recording_audio_dispatch(rec);
  config.blocks = &blocks;
  PullServiceImpl service(cache, backend, recording_dispatch(std::make_shared<DispatchRecorder>()),
                          config);

  const std::uint32_t rate = 48'000;
  const std::uint32_t frames = 8;
  std::vector<float> buf;
  AudioBlock block{};
  const AudioRequest req = audio_request(block, buf, rate, frames, StateHandle{},
                                         arbc::Exactness::Exact, arbc::Time::zero());

  // Pre-populate the cache under the exact key `pull_audio` will compute.
  AudioBlockValue value;
  value.samples.assign(static_cast<std::size_t>(frames) * 2, 0.0F);
  for (std::size_t i = 0; i < value.samples.size(); ++i) {
    value.samples[i] = static_cast<float>(i) + 1.0F; // a distinctive resident block
  }
  value.frames = frames;
  value.layout = ChannelLayout::Stereo;
  value.rate = rate;
  value.meta = AudioResult{rate, true};
  const std::size_t value_bytes = value.samples.size() * sizeof(float);
  const BlockKey key{leaf_id, k_rev, arbc::audio_block_index(req), rate};
  blocks.insert(key, std::move(value), value_bytes, PriorityClass::Visible);

  auto done = std::make_shared<AudioCompletion>();
  service.pull_audio(&leaf, req, done);

  REQUIRE(done->settled());
  const auto settled = done->take();
  REQUIRE(settled.has_value());
  REQUIRE(settled->has_value());
  CHECK((*settled)->achieved_rate == rate);
  CHECK((*settled)->exact);
  CHECK(counters.audio_dispatches() == 0); // ZERO dispatch: served from the cache
  CHECK(rec->calls == 0);
  for (std::uint32_t i = 0; i < frames * 2; ++i) {
    CHECK(buf[i] == static_cast<float>(i) + 1.0F); // the resident samples were copied out
  }
}

// enforces: 12-audio#pull-audio-is-cache-first-single-settle
TEST_CASE("pull_audio dispatches a miss exactly once, carrying the request verbatim") {
  MarkBackend backend;
  TileCache cache(64u * 1024 * 1024);
  BlockCache blocks(64u * 1024 * 1024);

  GraphContent leaf;
  const arbc::ObjectId leaf_id{72};
  const std::unordered_map<const Content*, arbc::ObjectId> ids{{&leaf, leaf_id}};

  CompositorCounters counters;
  auto rec = std::make_shared<AudioDispatchRecorder>();
  PullConfig config;
  config.counters = &counters;
  config.id_of = id_map(ids);
  config.contribution = const_contribution();
  config.audio_dispatch = recording_audio_dispatch(rec);
  config.blocks = &blocks; // empty: the request misses
  PullServiceImpl service(cache, backend, recording_dispatch(std::make_shared<DispatchRecorder>()),
                          config);

  const std::uint32_t rate = 44'100;
  std::vector<float> buf;
  AudioBlock block{};
  const StateHandle snap{};
  const AudioRequest req =
      audio_request(block, buf, rate, 8, snap, arbc::Exactness::Exact, arbc::Time::zero());

  auto done = std::make_shared<AudioCompletion>();
  service.pull_audio(&leaf, req, done);

  CHECK(counters.audio_dispatches() == 1); // exactly one dispatch on the miss
  CHECK(rec->calls == 1);
  CHECK(rec->last_input == &leaf);
  CHECK(rec->last_rate == rate);                        // rate verbatim
  CHECK(rec->last_exactness == arbc::Exactness::Exact); // exactness verbatim
  CHECK(rec->last_snapshot == snap);                    // snapshot verbatim
  REQUIRE(done->settled());                             // settled exactly once, by the dispatch
  const auto settled = done->take();
  REQUIRE(settled.has_value());
  CHECK(settled->has_value());
}

// enforces: 12-audio#pull-audio-is-cache-first-single-settle
TEST_CASE("pull_audio settles the placeholder once when the seam is absent or the input is null") {
  MarkBackend backend;
  TileCache cache(64u * 1024 * 1024);

  GraphContent leaf;
  const arbc::ObjectId leaf_id{73};
  const std::unordered_map<const Content*, arbc::ObjectId> ids{{&leaf, leaf_id}};
  CompositorCounters counters;
  PullConfig config;
  config.counters = &counters;
  config.id_of = id_map(ids);
  config.contribution = const_contribution();
  // No audio_dispatch, no blocks: a service that predates audio.
  PullServiceImpl service(cache, backend, recording_dispatch(std::make_shared<DispatchRecorder>()),
                          config);

  std::vector<float> buf;
  AudioBlock block{};

  SECTION("an unconfigured audio worker settles ResourceUnavailable exactly once") {
    const AudioRequest req = audio_request(block, buf, 48'000, 8, StateHandle{},
                                           arbc::Exactness::Exact, arbc::Time::zero());
    auto done = std::make_shared<AudioCompletion>();
    service.pull_audio(&leaf, req, done);
    REQUIRE(done->settled());
    const auto settled = done->take();
    REQUIRE(settled.has_value());
    CHECK_FALSE(settled->has_value()); // the placeholder failure
    CHECK(counters.audio_dispatches() == 0);
  }

  SECTION("a null input fails once, dispatching nothing") {
    const AudioRequest req = audio_request(block, buf, 48'000, 8, StateHandle{},
                                           arbc::Exactness::Exact, arbc::Time::zero());
    auto done = std::make_shared<AudioCompletion>();
    service.pull_audio(nullptr, req, done);
    REQUIRE(done->settled());
    const auto settled = done->take();
    REQUIRE(settled.has_value());
    CHECK_FALSE(settled->has_value());
    CHECK(counters.audio_dispatches() == 0);
  }
}

// enforces: 12-audio#pull-audio-is-cache-first-single-settle
TEST_CASE("pull_audio bounds a self-dispatching audio cycle by the shared recursion budget") {
  MarkBackend backend;
  TileCache cache(64u * 1024 * 1024);

  GraphContent leaf;
  const arbc::ObjectId leaf_id{74};
  const std::unordered_map<const Content*, arbc::ObjectId> ids{{&leaf, leaf_id}};
  CompositorCounters counters;

  // A dispatch that re-enters pull_audio on the same input: a gain>=1 self-embedding
  // audio cycle. It terminates on GraphBudget.max_depth (doc 12:143, doc 05:66-70),
  // the same backstop `pull` uses one dimension over.
  PullServiceImpl* svc = nullptr;
  AudioDispatch recursive = [&svc](Content* content, const AudioRequest& request,
                                   std::shared_ptr<AudioCompletion> done) {
    auto inner = std::make_shared<AudioCompletion>();
    svc->pull_audio(content, request, inner);
    const auto settled = inner->take();
    if (settled.has_value() && settled->has_value()) {
      done->complete(**settled);
    } else {
      done->fail(arbc::RenderError::ResourceUnavailable);
    }
  };

  PullConfig config;
  config.counters = &counters;
  config.id_of = id_map(ids);
  config.contribution = const_contribution();
  config.audio_dispatch = recursive;
  config.budget.max_depth = 4;
  PullServiceImpl service(cache, backend, recording_dispatch(std::make_shared<DispatchRecorder>()),
                          config);
  svc = &service;

  std::vector<float> buf;
  AudioBlock block{};
  const AudioRequest req = audio_request(block, buf, 48'000, 8, StateHandle{},
                                         arbc::Exactness::Exact, arbc::Time::zero());
  auto done = std::make_shared<AudioCompletion>();
  service.pull_audio(&leaf, req, done);

  // The cycle terminated (no unbounded recursion): the top completion settled
  // exactly once, as the placeholder, and dispatch fired exactly `max_depth` times.
  REQUIRE(done->settled());
  const auto settled = done->take();
  REQUIRE(settled.has_value());
  CHECK_FALSE(settled->has_value());
  CHECK(counters.audio_dispatches() == 4);
}
