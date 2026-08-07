// runtime.registry_bootstrap end-to-end proof (docs 03/17): the L6 umbrella's
// `register_builtin_kinds(Registry&)` presents the six in-lib kinds through the
// same `Registry` surface plugins arrive by -- factory and metadata only,
// skip-on-duplicate, idempotent -- and a bootstrapped registry changes NOTHING
// about document save/load: no codec is appended, the image gate stays closed,
// and the binder-conditional registry retention never fires (bytes identical to
// the empty-registry baseline). Links the umbrella `arbc`, so the assertions
// exercise the real shipped surface.

#include <arbc/base/expected.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/builtin_kinds.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/contract/registry.hpp>
#include <arbc/kind_crossfade/crossfade_content.hpp>
#include <arbc/kind_fade/fade_content.hpp>
#include <arbc/kind_nested/nested_content.hpp>
#include <arbc/kind_raster/raster_content.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/kind_tone/tone_content.hpp>
#include <arbc/model/model.hpp>
#include <arbc/runtime/builtin_kind_versions.hpp> // k_*_kind_version (metadata source of truth)
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/document_serialize.hpp>
#include <arbc/runtime/plugin_host.hpp>
#include <arbc/serialize/codec.hpp> // CodecTable (to hold builtin_codecs(registry) by value)

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

using arbc::Affine;
using arbc::Content;
using arbc::CrossfadeContent;
using arbc::Document;
using arbc::FadeContent;
using arbc::KindBridge;
using arbc::KindMetadata;
using arbc::NestedContent;
using arbc::ObjectId;
using arbc::PluginHost;
using arbc::RasterContent;
using arbc::Rect;
using arbc::Registry;
using arbc::Rgba;
using arbc::SolidContent;
using arbc::Stability;
using arbc::ToneContent;

using Made = arbc::expected<std::unique_ptr<Content>, std::string>;

// The documented enumeration order (doc 03 § Registry, the bootstrap paragraph).
const std::vector<std::string_view> k_bootstrap_order = {
    SolidContent::kind_id, ToneContent::kind_id,      RasterContent::kind_id,
    FadeContent::kind_id,  CrossfadeContent::kind_id, NestedContent::kind_id};

Made make(const Registry& registry, std::string_view id, std::string_view config) {
  const arbc::ContentFactory* factory = registry.factory(id);
  REQUIRE(factory != nullptr);
  return (*factory)(config);
}

// The document_serialize_golden.t.cpp scene: an org.arbc.solid and an
// org.arbc.tone content on placed, attached layers of a 1920x1080 canvas.
void build_scene(Document& doc, KindBridge& bridge) {
  const ObjectId comp = doc.add_composition(1920.0, 1080.0);
  const ObjectId solid =
      doc.add_content(std::make_shared<SolidContent>(Rgba{1.0F, 0.5F, 0.25F, 1.0F}),
                      bridge.intern(SolidContent::kind_id, "1"));
  const ObjectId tone = doc.add_content(std::make_shared<ToneContent>(440U, 0.5F),
                                        bridge.intern(ToneContent::kind_id, "1"));
  doc.attach_layer(comp, doc.add_layer(solid, Affine::identity(), 1.0));
  doc.attach_layer(comp, doc.add_layer(tone, Affine::identity(), 1.0));
}

} // namespace

