#include <arbc/model/journal.hpp>
#include <arbc/model/model.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

namespace {

// Behavioral-counter doubles (doc 16:54-62): a recording StateRefSink counts
// retain/release and keeps each handle, so a test can witness the record-slot
// lifecycle balance. Extends the RecordingCommitSink/RecordingDamageSink pattern
// (transactions.t.cpp:15-31). The sink is type-erased -- it never resolves a real
// slot, it just tallies -- so the tests use arbitrary handle values.
// Every state seam names its owner, so the sink records the owning `ObjectId`
// alongside each handle: that is what a real multiplexing sink routes on
// (`arbc::EditableRouter`), and recording it here is what pins the model's half of
// that contract -- retain names the content the capture was published against,
// release names the content whose record slot reclaimed.
struct RecordingStateRefSink final : arbc::StateRefSink {
  int retains = 0;
  int releases = 0;
  std::vector<arbc::StateHandle> retained;
  std::vector<arbc::StateHandle> released;
  std::vector<arbc::ObjectId> retained_owners;
  std::vector<arbc::ObjectId> released_owners;
  void retain(arbc::ObjectId content, arbc::StateHandle h) override {
    ++retains;
    retained.push_back(h);
    retained_owners.push_back(content);
  }
  void release(arbc::ObjectId content, arbc::StateHandle h) override {
    ++releases;
    released.push_back(h);
    released_owners.push_back(content);
  }
};

struct CountingRestoreSink final : arbc::RestoreSink {
  int calls = 0;
  std::vector<std::pair<arbc::ObjectId, arbc::StateHandle>> events;
  void on_restore(arbc::ObjectId content, arbc::StateHandle target) override {
    ++calls;
    events.emplace_back(content, target);
  }
};

struct NoopDamageSink final : arbc::DamageSink {
  void flush(const std::vector<arbc::Damage>&) override {}
};

// enforces: 14-data-model-and-editing#content-state-reclaimed-by-refcount
TEST_CASE("a published content StateHandle retains once and releases once at slot reclaim") {
  // The recording sink is declared BEFORE the model so it outlives the ~Model
  // drain (which fires the final releases through it).
  RecordingStateRefSink refsink;
  arbc::Model model;
  model.set_state_ref_sink(&refsink);

  // A fresh content object embeds the inert sentinel -> no retain.
  arbc::ObjectId content;
  {
    auto txn = model.transact("add");
    content = txn.add_content(0);
    REQUIRE(txn.commit().has_value());
  }
  model.drain();
  REQUIRE(refsink.retains == 0);
  REQUIRE(refsink.releases == 0);

  // Capturing a non-none handle retains it exactly once for the new record slot.
  {
    auto txn = model.transact("capture-42");
    txn.set_content_state(content, arbc::StateHandle{42});
    REQUIRE(txn.commit().has_value());
  }
  REQUIRE(refsink.retains == 1);
  model.drain();
  REQUIRE(refsink.releases == 0); // the {42} record is the live version

  // Superseding it retains the new handle; draining reclaims the superseded {42}
  // record slot and releases its handle exactly once.
  {
    auto txn = model.transact("capture-43");
    txn.set_content_state(content, arbc::StateHandle{43});
    REQUIRE(txn.commit().has_value());
  }
  REQUIRE(refsink.retains == 2);
  model.drain();
  REQUIRE(refsink.releases == 1);
  REQUIRE(refsink.released.back() == arbc::StateHandle{42});

  // An unrelated edit that does NOT touch the content object shares its record
  // slot across the two versions -> no additional retain, no release.
  {
    auto txn = model.transact("unrelated");
    txn.add_composition(10.0, 10.0);
    REQUIRE(txn.commit().has_value());
  }
  model.drain();
  REQUIRE(refsink.retains == 2);
  REQUIRE(refsink.releases == 1);

  // Removing the object drops the last reference; drain releases {43}. The counts
  // balance: two distinct content records created, two handles retained + released.
  {
    auto txn = model.transact("remove");
    txn.remove(content);
    REQUIRE(txn.commit().has_value());
  }
  model.drain();
  REQUIRE(refsink.retains == 2);
  REQUIRE(refsink.releases == 2);
  REQUIRE(refsink.released ==
          std::vector<arbc::StateHandle>{arbc::StateHandle{42}, arbc::StateHandle{43}});

  // Every seam call named the content that owns the handle -- the fact a bare
  // `StateHandle` (a slot index local to its content's own store) cannot carry,
  // and the fact a multiplexing sink routes on.
  REQUIRE(refsink.retained_owners == std::vector<arbc::ObjectId>{content, content});
  REQUIRE(refsink.released_owners == std::vector<arbc::ObjectId>{content, content});
}

// enforces: 14-data-model-and-editing#pin-holds-content-state
TEST_CASE("a pinned version keeps its captured content handle live and resolvable") {
  RecordingStateRefSink refsink;
  arbc::Model model;
  model.set_state_ref_sink(&refsink);
  NoopDamageSink dsink;
  model.set_damage_sink(&dsink);

  arbc::ObjectId content;
  {
    arbc::Journal journal(model);
    model.set_commit_sink(&journal);

    {
      auto txn = model.transact("add");
      content = txn.add_content(7);
      REQUIRE(txn.commit().has_value());
    }
    model.drain();

    {
      auto txn = model.transact("capture-42");
      txn.set_content_state(content, arbc::StateHandle{42});
      REQUIRE(txn.commit().has_value());
    }
    model.drain();
    REQUIRE(refsink.retains == 1);

    // Pin the version that captured {42}; the resolver reads that frozen handle.
    arbc::DocStatePtr pinned = model.current();
    REQUIRE(pinned->content_state(content) == arbc::StateHandle{42});

    // A later capture + publish on the SAME object leaves the pinned version's
    // resolved handle unchanged and releases nothing -- the pin holds it live.
    {
      auto txn = model.transact("capture-43");
      txn.set_content_state(content, arbc::StateHandle{43});
      REQUIRE(txn.commit().has_value());
    }
    model.drain();
    REQUIRE(refsink.retains == 2);
    REQUIRE(refsink.releases == 0);
    REQUIRE(pinned->content_state(content) == arbc::StateHandle{42});          // pinned: frozen
    REQUIRE(model.current()->content_state(content) == arbc::StateHandle{43}); // live: newest

    // Dropping the pin alone does NOT release {42}: the journal's before/after
    // edges still reference that record.
    pinned.reset();
    model.drain();
    REQUIRE(refsink.releases == 0);

    model.set_commit_sink(nullptr);
  } // ~Journal releases its stored record edges

  // Only now, with the last version AND the journal reference gone, does drain
  // release the {42} handle. {43} stays live on the current version.
  model.drain();
  REQUIRE(refsink.releases == 1);
  REQUIRE(refsink.released.back() == arbc::StateHandle{42});
  REQUIRE(model.current()->content_state(content) == arbc::StateHandle{43});
}

// enforces: 14-data-model-and-editing#coalesced-gesture-captures-once
TEST_CASE("a coalesced capture gesture keeps only first-before/last-after; undo restores before") {
  RecordingStateRefSink refsink;
  CountingRestoreSink rsink;
  arbc::Model model;
  arbc::Journal journal(model);
  model.set_commit_sink(&journal);
  model.set_state_ref_sink(&refsink);
  journal.set_restore_sink(&rsink);
  NoopDamageSink dsink;
  model.set_damage_sink(&dsink);

  arbc::ObjectId content;
  {
    auto txn = model.transact("add");
    content = txn.add_content(0);
    REQUIRE(txn.commit().has_value());
  }
  model.drain();

  // A pre-gesture captured state, so the gesture's first-before is a real handle.
  {
    auto txn = model.transact("seed-state");
    txn.set_content_state(content, arbc::StateHandle{5});
    REQUIRE(txn.commit().has_value());
  }
  model.drain();
  REQUIRE(refsink.retains == 1);

  // A coalesced gesture: three keyed captures, each publishing its own revision.
  const arbc::CoalesceKey key = 0xBEEF;
  const arbc::StateHandle steps[] = {arbc::StateHandle{10}, arbc::StateHandle{11},
                                     arbc::StateHandle{12}};
  for (const arbc::StateHandle h : steps) {
    {
      auto txn = model.transact("drag");
      txn.coalesce(key);
      txn.set_content_state(content, h);
      REQUIRE(txn.commit().has_value());
    } // close the transaction (drop its base-version pin) before draining, so the
    // superseded intermediate record slot actually reclaims on this drain
    model.drain();
  }

  // Each capture retained its handle; the two intermediate handles ({10}, {11})
  // are released once their superseded record slots reclaim on drain. Only the
  // seed {5} (first-before, held by the journal) and {12} (last-after, the live
  // tip) remain referenced.
  REQUIRE(refsink.retains == 4);
  REQUIRE(refsink.releases == 2);
  REQUIRE(refsink.released ==
          std::vector<arbc::StateHandle>{arbc::StateHandle{10}, arbc::StateHandle{11}});
  REQUIRE(journal.depth() == 3); // add + seed-state + one coalesced gesture entry
  REQUIRE(journal.cursor() == 3);
  REQUIRE(model.current()->content_state(content) == arbc::StateHandle{12});

  // One undo reverts the whole gesture and fires the RestoreSink exactly once for
  // the content object, targeting the PRE-GESTURE before handle {5}.
  const int restore_base = rsink.calls;
  REQUIRE(journal.undo());
  REQUIRE(rsink.calls == restore_base + 1);
  REQUIRE(rsink.events.back().first == content);
  REQUIRE(rsink.events.back().second == arbc::StateHandle{5});
  REQUIRE(model.current()->content_state(content) == arbc::StateHandle{5});
  REQUIRE(journal.cursor() == 2);
}

// Concurrency (doc 16 tier 6, asan/tsan lanes): readers pin a version and read
// content_state via peek while the writer publishes new captured handles for TWO
// editable contents by undo/redo navigation, and the drain thread releases the
// superseded ones through the state sink. Each content's content_state resolves to
// a stable, self-consistent handle (never garbage, never the OTHER content's) under
// concurrent publishes.
//
// The two contents deliberately hold COLLIDING slot indices ({1} and {2} for both):
// a `StateHandle` is a slot index local to its own content's store, so the handles
// are indistinguishable by value and only the owning `ObjectId` on the seam tells
// them apart. The routing that owner drives lives entirely on the writer/drain
// thread; the reader touches nothing but the pinned `DocState`, which is what TSan
// proves here. The full seeded schedule-perturbation stress lives in
// quality.stress_harness (not duplicated here).
TEST_CASE("concurrent pin + content_state peek against a writer publishing captures for two "
          "editable contents") {
  RecordingStateRefSink refsink;
  arbc::Model model;
  arbc::Journal journal(model);
  model.set_commit_sink(&journal);
  model.set_state_ref_sink(&refsink);
  NoopDamageSink dsink;
  model.set_damage_sink(&dsink);

  arbc::ObjectId a;
  arbc::ObjectId b;
  {
    auto txn = model.transact("seed"); // add both + capture {1} on each, in one version
    a = txn.add_content(0);
    b = txn.add_content(0);
    txn.set_content_state(a, arbc::StateHandle{1});
    txn.set_content_state(b, arbc::StateHandle{1});
    REQUIRE(txn.commit().has_value());
  }
  {
    auto txn = model.transact("recapture-both");
    txn.set_content_state(a, arbc::StateHandle{2});
    txn.set_content_state(b, arbc::StateHandle{2});
    REQUIRE(txn.commit().has_value());
  }
  model.drain();

  constexpr int writer_iterations = 2000;
  std::atomic<bool> go{false};
  std::atomic<bool> stop{false};
  std::atomic<bool> bad{false};

  const auto peek = [&](arbc::ObjectId content) {
    while (!go.load(std::memory_order_acquire)) {
    }
    while (!stop.load(std::memory_order_acquire)) {
      const arbc::DocStatePtr pinned = model.current();
      const arbc::StateHandle h = pinned->content_state(content);
      // The content object always exists (never undone past its add), and its
      // captured handle is exactly one of the two published states.
      if (!(h == arbc::StateHandle{1} || h == arbc::StateHandle{2})) {
        bad.store(true, std::memory_order_relaxed);
      }
    }
  };
  std::thread reader_a([&] { peek(a); });
  std::thread reader_b([&] { peek(b); });

  go.store(true, std::memory_order_release);
  for (int i = 0; i < writer_iterations; ++i) {
    REQUIRE(journal.undo()); // rebind BOTH contents to their {1} records
    model.drain();
    REQUIRE(journal.redo()); // rebind BOTH contents to their {2} records
    model.drain();
  }
  stop.store(true, std::memory_order_release);
  reader_a.join();
  reader_b.join();

  REQUIRE_FALSE(bad.load());
  REQUIRE(model.current()->content_state(a) == arbc::StateHandle{2});
  REQUIRE(model.current()->content_state(b) == arbc::StateHandle{2});

  // Every retain/release the navigation churn drove named one of the two contents
  // -- the seam never handed a handle to the wrong owner, which is the only thing
  // standing between two colliding slot indices and a cross-content free.
  REQUIRE(refsink.retains > 0);
  for (const arbc::ObjectId owner : refsink.retained_owners) {
    REQUIRE((owner == a || owner == b));
  }
  for (const arbc::ObjectId owner : refsink.released_owners) {
    REQUIRE((owner == a || owner == b));
  }
}

} // namespace
