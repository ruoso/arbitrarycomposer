# Refinement — `model.per_object_revision`

## TaskJuggler entry

`task per_object_revision "Per-object revision for cache-key selectivity"` —
[`tasks/10-model.tji:71-76`](../../10-model.tji), under the `model` parent.

> Stop every edit from invalidating every layer's tile cache. TODAY: a commit
> is 'exactly one revision increment' on the DOCUMENT-GLOBAL counter
> (model.cpp:1589-1591), TileKey carries that revision (cache/key_shapes.hpp:66),
> and the interactive planner hands the document-global revision to EVERY layer
> as its key [...] So ONE edit anywhere makes the cached tiles of EVERY layer in
> the document unreachable. [...] THE WORK IS THE MODEL DECISION, which this task
> must settle and must NOT presume: what a per-object version stamp IS.

The note names two candidate stamps — (a) a `uint64` field on `ObjectRecord`,
(b) the owning-edge `SlotRef` identity packed to `uint64` — and instructs the
task to settle the ABA hazard before choosing (b). It also asserts that "THE
COMPOSITOR SIDE IS ALREADY DONE". Both of those framings need correcting, and
Decisions 1 and 6 below do so: (b) is unsound, and the compositor/runtime side
has three seams the note does not mention (Decisions 4, 5, 6).

## Effort estimate

**3d** (`tasks/10-model.tji:72`).

The model half is genuinely small — one field, one write folded into an
existing copy-on-write path, one accessor. The estimate is nonetheless *tight*,
because the note's "the compositor side is already done" is true only of the
`aggregate_revision` fold itself. Three consumers currently key off a single
document-global scalar and stop being well-defined the moment contributions
become per-object: the stale-revision fallback probe
(`interactive.hpp:415`), the audio lookahead ring's warm key
(`lookahead.hpp:65-69`), and the fold's own combine operator
(`operator_graph.cpp:38`, which is an integer *sum* — safe only while every
contribution carries the same value, and demonstrably unsafe once they do not;
see Decision 3). None of the three is optional: leaving any as-is trades the
document-wide over-invalidation this task removes for a *stale-pixel* path,
which is strictly worse. Budget the 3d as roughly 0.5d model, 1d
compositor/runtime rewiring, 1.5d tests and doc deltas.

## Inherited dependencies

**Settled:**

- **`model.transactions`** — a commit is exactly one `d_current.store()` and
  exactly one revision increment (`src/model/model.cpp:1589-1591`), from a
  base revision held on the transaction (`model.hpp:501-507`). Every mutator
  already `touch()`es the mutated id into `d_touched`
  (`src/model/model.cpp:811-818`) and every mutator already *re-creates* the
  record copy-on-write — `d_records.create()`, `nr = *old`, override, `touch()`
  (`src/model/model.cpp:825-843`, `:853-870`, `:886-905`, `:922-938`, `:954-970`,
  `:986-1002`, `:1018-1034`, `:1050-1056`). This uniform COW shape is what makes
  the stamp-write site a one-liner with no new traversal. Claims
  `14-data-model-and-editing#commit-publishes-once`,
  `#abort-publishes-nothing`.
- **`model.journal`** — `Model::navigate()` (`src/model/model.cpp:1606-1644`)
  republishes each touched object's *stored owning edge by `SlotRef` identity*
  (`model.cpp:1615-1619`), never re-creating a record, and builds a `DocRoot` at
  revision + 1 (`model.cpp:1635`). This is load-bearing: a stamp stored *inside*
  the record travels back to its pre-edit value on undo for free, with no
  navigate-side code at all. Claims `#undo-is-a-forward-publish`,
  `#undo-redo-round-trips`, `#history-is-never-mutated`.
  `sizeof(ObjectRecord)` is the journal's byte-cost unit
  (`src/model/journal.cpp:14,17`, `journal.hpp:99`) — growing the record moves
  journal budget accounting (see Decision 1).
- **`model.content_binding`** — the runtime side-map binding an `ObjectId` to a
  `Content*`, and its reverse (`src/runtime/arbc/runtime/pull_identity.hpp:16-52`,
  `make_pull_identity_of` / `pull_identity_of`), memoized per pinned revision in
  the interactive driver (`src/runtime/interactive.cpp:109`, refreshed at `:326`;
  member at `runtime/interactive.hpp:448`). This is the exact seam the per-object
  contribution memo extends — Decision 4 adds a second column to it rather than a
  new mechanism. Claims `#content-add-is-versioned`,
  `#content-binding-resolves-via-side-map`.

**Settled sibling context** (leaned on, not a formal `depends`):

- **`model.persistent_state`** — path-copy shares every untouched record by
  `SlotRef` identity (`src/model/model.cpp:57-58`), live-slot growth is
  O(path depth) (claim `#commit-shares-untouched-structure`). This claim *is*
  the per-object-revision mechanism: an untouched object keeps its record, hence
  keeps its stamp, at zero extra cost. Const traversal touches no refcount page
  (claim `15-memory-model#const-ref-traversal-touches-no-refcount-page`) — the
  stamp read on the render path must preserve that (Decision 2).
- **`model.composition_membership`** — `CompositionRecord` names its layers by
  `ObjectId`, inline to `k_max_inline_layers` and spilling to a `LayerOrderChunk`
  chain (`src/model/arbc/model/records.hpp:108-112`, `:136-144`). `LayerRecord`
  carries **no back-pointer to its owning composition**
  (`records.hpp:68-92`) — the fact that forces Decision 5's shape.
- **`model.workspace_backing`** — records are index-only and position-independent
  so they can live in an mmapped workspace file; recovery walks records by raw
  storage index and must not assert `SlotRef` generations; `Model::open`
  reconstructs at revision 0 (`model.hpp:225`). Decision 2 settles the
  recovery-vs-stamp reconciliation this creates.

**Pending:** none.

## What this task is

Give every object its own revision stamp, and make the tile and block cache keys
carry *that* stamp instead of the document-global revision — so an edit to one
layer stops making every other layer's cached tiles unreachable.

