# Refinement — `model.content_removal`

## TaskJuggler entry

`tasks/10-model.tji:78-83`, task `content_removal` ("Per-content removal path"),
under `task model`. The `.tji` note (verbatim):

> "Surface removal of a single content at the DOCUMENT level
> (Document::remove_content) — today the only teardown is document-wide
> (~Document). RESCOPED 2026-07-12 (WBS audit): the prior note claimed the
> model-transaction semantics were 'unsettled and must be decided here' and that
> 'removal is not in the transaction/journal model today'. That is false of the
> CODE. Transaction::remove() already exists (model.cpp:1509): a journaled
> hamt_erase that path-copies, shares untouched siblings, touch()es the id into
> the entry, and emits damage; it is tested in src/model/t/transactions.t.cpp
> (erase, sibling survives, absent no-op, add-then-remove within one
> transaction). Undo/redo of a removal already falls out of the before/after
> owning-edge design — Model::navigate() rebinds an EMPTY target Ref as an erase
> and a non-empty one as a re-insert (model.hpp:310-323) — so a removal is
> undoable by construction and a pinned snapshot keeps its own DocRoot and still
> observes the removed content by structural sharing. Nothing needs deciding.
> What is ACTUALLY missing: (1) the Document-level wrapper composing the existing
> Transaction::remove() + Transaction::detach_layer() + EditableBinding::unbind()
> (already shaped for per-content teardown); (2) test coverage for undo-of-a-remove
> — src/model/t/journal.t.cpp has none; (3) the doc 14 delta, which IS real:
> doc 14 has ZERO occurrences of 'remove', 'delete' or 'erase', so the removal
> the code already implements is undocumented. Source: tasks/parking-lot.md
> 2026-07-10 (editable-content removal path). Doc 14."

Feeds milestone `m9_release` (`tasks/99-milestones.tji:72`).

## Effort estimate

**1d** (`tasks/10-model.tji:79`). The model and runtime primitives all exist and
are individually tested; the work is one runtime composition method, two tests
(one model-layer, one runtime-layer), and a doc-14 delta. No new model code, no
new journal entry type, no new sink. The estimate holds because the only genuine
design work — the eager-vs-retained binding-teardown question (Decision 1) —
resolves to "reuse what exists and defer the optimization", not new machinery.

## Inherited dependencies

**Settled:**

- `model.editable_facet` (`tasks/10-model.tji:51-56`) — froze the
  `Editable`-facet lifecycle: state handles retain on record-slot create and
  release on record-slot **reclaim**, routed through the writer-owned
  `StateRefSink` (`src/model/arbc/model/model.hpp:331-338`). It also froze the
  runtime `EditableBinding` as the per-document sink multiplexer that routes each
  release to its owning content's row, with an asserted-zero
  `unrouted_state_calls()` tripwire for a release that finds no row
  (`src/runtime/arbc/runtime/editable_binding.hpp:264-277`). Both facts are
  load-bearing for Decision 1. Refinement:
  `tasks/refinements/model/editable_facet.md`.
- `model.journal` (`tasks/10-model.tji:37-42`) — froze undo/redo as ordinary
  forward publishes via the writer-only `Model::navigate(entry, direction)`,
  which rebinds each `ObjectEdit`'s stored owning `Ref` by `SlotRef` identity
  (`hamt_insert` for a non-empty target, `hamt_erase` for empty), publishes at
  revision +1, replays the entry's damage once, and never notifies the
  `CommitSink` (`src/model/model.cpp:1701-1741`). This is exactly the seam a
  removal's undo rides; the removal's `ObjectEdit` carries a populated `before`
  and empty `after`, so undo re-inserts and redo re-erases. Refinement:
  `tasks/refinements/model/journal.md`.
- `model.content_binding` (`tasks/10-model.tji:58-63`) — froze that a
  `ContentRecord` is `{kind id, StateHandle}` with no vtable pointer
  (`src/model/arbc/model/records.hpp:60-63`) and that the id→live-`Content*`
  binding is served by the runtime side-map (`Document::d_contents`), not the
  versioned record. A `LayerRecord` names a content by `ObjectId` **value**, not
  an owning edge, so removing a content is orthogonal to the layers that name it;
  the wrapper must detach them explicitly. Refinement:
  `tasks/refinements/model/content_binding.md`.

**Settled sibling context (leaned on, not a formal `depends`):**

