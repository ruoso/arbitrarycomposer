// The original repro for `compositor.refine_frame_composite_idempotence`, under a
// real worker pool (doc 02 § The frame, interactively; doc 16 tier 6).
//
// The bug this file guards is silent and it converges on WRONG PIXELS. From the
// stashed `runtime.interactive_worker_count_default` work: 2/60 runs with
// `worker_count == 1` and CPU oversubscription on a nested semi-transparent scene
// produced `composites == 6` where the single-pass oracle does 3 -- and the loop
// quiesced cleanly, with `schedule_follow_up == false`, nothing pending, zero
// degraded composites and nothing cancelled. Nothing in the frame loop's own
// bookkeeping was wrong; only the pixels were.
//
// The mechanism: at `worker_count > 0` a leaf miss fans out to a worker and the
// frame reaches its deadline before the render settles, so the arrival lands on a
// FOLLOW-UP frame -- which is damage-gated, and therefore (before this task) did
// not clear the region it was about to re-composite. Source-over is not idempotent
// for translucent content, so the layer's contribution landed twice. At the shipped
// `worker_count == 0` every miss settles inline and the refine path essentially
// never fires, which is why this stayed latent.
//
// So the assertion is byte-exactness against a single full pass, on every
// iteration, at every worker count that makes refine frames real. It is
// schedule-INDEPENDENT: exactness does not depend on how the arrivals interleaved,
// which is precisely the invariant the clear + clip buy. A failure here is a wrong
// picture, not a flake.
//
// Registered in nightly.yml's tsan-full sweep (the `[.nightly]` case), so the
// arrival/refine path gets repeated schedule stress under TSan and a real pool
// rather than one lucky interleaving; the short-form cases below run per-push,
// including under the gcc-tsan lane.
//
// CROSS-COMPONENT (tests/, linking the umbrella `arbc`): a real `CpuBackend` and
// the concrete kinds beside the runtime loop and the worker pool (doc 17).

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/expected.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/cache/keyed_store.hpp>
#include <arbc/compositor/compositor.hpp>
#include <arbc/compositor/counters.hpp>
#include <arbc/compositor/pull_service.hpp>
#include <arbc/compositor/refinement.hpp>
#include <arbc/compositor/tile_planning.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/kind_fade/fade_content.hpp>
#include <arbc/kind_nested/nested_content.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/media/pixel_format.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/model/damage.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/interactive.hpp>
#include <arbc/runtime/operator_binding.hpp>
#include <arbc/runtime/pull_identity.hpp>
#include <arbc/runtime/worker_pool.hpp>
#include <arbc/surface/surface.hpp>
#include <arbc/surface/surface_pool.hpp>

#include <catch2/catch_test_macros.hpp>

#include "support/schedule_perturb.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

using namespace arbc;

