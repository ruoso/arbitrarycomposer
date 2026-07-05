# cache.keyed_store — Budgeted keyed store

## TaskJuggler entry

`tasks/30-cache.tji:6-10` → `cache.keyed_store` ("Budgeted keyed store"), the
first leaf under `task cache`. The parent carries `depends
surfaces.surface_pool` (`30-cache.tji:4`), so this leaf inherits that edge; the
three siblings chain off it (`key_shapes` `depends !keyed_store`,
`30-cache.tji:14`). Note line:

> "Byte-budgeted store, LRU within priority classes (visible > adjacent >
> recent > speculative); values are backend surfaces/blocks with metadata.
> Doc 02."

## Effort estimate

**3d.** The store machinery (byte accounting, per-class LRU eviction, the
pin/hold lifecycle, the deferred-drop-while-pinned path) is the substantive
part; the generic container shell is small. The concrete key/value shapes,
invalidation index, and prefetch rings are *not* here — they are the three
successor leaves.

## Inherited dependencies

**Settled:**

- `surfaces.surface_pool` (commit `8063b91`, DONE 2026-07-05) — the direct WBS
  predecessor. Its refinement (`tasks/refinements/surfaces/surface_pool.md`)
  **explicitly separates the temp surface pool from this task's tile cache**:
  the temp pool "is not the tile cache and does not share its budget"
  (`surface_pool.md:100-107`, `:234-241`), and it names doc 02's byte-budgeted
  LRU keyed by `(content id, revision, scale rung, tile coords)` as the
  *separate* structure this leaf builds (`surface_pool.md:104-106`). It also
  **parked** a byte-budget for the temp pool as a profiling-dependent judgment
  call (`surface_pool.md:283-305`) — that parked item is the *temp pool's*
  budget, a different structure, and is **not** resolved here. The reusable
  precedents this task carries forward: the move-only RAII handle discipline
  (`PooledSurface`, `surface_pool.md:52-56`), render-thread-confinement as a
  documented design decision rather than a lock (`:165-169`, `:267-272`), and
  errors-/values-as-return (`:253-255`).
- Transitively, `arbc::base` primitives: `arbc::expected`
  (`src/base/arbc/base/expected.hpp`) and `ObjectId` (`src/base/arbc/base/ids.hpp`).
  The store's core machinery needs only `base`.

**Sibling substrate (settled, cited for continuity):**

- `contract.snapshot_pins` (DONE 2026-07-05) established that a tile key's
  revision is honest because state is immutable: "`cache.*` builds tile keys
  that include the pinned revision; the handle is what guarantees the key
  uniquely identifies the pixels" (`snapshot_pins.md:77-99`). This task treats
  the **key as opaque** and does not itself resolve revisions/pins — but it is
  the reason a cached value can never show stale-for-its-key pixels.
- `surfaces.capabilities` (commit `62ff4df`) / `color.format_set` — the
  "one value struct, not loose flags" and errors-as-values idioms
  (`capabilities.md:147-153`, `:224-232`) this task follows.

**Pending:** none — every predecessor is landed. The three consumers of this
seam (`cache.key_shapes`, `cache.invalidation`, `cache.prefetch`) are the
successor leaves and are out of scope here.

## What this task is

Build the **budgeted keyed store** at the heart of `arbc::cache`: a generic
container that holds cached values under opaque keys, tracks resident bytes
against a byte budget, and evicts **LRU within priority classes** when an insert
would exceed the budget. Per doc 17 the cache is **engine-agnostic** — "tiles
and blocks are the same machinery with different key shapes"
(`17:73-74`) — so this leaf delivers the *machinery*, and the tile/block key and
value shapes are the next leaf (`cache.key_shapes`).

Concrete deliverables:

