// Tier-6 TSan lane (runtime.document_serialize Decision 6): a save captured on the
// writer thread emits off-thread against ongoing edits without a torn read. The
// single writer thread (main) churns add_content/attach mutations while a background
// thread re-emits a pinned ContentSnapshot; the snapshot is fully immutable (a pinned
// DocRoot + captured raw Content* + owned kind strings), so the emit shares no mutable
// state with the writer. Assertions are OUTCOMES only -- the off-thread bytes equal a
// synchronous save of the same pinned revision, and TSan is clean -- never a
// wall-clock assertion (doc 16). Catch2 macros are main-thread-only, so the worker
// latches its verdicts into atomics.

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/compositor/pull_service.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/contract/registry.hpp>
#include <arbc/kind_nested/nested_content.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/kind_tone/tone_content.hpp>
#include <arbc/model/records.hpp> // CompositionRecord (find_first_composition)
#include <arbc/runtime/builtin_codecs.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/document_serialize.hpp>
#include <arbc/runtime/operator_binding.hpp>
#include <arbc/runtime/pull_identity.hpp>
#include <arbc/serialize/codec.hpp> // CodecTable (to hold builtin_codecs() by value)

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
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

// enforces: 08-serialization#writer-serializes-the-pinned-version
// enforces: 08-serialization#child-compositions-round-trip-in-document
TEST_CASE("a MULTI-COMPOSITION snapshot emits off-thread against ongoing edits") {
  // serialize.compositions_table Constraint 11: `capture_snapshot`'s new composition walk
  // runs on the WRITER thread -- it calls `doc.resolve`, the writer-thread-owned side map
  // -- and the off-thread `serialize_snapshot` continues to read only immutable snapshot
  // data plus the pinned `DocRoot`. So a document that is a GRAPH of compositions emits
  // off-thread with no data race, and the bytes match the pinned revision even while the
  // writer keeps committing into BOTH compositions.
  using arbc::Affine;
  using arbc::Content;
  using arbc::ContentRef;
  using arbc::Document;
  using arbc::KindBridge;
  using arbc::ObjectId;
  using arbc::Rgba;
  using arbc::SolidContent;

  // The nesting content is an UNKNOWN kind here (no codec anywhere in this build): its
  // PlaceholderContent carries the child reference, which is exactly what makes the
  // composition walk kind-agnostic (Decision 1/6). Built through the reader, so the
  // placeholder is a real one.
  const char* const k_two_comps = R"json({
  "arbc": { "format": 1 },
  "composition": {
    "canvas": [0, 0, 1920, 1080],
    "layers": [
      { "kind": "com.example.nest", "kind_version": "1.0", "params": {}, "composition": "1" }
    ]
  },
  "compositions": {
    "1": { "canvas": [0, 0, 640, 480], "layers": [
      { "kind": "org.arbc.solid", "kind_version": "1",
        "params": { "color": [1.0, 0.5, 0.25, 1.0] } }
    ]}
  }
})json";

  const arbc::CodecTable codecs = arbc::builtin_codecs();
  KindBridge bridge;
  Document doc;
  const arbc::Registry registry;
  REQUIRE(arbc::load_document(k_two_comps, doc, bridge, registry).has_value());

  // Both compositions are live; the root is the lowest-id one.
  const auto pinned = doc.pin();
  ObjectId root;
  const arbc::CompositionRecord* rec = nullptr;
  REQUIRE(pinned->find_first_composition(root, rec));
  ObjectId nest_content;
  pinned->for_each_layer_in(root, [&](ObjectId lid) {
    const arbc::LayerRecord* lr = pinned->find_layer(lid);
    REQUIRE(lr != nullptr);
    nest_content = lr->content;
  });
  const Content* const ghost = doc.resolve(nest_content);
  REQUIRE(ghost != nullptr);
  const ObjectId child = ghost->composition_ref();
  REQUIRE(child.valid());

  // Capture on the writer thread (this thread), then compute the reference bytes once.
  const arbc::ContentSnapshot snap = arbc::capture_snapshot(doc, bridge);
  const arbc::expected<std::string, arbc::SerializeError> reference =
      arbc::serialize_snapshot(snap, codecs);
  REQUIRE(reference.has_value());
  const std::string expected_bytes = *reference;
  CHECK(expected_bytes.find("\"compositions\"") != std::string::npos);
  CHECK(expected_bytes.find("\"composition\": \"1\"") != std::string::npos);

  constexpr int k_iterations = 500;
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

  // The single writer churns BOTH compositions while the pinned emit runs.
  for (int i = 0; i < k_iterations; ++i) {
    const ObjectId c = doc.add_content(std::make_shared<SolidContent>(Rgba{0.0F, 0.0F, 0.0F, 1.0F}),
                                       bridge.intern(SolidContent::kind_id, "1"));
    doc.attach_layer((i % 2 == 0) ? root : child, doc.add_layer(c, Affine::identity(), 1.0));
  }

  reader.join();
  CHECK_FALSE(serialize_error.load());
  CHECK_FALSE(mismatch.load()); // the pinned revision's bytes, unmoved by the edits
  CHECK(serialized.load() == k_iterations);
  CHECK(doc.pin()->revision() > snap.state->revision());
}