// enforces: 03-layer-plugin-interface#builtin-kinds-present-through-registry
TEST_CASE("register_builtin_kinds presents the six built-in kinds through one registry") {
  Registry registry;
  arbc::register_builtin_kinds(registry);

  CHECK(registry.size() == 6);
  CHECK(registry.ids() == k_bootstrap_order);

  const struct {
    const char* id;
    const char* version;
  } expected_metadata[] = {
      {SolidContent::kind_id, arbc::k_solid_kind_version},
      {ToneContent::kind_id, arbc::k_tone_kind_version},
      {RasterContent::kind_id, arbc::k_raster_kind_version},
      {FadeContent::kind_id, arbc::k_fade_kind_version},
      {CrossfadeContent::kind_id, arbc::k_crossfade_kind_version},
      {NestedContent::kind_id, arbc::k_nested_kind_version},
  };
  for (const auto& expected : expected_metadata) {
    INFO(expected.id);
    CHECK(registry.factory(expected.id) != nullptr);
    const KindMetadata* metadata = registry.metadata(expected.id);
    REQUIRE(metadata != nullptr);
    CHECK_FALSE(metadata->human_name.empty());
    // Registry metadata equals the persisted kind_version (Constraint 6).
    CHECK(metadata->version == expected.version);
  }
}

// enforces: 17-internal-components#umbrella-bootstrap-is-factory-and-metadata-only
TEST_CASE("bootstrap entries are factory-and-metadata only") {
  Registry registry;
  arbc::register_builtin_kinds(registry);

  for (const std::string_view id : registry.ids()) {
    INFO(id);
    CHECK(registry.codec(id) == nullptr);
    CHECK(registry.binder(id) == nullptr);
  }
}

TEST_CASE("config-constructible factories build content matching the config") {
  Registry registry;
  arbc::register_builtin_kinds(registry);

  SECTION("solid: premultiplied r,g,b,a; unbounded, visual-only, Static") {
    const Made made = make(registry, SolidContent::kind_id, "0.5,0.25,0.125,1");
    REQUIRE(made.has_value());
    Content& content = **made;
    const auto* solid = dynamic_cast<const SolidContent*>(&content);
    REQUIRE(solid != nullptr);
    CHECK(solid->color().r == 0.5F);
    CHECK(solid->color().g == 0.25F);
    CHECK(solid->color().b == 0.125F);
    CHECK(solid->color().a == 1.0F);
    CHECK_FALSE(content.bounds().has_value());
    CHECK(content.audio() == nullptr);
    CHECK(content.stability() == Stability::Static);
  }

  SECTION("solid: r,g,b,a,x,y,w,h names the extent SolidContent always supported") {
    // Issue #22: the four-field grammar could not express the optional extent the type
    // takes, so a solid built through the `Registry` -- the only route a host has that
    // does not bypass the factory to name the concrete type -- was ALWAYS unbounded.
    // An unbounded solid fills everywhere, which makes its layer transform a no-op:
    // placing, scaling or rotating it changed nothing on screen.
    const Made made = make(registry, SolidContent::kind_id, "0.5,0.25,0.125,1,10,20,30,40");
    REQUIRE(made.has_value());
    Content& content = **made;
    const auto* solid = dynamic_cast<const SolidContent*>(&content);
    REQUIRE(solid != nullptr);
    CHECK(solid->color().r == 0.5F); // the colour fields keep their meaning exactly
    REQUIRE(content.bounds().has_value());
    // Origin plus SIZE, not two corners: it is what an insert UI collects, and a typo
    // cannot express an inverted rect.
    CHECK(content.bounds()->x0 == 10.0);
    CHECK(content.bounds()->y0 == 20.0);
    CHECK(content.bounds()->x1 == 40.0);
    CHECK(content.bounds()->y1 == 60.0);
  }

  SECTION("solid: a malformed or empty extent is an error value, never a silent default") {
    // Wrong field count, and a non-positive size. The latter matters because an empty
    // extent renders nothing, which on screen is indistinguishable from the unbounded
    // bug this grammar exists to fix -- so it is refused rather than accepted.
    CHECK_FALSE(make(registry, SolidContent::kind_id, "1,1,1,1,0,0").has_value());
    CHECK_FALSE(make(registry, SolidContent::kind_id, "1,1,1,1,0,0,0,10").has_value());
    CHECK_FALSE(make(registry, SolidContent::kind_id, "1,1,1,1,0,0,10,-1").has_value());
    CHECK_FALSE(make(registry, SolidContent::kind_id, "1,1,1,1,0,0,x,10").has_value());
  }

  SECTION("tone: frequency_hz,amplitude; audio facet present, empty visual bounds") {
    const Made made = make(registry, ToneContent::kind_id, "440,0.5");
    REQUIRE(made.has_value());
    Content& content = **made;
    const auto* tone = dynamic_cast<const ToneContent*>(&content);
    REQUIRE(tone != nullptr);
    CHECK(tone->frequency_hz() == 440U);
    CHECK(tone->amplitude() == 0.5F);
    CHECK(content.audio() != nullptr);
    REQUIRE(content.bounds().has_value());
    CHECK(content.bounds()->empty());
  }

  SECTION("raster: WxH builds a transparent, editable raster of that extent") {
    const Made made = make(registry, RasterContent::kind_id, "8x8");
    REQUIRE(made.has_value());
    Content& content = **made;
    REQUIRE(dynamic_cast<const RasterContent*>(&content) != nullptr);
    REQUIRE(content.bounds().has_value());
    CHECK(*content.bounds() == Rect{0.0, 0.0, 8.0, 8.0});
    CHECK(content.editable() != nullptr);

    // Production extents beyond the CI dual-build's deliberately small [2, 64]
    // test range work: the grammar is shared, the range policy is not.
    const Made big = make(registry, RasterContent::kind_id, "100x70");
    REQUIRE(big.has_value());
    REQUIRE((*big)->bounds().has_value());
    CHECK(*(*big)->bounds() == Rect{0.0, 0.0, 100.0, 70.0});
  }

  SECTION("nested: decimal ObjectId; unattached empty placeholder until attach") {
    const Made made = make(registry, NestedContent::kind_id, "7");
    REQUIRE(made.has_value());
    Content& content = **made;
    const auto* nested = dynamic_cast<const NestedContent*>(&content);
    REQUIRE(nested != nullptr);
    CHECK(nested->child() == ObjectId{7});
    CHECK_FALSE(nested->attached());
    REQUIRE(content.bounds().has_value()); // the empty placeholder: present-but-empty
    CHECK(content.bounds()->empty());
  }
}

