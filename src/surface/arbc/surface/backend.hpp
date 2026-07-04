#pragma once

#include <arbc/base/transform.hpp>
#include <arbc/surface/surface.hpp>

#include <memory>

namespace arbc {

// Backend contract (doc 09): surfaces and the composite operation set. The
// core never loops over pixels itself; all composite operations route
// through here so the abstraction maps onto GPU command lists later.
class Backend {
public:
  Backend(const Backend&) = delete;
  Backend& operator=(const Backend&) = delete;
  virtual ~Backend();

  virtual std::unique_ptr<Surface> make_surface(int width, int height, PixelFormat format) = 0;

  // Premultiplied working-space color (doc 07).
  virtual void clear(Surface& surface, float r, float g, float b, float a) = 0;

  // Source-over composite of `src` onto `dst` under the src->dst mapping,
  // on premultiplied alpha, scaled by `opacity`.
  virtual void composite(Surface& dst, const Surface& src, const Affine& src_to_dst,
                         double opacity) = 0;

protected:
  Backend() = default;
};

} // namespace arbc
