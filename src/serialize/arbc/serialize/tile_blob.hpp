#pragma once

// The content-addressed TILE BLOB FORMAT (serialize.raster_tile_store Decision 2; doc
// 08 Principle 8).
//
// THE L4/L5 SPLIT. `arbc::serialize` and `arbc::kind_raster` are both L4 and doc 17
// permits no same-level edge, so neither may include the other. This header is the
// serialize half: the FORMAT -- content hash, byte-shuffle, storage-format conversion,
// blob framing, geometry validation, blob-URI derivation -- over a strictly
// BYTE-ORIENTED API that names no raster type. `std::span` in, `std::vector` out. The
// CODEC (`runtime/codec_raster.cpp`, L5) is the only place that sees both
// `RasterContent` and `nlohmann::json`; it does the `peek()` and hands us plain bytes.
//
// THE COMPOSITION, in the order `blob_compress.hpp` prescribed when it deferred to this
// task: shuffle -> compress -> hash-named file. And on the way back in: decompress ->
// unshuffle -> hash -> compare against the name it was fetched under.
//
// THE HASH IS OVER THE UNCOMPRESSED STORAGE BYTES -- not the working bytes, not the
// compressed frame (doc 08: "compression is a storage encoding, never content
// identity"). Two consequences the implementation must keep true, because the format
// promises both: a zstd version or level change must not rename a single blob, and a
// blob is SELF-VERIFYING -- its name is a checksum of its own contents, so a truncated
// file, a bit-flipped frame, or a substituted blob is caught as a value rather than
// decoded into silently wrong pixels.

#include <arbc/arbc_api.h>
#include <arbc/base/expected.hpp>
#include <arbc/media/pixel_format.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace arbc {

// SHA-256 truncated to its leading 128 bits, lowercase hex (Decision 3).
inline constexpr std::size_t k_tile_hash_bytes = 16;
inline constexpr std::size_t k_tile_hash_chars = k_tile_hash_bytes * 2;

// The untrusted-geometry caps (Constraint 7). `zstd_dep` bounded decompression by the
// CALLER's declared size; that size is now derived from an `edge`/`width`/`height` read
// out of a possibly-hostile document, so the geometry itself has become part of the
// threat model. These are the bounds that get checked BEFORE any allocation is sized by
// them -- generous enough that no real document meets them, small enough that a hostile
// one cannot ask for an allocation that matters.
inline constexpr std::int64_t k_max_tile_edge = 4096;    // and a power of two
inline constexpr std::int64_t k_max_dimension = 1 << 20; // 1 048 576 px per side
inline constexpr std::int64_t k_max_tiles = 1 << 22;     // 4 194 304 tiles per level

// A tile-blob format failure, as a value (doc 10). Never a throw, never a crash, and
// never silently wrong pixels.
struct TileBlobError {
  enum class Kind {
    BadGeometry,  // edge/width/height/tile-count outside the caps, or not a power of two
    BadHash,      // a blob name that is not 32 lowercase hex chars
    CorruptFrame, // the zstd frame is truncated, malformed, or decodes to the wrong size
    HashMismatch, // the decoded storage bytes do not hash to the name they arrived under
  };
  Kind kind{Kind::BadGeometry};

  friend bool operator==(const TileBlobError&, const TileBlobError&) = default;
};

// A validated level-0 tile grid. Constructed only by `validate_tile_geometry`, so
// holding one IS the proof the numbers were checked.
struct TileGeometry {
  int edge{0};
  int width{0};
  int height{0};
  int tiles_x{0};
  int tiles_y{0};

  // Tiles in the level-0 grid -- exactly the length the `blobs` array must have.
  std::size_t tile_count() const noexcept {
    return static_cast<std::size_t>(tiles_x) * static_cast<std::size_t>(tiles_y);
  }
  // Samples in one tile blob: edge * edge * 4 channels.
  std::size_t tile_samples() const noexcept {
    return static_cast<std::size_t>(edge) * static_cast<std::size_t>(edge) *
           channels_per_pixel(PixelFormat::Rgba32fLinearPremul);
  }
};

