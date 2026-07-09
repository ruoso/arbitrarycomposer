# runtime.document_serialize — Wire Document to the serialize content seams (the L5 load↔save integration)

## TaskJuggler entry

`tasks/65-runtime.tji:40-45` → `runtime.document_serialize` ("Wire Document to
serialize content seams"), the sixth leaf under `task runtime`. Its own edge is
`depends !threading, serialize.kind_params, model.content_binding`
(`65-runtime.tji:43`); the parent `task runtime` adds
`depends compositor.tile_planning` (`65-runtime.tji:6`) as an inherited edge.
Note line:

> "Implement the reader's ContentSink (loaded bodies populate d_contents),
> supply the writer's content-body provider from a pinned snapshot (incl.
> ContentRecord.kind uint64/reverse-DNS bridge and pinned-content-state
> serialization), register built-in kind codecs (solid/tone), and add an
> end-to-end full-document load/save byte-exact golden. Docs 08/17. Source:
> tasks/refinements/serialize/kind_params.md Decision 5."

This leaf was **registered ahead of implementation** by `serialize.kind_params`
Decision 5 (`tasks/refinements/serialize/kind_params.md:456-486`). It feeds
milestone **M8 (`m8_persistence`, "Documents as files")**, one of that
milestone's four dependencies (`tasks/99-milestones.tji:63-65`:
`serialize.format_tests, runtime.plugin_loading, model.workspace_backing,
runtime.document_serialize`). It is **not** guaranteed to be M8's last remaining
dependency — the closer adds `complete 100` to `m8_persistence` only if the
other three are already complete when this lands; otherwise it marks this leaf
alone. The closer appends `Refinement:
tasks/refinements/runtime/document_serialize.md` to this task's note.

## Effort estimate

**2d** (`tasks/65-runtime.tji:41`), the figure `kind_params` Decision 5 reserved.
The whole L4 machinery this task consumes is already Done and stable — the
`ContentSink`/`ContentBodyProvider`/`ContentMetaProvider` seams
(`src/serialize/arbc/serialize/reader.hpp:84`, `writer.hpp:74,93`), the
serialize-owned `CodecTable` and `content_body_from_json`/`content_body_to_json`
routing (`src/serialize/arbc/serialize/codec.hpp:55,88,101`), `PlaceholderContent`,
and the content-aware `load_document`/`serialize_document` overloads. What is
genuinely new is all L5 glue, none of it large:

1. a runtime-owned **`KindBridge`** (bijective reverse-DNS `kind_id` string ↔
   `std::uint64_t`, with a `kind_version` per entry) that realizes the
   `ContentRecord.kind` uint64 ↔ reverse-DNS bridge the model and writer deferred;
2. two **built-in codecs** — `org.arbc.solid` and `org.arbc.tone` — each a
   `SerializeFn`/`DeserializeFn` pair registered into a runtime-owned
   `CodecTable`, living in a runtime TU that alone can see both the concrete kind
   type and `nlohmann::json` (Decision 3 of `kind_params`);
3. the **`ContentSink`** implementation binding loaded contents into
   `Document::d_contents`, and the **content-body/meta providers** built from a
   pinned snapshot;
4. a thin **`save_document`/`load_document` façade** on the runtime side plus the
   **end-to-end byte-exact golden**.

The deliverable is one header/impl pair (`document_serialize.hpp`/`.cpp`), two
built-in codec TUs (`codec_solid.cpp`, `codec_tone.cpp`, nlohmann PRIVATE), a
cross-component golden + a TSan lane in `tests/`, two new claims, and the CMake
wiring. **No new `arbc_*` levelization edge** — `runtime` (L5) may already depend
on `serialize`/`kind_solid`/`kind_tone` (`scripts/check_levels.py:35-39`); only
the `DEPENDS` line and a PRIVATE nlohmann link are added. **No change to
`Document`'s class shape** (Decision 5): the interned kind id rides the existing
`add_content(content, kind)` parameter.

## Inherited dependencies

**Settled:**

- `runtime.threading` (DONE 2026-07-06, `65-runtime.tji:8-12`, this task's
  `!threading` edge) — established the single-writer / immutable-published-version
  discipline this task's save path relies on: the scene is read under a **pinned
  document version** so a snapshot never races edits and never takes a lock
  (`tasks/refinements/runtime/threading.md:276-281`). `Document::pin()`
  (`src/runtime/document.cpp:82`, returning a `DocStatePtr =
  std::shared_ptr<const DocRoot>`) is the primitive; a held pin is
  "unaffected by later transactions" (claim
  `14-data-model-and-editing#pinned-version-never-observes-later-edit`,
  `tests/claims/registry.tsv:4`). Also fixed that the runtime content side-map
  (`d_contents`) is **writer-thread-owned** (`document.hpp:81`) — the constraint
  that shapes this task's save-off-the-writer-thread capture (Decision 6).
