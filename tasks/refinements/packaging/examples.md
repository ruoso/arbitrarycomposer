# packaging.examples — Host examples

## TaskJuggler entry

`tasks/75-packaging.tji:25-30`:

```
task examples "Host examples" {
  effort 2d
  allocate team
  depends !install
  note "A minimal embedding example (offline render to PNG via a tiny writer) and an interactive pan/zoom example; examples compile and run in CI. Doc 16."
}
```

Downstream: `packaging.release_01` (`tasks/75-packaging.tji:31-36`) depends on
this task; `m9_release` (`tasks/99-milestones.tji:70-73`) depends on
`release_01`, so this task is already milestone-wired.

## Effort estimate

Booked 2d; realistic 2d.

- Tiny PNG writer (`examples/common/png_writer.hpp`, stored-deflate blocks,
  CRC-32 + Adler-32) — ~0.5d.
- Offline embedding example (`examples/host-offline/`) — ~0.25d.
- Interactive pan/zoom example (`examples/host-interactive/`, gesture tape) —
  ~0.5d.
- CI wiring: extend `tests/consumer/run_staged_install.cmake` to
  configure/build/run both examples; new consumer validation TU
  `tests/consumer/host_example_artifacts.cpp` — ~0.5d.
- Per-example READMEs, doc 17 repo-layout delta, claims-register entry, WBS
  gate ritual — ~0.25d.

## Inherited dependencies

**Settled (formal `depends`):**

- `packaging.install` (`complete 100`) — the whole delivery vehicle: staged
  `cmake --install` into a fresh prefix, `find_package(arbc CONFIG)` yielding
  `arbc::arbc`, the `install.consumer` CTest at
  `tests/CMakeLists.txt:1393-1427` driving
  `tests/consumer/run_staged_install.cmake`, and the
  core-imposes-only-zstd metadata claim
  (`packaging.core-metadata-imposes-only-zstd`). Refinement:
  `tasks/refinements/packaging/install.md`.

**Settled (informal):**

- `packaging.plugin_helper` — created `examples/` and established every
  convention this task extends: an example is a standalone foreign project
  (`find_package(arbc CONFIG REQUIRED)`, never `add_subdirectory`'d —
  `examples/plugin-template/CMakeLists.txt:5-6,24`), compiled in CI inside
  `run_staged_install.cmake` against the staged prefix
  (`tests/consumer/run_staged_install.cmake:68-86`), with the enforcing test
  under `tests/consumer/` because `scripts/check_claims.py:32` does not scan
  `examples/`. Its D8 explicitly handed this task ownership of any doc 17
  repo-layout note for `examples/`. Refinement:
  `tasks/refinements/packaging/plugin_helper.md`.
- `packaging.version_api` — `<arbc/version.hpp>` at the include root:
  `ARBC_VERSION_STRING`, `arbc::compiled_version()`,
  `arbc::linked_version_string()`. The offline example prints the
  compiled/linked pair (their agreement is already pinned by
  `tests/consumer/core_only.cpp`). Refinement:
  `tasks/refinements/packaging/version_api.md`.
- `packaging.shared_library_build` / `_msvc` / `_zstd_shared_link` — the
  `gcc-shared` and `msvc-shared` lanes run `install.consumer`, so the
  examples build and run against shared `libarbc` for free; running an
  example executable on Windows shared needs the staged `bin/` on `PATH`
  (msvc refinement D4 precedent). The embedder link line is clean under both
  linkages (`zstd_shared_link` D1/D2).
- `runtime.registry_bootstrap` — `arbc::register_builtin_kinds(Registry&)`
  at the L6 umbrella (`src/api/arbc/builtin_kinds.hpp:39`), the host's
  one-call kind bootstrap both examples demonstrate.
- `runtime.host_viewport_document_binding`, `runtime.interactive_pull_wiring`
  and the M9 interactive-runtime train — `HostViewport`
  (`src/runtime/arbc/runtime/host_viewport.hpp:68`) and
  `InteractiveRenderer` (`src/runtime/arbc/runtime/interactive.hpp:209`),
  the surface the interactive example drives.

**Pending, and deliberately not a dependency:**

- `packaging.release_01` — downstream consumer of this task (README
  quickstart reflects the shipped examples).