namespace {

constexpr int k_dim = 256; // one rung-0 tile per layer at scale 1.0
constexpr auto k_frame_budget = std::chrono::milliseconds(100);
constexpr Time k_interior{500}; // the fades' envelope is 0.5 here: interior, so they render

Rect canvas() { return Rect{0.0, 0.0, static_cast<double>(k_dim), static_cast<double>(k_dim)}; }
// The frame walk is composition-scoped, so a viewport anchors at the composition it draws
// (compositor.root_composition_frame_walk, doc 05:28-36).
Viewport viewport(ObjectId anchor) { return Viewport{k_dim, k_dim, Affine::identity(), anchor}; }

// A fade whose out-ramp spans [0, 1000): at `k_interior` the envelope is 0.5, an
// INTERIOR weight -- so `identity()` declines, the operator's own `render` runs, and
// its composed output is translucent. An endpoint fade would be served straight from
// its terminal input's tiles and this file would be vacuous.
FadeParams half_fade() {
  return FadeParams{FadeShape::Linear, std::nullopt, FadeWindow{Time{0}, Time{1000}}};
}

// The scene: SEMI-TRANSPARENT content, nested one level -- the shape the repro used.
//
// The child composition holds two translucent solids behind half-fades; the parent shows
// it through a `NestedContent` over a translucent background. EVERY layer is
// semi-transparent, and that is load-bearing: an opaque full-viewport bottom layer would
// mask the bug outright, because an opaque source-over is a REPLACE -- it overwrites
// whatever the previous frame left there, so a doubled contribution vanishes. With a
// translucent stack the previous frame's pixels survive into the blend and a
// contribution that lands twice moves the bytes.
//
// The leaves fan out to workers (leaf-only dispatch); the fades and the nesting render
// inline and re-pull them, which is exactly the arrival -> damage -> refine-frame loop
// under test.
struct NestedTranslucentScene {
  std::shared_ptr<SolidContent> back =
      std::make_shared<SolidContent>(Rgba{0.40F, 0.08F, 0.08F, 0.80F}, canvas());
  std::shared_ptr<SolidContent> veil_a =
      std::make_shared<SolidContent>(Rgba{0.05F, 0.20F, 0.35F, 0.50F}, canvas());
  std::shared_ptr<SolidContent> veil_b =
      std::make_shared<SolidContent>(Rgba{0.30F, 0.10F, 0.05F, 0.60F}, canvas());

  std::shared_ptr<FadeContent> fade_a = std::make_shared<FadeContent>(veil_a.get(), half_fade());
  std::shared_ptr<FadeContent> fade_b = std::make_shared<FadeContent>(veil_b.get(), half_fade());
  std::shared_ptr<NestedContent> nested;

  Document doc;
  ObjectId root{}; // the composition the frame walk anchors at
  // The background's content id, so a test can damage two far-apart sub-regions of it
  // in ONE frame -- the multi-rect repaint set (`compositor.disjoint_dirty_repaint`).
  ObjectId back_id{};