- `serialize.kind_params` (DONE 2026-07-09, `60-serialize.tji:26-31`, this task's
  `serialize.kind_params` edge) — landed every L4 content seam this task fills:
  - **`ContentSink`** `= std::function<SunkContent(std::unique_ptr<Content>)>`
    (`reader.hpp:84`), `SunkContent { ObjectId id; Content* live; }`
    (`reader.hpp:71`), and the content-aware `load_document(json, registry,
    codecs, ctx, sink, into)` overload (`reader.hpp:98-100`). The header states
    outright that `runtime.document_serialize` implements this sink against
    `Document`'s content map (`reader.hpp:78-83`).
  - **`ContentBodyProvider`** `= std::function<std::optional<ContentBody>(ObjectId)>`
    with `ContentBody { std::string_view kind, kind_version; const Content& content; }`
    (`writer.hpp:64,74`), and the content-aware
    `serialize_document(doc, provider, meta, codecs)` overload
    (`writer.hpp:111-113`). The no-provider overload (`writer.hpp:56`) stays
    byte-identical to today's goldens (`kind_params` Constraint 6).
  - the serialize-owned **`CodecTable`** (`codec.hpp:55`, `add(kind_id, Codec)`
    `:59`, `find(kind_id)` `:63`), `SerializeFn = std::function<expected<
    nlohmann::json, SerializeError>(const Content&)>` (`codec.hpp:40`),
    `DeserializeFn` (`deserialize.hpp:36`), and the node routing
    `content_body_from_json` (`codec.hpp:88`) / `content_body_to_json`
    (`codec.hpp:101`).
  - `PlaceholderContent` (`placeholder_content.hpp:36`) — the unknown-kind
    fall-through this task's load path exercises but never constructs itself.
  - the doc-08 Principle 1 delta (codecs are serialize-owned, keyed by kind id,
    registered from L5/plugins) that makes "built-in codecs live in `runtime`"
    the sanctioned design.
- `model.content_binding` (DONE 2026-07-09, this task's `model.content_binding`
  edge) — made `Document::add_content(content, kind)` mint a **versioned
  `ContentRecord`** in a pinnable `DocState` (`document.cpp:7-18`), so a pin taken
  after an add exposes the record's opaque `{kind, StateHandle}`
  (`records.hpp:60-63`). It froze `add_content`'s `kind` uint64 as an
  **opaque caller-supplied token** whose "reverse-DNS ↔ numeric bridge is
  `runtime.document_serialize`'s" (`document.hpp:28-30`), and left the
  `StateHandle` **inert** (`k_state_none`, `records.hpp:37,51-56`) — no editable
  state is captured yet (`model.editable_facet` is a downstream seam), which is
  why this task's "pinned-content-state serialization" reads params from the live
  immutable `Content` via the codec, not from a captured handle (Decision 2).
- `serialize.reader`/`serialize.writer`/`serialize.sharing` (DONE 2026-07-09,
  transitive through `serialize.kind_params`) — `reader` landed `LoadContext`
  (`load_context.hpp:49`) and the `Model::load_baseline` version-0 install
  (`model.hpp:418`); `writer` landed the canonical pinned-snapshot emission
  engine and byte-stable formatting; `sharing` **extended `SunkContent` to carry
  the live `Content*`** and **added the `const Content&`-keyed
  `ContentMetaProvider`** (`writer.hpp:93`) — the extended seam shapes this task
  consumes. `sharing` also interprets `inputs`/`contents`/`$ref` (operator
  graphs); **this task registers no operator codecs** (see *Not this task* and
  Decision 7).

**Pending:** none — every predecessor is landed.

## What this task is

Make a `runtime::Document` round-trip through the `.arbc` format **with its
content bodies intact**, for the built-in leaf kinds `org.arbc.solid` and
`org.arbc.tone`. The L4 stream made a document's *content* round-trip in the
abstract (a test-registered codec, `kind_params`); this task connects that
machinery to the real host object and the real built-in kinds. Concretely:

(a) A runtime-owned **`KindBridge`**: a bijective map between a kind's reverse-DNS
`kind_id` string (the format's persistent token,
`03-layer-plugin-interface#registry-resolves-kind-id-to-factory-and-metadata`,
`registry.tsv:184`) and the opaque `std::uint64_t` that `ContentRecord.kind`
stores, carrying each kind's producer `kind_version`. Built-in kinds are
pre-interned; `intern(kind_id, kind_version) -> uint64` assigns on first sight;
`lookup(uint64) -> {kind_id, kind_version}` is the reverse the save path reads.
It owns stable string storage so the `string_view`s handed to
`ContentBody`/`ContentMeta` outlive the serialize call (`writer.hpp:60,90`).

(b) Two **built-in codecs** registered into a runtime-owned `CodecTable`:
`org.arbc.solid` (encodes/decodes `SolidContent`'s `Rgba`,
`src/kind_solid/arbc/kind_solid/solid_content.hpp:20,30`) and `org.arbc.tone`
(encodes/decodes `ToneContent`'s `{frequency_hz, amplitude}`,
`src/kind_tone/arbc/kind_tone/tone_content.hpp:18,23,37`). Each is a
`SerializeFn`/`DeserializeFn` pair in a runtime TU that includes the internal
`codec.hpp` and links `nlohmann::json` PRIVATE — the only layer that legally sees
both the concrete kind type (L4 `kind-solid`/`kind-tone`) and the JSON library
(via L4 `serialize`), per `kind_params` Decision 3.

