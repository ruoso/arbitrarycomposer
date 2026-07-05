# model.journal — Journal, undo/redo

## TaskJuggler entry

`task journal "Journal, undo/redo"` in
[`tasks/10-model.tji`](../../10-model.tji) (lines 35-40), under
`task model "Versioned model"`. Note line: _"Document-wide history: per-entry
placement deltas + (before, after) state handle pairs + damage sets; undo/redo
as ordinary forward publishes; byte budget via state_cost, tail trimming.
Doc 14."_

## Effort estimate

**3d** (`effort 3d`, `tasks/10-model.tji:36`). The `JournalEntry` value, its
`coalesce_entries` merge helper, and the `CommitSink`/`DamageSink` seams already
exist (`model.transactions`, `25e4b41`), so this task is the *history machine*
that consumes them: the entry store + cursor, the two-way navigation publish,
gesture threading, and the byte-budget trimmer with its L3 cost seam. The 3d
over `transactions`' 2d reflects the navigation-publish primitive (re-binding
stored owning edges into a new version) and the two new L2/L3 seams
(`StateCostFn`, `RestoreSink`) this task freezes for `model.editable_facet`.

## Inherited dependencies

- **`model.transactions`** — **settled** (landed `25e4b41`). Provides the whole
  surface this task consumes:
  - `struct JournalEntry { name; coalesce_key; std::vector<ObjectEdit> objects;
    std::vector<ContentStateEdit> contents; std::vector<Damage> damage; }`
    (`src/model/arbc/model/journal_entry.hpp:43-49`) — the unit the journal
    stores. `ObjectEdit { object; Ref<ObjectRecord> before; Ref<ObjectRecord>
    after; }` (`journal_entry.hpp:24-28`): an empty `before` marks an add, an
    empty `after` marks a remove. `ContentStateEdit { object; StateHandle
    before; StateHandle after; }` (`journal_entry.hpp:33-37`). The comment at
    `journal_entry.hpp:39-42` already states the entry "carries enough for a
    pure inverse publish so `model.journal` implements undo without re-reading
    the live objects" — this task is that consumer.
  - `void coalesce_entries(JournalEntry& base, const JournalEntry& follow)`
    (`journal_entry.hpp:55`) — the pure first-before/last-after value merge;
    `transactions` explicitly left "WHICH entries are consecutive in history"
    to `model.journal` (`journal_entry.hpp:53-54`).
  - `class CommitSink { virtual void on_commit(JournalEntry entry) = 0; }`
    (`journal_entry.hpp:60-64`) and `class DamageSink { virtual void
    flush(const std::vector<Damage>&) = 0; }` (`damage.hpp:60-64`), installed on
    the `Model` via `set_commit_sink` / `set_damage_sink`
    (`model.hpp:110-111`, writer-owned single sink each). The journal is the
    concrete `CommitSink`.
  - `CoalesceKey = std::uint64_t`, `k_no_coalesce = 0` (`journal_entry.hpp:16-17`);
    `Transaction::coalesce(CoalesceKey)` (`model.hpp:149`) already stamps the key
    onto the emitted entry.
  - `hamt_insert` / `hamt_erase` / `hamt_lookup` (`hamt.hpp:133,141,147`) — the
    path-copy primitives the navigation publish rebuilds versions with.
  - `Model::current()` (`model.hpp:91`), `std::atomic<DocStatePtr> d_current`
    (`model.hpp:208`), `Model::drain()` (`model.hpp:99`), `Model::live_slots()`
    (`model.hpp:104`).
- **`pool.refs` / `pool` arenas** — settled transitively; the journal holds and
  releases the entry's owning `Ref<ObjectRecord>` edges (`refs.hpp:100`, `slot()`
  at `:183`, `operator bool` at `:166`) and reuses the model's existing
  `RefStore` / `Arena` — it allocates no new store.

No **pending** inherited dependencies.

## What this task is

