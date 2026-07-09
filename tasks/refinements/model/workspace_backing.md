# Refinement — `model.workspace_backing`

## TaskJuggler entry

`tasks/10-model.tji:16-21` — `task workspace_backing "Workspace-file-backed DocState arena"`,
under `task model` (10 — Versioned model, doc 14).

> note "Allocate DocState records in a workspace-file-backed Arena; commit on
> autosave/publish via Checkpointer::commit; implement the typed reachability
> walk that Checkpointer::finalize_open (checkpoint.hpp:205) requires to rebuild
> counts on open — the walk pool.checkpoints left to the model layer. Source:
> tasks/refinements/model/persistent_state.md (deferred follow-up). Docs 14/15."

## Effort estimate

**3d** (`effort 3d`, `tasks/10-model.tji:17`). The durability substrate already
ships whole in `arbc::pool` — `Checkpointer` (`checkpoint.hpp:87`), the
workspace-file-backed `Arena(ChunkSource&)` constructor (`slot_store.hpp:340`),
the `DurabilityEpochFence` slot quarantine (`checkpoint.hpp:58`), and the three
reconstruction seams (`RefStore<T>::restore`/`set_count_index`/`peek_index`,
`refs.hpp:361,380,379`). This task is the *model-side driver + the typed walk*:
give `Model` a construction-time file-backed mode, thread the `Checkpointer` and
its fences onto the two document stores, add a writer-only `checkpoint()`
primitive that flips the durable root, and — the load-bearing deliverable —
write the typed reachability walk over the `DocState` HAMT that rebuilds
refcounts on open, feeding `Checkpointer::finalize_open`. No new component, no
new `arbc::pool` code, no CMake edge (`model` already depends on `pool`).

## Inherited dependencies

**Settled:**

- `model.persistent_state` (`faad68d`, `tasks/10-model.tji:9-14`, the formal
  `depends !persistent_state` at `:19`). Provides the migration *target*: the
  path-copying `DocState` HAMT over `ObjectId`, `Model` owning `Arena d_arena`
  plus `RefStore<HamtNode> d_nodes` and `RefStore<ObjectRecord> d_records`
  (`model.hpp:426-428`), atomic publish via
  `std::atomic<DocStatePtr> d_current` (`:436`), `DocRoot` owning one `Ref` into
  the arena root record (`:37`), `live_slots()` (`:192`), and the deferred,
  sink-based reclamation of dropped versions. It built the records to be
  *checkpoint-compatible* — index-only, fixed-size, standard-layout,
  pointer-free (`records.hpp:151-164,166-182`), spill chunks named by `ObjectId`
  value never an owning edge (`records.hpp:104-105`) — but explicitly did **not**
  drive `Checkpointer`. This task is the follow-up it named
  (`persistent_state.md:276-288`).

- `pool.checkpoints` (Done 2026-07-04, `tasks/refinements/pool/checkpoints.md`,
  the formal `depends pool.checkpoints` at `tasks/10-model.tji:19`). Ships the
  durability protocol as pure `arbc::pool` mechanism and *explicitly leaves the
  typed walk to the model*:
  - `Checkpointer::commit(SlotIndex root_index)` (`checkpoint.hpp:133`) — ordered
    A/B-root durable publish: msync live data chunks → publish the new root into
    the inactive header slot with a bumped generation and flip → msync the
    header; on success the durability fences drain (`checkpoint.hpp:126-171`).
  - `Checkpointer::open(path)` (`checkpoint.hpp:185`) — maps the file, validates
    the header, selects the highest-generation valid root, returns
    `OpenState{ source, root_index, generation, valid }` (`:178-200`) with
    *counts at zero, free lists empty* — a "rebuild-in-progress" state.
  - `Checkpointer::finalize_open(SlotStore&, const std::vector<SlotIndex>& live_set)`
    (`checkpoint.hpp:205`) — repopulates one store's free list with the
    below-high-water complement of the walk's `live_set` and sets the live count.
  - Reconstruction seams on `RefStore<T>`: `restore(high_water)` (`refs.hpp:361`,
    re-binds file chunks, publishes count/reclaim tables, constructs nothing),
    and the raw-index walk primitives `peek_index`/`set_count_index`/`count_index`
    (`refs.hpp:379-385`) — "the walk reads records by storage index — SlotRef
    generations are anonymous and reset on open, so the walk must not assert
    them" (`refs.hpp:375-378`).
  - The `DurabilityEpochFence` slot fence (`checkpoint.hpp:58`, base
    `ReleaseFence` at `slot_store.hpp:59`) installed per workspace-backed store
    via `SlotStore::set_release_fence` (`slot_store.hpp:166`); `slot_fence()`
    accessor at `checkpoint.hpp:124`.
  - The contract line (`checkpoints.md:93-97,358-365`): "The caller (model layer)
    walks the graph from the root, incrementing counts through
    `RefStore<T>::set_count` … The typed walk over real document nodes is out of
    scope (model-layer, L2 — pool must not depend up)." This task is that walk.

