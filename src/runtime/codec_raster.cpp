// The org.arbc.raster built-in codec (serialize.raster_tile_store Decision 2). THE
// CENTRAL RULE, which a simpler implementation would quietly violate:
//
//     PERSIST THE TILE TABLE, NOT THE IMAGE.
//
// A painted layer's pixels are document state with no source file (doc 08 Principle 8),
// so unlike an imported `org.arbc.image` they cannot serialize as a URI -- they have to
// BE in the document's asset directory. The save therefore walks the level-0
// `BlockSlotRef` grid, `peek()`s each blob, and encodes each tile INDEPENDENTLY. It never
// materializes a dense pixel buffer of the layer and writes that: a dense flatten throws
// away exactly the sparsity and sharing that make the store small, and -- this is the
// trap -- it would pass a naive round-trip test perfectly. The content-addressing
// COUNTERS are the guard, not the round-trip.
//
// Identical tiles collapse to one blob, so dedup falls out across layers and across undo
// versions, and INCREMENTAL SAVE falls out as a consequence: a save writes only the blobs
// not already on disk under that name. Mip levels are not persisted; a load rebuilds them
// through `RasterContent`'s existing `DecodedImage` constructor, which is already
// claim-proven byte-identical to the incremental recompute
// (`14-data-model-and-editing#paint-mip-recompute-matches-full-rebuild`).
//
// THE LEVELIZATION LINE (doc 17, CI-enforced). `serialize` (L4) may not include
// `kind_raster` (L4), and vice versa -- no same-level edges. So `arbc::serialize` owns
// the FORMAT over a byte-oriented API naming no raster type (`tile_blob.hpp`: hash,
// shuffle, storage conversion, framing, geometry validation), and THIS TU -- `runtime`,
// L5 -- owns the CODEC, the only place that may see both `RasterContent` and
// `nlohmann::json`. `check_levels.py` already permits both edges; no `ALLOWED` entry is
// widened, and if an implementer finds themselves editing it, this split has been broken.
//
// THE CORE WRITES ASSET BYTES; THE KIND ONLY ENCODES THEM (doc 08 Principle 3, applied
// symmetrically -- Decision 1). This TU does not open, create, or rename a file. It hands
// finished blob frames to the `SaveContext`'s `AssetSink`.

