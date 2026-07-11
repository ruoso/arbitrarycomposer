// runtime.nested_codec unit tests: the two halves of `nested_codec()` driven directly,
// plus the document round-trip that proves nested's empty-`params` choice buys free
// unknown-key preservation. The byte-exact goldens, the attach-invariance proof and the
// TSan lane live in the cross-component `tests/` tree (they need a live Backend / a real
// binding scope); what belongs here is the codec itself.
//
// This TU names `Codec` -- and through it nlohmann::json, PRIVATE to arbc_runtime -- so
// the component-test binary links nlohmann explicitly (src/runtime/CMakeLists.txt).

#include <arbc/base/expected.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/contract/registry.hpp>
#include <arbc/kind_nested/nested_content.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/model/records.hpp>
#include <arbc/runtime/builtin_codecs.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/document_serialize.hpp>
#include <arbc/serialize/codec.hpp>
#include <arbc/serialize/load_context.hpp>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <memory>
#include <span>
#include <string>

namespace {

using arbc::Affine;
using arbc::Codec;
using arbc::Content;
using arbc::ContentRef;
using arbc::Document;
using arbc::KindBridge;
using arbc::LoadContext;
using arbc::NestedContent;
using arbc::ObjectId;
using arbc::ReaderError;
using arbc::Registry;
using arbc::Rgba;
using arbc::SerializeError;
using arbc::SolidContent;
using json = nlohmann::json;

} // namespace

// enforces: 08-serialization#builtin-nested-codec-round-trips
TEST_CASE("serialize_nested emits an empty params object and never the child id") {
  const Codec codec = arbc::nested_codec();
  REQUIRE(codec.serialize);

  // The child reference is CORE-owned (doc 08 Principle 7): the writer re-derives the
  // `"composition"` id from graph structure AFTER the codec returns, so the codec neither
  // writes it nor could fight it. Nested has no other state, so `params` is empty --
  // present as an object, not omitted, matching what `content_body_to_json` frames for
  // every kind.
  const NestedContent nested(ObjectId{77});
  const arbc::expected<json, SerializeError> params = codec.serialize(nested);
  REQUIRE(params);
  CHECK(params->is_object());
  CHECK(params->empty());
  CHECK(params->dump() == "{}");
}

// enforces: 08-serialization#builtin-nested-codec-round-trips
TEST_CASE("serialize_nested on a non-nested content is a CodecFailed value") {
  const Codec codec = arbc::nested_codec();
  const SolidContent solid(Rgba{1.0F, 0.0F, 0.0F, 1.0F});

  arbc::expected<json, SerializeError> out = json::object();
  REQUIRE_NOTHROW(out = codec.serialize(solid)); // a routing mismatch is a VALUE, never a throw
  REQUIRE_FALSE(out);
  CHECK(out.error().kind == SerializeError::Kind::CodecFailed);
}

// enforces: 08-serialization#builtin-nested-codec-round-trips
TEST_CASE("deserialize_nested builds a NestedContent around the pre-resolved child id") {
  const Codec codec = arbc::nested_codec();
  REQUIRE(codec.deserialize);
  LoadContext ctx{std::string{}};

  // The reader's `CompResolver` allocated the child's ObjectId before the codec ran (which
  // is why a Droste back-edge costs nothing); the codec just builds around it. It interns
  // nothing and resolves nothing.
  const arbc::expected<std::unique_ptr<Content>, ReaderError> built =
      codec.deserialize(json::object(), std::span<const ContentRef>{}, ObjectId{42}, ctx);
  REQUIRE(built);
  const auto* const nested = dynamic_cast<const NestedContent*>(built->get());
  REQUIRE(nested != nullptr);
  CHECK(nested->child() == ObjectId{42});
}

// enforces: 08-serialization#builtin-nested-codec-round-trips
TEST_CASE("deserialize_nested with no child is a MissingRequiredField value at /composition") {
  const Codec codec = arbc::nested_codec();
  LoadContext ctx{std::string{}};

  // A nested content with no child is a nested content with nothing to nest: tolerating it
  // would round-trip as the empty placeholder forever, a silent data-loss shape (Decision
  // 3). An id naming a composition ABSENT from the table is a different error the reader
  // already raises ahead of the codec (UnresolvableReference), never duplicated here.
  arbc::expected<std::unique_ptr<Content>, ReaderError> out{nullptr};
  REQUIRE_NOTHROW(
      out = codec.deserialize(json::object(), std::span<const ContentRef>{}, ObjectId{}, ctx));
  REQUIRE_FALSE(out);
  CHECK(out.error().kind == ReaderError::Kind::MissingRequiredField);
  CHECK(out.error().path == "/composition");
}

