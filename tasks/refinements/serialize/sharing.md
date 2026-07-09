# serialize.sharing — inputs arrays + contents table

## TaskJuggler entry

Back-link: [`tasks/60-serialize.tji:33-38`](../../60-serialize.tji) — `task sharing`
inside `task serialize`.

> note "Core-owned operator input edges beside params; document-level
> contents table with $ref for shared content. Docs 08/13."

## Effort estimate

**1d** (`tasks/60-serialize.tji:34`) — lighter than the 2d `writer`/`reader`/
`kind_params` peers, and deliberately so: `kind_params` already stood up the
whole content-body machinery this task turns from *preservation* into
*interpretation*. The `{kind, kind_version, params, inputs}` body is already
captured verbatim on read (`reader.cpp:196`) and re-emitted canonical on write
(`codec.cpp:83-84`); `PlaceholderContent` already carries an (unused) `inputs`
constructor parameter (`placeholder_content.hpp:41-42`); `ReaderError::
UnresolvableReference` is already reserved for this task (`reader.hpp:33`). So
the day covers three focused pieces of **L4 structural work on landed seams**:
(1) the **write recursion** — walk `Content::inputs()`, emit an `inputs` array
beside `params`, hoist any content referenced twice into a document-level
`contents` table and emit `{"$ref": id}` at each use site; (2) the **read
recursion** — parse `inputs`/`contents`/`$ref`, resolve refs with intra-document
dedup, build the input graph through the existing codec routing, and wire the
edges; (3) the **seam extensions** these force (a `const Content&`-keyed metadata
lookup on the write provider, a live-`Content*` return on `ContentSink`, and an
`inputs`-carrying `DeserializeFn`) plus their L4 goldens/claims. The live-runtime
graph binding against `Document` (ownership, the built-in operator codecs) stays
where `kind_params` already put it — the L5 leaf `runtime.document_serialize`
(Decision 5, below).

## Inherited dependencies

`sharing` declares `depends !kind_params` (`tasks/60-serialize.tji:36`) and,
through its parent `task serialize` (`tasks/60-serialize.tji:4`), inherits
`depends contract.registry, model.journal`.

**Settled (formal `depends`, all Done — 2026-07-09):**

