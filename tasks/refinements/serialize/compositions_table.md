# serialize.compositions_table — In-document compositions table

## TaskJuggler entry

[`tasks/60-serialize.tji:54-59`](../../60-serialize.tji) — `serialize.compositions_table`:

> Document-level 'compositions' table (doc 08 Principle 7, analogous to
> 'contents'), keyed by core-owned non-semantic ids re-derived on save;
> reader/writer recurse into non-root compositions; nested content bodies name
> children via the core-owned 'composition' field beside kind/inputs/params;
> in-document cycles (Droste) round-trip as data; multi-composition model
> round-trip through the runtime sinks. Settles doc 05 Droste serialization.
> Source: tasks/parking-lot.md 2026-07-09 (org.arbc.nested codec design,
> resolution a). Docs 05/08.

## Effort estimate

**3d** (`tasks/60-serialize.tji:55`). Budget:

- ~0.75d — writer: composition reachability walk, ordinal assignment, the
  `compositions` table, the `"composition"` field on bodies.
- ~1d — reader: `CompResolver` (allocate-then-enqueue, cycle-safe), the
  multi-composition `load_baseline`, the two new error paths.
- ~0.5d — the seams: `Content::composition_ref()` (contract), `NestedContent`'s
  override, `PlaceholderContent`'s child reference, `DeserializeFn`'s new
  parameter, `Model::Transaction::add_composition(ObjectId, w, h)`.
- ~0.25d — runtime: `capture_snapshot`'s walk spans compositions.
- ~0.5d — goldens, claims, fuzz seeds, TSan extension.

It carries 3d against `sharing`'s 1d because `sharing` extended one traversal
(content → its inputs) while this one adds a *second, mutually recursive*
traversal (composition → its layers' contents → their child compositions) whose
cycles are **legal**, and because the read path has an ordering problem
`sharing` did not: a content cannot be constructed until its child
composition's `ObjectId` exists, and the composition's layers cannot be built
until their contents exist.

## Inherited dependencies

**Settled (formal `depends`):**

- `serialize.sharing` (`tasks/60-serialize.tji:33-38`, DONE 2026-07-09,
  [`sharing.md`](sharing.md)) — the `contents` table, `{"$ref": id}`, the
  first-encounter-ordinal id scheme re-derived on save (D2), the
  `ContentSink`→`SunkContent` and `ContentMetaProvider` seams, `RefResolver`,
  and the v1 acyclic-`$ref` commitment (D8). This task copies its id discipline
  wholesale and extends its two traversals.

**Settled (informal — the seams this task edits):**

- `serialize.kind_params` ([`kind_params.md`](kind_params.md)) — the
  serialize-owned `CodecTable` keyed by kind id, `DeserializeFn`,
  `PlaceholderContent`.
- `serialize.unknown_field_preservation` (DONE 2026-07-11,
  [`unknown_field_preservation.md`](unknown_field_preservation.md)) — landed a
  **sibling, not a predecessor** (it depends on `!format_tests`; the two were
  independently unblocked). Its Constraint 7 (`unknown_field_preservation.md:405-414`)
  is an explicit hand-off to this task: it wrote a TODO at `src/serialize/reader.cpp:42-45`
  saying whichever of the two lands second must add `compositions` to
  `k_root_keys` and `composition` to `k_body_keys`, or they will be preserved as
  unknowns *and* re-emitted by the core. This task lands second: **that TODO is
  a deliverable.**
- `runtime.document_serialize` / `runtime.operator_codecs` — `capture_snapshot`,
  `ContentSnapshot`, `KindBridge`, the L5 `ContentSink`.
- `kinds.nested` — `NestedContent`, which holds its child composition as a bare
  `ObjectId` (`src/kind_nested/arbc/kind_nested/nested_content.hpp:249`).

**Pending (none blocking):**

- `runtime.nested_codec` (`tasks/65-runtime.tji:90-95`, M8) is the **downstream**
  consumer, not a dependency — it `depends serialize.compositions_table`. It
  registers the `org.arbc.nested` codec (`KindBridge` intern, `builtin_codecs()`
  arm, deserialize constructing `NestedContent(child)`) and lands the end-to-end
  Droste golden through a live `Document`. This task ships the format and every
  seam that codec needs, and proves the whole path end-to-end through an
  **unknown-kind** nesting content (see Decision 6), which needs no codec at all.

**Downstream (this task unblocks):** `runtime.nested_codec`; M8
(`tasks/99-milestones.tji:65-66`).

## What this task is

Today the `.arbc` format models **exactly one, unkeyed composition** — the
writer finds it with `find_first_composition` (`src/serialize/writer.cpp:371`)
and emits it as the singular root key `root["composition"]`
(`src/serialize/writer.cpp:382`); the reader finds `root.find("composition")`
(`src/serialize/reader.cpp:481`) and calls `txn.add_composition` exactly once
(`src/serialize/reader.cpp:548`). A `NestedContent`'s child composition is
therefore unpersistable: it is an `ObjectId` living inside an L4 kind object,
invisible to the model (no `CompositionRecord` edge — `records.hpp:136-144`),
invisible to `Content::inputs()`, and with no place in the format to land.

This task makes a document a **graph of compositions**:

(a) **The format.** A document-level `compositions` table beside `contents`,
    keyed by core-owned non-semantic ordinals re-derived on every save; a
    core-owned `"composition": "<id>"` field on a content body beside
    `kind`/`inputs`/`params`. The root keeps its home at `root["composition"]`
    and holds the reserved ordinal `"0"`, so a Droste back-edge can name it.

(b) **The core-visible accessor.** `Content::composition_ref()` — a
    null-default discovery virtual in `contract`, the exact mirror of
    `inputs()` (doc 03:113-120 delta). `NestedContent` overrides it;
    `PlaceholderContent` carries one too, so a **missing plugin never orphans
    the composition it embeds**.

(c) **The writer.** One interleaved walk — compositions breadth-first from the
    root, each composition's layers bottom-to-top, each layer's content graph
    depth-first over `inputs()` — that assigns composition ordinals *and* the
    existing `contents` refcounts in first-encounter order. Cycle-safe by a
    visited set. Emits every reachable non-root composition into `compositions`.

(d) **The reader.** A `CompResolver` that pre-allocates each reachable
    composition's `ObjectId` **before** building its layers (which is what makes
    a cycle terminate), threads the resolved child `ObjectId` into
    `DeserializeFn`, and installs all N compositions in one `load_baseline`.

