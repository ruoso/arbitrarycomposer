# Refinement — `kinds.raster_runtime_binding`

## TaskJuggler entry

Defined in [`tasks/55-kinds.tji:26-31`](../../55-kinds.tji):

> task raster_runtime_binding "org.arbc.raster runtime sink wiring" {
>   effort 1d
>   allocate team
>   depends !raster, model.content_binding
>   note "When the runtime instantiates an org.arbc.raster content, register its
>   StateRefSink/StateCostFn/RestoreSink onto the live Model/Journal and tear down
>   on release — closing the production wiring raster's tests drive manually.
>   Docs 03/14/17. Source-of-debt: tasks/refinements/kinds/raster.md"

Milestone: `m9_release` (`tasks/99-milestones.tji:71`).

## Effort estimate

`effort 1d`, `allocate team`. The task reuses seams that already exist on
both sides (raster's concrete state behaviour; the model's single-slot sink
setters); the work is the runtime glue that joins them plus one facet
extension. The estimate holds but is on the fuller side of 1d because it
touches five components (contract, model-consumer runtime, kind-raster, and
docs) with small edits in each.

## Inherited dependencies

**Settled predecessors this task builds on (all `complete 100`):**

- `kinds.raster` — landed the concrete state behaviour: `RasterStore`'s
  version refcount (`retain_version`/`release_version`,
  `src/kind_raster/arbc/kind_raster/raster_content.hpp:180-181`), tile-table
  byte cost (`state_cost`, `:183`), and the three concrete adapter classes
  `RasterStateRefSink`/`RasterStateCostFn`/`RasterRestoreSink`
  (`raster_content.hpp:272-310`). Its header comment names this task as the
  owner of production auto-registration (`raster_content.hpp:267-269`), and
  its refinement scoped this follow-up explicitly
  (`tasks/refinements/kinds/raster.md`, Deferred follow-ups). `RasterContent`
  already implements the `Editable` facet — `editable()` returns `this`
  (`raster_content.hpp:237`), with `capture`/`restore`/`state_cost` at
  `:240-242`.
- `model.content_binding` — landed the runtime-side content instantiation
  path: `Document::add_content` mints a versioned `ContentRecord` and binds
  the `id → Content*` side-map `d_contents`
  (`src/runtime/document.cpp:7-18`, `src/runtime/arbc/runtime/document.hpp`).
  Its Decision 4 (`tasks/refinements/model/content_binding.md:187-192`)
  deliberately registered **no** state sink and left the `ContentRecord`'s
  `StateHandle` inert (`k_state_none`) — closing that gap is precisely this
  task. The model-side release plumbing already exists from
  `model.editable_facet`: `ContentStateReclaimSink::on_zero`
  (`src/model/arbc/model/model.hpp:146-162`) fires `StateRefSink::release`
  at the content record's zero-count reclaim boundary — a no-op today
  because nothing registers a `StateRefSink`.

**Pending (must not be assumed at implementation time):**

- `kinds.nested_runtime_binding` — the sibling wiring task for
  `org.arbc.nested` (`tasks/55-kinds.tji:66-71`) is **not** yet done. It
  injects services (`PullService`/`Backend`/resolver/`DocRoot`), a different
  binding shape; do not couple to it. The facet-generic binding this task
  introduces should be reusable by it, but nested's binding is out of scope.
- `runtime.editable_sink_multiplex` — the multi-editable-content follow-up
  this task defers (see Acceptance criteria). Its routing machinery must not
  be assumed present; this task ships the single-editable-content path.

## What this task is

When the runtime instantiates an `org.arbc.raster` content into a live
document, its editable state must participate in the document's history:
paint strokes journaled and undoable, undo memory budgeted, pinned versions
held alive against reclamation. Today that participation is wired **by hand
in raster's tests** (`src/kind_raster/t/raster_paint.t.cpp:242-261` constructs
the three sink objects and calls `model.set_state_ref_sink` /
`journal.set_state_cost_fn` / `journal.set_restore_sink` directly). This task
moves that wiring into production: the runtime `Document` gains a live
`Journal`, and at content-instantiation time it registers the content's
state sinks onto the live `Model`/`Journal` through the `Editable` facet,
tearing them down deterministically on release. It also closes the
`model.content_binding` gap where the `ContentRecord`'s `StateHandle` was
inert.

