// The shipped interactive default renders leaf misses OFF the frame thread, so the
// deadline is enforceable (`runtime.interactive_worker_count_default`, doc 02
// § Threading model, doc 02:61-65).
//
// This is the whole argument for a non-zero default, and it is a CORRECTNESS argument,
// not a performance one. Doc 02:61-65 promises: "Cache misses become render requests with
// a deadline... When the deadline nears, the frame proceeds with what it has:
// stale-revision tiles, coarser-scale tiles rescaled, or checkerboard/transparent, in
// that preference order." At `worker_count == 0` the pool is the DEGENERATE INLINE
// EXECUTOR (`worker_pool.cpp:66`, `02-architecture#worker-pool-degenerates-to-inline`):
// `submit` IS the render, so the frame thread is inside a slow leaf's `render` when the
// deadline passes. It reaches the deadline park only AFTER every miss has been rendered
// to completion -- there is nothing left to degrade to, and nothing to cancel. The
// promise cannot be kept, whatever the budget says.
//
// The two arms below are that sentence, made observable, over the SAME scene and the same
// already-expired injected clock, differing only in the pool config:
//
//   * `WorkerPoolConfig{}` (inline): the leaf's `render` runs on the FRAME THREAD, and the
//     frame degrades NOWHERE -- zero cancellations, zero degraded composites -- because it
//     could not return until the leaf was done.
//   * `default_interactive_pool_config()` (the shipped default): the leaf's `render` runs
//     on a WORKER, and the frame returns with the deadline expired -- one deadline expiry,
//     the in-flight tile cancelled, and the previous revision's tile composited as the
//     degraded fallback, which is doc 02:63-65's preference order actually firing. The
//     latch is then released and the arrival settles on a later frame.
//
// Everything is a latch or a counter: no test here reads a wall clock or sleeps to
// synchronize (doc 16:54-62). The deadline instant is in the real past by construction
// (the injected epoch clock), so "the deadline expired" is a fact about the schedule, not
// a race with one.
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
#include <arbc/compositor/counters.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/kind_crossfade/crossfade_content.hpp>
#include <arbc/kind_fade/fade_content.hpp>
#include <arbc/kind_nested/nested_content.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/media/pixel_format.hpp>
#include <arbc/model/damage.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/interactive.hpp>
#include <arbc/runtime/worker_pool.hpp>
#include <arbc/surface/surface.hpp>
#include <arbc/surface/surface_pool.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

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

constexpr int k_dim = 256; // one rung-0 tile per full-canvas layer
constexpr Time k_interior{500};

Rect canvas() { return Rect{0.0, 0.0, static_cast<double>(k_dim), static_cast<double>(k_dim)}; }

FadeParams half_fade() {
  return FadeParams{FadeShape::Linear, std::nullopt, FadeWindow{Time{0}, Time{1000}}};
}
CrossfadeParams half_crossfade() {
  return CrossfadeParams{CrossfadeShape::Linear, Time{0}, Time{1000}};
}

// The loop's only clock, injected and fixed at the steady_clock epoch: every deadline
// instant (`epoch + budget`) is in the real past, so `wait_completions` never parks and
// "the deadline has already passed" is a property of the schedule rather than a race.
InteractiveRenderer::Clock epoch_clock() {
  return [] { return std::chrono::steady_clock::time_point{}; };
}

// A clock the test moves between "already expired" and real, because A3 needs both
// (`worker_dispatch_leaf_only.t.cpp:117-129` uses the same device in the other direction).
//
// REAL, for the frames that must genuinely WAIT for a worker -- the cold warm-up and the
// final reap. The frame parks on the pool's completion condvar and wakes when a worker
// settles, which is what a host does. Leaving the clock expired there would make every
// reaping frame a no-wait poll that re-dispatches and immediately gives up: a busy spin
// against the very worker it is waiting on.
//
// EXPIRED (the steady_clock epoch, so every deadline lands in the real past), for the ONE
// frame under test: `wait_completions` returns immediately without parking and the frame
// takes its deadline-cancel path with a render still in flight. That is the schedule the
// whole claim is about, and "the deadline has passed" is then a fact about the schedule,
// not a race with one. Neither state is a timing assumption.
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
  std::shared_ptr<std::atomic<bool>> d_expired = std::make_shared<std::atomic<bool>>(false);
};

// A park bound over the REAL clock for the frames that must actually WAIT for a worker
// (the reaping loops). Nothing is timed and no assertion depends on the value: a frame
// with work in flight returns the instant a completion settles, and cancellation is
// advisory (`content.hpp:161-165`), so a missed deadline costs the loop one more turn,
// never a wrong answer.
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

// --- The latch ----------------------------------------------------------------

// A manually-opened gate. A gate, not a sleep: wall-clock tests lie in CI, latches do not
// (doc 16:54-62). `await_arrivals` is what turns "the render is still in flight" from an
// assumption into an observation.
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

// A LEAF (no inputs, so `worker_backed_dispatch` may fan it out) that records the thread
// each `render` ran on, and -- once ARMED -- parks inside `render` on the gate.
//
// The arming is what lets one content serve both halves of the scene: the FIRST render
// (frame 1, warming the cache at revision R) passes straight through, and only the render
// the deadline case depends on blocks. Deterministic pixels -- a pure function of the
// request -- so the frame is byte-identical whichever thread paints it, which is exactly
// what makes the fan-out sound.
class LatchLeaf final : public Content {
public:
  // `bounds` defaults to the k_dim canvas; the cross-frame-join scene below spells a wider
  // one so its single layer covers a 2x2 tile grid rather than one tile.
  LatchLeaf(Gate& gate, Rgba color, Rect bounds = canvas())
      : d_gate(gate), d_color(color), d_bounds(bounds) {}

