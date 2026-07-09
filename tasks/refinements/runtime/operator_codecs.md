# runtime.operator_codecs — Register the built-in operator-graph codecs (the L5 fade/crossfade round-trip)

## TaskJuggler entry

`tasks/65-runtime.tji:47-51` → `runtime.operator_codecs` ("Register built-in
operator-graph codecs"), the seventh leaf under `task runtime`. Its own edge is
`depends !document_serialize, serialize.sharing, operators.fade, operators.crossfade,
kinds.nested` (`65-runtime.tji:50`); the parent `task runtime` adds
`depends compositor.tile_planning` (`65-runtime.tji:6`) as an inherited edge.
Note line:

> "Register org.arbc.fade/crossfade/nested codecs against the extended
> DeserializeFn (build input ContentRefs), give the runtime ContentSink
> input-child ownership (nodes without an ObjectId) and the live
> Content-ptr-to-kind reverse map behind the const Content-ref-keyed
> ContentMetaProvider, and add an end-to-end operator-graph load-save byte-exact
> golden. Docs 08/17. Source: tasks/refinements/runtime/document_serialize.md
> Decision 7. Refinement: tasks/refinements/runtime/operator_codecs.md"

This leaf was **registered ahead of implementation** by `runtime.document_serialize`
Decision 7 (`tasks/refinements/runtime/document_serialize.md:531-560`), which split the
operator-graph codecs off the built-in-kind (solid/tone) work rather than widen that
leaf past 2d and across undeclared edges. It feeds milestone **M8
(`m8_persistence`, "Documents as files")** (`tasks/99-milestones.tji:63-65`). The
closer appends `Refinement: tasks/refinements/runtime/operator_codecs.md` to this
task's note (already present) and adds `complete 100` to `m8_persistence` only if
this leaf is its last incomplete dependency when it lands.

**Scope note surfaced up front (see Decision 1):** the WBS note names three codecs —
`fade`, `crossfade`, `nested`. Refinement investigation establishes that `fade` and
`crossfade` are operator-graph `inputs` kinds the `serialize.sharing` machinery fully
supports today, while **`org.arbc.nested` is structurally different** (a
composition reference, not an `inputs` edge) and its faithful serialization is blocked
on an undesigned document-format capability (in-document multi-composition addressing
or the cross-file composition loader) that is L4-serialize / format-level design work,
not a runtime codec. This leaf therefore delivers the **fade + crossfade** codecs and
the operator-graph golden — closing the operator-graph serialization gap — and surfaces
the nested-composition-serialization design question to `tasks/parking-lot.md` (Decision
1, Open questions).

## Effort estimate

**2d** (`tasks/65-runtime.tji:48`), the figure Decision 7 reserved. Everything the
fade/crossfade codecs consume is already Done and stable:

- the **operator-graph format** — `inputs` arrays, the document-level `contents`
  table, and `{"$ref": id}` deduplication — landed in `serialize.sharing`
  (`8a90cae`), including the document-level read/write recursion in
  `src/serialize/reader.cpp` and `src/serialize/writer.cpp` that walks
  `Content::inputs()`, hoists ≥2-referenced nodes, and resolves `$ref`;
- the **extended `DeserializeFn`** carrying a `std::span<const ContentRef> inputs`
  (`src/serialize/arbc/serialize/deserialize.hpp:36-37`) and the extended
  **`SunkContent { ObjectId id; Content* live; }`** / `ContentSink`
  (`src/serialize/arbc/serialize/reader.hpp:71-74,84`);
- the **`const Content&`-keyed `ContentMetaProvider`**
  (`src/serialize/arbc/serialize/writer.hpp:93`) for resolving an input child's
  `(kind, kind_version)`;
- the whole **L5 façade + `KindBridge`** — `load_document`/`save_document`, the
  bijective reverse-DNS ↔ uint64 bridge with stable string storage, the `ContentSink`
  closure, and the pinned-snapshot `ContentBodyProvider`/`ContentMetaProvider` — landed
  in `runtime.document_serialize` (`6d9e9e0`).

What is genuinely new is small and additive:

1. two **built-in operator codecs** — `org.arbc.fade` and `org.arbc.crossfade` — each a
   `SerializeFn` (`params` only) / `DeserializeFn` (adopts `inputs` at construction)
   pair registered into the runtime `CodecTable`, in runtime TUs that link
   `nlohmann::json` PRIVATE (`codec_fade.cpp`, `codec_crossfade.cpp`);
2. two **const param accessors** on the L4 kind headers (`FadeContent`,
   `CrossfadeContent`) so a codec can read back the immutable construction params
   (`FadeParams`/`CrossfadeParams`), mirroring the `color()`/`frequency_hz()`
   accessors `runtime.document_serialize` added to solid/tone;
3. **`ContentSink` input-child ownership** in the runtime sink closure — input children
   (nodes with **no `ObjectId`**) are owned by `Document` and populate `d_contents` but
   mint no `ContentRecord`;
4. the **live `Content*` → kind reverse map** behind the `ContentMetaProvider`, captured
   over the pinned snapshot by walking `Content::inputs()`, so the writer can resolve an
   input child's kind (the need `document_serialize` Decision 5 explicitly deferred to
   this leaf);
5. the **end-to-end operator-graph byte-exact golden** + a runtime component test + a
   TSan lane, two new claims, and the CMake wiring.

The deliverable is two built-in codec TUs, edits to the existing
`document_serialize.cpp` sink/meta closures, two L4 accessor additions, a
cross-component golden + a TSan lane in `tests/`, one runtime component test, two new
claims, and the CMake `DEPENDS`/test wiring. **No new `arbc_*` levelization edge** —
`runtime` (L5) already allows `kind_fade`, `kind_crossfade`, `kind_nested`, and
`serialize` (`scripts/check_levels.py:35-40`); only the `DEPENDS` line gains
`kind_fade kind_crossfade` and the new codec TUs link nlohmann PRIVATE. **No change to
`Document`'s public shape.**

