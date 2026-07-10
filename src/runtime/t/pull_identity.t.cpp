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

#include <functional>
#include <memory>

using namespace arbc;

namespace {

CrossfadeParams linear_params() {
  return CrossfadeParams{CrossfadeShape::Linear, Time{0}, Time{1000}};
}

SolidContent make_solid(float r) {
  return SolidContent{Rgba{r, r, r, 1.0F}, Rect{0.0, 0.0, 2.0, 2.0}};
}

std::function<Content*(ObjectId)> resolver_of(Document& doc) {
  return [&doc](ObjectId id) { return doc.resolve(id); };
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
