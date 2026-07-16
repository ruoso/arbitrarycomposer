# serialize.tile_store_parallel_load — fan the per-tile decode pipeline across pool workers

## TaskJuggler entry

[`tasks/60-serialize.tji:88-93`](../../60-serialize.tji) — `task
tile_store_parallel_load "Fan per-tile decode pipeline across pool workers"`,
`effort 2d`, `depends !raster_tile_store, kinds.raster_tilewise_load`.

> Dispatch the per-tile decode pipeline (fetch -> decompress -> unshuffle ->
> verify hash) across pool workers with a bounded in-flight queue, mirroring
> `tile_store_parallel_save`. The pool's `allocate` is writer-only, so the
> parallel form is decode-on-workers / write-into-pool-on-the-loading-thread; the
> bounded queue preserves the O(tile)-per-in-flight-tile bound
> `kinds.raster_tilewise_load` establishes (peak becomes O(workers * tile), still
> independent of image size). Behavioral-counter and TSan coverage; no wall-clock
> assertion. Source-of-debt: `tasks/refinements/kinds/raster_tilewise_load.md`
> (Deferred follow-ups), commit `kinds.raster_tilewise_load`. Doc 08.

Milestone: `m9_release` ([`tasks/99-milestones.tji`](../../99-milestones.tji) —
listed by id in the `m9_release` `depends`, the sibling of
`serialize.tile_store_parallel_save`).

## Effort estimate

**2d**, apportioned:

| piece | days |
| --- | --- |
| `runtime` `TileDecodeDispatch` seam (inline + worker-backed impls, one algorithm, bounded in-flight window, `peak_in_flight()`/`window()`), mirroring `TileEncodeDispatch` | 0.75 |
| `runtime` restructure of `deserialize_raster`'s `fill` closure into a bounded look-ahead pump: fetch-then-submit-ahead on the loading thread, pure decode on workers, reap-by-index + copy-into-pool on the loading thread; dispatch-carrying `raster_codec`/`document_serialize` load overload | 0.5 |
| Executor-independence round-trip golden + bounded-decode counter + tilewise-bound-preserved parameterization; TSan worker-backed load + seeded schedule-perturbation stress; claims rows | 0.5 |
| Design-doc deltas (08, 00, 02), `-Werror`/level/`check_worker_dispatch`/claims/`tj3` gate, ≥90% diff coverage | 0.25 |

The 2d is **not** in the decode math — `decode_tile_blob` is already a pure,
reentrant function of one immutable frame's bytes (Inherited dependencies). It is
in the two seams the single-threaded load did not need: a `TileDecodeDispatch`
executor (a near-exact structural twin of the shipped `TileEncodeDispatch`), and a
bounded look-ahead driver in the `fill` closure that keeps every pool write on the
loading thread. The generic work lane the fan-out rides is **reused, not built** —
`serialize.tile_store_parallel_save` landed it (Inherited dependencies), so this
task is materially lighter than the save was, and the estimate holds only because
the seam and idiom already exist to copy.

## Inherited dependencies

### Settled

- **`kinds.raster_tilewise_load`** ([refinement](../kinds/raster_tilewise_load.md),
  done 2026-07-14) — the source-of-debt. It rewrote the load off a dense
  `w*h*16` working buffer onto a per-tile streaming fill, and handed forward every
  primitive this task parallelizes:
  - The tilewise load driver this task restructures:
    [`src/runtime/codec_raster.cpp:255-394`](../../../src/runtime/codec_raster.cpp)
    (`deserialize_raster`). Geometry is validated **before any allocation**
    (L276-294), then a `fill` closure (L313-364) per tile does: read the
    `blobs[t]` entry → validate it is a tile hash (L320-324) → `ctx.resolve` +
    `ctx.load_asset` the frame bytes (L328-330) → `decode_tile_blob` (L345-346) →
    `std::copy_n` the decoded floats into `dst` (L362), which is the pool blob's
    own memory. `RasterContent::from_tiles` (L366-367) drives the loop, calling
    `fill(t, dst)` in ascending row-major `t`; the memo is seeded on the loading
    thread afterward (L373-392).
  - The pure, stateless, `arbc::serialize`-owned (L4) decode entry safe to call
    from any worker: `decode_tile_blob(frame, expected_hash, storage, samples)`
    ([`tile_blob.hpp:149`](../../../src/serialize/arbc/serialize/tile_blob.hpp)) —
    it decompresses (`decompress_blob`, the reentrant one-shot zstd the save
    refinement confirmed pre-cleared for concurrent worker use,
    [`blob_compress.hpp:67-91`](../../../src/serialize/arbc/serialize/blob_compress.hpp)),
    unshuffles (`unshuffle_bytes`, pure), and **verifies the decompressed storage
    bytes hash to the fetched name** (`sha256`, stateless), returning the tile's
    working RGBA32F or a `TileBlobError` value on mismatch — never a throw, never
    silent wrong pixels (doc 08 Principle 8). This is the exact inverse of the
    save's `frame_tile_blob`, and equally pure.
  - The geometry gate: `validate_tile_geometry`
    ([`tile_blob.hpp:92`](../../../src/serialize/arbc/serialize/tile_blob.hpp)) →
    `TileGeometry` ([`tile_blob.hpp:68`](../../../src/serialize/arbc/serialize/tile_blob.hpp)),
    which sizes the decompression from the *declared* geometry, not the frame
    header (doc 08:440-442; claim `08-serialization#raster-tile-geometry-is-validated-before-allocation`,
    registry id 322). This runs on the loading thread **before** any dispatch, so
    no worker is ever handed an allocation sized by unchecked numbers.
  - The write-into-pool seam: `RasterContent::from_tiles`
    ([`raster_content.hpp:300`](../../../src/kind_raster/arbc/kind_raster/raster_content.hpp))
    → `RasterStore::build_from_tiles`
    ([`raster_content.hpp:218`](../../../src/kind_raster/arbc/kind_raster/raster_content.hpp))
    driving the byte-oriented `TileFill = std::function<bool(std::size_t, std::span<float>)>`
    ([`raster_content.hpp:75`](../../../src/kind_raster/arbc/kind_raster/raster_content.hpp)),
    which allocates one pool blob per tile on the loading thread and hands its
    memory to `fill`. `TileFill` names no `serialize`/`runtime` type (kind_raster
    is L4); this task **keeps that seam unchanged** and puts the parallelism inside
    the `fill` closure (runtime L5).
  - The behavioral bound this task must not silently break:
    `15-memory-model#raster-load-is-tilewise` (registry id 284) — the largest
    single load-path heap allocation ≤ one tile blob and the transient high-water
    is invariant in tile count, asserted over a counting global `operator new` at
    [`tests/raster_tilewise_load.t.cpp`](../../../tests/raster_tilewise_load.t.cpp).
    The parallel path's transient becomes O(`window` · tile) = O(`workers` · tile),
    still invariant in tile count; the existing bound is re-affirmed at
    `worker_count == 0` and complemented by a new bounded-decode claim (Decision 4).

