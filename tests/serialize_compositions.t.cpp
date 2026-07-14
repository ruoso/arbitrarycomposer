// serialize.compositions_table: the L4 read/write of the document's GRAPH of
// compositions -- the document-level `compositions` table beside `contents`, the
// core-owned `"composition"` field on a content body, the root's reserved ordinal `"0"`,
// legal Droste cycles, and the two new read errors (doc 08 Principle 7, doc 05). Like
// serialize.sharing these tests name concrete test kinds AND `nlohmann::json` through the
// serialize-internal codec/placeholder headers, so they are cross-component and link the
// umbrella `arbc` plus nlohmann explicitly.
//
// The `org.arbc.nested` codec itself is runtime.nested_codec (M8, which depends on this):
// these tests prove the format and every seam it needs with a purpose-built nesting test
// kind -- and, for the missing-plugin case, with no codec at all.

#include <arbc/base/expected.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/contract/registry.hpp>
#include <arbc/model/model.hpp>
#include <arbc/model/records.hpp>
#include <arbc/serialize/codec.hpp>
#include <arbc/serialize/load_context.hpp>
#include <arbc/serialize/placeholder_content.hpp>
#include <arbc/serialize/reader.hpp>
#include <arbc/serialize/writer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

using arbc::Codec;
using arbc::CodecTable;
using arbc::CompositionRecord;
using arbc::Content;
using arbc::ContentBody;
using arbc::ContentBodyProvider;
using arbc::ContentMeta;
using arbc::ContentMetaProvider;
using arbc::ContentRef;
using arbc::ContentSink;
using arbc::LoadContext;
using arbc::Model;
using arbc::ObjectId;
using arbc::PlaceholderContent;
using arbc::ReaderError;
using arbc::Registry;
using arbc::RenderRequest;
using arbc::SerializeError;
using arbc::SunkContent;
using json = nlohmann::json;

namespace {

// The write-side asset seam (serialize.raster_tile_store Decision 1). These codecs store
// no asset bytes, so a default sink-less context is exactly right: it is threaded through
// for the signature, never consulted.
arbc::SaveContext save_ctx;

// A leaf test kind, so a child composition has something to hold.
class ClipContent final : public Content {
public:
  explicit ClipContent(std::string src) : d_src(std::move(src)) {}
  const std::string& src() const { return d_src; }

  std::optional<arbc::Rect> bounds() const override { return std::nullopt; }
  arbc::Stability stability() const override { return arbc::Stability::Static; }
  std::optional<arbc::TimeRange> time_extent() const override { return std::nullopt; }
  std::optional<arbc::RenderResult> render(const RenderRequest& request,
                                           std::shared_ptr<arbc::RenderCompletion>) override {
    return arbc::RenderResult{request.scale, true};
  }

  static constexpr const char* kind_id = "com.test.clip";

private:
  std::string d_src;
};

// A NESTING test kind (the org.arbc.nested shape): it holds its child composition as a
// bare `ObjectId` and surfaces it to the core through `composition_ref()` -- the exact
// mirror of `inputs()` (doc 03, doc 08 Principle 7). Its `params` carry NOTHING about the
// child: the reference is core-owned graph structure the writer re-derives on every save
// (Constraint 1).
class NestKind final : public Content {
public:
  explicit NestKind(ObjectId child, std::vector<ContentRef> inputs = {})
      : d_child(child), d_inputs(std::move(inputs)) {}

  std::optional<arbc::Rect> bounds() const override { return std::nullopt; }
  arbc::Stability stability() const override { return arbc::Stability::Static; }
  std::optional<arbc::TimeRange> time_extent() const override { return std::nullopt; }
  std::optional<arbc::RenderResult> render(const RenderRequest& request,
                                           std::shared_ptr<arbc::RenderCompletion>) override {
    return arbc::RenderResult{request.scale, true};
  }
  std::span<const ContentRef> inputs() const override { return d_inputs; }
  ObjectId composition_ref() const override { return d_child; }