## Inherited dependencies

**Settled:**

- `runtime.document_serialize` (DONE 2026-07-09, `65-runtime.tji:40-45`, this task's
  `!document_serialize` edge) — landed the L5 façade this leaf extends:
  `load_document`/`save_document`, the `KindBridge` (bijective reverse-DNS `kind_id`
  ↔ `std::uint64_t` + `kind_version` + stable string storage), the `ContentSink`
  closure binding sunk contents into `Document::d_contents`, and the pinned-snapshot
  `ContentBodyProvider`/`ContentMetaProvider`
  (`src/runtime/arbc/runtime/document_serialize.hpp`,
  `src/runtime/document_serialize.cpp`). It also established the per-load session that
  threads a content's `kind_id` from codec to sink (Decision 4 there), the
  `codec_solid.cpp`/`codec_tone.cpp` TU pattern (nlohmann PRIVATE), and the byte-exact
  inline-golden + TSan-lane test conventions this leaf mirrors. Its Decision 5 froze
  the boundary this leaf crosses: "leaf kinds have no input children needing a live
  `Content*`→kind lookup; that need is the `runtime.operator_codecs` follow-up's"
  (`document_serialize.md:511-515`).
- `serialize.sharing` (DONE 2026-07-09, `60-serialize.tji`, this task's
  `serialize.sharing` edge) — landed the operator-graph format and the document-level
  read/write recursion this leaf's codecs plug into:
  - the **`inputs` array** (core-owned, emitted beside `kind`/`kind_version`/`params`,
    slot *i* == `Content::inputs()` index *i*, order-significant, omitted when empty)
    and the **document-level `contents` table + `{"$ref": id}`** dedup for content
    referenced ≥2 times, keyed by deterministic first-encounter ordinal
    (`sharing.md:124-140`, doc 08 Principle 6);
  - the **extended `DeserializeFn`** `= expected<std::unique_ptr<Content>, ReaderError>(
    const nlohmann::json& params, std::span<const ContentRef> inputs, LoadContext& ctx)`
    (`deserialize.hpp:36-37`) — children built bottom-up first, live `Content*`s threaded
    down so a known operator adopts inputs at construction (`sharing` Decision 4);
  - the **`ContentSink`** extension returning `SunkContent { ObjectId id; Content* live; }`
    (`reader.hpp:71-74,84`) — only layer roots get an `ObjectId`; input children live as
    `Content*` alone, all nodes owned by the sink (`sharing` Decision 3);
  - the **`const Content&`-keyed `ContentMetaProvider`** (`writer.hpp:82-93`) — resolves
    an input child (a `Content&` with no `ObjectId`) to `(kind, kind_version)`; L5 supplies
    it from the reverse map (`sharing` Decision 3);
  - the `content_body_from_json`/`content_body_to_json` **single-node** codec seam
    (`codec.hpp:88-104`) — the `inputs` limb is graph-structural and re-derived on save
    from `Content::inputs()`, never stored in the opaque body;
  - **read-error semantics** — a dangling or cyclic `$ref` → `ReaderError::UnresolvableReference`,
    validated before any model mutation, so failure leaves `revision() == 0` and no
    nlohmann exception (`sharing` Decision 7, `reader.hpp:33`); v1 `$ref` graphs are
    acyclic DAGs (`sharing` Decision 8, doc 08 P6 tail).
  - claims already landed: `08-serialization#inputs-array-round-trips`
    (`registry.tsv:194`), `#shared-content-dedups-via-ref` (`:195`),
    `#dangling-ref-is-read-error` (`:196`), `#placeholder-renders-input-0-passthrough`
    (`:193`). This leaf **re-enforces** them through the concrete built-in operators.
