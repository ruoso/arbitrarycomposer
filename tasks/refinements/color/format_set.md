# color.format_set — Pixel format set + surface tags

## TaskJuggler entry

`tasks/15-color.tji` → `color.format_set` ("Pixel format set + surface
tags").

## Effort estimate

1d.

## Inherited dependencies

- `bootstrap.walking_skeleton` — settled (commit `42dbd22`): `arbc::media`
  exists with the single-member `PixelFormat` enum; `arbc::surface`
  carries `format()`; the CPU backend and golden tests exercise
  `Rgba32fLinearPremul` end to end.

## What this task is

Complete the *descriptive* layer of doc 07: extend the core-owned closed
pixel-format set with `Rgba16fLinearPremul` (the designed default working
format) and `Rgba8Srgb`, introduce color-space descriptors (primaries +
transfer function) and premultiplication as explicit tags, and make every
surface carry the full tag triple from creation. **Descriptors and tags
only** — the format-templated kernels, conversions, and f16 storage are
`color.kernels`; the per-composition working-space configuration is
`color.working_space`. Until kernels land, the CPU backend honestly
supports only `Rgba32fLinearPremul` storage and reports anything else as
unsupported.

## Why it needs to be done

Doc 07 rule 1: tags are the part that is prohibitively expensive to
retrofit — every surface must carry format + color space +
premultiplication *from day one*, while the defaults stay cheap to
change. Everything downstream that converts, caches, or imports pixels
keys off these tags; landing them before the kernel work keeps `kernels`
a pure implementation task. Gates M3 (via `color.kernel_goldens` →
`compositor`) and, through `color.working_space`, the nested kind.

## Inputs / context

- `docs/design/07-color-and-pixel-formats.md` — "The model" (the three
  separated concepts and rules 1–4); "Templates and variants" for what is
  deliberately NOT this task.
- `docs/design/09-surfaces-and-backends.md` — surface tag list (pixel
  format, color space, premultiplication, size).
- `src/media/arbc/media/pixel_format.hpp` — current enum + helpers.
- `src/surface/arbc/surface/surface.hpp:11` (`Surface`),
  `src/surface/arbc/surface/backend.hpp:14` (`Backend::make_surface`).
- `src/backend_cpu/arbc/backend_cpu/cpu_backend.hpp` — the only backend;
  `tests/walking_skeleton.t.cpp` — must keep passing unchanged (still
  rgba32f).

## Constraints / requirements

- `media` stays level 1 (depends `base` only); descriptors are constexpr
  value types; the format enum remains core-owned and closed (doc 07's
  guardrail — plugins cannot add formats).
- The tag triple travels as one value type so signatures don't accrete
  parameters: `SurfaceFormat { PixelFormat, ColorSpace, Premultiplied }`
  (name bikesheddable at implementation; one struct, not three loose
  args).
- `ColorSpace` = `{ Primaries, TransferFunction }` enums with `Srgb`
  primaries and `Linear`/`Srgb` transfer as the initial members —
  sufficient for the doc 07 default (linear-light sRGB primaries) and the
  8-bit sRGB mode; wide-gamut/HDR members are later enum additions, not
  redesign.
- Descriptor helpers: `bytes_per_pixel` (16 / 8 / 4 for 32f / 16f / 8),
  `channels_per_pixel` (4), `is_float`, `to_string` — all constexpr where
  possible, unit-tested exhaustively over the closed set.
- **Capability honesty**: `CpuBackend::make_surface` for a format it
  cannot store returns null (documented; migrates to `arbc::expected`
  when `pool.arena_core` lands it in `base` — noted as a follow-edit, not
  a WBS task, since it is a one-line signature change scoped to
  `surfaces.capabilities`). `CpuSurface::cpu_pixels()`'s float-span
  contract remains rgba32f-only and says so.
- Compositing paths assert tag agreement between src/dst (same working
  format) — conversions are `color.kernels`' job; silent mixed-tag
  compositing must be impossible in the meantime.
- The walking-skeleton golden tests keep passing byte-identically (the
  default surfaces stay `Rgba32fLinearPremul` + linear + premultiplied).

## Acceptance criteria

- Unit tests (`src/media/t/`): descriptor helpers over the full closed
  set; `SurfaceFormat` equality/copy semantics; to_string coverage.
- Unit tests (`src/backend_cpu/t/`): make_surface honors supported
  formats and reports unsupported ones; created surfaces echo their full
  tag triple; composite with mismatched tags is rejected (debug assert /
  no-op release — match the degenerate-transform convention at
  `cpu_backend.cpp:34`).
- Claim (register + `enforces:` tag): `07-color-and-pixel-formats#surfaces-carry-tags`
  — every surface exposes its complete tag triple from creation, and the
  reference backend refuses to composite across mismatched tags.
- Gate green (including asan); goldens unchanged.

## Decisions

- **One `SurfaceFormat` value struct** carried through `make_surface` and
  echoed by `Surface`, replacing the bare `PixelFormat` parameter.
  Rejected: three separate virtuals/params (accretes; doc 09 lists the
  tags as a unit); rejected: encoding color space into the PixelFormat
  enum members permanently (doc 07 separates memory layout from
  interpretation — `Rgba8Srgb` names its *storage transfer* as part of
  format identity, but primaries and premultiplication remain orthogonal
  tags).
- **Premultiplication is a tag, not a convention.** The walking skeleton
  hard-codes premultiplied; making it a tag now costs one bool and makes
  the doc 07 rule ("compositing happens on premultiplied alpha in the
  working space") assertable instead of implicit.
- **No f16 storage in this task.** `Rgba16fLinearPremul` becomes
  *describable* (correct bytes_per_pixel, creatable nowhere) before it
  becomes *storable* (kernels task) — capability honesty over stubs.
  Rejected: landing a half-float type now (drags conversion kernels in,
  doubling the task).

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-05.

- Extended `src/media/arbc/media/pixel_format.hpp` with `Rgba16fLinearPremul` and `Rgba8Srgb` members; added descriptor helpers (`bytes_per_pixel`, `channels_per_pixel`, `is_float`, `to_string`) — all constexpr.
- Created `src/media/arbc/media/color_space.hpp` (`Primaries` + `TransferFunction` enums with `Srgb`/`Linear`/`Srgb` members).
- Created `src/media/arbc/media/surface_format.hpp` — `SurfaceFormat { PixelFormat, ColorSpace, Premultiplied }` value struct carried through `make_surface` and echoed by every `Surface`.
- Updated `src/surface/arbc/surface/{surface,backend}.hpp` to accept `SurfaceFormat` instead of bare `PixelFormat`.
- Updated `src/backend_cpu/arbc/backend_cpu/cpu_backend.hpp` and `src/backend_cpu/cpu_backend.cpp` to store the `SurfaceFormat`, return null for unsupported formats, and assert tag agreement on composite.
- Added `src/media/t/pixel_format.t.cpp` — unit tests for descriptor helpers, `SurfaceFormat` equality/copy, and `to_string` coverage (7 cases).
- Extended `src/backend_cpu/t/cpu_backend.t.cpp` with tag-echo, capability-honesty, and mismatch-rejection cases (6 cases).
- Registered claim `07-color-and-pixel-formats#surfaces-carry-tags` in `tests/claims/registry.tsv`; enforced by the new CPU backend tests.
