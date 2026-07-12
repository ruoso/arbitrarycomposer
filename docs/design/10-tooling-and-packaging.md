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
| Test framework | tests only, not shipped | Catch2 or GoogleTest; lean Catch2. |
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
