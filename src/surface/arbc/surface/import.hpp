#pragma once

#include <arbc/media/surface_format.hpp>

#include <cstddef>
#include <functional>
#include <span>

namespace arbc {

// A sync primitive the backend waits on before sampling an imported surface, and
// hands back on completion (doc 09 §Content-provided surfaces): a GPU fence /
// semaphore / EGL sync in a GPU backend. v0.1's CPU reference backend needs no
// wait -- caller memory is immediately readable -- so this is an empty, opaque
// value SHAPED to carry a native handle when a GPU backend lands. Importing with
// a default-constructed `ImportSync` means "no cross-device dependency"; the CPU
// backend ignores it. Shipping the type inert now (rather than deferring it)
// keeps `import_cpu_memory`'s signature stable across the GPU-backend edit.
struct ImportSync {
  friend constexpr bool operator==(const ImportSync&, const ImportSync&) = default;
};

// A request to import caller-owned CPU memory as a backend surface (doc 09:59-61,
// 114-120: "import is wrap-or-copy of caller memory"). The wrap/copy fork is
// DECIDED POLICY (parking-lot triage 2026-07-16, resolving the 2026-07-07
// "Cross-tag convert-at-composite" entry) and deterministic on the tag:
//
//   - `source_format == target_format`  -> WRAP: the returned surface references
//     `memory` zero-copy (no allocation, no convert); `release` fires when THAT
//     surface is destroyed, telling the caller its buffer is free to reclaim or
//     mutate.
//   - `source_format != target_format`  -> COPY: a fresh `target_format`-tagged
//     surface is allocated and the source is CONVERTED into it at import time
//     (the imageseq precedent, Rgba8Srgb -> premultiplied linear working float);
//     `release` fires BEFORE import returns -- the copy is done, the caller's
//     buffer is free at once.
//
// Converting at import keeps a foreign tag from ever reaching the compositor,
// which composites in one working space and does not convert at composite time in
// v0.1 (doc 09:220-230; `RenderResult.provided` requires the working-space tag).
// So the returned surface always carries `target_format`.
//
// Bundled into one value struct (mirroring `RenderRequest` / `BackendCaps`) so
// the seam signature does not accrete parameters as GPU handle kinds and a real
// sync wait arrive. `source_format` must be a format the backend can store (the
// closed working set); a `source_format` the backend cannot interpret, or a
// `memory` span inconsistent with `width*height*bytes_per_pixel(source_format)`,
// is a value error out of `import_cpu_memory`, never an abort.
struct CpuImport {
  std::span<std::byte> memory;
  int width = 0;
  int height = 0;
  SurfaceFormat source_format;
  SurfaceFormat target_format;
  // Fired when the backend no longer needs `memory` (wrap: at the returned
  // surface's destruction; copy: before import returns). NEVER fired if the
  // import faults -- the caller still owns its memory. A null callback is legal
  // (no teardown obligation), matching `SurfaceRef`.
  std::function<void()> release;
  ImportSync sync;
};

} // namespace arbc
