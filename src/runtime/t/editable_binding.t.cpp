#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/model/records.hpp>
#include <arbc/runtime/document.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

// Runtime binding of the editable-state facet (`kinds.raster_runtime_binding`,
// docs 03:113-118, 14:133-152/173-176, 17:66-72): instantiating an editable
// content into a `Document` registers its state sinks onto the live
// `Model`/`Journal` -- with NO manual `set_*_sink` call by the host -- and
// releases them when the content is released.
//
// These are the kind-AGNOSTIC assertions, driven through a fake `Editable` that
// counts every facet call, so they pin the binding's protocol exactly (who is
// called, how many times, in what order) without dragging a concrete kind into
// the runtime's dependency closure (doc 17: no `runtime -> kind_raster` edge).
// `tests/raster_runtime_binding.t.cpp` is the same wiring proven against the real
// `org.arbc.raster`, with its tile-blob counters.

namespace {

using arbc::Content;
using arbc::Editable;
using arbc::ObjectId;
using arbc::RenderCompletion;
using arbc::RenderRequest;
using arbc::RenderResult;
using arbc::Stability;
using arbc::StateHandle;

// A minimal editable content: versions are integers, each with a refcount -- the
// same state lifecycle shape as raster's tile-table store (retain on publish,
// release at record reclaim), shrunk to something a test can count exactly.
class FakeEditable final : public Content, public Editable {
public:
  // Per-version byte cost the journal's budget should pick up through the
  // registered cost fn (any non-zero value; the assertion is that it is CONSULTED).
  static constexpr std::size_t k_state_cost = 4096;

  // --- Content ---
  std::optional<arbc::Rect> bounds() const override { return std::nullopt; }
  Stability stability() const override { return Stability::Static; }
  std::optional<arbc::TimeRange> time_extent() const override { return std::nullopt; }
  std::optional<RenderResult> render(const RenderRequest&,
                                     std::shared_ptr<RenderCompletion>) override {
    return RenderResult{};
  }
  Editable* editable() override { return this; }

  // --- Editable ---
  StateHandle capture() override { return d_base; }
  void restore(StateHandle state) override {
    d_base = state;
    ++restores;
  }
  std::size_t state_cost(StateHandle state) const override {
    ++costs;
    return state.has_state() ? k_state_cost : 0;
  }
  void retain(StateHandle state) override {
    ++retains;
    ++refcount[state.slot];
  }
  void release(StateHandle state) override {
    ++releases;
    --refcount[state.slot];
  }

  // Mint a new version and assign it to `self` under transactional discipline --
  // the shape of `RasterContent::paint` (doc 03:152-158), minus the pixels.
  void edit(arbc::Model::Transaction& txn, ObjectId self) {
    const StateHandle after{d_next_slot++};
    d_base = after;
    txn.set_content_state(self, after);
  }

  StateHandle base() const { return d_base; }

  // Behavioral counters (doc 16: counters, never wall-clock).
  int retains{0};
  int releases{0};
  int restores{0};
  mutable int costs{0};
  std::map<arbc::SlotIndex, int> refcount;

private:
  StateHandle d_base{};
  arbc::SlotIndex d_next_slot{0};
};

// A leaf content with no editable facet (the `org.arbc.tone`/`org.arbc.solid`
// shape): the inert-path control.
class InertContent final : public Content {
public:
  std::optional<arbc::Rect> bounds() const override { return std::nullopt; }
  Stability stability() const override { return Stability::Static; }
  std::optional<arbc::TimeRange> time_extent() const override { return std::nullopt; }
  std::optional<RenderResult> render(const RenderRequest&,
                                     std::shared_ptr<RenderCompletion>) override {
    return RenderResult{};
  }
};

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
  CHECK(pinned->content_state(cid) == v1);      // frozen: the pin's own state
  CHECK(doc.pin()->content_state(cid) == v2);   // live: the newest
  CHECK(content->refcount[v1.slot] == 1);       // still held: the pin (and journal)
  CHECK(content->refcount[v2.slot] == 1);
}

