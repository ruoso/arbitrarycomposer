#include <arbc/base/ids.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/model/damage.hpp>
#include <arbc/model/records.hpp>
#include <arbc/runtime/document.hpp>

#include <catch2/catch_test_macros.hpp>

#include "fake_editable.hpp"

#include <cstdint>
#include <memory>
#include <vector>

// Per-content removal at the document level (`model.content_removal`, doc 14 §
// Transactions -- the removal paragraph). `Document::remove_content` composes the
// existing model + binding primitives into one atomic, undoable deletion: it detaches
// the referencing layer, erases the layer and content records in one transaction, and
// -- crucially -- does NOT eagerly drop the content's `EditableBinding` row. The
// removal is journaled, so the erased ContentRecord is held for undo; its deferred
// state release must still route to a live row, so the row is RETAINED until the
// removal leaves history (Decision 1). These tests pin that retention on the binding's
// behavioral counters, kind-agnostically through `fake_editable.hpp`.

namespace {

using arbc::ObjectId;
using arbc::StateHandle;
using arbc_test::FakeEditable;

struct CountingDamageSink final : arbc::DamageSink {
  int calls = 0;
  void flush(const std::vector<arbc::Damage>&) override { ++calls; }
};

} // namespace

// enforces: 14-data-model-and-editing#remove-content-retains-binding-until-history-drops-it
TEST_CASE("remove_content deletes a content + its layer atomically and retains the binding") {
  const auto content = std::make_shared<FakeEditable>();
  const auto other = std::make_shared<FakeEditable>();

  arbc::Document doc;
  CountingDamageSink dsink;
  doc.set_damage_sink(&dsink);

  const ObjectId cid = doc.add_content(content, /*kind=*/1);
  const ObjectId oid = doc.add_content(other, /*kind=*/1);
  const ObjectId comp = doc.add_composition(100.0, 100.0);
  const ObjectId layer = doc.add_layer(cid, arbc::Affine::identity());
  doc.attach_layer(comp, layer);

  // Give the content real captured state, so the record it embeds names a live handle
  // an undo must resolve back through -- not the inert one.
  {
    auto txn = doc.transact("edit");
    content->edit(txn, cid);
    REQUIRE(txn.commit().has_value());
  }
  doc.drain();
  const StateHandle v1 = content->base();
  REQUIRE(v1.has_state());
  REQUIRE(doc.pin()->content_state(cid) == v1);

  // Both editable contents are bound before the removal; the tripwire is clean.
  REQUIRE(doc.editable_binding().bound_count() == 2);
  REQUIRE(doc.editable_binding().bound(cid));
  REQUIRE(doc.editable_binding().unrouted_state_calls() == 0);

  const std::uint64_t rev_before = doc.pin()->revision();
  const int damage_base = dsink.calls;

  // --- The removal: one publish, one journal entry, one damage flush ---
  doc.remove_content(cid, comp, layer);

  CHECK(doc.pin()->revision() == rev_before + 1); // exactly one new version
  CHECK(dsink.calls == damage_base + 1);          // the union flushed exactly once

  // Structurally gone from the live version: neither the content nor its layer record
  // survives, and the composition no longer names the layer.
  CHECK(doc.pin()->find_content(cid) == nullptr);
  CHECK(doc.pin()->find_layer(layer) == nullptr);
  std::vector<ObjectId> order;
  doc.pin()->for_each_layer_in(comp, [&](ObjectId id) { order.push_back(id); });
  CHECK(order.empty());

  // BUT the runtime binding is RETAINED (Decision 1): the row is not dropped and the
  // live Content* is still resolvable, so the record's later deferred release will
  // still find its owner. bound_count is unchanged; the side-map still resolves cid.
  CHECK(doc.editable_binding().bound_count() == 2);
  CHECK(doc.editable_binding().bound(cid));
  CHECK(doc.resolve(cid) == content.get());
  doc.drain(); // the removal is journal-held, so this reclaims nothing -> no release
  CHECK(content->releases.load() == 0);
  CHECK(doc.editable_binding().unrouted_state_calls() == 0);

  // --- Undo re-resolves the restored content with its captured state intact ---
  REQUIRE(doc.journal().undo());
  doc.drain();
  REQUIRE(doc.pin()->find_content(cid) != nullptr);
  CHECK(doc.pin()->content_state(cid) == v1); // the set_content_state, through the record
  CHECK(doc.resolve(cid) == content.get());
  CHECK(doc.editable_binding().unrouted_state_calls() == 0);

  // --- Redo re-erases it ---
  REQUIRE(doc.journal().redo());
  doc.drain();
  CHECK(doc.pin()->find_content(cid) == nullptr);
  CHECK(doc.editable_binding().unrouted_state_calls() == 0);

  // --- A fresh edit on the sibling still routes cleanly, on the retained trio ---
  {
    auto txn = doc.transact("edit-other");
    other->edit(txn, oid);
    REQUIRE(txn.commit().has_value());
  }
  doc.drain();
  CHECK(other->retains.load() == 1);
  CHECK(doc.editable_binding().bound_count() == 2); // cid's row never dropped
  CHECK(doc.editable_binding().unrouted_state_calls() == 0);
}
