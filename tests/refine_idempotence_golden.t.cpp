// A damage-gated frame CLEARS and CLIPS its repaint region
// (`compositor.refine_frame_composite_idempotence`, doc 02 § The frame,
// interactively; doc 09 § Backend contract).
//
// The target surface is the caller's and persists across frames, so a gated frame
// re-composites onto pixels a previous frame already painted -- and source-over is
// not idempotent for anything but fully-opaque content. Before this task a gated
// frame did not clear (the clear was gated on `dirty == nullptr`), so a
// translucent layer's contribution landed twice, and the refine loop quiesced --
// cleanly, with nothing pending and nothing degraded -- on wrong pixels.
//
// The existing gated golden (`damage_planning_golden.t.cpp:35-38`) cannot see this
// and says so: its scene is deliberately built from opaque solids over a
// full-viewport opaque background, "so the un-cleared gated composite reproduce[s]
// a cleared full re-render byte-for-byte (opaque source-over is a replace)". This
// file is its inverse -- a TRANSLUCENT scene, nested one level, where a double
// source-over is observable -- and it pins the three promises the clear + clip buy:
//
//   1. compositing the same gated frame twice is a no-op (the core regression);
//   2. a refined interactive sequence quiesces byte-identical to a single full
//      pass, at worker_count 0 and 2;
//   3. a gated frame writes no pixel outside its repaint region (which is what
//      forbids "just repaint everything" as the fix).
//
// Byte-exact, deterministic, no worker race in cases 1 and 3 (doc 16:48-53). Case 2
// drives a real pool but asserts only byte-exactness at quiescence, which is
// schedule-independent; the seeded schedule sweep is
// `refine_idempotence_stress.t.cpp`.
//
// CROSS-COMPONENT (tests/, linking the umbrella `arbc`): a real `CpuBackend`, the
// concrete kinds, and both the compositor driver and the runtime loop (doc 17).

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/expected.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/cache/keyed_store.hpp>
#include <arbc/compositor/compositor.hpp>
#include <arbc/compositor/counters.hpp>
#include <arbc/compositor/damage_planning.hpp>
#include <arbc/compositor/pull_service.hpp>
#include <arbc/compositor/tile_planning.hpp>
#include <arbc/contract/content.hpp>
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

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

using namespace arbc;

namespace {

// Two rung-0 tiles across, so a sub-rect damage can gate a PROPER SUBSET of the
// planned tiles -- which is what makes case 3 (and the tile-overhang half of the
// fix) meaningful. A 512x512 viewport at scale 1.0 covers a 2x2 tile grid.
constexpr int k_dim = 512;

Rect canvas() { return Rect{0.0, 0.0, static_cast<double>(k_dim), static_cast<double>(k_dim)}; }
Viewport viewport() { return Viewport{k_dim, k_dim, Affine::identity()}; }
constexpr auto k_frame_budget = std::chrono::milliseconds(100);

// The scene the whole file turns on: TRANSLUCENT content, nested one level.
//
// EVERY layer is semi-transparent, and that is the load-bearing property -- it is what
// `damage_planning_golden.t.cpp:35-38` deliberately does NOT have. An opaque
// full-viewport bottom layer would make the un-cleared gated composite reproduce the
// cleared one byte-for-byte, because an opaque source-over IS a replace: it overwrites
// whatever the previous frame left there, masking the double-blend completely. With a
// translucent stack the previous frame's pixels SURVIVE into the blend, so a
// contribution that lands twice moves the bytes -- which is the whole point.
//
// The veils live inside a child composition shown through a `NestedContent`, so the
// frame's top layer is an operator whose composed output is itself translucent: exactly
// the shape a refine frame re-composites. The frame walk is composition-scoped (doc
// 05:28-36), so the top-level members are the background and the nested layer, held in a
// root composition the frame anchors at; the child's veil layers are reached only through
// `NestedContent::render`.
//
// Every color is premultiplied working-space (doc 07 rule 2): each channel <= its
// alpha.
struct TranslucentScene {
  Document doc;
  ObjectId root{}; // the composition the frame walk anchors at
  ObjectId background{};
  ObjectId veil_content{};
  ObjectId nested_content{};

