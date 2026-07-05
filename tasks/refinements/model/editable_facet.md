# Refinement — `model.editable_facet`

## TaskJuggler entry

`tasks/10-model.tji:48-53` — `task editable_facet "Editable facet plumbing"`,
under `task model` (10 — Versioned model, doc 14).

> note "capture()/restore()/state_cost() contract; pinned StateHandle
> travels with render requests (purity refinement); capture-once-per-gesture
> discipline; conformance suite entries. Docs 14/03."

## Effort estimate

**2d** (`tasks/10-model.tji:49`). The new code is one model-defined L3 seam
(`StateRefSink`) plus its two call sites (retain on record creation, release
on record reclamation), one pinned-version resolver, and the claims/tests.
The other two editable seams (`StateCostFn`, `RestoreSink`) already landed
and are already invoked by `model.journal`; this task only registers into
them and makes their operands (live `StateHandle`s) real.

## Inherited dependencies

**Settled:**

- `model.journal` (`d27a9d2`, `tasks/10-model.tji:35-40`, `depends !journal`
  at `:51`). Froze the two L2/L3 editable seams this task consumes:
  - `StateCostFn` — `virtual std::size_t cost(const StateHandle&) const = 0;`
    (`src/model/arbc/model/journal.hpp:21-25`), registered via
    `Journal::set_state_cost_fn` (`journal.hpp:64`), already summed into the
    trim budget (`journal.hpp:95-98`, `entry_cost`). Unregistered ⇒ a content
    handle contributes 0 (`journal.hpp:18-20`).
  - `RestoreSink` — `virtual void on_restore(ObjectId content, StateHandle
    target) = 0;` (`journal.hpp:34-38`), registered via
    `Journal::set_restore_sink` (`journal.hpp:65`), already fired once per
    `ContentStateEdit` on every `undo()`/`redo()` navigation
    (`journal.hpp:72-79`). Unregistered ⇒ no-op. **Explicitly off the render
    correctness path** (`journal.hpp:27-33`): the rebound `ObjectRecord`
    already carries the target `StateHandle`, so render workers reading the
    pinned handle are correct with zero `Editable` calls; restore exists only
    so the live editor's next `capture()` builds on the restored base.
  - The journal refinement records that these seams **freeze the L2/L3
    boundary for `model.editable_facet`** — this task treats them as stable
    API, not provisional (`tasks/refinements/model/journal.md`, Decisions).
- `model.transactions` (`25e4b41`). Provides the opaque capture entry point
  `Transaction::set_content_state(ObjectId, StateHandle after)`
  (`src/model/arbc/model/model.hpp:158`) — reads the current handle off the
  `ContentRecord` as *before*, writes the caller-supplied *after*, never
  touching `Editable`. Provides `ContentStateEdit { object; StateHandle
  before; StateHandle after; }` (`src/model/arbc/model/journal_entry.hpp:33`)
  and `coalesce_entries` (first-before / last-after, `journal_entry.hpp:55`).
- `model.persistent_state` (`faad68d`). Defines `StateHandle { SlotIndex
  slot }` (`src/model/arbc/model/records.hpp:38`, sentinel `k_state_none` at
  `:29`) as a fixed-size, index-only, **inert** slab handle
  (`records.hpp:27-37`), embedded in `ContentRecord.state` (`records.hpp:49`);
  the atomic-publish + pin substrate (`DocRoot`, `Model::current()`,
  `Model::drain()`, `live_slots()`); and the zero-count / deferred-reclamation
  reclaim path that drops superseded record slots on `drain()`.

**Pending:** none. All predecessors are landed.

## What this task is

Make the editable-content plumbing real at L2. Three things:

1. **StateHandle reference-lifecycle.** Today `StateHandle` is inert — never
   populated, never retained, never released (`records.hpp:27-37`). This task
   introduces one more model-defined seam, `StateRefSink` (`retain`/`release`,
   type-erased), and drives it from exactly two places: retain a non-`none`
   `StateHandle` when the content-arm `ObjectRecord` slot that embeds it is
   created, and release it when that slot is reclaimed. That single hook makes
   doc 14's two promises real — "a pinned version pins content state too"
   (doc 14:133-136) and "version GC releases … unreferenced state handles by
   refcount" (doc 14:173-176) — while every record stays trivially
   destructible and mmap-resident (`records.hpp:12-19`).

