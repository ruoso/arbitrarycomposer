# serialize.tile_store_parallel_save — fan the per-tile encode pipeline across pool workers

## TaskJuggler entry

[`tasks/60-serialize.tji:81-86`](../../60-serialize.tji) — `task
tile_store_parallel_save "Fan per-tile encode pipeline across pool workers"`,
`effort 2d`, `depends !raster_tile_store`.

> Dispatch the per-tile encode pipeline (storage-convert -> hash -> shuffle ->
> compress) across pool workers. Already a pure function of immutable `peek()`ed
> bytes with a stateless compressor, so the parallelism is sound by construction;
> `raster_tile_store` ships it single-threaded and correct first. Behavioral-counter
> and TSan coverage, no wall-clock assertion. Source-of-debt:
> `tasks/refinements/serialize/raster_tile_store.md` (Deferred follow-ups), commit
> `serialize.raster_tile_store`. Doc 08.

Milestone: `m9_release` ([`tasks/99-milestones.tji:72`](../../99-milestones.tji) —
listed by id in the `m9_release` `depends`).

## Effort estimate

**2d**, apportioned:

| piece | days |
| --- | --- |
| `runtime` generic work lane on `WorkerPool` (`submit_work` + `WorkCompletion`, worker-loop drain, counters, `worker_count==0` inline) + `worker_pool.t.cpp` coverage | 0.5 |
| `runtime` `TileEncodeDispatch` seam (inline + worker-backed impls, bounded in-flight window) injected into `codec_raster` save; restructure the save loop to fan pure encode / reap-by-index on the save thread | 0.75 |
| Determinism / executor-independence golden + incremental-counter + bounded-in-flight tests; TSan save-races-writer + seeded schedule-perturbation stress; claims rows | 0.5 |
| Design-doc deltas (02, 08, 00), `-Werror`/level/claims/`tj3` gate, ≥90% diff coverage | 0.25 |

The task's own note is right that the encode is *sound by construction*: the
per-tile transform is already a pure function of immutable `peek()`ed bytes over a
stateless compressor. The 2d is **not** in the encode math — it is in the two
seams the single-threaded path did not need: a generic (non-render) work lane on
the runtime pool, and a fan-out/reap-by-index driver that keeps every mutation on
the save thread so no new shared mutable state is introduced.

## Inherited dependencies

### Settled

- **`serialize.raster_tile_store`** ([refinement](raster_tile_store.md), done
  2026-07-14) — the source-of-debt. It shipped the encode pipeline
  single-threaded and correct, and handed forward every primitive this task
  parallelizes:
  - The single-threaded save loop this task rewrites:
    [`src/runtime/codec_raster.cpp:120-169`](../../../src/runtime/codec_raster.cpp).
    Per tile the loop does memo hash (`hash_of`, L131) → positional
    `blobs.push_back` (L132) → in-save dedup (`seen.insert`, L134) → on-disk dedup
    (`ctx.has_asset`, L138) → **the pure encode** `peek` → `to_storage_bytes` →
    `frame_tile_blob` (shuffle+compress) (L145-150) → sink write (`ctx.store_asset`,
    L154). The pinned snapshot the whole loop reads is `raster->store().base_table()`
    (L104-108), whose `shared_ptr` *"holds a pool count on every slot it names, so
    every `peek()` below is valid for the whole save even as the writer thread
    paints on and drops versions concurrently."*
  - The pure, stateless, `arbc::serialize`-owned (L4) encode functions safe to call
    from any worker: `to_storage_bytes`
    ([`tile_blob.hpp:132`](../../../src/serialize/arbc/serialize/tile_blob.hpp)),
    `frame_tile_blob` (shuffle+compress,
    [`tile_blob.hpp:139-140`](../../../src/serialize/arbc/serialize/tile_blob.hpp)),
    `shuffle_bytes`/`unshuffle_bytes` ([`tile_blob.hpp:111-125`](../../../src/serialize/arbc/serialize/tile_blob.hpp)),
    and `compress_blob`/`decompress_blob`
    ([`blob_compress.hpp:67-91`](../../../src/serialize/arbc/serialize/blob_compress.hpp)).
    **`blob_compress.hpp:67-72` pre-clears the compressor for this task by name**:
    *"Stateless and reentrant: it uses zstd's one-shot API, never a shared
    `ZSTD_CCtx` … Tile compression will be called from pool workers
    (serialize.raster_tile_store), so this is safe to call concurrently with no
    locking and no new TSan surface. A later 'optimization' to a shared reused
    context would be a data race."* `sha256`
    ([`src/base/arbc/base/sha256.hpp`](../../../src/base/arbc/base/sha256.hpp)) is
    likewise stateless.
  - The any-thread read primitive: `BigBlockPool::peek(BlockSlotRef) const noexcept`
    ([`src/pool/arbc/pool/big_block_pool.hpp:190-196`](../../../src/pool/arbc/pool/big_block_pool.hpp))
    — *"Zero-refcount-traffic read … Valid only while the target is kept alive by a
    count the caller holds. Any thread."* The `base_table()` pin is exactly that
    caller-held count. `allocate` is writer-only; **workers must never allocate a
    pool slot** (they only `peek` and encode into their own output buffer).
  - The stateful memo whose interaction changes: `RasterTileStore`
    ([`src/runtime/arbc/runtime/raster_tile_store.hpp`](../../../src/runtime/arbc/runtime/raster_tile_store.hpp))
    — `hash_of` (L73, currently hashes *inside* `d_mutex` L119), atomic
    `tiles_hashed()` (L91-93, advancing once per tile actually hashed, never on a
    memo hit), `memoized()` (L96).
  - The behavioral counters this task must keep invariant across executors:
    `AssetSink::blobs_written()`
    ([`src/serialize/arbc/serialize/save_context.hpp`](../../../src/serialize/arbc/serialize/save_context.hpp)),
    `RasterTileStore::tiles_hashed()`, and the round-trip / incremental claims
    `08-serialization#raster-save-is-incremental` (`tests/claims/registry.tsv` id
    317) and `#raster-tile-store-round-trips-byte-exactly` (id 318).

