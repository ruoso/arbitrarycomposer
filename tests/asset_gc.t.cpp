// EXPLICIT MARK-AND-SWEEP GC over the on-disk tile store (serialize.asset_gc). The reverse of
// the never-delete save: `FilesystemAssetSink` writes-if-absent and NEVER deletes (a save
// cannot prove a blob dead), so the asset directory only grows -- until this user-driven sweep
// enumerates the on-disk blobs, subtracts the union of what the caller's documents reference,
// and deletes the difference.
//
// The assertions are BEHAVIORAL COUNTERS on the `GcReport` and blob presence (doc 16 tier 4),
// never a directory size or a wall-clock. Each claim is pinned by painting a real raster
// document to a temp project directory through the same save path a host uses, then reasoning
// about which blobs the sweep must keep and which it must reclaim.

#include <arbc/base/transform.hpp>
#include <arbc/contract/registry.hpp>
#include <arbc/kind_raster/raster_content.hpp>
#include <arbc/media/pixel_format.hpp>
#include <arbc/runtime/asset_gc.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/document_serialize.hpp>
#include <arbc/runtime/filesystem_asset_sink.hpp>
#include <arbc/runtime/filesystem_asset_source.hpp>
#include <arbc/runtime/raster_tile_store.hpp>
#include <arbc/serialize/codec.hpp>
#include <arbc/serialize/save_context.hpp>
#include <arbc/serialize/tile_blob.hpp>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

using namespace arbc;
using json = nlohmann::json;

namespace {

class ProjectDir {
public:
  explicit ProjectDir(const std::string& name)
      : d_root(std::filesystem::temp_directory_path() / ("arbc_asset_gc_" + name)) {
    std::error_code ec;
    std::filesystem::remove_all(d_root, ec);
    REQUIRE(std::filesystem::create_directories(d_root, ec));
  }
  ~ProjectDir() {
    std::error_code ec;
    std::filesystem::remove_all(d_root, ec);
  }
  ProjectDir(const ProjectDir&) = delete;
  ProjectDir& operator=(const ProjectDir&) = delete;

  std::string base_uri() const { return (d_root / "project.arbc").string(); }
  std::string doc_path(const std::string& name) const { return (d_root / name).string(); }
  const std::filesystem::path& root() const { return d_root; }

  void write_text(const std::string& rel, const std::string& text) const {
    const std::filesystem::path p = d_root / rel;
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    REQUIRE(out.good());
  }

  // Drop a blob into the fan-out slot the store derives, as if a prior save had written it.
  void write_blob(const std::string& hash, const std::string& content) const {
    const std::filesystem::path dir = d_root / "assets" / "tiles" / hash.substr(0, 2);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream out(dir / hash, std::ios::binary);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    REQUIRE(out.good());
  }

  bool blob_exists(const std::string& hash) const {
    return std::filesystem::exists(d_root / "assets" / "tiles" / hash.substr(0, 2) / hash);
  }

private:
  std::filesystem::path d_root;
};

float sample_at(int x, int y, std::size_t c, float bias) {
  const std::uint32_t k =
      (static_cast<std::uint32_t>(y) * 7919U + static_cast<std::uint32_t>(x)) * 4U +
      static_cast<std::uint32_t>(c);
  const std::uint32_t s = k * 2654435761U;
  const float base = static_cast<float>(s >> 8U) / static_cast<float>(1U << 24U);
  const float v = base + bias;
  return v - static_cast<float>(static_cast<int>(v)); // fractional part: a distinct value set
}

// `bias` selects a distinct pixel field -- two different biases produce disjoint tile hashes,
// which is how a "later painted state" is simulated without the brush pipeline.
DecodedImage textured(int w, int h, float bias) {
  DecodedImage img;
  img.width = w;
  img.height = h;
  img.format = k_working_rgba32f;
  std::vector<float> f(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * k_tile_channels);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      for (std::size_t c = 0; c < k_tile_channels; ++c) {
        f[(static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
           static_cast<std::size_t>(x)) *
              k_tile_channels +
          c] = sample_at(x, y, c, bias);
      }
    }
  }
  const auto* src = reinterpret_cast<const std::byte*>(f.data());
  img.bytes.assign(src, src + f.size() * sizeof(float));
  return img;
}

