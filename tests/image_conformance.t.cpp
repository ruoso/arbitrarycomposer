// Contract conformance driver for org.arbc.image (doc 16: a content kind runs the public
// arbc-testing suite; kinds/image.md Acceptance "Conformance suite"). Cross-component (it
// pulls the plugin's impl archive AND arbc-testing), so it lives in tests/ -- and it must,
// because `check_claims.py` scans only src/, tests/ and testing/, never plugins/.
//
// `snapshot_sensitive` stays false (matching `raster_conformance.t.cpp`): the suite
// fabricates `StateHandle`s naming no interned version, and `org.arbc.image` ignores them
// entirely -- it is READ-ONLY, so there is no editable state for a snapshot to pin. That is
// not an accident of this driver; it is the kind's defining property, asserted directly
// below.

#include <arbc/contract/content.hpp>
#include <arbc/kind_image/image_content.hpp>
#include <arbc/kind_solid/solid_content.hpp> // the null-default side of external_asset_ref()
#include <arbc/testing/contract_tests.hpp>

#include <catch2/catch_test_macros.hpp>

#include "support/image_fixtures.hpp"

#include <memory>

namespace {

using namespace arbc;
namespace fix = arbc::image::testfix;

testing::ContentFactory image_factory() {
  return []() -> std::unique_ptr<Content> { return fix::make_content(); };
}

} // namespace

// enforces: 03-layer-plugin-interface#render-scale-honest
// enforces: 03-layer-plugin-interface#render-within-declared-bounds
// enforces: 03-layer-plugin-interface#render-pure-over-pinned-state
// enforces: 03-layer-plugin-interface#capture-restore-roundtrip
// enforces: 03-layer-plugin-interface#facet-consistency
// enforces: 03-layer-plugin-interface#leaf-content-has-no-operator-graph
// enforces: 03-layer-plugin-interface#static-time-invariant
TEST_CASE("org.arbc.image passes the contract conformance suite") {
  arbc::contract_tests(image_factory());
}

// The load-bearing omission (doc 03:256-268). `org.arbc.image` and `org.arbc.raster` differ
// in exactly two ways: raster is codec-free and EDITABLE, image is codec-carrying and
// READ-ONLY. That second difference is what makes non-destructive editing STRUCTURAL rather
// than conventional -- you cannot paint on a photograph, so retouching one MUST stack an
// editable `org.arbc.raster` above a referenced `org.arbc.image`. `editable()` returning
// nullptr is not an omission to be "fixed" later; it is the property.
//
// enforces: 03-layer-plugin-interface#image-has-no-editable-facet
TEST_CASE("org.arbc.image exposes no Editable facet, so retouching must stack a raster") {
  const std::unique_ptr<arbc::image::ImageContent> content = fix::make_content();

  REQUIRE(content->editable() == nullptr);

  // ...and the rest of the read-only, Static, leaf shape it entails (Constraint 3): it is
  // time-invariant, silent, and a graph leaf, so it adds no time dimension to the cache key
  // and owns no operator inputs.
  CHECK(content->stability() == arbc::Stability::Static);
  CHECK(content->time_extent() == std::nullopt);
  CHECK(content->quantize_time(arbc::Time{12345}) == std::nullopt);
  CHECK(content->audio() == nullptr);
  CHECK(content->inputs().empty());
  CHECK_FALSE(content->composition_ref().valid());

  // Read-only implies immutable implies a pure render -- which is what lets the core compute
  // this leaf on worker threads (Decision 4, doc 00:203).
  CHECK(content->render_thread_safe());

  // `external_asset_ref()` is the one accessor this kind adds to the contract (Decision 3),
  // and it is NULL-DEFAULTED: empty means "references no external asset", which is the answer
  // for every kind but this one. So no existing kind changed shape to make room for it.
  CHECK(content->external_asset_ref() == "assets/photo.ppm");
  const arbc::SolidContent solid{arbc::Rgba{1.0F, 0.0F, 0.0F, 1.0F}};
  CHECK(solid.external_asset_ref().empty());
  // ...and it stays distinct from the composition ref: an asset and a child composition are
  // different targets, and conflating them would make the nested codec's "a body naming both
  // is malformed" check incoherent.
  CHECK(content->external_composition_ref().empty());
}
