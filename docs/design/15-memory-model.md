# 15 — Memory Model

Doc 14's versioned document turns memory management into the design's next
load-bearing wall: every transaction creates records, every unpin
potentially frees a cascade of them, and three thread classes with very
different rules (writer, render workers, realtime audio) touch the same
structures. "When do we garbage collect old revisions" must have a cheap,
deterministic answer. This doc gives the model, and evaluates the
`poc-inside-out-objects` prototype (cpioo) as its foundation.

## Memory populations

Different lifetimes and access patterns want different allocators; the
first design act is refusing a single one:

| Population | Examples | Size/churn | Readers | Allocator |
| --- | --- | --- | --- | --- |
| **Document records** | DocState map nodes, composition/layer/content records (doc 14) | small (≤ ~256 B), fixed per type, high churn per transaction | render/audio-engine threads traverse pinned versions lock-free | **inside-out slabs** (this doc's subject) |
| **Content state nodes** | persistent tile *tables*, vector tree nodes (`Editable` states) | small, kind-defined, churn per gesture | render workers via pinned `StateHandle`s | same slabs, offered to kinds as a core utility |
| **Bulk media data** | raster tile pixels, decoded frames, audio sample runs | page-scale blobs, immutable once filled | workers, backends | dedicated big-block pool (page-aligned, size-classed), refcounted from the owning nodes |
| **Cache values** | composed tiles, audio blocks (docs 02/12), surfaces (doc 09) | backend-owned, budgeted, LRU | render/audio | backend pools, already designed; budgets are the eviction policy |
| **Frame transients** | frame plans, request lists, culling scratch | tiny, per-frame | one thread each | bump arenas, reset per frame, never freed piecemeal |

The rest of this doc is about the first two rows — the version-structured
data — because that is where "GC of old revisions" lives.

## Evaluation: `poc-inside-out-objects` (cpioo)

What the prototype does (read from `src/cpioo/managed_entity.hpp`,
`t/003_deeply_nested.t.cpp`, `src/benchmark/benchmark.cpp`):

- **Type-segregated slab storage**: per-type `storage<T>` growing as a
  two-level table (superbuffer → fixed 2^k-element buffers). Slots are
  fixed-size, so a freed slot is a perfect hole for the next same-type
  allocation — **fragmentation is structurally impossible**, and the
  allocation fast path is pop-from-freelist.
- **Inside-out refcounts**: refcounts live in *parallel* buffers, not next
  to the data. After construction, data pages are never written again —
  reader threads traversing a version touch only genuinely immutable
  pages (no refcount-dirtied cache lines, page-cache friendly, safely
  shareable across cores).
- **References are (pointer, index)** with the convention that traversal
  passes `const&` — only ownership points bump counts, so *reads don't
  touch refcount pages at all*.
- **Thread-local free pools** with spill to a global pool: release is
  push-to-local-queue, reuse is thread-affine.
- Benchmarked against `shared_ptr` on exactly our topology — a producer
  thread publishing immutable tree versions while a consumer traverses:
  the prototype reported ~2.4–3× version-production rate, ~16–18×
  traversal rate. **Those figures do not survive an honest rerun against
  the shipped `arbc::pool`** — see caveat 3 and `src/pool/bench/`.

**Fit assessment: this is the right model for the document records.** The
correspondence is one-to-one — doc 14's writer is the producer, pinned
`DocState`s are the published roots, render planning is the consumer
traversal, and doc 02's "reads never take a lock" is exactly the
immutable-pages property. The fixed-slot/no-fragmentation property is what
an editor that runs for days needs. And the refcount-at-a-distance layout
answers a subtlety doc 14 skated over: with `make_shared`-style layouts,
*pinning and unpinning versions from the render thread would dirty the
same cache lines the traversal reads*; inside-out storage makes version
pins interference-free.

**Honest caveats from the code review**, all fixable, none structural:

1. **Releases never run `~T`** (`refcnt_subtract` pushes the slot to the
   free pool without destroying the object; `make_entity` placement-news
   over it). For node types holding child references — our entire use
   case — child refcounts are never decremented on reuse: a leak. The fix
   interacts nicely with reclamation design (below): destruction becomes
   an explicit, deferrable step rather than an implicit recursive one.
2. **Storage is `inline static`** — a global singleton per type. The
   library needs instance arenas: per-document (or per-core-instance)
   ownership, per-arena accounting, and O(1) whole-document teardown by
   dropping the arena. Statics also fight plugin `dlopen` lifetimes
   (doc 03) and multi-document hosts (doc 14).
3. **The benchmark flatters the traversal number**: the `shared_ptr`
   visitor takes its argument *by value* (refcount traffic per node per
   visit) while the managed visitor takes `const&`. The architectural
   advantages (slab locality, undirtied pages, no control-block
   interleaving) are real — and the undirtied-pages one is now
   machine-checked: `15-memory-model#const-ref-traversal-touches-no-refcount-page`
   proves a `peek`/`const&` traversal never writes a count page (the
   refcount table `mprotect`ed read-only does not fault), which a by-value
   `shared_ptr` walk would. But the honest rerun
   (`src/pool/bench/allocator_bench.cpp`, `arbc::pool` vs `std::shared_ptr`,
   GCC 14 Release) shows the 16× was almost entirely the by-value artifact:
   a by-value walk costs only ~3× a `const&` walk, and the managed `peek`
   traversal lands at roughly *that same* by-value cost — i.e. ~2.8×
   *slower* than a fair `const&` `shared_ptr` walk, single-threaded, not
   faster. The reason is a real tradeoff, not a defect: arbc's in-record
   reference is the 4-byte index-only `SlotRef` (doc 15 chose it to shrink
   persistent-map nodes and stay mmap-portable), so `peek` pays a two-level
   directory resolve per edge that cpioo's fat 24-byte (pointer+index)
   `reference` — and the flattering by-value baseline — both hid. So no
   traversal *speedup* is claimed: the design's read-path win is the
   *interference-free concurrent pin* (a producer pinning/unpinning versions
   never dirties the cache lines the consumer reads — the property caveat's
   `const-ref` claim guards, and now the concurrent
   `BM_ConcurrentPin_Managed`/`_Shared` benches in `src/pool/bench/` measure it
   against the `make_shared` co-located-control-block baseline, with the
   page-disjointness under contention machine-checked by the
   `15-memory-model#interference-free-concurrent-pin` claim), plus
   fragmentation-free reuse and O(1) arena teardown, none of which a
   single-threaded traversal ratio captures. Numbers trend per-commit through
   the benchmark's JSON output, never a quoted ratio in the docs (doc
   16:82-87, 225-226).
