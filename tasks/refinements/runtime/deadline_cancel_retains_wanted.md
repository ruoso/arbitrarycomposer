# runtime.deadline_cancel_retains_wanted — Narrow the deadline sweep's blanket cancel to unwanted tiles

## TaskJuggler entry

[`tasks/65-runtime.tji:166-171`](../../65-runtime.tji):

```
  task deadline_cancel_retains_wanted "Narrow deadline sweep's blanket cancel to unwanted tiles" {
    effort 2d
    allocate team
    depends compositor.in_flight_tile_dedup
    note "The deadline sweep (interactive.cpp:383-391) cancels every unsettled pending tile,
          including tiles the very next frame still wants — forfeiting cross-frame in-flight
          dedup, since a cancelled entry cannot be suppressed against (Decision 3 of
          compositor.in_flight_tile_dedup). Narrow the sweep to cancel only tiles that are no
          longer wanted (revision superseded, or no longer visible), leaving still-wanted renders
          in flight so the follow-up frame suppresses their re-dispatch. Counter-identity
          acceptance: same as in_flight_tile_dedup's test, extended one frame further out.
          Source-of-debt: tasks/refinements/compositor/in_flight_tile_dedup.md. Docs 02/13."
  }
```

Milestone **M9** (`tasks/99-milestones.tji:72`).

## Effort estimate

WBS says **2d**. **Realistic estimate: 3d** — the closer should bump the WBS
effort when this lands.

The sweep itself is a nine-line loop and the predicate that narrows it is
another dozen. The 2d estimate priced that. What it did not price:

1. **The wanted set does not exist yet.** `InteractiveRenderer` holds no
   persistent viewport, no visible-tile set, and no per-content current
   revision (`interactive.hpp:345-379`); the frame's plans are frame-local
   (`interactive.cpp:345`) and, worse, *repaint-scoped* — planning runs
   per-dirty-rect (`tile_planning.cpp:451-491`), so "the keys this frame
   planned" is a strict **subset** of "the tiles this frame wants" whenever
   the frame is a partial repaint. Getting "wanted" right means computing the
   frame's **full visible footprint**, independent of the dirty region
   (Decision 2). That is a new (small) pass in `tile_planning`, plumbed through
   two configs.
2. **This invalidates two shipped claims and their enforcing assertions.**
   `02-architecture#interactive-frame-loop-bounded-by-deadline`
   (`tests/claims/registry.tsv:156`) and
   `02-architecture#interactive-default-renders-leaves-off-the-frame-thread`
   (`:155`) both assert that the expired frame *cancelled* the in-flight
   render — and their tests assert it on a tile that, after this task, is
   still wanted and therefore **retained** (Decision 6). Amending a normative
   claim, its doc sentence, and three test sites is most of the day the
   estimate is missing.

## Inherited dependencies

**Settled:**

- **`compositor.in_flight_tile_dedup`** (Done 2026-07-12) — landed
  `tile_in_flight(queue, key)` (`src/compositor/refinement.cpp:72-88`), the
  dispatch guard at both sites (`tile_planning.cpp:518-521`,
  `pull_service.cpp:260-267`), and the `requests_suppressed` counter
  (`counters.hpp:85`). Its **Decision 3** — a *cancelled* pending tile does
  not suppress re-dispatch — is why this task exists: with the sweep
  cancelling every unsettled entry on expiry, every entry that survives a
  frame boundary is cancelled, so cross-frame suppression provably never
  fires. Its **Decision 4** states the contract this task must meet: *"this
  task's guard is already correct for that world — no re-work, just more
  entries that pass the predicate."*
- **`compositor.operator_refinement_wave_amplification`** (Done 2026-07-12) —
  landed `OperatorWait{output, unmet}` on `RefinementQueue`
  (`refinement.hpp:114-117`, `:132-135`), the `operator_wave_pending` gate
  (`refinement.cpp:138-153`), and `renders_coalesced`. Its **Decision 4**
  deliberately made the wave gate *cancel-agnostic* precisely so the blanket
  sweep could not tear it down, and states: **"When
  `deadline_cancel_retains_wanted` lands, nothing here changes."** This task
  must keep that true — and must not strand a wave (Constraint 4).
- **`runtime.worker_dispatch_leaf_only`**, **`runtime.interactive_worker_count_default`** —
  the shipped non-zero worker default is what makes deadline expiry (and
  therefore the sweep) a routine event rather than a corner case.

**Pending:** none.

## What this task is

On deadline expiry, the interactive frame loop currently cancels **every**
unsettled pending tile (`src/runtime/interactive.cpp:383-391`) — with no test
of whether the tile is one the next frame will still ask for. Since
`tile_in_flight` deliberately refuses to suppress against a cancelled entry
(`refinement.cpp:83`), every pending tile that crosses a frame boundary is
already disqualified from suppression before the next frame plans. Cross-frame
in-flight dedup is therefore forfeited, structurally, by this loop.

This task narrows the sweep: cancel a pending tile only if the frame **no
longer wants** it, and leave still-wanted renders in flight, uncancelled, so
the following frame's `tile_in_flight` gate suppresses their re-dispatch
instead of re-issuing it. "Wanted" is made concrete as the frame's **visible
tile footprint** — every `TileKey` a surviving visible layer covers over the
whole viewport at the frame's chosen rung, revision and composition time, plus
the keys the frame's pulls named, plus the unmet inputs of any live
`OperatorWait` whose output is itself wanted. Both of the WBS note's cancel
criteria fall out of key equality: a revision bump changes `TileKey::revision`
(`key_shapes.hpp:64-76`), so a superseded tile's key is absent from the
footprint; a pan, a zoom (new rung), a clock advance (new `achieved_time`), or
a layer culled out of its time span likewise leave the old key out. There is no
second predicate to keep in sync.

