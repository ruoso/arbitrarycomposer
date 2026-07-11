#include "fake_editable.hpp"

#include <arbc/base/ids.hpp>
#include <arbc/model/records.hpp>
#include <arbc/runtime/document.hpp>

#include <atomic>
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <thread>
#include <vector>

// The multiplex (`runtime.editable_sink_multiplex`, doc 14 § Runtime binding of
// the facet): a `Document` holds ANY NUMBER of editable contents on its single
// document-wide journal, because every state seam now names its owning `ObjectId`
// and the document's one sink trio routes each retain/release/cost/restore to the
// content that owns the handle.
//
// The property under test is exclusion, not just capacity: a call for content A
// must reach A and NO OTHER content. That is why the fakes all mint slot 0, then
// slot 1, ...: their handles COLLIDE by value, so a sink that dispatched on the
// handle alone -- as the pre-multiplex seams were forced to -- would silently
// misroute. Since `kinds.raster_pool_backing` a raster's `StateHandle`
// transitively owns its tile blobs, a misrouted release frees the wrong content's
// pixels; `tests/raster_runtime_binding.t.cpp` pins that failure mode against the
// real kind. Here it is pinned kind-agnostically, on counters.

namespace {

using arbc::ObjectId;
using arbc::StateHandle;
using arbc_test::FakeEditable;

} // namespace

// enforces: 14-data-model-and-editing#editable-sinks-route-by-owner
TEST_CASE("each editable content's state calls reach that content and no other") {
  const auto a = std::make_shared<FakeEditable>();
  const auto b = std::make_shared<FakeEditable>();
  const auto c = std::make_shared<FakeEditable>();

  arbc::Document doc;
  const ObjectId ia = doc.add_content(a, /*kind=*/1);
  const ObjectId ib = doc.add_content(b, /*kind=*/1);
  const ObjectId ic = doc.add_content(c, /*kind=*/1);

  REQUIRE(doc.editable_binding().bound_count() == 3);
  REQUIRE(doc.editable_binding().bound(ia));
  REQUIRE(doc.editable_binding().bound(ib));
  REQUIRE(doc.editable_binding().bound(ic));

  // Instantiation alone touches no facet beyond the initial `capture()` (which is
  // inert here), so the counters start clean and every later move is attributable.
  REQUIRE(a->touches() == 0);
  REQUIRE(b->touches() == 0);
  REQUIRE(c->touches() == 0);

  // --- retain + cost route by owner ---
  {
    auto txn = doc.transact("edit-a");
    a->edit(txn, ia);
    REQUIRE(txn.commit().has_value());
  }
  doc.drain();

  // A retained its version and was priced into the journal budget. B and C saw
  // NOTHING: not a retain, not a release, not a cost, not a restore.
  CHECK(a->retains == 1);
  CHECK(a->costs > 0);
  CHECK(b->touches() == 0);
  CHECK(c->touches() == 0);

  {
    auto txn = doc.transact("edit-b");
    b->edit(txn, ib);
    REQUIRE(txn.commit().has_value());
  }
  doc.drain();

  CHECK(b->retains == 1);
  CHECK(a->retains == 1); // unmoved by B's edit
  CHECK(c->touches() == 0);

  // The handles COLLIDE: both contents minted slot 0, so `a`'s version and `b`'s
  // version are equal by value. Only the owner on the seam told them apart, and the
  // document resolves each content to its own.
  REQUIRE(a->base() == b->base());
  CHECK(a->refcount[a->base().slot] == 1);
  CHECK(b->refcount[b->base().slot] == 1);
  CHECK(doc.pin()->content_state(ia) == a->base());
  CHECK(doc.pin()->content_state(ib) == b->base());

  // --- restore routes by owner ---
  // Undo the "edit-b" entry: only B is rebased. A's live state is untouched, even
  // though A holds an identically-valued handle.
  const StateHandle a_before_undo = a->base();
  REQUIRE(doc.journal().undo());
  CHECK(b->restores == 1);
  CHECK(a->restores == 0);
  CHECK(c->restores == 0);
  CHECK(a->base() == a_before_undo);

  REQUIRE(doc.journal().redo());
  CHECK(b->restores == 2);
  CHECK(a->restores == 0);
  CHECK(c->touches() == 0); // C was never edited, so no seam call ever named it

  // --- the whole run routed cleanly, on ONE registration ---
  CHECK(doc.editable_binding().unrouted_state_calls() == 0);
  CHECK(doc.editable_binding().seam_registrations() == 3);
}