1. **A `KeyedStore<Key, Value>` class template in `arbc::cache`** (new component,
   L3), living at `src/cache/arbc/cache/keyed_store.hpp`. Constructed with a byte
   budget. Core operations:
   - `insert(Key, Value, std::size_t bytes, PriorityClass)` → a **pinned**
     move-only `CacheHold<Value>` on the just-stored entry. The caller supplies
     the value's resident `bytes` explicitly (the store never introspects
     `Value`). Inserting past budget triggers eviction (below) *before* the new
     entry is admitted.
   - `lookup(const Key&)` → `std::optional<CacheHold<Value>>`. A hit refreshes
     the entry's LRU recency **within its class** and returns a pinned hold; a
     miss returns `std::nullopt`. (Hit/miss both bump observable counters.)
   - `reclassify(const Key&, PriorityClass)` — move an entry between classes
     (no-op if absent). The *policy* deciding an entry's class is the caller's
     (`cache.prefetch` / the compositor); the store only honors the tag.
   - `remove(const Key&)` — drop a key. If the entry is pinned, it becomes
     immediately unreachable by lookup but its value is held until the last
     `CacheHold` releases (deferred drop; see Constraints). This is the primitive
     `cache.invalidation` builds its `(content, region)` / revision-orphan index
     over.
   - Observability: `resident_bytes()`, `budget()`, and counter accessors
     (`hits()`, `misses()`, `evictions()`) for behavioral-counter tests.
2. **`PriorityClass`** — the cache's shared eviction-ordered class vocabulary
   (doc 02): `enum class PriorityClass { Speculative, Recent, Adjacent, Visible }`,
   ordered so **Speculative is evicted first and Visible last**. This is the
   authoritative definition of doc 02's "visible > adjacent > recently visible >
   speculative"; both engines share it (doc 17). The doc-11 **temporal prefetch
   ring** is a later extension of this enum owned by `cache.prefetch` — the
   eviction machinery walks the classes in declaration order, so adding a class is
   a localized change.
3. **`CacheHold<Value>`** — a move-only RAII pin handle. Constructing it pins the
   entry (excludes it from eviction candidacy); destroying/moving-out unpins.
   Dereferencing yields the live `Value&`. Moving transfers the unpin obligation
   (no double-unpin).

**Not this task:** the concrete tile key `(content, revision, scale rung, tile
coords, achieved_time)` and audio block key `(content, revision, block index,
rate)` and their value+metadata shapes (`cache.key_shapes`); the `(content,
region)` / revision-bump invalidation index (`cache.invalidation`); spatial pan
and temporal playback prefetch rings and the policy assigning entries to classes
(`cache.prefetch`); the achieved-time-coalescing / stale-fallback / exact-scale-
only-offline *hit-qualification* policy — the store does exact-key lookup and
returns the value; whether a hit *qualifies* is the compositor's read of the
value's metadata (doc 02:82-85, doc 11:108-114); returning evicted surfaces to
`SurfacePool` for recycling (an optimization — see Open questions); thread-safe
concurrent access (v1 is single-threaded-confined — see Decisions/Open questions).

## Why it needs to be done

Doc 02's frame loop is built on the tile cache: "split into tiles … and look each
tile up in the cache" (`02:57-60`); "Cache misses become render requests"
(`02:61`); "Composite. Draw tiles" (`02:66`). The offline path likewise "still
uses the tile cache when content stability allows … rendering 4K video of a
mostly-static scene should not re-rasterize every layer every frame" (`02:82-85`).
Doc 15 makes cache budgets one of the two load-bearing memory knobs: "cache
budgets (docs 02/12) decide how much *derived* data stays alive … memory pressure
maps to 'trim journal tail, shrink caches'" (`15:124-128`), and "budgets are the
eviction policy" (`15:21`). None of that exists yet — `src/cache/` is empty.

This leaf is the foundation the rest of `arbc::cache` and both L4 engines stand
on: `cache.key_shapes`, `cache.invalidation`, and `cache.prefetch` all
`depend !` down to it, and `arbc::compositor` and `arbc::audio-engine` (L4) both
`Depends on: … cache` (`17:56-57`). Landing the store now gives them a stable
insert/lookup/evict/pin contract to build request planning and mix scheduling
over.

