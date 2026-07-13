# runtime.shared_worker_pool — Let several `InteractiveRenderer`s share one `WorkerPool`

## TaskJuggler entry

[`tasks/65-runtime.tji:160-165`](../../65-runtime.tji) — `task shared_worker_pool "Let
several InteractiveRenderers share one WorkerPool"`, milestone `m9_release`.

> `InteractiveRenderer` owns its `WorkerPool` by value and is non-movable
> (interactive.hpp:154,:270), so K viewports spawn K×N threads. Add a non-owning
> constructor overload taking `WorkerPool&` (and a `std::variant`/pointer member for
> owned-vs-borrowed), so a host builds one pool and hands it to every viewport.
> Requires its own teardown/TSan story. Source-of-debt:
> `tasks/refinements/runtime/interactive_worker_count_default.md` A9. Docs 02/17.

(The note's `interactive.hpp:154,:270` line refs are stale. The live anchors are
`interactive.hpp:139-144` for the per-renderer-pool comment and `interactive.hpp:363`
for the `WorkerPool d_pool;` member.)

## Effort estimate

`2d`. Unchanged. The seam is small — one member, one constructor overload, one
destructor — but the two invariants underneath it (the completion wake and the
teardown drain) are both currently written *assuming exclusive ownership*, and both
have to be re-derived. The bulk of the estimate is the pool-side protocol change and
its concurrency coverage, not the renderer-side plumbing.

## Inherited dependencies

**Settled (all `complete 100`):**

- `runtime.interactive_worker_count_default` — shipped `default_interactive_pool_config()`
  (`clamp(hardware_concurrency() - 1, 1, k_max_interactive_workers)`, with
  `k_max_interactive_workers = 2`), the counters `frames_rendered()` /
  `deadline_expiries()` / `tiles_cancelled()`, and claim
  `02-architecture#interactive-default-renders-leaves-off-the-frame-thread`
  (`registry.tsv:155`). **This task is its A9**, the single deferral it registered.
- `runtime.worker_dispatch_leaf_only` — hoisted every worker submission into the one
  helper `worker_backed_dispatch(WorkerPool&)` (`src/runtime/worker_dispatch.cpp:12-25`),
  grep-enforced by `scripts/check_worker_dispatch.py`. That helper already takes the
  pool *by reference*, which is why this task's blast radius is the pool's internal
  protocol and not its submission seam.
- `runtime.deadline_cancel_retains_wanted` — rewrote the deadline sweep
  (`src/runtime/interactive.cpp:416-429`) to cancel only *unwanted* unsettled tiles and
  retain the wanted ones, adding `d_tiles_retained`. The sweep iterates `d_pending.tiles`,
  a renderer **member** — so it is already owner-scoped by construction and needs no
  change here (Constraint 6).
- `compositor.in_flight_tile_dedup` — the pending-set guard at both dispatch sites.
- `runtime.damage_router` — `DamageRouter` (`src/runtime/arbc/runtime/damage_router.hpp:17-19`)
  is the shipped type whose stated purpose is *"multiple viewports observing one
  composition simultaneously, damage propagating to every viewport independently"*. It is
  the consumer that makes this task load-bearing rather than speculative.

**Pending:** none. Every dependency is `complete 100`.

**Adjacent, deliberately not depended on:** `runtime.async_external_load` (a deferring
`AssetSource` installing an externally-loaded nested child; it settles no
`RenderCompletion` and renders inline — see Constraint 8).

## What this task is

`InteractiveRenderer` owns its `WorkerPool` by value (`interactive.hpp:363`) and is
non-copyable and non-movable (`interactive.hpp:224-227`), so a host that wants K
viewports must build K renderers and therefore K pools — *K × N* threads where one pool
of N would do. This task adds a **borrowing** constructor overload,
`InteractiveRenderer(WorkerPool&, Clock)`, so a host constructs one pool and hands it to
every viewport, and makes the pool actually correct under that sharing.

The overload itself is five lines. The work is the two invariants it breaks:

1. **The completion wake is a single pool-global cursor.** `poke()` bumps `d_settle_gen`;
   `wait_completions` returns when it advances past `d_drained_gen` and then **consumes
   it for everybody** — `d_drained_gen = d_settle_gen;` (`worker_pool.cpp:53`). With two
   renderers parked on one pool, renderer A's park consumes the generation bump produced
   by renderer B's tile settling, so B re-parks to its deadline believing nothing settled,
   spuriously expires, cancels or degrades tiles that are *sitting finished in its
   `RefinementQueue`*, and schedules a follow-up frame it did not owe. This is a
   wrong-output bug, not a performance one, and it fires the moment a second renderer
   touches the pool. The fix is a **caller-owned drain cursor**: the pool keeps the settle
   counter, each parking thread keeps its own cursor.

2. **Teardown safety comes entirely from `~WorkerPool`.** The member-order comment at
   `interactive.hpp:355-363` spells it out — `d_pending` is declared *before* `d_pool` so
   the pool destructs first and its `stop`-and-join runs "while the pending surfaces those
   workers may still be writing are all alive"; reversing the two "is a use-after-free, not
   a style choice." A **borrowed** pool is not destructed when the renderer dies, so that
   join never happens: a worker can still be inside `content->render`, writing into a
   `PendingTile`'s surface, when `d_pending` is destroyed. The fix is an explicit
   `~InteractiveRenderer` that drains **its own** submissions out of the pool — and only
   its own, because a sibling renderer's renders must keep running.

So the pool gains a per-submitter identity (an opaque owner tag on `RenderTask`), a
per-submitter drain (`drain_owner`), and a caller-owned wake cursor. The renderer gains
the overload, a `std::optional<WorkerPool>` + `WorkerPool&` member pair, and a destructor.

## Why it needs to be done

