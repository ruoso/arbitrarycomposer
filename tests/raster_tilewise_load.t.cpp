// A RASTER LOAD'S TRANSIENT PEAK IS BOUNDED BY ONE TILE (kinds.raster_tilewise_load; doc 15
// "Reconstructing a tiled payload is tilewise").
//
// The promise this file exists to falsify is a MEMORY promise, and a memory promise with no
// enforcing test is a comment. The route it replaced -- scatter every decoded blob into a
// dense `w * h * 16` working buffer, hand that to `RasterContent`, let it re-tile the buffer
// straight back into the pool -- was "structurally obvious" too, in the other direction: it
// was a `std::vector` whose size nobody asserted, and it cost 384 MB of spare copy to open a
// 24 MP layer.
//
// SO THE ASSERTION IS A BEHAVIORAL COUNTER, NEVER A WALL-CLOCK AND NEVER AN RSS SAMPLE (doc
// 16 tier 4). This TU replaces the global `operator new` / `operator delete` with a counting
// pair (it gets its own executable target, as every `tests/*.t.cpp` does, so nothing else in
// the suite is measured through it) and loads THE SAME DOCUMENT GEOMETRY at two tile counts:
// a 4x4 grid and a 16x16 grid at the same tile edge -- a 16x IMAGE-AREA increase. Both the
// largest single heap allocation on the load path and the transient high-water must stay
// inside ONE TILE BLOB plus a fixed slack, at BOTH sizes. The dense buffer fails both
// outright: at the larger size it IS a single 16 MiB allocation.
//
// The resident cost is a different question and is legitimately O(image): the tile table the
// load is genuinely reading. The pool's `ChunkSource` reservations and the table's index
// slabs are therefore SUBTRACTED explicitly rather than assumed to bypass `operator new` --
// the number under assertion is the TRANSIENT one.
//
// The second test is the other half of the route change (Decision 3): a persisted blob's
// bytes now land in the pool blob VERBATIM, padding included. Today's dense route dropped the
// padding and then seeded the hash memo with the original blob's name anyway -- for a foreign
// writer's non-zero padding that memo entry was a LIE, and the next save would have published
// the stale name over different bytes. The blob NAME is the golden that pins it.

#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/contract/registry.hpp>
#include <arbc/kind_raster/raster_content.hpp>
#include <arbc/media/pixel_format.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/model/model.hpp>
#include <arbc/pool/big_block_pool.hpp>
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

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <set>
#include <string>
#include <vector>

using namespace arbc;
using json = nlohmann::json;

// --- the counting global allocator -----------------------------------------------------
//
// Every tracked block carries a header holding its size and whether it was counted, so an
// unsized `operator delete` can still decrement the right amount, and a block allocated
// BEFORE tracking was armed never decrements a counter it never incremented. The ALIGNED
// `operator new`/`delete` overloads are deliberately NOT replaced: they are a separate
// replaceable family, so an over-aligned allocation goes default-new/default-delete
// consistently and simply stays untracked (which is also how the pool's page-aligned chunks
// escape this counter on the non-mmap fallback -- and why the pool's reservations are
// subtracted explicitly below rather than assumed away).

