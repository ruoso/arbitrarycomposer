# audio.latency — Declared-latency pre-roll

## TaskJuggler entry

Back-link: [`tasks/45-audio.tji`](../../45-audio.tji), `task latency`
(lines 45–50). WBS note, verbatim:

> Constant `latency()` honored by the scheduler (pre-roll requests earlier);
> full PDC deferred. Doc 12.

## Effort estimate

`effort 1d` (`tasks/45-audio.tji:46`). The seam already exists and is
already documented — `LookaheadRingConfig::preroll`
(`src/audio_engine/arbc/audio_engine/lookahead.hpp:69-72`) is declared,
default `Time::zero()`, and `AudioFacet::latency()`
(`src/contract/arbc/contract/content.hpp:280`) is already in the contract.
The task is to *read* the value and extend the ring's fill lead by it, plus
the golden and behavioral-counter tests. One day is right: no new type, no
new contract surface, no signature churn — a wire-up plus its proofs.

## Inherited dependencies

- **`audio.lookahead`** — *settled* (DONE 2026, `tasks/45-audio.tji:19-25`,
  [`tasks/refinements/audio/lookahead.md`](lookahead.md)). Landed the L4
  ring (`LookaheadRing`, `src/audio_engine/lookahead.cpp`,
  `src/audio_engine/arbc/audio_engine/lookahead.hpp`), the L5 pump
  (`LookaheadPump`, `src/runtime/lookahead_pump.cpp`), and the worker pool.
  It **explicitly left this task the pre-roll seam**: Decision *"Latency
  honored as zero, seam left for `audio.latency`"* — *"Structure the
  per-contributor request-time computation so `audio.latency` inserts the
  offset additively — no signature churn later."* The `preroll` config field
  (`lookahead.hpp:69-72`) is that seam and is **read nowhere today** — wiring
  it is this task's core.
- **`audio.lookahead_recursive_prefetch`** — *settled* (DONE 2026,
  `tasks/45-audio.tji:26-32`,
  [`tasks/refinements/audio/lookahead_recursive_prefetch.md`](lookahead_recursive_prefetch.md)).
  Established that the threaded fill (`worker_count > 0`) is byte-identical
  to the inline fill for nested and below-rate scenes, and pre-assigned any
  "full-PDC-aware pre-roll interacting with the recursive fill" to this task
  ("not a new leaf"). That closes the door on spawning a successor here.

Both dependencies are settled; nothing this task needs is pending.

## What this task is

Make the lookahead scheduler honor a content's declared constant processing
`latency()`. A content declaring `latency() = k` (a live 3D engine's sound
with a pipeline delay, a lookahead limiter) needs its blocks resident
*earlier* than a zero-latency content, because a latency-introducing (`Live`
or stateful) source physically cannot produce output window `[t, t+n)` until
it has run `k` further. The ring already renders ahead of the playhead into
a horizon; this task **extends that fill lead by the declared latency** so
the latent contributor's blocks are warmed early enough that the drain never
starves on them. The mixed output blocks, their keys, and their windows are
**unchanged** — only *more* blocks (further ahead) get primed — so the drain
stays byte-identical to the zero-latency mix. Differential per-content
window re-alignment (moving *which* window a content is requested for
relative to the mix) is full PDC and stays deferred with the effects stack.

## Why it needs to be done

Doc 12 promises `latency()` is honored in the lookahead scheduler
(`docs/design/12-audio.md:192-199`). `audio.lookahead` shipped the ring
honoring `Time::zero()` only and left the offset seam idle. Downstream
consumers depend on it: `audio.device_monitor`
(`tasks/45-audio.tji:33-38`) primes the ring against a live device clock —
a latent contributor that isn't pre-rolled underruns at the device
callback; `audio.export_monitor` (`tasks/45-audio.tji:39-44`) needs the
same lead for a sample-exact offline mix. Leaving the seam dead means the
first content plugin that declares a nonzero latency silently glitches under
lookahead. This task turns the declared value into observable scheduler
behavior with a proof that pins it.

## Inputs / context

### Governing design doc

- **Doc 12 §"Sync and latency"** (`docs/design/12-audio.md:192-199`) — the
  normative promise: content declares `latency()`; *"the engine pre-rolls
  that content's requests earlier by its latency so contributions align";*
  *"v1 honors declared constant latency in the lookahead scheduler; full PDC
  (dynamic latency, latency in nested graphs' effect chains) is deferred with
  the effects stack."* This task lands a **clarifying delta** here (see
  Decisions) pinning the v1 mechanism.
