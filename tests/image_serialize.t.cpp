// org.arbc.image persistence (kinds.image Decisions 2/5/7; doc 08 Principle 3).
//
// This is the half of the pixel-persistence split that STORES NOTHING. Its whole document
// footprint is a URI -- which is why a 30-layer 24 MP composition is ~32 MB instead of ~490
// MB, and why no compressor closes that gap (after content-addressed dedup, photographic
// tiles are 93% of the bytes and compress only ~2.1x; sensor noise is incompressible).
//
// Three properties, each asserted end-to-end through a real `Document` load/save:
//
//   * The emitted `params` is EXACTLY `{"source": "<authored-uri>"}`. No pixels, no tiles, no
//     intrinsic size. And the AUTHORED reference is what re-saves -- never absolutised, never
//     rewritten to the resolved URI -- so a project directory stays relocatable.
//   * An UNAVAILABLE asset is never a read error. A missing file, an undecodable one, and a
//     `LoadContext` with no `AssetSource` installed at all ALL load successfully.
//   * The codec is GATED on the plugin: without it, no codec is registered and the layer
//     round-trips verbatim as a `PlaceholderContent`, losing nothing.
//
// The plugin is reached by LINKING its impl archive and registering the factory into a
// `Registry` by hand -- exactly what `arbc_plugin_register` does across the ABI, minus the
// `dlopen`. The codec under test lives in `runtime` and names no plugin type at all: it finds
// the kind solely through that `Registry`.

#include <arbc/base/ids.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/contract/registry.hpp>
#include <arbc/kind_image/image_content.hpp>
#include <arbc/runtime/builtin_codecs.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/document_serialize.hpp>
#include <arbc/runtime/filesystem_asset_source.hpp>
#include <arbc/serialize/codec.hpp> // CodecTable / Codec (names nlohmann::json)
#include <arbc/serialize/load_context.hpp>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include "support/image_fixtures.hpp"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

using namespace arbc;
namespace fix = arbc::image::testfix;

namespace {

using json = nlohmann::json;

// A project directory under the OS temp dir, removed on scope exit. Named after the test so
// two lanes of the ctest matrix never collide.
class ProjectDir {
public:
  explicit ProjectDir(const std::string& name)
      : d_root(std::filesystem::temp_directory_path() / ("arbc_image_" + name)) {
    std::error_code ec;
    std::filesystem::remove_all(d_root, ec);
    REQUIRE(std::filesystem::create_directories(d_root / "assets", ec));
  }
  ~ProjectDir() {
    std::error_code ec;
    std::filesystem::remove_all(d_root, ec);
  }
  ProjectDir(const ProjectDir&) = delete;
  ProjectDir& operator=(const ProjectDir&) = delete;

  std::string write(const std::string& relative, std::string_view bytes) const {
    const std::filesystem::path path = d_root / relative;
    std::ofstream out(path, std::ios::binary);
    REQUIRE(out);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    REQUIRE(out);
    return path.string();
  }

  // The document's own URI: what every relative reference in it resolves against.
  std::string base_uri() const { return (d_root / "project.arbc").string(); }

private:
  std::filesystem::path d_root;
};

// The `Registry` a host has after `arbc_plugin_register` ran -- assembled here by linking the
// impl archive instead of dlopening the MODULE (`tests/image_containment.t.cpp` proves the
// decode dep still never reaches libarbc, and `plugin_loading.t.cpp` proves the dlopen path).
Registry image_registry() {
  Registry registry;
  REQUIRE(registry
              .add(arbc::image::ImageContent::kind_id,
                   [](ContentConfig config) { return arbc::image::make_image_content(config); },
                   KindMetadata{"Image", "1"})
              .has_value());
  return registry;
}

// One `org.arbc.image` layer per authored reference, in the writer's CANONICAL form (sorted
// keys, 2-space indent, trailing newline, every core-owned layer field spelled out). Canonical
// matters: these bytes are compared byte-for-byte against what a save re-emits, so anything the
// writer would normalize -- an omitted `visible`, a different key order -- would make the
// round-trip assertion about JSON formatting rather than about the image reference surviving.
std::string document_with_sources(const std::vector<std::string>& sources) {
  json layers = json::array();
  for (const std::string& source : sources) {
    layers.push_back(json{{"kind", "org.arbc.image"},
                          {"kind_version", "1"},
                          {"opacity", 1.0},
                          {"params", json{{"source", source}}},
                          {"transform", json::array({1.0, 0.0, 0.0, 1.0, 0.0, 0.0})},
                          {"visible", true}});
  }
  const json doc{{"arbc", json{{"format", 1}}},
                 {"composition",
                  json{{"canvas", json::array({0, 0, 1920, 1080})}, {"layers", std::move(layers)}}}};
  return doc.dump(2) + "\n";
}

// Reload `bytes` and re-save: the byte-for-byte identity that "the authored reference
// round-trips verbatim" actually means.
std::string reload_and_resave(const std::string& bytes, const Registry& registry,
                              const std::string& base_uri, AssetSource* assets) {
  KindBridge bridge;
  Document doc;
  const expected<std::monostate, ReaderError> loaded =
      load_document(bytes, doc, bridge, registry, base_uri, assets);
  REQUIRE(loaded.has_value()); // the parent document ALWAYS loads (Constraint 6)
  const CodecTable codecs = builtin_codecs(registry);
  const expected<std::string, SerializeError> resaved = save_document(doc, bridge, codecs);
  REQUIRE(resaved.has_value());
  return *resaved;
}

// The `params` object the writer emitted for layer `index`.
json params_of(const std::string& bytes, std::size_t index) {
  const json parsed = json::parse(bytes);
  return parsed.at("composition").at("layers").at(index).at("params");
}

} // namespace

