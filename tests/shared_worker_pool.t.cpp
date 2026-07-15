// Several `InteractiveRenderer`s share one `WorkerPool` (`runtime.shared_worker_pool`,
// doc 02 § Threading model).
//
// K viewports over K renderers over ONE pool is the only correct multi-viewport shape, and
// until this task it did not exist: the renderer built and owned its pool, so K viewports
// cost K x N threads. Handing them one pool is five lines. Making that pool CORRECT under
// sharing is the task, and it is two invariants, both of which were written assuming
// exclusive ownership and both of which this file drives through the real frame loop:
//
//   * THE WAKE (A3). The pool keeps one settle counter, but the DRAIN cursor is the
//     caller's. A sibling's settle may WAKE a parked renderer -- the wake is a broadcast,
//     because `poke()` is the contract-facing handle and its caller knows nothing about
//     renderers -- but it can never be CONSUMED by it. Under a pool-global cursor, renderer
//     A's park swallowed the generation bump produced by renderer B's tile settling, and B
//     re-parked to its deadline believing nothing had landed: it spuriously expired, and
//     cancelled or degraded tiles that were sitting finished in its own `RefinementQueue`.
//     A wrong-output bug, not a slow one, and it fired the moment a second renderer touched
//     the pool.
//
//   * THE TEARDOWN (A4). A BORROWED pool is not destructed when its renderer dies, so
//     `~WorkerPool`'s stop-and-join -- the entire reason a worker can never be caught
//     writing into a dead `PendingTile` today -- never runs. `~InteractiveRenderer` drains
//     its OWN submissions out of the pool instead: it purges what it queued and waits out
//     what it started, and it touches nothing of a sibling's, because a sibling viewport is
//     still rendering and must stay that way.
//
// And (A6) the pixels do not care. A renderer that borrows a pool of N workers paints what a
// renderer that owns a pool of N workers paints, byte for byte: pool OWNERSHIP is the newest
// instance of `02-architecture#worker-pool-degenerates-to-inline`'s standing promise that the
// pool's shape changes performance, never results.
//
// Everything here is a latch or a counter (doc 16:54-62). The park bounds are hang guards,
// asserted only through what they return.
//
// CROSS-COMPONENT (tests/, linking the umbrella `arbc`): a real `CpuBackend` and real content
// kinds beside the runtime drivers and the pool (doc 17:153).

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/expected.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/cache/keyed_store.hpp>
#include <arbc/compositor/counters.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/kind_solid/solid_content.hpp> // Rgba
#include <arbc/media/pixel_format.hpp>
#include <arbc/model/damage.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/interactive.hpp>
#include <arbc/runtime/worker_pool.hpp>
#include <arbc/surface/surface.hpp>
#include <arbc/surface/surface_pool.hpp>

#include <catch2/catch_test_macros.hpp>

#include "support/root_anchor.hpp"

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
#include <thread>
#include <utility>
#include <vector>

using namespace arbc;

