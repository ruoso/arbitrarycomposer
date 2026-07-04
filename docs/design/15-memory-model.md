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
  ~2.4–3× version-production rate, ~16–18× traversal rate.

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
   interleaving) are real, but a fair baseline would shrink the 16× —
   re-benchmark honestly before quoting numbers in arbc docs.
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
  amortized cost, and free slots return to the pools warm.
- **Thread rules** (sharpening doc 02/12): the audio *callback* touches no
  allocator, no refcount, ever (it consumes prepared blocks — doc 12
  already guarantees this). Render and audio-engine threads may pin/unpin
  (one refcount op) and enqueue reclamation, nothing more. Workers
  allocate only from pools/arenas warmed for them. The writer thread is
  the only structural allocator — which is what makes thread-local free
  pools effective: churn is concentrated where the free pool lives.
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

## File-backed arenas: mmap instead of process memory

A deliberate departure from the PoC: arena buffers are **mmapped from a
per-document workspace file** rather than allocated as anonymous process
memory. Backing is a construction-time arena policy — `anonymous` remains
available (a live-only OBS-style host may want no files) — but file-backed
is the default for document arenas, because it buys four things at once:

1. **Crash recovery.** The workspace file always contains the records of
   every checkpointed version. Recovery is: map the file, read the last
   valid root, resume. An editor crash costs at-most-since-last-checkpoint,
   not the document.
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
header with layout schema version, per-type slot sizes, and arena
directory; workspace files are same-machine artifacts (native endianness
and padding, no portability promise) — **the doc 08 JSON document remains
the interchange and version-control format**; the workspace file is a
session/scratch artifact beside it, like a database's data file vs its
dump.

**Checkpointing rides the version model.** Because live records are never
overwritten (immutability), consistency needs only ordering, LMDB-style:
msync data chunks, then publish the root by flipping an A/B root slot in
the header, then msync the header. A crash lands on the old or new root,
both consistent. The one interaction to get right is **slot reuse**: a
slot freed *after* the last durable checkpoint may still be referenced by
the on-disk root, so reclamation quarantines freed slots per **durability
epoch** — the deferred-reclamation queue (above) gains a "reusable after
checkpoint N" fence. Checkpoint cadence is policy (timer, transaction
count, explicit host call); the doc 14 autosave scenario becomes "msync +
root flip" — cheaper than serializing, though the JSON autosave remains
the belt to this suspender.

Debug hardening gets stronger here too: published data chunks can be
`mprotect`ed read-only between transactions in debug builds, making any
stray write through a stale pointer fault at the write site — the class of
bug that silently corrupts documents under anonymous memory.

## What this asks of doc 14 and the kinds

- `DocState` map nodes and object records get fixed-size slab types —
  a constraint on how the persistent map is written (node arity chosen so
  records land in a small number of size classes), not on its interface.
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
