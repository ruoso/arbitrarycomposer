#include <arbc/model/model.hpp>

#include <catch2/catch_test_macros.hpp>

namespace {

// enforces: 14-data-model-and-editing#pinned-version-never-observes-later-edit
TEST_CASE("a pinned version never observes a later edit") {
  arbc::Model model;
  const arbc::DocStatePtr pinned = model.current();

  auto txn = model.transact();
  const arbc::ObjectId content = model.allocate_id();
  const arbc::ObjectId layer = txn.add_layer(content, arbc::Affine::identity());
  REQUIRE(txn.commit().has_value());

  // The version pinned before the commit is the empty map: it never sees the
  // layer the later transaction added.
  REQUIRE(pinned->find_layer(layer) == nullptr);
  REQUIRE_FALSE(pinned->contains(layer));

  // The published version resolves it.
  const arbc::DocStatePtr now = model.current();
  REQUIRE(now->find_layer(layer) != nullptr);
  REQUIRE(now->revision() == pinned->revision() + 1);
}

TEST_CASE("transactions publish atomically with monotonic revisions") {
  arbc::Model model;
  const arbc::ObjectId content = model.allocate_id();

  arbc::ObjectId layer;
  {
    auto txn = model.transact();
    layer = txn.add_layer(content, arbc::Affine::scaling(2.0, 2.0));
    REQUIRE(txn.commit().has_value());
  }
  {
    auto txn = model.transact();
    txn.set_transform(layer, arbc::Affine::translation(1.0, 1.0));
    REQUIRE(txn.commit().has_value());
  }

  const arbc::DocStatePtr state = model.current();
  REQUIRE(state->revision() == 2);
  const arbc::LayerRecord* record = state->find_layer(layer);
  REQUIRE(record != nullptr);
  REQUIRE(record->transform == arbc::Affine::translation(1.0, 1.0));
  REQUIRE(record->content == content);
}

TEST_CASE("content objects round-trip through the versioned map") {
  arbc::Model model;
  arbc::ObjectId content;
  {
    auto txn = model.transact();
    content = txn.add_content(0x51D);
    REQUIRE(txn.commit().has_value());
  }
  const arbc::DocStatePtr state = model.current();
  const arbc::ContentRecord* record = state->find_content(content);
  REQUIRE(record != nullptr);
  REQUIRE(record->kind == 0x51D);
  REQUIRE_FALSE(record->state.has_state()); // inert in this task
  // A content id is not a layer id.
  REQUIRE(state->find_layer(content) == nullptr);
}

TEST_CASE("composition objects round-trip through the versioned map") {
  arbc::Model model;
  arbc::ObjectId comp;
  {
    auto txn = model.transact();
    comp = txn.add_composition(1920.0, 1080.0);
    REQUIRE(txn.commit().has_value());
  }
  const arbc::DocStatePtr state = model.current();
  const arbc::CompositionRecord* record = state->find_composition(comp);
  REQUIRE(record != nullptr);
  REQUIRE(record->canvas_w == 1920.0);
  REQUIRE(record->canvas_h == 1080.0);
  REQUIRE(record->layer_count == 0);
  // A fresh composition carries the doc 07 default working space (32f until the
  // 16f designed default becomes storable, refinement Decision "staged default").
  REQUIRE(record->working_space == arbc::k_working_rgba32f);
  // Kind is enforced on lookup: a composition id is neither a layer nor content.
  REQUIRE(state->find_layer(comp) == nullptr);
  REQUIRE(state->find_content(comp) == nullptr);
}

TEST_CASE("a composition's working space defaults, sets via transaction, and versions") {
  arbc::Model model;
  arbc::ObjectId comp;
  {
    auto txn = model.transact();
    comp = txn.add_composition(64.0, 64.0);
    REQUIRE(txn.commit().has_value());
  }
  const arbc::DocStatePtr v1 = model.current();
  // Defaults on a fresh composition; `working_space()` resolves the single
  // composition, so a fresh document renders in the doc 07 default.
  REQUIRE(v1->find_composition(comp)->working_space == arbc::k_working_rgba32f);
  REQUIRE(v1->working_space() == arbc::k_working_rgba32f);

  // Set to a distinct working space: a published version, revision +1.
  {
    auto txn = model.transact();
    txn.set_working_space(comp, arbc::k_fast_rgba8srgb);
    REQUIRE(txn.commit().has_value());
  }
  const arbc::DocStatePtr v2 = model.current();
  REQUIRE(v2->revision() == v1->revision() + 1);
  REQUIRE(v2->find_composition(comp)->working_space == arbc::k_fast_rgba8srgb);
  REQUIRE(v2->working_space() == arbc::k_fast_rgba8srgb);

  // The version pinned before the config edit still sees its own value -- the
  // doc 14 pinned-version property, extended to composition records.
  REQUIRE(v1->find_composition(comp)->working_space == arbc::k_working_rgba32f);
  REQUIRE(v1->working_space() == arbc::k_working_rgba32f);
}

TEST_CASE("set_working_space is a no-op on an absent or non-composition id") {
  arbc::Model model;
  arbc::ObjectId comp;
  arbc::ObjectId layer;
  {
    auto txn = model.transact();
    comp = txn.add_composition(8.0, 8.0);
    layer = txn.add_layer(model.allocate_id(), arbc::Affine::identity());
    REQUIRE(txn.commit().has_value());
  }
  {
    auto txn = model.transact();
    txn.set_working_space(arbc::ObjectId{424242}, arbc::k_fast_rgba8srgb); // absent
    txn.set_working_space(layer, arbc::k_fast_rgba8srgb);                  // not a composition
    REQUIRE(txn.commit().has_value());
  }
  // The composition keeps the default; a no-op still commits cleanly.
  REQUIRE(model.current()->find_composition(comp)->working_space == arbc::k_working_rgba32f);
}

TEST_CASE("set_transform is a no-op on an absent or non-layer id") {
  arbc::Model model;
  arbc::ObjectId layer;
  arbc::ObjectId content;
  {
    auto txn = model.transact();
    layer = txn.add_layer(model.allocate_id(), arbc::Affine::identity());
    content = txn.add_content(1);
    REQUIRE(txn.commit().has_value());
  }
  {
    auto txn = model.transact();
    txn.set_transform(arbc::ObjectId{424242}, arbc::Affine::scaling(2.0, 2.0)); // absent
    txn.set_transform(content, arbc::Affine::scaling(2.0, 2.0));                // not a layer
    REQUIRE(txn.commit().has_value());
  }
  // Nothing changed: the layer keeps its identity transform.
  REQUIRE(model.current()->find_layer(layer)->transform == arbc::Affine::identity());
}

} // namespace