## Why it needs to be done

Without it, an `org.arbc.raster` content instantiated through the normal
runtime path (`Document::add_content`, and the serialize load path at
`src/runtime/document_serialize.cpp:332`) has **no journal participation at
all**: edits are not journaled, undo/redo is unavailable, `state_cost`
budgeting is never consulted, and — most seriously — `StateRefSink::release`
never fires, so the version-refcount lifecycle doc 14:173-176 promises
("version GC releases … unreferenced state handles by refcount") is dead. The
`kinds.raster` reference proof for the `Editable` facet is only demonstrated
under manual test scaffolding; this task is what makes it real in the
runtime. It is:

- The production closure of the manual wiring in
  `src/kind_raster/t/raster_paint.t.cpp:242-261`.
- The consumer of the inert-`StateHandle` seam `model.content_binding` left
  open (`tasks/refinements/model/content_binding.md:187-192`).
- A dependency of `m9_release` (`tasks/99-milestones.tji:71`).

## Inputs / context

**Governing design-doc sections (normative, doc 16):**

- `docs/design/17-internal-components.md:66-72` — **the decisive
  levelization constraint.** "`contract` sits above `model` … the model stays
  free of the `Content` vtable (records hold opaque content slots; **binding
  happens in `runtime`**)." The component table (`:52-60`) places `model` at
  L2, `contract` at L3, `kind-*` at L4, `runtime` at L5; the rule (`:41-44`)
  is CI-enforced. This binding therefore lands in `runtime`, over the
  `contract`-owned `Editable` facet — never a `kind-raster → model` edge and
  never a `runtime → kind-raster` concrete-type dependency.
- `docs/design/14-data-model-and-editing.md:133-152` — the settled
  `Editable` facet (`capture`/`restore`/`state_cost`); `:154-165` the
  edit discipline (capture-once/mutate/damage, `(before, after)` pairs in the
  journal entry, the published `DocState` holds the *after* handle);
  `:167-174` raster as the reference proof; `:173-176` the refcount GC
  promise this task activates; `:181-190` render purity (a request's snapshot
  resolves to a `StateHandle`); `:193-212` the Journal is core-owned and
  document-wide, budgeted by `state_cost`; `:244-255` the "full editing model
  in v1" decision that makes this v1 contract.
- `docs/design/03-layer-plugin-interface.md:98-99` — the `editable()` facet
  accessor (null-default opt-out); `:113-118` the **attach-wiring
  precedent**: "The core connects this on attach; content calls `damage()`
  when it changes." This task generalises that register-on-attach pattern
  from the damage sink to the three state sinks (with the teardown half made
  explicit); `:152-158` mutation through concrete types under transactional
  discipline; `:160-207` the factory/registry instantiation path (runtime
  works through `std::unique_ptr<Content>`, not concrete kinds).

**Real source seams:**

- Model sink setters (single pointer slot each; writer-thread only; null
  clears; while unregistered behaviour is byte-identical to inert handles):
  `Model::set_state_ref_sink` (`src/model/arbc/model/model.hpp:205`),
  `Journal::set_state_cost_fn` / `set_restore_sink`
  (`src/model/arbc/model/journal.hpp:64-65`).
- The sink interfaces (all in `arbc::model`, L2):
  `StateRefSink{ retain(StateHandle); release(StateHandle); }`
  (`model.hpp:130-135`),
  `StateCostFn{ cost(const StateHandle&); }` (`journal.hpp:21-25`),
  `RestoreSink{ on_restore(ObjectId content, StateHandle target); }`
  (`journal.hpp:34-38`). **Note the asymmetry:** only `on_restore` carries
  the owning `ObjectId`; `retain`/`release`/`cost` receive a bare
  `StateHandle`.
