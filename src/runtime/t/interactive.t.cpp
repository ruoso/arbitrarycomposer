#include <arbc/base/expected.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/cache/key_shapes.hpp>
#include <arbc/cache/keyed_store.hpp>
#include <arbc/compositor/compositor.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/model/damage.hpp>
#include <arbc/model/model.hpp>
#include <arbc/runtime/interactive.hpp>
#include <arbc/surface/backend.hpp>
#include <arbc/surface/surface.hpp>
#include <arbc/surface/surface_error.hpp>
#include <arbc/surface/surface_pool.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <vector>

// Unit + golden + focused-concurrency tests for the interactive frame loop
// (`arbc::InteractiveRenderer`, doc 02:49-71, 17:60). Deterministic: every clock
// read is an injected fake clock, every arrival is settled or poked explicitly,
// and synchronization is via `wait_completions` / the completion state -- no test
// reads the real wall clock or sleeps to synchronize (doc 16:54-62). The
// byte-exact goldens are computed in-process against an independent reference
// render (the repo's golden idiom); a stub backend keeps the file inside
// `runtime`'s declared dependency closure.

namespace {

using arbc::Damage;
using arbc::RenderCompletion;
using arbc::RenderRequest;
using arbc::RenderResult;
using arbc::Stability;
using arbc::TileCache;

// A CPU-buffer surface so a two-frame byte comparison is meaningful; the backend
// folds a deterministic marker into it per composite.
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

// A backend over real-buffer surfaces whose composite folds a deterministic
// function of the source's first byte and the opacity into every destination
// byte, so an identical composite sequence reproduces identical bytes and a
// dropped composite would not. Counts nothing itself.
class MarkBackend : public arbc::Backend {
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
  void composite(arbc::Surface& dst, const arbc::Surface& src, const arbc::Affine& /*m*/,
                 double opacity) override {
    const std::span<const std::byte> s = src.cpu_bytes();
    const unsigned seed = s.empty() ? 0u : std::to_integer<unsigned>(s[0]);
    const auto mark = (static_cast<unsigned>(opacity * 251.0) + 1u + seed) & 0xFFu;
    for (std::byte& b : dst.cpu_bytes()) {
      b = static_cast<std::byte>((std::to_integer<unsigned>(b) + mark) & 0xFFu);
    }
  }
  void downsample(arbc::Surface& /*dst*/, const arbc::Surface& /*src*/) override {}
};

// A deterministic non-zero fill, so a composited tile is distinguishable
// byte-for-byte from an uncomposited (transparent) placeholder.
void fill_solid(arbc::Surface& target) {
  const std::span<std::byte> bytes = target.cpu_bytes();
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<std::byte>((i * 31u + 7u) & 0xFFu);
  }
}

// Content that answers synchronously with an exact, at-scale result and fills the
// tile deterministically. `stability` is configurable so a scene can be Static
// (clock-invariant) or Timed (damaged by a clock advance).
class SyncSolid : public arbc::Content {
public:
  explicit SyncSolid(Stability stability = Stability::Static) : d_stability(stability) {}
  std::optional<arbc::Rect> bounds() const override { return arbc::Rect{0.0, 0.0, 512.0, 512.0}; }
  Stability stability() const override { return d_stability; }
  std::optional<arbc::TimeRange> time_extent() const override {
    return d_stability == Stability::Static
               ? std::optional<arbc::TimeRange>(std::nullopt)
               : std::optional<arbc::TimeRange>(arbc::TimeRange::all());
  }
  std::optional<RenderResult> render(const RenderRequest& request,
                                     std::shared_ptr<RenderCompletion> /*done*/) override {
    fill_solid(request.target);
    return RenderResult{request.scale, /*exact=*/true};
  }

private:
  Stability d_stability;
};

// Content that answers "asynchronously" but pre-settles inline: it fills the tile
// and completes `done` before returning nullopt, so the driver records it pending
// already-settled and the very next poll reaps it with no off-thread wake -- a
// deterministic stand-in for "the async result arrived within the frame budget".
class AsyncPresettleSolid : public arbc::Content {
public:
  std::optional<arbc::Rect> bounds() const override { return arbc::Rect{0.0, 0.0, 512.0, 512.0}; }
  Stability stability() const override { return Stability::Static; }
  std::optional<arbc::TimeRange> time_extent() const override { return std::nullopt; }
  std::optional<RenderResult> render(const RenderRequest& request,
                                     std::shared_ptr<RenderCompletion> done) override {
    fill_solid(request.target);
    done->complete(RenderResult{request.scale, /*exact=*/true});
    return std::nullopt;
  }
};

