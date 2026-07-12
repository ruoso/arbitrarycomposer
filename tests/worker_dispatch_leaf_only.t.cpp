// Worker dispatch is LEAF-ONLY (`runtime.worker_dispatch_leaf_only`, doc 02
// § Threading model, doc 00 § decision record).
//
// The render worker pool executes leaf renders only. An operator -- a fade, a
// crossfade, a nested composition (doc 13) -- renders INLINE on the driver thread,
// because its `render` re-enters the `PullService` to fetch its inputs, and a pull
// probes and inserts the `TileCache` and walks the service's own descent depth, both
// of which are render-thread-confined. Handing an operator to a worker compiles, does
// not assert, and usually produces the right pixels on a developer's machine; what it
// actually produces is a data race on the cache. TSan confirmed that race was latent
// for all three operator kinds (`kinds.nested_runtime_binding`).
//
// The rule now lives in exactly one place -- `worker_backed_dispatch(pool)` -- and both
// drivers obtain their dispatch from it. This file pins that, at three altitudes:
//
//   * the helper itself, called directly, with recorded thread ids on both branches;
//   * both DRIVERS (`SequenceRenderer`'s parallel-exact path and
//     `InteractiveRenderer::render_frame`) over a scene holding all three operator
//     kinds, at `worker_count > 0`;
//   * the lifetime consequences the interactive swap introduces -- a leaf render still
//     in flight across the frame's deadline-cancel and across renderer teardown.
//
// How an operator render is OBSERVED to stay off the workers: the pool is the only
// mechanism in the tree that moves a render off the calling thread, and a content
// reaches a worker if and only if it is admitted through `WorkerPool::submit`. The
// pool consults `WorkerPoolConfig::serialize_predicate` with the exact `Content*` on
// every admission (`worker_pool.cpp:72,90`) and again on every worker-side dequeue
// (`:168`) -- an injectable seam the config documents as a test hook. Spying it
// therefore observes precisely the set of contents that reached the pool, at both
// ends, and a submitted operator cannot hide from it. `is_operator` (the helper's own
// predicate) classifies each one.
//
// CROSS-COMPONENT (tests/, linking the umbrella `arbc`): a real `CpuBackend` and the
// concrete operator kinds beside the runtime drivers and the pool (doc 17).

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/expected.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/cache/keyed_store.hpp>
#include <arbc/compositor/compositor.hpp>
#include <arbc/compositor/operator_graph.hpp>
#include <arbc/compositor/pull_service.hpp>
#include <arbc/compositor/refinement.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/kind_crossfade/crossfade_content.hpp>
#include <arbc/kind_fade/fade_content.hpp>
#include <arbc/kind_nested/nested_content.hpp>
#include <arbc/kind_solid/solid_content.hpp> // Rgba
#include <arbc/media/pixel_format.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/interactive.hpp>
#include <arbc/runtime/offline_sequence.hpp>
#include <arbc/runtime/worker_dispatch.hpp>
#include <arbc/runtime/worker_pool.hpp>
#include <arbc/surface/surface.hpp>
#include <arbc/surface/surface_pool.hpp>

#include <catch2/catch_test_macros.hpp>

#include "support/schedule_perturb.hpp"

#include <algorithm>
#include <array>
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
#include <thread>
#include <utility>
#include <vector>

using namespace arbc;