## Inputs / context

- `docs/design/02-architecture.md`:
  - `:87-99` — **the governing "Tile cache" section.** Key `(content id,
    revision, scale rung, tile coords)` (`:89`); value = backend surface +
    metadata (`:90-91`); "**Budgeted (bytes), LRU within priority classes:
    visible > adjacent (pan prefetch ring) > recently visible > speculative
    (next/previous zoom rung)**" (`:92-93`) — the exact class ordering this task
    pins; damage/revision invalidation (`:94-95`, delegated to
    `cache.invalidation`).
  - `:100-113` (the residency-pin + soft-budget bullets **added by this task's
    design-doc delta**) — pinned entries survive eviction; budget is a soft
    target the pinned working set may exceed; the residency pin is store-internal,
    distinct from the payload's backend-pool refcount.
  - `:57-71` — how the cache is consulted/filled in the interactive frame; the
    degradation preference order (stale / coarser / transparent, `:61-65`) the
    *value metadata* must support (that metadata is `cache.key_shapes`).
  - `:82-85` — offline correctness: "only exact-scale, current-revision entries
    qualify" (a hit-qualification rule the *consumer* enforces over returned
    metadata, not the store).
- `docs/design/11-time-and-video.md:138-143` — the key gains `achieved_time` for
  `Timed` content (Static keys unchanged); the **temporal prefetch ring** joins
  the priority classes bounded by the playback-hint horizon. Both land in
  `cache.key_shapes` / `cache.prefetch`; cited here so the `PriorityClass` enum is
  designed to extend.
- `docs/design/12-audio.md:169-174` — "The block cache is the tile cache with 1D
  keys — `(content id, revision, block index, rate)`" — the normative basis for
  one engine-agnostic store serving both.
- `docs/design/15-memory-model.md`:
  - `:21` — "Cache values … backend-owned, budgeted, LRU … **budgets are the
    eviction policy**"; cache values live in backend pools, not document arenas.
  - `:124-128` — cache budgets as a named memory knob; pressure → shrink caches.
- `docs/design/17-internal-components.md`:
  - `:54` — `arbc::cache`, **Level 3, Depends on: `base, surface`** — the
    CI-enforced dependency rule.
  - `:40-44` — "A component may depend only on strictly lower levels. **No
    same-level edges.**" — cache (L3) cannot touch `contract`, `backend-cpu`
    (L3), `pool`, `model`, or `media` except transitively through `surface`
    (which pulls `media`, not `pool`).
  - `:73-74` — "**`cache` is engine-agnostic** — tiles and blocks are the same
    machinery with different key shapes."
- `docs/design/16-sdlc-and-quality.md:54-62` — behavioral-counter tests are the
  **primary tier** for a cache: "cache hits/misses" is a named debug counter
  (`:58`); "most claims-register entries about efficiency land here" (`:62`);
  wall-clock gates are rejected (`:54-55`, `:224-225`).
- `src/base/arbc/base/expected.hpp`, `src/base/arbc/base/ids.hpp` — the only
  primitives the store core consumes.
- `src/surface/CMakeLists.txt:1-10` — the `arbc_add_component(NAME …
  PUBLIC_HEADERS … DEPENDS …)` + `arbc_component_test(...)` pattern the new
  `src/cache/CMakeLists.txt` follows.
- `tasks/refinements/surfaces/surface_pool.md` — the RAII-handle, render-thread-
  confinement, and errors-as-values precedents; the explicit temp-pool-≠-tile-
  cache boundary.
- `tests/claims/registry.tsv` — TAB-separated `<claim-id>\t<description>`; this
  task adds entries (below).

## Constraints / requirements