(e) **The model seam.** `Model::Transaction::add_composition(ObjectId, double, double)`
    — an overload taking a caller-pre-allocated id, so the reader can hand a
    child's `ObjectId` to a codec before any record exists.

(f) **The runtime.** `capture_snapshot`'s walk spans every reachable
    composition instead of the lowest-id one, so a multi-composition `Document`
    round-trips through the L5 sinks.

This task does **not** ship the `org.arbc.nested` codec, `KindBridge` intern, or
`builtin_codecs()` arm — that is `runtime.nested_codec`, which depends on this.
It does not touch the **external** `params.ref` child form (doc 08 Principle 3,
doc 05), which stays kind-owned loader territory. It does not make operator-input
(`$ref`) cycles legal: they remain `ReaderError::UnresolvableReference`
(`sharing.md` D8), and one of this task's tests exists precisely to prove the two
cycle notions do not contaminate each other.

## Why it needs to be done

Doc 05 is the project's headline feature ("Rendering *is* recursion",
`05:10-26`) and a Droste scene is called out as a *legitimate object*
(`05:60-80`). `kinds.nested` ships the renderer; `kinds.nested_runtime_binding`
ships the live binding. But none of it survives a save: `NestedContent` is
absent from `KindBridge` (`src/runtime/document_serialize.cpp:109-116`),
`builtin_codecs()` (`:142-149`), and `builtin_kind_of()` (`:81-103`), because
there was nothing in the format for a codec to write to. `runtime.operator_codecs`
shipped no nested codec and parked the question
(`tasks/parking-lot.md:145-149`); triage resolved it on 2026-07-10 to option
(a) — the in-document table — and doc 08 Principle 7 was written. This task is
the implementation of that decision, and the last format gap before M8's
"nested compositions round-trip in-document (compositions table, Droste
included)" (`tasks/99-milestones.tji:65-66`).

## Inputs / context

### Design docs (normative, doc 16)

- **`docs/design/08-serialization.md` Principle 7 (`08:157-190`)** — the
  governing text. Child compositions are document-local by default; the
  `compositions` table; the core-owned `"composition": id` field; ids re-derived
  on save in first-encounter order; cycles serialize directly; dangling id is an
  error value. **Amended by this task** (see Decisions 2, 3, 6): the accessor,
  the root's reserved ordinal `"0"`, the reserved-key error, ignore-on-load of
  unreachable entries.
- **Principle 6 (`08:129-156`)** — the `contents`/`$ref` discipline this table
  is "analogous to": order-significant `inputs`, omit-when-empty, hoist at ≥2
  references, non-semantic first-encounter ordinals, dangling ref is an error
  value, `$ref` graphs are acyclic.
- **Principle 4 (`08:88-115`)** — unknown fields at every tier; and the clause
  that already governs this table: "The `contents` and `compositions` tables are
  core-owned id-keyed maps, not sibling surfaces: an entry no reference reaches
  is dropped on save."
- **Principle 2 (`08:75-81`)** — "A missing plugin must never destroy data."
  Load-bearing for Decision 6.
- **Principle 5 (`08:116-128`)** — canonical output.
- **`docs/design/05-recursive-composition.md:54-61`** (persistence, amended) and
  **`:60-80`** (Cycles: "cycles are representable … and even *meaningful*").
