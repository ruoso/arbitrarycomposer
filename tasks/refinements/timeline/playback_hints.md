# timeline.playback_hints — Playback hints

## TaskJuggler entry

Defined in [`tasks/40-time.tji`](../../40-time.tji) at lines 33–38, under
`task timeline "Time and video"`:

```
task playback_hints "Playback hints" {
  effort 1d
  allocate team
  depends !transport
  note "Advisory (direction, rate, horizon) from the transport to decoder-backed
        content; feeds the temporal prefetch ring. Doc 11."
}
```

The `!transport` dependency is sibling-relative (the `!` names the sibling
`timeline.transport` within the same parent container).

## Effort estimate

**1d** (`tasks/40-time.tji:34`). Smaller than the 2d transport/temporal-cache
tasks: both ends of the wire already exist (see Inherited dependencies), so
this task is a derivation, a contract seam, and a runtime drive — not new
machinery. Doc 11 sizes it as part of "Time-keyed caching + temporal prefetch
ring + playback hints … the only part with real performance subtlety"
(`docs/design/11-time-and-video.md:232–243`); the subtlety (decode-ahead,
cache pressure) lives in the already-built ring, not here.

## Inherited dependencies

- **`timeline.transport`** — *settled*
  (`tasks/refinements/timeline/transport.md`, landed 2026-07-07). Produced the
  hint's producer, `arbc::Transport`
  (`src/runtime/arbc/runtime/transport.hpp:34`), an L5 `arbc::runtime` value
  type. This task reads its pure accessors: `position()` (`:42`), `rate()`
  (`:44`, a `Rational`, negative = reverse, retained across pause),
  `is_paused()` (`:45`, a distinct boolean — **not** `rate == 0`), and
  `loop()` (`:46`). There is **no `direction` accessor**: direction is the
  sign of `rate()` (transport.md Decision 2). The transport's rational
  real-time→content-time scaling (`advance`, `:86`, reusing
  `TimeMap{in=0, rate, offset=0}.evaluate` with one ties-to-even leaf
  rounding) is the exact-arithmetic primitive this task reuses to scale the
  horizon.
- **`timeline.rational_time`** — *settled* (transitive, via transport). Supplies
  `base::Time` (flicks) and `base::Rational`, the vocabulary the hint is built
  from and the `base`-typed values the cache ring consumes.
- **`cache.prefetch`** — *settled* (the consumer, already built). The temporal
  prefetch ring this task feeds already exists in L3 `arbc::cache`:
  `cache::temporal_prefetch_ring(base, int direction, Time step, Time horizon)`
  (`src/cache/arbc/cache/prefetch.hpp:110`) and the residency driver
  `cache::prime_ring(store, ring, PriorityClass::Temporal)` (`:133`).
  `PriorityClass::Temporal` is defined at
  `src/cache/arbc/cache/keyed_store.hpp:29`. The ring takes direction/step/
  horizon as plain `base` data by design (`prefetch.hpp:27–33`); the cache
  cannot name a hint type and never calls up.

No **pending** inherited dependencies — every seam this task touches is landed.

## What this task is

Two seams and one derivation:

1. **Contract seam.** Add the advisory `playback_hint` interface to
   `arbc::Content`: a `PlaybackHint` value (`{direction, rate, horizon}`) and
   an optional, null-defaulted `Content::playback_hint(const PlaybackHint&)`
   method that decoder-backed content overrides to pre-roll sequentially.
2. **Runtime derivation.** `derive_playback_hint(const Transport&, Time
   real_lookahead)` — the pure function that turns transport state into the
   `(direction, rate, horizon)` triple, with the paused/reverse/zero-rate edge
   cases and the rate-scaled horizon settled.
3. **Runtime drive.** `drive_playback_prefetch(...)` — the per-frame runtime
   path that (a) fans the derived hint out to the frame's participating
   `Timed` content via `playback_hint`, and (b) unpacks the hint into the
   `base`-typed `(direction, step, horizon)` and drives
   `cache::temporal_prefetch_ring` + `prime_ring(…, Temporal)` from the visible
   `Timed` anchor key(s), collecting the absent want-list for the pull service
   to render opportunistically.

The hint is **advisory** end to end: it changes no pixels and no cache
correctness. Determinism is owned by `quantize_time`/`achieved_time`, not by
whether a hint was issued or honored (`docs/design/11-time-and-video.md:156–166`).

## Why it needs to be done