namespace {

constexpr int k_dim = 256;        // one rung-0 tile per full-canvas layer
constexpr int k_wide = 2 * k_dim; // a 2x2 grid of rung-0 tiles
constexpr Time k_interior{500};

Rect canvas() { return Rect{0.0, 0.0, static_cast<double>(k_dim), static_cast<double>(k_dim)}; }
Rect wide_canvas() {
  return Rect{0.0, 0.0, static_cast<double>(k_wide), static_cast<double>(k_wide)};
}

// The loop's only clock, fixed at the steady_clock epoch: every deadline instant
// (`epoch + budget`) is in the real past, so the park never blocks and "the deadline has
// already passed" is a property of the schedule rather than a race with one.
InteractiveRenderer::Clock epoch_clock() {
  return [] { return std::chrono::steady_clock::time_point{}; };
}

// A clock a test moves between "already expired" and real, because the teardown cases need
// both (the idiom `interactive_worker_default.t.cpp:113-126` uses).
//
// EXPIRED for the frame that must return with a render STILL IN FLIGHT: the park does not block
// and the frame takes its deadline path with the leaf still on a worker. That is the state a
// renderer has to be destroyed in for its drain to mean anything.
//
// REAL for the frames that must genuinely WAIT for a worker -- the convergence afterwards. Left
// expired there, every reaping frame would be a no-wait poll that gives up instantly, and
// "converged" would be a race against the very worker it is waiting on rather than a wait for it.
class SwitchableClock {
public:
  InteractiveRenderer::Clock functor() const {
    return [expired = d_expired] {
      return expired->load(std::memory_order_acquire) ? std::chrono::steady_clock::time_point{}
                                                      : std::chrono::steady_clock::now();
    };
  }
  void expire() const { d_expired->store(true, std::memory_order_release); }
  void unexpire() const { d_expired->store(false, std::memory_order_release); }

private:
  std::shared_ptr<std::atomic<bool>> d_expired = std::make_shared<std::atomic<bool>>(true);
};

// A park bound over the REAL clock, for the frames that must genuinely WAIT for a worker.
// Nothing is timed and no assertion depends on its value; a frame with work in flight returns
// the instant a completion settles. The idiom `interactive_worker_default.t.cpp:133` uses.
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

// A manually-opened gate. A gate, not a sleep (doc 16:54-62): `await_arrivals` is what turns
// "a worker is inside `render` right now" from an assumption into an observation.
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

// A LEAF (no inputs, so `worker_backed_dispatch` may fan it out) that paints deterministically
// and, when `blocks` is non-zero, parks that many of its renders inside `render` on the gate.
//
// THE PAINT IS THE LAST THING IT DOES before recording its exit, and that matters more than it
// looks: `request.target` is a surface owned by a `PendingTile` in the SUBMITTING renderer's
// `RefinementQueue` (`refinement.hpp:65-79`). If a destroyed renderer's teardown failed to wait
// this render out, that write lands in freed memory -- so the teardown case below fails as an
// ASan/TSan report on the sanitizer lanes, not as a lucky-schedule pass.
class LatchLeaf final : public Content {
public:
  LatchLeaf(Gate& gate, Rgba color, int blocks, Rect bounds = canvas())
      : d_gate(gate), d_color(color), d_blocks(blocks), d_bounds(bounds) {}

  std::optional<Rect> bounds() const override { return d_bounds; }
  Stability stability() const override { return Stability::Static; }
  std::optional<TimeRange> time_extent() const override { return std::nullopt; }
  bool render_thread_safe() const override { return true; }

  std::optional<RenderResult> render(const RenderRequest& request,
                                     std::shared_ptr<RenderCompletion> /*done*/) override {
    d_entered.fetch_add(1, std::memory_order_acq_rel);
    if (d_blocks.fetch_sub(1, std::memory_order_acq_rel) > 0) {
      d_gate.arrive_and_wait();
    }
    paint(request, d_color); // into the submitter's PendingTile surface -- see above
    d_exited.fetch_add(1, std::memory_order_acq_rel);
    return RenderResult{request.scale, /*exact=*/true};
  }

  std::uint64_t entered() const { return d_entered.load(std::memory_order_acquire); }
  std::uint64_t exited() const { return d_exited.load(std::memory_order_acquire); }

private:
  Gate& d_gate;
  Rgba d_color;
  std::atomic<int> d_blocks;
  Rect d_bounds;
  std::atomic<std::uint64_t> d_entered{0};
  std::atomic<std::uint64_t> d_exited{0};
};

// One viewport: an `InteractiveRenderer` plus the cache, surface pool and target a host
// persists for it. Two constructors, and the difference between them IS the task -- one BUILDS
// a pool, one BORROWS the host's.
//
// Each viewport gets its OWN `TileCache` (Constraint 7). Sharing a pool does not license
// sharing a cache: workers never touch the cache (the leaf-only rule), so all cache traffic is
// on each renderer's own frame thread -- and `KeyedStore` is single-thread-confined, so K
// renderers driven from K threads need K caches. A pool is shareable across threads; a cache
// is not, and one does not imply the other.
class ViewportDriver {
public:
  // BORROW the host's pool.
  ViewportDriver(Document& doc, Backend& backend, WorkerPool& pool,
                 InteractiveRenderer::Clock clock, int dim = k_dim)
      : d_doc(doc), d_cache(64U * 1024 * 1024), d_surfaces(backend), d_backend(backend), d_dim(dim),
        d_renderer(pool, std::move(clock)) {
    make_target();
  }
  // OWN one (the shipped per-viewport default).
  ViewportDriver(Document& doc, Backend& backend, WorkerPoolConfig config,
                 InteractiveRenderer::Clock clock, int dim = k_dim)
      : d_doc(doc), d_cache(64U * 1024 * 1024), d_surfaces(backend), d_backend(backend), d_dim(dim),
        d_renderer(std::move(config), std::move(clock)) {
    make_target();
  }