  std::optional<Rect> bounds() const override { return d_bounds; }
  Stability stability() const override { return Stability::Static; }
  std::optional<TimeRange> time_extent() const override { return std::nullopt; }
  bool render_thread_safe() const override { return true; }

  std::optional<RenderResult> render(const RenderRequest& request,
                                     std::shared_ptr<RenderCompletion> /*done*/) override {
    {
      const std::lock_guard<std::mutex> lock(d_mutex);
      d_threads.push_back(std::this_thread::get_id());
    }
    if (d_armed.load(std::memory_order_acquire)) {
      d_gate.arrive_and_wait();
    }
    paint(request, d_color);
    return RenderResult{request.scale, /*exact=*/true};
  }

  void arm() { d_armed.store(true, std::memory_order_release); }

  std::size_t renders() const {
    const std::lock_guard<std::mutex> lock(d_mutex);
    return d_threads.size();
  }
  // Renders observed on a thread OTHER than `driver` -- i.e. on a worker. The pool is the
  // only mechanism in the tree that moves a render off the calling thread, so thread
  // identity IS the observable (the idiom `worker_dispatch_leaf_only.t.cpp:541,562` uses).
  std::size_t renders_off(std::thread::id driver) const {
    const std::lock_guard<std::mutex> lock(d_mutex);
    return static_cast<std::size_t>(std::count_if(
        d_threads.begin(), d_threads.end(), [driver](std::thread::id t) { return t != driver; }));
  }

private:
  Gate& d_gate;
  Rgba d_color;
  Rect d_bounds;
  std::atomic<bool> d_armed{false};
  mutable std::mutex d_mutex;
  std::vector<std::thread::id> d_threads;
};

// --- The driver ---------------------------------------------------------------

// An `InteractiveRenderer` over one document, with the caller-persisted cache/target a
// host owns. `frame()` drives one pass; `drive_to_quiescence` frames until nothing is in
// flight AND no follow-up is owed -- both conditions load-bearing, because a frame parks
// only until the FIRST completion settles and a frame that REAPS an arrival does not
// composite it (it carries the routed damage for the next frame). This is the loop a host
// runs on `FrameOutcome::schedule_follow_up` (`host_viewport.cpp:160,178`).
class Driver {
public:
  // `dim` is the square viewport/target edge, defaulting to the one-tile `k_dim` every arm
  // above uses; the cross-frame-join scene drives a 2x2 tile grid instead.
  Driver(Document& doc, Backend& backend, WorkerPoolConfig pool_config,
         InteractiveRenderer::Clock clock, int dim = k_dim)
      : d_doc(doc), d_cache(64U * 1024 * 1024), d_surfaces(backend), d_backend(backend), d_dim(dim),
        d_renderer(std::move(pool_config), std::move(clock)) {
    const DocStatePtr pin = doc.pin();
    // The frame walk is composition-scoped: anchor at the document's root composition, the
    // same id the shipped drivers source (compositor.root_composition_frame_walk, doc
    // 05:28-36). Computed once here, on the main thread.
    d_anchor = arbc::test::root_composition_of(*pin);
    expected<std::unique_ptr<Surface>, SurfaceError> target =
        backend.make_surface(d_dim, d_dim, pin->working_space());
    REQUIRE(target.has_value());
    d_target = std::move(*target);
  }

  InteractiveRenderer::FrameOutcome frame(std::chrono::steady_clock::duration budget,
                                          std::span<const Damage> damage = {}) {
    const DocStatePtr pin = d_doc.pin();
    const ContentResolver resolve = [this](ObjectId id) { return d_doc.resolve(id); };
    const FrameBinding binding{&d_doc, pin};
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
  InteractiveRenderer& renderer() noexcept { return d_renderer; }
  std::vector<float> pixels() const { return snapshot(*d_target); }

private:
  Document& d_doc;
  TileCache d_cache;
  SurfacePool d_surfaces;
  Backend& d_backend;
  int d_dim;
  ObjectId d_anchor{};
  std::unique_ptr<Surface> d_target;
  InteractiveRenderer d_renderer;
};

} // namespace

// --- A1: the default is a policy, not a magic number ---------------------------

// `hardware_concurrency()` varies by machine, so the SHIPPED default varies by machine: a
// test that hard-codes a number is green on the author's box and red in CI. Every
// assertion here is therefore a PROPERTY (Constraint 8).
TEST_CASE("the interactive default worker count is a non-zero, capped policy") {
  const std::size_t n = default_interactive_worker_count();
  CHECK(n >= 1);                         // never the degenerate inline executor
  CHECK(n <= k_max_interactive_workers); // the pool is per-renderer: never hw-1 unbounded
  CHECK(default_interactive_pool_config().worker_count == n);

  // The renderer a host gets when it names no config carries exactly that pool...
  InteractiveRenderer shipped;
  CHECK(shipped.worker_pool().worker_count() == n);

  // ...and the explicit inline opt-out still gives a thread-free renderer (Constraint 3):
  // one spelling, and every deterministic golden and unit test in the tree uses it.
  InteractiveRenderer opted_out(WorkerPoolConfig{}, epoch_clock());
  CHECK(opted_out.worker_pool().worker_count() == 0);
}

// --- A3: the headline behavioral claim ------------------------------------------

