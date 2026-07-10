// Unit coverage for the minimal contract Registry seam (doc 03 §Registry) the
// plugin entry point registers into. The end-to-end dlopen path is exercised by
// tests/imageseq_plugin_path.t.cpp; this pins the value-level API and its
// errors-as-values branches (doc 03:177-180) directly.

#include <arbc/contract/content.hpp>
#include <arbc/contract/registry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

using namespace arbc;

namespace {

ContentFactory ok_factory() {
  return [](ContentConfig) -> expected<std::unique_ptr<Content>, std::string> {
    // A registry test needs no real content; the factory shape is what matters.
    return arbc::unexpected<std::string>("not constructed in this test");
  };
}

// The minimal concrete Content the codec-hook callability checks below hand the
// stored `serialize` (which takes `const Content&`); nothing renders it.
class StubContent final : public Content {
public:
  std::optional<Rect> bounds() const override { return std::nullopt; }
  Stability stability() const override { return Stability::Static; }
  std::optional<TimeRange> time_extent() const override { return std::nullopt; }
  std::optional<RenderResult> render(const RenderRequest&,
                                     std::shared_ptr<RenderCompletion>) override {
    return RenderResult{};
  }
};

// Named thunks (not lambdas) so the storage assertion can compare the resolved
// binder's function pointers against exactly what was registered.
bool stub_try_attach(Content& /*content*/, const OperatorBindServices& /*services*/) {
  return false;
}
void stub_detach(Content& /*content*/) noexcept {}

KindCodec stub_codec() {
  KindCodec codec;
  codec.kind_version = "3";
  codec.serialize = [](const Content&) -> expected<std::string, std::string> {
    return std::string("{\"x\":1}");
  };
  codec.deserialize =
      [](std::string_view params_text, std::span<const ContentRef> inputs,
         ObjectId /*composition*/) -> expected<std::unique_ptr<Content>, std::string> {
    // Arity is the codec's own responsibility (the operator_codecs idiom): wrong
    // arity is an error value. The one-input branch echoes the params text back
    // as an "error" so the test can observe the stored hook was invoked with the
    // text it passed -- no real content is constructed in a registry unit test.
    if (inputs.size() != 1) {
      return unexpected<std::string>("stub: expected exactly one input");
    }
    return unexpected<std::string>(std::string(params_text));
  };
  return codec;
}

} // namespace

// enforces: 03-layer-plugin-interface#registry-resolves-kind-id-to-factory-and-metadata
TEST_CASE("Registry maps reverse-DNS ids to factories and metadata") {
  Registry registry;
  REQUIRE(registry.size() == 0);
  REQUIRE(registry.factory("org.arbc.imageseq") == nullptr);
  REQUIRE(registry.metadata("org.arbc.imageseq") == nullptr);
  REQUIRE(registry.ids().empty());

  SECTION("a successful registration is looked up by id, in registration order") {
    REQUIRE(registry.add("org.arbc.imageseq", ok_factory(), KindMetadata{"Image Sequence", "1"})
                .has_value());
    REQUIRE(registry.add("org.arbc.solid", ok_factory()).has_value());
    REQUIRE(registry.size() == 2);

    const ContentFactory* factory = registry.factory("org.arbc.imageseq");
    REQUIRE(factory != nullptr);
    REQUIRE_FALSE((*factory)("anything").has_value()); // the factory is callable

    const KindMetadata* meta = registry.metadata("org.arbc.imageseq");
    REQUIRE(meta != nullptr);
    REQUIRE(meta->human_name == "Image Sequence");
    REQUIRE(meta->version == "1");

    const std::vector<std::string_view> ids = registry.ids();
    REQUIRE(ids.size() == 2);
    REQUIRE(ids[0] == "org.arbc.imageseq"); // registration order preserved
    REQUIRE(ids[1] == "org.arbc.solid");
  }

  SECTION("errors are values: an empty id is rejected") {
    const expected<std::monostate, RegistryError> result = registry.add("", ok_factory());
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == RegistryError::EmptyId);
    REQUIRE(registry.size() == 0);
  }

  SECTION("errors are values: a duplicate id is rejected, leaving the first intact") {
    REQUIRE(
        registry.add("org.arbc.imageseq", ok_factory(), KindMetadata{"first", "1"}).has_value());
    const expected<std::monostate, RegistryError> dup =
        registry.add("org.arbc.imageseq", ok_factory(), KindMetadata{"second", "2"});
    REQUIRE_FALSE(dup.has_value());
    REQUIRE(dup.error() == RegistryError::DuplicateId);
    REQUIRE(registry.size() == 1);
    REQUIRE(registry.metadata("org.arbc.imageseq")->human_name == "first");
  }
}

