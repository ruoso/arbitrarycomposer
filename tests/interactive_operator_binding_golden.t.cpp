// The interactive driver BINDS its operators, pinned against the export driver
// (`runtime.interactive_binder_wiring`, doc 13 § "Binding is the render driver's
// obligation, and every driver discharges it").
//
// `runtime.interactive_pull_wiring` served the frame driver a real `PullService`, which
// covered the operator behaviors the DRIVER performs: an identity endpoint (a crossfade
// at w == 0/1, a fade at envelope == 1) whose terminal input's tiles the driver delivers
// itself. It did not cover the ones the OPERATOR performs. A fade at envelope 0.5 and a
// crossfade at w 0.5 are not endpoints: the compositor calls the operator's own
// `render()`, which pulls its input through the service it can only have received at
// ATTACH -- and unattached it trips `assert(d_pull != nullptr ... "rendered before
// attach")` (`fade_content.cpp:94`), or in a release build composites nothing. A nesting
// is worse still: `NestedContent::inputs()` is a memo projected from the child
// composition and is EMPTY until attach, so an unbound nested scene that exports
// perfectly through `SequenceRenderer` shows nothing at all in a viewport.
//
// This file is that gap closed, on the path a host actually drives -- a `Document`-bound
// `HostViewport::step()`, which is where the pin lives -- and byte-exact against the
// export driver's frame of the same document at the same instant (the CPU backend is
// specified deterministic, doc 16 tier 3, and an unpressured `BestEffort` frame degrades
// nowhere). Pre-task every interactive frame below asserts or comes out blank.
//
// CROSS-COMPONENT (tests/, linking the umbrella `arbc`): a real `CpuBackend` and the
// concrete operator kinds beside the runtime drivers (doc 17).

#include <arbc/backend_cpu/cpu_backend.hpp>
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
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/media/pixel_format.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/host_viewport.hpp>
#include <arbc/runtime/interactive.hpp>
#include <arbc/runtime/offline_sequence.hpp>
#include <arbc/runtime/pull_identity.hpp>
#include <arbc/surface/surface.hpp>
#include <arbc/surface/surface_pool.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

using namespace arbc;

