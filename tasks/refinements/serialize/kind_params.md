# serialize.kind_params — Kind param hooks + unknown-kind round-trip

## TaskJuggler entry

Back-link: [`tasks/60-serialize.tji:26-31`](../../60-serialize.tji) — `task kind_params`
inside `task serialize`.

> note "serialize()/deserialize() on kinds; placeholder content preserving
> kind/params/inputs verbatim, rendering pass-through of input 0;
> byte-equivalent re-serialization. Docs 08/13."

## Effort estimate

**2d** (`tasks/60-serialize.tji:27`), peer-weighted with `writer` and `reader`.
The day-count carries three pieces: (1) the **content-body codec seam** — the
`SerializeFn`/`DeserializeFn` pair, the serialize-owned codec table keyed by
kind id, and the `content_body_to_json`/`content_body_from_json` translation
functions that route by kind; (2) the **`PlaceholderContent`** type (unknown
kind, verbatim `kind`/`kind_version`/`params`/`inputs`, input-0 pass-through
render, byte-equivalent re-serialization); (3) the **L4 read/write wiring** —
threading the codec through `load_document` (currently `(void)`-discarding its
`registry`/`ctx`) and adding the content-body injection seam to the writer's
`layer_json`, plus the doc-08 Principle 1 delta the levelization forces. The
end-to-end model-integrated round-trip through `runtime::Document` is deferred
to a named L5 leaf (see Decisions 5 and Acceptance), which is why this task
stays bounded at 2d.

## Inherited dependencies

`kind_params` declares `depends !reader` (`tasks/60-serialize.tji:29`) and,
through its parent `task serialize` (`tasks/60-serialize.tji:4`), inherits
`depends contract.registry, model.journal`.

**Settled (formal `depends`, all Done — 2026-07-09):**

- **`serialize.reader`** ([`serialize/reader.md`](reader.md)) — landed the
  deserialization face this task fills the content half of. The exact seams it
  handed forward:
  - The **`DeserializeFn` signature seam** already exists at
    `src/serialize/arbc/serialize/deserialize.hpp:33-34`:
    `using DeserializeFn = std::function<expected<std::unique_ptr<Content>,
    ReaderError>(const nlohmann::json& params, LoadContext& ctx)>;`. The
    header's doc comment (`deserialize.hpp:21-32`) states "serialize.reader
    lands this SIGNATURE SEAM; serialize.kind_params (`!reader`) fills the
    per-kind bodies and wires the registry routing." `deserialize.hpp` is
    **internal** (not in `PUBLIC_HEADERS`) precisely because it names
    `nlohmann::json`, which is linked PRIVATE (`src/serialize/CMakeLists.txt`).
  - The reader entry point `expected<std::monostate, ReaderError>
    load_document(std::string_view json, const Registry& registry,
    LoadContext& ctx, Model& into)` (`src/serialize/arbc/serialize/reader.hpp:56-57`),
    whose `registry`/`ctx` are documented as "the seams the content half of
    the read path (serialize.kind_params / .sharing) threads through"
    (`reader.hpp:47-52`) and are currently `(void)`-discarded
    (`src/serialize/reader.cpp:229-230`).
  - The layer reconstruction path (`parse_layer`/`parse_composition`,
    `src/serialize/reader.cpp:180-219`) and `Model::load_baseline`
    (`src/serialize/reader.cpp:282-314`), which today **binds the invalid
    `ObjectId{}` placeholder for content** (`reader.cpp:294`) because no
    content body is reconstructed yet.
  - `LoadContext` (`src/serialize/arbc/serialize/load_context.hpp:49-87`) with
    `resolve()`/`resolved_uri()`/`load_asset()` and the `AssetSource` async
    hook — the object `DeserializeFn` receives so a kind's `params` reference
    strings resolve through the one choke point.
  - `ReaderError` (`src/serialize/arbc/serialize/reader.hpp:21-34`), whose
    kinds already reserve `MalformedField`/`MissingRequiredField`/
    `UnresolvableReference` for the content half.
