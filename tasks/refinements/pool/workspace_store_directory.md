# pool.workspace_store_directory — per-store chunk directory in workspace format

## TaskJuggler entry

`tasks/05-pool.tji:78-83` → `pool.workspace_store_directory`
("Per-store chunk directory in workspace format").

## Effort estimate

2d.

## Inherited dependencies

- `pool.checkpoints` — **settled** (`complete 100`,
  `tasks/refinements/pool/checkpoints.md`): ships the A/B root protocol,
  the ordered commit (msync data → flip root → msync header), the
  durability-epoch quarantine, and the `map → validate → select → rebuild`
  recovery skeleton whose *rebuild* step this task makes correct for a
  multi-store file. Its `Checkpointer::open` /
  `Checkpointer::finalize_open` (`src/pool/arbc/pool/checkpoint.hpp:185-207`)
  are the seams this task extends.
- `pool.mmap_backing` — settled: owns the on-disk layout this task amends
  (`WorkspaceHeader`, `WorkspaceChunkEntry`), and explicitly deferred the
  "per-type slot sizes / arena directory" header field to a later task
  (`tasks/refinements/pool/mmap_backing.md:55-58`). This is that task.
- Downstream, **pending**: `model.workspace_backing`
  (`tasks/10-model.tji:16-21`) depends on this task and is the M8 gate. It
  is the sole consumer today.

## What this task is

The workspace file records *that* a data chunk exists but not *whose* it
is. One file backs several slab stores at once — a document's HAMT nodes
and its object records are different size classes, hence different
`SlotStore`s over one `Arena` (`src/model/model.cpp:512`) — and on reopen
the file hands chunks back in a size-blind, owner-blind FIFO
(`src/pool/workspace_file.cpp:498-505`), so with two stores the chunks
mis-route and recovery reads records as nodes. This task lands the missing
piece of doc 15's header promise: an **arena directory** that tags each
chunk with the store that owns it, and a **store table** that records each
store's slot stride, alignment, slots-per-chunk, and durable slot
high-water — published under the same A/B discipline as the root, so a
recovery always reads the high-water belonging to the root it selected.
Reopen then routes every chunk to its owning store deterministically, and
`model.workspace_backing`'s cold-recovery partition of `d_nodes` vs
`d_records` becomes a lookup rather than a guess.

## Why it needs to be done

`model.workspace_backing` — the M8 gate — cannot be implemented without
it, which is why it was re-deferred (`tasks/parking-lot.md:164-169`). The
implementer's analysis there is worth restating because it is the whole
justification for doing format work rather than something cheaper: routing
reopened chunks by requested byte size (candidate B) is not merely
fragile, it is **wrong in the ASan/debug lane**, where `HamtNode` (stride
288 → 128 slots) and `ObjectRecord` (stride 144 → 256 slots) both yield
**36864-byte chunks**. `d_chunk_bytes = chunk_slots * slot_stride` with
`chunk_slots` a power of two (`src/pool/arbc/pool/slot_store.hpp:36-44`,
`src/pool/slot_store.cpp:97`) is not monotone in stride, so chunk byte
size is simply not a store identity. Nothing recorded on disk today can
tell the two apart. Triage chose resolution (A) — the doc-faithful fix —
on 2026-07-10 (`tasks/parking-lot.md:169`).

## Inputs / context

Design docs (normative):

- `docs/design/15-memory-model.md:207-218` — "the file carries a header
  with layout schema version, per-store slot sizes, and arena directory".
  The field this task implements; **never implemented** by `mmap_backing`.
- `docs/design/15-memory-model.md:199-205` — the persistence split:
  refcounts, free pools, generation tags, and the reclaim queue are
  anonymous, "rebuilt on open (a reachability walk from the live roots
  reconstructs counts; free slots are the complement)". The store table
  persists *geometry and high-water*, never bookkeeping.
- `docs/design/15-memory-model.md:220-231` — the ordered commit and the
  A/B root; amended by this task's delta (below) to state that the store
  table rides the same A/B discipline.