**The thread arithmetic.** `default_interactive_worker_count()` is
`clamp(hardware_concurrency() - 1, 1, 2)`, and `k_max_interactive_workers` was clamped to
`2` **specifically because of this gap** — `interactive.hpp:139-144` says so in the source,
and doc 02:325 and doc 00:293 both justify the cap with the words *"because the pool is
per-viewport."* The cap is a workaround for a missing seam. It is a cheap workaround
today (one viewport, two threads) and an expensive one the day a host opens four
viewports on a 64-core workstation.

**The correctness trap it removes.** `HostViewport` holds `InteractiveRenderer&` and does
not own it (`src/runtime/host_viewport.cpp:43`, `host_viewport.hpp:266`). Nothing today
*stops* a multi-viewport host from pointing K `HostViewport`s at one shared
`InteractiveRenderer` to avoid the K×N threads — and that would be silently, badly wrong:
`d_pending`, `d_wanted_tiles`, `d_carried_damage`, `d_prev_time`, `d_prev_camera_scale`
and the pull-identity memo are all *per-viewport* state (`interactive.hpp:365-406`).
Sharing a renderer cross-contaminates one viewport's wanted-tile set and carried damage
into another's frame. The only correct multi-viewport shape is **K renderers, 1 pool**,
and that shape does not currently exist. This task is what makes the cheap thing and the
correct thing the same thing.

**The doc already promises it.** Doc 02:327-328 states, normatively, *"A host that wants a
different pool passes one."* No such API exists: the renderer takes a `WorkerPoolConfig`
and builds the pool itself. That sentence is the seam this task lands in, and closing the
gap between it and the code is a doc-16 same-commit obligation either way (D6).

**Downstream.** `runtime.damage_router` already ships the multi-viewport fan-out. Nothing
in `m9_release`'s single-viewport image editor *blocks* on this, which is why it was A9
and not part of the predecessor — but the release ships `DamageRouter` as public surface,
and a public multi-viewport type whose only supported wiring costs K×N threads or a
data-race is a promise the release should not make.

## Inputs / context

