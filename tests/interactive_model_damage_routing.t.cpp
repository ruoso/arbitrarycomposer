// MODEL damage, interactively, through the operator graph
// (`runtime.operator_model_damage_routing`). Doc 13:181-184's `$ref` mechanism lets a
// document's `contents` table hold shared content referenced from any operator's
// `inputs` slot -- one graded clip feeding a crossfade in three places -- and doc
// 05:141-144 promises that "an edit deep inside a shared component invalidates every
// place it appears, at every viewport, at the right screen rectangles".
//
// `map_damage_to_device` matches damage against LAYER ROOTS only
// (`damage_planning.cpp:39`: `layer.content == d.object`), and a `$ref`'d content that
// is ONLY an operator input is no layer. So an edit to it -- which the model publishes
// as `Damage{its own model id, ...}` (`model.cpp:1567-1579`) -- used to match nothing:
// the frame's dirty region came back empty, the no-damage early-out fired, and the frame
// that should have repainted the operator never happened. Stale pixels survived the
// edit: the under-approximation doc 13:124-128 calls a correctness bug outright.
//
// The driver now routes model damage through `route_operator_damage` before it reaches
// either `map_damage_to_device` or `invalidate_damage`, resolving the damaged model id
// through the `ContentResolver` (NOT the inverse pull-identity map -- different id
// spaces), and additionally emits the damaged input's PULL identity, because that is the
// key its cached tiles actually live under (doc 13:145-149).
//
// This file is that path's regression test. Strip the routing and the first frame-2
// assertion below fails: zero composites, a stale `target`.

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/cache/invalidation.hpp>
#include <arbc/cache/keyed_store.hpp>
#include <arbc/compositor/compositor.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/kind_crossfade/crossfade_content.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/media/pixel_format.hpp>
#include <arbc/model/damage.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/interactive.hpp>
#include <arbc/runtime/offline_sequence.hpp>
#include <arbc/runtime/pull_identity.hpp>
#include <arbc/surface/surface.hpp>
#include <arbc/surface/surface_pool.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
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

constexpr int k_dim = 512; // a 2x2 grid of rung-0 tiles at scale 1.0
constexpr auto k_budget = std::chrono::milliseconds(16);
constexpr Rgba k_from_color{0.5F, 0.25F, 0.125F, 1.0F};
constexpr Rgba k_edited_color{0.75F, 0.5F, 0.25F, 1.0F};
constexpr Rgba k_to_color{0.125F, 0.375F, 0.75F, 1.0F};
constexpr Rgba k_stranger_color{0.25F, 0.25F, 0.25F, 1.0F};

// The finite damage rect an edit confined to one corner produces (a raster paint
// stroke's shape): entirely inside the top-left rung-0 tile, so exactly one tile of the
// 2x2 grid is re-planned. `14-data-model-and-editing#damage-carries-region-and-time` is
// the model-side guarantee that a commit carries this rect through intact.
constexpr Rect k_stroke{16.0, 16.0, 64.0, 64.0};

Rect canvas() { return Rect{0.0, 0.0, static_cast<double>(k_dim), static_cast<double>(k_dim)}; }
Viewport viewport() { return Viewport{k_dim, k_dim, Affine::identity()}; }

// A crossfade over [1000, 2000). Every frame here renders at `k_when` == 500, strictly
// before the window, so `w == 0`: the crossfade is an identity pass-through to input 0
// and the compositor's identity branch PULLS `from` (caching its tiles under `from`'s
// pull identity) rather than rendering the operator.
CrossfadeParams window_params() {
  return CrossfadeParams{CrossfadeShape::Linear, Time{1000}, Time{1000}};
}
constexpr Time k_when{500};

// The fake clock puts every deadline instant in the real past, so a frame never blocks
// and the deadline path is deterministic (doc 16:54-62).
InteractiveRenderer::Clock epoch_clock() {
  return [] { return std::chrono::steady_clock::time_point{}; };
}