(c) The **read path**: a `load_document` façade that constructs a
`LoadContext`, the built-in `CodecTable`, and a **`ContentSink`** closure, then
calls the content-aware serialize `load_document` (`reader.hpp:98-100`). The sink
takes ownership of each reconstructed `Content`, calls
`Document::add_content(std::move(content), kind_uint64)` (which mints the
`ContentRecord` and binds `d_contents`), and returns
`SunkContent{ record_id, live_ptr }`. The kind uint64 the sink stamps into the
record is the `KindBridge`-interned id of the content's kind — supplied through a
**per-load session** the built-in codecs' own `DeserializeFn` wrappers populate
(Decision 4), so the Done `ContentSink` signature is untouched.

(d) The **write path**: a `save_document` façade that pins the current version,
captures (on the writer thread) an immutable snapshot of the content bindings and
their interned kinds, then calls the content-aware `serialize_document`
(`writer.hpp:111-113`) with a **`ContentBodyProvider`** (`ObjectId` →
`ContentBody{kind, kind_version, content&}`, resolving the kind from the **pinned**
`ContentRecord.kind` uint64 through the bridge) and a **`ContentMetaProvider`**
(`const Content&` → `{kind, kind_version}`, over the captured snapshot). An
unknown-kind `PlaceholderContent` — reachable only in a load→save of a
foreign-kind file — self-describes; the provider's advisory kind for it is ignored
by the writer.

(e) The **end-to-end byte-exact golden**: build a `Document` with a solid and a
tone content on placed layers, `save_document` → canonical bytes compared to an
inline golden; then `load_document` those bytes into a fresh `Document` and
`save_document` again → byte-identical. Plus a TSan lane proving a save captured
on the writer thread runs to completion off-thread against ongoing edits without
observing a torn read (Decision 6).

**Not this task:**

- **Operator-graph codecs** (`org.arbc.fade`, `org.arbc.crossfade`,
  `org.arbc.nested`) and the operator-graph load→save golden. `serialize.sharing`
  Decision 5 proposed *widening this leaf* to cover them, but the WBS leaf's note
  and `depends` were not widened (no edge to `serialize.sharing` or the operator
  kinds), and folding three more codecs plus the input-child sink-ownership /
  `Content*`↔kind reverse-map machinery would overrun 2d and cross dependency
  edges this leaf does not declare. They are registered as the follow-up leaf
  **`runtime.operator_codecs`** (Decision 7; closer registers in WBS).
- **Captured editable state.** `StateHandle` is inert (`records.hpp:37`); there is
  no per-content state to serialize until `model.editable_facet` fills it. This
  task serializes params from the live immutable content via the codec; true
  captured-state serialization is that downstream seam's concern (Decision 2).
- **Plugin/out-of-tree codecs** (registered in a plugin TU, `kind_params`
  Decision 3) and the **`ARBC_PLUGIN_PATH` loader** (`runtime.plugin_loading`).
- **Interpreting `inputs`/`contents`/`$ref`** — that is L4, already Done in
  `serialize.sharing`; this task only supplies the runtime side of the seams it
  extended, exercised for leaves.

## Why it needs to be done

Everything below `runtime` can serialize a document's *skeleton* and, in the
abstract, its *content* — but the two halves have never met over a real host
object. `Document`'s `add_content` mints a `ContentRecord` whose `kind` is an
opaque `0`-defaulted token nobody bridges to a name; the writer's provider seam
has no runtime supplier; the reader's `ContentSink` has no implementation. Until
this task, saving a `Document` emits layers with **no content body** (the
no-provider overload), so a solid layer reloads as an empty frame. This is the
leaf that first makes a built-in-kind document a **file you can save and reopen**
— the chartered promise of milestone M8. It is also where doc 08's
pinned-version fidelity (`08-serialization#writer-serializes-the-pinned-version`)
and the `ContentRecord.kind` ↔ reverse-DNS bridge that `model.content_binding`
and `serialize.writer` both deferred first become concrete, tested behavior.

## Inputs / context

### Design docs (normative, doc 16)

- `docs/design/08-serialization.md`:
  - **Principle 1** (`:59-74`, as amended by `kind_params`) — the core owns
    placement, the kind owns `params`; the (de)serialize hooks are
    **serialize-owned codecs keyed by kind id**, and "concrete per-kind codecs
    are registered from a layer that can see both the kind's concrete type and
    the JSON library — `runtime` (L5) for built-in kinds". This task is that L5
    registration for solid/tone.
  - **Principle 2** (`:67-81`) — unknown kinds round-trip losslessly; "a missing
    plugin **must never** destroy data." Exercised here in a load→save of a
    foreign-kind body through `PlaceholderContent`.
  - **Principle 5** (`:84-104`) — canonical, byte-stable output; the definition of
    "byte-exact" the golden asserts.
