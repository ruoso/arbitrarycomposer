// serialize.sharing: the L4 structural read/write of the operator graph -- the
// core-owned `inputs` array beside `params`, the document-level `contents` table
// with `{"$ref": id}` for shared content, and their intra-document dedup /
// dangling-and-cyclic-ref discipline on read (doc 08 Principle 6, doc 13
// §Serialization). Like serialize.kind_params these tests name concrete test kinds
// AND `nlohmann::json` through the serialize-internal codec/placeholder headers, so
// they are cross-component and link the umbrella `arbc` plus nlohmann explicitly.
// The live Document bind + the built-in operator codecs are runtime.document_serialize
// (M8, Decision 5); these tests prove the seam with purpose-built test codecs.

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/expected.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/contract/registry.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/model/model.hpp>
#include <arbc/serialize/codec.hpp>
#include <arbc/serialize/load_context.hpp>
#include <arbc/serialize/placeholder_content.hpp>
#include <arbc/serialize/reader.hpp>
#include <arbc/serialize/writer.hpp>
#include <arbc/surface/surface.hpp>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

using arbc::Codec;
using arbc::CodecTable;
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

// A leaf test kind (a clip): empty `inputs()`, so it serializes with no `inputs`
// array (omit-when-empty, doc 08 Principle 6). Its `params` carry a source token.
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

// A single-input test operator (a fade-over-clip shape, doc 13:167): it owns one
// input edge, visible to the core through `inputs()`, so it serializes with an
// `inputs` array beside its `params`.
class OpContent final : public Content {
public:
  OpContent(std::int64_t amount, std::vector<ContentRef> inputs)
      : d_amount(amount), d_inputs(std::move(inputs)) {}
  std::int64_t amount() const { return d_amount; }

  std::optional<arbc::Rect> bounds() const override { return std::nullopt; }
  arbc::Stability stability() const override { return arbc::Stability::Static; }
  std::optional<arbc::TimeRange> time_extent() const override { return std::nullopt; }
  std::optional<arbc::RenderResult> render(const RenderRequest& request,
                                           std::shared_ptr<arbc::RenderCompletion>) override {
    return arbc::RenderResult{request.scale, true};
  }
  std::span<const ContentRef> inputs() const override { return d_inputs; }

  static constexpr const char* kind_id = "com.test.op";

private:
  std::int64_t d_amount;
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
  return [](const Content& c) -> arbc::expected<json, SerializeError> {
    const auto* cc = dynamic_cast<const ClipContent*>(&c);
    if (cc == nullptr) {
      return arbc::unexpected(SerializeError{SerializeError::Kind::CodecFailed, ObjectId{}});
    }
    json params = json::object();
    params["src"] = cc->src();
    return params;
  };
}

// The operator's read codec adopts its already-reconstructed input edges at
// construction (Decision 4): the read recursion built the children first.
arbc::DeserializeFn op_deserialize() {
  return [](const json& params, std::span<const ContentRef> inputs, ObjectId /*composition*/,
            LoadContext&) -> arbc::expected<std::unique_ptr<Content>, ReaderError> {
    std::int64_t amount = 0;
    if (const auto it = params.find("amount"); it != params.end() && it->is_number_integer()) {
      amount = it->get<std::int64_t>();
    }
    return std::unique_ptr<Content>(
        std::make_unique<OpContent>(amount, std::vector<ContentRef>(inputs.begin(), inputs.end())));
  };
}

arbc::SerializeFn op_serialize() {
  return [](const Content& c) -> arbc::expected<json, SerializeError> {
    const auto* oc = dynamic_cast<const OpContent*>(&c);
    if (oc == nullptr) {
      return arbc::unexpected(SerializeError{SerializeError::Kind::CodecFailed, ObjectId{}});
    }
    json params = json::object();
    params["amount"] = oc->amount();
    return params;
  };
}

