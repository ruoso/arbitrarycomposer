# audio.audio_types — Audio block types + working format

## TaskJuggler entry

Back-link: `tasks/45-audio.tji`, `task audio.audio_types` ("Audio block
types + working format"). This refinement expands that one-line WBS leaf.

## Effort estimate

`1d` (from the `.tji`). The vocabulary half already shipped with
`contract.audio_facet`; what remains is one small value type in `arbc::media`
and a mechanical mirror of the existing per-composition working-space
plumbing in `arbc::model`.

## Inherited dependencies

The parent `task audio` carries `depends contract.audio_facet,
timeline.transport`; `audio_types` is its first child and adds no `depends`
of its own, so it inherits both.

- **`contract.audio_facet`** — *settled* (DONE 2026-07-07). Landed the block
  vocabulary this task builds on and explicitly deferred the working-format
  configuration here:
  - `enum class ChannelLayout { Mono, Stereo }` + `constexpr uint32_t
    channel_count(ChannelLayout)` at
    `src/media/arbc/media/audio_block.hpp:14-29` (L1 `arbc::media`).
  - `struct AudioBlock { float* samples; uint32_t frames; ChannelLayout
    layout; uint32_t rate; }` — a non-owning, trivially-copyable interleaved
    **float32** view at `src/media/arbc/media/audio_block.hpp:42-47`.
  - `AudioRequest` (carries `uint32_t sample_rate`, `ChannelLayout layout`)
    and `AudioResult` (carries `uint32_t achieved_rate`) in `arbc::contract`
    at `src/contract/arbc/contract/content.hpp:217-249` — the
    requested-vs-achieved-rate seam that a working format sits behind.
  - The header comment at `src/media/arbc/media/audio_block.hpp:38-40`
    names this task as the owner of "owned block storage and the
    working-format configuration", and `:11` names it as the owner of any
    richer channel layouts (5.1/ambisonic) "when a caller needs them".
- **`timeline.transport`** — *settled* (DONE 2026-07-07). No type-level
  constraint on this task; it only establishes that the transport already
  exposes the seek/position surface a future audio-clock master will drive,
  so audio work does not modify `arbc::Transport`. Listed for completeness.

## What this task is

Define the **per-composition working audio format** — the audio analog of
doc 07's working color space — as a small value type in `arbc::media`, and
store it on the composition record in `arbc::model` exactly as
`SurfaceFormat working_space` is already stored. The working format is a
working **sample rate** (default 48 kHz) and **channel layout** (default
stereo); samples are always float32, so — unlike the pixel side — there is
no format/variant axis to encode. This is the vocabulary and the
per-composition configuration; the machinery that *acts* on it (pulling
content at the working rate, resample/remix conversions at edges and nesting
boundaries) belongs to the mix engine and is deferred there (see
Constraints).

## Why it needs to be done

`audio.mix_engine` (`depends !audio_types`) needs a typed answer to "what
rate and layout does this composition mix at?" before it can pull blocks,
size targets, or convert a child composition's output at a nesting boundary.
Doc 12 makes the working format the pull "scale" for audio — the temporal
analog of pixels-per-unit — so it is a prerequisite for every audio pull.
Storing it on the composition record (rather than passing it as a loose
argument) is what lets nested compositions declare different working formats
and lets the boundary converter read both sides, which is the whole point of
the doc-07/doc-12 working-format symmetry. Downstream consumers:
`mix_engine`, `export_monitor`, `device_monitor`, and every audio facet's
`render_audio` (which receives the working rate/layout through
`AudioRequest`).

## Inputs / context

- **doc 12 "Working format"** (`docs/design/12-audio.md:95-105`) — the
  normative spec: "a working sample rate (default 48 kHz) and channel layout
  (default stereo), samples always float32 … one format suffices …
  Conversions live at the edges exactly as in doc 07 … Nested compositions
  may declare different working formats; the nesting boundary converts
  (resample + remix), homogeneous trees pay nothing."
- **doc 12 "The symmetry"** (`docs/design/12-audio.md:7-22`) — the row
  "Working color space + pixel format (doc 07) | Working sample rate +
  channel layout + float32" pins the analogy this task implements.
- **doc 12 "Rate maps, varispeed"** (`docs/design/12-audio.md:107-121`) and
  **"Recursion"** (`:202-208`) — establish that rate conversion is
  *varispeed resampling* through composed rational rates, i.e. DSP owned by
  the mix engine, not this task.
- **doc 17 levelization** (`docs/design/17-internal-components.md:50,52,78`)
  — `arbc::media` (L1) is "channel layouts, typed pixel/sample span views"
  (docs 07, 12); the `model → media` edge already exists ("a composition
  record stores its per-composition working space as a `SurfaceFormat`");
  media descriptors are "level-1 vocabulary and composition records are
  precisely where configuration vocabulary lives".
- **The color precedent** — `tasks/refinements/color/working_space.md` and
  `tasks/refinements/color/format_set.md`. `SurfaceFormat {PixelFormat,
  ColorSpace, Premultiplied}` at `src/media/arbc/media/surface_format.hpp:26`
  with the default constant `k_working_rgba32f` at `:36`; stored as
  `SurfaceFormat working_space{}` on the composition record
  (`src/model/arbc/model/records.hpp:117`, comment `:107-109`), configured
  via `Model::Transaction::set_working_space`
  (`src/model/model.cpp:601-620`) and resolved via `DocRoot::working_space()`
  (`src/model/model.cpp:409-429`; declared `src/model/arbc/model/model.hpp:61,235`).
  Unit tests at `src/model/t/model.t.cpp:85-139`.
- **Existing block vocabulary** — `src/media/arbc/media/audio_block.hpp`
  (whole file); media wiring `src/media/CMakeLists.txt`.
- **doc 12 scheduling decision** (`docs/design/12-audio.md:218-232`) —
  "contract + model first, export monitor … second, device monitor …
  last"; this task is the "model first" step for audio.

## Constraints / requirements

1. **New value type in `arbc::media` (L1), header-only.** Add
   `struct AudioFormat { uint32_t sample_rate; ChannelLayout layout; }` with
   member-wise `operator==` (defaulted), plus the default constant
   `k_working_audio` = `{48000, ChannelLayout::Stereo}`. Put it in a new
   `src/media/arbc/media/audio_format.hpp` that `#include`s
   `audio_block.hpp` for `ChannelLayout` — mirroring how `surface_format.hpp`
   is separate from `pixel_format.hpp`. Register it in the `media`
   `PUBLIC_HEADERS` set. `AudioFormat` must be trivially copyable. Do **not**
   fold a sample-format field in: float32 is the sole format (doc 12:98), so
   there is no analog of `PixelFormat` on the audio side.
2. **Do not break the shipped block vocabulary.** `ChannelLayout` and
   `AudioBlock` (`audio_block.hpp`) keep their exact field names, order, and
   trivial-copyability; `AudioBlock` stays a non-owning view. `ChannelLayout`
   may be *extended* losslessly if a caller needs it, but v1's working
   default is stereo, so this task adds no new enumerators (5.1/ambisonic
   stay deferred per `audio_block.hpp:11`).
3. **Per-composition storage in `arbc::model` (L2), mirroring
   `working_space`.** Add `AudioFormat working_audio_format{}` to the
   composition record (`src/model/arbc/model/records.hpp`, beside
   `working_space`); add `Model::Transaction::set_working_audio_format(
   ObjectId composition, const AudioFormat&)` and
   `DocRoot::working_audio_format()` (plus the single-composition no-arg
   convenience form that `working_space()` has), routing through the same
   transaction/revision/pin machinery. `records.hpp` gains
   `#include <arbc/media/audio_format.hpp>`.
4. **No new levelization edge, no design-doc delta.** The `model → media`
   edge already exists (doc 17:52,78; landed by `color.working_space`), and
   doc 12 already specifies the format and its defaults. This task turns a
   settled design into a work order; it must *not* amend a design doc. If
   implementation surfaces a genuine gap, that is an escalation (return
   summary → parking lot), not a silent delta.
5. **Conversion machinery is out of scope — it is the mix engine's.** Per
   doc 12:100-104,107-121,202-208 the acts of pulling content at the working
   rate, varispeed **resample** (rate conversion, exact rational rates), and
   **remix** (channel-layout conversion) at edges and nesting boundaries are
   the mix engine's normalization of contributions into the composition's
   working format. `audio.mix_engine` (`tasks/45-audio.tji:11-16`) and the
   nested kind's audio facet (`arbc::kind-nested`) already own that work;
   this task defines the format they read and convert *toward*. No
   conversion kernel lands here, and — since no audio mix path exists yet —
   no cross-format assertion is added yet either (it lands with the consumer,
   as the color side's tag-agreement assertion landed with the compositor).
6. **Capability honesty holds trivially.** Unlike the pixel side, where the
   designed default (16f) was describable before storable, audio's single
   float32 format is fully storable from day one: `k_working_audio` is a
   real, functional default the moment it exists — no staged-default caveat,
   no "describable before storable" gap.
7. **Diff coverage ≥ 90%** on changed lines (doc 16 CI gate); tests ship in
   this task, not a follow-up.

## Acceptance criteria

- **Claims-register growth.** Add one entry to `tests/claims/registry.tsv`:
  - `12-audio#composition-carries-working-audio-format` — "A fresh
    composition defaults its working audio format to `k_working_audio`
    (48 kHz, stereo); `set_working_audio_format` publishes a new revision
    whose value is visible on the pinned version and leaves earlier pinned
    versions unchanged; it is a no-op on an absent or non-composition id."
    Enforced by an `enforces: 12-audio#composition-carries-working-audio-format`
    tagged block of tests in `src/model/t/model.t.cpp`, mirroring the
    `working_space` cases at `src/model/t/model.t.cpp:85-139`
    (defaults-on-fresh, set-bumps-revision, pinned-version-sees-value,
    no-op-on-absent/non-composition).
- **Media unit tests.** New `src/media/t/audio_format.t.cpp` (added to the
  `media` `arbc_component_test` list), mirroring the style of
  `src/media/t/pixel_format.t.cpp`: `AudioFormat` member-wise equality and
  copy; `k_working_audio == AudioFormat{48000, ChannelLayout::Stereo}`;
  `static_assert(std::is_trivially_copyable_v<AudioFormat>)`;
  `channel_count(k_working_audio.layout) == 2`.
- **No golden churn.** Nothing is rendered or mixed by this task, so there
  are no byte-exact goldens to add and no existing golden output changes
  (the default composition still renders identically). Deterministic unit
  assertions only — no tolerances, no wall-clock assertions.
- **No new concurrency surface.** `AudioFormat` is an immutable value and
  the model mutation reuses the existing transaction machinery, which
  carries its own TSan coverage; this task adds no thread and no TSan
  obligation.
- **WBS gate.** After the closer adds `complete 100` and the `Refinement:`
  back-link to `tasks/45-audio.tji`, `tj3 project.tjp 2>&1 | grep -iE
  "error|warning"` is silent.
- **No deferred WBS follow-ups from this task.** The one piece of
  implementable work this task does not do — the resample/remix conversion —
  is already owned by the existing `audio.mix_engine` leaf (and the nested
  kind's audio facet), so there is nothing new to register. This task
  registers no successor.

## Decisions

- **`AudioFormat` as one value type, not loose `(rate, layout)` args**
  — mirrors `SurfaceFormat`'s "one value so signatures don't accrete
  parameters" (`surface_format.hpp:18-25`). The composition stores one
  field; the mix engine reads one value; a nesting boundary compares two
  values. *Rejected:* threading `uint32_t sample_rate` + `ChannelLayout`
  separately through every audio signature — the exact parameter-accretion
  the pixel side already refused.
- **No sample-format axis on `AudioFormat`.** float32 is the sole audio
  format (doc 12:98-100: "audio's numbers are already linear … one format
  suffices"), so there is no audio analog of `PixelFormat`/kernel-variant
  machinery. `AudioFormat` carries only the two axes that vary: rate and
  layout. *Rejected:* a symmetric-for-symmetry's-sake `SampleFormat` enum
  with one member — dead vocabulary.
- **New `audio_format.hpp`, separate from `audio_block.hpp`.** Keeps the
  facet-named *block view* vocabulary (`AudioBlock`, `ChannelLayout` — what
  `render_audio` names) distinct from the composition-named *working-format
  configuration* (`AudioFormat` — what the model stores), exactly as
  `surface_format.hpp` is separate from `pixel_format.hpp`. The
  `audio_block.hpp` header comment already frames these as distinct
  concerns. *Rejected:* appending `AudioFormat` into `audio_block.hpp` —
  smaller diff, but blurs the block/config split the facet task drew.
- **Store on the composition record in `arbc::model`, not in the engine.**
  doc 17:78 settles that per-composition configuration vocabulary lives on
  the composition record and that the `model → media` edge is the clean
  downward one; `working_space` set the precedent. This is what lets nested
  compositions declare their own formats (doc 12:104) and keeps the mix
  engine stateless over pinned versions. *Rejected:* an engine-side format
  map keyed by composition id — reintroduces cross-frame state into a
  library meant to be pure over pinned revisions.
- **Conversion deferred to the mix engine, not stubbed here.** Following
  `color.format_set`'s "capability honesty over stubs": define the format,
  let the consumer do the conversion. Audio's advantage over color is that
  the default format is fully functional immediately (single float32
  format), so there is not even a staged-default flip to hand off. The
  resample+remix is inherently the mix engine's per-pull normalization
  (doc 12:100-104,202-208) and is already a named leaf — no new task, and
  crucially **no "audit/revisit" task**.
- **`k_working_audio` as the default constant name.** Mirrors the
  `k_working_rgba32f` convention (`surface_format.hpp:36`). Documented in
  the header as 48 kHz stereo. *Rejected:* a verbose
  `k_working_audio_48k_stereo` — the pixel side keeps the format terse in
  the name and full in the comment; match it.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-07.

- Added `struct AudioFormat { uint32_t sample_rate; ChannelLayout layout; }` with defaulted `operator==` and constant `k_working_audio` = `{48000, ChannelLayout::Stereo}` at `src/media/arbc/media/audio_format.hpp`.
- Added unit tests for `AudioFormat` (equality, copy, `is_trivially_copyable`, `channel_count`) at `src/media/t/audio_format.t.cpp`; wired into the `media` component-test list via `src/media/CMakeLists.txt`.
- Added `AudioFormat working_audio_format{}` to the composition record in `src/model/arbc/model/records.hpp`, mirroring `SurfaceFormat working_space`.
- Added `DocRoot::working_audio_format()` and `Transaction::set_working_audio_format` declarations to `src/model/arbc/model/model.hpp`.
- Implemented both methods in `src/model/model.cpp`, routing through the same transaction/revision/pin machinery as `working_space`.
- Registered claim `12-audio#composition-carries-working-audio-format` in `tests/claims/registry.tsv`; enforced by two `enforces:`-tagged cases in `src/model/t/model.t.cpp` (defaults+set+version, no-op on absent/non-composition).
