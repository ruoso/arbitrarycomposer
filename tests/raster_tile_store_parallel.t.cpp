// The PARALLEL tile-encode save, end to end (serialize.tile_store_parallel_save).
//
// The oracle is EQUALITY TO THE SERIAL SAVE (Decision 3): the same painted document saved
// through the inline executor and across N pool workers must diff to zero bytes -- canonical
// .arbc JSON, the row-major `blobs` array, and every on-disk blob (name AND bytes). That is
// the strongest possible check that parallelism is a pure throughput change and nothing
// else. Every assertion here is a behavioral counter or a byte-exact diff; there is NO
// wall-clock assertion (doc 16:225-226).

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
#include <arbc/runtime/raster_tile_store.hpp>
#include <arbc/runtime/tile_encode_dispatch.hpp>
#include <arbc/runtime/worker_pool.hpp>
#include <arbc/serialize/codec.hpp> // CodecTable (names nlohmann::json)
#include <arbc/serialize/save_context.hpp>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace arbc;
using json = nlohmann::json;

namespace {

// An in-memory sink: the point of these tests is executor-independence, not the filesystem.
// It keeps the full name->bytes map so two saves can be diffed blob-for-blob, and can be
// told to FAIL its next write, to drive the fan-out's mid-save abort/drain path.
class MemorySink final : public AssetSink {
public:
  expected<bool, AssetSinkError> put(std::string_view uri, std::span<const std::byte> bytes) override {
    const std::lock_guard<std::mutex> lock(d_mutex);
    if (d_fail_writes) {
      return unexpected(AssetSinkError{AssetSinkError::Kind::WriteFailed});
    }
    const auto it = d_blobs.find(std::string(uri));
    if (it != d_blobs.end()) {
      return false; // already present: write-if-absent
    }
    d_blobs.emplace(std::string(uri), std::vector<std::byte>(bytes.begin(), bytes.end()));
    d_written.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  bool contains(std::string_view uri) const override {
    const std::lock_guard<std::mutex> lock(d_mutex);
    return d_blobs.find(std::string(uri)) != d_blobs.end();
  }
  std::uint64_t blobs_written() const noexcept override {
    return d_written.load(std::memory_order_relaxed);
  }

  void fail_writes() { d_fail_writes = true; }
  std::map<std::string, std::vector<std::byte>> snapshot() const {
    const std::lock_guard<std::mutex> lock(d_mutex);
    return d_blobs;
  }

private:
  mutable std::mutex d_mutex;
  std::map<std::string, std::vector<std::byte>> d_blobs;
  std::atomic<std::uint64_t> d_written{0};
  bool d_fail_writes{false};
};

// A deterministic non-uniform buffer: every tile of it is distinct (same seed as the serial
// golden, so it is the same document both suites reason about).
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

// A `w x h` rgba32f buffer, uniform in every channel -- every tile identical and ref-shared.
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

  void dab(const Rect& region, const WorkingPixel& color) {
    Model::Transaction txn = doc.transact("dab");
    raster->paint(txn, content, region, color);
    REQUIRE(txn.commit());
    doc.drain();
  }
};

constexpr const char* k_base = "/proj/project.arbc";
constexpr std::size_t k_workers = 4;

} // namespace

