# runtime.threading — Worker pool + threading structure (render dispatch + per-content serialization + async completion wake)

## TaskJuggler entry

`tasks/65-runtime.tji:8-12` → `runtime.threading` ("Worker pool + threading
structure"), the first leaf under `task runtime`. It carries no `depends` of its
own; the parent `task runtime` adds `depends compositor.tile_planning`
(`65-runtime.tji:6`), so that is its sole inherited edge. Note line:

> "Render-thread frame planning, worker pool for layer rendering, per-content
> serialization queues for non-thread-safe content, async completion plumbing.
> Doc 02."

It is the direct predecessor of `runtime.interactive` (`65-runtime.tji:13-18`,
`depends !threading, …`) and `runtime.offline_sequences`
(`65-runtime.tji:19-23`, `depends !threading, …`); those two are the milestone
leaves (`m3_still_compositor` → `runtime.interactive`,
`tasks/99-milestones.tji:28-31`; `m5_video` → `runtime.offline_sequences`,
`:40-42`). `runtime.threading` reaches those milestones transitively through its
successors — it is no milestone's last dependency, so it wires nothing new into
`99-milestones.tji`.

## Effort estimate

**3d.** The repo already has the concurrency *idiom* this task generalizes —
`runtime.housekeeping_thread` (DONE 2026-07-05) shipped the first owned
background thread with a `std::condition_variable` park/wake/stop/join lifecycle
(`src/runtime/arbc/runtime/housekeeping_thread.hpp`). It also has the whole
render request/completion *structure* already built at the contract: `Content`,
`RenderRequest`, `RenderResult`, and the thread-safe one-shot `RenderCompletion`
(`src/contract/arbc/contract/content.hpp:56-289`), plus the compositor's
async-arrival seam (`RefinementQueue`/`PendingTile`/`poll_refinements`,
`src/compositor/arbc/compositor/refinement.hpp:57-140`). This task adds **one new
mechanism** on top of those: a *pool* of worker threads (N, not 1) with a work
queue, a **per-content serialization gate**, and the **completion-wake** the
contract deliberately left as runtime policy (`content.hpp:117-118`). The ~1d
premium over `housekeeping_thread` is the multi-worker fan-out (a shared work
queue with correct wake/stop across N threads instead of one), the per-content
serialization queue, and the byte-identical **degenerate (worker_count 0) mode**
that doc 02:135-137 mandates so goldens hold whether or not real threads run. The
deliverable is one header/impl pair
(`worker_pool.hpp`/`.cpp`), a unit + stress test file, a one-line defaulted
`Content::render_thread_safe()` addition (doc-03 delta), three claims, and the
CMake wiring (no new `DEPENDS` edge — `contract` and `compositor` are already
present).

## Inherited dependencies

**Settled:**

- `compositor.tile_planning` (DONE, the parent `runtime`'s sole edge,
  `65-runtime.tji:6`) — shipped the unit of parallel work and the exact
  render-and-settle path this pool drives. From
  `src/compositor/arbc/compositor/tile_planning.hpp`:
  - **`PlannedTile`** / **`LayerTilePlan`** (`:100-122`) — planning splits a
    visible layer into a fixed local-space tile grid per scale rung (doc
    02:57-60); a tile whose fresh key misses becomes a render request. The
    **tile is the unit of work** a worker renders.
  - **`render_frame_interactive`** (`:152-199`) — the synchronous tiled driver
    that "fills a miss inline (materializing a `BestEffort` `RenderRequest`
    targeting the tile footprint and driving it through a `RenderCompletion`,
    the exact settle path `render_frame` uses)". It is explicitly
    **"Single-threaded (doc 02:135-137); the async worker pool and real deadline
    clock are `compositor.pull_service`"** (`:163-164`) — i.e. today misses are
    rendered inline; this task supplies the pool that lets that render run on a
    worker instead.
- `compositor.refinement` (DONE, transitively; a direct edge of the successor
  `runtime.interactive`) — the caller-owned async-arrival seam this pool feeds.
  From `src/compositor/arbc/compositor/refinement.hpp`:
  - **`PendingTile`** (`:72-79`) — `{TileKey, Rect, ObjectId, bytes,
    unique_ptr<Surface> surface, shared_ptr<RenderCompletion> done}`: the
    **surface and the completion are caller-owned and outlive the frame**
    ("`surface` owns the render target the async worker writes the pixels
    into … must outlive the frame that issued the request"). This is the
    lifetime contract the pool relies on — it never owns a target surface.
  - **`RefinementQueue`** (`:81-87`) and **`poll_refinements`** (`:124-140`) —
    the runtime-loop-owned registry of deferred renders and its drain: a settled
    completion is moved into a `Visible` cache insert **on the render thread**
    and emits `Damage` (claim `02-architecture#async-arrival-emits-damage`,
    `registry.tsv:88`). This seam is what keeps every cache mutation
    single-threaded even though renders run on workers (see Decisions).
- `contract` (L3, DONE) — the render contract the pool moves across threads,
  from `src/contract/arbc/contract/content.hpp`:
  - **`RenderRequest`** (`:76-84`) — `{Rect region, double scale, Time, StateHandle
    snapshot, Surface& target, Exactness, Deadline}`, a cheap trivially-copyable
    by-value descriptor that "carries everything the layer needs" (doc 02:127).
    `snapshot` is an index-only slab handle (no refcount touch), so a worker
    reads frozen state lock-free (`:65-75`, doc 14:159-162).
  - **`RenderCompletion`** (`:101-151`) — "**A thread-safe, one-shot completion
    handle.** `complete`/`fail` may run on a renderer thread while
    `cancel`/`cancelled`/`take`/`settled` run on the caller thread. The
    settlement state is published with release/acquire ordering after the
    payload is written." Settles **exactly once** (claim
    `03-layer-plugin-interface#render-completion-settles-once`,
    `registry.tsv:56`, already exercises concurrent complete/cancel from
    different threads). Crucially: "**How the caller is *woken* on completion
    (condvar/eventfd) is runtime policy and is out of scope**" (`:117-118`) —
    **this task is that wake.**
  - **`Content::render`** (`:229-230`) — "One entry point, sync and async
    unified … Return a `RenderResult` to settle INLINE; return `nullopt` to
    answer ASYNCHRONOUSLY" (claim `03-layer-plugin-interface#render-inline-or-async`,
    `registry.tsv:54`). A worker calls exactly this.
  - **`Deadline`** (`:56-63`) — "The contract carries the *value only* — reading
    the clock and enforcing the deadline is runtime policy (doc 17:39-41), so
    this type exposes no `now()`/`expired()`." The pool carries the value; it
    reads no clock (see Decisions).
