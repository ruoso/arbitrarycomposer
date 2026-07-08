# lookahead_recursive_prefetch — Threaded recursive pre-fill for nested contributors

## TaskJuggler entry

`task audio.lookahead_recursive_prefetch` in
[`tasks/45-audio.tji`](../../45-audio.tji) (lines 26–31):

> "Warm nested (recursive descent) and below-rate native contributor
> BlockKeys in the LookaheadPump's fill so the threaded path
> (`worker_count>0`) covers recursion depth + native re-requests,
> completing the nested-of-tones byte-exact golden under `worker_count>0`.
> Source: audio.lookahead implementation gap; see
> tasks/refinements/audio/lookahead.md Status block. Doc 12."

## Effort estimate

1d (per the `.tji`). This is a targeted extension of a shipped seam, not a
new subsystem: the ring, worker pool, pump, and inline (`worker_count==0`)
golden already exist from `audio.lookahead`. The work is (a) making the
fill's contributor enumeration recursive and rate-aware, (b) gating
dispatch/mix on transitive residency, and (c) extending the goldens and
concurrency coverage to drive nested-of-tones through the threaded pump.

## Inherited dependencies

- **`audio.lookahead`** (settled — Done 2026-07-08,
  [`tasks/refinements/audio/lookahead.md`](lookahead.md)). Ships the
  `LookaheadRing` (`src/audio_engine/arbc/audio_engine/lookahead.hpp` +
  `src/audio_engine/lookahead.cpp`), the `AudioWorkerPool`
  (`src/runtime/arbc/runtime/audio_worker_pool.hpp` +
  `src/runtime/audio_worker_pool.cpp`, with the `worker_count==0` inline
  mode), the `LookaheadPump`
  (`src/runtime/arbc/runtime/lookahead_pump.hpp` +
  `src/runtime/lookahead_pump.cpp`), and the `PullConfig::blocks` /
  `PullConfig::audio_dispatch` bindings in
  `src/compositor/arbc/compositor/pull_service.hpp` +
  `src/compositor/pull_service.cpp`. Its Status block names this task as its
  single explicit deferral (`lookahead.md:484`).
- **`audio.mix_engine`** (settled — the recursion + below-rate resampling
  behavior this fill must warm ahead of). `mix_layer`'s varispeed child
  pull and its below-rate second native-rate pull + `resample_audio` live
  in `src/audio_engine/mix.cpp`; the nested facet's duplicate walk lives in
  `src/kind_nested/nested_content.cpp`.
- **`audio.audio_types`** (settled — the working format `AudioFormat`,
  `k_working_audio = {48000, Stereo}`, and the per-composition working rate
  the keys are parameterized against).

Pending downstream (not blockers, informational): `audio.device_monitor`,
`audio.latency`, `audio.spatial_policy`, `audio.rt_safety` all `depend
!lookahead` and inherit the now-complete threaded fill.

## What this task is

`audio.lookahead`'s fill enumerates only the **direct** layers of the root
composition. `LookaheadRing::contributions_for`
(`src/audio_engine/lookahead.cpp:59-106`) walks
`for_each_layer_in(d_config.composition)` — one level — and forms one
`BlockKey` per direct contributor at the working rate
(`contribution_key`, `src/audio_engine/lookahead.cpp:108-118`), classifying
those keys `Temporal` via `prime_ring` and reporting the absent ones as the
want-list (`LookaheadRing::prime`,
`src/audio_engine/arbc/audio_engine/lookahead.hpp:179-224`). A block is
mixed only once *all its direct contributors* are resident
(`lookahead.hpp:104-109`, `:219-220`).

Two classes of `BlockKey` the mixer actually reads are never enumerated by
this single-level walk:

1. **Nested (recursive-descent) contributors.** A direct contributor that
   is itself a nested composition renders via
   `NestedAudioFacet::render_audio`
   (`src/kind_nested/nested_content.cpp:569-608`), which settles **inline**
   by mixing each child layer (`mix_child_layer`, `:470,520,525`), pulling
   the nested composition's own leaf blocks. `contributions_for` does not
   descend into that nested composition, so those leaf `BlockKey`s are never
   warmed.
2. **Below-rate native re-requests.** When a contributor's `achieved_rate <
   child_rate`, the mixer issues a *second* pull at the native rate and
   `resample_audio`s it up (`src/audio_engine/mix.cpp:115-162`, mirrored in
   `nested_content.cpp:515-525`). That native-rate pull is a distinct
   `BlockKey` (the `rate` field differs — `src/cache/arbc/cache/key_shapes.hpp:83-90`),
   never warmed by the working-rate-only enumeration.

Under `worker_count==0` this is invisible: `AudioWorkerPool::submit_inline`
(`src/runtime/audio_worker_pool.cpp:65-67,87-120`) runs every render — and
every inner recursive/native re-request pull it triggers — synchronously on
the calling thread, so the whole tree resolves in place and the inline
nested-of-tones golden (`tests/nested_audio_goldens.t.cpp:131`) passes.
Under `worker_count>0` the worker running a nested contributor's inline
`render_audio` re-enters `pull_audio`, whose miss dispatches **async**
(`src/compositor/pull_service.cpp:336-341`); the descendant is not resident
when the nested render reads it, so the pass mixes silence for it
(`src/audio_engine/mix.cpp:102-106`) and the drained bytes diverge from the
inline oracle. This task makes the fill warm the **transitive contributor
closure** — recursive descent + below-rate native re-requests — so that by
the time a worker renders any contributor its whole subtree is already
resident, and the threaded drain is byte-identical to inline.

## Why it needs to be done

The `audio.lookahead` Status block leaves exactly one thing deferred
(`lookahead.md:484`): *"threaded recursive pre-fill for nested + below-rate
native contributors under `worker_count>0`."* Until it lands, the threaded
device-monitor path — the *production* path for interactive playback — is
correct only for flat native-leaf scenes
(`tests/audio_lookahead_concurrency.t.cpp:236` drives `worker_count=8` with
flat `SineLeaf` contributors, never a nested composition through the pump).
Any scene with a nested composition or a below-native-rate contributor
would glitch (silence-mixed layers) under the real threaded scheduler. Doc
12's whole premise is that *"audio never renders on a deadline"* and *"a
late block is a glitch"* (`docs/design/12-audio.md:31-34`); a threaded fill
that mixes silence for un-warmed descendants breaks that premise for the
recursive case. `audio.device_monitor` (the interactive sink that runs the
pump with real worker threads) depends on this being correct.

## Inputs / context

**Governing design-doc sections (normative — doc 16):**

- `docs/design/12-audio.md:155-164` — Device monitor: renders ahead into a
  ring, *"worker threads execute `render_audio` pulls and the mix graph off
  the device thread"*, flush/re-prime on transport change.
- `docs/design/12-audio.md:31-34` — *"audio never renders on a deadline, it
  renders ahead"*; a late block is a glitch (no graceful degradation).
- `docs/design/12-audio.md:180-185` — the block cache 1D key
  `(content id, revision, block index, rate)`, temporal prefetch ring as
  primary fill driver.
- `docs/design/12-audio.md:201-208` **plus the same-commit delta appended
  here** — Recursion: a nested composition's audio facet mixes its children
  through the child working format + embedding time map + gain, budgets
  flow through per doc 05. The delta (lines added after `:208`) makes the
  transitive-closure warming and `worker_count>0` ↔ `worker_count==0`
  byte-identity **normative** — doc 12 was previously silent on both (see
  Decisions → design-doc delta).
- `docs/design/12-audio.md:23-25,58-61` — `achieved_rate` (native rate if
  lower than requested); the engine resamples below-rate content. This is
  the below-rate re-request's root cause.
- `docs/design/12-audio.md:94-104` — working sample rate (default 48 kHz);
  the nesting boundary converts (resample + remix); *"homogeneous trees pay
  nothing."*
- `docs/design/05-recursive-composition.md:24-26` (*"Rendering is
  recursion"*), `:68-70` (recursion-depth budget backstop), `:95-100`
  (budgets flow through nesting, *"the recursive case must cost what an
  equivalent flat scene would"*).
- `docs/design/12-audio.md:139-143` — Flat-mode recursion termination:
  `gain < 1` converges (feedback echo, well-defined when the cycle's offset
  is ≥ one block), `gain ≥ 1` hits the doc-05 depth budget + diagnostic.

**Source seams this task extends:**

- `src/audio_engine/lookahead.cpp:59-106` — `contributions_for`, the
  single-level contributor enumeration + culls to make recursive.
- `src/audio_engine/lookahead.cpp:108-118` — `contribution_key`, the
  `BlockKey` construction (must also produce below-rate native re-request
  keys).
- `src/audio_engine/arbc/audio_engine/lookahead.hpp:179-224` —
  `LookaheadRing::prime`: the `prime_ring` → want-list → `all_resident` →
  `mix_block` gate (residency invariant at `:104-109`, `:219-220`) to
  extend transitively.
- `src/runtime/lookahead_pump.cpp:90-139` (`fill_and_insert`, the
  `d_pool.submit` per want + cache insert) and `:141-177` (`tick_once`, the
  bounded `for (round = 0; round < 8; …)` prime/fill loop — the fixpoint
  driver).
- `src/runtime/audio_worker_pool.cpp:65-84` (submit branch),
  `:87-120` (inline), `:122-151` (`run_task` → `render_audio`),
  `:153-185` (worker loop).
- `src/compositor/pull_service.cpp:301` (child `BlockKey` construction the
  ring must reproduce), `:336-341` (nested re-entry `++d_depth /
  audio_dispatch`), `:275-281` (depth backstop).
- `src/audio_engine/mix.cpp:115-162` — below-rate native re-request +
  `resample_audio`; `nested_content.cpp:470,515-525` — the nested facet's
  duplicate.
- `src/cache/arbc/cache/key_shapes.hpp:83-90` — `BlockKey` fields
  (`content`, `revision`, `block_index`, `rate`; layout deliberately not a
  key field).

**Predecessor decisions carried forward** (from `lookahead.md`): the L4
ring / L5 pump+pool split; the ring holds mixed **output** blocks while the
`BlockCache` holds per-content blocks; the drain path is pure-consume
(never mixes inline); the fill reuses `cache::prime_ring` /
`temporal_prefetch_ring`; retain-on-reprime, re-mix only invalidated
blocks. The recursive fill must preserve every one of these.

## Constraints / requirements

1. **Transitive closure, not one level.** The fill enumerates the full
   contributor tree the mixer would walk: recursive descent through nested
   composition contributors (applying each layer's composed rational
   `time_map` per descent, never accumulated — doc 11's varispeed rule) and
   the below-rate native re-request key for any contributor whose resident
   `achieved_rate < child_rate`.
2. **Structural re-expression, no plugin/render calls from the ring.** The
   ring (`arbc::audio-engine`, L4) must not acquire a dependency on
   `arbc::runtime` (L5), `arbc::kind_nested` (L3), or the pull service's
   render path. The recursive descent is a *structural* walk of the
   composition tree plus rational time-map math plus **cache reads** of the
   resident block's `achieved_rate` — the same way `contributions_for`
   already "re-expresses" `mix_layer`'s cull for the root's direct layers
   (`lookahead.cpp:74-76`), and the same accepted duplication as
   `mix_child_layer` vs `mix_layer`. Doc 17 levelization must stay green
   (CI-enforced) — the ring continues to name only `cache::BlockKey` /
   `KeyedStore` / the model, never `runtime`. If descent needs a capability
   the ring structurally lacks, supply it as an **injected enumerator** on
   the ring config (mirroring the pump's injected `tick_source` /
   `BlockCache`), populated by the runtime pump — do not up-level the ring
   into runtime.
3. **Dispatch/mix gating extends the residency invariant transitively.** A
   contributor (leaf or nested) is dispatched to a worker only once its own
   contributor closure is resident; a root output block is mixed only at
   **full transitive residency**. This preserves the shipped invariant
   *"a threaded fill never mixes silence for a not-yet-rendered
   contributor"* (`lookahead.hpp:104-109`) recursively — the fill must warm
   bottom-up (descendants before their parents).
4. **Below-rate keys discovered across rounds.** The native re-request
   `BlockKey` cannot be known before the working-rate pull settles
   (`achieved_rate` is a render *result*). The existing bounded round loop
   (`lookahead_pump.cpp:158`, 8 rounds) is the fixpoint driver: a round
   emits newly-unblocked wants and, on the next round, reads freshly-resident
   `achieved_rate` to emit native re-request wants, until the want-list
   empties. The loop must remain **bounded** — a runaway self-referential
   scene terminates on the doc-05 depth budget, not by spinning the round
   cap.
5. **Warm exactly the mixer's tree — honor budget + culls.** The descent
   respects the shared `GraphBudget.max_depth = 64`
   (`docs/design/05-recursive-composition.md:68-70`) threaded through, never
   reset per pull, and the Flat-mode culls (`gain ≤ 0`, `!audible()`,
   facet-less, sub-audible, out-of-span). Warming past the culls both wastes
   work and diverges from the inline golden. `gain < 1` self-referential
   feedback converges block-by-block (each turn reads a prior block already
   prepared-or-silence — the ring already owns feedback); the pre-fill must
   not attempt an infinite tree.
6. **Determinism preserved.** Threaded and inline fills must produce
   **byte-identical** samples for every scene, including nested and
   below-rate. No tolerances (doc 16 — deterministic rendering gets
   byte-exact goldens).
7. **No regression to the flat/shipped path.** The existing flat
   multi-tone and single-level goldens, the transport reprime counters, and
   the concurrent fill+drain TSan case (`tests/audio_lookahead_concurrency.t.cpp`)
   must continue to pass unchanged.

## Acceptance criteria

1. **Nested-of-tones byte-exact golden through the threaded pump (the
   task's named deliverable).** Extend the nested-of-tones oracle currently
   inline-only in `tests/nested_audio_goldens.t.cpp:131` (or add a sibling
   in `src/runtime/t/lookahead_pump.t.cpp`) to drive the same two-tone
   nested composition through `LookaheadPump` + `AudioWorkerPool` with
   `worker_count > 0`, drain the ring, and assert the drained samples are
   **byte-identical** to (a) the inline `render_tone` direct-mix oracle and
   (b) the `worker_count == 0` drain. Deterministic, no tolerance.
2. **Below-rate native re-request golden through the threaded pump.** A
   nested/native contributor with `achieved_rate < child_rate` (reuse the
   `tests/nested_audio_resampling_goldens.t.cpp` fixture semantics) driven
   through the threaded pump drains byte-identical to the inline path (which
   already resamples via the 16-tap Blackman-Harris / 32-phase polyphase
   `resample_audio`).
3. **New claims-register entry.** Register
   `12-audio#lookahead-warms-recursive-contributor-closure` in
   `tests/claims/registry.tsv`, worded: *"The lookahead fill warms the
   transitive contributor closure — recursive descent through nested
   compositions and the below-rate native re-request a resampling boundary
   provokes — dispatching a contributor only once its own closure is
   resident and mixing an output block only at full transitive residency;
   the threaded fill (`worker_count > 0`) is byte-identical to the inline
   fill for nested and below-rate scenes, never mixing silence for a
   not-yet-rendered descendant."* Enforced by the goldens in criteria 1–2
   (`enforces:`-tagged). This **extends** — does not duplicate —
   `12-audio#lookahead-prepares-ahead-of-playhead` (`registry.tsv:76`,
   whose `worker_count==0` ↔ `>0` identity covered only the flat/multi-tone
   case) and `12-audio#lookahead-fills-block-cache-through-prefetch-ring`
   (`registry.tsv:78`, single-level prime); cite both by line as source,
   not duplicate.
