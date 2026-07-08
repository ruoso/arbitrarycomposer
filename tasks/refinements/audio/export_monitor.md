# audio.export_monitor — Export monitor (sample-exact offline mix over a time range, single-revision pinned, host-owned muxing)

## TaskJuggler entry

`tasks/45-audio.tji:61-66` → `audio.export_monitor` ("Export monitor"), a
leaf under `task audio`. Its own edge is `depends !mix_engine`
(`45-audio.tji:64`) — notably **not** `!lookahead` or `!device_monitor`: the
offline path needs the pure mix core, not the realtime scheduler or the
device clock. Note line:

> "Sample-exact mix over a time range, driven by the offline frame loop under
> snapshots; muxing stays host territory. Doc 12."

It is a dependency of the milestone `m6_audio`
(`tasks/99-milestones.tji:49-51`, which lists `audio.export_monitor` among its
`depends`). On completion the closer adds `complete 100` to this leaf and
appends the `Refinement:` back-link to its note; `m6_audio` also gains the
deferred follow-up leaf named under Acceptance criteria.

## Effort estimate

**2d** (matching the `.tji`). The heavy lifting already shipped in
`audio.mix_engine`: `mix_composition` (`src/audio_engine/arbc/audio_engine/mix.hpp:59-61`)
is a **pure function over a pinned `DocRoot`** that walks a composition's
layers, additively mixes each audible contributor pulled through
`PullService::pull_audio`, and already accepts an `Exactness::Exact` request.
The genuinely new work is thin and concentrated — the audio twin of the
video-side `SequenceRenderer` (`src/runtime/arbc/runtime/offline_sequence.hpp`,
`runtime.offline_sequences`, DONE 2026-07-07): a small stateful L5 driver that
(1) **pins one document revision for the whole export** and mixes every block
against it, (2) **tiles a caller-supplied half-open time range** into
working-rate block windows and drives `mix_composition` in `Exact` mode per
window, (3) hands each sample-exact mixed block to a **host-owned sink** (no
files, no muxing, no codec), and (4) holds the `PullServiceImpl` + `BlockCache`
the mix core pulls through. The deliverable is one header/impl pair, one
component unit+golden test, one cross-component byte-exact integration golden,
one focused concurrency (TSan) test, two new claims plus re-enforcement tags on
three existing mix-engine claims, a one-sentence doc-12 delta clarifying the
export output rate, and CMake wiring (**no new `DEPENDS` edge** — `runtime`
already reaches `audio_engine` and `compositor`). The premium over trivially
looping `mix_composition` is the single-revision pin held across the whole
export and its concurrent-writer test.

## Inherited dependencies

**Settled:**

- `audio.mix_engine` (DONE, `tasks/refinements/audio/mix_engine.md`,
  `45-audio.tji:12-18`) — the direct `!mix_engine` edge and the seam this task
  drives. Shipped the free function
  `AudioResult mix_composition(const DocRoot& doc, ObjectId composition, const MixResolver& resolve, PullService& pull, const AudioRequest& request, MixPolicy policy = MixPolicy::Flat)`
  (`src/audio_engine/arbc/audio_engine/mix.hpp:59-61`), a **pure function over
  the pinned `DocRoot`**: two calls with equal `(pin, composition, window,
  rate, layout)` settle to **bit-identical** samples (`mix.hpp:43-58`). It
  walks layers bottom-to-top, pulls each audible in-span layer through
  `PullService::pull_audio` (never `render_audio` inline), band-limit-
  reconstructs below-rate children via `resample_audio`, remixes to the request
  layout, scales by `gain`, sums, and folds `achieved_rate = min` /
  `exact = conjunction` over contributors. Its `mix_engine.md` Status block
  explicitly pre-assigns **"the offline sample-exact drive loop →
  `audio.export_monitor`"** (mix_engine.md:358-359) — this is that task.
  Supporting seams from the same task: `using MixResolver = std::function<Content*(ObjectId)>`
  (`mix.hpp:33`), `enum class MixPolicy { Flat }` (`mix.hpp:41`), and the
  concrete pull machinery `PullServiceImpl` + `BlockCache` in `compositor`
  (`src/compositor/arbc/compositor/pull_service.hpp:93`) with the
  `note_audio_dispatch` behavioral counter (`src/compositor/arbc/compositor/counters.hpp`).
