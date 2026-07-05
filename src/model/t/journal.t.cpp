#include <arbc/model/journal.hpp>
#include <arbc/model/model.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

// Behavioral-counter doubles (doc 16:54-62): count notifications, keep the last
// payload so a test can witness the target handle / damage union crossing a seam.
struct CountingDamageSink final : arbc::DamageSink {
  int calls = 0;
  std::vector<arbc::Damage> last;
  void flush(const std::vector<arbc::Damage>& damage) override {
    ++calls;
    last = damage;
  }
};

struct NoopDamageSink final : arbc::DamageSink {
  void flush(const std::vector<arbc::Damage>&) override {}
};

struct CountingRestoreSink final : arbc::RestoreSink {
  int calls = 0;
  std::vector<std::pair<arbc::ObjectId, arbc::StateHandle>> events;
  void on_restore(arbc::ObjectId content, arbc::StateHandle target) override {
    ++calls;
    events.emplace_back(content, target);
  }
};

// A fixed per-handle coster: returns a constant for any handle carrying state, 0
// for an inert one, and counts every consultation.
struct FixedStateCostFn final : arbc::StateCostFn {
  std::size_t per_handle;
  mutable int calls = 0;
  explicit FixedStateCostFn(std::size_t c) : per_handle(c) {}
  std::size_t cost(const arbc::StateHandle& handle) const override {
    ++calls;
    return handle.has_state() ? per_handle : 0;
  }
};

const arbc::ObjectId k_dummy_content{0xC0FFEE};

// enforces: 14-data-model-and-editing#undo-is-a-forward-publish
TEST_CASE("undo/redo publish new revisions rebinding to before/after; commit sink untouched") {
  arbc::Model model;
  arbc::Journal journal(model);
  model.set_commit_sink(&journal);
  CountingDamageSink dsink;
  model.set_damage_sink(&dsink);

  arbc::ObjectId layer;
  {
    auto txn = model.transact("add");
    layer = txn.add_layer(k_dummy_content, arbc::Affine::identity());
    REQUIRE(txn.commit().has_value());
  }
  REQUIRE(journal.depth() == 1);
  REQUIRE(journal.cursor() == 1);
  REQUIRE(journal.can_undo());
  REQUIRE_FALSE(journal.can_redo());
  REQUIRE(model.current()->contains(layer));

  const std::uint64_t rev = model.current()->revision();
  const int damage_base = dsink.calls;

  // Undo: rebind to the *before* edge (empty -> erase the add). Ordinary publish
  // (revision +1), damage flushed once, NO new entry (commit sink not re-entered).
  REQUIRE(journal.undo());
  REQUIRE(model.current()->revision() == rev + 1);
  REQUIRE_FALSE(model.current()->contains(layer));
  REQUIRE(dsink.calls == damage_base + 1); // exactly one flush per navigation
  REQUIRE(journal.depth() == 1);           // history unchanged
  REQUIRE(journal.cursor() == 0);
  REQUIRE_FALSE(journal.can_undo());
  REQUIRE(journal.can_redo());

  // Redo: the symmetric *after* re-application (re-adds the layer).
  REQUIRE(journal.redo());
  REQUIRE(model.current()->revision() == rev + 2);
  REQUIRE(model.current()->contains(layer));
  REQUIRE(dsink.calls == damage_base + 2);
  REQUIRE(journal.depth() == 1);
  REQUIRE(journal.cursor() == 1);
  REQUIRE_FALSE(journal.can_redo());

  // The cursor clamps at both ends: no publish past base or tip.
  const std::uint64_t rev_tip = model.current()->revision();
  REQUIRE(journal.undo());
  REQUIRE_FALSE(journal.undo());
  REQUIRE(model.current()->revision() == rev_tip + 1); // only the one real undo
  REQUIRE(journal.redo());
  REQUIRE_FALSE(journal.redo());
}

