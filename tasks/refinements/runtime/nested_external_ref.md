# runtime.nested_external_ref — resolve a nested content's external `params.ref` through `LoadContext`

## TaskJuggler entry

[`tasks/65-runtime.tji:97-102`](../../65-runtime.tji) — `runtime.nested_external_ref`,
milestone **M8** (`m8_persistence`, `tasks/99-milestones.tji:63-66`). It is the
**last** leaf `m8_persistence` depends on, so the closer also marks the milestone
`complete 100`.

```
task nested_external_ref "Resolve external nested child through LoadContext" {
  effort 2d
  allocate team
  depends !nested_codec
  note "Resolve a kind-owned params.ref URI in a NestedContent's body through
        LoadContext's base-URI resolution and async AssetSource hook
        (load_context.hpp:49-87), loading the referenced .arbc as a child
        composition and deduping by resolved identity, so nested renders an
        external project. Source-of-debt: tasks/refinements/runtime/nested_codec.md
        Acceptance criteria (deferred follow-up). Docs 05/08."
}
```

## Effort estimate

**2d.** Budget:

- **~0.25d** — the `Content::external_composition_ref()` discovery accessor
  (`src/contract/arbc/contract/content.hpp`, beside `composition_ref()` at `:597`),
  `NestedContent`'s `std::string d_ref` + override, and the two-line writer
  suppression in `src/serialize/writer.cpp` (`:277-279`, `:316-329`) plus its
  `capture_snapshot` mirror (`src/runtime/document_serialize.cpp:184-267`).
- **~0.5d** — `serialize::load_composition()`: factor the composition-graph install
  out of `load_document` (`src/serialize/reader.cpp:587-665`) so it can install a
  document's compositions into an *existing* model under a **caller-supplied root
  `ObjectId`** and return it, without touching the model root. This is a refactor of
  code that already exists; the new part is the seeded root id.
- **~0.5d** — `ExternalCompositionLoader` (runtime): the resolved-URI → child-root
  `ObjectId` map, the eager-allocate-before-parse knot-cut, the depth cap, and the
  recursion that gives each loaded document its **own** `LoadContext` (its own base
  URI) while sharing the one loader-owned dedup map. Plus `FilesystemAssetSource`
  — the first `AssetSource` implementation in the tree — and the codec-closure
  registration that hands the loader to `deserialize_nested`.
- **~0.75d** — tests: the end-to-end external golden (real files, real
  `FilesystemAssetSource`), the dedup behavioural counter, the A→B→A cross-document
  cycle, the missing-file placeholder, the `composition`+`ref` rejection, the depth
  cap, a fuzz seed, the TSan lane, and four claims rows.

## Inherited dependencies

**Settled:**

- **`runtime.nested_codec`** (done 2026-07-11,
  [`nested_codec.md`](nested_codec.md)) — the direct predecessor and the
  source-of-debt. It shipped `src/runtime/codec_nested.cpp` with
  `deserialize_nested(json, LoadContext&, ObjectId composition, …)` already taking
  the `LoadContext&` **and ignoring it** (`codec_nested.cpp:64`, the parameter is
  spelled `LoadContext& /*ctx*/`). Its **Decision 1** established the invariant this
  task leans on hardest: a content answering a resolvable `composition_ref()` emits
  **no `inputs` array** and the writer **does not descend** its `inputs()`
  (`writer.cpp:277-279`, `:316-329`). Its **Decision 6** deliberately made
  `serialize_nested` emit `json::object()` rather than omitting `params`, so that a
  hand-authored `params.ref` survives today through the core's load-time residual
  diff — "what keeps an external-ref document readable-and-rewritable today, ahead
  of `runtime.nested_external_ref`" (`nested_codec.md:452-453`). **That freebie ends
  with this task**: once the codec *consumes* `ref`, `ref` stops being a residual
  and the codec must emit it explicitly (Constraint 4).
- **`serialize.compositions_table`** (done 2026-07-11,
  [`../serialize/compositions_table.md`](../serialize/compositions_table.md)) — the
  format and the machinery. Its **Decision 1** put the child reference behind a
  null-default discovery virtual on `Content` rather than a provider callback or a
  `Codec` field; this task adds its sibling and inherits that rationale wholesale.
  Its **Decision 4** is the trick this task reuses across documents: `CompResolver`
  pre-allocates every reachable composition's `ObjectId` with `Model::allocate_id()`
  **before any codec runs** (`reader.cpp:325-346`), which is what lets an in-document
  Droste back-edge resolve to an in-flight composition. Its **Decision 7** (BFS over
  compositions) is what keeps the walk cycle-safe.
- **`runtime.operator_codecs`** (done 2026-07-09, [`operator_codecs.md`](operator_codecs.md))
  — **Decision 2**, the codec-registration seam: one runtime-owned `CodecTable` keyed
  by kind id, built-in codecs living in **runtime (L5)** TUs because that is the only
  level that can see both a kind's concrete type and the JSON library. Its per-load
  registration path (`document_serialize.cpp`) is where this task binds the loader
  into the nested codec's closure without widening `DeserializeFn`.
- **`serialize.reader`** (done, [`../serialize/reader.md`](../serialize/reader.md))
  — shipped the whole `LoadContext` / `AssetSource` / `ResolvedRef` seam
  (`src/serialize/arbc/serialize/load_context.hpp`) and its **Constraint 6**
  ("`LoadContext` is the single resolution/loading choke point"), then deferred
  end-to-end external-`.arbc` dedup to exactly this leaf (`reader.md:328`). The
  claim `08-serialization#loadcontext-dedups-by-resolved-identity`
  (`tests/claims/registry.tsv:219`) is enforced today only at the unit level; this
  task is what finally enforces it end-to-end.
- **`kinds.nested`** / **`kinds.nested_runtime_binding`** — `NestedContent` itself.
  Not declared `depends` edges (M4/M9 work already on disk); consumed as facts:
  `nested_content.hpp:64` (`explicit NestedContent(ObjectId child)`), `:153`
  (`composition_ref()` override), `:254` (`ObjectId d_child` — the content's *entire*
  authored state), `:86` (`attach(PullService&, Backend&, NestedResolver, const DocRoot&)`).