// enforces: 08-serialization#image-serializes-as-uri-only
// enforces: 08-serialization#document-content-round-trips-byte-exact
TEST_CASE("an org.arbc.image layer serializes as a URI and nothing more, byte-exact") {
  ProjectDir project("uri_only");
  project.write("assets/bg.ppm", fix::fixture_bytes());
  const Registry registry = image_registry();
  FilesystemAssetSource assets;

  const std::string authored = "assets/bg.ppm";
  const std::string bytes = document_with_sources({authored});

  KindBridge bridge;
  Document doc;
  REQUIRE(load_document(bytes, doc, bridge, registry, project.base_uri(), &assets).has_value());

  // The asset genuinely resolved and decoded: this is a LIVE image, not a degraded one.
  Content* live = nullptr;
  doc.for_each_content([&live](ObjectId, Content* c) { live = c; });
  REQUIRE(live != nullptr);
  const auto* image = dynamic_cast<const arbc::image::ImageContent*>(live);
  REQUIRE(image != nullptr);
  REQUIRE(image->available());
  REQUIRE(image->bounds() == Rect{0.0, 0.0, fix::k_width, fix::k_height});

  const CodecTable codecs = builtin_codecs(registry);
  const expected<std::string, SerializeError> saved = save_document(doc, bridge, codecs);
  REQUIRE(saved.has_value());

  // THE CLAIM: `params` is EXACTLY {"source": <authored>} -- one key, and it is the URI. No
  // pixel data, no tile table, no intrinsic size (which is why the file stays small).
  const json params = params_of(*saved, 0);
  CHECK(params == json{{"source", authored}});
  CHECK(params.size() == 1);

  // The AUTHORED reference re-saves, not the resolved one. The resolved URI is an absolute
  // path under the OS temp dir; if the writer had absolutised, the document would carry it --
  // and moving the project directory would break every reference in it.
  const std::string resolved = normalize_uri((std::filesystem::path(project.base_uri())
                                                  .parent_path() /
                                              "assets/bg.ppm")
                                                 .string());
  CHECK(saved->find(resolved) == std::string::npos);

  // save -> load -> save is byte-identical.
  CHECK(reload_and_resave(*saved, registry, project.base_uri(), &assets) == *saved);
}