  static constexpr const char* kind_id = "com.test.nest";

private:
  ObjectId d_child;
  std::vector<ContentRef> d_inputs;
};

arbc::DeserializeFn clip_deserialize() {
  return [](const json& params, std::span<const ContentRef> /*inputs*/, ObjectId /*composition*/,
            LoadContext&) -> arbc::expected<std::unique_ptr<Content>, ReaderError> {
    std::string src;
    if (const auto it = params.find("src"); it != params.end() && it->is_string()) {
      src = it->get<std::string>();
    }
    return std::unique_ptr<Content>(std::make_unique<ClipContent>(std::move(src)));
  };
}

arbc::SerializeFn clip_serialize() {
  return [](const Content& c, arbc::SaveContext& /*ctx*/) -> arbc::expected<json, SerializeError> {
    const auto* cc = dynamic_cast<const ClipContent*>(&c);
    if (cc == nullptr) {
      return arbc::unexpected(SerializeError{SerializeError::Kind::CodecFailed, ObjectId{}});
    }
    json params = json::object();
    params["src"] = cc->src();
    return params;
  };
}

// The nesting kind's read codec builds itself around the child `ObjectId` the reader
// ALREADY resolved and pre-allocated (Decision 5) -- which is why a Droste back-edge
// costs nothing: the id exists long before the child composition's layers do.
arbc::DeserializeFn nest_deserialize() {
  return [](const json& /*params*/, std::span<const ContentRef> inputs, ObjectId composition,
            LoadContext&) -> arbc::expected<std::unique_ptr<Content>, ReaderError> {
    return std::unique_ptr<Content>(std::make_unique<NestKind>(
        composition, std::vector<ContentRef>(inputs.begin(), inputs.end())));
  };
}

// Deliberately emits NO child reference: `SerializeFn` is unchanged by this task, because
// the core appends the re-derived `"composition"` after the codec returns (Constraint 1).
arbc::SerializeFn nest_serialize() {
  return [](const Content& c, arbc::SaveContext& /*ctx*/) -> arbc::expected<json, SerializeError> {
    if (dynamic_cast<const NestKind*>(&c) == nullptr) {
      return arbc::unexpected(SerializeError{SerializeError::Kind::CodecFailed, ObjectId{}});
    }
    return json::object();
  };
}

CodecTable nest_table() {
  CodecTable codecs;
  codecs.add(ClipContent::kind_id, Codec{clip_serialize(), clip_deserialize()});
  codecs.add(NestKind::kind_id, Codec{nest_serialize(), nest_deserialize()});
  return codecs;
}

std::string_view kind_of(const Content& c) {
  if (dynamic_cast<const NestKind*>(&c) != nullptr) {
    return NestKind::kind_id;
  }
  if (dynamic_cast<const ClipContent*>(&c) != nullptr) {
    return ClipContent::kind_id;
  }
  return {};
}

// A hand-built multi-composition document: the `Model` (the composition/layer records) and
// the live contents the L5 side-map would otherwise own, wired through the writer's two
// provider seams.
struct Scene {
  Model model;
  std::vector<std::unique_ptr<Content>> owned;
  std::unordered_map<ObjectId, Content*> bound;     // layer-root content id -> live node
  std::unordered_map<const Content*, ObjectId> ids; // live node -> its id (stash key)

  Content* own(std::unique_ptr<Content> c) {
    Content* const live = c.get();
    owned.push_back(std::move(c));
    return live;
  }

  ContentBodyProvider provider() const {
    return [this](ObjectId id) -> std::optional<ContentBody> {
      const auto it = bound.find(id);
      if (it == bound.end()) {
        return std::nullopt;
      }
      return ContentBody{kind_of(*it->second), "1.0", *it->second};
    };
  }

  ContentMetaProvider meta() const {
    return [this](const Content& c) -> std::optional<ContentMeta> {
      const std::string_view kind = kind_of(c);
      if (kind.empty()) {
        return std::nullopt; // a placeholder: its stored body wins
      }
      const auto it = ids.find(&c);
      return ContentMeta{kind, "1.0", it != ids.end() ? it->second : ObjectId{}};
    };
  }
};

// A stub sink that owns every reconstructed node and records `id -> live`, so a test can
// prove intra-document dedup by pointer identity and re-drive the writer.
struct RecordingSink {
  std::vector<std::unique_ptr<Content>> owned;
  std::unordered_map<ObjectId, Content*> by_id;
  std::uint64_t next = 1000;

