// An ASYNC operator input, interactively (`runtime.interactive_pull_wiring`,
// Constraint 6 / Decision 4). Wiring `PullConfig::pending` into the interactive
// frame's pull service makes the driver capable, for the first time, of an operator
// INPUT answering asynchronously -- and that arrival's damage names the input's
// identity, which for an operator's `$ref`'d child is a SYNTHESIZED id
// (`pull_identity.hpp:26-30`), disjoint from every model id by construction.
//
// `map_damage_to_device` matches damage against LAYER ROOTS only
// (`damage_planning.cpp:39`: `layer.content == d.object`), and an operator's input
// child is not a layer root. So left unrouted that arrival maps to zero device rects:
// `schedule_follow_up` is false, the refined tile sits unread in the cache, and no
// frame is ever scheduled to composite it -- a silent violation of doc 02:69-71
// ("async results that arrive later produce damage for their region, scheduling a
// follow-up frame"). The driver therefore routes every arrival up through the
// compositor's `route_operator_damage` to the operator layers that show it, and
// carries the ROUTED set, so the follow-up frame re-plans the operator's footprint
// and re-enters the identity delivery branch. The interactive follow-up frame IS the
// export driver's re-composite pass.
//
// This file is that path's regression test: strip the routing and the frame-2
// assertion below fails on its first line.

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/cache/keyed_store.hpp>
#include <arbc/compositor/compositor.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/kind_crossfade/crossfade_content.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/media/pixel_format.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/interactive.hpp>
#include <arbc/runtime/offline_sequence.hpp>
#include <arbc/surface/surface.hpp>
#include <arbc/surface/surface_pool.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

using namespace arbc;

namespace {

constexpr int k_dim = 256;                            // one rung-0 tile at scale 1.0
constexpr auto k_budget = std::chrono::milliseconds(16);
constexpr Rgba k_from_color{0.5F, 0.25F, 0.125F, 1.0F};
constexpr Rgba k_to_color{0.125F, 0.375F, 0.75F, 1.0F};

Rect canvas() { return Rect{0.0, 0.0, static_cast<double>(k_dim), static_cast<double>(k_dim)}; }
Viewport viewport() { return Viewport{k_dim, k_dim, Affine::identity()}; }

// A crossfade over [1000, 2000): w == 0 strictly before it (identity -> input 0).
CrossfadeParams window_params() {
  return CrossfadeParams{CrossfadeShape::Linear, Time{1000}, Time{1000}};
}

// The fake clock puts every deadline instant in the real past, so a frame never
// blocks and the deadline path is deterministic (doc 16:54-62).
InteractiveRenderer::Clock epoch_clock() {
  return [] { return std::chrono::steady_clock::time_point{}; };
}

// The DEFERRING leaf: it fills the tile (the cache-owned surface travels with the
// pending record) but leaves `done` LIVE and returns nullopt, so the render is a
// genuine async miss. The test decides when the result "arrives" -- no thread, no
// sleep, no wall clock. Same shape as `tests/async_external_load.t.cpp`'s deferring
// asset source, one level down.
class DeferringSolid : public Content {
public:
  DeferringSolid(Rgba color, Rect bounds) : d_solid(color, bounds) {}

  std::optional<Rect> bounds() const override { return d_solid.bounds(); }
  Stability stability() const override { return d_solid.stability(); }
  std::optional<TimeRange> time_extent() const override { return d_solid.time_extent(); }

  std::optional<RenderResult> render(const RenderRequest& request,
                                     std::shared_ptr<RenderCompletion> done) override {
    ++d_renders;
    const std::optional<RenderResult> painted = d_solid.render(request, done);
    REQUIRE(painted.has_value()); // the solid always answers inline; we defer its settle
    d_deferred.push_back(Deferred{std::move(done), *painted});
    return std::nullopt;
  }

  // Deliver every outstanding render RIGHT NOW; returns how many settled. A
  // `cancel()`ed completion still settles: cancellation is advisory and does not
  // prevent a later `complete` (doc 03:66,122-123).
  std::size_t settle_all() {
    std::vector<Deferred> firing;
    firing.swap(d_deferred);
    for (const Deferred& d : firing) {
      d.done->complete(d.result);
    }
    return firing.size();
  }

  std::size_t outstanding() const noexcept { return d_deferred.size(); }
  std::size_t renders() const noexcept { return d_renders; }

private:
  struct Deferred {
    std::shared_ptr<RenderCompletion> done;
    RenderResult result;
  };

