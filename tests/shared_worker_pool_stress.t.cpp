// The concurrency story of a SHARED `WorkerPool` (`runtime.shared_worker_pool`, doc 02
// § Threading model; doc 16 tier 6).
//
// K = 4 `InteractiveRenderer`s over ONE pool, each on its OWN thread, each with its own
// `TileCache`, `SurfacePool` and target (Constraint 7 -- sharing a pool does not license
// sharing a cache; `KeyedStore` is single-thread-confined and the leaf-only rule, not the
// pool's topology, is what keeps the cache off the workers). Renderers are constructed and
// destroyed MID-FLIGHT on a seeded schedule, which is the point: the destructor's per-owner
// drain is the mechanism under test, and a drain is only interesting while somebody else is
// still rendering.
//
// The invariants asserted at quiescence:
//
//   * `max_in_flight_per_content() <= 1` for the non-thread-safe leaf. This is
//     `02-architecture#worker-pool-serializes-non-thread-safe-content` holding MORE strongly
//     than it used to: `d_serial` is keyed by `const Content*`, and two viewports onto one
//     document render the same `Content*`, so the gate now serializes that content ACROSS
//     renderers, not merely within one. A strengthening worth asserting, not a new claim
//     (Decision 5).
//   * `tasks_submitted() == tasks_completed() + tasks_dropped()` -- the accounting identity.
//     Every task the pool admitted either ran to a settlement or was purged by a dying
//     renderer's drain. A task that went missing (a purge that dropped a sibling's work, a
//     drain that returned with work still queued) breaks it.
//   * no renderer that ran to quiescence expired its deadline: none of these leaves blocks,
//     so a deadline expiry here would mean a lost wake, which is the wrong-output bug the
//     caller-owned cursor exists to prevent.
//   * the process exits clean -- no hang in a drain, no worker left writing into a destroyed
//     renderer's `PendingTile` surface. That last one is what ASan and TSan are for, and this
//     binary runs in the standard suite, so it lands on the per-push `gcc-tsan` lane with no
//     exclusion.
//
// The hidden `[.nightly]` section sweeps more seeds; it is registered in nightly.yml's
// `tsan-full` long-form list beside `worker_dispatch_leaf_only`.
//
// No wall-clock assertion (doc 16:224-226): the frame budget is a park bound, the frame count
// a convergence bound, and neither is ever asserted as a duration.

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

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

using namespace arbc;

namespace {

constexpr int k_dim = 256;
constexpr Time k_interior{500};
constexpr auto k_frame_budget = std::chrono::milliseconds(100); // a park bound, never a timing
constexpr int k_renderers = 4;

Rect canvas() { return Rect{0.0, 0.0, static_cast<double>(k_dim), static_cast<double>(k_dim)}; }

// A deterministic 32-bit LCG. A SEEDED schedule, not a random one: a failing seed is a
// reproducible failing seed, which is the only kind worth having in a concurrency stress
// (the idiom `worker_dispatch_leaf_only.t.cpp`'s nightly sweep uses).
class Rng {
public:
  explicit Rng(std::uint32_t seed) : d_state(seed | 1U) {}
  std::uint32_t next() {
    d_state = d_state * 1664525U + 1013904223U;
    return d_state;
  }
  std::size_t below(std::size_t n) { return static_cast<std::size_t>(next() % n); }

private:
  std::uint32_t d_state;
};

// A leaf whose thread-safety declaration is a CONSTRUCTOR ARGUMENT, so one scene carries both
// disciplines: the thread-safe leaves fan out freely, and the one that declares
// `render_thread_safe() == false` must be serialized by the pool -- now across renderers, since
// every viewport onto this document renders the same `Content*`.
class StressLeaf final : public Content {
public:
  StressLeaf(Rgba color, bool thread_safe) : d_color(color), d_thread_safe(thread_safe) {}

  std::optional<Rect> bounds() const override { return canvas(); }
  Stability stability() const override { return Stability::Static; }
  std::optional<TimeRange> time_extent() const override { return std::nullopt; }
  bool render_thread_safe() const override { return d_thread_safe; }

  std::optional<RenderResult> render(const RenderRequest& request,
                                     std::shared_ptr<RenderCompletion> /*done*/) override {
    const std::span<float> px = request.target.span<PixelFormat::Rgba32fLinearPremul>();
    for (std::size_t i = 0; i + 3 < px.size(); i += 4) {
      px[i + 0] = d_color.r;
      px[i + 1] = d_color.g;
      px[i + 2] = d_color.b;
      px[i + 3] = d_color.a;
    }
    d_renders.fetch_add(1, std::memory_order_acq_rel);
    return RenderResult{request.scale, /*exact=*/true};
  }