namespace {

// The scene both arms drive, and the shape is load-bearing.
//
// TWO full-canvas leaf layers: the LATCHED leaf under test, and a TRIGGER leaf that carries
// the model damage. The trigger exists because the damage has to re-plan the latched leaf's
// tile WITHOUT invalidating it: `invalidate_damage` drops the tiles of the content the
// damage NAMES, so damage on the latched leaf would evict the very prior-revision tile the
// degrade is supposed to fall back to, and there would be nothing to observe. Damaging the
// trigger instead maps to the whole viewport (it is full-canvas), so BOTH layers re-plan --
// and only the trigger's tiles are dropped. The latched leaf's revision-R tile survives,
// resident, as the stale fallback doc 02:63-65 promises.
//
// The trigger is a LatchLeaf too, and not a plain SolidContent, because it MISSES on the
// measured frame as well: the damage names the trigger, so `invalidate_damage` drops its
// tiles outright, and a leaf miss is dispatched to a worker. (Before per-object revisions
// it missed for a second reason too -- the document-global bump re-keyed every visible
// tile. That reason is gone; the invalidation one was always the load-bearing one, and it
// is untouched.) A fast trigger is therefore a second render RACING the frame thread -- and if it
// settles before the deadline park is reached, its arrival damage schedules a follow-up
// frame, so `schedule_follow_up` stops being a statement about the design and becomes a
// statement about which thread won. Gating the trigger on the same latch is what removes
// that race: on the measured frame NOTHING can settle, so "no arrival settled, so no
// follow-up is owed" is a fact. Only the worker arm arms it; inline, the frame thread does
// every render itself and there is no race to remove.
struct LatchScene {
  Gate gate;
  std::shared_ptr<LatchLeaf> leaf =
      std::make_shared<LatchLeaf>(gate, Rgba{0.75F, 0.5F, 0.25F, 1.0F});
  std::shared_ptr<LatchLeaf> trigger =
      std::make_shared<LatchLeaf>(gate, Rgba{0.1F, 0.2F, 0.3F, 0.4F});
  Document doc;
  ObjectId leaf_content{};
  ObjectId trigger_content{};
  ObjectId trigger_layer{};

  LatchScene() {
    const ObjectId comp =
        doc.add_composition(static_cast<double>(k_dim), static_cast<double>(k_dim));
    leaf_content = doc.add_content(leaf);
    doc.attach_layer(comp, doc.add_layer(leaf_content, Affine::identity()));
    trigger_content = doc.add_content(trigger);
    trigger_layer = doc.add_layer(trigger_content, Affine::identity());
    doc.attach_layer(comp, trigger_layer);
  }

  // Re-key the latched leaf (so its fresh key becomes a miss) and hand back the damage
  // that re-plans the viewport without dropping its prior-stamp tile.
  //
  // The re-key is a re-STAMP of the leaf's own CONTENT record. Under per-object revisions
  // (`model.per_object_revision`) that is the only thing that moves a content's tile key:
  // a placement edit on some OTHER layer publishes a new document revision but leaves
  // every content's stamp exactly where it was -- which is the whole point of the task,
  // and which would leave the latched leaf's tile FRESH, so it would never re-render and
  // the releaser below would wait forever. The damage still names the TRIGGER, not the
  // leaf, so `invalidate_damage` drops the trigger's tiles and spares the leaf's prior
  // tile -- the resident stale fallback the degrade is supposed to reach for.
  std::vector<Damage> disturb() {
    auto txn = doc.transact("disturb");
    txn.set_content_state(leaf_content, StateHandle{}); // re-stamps the LEAF: its key moves
    REQUIRE(txn.commit().has_value());
    return {Damage{trigger_content, Rect::infinite(), TimeRange::all()}};
  }
};

} // namespace

