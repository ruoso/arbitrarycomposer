#include <arbc/model/records.hpp>
#include <arbc/pool/refs.hpp>
#include <arbc/pool/slot_store.hpp>

#include <catch2/catch_test_macros.hpp>

#include <type_traits>

namespace {

// The record-shape invariants are compile-time (doc 15:243-245); re-assert them
// at the test site so the acceptance criterion is visible where it is checked.
static_assert(std::is_standard_layout_v<arbc::ObjectRecord>);
static_assert(std::is_trivially_destructible_v<arbc::ObjectRecord>);
static_assert(std::is_standard_layout_v<arbc::LayerRecord>);
static_assert(std::is_trivially_destructible_v<arbc::LayerRecord>);
static_assert(std::is_standard_layout_v<arbc::ContentRecord>);
static_assert(std::is_trivially_destructible_v<arbc::ContentRecord>);
static_assert(std::is_standard_layout_v<arbc::CompositionRecord>);
static_assert(std::is_trivially_destructible_v<arbc::CompositionRecord>);

TEST_CASE("object records round-trip through a SlotRef edge resolved by peek") {
  arbc::Arena arena;
  arbc::RefStore<arbc::ObjectRecord> records(arena);

  // A record type holding an in-record SlotRef edge (index-only, as inside a
  // HAMT leaf) round-trips through peek.
  struct Edge {
    arbc::SlotRef<arbc::ObjectRecord> ref;
  };
  static_assert(std::is_trivially_copyable_v<Edge>);

  SECTION("layer") {
    arbc::Ref<arbc::ObjectRecord> owner = *records.create();
    owner->kind = arbc::RecordKind::Layer;
    owner->id = arbc::ObjectId{7};
    owner->as.layer = arbc::LayerRecord{arbc::ObjectId{99}, arbc::Affine::scaling(3.0, 3.0), 0.5,
                                        arbc::k_layer_visible};
    Edge edge{owner.slot()};

    const arbc::ObjectRecord* resolved = records.peek(edge.ref);
    REQUIRE(resolved->kind == arbc::RecordKind::Layer);
    REQUIRE(resolved->id == arbc::ObjectId{7});
    REQUIRE(resolved->as.layer.content == arbc::ObjectId{99});
    REQUIRE(resolved->as.layer.opacity == 0.5);
    REQUIRE(resolved->as.layer.visible());
  }

  SECTION("content") {
    arbc::Ref<arbc::ObjectRecord> owner = *records.create();
    owner->kind = arbc::RecordKind::Content;
    owner->id = arbc::ObjectId{8};
    owner->as.content = arbc::ContentRecord{0xC0FFEE, arbc::StateHandle{}};
    Edge edge{owner.slot()};

    const arbc::ObjectRecord* resolved = records.peek(edge.ref);
    REQUIRE(resolved->kind == arbc::RecordKind::Content);
    REQUIRE(resolved->as.content.kind == 0xC0FFEE);
    REQUIRE_FALSE(resolved->as.content.state.has_state());
  }

  SECTION("composition") {
    arbc::Ref<arbc::ObjectRecord> owner = *records.create();
    owner->kind = arbc::RecordKind::Composition;
    owner->id = arbc::ObjectId{9};
    arbc::CompositionRecord comp{};
    comp.canvas_w = 1920.0;
    comp.canvas_h = 1080.0;
    comp.layer_count = 2;
    comp.layers[0] = arbc::ObjectId{7};
    comp.layers[1] = arbc::ObjectId{8};
    owner->as.composition = comp;
    Edge edge{owner.slot()};

    const arbc::ObjectRecord* resolved = records.peek(edge.ref);
    REQUIRE(resolved->kind == arbc::RecordKind::Composition);
    REQUIRE(resolved->as.composition.canvas_w == 1920.0);
    REQUIRE(resolved->as.composition.layer_count == 2);
    REQUIRE(resolved->as.composition.layers[1] == arbc::ObjectId{8});
  }
}

} // namespace