// Content that answers asynchronously and never settles: the recorded pending
// render stays in flight past the frame deadline, so the loop must cancel it.
class AsyncNever : public arbc::Content {
public:
  std::optional<arbc::Rect> bounds() const override { return arbc::Rect{0.0, 0.0, 512.0, 512.0}; }
  Stability stability() const override { return Stability::Static; }
  std::optional<arbc::TimeRange> time_extent() const override { return std::nullopt; }
  std::optional<RenderResult> render(const RenderRequest& /*request*/,
                                     std::shared_ptr<RenderCompletion> /*done*/) override {
    return std::nullopt;
  }
};

// Content that answers asynchronously by capturing `done` (and filling the tile)
// for an off-thread settler to complete + poke -- the concurrency reap/wake case.
class AsyncCapture : public arbc::Content {
public:
  std::optional<arbc::Rect> bounds() const override { return arbc::Rect{0.0, 0.0, 512.0, 512.0}; }
  Stability stability() const override { return Stability::Timed; }
  std::optional<arbc::TimeRange> time_extent() const override { return arbc::TimeRange::all(); }
  std::optional<RenderResult> render(const RenderRequest& request,
                                     std::shared_ptr<RenderCompletion> done) override {
    fill_solid(request.target);
    {
      const std::lock_guard<std::mutex> lock(d_mutex);
      d_done = std::move(done);
      d_result = RenderResult{request.scale, /*exact=*/true};
    }
    d_ready.store(true, std::memory_order_release);
    return std::nullopt;
  }

  // Settler side: complete a captured completion, if any. Returns whether it
  // settled one (the settler then pokes the pool). `d_done` is published/consumed
  // under the lock, gated by the release/acquire `d_ready` flag -- no torn read.
  bool settle_pending() {
    if (!d_ready.load(std::memory_order_acquire)) {
      return false;
    }
    std::shared_ptr<RenderCompletion> done;
    RenderResult result;
    {
      const std::lock_guard<std::mutex> lock(d_mutex);
      done = std::move(d_done);
      result = d_result;
    }
    d_ready.store(false, std::memory_order_release);
    if (done) {
      done->complete(result);
      return true;
    }
    return false;
  }

private:
  std::mutex d_mutex;
  std::atomic<bool> d_ready{false};
  std::shared_ptr<RenderCompletion> d_done;
  RenderResult d_result{};
};

struct Scene {
  arbc::ObjectId content;
  arbc::ObjectId layer;
};

// A single identity-placed layer bound to `content`; returns the ids.
Scene add_single_layer(arbc::Model& model) {
  auto txn = model.transact();
  const arbc::ObjectId content = txn.add_content(0);
  const arbc::ObjectId layer = txn.add_layer(content, arbc::Affine::identity());
  REQUIRE(txn.commit().has_value());
  return {content, layer};
}

// A trivial commit (re-set the same transform) that publishes a fresh revision
// without moving the layer -- the revision bump the stale probe keys off.
void bump_revision(arbc::Model& model, arbc::ObjectId layer) {
  auto txn = model.transact();
  txn.set_transform(layer, arbc::Affine::identity());
  REQUIRE(txn.commit().has_value());
}

bool bytes_identical(std::span<const std::byte> a, std::span<const std::byte> b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size_bytes()) == 0;
}

std::vector<std::byte> snapshot(const arbc::Surface& surface) {
  const std::span<const std::byte> b = surface.cpu_bytes();
  return {b.begin(), b.end()};
}

// The loop's only clock is injected; a fake clock fixed at the steady_clock epoch
// keeps every deadline instant (`epoch + budget`) in the real past, so
// `wait_completions` never blocks and the deadline path is deterministic.
arbc::InteractiveRenderer::Clock epoch_clock() {
  return [] { return std::chrono::steady_clock::time_point{}; };
}

