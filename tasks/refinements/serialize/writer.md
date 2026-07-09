# serialize.writer — Canonical writer

## TaskJuggler entry

Back-link: [`tasks/60-serialize.tji:12-16`](../../60-serialize.tji) — `task writer`
inside `task serialize`.

> note "Sorted keys, fixed number formatting — documents diff cleanly; serialize
> a pinned version (autosave never pauses editing). Docs 08/14."

## Effort estimate

**2d** (`tasks/60-serialize.tji:13`). This is the first *implementation* task in
the serialize stream and the one that stands up the whole component: it creates
`src/serialize/` and the `arbc_serialize` library, links the `nlohmann/json`
target settled by `json_dep` onto it, and lands the canonical emission engine
plus the pinned-snapshot document walk. It is peer-weighted with `reader` (2d)
and `kind_params` (2d) — heavier than the `json_dep` decision gate (0.5d) because
it writes real, golden-pinned code, but bounded by a deliberately narrow field
scope (envelope + composition + core-owned layer *placement*; content bodies and
sharing are the next two tasks).

## Inherited dependencies

`writer` declares `depends !json_dep` (`tasks/60-serialize.tji:15`) and, through
its parent `task serialize` (`tasks/60-serialize.tji:4`), inherits
`depends contract.registry, model.journal`.

**Settled (formal `depends`):**

- **`serialize.json_dep`** — DONE (2026-07-09,
  [`serialize/json_dep.md`](json_dep.md)). Ratified `nlohmann/json` at pinned
  `GIT_TAG v3.11.3`, wired find-first `FetchContent` with SYSTEM includes
  (`CMakeLists.txt:107-131`), and proved the three load-bearing capabilities in a
  standalone smoke test (`tests/serialize_json_dep_smoke.t.cpp`): verbatim tree
  round-trip, the non-throwing `parse(input, nullptr, false)` path, and default
  `json` sorted-key ordering. This task is the first consumer that links
  `nlohmann_json::nlohmann_json` onto an `arbc_*` component. json_dep Decision 4
  explicitly deferred creating `src/serialize/` and the `arbc_serialize`
  component to **this** task ("`writer`'s first act", `json_dep.md:280-288`).
- **`model.journal`** — DONE (2026-07-05,
  [`model/journal.md`](../model/journal.md)). Supplies the immutable-versions
  model, the atomic publish cell, and the pin machinery the writer reads to
  "serialize a pinned version." The concrete seam: `Model::current()` returns a
  `DocStatePtr` (a `std::shared_ptr<const DocRoot>`) that pins one revision
  without locks (`src/model/arbc/model/model.hpp:171,109`).
- **`contract.registry`** — DONE (2026-07-09,
  [`contract/registry.md`](../contract/registry.md)). Establishes the reverse-DNS
  kind id as the persistent serialization token (`registry.md:66-69`) and the
  unknown-kind-round-trip discipline. Bears on this task only indirectly: the
  writer's field scope stops *before* the content body (`kind`/`params`), so it
  does not yet resolve kind ids — that is `kind_params`'s inheritance (see
  Decision 4).

No **pending** inherited dependencies — all three predecessors landed.

**Downstream (this task unblocks):**

- `serialize.reader` (`tasks/60-serialize.tji:18-22`, `depends !writer`) — the
  inverse, loading to a fresh document at version 0.
- Transitively `serialize.kind_params` (`:24-29`), `serialize.sharing` (`:30-35`),
  `serialize.format_tests` (`:36-41`). Each layers onto the component and engine
  this task stands up: `kind_params` adds per-content `serialize()`/`deserialize()`
  hooks and the unknown-kind placeholder; `sharing` adds `inputs` arrays and the
  `contents`/`$ref` table; `format_tests` broadens goldens and adds loader fuzzing.

## What this task is

