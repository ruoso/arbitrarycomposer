# compositor.in_flight_tile_dedup — Suppress re-dispatch of tiles already in flight

## TaskJuggler entry

[`tasks/35-compositor.tji:111-116`](../../35-compositor.tji):

```
  task in_flight_tile_dedup "Suppress re-dispatch of tiles already in flight in RefinementQueue" {
    effort 1d
    allocate team
    depends !refine_frame_composite_idempotence
    note "tile_planning.cpp:351 and pull_service.cpp:219-243 are cache-first with no check against the RefinementQueue, so a tile whose render is already in flight is re-dispatched on the next refinement wave; an operator scene pays one extra operator render per wave. Waste, not incorrectness. Fix: query the RefinementQueue for in-flight tile ids before scheduling a new dispatch and skip duplicates. Source-of-debt: tasks/refinements/runtime/interactive_worker_count_default.md. Docs 02/13."
  }
```

## Effort estimate

WBS says **1d**. **Realistic estimate: 2d** — the closer should bump the WBS
effort when this lands, as `refine_frame_composite_idempotence` did.

The 1d figure costs the guard itself, which is genuinely two `if`s and one
helper. It does not cost what the guard turns out to require: a design-doc
delta across three docs (nothing in doc 02 or doc 13 says a tile is
dispatched at most once — Decision 6), a new behavioral counter so the
suppression is provable positively rather than by absence (Decision 5), and
the cancelled-entry carve-out (Decision 3), which is the difference between
"removes waste" and "strands a tile forever". The test surface is likewise
four files, one of which (`tests/interactive_worker_default.t.cpp`) is a
weakened assertion this task exists to strengthen.

## Inherited dependencies

**Settled:**

- `compositor.refine_frame_composite_idempotence` (the `depends` edge) —
  Done 2026-07-12. A damage-gated refine frame now clears and clips its
  repaint region, so a refined frame is byte-identical to a single full pass
  (`02-architecture#gated-frame-equals-single-pass`). This is the *reason*
  this task is safe to do second: with idempotence landed, the frame's pixels
  are a function of the tiles that are *resident*, not of how many times a
  tile was dispatched — so removing duplicate dispatches is provably
  pixel-neutral, and the goldens that pin it already exist.
- `compositor.refinement` — `RefinementQueue`, `PendingTile`,
  `poll_refinements` (`src/compositor/arbc/compositor/refinement.hpp:77-98`,
  `:156-158`). Its Decision 1 — frame-to-frame state belongs to the runtime
  loop, not L4 planning — is why the queue is a caller-owned value threaded in
  as a parameter, which is what makes it queryable from both dispatch sites
  with no new plumbing.
- `compositor.pull_service`, `compositor.operator_pull_delivers_target`,
  `compositor.pull_multi_tile_region` — `PullServiceImpl::pull` is cache-first
  per covering tile (`13-effects-as-operators#pull-is-cache-first`) and now
  covers the full multi-tile region, which is what makes an operator over a
  region *larger than one tile* the dominant duplicate-dispatch source.
- `compositor.counters` — `CompositorCounters` (`counters.hpp:34-89`); its
  Decision 5 (`requests_issued` counts renders *driven*, not misses *planned*)
  is the definition this task's headline assertion leans on.
- `runtime.interactive_worker_count_default` — the source of debt. It shipped
  a non-zero default worker count, which is what made this latent gap routine,
  and it landed the guard test that documents the gap in prose
  (`tests/interactive_worker_default.t.cpp:635-655`) and weakens its own
  assertion to `>=` because of it (`:697`).
- `runtime.operator_model_damage_routing` — arrival damage is routed on the
  input's *shared pull identity* to every operator that consumes it
  (`src/runtime/interactive.cpp:398-406`, doc 13:145-149). This is the
  load-bearing fact that makes dedup safe (Decision 2).

**Pending:** none. Every edge this task needs is landed.

## What this task is

Give the two dispatch sites — the driver's tile loop and the pull service's
per-covering-tile loop — a second suppression key alongside the cache: the
**pending set**. Today both are cache-first and *only* cache-first, so a tile
whose render is already in flight looks exactly like a tile nobody has ever
asked for, and gets dispatched again. Add one predicate,
`tile_in_flight(const RefinementQueue*, const TileKey&)`, and consult it at
both sites before allocating a surface and driving a render.

