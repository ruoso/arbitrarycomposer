# model.composition_membership — Composition layer membership mutators

## TaskJuggler entry

`tasks/10-model.tji:29-34` — `task composition_membership "Composition
layer membership mutators"`:

```
task composition_membership "Composition layer membership mutators" {
  effort 2d
  allocate team
  depends !transactions
  note "Populate CompositionRecord layers array and layer_count with add/remove/reorder mutators incl. inline-cap spill to HAMT-backed layer list (beyond k_max_inline_layers=8). Source: tasks/refinements/model/transactions.md (deferred follow-up). Doc 14."
}
```

Registered debt leaf spun out of `model.transactions`
(`tasks/refinements/model/transactions.md`, Acceptance-criteria "Deferred
follow-up"). Milestone: the versioned-model milestone (`m2_editing`) that
carries the other `model.*` leaves.

## Effort estimate

2d. Scope: one new object-record arm (the spill chunk) + a spill-root
reference field on `CompositionRecord`, three transaction mutators
(attach / detach / reorder), the inline↔spill migration, a
composition-scoped ordered read accessor on `DocRoot`, plus the Catch2
suite, claims, and the doc-14 delta. No new external dependency, no new
concurrency surface (writer-thread-only, like every other mutator).

## Inherited dependencies

**Settled (from `!transactions`, `!persistent_state`):**

- The **transaction seam** is live: `Model::transact(name)` opens a named,
  writer-thread mutation batch (`model.hpp:269`); mutators path-copy records
  and the map; `commit()` publishes one atomic version and assembles one
  `JournalEntry`; `abort()`/drop discards (`model.hpp:241-247`). New membership
  mutators are additional `Transaction` methods that follow the exact
  `set_transform` shape (`model.cpp:529-559`).
- The **DocState HAMT** over `ObjectId` is live: `hamt_insert`
  (`hamt.hpp:133`), `hamt_erase` (`hamt.hpp:141`, path-copy that shares
  untouched siblings and collapses emptied branches), `hamt_lookup`
  (`hamt.hpp:147`). Any object placed in the map is owned, path-copied, and
  reclaimed by this machinery — no per-record ownership code.
- The **record model** is fixed: `RecordKind` (`records.hpp:25`),
  `ObjectRecord` tagged union (`records.hpp:85-97`), and the standard-layout /
  trivially-destructible / pointer-free `static_assert`s (`records.hpp:99-113`).
  `CompositionRecord` already carries the inline `layers[k_max_inline_layers]`
  array and `layer_count` (`records.hpp:70-78`), **currently never written**:
  `Transaction::add_composition` constructs a default record and sets only the
  canvas dims (`model.cpp:500-527`).
- **Damage** rides the transaction: `Transaction::add_damage`
  (`model.hpp:233`, `model.cpp` `damage_add` union `damage.hpp:44`), flushed
  once per commit through the writer-owned `DamageSink` (`damage.hpp:60-64`).
- The **journal** captures every touched object automatically as an
  `ObjectEdit { object, Ref<ObjectRecord> before, Ref<ObjectRecord> after }`
  (`journal_entry.hpp:24-28`); undo rebinds the *before* owning edge. A
  membership edit that only rewrites records needs **no new journal edit type**.

**Pending:** none. `!transactions` and its own predecessors
(`!persistent_state`, `!journal`) are all `complete 100`.

## What this task is

Turn `CompositionRecord`'s dormant `layers[]` / `layer_count` fields
(`records.hpp:70-78`) into live, mutated state. Add three writer-thread
transaction mutators that edit a composition's ordered (bottom-to-top,
doc 01:6-11) layer membership:

- **attach** a layer id into a composition's order at a given index (append
  past the end),
- **detach** a layer id from a composition's order,
- **reorder** a layer from one index to another (a stable move).

For compositions with at most `k_max_inline_layers = 8` members the order
lives in the inline `layers[]` array. Beyond the cap it **spills to a
HAMT-backed chunk chain** — a new object-record arm whose instances live in
the DocState HAMT keyed by their own `ObjectId`, referenced from the
composition by a plain `ObjectId` (an index-free value, so the composition
record stays trivially destructible). Crossing the cap up migrates inline →
chunks; dropping back to ≤ 8 collapses chunks → inline. The task also adds a
composition-scoped ordered read accessor on `DocRoot` so consumers walk true
membership order rather than the walking-skeleton's global-id order.

## Why it needs to be done

