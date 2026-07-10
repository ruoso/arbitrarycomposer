#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/model/damage.hpp>
#include <arbc/model/model.hpp>
#include <arbc/runtime/damage_router.hpp>

#include <catch2/catch_test_macros.hpp>

#include <utility>
#include <vector>

// Unit + behavioral tests for the fan-out damage sink (`arbc::DamageRouter`, doc
// 01 § Invalidation / 17:60). These pin the router mechanics in isolation --
// registration-order fan-out, exactly-once delivery, the zero-registrant no-op,
// and the RAII `Registration` lifetime -- with recording sinks and no render
// stubs. The end-to-end two-`HostViewport` fan-out (over the live host object) is
// enforced in `host_viewport.t.cpp`. Deterministic: only a behavioral
// `deliveries()` counter, never wall-clock (doc 16:54-62).

namespace {

using arbc::Damage;
using arbc::DamageRouter;
using arbc::ObjectId;

// A child sink that records the identity of each `flush` it receives, so the
// fan-out ORDER and exactly-once delivery are directly observable.
class RecordingSink final : public arbc::DamageSink {
public:
  RecordingSink(int id, std::vector<int>& log) : d_id(id), d_log(&log) {}
  void flush(const std::vector<Damage>& damage) override {
    d_log->push_back(d_id);
    d_last_size = damage.size();
  }
  std::size_t last_size() const { return d_last_size; }

private:
  int d_id;
  std::vector<int>* d_log;
  std::size_t d_last_size{0};
};

std::vector<Damage> one_batch() {
  return {Damage{ObjectId{}, arbc::Rect::infinite(), arbc::TimeRange::all()}};
}

} // namespace

// enforces: 01-core-concepts#multiple-viewports-observe-one-composition
TEST_CASE("damage_router: fans a batch out to every registrant once, in registration order") {
  arbc::Model model;
  DamageRouter router(model);
  CHECK(router.registered() == 0);

  const std::vector<Damage> batch = one_batch();

  // Zero registrants: a flush is a no-op (no deliveries).
  router.flush(batch);
  CHECK(router.deliveries() == 0);

  std::vector<int> log;
  RecordingSink a(1, log);
  RecordingSink b(2, log);
  RecordingSink c(3, log);
  DamageRouter::Registration ra = router.register_sink(a);
  DamageRouter::Registration rb = router.register_sink(b);
  DamageRouter::Registration rc = router.register_sink(c);
  CHECK(router.registered() == 3);

  // One flush reaches each registrant exactly once, in registration order, with
  // the same batch forwarded verbatim (Constraint 2/Decision 4).
  router.flush(batch);
  CHECK(log == std::vector<int>{1, 2, 3});
  CHECK(router.deliveries() == 3);
  CHECK(a.last_size() == batch.size());
  CHECK(c.last_size() == batch.size());
}

// enforces: 01-core-concepts#multiple-viewports-observe-one-composition
TEST_CASE("damage_router: unregister-on-destroy leaves the router and remaining registrants intact") {
  arbc::Model model;
  DamageRouter router(model);
  const std::vector<Damage> batch = one_batch();

  std::vector<int> log;
  RecordingSink a(1, log);
  RecordingSink b(2, log);
  RecordingSink c(3, log);
  DamageRouter::Registration ra = router.register_sink(a);
  DamageRouter::Registration rc = router.register_sink(c);
  {
    // A registration scoped tighter than the router: its destruction unregisters
    // exactly `b`, leaving `a` and `c` (and the router) intact. `b` registers
    // LAST, so it dispatches last (registration order, Decision 4).
    DamageRouter::Registration rb = router.register_sink(b);
    CHECK(router.registered() == 3);
    router.flush(batch);
    CHECK(log == std::vector<int>{1, 3, 2});
  }
  CHECK(router.registered() == 2);

  log.clear();
  router.flush(batch);
  CHECK(log == std::vector<int>{1, 3}); // b gone; a and c still in order
  CHECK(router.deliveries() == 5);      // 3 (first flush) + 2 (second)
}

// enforces: 01-core-concepts#multiple-viewports-observe-one-composition
TEST_CASE("damage_router: a moved-from Registration is inert and unregisters nothing twice") {
  arbc::Model model;
  DamageRouter router(model);
  const std::vector<Damage> batch = one_batch();

  std::vector<int> log;
  RecordingSink a(1, log);
  RecordingSink b(2, log);

  DamageRouter::Registration ra = router.register_sink(a);
  DamageRouter::Registration rb = router.register_sink(b);
  CHECK(router.registered() == 2);
  CHECK(ra.valid());

  // Move `ra`'s obligation into `moved`: the source becomes inert, the target owns
  // the single unregister obligation -- no registrant is added or lost.
  DamageRouter::Registration moved = std::move(ra);
  CHECK_FALSE(ra.valid());
  CHECK(moved.valid());
  CHECK(router.registered() == 2);

  {
    // Destroying the moved-from `ra` unregisters nothing (a double-unregister would
    // otherwise drop a live registrant).
    DamageRouter::Registration inert = std::move(ra);
    CHECK_FALSE(inert.valid());
  }
  CHECK(router.registered() == 2);

  router.flush(batch);
  CHECK(log == std::vector<int>{1, 2}); // both still registered
}
