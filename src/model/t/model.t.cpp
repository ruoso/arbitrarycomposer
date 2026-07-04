#include <arbc/model/model.hpp>

#include <catch2/catch_test_macros.hpp>

namespace {

// enforces: 14-data-model-and-editing#pinned-version-never-observes-later-edit
TEST_CASE("a pinned version never observes a later edit") {
  arbc::Model model;
  const arbc::DocStatePtr pinned = model.current();

  auto txn = model.transact();
  const arbc::ObjectId content = model.allocate_id();
  txn.add_layer(content, arbc::Affine::identity());
  txn.commit();

  REQUIRE(pinned->layers.empty());
  REQUIRE(model.current()->layers.size() == 1);
  REQUIRE(model.current()->revision == pinned->revision + 1);
}

TEST_CASE("transactions publish atomically with monotonic revisions") {
  arbc::Model model;
  const arbc::ObjectId content = model.allocate_id();

  auto txn = model.transact();
  const arbc::ObjectId layer = txn.add_layer(content, arbc::Affine::scaling(2.0, 2.0));
  txn.commit();

  auto txn2 = model.transact();
  txn2.set_transform(layer, arbc::Affine::translation(1.0, 1.0));
  txn2.commit();

  const arbc::DocStatePtr state = model.current();
  REQUIRE(state->revision == 2);
  REQUIRE(state->layers.size() == 1);
  REQUIRE(state->layers[0].transform == arbc::Affine::translation(1.0, 1.0));
}

} // namespace
