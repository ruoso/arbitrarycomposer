// runtime.nested_codec end-to-end: a REAL `NestedContent` -- not a test double -- round-trips
// through a live `Document` and the L5 `save_document`/`load_document` façade, byte-exact.
// The predecessor (serialize.compositions_table) proved the compositions-table FORMAT with a
// `NestKind` double whose `inputs()` is always empty; this pins the format against the kind
// whose `inputs()` is memo-derived from its child composition's layers, which is where the
// format's one real hazard lives.
//
// CROSS-COMPONENT (tests/, linking the umbrella `arbc` + nlohmann): the attach-invariance
// proof needs a real Backend and a real `OperatorBindingScope`, which a runtime component
// test may not name (doc 17 / check_levels.py). The codec's own units live in
// src/runtime/t/nested_codec.t.cpp.
//
// The invariant these goldens exist to pin (doc 08 Principle 7's closing rule, Decision 1):
// a nesting content's input edges are a PROJECTION of its child composition, never authored
// data, so the writer neither descends nor emits them -- and saving a document is therefore a
// pure function of the document, not of whether a render binding happened to be attached when
// the save was taken.

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/expected.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/compositor/pull_service.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/contract/registry.hpp>
#include <arbc/kind_fade/fade_content.hpp>
#include <arbc/kind_nested/nested_content.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/model/model.hpp>
#include <arbc/model/records.hpp>
#include <arbc/runtime/builtin_codecs.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/document_serialize.hpp>
#include <arbc/runtime/operator_binding.hpp>
#include <arbc/runtime/pull_identity.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <string>

using arbc::Affine;
using arbc::CompositionRecord;
using arbc::ContentResolver;
using arbc::CpuBackend;
using arbc::DocRoot;
using arbc::DocStatePtr;
using arbc::Document;
using arbc::FadeContent;
using arbc::FadeParams;
using arbc::FadeShape;
using arbc::FadeWindow;
using arbc::KindBridge;
using arbc::LayerRecord;
using arbc::NestedContent;
using arbc::ObjectId;
using arbc::PullConfig;
using arbc::PullServiceImpl;
using arbc::Registry;
using arbc::Rgba;
using arbc::SerializeError;
using arbc::SolidContent;
using arbc::TileCache;
using arbc::Time;

namespace {

std::uint64_t solid_kind(KindBridge& bridge) {
  return bridge.intern(SolidContent::kind_id, arbc::k_solid_kind_version);
}

std::uint64_t nested_kind(KindBridge& bridge) {
  return bridge.intern(NestedContent::kind_id, arbc::k_nested_kind_version);
}

std::uint64_t fade_kind(KindBridge& bridge) {
  return bridge.intern(FadeContent::kind_id, arbc::k_fade_kind_version);
}

std::string save(const Document& doc, const KindBridge& bridge) {
  const arbc::expected<std::string, SerializeError> out = arbc::save_document(doc, bridge);
  REQUIRE(out);
  return *out;
}

// Load `bytes` into a fresh Document through the full runtime façade and re-save it. The
// reader/writer fixed point every golden below pins.
std::string reload_and_resave(const std::string& bytes) {
  Document doc;
  KindBridge bridge;
  const Registry registry;
  REQUIRE(arbc::load_document(bytes, doc, bridge, registry));
  return save(doc, bridge);
}

// The live PullService config the render drivers build (`offline_sequence.cpp`).
PullConfig live_config(const DocRoot& pin, const ContentResolver& resolve) {
  PullConfig config;
  config.id_of = arbc::make_pull_identity_of(pin, resolve);
  const std::uint64_t revision = pin.revision();
  config.contribution = [revision](const arbc::Content*) { return revision; };
  return config;
}

// A live render binding over `doc`, exactly as `SequenceRenderer` takes one for every frame
// of an interactive session or an offline export -- the state in which a real save genuinely
// happens (autosave during playback; `offline_sequence.cpp` holds a binding for the length
// of an export). It is what makes a bound `NestedContent`'s `inputs()` non-empty.
struct Binding {
  CpuBackend backend;
  TileCache cache{64U * 1024 * 1024};
  DocStatePtr pin;
  ContentResolver resolve;
  std::unique_ptr<PullServiceImpl> service;
  arbc::OperatorBindingScope scope;

