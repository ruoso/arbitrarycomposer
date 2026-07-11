# 17 — Internal Components

The library ships as **one artifact** (`libarbc`), but is built internally
as levelized CMake **object libraries** with explicit, CI-enforced
dependency edges (doc 16's levelization, made concrete). Components are
the unit of physical design: each owns its public headers, its sources,
its unit tests, and its allowed-dependency list.

## Shipped artifacts

| Artifact | Contents |
| --- | --- |
| `libarbc` (shared and static) | all components below, composed |
| `arbc-testing` (static) | the contract conformance suite (doc 16) — linked by plugin authors' test binaries, never by `libarbc` |
| `arbc-plugin-imageseq` (shared) | the image-sequence reference kind — *outside* `libarbc` (see "the codec line") |
| headers | `include/arbc/<component>/…`, one directory per public-facing component |
| CMake/pkg-config/CPS metadata | doc 10 |

## The component graph

```
L6  arbc               (umbrella: symbol surface, version, registry bootstrap)
     │
L5  runtime            Document · viewports/transports/monitors · interactive
     │                 & offline render drivers · dlopen plugin loading ·
     │                 housekeeping (reclamation, checkpoints)
     │
L4  compositor   audio-engine   serialize      kind-solid kind-tone
     │            │              │              kind-raster kind-fade
     │            │              │              kind-crossfade kind-nested
     ├────────────┴──────┬───────┴──────────────────┬─────┘
L3  contract         cache          backend-cpu
     │                │                 │
L2  model          surface ────────────┘
     │  ┌───────────┐ │
L1  pool            media
     └────────┬───────┘
L0          base
```

A component may depend only on strictly lower levels. No same-level
edges. The arrows above are the *complete* allowed set; the CI dependency
check (doc 16) validates the CMake target graph and the include graph
against this table:

| Component | Level | Contents | Design docs | Depends on |
| --- | --- | --- | --- | --- |
| `arbc::base` | 0 | `expected`-style errors, `ObjectId`, `Rect`/geometry, affine math + singular values, `Time`/rational rates/time maps, debug counters | 03, 04, 11, 16 | — |
| `arbc::pool` | 1 | inside-out slab arenas, `arbc::ref`, deferred reclamation, mmap/anonymous backing, checkpoint protocol, generation tags | 15 | base |
| `arbc::media` | 1 | pixel-format & color-space descriptors, premultiplication tags, channel layouts, typed pixel/sample span views, format-agnostic resampling filters over decoded working samples (audio + image) | 07, 12 | base |
| `arbc::surface` | 2 | `Surface` handles, the backend contract, external import + sync tokens, format conversion *interfaces* | 09 | base, media |
| `arbc::model` | 2 | object records, persistent `DocState`, transactions, journal/undo, damage, revisions, pins | 01, 14 | base, pool, media |
| `arbc::contract` | 3 | `Content` + `AudioFacet` + `Editable`, requests/results, `Stability`, `Registry`, `PullService` *interface*, damage sinks | 03, 11, 12, 13, 14 | base, pool, media, surface, model |
| `arbc::cache` | 3 | budgeted keyed cache (2D tiles, 1D blocks), priority classes, prefetch rings, eviction | 02, 11, 12 | base, surface |
| `arbc::backend-cpu` | 3 | format-templated kernels + variant dispatch, CPU surfaces, wrap-or-copy import | 07, 09 | base, media, surface |
| `arbc::compositor` | 4 | transform resolution, culling, request planning, scale ladder, damage routing over `inputs()`, aggregate revisions, cycle handling, `PullService` implementation | 02, 04, 05, 13 | contract, cache (+ below) |
| `arbc::audio-engine` | 4 | pull-based mix, lookahead scheduler, block pipeline, clock mastering, latency pre-roll | 12 | contract, cache (+ below) |
| `arbc::serialize` | 4 | JSON read/write, canonical form, unknown-kind placeholders, `LoadContext`, `$ref` resolution | 08 | contract, model (+ below); JSON dep |
| `arbc::kind-*` (six) | 4 | solid, tone, raster (CoW tile table, decoded-buffer input), fade, crossfade, nested | 03, 05, 11, 12, 13 | contract (+ below); nested uses only the `PullService` interface |
| `arbc::runtime` | 5 | `Document` (arenas + model + registry + loaders), viewport/transport/monitor objects, interactive frame loop, offline/export drivers, `dlopen` loading, housekeeping thread | 01, 02, 14, 15 | everything below |
| `arbc` | 6 | umbrella target; public symbol surface; built-in kind registration | — | runtime + all |

"(+ below)" means the transitive closure of the listed components' own
dependencies — listing is explicit in CMake, the table shows the news.

Notes on placements that were genuinely contested:

- **`contract` sits above `model`** because requests carry snapshot pins
  and `Editable` trades in journal-visible state handles. The model stays
  free of the `Content` vtable (records hold opaque content slots; binding
  happens in `runtime`), preserving "pure data plus change notification"
  (doc 02).
- **`model` depends on `media`** because a composition record stores its
  per-composition working space as a `SurfaceFormat` (doc 07 rule 2): media
  descriptors are level-1 vocabulary and composition records are precisely where
  configuration vocabulary lives, so the edge is a levelization-clean downward
  one and the model stays typed rather than smuggling the working space as opaque
  integers interpreted upstairs (`color.working_space`).
- **`cache` is engine-agnostic** — tiles and blocks are the same machinery
  with different key shapes (doc 12), so both engines share one component.
- **Format-templated kernels are not `media`.** Format/space *descriptors*
  are vocabulary (L1); the *format-templated* kernel bodies — the loops
  parameterised on `(PixelFormat, ColorTransfer)` that read and write
  surface storage — are backend implementation (L3, `backend-cpu`), per
  doc 07's types-at-the-boundary/kernels-inside split. The rule is about
  the format template, not about arithmetic: **format-agnostic DSP over
  already-decoded working samples** — a filter weight bank and its tap
  combiner, taking decoded `WorkingPixel`/float samples in and out, naming
  no `PixelFormat` and touching no surface — *is* `media` (L1). It has to
  be: the L4 kinds may reach `media` transitively through `contract` but
  may never reach `backend-cpu`, so a shared floor below both is the only
  place a resampling filter can serve `backend-cpu`'s compositing kernels
  and `kind-raster`'s mip pyramid without duplicating a byte-exact
  coefficient table across an un-crossable level boundary. `media`'s audio
  resampler (doc 12) already sits here; the image filter bank
  (`kinds.raster_resampling_quality`) joins it. The split is mechanical:
  if it needs `PixelTraits<F>` or a `Surface`, it is `backend-cpu`; if it
  is arithmetic on decoded working values, it is `media`.
- **The two render drivers live in `runtime`, not the engines.** The
  engines are libraries over pinned versions; deadlines, frame loops, and
  device clocks are runtime policy (doc 02's renderer/compositor split).
- **"Debug counters" is a convention, not a central registry.** The
  `arbc::base` "debug counters" entry names the *primitive style* — plain
  `std::uint64_t` fields with `noexcept` accessors, the wall-clock-free
  behavioral-counter surface doc 16:54-62 requires — not a shared mutable
  counter object every component writes into. Each component **owns its own
  counters** and exposes them where they naturally live: `cache` publishes
  `hits()/misses()/evictions()` on the keyed store; `runtime` composes a
  `HousekeepingStats` snapshot; `pool` counts on its arenas. Components that
  are **pure per-frame libraries** (the compositor) do not hold persistent
  counter state — they take a caller-owned counters struct by pointer, the
  same way they already take the `SurfacePool` and `RefinementQueue`, so the
  persistent value lives in `runtime` and the library stays stateless. There
  is deliberately no `base`-level global registry: a mutable singleton would
  reintroduce cross-frame state into stateless engines and make behavioral
  assertions order-dependent under parallel test execution.

## Why object libraries specifically

1. **Self-registration survives.** Built-in kinds register via static
   registrars; composing *static* libraries would let the linker drop
   unreferenced registrar objects, the classic silent failure. Object
   libraries link every object into `libarbc` unconditionally — the
   composition is exact, not link-order-dependent.
2. **Physical design without shipping fragmentation.** Users get one
   library, one `find_package(arbc)`; internally every component still has
   its own target, usage requirements, and unit-test binary, so
   levelization is enforced by the build rather than by convention.
3. **Dual-build of kinds falls out.** Each `kind-*` object library also
   links into a tiny per-kind shared library in CI, loaded via `dlopen`
   through the doc-03 entry point — proving the plugin path stays honest
   without shipping six .so files nobody wants.

CMake mechanics (recorded so bootstrap doesn't rediscover them):

- `POSITION_INDEPENDENT_CODE ON` globally (objects must be PIC for the
  shared `libarbc`); default `-fvisibility=hidden` with an export macro so
  only the deliberate public API (doc 16) is visible from the shared
  build; `arbc::<name>` alias targets everywhere; the dependency manifest
  lives as data (the CMake lists themselves) consumed by the CI
  levelization check.
- **Public vs private headers via `FILE_SET HEADERS` (CMake ≥ 3.24), not
  a separate include tree.** Each component keeps one directory; a header
  is public iff it is a member of the component's header file set
  (`target_sources(arbc_model PUBLIC FILE_SET HEADERS BASE_DIRS
  ${CMAKE_CURRENT_SOURCE_DIR} FILES arbc/model/document.hpp …)`). Public
  headers live under the `arbc/<component>/` path inside the component
  directory so `#include <arbc/model/document.hpp>` resolves through the
  file set's usage requirements; private headers sit beside the sources
  without the `arbc/` prefix and are unreachable by that spelling from
  other components — the include-hygiene CI check (doc 16) enforces that
  cross-component includes resolve only through file sets.
- **Install aggregation is the one manual step**: object libraries are
  not installed targets, so the `arbc_add_component()` helper records
  each component's header set on a global property and the umbrella
  `arbc` target declares one installed `FILE_SET` with per-component
  `BASE_DIRS`, preserving the `arbc/<component>/…` layout under the
  install include dir. `install(TARGETS arbc … FILE_SET HEADERS)` +
  `install(EXPORT …)` then produce the package as doc 10 specifies.
- **`VERIFY_INTERFACE_HEADER_SETS ON`** in CI: every public header is
  compiled standalone, so a public header that secretly depends on its
  includer's context is a build failure — self-containedness as a gate,
  free with the file-set approach.

## The codec line

Two decisions collide at the image-sequence kind: it is v1 scope (doc 11),
but codecs must never ride into an embedder's link line (doc 10). The
resolution: **`libarbc`'s built-in kinds are codec-free** — `kind-raster`
accepts decoded pixel buffers — and `arbc-plugin-imageseq` ships as a
separate plugin artifact carrying its own decode dependency (stb-class),
built and tested in the same repo, loaded like any third-party plugin.
This also makes imageseq the permanent end-to-end test of the real plugin
path, which the in-lib kinds no longer exercise at runtime.

The same line holds for **device backends**: an OS audio API (PortAudio /
CoreAudio / ALSA / a miniaudio-class single-header backend) is the audio
analog of a codec — a system dependency that must never ride into an
embedder's link line. So the interactive audio device sink splits the same
way the codec kinds do: the **`DeviceSink` interface and the `DeviceMonitor`
that masters the transport are `runtime` (L5) and dependency-free of any OS
audio API** (device clocks are runtime policy, above), while the concrete
reference backend that carries the OS audio dependency ships as a **separate
out-of-lib plugin artifact** (`arbc-plugin-<device>` under `plugins/`,
mirroring `arbc-plugin-imageseq`: a hand-rolled `MODULE`, its backend
dependency private and never in `libarbc` / `arbc-testing`). `libarbc` stays
codec- *and* device-free; the audio milestone's `audio.device_monitor` lands
this split.

## Repo layout (supersedes the doc 10 sketch)

One tree per component — no parallel `include/` hierarchy; publicness is
file-set membership (above):

```
src/<component>/
  arbc/<component>/…          public headers (FILE_SET HEADERS members)
  *.hpp, *.cpp                private headers and sources
  t/                          component unit tests
plugins/imageseq/             out-of-lib reference plugin (own deps)
testing/                      arbc-testing conformance suite sources
tests/                        cross-component: integration, claims register,
                              golden, stress, crash-recovery, fuzz corpus
docs/design/
```