**Design docs (normative, doc 16's same-commit rule applies):**

- `docs/design/02-architecture.md:286-339` — § Threading model, in full. Load-bearing
  clauses: *"Worker dispatch is leaf-only"* (`:301-314`), the non-zero-worker-count bullet
  (`:315-331`), the cap's stated justification *"and capped, because the pool is
  per-viewport"* (`:325`), the unfulfilled promise *"A host that wants a different pool
  passes one, and a host that wants no threads at all still gets the inline executor by
  asking for it"* (`:327-328`), and the closing paragraph on the request/completion
  structure (`:334-339`).
- `docs/design/02-architecture.md:140-235` — the deadline / in-flight-join / refinement-wave
  section, including *"A frame cancels the renders it no longer wants, not the renders it
  could not wait for"* (`:169-170`). Everything here is keyed on tiles and content, so it is
  pool-agnostic — **except** that nothing in it says what happens when one renderer's
  deadline sweep runs against a pool another renderer is also using. Constraint 6 pins that.
- `docs/design/00-overview.md:283-295` — the `interactive_worker_count_default` decision
  record, whose `:293` repeats *"the cap exists because the pool is per-viewport."*
- `docs/design/00-overview.md:211-221` — the leaf-only-dispatch decision record.
- `docs/design/17-internal-components.md:61` — `arbc::runtime` is **L5**, depends on
  "everything below", no same-level edges (`:42-45`). Both `WorkerPool` and
  `InteractiveRenderer` already live in `arbc_runtime`, so this task introduces **no new
  levelization edge**.
- `docs/design/17-internal-components.md:111-113` — *"The two render drivers live in
  `runtime`, not the engines… deadlines, frame loops, and device clocks are runtime
  policy."* Pool ownership is the same kind of policy.
- `docs/design/17-internal-components.md:114-128` — the caller-owned-state pattern:
  stateless libraries *"take a caller-owned counters struct by pointer, the same way they
  already take the `SurfacePool` and `RefinementQueue`, so the persistent value lives in
  `runtime` and the library stays stateless."* This is the house precedent the
  caller-owned drain cursor (D1) follows.
- `docs/design/16-sdlc-and-quality.md` — the claims register, the behavioral-counter rule
  (`:54-62`), the ≥90% diff-coverage gate (`:113-118`), and `:224-226`'s rule that
  wall-clock gates stay out of the merge path.

**Sources the task extends:**

- `src/runtime/arbc/runtime/worker_pool.hpp` —
  - `:44-49` the load-bearing lifetime contract: *"A submitted task must not outlive its
    backing `PendingTile`; the frame loop drains the pool before dropping pending
    surfaces."* Today that "drain" is `~WorkerPool`. This task has to supply a real one.
  - `:50-54` `struct RenderTask { Content* content; RenderRequest request;
    std::shared_ptr<RenderCompletion> done; }` — gains the owner tag (D3).
  - `:57-73` `WorkerPoolConfig` (`worker_count = 0` default = the degenerate inline
    executor).
  - `:96` `submit`, `:102` `poke`, `:104-110` `wait_completions`, `:113` `request_stop`,
    `:121` `worker_count()`, `:124-136` the counters
    (`tasks_submitted`/`tasks_completed`/`max_in_flight_per_content`).
  - `:161-164` **the bug**: `d_settle_gen` / `d_drained_gen`, *"the completion-wake
    condition."* One drain cursor, pool-wide.
  - `:152-170` the member block; `:142-146` `SerialState` (the per-content gate, keyed by
    `const Content*`).
- `src/runtime/worker_pool.cpp` —
  - `:44-55` `wait_completions`; **`:53`** `d_drained_gen = d_settle_gen; // consume` — the
    line that makes multi-waiter parking incorrect.
  - `:36-42` `poke()` (bump + `notify_all`), `:127-152` `run_task` (`:147-148`: settle →
    count → `poke()`), `:154-187` `run()` (`:160-162`: *"queued work is abandoned"* on stop),
    `:57-86` `submit` (`:60-62`: post-stop submit is a no-op that *"leaves the completion
    unsettled"* — the precedent D4's purge follows), `:88-125` `submit_inline`, `:18-25` the
    dtor (`request_stop` + join).
- `src/runtime/arbc/runtime/interactive.hpp` —
  - `:139-144` — *"The pool is PER-RENDERER (`d_pool`, below) and `InteractiveRenderer` owns
    it by value, so an uncapped `hardware_concurrency() - 1` would spawn 63 threads per
    viewport on a 64-core workstation — and a second viewport would double that until
    `runtime.shared_worker_pool` lets viewports share one pool."* The source names this task.
  - `:221-222` the sole constructor; `:224-227` copy/move deleted; **no user-declared
    destructor**.
  - `:318` `WorkerPool& worker_pool() noexcept` — the public accessor externally-async
    content uses to `poke()`. Must keep working for a borrowed pool.
  - `:355-363` the member block and its DECLARATION-ORDER-IS-LOAD-BEARING comment.
  - `:365-406` the per-viewport state (`d_wanted_tiles`, `d_carried_damage`, the identity
    memo, the counters) — the reason K viewports need K renderers.
- `src/runtime/interactive.cpp` — `:61-69` the ctor; `:306-310` the per-frame
  `PullServiceImpl` with `worker_backed_dispatch(d_pool)`; **`:376-386`** the park loop
  (`while (unsettled()) { if (!d_pool.wait_completions(deadline_at)) { expired = true; break; } }`)
  — the consumer of the broken cursor; `:416-429` the (already owner-scoped) deadline
  sweep; `:435` `poll_refinements`.
- `src/runtime/worker_dispatch.cpp:12-25` and `worker_dispatch.hpp:41-44,:49-51,:57` — the
  single submission site. Its header already states *"the returned dispatch borrows `pool`
  by reference and must not outlive it. Both drivers own their pool as a member"* — the
  second sentence is what this task falsifies.
- `src/runtime/offline_sequence.cpp:52-58` (`d_pool` by value, `d_parallel`),
  `:155` (the second `worker_backed_dispatch` call site), `:189-197` (parks to quiescence
  with `wait_completions(std::nullopt)` — a second cursor holder).
- `src/compositor/arbc/compositor/refinement.hpp:65-79` and `src/compositor/refinement.cpp:200-204`
  — `PendingTile` **owns** the render target the worker writes into, and `poll_refinements`
  retains unsettled tiles across frames. Corroborated at `src/compositor/pull_service.cpp:429-434`
  (*"The reap sink, not the caller, owns the surface + `inner` until the worker settles"*).
  This is precisely the memory a borrowed pool's workers can outlive.
- `src/contract/arbc/contract/content.hpp:117-118` (the wake is runtime policy),
  `:175-179` (`RenderCompletion::cancel()/cancelled()` — advisory, per-completion; there is
  **no** pool-wide or per-submitter cancel today), `:521-526` (`render` returns `nullopt`
  to settle asynchronously).
- `src/runtime/host_viewport.cpp:43,:76`, `host_viewport.hpp:266` — `HostViewport` borrows
  `InteractiveRenderer&`.
- `scripts/check_worker_dispatch.py` — greps for `\bRenderTask\b` outside
  `{worker_pool.hpp, worker_pool.cpp, worker_dispatch.cpp}` and rejects
  `submit({...})` (a braced submit names no type) *everywhere*, tests exempt. Adding a
  **field** to `RenderTask` and a **parameter** to `worker_backed_dispatch` both pass the
  lint, provided the helper keeps naming the type (`pool.submit(RenderTask{...})`) and no
  new non-test TU names `RenderTask`. Constraint 5.

**Existing tests this task extends or must not break:**

- `src/runtime/t/worker_pool.t.cpp` — `:151` inline degenerate, `:195` real pool wakes the
  render thread, `:238` per-content serialization ≤1, `:283` graceful stop
  (`02-architecture#worker-pool-stops-gracefully`), `:325` construct-and-destroy with no
  work, `:336` quiescent substrate, `:351` producer stress. Every call to
  `wait_completions` here changes shape under D1.
- `src/runtime/t/interactive.t.cpp` — `:409` the default worker count, `:745` "the frame
  never blocks past its deadline", `:849`/`:916`/`:994` the deadline-sweep cases, `:1236`
  off-thread arrival reaped without blocking. **No test today destroys an
  `InteractiveRenderer` with renders in flight** — A4 fixes that.
- `tests/interactive_worker_default.t.cpp`, `tests/worker_dispatch_leaf_only.t.cpp:495`
  (the third `worker_backed_dispatch` call site).
- `tests/claims/registry.tsv:151-155` — the pool claim cluster
  (`worker-pool-degenerates-to-inline`, `worker-pool-serializes-non-thread-safe-content`,
  `worker-pool-stops-gracefully`, `worker-dispatch-is-leaf-only`,
  `interactive-default-renders-leaves-off-the-frame-thread`). The new rows belong adjacent
  to these. Format: two tab-separated columns, `<doc-file-stem>#<slug>` then free text.
- `.github/workflows/ci.yml:59` — the per-push `gcc-tsan` lane runs the full suite with no
  exclusions. `.github/workflows/nightly.yml:56-83` — the `tsan-full` job's long-form
  `[.nightly]` sweep (`stress_publish_pin`, `stress_reclamation_queue`,
  `arena_growth_litmus`, `worker_dispatch_leaf_only`, `refine_idempotence_stress`).

**Predecessor decisions this task inherits:**

- `interactive_worker_count_default` **D5** — *"do not mint a claim id for a sentence no
  design doc contains."* Two new claims here, both hung on sentences D6's delta adds.
- `interactive_worker_count_default` **D6** — persistent behavioral counters live on the
  driver (or, for pool-internal facts, on the pool alongside
  `tasks_submitted`/`tasks_completed`), never widened into a host-facing result type.
- `interactive_worker_count_default` **D4** — the design-doc delta lands in the
  **implementation** commit, not with the refinement: *"a doc that promises a non-zero
  default while the shipped default is still `0` is a lie for as long as the refinement
  sits ahead of the code."* D6 below follows this precedent exactly.
- `interactive_worker_count_default` **D7** — invariants gate per-push; tuning numbers go
  to an opt-in bench. No wall-clock assertion enters the merge path.

## Constraints / requirements

1. **A parked renderer's settle may never be consumed by a sibling.** The pool keeps the
   settle counter; the *drain cursor* becomes caller-owned state (D1). Concretely: if
   renderer B's tile settles while renderer A is parked, B's next `wait_completions` must
   return `true`. A *spurious wake* (A woken by B's settle, finding nothing of its own) is
   permitted and expected — the park loop at `interactive.cpp:376-386` already re-tests
   `unsettled()` and re-parks against the same absolute deadline, so a spurious wake costs
   one predicate evaluation and cannot spin. A *stolen* wake is not permitted.

2. **`poke()` stays owner-blind.** It is the contract-facing async wake handle
   (`content.hpp:117-118`, reached by content through `InteractiveRenderer::worker_pool()`
   at `interactive.hpp:318`). Content knows nothing about renderers, so the wake must
   remain a broadcast. Any design that routes wakes to a specific waiter is ruled out by
   this seam (D1's rejected alternative).

3. **A renderer's teardown drains only its own submissions.** `~InteractiveRenderer` must
   guarantee, before any member is destroyed, that (a) no worker thread is inside
   `content->render` for a task this renderer submitted, and (b) no unstarted task of this
   renderer remains queued anywhere in the pool (`d_ready` or a `SerialState::parked`
   FIFO). It must **not** stop the pool, must not cancel or drop a sibling's tasks, and must
   not wait for a sibling's tasks. A sibling's in-flight renders keep running and the pool
   remains usable after a borrowing renderer dies.

4. **The drain must terminate unconditionally.** It waits on "the pool is no longer
   *touching* my surfaces", never on "my completions have settled." Those differ: a
   cancelled task still runs (`cancel()` is advisory — `content.hpp:175-179` — and
   `run_task` calls `content->render` regardless), and a content that settles off-thread
   may never settle at all. Waiting for settlement can hang; waiting for
   *no-longer-outstanding* cannot, because every **started** task's `run_task` returns even
   under `d_stop` (claim `02-architecture#worker-pool-stops-gracefully`, `registry.tsv:153`:
   stop *"lets in-flight renders finish"*), and every **unstarted** task of the draining
   owner is purged from the queues before the wait begins.

5. **The leaf-only lint stays green and the submission site stays singular.**
   `RenderTask` may gain a field and `worker_backed_dispatch` a parameter, but
   `worker_dispatch.cpp` must keep naming the type
   (`pool.submit(RenderTask{content, request, std::move(done), owner})`), no new non-test TU
   may name `RenderTask`, and no call site may use a braced `submit({...})` —
   `scripts/check_worker_dispatch.py` rejects all three. Operators still render inline; the
   owner tag changes *who is dispatching*, never *what is dispatched*.

6. **The deadline sweep is already owner-scoped; keep it that way.** `interactive.cpp:416-429`
   iterates `d_pending.tiles` — a renderer member — so it can only cancel its own
   completions. This is correct by construction today and must not be "generalized" into a
   pool-level cancel. Doc 02:169-170's *"A frame cancels the renders it no longer wants"*
   means **its own** renders; D6's delta says so out loud, because doc 02 currently has no
   sentence covering the cross-renderer case.

7. **Sharing a pool does not license sharing a `TileCache`.** Workers never touch the cache
   (the leaf-only rule; `worker_pool.hpp:37-40`), so all cache traffic is on each renderer's
   own frame thread. A host driving K renderers from **K threads** must give each its own
   `TileCache` — `KeyedStore` is single-thread-confined. A host driving K renderers from
   **one thread** may share one cache and should (tile reuse across viewports). This task
   ships the pool seam and says nothing else about cache topology; the multi-threaded
   integration test (A5) uses one cache per renderer, and the constraint is stated in D6's
   doc delta so a host cannot read "share the pool" as "share everything."

8. **Externally-async settlement is out of scope, and is not made worse.** No shipped
   content kind settles a `RenderCompletion` off-thread from a thread it owns: `Raster`,
   `Nested`, `Solid`, `Placeholder`, `Fade` and `Crossfade` all return a `RenderResult`
   inline, and `ImageSeqContent` (`plugins/imageseq/imageseq_content.cpp:175`) returns
   `nullopt` only *after* calling `done->fail(...)`. The `nullopt`-with-a-live-`done` path
   exists today only in test doubles. A content that hands `done` to a thread it owns must
   join that thread in its own destructor — the pool cannot know about it, and
   `~WorkerPool` does not wait for it today either. The drain's guarantee is therefore
   stated precisely in terms of *worker threads and queued tasks*, and A4's test asserts
   exactly that and no more. This is a pre-existing property, unchanged by this task, and it
   is **not** a deferred follow-up.

9. **The borrowed pool must outlive every renderer that borrows it.** The host declares the
   pool **before** the renderers so it destructs after them, mirroring the existing
   load-bearing-declaration-order discipline at `interactive.hpp:355-363` and
   `src/runtime/bench/interactive_bench_workloads.hpp:443`. `~InteractiveRenderer` calls into
   the pool, so a pool that died first is a use-after-free. The overload's header comment
   states this, and the doc delta states it normatively.

10. **`request_stop()` is the pool owner's prerogative.** A borrowing renderer must never
    call it: stop is terminal and pool-global (`worker_pool.cpp:60-62` makes every
    subsequent `submit` a silent no-op), so one viewport stopping the pool would strand
    every other viewport's renders unsettled and forever. For a host-owned pool, stop
    happens exactly once, in `~WorkerPool`, after the last renderer is gone.

11. **No new levelization edge, no new dependency.** `WorkerPool` and `InteractiveRenderer`
    are both in `arbc_runtime` (L5, `doc 17:61`); `WorkerPool` includes only
    `<arbc/contract/content.hpp>`. `scripts/check_levels.py` must stay green with no table
    change. Nothing new enters `docs/design/10`'s dependency ledger.

12. **The owned path stays byte-identical and costs nothing.** The default constructor,
    the default worker count, the offline driver's inline-exact default, and every existing
    golden are unchanged. A renderer that owns its pool behaves exactly as it does today;
    no golden is re-baselined (A6).

## Acceptance criteria

**A1 — the borrowing constructor and the owned/borrowed member pair.**
`explicit InteractiveRenderer(WorkerPool& pool, Clock clock = {});` declared at
`src/runtime/arbc/runtime/interactive.hpp` next to the existing ctor (`:221-222`), defined
in `src/runtime/interactive.cpp`. Members become
`std::optional<WorkerPool> d_owned_pool;` + `WorkerPool& d_pool;` (D2), with `d_pending`
still declared first and the existing DECLARATION-ORDER comment (`:355-363`) amended to
explain that it now covers only the *owned* case and that the borrowed case is covered by
the destructor body. `worker_pool()` (`:318`) keeps returning `d_pool` and keeps working for
both. Unit test in `src/runtime/t/interactive.t.cpp`: a renderer built on a borrowed pool
reports `&r.worker_pool() == &pool`; two renderers built on the same pool report the *same*
pool address and the same `worker_count()`; a default-constructed renderer still reports a
pool of its own (`&a.worker_pool() != &b.worker_pool()`), and the default worker count
property from the predecessor's A1 still holds.

**A2 — wake isolation, at the pool.** New claim id
**`02-architecture#shared-pool-park-observes-only-its-own-settles`** in
`tests/claims/registry.tsv` (adjacent to the pool cluster at `:151-155`), hung on the doc
delta D6 lands, enforced by a new `TEST_CASE` in `src/runtime/t/worker_pool.t.cpp`:
two independent `CompletionCursor`s over one pool; one task submitted and settled; **both**
cursors' `wait_completions` return `true`. Under today's shared `d_drained_gen`
(`worker_pool.cpp:53`) exactly one of them can — the second blocks. Then, for the
cross-consumption direction: cursor A parks and returns; a *second* settle occurs; cursor B
(which has never parked) must still observe it. Park with an `until` bound so a regressed
implementation fails as an **assertion** rather than a CTest timeout; the bound is a
hang-guard, never a timing assertion (doc 16:224-226).

**A3 — wake isolation, at the driver (the wrong-output regression this prevents).** In a
new `tests/shared_worker_pool.t.cpp`: two `InteractiveRenderer`s over one borrowed pool,
each with its own `TileCache` and target (Constraint 7). Renderer A is parked against an
already-past deadline (injected `epoch_clock()`, the idiom at
`src/runtime/t/interactive.t.cpp:327-330`) on a leaf that blocks on a `std::latch`;
renderer B's leaves settle promptly. Drive B's frame to completion. Assert
`b.deadline_expiries() == 0`, `b.tiles_cancelled() == 0`,
`b.counters().degraded_composites() == 0`, and `FrameOutcome::schedule_follow_up == false`
for B — i.e. **B never loses a wake to A**. Assert A's own `deadline_expiries() == 1` after
its park (A *should* expire; it is genuinely blocked). Second `enforces:` tag on the A2
claim id.

**A4 — teardown drains only its own.** New claim id
**`02-architecture#shared-pool-teardown-drains-only-its-own-submissions`**, enforced in
`tests/shared_worker_pool.t.cpp`, three sections:
  - *In-flight:* renderer B is destroyed while a worker is inside B's leaf `render` (the
    leaf signals entry on a latch, then blocks on a second one). `~InteractiveRenderer`
    must not return until that `render` returns — asserted with a flag the leaf sets on
    exit and the test reads after the destructor, plus an ASan/TSan-visible write into the
    `PendingTile` surface as the last statement of `render` (the use-after-free the old
    shape would produce is then a *sanitizer* failure on the `gcc-tsan` lane, not a silent
    pass).
  - *Queued:* with every worker occupied, B submits M tiles that never start, then is
    destroyed. Assert `pool.tasks_dropped()` grows by exactly M (A7), `pool.tasks_completed()`
    does **not** grow by M, and the destructor returns without running them.
  - *Sibling untouched:* through both of the above, renderer A (sharing the pool) has work in
    flight and queued. After B is gone, assert A's tiles still settle, `a.deadline_expiries()
    == 0`, `pool.tasks_completed()` accounts for every one of A's submissions, and a *fresh*
    submission by A after B's death still runs — proving the drain did not call
    `request_stop()` (Constraint 10).

**A5 — TSan / stress coverage (the task's concurrency story).** New
`tests/shared_worker_pool_stress.t.cpp`: K = 4 `InteractiveRenderer`s over one pool, each on
its own thread with its own `TileCache`, `SurfacePool` and target, driving frames over a
scene of thread-safe and non-thread-safe leaves, with renderers constructed and destroyed
mid-flight on a seeded schedule. Invariants asserted at quiescence:
`pool.max_in_flight_per_content() <= 1` (claim `02-architecture#worker-pool-serializes-non-thread-safe-content`,
`registry.tsv:152` — note the gate now serializes *across* renderers, a strengthening D5
records); `tasks_submitted() == tasks_completed() + tasks_dropped()`; no renderer observes a
`deadline_expiry` on a scene whose leaves all settle; the process exits clean. Runs in the
standard suite, so it lands on the per-push `gcc-tsan` lane (`ci.yml:59`) with no exclusion.
A hidden `[.nightly]` seed-sweep section is added, and
`shared_worker_pool_stress` is appended to the `tsan-full` sweep list in
`.github/workflows/nightly.yml:56-83` alongside `worker_dispatch_leaf_only`.

**A6 — the owned path is byte-identical; the borrowed path produces the same pixels.**
`tests/interactive_operator_binding_golden.t.cpp` and
`tests/host_viewport_reanchor_golden.t.cpp` pass against **unchanged golden bytes** (no
re-baselining — Constraint 12). Plus a new case in `tests/shared_worker_pool.t.cpp`: the
same scene rendered by a renderer that *owns* a pool of N workers and by a renderer that
*borrows* a pool of N workers is **byte-identical**. Second `enforces:` tag on
`02-architecture#worker-pool-degenerates-to-inline` (`registry.tsv:151`) — its promise is
that the pool's shape changes performance, not results, and pool *ownership* is the newest
instance of that.

**A7 — the purge counter.** `std::uint64_t tasks_dropped() const noexcept` joins
`tasks_submitted()` / `tasks_completed()` / `max_in_flight_per_content()` on `WorkerPool`
(`worker_pool.hpp:124-136`), in the doc-16:54-62 plain-`uint64_t`-plus-`noexcept`-accessor
style, with a rationale comment. It counts tasks removed from the queues without running:
the `drain_owner` purge, and (making an existing silent behavior observable) the queued work
`run()` abandons on stop (`worker_pool.cpp:160-162`). The accounting identity
`tasks_submitted() == tasks_completed() + tasks_dropped() + <still outstanding>` is what A5
asserts at quiescence.

**A8 — the two other cursor holders are migrated.** `SequenceRenderer` gains a
`CompletionCursor` member and its quiescence park (`offline_sequence.cpp:189-197`) passes it;
`src/runtime/t/worker_pool.t.cpp`'s existing cases (`:151`, `:195`, `:238`, `:283`, `:325`,
`:336`, `:351`) are updated to the new signature. `worker_backed_dispatch` grows its owner
parameter at its declaration (`worker_dispatch.hpp:57`), definition (`worker_dispatch.cpp:12`)
and all three call sites (`interactive.cpp:310` → `this`, `offline_sequence.cpp:155` → `this`,
`tests/worker_dispatch_leaf_only.t.cpp:495` → a test-local tag). `scripts/check_worker_dispatch.py`
and `scripts/check_levels.py` stay green (Constraints 5, 11).

**A9 — coverage.** The pool's new code (`CompletionCursor`, `drain_owner`, the purge, the
owner map, `tasks_dropped`) and the renderer's new ctor/dtor are all directly exercised by
A2/A4/A7, clearing doc 16's ≥90% diff-coverage gate on changed lines. The stress binary (A5)
carries its own bodies.