  explicit Binding(Document& doc) : pin(doc.pin()) {
    resolve = [&doc](ObjectId id) { return doc.resolve(id); };
    service = std::make_unique<PullServiceImpl>(cache, backend, arbc::direct_dispatch(),
                                                live_config(*pin, resolve));
    arbc::register_builtin_operator_binders();
    scope = arbc::bind_operators(doc, *service, backend, pin);
  }
};

// The document the plain-nesting golden pins: a root composition whose single layer holds a
// real NestedContent over a child composition of one solid. The PARENT is created before the
// child (Constraint 9), so the root is the lowest-id composition.
struct NestingScene {
  Document doc;
  KindBridge bridge;
  ObjectId root;
  ObjectId child;
  std::shared_ptr<NestedContent> nested;

  NestingScene() {
    root = doc.add_composition(16.0, 16.0);
    child = doc.add_composition(8.0, 8.0);
    nested = std::make_shared<NestedContent>(child);
    doc.attach_layer(
        root, doc.add_layer(doc.add_content(nested, nested_kind(bridge)), Affine::identity(), 1.0));
    const ObjectId solid = doc.add_content(
        std::make_shared<SolidContent>(Rgba{0.0F, 1.0F, 0.0F, 1.0F}), solid_kind(bridge));
    doc.attach_layer(child, doc.add_layer(solid, Affine::identity(), 1.0));
  }
};

// The Droste scene: the root composition embeds ITSELF. The nesting content is a member layer
// of the very composition it names, so a BOUND nested reports itself as its own transitive
// input -- the shape whose pre-fix save emitted a `$ref` closing an operator-input cycle.
struct DrosteScene {
  Document doc;
  KindBridge bridge;
  ObjectId root;
  std::shared_ptr<NestedContent> nested;

  DrosteScene() {
    root = doc.add_composition(16.0, 16.0);
    nested = std::make_shared<NestedContent>(root);
    doc.attach_layer(
        root, doc.add_layer(doc.add_content(nested, nested_kind(bridge)), Affine::identity(), 1.0));
  }
};

const char* const k_nesting_golden = R"json({
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

// The Droste back-edge is spelled `"composition": "0"` -- the root's reserved ordinal -- and
// the document carries NO `compositions` key (the root keeps its home at the root
// `composition` object and is never a table key) and NO `contents` key (nothing is shared;
// the cycle is a COMPOSITION cycle, not a `$ref` one).
const char* const k_droste_golden = R"json({
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
        "composition": "0",
        "kind": "org.arbc.nested",
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
  }
}
)json";

} // namespace

// enforces: 08-serialization#builtin-nested-codec-round-trips
// enforces: 08-serialization#child-compositions-round-trip-in-document
// enforces: 16-sdlc-and-quality#byte-exact-goldens
TEST_CASE("a real org.arbc.nested content round-trips byte-exact through the runtime façade") {
  NestingScene scene;
  const std::string saved = save(scene.doc, scene.bridge);
  CHECK(saved == std::string(k_nesting_golden));

  // load -> save is the fixed point: the codec rebuilt a live NestedContent around the child
  // ObjectId the reader pre-allocated, and the writer re-derived the same ordinal.
  CHECK(reload_and_resave(saved) == saved);
}

// enforces: 08-serialization#droste-cycle-round-trips-as-data
// enforces: 08-serialization#builtin-nested-codec-round-trips
TEST_CASE("a Droste self-cycle round-trips byte-exact through the REAL nested kind") {
  DrosteScene scene;
  const std::string saved = save(scene.doc, scene.bridge);
  CHECK(saved == std::string(k_droste_golden));
  CHECK(saved.find("\"compositions\"") == std::string::npos);
  CHECK(saved.find("\"contents\"") == std::string::npos);
  CHECK(saved.find("\"$ref\"") == std::string::npos);

  CHECK(reload_and_resave(saved) == saved);
}