// A solid whose color the test can edit -- the `contents`-table content standing in for
// doc 13:181-184's shared `$ref`'d component. Its pixels live outside the model (a solid
// holds no `Editable` facet), so the test makes both halves of an edit explicit: mutate
// the content here, and publish the version + `Damage` record the commit would have.
class MutableSolid final : public Content {
public:
  MutableSolid(Rgba color, Rect bounds)
      : d_bounds(bounds), d_solid(std::make_unique<SolidContent>(color, bounds)) {}

  void set_color(Rgba color) { d_solid = std::make_unique<SolidContent>(color, d_bounds); }

  std::optional<Rect> bounds() const override { return d_solid->bounds(); }
  Stability stability() const override { return d_solid->stability(); }
  std::optional<TimeRange> time_extent() const override { return d_solid->time_extent(); }
  std::optional<RenderResult> render(const RenderRequest& request,
                                     std::shared_ptr<RenderCompletion> done) override {
    return d_solid->render(request, std::move(done));
  }

private:
  Rect d_bounds;
  std::unique_ptr<SolidContent> d_solid;
};

// A one-input operator that RENDERS (it is not an identity pass-through), so its own
// output tiles land in the cache under its layer id -- the second key an edit to its
// input has to drop. It paints by delegating straight to its input rather than pulling
// through a `PullService`, because `bind_operators` does not yet run on the interactive
// path (`runtime.interactive_binder_wiring`) and an unattached `CrossfadeContent` cannot
// render there. It leaves `map_input_damage` at the contract default (the identity,
// `content.cpp:11`), which is exactly what a pixel-local effect reports.
class DelegatingOperator final : public Content {
public:
  explicit DelegatingOperator(ContentRef input) : d_inputs{input} {}

  std::optional<Rect> bounds() const override { return d_inputs[0]->bounds(); }
  Stability stability() const override { return d_inputs[0]->stability(); }
  std::optional<TimeRange> time_extent() const override { return d_inputs[0]->time_extent(); }
  std::span<const ContentRef> inputs() const override { return d_inputs; }
  std::optional<RenderResult> render(const RenderRequest& request,
                                     std::shared_ptr<RenderCompletion> done) override {
    return d_inputs[0]->render(request, std::move(done));
  }

private:
  std::array<ContentRef, 1> d_inputs;
};

// The scene doc 13:181-184 describes: two operator layers over ONE shared `contents`-
// table input, plus a `contents`-table content nothing reaches.
//
//   from (F)  -- a layer? NO. Only an operator input, under a SYNTHESIZED pull identity.
//   to   (T)  -- likewise.
//   xfade(C)  -- LAYER. inputs (F, T). At `k_when` it is an identity endpoint onto F,
//                so the compositor pulls F and caches F's tiles under F's pull identity.
//   deleg(D)  -- LAYER. input (F). Renders, so ITS output tiles cache under D's model id.
//   stranger(U) -- a `contents`-table content reached by no operator and placed as no
//                  layer: the edit that must route to nothing.
struct Fixture {
  Document doc;
  MutableSolid* from{nullptr};
  ObjectId from_id;
  ObjectId deleg_id;
  ObjectId stranger_id;
  ObjectId xfade_layer;

  Fixture() {
    from_id = doc.add_content(std::make_shared<MutableSolid>(k_from_color, canvas()));
    const ObjectId to_id = doc.add_content(std::make_shared<SolidContent>(k_to_color, canvas()));
    // Constraint 7: the operator's `inputs()` edge must be the SAME `Content*` the
    // `ContentResolver` hands back, or routing's pointer-identity walk finds nothing --
    // exactly as `bind_operators` builds its resolver from `document.resolve(id)`.
    ContentRef from_ref = doc.resolve(from_id);
    ContentRef to_ref = doc.resolve(to_id);
    const ObjectId xfade_id =
        doc.add_content(std::make_shared<CrossfadeContent>(from_ref, to_ref, window_params()));
    deleg_id = doc.add_content(std::make_shared<DelegatingOperator>(from_ref));
    xfade_layer = doc.add_layer(xfade_id, Affine::identity());
    doc.add_layer(deleg_id, Affine::identity());
    stranger_id = doc.add_content(std::make_shared<SolidContent>(k_stranger_color, canvas()));
    from = static_cast<MutableSolid*>(from_ref);
  }
};

