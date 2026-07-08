# audio.seek_drain_realign — Post-seek RT drain cursor realignment

## TaskJuggler entry

`tasks/45-audio.tji:53-58`, `task seek_drain_realign` in the `audio` tree.
Verbatim note:

> "Realign the RT drain cursor to the reprimed block window on seek/rate
> change so post-seek device output is byte-exact; the device_monitor task
> lands the clock rebase + pump reprime but the RT cursor is free-running.
> Source: audio.device_monitor implementation gap; see
> tasks/refinements/audio/device_monitor.md Status block. Doc 12."

`depends !device_monitor`; wired into milestone `m6_audio`
(`tasks/99-milestones.tji:51`).

## Effort estimate

**1d.** The mechanism is a near-exact copy of the owner→RT control channel
`audio.device_monitor` already shipped for the resampler flush
(`d_resampler_flush`, `device_monitor.hpp:154-158`;
`device_monitor.cpp:263-269`; consumed in `fill_rt` at
`device_monitor.cpp:188-192`). This task adds one more owner→RT channel — a
"re-seat the drain cursor" request carrying the new start block index —
consumed at the top of `fill_rt`, plus the owner-side compute in
`master_step`. No new architecture, no new dependency, no new levelization
edge. The remaining budget is goldens (a non-degenerate post-seek tone), one
claim, one behavioral counter, and extending the existing concurrency test.

## Inherited dependencies

- **`audio.device_monitor`** — ***settled***, DONE (git `13ca1bb`). Landed the
  `DeviceMonitor` clock-mastering adapter, the RT drain path (`fill_rt` /
  `drain_block`), the single-owner mastering thread (`master_step` /
  `run_master`), and the seek/rate handling this task completes: the clock
  rebase (`device_monitor.cpp:240-244`) and pump reprime
  (`device_monitor.cpp:263-273`) are done; **the RT drain cursor
  `d_drain_index` is left un-realigned** — that is the explicit gap this task
  closes. See `tasks/refinements/audio/device_monitor.md` Constraint 8 and
  the code comment at `device_monitor.cpp:186-188` naming this leaf.
- **`audio.lookahead`** — ***settled***, DONE (git `85e244f`). The
  `LookaheadPump` / `LookaheadRing` this task realigns against: `reprime`
  recomputes the prepared-block keep-set from the new playhead
  (`src/audio_engine/arbc/audio_engine/lookahead.hpp` `reprime`), `drain`
  is the block-indexed consume path the cursor walks
  (`lookahead_pump.hpp:76`).
- **`audio.device_edge_resample`** — ***settled***, DONE (git `d6851fa`). Its
  refinement (`tasks/refinements/audio/device_edge_resample.md`, Constraint 8)
  named this leaf as its **peer** and deliberately deferred drain-cursor
  realignment here: "This task owns only the resampler's filter memory;
  realigning the RT drain cursor to the reprimed window is the peer leaf
  `audio.seek_drain_realign`." Its `d_resampler_flush` channel is the pattern
  this task mirrors, and its SRC drain path is one of the two paths this task
  must realign.

_Pending:_ none — all predecessors settled.

## What this task is

On a host `seek` / `set_rate`, `DeviceMonitor::master_step`
(`device_monitor.cpp:225-274`) rebases the mastered clock's sample origin,
flips the resampler-flush request, republishes the new playhead, and calls
`d_pump.notify_transport_change()` — which reprimes the lookahead ring so its
prepared-block window jumps to the block covering the **new** playhead. The
RT callback, however, keeps draining from `d_drain_index`
(`device_monitor.hpp:141`), a cursor set once at construction
(`device_monitor.cpp:70`) and only ever incremented in `drain_block`
(`device_monitor.cpp:156`). After a seek the reprimed ring and the
free-running cursor point at different block indices, so the device callback
either drains blocks the ring no longer holds (underrun/silence) or drains
stale in-window blocks that do not correspond to the new playhead — post-seek
device output is not byte-exact.

This task adds a second owner→RT control channel, mirroring
`d_resampler_flush`: on a rebase the owner thread computes the new start
block index (`start_block_index()` at the freshly-published playhead) and
publishes a "re-seat" request; the RT callback consumes it at the top of
`fill_rt`, assigns `d_drain_index` to the published index, and drops the
stale pre-seek working carry (`d_carry_frames` / `d_carry_pos`). Both the
matched-rate 1:1 path and the device-edge SRC path realign. The result: from
the first frame delivered after a seek/rate change, the device drain reads
exactly the blocks the ring reprimed, and the device output is byte-identical
to a fresh mix at the new playhead.