- `StateHandle` is `{ SlotIndex slot }` (`src/model/arbc/model/records.hpp:51-56`)
  — **slot indices are local to each content's `RasterStore`**
  (`raster_content.hpp:199-219`, per-store `d_versions`/`d_free`), so a bare
  handle does **not** identify its owning content. This is why multiplexing
  many editable contents onto the single-slot seams is deferred (Decisions).
- `Journal` construction: `Journal(Model&, byte_budget = k_no_budget)`
  (`journal.hpp:56`); it is itself the `CommitSink`
  (`model.set_commit_sink(&journal)`); `undo()`/`redo()` (`:78-79`),
  `byte_cost()` (`:89`).
- The content's concrete behaviour: `RasterStore::retain_version` /
  `release_version` (`raster_content.hpp:180-181`), `state_cost` (`:183`),
  and the inspection counters `version_refcount` (`:192`),
  `live_versions` (`:191`), `blobs_allocated` (`:189`).
- The manual wiring to be productionised:
  `src/kind_raster/t/raster_paint.t.cpp:242-261` (full three-sink registration
  + `restoresink.set_object(cid)` after the id is minted, `:260`);
  partial registrations at `:147`, `:187`, `:321-325`.
- The instantiation site: `Document::add_content`
  (`src/runtime/document.cpp:7-18`) — already holds both the freshly-minted
  `ObjectId` and the `Content*`, the two things binding needs. The `Document`
  today owns `Model d_model` but **no `Journal`**, and there is **no
  content-removal path** (`d_contents` is only ever inserted into).

**Out-of-scope boundaries (do not touch):**

- The `org.arbc.nested` binding (`kinds.nested_runtime_binding`).
- Multi-editable-content routing (`runtime.editable_sink_multiplex`, deferred
  below).
- Content-state serialization / the `kind` numeric↔reverse-DNS bridge
  (`runtime.document_serialize`, per
  `tasks/refinements/model/content_binding.md:244-253`).
- The `StateHandle`-level record layout: this task does **not** grow
  `StateHandle` (that option belongs to the deferred multiplex task).

## Constraints / requirements

1. **Binding lands in `runtime`, over the `contract` facet.** The
   registration code lives in the `runtime` component (the `Document`), per
   doc 17:66-72. It reaches the content only through the `contract`-owned
   `Editable` facet — no `runtime → kind-raster` link, no `dynamic_cast` to
   `RasterContent`, no `kind-raster → model` edge. The CI dependency check
   (doc 16, doc 17:41-44) must stay green.

2. **The `Document` owns a live `Journal`.** Add a `Journal` member over
   `d_model`, registered as the model's `CommitSink` at construction
   (mirroring `raster_paint.t.cpp:246-247`). This gives cost/restore
   somewhere to attach and makes runtime edits journaled/undoable. One
   journal per document (doc 14:193-195, core-owned and document-wide).

3. **Register on instantiate.** When `Document::add_content` (and the load
   path that reaches it) instantiates a content whose `editable()` is
   non-null, the runtime installs the three sinks onto the live `Model`
   (`set_state_ref_sink`) and `Journal` (`set_state_cost_fn`,
   `set_restore_sink`), with the restore sink bound to the just-minted
   `ObjectId` (the ordering the manual test captures at
   `raster_paint.t.cpp:260`). A non-editable content (`editable()==nullptr`,
   e.g. `org.arbc.tone`/`org.arbc.nested`) registers nothing and keeps the
   byte-identical inert path (doc 14:176-182).

4. **Tear down on release.** The binding is cleared (`set_*_sink(nullptr)`)
   when the bound content leaves the live document. Because no per-content
   removal API exists yet, "release" for v1 is **document teardown**:
   `~Document` must clear the three sink slots and drain **before** the sink
   objects and the model/journal destruct, so the model's final reclamation
   drain does not call through a freed sink (the RAII ordering
   `raster_paint.t.cpp` gets for free by stack-declaration order, made
   explicit and deterministic here). The teardown must be a named method the
   binding owns so a future content-removal path can reuse it per-content.