namespace {

// One rung-0 tile at scale 1.0, so a frame is a single planned tile per layer.
constexpr int k_dim = 256;

Rect canvas() { return Rect{0.0, 0.0, static_cast<double>(k_dim), static_cast<double>(k_dim)}; }
Viewport viewport() { return Viewport{k_dim, k_dim, Affine::identity()}; }

// A fade whose out-ramp spans [0, 1000): the envelope is 0.5 at t == 500 -- an INTERIOR
// weight, so `identity()` declines and the operator's own `render` runs. Same for the
// crossfade's w. An endpoint operator would be served by the driver from its terminal
// input's tiles and would never render at all, which would make this file vacuous.
FadeParams half_fade() {
  return FadeParams{FadeShape::Linear, std::nullopt, FadeWindow{Time{0}, Time{1000}}};
}
CrossfadeParams half_crossfade() {
  return CrossfadeParams{CrossfadeShape::Linear, Time{0}, Time{1000}};
}
constexpr Time k_interior{500}; // the instant every frame below renders at

// A clock the test moves from "already expired" to real, because A3 needs BOTH.
//
// Expired (the steady_clock epoch, so every deadline lands in the real past):
// `wait_completions` returns immediately without parking, and the frame takes its
// deadline-cancel path (`interactive.cpp:322-334`) with renders still in flight. That is
// the schedule under test.
//
// Real: the frames that then REAP those renders must find an un-expired deadline, so the
// frame parks on the pool's completion condvar and wakes when a worker settles -- what a
// host actually does. Leaving the clock expired would make every reaping frame a no-wait
// poll that re-dispatches and immediately gives up, a busy spin that starves the very
// workers it is waiting on. Neither state is a timing assumption: expired means "in the
// past" (any uptime), real means "the frame waits for a condition, not a duration".
class ExpiringClock {
public:
  InteractiveRenderer::Clock functor() const {
    return [expired = d_expired] {
      return expired->load(std::memory_order_acquire) ? std::chrono::steady_clock::time_point{}
                                                      : std::chrono::steady_clock::now();
    };
  }
  void unexpire() const { d_expired->store(false, std::memory_order_release); }

private:
  std::shared_ptr<std::atomic<bool>> d_expired = std::make_shared<std::atomic<bool>>(true);
};
// The frame budget for the reaping loops, over the default (real) `steady_clock` a host
// uses. It is a PARK BOUND, never an assertion, and NOTHING here is timed: a frame with
// work in flight returns from `wait_completions` the instant a completion settles (a leaf
// paint is microseconds), and a frame with none returns when the bound elapses. No
// assertion depends on the value -- if a render DID miss this deadline, cancellation is
// advisory (`content.hpp:161-165`), the worker still completes it, and `poll_refinements`
// still reaps it on a later frame; the loop simply takes one more turn. So this cannot
// make a test lie, only make it slower (doc 16:54-62).
//
// It must be SMALL, and that is load-bearing: a frame carrying arrival damage but
// dispatching nothing (every tile warm) parks for the WHOLE bound with nothing to wait
// for, so a large budget here is a stall, not a safety margin.
constexpr auto k_frame_budget = std::chrono::milliseconds(100);

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

void paint(const RenderRequest& request, const Rgba& color) {
  const std::span<float> px = request.target.span<PixelFormat::Rgba32fLinearPremul>();
  for (std::size_t i = 0; i + 3 < px.size(); i += 4) {
    px[i + 0] = color.r;
    px[i + 1] = color.g;
    px[i + 2] = color.b;
    px[i + 3] = color.a;
  }
}

// --- Thread-recording contents -------------------------------------------------

// The set of threads a content was rendered on. Guarded, because the whole question is
// which threads touch it.
class ThreadLog {
public:
  void record() {
    const std::lock_guard<std::mutex> lock(d_mutex);
    d_threads.push_back(std::this_thread::get_id());
  }
  std::vector<std::thread::id> threads() const {
    const std::lock_guard<std::mutex> lock(d_mutex);
    return d_threads;
  }
  std::size_t renders() const {
    const std::lock_guard<std::mutex> lock(d_mutex);
    return d_threads.size();
  }
  // Renders observed on a thread OTHER than `driver` -- i.e. on a worker.
  std::size_t off_thread(std::thread::id driver) const {
    const std::lock_guard<std::mutex> lock(d_mutex);
    return static_cast<std::size_t>(std::count_if(
        d_threads.begin(), d_threads.end(), [driver](std::thread::id t) { return t != driver; }));
  }

private:
  mutable std::mutex d_mutex;
  std::vector<std::thread::id> d_threads;
};

// A LEAF (no inputs): it paints its own thread-confined target and pulls nothing, which
// is exactly why it may fan out. Deterministic -- the pixels are a pure function of the
// request -- so a frame is byte-identical whichever thread paints it.
class RecordingLeaf : public Content {
public:
  explicit RecordingLeaf(Rgba color) : d_color(color) {}