- **`docs/design/03-layer-plugin-interface.md:102-121`** — the `Content`
  operator-graph accessors; `composition_ref()` added here (delta).
- **`docs/design/17-internal-components.md:53,58`** — `contract` (L3) may name
  `model` and `base`; `arbc::serialize` (L4) depends on `contract`, `model`. The
  accessor is legal at L3: `ObjectId` lives in `base`
  (`src/base/arbc/base/ids.hpp:11`).
- **`docs/design/00-overview.md:108-113`** — the decision record already names
  "in-document child compositions in a core-owned `compositions` table (Droste
  cycles serialize)". No new bullet (Decision 9).

### Source seams

Writer (`src/serialize/`):

| seam | file:line | change |
|---|---|---|
| root discovery | `writer.cpp:369-371` (`find_first_composition`) | becomes the *seed* of a composition worklist |
| `ContentGraph::build` | `writer.cpp:147-168` | walks N compositions; assigns composition ordinals alongside `contents` ordinals |
| `ContentGraph::visit` | `writer.cpp:201-227` | after counting a content, enqueues `c->composition_ref()` |
| `ContentGraph::emit_definition` | `writer.cpp:229-255` | appends `body["composition"]` next to where it appends `body["inputs"]` (`:246-253`) |
| `composition_json` | `writer.cpp:321-352` | already parameterized by `comp_id` — called once per composition, unchanged |
| envelope build | `writer.cpp:382-388` | `root["compositions"]` emitted beside `root["contents"]` when non-empty |
| `ContentBody`/`ContentMeta` | `writer.hpp:65-101` | **unchanged** — the writer reads the reference off `Content` directly (Decision 1) |

Reader (`src/serialize/`):

| seam | file:line | change |
|---|---|---|
| key sets + TODO | `reader.cpp:42-45, 51, 60` | `compositions` → `k_root_keys`; `composition` → `k_body_keys`; delete the TODO |
| `parse_composition` | `reader.cpp:283-329` | already takes a bare `const json&` → reused per table entry, unchanged |
| `RefResolver::build` | `reader.cpp:386-424` | reads the body's `composition`, resolves it, threads it into `content_body_from_json` |
| singular composition | `reader.cpp:479-488` | root becomes composition `"0"` of a `CompResolver` |
| `load_baseline` closure | `reader.cpp:543-581` | loops over N resolved compositions |
| `DeserializeFn` | `deserialize.hpp:36-37` | gains `ObjectId composition` |
| `content_body_from_json` | `codec.hpp:99-102`, `codec.cpp:33-102` | gains `ObjectId composition`; `stored.erase("composition")` beside the existing `stored.erase("inputs")` (`codec.cpp:97-101`) |
| `PlaceholderContent` | `placeholder_content.hpp:45-46` | ctor takes the child `ObjectId`; overrides `composition_ref()` |

Contract / model / kinds / runtime:

- `src/contract/arbc/contract/content.hpp:587` — `inputs()`, the null-default
  discovery virtual `composition_ref()` sits beside (with `editable()` :570,
  `audio()` :580 as the pattern).
- `src/model/model.cpp:648-675` — `Transaction::add_composition`, whose body
  already begins `const ObjectId id = d_model->allocate_id();`; the overload
  factors that line out. `Model::allocate_id()` (`model.hpp:190`,
  `model.cpp:537`) is a bare monotonic `fetch_add` — **it mutates no `DocState`**,
  which is what makes pre-allocation safe (Decision 4).
- `src/model/arbc/model/model.hpp:51` — `DocRoot::find_composition(ObjectId)`,
  unused by serialize today; the writer starts calling it.
- `src/kind_nested/arbc/kind_nested/nested_content.hpp:161,249` — `child()` and
  `d_child`; the override is one line.
- `src/runtime/document_serialize.cpp:153-246` — `capture_snapshot`. Line 165
  (`find_first_composition`) seeds a composition worklist; the layer walk
  (`:176-199`) runs per composition; the `inputs()` walk (`:214-244`) also
  enqueues `composition_ref()` targets. Line 193's comment ("unknown → empty
  views (placeholder body wins)") is the fact Decision 6 leans on: **an
  unknown-kind layer root already survives the L5 save path.**

**Predecessor / sibling refinements:** `sharing.md` D2 (ordinal ids re-derived
on save), D6 (intra-document `$ref` ≠ `LoadContext` cross-file dedup), D8
(acyclic `$ref`); `unknown_field_preservation.md` C7 (the key-set hand-off), D3
(stashes keyed by `ObjectId`, never `Content*`), D5 (the id-keyed tables are not
a sibling surface); `reader.md` D3 (`load_baseline`, model-unmutated-on-error);
`format_tests.md` D2 (the differential-determinism fuzz invariant), D3 (the
on-disk corpus).

## Constraints / requirements