The suppressed tile is **not** treated as a hit. It contributes no pixels this
pass and it does not settle anything: it takes exactly the path a
freshly-dispatched async tile takes — the driver composites the degraded
fallback (stale → coarser → transparent), the pull marks the region async and
inexact so the operator degrades this frame — minus the second surface
allocation, the second `content->render`, and the second `PendingTile`. The
first dispatch's arrival then re-drives everyone, because arrival damage is
broadcast on the input's shared pull identity, not delivered per-dispatch.

**Not this task:**

- **Cross-frame dedup.** Suppression fires only on entries recorded earlier in
  the *same* frame — not because the predicate says so, but because the
  interactive driver's park loop leaves nothing else behind (Decision 4). The
  cross-frame half is forfeited by the deadline sweep's blanket `cancel()`,
  and recovering it is `runtime.deadline_cancel_retains_wanted`, registered
  below.
- **Changing quiescence, cancellation or deadline semantics.** A failing tile
  still degrades to a placeholder and the loop still quiesces on it. This task
  removes work; it does not change when the frame loop stops.
- **Making the queue a set / indexing it.** The membership predicate is a
  linear scan (Decision 1).
- **Fixing `PendingTile::local_rect`'s two meanings.** `tile_planning` records
  the tile cell (`tile_planning.cpp:507`), `pull_service` records
  `request.region` (`pull_service.cpp:336`). It is dead weight either way:
  `poll_refinements` derives its damage rect from the *key*
  (`tile_local_rect(pending.key.rung, pending.key.coord)`,
  `refinement.cpp:114`), never from `local_rect`. Dedup keys on `TileKey`
  alone, so first-writer-wins on that field is observationally inert
  (Decision 7).

## Why it needs to be done

`runtime.interactive_worker_count_default` shipped `worker_count` non-zero by
default. At `worker_count == 0`, `submit` *is* the render: a dispatched tile
settles inline and lands in the cache before anything can ask for it again, so
no tile is ever in flight and this gap is unobservable. Making the default
non-zero made it routine, and it is measurable: that task's own benchmark
table records `nested_deep` at **3027 ms** with 0 workers and **6247 ms** at
the shipped 2 — a scene made ~2× *slower* by adding threads, which is the
signature of every worker doing redundant work rather than parallel work
(`tasks/refinements/runtime/interactive_worker_count_default.md:480-484`).

That task could not assert the invariant it wanted. Its acceptance criterion
A5 asked that the tuple `(requests_issued(), operator_renders(),
composites())` per rendered frame be *identical* across worker counts —
"adding workers buys parallelism, never duplicate or wasted renders" — and it
had to ship `CHECK(... >= oracle_requests)` for the operator scenes
(`tests/interactive_worker_default.t.cpp:697`) with a 20-line comment
explaining why the `==` it wanted is false on the shipped compositor. This
task is what makes that `>=` an `==`.

The downstream consumers are everything that renders an operator scene with
workers on: the interactive editor (every frame of a fade/crossfade/nested
scene over a region wider than one tile), and the offline export driver, which
reaps to quiescence within one frame and so pays the intra-frame duplication
on every exported frame.

## Inputs / context

### Governing design docs (normative)

- **`docs/design/02-architecture.md:49-104`** § "The frame, interactively" —
  the six steps. Step 3 (`:57-60`) ends at "*and look each tile up in the
  cache*"; step 4 (`:61-65`) opens "*Cache misses become render requests with
  a deadline*". Both sentences are unconditional today, and both are what this
  task narrows. Step 6 (`:69-71`) is the refinement wave the duplicates ride
  on.
- **`docs/design/13-effects-as-operators.md:74-82`** — the `PullService`
  contract: "*cache lookup first, worker scheduling, snapshot token respected,
  deadline/budget inherited*" (claim `13-effects-as-operators#pull-is-cache-first`).
  Nothing sits between the lookup and the scheduling.
- **`docs/design/13-effects-as-operators.md:117-120`** — "*A pull whose input
  answers asynchronously — any covering tile — delivers nothing usable this
  pass: the completion is left unsettled, the operator degrades for this frame,
  and each async tile's arrival re-drives it*". The suppressed tile must land
  in exactly this state.