  std::optional<Rect> bounds() const override { return canvas(); }
  Stability stability() const override { return Stability::Static; }
  std::optional<TimeRange> time_extent() const override { return std::nullopt; }
  bool render_thread_safe() const override { return true; }

  std::optional<RenderResult> render(const RenderRequest& request,
                                     std::shared_ptr<RenderCompletion> /*done*/) override {
    d_log.record();
    paint(request, d_color);
    return RenderResult{request.scale, /*exact=*/true};
  }

  const ThreadLog& log() const noexcept { return d_log; }

private:
  Rgba d_color;
  ThreadLog d_log;
};

// An OPERATOR by the tree's ONE definition of the word: non-empty `inputs()`
// (`operator_graph.hpp:80-85`), which is precisely the predicate
// `worker_backed_dispatch` branches on. It records the thread its `render` runs on, so
// the helper's inline branch can be observed by thread identity directly -- something
// the three shipped kinds cannot do, being `final`.
class RecordingOperator : public Content {
public:
  explicit RecordingOperator(ContentRef input) : d_inputs{input} {}

  std::optional<Rect> bounds() const override { return canvas(); }
  Stability stability() const override { return Stability::Static; }
  std::optional<TimeRange> time_extent() const override { return std::nullopt; }
  bool render_thread_safe() const override { return true; }
  std::span<const ContentRef> inputs() const override { return d_inputs; }

  std::optional<RenderResult> render(const RenderRequest& request,
                                     std::shared_ptr<RenderCompletion> /*done*/) override {
    d_log.record();
    paint(request, Rgba{0.125F, 0.25F, 0.375F, 1.0F});
    return RenderResult{request.scale, /*exact=*/true};
  }

  const ThreadLog& log() const noexcept { return d_log; }

private:
  std::array<ContentRef, 1> d_inputs;
  ThreadLog d_log;
};

// A leaf that blocks inside `render` on a manually-opened gate: the render is genuinely
// IN FLIGHT on a worker, holding a `Surface&` into a `PendingTile`, for as long as the
// test wants. A gate, not a sleep -- wall-clock tests lie in CI, latches do not.
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
  // Block until at least `n` renders are parked inside the gate. This is what makes
  // "still in flight" an observation rather than an assumption.
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

class GatedLeaf : public Content {
public:
  GatedLeaf(Gate& gate, Rgba color) : d_gate(gate), d_color(color) {}

  std::optional<Rect> bounds() const override { return canvas(); }
  Stability stability() const override { return Stability::Static; }
  std::optional<TimeRange> time_extent() const override { return std::nullopt; }
  bool render_thread_safe() const override { return true; }

  std::optional<RenderResult> render(const RenderRequest& request,
                                     std::shared_ptr<RenderCompletion> /*done*/) override {
    d_gate.arrive_and_wait();
    // The write lands AFTER the frame that issued this request has returned, and (in the
    // teardown case) while `~InteractiveRenderer` is joining the pool. `request.target`
    // is a `Surface&` into a `PendingTile` owned by the renderer's `d_pending` member --
    // which outlives `d_pool` by declaration order (`interactive.hpp:262-267`). If that
    // order ever reverses, this store is a use-after-free and ASan/TSan say so.
    paint(request, d_color);
    return RenderResult{request.scale, /*exact=*/true};
  }

private:
  Gate& d_gate;
  Rgba d_color;
};

// --- The pool spy ---------------------------------------------------------------

// Every content the pool ever admitted, and whether any of them was an operator.
// `serialize_predicate` is the config's documented test hook; this wrapper spies it
// while preserving its DEFAULT semantics exactly (`worker_pool.hpp:70-72`), so the
// serialization gate under test is the shipped one.
class DispatchSpy {
public:
  bool note(const Content* content) {
    {
      const std::lock_guard<std::mutex> lock(d_mutex);
      ++d_admissions;
      if (is_operator(content)) {
        d_operators.push_back(content);
      }
    }
    return content != nullptr && !content->render_thread_safe();
  }