  SolidContent d_solid;
  std::vector<Deferred> d_deferred;
  std::size_t d_renders{0};
};

std::vector<float> snapshot(const Surface& surface) {
  const std::span<const float> px = std::as_const(surface).span<PixelFormat::Rgba32fLinearPremul>();
  return {px.begin(), px.end()};
}

bool byte_identical(const std::vector<float>& a, const std::vector<float>& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

// The oracle: the same crossfade at the same instant over two SYNCHRONOUS solids,
// through the export driver. The refined interactive frame must converge to exactly
// this -- the whole point of the follow-up frame.
std::vector<float> synchronous_reference(Backend& backend, Time time) {
  SolidContent from{k_from_color, canvas()};
  SolidContent to{k_to_color, canvas()};
  Document doc;
  doc.add_layer(doc.add_content(std::make_shared<CrossfadeContent>(&from, &to, window_params())),
                Affine::identity());

  SequenceRenderer renderer(doc, viewport(), backend);
  const expected<std::unique_ptr<Surface>, SurfaceError> frame = renderer.render_frame_at(time);
  REQUIRE(frame.has_value());
  return snapshot(**frame);
}

} // namespace

// enforces: 02-architecture#async-arrival-emits-damage
// enforces: 13-effects-as-operators#identity-layer-delivers-input-to-frame
TEST_CASE("interactive: an async operator-input arrival schedules the follow-up frame that "
          "re-drives the identity endpoint") {
  CpuBackend backend;
  DeferringSolid from{k_from_color, canvas()}; // the terminal the w == 0 endpoint resolves to
  SolidContent to{k_to_color, canvas()};

  Document doc;
  doc.add_layer(doc.add_content(std::make_shared<CrossfadeContent>(&from, &to, window_params())),
                Affine::identity());

  const DocStatePtr pin = doc.pin();
  const ContentResolver resolve = [&doc](ObjectId id) { return doc.resolve(id); };
  TileCache cache(64U * 1024 * 1024);
  SurfacePool pool(backend);
  const expected<std::unique_ptr<Surface>, SurfaceError> target =
      backend.make_surface(k_dim, k_dim, pin->working_space());
  REQUIRE(target.has_value());
  InteractiveRenderer renderer({}, epoch_clock());

  const Time when{500}; // w == 0: the crossfade is an identity pass-through to `from`

  // --- Frame 1: the endpoint's pull answers asynchronously. -------------------
  // The identity plan issues no operator render; the driver pulls the terminal input,
  // which defers, so the layer keeps its planned placeholder and composites nothing.
  // The deferred render is RECORDED (not dropped) -- `PullConfig::pending` is wired.
  const InteractiveRenderer::FrameOutcome out1 = renderer.render_frame(
      *pin, resolve, viewport(), cache, backend, pool, **target, {}, when, k_budget);
  const std::vector<float> placeholder = snapshot(**target);

  CHECK(renderer.counters().operator_renders() == 0U);
  CHECK(renderer.counters().requests_issued() == 1U); // exactly one: the terminal's tile
  CHECK(renderer.counters().composites() == 0U);      // transparent placeholder
  CHECK_FALSE(out1.schedule_follow_up);               // nothing has arrived yet
  REQUIRE(renderer.pending().tiles.size() == 1U);
  REQUIRE(from.outstanding() == 1U);
  // The frame reached its deadline with nothing settled, so the still-in-flight
  // BestEffort render was cancelled -- advisory only, and this content ignores it.
  CHECK(renderer.pending().tiles.front().done->cancelled());

  // --- The result arrives. ----------------------------------------------------
  REQUIRE(from.settle_all() == 1U);

  // --- Frame 2: the arrival is reaped, and its damage is ROUTED. ---------------
  // Driven purely by the non-empty pending queue: no model damage, no clock advance,
  // an empty dirty region -- so this frame plans and composites NOTHING. It exists to
  // poll. The poll emits `Damage{from's synthesized id, ...}`, which names no layer
  // root; ONLY `route_operator_damage` turns it into damage on the crossfade layer,
  // and only that makes `schedule_follow_up` true. Strip Decision 4's routing and this
  // is the assertion that fails.
  const InteractiveRenderer::FrameOutcome out2 = renderer.render_frame(
      *pin, resolve, viewport(), cache, backend, pool, **target, {}, when, k_budget);

  CHECK(out2.schedule_follow_up);
  CHECK(renderer.counters().follow_up_frames() == 1U); // reaped + inserted under the input's id
  CHECK(renderer.pending().tiles.empty());
  CHECK(renderer.counters().composites() == 0U);       // an empty dirty region paints nothing
  CHECK(byte_identical(placeholder, snapshot(**target)));

  // --- Frame 3: the follow-up frame re-drives the endpoint. -------------------
  // The carried (routed) damage re-plans the crossfade layer's footprint; the identity
  // branch pulls `from` again and now HITS its resident tile, delivering the sharp
  // pixels. Still zero operator renders, still exactly one render ever dispatched.
  renderer.render_frame(*pin, resolve, viewport(), cache, backend, pool, **target, {}, when,
                        k_budget);
  const std::vector<float> refined = snapshot(**target);

  CHECK(renderer.counters().operator_renders() == 0U);
  CHECK(renderer.counters().requests_issued() == 1U); // the cache hit dispatched nothing
  CHECK(from.renders() == 1U);
  CHECK(renderer.counters().composites() == 1U);

  // Coarse-then-refine: the placeholder frame is not the refined frame, and the
  // refined frame is byte-exact to the fully synchronous export of the same document
  // at the same instant.
  CHECK_FALSE(byte_identical(placeholder, refined));
  CHECK(byte_identical(refined, synchronous_reference(backend, when)));
}
