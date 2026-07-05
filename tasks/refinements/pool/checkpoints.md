# pool.checkpoints — Checkpoint protocol

## TaskJuggler entry

`tasks/05-pool.tji` → `pool.checkpoints` ("Checkpoint protocol").

## Effort estimate

3d.

## Inherited dependencies

- `pool.mmap_backing` — **settled** (commit `22a1c71`). Provides the on-disk
  contract this task turns into a protocol:
  - `WorkspaceFileChunkSource` — one `MAP_SHARED` mapping per data chunk,
    `ftruncate` growth with stable addresses, `fallocate(PUNCH_HOLE)`
    release (`src/pool/arbc/pool/workspace_file.hpp:115`,
    `:125-126`; impl `src/pool/workspace_file.cpp:131-196`).
  - `struct WorkspaceHeader` with the **two reserved A/B root slots**
    `root_slot_a` / `root_slot_b` (written zero at create) plus
    `data_offset` / `chunk_count`
    (`src/pool/arbc/pool/workspace_file.hpp:57-70`;
    `src/pool/workspace_file.cpp:103-104`). mmap_backing's decision "header
    owns the root slots now, protocol later"
    (`tasks/refinements/pool/mmap_backing.md:109-112`) means this task
    changes **no on-disk layout**, only behavior.
  - The on-disk chunk directory `struct WorkspaceChunkEntry` and its
    `k_workspace_chunk_free` / `k_workspace_chunk_live` states
    (`src/pool/arbc/pool/workspace_file.hpp:75-85`) — the map used to
    enumerate live chunks for `msync` and rebuild bookkeeping on open.
  - `WorkspaceFileChunkSource::read_header(path)` — reopen-and-validate
    entry point (`src/pool/arbc/pool/workspace_file.hpp:131`;
    `src/pool/workspace_file.cpp:198`). mmap_backing explicitly reserved
    "opening an existing file, root discovery, and bookkeeping rebuild" for
    this task (`tasks/refinements/pool/mmap_backing.md:25-28`).
  - The strict inside-out split: only immutable data chunks are file-backed;
    refcounts, generation tags, free lists, and the reclaim-link stack are
    anonymous and never persisted
    (`tasks/refinements/pool/mmap_backing.md:65-68`, doc 15:162-168).
- `pool.reclamation` — **settled** (commit `c5b828e`). Provides the
  reclaim→free-list boundary this task fences:
  - `RefStore<T>::reclaim(SlotIndex)` runs `~T` then returns the slot to its
    `SlotStore` free list (`src/pool/arbc/pool/refs.hpp:344`), driven by
    `ReclamationQueue::drain()` to quiescence
    (`src/pool/arbc/pool/reclamation.hpp:59-103`).
  - reclamation deliberately "routes reclaim straight to `SlotStore::release`
    and leaves that interposition point clean; checkpoints interposes the
    quarantine buffer there"
    (`tasks/refinements/pool/reclamation.md:268-275`).
  - The count-reconstruction seam `RefStore<T>::set_count`
    (`src/pool/arbc/pool/refs.hpp:333-338`) — the documented hook for
    rebuilding refcounts by a reachability walk on workspace open.
- `pool.arena_core` — **settled** (commit `32c0d5d`). Provides
  `SlotStore::release` (writer-only, LIFO free list outside data pages,
  `src/pool/arbc/pool/slot_store.hpp:73`, `:100-101`) — the sink the
  durability fence sits in front of.

## What this task is

The durability protocol for the mmap workspace file (design doc 15,
"File-backed arenas: mmap instead of process memory", lines 134-199): make a
crash cost *at most the work since the last checkpoint*, never a corrupt
document. Concretely, four coupled mechanisms, all inside `arbc::pool`:

1. **Ordered commit (LMDB-style A/B root).** A checkpoint (a) `msync`s every
   live data chunk, (b) publishes the new root by writing it into the *other*
   of the two header root slots and flipping which is current, (c) `msync`s
   the header. Because live records are never overwritten (immutability), a
   crash at any point lands on the old *or* the new root — both internally
   consistent (doc 15:183-186).

2. **Durability-epoch quarantine of freed slots.** A slot freed *after* the
   last durable checkpoint may still be referenced by the on-disk root, so
   its data bytes must not be overwritten by a new allocation until the
   freeing is itself durable. Reclamation's reclaim→free-list step is fenced:
   freed slots (and emptied chunks) are stamped with the current epoch and
   held; a checkpoint that makes their freeing durable releases them
   (doc 15:187-191).

