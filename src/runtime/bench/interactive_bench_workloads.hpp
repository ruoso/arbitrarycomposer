#pragma once

// Reusable benchmark workloads for the interactive frame loop
// (`runtime.interactive_worker_count_default`, doc 02:49-71).
//
// The workload BODIES live here, free of any harness dependency, so two very
// different drivers can share them (the idiom `src/pool/bench/pool_bench_workloads.hpp`
// established, and `cmake/ArbcComponent.cmake:58-67` writes down):
//   * src/runtime/bench/interactive_worker_bench.cpp wraps each in a Google Benchmark
//     `for (auto _ : state)` sweep over the worker counts (built only under
//     ARBC_BENCHMARKS), and
//   * src/runtime/t/bench_smoke.t.cpp drives each once at a minimal size under the
//     normal dev/asan test build, asserting the BEHAVIORAL facts (every scene really
//     renders, the counters are non-vacuous, the loop reaches quiescence), so the
//     benchmark code carries diff coverage and rot protection without any wall-clock
//     assertion entering the merge path (doc 16:82-87, 225-226).
//
// Three scenes, because the worker-count question has three different answers and the
// task has to see all of them (A6):
//   * LEAF-HEAVY -- many independent leaf contents tiled across the viewport. Every
//     miss is a leaf, so this is the scene the fan-out is FOR.
//   * OPERATOR-HEAVY -- fades and crossfades over leaves. Every operator render is
//     inline by the leaf-only rule (doc 02 § Threading model), so this scene measures
//     that rule's CEILING: only the operators' leaf inputs can fan out.
//   * NESTED-DEEP -- a chain of nested compositions. The deepest inline descent, and
//     the case where a worker's arrival has furthest to travel back up the graph.
//
// LEVELIZATION (doc 17): everything here is inside `runtime`'s declared dependency
// closure -- the concrete kinds (kind_solid/fade/crossfade/nested) are named by
// `src/runtime/CMakeLists.txt`'s DEPENDS, and the backend is a `surface`-level
// `arbc::testing::StubBackend` double rather than `arbc::CpuBackend`, which is NOT in
// that closure. The same reason `src/runtime/t/interactive.t.cpp` uses a stub.

#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/cache/keyed_store.hpp>
#include <arbc/compositor/compositor.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/kind_crossfade/crossfade_content.hpp>
#include <arbc/kind_fade/fade_content.hpp>
#include <arbc/kind_nested/nested_content.hpp>
#include <arbc/kind_solid/solid_content.hpp> // Rgba
#include <arbc/media/pixel_format.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/model/damage.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/interactive.hpp>
#include <arbc/runtime/worker_pool.hpp>
#include <arbc/surface/surface.hpp>
#include <arbc/surface/surface_pool.hpp>
#include <arbc/surface/testing/stub_backend.hpp>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace arbc::runtime_bench {

// --- The backend -------------------------------------------------------------

// A CPU-buffer surface in the working rgba32f format.
class BufferSurface final : public Surface {
public:
  BufferSurface(int width, int height)
      : d_width(width), d_height(height),
        d_bytes(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 16,
                std::byte{0}) {}

  int width() const override { return d_width; }
  int height() const override { return d_height; }
  SurfaceFormat format() const override { return k_working_rgba32f; }
  std::span<std::byte> cpu_bytes() override { return d_bytes; }
  std::span<const std::byte> cpu_bytes() const override { return d_bytes; }

private:
  int d_width;
  int d_height;
  std::vector<std::byte> d_bytes;
};

// A backend that really composites: nearest-sampled premultiplied source-over through
// the inverse of the source-to-destination affine. It is not `CpuBackend` (which lives
// outside `runtime`'s dependency closure, see the header note) and it is not a pixel
// ORACLE -- nothing here asserts a color. It exists so an operator render costs what an
// operator render costs, rather than being a no-op the benchmark then reports as fast.
class BenchBackend final : public arbc::testing::StubBackend {
public:
  BackendCaps capabilities() const override { return {}; }

  expected<std::unique_ptr<Surface>, SurfaceError> make_surface(int width, int height,
                                                                SurfaceFormat /*format*/) override {
    return std::unique_ptr<Surface>(std::make_unique<BufferSurface>(width, height));
  }