  std::size_t admissions() const {
    const std::lock_guard<std::mutex> lock(d_mutex);
    return d_admissions;
  }
  std::size_t operator_admissions() const {
    const std::lock_guard<std::mutex> lock(d_mutex);
    return d_operators.size();
  }

private:
  mutable std::mutex d_mutex;
  std::size_t d_admissions{0};
  std::vector<const Content*> d_operators;
};

WorkerPoolConfig spied_pool(std::size_t workers, DispatchSpy& spy) {
  WorkerPoolConfig config;
  config.worker_count = workers;
  config.serialize_predicate = [&spy](const Content* c) { return spy.note(c); };
  return config;
}

// --- The scene ------------------------------------------------------------------

// All three operator kinds over recording leaves, at an instant where NONE of them is an
// identity endpoint: the fade's envelope is 0.5, the crossfade's w is 0.5, and the
// nesting composites two interior fades of its own. So every frame below really does
// call fade's, crossfade's and nested's own `render` -- the three renders TSan caught
// racing the tile cache when they were dispatched to workers.
//
// The operators borrow their leaves non-owningly (`ContentRef` is a raw `Content*`), so
// the leaves are held here and outlive the `Document`.
struct OperatorScene {
  std::shared_ptr<RecordingLeaf> under =
      std::make_shared<RecordingLeaf>(Rgba{0.25F, 0.50F, 0.75F, 1.0F});
  std::shared_ptr<RecordingLeaf> from =
      std::make_shared<RecordingLeaf>(Rgba{0.50F, 0.25F, 0.125F, 1.0F});
  std::shared_ptr<RecordingLeaf> to =
      std::make_shared<RecordingLeaf>(Rgba{0.125F, 0.375F, 0.75F, 1.0F});
  std::shared_ptr<RecordingLeaf> deep_a =
      std::make_shared<RecordingLeaf>(Rgba{0.60F, 0.20F, 0.10F, 0.80F});
  std::shared_ptr<RecordingLeaf> deep_b =
      std::make_shared<RecordingLeaf>(Rgba{0.10F, 0.40F, 0.30F, 0.50F});

  std::shared_ptr<FadeContent> fade = std::make_shared<FadeContent>(under.get(), half_fade());
  std::shared_ptr<CrossfadeContent> crossfade =
      std::make_shared<CrossfadeContent>(from.get(), to.get(), half_crossfade());
  std::shared_ptr<FadeContent> deep_fade_a =
      std::make_shared<FadeContent>(deep_a.get(), half_fade());
  std::shared_ptr<FadeContent> deep_fade_b =
      std::make_shared<FadeContent>(deep_b.get(), half_fade());
  std::shared_ptr<NestedContent> nested;

  Document doc;
  ObjectId bootstrap_layer{};

  OperatorScene() {
    const ObjectId child =
        doc.add_composition(static_cast<double>(k_dim), static_cast<double>(k_dim));
    doc.attach_layer(child, doc.add_layer(doc.add_content(deep_fade_a), Affine::identity()));
    doc.attach_layer(child, doc.add_layer(doc.add_content(deep_fade_b), Affine::identity()));
    nested = std::make_shared<NestedContent>(child);

    bootstrap_layer = doc.add_layer(doc.add_content(fade), Affine::identity());
    doc.add_layer(doc.add_content(crossfade), Affine::identity());
    doc.add_layer(doc.add_content(nested), Affine::identity());
  }