  TranslucentScene() {
    root = doc.add_composition(static_cast<double>(k_dim), static_cast<double>(k_dim));
    background =
        doc.add_content(std::make_shared<SolidContent>(Rgba{0.40F, 0.08F, 0.08F, 0.80F}, canvas()));
    doc.attach_layer(root, doc.add_layer(background, Affine::identity()));

    const ObjectId child =
        doc.add_composition(static_cast<double>(k_dim), static_cast<double>(k_dim));
    veil_content =
        doc.add_content(std::make_shared<SolidContent>(Rgba{0.05F, 0.20F, 0.35F, 0.5F}, canvas()));
    doc.attach_layer(child, doc.add_layer(veil_content, Affine::identity()));
    const ObjectId veil_b =
        doc.add_content(std::make_shared<SolidContent>(Rgba{0.30F, 0.10F, 0.05F, 0.60F}, canvas()));
    doc.attach_layer(child, doc.add_layer(veil_b, Affine::identity()));

    nested_content = doc.add_content(std::make_shared<NestedContent>(child));
    doc.attach_layer(root, doc.add_layer(nested_content, Affine::identity()));
  }
};

std::vector<float> snapshot(const Surface& surface) {
  const std::span<const float> px = std::as_const(surface).span<PixelFormat::Rgba32fLinearPremul>();
  return {px.begin(), px.end()};
}

bool byte_identical(const std::vector<float>& a, const std::vector<float>& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

// The pixel at (x, y) of a snapshot, as its four premultiplied working floats.
std::span<const float> pixel_at(const std::vector<float>& px, int x, int y) {
  const std::size_t at = 4U * (static_cast<std::size_t>(y) * static_cast<std::size_t>(k_dim) +
                               static_cast<std::size_t>(x));
  return std::span<const float>(px).subspan(at, 4);
}

bool pixels_equal(std::span<const float> a, std::span<const float> b) {
  return std::memcmp(a.data(), b.data(), 4 * sizeof(float)) == 0;
}

// A frame's worth of compositor plumbing over one persisted target, so a test can
// run several `render_frame_interactive` passes against the same warm cache and the
// same pixels -- which is the whole subject here.
class Frames {
public:
  explicit Frames(Backend& backend, TranslucentScene& scene)
      : d_scene(scene), d_backend(backend), d_pool(backend), d_cache(64U * 1024 * 1024) {
    expected<std::unique_ptr<Surface>, SurfaceError> target =
        backend.make_surface(k_dim, k_dim, k_working_rgba32f);
    REQUIRE(target.has_value());
    d_target = std::move(*target);
  }

  // One pass. `dirty == nullptr` is the un-gated full pass (clears the whole target,
  // plans the whole viewport); non-null is the damage-gated frame under test. The
  // dispatch is `direct_dispatch` and there is no `pending` sink, so every miss --
  // including the nesting's descent into its child composition -- renders and settles
  // INLINE: these passes are deterministic to the byte, with no pool and no race.
  void pass(const DirtyRegion* dirty, CompositorCounters* counters = nullptr) {
    const DocStatePtr pin = d_scene.doc.pin();
    const ContentResolver resolve = [this](ObjectId id) { return d_scene.doc.resolve(id); };
    // The nesting descends through a `PullService` the frame driver owns, and it
    // renders nothing at all unless its operators are bound to that service -- so
    // bind them exactly as `InteractiveRenderer::render_frame` does (doc 13 § Binding
    // is the render driver's obligation). Frame-local, like the driver's.
    // The config is the interactive driver's, field for field (`interactive.cpp`), minus
    // the async plumbing this deterministic path has no use for: the synthesized pull
    // identity (so an operator's inputs key under distinct ids, doc 13:141-154) and the
    // one-revision contribution fold. Without `id_of` the nesting's descent keys its
    // inputs under a colliding id and renders nothing at all -- the pass would still be
    // self-consistent, and would still be the WRONG oracle.
    // The operator kinds bind through the global binder registry, which both drivers
    // populate before they bind (`interactive.cpp:47`, `offline_sequence.cpp:63`).
    // Unregistered, `bind_operators` binds NOTHING and the nesting renders nothing --
    // silently, and self-consistently, into a wrong oracle. Idempotent.
    register_builtin_operator_binders();
    PullConfig config;
    config.id_of = make_pull_identity_of(*pin, resolve);
    const std::uint64_t revision = pin->revision();
    config.contribution = [revision](const Content*) { return revision; };
    PullServiceImpl pulls(d_cache, d_backend, direct_dispatch(), std::move(config));
    const OperatorBindingScope binding = bind_operators(d_scene.doc, pulls, d_backend, pin);
    const Viewport view{k_dim, k_dim, Affine::identity(), d_scene.root};
    render_frame_interactive(*pin, resolve, view, d_cache, d_backend, d_pool, *d_target,
                             Deadline::none(), std::nullopt, /*pending=*/nullptr, counters, dirty,
                             Time::zero(), /*visible_plans=*/nullptr, /*diagnostics=*/nullptr,
                             &pulls);
  }

  std::vector<float> pixels() const { return snapshot(*d_target); }

private:
  TranslucentScene& d_scene;
  Backend& d_backend;
  SurfacePool d_pool;
  TileCache d_cache;
  std::unique_ptr<Surface> d_target;
};

// The whole viewport, as a damage-gated frame's dirty region: every layer is
// re-planned and re-composited, so the gated pass paints exactly what a full pass
// would -- which is precisely why doubling it is detectable.
DirtyRegion whole_viewport_dirty() {
  return DirtyRegion{{Rect{0.0, 0.0, static_cast<double>(k_dim), static_cast<double>(k_dim)}}};
}

// An `InteractiveRenderer` driven to quiescence: frames until nothing is pending and
// no follow-up is owed. Both conditions matter -- a frame that reaps an arrival
// carries its damage to the NEXT frame rather than compositing it, so stopping at
// `pending.empty()` stops one frame before the pixels exist.
std::vector<float> quiesced_pixels(Backend& backend, TranslucentScene& scene,
                                   std::size_t worker_count) {
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
    const DocStatePtr pin = scene.doc.pin();
    const ContentResolver resolve = [&scene](ObjectId id) { return scene.doc.resolve(id); };
    const FrameBinding binding{&scene.doc, pin};
    const InteractiveRenderer::FrameOutcome outcome = renderer.render_frame(
        *pin, resolve, Viewport{k_dim, k_dim, Affine::identity(), scene.root}, cache, backend, pool,
        **target, {}, Time::zero(), k_frame_budget, binding);
    if (!outcome.schedule_follow_up && renderer.pending().tiles.empty()) {
      return snapshot(**target);
    }
  }
  FAIL("the interactive frame loop did not reach quiescence");
  return {};
}

} // namespace

