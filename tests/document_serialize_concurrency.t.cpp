// Tier-6 TSan lane (runtime.document_serialize Decision 6): a save captured on the
// writer thread emits off-thread against ongoing edits without a torn read. The
// single writer thread (main) churns add_content/attach mutations while a background
// thread re-emits a pinned ContentSnapshot; the snapshot is fully immutable (a pinned
// DocRoot + captured raw Content* + owned kind strings), so the emit shares no mutable
// state with the writer. Assertions are OUTCOMES only -- the off-thread bytes equal a
// synchronous save of the same pinned revision, and TSan is clean -- never a
// wall-clock assertion (doc 16). Catch2 macros are main-thread-only, so the worker
// latches its verdicts into atomics.

#include <arbc/base/ids.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/contract/registry.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/kind_tone/tone_content.hpp>
#include <arbc/model/records.hpp> // CompositionRecord (find_first_composition)
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/document_serialize.hpp>
#include <arbc/serialize/codec.hpp> // CodecTable (to hold builtin_codecs() by value)

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <thread>

TEST_CASE("a captured snapshot serializes off-thread while the writer thread mutates") {
  using arbc::Affine;
  using arbc::Document;
  using arbc::KindBridge;
  using arbc::ObjectId;
  using arbc::Rgba;
  using arbc::SolidContent;
  using arbc::ToneContent;

  const arbc::CodecTable codecs = arbc::builtin_codecs();

  KindBridge bridge;
  Document doc;
  const ObjectId comp = doc.add_composition(1920.0, 1080.0);
  const ObjectId solid =
      doc.add_content(std::make_shared<SolidContent>(Rgba{1.0F, 0.5F, 0.25F, 1.0F}),
                      bridge.intern(SolidContent::kind_id, "1"));
  const ObjectId tone = doc.add_content(std::make_shared<ToneContent>(440U, 0.5F),
                                        bridge.intern(ToneContent::kind_id, "1"));
  doc.attach_layer(comp, doc.add_layer(solid, Affine::identity(), 1.0));
  doc.attach_layer(comp, doc.add_layer(tone, Affine::identity(), 1.0));

  // Capture on the writer thread (this thread), then compute the reference bytes once.
  const arbc::ContentSnapshot snap = arbc::capture_snapshot(doc, bridge);
  const arbc::expected<std::string, arbc::SerializeError> reference =
      arbc::serialize_snapshot(snap, codecs);
  REQUIRE(reference.has_value());
  const std::string expected_bytes = *reference;

  constexpr int k_iterations = 2000;
  std::atomic<bool> serialize_error{false};
  std::atomic<bool> mismatch{false};
  std::atomic<int> serialized{0};

  std::thread reader([&] {
    for (int i = 0; i < k_iterations; ++i) {
      const arbc::expected<std::string, arbc::SerializeError> out =
          arbc::serialize_snapshot(snap, codecs);
      if (!out.has_value()) {
        serialize_error.store(true);
      } else if (*out != expected_bytes) {
        mismatch.store(true);
      } else {
        serialized.fetch_add(1);
      }
    }
  });

  // The main thread is the single writer: append fresh content + layer each round.
  for (int i = 0; i < k_iterations; ++i) {
    const ObjectId c = doc.add_content(std::make_shared<SolidContent>(Rgba{0.0F, 0.0F, 0.0F, 1.0F}),
                                       bridge.intern(SolidContent::kind_id, "1"));
    doc.attach_layer(comp, doc.add_layer(c, Affine::identity(), 1.0));
  }

  reader.join();
  CHECK_FALSE(serialize_error.load());
  CHECK_FALSE(mismatch.load());
  CHECK(serialized.load() == k_iterations);
  CHECK(doc.pin()->revision() > snap.state->revision());
}