CodecTable graph_table() {
  CodecTable codecs;
  codecs.add(ClipContent::kind_id, Codec{clip_serialize(), clip_deserialize()});
  codecs.add(OpContent::kind_id, Codec{op_serialize(), op_deserialize()});
  return codecs;
}

// A stub sink that owns every reconstructed node and records `id -> live` so a test
// can prove intra-document dedup by pointer identity and re-drive the writer.
struct RecordingSink {
  std::vector<std::unique_ptr<Content>> owned;
  std::unordered_map<ObjectId, Content*> by_id;
  std::uint64_t next = 100;

  ContentSink as_sink() {
    return [this](std::unique_ptr<Content> c) -> SunkContent {
      Content* live = c.get();
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

} // namespace

// enforces: 08-serialization#inputs-array-round-trips
TEST_CASE("an inline operator graph round-trips with an inputs array beside params") {
  // A single-input operator over a leaf (a fade-over-clip shape) serializes to an
  // inline `inputs` array beside `params` in canonical key order (doc 08 Principle 6),
  // and loads back to a two-node graph whose root `inputs()` yields the child.
  Model model;
  ObjectId c_op;
  {
    auto txn = model.transact("built");
    const ObjectId comp = txn.add_composition(16.0, 16.0);
    c_op = txn.add_content(1);
    const ObjectId l0 = txn.add_layer(c_op, arbc::Affine::identity(), 1.0);
    txn.attach_layer(comp, l0);
    REQUIRE(txn.commit());
  }
  const auto pin = model.current();

  ClipContent clip("clip.png");
  OpContent op(3, std::vector<ContentRef>{&clip});
  const CodecTable codecs = graph_table();
  const ContentBodyProvider provider = [&](ObjectId id) -> std::optional<ContentBody> {
    if (id == c_op) {
      return ContentBody{OpContent::kind_id, "1.0", op};
    }
    return std::nullopt;
  };
  const ContentMetaProvider meta = [&](const Content& c) -> std::optional<ContentMeta> {
    if (&c == static_cast<const Content*>(&op)) {
      return ContentMeta{OpContent::kind_id, "1.0"};
    }
    if (&c == static_cast<const Content*>(&clip)) {
      return ContentMeta{ClipContent::kind_id, "1.0"};
    }
    return std::nullopt;
  };

  const std::string out = canonical(arbc::serialize_document(*pin, provider, meta, codecs));

  const char* const k_inline = R"json({
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
        "inputs": [
          {
            "kind": "com.test.clip",
            "kind_version": "1.0",
            "params": {
              "src": "clip.png"
            }
          }
        ],
        "kind": "com.test.op",
        "kind_version": "1.0",
        "opacity": 1.0,
        "params": {
          "amount": 3
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
  CHECK(out == std::string(k_inline));

  // Load the golden back: the root operator's single input edge is the leaf clip.
  Registry registry;
  LoadContext ctx("mem://doc.arbc");
  RecordingSink sink;
  Model loaded;
  REQUIRE(arbc::load_document(out, registry, codecs, ctx, sink.as_sink(), loaded));

  REQUIRE(sink.owned.size() == 2); // clip (child, built first) + op (root)
  auto* loaded_op = dynamic_cast<OpContent*>(sink.owned[1].get());
  auto* loaded_clip = dynamic_cast<ClipContent*>(sink.owned[0].get());
  REQUIRE(loaded_op != nullptr);
  REQUIRE(loaded_clip != nullptr);
  REQUIRE(loaded_op->inputs().size() == 1);
  CHECK(loaded_op->inputs()[0] == static_cast<Content*>(loaded_clip));
}

// enforces: 08-serialization#shared-content-dedups-via-ref
TEST_CASE("shared content is hoisted once into contents and dedups to one live node") {
  // A clip referenced from an operator input AND directly as a second layer's content
  // is shared (referenced >= 2 times), so it is emitted ONCE into a document-level
  // `contents` table and referenced by `{"$ref": id}` at both sites; the operator,
  // referenced once, stays inline. On load both sites resolve to the SAME live
  // Content* (intra-document dedup, Constraint 3).
  Model model;
  ObjectId c_op;
  ObjectId c_clip;
  {
    auto txn = model.transact("built");
    const ObjectId comp = txn.add_composition(16.0, 16.0);
    c_op = txn.add_content(1);
    c_clip = txn.add_content(2);
    const ObjectId l0 = txn.add_layer(c_op, arbc::Affine::identity(), 1.0);
    const ObjectId l1 = txn.add_layer(c_clip, arbc::Affine::identity(), 1.0);
    txn.attach_layer(comp, l0);
    txn.attach_layer(comp, l1);
    REQUIRE(txn.commit());
  }
  const auto pin = model.current();

  ClipContent clip("shared.png");
  OpContent op(3, std::vector<ContentRef>{&clip});
  const CodecTable codecs = graph_table();
  const ContentBodyProvider provider = [&](ObjectId id) -> std::optional<ContentBody> {
    if (id == c_op) {
      return ContentBody{OpContent::kind_id, "1.0", op};
    }
    if (id == c_clip) {
      return ContentBody{ClipContent::kind_id, "1.0", clip};
    }
    return std::nullopt;
  };
  const ContentMetaProvider meta = [&](const Content& c) -> std::optional<ContentMeta> {
    if (&c == static_cast<const Content*>(&op)) {
      return ContentMeta{OpContent::kind_id, "1.0"};
    }
    if (&c == static_cast<const Content*>(&clip)) {
      return ContentMeta{ClipContent::kind_id, "1.0"};
    }
    return std::nullopt;
  };

  const std::string out = canonical(arbc::serialize_document(*pin, provider, meta, codecs));

  const char* const k_shared = R"json({
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
        "inputs": [
          {
            "$ref": "0"
          }
        ],
        "kind": "com.test.op",
        "kind_version": "1.0",
        "opacity": 1.0,
        "params": {
          "amount": 3
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
        "$ref": "0",
        "opacity": 1.0,
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
      "kind": "com.test.clip",
      "kind_version": "1.0",
      "params": {
        "src": "shared.png"
      }
    }
  }
}
)json";
  CHECK(out == std::string(k_shared));

