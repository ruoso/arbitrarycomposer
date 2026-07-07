# timeline.temporal_cache — achieved_time coalescing

## TaskJuggler entry

`task temporal_cache` in [`tasks/40-time.tji`](../../40-time.tji) (lines 25–30),
under `task timeline`:

```
task temporal_cache "achieved_time coalescing" {
  effort 2d
  allocate team
  depends !rational_time, cache.key_shapes
  note "Time-keyed cache entries for Timed content; achieved_time coalescing
        serves whole inter-frame intervals from one entry (24fps content on
        60fps output). Doc 11."
}
```

## Effort estimate

**2d.** The mechanism this task names (time-keyed tile entries, quantize-before-lookup
coalescing, the zero-render behavioral counter) already shipped in the predecessor
tasks (see *Inherited dependencies* and *Inputs / context*). The remaining, genuinely
unshipped increment is narrow and concrete: close the **soundness loop at the live
tile-cache boundary** — the one linkage every predecessor proves only *indirectly*
(via composited-output byte-identity + a render counter) or *abstractly* (in a
contract-unit test against an in-test content double), never at the `TileCache`
insert→lookup API where the compositor actually keys a Timed tile. Two small edits
(a shared insert-key validation helper + its use at the three insert sites) plus two
enforcing tests and one claim fit the 2d estimate.

## Inherited dependencies

- **`timeline.rational_time`** — *settled* (`tasks/refinements/timeline/rational_time.md`,
  Done 2026-07-07). Delivered `arbc::base` (L0) time machinery: `Time`
  (integer flicks, `flicks_per_second = 705'600'000`) at
  `src/base/arbc/base/time.hpp:11-20`; `TimeRange` half-open `[start,end)` at
  `time.hpp:29-54`; `Rational`, `TimeMap`, `ComposedTimeMap` (single leaf rounding,
  ties-to-even) at `src/base/arbc/base/rational_time.hpp`. It explicitly **deferred
  `achieved_time` derivation and coalescing** to this task
  (rational_time.md:268-276) and left claim
  `11-time-and-video#quantize-time-matches-achieved-time` registered.
- **`cache.key_shapes`** — *settled* (`tasks/refinements/cache/key_shapes.md`,
  Done 2026-07-05). Delivered `struct TileKey` at
  `src/cache/arbc/cache/key_shapes.hpp:64-76`, carrying
  `std::optional<Time> achieved_time` (line 69) as a **single instant, not an
  interval** — `nullopt` for `Static`, present for `Timed`, with a `flicks==0`
  present-vs-absent collision guard in `std::hash<TileKey>` at `key_shapes.hpp:144-161`.
  `using TileCache = KeyedStore<TileKey, TileValue>` at `key_shapes.hpp:129`;
  `TileMeta { achieved_scale, exact }` at `key_shapes.hpp:99-102` (no time field).

Two further predecessors are not in this task's `depends` line but **already landed
most of the behavior the note describes**, and this refinement is scoped against
them so as not to duplicate their coverage:

- **`contract.temporal_fields`** — delivered the `Content` achieved-time surface:
  `Stability { Static, Timed }` (`src/contract/arbc/contract/content.hpp:25-33`),
  `virtual std::optional<Time> quantize_time(Time) const` defaulting to `nullopt`
  (`content.hpp:235`), `time_extent()` (`content.hpp:214`), and
  `RenderResult::achieved_time` (`content.hpp:88-95`). Its unit test
  `src/contract/t/temporal_fields.t.cpp:187-214` already enforces
  `11-time-and-video#quantize-time-matches-achieved-time` (render lands on
  `quantize_time`, idempotent) — but **against an inline `TimedContent` double, never
  against the key the compositor inserts under**.
- **The compositor `tile_planning` / `pull_service` tasks** — already implement the
  quantize-before-lookup coalescing: `plan_layer` computes
  `content->quantize_time(time).value_or(time)` and stamps the Timed key at
  `src/compositor/tile_planning.cpp:141-149`, then renders-on-miss and inserts inline
  at `tile_planning.cpp:378-382` (mirror sites `src/compositor/pull_service.cpp:184-185`
  and `src/compositor/refinement.cpp:85-87`). Their tests already enforce
  `11-time-and-video#achieved-time-coalescing-issues-zero-renders`
  (`tests/temporal_coalescing_golden.t.cpp:85`) and
  `11-time-and-video#static-tiles-survive-clock` (five enforcers across
  `src/compositor/t/{counters,damage_planning,temporal_coalescing,tile_planning}.t.cpp`).