- **Doc 12 §"The engine: monitors, clocking, lookahead"**
  (`docs/design/12-audio.md:150-190`) — the render-ahead ring, the
  lookahead budget (horizon, "e.g. 100–500 ms"), transport-change
  flush/re-prime. The horizon is the surface this task extends.
- **Doc 12 §"Recursion"** (`docs/design/12-audio.md:201-222`) — the
  transitive-contributor-closure warming this task must not disturb; the
  deferred clause explicitly names "latency in nested graphs' effect
  chains" as *out* of v1.

### Supporting docs

- **Doc 17 §levelization** (`docs/design/17-internal-components.md`) — the
  ring is **L4** (`audio-engine`, may name only `contract`/`cache`/`model`/
  `media`/`base`); the pump is **L5** (`runtime`, owns threads/clock/
  transport). "latency pre-roll" is already listed under the L4
  `audio-engine` contents. This task stays inside those edges (see
  Constraints).

### Code seams the implementation extends

- **The pre-roll seam (dead today):**
  `src/audio_engine/arbc/audio_engine/lookahead.hpp:69-72` —
  `Time preroll{Time::zero()}` on `LookaheadRingConfig`, with the comment
  *"the additive `-latency()` offset drops in here without a signature
  change."* `grep` finds `preroll` only at this declaration site.
- **The declared value (already in the contract):**
  `src/contract/arbc/contract/content.hpp:276-280` —
  `virtual Time latency() const { return Time::zero(); }`, doc comment:
  *"honored by the L4 lookahead scheduler's pre-roll. The contract carries
  the value only … enforcing it is runtime policy."* Reachable from the ring
  via the already-configured resolver at
  `src/audio_engine/lookahead.cpp:98-99`
  (`d_config.resolve(layer->content)->audio()`).
- **The horizon enumeration to extend:** `LookaheadRing::horizon_blocks`
  (`src/audio_engine/lookahead.cpp:39-58`) — anchors on
  `block_index_at(playhead)` and enumerates output blocks out to `horizon`
  via `cache::temporal_prefetch_ring` (`:51-52`). Called from `prime`
  (`src/audio_engine/arbc/audio_engine/lookahead.hpp:287`) and `reprime`
  (`:319`).
- **The direct-contributor enumeration to read latency from:**
  `LookaheadRing::contributions_for` / `descend`
  (`src/audio_engine/lookahead.cpp:60-137`). The depth-1 (direct)
  contributors are where the resolved `content->audio()->latency()` is read.
- **The pump's prime/reprime call sites:** `LookaheadPump::tick_once`
  (`src/runtime/lookahead_pump.cpp:138-174`) passes `d_config.horizon` into
  `prime`/`reprime` at `:164-166`. The horizon extension happens **inside
  the ring**, so this file is untouched (see Constraints/Decisions).

### Existing claims to extend, not duplicate

The 14 audio claims are `tests/claims/registry.tsv:67-80`. This task extends,
by line, `12-audio#lookahead-prepares-ahead-of-playhead`
(`registry.tsv:76`, the horizon-and-drain identity) — it does **not**
restate the byte-identical drain invariant, it constrains a new dimension of
it. A new claim (`registry.tsv:81`) is added.

## Constraints / requirements

1. **Output byte-identical to zero-latency.** The mixed output blocks,
   their `BlockKey`s, and their `PrefetchWant` windows are unchanged; the
   pre-roll only *adds* output blocks further ahead of the playhead. A
   primed ring drained in order stays byte-identical to `mix_composition`
   called directly per output window, and identical between `worker_count`
   0 and > 0 — the invariant every predecessor golden rests on
   (`registry.tsv:76,80`) must survive unchanged.
2. **Levelization (doc 17).** The horizon extension lives in the **L4**
   ring, which already holds the `d_config.resolve` seam to read
   `AudioFacet::latency()` — no new dependency, the ring names nothing above
   L4. The **L5** pump is untouched: it already passes `d_config.horizon`;
   the ring internally lifts it by the effective pre-roll. CI's levelization
   gate must stay green.
3. **No signature churn.** As `audio.lookahead` promised, the offset drops
   in additively: `LookaheadRingConfig::preroll` is *read* (as a manual
   floor), the `latency()` max is computed from the already-enumerated
   contributors, and `horizon_blocks` is called with `horizon +
   effective_preroll`. No public signature on the ring, pump, config, or
   contract changes.