- **`serialize.raster_tile_store`** ([refinement](raster_tile_store.md), done
  2026-07-14) — established the store, the memo, and the pool primitives the load
  reads:
  - The any-thread read primitive workers **do** use and the writer-only one they
    **must not**: `BigBlockPool::peek(BlockSlotRef) const noexcept`
    ([`big_block_pool.hpp:193`](../../../src/pool/arbc/pool/big_block_pool.hpp),
    zero-refcount-traffic read, any thread) is not even needed on the load fan-out
    (workers hold job-owned frame bytes, not pool slots); `allocate`
    ([`big_block_pool.hpp:174`](../../../src/pool/arbc/pool/big_block_pool.hpp)) is
    **writer-only, single-threaded** — so every pool allocation stays inside
    `build_from_tiles` on the loading thread (doc 15 L178-184: *"the writer thread
    is the only structural allocator"*).
  - The load-side memo seeding: `RasterTileStore::seed`
    ([`raster_tile_store.hpp:109`](../../../src/runtime/arbc/runtime/raster_tile_store.hpp))
    — unchanged, runs on the loading thread after the build, so a re-save after a
    load is a pure memo sweep regardless of the load executor.

- **`serialize.tile_store_parallel_save`** ([refinement](tile_store_parallel_save.md),
  done 2026-07-15) — the direct mirror; this task copies its shape and **reuses
  its shared mechanism**:
  - The generic (non-render) work lane on the runtime `WorkerPool` this task's
    decode jobs ride: `submit_work(WorkTask)`
    ([`worker_pool.hpp:181`](../../../src/runtime/arbc/runtime/worker_pool.hpp)),
    the caller-owned `WorkCompletion` ([`worker_pool.hpp:93`](../../../src/runtime/arbc/runtime/worker_pool.hpp)),
    the `WorkTask` ([`worker_pool.hpp:102`](../../../src/runtime/arbc/runtime/worker_pool.hpp)),
    `drain_owner` teardown, `worker_count == 0` inline execution, and the shared
    `tasks_submitted`/`tasks_completed`/`tasks_dropped` accounting identity. The
    lane is cache-free by construction (jobs touch only caller-owned buffers and
    the immutable pinned data); doc 02 §Threading model already normativizes it.
    **This task builds no new lane** — it is a second user of the one the save
    landed.
  - The executor-seam pattern to mirror: `TileEncodeDispatch`
    ([`tile_encode_dispatch.hpp`](../../../src/runtime/arbc/runtime/tile_encode_dispatch.hpp))
    — one class, two executors, one algorithm; default-constructed inline
    (byte-identical to the serial path, offline-export default), constructed over a
    `WorkerPool` it fans the pure job bounded to O(`worker_count`) in flight,
    reaping by index on the calling thread; `peak_in_flight()`/`window()`
    behavioral counters. `TileDecodeDispatch` is its structural twin (Decision 1).
  - The claims-parameterization pattern (existing claims re-run over
    `worker_count ∈ {0, N}`; a new bounded-in-flight claim) and the doc-delta
    ritual (doc 08 Principle 8 gains the parallelism sentence; doc 00 a
    decision-record bullet).

### Pending

(none — all three predecessors are `complete 100`.)

### A note on the WBS `depends` line

The `.tji` `depends` is `!raster_tile_store, kinds.raster_tilewise_load` — it does
**not** name `serialize.tile_store_parallel_save`, whose generic work lane this
task reuses. This is deliberate and matches the save refinement's own framing
(*"the two are independent parallelizations of the same store … whichever lands
first builds the lane; the load task reuses it"*): the two tasks are
order-independent, and `tile_store_parallel_save` is already `complete 100`, so the
lane and the `Tile*Dispatch` pattern exist for this task to copy. Were this task
ever to land first, it would build the lane itself (the save's Decision 1 work),
absorbing that 0.5d — but it will not, so the estimate reuses. **No `.tji` edit is
required or requested here** (the orchestrator/closer own the WBS shape); the
reuse is a compile-time fact, not a scheduling constraint.

### Downstream (this task unblocks)

Nothing in the WBS `depends` on `tile_store_parallel_load`; it is a leaf of
`m9_release`.

## What this task is

Take the per-tile load loop `kinds.raster_tilewise_load` shipped single-threaded
([`codec_raster.cpp:313-367`](../../../src/runtime/codec_raster.cpp)) and fan its
CPU-bound per-tile work — **decompress → unshuffle → verify-hash**, all computed
from one fetched immutable frame — across the runtime worker pool, while keeping
**every pool write on the loading thread**. `decode_tile_blob` is already a pure,
reentrant function of the frame bytes over the reentrant zstd decompressor, so the
parallel form is a mechanical restructure into the codebase's established
fan-out/reap-by-index shape (the audio look-ahead pump,
[`src/runtime/lookahead_pump.cpp:113-162`](../../../src/runtime/lookahead_pump.cpp)):
the loading thread fetches a bounded window of frames ahead, submits each pure
decode job, and reaps **by index** — copying the decoded floats into the tile's
pool blob and letting `build_from_tiles` allocate the slot — on the loading thread.

Because the runtime pool's decode jobs are neither `Content` nor renders, they ride
the **generic work lane** `serialize.tile_store_parallel_save` added to `WorkerPool`
(`submit_work`, distinct from the leaf-only `RenderTask` render lane) — reused
verbatim, no new lane. The decode jobs provably touch only their own job-owned
frame buffer and produce a job-owned float vector, never the cache, so doc 02's
*"workers never touch the cache"* discipline holds by construction, exactly as for
the save's encode jobs.

The **fetch stays on the loading thread**, not on the workers. The pure decode is
the parallelized part; the `LoadContext::resolve` + `AssetSource` read is the load's
mirror of the save's serial `AssetSink` write, and it stays single-threaded for the
same reason the sink write did — `LoadContext` is documented single-writer and the
asset source keeps non-atomic witness counters. This is a deliberate, minimal
refinement of the task note's *"fetch → … across pool workers"* wording; the note's
own load-bearing phrase is *"decode-on-workers / write-into-pool-on-the-loading-thread"*,
and the fetch is the third I/O party that, like the write, belongs on the one
thread (Decision 2, with the concrete blocker and the rejected alternative).

The fan-out is bounded: at most O(`worker_count`) decode jobs (and their
look-ahead frames) are in flight at once, so a load's transient scratch is
O(`worker_count` · tile), not O(image) — preserving the O(tile)-per-in-flight-tile
posture `kinds.raster_tilewise_load` established, now at O(`workers` · tile) but
still independent of image size. With `worker_count == 0` (the offline path, or any
inline host) the decode runs inline at `window == 1` — bit-for-bit the path
`kinds.raster_tilewise_load` already ships.

It does **not** change the on-disk format, the canonical `.arbc` bytes, the hash,
the storage format, or any codec-visible contract: a document loaded through the
worker-backed decode is byte-identical (every level-0 tile bit-identical, every
re-serialize byte-identical) to one loaded inline. It does **not** touch
`arbc::serialize` (L4), which stays pool-free; the whole fan-out is driven from
`arbc::runtime` (L5).

## Why it needs to be done

`kinds.raster_tilewise_load` made a raster document load in O(tile) transient
memory instead of O(image), but it decodes every tile **serially** on one thread:
for a large paint — the 30-layer 24 MP composition, thousands of level-0 tiles —
the zstd-decompress → unshuffle → SHA-256-verify chain is the load's dominant cost,
and it runs on a single core while the rest sit idle. The transform is
embarrassingly parallel (each tile decodes independently from its own frame; the
hash verify is over the tile's own uncompressed bytes,
[`08-serialization.md:405-414`](../../../docs/design/08-serialization.md); the
`blobs` array order is fixed row-major,
[`08:385`](../../../docs/design/08-serialization.md)), and the decompressor was
built reentrant *specifically* so this fan-out would be a data-race-free drop-in
([`blob_compress.hpp:67-72`](../../../src/serialize/arbc/serialize/blob_compress.hpp)).
`kinds.raster_tilewise_load` deferred it to *ship correct first*; this task collects
the deferred parallelism now that the single-threaded reference exists to prove the
parallel form byte-identical against, and now that the generic work lane
(`tile_store_parallel_save`) exists to carry it.

Opening a large document is a foreground operation — the wall-clock is a user's
wait on a blank canvas — so fanning the decode across the cores the runtime pool
already owns is a direct interactive win, and doing it now, while the serial path is
the oracle and the shared lane is in place, is when it is cheapest and safest.

## Inputs / context

### Design docs (normative, doc 16)

- **[`docs/design/08-serialization.md:369-490`](../../../docs/design/08-serialization.md)
  — Principle 8.** The per-tile decode this task parallelizes: content-hash keyed
  blobs, `blobs` a flat row-major level-0 array (L384-390); **the hash is over the
  tile's *uncompressed storage-format* bytes** (L405-414) — which is why each tile
  verifies independently and fans out soundly, and why *"a blob whose bytes do not
  hash to the name they were fetched under is a load error, never silent wrong
  pixels"*; per-blob zstd + byte-shuffle where *"a decode decompresses, unshuffles,
  and only then hashes"* (L435-443, the pipeline order this task preserves); mips
  rebuilt on load, not persisted (L415-419). The **parallel-encode bullet**
  (L444-458, added by `tile_store_parallel_save`) is the sentence this task mirrors
  on the decode side. **Doc 08 does not yet spell out parallel load** — the note
  observes the load is symmetric but the doc normativizes only the save — so this
  task carries a doc 08 delta making the decode parallelizability and its
  fetch-on-the-loading-thread shape normative (Decision 4).
- **[`docs/design/08-serialization.md:21-53`](../../../docs/design/08-serialization.md)
  — §The asset directory.** *"The core writes/reads asset bytes; the kind only
  encodes/decodes them"* — the `LoadContext`/`AssetSource` fetch seam. This task
  keeps the fetch on the loading thread, so no property of the asset source's
  single-threaded contract is disturbed. Bounded decompression of untrusted input
  (L440-442, geometry sizes the output, never the frame header) is validated on the
  loading thread before dispatch (Constraint 3).
- **[`docs/design/02-architecture.md:286-373`](../../../docs/design/02-architecture.md)
  — §Threading model.** The worker pool is a leaf-render executor whose leaf-only
  rule is *"what makes 'workers never touch the cache' true rather than
  aspirational"* (L301-314); **the pool also runs a generic, cache-free work lane**
  (`submit_work`, first used by the parallel tile-encode save, L315-332) whose jobs
  *"touch only their own caller-owned output buffer and the immutable pinned
  document version, never the tile cache"* — this task is a second user of that
  lane, and its doc 02 delta names the decode load alongside the encode save
  (Decision 4). `worker_count == 0` is the inline executor (L333-349), the offline
  default because *"byte-determinism is the whole point of an export."*
- **[`docs/design/15-memory-model.md:20,38-45,83-105,178-184,361-369`](../../../docs/design/15-memory-model.md).**
  Tile pixels are *"immutable once filled"* and refcount-shared (L20); a
  `peek`/`const&` traversal is interference-free (claim
  `15-memory-model#interference-free-concurrent-pin`, L83-105) — though on load the
  workers hold job-owned frame bytes, not pool slots, so they do not even pin.
  **L178-184 is the shaping constraint:** *"Workers allocate only from
  pools/arenas warmed for them. The writer thread is the only structural
  allocator."* — so decode workers never call `BigBlockPool::allocate`; the pool
  write stays on the loading thread (Constraint 3). L361-369 is the O(tile)-per-unit
  posture the bounded window preserves at O(`workers` · tile).
- **[`docs/design/16-sdlc-and-quality.md`](../../../docs/design/16-sdlc-and-quality.md)**
  — claims register + `enforces:` tags, same-commit doc delta (L14-26); **tier-4
  behavioral counters, never wall-clock** (L54-62; L225-226); **tier-6 concurrency:
  TSan on the full suite + dedicated stress with seeded schedule perturbation**
  (L64-73); diff coverage ≥90% hard gate (L112-118).
- **[`docs/design/17-internal-components.md:52,69,71,84`](../../../docs/design/17-internal-components.md)**
  — the level rule (L52); `arbc::serialize` is **L4** (L69), `arbc::runtime` is
  **L5** (L71); do not widen an edge unless a genuinely new direct dependency
  requires it (L84). `WorkerPool`/`Tile*Dispatch` are classes *inside* `runtime`,
  not named in doc 17, so this task needs **no doc 17 change** and **no
  `scripts/check_levels.py` edit** — the dispatch, the driver, and the pool are all
  L5; the pure `decode_tile_blob` stays L4.

### Source seams

- **[`src/runtime/codec_raster.cpp:255-394`](../../../src/runtime/codec_raster.cpp)** —
  `deserialize_raster`, the load driver to restructure. Geometry validation
  (L276-294) and the `blobs`-array-length check (L286-294) stay on the loading
  thread, before any dispatch. The `fill` closure (L313-364) is rewritten from
  *decode-inline-per-call* into a bounded look-ahead pump: on `fill(t, dst)` the
  loading thread advances the fetch/submit window (validate entry L316-324, resolve
  + `load_asset` the frame L328-330, submit the pure decode job), waits for job
  `t`'s completion, and `std::copy_n` its floats into `dst` (L362). `from_tiles`
  (L366-367) and the memo seed (L373-392) are unchanged.
- **[`src/serialize/arbc/serialize/tile_blob.hpp:149`](../../../src/serialize/arbc/serialize/tile_blob.hpp)**
  (`decode_tile_blob`) and
  **[`blob_compress.hpp:67-91`](../../../src/serialize/arbc/serialize/blob_compress.hpp)**
  (`decompress_blob`, reentrant) — the pure decode functions, **unchanged**; the
  worker calls `decode_tile_blob(frame, expected_hash, storage, samples)` and gets
  floats-or-`TileBlobError`.
- **[`src/runtime/arbc/runtime/tile_encode_dispatch.hpp`](../../../src/runtime/arbc/runtime/tile_encode_dispatch.hpp)** —
  the exact template for the new `tile_decode_dispatch.{hpp,cpp}`: `struct
  TileDecodeOutput { std::vector<float> pixels; std::optional<TileBlobError> error; }`
  (mirror of `TileEncodeOutput`), `DecodeFn = std::function<TileDecodeOutput(std::size_t)>`,
  `ReapFn = std::function<bool(std::size_t, TileDecodeOutput&)>`, `run(count,
  decode, reap)`, `peak_in_flight()`, `window()`, inline + `WorkerPool&`-backed
  ctors, `drain_owner`-keyed teardown.
- **[`src/runtime/arbc/runtime/worker_pool.hpp:93,102,181`](../../../src/runtime/arbc/runtime/worker_pool.hpp)** —
  the generic lane (`WorkCompletion`, `WorkTask`, `submit_work`) the decode jobs
  ride, reused unchanged; the render lane (`RenderTask` at L59, `submit` at L169)
  and its leaf-only guard are untouched.
- **[`src/runtime/lookahead_pump.cpp:113-162`](../../../src/runtime/lookahead_pump.cpp)** —
  the bounded look-ahead / reap-by-index idiom to mirror: submit a window ahead,
  park on completion, consume in order on the driver thread.
- **[`src/serialize/arbc/serialize/load_context.hpp:72-73,99,117-123`](../../../src/serialize/arbc/serialize/load_context.hpp)**
  and
  **[`src/serialize/load_context.cpp:88-106`](../../../src/serialize/load_context.cpp)** —
  `LoadContext` is *"Single-writer, not thread-safe"* (L72-73); `resolve` mutates
  the `d_resolved`/`d_by_uri` dedup cache with no lock (L88-98); `set_asset_source`
  is *"WRITER-THREAD ONLY"* (L99). This is the concrete reason the fetch stays on
  the loading thread (Decision 2).
- **[`src/runtime/arbc/runtime/filesystem_asset_source.hpp:41,56-57`](../../../src/runtime/arbc/runtime/filesystem_asset_source.hpp)**
  and
  **[`src/runtime/filesystem_asset_source.cpp:27-59`](../../../src/runtime/filesystem_asset_source.cpp)** —
  the fetch backing: *"Single-threaded, like the load it serves"* (hpp:41); each
  `request` opens a **fresh** `std::ifstream` and a **fresh** buffer (cpp:27-59, so
  the *byte read* is independent per blob), but bumps non-atomic witness counters
  `d_requests`/`d_hits` (hpp:56-57). Keeping the fetch on the loading thread avoids
  racing those counters and avoids asserting a concurrent-read contract the class
  disclaims (Decision 2).
- **[`src/runtime/document_serialize.cpp`](../../../src/runtime/document_serialize.cpp)** —
  `load_document` (~L668-686) constructs the one `LoadContext` and installs the
  source; the save path already threads a `TileEncodeDispatch*` (~L187-195). This
  task adds the symmetric load wiring: a `TileDecodeDispatch` constructed over the
  pool (inline when the host is inline) and threaded into `deserialize_raster`, via
  a dispatch-carrying `raster_codec` load overload mirroring the save's
  `raster_codec(RasterTileStore*, TileEncodeDispatch*)`
  ([`codec_raster.cpp:402-414`](../../../src/runtime/codec_raster.cpp)).

### Predecessor / sibling refinements

- [`../kinds/raster_tilewise_load.md`](../kinds/raster_tilewise_load.md) — the O(tile)
  bound (claim id 284), the synchronous-no-pending-state decision (a painted layer
  whose tiles have not arrived is a *broken document*, not a pending one — this
  task streams for **memory/throughput**, not progressiveness, so it preserves the
  synchronous, verify-or-fail contract), and the Deferred follow-ups block that
  registers **this task** with its 2d estimate and behavioral-counter+TSan mandate.
- [`tile_store_parallel_save.md`](tile_store_parallel_save.md) — the direct mirror:
  the generic work lane, the `Tile*Dispatch` one-class/two-executor/one-algorithm
  seam, the all-mutation-on-one-thread discipline, the serial-path-is-the-oracle
  determinism check, the claims-parameterization + doc-delta ritual, and the
  explicit no-wall-clock rule. Its Decision 1 (add the lane; drive from `runtime`,
  not `serialize`) and Decision 3 (serial is the executor-independence oracle) apply
  verbatim to the decode side.
- [`raster_tile_store.md`](raster_tile_store.md) — the writer-only `allocate` /
  any-thread `peek` split and the refcount-pinned memo the load seeds.

## Constraints / requirements

1. **Byte-identical to the single-threaded load, under any worker count.** A
   document loaded through a worker-backed `TileDecodeDispatch` (N > 1) yields a
   `TileTable` whose every level-0 tile is bit-identical to one loaded inline
   (`worker_count == 0`), and re-serializing the two loaded documents yields
   byte-identical canonical `.arbc` JSON and an identical `params.blobs` array. This
   is the load-bearing correctness property; the inline load
   `kinds.raster_tilewise_load` shipped is the oracle (Decision 3, Acceptance claim
   `#raster-load-is-executor-independent`). It follows from the format, not from
   scheduling: the reap is strictly by ascending index, and the decode is a pure
   function of the frame — completion order cannot leak into the tile table.

2. **All pool writes and all fetch/resolve stay on the loading thread; workers run
   pure decode jobs only.** A decode job reads its **job-owned** frame bytes (moved
   into the job at submit) and writes only into its **own** output `std::vector<float>`
   (the decoded tile) plus an optional `TileBlobError`. The `ctx.resolve` +
   `ctx.load_asset` fetch, the `std::copy_n` into the pool blob, the
   `build_from_tiles` allocation, and the memo seed happen only on the loading
   thread. No new shared mutable state, no lock on the hot path — the fan-out is
   sound by construction, exactly as the save's.

3. **No pool allocation on workers; geometry validated before dispatch.** Workers
   never call `BigBlockPool::allocate` (writer-only, doc 15 L178-184); the pool slot
   for tile `t` is allocated inside `build_from_tiles` on the loading thread and its
   memory handed to the reap. The `edge`/`width`/`height` and the `blobs`-array
   length are validated (`validate_tile_geometry`, the length check) on the loading
   thread **before** any decode job is submitted, so no worker is handed a
   `samples` sized by unchecked numbers (claim id 322 preserved).

4. **Bounded in-flight: peak decode jobs ≤ O(`worker_count`).** The fan-out is a
   windowed fetch/submit/reap look-ahead, not fetch-all-then-decode-all: at most a
   small multiple of `worker_count` decode jobs (and their look-ahead frame
   buffers) are outstanding at once, so a load's transient scratch is
   O(`worker_count` · tile) independent of image size (the O(tile)-per-in-flight
   posture of `kinds.raster_tilewise_load`, doc 15 L361-369), and opening a document
   cannot flood the shared render pool with an image's worth of queued jobs.
   Asserted by a behavioral counter (`TileDecodeDispatch::peak_in_flight()`), never
   wall-clock.

5. **Inline degeneracy is the shipped path.** With `worker_count == 0` (offline
   default, or any inline host) the decode runs on the loading thread at
   `window == 1` — fetch, decode, copy, one tile at a time in row-major order,
   bit-identical to `codec_raster.cpp:313-367` today, and preserving the strict
   ≤ one-tile transient the existing tilewise bound (id 284) asserts. The
   worker-backed and inline `TileDecodeDispatch` impls are the same algorithm with a
   different executor; determinism is testable by diffing the two.

6. **The self-verifying-blob contract survives the boundary.** `decode_tile_blob`
   verifies the hash on the worker and returns a `TileBlobError` **value** on
   mismatch (truncated/bit-flipped/substituted blob); the loading thread turns the
   first failed completion into the same `ReaderError` (`MalformedField
   /params/blobs`) the serial path returns
   ([`codec_raster.cpp:347-353`](../../../src/runtime/codec_raster.cpp)) and aborts
   the build (`fill` returns false → `from_tiles` abandons, every allocated pool
   blob returned). A missing/empty fetched frame is an `UnresolvableReference` on
   the loading thread (L331-341), before any decode job — unchanged. No exception
   crosses the worker boundary.

7. **Fail-fast drains outstanding jobs.** A load that aborts mid-fan-out (decode
   error, fetch failure, or malformed entry) drains every outstanding decode job
   (`drain_owner`) before returning the error, so no worker outlives a reference to
   a freed frame buffer — mirroring the save's Constraint 7.

8. **The generic work lane's invariants are untouched.** The decode jobs ride the
   existing `submit_work` lane unchanged; the pool's accounting identity
   `tasks_submitted == tasks_completed + tasks_dropped + <outstanding>` at
   quiescence must still close with decode jobs included, and the run must be
   TSan-clean under the existing per-push `gcc-tsan` lane
   ([`.github/workflows/ci.yml`](../../../.github/workflows/ci.yml)). The
   `RenderTask` leaf-only render policy and `scripts/check_worker_dispatch.py` are
   **not** touched (decode jobs are not `RenderTask`s).

9. **Levelization (doc 17, CI-enforced).** The pure decode stays in `arbc::serialize`
   (L4); the `TileDecodeDispatch` seam and the look-ahead driver are in
   `arbc::runtime` (L5), intra-component with `WorkerPool` and `codec_raster.cpp`.
   **`arbc::serialize` gains no pool edge**, and `scripts/check_levels.py` `ALLOWED`
   is **not** edited — an edit there means the L4/L5 split was violated.

## Acceptance criteria

- **Worker-backed and inline loads are byte-identical** *(golden + behavioral)*.
  New claim **`08-serialization#raster-load-is-executor-independent`**: loading one
  saved document through a worker-backed `TileDecodeDispatch` (N > 1 workers) and
  through the inline dispatch produces a `TileTable` whose every level-0 tile is
  bit-identical, and re-serializing each yields byte-identical canonical `.arbc`
  bytes and an identical `params.blobs` array. Enforces-tagged; the inline load is
  the oracle and the worker-backed run is diffed against it. This is Constraint 1.

- **Round-trip stays byte-exact under a worker-backed load** *(behavioral)*. The
  existing round-trip claim `08-serialization#raster-tile-store-round-trips-byte-exactly`
  (registry id 318) gains a **load `worker_count ∈ {0, N}`** parameterization:
  `load(save(D))` yields a bit-identical tile table and a zero-blob, zero-hash
  re-save (the memo seed is executor-independent) whether the load ran inline or
  across N workers. Pins that the decode executor changes throughput, never the
  loaded pixels or the memo state.

- **The decode fan-out is bounded** *(behavioral counter)*. New claim
  **`08-serialization#raster-parallel-load-decode-is-bounded`** (mirror of
  `#raster-parallel-save-encode-is-bounded`, id 332): across a load of an image with
  many more tiles than workers, the peak outstanding decode jobs
  (`TileDecodeDispatch::peak_in_flight()`) never exceeds `window()` — O(`worker_count`),
  exactly 1 inline — so the transient scratch is O(`worker_count` · tile),
  asserted regardless of tile count, never against wall-clock or RSS. This is
  Constraint 4.

- **The tilewise O(tile) bound is preserved, not regressed** *(behavioral counter)*.
  `15-memory-model#raster-load-is-tilewise` (id 284) is re-affirmed at
  `worker_count == 0` (largest single allocation ≤ one tile blob + fixed slack, and
  transient high-water within one tile, both invariant in tile count — unchanged).
  The counting-`operator new` test at
  [`tests/raster_tilewise_load.t.cpp`](../../../tests/raster_tilewise_load.t.cpp)
  gains a `worker_count == N` case asserting the transient high-water stays within
  **O(`window` · tile)** and **invariant in tile count** (4×4 vs 16×16), so the
  parallel path never becomes O(image). This pins that bounding is preserved, not
  merely un-broken.

- **Concurrency: TSan + seeded schedule-perturbation stress** *(tier 6, doc 16
  L64-73)*. Add a **worker-backed load** under TSan: the loading thread fetches +
  allocates + reaps while N workers decode concurrently — TSan clean, and the
  produced document byte-identical to an inline load of the same file. (There is no
  concurrent *writer* to race, unlike the save's editor thread — a load builds a
  fresh content whose only writer is the loading thread; the concurrency surface is
  the decode workers + the completion machinery, which the generic lane already
  covers.) Add a **seeded schedule-perturbation** stress (randomized worker yields
  under a seed) over the decode fan-out asserting executor-independence across many
  schedules. These extend
  [`tests/raster_tile_store_concurrency.t.cpp`](../../../tests/raster_tile_store_concurrency.t.cpp).
  The generic-lane unit coverage (`src/runtime/t/worker_pool.t.cpp`) already exists
  from `tile_store_parallel_save`; no new lane tests are needed.

- **Design-doc deltas (same commit)**. **Doc 08** Principle 8 — the per-tile
  **decode** may be fanned across workers, byte-identically and independent of
  completion order (hash verify over uncompressed storage bytes; `blobs` fixed
  row-major; reap strictly by index); the pure decode (decompress → unshuffle →
  verify-hash) runs on workers while the fetch and the pool write stay on the
  loading thread. **Doc 02** §Threading model — extend the existing generic-work-lane
  paragraph to name the tile-**decode load** as a second user of the lane alongside
  the encode save. **Doc 00** — one decision-record bullet: the parallel
  tile-decode load reuses the generic work lane, byte-identical to the serial load,
  fetch on the loading thread; decided in `serialize.tile_store_parallel_load`.
  **Doc 17 — no change** (Dispatch/WorkerPool are not named there); stated
  explicitly so the closer does not hunt for one.

- **No wall-clock assertion anywhere.** The speedup is a benchmark *trend*
  (doc 16 L225-226), never a merge gate. Every assertion is a behavioral counter or
  a byte-exact diff.

- **Coverage / build / WBS gate**. ≥90% diff coverage on changed lines;
  `-Werror -Wpedantic` clean; `scripts/check_levels.py` green with **no `ALLOWED`
  edit**; `scripts/check_worker_dispatch.py` green with **no allowlist edit**;
  `scripts/check_claims.py` green (the two new claim rows + `enforces:`-tagged tests
  land together, and the re-parameterized id 284/318 still resolve);
  `tj3 project.tjp 2>&1 | grep -iE "error|warning"` silent.

- **Test locations** (area convention): the executor-independence golden, the
  round-trip load-parameterization, and the bounded-decode counter live beside the
  save's parallel tests in
  [`tests/raster_tile_store_parallel.t.cpp`](../../../tests/raster_tile_store_parallel.t.cpp)
  (created by `tile_store_parallel_save`; extend with the load cases); the tilewise
  O(tile) parameterization extends
  [`tests/raster_tilewise_load.t.cpp`](../../../tests/raster_tilewise_load.t.cpp);
  the TSan load + seeded stress extend
  [`tests/raster_tile_store_concurrency.t.cpp`](../../../tests/raster_tile_store_concurrency.t.cpp).

- **No deferred WBS follow-ups.** Every primitive exists (the lane, the pure decode,
  the geometry gate, the memo seed); the task is a restructure plus one contained
  dispatch seam. Fetch-on-workers for a hypothetical slow/remote `AssetSource`
  (which would overlap I/O latency) is **not** scoped: no such source exists, the
  only backing is the local filesystem where the byte read is fast and page-cached,
  and putting it on workers would impose a concurrent-read contract the `LoadContext`
  and `AssetSource` explicitly disclaim (Decision 2). A measured need for it would
  be a speculative future optimization, not a decidable unit today — surfaced in the
  return summary for the parking lot rather than encoded as a WBS leaf (never an
  "audit" task).

## Decisions

### 1. Add a `TileDecodeDispatch` mirroring `TileEncodeDispatch`; reuse the generic work lane; drive from `runtime`, not `serialize`. *(doc 08 + doc 00 delta)*

The decode load fans through a new `TileDecodeDispatch`
([`tile_decode_dispatch.{hpp,cpp}`], structural twin of the shipped
[`tile_encode_dispatch.hpp`](../../../src/runtime/arbc/runtime/tile_encode_dispatch.hpp)):
one concrete class, two executors, one algorithm — default-constructed it is the
inline executor (byte-identical to the serial load, offline default), constructed
over a `WorkerPool` it fans the pure decode across the pool's **generic work lane**
(`submit_work`, reused from `tile_store_parallel_save`) bounded to O(`worker_count`)
in flight, reaping by index on the loading thread. The pure `decode_tile_blob` stays
in `arbc::serialize` (L4, gains no pool edge); the dispatch and the look-ahead driver
live in `arbc::runtime` (L5), injected downward into `codec_raster.cpp` exactly as
`TileEncodeDispatch` is on the save side.

*Rejected — a second, decode-specific thread pool.* A second concurrency surface, a
second TSan story, contradicting doc 02's *"one runtime pool"* and doc 15 L178-184;
the generic lane already exists and is the coherent, plain reading of *"across pool
workers."*

*Rejected — make `arbc::serialize` (L4) own the fan-out with a pool dependency.*
Forbidden by doc 17 (L4 → L5 is upward); injecting the executor downward (a
`TileDecodeDispatch` functor threaded into the codec) keeps the level split intact,
mirroring the save.

### 2. Fetch (resolve + load_asset) stays on the loading thread; only the pure decode fans out. *(the primary refinement of the task note)*

The task note lists *"fetch → decompress → unshuffle → verify hash across pool
workers,"* but its load-bearing phrase is *"decode-on-workers /
write-into-pool-on-the-loading-thread."* Two pieces of shared mutable state make a
literal fetch-on-workers a data race as the code stands:

- **`LoadContext::resolve` mutates an unsynchronized dedup cache** — a
  `d_resolved.push_back()` + `d_by_uri.emplace()` per miss with no lock
  ([`load_context.cpp:88-98`](../../../src/serialize/load_context.cpp)); the class
  documents itself *"Single-writer, not thread-safe"*
  ([`load_context.hpp:72-73`](../../../src/serialize/arbc/serialize/load_context.hpp)).
- **`FilesystemAssetSource::request` bumps non-atomic witness counters**
  `d_requests`/`d_hits`
  ([`filesystem_asset_source.hpp:56-57`](../../../src/runtime/arbc/runtime/filesystem_asset_source.hpp)),
  and documents itself *"Single-threaded, like the load it serves"* (hpp:41).

So the fetch is the load's mirror of the save's serial `AssetSink` write, and it
stays on the one thread for the same reason the sink write did. This is the minimal,
precedent-consistent call: the *byte read itself* is already independent per blob (a
fresh `ifstream` per `request`,
[`filesystem_asset_source.cpp:27-59`](../../../src/runtime/filesystem_asset_source.cpp)),
so the parallelized decompress+unshuffle+verify — the CPU-dominant part, exactly as
compress+hash was on save — captures the throughput win, while nothing in the
single-threaded read path is disturbed. Keeping the fetch serial also keeps the
`FilesystemAssetSource` counters off the concurrency surface entirely, so this task
adds **zero** new TSan surface to the asset source.

*Rejected — resolve up-front on the loading thread, then fetch (`load_asset`) +
decode on workers, making the source's counters atomic.* This would overlap I/O
latency, but it (a) imposes a concurrent-read contract on the `LoadContext`/
`AssetSource` seam that both classes explicitly disclaim — a design-level widening
of doc 08's asset-source contract, (b) buys ~nothing for the only extant backing
(local filesystem, fast and OS-page-cached), and (c) adds a genuine concurrency
surface to the read path for a speculative benefit. This is the same reasoning the
save used to keep its sink serial. If a slow/remote `AssetSource` ever lands, a
concurrent-read source + fetch-on-workers can be revisited then (parking lot, not a
WBS task) — it is not a decidable, agent-implementable unit today.

### 3. The serial load is the executor-independence oracle. *(doc 16 tier 3/4)*

Because `kinds.raster_tilewise_load` shipped a correct, tested serial load, the
parallel path is verified by *equality to the serial path*: the same file loaded
inline and across N workers must produce bit-identical tile tables and byte-identical
re-serializations. This is the strongest possible check — it pins that the decode
executor is a pure throughput change — and it is a byte/bit diff, never a tolerance
and never a timing. It is sound because the reap is strictly by index and the decode
is pure over the frame, so completion order provably cannot reach the tile table.

### 4. Parallel decode is normative; record it in doc 08 (and name the second lane user in doc 02). *(doc 08 + doc 02 + doc 00 delta)*

Doc 08 Principle 8 currently normativizes the parallel *encode* (L444-458) and is
silent on the decode. Since this task makes the decode fan-out a shipped property,
the doc gets the symmetric sentence: the per-tile decode may be fanned across
workers with byte-identical, completion-order-independent output — the hash verify
is over the tile's own uncompressed storage bytes and the `blobs` array is fixed
row-major, so the property follows from the format, not from scheduling; the fetch
and the pool write stay on the loading thread. Doc 02's generic-work-lane paragraph
gains the decode load as a second named user of the lane, and doc 00 gets a
decision-record bullet. This keeps the docs the executable spec (doc 16 L14-26:
a change that adds a designed behavior updates the governing doc in the same commit).

## Open questions

(none — all decided.)

## Status

**Done** — 2026-07-15.

- `TileDecodeDispatch` seam added (`src/runtime/arbc/runtime/tile_decode_dispatch.hpp`, `src/runtime/tile_decode_dispatch.cpp`) — structural twin of `TileEncodeDispatch`: one class, two executors (inline `window==1` + `WorkerPool`-backed), bounded in-flight look-ahead, `peak_in_flight()`/`window()` behavioral counters, `drain_owner` teardown; reuses the generic work lane (`submit_work`) from `tile_store_parallel_save`.
- `deserialize_raster` fill closure restructured into a bounded look-ahead pump (`src/runtime/codec_raster.cpp`): fetch + submit ahead on the loading thread, pure decode on workers, reap-by-index + `std::copy_n` into pool blob on the loading thread; geometry validated before any dispatch.
- `document_serialize.cpp` / `document_serialize.hpp` wired with dispatch-carrying `raster_codec` load overload, symmetric to the save-side `TileEncodeDispatch*` injection.
- `builtin_codecs.hpp`, `CMakeLists.txt` updated to register the new decode dispatch source.
- Tests added in `tests/raster_tile_store_parallel.t.cpp` (executor-independence golden, round-trip `worker_count∈{0,N}` parameterization, bounded-decode counter), `tests/raster_tilewise_load.t.cpp` (O(`window`·tile) tilewise bound, parallel-path invariance in tile count), `tests/raster_tile_store_concurrency.t.cpp` (TSan worker-backed load, seeded schedule-perturbation stress).
- Claims `08-serialization#raster-load-is-executor-independent` and `08-serialization#raster-parallel-load-decode-is-bounded` registered in `tests/claims/registry.tsv` with `enforces:` tags; ids 284 and 318 re-parameterized over `worker_count`.
- Design-doc deltas landed in `docs/design/08-serialization.md` (parallel decode normative), `docs/design/02-architecture.md` (decode load named as second generic-lane user), `docs/design/00-overview.md` (decision-record bullet).
