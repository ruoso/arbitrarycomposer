// org.arbc.fade identity behavioral counter (refinement Acceptance "Behavioral
// counter (identity)"): a real fade driven through the compositor's
// PullServiceImpl records `operator_renders` delta 0 when its envelope is
// exactly 1 at the request time (the identity short-circuit serves input 0's
// tile directly, no operator render) and delta 1 otherwise (the operator is
// inline-rendered). Never a wall-clock assertion. Models
// src/compositor/t/pull_service.t.cpp's operator/identity counter case, with a
// real FadeContent over a real org.arbc.solid input.

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/cache/key_shapes.hpp>
#include <arbc/cache/keyed_store.hpp>
#include <arbc/compositor/counters.hpp>
#include <arbc/compositor/pull_service.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/kind_fade/fade_content.hpp>
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

// Drive a single pull of `fade` at `time` and return the operator_renders delta.
std::uint64_t operator_renders_for(FadeContent& fade, SolidContent& solid, Time time) {
  CpuBackend backend;
  TileCache cache(64u * 1024 * 1024);
  const std::unordered_map<const Content*, ObjectId> ids{{&solid, ObjectId{1}},
                                                         {&fade, ObjectId{2}}};

  CompositorCounters counters;
  PullConfig config;
  config.counters = &counters;
  config.id_of = id_map(ids);
  config.contribution = [](const Content*) { return std::uint64_t{1}; };
  PullServiceImpl service(cache, backend, direct_dispatch(), config);

  fade.attach(service, backend);

  const auto target = backend.make_surface(256, 256, k_working_rgba32f);
  REQUIRE(target.has_value());
  auto done = std::make_shared<RenderCompletion>();
  service.pull(&fade, one_tile_request(**target, time), done);

  return counters.operator_renders();
}

} // namespace

// enforces: 13-effects-as-operators#identity-plan-issues-no-operator-render
TEST_CASE("org.arbc.fade issues zero operator renders at a fully-open envelope") {
  SolidContent solid{Rgba{0.5F, 0.25F, 0.125F, 1.0F}, Rect{0.0, 0.0, 256.0, 256.0}};
  // No windows -> the envelope is identically 1, so identity() returns input 0
  // at every time and the pull short-circuits to the input's tile.
  FadeContent open_fade{&solid, FadeParams{}};
  CHECK(operator_renders_for(open_fade, solid, Time{500}) == 0U);
}

// enforces: 13-effects-as-operators#identity-plan-issues-no-operator-render
TEST_CASE("org.arbc.fade issues exactly one operator render mid-fade") {
  SolidContent solid{Rgba{0.5F, 0.25F, 0.125F, 1.0F}, Rect{0.0, 0.0, 256.0, 256.0}};
  // Fade-in over [0, 1000): at t = 500 the envelope is 0.5, not identity, so the
  // operator is inline-rendered exactly once.
  FadeContent partial_fade{
      &solid, FadeParams{FadeShape::Linear, FadeWindow{Time{0}, Time{1000}}, std::nullopt}};
  CHECK(operator_renders_for(partial_fade, solid, Time{500}) == 1U);
}
