// serialize.kind_params: the content-body codec seam, the unknown-kind
// PlaceholderContent, and the L4 read/write routing. These tests name both a
// concrete kind (a purpose-built test double) and `nlohmann::json` through the
// serialize-internal codec/placeholder headers, so they are cross-component and
// link the umbrella `arbc` plus nlohmann explicitly (nlohmann is PRIVATE to
// arbc_serialize and does not propagate). The end-to-end Document integration and
// the built-in kind codecs are runtime.document_serialize (M8, Decision 5).

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/expected.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/contract/registry.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/media/pixel_format.hpp>
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

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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

// A stand-in for a real known kind: it carries exactly what its `params` encode
// (an integer gain and a source reference), so a codec can round-trip it. Never
// rendered by these seam tests, so `render` is a trivial inline settle.
class GadgetContent final : public Content {
public:
  GadgetContent(std::int64_t gain, std::string source)
      : d_gain(gain), d_source(std::move(source)) {}

  std::int64_t gain() const { return d_gain; }
  const std::string& source() const { return d_source; }

  std::optional<arbc::Rect> bounds() const override { return std::nullopt; }
  arbc::Stability stability() const override { return arbc::Stability::Static; }
  std::optional<arbc::TimeRange> time_extent() const override { return std::nullopt; }
  std::optional<arbc::RenderResult> render(const RenderRequest& request,
                                           std::shared_ptr<arbc::RenderCompletion>) override {
    return arbc::RenderResult{request.scale, true};
  }

  static constexpr const char* kind_id = "com.test.gadget";

private:
  std::int64_t d_gain;
  std::string d_source;
};

// The gadget's read codec: `params` -> a live GadgetContent. Resolves the `source`
// reference through the LoadContext (the base-URI resolution seam, Principle 1) --
// which also witnesses that the routing threads `ctx` into the codec. A missing or
// mistyped `gain` is a MalformedField value (Constraint 5), never a throw.
arbc::DeserializeFn gadget_deserialize() {
  return
      [](const json& params, std::span<const ContentRef> /*inputs*/, arbc::ObjectId /*composition*/,
         LoadContext& ctx) -> arbc::expected<std::unique_ptr<Content>, ReaderError> {
        const auto git = params.find("gain");
        if (git == params.end() || !git->is_number_integer()) {
          return arbc::unexpected(
              ReaderError{ReaderError::Kind::MalformedField, "/params/gain", ObjectId{}});
        }
        std::string source;
        if (const auto sit = params.find("source"); sit != params.end() && sit->is_string()) {
          source = sit->get<std::string>();
          ctx.resolve(source); // base-URI resolution: the ctx-consulted witness
        }
        return std::unique_ptr<Content>(
            std::make_unique<GadgetContent>(git->get<std::int64_t>(), source));
      };
}

// The gadget's write codec: a live GadgetContent -> its `params` JSON. A content of
// the wrong dynamic type is a CodecFailed value (Constraint 5).
arbc::SerializeFn gadget_serialize() {
  return [](const Content& c) -> arbc::expected<json, SerializeError> {
    const auto* gc = dynamic_cast<const GadgetContent*>(&c);
    if (gc == nullptr) {
      return arbc::unexpected(SerializeError{SerializeError::Kind::CodecFailed, ObjectId{}});
    }
    json params = json::object();
    params["gain"] = gc->gain();
    params["source"] = gc->source();
    return params;
  };
}

CodecTable gadget_table() {
  CodecTable codecs;
  codecs.add(GadgetContent::kind_id, Codec{gadget_serialize(), gadget_deserialize()});
  return codecs;
}

// A dummy factory so a kind's *plugin* registers in the Registry (the presence the
// placeholder's kind_registered() witness reads); never invoked by these tests.
arbc::ContentFactory dummy_factory() {
  return [](arbc::ContentConfig) -> arbc::expected<std::unique_ptr<Content>, std::string> {
    return arbc::unexpected<std::string>("unused");
  };
}

std::string canonical_dump(const json& j) {
  return j.dump(2, ' ', false, json::error_handler_t::replace);
}

} // namespace