Concretely: mint a stamp into each `ObjectRecord` at the copy-on-write that
already happens on every mutation; expose it on `DocRoot`; teach the runtime to
project it (folded with the arrangement of any composition the content names)
into the `PullConfig::contribution` / `LookaheadConfig` seams the compositor and
audio engine already read; and repair the three consumers that silently assumed
all contributions were equal.

## Why it needs to be done

The v0.1 image-editor framing fires this by construction. Painting is a
high-frequency commit loop — each brush dab is a transaction — and coalescing
does not help, because a coalesced commit still publishes and still bumps the
document revision (claim `#coalesced-commits-merge-to-one-entry` says so
explicitly: "while each commit still publishes a new revision"). A 60-dab stroke
therefore orphans the whole tile cache 60 times. The damage gate keeps the
*immediate* cost small — only tiles intersecting the dab are planned — but every
tile *outside* the dab is left cached under a dead key, so the first pan or zoom
after a stroke re-renders the entire viewport cold, and the store fills with dead
entries at 60 distinct revisions.

There is a sharper reason, and it is the one that makes this a correctness task
rather than an optimization. **Doc 05's caching promise is currently enforced
only against a test stub.** `tests/nested_cache.t.cpp:87` enforces
`05-recursive-composition#static-subtree-served-from-cache`, whose registered
text reads "*A static nested subtree's composed-result tile survives a clock
advance **and an unrelated edit** (zero dispatched renders)*" — but the test
supplies its own hand-written per-node contribution
(`tests/nested_cache.t.cpp:109`, `config.contribution = [&revision](const Content* c)`),
while *every shipped driver* passes the document-global revision for every node
(`src/runtime/interactive.cpp:337`, `src/runtime/offline_sequence.cpp:99`,
`src/runtime/export_monitor.cpp:121`, `src/compositor/tile_planning.cpp:401`).
The claim is true of the stub and false of the product. This task is what makes
a registered claim honest.

Downstream: doc 01:155-165 already promises exactly this cache key, and doc
05:129-144's "static children make deep hierarchies cheap: a 10-level tree
re-renders only the spine from the edited layer to the viewed root" is
unreachable without it.

## Inputs / context

### Design docs (normative — doc 16's executable-spec discipline)

- **`docs/design/01-core-concepts.md` § Identity and versioning, lines 155-165** —
  the governing text, and it is a *promise already made*: "Every content object
  and every layer instance carries a monotonically increasing **revision**,
  bumped on any change. Cache keys are `(content identity, revision, region,
  quantized scale)`. This keeps caching correct without the cache needing to
  understand what changed." Note the task's `.tji` note cites doc 05:84 for this;
  that is a misattribution — doc 05:84 is the tail of the async-external-load
  paragraph, and doc 05 speaks only of the *aggregate* revision. Doc 01 is the
  per-object source.
- **`docs/design/05-recursive-composition.md` § Caching across the nesting
  boundary, lines 129-144** — the composed result is "keyed by the child's
  *aggregate revision* (a composition-level revision bumped by **any reachable
  change**)". Doc 05 prescribes *what the fold must detect*, and pointedly not
  *what arithmetic it uses* — which is the licence for Decision 3.
- **`docs/design/14-data-model-and-editing.md` § Cross-doc impact, line 349** —
  "Doc 01's 'identity and versioning' section is realized by `ObjectId` +
  `DocState`; **revisions become version-monotonic** as before." This sentence is
  what this task must amend (Decision 2 / the doc delta): a per-object stamp is
  monotone across forward commits but is *restored*, not advanced, by undo.
  Also § Render purity, lines 221-227 — "revision identifies state, state is
  immutable, so a cached tile can never show pixels newer than its key" — the
  invariant every decision here is measured against.
- **`docs/design/15-memory-model.md` lines 227-256** — position independence, the
  workspace header's "layout schema version, per-store slot sizes", and the rule
  that "a file whose store table disagrees with the reopening build's strides
  [...] is **refused as a value** rather than silently mis-routed". Store identity
  is the (slot stride, slot alignment) pair (lines 244-248). Doc 15 pins record
  *properties* (standard-layout, fixed-size, pointer-free, trivially
  destructible — lines 141-151, 306-308), never a field list. Growing
  `ObjectRecord` is therefore a *sanctioned* change whose consequence (old
  workspace files refused) is already-designed behavior, **not a doc 15 delta**.
  The `.tji` note's claim that option (a) "carries a doc 15 delta" is incorrect.
- **`docs/design/17-internal-components.md` lines 47-62** — the levelization this
  must respect. `model` is L2 and owns "**revisions**" (line 53). `cache` is L3
  and depends on `base, surface` only — **not** on `model` (line 55), which is
  why `TileKey::revision` is a bare opaque `uint64`
  (`src/cache/arbc/cache/key_shapes.hpp:23-31`). `compositor` is L4 and owns
  "**aggregate revisions**" (line 57). Lines 80-84: "**The model stays free of the
  `Content` vtable** (records hold opaque content slots; binding happens in
  `runtime`)" — the constraint that decides *where* the composition fold lives
  (Decision 5).
- **`docs/design/16-sdlc-and-quality.md`** — claims register, behavioral counters
  over wall-clock, ≥90% diff coverage.

### Source seams this task extends (all at `28f9b59`)

Model:

- `src/model/arbc/model/records.hpp:151-164` — `ObjectRecord`: `RecordKind kind`
  (a `uint32`, `:28-33`) at offset 0, `ObjectId id` (a `uint64`, `base/ids.hpp:11-16`),
  then the `union { CompositionRecord composition; LayerRecord layer; ContentRecord
  content; LayerOrderChunk order_chunk; } as`. Static asserts at `:166-185`
  (standard-layout, trivially destructible, trivially copyable, pointer-free,
  `sizeof(LayerOrderChunk) <= sizeof(CompositionRecord)`). There is **no reserved
  slack** — but there *is* an existing 4-byte alignment hole at offset 4 between
  `kind` and `id` (see Decision 1's rejected alternative).
- `src/model/arbc/model/records.hpp:58-63` (`ContentRecord`: kind + `StateHandle`),
  `:68-92` (`LayerRecord` — names its content by `ObjectId`, **no owning-composition
  back-pointer**), `:136-144` (`CompositionRecord`: `layer_count`,
  `layers[k_max_inline_layers]`, `spill_root`), `:108-112` (`LayerOrderChunk`).
- `src/model/arbc/model/model.hpp:40-48` (`DocRoot(StoreBundle, Ref<HamtNode>,
  uint64 revision)`, `revision()`), `:114-118` (members), `:96-100`
  (`DocRoot::record_edge(ObjectId, SlotRef<ObjectRecord>&)`), `:590`
  (`std::atomic<DocStatePtr> d_current`).
- `src/model/model.cpp:1547-1602` — commit: journal entry assembled from
  `d_touched` (`:1557-1587`), then the single publish at `:1589-1591`.
- `src/model/model.cpp:1606-1644` — `navigate()`: rebinds by `SlotRef` identity
  (`:1615-1619`), publishes at revision + 1 (`:1635`), never re-creates a record,
  never notifies the `CommitSink` (`:1637-1639`).
- `src/model/model.cpp:369-408` — `record_edge`, reading via refcount-free `peek`.

Cache / compositor:

- `src/cache/arbc/cache/key_shapes.hpp:64-76` (`TileKey{ObjectId content; uint64
  revision; ScaleRung rung; TileCoord coord; optional<Time> achieved_time;}`,
  defaulted memberwise `operator==` at `:75`), `:95-103` (`BlockKey`, the 1D twin),
  `:142` (`using TileCache = KeyedStore<TileKey, TileValue>`), `:157-188` (the
  hashes). **The key shape does not change** — only the value projected into the
  `revision` slot.
- `src/compositor/arbc/compositor/operator_graph.hpp:87-107` — `aggregate_revision`
  and its `std::function<uint64(const Content*)> contribution` callback; the
  comment at `:98-101` says per-node contributions "drop in with no change here
  when the model exposes them". Combine is integer addition —
  `src/compositor/operator_graph.cpp:38`, `acc += contribution(node)` (Decision 3
  changes this).
- `src/compositor/arbc/compositor/pull_service.hpp:131-140` — `PullConfig::id_of`
  (`Content* -> ObjectId`) and `PullConfig::contribution` (`Content* -> uint64`),
  the two seams the runtime fills.
- `src/compositor/tile_planning.cpp:300-308` (`render_frame_interactive`, `const
  uint64 revision = state.revision()`), `:391-401` (the leaf/operator lambda),
  `:448` (the `TileKey` build), `:190-191` (`plan_layer`'s `prior_revision`
  parameter), `:257-261` (the stale probe), `:270-278` (the coarser-rung probe),
  `:480` (`plan_layer` call).
- `src/compositor/pull_service.cpp:179-195` (visual fold, key at `:229`),
  `:509-534` (audio fold, `BlockKey` at `:533`).

Runtime / audio:

- `src/runtime/interactive.cpp:336-337`, `src/runtime/offline_sequence.cpp:92,98-99`,
  `src/runtime/export_monitor.cpp:117,121` — the three drivers that fill
  `config.contribution` with `[revision](const Content*) { return revision; }`.
- `src/runtime/arbc/runtime/interactive.hpp:415` —
  `std::optional<uint64> d_prior_revision` ("last-completed revision (stale
  probe)"), written at `src/runtime/interactive.cpp:275` and `:514`, read at
  `:384`; accessor at `interactive.hpp:286`.
- `src/runtime/arbc/runtime/pull_identity.hpp:16-52` — `make_pull_identity_of` /
  `pull_identity_of`, and the per-revision memo at `src/runtime/interactive.cpp:109`,
  `:326` (`refresh_identity_memo`).
- `src/audio_engine/arbc/audio_engine/lookahead.hpp:65-69` — `uint64 revision{0}`,
  a **single scalar for the whole ring**, whose comment at `:67` states the
  binding constraint: "it must equal `PullConfig::contribution` for every leaf so
  the fill keys the key the pull probes". Used at
  `src/audio_engine/lookahead.cpp:326-345` (`contribution_key`) and `:374`.

Pool (the hazard that kills option (b)):

- `src/pool/arbc/pool/refs.hpp:64-88` — `SlotRef` is a bare `SlotIndex`
  (`uint32`), and **`operator==` compares the index alone** (`:70`). The
  generation tag is `#ifndef NDEBUG` (`:77-87`); `static_assert(sizeof(SlotRef<int>)
  == 4)` under `NDEBUG` (`:604-606`). `assert_generation` compiles to `{}` in
  release (`refs.hpp:569-580`).
- `src/pool/arbc/pool/slot_store.hpp:96-98` — "a released slot is a perfect hole
  reused by the next same-class allocation"; free list at `:315`, `push_free` at
  `:281`. And `:24`, decisively: "**a slot is a storage location that recycles, an
  ObjectId is a document identity**".

### Predecessor-refinement conventions

`tasks/refinements/model/transactions.md`, `journal.md`, `content_binding.md`,
`persistent_state.md`, `workspace_backing.md`, `composition_membership.md`.

## Constraints / requirements

1. **The cache key shape does not change.** `TileKey`/`BlockKey` keep their bare
   `uint64 revision` slot (`key_shapes.hpp:66,:97`). `cache` (L3) may not learn
   about model types (doc 17:55). The compositor (L4) projects the stamp into the
   slot with a plain copy, exactly as the comment at `key_shapes.hpp:23-31`
   already prescribes. This preserves claims
   `02-architecture#tile-cache-key-and-value-shape`,
   `05-recursive-composition#composed-result-invalidated-like-leaf`, and
   `12-audio#block-cache-is-tile-cache-1d` verbatim.
2. **Levelization (doc 17:47-62, CI-enforced by `scripts/check_levels.py`).**
   `model` (L2) exposes the per-object stamp primitive and the
   composition-arrangement fold (both are pure model vocabulary:
   `ObjectRecord`, `CompositionRecord::layers`/`spill_root`). `model` may **not**
   see `Content::composition_ref()` — that is the contract vtable, and doc
   17:80-84 explicitly keeps the model free of it. `compositor` (L4) keeps owning
   the `inputs()` fold. `runtime` (top) is the only place that sees both the
   `DocRoot` and the `Content` vtable, so it is where the two are joined. **No new
   levelization edge is introduced.**
3. **The render-path read must not dirty a refcount page.** Reading a stamp goes
   through the existing refcount-free `peek` path (`model.cpp:373-408`),
   preserving `15-memory-model#const-ref-traversal-touches-no-refcount-page`.
4. **Structural sharing must not be perturbed.** A commit must not write a stamp
   into any record it did not already path-copy — that would destroy
   `14-data-model-and-editing#commit-shares-untouched-structure` (O(path depth),
   not O(document)). The stamp write must ride *inside* the existing
   `nr = *old; ...; touch(id)` COW sequence, adding no traversal.
5. **Undo must restore the old stamp, not advance it.** `navigate()` republishes
   byte-identical records; a tile keyed on a restored stamp shows exactly the
   pixels that record produces, so the pre-edit tiles become legitimately
   reachable again. This is the desirable behavior (undoing a stroke should make
   the pre-stroke cache live) *and* it is what doc 14:221-227's render-purity
   invariant requires. It also means a per-object stamp is **not** globally
   monotone — the doc 14 delta.
6. **Write-side and read-side keys must stay equal.** The audio lookahead ring
   warms blocks under a key it computes itself (`lookahead.hpp:67`,
   `lookahead.cpp:326-345`). If the pull side goes per-object and the ring stays
   document-global, the ring warms keys nobody probes: every pull misses, the ring
   becomes pure waste, and `12-audio#block-key-disambiguates-spatial-context`'s
   residency clause breaks. The ring must take the *same* contribution functor.
7. **The stale-revision fallback tier must survive.** `plan_layer`'s
   `prior_revision` (`tile_planning.cpp:191,:257-261`) is fed from a single
   document-global scalar (`interactive.hpp:415`). Under per-object stamps that
   scalar names no content's prior stamp, so the stale probe would always miss and
   the degradation ladder would silently lose a tier — breaking
   `02-architecture#degraded-fallback-preference-order` and
   `#revision-bump-preserves-stale-tiles-as-fallback`.
8. **No `NDEBUG`-conditional behavior.** Anything whose correctness depends on a
   `#ifndef NDEBUG` generation tag is out (Decision 6).

## Decisions

### 1. The stamp is a `std::uint64_t revision` field on `ObjectRecord`, minted from the document revision at each copy-on-write.

Add `std::uint64_t revision{0}` to `ObjectRecord` (`records.hpp:151-164`), and
in each mutator's existing COW sequence set `nr.revision = d_base_revision + 1`
(the revision this transaction will publish, already on hand at
`model.hpp:501-507`) right beside the existing `touch(id)`.

This is option (a) of the note, and it is correct for a reason the note does not
state: **structural sharing is already exactly the semantics we want.** A commit
path-copies only the touched objects (`model.cpp:57-58`), so writing the stamp
into the fresh record costs nothing, and every untouched sibling keeps its old
record — and therefore its old stamp — by refcount, with zero writes. The stamp
needs no new traversal, no side table, and no bookkeeping in `navigate()`, which
republishes the record wholesale (`model.cpp:1615-1619`) and thus carries the old
stamp back for free (constraint 5).

Cost: `sizeof(ObjectRecord)` grows by 8 bytes. Two consequences, both benign and
both already-designed:

- The workspace store stride changes, so a workspace file written by a
  pre-change build is **refused as a value** on reopen by the existing store-table
  guard (doc 15:240-256). That is the designed behavior for exactly this class of
  change, and the workspace file is an explicitly non-portable same-machine
  scratch artifact (doc 15:232-238) beside the doc 08 JSON, which is the durable
  format. **No doc 15 delta is required** — doc 15 pins record properties, not a
  field list, and the added field keeps every one of them (standard-layout,
  fixed-size, pointer-free, trivially destructible/copyable).
- The journal's byte budget is denominated in `sizeof(ObjectRecord)`
  (`journal.cpp:14,17`), so a fixed byte budget now holds ~15% fewer entries.
  Claim `#journal-trims-to-byte-budget` is unchanged in *meaning* (it trims to a
  byte budget); only the entry count at a given budget moves. The implementer
  must check whether any journal test asserts an entry count at a hard-coded
  budget and re-derive it from `sizeof(ObjectRecord)` rather than re-typing a
  constant.

*Rejected: `std::uint32_t` in the existing 4-byte alignment hole at offset 4
(between `kind` and `id`).* This is genuinely tempting — it costs **zero** bytes,
changes no stride, refuses no workspace file, and moves no journal budget. It is
rejected because it wraps at 2^32 commits, and the wrap is a *silent wrong-pixel*
path created by this very task: a static layer's tiles are now explicitly
designed to survive unrelated edits indefinitely, so a resident tile keyed
`(X, s)` can outlive 2^32 commits elsewhere, after which X's next edit mints `s`
again and the cache serves ancient pixels. The scenario is absurd in wall-clock
terms and airtight in mechanism terms; in a project that pins byte-exact goldens
and treats tolerances as the justified exception, trading a silent
correctness landmine for 8 bytes per record is the wrong trade. 8 bytes across a
100k-object document is 800 KB.

*Rejected: a non-persisted side table (`SlotIndex -> uint64`), mirroring the
`SlotStore` refcount/generation columns (`slot_store.hpp:100-110`).* This has real
appeal — it keeps `ObjectRecord` byte-identical, so no stride change, no workspace
refusal, no journal-budget shift — and it is sound (the stamp is written at every
path-copy, so a recycled slot always carries a fresh stamp, and an old record
retained by the journal keeps its slot and hence its stamp). It is rejected on
simplicity: it adds a second structure the model must grow in lock-step with
`RefStore<ObjectRecord>`, keyed on a *storage* index rather than a document
identity — precisely the conflation `slot_store.hpp:24` warns against — to save
8 bytes on a record. One field beats one parallel column.

### 2. Recovery seeds the document revision counter above every persisted stamp; a doc 08 load does not.

Because the stamp is in the record, it persists into the workspace file — while
`Model::open` reconstructs the recovered document at **revision 0**
(`model.hpp:225`). Left alone, that is a latent wrong-pixel path: after 500
commits in the recovered session, a new commit would mint stamp 500 for object X,
while X's *recovered* record — still reachable from a journal before-edge or a
pinned snapshot, and carrying different content — also reads 500. Two different
renderings of X under one key.

Fix: fold `max(record.revision)` into the reachability walk that
`Model::open` **already runs** over every record to rebuild refcounts (claim
`15-memory-model#counts-rebuilt-by-reachability-walk`), and open the document at
`max_stamp + 1` rather than 0. The walk already visits every record; the max is
free. The recovered document's *content* is unchanged; only its starting revision
number moves, which no claim constrains (the durable root is the document —
`model.hpp:225` — not the number beside it).

A doc 08 JSON load needs none of this: the writer emits records field-by-field
and never emitted a stamp, so a load mints every record with `revision = 0` at a
document opened at revision 0 (claim `08-serialization#load-installs-version-0-baseline`),
and the counter is monotone from there against a cold cache. **No doc 08 delta,
no format-major bump, no reader/writer change.**

The residual non-monotonicity — undo restoring an old stamp (constraint 5) — is
*correct*, not a hazard, because the restored record is byte-identical to the one
the old tiles were rendered from. But it does contradict doc 14:349's "revisions
become version-monotonic", which is the **doc 14 delta** below.

### 3. The aggregate fold sums a bijective 64-bit mix of each contribution, not the raw contributions.

`aggregate_revision` currently folds with `acc += contribution(node)`
(`operator_graph.cpp:38`), and `operator_graph.hpp:87-96` argues this is
collision-free. **That argument holds only while every contribution carries the
same document-global value, and it collapses the moment they do not** — which is
what this task does.

The failure is not exotic. Stamps are small monotone integers. Two reachable
inputs at stamps 7 and 3 fold to 10; a later configuration at stamps 6 and 4
folds to 10 as well. Opposite-direction moves of equal magnitude are reachable
through ordinary undo/redo interleavings and through membership edits that swap a
high-stamp layer out for a low-stamp one — and a collision here means the
composed-result tile of a *different* configuration is served: wrong pixels,
silently, from a cache hit.

So: fold `acc += mix64(contribution(node))`, where `mix64` is a bijective 64-bit
finalizer (splitmix64's) in a new `src/base/arbc/base/hash_mix.hpp` (`base` is
L0; every component already depends on it, so this adds **no** levelization edge —
`cache`'s `detail::key_hash_combine` at `key_shapes.hpp:149-151` is L3 and
therefore unusable from `model`). Sum-of-bijective-hashes keeps every property the
existing claim names — order-independent (permuting `inputs()` yields the same
value), each reachable node visited exactly once, cycles terminating — while
removing structured cancellation.

This **strengthens** `05-recursive-composition#aggregate-revision-folds-reachable-inputs`,
whose registered text already promises the fold "changes **iff** some reachable
input's revision contribution changes" — an "iff" the raw sum does not actually
deliver and was only getting away with because contributions were uniform. Doc
05:129-144 prescribes *what the fold must detect* and never prescribes its
arithmetic, so this needs **no doc delta**. The numeric aggregate value changes,
so any test asserting a specific aggregate number must be re-derived; pixel
goldens are unaffected (they key nothing).

*Rejected: keeping the sum and accepting the collision risk.* Shipping a known
wrong-pixel path to save ten lines, in the same commit that ships the mechanism
which creates it, is indefensible. *Rejected: widening the aggregate to 128 bits
or a full hash struct.* The key slot is a `uint64` by doc-17 constraint
(`key_shapes.hpp:23-31`); mixing before summing gets the collision resistance
without touching the key shape.

### 4. The runtime projects the stamp through a per-revision memo, built beside the existing identity memo.

`PullConfig::contribution` is keyed on `const Content*`, which carries no id — the
runtime already solves exactly this with `make_pull_identity_of`
(`pull_identity.hpp:16-52`), memoized per pinned revision
(`interactive.cpp:109,:326`). Add a second column to that memo: an
`ObjectId -> uint64` stamp map, built **eagerly on the frame thread** during
`refresh_identity_memo`, then read lock-free by workers.

Eager + immutable is the load-bearing choice. `DocRoot` is immutable, so a lazy
memo would need mutable state and a lock — and workers pull concurrently
(`pull_service.cpp:179-195`), which is precisely the shape that produced the
audio-lookahead cache-thread-safety trap already on record. Building the map once
per revision on the frame thread and handing workers a read-only snapshot keeps
the render path allocation-free and lock-free, matching what `d_identity_map`
already does.

The three drivers then become
`config.contribution = revision_contribution_of(memo)` at
`interactive.cpp:337`, `offline_sequence.cpp:99`, `export_monitor.cpp:121`; the
leaf/operator lambda at `tile_planning.cpp:391-401` takes the same functor
instead of closing over `state.revision()`; and — per constraint 6 —
`LookaheadConfig`'s scalar `revision` (`lookahead.hpp:65-69`) becomes that same
functor, so the ring's warm key and the pull's probe key remain equal by
construction rather than by coincidence.

Offline stays revision-consistent: the contribution reads stamps from the *pinned*
`DocRoot`, so `02-architecture#offline-sequence-pins-single-revision` and
`12-audio#export-monitor-pins-single-revision` hold unchanged — a pinned state's
stamps cannot move.

### 5. A content's contribution folds in the *arrangement* of any composition it names.

This is the hazard the `.tji` note misses entirely, and it is a wrong-pixel one.

The compositor's fold walks `inputs()`, which yields **`Content`s**. But a nested
content's composed-result pixels also depend on things that are **not** contents:
the child composition's layer order, and each member layer's transform, opacity,
span and time map. Those live in `CompositionRecord` (`records.hpp:136-144`) and
`LayerRecord` (`records.hpp:68-92`) — separate objects with their own `ObjectId`s.
Reorder a child composition, or nudge one child layer's transform, and **no child
content's stamp moves**. Under Decision 1 alone, the parent's composed-result key
would be unchanged and the cache would serve the pre-edit composite. Today's
document-global revision masks this by bumping everything.

So the contribution for a content `N` is:

```
contribution(N) = mix64(object_revision(id_of(N)))
                + (N->composition_ref().valid()
                     ? composition_revision(N->composition_ref())
                     : 0)

composition_revision(C) = mix64(object_revision(C))
                        + Σ over C's member layers L of mix64(object_revision(L))
```

`composition_revision` is a shallow, one-level walk of `CompositionRecord::layers`
plus the `spill_root` chunk chain — **pure model vocabulary**, so it lives in
`model` (L2), where doc 17:53 already puts "revisions". It does *not* recurse:
the grandchildren are reached by the compositor's own `inputs()` fold, each level
contributing its own composition's arrangement. Correct by induction, and it costs
one shallow walk per composition per revision, memoized by Decision 4.

The *joining* of the two halves must happen in `runtime`, not `model`, because
`composition_ref()` is on the `Content` vtable (`contract/content.hpp:598`) and
doc 17:80-84 keeps the model free of that vtable. Model exposes the two
primitives (`DocRoot::object_revision(ObjectId)`,
`DocRoot::composition_revision(ObjectId)`); the runtime memo (Decision 4) calls
both and stores the combined value. No new levelization edge.

*Rejected: giving `LayerRecord` a back-pointer to its owning composition, so a
layer edit could stamp the composition in the model and collapse the fold to
O(1).* This would be simpler at the fold — but `LayerRecord` has no such field
(`records.hpp:68-92`), adding one is a second record-layout change, and it would
make every layer mutator responsible for a second write into an object it did not
otherwise touch, which erodes constraint 4's "path-copy only what you touched".
Memoizing a shallow walk is cheaper than a new structural edge.

### 6. The owning-edge `SlotRef` identity (the note's option (b)) is rejected as unsound.

The note asks the task to "settle that hazard before choosing (b); do not ship (b)
on the assumption that slots do not recycle". Settled: **slots do recycle, and (b)
aliases in release builds.**

`SlotRef::operator==` compares the index alone (`refs.hpp:70`); the generation tag
that would distinguish a recycled slot is `#ifndef NDEBUG` (`refs.hpp:77-87`), and
`assert_generation` compiles to `{}` in release (`refs.hpp:569-580`). Slots are
recycled by design — "a released slot is a perfect hole reused by the next
same-class allocation" (`slot_store.hpp:96-98`), and the store's own header says
it outright: "**a slot is a storage location that recycles, an ObjectId is a
document identity**" (`slot_store.hpp:24`).

The aliasing is not a corner case under the workload this task exists to serve. A
free list is LIFO: in a paint loop, object X's record is path-copied to a fresh
slot each dab while the prior slot is retained by the journal's before-edge — and
once that journal entry trims to its byte budget, the slot frees and is handed
straight back to the next path-copy of X. X's stamp returns to a value under which
stale tiles are still resident. The failure is timing-dependent, release-only, and
manifests as silently wrong pixels.

Making generations release-mode would fix it, but it grows `SlotRef` from 4 to 8
bytes — and `SlotRef` is a **per-edge** type (`HamtNode` holds an array of them,
`hamt.hpp:88-97`), so that is paid on every edge of the persistent map, against a
`uint64` on `ObjectRecord` which is paid once per *object*. It would also
reverse a settled pool decision (`tasks/refinements/pool/refs.md:104`, "generation
tags debug-only per doc 15's debug-discipline framing") and break the
`static_assert(sizeof(SlotRef<int>) == 4)` at `refs.hpp:604-606`. Strictly worse
than Decision 1 on every axis.

The `.tji` note's parenthetical that (b) is "FREE" is therefore false: it is free
only in a build where it is also wrong.

### 7. The stale-revision fallback probe becomes per-content.

`d_prior_revision` (`interactive.hpp:415`) is a single scalar — "the caller's
last-completed revision" — fed to `plan_layer`'s `prior_revision`
(`tile_planning.cpp:191`) and used for the deliberate prior-revision probe at
`:257-261`. Under per-object stamps a document-global prior revision names no
content's prior stamp, so the probe would miss unconditionally and the
degradation ladder would quietly lose its stale tier.

Replace the scalar with an `ObjectId -> uint64` map of *last-composited stamps*,
written at frame end from the `visible_plans` the driver already collects
(`interactive.cpp:382,:509`) and storing the `layer_revision` actually used —
which makes the operator (aggregate) and leaf cases uniform, since both flow
through the same variable at `tile_planning.cpp:399-401`.
`render_frame_interactive` takes `const PriorStamps*` and resolves it per layer;
`plan_layer`'s signature is **unchanged** (it still takes a per-call
`optional<uint64>`), so the probe logic at `:257-261` is untouched. A content the
frame has never composited has no prior stamp and simply has no stale tier —
which is exactly today's `nullopt` behavior, and what the offline drivers already
pass (`offline_sequence.cpp:135,:186`).

This preserves `02-architecture#degraded-fallback-preference-order` and
`#revision-bump-preserves-stale-tiles-as-fallback` under the new key. The two
interactive tests that assert `renderer.prior_revision() == state->revision()`
(`src/runtime/t/interactive.t.cpp:1200,:1248`, `tests/host_viewport.t.cpp:727-728`)
move to the per-content accessor.

### Design-doc delta

Three edits, riding the closer's commit (doc 16's same-commit rule):