  // Load it back: exactly ONE clip is built; the operator's input and the second
  // layer's content are the same live Content* (pointer identity, not two copies).
  Registry registry;
  LoadContext ctx("mem://doc.arbc");
  RecordingSink sink;
  Model loaded;
  REQUIRE(arbc::load_document(out, registry, codecs, ctx, sink.as_sink(), loaded));

  int clips = 0;
  ClipContent* the_clip = nullptr;
  OpContent* the_op = nullptr;
  for (const auto& owned : sink.owned) {
    if (auto* cc = dynamic_cast<ClipContent*>(owned.get())) {
      ++clips;
      the_clip = cc;
    }
    if (auto* oc = dynamic_cast<OpContent*>(owned.get())) {
      the_op = oc;
    }
  }
  CHECK(clips == 1); // the shared clip was built at most once (Constraint 3)
  REQUIRE(the_op != nullptr);
  REQUIRE(the_clip != nullptr);
  REQUIRE(the_op->inputs().size() == 1);
  CHECK(the_op->inputs()[0] == static_cast<Content*>(the_clip)); // input edge == shared node

  // The second layer's bound content resolves to that very same live clip.
  const auto lpin = loaded.current();
  ObjectId comp_id;
  const arbc::CompositionRecord* comp = nullptr;
  REQUIRE(lpin->find_first_composition(comp_id, comp));
  std::vector<ObjectId> layer_content;
  lpin->for_each_layer_in(comp_id, [&](ObjectId lid) {
    const arbc::LayerRecord* lr = lpin->find_layer(lid);
    REQUIRE(lr != nullptr);
    layer_content.push_back(lr->content);
  });
  REQUIRE(layer_content.size() == 2);
  CHECK(sink.by_id.at(layer_content[1]) == static_cast<Content*>(the_clip));
}