  ContentSink as_sink() {
    return [this](std::unique_ptr<Content> c) -> SunkContent {
      Content* const live = c.get();
      owned.push_back(std::move(c));
      const ObjectId id{next++};
      by_id.emplace(id, live);
      return SunkContent{id, live};
    };
  }
};

std::string canonical(const arbc::expected<std::string, SerializeError>& out) {
  REQUIRE(out);
  return *out;
}

// Load `bytes`, then re-serialize the loaded document by re-deriving both provider seams
// from the freshly-sunk nodes -- the reader/writer fixed point the goldens below pin.
std::string reload_and_resave(const std::string& bytes, const CodecTable& codecs) {
  Registry registry;
  LoadContext ctx("mem://doc.arbc");
  RecordingSink sink;
  Model loaded;
  REQUIRE(arbc::load_document(bytes, registry, codecs, ctx, sink.as_sink(), loaded));

  const auto pin = loaded.current();
  const ContentBodyProvider provider = [&sink](ObjectId id) -> std::optional<ContentBody> {
    const auto it = sink.by_id.find(id);
    if (it == sink.by_id.end()) {
      return std::nullopt;
    }
    // An unknown kind resolves to a PlaceholderContent, whose stored body wins outright --
    // the advisory kind/version here are ignored (codec.cpp).
    return ContentBody{kind_of(*it->second), "1.0", *it->second};
  };
  const ContentMetaProvider meta = [&sink](const Content& c) -> std::optional<ContentMeta> {
    const std::string_view kind = kind_of(c);
    if (kind.empty()) {
      return std::nullopt;
    }
    for (const auto& [id, live] : sink.by_id) {
      if (live == &c) {
        return ContentMeta{kind, "1.0", id};
      }
    }
    return ContentMeta{kind, "1.0", ObjectId{}};
  };
  return canonical(arbc::serialize_document(*pin, provider, meta, codecs, save_ctx));
}

} // namespace