**Pending:** none. Both formal predecessors are landed; the seam is complete.

## What this task is

Turn the document's in-memory arena into a **workspace-file-backed** one and
close the durability loop the two predecessors left open: records live in an
mmapped per-document workspace file, a checkpoint makes the current version
crash-recoverable, and reopening the file rebuilds the whole `DocState` — counts
and free lists included — by a typed reachability walk the model owns.

Concretely:

1. **File-backed construction mode.** Today `Model` owns a default
   `Arena d_arena` (`model.hpp:426`) over an anonymous chunk source
   (`Arena()`, `slot_store.hpp:339`). Add a construction-time backing policy: a
   `Model` built with a workspace path owns a `WorkspaceFileChunkSource` and
   constructs its `Arena` over it (`Arena(ChunkSource&)`, `slot_store.hpp:340`),
   owns a `Checkpointer` bound to that source and arena (`checkpoint.hpp:94`), and
   installs the `DurabilityEpochFence` slot fence on **both** document stores
   (`d_nodes`, `d_records`) via `SlotStore::set_release_fence`. The default
   `Model()` stays anonymous and byte-identical — a live-only host (no files)
   and every existing test are unaffected (doc 15:158-160 — "Backing is a
   construction-time arena policy; anonymous remains available").

2. **`checkpoint()` primitive.** A writer-only `Model::checkpoint()` that calls
   `Checkpointer::commit` with the current published `DocState` root's slot
   index, making that version durable (msync + root flip). Cadence — timer,
   transaction count, explicit host call — is host policy, out of scope
   (doc 15:213-216). Per-transaction version publish (the atomic `d_current`
   swap) is unchanged and does **not** checkpoint (see Decision 3).

3. **The typed reachability walk (the deliverable pool left to the model).**
   A static `Model::open(path)` recovery factory that: calls
   `Checkpointer::open` for the durable root, builds an `Arena` over the reopened
   source, calls `RefStore<T>::restore(high_water)` on each store, then **walks
   the HAMT ownership tree from the durable root**, rebuilding every reached
   slot's refcount via the raw-index primitives, and finishes with one
   `Checkpointer::finalize_open(store, live_set)` per workspace-backed store. The
   walk follows only the counted **`SlotRef` edges** — `HamtNode → child
   HamtNode` and `HamtNode → leaf ObjectRecord` (`hamt.hpp:88-97`) — because the
   entire ownership graph is the HAMT: every `ObjectRecord` arm
   (composition / layer / content / spill chunk, `records.hpp:151-159`) is a HAMT
   leaf keyed by its own `ObjectId`, and records reference each other only by
   `ObjectId` *value*, never an owning edge (`records.hpp:104-105`,
   `14-data-model-and-editing.md:58-72`). Result: two per-store `live_set`
   vectors (one for `d_nodes`, one for `d_records`); each reached slot's count is
   its in-degree among reachable edges (Decision 4).

The `ContentRecord.state` (`records.hpp:60`) `StateHandle` stays **inert**
(`k_state_none`) throughout v1 — no kind ships persistent workspace-backed
content-state slabs yet — so the walk descends no state slabs. The walk visits
each `ContentRecord`'s handle and is structured so a per-kind state-slab hook
slots in without rework when a persistent kind lands (Decision 5, and the
deferred concern surfaced below).

## Why it needs to be done

`model.persistent_state` built records that *could* live in a workspace file
(index-only, position-independent) but left them in anonymous memory, and
`pool.checkpoints` built the durability protocol but proved it only against a
test-local record graph — "the model stream drives the real walk on document
open" (`checkpoints.md:358-365`). Until this task lands, the document is not
crash-recoverable through the workspace file: an editor crash loses the whole
in-memory `DocState`, and the file-backed-arena machinery (mmap chunks, A/B
root, epoch fence) has no real consumer. This closes the last open edge of the
memory model (doc 15) — records persist, a checkpoint bounds crash loss to
"at-most-since-last-checkpoint, not the document" (doc 15:163-166), and reopen
reconstructs the exact reachable graph.

It feeds the **document-durability milestone** (`tasks/99-milestones.tji`, the
milestone `pool.checkpoints` also feeds). The doc 08 JSON autosave remains the
interchange / version-control format and the redundant "belt" to this
"suspender" (doc 15:197-203,216); the workspace file is the session/scratch
recovery artifact beside it.

## Inputs / context

Design docs (normative, doc 16):

- **doc 15 § "File-backed arenas: mmap instead of process memory"**
  (`15-memory-model.md:156`):
  - `:158-160` — "arena buffers are **mmapped from a per-document workspace
    file** … Backing is a construction-time arena policy — `anonymous` remains
    available … but file-backed is the default for document arenas." (Decision 1.)
  - `:184-190` — **"The inside-out split is the persistence split."** "data
    buffers live in the file mapping; refcounts, free pools, generation tags, and
    the reclamation queue are anonymous runtime state, **rebuilt on open (a
    reachability walk from the live roots reconstructs counts; free slots are the
    complement)**. Nothing about the runtime bookkeeping ever hits disk." — the
    direct mandate for the typed walk + `finalize_open`.
  - `:192-197` — "in-record references are **index-only** (arena id + slot
    index); the pointer-carrying reference variant survives only as a transient
    handle." — why the walk uses `peek_index`/`set_count_index` and must not
    assert `SlotRef` generations (`refs.hpp:375-378`).
  - `:205-216` — **"Checkpointing rides the version model."** "consistency needs
    only ordering, LMDB-style: msync data chunks, then publish the root by
    flipping an A/B root slot in the header, then msync the header. A crash lands
    on the old or new root, both consistent." and "Checkpoint cadence is policy
    (timer, transaction count, explicit host call); the doc 14 autosave scenario
    becomes 'msync + root flip'." — Decisions 2, 3.
  - `:163-166` — "Recovery is: map the file, read the last valid root, resume. An
    editor crash costs at-most-since-last-checkpoint, not the document."
  - `:209-213` — the durability-epoch quarantine: "a slot freed *after* the last
    durable checkpoint may still be referenced by the on-disk root, so
    reclamation quarantines freed slots per **durability epoch**." — why the slot
    fence installs on both document stores.
