# pool.stats_counter_race — Publish SlotStore accounting counters atomically

## TaskJuggler entry

[`tasks/05-pool.tji:119-124`](../../05-pool.tji) — `task stats_counter_race
"Publish SlotStore accounting counters atomically"`, `effort 1d`,
`depends !arena_core, runtime.housekeeping`. Wired into M9
(`tasks/99-milestones.tji:72`).

The note names the race precisely: `Housekeeper::stats()` reads
`d_target->bytes_reserved()` (`src/runtime/housekeeping.cpp:81`), which bottoms
out in `SlotStore::d_bytes_reserved` / `d_slots_capacity` — plain
`std::size_t` (`src/pool/arbc/pool/slot_store.hpp:329-330`) written by the
writer's grow path with no housekeeping mutex
(`src/pool/slot_store.cpp:225-226`). `HousekeepingThread::stats()` takes
`d_mutex`, but the writer's `allocate` does not, **so the mutex serializes
nothing here**.

## Effort estimate

**1d.** The counter change is mechanical (three members, four write sites, three
accessors). The day is spent on the *second* hole this refinement surfaces (the
arena store-map walk, below) and on building a TSan test that actually overlaps
a stats poll with a *growing* writer — the thing no existing test does.

## Inherited dependencies

**Settled:**

