#include <arbc/base/ids.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/contract/registry.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/kind_tone/tone_content.hpp>
#include <arbc/model/model.hpp>
#include <arbc/model/records.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/document_serialize.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <string>

// runtime.document_serialize behavioral tests (docs 08, 17). These drive the
// `Document` load<->save facade for the built-in leaf kinds without naming the JSON
// library (nlohmann is PRIVATE to the runtime codec TUs); the byte-exact golden and
// the TSan lane live in the cross-component `tests/` tree.

namespace {

using arbc::Content;
using arbc::Document;
using arbc::KindBridge;
using arbc::ObjectId;
using arbc::Registry;
using arbc::Rgba;
using arbc::SolidContent;
using arbc::ToneContent;

// The interned kind version is inert here: the built-in kinds are pre-interned at
// bridge construction, so `intern` returns the existing token regardless (the arg
// documents intent). Passing the pinned "1" keeps the call self-describing.
constexpr const char* k_version = "1";

// Build a document with one solid and one tone content on placed, attached layers,
// returning the two content ids so a test can assert their round-trip.
struct BuiltDoc {
  ObjectId comp;
  ObjectId solid;
  ObjectId tone;
};

BuiltDoc build_scene(Document& doc, KindBridge& bridge) {
  const ObjectId comp = doc.add_composition(1920.0, 1080.0);
  const ObjectId solid =
      doc.add_content(std::make_shared<SolidContent>(Rgba{1.0F, 0.5F, 0.25F, 1.0F}),
                      bridge.intern(SolidContent::kind_id, k_version));
  const ObjectId tone = doc.add_content(std::make_shared<ToneContent>(440U, 0.5F),
                                        bridge.intern(ToneContent::kind_id, k_version));
  const ObjectId solid_layer = doc.add_layer(solid, arbc::Affine::identity(), 1.0);
  const ObjectId tone_layer = doc.add_layer(tone, arbc::Affine::identity(), 1.0);
  doc.attach_layer(comp, solid_layer);
  doc.attach_layer(comp, tone_layer);
  return BuiltDoc{comp, solid, tone};
}

// The single composition's layer-bound content ids, bottom-to-top, off a pinned root.
std::vector<ObjectId> layer_contents(const arbc::DocStatePtr& state) {
  std::vector<ObjectId> out;
  ObjectId comp_id;
  const arbc::CompositionRecord* comp = nullptr;
  if (!state->find_first_composition(comp_id, comp)) {
    return out;
  }
  state->for_each_layer_in(comp_id, [&](ObjectId lid) {
    const arbc::LayerRecord* lr = state->find_layer(lid);
    if (lr != nullptr) {
      out.push_back(lr->content);
    }
  });
  return out;
}

} // namespace

// enforces: 08-serialization#builtin-solid-tone-codecs-round-trip
TEST_CASE("built-in solid/tone contents round-trip through their codecs into a Document") {
  KindBridge save_bridge;
  Document src;
  build_scene(src, save_bridge);

  const arbc::expected<std::string, arbc::SerializeError> saved =
      arbc::save_document(src, save_bridge);
  REQUIRE(saved.has_value());

  // Reload into a fresh document through a fresh bridge (the round-trip is bijective
  // across an independent bridge instance -- only the reverse-DNS string is durable).
  KindBridge load_bridge;
  Document dst;
  Registry registry;
  const arbc::expected<std::monostate, arbc::ReaderError> loaded =
      arbc::load_document(*saved, dst, load_bridge, registry);
  REQUIRE(loaded.has_value());

  const arbc::DocStatePtr pinned = dst.pin();
  const std::vector<ObjectId> contents = layer_contents(pinned);
  REQUIRE(contents.size() == 2);

  // Each layer binds a live, non-null content (a real ObjectId), and dispatch chose
  // the codec -- the reconstructed content is the concrete built-in, not a placeholder.
  const auto* solid = dynamic_cast<const SolidContent*>(dst.resolve(contents[0]));
  REQUIRE(solid != nullptr);
  CHECK(contents[0] != ObjectId{});
  const Rgba color = solid->color();
  CHECK(color.r == 1.0F);
  CHECK(color.g == 0.5F);
  CHECK(color.b == 0.25F);
  CHECK(color.a == 1.0F);

  const auto* tone = dynamic_cast<const ToneContent*>(dst.resolve(contents[1]));
  REQUIRE(tone != nullptr);
  CHECK(tone->frequency_hz() == 440U);
  CHECK(tone->amplitude() == 0.5F);

  // The sink stamped the bridged kind: the pinned ContentRecord.kind equals the
  // load bridge's interned token for that kind id (bijective across the round-trip).
  const arbc::ContentRecord* solid_rec = pinned->find_content(contents[0]);
  const arbc::ContentRecord* tone_rec = pinned->find_content(contents[1]);
  REQUIRE(solid_rec != nullptr);
  REQUIRE(tone_rec != nullptr);
  CHECK(solid_rec->kind == load_bridge.intern(SolidContent::kind_id, k_version));
  CHECK(tone_rec->kind == load_bridge.intern(ToneContent::kind_id, k_version));
  CHECK(solid_rec->kind != tone_rec->kind);

  // A load lands at the version-0 baseline.
  CHECK(pinned->revision() == 0);
}