  InteractiveRenderer::FrameOutcome frame(std::chrono::steady_clock::duration budget,
                                          std::span<const Damage> damage = {}) {
    const DocStatePtr pin = d_doc.pin();
    const ContentResolver resolve = [this](ObjectId id) { return d_doc.resolve(id); };
    const FrameBinding binding{&d_doc, pin};
    // The anchor is computed once on the constructing (main) thread in `make_target`:
    // `frame` runs on a worker-driving thread too (the A3 thief), and deriving the root
    // via a Catch `REQUIRE` there would be an off-main-thread assertion.
    const Viewport view{d_dim, d_dim, Affine::identity(), d_anchor};
    return d_renderer.render_frame(*pin, resolve, view, d_cache, d_backend, d_surfaces, *d_target,
                                   damage, k_interior, budget, binding);
  }

  void drive_to_quiescence(std::chrono::steady_clock::duration budget) {
    constexpr int k_max_frames = 64; // a convergence bound, never a timing assumption
    for (int i = 0; i < k_max_frames; ++i) {
      const InteractiveRenderer::FrameOutcome outcome = frame(budget);
      if (!outcome.schedule_follow_up && d_renderer.pending().tiles.empty()) {
        return;
      }
    }
    FAIL("the interactive frame loop did not reach quiescence");
  }

  const InteractiveRenderer& renderer() const noexcept { return d_renderer; }
  InteractiveRenderer& renderer() noexcept { return d_renderer; } // worker_pool() is non-const
  std::vector<float> pixels() const { return snapshot(*d_target); }

private:
  void make_target() {
    const DocStatePtr pin = d_doc.pin();
    d_anchor = arbc::test::root_composition_of(*pin);
    expected<std::unique_ptr<Surface>, SurfaceError> target =
        d_backend.make_surface(d_dim, d_dim, pin->working_space());
    REQUIRE(target.has_value());
    d_target = std::move(*target);
  }

  Document& d_doc;
  TileCache d_cache;
  SurfacePool d_surfaces;
  Backend& d_backend;
  int d_dim;
  ObjectId d_anchor{};
  std::unique_ptr<Surface> d_target;
  InteractiveRenderer d_renderer;
};

// One leaf, one layer, one document -- the smallest scene that dispatches a real leaf miss to
// a worker. `wide` picks a four-tile footprint over a one-tile one.
struct Scene {
  Gate gate;
  std::shared_ptr<LatchLeaf> leaf;
  Document doc;
  ObjectId content{};

  Scene(Rgba color, int blocks, bool wide = false)
      : leaf(std::make_shared<LatchLeaf>(gate, color, blocks, wide ? wide_canvas() : canvas())) {
    // The frame walk is composition-scoped, so the leaf's layer must be a member of the
    // composition the viewport anchors at (compositor.root_composition_frame_walk, doc
    // 05:28-36).
    const double edge = static_cast<double>(wide ? k_wide : k_dim);
    const ObjectId comp = doc.add_composition(edge, edge);
    content = doc.add_content(leaf);
    doc.attach_layer(comp, doc.add_layer(content, Affine::identity()));
  }
};

WorkerPoolConfig pool_of(std::size_t workers) {
  WorkerPoolConfig config;
  config.worker_count = workers;
  return config;
}

} // namespace

// --- A3: the wake, at the driver -- the wrong-output regression this prevents ------------