// `from`'s cache identity: a SYNTHESIZED id, not its model id -- `build_pull_identity_map`
// seeds only layer roots under their model `ObjectId` (`pull_identity.cpp:22-33`) and
// `from` is not one.
ObjectId pull_identity(const Document& doc, const DocRoot& state, ObjectId content) {
  const ContentResolver resolve = [&doc](ObjectId id) { return doc.resolve(id); };
  const std::shared_ptr<const PullIdentityMap> ids = build_pull_identity_map(state, resolve);
  const auto it = ids->find(doc.resolve(content));
  REQUIRE(it != ids->end());
  return it->second;
}

// Publish the version an edit's commit would publish. A `MutableSolid` holds no model
// state, so the revision bump rides a no-op re-set of a layer transform -- the same
// trivial-commit idiom `src/runtime/t/interactive.t.cpp` uses.
void commit_edit(Fixture& f, Rgba color) {
  f.from->set_color(color);
  f.doc.set_layer_transform(f.xfade_layer, Affine::identity());
}

std::vector<float> snapshot(const Surface& surface) {
  const std::span<const float> px = std::as_const(surface).span<PixelFormat::Rgba32fLinearPremul>();
  return {px.begin(), px.end()};
}

bool byte_identical(const std::vector<float>& a, const std::vector<float>& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

// Byte-compare the two snapshots over the half-open device rect [x0,x1) x [y0,y1).
bool region_identical(const std::vector<float>& a, const std::vector<float>& b, int x0, int y0,
                      int x1, int y1) {
  REQUIRE(a.size() == b.size());
  for (int y = y0; y < y1; ++y) {
    const std::size_t row = static_cast<std::size_t>(y) * static_cast<std::size_t>(k_dim) * 4U;
    const std::size_t lo = row + static_cast<std::size_t>(x0) * 4U;
    const std::size_t hi = row + static_cast<std::size_t>(x1) * 4U;
    if (std::memcmp(a.data() + lo, b.data() + lo, (hi - lo) * sizeof(float)) != 0) {
      return false;
    }
  }
  return true;
}

// The oracle: the same document at the same instant through the EXPORT driver, which
// plans the whole viewport every frame and needs no damage at all (Decision 5 -- which is
// exactly why it needs no code change). The damage-gated interactive frame must converge
// to it byte-for-byte ("two drivers over the same core", doc 02:40-41).
std::vector<float> synchronous_reference(Document& doc, Backend& backend) {
  SequenceRenderer renderer(doc, viewport(), backend);
  const expected<std::unique_ptr<Surface>, SurfaceError> frame = renderer.render_frame_at(k_when);
  REQUIRE(frame.has_value());
  return snapshot(**frame);
}

} // namespace

