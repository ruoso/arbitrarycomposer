#pragma once

#include <arbc/base/expected.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/surface/backend.hpp>
#include <arbc/surface/surface.hpp>
#include <arbc/surface/surface_error.hpp>

#include <memory>
#include <vector>

namespace arbc {

class SurfacePool;

// Move-only RAII handle to a surface acquired from a SurfacePool (doc 09).
// Destroying (or move-assigning over) the handle returns the surface to the
// pool's free list instead of freeing it, so a subsequent same-key acquire
// recycles it. The wrapped surface is a live target of the requested
// (width, height, SurfaceFormat); the handle scopes its useful life exactly
// as the former per-request `unique_ptr<Surface>` did.
class PooledSurface {
public:
  PooledSurface(const PooledSurface&) = delete;
  PooledSurface& operator=(const PooledSurface&) = delete;

  PooledSurface(PooledSurface&& other) noexcept;
  PooledSurface& operator=(PooledSurface&& other) noexcept;
  ~PooledSurface();

  Surface& operator*() const noexcept { return *d_surface; }
  Surface* operator->() const noexcept { return d_surface.get(); }
  Surface& get() const noexcept { return *d_surface; }

private:
  friend class SurfacePool;

  PooledSurface(SurfacePool& pool, std::unique_ptr<Surface> surface) noexcept
      : d_pool(&pool), d_surface(std::move(surface)) {}

  // Return the wrapped surface to the pool (no-op if already released/moved).
  void release() noexcept;

  SurfacePool* d_pool;
  std::unique_ptr<Surface> d_surface;
};

// Core-owned surface recycling layer (doc 09) composing over a single
// Backend's `make_surface`. `acquire` hands out a PooledSurface keyed by
// exact (width, height, SurfaceFormat): a free-list hit recycles a
// previously-released surface, a miss allocates through the backend and
// forwards its SurfaceError. Release (handle destruction) returns the
// surface to the free list rather than freeing it.
//
// Render-thread-confined (doc 09 Threading note): acquire/release run only
// where allocation runs, so the pool holds no lock and is not thread-safe by
// design. Not copyable or movable -- callers hold it by reference.
class SurfacePool {
public:
  explicit SurfacePool(Backend& backend) : d_backend(backend) {}

  SurfacePool(const SurfacePool&) = delete;
  SurfacePool& operator=(const SurfacePool&) = delete;
  SurfacePool(SurfacePool&&) = delete;
  SurfacePool& operator=(SurfacePool&&) = delete;
  ~SurfacePool() = default;

  // Acquire a live surface of exactly (width, height, format). Errors as
  // values (doc 10): on a free-list miss the backend allocates and any
  // SurfaceError is forwarded verbatim, never null and never an abort.
  //
  // A recycled surface carries **undefined contents** -- its prior user's
  // pixels -- so the caller must clear or fully overwrite before reading
  // (the compositor clears each temp). The pool is a pure allocation-recycling
  // layer and does not clear.
  expected<PooledSurface, SurfaceError> acquire(int width, int height, SurfaceFormat format);

private:
  friend class PooledSurface;

  void reclaim(std::unique_ptr<Surface> surface);

  Backend& d_backend;
  std::vector<std::unique_ptr<Surface>> d_free;
};

} // namespace arbc
