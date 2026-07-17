#include <arbc/audio_engine/lookahead.hpp>
#include <arbc/base/expected.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/rational_time.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/compositor/anchored_viewports.hpp>
#include <arbc/compositor/compositor.hpp>
#include <arbc/compositor/pull_service.hpp> // BlockCache
#include <arbc/contract/content.hpp>
#include <arbc/contract/registry.hpp> // Registry (the DocumentBinding half the settle consumes)
#include <arbc/media/audio_block.hpp> // ChannelLayout, channel_count
#include <arbc/media/surface_format.hpp>
#include <arbc/model/damage.hpp>
#include <arbc/model/model.hpp>
#include <arbc/runtime/audio_worker_pool.hpp>
#include <arbc/runtime/damage_router.hpp>
#include <arbc/runtime/device_monitor.hpp>
#include <arbc/runtime/device_sink.hpp>
#include <arbc/runtime/document.hpp>           // Document (the Document& binding)
#include <arbc/runtime/document_serialize.hpp> // KindBridge
#include <arbc/runtime/host_viewport.hpp>
#include <arbc/runtime/interactive.hpp>
#include <arbc/runtime/lookahead_pump.hpp>
#include <arbc/runtime/pull_identity.hpp>
#include <arbc/runtime/transport.hpp>
#include <arbc/surface/backend.hpp>
#include <arbc/surface/surface.hpp>
#include <arbc/surface/surface_pool.hpp>
#include <arbc/surface/testing/stub_backend.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

// Unit + behavioral tests for the host-facing per-viewport object
// (`arbc::HostViewport`, doc 01 § Viewport / doc 04:49-86, 17:60). Deterministic:
// every clock read is an injected fake clock and the audio master is driven to a
// deterministic state via the fake device sink + `flush_master`; no test reads the
// wall clock or sleeps (doc 16:54-62). A stub backend keeps the file inside
// `runtime`'s declared dependency closure.

namespace {

using arbc::Affine;
using arbc::compose;
using arbc::ObjectId;
using arbc::Time;
using arbc::Vec2;
using arbc::Viewport;

// --- Stub render seams (mirrors interactive.t.cpp) ---------------------------

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

class MarkBackend : public arbc::testing::StubBackend {
public:
  arbc::BackendCaps capabilities() const override { return {}; }
  arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError>
  make_surface(int width, int height, arbc::SurfaceFormat /*format*/) override {
    return std::unique_ptr<arbc::Surface>(std::make_unique<BufferSurface>(width, height));
  }
  void clear(arbc::Surface& surface, float, float, float, float) override {
    std::span<std::byte> bytes = surface.cpu_bytes();
    std::memset(bytes.data(), 0, bytes.size_bytes());
  }
  void composite(arbc::Surface& dst, const arbc::Surface& src, const arbc::Affine&,
                 double opacity) override {
    const std::span<const std::byte> s = src.cpu_bytes();
    const unsigned seed = s.empty() ? 0u : std::to_integer<unsigned>(s[0]);
    const auto mark = (static_cast<unsigned>(opacity * 251.0) + 1u + seed) & 0xFFu;
    for (std::byte& b : dst.cpu_bytes()) {
      b = static_cast<std::byte>((std::to_integer<unsigned>(b) + mark) & 0xFFu);
    }
  }
};

void fill_solid(arbc::Surface& target) {
  const std::span<std::byte> bytes = target.cpu_bytes();
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<std::byte>((i * 31u + 7u) & 0xFFu);
  }
}

class SyncSolid : public arbc::Content {
public:
  std::optional<arbc::Rect> bounds() const override { return arbc::Rect{0.0, 0.0, 512.0, 512.0}; }
  arbc::Stability stability() const override { return arbc::Stability::Static; }
  std::optional<arbc::TimeRange> time_extent() const override { return std::nullopt; }
  std::optional<arbc::RenderResult> render(const arbc::RenderRequest& request,
                                           std::shared_ptr<arbc::RenderCompletion>) override {
    fill_solid(request.target);
    return arbc::RenderResult{request.scale, /*exact=*/true};
  }
};

// --- Scene helpers (mirrors anchored_viewports.t.cpp geometry) ---------------

constexpr double k_canvas = 1000.0;
constexpr double k_center = 500.0;
constexpr double k_level_scale = 0.001; // child 1000 units -> 1 parent unit

// `edge` maps CHILD-local -> PARENT-local (the layer transform rebase descends).
Affine level_edge() {
  return compose(Affine::translation(k_center - 0.5, k_center - 0.5),
                 Affine::scaling(k_level_scale, k_level_scale));
}

// A camera framing a composition's center at scale `s` (`max_scale() == s`).
Affine frame_camera(double s) {
  return compose(Affine::translation(k_center, k_center),
                 compose(Affine::scaling(s, s), Affine::translation(-k_center, -k_center)));
}

// comps[0..levels] nested compositions: comps[i] holds one layer placed by
// `level_edge()` whose content is comps[i+1]; the deepest holds a leaf.
std::vector<ObjectId> build_chain(arbc::Model& model, int levels) {
  std::vector<ObjectId> comps;
  auto txn = model.transact();
  const ObjectId leaf = txn.add_content(0);
  for (int i = 0; i <= levels; ++i) {
    comps.push_back(txn.add_composition(k_canvas, k_canvas));
  }
  const ObjectId leaf_layer = txn.add_layer(leaf, Affine::identity());
  txn.attach_layer(comps[static_cast<std::size_t>(levels)], leaf_layer);
  for (int i = 0; i < levels; ++i) {
    const ObjectId l = txn.add_layer(comps[static_cast<std::size_t>(i + 1)], level_edge());
    txn.attach_layer(comps[static_cast<std::size_t>(i)], l);
  }
  REQUIRE(txn.commit().has_value());
  return comps;
}

struct Scene {
  ObjectId content;
  ObjectId layer;
};

Scene add_single_layer(arbc::Model& model) {
  auto txn = model.transact();
  const ObjectId content = txn.add_content(0);
  const ObjectId layer = txn.add_layer(content, Affine::identity());
  // A member of a root composition: the frame walk is composition-scoped and the
  // HostViewport driver sources this (lowest-id) composition as the anchor
  // (compositor.root_composition_frame_walk, doc 05:28-36).
  const ObjectId comp = txn.add_composition(512.0, 512.0);
  txn.attach_layer(comp, layer);
  REQUIRE(txn.commit().has_value());
  return {content, layer};
}

// A commit that flushes model damage to the installed sink (any placement change
// auto-damages the layer once at commit, model.cpp:770), bootstrapping a render.
void bump_damage(arbc::Model& model, ObjectId layer) {
  auto txn = model.transact();
  txn.set_transform(layer, Affine::identity());
  REQUIRE(txn.commit().has_value());
}

bool within_one_rounding(Vec2 a, Vec2 b) {
  const double eps = std::numeric_limits<double>::epsilon();
  const double tol_x = 8.0 * eps * std::max(1.0, std::abs(a.x));
  const double tol_y = 8.0 * eps * std::max(1.0, std::abs(a.y));
  return std::abs(a.x - b.x) <= tol_x && std::abs(a.y - b.y) <= tol_y;
}

arbc::HostViewport::Clock epoch_clock() {
  return [] { return std::chrono::steady_clock::time_point{}; };
}