`model.transactions` deferred this because none of its immediate consumers
(journal, damage, editable_facet) needed composition membership, and the
inline-cap spill is a self-contained design
(`tasks/refinements/model/transactions.md`, "Deferred follow-up"). It is now
the missing write path for the record model persistent_state landed: today a
composition can be created (`add_composition`) but nothing can put a layer
into it, so `DocRoot::for_each_layer` (`model.cpp:373-387`) falls back to
"every layer record in ascending object-id order" — an explicit
walking-skeleton stand-in (`model.hpp:53-56`: "explicit layer reorder is
`model.transactions`' concern"). Downstream, the compositor and
recursive-composition traversal (doc 05) need real per-composition ordered
membership; serialization (doc 08) needs the populated order. This task lands
the model-side seam they consume.

## Inputs / context

**Design docs (normative — doc 16's constitution rule):**

- `docs/design/14-data-model-and-editing.md`
  - § *The central decision* (lines 44-56): the composition record explicitly
    owns **"layer order"**; "Value semantics matches the scale. Compositions
    are small graphs (**hundreds of layers** × small placement structs);
    copying the touched path of a persistent map per commit is
    nanoseconds-to-microseconds." — the blessing for O(members) order rewrites.
  - § *Transactions* (lines 70-101): atomicity (one publish), coalescing,
    damage rides the transaction and flushes once at commit, abort discards.
  - § *Transactions* (lines 200-203): "trims, moves, and transition inserts
    are placement/graph transactions — pure core records, no content
    involvement" — membership edits are exactly this class.
  - § *History* (lines 164-183): entry = per-object placement deltas; undo is
    an ordinary inverse publish; structural sharing bounds journal cost.
- `docs/design/01-core-concepts.md`
  - § *Composition* (lines 6-11): "an **ordered list (bottom to top)** of
    layer instances" — order index 0 = bottom, last = top; there is no
    separate z-order field, the array order **is** the z-order.
  - § *Invalidation* (lines 133-139): "Placement changes (transform, opacity,
    **order**) generate damage in the parent automatically." — every
    attach/detach/reorder must damage the parent composition.
- `docs/design/17-internal-components.md`
  - Component table (line 52): `arbc::model`, level 2, deps **base, pool**
    only; CI-gated (lines 41-44, `scripts/check_levels.py:22`
    `"model": {"base", "pool"}`). The spill structure must be built from
    `arbc::pool` primitives / live inside `model`.
  - Lines 66-72: `contract` sits above `model`; records hold opaque content
    slots / `ObjectId`s, never `Content*`.
- `docs/design/16-sdlc-and-quality.md`
  - § claims register (lines 14-26): each normative claim gets a
    `<doc-file-stem>#<slug>` id in `tests/claims/registry.tsv` and an
    `// enforces: <id>` test; a change that alters designed behavior updates
    the design doc in the same commit.
  - § diff coverage (lines 112-118): ≥ 90% on changed lines, hard gate.

**Source seams:**

- `src/model/arbc/model/records.hpp` — `:25` `RecordKind` (Composition=0,
  Layer=1, Content=2), `:33` `k_layer_visible`, `:55-62` `LayerRecord`
  (content id + placement, trivially destructible, references content by
  `ObjectId` not edge), `:64-68` `k_max_inline_layers = 8` + the
  "Unbounded / chunked layer order is deferred to `model.transactions`"
  comment, `:70-78` `CompositionRecord` (`layer_count`, inline
  `layers[k_max_inline_layers]`), `:85-97` `ObjectRecord` tagged union,
  `:99-113` standard-layout / trivially-destructible `static_assert`s.
- `src/model/arbc/model/model.hpp` — `:48-57` `find_composition` /
  `find_layer` / `for_each_layer` (walking-skeleton global-id order),
  `:192-267` `Transaction` (the class the mutators extend), `:200` `add_layer`,
  `:207` `add_composition`, `:211` `set_transform`, `:223` `remove`, `:228`
  `coalesce`, `:233` `add_damage`, `:241` `commit`, `:247` `abort`, `:254`
  `touch`, `:269` `transact`, `:291-292` writer-owned `CommitSink`/`DamageSink`.
- `src/model/model.cpp` — `:373-387` `for_each_layer` (the global-order
  stand-in this task supersedes with a composition-scoped accessor),
  `:500-527` `add_composition` (constructs a default `CompositionRecord{}`,
  leaves `layers[]`/`layer_count` empty — the gap), `:529-559` `set_transform`
  (**the canonical path-copy mutator pattern** the new mutators mirror: lookup
  → `records.create()` → `nr = *old` copy → override field → `hamt_insert` →
  `touch` → `add_damage`), `:615-631` `remove` (`hamt_erase` path).
- `src/model/arbc/model/hamt.hpp` — `:88-121` `HamtNode`, `:133` `hamt_insert`,
  `:141` `hamt_erase`, `:147` `hamt_lookup`.
- `src/model/arbc/model/journal_entry.hpp` — `:24-28` `ObjectEdit`, `:43-49`
  `JournalEntry`, `:60-64` `CommitSink`.
- `src/model/arbc/model/damage.hpp` — `:44` `damage_add`, `:60-64` `DamageSink`.
- `src/base/arbc/base/ids.hpp:11-17` — `ObjectId` (64-bit, `valid()` iff
  `value != 0`); the order list stores these.
- Tests: `src/model/t/transactions.t.cpp:15-31` (recording
  `CommitSink`/`DamageSink` doubles to reuse), `:42-43` (the `// enforces:`
  tag immediately above a `TEST_CASE`). `tests/claims/registry.tsv` (2-column
  TSV `<claim-id>\t<description>`, header lines 1-3), gated by
  `scripts/check_claims.py`.

## Constraints / requirements

- **Levelization.** All new code stays in `arbc::model` (L2), deps `base` +
  `pool` only (`scripts/check_levels.py:22`). The spill chunk is a model
  record; no reach toward `contract`/`Content` (doc 17:66-72). A new public
  header (if any) compiles standalone (`VERIFY_INTERFACE_HEADER_SETS`).
- **Records stay fixed-size, standard-layout, trivially destructible,
  pointer-free (`records.hpp:99-113`).** The composition references its spill
  chain by **`ObjectId` value**, never an owning edge — a trivially
  destructible record has no destructor to release a counted edge and the
  reclaim cascade does not walk record fields (this is exactly why
  `LayerRecord` names its content by `ObjectId`, `records.hpp:52-54`). The new
  chunk arm must satisfy the same `static_assert`s and must not grow the
  `ObjectRecord` union's size class beyond the existing `CompositionRecord`
  arm (8 `ObjectId`s + two doubles).
- **Single publish, atomicity, one damage flush.** A membership edit is one
  or more record rewrites inside one transaction; `commit()` publishes one
  version, assembles one `JournalEntry`, flushes damage once (doc 14:70-101).
- **Order change damages the parent composition** (doc 01:133-139): each
  attach/detach/reorder contributes exactly one `Damage` for the composition
  `ObjectId`, deduped/flushed once at commit.
- **Structural sharing preserved.** A membership edit path-copies only the
  touched records' map paths (the composition record, plus the touched chunk
  records when spilled); every untouched object — including untouched chunks
  and all other layers — is shared by `SlotRef` identity between the pre- and
  post-commit versions (`14-data-model-and-editing#commit-shares-untouched-structure`,
  `registry.tsv`).