// enforces: 16-sdlc-and-quality#byte-exact-goldens
// enforces: 02-architecture#gated-frame-repaint-is-idempotent
TEST_CASE("refine golden: a gated frame composited twice is byte-identical to composited once") {
  CpuBackend backend;
  TranslucentScene scene;
  Frames frames(backend, scene);

  // Warm the cache with a full un-gated pass, so the gated frames below re-composite
  // from cache and issue no renders at all -- the fix must be about the COMPOSITE,
  // not about skipping work.
  frames.pass(nullptr);

  const DirtyRegion dirty = whole_viewport_dirty();

  CompositorCounters first;
  frames.pass(&dirty, &first);
  const std::vector<float> once = frames.pixels();

  // The same gated frame, again, onto the same persisted target. Source-over is not
  // idempotent for translucent content, so before this task the veil's contribution
  // landed a second time and these bytes moved. The clear is what makes the second
  // pass a no-op.
  CompositorCounters second;
  frames.pass(&dirty, &second);
  const std::vector<float> twice = frames.pixels();

  REQUIRE(byte_identical(once, twice));

  // ... and it is idempotent because it REPAINTS, not because it skipped the work
  // (doc 16:54-62, behavioral counters, never wall clock). A second identical gated
  // frame over a warm cache issues zero renders and re-composites the same tile
  // count -- the fix must not turn idempotence into "do nothing", and must not
  // double the composites either.
  CHECK(second.requests_issued() == 0);
  CHECK(first.requests_issued() == 0); // the warm pass above already rendered every tile
  CHECK(second.composites() == first.composites());
  CHECK(second.composites() > 0);

  // A third pass pins that the invariant is a fixed point, not an alternation.
  frames.pass(&dirty);
  CHECK(byte_identical(once, frames.pixels()));
}