## Why it needs to be done

`audio.device_monitor` shipped the clock master and the reprime, but its own
refinement's Constraint 8 and the shipped code comment
(`device_monitor.cpp:186-188`) explicitly carved out the drain-cursor
realignment as a peer leaf. Until it lands, the device monitor's central
promise — that post-seek device output resumes byte-exact against the
reprimed window (doc 12, §"The engine: monitors, clocking, lookahead":
"play/seek flushes and re-primes the ring") — holds for the ring but not for
the drain that consumes it. The gap is masked today only because the sole
seek test (`device_monitor.t.cpp:768`) uses a degenerate 1500 Hz tone whose
period divides the 32-frame block, so every working block is identical and a
mis-aligned cursor is unobservable. This task makes post-seek device output
actually correct and pins it with a non-degenerate golden. Downstream, the
`audio.rt_safety` enforcement leaf (`tasks/45-audio.tji:78`) will assert the
whole callback chain — including this new re-seat consume — is RT-pure, so
the mechanism must stay allocation/lock/refcount-free on the RT thread.

## Inputs / context

### Governing design doc — doc 12 (normative, doc 16)

- `docs/design/12-audio.md`, §"The engine: monitors, clocking, lookahead"
  → **Device monitor** bullet: "the device callback only consumes prepared,
  mixed blocks"; "play/seek flushes and re-primes the ring". The reprime is
  designed; this task makes the *drain* honor it.
- Same section, §"Clock mastering": "the transport derives composition time
  from samples delivered". The drain cursor and the mastered clock both
  derive from the same delivered-frame position; realignment re-establishes
  that shared origin after a seek.
- §"Working format": "A device whose rate equals the working rate keeps a
  byte-for-byte 1:1 drain (no SRC cost)". The realigned matched-rate drain
  must preserve this byte-for-byte property from the first post-seek frame.

### Supporting docs

- `docs/design/16-sdlc-and-quality.md`, §"Philosophy: the design docs are an
  executable specification" (claims register + `// enforces:` tag),
  §"Test taxonomy" item 3 (byte-exact goldens, no tolerances), item 4
  (behavioral-counter tests, "blocks mixed" / underruns — never wall-clock),
  item 6 (concurrency tests: TSan on the full suite, RealtimeSanitizer /
  `[[clang::nonblocking]]` on the callback chain, debug hooks asserting no
  allocation/lock/refcount on RT threads).
- `docs/design/17-internal-components.md` levelization: the pump/monitor are
  L5 `arbc::runtime`, the ring is L4 `arbc::audio_engine`. This task edits
  only the L5 `DeviceMonitor`; it adds no new cross-level edge.

### Design-doc delta

Doc 12 promises "play/seek flushes and re-primes the ring" but is silent on
how the **device drain cursor** re-anchors to the reprimed window. This task
lands observable behavior (post-seek byte-exact device output) that a claim
enforces, so per doc 16's same-commit rule the implementer adds **one
clarifying sentence** to doc 12 §"Clock mastering" (around
`docs/design/12-audio.md:186-193`), in the same commit as the code:

> On such a rebase the device drain cursor is re-seated to the block covering
> the reprimed playhead — the pre-change working carry is dropped — so
> post-seek/-rate device output is byte-exact from the first delivered frame.

This is a doc-12 clarification of already-designed behavior, not a new
architectural decision — **no doc-00 decision-record bullet** (mirroring the
`audio.device_edge_resample` / `audio.latency` discipline; only a
project-shaping change earns a doc-00 bullet). The sentence supplies the
`// enforces:` anchor `12-audio#device-drain-realigns-on-transport-change`.
The implementer applies this delta; the refinement does not edit doc 12
ahead of implementation (which would leave the doc describing unshipped
behavior).

### Code seams the implementation extends

- `src/runtime/device_monitor.cpp:225-274` — `master_step`, the owner-thread
  seek/rate handler. The rebase branch (`:240-244`), the resampler-flush
  request (`:263-269`), and `d_pump.notify_transport_change()` (`:270`) are
  where the new drain-realign request is computed and published (owner
  already holds the post-seek transport position; publish `start_block_index()`
  at that position — the same position it publishes to `d_published` at
  `:259`).