// The inert path (doc 14:176-182): non-editable content registers nothing, so the
// journal's budget is bounded by record sizes only and no facet call ever fires.
TEST_CASE("a non-editable content registers no sinks and leaves the budget inert") {
  arbc::Document doc;
  const ObjectId cid = doc.add_content(std::make_shared<InertContent>(), /*kind=*/2);

  {
    auto txn = doc.transact("move");
    txn.set_content_state(cid, StateHandle{}); // an inert handle: nothing to cost
    REQUIRE(txn.commit().has_value());
  }

  // Record sizes only -- no content-state contribution, because there is no
  // registered StateCostFn to ask.
  CHECK(doc.journal().byte_cost() > 0);
  CHECK(doc.journal().byte_cost() < FakeEditable::k_state_cost);

  // And a non-editable content leaves the seam free for a later editable one.
  const auto editable = std::make_shared<FakeEditable>();
  CHECK_NOTHROW(doc.add_content(editable, 1));
}

// The v1 limit, made loud (Constraint 9 / doc 16: never a silent overwrite). A
// second editable content would replace the first's sinks in the single model /
// journal slots and misroute the first content's retain/release to the wrong
// store, so it is a hard precondition failure.
TEST_CASE("a second editable content in one document is a loud precondition failure") {
  arbc::Document doc;
  doc.add_content(std::make_shared<FakeEditable>(), 1);

  const auto second = std::make_shared<FakeEditable>();
  CHECK_THROWS_AS(doc.add_content(second, 1), std::logic_error);
}

// The release half of the contract, and the reason the binding must outlive the
// model: tearing the document down reclaims the content records, and each
// reclaim releases its pinned handle through the still-live sink EXACTLY ONCE --
// the doc 14:173-176 refcount GC promise. The content outlives the document here
// (the test holds the shared_ptr), so the counters survive to be read.
TEST_CASE("document teardown releases the bound content's state exactly once per retain") {
  const auto content = std::make_shared<FakeEditable>();
  std::vector<StateHandle> handles;
  {
    arbc::Document doc;
    const ObjectId cid = doc.add_content(content, 1);
    for (int i = 0; i < 3; ++i) {
      auto txn = doc.transact("edit");
      content->edit(txn, cid);
      REQUIRE(txn.commit().has_value());
      handles.push_back(content->base());
    }
    doc.drain();
    CHECK(content->retains == 3);
  }

  // Every retain has been matched by exactly one release, and no version is left
  // pinned -- nothing leaked, and nothing was released twice.
  CHECK(content->releases == content->retains);
  for (const StateHandle h : handles) {
    CHECK(content->refcount[h.slot] == 0);
  }
}

// The named, reusable teardown (Constraint 4): `unbind()` drains FIRST, so a
// released content's records reclaim through the live sink, and only then clears
// the slots -- the seam a future per-content removal path drives.
TEST_CASE("unbinding releases the content's state and frees the seam for another") {
  const auto first = std::make_shared<FakeEditable>();
  arbc::EditableBinding binding;
  arbc::Model model;
  arbc::Journal journal(model);
  model.set_commit_sink(&journal);
  binding.attach(model, journal);

  ObjectId cid{};
  {
    auto txn = model.transact("add");
    cid = txn.add_content(1);
    REQUIRE(txn.commit().has_value());
  }
  binding.bind(cid, *first);
  CHECK(binding.bound());
  CHECK(binding.owner() == cid);

  binding.unbind();
  CHECK_FALSE(binding.bound());

  // The seam is free: a second editable content now binds without a throw (this is
  // exactly what a per-content removal path would do).
  const auto second = std::make_shared<FakeEditable>();
  CHECK_NOTHROW(binding.bind(cid, *second));
  binding.unbind();
}