- **`serialize.writer`** (transitive, [`serialize/writer.md`](writer.md)) —
  stood up the `arbc_serialize` component, the canonical emission engine, and
  the `SerializeError` errors-as-values shape
  (`src/serialize/arbc/serialize/writer.hpp:15-23`). It emits the envelope +
  composition + core-owned layer placement and **explicitly stops before the
  content body**, leaving `layer_json` (`src/serialize/writer.cpp:118-142`) as
  the injection seam this task extends (writer.md Decision 1). It flagged the
  two missing seams as `kind_params`'s to own: the kind's serialize hook, and a
  bridge from `ContentRecord.kind` (`std::uint64_t`,
  `src/model/arbc/model/records.hpp:60-63`) to the registry's reverse-DNS
  **string** id.
- **`contract.registry`** ([`contract/registry.md`](../contract/registry.md)) —
  the reverse-DNS kind id is the persistent serialization token
  (`03-layer-plugin-interface#registry-resolves-kind-id-to-factory-and-metadata`,
  `tests/claims/registry.tsv:182`); `Registry::factory(id)`/`metadata(id)`
  (`src/contract/arbc/contract/registry.hpp:61,64`) are the by-name lookup. The
  `ContentConfig = std::string_view` note (`registry.hpp:33-34`) anticipated "a
  serialization format gives it structure later" — this task realizes the
  structured route as the codec seam.
- **`model.journal`** (transitive) — the versioning/`load_baseline` machinery
  the reader installs into; unchanged here.

**No pending inherited dependencies** — every predecessor is Done.

**Downstream (this task unblocks):**

- `serialize.sharing` (`!kind_params`, `tasks/60-serialize.tji:32-37`) —
  operator `inputs` arrays and the document-level `contents`/`$ref` table on
  both read and write sides. This task delivers the placeholder's **verbatim
  preservation** of `inputs`; `sharing` owns the *interpretation*, nesting,
  `$ref` emission/resolution, and cross-file dedup.
- `serialize.format_tests` (`!kind_params`, `tasks/60-serialize.tji:38-43`) —
  the libFuzzer harness over the loader and the load→save determinism corpus.
  It fuzzes the content-body parse this task adds; the placeholder's verbatim
  round-trip is a determinism-corpus case.
- `runtime.document_serialize` (**new leaf this refinement registers**, M8) —
  the end-to-end integration that wires `runtime::Document` to the read/write
  seams this task lands (see Decision 5 / Acceptance).

## What this task is

Give the serialization format its **content body**: the per-layer
`{kind, kind_version, params}` object, and the routing that turns a body into a
live `Content` on load and back into canonical JSON on save. Concretely:

(a) A **content-body codec seam** owned by `arbc_serialize`: the read hook
`DeserializeFn` (already defined, `deserialize.hpp:33-34`) and a symmetric
write hook `SerializeFn` (`Content&` → the `params` JSON), held in a
serialize-owned **codec table keyed by kind id**. Concrete per-kind codecs are
registered into this table from a layer that can see both the kind's concrete
type *and* the JSON library — `runtime` (L5) for built-in kinds, a plugin's own
TU for out-of-tree kinds — never from `contract` (L2, which must not name
JSON). The routing consults the table; a kind with no registered codec falls
through to the placeholder.

(b) A **`PlaceholderContent`** type (a `Content` subclass internal to
`arbc_serialize`) that preserves an unknown kind's `kind`, `kind_version`,
`params`, and `inputs` **verbatim**, re-serializes them **byte-equivalent under
canonical formatting**, and renders **input 0 as pass-through** (via
`identity()` returning `0` when an input is present; a diagnostic fill
otherwise). A missing plugin never destroys data (doc 08 Principle 2) and never
punches a hole (doc 13; doc 08 Principle 6).

(c) The **L4 read wiring**: `load_document` stops discarding `registry`/`ctx`,
parses each layer's content body, dispatches by kind id through the codec table
(known → live `Content`; unknown → `PlaceholderContent`), and binds the
produced content through a caller-supplied **`ContentSink`** — replacing the
`ObjectId{}` placeholder bind at `reader.cpp:294`.

(d) The **L4 write wiring**: `serialize_document`/`layer_json` gain a
**content-body provider** seam that, given a layer's content, yields
`(kind_id, const Content&)`; the writer emits `{kind, kind_version, params}`
via the codec. A no-provider overload preserves the writer's current
content-body-free output and its existing goldens.