**Pending:** none. Every seam this task needs is on `main`. Notably, **no
`AssetSource` implementation exists anywhere in the tree** — the interface has been
an unfilled extension point since `serialize.reader`, exactly as its header comment
predicted ("a filled-in loader lands with the kinds that first need it
(`org.arbc.raster`, `org.arbc.nested`)", `load_context.hpp:25-30`). This task writes
the first one.

## What this task is

Make a `NestedContent` whose body carries a kind-owned `params.ref` URI actually
load and render the `.arbc` project that URI names. Seven pieces:

(a) **Resolve** — `deserialize_nested` reads `params.ref`, hands it to
`LoadContext::resolve()` for base-URI joining and dedup (`load_context.hpp:59`,
`load_context.cpp:34-58`).

(b) **Fetch** — the resolved URI's bytes come through `LoadContext::load_asset()`
(`load_context.hpp:78`) and therefore through an `AssetSource`. This task ships the
first one, a `FilesystemAssetSource` in runtime.

(c) **Install** — those bytes are parsed as an `.arbc` document and its composition
graph is installed **into the host document's own `Model`** as an ordinary child
composition subtree, whose root composition `ObjectId` becomes the `NestedContent`'s
`d_child`. From that point every downstream system — render, audio, aggregate
revision, damage routing, tile caching — sees an in-document child and needs no
change whatsoever (Decision 1).

(d) **Dedup** — two references resolving to the same URI yield **one** child
composition, so doc 05's shared-content semantics survive persistence (doc 08
Principle 3, `08:82-87`). The witness is a behavioural counter: the `AssetSource` is
asked for the bytes **once**.

(e) **Terminate** — an external cycle (`a.arbc` → `b.arbc` → `a.arbc`, or a document
referencing *itself*) loads as a finite graph, because the loader allocates the child
root's `ObjectId` and records it in the dedup map **before** parsing the child's
bytes. A hostile non-cyclic chain is bounded by a load-time depth cap.

(f) **Preserve on save** — the writer must **not** hoist the external child into the
in-document `compositions` table. This is the one place the normative text has a
genuine hole (see the design-doc delta), and it is the task's only architectural
call: without it, opening a project and pressing save silently *inlines* the external
widget and destroys the reference.

(g) **Degrade** — no `AssetSource` installed, a missing file, or a depth-cap
overrun makes the reference *unavailable*: the nested content loads with a null
child and renders the placeholder (doc 05:50-52), the `ref` round-trips verbatim, and
the **parent load still succeeds**.

It is **not**: a truly async, deferred-arrival load (the `AssetSource` is driven to
completion within the load; a source that defers is deferred to
`runtime.async_external_load` — see Acceptance criteria). It is **not** URI *scheme*
dispatch — doc 08 Principle 3 says "v1 supports relative paths; the resolution hook
in `LoadContext` is where schemes (http, content stores) plug in later" (`08:82-87`),
and `has_scheme` already passes schemed and absolute references through verbatim
(`load_context.cpp:16-30`), which is all v1 owes.

## Why it needs to be done

**It is the last leaf of M8, and M8's own note promises it.** `m8_persistence`
("Documents as files") depends on nine leaves; eight are `complete 100`. Nothing else
delivers "external nested projects load", and doc 00 already records the decision as
made: "in-document child compositions in a core-owned `compositions` table (Droste
cycles serialize), **external references by URI**. Decided in doc 08."
(`00-overview.md:109-113`). The `LoadContext` seam has been sitting unconsumed since
`serialize.reader`; this is the leaf that consumes it.

**And it closes a live data-loss hazard.** Today a document may legally carry
`"params": {"ref": "widgets/gauge.arbc"}` on a nested body — `nested_codec` D6
deliberately made that round-trip through the residual diff, and
`src/runtime/t/nested_codec.t.cpp:116-160` pins it. But that document's nested
content has a **null** child, so nothing renders. The moment we make it render — i.e.
the moment `d_child` names a real composition — `writer.cpp:277-279`'s
`enqueue_composition(c->composition_ref())` starts returning `true` for it, and the
writer will happily walk the external project's contents into *this* document's
`contents` table and emit `"composition": "1"` in place of the URI. Load, save, and
the external reference is gone — replaced by a frozen copy. So (c) and (f) are not
separable: whoever makes external refs *render* must, in the same change, teach the
writer not to *inline* them.

## Inputs / context

### Design docs (normative, doc 16)

- **`docs/design/05-recursive-composition.md:47-52`** — "'Recursion into an entirely
  independent compose project' … is the same mechanism **plus a loader**; from the
  compositor's perspective there is **no difference between an inline child and an
  external one**. External loading is async by nature, and the nested kind's async
  render path plus placeholder policy already cover the not-yet-loaded state." This
  sentence is the whole warrant for Decision 1 (load the child into the host model,
  change nothing downstream) and Decision 6 (unavailable ⇒ placeholder, not a read
  error). **Amended by this task** — see the delta below.
- **`docs/design/05-recursive-composition.md:54-61`** — the persistence split: an
  in-document child rides the `compositions` table by core-owned id, "while an
  **external child stays a kind-owned `params.ref` URI resolved through the
  loader**."
- **`docs/design/05-recursive-composition.md:41-45`** — "content is shared, placement
  is per-instance … Layer-level tile caches inside the child are keyed by the child's
  content identities, so all embeddings share them". This is *why* dedup matters: two
  parents that load one `.arbc` twice would get two composition identities and two
  cold tile caches.
- **`docs/design/05-recursive-composition.md:66-79`** — cycles are legal and
  meaningful (Droste); termination is the sub-pixel cull, with a **per-request
  recursion-depth budget as a backstop; exceeding it renders the placeholder and
  reports a diagnostic naming the cycle**. Note this is a *render-time* budget — it
  says nothing about a *load-time* chain, which is the gap Decision 5 fills.
