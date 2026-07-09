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

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// runtime.operator_codecs behavioral tests (docs 08, 17). These drive the `Document`
// load<->save facade for the built-in operator kinds (org.arbc.fade / org.arbc.crossfade)
// without naming the JSON library (nlohmann is PRIVATE to the runtime codec TUs); the
// byte-exact golden and the TSan lane live in the cross-component `tests/` tree.

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
using arbc::Registry;
using arbc::Rgba;
using arbc::SolidContent;
using arbc::Time;

constexpr const char* k_version = "1";

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

// Count non-overlapping occurrences of `needle` in `hay` (a substring witness that a
// body survived a round-trip, used where the component TU cannot parse JSON).
std::size_t count(const std::string& hay, const std::string& needle) {
  std::size_t n = 0;
  for (std::size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + 1)) {
    ++n;
  }
  return n;
}

} // namespace

// enforces: 08-serialization#builtin-operator-codecs-adopt-inputs
TEST_CASE("a fade round-trips its params and adopts its one input through the codec") {
  KindBridge save_bridge;
  Document src;
  const ObjectId comp = src.add_composition(640.0, 480.0);
  const ObjectId leaf =
      src.add_content(std::make_shared<SolidContent>(Rgba{1.0F, 0.5F, 0.25F, 1.0F}),
                      save_bridge.intern(SolidContent::kind_id, k_version));
  const FadeParams fp{FadeShape::Linear, FadeWindow{Time{100}, Time{200}},
                      FadeWindow{Time{300}, Time{400}}};
  const ObjectId fade = src.add_content(std::make_shared<FadeContent>(src.resolve(leaf), fp),
                                        save_bridge.intern(FadeContent::kind_id, k_version));
  src.attach_layer(comp, src.add_layer(fade, Affine::identity(), 1.0));

  const arbc::expected<std::string, arbc::SerializeError> saved =
      arbc::save_document(src, save_bridge);
  REQUIRE(saved.has_value());

  KindBridge load_bridge;
  Document dst;
  Registry registry;
  REQUIRE(arbc::load_document(*saved, dst, load_bridge, registry).has_value());

  const arbc::DocStatePtr pinned = dst.pin();
  const std::vector<ObjectId> roots = layer_contents(pinned);
  REQUIRE(roots.size() == 1);

  // Dispatch selected the fade codec (a live FadeContent, not a PlaceholderContent).
  const auto* loaded_fade = dynamic_cast<const FadeContent*>(dst.resolve(roots[0]));
  REQUIRE(loaded_fade != nullptr);

  // The reconstructed params equal the originals (byte-exact params round-trip); both
  // the `in` and `out` windows survive.
  const FadeParams& lp = loaded_fade->params();
  CHECK(lp.shape == FadeShape::Linear);
  REQUIRE(lp.in.has_value());
  CHECK(lp.in->start.flicks == 100);
  CHECK(lp.in->end.flicks == 200);
  REQUIRE(lp.out.has_value());
  CHECK(lp.out->start.flicks == 300);
  CHECK(lp.out->end.flicks == 400);

  // The one input edge is adopted in slot 0 as the built solid input child.
  REQUIRE(loaded_fade->inputs().size() == 1);
  CHECK(dynamic_cast<const SolidContent*>(loaded_fade->inputs()[0]) != nullptr);
}

// enforces: 08-serialization#builtin-operator-codecs-adopt-inputs
TEST_CASE("a crossfade round-trips its params and adopts from=0/to=1 through the codec") {
  KindBridge save_bridge;
  Document src;
  const ObjectId comp = src.add_composition(640.0, 480.0);
  const ObjectId from = // red
      src.add_content(std::make_shared<SolidContent>(Rgba{1.0F, 0.0F, 0.0F, 1.0F}),
                      save_bridge.intern(SolidContent::kind_id, k_version));
  const ObjectId to = // green
      src.add_content(std::make_shared<SolidContent>(Rgba{0.0F, 1.0F, 0.0F, 1.0F}),
                      save_bridge.intern(SolidContent::kind_id, k_version));
  const CrossfadeParams cp{CrossfadeShape::Linear, Time{50}, Time{300}};
  const ObjectId crossfade =
      src.add_content(std::make_shared<CrossfadeContent>(src.resolve(from), src.resolve(to), cp),
                      save_bridge.intern(CrossfadeContent::kind_id, k_version));
  src.attach_layer(comp, src.add_layer(crossfade, Affine::identity(), 1.0));

  const arbc::expected<std::string, arbc::SerializeError> saved =
      arbc::save_document(src, save_bridge);
  REQUIRE(saved.has_value());

  KindBridge load_bridge;
  Document dst;
  Registry registry;
  REQUIRE(arbc::load_document(*saved, dst, load_bridge, registry).has_value());

  const arbc::DocStatePtr pinned = dst.pin();
  const std::vector<ObjectId> roots = layer_contents(pinned);
  REQUIRE(roots.size() == 1);
  const auto* loaded = dynamic_cast<const CrossfadeContent*>(dst.resolve(roots[0]));
  REQUIRE(loaded != nullptr);

  const CrossfadeParams& lp = loaded->params();
  CHECK(lp.shape == CrossfadeShape::Linear);
  CHECK(lp.start.flicks == 50);
  CHECK(lp.duration.flicks == 300);

  // Input slots are order-significant: from=0 (red), to=1 (green).
  REQUIRE(loaded->inputs().size() == 2);
  const auto* slot0 = dynamic_cast<const SolidContent*>(loaded->inputs()[0]);
  const auto* slot1 = dynamic_cast<const SolidContent*>(loaded->inputs()[1]);
  REQUIRE(slot0 != nullptr);
  REQUIRE(slot1 != nullptr);
  CHECK(slot0->color().r == 1.0F);
  CHECK(slot1->color().g == 1.0F);
}