- `docs/design/15-memory-model.md:244-251` — the count/generation columns
  belong to the **size-class slab store**, not the typed view: several
  typed views can share one store. This is why the store table is keyed by
  size class, not by type, despite the doc's older "per-type" wording.
- `docs/design/17-internal-components.md:36,49` — `arbc::pool` is **L1**,
  may depend on `base` (L0) **only**. All of this task's work is inside
  `pool`; it adds no dependency edge.
- `docs/design/16-sdlc-and-quality.md:66-78` — TSan + the crash-recovery
  fault-injection tier, both binding here.

Design-doc delta landed by this task (doc 16 same-commit rule):

- `docs/design/15-memory-model.md` — new paragraph "**The arena directory
  is what makes reopen unambiguous**" after the position-independence
  paragraph (store identity = `(slot stride, slot alignment)`; chunks
  carry their owner; byte size is not an identity; stride mismatch is
  refused as a value), plus a sentence in the checkpointing paragraph
  binding the store table to the A/B root snapshot. `per-type slot sizes`
  → `per-store slot sizes` at line 213, reconciling the doc's own
  size-class-store model (15:244-251). Not project-shaping — no doc 00
  decision-record bullet.

Code (the seams this task extends):

- `src/pool/arbc/pool/workspace_file.hpp:59-95` — `WorkspaceHeader` (112
  B: magic, format major/minor, page_size, max_chunks, data_offset,
  chunk_count, `root_slot_a`/`root_slot_b`, `reserved[7]`),
  `WorkspaceChunkEntry` (24 B: `offset`, `size`, `state`, **`reserved`
  u32 — written 0, unused**), `k_workspace_format_major = 1`,
  `static_assert(sizeof(WorkspaceChunkEntry) == 24)`.
- `src/pool/workspace_file.cpp:352-454` — `create`: computes
  `header_bytes = align_up(sizeof(WorkspaceHeader) + max_chunks*24, page)`
  and stamps the header; the directory region is left as a hole so the
  file stays sparse.