Build the **document-wide history** doc 14 §History promises: a core-owned
journal that stores the `JournalEntry` stream, navigates it with undo/redo, and
bounds it by bytes. Concretely, an `arbc::model` (L2) `Journal` that (1)
implements `CommitSink` and, on each `on_commit`, appends the entry (or merges
it into the tip under a matching coalescing key) and drops any redo tail; (2)
implements **undo/redo as ordinary forward publishes** — a *navigation publish*
that rebinds each touched object's record edge to the entry's `before` (undo) or
`after` (redo), publishes it as a new revision, and flushes the entry's stored
damage once, **without appending a new entry** (history is never mutated); (3)
budgets the stored entries by bytes (record sizes the journal knows at L2, plus
content-state cost via a registered L3 seam) and **trims from the tail**,
releasing the trimmed entries' owning edges so version GC reclaims the
uniquely-superseded records. It also freezes the two L2/L3 seams
`model.editable_facet` needs — a `StateCostFn` (byte budgeting →
`Editable::state_cost`) and a `RestoreSink` (content undo/redo →
`Editable::restore`) — so that task lands against a settled surface.

## Why it needs to be done

`model.transactions` assembles a `JournalEntry` per commit and hands it to a
`CommitSink`, but no one stores it, navigates it, or bounds it — there is no
undo. Two consumers are blocked on the surface this task freezes:

- **`model.editable_facet`** (`tasks/10-model.tji:47-52`, `depends !journal`) —
  registers the concrete `StateCostFn` (dispatching to `Editable::state_cost`)
  and the concrete `RestoreSink` (dispatching to `Editable::restore`), and adds
  the capture-once-per-gesture discipline + conformance-suite entries on top.
  It needs the journal's entry store, cursor semantics, and both seam signatures
  frozen so it "can consume it without guessing."
- **`m2_editing`** (`tasks/99-milestones.tji:21-23`) — the versioned-editing
  milestone depends directly on `model.journal`; undo/redo is the headline
  capability it marks.

Doc 14 §History (lines 165-172) is explicit that undo/redo must be *ordinary
publishes* so "outputs and viewports need no special path (undo during playback
or streaming just works)"; the transactions task already made every publish
atomic, so the journal reuses that machinery rather than inventing a rollback
path.

## Inputs / context