5. **Generic through the facet — one facet extension.** The `contract`
   `Editable` facet gains `retain(StateHandle)` / `release(StateHandle)`
   (completing the state-lifecycle contract alongside the existing
   `restore`/`state_cost`; the facet already trades in `StateHandle`). The
   runtime owns three small generic adapters
   (`EditableStateRefSink`/`EditableStateCostFn`/`EditableRestoreSink`)
   that wrap `Editable*` (+ owning `ObjectId` for restore) and forward to the
   facet methods. `RasterContent` implements `retain`/`release` by delegating
   to `d_store.retain_version`/`release_version`. This is the design-doc delta
   below.

6. **Retire raster's now-redundant concrete sinks.** With the runtime
   supplying generic facet-backed adapters,
   `RasterStateRefSink`/`RasterStateCostFn`/`RasterRestoreSink`
   (`raster_content.hpp:272-310`) become dead weight; remove them and migrate
   `raster_paint.t.cpp` to drive editable state through the facet (or through
   a `Document`). Keep raster's store-level unit coverage
   (`retain_version`/`release_version`/`state_cost` on `RasterStore`)
   unchanged — only the standalone sink adapters and their manual model/journal
   wiring move.

7. **Writer-thread discipline preserved.** Sink registration and
   retain/release/cost/restore fire only on the writer/drain thread
   (`model.hpp:204`, `raster_content.hpp:146-148`). Render workers reading
   pinned handles concurrently must be unaffected; the retained version's
   blobs must stay alive for any in-flight pinned read across a release
   (doc 14:181-190).

8. **No behaviour change to render/capture/restore semantics.** This is
   wiring; the raster contract conformance surface (render honesty,
   capture/restore round-trips, damage) must be byte-identical before and
   after (doc 14:167-174).

9. **Single editable content per document (v1 limitation, made loud).** The
   model/journal seams are single pointer slots and a bare `StateHandle` does
   not identify its content (Inputs), so v1 binds **one** editable content per
   document. A second editable-content bind while one is live is a defined
   precondition failure (assert/`std::terminate`-class contract violation with
   a clear message), never a silent overwrite that would misroute the first
   content's retain/release. Log/document the limit; do not silently truncate
   (doc 16). Lifting it is the deferred `runtime.editable_sink_multiplex`.

10. **Diff coverage ≥90% (doc 16 CI gate)** on the changed lines; tests ship
    with the task.

## Acceptance criteria

**Runtime binding integration test (new, `src/runtime/t/`).** A test that
builds a `Document`, `add_content`s an editable `org.arbc.raster`, edits it
(paint under transactional discipline), and asserts — **without any manual
`set_*_sink` call** — that:

- undo/redo works through the document's `Journal` (`undo()`/`redo()`
  observable via content state or a restore counter);
- `journal.byte_cost()` is non-zero after an edit and reflects the content's
  `state_cost` contribution (the cost fn was consulted) — contrast a control
  run with a non-editable content where only record sizes bound the budget;
- the pinned/base version stays resolvable across the edit (render purity,
  doc 14:181-190).

This is the literal "closing the production wiring raster's tests drive
manually" deliverable — the test asserts the runtime auto-registered.

**New claims-register entry (`tests/claims/registry.tsv` +
`enforces:`-tagged test).** Register a claim pinning the doc-14 promise this
task activates:

- `14-data-model-and-editing#editable-runtime-bound` — "the runtime
  registers an editable content's state sinks on instantiation and tears them
  down on release; a pinned version's tile blobs survive until its record is
  reclaimed, then `release` fires exactly once." Enforced by a behavioral-
  counter test (below).

**Behavioral-counter assertions (doc 16 — counters, never wall-clock):**

- Using `RasterStore::version_refcount` (`raster_content.hpp:192`) and
  `live_versions` (`:191`): a committed edit's version is retained exactly
  once by the runtime binding (refcount reaches 1 via `StateRefSink::retain`),
  and after the content's record is reclaimed on document teardown, `release`
  fires exactly once (refcount returns to 0 / the version count drops).
