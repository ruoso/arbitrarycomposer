// Synthesized pull identities live in a namespace DISJOINT from every model ObjectId
// (`runtime.pull_identity_disjoint_ids`, doc 14 § Identity).
//
// The bug this file pins: `build_pull_identity_map` used to seed synthesized child ids
// from `1 + max(layer-root ObjectId)`. That is disjoint from the LAYER ROOTS -- and from
// nothing else. The model allocates ids to layer RECORDS and to `contents`-table entries
// that are not layers (doc 13:181-184's `$ref` targets) from the same monotonic counter,
// so a synthesized child id could equal a real model id.
//
// That was destructive, not merely confusing. `invalidate_damage`
// (`damage_planning.cpp:84-100`) funnels into `cache::invalidate_content` /
// `invalidate_region`, whose predicates match on `key.content` and NOTHING ELSE --
// revision-agnostic, rung-agnostic, time-agnostic (doc 02:94-95). So damaging a
// `contents`-table content `C` also dropped every tile an unrelated operator input child
// happened to hold under the same 64-bit number, at every revision, wholesale. A sound
// over-approximation (a spurious drop, never a stale read) -- which is exactly why it was
// silent: the only symptom was unexplained re-render work in the documents where
// operators and shared `$ref` content coexist.
//
// The fix is structural: the top bit of the id space is reserved. The model allocator
// issues only bit-63-clear ids; the runtime mints synthesized ids with bit 63 set. The
// halves cannot meet, whatever the allocation order.
//
// The document below is shaped to provoke the OLD collision exactly: `C`'s model id
// equals the id the old seed would have synthesized for the INLINE child `X`. The test
// asserts that precondition explicitly (so it cannot quietly stop testing anything), then
// proves behaviorally, with a cache eviction count, that damaging `C` no longer touches
// `X`'s tiles.

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/cache/invalidation.hpp>
#include <arbc/cache/keyed_store.hpp>
#include <arbc/compositor/compositor.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/contract/registry.hpp>
#include <arbc/kind_crossfade/crossfade_content.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/media/pixel_format.hpp>
#include <arbc/model/damage.hpp>
#include <arbc/model/records.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/document_serialize.hpp>
#include <arbc/runtime/interactive.hpp>
#include <arbc/runtime/pull_identity.hpp>
#include <arbc/surface/surface.hpp>
#include <arbc/surface/surface_pool.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace arbc;

namespace {

constexpr int k_dim = 512; // a 2x2 grid of rung-0 tiles at scale 1.0
constexpr auto k_budget = std::chrono::milliseconds(16);
constexpr Rgba k_c_color{0.5F, 0.25F, 0.125F, 1.0F};
constexpr Rgba k_c_edited{0.75F, 0.5F, 0.25F, 1.0F};
constexpr Rgba k_x_color{0.125F, 0.375F, 0.75F, 1.0F};
constexpr Rgba k_dummy_color{0.25F, 0.25F, 0.25F, 1.0F};

Rect canvas() { return Rect{0.0, 0.0, static_cast<double>(k_dim), static_cast<double>(k_dim)}; }
Viewport viewport() { return Viewport{k_dim, k_dim, Affine::identity()}; }

// A crossfade whose window starts after `k_when`, so `w == 0` and the operator is an
// identity pass-through to input 0: the compositor PULLS input 0 and caches its tiles
// under input 0's pull identity, rather than rendering the operator. That is what puts
// tiles in the cache under a synthesized id at all.
CrossfadeParams window_params() {
  return CrossfadeParams{CrossfadeShape::Linear, Time{1000}, Time{1000}};
}
constexpr Time k_when{500};
constexpr const char* k_version = "1.0";

InteractiveRenderer::Clock epoch_clock() {
  return [] { return std::chrono::steady_clock::time_point{}; };
}

// A solid whose color the test can edit (a solid holds no `Editable` facet, so the test
// makes both halves of an edit explicit: mutate here, publish the version + `Damage`).
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

// The collision scene.
//
//   C        -- a `contents`-table content (a real model `ObjectId`), reached ONLY as an
//               operator input, never placed as a layer: doc 13:181-184's `$ref` target.
//               Added to the document AFTER the operator layers, so its model id lands in
//               the range the OLD seed (`1 + max(layer-root id)`) synthesized from.
//   X        -- an INLINE input child: owned by its operator, never in the `contents`
//               table, so it has no model id and must be synthesized one.
//   op_c     -- LAYER. Crossfade(C, dummy_c). At `k_when` an identity endpoint onto C, so
//               the compositor caches C's tiles under C's pull identity.
//   op_x     -- LAYER. Crossfade(X, dummy_x). Likewise for X.
//
// Under the OLD seed X's synthesized id came out EQUAL to C's model id, so damaging C
// evicted X's tiles. `collides()` below asserts that precondition still holds of the
// shape, independently of the fix.
struct Fixture {
  SolidContent dummy_c{k_dummy_color, canvas()};
  SolidContent dummy_x{k_dummy_color, canvas()};
  SolidContent x_inline{k_x_color, canvas()};
  std::shared_ptr<MutableSolid> c = std::make_shared<MutableSolid>(k_c_color, canvas());

