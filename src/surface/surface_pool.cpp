#include <arbc/surface/surface_pool.hpp>

#include <utility>

namespace arbc {

PooledSurface::PooledSurface(PooledSurface&& other) noexcept
    : d_pool(other.d_pool), d_surface(std::move(other.d_surface)) {
  other.d_pool = nullptr;
}

PooledSurface& PooledSurface::operator=(PooledSurface&& other) noexcept {
  if (this != &other) {
    release();
    d_pool = other.d_pool;
    d_surface = std::move(other.d_surface);
    other.d_pool = nullptr;
  }
  return *this;
}

PooledSurface::~PooledSurface() { release(); }

void PooledSurface::release() noexcept {
  // A moved-from handle has a null surface (unique_ptr moved out) or a null
  // pool; either way there is nothing to return -- no double-release, no leak.
  if (d_pool != nullptr && d_surface != nullptr) {
    d_pool->reclaim(std::move(d_surface));
  }
  d_pool = nullptr;
}

expected<PooledSurface, SurfaceError> SurfacePool::acquire(int width, int height,
                                                           SurfaceFormat format) {
  // Free-list hit: recycle the first released surface matching the exact
  // (width, height, format) key. Only *released* surfaces are on the list, so
  // two concurrent live handles of one key never collide here.
  for (auto it = d_free.begin(); it != d_free.end(); ++it) {
    const Surface& surface = **it;
    if (surface.width() == width && surface.height() == height && surface.format() == format) {
      std::unique_ptr<Surface> recycled = std::move(*it);
      d_free.erase(it);
      return PooledSurface(*this, std::move(recycled));
    }
  }

  // Miss: allocate through the backend, forwarding its SurfaceError verbatim.
  expected<std::unique_ptr<Surface>, SurfaceError> made =
      d_backend.make_surface(width, height, format);
  if (!made.has_value()) {
    return unexpected(made.error());
  }
  return PooledSurface(*this, std::move(*made));
}

void SurfacePool::reclaim(std::unique_ptr<Surface> surface) {
  d_free.push_back(std::move(surface));
}

} // namespace arbc