struct Scene {
  Document doc;
  KindBridge bridge;
  Registry registry;
  std::shared_ptr<RasterContent> raster;

  Scene(DecodedImage image, int edge) {
    raster = std::make_shared<RasterContent>(std::move(image), edge);
    const ObjectId comp = doc.add_composition(64.0, 64.0);
    const ObjectId content = doc.add_content(raster, bridge.intern(RasterContent::kind_id, "1"));
    doc.attach_layer(comp, doc.add_layer(content, Affine::identity(), 1.0));
  }
};

// Save `scene` into the asset directory `base_uri` resolves against; return the `.arbc` text.
std::string save_scene(Scene& scene, const std::string& base_uri) {
  RasterTileStore tiles;
  FilesystemAssetSink sink;
  SaveContext ctx{base_uri};
  ctx.set_asset_sink(&sink);
  ctx.set_storage_format(PixelFormat::Rgba32fLinearPremul);
  const CodecTable codecs = builtin_codecs(scene.registry, &tiles);
  const expected<std::string, SerializeError> saved =
      save_document(scene.doc, scene.bridge, codecs, ctx);
  REQUIRE(saved.has_value());
  return *saved;
}

// The level-0 blob hashes a saved document references (its `params.blobs`).
std::set<std::string> blobs_of(const std::string& arbc_text) {
  const json params = json::parse(arbc_text).at("composition").at("layers").at(0).at("params");
  std::set<std::string> out;
  for (const json& b : params.at("blobs")) {
    out.insert(b.get<std::string>());
  }
  return out;
}

// Every valid tile-hash blob currently on disk under the project's tiles subtree.
std::set<std::string> on_disk_tiles(const std::filesystem::path& root) {
  std::set<std::string> out;
  const std::filesystem::path tiles = root / "assets" / "tiles";
  std::error_code ec;
  if (!std::filesystem::exists(tiles, ec)) {
    return out;
  }
  for (const auto& e : std::filesystem::recursive_directory_iterator(tiles, ec)) {
    if (e.is_regular_file() && is_tile_hash(e.path().filename().string())) {
      out.insert(e.path().filename().string());
    }
  }
  return out;
}

const std::string k_orphan = std::string(k_tile_hash_chars, '0'); // a hash no document names

} // namespace

TEST_CASE("the mark walk harvests params.blobs from every reachable body, keyed on shape") {
  // A document body's tile references live in params.blobs; the walk recurses the whole tree,
  // so a body nested inside a compositions table or an inputs list is reached wherever it sits.
  const std::string h0 = std::string(k_tile_hash_chars, 'a');
  const std::string h1 = std::string(k_tile_hash_chars, 'b');
  json doc;
  doc["composition"]["layers"][0]["params"]["blobs"] = json::array({h0});
  // A deeper body, as a nested composition / operator input child would nest.
  doc["compositions"][0]["layers"][0]["inputs"][0]["params"]["blobs"] = json::array({h1});

  const expected<std::unordered_set<std::string>, ReaderError> refs =
      collect_referenced_tiles(doc.dump());
  REQUIRE(refs.has_value());
  CHECK(refs->size() == 2);
  CHECK(refs->count(h0) == 1);
  CHECK(refs->count(h1) == 1); // the nested body's blob was reached too
}