// enforces: 08-serialization#child-compositions-round-trip-in-document
TEST_CASE("a two-composition document round-trips byte-exact through the compositions table") {
  // A root composition whose only layer holds a NESTING content, and the child composition
  // it names holding a clip. The child lands in `compositions` under the ordinal `"1"`; the
  // nesting body names it through the core-owned `"composition"` field beside
  // `kind`/`params` -- never inside `params`, which the codec deliberately leaves empty.
  Scene scene;
  ObjectId root_comp;
  ObjectId child_comp;
  ObjectId nest_id;
  ObjectId clip_id;
  {
    auto txn = scene.model.transact("built");
    root_comp = txn.add_composition(16.0, 16.0); // created FIRST -> the lowest id -> the root
    child_comp = txn.add_composition(8.0, 8.0);
    nest_id = txn.add_content(1);
    clip_id = txn.add_content(2);
    txn.attach_layer(root_comp, txn.add_layer(nest_id, arbc::Affine::identity(), 1.0));
    txn.attach_layer(child_comp, txn.add_layer(clip_id, arbc::Affine::identity(), 1.0));
    REQUIRE(txn.commit());
  }
  Content* const nest = scene.own(std::make_unique<NestKind>(child_comp));
  Content* const clip = scene.own(std::make_unique<ClipContent>("child.png"));
  scene.bound.emplace(nest_id, nest);
  scene.bound.emplace(clip_id, clip);
  scene.ids.emplace(nest, nest_id);
  scene.ids.emplace(clip, clip_id);

  const CodecTable codecs = nest_table();
  const std::string out = canonical(arbc::serialize_document(
      *scene.model.current(), scene.provider(), scene.meta(), codecs, save_ctx));

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
        "kind_version": "1.0",
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
          "kind": "com.test.clip",
          "kind_version": "1.0",
          "opacity": 1.0,
          "params": {
            "src": "child.png"
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
  CHECK(out == std::string(k_golden));

  // serialize(load(x)) == x, and re-serialization is idempotent.
  const std::string again = reload_and_resave(out, codecs);
  CHECK(again == out);
  CHECK(reload_and_resave(again, codecs) == again);

  // The loaded child composition really is a live composition holding the clip, and the
  // nesting content's `composition_ref()` names it.
  Registry registry;
  LoadContext ctx("mem://doc.arbc");
  RecordingSink sink;
  Model loaded;
  REQUIRE(arbc::load_document(out, registry, codecs, ctx, sink.as_sink(), loaded));
  const auto pin = loaded.current();
  NestKind* live_nest = nullptr;
  for (const auto& owned : sink.owned) {
    if (auto* nk = dynamic_cast<NestKind*>(owned.get())) {
      live_nest = nk;
    }
  }
  REQUIRE(live_nest != nullptr);
  const ObjectId child = live_nest->composition_ref();
  REQUIRE(child.valid());
  REQUIRE(pin->find_composition(child) != nullptr);
  // The root is the LOWEST-id composition, guaranteed by the reader allocating it first
  // (Constraint 8) -- so `find_first_composition` still finds the true root.
  ObjectId first;
  const CompositionRecord* rec = nullptr;
  REQUIRE(pin->find_first_composition(first, rec));
  CHECK(first.value < child.value);
}

// enforces: 08-serialization#droste-cycle-round-trips-as-data
TEST_CASE("a composition cycle round-trips as data, and is not an operator-input cycle") {
  const CodecTable codecs = nest_table();

  SECTION("a composition that embeds ITSELF names the root's reserved ordinal 0") {
    // The root is the only reachable composition, so there is no `compositions` key at all
    // -- the back-edge is spelled `"composition": "0"` (Decision 2).
    Scene scene;
    ObjectId root_comp;
    ObjectId nest_id;
    {
      auto txn = scene.model.transact("built");
      root_comp = txn.add_composition(16.0, 16.0);
      nest_id = txn.add_content(1);
      txn.attach_layer(root_comp, txn.add_layer(nest_id, arbc::Affine::identity(), 1.0));
      REQUIRE(txn.commit());
    }
    Content* const nest = scene.own(std::make_unique<NestKind>(root_comp));
    scene.bound.emplace(nest_id, nest);
    scene.ids.emplace(nest, nest_id);

    const std::string out = canonical(arbc::serialize_document(
        *scene.model.current(), scene.provider(), scene.meta(), codecs, save_ctx));
    CHECK(out.find("\"composition\": \"0\"") != std::string::npos);
    CHECK(out.find("\"compositions\"") == std::string::npos);

    // It LOADS (terminating -- the pre-allocated root id resolves the back-edge with no
    // re-entry) and re-serializes byte-exact.
    const std::string again = reload_and_resave(out, codecs);
    CHECK(again == out);

    Registry registry;
    LoadContext ctx("mem://doc.arbc");
    RecordingSink sink;
    Model loaded;
    REQUIRE_NOTHROW(arbc::load_document(out, registry, codecs, ctx, sink.as_sink(), loaded));
    const auto pin = loaded.current();
    ObjectId first;
    const CompositionRecord* rec = nullptr;
    REQUIRE(pin->find_first_composition(first, rec));
    auto* const live_nest = dynamic_cast<NestKind*>(sink.owned.at(0).get());
    REQUIRE(live_nest != nullptr);
    CHECK(live_nest->composition_ref() == first); // the self-embedding closed on the root
  }

  SECTION("A embeds B embeds A: the back-edge to the root is composition 0") {
    Scene scene;
    ObjectId comp_a;
    ObjectId comp_b;
    ObjectId nest_a; // in A, names B
    ObjectId nest_b; // in B, names A
    {
      auto txn = scene.model.transact("built");
      comp_a = txn.add_composition(16.0, 16.0);
      comp_b = txn.add_composition(8.0, 8.0);
      nest_a = txn.add_content(1);
      nest_b = txn.add_content(1);
      txn.attach_layer(comp_a, txn.add_layer(nest_a, arbc::Affine::identity(), 1.0));
      txn.attach_layer(comp_b, txn.add_layer(nest_b, arbc::Affine::identity(), 1.0));
      REQUIRE(txn.commit());
    }
    Content* const na = scene.own(std::make_unique<NestKind>(comp_b));
    Content* const nb = scene.own(std::make_unique<NestKind>(comp_a));
    scene.bound.emplace(nest_a, na);
    scene.bound.emplace(nest_b, nb);
    scene.ids.emplace(na, nest_a);
    scene.ids.emplace(nb, nest_b);

    const std::string out = canonical(arbc::serialize_document(
        *scene.model.current(), scene.provider(), scene.meta(), codecs, save_ctx));
    // B lands under "1"; its own nesting body names the root back as "0".
    CHECK(out.find("\"compositions\"") != std::string::npos);
    CHECK(out.find("\"composition\": \"1\"") != std::string::npos);
    CHECK(out.find("\"composition\": \"0\"") != std::string::npos);

    const std::string again = reload_and_resave(out, codecs);
    CHECK(again == out);
    CHECK(reload_and_resave(again, codecs) == again);
  }

  SECTION("a nesting content SHARED across the cycle loads: a $ref is not an input cycle") {
    // ONE nesting content is the layer root of BOTH compositions, so it is referenced twice
    // and hoists into `contents` under a single `{"$ref": "0"}` used at both sites. Its
    // `composition` names the child, whose layer names it right back through the same
    // `$ref` -- a composition cycle that CLOSES THROUGH a shared `contents` entry. It must
    // load: resolving the back-edge returns a pre-allocated id without re-entering the
    // content, so `RefResolver`'s in-progress set is never re-entered (Constraint 7).
    const char* const doc = R"json({
  "arbc": {"format": 1},
  "composition": {
    "canvas": [0, 0, 16, 16],
    "layers": [{"$ref": "0"}]
  },
  "compositions": {
    "1": {"canvas": [0, 0, 8, 8], "layers": [{"$ref": "0"}]}
  },
  "contents": {
    "0": {"kind": "com.test.nest", "kind_version": "1.0", "params": {}, "composition": "1"}
  }
})json";
    Registry registry;
    LoadContext ctx("mem://doc.arbc");
    RecordingSink sink;
    Model loaded;
    arbc::expected<std::monostate, ReaderError> result{std::monostate{}};
    REQUIRE_NOTHROW(result =
                        arbc::load_document(doc, registry, codecs, ctx, sink.as_sink(), loaded));
    REQUIRE(result); // a LEGAL composition cycle, not an UnresolvableReference

    // Exactly one nesting node was built, shared by both compositions' layers.
    CHECK(sink.owned.size() == 1);
    auto* const shared = dynamic_cast<NestKind*>(sink.owned.at(0).get());
    REQUIRE(shared != nullptr);
    const auto pin = loaded.current();
    ObjectId root;
    const CompositionRecord* rec = nullptr;
    REQUIRE(pin->find_first_composition(root, rec));
    const ObjectId child = shared->composition_ref();
    REQUIRE(pin->find_composition(child) != nullptr);
    CHECK_FALSE(child == root); // it names the CHILD; the child's layer names it back
    pin->for_each_layer_in(child, [&](ObjectId lid) {
      const arbc::LayerRecord* lr = pin->find_layer(lid);
      REQUIRE(lr != nullptr);
      CHECK(sink.by_id.at(lr->content) == static_cast<Content*>(shared));
    });
  }

  // enforces: 08-serialization#dangling-ref-is-read-error
  SECTION("an operator-INPUT cycle in the same document shape is still unresolvable") {
    // The same two-composition document, carrying BOTH cycle notions at once: the root's
    // first layer closes a legal COMPOSITION edge into `compositions["1"]`, while the shared
    // `contents` bodies close an operator-INPUT cycle through `inputs`. `$ref` graphs stay
    // acyclic (doc 08 Principle 6), so the input cycle is rejected -- and the live
    // composition edge beside it neither rescues it nor is contaminated by it. The two
    // notions keep separate state.
    //
    // Each edge set rides its OWN body: a single body carrying both is malformed outright
    // (doc 08 P7's closing rule -- a kind names its child through `composition_ref()` or
    // takes authored `inputs`, never both), which is a DIFFERENT error, pinned by
    // `#composition-and-inputs-is-read-error` below. Keeping them apart is what leaves this
    // section proving the cycle rule rather than the exclusivity rule.
    const char* const doc = R"json({
  "arbc": {"format": 1},
  "composition": {
    "canvas": [0, 0, 16, 16],
    "layers": [
      {"kind": "com.test.nest", "kind_version": "1.0", "params": {}, "composition": "1"},
      {"$ref": "0"}
    ]
  },
  "compositions": {
    "1": {"canvas": [0, 0, 8, 8], "layers": [{"$ref": "0"}]}
  },
  "contents": {
    "0": {"kind": "com.test.nest", "kind_version": "1.0", "params": {},
          "inputs": [{"$ref": "1"}]},
    "1": {"kind": "com.test.nest", "kind_version": "1.0", "params": {},
          "inputs": [{"$ref": "0"}]}
  }
})json";
    Registry registry;
    LoadContext ctx("mem://doc.arbc");
    RecordingSink sink;
    Model loaded;
    arbc::expected<std::monostate, ReaderError> result{std::monostate{}};
    REQUIRE_NOTHROW(result =
                        arbc::load_document(doc, registry, codecs, ctx, sink.as_sink(), loaded));
    REQUIRE_FALSE(result);
    CHECK(result.error().kind == ReaderError::Kind::UnresolvableReference);
    CHECK(loaded.current()->revision() == 0);
  }
}