// enforces: 08-serialization#raster-save-is-executor-independent
TEST_CASE("a worker-backed save is byte-identical to the inline save") {
  // 64x64 at edge 16 => a 4x4 grid of 16 distinct tiles, so the fan-out has real work.
  // Two independent scenes over the SAME deterministic image: identical pool slots, identical
  // pixels, so any divergence is the executor's doing and nothing else.
  Scene inline_scene(textured(64, 64), /*edge=*/16);
  Scene worker_scene(textured(64, 64), /*edge=*/16);

  // Inline: the default executor (null dispatch), byte-identical to the serial save.
  RasterTileStore inline_tiles;
  MemorySink inline_sink;
  SaveContext inline_ctx{k_base};
  inline_ctx.set_asset_sink(&inline_sink);
  inline_ctx.set_storage_format(PixelFormat::Rgba32fLinearPremul);
  const CodecTable inline_codecs = builtin_codecs(inline_scene.registry, &inline_tiles);
  const expected<std::string, SerializeError> inline_saved =
      save_document(inline_scene.doc, inline_scene.bridge, inline_codecs, inline_ctx);
  REQUIRE(inline_saved.has_value());

  // Worker-backed: fan the encode across a real pool.
  WorkerPoolConfig cfg;
  cfg.worker_count = k_workers;
  WorkerPool pool(cfg);
  int owner = 0;
  TileEncodeDispatch dispatch(pool, &owner);
  RasterTileStore worker_tiles;
  MemorySink worker_sink;
  SaveContext worker_ctx{k_base};
  worker_ctx.set_asset_sink(&worker_sink);
  worker_ctx.set_storage_format(PixelFormat::Rgba32fLinearPremul);
  const CodecTable worker_codecs =
      builtin_codecs(worker_scene.registry, &worker_tiles, &dispatch);
  const expected<std::string, SerializeError> worker_saved =
      save_document(worker_scene.doc, worker_scene.bridge, worker_codecs, worker_ctx);
  REQUIRE(worker_saved.has_value());

  // BYTE-IDENTICAL canonical JSON, identical on-disk blob set (names AND bytes), identical
  // behavioral counters. This is Constraint 1.
  CHECK(*worker_saved == *inline_saved);
  CHECK(worker_sink.snapshot() == inline_sink.snapshot());
  CHECK(worker_sink.blobs_written() == inline_sink.blobs_written());
  CHECK(worker_tiles.tiles_hashed() == inline_tiles.tiles_hashed());
  CHECK(inline_sink.blobs_written() == 16); // sanity: 16 distinct tiles really were written
  CHECK(dispatch.peak_in_flight() >= 1);    // the fan-out actually ran on the pool
}

// enforces: 08-serialization#raster-save-is-incremental
// enforces: 08-serialization#raster-tile-store-round-trips-byte-exactly
TEST_CASE("incremental-save counters are executor-independent") {
  // The same incremental behavior under worker_count in {0, N}: parallelism changes
  // throughput, never the |T| blobs written / |T| tiles hashed of a re-save after one dab.
  const auto run = [](std::size_t worker_count) {
    Scene scene(textured(64, 64), /*edge=*/16);

    WorkerPoolConfig cfg;
    cfg.worker_count = worker_count;
    WorkerPool pool(cfg);
    int owner = 0;
    TileEncodeDispatch dispatch(pool, &owner);

    RasterTileStore tiles;
    MemorySink sink;
    SaveContext ctx{k_base};
    ctx.set_asset_sink(&sink);
    ctx.set_storage_format(PixelFormat::Rgba32fLinearPremul);
    const CodecTable codecs = builtin_codecs(scene.registry, &tiles, &dispatch);

    REQUIRE(save_document(scene.doc, scene.bridge, codecs, ctx).has_value());
    CHECK(sink.blobs_written() == 16); // every tile distinct
    CHECK(tiles.tiles_hashed() == 16);

    // A dab landing inside ONE tile (grid (0,0)).
    scene.dab(Rect{2.0, 2.0, 6.0, 6.0}, WorkingPixel{1.0F, 0.0F, 0.0F, 1.0F});

    const std::uint64_t written_before = sink.blobs_written();
    const std::uint64_t hashed_before = tiles.tiles_hashed();
    REQUIRE(save_document(scene.doc, scene.bridge, codecs, ctx).has_value());

    // |T| == 1 under EITHER executor: exactly one new blob, exactly one tile hashed; the
    // other fifteen were memo hits (a CoW paint left their BlockSlotRef identity untouched)
    // and dispatched no encode.
    CHECK(sink.blobs_written() - written_before == 1);
    CHECK(tiles.tiles_hashed() - hashed_before == 1);

    // And a save-with-no-changes re-save writes and hashes zero, worker-backed too.
    const std::uint64_t written_after = sink.blobs_written();
    const std::uint64_t hashed_after = tiles.tiles_hashed();
    REQUIRE(save_document(scene.doc, scene.bridge, codecs, ctx).has_value());
    CHECK(sink.blobs_written() == written_after);
    CHECK(tiles.tiles_hashed() == hashed_after);
  };

  SECTION("inline executor (worker_count == 0)") { run(0); }
  SECTION("worker-backed executor (worker_count == N)") { run(k_workers); }
}

