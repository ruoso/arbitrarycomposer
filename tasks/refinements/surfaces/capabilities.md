# surfaces.capabilities — Backend capability flags

## TaskJuggler entry

`tasks/20-surfaces.tji` → `surfaces.capabilities` ("Backend capability
flags"), the first leaf under `task surfaces`. Its siblings
(`provided_surfaces`, `import`, `surface_pool`) all `depends !capabilities`
— this task establishes the capability vocabulary they consume.

## Effort estimate

1d.

## Inherited dependencies

**Settled:**

- `color.format_set` (commit `2696571`) — `SurfaceFormat { PixelFormat,
  ColorSpace, Premultiplied }` (`src/media/arbc/media/surface_format.hpp:26`)
  travels through `Backend::make_surface`
  (`src/surface/arbc/surface/backend.hpp:21`); the CPU backend stores only
  `k_working_rgba32f` and honestly reports anything else as **null**
  (`src/backend_cpu/cpu_backend.cpp:17-29`); composite asserts tag agreement
  (`cpu_backend.cpp:49-52`). This task consumes that null-return seam and
  the tag types.
- `pool.arena_core` — `arbc::expected<T, E>` now exists in the `base` level
  (`src/base/arbc/base/expected.hpp:28`), errors-as-values per doc 10, with
  a move constructor (`expected.hpp:45`) so a move-only value type
  (`std::unique_ptr<Surface>`) is returnable by value. Its presence is the
  precondition `color.format_set` named for the null→`expected` migration it
  deferred here (`tasks/refinements/color/format_set.md:73-78`). The `surface`
  component already `DEPENDS base media` (doc 17; `src/surface/CMakeLists.txt`),
  so the migration adds **no new levelization edge**.

**Pending:** none — both predecessors are landed.

## What this task is

Give the backend contract an explicit, queryable **capability descriptor**
and migrate surface creation onto the errors-as-values path.

Two concrete deliverables:

1. **`BackendCaps` query.** A small value descriptor a backend advertises,
   covering the three axes doc 09 names as "capability flags (CPU access?
   external import? which handle types? sync primitives?)": CPU access,
   import handle types, and sync-primitive support. A backend answers it
   through a new `virtual BackendCaps capabilities() const`. The CPU
   reference backend reports its **honest current** caps: CPU access yes,
   import handles none, sync primitives none — the vocabulary is established
   now; `surfaces.import` flips the import bits on when wrap-or-copy lands.
2. **`make_surface` null → `arbc::expected` migration.** Change
   `make_surface`'s return from `std::unique_ptr<Surface>` (null on
   unsupported) to `arbc::expected<std::unique_ptr<Surface>, SurfaceError>`.
   This is the one-line-signature change `color.format_set` explicitly
   scoped to this task; it is now unblocked because `arbc::expected` has
   landed in `base`. All call sites unwrap the result.

Typed CPU access is documented as gated on **capability + tag**: a surface
hands out a CPU span only when its backend advertises `cpu_access`, and
(already) only when the requested view matches the surface's `SurfaceFormat`
tag. Today only the rgba32f `cpu_pixels()` accessor exists; the multi-format
`span<Format>()` accessors land with `color.kernels`, and slot into the same
capability-plus-tag check.

**Not this task:** the import machinery itself (`surfaces.import`),
content-provided surfaces and their lifetime/transient rules
(`surfaces.provided_surfaces`), pooled allocation (`surfaces.surface_pool`),
and the typed multi-format spans (`color.kernels`). This task lands only the
capability *descriptor*, the *query* seam, and the surface-creation
*error type*.

## Why it needs to be done

Doc 09's backend contract promises that a backend advertises capability
flags and that typed CPU access is available "where the backend supports
it" — but the code has no capability descriptor at all; support is discovered
only by getting an empty span or a null surface. The three sibling tasks
(`provided_surfaces`, `import`, `surface_pool`) each key off "what does this
backend support" and cannot be written honestly against an implicit answer.
Landing the descriptor and the errors-as-values creation path first turns
those siblings into pure feature tasks that read a stable seam.