- **No-op contract matches the existing mutators.** Absent composition,
  absent layer, out-of-range index, or a mutator called after a prior sticky
  allocation failure → no-op (the sticky `d_status` surfaces at `commit`),
  exactly like `set_transform` / `remove` (`model.cpp:529-536, 615-621`).
- **Writer-thread only.** No new reader-visible lock or refcount traffic; the
  composition-scoped read accessor is a lock-free `peek` traversal like
  `for_each_layer` / `content_state` (`model.hpp:57, 72`).
- **Journal reuse.** Membership edits ride the existing `ObjectEdit`
  before/after owning edges; undo/redo of attach/detach/reorder is an ordinary
  navigation publish with no new entry type.

## Acceptance criteria

Unit tests in `src/model/t/composition_membership.t.cpp` (Catch2), reusing
the recording `CommitSink`/`DamageSink` doubles at
`src/model/t/transactions.t.cpp:15-31`. Coverage: ≥ 90% diff coverage on
changed lines; `scripts/gate` green including asan, `scripts/check_levels.py`,
`scripts/check_claims.py`.

**Behavioral-counter assertions** (doc 16 tier 4 — counters, never
wall-clock):

- attach/detach/reorder each raise `Model::revision()` by exactly 1 per
  commit and fire the `DamageSink` exactly once with the composition id.
- A composition crossing the cap (attach the 9th layer) migrates to chunks:
  `Model::live_slots()` rises by the chunk allocation; dropping back to ≤ 8
  (detach) collapses to inline and, after `drain()`, reclaims the chunk slots.