- **`docs/design/13-effects-as-operators.md:135-144`** — the TRANSIENT/FINAL
  split: a *dispatched* pull (completion unsettled) is TRANSIENT, "*something
  more is coming for this revision*". A suppressed pull is still TRANSIENT —
  something more *is* coming, from the dispatch that is already in flight — so
  the split is preserved unchanged, not amended.
- **`docs/design/03-layer-plugin-interface.md:152-157`** — the per-content
  serialization queue ("one in-flight render at a time") is *mutual exclusion
  for non-thread-safe content*, not deduplication: two requests for the same
  tile both go through it and both render. The pool's
  `max_in_flight_per_content() <= 1` guard is therefore not the fix, and the
  dedup key must be the `TileKey`, not the content id.

**None of these docs says, or implies, that a tile is dispatched at most
once**, that an in-flight render is not re-issued, or that anything but the
cache is consulted at plan time. The delta is genuinely new normative text —
see Decision 6.

### Source seams

- **`src/compositor/arbc/compositor/refinement.hpp:77-98`** — `PendingTile`
  (`TileKey key; …; std::shared_ptr<RenderCompletion> done;`) and
  `struct RefinementQueue { std::vector<PendingTile> tiles; };`. A plain
  vector: no index, no membership query, no mutex. Caller-owned, frame-thread
  only (`:64-68`).
- **`src/compositor/refinement.cpp:71-128`** — `poll_refinements`. Retains
  `!done->settled()` entries (`:78-81`); a settled-with-value entry inserts
  under its exact key, emits one `Damage` keyed off
  `tile_local_rect(pending.key.rung, pending.key.coord)` (`:114`) and bumps
  `follow_up_frames`; a settled-via-`fail` entry is **dropped with no insert
  and no damage** (`:122-124`). It does not consult `cancelled()`.
- **`src/compositor/tile_planning.cpp:383-510`** — the driver's tile loop.
  `if (tile.is_miss)` (`:390`) is the only gate; the surface is allocated at
  `:391-395`, the identity short-circuit resolved at `:420-436`, the render
  driven and counted at `:437-447`, dispatched at `:458-467`, and the async
  record pushed at `:497-509`. Nothing consults `pending`, which is *already a
  parameter in scope* (`tile_planning.hpp:324`).