constexpr auto k_budget = std::chrono::milliseconds(16);

} // namespace

// enforces: 02-architecture#interactive-still-scene-schedules-no-frame
// enforces: 11-time-and-video#clock-advance-damages-only-moving-layers
// enforces: 02-architecture#quiescent-refinement-schedules-no-frame
TEST_CASE("interactive: a still scene advancing only the clock does no work") {
  MarkBackend backend;
  SyncSolid content(Stability::Static);
  arbc::Model model;
  const Scene scene = add_single_layer(model);
  const arbc::DocStatePtr state = model.current();
  const auto resolver = [&](arbc::ObjectId id) -> arbc::Content* {
    return id == scene.content ? &content : nullptr;
  };
  const arbc::Viewport viewport{256, 256, arbc::Affine::identity()}; // one rung-0 tile
  arbc::SurfacePool pool(backend);
  TileCache cache(64u * 1024 * 1024);
  auto target = backend.make_surface(256, 256, arbc::k_working_rgba32f);
  REQUIRE(target.has_value());

  arbc::InteractiveRenderer renderer({}, epoch_clock());

  // Frame 1 warms the cache (first frame -> whole viewport).
  renderer.render_frame(*state, resolver, viewport, cache, backend, pool, **target, {},
                        arbc::Time{0}, k_budget);
  CHECK(renderer.counters().requests_issued() == 1);
  CHECK(renderer.counters().composites() == 1);
  const std::vector<std::byte> after_frame1 = snapshot(**target);

  // Frame 2: advance only the clock over the all-Static scene, no model damage.
  // Collected damage is empty and the queue is empty -> zero work, no frame.
  const auto out2 = renderer.render_frame(*state, resolver, viewport, cache, backend, pool,
                                          **target, {}, arbc::Time{1'000'000}, k_budget);
  CHECK_FALSE(out2.schedule_follow_up);
  CHECK(renderer.counters().requests_issued() == 1); // delta 0
  CHECK(renderer.counters().composites() == 1);      // delta 0
  CHECK(renderer.counters().follow_up_frames() == 0);
  // The zero-work frame does not touch (does not clear) the persisted target.
  CHECK(bytes_identical(after_frame1, snapshot(**target)));
}

// enforces: 02-architecture#interactive-frame-loop-bounded-by-deadline
TEST_CASE("interactive: the frame never blocks past its deadline") {
  MarkBackend backend;
  arbc::Model model;
  const Scene scene = add_single_layer(model);
  const arbc::DocStatePtr state = model.current();
  const arbc::Viewport viewport{256, 256, arbc::Affine::identity()};
  arbc::SurfacePool pool(backend);

  SECTION("an async miss that never settles is cancelled, and no frame is scheduled") {
    AsyncNever content;
    const auto resolver = [&](arbc::ObjectId id) -> arbc::Content* {
      return id == scene.content ? &content : nullptr;
    };
    TileCache cache(64u * 1024 * 1024);
    auto target = backend.make_surface(256, 256, arbc::k_working_rgba32f);
    REQUIRE(target.has_value());
    arbc::InteractiveRenderer renderer({}, epoch_clock());

    const auto out = renderer.render_frame(*state, resolver, viewport, cache, backend, pool,
                                           **target, {}, arbc::Time{0}, k_budget);
    // Did not block, composited its best (placeholder) fallback, scheduled nothing.
    CHECK_FALSE(out.schedule_follow_up);
    CHECK(renderer.counters().requests_issued() == 1);
    CHECK(renderer.counters().composites() == 0); // transparent placeholder
    CHECK(renderer.counters().follow_up_frames() == 0);
    // The expired BestEffort pending render was cancelled (advisory).
    REQUIRE(renderer.pending().tiles.size() == 1);
    CHECK(renderer.pending().tiles.front().done->cancelled());
  }

  SECTION("an async miss that settled within budget is reaped and schedules a frame") {
    AsyncPresettleSolid content;
    const auto resolver = [&](arbc::ObjectId id) -> arbc::Content* {
      return id == scene.content ? &content : nullptr;
    };
    TileCache cache(64u * 1024 * 1024);
    auto target = backend.make_surface(256, 256, arbc::k_working_rgba32f);
    REQUIRE(target.has_value());
    arbc::InteractiveRenderer renderer({}, epoch_clock());

    const auto out = renderer.render_frame(*state, resolver, viewport, cache, backend, pool,
                                           **target, {}, arbc::Time{0}, k_budget);
    CHECK(out.schedule_follow_up);
    CHECK(renderer.counters().requests_issued() == 1);
    CHECK(renderer.counters().follow_up_frames() == 1); // reaped + inserted
    CHECK(renderer.pending().tiles.empty());
  }
}