// enforces: 08-serialization#builtin-operator-codecs-round-trip
TEST_CASE("an operator input child is owned by Document with no layer binding") {
  KindBridge save_bridge;
  Document src;
  const ObjectId comp = src.add_composition(640.0, 480.0);
  const ObjectId leaf =
      src.add_content(std::make_shared<SolidContent>(Rgba{0.2F, 0.4F, 0.6F, 1.0F}),
                      save_bridge.intern(SolidContent::kind_id, k_version));
  const FadeParams fp{FadeShape::Linear, std::nullopt, std::nullopt};
  const ObjectId fade = src.add_content(std::make_shared<FadeContent>(src.resolve(leaf), fp),
                                        save_bridge.intern(FadeContent::kind_id, k_version));
  src.attach_layer(comp, src.add_layer(fade, Affine::identity(), 1.0));

  const arbc::expected<std::string, arbc::SerializeError> saved =
      arbc::save_document(src, save_bridge);
  REQUIRE(saved.has_value());

  KindBridge load_bridge;
  Document dst;
  Registry registry;
  REQUIRE(arbc::load_document(*saved, dst, load_bridge, registry).has_value());

  const arbc::DocStatePtr pinned = dst.pin();
  const std::vector<ObjectId> roots = layer_contents(pinned);
  REQUIRE(roots.size() == 1);

  // The layer root binds an ObjectId AND a ContentRecord (find_content resolves it).
  CHECK(roots[0] != ObjectId{});
  CHECK(pinned->find_content(roots[0]) != nullptr);
  const auto* loaded_fade = dynamic_cast<const FadeContent*>(dst.resolve(roots[0]));
  REQUIRE(loaded_fade != nullptr);

  // Its input child is a live built-in content owned by Document, but it is NOT a
  // layer root: no layer binds it (the root/child ownership split, sharing Decision 3).
  REQUIRE(loaded_fade->inputs().size() == 1);
  const Content* child = loaded_fade->inputs()[0];
  CHECK(dynamic_cast<const SolidContent*>(child) != nullptr);
  for (const ObjectId root : roots) {
    CHECK(dst.resolve(root) != child); // the child is bound by no layer
  }
}

// enforces: 08-serialization#builtin-operator-codecs-round-trip
TEST_CASE("the writer resolves codec-backed input-child kinds so re-save is stable") {
  // A crossfade over two live solids: on re-save the writer resolves BOTH input
  // children's (kind, kind_version) through the live reverse map, so both solid input
  // bodies are present and the round-trip is idempotent.
  KindBridge save_bridge;
  Document src;
  const ObjectId comp = src.add_composition(640.0, 480.0);
  const ObjectId from =
      src.add_content(std::make_shared<SolidContent>(Rgba{1.0F, 0.0F, 0.0F, 1.0F}),
                      save_bridge.intern(SolidContent::kind_id, k_version));
  const ObjectId to = src.add_content(std::make_shared<SolidContent>(Rgba{0.0F, 1.0F, 0.0F, 1.0F}),
                                      save_bridge.intern(SolidContent::kind_id, k_version));
  const CrossfadeParams cp{CrossfadeShape::Linear, Time{0}, Time{100}};
  const ObjectId crossfade =
      src.add_content(std::make_shared<CrossfadeContent>(src.resolve(from), src.resolve(to), cp),
                      save_bridge.intern(CrossfadeContent::kind_id, k_version));
  src.attach_layer(comp, src.add_layer(crossfade, Affine::identity(), 1.0));

  const arbc::expected<std::string, arbc::SerializeError> saved =
      arbc::save_document(src, save_bridge);
  REQUIRE(saved.has_value());
  CHECK(count(*saved, "org.arbc.solid") == 2); // both input bodies present
  CHECK(count(*saved, "org.arbc.crossfade") == 1);

  KindBridge load_bridge;
  Document dst;
  Registry registry;
  REQUIRE(arbc::load_document(*saved, dst, load_bridge, registry).has_value());
  const arbc::expected<std::string, arbc::SerializeError> resaved =
      arbc::save_document(dst, load_bridge);
  REQUIRE(resaved.has_value());
  CHECK(*resaved == *saved); // idempotent: no NoCodec on the input children
}

