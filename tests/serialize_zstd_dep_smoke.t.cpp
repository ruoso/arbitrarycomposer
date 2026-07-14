// serialize.zstd_dep build-integration smoke test for the tile-blob compression
// seam (doc 08 Principle 8; compressor decided in doc 10:27).
//
// READ THE LINK LINE FIRST -- it is the point of this file. This test links the
// umbrella `arbc` and NOTHING ELSE (tests/CMakeLists.txt): it never names
// `arbc_zstd`, never names a `zstd::*` target, and never includes `<zstd.h>`. That
// construction is what PROVES the load-bearing property of zstd_dep, rather than
// asserting it:
//
//   * it COMPILES only if no zstd type leaked onto libarbc's public include
//     interface (blob_compress.hpp names std::span/std::vector/arbc::expected and
//     nothing else -- zstd_dep Constraint 3);
//   * it LINKS only if `target_link_libraries(arbc_serialize PRIVATE arbc_zstd)` is
//     genuinely right for the shipped library shape.
//
// The sibling serialize.json_dep probe took the cheaper shape -- link the dependency
// STANDALONE and exercise it (tests/serialize_json_dep_smoke.t.cpp). That shape
// would prove NOTHING here. A test that links zstd directly exercises zstd; it says
// nothing whatsoever about libarbc's interface. For a header-only dependency the
// distinction was academic; for a COMPILED one it is the entire question, which is
// why this task deviates (zstd_dep Decision 4).
//
// What it does NOT cover, deliberately: the byte-shuffle, the content-addressed tile
// store, the hashing, incremental save, the storage-format field, and the
// org.arbc.raster codec. All are serialize.raster_tile_store's, composed ON TOP of
// this seam (doc 10:27 -- "the shuffle is ours, not the library's").

#include <catch2/catch_test_macros.hpp>

#include <arbc/kind_raster/raster_content.hpp> // k_default_tile_edge, k_tile_channels
#include <arbc/serialize/blob_compress.hpp>

#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <vector>

using arbc::BlobCompressError;
using arbc::compress_blob;
using arbc::decompress_blob;

namespace {

std::span<const std::byte> bytes_of(const std::vector<std::byte>& v) {
  return std::span<const std::byte>(v.data(), v.size());
}

// A deterministic high-entropy buffer: the photographic case, where the pixel noise
// floor is genuinely incompressible.
std::vector<std::byte> random_bytes(std::size_t n, std::uint32_t seed) {
  std::mt19937 rng(seed);
  std::vector<std::byte> out(n);
  for (std::byte& b : out) {
    b = static_cast<std::byte>(rng() & 0xFFU);
  }
  return out;
}

// The real in-memory tile blob size: `edge * edge * k_tile_channels * sizeof(float)`
// (src/kind_raster/raster_content.cpp:54-56), which at the default 256 edge
// (raster_content.hpp:33) is exactly 1 MiB in the rgba32f working format.
std::size_t working_tile_bytes(int edge) {
  return static_cast<std::size_t>(edge) * static_cast<std::size_t>(edge) *
         arbc::k_tile_channels * sizeof(float);
}

// The round-trip every case below reduces to: compress, decompress against the
// caller's known bound, and demand the original bytes back exactly.
void require_byte_exact_round_trip(const std::vector<std::byte>& blob) {
  const auto frame = compress_blob(bytes_of(blob));
  REQUIRE(frame.has_value());

  const auto back = decompress_blob(bytes_of(*frame), blob.size());
  REQUIRE(back.has_value());
  REQUIRE(*back == blob); // byte-for-byte, not merely same-length
}

} // namespace