TEST_CASE("the mark walk is fail-safe on a malformed blobs reference") {
  SECTION("unparseable text is MalformedJson") {
    const expected<std::unordered_set<std::string>, ReaderError> refs =
        collect_referenced_tiles("{ not json ");
    REQUIRE_FALSE(refs.has_value());
    CHECK(refs.error().kind == ReaderError::Kind::MalformedJson);
  }
  SECTION("a non-array blobs is MalformedField") {
    json doc;
    doc["composition"]["layers"][0]["params"]["blobs"] = "not-an-array";
    const expected<std::unordered_set<std::string>, ReaderError> refs =
        collect_referenced_tiles(doc.dump());
    REQUIRE_FALSE(refs.has_value());
    CHECK(refs.error().kind == ReaderError::Kind::MalformedField);
  }
  SECTION("a non-string blobs entry is MalformedField") {
    json doc;
    doc["composition"]["layers"][0]["params"]["blobs"] = json::array({42});
    const expected<std::unordered_set<std::string>, ReaderError> refs =
        collect_referenced_tiles(doc.dump());
    REQUIRE_FALSE(refs.has_value());
    CHECK(refs.error().kind == ReaderError::Kind::MalformedField);
  }
  SECTION("a non-hex blobs entry is MalformedField") {
    json doc;
    doc["composition"]["layers"][0]["params"]["blobs"] = json::array({"not-a-hash"});
    const expected<std::unordered_set<std::string>, ReaderError> refs =
        collect_referenced_tiles(doc.dump());
    REQUIRE_FALSE(refs.has_value());
    CHECK(refs.error().kind == ReaderError::Kind::MalformedField);
  }
}

TEST_CASE("gc_project_directory on a directory that does not exist is a MarkFailed value") {
  const std::filesystem::path missing =
      std::filesystem::temp_directory_path() / "arbc_asset_gc_nonexistent_dir_xyz";
  std::error_code ec;
  std::filesystem::remove_all(missing, ec);
  const expected<GcReport, GcError> report = gc_project_directory(missing, false);
  REQUIRE_FALSE(report.has_value());
  CHECK(report.error().kind == GcError::Kind::MarkFailed);
}

TEST_CASE("a store whose tiles path cannot be enumerated fails the sweep, deleting nothing") {
  ProjectDir project("unenumerable");
  // Make `assets/tiles` a regular FILE, not a directory: enumeration then fails as a value.
  project.write_text("assets/tiles", "i am a file, not a directory");

  const std::string tiles_base = (project.root() / "assets" / "tiles").string(); // no slash
  FilesystemAssetReaper reaper(tiles_base);
  const expected<GcReport, GcError> report = sweep_tile_store({}, reaper, false);
  REQUIRE_FALSE(report.has_value());
  CHECK(report.error().kind == GcError::Kind::EnumerateFailed);
}

TEST_CASE("the sweep deletes exactly the unreferenced set and the swept document round-trips") {
  // enforces: 08-serialization#asset-gc-deletes-only-unreferenced-tiles
  ProjectDir project("deletes_only");
  Scene scene(textured(16, 16, 0.0f), 8);
  const std::string arbc = save_scene(scene, project.base_uri());
  project.write_text("project.arbc", arbc);

  const std::set<std::string> referenced = blobs_of(arbc);
  REQUIRE_FALSE(referenced.empty());
  REQUIRE(referenced.find(k_orphan) == referenced.end());
  project.write_blob(k_orphan, "orphan-bytes");

  const expected<GcReport, GcError> report =
      gc_project_directory(project.root(), /*dry_run=*/false);
  REQUIRE(report.has_value());
  CHECK(report->deleted == 1); // exactly D \ R = {orphan}
  CHECK(report->referenced == referenced.size());
  CHECK_FALSE(project.blob_exists(k_orphan));
  for (const std::string& h : referenced) {
    CHECK(project.blob_exists(h)); // every blob in R is retained
  }

  // Round-trip: the swept document reloads and re-saves byte-identically -- every referenced
  // blob still decodes and verifies against its name.
  Document loaded;
  KindBridge lbridge;
  RasterTileStore ltiles;
  FilesystemAssetSource source;
  const expected<std::monostate, ReaderError> ok =
      load_document(arbc, loaded, lbridge, scene.registry, project.base_uri(), &source, &ltiles);
  REQUIRE(ok.has_value());

  FilesystemAssetSink sink2;
  SaveContext ctx2{project.base_uri()};
  ctx2.set_asset_sink(&sink2);
  ctx2.set_storage_format(PixelFormat::Rgba32fLinearPremul);
  const CodecTable codecs2 = builtin_codecs(scene.registry, &ltiles);
  const expected<std::string, SerializeError> resaved =
      save_document(loaded, lbridge, codecs2, ctx2);
  REQUIRE(resaved.has_value());
  CHECK(*resaved == arbc);
}

