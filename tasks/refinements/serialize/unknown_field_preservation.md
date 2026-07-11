# serialize.unknown_field_preservation — All-tier unknown-field preservation

## TaskJuggler entry

[`tasks/60-serialize.tji:47-52`](../../60-serialize.tji) — `task
unknown_field_preservation "All-tier unknown-field preservation"`, inside
`task serialize`.

> Preserve unknown sibling fields verbatim at every document tier (envelope,
> composition, layer, content-body siblings) per doc 08 Principle 4 — today
> only params interiors survive and the reader drops the rest (incl. doc 08's
> own example layer 'name' field): capture leftover keys into an opaque
> serialized-bytes stash carried on the records (bytes, not JSON — the JSON
> type stays private to arbc::serialize), re-merge into canonical sorted
> output on save without shadowing known keys, extend the determinism corpus
> with unknown-sibling fixtures at each tier. Source: tasks/parking-lot.md
> 2026-07-09 (layer-level unknown-field gap). Docs 08/16.

Milestone: **`m8_persistence`** ("M8: Documents as files",
[`tasks/99-milestones.tji:63-67`](../../99-milestones.tji)), whose note
already promises "canonical `.arbc` round-trips with unknown-kind and
**every-tier** unknown-field preservation".

## Effort estimate

**2d** (`tasks/60-serialize.tji:48`). Budget:

- **0.5d** — the residual/merge core in `arbc::serialize`: a per-tier
  known-key subtraction that yields the leftover object, and its mirror, a
  recursive merge where the known side always wins. Wire it into the four
  reader parse sites and the four writer emit sites.
- **0.5d** — the `UnknownFields` / `UnknownFieldStore` carrier (a new public,
  JSON-free header), the two entry-point signature extensions, and the L5
  plumbing: `Document` owns a store; `capture_snapshot` copies it into the
  snapshot so the off-thread save never reads live editor state.
- **0.75d** — tests: unknown-sibling fixtures at each tier in the determinism
  corpus, a new unit test for the never-shadow and leniency rules, four fuzz
  corpus seeds, the L5 golden, three claims-register rows.
- **0.25d** — the doc 08 Principle 4 delta and the WBS gate (`tj3`,
  `check_levels.py`, `check_claims.py`).

This is tight but real: the mechanism is one pair of functions applied
uniformly, and the tier wiring is mechanical once the known-key sets are
named constants.

## Inherited dependencies

**Settled.**

