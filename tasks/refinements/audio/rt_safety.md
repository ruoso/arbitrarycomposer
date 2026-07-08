# audio.rt_safety — RT-safety enforcement

## TaskJuggler entry

`tasks/45-audio.tji:80-85`, `task rt_safety "RT-safety enforcement"`:

> RealtimeSanitizer annotations on the callback chain + debug
> allocator/refcount hooks asserting nothing forbidden on RT threads;
> build-failing, not conventional. Docs 12/16.

- `effort 1d`, `allocate team`.
- `depends !device_monitor` (the callback chain must exist before it can be
  annotated).
- Milestone: the audio milestone (`tasks/99-milestones.tji`), same family as
  `audio.device_monitor` and its follow-ons.

## Effort estimate

**1d.** The task is small because it does not invent the RT path — that
whole surface shipped with `audio.device_monitor` and its follow-ons
(`device_edge_resample`, `device_edge_decimation`, `seek_drain_realign`).
The three deliverables each ride existing machinery:

1. The `[[clang::nonblocking]]` annotations are a mechanical macro
   application over a call graph that is already enumerated (six functions,
   §"Code seams").
2. The sanitizer wiring reuses the existing `ARBC_SANITIZERS` knob
   (`CMakeLists.txt:29-35`), the `asan`/`tsan` preset pattern
   (`CMakePresets.json:24-40`), and the per-push CI matrix
   (`.github/workflows/ci.yml:35-41`) — an `rtsan` preset + one Clang lane.
3. The only non-trivial code change is converting `LookaheadPump::drain` /
   `LookaheadRing::drain` from mutex-guarded to lock-free (Decision **D2**).
   The ring is already bounded and its `Prepared` blocks pre-allocated, so
   this is a per-slot atomic-ready/generation publish, not a new data
   structure or a new allocation.

The enforcement test vehicle is the *existing* synchronous device-monitor
tests (Decision **D4**), so no device harness has to be built.

## Inherited dependencies

- **`audio.device_monitor`** — ***settled*** (DONE, git `13ca1bb`). Landed
  the full RT callback chain this task annotates: the `DeviceSink`
  pure-virtual interface (`src/runtime/arbc/runtime/device_sink.hpp`), the
  `DeviceMonitor` clock-mastering adapter with its RT callback body
  `DeviceMonitor::fill_rt` (`src/runtime/device_monitor.cpp:157-238`), the
  lock-free mastered-playhead publish, and the reference miniaudio backend
  plugin. Its refinement explicitly reserved RT-safety annotation for *this*
  task (`tasks/refinements/audio/device_monitor.md:129-130,147-149`) and
  flagged the one residual blocking op on the chain — the pump drain mutex —
  as "the reserved RT double-buffer that `audio.rt_safety` annotates
  build-failingly" (`src/runtime/arbc/runtime/device_monitor.hpp:34-36`).
  device_monitor already pins RT-callback purity *behaviorally* (a
  callback-thread render/mix/pull/alloc counter == 0,
  `device_monitor.md:235-241,311-312`); this task turns that behavioral
  assertion into a structural, build-failing one.
- **`audio.seek_drain_realign`** — ***settled*** (DONE, `be4e69e`). Added the
  `d_realign_index`/`d_realign_request` control channel consumed at the top
  of `fill_rt` and required its own extended callback to "stay
  allocation/lock/refcount-free on the RT thread ... the `audio.rt_safety`
  enforcement leaf will assert the whole callback chain — including this new
  re-seat consume — is RT-pure" (`seek_drain_realign.md:99-101,284`). This
  task delivers that assertion.
- **`audio.device_edge_resample` / `audio.device_edge_decimation`** —
  ***settled*** (DONE, `d6851fa`, `b007d78`). Added the streaming-resampler
  SRC path inside `fill_rt` (`device_monitor.cpp:194-237`). Its coefficient
  tables are generated *off* the RT thread at construction (doc 12:113-116);
  the RT-side `StreamingResampler` feed/produce loop is allocation-free and
  must stay inside the annotated boundary.

_Pending:_ none — every predecessor is DONE.

## What this task is