- **doc 14 § "The central decision: versions, not mutations"**
  (`14-data-model-and-editing.md:50-72`): `DocState` is the path-copying map
  `ObjectId → immutable record`; compositions name layers, layers name content,
  compositions name spill chunks — all **by `ObjectId` value, never an owning
  edge** (`:58-64`). Fixes the shape the walk traverses (HAMT edges only).
- **doc 14 § scenarios → Autosave** (`:230-231`): "pin, serialize (doc 08) on a
  background thread, unpin." — the JSON belt; the workspace checkpoint is the
  doc-15 suspender.
- **doc 17 § levelization** (`17-internal-components.md:41-43,49,52`): the rule
  "A component may depend only on strictly lower levels"; `arbc::pool` is L1
  ("checkpoint protocol"), `arbc::model` is L2 (`Depends on: base, pool, media`).
  This task's driver lives in `model`, drives the `pool` seam downward — the
  allowed edge, CI-gated by `scripts/check_levels.py` (`"model": {"base","pool","media"}`).
- **doc 16** — claims register (`:14-21`); behavioral-counter tests over
  wall-clock (`:54-62`, "slots allocated/reclaimed"); **crash-recovery tests**
  (`:74-78`, "a fault-injection shim over msync/write/mmap drives
  kill-at-every-syscall sweeps … remap and verify a consistent root"); diff
  coverage ≥90% (`:112-118`).

Source seams (verified):

- `src/pool/arbc/pool/checkpoint.hpp` — `Checkpointer:87`, ctor `:94` (installs
  the chunk fence; caller installs the slot fence via `slot_fence()`, `:92-93,124`),
  `commit:133` (contract `:126-132`), `open:185` (contract `:173-177`),
  `OpenState:178-183`, `finalize_open:205` (contract `:202-204`), behavioral
  counters `:210-216` (`commit_count`, `data_msyncs`, `header_msyncs`,
  `slots_freed_to_list`, `epoch`, `durable_epoch`, `generation`),
  `DurabilityEpochFence:58`, `WorkspaceRoot:38-50`.
- `src/pool/arbc/pool/refs.hpp` — `set_count:351` (`:348-350`), `restore:361`
  (`:355-360`), raw-index walk primitives `peek_index:379`, `set_count_index:380`,
  `count_index:383` (contract `:375-378`).
- `src/pool/arbc/pool/slot_store.hpp` — `ReleaseFence:59` (`:51-65`),
  `Arena:337`, `Arena():339`, `Arena(ChunkSource&):340`, `store_for:350`,
  `for_each_store:359`, `total_slots_live:354`; `SlotStore::set_release_fence:166`,
  `free_now:174`, `reserve_restored:186`, `finalize_restore:191`,
  `for_each_sealable_chunk:199`.
- `src/pool/arbc/pool/workspace_file.hpp` — `WorkspaceFileChunkSource`,
  `ChunkReleaseFence:110`.
- `src/model/arbc/model/model.hpp` — `Model:169`, `Model():171`, `current():179`,
  `allocate_id():182`, `drain():187`, `live_slots():192`, `DocRoot:37`,
  `Arena d_arena:426`, `RefStore<HamtNode> d_nodes:427`,
  `RefStore<ObjectRecord> d_records:428`, `std::atomic<DocStatePtr> d_current:436`.
- `src/model/arbc/model/hamt.hpp` — `HamtNode:88-97` (children
  `SlotRef<HamtNode> children[k_hamt_arity]` + leaf `SlotRef<ObjectRecord> record`
  — the counted edges the walk follows).
- `src/model/arbc/model/records.hpp` — `ObjectRecord:151-164` (tagged union;
  `LayerOrderChunk` arm at `:158`), `CompositionRecord:136-144` (layers + spill
  by `ObjectId` value), `ContentRecord:60`, `StateHandle:49-55`, spill-by-value
  note `:104-105`.

Predecessor decisions carried forward (`tasks/refinements/model/*.md`,
`tasks/refinements/pool/*.md`): records are index-only / position-independent /
trivially destructible (the three object arms) so file-backing is safe; the
`DocState` ownership graph is the HAMT and records name each other by `ObjectId`
value; behavioral counters (`live_slots()`, `Checkpointer` counters) over
wall-clock; asan-lane concurrency smoke, seeded stress parked in
`quality.stress_harness`; **`HamtNode` is deliberately *not* trivially
destructible** — it releases child `SlotRef` edges in the reclaim cascade
(`persistent_state.md:357`) — a *runtime-reclamation* concern orthogonal to file
backing, since recovery reconstructs counts by walking bytes and constructs /
destructs nothing (Decision 6).

## Constraints / requirements

- **Levelization (CI-gated, `scripts/check_levels.py`, `"model": {"base","pool","media"}`).**
  The driver and walk live in `arbc::model` (L2) and drive the `arbc::pool` (L1)
  `Checkpointer`/`Arena`/`RefStore` seams **downward** — the allowed edge. **No
  new dependency edge**; `pool` must not learn about `DocState` (the reason the
  walk is model-owned). Doc 17:41-43,49,52.
- **The anonymous path stays byte-identical.** `Model()` keeps its default
  `Arena()` (anonymous source), installs no `Checkpointer`, installs no slot
  fence — the `DurabilityEpochFence` is "off by default so anonymous paths are
  byte-for-byte unchanged" (`checkpoint.hpp:52-57`). Every existing `Model`/model
  test and every live-only host is unaffected.
- **Both document stores are workspace-backed and fenced.** `d_nodes` (HamtNode)
  and `d_records` (ObjectRecord) are both minted from the file source, and the
  slot fence installs on **both** — a freed HamtNode or ObjectRecord slot that
  still backs the on-disk root must be quarantined until the next checkpoint
  makes the freeing durable (doc 15:209-213). The walk therefore produces **two**
  `live_set` vectors and calls `finalize_open` once per store.
- **The walk uses raw-index primitives and asserts no `SlotRef` generation.**
  It reads records by storage index (`peek_index`), follows the in-place
  `SlotRef::index()` child edges, and writes counts with `set_count_index`
  (`refs.hpp:375-385`). Generations are anonymous and reset on open; the walk
  must not depend on them.
- **Counts are reconstructed, never read from disk.** `restore` starts every
  count at zero (`refs.hpp:355-360`); the walk sets each reached slot's count to
  its in-degree among reachable edges; `finalize_open` derives the free list as
  the below-high-water complement (`checkpoint.hpp:202-204`). Nothing on disk is
  trusted for bookkeeping (doc 15:184-190).
- **Root index for `commit`.** `checkpoint()` passes the current published
  `DocState` root's HamtNode slot index (the slot `DocRoot`'s `Ref` names,
  `model.hpp:37`) to `Checkpointer::commit`. `Model` tracks that index alongside
  `d_current`.