- `src/runtime/device_monitor.cpp:161-223` — `fill_rt`, the RT callback. The
  realign consume goes at the very top (before the `if (!d_resampling)`
  branch, so both paths realign), immediately adjacent to the existing
  resampler-flush consume at `:188-192`.
- `src/runtime/device_monitor.cpp:107-120` — `start_block_index()`, the
  floored playhead→block-index compute reused for the realign target (today
  called only at construction, `:70`).
- `src/runtime/device_monitor.cpp:147-159` — `drain_block`, where
  `d_drain_index` advances (`:156`); realign re-seats the same field.
- `src/runtime/arbc/runtime/device_monitor.hpp:139-158` — the RT-callback-owned
  state block (`d_drain_index` `:141`, carry `d_carry_frames`/`d_carry_pos`
  `:142-143`) and the cross-thread published surface (`d_resampler_flush`
  `:158`, the exemplar for the new channel).
- `src/runtime/arbc/runtime/device_monitor.hpp:104` — `flush_master()`, the
  deterministic (wall-clock-free) test barrier; `:106-113` the behavioral
  counters (`delivered_frames` / `underruns` / `master_steps`) the new
  counter joins.

### Existing claims to extend, not duplicate

- `tests/claims/registry.tsv:83`
  `12-audio#device-callback-consumes-prepared-blocks-only` — "the bytes
  drained through the device path equal a direct `mix_composition` oracle …
  byte-identical between `worker_count == 0` and `worker_count > 0`". The new
  claim **extends** this to the post-seek case.
- `tests/claims/registry.tsv:77`
  `12-audio#lookahead-transport-change-flushes-and-reprimes` — the ring-side
  reprime this task's drain now tracks. **Preserved, not restated.**
- `tests/claims/registry.tsv:82` `12-audio#device-clock-masters-transport` —
  "a host seek/rate change rebases the master's sample origin … and reprimes
  the pump". **Preserved:** this task adds the drain-cursor half of the same
  rebase without altering the clock/reprime half.
- `tests/claims/registry.tsv:159`
  `12-audio#device-edge-resamples-working-to-device` — the SRC path's
  resampler-flush guarantee. **Preserved:** realignment re-seats the block
  cursor; the resampler flush already handles the filter memory.

## Constraints / requirements

1. **RT-callback purity (doc 12 / doc 16 item 6).** The realign consume runs
   on the device thread and must add no allocation, lock, refcount, or plugin
   call. It re-seats `d_drain_index` and resets the two carry counters — all
   RT-owned scalars — gated by an `exchange`/`load` on an atomic control flag.
   The `d_drain_index` field stays RT-single-owner: the owner thread only ever
   writes the *request* (a separate atomic index + flag), never
   `d_drain_index` itself.
2. **Single-owner Transport preserved.** `start_block_index()` reads
   `d_transport.position()`; it must be evaluated on the **owner thread**
   inside `master_step` (which already owns the transport), and only the
   resulting integer block index is published to the RT thread. The RT thread
   never touches the transport.
3. **Both drain paths realign.** The matched-rate 1:1 path
   (`fill_rt:166-182`) and the device-edge SRC path (`fill_rt:184-223`) must
   both consume the realign request. The realign check therefore precedes the
   `d_resampling` branch. On the SRC path the realign and the existing
   resampler flush both fire on the same rebase (block cursor re-seated,
   filter memory reset); they are independent control flags but co-triggered.
4. **Drop the stale carry.** Realignment sets `d_carry_frames = 0` and
   `d_carry_pos = 0`, discarding the partial pre-seek working block — the same
   reset the resampler-flush path already performs
   (`device_monitor.cpp:190-191`).
5. **Block granularity matches construction.** The realign target is
   `start_block_index()` (the floored block covering the new playhead) —
   identical to the construction-time alignment (`device_monitor.cpp:70`) and
   to the ring's own `block_index_at(playhead)` reprime granularity. Sub-block
   seek phase (starting mid-block) is **out of scope**: it is neither a
   regression this task introduces nor a behavior any predecessor delivered;
   the device drain has always been block-granular. (Not a WBS leaf — see
   Open questions / return summary.)
6. **Exactly-once, deterministic.** One rebase (`master_step` with a pending
   seek/rate) publishes exactly one realign request; the RT callback consumes
   it exactly once (`exchange(false)`), so a rebase followed by N ordinary
   fills realigns on the first fill and no other. Observable via the
   `flush_master()` barrier, not wall-clock.
