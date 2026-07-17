// The host-examples half of the out-of-tree consumer (packaging.examples).
//
// run_staged_install.cmake configured, built, and RAN examples/host-offline and
// examples/host-interactive -- two standalone foreign projects whose whole
// dependency surface is find_package(arbc CONFIG) -- against the SAME staged
// prefix, and hands their PNG artifact paths here. This TU is the enforcing
// test for doc 16:88-90's examples tier ("every code sample ... compiles and
// runs in CI"): it validates each artifact structurally (PNG signature, IHDR,
// the CRC-32 of every chunk, the zlib stored-deflate framing and its Adler-32)
// and then pins the PIXELS byte-exactly against expectations computed from
// first principles: the examples' scenes are axis-aligned solids with exact
// binary-float colors, so premultiplied source-over plus the library's own
// PixelTraits<Rgba8Srgb>::encode reproduces every expected byte with no
// tolerance anywhere -- the CPU backend is deterministic and the writer's
// stored-deflate output depends on nothing but the pixels (doc 16:48-53).
//
// The checksum/inflate code here is deliberately an independent
// implementation, not an include of examples/common/png_writer.hpp -- a test
// that verified the writer with the writer would prove nothing.
//
// No Catch2: this TU builds in the embedder's core-only configuration too.

#include <arbc/media/pixel_traits.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

std::vector<std::uint8_t> read_file(const char* path) {
  std::vector<std::uint8_t> bytes;
  std::FILE* file = std::fopen(path, "rb");
  if (file == nullptr) {
    return bytes;
  }
  std::uint8_t chunk[4096];
  std::size_t got = 0;
  while ((got = std::fread(chunk, 1, sizeof chunk, file)) > 0) {
    bytes.insert(bytes.end(), chunk, chunk + got);
  }
  std::fclose(file);
  return bytes;
}

std::uint32_t be32(const std::uint8_t* p) {
  return (static_cast<std::uint32_t>(p[0]) << 24U) | (static_cast<std::uint32_t>(p[1]) << 16U) |
         (static_cast<std::uint32_t>(p[2]) << 8U) | static_cast<std::uint32_t>(p[3]);
}

// Independent CRC-32 (ISO 3309) and Adler-32 (RFC 1950), verified against the
// values the artifact carries.
std::uint32_t crc32(const std::uint8_t* data, std::size_t size) {
  std::uint32_t crc = ~0U;
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1U) != 0 ? (crc >> 1U) ^ 0xEDB88320U : crc >> 1U;
    }
  }
  return ~crc;
}

std::uint32_t adler32(const std::uint8_t* data, std::size_t size) {
  std::uint32_t s1 = 1;
  std::uint32_t s2 = 0;
  for (std::size_t i = 0; i < size; ++i) {
    s1 = (s1 + data[i]) % 65521U;
    s2 = (s2 + s1) % 65521U;
  }
  return (s2 << 16U) | s1;
}