- **`docs/design/14-data-model-and-editing.md`** — the substantive delta. Amend §
  Cross-doc impact line 349 ("revisions become version-monotonic") and add a short
  § *Per-object revisions* under the versioning material: the **document**
  revision is version-monotonic; each object additionally carries a revision stamp
  minted from the publishing revision at the copy-on-write that every mutation
  already performs, so a commit stamps exactly the objects it touched and every
  untouched object keeps its stamp by structural sharing; a stamp is monotone for
  a given object across forward commits but is **restored, not advanced**, by
  undo/redo navigation, because `navigate()` republishes the byte-identical record
  — which is correct under § Render purity (lines 221-227), since a tile keyed on
  the restored stamp shows exactly the pixels that record produces, and is
  desirable, since undoing a stroke makes the pre-stroke tiles reachable again.
  Note the recovery rule (Decision 2).
- **`docs/design/01-core-concepts.md` lines 155-165** — one-clause honesty fix:
  the per-object revision is monotone across forward commits and is restored by
  undo, so "monotonically increasing" is qualified rather than left literally
  false. Doc 01's cache-key formula itself is *realized*, not amended.
- **`docs/design/00-overview.md`** — a Resolved-questions decision-record bullet
  (this is project-shaping: it changes the cache-key semantics of the whole
  engine): per-object revision stamps live in `ObjectRecord` and ride structural
  sharing; the aggregate fold mixes before summing; a composition's arrangement
  joins its embedder's contribution; `SlotRef` identity is not an object stamp
  ("a slot recycles, an ObjectId does not").