  Document doc;
  std::vector<ObjectId> model_ids; // every ObjectId the model allocated
  ObjectId op_c_id;
  ObjectId op_x_id;
  ObjectId c_id; // C's MODEL id
  ObjectId op_c_layer;

  Fixture() {
    auto op_c = std::make_shared<CrossfadeContent>(c.get(), &dummy_c, window_params());
    auto op_x = std::make_shared<CrossfadeContent>(&x_inline, &dummy_x, window_params());

    op_c_id = doc.add_content(op_c);
    model_ids.push_back(op_c_id);
    op_x_id = doc.add_content(op_x);
    model_ids.push_back(op_x_id);
    // `op_x`'s layer is added FIRST so the identity walk reaches op_c's children before
    // op_x's: that ordering is what made the old seed land X's id exactly on C's model id.
    model_ids.push_back(doc.add_layer(op_x_id, Affine::identity()));
    op_c_layer = doc.add_layer(op_c_id, Affine::identity());
    model_ids.push_back(op_c_layer);
    // C enters the `contents` table LAST -- it is a shared component, not a layer.
    c_id = doc.add_content(c);
    model_ids.push_back(c_id);
  }

  ContentResolver resolver() {
    return [this](ObjectId id) { return doc.resolve(id); };
  }
};

std::shared_ptr<const PullIdentityMap> identities(Fixture& f, const DocRoot& state) {
  return build_pull_identity_map(state, f.resolver());
}

// The synthesized ordinal of `id` (its 1-based position in the walk), recovered from the
// reserved-half bit pattern.
std::uint64_t ordinal_of(ObjectId id) {
  REQUIRE(synthetic(id));
  return id.value & ~kSyntheticIdBit;
}

// A NON-DESTRUCTIVE resident-tile count for one content id. `KeyedStore` exposes no
// size()/iterator, only `remove_if`, so counting means a predicate that inspects every
// live key and removes nothing. This is the behavioral counter the eviction assertions
// below read (doc 16 tier 4: a counter, never wall-clock) -- and unlike
// `cache::invalidate_content` it can be read twice without perturbing what it measures.
std::size_t tiles_for(TileCache& cache, ObjectId content) {
  std::size_t live = 0;
  cache.remove_if([&](const TileKey& key) {
    if (key.content == content) {
      ++live;
    }
    return false; // remove nothing
  });
  return live;
}

} // namespace