  void clear(Surface& surface, float r, float g, float b, float a) override {
    const std::span<float> px = surface.span<PixelFormat::Rgba32fLinearPremul>();
    for (std::size_t i = 0; i + 3 < px.size(); i += 4) {
      px[i + 0] = r;
      px[i + 1] = g;
      px[i + 2] = b;
      px[i + 3] = a;
    }
  }

  void composite(Surface& dst, const Surface& src, const Affine& src_to_dst,
                 double opacity) override {
    const std::optional<Affine> inv = src_to_dst.inverse();
    if (!inv.has_value()) {
      return; // a degenerate placement contributes nothing (doc 04:116-117)
    }
    // Both spans are non-empty by construction: every surface this backend hands out is a
    // `BufferSurface`, and `BufferSurface::format()` is always `k_working_rgba32f`, so the
    // checked typed access never degrades to the empty-span mismatch path.
    const std::span<float> d = dst.span<PixelFormat::Rgba32fLinearPremul>();
    const std::span<const float> s = std::as_const(src).span<PixelFormat::Rgba32fLinearPremul>();
    const int sw = src.width();
    const int sh = src.height();
    const auto w = static_cast<float>(opacity);
    for (int y = 0; y < dst.height(); ++y) {
      for (int x = 0; x < dst.width(); ++x) {
        const Vec2 p = inv->apply(Vec2{static_cast<double>(x) + 0.5, static_cast<double>(y) + 0.5});
        const auto sx = static_cast<int>(std::floor(p.x));
        const auto sy = static_cast<int>(std::floor(p.y));
        if (sx < 0 || sy < 0 || sx >= sw || sy >= sh) {
          continue;
        }
        const std::size_t si = (static_cast<std::size_t>(sy) * static_cast<std::size_t>(sw) +
                                static_cast<std::size_t>(sx)) *
                               4;
        const std::size_t di =
            (static_cast<std::size_t>(y) * static_cast<std::size_t>(dst.width()) +
             static_cast<std::size_t>(x)) *
            4;
        const float sa = s[si + 3] * w;
        for (std::size_t c = 0; c < 4; ++c) {
          d[di + c] = s[si + c] * w + d[di + c] * (1.0F - sa);
        }
      }
    }
  }
};

// --- The leaf ----------------------------------------------------------------

// A LEAF (no inputs, so `worker_backed_dispatch` may fan it out) whose `render` costs a
// real, tunable amount of per-pixel arithmetic. A `SolidContent` fill would be pure
// memory bandwidth, which threads badly and would make the benchmark a measurement of
// the allocator rather than of the frame loop; a procedural gradient is what a real leaf
// (a decoded frame, a generated texture) actually looks like from the pool's side.
// Deterministic -- the pixels are a pure function of the request -- so a frame is
// byte-identical whichever thread paints it, which is what makes the fan-out sound.
class PaintedLeaf final : public Content {
public:
  PaintedLeaf(Rgba tint, Rect bounds, int work)
      : d_tint(tint), d_bounds(bounds), d_work(work < 1 ? 1 : work) {}

  std::optional<Rect> bounds() const override { return d_bounds; }
  Stability stability() const override { return Stability::Static; }
  std::optional<TimeRange> time_extent() const override { return std::nullopt; }
  bool render_thread_safe() const override { return true; }

  std::optional<RenderResult> render(const RenderRequest& request,
                                     std::shared_ptr<RenderCompletion> /*done*/) override {
    const std::span<float> px = request.target.span<PixelFormat::Rgba32fLinearPremul>();
    for (std::size_t i = 0; i + 3 < px.size(); i += 4) {
      // `d_work` passes of a cheap flop chain over the pixel index: the per-pixel cost
      // knob. No transcendentals, so the cost is stable across libm implementations.
      float v = static_cast<float>(i & 0xFFu) / 255.0F;
      for (int k = 0; k < d_work; ++k) {
        v = v * 0.5F + 0.25F * (1.0F - v * v);
      }
      px[i + 0] = d_tint.r * v;
      px[i + 1] = d_tint.g * v;
      px[i + 2] = d_tint.b * v;
      px[i + 3] = d_tint.a;
    }
    return RenderResult{request.scale, /*exact=*/true};
  }

private:
  Rgba d_tint;
  Rect d_bounds;
  int d_work;
};