// enforces: 08-serialization#child-compositions-round-trip-in-document
TEST_CASE("two nested contents naming the same child emit one table entry and two references") {
  Document doc;
  KindBridge bridge;
  const ObjectId root = doc.add_composition(16.0, 16.0);
  const ObjectId child = doc.add_composition(8.0, 8.0);
  // Two DISTINCT nesting contents over the SAME child composition. The child is emitted once,
  // under one ordinal, and named twice -- composition sharing rides the `compositions` table,
  // never the `contents`/`$ref` table (the two nesting contents are not themselves shared).
  for (int i = 0; i < 2; ++i) {
    const ObjectId nest =
        doc.add_content(std::make_shared<NestedContent>(child), nested_kind(bridge));
    doc.attach_layer(root, doc.add_layer(nest, Affine::identity(), 1.0));
  }
  const ObjectId solid = doc.add_content(
      std::make_shared<SolidContent>(Rgba{0.0F, 1.0F, 0.0F, 1.0F}), solid_kind(bridge));
  doc.attach_layer(child, doc.add_layer(solid, Affine::identity(), 1.0));

  const std::string saved = save(doc, bridge);
  // One table entry...
  CHECK(saved.find("\"compositions\"") != std::string::npos);
  CHECK(saved.find("\"2\": {") == std::string::npos);
  // ...named twice.
  const std::size_t first = saved.find("\"composition\": \"1\"");
  REQUIRE(first != std::string::npos);
  const std::size_t second = saved.find("\"composition\": \"1\"", first + 1);
  CHECK(second != std::string::npos);
  CHECK(saved.find("\"composition\": \"1\"", second + 1) == std::string::npos);
  CHECK(saved.find("\"contents\"") == std::string::npos);

  CHECK(reload_and_resave(saved) == saved);
}

// enforces: 08-serialization#nesting-inputs-are-derived-not-persisted
// enforces: 16-sdlc-and-quality#byte-exact-goldens
TEST_CASE("saving a nesting document is invariant under a live render binding") {
  NestingScene scene;
  const std::string unbound = save(scene.doc, scene.bridge);
  REQUIRE(unbound == std::string(k_nesting_golden));
  REQUIRE_FALSE(scene.nested->attached());

  const Binding binding(scene.doc);
  REQUIRE(binding.scope.size() == 1U);
  REQUIRE(scene.nested->attached());
  // The state that used to corrupt the save: the memo is keyed and nested now reports the
  // child's member layers as its inputs. This is what `bind_operators` leaves behind for
  // EVERY frame of an interactive session or an offline export, so a save taken during one
  // (an autosave; a crash-recovery snapshot) sees exactly this.
  REQUIRE_FALSE(scene.nested->inputs().empty());

  // Saving is a pure function of the DOCUMENT. Not one byte moves.
  const std::string bound = save(scene.doc, scene.bridge);
  CHECK(bound == unbound);
  // Concretely: the derived edges are neither emitted as an `inputs` array nor counted as
  // occurrences -- so the child's solid is not hoisted into `contents` behind a `$ref` it
  // never earned. Pre-fix, both of those keys appeared here and nowhere else.
  CHECK(bound.find("\"inputs\"") == std::string::npos);
  CHECK(bound.find("\"contents\"") == std::string::npos);
  CHECK(bound.find("\"$ref\"") == std::string::npos);
  CHECK(reload_and_resave(bound) == bound);
}

// enforces: 08-serialization#nesting-inputs-are-derived-not-persisted
// enforces: 08-serialization#droste-cycle-round-trips-as-data
TEST_CASE("a BOUND Droste save is byte-identical and still loads -- no operator-input cycle") {
  DrosteScene scene;
  const std::string unbound = save(scene.doc, scene.bridge);
  REQUIRE(unbound == std::string(k_droste_golden));

  const Binding binding(scene.doc);
  REQUIRE(binding.scope.size() == 1U);
  REQUIRE(scene.nested->attached());
  // A bound Droste nested is its OWN transitive input: the composition it names is the one
  // whose member layer holds it.
  REQUIRE_FALSE(scene.nested->inputs().empty());

  const std::string bound = save(scene.doc, scene.bridge);
  CHECK(bound == unbound);
  // The pre-fix writer instead counted the nesting content twice (once as a layer root, once
  // as its own derived input), hoisted it into `contents`, and emitted a `$ref` closing an
  // operator-input cycle -- which doc 08 Principle 6 forbids and the reader rejects as
  // UnresolvableReference. That save wrote a file that would not load. This one does.
  CHECK(bound.find("\"$ref\"") == std::string::npos);
  CHECK(bound.find("\"contents\"") == std::string::npos);

  Document reloaded;
  KindBridge fresh;
  const Registry registry;
  REQUIRE(arbc::load_document(bound, reloaded, fresh, registry));
  CHECK(reload_and_resave(bound) == bound);
}