- **Recovery is writer-only, single-threaded.** `Model::open` runs before any
  reader pins the recovered version; `checkpoint()` is writer-thread-only and
  mutates no live record (it msyncs + flips the header — live records are
  immutable), so a concurrent pinned reader sees a consistent version throughout.
- **Coverage:** ≥90% diff coverage; `scripts/gate` green (build + asan +
  `check_levels.py` + `check_claims.py`) before commit (user rule: always build
  and test before committing).

## Acceptance criteria

**Unit tests** (`src/model/t/workspace_backing.t.cpp`, wired via
`arbc_component_test(COMPONENT model …)` in `src/model/CMakeLists.txt`, Catch2,
matching `persistent_state.t.cpp` style; workspace files created under the test's
temp dir):

- Build a document (a composition with inline layers, layers past
  `k_max_inline_layers` forcing a spill chain, content records), `checkpoint()`,
  drop the `Model`, `Model::open(path)`, and assert the recovered `DocState` is
  **field-identical** to the committed one: every `ObjectId` resolves through the
  recovered `current()` to a record with identical fields (composition
  canvas/order/spill, layer transform/opacity/content id, content kind), and
  `live_slots()` equals the pre-crash value (the walk rebuilt exactly the
  reachable slots — no leak, no under-count).
- **Counts are correct, not merely nonzero:** after recovery, run one
  transaction that drops the last pin on the recovered version + `drain()`, and
  assert `live_slots()` returns to the shared-node count — i.e. reclamation frees
  exactly the unique slots, proving the walk's in-degrees were right (a corrupted
  count would either leak or double-free, both observable).