The migration is also a standing correctness debt: `make_surface` returning
a bare null is the one place in the surface API that still signals failure
out-of-band. Doc 10 mandates errors as values across the public API; every
other allocation seam (`pool`) already returns `expected`. Closing this
keeps the API uniform before three consumers build on it.

## Inputs / context

- `docs/design/09-surfaces-and-backends.md` — "Surface" (lines 8-20: typed
  CPU access "where the backend supports it"); "Backend contract" (lines
  22-27: the capability-flags list — CPU access / external import / handle
  types / sync primitives — and the GPU-reachability rules that keep the
  descriptor forward-compatible); "Content-provided surfaces" (lines 78-83:
  import handle types `GL texture, Vulkan image, DMA-BUF, plain CPU memory`
  "per backend capability flags", plus the optional sync primitive). This
  task's delta lands here (see Decisions).
- `docs/design/07-color-and-pixel-formats.md` — the closed core-owned format
  set is the substrate: capability queries are over a fixed enumerable
  universe, not an open registry (rule 1; guardrail lines 98-103).
- `docs/design/10-tooling-and-packaging.md` — errors-as-values policy the
  `make_surface` migration satisfies.
- `docs/design/17-internal-components.md:51` — `arbc::surface` (L2, DEPENDS
  `base media`): "the backend contract, external import + sync tokens,
  format conversion *interfaces*." Capability descriptor + surface-error
  type belong in `surface`; `arbc::expected` in `base` is already reachable.
- `src/surface/arbc/surface/backend.hpp:19-21` — `make_surface` decl + the
  "Returns null … (capability honesty)" comment to update.
- `src/surface/arbc/surface/surface.hpp:24-29` — `cpu_pixels()` "empty when
  unavailable" contract to tie to `cpu_access`.
- `src/backend_cpu/cpu_backend.cpp:17-29` — the null-return body carrying the
  exact `// Migrates to arbc::expected once pool.arena_core lands it in base
  … scoped to surfaces.capabilities` comment this task discharges.
- `src/base/arbc/base/expected.hpp:28,45` — the target type; move ctor.
- `src/media/arbc/media/surface_format.hpp:26,37` — `SurfaceFormat`,
  `k_working_rgba32f`.
- Call sites to migrate: `src/runtime/offline.cpp:12`,
  `src/compositor/compositor.cpp:54`,
  `src/backend_cpu/t/cpu_backend.t.cpp:66,81,87,90,97,100` (the test at line
  78, "reports formats it cannot store as null", is rewritten against the
  `expected` shape).
- `tasks/refinements/color/format_set.md:73-78` — the deferral this task
  discharges; `:94` — claim-citation style; `:99-117` — the "one value
  struct, not loose args" decision precedent.
- `tests/claims/registry.tsv:25` — sibling claim
  `07-color-and-pixel-formats#surfaces-carry-tags`;
  `:36` — precedent for a faults-as-values claim
  (`15-memory-model#workspace-io-faults-surface-as-values`).

## Constraints / requirements

- **Levelization (doc 17).** `BackendCaps`, the import-handle-type flags, and
  `SurfaceError` live in `arbc::surface` (L2). No new component and no new
  dependency edge — `surface` already `DEPENDS base media`. `arbc::media`
  stays untouched by this task.
- **Capability honesty (doc 07/09).** The CPU backend advertises exactly what
  it does: `cpu_access` true, import handles empty, sync primitives absent.
  No capability is advertised before its machinery exists — advertising
  `CpuMemory` import is `surfaces.import`'s job, not this task's.
- **`make_surface` is errors-as-values (doc 10).** Return
  `arbc::expected<std::unique_ptr<Surface>, SurfaceError>`. `SurfaceError`
  carries at least `UnsupportedFormat` (the current null case). Additional
  variants (size/allocation faults) are added when a consumer produces them
  — do not speculatively enumerate. Never null, never abort.
- **Descriptor is a value struct, not loose flags** (mirroring
  `color.format_set`'s "one struct, not three loose args",
  `format_set.md:99-108`): `BackendCaps { bool cpu_access; ImportHandleTypes
  import_handles; bool sync_primitives; }`. `ImportHandleTypes` is a
  bitmask over the doc-09 handle set (`CpuMemory`, `GlTexture`,
  `VulkanImage`, `DmaBuf`); the CPU backend reports the empty set today.
  Names bikesheddable at implementation.