- **`runtime.shared_worker_pool`** ([refinement](../runtime/shared_worker_pool.md),
  done 2026-07-13) — the runtime `WorkerPool`
  ([`src/runtime/arbc/runtime/worker_pool.hpp:109-229`](../../../src/runtime/arbc/runtime/worker_pool.hpp))
  this task extends. It established the fan-out idiom this task mirrors, and three
  invariants this task must not break:
  - Result ordering is **not** pool-guaranteed (shared FIFO, workers steal, settles
    land in completion order); deterministic output is **caller-imposed** by an
    indexed `vector<shared_ptr<Completion>>` reaped by index.
  - Errors are **values, never exceptions** across the worker boundary: `run_task`
    calls the job with no try/catch
    ([`src/runtime/worker_pool.cpp:261`](../../../src/runtime/worker_pool.cpp)); a
    task body that throws terminates. Job bodies convert failure into a failed
    completion.
  - `worker_count == 0` is the **degenerate inline executor** — `submit` *is* the
    run, on the caller thread (`worker_pool.hpp:88-104`). The offline export driver
    defaults to inline-exact.

### Pending

(none — both predecessors are `complete 100`.)

### Downstream (this task unblocks)

Nothing in the WBS `depends` on `tile_store_parallel_save`; it is a leaf of
`m9_release`. Its sibling `serialize.tile_store_parallel_load`
([`tasks/60-serialize.tji:87-92`](../../60-serialize.tji)) is the decode mirror and
depends on `raster_tile_store` + `kinds.raster_tilewise_load`, **not** on this task
— the two are independent parallelizations of the same store, and the load side's
own bounded-in-flight seam is scoped there. Where a mechanism is genuinely shared
(the generic work lane, the bounded-window fan-out helper), this task lands it and
the load task reuses it; nothing in this refinement blocks on that reuse.

## What this task is

Take the per-tile encode loop `raster_tile_store` shipped single-threaded
([`codec_raster.cpp:126-160`](../../../src/runtime/codec_raster.cpp)) and fan its
CPU-bound per-tile work — hash and shuffle-then-compress, both computed from one
`peek()`ed immutable tile — across the runtime worker pool, while keeping **every
mutation on the save thread**. The transform is already pure over immutable inputs
with a reentrant compressor, so the parallel form is a mechanical restructure into
the codebase's established fan-out/reap-by-index shape (the audio lookahead pump,
[`src/runtime/lookahead_pump.cpp:113-162`](../../../src/runtime/lookahead_pump.cpp)):
allocate one completion per tile, submit each pure encode job, park until settled,
and reap **by index** on the save thread — where the memo update, the `seen`/
`has_asset` dedup, the positional `blobs[i] = hash` write, and the `AssetSink` write
all happen, single-threaded, exactly as today.

Because the runtime pool is a **render** executor whose only submission seam takes a
`RenderTask` (a `Content*` rendering into a caller `Surface&`, enforced leaf-only by
[`worker_dispatch.hpp:67`](../../../src/runtime/arbc/runtime/worker_dispatch.hpp) +
[`scripts/check_worker_dispatch.py`](../../../scripts/check_worker_dispatch.py)), a
tile-encode job cannot ride the render lane. This task therefore adds a **second,
generic work lane** to `WorkerPool` — a plain movable-callable job that settles a
caller-owned completion — distinct from the render lane and its leaf-only policy.
The generic lane's jobs provably touch only their own output buffer and the
immutable pinned document version, never the cache, so doc 02's *"workers never
touch the cache"* discipline holds by construction.