- `runtime.housekeeping_thread` (DONE 2026-07-05,
  `src/runtime/arbc/runtime/housekeeping_thread.hpp`) — not a WBS edge, but the
  **proven concurrency template** this task follows: a `std::mutex` +
  `std::condition_variable` loop that `wait_for(period, predicate = stop ||
  poke)`, wakes on timeout *or* explicit `poke()`, stops via cv notify (not a
  timeout), runs a final drain on the stop path, and `join()`s in the
  destructor — "No detach, no `std::terminate` on a live thread, no hang." Its
  stress test (`src/runtime/t/housekeeping_thread.t.cpp:285-330`) — writer
  partitions disjoint blocks to N producers spin-waiting on an atomic `go`,
  `std::this_thread::yield()` at fixed-stride perturbation points, outcome-only
  assertions — is the idiom this task's stress test mirrors.

**Pending:** none — every predecessor is landed.

## What this task is

Deliver **`arbc::runtime::WorkerPool`** (L5, doc 17:60), the concurrency
substrate for layer rendering: the mechanism that turns "call `content->render`
inline on the render thread" into "dispatch the render to a worker, serialize
the ones that must not run concurrently, and wake the render thread when a result
settles." In a new header/impl `src/runtime/arbc/runtime/worker_pool.hpp`
(+ `worker_pool.cpp`):

1. **`struct RenderTask`** — the unit of dispatched work: `{ Content* content;
   RenderRequest request; std::shared_ptr<RenderCompletion> done; }`. The
   `RenderRequest` (holding `Surface& target`) and the `done` completion are
   **caller-owned** (the compositor's `PendingTile` owns the surface + the
   completion, `refinement.hpp:65-79`); the task references them and the pool
   stores no surface across the render.
2. **`struct WorkerPoolConfig`** — thread-lifecycle knobs, all defaulted:
   - `std::size_t worker_count` — number of worker threads. **`0` selects the
     degenerate inline executor** (doc 02:135-137): `submit` runs the render on
     the calling thread and returns, preserving the request/completion structure
     with no thread. Default: `0` (so the walking-skeleton path is byte-identical
     to `render_frame_interactive`'s inline fill until a caller opts into real
     threads).
   - `std::function<bool(const Content*)> serialize_predicate` — the
     serialization signal. Default: `[](const Content* c){ return c &&
     !c->render_thread_safe(); }`, reading the new `Content::render_thread_safe()`
     declaration (doc-03 delta below). Injectable so tests drive the
     serialization gate with a stub content without depending on a real
     non-thread-safe kind.
3. **`class WorkerPool`** — owns the `worker_count` `std::thread`s (started last
   in the ctor), a `std::mutex`, a `std::condition_variable` for the work queue,
   a second `std::condition_variable` for completion wake, the shared FIFO work
   queue, the per-content serialization map, the stop flag, and behavioral
   counters. Non-copyable, non-movable (owns threads + mutexes):
   - **Constructor** `(WorkerPoolConfig config)` — launches the workers (or none,
     for inline mode).
   - **Destructor** — `request_stop()` then `join()` all workers; drains no cache
     (there is none to drain — see Decisions) but lets in-flight renders finish
     and refuses to enqueue new work after stop.
   - **`void submit(RenderTask task)`** — enqueue. If
     `serialize_predicate(task.content)` is true and that content already has an
     in-flight render, the task is parked on the content's **per-content queue**
     and released when the in-flight one settles; otherwise it goes on the shared
     queue for any worker. In inline mode (`worker_count == 0`) it runs
     immediately on the calling thread (still honoring the per-content gate: a
     re-entrant submit for an already-in-flight serialized content parks and runs
     after).
   - **`void poke() noexcept`** — the async-completion wake handle: any settler
     (a worker after an inline-in-worker settle, or externally-async content on
     its own thread) calls this to wake a render thread parked in
     `wait_completions`. This is the runtime-owned wake `content.hpp:117-118`
     defers.
   - **`bool wait_completions(std::optional<std::chrono::steady_clock::time_point>
     until)`** — block the calling (render) thread until at least one dispatched
     completion has settled since the last drain, or `until` elapses (nullopt =
     no timeout); returns whether a completion is ready. Waits on a *condition*
     (a settled-count advance), never a fixed sleep. The frame loop parks here
     between `submit`-ing misses and calling `poll_refinements`.
   - **`void request_stop() noexcept`** — idempotent; sets the stop flag and
     wakes all workers (join happens in the destructor).
   - **`std::uint64_t tasks_submitted() const noexcept`**,
     **`tasks_completed() const noexcept`**, and
     **`max_in_flight_per_content() const noexcept`** (the high-water mark of
     concurrent renders for any single serialized content — the observable that
     pins "≤ 1" for serialized content) — caller-readable `std::uint64_t`
     behavioral counters (doc 16:54-62), no wall clock.