namespace {

struct AllocHeader {
  std::size_t size;
  std::size_t tracked;
};
constexpr std::size_t k_hdr = (sizeof(AllocHeader) + __STDCPP_DEFAULT_NEW_ALIGNMENT__ - 1) /
                              __STDCPP_DEFAULT_NEW_ALIGNMENT__ * __STDCPP_DEFAULT_NEW_ALIGNMENT__;

std::atomic<bool> g_arm{false};
std::atomic<std::size_t> g_live{0};
std::atomic<std::size_t> g_peak{0};
std::atomic<std::size_t> g_largest{0};

void* count_alloc(std::size_t n) noexcept {
  auto* raw = static_cast<std::byte*>(std::malloc(n + k_hdr));
  if (raw == nullptr) {
    return nullptr;
  }
  auto* hdr = reinterpret_cast<AllocHeader*>(raw);
  hdr->size = n;
  hdr->tracked = 0;
  if (g_arm.load(std::memory_order_relaxed)) {
    hdr->tracked = 1;
    const std::size_t live = g_live.fetch_add(n, std::memory_order_relaxed) + n;
    std::size_t peak = g_peak.load(std::memory_order_relaxed);
    while (live > peak && !g_peak.compare_exchange_weak(peak, live, std::memory_order_relaxed)) {
    }
    std::size_t largest = g_largest.load(std::memory_order_relaxed);
    while (n > largest && !g_largest.compare_exchange_weak(largest, n, std::memory_order_relaxed)) {
    }
  }
  return raw + k_hdr;
}

void count_free(void* p) noexcept {
  if (p == nullptr) {
    return;
  }
  auto* raw = static_cast<std::byte*>(p) - k_hdr;
  const auto* hdr = reinterpret_cast<const AllocHeader*>(raw);
  if (hdr->tracked != 0) {
    g_live.fetch_sub(hdr->size, std::memory_order_relaxed);
  }
  std::free(raw);
}

// Arm the counters around one measured region, and report the transient numbers.
struct HeapProbe {
  HeapProbe() {
    g_live.store(0, std::memory_order_relaxed);
    g_peak.store(0, std::memory_order_relaxed);
    g_largest.store(0, std::memory_order_relaxed);
    g_arm.store(true, std::memory_order_relaxed);
  }
  ~HeapProbe() { g_arm.store(false, std::memory_order_relaxed); }
  HeapProbe(const HeapProbe&) = delete;
  HeapProbe& operator=(const HeapProbe&) = delete;

  void disarm() { g_arm.store(false, std::memory_order_relaxed); }
  std::size_t peak() const { return g_peak.load(std::memory_order_relaxed); }
  std::size_t largest() const { return g_largest.load(std::memory_order_relaxed); }
};

} // namespace