- `runtime.offline_sequences` (DONE 2026-07-07,
  `tasks/refinements/runtime/offline_sequences.md`) — the **structural template**
  and the video half of the same export. Shipped `arbc::SequenceRenderer`
  (`src/runtime/arbc/runtime/offline_sequence.hpp`): pins one `DocStatePtr` at
  construction so "frame N never observes frame N+1's edits", steps a
  caller-supplied series of instants against the pinned revision, enforces the
  exact/no-degrade discipline, and hands each frame to a
  `FrameSink = std::function<void(Time, expected<std::unique_ptr<Surface>, SurfaceError>)>`
  (`offline_sequence.hpp:69`); the host owns encoding/muxing. Its refinement
  drew the boundary explicitly (offline_sequences.md:154-162): "The audio export
  monitor. Sample-exact mix over a time range is `audio.export_monitor` … which
  per its note 'is driven by the offline frame loop under snapshots.' This task
  builds that frame loop and its single-revision pinning; the audio monitor
  hooks into the same per-frame-time + pinned-snapshot structure later." This
  task is that hook. Note its `frame_times_over(TimeRange, Rational)` helper
  (`offline_sequence.hpp:58`) is the fixed-rate-stepping precedent for this
  task's block-window helper.
- `arbc::runtime` L5 platform (DONE) — the snapshot/pin primitive this task's
  consistency guarantee rests on: `Document::pin()`
  (`src/runtime/arbc/runtime/document.hpp:34`), `DocRoot` / `DocRoot::revision()`
  / `using DocStatePtr = std::shared_ptr<const DocRoot>` (`src/model/arbc/model/model.hpp:37,45,101`),
  and `Model::current()` publishing a new immutable version per commit
  (`model.hpp:163`). The `AudioRequest.snapshot` `model::StateHandle` rides each
  request index-only and lock-free, the same pin a `RenderRequest` carries
  (`src/contract/arbc/contract/content.hpp:234-249`).

**Pending:** none — every predecessor is landed.

## What this task is

Deliver **`arbc::ExportMonitor`** (L5, `arbc::runtime`, doc 17:60), a stateful
offline/export driver that renders a *sample-exact mix of a composition over a
half-open time range against one frozen document revision* — the audio
counterpart to `SequenceRenderer` and the offline sibling of `DeviceMonitor`.
In a new header/impl `src/runtime/arbc/runtime/export_monitor.hpp`
(+ `export_monitor.cpp`):

1. **Construction pins the export.** The constructor takes a `Document&`, the
   root composition `ObjectId`, the working `AudioFormat` (rate + layout the
   mix is produced at, defaulting to the composition's working format
   `k_working_audio`, `src/media/arbc/media/audio_format.hpp:17-28`), and an
   optional `MixPolicy` (`Flat`, the only policy shipped). It immediately pins
   one snapshot (`DocStatePtr d_pinned = document.pin()`); every block this
   instance mixes reads `*d_pinned` — a single, frozen `DocRoot::revision()` —
   so the export is internally consistent even while the host keeps editing on
   the writer thread (doc 02:77-80, doc 12:189-190 "snapshot semantics per doc
   02"). The instance owns the pull substrate: a `PullServiceImpl` and its
   `BlockCache` (`src/compositor/arbc/compositor/pull_service.hpp:93`), and a
   `MixResolver` over the pinned doc's bindings (the audio-engine's own resolver
   seam, `mix.hpp:33`).
2. **`render_block_at(TimeRange window, AudioBlock& target) -> AudioResult`** —
   the core method. It zero-initializes the caller-owned `target` block (the mix
   engine sums into silence, `mix.hpp:44`), builds an `AudioRequest{ window,
   sample_rate = working_rate, layout = working_layout, target, exactness =
   Exactness::Exact, snapshot = <pinned handle> }`, and calls
   `mix_composition(*d_pinned, d_composition, d_resolve, d_pull, request, d_policy)`.
   Because the request is `Exactness::Exact` (the offline-export mode, contract
   `content.hpp:239-240,285-287`) and offline carries **no deadline**, the mix is
   faithful: for a composition whose contributors honor the working rate, the
   returned `AudioResult` reports `exact == true` and
   `achieved_rate == working_rate`. It returns the aggregate `AudioResult`.