2. **The pinned-version resolver for the purity path.** Expose
   `DocRoot::content_state(ObjectId) const -> StateHandle` — a lock-free
   `peek` that touches no refcount page — so a pinned `DocState` resolves an
   object's captured handle as of that version. This is the L2 half of the
   render-purity refinement (doc 14:155-161); the L3 half — placing the handle
   on `RenderRequest` — is `contract.snapshot_pins` (below), which consumes
   this accessor.

3. **Registration + wiring** of the concrete editable seams onto the model:
   `Model::set_state_ref_sink(StateRefSink*)` (lifecycle rides the record
   store the `Model` owns) alongside the existing `Journal::set_state_cost_fn`
   / `set_restore_sink`. Plus the claims and behavioral-counter tests that pin
   the lifecycle.

It does **not** declare the `Editable` vtable, does **not** implement
`capture()`, and does **not** add a field to `RenderRequest` — all three are
L3 `arbc::contract` work (see Decisions and Constraints).

## Why it needs to be done

Content *internals* (pixels, bezier trees, child compositions) are kind-owned
but must ride the same history and the same snapshots as core placement
(doc 14:103-108). The journal already stores `(before, after)` `StateHandle`
pairs and already knows how to cost and restore them — but the handles it
stores are inert, so a pinned export, an undo, or a slow autosave does **not**
actually keep a superseded tile table alive, and nothing reclaims a
superseded one either. This task closes that gap: it is the last piece before
any editable kind (the `org.arbc.raster` reference tile table, `kinds` stream)
can be journaled correctly, and it is the model-side dependency the whole
`contract` component waits on (`tasks/25-contract.tji:6-7`,
`depends model.editable_facet`). Downstream consumers:

- `contract.snapshot_pins` (`tasks/25-contract.tji:14-17`) reads
  `DocRoot::content_state` to put the pinned handle on `RenderRequest`, making
  render a pure function of `(state, region, scale, time)`.
- `contract.conformance_suite` (`tasks/25-contract.tji:39-43`) is the public
  `arbc-testing` property suite that exercises capture/restore round-trips and
  damage honesty against a real `Editable` — the L3 counterpart of this task's
  L2 claims.
- The `kinds` stream's `org.arbc.raster` implements `Editable` and registers
  the concrete `StateRefSink`/`StateCostFn`/`RestoreSink` (via runtime
  binding); until then the seams are unregistered and inert, exactly as today.

## Inputs / context

Design docs (normative, doc 16):

- **doc 14 § "Content state: the `Editable` facet"** (`14-data-model-and-editing.md:103-161`).
  The facet contract (`:110-123`): `capture()` "must be O(small) — cheap
  enough to call once per gesture"; `restore()` "Adopt a prior state
  (undo/redo path); emit damage for what changed"; `state_cost()` "Journal
  memory budgeting." The capture discipline (`:125-136`): "capture-once-per-
  entry, mutate, damage"; "The transaction stores (before, after) state handle
  pairs in the journal entry; the published `DocState` holds the *after*
  handle, so a pinned version pins content state too — render workers see
  frozen pixels while the user keeps painting." The **purity refinement**
  (`:155-161`): "for editable content, the pinned state travels with the
  request — `RenderRequest`'s snapshot resolves to the content's `StateHandle`,
  and `render()` must render *that* state … revision identifies state, state is
  immutable, so a cached tile can never show pixels newer than its key."
- **doc 14 § History** (`:163-183`): "Budgeted by bytes via `state_cost` (+
  record sizes) … version GC releases unpinned `DocState` nodes and
  unreferenced state handles by refcount. A pinned old version (a slow export)
  keeps only what it references alive." — the refcount-GC this task
  implements.
- **doc 03** (`03-layer-plugin-interface.md:89`): `virtual Editable*
  editable() { return nullptr; }` (null-default facet, "Live content omits").
  § Parameters and editing (`:134-140`): "mutation follows the transactional
  discipline of doc 14 … The render contract itself only needs mutation to be
  *visible* (damage + revision) and rendering to be pure over the pinned
  state."