TEST_CASE("factory config failures are error values naming the kind") {
  Registry registry;
  arbc::register_builtin_kinds(registry);

  const struct {
    const char* id;
    const char* config;
  } bad_configs[] = {
      {SolidContent::kind_id, "1,2,3"},         // wrong field count
      {SolidContent::kind_id, "a,b,c,d"},       // channel not a number
      {ToneContent::kind_id, "440"},            // wrong field count
      {ToneContent::kind_id, "0,0.5"},          // frequency not positive
      {ToneContent::kind_id, "4294967296,0.5"}, // frequency beyond uint32
      {ToneContent::kind_id, "440,loud"},       // amplitude not a number
      {RasterContent::kind_id, "8"},            // no 'x' separator
      {RasterContent::kind_id, "axb"},          // extent not a number
      {RasterContent::kind_id, "0x8"},          // extent not positive
      {RasterContent::kind_id, "8589934592x8"}, // extent beyond int range
      {NestedContent::kind_id, "seven"},        // not a decimal id
      {NestedContent::kind_id, "0"},            // ObjectId zero is never valid
  };
  for (const auto& bad : bad_configs) {
    INFO(std::string(bad.id) + " <- \"" + bad.config + "\"");
    const Made made = make(registry, bad.id, bad.config);
    REQUIRE_FALSE(made.has_value());
    CHECK(made.error().find(bad.id) == 0); // the message names the kind
  }
}

