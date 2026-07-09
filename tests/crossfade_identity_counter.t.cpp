// org.arbc.crossfade identity behavioral counter (refinement Acceptance
// "Behavioral counter (identity)"): a real crossfade driven through the
// compositor's PullServiceImpl records `operator_renders` delta 0 at w == 0 (the
// identity short-circuit serves input 0's tile directly) and delta 0 at w == 1
// (serves input 1's), but delta 1 for an interior w (the operator is
// inline-rendered as a dissolve). Never a wall-clock assertion. Models
// tests/fade_identity_counter.t.cpp, one input arity up.

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/cache/key_shapes.hpp>
#include <arbc/cache/keyed_store.hpp>
#include <arbc/compositor/counters.hpp>
#include <arbc/compositor/pull_service.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/kind_crossfade/crossfade_content.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/media/surface_format.hpp>

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>

using namespace arbc;

namespace {

// The single rung-0 tile a 256x256 region at scale 1.0 covers.
RenderRequest one_tile_request(Surface& target, Time time) {
  return RenderRequest{Rect::from_size(256.0, 256.0),
                       1.0,
                       time,
                       StateHandle{},
                       target,
                       Exactness::BestEffort,
                       Deadline::none()};
}

std::function<ObjectId(const Content*)>
id_map(const std::unordered_map<const Content*, ObjectId>& ids) {
  return [&ids](const Content* c) {
    const auto it = ids.find(c);
    return it != ids.end() ? it->second : ObjectId{};
  };
}

// A crossfade transition over [1000, 2000): w == 0 before it, w == 1 at/after
// its end, an interior dissolve inside.
CrossfadeParams window_params() {
  return CrossfadeParams{CrossfadeShape::Linear, Time{1000}, Time{1000}};
}

// Drive a single pull of `xf` at `time` and return the operator_renders delta.
std::uint64_t operator_renders_for(CrossfadeContent& xf, SolidContent& from, SolidContent& to,
                                   Time time) {
  CpuBackend backend;
  TileCache cache(64u * 1024 * 1024);
  const std::unordered_map<const Content*, ObjectId> ids{
      {&from, ObjectId{1}}, {&to, ObjectId{2}}, {&xf, ObjectId{3}}};

  CompositorCounters counters;
  PullConfig config;
  config.counters = &counters;
  config.id_of = id_map(ids);
  config.contribution = [](const Content*) { return std::uint64_t{1}; };
  PullServiceImpl service(cache, backend, direct_dispatch(), config);

  xf.attach(service, backend);

  const auto target = backend.make_surface(256, 256, k_working_rgba32f);
  REQUIRE(target.has_value());
  auto done = std::make_shared<RenderCompletion>();
  service.pull(&xf, one_tile_request(**target, time), done);

  return counters.operator_renders();
}

} // namespace

// enforces: 13-effects-as-operators#identity-plan-issues-no-operator-render
// enforces: 13-effects-as-operators#crossfade-identity-at-endpoints
TEST_CASE("org.arbc.crossfade issues zero operator renders at each endpoint") {
  SolidContent from{Rgba{0.5F, 0.25F, 0.125F, 1.0F}, Rect{0.0, 0.0, 256.0, 256.0}};
  SolidContent to{Rgba{0.125F, 0.375F, 0.75F, 1.0F}, Rect{0.0, 0.0, 256.0, 256.0}};
  CrossfadeContent xf{&from, &to, window_params()};

  // Before the window (w == 0): identity() returns input 0, so the pull
  // short-circuits to input 0's tile and issues no operator render.
  CHECK(operator_renders_for(xf, from, to, Time{500}) == 0U);
  // After the window (w == 1): identity() returns input 1, likewise zero.
  CHECK(operator_renders_for(xf, from, to, Time{2500}) == 0U);
}

// enforces: 13-effects-as-operators#identity-plan-issues-no-operator-render
// enforces: 13-effects-as-operators#crossfade-identity-at-endpoints
TEST_CASE("org.arbc.crossfade issues exactly one operator render mid-transition") {
  SolidContent from{Rgba{0.5F, 0.25F, 0.125F, 1.0F}, Rect{0.0, 0.0, 256.0, 256.0}};
  SolidContent to{Rgba{0.125F, 0.375F, 0.75F, 1.0F}, Rect{0.0, 0.0, 256.0, 256.0}};
  CrossfadeContent xf{&from, &to, window_params()};

  // Interior (w == 0.5 at t = 1500): not identity, so the operator is
  // inline-rendered exactly once as a dissolve.
  CHECK(operator_renders_for(xf, from, to, Time{1500}) == 1U);
}