**Behavioral-counter assertions (doc 16:54-62 — counters, never wall-clock),**
over the `Checkpointer` counters (`checkpoint.hpp:210-216`) and `live_slots()`:

- A `checkpoint()` of an **unchanged** scene issues **zero** data msyncs but
  **one** header msync (`data_msyncs` delta 0, `header_msyncs` delta 1) — the
  "unchanged scene skips the data msync … but still flips + syncs the header"
  contract (`checkpoint.hpp:130-132`).
- The durability-epoch fence quarantines: a slot freed after a checkpoint is not
  returned to the free list until the next `checkpoint()` — `slots_freed_to_list`
  advances only at commit, and a `peek`/`resolve` of the freed slot stays valid
  in the interim (doc 15:209-213).

**Crash-recovery tests (doc 16:74-78, doc 15).** Reuse the pool fault-injection
shim from `pool.crash_tests` (`tasks/refinements/pool/crash_tests.md`) over
msync/write/mmap: drive a `checkpoint()` under a kill-at-every-syscall sweep;
after each injected death, `Model::open(path)` and assert a consistent root —
a death **before** the header msync recovers the **prior** committed version, a
death **after** recovers the **new** one, both field-consistent and with correct
rebuilt counts (SQLite/LMDB discipline). Disk-full / short-file paths surface as
`WorkspaceFileError` values from `open`, not crashes.