- **`serialize.kind_params`** ([`serialize/kind_params.md`](kind_params.md)) —
  the direct predecessor, which landed every seam this task interprets and
  **explicitly deferred `inputs`/`contents`/`$ref` interpretation to
  `serialize.sharing`** (kind_params Decision 6, `kind_params.md:488-498`;
  "Downstream", `kind_params.md:91-97`). Concretely handed forward:
  - The **content-body codec seam**: `content_body_from_json(const json& body,
    const CodecTable&, const Registry&, LoadContext&)` (`src/serialize/arbc/
    serialize/codec.hpp:80-82`, `codec.cpp:31-74`) and `content_body_to_json(
    std::string_view kind, std::string_view kind_version, const Content&,
    const CodecTable&)` (`codec.hpp:91-93`, `codec.cpp:76-103`). **Both ignore
    `inputs` today** — the read path validates only `kind`/`params` and hands
    the whole body to the placeholder verbatim (`codec.cpp:73`); the write path
    emits only `{kind, kind_version, params}` (`codec.cpp:98-101`). This task
    adds the `inputs`/`$ref` limb to both.
  - The **`CodecTable`** keyed by reverse-DNS kind id (`codec.hpp:54-69`) and
    the `Codec { SerializeFn serialize; DeserializeFn deserialize; }` pair
    (`codec.hpp:44-47`); `DeserializeFn = std::function<expected<std::unique_ptr
    <Content>, ReaderError>(const nlohmann::json& params, LoadContext& ctx)>`
    (`deserialize.hpp:33-34`) — the signature this task extends to carry inputs
    (Decision 4).
  - **`PlaceholderContent`** (`placeholder_content.hpp:34-73`), whose ctor
    already accepts `std::vector<ContentRef> inputs` ("empty on the read path
    until serialize.sharing binds them", `placeholder_content.hpp:38-42`),
    surfaces them via `inputs()` (`:59`, `:72`), and returns input 0 from
    `identity()` when present (`:65`). This task is what finally populates that
    vector on the read path.
  - The **L4 read wiring**: `load_document(json, const Registry&, const
    CodecTable&, LoadContext&, const ContentSink&, Model&)` (`reader.hpp:85-87`,
    `reader.cpp:249`), `extract_content_body` (`reader.cpp:191-203`) which
    **already copies `inputs` verbatim into the captured body** (`:196`), and
    the `ContentSink = std::function<ObjectId(std::unique_ptr<Content>)>`
    (`reader.hpp:71`) that owns each built content — the seam this task extends
    to also yield the live `Content*` for edge wiring (Decision 3).
  - The **L4 write wiring**: `serialize_document(const DocRoot&, const
    ContentBodyProvider&, const CodecTable&)` (`writer.hpp:84-86`,
    `writer.cpp:232`), `ContentBody { std::string_view kind; std::string_view
    kind_version; const Content& content; }` and `ContentBodyProvider =
    std::function<std::optional<ContentBody>(ObjectId)>` (`writer.hpp:63-73`),
    and the `layer_json` content-body injection/merge point
    (`writer.cpp:153-166`). The writer header already names the future scope:
    "operator `inputs`, and the `contents`/`$ref` sharing table are later
    serialize tasks" (`writer.hpp:50-51`).
  - The **new L5 leaf `runtime.document_serialize`** (kind_params Decision 5,
    `kind_params.md:456-486`) — where the real `ContentSink`/provider against
    `Document` and the built-in kind codecs land. This task widens that leaf's
    scope to include the operator-graph binding rather than register a peer
    (Acceptance / Decision 5).
- **`serialize.reader`** (transitive, [`serialize/reader.md`](reader.md)) —
  landed `LoadContext` (`load_context.hpp:49-87`) with `resolve()`/`ResolvedRef`
  (`load_context.hpp:19-23,59-68`) and the **cross-file resolved-identity dedup**
  (claim `08-serialization#loadcontext-dedups-by-resolved-identity`). That is a
  **distinct** mechanism from this task's intra-document `contents`/`$ref` dedup
  (Decision 6); this task reuses `LoadContext` only as the pass-through resolver
  a nested kind's `params` URIs already flow through, unchanged.
- **`serialize.writer`** (transitive, [`serialize/writer.md`](writer.md)) — the
  canonical emission engine and the Principle-5 canonical rules (sorted keys,
  shortest-round-trip numbers, non-finite → error value) that govern how the new
  `inputs` array, `contents` table, and `$ref` objects must be emitted.
- **`contract.registry`** ([`contract/registry.md`](../contract/registry.md)) —
  the reverse-DNS kind id is the persistent serialization token; `Registry`
  supplies the plugin-present witness the placeholder falls back on
  (`codec.cpp:72`).
- **`model.journal`** (transitive) — the `load_baseline` machinery; unchanged.

**No pending inherited dependencies** — every predecessor is Done.

**Downstream (this task unblocks / feeds):**

- `serialize.format_tests` (`!kind_params`, `tasks/60-serialize.tji:39-44`) —
  the libFuzzer harness now also fuzzes the `inputs`/`$ref` parse this task adds,
  and the shared-`contents` round-trip joins the determinism corpus.
- `runtime.document_serialize` (existing leaf, M8) — consumes the extended
  `ContentSink`/provider/`DeserializeFn` seams to bind live operator graphs into
  `Document` and register the built-in `org.arbc.fade`/`org.arbc.crossfade`/
  `org.arbc.nested` codecs.

## What this task is

Turn the serialization format's content body from a single leaf into a **graph**:
give operators their core-owned input edges and let shared content be written
once and referenced. Concretely:

(a) **Write-side `inputs` emission.** Extend the content-body write path so that,
given a live `Content&`, it walks `content.inputs()` (`content.hpp:579`) and
emits an `inputs` array **beside** `kind`/`kind_version`/`params` (doc 08
Principle 6; doc 13 §Serialization), in declared order, each slot an inline
`{kind, kind_version, params, inputs?}` body — nested recursively for chains.
The array is **omitted when empty** (leaf content), mirroring the omit-when-
default discipline of placement fields (doc 08:48-55).

(b) **Write-side `contents`/`$ref` hoisting.** A pre-pass counts distinct
`Content*` occurrences across the document's reachable graph (all layers'
roots, depth-first over `inputs()`). A content referenced **≥ 2 times** is
*shared*: it is emitted **once** into an optional document-level `contents`
table (a JSON object at document root, beside `composition`) under a
deterministic id, and every use site (an `inputs` slot **or** a layer's content
position) emits `{"$ref": id}` instead of the inline body. Singly-referenced
content stays inline. Ids are **core-owned, deterministic first-encounter
ordinals** (Decision 2).

(c) **Read-side `inputs`/`contents`/`$ref` interpretation.** Extend the
content-body read path so that, after routing a node's `{kind, params}` through
the codec table (known → codec `Content`; unknown → `PlaceholderContent`), it
recursively processes `body["inputs"]`: each slot is an inline body (recurse) or
`{"$ref": id}` (resolve against the parsed `contents` table, **built once and
shared** so a shared entry yields **one** live `Content` referenced from every
use site — intra-document dedup, Decision 6). A layer's content position may
itself be a `{"$ref": id}`. The built child `Content*`s are wired as the
parent's input edges (Decision 4). An unresolvable or cyclic `$ref` is a
`ReaderError::UnresolvableReference` value with no model mutation (Decision 7).