// enforces: 02-architecture#interactive-default-renders-leaves-off-the-frame-thread
TEST_CASE("the shipped default renders leaf misses off the frame thread, so the deadline "
          "is enforceable") {
  const std::thread::id driver = std::this_thread::get_id();

  SECTION("inline (WorkerPoolConfig{}): the leaf renders ON the frame thread, so the frame "
          "has nothing to degrade to") {
    CpuBackend backend;
    LatchScene scene;
    const SwitchableClock clock;
    Driver view(scene.doc, backend, WorkerPoolConfig{}, clock.functor());

    // The cold frame warms both tiles at revision R. `submit` IS the render here, so one
    // frame is one complete frame: nothing is left in flight.
    view.frame(k_frame_budget);
    REQUIRE(scene.leaf->renders() == 1);
    REQUIRE(scene.leaf->renders_off(driver) == 0); // inline: on this thread, by construction
    REQUIRE(view.renderer().pending().tiles.empty());
    const std::vector<float> sharp = view.pixels();
    REQUIRE_FALSE(all_transparent(sharp));

    scene.leaf->arm();
    const std::vector<Damage> damage = scene.disturb();
    clock.expire(); // the deadline for the next frame is already in the past

    // The frame thread is about to block INSIDE the latched leaf's `render`. A releaser
    // opens the gate once it OBSERVES the arrival -- which is the proof the frame thread
    // really is in there, and the only way this arm terminates at all.
    std::thread releaser([&scene] {
      scene.gate.await_arrivals(1);
      scene.gate.open();
    });
    const std::uint64_t expiries_before = view.renderer().deadline_expiries();
    const InteractiveRenderer::FrameOutcome out = view.frame(k_frame_budget, damage);
    releaser.join();

    // The leaf rendered again, on THIS thread, and `render_frame` could not return until
    // it finished. So by the time the frame reached its (already-expired) deadline park,
    // the miss was ALREADY filled: nothing in flight to cancel, and no degraded source to
    // fall back to -- because it never had to fall back. Doc 02:61-65's promise is not
    // merely unmet here, it is structurally unmeetable, and these are the counters that
    // say so.
    REQUIRE(scene.leaf->renders() == 2);
    CHECK(scene.leaf->renders_off(driver) == 0);
    CHECK(view.renderer().tiles_cancelled() == 0);
    // ...and nothing to RETAIN either (`runtime.deadline_cancel_retains_wanted`). Inline
    // dispatch settles every miss before the park is reached, so the sweep -- whether it
    // cancels or retains -- has an empty queue to walk. The two counters partition the
    // unsettled entries at expiry, and here there are none of either.
    CHECK(view.renderer().tiles_retained() == 0);
    CHECK(view.renderer().counters().degraded_composites() == 0);
    CHECK(view.renderer().pending().tiles.empty());
    CHECK_FALSE(out.schedule_follow_up);
    // The deadline did not even EXPIRE, though the deadline instant was already in the
    // past when the frame started. `wait_completions` returns whether a completion settled
    // since the last drain -- and the inline executor settled every one of them, on this
    // thread, before the park was reached at all. So the frame sails through Step 5 with
    // nothing to enforce. That is the sharpest form of the point: at `worker_count == 0`
    // the deadline is not missed, it is IRRELEVANT -- by the time the frame is in a
    // position to ask "has my budget run out?", the answer cannot change anything.
    CHECK(view.renderer().deadline_expiries() == expiries_before);
  }

  SECTION("the shipped default: the leaf renders on a WORKER, so the frame degrades to the "
          "stale tile and refines") {
    CpuBackend backend;
    LatchScene scene;
    const SwitchableClock clock; // real for now: the warm-up frames must WAIT for the pool
    Driver view(scene.doc, backend, default_interactive_pool_config(), clock.functor());

    view.drive_to_quiescence(k_frame_budget);
    // Every render of the latched leaf so far ran on a WORKER, not on this thread. That is
    // the assertion; the COUNT is deliberately not one, because it is not deterministic.
    // The two layers overlap (they must -- see `LatchScene`), so if the trigger's tile
    // settles first, its arrival damage re-plans the whole viewport while the latched
    // leaf's tile is still in flight, and a tile in flight is a cache miss that gets
    // dispatched again (`tile_planning.cpp:351`, `compositor.in_flight_tile_dedup`).
    // Whether that happens depends on which worker finishes first, so pinning `== 1` here
    // would be a test that passes on an idle machine and fails on a busy one.
    const std::size_t warm_renders = scene.leaf->renders();
    REQUIRE(warm_renders >= 1);
    CHECK(scene.leaf->renders_off(driver) == warm_renders);
    const std::vector<float> sharp = view.pixels();
    REQUIRE_FALSE(all_transparent(sharp));

    // Arm BOTH leaves: the one under test, and the trigger whose R+1 miss would otherwise
    // race the frame thread to settle (see `LatchScene`). With both latched, no render can
    // complete during the measured frame, so every counter below is a fact about the
    // deadline and not about the scheduler.
    scene.leaf->arm();
    scene.trigger->arm();
    const std::vector<Damage> damage = scene.disturb();
    clock.expire();

    const std::uint64_t cancelled_before = view.renderer().tiles_cancelled();
    const std::uint64_t retained_before = view.renderer().tiles_retained();
    const std::uint64_t degraded_before = view.renderer().counters().degraded_composites();
    const std::uint64_t expiries_before = view.renderer().deadline_expiries();
    const std::uint64_t follow_ups_before = view.renderer().counters().follow_up_frames();

    // The frame the whole task is about. The R+1 miss goes to a worker, which parks inside
    // `render`. The frame thread is NOT in there, so it reaches the deadline park, finds
    // the deadline already gone, and returns with the best pixels it has -- compositing the
    // resident revision-R tile as the degraded fallback. That is doc 02:63-65's preference
    // order actually firing, and it can only fire because the render is somewhere the frame
    // is not.
    //
    // What it does NOT do is cancel that render (`runtime.deadline_cancel_retains_wanted`).
    // The R+1 tile is exactly a tile the frame still wants -- visible, at this revision, at
    // this camera -- so the sweep leaves it in flight, and the follow-up frame that
    // re-plans it joins the render already running instead of dispatching a second one. The
    // DEGRADE is the park's doing, not the cancel's, which is why it is unchanged here: the
    // deadline is enforced by not waiting past it (Decision 6).
    const InteractiveRenderer::FrameOutcome out = view.frame(k_frame_budget, damage);
    scene.gate.await_arrivals(1); // the render is genuinely in flight, and the frame returned

    CHECK(view.renderer().deadline_expiries() == expiries_before + 1);
    CHECK(view.renderer().tiles_retained() >= retained_before + 1);
    CHECK(view.renderer().tiles_cancelled() == cancelled_before);
    CHECK(view.renderer().counters().degraded_composites() >= degraded_before + 1);
    REQUIRE_FALSE(view.renderer().pending().tiles.empty());
    // NOTHING settled during this frame -- both leaves are latched, which is precisely why
    // the scene latches the trigger too -- so this frame schedules no follow-up: there is
    // no arrival damage to schedule one FROM. The work is owed through the pending queue,
    // which is what keeps the loop turning (the still-scene early-out cannot fire while a
    // tile is in flight, `interactive.cpp:225`) and what `HostViewport::step()` now also
    // checks before deciding a viewport is idle.
    CHECK_FALSE(out.schedule_follow_up);

    // Release the latch. Cancellation is ADVISORY (`content.hpp:161-165`): it never revoked
    // the worker's target, so the render completes into a surface that is still alive, the
    // arrival reaps on a later frame, and the sharp tile replaces the stale one -- landing
    // byte-for-byte on the pixels the cold frame produced.
    clock.unexpire();
    scene.gate.open();
    view.drive_to_quiescence(k_frame_budget);
    CHECK(view.renderer().counters().follow_up_frames() > follow_ups_before);
    CHECK(view.renderer().pending().tiles.empty());
    CHECK(scene.leaf->renders_off(driver) == scene.leaf->renders()); // every render, on a worker
    CHECK(byte_identical(view.pixels(), sharp));
  }
}

// --- A4: the cross-frame in-flight join (runtime.deadline_cancel_retains_wanted) ---

