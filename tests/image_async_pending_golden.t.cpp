// kinds.image_async_pending, at the pixel: the empty layer REPLACED BY THE PHOTOGRAPH, live.
//
// The behavioural half (`tests/image_async_pending.t.cpp`) proves the counters -- one revision,
// one damage flush, one decode, `pending_external_loads()` reaching zero. This proves the thing
// those counters exist to cause: that after the arrival settles, the frame the user sees is
// BYTE-IDENTICAL to the one they would have seen had the file been local all along. That is doc
// 05:83's "loading a file is async -- mutating the document is not", asserted at the pixel.
//
// The oracle is NOT a hand-typed constant and not a checked-in image: it is THE SAME SCENE,
// loaded synchronously through an inline-firing `FilesystemAssetSource`. Byte-exact, no
// tolerances (doc 16:48-53).

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/expected.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/compositor/compositor.hpp>
#include <arbc/compositor/pull_service.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/contract/registry.hpp>
#include <arbc/kind_image/image_content.hpp>
#include <arbc/media/pixel_format.hpp>
#include <arbc/model/records.hpp>
#include <arbc/runtime/builtin_codecs.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/document_serialize.hpp>
#include <arbc/runtime/filesystem_asset_source.hpp>
#include <arbc/runtime/operator_binding.hpp>
#include <arbc/runtime/pull_identity.hpp>
#include <arbc/serialize/codec.hpp> // CodecTable (names nlohmann::json)
#include <arbc/serialize/load_context.hpp>
#include <arbc/surface/surface.hpp>
#include <arbc/surface/surface_pool.hpp>

#include <catch2/catch_test_macros.hpp>

#include "support/image_fixtures.hpp"
#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace arbc;
namespace fix = arbc::image::testfix;

namespace {

using json = nlohmann::json;

// The deferring double, again -- see `image_async_pending.t.cpp` for why arrival is a thing the
// TEST schedules and never a thing a timer does.
class DeferringAssetSource final : public AssetSource {
public:
  void put(std::string uri, std::string bytes) {
    d_files.insert_or_assign(std::move(uri), std::move(bytes));
  }

  void request(std::string_view resolved_uri,
               std::function<void(std::string_view)> on_ready) override {
    d_outstanding.push_back(Request{std::string(resolved_uri), std::move(on_ready)});
  }

  std::size_t fire_all() {
    std::vector<Request> firing;
    firing.swap(d_outstanding);
    for (const Request& r : firing) {
      const auto it = d_files.find(r.uri);
      r.on_ready(it != d_files.end() ? std::string_view(it->second) : std::string_view{});
    }
    return firing.size();
  }

private:
  struct Request {
    std::string uri;
    std::function<void(std::string_view)> on_ready;
  };