Design docs (normative — doc 16's executable-spec discipline):

- `docs/design/14-data-model-and-editing.md`
  - § *History* (lines 163-183) — the whole task. Entry shape (line 168-169:
    "name, coalescing key, per-object placement deltas, per-content (before,
    after) state handle pairs, damage set"); **undo publishes a new version
    applying the entry's inverse; redo re-applies. Both are ordinary publishes**
    (lines 170-172); **budgeted by bytes via `state_cost` (+ record sizes),
    trimmed from the tail; version GC releases unpinned `DocState` nodes and
    unreferenced state handles by refcount** (lines 173-179); handle pairs with
    structural sharing are the **v1 contract** — deltas/operation records are a
    deferred extension (lines 180-183).
  - § *The central decision: versions, not mutations* (lines 40-43) — **"History
    is a journal of versions … undo publishes a new version restoring prior
    state — history itself is never mutated, so redo, coalescing, and history
    inspection are trivial and always consistent."**
  - § *Content state: the `Editable` facet* (lines 103-161) — the L3 contract
    this task defers to via seams: `capture()`/`restore()`/`state_cost()`
    (lines 111-122); "The transaction stores (before, after) state handle pairs
    in the journal entry; the published `DocState` holds the *after* handle"
    (lines 133-135); `restore()` is the "undo/redo path; emit damage for what
    changed" (line 117).
  - § *Transactions* (lines 69-101) — **Coalescing** (lines 86-91:
    "`txn.coalesce(gesture_id)` merges consecutive journal entries so undo lands
    at gesture granularity while the display still updates per-commit"); the
    atomic-publish and damage-flush machinery the navigation publish reuses.
- `docs/design/15-memory-model.md`
  - § *Version reclamation* (lines 112-154) — a version is memory-live while
    pinned **or referenced by a journal entry's state handles** (lines 119-123);
    **"the writer thread is the only structural allocator"** (lines 137-143);
    cascades are deferred, never inline (lines 130-136). This is why trimming
    the tail is what makes superseded records reclaimable, and why undo/redo (a
    writer op) is the correct place to publish.
- `docs/design/17-internal-components.md`
  - Component table (line 52): `arbc::model` is **L2**, and its listed contents
    include **`journal/undo`** — the journal is a task *inside* `arbc::model`,
    not a separate component. Depends on **`base`, `pool` only**.
  - Levelization rule (lines 41-44) — "A component may depend only on strictly
    lower levels"; CI validates the CMake + include graph
    (`scripts/check_levels.py`, `"model": {"base", "pool"}`).
  - `contract` sits **above** `model` (line 53, rationale lines 66-72): the
    `Editable`/`Content` vtable is L3; "The model stays free of the `Content`
    vtable … preserving 'pure data plus change notification'." This forces the
    journal to reach `Editable::state_cost`/`restore` only through
    model-defined abstract seams, exactly as `transactions` did for
    `CommitSink`/`DamageSink`.
- `docs/design/16-sdlc-and-quality.md`
  - § *Test taxonomy* — unit tests explicitly list the **journal** as core
    machinery (Catch2, exhaustive on edge cases); tier-4 behavioral counters
    (lines 54-62, "Wall-clock tests lie in CI; counters don't" — the exposed
    slots-allocated/reclaimed counters); tier-6 concurrency.
  - § *CI structure* — diff-coverage hard gate ≥90% on changed lines (lines
    112-118).
  - Claims mechanics: `tests/claims/registry.tsv` (two tab-separated columns
    `<claim-id>\t<description>`; a claim id is `<doc-file-stem>#<slug>`) and
    `scripts/check_claims.py` (the `// enforces: <claim-id>` tag scan).

Source seams this task extends (all at `25e4b41`):

- `src/model/arbc/model/journal_entry.hpp:16-64` — `CoalesceKey`,
  `k_no_coalesce`, `ObjectEdit`, `ContentStateEdit`, `JournalEntry`,
  `coalesce_entries`, `CommitSink` (all consumed here; this task adds no field to
  `JournalEntry`).
- `src/model/arbc/model/damage.hpp:19-64` — `Damage {ObjectId, Rect, Time
  start, Time end}` and `DamageSink` (the navigation publish flushes the stored
  `entry.damage` through the model's installed `DamageSink`).
- `src/model/arbc/model/model.hpp:81-214` — `class Model`: `current()` (:91),
  `set_commit_sink`/`set_damage_sink` (:110-111), `Transaction` (:113-188) with
  `coalesce` (:149) and `commit` (:162), `transact()` (:190), `drain()` (:99),
  `live_slots()` (:104), private `d_bundle` (:201), `d_current` (:208),
  `d_commit_sink`/`d_damage_sink` (:212-213). **This task adds one writer-only
  navigation-publish primitive here** (see Decisions).
- `src/model/arbc/model/hamt.hpp:39-41` (`StoreBundle`), `:133`
  (`hamt_insert`), `:141` (`hamt_erase`), `:147` (`hamt_lookup`) — the journal's
  navigation publish rebuilds a version by re-binding / erasing the touched ids.
- `src/model/arbc/model/records.hpp:38` (`StateHandle { SlotIndex slot }`,
  `k_state_none` at :29), `:47` (`ContentRecord`), `:85` (`ObjectRecord`),
  `:99-113` (`static_assert`s) — the record edges an `ObjectEdit` holds. Since an
  `ObjectRecord` for a content object embeds its `ContentRecord.state` handle,
  rebinding the *before* `ObjectRecord` restores the *before* published
  `StateHandle` automatically — no `Editable` call is needed to make the
  published state correct.
- `src/pool/arbc/pool/refs.hpp:100` (`Ref`, owning), `:166` (`operator bool` —
  empty-edge test for add/remove), `:183` (`slot()` → `SlotRef` for
  `hamt_insert`) — the journal reuses the entry's owning edges, retaining (never
  deep-copying) them into the republished version.
- `src/model/CMakeLists.txt` — `arbc_add_component(NAME model … DEPENDS base
  pool)`; unit tests via `arbc_component_test(COMPONENT model SOURCES t/…)`.
- Existing tests for style: `src/model/t/transactions.t.cpp` (Catch2;
  `RecordingCommitSink`/`RecordingDamageSink` doubles at :15-31, coalescing test
  at :267-310 which exercises only the *pure* `coalesce_entries` helper — the
  history threading is unbuilt and is this task's; concurrency smoke at :346),
  `src/model/t/persistent_state.t.cpp`.

Predecessor-refinement conventions:
`tasks/refinements/model/transactions.md`,
`tasks/refinements/model/persistent_state.md` (behavioral counters over
wall-clock; asan-lane concurrency; abstract model-defined seams whose concrete
consumers wire from above).

## Constraints / requirements

- **Levelization (doc 17, CI-gated).** The `Journal` lives in `arbc::model` at
  L2 (doc 17:52 lists `journal/undo` as model contents), depending only on
  `base` and `pool`. It **never** includes or calls `Editable`/`Content`. The two
  operations doc 14 routes through `Editable` are reached through model-defined
  abstract seams the L3 `model.editable_facet` registers:
  - `StateCostFn` — `virtual std::size_t cost(const StateHandle&) const = 0;` —
    the byte-budget's content-state coster (→ `Editable::state_cost`). When none
    is registered, a content handle contributes **0** to the budget and only the
    record sizes are counted (the budget still bounds record growth).
  - `RestoreSink` — `virtual void on_restore(ObjectId content, StateHandle
    target) = 0;` — invoked once per `ContentStateEdit` during a navigation
    publish so the live L3 content object adopts the target state
    (→ `Editable::restore`). When none is registered it is a no-op; the
    *published* record state is already correct (the rebound `ObjectRecord`
    carries the target `StateHandle`), so render workers reading the pinned
    handle are unaffected. `scripts/check_levels.py` must stay green.
- **Undo/redo are ordinary forward publishes; history is never mutated
  (doc 14:40-43, 170-172).** Undo/redo build the next version with the model's
  existing path-copy primitives and swap `d_current` atomically (revision `+1`),
  exactly like a commit — outputs/viewports need no special path. They **do not
  append or rewrite a journal entry**; they move a cursor. The `CommitSink` is
  **not** notified by a navigation publish (otherwise navigation would re-journal
  itself). The entry's stored `damage` is flushed once through the installed
  `DamageSink` (doc 14: "Undo/redo replays the entry's damage so invalidation is
  exactly right without diffing").
- **Inverse via re-bound owning edges, not re-capture.** For each `ObjectEdit`,
  the navigation target is `after` (redo) or `before` (undo). An **empty** target
  `Ref` → `hamt_erase(id)` (the object did not exist in that direction); a
  non-empty target → `hamt_insert(id, target.slot())`, **reusing the entry's
  owning edge** (retained into the new version, shared by `SlotRef` identity —
  never deep-copied, doc 14:175-178). This is why undo→redo round-trips to the
  same `SlotRef` edges. Content-state undo needs no record re-capture because the
  target `ObjectRecord` already embeds the target `StateHandle`.
- **Single-writer (doc 14:71, doc 15:137-143).** The journal store, the cursor,
  trimming, and every navigation publish are **writer-thread only**. Readers keep
  pinning `DocStatePtr` and traversing via `peek`; a slow export pinning an old
  version keeps only what it references alive (doc 14:175-176) — trimming an
  entry does not free a version another party still pins.
- **Coalescing threads at the tip (doc 14:86-91).** On `on_commit(entry)` with a
  non-zero `entry.coalesce_key`: if the cursor is at the tip **and** the tip
  entry carries the same key, fold the new entry into the tip via
  `coalesce_entries` (first-before/last-after, unioned damage/objects/contents) —
  no new slot, no cursor move. Otherwise append. A `k_no_coalesce` (0) entry
  always appends. The result: one `undo()` reverts an entire gesture, while each
  commit already published its own revision (display updated per-commit).
- **Redo tail is dropped on a fresh commit (doc 14:43 — "always consistent").**
  A non-coalescing `on_commit` while the cursor is not at the tip discards the
  entries after the cursor (their owning edges release) before appending, then
  sets the cursor to the new tip. Surviving entries are never rewritten.
- **Byte budget + tail trimming (doc 14:173-179).** The journal tracks an
  accumulated byte cost = Σ over stored entries of (record sizes it computes at
  L2: `sizeof(ObjectRecord)` per non-empty `before`/`after` edge) + (Σ content
  `StateHandle` cost via the registered `StateCostFn`, else 0). After each append
  it trims **from the oldest end** until within a configured budget (never
  trimming below one entry). Dropping an entry releases its owning `Ref` edges;
  on the next `Model::drain()` the uniquely-superseded records are reclaimed by
  refcount (doc 15:119-123) — the behavioral witness is `live_slots()` falling.
- **Transient types only.** The `Journal`, its entry vector, and the two seams
  are heap/handle types (they hold `Ref`/STL); none is an in-arena record, so the
  `records.hpp:99-113` `static_assert`s are untouched. `StateHandle`/`Damage`
  cross the seams by value.

## Acceptance criteria

- **Unit tests** (`src/model/t/journal.t.cpp`, wired via
  `arbc_component_test(COMPONENT model …)` in `src/model/CMakeLists.txt`; Catch2,
  reusing the `RecordingCommitSink`/`RecordingDamageSink` doubles' style from
  `transactions.t.cpp:15-31`, plus a counting `StateCostFn`/`RestoreSink`
  double):
  - install the journal as the model's commit sink; commit N mutations, then
    `undo()`/`redo()` and assert `current()` observes the expected records at
    each cursor position; `can_undo()`/`can_redo()` track the cursor;
  - a navigation publish notifies the `RestoreSink` once per `ContentStateEdit`
    with the correct target handle, and flushes the entry's damage exactly once,
    while the `CommitSink` is **not** re-entered;
  - a fresh commit after an `undo()` drops the redo tail; a coalescing run folds
    to one undoable step;
  - trimming: with a small byte budget and a counting `StateCostFn`, oldest
    entries drop until within budget and `can_undo()` shrinks.
- **Behavioral-counter assertions (doc 16:54-62 — counters, never wall-clock).**
  Assert on `Model::revision()` deltas (each undo/redo `+1`), `DamageSink`
  call-count (one per navigation), `RestoreSink`/`StateCostFn` call-counts,
  cursor depth, and `Model::live_slots()` (round-trips across undo/redo; falls
  after a trim + `drain()`).
- **Claims (register in `tests/claims/registry.tsv`; enforce with an
  `// enforces: <claim-id>` tagged test — `scripts/check_claims.py` gates both
  directions):**
  - `14-data-model-and-editing#undo-is-a-forward-publish` — an `undo()` publishes
    a new version (revision `+1`) that rebinds every touched object to the
    entry's *before* edge and flushes the entry's damage through the `DamageSink`
    exactly once, **without** notifying the `CommitSink` and with no special
    output path; `redo()` is the symmetric *after* re-application.
  - `14-data-model-and-editing#undo-redo-round-trips` — `commit → undo → redo`
    returns the published version's object-record edges to the same `SlotRef`
    identities as the post-commit version, and returns the cursor to the tip; a
    full undo-to-base then redo-to-tip round-trips `live_slots()` (structural
    sharing, no leak).
  - `14-data-model-and-editing#history-is-never-mutated` — undo/redo only move
    the cursor and never rewrite a stored entry; a fresh non-coalescing commit
    while the cursor is not at the tip discards the redo tail and appends but
    leaves every surviving entry byte-identical.
  - `14-data-model-and-editing#journal-trims-to-byte-budget` — when the
    accumulated cost (record sizes + registered `StateCostFn`) exceeds the
    budget, the journal drops oldest entries from the tail until within budget
    (never below one entry); after a `drain()` the uniquely-superseded records
    those entries pinned are reclaimed (`live_slots()` falls) and `can_undo()`
    reflects the reduced depth.
  - `14-data-model-and-editing#coalesced-gesture-undoes-as-one` — a run of
    commits sharing a non-zero coalescing key collapses into a single tip entry,
    so one `undo()` reverts the whole gesture to its pre-gesture state, while
    each commit had published its own revision (display updated per-commit).
- **Concurrency (doc 16 tier 6).** Extend the pin/traverse-vs-writer smoke
  (`transactions.t.cpp:346`) so the writer also runs `undo()`/`redo()` cycles
  (navigation publishes) against readers pinning and traversing older versions;
  assert no torn read and no use-after-free under the **asan lane** (no TSan
  preset in-tree yet — the parked convention from
  `tasks/refinements/pool/reclamation.md`; full seeded schedule-perturbation
  stress remains `quality.stress_harness`, `tasks/70-quality.tji`, not duplicated
  here).
- **Coverage / gate.** ≥90% diff coverage on changed lines; `scripts/gate` green
  including asan, `scripts/check_levels.py`, `scripts/check_claims.py`.
- **No deferred follow-up (closer registers nothing new).** The two L2/L3 seams
  this task defines (`StateCostFn`, `RestoreSink`) are consumed by the
  already-planned `model.editable_facet` leaf (`tasks/10-model.tji:47-52`,
  `depends !journal`, already wired into `m2_editing`); no new WBS leaf is
  warranted. The doc-14 "operation/delta records" optimization stays a design-doc
  deferred extension (line 180-183) — nothing in `m2_editing` needs it, so it is
  not encoded as a task.

## Decisions

- **The `Journal` is an `arbc::model` (L2) `CommitSink`, and reaches `Editable`
  only through two model-defined abstract seams.** Doc 17:52 places `journal/undo`
  in `arbc::model`; doc 17:66-72 keeps model free of the `Content` vtable.
  Byte-budget costing needs `Editable::state_cost` and content undo needs
  `Editable::restore`, both L3 — so the journal defines `StateCostFn` and
  `RestoreSink` (pure change-notification, doc 02) and `model.editable_facet`
  registers concrete ones, exactly mirroring how `transactions` introduced
  `CommitSink`/`DamageSink`. _Rejected:_ placing the journal in `arbc::contract`
  at L3 (contradicts doc 17:52 and would force a model→contract inversion for the
  navigation publish, which lives where `d_current` is); pulling `Editable` down
  into model (illegal down-edge, breaks `check_levels.py`).
- **Undo/redo are a *navigation publish* — a new writer-only `Model` primitive
  that rebinds the entry's owning edges and does not notify the `CommitSink`.**
  The journal drives undo/redo through a single new `Model` method (writer-only)
  that, given a `JournalEntry` and a direction, `hamt_insert`s each non-empty
  target edge (reusing the stored owning `Ref`) / `hamt_erase`s each empty one,
  builds a `DocRoot` at revision `+1`, `d_current.store`s it, and flushes the
  entry's damage once through `d_damage_sink` — but never touches `d_commit_sink`.
  This makes undo/redo "ordinary publishes" (doc 14:170-172) while keeping
  "history is never mutated" (doc 14:43): navigation cannot re-journal itself.
  Living on `Model` keeps the atomic swap, revision bump, and damage flush next
  to `d_current`, and reuses the exact path-copy code commits use. _Rejected:_
  the journal opening a normal `Transaction` and committing the inverse (would
  re-enter the `CommitSink` and append a spurious entry, violating "never
  mutated", and the transaction's `add_*` mint fresh ids rather than re-binding a
  removed object's *exact* id to its stored record); temporarily clearing the
  commit sink around an inverse transaction (fragile, and still cannot re-bind a
  stored owning `Ref` at an exact id via the mutator API).
- **The inverse reuses the stored owning `Ref` edges by identity; content undo
  needs no re-capture.** Because an `ObjectRecord` for a content object embeds its
  `ContentRecord.state` `StateHandle`, rebinding the *before* `ObjectRecord`
  restores the *before* published handle automatically — the render path (pure
  over `StateHandle`, doc 14:155-161) is correct with zero `Editable` calls. The
  `RestoreSink` fires *in addition*, only so the live L3 editing object follows
  (its next `capture()` builds on the restored base and it emits damage per
  doc 14:117) — it is not on the correctness path for outputs. This cleanly
  splits "what render workers see" (L2 record state) from "what the live editor
  holds" (L3 `Editable`). _Rejected:_ storing content undo as a record-level
  `set_content_state` inside the navigation (redundant — the `before`
  `ObjectRecord` already carries it) and routing the render-visible restore
  through `Editable` (an L3 dependency for an L2 correctness property).
- **`StateCostFn` unregistered ⇒ content cost 0; record sizes always counted.**
  The journal must honor its own byte-budget note even before
  `model.editable_facet` lands a coster, so an absent `StateCostFn` degrades to
  counting only record sizes (which the journal knows at L2). This keeps the
  trimming claim testable in this task with a double, and keeps the budget a real
  bound on record growth in the interim. _Rejected:_ making a coster mandatory
  (couples journal landing to editable_facet, inverting the dependency); ignoring
  content cost entirely (the budget would under-count exactly the large payloads
  it exists to bound).
- **Coalescing threads only at the tip; `coalesce_entries` is reused verbatim.**
  `transactions` already specified and tested the pure merge; the journal adds
  only the "consecutive" decision — same non-zero key **and** cursor at tip. Any
  redo tail or a keyless commit breaks the run. This is the standard editor
  gesture-granularity behavior (doc 14:86-91) with no new merge semantics.
  _Rejected:_ merging across non-adjacent entries or across a redo boundary
  (would let an undo skip mid-history state, breaking "always consistent");
  re-deriving a merge in the journal (duplicates the `journal_entry.hpp` helper).
- **History storage is a `std::vector<JournalEntry>` + an integer cursor;
  trimming pops the front.** Entries are append-mostly with tail trimming and a
  tip merge; a vector gives O(1) tip access and cheap contiguous iteration for a
  future history-inspection panel (doc 14:43). Front-trim cost is amortized and
  the budget keeps depth bounded. _Rejected:_ a deque/linked list (no measured
  need; the vector's identity-stable entries suffice) — revisit only if
  profiling shows front-trim churn, which the behavioral-counter suite would
  surface.

**No design-doc delta.** Doc 14 §History already specifies the entry shape,
undo/redo-as-forward-publish, the never-mutated invariant, and byte-budget tail
trimming; doc 15 §Version reclamation specifies the refcount GC trimming relies
on; doc 17:52/66-72 already settles the L2 placement and the change-notification
seam pattern. The `StateCostFn`/`RestoreSink` seams are the L2/L3-boundary
realization of the already-documented `Editable::state_cost`/`restore` — the same
kind of plumbing `transactions`' `CommitSink`/`DamageSink` were, which needed no
delta. The one new `Model` navigation-publish method is an implementation seam
within the model, not a change to designed behavior.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-05.

- `src/model/arbc/model/journal.hpp` — `Journal` (L2 `CommitSink`) with `undo()`/`redo()`/`can_undo()`/`can_redo()`, plus the two model-defined L2/L3 seams `StateCostFn` and `RestoreSink`.
- `src/model/journal.cpp` — `on_commit` (append / tip-coalesce / redo-tail-drop / byte-budget trim), `undo`/`redo` as forward navigation publishes, cost and trim helpers.
- `src/model/t/journal.t.cpp` — 7 Catch2 unit tests (incl. ASan-lane concurrency smoke); claims enforced: `undo-is-a-forward-publish`, `undo-redo-round-trips`, `history-is-never-mutated`, `coalesced-gesture-undoes-as-one`, `journal-trims-to-byte-budget`.
- `src/model/arbc/model/model.hpp` + `src/model/model.cpp` — new writer-only `Model::navigate(entry, NavDirection)` primitive (rebinds owning edges, revision +1, flushes damage once, never notifies `CommitSink`).
- `src/model/CMakeLists.txt` — wired `journal.cpp`, `journal.hpp`, `t/journal.t.cpp`.
- `src/model/t/transactions.t.cpp` — minor updates for compatibility with new `navigate` primitive.
- `tests/claims/registry.tsv` — registered 5 new `14-data-model-and-editing#…` claims.
- Gate: 29 test cases / 13 288 assertions green in both `dev` and `asan`; claims and levelization clean.