// enforces: 08-serialization#writer-serializes-the-pinned-version
// enforces: 08-serialization#unknown-fields-preserved-at-every-tier
TEST_CASE("a snapshot's COPIED unknown-field stash emits off-thread against ongoing edits") {
  // serialize.unknown_field_preservation Constraint 9 / Decision 6: the store is
  // writer-thread-owned like the content side-map, so `capture_snapshot` COPIES it into
  // the snapshot rather than letting the off-thread emit read live editor state. Handing
  // the live document's store to the serializer instead would be a TSan finding here AND
  // would break the pinned-version guarantee -- so this lane pins both at once: the
  // off-thread bytes carry the PINNED revision's unknowns while the writer thread churns.
  using arbc::Affine;
  using arbc::Document;
  using arbc::KindBridge;
  using arbc::ObjectId;
  using arbc::Rgba;
  using arbc::SolidContent;

  constexpr const char* k_annotated = R"json({
  "arbc": { "format": 1, "generator": "acme/2.1" },
  "composition": {
    "canvas": [0, 0, 1920, 1080],
    "title": "scene one",
    "layers": [
      {
        "kind": "org.arbc.solid",
        "kind_version": "1",
        "name": "backdrop",
        "opacity": 1.0,
        "params": { "color": [1.0, 0.5, 0.25, 1.0], "vendor_tag": "x" },
        "transform": [1.0, 0.0, 0.0, 1.0, 0.0, 0.0],
        "visible": true
      }
    ]
  },
  "vendor": { "tool": "acme" }
})json";

  const arbc::CodecTable codecs = arbc::builtin_codecs();

  KindBridge bridge;
  Document doc;
  arbc::Registry registry;
  REQUIRE(arbc::load_document(k_annotated, doc, bridge, registry).has_value());

  ObjectId comp_id;
  const arbc::CompositionRecord* comp = nullptr;
  REQUIRE(doc.pin()->find_first_composition(comp_id, comp));

  const arbc::ContentSnapshot snap = arbc::capture_snapshot(doc, bridge);
  const arbc::expected<std::string, arbc::SerializeError> reference =
      arbc::serialize_snapshot(snap, codecs);
  REQUIRE(reference.has_value());
  const std::string expected_bytes = *reference;
  // The reference genuinely carries every tier's unknowns -- otherwise the byte-equality
  // below would be vacuous.
  REQUIRE(expected_bytes.find("\"generator\": \"acme/2.1\"") != std::string::npos);
  REQUIRE(expected_bytes.find("\"title\": \"scene one\"") != std::string::npos);
  REQUIRE(expected_bytes.find("\"name\": \"backdrop\"") != std::string::npos);
  REQUIRE(expected_bytes.find("\"vendor_tag\": \"x\"") != std::string::npos);
  REQUIRE(expected_bytes.find("\"tool\": \"acme\"") != std::string::npos);

  constexpr int k_rounds = 2000;
  std::atomic<bool> serialize_error{false};
  std::atomic<bool> mismatch{false};
  std::atomic<int> serialized{0};

  std::thread emitter([&] {
    for (int i = 0; i < k_rounds; ++i) {
      const arbc::expected<std::string, arbc::SerializeError> out =
          arbc::serialize_snapshot(snap, codecs);
      if (!out.has_value()) {
        serialize_error.store(true);
      } else if (*out != expected_bytes) {
        mismatch.store(true);
      } else {
        serialized.fetch_add(1);
      }
    }
  });

  // The single writer thread churns the LIVE document -- and with it the live store the
  // snapshot's copy was taken from.
  for (int i = 0; i < k_rounds; ++i) {
    const ObjectId c = doc.add_content(std::make_shared<SolidContent>(Rgba{0.0F, 0.0F, 0.0F, 1.0F}),
                                       bridge.intern(SolidContent::kind_id, "1"));
    doc.attach_layer(comp_id, doc.add_layer(c, Affine::identity(), 1.0));
  }

  emitter.join();
  CHECK_FALSE(serialize_error.load());
  CHECK_FALSE(mismatch.load());
  CHECK(serialized.load() == k_rounds);
  CHECK(doc.pin()->revision() > snap.state->revision());
}
