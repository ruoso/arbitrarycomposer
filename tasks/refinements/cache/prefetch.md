# cache.prefetch — Prefetch rings

## TaskJuggler entry

Back-link: `tasks/30-cache.tji:26-31` — `task prefetch "Prefetch rings"`.

> "Spatial pan-prefetch ring + temporal playback-direction ring bounded by
> playback-hint horizon; priority-class integration. Docs 02/11."
> (`tasks/30-cache.tji:30`)

`depends !key_shapes` (`tasks/30-cache.tji:29`) — a sibling dependency within
the `cache` task, so transitively `keyed_store` → `key_shapes` → this.

## Effort estimate

**2d.** The store, the four base priority classes, the tile/block key shapes,
and the eviction machinery already exist (predecessors, below). This task adds
one `PriorityClass` enumerator, two pure ring-enumeration helpers, and a thin
classify-resident / report-absent driver layered over the existing
`reclassify` / `lookup` surface — plus their behavioral-counter tests and
three claims. No new rendering, no new data structures in the store, no
concurrency surface. The only real subtlety is the temporal-ring horizon
arithmetic and pinning the temporal class's eviction position, both localized.

## Inherited dependencies

**Settled:**

- `cache.keyed_store` — DONE 2026-07-05 (`bb63b9d`),
  `tasks/refinements/cache/keyed_store.md`. Delivers `arbc::KeyedStore<Key,
  Value>` with `insert / lookup / reclassify / remove / remove_if` and the
  counters `resident_bytes / budget / hits / misses / evictions`
  (`src/cache/arbc/cache/keyed_store.hpp:139-188`), the move-only RAII
  residency pin `arbc::CacheHold<Value>` (`keyed_store.hpp:71-119`), and the
  four-member `enum class PriorityClass { Speculative, Recent, Adjacent,
  Visible }` whose **declaration order is eviction order (victim-first)**
  (`keyed_store.hpp:21-26`). Its decision `keyed_store.md:314-325` already
  names *this* task as the owner of doc 11's temporal-ring enum extension and
  the class-assignment policy.
- `cache.key_shapes` — DONE 2026-07-05 (`a198981`),
  `tasks/refinements/cache/key_shapes.md`. Delivers `arbc::TileKey`
  (`content, revision, rung, coord, achieved_time`;
  `src/cache/arbc/cache/key_shapes.hpp:64-76`), `arbc::TileValue` /
  `arbc::TileMeta` (`key_shapes.hpp:99-113`), `using TileCache =
  KeyedStore<TileKey, TileValue>` (`key_shapes.hpp:129`), and the
  `arbc::BlockKey` vocabulary (`key_shapes.hpp:83-90`). `ScaleRung` /
  `TileCoord` newtypes at `key_shapes.hpp:39-53`.
- `cache.invalidation` — DONE 2026-07-05 (`df663cb`),
  `tasks/refinements/cache/invalidation.md`. Establishes the precedent this
  task mirrors: a **header-only free-function layer in `namespace
  arbc::cache`** (`src/cache/arbc/cache/invalidation.hpp:32`) built over the
  store's public surface, with all viewport/tile geometry **injected by the
  caller** (`invalidate_region`'s `Geom&& tile_rect`,
  `invalidation.hpp:47-57`).

**Pending (downstream consumers — not blockers):**

- `compositor.pull_service` (L4) — the concrete `PullService`
  (`src/contract/arbc/contract/content.hpp:254-262`) that will read the
  `playback_hint` and *drive* this layer. It does not exist yet
  (`content.hpp:249-253`), and this task must not wait on it: the seam is
  built so the compositor calls in, never the reverse.

## What this task is

The capstone of the cache stream. It turns the cache's flat priority-class
LRU into a **predictive residency policy** with two prefetch rings, exactly as
docs 02/11 promise. Concretely, three deliverables:

1. **Temporal-ring priority class.** Add `PriorityClass::Temporal` (doc 11's
   temporal prefetch ring) to the enum and to the out-of-line eviction-order
   array, positioned between `Recent` and `Adjacent`
   (`keyed_store.hpp:21-26`, `detail::cache_eviction_order()` in
   `keyed_store.cpp:10-19`, `k_priority_class_count` + its `static_assert` at
   `keyed_store.hpp:33`). This is the note's "priority-class integration."