**A10 — the doc delta ships with the code.** D6's edits to `docs/design/02-architecture.md`
and `docs/design/00-overview.md` land in the **implementation commit**, per the predecessor's
D4. The two new claim rows must not contradict `registry.tsv:151-155`.

**Deferred follow-ups: (none.)** Every hazard this task surfaces is either closed here or
(Constraint 8) pre-existing, unchanged, and correctly out of scope. Nothing is registered in
the WBS.

## Decisions

**D1 — The drain cursor moves out of the pool and into the caller; `poke()` stays a
broadcast.** `WorkerPool` keeps `d_settle_gen` and drops `d_drained_gen`. The signature
becomes:

```cpp
// A caller-owned drain cursor. Each thread that parks in `wait_completions` owns one, so
// two renderers sharing a pool cannot consume each other's settles. The pool holds the
// counter; the cursor holds how much of it this waiter has already seen.
struct CompletionCursor { std::uint64_t drained_gen = 0; };

bool wait_completions(CompletionCursor& cursor,
                      std::optional<std::chrono::steady_clock::time_point> until);
```

`poke()` is unchanged: bump the generation, `notify_all`. A settle therefore wakes *every*
parked renderer, and each decides for itself, against its own cursor, whether it saw
something new. `wait_completions` still returns `false` only on timeout, so the park loop's
`expired` semantics at `interactive.cpp:376-386` are preserved exactly. A renderer seeds its
cursor from a new `settle_generation()` accessor at construction, so a renderer joining a
long-lived pool does not take one free spurious return; that seeding is a nicety, not a
correctness requirement — the park loop re-tests `unsettled()`, so a stale cursor costs at
most one extra predicate evaluation.

