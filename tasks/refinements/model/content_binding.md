# Refinement — `model.content_binding`

## TaskJuggler entry

`tasks/10-model.tji:57-62` — `task content_binding "Content records in DocState"`,
under `task model` (10 — Versioned model, doc 14).

> note "Migrate the runtime Document's side-map binding into versioned content
> records (kind id + state handle), closing the walking-skeleton TODO. Docs 14/17."

## Effort estimate

**1d** (`tasks/10-model.tji:58`). The change is small and localized: rewrite one
runtime method (`Document::add_content`, `src/runtime/document.cpp:7-11`) to mint
a versioned `ContentRecord` through the already-shipped `Transaction::add_content`
seam instead of a bare `allocate_id()`, keep the id→`Content*` vtable binding in
the runtime side-map, refresh the stale walking-skeleton comment
(`src/runtime/document.hpp:71-72`), and add the claims + behavioral-counter tests.
No new component, no new seam, no CMake edge (runtime already depends on model).

## Inherited dependencies

**Settled:**

- `model.persistent_state` (`faad68d`, `tasks/10-model.tji:9-14`, the formal
  `depends !persistent_state` at `:60`). Defines the migration *target*: the
  path-copying `DocState` HAMT keyed by `ObjectId`, the `ContentRecord { kind;
  StateHandle }` record arm (`src/model/arbc/model/records.hpp:60-63`) already
  present as a stable size class (defined then so later siblings don't churn the
  layout), `DocRoot`/`Model::current()`/`pin()`, `live_slots()`, and the
  zero-count / deferred-reclamation path.

**Settled sibling context (already landed; leaned on but not a formal `depends`):**

- `model.transactions` (`25e4b41`) — provides the record-minting entry point
  `Transaction::add_content(std::uint64_t kind)`
  (`src/model/arbc/model/model.hpp:237`, impl `src/model/model.cpp:621-646`),
  which allocates an id, creates an `ObjectRecord` with `RecordKind::Content` and
  `as.content = ContentRecord{kind, StateHandle{}}`, HAMT-inserts it, touches it,
  and emits whole-object damage. This task calls that seam; it adds no new model
  code.
- `model.editable_facet` (`6a799ae`) — froze the inert-`StateHandle` semantics
  (`k_state_none`, `records.hpp:37,48-56`), the `StateRefSink` retain-on-create /
  release-on-reclaim lifecycle (`model.hpp:130-135,146-162,205`), and
  `DocRoot::content_state(ObjectId)` (`model.hpp:106`). This task mints records
  whose `StateHandle` stays **inert** (`k_state_none`); populating it is the
  downstream runtime-binding concern, unchanged by this task.

**Pending:** none. All predecessors and leaned-on siblings are landed.

## What this task is

Close the walking-skeleton shortcut in the runtime `Document` so a content object
becomes a **versioned** record in `DocState`, not just a side-map entry.

Today `Document::add_content` (`src/runtime/document.cpp:7-11`) allocates a bare
id via `d_model.allocate_id()` and stores the `Content` in a writer-thread-owned
side-map `d_contents` (`src/runtime/document.hpp:73`) — it **never creates a
`ContentRecord`**. A layer added afterward (`Document::add_layer` →
`Transaction::add_layer(content, …)`) therefore references a content `ObjectId`
that has **no record in the versioned `DocState`**; the content exists only in the
side-map. Every render/mix driver resolves it through `Document::resolve` (the
id→`Content*` lookup, `document.cpp:77-80`), so the walking skeleton renders
correctly while the model's content-record story sits unpopulated.

This task rewrites `Document::add_content` to:

1. Open a transaction and call `Transaction::add_content(kind)`, minting a real
   versioned `ContentRecord` (kind id + inert `StateHandle`) as a top-level entry
   in the `DocState` HAMT, then `commit()` — one published version, revision +1,
   exactly like every other `Document` mutator.