- A membership edit on a spilled composition path-copies only the touched
  chunk map-paths: `live_slots()` growth is O(touched chunks + path depth),
  not O(members) — witnessed with `DocRoot::record_edge` (`model.hpp:63`)
  showing untouched chunks shared by `SlotRef` identity across versions.

**Claims-register growth** (`tests/claims/registry.tsv`, each with an
`// enforces: <id>` test in `src/model/t/composition_membership.t.cpp`; gated
both directions by `scripts/check_claims.py`). New claims under the
`14-data-model-and-editing#` stem:

- `14-data-model-and-editing#layer-order-is-explicit` — attach at an index,
  detach, and reorder produce exactly the intended bottom-to-top `ObjectId`
  sequence, read back through the composition-scoped accessor; reorder is a
  stable move (relative order of untouched members preserved).
- `14-data-model-and-editing#membership-spills-past-inline-cap` — a
  composition with more than `k_max_inline_layers` members stores its order in
  HAMT-backed chunk objects and reports the correct full order; migrating up
  across the cap and back down is lossless (round-trip equality).
- `14-data-model-and-editing#membership-edit-damages-composition` — each
  attach/detach/reorder contributes exactly one damage record for the
  composition id, flushed once at commit.
- `14-data-model-and-editing#membership-undo-round-trips` — undo of an
  attach/detach/reorder restores the prior order exactly (including across the
  spill boundary), and redo re-applies it, as ordinary journal navigation
  publishes with no new entry type.

**Concurrency (asan lane).** Extend the pin/traverse-vs-writer smoke
(`src/model/t/transactions.t.cpp:346` pattern) so the writer runs
attach/detach/reorder cycles (incl. spill migration) while a pinned reader
walks the composition-scoped accessor: no torn read, no use-after-free. No
TSan preset in-tree yet (parked convention from
`tasks/refinements/pool/reclamation.md`); full seeded schedule-perturbation
stress stays in `quality.stress_harness`, not duplicated here.

**Design-doc delta** (see Decisions) lands in the same commit as the
implementation (doc 16:23-26): a doc-14 paragraph describing composition
layer-order storage + the four new claim ids.

**Deferred follow-up:** none registered as a new WBS leaf. The
composition-scoped read accessor (`DocRoot::for_each_layer_in`) is landed by
this task as the seam consumers adopt; migrating `arbc::compositor` and the
recursive-composition traversal off the global-order `for_each_layer` onto it
is the consumers' own already-planned work (doc 05 / compositor stream), not
a model leaf — surfaced in the return summary, not encoded here (avoids an
orphan/duplicate leaf).

## Decisions

- **Spill representation = a HAMT-backed chunk chain of first-class object
  records, referenced by `ObjectId`.** When a composition exceeds
  `k_max_inline_layers`, its ordered `ObjectId` list migrates into a
  singly-linked chain of a new `ObjectRecord` arm (`RecordKind::LayerOrderChunk`,
  each holding up to `k_max_inline_layers` member ids + a `next` `ObjectId`,
  `0` = end). The chunks are ordinary objects in the DocState HAMT keyed by
  their own `ObjectId`; the composition names the chain head by a plain
  `ObjectId` spill-root field (`0` when inline). Membership edits rewrite the
  affected chunk records via ordinary `hamt_insert`/`hamt_erase`, so the
  existing HAMT owns, path-copies, and reclaims them with **zero new
  ownership machinery**, and each edit path-copies only the touched chunk
  map-paths (`#commit-shares-untouched-structure`). Order rewrites are
  O(members) worst case, explicitly blessed at "hundreds of layers …
  nanoseconds-to-microseconds" (doc 14:44-48).
  _Rejected: a separately-counted radix trie referenced by a `SlotRef` edge
  from `CompositionRecord`._ A trivially destructible slab record has no
  destructor to release a counted downward edge, and the record reclaim
  cascade does not walk record fields — the trie root would leak or underflow.
  The main HAMT root is owned only because `DocRoot` is a heap C++ object
  holding a `Ref<HamtNode>` (`model.hpp:79`); a per-composition record has no
  such owner. Referencing the spill by `ObjectId` value (owned by the map key,
  like `LayerRecord.content`) is the only representation consistent with the
  fixed record contract.
  _Rejected: an intrusive doubly-linked order list threaded through the
  `LayerRecord`s themselves (O(1) splice)._ Elegant, but it abandons the
  inline `layers[]` array that `model.persistent_state` deliberately shipped
  and changes `LayerRecord`'s shape — a redesign of the prescribed record
  model (task note + `records.hpp:64-68`) without cause. The refinement's job
  is to realize that model, not replace it.
