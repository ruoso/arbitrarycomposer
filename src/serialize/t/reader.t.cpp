// serialize.reader component unit tests: the LoadContext resolution + dedup
// choke point and its async asset-loading hook, plus the errors-as-values
// boundary (no nlohmann exception escapes on malformed / missing-field /
// non-object input, and the target Model stays unmutated). The byte-exact
// load->save round-trip, the version-0 baseline, and the unknown-format-major
// rejection are cross-component (drive the model + writer) and live in tests/.

#include <arbc/contract/registry.hpp>
#include <arbc/model/model.hpp>
#include <arbc/serialize/load_context.hpp>
#include <arbc/serialize/reader.hpp>

#include <catch2/catch_test_macros.hpp>

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

} // namespace
