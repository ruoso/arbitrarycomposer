#include <arbc/media/pixel_format.hpp>

namespace arbc {

const char* to_string(PixelFormat format) {
  switch (format) {
  case PixelFormat::Rgba32fLinearPremul:
    return "rgba32f-linear-premul";
  }
  return "unknown";
}

} // namespace arbc