// enforces: 02-architecture#interactive-frame-loop-bounded-by-deadline
// enforces: 02-architecture#degraded-fallback-preference-order
// enforces: 02-architecture#async-arrival-emits-damage
TEST_CASE("interactive: a coarse fallback frame refines to a sharp frame (byte-exact golden)") {
  MarkBackend backend;
  arbc::Model model;
  const Scene scene = add_single_layer(model);
  const arbc::DocStatePtr state = model.current();
  const arbc::Viewport viewport{256, 256, arbc::Affine::identity()};

  // The golden run: frame 1 answers the miss async (composites the transparent
  // placeholder fallback), the arrival settles within budget, and frame 2
  // re-plans the now-resident tile Fresh and composites it sharp.
  AsyncPresettleSolid async_content;
  const auto async_resolver = [&](arbc::ObjectId id) -> arbc::Content* {
    return id == scene.content ? &async_content : nullptr;
  };
  arbc::SurfacePool pool(backend);
  TileCache cache(64u * 1024 * 1024);
  auto target = backend.make_surface(256, 256, arbc::k_working_rgba32f);
  REQUIRE(target.has_value());
  arbc::InteractiveRenderer renderer({}, epoch_clock());

  const auto out1 = renderer.render_frame(*state, async_resolver, viewport, cache, backend, pool,
                                          **target, {}, arbc::Time{0}, k_budget);
  const std::vector<std::byte> coarse = snapshot(**target); // placeholder fallback
  CHECK(out1.schedule_follow_up);                           // the arrival owes a follow-up frame

  const auto out2 = renderer.render_frame(*state, async_resolver, viewport, cache, backend, pool,
                                          **target, {}, arbc::Time{0}, k_budget);
  const std::vector<std::byte> sharp = snapshot(**target);
  (void)out2;

  // The reference: an independent, fully synchronous single-frame render of the
  // same scene -- the sharp result the refinement must converge to.
  MarkBackend ref_backend;
  SyncSolid ref_content(Stability::Static);
  const auto ref_resolver = [&](arbc::ObjectId id) -> arbc::Content* {
    return id == scene.content ? &ref_content : nullptr;
  };
  arbc::SurfacePool ref_pool(ref_backend);
  TileCache ref_cache(64u * 1024 * 1024);
  auto reference = ref_backend.make_surface(256, 256, arbc::k_working_rgba32f);
  REQUIRE(reference.has_value());
  arbc::InteractiveRenderer ref_renderer({}, epoch_clock());
  ref_renderer.render_frame(*state, ref_resolver, viewport, ref_cache, ref_backend, ref_pool,
                            **reference, {}, arbc::Time{0}, k_budget);

  // Coarse-then-refine: frame 1 differs from frame 2, and frame 2 is byte-exact
  // to the fully-sharp reference.
  CHECK_FALSE(bytes_identical(coarse, sharp));
  CHECK(bytes_identical(sharp, snapshot(**reference)));
}