TEST_CASE("a save leaks; only an explicit GC reclaims the orphaned pre-edit blobs") {
  // enforces: 08-serialization#asset-gc-is-explicit-never-on-save
  ProjectDir project("never_on_save");

  Scene scene_a(textured(16, 16, 0.0f), 8);
  const std::string arbc_a = save_scene(scene_a, project.base_uri());
  const std::set<std::string> ha = blobs_of(arbc_a);

  // A later painted state ("one dab touching tile set T") saved into the SAME asset directory.
  Scene scene_b(textured(16, 16, 0.5f), 8);
  const std::string arbc_b = save_scene(scene_b, project.base_uri());
  const std::set<std::string> hb = blobs_of(arbc_b);
  project.write_text("project.arbc", arbc_b); // the current document is B

  // The save NEVER DELETED: every pre-edit blob A referenced is still on disk beside B's.
  for (const std::string& h : ha) {
    CHECK(project.blob_exists(h));
  }

  std::set<std::string> orphaned; // HA \ HB: the now-unreferenced pre-edit blobs
  for (const std::string& h : ha) {
    if (hb.find(h) == hb.end()) {
      orphaned.insert(h);
    }
  }
  REQUIRE_FALSE(orphaned.empty());

  // GC with the current document (B) as the sole root: exactly A's orphaned blobs go.
  const expected<GcReport, GcError> report = gc_project_directory(project.root(), false);
  REQUIRE(report.has_value());
  CHECK(report->deleted == orphaned.size());
  for (const std::string& h : orphaned) {
    CHECK_FALSE(project.blob_exists(h));
  }
  for (const std::string& h : hb) {
    CHECK(project.blob_exists(h)); // the current tiles are retained
  }
}

TEST_CASE("GC unions references across documents sharing one asset directory") {
  // enforces: 08-serialization#asset-gc-unions-references-across-documents
  ProjectDir project("unions");

  Scene scene_a(textured(16, 16, 0.0f), 8);
  const std::string arbc_a = save_scene(scene_a, project.doc_path("docA.arbc"));
  project.write_text("docA.arbc", arbc_a);
  const std::set<std::string> ha = blobs_of(arbc_a);

  Scene scene_b(textured(16, 16, 0.5f), 8);
  const std::string arbc_b = save_scene(scene_b, project.doc_path("docB.arbc"));
  project.write_text("docB.arbc", arbc_b);
  const std::set<std::string> hb = blobs_of(arbc_b);

  project.write_blob(k_orphan, "orphan"); // referenced by neither document

  const expected<GcReport, GcError> report = gc_project_directory(project.root(), false);
  REQUIRE(report.has_value());
  // A blob referenced only by the SECOND document is not deleted; the union covers both.
  for (const std::string& h : ha) {
    CHECK(project.blob_exists(h));
  }
  for (const std::string& h : hb) {
    CHECK(project.blob_exists(h));
  }
  CHECK_FALSE(project.blob_exists(k_orphan)); // referenced by neither: reclaimed
  CHECK(report->deleted == 1);
}