- `src/pool/workspace_file.cpp:491-541` — `acquire`: **the defect site.**
  Lines 498-505 drain the reopened-chunk queue (`d_reopened`,
  `d_reopened_cursor`) front-to-back *ignoring the requested size* before
  considering growth. Lines 507-541 are the append-only growth path
  (`d_chunk_count` / `d_next_offset` only advance; a punched directory
  entry is never re-used — the invariant this task's ordering relies on).
- `src/pool/workspace_file.cpp:628-787` — `open`: validates magic/major,
  maps header+directory, remaps every live chunk **in directory order**
  into `d_reopened` (:752-784), with a short-file guard per entry
  (:763-768).
- `src/pool/workspace_file.cpp:591-626` — `read_header`; `format_major`
  mismatch → `UnsupportedFormat` (:622-624). `format_minor` unchecked.
- `src/pool/arbc/pool/slot_store.hpp:180-193` —
  `reserve_restored(high_water)` ("the reopened source returns the
  already-mapped file chunks in order, so this never grows the file") and
  `high_water()`. Impl `src/pool/slot_store.cpp:255-285`: computes
  `chunks_needed = ((high_water-1) >> chunk_bits) + 1` and calls
  `acquire` once per chunk. **The caller must supply `high_water`; nothing
  in the file records it.**
- `src/pool/arbc/pool/slot_store.hpp:210-211` — `slot_stride()`,
  `slot_align()` accessors already exist. `src/pool/slot_store.cpp:322-340`
  — `Arena::store_for` keys stores in a `std::map` by `{stride, align}`:
  **the store identity this task persists already exists in memory.**
- `src/pool/arbc/pool/checkpoint.hpp:133-171` — `Checkpointer::commit`
  (sync_data → publish root into the inactive slot → sync_header);
  `:185-207` — `open` (selects the higher-generation valid root) and
  `finalize_open(SlotStore&, live_set)`.
- `src/pool/arbc/pool/chunk_source.hpp:26-46` — the `ChunkSource` seam
  (`acquire(size, alignment)` / `release`) and `AnonymousChunkSource`.
  Also consumed by `big_block_pool`; **not to be widened** (Decision 2).

Predecessor decisions this task honors:

- `mmap_backing.md:109-112` — "Header owns the root slots now, protocol
  later": on-disk layout changes are the expensive kind, so make them
  once, deliberately. This task spends a format-major bump and buys the
  full arena directory rather than dribbling fields in.
- `mmap_backing.md:65-68` — bookkeeping stays anonymous, "enforced
  structurally". The store table persists geometry + high-water only; no
  refcount, free list, or generation tag ever reaches disk.
- `checkpoints.md` — errors are values (`WorkspaceFileErrc`), never
  aborts; the fault-injection `SyscallInjector` seam
  (`workspace_file.hpp:129-174`) covers every new syscall-adjacent step.

## Constraints / requirements

**Store identity is `(slot_stride, slot_align)`** — the key
`Arena::store_for` already uses (`slot_store.cpp:322-340`). It is stable
across runs of the same build, requires no id constants invented by the
model layer (and so cannot be swapped by a caller), and directly realizes
doc 15's "per-store slot sizes". The store-table row index is an internal
`StoreId` (a `std::uint32_t`), an implementation detail persisted only as
the chunk tag.

**On-disk format (format_major 1 → 2).**

- `WorkspaceChunkEntry` stays **24 bytes**: the existing unused
  `reserved` u32 (`workspace_file.hpp:81`) becomes `owner` — the
  store-table row index of the chunk's owning store. The
  `static_assert(sizeof(WorkspaceChunkEntry) == 24)` and the directory's
  offset (immediately after the header) are unchanged.
- New `WorkspaceStoreEntry`, **16 bytes**:
  `{ uint32 slot_stride; uint32 slot_align; uint32 chunk_slots; uint32 high_water; }`.
  `slot_stride == 0` means the row is unused (no separate state field).
- **Two store tables, A and B** — one per root slot, selected by the same
  index as the root. Each is `max_stores` rows and is self-contained
  (identity columns duplicated into both at bind time; only `high_water`
  differs per snapshot).
- Placement: after the chunk directory, before the page-aligned
  `data_offset` — so the chunk directory **does not move**. `create`
  computes
  `header_bytes = align_up(sizeof(WorkspaceHeader) + max_chunks*24 + 2*max_stores*16, page)`.
  For the default layout (`max_chunks = 16384`, `max_stores = 16`) the
  tables fit entirely in the existing page slack between the directory's
  end and the first page boundary, so the default file grows by zero
  pages. `data_offset` is a stored header field, so the layout stays
  self-describing regardless.
- Two of the seven reserved `WorkspaceHeader` u64s are consumed:
  `store_table_offset` (file offset of table A; table B follows) and a
  packed `max_stores`. Five remain reserved and zeroed.
- `WorkspaceLayout` gains `max_stores{16}` (`workspace_file.hpp:97-99`).
- **`k_workspace_format_major` → 2.** A v1 file has untagged chunks and no
  store table; opening one two-store must not be attempted. The existing
  `UnsupportedFormat` check (`workspace_file.cpp:622-624`, `:667-683`)
  already rejects it as a value — the bump is the whole cost. Workspace
  files are same-machine session artifacts with no portability promise
  (doc 15:214-218), so a rejected v1 file simply falls back to loading the
  doc 08 JSON document; nothing durable is lost.

**Per-store binding seam.** `WorkspaceFileChunkSource` gains

```
expected<ChunkSource*, WorkspaceFileError>
store_view(std::uint32_t slot_stride, std::uint32_t slot_align,
           std::uint32_t chunk_slots);
```

returning a per-store `ChunkSource` facade owned by the source. On a fresh
file it appends a store-table row (both snapshots); on a reopened file it
matches the row by `(slot_stride, slot_align)` and **validates
`chunk_slots`**, returning `StoreLayoutMismatch` on disagreement (this is
what turns a debug-vs-release lane mismatch into a clean value error
instead of silent mis-routing). `MaxStoresExceeded` when the table is
full. The facade's `acquire` serves *only its own store's* chunks and tags
every newly grown chunk with its `owner`; its `release` routes to the
existing fenced hole-punch path unchanged.

**`ChunkSource` (`chunk_source.hpp:34`) is not widened** — see Decision 2.
`SlotStore` continues to hold a plain `ChunkSource*` and needs **no
changes to its acquire path**. `Arena` gains an optional workspace-aware
construction that calls `store_view(...)` in `store_for` so each store is
handed its own facade; the anonymous path is untouched.

**Reopen routing.** `open` partitions the remapped live chunks into
per-store restore queues by the `owner` tag, preserving directory order
within each owner. Directory order **is** per-store acquisition order:
growth is strictly append-only and a punched entry is never re-used
(`workspace_file.cpp:507-541`), so a store's *k*-th directory-ordered
chunk backs its slot range `[k*chunk_slots, (k+1)*chunk_slots)` — exactly
what `reserve_restored` assumes. This invariant must be **asserted, not
trusted**: on open, each bound store's restore-queue length must be
`>= ceil(high_water / chunk_slots)`; a shortfall (a hole, a mis-tagged
chunk, a torn directory) is `StoreDirectoryInconsistent`, a value error,
never a mis-route.

**Post-checkpoint chunk garbage.** A crash after chunks were appended but
before the commit that would publish them leaves live directory entries
above the selected root's high-water. Those chunks are beyond
`chunks_needed`, so `reserve_restored` never claims them: after all stores
have re-bound, `open` releases (hole-punches) every live chunk whose
owner's high-water does not cover it. This is the chunk-level analogue of
the existing "freed after the last checkpoint" quarantine
(`15-memory-model#freed-slot-quarantined-until-durable`) and it keeps the
file from leaking storage across an unclean shutdown.

**High-water publication is part of the commit, not of `allocate`.**
`Checkpointer` gains `register_store(SlotStore&)` (writer-only setup,
mirroring `set_release_fence`). `commit` (`checkpoint.hpp:133-171`) writes
each registered store's current `high_water()` into the **inactive**
store-table snapshot immediately before flipping that root slot — i.e.
between today's step 2 and step 3 — so the same `sync_header()` that makes
the new root durable makes its high-waters durable. Step 1 (`sync_data`)
has already made every chunk those high-waters cover durable. No new
msync, no new fence, no change to the commit's syscall sequence. A crash
before `sync_header` leaves the *old* root durable, and the old root's
snapshot is the one this commit did not touch — old root pairs with old
high-water, always.

**Recovery entry point.** `Checkpointer::open` exposes the selected root's
store table; a `reserve_restored_all(Arena&)` helper walks it, calling
`Arena::store_for(stride, align)` and `reserve_restored(entry.high_water)`
per row. The model layer then runs its typed reachability walk and
`finalize_open` per store, unchanged.

**Platform.** The format structs and all directory/store-table logic are
platform-neutral and shared by both lanes; the Win32 backing
(`pool.mmap_backing_win32` / `pool.checkpoints_win32`, both `complete 100`)
maps the same header region via `MapViewOfFile` and needs no separate
port. Keeping the `msvc-debug`/`win-dev` CI lane
(`.github/workflows/ci.yml:60`) and `crash_tests_win32` green is **in
scope for this task**, not deferred.

**Levelization.** Everything lands in `arbc::pool` (L1 → `base` only).
`model` (L2) consumes it. `scripts/check_levels.py` must stay green.

## Acceptance criteria

Unit tests — new `src/pool/t/workspace_store_directory.t.cpp`, wired into
`arbc_component_test` (`src/pool/CMakeLists.txt:11-14`):

- **The regression that motivated the task**: two stores of *deliberately
  colliding chunk byte size* (the debug lane's 288/144 strides, forced
  explicitly so the test bites in every lane) allocate **interleaved**
  records into one workspace file; commit; reopen; every record resolves
  to the bytes it had before, and each store's restored chunk set is
  exactly the set it acquired. This test fails on today's FIFO.
- Fresh-file bind: `store_view` appends a row to both snapshots; a second
  bind with the same `(stride, align)` returns the same facade.
- Reopen validation: a store table declaring a different `chunk_slots`
  than the reopening build → `StoreLayoutMismatch` value; a v1-major file
  → `UnsupportedFormat` value; a store-table row whose chunk set does not
  cover its high-water → `StoreDirectoryInconsistent` value. All three
  are errors, never aborts, never mis-routes.
- `MaxStoresExceeded` when more than `max_stores` distinct size classes
  bind.
- Post-checkpoint chunk garbage: append chunks, kill before commit,
  reopen — the uncovered chunks are hole-punched and `st_blocks` drops
  (Linux-guarded, matching the existing `#hole-punch-returns-storage`
  test's guard).
- Default-layout size assertion: with `max_chunks = 16384, max_stores = 16`
  the store tables consume existing page slack — `data_offset` is
  byte-identical to format 1's. Pins the "no extra pages" claim.

Byte-exact golden (deterministic format, doc 16's default):

- A golden of the freshly-created header + store-table + directory region
  of a fixed-`WorkspaceLayout` file, with `page_size`/`data_offset`
  normalized (4 KiB page guard). Catches silent format drift — the failure
  mode that makes a workspace file unopenable by the next build. Tolerance
  is not used; this is a byte comparison.

Claims register (`tests/claims/registry.tsv` + `enforces:` tags, checked
by `scripts/check_claims.py`):

- **`15-memory-model#reopen-routes-chunks-to-owning-store`** — Reopening a
  workspace file that backs several size-class stores re-binds each store
  to exactly the chunks the arena directory records as its own, in slot
  order, so every record resolves to its pre-crash bytes; routing is by
  the recorded owner tag, never by chunk byte size (two stores of
  different strides can share one chunk size).
- **`15-memory-model#store-high-water-durable-with-root`** — The per-store
  high-water a recovery reads is the one published by the root it
  selected: a kill anywhere between the data msync and the header msync
  recovers the old root paired with the old store-table snapshot, never a
  mismatched pair.
- **`15-memory-model#store-layout-mismatch-rejected`** — Opening a
  workspace whose store table disagrees with the reopening build's slot
  stride / alignment / slots-per-chunk yields a `WorkspaceFileErrc` value
  and no mapping, rather than silently mis-routing chunks.

Existing claims that must stay green (regression surface):
`#checkpoint-recovers-consistent-root`, `#freed-slot-quarantined-until-durable`,
`#checkpoint-published-chunks-read-only`, `#workspace-io-faults-surface-as-values`,
`#chunk-growth-preserves-addresses`, `#hole-punch-returns-storage`,
`#one-count-column-per-size-class` (registry.tsv:53-67).

Behavioral counters (doc 16 — never wall-clock):

- `commit` issues **exactly the same syscall sequence** as before the
  change: N data msyncs + 1 header msync, with the store-table write
  adding **zero** additional msyncs. Assert via the existing
  `SyscallInjector` counters (`data_msyncs()` / `header_msyncs()`,
  `checkpoint.hpp:211-212`) — this is the load-bearing claim that
  high-water durability is free.
- A reopen of an N-store file performs zero file growth (`chunk_count`
  unchanged across `reserve_restored_all`).

Crash / concurrency (doc 16:66-78, mandatory for a pool task):

- **Crash sweep extended**: `src/pool/t/crash_tests.t.cpp`'s
  kill-at-every-syscall sweep re-run with a **two-store** workspace, now
  additionally asserting after every injected death that the recovered
  root's store-table snapshot is self-consistent (each store's chunk set
  covers its high-water) and that the recovered graph resolves. The new
  `RootFlip`-adjacent store-table write is inside the injector's reach.
- Torn/short-file paths extended to the store-table region (a file
  truncated inside table B → value error).
- **TSan**: a writer-grows-and-commits / readers-`resolve` litmus over a
  two-store workspace, asserting no race on the header mapping now that
  `commit` writes the store table there. `store_view` /
  `register_store` are writer-only setup (debug assert, matching
  `slot_store.hpp:113-114`).
- Windows lane (`crash_tests_win32`, `.github/workflows/ci.yml:55`) green.

Gate: full suite green including ASan/UBSan and TSan; **≥90% diff
coverage** on changed lines.

**No deferred follow-up.** The Win32 backing shares these format structs
and rides in scope; no new WBS leaf is registered by this task.

## Decisions

1. **Store identity is `(slot_stride, slot_align)`, not a caller-assigned
   id.** *Rationale*: it is already the `Arena::store_for` map key
   (`slot_store.cpp:322-340`), so it is guaranteed to exist, to be unique
   per store, and to be stable across runs of a build — and the model
   layer cannot get it wrong, because there is nothing for it to choose.
   It is also literally what doc 15:213 promises ("per-store slot sizes").
   *Rejected*: `enum StoreId { k_nodes, k_records }` constants owned by
   the model. It puts a stable-id contract on an L2 caller for an L1
   format, and a swapped constant is undetectable in the debug lane, where
   the two stores' chunk sizes coincide — the exact hazard this task
   exists to remove. *Rejected*: identity by chunk byte size — this is
   candidate (B) from `tasks/parking-lot.md:167`, and it is wrong, not
   merely fragile: debug `HamtNode` (stride 288) and `ObjectRecord`
   (stride 144) both produce 36864-byte chunks.

2. **A per-store `ChunkSource` facade, rather than widening
   `ChunkSource::acquire` with a store parameter.** *Rationale*: the
   `ChunkSource` seam (`chunk_source.hpp:26-46`) is the generic backing
   interface — `AnonymousChunkSource` and `big_block_pool` also speak it,
   and neither has any notion of a store directory. A facade keeps that
   interface at zero churn, leaves `SlotStore`'s acquire path **entirely
   unmodified**, and makes mis-routing *structurally impossible*: a store
   physically cannot see another store's chunks, because it holds a handle
   that only serves its own. This is the same "enforce structurally, not
   by convention" move `mmap_backing.md:65-68` made for the
   anonymous-bookkeeping split. *Rejected*: adding a `StoreId` argument to
   `acquire` — it pushes a workspace-only concept into every
   `ChunkSource`, and mis-routing stays one wrong argument away.
   *Rejected*: a `set_current_store()` side-channel setter on the source —
   stateful, order-dependent, and hostile to the writer-only-growth
   assertions.

3. **Two store tables, A/B, one per root slot.** *Rationale*: this is the
   only way to make the recovered high-water *provably* consistent with
   the recovered root. The root flip is a single naturally-aligned 8-byte
   store (torn-write-free), but a store table is 16 bytes per row and
   cannot be published atomically. If both roots shared one table, a crash
   between the table write and `sync_header` could leave a durable
   *new* high-water paired with a durable *old* root — a high-water
   pointing past the chunks that root's commit made durable, i.e. exactly
   the corruption the A/B protocol exists to prevent. Snapshotting per
   root slot reduces the problem to the one already solved: the commit
   only ever writes the *inactive* snapshot, so the surviving root's
   snapshot is untouched by the commit that died. Cost is 256 bytes of
   header slack. *Rejected*: a single shared table plus a checksum —
   detects the mismatch but cannot repair it, turning a recoverable crash
   into a failed open. *Rejected*: putting high-water in the root record —
   it does not fit; the u64 root slot is fully spent on
   `(generation, root_index)` (`checkpoint.hpp:38-50`).

4. **High-water is written at commit, not at `allocate`.** *Rationale*: a
   high-water written eagerly on every allocation would routinely be ahead
   of the durable root, describing chunks that no `msync` has yet made
   durable. Writing it in the commit — between the root-slot write and
   `sync_header` — makes it durable by the *same* header msync that
   publishes the root, after `sync_data` has already flushed the chunks it
   covers. This is why the change adds **zero** syscalls to the commit
   path, which the behavioral-counter assertion pins.

5. **Bump `format_major` to 2 and refuse v1 files.** *Rationale*: a v1
   file's chunks are untagged; a v2 reader could only guess at ownership —
   the very bug being fixed. The existing `UnsupportedFormat` check makes
   the refusal a clean value error, and doc 15:214-218 is explicit that
   the workspace file is a same-machine session artifact with no
   portability promise, sitting *beside* the doc 08 JSON document, which
   remains the interchange format. Refusing a v1 workspace therefore costs
   a re-open from JSON, not data. *Rejected*: a compatibility shim that
   opens v1 files as single-store — it would silently succeed on precisely
   the interleaved two-store files that are corrupt, which is worse than
   refusing.

6. **The chunk entry keeps its 24-byte size; the owner tag goes in the
   existing spare `reserved` u32** (`workspace_file.hpp:81`), and the
   chunk's slot range is *derived* from its position in its owner's
   directory-ordered chunk list rather than stored. *Rationale*: the
   derivation is sound because the directory is strictly append-only and a
   punched entry is never re-used (`workspace_file.cpp:507-541`), so
   per-owner directory order is per-owner acquisition order, permanently.
   Storing an explicit `first_slot` would grow the entry to 32 B (a 33%
   larger directory region) to record something already implied — and it
   would not actually buy safety, because `reserve_restored`
   (`slot_store.cpp:255-285`) requires a store's chunks to be *contiguous*
   from slot 0 regardless. The residual risk (a future mid-life
   emptied-chunk punch putting a hole in a store's sequence) is therefore
   handled where it actually bites, by the `StoreDirectoryInconsistent`
   coverage check on open, not by a redundant field. *Alternative kept on
   the table*: if a later task wires mid-life chunk punching into
   `SlotStore`, it must revisit `reserve_restored`'s contiguity assumption
   — the check added here will fail loudly rather than corrupt, which is
   the point.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-11.

- Bumped `k_workspace_format_major` to 2; `WorkspaceChunkEntry.reserved` u32
  repurposed as `owner` (store-table row index); new `WorkspaceStoreEntry`
  (16 bytes); two A/B store tables after the chunk directory in
  `src/pool/arbc/pool/workspace_file.hpp`.
- Per-store `ChunkSource` facade (`store_view`) and `register_store` seam
  on `Checkpointer` wired into `src/pool/arbc/pool/chunk_source.hpp`,
  `src/pool/arbc/pool/checkpoint.hpp`, `src/pool/workspace_file.cpp`,
  `src/pool/slot_store.cpp` so high-water is published at commit with zero
  extra msyncs.
- Reopen routes chunks by owner tag; `reserve_restored_all` helper walks the
  selected root's store table; `StoreDirectoryInconsistent` / `StoreLayoutMismatch`
  / `MaxStoresExceeded` / `HeaderIoFailed` value-errors added.
- Unit, golden, counter, crash, and TSan tests in
  `src/pool/t/workspace_store_directory.t.cpp`; crash sweep extended for
  two-store workspace in `src/pool/t/crash_tests.t.cpp`; wired via
  `src/pool/CMakeLists.txt`.
- Design-doc delta: `docs/design/15-memory-model.md` — "arena directory makes
  reopen unambiguous" paragraph; store table tied to A/B root snapshot.
- Claims `15-memory-model#reopen-routes-chunks-to-owning-store`,
  `#store-high-water-durable-with-root`, `#store-layout-mismatch-rejected`
  registered in `tests/claims/registry.tsv`.
- Two pre-existing behaviors flagged for human review (not changed):
  `pool.header_writeback_ordering` (kernel write-ordering gap between root-slot
  and store-table pages) and `pool.clean_close_preserves_file` (clean teardown
  still hole-punches live chunks; `model.workspace_backing` will encounter
  this). Both entered `tasks/parking-lot.md`.