  NestedTranslucentScene() {
    root = doc.add_composition(static_cast<double>(k_dim), static_cast<double>(k_dim));
    back_id = doc.add_content(back);
    doc.attach_layer(root, doc.add_layer(back_id, Affine::identity()));

    const ObjectId child =
        doc.add_composition(static_cast<double>(k_dim), static_cast<double>(k_dim));
    doc.attach_layer(child, doc.add_layer(doc.add_content(fade_a), Affine::identity()));
    doc.attach_layer(child, doc.add_layer(doc.add_content(fade_b), Affine::identity()));
    nested = std::make_shared<NestedContent>(child);

    doc.attach_layer(root, doc.add_layer(doc.add_content(nested), Affine::identity()));
  }
};

std::vector<float> snapshot(const Surface& surface) {
  const std::span<const float> px = std::as_const(surface).span<PixelFormat::Rgba32fLinearPremul>();
  return {px.begin(), px.end()};
}

bool byte_identical(const std::vector<float>& a, const std::vector<float>& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

bool all_transparent(const std::vector<float>& px) {
  return std::all_of(px.begin(), px.end(), [](float v) { return v == 0.0F; });
}

// The ORACLE: one un-gated pass over a cold cache, every miss rendered and settled
// INLINE (no `pending` sink, no `pulls`, no deadline). It is unconditionally the exact
// composite -- what the scene looks like when nothing is refined, nothing is deferred,
// and nothing is composited twice. Every interactive run below must land on these bytes.
std::vector<float> single_pass_oracle(Backend& backend) {
  NestedTranslucentScene scene;
  SurfacePool pool(backend);
  TileCache cache(64U * 1024 * 1024);
  expected<std::unique_ptr<Surface>, SurfaceError> target =
      backend.make_surface(k_dim, k_dim, k_working_rgba32f);
  REQUIRE(target.has_value());

  const DocStatePtr pin = scene.doc.pin();
  const ContentResolver resolve = [&scene](ObjectId id) { return scene.doc.resolve(id); };
  // The nesting descends through a `PullService` the frame driver owns, so bind the
  // operators to this pass's services exactly as the interactive loop does -- including
  // the synthesized pull identity (doc 13:141-154) and the one-revision contribution
  // fold. Without `id_of` the descent keys its inputs under a colliding id and renders
  // nothing: a pass that is self-consistent and still the WRONG oracle.
  // The operator kinds bind through the global binder registry, which both drivers
  // populate before they bind (`interactive.cpp:47`, `offline_sequence.cpp:63`).
  // Unregistered, `bind_operators` binds NOTHING and the nesting renders nothing --
  // silently, and self-consistently, into a wrong oracle. Idempotent.
  register_builtin_operator_binders();
  PullConfig config;
  config.id_of = make_pull_identity_of(*pin, resolve);
  const std::uint64_t revision = pin->revision();
  config.contribution = [revision](const Content*) { return revision; };
  PullServiceImpl pulls(cache, backend, direct_dispatch(), std::move(config));
  const OperatorBindingScope binding = bind_operators(scene.doc, pulls, backend, pin);

  render_frame_interactive(*pin, resolve, viewport(scene.root), cache, backend, pool, **target,
                           Deadline::none(), std::nullopt, /*pending=*/nullptr,
                           /*counters=*/nullptr, /*dirty=*/nullptr, k_interior,
                           /*visible_plans=*/nullptr, /*diagnostics=*/nullptr, &pulls);
  return snapshot(**target);
}

// A latch a leaf render parks on, so a test can hold ONE leaf in flight across a frame
// boundary on purpose. A gate, not a sleep: wall-clock tests lie in CI, latches do not.
class Gate {
public:
  void arrive_and_wait() {
    std::unique_lock<std::mutex> lock(d_mutex);
    ++d_arrived;
    d_arrival_cv.notify_all();
    d_open_cv.wait(lock, [this] { return d_open; });
  }
  void open() {
    {
      const std::lock_guard<std::mutex> lock(d_mutex);
      d_open = true;
    }
    d_open_cv.notify_all();
  }
  // Block until at least `n` renders are parked inside the gate: what makes "still in
  // flight when the frame returned" an observation rather than an assumption.
  void await_arrivals(std::size_t n) {
    std::unique_lock<std::mutex> lock(d_mutex);
    d_arrival_cv.wait(lock, [this, n] { return d_arrived >= n; });
  }

private:
  std::mutex d_mutex;
  std::condition_variable d_open_cv;
  std::condition_variable d_arrival_cv;
  std::size_t d_arrived{0};
  bool d_open{false};
};

// A translucent leaf whose FIRST render parks on the gate: it fans out to a worker
// (leaf-only dispatch) and stays in flight until the test opens the gate, so it is
// guaranteed to arrive on a LATER frame -- which is what makes the refine path fire on
// demand instead of by luck. Later renders (the re-plan after the arrival) pass straight
// through, or the loop could never quiesce.
class GatedVeil : public Content {
public:
  GatedVeil(Gate& gate, Rgba color) : d_gate(gate), d_solid(color, canvas()) {}

  std::optional<Rect> bounds() const override { return d_solid.bounds(); }
  Stability stability() const override { return d_solid.stability(); }
  std::optional<TimeRange> time_extent() const override { return d_solid.time_extent(); }
  bool render_thread_safe() const override { return true; }

  std::optional<RenderResult> render(const RenderRequest& request,
                                     std::shared_ptr<RenderCompletion> done) override {
    if (!d_gated.exchange(true, std::memory_order_acq_rel)) {
      d_gate.arrive_and_wait();
    }
    return d_solid.render(request, std::move(done));
  }

private:
  Gate& d_gate;
  SolidContent d_solid;
  std::atomic<bool> d_gated{false};
};

// The gated variant of the scene: same layers, same colors, but the top child veil is a
// `GatedVeil`, so the test controls exactly when it arrives.
struct GatedScene {
  Gate gate;
  std::shared_ptr<SolidContent> back =
      std::make_shared<SolidContent>(Rgba{0.40F, 0.08F, 0.08F, 0.80F}, canvas());
  std::shared_ptr<SolidContent> veil_a =
      std::make_shared<SolidContent>(Rgba{0.05F, 0.20F, 0.35F, 0.50F}, canvas());
  std::shared_ptr<GatedVeil> veil_b =
      std::make_shared<GatedVeil>(gate, Rgba{0.30F, 0.10F, 0.05F, 0.60F});

  std::shared_ptr<FadeContent> fade_a = std::make_shared<FadeContent>(veil_a.get(), half_fade());
  std::shared_ptr<FadeContent> fade_b = std::make_shared<FadeContent>(veil_b.get(), half_fade());
  std::shared_ptr<NestedContent> nested;

  Document doc;
  ObjectId root{}; // the composition the frame walk anchors at

  GatedScene() {
    root = doc.add_composition(static_cast<double>(k_dim), static_cast<double>(k_dim));
    doc.attach_layer(root, doc.add_layer(doc.add_content(back), Affine::identity()));
    const ObjectId child =
        doc.add_composition(static_cast<double>(k_dim), static_cast<double>(k_dim));
    doc.attach_layer(child, doc.add_layer(doc.add_content(fade_a), Affine::identity()));
    doc.attach_layer(child, doc.add_layer(doc.add_content(fade_b), Affine::identity()));
    nested = std::make_shared<NestedContent>(child);
    doc.attach_layer(root, doc.add_layer(doc.add_content(nested), Affine::identity()));
  }
};

// The `InteractiveRenderer` driven to quiescence at `k_interior`: frames until nothing
// is in flight AND no follow-up is owed. Both conditions are load-bearing -- a frame
// that reaps an arrival carries its damage to the NEXT frame rather than compositing it,
// so stopping at `pending.empty()` stops one frame before the pixels exist. This is the
// loop a host runs on `FrameOutcome::schedule_follow_up`, and every frame after the
// first is damage-gated: the refine path this task fixes.
std::vector<float> quiesced_pixels(Backend& backend, std::size_t worker_count,
                                   arbc::test::Perturber* perturb, CompositorCounters* out) {
  NestedTranslucentScene scene;
  WorkerPoolConfig pool_config;
  pool_config.worker_count = worker_count;
  InteractiveRenderer renderer(std::move(pool_config), InteractiveRenderer::Clock{});

  SurfacePool pool(backend);
  TileCache cache(64U * 1024 * 1024);
  expected<std::unique_ptr<Surface>, SurfaceError> target =
      backend.make_surface(k_dim, k_dim, k_working_rgba32f);
  REQUIRE(target.has_value());

  constexpr int k_max_frames = 64; // a convergence bound, never a timing assumption
  for (int i = 0; i < k_max_frames; ++i) {
    if (perturb != nullptr) {
      perturb->maybe_yield(); // widen the arrival window; never paces the test
    }
    const DocStatePtr pin = scene.doc.pin();
    const ContentResolver resolve = [&scene](ObjectId id) { return scene.doc.resolve(id); };
    const FrameBinding binding{&scene.doc, pin};
    const InteractiveRenderer::FrameOutcome outcome =
        renderer.render_frame(*pin, resolve, viewport(scene.root), cache, backend, pool, **target,
                              {}, k_interior, k_frame_budget, binding);
    if (!outcome.schedule_follow_up && renderer.pending().tiles.empty()) {
      if (out != nullptr) {
        *out = renderer.counters();
      }
      return snapshot(**target);
    }
  }
  FAIL("the interactive frame loop did not reach quiescence");
  return {};
}

// The loop driven to quiescence, then RE-DAMAGED at two far-apart sub-regions of the
// background in one frame, then driven to quiescence again.
//
// That second sequence is the path this task opens, and it is why re-running the
// single-damage cases above would not have covered it. Two far-apart damages normalize
// to a genuinely DISJOINT two-rect repaint set, so within one frame every layer is
// planned TWICE -- once per rect -- and every layer's tile straddles both rects. That is
// a new way for two plans of the same `TileKey` to meet inside a single frame, which is
// precisely what the pending-set guard and the per-rect plan merge exist for, and
// precisely what a single-damage frame cannot exercise. Under a real worker pool the
// re-rendered tiles arrive across frame boundaries while the disjoint rects are being
// re-composited, so the interleaving is real.
//
// The assertion stays the one that matters: byte-exactness against the single-pass
// oracle at quiescence, schedule-independently. Two clipped composites of a straddling
// tile are correct exactly because the rects do not overlap; a bug there is a doubled
// translucent blend inside one of the rects, and it moves the bytes.
std::vector<float> quiesced_after_split_damage(Backend& backend, std::size_t worker_count) {
  NestedTranslucentScene scene;
  WorkerPoolConfig pool_config;
  pool_config.worker_count = worker_count;
  InteractiveRenderer renderer(std::move(pool_config), InteractiveRenderer::Clock{});

  SurfacePool pool(backend);
  TileCache cache(64U * 1024 * 1024);
  expected<std::unique_ptr<Surface>, SurfaceError> target =
      backend.make_surface(k_dim, k_dim, k_working_rgba32f);
  REQUIRE(target.has_value());

  const auto frame = [&](std::span<const Damage> damage) {
    const DocStatePtr pin = scene.doc.pin();
    const ContentResolver resolve = [&scene](ObjectId id) { return scene.doc.resolve(id); };
    const FrameBinding binding{&scene.doc, pin};
    return renderer.render_frame(*pin, resolve, viewport(scene.root), cache, backend, pool,
                                 **target, damage, k_interior, k_frame_budget, binding);
  };

  constexpr int k_max_frames = 64; // a convergence bound, never a timing assumption
  const auto settle = [&](std::span<const Damage> first) {
    std::span<const Damage> damage = first;
    for (int i = 0; i < k_max_frames; ++i) {
      const InteractiveRenderer::FrameOutcome outcome = frame(damage);
      damage = {}; // the injected damage rides exactly one frame; the rest are refines
      if (!outcome.schedule_follow_up && renderer.pending().tiles.empty()) {
        return true;
      }
    }
    return false;
  };

  REQUIRE(settle({})); // the ordinary loop, to a clean fixed point

  // Opposite corners of the canvas, far enough apart that their bounding box is the
  // whole viewport but their disjoint cover is two small rects.
  const std::vector<Damage> split{
      Damage{scene.back_id, Rect{0.0, 0.0, 32.0, 32.0}, TimeRange::all()},
      Damage{scene.back_id,
             Rect{static_cast<double>(k_dim) - 32.0, static_cast<double>(k_dim) - 32.0,
                  static_cast<double>(k_dim), static_cast<double>(k_dim)},
             TimeRange::all()}};
  REQUIRE(settle(split));

  return snapshot(**target);
}

} // namespace

// enforces: 02-architecture#gated-frame-equals-single-pass
TEST_CASE("a refined interactive sequence on a translucent nested scene is byte-exact under a "
          "worker pool") {
  CpuBackend backend;
  const std::vector<float> oracle = single_pass_oracle(backend);
  REQUIRE_FALSE(all_transparent(oracle));

  // 60 iterations x worker_count in {1, 2, 4} -- the shape of the original repro,
  // which failed 2/60 at worker_count 1. Each iteration is a fresh scene (cold cache)
  // and a fresh pool, so each really does drive misses through workers and refine them
  // on follow-up frames.
  constexpr int k_iterations = 60;
  for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}}) {
    for (int i = 0; i < k_iterations; ++i) {
      CAPTURE(workers, i);
      REQUIRE(byte_identical(quiesced_pixels(backend, workers, nullptr, nullptr), oracle));
    }
  }
}