// enforces: 05-recursive-composition#operator-damage-routes-through-map-input-damage
// enforces: 02-architecture#damage-maps-to-device-dirty-regions
// enforces: 02-architecture#damage-invalidates-by-content-region-across-rungs
TEST_CASE("interactive: an edit to an operator's $ref input repaints every operator layer "
          "that reaches it") {
  CpuBackend backend;
  Fixture f;
  const ContentResolver resolve = [&f](ObjectId id) { return f.doc.resolve(id); };
  TileCache cache(64U * 1024 * 1024);
  SurfacePool pool(backend);
  InteractiveRenderer renderer({}, epoch_clock());

  const DocStatePtr before = f.doc.pin();
  const expected<std::unique_ptr<Surface>, SurfaceError> target =
      backend.make_surface(k_dim, k_dim, before->working_space());
  REQUIRE(target.has_value());

  // --- Frame 1: the whole viewport, cold. -------------------------------------
  renderer.render_frame(*before, resolve, viewport(), cache, backend, pool, **target, {}, k_when,
                        k_budget);
  const std::vector<float> stale = snapshot(**target);
  const std::uint64_t requests_1 = renderer.counters().requests_issued();
  const std::uint64_t composites_1 = renderer.counters().composites();
  CHECK(composites_1 > 0U);

  // --- The edit. --------------------------------------------------------------
  // `from` is in the `contents` table and is NOT a layer, so the damage the commit
  // flushes names an id that matches no `LayerRecord`. Structural, whole-object damage
  // (`Rect::infinite()`, `TimeRange::all()`) -- what every model mutator emits
  // (`model.cpp:822-823`).
  commit_edit(f, k_edited_color);
  const DocStatePtr after = f.doc.pin();
  REQUIRE(after->revision() != before->revision());
  const ObjectId from_pull = pull_identity(f.doc, *after, f.from_id);
  REQUIRE_FALSE(from_pull == f.from_id); // a SYNTHESIZED id: `from` is no layer root
  const std::vector<Damage> edit{Damage{f.from_id, Rect::infinite(), TimeRange::all()}};

  // --- Frame 2: the routed edit repaints both operator layers. -----------------
  // Unrouted, `Damage{from_id, ...}` maps to zero device rects, the early-out fires, and
  // this frame does not happen at all -- `composites` delta 0 and a `target` still
  // showing the pre-edit pixels. THIS is the regression the file exists for.
  renderer.render_frame(*after, resolve, viewport(), cache, backend, pool, **target, edit, k_when,
                        k_budget);
  const std::vector<float> repainted = snapshot(**target);

  CHECK(renderer.counters().composites() > composites_1);    // the frame happened...
  CHECK(renderer.counters().requests_issued() > requests_1); // ...and re-rendered misses
  CHECK_FALSE(byte_identical(stale, repainted));             // the stale pixels are gone
  // Structural infinite damage routes to non-finite damage on each operator layer, which
  // `map_damage_to_device` takes to the WHOLE-VIEWPORT branch (`damage_planning.cpp:43-47`)
  // -- never clamped to `bounds()`, never NaN-degenerate into an empty region. So every
  // pixel repaints, and the damage-gated interactive frame lands byte-exact on the export
  // driver's whole-viewport render of the same document at the same instant.
  CHECK(byte_identical(repainted, synchronous_reference(f.doc, backend)));

  // --- The edited input's tiles were dropped under its PULL identity. -----------
  // `from`'s tiles cache under `from_pull`, so the model-id record drops nothing; only
  // the pull-identity record the router adds does. After the frame exactly ONE tile per
  // rung-0 grid cell is resident under each id -- the freshly rendered one. Without the
  // pull-identity arm the superseded-revision tiles are still resident (unreachable by
  // key, but never reclaimed), and this count comes back doubled.
  CHECK(cache::invalidate_content(cache, from_pull) == 4U);
  // The reaching operator's OWN output tiles drop the same way, under the operator's id
  // (doc 13:145-149's second cache level): the routed `Damage{deleg_id, ...}` record is
  // what drops them, and again exactly the four freshly rendered ones remain.
  CHECK(cache::invalidate_content(cache, f.deleg_id) == 4U);
}