// enforces: 08-serialization#builtin-nested-codec-round-trips
// enforces: 08-serialization#nesting-inputs-are-derived-not-persisted
// The other goldens reach the nesting content as a LAYER ROOT, whose kind the save reads off
// the ObjectId-keyed ContentRecord. Here it is an operator INPUT CHILD instead -- a fade over a
// nested child composition -- and an input child is reached through no layer, so its kind can
// only be recovered from its live type through the input-child reverse map
// (runtime.operator_codecs Decision 5). That is the one path on which the save names
// `org.arbc.nested`, and both halves of Principle 7 have to survive it: the child-composition
// edge is still emitted (so the body is a real nested content, not a verbatim placeholder), and
// the derived `inputs()` are still not descended -- in a position where the walk is already
// mid-descent through the fade's authored inputs, and under a live binding at that.
TEST_CASE("a nesting content in operator-input position keeps its kind and its child edge") {
  Document doc;
  KindBridge bridge;
  const ObjectId root = doc.add_composition(16.0, 16.0);
  const ObjectId child = doc.add_composition(8.0, 8.0);
  // The nesting content is `add_content`'d (so it owns an ObjectId) but never `attach_layer`'d:
  // it hangs off the fade's input slot alone.
  const auto nested = std::make_shared<NestedContent>(child);
  const ObjectId nest = doc.add_content(nested, nested_kind(bridge));
  const FadeParams params{FadeShape::Linear, FadeWindow{Time{0}, Time{Time::flicks_per_second}},
                          std::nullopt};
  const ObjectId fade =
      doc.add_content(std::make_shared<FadeContent>(doc.resolve(nest), params), fade_kind(bridge));
  doc.attach_layer(root, doc.add_layer(fade, Affine::identity(), 1.0));
  const ObjectId solid = doc.add_content(
      std::make_shared<SolidContent>(Rgba{0.0F, 1.0F, 0.0F, 1.0F}), solid_kind(bridge));
  doc.attach_layer(child, doc.add_layer(solid, Affine::identity(), 1.0));

  const std::string unbound = save(doc, bridge);
  // The fade's authored input is the nesting content, emitted inline with its own kind
  // recovered -- an unrecovered kind would have demoted the body to a verbatim placeholder and
  // dropped the child edge with it.
  CHECK(unbound.find("\"kind\": \"org.arbc.fade\"") != std::string::npos);
  CHECK(unbound.find("\"kind\": \"org.arbc.nested\"") != std::string::npos);
  // ...naming the child composition, which is itself emitted in the table.
  CHECK(unbound.find("\"composition\": \"1\"") != std::string::npos);
  CHECK(unbound.find("\"compositions\"") != std::string::npos);
  CHECK(reload_and_resave(unbound) == unbound);

  const Binding binding(doc);
  REQUIRE(nested->attached());
  REQUIRE_FALSE(nested->inputs().empty());

  // Saving is still a pure function of the document: the fade's ONE authored input edge is
  // emitted, and the nesting content's derived edges under it are not -- so the child's solid is
  // never hoisted behind a `$ref` it did not earn. The only `inputs` key is the fade's.
  const std::string bound = save(doc, bridge);
  CHECK(bound == unbound);
  CHECK(bound.find("\"$ref\"") == std::string::npos);
  CHECK(bound.find("\"contents\"") == std::string::npos);
  const std::size_t inputs = bound.find("\"inputs\"");
  REQUIRE(inputs != std::string::npos);
  CHECK(bound.find("\"inputs\"", inputs + 1) == std::string::npos);
  CHECK(reload_and_resave(bound) == bound);
}
