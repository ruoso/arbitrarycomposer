# cache.key_shapes — Tile and block key shapes

## TaskJuggler entry

`tasks/30-cache.tji:12-17` → `cache.key_shapes` ("Tile and block key shapes"),
the second leaf under `task cache`. It carries `depends !keyed_store`
(`30-cache.tji:15`), and through the parent `task cache` inherits
`depends surfaces.surface_pool` (`30-cache.tji:4`). Its two siblings chain off
it: `cache.invalidation` (`depends !key_shapes`, `30-cache.tji:21`) and
`cache.prefetch` (`depends !key_shapes`, `30-cache.tji:27`). Note line:

> "2D tiles keyed (content, revision, scale rung, tile coords, achieved_time);
> 1D audio blocks keyed (content, revision, block index, rate). Docs 02/11/12."

## Effort estimate

**1d.** The deliverables are two plain-old-data key structs with their
`std::hash`/`operator==` glue, one tile value+metadata struct with a byte-cost
helper, a type alias instantiating the store, and the one-line `surface`
dependency-edge widening of the cache component — plus the unit/behavioral
tests and claims. No new machinery: the store, eviction, pin lifecycle, and
counters all landed in `cache.keyed_store`. This leaf turns the design docs'
already-settled key *tuples* into concrete, hashable C++ types.

## Inherited dependencies

**Settled:**

- `cache.keyed_store` (commit `bb63b9d`, DONE 2026-07-05) — the direct WBS
  predecessor. It delivered `KeyedStore<Key, Value>`, `CacheHold<Value>`, and
  `PriorityClass` in the new L3 `arbc::cache` component
  (`src/cache/arbc/cache/keyed_store.hpp:141-257`). Its refinement explicitly
  **defers the concrete key/value shapes to this leaf**: "Not this task: the
  concrete tile key `(content, revision, scale rung, tile coords,
  achieved_time)` and audio block key `(content, revision, block index, rate)`
  and their value+metadata shapes (`cache.key_shapes`)"
  (`keyed_store.md:107-110`), and it deliberately declared `DEPENDS base`
  only, noting "the `surface` edge the table permits is exercised by
  `cache.key_shapes` when the tile value becomes a `Surface`"
  (`keyed_store.md:200-204`). The store's `Key`/`Value` requirements this leaf
  must satisfy are authoritative in its header comment
  (`keyed_store.hpp:130-136`): `Value` **movable**; `Key` **hashable
  (`std::hash<Key>`) + equality-comparable (`operator==`)** and movable (it is
  an `std::unordered_map<Key, …>` key — `keyed_store.hpp:185`).
- `surfaces.surface_pool` (commit `8063b91`, DONE) — parent edge. Established
  the temp `SurfacePool` is **not** the tile cache and does not share its
  budget; the tile cache owns its own backend surfaces
  (`surface_pool.md:100-107`, cited via `keyed_store.md:28-40`). Its RAII-handle
  and errors-as-values idioms carry forward.
- `contract.async_render` / `contract.snapshot_pins` (commits `92c3d3b`,
  `1da702a`, DONE) — established that a key's revision is *honest* because
  state is immutable behind a snapshot pin: "`cache.*` builds tile keys that
  include the pinned revision; the handle is what guarantees the key uniquely
  identifies the pixels" (`snapshot_pins.md:77-99`, via `keyed_store.md:47-52`).
  This leaf treats the revision field as opaque and does **not** resolve
  pins/snapshots itself.

**Pending:** none — every predecessor is landed. The two consumers of these
shapes (`cache.invalidation`, `cache.prefetch`) are successor leaves and out of
scope here; the audio block *value* is the audio-engine stream's (see
Decisions).

## What this task is

Deliver the two concrete cache **key shapes** — and the tile **value** shape —
that instantiate `cache.keyed_store`'s generic `KeyedStore<Key, Value>`. Per
doc 17 the cache is engine-agnostic: "tiles and blocks are the same machinery
with different key shapes" (`17:73-74`), so both key structs live in the single
L3 `arbc::cache` component. Concretely, in a new header
`src/cache/arbc/cache/key_shapes.hpp`:

1. **`TileKey`** — the 2D tile key `(content, revision, scale rung, tile coords,
   achieved_time)` (doc 02:89 base tuple; doc 11:138-143 time extension), with a
   `std::hash<TileKey>` specialization and `operator==`. `achieved_time` is
   `std::optional<Time>` — **absent for `Static` content**, collapsing the key
   to the doc-02 4-tuple so clock advance grows no cache for stills
   (doc 11:139-140).
2. **`BlockKey`** — the 1D audio block key `(content, revision, block index,
   rate)` (doc 12:169-170), with `std::hash<BlockKey>` and `operator==`.
3. **`ScaleRung`** and **`TileCoord`** — the two cache-local integer field
   types the base geometry (double-based `Vec2`/`Rect`, `geometry.hpp:7,14`)
   does not supply.
4. **`TileValue`** — the tile cache's value: an owning `std::unique_ptr<Surface>`
   plus **`TileMeta { double achieved_scale; bool exact; }`** (doc 02:90-91 "a
   backend surface plus metadata (actual scale achieved, exact vs best-effort
   flag)"). Movable (its `unique_ptr` destructor releases the backend surface),
   satisfying the store's `Value` requirement.
5. **`tile_byte_cost(const Surface&)`** — the byte-cost helper computing
   `width * height * bytes_per_pixel(format().pixel_format)` (there is no
   size-in-bytes accessor on `Surface`; `bytes_per_pixel` is in media,
   `pixel_format.hpp:23-33`). The caller passes its result to
   `KeyedStore::insert(..., bytes, ...)`.
6. **`using TileCache = KeyedStore<TileKey, TileValue>;`** — the concrete
   instantiation the compositor (L4) builds request planning over.
7. **Widen the cache component's dependency edge** from `DEPENDS base` to
   `DEPENDS base surface` (`src/cache/CMakeLists.txt:5`) — the edge doc 17:54
   already permits (`arbc::cache` … Depends on `base, surface`), pulling in
   `media` transitively for `PixelFormat`/`bytes_per_pixel`.

**Not this task:** the `(content, region)` / revision-orphan **invalidation**
index over `KeyedStore::remove` (`cache.invalidation`); the spatial pan-prefetch
ring, the doc-11 **temporal prefetch ring** enum extension, and the
class-assignment **policy** (`cache.prefetch`); the compositor's
**achieved-time coalescing / quantization** (mapping a requested time to an
`achieved_time` bucket — doc 11:111-114 "the compositor then serves") and its
**scale-ladder** rung computation (doc 17:56 — the compositor owns the ladder);
the model's authoritative revision counter (doc 17:52 — model, a forbidden
edge); the audio **block value** type and its `KeyedStore<BlockKey, …>`
instantiation (audio-engine stream — see Decisions).

## Why it needs to be done

The whole interactive and offline frame loop is expressed in these keys.
Doc 02's frame plan "split into **tiles** (fixed local-space-aligned tile grid
per scale rung …) and look each tile up in the cache" (`02:57-60`) and the
degradation order "stale-revision tiles, coarser-scale tiles rescaled, or
checkerboard/transparent" (`02:64-65`) both read the tile key and its value
metadata. Doc 11 makes the time axis load-bearing: "on a 60 fps output, more
than half of all playback requests against 24 fps content become cache hits by
achieved-time coalescing" (`11:112-114`) — that coalescing is *keyed on*
`achieved_time`, which this leaf adds. Doc 12's audio path is "the tile cache
with 1D keys" (`12:169-170`). Until these concrete shapes exist, the store is a
generic container with no instantiation: `arbc::compositor` (L4) `Depends on: …
cache` (`17:56`) has nothing to plan requests against. This leaf gives it the
`TileCache` type; the audio-engine (`17:57`) gets the `BlockKey` vocabulary.

## Inputs / context