3. **Recovery = map, read root, resume.** Opening an existing workspace file
   validates the header, selects the newest valid root, and drives the
   anonymous-bookkeeping rebuild (refcounts and free list are the complement
   of a reachability walk from the root — doc 15:142-145, 162-168).

4. **Debug hardening: `mprotect` of published chunks.** In debug builds, data
   chunks published by a checkpoint are `mprotect`ed read-only, so a stray
   write through a stale pointer faults at the write site rather than
   silently corrupting a checkpointed document (doc 15:196-199).

The protocol lives in a new `Checkpointer` bound to an `Arena` + its
`WorkspaceFileChunkSource`; `WorkspaceFileChunkSource` gains only the minimal
`msync`/root-access helpers the protocol calls (no layout change). Because
the graph walk that reconstructs refcounts needs record-shape knowledge that
is a **model-layer** concern (doc 14 node types, L2), this task ships the
recovery *mechanism* and reconstruction seams and proves them with a
test-local record graph standing in for the model; the typed walk over real
document nodes is the model stream's job (see Decisions).

## Why it needs to be done

- It is the payoff of the whole file-backed-arena line: mmap_backing built
  the file and reserved the root slots; without the checkpoint protocol the
  workspace file has no crash-recovery value (doc 00:152-161 lists "crash
  recovery" as the first motivation for mmapping arenas).
- Direct downstream WBS consumers gate on it:
  - `pool.crash_tests` (`tasks/05-pool.tji:60-64`, depends `!checkpoints`)
    builds the fault-injection shim over `msync`/`write`/`mmap` and the
    kill-at-every-syscall sweep — it needs the protocol to fault-inject.
  - `runtime.housekeeping` owns **checkpoint cadence** (timer / transaction
    count / explicit host call, doc 15:191-193); this task must expose commit
    as a callable *mechanism*, not schedule it (doc 17: cadence is L5
    `arbc::runtime`, the protocol is L1 `arbc::pool`).
  - The model stream's workspace-open path (doc 14 / `model.persistent_state`
    neighborhood) drives the typed reachability walk over real nodes through
    the reconstruction seams this task exposes.
  - M1 (`tasks/99-milestones.tji`, "Memory foundation") depends on
    `checkpoints` / `crash_tests` / `benchmarks`.

## Inputs / context

- `docs/design/15-memory-model.md`:
  - **"Checkpointing rides the version model"** (lines 183-194) — the
    normative core: "consistency needs only ordering, LMDB-style: msync data
    chunks, then publish the root by flipping an A/B root slot in the header,
    then msync the header. A crash lands on the old or new root, both
    consistent." Plus the slot-reuse hazard and the "reusable after
    checkpoint N" durability-epoch fence.
  - **Crash recovery** (lines 142-145): "Recovery is: map the file, read the
    last valid root, resume."
  - **The persistence split** (lines 162-168): data buffers live in the file;
    refcounts, free pools, generation tags, and the reclamation queue are
    anonymous, rebuilt on open by a reachability walk from the live roots;
    "Nothing about the runtime bookkeeping ever hits disk."
  - **Position independence** (lines 170-181): in-record refs are index-only
    `SlotRef`; the header carries "layout schema version, per-type slot sizes,
    and arena directory."
  - **Debug `mprotect`** (lines 196-199): published data chunks `mprotect`ed
    read-only between transactions in debug builds.
  - **Thread rules** (lines 115-121): "The writer thread is the only
    structural allocator"; RT threads may only pin/unpin + enqueue — so
    `msync`/commit/fence-drain work is writer-thread-only.
  - **Version reclamation** (lines 90-114): the deferred-reclamation queue the
    epoch fence extends.
- `src/pool/arbc/pool/workspace_file.hpp` — `WorkspaceHeader` A/B root slots
  (`:57-70`), `WorkspaceChunkEntry` + states (`:75-85`), `read_header`
  (`:131`), `debug_assert_position_independent` (`:173`); impl
  `src/pool/workspace_file.cpp:103-104` (root slots zeroed), `:131-196`
  (`acquire`/`release` — the `msync`-ordering insertion points).