TEST_CASE("fade and crossfade factories direct construction through document deserialize") {
  Registry registry;
  arbc::register_builtin_kinds(registry);

  for (const char* id : {FadeContent::kind_id, CrossfadeContent::kind_id}) {
    INFO(id);
    // Enumeration stays complete: the entry exists and its metadata answers...
    CHECK(registry.factory(id) != nullptr);
    REQUIRE(registry.metadata(id) != nullptr);
    CHECK_FALSE(registry.metadata(id)->human_name.empty());
    // ...but input edges cannot travel ContentConfig, so ANY config is refused
    // with an error value pointing at the document-deserialize path.
    const Made made = make(registry, id, "0,10");
    REQUIRE_FALSE(made.has_value());
    CHECK(made.error().find(id) == 0);
    CHECK(made.error().find("document deserialize") != std::string::npos);
  }
}

TEST_CASE("a second bootstrap is a no-op") {
  Registry registry;
  arbc::register_builtin_kinds(registry);
  arbc::register_builtin_kinds(registry);

  CHECK(registry.size() == 6);
  CHECK(registry.ids() == k_bootstrap_order);
}

TEST_CASE("a host pre-registration survives the bootstrap (skip-on-duplicate)") {
  Registry registry;
  REQUIRE(registry
              .add(
                  SolidContent::kind_id,
                  [](arbc::ContentConfig) -> Made {
                    return arbc::unexpected<std::string>("host stub wins");
                  },
                  KindMetadata{"Host Solid", "9"})
              .has_value());

  arbc::register_builtin_kinds(registry);

  CHECK(registry.size() == 6);
  CHECK(registry.ids() == k_bootstrap_order); // the stub occupied slot 0 first
  const Made made = make(registry, SolidContent::kind_id, "0.5,0.25,0.125,1");
  REQUIRE_FALSE(made.has_value());
  CHECK(made.error() == "host stub wins"); // the stub's factory is still the one returned
  REQUIRE(registry.metadata(SolidContent::kind_id) != nullptr);
  CHECK(registry.metadata(SolidContent::kind_id)->human_name == "Host Solid");
}

TEST_CASE("bootstrap and plugin loading coexist on one PluginHost registry") {
  PluginHost host;
  arbc::register_builtin_kinds(host.registry());
  REQUIRE(host.registry().size() == 6);

  REQUIRE(host.load_plugin(ARBC_CI_PLUGIN_PASSTHROUGH_FILE).has_value());
  CHECK(host.registry().size() == 7); // no DuplicateId: built-ins + plugin, one surface
  CHECK(host.registry().factory("org.arbc.ci.passthrough") != nullptr);
}

TEST_CASE("a bootstrapped registry leaves document bytes and load behavior unchanged") {
  KindBridge bridge;
  Document doc;
  build_scene(doc, bridge);

  // The empty-registry baseline...
  const arbc::CodecTable baseline_codecs = arbc::builtin_codecs();
  const arbc::expected<std::string, arbc::SerializeError> baseline =
      arbc::save_document(doc, bridge, baseline_codecs);
  REQUIRE(baseline.has_value());

  // ...is byte-identical through a bootstrapped registry: entries carry no
  // codec, so the registry-codec append finds nothing, and image (never
  // bootstrapped) keeps its gate closed.
  Registry registry;
  arbc::register_builtin_kinds(registry);
  const arbc::CodecTable bootstrap_codecs = arbc::builtin_codecs(registry);
  const arbc::expected<std::string, arbc::SerializeError> through_bootstrap =
      arbc::save_document(doc, bridge, bootstrap_codecs);
  REQUIRE(through_bootstrap.has_value());
  CHECK(*through_bootstrap == *baseline);

  // Loading through the bootstrapped registry: no entry carries a binder, so
  // the binder-conditional retention never fires and the document does not
  // hold the registry; a re-save stays byte-identical.
  KindBridge reload_bridge;
  Document reloaded;
  REQUIRE(arbc::load_document(*baseline, reloaded, reload_bridge, registry).has_value());
  CHECK(reloaded.registry() == nullptr);
  const arbc::expected<std::string, arbc::SerializeError> resaved =
      arbc::save_document(reloaded, reload_bridge, bootstrap_codecs);
  REQUIRE(resaved.has_value());
  CHECK(*resaved == *baseline);
}

