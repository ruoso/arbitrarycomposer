#include <arbc/media/pixel_format.hpp>

namespace arbc {

const char* to_string(PixelFormat format) {
  switch (format) {
  case PixelFormat::Rgba32fLinearPremul:
    return "rgba32f-linear-premul";
  case PixelFormat::Rgba16fLinearPremul:
    return "rgba16f-linear-premul";
  case PixelFormat::Rgba8Srgb:
    return "rgba8-srgb";
  }
  return "unknown";
}

} // namespace arbc