- `docs/design/17-internal-components.md`:
  - levelization table (`:46-61`): `runtime` is **L5**, owns "`Document` (arenas +
    model + registry + loaders)" and "depends everything below" (`:60`);
    `serialize` is **L4** (`:58`); `kind-solid`/`kind-tone` are **L4** (`:59`).
    Runtime is the only in-lib layer that can see a concrete kind type + the JSON
    library at once — the structural reason built-in codec registration lives here.
  - `:66-72` — the model stays free of the `Content` vtable; the id→`Content`
    binding lives in `runtime`. This task honors it: no vtable, no JSON, and no
    kind-string knowledge leaks below L5.

### Source seams

- `src/runtime/arbc/runtime/document.hpp:21-83` / `src/runtime/document.cpp:7-87`
  — `Document`: `add_content(std::shared_ptr<Content>, std::uint64_t kind = 0)`
  (`document.hpp:31`, mints the `ContentRecord`, binds `d_contents`,
  `document.cpp:7-18`); `resolve(ObjectId) -> Content*` (`:84-87`); `pin() ->
  DocStatePtr` (`:82`); `d_contents` (`document.hpp:82`, writer-thread-owned).
  **The reverse-DNS↔numeric bridge is explicitly this task's**
  (`document.hpp:28-30`).
- `src/serialize/arbc/serialize/reader.hpp:71,84,98-100` — `SunkContent`,
  `ContentSink`, content-aware `load_document`.
- `src/serialize/arbc/serialize/writer.hpp:23,64,74,82,93,111-113` —
  `SerializeError` (`NonFiniteValue`/`NoCodec`/`CodecFailed`), `ContentBody`,
  `ContentBodyProvider`, `ContentMeta`, `ContentMetaProvider`, content-aware
  `serialize_document`.
- `src/serialize/arbc/serialize/codec.hpp:40,45,55-63,88,101` — `SerializeFn`,
  `Codec`, `CodecTable::add`/`find`, `content_body_from_json`,
  `content_body_to_json`; `src/serialize/arbc/serialize/deserialize.hpp:36` —
  `DeserializeFn` (receives `params`, built input `ContentRef`s, and `LoadContext&`).
- `src/serialize/arbc/serialize/load_context.hpp:49-87` — `LoadContext` (ctor
  `explicit LoadContext(std::string base_uri)`), the resolution/asset choke point
  every `DeserializeFn` receives.
- `src/model/arbc/model/records.hpp:37,51-56,60-63` — `ContentRecord {
  std::uint64_t kind; StateHandle state; }`, inert `StateHandle`/`k_state_none`.
- `src/model/arbc/model/model.hpp:50,60,91,117,179,418` —
  `DocRoot::find_content`, `find_first_composition`, `for_each_layer_in`,
  `DocStatePtr`, `Model::current`, `Model::load_baseline` (the reader's install
  path; a load lands at revision 0 with an empty journal so undo-after-load is a
  no-op).
- `src/contract/arbc/contract/registry.hpp:51-70` — `Registry` (reverse-DNS
  `factory(id)`/`metadata(id)`/`ids()`), passed to the reader as the
  plugin-present witness.
- `src/kind_solid/arbc/kind_solid/solid_content.hpp:20,30` — `SolidContent`,
  `kind_id = "org.arbc.solid"`, its `Rgba{r,g,b,a}` params;
  `src/kind_tone/arbc/kind_tone/tone_content.hpp:18,23,37` — `ToneContent`,
  `kind_id = "org.arbc.tone"`, ctor `(std::uint32_t frequency_hz, float
  amplitude)`.
- `src/runtime/CMakeLists.txt` — `DEPENDS base model contract compositor pool
  cache audio_engine`; **must add `serialize kind_solid kind_tone`**, and the
  built-in-codec TUs must `target_link_libraries(... PRIVATE
  nlohmann_json::nlohmann_json)` (mirroring `src/serialize/CMakeLists.txt:12`).
- `scripts/check_levels.py:35-39` — `runtime`'s allow-list already includes
  `serialize`, `kind_solid`, `kind_tone`; **no graph edit**.

### Tests / claims

- `tests/serialize_writer_golden.t.cpp` (inline `k_golden` raw-string, `build_golden`
  over the model API), `tests/serialize_reader_roundtrip.t.cpp`,
  `tests/serialize_kind_params.t.cpp` (the `GadgetContent`/test-codec pattern to
  mirror for solid/tone) — the golden/round-trip conventions: **inline raw-string
  goldens, no on-disk `.arbc` fixtures, byte-exact `CHECK`, not tolerances**.
- `tests/serialize_writer_concurrency.t.cpp` — the TSan lane pattern (save vs.
  writer mutation) to mirror for the side-map capture.