// enforces: 02-architecture#gated-frame-equals-single-pass
TEST_CASE("a leaf that arrives on a later frame refines onto byte-exact pixels") {
  CpuBackend backend;
  const std::vector<float> oracle = single_pass_oracle(backend);
  REQUIRE_FALSE(all_transparent(oracle));

  // The repro, made DETERMINISTIC. The case above drives the loop and hopes an arrival
  // lands late; here one leaf provably does. Frame 1 dispatches every leaf to a worker,
  // the gated one parks, and the frame returns at its deadline with that render still in
  // flight -- so frame 1 paints the layers that DID settle and shows the gated one's
  // placeholder. Opening the gate lets it arrive, which damages its region and schedules
  // a follow-up: a damage-gated frame that re-composites, over the pixels frame 1 already
  // painted, every layer intersecting that region.
  //
  // That is the bug in one sentence. Un-cleared, the layers frame 1 already painted land
  // a SECOND translucent contribution and the loop quiesces on wrong pixels -- silently,
  // with nothing pending and nothing degraded. Cleared and clipped, the region is
  // repainted from scratch and equals the single full pass.
  GatedScene scene;
  WorkerPoolConfig pool_config;
  pool_config.worker_count = 2;
  InteractiveRenderer renderer(std::move(pool_config), InteractiveRenderer::Clock{});

  SurfacePool pool(backend);
  TileCache cache(64U * 1024 * 1024);
  expected<std::unique_ptr<Surface>, SurfaceError> target =
      backend.make_surface(k_dim, k_dim, k_working_rgba32f);
  REQUIRE(target.has_value());

  const auto frame = [&] {
    const DocStatePtr pin = scene.doc.pin();
    const ContentResolver resolve = [&scene](ObjectId id) { return scene.doc.resolve(id); };
    const FrameBinding binding{&scene.doc, pin};
    return renderer.render_frame(*pin, resolve, viewport(scene.root), cache, backend, pool,
                                 **target, {}, k_interior, k_frame_budget, binding);
  };

  frame();                      // dispatches; the gated leaf parks on a worker
  scene.gate.await_arrivals(1); // it really is in flight -- observed, not assumed
  scene.gate.open();            // ... and now it may arrive

  constexpr int k_max_frames = 64; // a convergence bound, never a timing assumption
  bool quiesced = false;
  for (int i = 0; i < k_max_frames && !quiesced; ++i) {
    const InteractiveRenderer::FrameOutcome outcome = frame();
    quiesced = !outcome.schedule_follow_up && renderer.pending().tiles.empty();
  }
  REQUIRE(quiesced);

  // The loop settled cleanly -- and, decisively, on the right pixels.
  CHECK(byte_identical(snapshot(**target), oracle));
}