2. **Ring-enumeration helpers** (pure, `base`-level, header-only in
   `arbc::cache`):
   - `pan_prefetch_ring(...)` — the spatial pan ring: the annulus of
     `TileKey`s within a caller-supplied tile radius of the visible tile set,
     excluding the visible set itself (doc 02's "adjacent (pan prefetch
     ring)").
   - `temporal_prefetch_ring(...)` — the temporal ring: the sequence of
     `TileKey`s at upcoming `achieved_time` buckets `start ± step·k` (k ≥ 1)
     in the playback `direction`, bounded so `|step·k| ≤ horizon` (doc 11's
     "upcoming times in playback direction … the playback-hint horizon bounds
     it").
3. **Classify-resident / report-absent driver.** Given an enumerated ring key
   set and its target `PriorityClass`, `reclassify` every already-resident
   member and return the absent members as a want-list for the caller to
   render. The cache renders and inserts nothing itself.

## Why it needs to be done

Doc 11's whole time story rests on prefetch. Smooth playback is *cheap*
(rather than merely possible) only because decoder-backed content pre-rolls
forward: the temporal ring is what keeps the next N frames resident so
achieved-time coalescing (doc 11:110-114 — "more than half of all playback
requests … become cache hits") pays off frame-over-frame. Without the
temporal class those upcoming tiles compete as ordinary entries and get
evicted between frames. Symmetrically, the spatial pan ring is what makes an
interactive pan hit cache instead of stalling on a fresh render at the leading
edge. Both rings are named in the tile-cache design (doc 02:92-93) as
first-class members of the priority ladder; the predecessors deliberately
deferred them here (`keyed_store.md:110-113`, `key_shapes.md:101-106`). The
downstream consumers are `compositor` (2D tiles) and `audio-engine` (1D
blocks), both L4, both driving this L3 layer (doc 17:56-57).

## Inputs / context

- **Governing design sections:**
  - `docs/design/02-architecture.md:87-116` — the tile cache: key/value
    shape, the priority ladder `visible > adjacent (pan prefetch ring) >
    recently visible > speculative (next/previous zoom rung)` (`:92-93`), the
    residency-pin semantics (`:100-109`), and the soft-budget eviction policy
    that climbs classes victim-first (`:110-116`).
  - `docs/design/11-time-and-video.md:138-143` — the cache bullet: the
    temporal prefetch ring, "upcoming times in playback direction … the
    playback-hint horizon bounds it," **and its eviction position** (amended
    by this task's delta, below).
  - `docs/design/11-time-and-video.md:115-121` — `playback_hint(direction,
    rate, horizon)`, advisory, *issued by the transport*. This is a **Content
    interface**, not a cache input: the compositor reads it and passes the
    resulting `direction` / `step` / `horizon` **values** into the cache.
  - `docs/design/11-time-and-video.md:110-114` — achieved-time coalescing is
    the *compositor's* read (the quantum/`step` the temporal ring walks is
    computed there, per `key_shapes.md:144-146`), not the cache's.
- **Levelization (load-bearing):** `docs/design/17-internal-components.md`
  places `arbc::cache` at **L3, deps `base, surface` only** (the row for
  `arbc::cache`), enforced by `scripts/check_levels.py`
  (`"cache": {"base", "surface"}`). "No same-level edges." `contract` (home of
  `PullService`) and the transport/`playback_hint` are at or above L3, so the
  cache **cannot name them**. `Time` (flicks) lives in `base`
  (`src/base/arbc/base/time.hpp`, `Time { std::int64_t flicks; }`), so the
  horizon and the achieved-time step are legal cache inputs as plain data.
  Doc 17:73-74 — "`cache` is engine-agnostic … tiles and blocks are the same
  machinery with different key shapes" — so this layer must not assume 2D.
- **Store surface built over:** `src/cache/arbc/cache/keyed_store.hpp` —
  `lookup(const Key&) -> std::optional<CacheHold>` (`:162`),
  `reclassify(const Key&, PriorityClass)` (`:166`),
  `insert(Key, Value, std::size_t, PriorityClass) -> CacheHold` (`:156`), the
  counters (`:184-188`). Enum + order: `keyed_store.hpp:21-33`,
  `keyed_store.cpp:10-19`.
- **Key shapes to enumerate:** `TileKey` (`key_shapes.hpp:64-76`),
  `ScaleRung` / `TileCoord` (`key_shapes.hpp:39-53`), `BlockKey`
  (`key_shapes.hpp:83-90`), `TileCache` (`key_shapes.hpp:129`).
- **Precedent to mirror:** `src/cache/arbc/cache/invalidation.hpp` — a
  header-only `arbc::cache` free-function layer over `remove_if`, with
  injected `Geom`. Prefetch is its dual over `reclassify` / `lookup`.
- **Tests:** Catch2 in `src/cache/t/` (registered via
  `arbc_component_test(COMPONENT cache …)` in `src/cache/CMakeLists.txt`);
  counters read directly off the store (e.g. `keyed_store.t.cpp:59-84`); the
  claims register `tests/claims/registry.tsv` (TAB-separated
  `<claim-id>\t<description>`), enforced bidirectionally by
  `scripts/check_claims.py` via `// enforces: <claim-id>` tags. Existing cache
  claims sit at `registry.tsv:54-59`.

## Constraints / requirements

- **L3 purity — no upward names.** No `#include` of, and no reference to,
  `contract` (`PullService`), the transport, `playback_hint`, the compositor,
  or any `model` type. Everything the rings need arrives as `base`/`surface`
  data: a `Time` horizon, a `Time` step, a direction sign, `TileKey`/`TileCoord`
  values, a radius, a target `PriorityClass`. CI (`scripts/check_levels.py`)
  fails the build otherwise.
- **The cache never renders and never fabricates a value.** Prefetch is a
  *survival + want-list* operation: it may `reclassify` existing entries and
  report absent keys, but it must not `insert`. A prefetch call must leave
  `resident_bytes()` and `evictions()` unchanged (it neither adds bytes nor
  forces eviction; only later caller-driven inserts do).
- **Enum extension stays localized and ordered.** `PriorityClass::Temporal`
  is inserted between `Recent` and `Adjacent` in *both* the enum
  (`keyed_store.hpp:21-26`) and `detail::cache_eviction_order()`
  (`keyed_store.cpp:10-19`); `k_priority_class_count` becomes 5 and its
  `static_assert` updated. The eviction loop already walks the array in order,
  so no eviction logic changes — this is the whole point of the predecessor's
  design (`keyed_store.md:98-101`).
- **Spatial ring → existing `Adjacent`; zoom rung → existing `Speculative`.**
  Per doc 02:92-93 the pan ring *is* the `Adjacent` class and the
  next/previous zoom rung *is* `Speculative`; only the temporal ring adds a
  class. The helpers classify into these existing members.
- **Temporal ring is horizon-bounded and direction-signed by injected
  values.** The cache does not compute frame cadence or quantize time — it
  walks `start ± step·k` while `|step·k| ≤ horizon`, all `Time` arithmetic on
  `base` values the compositor supplies (`key_shapes.md:144-146`,
  doc 11:110-114). Reverse-direction and beyond-horizon buckets are never
  enumerated.
- **Engine-agnostic shape.** The temporal-ring and classify/report helpers
  must be expressible for `BlockKey` too (audio blocks along playback
  direction, doc 17:73-74). The spatial pan ring is 2D-tile-specific and may
  be `TileKey`-typed; the temporal/classify surface should template over the
  key so `audio-engine` reuses it when `tasks/45-audio.tji` instantiates
  `BlockCache` (no `BlockCache` exists yet — `key_shapes.md:372-384`).
- **Single-threaded, header-only, no new deps.** Same posture as the
  predecessors: the store is render-thread-confined (no TSan obligation for
  this layer), the public header compiles standalone under
  `VERIFY_INTERFACE_HEADER_SETS`, and CI gates ≥90% diff coverage on changed
  lines (doc 16).

## Proposed surface (illustrative, not binding)

Header `src/cache/arbc/cache/prefetch.hpp`, `namespace arbc::cache`:

```cpp
// Spatial pan-prefetch ring: tiles within `radius` of the visible set,
// excluding the visible set. Pure tile-coordinate arithmetic.
std::vector<TileKey> pan_prefetch_ring(
    std::span<const TileKey> visible, std::int32_t radius);

// Temporal prefetch ring: `base` at each upcoming achieved_time bucket
// start + direction*step*k, k = 1.., while |step*k| <= horizon.
// direction is +1 / -1; step and horizon are base::Time.
template <class Key>
std::vector<Key> temporal_prefetch_ring(
    const Key& base, int direction, Time step, Time horizon);

// Reclassify already-resident ring members to `klass`; return the absent
// keys for the caller (compositor / audio-engine) to render + insert.
// Renders nothing, inserts nothing.
template <class Key, class Value>
std::vector<Key> prime_ring(KeyedStore<Key, Value>& store,
                            std::span<const Key> ring, PriorityClass klass);
```

The compositor's per-frame call pattern (illustrative): compute the visible
set → `prime_ring(store, visible, Visible)`; `prime_ring(store,
pan_prefetch_ring(visible, r), Adjacent)`; for a live `playback_hint`,
`prime_ring(store, temporal_prefetch_ring(v, dir, step, horizon), Temporal)`;
render each returned want-list entry and `insert` it with the same class.

## Acceptance criteria

- **Behavioral-counter unit tests** in `src/cache/t/prefetch.t.cpp` (Catch2),
  asserting exact counter deltas — never wall-clock. In particular: a
  `prime_ring` call leaves `resident_bytes()` and `evictions()` unchanged and
  reclassifies only resident members; the want-list equals exactly the absent
  members.
- **Claims-register growth** (`tests/claims/registry.tsv` + `// enforces:`
  tagged Catch2 cases), three entries pinning the doc promises:
  1. `11-time-and-video#temporal-prefetch-ring-bounded-by-horizon` — the
     temporal ring enumerates exactly the upcoming achieved-time buckets in
     the playback direction within the horizon (`start + dir*step*k` for
     k ≥ 1 while `|step*k| ≤ horizon`); no bucket beyond the horizon and none
     in the reverse direction is enumerated.
  2. `11-time-and-video#temporal-ring-evicts-between-recent-and-adjacent` —
     under budget pressure a `Temporal`-class entry is evicted *after* a
     `Recent` entry and *before* an `Adjacent` entry, pinning the
     victim-first order speculative < recent < temporal < adjacent < visible.
  3. `02-architecture#prefetch-ring-classifies-resident-reports-absent` —
     applying a prefetch ring reclassifies already-resident ring tiles to the
     ring's class (pan ring → `Adjacent`, zoom rung → `Speculative`) and
     returns the absent ring keys for the caller to render; the cache inserts
     nothing (resident bytes and evictions unchanged across the call).
- **Enum invariant test** — `k_priority_class_count == 5`, the `static_assert`
  holds, and `detail::cache_eviction_order()` lists all five members in
  victim-first order with `Temporal` between `Recent` and `Adjacent`.
- **Engine-agnostic instantiation test** — `temporal_prefetch_ring` and
  `prime_ring` instantiate and pass for both `TileKey` and `BlockKey` (over a
  local `KeyedStore<BlockKey, int>` fixture, since no production `BlockCache`
  exists yet), proving the surface is ready for `audio-engine` reuse.
- **Design-doc delta** `docs/design/11-time-and-video.md:138-143` (temporal
  ring's eviction position) rides in the closer's commit (doc 16 same-commit
  rule). No golden tests (pixels/blocks are produced downstream — same scoped
  absence the siblings note, `key_shapes.md:290-292`). No TSan (single-threaded
  store).
- **Gate:** build + cache tiers in Debug + ASan/UBSan green; `check_levels.py`,
  `check_claims.py` silent; `tj3 project.tjp` warning-free after the closer
  marks `complete 100`; ≥90% diff coverage on changed lines.

## Decisions

- **The temporal prefetch ring is a new `PriorityClass::Temporal`, ranked
  between `Recent` and `Adjacent`.** doc 11:141-143 sanctions the class
  ("priority classes gain a temporal prefetch ring") but left its eviction
  position open (a genuine gap); this task pins it via a design-doc delta.
  Rationale: an upcoming-playback frame is a *stronger* prediction than a
  maybe-return recently-visible tile, so it outranks `Recent`; but it yields
  to the spatial pan ring (`Adjacent`) so interactive scrubbing/panning stays
  responsive, and slotting it below `Adjacent` leaves doc 02's existing
  spatial ladder (visible/adjacent/recent/speculative relative order)
  untouched — the enum edit reorders no existing pair.
  *Rejected:* sharing the `Adjacent` class (loses the ability to tune
  playback-vs-pan survival, and doc 11 explicitly says a ring is added
  *alongside*, i.e. a distinct class); ranking it *above* `Adjacent` (would
  starve the pan ring during a scrub, and playback + pan rarely run hot at
  once so the gain is illusory); ranking it *below* `Recent` (understates how
  near-certain an imminent playback frame is).
- **Prefetch lives in `arbc::cache` as header-only free functions over
  `reclassify`/`lookup`; the enum edit lives in `keyed_store` (`arbc`).** This
  mirrors the `invalidation.hpp` precedent (dual operation, same namespace,
  same caller-injected geometry) and keeps the class list in its single owner.
  *Rejected:* a stateful `Prefetcher` object holding ring state (the store is
  already the state; a second cache of ring membership would drift and needs
  invalidation of its own); putting all ring logic in the compositor (then the
  pure ring arithmetic and the temporal class have no L3 home and can't be
  unit-tested against the store in isolation).
- **The cache classifies-resident and reports-absent; it never renders or
  inserts.** The cache is a passive L3 keyed store with no render capability —
  rendering is the L4 pull service. So "prefetch" at the cache is the survival
  *policy* (class assignment) plus the *want-list* (absent keys) the caller
  fills. *Rejected:* the cache triggering renders (requires naming
  `PullService` — a forbidden same-level edge, doc 17 — and makes the store
  non-passive).
- **The temporal ring is horizon-bounded by an injected `base::Time` and
  stepped by an injected achieved-time quantum; the cache does not quantize
  time.** Achieved-time coalescing/quantization is the compositor's
  responsibility (doc 11:110-114, `key_shapes.md:144-146`); the direction and
  horizon come from the transport's `playback_hint` (doc 11:115-121), which
  the cache may not name. Passing `(direction, step, horizon)` as plain values
  keeps the ring pure and testable while honoring levelization.
  *Rejected:* the cache reading `playback_hint` or computing frame cadence
  (both need knowledge/edges above L3).
- **The temporal/classify surface templates over the key so audio reuses it;
  the pan ring stays `TileKey`-specific.** doc 17:73-74 requires the cache be
  engine-agnostic; the temporal ring and the classify/report driver are
  key-shape-neutral, whereas 2D annulus arithmetic is inherently tile-shaped.
  *Rejected:* forcing a 1D "pan" analogue for audio (there is none — audio
  prefetch is purely temporal).

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-05.

- `src/cache/arbc/cache/prefetch.hpp` (new) — `pan_prefetch_ring`, `temporal_prefetch_ring<Key>`, `prime_ring<Key,Value>` + `arbc::prefetch_temporal_step` ADL axis overloads for `TileKey`/`BlockKey`.
- `src/cache/arbc/cache/keyed_store.hpp` — inserted `PriorityClass::Temporal` between `Recent`/`Adjacent`; `k_priority_class_count` → 5; comments updated.
- `src/cache/arbc/cache/keyed_store.cpp` — added `Temporal` to `cache_eviction_order()` array; `static_assert` → 5.
- `src/cache/CMakeLists.txt` — registered `prefetch.hpp` + `t/prefetch.t.cpp`.
- `src/cache/t/prefetch.t.cpp` (new) — 7 Catch2 cases: behavioral-counter assertions, enum-invariant, engine-agnostic `BlockKey` instantiation.
- `tests/claims/registry.tsv` — 3 claims appended: `11-time-and-video#temporal-prefetch-ring-bounded-by-horizon`, `11-time-and-video#temporal-ring-evicts-between-recent-and-adjacent`, `02-architecture#prefetch-ring-classifies-resident-reports-absent`.
- `docs/design/11-time-and-video.md:138-149` — eviction-position delta (Temporal between Recent and Adjacent) added per same-commit rule.
