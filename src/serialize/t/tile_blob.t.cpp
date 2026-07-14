// The content-addressed tile-blob FORMAT, driven as pure functions over bytes
// (serialize.raster_tile_store Decision 2). No raster type, no pool, no document -- if
// this file needed one, the L4/L5 split would have been violated.

#include <arbc/serialize/blob_compress.hpp>
#include <arbc/serialize/save_context.hpp>
#include <arbc/serialize/tile_blob.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace arbc;

namespace {

std::vector<std::byte> bytes_of(std::initializer_list<int> vals) {
  std::vector<std::byte> out;
  out.reserve(vals.size());
  for (const int v : vals) {
    out.push_back(static_cast<std::byte>(v));
  }
  return out;
}

// A deterministic pseudo-random working tile: `samples` floats in [0, 1).
std::vector<float> working_tile(std::size_t samples, std::uint32_t seed) {
  std::vector<float> out(samples);
  std::uint32_t s = seed;
  for (float& f : out) {
    s = s * 1664525U + 1013904223U;
    f = static_cast<float>(s >> 8U) / static_cast<float>(1U << 24U);
  }
  return out;
}

// A sink that records what it was asked to store.
class RecordingSink final : public AssetSink {
public:
  expected<bool, AssetSinkError> put(std::string_view uri,
                                     std::span<const std::byte> bytes) override {
    if (!d_names.insert(std::string(uri)).second) {
      return false; // write-if-absent: the name is already here
    }
    d_bytes += bytes.size();
    ++d_written;
    return true;
  }
  bool contains(std::string_view uri) const override {
    return d_names.find(std::string(uri)) != d_names.end();
  }
  std::uint64_t blobs_written() const noexcept override { return d_written; }
  std::size_t distinct_names() const { return d_names.size(); }
  std::size_t bytes_stored() const { return d_bytes; }

private:
  std::set<std::string> d_names;
  std::uint64_t d_written{0};
  std::size_t d_bytes{0};
};

} // namespace

// The byte-shuffle is OURS and version-stable (doc 08:372-376), so unlike the zstd frame
// it can be -- and is -- pinned byte-exactly as a pure function.
TEST_CASE("the byte-shuffle groups by sample and is a byte-exact golden") {
  // Stride 2 (rgba16f): four 2-byte samples. All byte-0s, then all byte-1s.
  const std::vector<std::byte> in2 = bytes_of({0x11, 0xAA, 0x22, 0xBB, 0x33, 0xCC, 0x44, 0xDD});
  CHECK(shuffle_bytes(in2, 2) == bytes_of({0x11, 0x22, 0x33, 0x44, 0xAA, 0xBB, 0xCC, 0xDD}));

  // Stride 4 (rgba32f): two 4-byte samples. Byte-plane 0, 1, 2, 3.
  const std::vector<std::byte> in4 = bytes_of({0x00, 0x01, 0x02, 0x03, 0x10, 0x11, 0x12, 0x13});
  CHECK(shuffle_bytes(in4, 4) == bytes_of({0x00, 0x10, 0x01, 0x11, 0x02, 0x12, 0x03, 0x13}));
}

TEST_CASE("unshuffle o shuffle is the identity, including on ragged input") {
  for (const std::size_t stride : {std::size_t{1}, std::size_t{2}, std::size_t{4}}) {
    for (std::size_t n = 0; n < 40; ++n) {
      std::vector<std::byte> in(n);
      for (std::size_t i = 0; i < n; ++i) {
        in[i] = static_cast<std::byte>((i * 37 + 11) & 0xFF);
      }
      // Ragged lengths never occur in production (a tile is always whole samples), but a
      // bijection on ALL inputs is what makes the pair safe to sit beneath the hash.
      CHECK(unshuffle_bytes(shuffle_bytes(in, stride), stride) == in);
    }
  }
}

