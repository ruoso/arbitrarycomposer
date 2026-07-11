# runtime.nested_codec — `org.arbc.nested` codec over the compositions table

## TaskJuggler entry

[`tasks/65-runtime.tji:90-95`](../../65-runtime.tji) — `runtime.nested_codec`,
milestone **M8** (`m8_persistence`, `tasks/99-milestones.tji:62-65`).

```
task nested_codec "org.arbc.nested codec over the compositions table" {
  effort 2d
  allocate team
  depends !operator_codecs, serialize.compositions_table
  note "Register the org.arbc.nested serialize/deserialize codec: persist a nested
  content's in-document child through the doc 08 compositions table (core-owned
  composition reference), rebuild the child composition + NestedContent binding on
  load, end-to-end byte-exact goldens incl. a Droste cycle; external params.ref
  children remain loader territory (doc 05). Source: tasks/parking-lot.md 2026-07-09
  (nested serialization format decision); tasks/refinements/runtime/operator_codecs.md
  Decision 1. Docs 05/08."
}
```

## Effort estimate

**2d.** Budget:

- **~0.25d** — the codec TU itself. `serialize_nested` returns an empty `params`
  object; `deserialize_nested` is `make_unique<NestedContent>(composition)`. The
  format work is already done (`serialize.compositions_table`); this is the
  thinnest codec in the tree.
- **~0.25d** — the four registration sites (`k_nested_kind_version`,
  `builtin_kind_of`, `KindBridge`, `builtin_codecs()`), plus absorbing
  `binder_nested.cpp` into `codec_nested.cpp`.
- **~0.75d** — the **derived-inputs fix** (Decision 1): the writer must stop
  descending and emitting a nesting content's `inputs()`. This is the one piece
  of real work; without it a *real* `NestedContent` (unlike the unattached test
  double the predecessor used) corrupts its own document. Carries the doc 08
  delta and the reader's both-edges rejection.
- **~0.75d** — tests: the end-to-end byte-exact goldens (plain nesting + Droste),
  the attach-invariance golden that pins Decision 1, the error cases, the fuzz
  seed, and the claims rows.

## Inherited dependencies

**Settled:**

- **`serialize.compositions_table`** (done 2026-07-11,
  [`tasks/refinements/serialize/compositions_table.md`](../serialize/compositions_table.md))
  — landed the entire format and both walkers. The `compositions` table, the
  core-owned `"composition"` body field, the root's reserved ordinal `"0"`, the
  Droste back-edge, `Content::composition_ref()`, the `ObjectId composition`
  parameter on `DeserializeFn`, `Model::Transaction::add_composition`, and the
  reader's `CompResolver`. It proved the format end-to-end with an **unknown-kind**
  nesting content (`tests/document_compositions_golden.t.cpp:66-101`, a `NestKind`
  double) precisely because the real codec was this task. It explicitly deferred to
  us (`compositions_table.md:415-421`): the `KindBridge` intern, the
  `builtin_codecs()` arm, `builtin_kind_of()`'s nested arm, the `DeserializeFn`
  constructing `NestedContent(child)`, absorbing `binder_nested.cpp`, and "the
  end-to-end Droste golden through a live `Document` with a *real* nested kind".
- **`runtime.operator_codecs`** (done 2026-07-09,
  [`operator_codecs.md`](operator_codecs.md)) — the codec-registration seam we
  extend: built-in codecs live in runtime TUs linking `nlohmann::json` PRIVATE
  (D2), the codec owns **only `params`** (D3), and input children are adopted in
  place by the sink (D4). Its **Decision 1** deferred nested exactly here, and the
  parking-lot entry it raised (`tasks/parking-lot.md:145-149`) was resolved on
  2026-07-10 in favour of the in-document table — the decision this task executes.
- **`kinds.nested_runtime_binding`** (done 2026-07-11,
  [`../kinds/nested_runtime_binding.md`](../kinds/nested_runtime_binding.md)) —
  `NestedContent::attach/detach`, the `OperatorBinder` registry, and
  `src/runtime/binder_nested.cpp`, whose **Decision 5** says outright that
  "`nested_codec` can trivially absorb `binder_nested.cpp` when it lands". Not a
  declared `depends` (it is M9 work that landed early); we consume it as a fact on
  disk, not as an edge.