- `src/pool/arbc/pool/refs.hpp` — `reclaim` (`:344`), `release_index`
  (`:475-479`), the `set_count` reconstruction seam (`:333-338`).
- `src/pool/arbc/pool/reclamation.hpp:59-103` — `ReclamationQueue` (the drain
  the fence is drained alongside).
- `src/pool/arbc/pool/slot_store.hpp:73,100-101` — `SlotStore::release` and
  the LIFO free list the fence guards.
- `src/pool/t/workspace_file.t.cpp:193-227` — the existing hole-punch +
  `::msync(..., MS_SYNC)` test pattern to mirror; `:138` notes the root slots
  are reserved for this task.
- `tasks/refinements/pool/mmap_backing.md`, `tasks/refinements/pool/reclamation.md`
  — the layout-contract and clean-seam decisions this task realizes.
- `docs/design/17-internal-components.md` — `arbc::pool` is L1, depends only
  on `arbc::base`; checkpoint cadence is `arbc::runtime` (L5).
- `docs/design/16-sdlc-and-quality.md` — claims register (`enforces:` tags,
  lines 15-21), behavioral counters (lines 54-62), crash-recovery tier
  (lines 74-78, the exhaustive sweep is `pool.crash_tests`), TSan
  (lines 66-73), ≥90% diff coverage (lines 112-118).
- `tests/claims/registry.tsv` + `scripts/check_claims.py` (run from
  `scripts/gate:42`) — where new `15-memory-model#…` claims register.

## Constraints / requirements

- **Ordered commit, never reordered.** `Checkpointer::commit()` performs, in
  this exact order: (1) `msync(MS_SYNC)` every live data-chunk mapping;
  (2) write the new root record into the currently-*inactive* root slot and
  set it active (the flip); (3) `msync(MS_SYNC)` the header mapping. The
  ordering is the correctness invariant — the data a root points to is durable
  before the root that publishes it. `WorkspaceFileChunkSource` gains the
  minimal helpers to enumerate live data mappings for step 1 and to reach the
  header page for steps 2-3; it adds **no** on-disk layout.
- **Self-validating, atomically-published root record.** Each root slot holds
  a naturally-aligned root record `{ generation:u32, root:SlotRef(u32) }`
  packed into the reserved slot, published by a single naturally-aligned
  store (single-sector, torn-write-free). Recovery selects the slot with the
  **higher generation** that validates; `generation == 0` means "never
  written" (the create-time zero). The flip is "bump generation, write the
  inactive slot" — the two slots alternate, so a crash always leaves at least
  one fully-written prior root. If the reserved slot proves too narrow for a
  future root record, add a trailing checksum (deferred, not needed now — see
  Decisions).
- **Durability-epoch quarantine sits between reclaim and free-list.** A
  `DurabilityEpochFence` (owned by the `Checkpointer`) is installed on the
  workspace-backed `Arena`'s `SlotStore`s via a new
  `SlotStore::set_release_fence(ReleaseFence*)` seam (default `nullptr` →
  today's direct push to the free list, so anonymous/arena-only usage is
  unchanged). When installed, `SlotStore::release(index)` diverts the slot to
  the fence stamped with the current epoch instead of pushing it to `d_free`;
  the fence returns it via a new un-fenced `SlotStore::free_now(index)` only
  once a checkpoint has made the freeing durable. This is the clean
  interposition point reclamation left
  (`tasks/refinements/pool/reclamation.md:268-275`).
