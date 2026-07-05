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
  // Kind is enforced on lookup: a composition id is neither a layer nor content.
  REQUIRE(state->find_layer(comp) == nullptr);
  REQUIRE(state->find_content(comp) == nullptr);
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
