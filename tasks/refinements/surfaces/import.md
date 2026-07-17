# surfaces.import — External import (wrap-or-copy)

## TaskJuggler entry

`tasks/20-surfaces.tji:18-24` → `surfaces.import` ("External import
(wrap-or-copy)"), the third leaf under `task surfaces`. `depends !capabilities`.
Note line: "Import of caller memory as surfaces in the CPU backend (wrap; copy
for anything else), sync token API shaped for GPU fences later. DECIDED POLICY
(parking-lot triage 2026-07-16 …): foreign-tagged caller memory takes the COPY
path and converts at import time … zero-copy WRAP is reserved for
working-space-tagged memory, making the wrap/copy fork deterministic on the tag.
Convert-at-composite stays out of v0.1 entirely: no backend seam, no foreign tag
ever reaches the compositor. Doc 09."

## Effort estimate

2d.

## Inherited dependencies

**Settled:**

- `surfaces.capabilities` (the declared `!capabilities` predecessor, landed) —
  `BackendCaps` (`src/surface/arbc/surface/capabilities.hpp:53-59`) with
  `import_handles` (`ImportHandleTypes`, `capabilities.hpp:20-45`) and the
  `ImportHandle` bit vocabulary (`capabilities.hpp:9-14`, `CpuMemory` included).
  This task is the one that *flips* the `CpuMemory` bit on: `capabilities.hpp`
  and the honest-caps comment in `cpu_backend.cpp:132-143` both name
  `surfaces.import` as the task that lands the machinery the bit advertises.
- `surfaces.provided_surfaces` (landed; `tasks/refinements/surfaces/provided_surfaces.md`)
  — `SurfaceRef` (`src/surface/arbc/surface/surface_ref.hpp:29-55`) and
  `RenderResult.provided` (`src/contract/arbc/contract/content.hpp:120`) are the
  seam an imported surface reaches the compositor *through*: a content that owns
  external memory imports it here, then hands the result back as a provided
  `SurfaceRef`. `RenderResult.provided`'s contract (`content.hpp:113-119`) —
  "the surface must carry the composition working-space tag (v1; cross-tag
  convert-at-composite is gated on a multi-format backend)" — is exactly what
  forces this task to land the foreign tag in the working space *at import*.
- `color.kernels` (landed) — `Backend::convert`
  (`src/surface/arbc/surface/backend.hpp:99`, CPU impl `cpu_backend.cpp:215-261`)
  is the transcode the copy path calls: format → premultiplied linear working
  float → format, total over the closed format set, an exact byte copy on the
  diagonal. The copy path is `make_surface(target)` + one `convert`, not a
  hand-rolled decode loop.
- `kinds.imageseq_plugin` / `kinds.image` (landed) — the convert-at-decode
  **precedent** the decided policy cites: `imageseq_content.cpp:158-165` decodes
  `Rgba8Srgb` → `WorkingPixel` → `k_working_rgba32f` and hands back a
  working-tagged surface, so no foreign tag reaches the compositor from an image
  kind. This task generalizes that hand-rolled decode into the backend seam.

**Pending:** none. The formal predecessor (`capabilities`) and every de-facto
seam (`provided_surfaces`, `convert`) are landed. *(The `.tji` declares only
`depends !capabilities`; `provided_surfaces` is complete, so there is no
scheduling hazard. Flagged for the closer as a candidate `depends
!provided_surfaces` edge — this task's output is consumed only through
`SurfaceRef`.)*

## What this task is

Add the **backend import seam** and its **CPU reference implementation**: turn
caller-owned CPU memory into a backend `Surface`, either by **wrapping** it
zero-copy (when it is already in the composition working space) or by **copying
and converting** it at import time (when it is foreign-tagged). Doc 09:59-61,114-120
("import is wrap-or-copy of caller memory") owns this; the wrap/copy fork is the
2026-07-16 parking-lot **DECIDED POLICY** made deterministic on the tag.

Four concrete deliverables:

1. **A `CpuImport` descriptor + an `ImportSync` token, in `arbc::surface`.** New
   L2 header `src/surface/arbc/surface/import.hpp`. `CpuImport` bundles the
   import request — `std::span<std::byte> memory`, `int width/height`,
   `SurfaceFormat source_format` (the tag the caller's bytes carry),
   `SurfaceFormat target_format` (the composition working space to land in),
   `std::function<void()> release`, and an `ImportSync sync`. `ImportSync` is an
   empty, opaque value **shaped** to carry a GPU fence/semaphore/EGL sync later
   (doc 09:116-118); the CPU backend needs no wait (memory is immediately
   readable) and ignores it. Bundled into one value struct, mirroring
   `RenderRequest`/`BackendCaps`, so the seam signature does not accrete
   parameters.
2. **`Backend::import_cpu_memory(const CpuImport&)`.** One new pure-virtual on
   `Backend` (`src/surface/arbc/surface/backend.hpp`), returning
   `expected<std::unique_ptr<Surface>, SurfaceError>` — symmetric with
   `make_surface`, errors as values. CPU memory is the one handle type every
   backend (CPU today, GPU-via-upload later) can accept; the GL/Vulkan/DMA-BUF
   handles doc 09:114-116 lists are **not** seamed here (no backend implements
   them; a GPU backend adds its own handle-typed import when it lands).
3. **The CPU backend implementation** (`src/backend_cpu/cpu_backend.cpp`), plus
   flipping `capabilities().import_handles` to `ImportHandle::CpuMemory`.
4. **Test-double absorption.** The new `Backend` op ripples into the shipped
   doubles per doc 09:110-142: `StubBackend` returns `UnsupportedFormat` (honest
   — it has no import), `ForwardingBackend` delegates, `CountingBackend` tallies,
   and the `RecordingBackend`/`NullBackend` in-tree doubles gain the override.

**Not this task:** GL/Vulkan/DMA-BUF import (no backend supports them —
GPU-backend work); a real (non-inert) sync-primitive wait (CPU needs none; the
token is shaped, not wired); zero-copy *adoption* of an imported surface as a
cache value (that is the `provided_surfaces` parking-lot item, GPU-gated); and
wiring a concrete content kind to call import (no v0.1 image-editor content
produces foreign-tagged caller memory — this lands the seam and proves it under
test, per doc 09:118-120 "the API path is exercised from day one even though
zero-copy only pays off on GPU").

## Why it needs to be done

Doc 09 makes import a stated part of the surface contract and the entry point
for the external-handle half of content-provided surfaces (09:114-116). It is a
direct M9 dependency (`m9_release`), and `capabilities`/`cpu_backend`
already advertise its arrival in comments ("the import bits flip on when
surfaces.import lands wrap-or-copy", `cpu_backend.cpp:135-137`) — a promise the
release must make true. The DECIDED POLICY resolved the last open question (a
foreign-tagged import must convert at *decode/import*, never at composite),
which is what unblocks writing the seam without leaving convert-at-composite
half-specified.

## Inputs / context

- **Doc 09** §"CPU reference backend" (`09:59-61`, "import is wrap-or-copy of
  caller memory") and §"Content-provided surfaces" (`09:114-120`, the external
  handle + sync-primitive contract; "wrap (when given memory) or copy (when
  given anything else it can read back)").
- **The DECIDED POLICY** (`tasks/20-surfaces.tji:23`; parking-lot triage
  2026-07-16, resolving the 2026-07-07 "Cross-tag convert-at-composite" entry
  after four re-parks). Its load-bearing reasoning: for wrap-or-copy content,
  "wrap" and "convert-at-composite" are the *same* choice and "copy" and
  "convert-at-decode" are the *same* choice — wrapping host memory zero-copy is
  fundamentally incompatible with converting at decode (converting means writing
  new pixels, i.e. copying, which defeats the wrap). The policy picks: foreign
  tag → copy + convert at import; working-space tag → wrap.
- **`Backend::convert`** (`backend.hpp:79-99`, `cpu_backend.cpp:215-261`) — the
  transcode the copy path reuses. Doc 09:40-42 already names "the import …
  edge[] will reuse" this operation.
- **The three closed storable formats** (`surface_format.hpp:36-49`):
  `k_working_rgba32f`, `k_working_rgba16f` (premultiplied linear working
  floats), `k_fast_rgba8srgb` (straight-alpha 8-bit sRGB fast mode). Each pins a
  *distinct* pixel format, so a tag is storable iff it equals one of the three;
  `make_surface` (`cpu_backend.cpp:155-157`) enforces exactly this set.
- **The imageseq decode precedent** (`imageseq_content.cpp:158-165`).

## Constraints / requirements

1. **The wrap/copy fork is `source_format == target_format`.** Full-triple
   equality, not "is it some working space": wrapping `rgba16f` bytes as an
   `rgba32f` surface is impossible, so precision differences take the copy path
   too. Equal tags → **wrap** (return a non-owning `Surface` over `memory`);
   unequal → **copy** (`make_surface(target_format)` + `convert`).
2. **`source_format` must be storable.** Import can only read bytes it can
   interpret: `source_format` must be one of the three closed formats, else
   `UnsupportedFormat` (errors as values, never abort). This is doc 09's
   "anything else it can read back" boundary made precise for CPU memory.
3. **Convert at import; no foreign tag reaches the compositor.** The returned
   surface always carries `target_format` (wrap: because `source==target`; copy:
   because `convert` lands it there). This is what `RenderResult.provided`'s
   working-space requirement (`content.hpp:113-119`) needs and what keeps
   convert-at-composite out of v0.1 (doc 09:220-230).
4. **Release semantics.** `release` signals "the backend no longer needs your
   `memory`". Wrap: fired when the returned surface is **destroyed** (the caller
   may then reclaim/mutate its buffer). Copy: fired **before import returns**
   (the copy is done). On **error** (unstorable source, size mismatch,
   `make_surface` fault): `release` is **never** fired — the import did not
   happen, the caller still owns and reclaims its memory. A null `release` is
   legal (a caller with no teardown obligation), mirroring `SurfaceRef`.
5. **Byte-size consistency is validated defensively.** `memory.size()` must equal
   `width*height*bytes_per_pixel(source_format.pixel_format)`. A public import
   boundary receives external memory, so a mismatch is a debug-assert **and** a
   release-mode `UnsupportedFormat` return (never an OOB read), unlike the
   internal `convert`/`composite` dimension mismatches which cull.
6. **Capability honesty.** `CpuBackend::capabilities().import_handles` flips to
   `ImportHandle::CpuMemory` (and *only* that bit — no GL/Vulkan/DMA-BUF, no
   `sync_primitives`, which stay off until a GPU backend). The
   `capabilities-are-honest` claim/test that asserts the bit is *empty* is
   updated in lockstep.
7. **Zero-copy is real on the wrap path.** The wrapped surface's `cpu_bytes()`
   returns the caller's bytes (`.data()` identity), no allocation, no `convert`.
8. **Test doubles absorb the new op** per doc 09:110-142 (Constraint above,
   deliverable 4). The `forwarding-double-delegates-every-op` claim/test
   enumerates every operation "so growing the contract grows this test by
   construction" (`backend_doubles.t.cpp:104-107`) — it gains the import case.

## Acceptance criteria

- **Wrap (zero-copy):** importing `k_working_rgba32f` caller memory with
  `target = k_working_rgba32f` returns a surface whose `cpu_bytes().data()` is
  the caller's pointer, carrying the working tag, no bytes touched; destroying it
  fires `release` exactly once. Same for `k_working_rgba16f → rgba16f`.
- **Copy + convert at import:** importing `k_fast_rgba8srgb` caller memory with
  `target = k_working_rgba32f` returns a *new* surface (distinct pointer) tagged
  `k_working_rgba32f`, whose pixels equal the `convert` of the source
  (byte-identical to the `imageseq` decode of the same bytes); `release` fires
  before return; the caller's buffer is untouched.
- **Unstorable source / size mismatch:** `UnsupportedFormat` value, `release`
  never fired, no surface.
- **Capabilities:** `import_handles.test(CpuMemory)` is true; the other three
  handle bits and `sync_primitives` stay false.
- **Doubles:** `StubBackend` import → `UnsupportedFormat`; a `CountingBackend`
  over a `RecordingBackend` forwards one import call and the inner sees it; the
  `forwarding-double-delegates-every-op` test drives import and asserts arrival.
- Full gate green: byte-exact goldens, levelization (`import.hpp` is L2, depends
  only on `media`+`surface`), claims-have-live-tests, clang-format, TSan/ASan.

## Decisions

- **D1. Return `unique_ptr<Surface>`, not `SurfaceRef`.** Symmetric with
  `make_surface`; keeps import a pure backend allocation primitive. The content
  that imports owns the returned surface (as `imageseq` owns its
  `FrameSurface`) and constructs the `SurfaceRef` borrowing it for
  `RenderResult.provided`. `SurfaceRef` deliberately does **not** own its
  `Surface` (`surface_ref.hpp:24-28`), so having import mint one would strand the
  wrapper's ownership; `unique_ptr` puts ownership where it belongs.
- **D2. One `import_cpu_memory` seam, not a handle-typed variant.** CPU memory is
  the universal handle; the GL/Vulkan/DMA-BUF bits exist as *advertising*
  vocabulary (`ImportHandle`) but seaming them now — with no backend to
  implement them — is the speculative generality doc 09:81 ("advertise only what
  it currently implements") warns against. A GPU backend adds its own typed
  import.
- **D3. `target_format` is an explicit parameter, not a hardcoded working
  space.** The compositor's working space is per-composition
  (`07…#compositing-in-working-space`), and passing it makes *both* working
  formats wrappable and operationalizes the fork as `source == target`. The
  alternative (hardcode `k_working_rgba32f`, à la `imageseq`) would force a
  needless `rgba16f → rgba32f` copy in an rgba16f composition and re-bake the
  working-space assumption `color.working_space` is meant to lift.
- **D4. `ImportSync` ships empty now.** Doc 09:116-118 promises a sync primitive;
  the CPU backend needs none. Shipping the *type* (inert) rather than deferring
  it keeps the seam signature stable when a GPU backend wires a real wait —
  callers already pass one, GPU backends start honoring it. Same "shape the API
  now, implement when the backend exists" move as `make_surface`'s GPU-fence
  comment.
- **D5. Size mismatch returns a value, dimension asserts elsewhere cull.** Import
  is a *public boundary receiving external memory*; a wrong size is an OOB-read
  hazard, not an internal invariant. So it is defended (assert + return), unlike
  `convert`/`composite`/`downsample` which debug-assert and release-cull on
  caller-internal dimension bugs. No new `SurfaceError` variant:
  "cannot form a surface of that format from the given memory" reads as
  `UnsupportedFormat`; the enum's own comment
  (`surface_error.hpp:10-14`) discourages speculative variants.
- **D6. The wrapped surface is a file-local `Surface` in `cpu_backend.cpp`.** It
  is a non-owning view over caller bytes plus an optional destruction-time
  release callback; nothing outside the CPU backend names its concrete type
  (callers hold `Surface&`/`unique_ptr<Surface>`), so it stays out of the public
  header. The same class serves as the read-only `convert` source on the copy
  path (constructed with a null release, fired manually after `convert`).

## Open questions

- **GL/Vulkan/DMA-BUF import + a live sync-primitive wait.** Owned by the GPU
  backend that first needs them; the `ImportHandle` bits and the `ImportSync`
  shape are the seam they will fill. Not parked — it is doc-09 future work with a
  named owner (a GPU backend), tracked by the existing post-v1 GPU-backend scope.
- **Zero-copy adoption of an imported surface as a cache value.** Already a
  parking-lot item under `provided_surfaces` (GPU-gated); import does not change
  it — the copy path still copies.

## Status

Implemented and landed. `Backend::import_cpu_memory` + `CpuImport`/`ImportSync`
(`src/surface/arbc/surface/import.hpp`, `backend.hpp`); CPU backend wrap-or-copy
(`cpu_backend.cpp`) with the `ImportHandle::CpuMemory` capability flip; the four
shipped/in-tree doubles absorb the op; three registered claims
(`09-surfaces-and-backends#import-wraps-matching-tag-zero-copy`,
`#import-converts-foreign-tag-at-import`, `#import-faults-as-value`) with live
tests in `cpu_backend.t.cpp`, and the `capabilities-are-honest` /
`forwarding-double-delegates-every-op` claims/tests updated in lockstep. Doc 09
realization addendum added. `surfaces.import` marked `complete 100`.

Closer notes: candidate `depends !provided_surfaces` edge on the `.tji` leaf (the
output is consumed only through `SurfaceRef`). This closes the last M9 leaf
before `packaging.tag_01`.