// enforces: 14-data-model-and-editing#editable-sinks-route-by-owner
//
// Release is the seam where a misroute is destructive rather than merely wrong (it
// frees another content's versions), so it gets its own teardown proof: with three
// editable contents on one journal, each one's releases balance ITS OWN retains at
// document teardown, and no version is left pinned. A multiplex that leaked, that
// double-freed, or that sent A's release to B would break exactly one of these.
TEST_CASE("document teardown balances retains and releases per content, with colliding handles") {
  const auto a = std::make_shared<FakeEditable>();
  const auto b = std::make_shared<FakeEditable>();
  const auto c = std::make_shared<FakeEditable>();
  std::vector<StateHandle> handles;

  {
    arbc::Document doc;
    const ObjectId ia = doc.add_content(a, 1);
    const ObjectId ib = doc.add_content(b, 1);
    const ObjectId ic = doc.add_content(c, 1);

    // Interleave the edits so all three walk the SAME slot sequence in lockstep --
    // every publish is a three-way handle collision.
    for (int i = 0; i < 4; ++i) {
      auto txn = doc.transact("edit-all");
      a->edit(txn, ia);
      b->edit(txn, ib);
      c->edit(txn, ic);
      REQUIRE(txn.commit().has_value());
      REQUIRE(a->base() == b->base());
      REQUIRE(b->base() == c->base());
      handles.push_back(a->base());
    }
    doc.drain();

    CHECK(a->retains == 4);
    CHECK(b->retains == 4);
    CHECK(c->retains == 4);
    CHECK(doc.editable_binding().unrouted_state_calls() == 0);
  } // ~Document: journal entries drop, records reclaim, release fires per retain

  for (const auto& content : {a, b, c}) {
    CHECK(content->releases == content->retains);
    for (const StateHandle h : handles) {
      CHECK(content->refcount[h.slot] == 0); // no version left pinned, none freed twice
    }
  }
}

// Writer/drain-thread discipline (Constraint 6, doc 15:117-145): the routing table
// is plain per-`Document` writer state, and render workers read pinned handles off
// `DocState` without ever touching it. This is the TSan witness: a reader thread
// pins and peeks two contents' state while the writer publishes new versions for
// both AND mutates the table (binding a third content, unbinding it again). If the
// sinks or the table were reachable from the render thread, TSan reports the race
// here; the assertions alone could not.
TEST_CASE("readers pin content state while the writer publishes and mutates the routing table") {
  const auto a = std::make_shared<FakeEditable>();
  const auto b = std::make_shared<FakeEditable>();

  arbc::Document doc;
  const ObjectId ia = doc.add_content(a, 1);
  const ObjectId ib = doc.add_content(b, 1);

  {
    auto txn = doc.transact("seed");
    a->edit(txn, ia);
    b->edit(txn, ib);
    REQUIRE(txn.commit().has_value());
  }
  doc.drain();

  constexpr int writer_iterations = 200;
  std::atomic<bool> go{false};
  std::atomic<bool> stop{false};
  std::atomic<bool> bad{false};

  const auto peek = [&](ObjectId content) {
    while (!go.load(std::memory_order_acquire)) {
    }
    while (!stop.load(std::memory_order_acquire)) {
      const arbc::DocStatePtr pinned = doc.pin();
      // The content always exists and always resolves to a real, self-consistent
      // captured handle -- never the inert sentinel, never garbage.
      if (!pinned->content_state(content).has_state()) {
        bad.store(true, std::memory_order_relaxed);
      }
    }
  };
  std::thread reader_a([&] { peek(ia); });
  std::thread reader_b([&] { peek(ib); });

  go.store(true, std::memory_order_release);
  for (int i = 0; i < writer_iterations; ++i) {
    {
      auto txn = doc.transact("edit-both");
      a->edit(txn, ia);
      b->edit(txn, ib);
      REQUIRE(txn.commit().has_value());
    }
    doc.drain();
    // Mutate the table under the readers: a third content joins and leaves.
    const auto transient = std::make_shared<FakeEditable>();
    doc.add_content(transient, 1);
  }
  stop.store(true, std::memory_order_release);
  reader_a.join();
  reader_b.join();

  REQUIRE_FALSE(bad.load());

  // Every publish retained, for the right content, on both sides -- and the journal
  // (unbudgeted, so it never trims) still holds every version, so nothing released.
  CHECK(a->retains == writer_iterations + 1);
  CHECK(b->retains == writer_iterations + 1);
  CHECK(a->releases == 0);
  CHECK(b->releases == 0);
  CHECK(doc.editable_binding().unrouted_state_calls() == 0);
  CHECK(doc.editable_binding().seam_registrations() == 3);
}