- `tests/claims/registry.tsv` — serialize block `:184-196`; relevant existing
  rows: `08-serialization#canonical-output-is-byte-stable` (`:185`),
  `#writer-serializes-the-pinned-version` (`:186`),
  `#load-save-round-trips-canonically` (`:187`),
  `#load-installs-version-0-baseline` (`:188`),
  `#unknown-kind-round-trips-verbatim` (`:191`),
  `#known-kind-params-round-trip` (`:192`); and
  `14-data-model-and-editing#pinned-version-never-observes-later-edit` (`:4`).
  New rows append after the serialize block.

### Predecessor / sibling refinements

`tasks/refinements/serialize/kind_params.md` (Decision 5 — this leaf's charter),
`serialize/reader.md` (`ContentSink`/`LoadContext`/`load_baseline`),
`serialize/writer.md` (provider seam + the uint64↔string bridge deferral),
`serialize/sharing.md` (Decision 3/5 — the extended `SunkContent`/`ContentMeta`
seams and the operator-graph widening this refinement re-routes to a follow-up),
`tasks/refinements/runtime/threading.md` (pin discipline, writer-thread-owned
side-map).

## Constraints / requirements

1. **The content body is `{kind, kind_version, params}`; the core routing owns
   `kind`/`kind_version`, the codec owns `params`.** Built-in codecs encode/decode
   only their kind's `params` (solid: `Rgba`; tone: `{frequency_hz, amplitude}`),
   using the fuzz-hardened non-throwing accessors and never letting an
   `nlohmann::json` exception escape (`kind_params` Constraint 5, doc 08 P5,
   doc 10).

2. **Kind identity survives the round-trip through the uint64 bridge.** A layer's
   content saved as `org.arbc.solid` reloads and re-saves as `org.arbc.solid`,
   byte-for-byte. On save the kind string is resolved from the **pinned**
   `ContentRecord.kind` uint64 via `KindBridge::lookup` — so the serialized kind
   reflects the pinned version, not a later edit
   (`08-serialization#writer-serializes-the-pinned-version`).

3. **`kind_version` is a per-built-in constant chosen and pinned by this task.**
   The built-in kinds declare no version today; registration supplies each a fixed
   producer `kind_version` (documented, golden-pinned). It is advisory metadata,
   not golden-determining beyond its own literal.

4. **`Document`'s public shape is unchanged; the interned id rides the existing
   `add_content` `kind` parameter.** No new `Document` member, no records.hpp
   change. The load session that threads the kind from codec to sink is a
   transient runtime structure discarded after the load (Decision 4).

5. **The no-provider writer output and every existing serialize golden are
   unchanged.** This task only *adds* a runtime caller of the content-aware
   overload; `tests/serialize_writer_golden.t.cpp` and
   `tests/serialize_reader_roundtrip.t.cpp` must not regress.

6. **Unknown-kind bodies (a foreign-kind file loaded then re-saved) still
   round-trip losslessly.** The sink stamps a reserved "unknown" uint64 for a
   `PlaceholderContent`; on save the writer re-emits the placeholder's stored body
   verbatim, ignoring the provider's advisory kind (doc 08 P2).

7. **Levelization / no new edge / no JSON below L5.** All new code is in
   `arbc::runtime`; `nlohmann::json` appears only in the built-in-codec TUs (PRIVATE
   link), never in a runtime public header. `scripts/check_levels.py` stays green
   with no graph edit; the `Content` interface and the model gain nothing.

8. **Save reads the writer-thread-owned content side-map safely.** The provider
   closures read only immutable data (a pinned `DocRoot` + a snapshot of content
   bindings captured on the writer thread); the actual `serialize_document` may run
   off the writer thread while editing continues, without a lock and without a torn
   `d_contents` read (Decision 6, mirroring `writer.md` D4's
   "autosave-never-pauses-editing").

9. **Diff coverage ≥ 90%** on changed lines (doc 16); `-Werror -Wpedantic` and
   `scripts/check_levels.py` green; the WBS gate `tj3 project.tjp 2>&1 | grep -iE
   "error|warning"` silent after the closer lands `complete 100` + the refinement
   back-link.

## Acceptance criteria

- **Full-document content round-trip, byte-exact (golden-backed).** A new
  cross-component test (`tests/document_serialize_golden.t.cpp`) builds a
  `Document` with an `org.arbc.solid` content and an `org.arbc.tone` content on
  placed, attached layers; `save_document(doc, bridge, codecs)` produces bytes
  compared **byte-for-byte** to an inline canonical `k_golden`; then
  `load_document` reloads those bytes into a fresh `Document` and `save_document`
  re-emits **byte-identical** output. Lands claim
  `08-serialization#document-content-round-trips-byte-exact` in
  `tests/claims/registry.tsv` with an `enforces:`-tagged test — the observable
  proof that a built-in-kind document is a file you can save and reopen. This
  test additionally re-enforces `08-serialization#writer-serializes-the-pinned-version`
  (`registry.tsv:186`) via a second `// enforces:` tag (the provider reads the
  pinned `ContentRecord.kind`).

- **Built-in solid/tone codec params round-trip (behavioral, per-kind).** A
  runtime component test (`src/runtime/t/document_serialize.t.cpp`) round-trips
  each built-in through its registered codec: a `SolidContent{Rgba}` and a
  `ToneContent{freq, amp}` are serialized to `params` JSON and back, asserting the
  reconstructed content's fields equal the originals and that dispatch selects the
  codec (not `PlaceholderContent`). Lands claim
  `08-serialization#builtin-solid-tone-codecs-round-trip` with an `enforces:`-tagged
  test.

- **`ContentSink` binds into the content map with the bridged kind.** The
  component test asserts that after `load_document`, each layer's content resolves
  through `Document::resolve` to a live built-in content (**non-`ObjectId{}`**
  binding), and that the layer's pinned `ContentRecord.kind`
  (`DocRoot::find_content`) equals `KindBridge::intern(kind_id, …)` for that kind —
  the bridge is bijective across the round-trip.

- **Pinned-snapshot fidelity (behavioral).** The test pins, saves to bytes, then
  mutates the `Document` (e.g. adds another content / layer) **after** the pin, and
  asserts the pinned save's bytes are unchanged — re-enforcing
  `14-data-model-and-editing#pinned-version-never-observes-later-edit`
  (`registry.tsv:4`) through the save path (second `// enforces:` tag, no new row).

- **Unknown-kind file survives a Document load→save (behavioral).** A test loads a
  document whose layer carries a foreign kind (`{"kind":"com.example.ghost",…}`)
  into a `Document`, re-saves, and asserts the foreign body re-emits **verbatim
  canonical** (via the sink's reserved-unknown id + the placeholder's stored body).
  Re-enforces `08-serialization#unknown-kind-round-trips-verbatim` (`registry.tsv:191`)
  through the L5 path (second tag, no new row).

- **Concurrency — save captured on writer thread, emitted off-thread (TSan).** A
  focused test under `tsan` (`tests/document_serialize_concurrency.t.cpp`) has the
  writer thread churn `add_content`/mutations while a background thread runs the
  captured-snapshot `serialize_document`; assertions are **outcomes only** — the
  emitted bytes equal a synchronous save of the same pinned revision, and TSan is
  clean. Never a wall-clock assertion (doc 16).

- **Errors-as-values across the façade.** A malformed built-in `params` (e.g. a
  non-object `params`, an out-of-range tone frequency) returns a `ReaderError`
  value with the `Document`/`Model` left unmutated (revision 0); a codec that
  cannot serialize returns a `SerializeError` (`CodecFailed`/`NoCodec`) — no
  `nlohmann` exception, no partial mutation.

- **Coverage / build / WBS gate.** ≥ 90% diff coverage; `-Werror -Wpedantic` and
  `scripts/check_levels.py` green (runtime `DEPENDS` gains `serialize kind_solid
  kind_tone`, nlohmann PRIVATE in the codec TUs, no graph edit); the closer
  confirms `tj3` silence after landing `complete 100` and the refinement back-link
  on `tasks/65-runtime.tji:40-45`, and adds `complete 100` to `m8_persistence`
  only if this is its last incomplete dependency.

- **Deferred — one new WBS leaf.** The operator-graph codecs + golden are the only
  registration the closer makes: **`runtime.operator_codecs`** (Decision 7),
  wired into milestone **M8** (`m8_persistence`, `tasks/99-milestones.tji:63-65`).
  No other follow-up.

## Decisions

1. **A runtime-owned `KindBridge` realizes the `ContentRecord.kind` uint64 ↔
   reverse-DNS bridge; the built-in kinds are pre-interned.** `Document::add_content`
   stores an opaque `uint64` (`records.hpp:61`); the format's contractual identity
   is the reverse-DNS string (`registry.tsv:184`, doc 08:30). The bridge is the one
   place that maps between them, plus a `kind_version` per entry, plus stable string
   storage so the `string_view`s in `ContentBody`/`ContentMeta` outlive the
   serialize call (`writer.hpp:60,90`). Assignment is monotonic on first sight;
   built-ins are pre-interned at bridge construction. *Rejected — key
   `ContentRecord.kind` by a stable hash of the string (FNV-1a) with no table:* a
   hash is not invertible, so `lookup(uint64) -> string` still needs a table — the
   hash buys nothing and adds collision risk. *Rejected — put the bridge in
   `contract::Registry`:* the Registry is L2 and cannot hold runtime-assigned
   numeric ids; and the bridge is a runtime concern (the uint64 is a runtime
   side-map token, `writer.md` Constraint 4). Because the uint64 is **never
   serialized** (only the string is), its concrete values do not affect goldens —
   byte-stability rests solely on the deterministic string, so assignment order is
   free.

2. **Params serialize from the live immutable content, not from a captured
   `StateHandle` — "pinned-content-state serialization" for v1.** `model.content_binding`
   left `StateHandle` inert (`records.hpp:37`); there is no captured editable state
   to serialize until `model.editable_facet` fills it. v1 content objects are
   immutable once added, so the codec reading `params` off the live `Content` (via
   `SerializeFn(const Content&)`) is snapshot-consistent with the pinned version.
   The "pinned snapshot" the task supplies from is the pinned `DocState` (layers +
   `ContentRecord`s), captured via `Document::pin()`. *Rejected — block on
   `model.editable_facet` to serialize captured state:* that seam is a separate,
   later concern; the params round-trip is fully defined without it, and coupling
   would stall M8. When editable state lands, extending the codec to read a
   resolved `StateHandle` is an additive change, not a rework.

3. **Built-in codecs live in runtime TUs that link `nlohmann::json` PRIVATE.** A
   concrete solid/tone codec must see both the concrete kind type (L4
   `kind-solid`/`kind-tone`) and the private JSON type (via L4 `serialize`); only
   `runtime` (L5) can (`kind_params` Decision 3, doc 17:60). The codec bodies go in
   `codec_solid.cpp`/`codec_tone.cpp`, which `#include <arbc/serialize/codec.hpp>`
   (the internal header) and link nlohmann PRIVATE; the runtime *public* headers
   name no JSON type. *Rejected — codecs in `kind-solid`/`kind-tone`:* those are L4
   peers of `serialize`, cannot depend on it, and must not name the private JSON
   type. *Rejected — codecs in `serialize`:* an L4→L4 edge to the kind components,
   which `check_levels.py` rejects — the very reason the codec table is an
   L5-populated seam.

4. **The loaded content's kind reaches the sink through a per-load session, not by
   changing the `ContentSink` signature.** The Done `ContentSink =
   std::function<SunkContent(std::unique_ptr<Content>)>` (`reader.hpp:84`) hands the
   sink only the `Content`, but the sink must stamp the right kind uint64 into the
   `ContentRecord`. Runtime owns **both** the `CodecTable` and the sink, so the
   built-in codecs' `DeserializeFn` bodies are registered as thin wrappers that,
   after constructing the content, record `content.get() -> kind_id` into a
   transient per-load session map; the sink reads that map to intern the kind before
   `add_content`. A `PlaceholderContent` (no registered codec, so no wrapper runs)
   is absent from the map → the sink stamps a reserved "unknown" uint64.
   *Rejected — widen `ContentSink` to carry the kind string:* it touches a Done L4
   public seam that `serialize.sharing` also depends on, forcing a serialize API
   change for information runtime can thread itself; the wrapper keeps the seam
   frozen and the JSON/kind knowledge entirely at L5. *Rejected — a second parse of
   the JSON in runtime to recover kinds by layer order:* runtime cannot name
   nlohmann in public code, and order-correlating a re-parse to sunk ids is fragile.

5. **No new `Document` member; the interned uint64 rides `add_content`.** The kind
   the sink interns is passed straight to the existing
   `add_content(content, kind)` (`document.hpp:31`), which already threads it into
   the versioned `ContentRecord`. On save, the provider reads that same uint64 back
   off the pinned record. The per-load session map (Decision 4) and the save-time
   capture (Decision 6) are transient locals of the `load_document`/`save_document`
   façades, not `Document` state. *Rejected — add a `Content*`→kind map to
   `Document`:* unnecessary — the durable copy already lives in the pinned
   `ContentRecord`, and a persistent live map would duplicate it and grow the
   writer-thread-owned surface for no round-trip benefit (leaf kinds have no input
   children needing a live `Content*`→kind lookup; that need is the
   `runtime.operator_codecs` follow-up's).

6. **Save captures an immutable content-binding snapshot on the writer thread, then
   emits off-thread against only immutable data.** `d_contents` is
   writer-thread-owned (`document.hpp:81`); reading it concurrently with
   `add_content` is a data race. `save_document` therefore, on the writer thread,
   pins the `DocState` and copies the needed `ObjectId`→`Content*` (and the derived
   `const Content&`→`{kind, kind_version}` view) into a cheap immutable structure;
   the provider/meta closures and `serialize_document` then run off-thread over only
   the pinned `DocRoot` (const peek) + that captured copy — no lock, matching
   `writer.md` D4's "autosave never pauses editing." A TSan test pins the guarantee.
   *Rejected — hold a lock over `d_contents` for the whole save:* stalls the writer
   thread for the duration of a serialization, contradicting the autosave promise.
   *Rejected — declare save writer-thread-synchronous only:* needlessly forfeits
   background autosave the cheap capture already affords.

7. **Operator-graph codecs are a dedicated follow-up leaf, not a widening of this
   one.** `serialize.sharing` Decision 5 proposed widening `runtime.document_serialize`
   to also register `fade`/`crossfade`/`nested` codecs and add an operator-graph
   golden, "no new leaf." But this WBS leaf's `note` and `depends`
   (`65-runtime.tji:43`) were never widened — it has no edge to `serialize.sharing`
   or the operator kinds — and the operator work carries genuinely larger machinery
   (input-child sink ownership for nodes with **no `ObjectId`** per `sharing`
   Decision 3, the live `Content*`→kind reverse map behind the `ContentMetaProvider`,
   three codecs, a second golden) that would overrun 2d and cross undeclared edges.
   Registering a dedicated leaf keeps the dependency graph honest — the same
   reasoning `kind_params` Decision 5 used to reject folding into `host_objects`.
   Crisp registration for the closer:
   - **id:** `runtime.operator_codecs`
   - **effort:** 2d
   - **description:** "Register the built-in operator-graph codecs
     (`org.arbc.fade`/`org.arbc.crossfade`/`org.arbc.nested`) against the extended
     `DeserializeFn` (built input `ContentRef`s), give the runtime `ContentSink`
     input-child ownership (nodes without an `ObjectId`) and the live
     `Content*`↔kind reverse map behind the `const Content&`-keyed
     `ContentMetaProvider`, and add an end-to-end operator-graph load→save byte-exact
     golden."
   - **depends:** `!document_serialize`, `serialize.sharing`, `operators.fade`,
     `operators.crossfade`, `kinds.nested`
   - **milestone:** M8 (`m8_persistence`, `tasks/99-milestones.tji:63-65`)
   - **refinement home:** `tasks/refinements/runtime/operator_codecs.md`
   This is concrete, agent-implementable wiring (three codecs, a sink-ownership
   change, a golden) — not an audit. *Rejected — fold into this leaf:* overruns 2d
   and needs edges this leaf does not declare. *Rejected — leave it unregistered
   (prose-only in the Status block):* Status notes are invisible to the pick-task
   pass; the operator round-trip is real work that must be a WBS leaf.

8. **No doc-00 decision-record bullet, no design-doc delta.** This task *implements*
   behavior docs 08 and 17 already settle (built-in codecs register from `runtime`;
   the model stays free of the vtable) — it relocates nothing and changes no
   designed behavior. It follows `writer` D6 / `reader` D6 / `kind_params` D7, which
   each declined a doc-00 bullet for L5/format wiring within a doc's existing scope.
   *Rejected — a doc-08 delta for the `KindBridge`:* the uint64↔string bridge is a
   runtime implementation detail doc 08 already anticipates ("registered from
   `runtime`"), not a format change.

## Open questions

(none — all decided.) The one live design-judgment item in the vicinity —
serialized operator **feedback cycles** (`sharing` Decision 8) — is parked at
`tasks/parking-lot.md` and is out of both this leaf's and `runtime.operator_codecs`'s
scope (v1 `$ref` graphs are acyclic DAGs).

## Status

**Done** — 2026-07-09.

- `src/runtime/arbc/runtime/document_serialize.hpp` + `src/runtime/document_serialize.cpp`: `load_document`/`save_document` façade with `KindBridge` (bijective reverse-DNS ↔ uint64 + stable string storage), `ContentSink` closure, and pinned-snapshot `ContentBodyProvider`/`ContentMetaProvider`.
- `src/runtime/arbc/runtime/builtin_codecs.hpp` (internal): declaration of built-in codec registration entry point.
- `src/runtime/codec_solid.cpp` + `src/runtime/codec_tone.cpp`: `SerializeFn`/`DeserializeFn` pairs for `org.arbc.solid` (`Rgba`) and `org.arbc.tone` (`frequency_hz`, `amplitude`), linking `nlohmann_json` PRIVATE.
- `src/runtime/t/document_serialize.t.cpp`: runtime component test — codec round-trip, sink/bridged-kind binding, unknown-kind verbatim, errors-as-values for both kinds.
- `tests/document_serialize_golden.t.cpp`: byte-exact inline golden + reload/resave; re-enforces `#writer-serializes-the-pinned-version` and `#pinned-version-never-observes-later-edit`.
- `tests/document_serialize_concurrency.t.cpp`: TSan lane (save-captured-off-writer-thread vs. ongoing edits); cannot build in GCC-14 env due to pre-existing `audio_engine/lookahead.cpp` `-Werror=tsan` issue (not a regression; existing tsan lanes fail identically).
- `tests/claims/registry.tsv`: +2 claims — `08-serialization#document-content-round-trips-byte-exact`, `#builtin-solid-tone-codecs-round-trip`.
- `src/runtime/CMakeLists.txt`: +sources, +`DEPENDS serialize kind_solid kind_tone`, nlohmann PRIVATE on codec TUs, +component test.
- `tests/CMakeLists.txt`: +2 targets (`document_serialize_golden`, `document_serialize_concurrency`), nlohmann PRIVATE.
- `src/runtime/arbc/runtime/document.hpp`: +`friend struct DocumentSerializeAccess` (no member/data/public-shape change).
- `src/kind_solid/arbc/kind_solid/solid_content.hpp`: +`color()` accessor.
- `src/kind_tone/arbc/kind_tone/tone_content.hpp` + `src/kind_tone/tone_content.cpp`: +`frequency_hz()`/`amplitude()` accessors.
