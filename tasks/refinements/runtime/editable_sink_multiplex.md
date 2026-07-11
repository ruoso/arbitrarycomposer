# runtime.editable_sink_multiplex — Multiplex many editable contents onto the single-slot Model/Journal seams

## TaskJuggler entry

[`tasks/65-runtime.tji:102-107`](../../65-runtime.tji) — `runtime.editable_sink_multiplex`, milestone `m9_release` ([`tasks/99-milestones.tji:71`](../../99-milestones.tji)).

> "Lift the single-editable-content-per-document limit (Constraint 9 of
> kinds.raster_runtime_binding): tag StateHandle with its owning ObjectId or
> carry ObjectId on the StateRefSink/StateCostFn seams (mirroring
> RestoreSink::on_restore), then install a per-Document multiplexing sink trio
> that routes retain/release/cost by owner. Source-of-debt:
> tasks/refinements/kinds/raster_runtime_binding.md. Docs 03/14/17."

## Effort estimate

**2d.** The seam widening is mechanical (three signatures, three call sites, all
of which already hold the owner). The work is in the runtime multiplexer, the
teardown/drain ordering, and the test matrix that proves a release for content A
can never reach content B.

## Inherited dependencies

**Settled:**

- `kinds.raster_runtime_binding` (Done 2026-07-10) — landed `EditableBinding`
  (`src/runtime/arbc/runtime/editable_binding.hpp:93-138`), the three
  facet-backed adapters, and the one-editable-per-document limit this task
  lifts. Its Constraint 9 and Decision 5 scope exactly this work.
- `model.editable_facet` (Done 2026-07-05) — landed the `StateRefSink`
  retain/release seam, `Model::set_state_ref_sink`, and
  `DocRoot::content_state(ObjectId)`. Its Decision 3 **rejected widening
  `StateHandle` to carry a store/type tag**; that rejection survives here and
  picks this task's design (Decision 1).

**Pending:** none. Every seam this task touches exists and is marked complete.

## What this task is

Today a `Document` binds exactly one editable content. A second `bind()` throws
`std::logic_error` (`src/runtime/editable_binding.cpp:18-28`) rather than
silently misrouting the first content's state. The cause is a type asymmetry:
`RestoreSink::on_restore(ObjectId content, StateHandle target)`
(`src/model/arbc/model/journal.hpp:37`) names the content that owns the handle,
but `StateRefSink::retain/release(StateHandle)`
(`src/model/arbc/model/model.hpp:133-134`) and
`StateCostFn::cost(const StateHandle&)` (`journal.hpp:24`) do not — and a
`StateHandle` is a bare `SlotIndex` (`src/model/arbc/model/records.hpp:51-56`)
whose slots are local to each content's own store. Two rasters both hold slot 3
and the handles are indistinguishable.

This task adds the owning `ObjectId` to the two owner-blind seams, bringing them
into line with `RestoreSink`, and turns `EditableBinding` from a single-slot
holder into a per-`Document` multiplexer: one sink trio for the whole document,
holding an `ObjectId → Editable*` routing table, dispatching every
retain/release/cost/restore to the content that owns the handle. A document may
then hold any number of editable contents on the one document-wide journal.

## Why it needs to be done

The single-editable limit is not a corner case — it is "one raster per
document", which no real composition survives. Every downstream editing story
(multi-layer raster documents, a vector layer over a raster layer, the
serialize round-trip of any scene with two editable layers) is blocked behind
it, and the limit is enforced by a throw, so those scenes don't degrade — they
fail loudly at load.

The failure mode the routing prevents is worse than an accounting slip. Since
`kinds.raster_pool_backing`, a raster `StateHandle` transitively owns its tile
blobs in a per-content `BigBlockPool`: `Editable::release(StateHandle)` →
`RasterStore::release_version` → a cascade of `BigBlockPool::release`. A handle
released against the wrong content **frees the wrong content's pixels**. That is
why the multiplex is a correctness feature with a hard test obligation, not a
capacity bump.