- `model.transactions` (`tasks/10-model.tji:23-28`) — landed
  `Transaction::remove(ObjectId)` (`src/model/model.cpp:1602-1618`): a journaled
  `hamt_erase` that path-copies, shares untouched siblings by `SlotRef`,
  `touch()`es the id, and emits `Damage{id, Rect::infinite(), TimeRange::all()}`;
  an absent id is a no-op. Also `Transaction::detach_layer(composition, layer)`
  (`src/model/model.cpp:1469-1504`), which removes a membership row and damages
  the composition once, no-op if the layer is not a member. Both are covered in
  `src/model/t/transactions.t.cpp`. Refinement:
  `tasks/refinements/model/transactions.md`.
- `model.composition_membership` — established the composition-scoped layer-order
  accessor and the attach/detach/reorder damage-once discipline this wrapper's
  detach step inherits. Refinement:
  `tasks/refinements/model/composition_membership.md`.

**Pending:** none. Every seam this task composes has shipped.

## What this task is

Three concrete deliverables, no more:

1. **`Document::remove_content(...)`** — the runtime-layer (L5) wrapper that
   composes the existing model and binding primitives into a single atomic,
   undoable, per-content deletion, mirroring `Document::add_content`
   (`src/runtime/document.cpp:83-116`) inverted. It opens one transaction,
   detaches the content's referencing layer(s) from their composition(s)
   (`detach_layer`), erases the content record (`remove`), and commits — one
   publish, one journal entry, one damage flush.
2. **Model-layer undo-of-remove test** — the coverage the `.tji` note calls out
   as missing: `src/model/t/journal.t.cpp` has no case that commits a `remove`
   and then undoes it. Add one proving `navigate` restores the erased record to
   its exact pre-removal `SlotRef` edge and redo re-erases it, with a
   before-the-removal pin still resolving the content throughout.
