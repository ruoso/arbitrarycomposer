# Changelog

All notable changes to this project are documented here, in
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) form. Versioning is
[semantic](https://semver.org/spec/v2.0.0.html) from the first tag; pre-1.0 the
surface moves freely, and changelog honesty is what makes that safe
(design doc 16, § Physical design and maintainability).

**The discipline** (kept by hand — there is no CI check, deliberately):

- One entry per landed change that a *consumer* could observe. If a commit changes
  the shipped surface — public headers, the installed package, behavior, a
  dependency — it lands its changelog line in the same commit.
- This file tracks the **shipped surface**; the git log tracks commits. It is not a
  transcription of the log, and a docs-only, test-only, or internal refactoring
  commit belongs in the log alone.
- New entries go under `[Unreleased]`, in the `Added` / `Changed` / `Fixed` /
  `Removed` groups. A release moves that section under its version heading.

## [Unreleased]

_Nothing yet._

## [0.1.0] - 2026-07-17

The surface the first tag (0.1.0) names. There is no predecessor version, so
there is nothing for these to have changed *from*: this section describes what
0.1.0 ships, not a diff against a predecessor.

### Added

- **`libarbc`** — a 2D scene composer with pluggable layer kinds, shipped as a
  single static library composed of levelized internal components (design doc 17).
  Its public headers install under `<prefix>/include/arbc/<component>/`.
- **The layer/plugin contract** (`arbc/contract/`) — `Content` and its optional
  facets (operator graph, audio, editable state), rendering as a pure function of
  (snapshot state, region, scale, time), settling inline or asynchronously through
  a `RenderCompletion`. Errors are values; no exceptions cross the public boundary.
- **`arbc-testing`** — the contract conformance suite, shipped as public API behind
  `find_package(arbc CONFIG REQUIRED COMPONENTS testing)`. A plugin author runs it
  over their own `Content` factory and gets the contract's behavioral promises
  checked for them.
- **Reference layer kinds** — `org.arbc.solid`, `org.arbc.tone`, `org.arbc.raster`,
  `org.arbc.nested`, `org.arbc.fade`, `org.arbc.crossfade`, built into the library
  and dual-built as `dlopen` plugins in CI; the codec-carrying `org.arbc.imageseq`
  ships as a separate plugin artifact.
- **The plugin seam** — a single `extern "C" arbc_plugin_register(Registry&)` entry
  point. It carries no ABI number and negotiates nothing: v1 accepts same-toolchain
  coupling (doc 03, Stage 1), and a versioned C ABI arrives at 1.0.
- **Registry-carried codecs and binders** — a `Registry` entry optionally supplies,
  atomically with its factory, a JSON-free `KindCodec` (text params ↔ content
  state) and a `KindBinder` (operator-graph input binding), so a loaded plugin's
  kind round-trips through document save/load and participates in the operator
  graph entirely from its own module — no JSON type crosses the plugin surface.
- **`arbc::register_builtin_kinds()`** — one call at the umbrella presents the six
  in-lib kinds (factory + metadata, skip-on-duplicate, idempotent) through the same
  `Registry` surface loaded plugins register into, so a host can enumerate what the
  library can instantiate — an "insert layer" menu straight off `Registry::ids()`.
- **Rendering** — a CPU reference backend with byte-exact deterministic output,
  compositing source-over on premultiplied alpha in a per-composition working color
  space, with a power-of-two scale-rung tile cache and higher-order (Lanczos-3 /
  Catmull-Rom) resampling. Content may hand back its own surface (a decoder's
  output, an engine framebuffer) instead of filling the target, and caller CPU
  memory imports wrap-or-copy through the backend — foreign pixel formats converted
  at import so no foreign tag reaches the compositor.
- **Audio** — a block cache, a composition mixer, and a lookahead scheduler that
  keeps the RT device callback free of rendering work.
- **The versioned data model** — persistent, structurally shared document state with
  transactions, undo/redo, damage propagation, and an arena-backed workspace file
  with crash-consistent checkpointing.
- **Serialization** — a JSON document format with zstd-compressed raster tile blobs.
- **`arbc/version.hpp` and the version symbols** — `ARBC_VERSION_MAJOR` / `_MINOR` /
  `_PATCH`, a comparable `ARBC_VERSION` with `ARBC_VERSION_ENCODE(major, minor,
  patch)` to build comparands, `ARBC_VERSION_STRING`, and a `constexpr`
  `arbc::compiled_version()` reporting the headers you compiled against — paired with
  `arbc::linked_version()` / `arbc::linked_version_string()`, out-of-line symbols
  reporting the library you actually linked or loaded. The two exist so header/library
  skew is *observable*; the library reports it and never enforces it (doc 10,
  § Versioning and the version API).
- **A CMake package** — `find_package(arbc CONFIG)`, with `SameMajorVersion`
  compatibility and an optional `testing` component.
- **`arbc_add_plugin()`** — a CMake helper shipped inside the package config:
  against an installed arbc, a complete third-party plugin build is
  `find_package(arbc CONFIG REQUIRED)` plus one `arbc_add_plugin()` call, producing
  a loadable module. `examples/plugin-template/` is the copyable starting point.
- **Shipped embedding examples** — `examples/host-offline/` (one exact frame to
  PNG, the minimal embedding) and `examples/host-interactive/` (a `HostViewport` +
  `InteractiveRenderer` pan/zoom frame loop, headless via a scripted gesture tape),
  standalone foreign projects that CI configures, builds, and runs against a staged
  install on every lane, validating their PNG output byte-exactly.
- **Dependencies: nlohmann/json** (header-only) and **zstd** — the core's only two,
  both consumed find-first with a pinned `FetchContent` fallback. Neither appears in
  an installed header, so no embedder compiles against either.