*Alternative rejected: route the wake to the owning renderer (a per-owner condvar or waiter
registry).* This is the "obviously right" design and it is closed off by the contract.
`poke()` is the **content-facing** async wake handle (`content.hpp:117-118`), reachable by
externally-async content through `InteractiveRenderer::worker_pool()` (`interactive.hpp:318`),
and content knows nothing about renderers — it holds a `WorkerPool&` and nothing else. To
route by owner, `poke()` would need an owner argument that its only contractual caller cannot
supply. Broadcasting and letting each waiter filter against its own cursor keeps the wake
seam exactly where doc 03 put it. The cost is a spurious wake per sibling settle: bounded by
the sibling's settle count, non-spinning (each wake re-parks against the same absolute
deadline), and paid only when a pool is actually shared.

*Alternative rejected: keep the pool-global cursor and serialize renderers' parks.* A lock
around "only one renderer may be inside `wait_completions`" turns K viewports into a
single-frame-at-a-time host, which is the opposite of the point.

**D2 — `std::optional<WorkerPool> d_owned_pool` + `WorkerPool& d_pool`, not
`std::variant<WorkerPool, WorkerPool*>`.** The `.tji` note suggests "a `std::variant`/pointer
member"; the reference-plus-optional pair is the pointer half of that suggestion, and it is
the better half. `WorkerPool` is non-movable (`worker_pool.hpp:83-86`), which makes a variant
awkward to construct and impossible to reassign, and every access through a variant needs a
`std::visit` or `std::get_if` at a call site that just wants *the pool*. `optional` +
reference gives an unconditional `d_pool` at zero access cost and zero branches. A reference
member deletes assignment, which `InteractiveRenderer` already deletes anyway
(`interactive.hpp:224-227`).