(d) **The seam extensions (c)/(b) force.** The write provider gains a
`const Content&`-keyed metadata lookup (so a child reached via `inputs()`
resolves to its `(kind, kind_version)`; L5 supplies it from `Document`'s reverse
map). `ContentSink` extends to return the live `Content*` alongside the
`ObjectId` (so the read recursion can wire non-owning `ContentRef` edges into
parents while the sink owns every node). `DeserializeFn` extends to receive the
already-built input `Content*`s (so a known-kind operator adopts its inputs at
construction). `PlaceholderContent`'s existing `inputs` ctor param
(`placeholder_content.hpp:41-42`) is finally populated on the read path.

(e) **A doc-08 Principle 6 delta** pinning the parts the doc left open — the
omit-when-empty `inputs` rule, the ≥2-refs hoisting rule, the deterministic-
ordinal id scheme and its canonicalization on save, the "`$ref` in any slot or
layer content" placement, and the v1 **acyclic-`$ref`** commitment (Decision 8).

This task does **not** implement the real `ContentSink`/provider against
`runtime::Document`, does **not** register the built-in operator codecs
(`org.arbc.fade`/`.crossfade`/`.nested`) — those adopt the extended seams in
`runtime.document_serialize` (Decision 5) — and does **not** serialize operator
**feedback cycles** (v1 `$ref` graphs are DAGs, Decision 8). It lands the L4
structural read/write of the operator graph and proves it with test-registered
codecs, synthetic `Content` graphs, and placeholders.

## Why it needs to be done

