#include <arbc/serialize/blob_compress.hpp>

#include <zstd.h>

#include <cstddef>
#include <span>
#include <vector>

namespace arbc {
namespace {

// zstd's own default level (3). The level is a free tuning knob -- a future "save
// fast" vs "save small" -- precisely BECAUSE the content hash is over the
// UNCOMPRESSED tile bytes (doc 08 Principle 8, as amended by zstd_dep Decision 1):
// changing it re-encodes a blob without renaming it, so it is never a format break.
constexpr int k_compression_level = ZSTD_CLEVEL_DEFAULT;

} // namespace

expected<std::vector<std::byte>, BlobCompressError> compress_blob(
    std::span<const std::byte> blob) {
  // compressBound, not blob.size(): an incompressible tile's frame is legitimately
  // larger than its input (see the header).
  std::vector<std::byte> frame(ZSTD_compressBound(blob.size()));
  const std::size_t written = ZSTD_compress(frame.data(), frame.size(), blob.data(),
                                            blob.size(), k_compression_level);
  if (ZSTD_isError(written) != 0U) {
    return unexpected(BlobCompressError{BlobCompressError::Kind::CompressFailed});
  }
  frame.resize(written);
  return frame;
}

expected<std::vector<std::byte>, BlobCompressError> decompress_blob(
    std::span<const std::byte> frame, std::size_t expected_size) {
  // The caller's bound is the ONLY allocation. The frame header is never consulted,
  // so `ZSTD_getFrameContentSize`'s sentinels -- and its lies -- are non-issues: a
  // frame claiming to expand to 64 GB simply does not fit here, and zstd reports
  // `dstSize_tooSmall` rather than us allocating 64 GB to find that out.
  std::vector<std::byte> blob(expected_size);
  const std::size_t produced =
      ZSTD_decompress(blob.data(), blob.size(), frame.data(), frame.size());
  if (ZSTD_isError(produced) != 0U) {
    return unexpected(BlobCompressError{BlobCompressError::Kind::CorruptFrame});
  }
  if (produced != expected_size) {
    return unexpected(BlobCompressError{BlobCompressError::Kind::SizeMismatch});
  }
  return blob;
}

} // namespace arbc