// Decode one of the writers' PNGs: signature, IHDR (must be `dim` x `dim`,
// 8-bit RGBA, no interlace), every chunk's CRC-32, the stored-deflate zlib
// stream (no inflate needed: BTYPE 0 blocks are the raw bytes), its Adler-32,
// and the all-None filter bytes. Fills `rgba` (4 bytes per pixel, row-major).
bool decode_png(const char* label, const char* path, std::uint32_t dim,
                std::vector<std::uint8_t>& rgba) {
  const std::vector<std::uint8_t> png = read_file(path);
  if (png.empty()) {
    std::printf("%s: cannot read %s\n", label, path);
    return false;
  }
  const std::uint8_t signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  if (png.size() < 8 || !std::equal(signature, signature + 8, png.begin())) {
    std::printf("%s: bad PNG signature\n", label);
    return false;
  }

  bool saw_ihdr = false;
  bool saw_iend = false;
  std::vector<std::uint8_t> idat;
  std::size_t at = 8;
  while (at + 12 <= png.size() && !saw_iend) {
    const std::uint32_t length = be32(&png[at]);
    if (at + 12 + length > png.size()) {
      std::printf("%s: chunk at %zu overruns the file\n", label, at);
      return false;
    }
    const std::uint8_t* type = &png[at + 4];
    const std::uint8_t* data = &png[at + 8];
    if (be32(data + length) != crc32(type, 4 + length)) {
      std::printf("%s: chunk %.4s carries a wrong CRC-32\n", label,
                  reinterpret_cast<const char*>(type));
      return false;
    }
    if (std::equal(type, type + 4, "IHDR")) {
      if (length != 13 || be32(data) != dim || be32(data + 4) != dim || data[8] != 8 ||
          data[9] != 6 || data[10] != 0 || data[11] != 0 || data[12] != 0) {
        std::printf("%s: IHDR is not %ux%u 8-bit RGBA, non-interlaced\n", label, dim, dim);
        return false;
      }
      saw_ihdr = true;
    } else if (std::equal(type, type + 4, "IDAT")) {
      idat.insert(idat.end(), data, data + length);
    } else if (std::equal(type, type + 4, "IEND")) {
      saw_iend = true;
    }
    at += 12 + length;
  }
  if (!saw_ihdr || !saw_iend || idat.empty()) {
    std::printf("%s: IHDR/IDAT/IEND missing\n", label);
    return false;
  }

  // The zlib stream: header pair passing the RFC 1950 check with deflate as
  // the method, stored blocks only, Adler-32 of the raw bytes at the end.
  if (idat.size() < 6 || (idat[0] & 0x0FU) != 8 ||
      (static_cast<std::uint32_t>(idat[0]) * 256 + idat[1]) % 31 != 0) {
    std::printf("%s: IDAT is not a zlib/deflate stream\n", label);
    return false;
  }
  std::vector<std::uint8_t> raw;
  std::size_t pos = 2;
  bool final_block = false;
  while (!final_block) {
    if (pos + 5 > idat.size() - 4) {
      std::printf("%s: deflate block header overruns the stream\n", label);
      return false;
    }
    const std::uint8_t header = idat[pos];
    final_block = (header & 1U) != 0;
    if ((header >> 1U) != 0) {
      std::printf("%s: deflate block is not STORED (the writers emit only stored blocks)\n", label);
      return false;
    }
    const std::uint32_t len = idat[pos + 1] | (static_cast<std::uint32_t>(idat[pos + 2]) << 8U);
    const std::uint32_t nlen = idat[pos + 3] | (static_cast<std::uint32_t>(idat[pos + 4]) << 8U);
    if ((len ^ nlen) != 0xFFFFU || pos + 5 + len > idat.size() - 4) {
      std::printf("%s: stored block framing is corrupt\n", label);
      return false;
    }
    raw.insert(raw.end(), &idat[pos + 5], &idat[pos + 5] + len);
    pos += 5 + len;
  }
  if (pos + 4 != idat.size()) {
    std::printf("%s: trailing bytes after the final stored block\n", label);
    return false;
  }
  if (be32(&idat[pos]) != adler32(raw.data(), raw.size())) {
    std::printf("%s: zlib Adler-32 mismatch\n", label);
    return false;
  }

  // Scanlines: one filter byte (must be 0 = None) plus dim * 4 RGBA bytes.
  const std::size_t row_bytes = static_cast<std::size_t>(dim) * 4;
  if (raw.size() != static_cast<std::size_t>(dim) * (row_bytes + 1)) {
    std::printf("%s: decompressed size is not %u scanlines\n", label, dim);
    return false;
  }
  rgba.clear();
  rgba.reserve(static_cast<std::size_t>(dim) * row_bytes);
  for (std::uint32_t y = 0; y < dim; ++y) {
    const std::uint8_t* row = raw.data() + static_cast<std::size_t>(y) * (row_bytes + 1);
    if (row[0] != 0) {
      std::printf("%s: scanline %u uses filter %u, expected None\n", label, y, row[0]);
      return false;
    }
    rgba.insert(rgba.end(), row + 1, row + 1 + row_bytes);
  }
  return true;
}