The fan-out is bounded: at most O(`worker_count`) encode jobs are in flight at once,
so a save's transient peak is O(`worker_count` · tile), not O(image), mirroring the
O(tile)-per-unit posture doc 15 asks of the load path and keeping an autosave from
flooding the shared pool that interactive rendering also uses. When the pool has
zero workers (the offline-export default), the encode runs inline on the save
thread — bit-for-bit the path `raster_tile_store` already ships.

It does **not** change the on-disk format, the canonical `.arbc` bytes, the hash,
the storage format, or any codec-visible contract: parallel and inline saves are
byte-identical. It does **not** touch `arbc::serialize` (L4), which stays pool-free;
the whole fan-out is driven from `arbc::runtime` (L5), the one component that
already sees both the pool and the raster params shape.

## Why it needs to be done

`raster_tile_store` made a painting saveable and made the save *incremental* (a
re-save after one dab writes only the touched tiles). But it encodes every new tile
**serially** on one thread: for a large paint — the note's 30-layer 24MP
composition, thousands of level-0 tiles on a first full save — the storage-convert
→ shuffle → zstd chain is the save's dominant cost, and it runs on a single core
while the rest sit idle. The transform is embarrassingly parallel (each tile is
independent; the hash is over the tile's own uncompressed bytes,
[`08-serialization.md:405-414`](../../../docs/design/08-serialization.md); the
`blobs` array order is fixed row-major, not completion order,
[`08:384-390`](../../../docs/design/08-serialization.md)), and the compressor was
built reentrant *specifically* so this fan-out would be a data-race-free drop-in
([`blob_compress.hpp:67-72`](../../../src/serialize/arbc/serialize/blob_compress.hpp)).
`raster_tile_store` deferred it to *ship correct first*; this task collects the
deferred parallelism now that the single-threaded reference (and its goldens) exist
to prove the parallel form byte-identical against.

Autosave runs *"on a background thread"* against a pinned version
([`14-data-model-and-editing.md:373-374`](../../../docs/design/14-data-model-and-editing.md)),
so a slow serial encode does not *block* editing — but it does hold a full
document's worth of scratch and one core busy for the whole save, and a manual
export is a foreground operation where the wall-clock is a user's wait. Fanning the
encode across the cores the runtime pool already owns is the direct win, and doing
it now — while the serial golden is the oracle — is when it is cheapest and safest.

## Inputs / context

### Design docs (normative, doc 16)

- **[`docs/design/08-serialization.md:369-475`](../../../docs/design/08-serialization.md)
  — Principle 8.** The per-tile encode this task parallelizes: content-hash keyed
  blobs, each distinct tile written once, saves incremental (L384-390); **the hash
  is over the tile's *uncompressed storage-format* bytes, not the compressed blob**
  (L405-414) — which is why hashing and compressing are independent per tile and
  fan out soundly; per-blob zstd + byte-shuffle (L435-443); mips not persisted
  (L415-419). **Doc 08 says nothing about parallelizing the encode** — this task is
  new ground, so it carries a doc 08 delta making the parallelizability and its
  order-independence normative (see Decisions 4).
- **[`docs/design/08-serialization.md:21-53`](../../../docs/design/08-serialization.md)
  — §The asset directory.** The `SaveContext`/`AssetSink` is *write-if-absent* and
  *"reports whether a name was newly written or already present, which is what makes
  the incremental save … an observable property rather than an aspiration"*
  (L48-53). This task keeps the sink single-threaded (writes on the reap thread), so
  `blobs_written()` stays the exact same observable counter under any executor.
  Principle 5 canonical output (L258-270) — sorted keys, shortest round-trip
  decimal — is a determinism the parallel path must not perturb; it can't, because
  `blobs` is filled by index.
- **[`docs/design/02-architecture.md:286-373`](../../../docs/design/02-architecture.md)
  — §Threading model.** Single-writer scene (L288-293); the worker pool is defined
  as a **leaf-render executor** whose leaf-only rule is *"what makes 'workers never
  touch the cache' true rather than aspirational"* (L301-314); `worker_count == 0`
  is the inline executor and the offline driver defaults to it because
  *"byte-determinism is the whole point of an export"* (L315-331); the pool is
  shareable across viewports and *"is not a shared `TileCache`: workers never touch
  the cache"* (L367-373). **This task carries a doc 02 delta** (Decision 1): the pool
  also runs a generic non-render lane whose jobs obey the same cache-free discipline;
  the leaf-only rule governs the *render* lane specifically.