- **Levelization (doc 17:54, :40-44).** `KeyedStore`, `CacheHold`, and
  `PriorityClass` live in the **new `arbc::cache` component at L3**. The store
  *core* needs only `arbc::base` — so `src/cache/CMakeLists.txt` declares
  `DEPENDS base` at this leaf; the `surface` edge the table permits is exercised
  by `cache.key_shapes` when the tile value becomes a `Surface` (declaring a
  subset of the allowed dep set is not a violation). **No dependency on `pool`,
  `model`, `contract`, or `backend-cpu`** — the CI dependency check enforces
  this. The store is generic (`template <class Key, class Value>`) precisely so it
  reaches neither engine.
- **Engine-agnostic, generic over `Key`/`Value` (doc 17:73-74).** The store never
  introspects `Value`: byte cost is supplied explicitly at `insert`, and `Value`
  need only be movable (its destructor releases whatever backend resource it
  owns). `Key` need only be hashable + equality-comparable. Tiles and blocks are
  two instantiations.
- **Byte budget is a soft target (doc 15:21, doc 02 delta).** An insert past
  budget evicts LRU within the lowest-populated class first, climbing classes,
  until it fits **or only pinned entries remain**. The pinned working set is never
  dropped to honor the budget — so `resident_bytes()` may transiently exceed
  `budget()`. Budget accounting is in the supplied `bytes` unit; the store trusts
  the caller's number.
- **Priority-class LRU eviction order (doc 02:92-93).** Eviction victims are
  chosen by scanning classes from `Speculative` upward; within a class the
  least-recently-used *evictable* (unpinned) entry goes first. A higher class is
  never evicted while a lower class holds an evictable entry. Lookup refreshes
  recency; `reclassify` moves an entry's class.
- **Residency pin, store-internal (doc 02 delta, reconciling doc 15 ↔ doc 17).**
  `lookup`/`insert` return a move-only `CacheHold`; a pinned entry is excluded
  from eviction candidacy. `remove` of a pinned key makes it immediately
  unreachable by lookup but **defers the value drop to the last `CacheHold`
  release** — an in-flight composite/mix never has its value freed underneath it.
  This pin is *not* `arbc::ref`/`SlotRef` (that lives in `pool`, a forbidden edge
  at L3); the store owns its entries and manages its own per-entry pin count.
- **Errors/absence as values (doc 10 idiom).** `lookup` returns
  `std::optional` (miss is not an error); the store never nulls-then-aborts and
  never throws through the frame loop (doc 02 "the composition must not" fail).
- **Render-/mix-thread-confined, single-threaded at v1 (doc 02:118-120,
  surface_pool precedent).** The store is **not** thread-safe and adds no lock:
  per doc 02 "v1 may degenerate to 'everything on one thread'." Concurrent
  lookup-from-readers + insert-from-workers is a designed *future* mode; hardening
  for it is deferred (Decisions / Open questions). Documented as an invariant, not
  an omission.
- **Observable counters, no wall-clock.** Hit/miss/eviction counts are exposed as
  plain accessors (there is no central `base` debug-counter registry yet — base
  has only `expected`/`geometry`/`ids`/`time`/`transform`). Behavioral tests read
  these accessors; **never** a timing assertion (doc 16:54-55).
- **CI diff coverage ≥90%** on changed lines (doc 16:114-118).

## Acceptance criteria