**Claims (register in `tests/claims/registry.tsv`; enforce with an
`// enforces: <claim-id>` tagged test — `scripts/check_claims.py` gates both
directions):**

- `15-memory-model#counts-rebuilt-by-reachability-walk` — after `Model::open`,
  every reachable slot's refcount equals its live in-degree, reconstructed by the
  model's typed HAMT walk with no count read from disk; witnessed by
  post-recovery reclamation freeing exactly the unique slots (behavioral-counter
  test; realizes doc 15:184-190).
- `15-memory-model#recovery-resumes-last-durable-root` — reopening a workspace
  file after a crash at any syscall in the checkpoint protocol yields the last
  durable committed version, field-identical; a crash before the header msync
  lands on the prior root (realizes doc 15:163-166,205-209).
- `15-memory-model#checkpoint-of-still-scene-skips-data-msync` — a `checkpoint()`
  of an unchanged scene issues zero data msyncs and one header msync
  (behavioral-counter test; realizes doc 15:213-216, `checkpoint.hpp:130-132`).

**Concurrency (doc 16 tier 6).** Extend the pin/traverse-vs-writer smoke
(`persistent_state.t.cpp`) so the writer also runs `checkpoint()` cycles while a
background thread pins the current version and traverses via `peek`; assert no
torn read and no use-after-free under the **asan lane** (no TSan preset in-tree
yet — the parked convention from `tasks/refinements/pool/reclamation.md`; full
seeded schedule-perturbation stress remains `quality.stress_harness`,
`tasks/70-quality.tji`, not duplicated here). `checkpoint()` mutates no live
record, so the invariant is that a reader pinned across a checkpoint observes an
unchanged, consistent version.

**Not applicable — contract conformance suite / byte-exact goldens.** This task
adds no content *kind* or operator, so the kind/operator conformance suite does
not apply. It produces no pixels, so byte-exact render goldens do not apply; the
workspace *file* is a same-machine session artifact with no portability promise
(native endianness/padding, doc 15:197-203), so it is **not** a determinism
golden either — structural round-trip equality + behavioral counters pin the
behavior instead.