- **`pool.arena_core`** (`tasks/refinements/pool/arena_core.md`, Done
  2026-07-04) — introduced the per-arena accounting this task hardens ("accounting
  (bytes reserved, slots live/capacity per store, arena aggregate) tracks
  alloc/release exactly") and settled that **allocation is writer-thread-only**,
  with readers lock-free. Its concurrency contract covers *index resolution*
  ("readers do two acquire-loads and an add — no locks") and says nothing about
  who may read the accounting. That silence is the hole.
- **`runtime.housekeeping`** (`tasks/refinements/runtime/housekeeping.md`, Done
  2026-07-05) — built `HousekeepingStats` and declared "**Observability
  aggregates existing counters into a runtime `HousekeepingStats`; no new
  counting mechanism**", rejecting "threading new atomics through the pool
  layer". It also asserted "Single-threaded, no concurrency surface — no TSan
  obligation." **This task narrowly invalidates that last sentence for
  `stats()`** — not by adding counters (the rejection stands: no new counting
  mechanism here) but by making the *existing* ones legal to read from the
  thread that the design already points at them.
- **`pool.free_pools`** (`tasks/refinements/pool/free_pools.md`) — the governing
  precedent: "`d_slots_live` and `d_release_fence` become atomic …
  `std::atomic<std::size_t>` (**relaxed** inc/dec)… *Rejected:* a mutex around
  the counter", and "`free_slots()` becomes **best-effort** … it is a
  **diagnostic counter, not an invariant source**."
- **`runtime.housekeeping_thread`** — established `d_mutex`
  (`src/runtime/arbc/runtime/housekeeping_thread.hpp:127`) as the *single-drainer*
  serializer over housekeeping entry points. Its scope was never the writer's
  allocate path, and this task must not extend it there.

**Pending:** none. Both `depends` are complete.

## What this task is

Make the `SlotStore`/`Arena` accounting readable from a non-writer thread
without undefined behavior, and prove it with a TSan test that polls
`Housekeeper::stats()` while a writer allocates through a growing arena.

Two distinct hazards sit on that one call path:

1. **The counter tear (named in the `.tji`).**
   `d_slots_capacity` / `d_bytes_reserved` (`slot_store.hpp:329-330`) and
   `d_high_water` (`:326`) are plain scalars, read-modify-written by the writer
   in `allocate()` (`slot_store.cpp:225-226`, `:229`) and in
   `reserve_restored()` (`:286-287`, `:290`) with no lock, while
   `bytes_reserved()` (`slot_store.hpp:258`), `slots_capacity()` (`:250`) and
   `high_water()` (`:193`) plain-load them from whatever thread calls
   `stats()`. Benign on the target ISAs; UB by the language; TSan-visible.

2. **The store-map walk (NOT named in the `.tji` — surfaced by this
   refinement).** The aggregators the stats path actually calls —
   `Arena::total_bytes_reserved()`, `total_slots_live()`, `total_high_water()`
   (`slot_store.cpp:361-383`), plus `store_count()` (`slot_store.hpp:369`) —
   iterate `d_stores`, a `std::map` (`slot_store.hpp:386`), while the writer
   *inserts* into it from `Arena::store_for()` (`slot_store.cpp:340-357`). The
   node addresses are stable (it is a `std::map`, not an `unordered_map`, so the
   returned `SlotStore&` survives), but a red-black insert **rewrites the link
   and colour words a concurrent traversal is reading**. This is strictly worse
   than the counter tear — it is not a word-sized load that happens not to tear,
   it is a tree walk over links being rebalanced — and **atomicizing the counters
   does not fix it.** Note it already makes `total_slots_live()` racy *today*
   even though `d_slots_live` has been atomic since `pool.free_pools`.

Both are in scope. Closing (1) alone would leave the task's own acceptance test
("poll `stats()` while a writer allocates through a **growing** arena") dirty
under TSan the moment the writer mints a store.

**Not this task:** the `Checkpointer`-side counters that `stats()` also reads —
`slots_freed_to_list()` / `durable_epoch()` (`housekeeping.cpp:82-83` →
`src/pool/arbc/pool/checkpoint.hpp:323-325`, plain members at `:418-425`) and
`DurabilityEpochFence::d_quarantined_total` (`checkpoint.hpp:93`). Those are the
latent races already scoped to **`runtime.background_checkpoint_quiesce`**: they
need `Checkpointer::commit` to run concurrently with an allocator or releaser,
and no shipped configuration does that (`tasks/99-milestones.tji:79` —
`Document::policy_for` never sets `checkpoint_tick_interval`, so every commit
originates on the writer thread under the `HousekeepingThread` mutex, which
`stats()` also takes). This task is the one race that is **live now**; it stays
split out and must not silently absorb the latent four. Likewise out of scope:
`WorkspaceFile::d_chunk_count` / `d_live` (`workspace_file.hpp:461-464`), which
no cross-thread reader reaches — they are not on the `HousekeepingStats` path.

## Why it needs to be done

This is a live data race in the shipped configuration, not a latent one. The
realistic call pattern is the one doc 15 explicitly designs for: an editor that
displays memory/arena usage while the user paints. `Document::memory_stats()`
(`src/runtime/document.cpp:133`) is the public host surface; it delegates to
`HousekeepingThread::stats()` (`src/runtime/housekeeping_thread.cpp:81-84`),
whose `d_mutex` serializes the *housekeeper* against the *background loop* and
nothing else. A host thread polling it once a frame while the writer thread
grows an arena is a genuine race today.

It has not gone red in CI only by omission. `tests/stress_document_housekeeping.t.cpp`
*does* run a writer, a background `HousekeepingThread`, and pinner threads under
the per-push `gcc-tsan` lane — but its `doc->memory_stats()` calls
(`:234`, `:257`, `:259`) all happen **after** `stop.store(true)` and
`th.join()` (`:222-225`). The poll never overlaps the writer. That is the entire
reason the lane is green, and it is why the fix must ship with a test that
overlaps them on purpose.

Downstream, doc 15 promises the accounting *to the host* ("per-arena live counts
and byte accounting exposed through the API (hosts *will* want a memory panel)",
`docs/design/15-memory-model.md:179-184`) and leans on it for memory-pressure
policy ("memory pressure maps to 'trim journal tail, shrink caches' — both
existing knobs, now with per-arena accounting to drive them", `:129-133`). An
exposed API a host cannot legally call from its own thread is not exposed.

## Inputs / context

- **`src/pool/arbc/pool/slot_store.hpp:326-332`** — the member block. `d_high_water`
  (`:326`, plain `SlotIndex`), `d_slots_live` (`:328`, **already**
  `std::atomic<std::size_t>`), `d_slots_capacity` (`:329`, plain),
  `d_bytes_reserved` (`:330`, plain), `d_spill_count`/`d_refill_count` (`:331-332`,
  already atomic). The three plain ones are the targets.
- **`src/pool/arbc/pool/slot_store.hpp:193, 249-258`** — the accessors, all
  `const noexcept`: `high_water()` (`:193`, plain load), `slots_live()` (`:249`,
  **`load(relaxed)` — the shape to copy**), `slots_capacity()` (`:250`, plain
  load), `free_slots()` (`:254-257`, **takes `d_pool_mutex` inside a `const
  noexcept` accessor — the in-house precedent for locking in one**),
  `bytes_reserved()` (`:258`, plain load).
- **`src/pool/arbc/pool/slot_store.hpp:112-126`** — the header's own threading
  contract, which states the hole in prose: "arena growth — chunk publish, column
  publish, **capacity accounting** — is single-threaded and unsynchronized". This
  comment is now wrong about the accounting and must be amended by the
  implementation.
- **`src/pool/slot_store.cpp:190-230`** — `allocate()`. `assert_writer_thread()`
  at `:191`; the grow branch at `:212-227` does `d_slots_capacity += d_chunk_slots`
  (`:225`) and `d_bytes_reserved += span->size` (`:226`); `++d_high_water` at
  `:229`. No lock on any of it.
- **`src/pool/slot_store.cpp:267, 286-292, 313`** — `reserve_restored()` /
  `finalize_restore()`, the recovery-path writers of the same three counters
  (writer-thread-only).
- **`src/pool/slot_store.cpp:333-359`** — `Arena::store_for()`. `d_stores.find`
  (`:340`) then `d_stores.emplace` (`:353-356`) — the writer-side mutation of the
  map the aggregators walk. Callers: `TypedStore`'s constructor
  (`src/pool/arbc/pool/typed_store.hpp:28`), `BigBlockPool::allocate`
  (`src/pool/big_block_pool.cpp:19`), and the restore path
  (`src/pool/arbc/pool/checkpoint.hpp:297`). **All cold** — per typed-view
  construction and per page-sized blob, never per slot.
- **`src/pool/slot_store.cpp:361-383`** — `total_slots_live()`,
  `total_high_water()`, `total_bytes_reserved()`: the three unguarded walks over
  `d_stores`. Plus `store_count()` (`slot_store.hpp:369`) and the loop at
  `slot_store.hpp:376`.
- **`src/pool/arbc/pool/slot_store.hpp:386`** — `std::map<std::pair<size_t,
  size_t>, std::unique_ptr<SlotStore>> d_stores`. A `std::map`, so node/reference
  stability holds across inserts — which is what makes a lock around
  `store_for` + the aggregators sufficient (the returned `SlotStore&` stays valid
  after the lock drops).
- **`src/runtime/housekeeping.cpp:74-85`** — `Housekeeper::stats()`, `const
  noexcept`, **no mutex of its own**; `:81` is the `bytes_reserved()` call the
  `.tji` names.
- **`src/runtime/arbc/runtime/housekeeping.hpp:106-115`** — `HousekeepingStats`;
  `live_slots` and `bytes_reserved` are the two pool-sourced fields.
  `src/runtime/arbc/runtime/housekeeping_targets.hpp:41, 82-86` — both target
  impls bottom out in `Arena::total_slots_live()` / `total_bytes_reserved()`.
- **`src/runtime/housekeeping_thread.cpp:81-84`** — `stats()` under `d_mutex`;
  `housekeeping_thread.hpp:135` — `d_background_ticks`, "Atomic so the `noexcept`
  accessor reads it without locking": the runtime layer already reached for
  exactly this fix.
- **`tests/stress_document_housekeeping.t.cpp:222-234`** — the existing TSan
  stress, and the proof it cannot catch this: every `memory_stats()` call is
  post-join.
- **`docs/design/15-memory-model.md:152-158`** — thread rules ("the writer thread
  is the only structural allocator"); **`:179-195`** — the Debug-discipline
  paragraph plus **this task's delta** (below), which states the cross-thread read
  contract; **`:129-133`** — accounting drives memory-pressure policy.
- **`docs/design/16-sdlc-and-quality.md:66-73`** — the concurrency tier ("TSan on
  the full suite; dedicated stress tests…"); **`:54-62`** — behavioral counters,
  never wall-clock.
- **`tests/claims/registry.tsv:112-119`** — the housekeeping claim block;
  `:114` `15-memory-model#housekeeping-reports-memory-panel-stats` is the
  single-threaded neighbour this task's new claim completes.
- **`CMakePresets.json:33-38`** (the `tsan` preset), **`.github/workflows/ci.yml:59`**
  (`gcc-tsan`, per-push), **`.github/workflows/nightly.yml:56-89`** (`tsan-full`).
  The lane exists; the stale "no TSan lane" caveat in the older pool refinements
  was already retired by `tasks/refinements/pool/concurrent_pin_benchmark.md`.

## Constraints / requirements

- **The writer's `allocate()` hot path pays nothing measurable.** Relaxed atomic
  read-modify-writes on the *grow* branch only (`slot_store.cpp:212-227`, taken
  once per chunk, not per slot) and one relaxed increment of `d_high_water` per
  slot — the same instruction the already-atomic `d_slots_live.fetch_add`
  (`:230`) emits today on that exact line. **No lock may be added to `allocate()`,
  `release()`, or `free_now()`** — that would contradict `pool.free_pools`'s
  hot-path discipline and doc 15:152-158's RT-adjacent thread rules.
- **The `HousekeepingThread::d_mutex` scope does not change.** It serializes the
  single-drainer; it is not, and must not become, an allocator lock. The fix
  lives entirely in `arbc::pool`.
- **No new counting mechanism** (`runtime.housekeeping`'s standing decision). The
  counters that exist keep counting exactly what they count; only their
  *publication* changes.
- **Accessors keep their signatures** — `const noexcept`, same return types. Every
  existing caller (`src/pool/t/pool.t.cpp:79-96`, `free_pools.t.cpp:87,110`,
  `big_block_pool.t.cpp:104-161`, `src/kind_raster/t/raster_pool_backing.t.cpp:98-115`,
  `tests/stress_publish_pin.t.cpp:124,214-215`, `checkpoint.hpp`'s store-table
  write) must compile unchanged. Locking inside a `const noexcept` accessor is
  already house practice (`free_slots()`, `slot_store.hpp:254-257`).
- **`high_water()` stays correctness-load-bearing on the writer.** The checkpointer
  reads it to write the store table (doc 15:168-171, "it reads the allocator's
  high-water"). A relaxed atomic is sound there precisely because the writer is
  its only mutator: program order gives the writer its own latest value.
- **The `slot_store.hpp:112-126` threading comment must be corrected** — it
  currently asserts the accounting is "single-threaded and unsynchronized".
- **Doc 17 levelization:** the change is confined to `arbc::pool` (L1) and its
  tests. No new component edge; `arbc::runtime` keeps reading the pool through the
  `HousekeepingTarget` vtable it already uses. A new top-level test in `tests/`
  links the `arbc` umbrella, as `tests/CMakeLists.txt:709-744` already does.
- **RT-safety:** neither the aggregators nor `store_for` sit on any
  `[[clang::nonblocking]]` chain (the audio callback touches no allocator — doc
  15:152-154). The gate's RT-safety check must stay green with the new lock.

## Acceptance criteria

1. **The counters are atomic.** `d_slots_capacity`, `d_bytes_reserved` and
   `d_high_water` become `std::atomic` (`std::size_t`, `std::size_t`,
   `SlotIndex`), written with `fetch_add(..., relaxed)` / `store(..., relaxed)`
   at `slot_store.cpp:225-226`, `:229`, `:267`, `:286-287`, `:290`, and read with
   `load(relaxed)` in `high_water()`, `slots_capacity()`, `bytes_reserved()`.

2. **The arena aggregate walk is guarded.** A `mutable std::mutex d_stores_mutex`
   on `Arena` is held across the body of `store_for()` and across
   `total_slots_live()` / `total_high_water()` / `total_bytes_reserved()` /
   `store_count()` and the loop at `slot_store.hpp:376`. The `SlotStore&`
   `store_for` returns is used after the lock drops — legal because `d_stores` is
   a `std::map` (node-stable), and a comment must say so, since the safety of the
   whole scheme rests on that container choice.

3. **The TSan test — the acceptance criterion the `.tji` names.** A new stress
   `TEST_CASE` (home: `tests/stress_document_housekeeping.t.cpp`, alongside the
   existing Document-level concurrency stress, or a sibling `tests/` executable
   registered exactly as `tests/CMakeLists.txt:741-744` does) in which a **host
   thread polls `Document::memory_stats()` in a loop** while the **writer thread
   allocates through a growing arena** — growing in both senses the fix cares
   about: forcing new *chunks* (hazard 1) and forcing new *size-class stores* via
   fresh typed views / `BigBlockPool` blobs (hazard 2). The poll must overlap the
   writer — it runs *before* the join, unlike `stress_document_housekeeping.t.cpp:234`.
   Clean under `ctest --preset tsan` (locally `ARBC_GATE_PRESET=tsan scripts/gate`)
   and on the per-push `gcc-tsan` lane. **Verify the test is a real witness:**
   confirm it reports the race under TSan *before* the fix, then goes clean after.
   A test that would have passed pre-fix is worthless here.

4. **Behavioral-counter assertion, not wall-clock** (doc 16:54-62): the poller
   asserts `bytes_reserved` is **monotonically non-decreasing** across its samples
   and that the final post-join sample equals `Arena::total_bytes_reserved()` read
   single-threaded — i.e. the concurrent reads saw values the counter actually
   held, and the fix did not cost an update. No sleeps, no timing assertions; the
   loops are bounded by an iteration count and a `stop` flag, as the existing
   stress files are.

5. **Claims-register growth** (doc 16). New row in `tests/claims/registry.tsv`:
   `15-memory-model#accounting-reads-concurrent-with-allocation` — *"Arena and
   SlotStore accounting accessors (bytes reserved, slots capacity/live, high
   water) are safe to read from a non-writer thread while the writer allocates and
   mints new size-class stores; the host memory-panel poll is race-free."* Tagged
   with an `enforces:` comment on the new test, per the register's format
   (`registry.tsv:1-2`). It completes `:114`
   `15-memory-model#housekeeping-reports-memory-panel-stats`, which only pins the
   snapshot's honesty single-threaded.

6. **Design-doc delta lands in the same commit** (doc 16's same-commit rule):
   `docs/design/15-memory-model.md:186-195` (written by this refinement) states the
   cross-thread read contract and its deliberate weakness (per-counter honest, not
   a coherent snapshot across stores).

7. **Coverage & gate:** ≥90% diff coverage on changed lines; `scripts/gate` green
   including the asan and tsan lanes; no regression in
   `src/pool/t/pool.t.cpp`'s monotonic-capacity assertion (`:96`) or the
   `tests/stress_publish_pin.t.cpp` counter reads (`:124`, `:214-215`).

No deferred follow-ups: this task closes its own hazards, and the adjacent
`Checkpointer`-counter races already have a WBS home in
`runtime.background_checkpoint_quiesce` (M10). Nothing new to register.

## Decisions

**1. Relaxed/relaxed, not the `.tji`'s "relaxed load / release store".** The note
says "relaxed load / release store"; that pairing is incoherent — a release store
orders *other* prior writes and buys nothing without an acquire load to pair
with, and these counters publish no data. They are diagnostics on no correctness
path. House style is already exactly this: relaxed for counters
(`d_slots_live` `slot_store.cpp:204,230,237`; `d_spill_count`/`d_refill_count`
`:170,187`; `BigBlockPool::d_blobs_allocated` `big_block_pool.cpp:36`),
acquire/release reserved for handoff (`d_release_fence` `slot_store.hpp:167`, the
`SlabDirectory` publication). *Rejected:* seq_cst (a full barrier on the writer's
grow path for a diagnostic — the one place doc 15 says must stay lock-free);
release/acquire (pays a barrier to establish a happens-before edge no reader
uses). The `.tji` note is WBS prose, not a design doc, so this deviation needs no
delta — it is recorded here.

**2. A mutex for the store-map walk; the counters stay lock-free.** The two
hazards get two different tools, because they have two different shapes. Counter
writes are on the writer's per-slot/per-chunk path and must stay lock-free →
atomics. The store-map mutation is *cold* (once per typed view, once per
page-sized blob — never per slot; see the `store_for` caller list above) and its
hazard is a tree walk over links being rebalanced, which no per-word atomic can
fix → a mutex, held on the cold insert and the cold aggregate walk. The
aggregators are polled by a host at frame cadence, so the lock is uncontended in
practice.
*Rejected — Arena-level atomic aggregates* (`d_total_bytes_reserved` maintained
by `SlotStore` on grow, so the aggregators never walk the map): needs a
back-pointer from `SlotStore` to `Arena`, introduces a second copy of state that
can drift from the per-store truth, and is precisely the "new counting mechanism"
`runtime.housekeeping` rejected. It also leaves `store_count()` and the `:376`
loop racing.
*Rejected — an atomic snapshot vector of stores* (an append-only published array,
mirroring `SlabDirectory`): the right answer if the walk were hot or on an RT
thread. It is neither. That is a new lock-free structure to justify, test and
maintain for a path a memory panel touches sixty times a second.
*Rejected — declaring the aggregators writer-thread-only:* it would make the fix
trivial and the feature useless. Doc 15:179-184 exposes the accounting *because*
a host wants a memory panel; a panel the host may not poll from its own thread is
not a panel. This is the alternative the doc delta explicitly names and closes.

**3. `d_high_water` joins the atomicization even though it is not in
`HousekeepingStats`.** It is the same plain counter, written on the same
unlocked writer path (`slot_store.cpp:229`), and read by the same aggregate
family (`total_high_water()`, `slot_store.cpp:369-375`) which is a public `const`
`Arena` accessor a memory panel can reach. Leaving one of three plain would
leave the seam half-closed and the next reader to wire `high_water` into a panel
would reopen it silently. The cost is zero: `++d_high_water` becomes a relaxed
`fetch_add` on the same line that already does one for `d_slots_live`.
*Rejected:* minimal-diff (touch only the two counters `stats()` reads today) —
it optimizes for a smaller diff over a coherent invariant, and the invariant is
what the new claim states.

**4. The Checkpointer counters stay out.** `slots_freed_to_list` and
`durable_epoch` are read by the *same* `stats()` call (`housekeeping.cpp:82-83`)
and are plain members — so it is tempting to sweep them in. They are not live:
every commit originates on the writer thread under the `HousekeepingThread`
mutex that `stats()` also takes, so they *are* serialized today
(`tasks/99-milestones.tji:79`). Folding them in would blur the boundary the M9/M10
audit drew — live race here, latent races there — and would quietly do part of
`runtime.background_checkpoint_quiesce`'s job without doing the rest of it. The
load-bearing invariant that keeps them latent (every drain and every commit
funnels through `HousekeepingThread`) is recorded in the milestone note and is
not weakened by anything in this task.

**5. The test lives at the `Document` level, not the `Arena` level.** A bare
two-thread `Arena` test would be smaller and would still trip TSan. But the race
the `.tji` cares about is the *shipped* one — a host polling
`Document::memory_stats()` — and only a `Document`-level test proves the whole
chain (`document.cpp:133` → `housekeeping_thread.cpp:81-84` →
`housekeeping.cpp:81` → `housekeeping_targets.hpp:41` → `Arena::total_*`) is
race-free. `tests/stress_document_housekeeping.t.cpp` already stands up exactly
that rig, minus the concurrent poll. Reuse it.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-14.

- `src/pool/arbc/pool/slot_store.hpp` — `d_high_water`, `d_slots_capacity`, and `d_bytes_reserved` converted to `std::atomic` with relaxed load/store; `mutable std::mutex d_stores_mutex` added to `Arena`; threading-contract comment at `:112-126` corrected; aggregator accessors (`store_count()`, `for_each_store()`) and `store_for()` locked on `d_stores_mutex`.
- `src/pool/slot_store.cpp` — write sites in `allocate()`, `reserve_restored()`, and `finalize_restore()` switched to `fetch_add(relaxed)` / `store(relaxed)`; `store_for()`, `total_slots_live()`, `total_high_water()`, `total_bytes_reserved()` guarded by `d_stores_mutex`; comment on `std::map` node-stability invariant added.
- `tests/stress_arena_accounting_poll.t.cpp` (new) — two stress `TEST_CASE`s: one polls `Document::memory_stats()` concurrently while the writer grows a `Document` arena (hazard 1 — counter tear); one polls `BigBlockPool` aggregates while the writer walks size classes (hazard 2 — store-map race). Both enforce monotone `bytes_reserved` and final-value equality.
- `tests/CMakeLists.txt` — registered `stress_arena_accounting_poll` as a new test executable, linked identically to the existing stress tests.
- `tests/claims/registry.tsv` — new row `15-memory-model#accounting-reads-concurrent-with-allocation` enforced by `stress_arena_accounting_poll`.
- `docs/design/15-memory-model.md` — design-doc delta landing in the same commit (doc 16 same-commit rule): states the cross-thread read contract and its deliberate weakness (per-counter honest, not a coherent snapshot across stores).
- TSan witness confirmed: stashing the fix revealed 12 TSan data races (writes at `slot_store.cpp:226` racing the poller's `Arena::total_bytes_reserved()`) and exit 66; restoring the fix is clean under `tsan`.
- Claim `15-memory-model#accounting-reads-concurrent-with-allocation` added to registry (290 enforced, all green).