1. **The composition reference is core-owned and never rides `params`.** Doc 08
   P7. The writer re-derives the emitted id from graph structure on every save;
   the kind's codec neither writes nor reads it. Concretely: `SerializeFn`
   (`codec.hpp:40`) is **unchanged** — the writer appends `"composition"` after
   the codec returns, exactly as it appends `"inputs"` (`writer.cpp:246-253`) —
   and `content_body_from_json` strips `composition` from a placeholder's stored
   body, exactly as it strips `inputs` (`codec.cpp:97-101`).

2. **Ids are non-semantic, re-derived, and stringly-keyed.** Decimal-string
   first-encounter ordinals over the canonical traversal, like `contents`
   (`sharing.md` D2). A hand-authored file's arbitrary composition keys
   (`"main"`, `"zzz"`) normalize to ordinals on re-serialization —
   canonicalization, not data loss. `"0"` is the root and is never a key in
   `compositions`.

3. **The canonical traversal is one walk, and it is cycle-safe.** Compositions
   breadth-first in first-encounter order from the root; within a composition,
   layers bottom-to-top (`for_each_layer_in`, membership order); within a layer,
   the content graph depth-first over `inputs()` in declared order. A visited set
   keyed by composition `ObjectId` bounds it. The same walk assigns composition
   ordinals and `contents` refcounts, so the two id spaces are derived from one
   deterministic order and output stays byte-stable (P5).

4. **`contents` sharing spans compositions.** A content reached from two
   different compositions is referenced ≥2 times and therefore hoists into
   `contents` under one `{"$ref": id}` (P6, `sharing.md` D2) — the refcount
   pre-pass must count across the whole reachable graph, not per composition.

5. **The model is unmutated on any read error.** `reader.md` D3 and the
   `#load-installs-version-0-baseline` claim. All composition and `$ref`
   resolution completes before `load_baseline` (`reader.cpp:543`).
   `Model::allocate_id()` is the *only* model call the resolution phase makes,
   and it installs no record: after a failed load the target `Model` still has
   `revision() == 0`, `contains(id) == false` for every allocated id, and no
   `CompositionRecord`.

6. **Errors stay values; the loader never faults.** Two new error paths, both
   surfaced before any mutation: a `composition` id absent from the table
   (and not `"0"`) → `ReaderError::UnresolvableReference` at
   `/compositions/<id>`; a `compositions` entry keyed `"0"` →
   `ReaderError::MalformedField` at `/compositions/0`. A non-object
   `compositions`, a non-string `composition`, a non-object table entry: all
   `MalformedField`. No nlohmann exception on any input
   (`#loader-never-faults-on-hostile-input`).

7. **A composition cycle is legal; an operator-input cycle is not.** The
   `CompResolver`'s visited set must return a pre-allocated `ObjectId`
   *immediately*, without building that composition's layers — otherwise a
   nesting content shared across the cycle re-enters `RefResolver`'s
   `d_in_progress` set (`reader.cpp:331-438`) and a legal Droste is misreported
   as an `UnresolvableReference`. Composition resolution and `$ref` resolution
   keep separate visited/in-progress state.

8. **The root is the lowest-id composition.** The model's existing rule
   (`model.hpp:60`, and what `working_space()`/`working_audio_format()`/
   `capture_snapshot` already assume). The reader **guarantees** it by allocating
   the root's `ObjectId` before any child's; a loaded document therefore always
   satisfies it. Programmatic `Document` construction must create the parent
   before the child (`runtime.nested_codec` does).

9. **Unknown fields, every tier, including the new ones.** `compositions` joins
   `k_root_keys`; `composition` joins `k_body_keys` (`unknown_field_preservation.md`
   C7). A non-root composition's unknown siblings stash under
   `UnknownScope::Composition` keyed by **its own** `ObjectId` — the scope
   already exists (`unknown_fields.hpp:41-46`) and was already `ObjectId`-keyed,
   so no new tier. The `compositions` map itself stays a non-surface (P4, D5).

10. **Levelization (doc 17).** `composition_ref()` is legal in `contract` (L3):
    it returns `ObjectId` from `base` (L0), and `contract` already depends on
    `model` (`src/contract/CMakeLists.txt`, doc 17:53). The JSON type stays
    private to `arbc::serialize` — `PUBLIC_HEADERS` gains nothing
    (`src/serialize/CMakeLists.txt:9-11`).

11. **The off-thread emit reads only the snapshot.** `capture_snapshot`'s new
    composition walk runs on the **writer thread** (it calls `doc.resolve`, the
    writer-thread-owned side map, `document_serialize.cpp:154-157`). The
    off-thread `serialize_snapshot` continues to read only immutable snapshot
    data plus the pinned `DocRoot`.

## Acceptance criteria

- **A two-composition document round-trips byte-exact.** New
  `tests/serialize_compositions.t.cpp`: an inline-raw-string golden with a root
  composition, one child in `compositions` under key `"1"`, and a nesting body
  carrying `"composition": "1"`. `serialize(load(x)) == x`; re-serialization is
  idempotent. Enforces the new claim
  `08-serialization#child-compositions-round-trip-in-document`.