4. **Direct contributors, root time (v1 boundary).** v1 reads `latency()`
   from the **depth-1 (direct) audible contributors** of the anchor block
   and maxes them, treating the value as an output-(root-)time quantity.
   Nested-graph effect-chain latency (mapping a nested contributor's local
   latency back through its time map) is the doc-12-deferred
   "latency in nested graphs' effect chains" and is **not** in scope.
5. **Degenerate = no regression.** A scene where no content declares latency
   (`latency() == Time::zero()` everywhere) yields effective pre-roll
   `Time::zero()`, so the primed-block set is byte-for-byte the shipped
   behavior — the feature adds nothing to the common path.
6. **Transport changes.** The extended horizon flows through `reprime`
   identically to `prime` (both call `horizon_blocks`), so a seek/rate/
   direction change flushes and re-primes the extended window with no
   special-casing — the existing flush/re-prime invariant
   (`registry.tsv:77`) is preserved.

## Acceptance criteria

- **Claims-register growth.** Add `tests/claims/registry.tsv:81`:
  `12-audio#latency-prerolls-declared-content` —
  *"A contributor declaring a constant processing `latency() = k` makes the
  lookahead ring extend its transitive fill lead by k: the effective
  pre-roll is the maximum declared latency among the anchor block's audible
  direct contributors, floored by the configured
  `LookaheadRingConfig::preroll`, so the ring warms output blocks k further
  ahead of the playhead; the mixed output blocks, their keys, and their
  windows are unchanged, so a primed ring drained in order stays
  byte-identical to the same scene rendered with `latency() == Time::zero()`
  and identical between `worker_count == 0` and `worker_count > 0`; a scene
  declaring no latency extends the horizon by zero (no regression); latency
  in nested effect chains and dynamic latency (full PDC) stay deferred
  (extends `12-audio#lookahead-prepares-ahead-of-playhead`)."*
  Enforced by a Catch2 `TEST_CASE` tagged
  `// enforces: 12-audio#latency-prerolls-declared-content`.
- **Byte-exact golden (no tolerance, doc 16).** In
  `src/audio_engine/t/lookahead.t.cpp`: a scene with a test contributor
  declaring `latency() = k` (a small test-only `AudioFacet` reporting a
  configurable latency, mixing a deterministic tone) drained in order is
  byte-identical to the identical scene with `latency() == Time::zero()`.
  The oracle is the zero-latency drain — no separate hand-sum needed.
- **Behavioral-counter assertion (never wall-clock, doc 16).** Assert the
  primed-block *window* grows by exactly the pre-roll: the count / max block
  index of prepared output blocks for the `latency() = k` scene equals the
  zero-latency count plus `ceil(k / block_span)`. Assert `effective_preroll`
  equals the max declared latency across audible direct contributors
  (floored by `config.preroll`), and equals `Time::zero()` for a
  no-declared-latency scene.
- **Concurrency / TSan.** Extend the pump-driven concurrency golden
  (`tests/audio_lookahead_concurrency.t.cpp`, the `worker_count` 0-vs-8
  drain-identity harness) with a `latency() = k` scene, asserting the
  threaded drain is byte-identical to the inline drain and to the
  zero-latency oracle. This runs under the existing TSan/stress
  configuration — no new concurrency seam is introduced (the ring only
  enumerates more blocks; the fill/worker path is unchanged).
- **CI diff coverage ≥ 90%** on the changed lines (the horizon-extension
  branch and the latency-max walk).
- **WBS gate.** After the closer wires `complete 100` and the note
  back-link, `tj3 project.tjp 2>&1 | grep -iE "error|warning"` is silent.
- **Registers no successor.** This task fully lands v1 constant-latency
  pre-roll. Full PDC (dynamic latency, nested-graph effect-chain latency,
  and true per-content output-window re-alignment) is a **design-level
  deferral tied to the not-yet-built effects stack** (doc 12:197-199,
  doc 13), not an agent-implementable leaf today — no seam exists to build
  against — so **no WBS successor is created**. It is recorded in the
  return summary for the parking lot, not encoded as a WBS task (which would
  orphan against a nonexistent effects stack).

## Decisions