// enforces: 08-serialization#composition-and-inputs-is-read-error
// enforces: 08-serialization#load-installs-version-0-baseline
TEST_CASE("a body carrying BOTH a composition and authored inputs is a read error") {
  // doc 08 Principle 7's closing rule (runtime.nested_codec Decision 2): a nesting content's
  // input edges are a PROJECTION of its child composition -- they ARE that child's layers --
  // so a kind names its child through `composition_ref()` or takes authored `inputs`, never
  // both. A body carrying both edge sets asserts something the format cannot express, and
  // rejecting it beats silently dropping one of the two (doc 08 says exactly that, twice, in
  // the principle this amends). The writer has never emitted such a body; only a hand-authored
  // (or hostile) file can carry one.
  const CodecTable codecs = nest_table();
  Registry registry;
  LoadContext ctx("mem://doc.arbc");

  SECTION("at a layer's content position") {
    const char* const doc = R"json({
  "arbc": {"format": 1},
  "composition": {
    "canvas": [0, 0, 16, 16],
    "layers": [
      {"kind": "com.test.nest", "kind_version": "1.0", "params": {}, "composition": "1",
       "inputs": [{"kind": "com.test.clip", "kind_version": "1.0", "params": {"src": "a.png"}}]}
    ]
  },
  "compositions": {
    "1": {"canvas": [0, 0, 8, 8], "layers": []}
  }
})json";
    RecordingSink sink;
    Model model;
    arbc::expected<std::monostate, ReaderError> result{std::monostate{}};
    REQUIRE_NOTHROW(result =
                        arbc::load_document(doc, registry, codecs, ctx, sink.as_sink(), model));
    REQUIRE_FALSE(result);
    CHECK(result.error().kind == ReaderError::Kind::MalformedField);
    CHECK(result.error().path.empty()); // the BODY's own path, as codec.cpp spells it
    // Rejected BEFORE either edge set is resolved, so no child was ever sunk and the model is
    // untouched: revision 0, no composition record (Constraint 8).
    CHECK(model.current()->revision() == 0);
    CHECK(sink.owned.empty());
    ObjectId first;
    const CompositionRecord* rec = nullptr;
    CHECK_FALSE(model.current()->find_first_composition(first, rec));
  }

  SECTION("in a standalone contents-table body") {
    // The same rule holds however the body is reached -- a `contents` entry stands alone.
    const char* const doc = R"json({
  "arbc": {"format": 1},
  "composition": {
    "canvas": [0, 0, 16, 16],
    "layers": [{"$ref": "0"}]
  },
  "compositions": {
    "1": {"canvas": [0, 0, 8, 8], "layers": []}
  },
  "contents": {
    "0": {"kind": "com.test.nest", "kind_version": "1.0", "params": {}, "composition": "1",
          "inputs": []}
  }
})json";
    RecordingSink sink;
    Model model;
    arbc::expected<std::monostate, ReaderError> result{std::monostate{}};
    REQUIRE_NOTHROW(result =
                        arbc::load_document(doc, registry, codecs, ctx, sink.as_sink(), model));
    REQUIRE_FALSE(result);
    // An EMPTY `inputs` array is still an authored `inputs` key: the rule is about the two
    // edge sets being declared at once, not about how many edges each carries. The writer
    // omits `inputs` entirely when it has none, so an empty array is never something it wrote.
    CHECK(result.error().kind == ReaderError::Kind::MalformedField);
    CHECK(model.current()->revision() == 0);
  }
}