**Pending:** none. Every seam this task needs is on `main`.

## What this task is

Ship the `org.arbc.nested` serialize/deserialize codec so a real
`NestedContent` — not a test double — round-trips through a `.arbc` file. Six
pieces:

(a) A new `src/runtime/codec_nested.cpp`: `serialize_nested` (empty `params`) and
    `deserialize_nested` (`NestedContent(composition)`), plus
    `register_nested_binder()` moved in from `binder_nested.cpp`, which is deleted.

(b) The four registration sites in `builtin_codecs.hpp` /
    `document_serialize.cpp` that today know Solid/Tone/Fade/Crossfade and not
    Nested: `k_nested_kind_version`, `builtin_kind_of()`, `KindBridge`'s
    pre-intern, and `builtin_codecs()`.

(c) **The derived-inputs fix** (Decision 1): the writer must not descend or emit
    the `inputs()` of a content that names a resolvable child composition. Today
    it does, which means an *attached* nested content — the state every render
    driver leaves it in — silently corrupts the document it is saved into.

(d) The reader's corollary: a content body carrying **both** `composition` and
    `inputs` is a `MalformedField` read error.

(e) End-to-end byte-exact goldens through a live `Document` + `save_document` /
    `load_document`: a plain nesting document, a **Droste** self-cycle, and the
    attach-invariance proof that pins (c).

(f) Four claims-register rows and a fuzz seed.

It is **not**: the external `params.ref` child loader (doc 05 keeps that in loader
territory — see Acceptance criteria for the follow-up we name), and not any change
to the `compositions` table format, which is settled and shipped.

## Why it needs to be done

`m8_persistence` promises "nested compositions round-trip in-document
(compositions table, Droste included)". The *format* half of that promise shipped
in `serialize.compositions_table`; the *kind* half is this task. Until it lands,
`org.arbc.nested` has no registered codec, so `builtin_kind_of()` returns `false`
for it, the meta provider answers `nullopt`, and a real nested content serializes
as an **unknown-kind placeholder** — it survives, but only as opaque bytes, and a
document authored programmatically (with no stored body to re-emit) loses it
entirely. M8 cannot close.

It also closes a latent corruption. `serialize.compositions_table` proved the
format with a `NestKind` double whose `inputs()` is always empty. A real
`NestedContent`'s `inputs()` is **memo-derived from the child composition's
layers** (`nested_content.cpp:126-140`) and is non-empty **whenever it is
attached** — which is exactly the state `bind_operators` leaves it in for every
frame of an interactive session or an offline export. The writer descends and
emits `inputs()` unconditionally (`writer.cpp:267`, `:302-309`), so a save taken
while a binding scope is live:

- **hoists the child's layer contents into `contents` behind `$ref`s** they never
  earned (they are now referenced twice: once as layers of the child composition,
  once as nested's inputs) — different bytes for the same scene depending on
  whether a render happened to be in flight;
- and for a **Droste** scene, emits a `$ref` that closes an **operator-input
  cycle**, which doc 08 Principle 6 forbids and the reader's `RefResolver`
  rejects as `UnresolvableReference` — a save that produces an **unloadable
  file** for the one scene the compositions table exists to round-trip.

The predecessor could not have caught this: its double had no derived inputs, and
no codec meant no real nested content ever reached the writer. This task is the
first that puts one there.

## Inputs / context

### Design docs (normative, doc 16)

- **`docs/design/08-serialization.md:157-190`** — Principle 7, the compositions
  table: root ordinal `"0"` reserved and never a `compositions` key; the
  core-owned `"composition": id` body field read off `composition_ref()`, never in
  `params`; a Droste back-edge spelled `"composition": "0"`; a `composition` id
  absent from the table is a read error; a table entry no reference reaches is
  dropped on save (canonicalization). **Amended by this task** — see the
  design-doc delta below.
- **`docs/design/08-serialization.md:129-156`** — Principle 6, `contents` and
  `$ref`: shared-two-or-more hoisting, ids re-derived per save, and (`:148-156`)
  "a `{"$ref": id}` that closes an operator-input cycle is a serialization error"
  — the rule an attached nested currently violates.
- **`docs/design/08-serialization.md:88-115`** — Principle 4, unknown-field
  preservation at every tier. Nested's codec consumes no `params`, so the
  load-time residual diff (`src/serialize/codec.cpp:79-82`) preserves any key a
  hand-authored file put there — including a `params.ref` — for free.
- **`docs/design/08-serialization.md:116-128`** — Principle 5, determinism: sorted
  keys, shortest-round-trip numbers, errors as values.
- **`docs/design/05-recursive-composition.md:54-61`** — the persistence split: "an
  in-document child serializes into the document-level `compositions` table, the
  nested content naming it by a core-owned id … while an external child stays a
  kind-owned `params.ref` URI resolved through the loader."
- **`docs/design/05-recursive-composition.md:63-83`** — cycles are representable
  and meaningful; termination is a *render-time* property, severed from
  representation by doc 08:178-180.
- **`docs/design/03-layer-plugin-interface.md:113-120`** — `composition_ref()` as
  the kind-agnostic mirror of `inputs()`.

### Design-doc delta (this task, same commit — doc 16's rule)

**`docs/design/08-serialization.md`, Principle 7** gains a closing paragraph
making the derived-inputs rule normative: a nesting content's input edges are a
*projection* of its child composition, not authored data; a content answering a
non-null resolvable `composition_ref()` emits **no `inputs` array** and the write
traversal does not descend its `inputs()`; loading rebuilds the projection from
the child composition. Its corollary is a rule on kinds — **a kind names its child
through `composition_ref()` or takes authored `inputs`, never both** — and a body
carrying both is a read error.

This is a clarification in spirit (Principle 6 already says composition cycles are
"neither a `contents`/`$ref` edge") but it is normative in effect: it changes what
the writer emits, so it rides the doc, not just the refinement. **No doc-00
bullet** — it settles a mechanism inside an already-recorded decision (the
2026-07-10 parking-lot resolution, already summarized at `00-overview.md:111-112`),
not a project-shaping choice. The same edit corrects a stale cross-reference in
that principle ("a renumbered id (Principle 4)" → Principle 6; renumbering is
Principle 6, as `08-serialization.md:108` cites correctly).

### Source seams

| seam | file:line | change |
| --- | --- | --- |
| kind version constants | `src/runtime/arbc/runtime/builtin_codecs.hpp:31-34` | add `k_nested_kind_version = "1"` |
| codec declarations | `src/runtime/arbc/runtime/builtin_codecs.hpp:38-64` | add `Codec nested_codec();` |
| binder declaration | `src/runtime/arbc/runtime/builtin_codecs.hpp:75` | keep `register_nested_binder()`; drop the "no codec TU to ride" comment |
| the codec TU | `src/runtime/codec_nested.cpp` (**new**) | `serialize_nested` / `deserialize_nested` / `nested_codec()` / `register_nested_binder()` |
| the binder TU | `src/runtime/binder_nested.cpp` (**delete**) | absorbed, per `nested_runtime_binding.md` D5 |
| build | `src/runtime/CMakeLists.txt:3-4` | `binder_nested.cpp` → `codec_nested.cpp` |
| kind reverse-map | `src/runtime/document_serialize.cpp:81-103` | `builtin_kind_of()` gains a `NestedContent` arm |
| kind interning | `src/runtime/document_serialize.cpp:109-116` | `KindBridge` pre-interns `NestedContent::kind_id` |
| save codec table | `src/runtime/document_serialize.cpp:142-149` | `table.add(NestedContent::kind_id, nested_codec());` |
| load codec table | `src/runtime/document_serialize.cpp:346-366` | same, wrapped in `recording_deserialize` |
| **writer traversal** | `src/serialize/writer.cpp:253-280` (`ContentGraph::visit`) | do not descend `inputs()` when `enqueue_composition(c->composition_ref())` returned true |
| **writer emission** | `src/serialize/writer.cpp:302-317` (`emit_definition`) | do not emit `body["inputs"]` when the body carries `"composition"` |
| snapshot walk | `src/runtime/document_serialize.cpp:184-267` (`capture_snapshot`) | mirror the skip, so the writer-thread reverse map matches the traversal it feeds |
| reader validation | `src/serialize/reader.cpp:488-510` (`RefResolver::build`) | a body with both `inputs` and `composition` → `MalformedField` |

Reference points that need **no** change: `DeserializeFn`
(`src/serialize/arbc/serialize/deserialize.hpp:46-48`) already carries
`ObjectId composition` and its header comment already names nested as the kind
that consumes it; `SerializeFn` (`codec.hpp:40`) stays as-is (the writer appends
`composition` after the codec returns); `content_body_from_json`
(`src/serialize/codec.cpp:90-101`) already strips both `inputs` and `composition`
from a placeholder's stored body; `PlaceholderContent`
(`placeholder_content.hpp:52-54,:77`) already carries a composition ref;
`scripts/check_levels.py:35-41` already whitelists `runtime → kind_nested,
serialize`, and `src/runtime/CMakeLists.txt` already lists `kind_nested` in
`DEPENDS` (both added by `kinds.nested_runtime_binding`) — **no new levelization
edge, no new dependency (doc 10)**.

### The kind

`src/kind_nested/arbc/kind_nested/nested_content.hpp` — `kind_id =
"org.arbc.nested"` (`:168`), the sole constructor `explicit NestedContent(ObjectId
child)` (`:64`), `composition_ref()` (`:153`), `child()` (`:166`), `inputs()`
(`:148`, memo-derived), `attach`/`detach` (`:86`,`:98`). **NestedContent has no
params at all** — the child `ObjectId` is its entire state, and it is core-owned.

### Tests / claims

- `tests/document_compositions_golden.t.cpp` — the L5 model to follow; its
  `NestKind` double (`:66-101`) is precisely what a real `NestedContent` replaces.
- `tests/operator_codecs_golden.t.cpp` — the byte-exact golden template
  (inline `R"json(...)"`, `save` → compare → `load` → `save` → compare).
- `tests/nested_runtime_binding.t.cpp` — how a test builds a *bound* nested
  document (`bind_operators` + a real pin); the attach-invariance golden needs it.
- `tests/claims/registry.tsv:235-238` — the four compositions-table claims we
  extend; `:159` `05-recursive-composition#nested-runtime-bound`.
- `tests/CMakeLists.txt:164-168` — a test naming a `CodecTable` must link
  `nlohmann_json::nlohmann_json` explicitly.

## Constraints / requirements

1. **The codec owns only `params`.** `kind`, `kind_version`, `inputs`, and
   `composition` are core-owned and re-derived from graph structure on every save.
   `serialize_nested` must **not** write the child id — it returns
   `json::object()`. (`operator_codecs.md` D3; doc 08 P7.)
2. **`deserialize_nested` does not intern or resolve anything.** The reader has
   already allocated the child's `ObjectId` (via `CompResolver`, `reader.cpp:347-398`)
   before the codec runs; the codec receives it and builds around it:
   `std::make_unique<NestedContent>(composition)`.
3. **A nested body with no resolvable `composition` is a read error.** An absent
   or null `composition` on an `org.arbc.nested` body →
   `ReaderError::Kind::MissingRequiredField` at `/composition` (the
   `read_fail(kind, path)` idiom, `codec_fade.cpp:108`). A `composition` naming an
   id absent from the table is already `UnresolvableReference` in the reader, ahead
   of the codec (doc 08:184-186) — do not duplicate that check.
4. **`deserialize_nested` consumes no `inputs`.** Nested's inputs are derived, so
   the codec must not adopt them (Decision 1's corollary; a body carrying both is
   rejected by the reader before the codec sees it, Constraint 5).
5. **A body carrying both `composition` and `inputs` is `MalformedField`** at the
   body's path, per the doc 08 delta. Rejecting beats silently dropping one of two
   authored edge sets.
6. **Save output must not depend on binding state.** Saving a document is a pure
   function of the document; whether a render `OperatorBindingScope` happens to be
   live must not change one byte. This is the invariant Decision 1 restores and the
   attach-invariance golden pins.
7. **A Droste scene must round-trip.** Root embeds itself → `"composition": "0"`
   on the nested body, **no `compositions` key at all**, and no `$ref` anywhere.
   Load → save must be byte-identical, bound or unbound.
8. **Errors as values, model unmutated on failure.** No nlohmann exceptions escape;
   a failed load leaves the model at `revision() == 0` (doc 10; the discipline every
   sibling codec keeps).
9. **Programmatic construction creates the parent before the child** — the ordering
   `compositions_table.md` Constraint 8 flagged as "`runtime.nested_codec` does".
   The goldens must build documents that way.
10. **No new levelization edge, no new dependency.** `runtime → kind_nested,
    serialize` is already whitelisted; `nlohmann` stays PRIVATE on `arbc_runtime`.
    `scripts/check_levels.py` must stay silent.

## Acceptance criteria

- **`tests/nested_codec_golden.t.cpp`** (new, L5, cross-component) — byte-exact
  goldens through a live `Document` → `save_document` → inline `R"json(...)"`
  comparison → `load_document` → `save_document` → byte-identical:
  - a plain nesting document (root layer holds a real `NestedContent`; child
    composition with its own canvas + layers in `compositions["1"]`), pinning
    **`08-serialization#builtin-nested-codec-round-trips`** *(new claim)*.
  - a **Droste** self-cycle (root embeds itself): the nested body carries
    `"composition": "0"`, the document carries **no** `compositions` key and **no**
    `contents` key, and load → save is byte-identical. Re-enforces
    **`08-serialization#droste-cycle-round-trips-as-data`** (`registry.tsv:236`)
    — now through the *real* kind, which is what makes it a proof rather than a
    double's rehearsal.
  - a shared child: two nested contents naming the **same** child composition emit
    one `compositions["1"]` entry and two `"composition": "1"` references.
    Re-enforces **`08-serialization#child-compositions-round-trip-in-document`**
    (`registry.tsv:235`).
- **Attach invariance** (same TU) — the criterion that pins Decision 1: build a
  nesting document, save it, then take a real pin and run `bind_operators`
  (`tests/nested_runtime_binding.t.cpp` shows the setup) so `NestedContent::inputs()`
  is non-empty and `attached()` is true, save **again**, and assert the two byte
  streams are **identical**; assert the emitted body carries no `inputs` key and
  the document no `contents` key. Repeat for the Droste document, where the
  pre-fix writer instead produces an operator-input `$ref` cycle: assert the bound
  save **loads back** without error. Pins a new claim
  **`08-serialization#nesting-inputs-are-derived-not-persisted`** *(new claim)*.
- **`tests/serialize_compositions.t.cpp`** (extend, L4) — reader rejection:
  a content body carrying both `composition` and `inputs` → `MalformedField` at the
  body path, model unmutated (`revision() == 0`). Pins a new claim
  **`08-serialization#composition-and-inputs-is-read-error`** *(new claim)*.
- **`src/runtime/t/nested_codec.t.cpp`** (new, runtime component test) — codec
  units: `serialize_nested` on a non-nested content →
  `SerializeError::Kind::CodecFailed`; `serialize_nested` emits an empty `params`
  object and never a child id; `deserialize_nested` with a valid `composition`
  builds a `NestedContent` whose `child()` is that id; with `ObjectId{}` →
  `MissingRequiredField` at `/composition`. A `params` key the codec never
  consumed (including a hand-authored `ref`) round-trips verbatim via the residual
  diff — re-enforces **`08-serialization#known-kind-params-unknowns-preserved`**
  (`registry.tsv:234`).
  *(Note: this TU must not include `<arbc/kind_nested/...>` if that trips
  `check_levels.py` as it did for `nested_runtime_binding` (`:242-244`) — if so it
  lives in `tests/` beside the goldens, as that task's test does.)*
- **Placeholder path preserved** — extend the existing unknown-kind case: a
  document whose nesting content's kind has **no** registered codec still carries
  its child reference and re-emits it. Re-enforces
  **`08-serialization#unknown-kind-preserves-composition-reference`**
  (`registry.tsv:237`).
- **Fuzz seed** — `tests/fuzz/corpus/load_document/composition_and_inputs.arbc`
  (a body carrying both edge sets), beside the three
  `serialize.compositions_table` added.
- **Claims register** — three new rows in `tests/claims/registry.tsv`, each with an
  `// enforces: <claim-id>` tagged test:
  `08-serialization#builtin-nested-codec-round-trips`,
  `08-serialization#nesting-inputs-are-derived-not-persisted`,
  `08-serialization#composition-and-inputs-is-read-error`.
- **Concurrency** — extend `tests/document_serialize_concurrency.t.cpp`'s TSan lane
  with a save taken **while a binding scope is live on another thread** (the exact
  race Constraint 6 makes benign). No new lock is expected: `capture_snapshot`
  already pins on the writer thread, and after Decision 1 the writer no longer reads
  the memo `attach` mutates. The lane is what proves that.
- **Gates** — `scripts/check_levels.py` silent; ≥90% diff coverage on changed
  lines; `-Werror -Wpedantic` clean; the full suite green.
- **Deferred to `runtime.nested_external_ref`** (closer registers in WBS, **effort
  2d**, `depends runtime.nested_codec`, wired into **`m8_persistence`**): resolve an
  **external** nested child — a kind-owned `params.ref` URI — through
  `LoadContext`'s base-URI resolution and async `AssetSource` hook
  (`load_context.hpp:49-87`), loading the referenced `.arbc` as a child composition
  and deduping by resolved identity, so nested renders an external project.
  Doc 05:47-61, doc 08 Principle 3. **This is not new scope**: M8's own note
  already promises "external nested projects load"
  (`tasks/99-milestones.tji:65`), and no leaf delivers it —
  `serialize.reader` shipped the `LoadContext` seam but nothing consumes it for
  nested. This task keeps `params.ref` in loader territory per its own `note`, so
  the promise needs its own leaf.

## Decisions

1. **A nesting content's `inputs()` are a projection of its child composition and
   are never persisted; the writer neither descends nor emits them.** This is the
   task's one architectural call, and it carries the doc 08 delta.

   The problem is concrete. `NestedContent::inputs()` is memo-derived from the
   child composition's layers (`nested_content.cpp:126-140`) and is non-empty
   exactly when the content is **attached** — the state `bind_operators` leaves it
   in for every rendered frame. `ContentGraph::visit` descends `inputs()`
   unconditionally (`writer.cpp:267`) and `emit_definition` emits them
   (`:302-309`), so a save taken while a binding is live counts the child's layer
   contents **twice** — once as layers of the child composition, once as nested's
   inputs — pushing them over Principle 6's shared-two-or-more threshold and
   hoisting them into `contents` behind `$ref`s that describe no authored sharing.
   Same scene, different bytes, depending on whether a render was in flight. For a
   **Droste** document it is worse: the nested content becomes a transitive input
   of *itself*, and the writer emits a `$ref` closing an **operator-input cycle** —
   which doc 08 Principle 6 forbids and the reader's `RefResolver` rejects. A bound
   save of a Droste scene writes a file that will not load.

   *Rejected — descend `inputs()` for the reverse map but suppress the emitted
   array:* the descent is itself the harm. It is what increments `d_counts`
   (`writer.cpp:254-262`) and therefore what hoists the child's contents into
   `contents`. Suppressing only the array leaves the byte instability intact.
   Nothing is lost by skipping: those contents are reached anyway as the child
   composition's layers, through the BFS `enqueue_composition` already drives.

   *Rejected — define save as running only against an unbound document:* it makes
   a data-format invariant depend on wall-clock luck. Saves and frames genuinely
   overlap (`capture_snapshot` runs on the writer thread while renders run;
   `offline_sequence.cpp` holds a binding for the length of an export). "Don't save
   during a frame" is not an invariant anyone can hold.

   *Rejected — make `NestedContent::inputs()` always empty:* it would fix the
   writer by breaking the operator graph. `inputs()` is what the core folds for
   aggregate revision, damage routing, and the `PullService` identity map
   (doc 13; `nested_content.hpp:144-148`). The edges are real at render time; they
   are just not *authored*.

   *Rejected — a new `bool inputs_derived() const` discovery virtual on `Content`:*
   it would let a hypothetical kind carry authored inputs *and* a child composition.
   No such kind exists, none is designed, and the core could not tell the two edge
   sets apart inside one `inputs()` span even with the flag — so the flag buys
   nothing while widening the L3 contract (doc 03) for a phantom. Making the
   exclusivity a **rule on kinds** instead (Decision 2) is the smaller and more
   honest abstraction: it needs no new virtual, and it is exactly what doc 05
   already says when it calls the child nested's "single input".

2. **A kind names its child through `composition_ref()` or takes authored
   `inputs`, never both — and a body carrying both is a read error
   (`MalformedField`).** The corollary of Decision 1, made explicit so it binds
   future kinds (including out-of-tree plugins) rather than living as an
   accident of nested's implementation.

   *Rejected — accept and silently ignore the `inputs`:* doc 08 states the opposite
   preference twice in the principle we are amending ("rejecting it beats silently
   dropping a composition the author wrote", `:186-188`). A hand-authored body with
   both edge sets means the author believed something the format cannot express;
   saying so beats discarding half of it. The rule is stricter than the old reader,
   but the format is pre-1.0 and the writer has never emitted such a body.

3. **`deserialize_nested` is `make_unique<NestedContent>(composition)`; a missing
   `composition` is `MissingRequiredField` at `/composition`.** `NestedContent` has
   no params and no null-child mode worth persisting — a nested content with no
   child is a nested content with nothing to nest.

   *Rejected — tolerate a null child and build an empty nested:* it round-trips as
   a content that describes as the empty placeholder forever. That is a silent
   data-loss shape (whatever the author meant to nest is gone), and doc 08's
   dangling-reference rule already treats an unresolvable child as an error — an
   *absent* one is not more benign.

4. **Absorb `binder_nested.cpp` into the new `codec_nested.cpp`.** Fade and
   crossfade each keep their binder in their codec TU — the one runtime TU that
   already legally names the concrete kind (`codec_fade.cpp:128-145`). Nested got a
   standalone binder TU only because it had no codec TU yet;
   `kinds.nested_runtime_binding` Decision 5 explicitly anticipated this absorption
   and `builtin_codecs.hpp:75` carries a comment saying so. Restoring the symmetry
   costs nothing and removes a file whose reason to exist is now gone.

5. **`k_nested_kind_version = "1"`,** matching every other built-in
   (`builtin_codecs.hpp:31-34`). Advisory and golden-pinned, per
   `operator_codecs.md`.

6. **`serialize_nested` emits `json::object()` — an empty `params`, not an omitted
   one.** It matches what `content_body_to_json` frames for every kind, keeps the
   golden's shape uniform with the `NestKind` double the predecessor already pinned
   (`document_compositions_golden.t.cpp:88-101`), and — via the load-time residual
   diff (`codec.cpp:79-82`) — gives nested free preservation of any `params` key a
   hand-authored file carries, `ref` included. That is what keeps an external-ref
   document readable-and-rewritable today, ahead of `runtime.nested_external_ref`.

7. **No doc-00 decision bullet.** The delta is a mechanism inside the already-recorded
   2026-07-10 decision (`00-overview.md:111-112`), not a project-shaping choice —
   the same call `operator_codecs.md` D6 and `compositions_table.md` made.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-11.

- `src/runtime/codec_nested.cpp` (new) — `serialize_nested` / `deserialize_nested` / `nested_codec()` / `register_nested_binder()` (absorbed from deleted `src/runtime/binder_nested.cpp` per D4).
- Four registration sites wired: `k_nested_kind_version`, `builtin_kind_of()` (`document_serialize.cpp`), `KindBridge` pre-intern, and `builtin_codecs()` save/load codec tables.
- Derived-inputs fix (D1) shipped: `src/serialize/writer.cpp` traversal skip + emission skip; `document_serialize.cpp` snapshot-walk mirror; `src/serialize/reader.cpp` both-edges `MalformedField` rejection.
- `docs/design/08-serialization.md` delta — Principle 7 closing paragraph making the no-derived-inputs rule normative (same commit, per doc 16).
- `tests/nested_codec_golden.t.cpp` (new, 6 cases): plain nesting, Droste, shared child, two attach-invariance proofs, and a `FadeContent`-with-nested-input case covering `builtin_kind_of`'s `NestedContent` branch via operator-input-child position.
- `src/runtime/t/nested_codec.t.cpp` (new, 6 unit cases): codec halves, `CodecFailed`, `MissingRequiredField`, params-residual `ref` preservation.
- Reader-error test (2 sections) in `tests/serialize_compositions.t.cpp`; TSan bound-save lane in `tests/document_serialize_concurrency.t.cpp`; fuzz seed `tests/fuzz/corpus/load_document/composition_and_inputs.arbc`; 3 new claims rows.