`kind_params` made a *single* content round-trip; but doc 13's whole thesis is
that "effects enter the model as content that consumes content" (doc 13:3-7) —
the layer stack is a DAG, and a fade-over-a-clip, a crossfade, or a shared graded
clip is meaningless if only the top node persists. Principle 6 is the promise
that operator graphs "serialize structurally"; this task is where that promise
becomes a testable format. It is also the last piece the reference operators
need to survive a save: `org.arbc.fade` is a single-input operator and
`org.arbc.crossfade` a two-input one (doc 13:167-168), both committed to v1
(doc 13:201-205), and neither can round-trip without an `inputs` array. Shared
content (doc 05:39-45; doc 13:28-29 — "several layers can reference the same
graded clip, with one cache") needs the `contents`/`$ref` table so persistence
does not silently fan a shared node into independent copies. And it completes
doc 13's degradation promise: a placeholder that *preserves and wires* its
inputs is what makes "a missing fade plugin degrades to an unfaded clip, not a
hole" (doc 08:110-111) real rather than latent.

## Inputs / context

### Design docs (normative, doc 16)

- `docs/design/08-serialization.md:105-111` — **Principle 6**, *amended by this
  task's delta*: "Input edges are core-owned, so they live in an `inputs` array
  beside `kind`/`params` (mirroring `Content::inputs()`), nested inline for
  chains; shared content goes in an optional document-level `contents` table
  referenced by `{"$ref": id}`. Unknown-kind placeholders preserve their inputs
  and may render input 0 as pass-through — a missing fade plugin degrades to an
  unfaded clip, not a hole." The chartered heart of this task.
- `docs/design/08-serialization.md:82-87` — **Principle 3**: cross-file sharing
  "deduplicates through the loader by resolved identity." A **distinct**
  mechanism from `contents`/`$ref`; this task does not touch it (Decision 6).
- `docs/design/08-serialization.md:92-104` — **Principle 5**: canonical output
  (sorted object keys, shortest-round-trip numbers, non-finite → error value).
  Governs how `inputs` arrays (order **preserved** — array order is semantic),
  the `contents` object (keys sorted), and `$ref` objects are emitted, and makes
  the id-canonicalization of Decision 2 well-defined.
- `docs/design/08-serialization.md:48-55` — the omit-when-default discipline for
  core-owned fields (`span`/`time_map`/`gain`/`audible`/`working_space`) the
  empty-`inputs` omission mirrors.
- `docs/design/13-effects-as-operators.md:44-67` — the `inputs()` contract:
  `virtual std::span<const ContentRef> inputs() const` (`:52`), "visible to the
  core", declared order, leaf returns empty; `identity()` (`:59-65`) the
  pass-through action; `map_input_damage(size_t input, …)` (`:57`) — index
  positions are semantic and correspond to `inputs` array positions.
- `docs/design/13-effects-as-operators.md:140-161` — **§Serialization (amends
  doc 08)**: the concrete `org.arbc.fade` example with an inline `inputs` array
  (`:145-153`), "for *shared* content the document gains an optional `"contents"`
  table of id → content description, referenced as `{"$ref": "id"}` from any
  `inputs` slot or layer" (`:155-157`), and the placeholder-preserves-inputs
  extension (`:158-161`).
- `docs/design/13-effects-as-operators.md:134` — operator-graph cycles are
  *feedback*; `docs/design/13-effects-as-operators.md:201-205` — the v1 commit is
  `fade`/`crossfade` (acyclic). Grounds the v1 acyclic-`$ref` call (Decision 8).
- `docs/design/05-recursive-composition.md:39-52` — "content is shared, placement
  is per-instance"; shared/instanced children. The model the `contents`/`$ref`
  table persists intra-document; cross-file children are the nested kind + loader
  (Principle 3), not `$ref`.
- `docs/design/17-internal-components.md:56-60` — levelization: `serialize` is
  **L4** (DEPENDS `contract`, `model`; nlohmann PRIVATE), `runtime` is **L5**.
  `contract` is L3, `model` L2. This is why the live-graph bind is L5
  (Decision 5) and the internal seams naming `nlohmann::json` stay off the
  public header.

### Source seams

- `src/serialize/arbc/serialize/codec.hpp:80-93`, `src/serialize/codec.cpp:31-103`
  — `content_body_from_json` (read) and `content_body_to_json` (write): the two
  routing functions this task grows an `inputs`/`$ref` limb on. Today `from_json`
  ignores `inputs` (`codec.cpp:73` hands the whole body to the placeholder) and
  `to_json` emits only `{kind, kind_version, params}` (`codec.cpp:98-101`).
- `src/serialize/arbc/serialize/deserialize.hpp:33-34` — `DeserializeFn`, extended
  to carry the built input `Content*`s (Decision 4).
- `src/serialize/arbc/serialize/placeholder_content.hpp:34-73` —
  `PlaceholderContent`; its `inputs` ctor param (`:41-42`), `d_inputs` (`:72`),
  `inputs()` (`:59`), `identity()` (`:65`). This task populates the vector on
  read.
- `src/serialize/arbc/serialize/reader.hpp:33,71,85-87`;
  `src/serialize/reader.cpp:191-203,249-296,305-317,323-357` —
  `ReaderError::UnresolvableReference` (reserved for this task, `:33`);
  `ContentSink` (`:71`, extended to yield the live `Content*`, Decision 3);
  the content-aware `load_document`; `extract_content_body` (already captures
  `inputs`, `reader.cpp:196`); the document-root parse where the `contents`
  table is read (`reader.cpp:249-296`); the per-layer routing loop
  (`reader.cpp:305-317`) and the `load_baseline` bind (`reader.cpp:323-357`).
- `src/serialize/arbc/serialize/writer.hpp:50-51,63-86`;
  `src/serialize/writer.cpp:153-166,185-190,198-224` — the future-scope note
  (`writer.hpp:50-51`); `ContentBody`/`ContentBodyProvider` (extended with a
  `const Content&`-keyed lookup, Decision 3); the `layer_json` content-body
  merge point (`writer.cpp:159-161`) where `inputs` lands among the sorted keys;
  the layer walk (`writer.cpp:185-190`); the envelope build
  (`writer.cpp:198-224`) where the `contents` table is emitted at document root.
- `src/serialize/reader.cpp:41-54` — the fuzz-hardened `num_or`/`bool_or`/
  `int_or` accessors to reuse when reading `$ref`/`inputs` entries (errors as
  values, no nlohmann exception).
- `src/contract/arbc/contract/content.hpp:211-212,579,591,598` — `ContentRef =
  Content*` (non-owning, `:211-212`), `inputs()` (`:579`), `map_input_damage`
  (`:591`), `identity()` (`:598`). The operator is a non-owning observer of its
  inputs (`content.hpp:204-212`); ownership lives in the runtime side-map.
- `src/kind_fade/arbc/kind_fade/fade_content.hpp:93-95`,
  `src/kind_crossfade/crossfade_content.cpp:56-57`,
  `src/kind_nested/arbc/kind_nested/nested_content.hpp:100,118-142` — the
  reference operators whose input storage the round-trip must reproduce; their
  codecs land in `runtime.document_serialize` (Decision 5), so this task proves
  the seam with a **test operator codec** instead.
- `src/model/arbc/model/records.hpp:60-92` — `ContentRecord { std::uint64_t
  kind; StateHandle state; }` (`:60-63`) and `LayerRecord` (`:68-92`): the model
  **stores no input edges** — a content is named by `ObjectId`, the input graph
  is purely the runtime `Content*` graph. This is why only layer *roots* get an
  `ObjectId`/`ContentRecord`; input children are owned by the sink and referenced
  by `Content*` alone (Decision 3).
- `src/runtime/arbc/runtime/document.hpp:73`, `src/runtime/document.cpp:9,78-79`
  — `Document::d_contents` (`std::unordered_map<ObjectId, std::shared_ptr<
  Content>>`) and `resolve`: the L5 side-map the extended sink/provider bind
  against (Decision 5).
- `tests/claims/registry.tsv:182-188+` — the serialize claim rows; this task
  appends its `08-*` rows (Acceptance).
- `tests/serialize_writer_golden.t.cpp:54` (the inline `k_golden` raw-string
  convention) and `src/serialize/t/*.t.cpp` — the golden/test convention: inline
  raw-string literals, **no on-disk `.arbc` fixtures**.

**Predecessor / sibling refinements:** [`serialize/kind_params.md`](kind_params.md)
(the codec seam, `PlaceholderContent`, the explicit `inputs`/`contents`/`$ref`
deferral in Decision 6, and the `runtime.document_serialize` leaf),
[`serialize/reader.md`](reader.md) (`LoadContext`, cross-file resolved-identity
dedup, `ContentSink`), [`serialize/writer.md`](writer.md) (canonical formatting,
the `contents`/`$ref` future-scope note).

## Constraints / requirements

1. **`inputs` is a core-owned array beside `params`, order-significant, omitted
   when empty.** It mirrors `Content::inputs()` (`content.hpp:579`): slot *i* is
   input index *i* (the same index `identity()` and `map_input_damage` name).
   The canonical writer sorts object *keys* but **preserves array order** (doc 08
   Principle 5). A leaf content (empty `inputs()`) emits no `inputs` key
   (doc 08:48-55 discipline).

2. **Shared content is hoisted iff referenced ≥ 2 times; ids are deterministic.**
   The write pre-pass counts distinct `Content*` occurrences over the full
   reachable graph. Occurrence ≥ 2 → one `contents` entry + `{"$ref": id}` at
   each site; occurrence 1 → inline. Id assignment is a deterministic
   first-encounter ordinal over the canonical traversal (layers bottom-to-top via
   `for_each_layer_in`, each root depth-first over `inputs()` in declared order),
   so output is byte-stable across runs and re-serializations (doc 08 Principle 5).

3. **Intra-document dedup on read: a shared `$ref` yields one live `Content`.**
   Resolving the same `contents` id from multiple use sites produces **one**
   built `Content` instance, referenced by every site's `ContentRef` — persistence
   must not fan a shared node into copies (doc 05:39-45; doc 13:28-29). A
   `contents` entry is built at most once regardless of reference count.

4. **`$ref` is valid in any input slot or a layer's content position; the layer
   graph is a DAG in v1.** A dangling `$ref` (id absent from `contents`) or a
   `$ref` that closes an operator-input cycle is a `ReaderError::
   UnresolvableReference` value with **no model mutation and no nlohmann
   exception** (reader.hpp:33; doc 08 Principle 5; doc 10 errors-as-values).
   Feedback-cycle serialization is out of v1 scope (Decision 8).

5. **Unknown-kind placeholders preserve *and wire* their inputs.** A placeholder
   still re-serializes its `{kind, kind_version, params, inputs}` body
   byte-equivalent (the `kind_params` guarantee, unchanged), and now its parsed
   input `Content*`s populate `PlaceholderContent`'s `inputs()` so `identity()`
   surfaces a real input-0 pass-through (doc 08:110-111; doc 13:158-161). The
   pass-through remains the placeholder's *chosen* behavior, not a format
   invariant (kind_params Decision 4).

6. **The no-provider writer path and the existing goldens are unchanged.** The
   content-free `serialize_document(const DocRoot&)` overload stays byte-identical;
   `inputs`/`contents`/`$ref` appear only through the content-aware overload, and
   a document with only leaf content (no operators, no sharing) emits **no**
   `inputs` key and **no** `contents` table — so `kind_params`' existing goldens
   do not regress.

7. **`$ref` ids are non-semantic, canonicalized handles.** They are core-owned
   document-global tokens, re-derived on every save from graph structure
   (Constraint 2). A hand-authored file's arbitrary ids are normalized to the
   derived ordinals on re-serialization — consistent with the canonical-output
   guarantee (ids are internal handles, like key ordering), and *not* a violation
   of unknown-kind verbatim preservation, which covers `kind`/`kind_version`/
   `params` and input *content*, never the specific `$ref` token (Decision 2).

8. **No exceptions across the boundary; errors as values.** `$ref`/`inputs`
   reads use the fuzz-hardened accessors (`reader.cpp:41-54`); malformed
   structure yields `ReaderError`, a codec that cannot serialize yields
   `SerializeError`. No nlohmann exception escapes on any input — the precondition
   `serialize.format_tests` fuzzing relies on.

9. **Levelization (doc 17), layout.** All code lives in `arbc_serialize`
   (L4; DEPENDS `contract`, `model`; nlohmann PRIVATE). The extended
   `DeserializeFn`/codec routing stay in **internal** headers (name
   `nlohmann::json`); the extended `ContentSink`/`ContentBodyProvider` (which
   name no JSON) stay public. No new `arbc_*` component edge — the live-graph
   bind is L5 (Decision 5), so `scripts/check_levels.py` stays green with no edit.

10. **Diff coverage ≥ 90%** on changed lines (doc 16); the WBS gate
    `tj3 project.tjp 2>&1 | grep -iE "error|warning"` is silent after the closer
    lands `complete 100` + the refinement back-link.

## Acceptance criteria

- **Inline operator-graph round-trips (golden-backed).** A test builds a
  single-input operator over a leaf (a fade-over-clip shape) as a synthetic
  `Content` graph, serializes it through the content-aware writer with a
  test codec + `const Content&`-keyed provider, and asserts a byte-exact inline
  `k_golden` with an `inputs` array beside `params` in canonical key order;
  loading that golden back through the content-aware reader (test codec + stub
  sink) rebuilds a two-node graph whose root `inputs()` yields the child. Lands
  claim `08-serialization#inputs-array-round-trips` in `tests/claims/registry.tsv`
  with an `enforces:`-tagged test.

- **Shared content dedups via `contents`/`$ref` (golden-backed + behavioral).**
  A test builds a document where **two** layers reference **one** shared child
  `Content*`; serializing emits the child **once** into a document-level
  `contents` table and `{"$ref": id}` at both sites (byte-exact golden). Loading
  it back asserts both sites resolve to the **same** live `Content*` (pointer
  identity — intra-document dedup, Constraint 3), and that a singly-referenced
  child stays inline. Lands claim `08-serialization#shared-content-dedups-via-ref`
  with an `enforces:`-tagged test. Behavioral (pointer identity / structural
  equality), never wall-clock.

- **Dangling / cyclic `$ref` is a read error (behavioral).** A test drives the
  content-aware `load_document` on a body whose `$ref` names an absent id, and on
  one whose `$ref` closes an operator-input cycle, and asserts each returns
  `ReaderError::UnresolvableReference` with the target `Model` unmutated
  (`revision() == 0`) and no nlohmann exception. Lands claim
  `08-serialization#dangling-ref-is-read-error` with an `enforces:`-tagged test.

- **Placeholder pass-through over a *parsed* input (behavioral).** A test loads
  an unknown-kind body carrying one `inputs` entry (a known test-kind leaf),
  asserts the resulting `PlaceholderContent::inputs()` surfaces the parsed child
  and `identity()` returns `0` (real input-0 pass-through), and that the body
  still re-serializes byte-equivalent. Extends the existing
  `08-serialization#placeholder-renders-input-0-passthrough` claim from a
  synthetic input to a parsed one (re-`enforces:` on the new test).

- **No-provider path and `kind_params` goldens unchanged.** A test re-asserts
  the content-free `serialize_document(const DocRoot&)` overload output and a
  leaf-only content-aware document both remain byte-identical to the existing
  goldens — no stray `inputs` key or `contents` table (Constraint 6).

- **Coverage / build / WBS gate.** ≥ 90% diff coverage; `-Werror -Wpedantic` and
  `scripts/check_levels.py` stay green (no new component edge); the closer
  confirms `tj3` silence after landing `complete 100` and the refinement
  back-link on `tasks/60-serialize.tji:33-38`.

- **Design-doc delta (same commit).** `docs/design/08-serialization.md`
  Principle 6 is amended (written by this refinement) with the omit-when-empty
  `inputs` rule, the ≥2-refs hoisting + deterministic-ordinal id scheme + save
  canonicalization, the "`$ref` in any slot or layer content" placement, and the
  v1 acyclic-`$ref` commitment; the closer commits it with the code (doc 16
  same-commit).

- **Deferred — no new WBS leaf; existing leaves widened.** No task registers a
  new leaf (matching `reader`/`writer` and the 1d budget). The **live-graph
  bind** (real `ContentSink`/provider against `Document`, the built-in
  `org.arbc.fade`/`.crossfade`/`.nested` codecs adopting the extended
  `DeserializeFn`, and the end-to-end operator-graph load→save golden) **widens
  the already-registered `runtime.document_serialize`** (M8) — the closer appends
  that scope to its `note`, not a new leaf. **Fuzzing** of the `inputs`/`$ref`
  parse and the shared-`contents` determinism-corpus case go to the existing
  `serialize.format_tests` (`!kind_params`). **Cross-file** `$ref`-vs-URI sharing
  end-to-end stays with `kinds.nested` + `serialize.format_tests` (per
  `reader.md`). Serialized operator **feedback cycles** are a genuine v1-scope
  question surfaced to `tasks/parking-lot.md`, **not** a WBS task (Decision 8).

## Decisions

1. **Interpret the operator graph structurally on both sides, keyed on
   `Content::inputs()` — not on any model-level edge.** The write side walks the
   live `Content*` graph (`content.hpp:579`); the read side rebuilds it and wires
   `ContentRef` edges. Rationale: the model **stores no input edges**
   (`records.hpp:60-92`) — the graph exists only as the runtime `Content*` graph,
   exactly the surface `inputs()` exposes and doc 08 Principle 6 says the format
   mirrors. *Rejected — add an input-edge list to `ContentRecord`:* duplicates
   the runtime graph into the model, contradicts doc 14's model shape (records
   are pointer-free, index-only, trivially destructible, `records.hpp:15-22`),
   and is an L2 change a serialization task has no business making. *Rejected —
   fold `inputs` into `params`:* doc 08/13 are explicit that input edges are
   **core-owned, beside `params`** (08:105-111; 13:142-143); burying them in the
   kind's opaque blob forfeits the core's graph awareness (aggregate revision,
   damage routing, cycle detection).