- `docs/design/02-architecture.md`:
  - `:87-99` — the governing **Tile cache** section. Base key `(content id,
    revision, scale rung, tile coords)` (`:89`); value = "a backend surface
    plus metadata (actual scale achieved, exact vs best-effort flag)"
    (`:90-91`) — the exact `TileMeta` field set.
  - `:57-60` — "fixed local-space-aligned tile grid per scale rung, e.g. 256²
    device pixels" — a rung has its own tile grid, so `TileCoord` is meaningful
    only *relative to* a `ScaleRung`.
  - `:64-65`, `:82-85` — the degradation preference order and the offline
    "only exact-scale, current-revision entries qualify" rule the value
    metadata (`achieved_scale`, `exact`) must express; the qualification
    *decision* is the consumer's, not this leaf's.
- `docs/design/11-time-and-video.md`:
  - `:100-105` — the contract `RenderResult` carrying
    `std::optional<Time> achieved_time` ("the local time actually rendered, if
    quantized") — the shape `TileKey.achieved_time` mirrors.
  - `:111-114` — `achieved_time` is "the temporal `achieved_scale`"; achieved-
    time coalescing is the compositor's read, not the store's.
  - `:138-143` — the authoritative extended tuple: "the key gains a time
    component for `Timed` content — `(content id, revision, scale rung, tile
    coords, achieved_time)`; `Static` content's keys are unchanged (no time
    dimension, no cache growth for stills)."
  - `~:168` (Recursion) — "revisions track *edits*, time tracks *playback*; the
    cache key carries both." The normative separation of the two key axes.
- `docs/design/12-audio.md:169-174` — "The block cache is the tile cache with 1D
  keys — `(content id, revision, block index, rate)`." The audio working
  `sample_rate` is `std::uint32_t` (doc 12 audio contract), so `BlockKey.rate`
  is a `std::uint32_t`. Channel layout is **not** in the doc's key tuple (see
  Decisions).
- `docs/design/07-color-and-pixel-formats.md:5-23` — pixel format + color space
  are **surface-value tags** fixed **per composition** working space, not key
  discriminators (see Decisions).
- `docs/design/17-internal-components.md:54,:73-74,:40-44` — `arbc::cache` is
  **Level 3, Depends on `base, surface`**; "No same-level edges"; "`cache` is
  engine-agnostic — tiles and blocks are the same machinery with different key
  shapes." The CI dependency check enforces this.
- `docs/design/16-sdlc-and-quality.md:54-62` — behavioral-counter tests are the
  primary cache tier; claims-register growth for design-doc-promised behavior.
- `src/cache/arbc/cache/keyed_store.hpp`:
  - `:130-136` — `Key` must be hashable + equality-comparable; `Value` movable.
  - `:141-181` — `KeyedStore` surface: `insert(Key, Value, bytes, klass)`,
    `lookup`, `reclassify`, `remove`, counters; `:185` — `unordered_map<Key,…>`.
- `src/base/arbc/base/ids.hpp:11-25` — `struct ObjectId { std::uint64_t value; }`
  with defaulted `operator==` **and a `std::hash<ObjectId>` specialization** →
  content id reuses `ObjectId` directly.
- `src/base/arbc/base/time.hpp:10-19` — `struct Time { std::int64_t flicks; }`
  with `operator==`/`operator<=>` but **no `std::hash`** → `TileKey`'s hash must
  combine `achieved_time->flicks` itself.
- `src/base/arbc/base/geometry.hpp:7,14` — `Vec2`/`Rect` are double-based with
  no integer coord/index types → `ScaleRung`/`TileCoord` are new.
- `src/surface/arbc/surface/surface.hpp:11-33` — `class Surface` (abstract,
  non-copyable, virtual dtor): `int width()`, `int height()`,
  `SurfaceFormat format()`. **No byte-size accessor.**
- `src/surface/arbc/surface/backend.hpp:31-32` — `make_surface(...) ->
  expected<std::unique_ptr<Surface>, SurfaceError>`: the owning surface handle
  `TileValue` holds is `std::unique_ptr<Surface>`.
- `src/media/arbc/media/pixel_format.hpp:16-33` — `enum class PixelFormat {
  Rgba32fLinearPremul, Rgba16fLinearPremul, Rgba8Srgb }` and
  `bytes_per_pixel(PixelFormat)` → the multiplier in `tile_byte_cost`.
