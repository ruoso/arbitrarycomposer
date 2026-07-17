# Arbitrary Composer

A C++ library for composing 2D scenes out of layers with *arbitrary
implementation kinds*. Each layer may natively be a raster image, vector
graphics, a live 3D scene, a procedural generator, or a recursive embedding of
another composition. Each layer carries an arbitrary 2D transform (translate,
scale, rotate — full affine), and layers render themselves at whatever
resolution the current view requires, enabling resolution-independent
composition and, in principle, infinite zoom. Compositions have a timeline:
layers occupy time spans with rate-mapped local time, so video-like content
composes through the same contract as stills — and content may carry an
audio facet, mixed through the same placement, making a composition a full
A/V scene.

The same scene model drives two rendering modes:

- **Interactive**: a live viewport that pans and zooms at frame rate, with
  progressive refinement as layers catch up.
- **Offline**: exact frames at arbitrary resolution, for export and video.

A document is a small, diffable `.arbc` JSON graph plus a sibling asset
directory. Imported images are *referenced*, never copied in — so a project
does not re-store the photographs it was built from. Pixels that were
*painted*, and therefore exist nowhere else, are stored as content-addressed
tiles: identical tiles collapse to one blob, and a save writes only the tiles
that changed. The two cases are different layer kinds, which is what makes
non-destructive editing structural — a referenced image cannot be painted on,
so you retouch by stacking an editable raster over it.

## Status

Released: **v0.1.0**. The library is built, tested, and installable: a
levelized component tree under `src/`, shared and static builds, a per-push CI
matrix across GCC/Clang × Debug/Release/ASan/TSan/RTSan, a claims register
that pins the design docs' promises to tests, and a relocatable CMake package
with shipped, CI-run embedding examples. Pre-1.0 the surface still moves freely;
[CHANGELOG.md](CHANGELOG.md) records what each tag ships, and its `[0.1.0]`
section describes this release's surface.

## Quickstart: embedding the library

Install from source (any prefix works):

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$HOME/arbc"
cmake --build build
cmake --install build
```

Consuming the installed package is a config-mode `find_package`. This is
[examples/host-offline/CMakeLists.txt](examples/host-offline/CMakeLists.txt),
verbatim:

<!-- readme-quickstart: consume -->
```cmake
find_package(arbc CONFIG REQUIRED)

add_executable(host_offline main.cpp)
target_link_libraries(host_offline PRIVATE arbc::arbc)
target_compile_features(host_offline PRIVATE cxx_std_20)
```

That is the entire dependency surface. The dependency policy is part of the
public promise (design doc 10, pinned by test): embedding the core never
transitively imposes codecs, GPU SDKs, or a GUI toolkit.

The embedding itself — bootstrap the kind registry, build a document, render
one exact frame — is the body of
[examples/host-offline/main.cpp](examples/host-offline/main.cpp), verbatim:

<!-- readme-quickstart: embed -->
```cpp
  // 1. Kind bootstrap: one call presents every built-in kind through the same
  //    Registry surface loaded plugins register into (doc 03 § Registry).
  arbc::Registry registry;
  arbc::register_builtin_kinds(registry);

  // 2. The document: a root composition and two solid layers, bottom-to-top in
  //    attach order (doc 05). The backdrop goes through the registry's factory
  //    -- the path a config-driven host takes; the overlay is constructed
  //    directly -- the programmatic host's path. Solid colors are
  //    PREMULTIPLIED working-space values (doc 07).
  arbc::Document document;
  const arbc::ObjectId comp = document.add_composition(32.0, 32.0);

  const arbc::ContentFactory* solid = registry.factory("org.arbc.solid");
  if (solid == nullptr) {
    std::puts("host-offline: org.arbc.solid is not registered");
    return 1;
  }
  // Opaque red, unbounded extent: the factory grammar is "r,g,b,a".
  arbc::expected<std::unique_ptr<arbc::Content>, std::string> backdrop = (*solid)("1,0,0,1");
  if (!backdrop.has_value()) {
    std::printf("host-offline: backdrop construction failed: %s\n", backdrop.error().c_str());
    return 1;
  }
  document.attach_layer(comp, document.add_layer(document.add_content(std::move(*backdrop)),
                                                 arbc::Affine::identity()));

  // Half-opacity green over the top-left quadrant: unit-square bounds scaled
  // to 16x16 composition units by the layer transform.
  const arbc::ObjectId overlay = document.add_content(std::make_shared<arbc::SolidContent>(
      arbc::Rgba{0.0F, 0.5F, 0.0F, 0.5F}, arbc::Rect{0.0, 0.0, 1.0, 1.0}));
  document.attach_layer(comp, document.add_layer(overlay, arbc::Affine::scaling(16.0, 16.0)));

  // 3. One exact frame (doc 02:241-253). The target arrives in the
  //    composition's working space; a backend that cannot store that format
  //    reports a SurfaceError value, never an abort.
  arbc::CpuBackend backend;
  const arbc::Viewport viewport{32, 32, arbc::Affine::identity()};
  const arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError> frame =
      arbc::render_offline(document, viewport, backend);
  if (!frame.has_value()) {
    std::puts("host-offline: render_offline could not produce the target surface");
    return 1;
  }
  const arbc::Surface& surface = **frame;
