#include <arbc/base/sha256.hpp>
#include <arbc/media/pixel_traits.hpp> // PixelTraits (the storage-format converter)
#include <arbc/serialize/blob_compress.hpp>
#include <arbc/serialize/tile_blob.hpp>

#include <cstring>

namespace arbc {
namespace {

unexpected<TileBlobError> fail(TileBlobError::Kind kind) { return unexpected(TileBlobError{kind}); }

bool is_power_of_two(std::int64_t v) { return v > 0 && (v & (v - 1)) == 0; }

// `PixelTraits<F>::encode` over a whole tile, templated so the format switch happens
// once rather than per sample.
template <PixelFormat F>
void encode_into(std::span<const float> working, std::vector<std::byte>& out) {
  using Traits = PixelTraits<F>;
  using Storage = typename Traits::Storage;
  const std::size_t pixels = working.size() / Traits::channels;
  out.resize(pixels * Traits::channels * sizeof(Storage));
  auto* p = reinterpret_cast<Storage*>(out.data());
  for (std::size_t i = 0; i < pixels; ++i) {
    const std::size_t o = i * Traits::channels;
    const WorkingPixel c{working[o], working[o + 1], working[o + 2], working[o + 3]};
    Traits::encode(c, p + o);
  }
}

template <PixelFormat F>
void decode_into(std::span<const std::byte> storage, std::vector<float>& out) {
  using Traits = PixelTraits<F>;
  using Storage = typename Traits::Storage;
  const std::size_t pixels = storage.size() / (Traits::channels * sizeof(Storage));
  out.resize(pixels * Traits::channels);
  const auto* p = reinterpret_cast<const Storage*>(storage.data());
  for (std::size_t i = 0; i < pixels; ++i) {
    const std::size_t o = i * Traits::channels;
    const WorkingPixel c = Traits::decode(p + o);
    out[o] = c[0];
    out[o + 1] = c[1];
    out[o + 2] = c[2];
    out[o + 3] = c[3];
  }
}

} // namespace

std::size_t bytes_per_sample(PixelFormat storage) {
  return bytes_per_pixel(storage) / channels_per_pixel(storage);
}

expected<TileGeometry, TileBlobError> validate_tile_geometry(std::int64_t edge, std::int64_t width,
                                                             std::int64_t height) {
  // Every check runs on 64-bit inputs and BEFORE anything is allocated from them: a
  // hostile `width` of 2^40 must be rejected, not truncated into something plausible
  // (Constraint 7).
  if (!is_power_of_two(edge) || edge > k_max_tile_edge) {
    return fail(TileBlobError::Kind::BadGeometry);
  }
  if (width <= 0 || height <= 0 || width > k_max_dimension || height > k_max_dimension) {
    return fail(TileBlobError::Kind::BadGeometry);
  }
  // Exact ceil-div on values already bounded above, so neither the add nor the multiply
  // below can overflow an int64.
  const std::int64_t tiles_x = (width + edge - 1) / edge;
  const std::int64_t tiles_y = (height + edge - 1) / edge;
  if (tiles_x * tiles_y > k_max_tiles) {
    return fail(TileBlobError::Kind::BadGeometry);
  }
  TileGeometry g;
  g.edge = static_cast<int>(edge);
  g.width = static_cast<int>(width);
  g.height = static_cast<int>(height);
  g.tiles_x = static_cast<int>(tiles_x);
  g.tiles_y = static_cast<int>(tiles_y);
  return g;
}

std::string hash_tile(std::span<const std::byte> storage_bytes) {
  return to_hex(sha256(storage_bytes), k_tile_hash_bytes);
}

bool is_tile_hash(std::string_view name) {
  if (name.size() != k_tile_hash_chars) {
    return false;
  }
  for (const char c : name) {
    const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    if (!hex) {
      return false; // uppercase included: the name is a canonical spelling, not a value
    }
  }
  return true;
}

std::string tile_blob_uri(std::string_view base, std::string_view hash) {
  std::string out(base);
  if (!out.empty() && out.back() != '/') {
    out.push_back('/');
  }
  out.append(hash.substr(0, 2)); // the 2-hex fan-out directory
  out.push_back('/');
  out.append(hash);
  return out;
}

std::vector<std::byte> shuffle_bytes(std::span<const std::byte> in, std::size_t stride) {
  std::vector<std::byte> out(in.size());
  if (stride <= 1) {
    std::memcpy(out.data(), in.data(), in.size());
    return out;
  }
  const std::size_t groups = in.size() / stride;
  for (std::size_t j = 0; j < stride; ++j) {
    for (std::size_t i = 0; i < groups; ++i) {
      out[j * groups + i] = in[i * stride + j];
    }
  }
  // The remainder (a length that is not a whole number of samples) rides verbatim at the
  // tail. Tiles are always whole samples, so this never fires in production -- it is what
  // makes the pair a bijection on ALL inputs rather than only on well-formed ones, which
  // is what the golden asserts.
  for (std::size_t k = groups * stride; k < in.size(); ++k) {
    out[k] = in[k];
  }
  return out;
}

std::vector<std::byte> unshuffle_bytes(std::span<const std::byte> in, std::size_t stride) {
  std::vector<std::byte> out(in.size());
  if (stride <= 1) {
    std::memcpy(out.data(), in.data(), in.size());
    return out;
  }
  const std::size_t groups = in.size() / stride;
  for (std::size_t j = 0; j < stride; ++j) {
    for (std::size_t i = 0; i < groups; ++i) {
      out[i * stride + j] = in[j * groups + i];
    }
  }
  for (std::size_t k = groups * stride; k < in.size(); ++k) {
    out[k] = in[k];
  }
  return out;
}

std::vector<std::byte> to_storage_bytes(std::span<const float> working, PixelFormat storage) {
  std::vector<std::byte> out;
  switch (storage) {
  case PixelFormat::Rgba32fLinearPremul:
    encode_into<PixelFormat::Rgba32fLinearPremul>(working, out);
    break;
  case PixelFormat::Rgba16fLinearPremul:
    encode_into<PixelFormat::Rgba16fLinearPremul>(working, out);
    break;
  case PixelFormat::Rgba8Srgb:
    // Not a permitted storage format (Decision 4): the reader rejects the token before a
    // document can reach here, and a host that sets it on a `SaveContext` by hand gets an
    // empty blob rather than an out-of-bounds write.
    break;
  }
  return out;
}

std::vector<float> from_storage_bytes(std::span<const std::byte> storage, PixelFormat storage_fmt) {
  std::vector<float> out;
  switch (storage_fmt) {
  case PixelFormat::Rgba32fLinearPremul:
    decode_into<PixelFormat::Rgba32fLinearPremul>(storage, out);
    break;
  case PixelFormat::Rgba16fLinearPremul:
    decode_into<PixelFormat::Rgba16fLinearPremul>(storage, out);
    break;
  case PixelFormat::Rgba8Srgb:
    break; // see to_storage_bytes
  }
  return out;
}

expected<std::vector<std::byte>, TileBlobError>
frame_tile_blob(std::span<const std::byte> storage_bytes, PixelFormat storage) {
  const std::vector<std::byte> shuffled = shuffle_bytes(storage_bytes, bytes_per_sample(storage));
  expected<std::vector<std::byte>, BlobCompressError> frame = compress_blob(shuffled);
  if (!frame) {
    return fail(TileBlobError::Kind::CorruptFrame);
  }
  return std::move(*frame);
}

expected<std::vector<float>, TileBlobError> decode_tile_blob(std::span<const std::byte> frame,
                                                             std::string_view expected_hash,
                                                             PixelFormat storage,
                                                             std::size_t sample_count) {
  if (!is_tile_hash(expected_hash)) {
    return fail(TileBlobError::Kind::BadHash);
  }
  // THE BOUND IS THE CALLER'S, NEVER THE FRAME'S (zstd_dep Decision 6, doc 08:440-442).
  // `sample_count` comes from a validated `TileGeometry`, so this multiply is bounded by
  // k_max_tile_edge^2 * 4 and cannot overflow.
  const std::size_t expected_size = sample_count * bytes_per_sample(storage);
  expected<std::vector<std::byte>, BlobCompressError> raw = decompress_blob(frame, expected_size);
  if (!raw) {
    return fail(TileBlobError::Kind::CorruptFrame);
  }

  // decompress -> unshuffle -> hash -> compare. The blob is SELF-VERIFYING: its name is
  // a checksum of its own storage bytes, so a truncated file, a bit-flipped frame, or a
  // substituted blob lands here as a value and never as wrong pixels (doc 08 Principle 8).
  const std::vector<std::byte> storage_bytes = unshuffle_bytes(*raw, bytes_per_sample(storage));
  if (hash_tile(storage_bytes) != expected_hash) {
    return fail(TileBlobError::Kind::HashMismatch);
  }
  return from_storage_bytes(storage_bytes, storage);
}

} // namespace arbc