- **Unit tests — `src/cache/t/keyed_store.t.cpp` (new, L3, Catch2).** Use a
  trivial in-test `Value` stub — a move-only struct carrying an id and a
  shared "released" counter its destructor bumps — with an explicit byte cost
  passed to `insert`. **No `Surface`/`CpuBackend`** (keeps the test at the store's
  true `base`-only dependency surface). Assertions:
  - insert then `lookup` same key → hit (counter++), returns the value;
    `lookup` of an absent key → `std::nullopt` (miss counter++).
  - insert past budget → eviction from `Speculative` first; a `Visible` entry is
    **not** evicted while an evictable `Speculative`/`Recent`/`Adjacent` entry
    remains; `resident_bytes()` returns within `budget()` afterward.
  - LRU *within* a class: of two same-class entries, the least-recently-`lookup`ed
    is evicted first; a `lookup` refreshes recency and flips the victim.
  - `reclassify` moves an entry to a lower class → it becomes the next victim; to
    a higher class → it is spared.
  - **pinned entry never evicted**: holding a `CacheHold` past budget keeps the
    entry resident and lets `resident_bytes()` exceed `budget()`; dropping the
    hold makes it evictable and the next insert reclaims it.
  - `remove` of a pinned key: immediately a lookup miss, but the value's
    "released" counter does **not** fire until the outstanding `CacheHold` drops
    (deferred drop).
  - `CacheHold` move semantics: moving transfers the unpin obligation — no
    double-unpin, no premature eviction, exactly-once release (assert via the
    stub's released-counter).
- **Behavioral-counter test — same target.** A scripted insert/lookup/evict
  sequence asserting exact `hits()`/`misses()`/`evictions()` deltas (doc 16:54-62
  — the primary cache tier). Never wall-clock.
- **Claims (register + `enforces:` tag)** in `tests/claims/registry.tsv`,
  enforced from the unit tests:
  - `02-architecture#cache-evicts-lru-within-priority-class` — "Eviction removes
    the least-recently-used *unpinned* entry from the lowest-populated priority
    class first; a higher-class entry is never evicted while a lower-class
    evictable entry remains."
  - `02-architecture#cache-pin-survives-eviction` — "A pinned entry is never
    evicted; the pin keeps it resident past the byte budget, and a removed-while-
    pinned entry's value is held until its last hold releases."
  - `15-memory-model#cache-budget-is-eviction-policy` — "Inserting past the byte
    budget evicts (LRU within priority class) until resident bytes fit or only
    pinned entries remain; the budget bounds resident bytes except for the pinned
    working set."
- **Gate green (build + tiers 1-5 in Debug+ASan/UBSan).** **No TSan obligation**
  at this leaf — the store is single-threaded-confined by design (no shared
  mutable access to race). If a future task drives concurrent fills, thread-safety
  and its TSan/schedule-perturbation stress become **that** task's obligation
  (see Open questions).
- **No golden tests here.** Pixels/samples are produced downstream; hit-vs-refill
  byte-exactness is a `cache.key_shapes` / compositor golden. Noted so its absence
  is a scoped decision, not a gap.
- **Component wiring:** `src/cache/CMakeLists.txt` (`arbc_add_component NAME cache
  … DEPENDS base`) added and registered in `src/CMakeLists.txt`; the public header
  compiles standalone under `VERIFY_INTERFACE_HEADER_SETS`.

## Decisions

- **A `KeyedStore<Key, Value>` class template, not a type-erased container.** Doc
  17:73-74 frames tiles and blocks as "the same machinery with different key
  shapes" — two template instantiations express that exactly, at zero runtime
  overhead, keeping the store's dependency surface at `base` alone. *Rejected:*
  type erasure (`std::any` value + a `std::function` byte-cost) — indirection and
  a heap-erased value for no benefit at two call sites, and it would obscure the
  RAII release the store relies on. *Rejected:* folding the store into
  `SurfacePool` — the sibling refinement already settled that the temp pool and
  the tile cache are different structures with different keys, lifetimes, and
  budgets (`surface_pool.md:100-107`, `:234-241`).
- **Caller supplies the byte cost at `insert`; the store never introspects
  `Value`.** Maximally decouples the store from value shape (a `Surface`'s cost is
  `w*h*bytes_per_pixel`, a block's is `samples*channels*sizeof(sample)` — both
  known to the *caller* in `cache.key_shapes`, not to a generic store). *Rejected:*
  a `Value::byte_cost()` concept method — couples the store to a value-shape
  contract for a number the caller already has.
- **`PriorityClass` is a fixed enum owned by the cache store, extended by
  `cache.prefetch`.** Doc 17:54 lists "priority classes" as cache-component
  contents, so the store is the authoritative home for doc 02's ordering. Defining
  the four doc-02 classes now and letting `cache.prefetch` add doc-11's temporal
  ring (the eviction loop walks classes in declaration order, so extension is
  localized) keeps the *mechanism* here and the class-*assignment policy* with the
  prefetch/compositor consumers. *Rejected:* an opaque integer priority with the
  named enum living in the consumer — it would scatter doc 02's normative ordering
  out of the component doc 17 says owns it, and lose the type-safety of a named
  class at the fill sites. *Rejected:* baking the temporal ring in now — that is
  doc 11 / `cache.prefetch` territory and would presuppose its horizon-bounding
  policy.
- **Residency pin is store-internal (`CacheHold`), not `arbc::ref`.** The cache
  is L3 and `pool` (home of `arbc::ref`/`SlotRef`) is a forbidden edge (doc
  17:40-44, :54). So the store owns its entries and manages its own per-entry pin
  count, exposed as a move-only RAII `CacheHold` — mirroring `PooledSurface`'s
  discipline (`surface_pool.md:52-56`). This *reconciles* doc 15's "cache values
  are refcounted from the owning nodes" (`15:21`) with the levelization: the cache
  holds its values for as long as they are cached (that *is* the count doc 15
  means) and releases on eviction; the backend-pool refcount on the value's pixel/
  sample payload is a lower, transitively-owned concern inside `Value`. *Rejected:*
  reaching into `arbc::pool` for `SlotRef` (illegal L3→L1 same-as-pool edge, and
  it would leak pool vocabulary into the engine-agnostic store).
- **Budget is soft w.r.t. the pinned working set.** You cannot evict a tile the
  current frame is mid-composite on, so when the pinned set alone exceeds the
  budget, the store overshoots rather than corrupting the frame — correctness
  outranks the budget (doc 15's "budgets are the *policy*", not a hard cap).
  *Rejected:* a hard cap that fails/blocks `insert` at budget — it would either
  drop an in-use tile (wrong pixels) or stall the render thread. Recorded in the
  doc-02 delta.
- **`remove` of a pinned key defers the value drop to last-unpin.** Invalidation
  (revision bump / damage) must make an old key un-hittable *immediately* so no new
  frame reads it, but an in-flight composite holding the value must finish safely.
  Immediate unreachability + deferred drop gives both, echoing doc 15's "release
  enqueues, never destroys inline" spirit (`reclamation.md:56-61`) without a `pool`
  edge (the drop runs inline on last unpin on the confining thread, which is
  correct for a single-threaded v1). *Rejected:* refusing to remove a pinned key
  (invalidation could not orphan an in-use old-revision tile); *rejected:* dropping
  it out from under the holder (use-after-free).
- **Single-threaded-confined at v1; concurrency deferred (surface_pool
  precedent).** Doc 02:118-120 allows "everything on one thread" for v1, and
  `surface_pool` set the precedent of declaring thread-confinement a design
  decision with no lock and no TSan obligation, deferring safety to whenever
  allocation moves off-thread (`surface_pool.md:217-222`, `:267-272`). The same
  applies: no L4 engine drives concurrent cache fills yet. *Rejected:* a
  mutex/sharded store "to be safe" now — it pays synchronization cost on the hot
  lookup path guarding against a caller the v1 model forbids, and the eventual
  concurrent design (reader-safe lookup vs. worker-fill) wants to be co-designed
  with the async fill path, not guessed at now.
- **Design-doc delta to doc 02 (rides the closer's commit).** Doc 02's Tile cache
  section named "Budgeted (bytes), LRU within priority classes" but pinned no
  seam for two questions this task must answer: (a) what protects an in-use entry
  from eviction, and (b) is the budget hard or soft. The delta adds two bullets:
  a store-internal **residency pin** (distinct from the payload's backend-pool
  refcount, reconciling doc 15 ↔ doc 17), and the **soft-budget** rule (the pinned
  working set may exceed budget). This concretizes an ambiguous seam rather than
  shaping the project, so it takes **no doc 00 decision-record bullet** (the
  surface_pool precedent for a seam-concretizing delta).
- **No new WBS task.** This leaf's follow-on work is already scoped as its three
  named siblings — `cache.key_shapes` (key/value shapes + the `surface` edge + the
  temporal-ring enum extension), `cache.invalidation` (the `(content, region)` /
  revision-orphan index over `remove`), and `cache.prefetch` (rings + class-
  assignment policy). Two items that are *not* new leaves — concurrency hardening
  (trigger is the async off-thread fill model, a runtime-architecture decision, not
  yet made) and recycling evicted surfaces back into `SurfacePool` (a profiling-
  dependent optimization coupling two lifecycles) — are surfaced for the
  **parking lot**, matching how `surface_pool` parked its analogous budget
  question. Milestone: this leaf feeds **M3 (Interactive still compositor)**
  transitively via `arbc::compositor`'s `cache` dependency; no milestone edit is
  needed (the compositor tasks already carry it).

## Open questions

- **Concurrency hardening.** When the async render/mix model (doc 02:101-117)
  actually drives cache **fills from worker threads** while the render thread does
  **lookups**, `KeyedStore` needs reader-safe lookup and synchronized insert/evict
  (plus TSan + schedule-perturbation stress, doc 16:64-73). Whether v1 stays
  single-threaded is a runtime-architecture decision not yet made (doc 02:118-120
  explicitly leaves it open), so hardening now would guess at the concurrent design
  and pay for it on the hot path meanwhile. **Resolution:** ship the single-
  threaded-confined store (documented invariant, no lock); surface "harden
  `KeyedStore` for concurrent lookup + worker-fill, with TSan/stress, when the
  async off-thread fill path lands" as a **parking-lot** item for the closer — a
  design-gated follow-up co-designed with that path, not a WBS leaf that could be
  picked up before the path exists.
- **Recycling evicted backend surfaces into `SurfacePool`.** On eviction the store
  drops the `Value`, freeing its backend surface; that surface *could* instead be
  returned to `surfaces::SurfacePool` for reuse. Whether that pays is profiling-
  dependent and it couples the cache's eviction path to the pool's lifecycle.
  **Resolution:** free on eviction at v1; surface the recycling optimization to the
  **parking lot** (the same shape as `surface_pool`'s own parked budget question).

## Status

**Done** — 2026-07-05.

- Created `src/cache/arbc/cache/keyed_store.hpp` — `PriorityClass` enum, `CacheHold<Value>` move-only RAII pin handle, and `KeyedStore<Key,Value>` class template with insert/lookup/reclassify/remove and observable counters.
- Created `src/cache/arbc/cache/keyed_store.cpp` — out-of-line authoritative eviction order; also the OBJECT-lib translation unit for the new component.
- Created `src/cache/CMakeLists.txt` — `arbc_add_component(NAME cache … DEPENDS base)` + `arbc_component_test(...)` wiring.
- Created `src/cache/t/keyed_store.t.cpp` — 11-case / 52-assertion Catch2 unit tests covering insert/lookup/miss, Speculative-first + cross-class eviction, in-class LRU + lookup-refresh, reclassify demote/promote, pinned-survives-eviction + budget overshoot + reclaim, deferred drop on remove-while-pinned, `CacheHold` move semantics, and one behavioral-counter test asserting exact hits/misses/evictions.
- Edited `src/CMakeLists.txt` — registered `add_subdirectory(cache)` after `surface`.
- Edited `tests/claims/registry.tsv` — appended 3 claims: `02-architecture#cache-evicts-lru-within-priority-class`, `02-architecture#cache-pin-survives-eviction`, `15-memory-model#cache-budget-is-eviction-policy` (all enforced).
- Edited `docs/design/02-architecture.md` — added residency-pin and soft-budget bullets to the Tile cache section (the design-doc delta called for in Decisions).
- No golden tests (scoped out per refinement). No tech-debt WBS tasks; concurrency hardening and evicted-surface recycling routed to parking lot.