(e) A **doc-08 Principle 1 delta** reconciling the "`Content` grows
`serialize() -> json`" wording with the levelization the predecessors already
committed to (nlohmann PRIVATE to serialize; `deserialize.hpp` off the public
interface).

This task does **not** interpret the `inputs` graph, the `contents` table, or
`$ref` (→ `serialize.sharing`), and does **not** populate `runtime::Document`'s
content map or resolve `ContentRecord.kind` (`uint64`) from a pinned snapshot
(→ `runtime.document_serialize`). It lands the L4 primitives and seams both of
those consume.

## Why it needs to be done

The writer and reader made the *skeleton* of a document persist — envelope,
composition, bottom-to-top layer placement — but a layer with no content body
is an empty frame. `kind_params` is the task that first makes content
round-trip, and it is the hinge the rest of the serialize stream turns on:
`serialize.sharing` extends the content body with `inputs`/`contents`/`$ref`;
`serialize.format_tests` fuzzes the content-body parse and adds the placeholder
round-trip to the determinism corpus; `runtime.document_serialize` wires the
`ContentSink` and provider into `Document` for the end-to-end file→scene→file
loop. It is also where doc 08's load-bearing forward-compatibility promise —
"a missing plugin must never destroy data" (Principle 2) — and doc 13's "a
missing fade plugin degrades to an unfaded clip rather than a hole" first
become concrete, testable behavior rather than prose.

## Inputs / context

### Design docs (normative, doc 16)

- `docs/design/08-serialization.md:59-66` — **Principle 1**, *amended by this
  task's delta*: the core owns placement, kinds own `params`; the (de)serialize
  hooks are **serialize-owned codecs keyed by kind id**, registered from a layer
  that can see both the kind type and the JSON library (runtime/plugins), not
  JSON-typed methods on the `Content` interface. `LoadContext` supplies base-URI
  resolution and async asset loading.
- `docs/design/08-serialization.md:67-73` — **Principle 2**: "Unknown kinds
  round-trip losslessly." A file using a missing plugin "loads as a *placeholder
  content* that preserves the original `kind`, `kind_version`, and `params`
  verbatim, renders as a diagnostic placeholder, and re-serializes
  byte-equivalent (modulo formatting). A missing plugin must never destroy
  data." Also covers version skew (a kind "may choose placeholder behavior over
  lossy parsing"). The chartered heart of this task.
- `docs/design/08-serialization.md:80-83` — **Principle 4**: within a known
  major, unknown *fields* are "preserved-and-ignored (same discipline as unknown
  kinds)." The placeholder's verbatim body is where field-level preservation
  lives on the content side.
- `docs/design/08-serialization.md:84-96` — **Principle 5**: canonical output
  (sorted keys, shortest-round-trip numbers, non-finite → error value). This is
  the "modulo formatting" that makes "byte-equivalent re-serialization"
  well-defined: the placeholder re-emits its body *canonicalized*, not
  byte-for-byte against arbitrary input whitespace.
- `docs/design/08-serialization.md:97-103` — **Principle 6**: input edges are
  core-owned, live in an `inputs` array *beside* `kind`/`params`; "Unknown-kind
  placeholders preserve their inputs and may render input 0 as pass-through — a
  missing fade plugin degrades to an unfaded clip, not a hole." The `inputs`
  *interpretation* is `serialize.sharing`; the placeholder's verbatim
  *preservation* + pass-through render is this task.
- `docs/design/13-effects-as-operators.md` §*Serialization (amends doc 08)* —
  "a placeholder operator preserves `kind`/`params` *and* its inputs — and may
  even render input 0 as its pass-through." The operator contract's
  `virtual std::optional<std::size_t> identity(const RenderRequest&) const;`
  (the OpenFX "identity action") is the mechanism the placeholder's pass-through
  maps onto. Note both docs say **may** — pass-through is permitted, so this
  task pins it as the placeholder's *chosen* behavior (input present → 0), not a
  format-mandated invariant on every unknown kind.

### Source seams

- `src/serialize/arbc/serialize/deserialize.hpp:21-34` — the internal
  `DeserializeFn` seam and its "kind_params fills the bodies + wires the
  registry routing" charter. This task adds the symmetric `SerializeFn` and the
  codec table beside it (internal header; keeps `nlohmann::json` PRIVATE).