// Validate untrusted geometry BEFORE it is used as an allocation bound (Constraint 7).
// Takes 64-bit inputs precisely because the document's numbers are not yet known to fit
// in an `int`: a hostile `width` of 2^40 must be REJECTED, not truncated into something
// plausible. This is the extension of doc 08's "the output size comes from the tile
// geometry the document declares, never from the frame header" -- which only helps if
// the geometry is itself checked first.
ARBC_API expected<TileGeometry, TileBlobError>
validate_tile_geometry(std::int64_t edge, std::int64_t width, std::int64_t height);

// The blob name: SHA-256 of the UNCOMPRESSED STORAGE bytes, leading 16 bytes, lowercase
// hex. Not the working bytes; not the compressed frame.
ARBC_API std::string hash_tile(std::span<const std::byte> storage_bytes);

// Whether `name` is a well-formed blob name (32 lowercase hex chars). A hostile
// document's `blobs` entry is a string that has been nowhere near a hash function.
ARBC_API bool is_tile_hash(std::string_view name);

// The blob's URI relative to the document: `<base><first-2-hex>/<full-hex>`, e.g.
// `assets/tiles/3f/3fa91c…`. The two-level fan-out is derived HERE, inside the store,
// so the JSON never names a blob path and the layout can change without a format break
// -- and so a flat directory of 10^5 blobs never happens, which is hostile to exactly
// the ordinary filesystem tooling doc 08 says the asset directory exists to preserve.
// `base` gains a trailing '/' if it lacks one.
ARBC_API std::string tile_blob_uri(std::string_view base, std::string_view hash);

// The byte-shuffle (doc 08: "the shuffle is ours, not the library's"). Groups by SAMPLE
// -- all byte-0s of every sample, then all byte-1s, and so on -- which is doc 08 read
// literally ("separating a FLOAT's noisy low mantissa bytes from its structured exponent
// and sign planes") and is the transform its calibrated 2.1x was measured against. The
// stride is `bytes_per_sample`: 2 at `rgba16f`, 4 at `rgba32f`.
//
// It lives INSIDE the blob, beneath the hash, so it must round-trip exactly:
// `unshuffle(shuffle(x), s) == x` for every `x` and every `s >= 1`, including an `x`
// whose length is not a multiple of `s` (the remainder rides verbatim at the tail, which
// is what keeps the pair a bijection on all inputs rather than only on well-formed ones).
ARBC_API std::vector<std::byte> shuffle_bytes(std::span<const std::byte> in, std::size_t stride);
ARBC_API std::vector<std::byte> unshuffle_bytes(std::span<const std::byte> in, std::size_t stride);

// Bytes one sample occupies in `storage` -- the shuffle stride (2 / 4).
ARBC_API std::size_t bytes_per_sample(PixelFormat storage);

// Working floats -> the document's storage format, the bytes the hash is taken over.
// At `rgba32f` this is the identity (lossless). At the `rgba16f` default it quantizes,
// exactly once: `f16_to_float` followed by `f16_from_float` is EXACT, so a save -> load
// -> save produces an identical hash set and writes zero new blobs. Stating that
// honestly matters -- "byte-exact round-trip" is unqualified only at `rgba32f`.
ARBC_API std::vector<std::byte> to_storage_bytes(std::span<const float> working,
                                                 PixelFormat storage);

// The document's storage format -> working floats.
ARBC_API std::vector<float> from_storage_bytes(std::span<const std::byte> storage,
                                               PixelFormat storage_fmt);

// Storage bytes -> the on-disk frame: shuffle, then zstd. The hash is NOT recomputed
// here (the caller already has it, and recomputing would invite the two to disagree).
ARBC_API expected<std::vector<std::byte>, TileBlobError>
frame_tile_blob(std::span<const std::byte> storage_bytes, PixelFormat storage);

// The on-disk frame -> working floats, VERIFIED against the name it was fetched under.
//
// `sample_count` is the caller's declared bound (edge * edge * 4, from a validated
// `TileGeometry`) and is what sizes the decompression -- never the frame header, which
// on a hostile file is attacker-controlled and will happily claim to expand to 64 GB
// (doc 08 § Dependency note). A frame that does not decode to exactly that size is
// `CorruptFrame`; storage bytes that do not hash to `expected_hash` are `HashMismatch`.
ARBC_API expected<std::vector<float>, TileBlobError>
decode_tile_blob(std::span<const std::byte> frame, std::string_view expected_hash,
                 PixelFormat storage, std::size_t sample_count);

} // namespace arbc