Doc 11 names playback hints as the mechanism "that makes smooth playback
*cheap* rather than merely possible"
(`docs/design/11-time-and-video.md:160–166`). Today the two ends exist but are
unconnected: the producer `arbc::Transport` exposes `rate()`/`is_paused()`/
`position()`, and the consumer `cache::temporal_prefetch_ring` already accepts
direction+horizon — but `temporal_prefetch_ring` has **no production caller**
(only unit tests, `src/cache/t/prefetch.t.cpp`), and `arbc::Content` has **no
`playback_hint` method at all**. The compositor's `prime_prefetch` deliberately
drives only the spatial/zoom rings and leaves the temporal ring to the runtime:
"driving the temporal (playback) prefetch ring is the runtime transport path"
(`src/compositor/arbc/compositor/refinement.hpp:52–53`). This task is that path.

Downstream consumers:

- **`kinds.imageseq_plugin`** (`tasks/55-kinds.tji:61`, `depends
  timeline.temporal_cache`) — the out-of-lib `Timed` visual plugin, "the
  permanent end-to-end test of the runtime plugin path." It is the first real
  decoder-backed content to *override* `Content::playback_hint` and pre-roll on
  it; this task lands the seam it implements. (No real codec/decoder is built
  here — that dependency is the plugin's, per doc 17's codec line.)
- **`runtime.host_objects`** (`tasks/65-runtime.tji:27`, `depends
  !interactive, model.content_binding`) — the object model that threads the
  transport into the live frame loop. It is the call site that, each frame,
  sources the visible `Timed` anchor key(s), the native `step`, and the
  participating content set from the plan and invokes this task's
  `derive_playback_hint` + `drive_playback_prefetch`. (Integration into the
  live loop is host_objects', not this task; see Acceptance criteria — no new
  WBS leaf is created.)
- The **audio pipeline** (doc 12) reuses "temporal prefetch, playback hints"
  verbatim (`docs/design/11-time-and-video.md:261–263`,
  `docs/design/12-audio.md:169–174`): the `BlockKey` axis of
  `temporal_prefetch_ring` is already templated, so the same derivation applies
  to `arbc::audio-engine` when it lands.

## Inputs / context

### Design docs (normative — doc 16's executable-spec discipline)

- **`docs/design/11-time-and-video.md:160–166`** — the `playback_hint(direction,
  rate, horizon)` definition: optional, advisory, issued by the transport, so
  decoder-backed content can pre-roll sequentially. **This task lands the
  design-doc delta extending this bullet** (see Decisions D3) with the
  derivation semantics (direction = sign of rate; horizon = `|rate|` × runtime
  lookahead, exact rational; paused/zero-rate → empty hint; void/advisory
  signature).
- **`docs/design/11-time-and-video.md:185–196`** — the temporal prefetch ring:
  "upcoming times in playback direction … the playback-hint horizon bounds
  it," a distinct priority class, and the eviction order (victim-first)
  `speculative < recently-visible < temporal-prefetch < pan-prefetch < visible`.
- **`docs/design/11-time-and-video.md:88–115`** — the transport (per-viewport
  clock; rate/pause/loop; wall-clock-free advance) whose state is derived from.
- **`docs/design/17-internal-components.md:54,60`** — levelization: `arbc::cache`
  is L3 (deps `base, surface` only); `arbc::runtime` is L5 (deps everything
  below); the transport and the hint-issuing path live in `runtime`.
- **`docs/design/16-sdlc-and-quality.md:14–21`** (claims register),
  **`:54–62`** (behavioral-counter tests — "playback of a still scene issues
  zero visual renders … counters, never wall-clock"), **:66–73** (concurrency/
  TSan), **:112–118** (≥90% diff-coverage hard gate).

### Source seams this task extends (all current at `HEAD`)

- `src/contract/arbc/contract/content.hpp:210` — `class Content`. `playback_hint`
  is added here, grouped with the other null-defaulted optional members
  (`quantize_time` at `:249`, `render_thread_safe` at `:289`) whose default
  keeps every existing content byte-identical. `Stability::Timed`
  (`:26`) is the classification a participating decoder reports; `RenderResult`
  carries `achieved_time` (`:99`) and the provided-surface path (`:112`).
- `src/runtime/arbc/runtime/transport.hpp:34` — the producer (accessors above).
- `src/cache/arbc/cache/prefetch.hpp:97–135` — `temporal_prefetch_ring` (`:110`)
  and `prime_ring` (`:133`), with the header's levelization note (`:27–33`)
  pinning that direction/step/horizon arrive as plain `base` data.