  std::unordered_map<std::string, std::string> d_files;
  std::vector<Request> d_outstanding;
};

// A project directory under the OS temp dir, removed on scope exit -- the inline-firing oracle
// needs a real file, because `FilesystemAssetSource` is the production source it stands for.
class ProjectDir {
public:
  explicit ProjectDir(const std::string& name)
      : d_root(std::filesystem::temp_directory_path() / ("arbc_image_pending_" + name)) {
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

  void write(const std::string& relative, std::string_view bytes) const {
    std::ofstream out(d_root / relative, std::ios::binary);
    REQUIRE(out);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    REQUIRE(out);
  }

  std::string base_uri() const { return (d_root / "project.arbc").string(); }
  // The resolved identity `assets/photo.ppm` names from this project's base URI -- the key the
  // deferring double must answer on, so the two lanes fetch the very same file.
  std::string resolved(const std::string& relative) const {
    return normalize_uri((d_root / relative).string());
  }

private:
  std::filesystem::path d_root;
};

constexpr int k_dim = 128;

// One `org.arbc.image` layer, in the writer's CANONICAL form (sorted keys, 2-space indent,
// trailing newline, every core-owned field spelled out). Canonical matters here and not in the
// behavioural test, because these bytes are compared byte-for-byte against what a save re-emits.
std::string document_with_source(const std::string& source) {
  const json doc{
      {"arbc", json{{"format", 1}}},
      {"composition",
       json{{"canvas", json::array({0, 0, k_dim, k_dim})},
            {"layers", json::array({json{{"kind", "org.arbc.image"},
                                         {"kind_version", "1"},
                                         {"opacity", 1.0},
                                         {"params", json{{"source", source}}},
                                         {"transform", json::array({1.0, 0.0, 0.0, 1.0, 0.0, 0.0})},
                                         {"visible", true}}})}}}};
  return doc.dump(2) + "\n";
}

Registry image_registry() {
  Registry registry;
  REQUIRE(registry
              .add(
                  arbc::image::ImageContent::kind_id,
                  [](ContentConfig config) { return arbc::image::make_image_content(config); },
                  KindMetadata{"Image", "1"})
              .has_value());
  return registry;
}

ObjectId root_composition_of(const Document& doc) {
  const DocStatePtr pin = doc.pin();
  ObjectId root;
  const CompositionRecord* rec = nullptr;
  REQUIRE(pin->find_first_composition(root, rec));
  return root;
}

// The composed frame, as working-space floats -- the whole scene through the real engine (a real
// backend, a real pull service, a real binding scope), which is the only place "the user sees the
// photograph" is actually true.
std::vector<float> composed_frame(Document& doc) {
  CpuBackend backend;
  TileCache cache(64U * 1024 * 1024);
  const DocStatePtr pin = doc.pin();
  const ContentResolver resolve = [&doc](ObjectId id) { return doc.resolve(id); };
  PullConfig config;
  config.id_of = make_pull_identity_of(*pin, resolve);
  const std::uint64_t revision = pin->revision();
  config.contribution = [revision](const Content*) { return revision; };
  PullServiceImpl service(cache, backend, direct_dispatch(), config);
  register_builtin_operator_binders();
  OperatorBindingScope scope = bind_operators(doc, service, backend, pin);
  const expected<std::unique_ptr<Surface>, SurfaceError> target =
      backend.make_surface(k_dim, k_dim, pin->working_space());
  REQUIRE(target.has_value());
  SurfacePool pool(backend);
  const Viewport viewport{k_dim, k_dim, Affine::identity(), root_composition_of(doc)};
  render_frame(*pin, resolve, viewport, backend, pool, **target);
  scope.release();
  const std::span<const float> px = (*target)->span<PixelFormat::Rgba32fLinearPremul>();
  return {px.begin(), px.end()};
}

// Through the codec table built FROM THE REGISTRY: the image codec is gated on the plugin being
// present, so the registry-free `builtin_codecs()` holds no codec for the kind and could not emit
// the layer at all (`image_serialize.t.cpp` asserts that gate directly).
std::string save(const Document& doc, const KindBridge& bridge, const Registry& registry) {
  const CodecTable codecs = builtin_codecs(registry);
  const expected<std::string, SerializeError> out = save_document(doc, bridge, codecs);
  REQUIRE(out.has_value());
  return *out;
}

} // namespace

// enforces: 08-serialization#pending-asset-installs-live
// enforces: 16-sdlc-and-quality#byte-exact-goldens
TEST_CASE("a pending image renders as nothing, then byte-identically to the inline load") {
  ProjectDir project("live");
  project.write("assets/photo.ppm", fix::fixture_bytes());
  const Registry registry = image_registry();
  const std::string bytes = document_with_source("assets/photo.ppm");

  // ORACLE 1 -- the scene with the file MISSING. No `AssetSource` at all, so the reference is
  // unavailable and the layer contributes nothing.
  std::vector<float> missing;
  {
    Document doc;
    KindBridge bridge;
    REQUIRE(load_document(bytes, doc, bridge, registry, project.base_uri(), nullptr).has_value());
    missing = composed_frame(doc);
  }

  // ORACLE 2 -- the scene loaded SYNCHRONOUSLY, through the production inline-firing source.
  // This is the frame the user would see if the photograph had been local all along.
  std::vector<float> inline_loaded;
  std::uint64_t inline_revision = 0;
  {
    FilesystemAssetSource assets;
    Document doc;
    KindBridge bridge;
    REQUIRE(load_document(bytes, doc, bridge, registry, project.base_uri(), &assets).has_value());
    // Constraint 7: the resolved path is unchanged, and does not cost the common case a single
    // extra revision or transaction. An inline load still lands at the revision-0 baseline.
    inline_revision = doc.pin()->revision();
    CHECK(inline_revision == 0);
    CHECK(doc.pending_external_loads() == 0);
    inline_loaded = composed_frame(doc);
  }
  // The two oracles genuinely differ -- otherwise every comparison below would be vacuous.
  REQUIRE(missing != inline_loaded);

  // THE PENDING PATH. The same document, the same file, a source that has not answered.
  DeferringAssetSource source;
  source.put(project.resolved("assets/photo.ppm"), fix::fixture_bytes());
  Document doc;
  KindBridge bridge;
  REQUIRE(load_document(bytes, doc, bridge, registry, project.base_uri(), &source).has_value());
  CHECK(doc.pending_external_loads() == 1);

  // BEFORE the arrival: byte-identical to the file being MISSING. The pixel difference between
  // pending and unavailable is nil, and that is correct -- the user whose photograph is still
  // downloading sees what the user whose photograph is gone sees. The only difference is that
  // one of them will fill in (Decision 1).
  CHECK(composed_frame(doc) == missing);

  CHECK(source.fire_all() == 1);
  CHECK(settle_external_loads(doc, bridge, registry) == 1);
  CHECK(doc.pending_external_loads() == 0);

  // AFTER: byte-identical to the SYNCHRONOUS load. Not approximately, not within a tolerance --
  // the same bytes. Doc 02's Refine step, driven by an ordinary revision bump and a damage route,
  // lands the photograph exactly where a local file would have put it.
  CHECK(composed_frame(doc) == inline_loaded);
  CHECK(doc.pin()->revision() > inline_revision);
}

// enforces: 08-serialization#image-serializes-as-uri-only
TEST_CASE("save-while-pending is a fixed point") {
  // The new state must not leak into the FORMAT. A document saved while its photograph is still
  // in flight is byte-identical to the same document saved with the photograph loaded, and to the
  // same document saved with it missing: `params` is exactly `{"source": "<authored>"}` and
  // nothing else, and it is the AUTHORED spelling, never the resolved one (doc 08 Principle 3).
  // Pending is a fact about the LOAD, and it lives in the core's `PendingExternalLoads`.
  ProjectDir project("fixed_point");
  project.write("assets/photo.ppm", fix::fixture_bytes());
  const Registry registry = image_registry();
  const std::string bytes = document_with_source("assets/photo.ppm");

  const auto saved_through = [&](AssetSource* source, bool settle) {
    Document doc;
    KindBridge bridge;
    REQUIRE(load_document(bytes, doc, bridge, registry, project.base_uri(), source).has_value());
    if (settle) {
      auto* const deferring = dynamic_cast<DeferringAssetSource*>(source);
      REQUIRE(deferring != nullptr);
      CHECK(deferring->fire_all() == 1);
      CHECK(settle_external_loads(doc, bridge, registry) == 1);
    }
    return save(doc, bridge, registry);
  };

  DeferringAssetSource deferring;
  deferring.put(project.resolved("assets/photo.ppm"), fix::fixture_bytes());
  FilesystemAssetSource inline_source;

  const std::string while_pending = saved_through(&deferring, /*settle=*/false);
  const std::string while_missing = saved_through(nullptr, /*settle=*/false);
  const std::string while_loaded = saved_through(&inline_source, /*settle=*/false);

  CHECK(while_pending == while_missing);
  CHECK(while_pending == while_loaded);
  CHECK(while_pending == bytes); // and it is the canonical form the document was authored in

  // A SETTLED image saves identically too: the model gained a revision and the content gained
  // pixels, but the document gained nothing -- which is the whole point of storing a URI.
  DeferringAssetSource settling;
  settling.put(project.resolved("assets/photo.ppm"), fix::fixture_bytes());
  CHECK(saved_through(&settling, /*settle=*/true) == while_pending);

  // `params` is EXACTLY one key, and it is the AUTHORED URI.
  const json params = json::parse(while_pending).at("composition").at("layers").at(0).at("params");
  CHECK(params == json{{"source", "assets/photo.ppm"}});
  CHECK(params.size() == 1);
  CHECK(while_pending.find(project.resolved("assets/photo.ppm")) == std::string::npos);
}