4. Production hardening: default `REFCNT_TYPE = short` (32 k pins
   overflows silently — needs `uint32_t` + overflow check), OOM handling
   by `std::abort`, racy capacity-growth handshake (works in practice,
   wants a clean acquire/release protocol), deleted copy-assignment on
   `reference` (containers need assignable refs), 24-byte references
   (an index-only 4-byte variant would shrink persistent-map nodes
   substantially at the cost of one table indirection — worth having as a
   choice per field).

## Version reclamation: refcounts as the GC, budgets as the policy

With arena-slab storage and ownership refcounts, "deciding when to GC old
revisions" stops being a *collector* question and becomes a *policy*
question, which is the goal:

- **A version is memory-live exactly while reachable**: pinned as a
  `DocState` root (outputs, exports, autosave), or referenced by a journal
  entry's state handles. Unpin/trim drops a root reference; everything
  unique to that version cascades; everything shared survives via its
  count. No tracing, no pauses, no heuristics — reclamation is exact and
  incremental by construction.
- **The only tunables are already designed**: the journal byte budget
  (doc 14, `state_cost`) decides how much *history* stays alive; cache
  budgets (docs 02/12) decide how much *derived* data stays alive. Memory
  pressure maps to "trim journal tail, shrink caches" — both existing
  knobs, now with per-arena accounting to drive them.
- **Cascades are deferred, never inline.** Dropping the last reference to
  a big subtree (a render thread unpinning after an edit-heavy export)
  must not make that thread destroy thousands of nodes. Release enqueues
  the object on a type-erased **reclamation queue**; a housekeeping pass —
  writer thread between transactions, or a low-priority thread — pops,
  runs destructors (fixing cpioo gap 1), whose child-release enqueues
  continue the cascade *iteratively*: bounded stack, bounded latency,
  amortized cost, and free slots return to the pools warm. Record types
  split in two here: **object records** (composition, layer, content,
  order chunk) are trivially destructible — pure index-and-value bytes,
  which is what lets the workspace file (below) carry them as raw slots —
  while the persistent map's **interior nodes** deliberately are not: a
  map node owns its counted child-edge references and releases them in
  its destructor as the cascade drains (reaching its stores through the
  drainer-published reclaim context). The split costs file backing
  nothing: recovery walks node *bytes* and constructs/destructs no
  object, so the non-trivial destructor is purely a live-editing-time
  concern.
- **Thread rules** (sharpening doc 02/12): the audio *callback* touches no
  allocator, no refcount, ever (it consumes prepared blocks — doc 12
  already guarantees this). Render and audio-engine threads may pin/unpin
  (one refcount op) and enqueue reclamation, nothing more. Workers
  allocate only from pools/arenas warmed for them. The writer thread is
  the only structural allocator — which is what makes thread-local free
  pools effective: churn is concentrated where the free pool lives.