constexpr auto k_budget = std::chrono::milliseconds(16);

} // namespace

// enforces: 04-transforms-and-infinite-zoom#zoom-out-reanchors-along-anchor-path
TEST_CASE("host_viewport: zoom-out re-anchors outward along the runtime-held anchor path") {
  MarkBackend backend;
  arbc::Model model;
  const std::vector<ObjectId> comps = build_chain(model, /*levels=*/1);
  const auto resolve = [](ObjectId) -> arbc::Content* { return nullptr; };
  arbc::SurfacePool pool(backend);
  arbc::TileCache cache(64u * 1024 * 1024);
  auto target = backend.make_surface(1000, 1000, arbc::k_working_rgba32f);
  REQUIRE(target.has_value());
  arbc::InteractiveRenderer renderer({}, epoch_clock());

  const double s = arbc::k_reanchor_scale_threshold * 2.0; // above the band -> zoom-in
  arbc::HostViewport::Config cfg;
  cfg.viewport = Viewport{1000, 1000, frame_camera(s), comps[0]};
  cfg.budget = k_budget;
  arbc::HostViewport viewport(renderer, model, resolve, backend, pool, cache, **target,
                              epoch_clock(), cfg);

  const Affine c0 = frame_camera(s); // the original (anchor comps[0]) camera

  // Zoom in: the pure rebase re-anchors to comps[1] and pushes {comps[0], edge}.
  const arbc::HostViewport::StepOutcome in = viewport.step();
  REQUIRE(in.reanchor.occurred);
  CHECK(in.reanchor.from == comps[0]);
  CHECK(in.reanchor.to == comps[1]);
  CHECK(viewport.anchor() == comps[1]);
  CHECK(viewport.anchor_depth() == 1);
  CHECK(viewport.reanchor_events() == 1);
  // The camera was rebuilt as the on-demand composition across the descent edge.
  CHECK(viewport.camera() == arbc::reanchor_camera(c0, level_edge()));
  const Affine c_prime = viewport.camera();

  // Now drive the camera below the band (the user zoomed way out) and step: the
  // viewport pops the anchor path and re-anchors upward by inverting the stored edge.
  const Affine c_low = frame_camera(1.0 / (arbc::k_reanchor_scale_threshold * 2.0));
  viewport.set_camera(c_low);
  const arbc::HostViewport::StepOutcome out = viewport.step();
  REQUIRE(out.reanchor.occurred);
  CHECK(out.reanchor.from == comps[1]);
  CHECK(out.reanchor.to == comps[0]);
  CHECK(viewport.anchor() == comps[0]); // anchor restored
  CHECK(viewport.anchor_depth() == 0);
  CHECK(viewport.reanchor_events() == 2);

  // The camera is rebuilt by inverting the stored descent edge.
  const std::optional<Affine> inv = level_edge().inverse();
  REQUIRE(inv.has_value());
  CHECK(viewport.camera() == arbc::reanchor_camera(c_low, *inv));

  // Probe-point continuity: a point in comps[0]-local maps to the same device
  // position before (through the child camera + inverse edge) and after the pop.
  const Vec2 probe{k_center, k_center};
  const Vec2 via_before = c_low.apply(inv->apply(probe));
  const Vec2 via_after = viewport.camera().apply(probe);
  CHECK(within_one_rounding(via_before, via_after));

  // Round-trip identity: a zoom-in then zoom-out of the SAME descent edge restores
  // the original (anchor, matrix) to within one double rounding.
  const Vec2 round_trip = arbc::reanchor_camera(c_prime, *inv).apply(probe);
  CHECK(within_one_rounding(round_trip, c0.apply(probe)));
}

// enforces: 04-transforms-and-infinite-zoom#reanchor-surfaced-as-host-event
TEST_CASE("host_viewport: every re-anchor is surfaced as a host event, quiet frames are not") {
  MarkBackend backend;
  arbc::Model model;
  constexpr int levels = 3;
  const std::vector<ObjectId> comps = build_chain(model, levels);
  const auto resolve = [](ObjectId) -> arbc::Content* { return nullptr; };
  arbc::SurfacePool pool(backend);
  arbc::TileCache cache(64u * 1024 * 1024);
  auto target = backend.make_surface(1000, 1000, arbc::k_working_rgba32f);
  REQUIRE(target.has_value());
  arbc::InteractiveRenderer renderer({}, epoch_clock());

  const double s = arbc::k_reanchor_scale_threshold * 2.0;
  arbc::HostViewport::Config cfg;
  cfg.viewport = Viewport{1000, 1000, frame_camera(s), comps[0]};
  cfg.budget = k_budget;
  arbc::HostViewport viewport(renderer, model, resolve, backend, pool, cache, **target,
                              epoch_clock(), cfg);

  // Zoom in one level per step; each step surfaces occurred == true with the right
  // old/new anchor ids.
  for (int i = 0; i < levels; ++i) {
    viewport.set_camera(frame_camera(s)); // the user keeps zooming into the next child
    const arbc::HostViewport::StepOutcome step = viewport.step();
    REQUIRE(step.reanchor.occurred);
    CHECK(step.reanchor.from == comps[static_cast<std::size_t>(i)]);
    CHECK(step.reanchor.to == comps[static_cast<std::size_t>(i + 1)]);
  }
  CHECK(viewport.reanchor_events() == static_cast<std::uint64_t>(levels));
  CHECK(viewport.anchor_depth() == static_cast<std::size_t>(levels));

  // A quiet, in-band frame surfaces no re-anchor.
  viewport.set_camera(frame_camera(2.0));
  const arbc::HostViewport::StepOutcome quiet = viewport.step();
  CHECK_FALSE(quiet.reanchor.occurred);
  CHECK(quiet.need == arbc::RebaseNeed::none);
  CHECK(viewport.reanchor_events() == static_cast<std::uint64_t>(levels));

  // Zoom back out the whole way: each step pops and surfaces a re-anchor event.
  for (int i = levels; i > 0; --i) {
    viewport.set_camera(frame_camera(1.0 / (arbc::k_reanchor_scale_threshold * 2.0)));
    const arbc::HostViewport::StepOutcome step = viewport.step();
    REQUIRE(step.reanchor.occurred);
    CHECK(step.reanchor.from == comps[static_cast<std::size_t>(i)]);
    CHECK(step.reanchor.to == comps[static_cast<std::size_t>(i - 1)]);
  }
  CHECK(viewport.reanchor_events() == static_cast<std::uint64_t>(2 * levels));
  CHECK(viewport.anchor() == comps[0]);
  CHECK(viewport.anchor_depth() == 0);
}