#include <arbc/kind_raster/raster_content.hpp>
#include <arbc/runtime/builtin_codecs.hpp>
#include <arbc/runtime/raster_tile_store.hpp>
#include <arbc/serialize/tile_blob.hpp>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace arbc {
namespace {

using json = nlohmann::json;

// The authored base URI of the blob store, as doc 08's example has it. It goes through
// the same `LoadContext::resolve` seam nested projects use, which is what keeps a project
// directory relocatable (doc 08:34-36).
constexpr const char* k_default_tiles_base = "assets/tiles/";

unexpected<ReaderError> read_fail(ReaderError::Kind kind, std::string path) {
  return unexpected(ReaderError{kind, std::move(path), ObjectId{}});
}

unexpected<SerializeError> write_fail(SerializeError::Kind kind) {
  return unexpected(SerializeError{kind, ObjectId{}});
}

// The pool's counts are atomic and `retain`/`resolve` are documented "any thread", so
// taking a count is not a mutation of the pool's LOGICAL state -- but they are spelled
// non-const, and `SerializeFn` hands us a `const Content&`. The alternative would be to
// widen `BigBlockPool` (L1) to serve one consumer, which is out of all proportion
// (Decision 5, rejected alternative).
BigBlockPool& pool_of(const RasterContent& raster) {
  return const_cast<RasterStore&>(raster.store()).pool();
}

// --- SAVE -------------------------------------------------------------------------

expected<json, SerializeError> serialize_raster(const Content& content, SaveContext& ctx,
                                                RasterTileStore* tiles) {
  const auto* const raster = dynamic_cast<const RasterContent*>(&content);
  if (raster == nullptr) {
    return write_fail(SerializeError::Kind::CodecFailed);
  }

  // A save that would DROP PIXELS is an error, never a silent success (Constraint 5).
  // Checked BEFORE any encoding, so a sink-less save costs nothing and fails honestly.
  // Existing sink-less call sites keep working: a document with no raster layer never
  // reaches this codec at all.
  if (ctx.asset_sink() == nullptr && !ctx.params_only()) {
    return write_fail(SerializeError::Kind::AssetSinkMissing);
  }

  // The store's current base version, pinned by the returned `shared_ptr` -- which holds
  // a pool count on every slot it names, so every `peek()` below is valid for the whole
  // save even as the writer thread paints on and drops versions concurrently
  // (Constraint 10; `RasterStore::base_table()` takes the store's mutex).
  const TileTablePtr table = raster->store().base_table();
  if (!table || table->level_count() == 0) {
    return write_fail(SerializeError::Kind::CodecFailed);
  }

  const PixelFormat storage = ctx.storage_format();
  BigBlockPool& pool = pool_of(*raster);

  // LEVEL 0 ONLY. Mips are derived, and no blob may appear in the asset directory for any
  // level above 0 (Constraint 8 -- asserted, not assumed).
  const Level& level0 = table->level(0);

  RasterTileStore local; // the non-memoizing fallback: correct, just not incremental
  RasterTileStore& memo = (tiles != nullptr) ? *tiles : local;
  memo.begin_pass(storage);

  json blobs = json::array();
  std::set<std::string> seen; // distinct blobs THIS save has already dealt with
  for (const BlockSlotRef& ref : level0.tiles) {
    // The memo is keyed by slot and PINNED BY A REFCOUNT (Decision 5). An untouched tile
    // keeps its `BlockSlotRef` identity across a CoW paint, so this is a hit and costs no
    // hash at all -- which is what makes a re-save after one dab O(dab) and not
    // O(document).
    const std::string hash = memo.hash_of(pool, ref);
    blobs.push_back(hash);

    if (!seen.insert(hash).second) {
      continue; // an identical tile, already handled: ONE blob, however many slots
    }
    const std::string uri = tile_blob_uri(k_default_tiles_base, hash);
    if (ctx.has_asset(uri)) {
      continue; // already on disk under this name; content-addressed, so it IS this tile
    }

    // Only now -- for a tile that is genuinely new to the store -- do we pay to produce
    // bytes: storage-convert, shuffle, compress. shuffle -> compress -> hash-named file,
    // exactly the composition `blob_compress.hpp` prescribed when it deferred to us.
    const std::span<const std::byte> raw = pool.peek(ref);
    const std::span<const float> working{reinterpret_cast<const float*>(raw.data()),
                                         raw.size() / sizeof(float)};
    const std::vector<std::byte> storage_bytes = to_storage_bytes(working, storage);
    const expected<std::vector<std::byte>, TileBlobError> frame =
        frame_tile_blob(storage_bytes, storage);
    if (!frame) {
      return write_fail(SerializeError::Kind::CodecFailed);
    }
    const expected<bool, AssetSinkError> stored = ctx.store_asset(uri, *frame);
    if (!stored) {
      return write_fail(stored.error().kind == AssetSinkError::Kind::NoSink
                            ? SerializeError::Kind::AssetSinkMissing
                            : SerializeError::Kind::AssetWriteFailed);
    }
  }
  memo.end_pass();

  json params = json::object();
  params["tiles"] = k_default_tiles_base;
  params["edge"] = static_cast<std::int64_t>(table->edge());
  params["width"] = static_cast<std::int64_t>(table->width());
  params["height"] = static_cast<std::int64_t>(table->height());
  params["blobs"] = std::move(blobs);
  return params;
}

// --- LOAD -------------------------------------------------------------------------

// A required integer `params` key. A hostile document's `edge` may be a string, a float,
// or 2^40; every one of those has to come back as a value BEFORE it sizes an allocation
// (Constraint 7), which is why this yields an int64 for `validate_tile_geometry` to bound
// rather than an already-narrowed `int`.
expected<std::int64_t, ReaderError> read_int(const json& params, const char* key) {
  const auto it = params.find(key);
  if (it == params.end()) {
    return read_fail(ReaderError::Kind::MissingRequiredField, std::string("/params/") + key);
  }
  if (!it->is_number_integer()) {
    return read_fail(ReaderError::Kind::MalformedField, std::string("/params/") + key);
  }
  return it->get<std::int64_t>();
}

expected<std::unique_ptr<Content>, ReaderError>
deserialize_raster(const json& params, LoadContext& ctx, RasterTileStore* tiles) {
  // A present-but-mistyped `tiles` is treated LENIENTLY AS ABSENT and rides the
  // params_residual diff back out verbatim, the idiom every sibling codec uses.
  std::string base = k_default_tiles_base;
  if (const auto it = params.find("tiles"); it != params.end() && it->is_string()) {
    base = it->get<std::string>();
  }

  const expected<std::int64_t, ReaderError> edge = read_int(params, "edge");
  if (!edge) {
    return unexpected(edge.error());
  }
  const expected<std::int64_t, ReaderError> width = read_int(params, "width");
  if (!width) {
    return unexpected(width.error());
  }
  const expected<std::int64_t, ReaderError> height = read_int(params, "height");
  if (!height) {
    return unexpected(height.error());
  }

  // UNTRUSTED GEOMETRY IS VALIDATED BEFORE IT IS USED AS AN ALLOCATION BOUND
  // (Constraint 7). `zstd_dep` bounded decompression by the caller's `expected_size`;
  // that size is now derived from numbers a hostile document supplied, so this is the
  // extension of doc 08:440-442's promise from the frame header to the tile table.
  // NOTHING below this line allocates from unchecked numbers.
  const expected<TileGeometry, TileBlobError> geom =
      validate_tile_geometry(*edge, *width, *height);
  if (!geom) {
    return read_fail(ReaderError::Kind::MalformedField, "/params/edge");
  }

  const auto bit = params.find("blobs");
  if (bit == params.end()) {
    return read_fail(ReaderError::Kind::MissingRequiredField, "/params/blobs");
  }
  if (!bit->is_array() || bit->size() != geom->tile_count()) {
    // A `blobs` array whose length is not exactly tiles_x * tiles_y is a lie about the
    // geometry, and one we must not paper over by padding or truncating.
    return read_fail(ReaderError::Kind::MalformedField, "/params/blobs");
  }

  const PixelFormat storage = ctx.storage_format();
  const std::size_t samples = geom->tile_samples();

  // The dense working buffer the existing `RasterContent` constructor takes (Decision 6):
  // correctness through the already-proven path first -- which is also what makes the mip
  // rebuild byte-identical for free. Its honest cost is a transient w*h*16 buffer (384 MB
  // for a 24 MP layer), and that cost is why `kinds.raster_tilewise_load` is a NAMED
  // follow-up rather than a shrug.
  DecodedImage image;
  image.width = geom->width;
  image.height = geom->height;
  image.format = k_working_rgba32f;
  image.bytes.assign(static_cast<std::size_t>(geom->width) *
                         static_cast<std::size_t>(geom->height) * k_tile_channels * sizeof(float),
                     std::byte{0});
  auto* const dense = reinterpret_cast<float*>(image.bytes.data());

  for (std::size_t t = 0; t < geom->tile_count(); ++t) {
    const json& entry = (*bit)[t];
    if (!entry.is_string()) {
      return read_fail(ReaderError::Kind::MalformedField, "/params/blobs");
    }
    const std::string hash = entry.get<std::string>();
    if (!is_tile_hash(hash)) {
      return read_fail(ReaderError::Kind::MalformedField, "/params/blobs");
    }

    // THE CORE FETCHES ASSET BYTES; THE KIND ONLY DECODES THEM (doc 08 Principle 3). The
    // fetch goes through the same `LoadContext` seam an external nested project uses.
    const ResolvedRef ref = ctx.resolve(tile_blob_uri(base, hash));
    auto frame = std::make_shared<std::string>();
    ctx.load_asset(ref, [frame](std::string_view bytes) { frame->assign(bytes); });
    if (frame->empty()) {
      // Tile blobs load SYNCHRONOUSLY; there is no pending state (Decision 7). An imported
      // image has a source file that may be slow, missing, or remote, and a sensible "not
      // yet" rendering. Painted pixels have none of that: they are document state, the
      // asset directory is a sibling of the `.arbc` by construction, and a raster layer
      // whose tiles have not arrived is not a layer in a pending state -- it is a BROKEN
      // DOCUMENT.
      return read_fail(ReaderError::Kind::UnresolvableReference, "/params/blobs");
    }

    const std::span<const std::byte> frame_bytes{
        reinterpret_cast<const std::byte*>(frame->data()), frame->size()};
    const expected<std::vector<float>, TileBlobError> pixels =
        decode_tile_blob(frame_bytes, hash, storage, samples);
    if (!pixels) {
      // A blob that does not hash to the name it was fetched under -- a truncated file, a
      // bit-flipped frame, a substituted blob -- is a ReaderError. Never a crash, never
      // silent wrong pixels (doc 08 Principle 8: the blob is self-verifying).
      return read_fail(ReaderError::Kind::MalformedField, "/params/blobs");
    }

    // Scatter the tile into the dense buffer, dropping the tile's right/bottom padding.
    const std::size_t tx = t % static_cast<std::size_t>(geom->tiles_x);
    const std::size_t ty = t / static_cast<std::size_t>(geom->tiles_x);
    for (int iy = 0; iy < geom->edge; ++iy) {
      const std::size_t gy = ty * static_cast<std::size_t>(geom->edge) +
                             static_cast<std::size_t>(iy);
      if (gy >= static_cast<std::size_t>(geom->height)) {
        break;
      }
      for (int ix = 0; ix < geom->edge; ++ix) {
        const std::size_t gx = tx * static_cast<std::size_t>(geom->edge) +
                               static_cast<std::size_t>(ix);
        if (gx >= static_cast<std::size_t>(geom->width)) {
          break;
        }
        const std::size_t src = (static_cast<std::size_t>(iy) *
                                     static_cast<std::size_t>(geom->edge) +
                                 static_cast<std::size_t>(ix)) *
                                k_tile_channels;
        const std::size_t dst =
            (gy * static_cast<std::size_t>(geom->width) + gx) * k_tile_channels;
        for (std::size_t c = 0; c < k_tile_channels; ++c) {
          dense[dst + c] = (*pixels)[src + c];
        }
      }
    }
  }

  auto content = std::make_unique<RasterContent>(std::move(image), geom->edge);

  // SEED THE MEMO with the tiles we just decoded: we already know each one's name, so
  // re-hashing it would be pure waste -- and it is what makes both the reader's
  // params-only residual re-serialize and the FIRST save after a load a pure memo sweep.
  //
  // Note the level-0 grid we seed against is the one `RasterStore::build` just minted, so
  // its slots are fresh: two identical tiles in the document occupy two distinct slots and
  // both map to the same hash, which is exactly right -- the dedup lives in the NAME, not
  // in the pool.
  if (tiles != nullptr) {
    if (const TileTablePtr built = content->store().base_table(); built && built->level_count() > 0) {
      const Level& level0 = built->level(0);
      BigBlockPool& pool = content->store().pool();
      for (std::size_t t = 0; t < level0.tiles.size() && t < geom->tile_count(); ++t) {
        tiles->seed(pool, level0.tiles[t], storage, (*bit)[t].get<std::string>());
      }
    }
  }
  return std::unique_ptr<Content>(std::move(content));
}

} // namespace

Codec raster_codec() { return raster_codec(nullptr); }

Codec raster_codec(RasterTileStore* tiles) {
  return Codec{
      [tiles](const Content& content, SaveContext& ctx) -> expected<json, SerializeError> {
        return serialize_raster(content, ctx, tiles);
      },
      [tiles](const json& params, std::span<const ContentRef> /*inputs*/,
              ObjectId /*composition*/,
              LoadContext& ctx) -> expected<std::unique_ptr<Content>, ReaderError> {
        // Neither `inputs` nor `composition` is consumed: `org.arbc.raster` is a leaf
        // kind with an empty `inputs()` and no child composition.
        return deserialize_raster(params, ctx, tiles);
      }};
}

} // namespace arbc