4. **Behavioral-counter assertions (doc 16 — no wall-clock).** For a
   primed nested-of-tones scene under `worker_count > 0`:
   - the drain/consumer thread issues **0** `render_audio` and **0**
     `pull_audio` dispatches (drain is pure-consume);
   - every nested contributor's `render_audio`, when a worker runs it,
     finds its descendants already resident — inner `pull_audio` during the
     nested render issues **0** dispatches (all cache hits);
   - `AudioWorkerPool::tasks_submitted` equals the **transitive** closure
     size (each leaf, each nested contributor, and each below-rate native
     re-request counted exactly once), demonstrating the fill enumerated
     the whole tree rather than one level;
   - no output block is mixed while any transitive contributor is absent
     (a silence-mixed-contributor counter stays **0**).
5. **TSan / stress concurrency (doc 16 requires it for the audio engine).**
   Extend `tests/audio_lookahead_concurrency.t.cpp` (today flat
   `SineLeaf`s at `worker_count=8`) with a nested-of-tones + below-rate
   scene driven through the threaded pump under TSan: every
   `AudioCompletion` settles exactly once, no data race across the recursive
   fill (including the per-content serialization gate under recursive
   re-entry), and the drained samples equal the inline-mode golden. Wire the
   target in `tests/CMakeLists.txt` if a new file is added.