// enforces: 02-architecture#gated-frame-repaint-is-idempotent
TEST_CASE("a quiesced refine loop composites each tile once per frame, never twice") {
  CpuBackend backend;

  // The behavioral half of the same statement (doc 16:54-62, counters, never wall
  // clock). The un-gated oracle run at `worker_count == 0` settles every miss inline,
  // so it composites each layer's tile exactly once: that is the single-pass composite
  // count. A worker-backed run refines over several frames and so composites MORE times
  // in total -- one per gated repaint, which is correct and is what progressive
  // refinement IS. What must never happen is a repaint landing on un-cleared pixels,
  // and that is what the byte-exactness above pins.
  //
  // What this case adds: the loop still QUIESCES (it does not spin re-damaging itself),
  // it degrades nothing at the shipped inline default, and it renders each tile once --
  // so the fix did not buy idempotence by re-rendering, and did not leave the loop
  // livelocked on its own repaints.
  CompositorCounters inline_counters;
  const std::vector<float> inline_pixels = quiesced_pixels(backend, 0, nullptr, &inline_counters);
  REQUIRE_FALSE(all_transparent(inline_pixels));
  CHECK(inline_counters.degraded_composites() == 0); // every miss settled inline: no fallbacks
  CHECK(inline_counters.composites() > 0);

  CompositorCounters worker_counters;
  const std::vector<float> worker_pixels = quiesced_pixels(backend, 2, nullptr, &worker_counters);
  CHECK(byte_identical(worker_pixels, inline_pixels));
  CHECK(worker_counters.composites() > 0);

  // The two-pass identity, under a LIVE worker pool and real arrival races. On a cold
  // cache with workers, each operator renders exactly TWICE -- once to request its inputs
  // and paint the transient placeholder (which is how the driver discovers the input tiles
  // at all), once when the wave lands and it can compose the real pixels. The inline oracle
  // renders each exactly once, because `submit` IS the render there and every leaf settles
  // into the cache before the pull returns, so its first render is already exact.
  REQUIRE(inline_counters.operator_renders() > 0);
  CHECK(worker_counters.operator_renders() == 2 * inline_counters.operator_renders());

  // No renders are COALESCED here, and after `compositor.root_composition_frame_walk` (doc
  // 05:28-36) that is the correct observation. The in-flight wave gate deterministically
  // fired on this scene ONLY while the old document-global walk double-drew the nested
  // child: it rendered the child's fades flat at top level (dispatching their leaves) and
  // then `nested` re-pulled those resident transient fade tiles the SAME frame, with the
  // leaves still in the queue -- an intra-frame coalesce the double-draw made deterministic.
  // Scoped, `nested` is the sole renderer of its child's operator tiles: each is rendered
  // once and its leaf dispatched once, so the wave-land re-render hits a warm leaf and
  // there is no pre-rendered transient to coalesce and no redundant re-pull to hold. The
  // gate's deterministic witness lives at the compositor level (`counters.t.cpp`,
  // `refinement.t.cpp`), where the partial arrival can be staggered by hand.
  CHECK(worker_counters.renders_coalesced() == 0);
  CHECK(inline_counters.renders_coalesced() == 0);
}