// enforces: 08-serialization#unknown-kind-round-trips-verbatim
TEST_CASE("an unknown kind round-trips verbatim and byte-equivalent under canonical form") {
  // A file using a plugin the host lacks must never destroy data (doc 08 Principle
  // 2): its LEAF content body deserializes to a PlaceholderContent preserving kind /
  // kind_version / params (and unknown fields, Principle 4) verbatim, and re-serializes
  // to the CANONICAL form of the input byte-for-byte (Principle 5). The `inputs` limb
  // is graph-structural now (serialize.sharing) -- re-derived on save from `inputs()`
  // with canonical `$ref` ids, NOT stored in the opaque body -- so its document-level
  // round-trip is proved by serialize_sharing.t.cpp, not this per-node case.
  const CodecTable codecs; // empty: the kind is unknown
  Registry registry;
  LoadContext ctx("mem://doc.arbc");

  // Deliberately unsorted keys + an unknown extra field, to prove canonicalization
  // sorts keys and preserves the whole leaf body, not just the three named ones.
  const json body = json::parse(R"json({
    "params": {"scale": 2.5, "nested": {"b": true, "a": [1, 2, 3]}},
    "kind": "com.example.gadget",
    "future_field": "kept",
    "kind_version": "3.0"
  })json");

  auto produced = arbc::content_body_from_json(body, {}, arbc::ObjectId{}, codecs, registry, ctx);
  REQUIRE(produced);
  std::unique_ptr<Content> content = std::move(*produced);
  auto* placeholder = dynamic_cast<PlaceholderContent*>(content.get());
  REQUIRE(placeholder != nullptr);
  CHECK_FALSE(placeholder->kind_registered()); // no plugin registered at all

  auto out = arbc::content_body_to_json("com.example.gadget", "3.0", *content, codecs);
  REQUIRE(out);
  // Byte-for-byte equal to the canonical form of the input -- sorted keys, canonical
  // numbers, every field (including future_field) preserved.
  CHECK(canonical_dump(*out) == canonical_dump(body));
}

// enforces: 08-serialization#known-kind-params-round-trip
TEST_CASE("a known kind round-trips its params through a registered codec, not the placeholder") {
  const CodecTable codecs = gadget_table();
  Registry registry;
  LoadContext ctx("mem://doc.arbc");

  const json body = json::parse(
      R"json({"kind":"com.test.gadget","kind_version":"1.0","params":{"gain":5,"source":"a.png"}})json");

  auto produced = arbc::content_body_from_json(body, {}, arbc::ObjectId{}, codecs, registry, ctx);
  REQUIRE(produced);
  std::unique_ptr<Content> content = std::move(*produced);
  // Dispatch by kind id selected the codec, not the placeholder.
  CHECK(dynamic_cast<PlaceholderContent*>(content.get()) == nullptr);
  CHECK(dynamic_cast<GadgetContent*>(content.get()) != nullptr);
  CHECK(ctx.resolved_count() == 1); // the codec resolved "a.png" through ctx

  auto out = arbc::content_body_to_json("com.test.gadget", "1.0", *content, codecs);
  REQUIRE(out);
  CHECK(canonical_dump(*out) == canonical_dump(body)); // params round-trip byte-equivalent
}

// enforces: 08-serialization#placeholder-renders-input-0-passthrough
TEST_CASE("a placeholder renders input 0 as pass-through, else a bounded diagnostic fill") {
  const json body =
      json::parse(R"json({"kind":"com.example.gadget","kind_version":"1.0","params":{}})json");

  arbc::CpuBackend backend;
  const auto target = backend.make_surface(4, 4, arbc::k_working_rgba32f);
  REQUIRE(target);
  arbc::Surface& surface = **target;
  const RenderRequest request{arbc::Rect::from_size(4.0, 4.0),
                              1.0,
                              arbc::Time{0},
                              arbc::StateHandle{},
                              surface,
                              arbc::Exactness::BestEffort,
                              arbc::Deadline::none()};

  SECTION("no bound input: a bounded diagnostic fill, identity is nullopt, never a fault") {
    PlaceholderContent placeholder(body);
    CHECK_FALSE(placeholder.identity(request).has_value());

    auto done = std::make_shared<arbc::RenderCompletion>();
    const auto result = placeholder.render(request, done);
    REQUIRE(result.has_value()); // settled inline, no fault

    // The requested region carries the premultiplied diagnostic color (doc 07).
    const std::span<float> px = surface.span<arbc::PixelFormat::Rgba32fLinearPremul>();
    REQUIRE(px.size() >= 4);
    CHECK(px[0] == arbc::k_placeholder_diagnostic[0]);
    CHECK(px[1] == arbc::k_placeholder_diagnostic[1]);
    CHECK(px[2] == arbc::k_placeholder_diagnostic[2]);
    CHECK(px[3] == arbc::k_placeholder_diagnostic[3]);
  }

  SECTION("one bound input: identity() == 0 so the compositor serves input 0 unchanged") {
    arbc::SolidContent input_content(arbc::Rgba{1.0F, 1.0F, 1.0F, 1.0F});
    std::vector<arbc::ContentRef> inputs{&input_content};
    PlaceholderContent placeholder(body, /*kind_registered=*/false, inputs);

    REQUIRE(placeholder.inputs().size() == 1);
    const auto id = placeholder.identity(request);
    REQUIRE(id.has_value());
    CHECK(*id == 0); // a missing fade plugin degrades to an unfaded clip, not a hole
  }
}