// enforces: 08-serialization#unavailable-asset-is-not-a-read-error
TEST_CASE("a missing, unreadable, or undecodable image asset loads successfully and re-saves") {
  const Registry registry = image_registry();
  const std::string bytes = document_with_sources({"assets/bg.ppm"});

  SECTION("a missing file") {
    ProjectDir project("missing");
    // The `assets/` directory exists; `bg.ppm` does not.
    FilesystemAssetSource assets;
    KindBridge bridge;
    Document doc;
    REQUIRE(load_document(bytes, doc, bridge, registry, project.base_uri(), &assets).has_value());

    Content* live = nullptr;
    doc.for_each_content([&live](ObjectId, Content* c) { live = c; });
    const auto* image = dynamic_cast<const arbc::image::ImageContent*>(live);
    REQUIRE(image != nullptr);
    CHECK_FALSE(image->available());
    CHECK(image->bounds()->empty());                     // no pixels, and no fabricated extent
    CHECK(image->external_asset_ref() == "assets/bg.ppm"); // the ref survives verbatim

    CHECK(reload_and_resave(bytes, registry, project.base_uri(), &assets) == bytes);
  }

  SECTION("an unreadable/undecodable file") {
    ProjectDir project("corrupt");
    project.write("assets/bg.ppm", "P6 this is not an image at all");
    FilesystemAssetSource assets;
    KindBridge bridge;
    Document doc;
    REQUIRE(load_document(bytes, doc, bridge, registry, project.base_uri(), &assets).has_value());

    Content* live = nullptr;
    doc.for_each_content([&live](ObjectId, Content* c) { live = c; });
    const auto* image = dynamic_cast<const arbc::image::ImageContent*>(live);
    REQUIRE(image != nullptr);
    CHECK_FALSE(image->available()); // a corrupt file is a VALUE, never UB and never a throw

    CHECK(reload_and_resave(bytes, registry, project.base_uri(), &assets) == bytes);
  }

  SECTION("no AssetSource installed at all") {
    // `LoadContext::load_asset` fires `on_ready` immediately with empty bytes when no source
    // is installed -- unavailable, never blocking. This is also what a DEFERRING source (a
    // future network source, which does not fire inside `request()`) looks like to v1
    // (Decision 5); the true pending state is `kinds.image_async_pending`.
    KindBridge bridge;
    Document doc;
    REQUIRE(load_document(bytes, doc, bridge, registry, {}, nullptr).has_value());

    Content* live = nullptr;
    doc.for_each_content([&live](ObjectId, Content* c) { live = c; });
    const auto* image = dynamic_cast<const arbc::image::ImageContent*>(live);
    REQUIRE(image != nullptr);
    CHECK_FALSE(image->available());

    CHECK(reload_and_resave(bytes, registry, {}, nullptr) == bytes);
  }
}

// enforces: 03-layer-plugin-interface#image-decodes-once-per-resolved-uri
TEST_CASE("N image layers resolving to one URI issue exactly one decode") {
  ProjectDir project("dedup");
  project.write("assets/bg.ppm", fix::fixture_bytes());
  const Registry registry = image_registry();
  FilesystemAssetSource assets;

  // Three layers, three DIFFERENT authored spellings, ONE resolved identity: doc 08:116-122
  // dedups on the resolved URI, not on the spelling. `normalize_uri` collapses the `.`
  // segments lexically, so all three name one file -- and therefore one decoded pyramid.
  const std::string bytes =
      document_with_sources({"assets/bg.ppm", "assets/./bg.ppm", "./assets/bg.ppm"});

  const std::uint64_t before = arbc::image::default_pyramid_cache().decodes_issued();

  KindBridge bridge;
  Document doc;
  REQUIRE(load_document(bytes, doc, bridge, registry, project.base_uri(), &assets).has_value());

  std::vector<const arbc::image::ImageContent*> images;
  doc.for_each_content([&images](ObjectId, Content* c) {
    images.push_back(dynamic_cast<const arbc::image::ImageContent*>(c));
  });
  REQUIRE(images.size() == 3);

  // EXACTLY ONE decode across the three layers -- a behavioral counter, never a wall-clock
  // assertion (doc 16:54-62).
  CHECK(arbc::image::default_pyramid_cache().decodes_issued() == before + 1);

  // ...and they genuinely share the one pyramid, which is what makes the counter mean
  // "one decode" rather than "three decodes, two discarded".
  for (const arbc::image::ImageContent* image : images) {
    REQUIRE(image != nullptr);
    REQUIRE(image->available());
    CHECK(image->pyramid().get() == images[0]->pyramid().get());
  }

  // Re-rendering an unchanged image issues ZERO further decodes: render is a pure read of the
  // immutable pyramid (Decision 4). Held open by the live document above, so the weak cache
  // entry is still resident.
  const std::uint64_t after_load = arbc::image::default_pyramid_cache().decodes_issued();
  KindBridge again_bridge;
  Document again;
  REQUIRE(
      load_document(bytes, again, again_bridge, registry, project.base_uri(), &assets).has_value());
  CHECK(arbc::image::default_pyramid_cache().decodes_issued() == after_load);

  // Each authored spelling still re-saves EXACTLY as authored -- dedup is about identity, not
  // about rewriting what the document says.
  CHECK(reload_and_resave(bytes, registry, project.base_uri(), &assets) == bytes);
}