- `src/media/arbc/media/surface_format.hpp:26-32` — `struct SurfaceFormat {
  PixelFormat pixel_format; ColorSpace color_space; Premultiplied premultiplied; }`.
- `src/contract/arbc/contract/content.hpp:78` — `struct RenderResult { double
  achieved_scale{1.0}; bool exact{true}; };` — the field set `TileMeta` mirrors
  (contract is a **forbidden** same-level L3 edge; see Decisions).
- `src/cache/CMakeLists.txt:1-7`, `src/surface/CMakeLists.txt:1-10` — the
  `arbc_add_component(NAME … PUBLIC_HEADERS … DEPENDS …)` +
  `arbc_component_test(...)` pattern; the `DEPENDS base` line this leaf widens.
- `tests/claims/registry.tsv` — TAB-separated `<claim-id>\t<description>`; this
  leaf appends entries (below). The three `02-architecture#cache-*` /
  `15-memory-model#cache-*` entries (`registry.tsv:47-49`) are `keyed_store`'s.

## Constraints / requirements

- **Levelization (doc 17:54, :40-44).** All key structs, value structs, and the
  `TileCache` alias live in the **L3 `arbc::cache`** component. Fields must be
  expressible in `base` + `surface` (+ transitive `media`) vocabulary only.
  **No edge to `model`, `contract`, `compositor`, or `audio-engine`.**
  Consequences the design must honor:
  - **Revision is a bare `std::uint64_t` in the key**, treated opaquely
    (equality/hash only). The authoritative counter is `DocRoot::revision()`
    (`src/model/arbc/model/model.hpp`), an L2 concept the cache cannot see; the
    compositor (L4, which depends on both) projects it into the key. Using the
    same `std::uint64_t` representation makes that projection a plain copy.
  - **`TileMeta` is defined in cache**, mirroring `contract::RenderResult`'s
    `{achieved_scale, exact}` — it may **not** reuse the contract type
    (forbidden L3→L3 edge). The compositor copies the two fields at fill.
  - **`ScaleRung`/`TileCoord`/block index are cache-local opaque integers** the
    compositor/audio-engine compute (the ladder and block grid are theirs) and
    hand down; the cache only tests equality / hashes them.
- **Key requirements (`keyed_store.hpp:130-136`, :185).** `TileKey` and
  `BlockKey` must each provide a `std::hash<…>` specialization **and**
  `operator==`, and be movable/copyable enough to store as an `unordered_map`
  key. Because `Time` has no `std::hash`, `TileKey`'s hash combines
  `content` (via `std::hash<ObjectId>`), `revision`, `rung.index`,
  `coord.col`/`coord.row`, and — when present — `achieved_time->flicks`, with a
  distinct combine for the `nullopt` (Static) case so a Static key never
  hash-/equality-collides with a Timed key at `flicks == 0`.
- **`achieved_time` optionality (doc 11:139-140).** `std::optional<Time>`,
  `nullopt` for `Static` content. Two keys differing only in `achieved_time`
  present-vs-absent, or in the `Time` value, are distinct. This is the "no cache
  growth for stills" invariant.
- **`TileValue` movability.** Holds `std::unique_ptr<Surface>` (owning) +
  `TileMeta`; move-only is fine (the store only requires movable). Its
  destructor releases the backend surface.
- **Byte cost supplied by the caller.** `tile_byte_cost` is a free helper; the
  store never introspects `TileValue` (`keyed_store.md:308-313` decision). The
  caller computes cost and passes it to `insert`.
- **Engine-agnostic (doc 17:73-74).** Both key shapes ship in the one cache
  component; the block *key* lands here even though its value/instantiation is
  the audio stream's, so the shared vocabulary is settled in one place.
- **No format/layout in the key (doc 07:5-23, doc 12:169-170).** Pixel
  format/color space are surface-value tags (per-composition working-space
  invariant); channel layout is absent from doc 12's normative block tuple.