4. **The worker loop** (`run()`, in the `.cpp`): park on the work-queue cv with
   a `stop || !queue.empty()` predicate; on wake, if stopping break, else pop one
   ready task (skipping tasks whose serialized content is currently in-flight,
   which stay parked on their per-content queue), mark its content in-flight if
   serialized, call `task.content->render(task.request, task.done)`. If `render`
   returns a `RenderResult`, fold it into `task.done->complete(result)` (the
   inline-in-worker settle, unifying sync and async on one settle path,
   `content.hpp:105-108`); if it returns `nullopt` the content settles `done`
   later off-thread. Either way, clear the content's in-flight mark (releasing its
   next queued task) once the render *call* returns, bump `tasks_completed` when
   the completion settles, and `poke()` the completion cv.

5. **`Content::render_thread_safe()` (doc-03 delta).** A one-line defaulted
   `virtual bool render_thread_safe() const { return true; }` added to `Content`
   (`src/contract/arbc/contract/content.hpp`), realizing doc 02:126-130's
   promised declaration. Default true keeps every existing walking-skeleton
   content byte-identical; a content whose renderer is not internally thread-safe
   overrides it to `false` and the pool serializes it. See the delta under
   Decisions.

**Not this task:**

- **The interactive frame loop that drives the pool** — collect damage → plan
  (`render_frame_interactive`) → `submit` the misses → `wait_completions` to the
  frame deadline → `poll_refinements` → composite → schedule the follow-up frame.
  That is `runtime.interactive` (`65-runtime.tji:13-18`, `depends !threading`),
  which owns the frame-to-frame `RefinementQueue`, the transport clock, and the
  **deadline enforcement** (reading `steady_clock` to cancel expired `BestEffort`
  work via `RenderCompletion::cancel`, `content.hpp:122-123`). The pool carries
  the `Deadline` value on each task and reads no clock (`content.hpp:50-55`, doc
  17:78-80).
- **The concrete `PullService`** — the cache-first request machinery that
  "pulls hit the cache first, schedule on workers, inherit deadline + snapshot"
  is `compositor.pull_service` (L4, `35-compositor.tji:41-46`,
  `depends !tile_planning, !operator_graph` — *not* on threading). It is
  constructed with an **injected dispatch seam** the runtime binds to this pool
  at wiring time (a plain `std::function`/callable over `WorkerPool::submit`, so
  no L4→L5 edge and no new contract type). Defining that injection point and its
  cache/budget policy is pull_service's/`runtime.interactive`'s concern; this
  task only exposes `submit`.
- **Cache concurrency.** Workers render into caller-owned target surfaces and
  settle completions; they **never touch the `KeyedStore`/`TileCache`**. All
  cache inserts stay on the render thread via `poll_refinements`
  (`refinement.hpp:124-140`). This deliberately keeps `KeyedStore`
  single-thread-confined, so the parked "harden `KeyedStore` for concurrent
  access" question (`parking-lot.md:38-41`) stays **parked, not triggered** — a
  design choice, not a deferral (see Decisions).
- **RT-safety enforcement** (RealtimeSanitizer / `[[clang::nonblocking]]` on any
  callback chain) — that is `audio.rt_safety`'s (`45-audio.tji:51`). This pool's
  workers are ordinary threads; the RT constraint applies only to the audio
  callback, which does not run here.
- **Audio worker rendering** (the mix-block worker pipeline) — that is
  `audio.*` (`45-audio.tji:21`), which will reuse this same `WorkerPool` or an
  analogous one against `AudioRequest`. Not this task.

## Why it needs to be done

`compositor.tile_planning` shipped `render_frame_interactive` filling misses
**inline on the render thread** and marked its own limit: "Single-threaded (doc
02:135-137); the async worker pool … [is] `compositor.pull_service`"
(`tile_planning.hpp:163-164`). Doc 02:126-132 designs the escape from that limit
— "**Layer rendering** runs on a worker pool … layer implementations declare
whether they are internally thread-safe or need serialization (the core provides
a per-content queue for the latter)" — and the contract was built async-ready for
exactly this: `RenderCompletion` is thread-safe with release/acquire settlement,
and `content.hpp:117-118` explicitly leaves the **completion wake** as "runtime
policy … out of scope." So the request/completion *structure* exists on both
sides (compositor emits `PendingTile`s into a `RefinementQueue`; content settles a
thread-safe `RenderCompletion`) but **nothing yet moves a render off the render
thread, serializes the content that can't be parallelized, or wakes the loop when
a worker finishes.** This task is that missing middle: the worker pool that
renders tiles in parallel, the per-content queue that keeps a non-thread-safe
renderer to one in-flight call, and the condition-variable wake that lets the
render thread park until results settle instead of busy-spinning.
`runtime.interactive` and `runtime.offline_sequences` both `depend !threading`
because a responsive frame loop cannot render every visible miss inline within a
frame deadline — it must fan the misses out to workers and reap the ones that
land in time (doc 02:61-71). Building the pool as its own leaf, ahead of the frame
loop, keeps the concurrency substrate independently testable (unit + TSan stress)
before it is wired into a live renderer.

