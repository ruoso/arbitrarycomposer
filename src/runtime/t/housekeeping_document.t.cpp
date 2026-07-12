#include <arbc/base/ids.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/pool/workspace_file.hpp>
#include <arbc/runtime/document.hpp>

#include <catch2/catch_test_macros.hpp>

#include "fake_editable.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

// `runtime.housekeeping_document_wiring`: a live `Document` drives its `Model`'s
// arena through an owned `HousekeepingThread` (docs 14/15). Before this, a
// `Document`'s entire housekeeping surface was a manual `drain()` no production code
// called: nothing drained between transactions, nothing ever checkpointed, nothing
// reported how much memory the document held.
//
// These are the kind-agnostic component tests (the `Editable` here is a fake, so no
// concrete-kind edge enters `runtime`, doc 17:66-72). The workspace-backed checkpoint
// cadence is `tests/document_workspace_checkpoint.t.cpp`; the concurrency coverage is
// `tests/stress_document_housekeeping.t.cpp`.

namespace {

using arbc::Document;
using arbc::DocumentHousekeepingConfig;
using arbc::HousekeepingStats;
using arbc::ObjectId;
using arbc_test::FakeEditable;

// A park period long enough that no automatic timeout tick fires during a
// deterministic test: the only background ticks are the ones `flush_housekeeping()`
// pokes. It never appears in an assertion (doc 16:54-62) -- it just makes the
// document's drains attributable to one site at a time.
constexpr std::chrono::steady_clock::duration kNoTimeout = std::chrono::hours(1);

DocumentHousekeepingConfig poke_driven() {
  DocumentHousekeepingConfig config;
  config.thread.tick_period = kNoTimeout;
  return config;
}

} // namespace

// enforces: 15-memory-model#document-drain-runs-through-housekeeper
//
// The regression test for the hazard that made this task more than plumbing: a
// `Housekeeper` pointed at the raw `ReclamationQueue` would call `drain()` bare, and
// `~HamtNode` -- which reaches its stores through a thread-local `ReclaimContext` --
// SILENTLY RETURNS WITHOUT RELEASING ITS CHILD EDGES when none is published
// (hamt.hpp:103-109). No crash, no failing assertion: just every interior HAMT edge
// leaked, forever. Routing the drain through `Model::drain()` (which publishes the
// context) is what makes the live-slot count below stay FLAT instead of climbing with
// the churn.
TEST_CASE("a Document's between-transaction drain reclaims each transaction's garbage") {
  // Poke-driven: with no background timeout tick, the ONLY drains are the writer's
  // `after_commit` ones, so a flat live-slot count is attributable to them alone.
  Document doc(poke_driven());

  const auto fake = std::make_shared<FakeEditable>();
  const ObjectId cid = doc.add_content(fake, /*kind=*/1);
  const ObjectId comp = doc.add_composition(4.0, 4.0);
  const ObjectId layer = doc.add_layer(cid, arbc::Affine::identity());
  doc.attach_layer(comp, layer);

  // One churn cycle: an edit that mints a fresh content version, then an undo. The
  // NEXT cycle's commit drops the redo tail, releasing that entry's record edges --
  // so the journal reaches a steady depth and every cycle produces a fixed quantum of
  // garbage: superseded HAMT nodes, a superseded content record, and the state handle
  // that record held.
  const auto churn = [&](int rounds) {
    for (int i = 0; i < rounds; ++i) {
      {
        auto txn = doc.transact("edit");
        fake->edit(txn, cid);
        REQUIRE(txn.commit().has_value());
      }
      REQUIRE(doc.journal().undo());
    }
  };

  churn(4); // warm up to the journal's steady state
  const std::size_t settled = doc.memory_stats().live_slots;
  const int releases_settled = fake->releases.load();
  const std::uint64_t drains_settled = doc.memory_stats().drains_run;

  churn(64); // ... and NOT ONE manual drain() in here

  const HousekeepingStats stats = doc.memory_stats();

  // FLAT. Sampled at the same phase of the cycle, so the between-transaction drain
  // reclaimed every cycle's garbage before the next cycle began. A bare-queue drain
  // (Hazard 1) leaks every interior edge, so this count would climb monotonically.
  CHECK(stats.live_slots == settled);
  CHECK(stats.drains_run >= drains_settled + 64); // one per commit

  // ...and the state handles those reclaimed records held were released too, through
  // the still-installed sink trio, to the content that owns them.
  CHECK(fake->releases.load() > releases_settled);
  CHECK(doc.editable_binding().unrouted_state_calls() == 0);

  // An explicit drain never GROWS the count -- it only sweeps up the one cycle still
  // in flight (the last transaction's base version is pinned by its `Transaction`
  // until that object dies, i.e. after its own commit's drain; that is precisely what
  // "drained BETWEEN transactions" means). And it quiesces: a second drain finds
  // nothing at all, which is the no-garbage baseline this whole cadence converges on.
  doc.drain();
  const std::size_t quiesced = doc.memory_stats().live_slots;
  CHECK(quiesced <= settled);
  doc.drain();
  CHECK(doc.memory_stats().live_slots == quiesced);
}