2. **`$ref` ids are core-owned deterministic first-encounter ordinals,
   canonicalized on save; sharing threshold is occurrence ≥ 2.** Doc 08/13 fix
   the *shape* (`contents` = id → content description, `{"$ref": id}`) but leave
   the id scheme and hoisting rule open. This task pins: hoist a `Content*`
   referenced ≥ 2 times, assign ids by first-encounter order over the canonical
   traversal (Constraint 2), emit ids as their decimal string. Rationale:
   determinism (Principle 5) demands a structure-derived, run-independent scheme;
   occurrence-count is the minimal, observable definition of "shared"; deriving
   ids from traversal order (not content hashes or authoring tokens) keeps them
   stable and makes re-serialization byte-exact. Ids are non-semantic handles, so
   normalizing hand-authored ids on save is canonicalization, not data loss
   (Constraint 7). *Rejected — content-hash ids:* stable but opaque, hurt
   diff-ability, and add a hashing dependency for no benefit over ordinals.
   *Rejected — preserve authoring-supplied ids verbatim:* breaks byte-stable
   canonical output (two files with the same graph and different id strings would
   diff), and ids are internal handles the format need not fix. *Rejected — hoist
   every content into `contents` (no inline):* inflates every document and buries
   the common chain case; doc 08:105-108 explicitly keeps chains "nested inline".
   This is a genuine spec fill → **doc-08 Principle 6 delta**.