- **`src/compositor/pull_service.cpp:213-243`** — the per-covering-tile loop
  (the WBS note's `219-243`). `const TileKey key{id, revision, selection.rung,
  coord, achieved_time};` (`:217`), cache probe (`:223-228`), and on a miss it
  falls straight into `make_surface` (`:236`) → counters (`:267-272`) →
  `d_dispatch` (`:283`) → async record (`:321-338`). The aggregate settle
  discipline is stated at `:202-211` (`region_exact`, `any_async`).
- **`src/contract/arbc/contract/content.hpp:141-198`** — `Completion<Result>`.
  The decisive lines are **`:161-163`**: "*`cancelled()` is an ADVISORY
  cooperative flag: `cancel` makes it observe `true` but does NOT prevent a
  later `complete`/`fail` — it only tells a long render it may abandon work*".
  `settled()`, `cancelled()`, `cancel()` and `take()` are all public and
  thread-safe against a renderer-side `complete`/`fail` (`:157-165`).
- **`src/runtime/interactive.cpp:346-397`** — the frame's Step 4/5/6. Plan and
  dispatch (`:346-348`), park in a loop while anything is unsettled
  (`:369-379`), on expiry **cancel every unsettled pending tile** (`:383-391`),
  then `poll_refinements` (`:397`) and route the arrival damage on the shared
  pull identity (`:398-406`).
- **`src/compositor/arbc/compositor/counters.hpp:34-89`** —
  `CompositorCounters`; `requests_issued()` is "one bump per `content->render`
  the driver issues for a fresh-key miss" (`:38-40`), `operator_renders()` at
  `:54`. Plain `std::uint64_t`, single-threaded driver path.
- **`tests/interactive_worker_default.t.cpp:635-655`** — the block comment
  that names this task as the fix, and **`:697`** — the `>=` this task
  tightens to `==`.

## Constraints / requirements

1. **Pixel-neutral.** Every existing golden must pass **byte-unchanged, with
   no re-baselining**: `tests/refinement_golden.t.cpp`,
   `tests/refine_idempotence_golden.t.cpp`,
   `tests/interactive_worker_default.t.cpp`'s `byte_identical` sweep. A
   suppressed dispatch was async, so it was never going to contribute pixels
   *this* pass; removing it can only remove work. If any golden moves, the
   guard is wrong.
2. **Never strand a tile.** A suppressed tile must be guaranteed to be
   re-driven by the in-flight render it deferred to. Where that guarantee does
   not hold — a *cancelled* completion, which may settle via `fail` and be
   dropped by `poll_refinements` with **no cache insert and no damage** — the
   tile must be dispatched, not suppressed (Decision 3). Waste is the bug being
   fixed; a permanent hole is not an acceptable trade for it.
3. **Null-tolerant.** Both guards must treat a null queue as "nothing in
   flight". The offline driver's first pass plans with `pending == nullptr`
   (`src/runtime/offline_sequence.cpp:136`), and `render_frame_interactive`'s
   `pending` parameter is optional by contract; the null path must stay
   byte-identical, per the house rule that every optional compositor parameter
   has a behavior-identical null path.
4. **No new thread-safety surface.** The queue stays frame-thread-only and
   unsynchronized (`refinement.hpp:64-68`). The predicate reads
   `done->settled()` and `done->cancelled()`, which are atomics the renderer
   thread writes — the same cross-thread read the driver's `unsettled()`
   predicate already makes (`interactive.cpp:369-372`). No lock, no new atomic,
   no new shared state.
5. **Levelization (doc 17).** The predicate lives in `compositor` (L4) next to
   the queue it queries; it consults `contract`'s `Completion` (L2) and
   `cache`'s `TileKey` (L3), both of which `compositor` already depends on. No
   new component edge, no new third-party dependency (doc 10).
6. **Suppression must not swallow the identity path.** An identity operator
   issues no render and creates no output cache entry — it *serves* its
   terminal input's tiles (`tile_planning.cpp:411-436`). The guard must not
   short-circuit that branch. It does not, and provably so: only *dispatched*
   keys are ever recorded pending, so an identity operator's output key is
   never in the queue.
7. **Counters stay honest.** A suppressed render bumps neither
   `requests_issued` nor `operator_renders` — that is the entire observable
   point. It bumps the new `requests_suppressed` instead (Decision 5), so the
   two are separable and the dedup is provable by presence, not only by
   absence.

## Acceptance criteria

**Claims-register entries** (`tests/claims/registry.tsv`, each with an
`// enforces:` tagged test; each requires the design-doc delta of Decision 6,
per the house rule "do not mint a claim id for a sentence no design doc
contains"):

- **`02-architecture#in-flight-tile-is-not-redispatched`** — a cache miss whose
  tile is already in flight (recorded pending, unsettled, uncancelled) issues
  no render request and allocates no surface; it degrades exactly as the
  dispatch it deferred to would have.
- **`02-architecture#cancelled-tile-is-redispatched`** — a pending tile whose
  completion has been cancelled does **not** suppress re-dispatch. It may
  settle via `fail` and be dropped with no insert and no damage, so nothing
  else would ever re-drive it.
- **`13-effects-as-operators#pull-joins-in-flight-tile`** — a pull whose
  covering tile is already in flight dispatches nothing, delivers nothing, and
  leaves the region async and inexact (TRANSIENT), so the operator degrades
  this pass and the in-flight arrival's damage re-drives it.

**The headline assertion — `tests/interactive_worker_default.t.cpp`:** tighten
`:697` from `CHECK(counters.requests_issued() >= oracle_requests)` to `==`,
for the **operator scenes** (`operator_heavy`, `nested_deep`), across the full
`{0, 1, 2, 4, hw-1}` worker sweep, and add the same `==` for
`operator_renders()`. This is `interactive_worker_count_default`'s A5 —
"adding workers buys parallelism, never duplicate or wasted renders" — landed
at last. The 20-line comment at `:635-655` explaining why the `==` is false
gets replaced by the invariant that is now true.

**Unit tests:**

- `src/compositor/t/refinement.t.cpp` — the predicate itself, one case per
  arm: null queue → false; empty queue → false; matching key, unsettled,
  uncancelled → **true**; matching key but `settled()` → false; matching key
  but `cancelled()` → false; non-matching key (differing in each of `content`,
  `revision`, `rung`, `coord`, `achieved_time`) → false. The last arm matters:
  a revision bump must re-render, and `TileKey` equality is what guarantees it.