- **Typed CPU access gated on capability + tag.** Document the invariant on
  `Surface::cpu_pixels()` / `Backend::capabilities()`: a CPU span is
  available iff the backend advertises `cpu_access`; the existing tag check
  (span typed against the surface's `SurfaceFormat`) is unchanged. For the
  sole backend `cpu_access` is always true, so this is a forward-design
  invariant with a live assertion, not a behavior change.
- **Goldens byte-identical.** The migration is signature-only for the
  supported format; `k_working_rgba32f` creation, `clear`, and `composite`
  behavior are untouched. The walking-skeleton goldens must stay
  byte-identical (default surfaces remain `Rgba32fLinearPremul` + linear +
  premultiplied).
- **CI diff coverage ≥90%** on changed lines (doc 16). This is not a
  concurrency-touching task (single-backend, no pool/publish/audio surface),
  so no TSan/stress obligation beyond the existing gate.

## Acceptance criteria

- **Unit tests (`src/surface/t/`, new):** `BackendCaps` value semantics;
  `ImportHandleTypes` bitmask ops (union/test/empty); `SurfaceError`
  round-trips through `arbc::expected` (error branch observable via
  `has_value()`/`error()`).
- **Unit tests (`src/backend_cpu/t/cpu_backend.t.cpp`, extended):**
  - `CpuBackend::capabilities()` reports `cpu_access == true`,
    `import_handles` empty, `sync_primitives == false`.
  - `make_surface(k_working_rgba32f)` returns a value; `*result` is a live
    surface echoing the full tag triple; its `cpu_pixels()` is non-empty and
    consistent with the advertised `cpu_access`.
  - `make_surface` of each unsupported format (f16, srgb8, non-linear tag,
    straight-alpha tag — the cases at `cpu_backend.t.cpp:87-100`) returns
    `unexpected(SurfaceError::UnsupportedFormat)`, never a null value and
    never an abort. (Rewrite of the line-78 "reports … as null" case.)
- **Claim (register + `enforces:` tag):**
  `09-surfaces-and-backends#make-surface-faults-as-value` — a backend that
  cannot store a requested `SurfaceFormat` returns a `SurfaceError` value,
  never null and never aborting; a supported format returns a live surface
  carrying that exact tag triple. Registered in `tests/claims/registry.tsv`,
  enforced from `src/backend_cpu/t/cpu_backend.t.cpp`.
- **Claim (register + `enforces:` tag):**
  `09-surfaces-and-backends#capabilities-are-honest` — a backend's advertised
  `BackendCaps` match its behavior: the reference backend advertises
  `cpu_access` and its stored surfaces yield a non-empty CPU span, while
  `import_handles` is empty and `sync_primitives` false until
  `surfaces.import`/GPU backends land the machinery. Enforced from the CPU
  backend tests.
- **Call-site migration compiles and passes:** `src/runtime/offline.cpp:12`
  and `src/compositor/compositor.cpp:54` unwrap the `expected` (propagating
  or asserting per their existing failure posture) with no behavior change on
  the supported path.
- **Gate green (including asan); walking-skeleton goldens unchanged.**

## Decisions

- **`BackendCaps` is a value descriptor queried through a `capabilities()`
  virtual, not a bitmask on `Backend` nor per-axis virtuals.** Doc 09 calls
  them "capability flags," but the axes are heterogeneous — a bool
  (CPU access), a set (handle types), a bool (sync) — so a small struct with
  a bitmask member reads better than one flat bitmask and accretes less than
  `supports_cpu_access()` / `supported_import_handles()` / … virtuals.
  Follows `color.format_set`'s "one struct, not loose args" precedent
  (`format_set.md:99-108`). *Rejected:* a single `uint32` bitflags (forces
  the heterogeneous axes into one namespace and loses the natural
  set-vs-bool distinction); *rejected:* N per-axis virtuals (accretes the
  contract the doc lists as a unit).
- **CPU backend advertises the honest current subset (`cpu_access` only),
  not its eventual import capability.** Advertising `CpuMemory` import before
  `surfaces.import` implements wrap-or-copy would be exactly the dishonesty
  `color.format_set` refused for unsupported formats. The enum vocabulary
  lands now; the bits flip in the task that lands the machinery. *Rejected:*
  pre-declaring `CpuMemory` import "because the CPU backend will obviously
  support it" — capability honesty over anticipated stubs.
- **`make_surface` returns `expected<unique_ptr<Surface>, SurfaceError>`;
  `SurfaceError` starts with only `UnsupportedFormat`.** Matches every other
  allocation seam (`pool` returns `expected<…, PoolError/RefError>`,
  `src/pool/arbc/pool/refs.hpp:276`) and satisfies doc 10. A single variant
  now avoids speculative error taxonomy — new variants land with the code
  that raises them (arena-backed sizing, OOM). *Rejected:* keeping the null
  return (out-of-band failure the doc-10 policy forbids and the only such
  seam left); *rejected:* `std::optional` (drops the reason, and the reason
  is exactly what a capability-negotiating caller needs).
- **Error enum named `SurfaceError`, placed in `arbc::surface`.** Matches the
  `PoolError`/`RefError` majority spelling; lives with the contract it
  belongs to (surface creation), not in `media`. Name bikesheddable.
- **Design-doc delta to doc 09 (rides the closer's commit).** Doc 09 names
  "capability flags" and typed access rhetorically but pins neither the
  descriptor shape nor `make_surface`'s failure mode — a genuine seam gap.
  The delta adds a short "Capability descriptor" note under **Backend
  contract** fixing `BackendCaps`'s three axes and stating surface creation
  returns `expected<…, SurfaceError>` (errors as values, doc 10). This
  concretizes an already-decided seam rather than shaping the project, so it
  takes **no doc 00 decision-record bullet**; if the closer judges the
  errors-as-values-for-allocation posture project-shaping, a one-line bullet
  is the only escalation.
- **No new WBS task.** The follow-on work is already scoped: `surfaces.import`
  flips `import_handles`/sync bits and builds the machinery;
  `color.kernels` adds the typed `span<Format>()` accessors that reuse the
  capability-plus-tag check; `surfaces.provided_surfaces` and
  `surfaces.surface_pool` consume the descriptor. Nothing here needs a
  successor leaf.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-05.

- Created `src/surface/arbc/surface/capabilities.hpp`: `ImportHandle` enum, `ImportHandleTypes` bitmask wrapper, `BackendCaps` value descriptor (`cpu_access`, `import_handles`, `sync_primitives`).
- Created `src/surface/arbc/surface/surface_error.hpp`: `SurfaceError` enum (`UnsupportedFormat`).
- Migrated `Backend::make_surface` in `src/surface/arbc/surface/backend.hpp` from `unique_ptr<Surface>` (null on failure) to `arbc::expected<unique_ptr<Surface>, SurfaceError>`; added `virtual BackendCaps capabilities() const`.
- Updated `src/surface/arbc/surface/surface.hpp` with capability+tag comment on `cpu_pixels()`.
- Implemented `capabilities()` in `src/backend_cpu/arbc/backend_cpu/cpu_backend.hpp` + `src/backend_cpu/cpu_backend.cpp` (honest caps: `cpu_access=true`, import_handles empty, sync_primitives=false); `make_surface` now returns `expected`.
- Migrated call sites: `src/runtime/offline.cpp`, `src/compositor/compositor.cpp` unwrap `expected`; `src/media/t/pixel_format.t.cpp` minor update.
- Wired `src/surface/CMakeLists.txt` for new headers and test target.
- Created `src/surface/t/capabilities.t.cpp`: unit tests for `ImportHandleTypes` bitmask ops, `BackendCaps` value semantics, `SurfaceError`/`expected` round-trips.
- Extended `src/backend_cpu/t/cpu_backend.t.cpp`: `capabilities()` honesty test; `make_surface` expected/fault cases (supported format returns live surface; unsupported returns `unexpected(SurfaceError::UnsupportedFormat)`).
- Registered two claims in `tests/claims/registry.tsv`: `09-surfaces-and-backends#make-surface-faults-as-value` and `#capabilities-are-honest`, both enforced from `cpu_backend.t.cpp`.
- Updated `docs/design/09-surfaces-and-backends.md` with "Capability descriptor" note under Backend contract.
