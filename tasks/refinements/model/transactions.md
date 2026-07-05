# model.transactions — Transactions + coalescing

## TaskJuggler entry

`task transactions "Transactions + coalescing"` in
[`tasks/10-model.tji`](../../10-model.tji) (lines 22-27), under
`task model "Versioned model"`. Note line: _"Atomic multi-mutation commits;
gesture coalescing keys; damage rides the transaction and flushes once per
commit. Doc 14."_

## Effort estimate

**2d** (`effort 2d`, `tasks/10-model.tji:23`). This task extends the
`Model::Transaction` scaffold that `model.persistent_state` already landed
rather than building the publish primitive from scratch, so the estimate is
about the commit *mechanism* (coalescing, damage flush, journal-entry
assembly, abort, removal), not the atomic swap.

## Inherited dependencies

- **`model.persistent_state`** — **settled** (landed `faad68d`). Provides the
  whole substrate this task drives: `class DocRoot` (immutable pinnable
  version, `src/model/arbc/model/model.hpp:32`), `using DocStatePtr =
  std::shared_ptr<const DocRoot>` (model.hpp:70), `class Model` with the
  atomic publish cell `std::atomic<DocStatePtr> d_current` (model.hpp:157)
  and `Model::current()` (model.hpp:87 / `model.cpp:279`), the
  `Model::Transaction` builder (model.hpp:102-136) with `transact()`
  (model.hpp:139) forking off `DocRoot::root_ref()` (model.hpp:62), the
  path-copying HAMT `hamt_insert`/`hamt_lookup` (`hamt.hpp:133-139`), the
  slab record types + inert `StateHandle` (`records.hpp:25-113`), and
  writer-only reclamation `Model::drain()` (model.hpp:95). The predecessor
  explicitly scoped the "build next version + swap root" primitive to itself
  and left "the commit *mechanism* (build/publish/journal/flush)" to this
  task (see `tasks/refinements/model/persistent_state.md`, Constraints and
  Why sections).
- **`pool.refs` / `pool` arenas** — settled transitively via
  `model.persistent_state`; this task allocates only through the model's
  existing `RefStore<HamtNode>` / `RefStore<ObjectRecord>` views over
  `Arena` (`refs.hpp:246,276`; `slot_store.hpp:337,350`).

No **pending** inherited dependencies.

## What this task is

Turn the minimal `Model::Transaction` scaffold into the full doc-14
transaction: a **named**, writer-thread mutation batch that (1) commits as a
single atomic publish of the next version (revision `+1`), (2) can be
**aborted** as a pure discard, (3) carries a **coalescing key** so
consecutive interactive commits merge into one undo unit, (4) accumulates
**damage** across its mutations and flushes the union exactly once at commit,
and (5) assembles the **journal entry** (name, coalescing key, per-object
before/after record edges, per-content before/after `StateHandle` pairs,
damage set) and hands it to a commit sink. It also adds the two mutation
primitives the scaffold left as TODOs — content-state assignment
(`StateHandle` into a content record) and object **removal** (a `hamt_erase`
path-copy primitive). It provides the `CommitSink` and `DamageSink` seams the
downstream `model.journal` and `model.damage` tasks plug into, so both can be
implemented without guessing the surface.

## Why it needs to be done

The scaffold publishes atomically but exposes no name, no abort, no
coalescing, no damage, and no journal-entry assembly — it is a mutation
builder, not the editing contract doc 14 promises. Three siblings are blocked
on this surface being nailed down:

- **`model.journal`** (`tasks/10-model.tji:28`, `depends !transactions`) —
  stores the `JournalEntry` values this task assembles, budgets them by
  bytes, and implements undo/redo as ordinary forward publishes. It needs the
  entry shape and the `CommitSink` interface frozen.
- **`model.damage`** (`tasks/10-model.tji:34`, `depends !transactions`) —
  builds auto-damage-on-placement-change and up-nesting propagation on top of
  the per-transaction damage accumulator + `DamageSink` this task introduces.
