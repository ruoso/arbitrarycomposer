// serialize.reader component unit tests: the LoadContext resolution + dedup
// choke point and its async asset-loading hook, plus the errors-as-values
// boundary (no nlohmann exception escapes on malformed / missing-field /
// non-object input, and the target Model stays unmutated). The byte-exact
// load->save round-trip, the version-0 baseline, and the unknown-format-major
// rejection are cross-component (drive the model + writer) and live in tests/.
//
// `load_composition` (runtime.nested_external_ref) is unit-tested here too: it is a
// serialize-owned read seam, and its two promises -- install under a caller-SEEDED root id,
// leave the model root alone -- are model-level facts a component test can pin. This TU
// therefore names `CodecTable`, and through it nlohmann::json (PRIVATE to arbc_serialize),
// so the component-test binary links nlohmann explicitly (src/serialize/CMakeLists.txt), the
// same idiom src/runtime/CMakeLists.txt already uses.

#include <arbc/contract/registry.hpp>
#include <arbc/model/model.hpp>
#include <arbc/serialize/codec.hpp> // CodecTable (internal; names nlohmann::json)
#include <arbc/serialize/load_context.hpp>
#include <arbc/serialize/reader.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace {

// enforces: 08-serialization#loadcontext-dedups-by-resolved-identity
TEST_CASE("LoadContext resolves relative references and dedups by resolved identity") {
  arbc::LoadContext ctx("proj/scene.arbc");

  SECTION("the same relative reference resolves to one shared identity") {
    const arbc::ResolvedRef a = ctx.resolve("assets/logo.png");
    const arbc::ResolvedRef b = ctx.resolve("assets/logo.png");
    CHECK(a == b);                                        // one identity ...
    CHECK(ctx.resolved_count() == 1);                     // ... one cache entry
    CHECK(ctx.resolved_uri(a) == "proj/assets/logo.png"); // joined onto the base dir

    // A distinct reference resolves to a distinct identity + a new cache entry.
    const arbc::ResolvedRef c = ctx.resolve("assets/other.png");
    CHECK_FALSE(a == c);
    CHECK(ctx.resolved_count() == 2);
    CHECK(ctx.resolved_uri(c) == "proj/assets/other.png");
  }

  SECTION("resolved identity is an identity, not a spelling") {
    // Dedup is by RESOLVED identity (doc 08 Principle 3), so two spellings of one file must
    // collapse to one `ResolvedRef` -- otherwise the external-composition loader would fetch
    // the same `.arbc` twice and install it as two child compositions with two cold tile
    // caches, which is exactly the doc-05 shared-content semantics persistence must preserve.
    const arbc::ResolvedRef plain = ctx.resolve("assets/logo.png");
    CHECK(ctx.resolve("./assets/logo.png") == plain);
    CHECK(ctx.resolve("assets/./logo.png") == plain);
    CHECK(ctx.resolve("assets//logo.png") == plain);
    CHECK(ctx.resolve("assets/sub/../logo.png") == plain);
    CHECK(ctx.resolved_count() == 1);

    // A reference that climbs out of the base directory really does climb out: `..` pops
    // `proj/`, landing on a DIFFERENT file and so a different identity -- never silently
    // clamped back onto the same one.
    const arbc::ResolvedRef up = ctx.resolve("../assets/logo.png");
    CHECK_FALSE(up == plain);
    CHECK(ctx.resolved_uri(up) == "assets/logo.png");
    // And a `..` with nothing left to pop is KEPT, rather than being silently dropped into
    // the base directory -- an honest identity for a reference outside the project tree.
    CHECK(ctx.resolved_uri(ctx.resolve("../../x.png")) == "../x.png");
  }

  SECTION("a schemed or absolute reference is taken verbatim (scheme hook stub)") {
    CHECK(ctx.resolved_uri(ctx.resolve("https://cdn.example/x.png")) ==
          "https://cdn.example/x.png");
    CHECK(ctx.resolved_uri(ctx.resolve("/abs/y.png")) == "/abs/y.png");
  }

  CHECK(ctx.base_uri() == "proj/scene.arbc");
}

