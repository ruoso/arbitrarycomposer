# serialize.reader — Reader + LoadContext

## TaskJuggler entry

Back-link: `tasks/60-serialize.tji:19-24` (`task reader` inside `task serialize`).

> note "Load to a fresh document at version 0; LoadContext supplies base-URI
> resolution and async asset loading; external .arbc references dedup by
> resolved identity. Doc 08."

## Effort estimate

**2 days** (`tasks/60-serialize.tji:20`). The parse/validate/reconstruct
inverse of the canonical writer is mechanically bounded — the writer already
fixed the on-wire envelope this task consumes — but three pieces carry the
day-count: (1) standing up `LoadContext` as a brand-new Level-4 type
(base-URI resolution, the async-asset hook interface, the resolved-identity
dedup cache), (2) the version-0-baseline model seam so a load installs its
graph without a spurious undo step, and (3) the load→save round-trip and
unknown-major-rejection test surface.

## Inherited dependencies

**Settled (formal `depends`, already Done — 2026-07-09):**

- `serialize.writer` (`!writer`, `tasks/60-serialize.tji:22`) — fixed the
  canonical envelope this reader is the exact inverse of: `{"arbc":{"format":1}}`
  wrapper, the `composition` object with `working_space`/`canvas`/
  `working_audio_format`, and the bottom-to-top `layers` array carrying
  core-owned placement (`transform`, `opacity`, `visible`, `name`, `span`,
  `time_map`, `gain`, `audible`), each field **omitted at its still/identity
  default**. Component `arbc_serialize` lives at `src/serialize/`,
  `DEPENDS contract model` (`src/serialize/CMakeLists.txt:5`), with
  `nlohmann_json::nlohmann_json` linked **PRIVATE**
  (`src/serialize/CMakeLists.txt:7-11`). Writer entry point:
  `expected<std::string, SerializeError> serialize_document(const DocRoot&)`
  (`src/serialize/arbc/serialize/writer.hpp:45`); errors-as-values shape is
  `struct SerializeError { enum class Kind { NonFiniteValue }; ObjectId object; }`
  (`src/serialize/arbc/serialize/writer.hpp:15-23`).
- `serialize.json_dep` (transitive, via `!writer`) — ratified `nlohmann/json`
  (`GIT_TAG v3.11.3`, `FIND_PACKAGE_ARGS 3`) and the **non-throwing** parse
  discipline (`parse(input, /*cb*/nullptr, /*allow_exceptions*/false)`) this
  reader must use so no `nlohmann` exception crosses the API boundary.