// enforces: 08-serialization#shared-content-dedups-via-ref
TEST_CASE("content shared across TWO compositions hoists into one contents entry") {
  // serialize.compositions_table Constraint 4: the refcount pre-pass counts across the
  // WHOLE reachable graph -- every reachable composition, not one at a time -- so a clip
  // used by a layer in the root AND a layer in the child is referenced twice, hoists into
  // `contents` under a single `{"$ref": id}`, and reloads to exactly ONE live Content.
  //
  // The nesting content that makes the child reachable is an UNKNOWN kind here, so no
  // nesting codec is needed: its `PlaceholderContent` carries the child reference.
  const CodecTable codecs = graph_table(); // clip known; com.example.nest -> placeholder
  const char* const doc = R"json({
  "arbc": {"format": 1},
  "composition": {
    "canvas": [0, 0, 16, 16],
    "layers": [
      {"kind": "com.example.nest", "kind_version": "1.0", "params": {}, "composition": "1"},
      {"$ref": "0"}
    ]
  },
  "compositions": {
    "1": {"canvas": [0, 0, 8, 8], "layers": [{"$ref": "0"}]}
  },
  "contents": {
    "0": {"kind": "com.test.clip", "kind_version": "1.0", "params": {"src": "shared.png"}}
  }
})json";

  Registry registry;
  LoadContext ctx("mem://doc.arbc");
  RecordingSink sink;
  Model loaded;
  REQUIRE(arbc::load_document(doc, registry, codecs, ctx, sink.as_sink(), loaded));

  // Exactly ONE clip was built for the two use sites straddling the composition boundary.
  int clips = 0;
  ClipContent* the_clip = nullptr;
  PlaceholderContent* ghost = nullptr;
  for (const auto& owned : sink.owned) {
    if (auto* cc = dynamic_cast<ClipContent*>(owned.get())) {
      ++clips;
      the_clip = cc;
    }
    if (auto* pc = dynamic_cast<PlaceholderContent*>(owned.get())) {
      ghost = pc;
    }
  }
  CHECK(clips == 1);
  REQUIRE(the_clip != nullptr);
  REQUIRE(ghost != nullptr);

  // Both layers -- one in the root composition, one in the child -- bind that same node.
  const auto pin = loaded.current();
  ObjectId root_comp;
  const arbc::CompositionRecord* comp = nullptr;
  REQUIRE(pin->find_first_composition(root_comp, comp));
  const ObjectId child_comp = ghost->composition_ref();
  REQUIRE(pin->find_composition(child_comp) != nullptr);

  std::vector<ObjectId> root_layers;
  pin->for_each_layer_in(root_comp, [&](ObjectId lid) {
    const arbc::LayerRecord* lr = pin->find_layer(lid);
    REQUIRE(lr != nullptr);
    root_layers.push_back(lr->content);
  });
  REQUIRE(root_layers.size() == 2);
  CHECK(sink.by_id.at(root_layers[1]) == static_cast<Content*>(the_clip));
  pin->for_each_layer_in(child_comp, [&](ObjectId lid) {
    const arbc::LayerRecord* lr = pin->find_layer(lid);
    REQUIRE(lr != nullptr);
    CHECK(sink.by_id.at(lr->content) == static_cast<Content*>(the_clip));
  });

  // And it re-serializes with the clip still hoisted ONCE into `contents`, referenced by
  // `$ref` at both sites across the two compositions.
  const ContentBodyProvider provider = [&sink](ObjectId id) -> std::optional<ContentBody> {
    const auto it = sink.by_id.find(id);
    if (it == sink.by_id.end()) {
      return std::nullopt;
    }
    const bool is_clip = dynamic_cast<ClipContent*>(it->second) != nullptr;
    return ContentBody{is_clip ? ClipContent::kind_id : "com.example.nest", "1.0", *it->second};
  };
  const ContentMetaProvider meta = [the_clip](const Content& c) -> std::optional<ContentMeta> {
    if (&c == static_cast<const Content*>(the_clip)) {
      return ContentMeta{ClipContent::kind_id, "1.0"};
    }
    return std::nullopt;
  };
  const std::string out = canonical(arbc::serialize_document(*pin, provider, meta, codecs));
  CHECK(out.find("\"src\": \"shared.png\"") != std::string::npos);
  CHECK(out.find("\"contents\"") != std::string::npos);
  CHECK(out.find("\"compositions\"") != std::string::npos);
  // ONE definition of the clip, referenced by `$ref` at both use sites.
  std::size_t refs = 0;
  for (std::size_t at = out.find("\"$ref\""); at != std::string::npos;
       at = out.find("\"$ref\"", at + 1)) {
    ++refs;
  }
  CHECK(refs == 2);
}