  // Every leaf render this scene performed, across all five leaves.
  std::size_t leaf_renders() const {
    return under->log().renders() + from->log().renders() + to->log().renders() +
           deep_a->log().renders() + deep_b->log().renders();
  }
  // Leaf renders observed on a thread other than the driver's -- i.e. on a worker.
  std::size_t leaf_renders_off(std::thread::id driver) const {
    return under->log().off_thread(driver) + from->log().off_thread(driver) +
           to->log().off_thread(driver) + deep_a->log().off_thread(driver) +
           deep_b->log().off_thread(driver);
  }
};

// --- The drivers ----------------------------------------------------------------

// The export driver's frame of `doc` at `k_interior`, rendered with `workers` workers.
// `SequenceRenderer` reaps to quiescence within the frame, so this is unconditionally
// the exact composite -- the oracle every other frame here is compared against.
std::vector<float> offline_frame(Document& doc, Backend& backend, std::size_t workers,
                                 DispatchSpy& spy) {
  SequenceRenderer renderer(doc, viewport(), backend, spied_pool(workers, spy));
  const expected<std::unique_ptr<Surface>, SurfaceError> frame =
      renderer.render_frame_at(k_interior);
  REQUIRE(frame.has_value());
  return snapshot(**frame);
}

// An `InteractiveRenderer` driven to QUIESCENCE at `k_interior`: frames until nothing is
// pending. One frame parks only until the FIRST completion settles, so a fan-out over
// several workers needs several frames to reap them all; while `d_pending` is non-empty
// the still-scene early-out (`interactive.cpp:225`) cannot fire, so each frame really
// does reap and composite. On return the target holds the fully-warm composite.
class InteractiveDriver {
public:
  InteractiveDriver(Document& doc, Backend& backend, WorkerPoolConfig pool_config,
                    InteractiveRenderer::Clock clock)
      : d_doc(doc), d_cache(64U * 1024 * 1024), d_surfaces(backend), d_backend(backend),
        d_renderer(std::in_place, std::move(pool_config), std::move(clock)) {
    const DocStatePtr pin = doc.pin();
    expected<std::unique_ptr<Surface>, SurfaceError> target =
        backend.make_surface(k_dim, k_dim, pin->working_space());
    REQUIRE(target.has_value());
    d_target = std::move(*target);
  }

  InteractiveRenderer::FrameOutcome frame(std::chrono::steady_clock::duration budget) {
    const DocStatePtr pin = d_doc.pin();
    const ContentResolver resolve = [this](ObjectId id) { return d_doc.resolve(id); };
    const FrameBinding binding{&d_doc, pin};
    return d_renderer->render_frame(*pin, resolve, viewport(), d_cache, d_backend, d_surfaces,
                                    *d_target, {}, k_interior, budget, binding);
  }

  // Frames until the loop is genuinely settled: nothing in flight AND no follow-up
  // owed. Both conditions are load-bearing. A frame parks only until the FIRST
  // completion settles, so a fan-out needs several frames to reap them all; and a
  // frame that reaps an arrival does NOT composite it -- it carries the routed damage
  // (`interactive.cpp:357`) so the NEXT frame re-plans the operator's footprint
  // against the now-warm input. Stopping at `pending.empty()` would stop one frame
  // before the pixels exist. This is the loop a host runs on
  // `FrameOutcome::schedule_follow_up` (`host_viewport.cpp:160,178`).
  void drive_to_quiescence(std::chrono::steady_clock::duration budget) {
    constexpr int k_max_frames = 64; // a convergence bound, never a timing assumption
    for (int i = 0; i < k_max_frames; ++i) {
      const InteractiveRenderer::FrameOutcome outcome = frame(budget);
      if (!outcome.schedule_follow_up && d_renderer->pending().tiles.empty()) {
        return;
      }
    }
    FAIL("the interactive frame loop did not reach quiescence");
  }

  const RefinementQueue& pending() const noexcept { return d_renderer->pending(); }
  std::vector<float> pixels() const { return snapshot(*d_target); }

  // Destroy the renderer (and with it the pool) while this object -- and the target
  // surface -- stay alive, so the teardown case can observe the join.
  void destroy_renderer() { d_renderer.reset(); }

private:
  Document& d_doc;
  TileCache d_cache;
  SurfacePool d_surfaces;
  Backend& d_backend;
  std::unique_ptr<Surface> d_target;
  std::optional<InteractiveRenderer> d_renderer;
};

} // namespace

// --- A1: the helper itself, by thread identity ----------------------------------