- **Droste round-trips as data.** Two goldens: (i) a composition that embeds
  **itself** (`"composition": "0"`, and *no* `compositions` key at all — the
  root is the only reachable composition); (ii) A embeds B embeds A (table holds
  `"1"` = B, whose body carries `"composition": "0"`). Both load, terminate, and
  re-serialize byte-exact. A third case pins Constraint 7: a single nesting
  content **shared** between A and B (hoisted into `contents`, `$ref` at both use
  sites) loads successfully — a composition cycle closing through a `$ref` is not
  an operator-input cycle — while an actual `$ref` input cycle in the same
  document shape still returns `UnresolvableReference`. Enforces
  `08-serialization#droste-cycle-round-trips-as-data`; re-enforces
  `08-serialization#dangling-ref-is-read-error`.

- **A missing plugin never orphans a child composition.** `tests/serialize_compositions.t.cpp`
  and an L5 golden in `tests/document_serialize_golden.t.cpp`: an **unknown-kind**
  body (no registered codec → `PlaceholderContent`) carrying `"composition": "1"`
  loads, keeps the reference on `composition_ref()`, keeps the child composition
  reachable, and re-saves byte-exact with the core-re-derived id — through the
  full runtime path (`runtime::load_document` → `save_document`), which needs no
  nested codec. Enforces `08-serialization#unknown-kind-preserves-composition-reference`;
  re-enforces `08-serialization#unknown-kind-round-trips-verbatim`.

- **The read errors are values, with the model unmutated.** A dangling
  `composition` id → `ReaderError::UnresolvableReference`; a `compositions` entry
  keyed `"0"` → `ReaderError::MalformedField`. Both assert `revision() == 0`, no
  `CompositionRecord` installed, and `REQUIRE_NOTHROW`. Enforces
  `08-serialization#dangling-composition-is-read-error`; re-enforces
  `08-serialization#load-installs-version-0-baseline`.

- **Canonicalization holds across the new id space.** A hand-authored document
  with arbitrary composition keys (`"main"`, `"zzz"`), unsorted keys, and an
  **unreachable** table entry re-serializes to ordinals `"1"`…`"N"` with the
  unreachable entry dropped. Added to `tests/serialize_determinism_corpus.t.cpp`;
  re-enforces `08-serialization#hand-authored-ids-normalize-deterministically`.

- **Sharing spans compositions.** A content used by a layer in the root *and* a
  layer in the child hoists into `contents` under one `{"$ref": id}` and reloads
  to **one** live `Content` (pointer identity asserted). Added to
  `tests/serialize_sharing.t.cpp`; re-enforces
  `08-serialization#shared-content-dedups-via-ref`.

- **Unknown fields survive at the new tiers.** An unknown sibling beside `layers`
  inside a `compositions` entry, and an unknown sibling beside `composition` in a
  nesting body, both round-trip byte-exact and never shadow a known key. Added to
  `tests/serialize_unknown_fields.t.cpp`; re-enforces
  `08-serialization#unknown-fields-preserved-at-every-tier` and
  `#preserved-unknown-never-shadows-known`.

- **The multi-composition model round-trips through the runtime sinks.** A
  `Document` holding two compositions (built with a test-double
  composition-reference kind interned through `KindBridge` and a codec injected
  into `serialize_snapshot`'s `CodecTable` — `tests/` sits outside the
  levelization graph, `format_tests.md` D4) captures **both** compositions'
  layer-root contents into the `ContentSnapshot` and emits them. Plus the
  unknown-kind end-to-end case above. New `tests/document_compositions_golden.t.cpp`.

- **Fuzz.** Three seeds added to `tests/fuzz/corpus/load_document/` (a Droste
  self-cycle, a two-composition document, a dangling `composition` id); the
  portable corpus-replay (`tests/fuzz_corpus_replay.t.cpp`) runs them under
  gcc+ASan on every push, holding the differential-determinism fixed point
  (`format_tests.md` D1/D2). Re-enforces
  `08-serialization#loader-never-faults-on-hostile-input`.

- **Concurrency.** `tests/document_serialize_concurrency.t.cpp` gains a
  multi-composition document: `capture_snapshot` on the writer thread while
  transactions commit concurrently, then `serialize_snapshot` off-thread; TSan
  clean, and the emitted bytes match the pinned revision
  (`#writer-serializes-the-pinned-version`).

- **Contract default.** `src/contract/t/operator_members.t.cpp` asserts a plain
  `Content` reports `composition_ref() == ObjectId{}` (the null default, like
  `inputs().empty()`); `tests/nested_conformance.t.cpp` asserts
  `NestedContent::composition_ref() == child()`. No conformance-suite change is
  otherwise needed — this task ships no kind.