```

The rest of that program (working-space floats → straight-alpha sRGB8 → PNG)
is in the full source. Both snippets above are byte-identical to anchored
regions of `examples/host-offline/` — a standalone foreign project CI
configures, builds, and **runs** against a staged install on every lane —
and a sync test enforces the identity, so this quickstart cannot drift from
a program that compiles and runs.

More of the shipped surface:

- [examples/host-interactive/](examples/host-interactive/) — the interactive
  mode: a `HostViewport` + `InteractiveRenderer` pan/zoom frame loop, driven
  headlessly by a scripted gesture tape.
- [examples/plugin-template/](examples/plugin-template/) — a third-party
  layer-kind plugin is `find_package(arbc CONFIG REQUIRED)` plus one line:
  `arbc_add_plugin(my-plugin SOURCES my_plugin.cpp)`. The helper ships
  inside the CMake package.
- `find_package(arbc CONFIG REQUIRED COMPONENTS testing)` adds
  `arbc::testing` — the contract conformance suite a plugin author runs over
  their own `Content` factory to get the contract's behavioral promises
  checked for them.

## Contributing

To build, test, and run the pre-push gate, see
[CONTRIBUTING.md](CONTRIBUTING.md):

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
scripts/gate
```

The design documents are normative — they are the specification the code is
checked against, not a description of it:

| Doc | Contents |
| --- | --- |
| [00-overview](docs/design/00-overview.md) | Vision, goals, non-goals, guiding decisions |
| [01-core-concepts](docs/design/01-core-concepts.md) | Composition, layer, transform, viewport, resolution model |
| [02-architecture](docs/design/02-architecture.md) | Components, render pipeline, caching, invalidation, threading |
| [03-layer-plugin-interface](docs/design/03-layer-plugin-interface.md) | The layer contract and plugin strategy |
| [04-transforms-and-infinite-zoom](docs/design/04-transforms-and-infinite-zoom.md) | Transform model, numeric precision, deep zoom |
| [05-recursive-composition](docs/design/05-recursive-composition.md) | Compositions as layers, cycles, budgets |
| [06-prior-art](docs/design/06-prior-art.md) | Survey of existing libraries and what they lack |
| [07-color-and-pixel-formats](docs/design/07-color-and-pixel-formats.md) | Working spaces, surface tags, templated kernels vs erased boundary |
| [08-serialization](docs/design/08-serialization.md) | The `.arbc` JSON format, unknown-kind round-trip, external references |
| [09-surfaces-and-backends](docs/design/09-surfaces-and-backends.md) | Surface abstraction, backend contract, texture adoption |
| [10-tooling-and-packaging](docs/design/10-tooling-and-packaging.md) | C++20, dependency policy, build, repo layout, testing |
| [11-time-and-video](docs/design/11-time-and-video.md) | Timeline, layer time spans, transport, timed content, video in v1 |
| [12-audio](docs/design/12-audio.md) | Audio facet, mixing, working format, monitors, clocking, lookahead engine |
| [13-effects-as-operators](docs/design/13-effects-as-operators.md) | Effects as content operators, PullService, pass-through, fade/crossfade |
| [14-data-model-and-editing](docs/design/14-data-model-and-editing.md) | Versioned document, transactions, undo journal, Editable facet |
| [15-memory-model](docs/design/15-memory-model.md) | Memory populations, inside-out slab arenas, version reclamation, thread rules |
| [16-sdlc-and-quality](docs/design/16-sdlc-and-quality.md) | Test taxonomy, claims register, CI, coverage gate, formatting/linting, maintainability |
| [17-internal-components](docs/design/17-internal-components.md) | Levelized object libraries, shipped artifacts, the codec line, repo layout |