// enforces: 01-core-concepts#viewport-step-drives-transport-damage-frame
TEST_CASE("host_viewport: a free-running step advances the transport and drives the frame") {
  MarkBackend backend;
  arbc::Model model;
  const Scene scene = add_single_layer(model);
  SyncSolid content;
  const auto resolve = [&](ObjectId id) -> arbc::Content* {
    return id == scene.content ? &content : nullptr;
  };
  arbc::SurfacePool pool(backend);
  arbc::TileCache cache(64u * 1024 * 1024);
  auto target = backend.make_surface(256, 256, arbc::k_working_rgba32f);
  REQUIRE(target.has_value());
  arbc::InteractiveRenderer renderer({}, epoch_clock());

  // A controllable fake clock: the same injected source both the renderer and the
  // viewport read (Constraint 4/8).
  std::chrono::steady_clock::time_point clk{};
  arbc::HostViewport::Clock clock = [&clk] { return clk; };

  arbc::HostViewport::Config cfg;
  cfg.viewport = Viewport{256, 256, Affine::identity()};
  cfg.budget = k_budget;
  arbc::HostViewport viewport(renderer, model, resolve, backend, pool, cache, **target, clock, cfg);

  // Step 1 (prev clock unset -> zero elapsed): a model edit bootstraps the render.
  bump_damage(model, scene.layer);
  viewport.step();
  CHECK(viewport.frames_issued() == 1);
  CHECK(viewport.transport_advances() == 1);
  REQUIRE(viewport.last_frame_time().has_value());
  CHECK(*viewport.last_frame_time() == Time::zero());
  CHECK(viewport.transport().position() == Time::zero());

  // Step 2: advance real time by one second at rate 1/1 -> exactly one second of
  // composition time (round_ties_even(real_elapsed*rate), via Transport::advance).
  clk += std::chrono::seconds(1);
  viewport.step();
  CHECK(viewport.frames_issued() == 2); // composition time moved -> a frame is owed
  CHECK(viewport.transport_advances() == 2);
  CHECK(viewport.transport().position() == Time{Time::flicks_per_second});
  CHECK(*viewport.last_frame_time() == Time{Time::flicks_per_second});

  // Step 3: rate 1/2 halves the composition-time advance for the same real second.
  viewport.transport().set_rate(arbc::Rational(1, 2));
  clk += std::chrono::seconds(1);
  viewport.step();
  CHECK(viewport.transport_advances() == 3);
  CHECK(viewport.transport().position() ==
        Time{Time::flicks_per_second + Time::flicks_per_second / 2});
  CHECK(*viewport.last_frame_time() == viewport.transport().position());
}

namespace {

// A trivial pull that fails every render: the audio master advances the transport
// from the DELIVERED-frame count regardless (each delivered block is silence + an
// underrun, never an inline mix), which is all the video-side chase test needs.
class FailPull final : public arbc::PullService {
public:
  void pull(arbc::ContentRef, const arbc::RenderRequest&,
            std::shared_ptr<arbc::RenderCompletion> done) override {
    done->fail(arbc::RenderError::ContentFailed);
  }
};

// The test-driven fake device sink: it captures the monitor's RT fill callback and
// `deliver(frames)` invokes it -- the fake sink IS the clock (no hardware).
class FakeDeviceSink final : public arbc::DeviceSink {
public:
  explicit FakeDeviceSink(arbc::DeviceFormat fmt) : d_format(fmt) {}
  arbc::DeviceFormat format() const override { return d_format; }
  void start(arbc::DeviceFillCallback fill) override { d_fill = std::move(fill); }
  void stop() override { d_fill = nullptr; }
  void deliver(std::uint32_t frames) {
    std::vector<float> buf(static_cast<std::size_t>(frames) * arbc::channel_count(d_format.layout),
                           0.0F);
    if (d_fill) {
      d_fill(buf.data(), frames);
    }
  }

private:
  arbc::DeviceFormat d_format;
  arbc::DeviceFillCallback d_fill;
};

constexpr std::uint32_t k_rate = 48'000;
constexpr std::uint32_t k_block_frames = 32;

} // namespace

// enforces: 01-core-concepts#viewport-step-drives-transport-damage-frame
TEST_CASE("host_viewport: an audio-mastered viewport chases the playhead and never advances") {
  MarkBackend backend;
  arbc::Model video_model;
  const Scene scene = add_single_layer(video_model);
  SyncSolid content;
  const auto resolve = [&](ObjectId id) -> arbc::Content* {
    return id == scene.content ? &content : nullptr;
  };
  arbc::SurfacePool pool(backend);
  arbc::TileCache cache(64u * 1024 * 1024);
  auto target = backend.make_surface(256, 256, arbc::k_working_rgba32f);
  REQUIRE(target.has_value());
  arbc::InteractiveRenderer renderer({}, epoch_clock());

  arbc::HostViewport::Config cfg;
  cfg.viewport = Viewport{256, 256, Affine::identity()};
  cfg.budget = k_budget;
  arbc::HostViewport viewport(renderer, video_model, resolve, backend, pool, cache, **target,
                              epoch_clock(), cfg);

  // --- The audio master over the viewport's transport (video chases audio) ------
  arbc::Model audio_model;
  ObjectId audio_comp{};
  ObjectId audio_content{};
  {
    auto txn = audio_model.transact();
    audio_content = txn.add_content(0);
    const ObjectId l = txn.add_layer(audio_content, Affine::identity());
    audio_comp = txn.add_composition(0.0, 0.0);
    txn.attach_layer(audio_comp, l);
    REQUIRE(txn.commit().has_value());
  }
  const arbc::DocStatePtr audio_doc = audio_model.current();
  SyncSolid audio_leaf;
  const auto audio_resolve = [&](ObjectId id) -> arbc::Content* {
    return id == audio_content ? &audio_leaf : nullptr;
  };

  arbc::BlockCache blocks{16u * 1024 * 1024};
  FailPull pull;
  arbc::LookaheadRingConfig ringcfg;
  ringcfg.composition = audio_comp;
  ringcfg.resolve = audio_resolve;
  ringcfg.sample_rate = k_rate;
  ringcfg.layout = arbc::ChannelLayout::Stereo;
  ringcfg.block_frames = k_block_frames;
  ringcfg.contribution = [rev = audio_doc->revision()](ObjectId) { return rev; };
  arbc::LookaheadRing ring(*audio_doc, pull, ringcfg);

  arbc::AudioWorkerPoolConfig apoolcfg;
  apoolcfg.worker_count = 0;
  arbc::AudioWorkerPool apool(apoolcfg);

  std::atomic<arbc::DeviceMonitor*> monptr{nullptr};
  arbc::LookaheadPumpConfig pumpcfg;
  pumpcfg.horizon = Time::zero();
  pumpcfg.resolve = audio_resolve;
  pumpcfg.sample_rate = k_rate;
  pumpcfg.layout = arbc::ChannelLayout::Stereo;
  pumpcfg.block_frames = k_block_frames;
  pumpcfg.tick_period = std::chrono::hours(1);
  pumpcfg.tick_source = [] { return std::uint64_t{0}; };
  pumpcfg.playhead_source = [&monptr] {
    arbc::DeviceMonitor* m = monptr.load(std::memory_order_acquire);
    return m != nullptr ? m->playhead_snapshot() : Time::zero();
  };
  arbc::LookaheadPump pump(ring, blocks, apool, pumpcfg);

  FakeDeviceSink sink{arbc::DeviceFormat{k_rate, arbc::ChannelLayout::Stereo}};
  arbc::DeviceMonitorConfig moncfg;
  moncfg.working_rate = k_rate;
  moncfg.working_layout = arbc::ChannelLayout::Stereo;
  moncfg.block_frames = k_block_frames;
  moncfg.master_period = std::chrono::hours(1);
  moncfg.camera_source = viewport.camera_source(); // wire the live camera (camera-follow seam)
  arbc::DeviceMonitor monitor(viewport.transport(), pump, sink, moncfg);
  monptr.store(&monitor, std::memory_order_release);

  // The viewport chases the mastered playhead snapshot; it must NOT advance the
  // transport (the device monitor is its sole mutator).
  viewport.set_playhead_source([&monitor] { return monitor.playhead_snapshot(); });

  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(k_rate);

  // The device consumes one block; the master advances the transport by K/rate.
  sink.deliver(k_block_frames);
  monitor.flush_master();
  const Time snap = monitor.playhead_snapshot();
  REQUIRE(snap == Time{static_cast<std::int64_t>(k_block_frames) * fpf});

  bump_damage(video_model, scene.layer); // bootstrap the first render
  viewport.step();
  CHECK(viewport.frames_issued() == 1);
  CHECK(viewport.transport_advances() == 0); // audio-mastered: the viewport never advances
  REQUIRE(viewport.last_frame_time().has_value());
  CHECK(*viewport.last_frame_time() == snap); // composition time chases the snapshot

  // A further block advances the master; the viewport's next step chases the new
  // snapshot (composition time moved -> a frame is owed) and still never advances.
  sink.deliver(k_block_frames);
  monitor.flush_master();
  const Time snap2 = monitor.playhead_snapshot();
  REQUIRE(snap2 == Time{2 * static_cast<std::int64_t>(k_block_frames) * fpf});

  viewport.step();
  CHECK(viewport.frames_issued() == 2);
  CHECK(viewport.transport_advances() == 0);
  CHECK(*viewport.last_frame_time() == snap2);

  pump.request_stop();
}