**No doc 15 delta** (Decision 1: doc 15 pins record properties, not a field list,
and already sanctions the stride change), **no doc 05 delta** (Decision 3: doc 05
prescribes what the fold detects, not its arithmetic), **no doc 08 delta**
(Decision 2: stamps are never serialized), **no doc 17 delta** (Decisions 4-5: no
new levelization edge).

## Acceptance criteria

**Unit tests** — `src/model/t/per_object_revision.t.cpp`, wired via
`arbc_component_test(COMPONENT model ...)` in `src/model/CMakeLists.txt`, Catch2,
matching `transactions.t.cpp` style:

- A commit stamps exactly the touched objects with the published revision; every
  untouched object's stamp is unchanged **and its record is still shared by
  `SlotRef` identity** (assert via `DocRoot::record_edge`, `model.hpp:96-100` —
  the accessor that exists for exactly this proof).
- `object_revision` on an absent id, and `composition_revision` on a
  non-composition id, are well-defined no-ops (0).
- `composition_revision` changes on attach, on detach, on reorder, and on a member
  layer's transform/opacity/span/time-map edit — and is unchanged by an edit to a
  member layer's *content* (which the compositor's `inputs()` fold covers instead).
  Cover the spill-chunk chain (> `k_max_inline_layers` layers), not just the
  inline arm.