- `src/cache/arbc/cache/keyed_store.hpp:26–32` — `PriorityClass` (with
  `Temporal`); `hits()`/`misses()`/`evictions()`/`resident_bytes()` are the
  behavioral counters the drive is asserted against.
- `src/runtime/arbc/runtime/interactive.hpp:97–101` — the live frame loop
  already threads a `TileCache&` and a `composition_time`; host_objects will
  add the derive+drive call here. `refinement.hpp:52–53` reserves the temporal
  ring for this runtime path.
- `src/runtime/CMakeLists.txt:8` — runtime `DEPENDS base model contract
  compositor pool`; this task adds `cache` (levelization-clean, L5→L3 already
  permitted by `scripts/check_levels.py:38–41`) so the drive may call the
  `cache::` prefetch templates directly.

### Predecessor / sibling conventions followed

- `tasks/refinements/timeline/transport.md` — the direct predecessor: the
  "vocabulary vs. policy" line (a clock is runtime policy, not `base`), the
  reuse of `TimeMap.evaluate` for exact rational rate-scaling with one
  ties-to-even rounding, and the "produces time/hints, not pixels → no byte-
  exact golden" convention.
- `tasks/refinements/cache/prefetch.md` — the consumer-side counterpart; owns
  `11-time-and-video#temporal-prefetch-ring-bounded-by-horizon` (enforced by
  `src/cache/t/prefetch.t.cpp`), which this task's drive keeps green as the
  first production feeder.
- `tasks/refinements/timeline/temporal_cache.md` — the in-repo test-double
  convention: `Timed` behavior is exercised with an in-repo double, never a
  real decoder/codec (that arrives with `kinds.imageseq_plugin`).

## Constraints / requirements

