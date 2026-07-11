#include <arbc/base/ids.hpp>
#include <arbc/model/records.hpp>
#include <arbc/runtime/document.hpp>

#include <catch2/catch_test_macros.hpp>

#include "fake_editable.hpp"

#include <memory>
#include <vector>

// Runtime binding of the editable-state facet (`kinds.raster_runtime_binding`,
// `runtime.editable_sink_multiplex`; docs 03:113-118, 14:133-152/173-176,
// 17:66-72): instantiating an editable content into a `Document` routes its state
// onto the live `Model`/`Journal` -- with NO manual `set_*_sink` call by the host
// -- and releases it when the content is released.
//
// These are the kind-AGNOSTIC assertions, driven through a fake `Editable` that
// counts every facet call, so they pin the binding's protocol exactly (who is
// called, how many times, in what order) without dragging a concrete kind into
// the runtime's dependency closure (doc 17: no `runtime -> kind_raster` edge).
// `tests/raster_runtime_binding.t.cpp` is the same wiring proven against the real
// `org.arbc.raster`, with its tile-blob counters. The N-content ROUTING
// assertions -- that a call for one content can never reach another -- live in
// `editable_sink_multiplex.t.cpp`.

namespace {

using arbc::ObjectId;
using arbc::StateHandle;
using arbc_test::FakeEditable;
using arbc_test::InertContent;

} // namespace

// The headline deliverable: the host wires NOTHING, and an editable content's
// edits are journaled, budgeted, undoable, and refcounted.
TEST_CASE("instantiating an editable content auto-registers its state sinks") {
  const auto content = std::make_shared<FakeEditable>();
  arbc::Document doc;
  const ObjectId cid = doc.add_content(content, /*kind=*/1);

  // A published edit retains its handle exactly once, through the StateRefSink the
  // runtime installed -- no set_state_ref_sink() anywhere in this test.
  {
    auto txn = doc.transact("edit");
    content->edit(txn, cid);
    REQUIRE(txn.commit().has_value());
  }
  const StateHandle v1 = content->base();
  CHECK(content->retains == 1);
  CHECK(content->refcount[v1.slot] == 1);
  CHECK(doc.pin()->content_state(cid) == v1);

  // The journal consulted the registered StateCostFn: the budget accounts more
  // than record sizes alone (doc 14:120-122).
  CHECK(content->costs > 0);
  CHECK(doc.journal().byte_cost() >= FakeEditable::k_state_cost);

  // Undo/redo run through the document's own Journal and rebase the live content
  // through the registered RestoreSink (doc 14:117).
  REQUIRE(doc.journal().undo());
  CHECK(content->restores == 1);
  CHECK(content->base() != v1); // rebased to the pre-edit (inert) handle

  REQUIRE(doc.journal().redo());
  CHECK(content->restores == 2);
  CHECK(content->base() == v1);
  CHECK(doc.pin()->content_state(cid) == v1);

  // Every state call found its owner (Constraint 4): the tripwire is silent.
  CHECK(doc.editable_binding().unrouted_state_calls() == 0);
}

// Render purity across an edit (doc 14:181-190): a version pinned before a second
// edit still resolves the handle it was published with, and that handle is NOT
// released while the pin holds it.
TEST_CASE("a pinned version keeps resolving its own state across a later edit") {
  const auto content = std::make_shared<FakeEditable>();
  arbc::Document doc;
  const ObjectId cid = doc.add_content(content, 1);

  {
    auto txn = doc.transact("edit1");
    content->edit(txn, cid);
    REQUIRE(txn.commit().has_value());
  }
  doc.drain();
  const StateHandle v1 = content->base();
  const arbc::DocStatePtr pinned = doc.pin();

  {
    auto txn = doc.transact("edit2");
    content->edit(txn, cid);
    REQUIRE(txn.commit().has_value());
  }
  doc.drain();
  const StateHandle v2 = content->base();

  REQUIRE(v2 != v1);
  CHECK(pinned->content_state(cid) == v1);    // frozen: the pin's own state
  CHECK(doc.pin()->content_state(cid) == v2); // live: the newest
  CHECK(content->refcount[v1.slot] == 1);     // still held: the pin (and journal)
  CHECK(content->refcount[v2.slot] == 1);
  CHECK(doc.editable_binding().unrouted_state_calls() == 0);
}