  std::uint64_t renders() const { return d_renders.load(std::memory_order_acquire); }

private:
  Rgba d_color;
  bool d_thread_safe;
  std::atomic<std::uint64_t> d_renders{0};
};

// ONE document, driven by every viewport -- which is what makes the cross-renderer
// serialization real rather than notional: all four renderers resolve the SAME `Content*` for
// the non-thread-safe leaf, so the pool's per-content gate is the only thing standing between
// them and two concurrent `render` calls on it.
//
// LEAVES ONLY, and deliberately so. An operator is bound to a driver's live `PullService` by
// `bind_operators`, which writes that pointer INTO the shared content object -- so K drivers on
// K threads binding one operator would be K threads writing one pointer, a per-document hazard
// that has nothing to do with the pool and is not what this task changed. The pool's own
// operator discipline (an operator renders inline on the driver thread, never on a worker) is
// pinned by `worker_dispatch_leaf_only.t.cpp`, over both drivers and all three operator kinds.
struct StressScene {
  std::shared_ptr<StressLeaf> safe_a =
      std::make_shared<StressLeaf>(Rgba{0.20F, 0.55F, 0.30F, 1.0F}, /*thread_safe=*/true);
  std::shared_ptr<StressLeaf> safe_b =
      std::make_shared<StressLeaf>(Rgba{0.55F, 0.20F, 0.35F, 1.0F}, /*thread_safe=*/true);
  std::shared_ptr<StressLeaf> serialized =
      std::make_shared<StressLeaf>(Rgba{0.15F, 0.30F, 0.75F, 1.0F}, /*thread_safe=*/false);
  Document doc;

  StressScene() {
    doc.add_layer(doc.add_content(safe_a), Affine::identity());
    doc.add_layer(doc.add_content(serialized), Affine::identity());
    doc.add_layer(doc.add_content(safe_b), Affine::identity());
  }
};

// One viewport's worth of caller-owned state, over a BORROWED pool. Its own cache, its own
// surface pool, its own target -- everything that is per-viewport, per viewport.
class ViewportDriver {
public:
  ViewportDriver(Document& doc, Backend& backend, WorkerPool& pool)
      : d_doc(doc), d_cache(32U * 1024 * 1024), d_surfaces(backend), d_backend(backend),
        d_renderer(pool) {
    const DocStatePtr pin = doc.pin();
    expected<std::unique_ptr<Surface>, SurfaceError> target =
        backend.make_surface(k_dim, k_dim, pin->working_space());
    if (target.has_value()) {
      d_target = std::move(*target);
    }
    // NOT `REQUIRE`. This constructor runs on the worker threads, and Catch2's assertion
    // machinery is single-threaded -- a `REQUIRE` from N threads races Catch2's own
    // `RunContext`, which TSan reports as a data race in the harness rather than in the code
    // under test. Every assertion in this file is made on the MAIN thread, against the atomics
    // the threads accumulated into; a failed surface is one of them (`ok()`).
  }

  bool ok() const noexcept { return d_target != nullptr; }

  InteractiveRenderer::FrameOutcome frame() {
    const DocStatePtr pin = d_doc.pin();
    const ContentResolver resolve = [this](ObjectId id) { return d_doc.resolve(id); };
    const Viewport view{k_dim, k_dim, Affine::identity()};
    // No `FrameBinding`: the scene is leaf-only, so there is no operator to bind (see
    // `StressScene`), and the default binds nothing.
    return d_renderer.render_frame(*pin, resolve, view, d_cache, d_backend, d_surfaces, *d_target,
                                   {}, k_interior, k_frame_budget);
  }

  // Frame until nothing is in flight and no follow-up is owed. Bounded: a fan-out that never
  // settles must FAIL, not hang.
  bool drive_to_quiescence() {
    constexpr int k_max_frames = 64;
    for (int i = 0; i < k_max_frames; ++i) {
      const InteractiveRenderer::FrameOutcome outcome = frame();
      if (!outcome.schedule_follow_up && d_renderer.pending().tiles.empty()) {
        return true;
      }
    }
    return false;
  }