- `src/serialize/arbc/serialize/reader.hpp:21-57` — `ReaderError` (reuse
  `MalformedField`/`MissingRequiredField`/`UnresolvableReference` for the
  content half) and `load_document` (add the `ContentSink&` seam; stop
  `(void)`-discarding `registry`/`ctx`).
- `src/serialize/reader.cpp:180-219,229-230,282-314` — `parse_layer`/
  `parse_composition`, the `(void)registry;(void)ctx;` discards to remove, and
  the `load_baseline` closure where `reader.cpp:294` binds `ObjectId{}` today;
  the fuzz-hardened `num_or`/`bool_or`/`int_or` accessors (`reader.cpp:41-54`)
  to reuse for `params` field reads.
- `src/serialize/arbc/serialize/writer.hpp:15-45` — `SerializeError`
  (reuse/extend for a codec failure) and `serialize_document`; add a
  content-body-provider overload.
- `src/serialize/writer.cpp:118-142,158-165` — `layer_json` (the content-body
  injection point, after the placement fields) and the `for_each_layer_in`
  emission loop; the canonical dump at `writer.cpp:193-195`.
- `src/serialize/arbc/serialize/load_context.hpp:49-87` — `LoadContext` passed
  to every `DeserializeFn` for `params` reference resolution.
- `src/contract/arbc/contract/content.hpp:462-602` — the `Content` interface
  the codec operates over: `render()` (`:525-526`), `inputs()` (`:579`,
  returning `std::span<const ContentRef>`, `ContentRef = Content*` at `:212`),
  `identity()` (`:598`, the pass-through hook `PlaceholderContent` overrides).
  **`Content` has no `params`/`kind_id`/`serialize()` member today** — and per
  the doc-08 delta must not grow a JSON-typed one; the codec supplies these
  off-interface.
- `src/contract/arbc/contract/registry.hpp:33-70` — `ContentConfig`/
  `ContentFactory` (the config-string factory whose "structure later" note this
  task realizes), `factory(id)`/`metadata(id)` lookup, `ids()` enumeration.
- `src/model/arbc/model/records.hpp:48-63` — `ContentRecord { std::uint64_t
  kind; StateHandle state; }`: the kind is an **opaque `uint64` populated from
  the runtime side-map**, not the reverse-DNS string, and the params live in the
  opaque `StateHandle` (an index-only slab handle). The `uint64` ↔ kind-id
  bridge and pinned-state serialization are therefore **runtime-level**
  (Decision 5).
- `src/runtime/document.cpp:7-11,77-80` — `Document::add_content(shared_ptr<
  Content>)` / `Document::resolve(ObjectId) -> Content*`: the L5 content map
  (`d_contents`) the `ContentSink` and provider bind against downstream.