## What this task is

Time-keyed caching of `Timed` content is the ability to serve a whole inter-frame
interval of output instants from **one** cached tile: 24 fps content sampled by a
60 fps transport renders each source frame once, and every output instant landing in
`[7/24, 8/24)` reuses that entry (doc 11:120-122). The key shape, the
quantize-before-lookup policy, and the "zero renders within one native frame"
behavioral counter all already exist (see *Inherited dependencies*).

What is **not** yet pinned — and is this task's deliverable — is the **soundness of
that reuse at the live cache boundary**, in two facets:

1. **Insert-key linkage.** The compositor keys a Timed tile at plan time from
   `quantize_time(time)` *before* it renders (`tile_planning.cpp:142-149`), then on a
   miss renders and inserts the resulting surface under that same key
   (`tile_planning.cpp:378-382`) — **silently discarding `result.achieved_time`**
   (`TileMeta` has no time field; `key_shapes.hpp:99-102`). Nothing verifies that the
   instant the render actually landed on equals the instant the tile was keyed under.
   A `Timed` content whose `render` lands off its own `quantize_time` grid (a
   contract violation of doc 11:134-137) would be cached under the wrong key and serve
   the wrong frame under seek — silently. This task adds a shared, directly-testable
   validation helper and calls it at the three insert sites.

2. **Live-cache round-trip.** Reuse is today proven only *indirectly*: the golden
   (`tests/temporal_coalescing_golden.t.cpp`) infers it from composited-output
   byte-identity plus a render counter; the compositor unit tests
   (`src/compositor/t/temporal_coalescing.t.cpp`) assert `plan_layer` *decision
   fields* against a hand-populated stub cache whose surfaces carry no pixels. No test
   drives an insert-on-miss and then a **direct `TileCache::lookup(key)`** that returns
   the *stored* `TileValue`/surface across two sub-frame-apart instants (hit), with a
   distinct entry across a native-frame boundary (miss). This task adds that
   round-trip test at the `KeyedStore` API.

This is a compositor + contract-boundary closure task, not a new cache data shape:
**no new `TileKey`/`TileValue`/`TileMeta` field, no new cache-component type, no new
levelization edge.**

## Why it needs to be done