namespace {

constexpr int k_wide = 2 * k_dim; // a 2x2 grid of rung-0 tiles

Rect wide_canvas() {
  return Rect{0.0, 0.0, static_cast<double>(k_wide), static_cast<double>(k_wide)};
}

// Every worker count the benchmark sweeps, including the machine's own `hw - 1` (the
// unclamped formula) -- so the invariants below are pinned on whatever CI runs on, not
// just on a hand-picked pair. Shared by the two sweeping cases (A4 and A5).
std::vector<std::size_t> swept_worker_counts() {
  std::vector<std::size_t> counts{0, 1, 2, 4};
  const unsigned hw = std::thread::hardware_concurrency();
  if (hw > 5) {
    counts.push_back(static_cast<std::size_t>(hw) - 1);
  }
  return counts;
}

WorkerPoolConfig pool_of(std::size_t workers) {
  WorkerPoolConfig config;
  config.worker_count = workers;
  return config;
}

// The scene the cross-frame identity needs, and every part of its shape is load-bearing.
//
// A VISIBLE LEAF LAYER, not an operator's input, because the wave gate would otherwise
// confound the measurement: it defers the operator's re-render before the input could ever
// be re-pulled, so the suppression under test would never be reached.
//
// FOUR TILES, so the frame's dispatch is plural and the retention is not a single-entry
// special case.
//
// ONE CONTENT, damaged over its whole footprint on the second frame, so every tile the
// first frame dispatched is RE-PLANNED while its render is still running. That is the
// collision the predecessor's scenes could not stage: theirs shared no leaf tile between
// askers, so `requests_suppressed()` was provably `0` and the whole intra-frame identity
// passed vacuously (`in_flight_tile_dedup`'s Status block).
struct WideLatchScene {
  Gate gate;
  std::shared_ptr<LatchLeaf> leaf =
      std::make_shared<LatchLeaf>(gate, Rgba{0.6F, 0.3F, 0.15F, 1.0F}, wide_canvas());
  Document doc;
  ObjectId content{};

  WideLatchScene() {
    const ObjectId comp =
        doc.add_composition(static_cast<double>(k_wide), static_cast<double>(k_wide));
    content = doc.add_content(leaf);
    doc.attach_layer(comp, doc.add_layer(content, Affine::identity()));
  }

  // Damage over the whole leaf, at the SAME revision -- so the next frame re-plans every
  // one of its tiles without re-keying them. Nothing is in the cache to invalidate (the
  // renders have not landed), so this is purely a repaint instruction.
  std::vector<Damage> repaint_all() const {
    return {Damage{content, Rect::infinite(), TimeRange::all()}};
  }
};

} // namespace

// The headline assertion, and the one the predecessor could not make. `in_flight_tile_dedup`
// landed the guard but shipped it unfired: its scenes shared no tile, so
// `requests_suppressed()` was 0 and "requests_issued did not grow" was equally what you
// observe when a mechanism is disconnected. And it could not have fired ACROSS a frame
// boundary in any scene, because the deadline sweep cancelled every unsettled entry on
// expiry and `tile_in_flight` refuses to suppress against a cancelled one -- so every entry
// that crossed a boundary was disqualified before the next frame planned. Cross-frame dedup
// was forfeited structurally, by that loop.
//
// Narrow the sweep to the tiles the frame no longer wants and the guard becomes reachable.
// This is the counter identity that proves it, one frame further out than the predecessor
// could reach:
//
//   * `requests_issued()` over the WHOLE two-frame sequence equals the number of distinct
//     `TileKey`s the sequence needed -- the oracle, measured on the inline run;
//   * `requests_suppressed() >= 1` -- the POSITIVE proof the cross-frame guard fired, since
//     without it the identity above passes vacuously (`in_flight_tile_dedup` Decision 5);
//   * `tiles_retained() >= 1` and `tiles_cancelled() == 0` -- the sweep looked at the queue
//     and decided to keep, rather than never having run;
//   * the quiesced image is byte-identical to the inline oracle's (Constraint 1).
//
// enforces: 02-architecture#retained-tile-is-suppressed-next-frame
// enforces: 02-architecture#deadline-sweep-retains-wanted-tiles
TEST_CASE("a tile retained across a deadline expiry is joined, not re-dispatched") {
  CpuBackend backend;

  // The oracle: the same scene at the degenerate inline executor. `submit` IS the render
  // there, so nothing is ever in flight -- nothing suppressed, nothing retained, nothing
  // cancelled -- and its `requests_issued` is exactly the number of distinct tile keys the
  // sequence needs (one leaf, one revision, one rung, four coords).
  WideLatchScene oracle_scene;
  Driver oracle(oracle_scene.doc, backend, WorkerPoolConfig{}, InteractiveRenderer::Clock{},
                k_wide);
  oracle.drive_to_quiescence(k_frame_budget);
  const std::vector<float> pixels = oracle.pixels();
  REQUIRE_FALSE(all_transparent(pixels));
  const std::uint64_t distinct_keys = oracle.renderer().counters().requests_issued();
  REQUIRE(distinct_keys == 4);
  CHECK(oracle.renderer().counters().requests_suppressed() == 0);
  CHECK(oracle.renderer().tiles_retained() == 0);
  CHECK(oracle.renderer().tiles_cancelled() == 0);

  for (const std::size_t workers : swept_worker_counts()) {
    if (workers == 0) {
      continue; // inline: nothing is ever in flight, so there is nothing to retain or join
    }
    INFO("worker_count = " << workers);

    WideLatchScene scene;
    const SwitchableClock clock;
    Driver view(scene.doc, backend, pool_of(workers), clock.functor(), k_wide);

    scene.leaf->arm(); // every render parks: nothing can settle during the two frames below
    clock.expire();    // ...and both frames' deadlines are already in the real past

    // Frame N. Four misses, four dispatches, four renders parked on workers, and a deadline
    // that is already gone. The sweep runs -- and every tile is visible, at this revision,
    // at this camera, so it RETAINS all four rather than cancelling them.
    view.frame(k_frame_budget);
    scene.gate.await_arrivals(1); // the renders really are in flight: observed, not assumed
    CHECK(view.renderer().deadline_expiries() >= 1);
    CHECK(view.renderer().tiles_retained() >= 1);
    CHECK(view.renderer().tiles_cancelled() == 0);
    CHECK(view.renderer().counters().requests_issued() == distinct_keys);
    CHECK(view.renderer().counters().requests_suppressed() == 0); // nothing to join YET

    // Frame N+1, the frame this task exists for: the same viewport, damaged over the whole
    // leaf, so all four tiles are re-planned while their renders are still running. Under
    // the blanket sweep every one of them was cancelled at frame N, `tile_in_flight` would
    // decline to suppress, and this frame would dispatch a SECOND render of four tiles that
    // are already being rendered. Retained, they are joined instead.
    view.frame(k_frame_budget, scene.repaint_all());
    CHECK(view.renderer().counters().requests_issued() == distinct_keys); // delta 0
    CHECK(view.renderer().counters().requests_suppressed() >= 1);         // ...and it FIRED
    CHECK(view.renderer().tiles_cancelled() == 0);

    // Release the latch and converge. The identity holds over the whole sequence -- exactly
    // one render per distinct tile key -- and the pixels are the inline oracle's, byte for
    // byte: retaining a render changes when it lands, never what it paints (Constraint 1).
    clock.unexpire();
    scene.gate.open();
    view.drive_to_quiescence(k_frame_budget);
    CHECK(view.renderer().counters().requests_issued() == distinct_keys);
    CHECK(view.renderer().counters().requests_suppressed() >= 1);
    CHECK(view.renderer().tiles_retained() >= 1);
    CHECK(view.renderer().tiles_cancelled() == 0);
    CHECK(view.renderer().pending().tiles.empty());
    CHECK(byte_identical(view.pixels(), pixels));
  }
}