- `docs/design/17-internal-components.md:28,58,60` — levelization: `serialize`
  is **L4** ("JSON read/write, canonical form, unknown-kind placeholders,
  `LoadContext`, `$ref` resolution"), `kind-solid`/`kind-tone` are **also L4**
  (`:28`), `runtime` is **L5** ("`Document` … registry … loaders"). This is why
  concrete per-kind codecs cannot live at L4 (Decision 3).
- `tests/claims/registry.tsv:182-188` — the registry/serialize claim rows;
  `:187` already reserves "within a known major, unknown fields are
  preserved-and-ignored." This task appends its `08-*` rows after `:188`.
- `src/serialize/t/*.t.cpp`, `tests/serialize_*_golden.t.cpp` — the inline
  raw-string golden convention (no on-disk fixtures) this task follows.

**Predecessor / sibling refinements:** [`serialize/reader.md`](reader.md)
(the `DeserializeFn` seam, `ContentSink`-shaped content-half deferral,
`LoadContext`, `load_baseline`), [`serialize/writer.md`](writer.md) (the
`layer_json` injection seam, the `uint64`→reverse-DNS bridge deferral,
canonical formatting), [`contract/registry.md`](../contract/registry.md) (kind
id as persistent token).

## Constraints / requirements

1. **Content body is `{kind, kind_version, params}`; the core owns the frame,
   the kind owns `params`.** `kind` (reverse-DNS string) and `kind_version`
   (string) are emitted/read by the core routing; `params` is handed verbatim
   to the codec (doc 08 Principle 1). `kind_params` does **not** interpret
   `params` — its structure is the kind's private business.

2. **Unknown kinds round-trip losslessly and byte-equivalently.** A body whose
   `kind` has no registered codec deserializes to `PlaceholderContent` holding
   `kind`/`kind_version`/`params`/`inputs` verbatim; re-serializing it produces
   the **canonical** form of that body byte-for-byte (doc 08 Principles 2, 5).
   No field is dropped, reordered destructively, or coerced — canonicalization
   only sorts keys and normalizes number formatting.

3. **Placeholder rendering is input-0 pass-through, never a hole or a crash.**
   `PlaceholderContent::identity()` returns `0` when it has ≥1 input (the
   preserved `inputs` are surfaced through `inputs()`), so the compositor
   renders input 0 unchanged; with no input it renders a bounded diagnostic
   fill. A placeholder never faults on render (doc 08 Principle 6, doc 13).
   (Interpreting `inputs` into live `ContentRef`s is `serialize.sharing`; until
   then a placeholder with no bound inputs renders the diagnostic — the
   pass-through path is unit-tested with a synthetic input.)

4. **Per-kind codecs are serialize-owned, keyed by kind id, registered from
   L5/plugins.** The codec table lives in `arbc_serialize`; concrete codec
   bodies are registered by `runtime` (built-ins) or a plugin TU (out-of-tree)
   — never by `contract`, and never by a peer L4 component (`kind-solid`/
   `kind-tone` cannot depend on `serialize`, and `serialize` cannot depend on
   them). `Content` grows no JSON-typed method (doc-08 delta).

5. **No exceptions across the boundary (doc 10, doc 08 Principle 5).** Codec
   reads use the fuzz-hardened accessors and the non-throwing `nlohmann` parse
   the reader established; a malformed `params` value yields a `ReaderError`
   value; a codec that cannot serialize yields a `SerializeError` value. No
   `nlohmann` exception escapes on any input — the precondition
   `serialize.format_tests`' loader fuzzing relies on.

6. **The writer's existing output is unchanged when no provider is supplied.**
   The content-body-free `serialize_document(const DocRoot&)` overload stays
   byte-identical to today's goldens (`08-serialization#canonical-output-is-
   byte-stable` must not regress); the content body is emitted only through the
   provider overload.

7. **Levelization (doc 17), namespace/layout.** Code lives inside
   `arbc_serialize` (`DEPENDS contract model` + PRIVATE nlohmann); the codec
   table and `SerializeFn`/`PlaceholderContent` go in **internal** headers
   (naming `nlohmann::json`), not `PUBLIC_HEADERS`. `ContentSink` and the
   content-body provider (which do **not** name JSON) may be public.
   `scripts/check_levels.py` stays green with no edit — no new `arbc_*` edge.

8. **Diff coverage ≥ 90%** on changed lines (doc 16); the WBS gate
   `tj3 project.tjp 2>&1 | grep -iE "error|warning"` is silent after the
   closer lands `complete 100` + the refinement back-link.

## Acceptance criteria

- **Unknown-kind verbatim byte-equivalent round-trip (golden-backed).** A test
  builds a content body for a made-up kind
  (`{"kind":"com.example.gadget","kind_version":"3.0","params":{…nested…},
  "inputs":[…]}`), deserializes it through the routing to a `PlaceholderContent`,
  re-serializes via the codec, and asserts the output equals the **canonical**
  form of the input **byte-for-byte** (sorted keys, canonical numbers). Lands
  claim `08-serialization#unknown-kind-round-trips-verbatim` in
  `tests/claims/registry.tsv` with an `enforces:`-tagged test — the observable
  proof "a missing plugin never destroys data."

- **Placeholder input-0 pass-through render (behavioral).** A test renders a
  `PlaceholderContent` given one synthetic input and asserts the output equals
  rendering input 0 (via `identity() == 0`); with no input it asserts a bounded
  diagnostic fill and **no fault**. Lands claim
  `08-serialization#placeholder-renders-input-0-passthrough` with an
  `enforces:`-tagged test. Behavioral assertion (output equality / no crash),
  never wall-clock (doc 16).

- **Known-kind params round-trip via a registered codec.** A test registers a
  codec for a test kind into the codec table (a `SerializeFn`/`DeserializeFn`
  pair defined in the test TU, which can name both the test `Content` and
  `nlohmann::json` through the internal header), then round-trips a body
  `params` object through `content_body_from_json` → `Content` →
  `content_body_to_json` and asserts byte-equivalence, and that dispatch by
  kind id selects the codec (not the placeholder). Lands claim
  `08-serialization#known-kind-params-round-trip` with an `enforces:`-tagged
  test — pins the codec-seam dispatch that real kinds plug into.

- **Reader routing wires content into the model.** A component test drives
  `load_document` on a document whose layers carry content bodies (one known
  test kind, one unknown), asserts each layer binds a **non-`ObjectId{}`**
  content through a stub `ContentSink` (known → codec-built `Content`, unknown →
  placeholder), and that `registry`/`ctx` are consulted (no longer discarded).

- **Writer routing emits the content body.** A component test drives the
  content-body-provider overload of `serialize_document` (provider yielding
  `(kind_id, const Content&)` over test contents) and asserts the emitted layer
  gains `{kind, kind_version, params}` in canonical order; a second assertion
  confirms the no-provider overload output is unchanged (Constraint 6).

- **Errors-as-values on malformed `params`.** A test asserts a non-object
  `params`, a `params` a codec rejects, and a body missing `kind` each return a
  distinct `ReaderError`/`SerializeError` value with no `nlohmann` exception and
  no partial mutation.

- **Coverage / build / WBS gate.** ≥ 90% diff coverage; `-Werror -Wpedantic`
  and `scripts/check_levels.py` stay green; the closer confirms `tj3` silence
  after landing `complete 100` and the refinement back-link on
  `tasks/60-serialize.tji:26-31`.

- **Design-doc delta (same commit).** `docs/design/08-serialization.md`
  Principle 1 is amended (already written by this refinement) to relocate the
  (de)serialize hooks to serialize-owned kind-id-keyed codecs; the closer
  commits it with the code (doc 16 same-commit).

- **Deferred — one new WBS leaf, the rest on existing leaves.** The **new leaf**
  `runtime.document_serialize` (Decision 5) is the only registration the closer
  makes; `inputs`/`contents`/`$ref` interpretation goes to the existing
  `serialize.sharing` (`!kind_params`); loader fuzzing + the placeholder
  determinism-corpus case go to the existing `serialize.format_tests`; concrete
  built-in kind codecs ride in `runtime.document_serialize` and out-of-tree kind
  codecs land in each plugin's TU (no per-kind WBS leaf).

## Decisions

1. **The write hook is a serialize-owned `SerializeFn` codec, not a
   `Content::serialize() -> json` method — doc-08 delta.** Doc 08 Principle 1
   literally says the `Content` interface "grows two members: `serialize() ->
   json` …". Levelization forbids it: `Content` lives in `contract` (L2), which
   must not name `nlohmann::json` (kept PRIVATE to `arbc_serialize`, doc 10 /
   doc 17); the predecessors already relocated the *read* hook off-interface as
   the `DeserializeFn` `std::function` (`deserialize.hpp:33-34`). So the write
   hook symmetrically becomes a `SerializeFn = std::function<expected<
   nlohmann::json, SerializeError>(const Content&)>` in the same internal
   header, and both live in a serialize-owned codec table keyed by kind id. This
   is a genuine deviation from the doc's stated member list, so it lands a
   **doc-08 Principle 1 delta** (written by this refinement, committed by the
   closer). *Rejected — add `serialize() -> json` to `Content`:* forces
   `contract` (and every consumer of `libarbc`'s public interface) to depend on
   the JSON library, breaking the PRIVATE-nlohmann isolation `json_dep`/`writer`
   chose and the doc-17 levelization CI gate. *Rejected — a string-typed
   `Content::serialize() -> std::string`:* kinds would format their own JSON,
   defeating the canonical-formatting guarantee (Principle 5) and duplicating
   the JSON dependency across kinds.

2. **Kind dispatch is keyed on the reverse-DNS `kind` string, through a
   serialize-owned codec table (not the `contract::Registry`).** The read
   routing looks up `body["kind"]` in the codec table; a hit runs the codec, a
   miss builds a `PlaceholderContent`. The table is separate from
   `contract::Registry` because the Registry lives at L2 and cannot hold
   `std::function`s naming `nlohmann::json`. The Registry still supplies kind
   *metadata* (`kind_version` sanity, human name) where useful. *Rejected —
   store the codecs in `contract::Registry`:* same nlohmann-in-L2 collision as
   Decision 1. *Rejected — dispatch on the `ContentRecord.kind` `uint64`:* that
   handle is a runtime side-map index, not the persistent token; the reverse-DNS
   string is the format's contractual identity (doc 08:30, registry claim
   `:182`).

3. **Concrete per-kind codec bodies are registered from L5/plugins, not L4.**
   A codec for a concrete kind must see both the kind's concrete type (e.g.
   `SolidContent`, in `kind-solid`) *and* `nlohmann::json` (private to
   `serialize`). No L4 component can: `serialize` and `kind-solid`/`kind-tone`
   are peers at L4 (doc 17:28) and cannot depend on each other. Only `runtime`
   (L5, "depends everything below") and plugins (outside the level graph) can.
   So `kind_params` delivers the **table + seam**, and the built-in codec
   registrations land in `runtime.document_serialize` (Decision 5); out-of-tree
   kinds register in their plugin TU. The known-kind path is proven at L4 with a
   **test-registered codec** (Acceptance). *Rejected — implement the solid/tone
   codecs in `serialize`:* an L4→L4 dependency `check_levels.py` rejects.
   *Rejected — implement them in `kind-solid`/`kind-tone`:* those would then
   depend on `serialize` (L4→L4) and name the private JSON type — same
   violation.

4. **`PlaceholderContent` lives inside `arbc_serialize` and stores the verbatim
   body JSON; it renders input 0 as pass-through.** It is a `Content` subclass
   in an internal header (may name `nlohmann::json`), holding the raw
   `{kind, kind_version, params, inputs}` value; `serialize()`-side re-emission
   returns that value canonicalized; `identity()` returns `0` when an input is
   present (compositor renders input 0 unchanged), else a bounded diagnostic
   fill. Rationale: the placeholder is the one content type that is
   kind-agnostic and JSON-shaped, so `serialize` is its natural home; storing
   the raw value tree is what makes "verbatim, byte-equivalent" trivially true.
   *Rejected — represent the placeholder as a generic `contract` type carrying
   an opaque blob:* would push a JSON-shaped payload into L2 or invent a second
   serialization; the value already lives in `serialize`. *Rejected — make
   pass-through a mandatory invariant of every unknown kind:* both docs say
   "may" (08:101-103, doc 13) — pass-through is the placeholder's chosen,
   tested behavior, not a format guarantee, so version-skew kinds keep the
   freedom to choose diagnostic-only.

5. **The end-to-end model-integrated round-trip is deferred to a new L5 leaf,
   `runtime.document_serialize`.** Binding a loaded `Content` into
   `runtime::Document::d_contents` (`document.cpp:7-11`), resolving a layer's
   content on save from a **pinned** snapshot (`ContentRecord.kind` `uint64` →
   reverse-DNS string, then to the live/pinned `Content`), and registering the
   built-in kind codecs are all **L5** concerns `serialize` (L4) cannot own —
   `Document`, the runtime side-map, and the pinned-content-state discipline
   live in `runtime` (doc 17:60). This task lands the L4 seams (`ContentSink`,
   the content-body provider, the codec table); `runtime.document_serialize`
   implements the sink, supplies a snapshot-safe provider, registers built-in
   codecs, and adds the **full-document (with content bodies) load→save golden**.
   Crisp registration for the closer:
   - **id:** `runtime.document_serialize`
   - **effort:** 2d
   - **description:** "Wire `runtime::Document` to the serialize content seams:
     implement the reader's `ContentSink` (loaded bodies populate `d_contents`),
     supply the writer's content-body provider from a pinned snapshot (incl. the
     `ContentRecord.kind` `uint64` ↔ reverse-DNS bridge and pinned-content-state
     serialization), register built-in kind codecs (solid/tone), and add an
     end-to-end full-document load→save byte-exact golden."
   - **depends:** `!kind_params`, `runtime.threading`, `model.content_binding`
   - **milestone:** M8 (Persistence, `tasks/99-milestones.tji:63-64`)
   - **refinement home:** `tasks/refinements/runtime/document_serialize.md`
   This is concrete, agent-implementable wiring (a sink impl, a provider, a
   bridge, a golden) — not an audit. *Rejected — do the Document integration in
   this task:* it is L5 code `serialize` cannot reach, and pinned-snapshot
   content serialization is its own discipline; folding it in would blow the 2d
   budget and cross a level boundary. *Rejected — fold it into
   `runtime.host_objects`:* that leaf is scoped to viewport/transport/monitor
   objects, a different concern; a dedicated leaf keeps the dependency graph
   honest.

6. **`inputs` are preserved verbatim here but interpreted in
   `serialize.sharing`.** The placeholder captures and re-emits `inputs`
   byte-equivalently (doc 08 Principle 6 requires it — "preserve their inputs"),
   and surfaces them for the pass-through render, but this task does **not**
   parse the `inputs` array into live `ContentRef` edges, handle inline nesting,
   or resolve `contents`/`$ref`. Those are `serialize.sharing`'s charter
   (`tasks/60-serialize.tji:32-37`). Rationale: preservation is a prerequisite
   of the unknown-kind round-trip (can't drop `inputs` and stay lossless);
   interpretation is a separable, larger concern the WBS already isolates.
   *Rejected — interpret `inputs` here:* duplicates `sharing`'s scope and drags
   the `$ref`/dedup machinery into a task chartered for `params`.

7. **No doc-00 decision-record bullet.** The Principle 1 delta is a
   levelization/format-detail reconciliation *within* doc 08's own scope —
   consistent with `json_dep` Decision 5, `writer` Decision 6, and `reader`
   Decision 6, which each declined a doc-00 bullet for their doc-08/doc-10
   format deltas. It relocates *where* a hook the doc already promised lives, not
   *what* the format does. *Rejected — add a doc-00 bullet:* doc 00 is for
   project-shaping decisions; this is a component-boundary detail doc 08 owns.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-09.

- `src/serialize/arbc/serialize/codec.hpp`, `src/serialize/codec.cpp` — `SerializeFn`, serialize-owned `CodecTable` keyed by reverse-DNS kind id, `content_body_from_json`/`content_body_to_json` routing.
- `src/serialize/arbc/serialize/placeholder_content.hpp`, `src/serialize/placeholder_content.cpp` — verbatim-preserving `PlaceholderContent` (input-0 pass-through via `identity()`, bounded diagnostic fill when no input).
- `src/serialize/arbc/serialize/writer.hpp`, `src/serialize/writer.cpp` — `ContentBody`/`ContentBodyProvider` seam, content-aware `serialize_document` overload, `SerializeError` extended (`NoCodec`/`CodecFailed`); no-provider path byte-identical to goldens.
- `src/serialize/arbc/serialize/reader.hpp`, `src/serialize/reader.cpp` — `ContentSink`, content-aware `load_document` (routes bodies, binds via sink, threads `registry`/`ctx`); content-free 4-arg overload preserved.
- `src/serialize/CMakeLists.txt`, `tests/CMakeLists.txt` — build wiring for new sources.
- `tests/claims/registry.tsv` — three new claims: `08-serialization#unknown-kind-round-trips-verbatim`, `#known-kind-params-round-trip`, `#placeholder-renders-input-0-passthrough`.
- `tests/serialize_kind_params.t.cpp` — 6 test cases / 64 assertions covering round-trip, routing, pass-through render, errors-as-values.
- `docs/design/08-serialization.md` — Principle 1 delta: (de)serialize hooks are serialize-owned codecs keyed by kind id, not JSON-typed `Content` methods.
- Tech-debt follow-up registered: `runtime.document_serialize` (L5/M8) — wire `runtime::Document` to `ContentSink` + content-body provider, register built-in kind codecs, `ContentRecord.kind` uint64↔reverse-DNS bridge, end-to-end load→save golden.