- `blobs_allocated` (`:189`) is unchanged by the binding itself (binding
  allocates no tile blobs) — the witness that wiring is inert w.r.t. pixel
  storage.
- A non-editable content (`org.arbc.tone`) instantiated through the same path
  registers zero sinks and leaves `journal.byte_cost()` bounded by record
  sizes only (the inert-path witness, doc 14:176-182).

**Concurrency / teardown-under-readers (TSan + the raster stress harness).**
Per doc 16's rule for concurrency-touching tasks: run the new integration
test and the existing `tests/raster_concurrency_stress.t.cpp` under TSan; add
an assertion that a pinned render read taken before a release stays valid
across the release (the retained version is not reclaimed out from under an
in-flight reader). The existing raster concurrency stress must not regress.

**Conformance (doc 16).** The raster contract conformance suite
(`tests/raster_conformance.t.cpp`) must stay byte-identical green — this task
adds no kind and changes no render/capture/restore semantics (Constraint 8).

**CI.** ≥90% diff coverage; the doc-17 levelization / include-graph check
green (Constraint 1); tidy/linters clean.

**Deferred follow-ups (closer registers each as a real WBS leaf):**

- `runtime.editable_sink_multiplex` (~2d) — lift the single-editable-content
  limit (Constraint 9) so many editable contents share the single-slot
  Model/Journal seams. Concrete work: give a `StateHandle` an owning-content
  tag **or** carry the owning `ObjectId` on the `StateRefSink`/`StateCostFn`
  seams (mirroring `RestoreSink::on_restore`), then install a per-`Document`
  multiplexing sink trio in the runtime that routes retain/release/cost by
  owner and demultiplex-migrate the single-binding install site. `depends
  kinds.raster_runtime_binding, model.editable_facet`. Milestone: `m9_release`.

## Decisions