// enforces: 03-layer-plugin-interface#registry-carries-optional-codec-and-binder
TEST_CASE("Registry entries optionally carry a kind codec and an operator binder") {
  Registry registry;

  SECTION("an add with codec and binder stores and resolves both, atomically") {
    REQUIRE(registry
                .add("org.test.op", ok_factory(), KindMetadata{"Op", "3"}, stub_codec(),
                     KindBinder{stub_try_attach, stub_detach})
                .has_value());

    const KindCodec* codec = registry.codec("org.test.op");
    REQUIRE(codec != nullptr);
    CHECK(codec->kind_version == "3");

    // The stored hooks are live: serialize answers with the registered params
    // text over any content...
    StubContent content;
    const expected<std::string, std::string> params = codec->serialize(content);
    REQUIRE(params.has_value());
    CHECK(*params == "{\"x\":1}");

    // ...and deserialize validates its own arity as an error value (never a
    // throw), then observes exactly the params text it is handed.
    const expected<std::unique_ptr<Content>, std::string> bad_arity =
        codec->deserialize("{\"x\":1}", {}, ObjectId{});
    REQUIRE_FALSE(bad_arity.has_value());
    CHECK(bad_arity.error() == "stub: expected exactly one input");
    const ContentRef one_input[] = {&content};
    const expected<std::unique_ptr<Content>, std::string> echoed =
        codec->deserialize("{\"x\":1}", one_input, ObjectId{});
    REQUIRE_FALSE(echoed.has_value());
    CHECK(echoed.error() == "{\"x\":1}");

    const KindBinder* binder = registry.binder("org.test.op");
    REQUIRE(binder != nullptr);
    CHECK(binder->try_attach == &stub_try_attach);
    CHECK(binder->detach == &stub_detach);
  }

  SECTION("a factory-only add leaves both slots empty") {
    REQUIRE(registry.add("org.test.leaf", ok_factory()).has_value());
    CHECK(registry.factory("org.test.leaf") != nullptr);
    CHECK(registry.codec("org.test.leaf") == nullptr);
    CHECK(registry.binder("org.test.leaf") == nullptr);
    CHECK(registry.codec("org.test.absent") == nullptr);
    CHECK(registry.binder("org.test.absent") == nullptr);
  }

  SECTION("duplicate/empty-id semantics are unchanged: a rejected add decorates nothing") {
    REQUIRE(registry.add("org.test.op", ok_factory()).has_value());
    const expected<std::monostate, RegistryError> dup =
        registry.add("org.test.op", ok_factory(), KindMetadata{"late", "9"}, stub_codec(),
                     KindBinder{stub_try_attach, stub_detach});
    REQUIRE_FALSE(dup.has_value());
    REQUIRE(dup.error() == RegistryError::DuplicateId);
    // The earlier, factory-only entry is intact: no post-hoc decoration
    // (Constraint 4 -- codec/binder ride the same add as the factory or not at all).
    CHECK(registry.codec("org.test.op") == nullptr);
    CHECK(registry.binder("org.test.op") == nullptr);

    const expected<std::monostate, RegistryError> empty = registry.add(
        "", ok_factory(), KindMetadata{}, stub_codec(), KindBinder{stub_try_attach, stub_detach});
    REQUIRE_FALSE(empty.has_value());
    REQUIRE(empty.error() == RegistryError::EmptyId);
    REQUIRE(registry.size() == 1);
  }
}