// enforces: 02-architecture#gated-frame-equals-single-pass
TEST_CASE("a refine sequence whose damage is two far-apart rects is byte-exact under a worker "
          "pool") {
  CpuBackend backend;
  const std::vector<float> oracle = single_pass_oracle(backend);
  REQUIRE_FALSE(all_transparent(oracle));

  // The same shape as the single-damage sweep above, over the multi-rect repaint path:
  // a fresh scene and a fresh pool per iteration, so each really does drive misses
  // through workers and refine them on follow-up frames -- but with a frame whose
  // repaint region is a two-rect disjoint set that every layer's tile straddles.
  constexpr int k_iterations = 30;
  for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}}) {
    for (int i = 0; i < k_iterations; ++i) {
      CAPTURE(workers, i);
      REQUIRE(byte_identical(quiesced_after_split_damage(backend, workers), oracle));
    }
  }
}

namespace {

// The same loop under DEADLINE PRESSURE (`runtime.deadline_cancel_retains_wanted`): the
// first few frames carry a zero budget, so their deadline is gone the instant it is
// sampled, the park never waits, and every render dispatched to a worker is still in flight
// when the frame gives up. The sweep then RETAINS them -- they are visible, at this
// revision, at this camera -- and the next frame reads those live `PendingTile`s through
// `tile_in_flight` while a worker is still writing their surfaces and may settle any one of
// them at any moment.
//
// The reads are the same two atomics the park already polls (`settled()` / `cancelled()`),
// so the thread-safety argument is unchanged. But this is the first code path that keeps a
// live pending entry ADDRESSABLE ACROSS A FRAME BOUNDARY by design, so it gets explicit
// TSan exposure rather than an argument. The frames after the tight ones carry the normal
// budget so the loop converges rather than spinning against the workers it is waiting on --
// a convergence device, never a timing assumption, and no assertion reads a clock.
std::vector<float> quiesced_pixels_under_deadline_pressure(Backend& backend,
                                                           std::size_t worker_count,
                                                           arbc::test::Perturber* perturb) {
  constexpr int k_tight_frames = 3; // enough for pendings to cross a boundary, then converge
  NestedTranslucentScene scene;
  WorkerPoolConfig pool_config;
  pool_config.worker_count = worker_count;
  InteractiveRenderer renderer(std::move(pool_config), InteractiveRenderer::Clock{});

  SurfacePool pool(backend);
  TileCache cache(64U * 1024 * 1024);
  expected<std::unique_ptr<Surface>, SurfaceError> target =
      backend.make_surface(k_dim, k_dim, k_working_rgba32f);
  REQUIRE(target.has_value());

  constexpr int k_max_frames = 64; // a convergence bound, never a timing assumption
  for (int i = 0; i < k_max_frames; ++i) {
    if (perturb != nullptr) {
      perturb->maybe_yield(); // widen the arrival window; never paces the test
    }
    const DocStatePtr pin = scene.doc.pin();
    const ContentResolver resolve = [&scene](ObjectId id) { return scene.doc.resolve(id); };
    const FrameBinding binding{&scene.doc, pin};
    const std::chrono::steady_clock::duration budget =
        (i < k_tight_frames) ? std::chrono::steady_clock::duration::zero() : k_frame_budget;
    const InteractiveRenderer::FrameOutcome outcome =
        renderer.render_frame(*pin, resolve, viewport(scene.root), cache, backend, pool, **target,
                              {}, k_interior, budget, binding);
    if (!outcome.schedule_follow_up && renderer.pending().tiles.empty()) {
      return snapshot(**target);
    }
  }
  FAIL("the interactive frame loop did not reach quiescence");
  return {};
}

} // namespace

