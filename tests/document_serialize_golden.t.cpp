#include <arbc/base/ids.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/contract/registry.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/kind_tone/tone_content.hpp>
#include <arbc/model/model.hpp>
#include <arbc/model/records.hpp> // CompositionRecord (find_first_composition)
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/document_serialize.hpp>
#include <arbc/serialize/codec.hpp> // CodecTable (to hold builtin_codecs() by value)

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

// runtime.document_serialize end-to-end byte-exact golden (docs 08, 17): the
// observable proof that a built-in-kind document is a file you can save and reopen.
// Inline raw-string golden, no on-disk fixture, byte-exact CHECK -- the serialize
// golden convention (serialize_writer_golden.t.cpp).

namespace {

using arbc::Affine;
using arbc::Document;
using arbc::KindBridge;
using arbc::ObjectId;
using arbc::Rgba;
using arbc::SolidContent;
using arbc::ToneContent;

// The canonical bytes for a document with an org.arbc.solid and an org.arbc.tone
// content on placed, attached layers (sorted keys, 2-space indent, trailing newline,
// shortest-round-trip numbers). The uint64 kind is never serialized -- only the
// reverse-DNS string is -- so the bytes rest solely on the deterministic string.
constexpr const char* k_golden = R"json({
  "arbc": {
    "format": 1
  },
  "composition": {
    "canvas": [
      0,
      0,
      1920,
      1080
    ],
    "layers": [
      {
        "kind": "org.arbc.solid",
        "kind_version": "1",
        "opacity": 1.0,
        "params": {
          "color": [
            1.0,
            0.5,
            0.25,
            1.0
          ]
        },
        "transform": [
          1.0,
          0.0,
          0.0,
          1.0,
          0.0,
          0.0
        ],
        "visible": true
      },
      {
        "kind": "org.arbc.tone",
        "kind_version": "1",
        "opacity": 1.0,
        "params": {
          "amplitude": 0.5,
          "frequency_hz": 440
        },
        "transform": [
          1.0,
          0.0,
          0.0,
          1.0,
          0.0,
          0.0
        ],
        "visible": true
      }
    ]
  }
}
)json";

// serialize.unknown_field_preservation: the same document annotated by a tool this build
// knows nothing about -- an envelope key, a root key, a composition key, a layer key, and
// an unknown interior of org.arbc.tone's `params`. Already canonical, so a byte-exact
// save->load->save is the proof that the `UnknownFieldStore` survives BOTH the
// `ContentSnapshot` copy and the off-thread provider closures (Decision 6).
constexpr const char* k_unknown_golden = R"json({
  "arbc": {
    "format": 1,
    "generator": "acme/2.1"
  },
  "composition": {
    "canvas": [
      0,
      0,
      1920,
      1080
    ],
    "layers": [
      {
        "kind": "org.arbc.tone",
        "kind_version": "1",
        "name": "hum",
        "opacity": 1.0,
        "params": {
          "amplitude": 0.5,
          "frequency_hz": 440,
          "vendor_detune": 0.01
        },
        "transform": [
          1.0,
          0.0,
          0.0,
          1.0,
          0.0,
          0.0
        ],
        "visible": true
      }
    ],
    "title": "scene one"
  },
  "vendor": {
    "tool": "acme"
  }
}
)json";

ObjectId build_scene(Document& doc, KindBridge& bridge) {
  const ObjectId comp = doc.add_composition(1920.0, 1080.0);
  const ObjectId solid =
      doc.add_content(std::make_shared<SolidContent>(Rgba{1.0F, 0.5F, 0.25F, 1.0F}),
                      bridge.intern(SolidContent::kind_id, "1"));
  const ObjectId tone = doc.add_content(std::make_shared<ToneContent>(440U, 0.5F),
                                        bridge.intern(ToneContent::kind_id, "1"));
  doc.attach_layer(comp, doc.add_layer(solid, Affine::identity(), 1.0));
  doc.attach_layer(comp, doc.add_layer(tone, Affine::identity(), 1.0));
  return comp;
}

} // namespace

