#pragma once

#include <cstddef>

namespace arbc {

// The core-owned closed pixel format set (doc 07): a surface's memory layout
// -- channel type, count, and (where it is part of format identity) the
// storage transfer. The set is closed and core-owned by design (doc 07
// guardrail): plugins cannot add formats; a genuinely new format class is a
// core enum + kernel addition, a minor version bump.
//
// Descriptive only in this task: f16 and 8-bit storage plus the conversion
// kernels are `color.kernels`. The reference backend can *describe* every
// member here but currently *stores* only Rgba32fLinearPremul.
enum class PixelFormat {
  Rgba32fLinearPremul, // walking-skeleton working format: 4x 32-bit float
  Rgba16fLinearPremul, // designed default working format (storable: kernels)
  Rgba8Srgb,           // 8-bit sRGB fast mode (storable: kernels)
};

// Bytes one pixel occupies in this format: 16 / 8 / 4 for 32f / 16f / 8.
constexpr std::size_t bytes_per_pixel(PixelFormat format) {
  switch (format) {
  case PixelFormat::Rgba32fLinearPremul:
    return 16;
  case PixelFormat::Rgba16fLinearPremul:
    return 8;
  case PixelFormat::Rgba8Srgb:
    return 4;
  }
  return 0;
}

// Every format in the set is 4-channel RGBA.
constexpr std::size_t channels_per_pixel(PixelFormat) { return 4; }

// Whether samples are stored as floating point (true for the 32f/16f working
// formats, false for the 8-bit integer fast mode).
constexpr bool is_float(PixelFormat format) {
  switch (format) {
  case PixelFormat::Rgba32fLinearPremul:
  case PixelFormat::Rgba16fLinearPremul:
    return true;
  case PixelFormat::Rgba8Srgb:
    return false;
  }
  return false;
}

const char* to_string(PixelFormat format);

} // namespace arbc