// enforces: 02-architecture#idle-viewport-issues-no-frames
TEST_CASE("host_viewport: an idle viewport issues no frames after its bootstrap frame") {
  MarkBackend backend;
  arbc::Model model;
  const Scene scene = add_single_layer(model);
  SyncSolid content;
  const auto resolve = [&](ObjectId id) -> arbc::Content* {
    return id == scene.content ? &content : nullptr;
  };
  arbc::SurfacePool pool(backend);
  arbc::TileCache cache(64u * 1024 * 1024);
  auto target = backend.make_surface(256, 256, arbc::k_working_rgba32f);
  REQUIRE(target.has_value());
  arbc::InteractiveRenderer renderer({}, epoch_clock());

  arbc::HostViewport::Config cfg;
  cfg.viewport = Viewport{256, 256, Affine::identity()};
  cfg.budget = k_budget;
  // A constant (epoch) clock: real time never advances, so the transport never
  // moves the playhead -- a genuinely still scene.
  arbc::HostViewport viewport(renderer, model, resolve, backend, pool, cache, **target,
                              epoch_clock(), cfg);

  // The first step is the BOOTSTRAP frame (doc 02 -- a never-rendered viewport is not
  // still, it is stale): the scene's commits predate the sink install, so no damage is
  // drained, yet the viewport composites the scene once.
  viewport.step();
  CHECK(viewport.frames_issued() == 1);

  // Idleness is measured AFTER the first composite: no pending damage, no follow-up
  // owed, no scene motion -> zero further render invocations (a still scene costs
  // nothing).
  for (int i = 0; i < 5; ++i) {
    const arbc::HostViewport::StepOutcome step = viewport.step();
    CHECK_FALSE(step.schedule_follow_up);
  }
  CHECK(viewport.frames_issued() == 1);
  CHECK(viewport.transport_advances() == 6); // free-running, but every advance is zero flicks

  // A no-op set_camera stays inside that idleness (A2): the delta detection is
  // value-based, so re-setting the IDENTICAL camera issues no frame.
  viewport.set_camera(viewport.camera());
  for (int i = 0; i < 3; ++i) {
    viewport.step();
  }
  CHECK(viewport.frames_issued() == 1);

  // The gate is not dead: a model edit produces damage, and the next step renders.
  bump_damage(model, scene.layer);
  viewport.step();
  CHECK(viewport.frames_issued() == 2);
}

// enforces: 01-core-concepts#multiple-viewports-observe-one-composition
TEST_CASE("host_viewport: one DamageRouter fans a commit out to two viewports, each independent") {
  MarkBackend backend;
  arbc::Model model;
  const Scene scene = add_single_layer(model);
  SyncSolid content;
  const auto resolve = [&](ObjectId id) -> arbc::Content* {
    return id == scene.content ? &content : nullptr;
  };
  arbc::SurfacePool pool(backend);
  arbc::TileCache cache(64u * 1024 * 1024);
  auto target_a = backend.make_surface(256, 256, arbc::k_working_rgba32f);
  auto target_b = backend.make_surface(256, 256, arbc::k_working_rgba32f);
  REQUIRE(target_a.has_value());
  REQUIRE(target_b.has_value());
  arbc::InteractiveRenderer renderer({}, epoch_clock());

  // One router occupies the model's single damage slot; both viewports register
  // with it (never with the model directly).
  arbc::DamageRouter router(model);

  arbc::HostViewport::Config cfg_a;
  cfg_a.viewport = Viewport{256, 256, Affine::identity()};
  cfg_a.budget = k_budget;
  cfg_a.router = &router;
  arbc::HostViewport vp_a(renderer, model, resolve, backend, pool, cache, **target_a, epoch_clock(),
                          cfg_a);

  std::optional<arbc::HostViewport> vp_b;
  arbc::HostViewport::Config cfg_b = cfg_a;
  vp_b.emplace(renderer, model, resolve, backend, pool, cache, **target_b, epoch_clock(), cfg_b);
  CHECK(router.registered() == 2);

  // A single committed edit fans out to BOTH viewports' accumulators: one flush,
  // once per registrant (deliveries == registrants).
  bump_damage(model, scene.layer);
  CHECK(router.deliveries() == 2);

  // Each viewport drains that batch into its own frame.
  vp_a.step();
  vp_b->step();
  CHECK(vp_a.frames_issued() == 1);
  CHECK(vp_b->frames_issued() == 1);

  // Destroying one viewport's registration (RAII) stops delivery to it; the other
  // keeps receiving subsequent commits.
  vp_b.reset();
  CHECK(router.registered() == 1);

  bump_damage(model, scene.layer);
  CHECK(router.deliveries() == 3); // one more delivery, to the surviving viewport only
  vp_a.step();
  CHECK(vp_a.frames_issued() == 2);
}