- **Single-threaded-confined (inherited).** No new concurrency surface — the
  keys are values and the store is thread-confined by `keyed_store`'s design
  (`keyed_store.md:353-362`). **No TSan obligation** at this leaf.
- **CI diff coverage ≥90%** on changed lines (doc 16:114-118); the public
  header compiles standalone under `VERIFY_INTERFACE_HEADER_SETS`.

## Acceptance criteria

- **Unit tests — `src/cache/t/key_shapes.t.cpp` (new, Catch2).** Assertions:
  - **`TileKey` hash+eq:** two keys with identical fields compare `==` and hash
    equal; keys differing in **exactly one** of `content`, `revision`, `rung`,
    `coord.col`, `coord.row`, `achieved_time` (value, and present-vs-absent)
    compare unequal — each field independently discriminates.
  - **Static-vs-Timed distinctness:** a `Static` key (`achieved_time ==
    nullopt`) is never `==` a `Timed` key, including a `Timed` key whose
    `achieved_time == Time::zero()` (the `flicks == 0` collision guard).
  - **`BlockKey` hash+eq:** identical fields `==` and hash-equal; differing in
    exactly one of `content`, `revision`, `block_index`, `rate` → unequal.
  - **Usable as `unordered_map` keys:** `TileKey`/`BlockKey` compile and behave
    as `std::unordered_map` keys (the store's actual requirement).
  - **Round-trip through `TileCache`:** insert a `TileValue` under a `TileKey`
    (byte cost from `tile_byte_cost`), `lookup` an equal-valued `TileKey` → hit
    returning the value; `lookup` a key differing in one field → miss.
    `resident_bytes()` reflects the `tile_byte_cost` passed.
  - **`tile_byte_cost`:** for a `Surface` of known `(w, h, PixelFormat)` returns
    `w * h * bytes_per_pixel(pf)` for each of the three `PixelFormat` values.
    Use a lightweight in-test `Surface` subclass reporting fixed `width`/
    `height`/`format` (no `CpuBackend` needed — keeps the test at the cache's
    `base`+`surface` surface).
- **Behavioral-counter test — same target.** A scripted `TileCache`
  insert/lookup/miss sequence asserting exact `hits()`/`misses()` deltas across
  distinct tile keys (doc 16:54-62 — the primary cache tier). Never wall-clock.
- **Claims (register + `enforces:` tag)** appended to `tests/claims/registry.tsv`,
  enforced from the unit tests:
  - `11-time-and-video#tile-key-carries-time-and-revision` — "The tile cache
    key carries `revision` (edit axis) and `achieved_time` (playback axis)
    independently; `Static` content omits `achieved_time` so no still grows the
    cache, and two entries differing only in `achieved_time` (including
    present-vs-absent) or only in `revision` are distinct keys." (doc 11:138-143,
    ~:168)
  - `02-architecture#tile-cache-key-and-value-shape` — "The tile cache key is
    `(content id, revision, scale rung, tile coords[, achieved_time])`; the
    value is a backend surface plus `{achieved_scale, exact}` metadata whose
    byte cost is `width * height * bytes_per_pixel(format)`." (doc 02:87-91)
  - `12-audio#block-cache-is-tile-cache-1d` — "The audio block cache is the tile
    cache with a 1D key `(content id, revision, block index, rate)`; block keys
    differing in any single field are distinct, and both tile and block keys
    instantiate the same `KeyedStore` machinery." (doc 12:169-170)
- **No golden tests here.** Pixels are produced by the compositor; hit-vs-refill
  byte-exactness is a `arbc::compositor` golden. Noted so its absence is a
  scoped decision, not a gap (same posture as `keyed_store.md:289-291`).