// A canonical document whose single layer carries a kind no built-in codec knows.
// Byte-for-byte what the writer emits for a placeholder re-save (sorted keys, 2-space
// indent, trailing newline), so a Document load->save is the identity.
constexpr const char* k_foreign = R"json({
  "arbc": {
    "format": 1
  },
  "composition": {
    "canvas": [
      0,
      0,
      640,
      480
    ],
    "layers": [
      {
        "kind": "com.example.ghost",
        "kind_version": "2",
        "opacity": 1.0,
        "params": {
          "hue": 7
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

// enforces: 08-serialization#unknown-kind-round-trips-verbatim
TEST_CASE("an unknown-kind document survives a Document load->save verbatim") {
  KindBridge bridge;
  Document doc;
  Registry registry; // no factory for com.example.ghost -> the missing-plugin path

  const arbc::expected<std::monostate, arbc::ReaderError> loaded =
      arbc::load_document(k_foreign, doc, bridge, registry);
  REQUIRE(loaded.has_value());

  // The foreign body is preserved through a PlaceholderContent (reserved-unknown kind
  // id) and re-emitted verbatim canonical -- a missing plugin never destroys data.
  const arbc::expected<std::string, arbc::SerializeError> resaved =
      arbc::save_document(doc, bridge);
  REQUIRE(resaved.has_value());
  CHECK(*resaved == std::string(k_foreign));
}

TEST_CASE("malformed built-in params is an error value leaving the Document unmutated") {
  // A single tone layer whose frequency is out of uint32 range: the codec's
  // deserialize rejects it as a value BEFORE the sink runs, so nothing is installed.
  constexpr const char* k_bad_tone = R"json({
  "arbc": {
    "format": 1
  },
  "composition": {
    "canvas": [
      0,
      0,
      640,
      480
    ],
    "layers": [
      {
        "kind": "org.arbc.tone",
        "kind_version": "1",
        "opacity": 1.0,
        "params": {
          "amplitude": 0.5,
          "frequency_hz": 99999999999
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

  // A single solid layer whose `color` param is not a 4-element numeric array: the
  // solid codec rejects it the same way (symmetric error path).
  constexpr const char* k_bad_solid = R"json({
  "arbc": {
    "format": 1
  },
  "composition": {
    "canvas": [
      0,
      0,
      640,
      480
    ],
    "layers": [
      {
        "kind": "org.arbc.solid",
        "kind_version": "1",
        "opacity": 1.0,
        "params": {
          "color": [
            1.0,
            0.5
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
)json";

  KindBridge bridge;
  Registry registry;

  for (const char* bad : {k_bad_tone, k_bad_solid}) {
    Document doc;
    const arbc::expected<std::monostate, arbc::ReaderError> loaded =
        arbc::load_document(bad, doc, bridge, registry);
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().kind == arbc::ReaderError::Kind::MalformedField);

    // The document is untouched: still the empty version-0 baseline, no content bound.
    const arbc::DocStatePtr pinned = doc.pin();
    CHECK(pinned->revision() == 0);
    ObjectId comp_id;
    const arbc::CompositionRecord* comp = nullptr;
    CHECK_FALSE(pinned->find_first_composition(comp_id, comp));
  }
}