- Undo restores the prior stamp while the document revision advances; a
  commit → undo → redo round-trip returns every stamp to its post-commit value
  (the stamp twin of `#undo-redo-round-trips`).
- Reopening a workspace file opens the document at `max(persisted stamp) + 1`, and
  a subsequent commit mints a stamp exceeding every recovered stamp (Decision 2).
- `sizeof(ObjectRecord)` still satisfies every static assert at `records.hpp:166-185`.

**Behavioral-counter assertions** (doc 16:54-62 — counters, never wall-clock):

- **The headline.** A 60-commit brush-stroke loop (one transaction per dab) on one
  layer of a multi-layer scene, driven through the **real `InteractiveRenderer`**:
  every unedited layer's tiles keep their keys across all 60 commits, and a pan
  after the stroke composites them from the cache with `requests_issued` delta 0
  for the untouched layers. Assert the pre-change behavior is what changed by
  construction — the same scene on a document-global key re-renders the whole
  viewport.
- **Re-enforce `05-recursive-composition#static-subtree-served-from-cache` through
  a shipped driver, not a stub.** The existing enforcement
  (`tests/nested_cache.t.cpp:87,:109`) supplies its own hand-written
  contribution. Add an enforcement that drives the real `InteractiveRenderer` (its
  own `config.contribution`, `interactive.cpp:337`) and asserts the registered
  text's "survives ... an unrelated edit" clause with `requests_issued` delta 0.
  This claim is currently true of the stub and false of the product; the task is
  not done until it is true of the product.