// enforces: 16-sdlc-and-quality#byte-exact-goldens
// enforces: 02-architecture#gated-frame-equals-single-pass
TEST_CASE("refine golden: a refined interactive sequence quiesces byte-identical to a single "
          "full pass") {
  CpuBackend backend;

  // The oracle: ONE un-gated pass over the same scene, every miss settled inline
  // (no pool, no deadline pressure), so it is unconditionally the exact composite.
  TranslucentScene oracle_scene;
  Frames oracle_frames(backend, oracle_scene);
  oracle_frames.pass(nullptr);
  const std::vector<float> oracle = oracle_frames.pixels();

  // A refine frame is byte-exact regardless of HOW the arrivals interleaved, so the
  // full interactive loop -- first frame un-gated, every later frame damage-gated on
  // the arrival damage -- must land on exactly those pixels. `worker_count == 0` is
  // the shipped default (every miss settles inline, so the refine path barely
  // fires); `worker_count == 2` is the configuration that makes refine frames the
  // NORMAL path and turned this bug from latent into routine.
  for (const std::size_t workers : {std::size_t{0}, std::size_t{2}}) {
    TranslucentScene scene;
    CHECK(byte_identical(quiesced_pixels(backend, scene, workers), oracle));
  }
}

// enforces: 16-sdlc-and-quality#byte-exact-goldens
// enforces: 02-architecture#gated-frame-touches-only-its-repaint-region
TEST_CASE("refine golden: a gated frame touches no pixel outside its repaint region") {
  CpuBackend backend;
  TranslucentScene scene;
  Frames frames(backend, scene);

  frames.pass(nullptr);
  const std::vector<float> before = frames.pixels();

  // Damage the top-left tile only. The repaint region is its device rect, rounded
  // out -- so the frame may repaint [0, 256) x [0, 256) and NOTHING else. This is
  // what forbids "just clear the whole target and repaint everything" as the fix:
  // that would also be idempotent, and would also be byte-identical to a full pass,
  // and would still be wrong -- it would throw away the damage gate.
  const DirtyRegion dirty{{Rect{0.0, 0.0, 256.0, 256.0}}};
  const Rect repaint = repaint_region(dirty, viewport());
  REQUIRE(repaint == Rect{0.0, 0.0, 256.0, 256.0});

  frames.pass(&dirty);
  const std::vector<float> after = frames.pixels();

  // Inside the region the pixels are re-composited from scratch (cleared, then every
  // intersecting layer repainted): they must land back on exactly what the full pass
  // put there. Outside it, every pixel survives from the previous frame BIT for bit
  // -- including the pixels a tile straddling the region's edge would have spilled
  // onto had the composite not been clipped.
  for (int y = 0; y < k_dim; ++y) {
    for (int x = 0; x < k_dim; ++x) {
      CAPTURE(x, y);
      REQUIRE(pixels_equal(pixel_at(after, x, y), pixel_at(before, x, y)));
    }
  }

  // The region really was repainted (not skipped): a cleared-and-not-repainted region
  // would be transparent, and the assertion above would have caught it -- but pin the
  // scene's own non-triviality too, so a scene that rendered nothing cannot pass this
  // test vacuously.
  const std::span<const float> inside = pixel_at(after, 8, 8);
  CHECK(inside[3] > 0.0F);
}

