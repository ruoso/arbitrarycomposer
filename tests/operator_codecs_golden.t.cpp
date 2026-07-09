#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/contract/registry.hpp>
#include <arbc/kind_crossfade/crossfade_content.hpp>
#include <arbc/kind_fade/fade_content.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/model/model.hpp>
#include <arbc/model/records.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/document_serialize.hpp>
#include <arbc/serialize/codec.hpp> // CodecTable (to hold builtin_codecs() by value)

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <vector>

// runtime.operator_codecs end-to-end byte-exact golden (docs 08, 17): the observable
// proof that an operator graph built from the built-in operators (org.arbc.fade over
// a shared input, org.arbc.crossfade over that same shared input plus a second) is a
// file you can save and reopen -- the `inputs` array, the `contents` table + `$ref`
// dedup, and the input-child ownership all becoming concrete, tested behavior. Inline
// raw-string golden, byte-exact CHECK -- the serialize golden convention.

namespace {

using arbc::Affine;
using arbc::Content;
using arbc::CrossfadeContent;
using arbc::CrossfadeParams;
using arbc::CrossfadeShape;
using arbc::Document;
using arbc::FadeContent;
using arbc::FadeParams;
using arbc::FadeShape;
using arbc::FadeWindow;
using arbc::KindBridge;
using arbc::ObjectId;
using arbc::Rgba;
using arbc::SolidContent;
using arbc::Time;

// The canonical bytes for the operator graph below (sorted keys, 2-space indent,
// trailing newline, shortest-round-trip numbers). Two layer-root operators (a fade
// and a crossfade) share one solid input, hoisted once into the `contents` table
// under `"$ref": "0"`; the crossfade's second, singly-referenced input stays inline.
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
        "inputs": [
          {
            "$ref": "0"
          }
        ],
        "kind": "org.arbc.fade",
        "kind_version": "1",
        "opacity": 1.0,
        "params": {
          "in": {
            "end": 705600000,
            "start": 0
          },
          "shape": "linear"
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
        "inputs": [
          {
            "$ref": "0"
          },
          {
            "kind": "org.arbc.solid",
            "kind_version": "1",
            "params": {
              "color": [
                0.0,
                0.0,
                1.0,
                1.0
              ]
            }
          }
        ],
        "kind": "org.arbc.crossfade",
        "kind_version": "1",
        "opacity": 1.0,
        "params": {
          "duration": 705600000,
          "shape": "linear",
          "start": 0
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
  "contents": {
    "0": {
      "kind": "org.arbc.solid",
      "kind_version": "1",
      "params": {
        "color": [
          1.0,
          0.5,
          0.25,
          1.0
        ]
      }
    }
  }
}
)json";

// One flick-second, an exact integer core scalar (doc 08 Principle 5).
constexpr std::int64_t k_one_second = Time::flicks_per_second;

// Build: layer 0 = fade(shared), layer 1 = crossfade(shared, other). `shared` is an
// input of BOTH operators (referenced >= 2 times) so the `contents`/`$ref` dedup
// fires; `other` is referenced once (stays inline). The three solids are input
// children of the operators, not layer roots.
struct BuiltGraph {
  ObjectId comp;
  ObjectId fade;
  ObjectId crossfade;
};

BuiltGraph build_graph(Document& doc, KindBridge& bridge) {
  const ObjectId comp = doc.add_composition(1920.0, 1080.0);
  const ObjectId shared =
      doc.add_content(std::make_shared<SolidContent>(Rgba{1.0F, 0.5F, 0.25F, 1.0F}),
                      bridge.intern(SolidContent::kind_id, "1"));
  const ObjectId other =
      doc.add_content(std::make_shared<SolidContent>(Rgba{0.0F, 0.0F, 1.0F, 1.0F}),
                      bridge.intern(SolidContent::kind_id, "1"));

  const FadeParams fp{FadeShape::Linear, FadeWindow{Time{0}, Time{k_one_second}}, std::nullopt};
  const ObjectId fade =
      doc.add_content(std::make_shared<FadeContent>(doc.resolve(shared), fp),
                      bridge.intern(FadeContent::kind_id, "1"));

  const CrossfadeParams cp{CrossfadeShape::Linear, Time{0}, Time{k_one_second}};
  const ObjectId crossfade = doc.add_content(
      std::make_shared<CrossfadeContent>(doc.resolve(shared), doc.resolve(other), cp),
      bridge.intern(CrossfadeContent::kind_id, "1"));

  doc.attach_layer(comp, doc.add_layer(fade, Affine::identity(), 1.0));
  doc.attach_layer(comp, doc.add_layer(crossfade, Affine::identity(), 1.0));
  return BuiltGraph{comp, fade, crossfade};
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

// enforces: 08-serialization#builtin-operator-codecs-round-trip
// enforces: 08-serialization#inputs-array-round-trips
// enforces: 08-serialization#shared-content-dedups-via-ref
// enforces: 08-serialization#writer-serializes-the-pinned-version
// enforces: 14-data-model-and-editing#pinned-version-never-observes-later-edit
TEST_CASE("an operator graph saves byte-exact and reload->resave is identical") {
  const arbc::CodecTable codecs = arbc::builtin_codecs();

  KindBridge bridge;
  Document doc;
  const BuiltGraph built = build_graph(doc, bridge);

  // Capture the pinned content snapshot (incl. the input-child reverse map), emit:
  // byte-exact to the golden.
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
  doc.attach_layer(built.comp, doc.add_layer(extra, Affine::identity(), 1.0));
  const arbc::expected<std::string, arbc::SerializeError> pinned_again =
      arbc::serialize_snapshot(snap, codecs);
  REQUIRE(pinned_again.has_value());
  CHECK(*pinned_again == std::string(k_golden));

  // A fresh save of the current (mutated) version DOES observe the edit.
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

  // The shared input loads to a SINGLE live Content* reached at both sites (intra-
  // document dedup by pointer identity, not a fan-out into copies): the fade's input
  // and the crossfade's `from` input are the very same object.
  const arbc::DocStatePtr pinned = reloaded.pin();
  const std::vector<ObjectId> roots = layer_contents(pinned);
  REQUIRE(roots.size() == 2);
  const auto* fade = dynamic_cast<const FadeContent*>(reloaded.resolve(roots[0]));
  const auto* crossfade = dynamic_cast<const CrossfadeContent*>(reloaded.resolve(roots[1]));
  REQUIRE(fade != nullptr);
  REQUIRE(crossfade != nullptr);
  REQUIRE(fade->inputs().size() == 1);
  REQUIRE(crossfade->inputs().size() == 2);
  CHECK(fade->inputs()[0] == crossfade->inputs()[0]); // shared child, one live node
  CHECK(fade->inputs()[0] != crossfade->inputs()[1]); // the singly-referenced `other`
}