3. **`render_range(TimeRange range, std::uint32_t block_frames, const BlockSink& sink)`**
   — a thin convenience loop that tiles `range` into contiguous half-open
   working-rate block windows of `block_frames` samples each (a partial trailing
   block at `range.end` where the range is not a whole multiple), calls
   `render_block_at` per window against the pinned revision, and hands each
   result to `sink`. `using BlockSink = std::function<void(TimeRange, const AudioBlock&, AudioResult)>`
   — the audio twin of `FrameSink`; the **host** keeps, resamples, encodes, or
   muxes the blocks (muxing is host territory, doc 12:190-191). A helper
   `block_windows_over(TimeRange range, std::uint32_t rate, std::uint32_t block_frames)`
   → `std::vector<TimeRange>` computes the window series in exact rational
   arithmetic (the audio analog of `frame_times_over`,
   `offline_sequence.hpp:58`), so a caller may compute windows itself and feed
   `render_block_at` raw. An empty range or a non-positive `block_frames` yields
   an empty series (faults-as-values, never an abort).

**Out of scope — each mapped to a named leaf or an owning task:**

- **Output-rate conversion at the export edge.** This task produces the mix at
  the composition working rate. Converting to a *different* container output
  rate (e.g. a 48 kHz working composition exported at 44.1 kHz) is the same
  working→edge resampler the device monitor uses (`StreamingResampler`,
  `src/media/arbc/media/streaming_resampler.hpp:44-97`, which already ships both
  up-sampling and the ratio-scaled widened-lowpass decimation from
  `audio.device_edge_resample` / `audio.device_edge_decimation`). Deferred to
  **`audio.export_edge_resample`** (closer registers in WBS, milestone
  `m6_audio`) — see Acceptance criteria. See the doc-12 delta below.
- **The realtime scheduler, the ring, the device clock.** Lookahead priming,
  the prepared-block ring, worker-thread render-ahead, and clock mastering are
  `audio.lookahead` / `audio.device_monitor` (both DONE). Export has no
  deadline and no device clock: it mixes exactly, synchronously, off any clock
  — "video chases audio" is the *device/interactive* discipline
  (device_monitor.md:133-136).
- **Muxing, containers, file I/O, codecs.** The driver hands back sample blocks;
  "Muxing audio with exported frames is the host's business (or a container
  plugin's), not the core's" (doc 12:190-191). No codec, container, or file I/O
  in this task (the codec line, doc 17:150-173).
- **Spatial mix policy.** Only `Flat` (the shipped `MixPolicy`); Spatial pan/
  attenuation/sub-audible-cull is `audio.spatial_policy` (`45-audio.tji:74-79`),
  inserted behind the same `MixPolicy` seam without a signature change.

## Why it needs to be done

`m6_audio` is the audio milestone (`99-milestones.tji:49-51`); every other
audio deliverable — mix engine, lookahead, device monitor and its edge
resampling, latency pre-roll, RT-safety — has landed or is in flight, and
`audio.export_monitor` is one of its remaining dependencies. Doc 12 makes the
export monitor one of the **two monitor implementations that matter** (doc
12:169-191) and, per the scheduling decision (doc 12:283-284), the *first* of
the two to implement — "export monitor (no realtime pressure) second, device
monitor + lookahead scheduler last" — precisely because it carries none of the
realtime complexity. What it adds over calling `mix_composition` directly is the
**export contract**: a whole time range mixed against **one** revision so that
an edit committed mid-export cannot leak into late blocks (doc 02:77-80 —
"needed for 'export while editing' … frame N must not see frame N+1's edits"),
and a block-loop whose concatenated output is deterministic and byte-
reproducible. The video half of that contract already shipped as
`SequenceRenderer`; this task completes the A/V export pair so a host can render
a consistent audio track alongside its exported frames (doc 12:187-191), leaving
the host to encode and mux.

## Inputs / context

- `docs/design/12-audio.md`:
  - **`:187-191`** (`## The engine: monitors, clocking, lookahead`, Export
    monitor bullet) — the normative scope: "**Export monitor** (offline):
    sample-exact rendering of the mix over a time range, driven by the offline
    renderer's frame loop; snapshot semantics per doc 02 keep an export
    consistent with concurrently-edited scenes. Muxing audio with exported
    frames is the host's business (or a container plugin's), not the core's."
    **This task adds a one-sentence delta here** (see Decisions) clarifying the
    output rate.
  - `:169-170` — "A **monitor** is the audio analog of a viewport: attached to a
    transport, it pulls the mix." (Export pulls the mix off the offline loop
    rather than a transport clock.)
  - `:94-104` (`## Working format`) — per-composition working sample rate
    (default 48 kHz) + layout, float32; "Conversions live at the edges … the
    monitor converts working → device." The export produces at the working rate;
    a different output rate is such an edge conversion.
  - `:106-121` — the shared band-limited polyphase resampler and its up / 1:1 /
    widened-lowpass-decimation directions (the seam the deferred export-edge
    leaf reuses).
  - `:271-284` (Scheduling decision) — the export monitor is v1 scope and
    sequenced before the device monitor.