- **`serialize.format_tests`** (DONE 2026-07-09,
  [`format_tests.md`](format_tests.md)) — the direct predecessor
  (`depends !format_tests`). It hands forward the determinism corpus this
  task extends (`tests/serialize_determinism_corpus.t.cpp`, inline raw-string
  goldens asserting `serialize(load(x)) == x` byte-exact), the dual-driver
  fuzz harness (`tests/fuzz/fuzz_target.hpp`'s `arbc_fuzz_load_document`,
  whose differential-determinism invariant is
  `serialize(load(serialize(load(x)))) == serialize(load(x))`), the on-disk
  seed corpus (`tests/fuzz/corpus/load_document/`, 15 seeds), and — most
  directly — **the deviation note that spawned this task**
  ([`format_tests.md:461`](format_tests.md): "runtime document reader drops
  layer-level unknown sibling fields; unknown-field preservation pinned at
  params/body level … layer-level verbatim preservation not asserted").
- **`serialize.kind_params`** (DONE 2026-07-09,
  [`kind_params.md`](kind_params.md)) — the doc 08 Principle 1 delta
  (serialize-owned codecs keyed by reverse-DNS kind id, because `Content`
  lives in `contract` and must not name JSON), the `CodecTable` /
  `Codec{SerializeFn, DeserializeFn}` seam
  (`src/serialize/arbc/serialize/codec.hpp:40-70`), and `PlaceholderContent`
  (`src/serialize/arbc/serialize/placeholder_content.hpp:36-78`), whose
  `nlohmann::json d_body` member (`:75`) is the **only** verbatim stash that
  exists today.
- **`serialize.sharing`** (DONE 2026-07-09, [`sharing.md`](sharing.md)) — the
  `contents` table + `{"$ref": id}` graph, and Decision 2's rule that
  `contents` ids are core-owned non-semantic handles re-derived every save,
  so hand-authored ids normalize (claim
  `08-serialization#hand-authored-ids-normalize-deterministically`,
  `tests/claims/registry.tsv:225`). That rule is the precedent this task
  leans on to declare the `contents` table a **non**-surface for unknown
  fields (Decision 5).
- **`serialize.reader`** / **`serialize.writer`** (DONE 2026-07-09) — the
  parse walk and the canonical emitter this task threads a stash through.
- **`runtime.document_serialize`** (DONE 2026-07-09,
  [`../runtime/document_serialize.md`](../runtime/document_serialize.md)) —
  the L5 façade: `KindBridge`, `ContentSnapshot`, `capture_snapshot` (writer
  thread) / `serialize_snapshot` (off-thread), and the pinned-`DocStatePtr`
  discipline that makes autosave non-blocking. The snapshot's
  copy-then-serialize shape is what Decision 6 extends.
- **`runtime.operator_codecs`** (DONE 2026-07-09,
  [`../runtime/operator_codecs.md`](../runtime/operator_codecs.md)) —
  Decision 4's "demote-after-sink": every content node the reader
  materializes gets an `ObjectId` from the sink, and operator **input
  children** then have their `ContentRecord` removed while the object stays
  alive in `Document::d_contents` under that id. This is load-bearing for
  Decision 3 — it means every content, root or child, has a unique, stable,
  never-reused `ObjectId` to key a stash by.

**Pending (none blocking).**

- **`serialize.compositions_table`** (`tasks/60-serialize.tji:53-58`, also in
  M8) is a **sibling, not a predecessor** — it depends on `!sharing`, so the
  two are independently unblocked and may land in either order. It introduces
  three new core-owned keys (`compositions` at the root, `composition` on a
  nested content body, and recursion into non-root compositions). Constraint 7
  makes that a one-line change for whichever task lands second.

**Downstream (this task unblocks).** `m8_persistence` directly. No other leaf
depends on it.

## What this task is

Today the `.arbc` reader is a **whitelist parser at every tier**: it names the
keys it knows, copies those out, and lets the rest die with the parsed
`nlohmann::json` tree. Doc 08 Principle 4 (`docs/design/08-serialization.md:88-98`)
promises the opposite — that within a known format major, unknown fields are
*preserved*-and-ignored at **every tier** of the document. Only one tier
delivers on that today, and only by accident of how the unknown-kind
placeholder works. This task closes the gap.

Concretely:

- **(a) A verbatim residual at each core-owned tier.** At the document root,
  the composition, the layer, and a standalone content body, compute the
  leftover — the keys present in the input object that the core did not name
  — and stash it verbatim. The subtraction is recursive through known
  sub-objects (`working_space`, `working_audio_format`, `time_map`), so an
  unknown key *inside* a known sub-object survives too.
- **(b) Unknown keys inside a known kind's `params`.** For a kind whose codec
  is registered, the codec parses `params` and the live `Content` keeps only
  what the codec understood; re-serialization rebuilds `params` from scratch
  (`src/serialize/codec.cpp:111-114`), dropping the rest. Recover the
  remainder by **differencing the codec's own re-serialization against the
  input params at load time** (Decision 4). For an unknown kind — or an
  unknown `kind_version` — the placeholder already holds the whole body
  verbatim; that path is unchanged.
- **(c) An opaque, JSON-free carrier.** The stash must survive the trip
  reader → `runtime::Document` → writer, and doc 08 Principle 1
  (`:61-63`) forbids the JSON type from crossing out of `arbc::serialize`.
  So the carrier is **canonical JSON object text in a `std::string`** —
  bytes every other component stores and hands back without interpreting
  (`UnknownFields`), held in an `UnknownFieldStore` keyed by
  `(scope, ObjectId)`.
- **(d) A never-shadowing merge on save.** The writer builds its known object
  first, then merges the stash in: a preserved key that collides with a known
  key **loses** (doc 08:96). This is not hypothetical — the reader ignores
  `working_space.primaries` (`src/serialize/reader.cpp:87-88`) while the
  writer emits it, so that key *is* an unknown at load and a known at save.
  Merging into an `nlohmann::json` object (a `std::map`) puts the result in
  canonical sorted order for free, so Principle 5 holds with no extra work.
- **(e) Fixtures at every tier.** Extend the determinism corpus with
  unknown-sibling documents at the root, composition, layer, and content-body
  tiers, plus the shadowing and known-kind-`params` cases; add fuzz seeds so
  the differential-determinism invariant exercises the stash.

**Not this task:**

- The `contents` / `compositions` id-keyed tables are **not** unknown-field
  surfaces (Decision 5) — an entry no `$ref` reaches is dropped on save, the
  same canonicalization that renumbers hand-authored ids.
- A **known** key carrying a **malformed value** is not an unknown field
  (Decision 2). The reader's leniency (`num_or`/`bool_or`/`int_or`,
  `src/serialize/reader.cpp:44-57`) substitutes the default; that value is
  not preserved and its round-trip behavior does not change.
- No new codec API. Codec authors write no preservation code; the mechanism
  is entirely inside `arbc::serialize` and works for plugin codecs for free
  (Decision 4).
- No change to `records.hpp`. The model's slab records are pointer-free and
  trivially destructible (`src/model/arbc/model/records.hpp:60-144`); there is
  nowhere to hang a variable-size blob and this task does not try.

## Why it needs to be done

Doc 08's Principle 4 is a **data-loss guarantee**, and it is the one the
format leans on hardest: it is what lets a newer authoring tool, a plugin's
sidecar metadata, or a downstream pipeline annotate an `.arbc` file without
an older build silently erasing the annotation on the next autosave. Today
that guarantee is false at four of five tiers — including for doc 08's *own
example layer field* (`"name"`, `docs/design/08-serialization.md:35`), which
the reader drops on the floor.

The claims register already asserts the guarantee:
`08-serialization#reader-rejects-unknown-format-major`
(`tests/claims/registry.tsv:212`) ends with "within a known major, unknown
fields are preserved-and-ignored, not an error". The code delivers "ignored"
and not "preserved". The register and the reader disagree, and the register is
supposed to be what stops the docs from going aspirational (doc 16:18-21).

The gap was found by `serialize.format_tests`' implementer, parked as a
human-judgment call (`tasks/parking-lot.md:138-142`), and triaged on
2026-07-10 (`:143`): confirmed a genuine Principle 4 violation, fix-the-loader
chosen over narrowing the doc, with the every-tier scope written into doc 08
in the same pass. This task is the implementation half of that triage.

M8 cannot close without it — the milestone's own note names "every-tier
unknown-field preservation" as part of what "documents as files" means.