// enforces: 11-time-and-video#transports-observe-composition-independently
TEST_CASE("host_viewport: two viewports over one document observe it at independent instants") {
  MarkBackend backend;
  arbc::Model model;
  const Scene scene = add_single_layer(model);
  SyncSolid content;
  const auto resolve = [&](ObjectId id) -> arbc::Content* {
    return id == scene.content ? &content : nullptr;
  };
  arbc::SurfacePool pool(backend);
  arbc::TileCache cache(64u * 1024 * 1024);
  auto target_a = backend.make_surface(256, 256, arbc::k_working_rgba32f);
  auto target_b = backend.make_surface(256, 256, arbc::k_working_rgba32f);
  REQUIRE(target_a.has_value());
  REQUIRE(target_b.has_value());
  arbc::InteractiveRenderer renderer({}, epoch_clock());

  arbc::DamageRouter router(model);

  arbc::HostViewport::Config cfg;
  cfg.viewport = Viewport{256, 256, Affine::identity()};
  cfg.budget = k_budget;
  cfg.router = &router;
  arbc::HostViewport vp_a(renderer, model, resolve, backend, pool, cache, **target_a, epoch_clock(),
                          cfg);
  arbc::HostViewport vp_b(renderer, model, resolve, backend, pool, cache, **target_b, epoch_clock(),
                          cfg);

  // Each viewport owns its own transport (per-viewport value state, doc 11:88-93);
  // seek them to distinct instants on the shared composition axis.
  const Time ta{7 * Time::flicks_per_second};
  const Time tb{3 * Time::flicks_per_second};
  vp_a.transport().seek(ta);
  vp_b.transport().seek(tb);

  // With fan-out damage in play, a single commit reaches both, and each samples the
  // shared current() document at ITS OWN transport instant.
  bump_damage(model, scene.layer);
  vp_a.step();
  vp_b.step();
  CHECK(vp_a.frames_issued() == 1);
  CHECK(vp_b.frames_issued() == 1);
  REQUIRE(vp_a.last_frame_time().has_value());
  REQUIRE(vp_b.last_frame_time().has_value());
  CHECK(*vp_a.last_frame_time() == ta);
  CHECK(*vp_b.last_frame_time() == tb);

  // Seeking one viewport's transport leaves the other's playhead unchanged.
  const Time ta2{11 * Time::flicks_per_second};
  vp_a.transport().seek(ta2);
  CHECK(vp_a.transport().position() == ta2);
  CHECK(vp_b.transport().position() == tb); // untouched

  // The moved viewport re-renders at its new instant; the other stays put.
  bump_damage(model, scene.layer);
  vp_a.step();
  vp_b.step();
  CHECK(*vp_a.last_frame_time() == ta2);
  CHECK(*vp_b.last_frame_time() == tb);
}

// enforces: 05-recursive-composition#deferred-external-child-installs-live
TEST_CASE("host_viewport: the settle hook runs at the top of the frame, before pin and drain") {
  // `runtime.async_external_load` Decision 7: the external-arrival settle belongs in doc 02
  // step 1 (collect damage), because an arrival IS damage. This pins the ORDERING that makes
  // "the placeholder is replaced live" true in one frame rather than two: the hook runs BEFORE
  // the pin (so the frame renders the revision the install published, not the stale one) and
  // BEFORE the damage drain (so the install's own damage is in the set this very frame plans
  // from). Get either backwards and the newly-arrived child sits invisible until something
  // unrelated damages the scene.
  //
  // The hook is driven with a stand-in that publishes exactly what a real settle publishes -- a
  // transaction that damages the embedding layer and returns an install count. The real
  // `settle_external_loads` over a real deferring `AssetSource` is proven end to end in
  // tests/async_external_load.t.cpp; what is under test HERE is the call site.
  MarkBackend backend;
  arbc::Model model;
  const Scene scene = add_single_layer(model);
  SyncSolid content;
  const auto resolve = [&](ObjectId id) -> arbc::Content* {
    return id == scene.content ? &content : nullptr;
  };
  arbc::SurfacePool pool(backend);
  arbc::TileCache cache(64u * 1024 * 1024);
  auto target = backend.make_surface(256, 256, arbc::k_working_rgba32f);
  REQUIRE(target.has_value());
  arbc::InteractiveRenderer renderer({}, epoch_clock());

  int settles = 0;
  bool arrival_pending = true;
  std::uint64_t revision_at_settle = 0;

  arbc::HostViewport::Config cfg;
  cfg.viewport = Viewport{256, 256, Affine::identity()};
  cfg.budget = k_budget;
  cfg.settle_external_loads = [&]() -> std::size_t {
    ++settles;
    if (!arrival_pending) {
      return 0; // a settle with nothing ready costs one queue check and publishes nothing
    }
    arrival_pending = false;
    bump_damage(model, scene.layer); // the install's commit: a new revision, carrying damage
    revision_at_settle = model.current()->revision();
    return 1;
  };
  arbc::HostViewport viewport(renderer, model, resolve, backend, pool, cache, **target,
                              epoch_clock(), cfg);

  // Frame 1: the arrival settles, and the SAME frame renders it. The hook ran, the damage it
  // published was drained by this step, and the frame issued.
  const arbc::HostViewport::StepOutcome first = viewport.step();
  static_cast<void>(first);
  CHECK(settles == 1);
  CHECK(viewport.external_loads_settled() == 1);
  CHECK(viewport.frames_issued() == 1);
  // The frame pinned AFTER the settle: the stamp the renderer recorded for the content it
  // composited is the one the INSTALL's version carries, not the one that preceded it. Under
  // per-object revisions (`model.per_object_revision`) the renderer tracks a prior stamp per
  // content rather than one document-global scalar, so the "which version did the frame
  // pin?" question is asked of the content the frame actually composited.
  const arbc::DocStatePtr settled = model.current();
  REQUIRE(settled->revision() == revision_at_settle);
  const auto settled_ids = arbc::build_pull_identity_map(*settled, resolve);
  const auto settled_stamps = arbc::build_pull_stamp_map(*settled, *settled_ids);
  REQUIRE(renderer.prior_stamp(scene.content).has_value());
  CHECK(*renderer.prior_stamp(scene.content) ==
        arbc::pull_contribution_of(settled_ids, settled_stamps)(&content));

  // Frame 2: the hook is still called (arrivals are polled, not predicted), settles nothing,
  // and -- with no damage, no follow-up owed and a still scene -- issues no frame at all. The
  // seam costs an idle viewport exactly one queue check per step.
  const arbc::HostViewport::StepOutcome second = viewport.step();
  CHECK_FALSE(second.schedule_follow_up);
  CHECK(settles == 2);
  CHECK(viewport.external_loads_settled() == 1);
  CHECK(viewport.frames_issued() == 1);

  // And a viewport with NO hook configured never calls one and behaves exactly as before.
  arbc::HostViewport::Config bare;
  bare.viewport = Viewport{256, 256, Affine::identity()};
  bare.budget = k_budget;
  arbc::Model other;
  const Scene other_scene = add_single_layer(other);
  const auto other_resolve = [&](ObjectId id) -> arbc::Content* {
    return id == other_scene.content ? &content : nullptr;
  };
  arbc::HostViewport plain(renderer, other, other_resolve, backend, pool, cache, **target,
                           epoch_clock(), bare);
  plain.step();
  CHECK(plain.external_loads_settled() == 0);
}

// --- The `Document&` binding (runtime.host_viewport_document_binding) ---------

namespace {

// The Document-side twin of `add_single_layer`: one solid content, one layer, minted through
// the DOCUMENT's own edit seams (so the content is bound in its side-map and `doc.resolve`
// serves it -- which is precisely what the Document-bound viewport is supposed to find without
// being told).
Scene add_single_layer(arbc::Document& doc) {
  const ObjectId content = doc.add_content(std::make_shared<SyncSolid>());
  const ObjectId layer = doc.add_layer(content, Affine::identity());
  // A member of a root composition, so the composition-scoped frame walk draws it
  // (the HostViewport driver sources this lowest-id composition as the anchor).
  const ObjectId comp = doc.add_composition(512.0, 512.0);
  doc.attach_layer(comp, layer);
  return {content, layer};
}

// A viewport `Config` at the size every test in this file renders at.
arbc::HostViewport::Config doc_config() {
  arbc::HostViewport::Config cfg;
  cfg.viewport = Viewport{256, 256, Affine::identity()};
  cfg.budget = k_budget;
  return cfg;
}

} // namespace

