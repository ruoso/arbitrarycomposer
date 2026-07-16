#pragma once

#include <arbc/arbc_api.h>
#include <arbc/media/pixel_traits.hpp>
#include <arbc/media/surface_format.hpp>

#include <cstddef>
#include <span>

namespace arbc {

// Backend-owned pixel target (doc 09). Opaque handle; allocation goes
// through a Backend, never directly.
class ARBC_API Surface {
public:
  Surface(const Surface&) = delete;
  Surface& operator=(const Surface&) = delete;
  virtual ~Surface();

  virtual int width() const = 0;
  virtual int height() const = 0;

  // The full tag triple (doc 07 rule 1 / doc 09): pixel format, color space,
  // and premultiplication, carried from creation.
  virtual SurfaceFormat format() const = 0;

  // Raw byte access to the CPU-side storage (doc 07/09): available iff the
  // owning backend advertises `cpu_access` and the surface is CPU-backed;
  // empty otherwise (e.g. a GPU surface without readback). Bytes are the honest
  // wire: storage is sized by the pixel format, so typed views must go through
  // the checked accessor below rather than reinterpreting blindly.
  virtual std::span<std::byte> cpu_bytes() = 0;
  virtual std::span<const std::byte> cpu_bytes() const = 0;

  // Checked typed CPU access (doc 07): a span of the format's storage samples,
  // valid iff the requested F matches this surface's pixel-format tag. A
  // mismatch (or no CPU access) yields an empty span -- never a silent
  // reinterpretation across formats. Kernels and format-generic callers reach
  // this via the variant visit helper in typed_span.hpp; a plugin that knows
  // its format calls `surface.span<F>()` directly.
  template <PixelFormat F> std::span<typename PixelTraits<F>::Storage> span() {
    using Storage = typename PixelTraits<F>::Storage;
    if (format().pixel_format != F) {
      return {};
    }
    const std::span<std::byte> bytes = cpu_bytes();
    return {reinterpret_cast<Storage*>(bytes.data()), bytes.size() / sizeof(Storage)};
  }

  template <PixelFormat F> std::span<const typename PixelTraits<F>::Storage> span() const {
    using Storage = typename PixelTraits<F>::Storage;
    if (format().pixel_format != F) {
      return {};
    }
    const std::span<const std::byte> bytes = cpu_bytes();
    return {reinterpret_cast<const Storage*>(bytes.data()), bytes.size() / sizeof(Storage)};
  }

protected:
  Surface() = default;
};

} // namespace arbc