// enforces: 08-serialization#document-content-round-trips-byte-exact
// enforces: 08-serialization#writer-serializes-the-pinned-version
// enforces: 14-data-model-and-editing#pinned-version-never-observes-later-edit
TEST_CASE("a built-in-kind document saves byte-exact and reload->resave is identical") {
  const arbc::CodecTable codecs = arbc::builtin_codecs();

  KindBridge bridge;
  Document doc;
  const ObjectId comp = build_scene(doc, bridge);

  // Capture the pinned content snapshot, then emit: byte-exact to the golden.
  const arbc::ContentSnapshot snap = arbc::capture_snapshot(doc, bridge);
  const arbc::expected<std::string, arbc::SerializeError> saved =
      arbc::serialize_snapshot(snap, codecs);
  REQUIRE(saved.has_value());
  CHECK(*saved == std::string(k_golden));

  // Mutate the document AFTER the pin, then re-emit the SAME captured snapshot: the
  // pinned save is unaffected by later edits (writer serializes the pinned version).
  const ObjectId extra =
      doc.add_content(std::make_shared<SolidContent>(Rgba{0.0F, 0.0F, 0.0F, 1.0F}),
                      bridge.intern(SolidContent::kind_id, "1"));
  doc.attach_layer(comp, doc.add_layer(extra, Affine::identity(), 1.0));
  const arbc::expected<std::string, arbc::SerializeError> pinned_again =
      arbc::serialize_snapshot(snap, codecs);
  REQUIRE(pinned_again.has_value());
  CHECK(*pinned_again == std::string(k_golden));

  // A fresh save of the current (mutated) version DOES observe the edit -- so the
  // stability above is the pin, not a frozen document.
  const arbc::expected<std::string, arbc::SerializeError> current =
      arbc::save_document(doc, bridge, codecs);
  REQUIRE(current.has_value());
  CHECK(*current != std::string(k_golden));

  // Reload the golden into a fresh document and re-save: byte-identical.
  KindBridge reload_bridge;
  Document reloaded;
  arbc::Registry registry;
  const arbc::expected<std::monostate, arbc::ReaderError> loaded =
      arbc::load_document(k_golden, reloaded, reload_bridge, registry);
  REQUIRE(loaded.has_value());
  const arbc::expected<std::string, arbc::SerializeError> resaved =
      arbc::save_document(reloaded, reload_bridge, codecs);
  REQUIRE(resaved.has_value());
  CHECK(*resaved == std::string(k_golden));
}

// enforces: 08-serialization#unknown-fields-preserved-at-every-tier
// enforces: 08-serialization#writer-serializes-the-pinned-version
TEST_CASE("a loaded document's unknown fields survive the snapshot copy and the L5 save") {
  const arbc::CodecTable codecs = arbc::builtin_codecs();

  KindBridge bridge;
  Document doc;
  arbc::Registry registry;
  REQUIRE(arbc::load_document(k_unknown_golden, doc, bridge, registry).has_value());

  // The whole path -- reader stash -> Document::d_unknown -> capture_snapshot's COPY ->
  // the off-thread provider closures -> the writer's never-shadow merge -- reproduces
  // every tier's unknowns byte-identical.
  const arbc::ContentSnapshot snap = arbc::capture_snapshot(doc, bridge);
  const arbc::expected<std::string, arbc::SerializeError> saved =
      arbc::serialize_snapshot(snap, codecs);
  REQUIRE(saved.has_value());
  CHECK(*saved == std::string(k_unknown_golden));

  // The snapshot holds a COPY of the store, not a window into live state (Decision 6):
  // an edit after the pin leaves the captured snapshot's bytes untouched, exactly as it
  // leaves its pinned revision untouched.
  ObjectId comp_id;
  const arbc::CompositionRecord* comp = nullptr;
  REQUIRE(doc.pin()->find_first_composition(comp_id, comp));
  const ObjectId extra =
      doc.add_content(std::make_shared<SolidContent>(Rgba{0.0F, 0.0F, 0.0F, 1.0F}),
                      bridge.intern(SolidContent::kind_id, "1"));
  doc.attach_layer(comp_id, doc.add_layer(extra, Affine::identity(), 1.0));
  const arbc::expected<std::string, arbc::SerializeError> pinned_again =
      arbc::serialize_snapshot(snap, codecs);
  REQUIRE(pinned_again.has_value());
  CHECK(*pinned_again == std::string(k_unknown_golden));

  // A fresh save DOES observe the new layer -- and still re-emits every preserved
  // unknown, since the freshly-added layer simply has no stash of its own.
  const arbc::expected<std::string, arbc::SerializeError> current =
      arbc::save_document(doc, bridge, codecs);
  REQUIRE(current.has_value());
  CHECK(*current != std::string(k_unknown_golden));
  CHECK(current->find("\"generator\": \"acme/2.1\"") != std::string::npos);
  CHECK(current->find("\"title\": \"scene one\"") != std::string::npos);
  CHECK(current->find("\"name\": \"hum\"") != std::string::npos);
  CHECK(current->find("\"vendor_detune\": 0.01") != std::string::npos);

  // Reload -> resave is a fixed point: the stash itself round-trips.
  KindBridge reload_bridge;
  Document reloaded;
  REQUIRE(arbc::load_document(k_unknown_golden, reloaded, reload_bridge, registry).has_value());
  const arbc::expected<std::string, arbc::SerializeError> resaved =
      arbc::save_document(reloaded, reload_bridge, codecs);
  REQUIRE(resaved.has_value());
  CHECK(*resaved == std::string(k_unknown_golden));
}