- `docs/design/02-architecture.md`:
  - **`:73-85`** (`## The frame, offline`) — "Same steps without deadlines,
    quantization, or placeholders: exact scale, every request rendered to
    completion … A **snapshot** mechanism (freeze revisions during a frame)
    keeps a frame consistent even if the scene is being mutated concurrently —
    needed for 'export while editing' and for video where frame N must not see
    frame N+1's edits." The audio export inherits this snapshot contract
    verbatim, one signal dimension down.
- `docs/design/11-time-and-video.md`:
  - `:209-213` — the offline sequence loop ("snapshot, set time, exact render …
    nothing new beyond `time` in the requests"); the export monitor's block loop
    is the audio analog (snapshot, window, exact mix).
- `docs/design/17-internal-components.md`:
  - **`:60`** — `arbc::runtime` is **L5**; contents include "monitor objects …
    offline/export drivers"; "Depends on: everything below." The export monitor
    is a runtime object.
  - **`:78-86`** — "The two render drivers live in `runtime`, not the engines.
    The engines are libraries over pinned versions; deadlines, frame loops, and
    device clocks are runtime policy." The audio-engine `mix_composition` is the
    library; the export driver is the runtime policy over it.
  - **`:150-173`** (the codec line) — `libarbc` stays codec- and device-free;
    muxing/containers ride the far side of this line (host or plugin), so the
    export monitor hands back blocks and links no codec.
- `docs/design/16-sdlc-and-quality.md` — `:15-25` claims register
  (`<doc-stem>#<slug>` + `enforces:` tag); `:54-62` behavioral counters, never
  wall-clock; `:66-73` concurrency tests under TSan; deterministic rendering →
  byte-exact goldens; `:112-118` ≥90% diff coverage.

- **Code seams the implementation extends:**
  - `src/audio_engine/arbc/audio_engine/mix.hpp:33,41,59-61` —
    `mix_composition`, `MixResolver`, `MixPolicy` (the L4 core this driver
    calls).
  - `src/contract/arbc/contract/content.hpp:217-249,285-287` — `AudioRequest`
    (`window`/`sample_rate`/`layout`/`target`/`exactness`/`snapshot`),
    `AudioResult` (`achieved_rate`/`exact`), and the contract note that `Exact`
    is the "offline export" mode that "must be faithful".
  - `src/compositor/arbc/compositor/pull_service.hpp:93` — `PullServiceImpl` +
    `using BlockCache = KeyedStore<BlockKey, AudioBlockValue>`; the concrete
    `pull_audio` the mix engine drives; `src/cache/arbc/cache/key_shapes.hpp:83-90`
    `BlockKey { content, revision, block_index, rate }`.
  - `src/compositor/arbc/compositor/counters.hpp` — `note_audio_dispatch`, the
    behavioral counter proving facet-less/silent layers cost zero dispatch.
  - `src/runtime/arbc/runtime/offline_sequence.hpp:58,63-104` —
    `SequenceRenderer`, `frame_times_over`, `FrameSink`, `render_frame_at`,
    `render_sequence`: the exact class shape (pin-once, caller-owned series,
    thin sink loop, non-copyable/non-movable) this task mirrors.
  - `src/runtime/arbc/runtime/document.hpp:34` (`Document::pin()`),
    `src/model/arbc/model/model.hpp:37,45,101,163` (`DocRoot`, `revision()`,
    `DocStatePtr`, `Model::current()`) — the snapshot pin.
  - `src/media/arbc/media/audio_format.hpp:17-28` (`AudioFormat`,
    `k_working_audio`), `src/media/arbc/media/audio_block.hpp` (`AudioBlock`,
    `ChannelLayout`), `src/base/arbc/base/time.hpp:29-54` (`TimeRange`,
    half-open).
  - `src/runtime/CMakeLists.txt` — `DEPENDS base model contract compositor pool
    cache audio_engine` (all present; **no new edge**); this task appends
    `export_monitor.cpp` to `SOURCES`, `arbc/runtime/export_monitor.hpp` to
    `PUBLIC_HEADERS`, and `t/export_monitor.t.cpp` to the component test.

- **Existing claims to extend, not duplicate** (`tests/claims/registry.tsv`):
  `:73` `12-audio#mix-engine-mixes-layers-additively`, `:74`
  `12-audio#pull-audio-is-cache-first-single-settle`, `:75`
  `12-audio#mix-engine-facetless-costs-nothing` — this task re-enforces each
  through the export driver via a second `enforces:` tag (no new row); it adds
  two new rows for the export-specific contract.

- **Test precedents:** `src/audio_engine/t/mix.t.cpp:250-251,292,330`
  (`bytes_equal` = `std::memcmp` byte-exact block goldens, `parab_sine`
  integer-flick oracle — never `std::sin`); `tests/tone_conformance.t.cpp`,
  `tests/nested_audio_concurrency.t.cpp` (cross-component byte-exact goldens
  linking real `PullServiceImpl` + `org.arbc.tone` in the not-level-checked
  top-level `tests/`); `src/runtime/t/offline_sequence.t.cpp` (the
  concurrent-writer single-revision-pin idiom: pin R, commit N revisions on a
  second thread synchronized on an atomic `go`, assert every output reads
  revision R and equals the single-revision golden, TSan-clean).

## Constraints / requirements

1. **Levelization (doc 17:41-44,60,78-86).** `ExportMonitor` is L5
   `arbc::runtime`, calling the L4 `mix_composition` directly and pulling
   through `compositor`'s `PullServiceImpl` over the pinned `DocRoot`, exactly as
   `SequenceRenderer` drives the compositor. **No new `DEPENDS` edge** —
   `audio_engine`, `compositor`, `contract`, `model`, `cache` are already
   present. The CI dependency check stays green; naming an out-of-closure type
   is a build failure.
2. **Single-revision snapshot for the whole export (doc 02:77-80, doc
   12:189-190).** The instance pins one `DocStatePtr` at construction and mixes
   **every** block against that one `DocRoot::revision()`. A writer thread
   committing new revisions during the export must not change any exported
   block; the export is consistent *across* blocks, not merely within one.
3. **Exact, faithful mix (contract `content.hpp:239-240,285-287`, doc
   02:73-75).** Every `AudioRequest` carries `Exactness::Exact` and no deadline;
   the mix renders every contributor to completion. For a composition whose
   contributors honor the working rate, every block's `AudioResult` reports
   `exact == true` and `achieved_rate == working_rate`. `achieved_rate` / `exact`
   fold as `min` / conjunction over contributors exactly as `mix_composition`
   defines (a recorded contributor bottoming out below the working rate is
   reported honestly, not hidden).
4. **Produce at the working rate (doc 12:94-104).** The mix is produced at the
   composition working `AudioFormat`; the block-window series and every
   `AudioRequest.sample_rate` use that rate. Converting to a different container
   output rate is an export-edge concern deferred to `audio.export_edge_resample`
   (Decision + doc-12 delta).
5. **Determinism / byte-reproducibility (doc 16).** `render_range` over a fixed
   `(range, block_frames)` against a fixed pinned revision produces byte-
   identical blocks on re-run, and the concatenation of the block outputs is
   byte-identical to `mix_composition` called once per window directly (the loop
   adds no per-block state or phase reset — the mix engine is stateless per
   call). Block-boundary invariance: the concatenated output is independent of
   `block_frames`.
6. **Facet-less / silent layers cost nothing (doc 12:73-77, behavioral
   counter).** Through the export driver, layers with no audio facet or that are
   `!audible()`/`gain<=0`/out-of-span issue **zero** `pull_audio` dispatches; the
   mixer issues exactly one dispatch per audible in-span layer with an audio
   facet — asserted on the `note_audio_dispatch` counter, never wall-clock.
7. **Output boundary (doc 12:190-191, doc 17:150-173).** The driver hands
   `AudioBlock`s to a host `BlockSink`; it writes no files, muxes nothing, and
   links no codec/container. The host owns encoding/muxing.
8. **Faults-as-values (doc 10).** An empty range, a non-positive `block_frames`,
   or an unresolved composition id yields an empty block series / a silence-
   filled block reporting the honest `AudioResult`, never an abort.
9. **Diff coverage ≥ 90%** on changed lines (doc 16:112-118).

## Acceptance criteria

**Claims-register growth** — two new rows appended to `tests/claims/registry.tsv`
(format `<claim-id><TAB><description>`), each with an `enforces:` tagged test:

- `12-audio#export-monitor-mixes-exactly-over-range` — "The export monitor tiles
  a half-open time range into contiguous working-rate blocks and mixes each
  through `mix_composition` in `Exact` mode against one pinned revision; the
  concatenated block output is byte-identical to `mix_composition` called once
  per window directly and independent of the block size, every block reports
  `exact == true` and `achieved_rate == working_rate` for a rate-honoring
  composition (folding `min`/conjunction honestly for a below-rate contributor),
  facet-less/silent layers issue zero `pull_audio` dispatches, and a re-run
  produces byte-identical output — the offline sample-exact drive of the mix
  core." (doc 12:187-191, contract `content.hpp:239-240,285-287`)
- `12-audio#export-monitor-pins-single-revision` — "The export monitor pins one
  document revision at construction and mixes every block of the range against
  it, so a writer thread committing new revisions during the export changes no
  exported block; every block reads `DocRoot::revision() == R` and the export
  equals the single-revision golden — the audio twin of
  `02-architecture#offline-sequence-pins-single-revision`." (doc 02:77-80, doc
  12:189-190)