- **Inline for ≤ cap, collapse back on drop.** Reads and edits use the inline
  `layers[]` array while `layer_count ≤ k_max_inline_layers`; crossing up
  migrates inline → chunks, dropping to ≤ cap collapses chunks → inline and
  reclaims the chunk objects. _Rejected: sticky spill (once spilled, stay
  spilled)._ Collapsing keeps the common small-composition case allocation-free
  and gives a **canonical record for a given membership** (a small composition
  is always inline), which future serialization dedup (doc 08) and
  round-trip claims rely on; the extra migrate-down cost is bounded and rare.
- **Mutator surface: `attach_layer(composition, layer, at_index)`,
  `detach_layer(composition, layer)`, `reorder_layer(composition, from_index,
  to_index)` on `Transaction`.** Attach is index-positional (append when
  `at_index ≥ layer_count`); detach is by member id (the caller knows which
  layer); reorder is a positional stable move. A convenience
  `attach_layer(composition, layer)` appends at the top. Each mirrors the
  `set_transform` path-copy shape (`model.cpp:529-559`): rewrite the
  composition record (and touched chunks), `touch`, `add_damage(Damage{composition,…})`.
  _Rejected: folding attach into `add_layer`._ Keeping layer **creation**
  (`add_layer`, `model.hpp:200`) orthogonal to **membership** lets a layer be
  moved between compositions as detach-then-attach and matches the existing
  free-floating-then-placed model.
- **Membership rides the existing journal `ObjectEdit`; no new entry type.**
  A membership edit only rewrites records, so `commit()`'s existing
  touched-object → `(before, after)` owning-edge assembly
  (`journal_entry.hpp:24-28`) captures it; undo rebinds the *before*
  composition (and *before* chunk) records, restoring prior order with
  structural sharing keeping the before-version's chunks alive.
  _Rejected: a dedicated membership-delta entry._ Superfluous — the record
  rewrite is the delta, exactly as `set_transform` needs no bespoke entry.
- **Composition-scoped ordered read accessor `DocRoot::for_each_layer_in(
  ObjectId composition, fn)`** (lock-free `peek`, inline-or-chain aware),
  landed alongside the mutators so tests and consumers read true membership
  order. The global-order `for_each_layer` (`model.cpp:373-387`) stays as-is;
  consumers migrate on their own schedule (see Deferred follow-up).

**Design-doc delta.** Doc 14's record model (lines 50-56) says the
composition record owns "layer order" but is silent on the inline cap and the
spill representation — the survey confirms `k_max_inline_layers` / HAMT-spill
appear in no design doc, and doc 16:23-26 requires designed behavior to be
documented. The implementer adds, in the same commit, a short paragraph to
`docs/design/14-data-model-and-editing.md` (§ *The central decision* /
*Transactions*) stating: a composition's ordered layer list is inline up to a
small fixed cap and spills to a HAMT-backed chain of order-chunk objects
beyond it; attach/detach/reorder are placement/graph transactions that damage
the parent and undo as ordinary inverse publishes; and citing the four new
claim ids. This is a record-representation detail below project-shaping, so it
takes **no** `docs/design/00-overview.md` decision-record bullet (the
existing goal-10 / editing-model bullets already cover the versioned model).

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-05.

- `src/model/arbc/model/records.hpp` — added `RecordKind::LayerOrderChunk`, `LayerOrderChunk` record arm + static_asserts, and `CompositionRecord::spill_root` field.
- `src/model/arbc/model/model.hpp` — added `Transaction::attach_layer` (×2 overloads), `detach_layer`, `reorder_layer`, `DocRoot::for_each_layer_in`; private `store_membership` helper.
- `src/model/model.cpp` — `read_layer_order` helper, composition-scoped read accessor, three mutators, spill-aware `store_membership` (prefix-chunk sharing, inline↔chunk migration).
- `docs/design/14-data-model-and-editing.md` — doc-14 delta: inline-cap + HAMT spill; membership edits as placement/graph transactions; cites 4 new claim ids.
- `tests/claims/registry.tsv` — 4 new claims: `14-data-model-and-editing#{layer-order-is-explicit, membership-spills-past-inline-cap, membership-edit-damages-composition, membership-undo-round-trips}`.
- `src/model/CMakeLists.txt` — registers the new test target.
- `src/model/t/composition_membership.t.cpp` — new Catch2 suite: unit + counters + asan concurrency; each claim has an `// enforces:` test.