// enforces: 02-architecture#deadline-sweep-retains-wanted-tiles
// enforces: 02-architecture#gated-frame-equals-single-pass
TEST_CASE("a refine sequence under deadline pressure retains its pendings and stays byte-exact",
          "[.nightly]") {
  CpuBackend backend;
  const std::vector<float> oracle = single_pass_oracle(backend);
  REQUIRE_FALSE(all_transparent(oracle));

  // Retention changes WHEN a render lands, never what it paints: a retained tile is one
  // nobody told to abandon its work, and the loop still converges on the single-pass pixels.
  // Under TSan this is also the coverage for the cross-boundary read of a live pending entry
  // -- the first such path in the tree.
  for (std::uint32_t seed = 0; seed < 60; ++seed) {
    INFO("seed = " << seed);
    arbc::test::Perturber perturb(seed);
    for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}}) {
      CAPTURE(workers);
      REQUIRE(byte_identical(quiesced_pixels_under_deadline_pressure(backend, workers, &perturb),
                             oracle));
    }
  }
}

// enforces: 02-architecture#gated-frame-equals-single-pass
TEST_CASE("a refined interactive sequence stays byte-exact across many schedules", "[.nightly]") {
  CpuBackend backend;
  const std::vector<float> oracle = single_pass_oracle(backend);
  REQUIRE_FALSE(all_transparent(oracle));

  // The per-push lane runs the loop on whatever interleaving the machine happens to
  // produce. Here it runs under many seeded schedules: the perturber yields on a random
  // bit between frames, widening the window in which a worker's arrival lands mid-frame
  // -- exactly the window where a gated frame re-composites a region a previous frame
  // already painted. Reproducible: a red run replays from the logged seed.
  for (std::uint32_t seed = 0; seed < 64; ++seed) {
    INFO("seed = " << seed);
    arbc::test::Perturber perturb(seed);
    for (const std::size_t workers : {std::size_t{1}, std::size_t{4}}) {
      CAPTURE(workers);
      REQUIRE(byte_identical(quiesced_pixels(backend, workers, &perturb, nullptr), oracle));
    }
  }
}