- **`docs/design/08-serialization.md:38-42`** — the spelling, verbatim from the
  document-shape example: `"params": { "ref": "widgets/gauge.arbc" }  // external
  project (doc 05)`.
- **`docs/design/08-serialization.md:72-74`** — "**`LoadContext` supplies base-URI
  resolution and async asset loading so kinds don't invent their own.**" The task's
  entire mechanism, in one sentence.
- **`docs/design/08-serialization.md:82-87`** — Principle 3, References not
  embedding: "Assets and nested projects are URIs resolved relative to the document.
  v1 supports relative paths … **Cross-file sharing (two parents referencing one
  child `.arbc`) deduplicates through the loader by resolved identity, so the doc-05
  shared-content semantics survive persistence.**" **Amended by this task.**
- **`docs/design/08-serialization.md:151-156`** — the cycle disambiguation: "composition
  cycles (Droste, doc 05) ride the `compositions` table (Principle 7) **or an external
  `params.ref` URI (Principle 3)** — neither is a `contents`/`$ref` edge." So an
  external cycle must **not** be rejected as an illegal `$ref` cycle.
- **`docs/design/08-serialization.md:181-184`** — "The kind-owned `params.ref` URI form
  remains the reference to an **external** project file (Principle 3, doc 05); the
  table is for in-document children."