- The stale tier still fires per-content (Decision 7): a frame that composited X
  under stamp `s`, then commits an edit to X, plans X's fresh key as a miss and
  reaches X's `s`-tile as the degraded display — `degraded_composites` delta 1,
  preserving `02-architecture#degraded-fallback-preference-order`.
- The audio ring's warm key equals the pull's probe key under per-object stamps
  (constraint 6): an edit to one audible layer leaves an unrelated layer's
  prepared blocks resident (`pull_audio` dispatch delta 0), where a
  document-global key re-mixed them.

**Golden tests** (deterministic rendering — byte-exact, no tolerance):

- The composition-arrangement fold (Decision 5) has a *stale-pixel* failure mode,
  so pin it at the pixel: a nested child whose layers are **reordered** (and,
  separately, one of whose layer transforms is nudged) re-renders the parent's
  composed result byte-identically to the same scene authored fresh. A
  contribution built from the content stamp alone serves the pre-edit composite —
  the golden is what catches it.
- The full interactive and offline golden suites are byte-unchanged by this task
  on a scene with no edits (the key value moves; the pixels do not).

**Conformance** — no new content kind or operator, so no new `arbc-testing` run is
scoped. The existing operator conformance suite (`tests/operator_conformance.hpp:242`,
which supplies its own constant contribution) must still pass unmodified: an
operator's conformance must not depend on where its contribution comes from.

**Claims** (register in `tests/claims/registry.tsv`, two tab-separated columns;
enforce with `// enforces: <claim-id>` — `scripts/check_claims.py` gates both
directions):