- `operators.fade` (DONE, `50-operators.tji`, this task's `operators.fade` edge) —
  landed `FadeContent` (`org.arbc.fade`): a single-input operator with
  `FadeParams { FadeShape shape; std::optional<FadeWindow> in; std::optional<FadeWindow> out; }`,
  `FadeWindow { Time start; Time end; }`, `FadeShape::Linear` only (v1); ctor
  `FadeContent(ContentRef input, FadeParams params)`; `inputs()` returns its one edge
  (`src/kind_fade/arbc/kind_fade/fade_content.hpp:18-35,48,66,74,93-95`).
  Params are immutable construction state; the operator refinement scoped serialization
  out (its own Decision 8) — this leaf is that serialization.
- `operators.crossfade` (DONE, `50-operators.tji`, this task's `operators.crossfade`
  edge) — landed `CrossfadeContent` (`org.arbc.crossfade`): a two-input operator
  (`from` = slot 0, `to` = slot 1) with
  `CrossfadeParams { CrossfadeShape shape; Time start; Time duration; }`,
  `CrossfadeShape::Linear` only; ctor
  `CrossfadeContent(ContentRef from, ContentRef to, CrossfadeParams params)`; `inputs()`
  returns `{from, to}`
  (`src/kind_crossfade/arbc/kind_crossfade/crossfade_content.hpp:18-30,47,66,76,95-99`).
- `kinds.nested` (DONE 2026-07-06, `55-kinds.tji`, this task's `kinds.nested` edge) —
  landed `NestedContent` (`org.arbc.nested`): a **composition-reference** content
  wrapping an in-document child composition by model `ObjectId`; ctor
  `NestedContent(ObjectId child)`, accessor `child()`, `inputs()` exposing the child's
  resolved member-layer contents
  (`src/kind_nested/arbc/kind_nested/nested_content.hpp:52,57,113,115,185`). Its
  refinement never designed serialization of the child reference
  (`tasks/refinements/kinds/nested.md:385-393` covers only compositing/`inputs()`
  resolution). See Decision 1 for why its codec is out of this leaf's scope.

**Pending:** none — every predecessor is landed.

## What this task is

Make an operator graph built from the v1 built-in operators — `org.arbc.fade` and
`org.arbc.crossfade` — round-trip through the `.arbc` format with their **params and
input edges intact**, over the real `runtime::Document`. `serialize.sharing` made the
operator-graph *structure* (`inputs`/`contents`/`$ref`) round-trip in the abstract, with
a test-registered codec; `runtime.document_serialize` connected the leaf-kind machinery
(solid/tone) to the host object. This leaf connects the operator machinery to the real
built-in operators. Concretely:

(a) Two **built-in operator codecs** registered into the runtime `CodecTable` alongside
solid/tone. Each is a `SerializeFn` returning **only** the kind's `params` JSON, and a
`DeserializeFn` that reconstructs the operator from `params` + the already-built
`std::span<const ContentRef> inputs`:
- `org.arbc.fade` encodes/decodes `FadeParams { shape, in, out }` (with each optional
  `FadeWindow { start, end }`), and constructs `FadeContent(inputs[0], params)`;
- `org.arbc.crossfade` encodes/decodes `CrossfadeParams { shape, start, duration }` and
  constructs `CrossfadeContent(inputs[0], inputs[1], params)`.
Both live in runtime TUs (`codec_fade.cpp`, `codec_crossfade.cpp`) that `#include
<arbc/serialize/codec.hpp>` (the internal header) and link `nlohmann::json` PRIVATE —
the only layer that legally sees both the concrete kind type (L4 `kind-fade`/`kind-crossfade`)
and the JSON library (via L4 `serialize`), per `document_serialize` Decision 3. Scalar
params (`Time`, enum `shape`) serialize under doc 08 Principle 5's number discipline,
matching the existing scalar/time serialization precedent in `writer.cpp`.

(b) Two **const param accessors** added to the L4 kind headers so the codecs can read
back the immutable construction params — `FadeContent` and `CrossfadeContent` today
expose only the *evaluated* `envelope(Time)` / `position(Time)`, not the raw
`FadeParams`/`CrossfadeParams` (`fade_content.hpp:93`, `crossfade_content.hpp:95`, both
private with no getter). A `params()` const accessor (or `friend struct` bridge) exposes
the immutable struct for serialization without changing behavior — the same additive move
`document_serialize` made adding `color()`/`frequency_hz()`/`amplitude()` to solid/tone
(`document_serialize.md:592-593`).

(c) The **runtime `ContentSink` input-child ownership** extension. The sink closure in
`document_serialize.cpp` today handles layer roots (mint a `ContentRecord`, bind
`d_contents`, return `{record_id, live}`). This leaf extends it so an **input child**
(a node the recursion sinks that is *not* a layer root, hence has no `ObjectId`) is still
owned by `Document` and reachable for lifetime, but mints **no `ContentRecord`** and
returns `SunkContent{ ObjectId{}, live }`. The model stores no input edges
(`records.hpp`); the child is named by `Content*` alone, threaded into its parent
operator's `inputs` at construction.

(d) The **live `Content*` → kind reverse map** behind the `ContentMetaProvider`. On save,
the write recursion resolves each input child (a `Content&` with no `ObjectId`) to its
`(kind, kind_version)` through the `const Content&`-keyed `ContentMetaProvider`
(`writer.hpp:93`). This leaf builds that map during the writer-thread save capture by
walking `Content::inputs()` transitively over the pinned graph and interning each reached
content's kind through the `KindBridge` — the reverse lookup `document_serialize` Decision 5
deferred. A placeholder input child (unknown kind, no interned entry) resolves to
`nullopt`, and its stored body re-emits verbatim (doc 08 P2).

(e) The **end-to-end operator-graph byte-exact golden**: build a `Document` with an
operator graph exercising both codecs, a chain (a `fade` over a `crossfade`), and a
**shared** input content used ≥2 times (to fire `$ref` dedup); `save_document` → canonical
bytes compared to an inline golden; then `load_document` those bytes into a fresh
`Document` and `save_document` again → byte-identical. Plus a TSan lane proving the graph
save captured on the writer thread runs to completion off-thread against ongoing edits.

**Not this task:**

- **The `org.arbc.nested` codec and any nested-composition serialization** (Decision 1).
  Faithful nested round-trip is blocked on an undesigned document-format capability
  (addressing more than one in-document composition, or the cross-file composition
  loader); that is L4-serialize / format-level design work, not a runtime codec, and is
  surfaced to `tasks/parking-lot.md`. This leaf ships no lossy nested codec.
- **Non-linear operator shapes.** Fade and crossfade are `Linear`-only in v1
  (`fade.md` Decision 3, `crossfade.md` Decision 3); the codecs encode the `shape` enum
  literally, but no new shape is added.
- **Interpreting `inputs`/`contents`/`$ref`, the read/write graph recursion, and
  `$ref` cycle detection** — all L4, already Done in `serialize.sharing`. This leaf only
  supplies the runtime side (the two codecs, the sink input-child ownership, the reverse
  map) and exercises the L4 machinery end-to-end for the real operators.
- **Captured editable state / operator automation.** Params serialize from the live
  immutable operator via the codec (`document_serialize` Decision 2); `StateHandle` is
  inert until `model.editable_facet`.
- **Serialized operator feedback cycles** — parked (`sharing` Decision 8); v1 `$ref`
  graphs are acyclic DAGs and fade/crossfade are acyclic.

## Why it needs to be done

`serialize.sharing` built the operator-graph format and `runtime.document_serialize` made
a leaf-kind `Document` a file you can save and reopen — but no *built-in operator* has
ever round-tripped over the real host object. A document with a fade or a crossfade saves
today with **no operator codec registered**, so its operator contents fall through to
`PlaceholderContent` (the unknown-kind path) instead of reconstructing as live
`FadeContent`/`CrossfadeContent` — a fade layer reloads as a diagnostic placeholder, not a
fade. This leaf is where the operator-graph format (`inputs` arrays, `$ref` dedup,
input-child ownership) first becomes concrete, tested behavior for the built-in operators,
and where the live `Content*`→kind reverse map that `document_serialize` Decision 5
deferred first exists. It completes the operator-graph half of milestone M8's
save-and-reopen promise.

## Inputs / context

### Design docs (normative, doc 16)

- `docs/design/08-serialization.md`:
  - **Principle 1** (`:59-74`) — the core owns placement, the kind owns `params`; the
    hooks are **serialize-owned codecs keyed by kind id**, and "concrete per-kind codecs
    are registered from a layer that can see both the kind's concrete type and the JSON
    library — `runtime` (L5) for built-in kinds" (`:68-72`). This leaf is that L5
    registration for fade/crossfade.
  - **Principle 2** (`:75-81`) — unknown kinds round-trip losslessly; a missing plugin
    never destroys data. Exercised here when an operator's input child is a foreign kind
    (placeholder → `nullopt` meta → verbatim re-emit).
  - **Principle 5 "Determinism"** (`:92-104`) — sorted keys, shortest-round-trip decimal,
    integer core scalars as JSON integers, fractional as reals, non-finite → error value.
    The number discipline the operator `params` (`Time`, shape enum) obey and the golden
    asserts.
  - **Principle 6** (`:105-131`) — the operator-graph format: `inputs` array (order-
    significant, omitted when empty), the `contents` table + `{"$ref": id}` for content
    referenced ≥2 times, deterministic core-owned re-derived ordinals, and the acyclic-DAG
    / `$ref`-cycle-is-an-error rule. The format the fade/crossfade round-trip lives in.
- `docs/design/17-internal-components.md`:
  - levelization table (`:46-61`): `runtime` is **L5**, "depends everything below"
    (`:60`); `serialize` is **L4** with the JSON dep (`:58`); the operator/nested kinds
    `kind-fade`/`kind-crossfade`/`kind-nested` are **L4** under `arbc::kind-*`
    (`:59`), each depending only on `contract`. Runtime is the only in-lib layer that can
    see a concrete kind type + the JSON library at once (`:41` no same-level edges) — the
    structural reason operator-codec registration lives here.

### Source seams

- `src/runtime/arbc/runtime/document_serialize.hpp` / `src/runtime/document_serialize.cpp`
  — the L5 façade (`load_document`/`save_document`), `KindBridge`, the `ContentSink`
  closure (extended here for input-child ownership), and the pinned-snapshot
  `ContentBodyProvider`/`ContentMetaProvider` (the meta provider gains the live
  `Content*`→kind reverse map here).
- `src/serialize/arbc/serialize/deserialize.hpp:36-37` — the extended `DeserializeFn`
  (`const nlohmann::json& params, std::span<const ContentRef> inputs, LoadContext& ctx`).
- `src/serialize/arbc/serialize/reader.hpp:33,71-74,84,98-100` —
  `ReaderError::UnresolvableReference`, `SunkContent`, `ContentSink`, content-aware
  `load_document`.
- `src/serialize/arbc/serialize/writer.hpp:23-33,64-68,74,82-85,93,111-114` —
  `SerializeError`, `ContentBody`, `ContentBodyProvider`, `ContentMeta`, the
  `const Content&`-keyed `ContentMetaProvider`, content-aware `serialize_document`.
- `src/serialize/arbc/serialize/codec.hpp:40,45-48,55-70,88-104` — `SerializeFn`,
  `Codec`, `CodecTable::add`/`find`, the single-node `content_body_from_json`/
  `content_body_to_json` routing.
- `src/kind_fade/arbc/kind_fade/fade_content.hpp:18-35,48,66,74,93-95` — `FadeContent`,
  `kind_id = "org.arbc.fade"`, `FadeParams`/`FadeWindow`/`FadeShape`, ctor, `inputs()`;
  **`d_params` is private with no getter — this leaf adds the accessor.**
- `src/kind_crossfade/arbc/kind_crossfade/crossfade_content.hpp:18-30,47,66,76,95-99` —
  `CrossfadeContent`, `kind_id = "org.arbc.crossfade"`, `CrossfadeParams`, ctor,
  `inputs()`; **`d_params` private, no getter — this leaf adds the accessor.**
- `src/model/arbc/model/records.hpp` — `ContentRecord`/`LayerRecord` store **no input
  edges**; only layer roots bind an `ObjectId` (`sharing` Decision 3). Input children are
  `Content*`-only.
- `src/runtime/CMakeLists.txt:15` — `DEPENDS base model contract compositor pool cache
  audio_engine serialize kind_solid kind_tone`; **must add `kind_fade kind_crossfade`**,
  and `codec_fade.cpp`/`codec_crossfade.cpp` are covered by the existing
  `target_link_libraries(arbc_runtime PRIVATE nlohmann_json::nlohmann_json)` (`:22`).
- `scripts/check_levels.py:35-40` — `runtime`'s allow-list already includes `kind_fade`,
  `kind_crossfade`, `kind_nested`, `serialize`; **no graph edit**.

### Tests / claims

- `tests/document_serialize_golden.t.cpp`, `src/runtime/t/document_serialize.t.cpp`,
  `tests/document_serialize_concurrency.t.cpp` — the byte-exact inline-golden, runtime
  component-test, and TSan-lane patterns this leaf mirrors for the operator graph.
- `tests/serialize_sharing.t.cpp` — the operator-graph structure tests (test-codec
  operators) whose real-operator analogue this leaf lands.
- `tests/claims/registry.tsv` — serialize block `:184-198`; existing rows this leaf
  **re-enforces**: `08-serialization#inputs-array-round-trips` (`:194`),
  `#shared-content-dedups-via-ref` (`:195`), `#dangling-ref-is-read-error` (`:196`),
  `#unknown-kind-round-trips-verbatim` (`:191`), `#writer-serializes-the-pinned-version`
  (`:186`), and `14-data-model-and-editing#pinned-version-never-observes-later-edit`
  (`:4`). New rows append after `:198`.

### Predecessor / sibling refinements

`tasks/refinements/runtime/document_serialize.md` (Decision 7 — this leaf's charter; the
KindBridge/sink/provider seams and the deferred `Content*`→kind reverse map),
`tasks/refinements/serialize/sharing.md` (the operator-graph format, extended
`DeserializeFn`/`ContentSink`, `ContentMetaProvider`, `$ref` semantics),
`tasks/refinements/operators/fade.md`, `tasks/refinements/operators/crossfade.md` (the
operator params and input roles), `tasks/refinements/kinds/nested.md` (the nested
composition-reference model — see Decision 1).

## Constraints / requirements

1. **The operator body is `{kind, kind_version, params, inputs?}`; the codec owns only
   `params`.** The `SerializeFn` returns the kind's `params` JSON; the core routing owns
   `kind`/`kind_version` and the `inputs` limb (re-derived on save from
   `Content::inputs()`, never stored in the opaque body — `sharing` Decision 2). The
   codec never emits or parses the `inputs` array (`codec.hpp:88-104`).

2. **Input edges reconstruct through the extended `DeserializeFn`, adopted at
   construction.** The `DeserializeFn` receives the already-built input children as
   `std::span<const ContentRef> inputs` (built children-first by the L4 recursion) and
   constructs `FadeContent(inputs[0], params)` / `CrossfadeContent(inputs[0], inputs[1],
   params)`. Wrong arity (a fade body with 0 or ≥2 inputs; a crossfade with ≠2) is a
   `ReaderError` value — no exception, model unmutated.

3. **Params round-trip exactly, byte-for-byte.** A `fade`/`crossfade` saved with given
   `params` reloads and re-saves byte-identical. Scalar encoding follows doc 08 P5
   (integer core scalars as JSON integers, fractional as shortest-round-trip reals,
   non-finite → `SerializeError::NonFiniteValue`), matching the existing `Time`/scalar
   serialization precedent in `writer.cpp`; the `shape` enum serializes as a stable
   string literal (`"linear"`). Optional `FadeWindow`s absent in the source are omitted
   (omit-when-default discipline, doc 08 P6).

4. **`kind_version` is a per-operator constant chosen and pinned by this task**,
   golden-pinned, advisory beyond its own literal — as `document_serialize` Constraint 3
   set for solid/tone.

5. **Input children are owned by `Document` with no `ObjectId`; only layer roots mint a
   `ContentRecord`.** The extended sink returns `SunkContent{ ObjectId{}, live }` for a
   non-root node while keeping `Document` the owner (`sharing` Decision 3, `records.hpp`
   stores no input edges). No new `Document` member and no records.hpp change; ownership
   rides the existing content-map machinery.

6. **The writer resolves every graph node's kind, including input children with no
   `ObjectId`.** The `ContentMetaProvider` is backed by a live `Content*`→`{kind,
   kind_version}` map captured on the writer thread by walking `Content::inputs()` over
   the pinned graph and interning through the `KindBridge`. A placeholder input child
   resolves to `nullopt`; a codec-backed input child with no metadata is
   `SerializeError::NoCodec` (`writer.hpp:24-28`), never a silent drop.

7. **Unknown-kind bodies (incl. a foreign-kind input child) still round-trip
   losslessly**, and a dangling/cyclic `$ref` in an operator input is a
   `ReaderError::UnresolvableReference` with `revision() == 0` (doc 08 P2/P6,
   `sharing` Decisions 7/8) — re-enforced through the L5 operator path.

8. **The no-provider writer output and every existing serialize/document golden are
   unchanged.** This leaf only *adds* two codecs and extends the sink/meta closures;
   `tests/serialize_writer_golden.t.cpp`, `tests/serialize_sharing.t.cpp`, and
   `tests/document_serialize_golden.t.cpp` must not regress.

9. **Levelization / no new edge / no JSON below L5.** All new code is in `arbc::runtime`;
   `nlohmann::json` appears only in the `codec_fade.cpp`/`codec_crossfade.cpp` TUs, never
   in a runtime public header. `scripts/check_levels.py` stays green with no graph edit;
   the L4 accessor additions name no JSON type.

10. **Save reads the writer-thread-owned content side-map safely.** The `Content*`→kind
    map and the graph capture are built on the writer thread; `serialize_document` runs
    off-thread over only immutable data (the pinned `DocRoot` + the captured map) — no
    lock, no torn `d_contents` read (`document_serialize` Decision 6, mirrored for the
    input-child capture).

11. **Diff coverage ≥ 90%** on changed lines (doc 16); `-Werror -Wpedantic` and
    `scripts/check_levels.py` green; the WBS gate `tj3 project.tjp 2>&1 | grep -iE
    "error|warning"` silent after the closer lands `complete 100` + the refinement
    back-link.

## Acceptance criteria

- **Operator-graph content round-trip, byte-exact (golden-backed).** A new
  cross-component test (`tests/operator_codecs_golden.t.cpp`) builds a `Document` with an
  operator graph — a `fade` over a `crossfade(from, to)`, plus a **shared** input content
  used ≥2 times so the `contents` table + `{"$ref": id}` dedup fires; `save_document`
  produces bytes compared **byte-for-byte** to an inline canonical `k_golden`; then
  `load_document` reloads into a fresh `Document` and `save_document` re-emits
  **byte-identical** output, with the shared child loading to a single `Content*` reached
  at both sites (pointer identity). Lands claim
  `08-serialization#builtin-operator-codecs-round-trip` in `tests/claims/registry.tsv`
  with an `enforces:`-tagged test. This test additionally re-enforces
  `08-serialization#inputs-array-round-trips` (`registry.tsv:194`) and
  `#shared-content-dedups-via-ref` (`:195`) through the real built-in operators (second
  `// enforces:` tags, no new rows).

- **Built-in fade/crossfade codec params + input adoption (behavioral, per-kind).** A
  runtime component test (`src/runtime/t/operator_codecs.t.cpp`) round-trips each
  operator through its registered codec: a `FadeContent{FadeParams}` over a leaf input and
  a `CrossfadeContent{CrossfadeParams}` over two leaf inputs are serialized to `params`
  JSON and back, asserting (i) the reconstructed params equal the originals, (ii) dispatch
  selects the codec (not `PlaceholderContent`), and (iii) the reconstructed operator's
  `inputs()` adopt the built input `Content*`s in the right slots (fade slot 0; crossfade
  `from`=0, `to`=1). Lands claim `08-serialization#builtin-operator-codecs-adopt-inputs`
  with an `enforces:`-tagged test.

- **`ContentSink` input-child ownership (behavioral).** The component test asserts that
  after `load_document`, an operator's input child resolves as a live built-in content
  **owned by `Document` with no `ObjectId`** (it is *not* returned by
  `DocRoot::find_content` and has no `ContentRecord`), while the operator's own layer root
  *does* bind an `ObjectId`/`ContentRecord` — the root/child ownership split
  (`sharing` Decision 3).

- **Writer resolves input-child kinds via the live reverse map (behavioral).** A test
  confirms the `ContentMetaProvider` resolves a codec-backed input child (no `ObjectId`)
  to its `(kind, kind_version)` — so a `crossfade` over two live solids re-saves with both
  input bodies present — and that a **placeholder** input child resolves to `nullopt` and
  re-emits its stored body verbatim (re-enforces
  `08-serialization#unknown-kind-round-trips-verbatim`, `registry.tsv:191`, second tag).

- **Pinned-snapshot fidelity (behavioral).** The golden test pins, saves the operator
  graph to bytes, then mutates the `Document` (adds another content/layer) **after** the
  pin, and asserts the pinned save's bytes are unchanged — re-enforcing
  `08-serialization#writer-serializes-the-pinned-version` (`registry.tsv:186`) and
  `14-data-model-and-editing#pinned-version-never-observes-later-edit` (`:4`) through the
  operator-graph save path (second `// enforces:` tags, no new rows).

- **Errors-as-values across the façade.** Malformed operator `params` (a non-object
  `params`, an unknown `shape` string, a fade body with wrong input arity) returns a
  `ReaderError` value with `Document`/`Model` left unmutated (`revision() == 0`); a
  dangling/cyclic `$ref` in an operator input returns
  `ReaderError::UnresolvableReference` (re-enforces `08-serialization#dangling-ref-is-read-error`,
  `registry.tsv:196`, second tag); a codec that cannot serialize returns a
  `SerializeError` (`NoCodec`/`CodecFailed`/`NonFiniteValue`) — no `nlohmann` exception,
  no partial mutation.

- **Concurrency — operator-graph save captured on writer thread, emitted off-thread
  (TSan).** A focused test under `tsan` (`tests/operator_codecs_concurrency.t.cpp`) has the
  writer thread churn `add_content`/mutations while a background thread runs the
  captured-snapshot `serialize_document` over the operator graph (incl. the input-child
  reverse-map capture); assertions are **outcomes only** — the emitted bytes equal a
  synchronous save of the same pinned revision, and TSan is clean. Never a wall-clock
  assertion (doc 16). (Note: the shared `audio_engine/lookahead.cpp` `-Werror=tsan` build
  issue that blocked `document_serialize`'s TSan lane in the GCC-14 env is pre-existing and
  not a regression of this leaf.)

- **Coverage / build / WBS gate.** ≥ 90% diff coverage; `-Werror -Wpedantic` and
  `scripts/check_levels.py` green (runtime `DEPENDS` gains `kind_fade kind_crossfade`,
  codec TUs covered by the existing nlohmann PRIVATE link, no graph edit); the closer
  confirms `tj3` silence after landing `complete 100` and the refinement back-link on
  `tasks/65-runtime.tji:47-51`, and adds `complete 100` to `m8_persistence` only if this is
  its last incomplete dependency.

- **No new WBS leaf; nested serialization parked.** This leaf registers **no** follow-up
  WBS task. The nested-composition-serialization design question (Decision 1) is a
  human design-judgment item the closer records in `tasks/parking-lot.md`; the concrete
  implementation leaf is registered only after that design is chosen (registering it now
  would be an uncloseable "decide-then-do" task, forbidden by doc 16 / the orchestrator
  brief).

## Decisions

1. **This leaf closes the operator-graph gap (fade + crossfade); `org.arbc.nested`
   serialization is out of scope and its design question is parked — not shipped lossy
   and not a widening.** Refinement investigation of the sources established that fade and
   crossfade are operator-graph `inputs` kinds — their inputs are `Content*` edges the
   `serialize.sharing` machinery already serializes (`fade_content.hpp:93-95`,
   `crossfade_content.hpp:95-99`) — whereas `org.arbc.nested` is structurally different: it
   stores an **in-document child composition by model `ObjectId`** (`nested_content.hpp:57,
   113,185`), and the `.arbc` format models **exactly one composition**, emitted under a
   single unkeyed `"composition"` root with **no id/name/URI**
   (`docs/design/08-serialization.md:22-24`; `src/serialize/writer.cpp` emits one
   composition via `find_first_composition`; `src/model/arbc/model/model.hpp:54-67`;
   `src/model/arbc/model/records.hpp:136-144`). Model `ObjectId`s are freshly re-allocated
   on load (`src/serialize/reader.cpp`, `txn.add_composition`), so a serialized raw
   `ObjectId` would not round-trip; and doc 08's `params.ref` for nested
   (`08-serialization.md:39-41`) is reserved for an **external** project file (doc 05:47-52),
   which additionally needs the cross-file composition loader `NestedContent` has no field
   for. A faithful nested round-trip therefore requires **new, undesigned format
   infrastructure** — either an in-document multi-composition addressing scheme (a
   `compositions` table analogous to `contents`, plus the L4 reader/writer composition
   recursion and multi-`CompositionRecord` model round-trip) or the external-file loader —
   which is L4-serialize / format-level design work, well past a 2d L5 codec leaf and
   across edges this leaf does not declare.

   Given that, the three options were: **(a)** force nested into this leaf by shipping the
   only thing feasible today — a codec that flattens the child's member layers through
   `inputs()` — which **loses the child's composition metadata (canvas, working_space) and
   identity**, violating doc 08 P2/P5 fidelity and the byte-exact golden; **(b)** design
   the multi-composition format myself via a doc-08/doc-05 delta and implement it here,
   ballooning a runtime-codec leaf into a cross-layer format redesign; or **(c)** deliver
   the fade/crossfade codecs that close the operator-graph gap, and **surface the
   nested-composition-serialization design question to `tasks/parking-lot.md`** as the
   human design-judgment item it is. Chosen: **(c)**. Rejected (a): doc 08 P2 ("a missing
   plugin **must never** destroy data") and P5 (byte-stable fidelity) forbid a knowingly
   lossy built-in codec. Rejected (b): originating a document-format multi-composition
   design (which reshapes doc 05 Droste semantics and the L4 serialize recursion) is above
   an operator-codecs refinement's remit and is not implementable in 2d; making that call
   unilaterally in an L5 codec leaf would be exactly the kind of undocumented
   design-by-side-effect the constitution guards against. The orchestrator brief's
   "close the full operator serialization gap / do not re-split" directive was premised on
   fade/crossfade/nested being three parallel operator-graph codecs; that premise is
   factually incorrect for nested, and the honest resolution is to close the *operator-graph*
   gap here and let a human settle the *composition-serialization* format before it becomes
   an implementation leaf. **No nested WBS leaf is registered now** — a leaf blocked on an
   unmade design decision is an uncloseable "decide-then-do" task (doc 16 / brief forbid
   audit/decide-later tasks); the implementation leaf follows the parking-lot decision.

2. **The built-in operator codecs live in runtime TUs that link `nlohmann::json`
   PRIVATE, registered into the existing runtime `CodecTable`.** A concrete
   fade/crossfade codec must see both the concrete kind type (L4 `kind-fade`/`kind-crossfade`)
   and the private JSON type (via L4 `serialize`); only `runtime` (L5) can (doc 08 P1,
   doc 17:41,59-60). The codec bodies go in `codec_fade.cpp`/`codec_crossfade.cpp`, which
   `#include <arbc/serialize/codec.hpp>` and are covered by the existing
   `arbc_runtime PRIVATE nlohmann_json` link (`CMakeLists.txt:22`); they register into the
   same runtime-owned `CodecTable` the solid/tone codecs already populate, extending the
   built-in registration entry point (`builtin_codecs.hpp`). *Rejected — codecs in
   `kind-fade`/`kind-crossfade`:* those are L4 peers of `serialize`, cannot depend on it,
   and must not name the private JSON type. *Rejected — a separate codec table for
   operators:* the `CodecTable` is keyed by kind id and dispatches uniformly; a second table
   buys nothing.

3. **Params serialize from the live immutable operator via a new const accessor, not from
   a captured `StateHandle`.** Fade/crossfade construction params are immutable
   (`fade.md`/`crossfade.md` Decision 7); `StateHandle` is inert until
   `model.editable_facet` (`document_serialize` Decision 2). The codec reads `params` off
   the live `Content` via a `params()` const accessor added to each kind header — today
   only the *evaluated* `envelope(Time)`/`position(Time)` are public
   (`fade_content.hpp:72`, `crossfade_content.hpp:74`), and reconstructing raw params from
   sampled envelope values is lossy, so a raw-struct getter is required. This is the same
   additive accessor move `document_serialize` made for solid/tone
   (`document_serialize.md:592-593`); it adds no JSON dependency to L4 and changes no
   behavior. *Rejected — reconstruct params from `envelope()`/`position()` samples:* lossy
   and non-invertible. *Rejected — a `friend` bridge instead of a public getter:* a plain
   const accessor for immutable construction params is simpler and has legitimate non-codec
   uses (inspection/tests); reserve `friend` for genuinely private surfaces.

4. **The runtime `ContentSink` gains input-child ownership in place; no `Document` shape
   change.** The sink closure extends to distinguish a layer root (mint `ContentRecord`,
   return `{id, live}`) from an input child (own it, return `{ObjectId{}, live}`), using
   the recursion context `serialize.sharing` already threads. The model stores no input
   edges (`records.hpp`), so the child needs no `ObjectId`; `Document` owning the object is
   sufficient for lifetime, and the parent adopts it by `Content*`. *Rejected — mint an
   `ObjectId`/`ContentRecord` for every input child:* pollutes the model with non-root
   contents the format never names and that `DocRoot::find_content` would wrongly surface.
   *Rejected — a separate child arena outside `Document`:* duplicates ownership machinery
   `d_contents` already provides.

5. **The `ContentMetaProvider` is backed by a live `Content*`→kind map built during the
   writer-thread save capture by walking `Content::inputs()`.** Input children have no
   `ObjectId`, so the `ObjectId`-keyed `ContentBodyProvider` cannot resolve them; the
   `const Content&`-keyed `ContentMetaProvider` (`writer.hpp:93`) does. This leaf builds the
   map by transitively walking `inputs()` over the pinned graph and interning each reached
   content's kind through the `KindBridge` — the reverse lookup `document_serialize`
   Decision 5 deferred. The walk happens inside the writer-thread capture (Decision 6 of
   `document_serialize`), so the off-thread emit reads only immutable data.
   *Rejected — add a persistent `Content*`→kind map to `Document`:* the durable copy for
   roots already lives in the pinned `ContentRecord`; a persistent live map for children
   would grow the writer-thread-owned surface for data that is cheap to re-derive per save
   from `inputs()`.

6. **No doc-00 decision-record bullet, no design-doc delta.** For fade/crossfade this task
   *implements* behavior docs 08 (P1/P6) and 17 already settle (built-in codecs register
   from `runtime`; the operator-graph format) — it relocates nothing and changes no
   designed behavior, following `document_serialize` Decision 8. The nested-composition
   *format* gap (Decision 1) is real design work, but this leaf **declines to originate that
   design** and parks it; because this leaf lands no nested behavior, it needs no
   design-doc delta. *Rejected — a doc-08 delta designing the `compositions` table here:*
   that is the parked decision's deliverable, not this codec leaf's; landing it as a
   side-effect of an operator-codecs task would bury a project-shaping format decision in
   the wrong change.

## Open questions

**Parked — human design judgment (`tasks/parking-lot.md`):** how v1 persists an
`org.arbc.nested` content's child-composition reference. The format models one unkeyed
composition and re-allocates model `ObjectId`s on load, so neither a raw `ObjectId` nor
today's single-`"composition"` shape can round-trip an in-document child; doc 08's
`params.ref` is reserved for external files and needs a cross-file composition loader
`NestedContent` has no field for. The decision — an in-document multi-composition
addressing scheme (a `compositions` table + L4 reader/writer composition recursion +
multi-`CompositionRecord` model round-trip, which also settles doc 05 Droste
serialization) vs. external-`params.ref`-plus-loader only for v1 — reshapes doc 05/08 and
the L4 serialize stream and is above this leaf's remit (Decision 1). Once chosen, it spawns
a concrete implementation leaf under milestone **M8**; it is deliberately **not** encoded as
a WBS "audit/decide" task.

All other questions are decided. Serialized operator **feedback cycles**
(`sharing` Decision 8) remain parked and out of scope; v1 `$ref` graphs are acyclic DAGs
and fade/crossfade are acyclic.

## Status

**Done** — 2026-07-09.

- `src/runtime/codec_fade.cpp`, `src/runtime/codec_crossfade.cpp` — two new built-in operator codec TUs: `SerializeFn` encodes `FadeParams`/`CrossfadeParams` to JSON; `DeserializeFn` validates arity first (0-input or wrong-arity body is a `ReaderError` leaving the model unmutated), then constructs `FadeContent(inputs[0], params)` / `CrossfadeContent(inputs[0], inputs[1], params)`.
- `src/kind_fade/arbc/kind_fade/fade_content.hpp`, `src/kind_crossfade/arbc/kind_crossfade/crossfade_content.hpp` — added const `params()` accessors so codecs can read back the immutable construction params (`FadeParams`/`CrossfadeParams`) without depending on JSON at L4.
- `src/runtime/arbc/runtime/builtin_codecs.hpp` — added `k_fade_kind_version`, `k_crossfade_kind_version` constants, and `fade_codec()`/`crossfade_codec()` declarations.
- `src/runtime/document_serialize.cpp` — pre-interned fade/crossfade kind strings; registered both codecs (save + per-load recording); extended `capture_snapshot` to walk `inputs()` transitively and build the live `Content*`→kind reverse map; extended `ContentSink` with demote-after-sink input-child ownership (provisional `ContentRecord` minted, then dropped when a parent adopts the node — only layer roots keep records).
- `src/runtime/CMakeLists.txt` — added `codec_fade.cpp`, `codec_crossfade.cpp` to sources; added `kind_fade kind_crossfade` to `DEPENDS`.
- `tests/operator_codecs_golden.t.cpp` — byte-exact golden: fade-over-crossfade graph with a shared input (`$ref` dedup fires), round-trips to identical bytes; pinned-snapshot mutation test; re-enforces `#writer-serializes-the-pinned-version` and `#pinned-version-never-observes-later-edit`.
- `tests/operator_codecs_concurrency.t.cpp` — TSan concurrency lane: writer-thread churns mutations while background thread emits the captured operator-graph snapshot.
- `src/runtime/t/operator_codecs.t.cpp` — runtime component test: per-codec param round-trip (6 cases, 79 assertions); input-child ownership (`no ObjectId`, not returned by `find_content`); placeholder-input verbatim re-emit; dangling-`$ref` error-as-value; `tests/CMakeLists.txt` wired for all three test targets.
- `tests/claims/registry.tsv` — two new rows: `08-serialization#builtin-operator-codecs-round-trip`, `#builtin-operator-codecs-adopt-inputs`; six existing claims re-enforced via second `// enforces:` tags (`#inputs-array-round-trips`, `#shared-content-dedups-via-ref`, `#unknown-kind-round-trips-verbatim`, `#writer-serializes-the-pinned-version`, `#dangling-ref-is-read-error`, `14-…#pinned-version-never-observes-later-edit`).
- Nested-composition serialization (Decision 1) parked to `tasks/parking-lot.md` — no WBS follow-up leaf registered.