- `contract.registry` (transitive, via `serialize`'s parent `depends`) —
  `Registry::factory(std::string_view id)` and
  `Registry::metadata(std::string_view id)` are the kind-name lookup seams a
  loader resolves against (`src/contract/arbc/contract/registry.hpp:61,64`).
- `model.journal` (transitive, via `serialize`'s parent `depends`) — the
  journal/version machinery the version-0-baseline decision constrains.

**No pending inherited dependencies** — every predecessor is Done.

**Downstream (this task unblocks):**

- `serialize.kind_params` (`!reader`, `tasks/60-serialize.tji:28`) — fills the
  content-body half of the read path (`kind`/`kind_version`/`params` →
  `Content` via the registry factory; unknown-kind placeholder preserving
  those verbatim; the `uint64` ↔ reverse-DNS kind-id bridge). This reader
  lands the `deserialize(json, LoadContext&)` **seam signature**;
  `kind_params` lands the per-kind bodies.
- `serialize.sharing` (`!kind_params`) — operator `inputs` arrays and the
  document-level `contents`/`$ref` table on the read side.
- `serialize.format_tests` (`!kind_params`) — the libFuzzer harness over the
  loader and the load→save determinism corpus; this reader's
  errors-as-values discipline is the precondition that makes fuzzing safe.
- `kinds.nested` (`org.arbc.nested`, `tasks/55-kinds.tji:38`) — the consumer
  that exercises `LoadContext`'s external-`.arbc` loading end-to-end; this
  reader provides the resolution + dedup **mechanism** it plugs into.

## What this task is

Stand up the deserialization face of `arbc_serialize`: turn a canonical
`.arbc` JSON string back into a live model document. Concretely:

(a) A public reader entry point that **parses** the JSON envelope with the
non-throwing `nlohmann` overload, **validates** the `arbc.format` major and
rejects unknown majors, and **reconstructs** the composition tree with its
core-owned placement — the exact inverse of what `serialize_document` emits
today, supplying each field's still/identity default where the writer omitted
the key.

(b) A brand-new `LoadContext` type (Level-4, owned by `arbc::serialize` per
`docs/design/17-internal-components.md:58`) that supplies **base-URI
resolution** (v1: relative paths resolved against the document's base URI,
with the scheme hook left for later per doc 08 Principle 3), an **async
asset-loading hook interface** kinds call through, and a **resolved-identity
dedup cache** so two references to one resolved URI share a single identity.

(c) The `deserialize(json, LoadContext&)` **hook signature** on the
registry/factory seam — defined here, its per-kind bodies deferred to
`serialize.kind_params`.

(d) A model seam so a load installs its reconstructed graph as the document's
**version-0 baseline with an empty journal** — an immediate undo after load
is a no-op, never an "undo into an empty document."

This task does **not** reconstruct content bodies (`kind`/`kind_version`/
`params` → `Content`), operator `inputs` arrays, or the `contents`/`$ref`
sharing table — those land in `serialize.kind_params` and `serialize.sharing`
respectively, matching exactly what the writer defers on the emit side.

## Why it needs to be done

The writer proved a document version can be pinned and emitted byte-stably,
but a serialization format that cannot be read back is half a format. Every
downstream serialize task — `kind_params`, `sharing`, `format_tests` — needs
the load path to exist before it can add to it: `kind_params` needs the
`deserialize(json, LoadContext&)` seam and the fresh-document target to write
content bodies into; `format_tests` needs a loader to fuzz and a load→save
round-trip to assert determinism against; `kinds.nested` needs `LoadContext`'s
resolution + dedup mechanism to load a child `.arbc` into a nested layer
without blocking the output thread (`docs/design/14-data-model-and-editing.md:213-217`).
The reader is the join point that makes persistence bidirectional and unblocks
the rest of the stream.

## Inputs / context

### Design docs (normative, doc 16)

- `docs/design/08-serialization.md:21-55` — the document JSON schema the reader
  parses: top-level `"arbc": {"format": 1}` envelope (line 23), the
  `composition` object (`working_space`, `canvas`, `working_audio_format`,
  bottom-to-top `layers`), per-layer `transform`/`opacity`/`visible`/`name`
  and `span`/`time_map`/`gain`/`audible` (lines 48-55), each **omitted at its
  still/identity default** — the reader must re-supply that default on absence.
- `docs/design/08-serialization.md:59-66` — Principle 1: the core owns
  placement and reads it directly; `params` is handed verbatim to the kind's
  factory via `deserialize(json, LoadContext&) -> Content*`. `LoadContext`
  supplies base-URI resolution and async asset loading (lines 64-66).
- `docs/design/08-serialization.md:67-73` — Principle 2: unknown kinds
  round-trip losslessly as placeholder content preserving `kind`/
  `kind_version`/`params` verbatim; "a missing plugin must never destroy
  data." (Placeholder body is `kind_params`; the reader must route to it, not
  drop the data.)
- `docs/design/08-serialization.md:74-79` — Principle 3: references, not
  embedding. Assets and nested projects are URIs resolved relative to the
  document; **cross-file sharing deduplicates through the loader by resolved
  identity** so doc-05 shared-content semantics survive persistence.
- `docs/design/08-serialization.md:80-83` — Principle 4: `arbc.format` is a
  **major** version; **readers reject majors they don't know**; within a known
  major, unknown *fields* are preserved-and-ignored.
- `docs/design/08-serialization.md:84-96` — Principle 5: determinism;
  non-finite values are a serialization error surfaced as a value at the API
  (doc 10 errors-as-values), never `null`. Governs the round-trip the reader
  closes.
- `docs/design/08-serialization.md:97-103` — Principle 6: `inputs` arrays and
  the `contents`/`$ref` table — **out of scope here**, deferred to
  `serialize.sharing`.
- `docs/design/14-data-model-and-editing.md:263-264` — the load-versioning
  norm: "Doc 08 serialization reads a pinned version; **load constructs a fresh
  document at version 0**" — a brand-new document whose journal/version counter
  starts at 0, no prior history carried in from the file.
- `docs/design/14-data-model-and-editing.md:40-43` — undo/redo is navigation
  over an append-only journal; "history itself is never mutated" — constrains
  how the version-0 baseline is installed (a baseline install, not a journal
  truncation).
- `docs/design/14-data-model-and-editing.md:74-83` — identity: 64-bit
  document-unique `ObjectId`; "persistent identity across save/load is only
  guaranteed where the format already references it (`$ref` targets)" — bounds
  what `LoadContext` dedup must preserve (resolved-URI identity, not per-object
  ObjectId identity).
- `docs/design/17-internal-components.md:58` — `arbc::serialize` (Level 4) owns
  `LoadContext` and `$ref` resolution; depends on `contract`, `model`, + the
  JSON dep. `scripts/check_levels.py:28` already permits
  `"serialize": {"contract", "model"}` — no levelization edit needed.

### Source seams

- `src/serialize/arbc/serialize/writer.hpp:15-45` — the writer's public shape
  the reader mirrors: `struct SerializeError { enum class Kind {...}; ObjectId; }`
  and `expected<std::string, SerializeError> serialize_document(const DocRoot&)`.
  New reader files sit beside it: `src/serialize/arbc/serialize/reader.hpp` and
  `.../load_context.hpp` (public, namespace `arbc`), `src/serialize/reader.cpp`,
  component test `src/serialize/t/reader.t.cpp`.
- `src/serialize/writer.cpp:194` — the writer's canonical dump
  (`root.dump(2, ' ', false, json::error_handler_t::replace)`); the reader
  parses with the mirror non-throwing overload
  `json::parse(input, nullptr, false)` so no exception escapes.
- `src/model/arbc/model/model.hpp` — the reconstruction target: `class DocRoot`
  (line 37, ctor line 39, `revision()` line 45), peek accessors `find_layer`/
  `find_content`/`find_composition` (lines 49-52), `find_first_composition`
  (line 60), `working_space()` (line 69), `working_audio_format()` (line 77),
  `for_each_layer` (line 83); `using DocStatePtr = std::shared_ptr<const DocRoot>`
  (line 117); `Model::current()` (line 179), `allocate` (line 181), the
  `transact`/`Transaction` API (~line 232), undo/redo publish (line 211).
- `src/model/model.cpp:511,523` — `Model::Model()` installs the empty HAMT root
  at `revision 0` (`std::make_shared<const DocRoot>(d_bundle, Ref<HamtNode>{}, 0)`).
  This is the fresh-document seam the reader populates; the version-0-baseline
  install extends it.
- `src/model/arbc/model/journal.hpp:50-92` — `class Journal` (`on_commit`,
  `undo`/`redo`, `entry_at`); relevant to the "empty journal after load"
  decision.
- `src/contract/arbc/contract/registry.hpp:33-64` — `ContentFactory` (line
  39-40) currently takes an opaque `ContentConfig = std::string_view` (line 35,
  noted line 33 as "a serialization format gives it structure later");
  `factory(id)`/`metadata(id)` (lines 61,64) are the by-name lookup. This
  reader introduces the structured `deserialize(json, LoadContext&)` shape the
  note at line 33 anticipates; `kind_params` fills its bodies.
- `src/base/arbc/base/expected.hpp:13,29` — `expected<T,E>`/`unexpected<E>`,
  the errors-as-values vocabulary the reader returns through.
- `tests/serialize_writer_golden.t.cpp:33-54,117,145` — the writer golden is an
  **inline raw-string literal `k_golden`** built from `build_golden(Model&)`
  via the transaction API (no on-disk `.arbc` fixtures). The reader's
  round-trip test reuses this exact canonical string as its load input.
- `tests/claims/registry.tsv:183-184` — the serialize-owned claim rows
  (`08-serialization#canonical-output-is-byte-stable`,
  `#writer-serializes-the-pinned-version`); the reader appends its `08-*` rows
  here. Verified by `scripts/check_claims.py`.

**Predecessor / sibling refinements:** `tasks/refinements/serialize/writer.md`
(canonical envelope, PRIVATE `nlohmann` link, `SerializeError` errors-as-values
pattern, the "load to a fresh document at version 0" note it flags forward for
this task, and the content-body/`inputs`/`$ref` deferrals this reader inherits
on the read side) and `tasks/refinements/serialize/json_dep.md` (the
non-throwing `parse(input, nullptr, false)` discipline and the explicit note
that `LoadContext`/`$ref` were left unimplemented for the `serialize.*`
stream). `tasks/refinements/contract/registry.md:315-319` records that
`deserialize(json, LoadContext&)` was deferred to "an L4 `serialize` type that
does not yet exist" — this task creates it.

## Constraints / requirements

1. **Exact inverse of the current writer envelope.** The reader reconstructs
   precisely what `serialize_document` emits today — envelope, composition
   (`working_space`/`canvas`/`working_audio_format`), and bottom-to-top layers
   with core-owned placement — and nothing more. Content bodies, `inputs`, and
   `contents`/`$ref` are out of scope (they are absent from today's output).
   The invariant that pins correctness is **load(writer_output) then
   serialize == writer_output, byte-identical.**

2. **Default-on-absence must match the writer's omit-on-default.** The writer
   omits every field at its still/identity default
   (`docs/design/08-serialization.md:48-55`). The reader must supply the
   identical default when a key is absent — same identity transform, opacity 1,
   visible true, still span, identity time map, unity gain, audible default —
   so an omit/restore pair is a fixed point. A mismatch here breaks the
   round-trip golden.

3. **No exceptions across the boundary (doc 10, doc 08 Principle 5).** Parse
   with `json::parse(input, nullptr, false)`; the public entry point returns
   `expected<…, ReaderError>` where `ReaderError` mirrors `SerializeError`'s
   shape (an enum `Kind` plus context — offending JSON path and/or `ObjectId`).
   No `nlohmann` exception may escape on any input, well-formed or hostile —
   this is the precondition `serialize.format_tests`' loader fuzzing relies on.

4. **Unknown format major is a clean rejection, not a partial load.** A
   document whose `arbc.format` is a major the reader does not know returns
   `ReaderError::UnknownFormatMajor` and produces **no** document mutation
   (`docs/design/08-serialization.md:80-83`). Unknown *fields* within a known
   major are preserved-and-ignored (the preserve half rides with `kind_params`;
   the reader must not error on them).

5. **Load installs a version-0 baseline with an empty journal.** The
   reconstructed graph becomes the document's baseline at `revision() == 0`
   with no journal entries, so a subsequent undo is a no-op and never reverts a
   freshly-loaded document to empty
   (`docs/design/14-data-model-and-editing.md:263-264,40-43`). The reader adds
   a small `model` seam for this (see Decision 3), following the precedent of
   the writer adding `find_first_composition`/`set_visible`.

6. **`LoadContext` is the single resolution/loading choke point.** Base-URI
   resolution (relative paths in v1; scheme hook stubbed for later), the async
   asset-loading hook interface, and a resolved-identity dedup cache all live
   on `LoadContext` so kinds never invent their own
   (`docs/design/08-serialization.md:64-66,74-79`). Dedup is keyed on
   **resolved URI identity**, matching doc 14's "identity guaranteed only where
   the format references it."

7. **Levelization (doc 17).** Public headers under
   `src/serialize/arbc/serialize/`, namespace `arbc` (no nested namespace,
   matching the writer). `arbc_serialize` may reach only `contract` and `model`
   (+ the PRIVATE JSON dep); `scripts/check_levels.py:28` already permits this —
   no CI-graph edit. The reader does not add any `arbc_*` edge.

8. **Diff coverage ≥ 90%** on changed lines (doc 16), and the WBS gate
   `tj3 project.tjp 2>&1 | grep -iE "error|warning"` is silent after the
   `complete 100` + refinement back-link land (the closer's step).

## Acceptance criteria

- **Load→save canonical round-trip (golden-backed).** A new cross-component
  test `tests/serialize_reader_roundtrip.t.cpp` loads the writer golden string
  `k_golden` (`tests/serialize_writer_golden.t.cpp:54`), re-serializes the
  loaded document via `serialize_document`, and asserts the output is
  **byte-identical** to `k_golden`. Lands a new claim
  `08-serialization#load-save-round-trips-canonically` in
  `tests/claims/registry.tsv` with an `enforces:`-tagged test — the observable
  proof the reader is the exact inverse of the writer for core placement.

- **Version-0 baseline / empty journal.** A test asserts that immediately after
  a successful load, `Model::current()->revision() == 0` and an `undo()` is a
  no-op that leaves the loaded graph intact (does not empty the document).
  Lands claim `08-serialization#load-installs-version-0-baseline` with an
  `enforces:`-tagged test.

- **Unknown-format-major rejection.** A test feeds a document with
  `{"arbc":{"format":<unknown major>}}` and asserts the entry point returns
  `ReaderError::UnknownFormatMajor` with the target `Model` **unmutated**
  (still `revision()==0`, empty). Lands claim
  `08-serialization#reader-rejects-unknown-format-major` with an
  `enforces:`-tagged test.

- **`LoadContext` resolves + dedups by resolved identity.** A component test
  `src/serialize/t/reader.t.cpp` resolves the same relative reference twice
  against one base URI and asserts one cache entry / a single shared resolved
  identity, and that a distinct reference resolves distinctly. Lands claim
  `08-serialization#loadcontext-dedups-by-resolved-identity` with an
  `enforces:`-tagged test. (End-to-end cross-file `.arbc` sharing — two parents
  referencing one child — is exercised downstream by `kinds.nested` +
  `serialize.format_tests`, which own the multi-document corpus; this criterion
  pins the mechanism the reader provides.)

- **Errors-as-values on malformed input (no exception escapes).** A component
  test asserts that malformed JSON, a missing required field, and a
  non-object envelope each return a distinct `ReaderError::Kind` value with no
  `nlohmann` exception thrown and no document mutation. (The systematic
  libFuzzer harness over the loader is `serialize.format_tests`, an existing
  leaf; this criterion is the unit-level safety net that makes fuzzing sound.)

- **Coverage / WBS gate.** ≥ 90% diff coverage on the changed lines; the closer
  confirms `tj3` silence after landing `complete 100` and the refinement
  back-link on `tasks/60-serialize.tji:19-24`.

- **Deferred, on existing leaves (no new WBS tasks).** Content-body
  reconstruction and the unknown-kind placeholder go to `serialize.kind_params`
  (`!reader`); operator `inputs` + `contents`/`$ref` reads go to
  `serialize.sharing`; the libFuzzer loader harness + determinism corpus go to
  `serialize.format_tests`; end-to-end external-`.arbc` dedup goes to
  `kinds.nested`. All four are already WBS leaves under the `serialize`/`kinds`
  streams — the closer registers **no** new task for these deferrals.

## Decisions

1. **Reader entry point takes a fresh `Model&` and populates it, rather than
   returning a new `Model`.** Signature:
   `expected<std::monostate, ReaderError> load_document(std::string_view json,
   const Registry& registry, LoadContext& ctx, Model& into)` in
   `src/serialize/arbc/serialize/reader.hpp`. The reader is a *loader*; doc 17
   places `Document` (arenas + model + registry + loaders) in `runtime`
   (`docs/design/17-internal-components.md:60`), so the reader must not own
   arena/registry/Model lifecycle. Taking a caller-constructed `Model` (which
   `Model::Model()` already brings up at `revision 0`,
   `src/model/model.cpp:523`) keeps the serialize layer free of runtime
   concerns and mirrors the writer, which takes `const DocRoot&` and returns a
   value. *Rejected — returning `expected<Model, ReaderError>`:* forces the
   serialize component to construct and own arenas/model bundles, an upward
   levelization smell and a duplicate of runtime's `Document` assembly.

2. **`ReaderError` mirrors `SerializeError`'s errors-as-values shape.**
   `struct ReaderError { enum class Kind { MalformedJson, UnknownFormatMajor,
   MissingRequiredField, MalformedField, UnresolvableReference }; Kind kind;
   /* JSON path + optional ObjectId context */ };`, returned via
   `expected<…, ReaderError>`. This matches the writer
   (`src/serialize/arbc/serialize/writer.hpp:15-23`) and doc 10's
   errors-as-values, and gives the fuzz harness (`format_tests`) a typed
   surface to assert against. *Rejected — a single opaque error string:* loses
   the machine-checkable `Kind` the round-trip and rejection claims assert on,
   and the fuzz corpus classifies against.

3. **A load installs its graph as the version-0 baseline via a new
   `Model::load_baseline` seam, not via a normal `transact`.** The reader adds
   a small `model` seam that atomically publishes the reconstructed `DocRoot`
   as the document's baseline at `revision 0` with an **empty** journal
   (following the precedent of the writer adding `find_first_composition` and
   `Transaction::set_visible` from a serialize task). This realizes doc 14's
   "load constructs a fresh document at version 0 … no prior history carried in
   from the file" (`docs/design/14-data-model-and-editing.md:263-264`) as
   stated behavior — no design-doc delta is needed. *Rejected — reconstruct via
   a normal `transact`:* the loaded state would land at `revision 1` with a
   journal entry, so undo-after-load empties the document — a direct
   contradiction of "no prior history carried in from the file" and a bad edit
   semantic (opening a file is not an undoable step). *Rejected — transact then
   truncate the journal:* violates doc 14:40-43 "history itself is never
   mutated"; a clean baseline install is the honest primitive.

4. **`LoadContext` v1 resolves relative paths only, behind a scheme hook, and
   dedups on resolved-URI identity.** Base-URI resolution handles relative
   filesystem paths against the document's base URI; the scheme dispatch
   (`http`, content stores) is a stubbed hook per doc 08 Principle 3
   (`docs/design/08-serialization.md:74-79`). The async asset-loading member is
   an **interface** the reader defines and kinds call through — the raster and
   nested kinds exercise it downstream, so v1 lands the hook, not a filled-in
   async loader. Dedup is a cache keyed on resolved URI. *Rejected — resolving
   full URI schemes now:* doc 08 explicitly scopes v1 to relative paths and
   names the hook as the later extension point; building scheme handlers now is
   speculative. *Rejected — dedup on ObjectId:* doc 14:74-83 guarantees
   cross-save identity only where the format references it (`$ref` targets, a
   resolved-URI concept), so resolved-URI identity is the correct key.

5. **The `deserialize(json, LoadContext&)` hook is defined here as a signature
   seam; its per-kind bodies land in `serialize.kind_params`.** The reader
   establishes the structured shape the `ContentFactory`/`ContentConfig` note
   anticipates (`src/contract/arbc/contract/registry.hpp:33`) and routes
   layer content through it, but the actual `kind`/`kind_version`/`params` →
   `Content` reconstruction (and the unknown-kind placeholder) is
   `kind_params`, exactly mirroring the writer's emit-side deferral of the same
   content body. *Rejected — implementing content bodies here:* duplicates
   `serialize.kind_params`'s chartered scope and needs the `uint64` ↔
   reverse-DNS kind-id bridge that task owns; the writer emits no content body
   today, so there is nothing for this reader to read into content yet.

6. **No doc-00 decision-record bullet, no design-doc delta.** The reader
   implements behavior doc 08 (Principles 1–4) and doc 14:263-264 already
   settle; `LoadContext`'s ownership is already stated at doc 17:58. Nothing
   here is project-shaping or a deviation from stated behavior, so — consistent
   with `json_dep` Decision 5 and `writer` Decision 6 — no doc-00 bullet and no
   governing-doc edit is warranted. *Rejected — a clarifying doc-14 delta on
   "undo-after-load is a no-op":* that is the plain reading of "no prior history
   carried in from the file," an implementation of stated behavior, not an
   amendment; doc 16 asks for a delta only on deviation.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-09.

- Implemented `load_document` entry point in `src/serialize/arbc/serialize/reader.hpp` + `src/serialize/reader.cpp`; parses the canonical envelope via non-throwing `json::parse`, validates `arbc.format` major, reconstructs composition + bottom-to-top layers with defaults-on-absence.
- Landed `LoadContext` (base-URI resolution, async-asset hook interface, resolved-identity dedup cache) in `src/serialize/arbc/serialize/load_context.hpp` + `src/serialize/load_context.cpp`.
- Landed `deserialize.hpp` signature seam (`DeserializeFn`) for `kind_params` to fill per-kind bodies.
- Added `Model::load_baseline` seam in `src/model/arbc/model/model.hpp` + `src/model/model.cpp`; installs reconstructed graph as version-0 baseline with empty journal.
- Unit tests in `src/serialize/t/reader.t.cpp`: LoadContext dedup, async-hook forwarding, errors-as-values for malformed/missing-field/unknown-major inputs.
- Cross-component golden round-trip test in `tests/serialize_reader_roundtrip.t.cpp`: byte-exact load→save (incl. non-default `working_space`/audio + bare envelope), version-0 baseline/undo-no-op, unknown-major rejection.
- Registered 4 claims in `tests/claims/registry.tsv`: `08-serialization#{load-save-round-trips-canonically, load-installs-version-0-baseline, reader-rejects-unknown-format-major, loadcontext-dedups-by-resolved-identity}`, each `enforces:`-tagged.
- Build wiring: `src/serialize/CMakeLists.txt` + `tests/CMakeLists.txt` updated for new sources and test targets.