**D3 — Ownership is an opaque `const void*` tag on `RenderTask`, stamped by
`worker_backed_dispatch`.** `RenderTask` gains `const void* owner{nullptr};` and
`worker_backed_dispatch(WorkerPool& pool, const void* owner)` gains a **required** second
parameter, so both drivers must name themselves (`this`). The pool keys an
`std::unordered_map<const void*, std::size_t> d_outstanding` by it — mirroring the existing
`d_serial` map keyed by `const Content*` (`worker_pool.hpp:158`). Incremented in `submit`
(after the `d_stop` check, so a refused submit does not count), decremented when `run_task`
returns (**not** when the completion settles — Constraint 4), erased at zero.

*Alternative rejected: a typed `SubmitterId` / a registration handle returned by the pool.*
A strong type buys type-safety over a tag whose only operations are "compare" and "hash", at
the cost of a new public type and a registration lifetime. The pool already keys a map on a
raw `const Content*`; a raw `const void*` submitter tag is the same idiom at the same level
of abstraction.

*Alternative rejected: infer the owner from `std::this_thread::get_id()` at submit.* It
happens to work today (each renderer submits from its own frame thread) and breaks the moment
a host drives two viewports from one thread — which is a supported and desirable topology
(Constraint 7). Identity must be the renderer's, not the thread's.

