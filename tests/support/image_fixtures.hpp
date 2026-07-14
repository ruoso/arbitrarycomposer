#pragma once

// Shared fixtures for the `org.arbc.image` enforcing tests (kinds.image). The tests live
// under `tests/` rather than `plugins/` because `check_claims.py:32` scans only
// `src/`/`tests/`/`testing/`; they construct the kind directly off the plugin's impl
// archive (`arbc-plugin-image-impl`), so no `dlopen` is involved.
//
// The checked-in fixture is a 384x320 binary P6 (`plugins/image/t/fixtures/photo.ppm`),
// deliberately MATERIALLY LARGER than the compositor's 256-px tile
// (`tile_planning.hpp:145`): the provided-surface claim -- that a pull hands back the
// requested region and not the whole decoded frame (kinds/image.md Decision 1) -- is only
// meaningful over an image that genuinely spans several tiles.
//
// The P6 reader below is an INDEPENDENT in-TU reference decoder, not the plugin's. That is
// what makes the goldens computed-reference rather than self-confirming: the test derives
// the expected working-space master from the fixture bytes by its own path, and the kind's
// pyramid must agree with it byte-for-byte. (The test tree cannot include `<imdec.h>`
// anyway -- the decode dep is linked PRIVATE and its include dir stops at the plugin, which
// is the containment the codec line demands.)

#include <arbc/base/geometry.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/kind_image/image_content.hpp>
#include <arbc/media/image_resampler.hpp>
#include <arbc/media/pixel_format.hpp>
#include <arbc/media/pixel_traits.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace arbc::image::testfix {

// The fixture's native extent (kept in sync with the checked-in .ppm by
// `reference_master`, which REQUIREs the decoded dimensions match).
inline constexpr int k_width = 384;
inline constexpr int k_height = 320;

inline std::string fixture_path() { return std::string(ARBC_IMAGE_FIXTURE_DIR) + "/photo.ppm"; }

inline std::string read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return {};
  }
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

inline std::string fixture_bytes() { return read_file(fixture_path()); }

// A working-space (premultiplied linear rgba32f) image, the reference the goldens compare
// against. `px` is tightly packed, row-major, 4 floats per pixel.
struct RefImage {
  int width{0};
  int height{0};
  std::vector<float> px;

  WorkingPixel at(int x, int y) const {
    const int cx = std::clamp(x, 0, width - 1); // clamp-to-edge, as the pyramid does
    const int cy = std::clamp(y, 0, height - 1);
    const std::size_t o = (static_cast<std::size_t>(cy) * static_cast<std::size_t>(width) +
                           static_cast<std::size_t>(cx)) *
                          4U;
    return {px[o], px[o + 1], px[o + 2], px[o + 3]};
  }
};

// The independent P6 reference decoder: binary PPM, maxval 255, `#` comments allowed. Empty
// on anything else -- it parses only the fixture shape it is the reference for.
inline RefImage decode_p6(const std::string& bytes) {
  RefImage img;
  std::size_t i = 0;
  const auto skip_ws = [&] {
    while (i < bytes.size()) {
      if (bytes[i] == '#') {
        while (i < bytes.size() && bytes[i] != '\n') {
          ++i;
        }
      } else if (std::isspace(static_cast<unsigned char>(bytes[i])) != 0) {
        ++i;
      } else {
        return;
      }
    }
  };
  const auto read_uint = [&]() -> int {
    skip_ws();
    int v = 0;
    while (i < bytes.size() && std::isdigit(static_cast<unsigned char>(bytes[i])) != 0) {
      v = v * 10 + (bytes[i] - '0');
      ++i;
    }
    return v;
  };
  if (bytes.size() < 2 || bytes[0] != 'P' || bytes[1] != '6') {
    return img;
  }
  i = 2;
  const int w = read_uint();
  const int h = read_uint();
  const int maxval = read_uint();
  ++i; // exactly one whitespace byte separates the header from the raster
  const auto need = static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 3U;
  if (w <= 0 || h <= 0 || maxval != 255 || i + need > bytes.size()) {
    return img;
  }

  img.width = w;
  img.height = h;
  img.px.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4U);
  for (std::size_t p = 0; p < static_cast<std::size_t>(w) * static_cast<std::size_t>(h); ++p) {
    // The plugin's decoder yields tightly-packed 8-bit RGBA with alpha forced to 255; the
    // conversion into the working space then runs Rgba8Srgb -> WorkingPixel ->
    // Rgba32fLinearPremul (convert at DECODE, never at composite).
    const unsigned char rgba[4] = {static_cast<unsigned char>(bytes[i + p * 3 + 0]),
                                   static_cast<unsigned char>(bytes[i + p * 3 + 1]),
                                   static_cast<unsigned char>(bytes[i + p * 3 + 2]), 255U};
    const WorkingPixel wp = PixelTraits<PixelFormat::Rgba8Srgb>::decode(rgba);
    PixelTraits<PixelFormat::Rgba32fLinearPremul>::encode(wp, &img.px[p * 4]);
  }
  return img;
}

// Level 0 of the reference pyramid: the fixture, decoded by the reference path above.
inline RefImage reference_master() { return decode_p6(fixture_bytes()); }

// One 2:1 Lanczos-3 half-band decimation of `child`, through `media`'s frozen bank with the
// clamp-to-edge convention -- the reference rung the downscale golden pins the kind's own
// pyramid against.
inline RefImage decimate(const RefImage& child) {
  RefImage up;
  up.width = std::max(1, (child.width + 1) / 2);
  up.height = std::max(1, (child.height + 1) / 2);
  up.px.resize(static_cast<std::size_t>(up.width) * static_cast<std::size_t>(up.height) * 4U);
  for (int y = 0; y < up.height; ++y) {
    for (int x = 0; x < up.width; ++x) {
      const WorkingPixel c =
          decimate_half_band(x, y, [&](int sx, int sy) { return child.at(sx, sy); });
      const std::size_t o = (static_cast<std::size_t>(y) * static_cast<std::size_t>(up.width) +
                             static_cast<std::size_t>(x)) *
                            4U;
      up.px[o] = c[0];
      up.px[o + 1] = c[1];
      up.px[o + 2] = c[2];
      up.px[o + 3] = c[3];
    }
  }
  return up;
}

// The fixture's encoded bytes as the decoder's input span.
inline PyramidPtr decode_fixture() {
  const std::string bytes = fixture_bytes();
  return Pyramid::decode(std::span<const unsigned char>(
      reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()));
}

// A live `ImageContent` over the fixture, built the way a load builds it.
inline std::unique_ptr<ImageContent> make_content(std::string_view authored = "assets/photo.ppm") {
  return std::make_unique<ImageContent>(std::string(authored), decode_fixture());
}

// A live `ImageContent` whose asset is UNAVAILABLE (missing / unreadable / undecodable, or
// no `AssetSource` installed at all): the authored URI is kept, there are no pixels.
inline std::unique_ptr<ImageContent>
make_unavailable_content(std::string_view authored = "assets/missing.png") {
  return std::make_unique<ImageContent>(std::string(authored), PyramidPtr{});
}

} // namespace arbc::image::testfix