TEST_CASE("the blob name is the hash of the UNCOMPRESSED storage bytes") {
  const std::vector<std::byte> storage = bytes_of({0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x11, 0x22, 0x33});
  const std::string name = hash_tile(storage);
  CHECK(is_tile_hash(name));
  CHECK(name.size() == k_tile_hash_chars);

  // Compression is a storage encoding, never content identity (doc 08:353-361): the name
  // is a function of the storage bytes alone, and neither the shuffle nor the frame
  // enters into it.
  const expected<std::vector<std::byte>, TileBlobError> frame =
      frame_tile_blob(storage, PixelFormat::Rgba16fLinearPremul);
  REQUIRE(frame.has_value());
  CHECK(hash_tile(storage) == name);
  CHECK(hash_tile(*frame) != name); // hashing the FRAME would be the bug this rules out
}

TEST_CASE("the blob URI fans out two hex digits") {
  const std::string hash = "3fa91c0000000000000000000000abcd";
  CHECK(tile_blob_uri("assets/tiles/", hash) == "assets/tiles/3f/3fa91c0000000000000000000000abcd");
  // A base without the trailing slash resolves the same way.
  CHECK(tile_blob_uri("assets/tiles", hash) == "assets/tiles/3f/3fa91c0000000000000000000000abcd");
}

TEST_CASE("is_tile_hash rejects everything that is not a canonical name") {
  CHECK(is_tile_hash("0123456789abcdef0123456789abcdef"));
  CHECK_FALSE(is_tile_hash(""));
  CHECK_FALSE(is_tile_hash("0123456789abcdef0123456789abcde"));   // 31 chars
  CHECK_FALSE(is_tile_hash("0123456789abcdef0123456789abcdef0")); // 33 chars
  CHECK_FALSE(is_tile_hash("0123456789ABCDEF0123456789abcdef"));  // uppercase
  CHECK_FALSE(is_tile_hash("0123456789abcdeg0123456789abcdef"));  // 'g'
  CHECK_FALSE(is_tile_hash("../../../../../../etc/passwd0000"));  // a path, not a hash
}

// enforces: 08-serialization#raster-tile-geometry-is-validated-before-allocation
TEST_CASE("hostile tile geometry is rejected as a value before it sizes an allocation") {
  // The well-formed case still passes.
  const expected<TileGeometry, TileBlobError> ok = validate_tile_geometry(256, 600, 300);
  REQUIRE(ok.has_value());
  CHECK(ok->tiles_x == 3);
  CHECK(ok->tiles_y == 2);
  CHECK(ok->tile_count() == 6);
  CHECK(ok->tile_samples() == 256U * 256U * 4U);

  const TileBlobError bad{TileBlobError::Kind::BadGeometry};

  // A non-power-of-two edge: the kind's tiling is power-of-two by construction, so this
  // is a document that was not produced by us.
  CHECK(validate_tile_geometry(255, 100, 100).error() == bad);
  CHECK(validate_tile_geometry(0, 100, 100).error() == bad);
  CHECK(validate_tile_geometry(-256, 100, 100).error() == bad);
  // An absurd edge: the allocation bound is edge^2 * 4 * 4, so an unbounded edge is an
  // unbounded allocation.
  CHECK(validate_tile_geometry(k_max_tile_edge * 2, 100, 100).error() == bad);
  // Non-positive or absurd extents. Taken as int64 precisely so a hostile 2^40 is
  // REJECTED rather than truncated into something plausible.
  CHECK(validate_tile_geometry(256, 0, 100).error() == bad);
  CHECK(validate_tile_geometry(256, 100, -1).error() == bad);
  CHECK(validate_tile_geometry(256, std::int64_t{1} << 40, 100).error() == bad);
  CHECK(validate_tile_geometry(256, 100, std::int64_t{1} << 40).error() == bad);
  // A tile count that would overflow the cap even though each side is individually legal.
  CHECK(validate_tile_geometry(1, k_max_dimension, k_max_dimension).error() == bad);
}