// enforces: 08-serialization#unknown-kind-round-trips-verbatim
TEST_CASE("without the image plugin no codec is registered and the layer round-trips verbatim") {
  ProjectDir project("no_plugin");
  project.write("assets/bg.ppm", fix::fixture_bytes());
  const Registry empty; // no plugin loaded => no org.arbc.image factory
  FilesystemAssetSource assets;

  // The gate: `builtin_codecs(registry)` registers the image codec IFF the registry holds the
  // kind's factory. That is the whole of Decision 2's degradation story -- no new machinery.
  CHECK(builtin_codecs(empty).find(k_image_kind_id) == nullptr);
  CHECK(builtin_codecs(image_registry()).find(k_image_kind_id) != nullptr);

  // With no codec, the body falls through to `PlaceholderContent`, which preserves
  // kind/kind_version/params VERBATIM. A user without the image plugin opens the document,
  // saves, and loses nothing.
  const std::string bytes = document_with_sources({"assets/bg.ppm"});
  CHECK(reload_and_resave(bytes, empty, project.base_uri(), &assets) == bytes);
}

// A `params` interior the codec does not consume rides the core's existing residual diff
// (`codec.hpp:99-108`) and re-saves verbatim -- the codec author writes NOTHING for this, and
// this test is the proof that the claim survives contact with a codec that reads exactly one
// key. Two shapes, both hostile-input-shaped and neither a load failure.
TEST_CASE("params interiors the image codec does not consume survive the residual round-trip") {
  ProjectDir project("residual");
  project.write("assets/bg.ppm", fix::fixture_bytes());
  const Registry registry = image_registry();
  FilesystemAssetSource assets;

  const auto round_trips = [&](const json& params) {
    const json body{
        {"arbc", json{{"format", 1}}},
        {"composition",
         json{{"canvas", json::array({0, 0, 1920, 1080})},
              {"layers",
               json::array({json{{"kind", "org.arbc.image"},
                                 {"kind_version", "1"},
                                 {"opacity", 1.0},
                                 {"params", params},
                                 {"transform", json::array({1.0, 0.0, 0.0, 1.0, 0.0, 0.0})},
                                 {"visible", true}}})}}}};
    const std::string bytes = body.dump(2) + "\n";
    CHECK(reload_and_resave(bytes, registry, project.base_uri(), &assets) == bytes);
  };

  // An UNKNOWN interior key beside a perfectly good `source`: a future version of the kind
  // wrote it, or another tool did. It survives untouched (doc 08 Principle 4).
  round_trips(json{{"source", "assets/bg.ppm"}, {"tint", json::array({0.1, 0.2, 0.3})}});

  // A MISTYPED `source` is treated LENIENTLY as absent (the `codec_nested.cpp` idiom): the
  // codec consumes nothing, so even `source` itself rides the residual diff and re-saves
  // verbatim. No hostile input can turn a mistyped key into a load failure.
  round_trips(json{{"source", 42}});
}

TEST_CASE("the image codec reports a registry without the kind, and a refusing factory, as values") {
  LoadContext ctx{""};
  const json params{{"source", "assets/bg.ppm"}};

  // The codec is only REGISTERED when the factory exists, so neither branch is reachable
  // through the assembled table -- but `image_codec` is a public factory function, and a
  // caller may hand it a registry the plugin never touched. Errors are values, never UB.
  const Registry empty;
  const Codec orphaned = image_codec(empty);
  const expected<std::unique_ptr<Content>, ReaderError> no_factory =
      orphaned.deserialize(params, {}, ObjectId{}, ctx);
  REQUIRE_FALSE(no_factory.has_value());
  CHECK(no_factory.error().kind == ReaderError::Kind::MalformedField);

  // A factory that REFUSES to construct is distinct from an unavailable asset (which the kind
  // accepts and reports as empty bounds): it propagates as a ReaderError value.
  Registry refusing;
  REQUIRE(refusing
              .add("org.arbc.image",
                   [](ContentConfig) -> expected<std::unique_ptr<Content>, std::string> {
                     return unexpected<std::string>("image: refused");
                   })
              .has_value());
  const Codec hostile = image_codec(refusing);
  const expected<std::unique_ptr<Content>, ReaderError> refused =
      hostile.deserialize(params, {}, ObjectId{}, ctx);
  REQUIRE_FALSE(refused.has_value());
  CHECK(refused.error().kind == ReaderError::Kind::MalformedField);
}