// enforces: 08-serialization#known-kind-params-unknowns-preserved
// enforces: 08-serialization#builtin-nested-codec-round-trips
TEST_CASE("a params key nested's codec never consumed round-trips verbatim, `ref` included") {
  // Nested consumes NO params at all, so the core's load-time residual diff
  // (`params_in - codec.serialize(built)`) preserves the whole object for free -- and that
  // is what keeps a hand-authored EXTERNAL child reference (`params.ref`, loader territory
  // until `runtime.nested_external_ref`) readable-and-rewritable today rather than silently
  // dropped. The content is a REAL NestedContent, not a placeholder: the codec ran.
  const char* const k_doc = R"json({
  "arbc": {
    "format": 1
  },
  "composition": {
    "canvas": [
      0,
      0,
      16,
      16
    ],
    "layers": [
      {
        "composition": "1",
        "kind": "org.arbc.nested",
        "kind_version": "1",
        "opacity": 1.0,
        "params": {
          "ref": "file://child.arbc",
          "vendor_tag": "x"
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
  },
  "compositions": {
    "1": {
      "canvas": [
        0,
        0,
        8,
        8
      ],
      "layers": [
        {
          "kind": "org.arbc.solid",
          "kind_version": "1",
          "opacity": 1.0,
          "params": {
            "color": [
              0.0,
              1.0,
              0.0,
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
        }
      ]
    }
  }
}
)json";

  Document doc;
  KindBridge bridge;
  const Registry registry;
  REQUIRE(arbc::load_document(k_doc, doc, bridge, registry));

  // The nesting layer really is a live NestedContent -- the codec is registered, so this is
  // no longer the unknown-kind placeholder path.
  const auto pin = doc.pin();
  ObjectId root;
  const arbc::CompositionRecord* rec = nullptr;
  REQUIRE(pin->find_first_composition(root, rec));
  ObjectId nest_id;
  pin->for_each_layer_in(root, [&](ObjectId lid) {
    const arbc::LayerRecord* lr = pin->find_layer(lid);
    REQUIRE(lr != nullptr);
    nest_id = lr->content;
  });
  const auto* const nested = dynamic_cast<const NestedContent*>(doc.resolve(nest_id));
  REQUIRE(nested != nullptr);
  CHECK(pin->find_composition(nested->child()) != nullptr);

  const arbc::expected<std::string, SerializeError> saved = arbc::save_document(doc, bridge);
  REQUIRE(saved);
  CHECK(*saved == std::string(k_doc)); // byte-exact, unknown params and all
}

// enforces: 08-serialization#nesting-inputs-are-derived-not-persisted
TEST_CASE("an UNATTACHED nested content emits no inputs array and hoists nothing") {
  // The baseline half of the attach-invariance proof (whose bound half lives in
  // tests/nested_codec_golden.t.cpp, where a Backend is available): even before any
  // binding, the writer must reach the child's contents through the `compositions` table
  // and never through nested's `inputs()`.
  Document doc;
  KindBridge bridge;
  const ObjectId root = doc.add_composition(16.0, 16.0); // parent FIRST -> the root
  const ObjectId child = doc.add_composition(8.0, 8.0);
  const ObjectId solid =
      doc.add_content(std::make_shared<SolidContent>(Rgba{0.0F, 1.0F, 0.0F, 1.0F}),
                      bridge.intern(SolidContent::kind_id, arbc::k_solid_kind_version));
  const ObjectId nest =
      doc.add_content(std::make_shared<NestedContent>(child),
                      bridge.intern(NestedContent::kind_id, arbc::k_nested_kind_version));
  doc.attach_layer(root, doc.add_layer(nest, Affine::identity(), 1.0));
  doc.attach_layer(child, doc.add_layer(solid, Affine::identity(), 1.0));

  const arbc::expected<std::string, SerializeError> saved = arbc::save_document(doc, bridge);
  REQUIRE(saved);
  CHECK(saved->find("\"inputs\"") == std::string::npos);
  CHECK(saved->find("\"contents\"") == std::string::npos);
  CHECK(saved->find("\"composition\": \"1\"") != std::string::npos);
}