// enforces: 08-serialization#nesting-inputs-are-derived-not-persisted
TEST_CASE("saving a nesting document races a live binding scope on another thread, benignly") {
  // runtime.nested_codec Constraint 6 / Decision 1. Saves and frames genuinely overlap:
  // `offline_sequence.cpp` holds an `OperatorBindingScope` for the length of an export, and an
  // interactive session holds one for every frame -- so an autosave taken mid-render is the
  // normal case, not a corner. "Don't save during a frame" is not an invariant anyone can hold.
  //
  // The render thread's `attach`/`detach` WRITES `NestedContent`'s borrowed services and its
  // derived-inputs memo; a metadata query READS them. The writer thread meanwhile runs the
  // whole save -- `capture_snapshot`'s reverse-map walk AND the emit. Before Decision 1 both
  // walks called `inputs()` on the nesting content, so the SAME scene serialized to different
  // bytes depending on whether a binding happened to be attached: the child's contents got
  // counted twice and hoisted into `contents` behind a `$ref` they never earned.
  //
  // After it, the writer touches only the immutable child `ObjectId` on a nesting content --
  // never the memo -- so the overlap is benign with NO new lock. That is what this lane
  // proves: TSan clean, and every one of the racing saves lands on the identical bytes.
  using arbc::Affine;
  using arbc::Content;
  using arbc::Document;
  using arbc::KindBridge;
  using arbc::NestedContent;
  using arbc::ObjectId;
  using arbc::Rgba;
  using arbc::SolidContent;

  KindBridge bridge;
  Document doc;
  const ObjectId root = doc.add_composition(16.0, 16.0); // the parent, created FIRST
  const ObjectId child = doc.add_composition(8.0, 8.0);
  auto nested = std::make_shared<NestedContent>(child);
  doc.attach_layer(
      root, doc.add_layer(doc.add_content(nested, bridge.intern(NestedContent::kind_id,
                                                                arbc::k_nested_kind_version)),
                          Affine::identity(), 1.0));
  doc.attach_layer(
      child, doc.add_layer(
                 doc.add_content(std::make_shared<SolidContent>(Rgba{0.0F, 1.0F, 0.0F, 1.0F}),
                                 bridge.intern(SolidContent::kind_id, arbc::k_solid_kind_version)),
                 Affine::identity(), 1.0));

  // The reference: the save of the UNBOUND document, taken before any binding exists.
  const arbc::expected<std::string, arbc::SerializeError> reference =
      arbc::save_document(doc, bridge);
  REQUIRE(reference.has_value());
  const std::string expected_bytes = *reference;
  REQUIRE(expected_bytes.find("\"composition\": \"1\"") != std::string::npos);

  // The document is FROZEN for the length of the lane -- no `add_content`/`attach` churn. The
  // only mutable state either thread touches is the nesting content's own binding + memo, and
  // isolating it is the point: a document mutation would drag the (already-proven) side-map
  // discipline back in and blur what this lane isolates.
  arbc::CpuBackend backend;
  arbc::TileCache cache(64U * 1024 * 1024);
  const arbc::DocStatePtr pin = doc.pin();
  const arbc::ContentResolver resolve = [&doc](ObjectId id) { return doc.resolve(id); };
  arbc::PullConfig config;
  config.id_of = arbc::make_pull_identity_of(*pin, resolve);
  const std::uint64_t revision = pin->revision();
  config.contribution = [revision](const Content*) { return revision; };
  arbc::PullServiceImpl service(cache, backend, arbc::direct_dispatch(), config);
  arbc::register_builtin_operator_binders();

  constexpr int k_rounds = 500;
  std::atomic<bool> stop{false};
  std::atomic<int> binds{0};

  // The RENDER thread: bind, drive the memo the binding populates, release -- over and over,
  // exactly as a driver does per frame.
  std::thread render([&] {
    while (!stop.load()) {
      arbc::OperatorBindingScope scope = arbc::bind_operators(doc, service, backend, pin);
      static_cast<void>(nested->inputs()); // the derived edges the writer must NOT read
      static_cast<void>(nested->bounds()); // the memo the binding keys
      binds.fetch_add(1);
      scope.release();
    }
  });

  // The WRITER thread (this one): a full save per round -- `capture_snapshot` and the emit --
  // taken at an arbitrary point in the render thread's bind/release cycle.
  std::atomic<bool> serialize_error{false};
  std::atomic<bool> mismatch{false};
  int saved = 0;
  for (int i = 0; i < k_rounds; ++i) {
    const arbc::expected<std::string, arbc::SerializeError> out = arbc::save_document(doc, bridge);
    if (!out.has_value()) {
      serialize_error.store(true);
    } else if (*out != expected_bytes) {
      mismatch.store(true);
    } else {
      ++saved;
    }
  }
  stop.store(true);
  render.join();

  CHECK_FALSE(serialize_error.load());
  // Every save landed on the unbound bytes: the output is a pure function of the document,
  // not of whether a render binding was live when it was taken (Constraint 6).
  CHECK_FALSE(mismatch.load());
  CHECK(saved == k_rounds);
  CHECK(binds.load() >= 1);
  // The binding really did populate the derived edges the writer skipped -- otherwise the
  // byte-equality above would be vacuous.
  {
    const arbc::OperatorBindingScope scope = arbc::bind_operators(doc, service, backend, pin);
    CHECK(nested->attached());
    CHECK_FALSE(nested->inputs().empty());
  }
}