**Second `enforces:` tags (no new rows)** exercising the mix core through the
export driver: `12-audio#mix-engine-mixes-layers-additively` (`registry.tsv:73`),
`12-audio#pull-audio-is-cache-first-single-settle` (`:74`), and
`12-audio#mix-engine-facetless-costs-nothing` (`:75`).

**Unit + byte-exact goldens — deterministic, no sleep — in
`src/runtime/t/export_monitor.t.cpp` (new, Catch2, registered via
`arbc_component_test`):**

- **Block-loop == per-window oracle (byte-exact):** export a composition of
  tones over a fixed `[t0,t1)` range; assert the concatenated `render_range`
  output is byte-identical (`std::memcmp`, no tolerance) to `mix_composition`
  called once per window directly, and byte-identical across two different
  `block_frames` values (block-boundary invariance). Enforces
  `12-audio#export-monitor-mixes-exactly-over-range` and re-enforces
  `12-audio#mix-engine-mixes-layers-additively`.
- **Exact faithfulness:** for a rate-honoring composition every block's
  `AudioResult` reports `exact == true` and `achieved_rate == working_rate`; a
  below-working-rate contributor folds `achieved_rate`/`exact` honestly (min /
  false), never silently upgraded.
- **Determinism:** re-running the same export yields byte-identical output.
- **Facet-less/silent cost nothing:** a composition mixing audible tones with
  silent/facet-less layers issues exactly N `pull_audio` dispatches for N
  audible in-span layers and zero for the rest — asserted on `note_audio_dispatch`.
  Re-enforces `12-audio#mix-engine-facetless-costs-nothing` and
  `12-audio#pull-audio-is-cache-first-single-settle`.