// enforces: 01-core-concepts#viewport-binds-to-document
TEST_CASE("host_viewport: a Document-bound viewport composites byte-for-byte what a hand-wired "
          "Model-and-resolver viewport does") {
  // The binding changes the WIRING and nothing else. Two viewports over the same scene -- one
  // constructed against a `Document` with no host-supplied resolver at all, one against a bare
  // `Model` with the resolver, the damage sink and the settle hook assembled by hand, which is
  // the only shape a host had before this task -- must paint identical bytes.
  MarkBackend backend;
  // One renderer EACH: `InteractiveRenderer` carries cross-frame state (the revision it last
  // completed), and these two viewports observe two different models. Sharing one would gate
  // the second viewport's frame against the first's revision -- a property of the renderer, not
  // of the binding under test.
  arbc::InteractiveRenderer renderer_a({}, epoch_clock());
  arbc::InteractiveRenderer renderer_b({}, epoch_clock());

  // (a) the Document-bound wiring: `doc` supplies the resolver AND the sink install.
  arbc::Document doc;
  const Scene doc_scene = add_single_layer(doc);
  arbc::SurfacePool pool_a(backend);
  arbc::TileCache cache_a(64u * 1024 * 1024);
  auto target_a = backend.make_surface(256, 256, arbc::k_working_rgba32f);
  REQUIRE(target_a.has_value());
  arbc::HostViewport bound(renderer_a, doc, arbc::HostViewport::DocumentBinding{}, backend, pool_a,
                           cache_a, **target_a, epoch_clock(), doc_config());

  // (b) the hand-wired `Model&` path, unchanged and still public (Constraint 1).
  arbc::Model model;
  const Scene model_scene = add_single_layer(model);
  SyncSolid content;
  const auto resolve = [&](ObjectId id) -> arbc::Content* {
    return id == model_scene.content ? &content : nullptr;
  };
  arbc::SurfacePool pool_b(backend);
  arbc::TileCache cache_b(64u * 1024 * 1024);
  auto target_b = backend.make_surface(256, 256, arbc::k_working_rgba32f);
  REQUIRE(target_b.has_value());
  arbc::HostViewport hand(renderer_b, model, resolve, backend, pool_b, cache_b, **target_b,
                          epoch_clock(), doc_config());

  // One equivalent edit each, to bootstrap a frame on both.
  doc.set_layer_transform(doc_scene.layer, Affine::identity());
  bump_damage(model, model_scene.layer);
  bound.step();
  hand.step();
  CHECK(bound.frames_issued() == 1);
  CHECK(hand.frames_issued() == 1);

  const std::span<const std::byte> a = (*target_a)->cpu_bytes();
  const std::span<const std::byte> b = (*target_b)->cpu_bytes();
  REQUIRE(a.size() == b.size());
  CHECK(std::equal(a.begin(), a.end(), b.begin()));
  // And the comparison is not vacuous: the Document-bound frame actually resolved the
  // document's content and painted it. A viewport whose resolver returned null everywhere
  // would leave the cleared target at zero.
  CHECK(std::any_of(a.begin(), a.end(), [](std::byte v) { return v != std::byte{0}; }));
}

// enforces: 01-core-concepts#viewport-binds-to-document
// enforces: 01-core-concepts#viewport-step-drives-transport-damage-frame
TEST_CASE("host_viewport: the document's damage reaches a Document-bound viewport's frame with no "
          "set_damage_sink call by the host") {
  MarkBackend backend;
  arbc::Document doc;
  const Scene scene = add_single_layer(doc);
  arbc::SurfacePool pool(backend);
  arbc::TileCache cache(64u * 1024 * 1024);
  auto target = backend.make_surface(256, 256, arbc::k_working_rgba32f);
  REQUIRE(target.has_value());
  arbc::InteractiveRenderer renderer({}, epoch_clock());

  arbc::HostViewport viewport(renderer, doc, arbc::HostViewport::DocumentBinding{}, backend, pool,
                              cache, **target, epoch_clock(), doc_config());

  // The scene was built BEFORE the viewport existed, so its commits predate the sink
  // install: the opening step drains no damage, but it is the BOOTSTRAP frame (doc 02
  // -- the never-rendered viewport is the degenerate mapping delta) and composites the
  // bound scene once.
  viewport.step();
  CHECK(viewport.frames_issued() == 1);

  // A placement change auto-damages the layer, and that damage reaches this viewport's
  // accumulator because the CONSTRUCTOR installed it on the document's sink slot. The host
  // never called `doc.set_damage_sink` -- there is nothing for it to call it WITH.
  doc.set_layer_transform(scene.layer, Affine::translation(4.0, 4.0));
  viewport.step();
  CHECK(viewport.frames_issued() == 2);

  // Still true on the next commit, and an undamaged step in between stays free.
  viewport.step();
  CHECK(viewport.frames_issued() == 2);
  doc.set_layer_transform(scene.layer, Affine::identity());
  viewport.step();
  CHECK(viewport.frames_issued() == 3);
}

// enforces: 02-architecture#idle-viewport-issues-no-frames
TEST_CASE("host_viewport: an idle Document-bound viewport with a settle hook issues no frames "
          "after its bootstrap frame") {
  // A full `DocumentBinding` over a document with no external references: the derived settle
  // hook runs on every step, finds an empty queue, installs nothing -- and, crucially, does NOT
  // wake the frame loop by itself. The seam costs an idle viewport one queue check per step.
  // Idleness is measured AFTER the bootstrap frame (doc 02): the first step composites the
  // bound scene once, and every later still step is free.
  MarkBackend backend;
  arbc::Document doc;
  arbc::KindBridge bridge;
  const arbc::Registry registry;
  const Scene scene = add_single_layer(doc);
  arbc::SurfacePool pool(backend);
  arbc::TileCache cache(64u * 1024 * 1024);
  auto target = backend.make_surface(256, 256, arbc::k_working_rgba32f);
  REQUIRE(target.has_value());
  arbc::InteractiveRenderer renderer({}, epoch_clock());

  arbc::HostViewport viewport(renderer, doc,
                              arbc::HostViewport::DocumentBinding{&bridge, &registry}, backend,
                              pool, cache, **target, epoch_clock(), doc_config());

  viewport.step(); // the bootstrap frame: composites once, settles nothing
  CHECK(viewport.frames_issued() == 1);
  for (int i = 0; i < 5; ++i) {
    const arbc::HostViewport::StepOutcome step = viewport.step();
    CHECK_FALSE(step.schedule_follow_up);
  }
  CHECK(viewport.frames_issued() == 1);
  CHECK(viewport.external_loads_settled() == 0);
  CHECK(doc.pending_external_loads() == 0);

  // The gate is not dead: a document edit still wakes the very next step, and settles nothing.
  doc.set_layer_transform(scene.layer, Affine::identity());
  viewport.step();
  CHECK(viewport.frames_issued() == 2);
  CHECK(viewport.external_loads_settled() == 0);
}