// Premultiplied source-over (doc 07): what the deterministic CPU backend
// computes for the examples' exact binary-float constants.
arbc::WorkingPixel over(const arbc::WorkingPixel& src, const arbc::WorkingPixel& dst) {
  const float inv = 1.0F - src[3];
  return {src[0] + dst[0] * inv, src[1] + dst[1] * inv, src[2] + dst[2] * inv,
          src[3] + dst[3] * inv};
}

// Validate one artifact against a two-region expectation: `top` composited
// over `base` wherever x < split && y < split, `base` alone elsewhere --
// encoded to straight-alpha sRGB8 through the SAME public PixelTraits encode
// the examples used, so expected bytes are derived, never frozen.
bool check_artifact(const char* label, const char* path, std::uint32_t dim, std::uint32_t split,
                    const arbc::WorkingPixel& base, const arbc::WorkingPixel& top) {
  std::vector<std::uint8_t> rgba;
  if (!decode_png(label, path, dim, rgba)) {
    return false;
  }
  using Srgb8 = arbc::PixelTraits<arbc::PixelFormat::Rgba8Srgb>;
  std::uint8_t expected_base[4];
  std::uint8_t expected_mixed[4];
  Srgb8::encode(base, expected_base);
  Srgb8::encode(over(top, base), expected_mixed);
  for (std::uint32_t y = 0; y < dim; ++y) {
    for (std::uint32_t x = 0; x < dim; ++x) {
      const std::uint8_t* expected = (x < split && y < split) ? expected_mixed : expected_base;
      const std::uint8_t* actual = &rgba[4 * (static_cast<std::size_t>(y) * dim + x)];
      for (int c = 0; c < 4; ++c) {
        if (actual[c] != expected[c]) {
          std::printf("%s: pixel (%u, %u) channel %d is %u, expected %u\n", label, x, y, c,
                      actual[c], expected[c]);
          return false;
        }
      }
    }
  }
  std::printf("%s: %s is a valid PNG with byte-exact pixels\n", label, path);
  return true;
}

} // namespace

// enforces: 16-sdlc-and-quality#shipped-examples-compile-and-run-in-ci
int main() {
  // The scenes as the examples define them (see examples/host-offline/main.cpp
  // and examples/host-interactive/main.cpp): premultiplied working-space
  // constants, all exact in binary float.
  //
  // host-offline renders at the identity camera: the half-green overlay spans
  // composition [0,16)^2, which is device [0,16)^2.
  const bool offline_ok = check_artifact("host_example_artifacts[offline]", ARBC_HOST_OFFLINE_PNG,
                                         32, 16, arbc::WorkingPixel{1.0F, 0.0F, 0.0F, 1.0F},
                                         arbc::WorkingPixel{0.0F, 0.5F, 0.0F, 0.5F});

  // host-interactive's gesture tape nets to a pan of (-64, -64) -- the two
  // zooms cancel exactly -- and its closing PLACEMENT gesture drags the panel's
  // layer by (64, 64), so the half-gray panel (composition [64,320)^2 after the
  // drag) lands exactly on device [0,256)^2; the unbounded backdrop covers the
  // rest at any camera.
  const bool interactive_ok = check_artifact(
      "host_example_artifacts[interactive]", ARBC_HOST_INTERACTIVE_PNG, 512, 256,
      arbc::WorkingPixel{0.0F, 0.0F, 0.25F, 1.0F}, arbc::WorkingPixel{0.5F, 0.5F, 0.5F, 0.5F});

  return offline_ok && interactive_ok ? 0 : 1;
}