Stand up the serialization component and its canonical *writer*: the code that
takes one pinned document version and emits its byte-canonical `.arbc` JSON.
Concretely: (a) create the **`arbc_serialize` component** (`src/serialize/`,
`DEPENDS contract model`) and link `nlohmann_json::nlohmann_json` onto it PRIVATE;
(b) implement the **pinned-snapshot walk** — pin `Model::current()`, hold the
`DocStatePtr`, and read the `DocRoot` object graph through its lock-free `const`
peek accessors on any thread while editing continues; (c) implement the
**canonical emission engine** — sorted keys and the concrete "fixed number
formatting" rule this task pins (doc 08 Principle 5, amended by this task's delta),
emitting to a byte buffer with errors surfaced as `arbc::expected`, never a
thrown `nlohmann` exception; (d) emit the **document envelope + composition +
core-owned layer placement** — `{"arbc":{"format":1}}`, the `composition` object
(`canvas`, `working_space`/`working_audio_format` omit-when-default), and each
layer's spatial *and* temporal/audio placement (`transform`, `opacity`,
`visible`, and the omit-when-default twins `gain`, `audible`, `span`, `time_map`),
in bottom-to-top order. This task does **not** emit the content body (`kind`,
`kind_version`, `params`), operator `inputs`, or the `contents`/`$ref` sharing
table — those are `kind_params`/`sharing` (Decision 4); it lands the walk and
the injection seam they extend.

## Why it needs to be done

The serialize stream — and milestone M7 — needs a component and a canonical
writer before anything else in the chain can exist: `reader` is the inverse of
this writer, `kind_params` extends its per-layer emission with the kind hook,
and `sharing` extends it with the contents table. json_dep settled the library
and proved it in isolation but deliberately left the component uncreated
(`json_dep.md:280-288`); this task is where `src/serialize/` and `arbc_serialize`
come into being and where `nlohmann/json` first folds into `libarbc`. It is also
where doc 08's two softest promises become concrete and testable: "fixed number
formatting" (Principle 5, undefined in the doc until this task's delta) and
"serialize a pinned version … autosave never pauses editing" (doc 14's autosave
sequence, `14:230-231`) — the writer is the component that actually reads a
pinned `DocRoot` off the editing thread and turns it into diff-clean bytes.

## Inputs / context

### Design docs (normative, doc 16)

- `docs/design/08-serialization.md:21-46` — the **canonical JSON example**: the
  `{"arbc":{"format":1}}` envelope, the `composition` object (`working_space`
  *omitted = default*, `canvas` hint, `layers` bottom-to-top), and the per-layer
  fields. The structural target the writer emits.
- `08:47-57` — **Principle 1 (core owns placement; kinds own `params`)**: "The
  core reads/writes everything except `params`." Fixes the writer/kind split —
  the writer owns every field *except* `params`, which comes from the kind's
  `serialize()` hook (a `kind_params` deliverable).
- `08:75-88` — **Principle 5 (Determinism)**, **amended by this task** to define
  the concrete canonical rules: UTF-8-byte key order, JSON-library shortest
  round-trip numbers (int vs real by value type), non-finite values are a
  serialization error not a `null`. The heart of "documents diff cleanly."
- `08:47-58` — the schema note **added by this task's delta**: the core-owned
  temporal/audio placement keys (`span`, `time_map`, `gain`, `audible`), each
  omit-when-default. Extends the example (`08:29-42`) with the LayerRecord fields
  it predates.
- `08:58-64` — **Principle 2 (unknown kinds round-trip byte-equivalent modulo
  formatting)**: the writer's canonicalization *is* the "modulo formatting" that
  makes a re-emitted placeholder byte-stable — but the placeholder itself is
  `kind_params`. Context for why the number/key rules must be deterministic.
- `08:71-74` — **Principle 4 (Versioning)**: `arbc.format` is a *major* only;
  unknown fields preserved-and-ignored. Bears on Decision 5 (no format minor).
- `08:78-84` — **Principle 6 (operator graphs serialize structurally)**: `inputs`
  arrays and the `contents`/`$ref` table — explicitly **out of scope** here,
  scoped to `serialize.sharing`.