// enforces: 08-serialization#unknown-kind-preserves-composition-reference
TEST_CASE("an unknown nesting kind keeps its child composition reachable through a save") {
  // A third-party nesting kind with NO registered codec. Its body's `composition` is
  // resolved like any other, handed to the `PlaceholderContent`, and STRIPPED from the
  // verbatim stored body -- exactly as `inputs` is, for exactly the same reason: it is
  // core-owned graph structure the writer re-derives (Decision 6). So the core still sees
  // the edge, the child composition stays reachable from the writer's walk, and a missing
  // plugin never orphans it (doc 08 Principle 2).
  const CodecTable codecs = nest_table(); // com.example.nest is NOT in it

  const char* const doc = R"json({
  "arbc": {"format": 1},
  "composition": {
    "canvas": [0, 0, 16, 16],
    "layers": [
      {"kind": "com.example.nest", "kind_version": "3.2", "params": {"blend": "over"},
       "composition": "1"}
    ]
  },
  "compositions": {
    "1": {"canvas": [0, 0, 8, 8], "layers": [
      {"kind": "com.test.clip", "kind_version": "1.0", "params": {"src": "child.png"}}
    ]}
  }
})json";

  Registry registry;
  LoadContext ctx("mem://doc.arbc");
  RecordingSink sink;
  Model loaded;
  REQUIRE(arbc::load_document(doc, registry, codecs, ctx, sink.as_sink(), loaded));

  PlaceholderContent* ghost = nullptr;
  for (const auto& owned : sink.owned) {
    if (auto* pc = dynamic_cast<PlaceholderContent*>(owned.get())) {
      ghost = pc;
    }
  }
  REQUIRE(ghost != nullptr);
  // The reference survives on the core-visible accessor...
  const ObjectId child = ghost->composition_ref();
  REQUIRE(child.valid());
  const auto pin = loaded.current();
  REQUIRE(pin->find_composition(child) != nullptr);
  // ...and is NOT left verbatim in the opaque body, where a re-derived ordinal would fight
  // it (a stale id can silently repoint at a different composition after renumbering).
  CHECK_FALSE(ghost->body().contains("composition"));
  CHECK(ghost->body().contains("params")); // the kind-owned params still round-trip verbatim

  // The whole document re-saves with the core-re-derived id, byte-exact -- with no codec
  // for `com.example.nest` in existence.
  const std::string saved = reload_and_resave(doc, codecs);
  CHECK(saved.find("\"com.example.nest\"") != std::string::npos);
  CHECK(saved.find("\"composition\": \"1\"") != std::string::npos);
  CHECK(saved.find("\"blend\": \"over\"") != std::string::npos);
  CHECK(saved.find("\"src\": \"child.png\"") != std::string::npos); // the child SURVIVED
  CHECK(reload_and_resave(saved, codecs) == saved);
}

