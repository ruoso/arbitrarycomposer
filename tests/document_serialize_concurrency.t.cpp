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
#include <arbc/serialize/codec.hpp>        // CodecTable (to hold builtin_codecs() by value)
#include <arbc/serialize/load_context.hpp> // AssetSource (the external-child lane)

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

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

// enforces: 08-serialization#nesting-inputs-are-derived-not-persisted
// enforces: 08-serialization#external-composition-ref-round-trips
TEST_CASE("saving an EXTERNALLY-loaded nesting document races a live binding scope, benignly") {
  // Constraint 9's second half (runtime.nested_external_ref). The sibling lane above froze an
  // IN-DOCUMENT child; this one freezes an EXTERNAL one -- a child composition loaded from
  // another `.arbc` and installed into this document's model, whose `NestedContent` therefore
  // holds BOTH a live child `ObjectId` and a non-empty authored `ref`.
  //
  // That is the configuration where a save could most plausibly go wrong under a live binding:
  // the content's `composition_ref()` RESOLVES (so the writer's old rule would hoist the other
  // document's contents into this one's tables) and its `inputs()` is memo-derived and
  // non-empty exactly while the render thread holds a binding. The writer must consult only the
  // immutable `external_composition_ref()` -- set once at construction, never touched by
  // attach/detach -- and stop. No new lock, TSan clean, and every racing save lands on the
  // identical bytes: no `compositions` key, no `contents` key, the `ref` intact.
  using arbc::Content;
  using arbc::Document;
  using arbc::KindBridge;
  using arbc::NestedContent;
  using arbc::ObjectId;

  // The child project, served from memory: no temp files in a TSan lane.
  class MemorySource final : public arbc::AssetSource {
  public:
    void request(std::string_view, std::function<void(std::string_view)> on_ready) override {
      static constexpr const char* k_child =
          R"({"arbc":{"format":1},"composition":{"canvas":[0,0,8,8],"layers":[)"
          R"({"kind":"org.arbc.solid","kind_version":"1","params":{"color":[0,1,0,1]}}]}})";
      on_ready(k_child);
    }
  };
  static constexpr const char* k_parent =
      R"({"arbc":{"format":1},"composition":{"canvas":[0,0,16,16],"layers":[)"
      R"({"kind":"org.arbc.nested","kind_version":"1","params":{"ref":"child.arbc"}}]}})";

  MemorySource source;
  Document doc;
  KindBridge bridge;
  const arbc::Registry registry;
  REQUIRE(arbc::load_document(k_parent, doc, bridge, registry, "proj/parent.arbc", &source));

  // Reach the loaded nesting content, and confirm its child really did resolve -- otherwise
  // this lane would be racing the (uninteresting) unavailable path.
  const arbc::DocStatePtr pin = doc.pin();
  ObjectId root;
  const arbc::CompositionRecord* rec = nullptr;
  REQUIRE(pin->find_first_composition(root, rec));
  ObjectId nest_id;
  pin->for_each_layer_in(root, [&](ObjectId lid) {
    const arbc::LayerRecord* lr = pin->find_layer(lid);
    REQUIRE(lr != nullptr);
    nest_id = lr->content;
  });
  auto* const nested = dynamic_cast<NestedContent*>(doc.resolve(nest_id));
  REQUIRE(nested != nullptr);
  REQUIRE(nested->child().valid());
  REQUIRE(nested->ref() == "child.arbc");

  const arbc::expected<std::string, arbc::SerializeError> reference =
      arbc::save_document(doc, bridge);
  REQUIRE(reference.has_value());
  const std::string expected_bytes = *reference;
  REQUIRE(expected_bytes.find(R"("ref": "child.arbc")") != std::string::npos);
  REQUIRE(expected_bytes.find("\"compositions\"") == std::string::npos);

  arbc::CpuBackend backend;
  arbc::TileCache cache(64U * 1024 * 1024);
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

  std::thread render([&] {
    while (!stop.load()) {
      arbc::OperatorBindingScope scope = arbc::bind_operators(doc, service, backend, pin);
      static_cast<void>(nested->inputs()); // the derived edges the writer must NOT read
      static_cast<void>(nested->bounds()); // the memo the binding keys
      binds.fetch_add(1);
      scope.release();
    }
  });

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
  CHECK_FALSE(mismatch.load());
  CHECK(saved == k_rounds);
  CHECK(binds.load() >= 1);
  // The binding really did populate the derived edges the writer skipped -- the child's layers
  // ARE reachable through the model, so `inputs()` is non-empty -- otherwise the byte-equality
  // above would be vacuous and the suppression would be proving nothing.
  {
    const arbc::OperatorBindingScope scope = arbc::bind_operators(doc, service, backend, pin);
    CHECK(nested->attached());
    CHECK_FALSE(nested->inputs().empty());
  }
}