// enforces: 14-data-model-and-editing#synthesized-identities-never-collide-with-model-ids
// enforces: 02-architecture#damage-invalidates-by-content-region-across-rungs
TEST_CASE("interactive: damaging a $ref content cannot evict an inline child's tiles") {
  CpuBackend backend;
  Fixture f;
  const ContentResolver resolve = f.resolver();
  TileCache cache(64U * 1024 * 1024);
  SurfacePool pool(backend);
  InteractiveRenderer renderer({}, epoch_clock());

  const DocStatePtr before = f.doc.pin();
  const auto ids = identities(f, *before);
  const ObjectId c_pull = ids->at(f.doc.resolve(f.c_id));
  const ObjectId x_pull = ids->at(&f.x_inline);

  // --- The id-space guarantee (Constraint 1). ---------------------------------
  // Both inputs are synthesized (neither is a layer root), and NEITHER equals any model
  // ObjectId in the document -- structurally, because they carry the reserved bit that
  // `Model::allocate_id` never sets.
  CHECK(synthetic(c_pull));
  CHECK(synthetic(x_pull));
  CHECK(c_pull != f.c_id);
  for (const ObjectId mid : f.model_ids) {
    CHECK_FALSE(synthetic(mid));
    CHECK(mid != c_pull);
    CHECK(mid != x_pull);
  }

  // --- The precondition: this shape really does provoke the OLD bug. -----------
  // The old seed was `1 + max(layer-root ObjectId)`, handing out consecutive ids in walk
  // order. Reconstruct what X would have been given, and check it collides with C's model
  // id. If this ever stops holding, the document has drifted and the eviction assertion
  // below has quietly stopped testing the regression.
  const std::uint64_t old_seed = std::max(f.op_c_id.value, f.op_x_id.value) + 1;
  const std::uint64_t x_old_id = old_seed + ordinal_of(x_pull) - 1;
  CHECK(x_old_id == f.c_id.value); // X's OLD id == C's model id: the collision

  // --- Frame 1: cold. Tiles land under C's and X's pull identities. ------------
  const auto target = backend.make_surface(k_dim, k_dim, before->working_space());
  REQUIRE(target.has_value());
  renderer.render_frame(*before, resolve, viewport(), cache, backend, pool, **target, {}, k_when,
                        k_budget);
  CHECK(renderer.counters().composites() > 0U);

  // Both operators are identity endpoints onto their input 0, so the compositor pulled
  // both inputs and cached each one's tiles under its own synthesized identity.
  CHECK(tiles_for(cache, c_pull) == 4U);
  CHECK(tiles_for(cache, x_pull) == 4U);

  // --- The edit: damage C, the shared $ref content. ---------------------------
  f.c->set_color(k_c_edited);
  f.doc.set_layer_transform(f.op_c_layer, Affine::identity()); // the commit's revision bump
  const DocStatePtr after = f.doc.pin();
  REQUIRE(after->revision() != before->revision());
  const std::vector<Damage> edit{Damage{f.c_id, Rect::infinite(), TimeRange::all()}};

  // --- Frame 2: the driver routes the damage and invalidates. ------------------
  renderer.render_frame(*after, resolve, viewport(), cache, backend, pool, **target, edit, k_when,
                        k_budget);

  // C's tiles WERE dropped: the router emits C's pull identity beside its model id
  // (`operator_model_damage_routing`), and that is the key its tiles actually live under.
  // Exactly the four freshly rendered tiles remain -- the superseded-revision four were
  // reclaimed, not merely made unreachable.
  CHECK(tiles_for(cache, c_pull) == 4U);

  // X's tiles were NOT: X is a different content, its operator reaches nothing damaged,
  // and its synthesized id carries the reserved bit that C's model id cannot. So X keeps
  // BOTH its frame-1 tiles and the frame-2 re-render's -- eight, none of them evicted.
  //
  // THIS is the regression. Under the old seed `x_pull` was numerically equal to `f.c_id`,
  // so `invalidate_damage`'s revision-agnostic `key.content == content` predicate dropped
  // every one of X's tiles along with C's, and this count came back at 4.
  CHECK(tiles_for(cache, x_pull) == 8U);
}

