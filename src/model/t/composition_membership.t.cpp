#include <arbc/model/journal.hpp>
#include <arbc/model/model.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

namespace {

// Recording sinks (doc 16:54-62 behavioral counters), mirroring the doubles at
// src/model/t/transactions.t.cpp:15-31.
struct RecordingDamageSink final : arbc::DamageSink {
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

// Read a composition's membership order through the composition-scoped accessor.
std::vector<arbc::ObjectId> order_of(const arbc::DocStatePtr& doc, arbc::ObjectId comp) {
  std::vector<arbc::ObjectId> out;
  doc->for_each_layer_in(comp, [&](arbc::ObjectId id) { out.push_back(id); });
  return out;
}

// Create `n` free-floating layers, returning their ids (committed + drained).
std::vector<arbc::ObjectId> seed_layers(arbc::Model& model, int n) {
  std::vector<arbc::ObjectId> ids;
  auto txn = model.transact("seed");
  for (int i = 0; i < n; ++i) {
    ids.push_back(txn.add_layer(arbc::ObjectId{0xC0FFEE}, arbc::Affine::identity()));
  }
  REQUIRE(txn.commit().has_value());
  model.drain();
  return ids;
}

// enforces: 14-data-model-and-editing#layer-order-is-explicit
TEST_CASE("attach at index, detach, and reorder produce the intended bottom-to-top order") {
  arbc::Model model;
  const std::vector<arbc::ObjectId> l = seed_layers(model, 4);
  arbc::ObjectId comp;
  {
    auto txn = model.transact();
    comp = txn.add_composition(100.0, 100.0);
    REQUIRE(txn.commit().has_value());
  }

  // Append three layers, then insert the fourth at index 1.
  {
    auto txn = model.transact("attach");
    txn.attach_layer(comp, l[0], 0);
    txn.attach_layer(comp, l[1], 1);
    txn.attach_layer(comp, l[2], 2);
    REQUIRE(txn.commit().has_value());
  }
  REQUIRE(order_of(model.current(), comp) == std::vector<arbc::ObjectId>{l[0], l[1], l[2]});

  {
    auto txn = model.transact("attach at index");
    txn.attach_layer(comp, l[3], 1); // insert between l0 and l1
    REQUIRE(txn.commit().has_value());
  }
  REQUIRE(order_of(model.current(), comp) == std::vector<arbc::ObjectId>{l[0], l[3], l[1], l[2]});

  // Detach a member id.
  {
    auto txn = model.transact("detach");
    txn.detach_layer(comp, l[1]);
    REQUIRE(txn.commit().has_value());
  }
  REQUIRE(order_of(model.current(), comp) == std::vector<arbc::ObjectId>{l[0], l[3], l[2]});

  // Reorder is a stable move: move index 0 (l0) to index 2.
  {
    auto txn = model.transact("reorder");
    txn.reorder_layer(comp, 0, 2);
    REQUIRE(txn.commit().has_value());
  }
  // l3 and l2 keep their relative order; l0 lands at the end.
  REQUIRE(order_of(model.current(), comp) == std::vector<arbc::ObjectId>{l[3], l[2], l[0]});
}

// enforces: 14-data-model-and-editing#membership-spills-past-inline-cap
TEST_CASE("membership spills past the inline cap and round-trips losslessly across it") {
  arbc::Model model;
  constexpr int kN = 21;
  const std::vector<arbc::ObjectId> l = seed_layers(model, kN);
  arbc::ObjectId comp;
  {
    auto txn = model.transact();
    comp = txn.add_composition(100.0, 100.0);
    REQUIRE(txn.commit().has_value());
  }

  const std::size_t cap = arbc::k_max_inline_layers;

  // Fill exactly to the inline cap: still inline (spill_root invalid).
  {
    auto txn = model.transact("inline fill");
    for (std::size_t i = 0; i < cap; ++i) {
      txn.attach_layer(comp, l[i]);
    }
    REQUIRE(txn.commit().has_value());
  }
  const std::vector<arbc::ObjectId> inline_order = order_of(model.current(), comp);
  REQUIRE(inline_order.size() == cap);
  REQUIRE_FALSE(model.current()->find_composition(comp)->spill_root.valid());

  // Cross the cap: attach through the 20th member -> spills to chunks.
  {
    auto txn = model.transact("spill");
    for (std::size_t i = cap; i < 20; ++i) {
      txn.attach_layer(comp, l[i]);
    }
    REQUIRE(txn.commit().has_value());
  }
  model.drain();
  std::vector<arbc::ObjectId> spilled_order = order_of(model.current(), comp);
  REQUIRE(spilled_order.size() == 20);
  for (std::size_t i = 0; i < 20; ++i) {
    REQUIRE(spilled_order[i] == l[i]); // full order reported correctly from chunks
  }
  REQUIRE(model.current()->find_composition(comp)->spill_root.valid());
  REQUIRE(model.current()->find_composition(comp)->layer_count == 20);

  // A spilled edit that appends at the top rewrites only the touched tail chunk;
  // the head chunk is shared by SlotRef identity between the two versions.
  const arbc::DocStatePtr before = model.current();
  const arbc::ObjectId head = before->find_composition(comp)->spill_root;
  arbc::SlotRef<arbc::ObjectRecord> head_before;
  REQUIRE(before->record_edge(head, head_before));
  const std::size_t live_before = model.live_slots();

  {
    auto txn = model.transact("append top");
    txn.attach_layer(comp, l[20]); // append at the top (last chunk only)
    REQUIRE(txn.commit().has_value());
  }
  const arbc::DocStatePtr after = model.current();
  REQUIRE(after->find_composition(comp)->spill_root == head); // head chunk id stable
  arbc::SlotRef<arbc::ObjectRecord> head_after;
  REQUIRE(after->record_edge(head, head_after));
  REQUIRE(head_after == head_before); // untouched head chunk shared, not deep-copied
  // Growth is O(touched chunks + path depth), not O(members): far below the 21
  // members it now holds.
  REQUIRE(model.live_slots() - live_before < static_cast<std::size_t>(kN));

  // Migrate back down across the cap: detach the spilled tail to return to inline.
  {
    auto txn = model.transact("collapse");
    for (std::size_t i = 20; i-- > cap;) {
      txn.detach_layer(comp, l[i]);
    }
    txn.detach_layer(comp, l[20]);
    REQUIRE(txn.commit().has_value());
  }
  REQUIRE_FALSE(model.current()->find_composition(comp)->spill_root.valid());
  REQUIRE(order_of(model.current(), comp) == inline_order); // lossless round-trip
}

// enforces: 14-data-model-and-editing#membership-spills-past-inline-cap
TEST_CASE("crossing the cap allocates chunk slots; collapsing back reclaims them after drain") {
  arbc::Model model;
  const std::vector<arbc::ObjectId> l = seed_layers(model, 9);
  arbc::ObjectId comp;
  {
    auto txn = model.transact();
    comp = txn.add_composition(100.0, 100.0);
    for (std::size_t i = 0; i < arbc::k_max_inline_layers; ++i) {
      txn.attach_layer(comp, l[i]); // inline baseline of 8
    }
    REQUIRE(txn.commit().has_value());
  }
  model.drain();
  const std::size_t live_inline = model.live_slots();

  // Attach the 9th (already-created) layer -> spills; live slots rise.
  {
    auto txn = model.transact("cross");
    txn.attach_layer(comp, l[8]);
    REQUIRE(txn.commit().has_value());
  }
  model.drain();
  REQUIRE(model.live_slots() > live_inline); // chunk objects allocated

  // Detach it -> collapses to inline; the chunk slots are reclaimed after drain.
  {
    auto txn = model.transact("collapse");
    txn.detach_layer(comp, l[8]);
    REQUIRE(txn.commit().has_value());
  }
  model.drain();
  REQUIRE(model.live_slots() == live_inline); // back to the inline structure exactly
}

// enforces: 14-data-model-and-editing#membership-edit-damages-composition
TEST_CASE("each attach/detach/reorder raises the revision once and damages the composition once") {
  arbc::Model model;
  const std::vector<arbc::ObjectId> l = seed_layers(model, 3);
  arbc::ObjectId comp;
  {
    auto txn = model.transact();
    comp = txn.add_composition(100.0, 100.0);
    txn.attach_layer(comp, l[0]);
    txn.attach_layer(comp, l[1]);
    REQUIRE(txn.commit().has_value());
  }

  // Install the damage sink after seeding so each measured commit is isolated.
  RecordingDamageSink dsink;
  model.set_damage_sink(&dsink);

  auto expect_one_composition_damage = [&](int expected_calls, std::uint64_t rev_before) {
    REQUIRE(dsink.calls == expected_calls);
    REQUIRE(dsink.last.size() == 1);
    REQUIRE(dsink.last.front().object == comp);
    // Footprint upgraded by model.damage: a membership edit is structural, so it
    // spans the whole composition (infinite rect) at all instants (all() range).
    // Count and keying are unchanged (still exactly one, keyed to the composition).
    REQUIRE(dsink.last.front().rect == arbc::Rect::infinite());
    REQUIRE(dsink.last.front().range == arbc::TimeRange::all());
    REQUIRE(model.current()->revision() == rev_before + 1);
  };

  std::uint64_t rev = model.current()->revision();
  {
    auto txn = model.transact("attach");
    txn.attach_layer(comp, l[2], 0);
    REQUIRE(txn.commit().has_value());
  }
  expect_one_composition_damage(1, rev);

  rev = model.current()->revision();
  {
    auto txn = model.transact("detach");
    txn.detach_layer(comp, l[2]);
    REQUIRE(txn.commit().has_value());
  }
  expect_one_composition_damage(2, rev);

  rev = model.current()->revision();
  {
    auto txn = model.transact("reorder");
    txn.reorder_layer(comp, 0, 1);
    REQUIRE(txn.commit().has_value());
  }
  expect_one_composition_damage(3, rev);
}

// enforces: 14-data-model-and-editing#membership-undo-round-trips
TEST_CASE("undo/redo of attach/detach/reorder round-trips the order across the spill boundary") {
  arbc::Model model;
  const std::size_t cap = arbc::k_max_inline_layers;
  const std::vector<arbc::ObjectId> l = seed_layers(model, 12);

  // Install the journal after seeding so the layer creation is not an undoable
  // entry -- the membership edits below are the only history under test.
  arbc::Journal journal(model);
  model.set_commit_sink(&journal);
  NoopDamageSink dsink;
  model.set_damage_sink(&dsink);

  arbc::ObjectId comp;
  {
    auto txn = model.transact("comp"); // commit index 0
    comp = txn.add_composition(100.0, 100.0);
    REQUIRE(txn.commit().has_value());
  }

  std::vector<std::vector<arbc::ObjectId>> snaps;
  snaps.push_back(order_of(model.current(), comp)); // after commit 0: empty

  {
    auto txn = model.transact("inline"); // commit 1: fill to the cap (inline)
    for (std::size_t i = 0; i < cap; ++i) {
      txn.attach_layer(comp, l[i]);
    }
    REQUIRE(txn.commit().has_value());
  }
  snaps.push_back(order_of(model.current(), comp));

  {
    auto txn = model.transact("spill"); // commit 2: cross the cap
    for (std::size_t i = cap; i < 12; ++i) {
      txn.attach_layer(comp, l[i]);
    }
    REQUIRE(txn.commit().has_value());
  }
  snaps.push_back(order_of(model.current(), comp));

  {
    auto txn = model.transact("collapse"); // commit 3: drop back below the cap
    for (std::size_t i = 12; i-- > cap;) {
      txn.detach_layer(comp, l[i]);
    }
    REQUIRE(txn.commit().has_value());
  }
  snaps.push_back(order_of(model.current(), comp));

  {
    auto txn = model.transact("reorder"); // commit 4
    txn.reorder_layer(comp, 0, cap - 1);
    REQUIRE(txn.commit().has_value());
  }
  snaps.push_back(order_of(model.current(), comp));

  const std::size_t depth = journal.depth();
  REQUIRE(depth == 5);

  // Undo back to just after commit 0; each undo is an ordinary navigation publish
  // that restores the prior order exactly, and never appends a new entry.
  for (std::size_t k = snaps.size() - 1; k > 0; --k) {
    REQUIRE(journal.undo());
    REQUIRE(order_of(model.current(), comp) == snaps[k - 1]);
    REQUIRE(journal.depth() == depth); // no new entry type / no re-journaling
  }
  // Redo forward to the tip.
  for (std::size_t k = 1; k < snaps.size(); ++k) {
    REQUIRE(journal.redo());
    REQUIRE(order_of(model.current(), comp) == snaps[k]);
    REQUIRE(journal.depth() == depth);
  }
}

TEST_CASE("membership mutators no-op on absent composition, absent/non-member layer, bad index") {
  arbc::Model model;
  const std::vector<arbc::ObjectId> l = seed_layers(model, 2);
  arbc::ObjectId comp;
  {
    auto txn = model.transact();
    comp = txn.add_composition(100.0, 100.0);
    txn.attach_layer(comp, l[0]);
    REQUIRE(txn.commit().has_value());
  }
  const std::vector<arbc::ObjectId> base = order_of(model.current(), comp);

  {
    auto txn = model.transact();
    txn.attach_layer(arbc::ObjectId{424242}, l[1]);    // absent composition
    txn.attach_layer(comp, arbc::ObjectId{999999}, 0); // absent layer
    txn.attach_layer(l[0], l[1], 0);                   // target is a layer, not a composition
    txn.detach_layer(comp, l[1]);                      // l1 is not a member
    txn.reorder_layer(comp, 0, 5);                     // to_index out of range
    txn.reorder_layer(comp, 5, 0);                     // from_index out of range
    txn.reorder_layer(comp, 0, 0);                     // equal indices
    REQUIRE(txn.commit().has_value());
  }
  REQUIRE(order_of(model.current(), comp) == base); // membership unchanged
}

// Concurrency smoke (doc 16 tier 6, asan lane): a reader pins and walks the
// composition-scoped accessor while the writer runs attach/detach/reorder cycles
// that migrate across the spill boundary. No torn read, no use-after-free. The
// full seeded schedule-perturbation stress lives in quality.stress_harness.
TEST_CASE("concurrent pin/traverse of composition membership against a mutating writer") {
  arbc::Model model;
  NoopDamageSink dsink;
  model.set_damage_sink(&dsink);

  constexpr int kBase = 8; // exactly the inline cap
  const std::vector<arbc::ObjectId> l = seed_layers(model, kBase + 1);
  std::vector<std::uint64_t> known;
  for (const arbc::ObjectId id : l) {
    known.push_back(id.value);
  }
  arbc::ObjectId comp;
  {
    auto txn = model.transact();
    comp = txn.add_composition(100.0, 100.0);
    for (int i = 0; i < kBase; ++i) {
      txn.attach_layer(comp, l[i]);
    }
    REQUIRE(txn.commit().has_value());
  }
  model.drain();

  auto is_known = [&](std::uint64_t v) {
    for (const std::uint64_t k : known) {
      if (k == v) {
        return true;
      }
    }
    return false;
  };

  constexpr int writer_iterations = 2000;
  std::atomic<bool> go{false};
  std::atomic<bool> stop{false};
  std::atomic<bool> bad{false};

  std::thread reader([&] {
    while (!go.load(std::memory_order_acquire)) {
    }
    while (!stop.load(std::memory_order_acquire)) {
      const arbc::DocStatePtr pinned = model.current();
      std::vector<arbc::ObjectId> seen = order_of(pinned, comp);
      if (seen.size() < static_cast<std::size_t>(kBase) ||
          seen.size() > static_cast<std::size_t>(kBase + 1)) {
        bad.store(true, std::memory_order_relaxed);
      }
      for (const arbc::ObjectId id : seen) {
        if (!is_known(id.value)) {
          bad.store(true, std::memory_order_relaxed);
        }
      }
    }
  });

  go.store(true, std::memory_order_release);
  for (int i = 0; i < writer_iterations; ++i) {
    {
      auto txn = model.transact("cross");
      txn.attach_layer(comp, l[kBase]); // 9th member -> spill migration
      REQUIRE(txn.commit().has_value());
    }
    model.drain();
    {
      auto txn = model.transact("reorder");
      txn.reorder_layer(comp, 0, kBase); // move within the spilled order
      REQUIRE(txn.commit().has_value());
    }
    model.drain();
    {
      auto txn = model.transact("collapse");
      txn.detach_layer(comp, l[kBase]); // back to inline
      REQUIRE(txn.commit().has_value());
    }
    model.drain();
  }
  stop.store(true, std::memory_order_release);
  reader.join();

  REQUIRE_FALSE(bad.load());
  REQUIRE(order_of(model.current(), comp).size() == static_cast<std::size_t>(kBase));
}

} // namespace