- **`model.editable_facet`** (`depends !journal`, transitively this task) —
  the capture-once-per-gesture discipline feeds `StateHandle` values into
  `Transaction::set_content_state`; the (before, after) pairs it produces are
  what the journal restores on undo.

## Inputs / context

Design docs (normative — doc 16's executable-spec discipline):

- `docs/design/14-data-model-and-editing.md`
  - § *Transactions* (lines 69-96) — the `doc.transact("Trim clip")` /
    typed-mutator / `commit()` API; the four bullets **Atomicity** (v(n) or
    v(n+1), never a half-edit; a multi-mutation edit is one publish),
    **Coalescing** (`txn.coalesce(gesture_id)` merges consecutive journal
    entries; undo at gesture granularity, display updates per-commit),
    **Damage rides the transaction** (commit flushes the union once; undo/redo
    replays the entry's damage without diffing), and **Abort** (the delta this
    refinement lands — a dropped/aborted txn publishes nothing).
  - § *Content state: the `Editable` facet* (lines 98-157) — the transaction
    stores `(before, after)` state-handle pairs in the journal entry; the
    published `DocState` holds the *after* handle (lines 128-131); kind edit
    APIs follow "capture-once-per-entry, mutate, damage" (lines 120-122).
    `Editable` itself lives above the model (see levelization below).
  - § *History* (lines 159-178) — the entry fields this task assembles:
    "name, coalescing key, per-object placement deltas, per-content (before,
    after) state handle pairs, damage set" (line 164). Undo/redo are
    "ordinary publishes" (lines 165-167). v1 contract is "handle pairs with
    structural sharing … impossible to implement incorrectly in ways that
    corrupt history" (lines 175-178).
  - § *Cross-doc impact* (lines 223-235) — single-writer unchanged; contract
    tests enforce capture/restore round-trips and damage honesty (lines
    234-235).
- `docs/design/15-memory-model.md`
  - § *Version reclamation…* (lines 112-154) — a version is memory-live while
    pinned as a `DocState` root **or referenced by a journal entry's state
    handles** (lines 119-123); "**the writer thread is the only structural
    allocator**" (lines 137-143); cascades are deferred, never inline (lines
    130-136).
  - § *What this asks of doc 14 and the kinds* (lines 223-245) — `StateHandle`
    is a slab reference; `capture()`'s "O(small)" is realized as "copy the
    touched path into same-arena slots" (lines 237-238).
- `docs/design/17-internal-components.md`
  - Component table (line 52): `arbc::model` is **L2**, owning "object
    records, persistent `DocState`, **transactions**, journal/undo, damage,
    revisions, pins", depending on **`base`, `pool` only**.
  - Levelization rule (lines 41-44) — "A component may depend only on strictly
    lower levels"; CI validates the CMake + include graph
    (`scripts/check_levels.py`).
  - `contract` sits **above** `model` (line 53, and the rationale at lines
    66-72): `Editable` and the `Content` vtable are in `arbc::contract` (L3);
    "The model stays free of the `Content` vtable … preserving 'pure data plus
    change notification'."
