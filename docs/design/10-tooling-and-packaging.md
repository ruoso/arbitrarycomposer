# 10 — Tooling, Dependencies, Packaging

Decisions recorded here so the code phase starts unambiguous. Lightest doc
by design; revisit freely.

## Language and toolchain

- **C++20** baseline. Motivations: concepts for constraining the plugin and
  kernel templates (doc 07), `std::span` for surface access, designated
  initializers for the request/result structs. Nothing in the design needs
  C++23; embedders on current-but-conservative toolchains stay in reach.
- Supported compilers: current GCC, Clang, MSVC. CI matrix from the first
  commit; warnings-as-errors with a curated flag set.
- No exceptions across public API and plugin boundaries (doc 03); internal
  use is unconstrained. Public errors are values (`expected`-style result —
  C++20 means we carry a small `arbc::expected` until/unless the baseline
  moves).

## Dependency policy: minimal, vetted

The core takes a dependency when hand-rolling would be silly, and each one
is individually justified:

| Dependency | For | Status |
| --- | --- | --- |
| JSON library | doc 08 serialization | **chosen: `nlohmann/json`** (`serialize.json_dep`). Meets doc 08's four requirements: its default `std::map`-backed object gives sorted keys (canonical output, doc 08 §Principle 5) for free; it round-trips arbitrary/unknown content verbatim (unknown-kind losslessness, §Principle 2); it offers a non-throwing `parse` overload the L4 API wraps as `arbc::expected` so no exception crosses the plugin boundary; and it consumes cleanly (below). `simdjson`+writer / `yyjson` (perf, C) stay parked for a future binary/perf profile, gated on evidence — "it's the format that's contractual, not the parser." Consumed find-first with a version-pinned `FetchContent` fallback (mirrors the Catch2 wiring, `CMakeLists.txt`), pinned in CI — never an in-tree vendored copy; this is the concrete reading of doc 08 §Dependency note's "unproblematic vendoring." |
| Compressor | doc 08 Principle 8 (raster tile blobs) | **chosen: `zstd`** (`serialize.zstd_dep`). The second core dependency, and taken deliberately: the content-addressed tile store needs a per-blob compressor, and a *painting* project — the case with no photographs to reference away — is where compression actually earns its keep (painted tiles ~7x, masks ~45x). `zstd` over LZMA on speed: an interactive editor saves incrementally, on a gesture cadence, and cannot pay LZMA's compression time for its extra ratio; over zlib on ratio, at comparable speed. Applied with a byte-shuffle (doc 08 Principle 8) — the shuffle is ours, not the library's. Note the honest bound: compression is the *weakest* of the size levers (2.9x, below content-addressed dedup's 4.3x), so it is worth exactly one small, well-vetted dependency and no more — and specifically it is **not** a reason to reach for an image codec in core (see the row below). Consumed find-first with a version-pinned `FetchContent` fallback, pinned in CI, mirroring the JSON wiring. |
| Test framework | tests, and the conformance artifact | **chosen: Catch2** (`quality.testing_artifact`). Not in `libarbc` — no core header includes it and no embedder ever sees it. It *is* the public assertion runtime of the separately-shipped `arbc-testing` (doc 17:14), because the conformance suite asserts from inside the *caller's* own `TEST_CASE` (doc 16 § The contract conformance suite). That surfaces in the package as an **optional CMake component**: `find_package(arbc)` yields `arbc::arbc` and requires nothing of the consumer; `find_package(arbc COMPONENTS testing)` additionally yields `arbc::testing` and `find_dependency(Catch2 3)`. So the promise below holds unchanged — embedding the core imposes no test framework — while a plugin author who *wants* the conformance suite opts into the one dependency it needs. |
| Image codecs | **not core** — and no in-lib kind needs one. `org.arbc.raster` is deliberately *codec-free*: it accepts already-decoded buffers, and the pixels it persists are painted document state, not an encoded file (doc 08 Principle 8). Decoding belongs to the two kinds that reference external image files — `org.arbc.image` and `org.arbc.imageseq` — which therefore ship *outside* `libarbc` as plugin artifacts carrying their own decode dependency (stb-class, vendored once and shared; libpng/libjpeg-turbo are equally admissible in a third-party plugin). This is doc 17's "codec line", and it keeps the core embeddable without codec baggage. |
| GPU APIs | **not core** — each GPU backend owns its API dependency. |

Vendoring vs system packages: consume through standard find mechanisms
(`find_package`), never vendored copies in-tree; lockstep versions pinned in
CI. The dependency *policy* is part of the public promise: embedding the
core must never transitively impose codecs, GPU SDKs, or a GUI toolkit.

## Build and packaging

- **CMake ≥ 3.24** with presets (`CMakePresets.json`) as the canonical
  build; target-based, no global state; internal object-library components
  per doc 17, with public headers declared as `FILE_SET HEADERS`
  (single-tree layout, `VERIFY_INTERFACE_HEADER_SETS` in CI).
- Install exports: CMake package config files, pkg-config files, and **CPS**
  (Common Package Specification) metadata as it becomes consumable by
  tooling — this library is a good early adopter candidate and the metadata
  cost is trivial.
- Plugins build as shared libraries with the single `extern "C"` register
  entry point (doc 03); a `arbc_add_plugin()` CMake helper ships with the
  package so third-party plugin builds are one line.
- Plugin discovery at runtime: explicit host registration first (embedders
  usually want control), plus an opt-in directory scan
  (`ARBC_PLUGIN_PATH` + platform-conventional locations) for
  application-style hosts.

## Versioning and the version API

Policy lives in doc 16 (§ Physical design and maintainability, 16:143-148):
semver from the first tag, pre-1.0 moving freely with changelog honesty, ABI
checking and a deprecation policy arriving at 1.0. This section records the
*mechanism* that policy is carried by (`packaging.version_api`).

- **One source of truth: `project(VERSION …)`.** The version is declared once,
  in the top-level `CMakeLists.txt`, and everything else is derived from it —
  the generated public header, the compiled-in symbols, and the
  `arbcConfigVersion.cmake` the package config already writes
  (`SameMajorVersion`). No second literal anywhere in the tree; a version bump
  is a one-line edit.
- **`arbc/version.hpp` — the compile-time half.** A public header owned by the
  L6 umbrella (doc 17:33 names "version" as an umbrella responsibility), not by
  any component, and therefore installed at `<prefix>/include/arbc/version.hpp`
  rather than under a component subdirectory. Generated from a template by
  `configure_file`. It carries `ARBC_VERSION_MAJOR/MINOR/PATCH`, a comparable
  encoded `ARBC_VERSION` with an `ARBC_VERSION_ENCODE(major, minor, patch)`
  macro to build comparands, `ARBC_VERSION_STRING`, and a `constexpr`
  `arbc::compiled_version()` returning the triple as a plain struct. The header
  includes nothing: it is the one header in the tree with no dependencies at
  all, so it stays compilable standalone under `VERIFY_INTERFACE_HEADER_SETS`
  and warning-clean for consumers who do not inherit our `-Wpedantic -Werror`.
- **The linked symbols — the run-time half.** `arbc::linked_version()` and
  `arbc::linked_version_string()` are compiled *into* `libarbc`, from the same
  generated header. They report the version of the library actually linked or
  loaded, which the macros cannot: macros describe the headers a consumer
  compiled against. The pair exists precisely so header/library skew is
  *observable* — a host that ships a prebuilt `libarbc`, or that `dlopen`s
  plugins built elsewhere, can compare `compiled_version()` against
  `linked_version()` and report the mismatch.
- **Skew is reported, never enforced.** The library does not abort, assert, or
  refuse to load on a version mismatch. It exposes both numbers and lets the
  host decide — consistent with errors-as-values and no exceptions across the
  public boundary (§ Language and toolchain, above). A library that kills its
  host's process over a version comparison is a worse failure than the skew.
- **No plugin ABI version in v1.** The `extern "C" arbc_plugin_register` seam
  carries no ABI number and negotiates nothing. Versioned, semver-gated vtables
  are Stage 2's C ABI (doc 03 § Stage 2), which doc 16:147 activates at 1.0;
  minting an ABI number while the C++ interface still churns would advertise a
  compatibility promise the v1 seam does not make (doc 03 accepts same-toolchain
  coupling for Stage 1). The library version above is the *library's* version,
  not a plugin contract.
- **`SOVERSION` rides the shared build.** The static library needs none; the
  shared one derives `VERSION`/`SOVERSION` from the same `project(VERSION …)`
  when `packaging.shared_library_build` lands its `BUILD_SHARED_LIBS` lane.
- **`CHANGELOG.md`** sits at the repository root in Keep-a-Changelog form
  (`[Unreleased]` on top, `Added`/`Changed`/`Fixed`/`Removed` groups), hand-kept
  per doc 16:143-145. It records the evolution of the *shipped surface* for
  consumers, so it begins at the surface the first tag names — the git log
  remains the per-commit record, and the changelog is not a transcription of it.

### Cutting a release

The release ritual (`packaging.release_01`; for v0.1.0 executed by
`packaging.tag_01`). It is deliberately manual: one release is cut at a
time, pre-1.0, every step but the last is a one-liner, and automation would
have exactly one consumer and nothing gated to publish.

1. **Readiness**: every leaf of the release milestone is complete —
   `scripts/unblocked.py` reports it. A tag never lands while a milestone
   leaf is open; the tag-cutting task depends on every other leaf, so the
   sequencing is a scheduler fact, not a hope.
2. **Roll the changelog**: retitle `[Unreleased]` to `## [X.Y.Z] - <date>`
   and open a fresh, empty `[Unreleased]` section above it.
3. **Flip the README**: the one release-status sentence stops reading "the
   first tag will be…" and names the released version.
4. **Tag**: `git tag -a vX.Y.Z` on the commit that rolled the changelog;
   the tag message is the rolled changelog section, verbatim — that section
   *is* the release notes, and writing them twice would be the real waste
   (`packaging.version_api` D6).
5. **Publish**: pushing the tag, and any announcement, is a human step —
   never automated from CI.
6. **Next cycle**: the version bump is the one line in `project(VERSION …)`
   — nothing else in the tree carries the number.

## Repository layout

Superseded by doc 17 (internal components): the library ships as a single
`libarbc` composed of levelized CMake object libraries, one directory per
component under `src/`, with reference kinds linked in-lib (and dual-built
as `dlopen` plugins in CI) except the codec-carrying imageseq plugin,
which stays a separate artifact.

## Testing strategy (sketch)

Superseded in depth by doc 16 (SDLC and quality: full test taxonomy, CI
structure, claims register, formatting/linting); the sketch below remains
as the original outline.

- **Contract tests**: a reusable suite any `Content` implementation runs
  against (bounds honesty, scale honesty, damage correctness, async
  completion, cancellation) — shipped so plugin authors get it too.
- **Golden-image tests** for kernels and compositing, with per-format
  tolerances (f16 vs 8-bit), on the CPU backend.
- **Numeric tests** for doc 04: deep-nesting scenarios asserting sub-pixel
  stability across rebase thresholds.
- **Determinism tests** for doc 08: load→save byte-canonical round-trips,
  unknown-kind preservation.

Per project convention: build and test before every commit.
