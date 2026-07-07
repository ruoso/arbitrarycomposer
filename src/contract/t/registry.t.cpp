// Unit coverage for the minimal contract Registry seam (doc 03 §Registry) the
// plugin entry point registers into. The end-to-end dlopen path is exercised by
// tests/imageseq_plugin_path.t.cpp; this pins the value-level API and its
// errors-as-values branches (doc 03:177-180) directly.

#include <arbc/contract/content.hpp>
#include <arbc/contract/registry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

using namespace arbc;

namespace {

ContentFactory ok_factory() {
  return [](ContentConfig) -> expected<std::unique_ptr<Content>, std::string> {
    // A registry test needs no real content; the factory shape is what matters.
    return unexpected<std::string>("not constructed in this test");
  };
}

} // namespace

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