3. **`ContentSink` yields the live `Content*`; the write provider gains a
   `const Content&`-keyed metadata lookup.** Read: a parent operator needs live
   `ContentRef`s to its children, but `ContentSink` currently returns only an
   `ObjectId` (`reader.hpp:71`) and the child is owned by the sink — so the sink
   is extended to return the built `Content*` too (e.g. a `{ObjectId id; Content*
   live; }`), and the recursion builds children-first, threading their `live`
   pointers into parents while the sink owns every node. Write: a child reached
   via `inputs()` is a `Content&`, but the provider is `ObjectId`-keyed
   (`writer.hpp:73`); it gains a `const Content&` → `(kind, kind_version)` lookup
   (L5 supplies it from `Document`'s reverse map). Rationale: only layer *roots*
   get an `ObjectId`/`ContentRecord` (`records.hpp`); input children live purely
   as `Content*`, so the seams must speak `Content*`. *Rejected — resolve child
   `ObjectId`s at L4:* the `Content*` → `ObjectId` map is `Document::d_contents`,
   an L5 structure `serialize` cannot reach (doc 17:60). *Rejected — a generic
   `Content::attach_inputs()` on the contract interface:* pushes serialize-
   specific wiring into L3 and mutates operators post-construction, when they
   already take inputs at construction (`fade_content.hpp:93-95`).

4. **`DeserializeFn` extends to receive the built input `Content*`s.** A
   known-kind codec constructs its operator with inputs in hand (e.g.
   `FadeContent(params, inputs[0])`), so `DeserializeFn` grows a
   `std::span<const ContentRef> inputs` parameter (`deserialize.hpp:33-34`);
   the read recursion builds inputs first and passes them down. Rationale: keeps
   input-adoption in the codec seam (L4-internal, no contract change), symmetric
   with the write side walking `inputs()`. *Rejected — a second post-construct
   hook:* two-phase construction for no gain; operators already accept inputs at
   construction. *Rejected — leave `DeserializeFn` as-is and wire inputs only for
   placeholders:* then no *known* operator could round-trip its graph, defeating
   the task; `fade`/`crossfade` are v1-committed (doc 13:201-205).

5. **The live-runtime graph bind and built-in operator codecs widen
   `runtime.document_serialize` — no new leaf.** Binding the built graph into
   `Document::d_contents` (ownership of every node, the `Content*` ↔ `ObjectId`
   reverse map behind the provider, the real sink), registering the
   `fade`/`crossfade`/`nested` codecs against the extended `DeserializeFn`, and
   the end-to-end operator-graph load→save golden are all **L5** concerns — the
   same leaf `kind_params` already registered for the solid/tone bind (M8,
   `kind_params.md:456-486`). This task hands that leaf the extended seams; the
   closer widens its `note` to name the operator-graph work. Rationale: a peer
   leaf would duplicate `runtime.document_serialize`'s exact scope (Document
   wiring + built-in codecs); widening keeps the graph honest and avoids an
   orphan. *Rejected — do the bind here:* L5 code `serialize` (L4) cannot reach
   (doc 17). *Rejected — a new `runtime.operator_graph_serialize` leaf:* same
   Document-wiring machinery as `runtime.document_serialize`; splitting it invents
   a dependency edge for no separation of concerns.