- `docs/design/14-data-model-and-editing.md:35-39` — **the pin mechanism**: "A
  pinned `DocState` *is* the doc-02 revision fence … an autosave [pins] a version
  and read[s] it without locks, at any pace, while editing proceeds." The exact
  property the writer relies on.
- `14:22-25` — immutable versions with structural sharing; editing publishes the
  next version atomically, never mutating a pinned one.
- `14:159-162` — a pinned version pins **content state** too (frozen pixels while
  the user paints) — the snapshot the writer sees is fully consistent.
- `14:230-231` — the **autosave sequence**: "pin, serialize (doc 08) on a
  background thread, unpin. Canonical output + a consistent version = safe,
  diff-friendly, pause-free." The writer's usage pattern verbatim.
- `14:263-264` — "Doc 08 serialization reads a pinned version; load constructs a
  fresh document at version 0" — confirms the writer↔reader split (reader owns
  version 0; the writer snapshots any revision).

### Source seams

- `CMakeLists.txt:107-131` — the landed `nlohmann/json` wiring: `include(FetchContent)`
  (`:117`), `FetchContent_Declare(nlohmann_json …)` at `GIT_TAG v3.11.3` (`:119-123`)
  with `FIND_PACKAGE_ARGS 3` (`:123`), `FetchContent_MakeAvailable` (`:124`), and
  SYSTEM-include re-marking (`:125-131`). The linkable target is
  `nlohmann_json::nlohmann_json` (its sole current consumer is
  `tests/CMakeLists.txt:22-26`).
- `cmake/ArbcComponent.cmake:14-40` — `arbc_add_component(NAME … SOURCES …
  PUBLIC_HEADERS … [DEPENDS …])` builds the `arbc_<name>` OBJECT library + `arbc::<name>`
  alias; `:46-56` — `arbc_component_test(COMPONENT … SOURCES …)` builds `arbc_<name>_t`
  against the umbrella `arbc` + Catch2. Note: `arbc_add_component` exposes **no
  parameter for extra link libraries** (`:14-40`), so wiring the header-only JSON
  target requires an explicit `target_link_libraries(arbc_serialize PRIVATE
  nlohmann_json::nlohmann_json)` in `src/serialize/CMakeLists.txt` (Decision 3).
- `src/kind_crossfade/CMakeLists.txt` — the two-call component template
  (`arbc_add_component` + `arbc_component_test`) the writer's `CMakeLists.txt`
  mirrors; `src/CMakeLists.txt` must gain `add_subdirectory(serialize)` between
  the `contract`/`model` entries and `runtime` (which already lists `serialize`
  as an allowed dep), before `arbc_finalize_library()`.
- `scripts/check_levels.py:28` — `"serialize": {"contract", "model"}` is already
  in `ALLOWED`; the component must declare exactly `DEPENDS contract model` (or a
  subset) and include only headers within that closure (base, pool, media,
  surface). The external `nlohmann_json` target is not an `arbc_*` edge and is not
  validated by the checker.
- **Model read seams** (all `const` peek traversals, worker-thread-safe):
  `src/model/arbc/model/model.hpp:171` `Model::current() -> DocStatePtr`; `:109`
  `using DocStatePtr = std::shared_ptr<const DocRoot>`; `:37-107` `class DocRoot`
  (revision `:45`); `:83` `for_each_layer_in(composition, fn)` (true bottom-to-top
  order — the accessor to use); `:49-51` `find_composition/find_layer/find_content`;
  `:89` `record_edge`; `:98` `content_state`. **Writer-only seams the worker must
  NOT touch**: `drain`, `navigate`, `transact`, any `Transaction`, `StateRefSink`,
  the `Journal` (`model.hpp:122-127,179,216`).