// enforces: 01-core-concepts#multiple-viewports-observe-one-composition
TEST_CASE("host_viewport: two Document-bound viewports fan out through one DamageRouter over one "
          "document") {
  MarkBackend backend;
  arbc::Document doc;
  const Scene scene = add_single_layer(doc);
  arbc::SurfacePool pool(backend);
  arbc::TileCache cache_a(64u * 1024 * 1024);
  arbc::TileCache cache_b(64u * 1024 * 1024);
  auto target_a = backend.make_surface(256, 256, arbc::k_working_rgba32f);
  auto target_b = backend.make_surface(256, 256, arbc::k_working_rgba32f);
  REQUIRE(target_a.has_value());
  REQUIRE(target_b.has_value());
  arbc::InteractiveRenderer renderer({}, epoch_clock());

  // The router occupies the DOCUMENT's single damage slot (the same slot
  // `Document::set_damage_sink` forwards into); both viewports register with it.
  arbc::DamageRouter router(doc);
  arbc::HostViewport::Config cfg = doc_config();
  cfg.router = &router;

  arbc::HostViewport vp_a(renderer, doc, arbc::HostViewport::DocumentBinding{}, backend, pool,
                          cache_a, **target_a, epoch_clock(), cfg);
  std::optional<arbc::HostViewport> vp_b;
  vp_b.emplace(renderer, doc, arbc::HostViewport::DocumentBinding{}, backend, pool, cache_b,
               **target_b, epoch_clock(), cfg);
  CHECK(router.registered() == 2);

  // One committed edit on the document fans out to both accumulators, once each.
  doc.set_layer_transform(scene.layer, Affine::identity());
  CHECK(router.deliveries() == 2);
  vp_a.step();
  vp_b->step();
  CHECK(vp_a.frames_issued() == 1);
  CHECK(vp_b->frames_issued() == 1);

  // Neither viewport STOLE the document's sink slot on its way in: with one of them gone, the
  // router is still the document's sink and still delivers the next batch. Had a Document-bound
  // viewport installed itself directly (the seam a host would otherwise reach for), this
  // delivery would never arrive.
  vp_b.reset();
  CHECK(router.registered() == 1);
  doc.set_layer_transform(scene.layer, Affine::translation(2.0, 2.0));
  CHECK(router.deliveries() == 3);
  vp_a.step();
  CHECK(vp_a.frames_issued() == 2);
}

// --- Camera-change damage (runtime.camera_change_damage, doc 02 § "A camera change is
// device damage") ---------------------------------------------------------------------

namespace {

// A still TWO-layer scene, so the full-viewport repaint below composites a real
// multi-layer plan and a dropped layer would show in the bytes.
struct TwoLayerScene {
  ObjectId content_a;
  ObjectId content_b;
};

TwoLayerScene add_two_layers(arbc::Model& model) {
  auto txn = model.transact();
  const ObjectId a = txn.add_content(0);
  const ObjectId b = txn.add_content(0);
  const ObjectId comp = txn.add_composition(512.0, 512.0);
  txn.attach_layer(comp, txn.add_layer(a, Affine::identity()));
  txn.attach_layer(comp, txn.add_layer(b, Affine::translation(64.0, 64.0)));
  REQUIRE(txn.commit().has_value());
  return {a, b};
}

std::vector<std::byte> snapshot(const arbc::Surface& surface) {
  const std::span<const std::byte> bytes = surface.cpu_bytes();
  return {bytes.begin(), bytes.end()};
}

} // namespace

// enforces: 02-architecture#camera-change-repaints-full-viewport
TEST_CASE("host_viewport: a camera edit repaints the full viewport, byte-identical to a fresh "
          "render at the new camera") {
  MarkBackend backend;
  arbc::Model model;
  const TwoLayerScene scene = add_two_layers(model);
  SyncSolid content;
  const auto resolve = [&](ObjectId id) -> arbc::Content* {
    return (id == scene.content_a || id == scene.content_b) ? &content : nullptr;
  };
  arbc::SurfacePool pool(backend);
  arbc::TileCache cache(64u * 1024 * 1024);
  auto target = backend.make_surface(256, 256, arbc::k_working_rgba32f);
  REQUIRE(target.has_value());
  arbc::InteractiveRenderer renderer({}, epoch_clock());

  arbc::HostViewport::Config cfg;
  cfg.viewport = Viewport{256, 256, Affine::identity()};
  cfg.budget = k_budget;
  arbc::HostViewport viewport(renderer, model, resolve, backend, pool, cache, **target,
                              epoch_clock(), cfg);

  // The bootstrap frame composites the scene at camera A.
  viewport.step();
  REQUIRE(viewport.frames_issued() == 1);
  const std::vector<std::byte> at_a = snapshot(**target);

  // The gesture: a pure translation, and separately a zoom about the viewport center.
  Affine camera_b = Affine::translation(-64.0, -64.0);
  SECTION("a pure translation") {}
  SECTION("a zoom about the viewport center") {
    camera_b = compose(Affine::translation(128.0, 128.0),
                       compose(Affine::scaling(2.0, 2.0), Affine::translation(-128.0, -128.0)));
  }

  // The edit + one step: EXACTLY one frame, repainting the full viewport at the new
  // camera with no host-forged damage and no other host action at all.
  const std::uint64_t composites_at_a = renderer.counters().composites();
  viewport.set_camera(camera_b);
  viewport.step();
  CHECK(viewport.frames_issued() == 2);
  // The repaint really happened (non-vacuous whatever the byte coincidences): the
  // camera-only frame composited tiles.
  CHECK(renderer.counters().composites() > composites_at_a);

  // The correctness bar (doc 02:120-130): the persistent target is byte-identical to a
  // fresh single-pass render at the new camera -- a fresh viewport over an identical
  // scene, whose bootstrap frame plans the same whole viewport.
  arbc::Model fresh_model;
  const TwoLayerScene fresh_scene = add_two_layers(fresh_model);
  const auto fresh_resolve = [&](ObjectId id) -> arbc::Content* {
    return (id == fresh_scene.content_a || id == fresh_scene.content_b) ? &content : nullptr;
  };
  arbc::InteractiveRenderer fresh_renderer({}, epoch_clock());
  arbc::SurfacePool fresh_pool(backend);
  arbc::TileCache fresh_cache(64u * 1024 * 1024);
  auto fresh_target = backend.make_surface(256, 256, arbc::k_working_rgba32f);
  REQUIRE(fresh_target.has_value());
  arbc::HostViewport::Config fresh_cfg;
  fresh_cfg.viewport = Viewport{256, 256, camera_b};
  fresh_cfg.budget = k_budget;
  arbc::HostViewport fresh(fresh_renderer, fresh_model, fresh_resolve, backend, fresh_pool,
                           fresh_cache, **fresh_target, epoch_clock(), fresh_cfg);
  fresh.step();
  REQUIRE(fresh.frames_issued() == 1);
  const std::vector<std::byte> at_b = snapshot(**target);
  CHECK(at_b == snapshot(**fresh_target));

  // And a further still step issues nothing: the camera edit was consumed by its frame.
  viewport.step();
  CHECK(viewport.frames_issued() == 2);
  CHECK(snapshot(**target) == at_b);
}