// enforces: 05-recursive-composition#deferred-external-child-installs-live
// enforces: 05-recursive-composition#deferred-external-chain-and-cycle-terminate
TEST_CASE("N deferring sources fire on_ready from N threads while the writer settles and saves") {
  // runtime.async_external_load introduces exactly ONE cross-thread channel -- the completion
  // queue -- so it owes exactly one TSan lane, and says so rather than inheriting
  // `damage_router`'s "no new concurrency obligation" by silence.
  //
  // The contract under test (Constraint 4): `on_ready` may run on ANY thread, and it touches no
  // `Model`, no `Document`, no `LoadContext` -- it copies the bytes into the mutex-guarded queue
  // and returns. Every parse, install, damage and commit happens on the WRITER thread, in
  // `settle_external_loads`, because `Model::Transaction::commit` and the damage seam are
  // writer-confined by design ("single writer, lock-free pinned reads"). Here N worker threads
  // deliver N children while the writer settles, renders metadata and saves in a loop.
  //
  // Assertions are OUTCOMES, never timings: every child lands exactly once, each URI is fetched
  // exactly once (no double-fetch under concurrent arrival), the save is byte-stable throughout,
  // and TSan is clean. Catch2 macros are main-thread-only, so the workers latch into atomics.
  using arbc::Content;
  using arbc::Document;
  using arbc::KindBridge;
  using arbc::NestedContent;
  using arbc::ObjectId;

  constexpr int k_children = 8;

  // A source that RECORDS its continuations and hands them to the test, which fires them from N
  // threads. The per-URI request tally is the dedup witness (doc 08 Principle 3).
  class ThreadedDeferringSource final : public arbc::AssetSource {
  public:
    void put(std::string uri, std::string bytes) {
      d_files.insert_or_assign(std::move(uri), std::move(bytes));
    }
    void request(std::string_view resolved_uri,
                 std::function<void(std::string_view)> on_ready) override {
      // Writer-thread only: `request` is issued from inside a load / a settle, both of which
      // run on the writer. It is `on_ready` that goes off-thread.
      ++d_requests[std::string(resolved_uri)];
      d_outstanding.emplace_back(std::string(resolved_uri), std::move(on_ready));
    }
    // Hand the outstanding continuations to the caller, which fires them concurrently.
    std::vector<std::pair<std::string, std::function<void(std::string_view)>>> take() {
      std::vector<std::pair<std::string, std::function<void(std::string_view)>>> out;
      out.swap(d_outstanding);
      return out;
    }
    std::string_view bytes_for(const std::string& uri) const {
      const auto it = d_files.find(uri);
      return it != d_files.end() ? std::string_view(it->second) : std::string_view{};
    }
    std::size_t requests_for(const std::string& uri) const {
      const auto it = d_requests.find(uri);
      return it == d_requests.end() ? 0 : it->second;
    }

  private:
    std::unordered_map<std::string, std::string> d_files;
    std::unordered_map<std::string, std::size_t> d_requests;
    std::vector<std::pair<std::string, std::function<void(std::string_view)>>> d_outstanding;
  };

  static constexpr const char* k_leaf =
      R"({"arbc":{"format":1},"composition":{"canvas":[0,0,8,8],"layers":[)"
      R"({"kind":"org.arbc.solid","kind_version":"1","params":{"color":[0,1,0,1]}}]}})";

  ThreadedDeferringSource source;
  std::string layers;
  for (int i = 0; i < k_children; ++i) {
    const std::string name = "c" + std::to_string(i) + ".arbc";
    source.put("proj/" + name, k_leaf);
    if (i > 0) {
      layers += ",";
    }
    layers += R"({"kind":"org.arbc.nested","kind_version":"1","params":{"ref":")" + name + R"("}})";
  }
  const std::string parent =
      R"({"arbc":{"format":1},"composition":{"canvas":[0,0,64,64],"layers":[)" + layers + "]}}";

  Document doc;
  KindBridge bridge;
  const arbc::Registry registry;
  REQUIRE(arbc::load_document(parent, doc, bridge, registry, "proj/parent.arbc", &source));

  // Every child is PENDING: nothing answered inside `request()`, so the parent loaded at
  // revision 0 with k_children valid-but-recordless child ids.
  REQUIRE(doc.pending_external_loads() == static_cast<std::size_t>(k_children));
  const arbc::expected<std::string, arbc::SerializeError> reference =
      arbc::save_document(doc, bridge);
  REQUIRE(reference.has_value());
  const std::string expected_bytes = *reference;

  // Fire the N callbacks from N threads, straight into the completion queue, while the writer
  // thread settles / saves in a loop. This is the race the queue exists to make safe.
  auto pending_calls = source.take();
  REQUIRE(pending_calls.size() == static_cast<std::size_t>(k_children));

  std::atomic<bool> serialize_error{false};
  std::atomic<bool> mismatch{false};
  std::atomic<int> fired{0};

  std::vector<std::thread> workers;
  workers.reserve(pending_calls.size());
  for (const auto& call : pending_calls) {
    workers.emplace_back([&source, &call, &fired] {
      call.second(source.bytes_for(call.first));
      fired.fetch_add(1);
    });
  }

  // The writer thread: settle to quiescence while the arrivals stream in, saving each round.
  std::size_t installed = 0;
  for (int round = 0; round < 2000 && installed < static_cast<std::size_t>(k_children); ++round) {
    installed += arbc::settle_external_loads(doc, bridge, registry);
    const arbc::expected<std::string, arbc::SerializeError> out = arbc::save_document(doc, bridge);
    if (!out.has_value()) {
      serialize_error.store(true);
    } else if (*out != expected_bytes) {
      mismatch.store(true); // the save must NOT depend on how many children have landed
    }
  }
  for (std::thread& t : workers) {
    t.join();
  }
  installed += arbc::settle_external_loads(doc, bridge, registry); // sweep any last arrival

  CHECK(fired.load() == k_children);
  CHECK_FALSE(serialize_error.load());
  CHECK_FALSE(mismatch.load());

  // Every child landed EXACTLY once, each fetched exactly once, and the model is complete.
  CHECK(installed == static_cast<std::size_t>(k_children));
  CHECK(doc.pending_external_loads() == 0);
  const arbc::DocStatePtr pin = doc.pin();
  ObjectId root;
  const arbc::CompositionRecord* rec = nullptr;
  REQUIRE(pin->find_first_composition(root, rec));
  int resolved = 0;
  pin->for_each_layer_in(root, [&](ObjectId lid) {
    const arbc::LayerRecord* lr = pin->find_layer(lid);
    REQUIRE(lr != nullptr);
    const auto* const nested = dynamic_cast<const NestedContent*>(doc.resolve(lr->content));
    REQUIRE(nested != nullptr);
    if (nested->child().valid() && pin->find_composition(nested->child()) != nullptr) {
      ++resolved;
    }
  });
  CHECK(resolved == k_children);
  for (int i = 0; i < k_children; ++i) {
    CHECK(source.requests_for("proj/c" + std::to_string(i) + ".arbc") == 1);
  }
  // And the bytes STILL do not depend on load state: the settled document saves identically to
  // the one whose children had not arrived (Constraint 9).
  const arbc::expected<std::string, arbc::SerializeError> settled =
      arbc::save_document(doc, bridge);
  REQUIRE(settled.has_value());
  CHECK(*settled == expected_bytes);
}