// The inert path (doc 14:176-182): non-editable content routes nowhere, so the
// journal's budget is bounded by record sizes only and no facet call ever fires.
// The document's sink trio is installed regardless (it is per-document, not
// per-content), so what keeps the budget inert is the INERT HANDLE costing 0
// ahead of any routing -- not the absence of a coster.
TEST_CASE("a non-editable content registers no sinks and leaves the budget inert") {
  arbc::Document doc;
  const ObjectId cid = doc.add_content(std::make_shared<InertContent>(), /*kind=*/2);

  CHECK(doc.editable_binding().bound_count() == 0);
  CHECK_FALSE(doc.editable_binding().bound(cid));

  {
    auto txn = doc.transact("move");
    txn.set_content_state(cid, StateHandle{}); // an inert handle: nothing to cost
    REQUIRE(txn.commit().has_value());
  }

  // Record sizes only -- no content-state contribution, because the inert handle
  // names no state for any content to price.
  CHECK(doc.journal().byte_cost() > 0);
  CHECK(doc.journal().byte_cost() < FakeEditable::k_state_cost);

  // And the inert handle is not a routing MISS either: it carries no state, so
  // nothing could be misrouted and the tripwire stays silent.
  CHECK(doc.editable_binding().unrouted_state_calls() == 0);

  // A later editable content in the same document binds normally.
  const auto editable = std::make_shared<FakeEditable>();
  const ObjectId eid = doc.add_content(editable, 1);
  CHECK(doc.editable_binding().bound(eid));
  CHECK(doc.editable_binding().bound_count() == 1);
}

// The lifted limit (`runtime.editable_sink_multiplex`). This test used to assert
// that a second editable content was a loud `std::logic_error`: with only a bare
// `StateHandle` on the retain/cost seams, a second binding could not have been
// routed back to the right content, so refusing it was the only honest option.
// The seams now carry the owning `ObjectId`, so N contents bind cleanly and each
// one's history is its own.
TEST_CASE("a second editable content in one document binds cleanly and keeps its own state") {
  arbc::Document doc;
  const auto first = std::make_shared<FakeEditable>();
  const auto second = std::make_shared<FakeEditable>();
  const ObjectId a = doc.add_content(first, 1);
  const ObjectId b = doc.add_content(second, 1);

  CHECK(doc.editable_binding().bound(a));
  CHECK(doc.editable_binding().bound(b));
  CHECK(doc.editable_binding().bound_count() == 2);

  {
    auto txn = doc.transact("edit-a");
    first->edit(txn, a);
    REQUIRE(txn.commit().has_value());
  }
  {
    auto txn = doc.transact("edit-b");
    second->edit(txn, b);
    REQUIRE(txn.commit().has_value());
  }
  doc.drain();

  // Both contents minted slot 0, so their handles are EQUAL by value and only the
  // owner on the seam distinguishes them. Each retained its own, exactly once.
  REQUIRE(first->base() == second->base());
  CHECK(first->retains == 1);
  CHECK(second->retains == 1);
  CHECK(doc.pin()->content_state(a) == first->base());
  CHECK(doc.pin()->content_state(b) == second->base());
  CHECK(doc.editable_binding().unrouted_state_calls() == 0);
}

