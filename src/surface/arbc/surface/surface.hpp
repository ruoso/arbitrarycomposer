#pragma once

#include <arbc/media/surface_format.hpp>

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

  // The full tag triple (doc 07 rule 1 / doc 09): pixel format, color space,
  // and premultiplication, carried from creation.
  virtual SurfaceFormat format() const = 0;

  // Typed CPU access, gated on capability *and* tag (doc 09): a CPU span is
  // available iff the owning backend advertises `cpu_access` in its
  // BackendCaps and the requested view matches this surface's SurfaceFormat
  // tag; empty when unavailable (e.g. a GPU surface without readback).
  // Walking-skeleton subset: the float span is premultiplied linear-light
  // rgba32f only (the reference backend's sole stored format until
  // color.kernels); the multi-format `span<Format>()` accessors land with
  // color.kernels and slot into the same capability-plus-tag check.
  virtual std::span<float> cpu_pixels() = 0;
  virtual std::span<const float> cpu_pixels() const = 0;

protected:
  Surface() = default;
};

} // namespace arbc
