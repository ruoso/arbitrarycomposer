#include <arbc/model/model.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

namespace {

// Test-double sinks (doc 16:54-62 behavioral counters): count notifications and
// keep the last payload so a test can witness the assembled entry / damage union.
struct RecordingCommitSink final : arbc::CommitSink {
  int calls = 0;
  std::vector<arbc::JournalEntry> entries;
  void on_commit(arbc::JournalEntry entry) override {
    ++calls;
    entries.push_back(std::move(entry));
  }
};

struct RecordingDamageSink final : arbc::DamageSink {
  int calls = 0;
  std::vector<arbc::Damage> last;
  void flush(const std::vector<arbc::Damage>& damage) override {
    ++calls;
    last = damage;
  }
};

struct NoopCommitSink final : arbc::CommitSink {
  void on_commit(arbc::JournalEntry) override {}
};
struct NoopDamageSink final : arbc::DamageSink {
  void flush(const std::vector<arbc::Damage>&) override {}
};

const arbc::ObjectId k_dummy_content{0xC0FFEE};

// enforces: 14-data-model-and-editing#commit-appends-one-journal-entry
TEST_CASE("a named commit notifies the commit sink once with the assembled entry") {
  arbc::Model model;
  RecordingCommitSink sink;
  model.set_commit_sink(&sink);

  {
    auto txn = model.transact("Add layer");
    const arbc::ObjectId content = txn.add_content(0);
    txn.add_layer(content, arbc::Affine::identity());
    REQUIRE(txn.commit().has_value());
  }

  REQUIRE(sink.calls == 1);
  const arbc::JournalEntry& e = sink.entries.front();
  REQUIRE(e.name == "Add layer"); // the transact() name round-trips into the entry
  REQUIRE(e.coalesce_key == arbc::k_no_coalesce);
  REQUIRE(e.objects.size() == 2); // content + layer, both freshly added
  for (const arbc::ObjectEdit& oe : e.objects) {
    REQUIRE_FALSE(static_cast<bool>(oe.before)); // add: no prior record edge
    REQUIRE(static_cast<bool>(oe.after));        // owning edge keeps the record live
  }
}

// enforces: 14-data-model-and-editing#commit-publishes-once
TEST_CASE("a multi-mutation transaction publishes exactly one version at commit") {
  arbc::Model model;
  RecordingCommitSink sink;
  model.set_commit_sink(&sink);
  const std::uint64_t rev0 = model.current()->revision();

  auto txn = model.transact("multi");
  const arbc::ObjectId content = txn.add_content(0);
  const arbc::ObjectId l1 = txn.add_layer(content, arbc::Affine::identity());
  const arbc::ObjectId l2 = txn.add_layer(content, arbc::Affine::identity());

  // Nothing observable mid-transaction: current() and its revision are unchanged.
  REQUIRE(model.current()->revision() == rev0);
  REQUIRE_FALSE(model.current()->contains(l1));

  REQUIRE(txn.commit().has_value());
  REQUIRE(model.current()->revision() == rev0 + 1); // exactly +1 for N mutations
  REQUIRE(sink.calls == 1);                         // exactly one entry
  REQUIRE(model.current()->contains(l1));
  REQUIRE(model.current()->contains(l2));
}

TEST_CASE("set_content_state assigns the handle and records the prior one as before") {
  arbc::Model model;
  RecordingCommitSink sink;
  model.set_commit_sink(&sink);

  arbc::ObjectId content;
  {
    auto txn = model.transact();
    content = txn.add_content(0);
    REQUIRE(txn.commit().has_value());
  }
  model.drain();
  REQUIRE_FALSE(model.current()->find_content(content)->state.has_state());

  {
    auto txn = model.transact("capture");
    txn.set_content_state(content, arbc::StateHandle{7});
    REQUIRE(txn.commit().has_value());
  }
  REQUIRE(model.current()->find_content(content)->state == arbc::StateHandle{7});

  const arbc::JournalEntry& e = sink.entries.back();
  REQUIRE(e.contents.size() == 1);
  REQUIRE(e.contents.front().object == content);
  REQUIRE_FALSE(e.contents.front().before.has_state()); // prior handle was inert
  REQUIRE(e.contents.front().after == arbc::StateHandle{7});
}

TEST_CASE("set_content_state on an absent or non-content id is a no-op") {
  arbc::Model model;
  arbc::ObjectId layer;
  {
    auto txn = model.transact();
    layer = txn.add_layer(k_dummy_content, arbc::Affine::identity());
    REQUIRE(txn.commit().has_value());
  }
  const std::uint64_t rev = model.current()->revision();
  {
    auto txn = model.transact();
    txn.set_content_state(arbc::ObjectId{424242}, arbc::StateHandle{9}); // absent
    txn.set_content_state(layer, arbc::StateHandle{9});                  // not content
    REQUIRE(txn.commit().has_value());
  }
  // The layer is unchanged; nothing captured a state handle onto it.
  REQUIRE(model.current()->find_layer(layer) != nullptr);
  REQUIRE(model.current()->revision() == rev + 1);
}

// enforces: 14-data-model-and-editing#commit-shares-untouched-structure
TEST_CASE("remove drops an object and shares untouched siblings by SlotRef identity") {
  arbc::Model model;
  std::vector<arbc::ObjectId> ids;
  {
    auto txn = model.transact();
    for (int i = 0; i < 5; ++i) {
      ids.push_back(txn.add_layer(k_dummy_content, arbc::Affine::identity()));
    }
    REQUIRE(txn.commit().has_value());
  }
  model.drain();

  const arbc::DocStatePtr before = model.current();
  arbc::SlotRef<arbc::ObjectRecord> shared_before;
  REQUIRE(before->record_edge(ids[1], shared_before));

  {
    auto txn = model.transact("remove");
    txn.remove(ids[0]);
    REQUIRE(txn.commit().has_value());
  }

  const arbc::DocStatePtr after = model.current();
  REQUIRE_FALSE(after->contains(ids[0])); // erased
  REQUIRE(after->contains(ids[1]));       // sibling survives

  arbc::SlotRef<arbc::ObjectRecord> shared_after;
  REQUIRE(after->record_edge(ids[1], shared_after));
  REQUIRE(shared_after == shared_before); // untouched sibling shared, not deep-copied

  // The pinned pre-remove version still resolves the erased object.
  REQUIRE(before->contains(ids[0]));
}

TEST_CASE("remove of an absent id is a no-op") {
  arbc::Model model;
  std::vector<arbc::ObjectId> ids;
  {
    auto txn = model.transact();
    for (int i = 0; i < 3; ++i) {
      ids.push_back(txn.add_layer(k_dummy_content, arbc::Affine::identity()));
    }
    REQUIRE(txn.commit().has_value());
  }
  model.drain();
  const std::size_t live = model.live_slots();

  {
    auto txn = model.transact();
    txn.remove(arbc::ObjectId{999999}); // absent
    REQUIRE(txn.commit().has_value());
  }
  model.drain();
  for (const arbc::ObjectId id : ids) {
    REQUIRE(model.current()->contains(id));
  }
  REQUIRE(model.live_slots() == live); // an absent-erase commit added no records
}

// enforces: 14-data-model-and-editing#abort-publishes-nothing
TEST_CASE("a dropped or aborted transaction publishes nothing and reclaims its records") {
  arbc::Model model;
  RecordingCommitSink csink;
  RecordingDamageSink dsink;
  model.set_commit_sink(&csink);
  model.set_damage_sink(&dsink);

  {
    auto txn = model.transact();
    txn.add_content(0);
    REQUIRE(txn.commit().has_value());
  }
  model.drain();
  const std::uint64_t rev = model.current()->revision();
  const std::size_t base_live = model.live_slots();
  const int commit_calls = csink.calls;
  const int damage_calls = dsink.calls;

  // (1) Dropped without commit(): abort-by-drop through member destruction.
  {
    auto txn = model.transact("dropped");
    const arbc::ObjectId content = txn.add_content(0);
    txn.add_layer(content, arbc::Affine::identity());
  }
  REQUIRE(model.current()->revision() == rev);
  REQUIRE(csink.calls == commit_calls); // no entry emitted
  REQUIRE(dsink.calls == damage_calls); // no damage flushed
  model.drain();
  REQUIRE(model.live_slots() == base_live); // working records reclaimed

  // (2) Explicit abort().
  {
    auto txn = model.transact("aborted");
    const arbc::ObjectId content = txn.add_content(0);
    txn.add_layer(content, arbc::Affine::identity());
    txn.abort();
  }
  REQUIRE(model.current()->revision() == rev);
  REQUIRE(csink.calls == commit_calls);
  REQUIRE(dsink.calls == damage_calls);
  model.drain();
  REQUIRE(model.live_slots() == base_live);
}

// enforces: 14-data-model-and-editing#damage-flushes-once-per-commit
TEST_CASE("commit flushes the deduped damage union once; abort flushes none") {
  arbc::Model model;
  RecordingDamageSink dsink;
  model.set_damage_sink(&dsink);

  {
    auto txn = model.transact();
    const arbc::ObjectId content = txn.add_content(0);
    const arbc::ObjectId layer = txn.add_layer(content, arbc::Affine::identity());
    txn.set_transform(layer, arbc::Affine::translation(1.0, 0.0)); // touches `layer` twice
    REQUIRE(txn.commit().has_value());
  }
  REQUIRE(dsink.calls == 1);       // exactly one flush for a multi-mutation commit
  REQUIRE(dsink.last.size() == 2); // union deduped by object: {content, layer}

  const int calls = dsink.calls;
  {
    auto txn = model.transact();
    txn.add_content(0);
    txn.abort();
  }
  REQUIRE(dsink.calls == calls); // abort flushes nothing
}

// enforces: 14-data-model-and-editing#coalesced-commits-merge-to-one-entry
TEST_CASE("consecutive keyed commits each publish, and merge to one journal entry") {
  arbc::Model model;
  arbc::ObjectId layer;
  {
    auto txn = model.transact();
    layer = txn.add_layer(k_dummy_content, arbc::Affine::identity());
    REQUIRE(txn.commit().has_value());
  }
  model.drain();

  // Install the sink after seeding so entries hold only the coalesced gesture.
  RecordingCommitSink sink;
  model.set_commit_sink(&sink);
  const arbc::CoalesceKey key = 0x9ABC;
  const std::uint64_t rev0 = model.current()->revision();

  constexpr int kN = 3;
  for (int i = 0; i < kN; ++i) {
    auto txn = model.transact("drag");
    txn.coalesce(key);
    txn.set_transform(layer, arbc::Affine::translation(static_cast<double>(i + 1), 0.0));
    REQUIRE(txn.commit().has_value());
    model.drain();
  }

  // Each coalesced commit still publishes a distinct revision (display updates
  // per-commit); the sink saw one entry per commit.
  REQUIRE(model.current()->revision() == rev0 + kN);
  REQUIRE(sink.entries.size() == static_cast<std::size_t>(kN));

  // The pure merge helper folds them into ONE entry: first-before / last-after,
  // unioned object set and damage.
  arbc::JournalEntry merged = sink.entries.front();
  for (std::size_t i = 1; i < sink.entries.size(); ++i) {
    REQUIRE(sink.entries[i].coalesce_key == key);
    arbc::coalesce_entries(merged, sink.entries[i]);
  }
  REQUIRE(merged.objects.size() == 1); // every commit touched the same layer
  REQUIRE(merged.objects.front().object == layer);
  REQUIRE(merged.objects.front().before == sink.entries.front().objects.front().before);
  REQUIRE(merged.objects.front().after == sink.entries.back().objects.front().after);
  REQUIRE(merged.damage.size() == 1); // damage unioned by object
}

TEST_CASE("behavioral counters: commit fires each sink once, abort fires neither") {
  arbc::Model model;
  RecordingCommitSink csink;
  RecordingDamageSink dsink;
  model.set_commit_sink(&csink);
  model.set_damage_sink(&dsink);

  const std::uint64_t rev0 = model.current()->revision();
  {
    auto txn = model.transact("edit");
    const arbc::ObjectId content = txn.add_content(0);
    const arbc::ObjectId layer = txn.add_layer(content, arbc::Affine::identity());
    txn.set_transform(layer, arbc::Affine::translation(2.0, 0.0));
    REQUIRE(txn.commit().has_value());
  }
  REQUIRE(model.current()->revision() == rev0 + 1);
  REQUIRE(csink.calls == 1);
  REQUIRE(dsink.calls == 1);

  const std::uint64_t rev1 = model.current()->revision();
  {
    auto txn = model.transact("discarded");
    txn.add_content(0);
    txn.abort();
  }
  REQUIRE(model.current()->revision() == rev1);
  REQUIRE(csink.calls == 1); // unchanged
  REQUIRE(dsink.calls == 1); // unchanged
}

// Concurrency smoke (doc 16 tier 6, asan lane): a reader pins -> traverses via
// peek -> unpins while the writer commits through no-op commit/damage sinks and
// exercises remove (add-then-remove a scratch object each commit). No torn read,
// no use-after-free. The full seeded stress lives in quality.stress_harness.
TEST_CASE("concurrent pin/traverse against a committing writer that emits + removes") {
  arbc::Model model;
  NoopCommitSink csink;
  NoopDamageSink dsink;
  model.set_commit_sink(&csink);
  model.set_damage_sink(&dsink);

  const arbc::ObjectId content{0xABCD};
  arbc::ObjectId keep;
  {
    auto txn = model.transact();
    keep = txn.add_layer(content, arbc::Affine::identity());
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
    auto txn = model.transact("edit");
    txn.coalesce(1);
    const double s = (i % 2 == 0) ? 2.0 : 3.0;
    txn.set_transform(keep, arbc::Affine::scaling(s, s));
    const arbc::ObjectId scratch = txn.add_content(0);
    txn.remove(scratch); // add-then-remove within one transaction
    REQUIRE(txn.commit().has_value());
    model.drain(); // single-drainer: only the writer thread drains
  }
  stop.store(true, std::memory_order_release);
  reader.join();

  REQUIRE_FALSE(bad.load());
  REQUIRE(model.current()->find_layer(keep) != nullptr);
}

} // namespace