## Inputs / context

### Design docs (normative, doc 16)

- **`docs/design/08-serialization.md:88-98`** — Principle 4, the governing
  text. Note it was amended by the 2026-07-10 triage to state the every-tier
  scope and the opaque-stash mechanism explicitly; this task implements what
  it already says. This refinement lands a further **clarifying delta** to it
  (Decisions 2, 4, 5 — see [Decisions](#decisions)).
- **`docs/design/08-serialization.md:59-74`** — Principle 1. `:61-63` is the
  JSON-privacy constraint that forces the byte carrier: "the JSON type stays
  private to `arbc::serialize` (doc 17 levelization: the `Content` interface
  lives in `contract`, which must not name the JSON library)".
- **`docs/design/08-serialization.md:75-81`** — Principle 2, the unknown-kind
  placeholder, which also covers `kind_version` skew. This is the escape hatch
  that makes Decision 4's known-kind rule safe.
- **`docs/design/08-serialization.md:99-111`** — Principle 5, canonical output:
  sorted keys (the JSON object's natural `std::map` ordering), platform-
  independent shortest round-trip numbers, non-finite as an error value.
- **`docs/design/08-serialization.md:112-139`** — Principle 6, the `contents`
  table and `$ref`; `:126-131` is the id-renumbering-is-canonicalization rule
  Decision 5 extends.
- **`docs/design/08-serialization.md:21-46`** — the example document. `:35` is
  the layer-tier `"name"` field the reader drops today. `:48-56` lists the
  core-owned placement keys omitted at their defaults.
- **`docs/design/16-sdlc-and-quality.md:10-21`** — the claims register and the
  `// enforces: <claim-id>` tag convention; `:23-25` is the same-commit
  doc-amendment rule the doc 08 delta rides.
- **`docs/design/17-internal-components.md:58`** — `arbc::serialize` is **L4**,
  may depend on `contract`, `model`, and the JSON dep. `:60` — `arbc::runtime`
  is **L5**. `:41-44` — a component may depend only on strictly lower levels.
  This task adds **no** new edge (Constraint 6).

### Source seams

**The drop sites** (all in `src/serialize/reader.cpp`) — these are what the
task fixes:

| Tier | Function | Lines | Keys it names |
|---|---|---|---|
| Root | `load_document` | `375-415` | `arbc`, `composition`, `contents` |
| Envelope | (inline) | `382` | `format` |
| Composition | `parse_composition` | `231-258` | `canvas`, `working_space`, `working_audio_format`, `layers` |
| — `working_space` | `parse_surface_format` | `77-94` | `format`, `premultiplied`, `transfer` (**`primaries` is written but never read** — `:87-88`) |
| — `working_audio_format` | `parse_audio_format` | `97-111` | `sample_rate`, `channels` |
| Layer | `parse_layer` | `218-229` | `transform`, `opacity`, `visible`, `gain`, `audible`, `span`, `time_map` |
| — `time_map` | `parse_time_map` | `137-158` | `rate`, `in`, `offset` |
| Content position | `extract_content_body` | `199-216` | `$ref` \| `kind`, `kind_version`, `params`, `inputs` — **the literal 4-key whitelist at `:209`**, a copying filter that truncates the body before it can even reach the placeholder |

Reader leniency (`num_or` / `bool_or` / `int_or`, `:44-57`) treats a
present-but-mistyped **known** key as absent — see Decision 2.

**The graph walk:** `RefResolver` (`:268-353`); `resolve()` (`:275-310`) dedups
repeated `$ref`s so the sink fires **once per shared node**; `build()`
(`:317-340`) recurses into `inputs` and makes **the one and only sink call**
(`:340`). Top-level driver at `:426-438`. Content-free overload at `:494-498`.

**The emit sites** (`src/serialize/writer.cpp`) — where the merge hooks in:

- `layer_json` `:253-295`; the content body is spliced flat into the layer
  object at `:285-293`. The **layer** stash merges here.
- `composition_json` `:297-320`. The **composition** stash merges after
  `o["layers"]` (~`:318`).
- `serialize_impl` `:326-371`. The **root** stash merges after
  `root["contents"]` (~`:354-358`), before the `em.ok()` check at `:359`.
  The single canonical dump is `:368` (`root.dump(2, ' ', false, replace)`).
- `Emitter` `:36-75` — the non-finite fault-as-value guard.
- `ContentGraph` `:138-251`; `emit_use` `:168-175` (`{"$ref": id}` at use
  sites), `emit_definition` `:223-241`.

**The codec seam** (`src/serialize/codec.cpp`):

- `content_body_from_json` `:32-84` — dispatch on `body["kind"]`; a miss falls
  through to the placeholder at `:79-83` (`json stored = body;
  stored.erase("inputs");`), which is the whole body verbatim **as filtered by
  `extract_content_body`**. Removing that filter (Constraint 1) is what makes
  the placeholder path actually verbatim.
- `content_body_to_json` — placeholder branch `:96-98` (re-emit `body()`
  unchanged); known-kind branch `:111-114`, which rebuilds
  `{kind, kind_version, params}` from scratch. Decision 4's residual merges
  here.

**The public seams** (unchanged shape, one new parameter each):

- `src/serialize/arbc/serialize/reader.hpp:62-63` (content-free
  `load_document`), `:71-74` (`SunkContent{ObjectId id; Content* live;}`),
  `:84` (`ContentSink = std::function<SunkContent(std::unique_ptr<Content>)>`),
  `:98-100` (content-aware `load_document`).
- `src/serialize/arbc/serialize/writer.hpp:56` (content-free
  `serialize_document`), `:64-68` (`ContentBody`), `:74`
  (`ContentBodyProvider`, keyed by `ObjectId`), `:82-85` (`ContentMeta`), `:93`
  (`ContentMetaProvider`, keyed by `const Content&`), `:111-114` (content-aware
  `serialize_document`).
- `src/serialize/CMakeLists.txt:4-12` — the three public headers, and
  `target_link_libraries(arbc_serialize PRIVATE nlohmann_json::nlohmann_json)`.
  The new `unknown_fields.hpp` joins the **public** list precisely because it
  names no JSON type.

**Identity and lifetime** — the facts Decision 3 rests on:

- `src/base/arbc/base/ids.hpp:9-17` — `ObjectId` is a bare `std::uint64_t`, no
  generation counter. `ObjectId{0}` is the never-valid sentinel.
- `src/model/model.cpp:537` — `ObjectId Model::allocate_id() { return
  ObjectId{d_next_id.fetch_add(1)}; }`, over
  `std::atomic<std::uint64_t> d_next_id{1}` (`src/model/arbc/model/model.hpp:443`).
  **There is no free list and no release path.** ObjectIds are strictly
  monotonic and never reused, so a stale stash entry can never alias a later
  object.
- `src/runtime/arbc/runtime/document.hpp:156` —
  `std::unordered_map<ObjectId, std::shared_ptr<Content>> d_contents`,
  writer-thread-owned, inserted at `src/runtime/document.cpp:46-48`, **never
  erased** (see Constraint 8).

**The L5 save/load path** (`src/runtime/document_serialize.cpp`):

- `capture_snapshot` `:149-229` — writer thread; walks layers (`:161-184`,
  populating `by_id` and `by_ptr`), then does a transitive `inputs()` walk
  (`:199-227`) interning reachable built-in children into `by_ptr` only.
- `serialize_snapshot` `:231-255` — builds the `ContentBodyProvider` /
  `ContentMetaProvider` closures over the **immutable snapshot** and calls
  `serialize_document`. This runs off the editing thread.
- `ContentSnapshot` — `src/runtime/arbc/runtime/document_serialize.hpp:75-85`.
- The load sink lambda `:340-360` (mint → demote parents' inputs →
  `doc.add_content`); the demotion comment at `:324-337`.
- `:68-76` — the note that no persistent `Content*`→kind map is kept because
  it is cheap to re-derive per save.

**Codec params shape** (relevant to Decision 4): `codec_solid.cpp:26-35` and
`codec_crossfade.cpp:45-56` always emit a total key set;
`codec_tone.cpp:27-36` likewise; `codec_fade.cpp:56-71` omits `in`/`out` when
`nullopt` — but they are `optional`-shaped, not value-defaulted, so its
deserializer (`:76-93`) round-trips absence as absence. **Every key any
built-in deserializer reads, its serializer emits.**

**Tests:**

- `tests/serialize_determinism_corpus.t.cpp` (wired at
  `tests/CMakeLists.txt:72-81`) — inline raw-string canonical goldens.
- `tests/fuzz/fuzz_target.hpp` (shared `arbc_fuzz_load_document`),
  `tests/fuzz/fuzz_load_document.cpp` (libFuzzer, clang-only),
  `tests/fuzz_corpus_replay.t.cpp` (portable per-push replay, wired at
  `tests/CMakeLists.txt:83-95`), corpus at
  `tests/fuzz/corpus/load_document/` (15 seeds; path injected via
  `ARBC_FUZZ_CORPUS_DIR`, `tests/CMakeLists.txt:89`/`:93`).
- `tests/document_serialize_golden.t.cpp`,
  `tests/document_serialize_concurrency.t.cpp`,
  `tests/operator_codecs_golden.t.cpp`.
- `tests/claims/registry.tsv` — serialization block at `:208-225`; the rows
  this task touches are `:208`, `:212`, `:214`, `:215`, `:224`.

**Predecessor / sibling refinements:** [`format_tests.md`](format_tests.md)
(esp. the `:461` deviation note and D3, inline goldens with the fuzz corpus as
the one sanctioned on-disk exception), [`kind_params.md`](kind_params.md) (D1
the Principle 1 delta, D4 the placeholder-holds-the-body-verbatim rule),
[`sharing.md`](sharing.md) (D2 id canonicalization, D6 the two distinct dedup
mechanisms), [`../runtime/document_serialize.md`](../runtime/document_serialize.md)
(the snapshot/off-thread-save discipline),
[`../runtime/operator_codecs.md`](../runtime/operator_codecs.md) (D4
demote-after-sink).

## Constraints / requirements

1. **`extract_content_body`'s 4-key filter must go.** `src/serialize/reader.cpp:209`
   truncates the layer/body object *before* anything downstream can preserve
   it — including the placeholder path, which is why claim
   `08-serialization#unknown-kind-round-trips-verbatim`
   (`tests/claims/registry.tsv:214`) currently over-promises ("and any unknown
   fields — verbatim") relative to what a layer-position body actually
   receives. The content body handed to `content_body_from_json` must carry
   every key the input body had.

2. **Known-key sets are named constants, declared once.** Each tier's known
   keys (root, envelope, composition, `working_space`, `working_audio_format`,
   layer, `time_map`, content body) become a single named constant in
   `src/serialize/reader.cpp` (e.g. `k_root_keys`, `k_layer_keys`,
   `k_body_keys`), used by both the parse and the subtraction. Adding a
   core-owned key must be a one-line edit in exactly one place — this is what
   makes Constraint 7 cheap.

3. **The carrier names no JSON type.** `UnknownFields` holds a `std::string` of
   canonical JSON object text and nothing else. It ships in a **public**
   header (`src/serialize/arbc/serialize/unknown_fields.hpp`) added to
   `PUBLIC_HEADERS` in `src/serialize/CMakeLists.txt:4-12`. `nlohmann::json`
   stays confined to the `.cpp` files and the three internal headers
   (`codec.hpp`, `deserialize.hpp`, `placeholder_content.hpp`) exactly as
   today. Doc 08:61-63; doc 17:58.

4. **The merge never shadows a known key.** The writer emits its known object
   first; a stashed key already present in that object is discarded, not
   written. Where both sides are objects the merge recurses; otherwise the
   known value wins outright. Doc 08:96. `working_space.primaries` is the live
   case (`reader.cpp:87-88` ignores it, `writer.cpp` emits it) and must be
   covered by a test.

5. **Canonical output is unaffected.** Merging into an `nlohmann::json` object
   (a `std::map`) yields ascending-key order automatically, so the single dump
   at `src/serialize/writer.cpp:368` still produces canonical bytes. The stash
   payload itself is stored as a canonical `dump()` so that the fuzz fixed-point
   invariant (`serialize(load(serialize(load(x)))) == serialize(load(x))`,
   claim `:224`) still holds. A stash whose bytes fail to re-parse at save is
   treated as **empty**, never as a fault — the loader-never-faults claim
   (`:224`) is not weakened.

6. **No new levelization edge.** `UnknownFields`/`UnknownFieldStore` live in
   `arbc::serialize` (L4) and name only `arbc::base::ObjectId` and a
   forward-declared `arbc::contract::Content`. `arbc::runtime` (L5) already
   depends on `serialize`. `scripts/check_levels.py` must stay green with no
   edit to its allow-list.

7. **`serialize.compositions_table` extends the key sets, not the mechanism.**
   The sibling task adds `compositions` (root) and `composition` (content
   body), and recurses into non-root compositions. Whichever of the two lands
   second adds those names to the Constraint-2 constants and, for
   `compositions_table`, keys non-root compositions' stashes by their
   `ObjectId` like any other composition. If `unknown_field_preservation`
   lands first, a `compositions` key would otherwise be **preserved as an
   unknown root field** and then double-emitted — so the second task's
   implementer must add the names. Call this out in the code comment on the
   key-set constants.

8. **`Document::d_contents` is never pruned — and this design does not depend
   on that.** `src/runtime/document.cpp:46-48` inserts and nothing erases, so a
   content removed by an edit leaks its map entry for the `Document`'s
   lifetime (pre-existing; demote-after-sink relies on it to keep operator
   input children alive). Because ObjectIds are strictly monotonic
   (`src/model/model.cpp:537`), a stale `UnknownFieldStore` entry can never be
   read back onto a *different* object, whether or not pruning is ever added.
   The store's growth is bounded by the number of objects the document has ever
   held — same bound as `d_contents`. Do not make correctness contingent on the
   leak: key by `ObjectId`, never by `Content*` (Decision 3).

9. **The off-thread save must not read live editor state.** `serialize_snapshot`
   (`src/runtime/document_serialize.cpp:231-255`) runs off the editing thread
   against an immutable `ContentSnapshot`; `capture_snapshot` (`:149-229`) runs
   on the writer thread. The store is writer-thread-owned like `d_contents`, so
   `capture_snapshot` must **copy** it into the snapshot (Decision 6) rather
   than let the writer hold a reference into live state. This preserves the
   `08-serialization#writer-serializes-the-pinned-version` guarantee
   (`tests/claims/registry.tsv:209`) and autosave's never-pause-editing
   property.

10. **Errors stay values.** No new exception path. A malformed stash, a codec
    that fails to re-serialize during the Decision-4 residual computation, or a
    missing store all degrade to "no preserved fields", never to a `ReaderError`
    or `SerializeError`. Doc 10 errors-as-values; doc 08:168-174.

## Acceptance criteria

- **Every-tier preservation (golden-backed).** Extend
  `tests/serialize_determinism_corpus.t.cpp` with inline canonical goldens
  carrying unknown sibling fields at each tier — a root-level key beside
  `arbc`/`composition`; a composition-level key beside `layers`; a layer-level
  key (use doc 08's own `"name"`, `docs/design/08-serialization.md:35`); an
  unknown key nested inside `time_map` and inside `working_space`; and unknown
  siblings on a standalone `contents`-table body. Assert `serialize(load(x)) ==
  x` byte-exact for each, and re-serialization idempotence. Lands claim
  **`08-serialization#unknown-fields-preserved-at-every-tier`** in
  `tests/claims/registry.tsv` with an `enforces:`-tagged test.

- **Never-shadow (golden-backed).** New test
  `tests/serialize_unknown_fields.t.cpp` covering the collision rule directly:
  a document whose `working_space` carries `primaries` (read-ignored by
  `reader.cpp:87-88`, emitted by the writer) round-trips with **exactly one**
  `primaries` key, carrying the writer's value — the preserved unknown loses.
  Same test pins the residual/merge unit behavior: recursion through known
  sub-objects, atomic treatment of arrays and scalars, and a stash whose bytes
  fail to parse degrading to empty. Lands claim
  **`08-serialization#preserved-unknown-never-shadows-known`**.

- **Known-kind `params` interiors (golden-backed).** In the same test: a
  document whose `org.arbc.solid` body carries `params: {"color": [...],
  "<unknown>": ...}` round-trips the unknown key byte-exact while the codec
  still produces the live `SolidContent`; and an `org.arbc.fade` body proves
  the load-time-residual rule does **not** resurrect a cleared optional —
  after loading a fade with `params.in` present and editing it to `nullopt`,
  the re-save omits `in` (Decision 4's failure mode for the rejected
  save-time-diff alternative). Lands claim
  **`08-serialization#known-kind-params-unknowns-preserved`**.

- **Unknown-kind bodies get *more* verbatim, not less (re-enforcement).** With
  `extract_content_body`'s filter gone (Constraint 1), a placeholder body at a
  layer position now genuinely preserves every sibling key — which is what
  claim `08-serialization#unknown-kind-round-trips-verbatim`
  (`tests/claims/registry.tsv:214`) already asserts. Add a corpus case with an
  unknown-kind body carrying an unrecognized sibling **at the layer position**
  and re-enforce `:214` via a second `// enforces:` tag (no new row).

- **L5 round-trip through a real `Document` (golden-backed + behavioral).**
  Extend `tests/document_serialize_golden.t.cpp`: save → load → save through
  `runtime::Document` reproduces unknown fields at all tiers byte-identical,
  proving the `UnknownFieldStore` survives the `ContentSnapshot` copy and the
  provider closures. Extend `tests/document_serialize_concurrency.t.cpp` to
  capture a snapshot while the writer thread mutates the document, asserting
  the serialized bytes match the pinned revision's unknowns — re-enforcing
  `08-serialization#writer-serializes-the-pinned-version`
  (`tests/claims/registry.tsv:209`) under the new state (Constraint 9).

- **Fuzz corpus growth (re-enforcement).** Add four seeds to
  `tests/fuzz/corpus/load_document/`: `unknown_fields_all_tiers.arbc`,
  `unknown_shadowing_known.arbc`, `unknown_in_known_params.arbc`,
  `unknown_nested_in_time_map.arbc`. Update `tests/fuzz/fuzz_target.hpp`'s
  `arbc_fuzz_load_document` to thread an `UnknownFieldStore` through its
  load→serialize→load→serialize chain, so the differential-determinism
  invariant actually exercises the stash. Re-enforces
  `08-serialization#loader-never-faults-on-hostile-input`
  (`tests/claims/registry.tsv:224`) via a second tag; no new row. The portable
  `tests/fuzz_corpus_replay.t.cpp` picks the seeds up on every push.

- **Canonical output unchanged for documents with no unknowns
  (golden-backed).** The existing goldens in
  `tests/serialize_writer_golden.t.cpp`, `tests/serialize_reader_roundtrip.t.cpp`,
  `tests/serialize_sharing.t.cpp`, and `tests/operator_codecs_golden.t.cpp`
  must pass **unmodified** — an empty stash changes nothing. Re-enforces
  `08-serialization#canonical-output-is-byte-stable`
  (`tests/claims/registry.tsv:208`).

- **Doc 08 delta (same-commit, doc 16:23-25).** `docs/design/08-serialization.md`
  Principle 4 gains the clarifications from Decisions 2, 4, and 5 — what
  "unknown" means against a malformed known value, where a layer-position
  body's unknown siblings are recorded, that the id-keyed tables are not a
  sibling surface, and how a known kind's `params` interior is preserved. No
  doc 00 decision-record bullet (Decision 7).

- **Deferred — none.** The closer registers **no new WBS leaf**. The one
  cross-task item is Constraint 7, a key-set extension owned by the existing
  `serialize.compositions_table`, not new work.

- **Coverage / build / WBS gate.** ≥90% diff coverage on changed lines;
  `scripts/check_levels.py` and `scripts/check_claims.py` green; the `dev`,
  `asan`, `tsan`, and `fuzz` presets build and pass; `tj3 project.tjp 2>&1 |
  grep -iE "error|warning"` silent after the `complete 100` edit.

## Decisions

1. **Stash the residual of a static, per-tier known-key subtraction — not a
   diff against the writer's output.** At load, for each core-owned tier, the
   leftover is `input_object − k_<tier>_keys`, computed recursively through
   known sub-objects (arrays and scalars are atomic: a key is either wholly
   known or wholly stashed). The known-key set is a compile-time constant, so
   the subtraction is exact and cheap, and it is defined at *load* time — no
   dependence on what a later save happens to emit.

   *Rejected — round-trip diff at the core tiers (serialize what we just
   parsed, subtract that from the input):* the writer's emitted set is
   **narrower** than the reader's known set by design — Principle 5 omits
   core-owned placement fields at their defaults (`docs/design/08-serialization.md:48-56`;
   `writer.cpp:253-295` omits `gain`, `audible`, `span`, `time_map`). Diffing
   against the writer would classify an *explicitly written default* (`"gain":
   1.0`) as an unknown field and re-emit it, breaking default-omission and
   with it `08-serialization#canonical-output-is-byte-stable`.

   *Rejected — preserve the entire raw input object and re-emit it as the base,
   overlaying known fields:* this makes non-canonical input survive
   canonicalization — hand-authored ids, unsorted keys, and explicit defaults
   would all round-trip verbatim, directly contradicting
   `08-serialization#hand-authored-ids-normalize-deterministically`
   (`tests/claims/registry.tsv:225`).

2. **"Unknown" means *a key the core does not name*, never *a key whose value
   the core could not use*.** A **known** key carrying a malformed value stays
   known: `num_or`/`bool_or`/`int_or` (`src/serialize/reader.cpp:44-57`)
   substitute the field's default and the bad value is **not** preserved. The
   subtraction is by key name only.

   This keeps the reader's existing leniency intact and keeps the rule
   decidable without consulting values. *Rejected — stash a known key whose
   value failed to parse:* it would resurrect malformed data on save (a
   `"opacity": "very"` string re-emitted next to the writer's numeric
   `opacity`, or — worse — shadowing it), turning a lenient recovery into a
   corruption vector, and it would make preservation depend on the *type* of a
   value rather than on the document's schema. It also has no doc backing:
   Principle 4 speaks of unknown *fields*, and a malformed `opacity` is not an
   unknown field. **Doc 08 delta** (same-commit per doc 16:23-25).

3. **One `UnknownFieldStore`, four scopes, keyed by `ObjectId` — never by
   `Content*`.** The carrier is:

   ```
   struct UnknownFields { std::string bytes; bool empty() const; };
   enum class UnknownScope : std::uint8_t { Document, Composition, Layer, Content };
   class UnknownFieldStore {  // copyable; ObjectId{} keys the Document scope
     void set(UnknownScope, ObjectId, UnknownFields);
     const UnknownFields* find(UnknownScope, ObjectId) const;
     ...
   };
   ```

   passed as a nullable `UnknownFieldStore*` / `const UnknownFieldStore*`
   parameter (defaulted to `nullptr`) on the **content-aware** `load_document`
   (`reader.hpp:98-100`) and `serialize_document` (`writer.hpp:111-114`).
   `nullptr` is today's lossy behavior, so no existing caller breaks and the
   diff stays small. `SunkContent`, `ContentSink`, `ContentBody`, and
   `ContentBodyProvider` are unchanged; `ContentMeta` (`writer.hpp:82-85`)
   gains an `ObjectId id` so the writer can look a body's stash up while
   emitting it (`capture_snapshot` fills it from `Document::for_each_content()`,
   which knows every content's id — including demoted input children, whose
   `d_contents` entry survives demotion per `document_serialize.cpp:324-337`).

   ObjectId keys are what make this safe: `Model::allocate_id`
   (`src/model/model.cpp:537`) only ever `fetch_add`s a monotonic counter and
   there is **no free list**, so a stale entry can never alias a later object.
   Every content the reader materializes gets one from the sink before it can
   be demoted (`reader.cpp:340`; `operator_codecs.md` D4).

   *Rejected — key content stashes by `const Content*`:* it works **only
   because** `Document::d_contents` is never erased
   (`src/runtime/document.cpp:46-48`), i.e. correctness would rest on a
   pre-existing leak. If that leak is ever fixed, a freed `Content` could be
   replaced at the same address and a stale stash would attach itself to an
   unrelated content. ObjectIds cannot alias, ever.

   *Rejected — a field on the model's `LayerRecord`/`CompositionRecord`:* the
   slab records (`src/model/arbc/model/records.hpp:60-144`) are fixed-layout,
   pointer-free, and trivially destructible for the HAMT/mmap path; there is
   nowhere to hang a variable-size blob, and putting one there would be a doc
   14/15 change far outside this task.

   *Rejected — separate sink/provider `std::function` pairs per tier:* three
   more seams to thread through two entry points, two overload sets, the L5
   façade, the corpus tests, and the fuzz harness, to express one concept.
   One store, one key type.

4. **Preserve a known kind's `params` unknowns by differencing the codec's own
   re-serialization against the input — at *load* time.** Immediately after
   `deserialize(params) -> Content*` succeeds, call the same codec's
   `serialize(*content) -> params_out` and stash `params_in − params_out`.
   That residual is exactly the set of keys the codec did not consume. At save,
   merge it back into the codec's freshly-produced `params` under the
   never-shadow rule (Constraint 4). Codec authors write nothing; plugin codecs
   get preservation for free; the `Codec`/`DeserializeFn` signatures
   (`codec.hpp:40-48`, `deserialize.hpp:36-37`) do not change.

   *Rejected — compute the diff at save time against a stashed raw `params`:*
   subtly wrong, and it is the whole reason the residual is frozen at load. If
   the user **edits** a param away — clears `org.arbc.fade`'s optional
   `params.in`, which `codec_fade.cpp:56-71` then legitimately omits — a
   save-time diff against the stashed raw input would see `in` as "dropped by
   the codec", classify it as unknown, and **resurrect the cleared value**.
   Freezing the residual at load, before any edit can touch the content, makes
   this impossible: at that instant `params_out` reflects precisely what the
   codec consumed. A test pins exactly this (Acceptance criteria).

   *Rejected — extend `DeserializeFn` to report its consumed key set:* an API
   change to a seam every plugin codec implements, to obtain information the
   codec's own serializer already reveals. Every built-in serializer emits
   every key its deserializer reads (`codec_solid.cpp:26-35`,
   `codec_tone.cpp:27-36`, `codec_fade.cpp:56-71`, `codec_crossfade.cpp:45-56`
   — `fade`'s `in`/`out` are `optional`-shaped, absent-round-trips-as-absent,
   not value-defaulted), so the round-trip is a faithful proxy. A codec that
   *does* omit a key it consumed would see that key re-merged on save — a
   plugin bug with a bounded, visible symptom, not silent data loss.

   *Rejected — leave known-kind `params` interiors unpreserved and narrow doc 08:*
   `docs/design/08-serialization.md:92` names "`params` interiors" in the
   every-tier list and M8's note says "every-tier"; narrowing would re-open the
   very gap the 2026-07-10 triage closed. The escape hatch for a kind that
   genuinely evolves its schema remains `kind_version` (doc 08:97): an
   unrecognized version falls to the placeholder (Principle 2), which holds the
   whole body verbatim. **Doc 08 delta** naming the mechanism.

5. **A layer-position body's unknown siblings are recorded as *layer* fields;
   the `contents`/`compositions` tables are not a sibling surface at all.**
   An inline content body shares the layer's JSON object (`extract_content_body`,
   `reader.cpp:199-216`; the writer splices it back flat at
   `writer.cpp:285-293`), so an unrecognized key there is genuinely
   indistinguishable from an unrecognized *layer* field — doc 08's own example
   puts `"name"` (`:35`) right beside `kind`. Recording it as a layer field is
   the only rule that both round-trips byte-exact and is decidable. Only a body
   standing alone in the `contents` table can carry unknown **content-body**
   siblings, and those key off that content's `ObjectId`.

   Separately, an entry in the `contents` (or, later, `compositions`) map that
   no `$ref` reaches is **dropped on save**, not preserved: those maps are
   core-owned, id-keyed, and re-derived from a first-encounter traversal every
   save (doc 08:126-131), so keeping an unreferenced entry would fight the
   renumbering that
   `08-serialization#hand-authored-ids-normalize-deterministically`
   (`tests/claims/registry.tsv:225`) already pins. **Canonicalization, not data
   loss** — the same call `sharing` D2 made. **Doc 08 delta.**

6. **`capture_snapshot` copies the store; the off-thread save reads only the
   copy.** `ContentSnapshot`
   (`src/runtime/arbc/runtime/document_serialize.hpp:75-85`) gains an
   `UnknownFieldStore unknown` member, filled on the writer thread alongside
   the kind strings it already copies (`document_serialize.cpp:149-229`);
   `serialize_snapshot` (`:231-255`) passes `&snapshot.unknown`. The store is
   plain maps of `std::string`, so the copy is cheap and the snapshot stays
   self-contained.

   *Rejected — pass the live `Document`'s store by pointer into the off-thread
   serialize:* it would let the serializer read state the editing thread can
   mutate concurrently, breaking the pinned-version guarantee
   (`tests/claims/registry.tsv:209`) and the "autosave never pauses editing"
   property `runtime.document_serialize` was built to deliver — and it would be
   a TSan finding. The snapshot exists precisely so nothing off-thread touches
   live state.

7. **No doc 00 decision-record bullet.** The doc 08 delta is a clarification of
   an already-amended Principle 4 (the 2026-07-10 triage wrote the every-tier
   scope and the opaque-stash mechanism in; this task pins down what "unknown"
   means at the edges and how the `params` tier is reached). It originates no
   new architectural seam, no new dependency (doc 10), and no new levelization
   edge (doc 17). Consistent with all six sibling serialize refinements and
   both runtime ones, which each declined a doc 00 bullet on the grounds that
   format details belong in doc 08.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-11.

- Residual/merge core in `arbc::serialize`: per-tier known-key subtraction (`k_root_keys`, `k_composition_keys`, `k_layer_keys`, `k_body_keys`, etc.) yields a leftover object, stashed as canonical JSON bytes; a recursive never-shadow merge re-injects it at each writer emit site (`src/serialize/reader.cpp`, `src/serialize/writer.cpp`, `src/serialize/codec.cpp`).
- JSON-free public carrier (`src/serialize/arbc/serialize/unknown_fields.hpp`: `UnknownFields` + `UnknownFieldStore`) ships in `PUBLIC_HEADERS`; JSON internals confined to `src/serialize/arbc/serialize/unknown_json.hpp` + `src/serialize/unknown_fields.cpp`.
- `extract_content_body`'s 4-key filter removed (Constraint 1); content body now reaches codec/placeholder with every sibling key intact.
- Known-kind `params` residual frozen at load time via Decision 4 (codec round-trip diff); `src/serialize/codec.cpp` and headers extended.
- L5 plumbing: `Document` owns an `UnknownFieldStore`; `ContentSnapshot` copies it on the writer thread (`src/runtime/document.cpp`, `src/runtime/document_serialize.cpp`, `src/runtime/arbc/runtime/document.hpp`, `src/runtime/arbc/runtime/document_serialize.hpp`).
- Three new claims in `tests/claims/registry.tsv`: `08-serialization#unknown-fields-preserved-at-every-tier`, `#preserved-unknown-never-shadows-known`, `#known-kind-params-unknowns-preserved`.
- New unit test `tests/serialize_unknown_fields.t.cpp`; corpus goldens at every tier and shadowing/params cases in `tests/serialize_determinism_corpus.t.cpp`; L5 golden lane extended in `tests/document_serialize_golden.t.cpp`; TSan concurrency lane in `tests/document_serialize_concurrency.t.cpp`; fuzz harness threaded in `tests/fuzz/fuzz_target.hpp`; 4 new fuzz seeds under `tests/fuzz/corpus/load_document/`; `tests/CMakeLists.txt` wired.