- `docs/design/16-sdlc-and-quality.md`
  - § *Test taxonomy* — tier 4 behavioral counters (lines 54-62, "Most
    claims-register entries about efficiency land here"); tier 6 concurrency
    (lines 66-73, TSan on the full suite + stress on the publish/pin
    protocol).
  - § *CI structure* — diff-coverage hard gate ≥90% on changed lines (lines
    112-118).
  - Claims mechanics live in `tests/claims/registry.tsv` (header lines 1-3:
    two tab-separated columns `<claim-id>\t<description>`; a claim id is
    `<doc-file-stem>#<slug>`) and `scripts/check_claims.py` (the `enforces:
    <claim-id>` tag scan, lines 15-42).

Source seams this task extends (all at `faad68d`):

- `src/model/arbc/model/model.hpp:102-136` — `Model::Transaction` scaffold:
  `add_layer` (:110), `add_content` (:113), `add_composition` (:117),
  `set_transform` (:121), `commit() -> expected<std::monostate, PoolError>`
  (:127), fields `d_root` (working root, :134), `d_base_revision` (:135),
  sticky `d_status` (:136). `Model::transact()` (:139), `Model::drain()`
  (:95), `Model::live_slots()` (:100).
- `src/model/model.cpp:294-296` — `Transaction` ctor forks `d_root =
  current->root_ref()`; `:407-415` — `commit()` builds a new `DocRoot` and
  does `d_current.store(...)` (the atomic publish); `:315-321,398-404` — each
  mutator path-copies via `hamt_insert`.
- `src/model/arbc/model/hamt.hpp:39-41` (`StoreBundle`), `:88-103`
  (`HamtNode`, standard-layout, `~HamtNode` releases child edges),
  `:133-139` (`hamt_insert` / `hamt_lookup` — **no `erase` yet**; this task
  adds `hamt_erase`).
- `src/model/arbc/model/records.hpp:25` (`RecordKind`), `:38`
  (`StateHandle { SlotIndex slot }`, `k_state_none` at :29), `:47`
  (`ContentRecord { kind, StateHandle state }`), `:55` (`LayerRecord`),
  `:72` (`CompositionRecord`, inline `layers[k_max_inline_layers=8]`), `:85`
  (`ObjectRecord`), `:99-113` (standard-layout / trivially-destructible
  `static_assert`s).
- `src/pool/arbc/pool/refs.hpp:64` (`SlotRef`, 4-byte index-only, `==`/`!=`),
  `:100` (`Ref`, owning), `:276` (`RefStore::create`), `:313`
  (`peek`, zero-traffic), `:322`/`:337` (`retain`/`release`).
- `src/model/CMakeLists.txt` — `arbc_add_component(NAME model … DEPENDS base
  pool)`; unit tests via `arbc_component_test(COMPONENT model SOURCES
  t/…)`.
- Existing tests for style: `src/model/t/persistent_state.t.cpp` (Catch2;
  `live_slots()` deltas, `SlotRef`-identity assertions, a pin/traverse/commit
  concurrency smoke at :171), `src/model/t/model.t.cpp` (atomic-publish /
  monotonic-revision at :28).

Predecessor-refinement conventions: `tasks/refinements/model/persistent_state.md`,
`tasks/refinements/pool/*.md` (asan-lane concurrency convention, behavioral
counters over wall-clock).

## Constraints / requirements

- **Levelization (doc 17, CI-gated).** `arbc::model` stays at L2, depending
  only on `base` and `pool`. `Editable`, `Content`, and damage *sinks* live
  in `arbc::contract` (L3, above model). Therefore:
  - The transaction **never** calls `Editable::capture()`. Content state
    enters through `Transaction::set_content_state(ObjectId, StateHandle)`
    taking an **opaque** `StateHandle` (a model-defined slab handle,
    `records.hpp:38`); the caller at contract/runtime level captures and
    passes it in. The transaction reads the *before* handle off the existing
    `ContentRecord` and records the `(before, after)` pair.
  - The `CommitSink` and `DamageSink` are **abstract interfaces defined in
    `arbc::model`** (pure data + change-notification, doc 02); the concrete
    consumers (journal, damage propagation, viewport sinks) are wired from
    above. `scripts/check_levels.py` must stay green.
- **Single-writer (doc 14 §Transactions:71, doc 15:137-143).** All mutation
  and all structural allocation is on the writer thread. No conflict
  detection, no optimistic retry — the single-writer rule makes conflicts
  moot; this must not be reintroduced. Readers continue to pin
  `DocStatePtr` and traverse via `peek` with no lock and no refcount traffic.
- **Atomic multi-mutation commit (doc 14:83-85).** A commit with N mutations
  is exactly **one** `d_current.store` and exactly **one** revision
  increment; mid-transaction `current()`/`revision()` are unchanged.
  Observers see v(n) or v(n+1), never a partial edit.
- **Abort is a discard, not a rollback (doc 14 delta, §Transactions).** A
  transaction dropped without `commit()`, or via `abort()`, publishes
  nothing: `current()` and its revision are unchanged, the working records it
  built are released (writer-side, via the existing reclamation queue), and
  no journal entry / damage is emitted. This is cheap precisely because
  nothing is observable until the commit store.
- **Damage flushes exactly once per commit (doc 14:92-94).** Per-mutation
  damage accumulates into a transaction-owned set (union/dedup); `commit()`
  flushes the union to the `DamageSink` in a single call; `abort()` flushes
  nothing. This task introduces the minimal `Damage` value `{ObjectId, Rect,
  time-range}` (all `arbc::base` types) plus the accumulator + sink;
  `model.damage` builds auto-damage and up-nesting propagation on top of this
  value (boundary recorded under Decisions).
- **Journal-entry assembly (doc 14:164, structural sharing).** `commit()`
  assembles one `JournalEntry` — name, coalescing key, per-touched-object
  `(before, after)` `ObjectRecord` edges (owning `Ref`s so the records stay
  memory-live for undo, doc 15:119-123), per-content `(before, after)`
  `StateHandle` pairs, and the damage set — and notifies the `CommitSink`
  once. Edges are shared by `SlotRef`/`Ref` identity, never deep-copied
  (doc 14:175-178). The entry must carry enough for a *pure inverse publish*
  so `model.journal` can implement undo without re-reading the live objects.
- **Coalescing (doc 14:86-91).** `Transaction::coalesce(CoalesceKey)` stamps
  a non-zero gesture key onto the entry (`CoalesceKey = std::uint64_t`, `0` =
  no coalescing). Each coalesced commit still publishes (display updates
  per-commit); consecutive entries sharing a non-zero key merge into one undo
  unit. The **merge semantics** (first entry's *before* + last entry's
  *after*, unioned damage, unioned object set) are a pure
  `JournalEntry`-level operation defined here; the *history threading* (which
  entries are "consecutive") is applied by `model.journal` via this helper
  (boundary under Decisions).
- **Removal + content-state primitives.** Add `hamt_erase(StoreBundle&,
  const Ref<HamtNode>&, key) -> expected<Ref<HamtNode>, PoolError>` (a
  path-copy that shares untouched siblings and collapses emptied branches),
  and `Transaction::remove(ObjectId)` + `Transaction::set_content_state`.
  Erase enables `model.journal`'s inverse of an add. Both are writer-only and
  must preserve the structural-sharing invariant
  (`14-…#commit-shares-untouched-structure`).
- **Record-shape invariants unchanged.** No new record type may break the
  `records.hpp:99-113` `static_assert`s (standard-layout, pointer-free,
  trivially-destructible object records; in-record edges index-only
  `SlotRef`). `JournalEntry`, `Damage`, and the sink interfaces are
  *transient* transaction/handle types (they may hold `Ref`/STL), never
  in-arena records.

## Acceptance criteria

- **Unit tests** (`src/model/t/transactions.t.cpp`, wired via
  `arbc_component_test(COMPONENT model …)` in `src/model/CMakeLists.txt`;
  Catch2, matching `persistent_state.t.cpp` style):
  - named `transact("…")` round-trips the name into the emitted entry;
  - `set_content_state` assigns a `StateHandle` and records the prior handle
    as *before*; `remove` + `hamt_erase` drops an object and shares untouched
    siblings by `SlotRef` identity; erase of an absent id is a no-op;
  - abort-by-drop and explicit `abort()` both leave `current()`/`revision()`
    unchanged and, after one `drain()`, return `live_slots()` to its
    pre-transaction value.
- **Behavioral-counter assertions (doc 16:54-62 — counters, never
  wall-clock).** Assert on a test-double `CommitSink`/`DamageSink` call
  count, on `Model::revision()` deltas, and on `Model::live_slots()`: a
  multi-mutation commit increments revision by exactly 1 and calls each sink
  exactly once; an aborted transaction increments neither and calls neither.
- **Claims (register in `tests/claims/registry.tsv`; enforce with an
  `// enforces: <claim-id>` tagged test — `scripts/check_claims.py` gates
  both directions):**
  - `14-data-model-and-editing#commit-publishes-once` — a transaction with
    multiple mutations produces exactly one published version (revision `+1`);
    `current()` is unchanged until commit (behavioral-counter test).
  - `14-data-model-and-editing#abort-publishes-nothing` — a dropped/aborted
    transaction leaves `current()`/`revision()` unchanged, emits no entry or
    damage, and reclaims its working records after `drain()` (enforced by the
    doc 14 §Transactions **Abort** delta this task lands).
  - `14-data-model-and-editing#damage-flushes-once-per-commit` — `commit()`
    flushes the union of per-mutation damage to the `DamageSink` in exactly
    one call; `abort()` flushes none.
  - `14-data-model-and-editing#commit-appends-one-journal-entry` — a
    non-coalesced commit notifies the `CommitSink` exactly once with the
    assembled entry (name, before/after edges + `StateHandle` pairs, damage).
  - `14-data-model-and-editing#coalesced-commits-merge-to-one-entry` — N
    consecutive commits sharing a non-zero coalescing key yield exactly one
    merged journal entry (first-before / last-after, unioned damage), while
    each commit still publishes a new revision.
- **Concurrency (doc 16 tier 6).** Extend the existing pin/traverse/commit
  smoke (`persistent_state.t.cpp:171`) so the committing writer also emits
  damage + entries through no-op sinks and exercises `remove`; assert no torn
  read and no use-after-free under the **asan lane** (no TSan preset in-tree
  yet — the parked convention from `tasks/refinements/pool/reclamation.md`;
  full seeded schedule-perturbation stress remains `quality.stress_harness`,
  `tasks/70-quality.tji`, not duplicated here).
- **Coverage / gate.** ≥90% diff coverage on changed lines; `scripts/gate`
  green including asan, `scripts/check_levels.py`, `scripts/check_claims.py`.
- **Deferred follow-up (closer registers in WBS).**
  `model.composition_membership` (~1.5d, `allocate team`, `depends
  model.transactions`, milestone: the versioned-model milestone that carries
  the other `model.*` leaves) — populate `CompositionRecord.layers[]` /
  `layer_count` (`records.hpp:72`, currently unwritten by the commit path):
  transaction mutators to add/remove/reorder a composition's layer
  membership, including spill beyond the inline `k_max_inline_layers = 8` cap
  (indirection to a HAMT-backed layer list). Out of scope here because none of
  transactions' immediate consumers (journal, damage, editable_facet) need
  composition membership, and the inline-cap spill is a self-contained design.

## Decisions

- **`StateHandle` crosses the transaction boundary opaquely; the transaction
  never touches `Editable`.** `set_content_state(ObjectId, StateHandle)`
  reads the current handle off the `ContentRecord` as *before* and writes the
  caller-supplied handle as *after*. This is forced by the doc-17 levelization
  (`Editable` is L3 `contract`, model is L2, line 52-53, 66-72) and keeps the
  model "free of the `Content` vtable." _Rejected:_ pulling `Editable` down
  into model (illegal down-edge, breaks `check_levels.py`); having the
  transaction call `capture()` itself (same violation, and it would put the
  capture-once discipline in the wrong layer).
- **`CommitSink` / `DamageSink` are model-defined abstract interfaces set on
  the `Model` (writer-owned, single sink each), not concrete history/damage
  code.** The transaction assembles the `JournalEntry` and the damage union
  and *notifies*; the journal and damage-propagation implementations register
  from above at wiring time. This respects "pure data plus change
  notification" (doc 02, doc 17:66-72) and lets `model.journal` and
  `model.damage` land against a frozen surface. _Rejected:_ the transaction
  owning history/propagation directly (would force model→contract/runtime
  up-edges and fuse three siblings into one task).
- **The journal entry stores `(before, after)` as owning `Ref` edges with
  structural sharing, not deltas.** This is doc 14's v1 contract (lines
  175-178) — "impossible to implement incorrectly in ways that corrupt
  history" — and the entry's `Ref`s are exactly the journal-entry references
  that keep superseded versions memory-live (doc 15:119-123). Content changes
  additionally carry the `(before, after)` `StateHandle` pair because undo of
  content is an `Editable::restore(handle)` at a higher layer, not a record
  swap. _Rejected:_ operation/delta records (doc 14 defers these as a later
  extension, line 178).