- **v1 mechanism is fill-lead extension, not per-content key shift.** The
  ring lifts its horizon by the effective pre-roll (`horizon_blocks` called
  with `horizon + effective_preroll`); every output block's window and
  `BlockKey` is unchanged, so *more* blocks are warmed but the drain output
  is byte-identical to zero-latency.
  *Rejected:* shifting a latent content's requested window earlier by
  `latency()` in `descend`/`contribution_key` (the literal "requests earlier"
  reading). That changes the `BlockKey` the ring populates, which must equal
  the key `PullServiceImpl::pull_audio` computes for the mixer's child
  request (`lookahead.cpp:140-145`). The **mixer does not shift**, so the
  ring's key would diverge from the mixer's probe — the pre-fill would miss
  and the byte-identical-drain invariant (the backbone of every predecessor
  golden) would break. True window re-alignment requires the mix engine to
  compensate too; that is full PDC, explicitly deferred with the effects
  stack (doc 12:197-199). In a deterministic pull model there is no real
  pipeline delay to *un*-align — `render_audio(window)` returns exactly that
  window — so v1 latency is a **residency/scheduling** concern (give a
  `Live`/stateful source enough lead), and fill-lead extension is the
  faithful, output-safe realization of it.
- **Global max over direct contributors, floored by `config.preroll`.** The
  effective pre-roll is `max(config.preroll, max_c latency(c))` over the
  anchor's depth-1 audible contributors. Because output blocks are shared
  across contributors (the horizon is per-output-window, not per-content),
  the fill lead can only be extended for the *whole* ring, not per
  contributor; taking the max guarantees the worst-case latent contributor
  has enough lead, and over-fetching the others is harmless (they were
  already covered at the base horizon). `config.preroll` is retained as a
  manual floor (default `Time::zero()`), so an operator can force extra lead
  without a code change.
  *Rejected:* a per-contributor differential horizon — output blocks are the
  shared unit, so there is no cheap per-contributor lead; the differential
  only matters once output windows re-align, i.e. under full PDC.
- **Direct contributors only; nested-chain latency deferred.** v1 reads
  `latency()` from depth-1 contributors and treats it as root-time. Mapping
  a nested contributor's local latency back through its time map is the
  doc-12-deferred "latency in nested graphs' effect chains."
  *Rejected:* walking the transitive closure for latency now — the deferred
  clause is explicit, and the time-map inverse mapping is exactly the
  dynamic/PDC complexity v1 excludes.
- **Extension lives in the L4 ring, pump untouched.** The ring already holds
  `d_config.resolve` (it reads facets in `descend`), so reading `latency()`
  and lifting the horizon internally keeps the change L4-local and leaves the
  L5 pump passing `d_config.horizon` verbatim.
  *Rejected:* having the pump compute the max latency and pass an enlarged
  horizon. That would make the pump walk contributors — duplicating the
  ring's `descend` enumeration across a level boundary for no benefit, and
  the pump would need the resolver the ring already owns.
- **Design-doc delta to doc 12 §"Sync and latency".** The existing text
  ("pre-rolls that content's requests earlier … so contributions align")
  reads, in isolation, like a per-content window shift. A one-paragraph
  clarifying delta pins the v1 mechanism (fill-lead extension over direct
  contributors, output byte-identical) and the boundary against full PDC
  (output-window re-alignment), so `device_monitor`/`export_monitor` and a
  future PDC task inherit an unambiguous contract. This is a clarification
  within an already-decided feature, not a project-shaping reversal, so it
  rides doc 12 only — no doc 00 decision-record bullet.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-08.

- Extended `LookaheadRing` to read `AudioFacet::latency()` from the anchor block's audible direct contributors and compute `effective_preroll` as the max across them, floored by `config.preroll` — `src/audio_engine/lookahead.cpp`, `src/audio_engine/arbc/audio_engine/lookahead.hpp`.
- Both `prime` and `reprime` call `horizon_blocks` with `horizon + effective_preroll`, warming more output blocks ahead while keeping every block's window/key unchanged; the drain is byte-identical to zero-latency — `src/audio_engine/lookahead.cpp`.
- Added unit tests (byte-exact golden + `prepared_count`/`effective_preroll` counters) and concurrency/TSan scene with declared latency — `src/audio_engine/t/lookahead.t.cpp`, `tests/audio_lookahead_concurrency.t.cpp`.
- Registered claim `12-audio#latency-prerolls-declared-content` — `tests/claims/registry.tsv`.
- Clarifying paragraph pinning the v1 fill-lead mechanism landed in design doc — `docs/design/12-audio.md`.
- L5 pump and all public signatures untouched; levelization and byte-identical-drain invariants preserved.