7. **byte-identity across worker counts (doc 16 item 3).** Post-seek drained
   bytes must be byte-identical between `worker_count == 0` and
   `worker_count > 0`, and byte-identical to a fresh `mix_composition` oracle
   at the new playhead. No tolerances.
8. **Levelization (doc 17).** Changes confined to L5
   `src/runtime/device_monitor.{hpp,cpp}`; no new edge into L4
   `arbc::audio_engine` (the ring's `drain`/`block_index_at` seams already
   exist).

## Acceptance criteria

- **Claims-register growth.** Add one row to `tests/claims/registry.tsv`:
  `12-audio#device-drain-realigns-on-transport-change` — *On a host
  seek/set_rate the device RT drain cursor is re-seated to the block covering
  the reprimed playhead and the pre-change working carry is dropped, so from
  the first post-change frame the bytes drained through the device path equal
  a fresh `mix_composition` oracle at the new playhead, byte-identical between
  `worker_count == 0` and `worker_count > 0`.* Enforced by a Catch2 block
  tagged `// enforces: 12-audio#device-drain-realigns-on-transport-change` in
  `src/runtime/t/device_monitor.t.cpp`. Extends
  `12-audio#device-callback-consumes-prepared-blocks-only`
  (`registry.tsv:83`); preserves `:77`, `:82`, `:159` (restate none).
- **Byte-exact goldens (no tolerances, doc 16 item 3).** In
  `src/runtime/t/device_monitor.t.cpp`, drive a **non-degenerate** tone whose
  period does *not* divide the working block (so consecutive blocks differ and
  a mis-aligned cursor produces observably wrong bytes — replacing / adjacent
  to the deliberately degenerate 1500 Hz case at `device_monitor.t.cpp:768`).
  For both the matched-rate path and the device-edge SRC path: drain M blocks,
  `seek` to a block-aligned target T, `flush_master()`, drain N more, assert
  the post-seek drained bytes are byte-for-byte equal to a fresh
  `mix_composition`/resample oracle evaluated at T, and byte-identical between
  `worker_count == 0` and `worker_count > 0`. Add a `set_rate` variant.
- **Behavioral-counter assertion (never wall-clock, doc 16 item 4).** Expose a
  wall-clock-free counter `drain_realigns()` on `DeviceMonitor` (mirroring
  `underruns()`, `device_monitor.hpp:110`), incremented each time the RT
  callback consumes a realign request. Anchored on the `flush_master()`
  barrier (`device_monitor.hpp:104`): assert one seek + `flush_master()` + one
  fill ⇒ `drain_realigns() == 1`; a plain advance (no rebase) ⇒ no increment;
  and a realigned post-seek drain against a resident reprimed window issues
  **zero** additional `underruns()` (the reprimed blocks are present, so the
  realigned cursor finds prepared blocks — a performance-shaped promise pinned
  by a counter, not a timer).
- **Concurrency / TSan (doc 16 item 6).** Extend
  `tests/device_monitor_concurrency.t.cpp` (seek-under-race at `:358`, `:467`)
  so the seek-storm assertion covers the new owner→RT realign channel:
  reprimed post-seek drain byte-identical to the single-threaded inline
  goldens under a seed-perturbed seek storm, TSan-clean. RealtimeSanitizer /
  no-alloc-on-RT lint must stay green on the extended `fill_rt`.
- **No new conformance family.** This is engine-internal behavior, not a
  content kind or operator; the contract conformance suite is not exercised.
- **Diff coverage ≥ 90%** on the changed L5 lines (doc 16 CI gate).
- **WBS gate.** After the closer sets `complete 100` and appends the Status
  block, `tj3 project.tjp 2>&1 | grep -iE "error|warning"` is silent.
- **Registers no successor.** Every deliverable lands within this leaf. The
  device-rate < working-rate decimating SRC is already the separate WBS leaf
  `audio.device_edge_decimation` (`tasks/45-audio.tji:47`); sub-block seek
  phase is not agent-implementable net-new work (Constraint 5) and is surfaced
  to the parking lot, not the WBS.

## Decisions

