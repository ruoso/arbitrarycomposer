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

## Status

Design phase. No code yet. See the design documents:

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