// enforces: 02-architecture#worker-dispatch-is-leaf-only
TEST_CASE("worker_backed_dispatch renders an operator inline and fans a leaf out") {
  CpuBackend backend;
  DispatchSpy spy;
  WorkerPool pool(spied_pool(4, spy));
  const RenderDispatch dispatch = worker_backed_dispatch(pool);
  const std::thread::id driver = std::this_thread::get_id();

  expected<std::unique_ptr<Surface>, SurfaceError> op_target =
      backend.make_surface(k_dim, k_dim, k_working_rgba32f);
  expected<std::unique_ptr<Surface>, SurfaceError> leaf_target =
      backend.make_surface(k_dim, k_dim, k_working_rgba32f);
  REQUIRE(op_target.has_value());
  REQUIRE(leaf_target.has_value());

  RecordingLeaf leaf{Rgba{1.0F, 0.5F, 0.25F, 1.0F}};
  RecordingOperator op{&leaf}; // non-empty inputs() -- an operator, by definition

  // The OPERATOR branch: rendered inline, on the calling thread, settled before the
  // dispatch even returns, and never admitted to the pool at all.
  auto op_done = std::make_shared<RenderCompletion>();
  dispatch(&op,
           RenderRequest{canvas(), 1.0, k_interior, StateHandle{}, **op_target,
                         Exactness::BestEffort, Deadline::none()},
           op_done);
  CHECK(op_done->settled()); // synchronous: the inline branch is direct_dispatch's
  REQUIRE(op.log().renders() == 1);
  CHECK(op.log().threads().front() == driver);
  CHECK(op.log().off_thread(driver) == 0);
  CHECK(pool.tasks_submitted() == 0); // the operator never reached the pool
  CHECK(spy.operator_admissions() == 0);

  // The LEAF branch: submitted to the pool and rendered on a worker.
  auto leaf_done = std::make_shared<RenderCompletion>();
  dispatch(&leaf,
           RenderRequest{canvas(), 1.0, k_interior, StateHandle{}, **leaf_target,
                         Exactness::BestEffort, Deadline::none()},
           leaf_done);
  CHECK(pool.tasks_submitted() == 1);
  while (!leaf_done->settled()) {
    pool.wait_completions(std::nullopt);
  }
  REQUIRE(leaf.log().renders() == 1);
  CHECK(leaf.log().off_thread(driver) == 1); // a worker painted it, not this thread
}

// --- A1: both drivers, all three operator kinds ---------------------------------

// enforces: 02-architecture#worker-dispatch-is-leaf-only
TEST_CASE("the offline driver fans out only leaves: fade, crossfade and nested render "
          "on the driver thread") {
  CpuBackend backend;
  OperatorScene scene;
  DispatchSpy spy;
  const std::thread::id driver = std::this_thread::get_id();

  const std::vector<float> pixels = offline_frame(scene.doc, backend, 4, spy);
  CHECK_FALSE(all_transparent(pixels)); // the scene really did composite

  // Zero operator renders on any worker: no operator content was ever admitted to the
  // pool, and admission is the only way onto a worker thread.
  CHECK(spy.operator_admissions() == 0);
  // ...and the check is not vacuous: the pool WAS used, and a leaf really did leave this
  // thread. Without this, an inline executor would pass the assertion above trivially.
  CHECK(spy.admissions() > 0);
  REQUIRE(scene.leaf_renders() > 0);
  CHECK(scene.leaf_renders_off(driver) > 0);
}

// enforces: 02-architecture#worker-dispatch-is-leaf-only
TEST_CASE("the interactive driver fans out only leaves: fade, crossfade and nested render "
          "on the frame thread") {
  CpuBackend backend;
  OperatorScene scene;
  DispatchSpy spy;
  const std::thread::id driver = std::this_thread::get_id();

  InteractiveDriver view(scene.doc, backend, spied_pool(4, spy), InteractiveRenderer::Clock{});
  view.drive_to_quiescence(k_frame_budget);

  CHECK_FALSE(all_transparent(view.pixels()));
  CHECK(spy.operator_admissions() == 0);
  CHECK(spy.admissions() > 0);
  REQUIRE(scene.leaf_renders() > 0);
  CHECK(scene.leaf_renders_off(driver) > 0);
}

