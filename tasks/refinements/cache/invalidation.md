# cache.invalidation — Damage/revision invalidation

## TaskJuggler entry

`tasks/30-cache.tji:19-24` — `task invalidation "Damage/revision
invalidation"` under `task cache`. Scope note:

> Invalidate by (content, region) across rungs; revision bumps orphan old
> keys; aggregate revisions for composite content. Docs 02/05.

Back-link to append on completion (closer):
`Refinement: tasks/refinements/cache/invalidation.md`.

## Effort estimate

1d (`tasks/30-cache.tji:20`). This lands a thin, key-shape-aware
invalidation layer plus one generic predicate-remove on the existing store;
no new component, no new dependency, no concurrency surface.

## Inherited dependencies

**Settled (predecessors in this stream):**

- `cache.keyed_store` (`tasks/refinements/cache/keyed_store.md`, Done
  2026-07-05) — `KeyedStore<Key, Value>` at
  `src/cache/arbc/cache/keyed_store.hpp:139`. Provides the primitive this
  task builds on: `remove(const Key&)` (`:172`), explicitly called out at
  `:171` as "the primitive `cache.invalidation` builds its orphan index
  over". Removal of a **pinned** key defers the value drop to last unpin —
  `detach()` (`:200-210`) marks the entry `removed` (`detail::CacheEntry`,
  `:41-48`) and moves it into `d_orphans` (`:253`); `unpin()` (`:184-195`)
  frees it on last unpin. `CacheHold<Value>` (`:71-119`) is the move-only
  residency pin returned by `insert`/`lookup`. Observability counters
  `resident_bytes()/hits()/misses()/evictions()` (`:174-178`).
- `cache.key_shapes` (`tasks/refinements/cache/key_shapes.md`, Done
  2026-07-05) — `TileKey` (`src/cache/arbc/cache/key_shapes.hpp:64-76`),
  `BlockKey` (`:83-90`), `ScaleRung` (`:39-43`), `TileCoord` (`:48-53`),
  `TileValue` (`:110-113`), `using TileCache = KeyedStore<TileKey,
  TileValue>` (`:129`). **Decided there and inherited here:** `revision` is
  a bare `std::uint64_t` opaque token — the cache takes no `model`/`contract`
  edge; the compositor (L4) projects `DocRoot::revision()` (or an aggregate
  revision) into the key with a plain copy (`:23-31`). `achieved_time` is
  `std::optional<Time>`, absent for Static content.

**Pending / parked (not blocking):** off-render-thread concurrent fill +
reader lookup is parked (keyed_store Open questions); this task stays
single-threaded and render-thread-confined, no TSan obligation.

## What this task is

A small invalidation layer over the landed tile cache that turns the two
invalidation triggers doc 02 names into concrete cache operations:

1. **Damage by `(content, region)` across all resident rungs** — drop every
   tile of a given content whose footprint intersects a content-space
   rectangle, at *every* scale rung it is cached at (doc 02:94-95).
2. **Revision-bump orphaning** — already lazy by key construction (a bumped
   revision produces fresh keys; old keys are simply never looked up again
   and serve as stale fallback until LRU reclaims them). This task adds the
   *opt-in eager reclaim* (`drop_superseded`) the compositor can call to
   reclaim bytes when a content's stale tiles are no longer needed as
   fallback, plus the wholesale `invalidate_content` sweep.

Composite content ("aggregate revisions") needs **no cache-specific
mechanism**: an aggregate revision is just a `std::uint64_t` in the same
`TileKey::revision` slot, so a composed-result tile is invalidated by the
exact same machinery as a leaf tile (this is verified, not merely asserted).

Delivered as a header-only, key-shape-aware module
`src/cache/arbc/cache/invalidation.hpp` plus one generic addition —
`KeyedStore::remove_if(Pred) -> std::size_t` — to the store (same
component). No `.cpp` (the free functions wrap `remove_if`; `remove_if` is a
template).