// enforces: 02-architecture#camera-change-does-not-invalidate
TEST_CASE("host_viewport: camera damage repaints without invalidating -- pan away and back "
          "re-plans entirely from cache") {
  MarkBackend backend;
  arbc::Model model;
  const Scene scene = add_single_layer(model);
  SyncSolid content;
  const auto resolve = [&](ObjectId id) -> arbc::Content* {
    return id == scene.content ? &content : nullptr;
  };
  arbc::SurfacePool pool(backend);
  arbc::TileCache cache(64u * 1024 * 1024); // ample: nothing is ever evicted for capacity
  auto target = backend.make_surface(256, 256, arbc::k_working_rgba32f);
  REQUIRE(target.has_value());
  arbc::InteractiveRenderer renderer({}, epoch_clock());

  arbc::HostViewport::Config cfg;
  cfg.viewport = Viewport{256, 256, Affine::identity()};
  cfg.budget = k_budget;
  arbc::HostViewport viewport(renderer, model, resolve, backend, pool, cache, **target,
                              epoch_clock(), cfg);

  // Bootstrap at camera A: the visible tiles render and land in the cache.
  viewport.step();
  const std::uint64_t after_a = renderer.counters().requests_issued();
  CHECK(after_a > 0);

  // Pan to B: the newly exposed tiles render (renders happened FOR B)...
  viewport.set_camera(Affine::translation(-64.0, -64.0));
  viewport.step();
  const std::uint64_t after_b = renderer.counters().requests_issued();
  CHECK(after_b > after_a);

  // ...and back to A: camera damage invalidated NOTHING, so the third frame re-plans
  // entirely from cache -- a requests_issued delta of exactly 0 (all hits), while the
  // frame itself really issued and really composited. Never a wall-clock assertion.
  viewport.set_camera(Affine::identity());
  const std::uint64_t composites_before = renderer.counters().composites();
  viewport.step();
  CHECK(viewport.frames_issued() == 3);
  CHECK(renderer.counters().requests_issued() == after_b); // delta 0: nothing was evicted
  CHECK(renderer.counters().composites() > composites_before);
}

// enforces: 02-architecture#first-step-composites-bound-scene
TEST_CASE("host_viewport: the first step composites a bound scene whose commits all predate the "
          "binding") {
  MarkBackend backend;

  // (a) the viewport under test: the whole scene is committed BEFORE the binding, so
  // its damage sink never sees a single record.
  arbc::Document doc;
  const Scene scene = add_single_layer(doc);
  static_cast<void>(scene);
  arbc::SurfacePool pool_a(backend);
  arbc::TileCache cache_a(64u * 1024 * 1024);
  auto target_a = backend.make_surface(256, 256, arbc::k_working_rgba32f);
  REQUIRE(target_a.has_value());
  arbc::InteractiveRenderer renderer_a({}, epoch_clock());
  arbc::HostViewport bound(renderer_a, doc, arbc::HostViewport::DocumentBinding{}, backend, pool_a,
                           cache_a, **target_a, epoch_clock(), doc_config());

  // One step, no edits: the bootstrap frame composites the bound scene.
  bound.step();
  CHECK(bound.frames_issued() == 1);

  // (b) the oracle: an identical document whose viewport is bootstrapped by a damage
  // commit -- the shape every host had to forge before the bootstrap frame existed.
  arbc::Document oracle_doc;
  const Scene oracle_scene = add_single_layer(oracle_doc);
  arbc::SurfacePool pool_b(backend);
  arbc::TileCache cache_b(64u * 1024 * 1024);
  auto target_b = backend.make_surface(256, 256, arbc::k_working_rgba32f);
  REQUIRE(target_b.has_value());
  arbc::InteractiveRenderer renderer_b({}, epoch_clock());
  arbc::HostViewport oracle(renderer_b, oracle_doc, arbc::HostViewport::DocumentBinding{}, backend,
                            pool_b, cache_b, **target_b, epoch_clock(), doc_config());
  oracle_doc.set_layer_transform(oracle_scene.layer, Affine::identity());
  oracle.step();
  REQUIRE(oracle.frames_issued() == 1);

  // The bootstrap frame matches the full render, and it is not vacuously blank.
  const std::span<const std::byte> a = (*target_a)->cpu_bytes();
  const std::span<const std::byte> b = (*target_b)->cpu_bytes();
  REQUIRE(a.size() == b.size());
  CHECK(std::equal(a.begin(), a.end(), b.begin()));
  CHECK(std::any_of(a.begin(), a.end(), [](std::byte v) { return v != std::byte{0}; }));

  // The bootstrap happens ONCE: subsequent still steps stay at 1.
  for (int i = 0; i < 3; ++i) {
    bound.step();
  }
  CHECK(bound.frames_issued() == 1);
}

// enforces: 01-core-concepts#multiple-viewports-observe-one-composition
TEST_CASE("host_viewport: a camera edit repaints only its own viewport -- a router-sharing "
          "sibling stays idle") {
  MarkBackend backend;
  arbc::Model model;
  const Scene scene = add_single_layer(model);
  SyncSolid content;
  const auto resolve = [&](ObjectId id) -> arbc::Content* {
    return id == scene.content ? &content : nullptr;
  };
  arbc::SurfacePool pool(backend);
  arbc::TileCache cache_a(64u * 1024 * 1024);
  arbc::TileCache cache_b(64u * 1024 * 1024);
  auto target_a = backend.make_surface(256, 256, arbc::k_working_rgba32f);
  auto target_b = backend.make_surface(256, 256, arbc::k_working_rgba32f);
  REQUIRE(target_a.has_value());
  REQUIRE(target_b.has_value());
  // One renderer EACH: the camera-delta detection state is per-viewport frame state
  // (`interactive.hpp` -- sharing a renderer cross-contaminates one viewport's frame
  // into another's).
  arbc::InteractiveRenderer renderer_a({}, epoch_clock());
  arbc::InteractiveRenderer renderer_b({}, epoch_clock());

  arbc::DamageRouter router(model);
  arbc::HostViewport::Config cfg;
  cfg.viewport = Viewport{256, 256, Affine::identity()};
  cfg.budget = k_budget;
  cfg.router = &router;
  arbc::HostViewport vp_a(renderer_a, model, resolve, backend, pool, cache_a, **target_a,
                          epoch_clock(), cfg);
  arbc::HostViewport vp_b(renderer_b, model, resolve, backend, pool, cache_b, **target_b,
                          epoch_clock(), cfg);

  // Both bootstrap once.
  vp_a.step();
  vp_b.step();
  CHECK(vp_a.frames_issued() == 1);
  CHECK(vp_b.frames_issued() == 1);

  // A camera edit on viewport 1 ONLY. Camera damage is per-viewport and device-side
  // (doc 01:108-110): nothing is flushed into the model's sink, so the router delivers
  // nothing and the sibling stays exactly idle.
  const std::uint64_t deliveries = router.deliveries();
  vp_a.set_camera(Affine::translation(-32.0, 0.0));
  vp_a.step();
  vp_b.step();
  CHECK(vp_a.frames_issued() == 2);
  CHECK(vp_b.frames_issued() == 1);         // delta 0: the sibling's camera did not change
  CHECK(router.deliveries() == deliveries); // nothing reached the model's damage slot
}