## Inputs / context

- `docs/design/02-architecture.md`:
  - **`:118-137`** (`## Threading model`) — the normative source. Scene model
    single-writer (`:120-122`); compositor plans on the render thread under a
    pinned snapshot, "never races edits and never takes a lock" (`:123-125`);
    **"Layer rendering runs on a worker pool. Requests carry everything the layer
    needs … layer implementations declare whether they are internally thread-safe
    or need serialization (the core provides a per-content queue for the latter).
    Layers whose rendering is inherently external … integrate through the async
    completion path rather than occupying a worker"** (`:126-132`); compositing on
    the render thread (`:133`); **"v1 may degenerate to 'everything on one thread'
    while keeping the request/completion structure"** (`:135-137`) — the
    worker_count-0 mode.
  - **`:49-71`** (`## The frame, interactively`) — the six-step loop the pool
    serves: step 3 splits into tiles (the unit of work); step 4 "Cache misses
    become render requests with a deadline. Layers may answer synchronously, or
    asynchronously"; step 6 "Async results that arrive later produce damage …
    scheduling a follow-up frame."
  - `:100-107` (`## Tile cache`) — the residency pin "must not be evicted
    mid-frame," distinct from the backend-pool refcount — the reason cache
    mutation stays render-thread-only.
- `docs/design/03-layer-plugin-interface.md`:
  - **`:117-121`** — "One render entry point, sync and async unified … The
    compositor treats 'returned inline' as an immediately-completed async
    request; there is one code path" — the settle path a worker drives.
  - `:122-123` — `cancelled()` is cooperative/best-effort (deadline cancellation
    is the frame loop's, not the pool's).
  - **The doc-03 delta this task lands** — `render_thread_safe()` added to the
    `Content` sketch (`:80-85`) and a `Points worth calling out` bullet
    (`:122`), realizing doc 02:126-130's declaration.
- `docs/design/16-sdlc-and-quality.md`:
  - `:15-25` — the claims register (`<doc-stem>#<slug>` + `// enforces:` tag; CI
    fails a registered claim with no live test).
  - **`:54-62`** — behavioral-counter tests, never wall-clock: assert via
    `tasks_completed()`/`max_in_flight_per_content()` and live counters,
    synchronize via `wait_completions`/atomic flags, never sleep.
  - **`:66-73`** — concurrency tests: "TSan on the full suite; dedicated stress
    tests … with schedule perturbation (randomized yields under a seed)." The
    worker pool is a first-class concurrency surface — this is the mandate its
    stress test answers.
  - `:112-118` — ≥90% diff coverage on changed lines.