  const InteractiveRenderer& renderer() const noexcept { return d_renderer; }

private:
  Document& d_doc;
  TileCache d_cache;
  SurfacePool d_surfaces;
  Backend& d_backend;
  std::unique_ptr<Surface> d_target;
  InteractiveRenderer d_renderer;
};

// One seeded run: K threads, each cycling a viewport of its own over the shared pool --
// constructing it, driving frames, and destroying it MID-FLIGHT, over and over, on a schedule
// the seed decides. Destroying a renderer with renders outstanding is the whole point: it is
// the only way to exercise `drain_owner` against a pool a sibling is actively using.
void run_seed(std::uint32_t seed, int cycles_per_thread) {
  CpuBackend backend;
  StressScene scene;

  // A pool sized above the renderer count so the drains are genuinely concurrent with a
  // sibling's renders rather than serialized behind them by worker starvation.
  WorkerPoolConfig config;
  config.worker_count = 4;
  WorkerPool pool(config);

  std::atomic<int> quiesce_failures{0};
  std::atomic<int> surface_failures{0};
  std::atomic<std::uint64_t> expiries{0};

  {
    std::vector<std::thread> threads;
    threads.reserve(k_renderers);
    for (int t = 0; t < k_renderers; ++t) {
      threads.emplace_back([&, t] {
        Rng rng(seed + static_cast<std::uint32_t>(t) * 7919U);
        for (int c = 0; c < cycles_per_thread; ++c) {
          ViewportDriver view(scene.doc, backend, pool);
          if (!view.ok()) {
            surface_failures.fetch_add(1, std::memory_order_acq_rel);
            continue; // reported on the main thread, below
          }
          // A seeded number of frames -- SOMETIMES ZERO, sometimes one (which dispatches a
          // cold fan-out and returns with renders still on workers), sometimes enough to
          // quiesce. The one-frame case is the interesting one: the viewport is destroyed
          // with its own tasks queued behind a sibling's and its own renders in flight, which
          // is exactly the state `~InteractiveRenderer` has to unwind without touching
          // anybody else's work.
          const std::size_t frames = rng.below(3);
          for (std::size_t f = 0; f < frames; ++f) {
            view.frame();
          }
          if (rng.below(4) == 0) {
            // Occasionally let one run all the way to quiescence: a viewport that converges
            // must never have expired a deadline, because nothing in this scene blocks -- so an
            // expiry here would be a settle consumed by a sibling's park.
            if (!view.drive_to_quiescence()) {
              quiesce_failures.fetch_add(1, std::memory_order_acq_rel);
            }
            expiries.fetch_add(view.renderer().deadline_expiries(), std::memory_order_acq_rel);
          }
          // ...and here the viewport DIES -- with, more often than not, work still outstanding
          // in the pool. Its drain purges what it queued and waits out what it started, and the
          // three sibling threads keep rendering through it.
        }
      });
    }
    for (std::thread& thread : threads) {
      thread.join();
    }
  }

  // Every renderer is gone; the pool is quiescent and still alive. Nothing is outstanding, so
  // the accounting identity is exact: every admitted task either ran to a settlement or was
  // purged by a dying renderer's drain. A purge that took a sibling's task, or a drain that
  // returned while its own work was still queued, breaks this.
  CHECK(pool.tasks_submitted() == pool.tasks_completed() + pool.tasks_dropped());

  // The per-content gate, now holding ACROSS renderers: four viewports onto one document all
  // resolve the same `Content*` for the non-thread-safe leaf, and the pool never let two of its
  // renders run at once. Measured at the render call site, independently of the admission gate.
  CHECK(pool.max_in_flight_per_content() <= 1);

  // A viewport that ran to quiescence over a scene whose leaves all settle promptly cannot have
  // reached a deadline with work outstanding -- unless a sibling ate its wake.
  CHECK(quiesce_failures.load(std::memory_order_acquire) == 0);
  CHECK(surface_failures.load(std::memory_order_acquire) == 0);
  CHECK(expiries.load(std::memory_order_acquire) == 0);

  // The pool outlived every borrower and is still usable: the drains did not stop it.
  CHECK(pool.worker_count() == 4);
}

} // namespace

// enforces: 02-architecture#worker-pool-serializes-non-thread-safe-content
// enforces: 02-architecture#shared-pool-teardown-drains-only-its-own-submissions
// enforces: 02-architecture#shared-pool-park-observes-only-its-own-settles
TEST_CASE("K renderers sharing one pool across K threads, constructed and destroyed "
          "mid-flight") {
  run_seed(/*seed=*/0x5EEDU, /*cycles_per_thread=*/6);
}

// The long-form seeded sweep. Hidden (`[.nightly]`), so `catch_discover_tests` does not
// register it with ctest and the per-push lanes stay fast; nightly.yml's `tsan-full` job
// invokes this binary with the tag. One interleaving is one lucky schedule -- the per-push TSan
// lane runs the case above once; this runs the mechanism against many.
TEST_CASE("shared pool: seeded schedule sweep", "[.nightly]") {
  for (std::uint32_t seed = 1; seed <= 24; ++seed) {
    run_seed(seed * 2654435761U, /*cycles_per_thread=*/8);
  }
}