## Why it needs to be done

The tile cache is inert without invalidation: once a leaf is edited or a
sub-region is damaged, stale tiles must stop being served, or the compositor
renders wrong pixels. Doc 02's frame loop depends on it — misses become
render requests (`docs/design/02-architecture.md:62-65`) and async results
"produce damage for their region, scheduling a follow-up frame" (`:69-71`).
Doc 05's recursive-composition affordance ("a 10-level tree re-renders only
the spine from the edited layer to the viewed root",
`docs/design/05-recursive-composition.md:78-86`) is exactly a statement
about which cached tiles survive an edit — it is unrealizable until
invalidation is precise.

**Downstream consumers:** the L4 compositor (doc 17:56 owns "aggregate
revisions" and "damage routing over `inputs()`") calls these operations
during its damage-collection pass, supplying the tile→content-space geometry
from its scale ladder. `cache.prefetch` (`tasks/30-cache.tji:25-30`) shares
the same store and is unaffected. The generic `remove_if` seam also serves
the future audio `BlockCache` (below).

## Inputs / context

**Governing design-doc sections (normative — doc 16):**

- `docs/design/02-architecture.md:87-116` — the Tile cache section. Key
  sentence, `:94-95`: *"Damage invalidates by `(content id, region)` across
  all rungs; revision bumps invalidate wholesale by making old keys
  unreachable."* Two distinct mechanisms in one line. Also `:100-109`
  (residency pin; *"removing a pinned key (invalidation) defers the drop to
  its last unpin"*), `:110-116` (soft budget), `:62-65` (fallback prefers
  "stale-revision tiles, coarser-scale tiles rescaled, or
  checkerboard/transparent, in that preference order"), `:69-71` (async
  results produce regional damage), `:84-85` (offline: "only exact-scale,
  current-revision entries qualify" — stale entries persist, filtered at
  lookup).
- `docs/design/05-recursive-composition.md:78-86` — composed result "cached
  by the parent like any other content's tiles, keyed by the child's
  *aggregate revision* (a composition-level revision bumped by any reachable
  change)". `:88-91` — the aggregate revision drives damage propagation:
  "child damage maps through each embedding's transform into parent-space
  damage". `:42-45` — shared inner tiles (keyed by child content id) vs
  each embedding's own composed-result cache. `:71-74` — within a frame,
  every visit sees the same revisions (snapshot consistency).
- `docs/design/17-internal-components.md:54` — `arbc::cache` is **Level 3**,
  allowed deps **`base, surface` only**. `:41-42` — "may depend only on
  strictly lower levels. No same-level edges" (so **no `contract`, no
  `model`**). `:56` — the L4 compositor owns "aggregate revisions … and
  damage routing over `inputs()`". `:73-76` — cache is engine-agnostic;
  tiles and blocks are the same machinery.
