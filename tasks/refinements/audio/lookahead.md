# audio.lookahead — Lookahead scheduler

## TaskJuggler entry

Back-link: `tasks/45-audio.tji`, `task audio.lookahead` ("Lookahead
scheduler"). This refinement expands that one-line WBS leaf. The `.tji` note:
"Render-ahead ring of prepared blocks; workers run all plugin code; transport
changes flush and re-prime; audio never renders against a deadline. Doc 12."

## Effort estimate

`4d` (from the `.tji`) — the largest leaf in the audio stream. The mix core
(`mix_composition`), the per-content pull (`pull_audio`), the 1D block-cache
seam, and the templated temporal prefetch ring already exist; this task
stands up the **render-ahead scheduler** that turns those static pieces into
a running, deadline-free pipeline: a ring of prepared *mixed* output blocks
filled ahead of a playhead by off-thread workers, flushed and re-primed on
transport change, and invalidated by damage inside the lookahead window. It
spans two components (the L4 scheduler mechanism and the L5 worker pool +
headless pump that drives it), which — with the required TSan/stress coverage
— is where the 4 days go.

## Inherited dependencies

The parent `task audio` carries `depends contract.audio_facet,
timeline.transport`; `lookahead` adds `depends !mix_engine`, so it inherits
all three.

- **`audio.mix_engine`** — *settled* (DONE 2026-07-07,
  `tasks/refinements/audio/mix_engine.md`). Landed the two pieces this
  scheduler drives and fills, and explicitly deferred the ring, the fill, the
  damage re-mix, and the worker binding here:
  - The composition mixer this task calls once per prepared block —
    `mix_composition(const DocRoot&, ObjectId composition, const MixResolver&,
    PullService&, const AudioRequest&, MixPolicy = Flat) -> AudioResult`
    (`src/audio_engine/arbc/audio_engine/mix.hpp:59-61`), with
    `using MixResolver = std::function<Content*(ObjectId)>` (`:33`) and
    `enum class MixPolicy { Flat }` (`:41`). It pulls every layer through
    `pull.pull_audio(...)`, never `render_audio` inline (`mix.cpp:102`), and
    folds `achieved_rate = min` / `exact = conjunction` (`mix.cpp:222-236`).
  - The concrete `PullService::pull_audio` in `compositor`'s
    `PullServiceImpl` (`src/compositor/pull_service.cpp:261-338`) — cache-first
    probe on the `BlockKey` (`:301-303`), single dispatch onto the injected
    `AudioDispatch` seam (`:326,338`), single-settle, depth-budget backstop.
    Its two deliberate gaps are **this task's**: `PullConfig::blocks` is
    `nullptr` today so every pull misses (`pull_service.hpp:139`, comment "the
    prefetch-ring fill is `audio.lookahead`'s"), and `PullConfig::audio_dispatch`
    is unbound (`pull_service.hpp:135`) so misses settle the placeholder. The
    `audio_block_index` helper already floors a window to a block index
    (`pull_service.cpp:51`), but the note at `pull_service.hpp:98` reserves the
    **fixed block size and the prepared-block ring** for this task.
  - The mixer's own "no contributor settled inline → mix silence this pass,
    priming is `lookahead`'s job" placeholder path (`mix.cpp:106`) is what the
    ring exists to make never-taken under steady playback.
- **`contract.audio_facet`** — *settled* (DONE 2026-07-07). Supplies
  `AudioRequest`/`AudioResult`/`AudioCompletion` and the `PullService`
  interface the scheduler dispatches through; the stub comment already names
  "the L4 lookahead scheduler's pre-roll" as a future consumer
  (`src/contract/arbc/contract/content.hpp:277`).
- **`timeline.transport`** — *settled* (DONE 2026-07-07). This task first
  reads the transport for real: the scheduler primes ahead of `position()`
  in the sign of `rate()`, and a `seek`/`set_rate` is the transport change
  that flushes and re-primes (`src/runtime/arbc/runtime/transport.hpp:42,44,
  59,65,86`). The scheduler samples an immutable `Time` playhead; it does not
  drive the transport (device-clock mastering is `device_monitor`).

## What this task is

The render-ahead scheduler that makes audio *render ahead*, never on a
deadline (doc 12:31-34). Concretely, three deliverables across two
components:

1. **`arbc::audio-engine` (L4): the prepared-block ring + block pipeline**
   (doc 17:57 "lookahead scheduler, block pipeline"). A **pure, clock-free,
   thread-free** mechanism over a pinned `DocRoot`: a ring of prepared
   *mixed* output blocks keyed by output block index at the working rate,
   filled by calling `mix_composition` once per not-yet-prepared block window
   from the playhead out to an **injected horizon** (the lookahead budget,
   e.g. 100–500 ms → N blocks), and driving `cache::prime_ring<BlockKey>`
   over the `BlockCache` so each contributor's decode-behind blocks are
   classified `Temporal` and the absent ones reported. It exposes: `prime`
   (fill the ring ahead of a given playhead), `drain` (hand out a prepared
   block by index, or silence + an underrun count if not ready — **never**
   mixing inline), `reprime` (flush blocks no longer ahead of a new playhead
   and re-enumerate), and `invalidate` (drop prepared blocks and cached
   `BlockKey`s intersecting a damage time-range, doc 12:187-190). It renders
   nothing itself beyond calling its sibling `mix_composition`.

2. **`arbc::runtime` (L5): the audio worker pool** — the `AudioRequest`/
   `AudioCompletion` analog of the render-only `WorkerPool`
   (`src/runtime/arbc/runtime/worker_pool.hpp:78`). A shared FIFO + per-content
   serialization gate + condvar completion wake, `worker_count == 0`
   byte-identical inline mode, bound into `PullConfig::audio_dispatch` so that
   **all `render_audio` (arbitrary plugin code) runs off the consumer/callback
   thread** — the load-bearing promise of the note. Also wires
   `PullConfig::blocks` to a real `BlockCache` so primed blocks become
   zero-dispatch hits.

3. **`arbc::runtime` (L5): the headless lookahead pump** — a driver that owns
   the worker pool and a `BlockCache`, samples an **injected monotonic clock**
   (no wall clock, no device — the housekeeping-thread pattern), and each tick
   advances the playhead, calls the L4 ring's `prime` to keep the horizon
   full, and answers a consumer's `drain`. A transport `seek`/`set_rate`/
   direction change triggers `reprime`; a damage callback triggers
   `invalidate`. This is "audio renders ahead," fully testable with a fake
   clock and no audio device.

**Out of scope — each an existing named leaf** (see Acceptance criteria's
"Registers no successor"): the **device sink** and **device-clock mastering**
(the pump's injected clock becomes the device clock; video chases) →
`audio.device_monitor`; **declared-latency pre-roll** (requesting a content's
blocks earlier by `latency()`) → `audio.latency` — this task leaves the
pre-roll offset seam, honoring `Time::zero()` only; the **offline
sample-exact drive** (no ring, no realtime pressure) → `audio.export_monitor`
(which `depends !mix_engine`, not this task); **Spatial** pan/attenuation and
**RT-safety** enforcement → `audio.spatial_policy` / `audio.rt_safety`.

## Why it needs to be done

`audio.lookahead` is the gate to interactive audio: `device_monitor`
(`depends !lookahead`, `tasks/45-audio.tji:28`) and `latency`
(`depends !lookahead`, `:40`) — and `rt_safety` behind `device_monitor` —
cannot be built until a scheduler prepares mixed blocks ahead of the playhead
so the device callback consumes only ready, mixed data and never runs plugin
code (doc 12:155-164). Today the audio pipeline is static: `mix_composition`
mixes one window on the calling thread, and `pull_audio` misses every time
because `PullConfig::blocks`/`audio_dispatch` are unbound and no ring fills
them. Nothing renders ahead; a device callback driving `mix_composition`
directly would run third-party `render_audio` on the RT thread — exactly the
failure the whole engine design exists to prevent. This task closes both
mix-engine gaps, binds the workers, and lands the ring — making "audio never
renders against a deadline" real and giving `device_monitor` a thin adapter
(device I/O + clock) to sit on.

## Inputs / context

**Governing design doc — doc 12 (normative, doc 16):**
- **The asymmetry** (`docs/design/12-audio.md:31-34`) — "a late block is a
  glitch … audio never renders on a deadline, it renders *ahead*." The whole
  reason this scheduler exists.
- **The engine: monitors, clocking, lookahead** (`:150-190`), specifically:
  - **Device monitor + lookahead ring** (`:155-164`) — "renders **ahead of
    the playhead** into a ring of prepared blocks (lookahead budget, e.g.
    100–500 ms, configurable) … worker threads execute `render_audio` pulls
    and the mix graph *off* the device thread; the device callback only
    consumes prepared, mixed blocks. Arbitrary plugin code never runs on the
    audio callback — the price is lookahead latency on transport changes
    (play/seek flushes and re-primes the ring)."
  - **Prefetch and caching** (`:180-185`) — "block cache is the tile cache
    with 1D keys … with the temporal prefetch ring (doc 11) as its primary
    fill driver … Caching matters less than for pixels (audio is cheap to
    re-render) except for decode-behind-seek, which the hints and lookahead
    already cover."
  - **Damage** (`:187-190`) — "Audio damage is a time range in local time …
    it invalidates cached blocks and — if within the lookahead window —
    forces a re-mix of prepared blocks."
- **Sync and latency** (`:192-199`) — "v1 honors declared constant `latency()`
  in the lookahead scheduler (pre-roll requests earlier); full PDC …
  deferred." The pre-roll offset is the seam this task leaves for
  `audio.latency`.
- **Scheduling decision** (`:218-232`) — "device monitor + lookahead
  scheduler last," i.e. the ring is the last mechanism before the drivers.

**Doc 11 (exact rational time), doc 05 (recursion budgets):**
- Composed rates are exact rationals; the mixer already rounds once per leaf
  (`docs/design/11-time-and-video.md:187-188`) — the ring introduces no new
  rounding; it only chooses *which windows* to mix.
- Recursion terminates on the shared per-graph depth budget threaded through
  `pull_audio` (`docs/design/05-recursive-composition.md:61-67,93-100`) — the
  ring inherits it unchanged.

**Doc 17 levelization (CI-enforced):**
- `arbc::audio-engine` is **L4**, contents "pull-based mix, **lookahead
  scheduler, block pipeline**, clock mastering, latency pre-roll," deps
  "contract, cache (+ below)" (`docs/design/17-internal-components.md:28-30,
  57`). The scheduler *mechanism* lives here.
- **"A component may depend only on strictly lower levels. No same-level
  edges."** (`:41-44`) — audio-engine (L4) may not name `compositor` (L4) or
  `runtime` (L5); the pump and worker pool therefore live in `runtime`.
- `arbc::runtime` is **L5**, contents "viewport/transport/**monitor objects**,
  interactive frame loop, offline/export drivers … housekeeping thread" (`:60`).
- **"The two render drivers live in `runtime`, not the engines. The engines
  are libraries over pinned versions; deadlines, frame loops, and device
  clocks are runtime policy."** (`:84-86`) — this is the rule that splits the
  task: the clock-free ring is L4; the thread-owning, clock-sampling pump and
  the worker pool are L5.

**Code seams the implementation extends:**
- **The mix core to drive** — `mix_composition`
  (`src/audio_engine/arbc/audio_engine/mix.hpp:59-61`; the ring calls it once
  per prepared block), its silent placeholder path (`mix.cpp:106`).
- **The pull gaps to close** — `PullConfig::blocks{nullptr}`
  (`src/compositor/arbc/compositor/pull_service.hpp:139`) and
  `PullConfig::audio_dispatch{}` (`:135`); `AudioDispatch =
  std::function<void(Content*, const AudioRequest&,
  std::shared_ptr<AudioCompletion>)>` (`:63-64`) with the inline
  `direct_audio_dispatch()` (`:72`); `AudioBlockValue`/`BlockCache =
  KeyedStore<BlockKey, AudioBlockValue>` (`:82-93`); `audio_block_index`
  (`pull_service.cpp:51`) and its reserved-for-lookahead note (`:98`); the
  cache-probe + single-dispatch body (`pull_service.cpp:290-338`);
  `note_audio_dispatch` counter (`src/compositor/arbc/compositor/counters.hpp`).
- **The block cache key** — `struct BlockKey { ObjectId content; uint64_t
  revision; int64_t block_index; uint32_t rate; }`
  (`src/cache/arbc/cache/key_shapes.hpp:83-90`; layout deliberately *not* a
  key field, `:81-82`); `KeyedStore` is the shared machinery
  (`registry.tsv:67`).
- **The fill driver to reuse** — the templated temporal ring, already
  `BlockKey`-ready: `temporal_prefetch_ring<Key>(const Key&, int direction,
  Time step, Time horizon)` (`src/cache/arbc/cache/prefetch.hpp:109-110`) and
  `prime_ring<Key,Value>(KeyedStore<Key,Value>&, std::span<const Key>,
  PriorityClass)` (`:132-134`, classifies residents `Temporal`, returns the
  absent want-list, **renders/inserts nothing**), with the `BlockKey`
  temporal-step overload advancing `block_index` (`:44-57`). The visual
  precedent that drives these against a transport is
  `src/runtime/playback_hints.cpp:8,31,63` (`prime_ring(cache, …, TileKey …,
  Temporal)`) — the audio pump mirrors it for `BlockKey`.
- **The worker-pool precedent to mirror** — `WorkerPool`
  (`src/runtime/arbc/runtime/worker_pool.hpp:78`), `WorkerPoolConfig` with
  `worker_count = 0` degenerate inline default (`:57`), `RenderTask`
  (`:50-54`), `submit`/`poke`/`wait_completions`/`request_stop` (`:96-113`),
  the `RenderDispatch` seam bound over `submit` (`:41-42`), counters
  `tasks_submitted`/`tasks_completed`/`max_in_flight_per_content` (`:116-128`).
  It carries `RenderTask` only — there is **no** audio task variant today, so
  this task adds the audio sibling (see Decisions).
- **The owned-background-thread precedent** — the housekeeping thread's
  park/wake/stop/join on a condvar with an **injectable monotonic
  `tick_source`** (no wall clock in tests) and a `flush()` counter for test
  synchronization (`tasks/refinements/runtime/housekeeping_thread.md`); the
  pump follows it so it is testable without a device.
- **Transport** — `Transport::position()`/`rate()`/`is_paused()`/`loop()`
  (`src/runtime/arbc/runtime/transport.hpp:42-46`), `seek`/`set_rate`
  (`:59,65`), `advance` (`:86`). The scheduler reads; it does not master.
- **Model / base** — `DocRoot::working_audio_format()`
  (`src/model/arbc/model/model.hpp:64-69`); `Time`/`flicks_per_second`,
  `TimeRange` half-open (`src/base/arbc/base/time.hpp:11-20,29-54`).
- **Existing audio claims** to extend, not duplicate:
  `tests/claims/registry.tsv:67-75` (block cache 1D, working format, tone
  rate, nested-mixes-through-pull, nested metadata/boundary, mix-engine
  additive/facetless, `pull_audio` cache-first-single-settle) and `:149-150`
  (facet optional / consistent). Line **26**
  (`14-data-model-and-editing#structural-damage-spans-all-time`) already
  names "the audio lookahead window" — the damage claim here is its temporal
  consumer, not a duplicate.

## Constraints / requirements

1. **The ring is `arbc::audio-engine` (L4): pure, clock-free, thread-free.**
   Add it to `src/audio_engine/` (public header `arbc/audio_engine/lookahead.hpp`
   + impl) as a peer of `mix.hpp`. Its `prime`/`drain`/`reprime`/`invalidate`
   take `Time` playhead and `Time` horizon **by value** — no wall clock, no
   `Transport`, no thread. Deps stay ⊆ {contract, cache, model, media, base};
   it may **not** name `compositor` or `runtime` (doc 17:41). It reuses
   `cache::prime_ring`/`temporal_prefetch_ring` (L2) and calls its sibling
   `mix_composition` (same component). CI levelization (doc 17:41-44) must
   stay green.
2. **The pump and the audio worker pool are `arbc::runtime` (L5).** Owning a
   thread, sampling a clock, holding a device — all runtime policy
   (doc 17:84-86). The pump follows the housekeeping-thread template: an
   injectable monotonic `tick_source` (fake clock in tests), park/wake/stop/
   join, a `flush()` test barrier. The audio worker pool mirrors `WorkerPool`
   (shared FIFO, per-content serialization gate, condvar wake,
   `worker_count == 0` inline mode).
3. **Close both mix-engine pull gaps.** The pump constructs a real
   `BlockCache` and binds `PullConfig::blocks` to it, and binds
   `PullConfig::audio_dispatch` to the audio worker pool's submit. After the
   ring primes a window's contributors, subsequent `pull_audio` for those
   `BlockKey`s must hit with **zero** dispatch. Existing `pull`/`pull_audio`
   behavior and every current `PullService` caller stay byte-identical when
   the seams are left unbound (`blocks == nullptr` / empty dispatch preserves
   today's placeholder path).
4. **Fill is driven by the temporal prefetch ring.** Per-content
   decode-behind blocks are enumerated with `temporal_prefetch_ring<BlockKey>`
   in the sign of `Transport::rate()` and classified via `prime_ring(…,
   PriorityClass::Temporal)` — the `BlockKey` mirror of `playback_hints.cpp`.
   The scheduler renders/inserts nothing through the ring helpers themselves
   (they only classify/report); mixing happens through `mix_composition` into
   the prepared-output ring, and per-content blocks land in the cache via the
   worker path.
5. **The drain path never mixes inline.** `drain(block_index)` returns the
   prepared, already-mixed block if ready, else a **silence** block plus an
   incremented underrun counter — it must **never** call `mix_composition` or
   `render_audio`, allocate, or block. This is the RT-safety invariant the
   whole design buys (doc 12:31-34,155-164); it is asserted by a
   behavioral-counter test, and the drain path is the surface `audio.rt_safety`
   later annotates.
6. **Transport change flushes and re-primes.** A `seek`, `set_rate`, or
   direction (rate-sign) change invokes `reprime(new_playhead)`: prepared
   blocks whose output window is no longer within `[playhead, playhead+horizon)`
   in the new direction are dropped; blocks still valid (identical output
   window and un-damaged) are **retained, not re-mixed**; the want-list is
   re-enumerated from the new playhead (doc 12:162-164). Retention is
   observable (a re-mix counter must show only newly-needed blocks mixed).
7. **Damage inside the lookahead window forces a re-mix.** `invalidate(range)`
   takes a local-time damage range (doc 12:187-190,
   `14-data-model-and-editing#structural-damage-spans-all-time`): it drops
   prepared output blocks and cached `BlockKey`s intersecting `range`, then
   the next `prime` re-mixes exactly those. Non-overlapping prepared blocks
   are retained. Aggregate revision covers audio damage since it is the same
   revision space (doc 12:203-208) — no new revision machinery.
8. **Determinism.** Draining a fully-primed ring in order yields blocks
   **bit-identical** to calling `mix_composition` directly for each output
   window (the ring is a scheduler, not a signal transform); no wall clock or
   thread interleaving may perturb sample values. Threaded (`worker_count > 0`)
   and inline (`worker_count == 0`) fills produce identical samples.
9. **Latency honored as `Time::zero()` only; leave the pre-roll seam.** The
   scheduler reads each content's `latency()` through the existing facet API
   but this task pre-rolls by zero (default). Structure the per-contributor
   request-time computation so `audio.latency` inserts the `-latency()` offset
   additively — no signature churn later. Do **not** implement dynamic PDC
   (doc 12:196-199).
10. **No design-doc delta.** doc 12 specifies the ring/flush/damage/latency
    behavior; doc 17 already lists "lookahead scheduler, block pipeline" under
    audio-engine (L4) and "monitor objects / frame loop / drivers" under
    runtime (L5), and `threading.md` already anticipated the audio worker pool
    ("reuse this same `WorkerPool` or an analogous one against
    `AudioRequest`"). The two reserved-for-lookahead comments
    (`pull_service.hpp:98,138`, `pull_service.cpp:323`) are prose the
    implementer refines in place to point at the shipped ring/pump — a
    same-commit code-comment touch-up, **not** a design change. A genuine
    architectural gap is an escalation (return summary → parking lot), not a
    silent delta.
11. **Concurrency coverage is mandatory (doc 16).** The audio engine is
    explicitly called out for TSan/stress; this task owns the audio worker
    pool and the ring under concurrent fill + drain, so it scopes that
    coverage (see Acceptance criteria).
12. **Diff coverage ≥ 90%** on changed lines (doc 16 CI gate); tests ship in
    this task.

## Acceptance criteria

- **Claims-register growth** (`tests/claims/registry.tsv`, `12-audio#…`):
  1. `12-audio#lookahead-prepares-ahead-of-playhead` — "The lookahead
     scheduler keeps a ring of prepared *mixed* blocks covering
     `[playhead, playhead+horizon)`, each produced by `mix_composition` on a
     worker; `drain` hands out a prepared block when ready and silence + an
     underrun count otherwise, and **never** invokes `mix_composition`/
     `render_audio` on the drain thread; a fully-primed ring drained in order
     is byte-identical to `mix_composition` called directly per output
     window." Enforced by an `enforces:`-tagged **byte-exact golden** +
     behavioral-counter test in `src/audio_engine/t/` (and the pump test in
     `src/runtime/t/`).
  2. `12-audio#lookahead-transport-change-flushes-and-reprimes` — "A
     `seek`/`set_rate`/direction change flushes prepared blocks no longer
     ahead of the new playhead and re-enumerates the want-list; blocks whose
     output window is unchanged and un-damaged are retained, not re-mixed —
     the re-mix count equals only the count of newly-needed blocks." Enforced
     by a behavioral-counter test.
  3. `12-audio#lookahead-fills-block-cache-through-prefetch-ring` — "Priming
     classifies each contributor's decode-behind blocks onto the temporal
     prefetch ring (`prime_ring`, `PriorityClass::Temporal`, in playback
     direction) and — via the bound `PullConfig::blocks`/`audio_dispatch` —
     populates the `BlockCache`, so a subsequent `pull_audio` for those
     `BlockKey`s hits with **zero** dispatch (closing the mix-engine no-fill
     gap)." Enforced by a behavioral-counter test on the audio-dispatch
     counter across a prime→pull sequence.
  4. `12-audio#lookahead-damage-remixes-prepared-blocks` — "A damage
     time-range intersecting the lookahead window invalidates exactly the
     prepared output blocks and cached `BlockKey`s it overlaps and forces
     their re-mix on the next prime; non-overlapping prepared blocks are
     retained (doc 12:187-190)." Enforced by a behavioral-counter + golden
     test; references `14-data-model-and-editing#structural-damage-spans-all-time`
     (`registry.tsv:26`) as the damage-range source, not a duplicate.
- **Byte-exact goldens** (deterministic; no tolerances, doc 16):
  - A ring primed over a multi-tone / nested-of-tones composition, drained in
    order, equals the same windows mixed directly by `mix_composition` —
    byte-identical, and identical between `worker_count == 0` and
    `worker_count > 0`.
  - After a mid-window `seek`, the re-primed ring's blocks equal a fresh
    direct mix from the new playhead — byte-identical.
- **Behavioral-counter assertions** (performance-shaped promises, never
  wall-clock, doc 16):
  - Over a full playback pass, the count of `mix_composition`/`render_audio`
    invocations **on the drain/consumer thread is 0** ("workers run all plugin
    code").
  - `reprime` after a seek mixes exactly the newly-needed blocks (retained
    blocks re-mix count 0); `invalidate` re-mixes exactly the overlapped
    blocks.
  - Post-prime `pull_audio` for primed `BlockKey`s issues **0** dispatches.
- **Concurrency / TSan** (doc 16 requires it for the audio engine
  explicitly): a TSan/stress test running the audio worker pool + ring under
  concurrent fill (multiple workers mixing distinct output blocks, pulling
  same and distinct `BlockKey`s) and drain, asserting each `AudioCompletion`
  settles exactly once, the ring and `BlockCache` see no data race, and the
  drained samples equal the inline-mode goldens — the audio twin of the
  render `WorkerPool` TSan case and `tests/pull_audio_concurrency.t.cpp`. New
  test target wired in `tests/CMakeLists.txt`.
- **No new conformance family.** The scheduler is engine/runtime machinery,
  not a content kind or operator, so it adds no `arbc-testing` family; its
  tests drive `org.arbc.tone`/`org.arbc.nested` through the ring and pump.
- **WBS gate.** After the closer adds `complete 100` and the `Refinement:`
  back-link to `tasks/45-audio.tji`, `tj3 project.tjp 2>&1 | grep -iE
  "error|warning"` is silent.
- **Registers no successor.** Every piece of implementable work this task
  does not do already maps to a named WBS leaf under `task audio` (milestone:
  the audio milestone in `tasks/99-milestones.tji`): the device sink +
  clock mastering (device clock replaces the pump's injected clock) →
  `audio.device_monitor`; declared-latency pre-roll behind the seam left in
  Constraint 9 → `audio.latency`; the offline sample-exact drive →
  `audio.export_monitor`; Spatial policy → `audio.spatial_policy`; RT-safety
  annotation of the drain path → `audio.rt_safety`. This task creates no new
  leaf.

## Decisions

- **The task splits L4 (ring) from L5 (pump + worker pool), mirroring
  `mix_engine`'s audio-engine/compositor split and the render side's
  engine/runtime split.** doc 17:84-86 is explicit: engines are clock-free
  libraries over pinned versions; deadlines, loops, threads, and device clocks
  are runtime. The pure ring/block-pipeline mechanism is doc-17:57's
  "lookahead scheduler, block pipeline" in audio-engine (L4); the thread that
  pumps it and the worker pool that runs plugin code are runtime (L5, "monitor
  objects / frame loop"). This is the same shape as `compositor::render_frame`
  (L4) vs `WorkerPool` + the interactive frame loop (L5), and
  `cache::prime_ring` (L2) vs `playback_hints` (L5). *Rejected:* putting the
  whole scheduler in audio-engine (L4 cannot own threads/clocks per doc
  17:84-86 and cannot name `runtime`); putting it all in runtime (abandons the
  doc-17:57 placement of the scheduler mechanism in audio-engine and would
  make the ring untestable as a pure unit).
- **The audio worker pool is a sibling of `WorkerPool`, not a templatization
  of it.** `threading.md` already anticipated "reuse this same `WorkerPool` or
  an analogous one against `AudioRequest`." `WorkerPool` carries `RenderTask`
  (`RenderRequest`/`RenderCompletion`) only; the audio pool carries the audio
  task (`AudioRequest`/`AudioCompletion`). A sibling that mirrors the shared
  FIFO + per-content gate + condvar wake keeps the TSan-verified render pool
  byte-identical and untouched; the duplicated shell is thin, exactly as
  `mix_engine` accepted duplicating the per-layer walk between the engine and
  nested. *Rejected:* templatizing `WorkerPool` over task type now — it churns
  verified render-path code and its callers for a two-consumer dedup, and per
  refinement policy a pure "refactor to share" is not a WBS-deferrable task.
  If a third task-typed consumer ever appears, that is the moment to
  generalize.
- **The ring holds prepared *mixed output* blocks; the block cache holds
  *per-content* blocks.** These are two distinct layers: `mix_composition`
  reads per-content blocks (the `BlockCache`, filled behind `pull_audio`) and
  writes one mixed block into the output ring. The prefetch ring
  (`temporal_prefetch_ring`/`prime_ring`) drives the *per-content* fill; the
  output ring is the scheduler's own bounded buffer. Conflating them would
  either re-mix on every drain (defeating lookahead) or cache un-keyable mixed
  output (the mix depends on the whole composition, not one content). *Rejected:*
  caching mixed output in the `BlockKey` store — mixed blocks have no single
  content id/revision to key on.
- **The drain path is pure-consume: prepared block or silence, never inline
  mix.** doc 12:31-34,155-164 — plugin code never runs on the consumer/RT
  thread. An underrun returns silence + a counter, not a synchronous mix.
  *Rejected:* a synchronous "mix now if not ready" fallback on drain — it
  would put `render_audio` on the callback thread, forfeiting the RT-safety
  the engine exists to buy; the correct response to chronic underrun is a
  larger horizon or more workers, a device_monitor tuning concern.
- **Fill reuses `cache::prime_ring`/`temporal_prefetch_ring` for `BlockKey`,
  mirroring `playback_hints.cpp` for tiles.** The templates are already
  `BlockKey`-ready (`prefetch.hpp:44-57,109-134`) precisely so audio reuses
  them (per `cache/prefetch.md`). *Rejected:* a bespoke audio prefetch
  enumerator — it would duplicate a helper written to be shared and diverge
  from the visual path's proven classify-resident/report-absent contract.
- **Retain-on-reprime; re-mix only invalidated blocks.** A seek that still
  overlaps prepared blocks keeps them; only genuinely-new or damaged windows
  re-mix (doc 12:162-164,187-190). *Rejected:* flush-everything on any
  transport touch — simpler but wasteful, and it would make a small nudge as
  expensive as a full seek, undermining scrubbing (doc 12:120-122).
- **Latency honored as zero, seam left for `audio.latency`.** doc 12:196-199
  scopes v1 to constant declared latency and defers PDC; the separate
  `audio.latency` leaf (`depends !lookahead`) owns wiring the pre-roll offset.
  This task lands the request-time computation so the offset drops in
  additively, honoring `Time::zero()` today. *Rejected:* implementing the
  pre-roll here (it is a whole named leaf with its own effort) or hard-coding
  no offset with no seam (forces a signature change in `audio.latency`).

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-08.

- L4 `LookaheadRing` (clock-free, thread-free) landed in `src/audio_engine/arbc/audio_engine/lookahead.hpp` + `src/audio_engine/lookahead.cpp`, with unit tests in `src/audio_engine/t/lookahead.t.cpp`; closes the `mix_composition`-per-block fill and the `drain`-never-mixes invariant.
- L5 `AudioWorkerPool` (sibling of `WorkerPool`, `worker_count==0` inline mode, per-content serialization gate) landed in `src/runtime/arbc/runtime/audio_worker_pool.hpp` + `src/runtime/audio_worker_pool.cpp`, with tests in `src/runtime/t/audio_worker_pool.t.cpp`.
- L5 `LookaheadPump` (housekeeping-thread pattern, injectable `tick_source`, `flush()` test barrier) landed in `src/runtime/arbc/runtime/lookahead_pump.hpp` + `src/runtime/lookahead_pump.cpp`, with tests in `src/runtime/t/lookahead_pump.t.cpp`.
- TSan/stress concurrency test covering concurrent fill + drain added in `tests/audio_lookahead_concurrency.t.cpp`; wired into `tests/CMakeLists.txt`.
- Both `PullConfig` gaps closed: `PullConfig::blocks` bound to a real `BlockCache`; `PullConfig::audio_dispatch` bound to the worker pool's submit — subsequent `pull_audio` for primed `BlockKey`s hits with 0 dispatch.
- `src/compositor/arbc/compositor/pull_service.hpp` and `src/compositor/pull_service.cpp` updated (comment-only) to point reserved-for-lookahead notes at the shipped ring/pump.
- 4 new claims registered in `tests/claims/registry.tsv`: `12-audio#lookahead-prepares-ahead-of-playhead`, `#lookahead-transport-change-flushes-and-reprimes`, `#lookahead-fills-block-cache-through-prefetch-ring`, `#lookahead-damage-remixes-prepared-blocks`; all enforced (151 total).
- `AudioBlockValue`/`BlockCache` live in `compositor` (L4 peer); the ring's cache-touching methods (`prime`/`reprime`/`invalidate`) are templated on the block-value type — the runtime pump instantiates them with `compositor::AudioBlockValue`; the ring names only `cache::BlockKey`/`KeyedStore`. Levelization stays green.
- Deferred: threaded recursive pre-fill for nested + below-rate native contributors under `worker_count>0` — registered as `audio.lookahead_recursive_prefetch`.