- **Damage value + accumulator live in `model.transactions`; propagation
  lives in `model.damage`.** This task owns the minimal `Damage {ObjectId,
  Rect, time-range}` value (base-typed), the per-transaction union
  accumulator, the `DamageSink`, and the flush-once-per-commit behavior —
  because the note assigns "damage rides the transaction and flushes once per
  commit" here and the behavior is untestable without a concrete value. The
  sibling `model.damage` reuses this `Damage` value and adds
  auto-damage-on-placement and up-nesting-to-viewport propagation. _Rejected:_
  an opaque/generic damage payload (can't test the union-once claim) or
  deferring the whole value to `model.damage` (would leave this task unable to
  honor its own note).
- **Coalescing = key on the entry + a pure `JournalEntry` merge helper here;
  "consecutive" is decided by `model.journal`.** The merge *semantics*
  (first-before, last-after, unioned damage/object-set) are specified in
  doc 14 and are a pure value operation, so they belong with the entry type in
  this task (which owns "coalescing" per the title). Which entries are
  adjacent in history is a property of history storage, which `model.journal`
  owns. _Rejected:_ putting the whole merge in the journal (splits a
  doc-14-specified semantic away from the coalescing task) or making
  transactions thread history (up-edge to journal).
- **Abort is a discard implemented by dropping the working `d_root` builder;
  documented via a doc 14 delta.** Because nothing is published until the
  `d_current.store` in `commit()`, abort needs no rollback — releasing the
  builder's `Ref<HamtNode>` cascades its unique nodes through the existing
  reclamation queue. The doc previously specified only begin/commit; the delta
  (`docs/design/14-data-model-and-editing.md`, § *Transactions*, **Abort**
  bullet) closes that gap so the `#abort-publishes-nothing` claim enforces a
  documented behavior. _Rejected:_ implementing abort as an inverse publish
  (pointless — the version was never published) or leaving abort undocumented
  (then the claim would enforce an un-specified behavior, violating doc 16).

**Design-doc delta.** One targeted edit to
`docs/design/14-data-model-and-editing.md` § *Transactions* adds the **Abort**
bullet (a dropped/aborted transaction publishes nothing). It is a spec-gap
fill, not a reversal of any project-shaping decision in doc 00, so no doc 00
decision-record bullet is warranted. Docs 14/15/17 otherwise already settle
the transaction shape, levelization, and reclamation model.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-05.

- Shipped full doc-14 `Model::Transaction`: `transact()`, `coalesce(key)`, `abort()`, `set_content_state` (opaque `StateHandle`, records before/after), `remove` with new `hamt_erase` path-copy (shares siblings, collapses single-leaf branches).
- Per-transaction damage accumulator flushed once at commit via model-defined abstract `DamageSink`; one `JournalEntry` assembled per commit and handed to abstract `CommitSink` — both pure-virtual, no concrete impl; levelization L2 preserved.
- New files: `src/model/arbc/model/damage.hpp`, `src/model/arbc/model/journal_entry.hpp`, `src/model/t/transactions.t.cpp`.
- Edited files: `src/model/arbc/model/model.hpp`, `src/model/model.cpp`, `src/model/arbc/model/hamt.hpp`, `src/model/CMakeLists.txt`, `tests/claims/registry.tsv`, `docs/design/14-data-model-and-editing.md` (Abort bullet added to §Transactions).
- 5 claims registered and enforced: `commit-publishes-once`, `abort-publishes-nothing`, `damage-flushes-once-per-commit`, `commit-appends-one-journal-entry`, `coalesced-commits-merge-to-one-entry`.
- All 22 model tests pass; gate + asan-gate green; `check_claims` (24 enforced), `check_levels` OK.
- Deferred: `model.composition_membership` (~1.5d) — populate `CompositionRecord.layers[]`/`layer_count` with add/remove/reorder mutators incl. inline-cap spill; registered in WBS, wired to `m2_editing`.