## Inputs / context

**Governing design-doc sections (normative, doc 16):**

- **Doc 14 § Content state: the `Editable` facet** — the facet (14:139-159), the
  `(before, after)` journal pairing (14:170-173), and § Runtime binding of the
  facet (14:200-215), which this task amends (see Constraint 7 / the delta).
- **Doc 14 § Identity** (14:75-84) — "Every object carries a stable `ObjectId`
  (64-bit, document-unique, assigned at creation) — the address used by the
  journal, by editor selection, by damage records". The routing key is the one
  the document already mints.
- **Doc 14 § History** (14:217-237) — journal entry shape carries "per-content
  (before, after) state handle pairs" (14:221-222): the journal is *already*
  per-content; only two of the four seams lost the owner on the way down.
- **Doc 15 § Version reclamation** (15:117-145) — "Cascades are deferred, never
  inline"; release enqueues, a housekeeping pass drains. The multiplexed release
  still runs on the single drain thread.
- **Doc 15 § What this asks of doc 14 and the kinds** (15:258-260) — "record
  types are standard-layout, fixed-size, and their references are slab refs".
  This is what forbids growing `StateHandle` (Decision 1).
- **Doc 17 § The component graph** (17:41-44 levelization rule; 17:46-61 table;
  17:66-72 "the model stays free of the `Content` vtable; binding happens in
  `runtime`") — the multiplexer must live in `arbc::runtime` (L5); the seam
  signature change is `arbc::model` (L2) and legal because `ObjectId` is L0
  `base`.
- **Doc 03 § Sketch** (03:113-118) — "the runtime registers them onto the live
  `Model`/`Journal` on instantiate and clears them on release". Unchanged by this
  task; the registration just stops being one-shot.

**Real source seams:**

- `src/runtime/arbc/runtime/editable_binding.hpp:28-37` `EditableStateRefSink`
  (holds only an `Editable*`, :36), `:41-51` `EditableStateCostFn` (same, :50),
  `:57-71` `EditableRestoreSink` (**already owner-keyed**: `ObjectId d_owner`
  :70, filter `content == d_owner` :63). `:93-138` `EditableBinding` —
  `attach(Model&, Journal&)` :103, `bind(ObjectId, Content&)` :116, `unbind()`
  :124, `bound()` :126, three `std::optional<...>` sink slots :131-137.
- `src/runtime/editable_binding.cpp:13-39` `bind` — facet probe :14, inert
  `nullptr` return :16, **the limit at :18-28** (throws `std::logic_error`),
  install :30-38. `:41-59` `unbind` — note `d_model->drain()` at :49 runs
  *before* the sink slots are nulled.
- `src/model/arbc/model/model.hpp:130-135` `StateRefSink`; `:146-162`
  `ContentStateReclaimSink::on_zero` — peeks the record at :152 and releases at
  :154; `:205` `set_state_ref_sink`; `:443`/`:449` storage.
- `src/model/arbc/model/journal.hpp:21-25` `StateCostFn`; `:34-38` `RestoreSink`;
  `:64-65` the two setters.
- **The three call sites the seam change touches — the owner is in hand at every
  one, which is the whole reason this design is cheap:**
  1. `retain` — `src/model/model.cpp:1243-1245`, inside
     `Model::Transaction::set_content_state(ObjectId content, StateHandle after)`.
     The owner is the function's own first parameter.
  2. `release` — `src/model/arbc/model/model.hpp:154`, inside
     `ContentStateReclaimSink::on_zero`. The `ObjectRecord* r` is dereferenced
     one line above (:152) and carries `r->id` (assigned at `model.cpp:633`).
  3. `cost` — `src/model/journal.cpp:21-24`, inside `Journal::entry_cost`,
     iterating `ContentStateEdit`s whose first member is
     `ObjectId object` (`src/model/arbc/model/journal_entry.hpp:33-34`).
- `src/runtime/document.cpp:16-50` `Document::add_content` — mints the `ObjectId`
  at :24, binds at :34 inside the open transaction, captures the initial state at
  :41-44. `src/runtime/arbc/runtime/document.hpp:125-149` — teardown is
  **declaration order** (no `~Document` body): journal, model, binding, contents.
  `document.hpp:43-45` documents the "throws on a SECOND editable content"
  clause that this task deletes.

**Predecessor decisions carried forward:**

- Binding reaches content **only through the `contract` `Editable` facet** —
  never a `dynamic_cast` to a concrete kind
  (`raster_runtime_binding.md` Decision 1). Virtual facet dispatch is the
  *sanctioned* dlopen mechanism (doc 03 § Plugin mechanism, 03:163-189: v1 is
  same-toolchain C++ with virtuals; the stable C ABI is Stage 2); the forbidden
  thing is the concrete-type cast, which breaks across a shared-library
  boundary.
- `unbind()` was deliberately built as "a named method the binding owns so a
  future content-removal path can reuse it per-content"
  (`raster_runtime_binding.md` Constraint 4). This task is that reuse.

## Constraints / requirements

1. **Widen the two owner-blind seams; do not widen `StateHandle`.**
   `StateRefSink::retain/release` take `(ObjectId content, StateHandle handle)`
   and `StateCostFn::cost` takes `(ObjectId content, const StateHandle& handle)`,
   matching `RestoreSink::on_restore(ObjectId, StateHandle)`
   (`journal.hpp:37`). `StateHandle` and every record type stay byte-identical —
   the `static_assert`s pinning records standard-layout / trivially-destructible
   / fixed-size (`records.hpp:99-113`, doc 15:258-260) must hold unchanged, and
   the mmapped record layout must not grow. Rationale in Decision 1.

2. **One sink trio per `Document`, routing by owner.** `EditableBinding` keeps a
   single `EditableStateRefSink` / `EditableStateCostFn` / `EditableRestoreSink`
   registered for the document's whole life, each holding a reference to the
   binding's `ObjectId → Editable*` table rather than a bare `Editable*`. The
   three model/journal setters are called **once at attach**, not per content.
   `bind(ObjectId, Content&)` becomes a table insert; the `std::logic_error` at
   `editable_binding.cpp:18-28` and the `document.hpp:43-45` contract clause are
   deleted.

3. **The `Editable` facet does not change.** It keeps trading in bare
   `StateHandle`s (`content.hpp:451-465`, doc 14:139-159): by the time a call
   reaches a content, routing has already selected that content. No `contract`
   (L3) change, no kind change, no `RasterContent` change — the blast radius is
   `model` (L2, three signatures) plus `runtime` (L5, the multiplexer).

4. **An unroutable state call is a defect, never a silent no-op.** A
   retain/release/cost/restore naming an `ObjectId` with no row in the table
   means a content's state outlived its binding — the pixel-freeing failure mode
   from § Why. It must be counted on a behavioral counter the tests assert is
   zero (Acceptance 3), and must not be absorbed silently. It must **not**
   throw: `on_zero` runs on the drain thread (`model.hpp:151`) where an escaping
   exception would tear down reclamation. Correct construction makes the counter
   unreachable — Constraint 5 is what guarantees it.

5. **Per-content unbind drains before it drops the row.** `unbind(ObjectId)`
   must `d_model->drain()` (the ordering `editable_binding.cpp:49` already gets
   right document-wide) so every pending reclaim for that content flushes
   *through the still-installed row* before the row disappears. Dropping the row
   first would strand the content's queued releases and leak its pool blocks.
   Keep a document-wide `unbind_all()` for teardown; `~Document`'s
   declaration-order contract (`document.hpp:125-149`) still holds, with the
   binding outliving the model.

6. **Writer/drain-thread discipline preserved.** Table insert/erase, and every
   retain/release/cost/restore dispatch, happen on the writer or drain thread
   only (`model.hpp:204`, doc 14:152-156, doc 15:117-145). The table is plain
   per-`Document` state — **never a static or global registry** (doc 15:73-76,
   doc 17:96-101). Render workers reading pinned handles via
   `DocRoot::content_state(ObjectId)` are untouched and must stay lock-free.

7. **Design-doc delta (doc 14).** Doc 14:212-215 currently states the one-per-
   document limit as *design*, not as debt, so lifting it is a doc amendment,
   landing in the closer's commit (doc 16's same-commit rule). Done: doc 14
   § Runtime binding of the facet now carries "Every state seam names its owner"
   with the widened seam signatures and the per-`Document` multiplexer. **No doc
   00 decision-record bullet** — doc 14 already anticipated the lift ("a later
   refinement"), so this fulfils a planned amendment rather than setting new
   project-shaping direction. No doc 03 delta: 03:113-118 ("registers on
   instantiate, clears on release") stays true verbatim.

8. **Levelization holds.** `scripts/check_levels.py` stays green. The
   multiplexer is `arbc::runtime` (L5) — the only component that owns the
   `Document` and may name both the L2 model seams and the L3 `Editable` facet
   (doc 17:41-44, 17:66-72). The seam change is confined to L2 `model` and
   introduces no new dependency: `ObjectId` is L0 `base`, already reachable, and
   already on `RestoreSink`.

9. **Diff coverage ≥90%** (doc 16 CI gate) on the changed lines. Tests ship with
   the task.

## Acceptance criteria

1. **Claims-register growth** (`tests/claims/registry.tsv`, `// enforces:`
   tagged tests):
   - **New:** `14-data-model-and-editing#editable-sinks-route-by-owner` — a
     document holding N editable contents dispatches each retain, release, cost
     and restore to the content that owns the handle, and to no other. Test: a
     new `src/runtime/t/editable_sink_multiplex.t.cpp` driving three
     `FakeEditable`s (the counter double at
     `src/runtime/t/editable_binding.t.cpp:43-100`) in one `Document`, editing
     each, asserting per-content counters move only for the edited content.
   - **New:** `14-data-model-and-editing#state-release-never-crosses-contents` —
     the pixel-freeing failure mode, pinned at the kind level: two `RasterContent`s
     in one `Document` sharing colliding slot indices (both at slot 0, then both
     at slot 1 — construct by interleaving paints), where releasing one content's
     version leaves the other's tile blobs intact and resolvable. This is the
     test that would have caught a naive multiplex; it belongs in
     `tests/raster_runtime_binding.t.cpp` alongside the existing teardown test
     (:165).
   - **Amended:** `14-data-model-and-editing#editable-runtime-bound`
     (`registry.tsv:137`) — its text still asserts the one-content binding and
     the inert path. Extend it to N contents; keep the inert-path clause (a
     non-editable content still registers nothing:
     `editable_binding.t.cpp:187`, `raster_runtime_binding.t.cpp:212`).

2. **The existing binding tests keep passing, re-pointed.**
   `src/runtime/t/editable_binding.t.cpp:211` ("a second editable content in one
   document is a loud precondition failure") **inverts**: a second editable
   content now binds cleanly, and the test becomes the N-content happy path.
   `:224` (teardown releases exactly once per retain) and `:251` (unbind frees
   the seam) must hold **per content**: `releases == retains` for every content
   independently. `src/model/t/editable_facet.t.cpp` and
   `src/kind_raster/t/raster_paint.t.cpp` must pass unchanged in behavior —
   their `#pin-holds-content-state`, `#content-state-reclaimed-by-refcount` and
   `#coalesced-gesture-captures-once` claims are the landed call sites this must
   not break (they take a mechanical signature update at the fake-sink
   definitions only).

3. **Behavioral-counter assertions** (never wall-clock):
   - **Unrouted state calls == 0** across the whole suite (Constraint 4) — the
     counter is the assertion, exposed off the binding for test read.
   - **Per-content retain/release balance**: for each content, releases equal
     retains at document teardown, and the model's reclamation queue drains
     empty. With N contents this is the only thing standing between a correct
     multiplex and a silent leak.
   - **Registration count**: the three model/journal setters are called exactly
     once per `Document` (at attach) regardless of N — the proof that the trio
     is per-document, not per-content (Constraint 2).

4. **Byte-exact goldens.** A two-editable-raster scene renders byte-identical to
   the same scene composed today as two documents' worth of pixels — the
   multiplex must not perturb rendering. Tolerances are not acceptable here
   (doc 16); the render path is unchanged, so any diff is a bug.

5. **Contract conformance.** `org.arbc.raster` re-runs the contract conformance
   suite (render honesty, capture/restore round-trips, damage) unchanged — the
   `Editable` facet is untouched (Constraint 3), so this is a regression gate,
   not new surface.

6. **Concurrency (doc 16: this task touches the model publish/pin path).** Extend
   the concurrent pin+peek stress at `src/model/t/editable_facet.t.cpp:248` to
   **multiple editable contents publishing captures concurrently against pinned
   readers**, run under TSan. The specific race to pin: a render worker holding a
   pin on content A's state while the writer publishes and reclaims content B's
   — the routing table must not be read from the render thread at all
   (Constraint 6), and TSan is what proves it.

7. **Gates.** `scripts/check_levels.py` green (Constraint 8); CI diff coverage
   ≥90% on changed lines; `tj3 project.tjp 2>&1 | grep -iE "error|warning"`
   silent (the WBS gate — no pre-commit hook enforces it).

**Deferred follow-ups:** none. The one adjacent gap — that `Document` still has
no per-content removal API, so `unbind(ObjectId)` is exercised only by teardown
and tests — is already a parking-lot item from `kinds.raster_runtime_binding`
("whether v1 needs an editable-content removal path at all"). It stays parked:
it is a scope judgment, not implementable work, and this task deliberately
builds `unbind(ObjectId)` so that whenever the answer is "yes", the removal path
is a call, not a redesign.

## Decisions

1. **Carry `ObjectId` on the seams; do not tag `StateHandle` with its owner.**
   The `.tji` note offers both. The handle-tagging option is already foreclosed:
   `model.editable_facet` Decision 3 explicitly rejected "widening `StateHandle`
   to carry a store/type tag" because it "grows the mmapped record beyond a bare
   index", and doc 15:258-260 pins record types to standard-layout, fixed-size
   slab refs. Tagging would grow every `ObjectRecord` in the mmapped store to
   buy a fact the caller already knows. And it *is* already known: the owning
   `ObjectId` is in hand at all three call sites — `set_content_state`'s own
   parameter (`model.cpp:1243`), the peeked `ObjectRecord`'s `r->id`
   (`model.hpp:152`), and `ContentStateEdit::object`
   (`journal_entry.hpp:33-34`). The seam change is therefore three signatures
   and zero plumbing, it makes all four seams symmetric with the `RestoreSink`
   that always did this, and it keeps the persistent format untouched.
   *Rejected:* tagging `StateHandle`. Bigger records, a settled decision
   reversed, a format change — to encode data that was never lost, only dropped
   at the seam.

2. **One sink trio per `Document` with a routing table — not a trio per content.**
   A trio per content would need the model/journal to hold N sink pointers,
   i.e. widening `set_state_ref_sink` into a registry — pushing per-content
   knowledge into L2 and reintroducing the `Content`-vtable proximity doc
   17:66-72 forbids. Routing in L5 keeps the model's half a single type-erased
   pointer (exactly as `model.editable_facet` Decision 3 wanted) and puts the N
   in the one component that already owns N contents.
   *Rejected:* an N-slot sink registry on `Model`/`Journal`. It relocates the
   multiplexer one level too low, and the model would then need a lifetime story
   for N sinks it doesn't own.

3. **Route through the facet, never a concrete kind.** Carried unchanged from
   `raster_runtime_binding` Decision 1: the table maps `ObjectId → Editable*`,
   the abstract facet pointer the registry hands back — no `dynamic_cast`, no
   `kind-raster` include in `runtime`. Virtual facet dispatch is what works
   across the `dlopen` boundary the `kinds.dual_build` proof exercises (doc
   03:163-189); a concrete cast is what breaks there.
   *Rejected:* keying the table on concrete kind types to skip a virtual call.
   It re-couples `runtime` to every editable kind and breaks the plugin path, to
   save an indirect call on the writer thread — which is not the hot path
   (render is, and render never touches this table).

4. **Unroutable calls are counted and asserted-zero, not thrown and not
   ignored.** Throwing is unavailable at the release site: `on_zero` runs on the
   drain thread (`model.hpp:151`), where an escaping exception kills
   reclamation. Ignoring silently is what doc 16 forbids ("do not silently
   truncate") and would hide precisely the pixel-freeing bug. A counter asserted
   zero by every test gives a loud, testable signal without an unsafe unwind —
   and drain-before-drop (Constraint 5) makes the state genuinely unreachable,
   so the counter is a tripwire on a bug, not a tolerated condition.
   *Rejected:* `std::terminate` on miss. It converts a recoverable accounting
   bug into a crash on the drain thread, in a path that runs during teardown.

5. **`unbind(ObjectId)` drains first — a per-content echo of the landed
   document-wide ordering.** `editable_binding.cpp:41-59` already drains before
   nulling the sink slots, for exactly this reason at document scope
   (`raster_runtime_binding` Constraint 4). Per content, the same hazard is
   sharper: the model's reclaim queue may hold releases for a content whose row
   is about to vanish. Drain, then drop.
   *Rejected:* dropping the row and letting the unrouted-call counter absorb the
   stragglers. That normalizes the tripwire the whole design leans on
   (Decision 4), and leaks the content's pool blocks besides.

## Open questions

(none — all decided.)

## Status

**Done** — 2026-07-10.

- `src/model/arbc/model/model.hpp`, `src/model/model.cpp` — `StateRefSink::retain/release` widened to `(ObjectId, StateHandle)`; `ContentStateReclaimSink::on_zero` passes `r->id`, bringing all four seams into alignment with `RestoreSink`.
- `src/model/arbc/model/journal.hpp`, `src/model/journal.cpp` — `StateCostFn::cost` widened to `(ObjectId, const StateHandle&)`; `entry_cost` passes `ce.object`.
- `src/runtime/arbc/runtime/editable_binding.hpp`, `src/runtime/editable_binding.cpp` — new `EditableRouter` (`ObjectId → Editable*`); the three sinks route through it; `EditableBinding::attach()` installs the trio once; `bind(ObjectId, Content&)` is now a table insert; `unbind(ObjectId)` drains before dropping the row; `unbind_all()` handles teardown. `std::logic_error` guard deleted.
- `src/runtime/arbc/runtime/document.hpp`, `src/runtime/document.cpp` — one-editable contract clause deleted; `editable_binding()` accessor exposes counters for tests.
- `src/runtime/t/fake_editable.hpp` (new shared double), `src/runtime/t/editable_binding.t.cpp`, `src/runtime/t/editable_sink_multiplex.t.cpp` (new) — N-content happy-path, per-content retain/release balance, TSan concurrency stress.
- `src/runtime/CMakeLists.txt`, `tests/raster_runtime_binding.t.cpp`, `src/model/t/editable_facet.t.cpp`, `src/model/t/journal.t.cpp`, `src/kind_raster/t/raster_paint.t.cpp` — mechanical signature updates; two-raster byte-exact golden; TSan two-content pin collision.
- `tests/claims/registry.tsv`, `docs/design/14-data-model-and-editing.md` — new claims `#editable-sinks-route-by-owner` and `#state-release-never-crosses-contents`; amended `#editable-runtime-bound` to N contents; doc 14 § Runtime binding updated to reflect widened seams and per-document multiplexer.