- **The drainer is not the writer, and the checkpointer is.** These are
  two separate consequences of the rule above, and both bite. *Draining*
  may run on the low-priority thread concurrently with a writer
  transaction: releases land in the drainer's own thread-local free pool
  (or, when a durability fence is installed, in the quarantine the
  drainer alone touches), so it never races the writer's allocation. But
  every seam a record's destructor reaches *through* — the runtime's
  content-state routing table above all (doc 14) — is therefore read on
  the drain thread while the writer mutates it, and must be synchronized
  for that. *Committing a checkpoint*, by contrast, is a **writer-thread
  operation**: it reads the allocator's high-water, seals full chunks
  read-only, and compacts the quarantine — all structures the writer
  mutates lock-free. The housekeeping thread drains; it does not commit.
  A commit issued while a transaction is in flight would seal a chunk out
  from under the writer's next placement-new.
- **Document teardown** is arena drop: O(live buffers), no per-object
  walk, no destructor storm on close (bulk-release path runs the
  reclamation queue to quiescence first for types with external
  resources).

**Debug discipline** (contract-test-enforced, doc 10): per-arena live
counts and byte accounting exposed through the API (hosts *will* want a
memory panel); generation tags on slots in debug builds so a stale
reference (use-after-release — possible since slots recycle) faults
loudly instead of silently reading a recycled object; leak check = arena
live count at teardown.

A memory panel polls, and it polls from the host's thread while the
writer allocates — so **the accounting accessors are readable from any
thread**, concurrently with allocation and with the creation of a new
size-class store. This is the one place the "writer is the only
structural allocator" rule (above) meets a genuine cross-thread reader,
and it is bought cheaply: the counters are **relaxed atomics**, and the
arena-level aggregate walk over the store map is guarded against store
creation. They are diagnostics on no correctness path, so the guarantee
is deliberately weak — each counter reads a value it actually held, but
an aggregate is **not a coherent snapshot** across stores, and no
correctness decision may be made from one. The alternative — declaring
the panel writer-thread-only — would make the exposed accounting
unreadable by the host that motivates it.

## File-backed arenas: mmap instead of process memory

A deliberate departure from the PoC: arena buffers are **mmapped from a
per-document workspace file** rather than allocated as anonymous process
memory. Backing is a construction-time arena policy — `anonymous` remains
available (a live-only OBS-style host may want no files) — but file-backed
is the default for document arenas, because it buys four things at once:

1. **Crash recovery.** The workspace file always contains the records of
   every checkpointed version. Recovery is: map the file, read the last
   valid root, resume. An editor crash costs at-most-since-last-checkpoint,
   not the document. A **clean** close is the same story, not a special one:
   reopening the file a document closed normally resumes it at its last
   durable root, with the reachability walk rebuilding the counts exactly as
   after a crash. Teardown must therefore not hole-punch the live chunks it
   is releasing — the file is a cache that survives an ordinary quit, and a
   close that emptied it would make the workspace useless across restarts.
2. **Larger-than-RAM documents.** Residency is the kernel's problem:
   file-backed pages are demand-paged in and — unlike anonymous memory,
   which needs swap — *clean pages evict for free* under pressure. This is
   GIMP's tile-swap-file idea, done at the allocator layer for everything.
3. **Real memory release.** cpioo's anonymous slabs grow to a high-water
   mark forever; file-backed chunks can be hole-punched
   (`fallocate(PUNCH_HOLE)` / `madvise`) when reclamation empties them,
   returning memory *and* disk.
4. **A path to shared mappings.** `MAP_SHARED` + position-independent
   records makes multi-process architectures possible: an out-of-process
   plugin or a crash-isolated render/export process maps the workspace
   *read-only* and renders from a pinned version it cannot corrupt. This
   turns doc 03's deferred plugin isolation from "hard someday" into a
   natural extension — the refcount pages aren't in the shared mapping at
   all, so the isolated process sees truly immutable memory.

**The inside-out split is the persistence split.** What the PoC separates
for cache behavior — immutable data buffers vs mutable refcount buffers —
is exactly the line between file and RAM: data buffers live in the file
mapping; refcounts, free pools, generation tags, and the reclamation queue
are anonymous runtime state, rebuilt on open (a reachability walk from the
live roots reconstructs counts; free slots are the complement). Nothing
about the runtime bookkeeping ever hits disk.