// enforces: 14-data-model-and-editing#undo-redo-round-trips
TEST_CASE("commit -> undo -> redo returns the same SlotRef edges and round-trips live_slots") {
  arbc::Model model;
  arbc::Journal journal(model);
  model.set_commit_sink(&journal);
  NoopDamageSink dsink;
  model.set_damage_sink(&dsink);

  arbc::ObjectId layer;
  {
    auto txn = model.transact("seed");
    layer = txn.add_layer(k_dummy_content, arbc::Affine::identity());
    REQUIRE(txn.commit().has_value());
  }
  model.drain();
  arbc::SlotRef<arbc::ObjectRecord> seed_edge;
  REQUIRE(model.current()->record_edge(layer, seed_edge));

  {
    auto txn = model.transact("move");
    txn.set_transform(layer, arbc::Affine::translation(5.0, 0.0));
    REQUIRE(txn.commit().has_value());
  }
  model.drain();
  arbc::SlotRef<arbc::ObjectRecord> moved_edge;
  REQUIRE(model.current()->record_edge(layer, moved_edge));
  REQUIRE_FALSE(moved_edge == seed_edge); // the edit path-copied a new record

  const std::size_t live_at_tip = model.live_slots();

  // Undo: the edge returns to the seed identity (before edge reused, not copied).
  REQUIRE(journal.undo());
  model.drain();
  arbc::SlotRef<arbc::ObjectRecord> undone_edge;
  REQUIRE(model.current()->record_edge(layer, undone_edge));
  REQUIRE(undone_edge == seed_edge);

  // Redo: the edge returns to the moved identity (after edge reused).
  REQUIRE(journal.redo());
  model.drain();
  arbc::SlotRef<arbc::ObjectRecord> redone_edge;
  REQUIRE(model.current()->record_edge(layer, redone_edge));
  REQUIRE(redone_edge == moved_edge);
  REQUIRE(journal.cursor() == journal.depth()); // cursor back at the tip

  // Structural sharing, no leak: a full undo/redo round-trip ends at the same
  // live-slot count it started from.
  REQUIRE(model.live_slots() == live_at_tip);
}

// enforces: 14-data-model-and-editing#history-is-never-mutated
TEST_CASE("undo/redo never rewrite entries; a fresh commit drops the redo tail") {
  arbc::Model model;
  arbc::Journal journal(model);
  model.set_commit_sink(&journal);
  NoopDamageSink dsink;
  model.set_damage_sink(&dsink);

  auto add = [&](const char* name) {
    auto txn = model.transact(name);
    const arbc::ObjectId id = txn.add_layer(k_dummy_content, arbc::Affine::identity());
    REQUIRE(txn.commit().has_value());
    return id;
  };
  add("A");
  const arbc::ObjectId b = add("B");
  const arbc::ObjectId c = add("C");
  REQUIRE(journal.depth() == 3);
  REQUIRE(journal.cursor() == 3);

  // Capture entry A's byte-identity witnesses (name + its object's after edge).
  const std::string a_name = journal.entry_at(0).name;
  const arbc::SlotRef<arbc::ObjectRecord> a_after =
      journal.entry_at(0).objects.front().after.slot();

  // Undo twice: only the cursor moves; the stored entries are unchanged.
  REQUIRE(journal.undo());
  REQUIRE(journal.undo());
  REQUIRE(journal.cursor() == 1);
  REQUIRE(journal.depth() == 3);
  REQUIRE(journal.entry_at(0).name == a_name);
  REQUIRE(journal.entry_at(0).objects.front().after.slot() == a_after);
  REQUIRE_FALSE(model.current()->contains(b)); // B, C reverted
  REQUIRE_FALSE(model.current()->contains(c));

  // A fresh non-coalescing commit while the cursor is not at the tip discards the
  // redo tail (B, C) and appends -- but leaves the surviving entry byte-identical.
  add("D");
  REQUIRE(journal.depth() == 2); // A + D
  REQUIRE(journal.cursor() == 2);
  REQUIRE_FALSE(journal.can_redo());
  REQUIRE(journal.entry_at(0).name == a_name);
  REQUIRE(journal.entry_at(0).objects.front().after.slot() == a_after);
  REQUIRE(journal.entry_at(1).name == "D");
}