// A fake async asset-loading hook: records the resolved URI it was handed and
// fires the continuation synchronously, so the LoadContext forwarding seam is
// exercised without real I/O.
class RecordingAssetSource final : public arbc::AssetSource {
public:
  void request(std::string_view resolved_uri,
               std::function<void(std::string_view)> on_ready) override {
    d_last_uri = std::string(resolved_uri);
    ++d_requests;
    on_ready("BYTES");
  }
  std::string d_last_uri;
  int d_requests{0};
};

TEST_CASE(
    "LoadContext.load_asset forwards to the installed AssetSource, else reports unavailable") {
  arbc::LoadContext ctx("proj/scene.arbc");
  const arbc::ResolvedRef ref = ctx.resolve("assets/logo.png");

  SECTION("no source installed: the continuation fires with empty bytes") {
    CHECK(ctx.asset_source() == nullptr);
    bool called = false;
    std::string got = "unset";
    ctx.load_asset(ref, [&](std::string_view bytes) {
      called = true;
      got = std::string(bytes);
    });
    CHECK(called);
    CHECK(got.empty());
  }

  SECTION("source installed: load_asset forwards the resolved URI through it") {
    RecordingAssetSource source;
    ctx.set_asset_source(&source);
    CHECK(ctx.asset_source() == &source);

    std::string got;
    ctx.load_asset(ref, [&](std::string_view bytes) { got = std::string(bytes); });
    CHECK(source.d_requests == 1);
    CHECK(source.d_last_uri == "proj/assets/logo.png");
    CHECK(got == "BYTES");
  }
}

TEST_CASE("load_document surfaces malformed input as a value with no document mutation") {
  // Errors as values (Constraint 3, doc 10:15-17): malformed JSON, a missing
  // required field, and a non-object envelope each return a distinct ReaderError
  // Kind, with no nlohmann exception thrown and the target Model unmutated.
  arbc::Model model;
  arbc::Registry registry;
  arbc::LoadContext ctx("mem://doc.arbc");

  auto unchanged = [&] {
    // The empty fresh document is still installed at revision 0.
    const auto pin = model.current();
    arbc::ObjectId comp;
    const arbc::CompositionRecord* rec = nullptr;
    return pin->revision() == 0 && !pin->find_first_composition(comp, rec);
  };

  SECTION("unparseable JSON -> MalformedJson") {
    const auto r = arbc::load_document("{ this is not json", registry, ctx, model);
    REQUIRE_FALSE(r);
    CHECK(r.error().kind == arbc::ReaderError::Kind::MalformedJson);
    CHECK(unchanged());
  }

  SECTION("a non-object top-level value -> MalformedField") {
    const auto r = arbc::load_document("[1, 2, 3]", registry, ctx, model);
    REQUIRE_FALSE(r);
    CHECK(r.error().kind == arbc::ReaderError::Kind::MalformedField);
    CHECK(unchanged());
  }

  SECTION("a missing required field (no arbc envelope) -> MissingRequiredField") {
    const auto r = arbc::load_document("{}", registry, ctx, model);
    REQUIRE_FALSE(r);
    CHECK(r.error().kind == arbc::ReaderError::Kind::MissingRequiredField);
    CHECK(unchanged());
  }

  SECTION("a missing format major inside the envelope -> MissingRequiredField") {
    const auto r = arbc::load_document(R"({"arbc":{}})", registry, ctx, model);
    REQUIRE_FALSE(r);
    CHECK(r.error().kind == arbc::ReaderError::Kind::MissingRequiredField);
    CHECK(unchanged());
  }

  SECTION("a mistyped format major -> MalformedField") {
    const auto r = arbc::load_document(R"({"arbc":{"format":"one"}})", registry, ctx, model);
    REQUIRE_FALSE(r);
    CHECK(r.error().kind == arbc::ReaderError::Kind::MalformedField);
    CHECK(unchanged());
  }

  SECTION("a non-object composition -> MalformedField") {
    const auto r =
        arbc::load_document(R"({"arbc":{"format":1},"composition":7})", registry, ctx, model);
    REQUIRE_FALSE(r);
    CHECK(r.error().kind == arbc::ReaderError::Kind::MalformedField);
    CHECK(unchanged());
  }
}

// A placement-only child document: two layers, no content bodies -- so it loads through the
// empty codec table and null sink below exactly as the content-free path does.
constexpr const char* k_child = R"({
  "arbc": {"format": 1},
  "composition": {
    "canvas": [0, 0, 8, 8],
    "layers": [{"opacity": 0.5}, {"opacity": 0.25}]
  }
})";