// --- The scenes ---------------------------------------------------------------

enum class SceneKind { LeafHeavy, OperatorHeavy, NestedDeep };

inline const char* scene_name(SceneKind kind) {
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

// The instant every frame renders at. Interior for both operator kinds below (the fade's
// envelope is 0.5, the crossfade's w is 0.5), so NEITHER short-circuits to an identity
// endpoint and each really runs its own `render` -- an endpoint scene would measure the
// driver's delivery path and call it an operator benchmark.
inline constexpr Time k_scene_time{500};

inline FadeParams half_fade() {
  return FadeParams{FadeShape::Linear, std::nullopt, FadeWindow{Time{0}, Time{1000}}};
}
inline CrossfadeParams half_crossfade() {
  return CrossfadeParams{CrossfadeShape::Linear, Time{0}, Time{1000}};
}

// One scene: a `Document`, the contents it borrows, and the whole-scene model damage a
// pass re-drives it with. `Document` is non-movable and the operators hold their inputs
// non-owningly (`ContentRef` is a raw `Content*`), so every content is owned HERE and
// outlives the document.
class BenchScene {
public:
  // `size` scales the scene: the number of leaf strips (LeafHeavy), the number of
  // operator layers (OperatorHeavy), or the nesting depth (NestedDeep). `work` is the
  // leaf's per-pixel cost knob.
  BenchScene(SceneKind kind, int dim, int size, int work) : d_kind(kind), d_dim(dim) {
    switch (kind) {
    case SceneKind::LeafHeavy:
      build_leaf_heavy(size, work);
      break;
    case SceneKind::OperatorHeavy:
      build_operator_heavy(size, work);
      break;
    case SceneKind::NestedDeep:
      build_nested_deep(size, work);
      break;
    }
  }

  Document& document() noexcept { return d_doc; }
  SceneKind kind() const noexcept { return d_kind; }
  int dim() const noexcept { return d_dim; }

  // The root composition whose direct members the frame walk draws. The frame walk
  // is composition-scoped (compositor.root_composition_frame_walk, doc 05:28-36), so a
  // caller driving `InteractiveRenderer::render_frame` directly anchors its `Viewport`
  // here; a nested chain's inner compositions are reached through content recursion.
  ObjectId root() const noexcept { return d_root; }

  // Infinite damage on every LEAF content -- the source of the scene's pixels, and the
  // only damage that reaches a worker. Damaging the operator LAYERS instead would drop
  // the operators' output tiles while leaving their inputs warm, so nothing would be
  // dispatched at all and the sweep would measure an empty pool. `route_model_damage`
  // folds each leaf record up to the operator layers that consume it and emits the twin
  // under the leaf's PULL identity (the key its shared input tiles cache under, doc
  // 13:145-149), so this one set invalidates the whole chain.
  std::vector<Damage> whole_scene_damage() const {
    std::vector<Damage> damage;
    damage.reserve(d_leaf_ids.size());
    for (const ObjectId id : d_leaf_ids) {
      damage.push_back(Damage{id, Rect::infinite(), TimeRange::all()});
    }
    return damage;
  }

  std::size_t leaf_count() const noexcept { return d_leaves.size(); }

private:
  Rect canvas() const {
    return Rect{0.0, 0.0, static_cast<double>(d_dim), static_cast<double>(d_dim)};
  }

  // Add a leaf to the contents table (so it carries the model id the damage set names)
  // and keep it alive here. Returns its id and its pointer -- an operator borrows the
  // pointer, a plain layer places the id.
  std::pair<ObjectId, PaintedLeaf*> add_leaf(Rect bounds, int work, int seed) {
    const auto f = static_cast<float>(seed % 7) / 7.0F;
    auto leaf = std::make_shared<PaintedLeaf>(Rgba{0.2F + 0.6F * f, 0.5F, 0.8F - 0.5F * f, 1.0F},
                                              bounds, work);
    const ObjectId id = d_doc.add_content(leaf);
    d_leaf_ids.push_back(id);
    d_leaves.push_back(leaf);
    return {id, leaf.get()};
  }

  // `size` leaf contents, each bounded to its own vertical strip of the viewport, all
  // placed at identity: `size` independent leaf layers whose tiles are disjoint, so the
  // pool sees `size`-ish concurrent, non-serialized renders with nothing to share.
  void build_leaf_heavy(int size, int work) {
    d_root = d_doc.add_composition(static_cast<double>(d_dim), static_cast<double>(d_dim));
    const double strip = static_cast<double>(d_dim) / static_cast<double>(size);
    for (int i = 0; i < size; ++i) {
      const Rect bounds{static_cast<double>(i) * strip, 0.0, strip, static_cast<double>(d_dim)};
      const auto [id, leaf] = add_leaf(bounds, work, i);
      (void)leaf;
      d_doc.attach_layer(d_root, d_doc.add_layer(id, Affine::identity()));
    }
  }

  // `size` operator layers alternating fade-over-a-leaf and crossfade-over-two-leaves,
  // all full-canvas and all at an interior weight. Every operator render is inline; only
  // its leaf inputs can reach a worker (the leaf-only rule's ceiling).
  void build_operator_heavy(int size, int work) {
    d_root = d_doc.add_composition(static_cast<double>(d_dim), static_cast<double>(d_dim));
    for (int i = 0; i < size; ++i) {
      if (i % 2 == 0) {
        auto fade = std::make_shared<FadeContent>(add_leaf(canvas(), work, i).second, half_fade());
        d_operators.push_back(fade);
        d_doc.attach_layer(d_root, d_doc.add_layer(d_doc.add_content(fade), Affine::identity()));
      } else {
        PaintedLeaf* const from = add_leaf(canvas(), work, i).second;
        PaintedLeaf* const to = add_leaf(canvas(), work, i + 3).second;
        auto xf = std::make_shared<CrossfadeContent>(from, to, half_crossfade());
        d_operators.push_back(xf);
        d_doc.attach_layer(d_root, d_doc.add_layer(d_doc.add_content(xf), Affine::identity()));
      }
    }
  }

  // A chain of `size` nested compositions, innermost first: each level holds a fade over
  // its own leaf plus (above the innermost) the nesting of the level below. The deepest
  // inline descent the loop can be asked for -- every level's `render` re-enters the
  // pull service on the frame thread, and only the leaves at the bottom fan out.
  void build_nested_deep(int size, int work) {
    // The root composition the frame draws: it holds the single outermost nested layer,
    // and the whole chain below is reached through that layer's content recursion.
    d_root = d_doc.add_composition(static_cast<double>(d_dim), static_cast<double>(d_dim));
    std::shared_ptr<NestedContent> inner;
    for (int level = 0; level < size; ++level) {
      const ObjectId comp =
          d_doc.add_composition(static_cast<double>(d_dim), static_cast<double>(d_dim));
      auto fade =
          std::make_shared<FadeContent>(add_leaf(canvas(), work, level).second, half_fade());
      d_operators.push_back(fade);
      d_doc.attach_layer(comp, d_doc.add_layer(d_doc.add_content(fade), Affine::identity()));
      if (inner) {
        d_doc.attach_layer(comp, d_doc.add_layer(d_doc.add_content(inner), Affine::identity()));
      }
      inner = std::make_shared<NestedContent>(comp);
      d_operators.push_back(inner);
    }
    d_doc.attach_layer(d_root, d_doc.add_layer(d_doc.add_content(inner), Affine::identity()));
  }

  SceneKind d_kind;
  int d_dim;
  Document d_doc;
  ObjectId d_root{};
  std::vector<std::shared_ptr<PaintedLeaf>> d_leaves;
  std::vector<std::shared_ptr<Content>> d_operators;
  std::vector<ObjectId> d_leaf_ids;
};

// --- The counters a pass reports ----------------------------------------------

// The behavioral snapshot one drive-to-quiescence produced. Every field is a wall-clock-
// free counter (doc 16:54-62); the benchmark emits them beside its timing, and the
// bench-smoke asserts on them. `max_in_flight` is the pool's own high-water mark, the
// observable that pins "adding workers buys parallelism, never duplicate renders".
struct SceneCounters {
  std::uint64_t frames_rendered{0};
  std::uint64_t requests_issued{0};
  std::uint64_t operator_renders{0};
  std::uint64_t composites{0};
  std::uint64_t degraded_composites{0};
  std::uint64_t follow_up_frames{0};
  std::uint64_t deadline_expiries{0};
  std::uint64_t tiles_cancelled{0};
  std::uint64_t max_in_flight{0};
};

// The frame-loop harness over one scene at one worker count: the renderer (and with it
// the pool), the cache, the surface pool and the persisted device target a host owns.
// Constructed ONCE per benchmark, so the per-iteration cost is the frame loop's, not
// `std::thread`'s.
class BenchHarness {
public:
  BenchHarness(BenchScene& scene, WorkerPoolConfig pool_config, std::size_t cache_bytes)
      : d_scene(scene), d_cache(cache_bytes), d_surfaces(d_backend),
        d_renderer(std::move(pool_config)) {
    expected<std::unique_ptr<Surface>, SurfaceError> target =
        d_backend.make_surface(scene.dim(), scene.dim(), k_working_rgba32f);
    d_target = target.has_value() ? std::move(*target) : nullptr;
  }

  // One pass: damage the whole scene, then drive frames until the loop is genuinely
  // settled -- nothing in flight AND no follow-up owed. Both conditions are load-bearing
  // (`worker_dispatch_leaf_only.t.cpp:461-470`): a frame parks only until the FIRST
  // completion settles, so a fan-out needs several frames to reap them all, and a frame
  // that REAPS an arrival does not composite it -- it carries the routed damage, so the
  // next frame is the one that re-plans against the now-warm input. This is exactly the
  // loop a host runs on `FrameOutcome::schedule_follow_up` (`host_viewport.cpp:160,178`).
  //
  // Returns the DELTA counters for this pass, so a benchmark iteration reports per-pass
  // numbers rather than a running total (Decision 6: the counters are persistent driver
  // state; a caller that wants per-frame numbers subtracts).
  SceneCounters run_pass(std::chrono::steady_clock::duration budget, int max_frames = 256) {
    const SceneCounters before = snapshot();
    const std::vector<Damage> damage = d_scene.whole_scene_damage();
    const DocStatePtr pin = d_scene.document().pin();
    const ContentResolver resolve = [this](ObjectId id) { return d_scene.document().resolve(id); };
    const Viewport view{d_scene.dim(), d_scene.dim(), Affine::identity(), d_scene.root()};
    const FrameBinding binding{&d_scene.document(), pin};

    for (int i = 0; i < max_frames; ++i) {
      const std::span<const Damage> frame_damage =
          i == 0 ? std::span<const Damage>(damage) : std::span<const Damage>{};
      const InteractiveRenderer::FrameOutcome outcome =
          d_renderer.render_frame(*pin, resolve, view, d_cache, d_backend, d_surfaces, *d_target,
                                  frame_damage, k_scene_time, budget, binding);
      if (!outcome.schedule_follow_up && d_renderer.pending().tiles.empty()) {
        break;
      }
    }
    return delta(before, snapshot());
  }

  const Surface& target() const noexcept { return *d_target; }
  bool usable() const noexcept { return d_target != nullptr; }

private:
  SceneCounters snapshot() {
    const CompositorCounters& c = d_renderer.counters();
    return SceneCounters{d_renderer.frames_rendered(), c.requests_issued(), c.operator_renders(),
                         c.composites(), c.degraded_composites(), c.follow_up_frames(),
                         d_renderer.deadline_expiries(), d_renderer.tiles_cancelled(),
                         // The pool's high-water mark is a MAX, not a count: it does not
                         // subtract across a pass, so it is carried through verbatim.
                         d_renderer.worker_pool().max_in_flight_per_content()};
  }

  static SceneCounters delta(const SceneCounters& a, const SceneCounters& b) {
    return SceneCounters{b.frames_rendered - a.frames_rendered,
                         b.requests_issued - a.requests_issued,
                         b.operator_renders - a.operator_renders,
                         b.composites - a.composites,
                         b.degraded_composites - a.degraded_composites,
                         b.follow_up_frames - a.follow_up_frames,
                         b.deadline_expiries - a.deadline_expiries,
                         b.tiles_cancelled - a.tiles_cancelled,
                         b.max_in_flight};
  }

  BenchScene& d_scene;
  BenchBackend d_backend;
  TileCache d_cache;
  SurfacePool d_surfaces;
  std::unique_ptr<Surface> d_target;
  InteractiveRenderer d_renderer; // declared after d_target: the pool joins before it dies
};

} // namespace arbc::runtime_bench