- `src/compositor/t/pull_service.t.cpp` — extending the existing async-record
  harness at `:782-796`: pull tile T (async, worker-backed), then pull T again
  in the same frame → `requests_issued() == 1`, `requests_suppressed() == 1`,
  `queue.tiles.size() == 1`, and the second pull's `done` is **unsettled**
  (TRANSIENT, not a placeholder-settled FINAL). Then hand-settle the queue's
  one completion, `poll_refinements`, and pull a third time → cache hit, still
  `requests_issued() == 1`. Enforces `#pull-joins-in-flight-tile`.
- `src/compositor/t/pull_service.t.cpp` — the carve-out: pull T (async),
  `queue.tiles.front().done->cancel()`, pull T again → **not** suppressed:
  `requests_issued() == 2`, `queue.tiles.size() == 2`. Enforces
  `#cancelled-tile-is-redispatched`.
- `src/compositor/t/counters.t.cpp` — a driver-side scene with an operator over
  a region spanning ≥ 2 tiles sharing one input leaf tile: `requests_issued()`
  equals the count of **distinct** `TileKey`s the frame needed, and
  `requests_suppressed()` equals the duplicate count (a positive number — the
  test fails if the dedup silently never fires). Enforces
  `#in-flight-tile-is-not-redispatched`.

**Behavioral-counter framing, never wall-clock** (doc 16:54-62): the promise
is "no duplicate renders", which is a counter identity, not a latency claim.
The `nested_deep` speedup is a benchmark to *trend*, not to gate — no test
asserts a millisecond figure.

**Concurrency coverage:** `tests/refine_idempotence_stress.t.cpp` (60 iters ×
`{1,2,4}` workers, `[.nightly]`, TSan-full lane) must stay green unchanged,
and gains one assertion: across the whole quiesced multi-worker run,
`requests_issued()` equals the single-worker oracle's — duplicate-free under
real thread interleaving, not just in the unit harness. The predicate's
cross-thread reads (`settled()`, `cancelled()`) are exercised there against a
live worker pool, which is where TSan would catch a mis-ordered read.

**Coverage:** ≥ 90% diff coverage on changed lines (CI gate). The guard has
five arms and each has a test above.

**Deferred to `runtime.deadline_cancel_retains_wanted` (closer registers in
WBS):** effort **2d**, `depends !in_flight_tile_dedup`, milestone **M9** (the
milestone this task and its siblings `compositor.disjoint_dirty_repaint` /
`compositor.refine_frame_composite_idempotence` already belong to,
`tasks/99-milestones.tji:72`). One-line description: *the deadline sweep
(`interactive.cpp:383-391`) cancels every unsettled pending tile, including
tiles the very next frame still wants — which forfeits in-flight dedup across
the frame boundary, since a cancelled entry cannot be suppressed against
(Decision 3). Narrow the sweep to cancel only tiles that are no longer wanted
(revision superseded, or no longer visible), leaving still-wanted renders in
flight so the follow-up frame suppresses their re-dispatch instead of
re-issuing it.* This is not a re-audit: it is a concrete change to one loop,
with the same counter-identity acceptance test one frame further out. Note the
deadline itself is enforced by the park *not waiting*, not by the cancel — so
narrowing the sweep costs nothing in deadline enforcement (Decision 4).

## Decisions

### 1. The membership query is a linear scan over `queue.tiles`, not a side index

`tile_in_flight(const RefinementQueue* queue, const TileKey& key)` — declared
next to the queue in `refinement.hpp`, implemented as a scan comparing
`.key` and testing the completion.

The tempting alternative is an `std::unordered_set<TileKey>` inside
`RefinementQueue` — and it would work: `std::hash<arbc::TileKey>` is already
specialized (`src/cache/arbc/cache/key_shapes.hpp:157-174`), so the set costs
nothing to write. **Rejected, and for a reason that is not "premature
optimization" but a correctness argument:** membership in the in-flight set is
not a function of the queue's *mutations*. It depends on
`done->cancelled()` and `done->settled()`, two atomics a **renderer thread**
flips asynchronously, with no notification to the queue. A precomputed set
would be stale the instant a worker settled or the driver cancelled, so it
would have to be rebuilt or re-validated against the completions on every
query anyway — which is the scan, plus an invariant to get wrong. The
single-source-of-truth version cannot drift.