// --- A2: byte-identical across worker counts, on both drivers --------------------

// enforces: 02-architecture#worker-dispatch-is-leaf-only
// enforces: 02-architecture#worker-pool-degenerates-to-inline
TEST_CASE("a fade/crossfade/nested frame is byte-identical at every worker count, on both "
          "drivers") {
  CpuBackend backend;

  // The oracle: the export driver, inline (`worker_count == 0`, the degenerate executor).
  OperatorScene oracle_scene;
  DispatchSpy oracle_spy;
  const std::vector<float> oracle = offline_frame(oracle_scene.doc, backend, 0, oracle_spy);
  REQUIRE_FALSE(all_transparent(oracle));
  CHECK(oracle_spy.operator_admissions() == 0); // the rule holds at worker_count == 0 too

  // Exactness is order-independent, so a fan-out changes performance, not results. Each
  // count gets a FRESH scene (and so a cold cache), or a warm cache would do the work.
  for (const std::size_t workers : {std::size_t{1}, std::size_t{4}}) {
    OperatorScene offline_scene;
    DispatchSpy offline_spy;
    CHECK(byte_identical(offline_frame(offline_scene.doc, backend, workers, offline_spy), oracle));
    CHECK(offline_spy.operator_admissions() == 0);
  }

  // The interactive driver, driven to quiescence, converges on the very same pixels --
  // at the shipped inline default and at every worker count above it.
  for (const std::size_t workers : {std::size_t{0}, std::size_t{1}, std::size_t{4}}) {
    OperatorScene scene;
    DispatchSpy spy;
    InteractiveDriver view(scene.doc, backend, spied_pool(workers, spy),
                           InteractiveRenderer::Clock{});
    view.drive_to_quiescence(k_frame_budget);
    CHECK(byte_identical(view.pixels(), oracle));
    CHECK(spy.operator_admissions() == 0);
  }
}

// --- A3: in flight across the frame, the sweep, and the teardown ----------------

// enforces: 13-effects-as-operators#pull-retains-render-surface-until-settle
TEST_CASE("an in-flight worker render survives the frame's deadline expiry and reaps on a "
          "later frame") {
  CpuBackend backend;
  Gate gate;
  auto gated = std::make_shared<GatedLeaf>(gate, Rgba{0.75F, 0.5F, 0.25F, 1.0F});

  Document doc;
  doc.add_layer(doc.add_content(gated), Affine::identity());

  DispatchSpy spy;
  const ExpiringClock clock;
  InteractiveDriver view(doc, backend, spied_pool(2, spy), clock.functor());

  // The deadline is already past, so the frame does not park: it dispatches its miss to a
  // worker, finds nothing settled, and runs its expiry sweep over the still-unsettled
  // BestEffort pendings. Meanwhile the worker is parked INSIDE `render`, holding a
  // `Surface&` into `d_pending`.
  view.frame(std::chrono::milliseconds(0));
  gate.await_arrivals(1); // the render is genuinely in flight, and the frame has returned

  // (a) The frame is over, the sweep ran -- and nothing was freed. The pending tile is
  //     still resident and its surface is still the live target of an in-flight render,
  //     which is the claim this case enforces and is what the deadline does NOT get to
  //     take away.
  //
  //     The sweep also left the render itself alone (`runtime.deadline_cancel_retains_wanted`):
  //     this leaf is visible, at this revision, at this camera, so it is a tile the frame
  //     still WANTS, and only tiles it no longer wants are cancelled. (Before that task the
  //     sweep cancelled every unsettled entry, and this asserted `cancelled()`. Cancelling
  //     it was never what kept the surface alive -- cancellation is advisory
  //     (`content.hpp:161-165`) and never revoked the worker's target -- so the retention
  //     claim reads identically either way, and the reap in (b) is unchanged.)
  REQUIRE_FALSE(view.pending().tiles.empty());
  const PendingTile& tile = view.pending().tiles.front();
  CHECK(tile.surface != nullptr);
  CHECK_FALSE(tile.done->cancelled());
  CHECK_FALSE(tile.done->settled());

  // (b) Release the latch: the arrival reaps normally through `poll_refinements` on a
  //     later frame and composites. `cancel()` never settled the completion -- it only
  //     raised the advisory flag (`content.hpp:161-165`) -- so the worker's `complete()`
  //     still wins the settle and `poll_refinements` inserts a real result. The pixels
  //     prove the worker wrote into a surface that was still alive to receive them.
  //     The clock un-expires here so the reaping frames park for their workers rather
  //     than spinning (see `ExpiringClock`).
  gate.open();
  clock.unexpire();
  view.drive_to_quiescence(k_frame_budget);
  CHECK(view.pending().tiles.empty());
  CHECK_FALSE(all_transparent(view.pixels()));
}