**D4 — Teardown is an explicit `WorkerPool::drain_owner(const void* owner)` called from an
explicit `~InteractiveRenderer` body, for owned and borrowed alike.**

```cpp
// Wait until the pool is no longer TOUCHING this owner's tasks: purge the owner's
// not-yet-started tasks from the ready queue and the per-content parked FIFOs, then block
// until every one of its STARTED tasks has returned from `content->render`. Purged tasks
// are dropped with their completion left unsettled -- the same disposition a post-stop
// `submit` gives (`worker_pool.cpp:60-62`), and safe for the same reason: the completion is
// a `shared_ptr` the caller owns, and it is about to die with the caller. Does not stop the
// pool and does not touch another owner's work.
void drain_owner(const void* owner);
```

and `~InteractiveRenderer() { d_pool.drain_owner(this); }`. A destructor **body** runs before
any member is destroyed, so this drains before `d_pending`'s surfaces die — for the borrowed
case, where nothing else would, and harmlessly for the owned case, where `~WorkerPool` would
have done it a moment later. One dtor, no branch on ownership.

*Alternative rejected: an RAII `PoolClient`/lease member that drains in its own destructor.*
Prettier, and wrong in a way that is easy to miss: to drain before the surfaces die, the lease
member would have to be declared **before** `d_pending` — inverting the
DECLARATION-ORDER-IS-LOAD-BEARING comment at `interactive.hpp:355-363`, whose whole point is
that `d_pending` comes first. Two opposite member-order rules in one class, each a
use-after-free if broken, is a trap. A statement in the destructor body is unconditionally
ordered before *all* member destruction and needs no comment to defend it.

*Alternative rejected: drain by waiting for the renderer's own `d_pending` tiles to settle.*
Needs no pool change at all, and hangs. Cancel is advisory (`content.hpp:175-179`), so a
cancelled task still runs; a content that settles off-thread may never settle; and waiting for
settlement means viewport teardown waits for the full rendering of tiles nobody wants any
more. Draining on *outstanding-ness* rather than *settled-ness* terminates unconditionally
(Constraint 4) and lets the purge throw away the work outright.

*Alternative rejected: `request_stop()` on teardown.* Terminal and pool-global
(`worker_pool.cpp:27-34,60-62`) — it would strand every sibling viewport's renders unsettled,
permanently. Constraint 10.