// enforces: 02-architecture#damage-maps-to-device-dirty-regions
TEST_CASE("interactive: a damage-gated frame re-plans only the changed tiles") {
  MarkBackend backend;
  SyncSolid content(Stability::Static);
  arbc::Model model;
  const Scene scene = add_single_layer(model);
  const arbc::DocStatePtr state = model.current();
  const auto resolver = [&](arbc::ObjectId id) -> arbc::Content* {
    return id == scene.content ? &content : nullptr;
  };
  const arbc::Viewport viewport{512, 512, arbc::Affine::identity()}; // a 2x2 grid of rung-0 tiles
  arbc::SurfacePool pool(backend);
  TileCache cache(64u * 1024 * 1024);
  auto target = backend.make_surface(512, 512, arbc::k_working_rgba32f);
  REQUIRE(target.has_value());
  arbc::InteractiveRenderer renderer({}, epoch_clock());

  // Frame 1: cold cache, whole viewport -> four misses, four composites.
  renderer.render_frame(*state, resolver, viewport, cache, backend, pool, **target, {},
                        arbc::Time{0}, k_budget);
  CHECK(renderer.counters().requests_issued() == 4);
  CHECK(renderer.counters().composites() == 4);

  // Frame 2: model damage confined to the top-left tile's content region. Only
  // that tile is invalidated, re-planned, and re-composited; the other three stay
  // cache hits.
  const std::vector<Damage> damage{Damage{scene.content, arbc::Rect{0.0, 0.0, 256.0, 256.0}, {}}};
  renderer.render_frame(*state, resolver, viewport, cache, backend, pool, **target, damage,
                        arbc::Time{0}, k_budget);
  CHECK(renderer.counters().requests_issued() == 5); // + exactly one
  CHECK(renderer.counters().composites() == 5);      // + exactly one
}

// enforces: 02-architecture#degraded-fallback-preference-order
TEST_CASE("interactive: frame-to-frame state advances across frames") {
  MarkBackend backend;
  arbc::Model model;
  const Scene scene = add_single_layer(model);
  const arbc::DocStatePtr state = model.current();
  const arbc::Viewport viewport{256, 256, arbc::Affine::identity()};
  arbc::SurfacePool pool(backend);

  SECTION("prior revision and previous time track the last frame") {
    SyncSolid content(Stability::Static);
    const auto resolver = [&](arbc::ObjectId id) -> arbc::Content* {
      return id == scene.content ? &content : nullptr;
    };
    TileCache cache(64u * 1024 * 1024);
    auto target = backend.make_surface(256, 256, arbc::k_working_rgba32f);
    REQUIRE(target.has_value());
    arbc::InteractiveRenderer renderer({}, epoch_clock());

    renderer.render_frame(*state, resolver, viewport, cache, backend, pool, **target, {},
                          arbc::Time{0}, k_budget);
    CHECK(renderer.prior_revision() == state->revision());
    CHECK(renderer.previous_time() == arbc::Time{0});

    // A later (no-work) frame still advances the previous composition time, so the
    // next clock advance computes the correct TimeRange.
    renderer.render_frame(*state, resolver, viewport, cache, backend, pool, **target, {},
                          arbc::Time{5'000'000}, k_budget);
    CHECK(renderer.previous_time() == arbc::Time{5'000'000});
  }

  SECTION("a clock advance re-renders a moving (Timed) layer via the previous time") {
    SyncSolid content(Stability::Timed);
    const auto resolver = [&](arbc::ObjectId id) -> arbc::Content* {
      return id == scene.content ? &content : nullptr;
    };
    TileCache cache(64u * 1024 * 1024);
    auto target = backend.make_surface(256, 256, arbc::k_working_rgba32f);
    REQUIRE(target.has_value());
    arbc::InteractiveRenderer renderer({}, epoch_clock());

    renderer.render_frame(*state, resolver, viewport, cache, backend, pool, **target, {},
                          arbc::Time{0}, k_budget);
    CHECK(renderer.counters().requests_issued() == 1);

    // The advance range {previous_time, now} = {0, 1e6} is non-empty, so the Timed
    // layer is damaged and re-rendered -- proving `previous_time` drove the range
    // (a stuck-at-nullopt previous time would collapse it to empty and do no work).
    renderer.render_frame(*state, resolver, viewport, cache, backend, pool, **target, {},
                          arbc::Time{1'000'000}, k_budget);
    CHECK(renderer.counters().requests_issued() == 2); // + one moving-layer render
    CHECK(renderer.previous_time() == arbc::Time{1'000'000});
  }

  SECTION("a revision bump surfaces the prior-revision tile as the stale fallback") {
    // Frame 1 at revision R answers async and pre-settles, so its poll caches the
    // tile Fresh at R and produces the arrival damage carried into frame 2.
    AsyncPresettleSolid r_content;
    const auto r_resolver = [&](arbc::ObjectId id) -> arbc::Content* {
      return id == scene.content ? &r_content : nullptr;
    };
    TileCache cache(64u * 1024 * 1024);
    auto target = backend.make_surface(256, 256, arbc::k_working_rgba32f);
    REQUIRE(target.has_value());
    arbc::InteractiveRenderer renderer({}, epoch_clock());

    const arbc::DocStatePtr state_r = model.current();
    renderer.render_frame(*state_r, r_resolver, viewport, cache, backend, pool, **target, {},
                          arbc::Time{0}, k_budget);
    REQUIRE(renderer.prior_revision() == state_r->revision());

    // Bump the revision without moving the layer and without invalidating the
    // cache. Frame 2 renders async-never, so the fresh (R+1) key stays a miss and
    // the prior-revision (R) tile -- reached by the stale probe that `prior_revision`
    // enables -- is composited as the fallback, not a transparent placeholder.
    bump_revision(model, scene.layer);
    const arbc::DocStatePtr state_r1 = model.current();
    REQUIRE(state_r1->revision() == state_r->revision() + 1);

    AsyncNever r1_content;
    const auto r1_resolver = [&](arbc::ObjectId id) -> arbc::Content* {
      return id == scene.content ? &r1_content : nullptr;
    };
    const std::uint64_t requests_before = renderer.counters().requests_issued();
    const std::uint64_t composites_before = renderer.counters().composites();
    renderer.render_frame(*state_r1, r1_resolver, viewport, cache, backend, pool, **target, {},
                          arbc::Time{0}, k_budget);
    CHECK(renderer.counters().requests_issued() - requests_before == 1); // the R+1 async miss
    CHECK(renderer.counters().composites() - composites_before == 1);    // the stale R fallback
  }
}

