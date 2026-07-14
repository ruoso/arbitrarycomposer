// serialize.compositions_table through the L5 sinks: a live `runtime::Document` holding
// TWO compositions round-trips in-document (doc 08 Principle 7). `capture_snapshot`'s walk
// spans every composition reachable from the root through a content's `composition_ref()`
// -- not just the lowest-id one -- so both compositions' layer-root contents land in the
// `ContentSnapshot` and are emitted off-thread.
//
// Two proofs, and neither needs the `org.arbc.nested` codec (that is runtime.nested_codec,
// M8, which depends on this task):
//
//  1. A KNOWN nesting kind: a test double interned through the `KindBridge` with its codec
//     injected into `serialize_snapshot`'s `CodecTable` (tests/ sits outside the
//     levelization graph -- format_tests D4).
//  2. An UNKNOWN nesting kind, through the full runtime `load_document` -> `save_document`
//     path with NO codec at all: the missing plugin's `PlaceholderContent` still carries
//     the child reference, so the child composition survives the save (Principle 2).

#include <arbc/base/expected.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/contract/registry.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/model/model.hpp>
#include <arbc/model/records.hpp>
#include <arbc/runtime/builtin_codecs.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/document_serialize.hpp>
#include <arbc/serialize/codec.hpp>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

using arbc::Affine;
using arbc::Codec;
using arbc::CodecTable;
using arbc::CompositionRecord;
using arbc::Content;
using arbc::ContentRef;
using arbc::Document;
using arbc::KindBridge;
using arbc::LoadContext;
using arbc::ObjectId;
using arbc::ReaderError;
using arbc::Registry;
using arbc::RenderRequest;
using arbc::Rgba;
using arbc::SerializeError;
using arbc::SolidContent;
using json = nlohmann::json;

namespace {

// The test double for a nesting kind: it holds a child composition's `ObjectId` and
// surfaces it on `composition_ref()`, exactly as `org.arbc.nested` does
// (nested_content.hpp). Nothing else about it matters here -- the machinery under test is
// kind-agnostic by construction (Decision 1).
class NestKind final : public Content {
public:
  explicit NestKind(ObjectId child) : d_child(child) {}

  std::optional<arbc::Rect> bounds() const override { return std::nullopt; }
  arbc::Stability stability() const override { return arbc::Stability::Static; }
  std::optional<arbc::TimeRange> time_extent() const override { return std::nullopt; }
  std::optional<arbc::RenderResult> render(const RenderRequest& request,
                                           std::shared_ptr<arbc::RenderCompletion>) override {
    return arbc::RenderResult{request.scale, true};
  }
  ObjectId composition_ref() const override { return d_child; }

  static constexpr const char* kind_id = "com.test.nest";

private:
  ObjectId d_child;
};

// The child reference is CORE-owned: the codec neither writes it (the writer appends the
// re-derived `"composition"` after `serialize` returns) nor reads it out of `params` (the
// reader hands it in as an already-allocated `ObjectId`). Constraint 1 / Decision 5.
Codec nest_codec() {
  Codec c;
  c.serialize = [](const Content& content,
                   arbc::SaveContext& /*ctx*/) -> arbc::expected<json, SerializeError> {
    if (dynamic_cast<const NestKind*>(&content) == nullptr) {
      return arbc::unexpected(SerializeError{SerializeError::Kind::CodecFailed, ObjectId{}});
    }
    return json::object();
  };
  c.deserialize = [](const json&, std::span<const ContentRef>, ObjectId composition,
                     LoadContext&) -> arbc::expected<std::unique_ptr<Content>, ReaderError> {
    return std::unique_ptr<Content>(std::make_unique<NestKind>(composition));
  };
  return c;
}

} // namespace

// enforces: 08-serialization#child-compositions-round-trip-in-document
TEST_CASE("a two-composition Document captures both compositions and emits them") {
  Document doc;
  KindBridge bridge;
  const ObjectId root = doc.add_composition(16.0, 16.0); // created FIRST -> the root
  const ObjectId child = doc.add_composition(8.0, 8.0);
  const ObjectId nest =
      doc.add_content(std::make_shared<NestKind>(child), bridge.intern(NestKind::kind_id, "1"));
  const ObjectId solid =
      doc.add_content(std::make_shared<SolidContent>(Rgba{1.0F, 0.5F, 0.25F, 1.0F}),
                      bridge.intern(SolidContent::kind_id, "1"));
  doc.attach_layer(root, doc.add_layer(nest, Affine::identity(), 1.0));
  doc.attach_layer(child, doc.add_layer(solid, Affine::identity(), 1.0));

  CodecTable codecs = arbc::builtin_codecs();
  codecs.add(NestKind::kind_id, nest_codec());

  const arbc::ContentSnapshot snap = arbc::capture_snapshot(doc, bridge);
  // BOTH compositions' layer roots are in the snapshot: the walk followed the nesting
  // content's `composition_ref()` into the child, instead of stopping at the lowest-id
  // composition as it used to.
  CHECK(snap.entries.size() == 2);
  CHECK(snap.by_id.find(nest) != snap.by_id.end());
  CHECK(snap.by_id.find(solid) != snap.by_id.end());

  const arbc::expected<std::string, SerializeError> out = arbc::serialize_snapshot(snap, codecs);
  REQUIRE(out);

  const char* const k_golden = R"json({
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
        "kind": "com.test.nest",
        "kind_version": "1",
        "opacity": 1.0,
        "params": {},
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
        }
      ]
    }
  }
}
)json";
  CHECK(*out == std::string(k_golden));
}

// enforces: 08-serialization#unknown-kind-preserves-composition-reference
// enforces: 08-serialization#unknown-kind-round-trips-verbatim
TEST_CASE("a missing plugin never orphans its child composition through the runtime path") {
  // No codec for `com.example.nest` exists anywhere in this build. It loads to a
  // `PlaceholderContent` that carries the RESOLVED child `ObjectId` on `composition_ref()`,
  // so `capture_snapshot`'s walk still reaches the child composition and the writer still
  // re-derives its ordinal. A missing plugin never destroys data (doc 08 Principle 2).
  const char* const k_golden = R"json({
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
        "kind": "com.example.nest",
        "kind_version": "3.2",
        "opacity": 1.0,
        "params": {
          "blend": "over"
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
  REQUIRE(arbc::load_document(k_golden, doc, bridge, registry));

  // The document really holds two compositions, and the root is the lowest-id one.
  const auto pin = doc.pin();
  ObjectId first;
  const CompositionRecord* rec = nullptr;
  REQUIRE(pin->find_first_composition(first, rec));
  ObjectId nest_content;
  pin->for_each_layer_in(first, [&](ObjectId lid) {
    const arbc::LayerRecord* lr = pin->find_layer(lid);
    REQUIRE(lr != nullptr);
    nest_content = lr->content;
  });
  const Content* const ghost = doc.resolve(nest_content);
  REQUIRE(ghost != nullptr);
  const ObjectId child = ghost->composition_ref(); // the placeholder kept the edge
  REQUIRE(child.valid());
  REQUIRE(pin->find_composition(child) != nullptr);
  CHECK(first.value < child.value);

  // Re-save through the full L5 path -- with no nested codec in existence -- byte-exact.
  const arbc::expected<std::string, SerializeError> saved = arbc::save_document(doc, bridge);
  REQUIRE(saved);
  CHECK(*saved == std::string(k_golden));
}
