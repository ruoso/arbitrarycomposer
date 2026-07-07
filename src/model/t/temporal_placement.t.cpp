#include <arbc/model/model.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

// Temporal placement on a layer record (timeline.temporal_placement, doc
// 11:59-93, doc 01:33-36): two POD fields -- a parent-time `span` and a 1D
// affine `time_map` -- beside the spatial `transform`, plus the transactional
// `set_span`/`set_time_map` setters that ride the existing path-copy substrate.
// This task stores and mutates only: no span culling, no time-map composition,
// no rounding (those are timeline.transport / pipeline work). The tests below
// witness the still-default, the round-trips (including reverse playback under a
// negative rate), structural sharing, no-op targeting, and the revision/publish
// counters -- byte-exact field comparison, behavioral counters, never
// wall-clock (doc 16:54-62).

namespace {

struct RecordingCommitSink final : arbc::CommitSink {
  int calls = 0;
  void on_commit(arbc::JournalEntry) override { ++calls; }
};

struct RecordingDamageSink final : arbc::DamageSink {
  int calls = 0;
  std::vector<arbc::Damage> last;
  void flush(const std::vector<arbc::Damage>& damage) override {
    ++calls;
    last = damage;
  }
};

const arbc::ObjectId k_dummy_content{0xC0FFEE};

// A single 24 fps output frame in flicks (705'600'000 / 24) -- an arbitrary,
// exactly representable duration for a "flash" span one frame long.
constexpr std::int64_t k_one_frame_24fps = arbc::Time::flicks_per_second / 24;

// enforces: 11-time-and-video#temporal-placement-lives-on-layer-defaults-all-present
TEST_CASE("a layer added with no temporal placement is a still: all-present span, identity map") {
  arbc::Model model;
  arbc::ObjectId layer;
  {
    auto txn = model.transact("add");
    layer = txn.add_layer(k_dummy_content, arbc::Affine::identity());
    REQUIRE(txn.commit().has_value());
  }

  const arbc::LayerRecord* rec = model.current()->find_layer(layer);
  REQUIRE(rec != nullptr);
  // Span default is the whole timeline -- the degenerate, always-present case.
  REQUIRE(rec->span == arbc::TimeRange::all());
  // Time map default is the identity: in=0, rate=1/1, offset=0 => local==parent.
  REQUIRE(rec->time_map == arbc::TimeMap{});
  REQUIRE(rec->time_map.in == arbc::Time{0});
  REQUIRE(rec->time_map.rate == arbc::Rational{1, 1});
  REQUIRE(rec->time_map.offset == arbc::Time{0});
}

TEST_CASE("set_span round-trips an arbitrary half-open span, including a one-frame flash") {
  arbc::Model model;
  arbc::ObjectId layer;
  {
    auto txn = model.transact();
    layer = txn.add_layer(k_dummy_content, arbc::Affine::identity());
    REQUIRE(txn.commit().has_value());
  }

  SECTION("an arbitrary [in, out)") {
    const arbc::TimeRange span{arbc::Time{123'456}, arbc::Time{987'654'321}};
    {
      auto txn = model.transact("span");
      txn.set_span(layer, span);
      REQUIRE(txn.commit().has_value());
    }
    REQUIRE(model.current()->find_layer(layer)->span == span);
  }

  SECTION("a single-output-frame-long flash span") {
    const arbc::Time in{1'000'000};
    const arbc::TimeRange flash{in, arbc::Time{in.flicks + k_one_frame_24fps}};
    {
      auto txn = model.transact("flash");
      txn.set_span(layer, flash);
      REQUIRE(txn.commit().has_value());
    }
    const arbc::LayerRecord* rec = model.current()->find_layer(layer);
    REQUIRE(rec->span == flash);
    REQUIRE_FALSE(rec->span.empty()); // out > in: a real, non-degenerate span
  }
}

TEST_CASE("set_time_map round-trips an arbitrary map, negative rate is first-class") {
  arbc::Model model;
  arbc::ObjectId layer;
  {
    auto txn = model.transact();
    layer = txn.add_layer(k_dummy_content, arbc::Affine::identity());
    REQUIRE(txn.commit().has_value());
  }

  // A reverse-playback map at exactly -24000/1001 (doc 11:65-67): negative rate,
  // non-zero in/offset -- stored verbatim, unrounded, uncomposed.
  const arbc::TimeMap map{arbc::Time{7'000}, arbc::Rational{-24'000, 1'001}, arbc::Time{-42}};
  {
    auto txn = model.transact("retime");
    txn.set_time_map(layer, map);
    REQUIRE(txn.commit().has_value());
  }
  const arbc::LayerRecord* rec = model.current()->find_layer(layer);
  REQUIRE(rec->time_map == map);
  REQUIRE(rec->time_map.rate.num() == -24'000); // sign carried in the numerator
  REQUIRE(rec->time_map.rate.den() == 1'001);
  // The span is untouched by a time-map edit (granular per-field setters).
  REQUIRE(rec->span == arbc::TimeRange::all());
}

TEST_CASE("setting placement on one layer leaves siblings and its own other fields untouched") {
  arbc::Model model;
  arbc::ObjectId a;
  arbc::ObjectId b;
  const arbc::Affine a_transform = arbc::Affine::translation(3.0, 4.0);
  {
    auto txn = model.transact();
    a = txn.add_layer(k_dummy_content, a_transform, 0.25);
    b = txn.add_layer(k_dummy_content, arbc::Affine::identity(), 0.5);
    REQUIRE(txn.commit().has_value());
  }
  model.drain();

  // Sibling `b`'s record edge before the edit, to prove it is shared, not copied.
  const arbc::DocStatePtr before = model.current();
  arbc::SlotRef<arbc::ObjectRecord> b_before;
  REQUIRE(before->record_edge(b, b_before));

  const arbc::TimeRange span{arbc::Time{10}, arbc::Time{20}};
  {
    auto txn = model.transact("span a");
    txn.set_span(a, span);
    REQUIRE(txn.commit().has_value());
  }

  const arbc::DocStatePtr after = model.current();

  // `a` got the new span but keeps its transform / opacity / flags intact.
  const arbc::LayerRecord* ra = after->find_layer(a);
  REQUIRE(ra->span == span);
  REQUIRE(ra->transform == a_transform);
  REQUIRE(ra->opacity == 0.25);
  REQUIRE(ra->flags == arbc::k_layer_visible);
  REQUIRE(ra->time_map == arbc::TimeMap{}); // the untouched sibling field on `a`

  // `b` is untouched: same placement AND the same slab slot (structural sharing).
  const arbc::LayerRecord* rb = after->find_layer(b);
  REQUIRE(rb->span == arbc::TimeRange::all());
  REQUIRE(rb->opacity == 0.5);
  arbc::SlotRef<arbc::ObjectRecord> b_after;
  REQUIRE(after->record_edge(b, b_after));
  REQUIRE(b_after == b_before); // unedited sibling shared by SlotRef identity
}

TEST_CASE("set_span / set_time_map on an absent or non-layer id is a no-op") {
  arbc::Model model;
  arbc::ObjectId layer;
  arbc::ObjectId content;
  {
    auto txn = model.transact();
    content = txn.add_content(0);
    layer = txn.add_layer(content, arbc::Affine::identity());
    REQUIRE(txn.commit().has_value());
  }
  const std::uint64_t rev = model.current()->revision();

  {
    auto txn = model.transact("no-op");
    txn.set_span(arbc::ObjectId{424242}, arbc::TimeRange{arbc::Time{1}, arbc::Time{2}}); // absent
    txn.set_time_map(content, arbc::TimeMap{arbc::Time{5}, arbc::Rational{2, 1}, {}}); // not a layer
    REQUIRE(txn.commit().has_value());
  }

  // Nothing threw/aborted; the layer keeps its still-default, the content stands.
  const arbc::LayerRecord* rec = model.current()->find_layer(layer);
  REQUIRE(rec != nullptr);
  REQUIRE(rec->span == arbc::TimeRange::all());
  REQUIRE(rec->time_map == arbc::TimeMap{});
  REQUIRE(model.current()->find_content(content) != nullptr);
  REQUIRE(model.current()->revision() == rev + 1); // commit still published once
}

TEST_CASE("behavioral counters: a committed placement edit bumps revision by exactly 1") {
  arbc::Model model;
  RecordingCommitSink csink;
  RecordingDamageSink dsink;
  model.set_commit_sink(&csink);
  model.set_damage_sink(&dsink);

  arbc::ObjectId layer;
  {
    auto txn = model.transact();
    layer = txn.add_layer(k_dummy_content, arbc::Affine::identity());
    REQUIRE(txn.commit().has_value());
  }
  const std::uint64_t rev0 = model.current()->revision();
  const int commits0 = csink.calls;

  {
    auto txn = model.transact("span");
    txn.set_span(layer, arbc::TimeRange{arbc::Time{0}, arbc::Time{100}});
    REQUIRE(txn.commit().has_value());
  }
  REQUIRE(model.current()->revision() == rev0 + 1); // exactly +1
  REQUIRE(csink.calls == commits0 + 1);             // exactly one entry
}

TEST_CASE("behavioral counters: two coalesced placement edits in one transaction publish once") {
  arbc::Model model;
  RecordingCommitSink csink;
  RecordingDamageSink dsink;
  model.set_commit_sink(&csink);
  model.set_damage_sink(&dsink);

  arbc::ObjectId layer;
  {
    auto txn = model.transact();
    layer = txn.add_layer(k_dummy_content, arbc::Affine::identity());
    REQUIRE(txn.commit().has_value());
  }
  const std::uint64_t rev0 = model.current()->revision();
  const int commits0 = csink.calls;
  const int damage0 = dsink.calls;

  // Span AND time-map edits to the SAME layer, in one transaction: they touch
  // one object, so the commit path-copies once and publishes once.
  const arbc::TimeRange span{arbc::Time{10}, arbc::Time{20}};
  const arbc::TimeMap map{arbc::Time{3}, arbc::Rational{1, 2}, arbc::Time{1}};
  {
    auto txn = model.transact("retime");
    txn.set_span(layer, span);
    txn.set_time_map(layer, map);
    REQUIRE(txn.commit().has_value());
  }

  REQUIRE(model.current()->revision() == rev0 + 1); // one publish for both edits
  REQUIRE(csink.calls == commits0 + 1);             // one journal entry
  REQUIRE(dsink.calls == damage0 + 1);              // one damage flush
  REQUIRE(dsink.last.size() == 1);                  // deduped by object: just {layer}

  // Both edits landed on the final record.
  const arbc::LayerRecord* rec = model.current()->find_layer(layer);
  REQUIRE(rec->span == span);
  REQUIRE(rec->time_map == map);
}

} // namespace