// --- A5: the parallel-path invariants, at every worker count ---------------------

namespace {

enum class SceneKind { LeafHeavy, OperatorHeavy, NestedDeep };

const char* scene_name(SceneKind kind) {
  switch (kind) {
  case SceneKind::LeafHeavy:
    return "leaf_heavy";
  case SceneKind::OperatorHeavy:
    return "operator_heavy";
  case SceneKind::NestedDeep:
    return "nested_deep";
  }
  return "unknown";
}

// The three scenes the benchmark sweeps, built with the real kinds at an instant where
// NEITHER operator is an identity endpoint (the fade's envelope is 0.5, the crossfade's w
// is 0.5), so each really runs its own `render`. The operators borrow their leaves
// non-owningly (`ContentRef` is a raw `Content*`), so the leaves are held here and outlive
// the `Document`.
//
// Built IN PLACE rather than returned from a factory: `Document` is non-copyable and
// non-movable (it owns arenas), so a scene that holds one by value is too.
class Scene {
public:
  explicit Scene(SceneKind kind) {
    switch (kind) {
    case SceneKind::LeafHeavy:
      build_leaf_heavy();
      break;
    case SceneKind::OperatorHeavy:
      build_operator_heavy();
      break;
    case SceneKind::NestedDeep:
      build_nested_deep();
      break;
    }
  }

  Document& doc() noexcept { return d_doc; }

private:
  // Independent leaf contents TILED ACROSS the viewport: each bounded to its own vertical
  // strip, so their footprints are DISJOINT. That is not decoration -- it is what makes
  // the render-count equality below a meaningful guard rather than a coincidence. An
  // arrival's damage re-plans the region it covers, and a tile that is re-planned while
  // its render is still in flight is a cache miss and is dispatched AGAIN
  // (`pull_service.cpp:219-243` is cache-first, with no check against the refinement
  // queue). With disjoint strips an arrival can only re-plan its OWN layer's tile, which
  // is resident by then, so nothing is re-dispatched. Make these three layers overlap --
  // three full-canvas solids, say -- and the first arrival re-plans the other two while
  // they are still on workers, and the scene renders five times instead of three.
  //
  // `compositor.in_flight_tile_dedup` has since landed the queue check, so an overlapping
  // variant would now be suppressed rather than re-dispatched, and the geometry is no
  // longer load-bearing for THIS assertion. It is kept as-is because the scene's job is to
  // pin the flat path, not to exercise the guard (`counters.t.cpp` does that, on a scene
  // built to share a tile); rewriting it would move this file's byte-identity baseline for
  // no coverage gained.
  void build_leaf_heavy() {
    const ObjectId comp =
        d_doc.add_composition(static_cast<double>(k_dim), static_cast<double>(k_dim));
    constexpr int k_strips = 3;
    const double strip = static_cast<double>(k_dim) / static_cast<double>(k_strips);
    for (int i = 0; i < k_strips; ++i) {
      const Rect bounds{static_cast<double>(i) * strip, 0.0, strip, static_cast<double>(k_dim)};
      auto leaf = std::make_shared<SolidContent>(
          Rgba{0.2F * static_cast<float>(i + 1), 0.5F, 0.25F, 1.0F}, bounds);
      d_doc.attach_layer(comp, d_doc.add_layer(d_doc.add_content(leaf), Affine::identity()));
      d_held.push_back(leaf);
    }
  }