Coalescing is a correctness mechanism, not just a performance one: the second request
in a native frame is served *pixels from the cache without rendering*, so those pixels
must be the frame the transport actually asked for. The linchpin is
`plan-time key == render-time achieved_time`. Doc 11:134-137 states it as a **MUST**
that the *content* honors (`quantize_time(t)` equals `render(t).achieved_time`,
idempotent), and `contract.temporal_fields` tests that the content honors it — but the
*compositor* trusts that MUST blind at the insert site, and no test exercises the
actual cache surface coming back. When `kinds.imageseq_plugin`
(`tasks/55-kinds.tji:61`, the first real `Timed` plugin: "spans, achieved_time on
source frames … the permanent end-to-end test of the runtime plugin path") lands, it
needs a live-cache soundness contract already pinned to render against; and any
first-party or test content that regresses the linkage should be caught by a
fail-fast tripwire rather than surface as a wrong-frame-under-seek bug. This task turns
doc 11's coalescing promise from "proven abstractly and behaviorally" into "pinned at
the cache API where the tile is stored and retrieved."

## Inputs / context

**Governing design doc (normative — doc 16):**

- **`docs/design/11-time-and-video.md`**, "Contract changes (doc 03)" (lines 96-148):
  `RenderResult.achieved_time` (`std::optional<Time>`, the local time actually
  rendered, lines 109-111); `achieved_time` as "the temporal `achieved_scale`" — a
  24 fps clip asked for `t=0.31 s` renders `7/24 s` and says so (lines 118-122);
  `quantize_time(Time) const → std::optional<Time>`, a render-free grid query
  returning `floor(t·24)/24` (lines 123-137). The **MUST** at lines 134-137: when
  `quantize_time(t)` has a value it MUST equal `render(time=t).achieved_time` and is
  idempotent, "so … the compositor may key a tile at the native instant before
  rendering and the render lands on that key."
- **`docs/design/11-time-and-video.md`**, "Pipeline changes (doc 02)" (lines 150-179):
  the tile key `(content id, revision, scale rung, tile coords, achieved_time)` for
  `Timed`, `Static` keys unchanged (lines 163-166); a sub-frame clock advance keys the
  same native frame and hits the cache (lines 152-157); "the compositor keys `Timed`
  tiles by `quantize_time(time).value_or(time)` … the second issues zero renders —
  sound under seek because the content, not the compositor, owns the grid"
  (lines 135-137).
- **`docs/design/11-time-and-video.md`** lines 124-130: why post-render `achieved_time`
  alone is insufficient ("one sample gives the floor, never the span; a floor-probe …
  over-serves a skipped frame under seek") — the motivation for forming the key at
  `7/24` *before* rendering, which this task's linkage check protects.

**Source seams this task extends:**

- `src/compositor/tile_planning.cpp:142-149` — Timed key construction from
  `quantize_time`.
- `src/compositor/tile_planning.cpp:307-399` — the render-on-miss + inline insert
  branch; insert at `:378-382` builds `TileValue{ surface, {result.achieved_scale,
  result.exact} }` and **drops `result.achieved_time`**. The linkage is asserted only
  as a comment at `:316-318`.
- `src/compositor/pull_service.cpp:122-125` (key), `:180-185` (render + insert) — the
  operator-input / recursive path, same shape.
- `src/compositor/refinement.cpp:79-87` — the async-arrival drain, same insert shape.
- `src/cache/arbc/cache/keyed_store.hpp` — `KeyedStore::insert` (line 163),
  `KeyedStore::lookup` (line 169, returns `std::optional<CacheHold<Value>>`), counters
  `hits()/misses()` (lines 193-194); `TileCache` alias at `key_shapes.hpp:129`. The
  round-trip test drives these directly.
- `src/contract/arbc/contract/content.hpp:88-95,235` —
  `RenderResult::achieved_time`, `quantize_time`.

**Predecessor / sibling conventions followed:**

- Claim-id format `<doc-file-stem>#<slug>`, tab-separated in
  `tests/claims/registry.tsv`, enforced both directions by `scripts/check_claims.py`
  via `// enforces: <claim-id>` comments immediately above the `TEST_CASE`
  (per `tasks/refinements/timeline/rational_time.md` and
  `tasks/refinements/cache/key_shapes.md`). `11-time-and-video` is the established doc
  stem for time-axis claims.
- Behavioral guarantees are pinned with **behavioral-counter / API assertions**, never
  wall-clock (doc 16), as `achieved-time-coalescing-issues-zero-renders` already does
  with a render counter.
- Component tests live in each component's `t/` dir; cross-component goldens live in
  `tests/`.

## Constraints / requirements

- **No new levelization edge (doc 17, CI-gated by `scripts/check_levels.py`).** The
  validation helper operates on `TileKey` (`arbc::cache`, L3) and `RenderResult` /
  `Stability` (`arbc::contract`, L3); both are already visible to `arbc::compositor`
  (L4, `Depends on: contract, cache`, doc 17:56). The helper therefore lives in
  **`arbc::compositor` (L4)** — e.g. `src/compositor/arbc/compositor/tile_planning.hpp`
  beside the plan types. It must **not** be placed in `arbc::cache` (L3 cannot depend
  on `contract` — a forbidden same-level edge, doc 17:53-54) and must **not** add a
  `contract`/`compositor` edge to the cache component.
- **Hot path unchanged.** The insert continues to key under the plan-time
  pre-quantized `TileKey`; the linkage check is additive and, for conformant content,
  a no-op. No re-keying after render.
- **`Static` content is exempt.** For `stability == Static` the key's `achieved_time`
  is `nullopt` and `render` reports `nullopt` `achieved_time`; the check applies only
  to `Timed` tiles and must treat the `Static`/`nullopt` case as trivially consistent.
- **No new cache field.** Do not add `achieved_time` to `TileMeta` — the key already
  carries the instant; duplicating it into every tile's metadata grows resident bytes
  for no lookup benefit (see Decision 2).
- **Deterministic, no new dependency (doc 10).** Test content is an in-repo double; no
  decoder/codec dependency (that arrives with `kinds.imageseq_plugin`).
- **≥90% diff coverage on changed lines (doc 16 CI gate).**

## Acceptance criteria

- **Claims-register growth.** Add one entry to `tests/claims/registry.tsv`:
  - `11-time-and-video#coalesced-timed-tile-round-trips-through-cache` — *On a
    cache miss the compositor keys a Timed tile at the `achieved_time` its render
    reports (equal to the pre-quantized plan key); a direct `TileCache::lookup` at any
    instant within the same native frame returns that stored surface, while an instant
    across a native-frame boundary keys a distinct entry — so coalescing serves the
    exact rendered frame and is sound under seek.*
- **Enforcing tests** (new `src/compositor/t/temporal_cache.t.cpp`, both tagged
  `// enforces: 11-time-and-video#coalesced-timed-tile-round-trips-through-cache`):
  1. **Live-cache round-trip (positive).** Build a real `arbc::TileCache`, a 24 fps
     `Timed` content double, and drive the compositor render-on-miss path at `t0`
     (frame 7); assert a subsequent **direct `cache.lookup(key)`** at `t0 + 1/60 s`
     (still frame 7) returns the *same stored `TileValue`/surface* (`hits()`
     increments, `misses()` does not, the returned surface is the inserted one), and
     that an instant at `8/24` keys a *distinct* entry (a second `miss`/insert). This
     asserts reuse at the `KeyedStore` API, not via composited-output byte-identity.
  2. **Insert-key linkage (negative).** A deliberately-misbehaving `Timed` double whose
     `render` lands on an `achieved_time` **off** its own `quantize_time` grid trips
     the validation helper (assert-fires under a debug build; and the extracted pure
     helper returns "inconsistent" when called directly in a release test build, so the
     claim holds regardless of `NDEBUG`). Conformant content passes.
- **Behavioral-counter, not wall-clock** (doc 16): reuse/seek are asserted via
  `KeyedStore::hits()/misses()` and the content double's render counter.
- **No regression of the existing coalescing claims.**
  `achieved-time-coalescing-issues-zero-renders`,
  `quantize-time-matches-achieved-time`, `static-tiles-survive-clock`, and
  `tile-key-carries-time-and-revision` continue to pass unchanged (this task adds
  coverage at a new boundary; it does not restate them).
- **Levelization + build green:** `scripts/check_levels.py` passes (no new edge);
  `scripts/check_claims.py` passes (registry ↔ `enforces:` both directions);
  `tj3 project.tjp 2>&1 | grep -iE "error|warning"` is silent after the closer marks
  `complete 100`.
- **Diff coverage ≥90%** on the changed compositor lines.
- **Real-content end-to-end proof is out of scope here** and is delivered by the
  existing WBS leaf `kinds.imageseq_plugin` (`tasks/55-kinds.tji:61`) — the first real
  `Timed` plugin renders against the live-cache soundness contract this task pins. No
  new task is registered for it.

## Decisions

1. **Scope this task to the live-cache-boundary soundness closure, not a
   reimplementation of coalescing.** The key shape, quantize-before-lookup, the
   zero-render golden, and `static-tiles-survive-clock` already shipped in
   `cache.key_shapes`, `contract.temporal_fields`, and the compositor
   `tile_planning`/`pull_service` tasks (evidence in *Inherited dependencies*). The
   honest remaining gap — surfaced by tracing the three insert sites — is that the
   `plan-time key == render-time achieved_time` linkage is proven only abstractly (a
   contract-unit double at `temporal_fields.t.cpp:209`) and reuse is proven only
   indirectly (composited byte-identity + render counter in the golden); neither is
   asserted at the `TileCache` insert→lookup API.
   *Rejected:* re-deriving the whole coalescing path here — it would duplicate shipped,
   already-enforced behavior and inflate the diff without adding coverage. *Rejected:*
   declaring the task already-done and closing it empty — the live-cache round-trip and
   the insert-site linkage are genuine, untested soundness properties, and closing
   empty would leave `kinds.imageseq_plugin` with no soundness contract to render
   against.
2. **Keep `achieved_time` in the key only; do not add it to `TileMeta`.** The linkage
   check reads `RenderResult.achieved_time` transiently at insert time and compares it
   to the already-present `key.achieved_time`; it does not need to persist in the
   stored tile.
   *Rejected:* adding `achieved_time` to `TileMeta` (`key_shapes.hpp:99-102`) so the
   stored tile "remembers" its instant — it grows resident bytes for every Timed tile
   and duplicates data the key already holds; the lookup already matches on the key, so
   the meta copy buys nothing.
3. **Validate at the insert site via a shared, directly-testable helper + a debug
   assertion, using standard `<cassert>` — not a faults-as-values error channel, and
   not a new project-wide assert macro.** Extract a pure predicate (e.g.
   `bool timed_insert_key_consistent(const TileKey&, const RenderResult&, Stability)`)
   in `arbc::compositor`; call it under `assert(...)` at the three insert sites
   (`tile_planning.cpp:378`, `pull_service.cpp:184`, `refinement.cpp:85`); and call it
   *directly* from the negative test so the claim holds even in `NDEBUG` test builds.
   The codebase has no assert facility today (grep for `ARBC_ASSERT`/`assert` in `src/`
   is empty) and uses faults-as-values (`expected<…, TimeError>`) for *runtime*
   fault domains; a content violating a doc-11 **MUST** is a *programming/conformance*
   fault, not a runtime input condition, and there is no error channel at the insert
   site (it returns a `CacheHold`, not an `expected`).
   *Rejected:* a faults-as-values path threaded through `insert` — over-engineers a
   by-contract-impossible condition through the hot path with one/two call sites today,
   and there is no caller positioned to handle the error. *Rejected:* introducing a new
   base-level `ARBC_ASSERT` seam — a new project convention is heavier than the one/two
   call sites justify; `<cassert>` is the language, not a new seam, and compiles out in
   release. *Rejected:* re-keying the tile to `result.achieved_time` after render — the
   design (doc 11:135-137) is that the content owns the grid and the plan key is
   authoritative *before* render precisely so seek is sound; re-keying after the fact
   would mask a contract violation instead of catching it.
4. **The linkage/round-trip claim lives under the `11-time-and-video` doc stem, as one
   new entry, distinct from the four existing time-axis claims.** It pins the
   *compositor-and-live-cache* boundary; the existing `quantize-time-matches-achieved-time`
   pins the *content* side and `achieved-time-coalescing-issues-zero-renders` pins the
   *behavioral counter*. One entry, two enforcing tests (positive round-trip + negative
   linkage).
   *Rejected:* two separate claims (one per facet) — they are two assertions of a single
   soundness property (serve the exact rendered frame; sound under seek) and read more
   clearly as one register line enforced by two tests. *Rejected:* extending an existing
   claim's `enforces:` set — the existing claims are content-side / behavioral and their
   descriptions do not cover the live-cache API round-trip; conflating them would blur
   the register.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-07.

- Declared `timed_insert_key_consistent` pure predicate in `src/compositor/arbc/compositor/tile_planning.hpp`.
- Defined the helper and added `assert` at the render-on-miss insert site in `src/compositor/tile_planning.cpp`; stamped `content->stability()` into `PendingTile`.
- Added `assert` at the operator-input insert site in `src/compositor/pull_service.cpp`; stamped `stability` into `PendingTile`; included `<cassert>`.
- Added `Stability stability` field to `PendingTile` in `src/compositor/arbc/compositor/refinement.hpp`.
- Added `assert` at the async-arrival drain insert site in `src/compositor/refinement.cpp`; included `<cassert>`.
- Registered new test in `src/compositor/CMakeLists.txt`; added claim `11-time-and-video#coalesced-timed-tile-round-trips-through-cache` to `tests/claims/registry.tsv`.
- Created `src/compositor/t/temporal_cache.t.cpp` with two unit tests: live-cache round-trip (positive, via `KeyedStore` insert→lookup with surface identity + hits/misses deltas) and insert-key linkage (negative, calling the pure helper directly so the claim holds under `NDEBUG`); both enforce the new claim.