// enforces: 02-architecture#interactive-frame-loop-bounded-by-deadline
TEST_CASE("interactive: an off-thread arrival is reaped and scheduled without blocking") {
  MarkBackend backend;
  AsyncCapture content; // Timed; captures each frame's completion for an off-thread settle
  arbc::Model model;
  const Scene scene = add_single_layer(model);
  const arbc::DocStatePtr state = model.current();
  const auto resolver = [&](arbc::ObjectId id) -> arbc::Content* {
    return id == scene.content ? &content : nullptr;
  };
  const arbc::Viewport viewport{256, 256, arbc::Affine::identity()};
  arbc::SurfacePool pool(backend);
  TileCache cache(64u * 1024 * 1024);
  auto target = backend.make_surface(256, 256, arbc::k_working_rgba32f);
  REQUIRE(target.has_value());

  // A real worker pool + the default (real) clock with an absurd budget, so the
  // render thread parks in `wait_completions` until the settler pokes, never the
  // timeout. Synchronization is on the completion, not on time.
  arbc::WorkerPoolConfig pool_config;
  pool_config.worker_count = 2;
  arbc::InteractiveRenderer renderer(pool_config);

  std::atomic<bool> stop{false};
  std::thread settler([&] {
    while (!stop.load(std::memory_order_acquire)) {
      if (content.settle_pending()) {
        renderer.worker_pool().poke();
      } else {
        std::this_thread::yield();
      }
    }
    if (content.settle_pending()) { // drain the last captured completion, if any
      renderer.worker_pool().poke();
    }
  });

  constexpr int k_frames = 8;
  constexpr auto k_big_budget = std::chrono::hours(1);
  int scheduled = 0;
  for (int i = 0; i < k_frames; ++i) {
    // Advance the media clock each frame so the Timed layer is damaged and
    // re-planned to a fresh miss the settler answers off-thread.
    const arbc::Time when{static_cast<std::int64_t>(i + 1) * 1'000'000};
    const auto out = renderer.render_frame(*state, resolver, viewport, cache, backend, pool,
                                           **target, {}, when, k_big_budget);
    if (out.schedule_follow_up) {
      ++scheduled;
    }
  }
  stop.store(true, std::memory_order_release);
  settler.join();

  // Every frame's miss settled before its deadline, was reaped exactly once, and
  // scheduled a follow-up; nothing was left pending; no crash or hang.
  CHECK(scheduled == k_frames);
  CHECK(renderer.counters().requests_issued() == static_cast<std::uint64_t>(k_frames));
  CHECK(renderer.counters().follow_up_frames() == static_cast<std::uint64_t>(k_frames));
  CHECK(renderer.pending().tiles.empty());
}