// enforces: 08-serialization#dangling-ref-is-read-error
TEST_CASE("a dangling or cyclic $ref is an unresolvable-reference read error, atomically") {
  const CodecTable codecs = graph_table();
  Registry registry;
  LoadContext ctx("mem://doc.arbc");

  SECTION("a $ref naming an absent contents id is UnresolvableReference, model unmutated") {
    const char* const doc = R"json({
  "arbc": {"format": 1},
  "composition": {
    "canvas": [0, 0, 16, 16],
    "layers": [
      {"kind": "com.test.op", "kind_version": "1.0", "params": {"amount": 1},
       "inputs": [{"$ref": "9"}]}
    ]
  }
})json";
    RecordingSink sink;
    Model model;
    const auto loaded = arbc::load_document(doc, registry, codecs, ctx, sink.as_sink(), model);
    REQUIRE_FALSE(loaded);
    CHECK(loaded.error().kind == ReaderError::Kind::UnresolvableReference);
    CHECK(model.current()->revision() == 0); // no model mutation (Decision 7)
  }

  SECTION("a $ref closing an operator-input cycle is UnresolvableReference, model unmutated") {
    // contents "0" -> input "1" -> input "0": resolving the layer's $ref "0" re-enters
    // the in-progress id "0" and faults before any model mutation.
    const char* const doc = R"json({
  "arbc": {"format": 1},
  "composition": {
    "canvas": [0, 0, 16, 16],
    "layers": [
      {"$ref": "0"}
    ]
  },
  "contents": {
    "0": {"kind": "com.test.op", "kind_version": "1.0", "params": {"amount": 1},
          "inputs": [{"$ref": "1"}]},
    "1": {"kind": "com.test.op", "kind_version": "1.0", "params": {"amount": 2},
          "inputs": [{"$ref": "0"}]}
  }
})json";
    RecordingSink sink;
    Model model;
    const auto loaded = arbc::load_document(doc, registry, codecs, ctx, sink.as_sink(), model);
    REQUIRE_FALSE(loaded);
    CHECK(loaded.error().kind == ReaderError::Kind::UnresolvableReference);
    CHECK(model.current()->revision() == 0);
  }
}