TEST_CASE("load_document routes each layer's content body into the model through the sink") {
  // Reader routing (Acceptance): a document whose layers carry content bodies -- one
  // known test kind, two unknown (one whose plugin is registered but has no codec,
  // one missing entirely). Each layer binds a non-ObjectId{} content through a stub
  // sink; registry and ctx are consulted (no longer discarded).
  const char* const doc = R"json({
  "arbc": {"format": 1},
  "composition": {
    "canvas": [0, 0, 16, 16],
    "layers": [
      {"transform": [1, 0, 0, 1, 0, 0], "opacity": 1.0, "visible": true,
       "kind": "com.test.gadget", "kind_version": "1.0",
       "params": {"gain": 5, "source": "a.png"}},
      {"kind": "com.plugin.present", "kind_version": "2.0", "params": {"x": 1}},
      {"kind": "com.plugin.missing", "kind_version": "1.0", "params": {}}
    ]
  }
})json";

  const CodecTable codecs = gadget_table();
  Registry registry;
  REQUIRE(
      registry.add("com.plugin.present", dummy_factory(), arbc::KindMetadata{"Present", "2.0"}));
  LoadContext ctx("proj/scene.arbc");

  std::vector<std::unique_ptr<Content>> captured;
  std::uint64_t next_id = 100;
  const ContentSink sink = [&](std::unique_ptr<Content> c) -> SunkContent {
    Content* live = c.get();
    captured.push_back(std::move(c));
    return SunkContent{ObjectId{next_id++}, live};
  };

  Model model;
  const auto loaded = arbc::load_document(doc, registry, codecs, ctx, sink, model);
  REQUIRE(loaded);

  // One reconstructed content per content-bearing layer, in bottom-to-top order.
  REQUIRE(captured.size() == 3);
  CHECK(dynamic_cast<GadgetContent*>(captured[0].get()) != nullptr); // known -> codec built
  auto* present = dynamic_cast<PlaceholderContent*>(captured[1].get());
  auto* missing = dynamic_cast<PlaceholderContent*>(captured[2].get());
  REQUIRE(present != nullptr); // unknown-to-codec -> placeholder
  REQUIRE(missing != nullptr);
  CHECK(present->kind_registered()); // registry consulted: plugin present, no codec
  CHECK_FALSE(missing->kind_registered());
  CHECK(ctx.resolved_count() == 1); // ctx consulted: the gadget codec resolved "a.png"

  // Every layer binds a non-ObjectId{} content (the sink's returned ids).
  const auto pin = model.current();
  ObjectId comp_id;
  const arbc::CompositionRecord* comp = nullptr;
  REQUIRE(pin->find_first_composition(comp_id, comp));
  int layer_count = 0;
  pin->for_each_layer_in(comp_id, [&](ObjectId lid) {
    const arbc::LayerRecord* lr = pin->find_layer(lid);
    REQUIRE(lr != nullptr);
    CHECK(lr->content.valid()); // no longer the invalid ObjectId{} placeholder
    ++layer_count;
  });
  CHECK(layer_count == 3);
}