The cost is `O(pending)` per miss, with `pending` bounded by the frame's
outstanding-miss count (tens to low hundreds) and the comparison a five-field
`TileKey` equality. That is nanoseconds against a tile render, and it is paid
only on the *miss* path — a warm frame never calls it. If it ever profiles
hot, the seam is one function and the index goes behind it without touching
either call site.

### 2. Dedup is safe because arrival damage is *broadcast*, not delivered

This is the load-bearing correctness argument, and it is worth stating
plainly, because "skip the dispatch" is only sound if something else is
guaranteed to re-drive the caller that was skipped.

The suppressed pull does not register a completion, receives nothing, and is
never told the tile landed. It does not need to be. When the in-flight render
settles, `poll_refinements` inserts it under its exact key and emits one
`Damage` for the tile region keyed on the **content's id**
(`refinement.cpp:107-115`), and `route_arrival_damage` maps that id — the
input's *shared pull identity*, "the input tiles every operator consuming it
shares" (`interactive.cpp:260-262`, doc 13:145-149) — to **every** operator
layer that reaches it. So one arrival re-plans every consumer of that tile,
whether that consumer dispatched the render, deferred to someone else's
dispatch, or hadn't been planned yet when it was issued. The re-drive is a
property of the *tile*, not of the *dispatch*.

That is precisely why deduplicating dispatches cannot lose a re-drive: N
dispatches of one tile produced N `PendingTile`s but still exactly **one**
broadcast per arrival to the same set of consumers. Collapsing N to 1 changes
the work done, not the wake-ups delivered.

**Alternative rejected:** *join the in-flight completion* — have the suppressed
caller register a continuation on the existing `PendingTile::done` so it is
notified directly. This is what a futures library would do, and it is strictly
more machinery for strictly less: `Completion` is deliberately **one-shot** and
single-settle (`content.hpp:141-149`), so a multi-waiter join means a new
contract type (an L2 change, plugin-ABI visible) to replicate a wake-up the
damage router already delivers. Rejected on doc 13's own terms: the arrival
re-drive *is* the join.

### 3. A **cancelled** pending tile does not suppress re-dispatch

The predicate is `!done->settled() && !done->cancelled()`, not merely "a
`PendingTile` with this key exists". The `cancelled()` clause is the whole
difference between removing waste and introducing a stall.

`cancel()` is **advisory** and does not settle: "*`cancel` makes it observe
`true` but does NOT prevent a later `complete`/`fail` — it only tells a long
render it may abandon work*" (`content.hpp:161-163`). So a cancelled tile stays
in the queue, unsettled, and a conformant content is entirely within its
rights to *honor* the cancel and settle via `fail`. `poll_refinements` drops a
failed arrival with **no cache insert and no damage** (`refinement.cpp:122-124`)
— by design, because that is what stops a persistently-failing tile from
spinning the refinement loop forever.

Chain those two facts and the naive predicate is a bug: suppress against a
cancelled entry, the content honors the cancel, the entry is dropped silently
— and now the tile is in neither the cache nor the queue, and **no damage was
ever emitted for it**. Nothing re-plans it. It shows a placeholder until some
unrelated edit happens to damage the same region. That is not waste; it is a
permanent hole, and it would be a *deterministic* one for any content that
implements cancellation, i.e. exactly the well-behaved plugins the contract
asks for.

Excluding cancelled entries costs a re-dispatch of a tile that was probably
going to land anyway (most content ignores the advisory flag). That is the
same waste we have today, on the cross-frame path only, and it is honestly
bounded and honestly deferred (`runtime.deadline_cancel_retains_wanted`).
Trading a live-but-wasteful loop for a quiet permanent hole is not a trade
worth making.