6. **`contents`/`$ref` (intra-document) is distinct from `LoadContext` resolved-
   identity dedup (cross-file); this task owns only the former.** Two parents
   referencing one child `.arbc` dedup by resolved URI through `LoadContext`
   (Principle 3, landed in `reader`, claim `#loadcontext-dedups-by-resolved-
   identity`); two use sites referencing one shared content *within* a document
   dedup by `$ref` id (Principle 6, this task). Rationale: the docs treat them
   separately (08:82-87 vs 08:105-111); conflating them would route intra-document
   sharing through URI resolution it has no URI for. *Rejected — express intra-
   document sharing as internal-fragment URIs through `LoadContext`:* invents a
   URI syntax the format doesn't define and couples two independent mechanisms.

7. **A dangling or cyclic `$ref` is `UnresolvableReference`, atomically.** The
   read side validates all refs before any model mutation (as `kind_params`
   validates content bodies before touching the model, `reader.cpp:305-317`), so
   an unresolvable ref leaves `revision() == 0`. Rationale: reuses the reserved
   error kind (`reader.hpp:33`) and the atomic-failure discipline the reader
   already established. *Rejected — best-effort partial load (drop the bad
   edge):* silently corrupts the graph, violating the round-trip contract; doc 08
   makes references errors-as-values (Principle 5), not warnings.