1. **Levelization (doc 17, CI-enforced by `scripts/check_levels.py`).** The
   derivation and drive are L5 `arbc::runtime` code (the transport lives there;
   the compositor refinement explicitly reserves the temporal ring for "the
   runtime transport path"). The `PlaybackHint` value and the
   `Content::playback_hint` method are L3 `arbc::contract` (the method is on the
   `Content` vtable). The cache (L3, deps `base, surface`) is never handed a
   `PlaybackHint`; the runtime **unpacks** it into `base`-typed
   `(direction, step, horizon)` scalars at the `temporal_prefetch_ring` call
   site. Adding `cache` to runtime's `DEPENDS` is the only build-graph change;
   it is already in the allowed set (`check_levels.py:38–41`), so **no new
   levelization edge and no design-doc delta to doc 17**.
2. **Advisory, correctness-neutral.** The hint changes no pixels and no cache
   correctness: `Content::playback_hint` is a null-defaulted no-op, so
   hint-ignoring content is byte-identical whether or not a hint is issued;
   priming is residency-only (reclassify resident, report absent, render/evict
   nothing — `prefetch.hpp:17–25`), so a drive leaves `resident_bytes()` and
   `evictions()` unchanged. Determinism stays owned by
   `quantize_time`/`achieved_time`, not by hints.
3. **Exact, wall-clock-free derivation.** The horizon is computed in exact
   rational arithmetic with a single ties-to-even leaf rounding (reusing the
   transport's `TimeMap`-based scaling); a pathological rate faults as a value
   (never wraps), matching the transport's advance contract. No `std::chrono`
   wall clock enters the derivation.
4. **Direction is derived, not stored.** `direction` = sign of `rate()`
   (`+1`/`-1`); a paused or zero-rate transport derives the *empty hint*
   (`direction 0`, `horizon 0`) — no pre-roll, an empty ring. This mirrors the
   transport's "a paused advance moves zero flicks."
5. **The drive takes `step` from the caller.** The `step` (native frame period /
   quantization grid of the visible `Timed` content) is supplied by the plan-
   aware caller (host_objects/compositor), exactly as `prefetch.hpp:29–33`
   already specifies — the runtime drive does not itself probe content cadence.
   A temporal ring is built only for `Timed` anchor keys (those carrying an
   `achieved_time`); `Static` scenes build no ring.
6. **≥90% diff coverage** on changed lines (`diff-cover --fail-under=90`),
   warning-free `clang-format` and `tj3 project.tjp`.

## Acceptance criteria

New claims registered in `tests/claims/registry.tsv` (stem `11-time-and-video`),
each referenced by a `// enforces: <claim-id>` comment above its `TEST_CASE`
(`scripts/check_claims.py` enforces both directions):

- **`11-time-and-video#playback-hint-derives-direction-from-rate-sign`** —
  `derive_playback_hint`: a positive rate yields `direction +1`, a negative
  rate `direction -1`; a paused transport and a zero rate both yield the empty
  hint (`direction 0`, `horizon 0`). Tier-2 exact-equality unit test in
  `src/runtime/t/playback_hints.t.cpp`.
- **`11-time-and-video#playback-hint-horizon-scales-with-rate`** — the horizon
  equals `|rate|` × the real-time lookahead, computed in exact rational
  arithmetic with one ties-to-even rounding; doubling the rate doubles the
  content-time horizon; reverse (`rate < 0`) gives the same magnitude as
  forward. Tier-2 unit test (same file).
- **`11-time-and-video#playback-hint-is-advisory-no-op-by-default`** — the
  default `Content::playback_hint` is a no-op: a scene of hint-ignoring content
  renders byte-identically and issues zero extra renders whether or not a hint
  is delivered. Contract-level test `src/contract/t/playback_hint.t.cpp`
  (default no-op + `PlaybackHint` value), reinforced by a behavioral-counter
  assertion in the runtime drive test (`requests_issued` unchanged).
- **`11-time-and-video#playback-prefetch-drives-temporal-ring`** —
  `drive_playback_prefetch` over a warm `TileCache` reclassifies exactly the
  resident temporal-neighbour tiles (`ring ∩ resident`) onto
  `PriorityClass::Temporal` and returns the absent members as the want-list,
  while `resident_bytes()` and `evictions()` are unchanged across the call.
  Tier-4 behavioral-counter test in `src/runtime/t/playback_hints.t.cpp` using
  an in-repo warm-cache fixture and a hint-recording `Content` double (no real
  decoder — deferred to `kinds.imageseq_plugin`).

Existing claim kept green (this task is its first production feeder):

- **`11-time-and-video#temporal-prefetch-ring-bounded-by-horizon`**
  (`src/cache/t/prefetch.t.cpp`, owned by `tasks/refinements/cache/prefetch.md`)
  — the drive test asserts the ring the drive builds is bounded by the derived
  horizon (`K = horizon / step` buckets, none reverse of direction), so the
  horizon bound now flows from a real transport-derived hint.

Other testing obligations:

- **No byte-exact golden** — this task produces advisory hints, not pixels
  (sibling convention). A change in rendered pixels caused by a hint would
  itself be a bug (the hint is advisory).
- **Still-scene behavioral counter** — a `Static`-only warm scene under a
  *playing* transport builds an empty temporal ring and issues zero temporal
  want-list entries and zero renders, reinforcing the existing still-scene
  promise (doc 16:60).
- **Concurrency (doc 16:66–73).** The hint is derived and issued single-threaded
  from the frame loop, and `prime_ring` reuses the cache's existing
  single-threaded reclassify contract — this task introduces **no new
  concurrent protocol** (no publish/pin or reclamation change), so it is covered
  by the full-suite TSan job and needs **no new dedicated stress harness**.
  Cross-thread delivery of decoded frames is the decoder's (imageseq's) concern.
- **≥90% diff coverage** on all changed lines.

Tests wired via `arbc_component_test(COMPONENT runtime …)` and
`arbc_component_test(COMPONENT contract …)` in the respective `CMakeLists.txt`.

No new WBS leaf is created: both downstream consumers already exist —
`kinds.imageseq_plugin` (`tasks/55-kinds.tji:61`) overrides the seam, and
`runtime.host_objects` (`tasks/65-runtime.tji:27`) is the live-loop call site.
The live-frame-loop integration (sourcing visible keys/step/content from the
plan and calling the drive each frame) lands with `runtime.host_objects`; this
task delivers the reusable `derive_playback_hint` + `drive_playback_prefetch`
unit it calls.

## Decisions

1. **`PlaybackHint` is a `contract`-level struct carrying `base` scalars;
   `Content::playback_hint` is a null-defaulted no-op returning `void`.**
   `struct PlaybackHint { int direction; base::Rational rate; base::Time
   horizon; };` bundles the doc's `(direction, rate, horizon)` triple; the
   method `virtual void playback_hint(const PlaybackHint&) {}` sits beside
   `quantize_time`/`render_thread_safe` as an opt-in optional. *Rejected:* three
   loose scalar parameters — a named struct is extensible (audio may add a
   quantum count) and matches how `RenderRequest` bundles inputs. *Rejected:*
   putting `PlaybackHint` in `base` — `base` is time/geometry *vocabulary*; a
   hint *issued to `Content`* is a contract concept, and the cache (which can't
   name contract types) receives unpacked `base` scalars anyway. *Rejected:* a
   non-void return (e.g. a "will pre-roll" acknowledgement) — the hint is
   advisory and solicits no answer; a `void` no-op default is the only signature
   that keeps every existing content byte-identical.
2. **`playback_hint` is non-const.** A decoder mutates internal pre-roll state
   when it receives a hint (it is precisely the `render_thread_safe() == false`
   stateful path). *Rejected:* `const` — would force decoders into `mutable`
   or a side table for no benefit; the render-purity contract (a pure function
   of the pinned snapshot) is unaffected because the hint feeds pre-roll, not
   the rendered pixels.
3. **The `(direction, rate, horizon)` triple is derived from the transport;
   the horizon is `|rate|` × a runtime real-time lookahead, scaled in exact
   rational arithmetic; paused/zero-rate → the empty hint.** This fills the gap
   doc 11 left on *how* the transport computes the triple, and lands as a
   **design-doc delta** at `docs/design/11-time-and-video.md:166` (same-commit
   rule, doc 16; sibling precedent — transport.md pinned advance semantics at
   `11:95–115`, rational_time pinned rounding at `11:46`). Direction = sign of
   `rate` (there is no stored direction); horizon is scaled so faster playback
   looks proportionally further ahead in *content* time, reusing the transport's
   `TimeMap.evaluate` with one ties-to-even leaf rounding. *Rejected:* a fixed
   content-time horizon independent of rate — 2× playback would under-prefetch
   by half. *Rejected:* a horizon expressed in "N frames ahead" — that needs the
   content's frame cadence, which the runtime does not own (the content owns its
   `quantize_time` grid); a real-time window scaled by rate is cadence-agnostic.
   *Rejected:* deriving a non-empty hint while paused — a paused transport
   consumes no upcoming frames, so the empty hint (empty ring, no pre-roll) is
   the correct, cheapest state and mirrors "a paused advance moves zero flicks."
