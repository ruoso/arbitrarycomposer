#pragma once

#include <arbc/media/pixel_format.hpp>

#include <span>

namespace arbc {

// Backend-owned pixel target (doc 09). Opaque handle; allocation goes
// through a Backend, never directly.
class Surface {
public:
  Surface(const Surface&) = delete;
  Surface& operator=(const Surface&) = delete;
  virtual ~Surface();

  virtual int width() const = 0;
  virtual int height() const = 0;
  virtual PixelFormat format() const = 0;

  // Typed CPU access where the backend supports it (doc 09); empty when
  // unavailable. Walking-skeleton subset: float32 RGBA only.
  virtual std::span<float> cpu_pixels() = 0;
  virtual std::span<const float> cpu_pixels() const = 0;

protected:
  Surface() = default;
};

} // namespace arbc