- `kinds.image` / plugin kinds — the examples use built-in kinds only
  (`register_builtin_kinds`), so no plugin artifact is involved; plugin
  loading is already demonstrated by `examples/plugin-template/` plus
  `tests/consumer/plugin_scan.cpp`.

**Downstream:** `packaging.release_01` (M9); the README quickstart it writes
should lift its embedding snippet from `examples/host-offline/`.

## What this task is

Populate `examples/` with two host-embedding examples, each a standalone
foreign CMake project consuming the installed `arbc` package exactly as a
third-party embedder would:

1. **`examples/host-offline/`** — the minimal embedding: build a small
   `Document` of built-in kinds, render one exact frame via
   `render_offline`, convert working-space pixels to straight-alpha sRGB8,
   and write a PNG through a tiny dependency-free writer. This is doc
   01:124-125 made executable: "the offline renderer is just a viewport with
   no deadline."
2. **`examples/host-interactive/`** — the interactive shape: a
   `HostViewport` bound to a `Document`, an `InteractiveRenderer` frame loop
   rendering into one persistent caller-owned surface, and pan/zoom driven
   as camera-transform edits (doc 01:108, doc 04:82-84). The shipped driver
   is a deterministic scripted gesture tape (so it runs headlessly in CI);
   the README marks the single swap-point where a real toolkit's event loop
   plugs in.

Both are compiled **and executed** in CI inside the existing
`install.consumer` staged-install test, with a consumer-side test validating
their PNG artifacts byte-exactly.

## Why it needs to be done