- **doc 17** (`17-internal-components.md:52-53`): `arbc::model` is **L2**
  ("object records, persistent `DocState`, transactions, journal/undo, damage,
  revisions, pins"), depends only on `base`, `pool`. `arbc::contract` is
  **L3** and *contains* "`Content` + `AudioFacet` + `Editable`, requests/
  results, … damage sinks". The rationale (`:66-72`): "**`contract` sits above
  `model`** because requests carry snapshot pins and `Editable` trades in
  journal-visible state handles. The model stays free of the `Content` vtable
  (records hold opaque content slots …)." — the hard levelization line this
  refinement is built around.
- **doc 16 § Test taxonomy** (`16-sdlc-and-quality.md:31-62`): contract
  conformance suite (tier 1, capture/restore round-trips), behavioral-counter
  tests (tier 4, "Wall-clock tests lie in CI; counters don't"). Diff coverage
  ≥90% (`:112-118`).

Source seams:

- `src/model/arbc/model/records.hpp:38` (`StateHandle`), `:29` (`k_state_none`),
  `:49` (`ContentRecord.state`), `:12-19` + `:99-113` (records are
  trivially-destructible, standard-layout, pointer-free, mmap-resident —
  the invariant that forbids releasing a handle from a record destructor).
- `src/model/arbc/model/journal.hpp:21-25,34-38,64-65,95-98` (`StateCostFn`,
  `RestoreSink`, their registrars, `entry_cost`).
- `src/model/arbc/model/model.hpp:91` (`current()`), `:110-111`
  (`set_commit_sink`/`set_damage_sink` — the pattern `set_state_ref_sink`
  follows), `:130` (`navigate`), `:158` (`Transaction::set_content_state`).
- `src/model/arbc/model/journal_entry.hpp:33` (`ContentStateEdit`), `:55`
  (`coalesce_entries`).
- `src/contract/arbc/contract/content.hpp:20` (`RenderRequest` — no
  `StateHandle` field yet), `:43` (`render`). **L3 — not edited by this task.**

Predecessor decisions carried forward (from `tasks/refinements/model/*.md`):
model-defines-a-pure-virtual-single-instance-writer-owned-sink, L3 registers
concrete impl from above (`CommitSink`/`DamageSink`/`StateCostFn`/`RestoreSink`
precedent); unregistered ⇒ inert; single-writer for all structural mutation;
deferred reclamation never inline (`drain()` runs the cascade); behavioral
counters (`live_slots()`, sink call-counts, `revision()` deltas) over
wall-clock; concurrency smoke runs the asan lane only (no in-tree TSan
preset); the seeded schedule-perturbation stress lives in
`quality.stress_harness`, never duplicated.

## Constraints / requirements

- **Levelization (CI-gated, `scripts/check_levels.py`).** `arbc::model` (L2)
  may include only `base` and `pool`. This task **must not** name `Editable`,
  `Content`, or `RenderRequest`, and must not add an edge to `arbc::contract`.
  Every reach toward editable content goes through a pure-virtual seam L3
  registers into. Declaring `Editable`, adding the `RenderRequest` state field,
  and the public conformance suite are L3 (`arbc::contract`) work.
- **Records stay trivially destructible + mmap-resident** (`records.hpp:12-19`,
  static_asserts `:99-113`). The `StateHandle`'s refcount **must not** be
  released from `~ContentRecord`/`~ObjectRecord` — that would make the record
  non-trivial and break workspace-file residency. Lifecycle is driven
  explicitly by the writer at the record store's create + zero-count-reclaim
  boundary, mirroring how a HAMT leaf already owns its `ObjectRecord` edge's
  count while the record itself is trivial.
- **One retain per distinct content-`ObjectRecord` instance, one matching
  release.** The `ObjectRecord`'s own refcount already handles multi-version
  structural sharing (an untouched content object shares one slot across
  versions); a `StateHandle` is retained once when its embedding record slot is
  created and released once when that slot is reclaimed. No per-version and no
  per-journal-entry retain.
- **Abort-, coalesce-, and undo/redo-safe by construction.** Because lifecycle
  rides record-slot create/reclaim: an aborted transaction's working records
  reclaim on `drain()` and release their handles; a coalesced gesture's
  intermediate `ObjectRecord`s reclaim when their superseded versions drain;
  navigation (undo/redo) reuses stored owning `Ref` edges and creates no new
  content records, so it needs no `StateHandle` retain of its own.
- **Single-writer.** `set_state_ref_sink`, retain, and release are all
  writer-thread-only (doc 14:71). The resolver `content_state` is a `peek`
  usable from any reader on a pinned version, touching no refcount page (claim
  `15-memory-model#const-ref-traversal-touches-no-refcount-page`).
- **Unregistered ⇒ inert.** With no `StateRefSink` registered, retain/release
  are no-ops and behavior is byte-identical to today (pure-graph Kdenlive-half
  edits — "no content involvement" doc 14:200-203 — pay nothing).
- **Coverage:** ≥90% diff coverage; `scripts/gate` green (build + asan +
  `check_levels.py` + `check_claims.py`) before commit (user rule: always
  build and test before committing).

## Acceptance criteria

Claims-register growth (`tests/claims/registry.tsv`, each with an
`// enforces: <id>` test in `src/model/t/editable_facet.t.cpp`; gated both
directions by `scripts/check_claims.py`):

- **`14-data-model-and-editing#pin-holds-content-state`** — A pinned `DocState`
  keeps every content object's captured `StateHandle` live and resolvable: a
  later `set_content_state` + publish on the same object leaves the pinned
  version's `content_state(id)` unchanged and does not release the pinned
  handle; only dropping the last version and journal reference (then `drain()`)
  releases it. Realizes doc 14:133-136,173-176. Witnessed by a recording
  `StateRefSink` (retain/release counts) + `content_state` reads across two
  pinned versions.
- **`14-data-model-and-editing#content-state-reclaimed-by-refcount`** —
  Publishing a `ContentRecord` embedding a non-`none` `StateHandle` retains it
  through the `StateRefSink` exactly once; reclaiming that record slot
  (zero-count, after `drain()`) releases it exactly once; an untouched content
  object shared across versions incurs no additional retain. Witnessed by
  `StateRefSink` retain/release call-counts balanced against `live_slots()`.
- **`14-data-model-and-editing#coalesced-gesture-captures-once`** — A coalesced
  gesture (N `set_content_state` commits under one non-zero coalescing key)
  leaves only the first-before and last-after handles referenced at the tip;
  the intermediate captured handles are released after `drain()`, and one
  `undo()` fires the `RestoreSink` once per content object with the pre-gesture
  *before* handle. Realizes capture-once-per-gesture (doc 14:88-89,125-136).
  Witnessed by recording `RestoreSink` + `StateRefSink` counts.

Behavioral-counter tests (doc 16 tier 4, `src/model/t/editable_facet.t.cpp`,
Catch2): recording `StateRefSink`/`StateCostFn`/`RestoreSink` test doubles
(extending the `RecordingCommitSink`/`RecordingDamageSink` doubles at
`src/model/t/transactions.t.cpp:15-31`); assert on retain/release counts,
`Model::revision()` deltas, `RestoreSink` call-count (one per `ContentStateEdit`
per navigate), `Journal::byte_cost()` (now non-zero when a `StateCostFn` is
registered against real handles), and `Model::live_slots()` before/after
`drain()`. No wall-clock assertions.

Concurrency: extend the existing asan-lane pin/traverse-vs-writer smoke
(`src/model/t/journal.t.cpp`, itself extending `transactions.t.cpp:346`) so
the writer captures/publishes content handles while a reader repeatedly pins a
version and calls `content_state` — asserting the resolver is a stable `peek`
under concurrent publishes. No new TSan preset (parked convention); the seeded
schedule-perturbation stress belongs to `quality.stress_harness`.

Deferred (owners already WBS leaves — no new task):

- The public `arbc-testing` capture/restore round-trip + damage-honesty
  property suite is **`contract.conformance_suite`** (`tasks/25-contract.tji:39-43`,
  milestone-wired in the `contract` stream) — it needs a real `Editable`, which
  is L3. This L2 task lands only the record-lifecycle claims above.
- Placing the resolved handle onto `RenderRequest` is **`contract.snapshot_pins`**
  (`tasks/25-contract.tji:14-17`), which consumes `DocRoot::content_state`.

## Decisions

1. **The `Editable` vtable, `capture()`, and the `RenderRequest` state field
   are L3, not this task.** Doc 17:52-53,66-72 puts `Editable` in
   `arbc::contract` and keeps `arbc::model` free of the `Content` vtable; the
   model may not depend on `contract`. So `model.editable_facet` delivers the
   model-side *bridge* (seams + lifecycle + resolver) and the behavioral claims
   that pin it, not the interface itself. *Rationale:* the levelization is
   CI-enforced and load-bearing (it is what lets a plugin process map the
   workspace read-only and render from a pinned version it cannot corrupt,
   doc 03:166-168). *Rejected:* declaring `Editable` in `model` (illegal
   down-edge from L3's contents into L2; would force `model`→`contract`
   inversion), matching the frozen-seam precedent of `StateCostFn`/`RestoreSink`.

2. **StateHandle lifecycle rides the content-`ObjectRecord` slot's
   create/zero-count-reclaim boundary through a new `StateRefSink`, not a
   record destructor.** Records must stay trivially destructible and
   mmap-resident (`records.hpp:12-19`). *Rationale:* the writer already
   retains/releases `ObjectRecord` edges explicitly at path-copy/reclaim while
   the record stays trivial (the HAMT leaf owns the count); the embedded
   `StateHandle` gets the identical treatment — one retain when the slot is
   created, one release when it is reclaimed — which makes lifecycle
   automatically abort-, coalesce-, and undo/redo-safe with no per-call-site
   logic. *Rejected:* releasing in `~ContentRecord` (breaks trivial
   destructibility → breaks workspace-file residency, and the static_asserts at
   `records.hpp:99-113`); scattering retain/release across
   `set_content_state`/`navigate`/`trim` (fragile, easy to leak on the
   reclamation-cascade path).

3. **`StateRefSink` is a type-erased retain/release seam, not a typed store the
   model resolves.** `StateHandle` is a bare `SlotIndex` (`records.hpp:38-39`)
   with no store identity, and a content object's state type + destructor are
   kind-owned (L3). *Rationale:* a registered pure-virtual seam (retain,
   release) keeps `arbc::model` ignorant of the kind's store, exactly like the
   existing four sinks; L3 (the kind, via runtime binding) resolves the handle
   in its own store and adjusts the per-slot count (per-slot refcount columns
   are already type-erased, `15-memory-model#one-count-column-per-size-class`).
   *Rejected:* widening `StateHandle` to carry a store/type tag so the model
   retains directly (grows the mmapped record beyond a bare index, and forces
   `model` to enumerate every kind's state store — a `contract`/registry concern
   pulled below its level).

4. **`set_state_ref_sink` on `Model`; `set_state_cost_fn`/`set_restore_sink`
   stay on `Journal`.** Lifecycle is a `DocState`/record-store concern the
   `Model` owns (`model.hpp:110-111` is the sibling `set_commit_sink`/
   `set_damage_sink` pattern); cost and restore are journal concerns and are
   already registered there (`journal.hpp:64-65`). *Rationale:* register each
   seam where its operand lives; avoids threading a bridge object through both.

5. **No design-doc delta.** This task implements behavior doc 14:173-176 already
   promises ("version GC releases … unreferenced state handles by refcount")
   using the model-defines-sink pattern doc 17:66-72 already establishes; the
   `StateRefSink` name and its create/reclaim call sites are implementation, not
   a constitutional amendment. *Precedent:* `model.journal` introduced
   `StateCostFn`/`RestoreSink` with no delta; `model.transactions` needed a
   delta only for the genuinely new **Abort** behavior. No designed behavior
   changes here.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-05.

- `src/model/arbc/model/model.hpp` — added `StateRefSink` pure-virtual seam (retain/release), `ContentStateReclaimSink` helper, `DocRoot::content_state(ObjectId) const -> StateHandle` lock-free resolver, and `Model::set_state_ref_sink` + supporting members.
- `src/model/model.cpp` — installed the content-reclaim sink over the deferred one; implemented `DocRoot::content_state`; retained the captured `StateHandle` on new content-record creation in `set_content_state`.
- `src/model/t/editable_facet.t.cpp` — new behavioral-counter test file with recording `StateRefSink`/`RestoreSink` test doubles; verifies retain/release balance, `live_slots()` deltas, `RestoreSink` call-counts, and pin-holds-state across concurrent publishes (asan lane).
- `src/model/CMakeLists.txt` — registered the new test target.
- `tests/claims/registry.tsv` — three new claims: `14-data-model-and-editing#{pin-holds-content-state, content-state-reclaimed-by-refcount, coalesced-gesture-captures-once}`.
- `src/model/t/journal.t.cpp` — extended the asan-lane pin/traverse-vs-writer smoke to exercise `content_state` reads under concurrent publishes.
