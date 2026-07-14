// The content-addressed raster tile store, end to end (serialize.raster_tile_store).
//
// THE CENTRAL RULE this file exists to enforce: PERSIST THE TILE TABLE, NOT THE IMAGE.
// A naive implementation that flattens the layer to a dense buffer and writes that would
// pass a round-trip test perfectly -- which is exactly why the round-trip is NOT the
// guard here. The CONTENT-ADDRESSING COUNTERS are: an all-identical layer must write ONE
// blob however many tiles it has, and a re-save after one dab must write (and hash)
// exactly the touched tiles and nothing else. A dense flatten cannot pass either.

#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/contract/registry.hpp>
#include <arbc/kind_raster/raster_content.hpp>
#include <arbc/media/pixel_format.hpp>
#include <arbc/media/pixel_traits.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/model/model.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/document_serialize.hpp>
#include <arbc/runtime/filesystem_asset_sink.hpp>
#include <arbc/runtime/filesystem_asset_source.hpp>
#include <arbc/runtime/raster_tile_store.hpp>
#include <arbc/serialize/codec.hpp> // CodecTable (names nlohmann::json)
#include <arbc/serialize/save_context.hpp>
#include <arbc/serialize/tile_blob.hpp>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace arbc;
using json = nlohmann::json;