**Deferred follow-up.** None new as a WBS leaf. The one deferred concern —
recovering **kind-owned persistent content-state slabs** (e.g. a raster's tile
table) through the walk when the first such kind ships — is left inert here
because no kind produces workspace-backed content-state in v1 (`StateHandle`s are
`k_state_none`), and *which* content persists to the workspace vs. re-derives is
a product/design judgment, not mechanical work. The walk is built so a per-kind
state-slab hook (mirroring the existing writer-owned `StateRefSink` seam,
`model.hpp:130`) slots in without rework; wiring the concrete hook is the natural
job of the first persistent-state kind's own task. Surfaced to the parking lot
(not encoded as a WBS leaf, per doc 16 — it needs a design call, and there is no
consumer to close it against today).

## Decisions

1. **File backing is a construction-time `Model` mode; anonymous stays the
   default.** A `Model` built with a workspace path owns a
   `WorkspaceFileChunkSource`, constructs its `Arena` over it
   (`Arena(ChunkSource&)`, `slot_store.hpp:340`), owns a `Checkpointer`, and
   fences both stores; `Model()` is unchanged (anonymous, no checkpointer, no
   fence). *Rationale:* doc 15:158-160 makes backing "a construction-time arena
   policy — anonymous remains available (a live-only OBS-style host may want no
   files)"; the pool `Arena` already exposes exactly the two constructors; keeping
   `Model()` untouched means zero blast radius on existing tests/hosts and the
   fence stays "off by default … byte-for-byte unchanged" (`checkpoint.hpp:52-57`).
   *Rejected:* making all `Model`s file-backed (breaks live-only hosts, forces a
   temp file on every unit test); a runtime `set_backing()` mutator (backing must
   be fixed before the first `store_for` mints a size-class store —
   `slot_store.hpp:345-351` — so it is inherently a construction-time property).

2. **A checkpoint makes the current version durable via `Checkpointer::commit`;
   recovery restores exactly that version.** `Model::open` seeds the walk from
   the single durable A/B root `Checkpointer::open` selects. *Rationale:* the
   header publishes one root (`WorkspaceRoot`, `checkpoint.hpp:38-50`); doc
   15:163-166 defines recovery as "read the last valid root, resume." Prior
   versions' shared structure physically remains in the file
   (doc 15:163 — "records of every checkpointed version") but only the current
   root is published, so the in-memory journal / undo history is **not**
   reconstructed across a close/open in v1 — consistent with "crash costs
   at-most-since-last-checkpoint, not the document" (undo-across-sessions is not a
   documented guarantee). *Rejected:* persisting a multi-root manifest to recover
   the journal's pinned versions (genuinely more scope, no doc mandate, and a
   product question about whether cross-session undo is even wanted — surfaced to
   the parking lot, not built speculatively). *No design-doc delta:* restricting
   recovery to the durable root is *within* doc 15's "read the last valid root,
   resume", not a deviation.

3. **`checkpoint()` is a decoupled primitive; per-transaction publish does not
   checkpoint.** The atomic `d_current` swap stays per-transaction and
   msync-free; a separate writer-only `Model::checkpoint()` drives
   `Checkpointer::commit`. *Rationale:* checkpointing on every version publish
   would msync per keystroke; doc 15:213-216 is explicit that "checkpoint cadence
   is policy (timer, transaction count, explicit host call)". The task note's
   "commit on autosave/publish" is realized as the primitive the host's autosave
   cadence and explicit save/publish actions drive — not a hook on the internal
   version publish. *Rejected:* checkpointing inside every `commit` (destroys the
   lock-free, allocation-only hot commit path with synchronous msyncs; violates
   the cadence-is-policy mandate).