6. **No regression.** The flat/single-level goldens, reprime counters, and
   existing concurrency case pass unchanged (constraint 7).
7. **Diff coverage ≥ 90%** on changed lines (CI gate).
8. **WBS gate.** After the closer adds `complete 100` + the `Refinement:`
   back-link to the task block in `tasks/45-audio.tji`,
   `tj3 project.tjp 2>&1 | grep -iE "error|warning"` is silent.
9. **Registers no successor.** Every below-rate / recursion facet this task
   promises is covered by the goldens above; nothing is deferred. (If, on
   implementation, a genuinely separable follow-up surfaces — e.g.
   full-PDC-aware pre-roll interacting with the recursive fill — it is
   already owned by `audio.latency`, not a new leaf.)

## Decisions

- **D1 — Recursive pre-fill (transitive-closure warming), driven to a
  fixpoint by the existing bounded pump round loop.** The fill warms the
  whole subtree so every worker's inner recursive/native-re-request pull is
  a cache hit (0 dispatch, synchronous-by-hit), and a contributor is
  dispatched only once its closure is resident.
  *Rejected: making the inner pull synchronous under threaded mode* (run the
  nested render's re-entrant `pull_audio` inline on the worker thread) — the
  task note mandates pre-fill; a worker blocking on work it must itself
  dispatch risks deadlock against the per-content serialization gate and
  defeats the cache-first design.
  *Rejected: a separate recursion-warming thread* — redundant with the pump's
  already-bounded round loop (`lookahead_pump.cpp:158`).
- **D2 — The ring re-expresses the recursive descent structurally; it never
  calls L3 nested or L4 pull-service render code.** Descent is a structural
  composition-tree walk + rational time-map math + cache reads of resident
  `achieved_rate`, mirroring `contributions_for`'s existing single-level
  re-expression of `mix_layer` and the accepted `mix_child_layer` /
  `mix_layer` duplication.
  *Rejected: the ring calling into nested content* — a doc-17 levelization
  violation (L4 → L3), CI-failing.
  *Rejected: up-leveling the whole recursive fill into the runtime pump* —
  abandons the ring's pure-unit testability and the L4/L5 split decided in
  `lookahead.md`. Where descent needs a capability the ring lacks, an
  injected enumerator on the ring config keeps the split intact.
- **D3 — Below-rate native re-request keys are discovered lazily from
  resident `achieved_rate`, across pump rounds.** The native key can't be
  known before the working-rate pull settles; the existing 8-round bounded
  loop is the fixpoint driver.
  *Rejected: statically pre-declaring native keys* — `achieved_rate` is
  unknown at enumeration time.
  *Rejected: an unbounded round loop* — a self-referential scene must
  terminate on the doc-05 depth budget, not spin.
- **D4 — Dispatch/mix gating extends the single-level residency invariant
  transitively.** A contributor is dispatched only once its own closure is
  resident; a root block mixes only at full transitive residency —
  preserving *"a threaded fill never mixes silence for a not-yet-rendered
  contributor"* (`lookahead.hpp:104-109`) recursively, warming bottom-up.
  *Rejected: dispatching nested renders eagerly and letting their inner
  pulls miss → silence* — that is precisely the current bug.
- **D5 — Warm exactly the mixer's tree; honor the shared depth budget and
  Flat-mode culls.** The pre-fill walks the same tree the mixer walks — no
  more (wasted work + golden divergence), no less (silence).
  *Rejected: warming past the mixer's culls or resetting the depth budget
  per pull* — diverges from the inline golden and from doc 05's
  "cost what an equivalent flat scene would."
- **Design-doc delta (same commit).** Doc 12 was silent on both full-tree
  warming vs lazy rendering and on `worker_count` inline/threaded
  byte-identity (the fill driver at `:180-185` and the ring at `:155-164`
  state neither). Per doc 16's same-commit rule, a paragraph is appended to
  the **Recursion** section of `docs/design/12-audio.md` (after line 208)
  making the transitive-closure warming, the dispatch-at-closure-residency
  gate, and the `worker_count>0` ↔ `worker_count==0` byte-identity
  normative, bounded by the doc-05 depth budget and the Flat-mode culls.
  This is a within-audio correctness property, not project-shaping, so no
  `docs/design/00-overview.md` decision-record bullet is added.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-08.

- `src/audio_engine/arbc/audio_engine/lookahead.hpp` — injected `nested_composition` enumerator + `max_depth` on config; recursive `Contribution.children`; templated `collect_wants` (bottom-up gating + lazy below-rate discovery); `prime` rewrite to transitive residency; `silence_mixed()` counter.
- `src/audio_engine/lookahead.cpp` — recursive `descend`, `make_want`, `native_rerequest_want` extending `contributions_for` to full transitive closure.
- `src/audio_engine/t/lookahead.t.cpp` — 3 ring unit tests: recursive gating, below-rate native re-request, Flat-mode cull.
- `src/runtime/arbc/runtime/lookahead_pump.hpp` and `src/runtime/lookahead_pump.cpp` — wired recursive config into pump.
- `src/runtime/t/lookahead_pump.t.cpp` and `src/runtime/t/audio_worker_pool.t.cpp` — golden tests for nested-of-tones + below-rate through the threaded pump at `worker_count` 0/4, byte-identical to inline oracle; behavioral-counter assertions (closure-size `tasks_submitted`, 0 inner dispatch, `silence_mixed==0`, pure drain).
- `tests/audio_lookahead_recursive.t.cpp` (new) + `tests/CMakeLists.txt` — new integration golden target for recursive/below-rate scenes through the full pump.
- `tests/audio_lookahead_concurrency.t.cpp` — TSan nested+below-rate case, verified race-free 3×.
- `tests/claims/registry.tsv` — new claim `12-audio#lookahead-warms-recursive-contributor-closure`.
- `docs/design/12-audio.md` — normative delta appended to the Recursion section making transitive-closure warming and `worker_count>0` ↔ `worker_count==0` byte-identity explicit.