- **Coverage.** ≥90% diff coverage on changed lines (CI gate).

- **Design-doc deltas ride this commit** (doc 16 same-commit rule):
  `docs/design/03-layer-plugin-interface.md:113-120` (the accessor),
  `docs/design/08-serialization.md:157-190` (Principle 7: the accessor, the
  root's reserved ordinal `"0"`, the reserved-key error, ignore-on-load of
  unreachable entries, composition-vs-`$ref` cycle separation),
  `docs/design/05-recursive-composition.md:54-61` (the accessor + `"0"`
  back-edge). **Written by this refinement; the closer commits them with the
  code.**

- **Deferred, with a concrete home (no new WBS leaf needed):** the
  `org.arbc.nested` codec itself — `KindBridge` intern, the `builtin_codecs()`
  arm, `builtin_kind_of()`'s nested arm, the `DeserializeFn` constructing
  `NestedContent(child)`, absorbing `binder_nested.cpp` into `codec_nested.cpp`,
  and the end-to-end Droste golden through a live `Document` with a *real*
  nested kind — is already `runtime.nested_codec` (`tasks/65-runtime.tji:90-95`,
  M8), which formally depends on this task. Nothing to register.

## Decisions

1. **The core reads the child reference off a `Content` accessor
   (`composition_ref()`), not off a new provider callback.** Doc 08 P7 says the
   reference is "core-owned because the reference is graph structure, exactly
   like `inputs`" — and `inputs` *is* a core-visible `Content` accessor
   (`content.hpp:587`, doc 03:107, doc 13:49-52). Making the composition
   reference one too is the doc's own analogy carried to its conclusion. It costs
   a null-default discovery virtual beside the three that already exist
   (`editable()`, `audio()`, `inputs()`), and it buys: the L4 writer reads it
   directly (`arbc::serialize` already depends on `contract`), so **no new
   `std::function` seam and no change to `ContentBody`/`ContentMeta`
   (`writer.hpp:65-101`)**; `capture_snapshot`'s composition walk is kind-agnostic
   (no `dynamic_cast` chain to extend); and `PlaceholderContent` can carry a
   child reference for a kind this build cannot even load (Decision 6).
   *Rejected — a new field on `ContentMeta` (`writer.hpp:83-93`) filled by L5's
   `ContentMetaProvider`:* works for the writer, but leaves `capture_snapshot`
   needing a runtime-internal `dynamic_cast` to `NestedContent` to *build* the
   value, which pins the generic multi-composition machinery to one concrete kind
   and gives out-of-tree nesting plugins no way in.
   *Rejected — a fifth `std::function` parameter to `serialize_document`:* pure
   cost; the data is already reachable through a type `serialize` may name.
   *Rejected — a `CompositionRefFn` on `Codec`:* the codec table is keyed by kind
   id and consulted only for `params`; a structural edge that the core re-derives
   does not belong behind the kind-owned seam (Constraint 1).

2. **The root holds the reserved ordinal `"0"` and is never a key in
   `compositions`.** P7 as written ("A document holds exactly one root
   `composition`; every *other* composition … lives in an optional
   document-level `compositions` table") gives a cycle nowhere to land: in "A
   embeds B embeds A" with A the root, B's body must *name A*, and A has no id.
   Extending the id space over every reachable composition — root first,
   therefore always `"0"` — costs nothing (the ordinals are re-derived anyway),
   keeps the table's contents exactly as P7 describes (`"1"`…`"N"`, the non-root
   ones), and makes the back-edge spell `"composition": "0"`. A document with no
   nesting emits no `compositions` key at all. **Doc 08 P7 delta.**
   *Rejected — a magic token (`"composition": "root"`):* introduces a semantic
   name into an id space P7 declares non-semantic, and collides with a
   hand-authored key of the same spelling.
   *Rejected — putting the root in the table too and replacing `root["composition"]`
   with a `"root": "<id>"` pointer:* a larger, gratuitous format change that
   contradicts P7's "A document holds exactly one root `composition`" and
   reshapes every existing golden.
   *Rejected — emitting the root's body twice (inline **and** in the table):*
   duplicated data, two ways to say one thing, and a fixed-point hazard for the
   fuzzer.

3. **A `compositions` entry keyed `"0"` is a `ReaderError::MalformedField`, not a
   silent drop.** P4 blesses dropping *unreachable* table entries on save, and a
   `"0"` entry is unreachable by construction (`"0"` always resolves to the root)
   — so dropping it would be defensible. But a hand-author who wrote a
   composition under key `"0"` meant something by it, and the quiet outcome is
   the deletion of a whole composition. The reader already errors on structural
   ambiguity (dangling `$ref`, cycle-closing `$ref`); rejecting a document that
   claims the root's reserved ordinal is cheap, testable, fuzz-safe (an error is
   a value), and tells the author to renumber. **Doc 08 P7 delta.**
   *Rejected — silently shadowing it:* the one outcome doc 08 P2/P4 are most
   anxious about (data loss without a diagnostic).