// enforces: 17-internal-components#umbrella-bootstrap-is-factory-and-metadata-only
TEST_CASE("a kind's insert schema describes its config without the host knowing the grammar") {
  // Issue #21: `Registry` advertised everything needed to USE a kind and nothing
  // describing what the factory's config string MEANS, so a host offering "insert a
  // cell of kind X" had to hardcode a per-kind grammar table -- and a plugin kind could
  // be listed in the menu but not inserted with anything but a guessed config, which is
  // exactly the case the plugin seam exists to serve.
  Registry registry;
  arbc::register_builtin_kinds(registry);

  const arbc::KindInsertSchema* const schema = registry.insert_schema(SolidContent::kind_id);
  REQUIRE(schema != nullptr);

  // The host renders the advertised fields and hands the collected strings back. It
  // never learns that solid's separator is a comma, or that raster's is an 'x' -- the
  // grammar stays the kind's own.
  std::vector<std::string> values;
  values.reserve(schema->fields.size());
  for (const arbc::KindInsertField& field : schema->fields) {
    values.push_back(field.default_value);
  }
  const auto config = schema->assemble(values);
  REQUIRE(config.has_value());

  // And the config it assembled is one the kind's OWN factory accepts -- the round trip
  // that makes the schema worth anything.
  const Made made = make(registry, SolidContent::kind_id, *config);
  REQUIRE(made.has_value());
  // Defaults produce a PLACEABLE solid: an unbounded one has a no-op layer transform,
  // which is the trap issue #22 was about, so the schema must not steer a host into it.
  CHECK((*made)->bounds().has_value());

  // Raster's grammar is a different one entirely, and the same host code drives it.
  const arbc::KindInsertSchema* const raster = registry.insert_schema(RasterContent::kind_id);
  REQUIRE(raster != nullptr);
  REQUIRE(raster->fields.size() == 2);
  CHECK(raster->fields[0].type == arbc::KindInsertField::Type::Integer);
  const auto raster_config = raster->assemble(std::vector<std::string>{"64", "32"});
  REQUIRE(raster_config.has_value());
  CHECK(*raster_config == "64x32");
  CHECK(make(registry, RasterContent::kind_id, *raster_config).has_value());

  // Nested advertises one REFERENCE field (issue #33): it was the last
  // config-constructible builtin without a schema, so a host enumerating schemas with no
  // per-kind allowlist showed it as an unlabelled box holding a bare decimal ObjectId.
  const arbc::KindInsertSchema* const nested = registry.insert_schema(NestedContent::kind_id);
  REQUIRE(nested != nullptr);
  REQUIRE(nested->fields.size() == 1);
  CHECK(nested->fields[0].type == arbc::KindInsertField::Type::ObjectId);
  CHECK(nested->fields[0].min == 1.0);
  // No default: no child id is valid in every document, so a pre-filled one would name
  // some other document's object. The field must actually be collected -- and the kind's
  // own factory, not the schema, is what says so.
  CHECK(nested->fields[0].default_value.empty());
  const auto uncollected = nested->assemble(std::vector<std::string>{""});
  REQUIRE(uncollected.has_value());
  CHECK_FALSE(make(registry, NestedContent::kind_id, *uncollected).has_value());
  const auto nested_config = nested->assemble(std::vector<std::string>{"7"});
  REQUIRE(nested_config.has_value());
  CHECK(*nested_config == "7");
  CHECK(make(registry, NestedContent::kind_id, *nested_config).has_value());

  // A kind that registers none is unaffected: the host falls back to a raw config text
  // box exactly as it must today. Operator kinds refuse config construction outright,
  // so they advertise nothing to collect.
  CHECK(registry.insert_schema(arbc::FadeContent::kind_id) == nullptr);
  CHECK(registry.insert_schema("org.example.absent") == nullptr);
}