// enforces: 14-data-model-and-editing#coalesced-gesture-undoes-as-one
TEST_CASE("a coalesced gesture collapses to one entry and undoes as one; each commit publishes") {
  arbc::Model model;
  arbc::Journal journal(model);
  model.set_commit_sink(&journal);
  NoopDamageSink dsink;
  model.set_damage_sink(&dsink);

  arbc::ObjectId layer;
  {
    auto txn = model.transact("seed");
    layer = txn.add_layer(k_dummy_content, arbc::Affine::identity());
    REQUIRE(txn.commit().has_value());
  }
  model.drain();
  arbc::SlotRef<arbc::ObjectRecord> pre_gesture;
  REQUIRE(model.current()->record_edge(layer, pre_gesture));
  REQUIRE(journal.depth() == 1);

  const std::uint64_t rev0 = model.current()->revision();
  const arbc::CoalesceKey key = 0x1234;
  constexpr int kN = 4;
  for (int i = 0; i < kN; ++i) {
    auto txn = model.transact("drag");
    txn.coalesce(key);
    txn.set_transform(layer, arbc::Affine::translation(static_cast<double>(i + 1), 0.0));
    REQUIRE(txn.commit().has_value());
    model.drain();
  }

  // Each keyed commit published its own revision (display updated per-commit)...
  REQUIRE(model.current()->revision() == rev0 + kN);
  // ...but the whole gesture is ONE undoable entry on top of the seed.
  REQUIRE(journal.depth() == 2);
  REQUIRE(journal.cursor() == 2);

  // One undo reverts the entire gesture to its pre-gesture state.
  REQUIRE(journal.undo());
  model.drain();
  arbc::SlotRef<arbc::ObjectRecord> reverted;
  REQUIRE(model.current()->record_edge(layer, reverted));
  REQUIRE(reverted == pre_gesture);
  REQUIRE(journal.cursor() == 1); // a single step reverted the gesture
  REQUIRE(journal.can_undo());    // the seed remains undoable
}

// enforces: 14-data-model-and-editing#journal-trims-to-byte-budget
TEST_CASE("the journal trims oldest entries to the byte budget; superseded records reclaim") {
  // Drive the SAME layer through many edits: each edit supersedes the prior record
  // version, which the journal's before/after edges keep memory-live until the
  // entry is trimmed. A bounded journal drops the oldest, reclaiming those records.
  const auto run = [](std::size_t budget) {
    arbc::Model model;
    arbc::Journal journal(model, budget);
    model.set_commit_sink(&journal);
    NoopDamageSink dsink;
    model.set_damage_sink(&dsink);

    arbc::ObjectId layer;
    {
      auto txn = model.transact("seed");
      layer = txn.add_layer(k_dummy_content, arbc::Affine::identity());
      REQUIRE(txn.commit().has_value());
    }
    model.drain();

    for (int i = 0; i < 8; ++i) {
      auto txn = model.transact("move");
      txn.set_transform(layer, arbc::Affine::translation(static_cast<double>(i + 1), 0.0));
      REQUIRE(txn.commit().has_value());
      model.drain();
    }
    struct Out {
      std::size_t depth;
      std::size_t live;
      std::size_t cost;
    };
    return Out{journal.depth(), model.live_slots(), journal.byte_cost()};
  };

  const std::size_t rec = sizeof(arbc::ObjectRecord);
  // Room for ~3 edit entries (each edit's cost is a before + an after edge).
  const auto tight = run(3 * rec);
  const auto loose = run(arbc::Journal::k_no_budget);

  REQUIRE(tight.depth >= 1);          // never trims below one entry
  REQUIRE(tight.depth < loose.depth); // the bound really dropped entries
  REQUIRE(tight.cost <= 3 * rec);     // and stayed within budget
  // Fewer retained entries pin fewer uniquely-superseded records: the bounded
  // journal's live-slot count is strictly lower after trimming + drain.
  REQUIRE(tight.live < loose.live);
}