// enforces: 08-serialization#raster-tiles-persist-as-a-content-addressed-store
TEST_CASE("an all-identical layer writes one blob under a parallel save; a re-save fans zero encodes") {
  // 64x64 at edge 16 => sixteen identical tiles. `RasterStore::build` mints a fresh slot per
  // tile, so the FIRST save legitimately hashes all sixteen (a slot-keyed memo cannot know
  // two slots hold the same bytes without hashing them) -- and the content dedup on the REAP
  // thread collapses them to ONE blob, identically to the serial oracle. This is claim 316
  // re-enforced under a worker-backed executor.
  Scene scene(flat(64, 64, 0.0F), /*edge=*/16);

  WorkerPoolConfig cfg;
  cfg.worker_count = k_workers;
  WorkerPool pool(cfg);
  int owner = 0;

  RasterTileStore tiles;
  MemorySink sink;
  SaveContext ctx{k_base};
  ctx.set_asset_sink(&sink);
  ctx.set_storage_format(PixelFormat::Rgba32fLinearPremul);

  {
    TileEncodeDispatch dispatch(pool, &owner);
    const CodecTable codecs = builtin_codecs(scene.registry, &tiles, &dispatch);
    const expected<std::string, SerializeError> saved =
        save_document(scene.doc, scene.bridge, codecs, ctx);
    REQUIRE(saved.has_value());

    CHECK(sink.blobs_written() == 1);  // ONE blob, content-deduped on the reap thread
    CHECK(tiles.tiles_hashed() == 16); // sixteen distinct fresh slots, hashed -- matching serial

    // ...and all sixteen grid positions name that one blob, row-major.
    const json params = json::parse(*saved).at("composition").at("layers").at(0).at("params");
    REQUIRE(params.at("blobs").size() == 16);
    const std::string first = params.at("blobs").at(0).get<std::string>();
    for (const auto& name : params.at("blobs")) {
      CHECK(name.get<std::string>() == first);
    }
  }

  // The RE-save is the sparse case the fan-out must cost nothing on: every tile is a memo HIT
  // (its BlockSlotRef identity untouched), so the save-thread probe resolves it for free and
  // the fan-out dispatches ZERO encode jobs -- peak in flight 0, not one redundant compress.
  {
    TileEncodeDispatch redispatch(pool, &owner);
    const CodecTable recodecs = builtin_codecs(scene.registry, &tiles, &redispatch);
    REQUIRE(save_document(scene.doc, scene.bridge, recodecs, ctx).has_value());
    CHECK(redispatch.peak_in_flight() == 0); // ref-shared/untouched tiles cost zero encodes
    CHECK(sink.blobs_written() == 1);         // unchanged
    CHECK(tiles.tiles_hashed() == 16);        // unchanged: no re-hashing
  }
}