namespace {

// One rung-0 tile at scale 1.0, so a frame is a single planned tile.
constexpr int k_dim = 256;
constexpr auto k_budget = std::chrono::hours(1); // no deadline pressure: nothing degrades

Rect canvas() { return Rect{0.0, 0.0, static_cast<double>(k_dim), static_cast<double>(k_dim)}; }
Viewport viewport() { return Viewport{k_dim, k_dim, Affine::identity()}; }

// The only clocks either driver reads are injected; a fake clock fixed at the
// steady_clock epoch puts every deadline instant in the real past, so no frame below
// reads a wall clock or blocks (doc 16:54-62). Every miss here answers inline anyway.
InteractiveRenderer::Clock epoch_clock() {
  return [] { return std::chrono::steady_clock::time_point{}; };
}

// A fade whose out-ramp spans [0, 1000): the envelope is 0.5 at t == 500 -- an INTERIOR
// weight, so `identity()` declines and the operator's own `render` runs.
FadeParams half_fade() {
  return FadeParams{FadeShape::Linear, std::nullopt, FadeWindow{Time{0}, Time{1000}}};
}

// A crossfade over [0, 1000): w is 0.5 at t == 500 -- likewise interior.
CrossfadeParams half_crossfade() {
  return CrossfadeParams{CrossfadeShape::Linear, Time{0}, Time{1000}};
}

constexpr Time k_interior{500}; // the instant every frame below renders at

std::vector<float> snapshot(const Surface& surface) {
  const std::span<const float> px = std::as_const(surface).span<PixelFormat::Rgba32fLinearPremul>();
  return {px.begin(), px.end()};
}

bool byte_identical(const std::vector<float>& a, const std::vector<float>& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

bool all_transparent(const std::vector<float>& px) {
  for (const float v : px) {
    if (v != 0.0F) {
      return false;
    }
  }
  return true;
}

// The export driver's frame of `doc` at `time` -- the oracle every interactive frame
// below is compared against. `SequenceRenderer` binds the operators itself.
std::vector<float> exported_frame(Document& doc, Backend& backend, Time time) {
  SequenceRenderer renderer(doc, viewport(), backend);
  const expected<std::unique_ptr<Surface>, SurfaceError> frame = renderer.render_frame_at(time);
  REQUIRE(frame.has_value());
  return snapshot(**frame);
}

// A `Document`-bound `HostViewport` over its own renderer/cache/target: the path a host
// actually drives, and the one that holds the pin `bind_operators` needs. Everything is
// owned here so a test can drive N steps and then read both the pixels and the counters.
class BoundViewport {
public:
  BoundViewport(Document& doc, Backend& backend, ObjectId bootstrap_layer)
      : d_doc(doc), d_bootstrap(bootstrap_layer), d_cache(64U * 1024 * 1024), d_pool(backend),
        d_target(make_target(doc, backend)), d_renderer({}, epoch_clock()),
        d_view(d_renderer, doc, HostViewport::DocumentBinding{}, backend, d_pool, d_cache,
               *d_target, epoch_clock(), config()) {
    // The playhead is an injected deterministic source, so `composition_time` is a pure
    // function of the frame index -- no transport advance, no wall clock (doc 16:54-62).
    d_view.set_playhead_source([this] { return d_time; });
  }

  // Drive one frame at `time`. A fresh viewport has no damage, no owed follow-up and an
  // unmoved scene, so its first `step()` would legitimately issue zero frames (doc
  // 01:140) -- a trivial re-set of the bootstrap layer's transform is the model edit that
  // damages it and makes the first step render, exactly as `host_viewport.t.cpp` does.
  void step_at(Time time) {
    d_time = time;
    if (!d_bootstrapped) {
      d_doc.set_layer_transform(d_bootstrap, Affine::identity());
      d_bootstrapped = true;
    }
    d_view.step();
  }

  std::vector<float> pixels() const { return snapshot(*d_target); }
  std::uint64_t frames_issued() const noexcept { return d_view.frames_issued(); }
  std::uint64_t operator_binds() const noexcept { return d_renderer.operator_binds(); }

private:
  static std::unique_ptr<Surface> make_target(Document& doc, Backend& backend) {
    const DocStatePtr pin = doc.pin();
    expected<std::unique_ptr<Surface>, SurfaceError> target =
        backend.make_surface(k_dim, k_dim, pin->working_space());
    REQUIRE(target.has_value());
    return std::move(*target);
  }
  static HostViewport::Config config() {
    HostViewport::Config cfg;
    cfg.viewport = viewport();
    cfg.budget = k_budget;
    return cfg;
  }

  Document& d_doc;
  ObjectId d_bootstrap;
  bool d_bootstrapped{false};
  Time d_time{};
  TileCache d_cache;
  SurfacePool d_pool;
  std::unique_ptr<Surface> d_target;
  InteractiveRenderer d_renderer;
  HostViewport d_view;
};

// A nested scene: a child composition of TWO fade-over-solid layers, embedded at the
// parent's global root (the layer walk both drivers run). Both child layers are
// operators, so the binder's attach-before-`inputs()` order is exercised THROUGH the
// nesting boundary -- the child's fades are reachable only once nested itself is attached
// and its `inputs()` memo projects them. Both are `Timed`, which is what A5 needs (two
// SAME-stability child layers) and what makes the nesting itself Timed, so a clock
// advance damages it and every frame of the A4 loop actually renders.
struct NestedScene {
  Document doc;
  std::shared_ptr<SolidContent> solid_a =
      std::make_shared<SolidContent>(Rgba{0.60F, 0.20F, 0.10F, 0.80F}, canvas());
  std::shared_ptr<SolidContent> solid_b =
      std::make_shared<SolidContent>(Rgba{0.10F, 0.40F, 0.30F, 0.50F}, canvas());
  std::shared_ptr<FadeContent> fade_a = std::make_shared<FadeContent>(solid_a.get(), half_fade());
  std::shared_ptr<FadeContent> fade_b = std::make_shared<FadeContent>(solid_b.get(), half_fade());
  std::shared_ptr<NestedContent> nested;
  ObjectId nest_layer{};

  NestedScene() {
    const ObjectId child =
        doc.add_composition(static_cast<double>(k_dim), static_cast<double>(k_dim));
    doc.attach_layer(child, doc.add_layer(doc.add_content(fade_a), Affine::identity()));
    doc.attach_layer(child, doc.add_layer(doc.add_content(fade_b), Affine::identity()));
    nested = std::make_shared<NestedContent>(child);
    nest_layer = doc.add_layer(doc.add_content(nested), Affine::identity());
  }
};

} // namespace

// enforces: 13-effects-as-operators#operators-bound-on-every-driver
TEST_CASE("interactive: a fade at an interior envelope and a crossfade at an interior weight "
          "render byte-exact against the export driver") {
  CpuBackend backend;
  SolidContent under{Rgba{0.25F, 0.50F, 0.75F, 1.0F}, canvas()};
  SolidContent from{Rgba{0.50F, 0.25F, 0.125F, 1.0F}, canvas()};
  SolidContent to{Rgba{0.125F, 0.375F, 0.75F, 1.0F}, canvas()};

  // `Document` is non-movable and the operators borrow the solids non-owningly, so all
  // four objects outlive every render below.
  Document doc;
  const ObjectId fade_layer = doc.add_layer(
      doc.add_content(std::make_shared<FadeContent>(&under, half_fade())), Affine::identity());
  doc.add_layer(doc.add_content(std::make_shared<CrossfadeContent>(&from, &to, half_crossfade())),
                Affine::identity());

  // At t == 500 the fade's envelope is 0.5 and the crossfade's w is 0.5: NEITHER is an
  // identity endpoint, so the compositor drives both operators' own `render`, each of
  // which pulls its inputs through the service the frame's binding injected. Pre-task
  // this frame tripped the fade's "rendered before attach" assert.
  BoundViewport view(doc, backend, fade_layer);
  view.step_at(k_interior);
  REQUIRE(view.frames_issued() == 1);

  const std::vector<float> pixels = view.pixels();
  CHECK_FALSE(all_transparent(pixels)); // an unbound frame composites nothing at all
  CHECK(byte_identical(pixels, exported_frame(doc, backend, k_interior)));
  CHECK(view.operator_binds() == 1);
}

// enforces: 05-recursive-composition#nested-runtime-bound
// enforces: 08-serialization#nesting-inputs-are-derived-not-persisted
TEST_CASE("interactive: a nested scene renders byte-exact against its export") {
  CpuBackend backend;
  NestedScene scene;

  // Unbound, `inputs()` is the empty pre-attach memo: there is no child graph to compose
  // and the nesting paints nothing. This is the state the interactive path was stuck in.
  REQUIRE(scene.nested->inputs().empty());

  BoundViewport view(scene.doc, backend, scene.nest_layer);
  view.step_at(k_interior);
  REQUIRE(view.frames_issued() == 1);

  const std::vector<float> pixels = view.pixels();
  CHECK_FALSE(all_transparent(pixels));
  CHECK(byte_identical(pixels, exported_frame(scene.doc, backend, k_interior)));

  // The interactive half of registry row 246: `inputs()` is non-empty exactly when
  // `bind_operators` has attached it -- "the state every rendered frame of an interactive
  // session leaves it in". The frame-local scope is long gone, but the memo survives
  // `detach` (it is a pure function of the child's aggregate revision), so the projection
  // the frame built is still the honest answer.
  CHECK(scene.nested->inputs().size() == 2);
}

// enforces: 05-recursive-composition#nested-metadata-memoized-on-aggregate-revision
TEST_CASE("interactive: the per-frame re-bind does not thrash nested's metadata memo") {
  CpuBackend backend;
  NestedScene scene;

  // A nested-of-fades scene is `Timed`, so ADVANCING THE CLOCK damages it every frame:
  // each step below really does render, and really does re-bind. The still-scene early-out
  // is no protection here -- that is the whole point. `bind_operators` re-runs `try_attach`
  // on every content on every call, and `NestedContent::attach` re-keys its metadata memo
  // only on an actually-NEW pin (`kinds.nested_runtime_binding` Decision 3). The
  // interactive loop re-pins every frame, but the revision is unchanged, so the pin is the
  // same snapshot and the memo must survive. A careless binder here would make
  // `metadata_recomputes()` grow linearly with frame count and quietly break the shipped
  // aggregate-revision memoization claim.
  BoundViewport view(scene.doc, backend, scene.nest_layer);
  view.step_at(k_interior);
  REQUIRE(view.frames_issued() == 1);
  REQUIRE(view.operator_binds() == 1);
  const std::uint64_t recomputes = scene.nested->metadata_recomputes();
  REQUIRE(recomputes >= 1); // the first bind DID key the memo -- the check is not vacuous

  for (int i = 1; i <= 8; ++i) {
    view.step_at(Time{k_interior.flicks + i});
  }
  CHECK(view.frames_issued() == 9);
  CHECK(view.operator_binds() == 9);                        // every damaged frame re-bound...
  CHECK(scene.nested->metadata_recomputes() == recomputes); // ...and none re-keyed the memo
}

// enforces: 13-effects-as-operators#operator-input-children-have-distinct-cache-identity
TEST_CASE("interactive: a nesting's child layers carry distinct pull identities") {
  CpuBackend backend;
  NestedScene scene;

  BoundViewport view(scene.doc, backend, scene.nest_layer);
  view.step_at(k_interior);
  REQUIRE(view.frames_issued() == 1);

  // The frame's own `id_of` seam, rebuilt over the frame's pin. Its map seeds from
  // `DocRoot::for_each_layer`, which is DOCUMENT-GLOBAL: it enumerates every `LayerRecord`
  // in the records HAMT, child compositions included. So a nesting's child layers are
  // identified by their real model `ObjectId`s -- NOT by the `inputs()` walk, which only
  // synthesizes ids for inputs no layer names.
  const DocStatePtr pin = scene.doc.pin();
  const ContentResolver resolve = [&scene](ObjectId id) { return scene.doc.resolve(id); };
  const std::function<ObjectId(const Content*)> id_of = make_pull_identity_of(*pin, resolve);

  const ObjectId id_a = id_of(scene.fade_a.get());
  const ObjectId id_b = id_of(scene.fade_b.get());
  // Two SAME-stability child layers of one nesting (both fades, both Timed). If
  // `for_each_layer` were ever made composition-scoped, neither would be seeded, both would
  // fall to the root `ObjectId{}`, and the two child layers would collapse onto one
  // `TileKey` -- silently serving one child's tiles for the other. This test fails instead.
  CHECK(id_a.valid());
  CHECK(id_b.valid());
  CHECK(id_a != id_b);

  // The child layers' own inputs -- the solids the fades pull -- are not layers anywhere,
  // so they draw SYNTHESIZED ids: distinct from each other and from every model id.
  const ObjectId in_a = id_of(scene.solid_a.get());
  const ObjectId in_b = id_of(scene.solid_b.get());
  CHECK(in_a != in_b);
  CHECK(in_a != id_a);
  CHECK(in_b != id_b);
}