// enforces: 08-serialization#loadcontext-dedups-by-resolved-identity
TEST_CASE("load_composition installs a child subtree under a caller-seeded root id") {
  // The read seam `runtime.nested_external_ref` factored out of `load_document`: the same
  // graph install, into an EXISTING model, under an id the caller pre-allocated -- which is
  // the allocate-before-parse knot-cut that makes a cross-document cycle terminate -- and
  // WITHOUT disturbing the model root.
  arbc::Model model;
  const arbc::Registry registry;
  const arbc::CodecTable codecs; // no kinds: the placement-only path
  const arbc::ContentSink sink{};

  // A host document already at revision 0 with its own (lowest-id) root composition.
  const arbc::ObjectId host_root = model.allocate_id();
  REQUIRE(model.load_baseline(
      [&](arbc::Model::Transaction& txn) { txn.add_composition(host_root, 16.0, 16.0); }));

  const arbc::ObjectId child_root = model.allocate_id();
  arbc::LoadContext child_ctx("proj/child.arbc");
  const arbc::expected<arbc::ObjectId, arbc::ReaderError> installed =
      arbc::load_composition(k_child, registry, codecs, child_ctx, sink, model, child_root);
  REQUIRE(installed);

  // The child landed under exactly the id the caller seeded -- the id its embedding
  // NestedContent already holds, recorded in the loader's map before these bytes were read.
  CHECK(*installed == child_root);
  const arbc::DocStatePtr pin = model.current();
  const arbc::CompositionRecord* child = pin->find_composition(child_root);
  REQUIRE(child != nullptr);
  CHECK(child->canvas_w == 8.0);
  int child_layers = 0;
  pin->for_each_layer_in(child_root, [&](arbc::ObjectId) { ++child_layers; });
  CHECK(child_layers == 2);

  // ... and the model ROOT is untouched: `find_first_composition` names the lowest-id
  // composition, and the host's was allocated before the child's (Decision 1).
  arbc::ObjectId root;
  const arbc::CompositionRecord* rec = nullptr;
  REQUIRE(pin->find_first_composition(root, rec));
  CHECK(root == host_root);
  CHECK(rec->canvas_w == 16.0);
}

TEST_CASE("load_composition leaves the model unmutated on malformed child bytes") {
  arbc::Model model;
  const arbc::Registry registry;
  const arbc::CodecTable codecs;
  const arbc::ContentSink sink{};

  const arbc::ObjectId host_root = model.allocate_id();
  REQUIRE(model.load_baseline(
      [&](arbc::Model::Transaction& txn) { txn.add_composition(host_root, 16.0, 16.0); }));
  const std::uint64_t before = model.current()->revision();

  auto rejects = [&](std::string_view bytes) {
    const arbc::ObjectId child_root = model.allocate_id();
    arbc::LoadContext ctx("proj/child.arbc");
    const auto r = arbc::load_composition(bytes, registry, codecs, ctx, sink, model, child_root);
    // Malformed child bytes are a VALUE (the caller turns it into "unavailable" -- a null
    // child and a preserved `ref` -- rather than failing the parent load), and nothing is
    // installed: the seeded id names no composition and the host document is untouched.
    const arbc::DocStatePtr pin = model.current();
    return !r.has_value() && pin->find_composition(child_root) == nullptr &&
           pin->find_composition(host_root) != nullptr && pin->revision() == before;
  };

  CHECK(rejects("{ this is not json"));
  CHECK(rejects("[1, 2, 3]"));
  CHECK(rejects(R"({"arbc":{"format":9}})"));
  CHECK(rejects(R"({"arbc":{"format":1},"composition":7})"));

  // Legal bytes that simply hold NO root composition: not an error, but nothing to embed --
  // so the caller gets an invalid id and reports the reference unavailable.
  const arbc::ObjectId spare = model.allocate_id();
  arbc::LoadContext ctx("proj/child.arbc");
  const auto empty =
      arbc::load_composition(R"({"arbc":{"format":1}})", registry, codecs, ctx, sink, model, spare);
  REQUIRE(empty);
  CHECK_FALSE(empty->valid());
}

} // namespace