// enforces: 08-serialization#raster-parallel-save-encode-is-bounded
TEST_CASE("the parallel encode fan-out is bounded in flight") {
  // 128x128 at edge 16 => an 8x8 grid of 64 distinct tiles, far more than the worker count,
  // so the window genuinely throttles: peak outstanding must stay O(worker_count), NOT grow
  // to the tile count. Asserted as a counter, never against wall-clock or directory size.
  Scene scene(textured(128, 128), /*edge=*/16);

  WorkerPoolConfig cfg;
  cfg.worker_count = k_workers;
  WorkerPool pool(cfg);
  int owner = 0;
  TileEncodeDispatch dispatch(pool, &owner);

  RasterTileStore tiles;
  MemorySink sink;
  SaveContext ctx{k_base};
  ctx.set_asset_sink(&sink);
  ctx.set_storage_format(PixelFormat::Rgba32fLinearPremul);
  const CodecTable codecs = builtin_codecs(scene.registry, &tiles, &dispatch);

  REQUIRE(save_document(scene.doc, scene.bridge, codecs, ctx).has_value());
  CHECK(sink.blobs_written() == 64); // all 64 distinct tiles written

  // The bound: peak in-flight never exceeds the window, and the window is O(worker_count)
  // (a small multiple), NOT the image's tile count.
  CHECK(dispatch.window() == 2 * k_workers);
  CHECK(dispatch.peak_in_flight() <= dispatch.window());
  CHECK(dispatch.peak_in_flight() < 64); // genuinely windowed, not fire-all-then-join
  CHECK(dispatch.peak_in_flight() >= 1);

  // The INLINE executor's window is exactly 1 and its peak exactly 1 (the same algorithm,
  // one job at a time), which a default-constructed dispatch exercises directly.
  TileEncodeDispatch inline_dispatch;
  CHECK(inline_dispatch.window() == 1);
  Scene inline_scene(textured(128, 128), /*edge=*/16);
  RasterTileStore inline_tiles;
  MemorySink inline_sink;
  SaveContext inline_ctx{k_base};
  inline_ctx.set_asset_sink(&inline_sink);
  inline_ctx.set_storage_format(PixelFormat::Rgba32fLinearPremul);
  const CodecTable inline_codecs =
      builtin_codecs(inline_scene.registry, &inline_tiles, &inline_dispatch);
  REQUIRE(
      save_document(inline_scene.doc, inline_scene.bridge, inline_codecs, inline_ctx).has_value());
  CHECK(inline_dispatch.peak_in_flight() == 1);
}

TEST_CASE("a mid-fan-out sink failure aborts the save and drains its workers") {
  // A worker-backed save whose sink refuses writes: the reap turns the first failed write
  // into an AssetWriteFailed error and the dispatch DRAINS its outstanding jobs before
  // returning, so no worker outlives a reference to a freed buffer (Constraint 7). ASan/TSan
  // catch a botched drain; the assertion here is that the error surfaces and the pool stays
  // usable.
  Scene scene(textured(128, 128), /*edge=*/16);

  WorkerPoolConfig cfg;
  cfg.worker_count = k_workers;
  WorkerPool pool(cfg);
  int owner = 0;
  TileEncodeDispatch dispatch(pool, &owner);

  RasterTileStore tiles;
  MemorySink sink;
  sink.fail_writes(); // every put fails
  SaveContext ctx{k_base};
  ctx.set_asset_sink(&sink);
  ctx.set_storage_format(PixelFormat::Rgba32fLinearPremul);
  const CodecTable codecs = builtin_codecs(scene.registry, &tiles, &dispatch);

  const expected<std::string, SerializeError> saved =
      save_document(scene.doc, scene.bridge, codecs, ctx);
  REQUIRE_FALSE(saved.has_value());
  CHECK(saved.error().kind == SerializeError::Kind::AssetWriteFailed);

  // The pool survived the drain and still runs work: a fresh save through the SAME dispatch,
  // against a sink that accepts writes, succeeds.
  MemorySink ok_sink;
  SaveContext ok_ctx{k_base};
  ok_ctx.set_asset_sink(&ok_sink);
  ok_ctx.set_storage_format(PixelFormat::Rgba32fLinearPremul);
  Scene ok_scene(textured(64, 64), /*edge=*/16);
  RasterTileStore ok_tiles;
  const CodecTable ok_codecs = builtin_codecs(ok_scene.registry, &ok_tiles, &dispatch);
  REQUIRE(save_document(ok_scene.doc, ok_scene.bridge, ok_codecs, ok_ctx).has_value());
  CHECK(ok_sink.blobs_written() == 16);
}