TEST_CASE("GC leaves non-tile assets untouched") {
  // enforces: 08-serialization#asset-gc-leaves-non-tile-assets
  ProjectDir project("non_tile");
  Scene scene(textured(16, 16, 0.0f), 8);
  const std::string arbc = save_scene(scene, project.base_uri());
  project.write_text("project.arbc", arbc);

  // An imported image beside the tiles, and a non-hash-named file UNDER tiles/.
  project.write_text("assets/bg.png", "png-bytes");
  project.write_text("assets/tiles/zz/readme.txt", "not a blob");
  project.write_blob(k_orphan, "orphan");

  const expected<GcReport, GcError> report = gc_project_directory(project.root(), false);
  REQUIRE(report.has_value());
  CHECK(report->deleted == 1); // only the orphan tile
  CHECK_FALSE(project.blob_exists(k_orphan));
  CHECK(std::filesystem::exists(project.root() / "assets" / "bg.png"));
  CHECK(std::filesystem::exists(project.root() / "assets" / "tiles" / "zz" / "readme.txt"));
}

TEST_CASE("fail-safe: a broken mark deletes nothing") {
  // enforces: 08-serialization#asset-gc-fails-safe-deletes-nothing
  ProjectDir project("fail_safe");
  Scene scene(textured(16, 16, 0.0f), 8);
  const std::string arbc = save_scene(scene, project.base_uri());
  project.write_blob(k_orphan, "orphan");
  const std::set<std::string> before = on_disk_tiles(project.root());
  REQUIRE(before.count(k_orphan) == 1);

  SECTION("an unparseable JSON root") {
    project.write_text("project.arbc", "{ not valid json ");
    const expected<GcReport, GcError> report = gc_project_directory(project.root(), false);
    REQUIRE_FALSE(report.has_value());
    CHECK(report.error().kind == GcError::Kind::MarkFailed);
  }
  SECTION("a blobs entry that is not a tile hash") {
    json doc = json::parse(arbc);
    doc["composition"]["layers"][0]["params"]["blobs"][0] = "not-a-hash";
    project.write_text("project.arbc", doc.dump());
    const expected<GcReport, GcError> report = gc_project_directory(project.root(), false);
    REQUIRE_FALSE(report.has_value());
    CHECK(report.error().kind == GcError::Kind::MarkFailed);
  }

  // Either way: the on-disk set is bit-for-bit unchanged -- ZERO deletions, orphan included.
  CHECK(on_disk_tiles(project.root()) == before);
  CHECK(project.blob_exists(k_orphan));
}

TEST_CASE("GC is idempotent") {
  // enforces: 08-serialization#asset-gc-is-idempotent
  ProjectDir project("idempotent");
  Scene scene(textured(16, 16, 0.0f), 8);
  const std::string arbc = save_scene(scene, project.base_uri());
  project.write_text("project.arbc", arbc);
  project.write_blob(k_orphan, "orphan");

  const expected<GcReport, GcError> first = gc_project_directory(project.root(), false);
  REQUIRE(first.has_value());
  CHECK(first->deleted == 1);

  const expected<GcReport, GcError> second = gc_project_directory(project.root(), false);
  REQUIRE(second.has_value());
  CHECK(second->deleted == 0); // a fixed point in one pass: re-running is a no-op
}

TEST_CASE("dry_run reports the reclamation without mutating the store") {
  ProjectDir project("dry_run");
  Scene scene(textured(16, 16, 0.0f), 8);
  const std::string arbc = save_scene(scene, project.base_uri());
  project.write_text("project.arbc", arbc);
  project.write_blob(k_orphan, "orphan-bytes"); // 12 bytes

  const expected<GcReport, GcError> preview =
      gc_project_directory(project.root(), /*dry_run=*/true);
  REQUIRE(preview.has_value());
  CHECK(preview->deleted == 1);
  CHECK(preview->bytes_reclaimed == 12);
  CHECK(project.blob_exists(k_orphan)); // previewed, not touched

  const expected<GcReport, GcError> real = gc_project_directory(project.root(), false);
  REQUIRE(real.has_value());
  CHECK(real->deleted == preview->deleted); // the real run reclaims exactly what was previewed
  CHECK(real->bytes_reclaimed == preview->bytes_reclaimed);
  CHECK_FALSE(project.blob_exists(k_orphan));
}