**Position independence becomes a requirement, not an optimization.**
A mapped file has no stable base address, so in-record references are
**index-only** (arena id + slot index); the pointer-carrying reference
variant survives only as a transient handle on stacks, never inside
records. Records were already standard-layout, fixed-size, and
pointer-free by doc 03/15 rules — this closes the loop. The file carries a
header with layout schema version, per-store slot sizes, and arena
directory; workspace files are same-machine artifacts (native endianness
and padding, no portability promise) — **the doc 08 JSON document remains
the interchange and version-control format**; the workspace file is a
session/scratch artifact beside it, like a database's data file vs its
dump.

**The arena directory is what makes reopen unambiguous.** One workspace
file backs *several* slab stores at once — a document's HAMT nodes and its
object records are different size classes, so different stores — and a
store is identified, in memory and on disk alike, by its **(slot stride,
slot alignment)** pair: the same key the arena already uses to find the
store for a type. The header's **store table** carries one entry per
store — stride, alignment, slots per chunk, and the store's **slot
high-water mark as of the checkpoint that published it** — and the arena
directory tags every data chunk with the store that owns it. Reopen
therefore does no guessing: each store re-binds exactly the chunks the
file says are its own, in slot order, and reserves exactly the high-water
the file records. Chunk *byte size* is emphatically not an identity — two
stores with different strides can land on the same chunk size — so routing
is by recorded owner, never inferred from geometry. A file whose store
table disagrees with the reopening build's strides (a debug-vs-release
lane mismatch, say) is refused as a value rather than silently
mis-routed.

**Checkpointing rides the version model.** Because live records are never
overwritten (immutability), consistency needs only ordering, LMDB-style:
msync data chunks, then publish the root by flipping an A/B root slot in
the header, then msync the header. A crash lands on the old or new root,
both consistent. The one interaction to get right is **slot reuse**: a
slot freed *after* the last durable checkpoint may still be referenced by
the on-disk root, so reclamation quarantines freed slots per **durability
epoch** — the deferred-reclamation queue (above) gains a "reusable after
checkpoint N" fence. The store table rides the same A/B discipline: each
root slot owns its own store-table snapshot, written before that root is
flipped, so the high-water a recovery reads is always the one belonging to
the root it selected — a crash mid-commit lands on the old root *and* the
old high-water, never a mismatched pair. Checkpoint cadence is policy
(timer, transaction count, explicit host call); the doc 14 autosave
scenario becomes "msync + root flip" — cheaper than serializing, though
the JSON autosave remains the belt to this suspender. Cadence decides
*when*, never *where*: because the commit is a writer-thread operation
(above), all three triggers are evaluated on the writer — the
transaction-count and host-call triggers naturally, and a timer trigger
by being *delivered* to the writer rather than fired on the housekeeping
thread. Committing off the writer at all requires a writer↔checkpointer
quiesce that the lock-free transaction path does not have today.

Debug hardening gets stronger here too: published data chunks can be
`mprotect`ed read-only between transactions in debug builds, making any
stray write through a stale pointer fault at the write site — the class of
bug that silently corrupts documents under anonymous memory.

## What this asks of doc 14 and the kinds

- `DocState` map nodes and object records get fixed-size slab types —
  a constraint on how the persistent map is written (node arity chosen so
  records land in a small number of size classes), not on its interface.
  Because several distinct record types can deliberately land in the *same*
  size class, the parallel refcount and generation buffers are owned by the
  size-class slab store and indexed by **physical slot**, not by the typed
  view over it: the several typed views that share one size class share a
  single count column (and one generation column) per slot rather than each
  keeping a duplicate table. This is what lets the persistent map mix record
  types within a size class without two typed views disagreeing about a
  shared slot's count — a slot has exactly one logical reference count,
  wherever it is viewed from.
- `StateHandle` is a slab reference; `capture()`'s "O(small)" promise
  (doc 14) is realized as "copy the touched path into same-arena slots".
  Kinds get the slab/pool utilities as public API so their state nodes
  inherit the same properties (and plugin authors don't hand-roll
  allocators); bulk pixel payloads go to the big-block pool with the tile
  *table* in slabs.
- The doc 03 no-STL-in-hot-structs rule extends: record types are
  standard-layout, fixed-size, and their references are slab refs — which
  also happens to be exactly what the future C ABI wants.

## Scheduling and provenance

Foundation, not feature: the arenas, the reference type, deferred
reclamation, and accounting are v1 groundwork — the doc 14 implementation
is built *on* them, not migrated *to* them.

**Decision: the pattern is reimplemented inside arbc core** (`arbc::pool`
/ `arbc::ref` or similar), shaped by the doc 14/15 requirements —
instance arenas, destructors via deferred reclamation, `uint32_t`
refcounts, generation tags, accounting — and exposed to plugin kinds
through arbc's public API. `poc-inside-out-objects` remains the reference
prototype and the source of the measured architectural claims; extracting
the matured implementation into a standalone library is a later option
once its API stops moving, not a v1 coordination cost.