Turn doc 12's structural policy ("arbitrary plugin code never runs on the
audio callback", doc 12:176-181) and doc 16's mechanism ("RealtimeSanitizer
plus debug allocator hooks asserting no allocation/refcount/lock on RT
threads — a build-failing check, not a convention", doc 16:70-73) into a
running, build-failing guard over the shipped RT callback chain. Three
deliverables:

1. **Annotate the callback chain** with `[[clang::nonblocking]]` behind a
   portable `ARBC_RT_NONBLOCKING` macro (Clang expands to the attribute;
   other compilers to nothing). The chain is
   `MiniaudioSink::Impl::on_data` → `DeviceFillCallback` →
   `DeviceMonitor::fill_rt` → `drain_block` + `convert_frames` +
   `StreamingResampler` feed/produce → `LookaheadPump::drain` →
   `LookaheadRing::drain`.

2. **Make the drain lock-free** (Decision **D2**). `LookaheadPump::drain`
   and `LookaheadRing::drain` currently take `d_mutex`
   (`lookahead_pump.hpp:109`, `lookahead_pump.cpp:37`) — a blocking op the
   annotation forbids. Convert the ring→callback handoff to a lock-free
   per-index atomic-ready/generation publish over the already-bounded,
   pre-allocated ring, so the annotated graph carries no lock.

3. **Land the enforcement layers** (Decision **D1**):
   - *Layer A (Clang):* an `rtsan` CMake preset (`-fsanitize=realtime`) + a
     per-push Clang CI lane, so the annotations fire at runtime.
   - *Layer B (all compilers):* a debug `RtScope` RAII guard (a thread-local
     "RT active" flag armed for the duration of `fill_rt`) plus hooked global
     `operator new`/`operator delete` and a pool refcount/lock assertion
     that `std::abort` build-failingly if invoked while armed — riding the
     debug-hardened per-push build (doc 16:103, "RT-safety hooks").
   - *Grep-lint fallback:* a `scripts/check_rt_safety.py` in the existing
     `lint` job (doc 16:194-198) for the non-Clang matrix members.

## Why it needs to be done

Doc 12:34 makes audio's foundational promise: it "never renders on a
deadline, it renders *ahead*" — a promise that a single hidden
allocation, lock, or refcount drop on the callback would silently break
(a page-faulting `malloc`, a contended mutex, or a `shared_ptr` destructor
running a free is exactly the glitch the lookahead architecture exists to
prevent). device_monitor asserts the callback issues zero
render/mix/pull/alloc *by counter* (`device_monitor.md:311-312`), but a
counter only catches the operations it was told to count; RealtimeSanitizer
catches *any* blocking primitive in the annotated call graph, and the debug
allocator/refcount/lock guard backstops it on the non-Clang matrix. Making
the check build-failing (doc 16:72-73) means a future edit that reintroduces
a lock or an allocation on the chain cannot merge. Directly, this closes the
one RT-hostile residue the predecessors left: the pump drain mutex, which
`device_monitor.hpp:34-36` reserved for this task.

## Inputs / context

### Governing design doc — doc 12 (normative, doc 16)

- **doc 12:34** — "audio never renders on a deadline, it renders *ahead*";
  the no-deadline guarantee this task protects.
- **doc 12:172-181** — the device-monitor bullet: worker threads run the mix
  graph *off* the device thread; "the device callback only consumes
  prepared, mixed blocks. Arbitrary plugin code never runs on the audio
  callback." The structural policy this task enforces.
- **doc 12:113-116** — decimation coefficients "generated off the RT thread"
  at construction: the RT-side resampler feed/produce loop stays inside the
  annotated boundary but its table generation does not.

### Supporting doc — doc 16

- **doc 16:66-73** (tier 6, Concurrency tests) — the mechanism, verbatim:
  "The audio callback path is guarded by **RealtimeSanitizer**
  (`[[clang::nonblocking]]` on the callback chain) plus debug allocator hooks
  asserting no allocation/refcount/lock on RT threads — doc 12's 'never on
  the callback' as a build-failing check, not a convention."