- **Record shapes** in `src/model/arbc/model/records.hpp`: `CompositionRecord`
  (`:136-144` — `canvas_w/h`, `working_space` `SurfaceFormat`, `working_audio_format`
  `AudioFormat`, ordered `layers` inline-then-spill); `LayerRecord` (`:68-92` —
  `content` ObjectId, `transform` `Affine`, `opacity`, `gain`, `flags`, `span`
  `TimeRange`, `time_map` `TimeMap`; `visible()`/`audible()` at `:90-91`);
  `ContentRecord` (`:60-63` — `kind` `std::uint64_t` + inert `StateHandle`); flag
  bits `k_layer_visible`/`k_layer_audible` (`:41-46`). `LayerRecord` has **no
  `name` field** — the doc-08 `name` metadata (`08:35`) is not yet a model field,
  so the writer does not emit it.
- **Value types**: `src/base/arbc/base/transform.hpp:12-18` — `Affine{a,b,c,d,tx,ty}`
  (six doubles) → `[a,b,c,d,tx,ty]`; `src/base/arbc/base/time.hpp:10-53` — `Time`
  is `int64 flicks` (`:12`, 705,600,000/s), `TimeRange{start,end}` with
  `TimeRange::all()` the omit-default (`:38-41`); `TimeMap` the parent→content-local
  1D affine. `src/media/arbc/media/surface_format.hpp:26-42` — `SurfaceFormat`
  (enum members) with `k_working_rgba32f` the omit-default.