8. **v1 `$ref` graphs are acyclic (DAGs); operator feedback-cycle serialization
   is deferred.** A `$ref` that closes an operator-input cycle is a read error
   (Constraint 4). Rationale: the v1 operator commitment is `fade`/`crossfade`
   (doc 13:201-205), both acyclic; a DAG lets the read side build bottom-up in
   one pass. Operator *feedback* (doc 13:134) and composition *cycles* (Droste,
   doc 05:54-74) are unaffected — feedback is a render-time concern bounded by the
   compositor's depth budget, and composition cycles ride the nested kind's
   `params` URI (Principle 3), neither of which is a `contents`/`$ref` edge.
   *Rejected — support `$ref` cycles now:* forces two-phase graph construction
   (build-all-nodes-then-wire) and a cycle-safe render path this task's 1d budget
   and the v1 operator set do not call for. Whether v1 ultimately needs
   serialized feedback cycles is a design-judgment item surfaced to
   `tasks/parking-lot.md`, **not** encoded as a WBS task (per the refinement
   brief's no-audit-task rule).

9. **No doc-00 decision-record bullet.** The Principle 6 delta is a
   format-detail fill *within* doc 08's own scope — consistent with `json_dep`
   D5, `writer` D6, `reader` D6, and `kind_params` D7, which each declined a
   doc-00 bullet for their doc-08/doc-10 format deltas. It pins *how* a table the
   doc already promised is keyed, not a project-shaping decision. *Rejected — add
   a doc-00 bullet:* doc 00 is for project-shaping decisions; this is a
   component-boundary detail doc 08 owns.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-09.

- Shipped L4 structural read/write of the operator graph: write walks `Content::inputs()` emitting an `inputs` array and hoists ≥2-referenced content into a document-level `contents` table with deterministic `{"$ref": id}` at each use site; read resolves `$ref` with intra-document dedup, builds input `Content*`s bottom-up, and wires edges atomically, erroring on dangling/cyclic refs.
- Extended `ContentSink` to return the live `Content*` alongside `ObjectId` (`src/serialize/arbc/serialize/reader.hpp`); `ContentBodyProvider` gained a `const Content&`-keyed metadata lookup (`src/serialize/arbc/serialize/writer.hpp`); `DeserializeFn` extended to receive built input `Content*`s (`src/serialize/arbc/serialize/deserialize.hpp`).
- `PlaceholderContent`'s `inputs` ctor param now populated on the read path (`src/serialize/arbc/serialize/placeholder_content.hpp`, `src/serialize/placeholder_content.cpp`); `identity()` returns a real input-0 pass-through over a parsed input.
- 4 unit/golden tests in `tests/serialize_sharing.t.cpp`: byte-exact inline-graph golden, byte-exact shared-`contents`/`$ref` golden + pointer-identity dedup assertion, dangling+cyclic `$ref` read-error (`revision() == 0`), placeholder pass-through over a parsed input with byte-equivalent re-serialize.
- Claims registered in `tests/claims/registry.tsv`: `08-serialization#inputs-array-round-trips`, `#shared-content-dedups-via-ref`, `#dangling-ref-is-read-error` (all `enforces:`-tagged); `#placeholder-renders-input-0-passthrough` re-enforced on the new parsed-input test.
- Existing `kind_params` goldens unchanged; the content-free writer overload remains byte-identical (Constraint 6).
- `docs/design/08-serialization.md` Principle 6 delta committed in the same change: omit-when-empty `inputs` rule, ≥2-refs hoisting + deterministic-ordinal id scheme + save canonicalization, `$ref` in any slot or layer content position, and v1 acyclic-`$ref` commitment.
- `arbc_contract` stays nlohmann-free; `check_levels`/`check_claims` green; `tests/CMakeLists.txt` wired `serialize_sharing` target.