**Alternative rejected:** *make `poll_refinements` emit damage when it drops a
cancelled arrival*, so a stranded tile re-plans. This closes the hole and would
let the predicate ignore `cancelled()` — but it re-opens a worse one: a
persistently-failing tile then damages → re-plans → fails → damages forever,
and the frame loop never quiesces, breaking `02-architecture`'s "still scene
issues zero renders". Distinguishing a cancel-drop from a fail-drop *is*
possible (`take()` yields the error; `cancelled()` is queryable), but that is a
change to the refinement loop's termination semantics — a bigger, separate
decision than "don't render the same tile twice", and squarely out of scope
for a task whose own WBS note says *waste, not incorrectness*.

### 4. Suppression fires only within a frame — and that is where the waste is

A consequence worth writing down, because it bounds what this task can claim.

The interactive driver's park loop exits on exactly two conditions
(`interactive.cpp:374-391`): either nothing is unsettled — every dispatch
landed, and `poll_refinements` drains them all — or the deadline expired, in
which case **every** unsettled tile is cancelled. There is no third exit. So at
the top of any *later* frame, every entry still in the queue is either
cancelled (excluded by Decision 3) or settled-since-the-poll (excluded by the
`settled()` clause). **Cross-frame suppression therefore never fires**, and the
guard is, in effect, an intra-frame duplicate filter.

That is not a disappointment, because intra-frame is where the duplicates come
from. Within one frame, before any cancellation has happened, every entry in
the queue is unsettled and uncancelled, and the duplicate sources are:

- an operator whose output spans **k** tiles: the driver drives its render once
  per output tile, each render pulls the covering input tiles, and the shared
  input tiles are re-pulled — up to *k−1* duplicate dispatches of the same
  leaf, the dominant source and the one `pull_multi_tile_region` made routine;
- **two operators sharing an input** — each pulls it, the second re-dispatches;
- **a nested chain** — one duplicate per level per wave, exactly as the WBS
  note says;
- a leaf that is **both a visible layer and an operator's input** — the driver
  dispatches it, then the operator's pull re-dispatches it.

Every one of these is uncancelled, in-flight, same-frame — and every one is
suppressed. It also explains the benchmark: at `worker_count == 0` the first
dispatch settles *inline* into the cache, so the second pull hits the cache and
no duplicate exists; that is why `nested_deep` is 2× *faster* with zero workers
today, and why the fix restores the counter identity across the whole sweep.

The cross-frame half is recovered by `runtime.deadline_cancel_retains_wanted`
(registered above), which narrows the blanket cancel. Note this task's guard is
**already correct** for that world — no re-work, just more entries that pass
the predicate.

### 5. A new counter, `requests_suppressed`, proves the dedup positively

`CompositorCounters` gains `requests_suppressed()` / `note_request_suppressed()`
(`counters.hpp`), bumped once per dispatch the guard skips.

Without it, every acceptance test is an assertion that a number *did not grow*
— and a number that did not grow is exactly what you also observe when the
dedup never fires, when the scene silently warmed the cache, or when a refactor
quietly disconnects the guard. `requests_issued() == oracle` passes vacuously in
all three cases. With it, the tests assert the identity **and** that the
suppression path was actually taken N times, so a guard that stops firing fails
loudly instead of passing quietly. This is doc 16's behavioral-counter taxonomy
applied to its own purpose: counters exist so a performance-shaped promise gets
a *behavioral* gate rather than a wall-clock one.

It is six lines, it follows the existing `note_*` pattern exactly
(`counters.hpp:75-80`), and `CompositorStats`/`counters_snapshot` compose it for
free.

**Alternative rejected:** *infer suppression from `requests_issued` vs. the
planned-miss count.* The compositor does not count planned misses — Decision 5
of `compositor.counters` deliberately counts renders *driven*, not misses
*planned* — so the denominator does not exist, and inventing it means a second
new counter to avoid a first.

### 6. Design-doc delta (rides in the closer's commit, doc 16's same-commit rule)

Neither doc 02 nor doc 13 contains any sentence about dedup, in-flight state,
or an at-most-once dispatch bound. They currently say the *opposite*, twice:
"*Cache misses become render requests with a deadline*" (`02:61`,
unconditional) and "*cache lookup first, worker scheduling*" (`13:76-77`,
nothing in between). The house rule from `interactive_pull_wiring` Decision 5 —
*do not mint a claim id for a sentence no design doc contains* — means the
three claims above require the docs to say it first:

- **`docs/design/02-architecture.md`** § "The frame, interactively" — step 3
  gains the pending set as a second thing planning consults; step 4 gains the
  "*unless it is already in flight*" clause; a new paragraph after the
  gated-frame invariant states the dedup rule, the cancelled carve-out, and
  *why* the arrival re-drive makes it safe (Decision 2).
- **`docs/design/13-effects-as-operators.md`** § "The operator contract" — the
  async-pull paragraph (`:117-120`) gains the in-flight case: a pull whose
  covering tile is already in flight dispatches nothing and stays TRANSIENT.
  This *narrows* `pull-is-cache-first` rather than contradicting it (the cache
  is still probed first; the pending set is only consulted on a miss), and it
  leaves the TRANSIENT/FINAL split at `:135-144` exactly as written — a
  suppressed pull is still "something more is coming for this revision".
- **`docs/design/00-overview.md`** § "Resolved questions" — a decision-record
  bullet in the `:211-234` style, since "the compositor never renders the same
  tile twice concurrently" is a project-shaping economy rule, not a local
  implementation detail.

### 7. Dedup keys on `TileKey` alone; `PendingTile::local_rect` is not consulted

The two record sites disagree about `local_rect`: `tile_planning` stores the
tile cell (`tile_planning.cpp:507`), `pull_service` stores `request.region`
(`pull_service.cpp:336`), which for a multi-tile pull is *wider than the tile*.
After dedup, only one of the two entries survives — so if anything downstream
read that field, first-writer-wins would be a behavior change, and a
frame-order-dependent one.

Nothing reads it. `poll_refinements` derives the damage rect from the **key** —
`tile_local_rect(pending.key.rung, pending.key.coord)` (`refinement.cpp:114`) —
not from `local_rect`, so the emitted damage is identical no matter which site
recorded the tile. The field is dead weight, the dedup is observationally inert
with respect to it, and this task neither depends on it nor cleans it up.
Recorded here so the next reader doesn't have to re-derive that it's safe.

**Alternative rejected:** *normalize `pull_service` to record the tile cell too,
so the two sites produce identical `PendingTile`s.* It is a one-line change and
it is arguably more correct — but it is a change to emitted damage (narrowing
it) in a task that must be pixel- and damage-neutral, and it buys nothing the
key-derived damage doesn't already give. Not worth the blast radius.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-12.

- `tile_in_flight(queue, key)` predicate added to `src/compositor/arbc/compositor/refinement.hpp`; tests `!done->settled() && !done->cancelled()` (Decision 3); null-tolerant (Constraint 3).
- `requests_suppressed` / `note_request_suppressed()` counter added to `src/compositor/arbc/compositor/counters.hpp` per Decision 5.
- Suppression wired at both dispatch sites: `src/compositor/tile_planning.cpp` (driver tile loop) and `src/compositor/pull_service.cpp` (per-covering-tile loop); `src/compositor/refinement.cpp` unchanged in logic.
- Unit tests: `src/compositor/t/refinement.t.cpp` (6-arm `tile_in_flight` suite: null / empty / live / settled / cancelled / key-mismatch across all five `TileKey` fields); `src/compositor/t/pull_service.t.cpp` (pull-join + cancelled-carve-out arms); `src/compositor/t/counters.t.cpp` (driver scene with two operator layers sharing one leaf over a 2×2-tile viewport → `requests_issued()==12` distinct keys, `requests_suppressed()==4`).
- Claims registered in `tests/claims/registry.tsv`: `02-architecture#in-flight-tile-is-not-redispatched`, `02-architecture#cancelled-tile-is-redispatched`, `13-effects-as-operators#pull-joins-in-flight-tile`.
- Design-doc delta (Decision 6): `docs/design/00-overview.md`, `docs/design/02-architecture.md`, `docs/design/13-effects-as-operators.md` — dedup rule, cancelled carve-out, and broadcast re-drive argument added.
- Headline `==` **not landed**: `requests_suppressed()==0` in `operator_heavy`/`nested_deep` at every worker count (MEASURED) — those scenes share no leaf tile so the guard provably never fires. Root cause of the 2× slowdown is TRANSIENT inexact tile re-planned as a miss on every arrival wave, not duplicate dispatch. `>=` kept at `tests/interactive_worker_default.t.cpp:697`; `requests_suppressed()==0` added as a positive guard; stale comments corrected. Follow-up: `compositor.operator_refinement_wave_amplification`.
