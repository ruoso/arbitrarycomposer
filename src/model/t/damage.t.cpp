#include <arbc/model/damage.hpp>
#include <arbc/model/journal.hpp>
#include <arbc/model/model.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

namespace {

// Behavioral-counter double (doc 16:54-62): count flushes, keep the last payload
// so a test can witness the assembled damage set / footprint crossing the seam.
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

const arbc::ObjectId k_dummy_content{0xC0FFEE};

// Assert the sink's most recent flush is exactly one structural damage keyed to
// `id`, spanning the whole object (infinite rect) at all instants (all() range).
void expect_structural(const RecordingDamageSink& dsink, arbc::ObjectId id) {
  REQUIRE(dsink.last.size() == 1);
  REQUIRE(dsink.last.front().object == id);
  REQUIRE(dsink.last.front().rect == arbc::Rect::infinite());
  REQUIRE(dsink.last.front().range == arbc::TimeRange::all());
}

// ---- The empty=identity / whole=absorbing convention on both axes ----------

TEST_CASE("rect_union and range_union: empty is identity, infinite/all is absorbing") {
  const arbc::Rect finite{1.0, 2.0, 5.0, 8.0};
  const arbc::Rect other{3.0, 0.0, 4.0, 10.0};

  // Empty rect contributes nothing (identity), either side.
  REQUIRE(arbc::rect_union(finite, arbc::Rect{}) == finite);
  REQUIRE(arbc::rect_union(arbc::Rect{}, finite) == finite);
  // Two finite rects fold to their bounding box.
  REQUIRE(arbc::rect_union(finite, other) == arbc::Rect{1.0, 0.0, 5.0, 10.0});
  // An infinite rect is absorbing under union (min/max reach the infinities).
  REQUIRE(arbc::rect_union(finite, arbc::Rect::infinite()) == arbc::Rect::infinite());
  REQUIRE(arbc::rect_union(arbc::Rect::infinite(), finite) == arbc::Rect::infinite());

  const arbc::TimeRange r1{arbc::Time{10}, arbc::Time{20}};
  const arbc::TimeRange r2{arbc::Time{5}, arbc::Time{15}};
  // Empty (degenerate) range is identity -- it does NOT fold a real range toward 0.
  REQUIRE(arbc::range_union(r1, arbc::TimeRange{}) == r1);
  REQUIRE(arbc::range_union(arbc::TimeRange{}, r1) == r1);
  REQUIRE(arbc::range_union(r1, arbc::TimeRange{arbc::Time{7}, arbc::Time{7}}) == r1); // end<=start
  // Two finite ranges fold to [min start, max end].
  REQUIRE(arbc::range_union(r1, r2) == arbc::TimeRange{arbc::Time{5}, arbc::Time{20}});
  // all() is absorbing.
  REQUIRE(arbc::range_union(r1, arbc::TimeRange::all()) == arbc::TimeRange::all());
  REQUIRE(arbc::range_union(arbc::TimeRange::all(), r1) == arbc::TimeRange::all());
}

// ---- Structural auto-damage: whole object, all time, keyed to the edited id --

// enforces: 01-core-concepts#placement-change-auto-damages
TEST_CASE("each placement/graph mutator flushes one whole-object all-time damage for the edited id") {
  SECTION("set_transform keys to the layer") {
    arbc::Model model;
    arbc::ObjectId layer;
    {
      auto txn = model.transact();
      layer = txn.add_layer(k_dummy_content, arbc::Affine::identity());
      REQUIRE(txn.commit().has_value());
    }
    RecordingDamageSink dsink; // install after seeding: measure only the edit
    model.set_damage_sink(&dsink);
    {
      auto txn = model.transact("move");
      txn.set_transform(layer, arbc::Affine::translation(3.0, 4.0));
      REQUIRE(txn.commit().has_value());
    }
    REQUIRE(dsink.calls == 1);
    expect_structural(dsink, layer);
  }

  SECTION("set_opacity keys to the layer") {
    arbc::Model model;
    arbc::ObjectId layer;
    {
      auto txn = model.transact();
      layer = txn.add_layer(k_dummy_content, arbc::Affine::identity());
      REQUIRE(txn.commit().has_value());
    }
    RecordingDamageSink dsink;
    model.set_damage_sink(&dsink);
    {
      auto txn = model.transact("fade");
      txn.set_opacity(layer, 0.5);
      REQUIRE(txn.commit().has_value());
    }
    REQUIRE(dsink.calls == 1);
    expect_structural(dsink, layer);
  }

  SECTION("attach/detach/reorder key to the composition") {
    arbc::Model model;
    arbc::ObjectId comp;
    std::vector<arbc::ObjectId> l;
    {
      auto txn = model.transact();
      comp = txn.add_composition(100.0, 100.0);
      for (int i = 0; i < 3; ++i) {
        l.push_back(txn.add_layer(k_dummy_content, arbc::Affine::identity()));
      }
      txn.attach_layer(comp, l[0]);
      txn.attach_layer(comp, l[1]);
      REQUIRE(txn.commit().has_value());
    }
    RecordingDamageSink dsink;
    model.set_damage_sink(&dsink);

    {
      auto txn = model.transact("attach");
      txn.attach_layer(comp, l[2], 0);
      REQUIRE(txn.commit().has_value());
    }
    expect_structural(dsink, comp);
    {
      auto txn = model.transact("detach");
      txn.detach_layer(comp, l[2]);
      REQUIRE(txn.commit().has_value());
    }
    expect_structural(dsink, comp);
    {
      auto txn = model.transact("reorder");
      txn.reorder_layer(comp, 0, 1);
      REQUIRE(txn.commit().has_value());
    }
    expect_structural(dsink, comp);
  }

  SECTION("remove keys to the removed object") {
    arbc::Model model;
    arbc::ObjectId layer;
    {
      auto txn = model.transact();
      layer = txn.add_layer(k_dummy_content, arbc::Affine::identity());
      REQUIRE(txn.commit().has_value());
    }
    RecordingDamageSink dsink;
    model.set_damage_sink(&dsink);
    {
      auto txn = model.transact("remove");
      txn.remove(layer);
      REQUIRE(txn.commit().has_value());
    }
    REQUIRE(dsink.calls == 1);
    expect_structural(dsink, layer);
  }
}

// enforces: 14-data-model-and-editing#structural-damage-spans-all-time
TEST_CASE("structural damage carries the all() range so a temporal consumer never skips it") {
  arbc::Model model;
  arbc::ObjectId layer;
  {
    auto txn = model.transact();
    layer = txn.add_layer(k_dummy_content, arbc::Affine::identity());
    REQUIRE(txn.commit().has_value());
  }
  RecordingDamageSink dsink;
  model.set_damage_sink(&dsink);
  {
    auto txn = model.transact("move");
    txn.set_transform(layer, arbc::Affine::translation(1.0, 0.0));
    REQUIRE(txn.commit().has_value());
  }
  REQUIRE(dsink.last.size() == 1);
  const arbc::TimeRange range = dsink.last.front().range;
  REQUIRE(range == arbc::TimeRange::all());
  REQUIRE_FALSE(range.empty());
  // A lookahead window at ANY instant sees the structural edit inside it -- an
  // unbounded range contains every finite instant (doc 14:213-217).
  REQUIRE(range.contains(arbc::Time{-1'000'000}));
  REQUIRE(range.contains(arbc::Time::zero()));
  REQUIRE(range.contains(arbc::Time{1'000'000}));
}

// ---- Content auto-damage: caller-supplied, floor removed --------------------

// enforces: 14-data-model-and-editing#damage-carries-region-and-time
TEST_CASE("a caller-supplied content damage survives commit -> flush -> journal -> undo/redo bit-identical") {
  arbc::Model model;
  arbc::Journal journal(model);
  model.set_commit_sink(&journal);
  RecordingDamageSink dsink;
  model.set_damage_sink(&dsink);

  arbc::ObjectId content;
  {
    auto txn = model.transact("add-content");
    content = txn.add_content(0);
    REQUIRE(txn.commit().has_value());
  }
  model.drain();

  // The kind's L3 Editable supplies the precise touched region + temporal extent.
  const arbc::Damage precise{content, arbc::Rect{4.0, 4.0, 12.0, 20.0},
                             arbc::TimeRange{arbc::Time{100}, arbc::Time{200}}};
  {
    auto txn = model.transact("edit");
    txn.set_content_state(content, arbc::StateHandle{7});
    txn.add_damage(precise);
    REQUIRE(txn.commit().has_value());
  }

  // Commit flush: the model added no coarse floor that widens or clobbers it.
  REQUIRE(dsink.last.size() == 1);
  REQUIRE(dsink.last.front() == precise);
  // The journal entry stored the same damage set.
  REQUIRE(journal.entry_at(journal.depth() - 1).damage.size() == 1);
  REQUIRE(journal.entry_at(journal.depth() - 1).damage.front() == precise);

  // Undo replays the entry's damage bit-identically (no diffing, doc 14:108-110).
  REQUIRE(journal.undo());
  REQUIRE(dsink.last.size() == 1);
  REQUIRE(dsink.last.front() == precise);
  // Redo is the symmetric forward publish, same footprint.
  REQUIRE(journal.redo());
  REQUIRE(dsink.last.size() == 1);
  REQUIRE(dsink.last.front() == precise);
}

TEST_CASE("set_content_state alone flushes no content damage (the coarse floor is removed)") {
  arbc::Model model;
  arbc::ObjectId content;
  {
    auto txn = model.transact();
    content = txn.add_content(0);
    REQUIRE(txn.commit().has_value());
  }
  RecordingDamageSink dsink; // install after seeding
  model.set_damage_sink(&dsink);
  {
    auto txn = model.transact("capture");
    txn.set_content_state(content, arbc::StateHandle{9}); // no add_damage
    REQUIRE(txn.commit().has_value());
  }
  REQUIRE(dsink.calls == 1);      // commit still flushes exactly once...
  REQUIRE(dsink.last.empty());    // ...but with no content damage keyed to it
}

TEST_CASE("two caller damages on one content union to the bbox / [min,max] within a transaction") {
  arbc::Model model;
  arbc::ObjectId content;
  {
    auto txn = model.transact();
    content = txn.add_content(0);
    REQUIRE(txn.commit().has_value());
  }
  RecordingDamageSink dsink;
  model.set_damage_sink(&dsink);
  {
    auto txn = model.transact("multi-stroke");
    txn.add_damage(arbc::Damage{content, arbc::Rect{1.0, 2.0, 5.0, 8.0},
                                arbc::TimeRange{arbc::Time{10}, arbc::Time{20}}});
    txn.add_damage(arbc::Damage{content, arbc::Rect{3.0, 0.0, 4.0, 10.0},
                                arbc::TimeRange{arbc::Time{5}, arbc::Time{15}}});
    // An empty-rect / empty-range damage is identity: it must not widen or reset.
    txn.add_damage(arbc::Damage{content, arbc::Rect{}, arbc::TimeRange{}});
    REQUIRE(txn.commit().has_value());
  }
  REQUIRE(dsink.calls == 1);
  REQUIRE(dsink.last.size() == 1); // deduped to one record for the content id
  REQUIRE(dsink.last.front().object == content);
  REQUIRE(dsink.last.front().rect == arbc::Rect{1.0, 0.0, 5.0, 10.0});
  REQUIRE(dsink.last.front().range == arbc::TimeRange{arbc::Time{5}, arbc::Time{20}});
}

TEST_CASE("an infinite rect / all() range from any caller is absorbing within a transaction") {
  arbc::Model model;
  arbc::ObjectId content;
  {
    auto txn = model.transact();
    content = txn.add_content(0);
    REQUIRE(txn.commit().has_value());
  }
  RecordingDamageSink dsink;
  model.set_damage_sink(&dsink);
  {
    auto txn = model.transact("whole-then-fine");
    txn.add_damage(arbc::Damage{content, arbc::Rect::infinite(), arbc::TimeRange::all()});
    txn.add_damage(arbc::Damage{content, arbc::Rect{1.0, 2.0, 5.0, 8.0},
                                arbc::TimeRange{arbc::Time{10}, arbc::Time{20}}});
    REQUIRE(txn.commit().has_value());
  }
  REQUIRE(dsink.last.size() == 1);
  REQUIRE(dsink.last.front().rect == arbc::Rect::infinite());
  REQUIRE(dsink.last.front().range == arbc::TimeRange::all());
}

// ---- Behavioral counters (doc 16:54-62 -- counters, never wall-clock) -------

TEST_CASE("behavioral counters: N placement mutations -> one flush, one record per edited object") {
  arbc::Model model;
  RecordingDamageSink dsink;
  model.set_damage_sink(&dsink);

  arbc::ObjectId comp;
  arbc::ObjectId layerA;
  arbc::ObjectId layerB;
  {
    auto txn = model.transact();
    comp = txn.add_composition(100.0, 100.0);
    layerA = txn.add_layer(k_dummy_content, arbc::Affine::identity());
    layerB = txn.add_layer(k_dummy_content, arbc::Affine::identity());
    txn.attach_layer(comp, layerA);
    txn.attach_layer(comp, layerB);
    REQUIRE(txn.commit().has_value());
  }
  const std::uint64_t rev = model.current()->revision();
  const int flushes = dsink.calls;

  // One commit touching three distinct objects (transform, opacity, order).
  {
    auto txn = model.transact("batch");
    txn.set_transform(layerA, arbc::Affine::translation(1.0, 0.0));
    txn.set_opacity(layerB, 0.25);
    txn.reorder_layer(comp, 0, 1);
    REQUIRE(txn.commit().has_value());
  }
  REQUIRE(model.current()->revision() == rev + 1); // exactly +1 for N mutations
  REQUIRE(dsink.calls == flushes + 1);             // exactly one flush
  REQUIRE(dsink.last.size() == 3);                 // one record per distinct edited object
  for (const arbc::Damage& d : dsink.last) {
    REQUIRE(d.rect == arbc::Rect::infinite());
    REQUIRE(d.range == arbc::TimeRange::all());
  }

  // An aborted transaction flushes nothing and does not publish.
  const int flushes_after = dsink.calls;
  const std::uint64_t rev_after = model.current()->revision();
  {
    auto txn = model.transact("discarded");
    txn.set_opacity(layerA, 0.9);
    txn.abort();
  }
  REQUIRE(dsink.calls == flushes_after);            // zero flushes on abort
  REQUIRE(model.current()->revision() == rev_after); // no publish
}

// Concurrency smoke (doc 16 tier 6, asan lane): the writer commits refined
// (whole-object / all-time + caller-finite) damage through a no-op sink while a
// reader pins -> traverses via peek -> unpins. No torn read, no use-after-free.
// The full seeded stress lives in quality.stress_harness (not duplicated here).
TEST_CASE("concurrent pin/traverse against a writer emitting refined damage") {
  arbc::Model model;
  NoopDamageSink dsink;
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
    txn.set_transform(keep, arbc::Affine::scaling(s, s)); // whole-object / all-time
    txn.set_opacity(keep, (i % 2 == 0) ? 0.5 : 1.0);      // second structural edit, same id
    const arbc::ObjectId scratch = txn.add_content(0);
    txn.add_damage(arbc::Damage{scratch, arbc::Rect{0.0, 0.0, 8.0, 8.0},
                                arbc::TimeRange{arbc::Time{0}, arbc::Time{16}}}); // caller-finite
    txn.remove(scratch);
    REQUIRE(txn.commit().has_value());
    model.drain(); // single-drainer: only the writer thread drains
  }
  stop.store(true, std::memory_order_release);
  reader.join();

  REQUIRE_FALSE(bad.load());
  REQUIRE(model.current()->find_layer(keep) != nullptr);
}

} // namespace
