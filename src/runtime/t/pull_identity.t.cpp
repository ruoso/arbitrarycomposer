// Direct pin for the offline/export driver's `PullConfig::id_of` reverse map
// (runtime.operator_input_cache_identity): the map must give every operator input
// child a distinct, stable cache identity so two same-stability inputs of one
// operator do not both fall to `ObjectId{}` and alias one cache key. Before this
// seam the driver seeded the map from layer roots only, so a crossfade's `from`/`to`
// -- `ObjectId`-less owned children reached solely through `Content::inputs()` --
// both mapped to the default `ObjectId{}` and collided (crossfade_runtime_binding.md
// deferral). This test builds the driver map over a real crossfade Document and pins
// the invariant WITHOUT a backend, so it stays a runtime-component test (doc 17: a
// runtime test may not include backend_cpu). The byte-exact end-to-end proof lives
// in tests/crossfade_offline_dissolve.t.cpp.

#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/kind_crossfade/crossfade_content.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/pull_identity.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

using namespace arbc;

namespace {

CrossfadeParams linear_params() {
  return CrossfadeParams{CrossfadeShape::Linear, Time{0}, Time{1000}};
}

SolidContent make_solid(float r) {
  return SolidContent{Rgba{r, r, r, 1.0F}, Rect{0.0, 0.0, 2.0, 2.0}};
}

// `Content` is neither copyable nor movable, so a heap solid must be constructed in
// place (a `contents`-table entry is owned by the Document via `shared_ptr`).
std::shared_ptr<SolidContent> make_shared_solid(float r) {
  return std::make_shared<SolidContent>(Rgba{r, r, r, 1.0F}, Rect{0.0, 0.0, 2.0, 2.0});
}

std::function<Content*(ObjectId)> resolver_of(Document& doc) {
  return [&doc](ObjectId id) { return doc.resolve(id); };
}

bool map_holds_value(const PullIdentityMap& map, ObjectId id) {
  return std::any_of(map.begin(), map.end(),
                     [id](const auto& entry) { return entry.second == id; });
}

} // namespace

// enforces: 13-effects-as-operators#operator-input-children-have-distinct-cache-identity
TEST_CASE("the offline id_of gives a crossfade's two input children distinct, non-sentinel ids") {
  // Two DISTINCT same-stability (Static solid) inputs of one crossfade: the exact
  // collision case. Only the crossfade is a layer root (add_content + add_layer); its
  // `from`/`to` are borrowed owned children with no ObjectId.
  SolidContent from = make_solid(0.25F);
  SolidContent to = make_solid(0.75F);
  auto xf = std::make_shared<CrossfadeContent>(&from, &to, linear_params());

  Document doc;
  const ObjectId cid = doc.add_content(xf);
  doc.add_layer(cid, Affine::identity());
  const auto pinned = doc.pin();

  const std::shared_ptr<const PullIdentityMap> map =
      build_pull_identity_map(*pinned, resolver_of(doc));
  const auto id_of = [&map](const Content* c) {
    const auto it = map->find(c);
    return it != map->end() ? it->second : ObjectId{};
  };

  // Distinctness (the invariant whose violation is the bug): the two children key
  // apart, neither on the reserved `ObjectId{}` sentinel, and both apart from the
  // enclosing layer-root id.
  CHECK(id_of(&from) != id_of(&to));
  CHECK(id_of(&from) != ObjectId{});
  CHECK(id_of(&to) != ObjectId{});
  CHECK(id_of(&from) != cid);
  CHECK(id_of(&to) != cid);
  // The layer root keeps its model id (only children are synthesized).
  CHECK(id_of(xf.get()) == cid);

  // Cross-frame stability: rebuilding the map over the immutable pinned graph yields
  // the same id for each child (a deterministic walk), so a Static input's tiles
  // survive clock advance across the frames of a render sequence.
  const std::shared_ptr<const PullIdentityMap> again =
      build_pull_identity_map(*pinned, resolver_of(doc));
  CHECK(again->at(&from) == map->at(&from));
  CHECK(again->at(&to) == map->at(&to));
}

// enforces: 13-effects-as-operators#operator-input-children-have-distinct-cache-identity
TEST_CASE("the offline id_of keys a shared input child under one identity for every consumer") {
  // A single content reached as an input from TWO different operator parents must key
  // under ONE identity (doc 13:141-154, "shared by every consumer"). The map is keyed
  // by `Content*` pointer identity and emplaced once, so the shared child appears with
  // a single id regardless of how many parents reach it.
  SolidContent shared = make_solid(0.5F);
  SolidContent other_a = make_solid(0.1F);
  SolidContent other_b = make_solid(0.9F);
  auto xf_a = std::make_shared<CrossfadeContent>(&shared, &other_a, linear_params());
  auto xf_b = std::make_shared<CrossfadeContent>(&other_b, &shared, linear_params());

  Document doc;
  doc.add_layer(doc.add_content(xf_a), Affine::identity());
  doc.add_layer(doc.add_content(xf_b), Affine::identity());
  const auto pinned = doc.pin();

  const std::shared_ptr<const PullIdentityMap> map =
      build_pull_identity_map(*pinned, resolver_of(doc));

  // One entry for the shared child, distinct from every other reachable node.
  REQUIRE(map->find(&shared) != map->end());
  const ObjectId shared_id = map->at(&shared);
  CHECK(shared_id != ObjectId{});
  CHECK(shared_id != map->at(&other_a));
  CHECK(shared_id != map->at(&other_b));
  // Every mapped node is distinct (no aliasing across the whole graph).
  CHECK(map->at(&other_a) != map->at(&other_b));
}