- **Faults-as-values:** an empty range and a non-positive `block_frames` yield an
  empty window series; an unresolved composition id yields a silence block with
  an honest `AudioResult` — asserted on values, no abort.

**Cross-component byte-exact integration golden — in the top-level (not
level-checked) `tests/audio_export_goldens.t.cpp` (new), linking real
`PullServiceImpl` + `org.arbc.tone`:** export a multi-tone composition over a
range and assert the concatenated blocks are byte-identical to summing the
individual tone renders directly (the `tone_conformance` oracle style), and
identical between two block sizes. Wired into `tests/CMakeLists.txt`.

**Focused concurrency test — outcome assertions only, runs under TSan — in
`src/runtime/t/export_monitor.t.cpp`:** construct an `ExportMonitor` (pinning
revision R), then on a second thread commit N revisions to the `Model`
(spin-synchronized on an atomic `go`, `std::this_thread::yield()` at fixed-stride
points — the idiom from `offline_sequence.t.cpp`) **while** `render_range` runs.
Assert **outcomes only**: every exported block reads `DocRoot::revision() == R`
(the pin held), the concatenated output equals the single-revision golden (no
mid-export edit leaked in), and no crash/hang. **No timing assertion.** Runs
green under `dev`, `asan`, and `ctest --preset tsan`. Enforces
`12-audio#export-monitor-pins-single-revision`. (Per
[[audio-lookahead-nested-cache-serialization]], the offline path is single-
threaded through one `PullServiceImpl`/`BlockCache`, so no worker fans out cache
reads here — the concurrency under test is solely the writer-vs-export snapshot
race, not concurrent cache reads.)