- **`docs/design/08-serialization.md:192-212`** — Principle 7's second half
  (`nested_codec`'s delta): "a content that answers a **non-null, resolvable**
  `composition_ref()` emits no `inputs` array, and the write traversal does not
  descend its `inputs()`", and the corollary rule on kinds: "a kind names its child
  through `composition_ref()` or takes authored `inputs`, **never both**".
  **Amended by this task** — this is the sentence that, read literally against a
  loaded external child, inlines it.
- **`docs/design/08-serialization.md:184-190`, `:116-128`** — a dangling in-document
  `composition` id is "a serialization error surfaced as a value"; every failure is a
  value, never a throw, never a partial load (doc 10's errors-as-values).
- **`docs/design/03-layer-plugin-interface.md:113-120`** — the `composition_ref()`
  accessor and its doc comment ("core-visible graph structure exactly as `inputs()`
  is, but it names an `ObjectId` … so it rides its own null-default accessor").
  **Amended by this task** — its sibling lands beside it.
- **`docs/design/17-internal-components.md:41-64`** — levelization. The decisive
  facts: `serialize` (L4) and `kind-*` (L4) are **same-level, no edge** — serialize
  can never name `NestedContent`, which is why the codec must live in `runtime`
  (L5); `kind-*` depends on `contract (+ below)` and so reaches `pool`/`model`
  transitively but **never** `cache`/`backend-cpu`; `runtime` (L5) depends on
  everything below and its stated contents are "`Document` (arenas + model +
  registry + **loaders**)" (`17:60`) — loaders are runtime's, by name.

### Design-doc delta (this task, same commit — doc 16's rule)

Three docs change. Following `nested_codec`'s pattern, the refinement **specifies**
the delta and the implementation commit **applies** it — a doc that says "the core
reads `external_composition_ref()`" while no such accessor exists on `main` would be
false, so the edit rides with the code.

**1. `docs/design/03-layer-plugin-interface.md`**, immediately after
`composition_ref()` (`:113-120`) — add the sibling accessor:

```cpp
  // When a nested content's child composition was loaded from an external
  // project file (doc 05), the child is an ordinary composition in this
  // document's model — but it is not this document's DATA. The kind answers
  // the authored reference here so the serializer knows to name the child by
  // that URI instead of emitting it into the compositions table (doc 08
  // Principle 3). Empty means "the child, if any, is document-local" — the
  // default, and the answer for every kind but nested.
  virtual std::string_view external_composition_ref() const { return {}; }
```

**2. `docs/design/05-recursive-composition.md:47-61`** — make the "plus a loader"
sentence say *where the loaded child lands*, and state the load-time termination rule
the render-time depth budget does not cover:

> The loaded child is installed as an ordinary composition in the **host document's
> model**, so render, aggregate revision, damage routing and tile caching see no
> difference between an inline child and an external one — the "plus a loader" is the
> whole of the difference. Load-time termination of an external cycle (A embeds B
> embeds A; a document embedding *itself*) is guaranteed the same way the in-document
> Droste knot is cut (doc 08 Principle 7): the loader records the child's composition
> id in its resolved-identity map *before* parsing the child's bytes, so a back-edge
> resolves to the in-flight composition instead of re-loading it. A non-cyclic
> external chain is bounded by a load-time depth cap; exceeding it, like a missing or
> unreadable file, makes the reference **unavailable** — the embedding content keeps
> its `ref` and renders the placeholder.

**3. `docs/design/08-serialization.md`** — the substantive one, in two places.

*Principle 3 (`:82-87`)* gains the authored-vs-resolved rule and the unavailability
rule:

> Dedup is keyed on the **resolved** URI; the **authored** reference is what
> round-trips — a document that says `widgets/gauge.arbc` saves back saying
> `widgets/gauge.arbc`, never an absolutised path, so a project directory stays
> relocatable and the output stays byte-stable. A reference that cannot be loaded —
> no asset source installed, a missing or unreadable file, a depth-cap overrun — is
> reported as **unavailable**, not as a read error: the embedding content loads with
> no child, keeps its reference verbatim, and renders the placeholder (doc 05). The
> asymmetry with a dangling in-document `composition` id (Principle 7, a read error)
> is deliberate — self-inconsistent bytes are a malformed document, whereas a missing
> external file is a condition of the environment that may resolve later, and doc 05
> already assigns that state the placeholder.

*Principle 7 (`:192-212`)* has its write-side rule qualified — this is the hole:

> The rule is a rule about *document-local* children. A content that answers a
> non-empty `external_composition_ref()` (doc 03) names its child by that URI: the
> core emits **neither** an `inputs` array **nor** a `composition` field for it, and
> the write traversal descends **neither** its `inputs()` nor its child composition —
> the child's contents belong to the other document and must not be copied into this
> one's `contents` table. The reference itself rides the kind's `params` (Principle
> 3), which is the one thing the core does not own. The third corollary on kinds
> follows: a body carrying **both** a core-owned `composition` and a kind's external
> `params.ref` names one child two contradictory ways and is a serialization error
> surfaced as a value on load — rejecting it beats silently preferring one.

**No doc-00 decision bullet.** `00-overview.md:109-113` already records "external
references by URI. Decided in doc 08"; every delta above is a mechanism inside that
recorded decision, not a project-shaping choice. Consistent with `nested_codec` D7
and `compositions_table` D9.

### Source seams

| seam | file:line | change |
| --- | --- | --- |
| the discovery accessor | `src/contract/arbc/contract/content.hpp:597` | **add** `virtual std::string_view external_composition_ref() const { return {}; }` beside `composition_ref()` |
| the kind's state | `src/kind_nested/arbc/kind_nested/nested_content.hpp:64,153,254` | second ctor `NestedContent(ObjectId child, std::string ref)`; `std::string d_ref`; the override; a `ref()` accessor for the codec |
| null-child tolerance | `src/kind_nested/nested_content.cpp:288,394,507,736` | `render` / `render_audio` / `inputs()` must be well-behaved when `d_child == ObjectId{}` (unavailable ⇒ nothing to compose) |
| writer traversal skip | `src/serialize/writer.cpp:277-279` | check `external_composition_ref()` **first**: non-empty ⇒ return without descending `inputs()` and without `enqueue_composition` |
| writer emission skip | `src/serialize/writer.cpp:316-329` | non-empty external ref ⇒ emit neither `composition` nor `inputs` |
| snapshot-walk mirror | `src/runtime/document_serialize.cpp:184-267` (`capture_snapshot`) | the same skip, so the pinned-write path agrees with the live path |
| the codec | `src/runtime/codec_nested.cpp:47-52` (`serialize_nested`), `:64` (`deserialize_nested`) | emit `{"ref": …}` when the ref is non-empty; consume `params.ref` on load, `LoadContext& ctx` stops being `/*ctx*/` |
| composition-subtree install | `src/serialize/reader.cpp:325-405` (`CompResolver`), `:587-665` (`load_document`) | **new** `load_composition(json, Registry&, …, LoadContext&, ObjectId seeded_root) -> expected<ObjectId, ReaderError>`: the same graph install, into an existing model, under a caller-supplied root id, leaving the model root alone |
| the loader | `src/runtime/arbc/runtime/external_composition_loader.hpp` + `.cpp` (**new**) | resolved-URI → child-root `ObjectId` map, eager allocate-before-parse, depth cap, per-document `LoadContext` construction |
| the first asset source | `src/runtime/arbc/runtime/filesystem_asset_source.hpp` + `.cpp` (**new**) | `AssetSource` reading the resolved URI as a filesystem path (`file://` prefix stripped); fires `on_ready` inline, empty bytes on any failure |
| loader injection | `src/runtime/document_serialize.cpp` (per-load codec registration) | bind the loader into `deserialize_nested`'s closure at load-codec-table build time — **no `DeserializeFn` signature change** |
| base URI | `src/runtime/arbc/runtime/document_serialize.hpp:130` | `load_document` gains a `base_uri` (overload or defaulted param) so relative refs have something to resolve against |
| build wiring | `src/runtime/CMakeLists.txt`, `tests/CMakeLists.txt` | two new runtime TUs; new test targets link the umbrella + `nlohmann_json` |

Reference points that need **no** change: `PlaceholderContent` (an unknown kind's
`params.ref` is still preserved verbatim by the residual diff, its
`external_composition_ref()` defaults empty, and it is never asked to load anything —
so a document whose nesting kind is missing keeps its external reference intact,
exactly as `compositions_table` D6 keeps its in-document one); `SerializeFn` (does not
grow — `operator_codecs` D3, `compositions_table` D5); `DeserializeFn` (does not grow —
the loader arrives by closure capture); `Codec`/`CodecTable`; `Transaction::add_composition`
(reused as-is); `model`, `pool`, `compositor`, `cache`. **No new levelization edge and
no new third-party dependency (doc 10)** — `<filesystem>`/`<fstream>` are stdlib.

### Tests / claims

- `tests/nested_codec_golden.t.cpp` — the L5 model to follow (Catch2, umbrella target
  + `nlohmann_json`, real `Document` → `save_document` → inline `R"json(…)"` byte
  comparison → `load_document` → re-save → byte-identical). Its header explains why
  cross-component nested tests live in `tests/` and not `src/runtime/t/`: the
  attach-invariance proof needs a real `CpuBackend` and `OperatorBindingScope`, which
  a runtime component test may not name under `scripts/check_levels.py`. The same
  applies here.
- `src/runtime/t/nested_codec.t.cpp:116-160` — the test that today pins `params.ref`
  surviving as an *unconsumed residual*. **It must be rewritten, not deleted**: the
  same document must now round-trip with `ref` as a *consumed* param, and the
  `vendor_tag` beside it must still ride the residual diff.
- `tests/claims/registry.tsv:219` — `08-serialization#loadcontext-dedups-by-resolved-identity`,
  enforced today only at the `LoadContext` unit level. This task re-enforces it
  end-to-end. `:230` — `loader-never-faults-on-hostile-input`. `:236` —
  `droste-cycle-round-trips-as-data`. `:239-241` — the three nested-codec claims.
- `tests/document_serialize_concurrency.t.cpp` — the TSan lane (`nested_codec` added
  the save-under-live-binding case).
- `tests/fuzz/corpus/load_document/` + `tests/fuzz_corpus_replay.t.cpp` — the
  hostile-input replay. With no `AssetSource` installed, the fuzz lane exercises the
  unavailable path by construction; that is the safe default and worth a seed.

## Constraints / requirements

1. **The external child is an ordinary composition in the host document's model.**
   Not a second `Model`, not a second `Document`. `NestedContent::attach` takes a
   single pinned `const DocRoot&` (`nested_content.hpp:86`) and reads the child's
   layers out of it; `ObjectId`s are per-model monotonic, so an id from a foreign
   model would either dangle or — worse — silently alias an unrelated composition in
   this one. Doc 05:47-52 is explicit that the compositor must see no difference.
2. **The codec owns `params`; the core owns structure.** `serialize_nested` emits
   `ref` inside `params` and nothing else; the core emits neither `composition` nor
   `inputs` for an external child. The core learns *that* the child is external from
   `Content::external_composition_ref()` — a read-only discovery accessor, never a
   write channel (doc 08 P1, `08:59-74`; `compositions_table` D1).
3. **`SerializeFn` and `DeserializeFn` do not grow.** The loader reaches the nested
   codec by closure capture at load-codec-table registration time, the same way
   `operator_codecs` D2's per-load registration already threads runtime state into
   codecs. A structural seam that only one kind needs does not belong in the
   signature every kind implements.
4. **The authored reference round-trips verbatim; the resolved URI is only the dedup
   key.** `save(load(bytes)) == bytes` for a document with a relative `ref`. Storing
   the absolutised URI would break byte-stability
   (`08-serialization#canonical-output-is-byte-stable`, `registry.tsv:214`) and make
   a project directory non-relocatable.
5. **Dedup is by resolved URI, not by `ResolvedRef`.** `ResolvedRef` is "an index into
   the **owning** `LoadContext`'s resolution table" (`load_context.hpp:19-23`) — it is
   not comparable across contexts, and this task creates one `LoadContext` per loaded
   document (each needs its own base URI, because a child's refs resolve relative to
   *the child*, doc 08 P3). The loader's map is therefore keyed on the resolved URI
   *string*, which is global and base-independent.
6. **Load-time cycle termination is allocate-before-parse.** The loader inserts
   `resolved-URI → child-root ObjectId` into its map **before** parsing the child's
   bytes, using an id obtained from `Model::allocate_id()` and handed to
   `load_composition` as the seeded root. This is `compositions_table` D4's knot-cut,
   applied across documents. The top-level document seeds the map with its **own** base
   URI → its own root composition id, so a document that references *itself* dedups to
   its own root — a cross-document Droste becomes the in-document Droste, exactly.
7. **Unavailable is not an error.** No `AssetSource`, a missing/unreadable file, a
   depth-cap overrun, or bytes that fail to parse as an `.arbc` ⇒ the nested content
   loads with a **null** child, keeps its `ref`, renders the placeholder, and the
   **parent load succeeds** (doc 05:50-52). This does not contradict `nested_codec`
   D3 ("a missing `composition` is `MissingRequiredField`"): that rejects a body that
   names **no** child at all — silent data loss — whereas here the `ref` is present,
   preserved, and re-saved byte-identically. Nothing is lost; the file simply is not
   there *right now*.
8. **A body carrying both `composition` and `params.ref` is rejected.** One child, two
   contradictory names. Because `params` is kind territory, the check lives in
   `deserialize_nested` and surfaces through the codec's existing failure value — the
   core reader cannot make it, since it may not read `params` semantics. Mirrors
   `nested_codec` D2 and `reader.cpp:465-473`.
9. **Save output must not depend on binding state, or on load state.** Inherited from
   `nested_codec` Constraint 6: a live `OperatorBindingScope` must not change one
   byte. Extended here: whether the external child's bytes were successfully loaded
   must not change one byte either — a document saved with the widget file missing is
   byte-identical to the same document saved with it present.
10. **Loading is single-threaded.** `LoadContext` is "single-writer, not thread-safe:
    a load runs on one thread" (`load_context.hpp:47-48`). The loader inherits that;
    it is not a shared service and is not stored on the `Document`.
11. **No new levelization edge, no new dependency.** `scripts/check_levels.py` must
    stay silent. `serialize` (L4) still never names `NestedContent`; `kind_nested`
    (L4) still never names `LoadContext` — it only holds a `std::string`.
12. **The reader never faults on hostile input.** The depth cap and the
    parse-failure-is-unavailable rule exist as much for fuzz hardening as for
    correctness (`08-serialization#loader-never-faults-on-hostile-input`,
    `registry.tsv:230`).

## Acceptance criteria

- **`tests/nested_external_ref_golden.t.cpp`** (new, L5, cross-component) — the
  end-to-end proof, driven by the real `FilesystemAssetSource` over a temp directory:
  - a parent `.arbc` with `"params": {"ref": "child.arbc"}` beside a real `child.arbc`
    loads, and the parent's `NestedContent` **renders the child's pixels** — a
    byte-exact frame golden against the same scene authored in-document. This is the
    milestone promise ("external nested projects load") and pins a new claim
    **`05-recursive-composition#external-nested-loads-through-loadcontext`**
    *(new claim)*.
  - the same document **saves back byte-identically**, with `"ref": "child.arbc"`
    intact, **no `compositions` key**, and the child's contents **absent** from
    `contents`. Pins **`08-serialization#external-composition-ref-round-trips`**
    *(new claim)*. Re-enforces `08-serialization#canonical-output-is-byte-stable`
    (`registry.tsv:214`).
  - **attach invariance**: the same save taken while an `OperatorBindingScope` is live
    is byte-identical (Constraint 9). Re-enforces
    `08-serialization#nesting-inputs-are-derived-not-persisted` (`registry.tsv:240`).
- **`tests/nested_external_ref.t.cpp`** (new, L5, cross-component, driven by an
  in-memory `AssetSource` test double holding a URI→bytes map — no temp files, fully
  deterministic):
  - **Dedup behavioural counter** — two nested contents referencing `child.arbc` and
    `./child.arbc` produce **one** child composition `ObjectId`, and the test double's
    `request()` is called **exactly once**. A behavioural counter, not a wall-clock
    assertion (doc 16). Re-enforces
    **`08-serialization#loadcontext-dedups-by-resolved-identity`**
    (`registry.tsv:219`) — the end-to-end enforcement `reader.md:328` deferred here.
  - **Cross-document cycle** — `a.arbc` → `b.arbc` → `a.arbc` loads as a finite graph
    (`request()` called exactly twice, once per file), `b`'s nested child resolves to
    `a`'s **root** composition, and the graph renders bounded by the sub-pixel cull.
    A **self**-reference (`a.arbc` → `a.arbc`) resolves to `a`'s own root. Pins a new
    claim **`05-recursive-composition#external-cycle-terminates-at-load`**
    *(new claim)*. Re-enforces `08-serialization#droste-cycle-round-trips-as-data`
    (`registry.tsv:236`).
  - **Unavailable ⇒ placeholder** — a `ref` naming a file the source does not have,
    and the same document loaded with **no** `AssetSource` installed at all: the
    parent load **succeeds**, the nested content has a null child and renders the
    placeholder, and the document re-saves with the `ref` verbatim. Pins a new claim
    **`05-recursive-composition#unresolvable-external-ref-renders-placeholder`**
    *(new claim)*.
  - **Depth cap** — a chain `d0.arbc` → `d1.arbc` → … deeper than the cap loads
    without faulting; the content at the cap has a null child and its `ref` preserved.
  - **Bad bytes** — a `ref` whose target is not a valid `.arbc` is unavailable, not a
    parent-load failure.
- **`src/runtime/t/nested_codec.t.cpp`** (extend) — `deserialize_nested` with a
  `params` carrying **both** a `ref` and a body `composition` fails as a value; a
  `params` carrying `ref` **plus** an unknown `vendor_tag` consumes the former and
  preserves the latter through the residual diff (the rewrite of the existing
  `:116-160` case). Pins a new claim
  **`08-serialization#composition-and-ref-is-read-error`** *(new claim)*, and
  re-enforces `08-serialization#known-kind-params-unknowns-preserved`
  (`registry.tsv:234`).
- **`src/serialize/t/`** (extend) — `load_composition` installs a composition subtree
  under a caller-seeded root id without disturbing the model root, and leaves the
  model unmutated on error.
- **Fuzz seed** — `tests/fuzz/corpus/load_document/external_ref.arbc`: a nested body
  with a `params.ref`, replayed through `fuzz_corpus_replay.t.cpp` with no
  `AssetSource` installed. Re-enforces
  `08-serialization#loader-never-faults-on-hostile-input` (`registry.tsv:230`).
- **Claims register** — **five** new rows in `tests/claims/registry.tsv`, each named by
  an `// enforces: <claim-id>` tag above the pinning `TEST_CASE`:
  `05-recursive-composition#external-nested-loads-through-loadcontext`,
  `05-recursive-composition#external-cycle-terminates-at-load`,
  `05-recursive-composition#unresolvable-external-ref-renders-placeholder`,
  `08-serialization#external-composition-ref-round-trips`,
  `08-serialization#composition-and-ref-is-read-error`. Five further claims are
  **re-enforced with a second `enforces:` tag and no new row**: `:214`
  (`canonical-output-is-byte-stable`), `:219`
  (`loadcontext-dedups-by-resolved-identity`), `:230`
  (`loader-never-faults-on-hostile-input`), `:236`
  (`droste-cycle-round-trips-as-data`), `:240`
  (`nesting-inputs-are-derived-not-persisted`).
- **Concurrency** — extend `tests/document_serialize_concurrency.t.cpp`'s TSan lane
  with a save taken while a binding is live on a document holding an **externally**
  loaded child (Constraint 9's second half). Loading itself is single-threaded
  (Constraint 10) and gets no concurrency lane.
- **Gates** — `scripts/check_levels.py` silent; `scripts/check_claims.py` silent;
  ≥90% diff coverage on changed lines; `-Werror -Wpedantic` clean; full suite green.
- **Milestone** — this is the last leaf of `m8_persistence`; the closer marks the
  milestone `complete 100` in `tasks/99-milestones.tji` (README ritual step 3).
- **Deferred to `runtime.async_external_load`** (closer registers in WBS, **effort
  2d**, `depends runtime.nested_external_ref`, wired into **`m9_release`** — the
  milestone that carries the post-M8 runtime wiring work): drive a **deferring**
  `AssetSource` — one whose `on_ready` fires after `load_document` has already
  returned — installing the child composition on a later model revision and damaging
  the embedding content so the placeholder is replaced live. Doc 05:47-52 ("external
  loading is async by nature") and doc 03's async render path. **This is not new
  scope creep**: v1 satisfies M8's "external nested projects load" with a source
  driven to completion inside the load, and doc 05 already assigns the not-yet-loaded
  state to the placeholder — which this task implements. What `async_external_load`
  adds is the *arrival* edge, which needs a revision bump and a damage route, i.e.
  runtime wiring, not serialization.

## Decisions

1. **The external child is loaded into the host document's own `Model`, as an
   ordinary composition; "external" is provenance, not a different runtime
   representation.** The `NestedContent` that names it holds a plain child `ObjectId`,
   exactly as an in-document one does, and every downstream system — `render`
   (`nested_content.cpp:394`), `render_audio` (`:736`), `inputs()` (`:258`), the
   aggregate-revision memo (`:179-200`), damage routing, tile caching — is untouched.
   This is doc 05:47-52 taken at its word: "the same mechanism **plus a loader**; from
   the compositor's perspective there is **no difference**". It is also forced: `attach`
   takes one pinned `const DocRoot&` (`nested_content.hpp:86`) and resolves `d_child`
   inside it.

   *Rejected — load the child into its own `Model`/`Document`, held by the parent in a
   loaded-children cache:* superficially tidier (the external document stays a
   document), and fatal in two ways. `ObjectId`s are per-model monotonic, so a child
   id from a foreign model would collide with an unrelated composition in the parent's
   — `writer.cpp:237-249`'s `enqueue_composition` resolves ids against *this*
   document's records and would happily match the wrong one. And nested's entire
   render/audio/revision story pins one `DocRoot`; a second model means a second pin,
   a second revision line, and a second damage domain. That is a redesign of doc 05,
   not an implementation of it.

2. **The core learns that a child is external through a new null-default discovery
   accessor, `Content::external_composition_ref() -> std::string_view`, beside
   `composition_ref()`.** The writer needs a per-content signal, and it cannot get one
   any other way: `serialize` (L4) may not name `NestedContent` (same-level as
   `kind_nested`, doc 17:41-44), so no `dynamic_cast`; and the codec runs *after* the
   core has already decided the body's structure. This is verbatim the shape
   `compositions_table` D1 chose for `composition_ref()` itself — one null-default
   virtual on L3, no new `std::function` seam, no `ContentBody`/`ContentMeta` change,
   and `capture_snapshot`'s walk stays kind-agnostic.

   *Rejected — a `bool composition_is_external()` predicate instead of the URI:* the
   writer only needs the boolean, but then the URI has to reach `serialize_nested`
   through a *second* channel anyway, and two accessors that must agree is worse than
   one that cannot disagree. The string is also the honest thing: the core can name the
   URI in a diagnostic.

   *Rejected — put the external origin on the `CompositionRecord` (model, L2) and have
   the writer ask the model:* it puts provenance in the right conceptual place, but
   `serialize_nested` receives only `const Content&` and could not reach the model to
   get the URI back out — so `SerializeFn` would have to grow a context parameter,
   which `operator_codecs` D3 and `compositions_table` D5 both refused, for every kind,
   to serve one.

   *Rejected — a `CompositionRefFn`/`ExternalRefFn` `std::function` on
   `serialize_document`:* `compositions_table` D1 already litigated and rejected this
   exact shape ("a structural edge the core re-derives does not belong behind the
   kind-owned seam").

3. **Dedup is keyed on the resolved URI string, held in a runtime-owned
   `ExternalCompositionLoader`; each loaded document gets its own `LoadContext`.** A
   child's relative refs resolve against *the child's* base URI (doc 08 P3, "resolved
   relative to the document"), so a single `LoadContext` spanning the whole load tree
   would resolve `b.arbc`'s references against `a.arbc`'s directory — wrong. But
   `ResolvedRef` is explicitly "an index into the **owning** `LoadContext`'s resolution
   table" (`load_context.hpp:19-23`) and so cannot be compared across contexts. The
   only identity that is both global and base-independent is the resolved URI string
   itself, which is exactly what `LoadContext::resolved_uri()` (`:62`) hands back. The
   loader owns the map; each `LoadContext` owns its base.

   *Rejected — one `LoadContext` for the whole load tree, resolving everything against
   the root document's base:* breaks any external project that itself references a
   sibling by a relative path — i.e. any real project directory.

   *Rejected — teach `LoadContext` to spawn child contexts sharing one resolution
   table:* a bigger change to an L4 type that four other codecs already depend on, to
   buy a map the L5 loader can just own. The existing `resolved_uri()` accessor is all
   the seam that is needed.

4. **The authored reference is what round-trips; the resolved URI is only the key.**
   `NestedContent` stores the string the document said (`widgets/gauge.arbc`), and
   `serialize_nested` emits that string back. Two nested contents may therefore share
   one child composition while carrying *different* authored strings (`./x.arbc` and
   `x.arbc`) — which is fine and correct: the suppression is per-content and there is
   no table entry for them to disagree about.

   *Rejected — store and re-emit the resolved absolute URI:* it makes the output depend
   on where the project directory happens to sit on disk, breaking
   `08-serialization#canonical-output-is-byte-stable` (`registry.tsv:214`) and making a
   project non-relocatable. The whole point of relative refs is that the directory
   moves as a unit.

5. **Load-time termination is allocate-before-parse, plus a depth cap for the
   non-cyclic pathological case.** On first encountering a resolved URI, the loader
   takes an `ObjectId` from `Model::allocate_id()`, records `URI → id` in its map, and
   *then* parses the child's bytes, handing that id to `load_composition` as the seeded
   root. A back-edge (`b.arbc` → `a.arbc`) hits the map and gets `a`'s in-flight root
   immediately, so the recursion is finite by construction. This is precisely
   `compositions_table` D4's knot-cut (`reader.cpp:325-346`) lifted from
   within-document to across-document. The top-level document seeds its own base URI →
   its own root composition id, so a self-referencing document collapses onto the
   in-document Droste case exactly. The depth cap (**64**) exists only for a hostile
   *acyclic* chain, which dedup cannot bound and which would otherwise overflow the C++
   stack.

   *Rejected — a load-time cycle *detector* (an in-progress set, error on re-entry):*
   it would reject a legal Droste, which doc 08:151-156 explicitly protects
   ("composition cycles … ride the `compositions` table **or an external `params.ref`
   URI** — neither is a `contents`/`$ref` edge") and `registry.tsv:236` pins. Cycles
   are data, not errors.

   *Rejected — no depth cap, relying on dedup alone:* dedup bounds *cycles*, not a
   10,000-link chain of distinct files, which a fuzz corpus or a malicious project can
   produce and which would fault the loader —
   `08-serialization#loader-never-faults-on-hostile-input` (`registry.tsv:230`) forbids
   that.

6. **An unresolvable external reference is *unavailable*, not a read error: null child,
   `ref` preserved, placeholder rendered, parent load succeeds.** Doc 05:50-52 already
   assigns the not-yet-loaded state to "the nested kind's async render path plus
   placeholder policy", and `LoadContext::load_asset` already *reports* absence rather
   than failing (`load_context.cpp:60-66`: with no source installed it fires `on_ready`
   with empty bytes). Making a missing widget file fail the parent load would mean a
   project becomes unopenable because a file moved — the opposite of doc 08 Principle
   2's stance, which keeps a document loadable even when the *kind* is missing.

   *Rejected — `UnresolvableReference`, symmetric with a dangling in-document
   `composition` id (`registry.tsv:238`):* the symmetry is superficial. A dangling
   in-document id means the bytes are **self-inconsistent** — the document lies about
   its own contents, and no environment can make it true. A missing external file means
   the bytes are fine and the **environment** is incomplete; it may be complete again in
   a second. Doc 08's delta records the asymmetry explicitly rather than leaving a
   reader to wonder.

   *Rejected — synthesize a `PlaceholderContent` in place of the `NestedContent`:*
   placeholder-the-content is for an unknown **kind** (doc 08 P2); here the kind is
   perfectly well known and its reference must stay a live, editable, re-resolvable
   `ref` — a user who restores the missing file and reloads must get their scene back.
   Nested renders the placeholder *itself*, which is what doc 05:75-79 already has it do
   when the depth budget is exceeded.

7. **A body carrying both a core-owned `composition` and a kind's external
   `params.ref` is a serialization error surfaced as a value.** The third corollary of
   Principle 7's "never both" rule, and the check lives in `deserialize_nested` — not
   in the core reader's `RefResolver::build` (`reader.cpp:465-473`, which catches
   `composition` + `inputs`) — because `params` is kind territory and the core may not
   read its semantics.

   *Rejected — prefer the `composition` and ignore the `ref` (or vice versa):* doc 08
   states the opposite preference twice, and `nested_codec` D2 already made the call
   for the sibling case: "rejecting it beats silently dropping a composition the author
   wrote".

8. **This task ships `FilesystemAssetSource`, the tree's first `AssetSource`, in
   `runtime`.** Nothing can be tested end-to-end without one, and `load_context.hpp:25-30`
   named this task's kind as the trigger ("a filled-in loader lands with the kinds that
   first need it (`org.arbc.raster`, `org.arbc.nested`)"). It belongs in runtime because
   doc 17:60 says so in as many words — runtime's contents are "`Document` (arenas +
   model + registry + **loaders**)". It fires `on_ready` inline and yields empty bytes on
   any failure, which the `AssetSource` contract permits (non-blocking does not mean
   must-defer) and which Decision 6 turns into the placeholder.

   *Rejected — a test-only in-memory source, deferring the real one:* the golden that
   proves the milestone promise ("external nested projects load") has to load a real
   file. Both exist: the filesystem source drives the golden, an in-memory double drives
   the dedup/cycle/depth cases deterministically without temp files.

9. **No doc-00 decision bullet.** `00-overview.md:109-113` already records "external
   references by URI. Decided in doc 08"; the three deltas above are mechanisms inside
   that recorded decision. Consistent with `nested_codec` D7, `compositions_table` D9,
   and `operator_codecs` D6.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-11.

- Added `Content::external_composition_ref()` virtual accessor (`src/contract/arbc/contract/content.hpp`) beside `composition_ref()`, plus `NestedContent`'s `std::string d_ref`, the override, and a `ref()` accessor (`src/kind_nested/arbc/kind_nested/nested_content.hpp`, `src/kind_nested/nested_content.cpp`).
- Writer suppression: `external_composition_ref()` non-empty skips `enqueue_composition` and `inputs` descent in `src/serialize/writer.cpp`; mirrored in `capture_snapshot` in `src/runtime/document_serialize.cpp`.
- New `serialize::load_composition()` (`src/serialize/reader.cpp`, `src/serialize/arbc/serialize/reader.hpp`) installs a composition subtree under a caller-seeded root `ObjectId` without touching the model root.
- `normalize_uri` added to `LoadContext` (`src/serialize/arbc/serialize/load_context.hpp`, `src/serialize/load_context.cpp`) for dedup-by-resolved-identity (Constraint 5 / claim `:219`).
- `ExternalCompositionLoader` (`src/runtime/arbc/runtime/external_composition_loader.hpp` + `src/runtime/external_composition_loader.cpp`): resolved-URI → child-root map, allocate-before-parse cycle cut, depth cap 64, per-document `LoadContext`.
- `FilesystemAssetSource` (`src/runtime/arbc/runtime/filesystem_asset_source.hpp` + `src/runtime/filesystem_asset_source.cpp`): tree's first `AssetSource` implementation; fires `on_ready` inline, empty bytes on any failure.
- Codec wiring: `codec_nested.cpp` now consumes `params.ref` on load and emits it on save; `LoadContext& ctx` no longer `/*ctx*/`; loader injected via closure in `src/runtime/document_serialize.cpp`; `src/runtime/arbc/runtime/document_serialize.hpp` gains `base_uri` param on `load_document`.
- TSan fix (fixer sub-agent): replaced per-instance `d_mutex` in `NestedContent::ensure_memo()` with a shared `memo_mutex()` to eliminate lock-order inversion across a cross-document cycle graph (`src/kind_nested/arbc/kind_nested/nested_content.hpp`, `src/kind_nested/nested_content.cpp`).
- Tests: `tests/nested_external_ref.t.cpp` (dedup counter, cross-document cycle, self-ref, unavailable, depth cap, bad bytes), `tests/nested_external_ref_golden.t.cpp` (pixel golden vs in-document oracle, attach invariance, round-trip), `src/runtime/t/nested_codec.t.cpp` extended (composition+ref rejection, vendor_tag residual); `tests/document_serialize_concurrency.t.cpp` TSan lane; fuzz seed `tests/fuzz/corpus/load_document/external_ref.arbc`.
- Build: `src/runtime/CMakeLists.txt`, `src/serialize/CMakeLists.txt`, `tests/CMakeLists.txt` updated; 5 new claim rows + re-enforcement tags in `tests/claims/registry.tsv`; design-doc deltas in `docs/design/03-layer-plugin-interface.md`, `docs/design/05-recursive-composition.md`, `docs/design/08-serialization.md`.
- All 851 tests green; levelization silent; claims register balanced (250 enforced); clang-format clean; every CI lane (gcc/clang × debug/release/asan/tsan/rtsan, coverage) passed.