4. **The walk is an in-degree accumulation over the HAMT ownership DAG,
   expanding each node once.** Starting from the durable root HamtNode (plus the
   `DocRoot`'s synthetic +1 on it), a worklist traversal: for every counted
   `SlotRef` edge encountered, `set_count_index(child, count_index(child)+1)`;
   each node is *expanded* (its children enqueued) only on first discovery.
   *Rationale:* this yields each slot's exact in-degree among reachable edges and
   terminates on shared structure; it needs only the raw-index primitives
   `restore` leaves at zero (`refs.hpp:355-385`). The ownership graph *is* the
   HAMT — all `ObjectRecord` arms are leaves referenced only by `HamtNode` edges,
   records name each other by `ObjectId` value (`records.hpp:104-105`,
   doc 14:58-64) — so no cross-record edges exist to follow. *Rejected:*
   assuming every count is 1 (wrong wherever genuine sharing exists, e.g. future
   shared content-state); a mark-then-recount two-pass (the increment-per-edge /
   expand-once single pass is simpler and already correct).

5. **`StateHandle` descent is a dormant seam in v1.** The walk visits each
   `ContentRecord`'s `StateHandle` but descends nothing: v1 handles are inert
   (`k_state_none`, `records.hpp:49-55`), and content-state slabs are kind-owned
   in kind-owned stores whose shape the model cannot know. *Rationale:* no kind
   ships persistent workspace-backed state yet, so there is nothing to walk;
   building a speculative slab-walk with zero call sites overreaches (bias toward
   the abstraction with call sites today). The hook mirrors the existing
   `StateRefSink` retain/release seam so it slots in when a persistent kind lands.
   *Rejected:* a fully-wired `StateReachabilitySink` now (no consumer, unknown
   slab shapes); ignoring the handle entirely (would force a rewrite of the
   per-record visit when the first persistent kind arrives).

6. **`HamtNode`'s non-trivial destructor does not block file backing.** The nodes
   persist as bytes (index-only `SlotRef`s, position-independent, doc 15:192-197);
   recovery `restore`s the chunks and rebuilds counts by walking — it constructs
   and destructs nothing. The destructor only runs during the *live-editing*
   reclaim cascade (`persistent_state.md:357`), which is unchanged. *Rationale:*
   file backing touches storage, not object lifetime; the checkpoint-compatibility
   the records already satisfy (index-only, standard-layout) is exactly what
   backing needs. *Rejected:* forcing `HamtNode` trivially destructible (would
   break the cascade's child-edge release the reclamation design depends on).

7. **No design-doc delta.** The task *realizes* behavior docs 14/15 already
   mandate — file-backed arena as a construction-time policy (15:158-160),
   checkpoint rides the version model (15:205-216), counts rebuilt by a
   reachability walk (15:184-190), single-root crash recovery (15:163-166) — and
   introduces no new seam, dependency, or deviation. *Precedent:*
   `model.persistent_state`, `model.editable_facet`, and `pool.checkpoints` all
   shipped with no delta when implementing already-normative behavior; a delta is
   reserved for genuinely new behavior (as `model.transactions` needed for Abort).

## Open questions

(none — all decided)

## Status

**Re-deferred** — 2026-07-09.

The task could not proceed. A blocking format gap in `arbc::pool` makes the
load-bearing deliverable — `Model::open` cold recovery — unrealizable under the
refinement's "no new `arbc::pool` code" constraint. Specifically:

- `WorkspaceHeader` (`workspace_file.hpp:59-72`) records only a **global**
  `chunk_count`; `WorkspaceChunkEntry` (`:77-82`) carries `{offset,size,state}`
  with **no store owner** — the chunk directory is not publicly exposed.
- On reopen, `acquire` serves chunks **size-blind FIFO** (`workspace_file.cpp:231-234`),
  so `d_records.restore()` then `d_nodes.restore()` mis-routes interleaved chunks:
  a records store handed a node-sized chunk mis-strides and the walk reads garbage.
- Chunk byte-size cannot disambiguate in the ASan/debug lane: `HamtNode`
  (stride 288→128 slots) and `ObjectRecord` (stride 144→256 slots) both yield
  **36864-byte** chunks in debug.
- The doc 15:199-203 header field that would close this ("per-type slot sizes,
  and arena directory") was never implemented.

No files were created or edited. No tests were added. No commit was made.

See `tasks/parking-lot.md` entry 2026-07-09 —
`model.workspace_backing` blocked: per-store chunk routing absent in workspace format.