// enforces: 08-serialization#placeholder-renders-input-0-passthrough
TEST_CASE("a placeholder pass-through wires a parsed input and re-serializes byte-equivalent") {
  // An unknown-kind body carrying one `inputs` entry (a known clip leaf) loads to a
  // PlaceholderContent whose parsed input surfaces through inputs() and drives an
  // input-0 pass-through identity() (doc 08:110-111, doc 13:158-161); its body still
  // re-serializes byte-equivalent (Constraint 5).
  Model model;
  ObjectId c_root;
  {
    auto txn = model.transact("built");
    const ObjectId comp = txn.add_composition(16.0, 16.0);
    c_root = txn.add_content(1);
    const ObjectId l0 = txn.add_layer(c_root, arbc::Affine::identity(), 1.0);
    txn.attach_layer(comp, l0);
    REQUIRE(txn.commit());
  }
  const auto pin = model.current();

  ClipContent clip("x.png");
  const json ghost_body =
      json::parse(R"json({"kind":"com.example.unknown","kind_version":"1.0","params":{}})json");
  PlaceholderContent ghost(ghost_body, /*kind_registered=*/false, std::vector<ContentRef>{&clip});

  const CodecTable codecs = graph_table(); // clip known; com.example.unknown -> placeholder
  const ContentBodyProvider provider = [&](ObjectId id) -> std::optional<ContentBody> {
    if (id == c_root) {
      return ContentBody{"com.example.unknown", "1.0", ghost};
    }
    return std::nullopt;
  };
  const ContentMetaProvider meta = [&](const Content& c) -> std::optional<ContentMeta> {
    if (&c == static_cast<const Content*>(&clip)) {
      return ContentMeta{ClipContent::kind_id, "1.0"};
    }
    return std::nullopt;
  };

  const std::string s1 = canonical(arbc::serialize_document(*pin, provider, meta, codecs));

  // Load s1: the placeholder's parsed input surfaces and drives the pass-through.
  Registry registry;
  LoadContext ctx("mem://doc.arbc");
  RecordingSink sink;
  Model loaded;
  REQUIRE(arbc::load_document(s1, registry, codecs, ctx, sink.as_sink(), loaded));

  PlaceholderContent* placeholder = nullptr;
  ClipContent* loaded_clip = nullptr;
  for (const auto& owned : sink.owned) {
    if (auto* pc = dynamic_cast<PlaceholderContent*>(owned.get())) {
      placeholder = pc;
    }
    if (auto* cc = dynamic_cast<ClipContent*>(owned.get())) {
      loaded_clip = cc;
    }
  }
  REQUIRE(placeholder != nullptr);
  REQUIRE(loaded_clip != nullptr);
  REQUIRE(placeholder->inputs().size() == 1);
  CHECK(placeholder->inputs()[0] == static_cast<Content*>(loaded_clip));

  arbc::CpuBackend backend;
  const auto target = backend.make_surface(4, 4, arbc::k_working_rgba32f);
  REQUIRE(target);
  const RenderRequest request{arbc::Rect::from_size(4.0, 4.0),
                              1.0,
                              arbc::Time{0},
                              arbc::StateHandle{},
                              **target,
                              arbc::Exactness::BestEffort,
                              arbc::Deadline::none()};
  const auto id = placeholder->identity(request);
  REQUIRE(id.has_value());
  CHECK(*id == 0); // a missing plugin degrades to an unfaded clip, not a hole

  // Re-serialize the loaded graph: byte-equivalent to s1 (Constraint 5). The layer's
  // bound content is the placeholder root; the clip is resolved as its input child.
  const auto lpin = loaded.current();
  ObjectId comp_id;
  const arbc::CompositionRecord* comp = nullptr;
  REQUIRE(lpin->find_first_composition(comp_id, comp));
  ObjectId root_id;
  lpin->for_each_layer_in(comp_id, [&](ObjectId lid) {
    const arbc::LayerRecord* lr = lpin->find_layer(lid);
    REQUIRE(lr != nullptr);
    root_id = lr->content;
  });
  const ContentBodyProvider provider2 = [&](ObjectId id2) -> std::optional<ContentBody> {
    if (id2 == root_id) {
      return ContentBody{"com.example.unknown", "1.0", *placeholder};
    }
    return std::nullopt;
  };
  const ContentMetaProvider meta2 = [&](const Content& c) -> std::optional<ContentMeta> {
    if (&c == static_cast<const Content*>(loaded_clip)) {
      return ContentMeta{ClipContent::kind_id, "1.0"};
    }
    return std::nullopt;
  };
  const std::string s2 = canonical(arbc::serialize_document(*lpin, provider2, meta2, codecs));
  CHECK(s2 == s1);
}