**Component wiring & CI dependency check:** `src/runtime/CMakeLists.txt` adds
`export_monitor.cpp` to `SOURCES`, `arbc/runtime/export_monitor.hpp` to
`PUBLIC_HEADERS`, and `t/export_monitor.t.cpp` to the component test; **no
`DEPENDS` change**; the header compiles standalone; the doc-17 dependency check
passes.

**Design-doc delta:** a one-sentence clarification to doc 12's Export monitor
bullet (`docs/design/12-audio.md:190-191`) stating the export produces the mix
at the working rate and that container-output-rate conversion is the same
shared working→edge resampler (a deferred edge leaf) — see Decisions. Rides in
the closer's commit (doc 16 same-commit rule). Not project-shaping → no doc-00
bullet.

**Deferred follow-up (named future task; closer registers in WBS,
milestone `m6_audio`):**

- **`audio.export_edge_resample`** — effort **1d**, `depends
  !export_monitor, audio.device_edge_decimation` — "Convert the export monitor's
  working-rate blocks to a caller-requested container output rate at the export
  edge, reusing the shipped `StreamingResampler`
  (`src/media/arbc/media/streaming_resampler.hpp`, which already ships up-sampling
  and the ratio-scaled widened-lowpass decimation), so exports can target
  44.1/96 kHz container rates byte-exactly and continuously across block
  boundaries; a matched output rate keeps the 1:1 pass unchanged. Source:
  `audio.export_monitor` scope boundary; see
  `tasks/refinements/audio/export_monitor.md`. Doc 12."

**Gate green:** build + tests in `dev` + ASan/UBSan + TSan; the concurrency test
green under `tsan`; ≥90% diff coverage; `tj3 project.tjp 2>&1 | grep -iE
"error|warning"` silent after the closer's `complete 100`.

## Decisions