- `14-data-model-and-editing#commit-stamps-only-touched-objects` — A commit writes
  the published revision as the per-object revision stamp of exactly the objects it
  touched, riding the copy-on-write each mutation already performs; every untouched
  object keeps its prior stamp with no write at all, by the same structural sharing
  that keeps its record (#commit-shares-untouched-structure), so the stamp costs no
  traversal and O(path depth) still bounds a commit
- `14-data-model-and-editing#undo-restores-the-prior-stamp` — An undo publishes a
  new document revision (revision +1) but RESTORES each touched object's prior
  revision stamp rather than advancing it, because navigate() republishes the
  stored owning edge by SlotRef identity and never re-creates a record; the
  restored record is byte-identical to the one the pre-edit tiles were rendered
  from, so those tiles become legitimately reachable again, and commit -> undo ->
  redo returns every stamp to its post-commit value
- `14-data-model-and-editing#stroke-does-not-orphan-the-viewport` — Over a 60-commit
  brush-stroke loop (one transaction per dab) on one layer of a multi-layer scene,
  every unedited layer's tile keys are unchanged across all 60 commits, so a pan
  after the stroke composites them from the cache with zero dispatched renders
  (requests_issued delta 0 for the untouched layers), where a document-global
  revision key left every tile outside the dab cached under a dead key and
  re-rendered the whole viewport cold
- `15-memory-model#recovery-resumes-above-every-persisted-stamp` — Reopening a
  workspace file opens the document at max(persisted per-object stamp) + 1, folding
  the max into the reachability walk that already rebuilds refcounts over every
  record, so a commit in the recovered session can never mint a stamp already
  carried by a record still reachable from a journal before-edge or a pinned
  snapshot -- which would key two different renderings of one object alike
- `05-recursive-composition#composition-arrangement-joins-the-contribution` — A
  content's revision contribution folds its own record stamp with the arrangement
  stamp of the composition it names (that composition's record stamp plus every
  member layer's record stamp, inline arm and spill chain alike), so a reorder, an
  attach/detach, or a member layer's transform/opacity/span/time-map edit -- none of
  which touches any child CONTENT, and none of which the inputs() fold can see --
  still changes the embedder's composed-result key and re-renders it; a contribution
  built from the content stamp alone serves the pre-edit composite
- `05-recursive-composition#aggregate-fold-mixes-before-summing` — The aggregate fold
  sums a bijective 64-bit mix of each reachable node's contribution rather than the
  raw values, so two inputs whose stamps move in opposite directions by equal
  amounts (an undo/redo interleaving; a membership edit swapping a high-stamp layer
  for a low-stamp one) cannot cancel to a previously-seen aggregate and serve
  another configuration's composed tile; the fold stays order-independent and visits
  each reachable node exactly once. This makes the "iff" of
  #aggregate-revision-folds-reachable-inputs true, which the raw sum delivered only
  while every contribution carried the same document-global value
- `02-architecture#stale-probe-is-per-content` — The stale-revision fallback probes
  the prior stamp of THAT content -- the stamp the last frame composited it under,
  aggregate for an operator layer and leaf stamp otherwise -- not a document-global
  prior revision, which under per-object stamps names no content's prior stamp and
  would miss unconditionally; a content the frame has never composited has no prior
  stamp and no stale tier, preserving #degraded-fallback-preference-order

**Concurrency** (doc 16 tier 6): the contribution memo is built eagerly on the
frame thread and read lock-free by workers (Decision 4), which is the shape that
keeps this off the TSan critical list. Assert it: a pull-side stress run with
`worker_count > 0` over a scene with a nested operator, under the asan lane, with
no writer thread committing into the pinned state. The parked-TSan line
(`tasks/refinements/pool/reclamation.md`) is unchanged; stress stays in
`quality.stress_harness`.

**Coverage / gate**: ≥90% diff coverage on changed lines; `scripts/gate`,
`scripts/check_levels.py` (Decisions 4-5 add no edge — this must stay green
without a table edit), `scripts/check_claims.py`.

**Deferred follow-up**: none new. This task deliberately absorbs the three
consumers the `.tji` note omits (Decisions 3, 5, 7) rather than deferring them —
each one, left alone, converts document-wide over-invalidation into a
silently-wrong-pixel path, which is a strictly worse place to stop.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-13.

- `src/model/arbc/model/records.hpp`, `src/model/model.cpp` — Added `uint64_t revision` field to `ObjectRecord`; stamp minted from `d_base_revision + 1` at the existing COW write site in every mutator (Decision 1); no new traversal, O(path depth) preserved.
- `src/base/arbc/base/hash_mix.hpp` (new), `src/compositor/operator_graph.{hpp,cpp}` — `mix64` bijective 64-bit finalizer (splitmix64's) placed in `base` (L0); aggregate fold now sums `mix64(contribution(node))` rather than raw contributions, removing structured cancellation (Decision 3).
- `src/model/arbc/model/model.hpp`, `src/model/model.cpp` — `DocRoot::object_revision`, `DocRoot::composition_revision` exposed; `Model::open` recovery walk seeds the document revision at `max(persisted stamp) + 1` (Decision 2); fixer sub-agent fixed release-build `SlotStore` aliasing by grouping live sets by store identity and calling `finalize_open` once per store.
- `src/runtime/arbc/runtime/pull_identity.hpp`, `src/runtime/pull_identity.cpp`, `src/runtime/arbc/runtime/interactive.hpp`, `src/runtime/interactive.cpp` — per-revision `ObjectId→uint64` contribution memo built eagerly on the frame thread; `content`+`composition_revision` joined here per Decision 5; all three drivers and audio lookahead (`src/audio_engine/arbc/audio_engine/lookahead.hpp`, `src/audio_engine/lookahead.cpp`) switched from document-global scalar to per-object contribution functor (Decisions 4, 6).
- `src/compositor/arbc/compositor/tile_planning.hpp`, `src/compositor/tile_planning.cpp` — stale probe replaced with per-content `PriorStamps` map written at frame end; `plan_layer` signature unchanged (Decision 7); offline drivers (`src/runtime/offline_sequence.cpp`, `src/runtime/export_monitor.cpp`) pass `nullptr`.
- New tests: `src/base/t/hash_mix.t.cpp` (new), `src/model/t/per_object_revision.t.cpp` (new), `tests/per_object_revision_keys.t.cpp` (new — 60-dab stroke, nested-arrangement pixel golden, static-subtree through shipped driver, audio ring/pull key equality); 7 new claims in `tests/claims/registry.tsv`.
- Design-doc deltas: `docs/design/00-overview.md` (decision record), `docs/design/01-core-concepts.md` (monotonicity qualification), `docs/design/14-data-model-and-editing.md` (per-object revisions section + monotonicity correction).
- Tech-debt follow-up registered: `pool.size_class_aliasing_is_debug_invisible` — the debug/release `sizeof` difference means the shared-store path is structurally invisible to the gate's debug build; wired to `milestones.m10_post_01`.