**D5 — Two new claim ids, and the cross-renderer serialization strengthening is recorded, not
minted.** A2's wake isolation and A4's teardown isolation are the two sentences D6's delta
adds to doc 02, so each gets a claim (predecessor D5: *"do not mint a claim id for a sentence
no design doc contains"*). The third observable — that the per-content gate now serializes a
non-thread-safe content **across** renderers, because `d_serial` is keyed by `const Content*`
and two viewports over one document share `Content*` — needs no new id: it is
`02-architecture#worker-pool-serializes-non-thread-safe-content` (`registry.tsv:152`) holding
*more* strongly than before, and A5 tags it. Worth stating in the delta as a note, worth
asserting in the stress test, not worth a claim of its own.

**D6 — Design-doc delta, landed by the implementer in the implementation commit.** Following
the predecessor's D4 verbatim in spirit: a doc that promises a shareable pool while the shipped
renderer still builds its own is a lie for as long as the refinement sits ahead of the code.
The implementer lands, in the same commit as the code:

  (a) **`docs/design/02-architecture.md` § Threading model** — a new bullet after the
  non-zero-worker-count bullet (which ends at `:331`) and before *"Compositing itself happens
  on the render thread"* (`:332`): **the pool is owned by the renderer or borrowed from the
  host.** Per-viewport ownership stays the default; a host with K viewports builds **one** pool
  and hands it to every viewport, so K viewports cost N threads and not K×N. The bullet states
  the three rules that make sharing correct and that no sentence in doc 02 currently covers:
  a parked renderer observes only its **own** settles (a sibling's completion may wake it but
  can never be consumed by it); a renderer's teardown drains only its **own** submissions,
  leaving a sibling's renders in flight and the pool alive; and one renderer's deadline sweep
  cancels only its own tiles — doc 02:169-170's *"a frame cancels the renders it no longer
  wants"* means its **own** renders. It also states the two things sharing does **not** buy:
  the pool must outlive every renderer borrowing it, and a shared pool is not a shared
  `TileCache` (K renderers on K threads take K caches; the leaf-only rule is what keeps the
  cache off the workers, not the pool's topology).

  (b) **`docs/design/02-architecture.md:325`** — *"and capped, because the pool is
  per-viewport"* becomes *"per-viewport **by default**"*, with a pointer to the new bullet. The
  cap is not repealed: it protects the host that does *not* share, which is still the common
  case and still the default.

  (c) **`docs/design/00-overview.md:293`** — the same one-word amendment inside the
  `interactive_worker_count_default` decision record, plus a short new decision bullet: the
  renderer's pool is an **ownership seam**, not a fixed member; doc 02's standing promise that
  *"a host that wants a different pool passes one"* (`02:327-328`) is now true; the wake and the
  drain are the two things that had to become per-submitter for it to be true, and the
  per-submitter drain (not a pool-wide stop) is why one viewport closing cannot strand another.

  (d) **`tests/claims/registry.tsv`** — the two rows from D5, adjacent to the pool cluster at
  `:151-155`.

No delta is written *now*, with this refinement. Doc 17 needs no change: `WorkerPool` is not
named in it, both types are already in `arbc_runtime` (L5, `17:61`), and the caller-owned
`CompletionCursor` is an instance of the pattern doc 17:114-128 already blesses.

**D7 — `k_max_interactive_workers` stays at 2.** Retuning the cap now that its stated premise
("the pool is per-viewport") has softened is tempting and out of scope. The cap governs the
*default, owned* pool — the single-viewport host, whose arithmetic is exactly what the
predecessor benchmarked. A host that shares a pool passes its own `WorkerPoolConfig` and is not
bound by the cap at all, which is the whole point. Changing the default's magnitude is a
benchmark question the predecessor already answered on the evidence it gathered; reopening it
without new evidence would be re-deciding a settled decision, and doing it in the same commit
that changes the ownership seam would confound the two.

## Open questions

(none — all decided. Constraint 8 records the one hazard this task deliberately leaves
standing — content that settles a `RenderCompletion` from a thread it owns must join that
thread itself — and it is pre-existing, not reachable from any shipped kind, and unchanged in
either direction by this task.)

## Status

**Done** — 2026-07-13.

- Added `CompletionCursor` (caller-owned drain cursor) to `WorkerPool`, removing the pool-global `d_drained_gen`; `wait_completions` now takes a `CompletionCursor&` so two renderers sharing a pool cannot steal each other's settles (`src/runtime/arbc/runtime/worker_pool.hpp`, `src/runtime/worker_pool.cpp`).
- Added `RenderTask::owner` (`const void*`), `d_outstanding` per-owner map, `drain_owner(const void*)`, `tasks_dropped()` counter, and `settle_generation()` accessor to `WorkerPool` (`src/runtime/arbc/runtime/worker_pool.hpp`, `src/runtime/worker_pool.cpp`).
- Added borrowing constructor `InteractiveRenderer(WorkerPool&, Clock)` and explicit `~InteractiveRenderer` (calls `d_pool.drain_owner(this)`); members became `std::optional<WorkerPool> d_owned_pool` + `WorkerPool& d_pool` (`src/runtime/arbc/runtime/interactive.hpp`, `src/runtime/interactive.cpp`).
- Extended `worker_backed_dispatch` with a required `owner` parameter; all three call sites pass `this` or a test-local tag (`src/runtime/arbc/runtime/worker_dispatch.hpp`, `src/runtime/worker_dispatch.cpp`).
- Migrated `SequenceRenderer` to carry its own `CompletionCursor` member (`src/runtime/arbc/runtime/offline_sequence.hpp`, `src/runtime/offline_sequence.cpp`).
- Updated all existing `wait_completions` call sites to the new signature (`src/runtime/t/worker_pool.t.cpp`, `tests/worker_dispatch_leaf_only.t.cpp`, `tests/pull_service_async.t.cpp`, `tests/nested_concurrency.t.cpp`, `tests/imageseq_concurrency_stress.t.cpp`).
- New unit tests in `src/runtime/t/worker_pool.t.cpp` (two-cursor wake isolation) and `src/runtime/t/interactive.t.cpp` (ownership seam); new cross-component tests in `tests/shared_worker_pool.t.cpp` (wake isolation, teardown in-flight/queued/sibling, owned-vs-borrowed byte identity) and `tests/shared_worker_pool_stress.t.cpp` (K=4 mid-flight stress + `[.nightly]` sweep).
- Two new claims registered: `02-architecture#shared-pool-park-observes-only-its-own-settles` and `02-architecture#shared-pool-teardown-drains-only-its-own-submissions` (`tests/claims/registry.tsv`); stress binary added to nightly TSan sweep (`.github/workflows/nightly.yml`).
- Doc delta landed in the same commit: new pool-sharing bullet in `docs/design/02-architecture.md` § Threading model, one-word amendment at `:325`, and a new decision bullet in `docs/design/00-overview.md:293` (D6).
- Deviation A3: `degraded_composites() == 0` is unachievable on a cold worker-backed frame (placeholder pass is doc 02:63-65 working); asserted `<= 1` plus `deadline_expiries() == 0` — still catches a lost-wake regression. Deviation A5: stress scene is leaf-only (operator binding from K threads would race `bind_operators`' write into shared content — a per-document hazard unrelated to the pool, noted in the test file).