// The release half of the contract, and the reason the binding must outlive the
// model: tearing the document down reclaims the content records, and each
// reclaim releases its pinned handle through the still-live sink EXACTLY ONCE --
// the doc 14:173-176 refcount GC promise. Now proven PER CONTENT: with N editable
// contents on one journal, each one's releases must balance its own retains, or a
// multiplex that leaks (or double-frees) would pass unnoticed. The contents
// outlive the document here (the test holds the shared_ptrs), so the counters
// survive to be read.
TEST_CASE("document teardown releases each bound content's state exactly once per retain") {
  const auto first = std::make_shared<FakeEditable>();
  const auto second = std::make_shared<FakeEditable>();
  std::vector<StateHandle> handles;
  {
    arbc::Document doc;
    const ObjectId a = doc.add_content(first, 1);
    const ObjectId b = doc.add_content(second, 1);
    for (int i = 0; i < 3; ++i) {
      auto txn = doc.transact("edit");
      first->edit(txn, a);
      second->edit(txn, b);
      REQUIRE(txn.commit().has_value());
      handles.push_back(first->base());
    }
    doc.drain();
    CHECK(first->retains == 3);
    CHECK(second->retains == 3);
    CHECK(doc.editable_binding().unrouted_state_calls() == 0);
  }

  // Every retain has been matched by exactly one release, per content -- nothing
  // leaked, nothing was released twice, and nothing crossed over.
  CHECK(first->releases == first->retains);
  CHECK(second->releases == second->retains);
  for (const StateHandle h : handles) {
    CHECK(first->refcount[h.slot] == 0);
    CHECK(second->refcount[h.slot] == 0);
  }
}

// The named, reusable teardown: `unbind(id)` drains FIRST, so the released
// content's queued reclaims still reach its facet through the live row, and only
// then drops the row -- the seam a future per-content removal path drives. Per
// content now: unbinding one leaves the other routing.
TEST_CASE("unbinding one content releases its state and leaves the others routing") {
  const auto first = std::make_shared<FakeEditable>();
  const auto second = std::make_shared<FakeEditable>();
  arbc::EditableBinding binding;
  arbc::Model model;
  arbc::Journal journal(model);
  binding.attach(model, journal);

  // The journal is attached (the binding registers the cost/restore seams onto it)
  // but is deliberately NOT the commit sink here: a journal storing entries holds
  // record edges, which would keep the removed content's record above zero count
  // and so defeat the very reclaim this test needs to observe. History is exercised
  // through `Document` in the tests above; this one is about the routing table.
  //
  // The trio is installed ONCE, at attach -- not per content (Constraint 2).
  CHECK(binding.seam_registrations() == 3);

  ObjectId a{};
  ObjectId b{};
  {
    auto txn = model.transact("add");
    a = txn.add_content(1);
    b = txn.add_content(1);
    REQUIRE(txn.commit().has_value());
  }
  binding.bind(a, *first);
  binding.bind(b, *second);
  CHECK(binding.bound(a));
  CHECK(binding.bound(b));
  CHECK(binding.seam_registrations() == 3); // still three: N binds, one registration

  {
    auto txn = model.transact("edit-both");
    first->edit(txn, a);
    second->edit(txn, b);
    REQUIRE(txn.commit().has_value());
  }
  model.drain();
  REQUIRE(first->retains == 1);
  REQUIRE(second->retains == 1);

  // Remove `a` from the document, then unbind it: the drain inside `unbind` flushes
  // a's reclaims through its still-installed row, so its retained version is
  // released before the row disappears -- no stranded release, no leak.
  {
    auto txn = model.transact("remove-a");
    txn.remove(a);
    REQUIRE(txn.commit().has_value());
  }
  binding.unbind(a);
  CHECK_FALSE(binding.bound(a));
  CHECK(binding.bound(b)); // b's row is untouched
  CHECK(binding.bound_count() == 1);
  CHECK(first->releases == first->retains);
  CHECK(second->releases == 0); // b never lost a version: the unbind did not cross over

  // b still routes: a fresh edit reaches its facet through the same installed trio.
  {
    auto txn = model.transact("edit-b-again");
    second->edit(txn, b);
    REQUIRE(txn.commit().has_value());
  }
  model.drain();
  CHECK(second->retains == 2);
  CHECK(binding.unrouted_state_calls() == 0);

  binding.unbind_all();
  CHECK(binding.bound_count() == 0);
  CHECK(binding.seam_registrations() == 6); // the three clearing calls
  CHECK(binding.unrouted_state_calls() == 0);
}