// enforces: 13-effects-as-operators#operator-input-children-have-distinct-cache-identity
// enforces: 14-data-model-and-editing#synthesized-identities-never-collide-with-model-ids
TEST_CASE("no synthesized pull identity equals any model ObjectId in the document") {
  // The regression shape (`runtime.pull_identity_disjoint_ids`). The OLD seed drew
  // synthesized ids from `1 + max(layer-root id)`, which is disjoint from the layer
  // roots and from NOTHING ELSE -- while the model allocates ids to layer RECORDS and
  // to `contents`-table entries that are not layers (doc 13:181-184's `$ref` targets)
  // from the same monotonic counter. So a synthesized child id could equal a real
  // model id, and because cache invalidation matches on `key.content` alone
  // (doc 02:94-95, revision-agnostic), damage naming that model id silently dropped an
  // unrelated content's tiles.
  //
  // This document provokes it: two operator layer roots, a `contents`-table content
  // reached ONLY as an operator input (never a layer), and inline input children.
  SolidContent inline_a = make_solid(0.1F);
  SolidContent inline_b = make_solid(0.2F);
  SolidContent inline_c = make_solid(0.3F);
  // `C`: a real model object with a real `ObjectId`, consumed only as an operator
  // input -- it is in the `contents` table but is NOT a layer root, so the identity
  // map never seeds it and it takes a synthesized id.
  auto shared_ref = make_shared_solid(0.5F);

  auto xf_ref = std::make_shared<CrossfadeContent>(shared_ref.get(), &inline_c, linear_params());
  auto xf_inline = std::make_shared<CrossfadeContent>(&inline_a, &inline_b, linear_params());

  Document doc;
  std::vector<ObjectId> model_ids; // every ObjectId the model allocates below
  const ObjectId op_ref = doc.add_content(xf_ref);
  model_ids.push_back(op_ref);
  model_ids.push_back(doc.add_layer(op_ref, Affine::identity()));
  const ObjectId op_inline = doc.add_content(xf_inline);
  model_ids.push_back(op_inline);
  model_ids.push_back(doc.add_layer(op_inline, Affine::identity()));
  const ObjectId ref_id = doc.add_content(shared_ref); // contents-table, never a layer
  model_ids.push_back(ref_id);

  const auto pinned = doc.pin();
  const std::shared_ptr<const PullIdentityMap> map =
      build_pull_identity_map(*pinned, resolver_of(doc));

  // The precondition that makes the OLD seed a bug: `1 + max(layer-root id)` is itself
  // a live model `ObjectId` in this document (layer records and the `$ref` content draw
  // from the same counter). So the old scheme's FIRST synthesized id collided with a
  // real model object. If this ever stops holding, the shape below no longer provokes
  // the regression and the test has quietly stopped testing anything.
  const std::uint64_t old_seed = std::max(op_ref.value, op_inline.value) + 1;
  CHECK(map_holds_value(*map, ObjectId{old_seed}) == false);
  CHECK(std::any_of(model_ids.begin(), model_ids.end(),
                    [old_seed](ObjectId m) { return m.value == old_seed; }));

  // A1: every value in the map is either a layer root's real model id (bit 63 clear)
  // or a synthesized id (bit 63 set). There is no third case.
  for (const auto& [content, id] : *map) {
    CHECK(id.valid());
    if (!synthetic(id)) {
      CHECK((id == op_ref || id == op_inline));
    }
  }

  // A2, the executable statement of Constraint 1: intersect the map's values with
  // EVERY ObjectId the model allocated. The intersection must be exactly the layer
  // roots -- the contents that genuinely are model objects and key under their real
  // identity. Every other map value is synthesized and matches no model id at all.
  // This fails loudly if the seed is ever reverted to a high-water mark.
  for (const ObjectId mid : model_ids) {
    const bool is_layer_root = (mid == op_ref || mid == op_inline);
    CHECK(map_holds_value(*map, mid) == is_layer_root);
  }

  // `C` in particular: it has a model id, but its CACHE identity is synthesized and
  // differs from it -- so damage naming `ref_id` cannot reach a synthesized key, and
  // the model id looked up in the inverse pull map is a guaranteed miss, never an
  // alias (`interactive.cpp` `route_model_damage`; Decision 4).
  REQUIRE(map->find(shared_ref.get()) != map->end());
  const ObjectId ref_pull_id = map->at(shared_ref.get());
  CHECK(synthetic(ref_pull_id));
  CHECK(ref_pull_id != ref_id);

  // The layer roots keep their real model ids (Constraint 4).
  CHECK(map->at(xf_ref.get()) == op_ref);
  CHECK(map->at(xf_inline.get()) == op_inline);
  CHECK(!synthetic(op_ref));
  CHECK(!synthetic(op_inline));

  // The inline children are synthesized, distinct, and valid (Constraints 3 and 4).
  CHECK(synthetic(map->at(&inline_a)));
  CHECK(synthetic(map->at(&inline_b)));
  CHECK(synthetic(map->at(&inline_c)));
  CHECK(map->at(&inline_a) != map->at(&inline_b));
  CHECK(map->at(&inline_a).valid());

  // Cross-frame stability survives the new base (Constraint 4 / predecessor's
  // Constraint 3): the walk is unchanged, only the counter's base moved.
  const std::shared_ptr<const PullIdentityMap> again =
      build_pull_identity_map(*pinned, resolver_of(doc));
  CHECK(again->at(&inline_a) == map->at(&inline_a));
  CHECK(again->at(shared_ref.get()) == ref_pull_id);
}