Doc 16:88-90 places examples in the test taxonomy ("every code sample in
docs and the plugin template compiles and runs in CI … A README example that
doesn't build is a bug") and doc 16:138-142 puts "examples still building"
in the definition of done. `packaging.release_01` cannot write an honest
README quickstart without a compiled, CI-run embedding example to lift from.
Today `examples/` demonstrates only the plugin-author path
(`plugin-template`); the embedder path — the project's primary audience per
doc 00:99 — has no reference artifact. The examples are also the living
proof of doc 10:34-35's promise: embedding the core imposes no codecs, no
GPU SDKs, no GUI toolkit — each example's entire dependency surface is
`find_package(arbc CONFIG)`.

## Inputs / context

**Design docs (normative, doc 16):**

- Doc 16:88-90 — examples tier of the taxonomy (compile **and run** in CI);
  16:138-142 — definition of done; 16:14-21 — claims-register mechanics;
  16:48-53 — CPU backend is deterministic, goldens are byte-exact.
- Doc 10:19-35 — dependency policy; 10:29 — image codecs are not core;
  10:34-35 — "embedding the core must never transitively impose codecs, GPU
  SDKs, or a GUI toolkit."
- Doc 00:76-79 — "Not a GUI framework. The library produces composed pixels
  into a surface the host provides; windowing, input handling, and widgets
  belong to the host application."
- Doc 01:91-125 — viewport semantics; 01:108 — "Pan/zoom/rotate of the view
  are edits to the camera transform"; 01:112-121 — binding a viewport to a
  document is the host's single wiring step; 01:124-125 — offline renderer
  is a viewport with no deadline.
- Doc 02:83-87 — the target surface is the caller's and persists across
  frames; 02:241-253 — the offline frame is exact; 02:350-354 — pool
  ownership defaults; 02:406-412 — failures reported per-layer, not thrown.
- Doc 04:82-84 — host-visible camera APIs speak the (anchor node, matrix)
  pair.
- Doc 09:14-17, 09:59-60 — typed CPU access (`span<Format>()`) everywhere on
  the reference backend.
- Doc 17:20-28 — the consumer surface (`arbc::arbc` imposes nothing);
  17:71 — `runtime` ships `Document`, viewport objects, interactive frame
  loop, offline drivers; 17:298-310 — system deps never ride an embedder's
  link line; 17:312-329 — repo layout listing (currently lacks `examples/`;
  see D8).

**Source seams:**

- `examples/plugin-template/CMakeLists.txt:5-6,24,26` — the foreign-project
  shape this task replicates twice.
- `tests/CMakeLists.txt:1393-1427` — `install.consumer` registration
  (RUN_SERIAL, sanitizer/generator forwarding, `-DARBC_TEMPLATE_SRC` at
  :1406).
- `tests/consumer/run_staged_install.cmake` — staging (:30-34), template
  build (:68-86), twin consumer configure/build/ctest passes (:88-107). The
  extension point for building and running the two new examples.
- `tests/consumer/CMakeLists.txt` — consumer test TUs; the new
  `host_example_artifacts.cpp` joins `core_only.cpp`, `plugin_scan.cpp`,
  `plugin_template_load.cpp` et al.
- `src/runtime/arbc/runtime/document.hpp:64,106,132,149,167,312-313` —
  `Document`: `add_content`, `add_layer`, `attach_layer`, `add_composition`,
  `registry()`/`set_registry`.
- `src/api/arbc/builtin_kinds.hpp:39` — `register_builtin_kinds(Registry&)`.
- `src/runtime/arbc/runtime/offline.hpp:20-21` —
  `render_offline(const Document&, const Viewport&, Backend&)` →
  `expected<std::unique_ptr<Surface>, SurfaceError>`.
- `src/compositor/arbc/compositor/compositor.hpp:16-31` — `Viewport{width,
  height, camera, anchor}`.
- `src/runtime/arbc/runtime/host_viewport.hpp:68,176,188,193,259-305` —
  `HostViewport`: document-bound ctor, `camera()`, `set_camera(Affine)`,
  anchored-camera reanchor/rebase stack.
- `src/runtime/arbc/runtime/interactive.hpp:209,271` —
  `InteractiveRenderer::render_frame`.
- `src/surface/arbc/surface/surface.hpp:14,25,32-33,41-56` — `Surface`:
  `format()`, `cpu_bytes()`, typed `span<PixelFormat>()`.
- `src/backend_cpu/arbc/backend_cpu/cpu_backend.hpp:13,35` — `CpuSurface`,
  `CpuBackend`.
- `src/media/arbc/media/pixel_format.hpp:18-22`,
  `src/media/arbc/media/pixel_traits.hpp:48-53,191-213` —
  `linear_to_srgb8` and `PixelTraits<PixelFormat::Rgba8Srgb>::encode`
  (unpremultiply → sRGB-encode → straight-alpha 8-bit): the exact
  working-space→PNG conversion the offline example performs per pixel.
- `tests/walking_skeleton.t.cpp:18-24,62-100` — the canonical offline
  render-to-pixels sequence the offline example mirrors.
- `plugins/imdec/third_party/imdec.h`,
  `plugins/miniaudio/third_party/maudio.h` — the repo's precedent for small
  hand-written single-purpose headers over vendored third-party code.
- `.github/workflows/ci.yml:151-176` — coverage lane: `gcovr --filter 'src/'`
  (:159), so `examples/` never enters `coverage.xml` and the diff-coverage
  gate skips it (same treatment `plugin-template` and the consumer TUs
  already get).

## Constraints / requirements

1. **Foreign-project rule** (plugin_helper Constraint 6): each example has
   its own `project()`, sees arbc only through
   `find_package(arbc CONFIG REQUIRED)` against a prefix supplied via
   `CMAKE_PREFIX_PATH`, is never `add_subdirectory`'d into the main build,
   compiles against installed public headers only, and is repo collateral —
   not installed into the prefix.
2. **Zero new dependencies.** No codec library, no windowing toolkit, no
   system package on any CI lane. The examples are the demonstration that
   doc 10:34-35 holds: their full dependency surface is `arbc::arbc`. The
   PNG writer is hand-written example-local code (D2); the interactive
   driver is headless-scripted (D3).
3. **Examples run, not just compile** (task note; doc 16:88-90). Both
   executables execute in CI on every lane via `install.consumer` and exit
   non-zero on any failure. On Windows shared lanes the staged `bin/` must
   be on `PATH` when the executables run (`cmake -E env` in the driver
   script; msvc refinement D4 precedent).
4. **Deterministic artifacts.** The CPU backend is deterministic (doc
   16:48-53) and the PNG writer uses stored (uncompressed) deflate blocks,
   so the emitted PNG bytes are byte-exact reproducible across lanes; the
   consumer test pins pixel content exactly, no tolerances.
5. **CI-pinned scenes are hand-computable.** Both examples' CI scenes use
   axis-aligned solid-color geometry with known colors/opacities so the
   consumer test derives expected pixel bytes from first principles (through
   the same public `PixelTraits` encode), rather than freezing an opaque
   byte table.
6. **Claims live under `tests/`** — `scripts/check_claims.py:32` scans only
   `src/`, `tests/`, `testing/`; the `enforces:` tag goes in
   `tests/consumer/host_example_artifacts.cpp`, never in `examples/`.
7. **Error-handling style.** Examples are teaching artifacts: they consume
   `arbc::expected` results and per-layer failure reporting the way the API
   intends (doc 02:406-412, doc 10:14-17) — no exceptions, no ignored
   returns.
8. **Doc 17 repo-layout delta** (plugin_helper D8 handed this task the
   note): add an `examples/` line to doc 17's repo-layout listing
   (17:312-329) describing the directory's contents; same-commit rule
   applies at close.
9. **Levelization untouched.** Examples sit outside the component graph as
   package consumers (doc 17:20-28); `scripts/check_levels.py` output is
   unchanged.
10. **No `.tji` edits by the implementer** — the closer owns `complete 100`
    and the note back-link.

## Acceptance criteria

- `examples/host-offline/` and `examples/host-interactive/` each configure,
  build, and **run** against the staged prefix inside the `install.consumer`
  CTest (`tests/consumer/run_staged_install.cmake` extended with a build+run
  pass per example), on every CI lane including `gcc-shared` and
  `msvc-shared`. Non-zero example exit fails the test.
- The offline example writes `out.png`; the interactive example runs its
  scripted pan/zoom tape and writes a final-frame PNG. Both artifact paths
  are handed into the consumer configure as `-D` cache entries (plugin_helper
  D6 pattern).
- New claim in `tests/claims/registry.tsv`:
  `16-sdlc-and-quality#shipped-examples-compile-and-run-in-ci` — "the
  shipped host examples compile and run in CI against the staged install" —
  enforced by `tests/consumer/host_example_artifacts.cpp` (carrying the
  `// enforces:` tag), which validates for each artifact: PNG signature,
  IHDR width/height/bit-depth/color-type, CRC-32 of every chunk, and —
  after decoding the stored-deflate scanlines (trivial: no inflate needed) —
  **byte-exact** pixel equality against expectations computed in-test from
  the known scene through `PixelTraits<PixelFormat::Rgba8Srgb>::encode`.
- Byte-exactness doubles as the doc 16 golden for this task's deterministic
  rendering surface; no tolerance anywhere.
- Deliberate reading of doc 16's taxonomy: no conformance-suite run (no new
  kind or operator), no behavioral-counter assertion (no performance-shaped
  promise), no TSan/stress scope (the interactive example runs the shipped
  single-writer frame loop; concurrency is exercised where it lives, in the
  runtime's own suites).
- Each example directory carries a `README.md`: build/run instructions
  against an installed prefix, and (interactive) the marked swap-point for a
  real windowing toolkit.
- Doc 17 repo-layout delta line for `examples/` lands in the same commit
  (Constraint 8).
- Coverage: `examples/` is outside `gcovr --filter 'src/'`
  (`.github/workflows/ci.yml:159`) and the consumer TU builds in the
  uninstrumented staged project — same diff-coverage treatment
  `plugin-template` and the existing consumer TUs already receive; no gate
  change needed.
- WBS gate ritual: `scripts/gate` green; `scripts/check_levels.py` output
  unchanged; after the closer's `complete 100` + note back-link,
  `tj3 project.tjp 2>&1 | grep -iE "error|warning"` is silent.
- **No deferred WBS leaves.**

## Decisions

**D1 — Two standalone foreign projects: `examples/host-offline/` and
`examples/host-interactive/`.**
*Rationale:* the task note names two distinct examples with distinct
audiences (batch/export embedder vs. interactive-app embedder); separate
projects keep each `CMakeLists.txt` a copy-paste-able embedding recipe, the
core value of an example. The naming parallels the task title ("Host
examples") and distinguishes them from the plugin-author artifact
(`plugin-template`).
*Rejected:* one example project with two executables — couples the two
recipes and muddies what a minimal embedder needs; a single "kitchen-sink"
example — doc 16's tier exists precisely so the *minimal* path stays
compiling.

**D2 — Hand-rolled PNG writer with stored (uncompressed) deflate blocks, in
`examples/common/png_writer.hpp`, shared by relative path.**
*Rationale:* PNG permits deflate "stored" blocks, so a complete, valid,
universally-viewable writer needs only chunk framing plus CRC-32 and
Adler-32 — roughly 150 lines, zero dependencies, and byte-deterministic
output (no compressor version variance), which is what lets the consumer
test pin bytes exactly (Constraint 4). This follows the repo's established
pattern of small hand-written single-purpose headers over vendored
third-party code (`imdec.h`, `maudio.h`). A shared `examples/common/` header
keeps the two examples from duplicating it; the relative include does not
compromise the foreign-project rule, which is about how arbc is consumed.
*Rejected:* vendoring `stb_image_write.h` — real third-party code in-tree
breaks the stand-in precedent, and its zlib-style compression makes output
bytes version-dependent; a libpng/zlib dependency — contradicts the very
promise the examples exist to demonstrate (doc 10:34-35) and adds system
packages to ten CI lanes; Netpbm output via the imdec precedent — the task
note says PNG, and PNG is what an embedder's stakeholders can open.

**D3 — The interactive example ships a headless scripted gesture tape, not a
windowing toolkit.**
*Rationale:* doc 00:76-79 is categorical — windowing and input belong to the
host; no design doc names a toolkit. The example's teaching payload is the
arbc side of the loop: viewport-to-document binding (doc 01:112-121), camera
edits as pan/zoom (doc 01:108) through `HostViewport::set_camera` and the
reanchor stack (doc 04:82-84), `InteractiveRenderer::render_frame` into one
persistent caller-owned surface (doc 02:83-87). A deterministic
`GestureScript` (a fixed sequence of pan deltas and zoom-about-point steps)
drives exactly that loop headlessly, which is the only way the example can
*run* on all ten CI lanes — including MSVC and headless Linux — with zero
new dependencies. The final frame is written as PNG so CI validates the
loop's output, not just its exit code. The README marks the single
swap-point ("replace the tape with your toolkit's event loop").
*Rejected:* real SDL2/GLFW behind `find_package` — a new system dependency
on every lane (including Windows), dummy-videodriver flakiness in headless
CI, and it plants the "GUI framework" optics doc 00:76-79 disclaims;
compile-only for the interactive example — violates the task note and doc
16:88-90's "runs in CI"; a maudio-style stand-in *window* header mirroring a
real toolkit's API — unlike `maudio.h` (which stands in for a shipped
plugin's real production dependency), there is no real dependency here to
stand in for, so the mirror would be dead weight; a plain gesture tape is
simpler and more honest.

**D4 — CI wiring rides `install.consumer`: `run_staged_install.cmake` builds
and executes both examples; a consumer TU validates the artifacts.**
*Rationale:* this is the exact vehicle `plugin-template` already uses
(plugin_helper D6), it runs on every lane (including both shared lanes) with
sanitizer/generator forwarding for free, and it tests the examples against a
*real staged install* — the surface an embedder actually consumes. Running
the executables from the driver script (via `cmake -E env` to prepend the
staged `bin/` on Windows-shared) and handing artifact paths into the
consumer configure keeps the enforcing test under `tests/consumer/` where
`check_claims.py` can see it (Constraint 6).
*Rejected:* a dedicated workflow step — duplicates staging, runs on fewer
lanes, and is invisible to `scripts/gate`; `ExternalProject_Add` in the main
build — breaks the never-`add_subdirectory`'d foreign-project purity that
makes the examples honest.

**D5 — Pixel conversion uses the public
`PixelTraits<PixelFormat::Rgba8Srgb>::encode` from
`arbc/media/pixel_traits.hpp`; rendering stays in the default working
format.**
*Rationale:* the offline render produces `Rgba32fLinearPremul` working-space
pixels (doc 02:241-253 exact path); PNG wants straight-alpha sRGB8.
`PixelTraits<Rgba8Srgb>::encode` (`pixel_traits.hpp:206-213`) is the
shipped, exhaustively round-trip-tested unpremultiply + linear→sRGB encode —
and using it teaches the working-space contract (premultiplied linear
inside, straight gamma at the edge) instead of hiding it. It also lets the
consumer test compute expected PNG bytes through the identical helper
(Constraint 5).
*Rejected:* hand-rolling the conversion in the example — duplicates vetted
code and risks teaching a subtly wrong encode (the traits comment itself
warns premultiplying 8-bit gamma samples is "doubly wrong"); rendering
directly into an `Rgba8Srgb` fast-mode surface — the offline path is the
exact path, and the conversion step is half the example's pedagogical value.

**D6 — One new claim; the interactive example mints none of its own.**
*Rationale:* doc 16:88-90 is the normative sentence this task lands, and one
claim (`16-sdlc-and-quality#shipped-examples-compile-and-run-in-ci`) with
one enforcing TU covers both examples' compile+run+output-validity surface.
The behaviors the interactive example *exercises* — target-surface
persistence, camera-edit semantics, exact offline output — are the runtime
and compositor streams' claims, already registered and enforced where the
code lives; an example re-claiming them would blur ownership.
*Rejected:* per-example claims — two registry rows saying "it runs in CI"
twice; claiming doc 01:124-125 or doc 02:83-87 from here — wrong stream.

**D7 — The offline example prints the compiled/linked version pair; nothing
asserts on it.**
*Rationale:* one line of output demonstrating `<arbc/version.hpp>`'s
purpose (`ARBC_VERSION_STRING` vs `arbc::linked_version_string()`) at
near-zero cost; agreement is already pinned by
`tests/consumer/core_only.cpp`, so asserting here would duplicate an
existing enforcement.
*Rejected:* omitting it — the version API is part of the embedding surface
release_01 documents; asserting it — redundant with `core_only.cpp`.

**D8 — Add the `examples/` line to doc 17's repo-layout listing now.**
*Rationale:* plugin_helper D8 deferred the layout note to this task ("owns
populating that directory and can carry the layout note if one is ever
warranted"). With three entries (`plugin-template/`, `host-offline/`,
`host-interactive/`) plus `common/`, `examples/` is now a stable, CI-run
repo surface; one descriptive line in doc 17:312-329 is warranted. This is a
descriptive-layout delta, not a behavioral one; no doc 00 decision-record
bullet.
*Rejected:* skipping again — a third packaging task leaving the listing
stale turns "if ever warranted" into "never"; a larger examples-policy
section — the conventions live in the refinements and the examples' own
READMEs, and doc 17's listing is deliberately terse.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-16.

- `examples/common/png_writer.hpp` — hand-rolled, dependency-free stored-deflate PNG writer (~150 lines, CRC-32 + Adler-32, byte-deterministic); shared by both examples via relative include.
- `examples/host-offline/` — standalone foreign CMake project: renders one exact frame via `render_offline`, converts working-space pixels through `PixelTraits<Rgba8Srgb>::encode`, writes `out.png`; prints compiled/linked version pair.
- `examples/host-interactive/` — standalone foreign CMake project: `HostViewport` + `InteractiveRenderer` frame loop, pan/zoom driven by a headless `GestureScript` (scripted gesture tape), writes final-frame PNG.
- `tests/consumer/host_example_artifacts.cpp` — new consumer TU enforcing claim `16-sdlc-and-quality#shipped-examples-compile-and-run-in-ci`; validates PNG signature, IHDR, per-chunk CRC-32, stored-deflate decode, and byte-exact pixel equality via `PixelTraits<Rgba8Srgb>::encode` derived in-test.
- `tests/consumer/run_staged_install.cmake` — extended to configure/build/run both examples and hand artifact paths into the consumer configure as `-D` cache entries.
- `tests/consumer/CMakeLists.txt`, `tests/CMakeLists.txt` — wired new TU and `-D` args.
- `tests/claims/registry.tsv` — new claim `16-sdlc-and-quality#shipped-examples-compile-and-run-in-ci`.
- `docs/design/17-internal-components.md` — added `examples/` line to the repo-layout listing (D8).
- Tech-debt surfaced and registered as WBS leaves: `runtime.camera_change_damage`, `runtime.placement_damage_maps_to_device`, `compositor.bounded_content_tile_clip` (workarounds documented in `examples/host-interactive/`).