namespace {

// A project directory under the OS temp dir, removed on scope exit. Named after the test
// so two lanes of the ctest matrix never collide.
class ProjectDir {
public:
  explicit ProjectDir(const std::string& name)
      : d_root(std::filesystem::temp_directory_path() / ("arbc_tiles_" + name)) {
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

  // Every blob currently in the asset directory, by hash name.
  std::set<std::string> blob_names() const {
    std::set<std::string> out;
    const std::filesystem::path tiles = d_root / "assets" / "tiles";
    std::error_code ec;
    if (!std::filesystem::is_directory(tiles, ec)) {
      return out;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(tiles, ec)) {
      if (entry.is_regular_file()) {
        out.insert(entry.path().filename().string());
      }
    }
    return out;
  }

  // The fan-out directory a blob must live under: `tiles/<first-2-hex>/<full-hex>`.
  bool fanned_out(const std::string& hash) const {
    return std::filesystem::exists(d_root / "assets" / "tiles" / hash.substr(0, 2) / hash);
  }

private:
  std::filesystem::path d_root;
};

// A `w x h` rgba32f buffer, uniform `value` in every channel.
DecodedImage flat(int w, int h, float value) {
  DecodedImage img;
  img.width = w;
  img.height = h;
  img.format = k_working_rgba32f;
  const std::vector<float> f(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4, value);
  const auto* src = reinterpret_cast<const std::byte*>(f.data());
  img.bytes.assign(src, src + f.size() * sizeof(float));
  return img;
}

// A deterministic non-uniform buffer: every tile of it is distinct.
DecodedImage textured(int w, int h) {
  DecodedImage img;
  img.width = w;
  img.height = h;
  img.format = k_working_rgba32f;
  std::vector<float> f(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
  std::uint32_t s = 12345;
  for (float& v : f) {
    s = s * 1664525U + 1013904223U;
    v = static_cast<float>(s >> 8U) / static_cast<float>(1U << 24U);
  }
  const auto* src = reinterpret_cast<const std::byte*>(f.data());
  img.bytes.assign(src, src + f.size() * sizeof(float));
  return img;
}

// A document holding exactly one raster layer.
struct Scene {
  Document doc;
  KindBridge bridge;
  Registry registry;
  std::shared_ptr<RasterContent> raster;
  ObjectId comp;
  ObjectId content;

  Scene(DecodedImage image, int edge) {
    raster = std::make_shared<RasterContent>(std::move(image), edge);
    comp = doc.add_composition(64.0, 64.0);
    content = doc.add_content(raster, bridge.intern(RasterContent::kind_id, "1"));
    doc.attach_layer(comp, doc.add_layer(content, Affine::identity(), 1.0));
  }
};

// The `params` of the document's single layer.
json params_of(const std::string& bytes) {
  const json doc = json::parse(bytes);
  return doc.at("composition").at("layers").at(0).at("params");
}

// Every level of a raster's pyramid, flattened -- the byte-exact mip surface.
std::vector<std::vector<float>> all_levels(const RasterContent& raster) {
  const TileTablePtr table = raster.store().base_table();
  REQUIRE(table);
  std::vector<std::vector<float>> out;
  out.reserve(table->level_count());
  for (std::size_t l = 0; l < table->level_count(); ++l) {
    out.push_back(table->level_pixels(l));
  }
  return out;
}

// The one raster content in a freshly-loaded document.
RasterContent* only_raster(Document& doc) {
  RasterContent* found = nullptr;
  doc.for_each_content([&found](ObjectId, Content* c) {
    if (auto* r = dynamic_cast<RasterContent*>(c); r != nullptr) {
      found = r;
    }
  });
  return found;
}

} // namespace

// A dense-flatten implementation CANNOT pass this. It is the whole point of the task.
//
// enforces: 08-serialization#raster-tiles-persist-as-a-content-addressed-store
TEST_CASE("an all-identical raster layer stores exactly ONE blob, however many tiles") {
  ProjectDir project("one_blob");
  // 64x64 at edge 16 => a 4x4 grid = SIXTEEN level-0 tiles, every one of them identical.
  Scene scene(flat(64, 64, 0.0F), /*edge=*/16);

  const TileTablePtr table = scene.raster->store().base_table();
  REQUIRE(table->level(0).tiles.size() == 16);

  RasterTileStore tiles;
  FilesystemAssetSink sink;
  SaveContext ctx{project.base_uri()};
  ctx.set_asset_sink(&sink);
  ctx.set_storage_format(PixelFormat::Rgba32fLinearPremul);

  const CodecTable codecs = builtin_codecs(scene.registry, &tiles);
  const expected<std::string, SerializeError> saved =
      save_document(scene.doc, scene.bridge, codecs, ctx);
  REQUIRE(saved.has_value());

  // THE COUNTER. Sixteen tiles, one distinct content, ONE blob on disk.
  CHECK(sink.blobs_written() == 1);
  CHECK(project.blob_names().size() == 1);

  // ...and the `params` still names all sixteen, every entry the same hash. The dedup
  // lives in the NAME, not in the tile table.
  const json params = params_of(*saved);
  CHECK(params.at("blobs").size() == 16);
  const std::set<std::string> distinct(params.at("blobs").begin(), params.at("blobs").end());
  CHECK(distinct.size() == 1);
  CHECK(project.fanned_out(*distinct.begin())); // tiles/<hh>/<hash>

  // The first save DOES hash all sixteen, and that is not a defect: `RasterStore::build`
  // mints a fresh pool blob per tile, so sixteen identical tiles are sixteen DISTINCT
  // slots, and the memo -- keyed by slot -- cannot know two slots hold the same bytes
  // without hashing them. The dedup is therefore in the NAME, downstream of the hash,
  // which is exactly why the blob count above is 1 while this is 16.
  CHECK(tiles.tiles_hashed() == 16);

  // What the memo buys is the RE-save: sixteen hits, zero hashes, zero writes.
  const std::uint64_t hashed = tiles.tiles_hashed();
  REQUIRE(save_document(scene.doc, scene.bridge, codecs, ctx).has_value());
  CHECK(tiles.tiles_hashed() == hashed);
  CHECK(sink.blobs_written() == 1);
  CHECK(project.blob_names().size() == 1);
}

// enforces: 08-serialization#raster-tiles-persist-as-a-content-addressed-store
TEST_CASE("two layers sharing a tile's exact pixels write that blob once") {
  ProjectDir project("cross_layer");

  // Two SEPARATE raster contents with identical pixels: different pool slots, different
  // documents' worth of tile tables -- and one blob, because the hash is over the bytes.
  Document doc;
  KindBridge bridge;
  Registry registry;
  const auto a = std::make_shared<RasterContent>(flat(32, 32, 0.25F), /*edge=*/16);
  const auto b = std::make_shared<RasterContent>(flat(32, 32, 0.25F), /*edge=*/16);
  const ObjectId comp = doc.add_composition(64.0, 64.0);
  const ObjectId ida = doc.add_content(a, bridge.intern(RasterContent::kind_id, "1"));
  const ObjectId idb = doc.add_content(b, bridge.intern(RasterContent::kind_id, "1"));
  doc.attach_layer(comp, doc.add_layer(ida, Affine::identity(), 1.0));
  doc.attach_layer(comp, doc.add_layer(idb, Affine::identity(), 1.0));

  RasterTileStore tiles;
  FilesystemAssetSink sink;
  SaveContext ctx{project.base_uri()};
  ctx.set_asset_sink(&sink);
  ctx.set_storage_format(PixelFormat::Rgba32fLinearPremul);

  const CodecTable codecs = builtin_codecs(registry, &tiles);
  REQUIRE(save_document(doc, bridge, codecs, ctx).has_value());

  // Eight tiles across two layers, one distinct content: ONE blob. Dedup across layers is
  // the single largest size lever in doc 08's table (4.3x, above compression's 2.9x).
  CHECK(sink.blobs_written() == 1);
  CHECK(project.blob_names().size() == 1);
}

// The counter that separates a gesture-cadence autosave from a lie: write-if-absent alone
// would give the right BLOB count while still re-hashing the whole document every save.
//
// enforces: 08-serialization#raster-save-is-incremental
TEST_CASE("a re-save after one dab writes and hashes exactly the touched tiles") {
  ProjectDir project("incremental");
  // 64x64 at edge 16 => a 4x4 grid of 16 distinct tiles.
  Scene scene(textured(64, 64), /*edge=*/16);

  RasterTileStore tiles;
  FilesystemAssetSink sink;
  SaveContext ctx{project.base_uri()};
  ctx.set_asset_sink(&sink);
  ctx.set_storage_format(PixelFormat::Rgba32fLinearPremul);
  const CodecTable codecs = builtin_codecs(scene.registry, &tiles);

  REQUIRE(save_document(scene.doc, scene.bridge, codecs, ctx).has_value());
  CHECK(sink.blobs_written() == 16); // every tile is distinct
  CHECK(tiles.tiles_hashed() == 16);

  // A dab landing inside ONE tile (the tile at grid (0,0), pixels [0,16) x [0,16)).
  {
    Model::Transaction txn = scene.doc.transact("dab");
    scene.raster->paint(txn, scene.content, Rect{2.0, 2.0, 6.0, 6.0},
                        WorkingPixel{1.0F, 0.0F, 0.0F, 1.0F});
    REQUIRE(txn.commit());
  }
  scene.doc.drain();

  const std::uint64_t written_before = sink.blobs_written();
  const std::uint64_t hashed_before = tiles.tiles_hashed();

  REQUIRE(save_document(scene.doc, scene.bridge, codecs, ctx).has_value());

  // |T| == 1. EXACTLY one new blob, and EXACTLY one tile hashed -- the other fifteen were
  // memo hits, because a CoW paint leaves their `BlockSlotRef` identity untouched.
  CHECK(sink.blobs_written() - written_before == 1);
  CHECK(tiles.tiles_hashed() - hashed_before == 1);

  // The old blob is still on disk: A SAVE NEVER DELETES (Constraint 6). Another version,
  // another document, or a concurrent editor may reference it.
  CHECK(project.blob_names().size() == 17);
}

// enforces: 08-serialization#raster-tile-store-round-trips-byte-exactly
// enforces: 08-serialization#raster-mips-are-not-persisted
TEST_CASE("a painted document round-trips byte-exactly at rgba32f, and rebuilds its mips") {
  ProjectDir project("roundtrip");
  Scene scene(textured(48, 32), /*edge=*/16);

  const std::vector<std::vector<float>> before = all_levels(*scene.raster);
  REQUIRE(before.size() > 1); // there IS a mip chain to lose

  RasterTileStore tiles;
  FilesystemAssetSink sink;
  FilesystemAssetSource source;
  SaveContext ctx{project.base_uri()};
  ctx.set_asset_sink(&sink);
  ctx.set_storage_format(PixelFormat::Rgba32fLinearPremul);
  const CodecTable codecs = builtin_codecs(scene.registry, &tiles);

  const expected<std::string, SerializeError> saved =
      save_document(scene.doc, scene.bridge, codecs, ctx);
  REQUIRE(saved.has_value());

  // MIPS ARE NOT PERSISTED: the number of blobs on disk is exactly the number of LEVEL-0
  // tiles, and the emitted name set is exactly the `blobs` array. Asserted by count AND by
  // the name set -- not assumed.
  const json params = params_of(*saved);
  const std::set<std::string> named(params.at("blobs").begin(), params.at("blobs").end());
  CHECK(params.at("blobs").size() == 3 * 2); // ceil(48/16) x ceil(32/16)
  CHECK(project.blob_names() == named);
  CHECK(params.at("edge") == 16);
  CHECK(params.at("width") == 48);
  CHECK(params.at("height") == 32);
  CHECK(params.at("tiles") == "assets/tiles/");
  // The `params` carry level 0 and NOTHING else -- no per-level array, no mip key.
  CHECK(params.size() == 5);

  // The storage format rides the document's `arbc` meta block, not the layer's params
  // (Decision 4: dedup requires one format per store).
  CHECK(json::parse(*saved).at("arbc").at("storage_format") == "rgba32f");

  // Reload.
  Document loaded;
  KindBridge lbridge;
  RasterTileStore ltiles;
  REQUIRE(load_document(*saved, loaded, lbridge, scene.registry, project.base_uri(), &source,
                        &ltiles)
              .has_value());
  CHECK(loaded.storage_format() == PixelFormat::Rgba32fLinearPremul);

  RasterContent* const back = only_raster(loaded);
  REQUIRE(back != nullptr);

  // BYTE-EXACT: every level-0 tile bit-identical, AND every mip rung bit-identical --
  // the latter for free, because the load rebuilds through the very `RasterStore::build`
  // path `14-data-model-and-editing#paint-mip-recompute-matches-full-rebuild` already
  // proves byte-identical. This test pins that the save/load path actually routes there.
  const std::vector<std::vector<float>> after = all_levels(*back);
  REQUIRE(after.size() == before.size());
  for (std::size_t l = 0; l < before.size(); ++l) {
    CHECK(after[l] == before[l]);
  }

  // ...and re-serializing yields byte-identical JSON.
  FilesystemAssetSink resink;
  SaveContext rctx{project.base_uri()};
  rctx.set_asset_sink(&resink);
  rctx.set_storage_format(loaded.storage_format());
  const CodecTable rcodecs = builtin_codecs(scene.registry, &ltiles);
  const expected<std::string, SerializeError> resaved =
      save_document(loaded, lbridge, rcodecs, rctx);
  REQUIRE(resaved.has_value());
  CHECK(*resaved == *saved);

  // The re-save wrote NOTHING: every blob is already on disk under its name. And it hashed
  // nothing either -- the load seeded the memo with the names it already knew.
  CHECK(resink.blobs_written() == 0);
  CHECK(ltiles.tiles_hashed() == 0);
}

// enforces: 08-serialization#raster-storage-quantization-is-idempotent
TEST_CASE("at the rgba16f default, save -> load -> save writes zero new blobs") {
  ProjectDir project("quantize");
  Scene scene(textured(32, 32), /*edge=*/16);

  RasterTileStore tiles;
  FilesystemAssetSink sink;
  FilesystemAssetSource source;
  SaveContext ctx{project.base_uri()};
  ctx.set_asset_sink(&sink);
  // The DEFAULT. Lossy from an rgba32f working space, by the user's authored choice.
  CHECK(ctx.storage_format() == PixelFormat::Rgba16fLinearPremul);
  const CodecTable codecs = builtin_codecs(scene.registry, &tiles);

  const expected<std::string, SerializeError> saved =
      save_document(scene.doc, scene.bridge, codecs, ctx);
  REQUIRE(saved.has_value());
  const std::set<std::string> first = project.blob_names();
  CHECK(first.size() == 4);

  // Omitted when default, so the envelope stays exactly today's bytes.
  CHECK_FALSE(json::parse(*saved).at("arbc").contains("storage_format"));

  // Reload and re-save. The first save quantized f32 -> f16; thereafter f16_to_float
  // followed by f16_from_float is EXACT, so the second save re-derives the same storage
  // bytes, hence the same hash set, hence ZERO new blobs. Stating this honestly matters:
  // "byte-exact round-trip" is unqualified only at rgba32f.
  Document loaded;
  KindBridge lbridge;
  RasterTileStore ltiles;
  REQUIRE(load_document(*saved, loaded, lbridge, scene.registry, project.base_uri(), &source,
                        &ltiles)
              .has_value());
  CHECK(loaded.storage_format() == PixelFormat::Rgba16fLinearPremul);

  FilesystemAssetSink resink;
  SaveContext rctx{project.base_uri()};
  rctx.set_asset_sink(&resink);
  rctx.set_storage_format(loaded.storage_format());
  const CodecTable rcodecs = builtin_codecs(scene.registry, &ltiles);
  const expected<std::string, SerializeError> resaved =
      save_document(loaded, lbridge, rcodecs, rctx);
  REQUIRE(resaved.has_value());

  CHECK(resink.blobs_written() == 0);
  CHECK(project.blob_names() == first); // an IDENTICAL hash set
  CHECK(*resaved == *saved);            // and identical bytes
}

TEST_CASE("a save with no AssetSink is an error, never a silent success") {
  Scene scene(flat(32, 32, 0.5F), /*edge=*/16);
  const CodecTable codecs = builtin_codecs(scene.registry, nullptr);

  // The convenience overload builds a SINK-LESS context. A document with pixels to store
  // must not quietly serialize to a `blobs` array naming files that are nowhere
  // (Constraint 5).
  const expected<std::string, SerializeError> saved =
      save_document(scene.doc, scene.bridge, codecs);
  REQUIRE_FALSE(saved.has_value());
  CHECK(saved.error().kind == SerializeError::Kind::AssetSinkMissing);
}

TEST_CASE("the zero-argument codec table saves correct pixels, just without the memo") {
  ProjectDir project("no_memo");
  Scene scene(textured(32, 32), /*edge=*/16);

  FilesystemAssetSink sink;
  SaveContext ctx{project.base_uri()};
  ctx.set_asset_sink(&sink);
  ctx.set_storage_format(PixelFormat::Rgba32fLinearPremul);

  // `builtin_codecs()` binds `raster_codec(nullptr)`: correct but NON-memoizing. Every
  // existing call site keeps working and still saves correct pixels -- it just re-hashes
  // every tile on every save.
  const CodecTable codecs = builtin_codecs();
  const expected<std::string, SerializeError> saved =
      save_document(scene.doc, scene.bridge, codecs, ctx);
  REQUIRE(saved.has_value());
  CHECK(sink.blobs_written() == 4);
  CHECK(project.blob_names().size() == 4);

  // ...and the pixels genuinely round-trip.
  FilesystemAssetSource source;
  Document loaded;
  KindBridge lbridge;
  REQUIRE(
      load_document(*saved, loaded, lbridge, scene.registry, project.base_uri(), &source).has_value());
  RasterContent* const back = only_raster(loaded);
  REQUIRE(back != nullptr);
  CHECK(all_levels(*back) == all_levels(*scene.raster));
}