- `src/base/arbc/base/expected.hpp` — `arbc::expected<T,E>` / `unexpected<E>`, the
  errors-as-values type (level 0, in serialize's closure). The writer returns
  `expected<…, E>` from its public entry point; no `nlohmann` exception escapes.
- `src/contract/arbc/contract/registry.hpp:42-61` — the registry keys on the
  reverse-DNS **string** id; there is **no bridge** from `ContentRecord.kind`
  (`uint64`) to that string yet (Decision 4). `src/kind_crossfade/arbc/kind_crossfade/crossfade_content.hpp:24-25`
  marks kind-param serialization deferred to `serialize.kind_params` / M7.

**Predecessor / sibling refinements:** [`serialize/json_dep.md`](json_dep.md)
(library + wiring settled; component creation deferred here — Decision 4),
[`model/journal.md`](../model/journal.md) (pin/version machinery),
[`contract/registry.md`](../contract/registry.md) (kind id is the persistent
token; `serialize`/`params` deferred to this stream).

## Constraints / requirements

1. **Serialize a pinned version, off the editing thread.** The writer pins
   `Model::current()` once, holds the `DocStatePtr` for the whole emit, and reads
   only through `DocRoot`'s `const` peek accessors (`model.hpp:83,49-51,89,98`).
   It must never call a writer-only seam (`drain`/`navigate`/`transact`/
   `Transaction`/`StateRefSink`/`Journal`) from a serialize worker. This is what
   makes "autosave never pauses editing" true (doc `14:35-39,230-231`).
2. **Canonical output (doc 08 Principle 5, as amended).** Keys in ascending UTF-8
   byte order (the default `nlohmann::json` `std::map` ordering — proven free by
   json_dep's smoke test); numbers via the library's platform-independent shortest
   round-trip serialization (locale-independent), integers for flicks/canvas
   extents and reals for placement scalars; non-finite doubles are a serialization
   **error value**, never emitted as `null`. Byte-identical output for a given
   pinned version across runs and re-serializations.
3. **Errors as values across the boundary (doc 10 `10:15-17`).** The public entry
   point returns `arbc::expected`; a malformed state (e.g. a non-finite placement
   scalar) yields an error value. No `nlohmann` exception crosses the API — use the
   non-throwing dump/serialize path proven by json_dep.
4. **Field scope: envelope + composition + core-owned layer placement only.**
   Emit `{"arbc":{"format":1}}`; the `composition` object with `canvas` (from
   `canvas_w/h`) and `working_space`/`working_audio_format` **omitted when default**
   (`08:25`); and each layer bottom-to-top (`for_each_layer_in`, `model.hpp:83`)
   with `transform`, `opacity`, `visible`, plus the omit-when-default twins `gain`,
   `audible`, `span`, `time_map`. Do **not** emit `kind`/`kind_version`/`params`
   (needs the kind hook + kind-id resolution → `kind_params`), operator `inputs`,
   or the `contents`/`$ref` table (→ `sharing`). Leave the per-layer content-body
   injection seam these tasks extend.
5. **Component levelization (doc 17 `17:58`, CI-enforced).** `arbc_serialize`
   `DEPENDS contract model` only; `scripts/check_levels.py` must stay green
   (`:28`). Link `nlohmann_json::nlohmann_json` **PRIVATE** — header-only, so it
   imposes no transitive link/system requirement on a `libarbc` embedder (doc 10's
   promise, `CMakeLists.txt:109-110`).
6. **Namespace/layout convention.** Public headers under
   `src/serialize/arbc/serialize/*.hpp` (namespace `arbc`), sources at
   `src/serialize/*.cpp`, component tests at `src/serialize/t/*.t.cpp`; the golden
   / concurrency tests follow the top-level `tests/` convention
   (operator-kind-conformance gotcha: backend/integration goldens live in `tests/`).
7. **Diff coverage ≥ 90%** (doc 16) on changed lines; the emission code and its
   error paths are exercised by the unit + golden + error tests below.

## Acceptance criteria

- **Byte-exact canonical golden.** A test builds a small known document through
  the model transaction API (a composition with a canvas, and layers exercising a
  non-identity `transform`, a non-unit `opacity`/`gain`, an invisible layer, a
  non-`all()` `span`, and a non-identity `time_map`), pins it, serializes it, and
  asserts the output equals an inline expected canonical `.arbc` string
  **byte-for-byte** (doc 16 byte-exact goldens; deterministic serialization work
  gets goldens, not tolerances). The golden demonstrates sorted keys, the
  fixed-number-formatting rule, and omit-when-default for the still-default twins.
  Tagged `enforces: 08-serialization#canonical-output-is-byte-stable`
  (**new claims-register entry**: *the canonical serialization of a given pinned
  document version is byte-identical across runs and across re-serialization —
  sorted keys, fixed number formatting, defaults omitted*).
- **Pinned-version fidelity.** A test pins `model.current()`, commits several
  further transactions on the model, then serializes the held pin and asserts the
  bytes reflect the **pinned** revision, not the later edits. Tagged
  `enforces: 08-serialization#writer-serializes-the-pinned-version` (**new
  claims-register entry**: *serializing a held `DocStatePtr` emits that revision's
  object graph, unaffected by transactions committed after the pin*). This is the
  serialize-output face of the existing model claim
  `14-data-model-and-editing#pinned-version-never-observes-later-edit` (which this
  test does not re-enforce).
- **Concurrency safety (TSan).** A stress test serializes a held pin on a worker
  thread while the writer thread commits N transactions in a loop, asserting a
  clean **ThreadSanitizer** run and that the serialized bytes equal the pinned
  revision's expected golden (behavioral, never a wall-clock assertion). Pins
  Constraint 1 — the writer reads a pinned `DocRoot` concurrently with the single
  mutator with no data race (doc `14:35-39`).
- **Errors as values.** A unit test drives a serialization whose input carries a
  non-finite placement scalar and asserts the entry point returns an
  `arbc::expected` **error value** with no exception thrown/escaping (Constraint 3,
  doc `10:15-17`).
- **Build both ways + levelization + WBS gate green.** `arbc_serialize` builds,
  links `nlohmann_json::nlohmann_json` PRIVATE, and `-Werror -Wpedantic` stays
  green; a clean configure fetches nlohmann and a system-package configure finds
  it; `scripts/check_levels.py` passes; the full build + test suite pass;
  `tj3 project.tjp 2>&1 | grep -iE "error|warning"` is silent after the `.tji`
  `complete 100` + refinement back-link land.
- **Design-doc delta (same commit).** `docs/design/08-serialization.md` Principle 5
  is amended with the concrete canonical rules, and the schema section gains the
  core-owned temporal/audio placement keys — both already written by this
  refinement; the closer commits them with the code (doc 16 same-commit).
- **Deferred, with concrete homes (no new WBS leaves needed):** content-body
  emission (`kind`/`kind_version`/`params` + the `uint64`→reverse-DNS kind-id
  resolution + unknown-kind placeholder) is already scoped to
  `serialize.kind_params` (`tasks/60-serialize.tji:24-29`); operator `inputs` and
  the `contents`/`$ref` table to `serialize.sharing` (`:30-35`); broadened
  determinism goldens + loader fuzzing to `serialize.format_tests` (`:36-41`).
  All three are existing WBS leaves depending transitively on this task — nothing
  new to register, no parking-lot item.

## Decisions

1. **Field scope stops at core-owned placement; the content body is `kind_params`.**
   The writer emits the envelope, composition, and per-layer spatial + temporal +
   audio placement, but not `kind`/`kind_version`/`params`. Rationale: the content
   body needs two seams that do not exist yet — the kind's `serialize()` hook
   (doc `08:50-57`; `crossfade_content.hpp:24-25` marks it deferred to
   `kind_params`/M7) and a resolution from `ContentRecord.kind` (`uint64`,
   `records.hpp:61`) to the registry's reverse-DNS **string** (`registry.hpp:57-61`
   keys on the string; **no bridge exists**). Both are squarely `kind_params`'s
   deliverable, which `depends !reader !writer`. Splitting the canonical engine +
   snapshot walk (this task) from the content body (`kind_params`) matches the WBS
   chain and keeps each task ~2d. *Rejected — emit a raw `uint64` kind now:* it is
   not the format's contractual token (the reverse-DNS string is, `08:30`), would
   bake a runtime-internal id into on-disk bytes, and would be thrown away by
   `kind_params` — data churn for no gain. *Rejected — build the `uint64`→string
   bridge here:* that is precisely the seam `kind_params` owns; standing it up here
   would split one concern across two tasks.

2. **Emit all core-owned `LayerRecord` placement, omit-when-default — including the
   temporal/audio twins.** `span`, `time_map`, `gain`, `audible` are core-owned
   placement (`records.hpp:68-92`), not `params`; the still/identity defaults
   (`TimeRange::all()`, identity `time_map`, `gain==1`, `audible` set) are the
   overwhelmingly common case and are omitted, so the still-layer golden stays
   diff-clean while a non-still layer round-trips losslessly. Rationale: omitting
   them entirely would be silent data loss with **no downstream home** (no serialize
   task covers temporal placement), violating doc 08 Principle 2's "never destroy
   data"; they are cheap (int64 flicks + a small affine + a double/bool). This is
   the reuse-the-model-fields, lossless call. Recorded as a doc-08 delta (the schema
   example predates these LayerRecord fields). *Rejected — defer the twins to a new
   `serialize.temporal_placement` leaf:* an extra task for a handful of trivial
   fields already in the record the writer walks, and a data-loss window until it
   lands. *Rejected — emit them always (no omit):* every still layer would carry
   four redundant keys, noising up diffs against Principle 5's intent.

3. **Wire `nlohmann_json` PRIVATE via an explicit `target_link_libraries` after
   `arbc_add_component`.** `arbc_add_component` has no extra-link parameter
   (`ArbcComponent.cmake:14-40`), so `src/serialize/CMakeLists.txt` calls
   `target_link_libraries(arbc_serialize PRIVATE nlohmann_json::nlohmann_json)`
   after it. PRIVATE + header-only keeps the dependency off `libarbc`'s public
   interface — no transitive link/system requirement on an embedder (doc 10,
   `CMakeLists.txt:109-110`). *Rejected — add a `LINK`/`EXTERN` parameter to
   `arbc_add_component`:* premature generalization of the shared helper for a
   single call site; a follow-up component that needs it can generalize then.
   *Rejected — link PUBLIC/INTERFACE:* would re-export the include requirement onto
   every `arbc` consumer, breaking the header-only isolation json_dep chose it for.

4. **The pinned `DocRoot` is read only through its `const` peek accessors, on any
   thread.** The writer treats itself as one of doc 14's "everyone else reads
   pinned versions" (`14:50-56`) — a single `model.current()` load, held for the
   emit, walked via `for_each_layer_in`/`find_*`/`content_state` (`model.hpp`), and
   never touching a writer-only seam. Rationale: this is the exact property doc 14
   guarantees (`14:35-39`) and the audio-lookahead gotcha warns about (nested cache
   reads on workers are the *unsafe* path; the `DocRoot` peek path is the *safe*
   one). Pinned via TSan (Acceptance). *Rejected — snapshot-copy the document into
   a private structure first:* defeats the whole zero-copy, structural-sharing
   design (`14:22-25,200-205`) and doubles memory for a slow autosave.

5. **`arbc.format` is a major only — no format minor is emitted.** The WBS note
   says "major/minor" loosely, but doc 08 defines only a major (`arbc.format`,
   `08:71-74`); within a major, unknown fields are preserved-and-ignored, which is
   the forward-compat mechanism a minor would otherwise serve. The writer emits
   `{"arbc":{"format":1}}` with no minor field. Rationale: adding a minor now would
   invent format surface doc 08 deliberately omits. *Rejected — emit a minor:*
   unsupported by the doc and redundant with preserve-and-ignore.

6. **No doc-00 decision-record bullet.** The two doc-08 deltas (concrete number
   formatting; the placement-key enumeration) are format-detail decisions within
   doc 08's own scope — the same reasoning json_dep Decision 5 applied to its
   library-slot detail. doc 00 is for project-shaping decisions; these are not.
   *Rejected — add a doc-00 bullet:* would inflate the decision record with detail
   doc 08 already owns.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-09.

- Created `src/serialize/` component (`arbc_serialize`) with `CMakeLists.txt` linking `nlohmann_json::nlohmann_json` PRIVATE; public header at `src/serialize/arbc/serialize/writer.hpp`; implementation at `src/serialize/writer.cpp`; component unit tests at `src/serialize/t/writer.t.cpp`.
- Added `add_subdirectory(serialize)` to `src/CMakeLists.txt` between `model` and `runtime`.
- Extended `tests/CMakeLists.txt` with two acceptance test executables (`serialize_writer_golden` and `serialize_writer_concurrency`); sources at `tests/serialize_writer_golden.t.cpp` and `tests/serialize_writer_concurrency.t.cpp`.
- Registered two new claims in `tests/claims/registry.tsv`: `08-serialization#canonical-output-is-byte-stable` and `08-serialization#writer-serializes-the-pinned-version`.
- Added two read/write seams to `src/model/arbc/model/model.hpp` + `src/model/model.cpp`: `DocRoot::find_first_composition` (composition-discovery peek) and `Transaction::set_visible` (twin of `set_audible`, needed for invisible-layer test fixture).
- Golden test (`tests/serialize_writer_golden.t.cpp`) asserts byte-exact canonical output + re-serialization stability (claim `08-serialization#canonical-output-is-byte-stable`) and pinned-version fidelity (claim `08-serialization#writer-serializes-the-pinned-version`).
- Concurrency test (`tests/serialize_writer_concurrency.t.cpp`) validates TSan pin-read-vs-committing-writer safety, also enforcing the pinned-version claim.
- Unit tests in `src/serialize/t/writer.t.cpp` cover errors-as-values for non-finite scalars, bare-envelope with no composition, and non-default `working_space`/`working_audio_format` emission.
- Doc 08 Principle 5 amended with concrete canonical rules and schema section extended with core-owned temporal/audio placement keys (`docs/design/08-serialization.md`).