// The two L3 seams: a navigation publish consults neither for its published-record
// correctness, but fires the RestoreSink once per content edit (so the live editor
// follows) and the StateCostFn participates in the byte budget. Behavioral counters.
TEST_CASE("navigation fires the RestoreSink per content edit; StateCostFn drives the budget") {
  const std::size_t rec = sizeof(arbc::ObjectRecord);
  FixedStateCostFn coster(1000); // content payload dominates the record sizes

  arbc::Model model;
  // Budget admits ~1 content edit (2*rec records + ~2 handles of content cost).
  arbc::Journal journal(model, 3 * rec + 2200);
  model.set_commit_sink(&journal);
  journal.set_state_cost_fn(&coster);
  CountingDamageSink dsink;
  model.set_damage_sink(&dsink);
  CountingRestoreSink rsink;
  journal.set_restore_sink(&rsink);

  arbc::ObjectId content;
  {
    auto txn = model.transact("add-content");
    content = txn.add_content(0);
    REQUIRE(txn.commit().has_value());
  }
  model.drain();

  // Two content-state captures on the same object -> two edit entries.
  {
    auto txn = model.transact("capture-1");
    txn.set_content_state(content, arbc::StateHandle{7});
    REQUIRE(txn.commit().has_value());
  }
  model.drain();
  {
    auto txn = model.transact("capture-2");
    txn.set_content_state(content, arbc::StateHandle{9});
    REQUIRE(txn.commit().has_value());
  }
  model.drain();

  // The coster was consulted for content cost, and the content-dominated budget
  // trimmed to fewer entries than the three commits produced.
  REQUIRE(coster.calls > 0);
  REQUIRE(journal.byte_cost() <= 3 * rec + 2200);
  REQUIRE(journal.depth() < 3);
  REQUIRE(journal.can_undo());

  // Undo one entry: the RestoreSink fires exactly once (one content edit) with the
  // target *before* handle, damage flushes once, and the commit sink is untouched.
  const std::size_t depth_before = journal.depth();
  const int restore_base = rsink.calls;
  const int damage_base = dsink.calls;
  REQUIRE(journal.undo());
  REQUIRE(rsink.calls == restore_base + 1);
  REQUIRE(rsink.events.back().first == content);
  REQUIRE(dsink.calls == damage_base + 1);
  REQUIRE(journal.depth() == depth_before); // history not mutated

  // Redo re-applies the *after* handle through the same seam.
  REQUIRE(journal.redo());
  REQUIRE(rsink.calls == restore_base + 2);
  REQUIRE(rsink.events.back().first == content);
}

// Concurrency smoke (doc 16 tier 6, asan lane): a reader pins -> traverses via
// peek -> unpins while the writer runs undo()/redo() navigation publishes against
// it. The reader's `keep` layer is a committed seed that is never undone past, so
// it is always present; no torn read, no use-after-free. The full seeded stress
// lives in quality.stress_harness (not duplicated here).
TEST_CASE("concurrent pin/traverse against a writer running undo/redo navigation cycles") {
  arbc::Model model;
  arbc::Journal journal(model);
  model.set_commit_sink(&journal);
  NoopDamageSink dsink;
  model.set_damage_sink(&dsink);

  const arbc::ObjectId content{0xABCD};
  arbc::ObjectId keep;
  {
    auto txn = model.transact("seed");
    keep = txn.add_layer(content, arbc::Affine::identity());
    REQUIRE(txn.commit().has_value());
  }
  // One edit entry above the seed; the writer toggles undo/redo over just this one
  // entry, so the cursor never drops below the seed and `keep` always exists.
  {
    auto txn = model.transact("move");
    txn.set_transform(keep, arbc::Affine::scaling(2.0, 2.0));
    REQUIRE(txn.commit().has_value());
  }
  model.drain();

  constexpr int writer_iterations = 3000;
  std::atomic<bool> go{false};
  std::atomic<bool> stop{false};
  std::atomic<bool> bad{false};

  std::thread reader([&] {
    while (!go.load(std::memory_order_acquire)) {
    }
    while (!stop.load(std::memory_order_acquire)) {
      const arbc::DocStatePtr pinned = model.current();
      const arbc::LayerRecord* record = pinned->find_layer(keep);
      if (record == nullptr) {
        bad.store(true, std::memory_order_relaxed);
        continue;
      }
      if (!(record->content == content) || !record->visible()) {
        bad.store(true, std::memory_order_relaxed);
      }
    }
  });

  go.store(true, std::memory_order_release);
  for (int i = 0; i < writer_iterations; ++i) {
    REQUIRE(journal.undo()); // navigation publish: rebind keep to the seed transform
    model.drain();
    REQUIRE(journal.redo()); // navigation publish: rebind keep to the edited transform
    model.drain();
  }
  stop.store(true, std::memory_order_release);
  reader.join();

  REQUIRE_FALSE(bad.load());
  REQUIRE(model.current()->find_layer(keep) != nullptr);
  REQUIRE(journal.cursor() == journal.depth());
}

} // namespace