// enforces: 08-serialization#tile-blob-verifies-against-its-name
TEST_CASE("a tile blob verifies against its name; a corrupt one fails as a value") {
  constexpr std::size_t k_samples = 8 * 8 * 4;
  const std::vector<float> working = working_tile(k_samples, 7);
  const PixelFormat storage = PixelFormat::Rgba32fLinearPremul;

  const std::vector<std::byte> storage_bytes = to_storage_bytes(working, storage);
  const std::string name = hash_tile(storage_bytes);
  const expected<std::vector<std::byte>, TileBlobError> frame =
      frame_tile_blob(storage_bytes, storage);
  REQUIRE(frame.has_value());

  // The honest decode: byte-exact at rgba32f.
  const expected<std::vector<float>, TileBlobError> back =
      decode_tile_blob(*frame, name, storage, k_samples);
  REQUIRE(back.has_value());
  CHECK(*back == working);

  // A SUBSTITUTED blob -- the frame is a perfectly valid one, just not this tile's.
  const std::vector<float> other = working_tile(k_samples, 99);
  const std::vector<std::byte> other_bytes = to_storage_bytes(other, storage);
  const expected<std::vector<std::byte>, TileBlobError> other_frame =
      frame_tile_blob(other_bytes, storage);
  REQUIRE(other_frame.has_value());
  CHECK(decode_tile_blob(*other_frame, name, storage, k_samples).error() ==
        TileBlobError{TileBlobError::Kind::HashMismatch});

  // A TRUNCATED file.
  std::vector<std::byte> truncated(frame->begin(), frame->begin() + frame->size() / 2);
  CHECK(decode_tile_blob(truncated, name, storage, k_samples).error() ==
        TileBlobError{TileBlobError::Kind::CorruptFrame});

  // A BIT-FLIPPED frame: either zstd's own frame checksum catches it (CorruptFrame) or it
  // decodes to bytes that do not hash to the name (HashMismatch). Both are values; the
  // one thing that must never happen is silently wrong pixels.
  std::vector<std::byte> flipped = *frame;
  flipped[flipped.size() / 2] ^= std::byte{0x40};
  const expected<std::vector<float>, TileBlobError> corrupt =
      decode_tile_blob(flipped, name, storage, k_samples);
  REQUIRE_FALSE(corrupt.has_value());
  CHECK((corrupt.error().kind == TileBlobError::Kind::CorruptFrame ||
         corrupt.error().kind == TileBlobError::Kind::HashMismatch));

  // A malformed NAME never reaches the decompressor at all.
  CHECK(decode_tile_blob(*frame, "not-a-hash", storage, k_samples).error() ==
        TileBlobError{TileBlobError::Kind::BadHash});

  // A geometry lie: the caller's bound is what sizes the decompression (zstd_dep Decision
  // 6), so a frame that does not decode to exactly that many bytes is corrupt.
  CHECK(decode_tile_blob(*frame, name, storage, k_samples * 2).error() ==
        TileBlobError{TileBlobError::Kind::CorruptFrame});
}

// enforces: 08-serialization#raster-storage-quantization-is-idempotent
TEST_CASE("the rgba16f storage default quantizes once and is then a fixed point") {
  constexpr std::size_t k_samples = 8 * 8 * 4;
  const std::vector<float> working = working_tile(k_samples, 3);
  const PixelFormat storage = PixelFormat::Rgba16fLinearPremul;

  // The FIRST save quantizes f32 -> f16. That is lossy, by the user's authored choice.
  const std::vector<std::byte> once = to_storage_bytes(working, storage);
  const std::vector<float> quantized = from_storage_bytes(once, storage);
  CHECK(quantized != working); // honestly: rgba16f is NOT byte-exact from an rgba32f space

  // Thereafter it is a FIXED POINT: f16_to_float followed by f16_from_float is exact, so
  // save -> load -> save re-derives the same storage bytes, hence the same hash, hence
  // ZERO new blobs. This is why "byte-exact round-trip" is unqualified only at rgba32f.
  const std::vector<std::byte> twice = to_storage_bytes(quantized, storage);
  CHECK(twice == once);
  CHECK(hash_tile(twice) == hash_tile(once));
  CHECK(from_storage_bytes(twice, storage) == quantized);

  // rgba32f, by contrast, is the identity on the first pass.
  const std::vector<std::byte> lossless =
      to_storage_bytes(working, PixelFormat::Rgba32fLinearPremul);
  CHECK(from_storage_bytes(lossless, PixelFormat::Rgba32fLinearPremul) == working);
}