// enforces: 13-effects-as-operators#pull-retains-render-surface-until-settle
TEST_CASE("destroying an InteractiveRenderer with a render in flight joins before it frees") {
  CpuBackend backend;
  Gate gate;
  auto gated = std::make_shared<GatedLeaf>(gate, Rgba{0.25F, 0.75F, 0.5F, 1.0F});

  Document doc;
  doc.add_layer(doc.add_content(gated), Affine::identity());

  DispatchSpy spy;
  const ExpiringClock clock; // stays expired: this case never reaps, it tears down
  InteractiveDriver view(doc, backend, spied_pool(2, spy), clock.functor());
  view.frame(std::chrono::milliseconds(0));
  gate.await_arrivals(1); // a worker is parked inside `render`, target surface in hand

  // (c) Tear the renderer down WHILE that render is in flight. `~InteractiveRenderer`
  //     destroys `d_pool` first (declaration order, `interactive.hpp:262-267`), and
  //     `~WorkerPool` stops and JOINS -- so the worker wakes, paints into the
  //     `PendingTile` surface, and settles, all before `d_pending` (which owns that
  //     surface) is destroyed. The releasing thread is started first so the store races
  //     the join, which is the schedule a reversed member order would fault on. ASan and
  //     TSan police the outcome; the test's own assertion is that it terminates.
  std::thread releaser([&gate] { gate.open(); });
  view.destroy_renderer();
  releaser.join();

  SUCCEED("renderer torn down with an in-flight worker render: joined, no use-after-free");
}

// --- A4: the nightly seeded schedule sweep --------------------------------------

// enforces: 02-architecture#worker-dispatch-is-leaf-only
TEST_CASE("worker dispatch stays leaf-only across many schedules", "[.nightly]") {
  CpuBackend backend;
  OperatorScene oracle_scene;
  DispatchSpy oracle_spy;
  const std::vector<float> oracle = offline_frame(oracle_scene.doc, backend, 0, oracle_spy);
  REQUIRE_FALSE(all_transparent(oracle));

  // The per-push TSan lane runs the fan-out once, on one interleaving. Here it runs under
  // many seeded schedules: the perturber yields on a random bit, widening the window in
  // which a worker and the driver thread would collide on the tile cache if an operator
  // ever escaped to a worker. Reproducible -- a red run replays from the logged seed.
  for (std::uint32_t seed = 0; seed < 64; ++seed) {
    INFO("seed = " << seed);
    arbc::test::Perturber perturb(seed);

    OperatorScene scene;
    DispatchSpy spy;
    perturb.maybe_yield();
    const std::vector<float> offline = offline_frame(scene.doc, backend, 4, spy);
    perturb.maybe_yield();

    REQUIRE(spy.operator_admissions() == 0);
    REQUIRE(spy.admissions() > 0);
    REQUIRE(byte_identical(offline, oracle));

    OperatorScene live;
    DispatchSpy live_spy;
    InteractiveDriver view(live.doc, backend, spied_pool(4, live_spy),
                           InteractiveRenderer::Clock{});
    perturb.maybe_yield();
    view.drive_to_quiescence(k_frame_budget);

    REQUIRE(live_spy.operator_admissions() == 0);
    REQUIRE(byte_identical(view.pixels(), oracle));
  }
}
