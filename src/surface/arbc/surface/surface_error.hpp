#pragma once

namespace arbc {

// Surface-creation failure surfaced as a value (doc 10: errors as values,
// never thrown, never aborting). `Backend::make_surface` returns
// `expected<unique_ptr<Surface>, SurfaceError>`; a backend that cannot store a
// requested SurfaceFormat yields an error value, never a null handle.
//
// Deliberately starts with the single variant the current code raises. Size
// and allocation faults are added by the task that produces them (arena-backed
// sizing, OOM) rather than speculatively enumerated.
enum class SurfaceError {
  UnsupportedFormat, // the backend cannot store the requested SurfaceFormat
};

} // namespace arbc