// enforces: 08-serialization#dangling-composition-is-read-error
// enforces: 08-serialization#load-installs-version-0-baseline
TEST_CASE("the composition-table read errors are values, with the model unmutated") {
  const CodecTable codecs = nest_table();
  Registry registry;
  LoadContext ctx("mem://doc.arbc");

  SECTION("a composition id absent from the table is UnresolvableReference") {
    const char* const doc = R"json({
  "arbc": {"format": 1},
  "composition": {
    "canvas": [0, 0, 16, 16],
    "layers": [
      {"kind": "com.test.nest", "kind_version": "1.0", "params": {}, "composition": "7"}
    ]
  }
})json";
    RecordingSink sink;
    Model model;
    arbc::expected<std::monostate, ReaderError> result{std::monostate{}};
    REQUIRE_NOTHROW(result =
                        arbc::load_document(doc, registry, codecs, ctx, sink.as_sink(), model));
    REQUIRE_FALSE(result);
    CHECK(result.error().kind == ReaderError::Kind::UnresolvableReference);
    CHECK(result.error().path == "/compositions/7");
    // Pre-allocating composition ids bumps a monotonic counter and installs NO record, so
    // the model is untouched: revision 0, no composition, no id present (Constraint 5).
    const auto pin = model.current();
    CHECK(pin->revision() == 0);
    ObjectId first;
    const CompositionRecord* rec = nullptr;
    CHECK_FALSE(pin->find_first_composition(first, rec));
    CHECK_FALSE(pin->contains(ObjectId{1}));
  }

  SECTION("a compositions entry keyed 0 claims the root's reserved ordinal: MalformedField") {
    const char* const doc = R"json({
  "arbc": {"format": 1},
  "composition": {
    "canvas": [0, 0, 16, 16],
    "layers": []
  },
  "compositions": {
    "0": {"canvas": [0, 0, 8, 8], "layers": []}
  }
})json";
    RecordingSink sink;
    Model model;
    arbc::expected<std::monostate, ReaderError> result{std::monostate{}};
    REQUIRE_NOTHROW(result =
                        arbc::load_document(doc, registry, codecs, ctx, sink.as_sink(), model));
    REQUIRE_FALSE(result);
    CHECK(result.error().kind == ReaderError::Kind::MalformedField);
    CHECK(result.error().path == "/compositions/0");
    CHECK(model.current()->revision() == 0);
    ObjectId first;
    const CompositionRecord* rec = nullptr;
    CHECK_FALSE(model.current()->find_first_composition(first, rec));
  }

  SECTION("a non-object compositions, a non-string composition, a non-object entry") {
    struct Case {
      const char* doc;
      const char* path;
    };
    const Case cases[] = {
        {R"json({"arbc":{"format":1},"composition":{"canvas":[0,0,4,4],"layers":[]},
                 "compositions": []})json",
         "/compositions"},
        {R"json({"arbc":{"format":1},"composition":{"canvas":[0,0,4,4],"layers":[
                   {"kind":"com.test.nest","kind_version":"1.0","params":{},"composition":7}]}})json",
         "/composition"},
        {R"json({"arbc":{"format":1},"composition":{"canvas":[0,0,4,4],"layers":[
                   {"kind":"com.test.nest","kind_version":"1.0","params":{},"composition":"1"}]},
                 "compositions": {"1": 42}})json",
         "/compositions/1"},
    };
    for (const Case& c : cases) {
      RecordingSink sink;
      Model model;
      arbc::expected<std::monostate, ReaderError> result{std::monostate{}};
      REQUIRE_NOTHROW(result =
                          arbc::load_document(c.doc, registry, codecs, ctx, sink.as_sink(), model));
      REQUIRE_FALSE(result);
      CHECK(result.error().kind == ReaderError::Kind::MalformedField);
      CHECK(result.error().path == std::string(c.path));
      CHECK(model.current()->revision() == 0);
    }
  }
}