// enforces: 08-serialization#unknown-kind-round-trips-verbatim
TEST_CASE("a placeholder input child re-emits its stored body verbatim") {
  // A fade whose single input is a foreign kind no codec knows: the input child loads
  // as a PlaceholderContent (meta -> nullopt on save), and its stored body re-emits
  // verbatim -- a missing plugin never destroys data even inside an operator graph.
  constexpr const char* k_fade_over_ghost =
      R"json({"arbc":{"format":1},"composition":{"canvas":[0,0,64,64],"layers":[{"transform":[1,0,0,1,0,0],"opacity":1.0,"visible":true,"kind":"org.arbc.fade","kind_version":"1","params":{"shape":"linear"},"inputs":[{"kind":"com.example.ghost","kind_version":"9","params":{"hue":3}}]}]}})json";

  KindBridge bridge;
  Document doc;
  Registry registry; // no factory for com.example.ghost
  REQUIRE(arbc::load_document(k_fade_over_ghost, doc, bridge, registry).has_value());

  const arbc::expected<std::string, arbc::SerializeError> saved = arbc::save_document(doc, bridge);
  REQUIRE(saved.has_value());
  CHECK(count(*saved, "com.example.ghost") == 1); // the foreign input survived
  CHECK(count(*saved, "org.arbc.fade") == 1);

  // Re-loading the canonical bytes and re-saving is byte-identical (verbatim re-emit).
  KindBridge bridge2;
  Document doc2;
  REQUIRE(arbc::load_document(*saved, doc2, bridge2, registry).has_value());
  const arbc::expected<std::string, arbc::SerializeError> resaved =
      arbc::save_document(doc2, bridge2);
  REQUIRE(resaved.has_value());
  CHECK(*resaved == *saved);
}

// enforces: 08-serialization#dangling-ref-is-read-error
TEST_CASE("malformed operator bodies and a dangling ref are error values, doc unmutated") {
  Registry registry;

  struct Case {
    const char* json;
    arbc::ReaderError::Kind kind;
  };
  // Each malformed body carries NO input children, so the error faults before any
  // child is bound and the document is left at the empty version-0 baseline.
  const Case cases[] = {
      // Unknown fade shape.
      {R"json({"arbc":{"format":1},"composition":{"canvas":[0,0,8,8],"layers":[{"kind":"org.arbc.fade","params":{"shape":"bogus"}}]}})json",
       arbc::ReaderError::Kind::MalformedField},
      // Wrong fade arity (zero inputs, valid shape).
      {R"json({"arbc":{"format":1},"composition":{"canvas":[0,0,8,8],"layers":[{"kind":"org.arbc.fade","params":{"shape":"linear"}}]}})json",
       arbc::ReaderError::Kind::MalformedField},
      // Malformed fade window (non-integer start).
      {R"json({"arbc":{"format":1},"composition":{"canvas":[0,0,8,8],"layers":[{"kind":"org.arbc.fade","params":{"shape":"linear","in":{"start":"x","end":1}}}]}})json",
       arbc::ReaderError::Kind::MalformedField},
      // Non-object crossfade params (caught at the core frame before dispatch).
      {R"json({"arbc":{"format":1},"composition":{"canvas":[0,0,8,8],"layers":[{"kind":"org.arbc.crossfade","params":5}]}})json",
       arbc::ReaderError::Kind::MalformedField},
      // Unknown crossfade shape.
      {R"json({"arbc":{"format":1},"composition":{"canvas":[0,0,8,8],"layers":[{"kind":"org.arbc.crossfade","params":{"shape":"bogus"}}]}})json",
       arbc::ReaderError::Kind::MalformedField},
      // Crossfade missing `start`.
      {R"json({"arbc":{"format":1},"composition":{"canvas":[0,0,8,8],"layers":[{"kind":"org.arbc.crossfade","params":{"shape":"linear","duration":5}}]}})json",
       arbc::ReaderError::Kind::MalformedField},
      // Crossfade missing `duration`.
      {R"json({"arbc":{"format":1},"composition":{"canvas":[0,0,8,8],"layers":[{"kind":"org.arbc.crossfade","params":{"shape":"linear","start":5}}]}})json",
       arbc::ReaderError::Kind::MalformedField},
      // Dangling $ref in a fade input (no contents table).
      {R"json({"arbc":{"format":1},"composition":{"canvas":[0,0,8,8],"layers":[{"kind":"org.arbc.fade","params":{"shape":"linear"},"inputs":[{"$ref":"7"}]}]}})json",
       arbc::ReaderError::Kind::UnresolvableReference},
  };

  for (const Case& c : cases) {
    KindBridge bridge;
    Document doc;
    const arbc::expected<std::monostate, arbc::ReaderError> loaded =
        arbc::load_document(c.json, doc, bridge, registry);
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().kind == c.kind);

    // The document is untouched: still the empty version-0 baseline, no composition.
    const arbc::DocStatePtr pinned = doc.pin();
    CHECK(pinned->revision() == 0);
    ObjectId comp_id;
    const arbc::CompositionRecord* comp = nullptr;
    CHECK_FALSE(pinned->find_first_composition(comp_id, comp));
  }
}