3. **Doc-14 delta** — document the removal/erase path (see
   [Design-doc delta](#design-doc-delta) under Decisions), since doc 14 has zero
   occurrences of `remove`/`delete`/`erase`.

It does **not** add a new journal entry type, a new model mutator, a new sink, or
a new damage shape — all of that already exists. It does **not** build the
"reclaim the removed content the moment its record leaves history" hook; that is
a named, deferred follow-up (Decision 1 / Acceptance criteria).

## Why it needs to be done

The document-level teardown surface today is all-or-nothing: `~Document`
(`src/runtime/document.cpp:48`) tears down the whole model, but there is no way
to delete a single content. The v0.1 forcing consumer is an image editor with
layers (`tasks/99-milestones.tji:73`), and "delete this layer" is a first-order
operation an editor cannot ship without. `remove_content` is the missing verb.
It is a direct dependency of `m9_release` (`tasks/99-milestones.tji:72`).

## Inputs / context

**Design docs (normative — the refinement realizes these, doc 16):**

- **doc 14 § "Transactions"** (`14-data-model-and-editing.md:102-146`) — all
  mutation is a writer-thread transaction; atomicity ("observers see v(n) or
  v(n+1), never a half-edit", :118-120); damage rides the transaction and undo
  replays it (:127-129); `detach_layer` is a placement/graph transaction that
  damages the parent composition once and undoes as an inverse publish over the
  existing `ObjectEdit` edges (:138-146). This is the class the removal joins.
- **doc 14 § "Content state: the `Editable` facet"**
  (`14-data-model-and-editing.md:148-297`) — the facet's retain/release handle
  lifetime ("release when its record is reclaimed") and the one-sink-trio-per-
  `Document` multiplexer where "releasing one drains the model and drops its row"
  and "a state call that finds no row is a defect, counted and asserted-zero".
  This is the invariant Decision 1 protects.
- **doc 14 § "History"** (`14-data-model-and-editing.md:298-319`) — undo/redo are
  ordinary publishes over the journal's `(before, after)` edges; history is never
  mutated.
- The doc-14 delta this task lands appends a removal paragraph at the tail of
  § Transactions (`14-data-model-and-editing.md:147a`, after the membership
  paragraph).

**Source seams (the wrapper composes these; none is modified):**

- `src/runtime/document.cpp:83-116` — `Document::add_content`, the analogue to
  invert: `txn = d_model->transact()`, `id = txn.add_content(kind)`,
  `d_binding.bind(id, live)` **inside** the txn, `capture()` →
  `txn.set_content_state(id, initial)`, `txn.commit()`, then
  `d_contents.emplace(id, std::move(content))`.
- `src/model/model.cpp:1602-1618` — `Transaction::remove(ObjectId)`.
- `src/model/model.cpp:1469-1504` — `Transaction::detach_layer(composition, layer)`.
- `src/model/model.cpp:1701-1741` — `Model::navigate(entry, direction)` (undo/redo).
- `src/runtime/arbc/runtime/editable_binding.hpp:246-262` — `bind` / `unbind` /
  `unbind_all`; the `unbind` doc-comment (:248-254) states the drain-first-then-
  drop-row contract and warns "Dropping the row first would strand the content's
  queued releases and leak its pool blocks."
- `src/runtime/arbc/runtime/editable_binding.hpp:264-277` — `bound(id)`,
  `bound_count()`, `unrouted_state_calls()` (the asserted-zero tripwire read by
  tests).
- `src/runtime/arbc/runtime/document.hpp:335-373` — `Document`'s member
  declaration/teardown-order contract (`d_contents`, `d_binding`, `d_model`,
  `d_journal`); `~Document` deliberately does **not** call `unbind_all` — the
  declaration order keeps the binding alive through `~Model`'s final drain.
- `src/runtime/document.cpp:118-120` — `Document::transact(name)`, the public
  transaction seam the wrapper opens.
- `src/model/t/transactions.t.cpp:138,172,346` — existing model-primitive remove
  coverage (drops + shares siblings; absent no-op; add-then-remove in one txn).
- `src/model/t/journal.t.cpp` — existing undo/redo cases (invert an **add**);
  confirmed to have **no** undo-of-a-committed-remove case.
- `src/runtime/t/content_binding.t.cpp`, `src/runtime/t/fake_editable.hpp` — the
  runtime-layer test home and the `Editable` test double the new runtime test
  reuses.

**Predecessor decisions carried forward.** The retain/release-rides-the-record-
slot lifecycle (`editable_facet.md` Decision 4), undo/redo-as-forward-publish
(`journal.md`), the value-named content edge (`content_binding.md`), and the
damage-once-per-commit discipline (`transactions.md`) are all consumed
unchanged. This refinement follows the predecessor conventions: claim ids as
`<doc-stem>#<slug>`, behavioral-counter assertions with **no wall-clock
assertions**, and the parked-TSan convention (below).

## Constraints / requirements

1. **Levelization (doc 17, CI-enforced by `scripts/check_levels.py`).** The core
   mechanism — record erase, layer detach, journaling, damage — is `arbc::model`
   (L2), which may reach only `base`/`pool`/`media` and must not touch the
   `Content` vtable. Those primitives already exist and are not modified.
   `remove_content` lives on `Document` in `arbc::runtime` (L5), which may drive
   `model` and the `EditableBinding` freely. Any editable teardown (dropping a
   routing row, releasing a handle) is driven from `runtime` through the model's
   owner-tagged `StateRefSink`, never by `model` reaching up
   (`docs/design/17-internal-components.md:63,71,90-94`).
2. **Atomicity.** The detach(es) + remove are one transaction and one commit — an
   observer sees the document with the content present or fully gone, never a
   layer pointing at an erased content.
3. **One publish, one journal entry, one damage flush** (doc 14 § Transactions).
   No new entry type; the removal is ordinary `ObjectEdit`s with empty `after`.
4. **Undoability is preserved, not re-implemented.** The wrapper must not trim
   history, bypass the journal, or eagerly release the content's `StateHandle` —
   undo restores the record via `navigate`, and a pin taken before the removal
   keeps resolving the content by structural sharing.
5. **The asserted-zero `unrouted_state_calls()` invariant holds across the whole
   remove→undo→redo cycle** — the binding must never be handed a release for a
   content whose row it has dropped (Decision 1).
6. **Teardown-order contract unchanged.** `remove_content` must not clear sink
   slots or violate `Document`'s member declaration order; document-close teardown
   remains the declaration-order drain (`document.hpp:335-373`).
7. **Writer-thread only**, consistent with every other `Document` mutator and the
   `Transaction`/`navigate`/`EditableBinding` contracts.

## Acceptance criteria

**Claims-register growth** (`tests/claims/registry.tsv`, each with an
`// enforces: <id>` test, gated both directions by `scripts/check_claims.py`):

- **`14-data-model-and-editing#remove-is-undoable`** — A committed content
  removal (`Transaction::remove` of a content id, coalesced with the layer
  `detach_layer` in one commit) is undone by `Model::navigate`: undo restores
  the erased `ContentRecord` to the **same `SlotRef` identity** its owning edge
  had before the removal (structural identity, not just equal bytes) and re-links
  every untouched sibling by identity; redo re-erases it; a version pinned before
  the removal resolves the content unchanged across the whole cycle. A full
  undo→redo round-trips `live_slots()` after `drain()` with no leak. Realizes
  doc 14:298-319 + the delta. Witnessed by `SlotRef` identity comparison and
  `live_slots()` before/after `drain()` in `src/model/t/journal.t.cpp`.
- **`14-data-model-and-editing#remove-content-retains-binding-until-history-drops-it`**
  — `Document::remove_content` deletes an editable content and its referencing
  layer(s) in one transaction (revision +1, one damage flush), and keeps
  `EditableBinding::unrouted_state_calls() == 0` across a
  remove→undo→redo→commit-new-edit sequence because the content's binding row is
  **retained** while the journal holds the removal's `before` edge; an undo
  re-resolves the restored content and its captured state is intact (a
  `set_content_state` reachable through the restored record). Realizes doc 14 §
  "Content state" + the delta. Witnessed by `unrouted_state_calls()`,
  `bound_count()`, and content-state resolution in a new
  `src/runtime/t/content_removal.t.cpp` (reusing `fake_editable.hpp`).

**Behavioral-counter tests** (doc 16 tier 4, Catch2): assert on `Model::revision()`
deltas (exactly +1 per commit / per undo / per redo), the damage-sink call count
(exactly one flush per commit), `EditableBinding::unrouted_state_calls()`
(exactly zero), and `live_slots()` before/after `drain()` (no leak after a
full undo→redo round-trip). **No wall-clock assertions.**

**Model-primitive coverage is not re-litigated.** `remove`'s path-copy,
sibling-sharing, and absent-no-op behavior are already enforced in
`src/model/t/transactions.t.cpp`; this task adds only the undo-of-remove case
that was missing.

**Conformance suite:** not applicable — `remove_content` is a document-level
mutator, not a `Content` kind or operator, so it exercises no per-`Content`
algebraic property (`contract.conformance_suite` is for kinds; precedent:
`editable_facet.md`, `content_binding.md`).

**Concurrency:** covered by the existing pin/traverse-vs-writer smoke in
`src/model/t/transactions.t.cpp:346` (which already exercises add-then-remove
under a concurrent reader), run in the **asan lane**. No new TSan preset in-tree
yet — the parked convention from `tasks/refinements/pool/reclamation.md`; full
seeded schedule-perturbation stress remains `quality.stress_harness`
(`tasks/70-quality.tji`), not duplicated here. `remove_content` mutates
`d_contents` / the binding row only on the writer thread, so it introduces no new
cross-thread shape.

**Coverage:** ≥90% diff coverage on the changed lines (CI + `scripts/gate`); the
two tests are part of the task, not a follow-up.

**Deferred follow-up (closer registers in WBS, milestone `m10_deferred`):**
`runtime.removed_content_reclaim` (~2d) — "Reclaim a removed content's retained
runtime `Content*` and its `EditableBinding` row the moment its record leaves
history (the removal entry is trimmed and no pin holds it), instead of at
document close. Hook the routed final `StateRefSink::release` of a content whose
id is absent from `Model::current()` to complete the runtime teardown (drop the
row, erase `d_contents`), on the writer thread, TSan-clean; witnessed by
`bound_count()` falling after a journal trim." This is a bounded memory-hygiene
optimization, not a correctness gap (Decision 1), so it belongs on the
deferred-past-0.1 milestone (`tasks/99-milestones.tji:78`) beside the other
memory-hygiene deferrals, not in `m9_release`.

## Decisions

1. **`remove_content` retains the runtime binding until the removal leaves
   history — it does NOT eagerly `unbind`/erase the side-map.** The `.tji` note's
   compose-list names `EditableBinding::unbind()` as a piece, but calling it
   *synchronously at removal time* is unsound. A content record's `StateHandle`
   releases only when the record slot is reclaimed, and the release is **routed
   to the content's binding row** (`editable_facet.md` Decision 4;
   `editable_binding.hpp:248-254,269-271`). After the removal commits, the record
   is not reclaimed — the journal's `before` edge holds it so undo can restore it.
   If `remove_content` dropped the row now, the record's later reclaim (on journal
   trim) would route its release to a missing row, tripping the asserted-zero
   `unrouted_state_calls()` invariant and leaking the handle's pool blocks — the
   exact hazard `unbind`'s own doc-comment warns against. So `remove_content`
   commits the structural removal and **keeps the live `Content*` + row alive**;
   they are torn down by the existing document-close path (declaration-order drain
   / `unbind_all`), and, once the follow-up lands, at history-trim.
   _Rationale:_ this is the only option that preserves the doc-14 undoability
   promise (undo re-resolves a *live, editable* content) while keeping the
   binding invariant true, and it adds zero new machinery.
   _Rejected — eager `unbind` + `d_contents.erase` at removal time:_ trips
   `unrouted_state_calls()` on the eventual reclaim and strands the release; and
   an undo would restore the record with no live `Content*` behind it, so the
   "removal is undoable" promise would restore an unusable ghost.
   _Rejected — non-journaled hard delete (trim the removal from history):_
   contradicts doc 14 § History ("history is never mutated") and breaks undo of
   any edit interleaved after the removal.
   The retained-content memory cost is bounded (journal depth / document life) and
   is reclaimed precisely by the named `runtime.removed_content_reclaim` follow-up.

2. **`remove_content` takes the content id together with its referencing
   `(composition, layer)` — the caller identifies the layer being deleted.**
   Because a `LayerRecord` names its content by `ObjectId` **value**, not an
   owning edge (`content_binding.md`), the content and its layers are distinct
   objects; the wrapper detaches the named layer(s) and erases both the layer
   record and the content record in one transaction. _Rationale:_ the v0.1 editor
   deletes a *specific layer* the UI already has in hand, so it supplies the ids;
   this keeps the wrapper O(1) and matches the `add_content` + `attach_layer`
   asymmetry it inverts. _Rejected — `remove_content(content_id)` that scans every
   composition for referencing layers:_ O(document) per delete and needs a policy
   for content shared by several layers (delete-when-last-layer-goes), neither of
   which the 1d scope or the forcing 1:1-layer-per-content editor needs; a shared-
   content deletion policy, if ever wanted, is a separate decision, not smuggled
   in here.

3. **The undo test lands at the model layer (`journal.t.cpp`), the retention test
   at the runtime layer (`content_removal.t.cpp`).** The record-restoration
   property is pure model (no binding involved) and belongs beside the other
   `navigate` cases; the `unrouted_state_calls()==0` retention property needs a
   real `Document` + `EditableBinding` and belongs in runtime. _Rationale:_ tests
   sit at the lowest layer that can express the claim, matching predecessor
   placement (`journal.md`, `editable_facet.md`).

**Design-doc delta.** Required and landed:
`docs/design/14-data-model-and-editing.md` gains a removal paragraph at the tail
of § Transactions (after the membership paragraph, before § Content state),
documenting the erase/undo/damage semantics and the retain-until-history-drops-it
binding rule, and naming the two new claim anchors. This delta is required
because doc 14 had zero occurrences of `remove`/`delete`/`erase` — the path the
code already implements was undocumented. **No doc-00 decision-record bullet:**
this is a localized editing-path clarification, not an engine-shaping decision;
precedent is `transactions.md` (added a doc-14 delta for the genuinely-new Abort
behavior with no doc-00 bullet), versus `per_object_revision.md` (which took a
doc-00 bullet only because it changed engine-wide cache-key semantics).

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-15.

- `src/runtime/arbc/runtime/document.hpp` — declared `Document::remove_content(content, composition, layer)`.
- `src/runtime/document.cpp` — implemented it: one transaction (`detach_layer` + `remove(layer)` + `remove(content)` + commit); no eager `unbind`/`d_contents.erase` per Decision 1.
- `src/runtime/CMakeLists.txt` — registered `t/content_removal.t.cpp` in the build.
- `src/model/t/journal.t.cpp` — added model-layer undo-of-remove test: `SlotRef`-identity restore, sibling identity, membership restore, `live_slots()` round-trip (enforces `14-data-model-and-editing#remove-is-undoable`).
- `src/runtime/t/content_removal.t.cpp` — new runtime retention test: rev +1, one damage flush, `bound_count`/`bound`/`resolve` retention, `unrouted_state_calls()==0` across remove→undo→redo→new-edit, restored `content_state==v1` (enforces `14-data-model-and-editing#remove-content-retains-binding-until-history-drops-it`).
- `tests/claims/registry.tsv` — added two claim rows for the above tests.
- `docs/design/14-data-model-and-editing.md` — doc-14 delta: removal paragraph appended at tail of § Transactions naming both new claim anchors and the retain-until-history-drops-it binding rule.
- Deferred follow-up registered in WBS as `runtime.removed_content_reclaim` (~2d, milestone `m10_post_01`): reclaim the retained `Content*` + binding row when the removal leaves history.