  void build_operator_heavy() {
    const ObjectId comp =
        d_doc.add_composition(static_cast<double>(k_dim), static_cast<double>(k_dim));
    auto under = std::make_shared<SolidContent>(Rgba{0.25F, 0.50F, 0.75F, 1.0F}, canvas());
    auto from = std::make_shared<SolidContent>(Rgba{0.50F, 0.25F, 0.125F, 1.0F}, canvas());
    auto to = std::make_shared<SolidContent>(Rgba{0.125F, 0.375F, 0.75F, 1.0F}, canvas());
    auto fade = std::make_shared<FadeContent>(under.get(), half_fade());
    auto xf = std::make_shared<CrossfadeContent>(from.get(), to.get(), half_crossfade());
    d_doc.attach_layer(comp, d_doc.add_layer(d_doc.add_content(fade), Affine::identity()));
    d_doc.attach_layer(comp, d_doc.add_layer(d_doc.add_content(xf), Affine::identity()));
    d_held = {under, from, to, fade, xf};
  }

  void build_nested_deep() {
    auto a = std::make_shared<SolidContent>(Rgba{0.60F, 0.20F, 0.10F, 0.80F}, canvas());
    auto b = std::make_shared<SolidContent>(Rgba{0.10F, 0.40F, 0.30F, 0.50F}, canvas());
    auto fade_a = std::make_shared<FadeContent>(a.get(), half_fade());
    auto fade_b = std::make_shared<FadeContent>(b.get(), half_fade());
    // The parent composition the frame anchors at is created FIRST so it is the lowest-id
    // (root) composition; the child holds the two fades the nested content shows
    // (compositor.root_composition_frame_walk, doc 05:28-36).
    const ObjectId parent =
        d_doc.add_composition(static_cast<double>(k_dim), static_cast<double>(k_dim));
    const ObjectId child =
        d_doc.add_composition(static_cast<double>(k_dim), static_cast<double>(k_dim));
    d_doc.attach_layer(child, d_doc.add_layer(d_doc.add_content(fade_a), Affine::identity()));
    d_doc.attach_layer(child, d_doc.add_layer(d_doc.add_content(fade_b), Affine::identity()));
    auto nested = std::make_shared<NestedContent>(child);
    d_doc.attach_layer(parent, d_doc.add_layer(d_doc.add_content(nested), Affine::identity()));
    d_held = {a, b, fade_a, fade_b, nested};
  }

  Document d_doc;
  std::vector<std::shared_ptr<Content>> d_held;
};

} // namespace