// enforces: 08-serialization#tile-blob-codec-round-trips-byte-exactly
TEST_CASE("tile blobs round-trip through the compressor byte-exactly") {
  SECTION("empty input") {
    // The degenerate bound: a zero-length blob compresses to a valid (non-empty)
    // frame and decompresses back to zero bytes, with expected_size == 0.
    require_byte_exact_round_trip({});
  }

  SECTION("an all-zero tile collapses to a tiny frame") {
    // The empty-tile case -- an untouched region of a painting. It must not cost a
    // megabyte on disk; this is where dedup and compression compound.
    const std::vector<std::byte> blob(working_tile_bytes(arbc::k_default_tile_edge),
                                      std::byte{0});
    require_byte_exact_round_trip(blob);

    const auto frame = compress_blob(bytes_of(blob));
    REQUIRE(frame.has_value());
    REQUIRE(frame->size() < blob.size() / 1000);
  }

  SECTION("a flat constant-colour tile compresses hard") {
    std::vector<std::byte> blob(working_tile_bytes(arbc::k_default_tile_edge));
    for (std::size_t i = 0; i < blob.size(); ++i) {
      blob[i] = static_cast<std::byte>(i % sizeof(float)); // one repeated float
    }
    require_byte_exact_round_trip(blob);

    const auto frame = compress_blob(bytes_of(blob));
    REQUIRE(frame.has_value());
    REQUIRE(frame->size() < blob.size());
  }

  SECTION("a high-entropy tile round-trips even though its frame is LARGER") {
    // The photographic case, and the reason `compress_blob` sizes its output buffer
    // at ZSTD_compressBound rather than at the input length: incompressible bytes
    // come back out as raw blocks plus framing, so the frame legitimately EXCEEDS
    // the input. A seam that sized its destination at `blob.size()` would fail here
    // -- on exactly the tiles that are 93% of a painting's bytes (doc 17:226-227).
    const std::vector<std::byte> blob = random_bytes(64U * 1024U, /*seed=*/1234U);
    require_byte_exact_round_trip(blob);

    const auto frame = compress_blob(bytes_of(blob));
    REQUIRE(frame.has_value());
    REQUIRE(frame->size() > blob.size());
  }

  SECTION("a tile at the real geometry -- 1 MiB rgba32f working, 512 KiB rgba16f") {
    // The sizes the tile store will actually hand this seam.
    const std::size_t working = working_tile_bytes(arbc::k_default_tile_edge);
    REQUIRE(working == 1024U * 1024U); // rgba32f, edge 256

    // Photographic-ish: a smooth gradient plus a noise floor -- structured enough to
    // compress, noisy enough to be realistic.
    std::mt19937 rng(99U);
    std::vector<std::byte> tile(working);
    for (std::size_t i = 0; i < tile.size(); ++i) {
      tile[i] = static_cast<std::byte>(((i / 977U) + (rng() & 0x0FU)) & 0xFFU);
    }
    require_byte_exact_round_trip(tile);

    // The rgba16f storage default is half the working size (doc 08 Principle 8).
    const std::vector<std::byte> storage_tile(tile.begin(),
                                              tile.begin() + static_cast<std::ptrdiff_t>(
                                                                 working / 2));
    REQUIRE(storage_tile.size() == 512U * 1024U);
    require_byte_exact_round_trip(storage_tile);
  }
}