- `docs/design/16-sdlc-and-quality.md:54-62` — behavioral-counter tier
  (this task's primary tier); `:114-118` — claims register; CI gates ≥90%
  diff coverage.

**Existing seams this task extends:**

- `src/cache/arbc/cache/keyed_store.hpp` — `KeyedStore` (`:139`), `remove`
  (`:172`), `lookup` (`:162`), `insert` (`:156`), `CacheHold` (`:71-119`),
  `detach()`/`unpin()`/`d_orphans` (`:200-210`/`:184-195`/`:253`), counters
  (`:174-178`). `remove_if` is added here.
- `src/cache/arbc/cache/key_shapes.hpp` — `TileKey` (`:64-76`: `.content`
  `ObjectId`, `.revision` `std::uint64_t`, `.rung` `ScaleRung`, `.coord`
  `TileCoord`, `.achieved_time` `std::optional<Time>`), `TileCache` (`:129`).
- `src/base/arbc/base/geometry.hpp:14-32` — `Rect { double x0,y0,x1,y1; }`,
  half-open axis-aligned, with `intersect()`, `empty()`, `width()/height()`.
  The content-space rectangle type damage is expressed in.
- `src/base/arbc/base/ids.hpp:11-25` — `ObjectId` (content identity; ships
  `std::hash` + `operator==`).
- `src/base/arbc/base/time.hpp:10-19` — `Time` (flicks).
- **Compositor-side drivers (not dependencies — they call *into* the
  cache):** `src/contract/arbc/contract/content.hpp:228`
  (`map_input_damage(input, Rect)`; over-approximation sound,
  under-approximation drops repaint) and `src/model/arbc/model/model.hpp:44`
  (`DocRoot::revision()`), bumped `+1` per publish at
  `src/model/model.cpp:985-987`. These live above L3; the compositor reads
  them and drives the cache operations below.
- Build/CI: `src/cache/CMakeLists.txt` (add header to `PUBLIC_HEADERS`, add
  `t/invalidation.t.cpp` to the component test), `tests/claims/registry.tsv`
  (append), `scripts/check_levels.py` (`"cache": {"base", "surface"}`) and
  `scripts/check_claims.py` (every claim needs ≥1 `// enforces:` test).

## Constraints / requirements

- **Levelization (CI-enforced, doc 17:41-42, 54).** `invalidation.hpp` may
  `#include` only from `arbc/base/`, `arbc/surface/`, `arbc/media/`
  (transitive via surface), and `arbc/cache/` itself. It must **not** touch
  `contract` (same-level L3) or `model` (L2, not in cache's allowed set).
  The scale-ladder / tile geometry is therefore **injected by the caller**,
  never embedded in the cache.
- **`(content, region)` across all rungs (doc 02:95).** Region damage must
  hit the content's tiles at *every* resident scale rung, not just the
  viewport rung. This requires enumerating the content's live keys, which
  the generic `KeyedStore::remove_if(Pred)` provides.
- **Pin-defer preserved (doc 02:100-104).** Invalidating a pinned tile must
  make its key immediately unreachable to new lookups while deferring the
  value drop to last unpin — i.e., `remove_if` must route each matched key
  through the same `detach()` path as `remove`, never a bare-erase that
  could pull the surface out from under an in-flight composite.
- **Revision-bump stays lazy (doc 02:62-65, 84-85).** A revision bump must
  **not** eagerly evict prior-revision tiles: they are the "stale-revision
  tiles" fallback and remain lookup-able by their old key until LRU reclaims
  them (or `drop_superseded` is explicitly called). No op fires on the bump
  hot path.
- **Store stays generic.** `remove_if` is key/value-agnostic (takes a
  `Pred(const Key&)`); it must not learn that keys have a `.content` field.
  All key-shape knowledge lives in `invalidation.hpp`.
- **Soft-budget / counter invariants unchanged.** `resident_bytes()` counts
  orphaned-but-pinned entries until unpin; `evictions()` counts only budget
  evictions, not invalidations (invalidations are removals, distinct).
- **Single-threaded / render-thread-confined.** No lock, no TSan obligation
  (inherited).

## Proposed surface (illustrative, not binding)

```cpp
// keyed_store.hpp — generic addition. Routes each match through detach(),
// so pinned matches are orphaned (unreachable now, dropped at last unpin).
template <class Pred>
std::size_t KeyedStore<Key, Value>::remove_if(Pred&& pred);   // returns count

// invalidation.hpp — key-shape-aware, header-only, deps base+surface+cache.
namespace arbc::cache {

// (content, region) damage across ALL resident rungs (doc 02:95).
// `tile_rect` maps a tile's (rung, coord) to its content-space Rect; the
// caller (compositor, L4) supplies it from its scale ladder — keeps the
// ladder out of L3. Predicate is revision- and achieved_time-agnostic:
// spatial damage supersedes the region at every revision/time (sound
// over-approximation, content.hpp:228).
template <class Geom>   // Geom: Rect(ScaleRung, TileCoord)
std::size_t invalidate_region(TileCache&, ObjectId content,
                              const Rect& region, Geom&& tile_rect);

// Wholesale: drop every resident tile of a content (all revisions, rungs,
// times). For content deletion / coarse structural damage.
std::size_t invalidate_content(TileCache&, ObjectId content);

// Opt-in eager reclaim of superseded revisions (revision < live_revision).
// NOT fired on the bump hot path — stale tiles stay valid fallback until
// the compositor chooses to reclaim.
std::size_t drop_superseded(TileCache&, ObjectId content,
                            std::uint64_t live_revision);
}
```

## Acceptance criteria

New Catch2 unit test `src/cache/t/invalidation.t.cpp` (behavioral-counter
tier, doc 16:54-62), added to `arbc_component_test(COMPONENT cache …)` in
`src/cache/CMakeLists.txt`; `invalidation.hpp` added to `PUBLIC_HEADERS`.
≥90% diff coverage on all changed lines (CI gate). No golden tests (no
deterministic-render surface here).

**Claims-register entries** (append to `tests/claims/registry.tsv`, each
with a `// enforces:` tagged case — `check_claims.py` requires the pairing
in the same change):

1. `02-architecture#damage-invalidates-by-content-region-across-rungs` —
   insert content `C` tiles at rungs {0,1,2}, some coords inside a region,
   some outside, plus a content `D` tile inside the region;
   `invalidate_region(C, region, geom)` misses every intersecting `C` key at
   *every* rung, spares non-intersecting `C` tiles and all of `D`; returned
   count == tiles removed. A companion assertion pins revision-/time-
   agnosticism (same coord, differing `revision` and `achieved_time`, all in
   region → all removed).
2. `02-architecture#revision-bump-preserves-stale-tiles-as-fallback` —
   after the compositor advances to revision `N+1`, `lookup(K_{N+1})` misses
   while `lookup(K_N)` still hits (stale fallback resident, `evictions()`
   unchanged); a subsequent `drop_superseded(C, N+1)` removes `K_N` (count
   == 1) and it then misses.
3. `02-architecture#invalidation-defers-drop-of-pinned-key` — hold a tile
   pinned via `lookup`; `invalidate_region` hitting it makes the key miss
   immediately while `resident_bytes()` is unchanged (orphaned); destroying
   the hold drops the bytes.
4. `05-recursive-composition#composed-result-invalidated-like-leaf` — a
   composed-result content id with its `revision` slot carrying an aggregate
   value is invalidated identically by `invalidate_content` /
   `drop_superseded` (a child bump simulated as a higher aggregate),
   demonstrating no cache-specific composite mechanism is needed.

**Generic seam:** a `remove_if` case at the store level (in
`src/cache/t/keyed_store.t.cpp` or the new file) verifying pinned matches are
orphaned (deferred drop), unpinned matches freed inline, and the count is
correct.

**Deferred (no WBS leaf created — cross-stream, no new cache work):**
audio `BlockKey` invalidation (by content / revision, and time-range block
spans) is scoped to the audio-engine stream when `tasks/45-audio.tji` lands
the `BlockCache`; it reuses the same generic `KeyedStore::remove_if` seam
this task adds, so nothing further is owed by the cache stream. The
compositor-side aggregate-revision computation and transform-based damage
mapping are owned by the L4 compositor (doc 17:56) and belong to the
compositor stream, not here.

## Decisions

- **Injected tile geometry, not an embedded scale ladder.** `invalidate_region`
  takes a caller-supplied `Geom: Rect(ScaleRung, TileCoord)`. *Rationale:*
  the scale ladder is compositor-owned (doc 17:56; base geometry supplies no
  integer grid, `key_shapes.hpp:37-38`). Injecting it keeps the cache at L3
  (base+surface only) and out of contract/model. *Rejected:* storing each
  tile's content-space `Rect` as key/value metadata (bloats the value,
  duplicates ladder state, and would need an `insert` API change to the
  generic store); having the compositor pre-compute the damaged tile set and
  call `remove` per key (it cannot — it doesn't know which rungs hold
  resident tiles for a content; only the cache does, which is the whole
  point of "across all rungs").
- **Generic `remove_if` over a side content-index.** Enumeration is a
  `KeyedStore::remove_if(Pred)` scan. *Rationale:* keeps the store
  key-agnostic, is minimal (fits 1d), and reuses the `detach()` pin-defer
  path for free. *Rejected:* a `content → live-keys` secondary index — an
  O(1)-lookup optimization with real bookkeeping cost and no design-doc
  performance promise to satisfy; at v1 cache sizes the O(n) scan is
  acceptable. (If a future benchmark shows the scan dominates, the index is
  a self-contained optimization — surfaced to the parking lot, not
  pre-registered as speculative WBS bloat.)
- **Region damage is revision- and time-agnostic (over-approximate).**
  `invalidate_region` matches on `content` + footprint∩region only, dropping
  matching tiles at every revision and `achieved_time`. *Rationale:* spatial
  damage to a content supersedes that region regardless of the edit/playback
  axes; over-invalidation is sound (`content.hpp:228`:
  under-approximation "drops repaint"), so the conservative choice is
  correct. *Rejected:* time-scoping region damage to a single
  `achieved_time` (would leave stale tiles at other frames of a Timed
  content).
- **Revision-bump invalidation is lazy; eager reclaim is opt-in.** No op
  fires on a bump — old keys are already unreachable by construction and
  serve as fallback (doc 02:62-65). `drop_superseded` is a separate,
  compositor-driven reclaim. *Rationale:* preserves the doc's stale-revision
  fallback tier; eager deletion on every bump would starve the fallback and
  churn re-renders. *Rejected:* eager delete-on-bump.
- **Aggregate revisions need no cache mechanism.** A composed-result tile is
  keyed by `(child_content_id, aggregate_revision, …)`; the aggregate is a
  plain `std::uint64_t` the compositor projects into the same slot, so all
  three operations apply unchanged. *Rationale:* doc 05:78-86 defines
  aggregate revision as "a composition-level revision" — same type, same
  slot; computing/propagating it is compositor work (doc 17:56).
- **Header-only, no new `.cpp`, no design-doc delta.** The free functions
  wrap `remove_if`; `remove_if` is a template. Every behavior is already
  settled doc text (docs 02/05/17), so — like `cache.key_shapes` — no
  design-doc amendment is required.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-05.

- `src/cache/arbc/cache/invalidation.hpp` (new) — header-only `arbc::cache` surface: `invalidate_region<Geom>`, `invalidate_content`, `drop_superseded`; geometry injected by caller to preserve L3 levelization.
- `src/cache/arbc/cache/keyed_store.hpp` — added `remove_if(Pred) -> std::size_t`; each match routed through `detach()` preserving pin-defer semantics.
- `src/cache/t/invalidation.t.cpp` (new) — 5 Catch2 unit cases covering damage-by-region, content sweep, drop_superseded, pinned-key deferral, and composite aggregate revision.
- `src/cache/t/keyed_store.t.cpp` — added `remove_if` seam case: pinned match orphaned (deferred drop), unpinned match freed inline, count correct.
- `src/cache/CMakeLists.txt` — `invalidation.hpp` added to `PUBLIC_HEADERS`; `t/invalidation.t.cpp` added to component test.
- `tests/claims/registry.tsv` — 4 claims registered: `02-architecture#damage-invalidates-by-content-region-across-rungs`, `#revision-bump-preserves-stale-tiles-as-fallback`, `#invalidation-defers-drop-of-pinned-key`, `05-recursive-composition#composed-result-invalidated-like-leaf`.