// enforces: 15-memory-model#document-drain-runs-through-housekeeper
//
// The other half of the claim, and the reason `runtime.housekeeping_thread` exists at
// all: a document edited and then LEFT IDLE still reclaims. The garbage a transaction
// produces lands on the queue only when the `Transaction` object dies -- AFTER its own
// commit's drain -- so it sits there until something else drains. If the next edit
// never comes (the user stopped typing), the background thread is the only thing that
// can, and this proves it does.
TEST_CASE("an idle Document's garbage is reclaimed by the background housekeeping thread") {
  Document doc(poke_driven());

  const ObjectId comp = doc.add_composition(4.0, 4.0);
  doc.drain();
  const std::size_t baseline = doc.memory_stats().live_slots;

  // One last edit, then the document goes idle -- no further commit, no manual drain.
  doc.set_working_space(comp, arbc::k_working_rgba32f);
  const std::size_t idle_dirty = doc.memory_stats().live_slots;
  REQUIRE(idle_dirty > baseline); // the superseded version is on the queue, undrained

  // Hand the work to the BACKGROUND thread and wait on the tick counter -- a
  // condition, never a clock (doc 16:54-62). `drain()` would do it on this thread and
  // prove nothing about idle reclamation.
  const std::uint64_t before = doc.background_ticks();
  const std::uint64_t after = doc.flush_housekeeping();

  CHECK(after > before);
  CHECK(doc.memory_stats().live_slots < idle_dirty);
}

// enforces: 15-memory-model#housekeeping-reports-memory-panel-stats
TEST_CASE("a Document reports its arena's live slots and reserved bytes") {
  Document doc(poke_driven());

  // A fresh document has published an empty version-0 root and allocated nothing, so
  // the arena has reserved no chunk yet -- an honest zero, not a stub.
  const HousekeepingStats empty = doc.memory_stats();
  CHECK(empty.live_slots == 0);
  CHECK(empty.bytes_reserved == 0);

  const ObjectId comp = doc.add_composition(4.0, 4.0);
  for (int i = 0; i < 8; ++i) {
    doc.add_layer(comp, arbc::Affine::translation(static_cast<double>(i), 0.0));
  }
  doc.drain();

  const HousekeepingStats grown = doc.memory_stats();
  CHECK(grown.live_slots > empty.live_slots); // the layers are really there
  // The byte half of the panel (doc 15:164-169): the arena now holds real chunks.
  CHECK(grown.bytes_reserved > 0);
  CHECK(grown.transactions_seen == 9); // one composition + eight layers
  CHECK(grown.drains_run >= grown.transactions_seen);

  // Anonymous: no checkpointer, so the durability counters are inert rather than lying.
  CHECK(grown.checkpoints_committed == 0);
  CHECK(grown.slots_freed_to_list == 0);
  CHECK(grown.durable_epoch == 0);
}

// enforces: 15-memory-model#document-checkpoint-cadence
//
// The anonymous leg of the cadence claim (its workspace-backed legs are the integration
// test): an anonymous document has no file and nothing to make durable, so an explicit
// checkpoint request ANSWERS -- errors as values, doc 10 -- rather than asserting or
// silently pretending to have written something.
TEST_CASE("checkpointing an anonymous Document is Unsupported, not a lie and not a crash") {
  Document doc(poke_driven());
  doc.add_composition(4.0, 4.0);

  CHECK_FALSE(doc.workspace_backed());

  const auto result = doc.checkpoint();
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().code == arbc::WorkspaceFileErrc::Unsupported);

  // ...and no trigger fired behind our back: the reclamation cadence still ran, but
  // nothing was committed and nothing was counted as skipped-clean.
  const HousekeepingStats stats = doc.memory_stats();
  CHECK(stats.checkpoints_committed == 0);
  CHECK(stats.checkpoints_skipped_clean == 0);
  CHECK(stats.drains_run > 0);
  CHECK_FALSE(doc.last_checkpoint_error().has_value());
}

// enforces: 15-memory-model#document-drain-runs-through-housekeeper
//
// TEARDOWN ORDERING (Constraint 5). `~HousekeepingThread` stops the loop and runs one
// final drain to quiescence, and that drain RELEASES content state through the sink
// trio -- reaching the router, and through it the `Content` objects. So the thread must
// be the LAST-DECLARED member of `Document`, destroyed FIRST, while every one of those
// is still alive. Get the order wrong and the final drain routes through a destroyed
// router into destroyed contents: ASan catches it, and so does the release count here,
// which the rasters' pool blocks depend on for real.
TEST_CASE("destroying a Document with a live background thread drains into live contents") {
  const auto a = std::make_shared<FakeEditable>(); // outlives the document
  const auto b = std::make_shared<FakeEditable>();
  std::vector<arbc::StateHandle> handles;

  {
    Document doc; // an ACTIVE background thread, running right up to the destructor
    const ObjectId ia = doc.add_content(a, 1);
    const ObjectId ib = doc.add_content(b, 1);

    for (int i = 0; i < 4; ++i) {
      auto txn = doc.transact("edit-both");
      a->edit(txn, ia);
      b->edit(txn, ib);
      REQUIRE(txn.commit().has_value());
      handles.push_back(a->base());
    }
    CHECK(doc.editable_binding().unrouted_state_calls() == 0);
  } // ~Document: thread joins after a final drain, THEN journal, model, binding, contents

  // Every retain was matched by exactly one release, and it landed on the content that
  // owned the handle -- even though `a` and `b` mint COLLIDING slot indices.
  for (const auto& content : {a, b}) {
    CHECK(content->releases.load() == content->retains.load());
    CHECK(content->retains.load() == 4);
    for (const arbc::StateHandle h : handles) {
      CHECK(content->refcount_of(h.slot) == 0); // none left pinned, none freed twice
    }
  }
}
