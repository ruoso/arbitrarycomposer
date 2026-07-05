# runtime.housekeeping_thread — Low-priority background housekeeping thread

## TaskJuggler entry

`tasks/65-runtime.tji:43-48` → `runtime.housekeeping_thread` ("Low-priority
housekeeping thread"), the seventh leaf under `task runtime`. It carries
`depends !housekeeping` (`65-runtime.tji:46`); the parent `task runtime` adds
`depends compositor.tile_planning` (`65-runtime.tji:6`). Note line:

> "Low-priority background housekeeping thread: a std::thread that parks/wakes on
> a condition variable and periodically calls Housekeeper::tick(), serializing as
> the single drainer against writer-thread drains, with graceful stop/join;
> stress + TSan coverage of RT-thread enqueue-while-draining. Docs 14/15."

It is a dependency of milestone `m2_editing` ("M2: Versioned editing",
`tasks/99-milestones.tji:21-25`, which already lists `runtime.housekeeping_thread`
in its `depends`).

## Effort estimate

**2d.** The passive policy object this task drives — `arbc::Housekeeper` — is
already built and tested (`runtime.housekeeping`, DONE 2026-07-05). This task adds
**one new mechanism**: the repo's *first* owned background worker thread. It is a
thin, correct concurrency wrapper — a `std::thread` that parks on a
`std::condition_variable`, wakes on a timeout or an explicit poke, calls
`Housekeeper::tick()` under a mutex that also guards the writer's `after_commit`
(so exactly one thread drains at a time), and stops/joins gracefully with a final
drain-to-quiescence. The 1d premium over `runtime.housekeeping` is entirely the
concurrency surface: a park/wake/stop/join lifecycle that does not exist anywhere
in the codebase yet (no `std::condition_variable` appears in `src/` today), plus
its stress + TSan-ready coverage of RT-thread enqueue-while-draining. The
deliverable is one header/impl pair (`housekeeping_thread.hpp`/`.cpp`), a unit +
stress test file, two claims, and the CMake wiring (no new `DEPENDS` edge — the
`pool` edge `runtime.housekeeping` added already covers it).

## Inherited dependencies

**Settled:**

- `runtime.housekeeping` (DONE 2026-07-05, `65-runtime.tji:36-42`,
  `complete 100`) — the direct predecessor (`depends !housekeeping`). It shipped
  the passive, thread-agnostic policy object this task wraps in a thread. From
  `src/runtime/arbc/runtime/housekeeping.hpp`:
  - **`class Housekeeper`** (`:64-119`) — constructed with
    `(ReclamationQueue& queue, Checkpointer* checkpointer, Arena* arena,
    HousekeepingConfig config)` (`:70-71`). Non-copyable. Entry points:
    - **`expected<std::monostate, WorkspaceFileError> after_commit(SlotIndex root)`**
      (`:81`, `housekeeping.cpp:20-36`) — the writer-between-transactions drain
      site; drains (when `drain_between_transactions`), records the tip, bumps
      counters, commits on the every-N-transactions trigger.
    - **`expected<std::monostate, WorkspaceFileError> tick(std::uint64_t
      monotonic_tick)`** (`:87`, `.cpp:38-60`) — the timer / low-priority-thread
      entry; **always drains**, then commits on the tick-interval trigger gated on
      ≥1 dirty transaction. `monotonic_tick` is a **handed** value — the object
      reads no clock.
    - **`request_checkpoint()`** (`:92`), **`drain_and_quiesce()`** (`:96`, the
      teardown drain, doc 15:144-147), **`stats()`** (`:99`, the wall-clock-free
      `HousekeepingStats` snapshot).
  - The class comment is explicit that the owned thread is *this* task
    (`housekeeping.hpp:24-31`): the object "owns no thread and reads no clock … The
    contract the CALLER must honor: exactly one thread calls a draining entry point
    at a time. RT threads only enqueue … The owned background low-priority thread
    is the deferred follow-up `runtime.housekeeping_thread`."
  - `housekeeping.md` (predecessor refinement) deferred this task verbatim under
    "Not this task" and "Named future task": "a `std::thread` that parks/wakes and
    calls `tick()` on an interval, serializing as the single drainer against writer
    drains … Building/owning the thread is genuinely concurrency-touching (needs
    TSan/stress coverage) and additive over this policy object."
- `pool.reclamation` (DONE, transitively) —
  `src/pool/arbc/pool/reclamation.hpp:57-62` states the single-drainer contract
  this task realizes: "Drain is **SINGLE-DRAINER, ANY ONE THREAD** … the drainer
  may be the writer between transactions **OR** the low-priority housekeeping
  thread — exactly one at a time (**runtime.housekeeping serializes the choice**);
  … RT threads only enqueue." `drain()` (`:82-92`) is a no-op on empty/double
  drain.
- `pool.free_pools` (DONE, transitively) — relaxed release to
  single-drainer-any-thread by admitting cross-thread release into a thread-local
  free pool (claims `#thread-local-free-pools-spill-to-global`,
  `#reuse-is-thread-affine`, `registry.tsv:48-49`). This is what makes the
  low-priority-thread drain site *safe*: RT producers release cross-thread onto
  local pools while a single drainer (writer or this thread) reclaims. Its
  concurrent test (`src/pool/t/free_pools.t.cpp:237-293`) is the nearest existing
  model for this task's stress test — a `std::thread` drainer looping
  `queue.drain()` while the writer allocates, joined on an atomic `stop`, asserting
  outcome (`slots_live() == 0`), never timing.
- `pool.checkpoints` (DONE, transitively) — `Checkpointer` and its behavioral
  counters (`src/pool/arbc/pool/checkpoint.hpp`), driven only through
  `Housekeeper::tick`/`request_checkpoint`.

**Pending:** none — every predecessor is landed.

## What this task is

Deliver the **owned background housekeeping thread** for `arbc::runtime` (L5,
doc 17:26,:60 — "housekeeping thread" is a listed runtime content): a thin
concurrency wrapper that turns the passive `Housekeeper` into a live low-priority
drain/checkpoint loop, and that serializes that loop against writer-thread drains
so the reclamation queue always has exactly one drainer. In a new header/impl
`src/runtime/arbc/runtime/housekeeping_thread.hpp` (+ `housekeeping_thread.cpp`):

1. **`struct HousekeepingThreadConfig`** — the thread-lifecycle knobs, all
   defaulted:
   - `std::chrono::steady_clock::duration tick_period` — the idle park interval
     between automatic wake-ups (default a modest low-priority cadence, e.g.
     `50ms`). It is the `condition_variable::wait_for` timeout; it never appears in
     a test assertion.
   - `std::function<std::uint64_t()> tick_source` — the **monotonic tick provider**
     the thread hands to `Housekeeper::tick()`. Default: a `steady_clock`-derived
     monotonic counter (elapsed ticks since construction). Injectable so tests hand
     a deterministic, controlled sequence — no test asserts on a wall clock
     (doc 16:54-62).
   - `std::function<void(const WorkspaceFileError&)> on_checkpoint_error` —
     nullable. A background `tick`'s checkpoint I/O failure has no synchronous
     caller to return the value to; this callback surfaces it (errors as values,
     never abort — doc 15, `checkpoint.hpp` contract). When null, the error is only
     recorded (see `last_checkpoint_error()`).
2. **`class HousekeepingThread`** — **owns** the `Housekeeper` by value (sole
   access path → serialization enforced by construction), a `std::mutex`, a
   `std::condition_variable`, the stop/poke flags, background counters, and the
   `std::thread` (started last in the ctor):
   - **Constructor** `(ReclamationQueue& queue, Checkpointer* checkpointer, Arena*
     arena, HousekeepingConfig policy, HousekeepingThreadConfig thread_config)` —
     constructs the owned `Housekeeper{queue, checkpointer, arena, policy}` and
     launches the background loop. Non-copyable, non-movable (owns a thread +
     mutex).
   - **Destructor** — `request_stop()` then `join()`; the loop's stop path runs a
     final `drain_and_quiesce()` so nothing is left on the queue at teardown
     (doc 15:144-147).
   - **`expected<std::monostate, WorkspaceFileError> after_commit(SlotIndex root)`**
     — the **synchronized writer entry**: acquires the mutex, delegates to
     `Housekeeper::after_commit`. Holding the same mutex the background loop holds
     while it `tick()`s is what serializes the two drainers into a single drainer.
   - **`expected<std::monostate, WorkspaceFileError> request_checkpoint()`** and
     **`void drain_and_quiesce()`** — synchronized delegates for the explicit
     host-call trigger and the explicit teardown/bulk-release drain.
   - **`void poke() noexcept`** — wake the loop for an immediate tick
     (non-blocking); a host "there was activity, drain soon" hint and the test
     driver's wake.
   - **`std::uint64_t flush()`** — poke and **block until the loop completes one
     further tick**, returning the new background-tick count. It waits on a
     *condition* (the tick counter advanced), never a wall clock — a deterministic
     synchronization point for tests and a host "drain now and wait" call.
   - **`void request_stop() noexcept`** — idempotent; sets the stop flag and wakes
     the loop (join happens in the destructor).
   - **`HousekeepingStats stats() const`** — synchronized snapshot delegating to
     `Housekeeper::stats()`.
   - **`std::uint64_t background_ticks() const noexcept`** and
     **`std::optional<WorkspaceFileError> last_checkpoint_error() const`** — the
     thread-level observability the policy object cannot expose (loop iterations
     completed; last background checkpoint error).
3. **The background loop** (`run()`, in the `.cpp`): park on the cv with the
   `tick_period` timeout and a `stop || poke` predicate; on wake (timeout *or*
   poke), if stopping break, else clear the poke, read a tick from `tick_source`,
   call `Housekeeper::tick(t)` **under the mutex**, capture any error
   (`last_checkpoint_error` + `on_checkpoint_error`), bump `background_ticks`, and
   notify the progress cv (waking any `flush()`). On stop, run one final
   `drain_and_quiesce()` before returning.

**Not this task:**

- **The reclamation/checkpoint *mechanisms* and the *policy*** — `ReclamationQueue`,
  `Checkpointer`, and the `Housekeeper` cadence object — are `pool.*` /
  `runtime.housekeeping` (DONE). This task adds no L1 code and no new policy
  decision; it drives the existing `Housekeeper` entry points from a thread and
  serializes them against the writer.
- **Wiring the `HousekeepingThread` into the live `Document`/`Model` arenas.**
  `runtime::Document` is not yet slab-arena-backed with installed
  `DeferredReclaimSink`s and a `Checkpointer` (`document.hpp:29-32`, "migrates …
  when the Editable facet and the slab arenas land"). This task validates the
  thread against **real pool fixtures** (an `Arena` + `RefStore` +
  `ReclamationQueue`, and a workspace-backed `Checkpointer`), exactly as
  `runtime.housekeeping` did; the Document→arena rewire that routes the real writer
  through `HousekeepingThread::after_commit` rides that later migration and is
  **already parked** (`parking-lot.md:63-66`).
- **A TSan CI *lane* / preset.** The repo has no ThreadSanitizer preset or CI job
  today (`CMakePresets.json` has only `asan` = `address,undefined`; the generic
  `ARBC_SANITIZERS` plumbing at `CMakeLists.txt:29-35` would accept `thread` but no
  preset selects it). Wiring a `tsan` preset + `.github/workflows/` lane is a
  standing human infra decision already parked for `pool.reclamation` and
  `pool.free_pools` (`parking-lot.md:33-41`); this task is a **third consumer** of
  that eventual lane, not the task that adds CI infra. Its stress test is written
  **TSan-ready** and runs green under the existing dev + ASan/UBSan gate today
  (ASan catches the use-after-release a mis-serialized drain would cause, even
  though it does not detect the race itself).
- **RT-safety enforcement of the enqueue path** (RealtimeSanitizer /
  `[[clang::nonblocking]]` on the callback chain, doc 16:69-72) — that is
  `audio.rt_safety`'s. This task's producers exercise the enqueue-only path
  (doc 15:137-143) but do not add the build-failing RT check.

## Why it needs to be done

`runtime.housekeeping` shipped the *policy* but left it passive: something else
must call `after_commit`/`tick` on a cadence, and doc 15:129-136 names two drain
sites — "**writer thread between transactions, or a low-priority thread**" — that
pop the *same* type-erased reclamation queue. Without an owned background thread,
the only drain site is the writer's own `after_commit`; a long idle stretch with
no transactions (a paused editor, a slow export holding a pin) never drains,
because a still scene issues no `after_commit`. The low-priority thread is the
sanctioned second drain site that keeps reclamation bounded and checkpoints on a
timer cadence (doc 15:213 lists "timer" as a first-class checkpoint trigger)
independent of writer activity. But a second drainer is only correct if it never
races the writer's drain — `reclamation.hpp:57-62` and `housekeeping.hpp:28-29`
both assign that serialization to runtime.housekeeping and neither implements it,
because the passive object owns no thread. **This task is the serialization: the
mutex that makes "writer-between-transactions OR low-priority-thread" a
single-drainer discipline rather than a data race, plus the thread that drives the
low-priority site.** It is a dependency of `m2_editing`
(`99-milestones.tji:21-25`): versioned editing under sustained churn needs a
background drain so reclamation does not accumulate to the teardown boundary, and
needs checkpoints on a timer so crash-cost has a bounded floor even when the user
walks away mid-session.

## Inputs / context

- `docs/design/15-memory-model.md`:
  - **`:129-136`** — the two drain sites: "a housekeeping pass — **writer thread
    between transactions, or a low-priority thread** — pops, runs destructors …
    continue the cascade *iteratively*." Both pop the one type-erased reclamation
    queue — the basis for serializing them as a single drainer.
  - **`:137-143`** — thread rules: "the audio *callback* touches no allocator, no
    refcount, ever … Render and audio-engine threads may pin/unpin … and **enqueue
    reclamation, nothing more** … The writer thread is the only structural
    allocator." Constrains the stress test: producers (RT surrogates) may only
    `retain`/`release` (enqueue); only the writer thread may `create`.
  - **`:144-147`** — teardown drain: "bulk-release path runs the reclamation queue
    to quiescence first for types with external resources" — the final
    `drain_and_quiesce()` the loop's stop path runs.
  - `:149-154` — debug discipline: per-arena live counts through the API;
    generation tags fault a use-after-release loudly; "leak check = arena live count
    at teardown" — the outcome assertion the stress test uses.
  - **`:213-214`** — "**Checkpoint cadence is policy (timer, transaction count,
    explicit host call)**" — the timer trigger this thread drives via `tick`.
- `docs/design/16-sdlc-and-quality.md`:
  - `:15-25` — the claims register (`<doc-stem>#<slug>` + `// enforces:` tag; CI
    fails a registered claim with no live test).
  - `:54-62` — **behavioral-counter tests, never wall-clock.** "slots
    allocated/reclaimed" is a named exposed counter; assert reclamation via the
    live-count and the `Housekeeper`/thread counters, never by sleeping.
  - **`:66-73`** — **concurrency tests:** "**TSan on the full suite; dedicated
    stress tests for the publish/pin protocol and the reclamation queue with
    schedule perturbation (randomized yields under a seed)**." The reclamation queue
    is named as a stress-test target — this is the mandate this task's stress test
    answers.
  - `:101-105` — per-push CI runs "ASan+UBSan and TSan jobs"; the TSan job is the
    parked infra the stress test will eventually run under.
  - `:112-118` — ≥90% diff coverage on changed lines.
- `docs/design/17-internal-components.md`:
  - `:24-26` / `:60` — `arbc::runtime` (L5) contents explicitly include "**housekeeping
    thread**" alongside the "interactive frame loop"; "Depends on: **everything
    below**."
  - `:78-80` — "deadlines, frame loops, and **device clocks are runtime policy**" —
    a runtime background thread reading a monotonic clock for its cadence is
    consistent with runtime owning clocks; no lower level supplies one.
  - `:41-44` — depend only on strictly lower levels; the CI dependency check
    validates CMake + include graph. No new edge: `pool` is already in runtime's
    `DEPENDS`.
- `docs/design/14-data-model-and-editing.md`:
  - `:111-115` — aborted transactions' "working records … are reclaimed" — feeds
    the same queue this thread drains.
  - `:199-205` — version GC "releases unpinned `DocState` nodes and unreferenced
    state handles by refcount"; "deferred reclamation … is doc 15" — the release
    that enqueues onto the queue this thread services.
- `src/runtime/arbc/runtime/housekeeping.hpp:24-31,:37-119` and
  `src/runtime/housekeeping.cpp:20-87` — the `Housekeeper` this task wraps: its
  passive/no-clock contract, its ctor, and the `after_commit`/`tick`/
  `request_checkpoint`/`drain_and_quiesce`/`stats` entry points.
- `src/runtime/t/housekeeping.t.cpp:29-57,:61-71,:136-199` — the fixture recipe
  this task reuses: the `Rec` test record + `build_chain`; the anonymous fixture
  (`Arena` → `RefStore<Rec>` → `DeferredReclaimSink<Rec>` → `ReclamationQueue` +
  `register_store`); the `WsFixture` (workspace-backed `Checkpointer` with its slot
  fence installed); `TempPath`; and `ErrnoInjector` for fault injection.
- `src/pool/arbc/pool/reclamation.hpp:57-62,:82-92` — the single-drainer-any-thread
  contract and `drain()`'s empty/double-drain no-op.
- `src/pool/t/reclamation.t.cpp:248-303` and `src/pool/t/free_pools.t.cpp:237-293`
  — the existing concurrent-stress idioms to mirror: writer creates all slots up
  front, partitions disjoint blocks to N producers, producers spin-wait on an
  atomic `go` then `release` their block (lock-free enqueue), a single drainer
  loops `drain()` concurrently, all joined on an atomic `stop`, a final drain after
  join, and **outcome-only assertions** (`destructions == total`, `slots_live() ==
  0`) — never timing.
- `src/pool/arbc/pool/slot_store.hpp:313,:327` — the pool's own
  `std::mutex d_pool_mutex` + tracked writer id, the only production mutex today;
  this task's wrapper mutex is the second, and the codebase's first
  `std::condition_variable`.
- `CMakeLists.txt:29-35`, `CMakePresets.json` (`asan` preset only) — the generic
  `ARBC_SANITIZERS` plumbing; no `tsan` preset exists (parked infra).
- `src/runtime/CMakeLists.txt:1-7` — `DEPENDS base model contract compositor pool`
  (pool already present); this task appends `housekeeping_thread.cpp` to `SOURCES`,
  `arbc/runtime/housekeeping_thread.hpp` to `PUBLIC_HEADERS`, and
  `t/housekeeping_thread.t.cpp` to the component test.
- `tasks/refinements/runtime/housekeeping.md` — the predecessor's Decisions/Status
  establishing the passive-object contract and deferring this thread.
- `tests/claims/registry.tsv:40-49` — the reclamation/free-pool claims this task
  builds on; this task appends two rows.
- `tasks/parking-lot.md:33-41` (TSan lane), `:63-66` (Document→arena rewire) — the
  two standing parked items this task inherits rather than resolves.

## Constraints / requirements

- **Levelization (doc 17:41-44,:60).** `arbc::runtime` is L5 and reaches the pool
  mechanisms only through the public pool headers, exactly as
  `runtime.housekeeping` does. **No new `DEPENDS` edge** — `pool` is already
  present. The CI dependency check stays green.
- **Single-drainer, enforced by construction (doc 15:129-136,
  `reclamation.hpp:57-62`).** The background `tick()` and the writer's
  `after_commit()` must never drain the reclamation queue concurrently. The wrapper
  guarantees this by **owning** the `Housekeeper` and gating every draining entry
  point (background `tick`, writer `after_commit`, `request_checkpoint`,
  `drain_and_quiesce`) behind one mutex — there is no unsynchronized path to the
  `Housekeeper`. RT producers never touch the wrapper; they enqueue via
  `store.release()` (lock-free, doc 15:137-143).
- **No wall-clock in tests (doc 16:54-62).** The production loop *may* read
  `steady_clock` to produce its monotonic tick and to time its park (a runtime
  timer cadence is sanctioned, doc 15:213; clocks are runtime policy, doc 17:80),
  but **no test asserts on a wall clock or sleeps to synchronize.** Tests inject a
  deterministic `tick_source` and synchronize via `flush()` (waits on the tick
  counter) and atomic flags; every assertion is on a behavioral counter or a live
  count.
- **Errors as values on a caller-less thread.** A background `tick` checkpoint
  failure never aborts: it is captured in `last_checkpoint_error()` and delivered
  to the optional `on_checkpoint_error` callback. Synchronous entry points
  (`after_commit`, `request_checkpoint`) return the `expected<…, WorkspaceFileError>`
  to their caller unchanged. The file remains recoverable to its last durable root
  (`registry.tsv:45`).
- **Graceful stop/join with a final drain (doc 15:144-147).** `request_stop()`
  must wake a parked thread promptly (cv notify, not a timeout wait); the loop must
  run one final `drain_and_quiesce()` before returning so teardown leaves no slot
  on the queue; the destructor must `join()`. No detach, no `std::terminate` on a
  live thread, no hang.
- **Anonymous arenas (doc 15:160-162).** With `Checkpointer* == nullptr` the
  checkpoint triggers inside `tick`/`after_commit` are already inert (the
  `Housekeeper` guards on null); the thread still drives the reclamation drain and
  the live-count observability.
- **First concurrency primitive in the engine — TSan/stress obligation
  (doc 16:66-73).** This is a concurrency-touching task by construction. It must
  ship a dedicated stress test for the reclamation-queue drain race with schedule
  perturbation, structured TSan-ready. Because no TSan preset exists yet
  (parked), the stress test runs under the dev + ASan/UBSan gate; the TSan lane is
  the third consumer of the standing parked infra.
- **The public header compiles standalone;** CI diff coverage ≥90%
  (doc 16:112-118).

## Acceptance criteria

- **Unit tests — deterministic, no sleep — in `src/runtime/t/housekeeping_thread.t.cpp`
  (new, Catch2, registered via `arbc_component_test`).** Reusing the
  `runtime.housekeeping` fixtures (`Rec`/`build_chain`, the anonymous fixture, and
  `WsFixture`/`TempPath`/`ErrnoInjector`):
  - **Background tick drains:** over the anonymous fixture, build+drop a retained
    chain so the queue is non-empty, `flush()` (poke + block until one background
    tick completes), and assert the arena's `total_slots_live()` returned to its
    no-garbage baseline and `background_ticks()` advanced by ≥1 — proving the loop
    actually drains, synchronized on the tick counter (no sleep).
  - **Graceful stop + final drain:** build+drop a chain (queue non-empty), then
    `request_stop()` **without** a prior flush; after the destructor joins, assert
    `total_slots_live() == baseline` — the stop path's final `drain_and_quiesce()`
    reclaimed it. A second case constructs and immediately destroys the thread
    (no work) and asserts the destructor returns (join did not hang).
  - **Serialized writer entry:** while the loop is parked, call
    `after_commit(root)` from the test thread over a non-empty queue; assert it
    returns a value and drained (`total_slots_live() == baseline`) — the writer
    path works through the wrapper's mutex.
  - **Tick-interval checkpoint via injected deterministic tick source:** over
    `WsFixture` with `checkpoint_tick_interval = 100` and a `tick_source` the test
    controls (a lambda returning a value it sets), do one `after_commit` (dirty),
    set the tick source to 50 and `flush()` → `commit_count() == 0`; set it to 100
    and `flush()` → `commit_count() == 1`; set it to 250 with no intervening
    transaction and `flush()` → `commit_count()` unchanged and
    `stats().checkpoints_skipped_clean` advanced. Deterministic — the tick source
    is test-controlled, no wall clock.
  - **Background error surfacing:** over `WsFixture` with a fault-injected `Msync`
    failure armed and a checkpoint trigger due, `flush()` a tick whose commit
    fails; assert `last_checkpoint_error()` holds the `WorkspaceFileError`
    (`HeaderIoFailed`/`EIO`), the `on_checkpoint_error` callback fired, the process
    did not abort, and `stats().checkpoints_committed` did not count the failed
    commit.
- **Stress test — concurrency, outcome assertions only, TSan-ready — same file,
  guarded like the pool concurrent tests.** Mirroring
  `pool/t/reclamation.t.cpp:248-303` / `free_pools.t.cpp:237-293`: the writer
  thread creates all slots up front and partitions disjoint blocks to N producer
  (RT-surrogate) threads; producers spin-wait on an atomic `go` then
  `retain`/`release` their block (lock-free enqueue, never touching the wrapper);
  the `HousekeepingThread` runs concurrently on a short `tick_period` draining as
  the low-priority single drainer while the writer thread also calls
  `after_commit` in a loop (serialized by the wrapper mutex). Drive a **fixed
  operation count** (not a duration) with light **schedule perturbation** (a
  `std::this_thread::yield()` at fixed-stride iteration points to widen the race
  window; a seeded-RNG yield variant is an acceptable enhancement). After the op
  count, drop all roots, `request_stop()`, join, and assert **outcomes only**: no
  crash/hang, and `arena.total_slots_live() == baseline` (every released slot
  reclaimed exactly once, none lost or double-freed). **No timing assertion.** This
  is the RT-thread enqueue-while-draining coverage; it runs green under dev +
  ASan/UBSan and is structured so `-fsanitize=thread` exercises the writer-drain /
  background-drain and producer-enqueue / drain race windows.
- **Claims (register + `enforces:` tags)** appended to `tests/claims/registry.tsv`
  (format `<claim-id><TAB><description>`, `registry.tsv:1`), enforced from the
  tests above:
  - `15-memory-model#housekeeping-thread-single-drainer` — "The background
    housekeeping thread and the writer's between-transaction drain are serialized
    so exactly one thread drains the reclamation queue at a time; under concurrent
    RT-thread enqueues while a drain runs, every released slot is reclaimed exactly
    once and the arena returns to its no-garbage baseline, with no lost or
    double-freed slots." (doc 15:129-136, :137-143) — enforced by the stress test.
  - `15-memory-model#housekeeping-thread-stops-gracefully` — "Requesting the
    background housekeeping thread to stop wakes it from its park, runs one final
    drain to quiescence, and joins cleanly, leaving no slot un-reclaimed at
    teardown." (doc 15:144-147) — enforced by the graceful-stop + final-drain
    tests.
  - The tick-interval unit test additionally re-enforces the predecessor's
    `15-memory-model#checkpoint-cadence-is-policy` through the thread's tick path
    (a second `// enforces:` tag on the existing claim — no new row).
- **Behavioral-counter discipline (doc 16:54-62).** Every assertion is on a live
  count, a `Housekeeper`/`Checkpointer` counter, or the thread's
  `background_ticks()`; synchronization is via `flush()` (tick-counter condition)
  and atomic `go`/`stop` flags. No test reads a wall clock or sleeps.
- **Component wiring & CI dependency check:** `src/runtime/CMakeLists.txt` adds
  `housekeeping_thread.cpp` to `SOURCES`, `arbc/runtime/housekeeping_thread.hpp` to
  `PUBLIC_HEADERS`, and `t/housekeeping_thread.t.cpp` to the component test; **no
  `DEPENDS` change** (`pool` already present); the header compiles standalone; the
  doc-17 dependency check passes.
- **Gate green (build + tests in Debug + ASan/UBSan).** The stress test runs green
  under the existing `asan` lane (ASan catches a mis-serialized drain's
  use-after-release). **TSan lane deferred to the standing parked infra**
  (`parking-lot.md:33-41`); this task adds no `.github/workflows/` or
  `CMakePresets.json` change — the closer records this task as a third consumer of
  that parked TSan-lane item.

## Decisions

- **The wrapper owns the `Housekeeper` by value; the mutex is the single-drainer
  guarantee.** Because the `Housekeeper` is passive and unsynchronized
  (`housekeeping.hpp:24-31`), *some* object must serialize the two sanctioned drain
  sites (doc 15:129-136, `reclamation.hpp:57-62`). Owning the `Housekeeper` inside
  `HousekeepingThread` and routing **every** draining entry point — background
  `tick`, writer `after_commit`, `request_checkpoint`, `drain_and_quiesce` —
  through one mutex means there is no unsynchronized backdoor to the drainer; the
  single-drainer invariant holds by construction. *Rejected:* holding a
  `Housekeeper&` and leaving the writer free to call `hk.after_commit` directly —
  that leaves an unsynchronized path that races the background drain, defeating the
  whole point. *Rejected:* a separate "drainer token"/lock held only across the
  `drain()` call rather than the whole `tick` — premature; a checkpoint inside
  `tick` must also be serialized against the writer's tip updates, and holding the
  mutex across the whole `tick` is simpler and correct. The cost — the writer's
  `after_commit` can block behind a slow background checkpoint msync — is inherent
  to single-drainer and acceptable for a low-priority background regime; if
  profiling ever shows writer stalls, finer-grained drainer hand-off is a future
  tuning question (parking-lot, not a WBS task — it has no trigger yet).
- **A handed/injectable monotonic tick source, defaulting to `steady_clock`.** The
  production loop needs a real timer to wake and a monotonic value to hand
  `Housekeeper::tick` (doc 15:213 sanctions a "timer" cadence; clocks are runtime
  policy, doc 17:80). Making the tick source a `std::function` defaulting to a
  `steady_clock`-derived counter keeps production honest (a real low-priority
  cadence) while letting tests inject a deterministic sequence — so no *test*
  reads a wall clock (doc 16:54-62), consistent with how `Housekeeper::tick` takes
  a *handed* value rather than reading a clock. *Rejected:* an internal-only
  `steady_clock::now()` with no injection — untestable without sleeps, reintroducing
  exactly the flaky wall-clock dependency doc 16 forbids.
- **`flush()` synchronizes tests on the tick counter, not on time.** A background
  loop is only deterministically testable if the test can wait for "a tick
  completed" without sleeping. `flush()` pokes the loop and blocks on the progress
  cv until `background_ticks` advances — a wait on a *condition*, fully
  deterministic and race-free. It doubles as a genuine host API ("drain now and
  wait", e.g. before an explicit save). *Rejected:* a fixed `sleep_for` in tests to
  "let the thread run" — flaky in CI and a wall-clock dependency; *rejected:*
  exposing raw internals for tests to poll-spin — `flush()` is the clean seam.
- **Background checkpoint errors are captured + callback-surfaced, never thrown.**
  A background `tick` has no synchronous caller to return an `expected` error to,
  but "errors as values, never abort" still binds. Recording the last error
  (`last_checkpoint_error()`) plus an optional `on_checkpoint_error` callback lets
  the host surface an autosave/checkpoint failure to its UI without the thread
  aborting or the error being silently dropped. *Rejected:* letting the error
  escape as an exception on the thread (would `std::terminate`); *rejected:*
  silently swallowing it (a durable-checkpoint failure the host never learns of —
  the opposite of "recoverable to the last durable root").
- **Stress test asserts outcomes, mirrors the pool concurrent idiom, ships
  TSan-ready under the parked lane.** The reclamation queue is a named stress-test
  target (doc 16:66-68). Following `reclamation.t.cpp`/`free_pools.t.cpp` — writer
  creates + partitions, producers enqueue-only, single drainer races, join, final
  drain, outcome-only asserts — reuses a proven, correct pattern and keeps the
  assertion on `slots_live() == baseline` (behavioral, not timing). *Rejected:*
  blocking this task on adding a TSan preset + CI lane — that is a human infra
  decision already parked for two prior pool tasks (`parking-lot.md:33-41`); this
  task's stress test provides the concurrency coverage that *can* gate today (ASan
  on the drain race) and joins the queue for the eventual TSan lane. *Rejected:*
  wall-clock-bounded stress ("run for 2s") — flaky; a fixed op count is
  deterministic.
- **No design-doc delta; no new `DEPENDS` edge.** Doc 15:129-136 already sanctions
  the low-priority-thread drain site, doc 15:213 the timer cadence, doc 17:26/:60
  lists the "housekeeping thread" as a runtime content, and `reclamation.hpp:57-62`
  + the `runtime.housekeeping` refinement already assign the single-drainer
  serialization to runtime.housekeeping. This task *implements* that already-stated
  serialization; it alters no designed behavior, so it needs no doc amendment and
  no doc-00 bullet. (The design-doc survey noted the docs never *spell out* the
  mutual-exclusion protocol between the two drain sites; that guarantee is already
  present implicitly — "a housekeeping pass … pops," singular, plus the
  reclamation.hpp contract — and was established without a delta by the predecessor,
  so introducing one here would be inconsistent over-reach rather than a genuine
  behavior change.) `pool` is already in runtime's `DEPENDS`, so the CMake/include
  graph is unchanged.

## Open questions

(none — all decided.)

Two standing items this task inherits rather than resolves, both already on the
parking lot: (1) the **TSan CI lane/preset** — a human infra + CI-cost decision
already parked for `pool.reclamation` and `pool.free_pools`
(`parking-lot.md:33-41`); this task's stress test is a third consumer. (2) The
**Document→slab-arena rewire** that will route the live writer through
`HousekeepingThread::after_commit` and give the thread real document memory to
drive — already parked (`parking-lot.md:63-66`), gated on the Editable-facet/arena
migration. Both are surfaced in the return summary; neither is encoded as a WBS
leaf.

## Status

**Done** — 2026-07-05.

- Delivered `src/runtime/arbc/runtime/housekeeping_thread.hpp` and `src/runtime/housekeeping_thread.cpp`: `HousekeepingThreadConfig` (tick_period, injectable tick_source, on_checkpoint_error callback) and `HousekeepingThread` owning the `Housekeeper` by value, serializing writer + background drains behind one mutex as the single-drainer guarantee.
- Background loop parks on a `std::condition_variable` with the tick_period timeout; on wake, calls `Housekeeper::tick()` under the mutex, captures checkpoint errors, bumps `background_ticks`, and notifies the progress cv (unblocking `flush()`); stop path runs a final `drain_and_quiesce()`.
- `flush()` synchronizes tests on the tick counter (no sleep, no wall-clock assertion); `poke()` wakes the loop for an immediate tick; `after_commit()`/`request_checkpoint()`/`drain_and_quiesce()` are synchronized delegates.
- Added `src/runtime/t/housekeeping_thread.t.cpp` with 6 unit/stress test cases: background-tick-drains, graceful-stop+final-drain (incl. no-hang construct/destroy), serialized writer `after_commit`, deterministic tick-interval checkpoint via injected tick_source, background checkpoint-error surfacing, and a TSan-ready stress test (8 producers + writer + background thread).
- Registered two new claims in `tests/claims/registry.tsv`: `15-memory-model#housekeeping-thread-single-drainer` (stress test) and `15-memory-model#housekeeping-thread-stops-gracefully` (stop test); added a second `enforces:` tag on `#checkpoint-cadence-is-policy` in the tick-interval test.
- Wired in `src/runtime/CMakeLists.txt`: `housekeeping_thread.cpp` to SOURCES, `arbc/runtime/housekeeping_thread.hpp` to PUBLIC_HEADERS, `t/housekeeping_thread.t.cpp` to component test; no new `DEPENDS` edge (pool already present).
- Gate green (build + ctest + format + levelization + claims) in both dev and ASan/UBSan presets; stress test runs TSan-ready under the standing parked TSan lane (`parking-lot.md:33-41`); Document→arena rewire remains parked (`parking-lot.md:63-66`).
