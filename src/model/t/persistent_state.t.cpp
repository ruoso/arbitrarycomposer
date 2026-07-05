#include <arbc/model/model.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

namespace {

// Build a document of `count` layers (ids 1..count, all bound to one dummy
// content) on a fresh model; return the layer ids.
std::vector<arbc::ObjectId> build_layers(arbc::Model& model, int count) {
  std::vector<arbc::ObjectId> ids;
  auto txn = model.transact();
  const arbc::ObjectId content{0xC0FFEE};
  for (int i = 0; i < count; ++i) {
    ids.push_back(txn.add_layer(content, arbc::Affine::identity()));
  }
  REQUIRE(txn.commit().has_value());
  // Reclaim the intermediate working roots superseded while building (a
  // multi-insert transaction path-copies the root once per insert), so callers
  // that assert exact live-slot deltas start from a clean baseline.
  model.drain();
  return ids;
}

TEST_CASE("insert / update / lookup across versions with a pin held") {
  arbc::Model model;
  const std::vector<arbc::ObjectId> ids = build_layers(model, 5);

  // A pin taken before the update still resolves the OLD record; the new version
  // resolves the updated one.
  const arbc::DocStatePtr before = model.current();
  const arbc::LayerRecord* old_record = before->find_layer(ids[2]);
  REQUIRE(old_record != nullptr);
  REQUIRE(old_record->transform == arbc::Affine::identity());

  {
    auto txn = model.transact();
    txn.set_transform(ids[2], arbc::Affine::translation(4.0, 4.0));
    REQUIRE(txn.commit().has_value());
  }

  // Old pin: unchanged. New version: updated. Every other id resolves in both.
  REQUIRE(before->find_layer(ids[2])->transform == arbc::Affine::identity());
  const arbc::DocStatePtr after = model.current();
  REQUIRE(after->find_layer(ids[2])->transform == arbc::Affine::translation(4.0, 4.0));
  for (const arbc::ObjectId id : ids) {
    REQUIRE(before->contains(id));
    REQUIRE(after->contains(id));
  }
  REQUIRE_FALSE(after->contains(arbc::ObjectId{9999}));
}

TEST_CASE("pins are root refs: order-independent unpin, last unpin releases the root") {
  arbc::Model model;
  const std::vector<arbc::ObjectId> ids = build_layers(model, 6);

  // Three independent pins on the same (soon-superseded) version.
  arbc::DocStatePtr pin_a = model.current();
  arbc::DocStatePtr pin_b = model.current();
  arbc::DocStatePtr pin_c = model.current();

  // Supersede the pinned version.
  {
    auto txn = model.transact();
    txn.set_transform(ids[0], arbc::Affine::scaling(2.0, 2.0));
    REQUIRE(txn.commit().has_value());
  }
  const std::size_t live_superseded = model.live_slots();

  // Dropping pins in an arbitrary order, while any pin survives, keeps the old
  // version memory-live: a drain reclaims nothing of it.
  pin_b.reset();
  pin_a.reset();
  model.drain();
  REQUIRE(model.live_slots() == live_superseded);
  REQUIRE(pin_c->find_layer(ids[0])->transform == arbc::Affine::identity());

  // The last unpin releases the root; the old version's unique nodes now cascade.
  pin_c.reset();
  model.drain();
  REQUIRE(model.live_slots() < live_superseded);
}

// enforces: 14-data-model-and-editing#commit-shares-untouched-structure
TEST_CASE("a commit path-copies only the touched path and shares untouched records") {
  // Independent of document size: touching one object always grows the live-slot
  // count by exactly the path-copied node count (one branch + one leaf) plus the
  // one new record -- never by the document size.
  auto touched_growth = [](int document_size) -> std::size_t {
    arbc::Model model;
    const std::vector<arbc::ObjectId> ids = build_layers(model, document_size);

    const arbc::DocStatePtr before = model.current();
    arbc::SlotRef<arbc::ObjectRecord> touched_before;
    arbc::SlotRef<arbc::ObjectRecord> shared_before;
    REQUIRE(before->record_edge(ids[0], touched_before));
    REQUIRE(before->record_edge(ids[1], shared_before));

    const std::size_t baseline = model.live_slots();
    {
      auto txn = model.transact();
      txn.set_transform(ids[0], arbc::Affine::translation(1.0, 0.0));
      REQUIRE(txn.commit().has_value());
    }
    const std::size_t growth = model.live_slots() - baseline;

    // SlotRef identity: the untouched record is SHARED between versions; the
    // touched record is a fresh slab.
    const arbc::DocStatePtr after = model.current();
    arbc::SlotRef<arbc::ObjectRecord> touched_after;
    arbc::SlotRef<arbc::ObjectRecord> shared_after;
    REQUIRE(after->record_edge(ids[0], touched_after));
    REQUIRE(after->record_edge(ids[1], shared_after));
    REQUIRE(shared_after == shared_before); // untouched: same slab, shared by identity
    REQUIRE_FALSE(touched_after == touched_before);

    // The pinned pre-commit version still sees the shared record at its old slab.
    arbc::SlotRef<arbc::ObjectRecord> shared_still;
    REQUIRE(before->record_edge(ids[1], shared_still));
    REQUIRE(shared_still == shared_before);
    return growth;
  };

  const std::size_t small = touched_growth(5);
  const std::size_t large = touched_growth(20);
  REQUIRE(small == large);         // O(path depth), NOT O(document size)
  REQUIRE(small < std::size_t{5}); // far below whole-document copy
}

// enforces: 14-data-model-and-editing#dropping-pin-reclaims-only-unique-nodes
TEST_CASE("dropping the last pin to a superseded version reclaims only its unique nodes") {
  arbc::Model model;
  const std::vector<arbc::ObjectId> ids = build_layers(model, 5);

  // Pin version A, then supersede it by touching one layer (version B). While A
  // is pinned, both versions are live and share every untouched leaf.
  arbc::DocStatePtr version_a = model.current();
  {
    auto txn = model.transact();
    txn.set_transform(ids[0], arbc::Affine::scaling(3.0, 3.0));
    REQUIRE(txn.commit().has_value());
  }
  const std::size_t live_both = model.live_slots();

  // Dropping the last (only) pin to A merely ENQUEUES: deferred, so nothing is
  // reclaimed until a drain (doc 15:129-136).
  version_a.reset();
  REQUIRE(model.live_slots() == live_both);

  // One drain reclaims exactly A's unique nodes: its root branch, its old leaf
  // for ids[0], and that leaf's old record -- three slots. Every node still
  // shared with B (the untouched leaves and their records) survives.
  model.drain();
  REQUIRE(model.live_slots() == live_both - 3);

  const arbc::DocStatePtr version_b = model.current();
  REQUIRE(version_b->find_layer(ids[0])->transform == arbc::Affine::scaling(3.0, 3.0));
  for (std::size_t i = 1; i < ids.size(); ++i) {
    REQUIRE(version_b->find_layer(ids[i]) != nullptr); // shared leaves intact
  }
}

// Concurrency smoke (doc 16:66-73): a reader repeatedly pins -> traverses via
// peek -> unpins while the writer commits and drains. No torn read, no
// use-after-free (asserted structurally; asan/tsan lanes catch UAF/races). The
// full seeded schedule-perturbation stress lives in quality.stress_harness.
TEST_CASE("concurrent pin/traverse/unpin against a committing writer stays consistent") {
  arbc::Model model;
  const arbc::ObjectId content{0xABCD};
  arbc::ObjectId layer;
  {
    auto txn = model.transact();
    layer = txn.add_layer(content, arbc::Affine::identity());
    REQUIRE(txn.commit().has_value());
  }

  constexpr int writer_iterations = 4000;
  std::atomic<bool> go{false};
  std::atomic<bool> stop{false};
  std::atomic<bool> bad{false};

  std::thread reader([&] {
    while (!go.load(std::memory_order_acquire)) {
    }
    while (!stop.load(std::memory_order_acquire)) {
      const arbc::DocStatePtr pinned = model.current();
      const arbc::LayerRecord* record = pinned->find_layer(layer);
      if (record == nullptr) {
        bad.store(true, std::memory_order_relaxed);
        continue;
      }
      // Immutable invariants across every version -- a torn read or a
      // use-after-free would corrupt them.
      if (!(record->content == content) || !record->visible()) {
        bad.store(true, std::memory_order_relaxed);
      }
    }
  });

  go.store(true, std::memory_order_release);
  for (int i = 0; i < writer_iterations; ++i) {
    auto txn = model.transact();
    const double s = (i % 2 == 0) ? 2.0 : 3.0;
    txn.set_transform(layer, arbc::Affine::scaling(s, s));
    REQUIRE(txn.commit().has_value());
    model.drain(); // single-drainer: only the writer thread drains
  }
  stop.store(true, std::memory_order_release);
  reader.join();

  REQUIRE_FALSE(bad.load());
  REQUIRE(model.current()->find_layer(layer) != nullptr);
}

} // namespace