- **Binding lives in `runtime`, generic over the `contract` `Editable`
  facet — not in `kind-raster`, not via `dynamic_cast`.** Doc 17:66-72
  mandates binding-in-runtime with the model free of the `Content` vtable, and
  the factory/registry path (doc 03:160-207) hands the runtime an abstract
  `std::unique_ptr<Content>`. Going through the facet keeps the runtime kind-
  agnostic and dlopen-safe (the `kinds.dual_build` proof loads raster as a
  shared library; virtual facet dispatch works across the boundary where a
  concrete `dynamic_cast<RasterContent*>` would be fragile).
  *Alternative rejected:* the runtime `dynamic_cast`s the `Content*` to
  `RasterContent` and installs its concrete
  `RasterStateRefSink`/`…CostFn`/`…RestoreSink` directly (which the
  source-of-debt refinement's prose literally suggested). It couples `runtime`
  to every concrete editable kind (a per-kind branch, a `kind-raster`
  include), and breaks under the dlopen plugin path — contradicting the
  registry/facet philosophy of doc 03.

- **Complete the `Editable` facet with `retain`/`release` (design-doc
  delta), rather than exposing raw model-sink pointers.** The facet already
  owns `restore` and `state_cost` (the analogues of `RestoreSink` and
  `StateCostFn`); the one lifecycle operation still missing is the
  `StateRefSink` retain/release. Adding `retain(StateHandle)` /
  `release(StateHandle)` — symmetric with the existing methods, and adding no
  new type dependency (the facet already names `StateHandle`) — lets the
  runtime own three tiny generic adapters and keeps "binding happens in
  runtime" literally true. See the doc delta below.
  *Alternative rejected:* a facet accessor returning the three concrete
  `StateRefSink*`/`StateCostFn*`/`RestoreSink*` pointers (reusing raster's
  existing adapter classes). It leaks three L2 model-sink pointer types into
  the `Editable` API and forces every editable content to own three sink
  member-objects with a `set_object` dance; the two facet methods are the
  cleaner, self-describing extension and let raster's redundant adapters be
  retired.

- **The `Document` owns the `Journal`.** The document is the natural home of
  the single, document-wide history (doc 14:193-195); the manual test already
  couples `Journal` to the same `Model` it wires the sinks onto
  (`raster_paint.t.cpp:246-247`). Folding journal ownership in is in scope
  because "the live Model/Journal" in the task note presupposes a live journal
  and cost/restore have nowhere to attach without one.
  *Alternative rejected:* a separate "runtime owns a Journal" task. There is
  no such WBS leaf (`model.journal` builds the L2 primitive, not a runtime
  instance), and splitting a two-line member addition off a 1d task is
  churn for no isolation benefit.

- **"Release" is document teardown for v1; teardown is a named, reusable
  method.** No per-content removal API exists (`d_contents` is insert-only),
  so the observable release event today is `~Document`. The binding clears the
  sink slots and drains deterministically before the sinks/model/journal
  destruct (making explicit the RAII ordering the tests rely on). Exposing the
  teardown as a method means a future content-removal path unbinds per-content
  without redesign.
  *Alternative rejected:* inventing a `Document::remove_content` now to give
  "release" a per-content trigger. Content removal/GC is a separate feature
  with its own model-transaction semantics; inventing it here would over-scope
  a 1d task and risk colliding with a future removal task.

- **Single editable content per document in v1; multiplex deferred.** A bare
  `StateHandle` (`records.hpp:51-56`) does not identify its owning content and
  the seams are single-slot, so routing N editable contents needs an
  owner-tag or an `ObjectId`-carrying seam — a model-layer change too large
  for this task and better isolated. v1 binds one and makes a second bind a
  loud precondition failure (Constraint 9). This matches today's coverage:
  every editable scene in the reference/conformance tests uses a single raster.
  *Alternative rejected:* fold the multiplexer (and the `StateHandle`/seam
  change it needs) into this task. It roughly doubles the effort, touches the
  settled L2 model seams and `model.editable_facet`'s landed call sites, and
  is cleanly separable — so it is the named `runtime.editable_sink_multiplex`
  follow-up.

## Open questions

(none — all decided.) Surfaced to the closer for the parking lot, not encoded
as WBS tasks: whether v1 needs an editable-content **removal** path at all
(its trigger and model-transaction semantics are a product/design judgment,
not agent-decidable) — the teardown method (Decision 4) is structured to
accept one if the human review says yes.

## Status

**Done** — 2026-07-10.

- `Editable` facet extended with `retain(StateHandle)`/`release(StateHandle)` in `src/contract/arbc/contract/content.hpp`
- Generic runtime adapters `EditableStateRefSink`/`EditableStateCostFn`/`EditableRestoreSink` plus `EditableBinding` class in `src/runtime/arbc/runtime/editable_binding.hpp` and `src/runtime/editable_binding.cpp`
- `Document` gains a live `Journal` member; `add_content` auto-registers sinks via `Editable` facet and embeds captured initial state in the `ContentRecord`; serialize load path suspends commit sink during reconstruction; updated in `src/runtime/{arbc/runtime/document.hpp,document.cpp,document_serialize.cpp,CMakeLists.txt}`
- `RasterStateRefSink`/`RasterStateCostFn`/`RasterRestoreSink` concrete adapters retired from `src/kind_raster/arbc/kind_raster/raster_content.hpp`; `RasterContent` implements new `retain`/`release` via `d_store`; raster tests migrated from manual sink wiring to facet-backed adapters in `src/kind_raster/t/{raster_paint.t.cpp,raster_pool_backing.t.cpp}`
- 6 kind-agnostic unit tests in `src/runtime/t/editable_binding.t.cpp`; 4 counter+claim integration tests in `tests/raster_runtime_binding.t.cpp` enforcing new claim `14-data-model-and-editing#editable-runtime-bound`
- Design-doc deltas in `docs/design/{03-layer-plugin-interface.md,14-data-model-and-editing.md}`; `#content-add-is-versioned` claim text scoped to non-editable content
- Tech-debt registered: `runtime.editable_sink_multiplex` (lift single-editable-content limit), wired to `m9_release`
- Parking lot: editable-content removal path (trigger and model-transaction semantics are a product/design judgment)