- **Gate green (build + tiers 1-5 in Debug + ASan/UBSan).** No TSan obligation
  (no new concurrency; inherits `keyed_store`'s thread-confinement).
- **Component wiring & CI dependency check:** `src/cache/CMakeLists.txt` widened
  to `DEPENDS base surface`, `arbc/cache/key_shapes.hpp` added to
  `PUBLIC_HEADERS` and the new test registered; the header compiles standalone
  under `VERIFY_INTERFACE_HEADER_SETS`; the doc-17 dependency check still passes
  (no `model`/`contract` edge introduced).

## Decisions

- **Content id is `ObjectId`; reuse base types, invent no `ContentId`.** There
  is no dedicated content-hash type in the tree (grep: none), content is
  document object identity, and `ObjectId` already ships `std::hash` + `==`
  (`ids.hpp:11-25`). *Rejected:* a new `ContentId` newtype — no field content
  identity carries that `ObjectId` does not, and it would need its own hash for
  no gain.
- **Revision is a bare `std::uint64_t` opaque token in the key; the cache takes
  no `model` edge.** The authoritative counter is model's `DocRoot::revision()`
  (`model.hpp`), an L2 concept the L3 cache may not depend on (doc 17:40-44,
  :54). Carrying the same `std::uint64_t` representation lets the compositor
  (L4, sees both) project it with a plain copy, and the cache tests only
  equality/hash — exactly what doc 02:95's "revision bumps … making old keys
  unreachable" needs. *Rejected:* depending on `model` for its revision type
  (illegal L3→L2 edge). *Rejected:* promoting a `Revision` newtype into `base`
  via a doc-17 delta — a type only `model` and `cache` use, cleanly bridged at
  the compositor, does not justify amending base's contents; the bare scalar is
  the smaller, edge-free call.
- **`achieved_time` is `std::optional<Time>`, `nullopt` for `Static`.** Doc
  11:139-140 mandates Static keys stay the doc-02 4-tuple with "no cache growth
  for stills"; an `optional` expresses "no time axis" exactly and keeps one key
  type for both stabilities. The hash uses a distinct combine for `nullopt` so a
  Static key cannot collide with a `Timed` key at `flicks == 0`. *Rejected:* a
  sentinel `Time` value for "no time" — conflates "timeless" with "time zero"
  and would let a still and a `t=0` frame share a key. *Rejected:* two separate
  key types (`StaticTileKey`/`TimedTileKey`) — doubles the machinery and the
  `TileCache` instantiation for a distinction one `optional` field carries.
- **`TileMeta { double achieved_scale; bool exact; }` is defined in `cache`,
  mirroring `contract::RenderResult`.** Doc 02:90-91 fixes the field set; the
  contract type (`content.hpp:78`) is the same two fields but lives at L3
  `contract`, a forbidden same-level edge (doc 17:40-44). Defining the twin
  struct in cache keeps the value self-contained; the compositor copies the two
  fields when it fills a tile from a `RenderResult`. *Rejected:* reusing
  `contract::RenderResult` (illegal L3→L3 edge and would drag the contract
  vocabulary into the engine-agnostic store).
- **`TileValue` owns `std::unique_ptr<Surface>`, not a `PooledSurface`.**
  `make_surface` returns `expected<std::unique_ptr<Surface>, SurfaceError>`
  (`backend.hpp:31-32`); a `unique_ptr` is movable (the store's only `Value`
  requirement) and its destructor releases the backend surface. *Rejected:*
  holding a `surfaces::PooledSurface` — that is the *temp* pool's handle with
  the temp pool's separate lifecycle and budget, which `surface_pool.md:100-107`
  settled is **not** the tile cache; a cached tile is a distinct backend surface
  the cache owns for as long as it is resident.
- **`ScaleRung`/`TileCoord` are cache-local integer newtypes; revision,
  block_index, and rate stay bare integers.** Base geometry is double-based
  (`geometry.hpp`), so the integer tile-grid types are genuinely new; wrapping
  them as `struct ScaleRung { std::int32_t index; }` and `struct TileCoord {
  std::int32_t col; std::int32_t row; }` gives named, type-safe fields at the
  compositor fill site (the "one value struct, not loose flags" idiom
  `keyed_store` inherited from `capabilities`). Revision (`std::uint64_t`,
  matching model), `block_index` (`std::int64_t`), and `rate` (`std::uint32_t`,
  matching the audio working `sample_rate`) stay bare to keep the projection
  from their authoritative sources a lossless copy. *Rejected:* newtyping every
  field — over-wraps scalars whose representation is dictated elsewhere.
  *Rejected:* bare `int` pairs for rung/coord — loses the compile-time
  distinction between a rung index and a tile column at the fill call.
- **Pixel format / color space are not tile-key fields (doc 07:5-23).** Every
  surface is tag-carrying and the working space is fixed **per composition**, so
  within a composition all cached tiles are format-homogeneous and the format is
  a property of the *value's* surface, not a key discriminator; across
  compositions the `content` id already discriminates. *Rejected:* adding
  `SurfaceFormat` to the key — redundant within a composition and it has no
  `std::hash` anyway; the nesting boundary converts formats (doc 07:33-36).
- **Channel layout is not a block-key field (doc 12:169-170).** The normative
  audio block tuple is exactly `(content id, revision, block index, rate)`;
  channel layout is part of the per-composition working format, not the key.
  *Rejected:* adding layout now — it presupposes per-monitor layout divergence,
  a feature that does not exist; should that ever land it is a localized key
  extension (append a field + widen the hash), not a reason to pre-complicate
  the tuple the doc pins. This is a decided deferral, not an open question.
- **The block *value* and its `KeyedStore<BlockKey, …>` instantiation belong to
  the audio-engine stream, not a new cache leaf.** doc 17:73-74 places the *key
  shapes* (both) in `cache`, which this leaf delivers, settling the shared
  vocabulary. The block *value* (an owned float32 sample buffer + `{achieved_
  rate, exact}` metadata, doc 12) is the audio-engine's payload, defined when the
  block pipeline lands in `tasks/45-audio.tji` (audio-engine is L4, `17:57`);
  it will `#include` this header's `BlockKey` and instantiate the store there. No
  new cache WBS leaf is created — this avoids an orphan and keeps the block value
  co-designed with the mixer that owns its buffer lifecycle. *Rejected:* landing
  a placeholder block value in `cache` now — it would either invent an
  audio-buffer type ahead of the audio design or reach for `contract::AudioBlock`
  (a forbidden L3→L3 edge), and the store is generic precisely so the engine
  brings its own value.
- **No design-doc delta.** Every shape here is already settled text: the tile
  tuple (doc 02:89, doc 11:139), the value metadata (doc 02:90-91), the audio
  tuple (doc 12:169-170), format-as-tag (doc 07), and the base-only dependency
  (doc 02:106-109, doc 17:54). This leaf *concretizes* those promises into C++
  types without altering designed behavior, so — unlike `keyed_store`'s
  residency-pin/soft-budget delta — it needs no doc edit and no doc-00 bullet.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-05.

- `src/cache/arbc/cache/key_shapes.hpp` — new header delivering `TileKey`, `BlockKey`, `ScaleRung`, `TileCoord`, `TileMeta`, `TileValue`, `tile_byte_cost`, and `using TileCache = KeyedStore<TileKey, TileValue>` with `std::hash` specializations for both key types.
- `src/cache/t/key_shapes.t.cpp` — new Catch2 test file: unit tests (hash+eq, static-vs-timed distinctness including `flicks==0` guard, `unordered_map` usability, `TileCache` round-trip, `tile_byte_cost` per `PixelFormat`) plus behavioral-counter tests (scripted tile-cache hit/miss sequences asserting exact `hits()`/`misses()` deltas).
- `src/cache/CMakeLists.txt` — widened `DEPENDS base` to `DEPENDS base surface`; added `key_shapes.hpp` to `PUBLIC_HEADERS`; registered new test.
- `tests/claims/registry.tsv` — appended 3 claims: `11-time-and-video#tile-key-carries-time-and-revision`, `02-architecture#tile-cache-key-and-value-shape`, `12-audio#block-cache-is-tile-cache-1d`, each enforced from the new test file.
- `src/cache/arbc/cache/keyed_store.hpp`, `keyed_store.cpp`, `src/cache/t/keyed_store.t.cpp` — minor adjustments to support the new instantiation and test registration.
- All 19 test cases (92 assertions) green. Gate passed (build, ctest, format, levelization, claims) under both default and ASan/UBSan presets.