4. **The reader pre-allocates composition `ObjectId`s with `Model::allocate_id()`
   and installs them via a new `Transaction::add_composition(ObjectId, w, h)`
   overload.** The read path has a genuine ordering knot: a nesting `Content`
   cannot be constructed without its child's `ObjectId`
   (`NestedContent::NestedContent(ObjectId)`, `nested_content.hpp:64`), but the
   composition's `CompositionRecord` cannot exist before its layers, which cannot
   exist before their contents. `Model::allocate_id()` (`model.cpp:537`) cuts it:
   a bare monotonic `fetch_add` that installs no record, so the reader can mint
   the id, hand it to the codec, and materialize the record later inside
   `load_baseline` — with **zero `DocState` mutation before validation**
   (Constraint 5). `Transaction::add_composition` already calls `allocate_id()`
   as its first line (`model.cpp:648-652`); the overload just factors it out. Ids
   are never reused (no free list — `unknown_field_preservation.md` D3 leans on
   the same fact), so a pre-allocated id cannot alias.
   *Rejected — mutating the model early (a pre-pass transaction creating the
   compositions before contents):* would break `reader.md` D3's
   model-unmutated-on-error guarantee and the `#load-installs-version-0-baseline`
   claim — a dangling `$ref` after a successful composition pre-pass would leave
   stray `CompositionRecord`s behind.
   *Rejected — constructing contents with an unset child and patching afterwards:*
   needs a mutator on the `Content` interface (or a `Content::bind_composition()`
   hook), putting a serialize concern into L3 for the sake of one kind.
   *Rejected — persisting the model `ObjectId` as the table key:* contradicts P7
   (non-semantic ids re-derived on save) and doc 14:77-84 ("Identity is runtime
   state, not (yet) file format state").

5. **`DeserializeFn` gains an `ObjectId composition` parameter; `SerializeFn` does
   not change.** Asymmetric on purpose, and correctly so: on load the codec
   *must* receive the resolved child id (only it knows how to build its kind
   around one); on save the core re-derives the id itself from
   `composition_ref()` and appends it to the body after the codec returns, so a
   codec that tried to emit one would be overwritten. This is exactly the shape
   `sharing` chose for `inputs` (D3/D4: `DeserializeFn` grew
   `std::span<const ContentRef> inputs`; `SerializeFn` did not grow anything, and
   the core re-derives the `inputs` array from `Content::inputs()`). One
   signature, one precedent, no new concept.
   *Rejected — passing the whole composition table / a resolver callback into the
   codec:* hands a kind the keys to the document graph to solve a problem the
   core has already solved by the time the codec runs.

6. **`PlaceholderContent` carries a composition reference, and `composition` is
   stripped from its verbatim body.** Doc 08 P2: "A missing plugin must never
   destroy data." An unknown nesting kind (a third-party `com.example.nest`)
   names a child composition. If the placeholder stored `"composition": "3"`
   verbatim and the core knew nothing of it, the child would be unreachable from
   the writer's walk, **dropped from the table on save, and the reference left
   dangling** — a missing plugin silently deleting a composition, the exact
   failure P2 forbids. So the reader resolves the placeholder's `composition` id
   like any other body's, hands the child `ObjectId` to the `PlaceholderContent`,
   and `content_body_from_json` erases `composition` from the stored body — the
   same treatment `inputs` already gets (`codec.cpp:97-101`), for the same
   reason: it is core-owned and the writer re-derives it. This also hands the task
   a *real* end-to-end runtime proof with no nested codec in existence
   (`capture_snapshot` already keeps unknown-kind layer roots —
   `document_serialize.cpp:190-196`, and `content_body_to_json` short-circuits on
   the placeholder — `codec.cpp:114`). **Doc 08 P7 delta.**
   *Rejected — leaving the id verbatim in the placeholder's body:* the writer
   would emit a stale ordinal pointing at nothing (or, worse, at a *different*
   composition after renumbering), and the fuzz fixed-point invariant would fail
   on any document with both an unknown nesting kind and ≥2 compositions.

7. **Composition ordinals and `contents` ordinals are assigned by one walk, and
   the composition worklist is breadth-first while the content walk stays
   depth-first.** The two id spaces must both be functions of one deterministic
   traversal or the output is not byte-stable (P5). Compositions are enqueued on
   first encounter and their layers built only when popped — which is not merely
   an ordering preference but the thing that makes a cycle terminate on **both**
   sides (Constraint 7): on read, the child's `ObjectId` is available the instant
   it is enqueued, so a nesting content whose child is still unbuilt constructs
   fine, and a nesting content *shared* across the cycle never re-enters
   `RefResolver`'s in-progress set.
   *Rejected — resolving a composition's layers inline at first encounter
   (depth-first compositions):* deadlocks exactly that case — a legal Droste
   would be reported as an operator-input cycle (`UnresolvableReference`).

8. **The root stays "the lowest-id composition" (`find_first_composition`,
   `model.hpp:60`); the reader guarantees the invariant by allocating the root's
   id first.** The model has no root marker, and adding one means a new field on
   the fixed-layout, mmap-backed `CompositionRecord` (`records.hpp:136-144`,
   doc 15) plus a doc 14 delta — an order of magnitude more than this task, for a
   property the reader can simply establish. Every consumer already assumes
   lowest-id-wins (`working_space()`, `working_audio_format()`,
   `capture_snapshot`), so keeping it means no consumer changes.
   *Rejected — an explicit root field on `DocRoot` / `CompositionRecord`:* record
   layout is workspace-file shape (doc 15); a bigger, riskier change than the
   problem justifies today. The residual hazard is narrow — a programmatic
   `Document` that creates a *child* composition before its root would serialize
   the child as the root — and is a live authoring question, not a load-path one.
   **Surfaced for `tasks/parking-lot.md`** (whether the model should carry an
   explicit root marker instead of lowest-id-wins) rather than encoded as a WBS
   task: it is a doc 14 / record-layout judgment call, not a closeable leaf.

9. **No doc 00 decision-record bullet.** `docs/design/00-overview.md:108-113`
   already records the project-shaping decision — "in-document child compositions
   in a core-owned `compositions` table (Droste cycles serialize), external
   references by URI. Decided in doc 08" — landed by the 2026-07-10 triage. This
   task implements it; the deltas here (the accessor, the root's ordinal, the
   reserved-key error) are format and interface detail belonging to docs 03/08,
   consistent with all seven sibling serialize refinements, which each declined a
   doc 00 bullet on the same grounds. The new `Content` virtual is not an ABI
   event either: doc 03:172-177 puts v1 explicitly at "stage 1 — C++ interface +
   `dlopen`", with the stable C ABI shim deferred "once the C++ interface stops
   churning".

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-11.

- `src/contract/arbc/contract/content.hpp` — `composition_ref()` null-default discovery virtual added beside `inputs()`.
- `src/model/arbc/model/model.hpp`, `src/model/model.cpp` — `Transaction::add_composition(ObjectId,w,h)` overload (pre-allocation path for the reader).
- `src/serialize/writer.cpp`, `src/serialize/reader.cpp`, `src/serialize/codec.cpp`, `src/serialize/placeholder_content.cpp` — interleaved BFS composition walk, `CompResolver`, `compositions` table emit/parse, `composition` body field strip; `compositions`/`composition` added to `k_root_keys`/`k_body_keys` (TODO from `unknown_field_preservation` C7 fulfilled).
- `src/serialize/arbc/serialize/codec.hpp`, `src/serialize/arbc/serialize/deserialize.hpp`, `src/serialize/arbc/serialize/placeholder_content.hpp`, `src/serialize/arbc/serialize/unknown_json.hpp` — `DeserializeFn` gains `ObjectId composition`; `PlaceholderContent` carries and overrides `composition_ref()`.
- `src/kind_nested/arbc/kind_nested/nested_content.hpp` — one-line `composition_ref()` override returning `child()`.
- `src/runtime/document_serialize.cpp`, `src/runtime/codec_solid.cpp`, `src/runtime/codec_tone.cpp`, `src/runtime/codec_fade.cpp`, `src/runtime/codec_crossfade.cpp` — `capture_snapshot` walks all reachable compositions; codecs updated for new `DeserializeFn` signature.
- `tests/serialize_compositions.t.cpp` — new: two-composition round-trip, Droste self-cycle, A↔B cycle, shared-content hoist across compositions, unknown-kind placeholder preserving child, both read-error paths with `revision()==0`.
- `tests/document_compositions_golden.t.cpp` — new: multi-composition runtime path golden (L4 + L5 codec), unknown-kind end-to-end.
- `tests/fuzz/corpus/load_document/{droste_self_cycle,two_compositions,dangling_composition}.arbc` — three new fuzz seeds.
- `tests/{CMakeLists.txt,claims/registry.tsv}` — four new claims registered; new test targets wired.
- `tests/{serialize_sharing,serialize_kind_params,serialize_determinism_corpus,serialize_unknown_fields,document_serialize_concurrency,nested_conformance}.t.cpp` — existing suites extended with multi-composition cases.
- `src/contract/t/operator_members.t.cpp` — asserts `composition_ref() == ObjectId{}` null default.
- `docs/design/03-layer-plugin-interface.md`, `docs/design/05-recursive-composition.md`, `docs/design/08-serialization.md` — same-commit doc deltas (accessor, `"0"` back-edge, reserved-key error, unreachable-entry drop, cycle separation).
- `tasks/parking-lot.md` — Decision 8 follow-up appended (explicit root-composition marker vs. lowest-id-wins).