- `docs/design/17-internal-components.md`:
  - **`:60`** — `arbc::runtime` is **L5**, contents include the "interactive
    frame loop, offline/export drivers, … housekeeping thread"; "Depends on:
    **everything below**." A runtime-owned worker pool sits here alongside the
    frame loop and the housekeeping thread.
  - `:53` / `:56` — `contract` (L3) owns `PullService` *interface*,
    requests/results, `RenderCompletion`; `compositor` (L4) owns the
    `PullService` *implementation*. Levelization forbids L4 depending on L5, so
    the pull_service dispatch seam is injected, not a direct call up.
  - **`:78-80`** — "The two render drivers live in `runtime`, not the engines …
    deadlines, frame loops, and device clocks are runtime policy." The worker
    pool is that runtime policy; deadline *enforcement* rides the frame loop.
  - `:41-44` — depend only on strictly lower levels; the CI dependency check
    validates CMake + include graph. No new edge (`contract`, `compositor` are
    already in runtime's `DEPENDS`).
- `src/contract/arbc/contract/content.hpp:56-289` — `Deadline`, `RenderRequest`,
  `RenderResult`, the thread-safe one-shot `RenderCompletion` (with its
  `k_pending→k_claimed→k_published→k_taken` release/acquire protocol,
  `:138-151`), `Content::render` (`:229-230`), and the wake-is-runtime-policy
  note (`:117-118`).
- `src/compositor/arbc/compositor/tile_planning.hpp:100-199` — `PlannedTile`,
  `LayerTilePlan`, and `render_frame_interactive` (the inline single-threaded
  driver naming the pool as future work, `:163-164`).
- `src/compositor/arbc/compositor/refinement.hpp:57-140` — `PendingTile`
  (caller-owned surface + completion), `RefinementQueue`, `poll_refinements`
  (render-thread cache insert + damage emit; claims
  `02-architecture#async-arrival-emits-damage` `registry.tsv:88`,
  `02-architecture#quiescent-refinement-schedules-no-frame` `:89`).
- `src/runtime/arbc/runtime/housekeeping_thread.hpp` and
  `src/runtime/t/housekeeping_thread.t.cpp:285-330` — the park/wake/stop/join
  lifecycle and the concurrent-stress idiom (writer partitions disjoint blocks,
  producers spin on atomic `go`, fixed-stride `yield()` perturbation, outcome-only
  asserts) this task mirrors.
- `src/pool/t/reclamation.t.cpp:248-303`, `src/pool/t/free_pools.t.cpp:237-293` —
  the older concurrent-stress models these idioms descend from.
- `src/runtime/CMakeLists.txt:1-8` — `DEPENDS base model contract compositor
  pool` (contract + compositor already present); this task appends
  `worker_pool.cpp` to `SOURCES`, `arbc/runtime/worker_pool.hpp` to
  `PUBLIC_HEADERS`, and `t/worker_pool.t.cpp` to the component test.
- `CMakePresets.json:32-40` — the **`tsan` preset**
  (`ARBC_SANITIZERS: thread`) and its `tsan` build/test presets (`:71`, `:80`)
  **exist today** — the stress test runs under `ctest --preset tsan` as a real
  gate (this is no longer parked infra, unlike when `housekeeping_thread`
  landed).
- `tests/claims/registry.tsv` — `:54` `#render-inline-or-async`, `:56`
  `#render-completion-settles-once`, `:88` `#async-arrival-emits-damage` (claims
  this task re-exercises); this task appends three rows.
- `tasks/parking-lot.md:38-41` — the `KeyedStore` concurrency-hardening question
  whose trigger is "the async off-thread fill path landing"; this task's design
  keeps it untriggered (workers never touch the cache).

## Constraints / requirements

- **Levelization (doc 17:41-44,:60).** `WorkerPool` is L5 `arbc::runtime`,
  reaching the render contract only through public `contract` headers
  (`Content`, `RenderRequest`, `RenderCompletion`) and the compositor's public
  types where needed, exactly as the other runtime code does. **No new `DEPENDS`
  edge** — `contract` and `compositor` are already present. The pull_service
  dispatch is injected downward (a `std::function` bound at runtime wiring), so
  no L4→L5 dependency is introduced. The CI dependency check stays green.
- **Degenerate single-thread mode is byte-identical (doc 02:135-137).**
  `worker_count == 0` must run every submitted render on the calling thread and
  produce results indistinguishable — same settlement, same pixels — from the
  same requests run inline by `render_frame_interactive` today. This is what lets
  the request/completion structure land now while real threading "arrives as an
  optimization rather than a redesign."
- **Per-content serialization, one in-flight (doc 02:126-130).** For content
  where `serialize_predicate` is true (`render_thread_safe() == false`), the pool
  must guarantee **at most one concurrent `render` call for that content**;
  concurrent submissions queue FIFO and release one at a time as each settles.
  Thread-safe content (the default) renders with full pool concurrency. The gate
  is observable via `max_in_flight_per_content()` (≤ 1 for serialized content).
- **Completion wake without busy-spin (`content.hpp:117-118`).** The render
  thread parks in `wait_completions` on a *condition* (a settled-count advance),
  woken by `poke()` from any settler; it never polls in a sleep loop. A pool with
  no submitted work wakes no worker and settles nothing — a quiescent substrate
  is idle, not spinning (behavioral, not timing).
- **No cache access from workers.** Workers write only into caller-owned target
  surfaces and settle caller-owned completions; they never call into
  `KeyedStore`/`TileCache`. Cache inserts remain render-thread-only via
  `poll_refinements`. (Keeps `parking-lot.md:38-41` untriggered.)
- **Caller owns surface + completion lifetime (`refinement.hpp:65-79`).** The
  pool holds a `Content*`, a by-value `RenderRequest` (whose `Surface&` refers to
  the caller's `PendingTile::surface`), and a `shared_ptr<RenderCompletion>`; it
  allocates and outlives no render target. A submitted task must not outlive its
  `PendingTile` — enforced by the frame loop's discipline (the pool drains before
  the loop drops pending surfaces), documented at the seam.
- **Clock-free pool; deadline is a carried value (doc 17:78-80,
  `content.hpp:50-55`).** The pool carries each task's `Deadline` value but reads
  no clock and enforces no deadline; deadline enforcement (cancelling expired
  `BestEffort` work) is `runtime.interactive`. `wait_completions`' optional
  timeout is a park bound, never a deadline decision, and never appears in a test
  assertion.
- **Graceful stop/join, no hang (mirrors `housekeeping_thread`).**
  `request_stop()` wakes all parked workers via cv notify; the destructor
  `join()`s every worker; in-flight renders finish; post-stop `submit` is a
  no-op/refusal. No detach, no `std::terminate` on a live thread, no hang.
  Constructing a pool and immediately destroying it (no work) returns cleanly.
- **First multi-thread pool in the engine — TSan/stress obligation (doc
  16:66-73).** This is concurrency-touching by construction; it ships a dedicated
  stress test with schedule perturbation and **runs under the existing `tsan`
  preset** as a real gate (not deferred — the preset exists,
  `CMakePresets.json:32-40`).
- **Public header compiles standalone;** CI diff coverage ≥90% (doc 16:112-118).

## Acceptance criteria

- **Unit tests — deterministic, no sleep — in `src/runtime/t/worker_pool.t.cpp`
  (new, Catch2, registered via `arbc_component_test`).** With a small stub
  `Content` (a solid-fill `render` that records the calling thread id and a
  per-content concurrency counter, and whose `render_thread_safe()` the test
  overrides), and caller-owned target `Surface`s + `shared_ptr<RenderCompletion>`s
  mirroring `PendingTile`:
  - **Inline degenerate mode is byte-identical:** with `worker_count == 0`,
    `submit` a request whose stub `render` fills a known pattern; assert the
    completion settled on the **calling** thread, `take()` yields the result once,
    and the target pixels are byte-identical to calling `content->render` directly
    (the pre-pool inline path). A second case submits an async stub (returns
    `nullopt`, settles later via the test's held `done`) and drives it through
    the same `submit → wait_completions → take` path, asserting equivalent
    settlement — re-enforcing `03-layer-plugin-interface#render-inline-or-async`.
  - **Real pool renders and wakes:** with `worker_count == 4`, submit K
    thread-safe renders; the test thread parks in `wait_completions(nullopt)` and
    drains settled completions until `tasks_completed() == K`, asserting every
    target got its pattern, every completion settled exactly once, and that
    renders ran on **worker** threads (thread-id ≠ the test thread). Synchronized
    on the completion counter — no sleep.
  - **Per-content serialization holds one in-flight:** submit many renders for a
    single content whose `render_thread_safe()` is false (its stub `render`
    increments a shared concurrency counter on entry, spins briefly under a
    perturbation yield, decrements on exit) interleaved with renders for
    thread-safe contents, over a `worker_count == 8` pool; after draining assert
    `max_in_flight_per_content()` for the serialized content is **exactly 1**
    (never observed > 1), the thread-safe contents ran concurrently (high-water
    > 1), and every submission settled exactly once — enforcing the new claim.
  - **Graceful stop + no hang:** submit work, `request_stop()`, let the
    destructor join; assert every already-settled completion is intact, post-stop
    `submit` did not enqueue (a submitted-then-stopped task's completion is left
    unsettled or cancelled, never lost to UB), and the destructor returned. A
    second case constructs and immediately destroys a `worker_count == 4` pool
    with no work and asserts the destructor returns (join did not hang).
  - **Quiescent substrate is idle:** a pool with no submitted work advances
    `tasks_completed()` by 0 and `wait_completions(now + short-bound)` returns
    false without a busy loop — asserted via the counter and the return value, not
    timing.
- **Stress test — concurrency, outcome assertions only, runs under `tsan` —
  same file, guarded like the pool concurrent tests.** Mirroring
  `housekeeping_thread.t.cpp:285-330` /
  `reclamation.t.cpp:248-303`: a fixed set of caller-owned targets/completions is
  partitioned; producer (render-thread-surrogate) threads spin-wait on an atomic
  `go` then `submit` their block — a mix of thread-safe contents and one
  serialized (non-thread-safe) content shared across producers — into a
  `worker_count == N` pool, while a consumer thread loops
  `wait_completions`/drain. Drive a **fixed operation count** (not a duration)
  with light **schedule perturbation** (a `std::this_thread::yield()` at
  fixed-stride points inside the stub render and the submit loop; a seeded-RNG
  yield variant is an acceptable enhancement). After the op count, `request_stop`,
  join, and assert **outcomes only**: no crash/hang; every submitted completion
  settled **exactly once** (a settled-count equal to the submit count, none lost
  or double-settled); and the serialized content's `max_in_flight_per_content()`
  stayed **1** throughout. **No timing assertion.** Runs green under dev +
  ASan/UBSan and under **`ctest --preset tsan`**, which exercises the
  submit/render/settle and producer/consumer race windows and the serialization
  gate for data races.
- **Claims (register + `enforces:` tags)** appended to `tests/claims/registry.tsv`
  (format `<claim-id><TAB><description>`, `registry.tsv:1`), enforced from the
  tests above:
  - `02-architecture#worker-pool-degenerates-to-inline` — "A worker pool with
    zero workers runs every submitted render on the calling thread and settles its
    `RenderCompletion` on that thread, producing pixels and a settlement
    byte-identical to running the same `RenderRequest` inline; the request and
    completion structure is preserved so real workers change performance, not
    results." (doc 02:135-137) — enforced by the inline-degenerate-mode test.
  - `02-architecture#worker-pool-serializes-non-thread-safe-content` — "For
    content declaring `render_thread_safe() == false` the pool renders at most
    one request for that content at a time (a per-content FIFO), while
    thread-safe content renders concurrently; under many interleaved concurrent
    submissions the serialized content's in-flight render count never exceeds one
    and every submission settles exactly once." (doc 02:126-130) — enforced by
    the serialization unit test and the stress test.
  - `02-architecture#worker-pool-stops-gracefully` — "Requesting the worker pool
    to stop wakes every parked worker, lets in-flight renders finish, refuses new
    submissions, and joins all workers cleanly with no hang and no lost or
    double-settled completion." (doc 02:135-137, `content.hpp:117-118`) —
    enforced by the graceful-stop + no-hang tests.
  - The inline/async equivalence test additionally re-enforces the predecessor's
    `03-layer-plugin-interface#render-inline-or-async` (`registry.tsv:54`) through
    the pool's `submit` path (a second `// enforces:` tag on the existing claim —
    no new row).
- **Doc-03 delta landed.** `docs/design/03-layer-plugin-interface.md` gains the
  `render_thread_safe()` declaration in the `Content` sketch and a
  `Points worth calling out` bullet, realizing doc 02:126-130; the implementer
  adds the matching one-line defaulted virtual to
  `src/contract/arbc/contract/content.hpp`. Default `true`, so every existing
  content is byte-unchanged and no existing golden or conformance case shifts.
- **Behavioral-counter discipline (doc 16:54-62).** Every assertion is on a live
  count, a `RenderCompletion::settled()`/`take()` result, or a pool counter
  (`tasks_completed()`, `max_in_flight_per_content()`); synchronization is via
  `wait_completions` (completion-count condition) and atomic `go`/`stop` flags.
  No test reads a wall clock or sleeps to synchronize.
- **Component wiring & CI dependency check:** `src/runtime/CMakeLists.txt` adds
  `worker_pool.cpp` to `SOURCES`, `arbc/runtime/worker_pool.hpp` to
  `PUBLIC_HEADERS`, and `t/worker_pool.t.cpp` to the component test; **no
  `DEPENDS` change** (`contract`, `compositor` already present); the header
  compiles standalone; the doc-17 dependency check passes.
- **Gate green (build + tests in dev + ASan/UBSan + TSan).** The stress test runs
  green under `dev`, `asan`, **and `tsan`** presets; ≥90% diff coverage on
  changed lines.

## Decisions

- **`WorkerPool` is a runtime (L5) primitive; the pull_service dispatch is
  injected downward, not a call up.** doc 17:78-80 assigns "deadlines, frame
  loops, and device clocks" to runtime, and doc 17:60 already lists runtime's
  threads (frame loop, housekeeping thread); a worker pool for layer rendering is
  the same class of runtime policy. But `compositor.pull_service` (L4) must also
  "schedule on workers" (`35-compositor.tji:45`) and cannot depend on L5. The
  standard dependency inversion resolves it: the pool lives in runtime; the
  concrete `PullService` is **constructed with an injected callable** (bound to
  `WorkerPool::submit`) at runtime wiring time. *Rejected:* introducing a new
  contract-level (L3) `Executor`/`RenderScheduler` interface that both name — a
  speculative seam with one caller today; a `std::function` injection is the
  simpler abstraction and adds no contract type. *Rejected:* putting the pool in
  the compositor (L4) — that would make the compositor own threads and clocks,
  contradicting doc 17:78-80's "engines are libraries over pinned versions" and
  doc 02:123-125's lock-free stateless-planner rule. The loose compositor comment
  "the async worker pool … [is] `compositor.pull_service`"
  (`tile_planning.hpp:163-164`) is the compositor's *local* vantage — pull_service
  owns the *decision* to dispatch and the cache/budget policy; the *thread pool
  primitive itself* is runtime's, exactly as doc 17 levelizes it.
- **Workers never touch the cache; all cache mutation stays on the render thread
  — this keeps the parked `KeyedStore` hardening untriggered.** The compositor's
  `PendingTile`/`RefinementQueue`/`poll_refinements` seam already inserts async
  arrivals into the cache **on the render thread** (`refinement.hpp:124-140`). By
  having workers render only into caller-owned surfaces and settle completions —
  and leaving the cache insert to the render thread's `poll_refinements` — the
  `KeyedStore` stays single-thread-confined. That is precisely the precondition
  the parked item `parking-lot.md:38-41` ("harden `KeyedStore` for concurrent
  access … when the async off-thread fill model is decided") waits on: **this
  task decides the async fill model as *render-into-surface, insert-on-render-
  thread*, which does not require a concurrent cache.** *Rejected:* letting
  workers insert directly into the cache — that would trigger the parked hardening
  (reader-safe lookup + synchronized insert/evict + its own TSan stress) and pay
  synchronization cost on the cache hot path, for no benefit the poll seam does
  not already deliver. The parking-lot item stays parked (surfaced in the return
  summary as *addressed-by-design*, not resolved-by-fiat).
- **`worker_count == 0` inline mode is the byte-identical default (doc
  02:135-137).** Doc 02 mandates that v1 "may degenerate to 'everything on one
  thread' while keeping the request/completion structure." Making the *default*
  config inline means landing this task changes no observable behavior in any
  existing caller or golden — `render_frame_interactive` keeps filling misses
  inline until a caller explicitly asks for `worker_count > 0`. The pool's value
  lands now (the structure, the serialization gate, the wake); real threads switch
  on as an optimization, exactly as doc 02 wants. *Rejected:* defaulting to a
  hardware-concurrency pool — that would flip every existing test onto real
  threads immediately and couple this leaf's landing to `runtime.interactive`'s
  loop being ready to reap completions; the inline default decouples them.
- **Per-content serialization keys on a `Content` declaration
  (`render_thread_safe()`), not an injected-only predicate — doc-03 delta.** Doc
  02:126-130 says "layer implementations *declare*" their thread-safety; the
  declaration belongs on the contract. Adding one defaulted
  `virtual bool render_thread_safe() const { return true; }` to `Content`
  realizes that promise with a permanent, honest signal every kind can set, and
  makes the serialization queue testable against a real override rather than a
  throwaway predicate. Default true keeps all existing content byte-identical.
  The `serialize_predicate` config still exists (defaulting to read the
  declaration) so tests can drive the gate with a stub and a future caller can
  override policy, but the *source of truth* is the contract method.
  *Rejected:* an injected-predicate-only approach with no contract change — it
  leaves the doc-02-promised declaration unrealized, forces a synthetic signal,
  and would need re-plumbing when the real flag lands. *Rejected:* serializing
  *all* content by default (conservative, no contract change) — that throws away
  the cross-tile parallelism that is the entire point of the pool. This is a
  contract-surface addition, so it rides a **doc-03 delta**
  (`docs/design/03-layer-plugin-interface.md`, the `Content` sketch + a
  `Points worth calling out` bullet); no doc-00 bullet, because it realizes an
  existing doc-02 architectural decision rather than making a new project-shaping
  one.
- **The completion wake is a runtime-owned condvar (`poke()`/`wait_completions`),
  mirroring `housekeeping_thread`.** `content.hpp:117-118` explicitly leaves the
  wake ("condvar/eventfd") to runtime. Reusing the proven
  park/wake/stop/join idiom from `housekeeping_thread` — a `condition_variable`
  the render thread waits on, `poke()`d by any settler — is the correct, minimal
  wake: inline-in-worker settles poke automatically, and truly-external content (a
  3D engine settling on its own thread) is handed the same `poke` so a quiescent
  frame still wakes on its arrival. *Rejected:* an eventfd/OS-handle wake — more
  portable-surface cost for no v1 benefit; a condvar is what the rest of the
  engine already uses. *Rejected:* busy-polling `settled()` in a sleep loop —
  a wall-clock dependency doc 16:54-62 forbids and a wasted-CPU regression.
- **The pool is clock-free; deadline enforcement is the frame loop's.** `Deadline`
  "carries the *value only* … enforcing the deadline is runtime policy"
  (`content.hpp:50-55`), and doc 17:78-80 puts device clocks in the frame loop.
  The pool carries each task's `Deadline` untouched and reads no clock;
  `runtime.interactive` reads `steady_clock`, cancels expired `BestEffort` work
  via `RenderCompletion::cancel` (advisory, `content.hpp:122-123`), and proceeds
  with the degraded fallback (claim `02-architecture#degraded-fallback-preference-order`).
  *Rejected:* having the pool skip expired queued tasks by reading a clock — it
  would duplicate the frame loop's deadline authority and put a clock in a
  library-level primitive, muddying the clean "value in the contract, clock in the
  runtime loop" split; keeping the pool clock-free also keeps its tests
  wall-clock-free.
- **Stress asserts outcomes, mirrors the existing concurrent idiom, runs under
  the live `tsan` preset.** The worker pool is a named concurrency surface (doc
  16:66-73). Following `housekeeping_thread.t.cpp`'s pattern — partition, atomic
  `go`, fixed-stride `yield()` perturbation, join, outcome-only asserts
  (settled-count == submit-count, `max_in_flight_per_content() == 1`) — reuses a
  proven, correct shape and keeps assertions behavioral, not timing. Unlike when
  `housekeeping_thread` landed, the `tsan` preset now exists
  (`CMakePresets.json:32-40`), so this test **gates under TSan directly** — no
  parking. *Rejected:* wall-clock-bounded stress ("run 2s") — flaky; a fixed op
  count is deterministic.

## Open questions

(none — all decided.)

One standing item this task **addresses by design rather than resolves**, surfaced
in the return summary for the human's awareness (not encoded as a WBS leaf): the
parked `KeyedStore` concurrency-hardening question (`parking-lot.md:38-41`). Its
trigger is "the async off-thread fill path landing"; this task lands that path but
*chooses a fill model (render-into-caller-surface, insert-on-render-thread) that
keeps `KeyedStore` single-thread-confined*, so the parked item stays untriggered.
If a future task ever moves cache inserts onto workers, that item re-activates —
but nothing here forces it, and there is no WBS leaf to add.

## Status

**Done** — 2026-07-06.

- Delivered `arbc::runtime::WorkerPool` (L5) in new `src/runtime/arbc/runtime/worker_pool.hpp` + `src/runtime/worker_pool.cpp`: inline-degenerate default (`worker_count==0`, byte-identical to inline fill), N-worker shared FIFO with per-content serialization gate (≤1 in-flight for `render_thread_safe()==false`), condvar completion wake (`poke()`/`wait_completions()`), graceful stop/join.
- Added `Content::render_thread_safe()` defaulted virtual to `src/contract/arbc/contract/content.hpp`; default `true` keeps all existing content byte-identical.
- Doc-03 delta landed: `docs/design/03-layer-plugin-interface.md` gains `render_thread_safe()` declaration in the `Content` sketch and a `Points worth calling out` bullet.
- Unit and stress tests in `src/runtime/t/worker_pool.t.cpp`: inline-degenerate/byte-identical, real-pool-wakes, per-content serialization, graceful-stop+no-hang, quiescent-idle, plus producer/consumer stress with fixed op count and schedule perturbation; TSan-clean (0 data races).
- Registered claims in `tests/claims/registry.tsv`: `02-architecture#worker-pool-degenerates-to-inline`, `02-architecture#worker-pool-serializes-non-thread-safe-content`, `02-architecture#worker-pool-stops-gracefully`; added a second `enforces:` tag on existing `03-layer-plugin-interface#render-inline-or-async`.
- CMake wiring in `src/runtime/CMakeLists.txt`: `worker_pool.cpp` to `SOURCES`, `arbc/runtime/worker_pool.hpp` to `PUBLIC_HEADERS`, `t/worker_pool.t.cpp` to component test; no new `DEPENDS` edge.
- Fixed `AsyncStub` test helper to copy `RenderRequest` by value instead of retaining a dangling pointer (was a hard segfault).
- `KeyedStore` concurrency-hardening (`parking-lot.md:38-41`) stays **addressed-by-design, not resolved** — workers render only into caller-owned surfaces; cache inserts remain render-thread-only, so the parked item stays untriggered.