**Not this task:**

- **Changing `tile_in_flight`.** It stays `!settled() && !cancelled()`
  (`refinement.cpp:83`). This task makes *fewer entries cancelled*; it does not
  make cancelled entries suppressible. `in_flight_tile_dedup`'s Decision 3
  stands, and `operator_refinement_wave_amplification`'s Decision 4 re-affirms
  it.
- **Changing the wave gate.** `operator_wave_pending` stays cancel-agnostic
  (`refinement.cpp:138-153`). This task must not regress it and must not need
  it (Constraint 4).
- **Changing what removes entries from the queue.** The sweep never removed
  anything; `poll_refinements` does, and only for settled entries
  (`refinement.cpp:173-247`). Queue occupancy, the frame loop's early-out
  (`interactive.cpp:246-250`) and its quiescence are untouched (Decision 5).
- **Changing deadline enforcement.** The deadline is enforced by the park *not
  waiting past it* (`interactive.cpp:369-379`), not by the cancel. Narrowing
  the sweep costs nothing in enforcement (Decision 6).
- **Bounding pending-surface memory.** A retained `PendingTile` holds its
  target `Surface` across the frame boundary — but so did a cancelled one
  (cancel is advisory; the entry, the surface and the worker writing into it
  all survive it). No new retention class (Decision 5).

## Why it needs to be done

Two reasons, and the second is the bigger one.