// The anti-waste guard, and the TSan coverage for the newly-default-threaded frame loop.
// It runs in the standard suite, so it is on the per-push `gcc-tsan` lane with no
// exclusions: a counter bumped from a worker (Constraint 7) fails here.
//
// The invariants are the ones that hold UNCONDITIONALLY, at every worker count:
//
//   * the loop reaches quiescence (a fan-out that never settles is the failure mode a
//     bounded frame count catches, and `drive_to_quiescence` FAILs rather than hangs);
//   * `max_in_flight_per_content() <= 1` -- the pool never ran two renders of one content
//     at once, measured at the render call site independently of the admission gate
//     (`worker_pool.hpp:126-128`);
//   * the pixels are byte-identical to the inline (`worker_count == 0`) run. Adding
//     workers changes WHEN a leaf's pixels land, never what they are -- the cash value of
//     `02-architecture#worker-dispatch-is-leaf-only`'s byte-identity clause, and what
//     makes the shipped default safe for every golden in the tree.
//
// And, for the operator scenes, the render count IS now pinned -- but not against the
// inline oracle, which is the wrong right-hand side and always was.
//
// The WBS chartered two prior tasks against `requests_issued() == oracle_requests`, and it
// is unachievable: no coalescing scheme can reach it, because the operator's FIRST render
// is how its inputs get requested at all. The driver does not know an operator's input
// tiles; it discovers them by rendering the operator and watching it pull (doc 13:69-83).
// So on a cold cache with a worker pool an operator MUST render once to dispatch its
// inputs, and that render necessarily produces a placeholder -- the inputs it just
// dispatched have not landed -- and MUST then render a second time, once they have, to
// compose the real pixels. Two renders per operator is the FLOOR, not the waste. At
// `worker_count == 0` the floor is one, because `submit` IS the render: the dispatched leaf
// settles inline into the cache before the pull returns, so the operator's first and only
// render is already exact. The inline oracle measures a regime that structurally cannot
// have a placeholder pass, and demanding equality against it is asking the threaded run to
// be synchronous.
//
// So the honest identity, derived from the mechanism: every LEAF renders exactly once (the
// cache and the in-flight guard between them ensure that), and every OPERATOR renders
// exactly twice. It is falsifiable, and it was already confirmed before this task landed:
// `operator_heavy` has 2 operators and 3 leaves, so it predicts 5 + 2 = 7 -- which is what
// the threaded run measured. `nested_deep` has 3 operators and 2 leaves, so it predicts
// 5 + 3 = 8 against 12 measured, and 12 -> 8 is what
// `compositor.operator_refinement_wave_amplification` delivers.
//
// The excess it removed was the refinement WAVE, not duplicate dispatch. An operator whose
// input answers asynchronously must still paint something this frame, so it composites a
// placeholder and reports it INEXACT (doc 13:117-120 -- flagging it exact would freeze the
// empty tile into the cache as a fresh hit). An inexact tile is not a hit, so the arrival
// re-drives the operator and it re-renders -- correctly, once: that re-render is how the
// real pixels finally get composed. The two-pass identity above (`operator_renders() ==
// 2x`, `requests_issued()` grown by exactly the operator count) is what this scene pins.
//
// This scene does NOT witness `renders_coalesced() > 0`, and after
// `compositor.root_composition_frame_walk` (doc 05:28-36) that is the correct, positive
// observation, not a gap. The in-flight wave gate only ever fired here because the old
// document-global walk DOUBLE-DREW the nested child -- rendering its fades flat at top
// level and letting `nested` re-pull the resident transient the same frame. With the walk
// composition-scoped, `nested` is the sole renderer of its child's tiles: nothing is pre-
// rendered for it to coalesce (see the `== 0` and its note in the body). The deterministic
// wave-coalescing witness now lives entirely at the compositor level, where a partial
// arrival can be staggered by hand: `src/compositor/t/counters.t.cpp` and
// `src/compositor/t/refinement.t.cpp` pin `renders_coalesced() > 0` under a controlled
// settle -- the `enforces:` for that claim moved there with the observation.
//
// Duplicate dispatch needs a tile that TWO askers want while it is in flight, and these two
// scenes have none: every layer is full-canvas at a 256px viewport, so each covers exactly
// one tile, and no leaf is shared -- the fade owns `under`, the crossfade owns `from`/`to`,
// and the nested chain's two fades own a leaf each.
//
// The flat scene has no operator layer at all, so it has no placeholder pass and no wave:
// its count is invariant against the oracle outright, and nothing is ever coalesced.
//
// enforces: 02-architecture#worker-dispatch-is-leaf-only
// enforces: 02-architecture#worker-pool-degenerates-to-inline
TEST_CASE("the interactive frame loop is byte-identical and duplicate-free at every "
          "worker count") {
  CpuBackend backend;

  const SceneKind kind =
      GENERATE(SceneKind::LeafHeavy, SceneKind::OperatorHeavy, SceneKind::NestedDeep);
  INFO("scene = " << scene_name(kind));
  const bool flat = kind == SceneKind::LeafHeavy;

  // The oracle: the same scene at the degenerate inline executor.
  Scene oracle_scene(kind);
  Driver oracle(oracle_scene.doc(), backend, WorkerPoolConfig{}, InteractiveRenderer::Clock{});
  oracle.drive_to_quiescence(k_frame_budget);
  const std::vector<float> pixels = oracle.pixels();
  REQUIRE_FALSE(all_transparent(pixels));
  const std::uint64_t oracle_requests = oracle.renderer().counters().requests_issued();
  REQUIRE(oracle_requests >= 1);
  // The chain's operator count -- the whole right-hand side of the coalescing identity.
  const std::uint64_t oracle_operator_renders = oracle.renderer().counters().operator_renders();
  REQUIRE((flat ? oracle_operator_renders == 0 : oracle_operator_renders >= 1));
  // The inline oracle never coalesces: nothing is ever in flight when `submit` IS the
  // render, so the gate has nothing to hold and provably does not fire.
  REQUIRE(oracle.renderer().counters().renders_coalesced() == 0);

  for (const std::size_t workers : swept_worker_counts()) {
    INFO("worker_count = " << workers);
    // A FRESH scene (and so a cold cache) per count, or a warm cache would do the work.
    Scene scene(kind);
    Driver view(scene.doc(), backend, pool_of(workers), InteractiveRenderer::Clock{});
    view.drive_to_quiescence(k_frame_budget);

    CHECK(byte_identical(view.pixels(), pixels));
    CHECK(view.renderer().worker_pool().max_in_flight_per_content() <= 1);
    CHECK(view.renderer().frames_rendered() >= 1);
    const arbc::CompositorCounters& counters = view.renderer().counters();
    if (flat) {
      // No operator layer => no placeholder pass, no wave => exactly the inline renders.
      CHECK(counters.requests_issued() == oracle_requests);
      CHECK(counters.operator_renders() == 0);
      CHECK(counters.renders_coalesced() == 0);
    } else if (workers == 0) {
      // `submit` IS the render: every dispatched leaf settles inline into the cache
      // before the pull returns, so the operator's first and only render is already
      // exact. Nothing is ever in flight, so nothing is ever coalesced -- the positive
      // witness that the gate does not fire where it has no business firing.
      CHECK(counters.requests_issued() == oracle_requests);
      CHECK(counters.operator_renders() == oracle_operator_renders);
      CHECK(counters.renders_coalesced() == 0);
    } else {
      // THE TWO-PASS IDENTITY. Every leaf renders exactly once; every operator exactly
      // twice -- once to request its inputs and paint the placeholder, once when the wave
      // lands. That is what "at most one chain re-render per wave" means when the wave is
      // singular, which on a cold-cache scene it is.
      CHECK(counters.requests_issued() == oracle_requests + oracle_operator_renders);
      CHECK(counters.operator_renders() == 2 * oracle_operator_renders);
      // And it is still NOT duplicate dispatch: nothing is re-dispatched, so nothing is
      // suppressed. The second operator render finds its leaf already warm in the cache,
      // so it re-pulls a hit -- never a second dispatch of a render already in flight.
      CHECK(counters.requests_suppressed() == 0);

      // No intra-frame coalescing at ANY worker count now, and that is the composition-
      // scoped walk working (compositor.root_composition_frame_walk, doc 05:28-36). The
      // in-flight wave gate only ever COALESCED a `nested` scene because the old document-
      // global walk DOUBLE-DREW the child: it rendered `nested`'s child fades FLAT at top
      // level (dispatching their leaves) and THEN `nested` re-pulled those same fade tiles
      // the same frame, found them resident-but-transient with leaves still queued, and the
      // gate held the redundant re-pull -- the `renders_coalesced()` the double-draw made
      // deterministic. With the walk scoped, `nested` is the SOLE renderer of its child's
      // operator tiles: each is rendered once, its leaf dispatched once, and the second
      // (wave-land) render hits a warm leaf -- there is no pre-rendered transient to
      // coalesce and no redundant re-pull to hold. Strictly less work, and the `== 0` is
      // the positive witness that the double-draw (and the gate cleanup it needed) are gone.
      // The deterministic wave-coalescing witness lives where the arrivals can be staggered
      // by hand -- the compositor-level `counters.t.cpp` / `refinement.t.cpp`, which pin
      // `renders_coalesced() > 0` under a controlled partial arrival.
      CHECK(counters.renders_coalesced() == 0);
    }
  }
}