TEST_CASE("the shuffle stride is one sample") {
  CHECK(bytes_per_sample(PixelFormat::Rgba16fLinearPremul) == 2);
  CHECK(bytes_per_sample(PixelFormat::Rgba32fLinearPremul) == 4);
}

TEST_CASE("storage-format tokens round-trip and reject anything else") {
  CHECK(std::string(storage_format_token(PixelFormat::Rgba16fLinearPremul)) == "rgba16f");
  CHECK(std::string(storage_format_token(PixelFormat::Rgba32fLinearPremul)) == "rgba32f");
  CHECK(storage_format_from_token("rgba16f") == PixelFormat::Rgba16fLinearPremul);
  CHECK(storage_format_from_token("rgba32f") == PixelFormat::Rgba32fLinearPremul);
  // The working-space spelling is a DIFFERENT key at a different tier (doc 07). Accepting
  // it here would be the confusion Decision 4 exists to prevent.
  CHECK_FALSE(storage_format_from_token("rgba16f-linear-premul").has_value());
  CHECK_FALSE(storage_format_from_token("rgba8-srgb").has_value());
  CHECK_FALSE(storage_format_from_token("").has_value());
}

TEST_CASE("SaveContext stores through the sink, write-if-absent") {
  RecordingSink sink;
  SaveContext ctx{"/proj/project.arbc"};
  ctx.set_asset_sink(&sink);

  const std::vector<std::byte> blob = bytes_of({1, 2, 3, 4});

  // The relative URI resolves against the document's DIRECTORY, exactly as the read side
  // resolves it -- one seam, both directions.
  const expected<bool, AssetSinkError> first = ctx.store_asset("assets/tiles/aa/aabb", blob);
  REQUIRE(first.has_value());
  CHECK(*first); // newly written
  CHECK(sink.blobs_written() == 1);

  // The same name again writes nothing: content-addressed, so same name => same content.
  const expected<bool, AssetSinkError> again = ctx.store_asset("assets/tiles/aa/aabb", blob);
  REQUIRE(again.has_value());
  CHECK_FALSE(*again);
  CHECK(sink.blobs_written() == 1);
  CHECK(sink.distinct_names() == 1);
}

TEST_CASE("a SaveContext with no sink is an error, never a silent success") {
  SaveContext ctx{"/proj/project.arbc"};
  const std::vector<std::byte> blob = bytes_of({1, 2, 3});
  // Constraint 5: a save that would DROP PIXELS must fail loudly.
  CHECK(ctx.store_asset("assets/tiles/aa/aabb", blob).error() ==
        AssetSinkError{AssetSinkError::Kind::NoSink});

  // ...except in params-only mode, where the codec is being run for its key set and the
  // blobs it would write are the ones the load just read (already on disk, by
  // construction). No sink needed, and nothing stored.
  ctx.set_params_only(true);
  const expected<bool, AssetSinkError> stored = ctx.store_asset("assets/tiles/aa/aabb", blob);
  REQUIRE(stored.has_value());
  CHECK_FALSE(*stored);
}

TEST_CASE("SaveContext defaults to the rgba16f storage format") {
  const SaveContext ctx;
  CHECK(ctx.storage_format() == PixelFormat::Rgba16fLinearPremul);
  CHECK(ctx.asset_sink() == nullptr);
  CHECK_FALSE(ctx.params_only());
}