// enforces: 05-recursive-composition#deferred-external-chain-and-cycle-terminate
TEST_CASE("an external arrival racing document TEARDOWN installs nothing and faults nothing") {
  // Constraint 6, under ASan + TSan. A network fetch outlives the document that started it, and
  // the completion queue is the exact object it would write through -- so the queue is owned by
  // `shared_ptr` and every `on_ready` captures a `weak_ptr`. A worker firing while the main
  // thread destroys the `Document` therefore either lands in a live queue (which nobody will
  // ever settle) or finds an expired one and drops its bytes. Never a use-after-free.
  using arbc::Document;
  using arbc::KindBridge;

  class HeldSource final : public arbc::AssetSource {
  public:
    void request(std::string_view, std::function<void(std::string_view)> on_ready) override {
      d_outstanding.push_back(std::move(on_ready));
    }
    std::vector<std::function<void(std::string_view)>> take() {
      std::vector<std::function<void(std::string_view)>> out;
      out.swap(d_outstanding);
      return out;
    }

  private:
    std::vector<std::function<void(std::string_view)>> d_outstanding;
  };

  static constexpr const char* k_child =
      R"({"arbc":{"format":1},"composition":{"canvas":[0,0,8,8],"layers":[)"
      R"({"kind":"org.arbc.solid","kind_version":"1","params":{"color":[0,1,0,1]}}]}})";
  static constexpr const char* k_parent =
      R"({"arbc":{"format":1},"composition":{"canvas":[0,0,16,16],"layers":[)"
      R"({"kind":"org.arbc.nested","kind_version":"1","params":{"ref":"child.arbc"}}]}})";

  HeldSource source;
  std::vector<std::function<void(std::string_view)>> callbacks;
  std::atomic<bool> fired{false};

  auto doc = std::make_unique<Document>();
  KindBridge bridge;
  const arbc::Registry registry;
  REQUIRE(arbc::load_document(k_parent, *doc, bridge, registry, "proj/parent.arbc", &source));
  REQUIRE(doc->pending_external_loads() == 1);
  callbacks = source.take();
  REQUIRE(callbacks.size() == 1);

  // The worker delivers the bytes while the main thread runs the `Document` destructor -- a
  // genuine race, not a sequence. The callback's `weak_ptr` is the arbiter, and both orderings
  // are safe by construction: lock FIRST and the queue's strong count is 2, so it outlives the
  // document's reset; lock SECOND and the count is already 0, so the lock fails and the bytes
  // are dropped. Neither branch writes through a dangling pointer, which is precisely what an
  // `AssetSource` that captured a raw `Document*` could not say.
  std::thread worker([&callbacks, &fired] {
    callbacks[0](k_child);
    fired.store(true);
  });
  doc.reset(); // <- concurrent with the delivery above
  worker.join();
  CHECK(fired.load());

  // And a callback firing long AFTER the document is gone is equally benign.
  REQUIRE_NOTHROW(callbacks[0](k_child));
}