TEST_CASE("serialize_document emits the content body through the provider, unchanged without one") {
  // Writer routing (Acceptance): the provider overload emits {kind, kind_version,
  // params} beside placement in canonical order; the no-provider overload stays
  // content-body-free (Constraint 6).
  Model model;
  ObjectId c_known;
  ObjectId c_placeholder;
  {
    auto txn = model.transact("built");
    const ObjectId comp = txn.add_composition(16.0, 16.0);
    c_known = txn.add_content(1);
    c_placeholder = txn.add_content(2);
    const ObjectId l0 = txn.add_layer(c_known, arbc::Affine::identity(), 1.0);
    const ObjectId l1 = txn.add_layer(c_placeholder, arbc::Affine::identity(), 1.0);
    txn.attach_layer(comp, l0);
    txn.attach_layer(comp, l1);
    REQUIRE(txn.commit());
  }
  const auto pin = model.current();

  GadgetContent known(7, "x.png");
  const json ghost_body =
      json::parse(R"json({"kind":"com.example.ghost","kind_version":"9.9","params":{"z":1}})json");
  PlaceholderContent ghost(ghost_body);

  const CodecTable codecs = gadget_table();
  const ContentBodyProvider provider = [&](ObjectId id) -> std::optional<ContentBody> {
    if (id == c_known) {
      return ContentBody{"com.test.gadget", "1.0", known};
    }
    if (id == c_placeholder) {
      return ContentBody{"com.example.ghost", "9.9", ghost};
    }
    return std::nullopt;
  };
  // Leaf contents: neither has inputs, so the meta provider is never consulted here.
  const ContentMetaProvider meta = [&](const Content& c) -> std::optional<ContentMeta> {
    if (&c == static_cast<const Content*>(&known)) {
      return ContentMeta{"com.test.gadget", "1.0"};
    }
    return std::nullopt;
  };

  const auto out = arbc::serialize_document(*pin, provider, meta, codecs);
  REQUIRE(out);
  const std::string& s = *out;

  // Both content bodies present.
  CHECK(s.find("\"com.test.gadget\"") != std::string::npos);
  CHECK(s.find("\"com.example.ghost\"") != std::string::npos); // placeholder re-emitted verbatim
  CHECK(s.find("\"x.png\"") != std::string::npos);             // known kind's params via the codec

  // Canonical key order within the known layer: kind < kind_version < opacity <
  // params < transform < visible (ascending UTF-8 byte order).
  const auto kind_at = s.find("\"kind\":");
  const auto version_at = s.find("\"kind_version\":");
  const auto params_at = s.find("\"params\":");
  REQUIRE(kind_at != std::string::npos);
  REQUIRE(version_at != std::string::npos);
  REQUIRE(params_at != std::string::npos);
  CHECK(kind_at < version_at);
  CHECK(version_at < params_at);

  // The no-provider overload is byte-identical to today's content-body-free output.
  const auto plain = arbc::serialize_document(*pin);
  REQUIRE(plain);
  CHECK(plain->find("\"kind\"") == std::string::npos);
  CHECK(plain->find("\"params\"") == std::string::npos);
}

TEST_CASE("content-body codecs surface malformed params and missing codecs as distinct values") {
  // Errors as values (Constraint 5, doc 10): each malformed shape returns a distinct
  // ReaderError / SerializeError with no nlohmann exception and no partial mutation.
  const CodecTable codecs = gadget_table();
  Registry registry;
  LoadContext ctx("mem://doc.arbc");

  SECTION("a non-object params -> MalformedField (before any codec dispatch)") {
    const json body = json::parse(R"json({"kind":"com.test.gadget","params":7})json");
    const auto r = arbc::content_body_from_json(body, {}, arbc::ObjectId{}, codecs, registry, ctx);
    REQUIRE_FALSE(r);
    CHECK(r.error().kind == ReaderError::Kind::MalformedField);
    CHECK(r.error().path == "/params");
  }

  SECTION("params the codec rejects -> the codec's MalformedField value") {
    const json body =
        json::parse(R"json({"kind":"com.test.gadget","params":{"gain":"not-an-int"}})json");
    const auto r = arbc::content_body_from_json(body, {}, arbc::ObjectId{}, codecs, registry, ctx);
    REQUIRE_FALSE(r);
    CHECK(r.error().kind == ReaderError::Kind::MalformedField);
    CHECK(r.error().path == "/params/gain");
  }

  SECTION("a body missing kind -> MissingRequiredField") {
    const json body = json::parse(R"json({"params":{"gain":1}})json");
    const auto r = arbc::content_body_from_json(body, {}, arbc::ObjectId{}, codecs, registry, ctx);
    REQUIRE_FALSE(r);
    CHECK(r.error().kind == ReaderError::Kind::MissingRequiredField);
    CHECK(r.error().path == "/kind");
  }

  SECTION("a non-placeholder content with no registered codec -> NoCodec on write") {
    GadgetContent orphan(1, "y.png");
    const CodecTable empty;
    const auto w = arbc::content_body_to_json("com.test.gadget", "1.0", orphan, empty);
    REQUIRE_FALSE(w);
    CHECK(w.error().kind == SerializeError::Kind::NoCodec);
  }
}
