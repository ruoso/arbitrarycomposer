#pragma once

#include <arbc/surface/backend.hpp>

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace arbc {

class CpuSurface final : public Surface {
public:
  CpuSurface(int width, int height, SurfaceFormat format);

  int width() const override { return d_width; }
  int height() const override { return d_height; }
  SurfaceFormat format() const override { return d_format; }
  // Byte-backed storage sized by the pixel format (doc 07): 16 / 8 / 4 bytes
  // per pixel for 32f / 16f / 8. Typed views go through Surface::span<F>(),
  // which checks the tag before reinterpreting.
  std::span<std::byte> cpu_bytes() override { return d_data; }
  std::span<const std::byte> cpu_bytes() const override { return d_data; }

private:
  int d_width;
  int d_height;
  SurfaceFormat d_format;
  std::vector<std::byte> d_data;
};

// Reference backend (doc 09): CPU memory surfaces, software compositing.
// Exists for correctness and determinism (doc 16: byte-exact goldens).
class CpuBackend final : public Backend {
public:
  CpuBackend() = default;

  BackendCaps capabilities() const override;
  expected<std::unique_ptr<Surface>, SurfaceError> make_surface(int width, int height,
                                                                SurfaceFormat format) override;
  void clear(Surface& surface, float r, float g, float b, float a) override;
  void composite(Surface& dst, const Surface& src, const Affine& src_to_dst,
                 double opacity) override;
  void downsample(Surface& dst, const Surface& src) override;
};

} // namespace arbc