// enforces: 08-serialization#tile-blob-decompress-is-bounded-and-fails-as-a-value
TEST_CASE("decompressing a hostile blob is bounded and fails as a value") {
  // The loader is an untrusted, FUZZED surface (serialize.format_tests ships a
  // libFuzzer harness over it), so every case below must come back as an
  // arbc::expected error VALUE: no exception escapes, no abort, and -- the property
  // that cannot be asserted from inside the process, only constructed -- no
  // allocation beyond the caller's declared bound. `decompress_blob` allocates
  // `expected_size` and never consults the frame header, so the bound holds by
  // construction (zstd_dep Constraint 7 / Decision 6).
  const std::size_t one_tile = working_tile_bytes(arbc::k_default_tile_edge);

  SECTION("random bytes are not a zstd frame") {
    const std::vector<std::byte> garbage = random_bytes(256U, /*seed=*/7U);
    const auto back = decompress_blob(bytes_of(garbage), one_tile);
    REQUIRE_FALSE(back.has_value());
    REQUIRE(back.error().kind == BlobCompressError::Kind::CorruptFrame);
  }

  SECTION("an empty blob decodes to nothing and misses the bound") {
    // Note WHICH error this is, because it is not the obvious one: zstd reads a
    // zero-length input as ZERO FRAMES and reports 0 bytes decoded rather than an
    // error, so an empty blob comes out the length check (SizeMismatch), not the
    // frame check. Either way it is an error VALUE, which is the claim -- but the
    // seam's bound is what catches it, so a caller cannot be handed a short tile.
    const auto back = decompress_blob({}, one_tile);
    REQUIRE_FALSE(back.has_value());
    REQUIRE(back.error().kind == BlobCompressError::Kind::SizeMismatch);
  }

  SECTION("a valid frame truncated mid-stream") {
    const std::vector<std::byte> blob = random_bytes(8192U, /*seed=*/11U);
    const auto frame = compress_blob(bytes_of(blob));
    REQUIRE(frame.has_value());

    std::vector<std::byte> truncated(frame->begin(),
                                     frame->begin() + static_cast<std::ptrdiff_t>(
                                                          frame->size() / 2));
    const auto back = decompress_blob(bytes_of(truncated), blob.size());
    REQUIRE_FALSE(back.has_value());
    REQUIRE(back.error().kind == BlobCompressError::Kind::CorruptFrame);
  }

  SECTION("a well-formed frame whose content is SHORTER than the caller's bound") {
    // Well-formed zstd; simply not the tile it claims to be. It decodes cleanly and
    // then fails the length check -- a distinct error kind from a corrupt frame.
    const std::vector<std::byte> blob(100U, std::byte{0xAB});
    const auto frame = compress_blob(bytes_of(blob));
    REQUIRE(frame.has_value());

    const auto back = decompress_blob(bytes_of(*frame), /*expected_size=*/200U);
    REQUIRE_FALSE(back.has_value());
    REQUIRE(back.error().kind == BlobCompressError::Kind::SizeMismatch);
  }

  SECTION("a well-formed frame whose content is LONGER than the caller's bound") {
    // zstd itself refuses: the content does not fit the destination we sized at the
    // caller's bound, so it never writes past it.
    const std::vector<std::byte> blob(4096U, std::byte{0xCD});
    const auto frame = compress_blob(bytes_of(blob));
    REQUIRE(frame.has_value());

    const auto back = decompress_blob(bytes_of(*frame), /*expected_size=*/100U);
    REQUIRE_FALSE(back.has_value());
    REQUIRE(back.error().kind == BlobCompressError::Kind::CorruptFrame);
  }

  SECTION("a frame whose header advertises a huge content size") {
    // THE case that matters most. A hand-edited `.arbc` can carry a tile blob whose
    // frame header claims it expands to 64 GiB. A reader that trusted
    // ZSTD_getFrameContentSize and allocated it would OOM before validating a single
    // byte -- a one-line remote denial of service. `decompress_blob` never reads that
    // field: it allocates the caller's one tile, hands zstd that bound, and zstd
    // reports the content does not fit. The 64 GiB is never allocated, and the fact
    // that this test completes at all is the assertion.
    //
    // Hand-built because it cannot be produced by compressing anything (RFC 8878
    // §3.1.1): magic 0xFD2FB528 LE; a Frame_Header_Descriptor with
    // Frame_Content_Size_flag = 3 (an 8-byte size field) and Single_Segment_flag = 1
    // (so no Window_Descriptor byte follows) = 0xE0; then the 8-byte little-endian
    // content size 0x00'0000'0010'0000'0000 = 64 GiB; then a final empty raw block.
    const std::vector<std::byte> hostile = {
        std::byte{0x28}, std::byte{0xB5}, std::byte{0x2F}, std::byte{0xFD}, // magic
        std::byte{0xE0},                                                    // FHD
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x10}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, // 64 GiB
        std::byte{0x01}, std::byte{0x00}, std::byte{0x00},                  // last raw block
    };

    const auto back = decompress_blob(bytes_of(hostile), one_tile);
    REQUIRE_FALSE(back.has_value());
    REQUIRE(back.error().kind == BlobCompressError::Kind::CorruptFrame);
  }
}