// enforces: 03-layer-plugin-interface#operator-damage-covers
// enforces: 03-layer-plugin-interface#undamaged-regions-stable
TEST_CASE("interactive: a finite input-damage rect maps through map_input_damage, covering") {
  CpuBackend backend;
  Fixture f;
  const ContentResolver resolve = [&f](ObjectId id) { return f.doc.resolve(id); };
  TileCache cache(64U * 1024 * 1024);
  SurfacePool pool(backend);
  InteractiveRenderer renderer({}, epoch_clock());

  const DocStatePtr before = f.doc.pin();
  const expected<std::unique_ptr<Surface>, SurfaceError> target =
      backend.make_surface(k_dim, k_dim, before->working_space());
  REQUIRE(target.has_value());

  renderer.render_frame(*before, resolve, viewport(), cache, backend, pool, **target, {}, k_when,
                        k_budget);
  const std::vector<float> stale = snapshot(**target);
  const std::uint64_t composites_1 = renderer.counters().composites();

  // A finite edit rect confined to the top-left tile. Both operators report the contract
  // default `map_input_damage` (the identity), so the composed image of `k_stroke` is
  // `k_stroke` itself and the device dirty region is `k_stroke` clipped to the viewport.
  commit_edit(f, k_edited_color);
  const DocStatePtr after = f.doc.pin();
  const std::vector<Damage> edit{Damage{f.from_id, k_stroke, TimeRange::all()}};
  renderer.render_frame(*after, resolve, viewport(), cache, backend, pool, **target, edit, k_when,
                        k_budget);
  const std::vector<float> repainted = snapshot(**target);

  const std::vector<float> reference = synchronous_reference(f.doc, backend);
  // COVERING: every pixel the input change affects inside the damaged rect re-rendered,
  // byte-exact to the full synchronous export. Under-report even one and this fails.
  CHECK(region_identical(repainted, reference, 16, 16, 64, 64));
  CHECK(renderer.counters().composites() > composites_1);
  // NOT over-approximated to the whole viewport: the damage never reached the bottom-right
  // quadrant, whose tiles were neither invalidated nor re-planned, so its pixels are still
  // frame 1's -- bit-identical, and (since the edit changed `from` everywhere) provably
  // NOT the export's.
  CHECK(region_identical(repainted, stale, 256, 256, 512, 512));
  CHECK_FALSE(region_identical(repainted, reference, 256, 256, 512, 512));
}

// enforces: 02-architecture#interactive-still-scene-schedules-no-frame
TEST_CASE("interactive: an edit no operator reaches routes to nothing") {
  CpuBackend backend;
  Fixture f;
  const ContentResolver resolve = [&f](ObjectId id) { return f.doc.resolve(id); };
  TileCache cache(64U * 1024 * 1024);
  SurfacePool pool(backend);
  InteractiveRenderer renderer({}, epoch_clock());

  const DocStatePtr before = f.doc.pin();
  const expected<std::unique_ptr<Surface>, SurfaceError> target =
      backend.make_surface(k_dim, k_dim, before->working_space());
  REQUIRE(target.has_value());

  renderer.render_frame(*before, resolve, viewport(), cache, backend, pool, **target, {}, k_when,
                        k_budget);
  const std::vector<float> painted = snapshot(**target);
  const std::uint64_t requests = renderer.counters().requests_issued();
  const std::uint64_t composites = renderer.counters().composites();
  const std::uint64_t follow_ups = renderer.counters().follow_up_frames();

  // `stranger` is in the `contents` table, is placed as no layer, and no operator's
  // `inputs()` reaches it. Editing it routes to ZERO operator layers and maps to ZERO
  // device rects -- the guard against "fixing" the under-approximation by
  // over-approximating everything.
  f.doc.set_layer_transform(f.xfade_layer, Affine::identity()); // the commit's revision bump
  const DocStatePtr after = f.doc.pin();
  const std::vector<Damage> edit{Damage{f.stranger_id, Rect::infinite(), TimeRange::all()}};
  const InteractiveRenderer::FrameOutcome out = renderer.render_frame(
      *after, resolve, viewport(), cache, backend, pool, **target, edit, k_when, k_budget);

  CHECK_FALSE(out.schedule_follow_up);
  CHECK(renderer.counters().requests_issued() == requests);    // delta 0
  CHECK(renderer.counters().composites() == composites);       // delta 0
  CHECK(renderer.counters().follow_up_frames() == follow_ups); // delta 0
  CHECK(byte_identical(painted, snapshot(**target)));          // the frame did not happen
}