// enforces: 16-sdlc-and-quality#byte-exact-goldens
// enforces: 02-architecture#disjoint-repaint-equals-bbox-repaint
TEST_CASE("refine golden: a disjoint-rect gated frame is byte-identical to a bbox gated frame") {
  CpuBackend backend;

  // The WBS note's scenario: two small damages at opposite corners, each inside its own
  // rung-0 tile. The disjoint repaint set is those two rects; their BOUNDING BOX is the
  // whole viewport -- so the bbox frame also re-composites the two tiles between them.
  const DirtyRegion dirty{
      {Rect{0.0, 0.0, 32.0, 32.0},
       Rect{static_cast<double>(k_dim) - 32.0, static_cast<double>(k_dim) - 32.0,
            static_cast<double>(k_dim), static_cast<double>(k_dim)}}};
  // The bbox path, forced: a one-rect `DirtyRegion` holding exactly `repaint_region`'s
  // box normalizes to itself, so that frame IS the pre-task, bounding-box behavior --
  // and is also what `repaint_regions` returns over its rect-count cap.
  const DirtyRegion boxed{{repaint_region(dirty, viewport())}};
  REQUIRE(repaint_regions(dirty, viewport()).size() == 2);
  REQUIRE(repaint_regions(boxed, viewport()).size() == 1);
  REQUIRE(repaint_regions(boxed, viewport())[0] ==
          Rect{0.0, 0.0, static_cast<double>(k_dim), static_cast<double>(k_dim)});

  // Two separately-persisted targets, each warmed by the same un-gated full pass, then
  // each given the same damage -- one as the disjoint set, one as its bounding box.
  TranslucentScene split_scene;
  Frames split_frames(backend, split_scene);
  split_frames.pass(nullptr);
  CompositorCounters split;
  split_frames.pass(&dirty, &split);

  TranslucentScene boxed_scene;
  Frames boxed_frames(backend, boxed_scene);
  boxed_frames.pass(nullptr);
  CompositorCounters box;
  boxed_frames.pass(&boxed, &box);

  // The oracle, and it is not a tautology: it holds BECAUSE the gap pixels the bbox
  // repaints are undamaged, so by the persisted-target contract they already hold
  // exactly what it repaints into them. Skipping them is therefore byte-free -- which
  // also means no existing golden needs a new baseline, and that is itself an assertion
  // this task makes.
  CHECK(byte_identical(split_frames.pixels(), boxed_frames.pixels()));

  // ... and the disjoint frame reached those pixels doing strictly less work (doc
  // 16:54-62, counters, never wall clock): it composites only the tiles its two repaint
  // rects overlap, where the bbox composites every tile of every layer between them.
  CHECK(split.composites() > 0);
  CHECK(split.composites() < box.composites());
  // Both re-composite from the warm cache -- the win is in the COMPOSITE, not in
  // skipping renders that were owed.
  CHECK(split.requests_issued() == 0);
  CHECK(box.requests_issued() == 0);
}

// enforces: 16-sdlc-and-quality#byte-exact-goldens
// enforces: 02-architecture#gated-frame-equals-single-pass
TEST_CASE("refine golden: a translucent layer under two OVERLAPPING damages lands exactly once") {
  CpuBackend backend;
  TranslucentScene scene;
  Frames frames(backend, scene);

  frames.pass(nullptr);
  const std::vector<float> full_pass = frames.pixels();

  // The rects `map_damage_to_device` really emits: one per (damage, layer) pair, and
  // they OVERLAP. This is the regression guard for the bug the predecessor's bounding
  // box was dodging -- clipping each tile once per RAW dirty rect would composite it
  // twice in the overlap, landing this deliberately translucent stack's contribution a
  // second time there. Normalized to a disjoint set, the overlap belongs to exactly one
  // repaint rect, so it is cleared once and repainted once, and the frame lands back on
  // exactly what the full pass put there.
  const DirtyRegion dirty{{Rect{0.0, 0.0, 300.0, 300.0}, Rect{200.0, 200.0, 512.0, 512.0}}};
  REQUIRE(repaint_regions(dirty, viewport()).size() > 1); // it really did decompose

  frames.pass(&dirty);
  CHECK(byte_identical(frames.pixels(), full_pass));
}
