// examples/common/png_writer.hpp -- a tiny dependency-free PNG writer shared by
// the host examples (packaging.examples).
//
// PNG's IDAT stream is zlib, and deflate permits STORED (uncompressed) blocks --
// so a complete, valid, universally viewable writer needs only chunk framing
// plus the two checksums (CRC-32 per chunk, Adler-32 over the raw scanlines).
// No compressor, no dependency, and -- the property the CI test pins on --
// byte-deterministic output: the bytes depend only on the pixels, never on a
// compression library's version. This mirrors the repo's precedent of small
// hand-written single-purpose headers (plugins/imdec/third_party/imdec.h).
//
// Not a general-purpose codec: 8-bit straight-alpha RGBA only (color type 6),
// no interlace, no ancillary chunks. An embedder shipping images should use a
// real codec; this exists so the examples' entire dependency surface stays
// `arbc::arbc` (doc 10:34-35).

#pragma once

#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

namespace png_writer {

// CRC-32 (ISO 3309, the PNG chunk checksum), bit-at-a-time -- a lookup table
// buys nothing at example-image sizes.
inline std::uint32_t crc32(std::span<const std::uint8_t> data) {
  std::uint32_t crc = 0xFFFFFFFFU;
  for (const std::uint8_t byte : data) {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
  }
  return crc ^ 0xFFFFFFFFU;
}

// Adler-32 (RFC 1950, the zlib stream checksum).
inline std::uint32_t adler32(std::span<const std::uint8_t> data) {
  std::uint32_t s1 = 1;
  std::uint32_t s2 = 0;
  for (const std::uint8_t byte : data) {
    s1 = (s1 + byte) % 65521U;
    s2 = (s2 + s1) % 65521U;
  }
  return (s2 << 16U) | s1;
}

namespace detail {

inline void put_u32_be(std::vector<std::uint8_t>& out, std::uint32_t value) {
  out.push_back(static_cast<std::uint8_t>(value >> 24U));
  out.push_back(static_cast<std::uint8_t>(value >> 16U));
  out.push_back(static_cast<std::uint8_t>(value >> 8U));
  out.push_back(static_cast<std::uint8_t>(value));
}

// One PNG chunk: 4-byte big-endian payload length, 4-byte type, payload,
// CRC-32 over type + payload.
inline void put_chunk(std::vector<std::uint8_t>& out, const char (&type)[5],
                      std::span<const std::uint8_t> payload) {
  put_u32_be(out, static_cast<std::uint32_t>(payload.size()));
  const std::size_t type_at = out.size();
  out.insert(out.end(), type, type + 4);
  out.insert(out.end(), payload.begin(), payload.end());
  put_u32_be(out, crc32({out.data() + type_at, 4 + payload.size()}));
}

} // namespace detail

// Encode straight-alpha RGBA8 pixels (row-major, top row first, 4 bytes per
// pixel, `rgba.size() == width * height * 4`) as a complete PNG byte stream.
// Returns empty on a size mismatch -- errors are values here too (doc 10).
inline std::vector<std::uint8_t> encode_rgba8(int width, int height,
                                              std::span<const std::uint8_t> rgba) {
  if (width <= 0 || height <= 0 ||
      rgba.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4) {
    return {};
  }

  std::vector<std::uint8_t> png;
  const std::uint8_t signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  png.insert(png.end(), signature, signature + 8);

  // IHDR: dimensions, bit depth 8, color type 6 (RGBA), deflate, filter
  // method 0, no interlace.
  std::vector<std::uint8_t> ihdr;
  detail::put_u32_be(ihdr, static_cast<std::uint32_t>(width));
  detail::put_u32_be(ihdr, static_cast<std::uint32_t>(height));
  ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
  detail::put_chunk(png, "IHDR", ihdr);

  // The raw deflate payload: each scanline is one filter byte (0 = None,
  // exactly the identity -- the point is determinism, not compression)
  // followed by the row's RGBA bytes.
  const std::size_t row_bytes = static_cast<std::size_t>(width) * 4;
  std::vector<std::uint8_t> raw;
  raw.reserve(static_cast<std::size_t>(height) * (row_bytes + 1));
  for (int y = 0; y < height; ++y) {
    raw.push_back(0);
    const std::uint8_t* row = rgba.data() + static_cast<std::size_t>(y) * row_bytes;
    raw.insert(raw.end(), row, row + row_bytes);
  }

  // The zlib stream: 2-byte header (deflate, 32K window, no preset dict; the
  // pair must satisfy the RFC 1950 check -- 0x78 0x01 does), then the raw
  // bytes framed as STORED deflate blocks of at most 65535 bytes (1-byte
  // block header, 16-bit little-endian LEN, NLEN = ~LEN), then Adler-32.
  std::vector<std::uint8_t> idat{0x78, 0x01};
  std::size_t at = 0;
  do {
    const std::size_t block = raw.size() - at < 65535 ? raw.size() - at : 65535;
    const bool final_block = at + block == raw.size();
    idat.push_back(final_block ? 0x01 : 0x00);
    idat.push_back(static_cast<std::uint8_t>(block & 0xFFU));
    idat.push_back(static_cast<std::uint8_t>(block >> 8U));
    idat.push_back(static_cast<std::uint8_t>(~block & 0xFFU));
    idat.push_back(static_cast<std::uint8_t>(~(block >> 8U) & 0xFFU));
    idat.insert(idat.end(), raw.data() + at, raw.data() + at + block);
    at += block;
  } while (at < raw.size());
  detail::put_u32_be(idat, adler32(raw));
  detail::put_chunk(png, "IDAT", idat);

  detail::put_chunk(png, "IEND", {});
  return png;
}

// Encode and write to `path`. Returns false on a size mismatch or any I/O
// failure -- never throws.
inline bool write_rgba8(const char* path, int width, int height,
                        std::span<const std::uint8_t> rgba) {
  const std::vector<std::uint8_t> png = encode_rgba8(width, height, rgba);
  if (png.empty()) {
    return false;
  }
  std::FILE* file = std::fopen(path, "wb");
  if (file == nullptr) {
    return false;
  }
  const bool written = std::fwrite(png.data(), 1, png.size(), file) == png.size();
  return std::fclose(file) == 0 && written;
}

} // namespace png_writer
