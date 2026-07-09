// serialize.writer concurrency stress (tier-6, tsan lane): a serialize worker reads
// a held pin on one thread while the single writer thread commits N transactions in
// a loop. Pins serialize.writer Constraint 1 -- the writer reads a pinned DocRoot
// through its lock-free const peek accessors concurrently with the single mutator
// with no data race (doc 14:35-39). The assertion is behavioral: every concurrent
// serialization of the pin equals the pinned revision's bytes (never a wall clock).
// Catch2 macros are main-thread-only; the worker latches failures into atomics.

#include <arbc/base/ids.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/model/model.hpp>
#include <arbc/serialize/writer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <string>
#include <thread>

namespace {

arbc::ObjectId build_scene(arbc::Model& model, arbc::ObjectId& out_layer) {
  auto txn = model.transact("scene");
  const arbc::ObjectId comp = txn.add_composition(640.0, 480.0);
  const arbc::ObjectId content = txn.add_content(1);
  const arbc::ObjectId layer = txn.add_layer(content, arbc::Affine{1.5, 0.0, 0.0, 1.5, 4.0, 8.0}, 0.75);
  txn.attach_layer(comp, layer);
  REQUIRE(txn.commit());
  out_layer = layer;
  return comp;
}

// enforces: 08-serialization#writer-serializes-the-pinned-version
TEST_CASE("serialize_document races a committing writer with no data race on the pinned read") {
  arbc::Model model;
  arbc::ObjectId layer;
  build_scene(model, layer);

  const auto pin = model.current();
  const auto expected = arbc::serialize_document(*pin);
  REQUIRE(expected);
  const std::string expected_bytes = *expected;

  constexpr int k_iterations = 2000;
  std::atomic<bool> serialize_error{false};
  std::atomic<bool> mismatch{false};
  std::atomic<int> serialized{0};

  std::thread reader([&] {
    for (int i = 0; i < k_iterations; ++i) {
      const auto r = arbc::serialize_document(*pin);
      if (!r) {
        serialize_error.store(true);
        return;
      }
      if (*r != expected_bytes) {
        mismatch.store(true);
        return;
      }
      serialized.fetch_add(1);
    }
  });

  // Single writer thread (this one): commit new revisions the whole time.
  for (int i = 0; i < k_iterations; ++i) {
    auto txn = model.transact("edit");
    txn.set_opacity(layer, 0.1 + 0.001 * static_cast<double>(i % 100));
    txn.commit();
  }

  reader.join();

  CHECK_FALSE(serialize_error.load());
  CHECK_FALSE(mismatch.load());
  CHECK(serialized.load() == k_iterations);
  // The live model moved on; the pinned bytes never did.
  CHECK(model.current()->revision() > pin->revision());
}

} // namespace