- **Single-checkpoint fence, justified by last-valid-root selection.** A slot
  freed while epoch = N is released to the free list once `commit()`
  completes the checkpoint that publishes root generation N (i.e. once
  `durable_epoch >= N`). One checkpoint suffices — not two — because recovery
  always selects the **highest-generation** valid root: after checkpoint N
  the newest root excludes the freed slot, and the older slot's still-durable
  root (which references it) is never selected while the newer one validates.
  Never-overwrite-live keeps that older root internally consistent until it is
  superseded (see Decisions for why this matches doc 15's singular "reusable
  after checkpoint N").
- **Data bytes of a freed slot are never mutated pre-durability.** Records are
  trivially destructible (`tasks/refinements/pool/refs.md`), so `reclaim`
  running `~T` touches only anonymous refcount/reclaim tables, never the data
  page (`15-memory-model#refcounts-outside-data-pages`). The *only* event that
  endangers the old root is slot **reuse** overwriting the data — which the
  fence prevents. No new data-page writes are introduced on the free path.
- **Chunk hole-punch is epoch-gated too.** An emptied chunk freed after the
  last checkpoint may still back the on-disk root, so
  `WorkspaceFileChunkSource::release` (`fallocate(PUNCH_HOLE)`) is routed
  through the same fence: hole-punch of an emptied chunk is performed by the
  post-durable drain at commit, not eagerly on emptying. This keeps
  `15-memory-model#hole-punch-returns-storage` (mmap_backing) true while
  making the punch durable-safe.
- **Recovery is map → validate → select → rebuild, mechanism only.**
  `Checkpointer::open(path, Arena&)` maps the file, validates the header
  (magic/format via `read_header`), selects the durable root, and returns the
  root `SlotRef` with counts at zero and free lists empty (a
  "rebuild-in-progress" state). The caller (model layer) walks the graph from
  the root, incrementing counts through `RefStore<T>::set_count`
  (`src/pool/arbc/pool/refs.hpp:333-338`); a
  `Checkpointer::finalize_open(live_set)` then repopulates each `SlotStore`
  free list with the below-high-water complement of the live set. Nothing on
  disk is trusted for bookkeeping (doc 15:162-168). The **typed walk over real
  document nodes is out of scope** (model-layer, L2 — pool must not depend up).
- **Debug `mprotect` of published chunks.** In debug builds, after `commit()`
  publishes the data chunks, every live data-chunk mapping is `mprotect`ed
  `PROT_READ`; a freshly-`acquire`d chunk maps writable and is flipped to
  read-only at the checkpoint that publishes it. A stray write through a stale
  pointer then faults at the write site. Compiled out in release (`#ifndef
  NDEBUG`), guarded to POSIX (`ARBC_HAS_WORKSPACE_FILES`).
- **Writer-thread-only.** `commit()`, the fence drain, and recovery run on the
  writer thread (doc 15:115-121). RT threads never `msync`, never checkpoint.
  Checkpointing serializes with reclamation drain (both writer-only) so epoch
  stamping is consistent.
- **Errors as values.** All syscall failures (`msync`/`ftruncate`/`fallocate`/
  `mprotect`/`pread`) surface as `arbc::expected` with errno context, never an
  abort — consistent with mmap_backing. Disk-full/short-file *handling* is
  proven here at focused injection points; the exhaustive kill sweep is
  `pool.crash_tests`.
- **No design-doc delta.** Doc 15 already designs A/B roots, ordered `msync`,
  the durability-epoch fence, recovery, and debug `mprotect`; this task turns
  those promises into a concrete protocol without changing designed behavior,
  so no `docs/design/` edit is required (same rule mmap_backing/reclamation
  applied).
- **Levelization.** Everything lands in `arbc::pool` (`arbc_pool`): a new
  `src/pool/arbc/pool/checkpoint.hpp` (+ impl in `src/pool/checkpoint.cpp` if
  non-header-only) and the minimal `set_release_fence`/`free_now` +
  `msync`/root helpers on existing headers. Depends only on `base` (doc 17,
  CI-enforced). No new component edge.

## Acceptance criteria

- **Unit tests** (`src/pool/t/checkpoint.t.cpp`, wired into
  `src/pool/CMakeLists.txt`'s `arbc_component_test` alongside the existing
  pool tests):
  - Commit round-trip: build a small graph in a workspace file, `commit()`,
    `read_header` sees the active root slot advanced (generation bumped, other
    slot older); `open()` + a test-local walk resolves the same graph.
  - A/B alternation: two successive commits write *different* root slots and
    the newer generation wins on `open()`.
  - Fence basics: with a `DurabilityEpochFence` installed, freeing a slot does
    **not** return it to the free list (next `create` does **not** reuse it);
    after a `commit()` that makes the freeing durable, the slot is reused.
    Without a workspace (anonymous arena, no fence) freeing returns to the free
    list immediately (arena_core behavior unchanged).
  - Recovery rebuild: `open()` yields counts-at-zero + empty free lists; after
    the test-local walk + `finalize_open`, live counts match the pre-crash
    graph and the free list is exactly the below-high-water complement.
- **Claim (register + `enforces:`)**
  `15-memory-model#checkpoint-recovers-consistent-root` — drive the ordered
  commit and simulate a crash at each ordering boundary (after data `msync`,
  after the root flip, after header `msync`) by reading the file's durable
  state at that point; assert recovery lands on the **old root before the
  header `msync`** and the **new root after it**, both resolving a consistent
  graph. (Focused two/three-point check here; the exhaustive
  kill-at-every-syscall sweep is `pool.crash_tests`.)
- **Claim (register + `enforces:`)**
  `15-memory-model#freed-slot-quarantined-until-durable` — a slot freed after
  the last checkpoint is **not** re-allocated (its data bytes stay intact and
  the old root still resolves it) until a `commit()` makes the freeing
  durable; then it becomes reusable. Extends to chunk hole-punch: an emptied
  chunk is not punched (its `st_blocks` unchanged, Linux-guarded) until the
  emptying is durable.
- **Claim (register + `enforces:`)**
  `15-memory-model#checkpoint-published-chunks-read-only` — in a debug build,
  after `commit()` a write through a pointer into a published data chunk
  faults (caught via a `SIGSEGV`/`sigsetjmp` harness, mirroring the
  `mprotect` witness of `15-memory-model#refcounts-outside-data-pages`);
  Linux/POSIX-guarded.
- **Behavioral-counter assertions** (doc 16, never wall-clock): a `commit()`
  of an unchanged scene (no new/freed slots) issues zero data-chunk `msync`s
  beyond the header flush and frees zero slots; the fence's release counter
  advances only at commit, never on the free path; the epoch counter advances
  exactly once per successful commit.
- **Concurrency (TSan, explicit per doc 16)**: a smoke where RT-role threads
  enqueue reclamation while the writer thread drains and checkpoints — the
  fence stamps/releases without data races, every destructor fires once, and
  recovery of the resulting file returns the expected graph. Runs clean under
  the asan lane; the TSan lane runs it if/when present (this repo has no TSan
  preset today — same gap noted in `tasks/refinements/pool/reclamation.md:305`;
  not re-litigated here).
- **Coverage**: ≥90% diff coverage on changed lines; gate green including asan
  (asan + `MAP_SHARED`/`mprotect` interplay verified, as mmap_backing did).
- **Deferred follow-up** (closer registers in WBS): the Windows ordered-commit
  port — `pool.checkpoints_win32` (est. 1d): `FlushViewOfFile` +
  `FlushFileBuffers` in place of `msync`, and `VirtualProtect` for the debug
  read-only publish, behind `ARBC_HAS_WORKSPACE_FILES`. Depends
  `!checkpoints, !mmap_backing_win32`; wired into `m9_release` (alongside
  `pool.mmap_backing_win32`).

## Decisions

- **Single-checkpoint durability fence, not two.** doc 15:187-191 phrases the
  fence as singular ("reusable after checkpoint N"), and a single checkpoint
  is sufficient here because recovery selects the **highest-generation valid
  root** (`15-memory-model` "read the *last* valid root", doc 15:143). After
  checkpoint N the newest durable root excludes the freed slot; the older
  A/B-sibling root that still references it is never selected while the newer
  one validates, and never-overwrite-live keeps that older root internally
  consistent until it is overwritten. *Rejected:* LMDB's conservative
  "reusable after two transactions" — LMDB needs it because its two meta pages
  are *both* candidate recovery targets chosen by max-txnid where a reader can
  pin the older; here the writer is the only allocator, there are no
  long-lived readers pinning an old on-disk root across a checkpoint, and
  max-generation selection makes the extra checkpoint dead conservatism.
- **Root record is a single aligned word, published atomically; no checksum
  yet.** A `{generation:u32, root:SlotRef(u32)}` fits a naturally-aligned
  8-byte store, which is torn-write-free within a sector, so the higher
  generation reliably wins with no checksum. *Rejected:* a checksummed root
  record now — extra bytes and a hash on the commit hot path for a torn-write
  class that single-sector aligned stores do not exhibit; if a future root
  record outgrows the reserved slot, a trailing checksum is the additive
  change (noted, not built — no untested scaffolding, doc 16).
- **Fence at `SlotStore::release`, mirroring `set_zero_sink`.** The durability
  fence is a nullable `ReleaseFence*` on `SlotStore` (default off), exactly
  the pattern refs/reclamation used for the zero-count sink. *Rejected:*
  threading the epoch through `RefStore<T>::reclaim` per type — the free-list
  return is a `SlotStore` concern (untyped, where reuse actually happens), and
  a per-store hook keeps `reclaim` and the RT enqueue path untouched. The
  default-off design keeps every anonymous/arena-only test and the whole
  non-workspace path byte-for-byte unchanged.
- **`Checkpointer` owns the protocol; `WorkspaceFileChunkSource` stays a
  layout/backing primitive.** Ordered `msync`, root flip, epoch, fence, and
  recovery live in a new `Checkpointer`; the chunk source only gains
  enumerate-live-mappings + header-page + `msync` helpers. *Rejected:* growing
  `WorkspaceFileChunkSource` into the protocol — it would entangle the
  `ChunkSource` seam (shared with the anonymous backing) with durability
  policy; mmap_backing scoped it as the layout owner and this preserves that.
- **Recovery ships the mechanism; the typed graph walk is model-layer.** Pool
  is L1 and cannot depend on the model's record types (L2, doc 14/17), so the
  reachability walk that rebuilds refcounts is caller-driven through
  `set_count` + `finalize_open`, proven here with a test-local record graph.
  *Rejected:* implementing a generic walk in pool — it would require pool to
  know record shapes (a levelization violation) or a runtime type registry
  (machinery with no L1 consumer). The model stream drives the real walk on
  document open.
- **Chunk hole-punch is fenced with slots, not eager.** Routing
  `WorkspaceFileChunkSource::release` through the durability fence (punch at
  commit-drain) is required for correctness: an eagerly-punched chunk still
  referenced by the durable old root would read as zeros after a crash.
  *Rejected:* leaving mmap_backing's eager punch — it is safe only for
  anonymous/pre-checkpoint use; under a live workspace it must wait for
  durability.
- **Cadence stays in `runtime.housekeeping`; commit is a bare mechanism.**
  This task ships `Checkpointer::commit()` and nothing that decides *when* it
  fires, because `runtime.housekeeping` (L5) owns cadence policy
  (doc 15:191-193, doc 17). *Rejected:* a timer/txn-count trigger here — two
  owners of the schedule and a levelization inversion (L1 driving L5 policy).
- **Focused crash checks here; the fault-injection shim is `pool.crash_tests`.**
  This task pins the ordering invariant at the two/three commit boundaries
  directly; the general `msync`/`write`/`mmap` intercept shim and the
  kill-at-every-syscall sweep are `pool.crash_tests` (already a WBS leaf
  depending on `!checkpoints`, doc 16:74-78). *Rejected:* building the shim
  here — it is a distinct, larger deliverable the WBS already separates.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-04.

- New `src/pool/arbc/pool/checkpoint.hpp`: `Checkpointer` class implementing A/B-root ordered commit (`msync` data → flip inactive root slot with bumped generation → `msync` header), `DurabilityEpochFence` quarantine seam, and `open`/`finalize_open` recovery entry points.
- New `src/pool/t/checkpoint.t.cpp`: 12-case unit test covering commit round-trip, A/B slot alternation, fence quarantine/release, recovery rebuild (counts-at-zero + complement free list after `finalize_open`), and TSan concurrency smoke.
- Edited `src/pool/arbc/pool/workspace_file.hpp` + `src/pool/workspace_file.cpp`: added enumerate-live-mappings, header-page access, and `msync` helpers used by `Checkpointer`; no on-disk layout change.
- Edited `src/pool/arbc/pool/slot_store.hpp` + `src/pool/slot_store.cpp`: added `set_release_fence(ReleaseFence*)` / `free_now(index)` seam; default-`nullptr` leaves anonymous/arena paths byte-for-byte unchanged.
- Edited `src/pool/arbc/pool/refs.hpp`: wired `set_count` reconstruction seam used by recovery caller on workspace open.
- Edited `src/pool/CMakeLists.txt`: wired `checkpoint.t.cpp` into `arbc_component_test`.
- Edited `tests/claims/registry.tsv`: registered 3 claims — `15-memory-model#checkpoint-recovers-consistent-root`, `#freed-slot-quarantined-until-durable`, `#checkpoint-published-chunks-read-only`.
- Debug `mprotect` sealing of fully-published chunks lands behind `#ifndef NDEBUG`/`ARBC_HAS_WORKSPACE_FILES`; no TSan preset yet (same gap as reclamation; parked).
