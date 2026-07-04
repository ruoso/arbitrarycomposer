#pragma once

#include <cstddef>

namespace arbc {

// The core-owned closed pixel format set (doc 07). Walking-skeleton subset:
// the reference pipeline composites in premultiplied linear-light float32;
// Rgba16f/Rgba8Srgb land with the real kernel set.
enum class PixelFormat {
  Rgba32fLinearPremul,
};

constexpr std::size_t channels_per_pixel(PixelFormat) { return 4; }

const char* to_string(PixelFormat format);

} // namespace arbc