- **[`docs/design/15-memory-model.md:20,38-45,83-100,178-184`](../../../docs/design/15-memory-model.md).**
  Tile pixels are *"immutable once filled"* and refcount-shared, read by *"workers,
  backends"* (L20); reader threads over a version touch only immutable pages, no
  refcount-dirtied lines (L38-45); a `peek`/`const&` traversal writes no count page
  and the design's read-path guarantee is the *"interference-free concurrent pin"*
  (claim `15-memory-model#interference-free-concurrent-pin`, L83-105). This is the
  formal basis that N workers may `peek` the pinned table concurrently. **L178-184
  is the constraint that shapes the design:** *"Workers allocate only from
  pools/arenas warmed for them. The writer thread is the only structural
  allocator."* — so encode workers must not allocate pool slots (they `peek` only),
  and the fan-out reuses the existing runtime pool rather than spinning a second one.
  L361-369 is the O(tile)-per-unit posture the bounded window preserves.
- **[`docs/design/16-sdlc-and-quality.md`](../../../docs/design/16-sdlc-and-quality.md)**
  — claims register + `enforces:` tags, same-commit doc delta (L14-26); **tier-4
  behavioral counters, never wall-clock** (L54-62; L225-226: *"Wall-clock
  performance gates in the merge path — flaky by nature; behavioral counters gate,
  benchmarks trend"*); **tier-6 concurrency: TSan on the full suite + dedicated
  stress with seeded schedule perturbation** (L64-73); diff coverage ≥90% hard gate
  (L112-118).
- **[`docs/design/17-internal-components.md:52,69,71,84`](../../../docs/design/17-internal-components.md)**
  — the level rule (L52, no upward or same-level edges); `arbc::serialize` is **L4**
  (L69), `arbc::runtime` is **L5** (L71); do not widen an edge unless a genuinely new
  direct dependency requires it (L84). `WorkerPool` is a class *inside* `runtime`,
  **not named in doc 17** (confirmed [`shared_worker_pool.md`](../runtime/shared_worker_pool.md)),
  so extending it needs **no doc 17 change** and **no `scripts/check_levels.py`
  edit** — the fan-out driver (`codec_raster.cpp`) and the pool are both L5.

### Source seams

- **[`src/runtime/codec_raster.cpp:104-169`](../../../src/runtime/codec_raster.cpp)** —
  the single-threaded save loop to restructure. The parallel form fans L145-150 (the
  pure encode) plus the hashing L131 currently buried in `hash_of`, and moves L132
  (`blobs.push_back` → positional `blobs[i]=`), L134-140 (dedup), and L154 (sink
  write) onto the reap thread. `base_table()` pin (L108), `storage_format()` (L113),
  `params` assembly (L163-168) are unchanged.
- **[`src/runtime/arbc/runtime/raster_tile_store.hpp:73,91-93,96,119,124`](../../../src/runtime/arbc/runtime/raster_tile_store.hpp)** —
  `hash_of` hashes under `d_mutex` today. The parallel form computes the hash on the
  worker (pure `peek`→`to_storage_bytes`→`sha256`) and updates the memo on the reap
  thread; the ref-keyed memo lookup stays on the save thread *before* dispatch so an
  untouched (ref-shared) tile is a hit and dispatches **no** job — preserving the
  O(dab) re-save and the `tiles_hashed()` semantics exactly (Decision 2).
- **[`src/runtime/arbc/runtime/worker_pool.hpp:59-229`](../../../src/runtime/arbc/runtime/worker_pool.hpp)**
  and **[`src/runtime/worker_pool.cpp:261-282`](../../../src/runtime/worker_pool.cpp)** —
  the pool gains the generic lane. `RenderTask` (L59-64), `CompletionCursor`
  (L83-85), `WorkerPoolConfig` (L88-104), `submit`/`poke`/`wait_completions`/
  `settle_generation`/`drain_owner` (L109-229), and the counters
  (`tasks_submitted`/`tasks_completed`/`tasks_dropped`) are the surface to extend
  symmetrically for the work lane.
- **[`src/runtime/arbc/runtime/worker_dispatch.hpp:67`](../../../src/runtime/arbc/runtime/worker_dispatch.hpp)**
  + **[`scripts/check_worker_dispatch.py`](../../../scripts/check_worker_dispatch.py)** —
  the leaf-only render seam and its structural guard. The guard rejects the token
  `RenderTask` outside `{worker_pool.hpp, worker_pool.cpp, worker_dispatch.cpp}`; the
  generic lane uses a *different* job type, so **no guard change is needed** and the
  render lane's leaf-only invariant is untouched (Decision 1).
- **[`src/runtime/lookahead_pump.cpp:113-162`](../../../src/runtime/lookahead_pump.cpp)** —
  the fan-out/reap-by-index idiom to mirror exactly: per-item staging buffers +
  per-item `shared_ptr<Completion>`, `submit` in a loop, park on
  `wait_completions` until all `settled()`, then reap by index on the driver thread
  and insert into single-thread-confined state.
  **[`src/runtime/interactive.cpp:451-457`](../../../src/runtime/interactive.cpp)**
  (cursor-park) and
  **[`src/runtime/offline_sequence.cpp:208-224`](../../../src/runtime/offline_sequence.cpp)**
  (park-to-quiescence) are the two park shapes.
- **[`src/serialize/arbc/serialize/tile_blob.hpp:14-24,111-140`](../../../src/serialize/arbc/serialize/tile_blob.hpp)**
  and **[`blob_compress.hpp:67-91`](../../../src/serialize/arbc/serialize/blob_compress.hpp)** —
  the pure encode functions (unchanged), and the reentrancy contract that pre-clears
  concurrent use.

### Predecessor / sibling refinements

- [`raster_tile_store.md`](raster_tile_store.md) — Constraint 10 (untouched tiles
  keep `BlockSlotRef` identity across CoW paint, the ground of ref-level dedup),
  Decision 5 (the memo is a refcount-pinned last-version memo), and the Deferred
  follow-ups block that registers **this task** with its 2d estimate and
  behavioral-counter+TSan mandate.
- [`../runtime/shared_worker_pool.md`](../runtime/shared_worker_pool.md) — the
  ordering-is-caller-imposed, errors-as-values, drain-on-outstanding, and
  TSan-clean invariants the generic lane inherits.
- [`asset_gc.md`](asset_gc.md) — the sibling that *deliberately scoped no TSan lane
  and said why* (no shared mutable state); this task is the opposite case — it
  **introduces** a concurrency surface, so it scopes TSan + seeded stress
  explicitly (Acceptance).

## Constraints / requirements

1. **Byte-identical to the single-threaded save, under any worker count.** The
   canonical `.arbc` bytes, the `params.blobs` array (row-major order,
   [`08:385`](../../../docs/design/08-serialization.md)), every blob's content, and
   its hash-derived name are bit-for-bit identical whether the save ran inline
   (`worker_count == 0`) or across N workers. This is the load-bearing correctness
   property; the serial golden `raster_tile_store` shipped is the oracle
   (Decision 3, Acceptance claim `#raster-save-is-executor-independent`).

2. **All mutation stays on the save thread; workers run pure jobs only.** An encode
   job reads its pinned tile via `peek` and writes only into its **own** output
   buffer (the `sha256` hash + the framed compressed bytes). The memo update, the
   `seen`/`has_asset` dedup, the positional `blobs[i] = hash`, and the `AssetSink`
   write happen only on the save/reap thread, single-threaded — exactly the audio
   pump's discipline (*"inserts happen on the driver/reap thread"*). No new shared
   mutable state, no lock on the hot path, so the fan-out is *"sound by
   construction."*

3. **No pool allocation on workers.** Workers `peek` (any-thread, no refcount
   traffic) and allocate only from worker-warmed scratch for their own output
   buffer; they never call `BigBlockPool::allocate` (writer-only) and never bump a
   refcount. Per doc 15 L178-184, the writer thread is the only structural allocator.

4. **Bounded in-flight: peak encode jobs ≤ O(`worker_count`).** The fan-out is a
   windowed submit/reap, not fire-all-then-join: at most a small multiple of
   `worker_count` jobs are outstanding at once, so a save's transient scratch is
   O(`worker_count` · tile) independent of image size (the O(tile)-per-unit posture
   of doc 15 L361-369), and an autosave cannot flood the shared render pool with an
   image's worth of queued jobs. Asserted by a behavioral counter, not wall-clock.

5. **Inline degeneracy is the shipped path.** With `worker_count == 0` (offline
   export default, or any host that asked for the inline pool) the encode runs on
   the save thread in row-major order — bit-identical to
   `codec_raster.cpp:126-160` today. The worker-backed and inline `TileEncodeDispatch`
   impls are the same algorithm with a different executor; determinism is testable by
   diffing the two.

6. **The generic work lane preserves every render-lane invariant.** Adding the lane
   must not touch the `RenderTask` leaf-only render policy
   ([`check_worker_dispatch.py`](../../../scripts/check_worker_dispatch.py) unchanged),
   must keep the pool's accounting identity `tasks_submitted == tasks_completed +
   tasks_dropped + <outstanding>` at quiescence *including* work jobs, must convert a
   failing job into a failed completion (no exception across the boundary,
   [`worker_pool.cpp:261`](../../../src/runtime/worker_pool.cpp)), and must be TSan-
   clean under the existing per-push `gcc-tsan` lane
   ([`.github/workflows/ci.yml`](../../../.github/workflows/ci.yml)).

7. **Errors are values across the boundary.** An encode job that fails (a
   `frame_tile_blob` `TileBlobError`) settles its completion with the error; the reap
   thread turns the first failed completion into the same `SerializeError`
   (`CodecFailed` / `AssetWriteFailed`) the serial path returns
   ([`codec_raster.cpp:151-159`](../../../src/runtime/codec_raster.cpp)). A save that
   fails mid-fan-out drains outstanding jobs (no leaked worker referencing a freed
   buffer) before returning the error.

8. **Levelization (doc 17, CI-enforced).** The pure encode stays in `arbc::serialize`
   (L4); the generic lane, the `TileEncodeDispatch` seam, and the fan-out driver are
   all in `arbc::runtime` (L5), intra-component with `WorkerPool` and
   `codec_raster.cpp`. **`arbc::serialize` gains no pool edge**, and
   `scripts/check_levels.py` `ALLOWED` is **not** edited — an edit there means the
   L4/L5 split was violated (the pool leaked into serialize).

## Acceptance criteria

- **Parallel and inline saves are byte-identical** *(golden + behavioral)*. New
  claim **`08-serialization#raster-save-is-executor-independent`**: saving one
  painted document through a worker-backed `TileEncodeDispatch` (N>1 workers) and
  through the inline dispatch produces byte-identical canonical `.arbc` bytes, an
  identical `params.blobs` array, and an identical on-disk blob set (same hash-named
  files, same bytes). Enforces-tagged; the serial golden from
  `tests/raster_tile_store_golden.t.cpp` is the oracle and the parallel run is
  diffed against it. This is Constraint 1.

- **Incremental-save counters are executor-independent** *(behavioral counter)*.
  The existing incremental claim `08-serialization#raster-save-is-incremental`
  (registry id 317) is re-enforced by a test **parameterized over `worker_count ∈
  {0, N}`**: a re-save after one dab touching tile set *T* writes exactly `|T|`
  blobs (`AssetSink::blobs_written()`) and hashes exactly the tiles actually rehashed
  (`RasterTileStore::tiles_hashed()`), *identically* under inline and worker-backed
  executors. Pins that parallelism changes throughput, never the incremental
  behavior. Likewise `#raster-tile-store-round-trips-byte-exactly` (id 318) gains the
  `worker_count == N` parameterization.

- **An all-empty layer stores one blob under parallel save** *(behavioral counter)*.
  Re-enforce the content-addressing claim (registry id 316) under `worker_count ==
  N`: an all-empty (ref-shared) layer dispatches at most one encode job and writes
  one blob — ref-level dedup on the save thread (Decision 2) means the common sparse
  case fans out zero redundant encodes.

- **The fan-out is bounded** *(behavioral counter)*. New claim
  **`08-serialization#raster-parallel-save-encode-is-bounded`**: a new counter (peak
  outstanding encode jobs, exposed on the dispatch or the save report) never exceeds
  the window bound O(`worker_count`) across a save of an image with many more tiles
  than workers — asserted regardless of tile count, never against wall-clock or
  directory size. This is Constraint 4 and the O(`workers`·tile) memory bound.

- **Concurrency: TSan + seeded schedule-perturbation stress** *(tier 6, doc 16
  L64-73)*. Extend `tests/raster_tile_store_concurrency.t.cpp` (the existing
  save-races-writer TSan test): a **worker-backed** save runs while the writer thread
  paints and drops versions concurrently — TSan clean, and the produced document
  byte-identical to an inline save of the same pinned version. Add a **seeded
  schedule-perturbation** stress (randomized worker yields under a seed) over the
  encode fan-out asserting executor-independence across many schedules. Extend
  `src/runtime/t/worker_pool.t.cpp` for the generic lane: submit/settle/drain of
  work jobs, the accounting identity including work jobs, and inline (`worker_count
  == 0`) work-job execution. This is the explicit concurrency coverage the task note
  mandates.

- **Design-doc deltas (same commit)**. **Doc 02** §Threading model — a normative
  paragraph: the worker pool also runs a generic, cache-free work lane (the tile
  encode fan-out); the leaf-only rule governs the *render* lane, and the work lane's
  jobs touch only caller-owned buffers and the immutable pinned version, so *"workers
  never touch the cache"* holds for both. **Doc 08** Principle 8 — the per-tile
  encode may be fanned across workers; output is byte-identical and independent of
  completion order (hash over uncompressed storage bytes; `blobs` fixed row-major).
  **Doc 00** — one decision-record bullet: the runtime pool gains a generic work
  lane for the parallel tile-encode save, byte-identical to the serial path, decided
  in `serialize.tile_store_parallel_save`. **Doc 17 — no change** (WorkerPool is not
  named there); stated explicitly so the closer does not hunt for one.

- **No wall-clock assertion anywhere.** The speedup is a benchmark *trend*
  (doc 16 L225-226), never a merge gate. Every assertion in this task is a
  behavioral counter or a byte-exact diff.

- **Coverage / build / WBS gate**. ≥90% diff coverage on changed lines;
  `-Werror -Wpedantic` clean; `scripts/check_levels.py` green with **no `ALLOWED`
  edit**; `scripts/check_worker_dispatch.py` green with **no allowlist edit**;
  `scripts/check_claims.py` green (the two new claim rows + `enforces:`-tagged tests
  land together, and the re-parameterized existing claims still resolve);
  `tj3 project.tjp 2>&1 | grep -iE "error|warning"` silent.

- **Test locations** (area convention): the generic-lane unit tests extend
  `src/runtime/t/worker_pool.t.cpp`; the executor-independence golden, incremental
  counter parameterization, and bounded-in-flight counter live beside the serial
  goldens in `tests/raster_tile_store_golden.t.cpp` (or a sibling
  `tests/raster_tile_store_parallel.t.cpp` if that file is kept serial-only); the
  TSan race + seeded stress extend `tests/raster_tile_store_concurrency.t.cpp`.

- **No deferred WBS follow-ups.** Every primitive exists; the task is a restructure
  plus one contained pool seam. A priority/QoS split between the render lane and the
  save lane on a *shared* pool (so an autosave never delays an interactive frame
  beyond the bounded window) is **not** scoped here: the bounded in-flight window
  (Constraint 4) already caps the interference to O(`worker_count`) queued jobs, and a
  measured priority need would be a speculative future optimization, not a decidable
  unit today — surfaced in the return summary for the parking lot rather than encoded
  as a WBS leaf (never an "audit" task).

## Decisions

### 1. Add a generic work lane to the runtime `WorkerPool`; drive the fan-out from `runtime`, not `serialize`. *(doc 02 + doc 00 delta)*

The runtime pool's only submission seam takes a `RenderTask` — a `Content*`
rendering into a caller `Surface&` — and is guarded leaf-only by
`check_worker_dispatch.py`. A tile-encode job is neither a `Content` nor a render;
it cannot be expressed as a `RenderTask`. So the pool gains a **second lane**: a
generic movable-callable job that settles a caller-owned `WorkCompletion`, sharing
the pool's threads, its `poke`/`wait_completions`/`CompletionCursor` park
machinery, its `drain_owner` teardown, and its counters — but *not* the render
lane's leaf-only policy. The encode fan-out is driven from `codec_raster.cpp`
(runtime L5), which submits the pure `arbc::serialize` encode over each tile and
reaps by index.

*Rejected — spin a dedicated encode thread pool for the duration of a save.* It
would add a second concurrency surface (a second TSan story, a second set of
worker-warmed scratch), and it contradicts doc 15 L178-184 (*"Workers allocate only
from pools/arenas warmed for them; the writer thread is the only structural
allocator"*) and doc 02's *"one runtime pool"* story — the pool is deliberately the
single work executor the runtime owns and shares across viewports. Reusing its
threads is the coherent choice and the plain reading of the task note (*"across pool
workers"*).

*Rejected — model the encode as a `RenderTask` (a synthetic leaf `Content` that
"renders" a tile blob into a surface).* A gross abuse of the render abstraction:
the leaf-only guard exists precisely so render tasks touch only a caller surface and
never the cache, and smuggling an encode through it would either weaken that guard
or contort the encode into a fake surface render. A distinct lane keeps both
abstractions legible.

*Rejected — make `arbc::serialize` (L4) own the fan-out with a pool dependency.*
Forbidden by doc 17 (L4 → L5 is an upward edge); it would require widening
`serialize`'s edge set and editing `check_levels.py`. The pure encode already lives
in `serialize` and the driver in `runtime`; injecting the executor downward
(a `TileEncodeDispatch` functor, mirroring how `RenderDispatch` is injected into the
`PullService`) keeps the level split intact.

**Doc 02 delta**: the §Threading model paragraph named under Acceptance.
**Doc 00 bullet**: the generic work lane, byte-identical to the serial save.

### 2. All mutation on the save thread; ref-level dedup before dispatch. *(mirrors the audio pump)*

The one non-pure hazard in the serial loop is its stateful tail: the memo, the
`seen` set, `has_asset`, the positional `blobs`, and the sink. Rather than lock any
of them, the parallel form keeps **all** of it on the save/reap thread and hands the
workers only the pure transform — exactly the audio lookahead pump's shape
([`lookahead_pump.cpp:113-162`](../../../src/runtime/lookahead_pump.cpp)), which fans
per-item render and inserts on the driver thread into the single-thread-confined
cache. The save thread iterates tiles in row-major order, consults the **ref-keyed**
memo *before* dispatching (an untouched, ref-shared tile is a hit → no job, the
common sparse case costs zero encodes), and windows the dispatch of genuine misses
across workers; on reap it updates the memo, applies `seen`/`has_asset` dedup, writes
the surviving blob through the sink, and fills `blobs[i]`. No lock, no shared mutable
state on the hot path.

A subtlety this makes explicit: a *content*-duplicate tile whose two slots are **not**
ref-shared (two independently painted identical tiles — rare; ref-sharing catches the
common empty/flat case) may have its bytes compressed on a worker before the reap
thread's content-hash `seen` dedup discards the second copy. This wastes compression
CPU on that rare tile but produces **byte-identical output** (the sink still writes
one blob) and does not change `blobs_written` or `tiles_hashed` (both already count
per computed hash / per newly-written blob, which the serial path does identically).
The task's asserted counters are preserved; only unasserted, unmeasured CPU differs.

*Rejected — a two-stage pipeline (fan hashing, barrier-dedup on the save thread, then
fan compression of survivors) to eliminate that rare redundant compression.* It adds
a barrier and a second fan-out for a rare case the store's ref-sharing already
mostly eliminates, buying nothing the acceptance criteria measure. The single-stage
fan (hash + compress together from one `peek`, dedup on reap) is the simpler
abstraction and the byte-output is identical.

*Rejected — make the memo / sink internally thread-safe and let workers mutate them.*
It moves the correctness argument from *"no shared mutable state"* (checkable by
inspection) to *"every shared structure is correctly locked"* (checkable only by
TSan and reasoning), for no benefit — the mutations are cheap and serializing them on
the reap thread is not the bottleneck (the encode is). Keep the workers pure.

### 3. The serial save is the executor-independence oracle. *(doc 16 tier 3/4)*

Because `raster_tile_store` shipped byte-exact goldens, the parallel path is verified
by *equality to the serial path*, not by a fresh golden: the same document saved
inline and across N workers must diff to zero bytes (canonical `.arbc`, `blobs`
array, and every blob file). This is the strongest possible check — it pins that
parallelism is a pure throughput change — and it is a byte diff, never a tolerance
and never a timing.

### 4. Parallelizability is normative; record it in doc 08. *(doc 08 delta)*

Doc 08 Principle 8 currently describes the encode as a sequential per-blob transform
and is silent on threading. Since this task makes the fan-out a shipped property, the
doc gets a sentence stating the encode may be fanned across workers with
byte-identical, completion-order-independent output — the hash is over the tile's own
uncompressed storage bytes (already stated, L405-414) and the `blobs` array is fixed
row-major (L385), so the property follows from the format, not from careful
scheduling. This keeps the doc the executable spec (doc 16 L23-26: a change that
adds a designed behavior updates the governing doc in the same commit).

## Open questions

(none — all decided.)

## Status

**Done** — 2026-07-15.

- Added generic (non-render) work lane to `WorkerPool`: `submit_work`/`WorkTask`/`WorkCompletion`, second ready queue, `run()` drain, `drain_owner` purge, shared counters — `src/runtime/arbc/runtime/worker_pool.hpp`, `src/runtime/worker_pool.cpp`.
- New `TileEncodeDispatch` seam (inline + worker-backed, one algorithm, bounded in-flight) — `src/runtime/arbc/runtime/tile_encode_dispatch.hpp`, `src/runtime/tile_encode_dispatch.cpp`.
- Restructured `codec_raster.cpp` save: classify → fan pure encode (hash + compress on workers) → reap-by-index on save thread; all mutation stays on the save thread — `src/runtime/codec_raster.cpp`.
- `RasterTileStore` `probe`/`record` split (save-thread lookup, reap-thread miss commit) — `src/runtime/arbc/runtime/raster_tile_store.hpp`, `src/runtime/raster_tile_store.cpp`.
- `builtin_codecs.hpp`, `document_serialize.{hpp,cpp}` updated with dispatch-carrying overloads — `src/runtime/arbc/runtime/builtin_codecs.hpp`, `src/runtime/arbc/runtime/document_serialize.hpp`, `src/runtime/document_serialize.cpp`.
- Tests: generic-lane unit (`src/runtime/t/worker_pool.t.cpp`); executor-independence golden + incremental-counter + bounded-in-flight (`tests/raster_tile_store_parallel.t.cpp`); TSan save-races-writer + seeded schedule-perturbation stress (`tests/raster_tile_store_concurrency.t.cpp`).
- New claims `08-serialization#raster-save-is-executor-independent`, `#raster-parallel-save-encode-is-bounded`, `02-architecture#worker-pool-runs-a-generic-work-lane` — `tests/claims/registry.tsv`.
- Doc 00/02/08 deltas; CMakeLists updated — `docs/design/00-overview.md`, `docs/design/02-architecture.md`, `docs/design/08-serialization.md`, `src/runtime/CMakeLists.txt`, `tests/CMakeLists.txt`.