void* operator new(std::size_t n) {
  void* p = count_alloc(n);
  if (p == nullptr) {
    throw std::bad_alloc();
  }
  return p;
}
void* operator new[](std::size_t n) {
  void* p = count_alloc(n);
  if (p == nullptr) {
    throw std::bad_alloc();
  }
  return p;
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return count_alloc(n); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return count_alloc(n); }
void operator delete(void* p) noexcept { count_free(p); }
void operator delete[](void* p) noexcept { count_free(p); }
void operator delete(void* p, std::size_t) noexcept { count_free(p); }
void operator delete[](void* p, std::size_t) noexcept { count_free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { count_free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { count_free(p); }

// --- the fixture ------------------------------------------------------------------------

namespace {

// The tile edge every case here uses. `blob_bytes(64)` is 64 KiB -- a page-exact pool size
// class -- so ONE TILE BLOB is 65 536 bytes at both grid sizes, and the bound below is the
// same constant whether the image is 256x256 or 1024x1024.
constexpr int k_edge = 64;
constexpr std::size_t k_blob_bytes = static_cast<std::size_t>(k_edge) *
                                     static_cast<std::size_t>(k_edge) * k_tile_channels *
                                     sizeof(float);

// One tile blob, plus a fixed slack that does NOT scale with the image: the per-tile decode
// temporaries (the fetched frame, the decompressed storage bytes, the unshuffled working
// floats -- each O(tile), each scoped to a single fill invocation) plus the document JSON and
// the tile table's index slabs. It is deliberately generous: the number it has to separate
// is not marginal. The dense route's buffer is 1 MiB at the SMALL grid and 16 MiB at the
// large one -- both over this, and the larger one by 25x.
constexpr std::size_t k_transient_bound = k_blob_bytes + (512U * 1024U);

class ProjectDir {
public:
  explicit ProjectDir(const std::string& name)
      : d_root(std::filesystem::temp_directory_path() / ("arbc_tilewise_" + name)) {
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
  const std::filesystem::path& root() const { return d_root; }

  // Drop a hand-crafted blob frame into the fan-out directory the store derives.
  void write_blob(const std::string& hash, const std::vector<std::byte>& frame) const {
    const std::filesystem::path dir = d_root / "assets" / "tiles" / hash.substr(0, 2);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream out(dir / hash, std::ios::binary);
    out.write(reinterpret_cast<const char*>(frame.data()),
              static_cast<std::streamsize>(frame.size()));
    REQUIRE(out.good());
  }

private:
  std::filesystem::path d_root;
};

float sample_at(int x, int y, std::size_t c) {
  const std::uint32_t k =
      (static_cast<std::uint32_t>(y) * 7919U + static_cast<std::uint32_t>(x)) * 4U +
      static_cast<std::uint32_t>(c);
  const std::uint32_t s = k * 2654435761U;
  return static_cast<float>(s >> 8U) / static_cast<float>(1U << 24U);
}

DecodedImage textured(int w, int h) {
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
          c] = sample_at(x, y, c);
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

RasterContent* only_raster(Document& doc) {
  RasterContent* found = nullptr;
  doc.for_each_content([&found](ObjectId, Content* c) {
    if (auto* r = dynamic_cast<RasterContent*>(c); r != nullptr) {
      found = r;
    }
  });
  return found;
}

json params_of(const std::string& bytes) {
  return json::parse(bytes).at("composition").at("layers").at(0).at("params");
}

// The tile table's own index memory: the `BlockSlotRef` slabs, which are legitimately
// O(tiles) and RESIDENT -- the load is genuinely reading them, so they are not part of the
// transient peak under assertion.
std::size_t table_index_bytes(const TileTable& table) {
  std::size_t n = 0;
  for (const Level& lvl : table.levels()) {
    n += lvl.tiles.size() * sizeof(BlockSlotRef);
  }
  return n;
}

// Measure one load of a document with a `tiles_x * tiles_y` grid at `k_edge`, and return the
// transient peak and the largest single allocation on the load path.
struct LoadPeak {
  std::size_t transient;
  std::size_t largest;
};

LoadPeak measure_load(const std::string& name, int tiles_side) {
  const int side = tiles_side * k_edge;
  ProjectDir project(name);
  Scene scene(textured(side, side), k_edge);

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

  Document loaded;
  KindBridge lbridge;
  RasterTileStore ltiles;

  std::size_t peak = 0;
  std::size_t largest = 0;
  std::size_t pool_reserved = 0;
  std::size_t index_bytes = 0;
  {
    HeapProbe probe;
    const auto ok = load_document(*saved, loaded, lbridge, scene.registry, project.base_uri(),
                                  &source, &ltiles);
    probe.disarm();
    REQUIRE(ok.has_value());
    peak = probe.peak();
    largest = probe.largest();
  }

  RasterContent* const back = only_raster(loaded);
  REQUIRE(back != nullptr);
  const TileTablePtr table = back->store().base_table();
  REQUIRE(table);
  // Sanity: the load really did build the grid we think it did.
  REQUIRE(table->level(0).tiles.size() ==
          static_cast<std::size_t>(tiles_side) * static_cast<std::size_t>(tiles_side));

  pool_reserved = back->store().pool().arena().total_bytes_reserved();
  index_bytes = table_index_bytes(*table);

  // The RESIDENT cost is subtracted, so what remains is the TRANSIENT peak: the pool's
  // ChunkSource reservations (the tile blobs themselves -- O(image), and the document) and
  // the table's index slabs. Saturating, because on the mmap-backed anonymous source the
  // pool's bytes never went through `operator new` at all and were never counted.
  const std::size_t resident = pool_reserved + index_bytes;
  return LoadPeak{peak > resident ? peak - resident : 0, largest};
}

} // namespace

// THE CLAIM. A load's transient peak is O(tile), invariant in tile count -- not O(image).
//
// enforces: 15-memory-model#raster-load-is-tilewise
TEST_CASE("a raster load's transient heap peak is bounded by one tile, at any image size") {
  // The same geometry at two tile counts: 4x4 => 256x256 px, and 16x16 => 1024x1024 px. A 16x
  // image-area increase, and therefore a 16x dense buffer: 1 MiB, then 16 MiB.
  const LoadPeak small = measure_load("small", 4);
  const LoadPeak large = measure_load("large", 16);

  INFO("small: transient=" << small.transient << " largest=" << small.largest);
  INFO("large: transient=" << large.transient << " largest=" << large.largest);

  // (a) NO SINGLE ALLOCATION ON THE LOAD PATH EXCEEDS ONE TILE BLOB (+ slack). This is the
  // assertion the dense buffer fails outright -- it was one contiguous `w * h * 16` vector,
  // 16 MiB at the larger grid.
  CHECK(small.largest <= k_transient_bound);
  CHECK(large.largest <= k_transient_bound);

  // (b) THE TRANSIENT HIGH-WATER IS INVARIANT IN TILE COUNT. Sixteen times the image area,
  // and the same fixed bound holds -- because exactly one tile's pixels are live at a time.
  CHECK(small.transient <= k_transient_bound);
  CHECK(large.transient <= k_transient_bound);

  // ...and it is invariant, not merely bounded: the 16x area buys no meaningful growth. (The
  // slack absorbs the genuinely O(tiles) scaffolding -- 256 hash strings in the `blobs` array
  // -- which is why this is a bound and not an equality.)
  CHECK(large.transient <= small.transient + k_blob_bytes + (256U * 1024U));
}

// A FOREIGN BLOB'S PADDING SURVIVES THE LOAD (Decision 3), and the blob NAME is the golden.
//
// The dense route dropped each tile's right/bottom padding on the way through the working
// buffer and then seeded the memo with the ORIGINAL blob's name regardless. For our own
// writer that is harmless -- `new_blob` zeroes and `paint` never writes outside
// `width`/`height`, so our padding is always zero. For a blob some OTHER writer produced, the
// memo would assert a name the pool slot no longer hashes to, and the next save would publish
// that name over different bytes. The tilewise route copies the blob wholesale and retires
// the inconsistency structurally.
//
// enforces: 08-serialization#raster-tile-store-round-trips-byte-exactly
TEST_CASE("a tile blob's padding bytes survive the load, and re-save reproduces its name") {
  ProjectDir project("padding");
  // 96x96 at edge 64 => a 2x2 grid whose right and bottom tiles are 32 px of PADDING.
  constexpr int k_w = 96;
  constexpr int k_h = 96;
  Scene scene(textured(k_w, k_h), k_edge);

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
  const json zero_params = params_of(*saved);
  REQUIRE(zero_params.at("blobs").size() == 4U);

  // Hand-craft the SAME four tiles with NON-ZERO padding: in-bounds samples untouched, every
  // sample outside `width`/`height` set to a value our own writer would never emit. Each gets
  // a new name, because the padding is inside the hash.
  const std::size_t samples =
      static_cast<std::size_t>(k_edge) * static_cast<std::size_t>(k_edge) * k_tile_channels;
  json padded_doc = json::parse(*saved);
  json& padded_blobs = padded_doc.at("composition").at("layers").at(0).at("params").at("blobs");
  for (std::size_t t = 0; t < 4U; ++t) {
    const int tx = static_cast<int>(t % 2U);
    const int ty = static_cast<int>(t / 2U);
    std::vector<float> working(samples, 0.0F);
    for (int iy = 0; iy < k_edge; ++iy) {
      for (int ix = 0; ix < k_edge; ++ix) {
        const int gx = tx * k_edge + ix;
        const int gy = ty * k_edge + iy;
        const std::size_t o = (static_cast<std::size_t>(iy) * static_cast<std::size_t>(k_edge) +
                               static_cast<std::size_t>(ix)) *
                              k_tile_channels;
        for (std::size_t c = 0; c < k_tile_channels; ++c) {
          // In bounds: the document's real pixels. Out of bounds: PADDING our writer would
          // have zeroed, and a foreign writer plainly did not.
          working[o + c] = (gx < k_w && gy < k_h) ? sample_at(gx, gy, c) : 0.5F;
        }
      }
    }
    const std::vector<std::byte> storage =
        to_storage_bytes(working, PixelFormat::Rgba32fLinearPremul);
    const std::string hash = hash_tile(storage);
    const expected<std::vector<std::byte>, TileBlobError> frame =
        frame_tile_blob(storage, PixelFormat::Rgba32fLinearPremul);
    REQUIRE(frame.has_value());
    project.write_blob(hash, *frame);
    padded_blobs[t] = hash;
  }

  // The padded blobs really ARE different blobs -- otherwise this test proves nothing. Tile 0
  // is the control: at 96x96/edge-64 it is entirely IN BOUNDS, so it has no padding to differ
  // in and must keep its original name. Tiles 1..3 each carry padding and must not.
  const std::set<std::string> zero_names(zero_params.at("blobs").begin(),
                                         zero_params.at("blobs").end());
  REQUIRE(padded_blobs[0] == zero_params.at("blobs")[0]);
  for (std::size_t t = 1; t < 4U; ++t) {
    REQUIRE(zero_names.count(padded_blobs[t].get<std::string>()) == 0U);
  }

  // (a) THE PADDING IS UNOBSERVABLE IN OUTPUT. Every read clamps to `[0,w-1] x [0,h-1]`
  // (`TileTable::pixel` / the decimation kernel), so no rendered pixel and no mip rung can
  // see it -- the loaded pyramid is identical to the zero-padded document's, rung for rung.
  // Hence no golden moves, asserted rather than assumed.
  Document zero_doc;
  KindBridge zero_bridge;
  RasterTileStore zero_memo;
  REQUIRE(load_document(*saved, zero_doc, zero_bridge, scene.registry, project.base_uri(), &source,
                        &zero_memo)
              .has_value());

  Document pad_doc;
  KindBridge pad_bridge;
  RasterTileStore pad_memo;
  REQUIRE(load_document(padded_doc.dump(), pad_doc, pad_bridge, scene.registry, project.base_uri(),
                        &source, &pad_memo)
              .has_value());

  RasterContent* const zero_raster = only_raster(zero_doc);
  RasterContent* const pad_raster = only_raster(pad_doc);
  REQUIRE(zero_raster != nullptr);
  REQUIRE(pad_raster != nullptr);
  const TileTablePtr zt = zero_raster->store().base_table();
  const TileTablePtr pt = pad_raster->store().base_table();
  REQUIRE(zt);
  REQUIRE(pt);
  REQUIRE(zt->level_count() == pt->level_count());
  for (std::size_t l = 0; l < zt->level_count(); ++l) {
    CHECK(zt->level_pixels(l) == pt->level_pixels(l));
  }

  // (b) THE BLOB NAME IS REPRODUCED. Re-save the padded document through a FRESH memo -- so
  // every tile is genuinely re-hashed from the pool bytes, with no seeded name to hide behind
  // -- and the names that come back are the PADDED ones, byte for byte. The dense route could
  // not do this: it re-zeroed the padding into the pool, so a fresh hash of its slots yields
  // the ZERO-padded names, and it would write four "new" blobs to prove it.
  RasterTileStore resave_memo;
  FilesystemAssetSink resave_sink;
  SaveContext resave_ctx{project.base_uri()};
  resave_ctx.set_asset_sink(&resave_sink);
  resave_ctx.set_storage_format(PixelFormat::Rgba32fLinearPremul);
  const CodecTable resave_codecs = builtin_codecs(scene.registry, &resave_memo);
  const expected<std::string, SerializeError> resaved =
      save_document(pad_doc, pad_bridge, resave_codecs, resave_ctx);
  REQUIRE(resaved.has_value());

  const json resaved_params = params_of(*resaved);
  CHECK(resaved_params.at("blobs") == padded_blobs);
  CHECK(resave_memo.tiles_hashed() == 4U);  // the fresh memo really did hash all four
  CHECK(resave_sink.blobs_written() == 0U); // ...and every name was already on disk
}