**1. It recovers cross-frame in-flight dedup.** This is the reason the WBS note
gives. Under deadline pressure — the regime a worker pool exists for, and the
one the shipped non-zero default made routine (doc 02:283-297) — a miss
dispatched at frame *N* typically has not landed when frame *N* gives up. Today
it is cancelled, so when frame *N+1* re-plans the same tile (its region is
dirty again, most commonly from a sibling's arrival damage), `tile_in_flight`
sees a cancelled entry, declines to suppress, and the frame dispatches a
**second render of a tile that is already being rendered**. The waste is
proportional to how often the deadline is missed, which is exactly proportional
to how loaded the machine is.

**2. It closes a latent permanent-hole bug that the blanket cancel itself
creates.** This is not in the WBS note; it emerged from taking Decision 3's
reasoning one step further, and it is a correctness defect, not waste.

`cancel()` is advisory (`content.hpp:161-163`) — a conformant content is
entirely within its rights to *honor* it and settle via `fail`. `poll_refinements`
drops a failed arrival with **no cache insert and no damage**
(`refinement.cpp:225-226`), by design, so that a persistently-failing tile
cannot spin the refinement loop forever. Now chain those two facts against the
blanket sweep:

> Frame *N* dispatches tile T (visible, wanted). The deadline expires. The
> sweep cancels T. The content honors the cancel and fails. `poll_refinements`
> drops T: not in the cache, not in the queue, **no damage emitted**. Frame
> *N+1* is a partial repaint whose dirty region does not intersect T (nothing
> damaged T — the drop was silent). T is never re-planned. The pending queue is
> now empty, so the loop's early-out (`interactive.cpp:246-250`) lets it
> quiesce. **T shows a placeholder until some unrelated edit happens to repaint
> its region.**

That is precisely the failure Decision 3 refused to introduce via suppression —
and the blanket sweep introduces it directly, without any suppression involved.
It is latent today only because the shipped test doubles ignore the advisory
flag (`in_flight_tile_dedup` Decision 3: *"most content ignores the advisory
flag"*), which means it is deterministic for exactly the well-behaved plugins
the contract asks for. Narrowing the sweep to unwanted tiles removes the cause:
a tile the frame still wants is never told to abandon its render, so a
conformant content never fails it. Unwanted tiles still get cancelled, still
may fail-drop, and still emit no damage — and that is correct, because nobody
wants them.

## Inputs / context

### Governing design docs (normative)

- **`docs/design/02-architecture.md:49-81`** — "The frame, interactively". Step
  3 already names the **pending set** as a second suppression key alongside the
  cache; step 4 already carries the *"unless the tile's render is already in
  flight"* clause. Both were added by `in_flight_tile_dedup`. The doc does not
  yet say anything about *what the sweep cancels* — that sentence has to exist
  before a claim can be minted against it (Decision 7).
- **`docs/design/02-architecture.md:132-166`** — the in-flight-dedup paragraphs,
  including the cancelled carve-out (`:157-166`): *"a suppression that trusted a
  cancelled entry would strand its tile … A cancelled entry is therefore
  re-dispatched, and only a live, uncancelled in-flight render is joined."* This
  paragraph is the constitutional text this task must leave standing while
  making it *reachable*.
- **`docs/design/02-architecture.md:192-199`** — the wave gate's asymmetry
  paragraph, which names the sweep explicitly: *"were it otherwise, the deadline
  sweep — which cancels every unsettled tile on expiry — would open every gate
  at every frame boundary."* The parenthetical describes today's sweep; the
  delta must update the description without disturbing the argument (the wave
  gate stays cancel-agnostic and stays correct either way).
- **`docs/design/02-architecture.md:127-130`** — the degrade promise the sweep
  must not weaken: *"A tile that is still un-rendered when the deadline arrives
  paints step 4's fallback … it does not leave the previous frame's pixels
  showing."* Nothing here depends on the cancel.
- **`docs/design/02-architecture.md:283-297`** — the threading-model paragraph:
  the deadline promise needs the render to be *"somewhere the frame is not"*.
  Note the phrase *"nothing left to degrade to and nothing to cancel"* describes
  the zero-worker degenerate case, not the sweep's policy.
- **`docs/design/03-layer-plugin-interface.md:162`** and
  **`src/contract/arbc/contract/content.hpp:161-163`** — cancellation is
  *"cooperative, best-effort"* and advisory: `cancel` makes `cancelled()`
  observe `true` but does **not** prevent a later `complete`/`fail`.
- **`docs/design/13-effects-as-operators.md`** — the operator/pull contract. No
  semantic change here: this task alters only which pendings carry an advisory
  flag. The wave machinery (doc 02:168-207) is the doc-level statement of the
  `OperatorWait` interaction Constraint 4 protects.
- **`docs/design/16-sdlc-and-quality.md:23-26`** (same-commit doc amendment),
  **`:54-62`** (behavioral counters, never wall-clock), **`:225-226`**
  (wall-clock gates stay out of the merge path).
- **`docs/design/17-internal-components.md:55,57,61`** — `cache` L3,
  `compositor` L4, `runtime` L5; a component depends only on strictly lower
  levels. The sweep is L5; the pending set, the key type and every gate are
  L4/L3. No new edge is needed (Constraint 6).

### Source seams

- **`src/runtime/interactive.cpp:383-391`** — the sweep, verbatim:

  ```cpp
  if (expired) {
    ++d_deadline_expiries;
    for (PendingTile& tile : d_pending.tiles) {
      if (!tile.done->settled()) {
        tile.done->cancel();
        ++d_tiles_cancelled;
      }
    }
  }
  ```

  No key test, no visibility test, no revision test. This loop is the whole
  behavioral change.
- **`src/runtime/interactive.cpp:369-379`** — the park. `wait_completions(deadline_at)`
  returning `false` *is* the deadline enforcement; `expired` is set there.
- **`src/runtime/interactive.cpp:332-348`** — Step 4: `deadline_at = d_clock() + budget`
  (`:336`), `dirty_ptr = first_frame ? nullptr : &dirty` (`:340`), then
  `render_frame_interactive(...)` filling frame-local `visible_plans` (`:345`).
- **`src/runtime/interactive.cpp:299-304`** — the frame-local `PullServiceImpl`
  and its `PullConfig` (`config.pending = &d_pending` at `:301`). This is where a
  `config.wanted` pointer joins it.
- **`src/runtime/interactive.cpp:246-250`** — the early-out:
  `if (!first_frame && dirty.device_rects.empty() && d_pending.tiles.empty())`.
  The pending queue keeps the loop alive; the sweep does not empty it.
- **`src/runtime/arbc/runtime/interactive.hpp:345-379`** — the member block.
  `RefinementQueue d_pending` (`:345`), `d_prior_revision` (`:349`),
  `d_prev_time` (`:350`), `d_prev_camera_scale` (`:356`), and the counters
  `d_deadline_expiries` (`:378`) / `d_tiles_cancelled` (`:379`). **There is no
  viewport member, no visible-tile set, and no per-content current revision** —
  the `Viewport` is a per-frame parameter of `render_frame` (`:246`) and is not
  stored. This absence is the task's real design problem.
- **`src/runtime/arbc/runtime/interactive.hpp:339-344`** — the load-bearing
  declaration-order comment: `d_pending` before `d_pool`, so the pool destructs
  first and its threads join before the queue's surfaces die. Retention does not
  disturb this and must not.
- **`src/compositor/arbc/compositor/refinement.hpp:77-90,114-117,132-135`** —
  `PendingTile{key, local_rect, content, stability, bytes, surface, done}`,
  `OperatorWait{output, unmet}`, `RefinementQueue{tiles, waits}`.
- **`src/compositor/refinement.cpp:72-88`** — `tile_in_flight`, the gate this
  task feeds. `:83`: `pending.key == key && !pending.done->settled() && !pending.done->cancelled()`.
- **`src/compositor/refinement.cpp:124-136`** — `record_operator_wait` (driven
  from `tile_planning.cpp:673-675` on every inexact operator render);
  **`:138-153`** — `operator_wave_pending`, deliberately *not* testing
  `cancelled()`; **`:225-226`** — the fail-drop (no insert, no damage);
  **`:237-244`** — stale-wait pruning.
- **`src/compositor/tile_planning.cpp:334-337`** — `repaint_regions(*dirty, viewport)`;
  a null `dirty` means the whole viewport. **`:360-422`** — the per-layer culls
  (invisible, zero opacity, `present_in_span`, null content, time-map failure,
  empty region, degenerate scale). **`:399-401`** — an operator layer's key
  revision is `aggregate_revision(...)`, a leaf's is the flat revision.
  **`:427-428`** — rung selection. **`:451-491`** — per-repaint-rect planning,
  merged by tile coord. **`:222`** — `TileKey` construction. **`:518-550`** — the
  in-flight gate, the wave gate, and the single render/dispatch path.
  **`:677-690`** — the record site that pushes a `PendingTile`.
- **`src/compositor/pull_service.cpp:229`** (key construction), **`:235-240`**
  (cache probe), **`:260-267`** (in-flight gate), **`:289-306`** (wave gate),
  **`:308`** (dispatch), **`:321-338`** (async record).
- **`src/cache/arbc/cache/key_shapes.hpp:64-76`** — `TileKey{content, revision,
  rung, coord, achieved_time}` with defaulted member-wise `operator==`;
  **`:157-174`** — `std::hash<arbc::TileKey>` already exists.

### Predecessor decisions this task is bound by

- `in_flight_tile_dedup` **Decision 3** — cancelled ⇒ not suppressible. Binding;
  do not revisit.
- `in_flight_tile_dedup` **Decision 4** — *"Cross-frame suppression therefore
  never fires"* today, and the guard is already correct for the narrowed world.
  This task must not need to touch either dispatch site's gate.
- `in_flight_tile_dedup` **Decision 5** — prove the mechanism **positively**
  with a counter, because a test that only asserts "a number did not grow"
  passes vacuously when the mechanism is disconnected. This task inherits that
  discipline as `tiles_retained` (Decision 4).
- `operator_refinement_wave_amplification` **Decision 4** — the wave gate treats
  a cancelled-but-unsettled input as still pending, *"and even with the sweep
  narrowed to 'no longer wanted' tiles, a tile genuinely dropped from the
  viewport can still be cancelled while an operator's recorded wait names it."*
  That is still true after this task — and Constraint 4 is why it is harmless.

## Constraints / requirements

1. **Pixel-neutral on the quiescent image.** Every deterministic golden must
   pass **byte-unchanged, no re-baselining**:
   `tests/refinement_golden.t.cpp`, `tests/refine_idempotence_golden.t.cpp`, and
   `tests/interactive_worker_default.t.cpp`'s `byte_identical` sweep. The offline
   driver has no deadline and no sweep, so its goldens cannot move at all; the
   interactive ones converge to the same image because the shipped doubles
   ignore the advisory cancel and land either way. A golden that *does* move is
   a bug in the change, not a re-baseline.
2. **Never under-approximate "wanted".** The wanted set must be an
   **over-approximation** of the frame's visible footprint. Retaining a tile
   nobody wants costs one wasted render that would have been wasted anyway;
   cancelling a tile somebody wants can strand it behind a placeholder (the bug
   in "Why", reason 2). Edge over-coverage (whole covering coord range rather
   than a sub-rect intersection) is therefore the correct direction to err in.
3. **Never strand a tile.** No pending tile may end a frame both (a) cancelled
   and (b) still needed by a live consumer. Concretely: an input named `unmet`
   by an `OperatorWait` whose `output` the frame still wants must be retained,
   even though the operator's *output* tile was wave-deferred this frame and so
   the input was never re-pulled (Constraint 4).
4. **The wave gate must not regress.** `operator_wave_pending` stays exactly as
   it is (`refinement.cpp:138-153`), cancel-agnostic. This task must neither
   depend on that property nor break it: `queue.waits` and the
   `renders_coalesced` counter must be observably unchanged for scenes that do
   not expire, and `tests/refine_idempotence_stress.t.cpp:507-508` must stay
   green.
5. **The sweep may never run against an absent wanted set.** An empty set means
   "nothing is wanted" and would degenerate to today's blanket cancel — silently.
   Pass the set **by reference** into the predicate so a null/never-populated set
   is not representable at the sweep; the compositor-side sink stays a nullable
   pointer (offline and one-shot drivers pass nothing, and they never sweep).
6. **Levelization (doc 17).** The wanted-set type and the predicate are
   `compositor` (L4), over a `cache` (L3) `TileKey`; the sweep and the set's
   owner are `runtime` (L5). `interactive.cpp` already includes
   `arbc/compositor/refinement.hpp`. **No new component edge.**
7. **No new thread-safety surface.** The sweep's added reads are of the frame's
   own plan (frame-thread-local) and of `queue.waits` (mutated only on the frame
   thread). The only cross-thread reads stay `done->settled()` /
   `done->cancelled()`, which `interactive.cpp:369-372` already makes.
8. **Counters stay honest.** `tiles_cancelled` must keep counting *only* actual
   cancels, and must not silently go to zero for every scene — during playback,
   `achieved_time` supersedes non-static tiles every frame, so cancels remain a
   live path (Decision 3). The new `tiles_retained` counts unsettled entries the
   sweep deliberately left alone; the two together partition the unsettled
   entries at expiry.
9. **Destruction order.** `d_pending` before `d_pool` (`interactive.hpp:339-344`)
   is load-bearing and unchanged: retained entries own surfaces that workers are
   still writing into, and the pool must join first. Do not reorder members.

## Acceptance criteria

**Claims-register entries** (`tests/claims/registry.tsv`, each with an
`// enforces: <claim-id>` tagged test; each requires the Decision 7 doc delta to
land the sentence first):

- `02-architecture#deadline-sweep-retains-wanted-tiles` — on deadline expiry the
  sweep cancels only pending tiles the frame no longer wants: a tile whose key
  is absent from the frame's visible footprint and from the unmet inputs of any
  live wait whose output is wanted. A still-wanted unsettled render is left in
  flight, uncancelled.
- `02-architecture#retained-tile-is-suppressed-next-frame` — a pending tile
  retained across a deadline expiry suppresses its own re-dispatch on the
  following frame that re-plans it: `requests_issued()` does not grow,
  `requests_suppressed()` does, and no second surface is allocated.
- `02-architecture#deadline-sweep-never-strands-a-waited-input` — an input tile
  named `unmet` by a live `OperatorWait` whose output the frame still wants is
  not cancelled by the sweep, even though the wave gate meant the operator never
  re-pulled it this frame.

**Claims-register entries that must be AMENDED** (their current text asserts the
blanket cancel, which this task removes — doc 16:23-26's same-commit rule):

- `tests/claims/registry.tsv:156` `02-architecture#interactive-frame-loop-bounded-by-deadline`
  — currently *"cancels expired BestEffort pending renders via
  `RenderCompletion::cancel`"*. Amend to state that the frame **returns at the
  deadline having composited the best-available fallback**, cancelling the
  pending renders it no longer wants; the bound is the park, not the cancel.
  Enforcing tests `src/runtime/t/interactive.t.cpp:690`, `:740`, `:919` — the
  assertion at `:717-718` (`pending().tiles.front().done->cancelled()`) inverts
  to `!cancelled()` plus `tiles_retained() == 1`, because that tile is exactly a
  tile the frame still wants. Add a sibling arm that *does* observe a cancel, so
  the claim still has a positive cancel witness (see the unit tests below).
- `tests/claims/registry.tsv:155` `02-architecture#interactive-default-renders-leaves-off-the-frame-thread`
  — currently asserts the frame returns *"having cancelled the in-flight
  BestEffort render"*. Amend to *"having left the still-wanted in-flight
  BestEffort render in flight"*. Enforcing assertions at
  `tests/interactive_worker_default.t.cpp:462-498`:
  `tiles_cancelled() >= cancelled_before + 1` becomes
  `tiles_retained() >= retained_before + 1` **and** `tiles_cancelled() == cancelled_before`;
  `deadline_expiries() == expiries_before + 1` and the degraded-composite
  assertion are unchanged (the degrade is the park's doing, not the cancel's).

**The headline assertion — cross-frame counter identity, one frame further out.**
`tests/interactive_worker_default.t.cpp`, a new scene beside the existing sweep
(`:690-775`). The predecessor could not tighten `:697` from `>=` to `==` because
its scenes measure `requests_suppressed() == 0` (`:749`) — they share no leaf
tile, so the intra-frame guard provably never fires there. The cross-frame guard
needs a different scene, and one that does **not** confound with the wave gate
(which would suppress the operator's *re-render* before the input could ever be
re-pulled). So: a **visible leaf layer**, not behind an operator, over a
2×2-tile viewport, with a render budget tight enough to expire while the leaf
tiles are in flight; frame *N+1* is driven with the same viewport and a dirty
region covering the leaf. Assert, across the worker sweep `{1, 2, 4, hw-1}`
(at `0` the pool is inline, nothing is ever in flight, and the scene is skipped):

- `requests_issued()` over the whole two-frame sequence `==` the number of
  **distinct `TileKey`s** the sequence needed (the oracle) — the identity the
  predecessor could only assert intra-frame;
- `requests_suppressed() >= 1` — the positive proof that the cross-frame guard
  actually fired (without it the identity passes vacuously; `in_flight_tile_dedup`
  Decision 5);
- `tiles_retained() >= 1` and `tiles_cancelled() == 0` for the sequence;
- the composited image at quiescence is byte-identical to the inline oracle's
  (Constraint 1).

**Unit tests:**

- `src/compositor/t/refinement.t.cpp` — arms for the new predicate
  `tile_wanted(queue, wanted, key)`, beside the existing `tile_in_flight` suite
  (`:324-393`): key in the wanted set → true; key absent → false; key absent but
  named `unmet` by a live wait whose `output` **is** wanted → **true**; key
  named `unmet` by a wait whose `output` is **not** wanted → false; key absent
  and `queue.waits` empty → false; settled entries do not change the answer (the
  predicate answers *wanted*, not *live* — the sweep composes it with
  `!settled()`). Enforces `#deadline-sweep-retains-wanted-tiles` and
  `#deadline-sweep-never-strands-a-waited-input`.
- `src/runtime/t/interactive.t.cpp` — the sweep's behavior, four arms:
  1. **Retained.** Visible leaf tile dispatched, deadline expires while
     unsettled → `deadline_expiries() == 1`, `tiles_retained() == 1`,
     `tiles_cancelled() == 0`, `pending().tiles.front().done->cancelled() == false`.
  2. **Superseded by revision.** Same, then an edit bumps the revision before the
     next frame → the old-revision pending is unwanted → `tiles_cancelled() == 1`.
     This is the positive cancel witness the amended claim `:156` needs.
  3. **No longer visible.** Same, then the camera pans the tile out of the
     viewport (and, separately, zooms to a different rung) → `tiles_cancelled() == 1`.
  4. **The permanent-hole regression** ("Why", reason 2). A test double that
     **honors** the advisory cancel by failing (`Completion::fail` when
     `cancelled()`), a visible leaf tile dispatched at frame *N*, expiry, then
     frame *N+1* driven with a dirty region that does **not** intersect T. Assert
     T is retained, is not failed, lands, and is present in the cache once the
     loop quiesces. On the pre-change sweep this test fails: T is cancelled,
     fails, is dropped with no damage, and never reappears. Enforces
     `#deadline-sweep-retains-wanted-tiles`.
- `src/runtime/t/interactive.t.cpp` — the wave interaction (Constraint 3): an
  operator whose output tile is wave-deferred this frame, whose `unmet` input is
  pending and whose output is still visible → expiry retains the input.
  Then drop the operator layer's visibility → the same input is cancelled.
  Enforces `#deadline-sweep-never-strands-a-waited-input`.
- `src/runtime/t/interactive.t.cpp:490-515` — the still-scene counter-delta
  block gains `tiles_retained() == retained` alongside the existing
  `deadline_expiries()` / `tiles_cancelled()` deltas.
- `tests/interactive_worker_default.t.cpp:405-428` — the `worker_count == 0` arm
  gains `tiles_retained() == 0` (inline dispatch settles before the park; nothing
  is ever in flight to retain or cancel).

**Behavioral-counter framing, never wall-clock** (doc 16:54-62, `:225-226`): every
assertion above is a counter identity or a queue-state predicate. The reduction
in duplicate renders under deadline pressure is a benchmark to *trend*
(`src/runtime/bench/interactive_bench_workloads.hpp`), never a gate. No test
asserts a millisecond figure.

**Concurrency coverage:** `tests/refine_idempotence_stress.t.cpp` (60 iters ×
`{1,2,4}` workers, `[.nightly]`, TSan-full lane) gains a **tight-budget regime**
so that pendings actually survive frame boundaries under workers — the retained
entry is now read by the *next* frame's `tile_in_flight` while a worker is still
writing its surface and may settle it at any moment. The reads are the same two
atomics the park already polls (Constraint 7), but this is the first code path
that keeps a live `PendingTile` addressable across a frame boundary by design,
so it gets explicit TSan exposure rather than an argument. `renders_coalesced() > 0`
with workers / `== 0` inline (`:507-508`) must stay green unchanged
(Constraint 4).

**Coverage:** ≥ 90% diff coverage on changed lines (CI gate).

**Deferred follow-ups:** none. Everything this task surfaces is decided here.

## Decisions

### 1. "Wanted" is the frame's **visible footprint**, not the keys it planned.

The obvious implementation — collect the keys the frame's planner touched and
cancel any pending whose key is not among them — is **wrong**, and quietly so.
Planning is *repaint-scoped*: `repaint_regions(*dirty, viewport)`
(`tile_planning.cpp:334-337`) drives a per-rect planning loop
(`:451-491`), so on any frame that is not a full repaint, a tile that is
plainly visible and plainly still missing is simply **not planned** — its region
is not dirty, and nothing re-dirties a tile merely because its render is late.
Under a plan-derived wanted set, that tile is "unwanted" and gets cancelled: the
exact tile the task exists to retain, and the exact tile whose fail-drop opens
the permanent hole ("Why", reason 2). The bug would be invisible on a
whole-viewport first frame and would show up only under partial repaint — i.e.
in production, not in the smallest test.

So the wanted set is computed from **visibility**, not from repaint. In
`render_frame_interactive`'s per-layer loop, for each layer that survives the
culls (`tile_planning.cpp:360-422`) and after the rung is chosen (`:427-428`),
insert into the sink one `TileKey` per tile coord covering the layer-local
mapping of the **whole viewport** at that rung — using exactly the revision the
plan uses (the flat revision for a leaf, `aggregate_revision(...)` for an
operator layer, `:399-401`) and exactly the key construction the plan uses
(`:222`). It is an integer coord-range walk: no cache probe, no plan, no
dispatch, no render. Culled layers contribute nothing, which is what makes a
layer scrolled out of its time span correctly *un*want its tiles.

Both of the WBS note's criteria then fall out of `TileKey` equality
(`key_shapes.hpp:64-76`) with no second predicate to keep in sync:

| the note's criterion | how the footprint expresses it |
| --- | --- |
| revision superseded | an edit bumps `revision` (or the operator's aggregate), so the old key is not in the footprint |
| no longer visible | a pan changes the covering `coord` range; a zoom changes the `rung`; a cull removes the layer entirely |
| (falls out for free) | a clock advance changes `achieved_time` for a non-`Static` layer |

**Alternative rejected:** *derive wantedness in the driver from the frame-local
`visible_plans` (`interactive.cpp:345`).* It needs no compositor change at all,
which is why it is tempting. It fails twice: `visible_plans` is repaint-scoped
(above), and it contains only *visible layers* — an operator's pulled input
leaves are not layers and appear nowhere in it, so every operator input pending
would read as unwanted and be cancelled, which is the population this task most
needs to retain.

**Alternative rejected:** *keep a sticky cross-frame wanted set — a key stays
wanted until something supersedes it.* No sound pruning rule exists: without one
the set grows without bound and nothing is ever cancelled again (the counter
`tiles_cancelled` dies and a panned-away tile renders forever); with one, the
pruning rule *is* the visibility test, so you have paid for the state and still
had to write the predicate.

### 2. The predicate also honors live `OperatorWait`s — and only theirs.

A pending tile is **wanted** iff:

```
wanted.contains(key)
  || any wait in queue.waits where wanted.contains(wait.output)
                            and  wait.unmet contains key
```

The second clause is not an optimization; without it the task strands operator
inputs. When the wave gate defers an operator's output re-render
(`refinement.cpp:138-153`, doc 02:168-190), the operator does **not** render, so
it does **not** pull, so its in-flight input tiles are named by *nothing* in this
frame's footprint — the footprint contains the operator's *output* tiles, which
are a different content and a different key. Cancel those inputs and the wave
never ends: the deferred output tile keeps compositing its transient placeholder
forever, which is Constraint 3's stranding, in the one regime
(`operator_refinement_wave_amplification`'s) where it is guaranteed to happen.

The `wanted.contains(wait.output)` guard is what keeps the clause from
degenerating into "anything ever waited on is forever wanted": a wait whose
output the frame no longer wants — panned away, revision-bumped — retains
nothing, and its inputs are cancelled like any other unwanted pending. This is
exactly the case `operator_refinement_wave_amplification`'s Decision 4
anticipated (*"a tile genuinely dropped from the viewport can still be cancelled
while an operator's recorded wait names it"*), and it remains true and remains
harmless: the wave gate ignores `cancelled()` by construction, so a cancelled
input still holds its (now-unwanted) output's gate closed until it leaves the
queue, and both leave together on the next drain.

The predicate lives in `compositor` beside the gates it complements —
`bool tile_wanted(const RefinementQueue&, const WantedTiles&, const TileKey&)`
in `refinement.hpp`/`refinement.cpp` — not in the driver, because it is a
statement about `RefinementQueue` semantics (L4), and the driver (L5) should not
be the second place that knows what `OperatorWait::unmet` means. Cost is
`O(pending × waits × unmet)`, paid only on the expiry path.

### 3. A hash set for the wanted keys — and why that does not contradict `in_flight_tile_dedup`'s Decision 1.

`WantedTiles = std::unordered_set<TileKey>`, aliased in `refinement.hpp`;
`std::hash<arbc::TileKey>` already exists (`key_shapes.hpp:157-174`).

The predecessor's Decision 1 **refused** an `unordered_set` index for the
in-flight set, and it is worth being explicit about why that argument does not
reach here. Its reason was not performance but *staleness*: in-flight membership
is a function of `done->settled()` and `done->cancelled()`, two atomics a worker
flips asynchronously with no notification to the queue — so a precomputed set
would be wrong the instant a render landed, and would have to be re-validated
against the completions on every query, which *is* the linear scan, plus an
invariant to get wrong.

The wanted set has no such dependence. It is a pure function of one frame's
plan inputs — viewport, camera, revision, composition time, layer culls — built
once on the frame thread, read once on the frame thread, and discarded. Nothing
can invalidate it while it is alive. It is also naturally a *set* rather than a
list: several layers, several repaint rects and several pulls name the same key,
and the sweep wants membership, not multiplicity.

The set is a frame-scoped scratch member (`WantedTiles d_wanted_tiles;`),
`clear()`ed at the top of Step 4 rather than constructed per frame, so its
buckets survive and the frame loop does not re-allocate a hash table every
frame. It is threaded to the two producer sites as a nullable
`WantedTiles* wanted` on the frame-render config (beside the existing `pending`
member, `tile_planning.hpp:324`) and on `PullConfig` (beside
`config.pending = &d_pending`, `interactive.cpp:301`) — the same plumbing shape
`in_flight_tile_dedup` used, and the same null-tolerance, so the offline driver
and every one-shot renderer pass nothing and change not at all. The **sweep**,
by contrast, takes it by `const&` (Constraint 5): an absent wanted set means
"nothing is wanted", which is a blanket cancel wearing a disguise, and it must
not be representable at the call site that would act on it.

### 4. A new counter, `tiles_retained`, proves the narrowing positively.

`InteractiveRenderer` gains `d_tiles_retained` / `tiles_retained()`, beside
`d_tiles_cancelled` / `tiles_cancelled()` (`interactive.hpp:379`, `:297`),
bumped once per unsettled pending the expired sweep deliberately leaves alone.
Together the two partition the unsettled entries at expiry, so a test can assert
the sweep *looked at* the queue and made a decision, rather than that a number
failed to grow.

This is `in_flight_tile_dedup`'s Decision 5 applied to this task, and the
predecessor's own Status block is the cautionary tale: its headline `==` could
not land because `requests_suppressed()` turned out to be `0` in the very scenes
the identity was asserted over — the mechanism provably never fired, and every
"did not grow" assertion had been passing vacuously the whole time. Without
`tiles_retained`, "the sweep cancelled nothing" is indistinguishable from "the
deadline never expired", "the tile settled in time", and "someone deleted the
sweep".

**Alternative rejected:** *infer retention as `pending().tiles.size()` minus the
cancel count.* The queue also holds settled-but-not-yet-drained entries at that
moment, so the arithmetic is wrong exactly when the timing is interesting, and
it makes every test depend on drain ordering.

### 5. The sweep's blast radius is the advisory flag and nothing else — so there is no new stall class.

Worth stating explicitly, because "leave renders in flight" *sounds* like it
should grow the queue, hold memory, or keep the loop awake:

- **Queue occupancy is unchanged.** The sweep never removed an entry. Entries
  leave only via `poll_refinements`, and only when settled
  (`refinement.cpp:173-247`). A cancelled entry that never settles sits in the
  queue exactly as long as a retained one — cancellation being advisory, it does
  not even stop the worker.
- **The early-out is unchanged.** `interactive.cpp:246-250` gates on
  `d_pending.tiles.empty()`, and both a retained render (settles → drained) and a
  cancelled one (settles or fails → drained/dropped) reach empty. The still-scene
  "zero renders" promise (doc 16:58) is untouched, because it is about a scene
  with **no** pendings.
- **Memory is unchanged in kind.** A `PendingTile` owns its target `Surface`
  (`refinement.hpp:77-90`) whether it is cancelled or not, and the worker keeps
  writing into it either way; the bound is the frame's dispatch count, and the
  destruction order that makes it safe (`interactive.hpp:339-344`, pool joins
  before the queue dies) is unchanged.
- **Follow-up scheduling is unchanged.** `schedule_follow_up` is driven by
  arrival damage (`interactive.cpp:414`). Retention makes an arrival *more*
  likely, never less.

The only thing this task changes about a pending tile is whether it is told it
may abandon its work. That is why the change is safe to make in one loop.

### 6. Deadline enforcement does not move.

The frame's deadline is enforced by the park **not waiting past it**:
`wait_completions(deadline_at)` returns `false` and the loop breaks
(`interactive.cpp:369-379`). The cancel that follows is a courtesy to the
renderer, not the mechanism. The frame still returns at the deadline; it still
composites step 4's fallback for every tile that has not landed
(doc 02:127-130); the degrade is still counted by `degraded_composites`. Nothing
in the enforcement path reads `cancelled()`.

This is why amending claim `:156`
(`02-architecture#interactive-frame-loop-bounded-by-deadline`) narrows its text
without weakening its promise: the promise was always "the frame returns at the
deadline with the best available pixels", and the cancel clause was an
implementation detail that the claim's wording had frozen. What *is* being given
up is the (never-stated, never-relied-upon) property that an expired frame leaves
no un-cancelled work behind — and giving it up is the point.

### 7. Design-doc delta (rides in the implementer's commit, doc 16:23-26).

House rule from `interactive_pull_wiring` Decision 5, restated by
`interactive_worker_count_default`: *do not mint a claim id for a sentence no
design doc contains.* The three new claims and the two amended ones need the doc
to say what the sweep does. So:

- **`docs/design/02-architecture.md`, § "The frame, interactively"** — a new
  paragraph after the in-flight-dedup paragraphs (`:132-166`) and before the
  refinement-wave paragraphs (`:168`), stating: on deadline expiry the frame
  cancels the pending renders it **no longer wants** — superseded by a revision
  bump, or no longer visible at the current camera and time — and leaves the
  rest **in flight**, so the next frame that re-plans them joins the render
  already running instead of dispatching a second one; that the deadline is
  enforced by the park, not by the cancel, so narrowing it costs nothing; and
  that the blanket alternative is not merely wasteful but *unsound*, because
  cancellation is advisory (doc 03) and a conformant content that honors it
  fails the render, which is dropped with no damage — stranding a tile the frame
  still wanted behind a placeholder. This paragraph is what makes the existing
  carve-out at `:157-166` reachable rather than vestigial.
- **`docs/design/02-architecture.md:192-199`** — the wave gate's parenthetical
  currently describes the sweep as one *"which cancels every unsettled tile on
  expiry"*. Update the description (*"which cancels the unsettled tiles it no
  longer wants"*) and keep the argument, which is unaffected and in fact
  strengthened: the wave gate must ignore `cancelled()` regardless, because a
  tile dropped from the viewport can still be cancelled while a wait names it.
- **`docs/design/00-overview.md`, § "Resolved questions"** — a decision-record
  bullet in the `:239-327` style (bold lead-in claim, argument, closing *Decided
  in doc NN (§ Section)* with the task id). Lead: **"A frame cancels the renders
  it no longer wants, not the renders it could not wait for."** It is
  project-shaping: it is the first place the project states that the deadline
  bounds *the frame*, not *the work*, and it is what makes the in-flight join a
  cross-frame mechanism rather than an intra-frame one.

No doc 13 change: the operator/pull contract is untouched — an operator that
pulls an in-flight tile degrades exactly as it already does.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-12.

- Implemented `tile_wanted(queue, wanted, key)` predicate in `src/compositor/arbc/compositor/refinement.hpp` / `src/compositor/refinement.cpp` — tests visible footprint containment and live `OperatorWait` unmet-input coverage.
- Added `WantedTiles` (aliased `std::unordered_set<TileKey>`) and a frame-scoped `d_wanted_tiles` scratch member to `src/runtime/arbc/runtime/interactive.hpp`; plumbed `wanted` pointer through `TilePlanningConfig` (`src/compositor/arbc/compositor/tile_planning.hpp`) and `PullConfig` (`src/compositor/arbc/compositor/pull_service.hpp`).
- Narrowed the deadline sweep in `src/runtime/interactive.cpp` to cancel only unwanted tiles; retained tiles increment new counter `tiles_retained()`.
- Amended `docs/design/02-architecture.md` (new wanted-sweep paragraph, updated wave-gate parenthetical) and `docs/design/00-overview.md` (resolved-questions bullet).
- Amended claims `registry.tsv:155` (`#interactive-default-renders-leaves-off-the-frame-thread`) and `:156` (`#interactive-frame-loop-bounded-by-deadline`); minted three new claims: `#deadline-sweep-retains-wanted-tiles`, `#retained-tile-is-suppressed-next-frame`, `#deadline-sweep-never-strands-a-waited-input` in `tests/claims/registry.tsv`.
- Added unit `tile_wanted` suite (7 arms) in `src/compositor/t/refinement.t.cpp`; runtime sweep arms (retained / superseded-by-revision / pan+zoom / operator-waited-input retain-then-cancel / permanent-hole regression) in `src/runtime/t/interactive.t.cpp`.
- Added cross-frame counter-identity worker sweep in `tests/interactive_worker_default.t.cpp`; tight-budget `[.nightly]` TSan regime in `tests/refine_idempotence_stress.t.cpp`; amended `tests/worker_dispatch_leaf_only.t.cpp` and `tests/interactive_operator_identity.t.cpp` to assert retention for tiles now correctly retained.