4. **The drive takes the visible anchor key(s) and `step` from the caller and
   builds the ring only for `Timed` keys.** `drive_playback_prefetch` mirrors
   the cache's existing contract (`prefetch.hpp:29–33`): the plan-aware caller
   (host_objects/compositor) supplies the `base` `TileKey` and the native
   `step`; the drive unpacks the hint and calls `temporal_prefetch_ring` +
   `prime_ring(…, Temporal)`. *Rejected:* the drive probing `content.quantize_time`
   to recover `step` — the compositor already resolves the grid at plan time;
   re-probing in the runtime duplicates that work and couples the drive to grid
   internals it need not know.
5. **No new WBS leaf; integration lands with the existing `runtime.host_objects`
   and end-to-end observation with the existing `kinds.imageseq_plugin`.** Both
   consumers are already WBS leaves wired into their milestones; the reusable
   derive+drive unit is this task, their call/override is theirs. *Rejected:*
   registering a dedicated `runtime.playback_prefetch_wiring` leaf — it would
   duplicate scope host_objects already owns (threading the transport into the
   live loop is exactly where the derive+drive call belongs).

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-07.

- Added `PlaybackHint` struct (`{int direction; base::Rational rate; base::Time horizon}`) and null-defaulted `Content::playback_hint(const PlaybackHint&)` to `src/contract/arbc/contract/content.hpp`.
- Implemented `derive_playback_hint(const Transport&, Time real_lookahead)` and `drive_playback_prefetch(...)` in `src/runtime/arbc/runtime/playback_hints.hpp` + `src/runtime/playback_hints.cpp`.
- Unit tests in `src/runtime/t/playback_hints.t.cpp` (direction/horizon derivation + tier-4 behavioral counter for ring/still-scene/paused-empty).
- Contract test in `src/contract/t/playback_hint.t.cpp` (no-op default + `PlaybackHint` value).
- `src/runtime/CMakeLists.txt` updated: added source, header, test, and `+cache` dependency.
- `src/contract/CMakeLists.txt` updated: added contract test.
- `tests/claims/registry.tsv`: 4 new claims registered (`playback-hint-derives-direction-from-rate-sign`, `playback-hint-horizon-scales-with-rate`, `playback-hint-is-advisory-no-op-by-default`, `playback-prefetch-drives-temporal-ring`).
- `docs/design/11-time-and-video.md`: Decision D3 design-doc delta landed (derivation semantics for direction, horizon, paused/zero-rate empty hint).
