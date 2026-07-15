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
// through `RasterContent::from_tiles`, which decimates the pyramid with the very same
// (single) kernel the dense constructor does, and is therefore still byte-identical to
// the incremental recompute
// (`14-data-model-and-editing#paint-mip-recompute-matches-full-rebuild`).
//
// AND THE LOAD IS TILEWISE, symmetrically (doc 15; kinds.raster_tilewise_load). The read
// side used to scatter each decoded blob into a dense `w * h` working buffer and hand THAT
// to `RasterContent`, which immediately re-tiled it -- an O(image) transient (384 MB for a
// 24 MP layer) in the middle of the one code path whose entire purpose is not being
// O(image). It now streams: one blob fetched, verified, and written into one pool tile at
// a time, so the load's peak is O(tile).
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
#include <arbc/runtime/tile_encode_dispatch.hpp>
#include <arbc/serialize/tile_blob.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <unordered_map>
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
                                                RasterTileStore* tiles,
                                                TileEncodeDispatch* dispatch) {
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

  // The executor. A null `dispatch` means the shipped inline path -- byte-identical to the
  // single-threaded save (Constraint 5); the offline export defaults here. A host that
  // wants the encode fanned across pool workers hands in a worker-backed dispatch, and the
  // two produce bit-for-bit identical output (Constraint 1, Decision 3).
  TileEncodeDispatch inline_dispatch; // the default executor when none is injected
  TileEncodeDispatch& enc = (dispatch != nullptr) ? *dispatch : inline_dispatch;

  // --- Phase 1: classify every tile row-major, ON THE SAVE THREAD ---------------------
  // A HIT (an untouched, ref-shared tile: the common sparse case) is resolved for free by
  // the memo and its `blobs[i]` filled now. A MISS is a job: its distinct SLOT keys one
  // encode, and every later tile sharing that slot (two layers sharing a tile, or the
  // all-empty layer where every slot IS one slot) aliases it -- so a ref-shared layer
  // dispatches AT MOST ONE encode (Decision 2, the save-thread ref-level dedup). No
  // mutation leaves this thread; workers only `peek` and encode into their own buffer.
  const std::vector<BlockSlotRef>& grid = level0.tiles;
  std::vector<std::string> blob_names(grid.size());       // row-major, filled positionally
  std::vector<BlockSlotRef> job_refs;                     // one per distinct miss slot
  std::vector<std::vector<std::size_t>> job_tiles;        // job -> the tile indices aliasing it
  std::unordered_map<std::uint64_t, std::size_t> pending; // packed slot key -> job index

  for (std::size_t i = 0; i < grid.size(); ++i) {
    const BlockSlotRef ref = grid[i];
    // The slot key: the storage format is constant across the save, so (index, size) alone
    // distinguishes the pending-job map's slots (the memo carries the format in its own
    // key). Two tiles with this same key ARE the same pool slot.
    const std::uint64_t key = (static_cast<std::uint64_t>(ref.index()) << 32) | ref.size();
    if (const auto it = pending.find(key); it != pending.end()) {
      job_tiles[it->second].push_back(i); // an already-dispatched miss slot: alias it
    } else if (std::optional<std::string> hit = memo.probe(pool, ref)) {
      blob_names[i] = std::move(*hit); // a memo hit: no encode, no job
    } else {
      const std::size_t j = job_refs.size();
      pending.emplace(key, j);
      job_refs.push_back(ref);
      job_tiles.push_back(std::vector<std::size_t>{i});
    }
  }

  // --- Phase 2: fan the pure encode across the executor, reap BY INDEX on this thread --
  // The encode is a pure function of one `peek()`ed immutable tile (hash + shuffle+zstd
  // frame), safe to run on any worker. The reap -- the memo commit, the in-save/on-disk
  // dedup, the positional `blobs[i]`, the sink write -- runs only here, single-threaded,
  // exactly as the serial loop's stateful tail did (Constraint 2).
  std::set<std::string> seen; // distinct blobs THIS save has already dealt with
  std::optional<SerializeError::Kind> failure;

  const TileEncodeDispatch::EncodeFn encode = [&pool, storage,
                                               &job_refs](std::size_t j) -> TileEncodeOutput {
    const BlockSlotRef ref = job_refs[j];
    // `peek` is any-thread and takes no refcount (the pinned `TileTablePtr` keeps the slot
    // alive); the encode allocates only its OWN output buffer (Constraint 3).
    const std::span<const std::byte> raw = pool.peek(ref);
    const std::span<const float> working{reinterpret_cast<const float*>(raw.data()),
                                         raw.size() / sizeof(float)};
    const std::vector<std::byte> storage_bytes = to_storage_bytes(working, storage);
    TileEncodeOutput out;
    out.hash = hash_tile(storage_bytes);
    expected<std::vector<std::byte>, TileBlobError> frame = frame_tile_blob(storage_bytes, storage);
    if (frame) {
      out.frame = std::move(*frame);
    } else {
      out.error = frame.error(); // a value across the lane, not a throw (Constraint 7)
    }
    return out;
  };

  const TileEncodeDispatch::ReapFn reap = [&](std::size_t j, TileEncodeOutput& out) -> bool {
    if (out.error) {
      failure = SerializeError::Kind::CodecFailed;
      return false; // abort: the dispatch drains outstanding jobs before returning
    }
    const BlockSlotRef ref = job_refs[j];
    // Commit the miss into the memo (takes the pin, advances `tiles_hashed`) and fill every
    // tile aliasing this slot -- all on the save thread.
    memo.record(pool, ref, out.hash);
    for (const std::size_t i : job_tiles[j]) {
      blob_names[i] = out.hash;
    }
    if (!seen.insert(out.hash).second) {
      return true; // an identical tile, already handled: ONE blob, however many slots
    }
    const std::string uri = tile_blob_uri(k_default_tiles_base, out.hash);
    if (ctx.has_asset(uri)) {
      return true; // already on disk under this name; content-addressed, so it IS this tile
    }
    const expected<bool, AssetSinkError> stored = ctx.store_asset(uri, out.frame);
    if (!stored) {
      failure = stored.error().kind == AssetSinkError::Kind::NoSink
                    ? SerializeError::Kind::AssetSinkMissing
                    : SerializeError::Kind::AssetWriteFailed;
      return false;
    }
    return true;
  };

  enc.run(job_refs.size(), encode, reap);
  if (failure.has_value()) {
    return write_fail(*failure); // a failed save abandons the pass; `d_live` is untouched
  }
  memo.end_pass();

  json params = json::object();
  params["tiles"] = k_default_tiles_base;
  params["edge"] = static_cast<std::int64_t>(table->edge());
  params["width"] = static_cast<std::int64_t>(table->width());
  params["height"] = static_cast<std::int64_t>(table->height());
  params["blobs"] = std::move(blob_names);
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
  const expected<TileGeometry, TileBlobError> geom = validate_tile_geometry(*edge, *width, *height);
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

  // THE LOAD IS TILEWISE (doc 15 "Reconstructing a tiled payload is tilewise";
  // kinds.raster_tilewise_load). There is no dense `w * h * 16` working buffer here, and
  // there must not be one: the tile store exists because flattening a sparse tile table
  // into a dense image throws away exactly the sparsity that makes it small, and a reader
  // that flattens it back out on the way in gives that away transiently -- 384 MB for a
  // 24 MP layer, on top of the table it is legitimately building. `build_from_tiles` hands
  // this callback ONE pool blob's memory at a time; the fetched frame and the decoded
  // samples below are scoped to a single invocation, so the O(tile) peak is a consequence
  // of the CONTROL FLOW rather than of anyone's discipline.
  //
  // The failure a fill declines on is carried out here: `TileFill` is a bool, because
  // `kind_raster` (L4) may not name a `ReaderError` (contract's, but the seam stays
  // byte-oriented -- Constraint 6), and there is exactly one call site to thread it back
  // through.
  std::optional<ReaderError> failure;
  auto fill = [&](std::size_t t, std::span<float> dst) -> bool {
    const json& entry = (*bit)[t];
    if (!entry.is_string()) {
      failure = ReaderError{ReaderError::Kind::MalformedField, "/params/blobs", ObjectId{}};
      return false;
    }
    const std::string hash = entry.get<std::string>();
    if (!is_tile_hash(hash)) {
      failure = ReaderError{ReaderError::Kind::MalformedField, "/params/blobs", ObjectId{}};
      return false;
    }

    // THE CORE FETCHES ASSET BYTES; THE KIND ONLY DECODES THEM (doc 08 Principle 3). The
    // fetch goes through the same `LoadContext` seam an external nested project uses.
    const ResolvedRef ref = ctx.resolve(tile_blob_uri(base, hash));
    auto frame = std::make_shared<std::string>();
    ctx.load_asset(ref, [frame](std::string_view bytes) { frame->assign(bytes); });
    if (frame->empty()) {
      // Tile blobs load SYNCHRONOUSLY; there is no pending state (Decision 7, unamended by
      // this task -- streaming here buys MEMORY, not progressiveness). An imported image
      // has a source file that may be slow, missing, or remote, and a sensible "not yet"
      // rendering. Painted pixels have none of that: they are document state, the asset
      // directory is a sibling of the `.arbc` by construction, and a raster layer whose
      // tiles have not arrived is not a layer in a pending state -- it is a BROKEN
      // DOCUMENT.
      failure = ReaderError{ReaderError::Kind::UnresolvableReference, "/params/blobs", ObjectId{}};
      return false;
    }

    const std::span<const std::byte> frame_bytes{reinterpret_cast<const std::byte*>(frame->data()),
                                                 frame->size()};
    const expected<std::vector<float>, TileBlobError> pixels =
        decode_tile_blob(frame_bytes, hash, storage, samples);
    if (!pixels) {
      // A blob that does not hash to the name it was fetched under -- a truncated file, a
      // bit-flipped frame, a substituted blob -- is a ReaderError. Never a crash, never
      // silent wrong pixels (doc 08 Principle 8: the blob is self-verifying).
      failure = ReaderError{ReaderError::Kind::MalformedField, "/params/blobs", ObjectId{}};
      return false;
    }

    // VERBATIM, PADDING INCLUDED (Decision 3). `decode_tile_blob` yields exactly
    // `samples` == `dst.size()` floats, which is the whole blob: the save side `peek()`s
    // the whole pool blob, so the padding is INSIDE the hash. Dropping it here -- as the
    // dense route did -- would leave the memo below asserting a name the pool blob no
    // longer hashes to, and the next save would publish that stale name over different
    // bytes. Padding stays unobservable in output either way: every read clamps to
    // `width`/`height`.
    std::copy_n(pixels->begin(), dst.size(), dst.begin());
    return true;
  };

  std::unique_ptr<RasterContent> content =
      RasterContent::from_tiles(geom->width, geom->height, geom->edge, fill);
  if (!content) {
    return unexpected(failure.value_or(
        ReaderError{ReaderError::Kind::MalformedField, "/params/blobs", ObjectId{}}));
  }

  // SEED THE MEMO with the tiles we just decoded: we already know each one's name, so
  // re-hashing it would be pure waste -- and it is what makes both the reader's
  // params-only residual re-serialize and the FIRST save after a load a pure memo sweep.
  //
  // Note the level-0 grid we seed against is the one `build_from_tiles` just minted, so its
  // slots are fresh: two identical tiles in the document occupy two distinct slots and both
  // map to the same hash, which is exactly right -- the dedup lives in the NAME, not in the
  // pool. The seeded name is now HONEST for a foreign blob too: the fill copied the blob
  // verbatim, so the pool slot really does hash to the name we are asserting for it, which
  // the dense route could not promise for a blob whose padding was not already zero.
  if (tiles != nullptr) {
    if (const TileTablePtr built = content->store().base_table();
        built && built->level_count() > 0) {
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

Codec raster_codec() { return raster_codec(nullptr, nullptr); }

Codec raster_codec(RasterTileStore* tiles) { return raster_codec(tiles, nullptr); }

Codec raster_codec(RasterTileStore* tiles, TileEncodeDispatch* dispatch) {
  return Codec{[tiles, dispatch](const Content& content,
                                 SaveContext& ctx) -> expected<json, SerializeError> {
                 return serialize_raster(content, ctx, tiles, dispatch);
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