2. Store the `Content` in the `d_contents` side-map keyed by the **record's**
   `ObjectId` (the id `Transaction::add_content` returned), and return that id.
   The id→`Content*` vtable binding **stays in the runtime side-map** — doc 17
   keeps `arbc::model` free of the `Content` vtable ("binding happens in
   `runtime`", `17-internal-components.md:70-71`). `resolve(id)` is unchanged.
3. Carry a caller-supplied numeric `kind` (defaulted `0`) into the record's `kind`
   half; refresh the now-stale walking-skeleton comment at
   `document.hpp:71-72` to describe the settled design (the side-map is the
   deliberate, permanent home of the vtable binding, no longer a stopgap).

The `StateHandle` half stays inert (`k_state_none`) — this task does not capture
content state. It does **not** touch `arbc::model` (the record shape and minting
seam already exist), does **not** remove the `d_contents` side-map (it is the
levelization-mandated home of the vtable binding), and does **not** implement the
reverse-DNS↔numeric kind bridge or content-body serialization (owned downstream —
see Decisions and Acceptance criteria).

## Why it needs to be done

A layer that binds a content id whose record does not exist in `DocState` is a
walking-skeleton hole: the content is invisible to version pinning, to the
journal, to reclamation, and to any consumer that reads content from a pinned
snapshot rather than the side-map. Closing it makes content a first-class
versioned object — pinnable with its version, reclaimed by refcount, and present
in the `DocState` a serializer or inspector walks.

It is a load-bearing prerequisite: **six** downstream leaves declare
`depends … model.content_binding`:

- `runtime.document_serialize` (`tasks/65-runtime.tji:40-44`) implements the
  serialize `ContentSink` against exactly this seam — "loaded bodies populate
  `d_contents`, the returned id is the `ContentRecord`'s" (`reader.hpp:80-82`) —
  and supplies the writer's content-body provider from a pinned snapshot,
  including the `ContentRecord.kind uint64/reverse-DNS bridge` and pinned
  content-state serialization. This task establishes the versioned-record +
  side-map binding that task then wires to serialize.
- `runtime.host_objects` (`tasks/65-runtime.tji:31`), `operators.fade`
  (`tasks/50-operators.tji:18`), `operators.crossfade`
  (`tasks/50-operators.tji:31`), `kinds.raster` (`tasks/55-kinds.tji:29`),
  `kinds.nested` (`tasks/55-kinds.tji:68`) all build on content being a real
  versioned record.

## Inputs / context

Design docs (normative, doc 16):

- **doc 14 § "The central decision: versions, not mutations"**
  (`14-data-model-and-editing.md:50-56`): "`DocState` is a persistent
  (path-copying) map from `ObjectId` to immutable object records — composition
  records …, layer records …, and **content records (kind id + a state handle,
  below)**. The writer thread is the single mutator …; `Document` holds the
  current `DocState` in an atomic shared pointer." — the target this migration
  realizes.
- **doc 17 § levelization table + contested-placement note**
  (`17-internal-components.md:52-53,60,66-72`): `arbc::model` is **L2** (depends
  only on `base, pool, media`); `arbc::contract` is **L3** (holds `Content`,
  `Editable`); `arbc::runtime` is **L5** ("`Document` (arenas + model + registry
  + loaders) …, depends on everything below"). The hard line
  (`:68-72`): "**`contract` sits above `model`** … The model stays free of the
  `Content` vtable (records hold opaque content slots; **binding happens in
  `runtime`**), preserving 'pure data plus change notification' (doc 02)." — why
  the id→`Content*` binding stays in the runtime side-map, not in the model.
- **doc 16 § Test taxonomy** (`16-sdlc-and-quality.md`): behavioral-counter tests
  over wall-clock ("Wall-clock tests lie in CI; counters don't"); claims register
  growth for designed behavior; diff coverage ≥90%.

Source seams:

- `src/runtime/arbc/runtime/document.hpp:17-19` (class comment — the permanent
  "id-to-Content binding lives here" design), `:20-24` (`add_content` /
  `add_layer` signatures), `:66-67` (`pin()`, `resolve()`), `:70-73`
  (`Model d_model` + the walking-skeleton `d_contents` side-map with the stale
  "migrates into versioned content records …" comment this task closes).
- `src/runtime/document.cpp:7-11` (`add_content` — the bare-`allocate_id()`
  shortcut being migrated), `:75` (`pin()`), `:77-80` (`resolve()` — the id→
  `Content*` lookup, unchanged).
- `src/model/arbc/model/model.hpp:182` (`allocate_id`), `:237`
  (`Transaction::add_content(std::uint64_t)`), `:50`
  (`DocRoot::find_content(ObjectId) const -> const ContentRecord*`), `:106`
  (`DocRoot::content_state`), `:334` (`Transaction::set_content_state`, for
  the downstream state-capture path, not this task).
- `src/model/model.cpp:621-646` (`Transaction::add_content` impl — mints
  `ContentRecord{kind, StateHandle{}}`, HAMT-inserts, touches, damages).
- `src/model/arbc/model/records.hpp:37` (`k_state_none`), `:48-56`
  (inert `StateHandle`), `:58-63` (`ContentRecord` + its own walking-skeleton
  comment: "`model.content_binding` populates these from the runtime side-map"),
  `:65-69` (`LayerRecord` names content by `ObjectId` value, not owning edge).
- `src/serialize/arbc/serialize/reader.hpp:71-84` (`SunkContent`, `ContentSink`
  — the downstream `runtime.document_serialize` seam whose doc comment names this
  task: "the returned id is the `ContentRecord`'s").
- `src/runtime/export_monitor.cpp:104,115`, `src/runtime/offline.cpp:26`,
  `src/runtime/offline_sequence.cpp:69` (the `resolve`-based content resolvers
  that must keep working byte-identically after the migration).

Predecessor decisions carried forward (`tasks/refinements/model/*.md`):
model-defines-the-record-shape / runtime-owns-the-vtable-binding (doc 17:66-72);
each `Document` mutator self-commits one version; content lifetime is independent
of referencing layers (layers name content by `ObjectId` value, not owning edge);
behavioral counters (`revision()` deltas, `live_slots()`) over wall-clock;
unregistered `StateRefSink` ⇒ inert handles, byte-identical.

## Constraints / requirements

- **Levelization (CI-gated, `scripts/check_levels.py:22,35-40`).** The change is
  confined to `arbc::runtime` (L5), which already depends on `model` and
  `contract` — **no new dependency edge**. The `Content` vtable stays out of
  `arbc::model`: the id→`Content*` binding remains the runtime `d_contents`
  side-map; the model record holds only the opaque `kind` (a `std::uint64_t`) and
  the `StateHandle` (doc 17:70-71).
- **The side-map is not removed.** "Migrate the side-map binding into versioned
  content records" means the content object *gains* a versioned `ContentRecord`;
  the id→`Content*` vtable lookup that `resolve()` serves stays in `d_contents`
  (its permanent, levelization-mandated home, `document.hpp:17-19`). The
  `d_contents` key becomes the `ContentRecord`'s `ObjectId`; `resolve()` is
  unchanged.
- **`StateHandle` stays inert.** `Transaction::add_content` mints
  `ContentRecord{kind, StateHandle{}}` = `k_state_none`. This task does not
  capture or populate content state; the `StateRefSink` stays unregistered here
  (retain/release no-ops), exactly as today. Populating the handle is the
  downstream runtime-binding path (`model.editable_facet` seam + a kind's
  `capture()`), out of scope.
- **`add_content` now publishes a version.** Previously `add_content` published
  nothing (bare `allocate_id`); after migration it commits one transaction,
  bumping `revision()` by 1 and growing `live_slots()`. **Existing runtime tests
  that assert a specific `revision()` after content adds must be updated** in this
  task (e.g. `src/runtime/t/offline_sequence.t.cpp`,
  `src/runtime/t/export_monitor.t.cpp` — the `Document::add_content` callers).
- **Content lifetime is independent of layers.** The `ContentRecord` is a
  top-level HAMT entry keyed by its own `ObjectId`; `LayerRecord` names content
  by `ObjectId` value (`records.hpp:65-69`), not an owning edge. A content with no
  referencing layer stays live as a map entry — correct and intended.
- **Coverage:** ≥90% diff coverage; `scripts/gate` green (build + asan +
  `check_levels.py` + `check_claims.py`) before commit (user rule: always build
  and test before committing).

## Acceptance criteria

Claims-register growth (`tests/claims/registry.tsv`, each with an
`// enforces: <id>` test in a new `src/runtime/t/content_binding.t.cpp`; gated
both directions by `scripts/check_claims.py`):

- **`14-data-model-and-editing#content-add-is-versioned`** — `Document::add_content`
  mints a versioned `ContentRecord` embedding the caller-supplied `kind` and an
  inert (`k_state_none`) `StateHandle`, reachable in the published `DocState` via
  `pin()->find_content(id)`; it publishes exactly one new version (`revision()`
  +1) and grows `live_slots()`; the returned `ObjectId` is the record's own key.
  Realizes doc 14:50-56. Witnessed by `find_content(id)` field checks +
  `revision()`/`live_slots()` deltas.
- **`14-data-model-and-editing#content-binding-resolves-via-side-map`** — After the
  migration the id→`Content*` binding is served by the runtime side-map, not the
  model: `Document::resolve(id)` returns the same live `Content*` that was added
  (unchanged behavior), while the versioned record exposes only the opaque
  `{kind, state}` (`find_content(id)` carries no vtable pointer); a `DocState`
  pinned **before** the add does not observe the new record (pinned-version
  isolation). Realizes doc 17:66-72. Witnessed by `resolve` identity + a
  before/after pin comparison.

Behavioral-counter tests (doc 16, `src/runtime/t/content_binding.t.cpp`, Catch2):
assert `revision()` delta == 1 per `add_content`, `find_content(id)->kind` ==
supplied kind, `find_content(id)->state.has_state()` == false, `resolve(id)`
pointer identity, and a pre-add pin's `find_content(id)` == `nullptr`. No
wall-clock assertions. Extend the existing runtime driver tests
(`offline_sequence.t.cpp`, `export_monitor.t.cpp`) to keep passing under the new
revision accounting (a render of a content-bearing scene still resolves through
`resolve` and produces the same frame).

**Not applicable — contract conformance suite.** This task adds no new content
*kind* or operator (it is runtime `Document` plumbing over the existing model
seam), so the kind/operator contract conformance suite does not apply. The
kinds/operators that *do* implement `Content` run it under their own tasks
(`kinds.raster`, `operators.fade`, …, which depend on this one).

Deferred (owner already a WBS leaf — no new task):

- The **`ContentRecord.kind` uint64 ↔ reverse-DNS bridge** and pinned
  content-body/state serialization are already scoped to
  **`runtime.document_serialize`** (`tasks/65-runtime.tji:40-44`,
  `depends … model.content_binding`, milestone M8). This task carries the numeric
  `kind` as an opaque caller-supplied token (defaulted `0`) — no live consumer
  reads the numeric field in v1 (render resolves via the side-map; serialize keys
  off the reverse-DNS string via its own `ContentMetaProvider`), so bridging it is
  premature until `document_serialize` needs it. No new leaf; no design-doc delta.

## Decisions

1. **Mint the versioned record via the existing `Transaction::add_content`; keep
   the id→`Content*` binding in the runtime side-map.** Doc 17:70-71 is explicit —
   "binding happens in `runtime`", the model stays free of the `Content` vtable.
   So the migration is entirely a runtime-side rewrite: `Document::add_content`
   commits a `Transaction::add_content(kind)` (already shipped by
   `model.transactions`) and stashes the `Content*` in `d_contents` keyed by the
   record's id. *Rationale:* reuses a landed seam with zero model change; respects
   the CI-gated levelization line; `resolve()` and every render/mix driver keep
   working unchanged. *Rejected:* moving the id→`Content*` map into `arbc::model`
   (illegal L3→L2 vtable down-edge, breaks `check_levels.py`); replacing
   `resolve()` with a model lookup (the model holds no `Content*`, by design).

2. **`Document::add_content` self-commits one version.** Every other `Document`
   mutator opens its own transaction and commits (`document.cpp:13-73`);
   `add_content` joins them, publishing the content record into a pinnable
   `DocState`. *Rationale:* the record must be in a *published* version for
   `pin()` to see it and for downstream serialize/inspection to walk it;
   consistency with the sibling mutators keeps the API uniform. *Consequence:*
   existing runtime tests counting revisions are updated in this task (Constraints).
   *Rejected:* deferring publication until the following `add_layer` commit
   (couples two independent API calls, leaves a window where the content id has no
   record, and breaks add-content-without-a-layer).

3. **Carry a caller-supplied numeric `kind` (defaulted `0`); leave the
   reverse-DNS↔numeric bridge to `runtime.document_serialize`.** `ContentRecord::kind`
   is a `std::uint64_t` (`records.hpp:61`); the registry keys kinds by reverse-DNS
   string (`registry.hpp:42-44`); `Content` carries no kind id. No v1 consumer
   reads the numeric field (render uses the side-map; serialize uses the string
   via `ContentMetaProvider`). *Rationale:* a defaulted parameter is zero-blast to
   existing callers yet lets a caller carry a real kind (pinned by a test that
   round-trips a non-zero kind through `find_content`); the meaningful bridge is
   *already* a named downstream deliverable (`tasks/65-runtime.tji:44`), so
   inventing it here would duplicate scope with no consumer. *Rejected:* wiring a
   registry lookup + interning table into `add_content` now (needs `Content` to
   expose its kind id and a live consumer — neither exists in v1; speculative);
   forcing an explicit (non-defaulted) `kind` argument (churns ~12 driver-test
   call sites for no behavioral gain while the field is unread).

4. **`StateHandle` stays inert; no state capture in this task.** The minting seam
   writes `StateHandle{}` (`k_state_none`) and the `StateRefSink` stays
   unregistered, byte-identical to today. *Rationale:* content-state capture is
   the `model.editable_facet` lifecycle driven by a kind's `capture()` via runtime
   binding — a separate concern the depending kind tasks own; scoping it here would
   overreach a 1d migration. *Rejected:* capturing an initial state on add (no
   `Editable` is registered in the walking skeleton; would require an L3 capability
   this task must not assume).

5. **No design-doc delta.** The migration *realizes* behavior docs 14:50-56 and
   17:66-72 already mandate (content records live in `DocState`; the vtable
   binding lives in runtime); it introduces no new seam, no new dependency, and no
   deviation. *Precedent:* `model.editable_facet` and `model.journal` also shipped
   with no delta when implementing already-normative behavior; a delta is reserved
   for genuinely new behavior (as `model.transactions` needed for Abort).

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-09.

- `src/runtime/document.cpp` — `add_content` rewritten to open a `Transaction::add_content(kind)`, commit it, and key `d_contents` by the record's `ObjectId`; one version published per call.
- `src/runtime/arbc/runtime/document.hpp` — class/side-map comments refreshed (permanent vtable-binding home, walking-skeleton TODO closed); `kind` param defaulted to `0`.
- `src/runtime/CMakeLists.txt` — `t/content_binding.t.cpp` registered as a test target.
- `src/runtime/t/content_binding.t.cpp` (new) — behavioral-counter tests: `revision()` delta==1, `find_content(id)->kind` round-trip, `has_state()==false`, `resolve(id)` pointer identity, pre-add pin isolation (returns `nullptr`).
- `tests/claims/registry.tsv` — claims `14-data-model-and-editing#content-add-is-versioned` and `14-data-model-and-editing#content-binding-resolves-via-side-map` added.
- `tests/serialize_sharing.t.cpp` — minor include-order / formatting fixup (clang-format).
- No existing driver-test revision edits required: `offline_sequence.t.cpp`, `export_monitor.t.cpp`, `interactive.t.cpp` all sample revision relative to a post-add pin, so the extra published version does not perturb their assertions.
