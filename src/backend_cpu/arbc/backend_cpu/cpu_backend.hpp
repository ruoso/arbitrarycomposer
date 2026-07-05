#pragma once

#include <arbc/surface/backend.hpp>

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
  std::span<float> cpu_pixels() override { return d_data; }
  std::span<const float> cpu_pixels() const override { return d_data; }

private:
  int d_width;
  int d_height;
  SurfaceFormat d_format;
  std::vector<float> d_data;
};

// Reference backend (doc 09): CPU memory surfaces, software compositing.
// Exists for correctness and determinism (doc 16: byte-exact goldens).
class CpuBackend final : public Backend {
public:
  CpuBackend() = default;

  std::unique_ptr<Surface> make_surface(int width, int height, SurfaceFormat format) override;
  void clear(Surface& surface, float r, float g, float b, float a) override;
  void composite(Surface& dst, const Surface& src, const Affine& src_to_dst,
                 double opacity) override;
};

} // namespace arbc