**D1 — Realign via a second owner→RT control channel, mirroring
`d_resampler_flush`.** The owner thread computes `start_block_index()` at the
post-seek playhead and publishes it (a `std::atomic` index + a
release-stored request flag); the RT callback `exchange`-consumes the flag
(acquire) at the top of `fill_rt` and assigns `d_drain_index`. Authority:
this is the exact idiom `audio.device_monitor` already ships for the
resampler flush (`device_monitor.hpp:154-158`; `device_monitor.cpp:263-269`,
`188-192`), keeping `d_drain_index` RT-single-owner and the transport
owner-single-owner.
*Rejected:* the owner thread writing `d_drain_index` directly — violates the
RT-single-owner discipline the whole engine buys (`device_monitor.hpp:139`,
"touched only on the device thread") and would need `d_drain_index` promoted
to atomic + a happens-before with every `drain_block`, adding shared mutable
audio state on the hot path.
*Rejected:* folding the realign into the existing `d_resampler_flush` flag —
the resampler flush exists only when `d_resampling` and resets DSP filter
memory; the drain realign must fire on the matched-rate path too and re-seats
a block index, a distinct concern. Co-triggered, separately expressed.

**D2 — Realign target is the floored `start_block_index()` at the new
playhead.** It equals the construction-time alignment
(`device_monitor.cpp:70`) and the ring's own reprime granularity
(`block_index_at(playhead)`), so the drain and the reprimed window share one
origin. Authority: doc 12 §"Clock mastering" ("the transport derives
composition time from samples delivered") — both cursor and clock derive from
the same position.
*Rejected:* a sub-block-precise cursor (fractional/sample-exact seek phase) —
the device drain has always been block-granular; making seek sample-exact is
a broader change touching the ring's window model, not this 1d realignment,
and no predecessor delivered it. Surfaced to the parking lot.

**D3 — Compute `start_block_index()` on the owner thread, publish only the
integer.** `start_block_index()` reads the transport; the owner already owns
it inside `master_step`. Authority: single-owner Transport (device_monitor
Constraint; `transport.hpp:26-30`).
*Rejected:* the RT thread calling `start_block_index()` — reads the transport
from the device thread, breaking single-owner and adding a data race on
`Transport::position()`.

**D4 — Drop the pre-seek working carry on realign.** Sets
`d_carry_frames = 0` / `d_carry_pos = 0`, discarding the partial block staged
before the seek. Authority: the resampler-flush path already does this
(`device_monitor.cpp:190-191`); a stale partial block would emit pre-seek
samples after the seek, defeating byte-exactness.
*Rejected:* preserving the carry — emits pre-change audio post-seek; the very
bug the task fixes.

**D5 — A non-degenerate golden tone.** The realignment is unobservable under
the existing 1500 Hz / 32-frame degenerate tone (`device_monitor.t.cpp:768`,
every block identical). The new golden uses a tone whose period is coprime
with the block length so a mis-aligned cursor yields wrong bytes. Authority:
doc 16 item 3 (goldens must actually distinguish correct from incorrect).
*Rejected:* reusing the degenerate tone — cannot detect the very failure the
claim asserts against.

## Open questions

(none — all decided.) Sub-block (sample-exact) seek phase is a deliberate
scope exclusion (D2 / Constraint 5), surfaced to the parking lot for human
triage rather than encoded as a WBS leaf.

## Status

**Done** — 2026-07-08.

- `src/runtime/arbc/runtime/device_monitor.hpp` — added `drain_realigns()` counter, `d_drain_realigns`, and the owner→RT realign channel (`d_realign_index` / `d_realign_request`).
- `src/runtime/device_monitor.cpp` — RT consume at top of `fill_rt` (re-seats `d_drain_index`, drops carry, counts); owner publishes `start_block_index()` + request in `master_step`'s rebase branch; refreshed SRC-path comment.
- `docs/design/12-audio.md` — one clarifying sentence added to §Clock mastering; supplies the `// enforces:` anchor `12-audio#device-drain-realigns-on-transport-change`.
- `tests/claims/registry.tsv` — new claim row `12-audio#device-drain-realigns-on-transport-change`.
- `src/runtime/t/device_monitor.t.cpp` — non-degenerate 300+700 Hz tone goldens (matched-rate + SRC × seek + set_rate, worker 0 == 4, byte-exact); behavioral-counter assertion (`drain_realigns() == 1` after one seek + flush + fill); claim-tagged Catch2 block.
- `tests/device_monitor_concurrency.t.cpp` — both concurrency / TSan tests extended with a seed-perturbed seek storm exercising the new realign channel.