// The bug, precisely. Renderer B parks in `wait_completions` with its leaf still rendering.
// The worker settles it and `poke()`s: the settle generation advances. B is about to wake and
// see it -- but renderer A, sharing the pool, parks in that window, and under a POOL-GLOBAL
// drain cursor A's park CONSUMES the advance for everybody (`d_drained_gen = d_settle_gen`).
// B wakes, re-tests, finds the generation "already drained", and keeps waiting -- to its
// deadline. It then expires: it cancels or degrades a tile that is sitting FINISHED in its own
// `RefinementQueue`, and schedules a follow-up frame it did not owe. Wrong output, from a
// renderer that did nothing wrong.
//
// A is the thief and it is a real one: an already-expired injected clock, so every one of its
// frames dispatches, parks, finds its deadline gone and returns -- as fast as it can loop --
// while its own leaf sits blocked on a latch and never settles. Its retained-not-cancelled
// tile is what keeps it looping (`runtime.deadline_cancel_retains_wanted`: the tile is still
// wanted, so the sweep leaves it in flight and the next frame joins it rather than dispatching
// a second one), and `tiles_retained() >= 1` is the positive witness that it really was
// parking-and-expiring rather than quietly quiescing into a still scene.
//
// B, meanwhile, is an ordinary viewport doing ordinary work on a real clock: its leaves settle
// promptly and it must converge without ever expiring. Every counter asserted on B is a
// statement about the wake, because a wake it loses is the ONLY thing on this scene that can
// make it expire.
//
// The deterministic, race-free half of this claim is at the pool
// (`src/runtime/t/worker_pool.t.cpp`, two cursors and one settle). This is the half that shows
// what the pool bug COSTS: a viewport that degrades for no reason.
//
// enforces: 02-architecture#shared-pool-park-observes-only-its-own-settles
TEST_CASE("a renderer parked on a shared pool never loses a wake to a sibling") {
  CpuBackend backend;

  // Four workers: A's one blocked render can occupy at most one of them (its tile is retained
  // and therefore joined, never re-dispatched), so B always has workers to render on. The
  // scenes are independent documents -- two viewports onto two compositions, the topology
  // `DamageRouter` ships for.
  WorkerPool pool(pool_of(4));

  Scene a_scene(Rgba{0.75F, 0.5F, 0.25F, 1.0F}, /*blocks=*/1);
  Scene b_scene(Rgba{0.2F, 0.4F, 0.6F, 1.0F}, /*blocks=*/0);

  // The oracle for B's pixels: the same scene on its own inline renderer. Sharing a pool must
  // not change what B paints, only when.
  Scene oracle_scene(Rgba{0.2F, 0.4F, 0.6F, 1.0F}, /*blocks=*/0);
  ViewportDriver oracle(oracle_scene.doc, backend, WorkerPoolConfig{},
                        InteractiveRenderer::Clock{});
  oracle.drive_to_quiescence(k_frame_budget);
  const std::vector<float> expected_pixels = oracle.pixels();
  REQUIRE_FALSE(all_transparent(expected_pixels));

  ViewportDriver a(a_scene.doc, backend, pool, epoch_clock());
  ViewportDriver b(b_scene.doc, backend, pool, InteractiveRenderer::Clock{});

  // A, on its own thread, hammering the pool's completion condvar with expired parks. K
  // renderers driven from K threads, each with its own `TileCache` -- the supported topology,
  // and the reason this file lands on the per-push TSan lane.
  std::atomic<bool> b_done{false};
  std::thread thief([&a, &b_done] {
    // Bounded, so the hot loop cannot outlive the measurement on a loaded CI box; then it
    // simply waits. B needs a handful of frames, and A steals from every one of them.
    for (int i = 0; i < 500 && !b_done.load(std::memory_order_acquire); ++i) {
      a.frame(k_frame_budget);
    }
    while (!b_done.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
  });
  a_scene.gate.await_arrivals(1); // A's render really is in flight, holding A unsettled

  const InteractiveRenderer::FrameOutcome out = [&] {
    InteractiveRenderer::FrameOutcome last{};
    for (int i = 0; i < 64; ++i) {
      last = b.frame(k_frame_budget);
      if (!last.schedule_follow_up && b.renderer().pending().tiles.empty()) {
        return last;
      }
    }
    FAIL("B did not reach quiescence");
    return last;
  }();

  b_done.store(true, std::memory_order_release);
  thief.join();

  // B never lost a wake. Its leaf settled while it was parked, and it was woken by that settle
  // and by nothing else -- so it never reached its deadline, never swept, never degraded, and
  // owed no follow-up frame it had not earned.
  CHECK(b.renderer().deadline_expiries() == 0);
  CHECK(b.renderer().tiles_cancelled() == 0);
  CHECK_FALSE(out.schedule_follow_up);
  CHECK(byte_identical(b.pixels(), expected_pixels));

  // Degrading is bounded by B's ONE tile, and that bound is the assertion -- not zero. A cold
  // worker-backed frame composites the placeholder exactly once, while its leaf is still on a
  // worker: that is doc 02:63-65's preference order doing its job, and it is what every
  // `worker_count > 0` first frame in the tree does (`interactive_worker_default.t.cpp` measures
  // the same `>= degraded_before + 1`). A LOST wake shows up as a SECOND one -- the frame that
  // should have reaped the settled tile instead parks to its deadline, expires, and re-composites
  // the placeholder -- so the bound still catches the regression, and `deadline_expiries() == 0`
  // above catches it head-on.
  CHECK(b.renderer().counters().degraded_composites() <= 1);

  // A, for its part, SHOULD expire -- it is genuinely blocked, and a deadline it cannot meet is
  // a deadline it must enforce. The point is not that nobody expires; it is that nobody expires
  // on somebody else's account.
  CHECK(a.renderer().deadline_expiries() >= 1);
  CHECK(a.renderer().tiles_retained() >= 1); // it was parking-and-expiring, not idling
  CHECK(a.renderer().tiles_cancelled() == 0);

  a_scene.gate.open(); // let A's render out before the pool and the renderers go
}

// --- A4: teardown drains only its own -----------------------------------------------------

// enforces: 02-architecture#shared-pool-teardown-drains-only-its-own-submissions
TEST_CASE("a renderer's teardown drains its own submissions out of a shared pool, and only "
          "its own") {
  CpuBackend backend;

  SECTION("in flight: ~InteractiveRenderer does not return while a worker is inside its "
          "leaf's render") {
    WorkerPool pool(pool_of(4));

    Scene a_scene(Rgba{0.75F, 0.5F, 0.25F, 1.0F}, /*blocks=*/1);
    Scene b_scene(Rgba{0.2F, 0.4F, 0.6F, 1.0F}, /*blocks=*/1);

    // A's clock starts EXPIRED, so its frame returns with its leaf still on a worker; it is
    // un-expired below, once A has to genuinely WAIT for that worker to converge.
    const SwitchableClock a_clock;
    ViewportDriver a(a_scene.doc, backend, pool, a_clock.functor());
    // B lives in an `optional` for one reason: this test needs to DESTROY it, on purpose, with
    // a render still running. That is the moment the whole acceptance criterion is about.
    std::optional<ViewportDriver> b;
    b.emplace(b_scene.doc, backend, pool, epoch_clock());

    // Both viewports dispatch, both park against an already-past deadline, both return with a
    // leaf still rendering on a worker. Observed on the gates, not assumed.
    a.frame(k_frame_budget);
    b->frame(k_frame_budget);
    a_scene.gate.await_arrivals(1);
    b_scene.gate.await_arrivals(1);
    REQUIRE_FALSE(b->renderer().pending().tiles.empty());
    REQUIRE(b_scene.leaf->exited() == 0); // still in there

    const std::uint64_t completed_before = pool.tasks_completed();

    // B dies with its render in flight. The releaser lets that render finish; the destructor
    // must not return until it HAS. `paint` -- the render's last act -- writes into a surface
    // owned by a `PendingTile` inside the very `RefinementQueue` this destructor is about to
    // destroy, so a teardown that returned early would be a use-after-free, and ASan/TSan would
    // say so rather than letting it pass on a lucky schedule.
    std::thread releaser([&b_scene] { b_scene.gate.open(); });
    b.reset();
    releaser.join();

    CHECK(b_scene.leaf->exited() == 1); // the render RETURNED before the renderer died
    CHECK(pool.tasks_completed() == completed_before + 1);

    // The sibling is untouched, and that is the other half of the promise. A's render was NOT
    // waited on (its gate is still shut) and was NOT cancelled or purged -- it is exactly where
    // it was, still in flight, still owed to A.
    CHECK(a_scene.leaf->exited() == 0);
    CHECK_FALSE(a.renderer().pending().tiles.empty());
    CHECK(a.renderer().tiles_cancelled() == 0);

    // And the pool is still ALIVE and still A's: releasing A's latch, A converges. The drain
    // did not reach for `request_stop()` -- which is terminal and pool-global, and would have
    // stranded A's renders unsettled forever (Constraint 10).
    a_scene.gate.open();
    a_clock.unexpire(); // the reaping frames must WAIT for A's worker, not poll past it
    a.drive_to_quiescence(k_frame_budget);
    CHECK(a.renderer().pending().tiles.empty());
    CHECK_FALSE(all_transparent(a.pixels()));
    CHECK(a_scene.leaf->exited() >= 1);
    CHECK(pool.tasks_submitted() == pool.tasks_completed() + pool.tasks_dropped());
  }

  SECTION("queued: the never-started tasks are purged, not run, and the sibling still gets a "
          "usable pool") {
    // ONE worker, held open inside a blocked render, is what makes "queued" a state this test
    // constructs rather than hopes for: with the only worker in the gate, every later dispatch
    // is provably unstarted.
    WorkerPool pool(pool_of(1));

    // B's leaf is wide -- a 2x2 tile grid -- so its frame dispatches FOUR renders. The first
    // takes the worker and blocks; the other three sit in the ready queue, untouched.
    Scene b_scene(Rgba{0.2F, 0.4F, 0.6F, 1.0F}, /*blocks=*/1, /*wide=*/true);
    std::optional<ViewportDriver> b;
    b.emplace(b_scene.doc, backend, pool, epoch_clock(), k_wide);

    b->frame(k_frame_budget);
    b_scene.gate.await_arrivals(1);
    REQUIRE(pool.tasks_submitted() == 4);
    REQUIRE(b_scene.leaf->entered() == 1); // one started; three queued and cannot start
    REQUIRE(pool.tasks_dropped() == 0);

    constexpr std::uint64_t k_queued = 3;
    std::thread releaser([&b_scene] { b_scene.gate.open(); });
    b.reset();
    releaser.join();

    // The three queued renders were THROWN AWAY, not run: the destructor purged them out of the
    // pool's queue and returned. Their completions are left unsettled, which is safe for the
    // same reason a post-stop `submit` is -- the completion is a `shared_ptr` its caller owned,
    // and it died with the caller.
    CHECK(pool.tasks_dropped() == k_queued);
    CHECK(pool.tasks_completed() == 1); // only the one that had already started
    CHECK(b_scene.leaf->entered() == 1);
    CHECK(b_scene.leaf->exited() == 1); // and that one was waited out, as above
    CHECK(pool.tasks_submitted() == pool.tasks_completed() + pool.tasks_dropped());

    // A fresh renderer on the same pool renders normally afterwards. The purge emptied B's work
    // out of the queues without stopping the pool, so the pool a dead viewport leaves behind is
    // a pool the living ones can still use.
    Scene a_scene(Rgba{0.75F, 0.5F, 0.25F, 1.0F}, /*blocks=*/0);
    ViewportDriver a(a_scene.doc, backend, pool, InteractiveRenderer::Clock{});
    a.drive_to_quiescence(k_frame_budget);
    CHECK(a.renderer().deadline_expiries() == 0);
    CHECK(a.renderer().pending().tiles.empty());
    CHECK_FALSE(all_transparent(a.pixels()));
    CHECK(pool.tasks_completed() >= 2);
    CHECK(pool.tasks_submitted() == pool.tasks_completed() + pool.tasks_dropped());
  }
}

// --- A6: owning a pool and borrowing one paint the same pixels ---------------------------

// The pool's shape changes performance, not results -- that is the standing promise of
// `02-architecture#worker-pool-degenerates-to-inline`, and pool OWNERSHIP is its newest
// instance. A renderer handed a pool of N workers must paint exactly what a renderer that
// builds a pool of N workers paints, and both must paint what the thread-free inline executor
// paints. Nothing about who allocated the threads can reach the pixels.
//
// enforces: 02-architecture#worker-pool-degenerates-to-inline
TEST_CASE("a borrowed pool and an owned pool of the same size paint byte-identical frames") {
  CpuBackend backend;

  Scene inline_scene(Rgba{0.35F, 0.65F, 0.15F, 1.0F}, /*blocks=*/0, /*wide=*/true);
  ViewportDriver inline_view(inline_scene.doc, backend, WorkerPoolConfig{},
                             InteractiveRenderer::Clock{}, k_wide);
  inline_view.drive_to_quiescence(k_frame_budget);
  const std::vector<float> pixels = inline_view.pixels();
  REQUIRE_FALSE(all_transparent(pixels));

  Scene owned_scene(Rgba{0.35F, 0.65F, 0.15F, 1.0F}, /*blocks=*/0, /*wide=*/true);
  ViewportDriver owned(owned_scene.doc, backend, pool_of(3), InteractiveRenderer::Clock{}, k_wide);
  owned.drive_to_quiescence(k_frame_budget);
  CHECK(owned.renderer().worker_pool().worker_count() == 3);
  CHECK(byte_identical(owned.pixels(), pixels));

  WorkerPool host_pool(pool_of(3));
  Scene borrowed_scene(Rgba{0.35F, 0.65F, 0.15F, 1.0F}, /*blocks=*/0, /*wide=*/true);
  ViewportDriver borrowed(borrowed_scene.doc, backend, host_pool, InteractiveRenderer::Clock{},
                          k_wide);
  borrowed.drive_to_quiescence(k_frame_budget);
  CHECK(&borrowed.renderer().worker_pool() == &host_pool);
  CHECK(byte_identical(borrowed.pixels(), pixels));
  CHECK(host_pool.tasks_submitted() == host_pool.tasks_completed() + host_pool.tasks_dropped());
}
