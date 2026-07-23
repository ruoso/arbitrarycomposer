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

### Added

- **`Model::on_writer_thread()` / `Document::on_writer_thread()`** — the document's
  single writer identity, now bound in *every* build (one atomic `thread::id`, set by
  the first transaction, never rebound) and queryable from any thread. True before any
  write, when the caller would become the writer. Doc 15's single-writer-identity
  contract was previously only a debug assert one level down in `SlotStore`; a host —
  and the library itself — can now *ask* instead of finding out by corruption.
- **`Document::external_loads_ready()`** — how many fetched external arrivals are queued
  awaiting a writer-thread install. Lock-free, allocation-free, any-thread: the poll a
  render loop uses to learn that a settle is owed. Beside the existing
  `pending_external_loads()` (fetches still in flight).
- **`HostViewport::StepOutcome::external_loads_ready`** — the same count, reported per
  frame by a step that declined to install (see *Fixed*). Zero when the step settled.
- **`HostViewport::Config::external_loads_ready`** — the readiness probe beside
  `Config::settle_external_loads`, derived from a bound `Document` and overridable, for
  a host driving a bespoke settle hook off its writer thread.
- **`Document::set_external_load_settler()` / `Document::external_loads_auto_settled()`**
  — the writer-thread settler a `Document`-bound `HostViewport` installs (and releases)
  automatically, run immediately ahead of the document's next edit whenever an arrival
  is waiting, plus its behavioral counter.

### Fixed

- **`HostViewport::step()` no longer publishes structural writes off the writer thread**
  (issue #13). Frame planning is render-thread-confined by design, but step 0 ran the
  external-arrival settle — a model transaction, an `add_content` and a commit —
  unconditionally, so a host that edits on its UI thread and renders on another got a
  *second* writer identity, which doc 15 forbids and no host-side mutex can repair. The
  step now asks `Model::on_writer_thread()`: on the writer thread it settles inline
  exactly as before (single-threaded hosts, and every driver in the tree, are
  unaffected); off it, it publishes nothing and reports `external_loads_ready`. The
  install then happens on the writer thread — driven by the host, or automatically ahead
  of the host's next edit, so ignoring the report costs latency, never correctness.
  `settle_external_loads()` is documented writer-thread-only and debug-asserts it.
- **The `HostViewport` damage handoff is synchronized.** `DamageAccumulator::flush` runs
  inside a commit on the writer thread while `step()` drains it on the render thread —
  an unguarded `std::vector<Damage>` for any host rendering off-thread. It now carries a
  mutex for that handoff alone (a bounded append or a swap; no render, plan, or pull
  inside it), so an off-thread host needs no coarse per-frame lock of its own.

## [0.2.0] - 2026-07-22

Additive since 0.1.0: the per-kind state-slab walk hook lands the recovery half
of the editable-state seam, plus a rendering-correctness fix and a lock-free
render read path. The plugin surface stays same-toolchain and unversioned
(the C ABI still arrives at 1.0); every 0.1.0 registration compiles unchanged.

### Added

- **`Registry::KindStateWalker`** — a per-kind state-slab reachability walker,
  registered atomically with the factory and looked up lock-free via
  `Registry::state_walker(id)`. `Registry::add()` gains a trailing defaulted
  `std::optional<KindStateWalker>` parameter, so every existing registration
  compiles unchanged. Mirrors `KindBinder`'s static-thunk idiom: the store is
  type-erased across the registry boundary and the owning kind's TU casts it back.
- **`Model::recovered_content_state()`** (and `Model::RecoveredContentState`) — on
  a workspace fast-reopen the model's recovery walk collects each reachable
  non-inert content `StateHandle` it cannot descend itself (owning `ObjectId`,
  kind id, handle), for the runtime to replay. The model holds only the opaque
  slot and, by levelization, cannot name the kind — so it collects rather than
  descends.
- **`arbc/runtime/recovered_state_replay.hpp`** — `replay_recovered_content_state()`
  routes each collected handle to its owning kind's registered walker, so a
  reopened document rebuilds the slab refcounts a persisted handle keeps reachable
  (the recovery twin of the writer-owned `StateRefSink` retain/release seam).
  Unresolvable kind tokens and walkerless kinds are skipped and counted, never
  fatal; it returns `{dispatched, skipped}` as a behavioral witness.

### Changed

- **Render reads are lock-free** — a `Document`'s content bindings now publish
  copy-on-write through an atomic `shared_ptr<const ContentBindings>`, so a render
  snapshot never contends with a concurrent edit that rebinds contents.

### Fixed

- **`render_offline` binds operators** — nested compositions rendered through the
  offline one-shot now have their operator graphs bound, instead of rendering
  unbound and producing wrong output.

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
