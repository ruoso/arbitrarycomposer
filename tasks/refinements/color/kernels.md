# color.kernels — Templated kernels + variant dispatch

## TaskJuggler entry

`tasks/15-color.tji` → `color.kernels` ("Templated kernels + variant
dispatch").

## Effort estimate

3d.

## Inherited dependencies

- `color.format_set` — pending at refinement time: provides the closed
  three-format set (`Rgba32fLinearPremul`, `Rgba16fLinearPremul`,
  `Rgba8Srgb`), the `SurfaceFormat` tag triple on every surface, and the
  mixed-tag compositing rejection this task upgrades into conversion.

## What this task is

The doc 07 "yes, inside the kernels" half: format-templated CPU kernels
(fill, premultiply, source-over blend, format/transfer conversion) that
are monomorphized per format and dispatched **once per operation** via a
variant over typed spans — plus the storage/access changes that make the
non-float32 formats real: byte-backed `CpuSurface` storage sized by
format, software f16 conversion, and checked typed accessors replacing
the skeleton's float-span shortcut. Filtered resampling stays in
`color.resampling`; the per-format byte-exact golden matrix stays in
`color.kernel_goldens`.

## Why it needs to be done

Doc 07's central design (templates inside, erased boundary outside) is
only claimable once the kernels exist; the 16f default working format and
the 8-bit fast mode are unshippable without them; `color.resampling`,
`color.kernel_goldens`, `color.working_space`'s default flip, and every
conversion edge (imports, nesting boundaries, display output) build on
this task.

## Inputs / context

- `docs/design/07-color-and-pixel-formats.md` — "Templates and variants:
  where compile-time formats live" (the `composite_tile` /
  `AnySurfaceRef` sketch is this task's blueprint), the guardrail (closed
  set keeps the variant total), and rule 2 (compositing on premultiplied
  alpha in the working space).
- `src/backend_cpu/cpu_backend.cpp` — current float32-only clear/composite
  loops to be re-expressed as kernels; the source-over math and its tests
  (`src/backend_cpu/t/cpu_backend.t.cpp`) carry over as the 32f
  instantiation's spec.
- `src/surface/arbc/surface/surface.hpp` — `cpu_pixels()` float-span
  contract this task supersedes.
- Call sites of `cpu_pixels()`: `src/kind_solid/solid_content.cpp`,
  `tests/walking_skeleton.t.cpp` helpers — to migrate to typed accessors.

## Constraints / requirements

- Kernels are function templates over a `PixelTraits<Format>` descriptor
  (storage type, encode/decode to working floats, transfer handling);
  the variant dispatch happens once per tile-sized operation, never per
  pixel (doc 07). The variant is total over the closed set —
  `std::visit` without a default arm, so adding a format without kernels
  is a compile error, not a runtime hole.
- **f16 portably**: storage is `std::uint16_t`; encode/decode via
  branch-light software conversion (round-to-nearest-even), no reliance
  on `_Float16`/`std::float16_t` (MSVC parity). SIMD is out of scope;
  the kernel structure (contiguous typed spans) must not preclude it.
- **8-bit sRGB honestly**: decode applies the sRGB EOTF to working
  linear floats, encode the inverse with correct rounding; blending
  8-bit surfaces happens in linear working floats per doc 07 rule 2 —
  the "fast mode" is about memory, not about blending in gamma space.
- Surface access: `Surface::cpu_pixels()` (float span) is replaced by
  byte-level `cpu_bytes()` plus checked `span<PixelTraits<F>::storage>`
  typed accessors that verify the tag and a format-generic visit helper
  (doc 07's plugin story). All existing call sites migrate in this task;
  the walking-skeleton goldens must still pass byte-identically for 32f.
- Conversion kernels route through working floats (format → linear
  premultiplied float → format), giving N formats with 2N codecs instead
  of N×N pairs (doc 07's "conversions route through the working space").
- Backend `clear`/`composite` re-dispatch through the kernels for all
  three formats; same-format composite only (mixed-tag remains rejected —
  the conversion *call sites* are later tasks' wiring: imports, nesting,
  display-out).
- Precision contracts stated in code: 8-bit round-trips
  (encode(decode(x)) == x for all 256 values per channel — exhaustive
  test); f16 round-trips within one ULP of f16; 32f is exact.

## Acceptance criteria

- Unit tests: exhaustive 8-bit round-trip; f16 encode/decode against a
  reference table of edge cases (±0, subnormals, inf, NaN payloads,
  1.0, 0.5); blend properties per format (transparent source = identity;
  opaque source replaces; premultiplied source-over matches the 32f
  reference within the format's stated precision).
- The existing 32f composite tests keep passing unchanged; the
  walking-skeleton goldens stay byte-identical.
- Claim (register + `enforces:`): `07-color-and-pixel-formats#blending-in-linear-working-space`
  — blending an `Rgba8Srgb` surface produces the linear-light result
  (decode→blend→encode), not the gamma-space result; the test constructs
  a case where the two differ decisively.
- Gate green including asan; `color.kernel_goldens` (follow-on task)
  gets a note in the Status block listing the kernel entry points it
  must cover.

## Decisions

- **`PixelTraits<Format>` + working-float codec design** (2N codecs via
  the working space) per doc 07. Rejected: direct format-pair conversion
  matrix (N×N growth against a set that doc 07 says will grow).
- **Software f16** for portability and determinism across the three
  compilers (doc 16's byte-exact discipline extends to conversion
  results); hardware f16 becomes an internal optimization later without
  contract change. Rejected: `_Float16` conditionals now (MSVC gap would
  fork golden results by platform).
- **Replace `cpu_pixels()` rather than deprecate-and-keep**: pre-1.0, one
  access idiom (checked typed spans + visit helper) is worth the two-file
  migration; keeping the float shortcut would let new code bypass tag
  checking silently.
- **Same-format-only composite retained** this task: conversion is a
  kernel, not yet a composite feature — wiring auto-convert into
  composite belongs to the tasks that own the edges (doc 07 rule 4's
  nesting boundary lands with `kinds.nested`; display-out with the
  interactive renderer). Rejected: auto-converting composite now
  (invisible conversions before the golden matrix exists).

## Open questions

(none — all decided)

## Status

_pending implementation_
