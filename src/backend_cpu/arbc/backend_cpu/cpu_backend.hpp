#pragma once

#include <arbc/arbc_api.h>
#include <arbc/surface/backend.hpp>

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace arbc {

class ARBC_API CpuSurface final : public Surface {
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
class ARBC_API CpuBackend final : public Backend {
public:
  CpuBackend() = default;

  BackendCaps capabilities() const override;
  expected<std::unique_ptr<Surface>, SurfaceError> make_surface(int width, int height,
                                                                SurfaceFormat format) override;
  void clear(Surface& surface, float r, float g, float b, float a) override;
  void composite(Surface& dst, const Surface& src, const Affine& src_to_dst,
                 double opacity) override;
  // The clip-scoped forms (doc 09). The unclipped ops above are defined as the
  // whole-destination-clip case of these, so the backend carries one kernel per
  // operation, not two.
  void clear_rect(Surface& dst, const Rect& device_rect, float r, float g, float b,
                  float a) override;
  void composite_clipped(Surface& dst, const Surface& src, const Affine& src_to_dst, double opacity,
                         const Rect& device_clip) override;
  void downsample(Surface& dst, const Surface& src) override;
  void convert(Surface& dst, const Surface& src) override;
  // Wrap-or-copy import of caller CPU memory (doc 09:59-61,114-120). Equal
  // source/target tags wrap `import.memory` zero-copy; unequal tags copy and
  // convert into a fresh `target_format` surface at import time. Errors as values.
  expected<std::unique_ptr<Surface>, SurfaceError>
  import_cpu_memory(const CpuImport& import) override;
};

} // namespace arbc