- **`ExportMonitor` is a stateful L5 class that pins one revision for the whole
  export, mirroring `SequenceRenderer`; not a per-call free function.** The
  consistency guarantee doc 02:77-80 demands — a mid-export edit must not leak
  into late blocks — is a property of the *whole range*, so the pinned
  `DocStatePtr` must outlive every block and be shared by all of them, and the
  `PullServiceImpl`/`BlockCache` the mix pulls through wants the same long-lived
  home. A class holds exactly this state, the audio twin of the video-side
  `SequenceRenderer` (a class for the identical "carries state across the
  sequence" reason, offline_sequences.md:446-462). *Rejected:* a free
  `export_range(...)` that re-pins per call — it would either re-pin per block
  (letting a mid-export edit leak in, the exact failure doc 02:79 warns against)
  or force the caller to thread the pin, resolver, and pull service through
  every call. *Rejected:* bundling audio into `SequenceRenderer` — audio mixing
  is L4 `audio_engine`, a different milestone (`m6_audio` vs `m5_video`), and
  the two drivers pin the same revision but produce independent artifacts a host
  pairs; keeping them siblings matches the doc-12 monitor/viewport symmetry.
- **Drive `mix_composition` directly in `Exact` mode; do not touch the ring,
  pump, or device clock.** The export path `depends !mix_engine` (not
  `!lookahead`/`!device_monitor`) precisely because the pure mix core is all it
  needs: `mix_composition` is a deterministic function over the pinned `DocRoot`
  and already accepts `Exactness::Exact`. Offline has no deadline, so the mix
  renders every contributor to completion synchronously — no render-ahead ring,
  no clock mastering, no RT-safety discipline (all `device_monitor` concerns).
  This is doc 12:283-284's "export monitor (no realtime pressure)". *Rejected:*
  reusing the `LookaheadRing`/`LookaheadPump` for offline — they exist to hide
  latency behind a realtime clock the export does not have; they would add a ring
  and a fake clock for no benefit and complicate the byte-exact golden.
- **Produce the mix at the working rate; defer container-output-rate conversion
  to `audio.export_edge_resample` — with a one-sentence doc-12 delta.** Doc 12's
  export bullet (`:187-191`) is silent on the output rate; doc 12:94-104 settles
  that the mix graph is defined at the working rate and "conversions live at the
  edges." Producing at the working rate is exactly what the mix core is built and
  fully tested for, and is the common export case (a 48 kHz composition exported
  at 48 kHz — no conversion). A *different* container rate is a working→edge
  conversion identical to the device monitor's, and the engine already ships the
  resampler that does it (up + widened-lowpass decimation, from
  `device_edge_resample`/`device_edge_decimation`). Bundling that edge into this
  2d task would drag the whole SRC-phase/continuity surface (which took the
  device monitor three follow-up leaves) into scope. So this task emits
  working-rate blocks and the edge is its own 1d leaf — the same
  ship-the-core-then-add-the-edge cadence `device_monitor` → `device_edge_*`
  followed. The doc-12 delta records this so the constitution is no longer
  silent. *Rejected:* requesting `mix_composition` directly at a non-working
  `sample_rate` to get the output rate "for free" — a contributor native
  *above* the requested rate would hit `mix_composition`'s decimation path, whose
  faithfulness is a mix-engine property not established for arbitrary sub-working
  request rates; routing output-rate conversion through the one shipped,
  byte-exact edge resampler (which the device monitor already proves) is the
  safe, symmetric choice. *Rejected:* declaring output-rate conversion pure host
  territory like muxing — the engine ships the resampler and the device monitor
  converts in-engine; making the host re-implement SRC breaks that symmetry.
- **Flat mix policy only, behind the shipped `MixPolicy` seam.** doc 12:143-144
  makes mix policy a monitor choice with `Flat` the default; Spatial is a whole
  leaf (`audio.spatial_policy`). The export monitor passes `MixPolicy::Flat`
  through to `mix_composition` and exposes the parameter so
  `audio.spatial_policy` can select `Spatial` later without a signature change.
  *Rejected:* hard-coding `Flat` with no parameter — it would force a signature
  change when Spatial lands.
- **Host-owned output: hand back `AudioBlock`s through a `BlockSink`; no files,
  containers, or codecs.** doc 12:190-191 ("Muxing … is the host's business")
  and the codec line (doc 17:150-173) keep `libarbc` codec- and device-free. The
  driver's boundary is the sample block, exactly as `SequenceRenderer`'s is the
  `Surface`. *Rejected:* a built-in WAV/container writer — it crosses the codec
  line into L5 against doc 10.

## Open questions

(none — all decided.)

The doc-survey noted doc 12:189 cites "snapshot semantics per doc 02" while
`AudioRequest.snapshot` is a doc-14 `model::StateHandle`; these are already
consistent (the doc-14 `StateHandle` is the concrete pin that *realizes* doc
02's snapshot semantics, exactly as `RenderRequest.snapshot` does for video,
`content.hpp:234-236`), so no reconciliation delta is needed — this task simply
pins one `DocStatePtr` and threads its handle, the same machinery video already
uses.

## Status

**Done** — 2026-07-08.

- Delivered `arbc::ExportMonitor` (L5 `arbc::runtime`) in `src/runtime/arbc/runtime/export_monitor.hpp` + `src/runtime/export_monitor.cpp`: pins one `DocStatePtr` at construction and mixes every block against that frozen revision via `mix_composition` in `Exact` mode.
- Added `block_windows_over` free function tiling a half-open time range into contiguous working-rate block windows (the audio analog of `frame_times_over`).
- Component unit + byte-exact goldens + TSan concurrency test in `src/runtime/t/export_monitor.t.cpp` (block-loop==per-window oracle, boundary invariance, exact/achieved-rate faithfulness, below-rate honest fold, determinism, faults-as-values, counter assertion, single-revision-pin TSan case).
- Cross-component byte-exact integration golden in `tests/audio_export_goldens.t.cpp` through real `PullServiceImpl` + `org.arbc.tone`.
- Two new claims registered in `tests/claims/registry.tsv`: `12-audio#export-monitor-mixes-exactly-over-range`, `12-audio#export-monitor-pins-single-revision`; second `enforces:` tags on `12-audio#mix-engine-mixes-layers-additively`, `#pull-audio-is-cache-first-single-settle`, `#mix-engine-facetless-costs-nothing`.
- Minimal host-facing façade wrappers (`attach_layer`, `set_layer_gain`, `set_layer_audible`, `set_working_audio_format`) added to `src/runtime/arbc/runtime/document.hpp` + `src/runtime/document.cpp` to enable composition membership/audio placement required by `mix_composition`.
- CMake wiring in `src/runtime/CMakeLists.txt` + `tests/CMakeLists.txt` (no new `DEPENDS` edge).
- Doc-12 delta in `docs/design/12-audio.md` clarifying the export produces at the working rate and that container-output-rate conversion is a deferred edge leaf (`audio.export_edge_resample`).
