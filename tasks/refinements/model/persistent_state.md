# model.persistent_state — Persistent DocState on arenas

## TaskJuggler entry

`tasks/10-model.tji` → `model.persistent_state` ("Persistent DocState on
arenas"). The task block (`tasks/10-model.tji:9-13`) carries no `depends`
line yet; the closer adds `depends pool.refs` (see Inherited dependencies)
and the `Refinement:` note back-link on completion.

## Effort estimate

3d.

## Inherited dependencies

- `pool.refs` — **settled** (`tasks/refinements/pool/refs.md`, Status Done
  2026-07-04). Provides the ownership layer this task builds records on:
  - `Ref<T>` — pointer+index owning transient handle for stack/API use
    (`src/pool/arbc/pool/refs.hpp:100-225`); a held `Ref` is one count on
    the target. This is the root-keeping reference a pin resolves to.
  - `SlotRef<T>` — 4-byte, position-independent, standard-layout,
    trivially-copyable index-only reference (`refs.hpp:64-88`); the **only**
    reference form allowed inside a record (doc 15's mmap requirement).
    `static_assert(sizeof(SlotRef)==4)` in release.
  - `RefStore<T>` (`refs.hpp:246-563`): `create(...)` (writer-only alloc +
    placement-new + count=1, `:276`), `resolve(SlotRef)` (pin: checked
    retain → `Ref`, any thread, `:296`), `peek(SlotRef)→T*` (zero-refcount
    traversal deref, any thread, `:313`), `retain`/`release` (`:322`,`:337`),
    `set_zero_sink(ZeroCountSink*)` (`:403`).
  - Debug generation tags: a record holding stale `SlotRef` edges faults
    loudly on `resolve`/`peek` (zero cost in release).
- Transitively settled (the `pool.refs` chain, all landed on `main`):
  - `pool.arena_core` — `Arena::store_for(size, align, …)` (one
    `SlotStore` per size class, `src/pool/arbc/pool/slot_store.hpp:350`);
    `SlotStore` fixed-slot storage with **stable slot addresses for the
    store's life, growth appends chunks and never reallocates**
    (`slot_store.hpp:132-330`); `TypedStore<T>` placement veneer
    (`typed_store.hpp:22-59`); `arbc::expected<T,E>` in `base`.
  - `pool.refcounts_in_store` — refcount + generation columns owned by the
    size-class store and indexed by **physical slot**, so composition/layer/
    content records that share a `(sizeof, alignof)` size class share one
    count column and one slot-index space (claim
    `15-memory-model#one-count-column-per-size-class`).
  - `pool.reclamation` / `pool.free_pools` — `DeferredReclaimSink<T>`
    (`reclamation.hpp:34-45`) makes dropping the last count a lock-free CAS
    push (no `~T`, no data-page touch); `ReclamationQueue::drain()`
    (`:63-107`) runs the destructor cascade **iteratively** (stack bounded
    independent of subtree depth); cross-thread release admitted, allocation
    stays writer-only.
  - `pool.checkpoints` — `Checkpointer` A/B-root durable publish
    (`checkpoint.hpp:87-301`); `finalize_open` rebuilds the free list as the
    below-high-water complement **after the caller's reachability walk sets
    counts** (`:205`) — the typed walk over DocState nodes it leaves to the
    model layer is deferred here (see Acceptance criteria).

## What this task is

Replace the walking-skeleton `DocState` — today a `std::vector<LayerRecord>`
whole-copied on every commit (`src/model/arbc/model/model.hpp:24-27`,
`src/model/model.cpp:12`) — with the real thing: a **path-copying persistent
map from `ObjectId` to immutable object records** (composition, layer, and
content records), the records living as fixed-size slab types on a
document-owned pool arena and referring to one another by 4-byte `SlotRef`
edges. A commit builds the next version by copying only the touched path
through the map and publishes it by an atomic swap of the current-version
handle; every untouched record is shared by `SlotRef` identity between
versions. A **pin** is a held root reference to a version; dropping the last
pin lets that version's unique nodes cascade through the pool's deferred
reclamation queue while shared nodes survive on their counts.

This task lands the persistent map, the three record types, atomic publish,
and pins-as-root-refs. It deliberately leaves seams (not implementations)
for the sibling tasks that build on it — transactions, journal, damage, the
`Editable` facet, and the content-binding migration.

## Why it needs to be done

`DocState` is the substrate the rest of the model stream and the render path
sit on. The walking skeleton's whole-copy stand-in is O(document) per commit
and cannot carry structural sharing, per-version pins, or content state —
the properties docs 14/15 promise. Directly downstream (all in
`tasks/10-model.tji`, each `depends !persistent_state` or transitively):

- `model.transactions` — drives "build next version + publish + swap root."
- `model.content_binding` — migrates the runtime `Document`'s
  `std::unordered_map<ObjectId, shared_ptr<Content>>` side-map
  (`src/runtime/arbc/runtime/document.hpp:30-32`) into the versioned content
  records this task defines.
- `model.journal` / `model.damage` / `model.editable_facet` — use the
  `ObjectId`-addressed, pinnable versions as their targets and state carriers.
- `quality.stress_harness` (`tasks/70-quality.tji`) already
  `depends model.persistent_state` — the seeded publish/pin + reclamation
  stress lives there, not here.

Milestone: this is the keystone of the versioned-model milestone; the closer
confirms its milestone `depends` edge on completion.

## Inputs / context

Design docs (normative; doc 16 same-commit rule — this task realizes their
promises, it does not redesign them):

- `docs/design/14-data-model-and-editing.md`
  - § *The central decision: versions, not mutations* (lines 20-56) — the
    defining sentence (lines 50-56): "`DocState` is a persistent
    (path-copying) map from `ObjectId` to immutable object records —
    composition records (layer order, canvas, working spaces), layer records
    (placement: transform, opacity/gain, span/time map, flags, content
    reference), and content records (kind id + a state handle …). The writer
    thread is the single mutator … `Document` holds the current `DocState`
    in an atomic shared pointer." Path-copy cost rationale, lines 44-48.
  - § *Identity* (lines 58-65) — `ObjectId` is "64-bit, document-unique,
    assigned at creation," the map key and the address used by journal,
    damage, and selection.
  - § *Transactions* (lines 69-96) — the atomicity guarantee ("observers see
    v(n) or v(n+1), never a half-edit," lines 83-85) and "Reading needs no
    transaction … everyone else reads pinned versions" (lines 95-96). The
    commit *mechanism* (build/publish/journal/flush) is `model.transactions`;
    this task provides the build-next-version + swap-root primitive it drives.
  - § *Content state: the `Editable` facet* (lines 98-131) — the published
    version "holds the *after* handle, so a pinned version pins content state
    too" (lines 128-131). `capture()`/`restore()`/`state_cost()` are
    `model.editable_facet`; this task only reserves the content record's
    `StateHandle` field.
  - § *History* (lines 159-178) — version GC "releases unpinned `DocState`
    nodes and unreferenced state handles by refcount" (lines 170-171): the
    refcount/root semantics this task must expose for the journal + queue.
- `docs/design/15-memory-model.md`
  - § *Memory populations* (line 18-19) — "Document records | DocState map
    nodes, composition/layer/content records | … | inside-out slabs."
  - § *Evaluation* (lines 42-44, 60-62, 93-100) — reads pass `const&`/`peek`
    and "reads don't touch refcount pages at all"; the read-path win is the
    "interference-free concurrent pin"; in-record edges are the 4-byte
    `SlotRef`.
  - § *Version reclamation* (lines 112-143) — "A version is memory-live
    exactly while reachable: pinned as a `DocState` root … or referenced by a
    journal entry's state handles. Unpin/trim drops a root reference;
    everything unique to that version cascades; everything shared survives via
    its count" (lines 118-123). "The writer thread is the only structural
    allocator" (lines 141-143); render/audio threads may only pin/unpin +
    enqueue reclamation.
  - § *What this asks of doc 14 and the kinds* (lines 223-245) — "`DocState`
    map nodes and object records get fixed-size slab types … node arity
    chosen so records land in a small number of size classes" (lines
    225-227); "record types are standard-layout, fixed-size, and their
    references are slab refs" (lines 243-245); "`StateHandle` is a slab
    reference" (lines 237-239).
  - § *File-backed arenas* (lines 192-214) — in-record refs are index-only;
    the durable A/B-root publish and durability-epoch slot quarantine (the
    checkpoint path this task's records must be *compatible* with, not drive).
- `docs/design/17-internal-components.md` — `arbc::model` is **L2**, may
  depend only on `base` (L0) and `pool` (L1) (component table line 52;
  levelization rule lines 41-44). CI-enforced by
  `scripts/check_levels.py:22` (`"model": {"base", "pool"}`).
- `docs/design/16-sdlc-and-quality.md` — claims register (`tests/claims/
  registry.tsv`, `enforces: <claim-id>` tags, lines 15-21); behavioral-
  counter test tier (lines 54-62, "Wall-clock tests lie in CI; counters
  don't"); concurrency/TSan tier (lines 66-73); ≥90% diff-coverage gate
  (lines 112-118).

Source seams this task extends / replaces:

- `src/model/arbc/model/model.hpp:24-27,31-34,45-66` — `DocState`,
  `DocStatePtr`, `Model`, `Model::Transaction`, `d_current`; the
  whole-copy stand-in comment at lines 31-34.
- `src/model/model.cpp:12,29-32` — the literal whole-`DocState` copy and the
  atomic publish store.
- `src/base/arbc/base/ids.hpp:11-17` — `ObjectId` (L0, unchanged).
- `src/model/CMakeLists.txt:5` — currently `DEPENDS base`; this task adds
  `pool`.
- `src/model/t/model.t.cpp:7` — the existing enforcing test for
  `14-data-model-and-editing#pinned-version-never-observes-later-edit`,
  rewritten against the new `DocState` surface (claim preserved).

Predecessor-refinement conventions (`tasks/refinements/pool/*.md`): errors as
`expected` values never throw/abort; records stay trivially destructible so
reclamation walks them explicitly; index-only `SlotRef` inside records,
pointer-carrying `Ref` only on stacks; levelization stated + CI-enforced; the
full seeded stress deferred to `quality.stress_harness`; TSan smoke runs
under the asan lane meanwhile (no TSan preset in-tree yet).

## Constraints / requirements

- **Levelization (doc 17, CI-gated).** `arbc::model` is L2; this task adds
  `pool` to `src/model/CMakeLists.txt`'s `DEPENDS` (→ `{base, pool}`, a
  subset of the allowed set at `scripts/check_levels.py:22`). No edge onto
  `media`/`surface`/`contract`; `pool` must never depend back on `model`.
- **Records are fixed-size slab types.** Every record — composition, layer,
  content — and every persistent-map interior node is standard-layout,
  fixed-size, pointer-free, and **trivially destructible** (docs 15:225-227,
  243-245; the trivial-dtor rule lets `ReclamationQueue::drain` walk them).
  In-record edges are **index-only `SlotRef`** (never `Ref`, never a raw
  pointer, never STL). Enforced by `static_assert(std::is_standard_layout /
  is_trivially_destructible)` per record type, mirroring
  `pool.refs`' `sizeof(SlotRef)==4` static_assert.
- **Records live on a document-owned `Arena`.** Allocation via
  `RefStore<T>::create` through `Arena::store_for` (`slot_store.hpp:350`),
  one store per size class. Multiple record types deliberately share a size
  class where their `(sizeof, alignof)` match, exercising the single
  count-column-per-size-class contract (`15-memory-model#one-count-column-
  per-size-class`) — a record type must therefore tolerate its slot being
  recycled to a different record type.
- **Persistent map = path-copying, structural-sharing.** Keyed on the 64-bit
  `ObjectId`. A commit touching k objects allocates only the O(k · depth)
  nodes on the touched paths; all other subtrees are shared by `SlotRef`
  identity. No whole-map copy (the stand-in's behavior) survives.
- **Single-writer discipline (docs 14:54, 15:141-143).** All allocation and
  structural mutation is writer-thread-only (`RefStore::create` is
  writer-only). Any thread may pin (resolve/retain) and unpin (release).
- **Reads are lock-free and refcount-free.** Traversal of a pinned version
  follows `SlotRef` edges via `RefStore<T>::peek` (`refs.hpp:313`) passing
  `const Ref&`, touching **no** refcount page (docs 15:42-44; anchor
  `15-memory-model#const-ref-traversal-touches-no-refcount-page`).
- **Atomic publish.** Publishing a new version is a single atomic swap of the
  current-version handle; observers see the old or the new root, never a
  half-edit (doc 14:83-85). See Decisions for the mechanism.
- **Pins are root refs (docs 15:118-123).** A pin holds one root reference to
  a version; while any pin (or a future journal state handle) holds it, the
  version is memory-live. Dropping the last pin releases the root; unique
  nodes cascade, shared nodes survive.
- **Reclamation is deferred, never inline (doc 15:129-136).** The document
  arena installs a `DeferredReclaimSink` (`set_zero_sink`), so dropping a
  version's root only enqueues; a `drain()` between transactions runs the
  cascade. The dropping thread never runs a destructor storm inline.
- **Content record carries `{ kind id, StateHandle }` but is inert here.**
  This task defines the content record's shape and a `StateHandle` field
  (a `SlotRef`-shaped, fixed-size slab handle per doc 15:237-239) but gives
  it no capture/restore semantics and does not populate it —
  `model.editable_facet` supplies `capture()`/`restore()`/`state_cost()` and
  `model.content_binding` populates the records from the runtime side-map.
- **Errors as values.** Allocation/growth failures surface as
  `expected<…, PoolError>` on the writer path; no throw/abort.

## Acceptance criteria

- **Unit tests** (`src/model/t/…`, wired via `arbc_component_test` in
  `src/model/CMakeLists.txt`):
  - Record types are standard-layout, fixed-size, trivially destructible
    (`static_assert` per type); a content/layer/composition record round-trips
    through a `SlotRef` edge and `peek` resolves it.
  - Insert / update / lookup on the persistent map: after a commit, the new
    version resolves the updated record and a pin taken before the commit
    still resolves the old one.
  - Publish + pin lifecycle: N pins on a version; unpin order-independent;
    last unpin releases the root.
- **Behavioral-counter assertions** (doc 16:54-62; assert on the pool's
  `Arena::total_slots_live` / per-store `slots_live()`, never wall-clock):
  - A commit touching one object grows `slots_live` by only the path-copied
    node count (O(depth)), not by the document size — the executable witness
    of structural sharing.
  - After dropping the last pin to a superseded version and one `drain()`,
    `slots_live` returns to the count of nodes still shared with the live
    version (unique nodes reclaimed, shared nodes survive).
- **Claims (register in `tests/claims/registry.tsv` + `enforces:` tag):**
  - Preserve `14-data-model-and-editing#pinned-version-never-observes-later-
    edit` (already at `registry.tsv:4`, enforced by `model.t.cpp:7`) —
    rewrite the test against the new `DocState` surface; the claim stays green.
  - New: `14-data-model-and-editing#commit-shares-untouched-structure` — a
    commit that touches one object path-copies only the nodes on its path;
    every untouched record is shared by `SlotRef` identity between the pre-
    and post-commit versions (behavioral-counter + `SlotRef`-identity test).
  - New: `14-data-model-and-editing#dropping-pin-reclaims-only-unique-nodes`
    — dropping the last pin to a superseded version and draining reclaims
    exactly that version's unique nodes and no shared node
    (behavioral-counter test).
- **Concurrency (doc 16:66-73).** A publish/pin smoke test: a background
  thread repeatedly pins → traverses via `peek` → unpins while the writer
  commits; assert no torn read and no use-after-free. Runs under the **asan
  lane** (no TSan preset in-tree yet — parked convention from
  `tasks/refinements/pool/reclamation.md`, not re-litigated); the full seeded
  schedule-perturbation stress is `quality.stress_harness` (already
  `depends model.persistent_state`), **not** duplicated here.
- **Coverage / gate.** ≥90% diff coverage on changed lines; `scripts/gate`
  green including asan, `scripts/check_levels.py`, and
  `scripts/check_claims.py`.
- **Deferred follow-up (closer registers in WBS).** Workspace-file-backed
  DocState + checkpoint recovery is out of scope here (this task builds
  in-memory arena records that are *checkpoint-compatible* — index-only,
  trivially destructible, fixed-size — but does not drive `Checkpointer`).
  Register a new leaf **`model.workspace_backing`** (~3d, `allocate team`,
  `depends model.persistent_state, pool.checkpoints`, note citing this
  refinement + docs 14/15): allocate DocState records in a workspace-file-
  backed `Arena`, commit on autosave/publish via `Checkpointer::commit`, and
  implement the typed reachability walk that `Checkpointer::finalize_open`
  (`checkpoint.hpp:205`) requires to rebuild counts on open — the walk
  `pool.checkpoints` explicitly left to the model layer. The closer wires it
  into the document-durability milestone (the milestone `pool.checkpoints`
  feeds).

## Decisions

- **Persistent map = a path-copying HAMT (bitmapped trie) over the 64-bit
  `ObjectId`, fixed-arity interior nodes (one/few size classes), leaves
  holding `SlotRef` record edges.** Path copy is O(log₃₂ n) fixed-arity slab
  node allocations per touched key, and the fixed arity is exactly the
  "records land in a small number of size classes" constraint (doc
  15:225-227). _Rejected:_ a persistent balanced tree (variable node shapes +
  rebalance copies bloat the size-class set and per-commit copy count); a
  sorted persistent vector (O(n) path copy — no better than the stand-in).
- **Atomic publish via `std::atomic<std::shared_ptr<const DocRoot>>`, where
  `DocRoot` is a tiny per-version holder owning one `Ref` into the arena
  root record.** This is doc 14's literal mechanism ("`Document` holds the
  current `DocState` in an atomic shared pointer," lines 55-56) and gives
  readers a race-free lock-free pin: the atomic load acquires the version and
  a count in one step, closing the read-index-then-retain race a bare atomic
  `SlotRef` would open (writer could drop + recycle the old root between a
  reader's load and its `retain`). Crucially the shared_ptr control block is
  **one coarse line per version root, dirtied only on version pin/unpin**
  (outputs, exports, autosave — a handful of pinners), *not* per record: the
  hot consumer traversal still follows `SlotRef` edges via `peek` and touches
  no refcount page, so doc 15's interference-free-concurrent-pin property
  (lines 60-62, 96-100) — which is about the lines the *traversal reads* —
  holds. _Rejected:_ arena-native atomic packed `{generation, root_index}`
  with reader-side retain-and-retry — needs hazard-pointer-grade protection
  against slot recycling for the live in-memory path, unnecessary complexity
  when `std::atomic<shared_ptr>` already gives race-free pinning and the
  durable A/B-root swap (`pool.checkpoints`) already covers crash
  consistency. _Rejected:_ a publish mutex — violates the lock-free-reads
  rule (doc 14:95-96, 15:96-100).
- **The content record's `StateHandle` field is defined but inert in this
  task.** Defining the content record's full shape now (per the tji note's
  "composition/layer/content") keeps record size classes stable across the
  sibling tasks, while capture/restore semantics land in
  `model.editable_facet` and population from the runtime side-map lands in
  `model.content_binding`. _Rejected:_ deferring the content record's
  definition to `content_binding` — would churn the size-class layout and the
  map's leaf edge types after this task's records ship.
- **Deferred, sink-based reclamation for dropped versions** (reuse
  `DeferredReclaimSink` + `ReclamationQueue`, `reclamation.hpp:34-107`) rather
  than inline free-on-unpin. Doc 15:129-136 mandates "release enqueues, never
  destroys inline," and the iterative drain keeps the cascade stack bounded
  regardless of version depth. _Rejected:_ inline recursive free — a
  destructor storm on the (possibly render/audio) unpinning thread and
  unbounded C++ recursion on deep versions.
- **No design-doc delta.** Docs 14/15 already settle the persistent map,
  atomic-shared-pointer publish, fixed-size slab records with `SlotRef`
  edges, pins-as-root-refs, and the `StateHandle` slab reference; the HAMT
  arity is an implementation choice within "path-copying persistent map," not
  a doc-level decision. Nothing here reverses a project-shaping decision in
  doc 00.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-05.

- **Records headers:** `src/model/arbc/model/records.hpp` (ObjectRecord + Composition/Layer/Content arms, StateHandle) and `src/model/arbc/model/hamt.hpp` (HamtNode + path-copying HAMT + ambient ReclaimContext).
- **Model core rewritten:** `src/model/arbc/model/model.hpp` and `src/model/model.cpp` — DocRoot/Model/Transaction, HAMT insert/lookup, `std::atomic<std::shared_ptr<const DocRoot>>` atomic publish, deferred reclamation via DeferredReclaimSink.
- **Build wired:** `src/model/CMakeLists.txt` — `DEPENDS base pool` (L2 levelization, CI-gated).
- **Unit tests:** `src/model/t/records.t.cpp` (static_asserts per record type; SlotRef-edge round-trip) and `src/model/t/persistent_state.t.cpp` (insert/update/lookup+pin; pin lifecycle; structural-sharing growth counter; last-pin-drop reclaims exactly 3 unique nodes; concurrency pin/traverse/unpin smoke under asan).
- **Existing test rewritten:** `src/model/t/model.t.cpp` — `14-data-model-and-editing#pinned-version-never-observes-later-edit` preserved and re-enforced against the new DocRoot surface.
- **Claims registered:** `tests/claims/registry.tsv` — two new entries (`14-data-model-and-editing#commit-shares-untouched-structure`, `#dropping-pin-reclaims-only-unique-nodes`); existing `#pinned-version-never-observes-later-edit` retained.
- **Downstream retargeted:** `src/compositor/compositor.cpp` and `src/compositor/arbc/compositor/compositor.hpp` — walking-skeleton consumer updated from removed `DocState` vector to `DocRoot` with ordered layer iteration.
- **Design deviation noted in parking lot:** HAMT interior nodes are not trivially destructible (child SlotRef edges released via `~T` in the drain cascade); the three *object* record types remain trivially destructible (acceptance-criteria static_asserts pass literally).