// enforces: 14-data-model-and-editing#synthesized-identities-never-collide-with-model-ids
TEST_CASE("no synthesized identity escapes into the model or the file format") {
  // Constraint 5 / doc 14 § Identity: synthesized ids are RENDER-TIME state -- keyed by
  // pointer over a pinned revision, never stored in a record, never journaled, never
  // serialized. The serializer assigns `contents`-table entries from the model, not from
  // the pull map, so this holds by construction -- but the round-trip is the only place it
  // could silently break, so it is pinned here rather than assumed.
  //
  // A serializable document (a composition root, interned kinds) whose operator's two
  // inputs are `contents`-table entries reached ONLY as inputs -- never layers. They carry
  // real model ids AND, at render time, synthesized pull identities. Exactly the shape
  // where a leak could happen.
  Document doc;
  KindBridge bridge;
  const ObjectId comp = doc.add_composition(static_cast<double>(k_dim), static_cast<double>(k_dim));

  auto in_a = std::make_shared<SolidContent>(k_c_color, canvas());
  auto in_b = std::make_shared<SolidContent>(k_x_color, canvas());
  const ObjectId a_id = doc.add_content(in_a, bridge.intern(SolidContent::kind_id, k_version));
  const ObjectId b_id = doc.add_content(in_b, bridge.intern(SolidContent::kind_id, k_version));

  auto op = std::make_shared<CrossfadeContent>(in_a.get(), in_b.get(), window_params());
  const ObjectId op_id = doc.add_content(op, bridge.intern(CrossfadeContent::kind_id, k_version));
  const ObjectId layer = doc.add_layer(op_id, Affine::identity(), 1.0);
  doc.attach_layer(comp, layer);

  const DocStatePtr pinned = doc.pin();
  const ContentResolver resolve = [&doc](ObjectId id) { return doc.resolve(id); };

  // The document's own ids are all in the model half (the allocator's side of the contract).
  for (const ObjectId mid : {comp, a_id, b_id, op_id, layer}) {
    CHECK_FALSE(synthetic(mid));
  }

  // Synthesized ids DO exist here -- the two inputs are not layer roots, so the map mints
  // reserved-half ids for them. The round-trip below is therefore not vacuous: there IS
  // something that could leak.
  const auto ids = build_pull_identity_map(*pinned, resolve);
  const ObjectId a_pull = ids->at(in_a.get());
  const ObjectId b_pull = ids->at(in_b.get());
  REQUIRE(synthetic(a_pull));
  REQUIRE(synthetic(b_pull));
  CHECK(a_pull != a_id); // the cache identity is NOT the model id
  CHECK(b_pull != b_id);

  // Save.
  const expected<std::string, SerializeError> saved = save_document(doc, bridge);
  REQUIRE(saved.has_value());

  // Byte level: neither synthesized id's decimal spelling appears anywhere in the file.
  // The serializer draws `contents`-table ids from the model, never from the pull map --
  // this is the assertion that would catch it if that ever changed.
  CHECK(saved->find(std::to_string(a_pull.value)) == std::string::npos);
  CHECK(saved->find(std::to_string(b_pull.value)) == std::string::npos);

  // Reload into a fresh Document: every ObjectId the reader mints is in the model half.
  Document reloaded;
  KindBridge reloaded_bridge;
  const Registry registry;
  REQUIRE(load_document(*saved, reloaded, reloaded_bridge, registry).has_value());

  const DocStatePtr rpin = reloaded.pin();
  ObjectId rcomp;
  const CompositionRecord* comp_rec = nullptr;
  REQUIRE(rpin->find_first_composition(rcomp, comp_rec));
  CHECK_FALSE(synthetic(rcomp));

  std::size_t layers = 0;
  rpin->for_each_layer_in(rcomp, [&](ObjectId lid) {
    ++layers;
    CHECK_FALSE(synthetic(lid)); // the layer record's own id
    const LayerRecord* lr = rpin->find_layer(lid);
    REQUIRE(lr != nullptr);
    CHECK_FALSE(synthetic(lr->content)); // and the content it binds
  });
  CHECK(layers == 1U);

  // The reloaded document re-synthesizes its input identities FROM THE GRAPH -- fresh
  // reserved-half ids, never read from the file. Nothing was persisted; nothing was
  // restored.
  const ContentResolver rresolve = [&reloaded](ObjectId id) { return reloaded.resolve(id); };
  const auto rids = build_pull_identity_map(*rpin, rresolve);
  std::size_t synthesized = 0;
  for (const auto& [content, id] : *rids) {
    CHECK(id.valid());
    if (synthetic(id)) {
      ++synthesized;
    }
  }
  CHECK(synthesized == 2U); // the operator's two inputs, synthesized anew
}
