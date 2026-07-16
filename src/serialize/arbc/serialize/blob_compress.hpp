#pragma once

#include <arbc/arbc_api.h>
#include <arbc/base/expected.hpp>

#include <cstddef>
#include <span>
#include <vector>

namespace arbc {

// The tile-blob compression seam (doc 08 Principle 8; the compressor is zstd, doc
// 10:27, wired in serialize.zstd_dep). Raw frame in, raw frame out: this is the
// whole of what `arbc::serialize` does with the compressor. The byte-shuffle that
// makes it pay on float tiles is OURS, not the library's (doc 10:27), and it --
// along with the content-addressed store, the hashing, and incremental save -- is
// serialize.raster_tile_store's, composed ON TOP of this seam as
// shuffle -> compress_blob -> hash-named file.
//
// Note what does NOT appear here: no `zstd.h`, and no zstd type in any signature.
// The seam trades in `std::span<const std::byte>` / `std::vector<std::byte>` /
// `arbc::expected` alone, so no embedder of `libarbc` ever compiles against zstd --
// the compile/include interface is unconditionally clean, which is what doc
// 10:34-35's "embedding the core must never transitively impose" is actually about
// (zstd_dep Decision 2, Constraint 3). The library is linked PRIVATE onto
// `arbc_serialize` (`src/serialize/CMakeLists.txt`), exactly as nlohmann/json is.
//
// The seam is deliberately byte-oriented rather than tile-aware, and the
// levelization forces that anyway: `arbc::serialize` and `arbc::kind-*` are BOTH
// Level 4 (doc 17:59-60) and same-level edges are forbidden (17:42-43), so
// `serialize` cannot see `RasterContent` at all. What it can see is the bytes
// `BigBlockPool::peek` hands out -- an immutable span of exactly the blob's length,
// which is precisely `compress_blob`'s parameter.

// The seam's error value (doc 10:15-17, errors as values -- no exception crosses
// this boundary). zstd's C API throws nothing and reports through `size_t` codes,
// so this falls out naturally; it is pinned by a test rather than assumed.
struct BlobCompressError {
  enum class Kind {
    // zstd rejected the frame outright: it is not a zstd frame at all, it is
    // truncated mid-stream, or its content does not fit the caller's declared
    // bound (including the hostile case -- a header advertising a huge content
    // size against a one-tile bound, which fails here rather than allocating).
    CorruptFrame,
    // The frame decoded cleanly but produced FEWER bytes than the caller declared.
    // The blob is well-formed zstd and simply is not the tile it claims to be. An
    // EMPTY blob lands here too rather than in CorruptFrame: zstd reads a
    // zero-length input as zero frames and reports 0 bytes decoded, not an error,
    // so it is the caller's bound that catches it.
    SizeMismatch,
    // `ZSTD_compress` failed into a `ZSTD_compressBound`-sized buffer. Unreachable
    // by construction short of an allocation failure inside zstd; reported as a
    // value rather than silently emitting a truncated frame.
    CompressFailed,
  };
  Kind kind{Kind::CorruptFrame};

  friend bool operator==(const BlobCompressError&, const BlobCompressError&) = default;
};

// Compress one tile blob to a raw zstd frame.
//
// The output buffer is sized at `ZSTD_compressBound(blob.size())`, NOT at
// `blob.size()`: a high-entropy photographic tile is incompressible and its frame
// is legitimately LARGER than its input. A seam that sized its output at the input
// length would fail on exactly the tiles that dominate a painting's bytes.
//
// Stateless and reentrant: it uses zstd's one-shot API, never a shared `ZSTD_CCtx`
// (which is explicitly not safe to share across threads). Tile compression will be
// called from pool workers (serialize.raster_tile_store), so this is safe to call
// concurrently with no locking and no new TSan surface. A later "optimization" to a
// shared reused context would be a data race; a per-call or thread-local context is
// the correct shape if profiling ever demands one (zstd_dep Constraint 6).
ARBC_API expected<std::vector<std::byte>, BlobCompressError>
compress_blob(std::span<const std::byte> blob);

// Decompress a raw zstd frame back to exactly `expected_size` bytes.
//
// The output bound comes FROM THE CALLER and NEVER from the frame header. The
// loader is an untrusted, fuzzed surface (serialize.format_tests ships a libFuzzer
// harness over it), and `ZSTD_getFrameContentSize` is attacker-controlled data: a
// hand-edited `.arbc` can claim its tile expands to 64 GB, and a reader that
// allocated on that value would OOM before validating a single byte. The caller
// always knows the true size -- tile edge x the storage format's bytes-per-pixel,
// both declared in the document and validated independently -- so it passes it, the
// seam allocates exactly that and no more, and a frame that does not produce
// exactly that many bytes is an error value (zstd_dep Constraint 7, Decision 6).
//
// Returns the decompressed bytes, or a `BlobCompressError` value on a corrupt,
// truncated, oversized, or hostile frame. Never throws, never aborts, and never
// allocates beyond `expected_size`.
ARBC_API expected<std::vector<std::byte>, BlobCompressError>
decompress_blob(std::span<const std::byte> frame, std::size_t expected_size);

} // namespace arbc