- **doc 16:101-103** — per-push CI carries "the debug-hardened build
  (generation tags, mprotect'd published chunks, RT-safety hooks)". Layer B
  rides this build.
- **doc 16:194-198** — boundary rules ("no allocation in
  `[[clang::nonblocking]]` call graphs") "start as grep-based lint scripts
  in the gate and graduate to clang-tidy/clang-query"; the grep-lint
  fallback's authority.
- **doc 16:15-21** — the claims-register convention: a claim id
  `<doc-slug>#<anchor>` referenced by a `// enforces: <claim-id>` test
  comment; "CI fails if a registered claim has no live test."
- **doc 16:112-118** — the ≥90% diff-coverage hard gate.

### Design-doc delta

- **doc 12:181** (this task, same commit): a sentence appended to the
  device-monitor bullet recording that the whole callback chain — the fill,
  the ring→callback drain, and the edge format/rate conversion — is
  lock-free, allocation-free, and refcount-free, and that this is a
  build-failing guarantee (RealtimeSanitizer + debug guard, doc 16). This
  memorializes the drain's conversion from a mutex-guarded seam
  (`lookahead_pump.cpp:37`) to a lock-free one; the constitution should not
  read as if a lock still sits on the callback. Doc 16 already fully
  specifies the enforcement mechanism (lines 70-73, 194-198), so no doc-16
  delta is needed; the decision is a consequence of doc 16's standing rule,
  not project-shaping in its own right, so no doc-00 bullet.

### Code seams the implementation extends

The `[[clang::nonblocking]]` boundary — annotate each and no other
(annotating a function that legitimately blocks is the failure mode to
avoid):

- `plugins/miniaudio/miniaudio_sink.cpp:19-24` — `MiniaudioSink::Impl::on_data`,
  the C backend trampoline; the RT boundary crossing into our code.
- `src/runtime/arbc/runtime/device_sink.hpp:27` — the `DeviceFillCallback`
  typedef (comment already states "Invoked on the device's RT thread ...
  no plugin code, no allocation").
- `src/runtime/device_monitor.cpp:157-238` — `DeviceMonitor::fill_rt`, the
  callback body; the `RtScope` arms here.
- `src/runtime/device_monitor.cpp:143-155` — `DeviceMonitor::drain_block`
  (builds an `AudioBlock` over pre-allocated `d_scratch`, calls
  `pump.drain`, bumps `d_underruns` relaxed).
- `src/runtime/device_monitor.cpp:118-141` — `DeviceMonitor::convert_frames`
  (memcpy / mono↔stereo, allocation-free).
- `src/media/arbc/media/streaming_resampler.hpp` — the RT-side
  `StreamingResampler` feed/produce used at `device_monitor.cpp:194-237`.
- `src/runtime/lookahead_pump.cpp:36-42` — `LookaheadPump::drain`
  (**currently `d_mutex`-guarded**, `lookahead_pump.hpp:109`; **D2** removes
  the lock).
- `src/audio_engine/arbc/audio_engine/lookahead.hpp:146` +
  `src/audio_engine/lookahead.cpp` — `LookaheadRing::drain` (its comment
  `lookahead.hpp:141-145` already names the RT-safety invariant).

New/edited enforcement surfaces:

- `src/base/arbc/base/rt_safety.hpp` (new, `arbc::base` L0) — the
  `ARBC_RT_NONBLOCKING` macro + the `RtScope` debug guard, plus a
  `src/base/rt_safety.cpp` global `operator new`/`operator delete` override
  compiled only into the debug-hardened build (Decision **D3**).
- `CMakeLists.txt:29-35` — the `ARBC_SANITIZERS` knob (accept `realtime`);
  `arbc_build_flags` (`CMakeLists.txt:23-27`, applied via
  `cmake/ArbcComponent.cmake:32`) is the per-target flag-injection point.
- `CMakePresets.json:24-40` — add an `rtsan` preset next to `asan`/`tsan`.
- `.github/workflows/ci.yml:35-41` — add a Clang `rtsan` lane; the grep-lint
  runs in the existing `lint` job (`ci.yml:11-25`).

### Existing claims to extend, not duplicate

- `tests/claims/registry.tsv` — `12-audio#device-callback-consumes-prepared-blocks-only`
  already asserts the callback "issues zero render_audio/mix_composition/
  pull_audio invocations on the callback thread ... never an inline mix" —
  the *behavioral-counter* form. This task adds the *structural,
  build-failing* form (no allocation/lock/refcount on the annotated chain),
  a distinct claim, not a duplicate.

## Constraints / requirements

1. **The annotated call graph must be genuinely clean.** After **D2**, no
   function in the chain (§"Code seams") may lock, allocate, free, take a
   refcount, or make a blocking syscall. RealtimeSanitizer verifies this at
   runtime; the annotation is a lie (and a worse bug than none) if the graph
   still blocks. The drain lock must go, not be annotated around.
2. **Portability of the annotation.** `[[clang::nonblocking]]` is Clang-only.
   It must sit behind `ARBC_RT_NONBLOCKING`, expanding to nothing on
   GCC/MSVC, so the `-Wall -Wextra -Wpedantic -Werror` matrix
   (`CMakeLists.txt:23-27`) still builds. Enforcement on those compilers is
   Layer B + the grep-lint.
3. **No new allocation on the drain path.** The lock-free conversion reuses
   the ring's existing bounded `Prepared` storage
   (`lookahead.hpp:189-192,237`); it must not introduce a second ring, a
   heap handoff, or a per-call allocation.
4. **Coefficient/table generation stays off the RT thread.** The resampler's
   coefficient tables are built at construction (doc 12:113-116); the
   annotation covers only the RT-side feed/produce, and construction-time
   generation must remain outside `fill_rt`.
5. **Levelization (doc 17, CI-enforced by `scripts/check_levels.py`).** The
   `ARBC_RT_NONBLOCKING` macro and `RtScope` live in `arbc::base` (L0),
   below every consumer — `arbc::audio-engine` (L4, `LookaheadRing::drain`)
   and `arbc::runtime` (L5, `DeviceMonitor`/`LookaheadPump`) may depend on
   base with no new edge. The global `operator new` override is a single
   base TU; no OS-audio or device dependency enters `libarbc` (doc 17:161-172).
6. **Determinism preserved.** The lock-free drain must not change the bytes
   drained. A primed ring drained in order stays byte-identical to
   `mix_composition` per output window and identical between
   `worker_count == 0` and `worker_count > 0`
   (the existing `12-audio#lookahead-prepares-ahead-of-playhead` and
   `#device-callback-consumes-prepared-blocks-only` claims must still pass).
   On reprime/seek the producer bumps the slot generation so a consumer read
   of a flushed slot returns silence + underrun, composing with
   `seek_drain_realign`'s cursor re-seat (byte-exactness is that task's
   `12-audio#device-drain-realigns-on-transport-change` claim).
7. **Zero wall-clock in tests.** The enforcement test drives `fill_rt`
   synchronously via the existing `flush_master` barrier (Decision **D4**);
   no real device, no sleep, no wall-clock assertion.

## Acceptance criteria

- **Claims-register growth.** Add
  `12-audio#rt-callback-chain-is-nonblocking` to `tests/claims/registry.tsv`
  (closer wires the row) with an `// enforces:` test asserting: the full
  device callback chain (`fill_rt` and its transitive callees) executes
  under an armed `RtScope` with **zero heap allocations, zero lock
  acquisitions, and zero refcount operations**, and under the `rtsan` preset
  RealtimeSanitizer reports no `[[clang::nonblocking]]` violation across the
  existing device-monitor test scenarios (matched-rate drain, up/down SRC,
  post-seek realign, underrun). Description text: *"Every function on the
  device RT callback chain — the fill, the ring→callback drain, the edge
  format/rate conversion — is `[[clang::nonblocking]]`-clean: executing it
  under an armed RtScope performs zero heap allocation, zero lock
  acquisition, and zero refcount operation, and RealtimeSanitizer reports no
  violation; the check is build-failing, not a counter."*
- **Behavioral-counter assertion (Layer B).** A test arms `RtScope` around
  `fill_rt` and asserts the debug allocator/refcount/lock counters read zero
  after a full drain sweep — the structural upgrade of device_monitor's
  existing callback-purity counter (`device_monitor.md:311-312`).
- **Byte-exact goldens preserved (no regression).** The device-path and
  lookahead goldens
  (`12-audio#device-callback-consumes-prepared-blocks-only`,
  `#lookahead-prepares-ahead-of-playhead`,
  `#device-drain-realigns-on-transport-change`,
  `#device-edge-resamples-working-to-device`,
  `#device-edge-decimates-working-to-device`) stay byte-identical after the
  lock-free drain conversion, both `worker_count == 0` and `> 0`.
- **Concurrency/TSan.** The existing audio TSan lanes
  (`tests/CMakeLists.txt:76-147`, incl.
  `arbc_device_monitor_concurrency_t`) stay green under the now-lock-free
  drain — the drain↔fill handoff must be race-free by construction, and TSan
  is the check that the atomic publish/consume ordering is correct (there is
  no mutex to hide a race behind anymore).
- **`rtsan` CI lane green.** The new Clang lane builds + runs tiers under
  RealtimeSanitizer with no violation; the grep-lint (`check_rt_safety.py`)
  passes in the `lint` job for the non-Clang matrix.
- **No new conformance family.** This task touches no `Content`/operator
  contract surface, so the `arbc-testing` conformance suite is unchanged.
- **Diff coverage ≥ 90%** on changed lines (doc 16:112-118), incl. the
  lock-free drain path (the underrun/stale-generation branch is exercised,
  not `GCOV_EXCL`-excluded).
- **WBS gate.** After the closer adds `complete 100` + the refinement
  back-link, `tj3 project.tjp 2>&1 | grep -iE "error|warning"` is silent.
- **Registers no successor.** Every deliverable lands in this task; nothing
  is deferred to a WBS leaf. Open questions (none) and any judgment residue
  go to the return summary / parking lot, never to a re-audit task.

## Decisions

**D1 — Two enforcement layers, not one.** *Chosen:* Layer A is Clang
RealtimeSanitizer (`-fsanitize=realtime` + `[[clang::nonblocking]]`) in a new
`rtsan` preset + Clang CI lane; Layer B is a compiler-independent debug
`RtScope` guard (thread-local "RT active" flag) with hooked global
`operator new`/`operator delete` and a pool refcount/lock assertion that
aborts build-failingly. *Authority:* doc 16:70-73 names both — "RealtimeSanitizer
... **plus** debug allocator hooks". *Rationale:* RTSan is Clang-only, but the
per-push matrix is GCC/Clang/MSVC (`ci.yml:32-42`); Layer B keeps the guard
build-failing on every compiler and rides the already-planned debug-hardened
build (doc 16:103). The two layers are complementary, not redundant: RTSan
catches any blocking primitive by call-graph analysis; the RtScope hooks
catch allocation/refcount/lock precisely and portably. *Rejected:*
RTSan-only — leaves the GCC/MSVC lanes unguarded and the note's "allocator/
refcount hooks" unbuilt.

**D2 — Convert `LookaheadPump::drain` / `LookaheadRing::drain` to
lock-free.** *Chosen:* replace the coarse `d_mutex` on the drain path with a
per-slot atomic-ready/generation publish over the already-bounded,
pre-allocated ring — the producer (tick loop) marks a `Prepared` slot ready
with a release store; `drain` reads by index with acquire loads and copies
out, taking no lock; a reprime/seek bumps the slot generation so a consumer
read of a flushed slot returns silence + underrun. *Authority:*
`device_monitor.hpp:34-36` reserved exactly this ("the reserved RT
double-buffer that `audio.rt_safety` annotates build-failingly"); doc 12:34's
no-deadline promise. *Rationale:* the annotation forces it — a `mutex.lock()`
is a blocking op RTSan and the RtScope guard both reject; the ring is already
bounded with pre-allocated `Prepared` storage, so the change is a
publish-discipline swap, not a new structure or allocation; the
silence-on-stale-generation branch composes with `seek_drain_realign`'s
cursor re-seat. *Rejected:* (a) leaving the mutex and drawing the
`[[clang::nonblocking]]` boundary to exclude `drain` — the real device RT
thread would still block on it, so the annotation would certify a false
guarantee (Constraint 1); (b) a second lock-free ring / heap handoff — a new
allocation on the very path we are proving allocation-free (Constraint 3).

**D3 — `RtScope` + hooks live in `arbc::base` (L0).** *Chosen:* the
`ARBC_RT_NONBLOCKING` macro and `RtScope` guard in
`src/base/arbc/base/rt_safety.hpp`; the global `operator new`/`operator
delete` override in one `src/base/rt_safety.cpp`, compiled only into the
debug-hardened build. *Authority:* doc 17:50-61 levelization — base is L0,
below every consumer. *Rationale:* the macro must be visible to the L4 ring
and the L5 monitor; a global `operator new` override must be a single TU; base
is the canonical lowest home and introduces no new levelization edge
(Constraint 5). *Rejected:* placing the guard in `arbc::runtime` (L5) — the
L4 `LookaheadRing::drain` annotation could not see the macro, and a
per-component `operator new` override risks ODR/duplicate-symbol trouble.

**D4 — Enforce through the existing synchronous device-monitor tests, not a
live device.** *Chosen:* RealtimeSanitizer keys on the
`[[clang::nonblocking]]` attribute at runtime, independent of which physical
thread runs the function, so the offline `device_monitor.t.cpp` tests
(driving `fill_rt` via `flush_master`) are the enforcement vehicle; a new
test arms `RtScope` around `fill_rt`. *Rationale:* deterministic, no device
harness, no wall clock (Constraint 7); the miniaudio plugin is out-of-lib and
absent from the CI matrix. *Rejected:* requiring a live miniaudio device in
CI — non-deterministic and unavailable in headless CI.

**D5 — Grep-lint as the non-Clang backstop, not the primary check.**
*Chosen:* a `scripts/check_rt_safety.py` in the existing `lint` job asserts
no forbidden token (`new`/`malloc`/`lock`/`shared_ptr` construction) in the
`ARBC_RT_NONBLOCKING`-tagged TUs. *Authority:* doc 16:194-198 ("start as
grep-based lint scripts ... graduate to clang-tidy/clang-query"). *Rationale:*
gives every push a cheap signal even on lanes without RTSan; the runtime
RtScope guard remains the precise check. *Rejected:* making grep the primary
enforcement — it cannot see through call graphs and false-negatives on
indirect allocation.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-08.

- Annotated the RT callback chain with `ARBC_RT_NONBLOCKING` macro (wraps `[[clang::nonblocking]]` on Clang): `plugins/miniaudio/miniaudio_sink.cpp` (`on_data`), `src/runtime/device_monitor.cpp` (`fill_rt`, `drain_block`, `convert_frames`), `src/audio_engine/arbc/audio_engine/lookahead.hpp` + `src/audio_engine/lookahead.cpp` (`LookaheadRing::drain`), `src/media/arbc/media/streaming_resampler.hpp` (RT feed/produce).
- Converted `LookaheadPump::drain` / `LookaheadRing::drain` to lock-free (D2): per-slot atomic-ready/generation publish replaces the coarse `d_mutex`; stale-generation reads return silence + underrun; `src/runtime/lookahead_pump.cpp`, `src/runtime/arbc/runtime/lookahead_pump.hpp`, `src/audio_engine/lookahead.cpp`, `src/audio_engine/arbc/audio_engine/lookahead.hpp` edited.
- Created `src/base/arbc/base/rt_safety.hpp` + `src/base/rt_safety.cpp`: `ARBC_RT_NONBLOCKING` macro, `RtScope` RAII guard (thread-local armed flag), hardened global `operator new`/`delete` aborting under armed scope (debug build only); `src/base/CMakeLists.txt` wired.
- Created `src/base/t/rt_safety.t.cpp`: unit test for `RtScope` arming/counters/macro (part of `arbc_base_t`).
- Created `scripts/check_rt_safety.py`: grep-lint for the `lint` CI job, asserting no forbidden token in `ARBC_RT_NONBLOCKING`-tagged TUs; `scripts/gate` updated.
- Added `rtsan` CMake preset (`CMakePresets.json`) and Clang CI lane (`.github/workflows/ci.yml`) for Layer A RTSan enforcement.
- Added claim `12-audio#rt-callback-chain-is-nonblocking` to `tests/claims/registry.tsv`; device-monitor test (`src/runtime/t/device_monitor.t.cpp`) extended to assert zero alloc/lock/refcount under armed `RtScope` across matched-rate drain, up/down SRC, post-seek realign, and starved-underrun scenarios.
- Byte-exact golden tests and TSan concurrency lanes remain green; lock-free drain is TSan-clean by construction via atomic release/acquire ordering.
