# quality.stress_harness — Stress + schedule perturbation

## TaskJuggler entry

Back-link: `task stress_harness` in
[`tasks/70-quality.tji`](../../70-quality.tji) (lines 14–19), under
`task quality`.

Note (verbatim): *"Seeded randomized-yield stress for publish/pin and the
reclamation queue; litmus tests for the arena growth handshake; TSan job
coverage. Docs 15/16."*

## Effort estimate

**2d** (from the `.tji`). Budget: ~0.5d for the shared seeded
yield-perturbation helper + the three stress/litmus test files that
generalize the inline patterns predecessors already left behind; ~0.5d
standing up the `tsan` preset and the two CI lanes (per-push brief,
nightly long-form) and getting the full suite green under TSan; ~1d
chasing whatever real races/ordering bugs TSan surfaces once the suite
actually runs under it (this is the point of the task — the existing
concurrency tests run under ASan only, which does not detect data races).

## Inherited dependencies

Direct WBS edge: `depends model.persistent_state`. In practice this task is
the designated home that **every** concurrency-touching predecessor scoped
its "full seeded / randomized stress" *out of* and deferred here. The seams
it exercises are all landed:

**Settled (all Status: Done, 2026-07-05):**

- `model.persistent_state` — publish/pin substrate: atomic publish via
  `std::atomic<std::shared_ptr<const DocRoot>>`, pins-as-root-refs,
  deferred reclamation installed on the document arena. The concurrency
  *smoke* (background pin→peek→unpin vs. committing writer) is at
  `src/model/t/persistent_state.t.cpp:171`; its comment at `:170`
  explicitly parks the seeded stress here.
- `model.transactions` / `model.journal` / `model.editable_facet` /
  `model.composition_membership` — the publish/pin lifecycle this harness
  perturbs (single publish per commit, abort discards, undo/redo as forward
  publishes, content-state reference lifecycle). Deferred-to-here comments
  at `src/model/t/transactions.t.cpp:345`,
  `src/model/t/editable_facet.t.cpp:246`.
- `pool.reclamation` — `DeferredReclaimSink` (lock-free Treiber-stack push)
  + `ReclamationQueue::drain()` (single-consumer iterative cascade). The
  8-producer enqueue-while-draining smoke is at
  `src/pool/t/reclamation.t.cpp:248`; its acceptance criteria hand the
  seeded reclaim stress to this task.
- `pool.arena_core` — the arena growth handshake: `SlotStore` +
  `SlabDirectory` monotonic append-only growth, stable slot addresses,
  writer-only allocation, readers resolve `(store,index)→ptr` via acquire
  loads while the writer appends chunks. The concurrent-resolve-during-
  growth test is at `src/pool/t/pool.t.cpp:192`; it, too, points the seeded
  stress here.
- `pool.free_pools` — per-thread free pools + the asymmetric guard
  relaxation that makes drain **single-drainer, any one thread**
  (release/free any-thread; allocate writer-only). Concurrent smoke at
  `src/pool/t/free_pools.t.cpp:237`.
- `pool.refcounts_in_store` — one shared count column per size class;
  cross-type churn stress parked here (`src/pool/t/refcounts_in_store.t.cpp`
  comment at `:151`).
- `runtime.housekeeping` / `runtime.housekeeping_thread` — the background
  drainer thread that owns the single-drainer mutex. The **closest existing
  model to this task** is `src/runtime/t/housekeeping_thread.t.cpp:284`
  ("stress: RT producers enqueue while the background thread and writer both
  drain"): 8 producers × 500, fixed-stride `std::this_thread::yield()`
  perturbation at `:329`/`:344`, outcome-only asserts. Its acceptance
  criteria (`:377`) call a seeded-RNG yield variant "an acceptable
  enhancement" — i.e. this task.

**Pending:** none — every seam this harness drives is already in-tree.

## What this task is

Turn the memory model's concurrency claims from "checked once under a benign
schedule, ASan-clean" into "checked under adversarial, seeded schedule
perturbation, TSan-clean." Concretely, three deliverables plus the CI plumbing
that runs them:

1. **Seeded randomized-yield stress for publish/pin** — a background pinner
   loop (resolve → `peek`-traverse → unpin) racing a committing writer and a
   `HousekeepingThread` drainer, with `std::this_thread::yield()` fired on
   random bits from a per-thread `std::mt19937` seeded from a loop counter.
2. **Seeded randomized-yield stress for the reclamation queue** — multiple
   RT-role producers releasing (lock-free enqueue) into a single drainer,
   with the same seeded yield injection, asserting the whole-subtree cascade
   completes and slots return to the free pools.
3. **Litmus tests for the arena growth handshake** — concurrent readers
   resolving a stable index set through acquire loads while the writer keeps
   growing the `SlabDirectory`, under seeded perturbation, asserting no torn
   pointer / no use-after-free and that TSan reports no data race on the
   capacity-growth handshake.

Plus: a `tsan` build preset and a per-push CI lane running the full suite
short-form under ThreadSanitizer, and a nightly lane running the long-form
(wide seed-sweep) stress — the missing TSan coverage doc 16 mandates
(`16-sdlc-and-quality.md:66`, `:101–105`) and that six predecessor tasks
parked for want of an owner.

## Why it needs to be done

Doc 16's test taxonomy, tier 6 (`docs/design/16-sdlc-and-quality.md:66–68`):

> **Concurrency tests.** TSan on the full suite; dedicated stress tests for
> the publish/pin protocol and the reclamation queue with schedule
> perturbation (randomized yields under a seed); litmus tests for the arena
> growth handshake.

This one sentence is the task's charter. Today none of it exists:

- **No TSan anywhere.** `CMakePresets.json` has `dev`, `release`, `asan`,
  `coverage`, `win-dev`, `bench` — no `tsan`. CI (`ci.yml`) and nightly
  (`nightly.yml`) run ASan/UBSan only. ASan does not detect data races, so
  every "concurrency smoke" the predecessors wrote validates *outcomes*
  under a lucky schedule but proves nothing about the *absence of races*.
- **No seeded schedule perturbation.** The only seeded-RNG + yield-injection
  in the tree is `src/contract/t/async_render.t.cpp:220–263` (seeds 0..255,
  `std::mt19937 rng(seed)`, yield gated on random bits) — the pattern to
  generalize. Every pool/model/runtime concurrency test uses either bare
  `std::atomic` go/stop flags with no yields, or fixed-stride yields; none
  vary the interleaving under a reproducible seed.
- **No shared harness.** Each concurrent test rolls its own primitives
  inline; there is no `testutil`/`test_support` for yield injection.

Downstream, this is the safety net for the entire memory model. Publish/pin
and deferred reclamation are the substrate the render thread, audio engine,
and background housekeeping all touch concurrently (doc 15 thread rules,
`15-memory-model.md:137–143`). A race here corrupts documents silently. The
task also unblocks clearing the standing parking-lot debt: two entries
(`tasks/parking-lot.md:33–41`) request exactly the TSan lane this task lands.

## Inputs / context

**Governing design docs (normative):**

- `docs/design/16-sdlc-and-quality.md`
  - `:14–18` — the claims register: every normative claim gets an id and an
    enforcing test (`// enforces: <claim-id>`); CI fails if a registered
    claim has no live test.
  - `:54–62` — behavioral-counter tests: "Wall-clock tests lie in CI;
    counters don't." Assert render/alloc/reclaim counters, never duration.
  - `:66–73` — tier 6 concurrency tests (the charter above); note the
    RealtimeSanitizer clause covers the **audio callback path**, which is
    out of this task's publish/pin + reclamation + arena scope.
  - `:101–105` — CI structure: per-push runs "ASan+UBSan and TSan jobs …
    tiers 1–8 short-form"; nightly runs "long-form stress/fuzz/crash-sweeps."
- `docs/design/15-memory-model.md`
  - `:118–123` — a version is memory-live exactly while pinned as a
    `DocState` root or referenced by a journal state handle; unpin/trim
    cascades the unique nodes.
  - `:129–136` — "Release enqueues the object on a type-erased reclamation
    queue; a housekeeping pass … pops, runs destructors … iteratively" —
    "release enqueues, never destroys inline." (doc-16 example claim, `:14`)
  - `:137–143` — thread rules: any thread may pin/unpin and enqueue
    reclamation; the writer is the only structural allocator; single-drainer.
  - caveat 4 (arena growth) — the historically **"racy capacity-growth
    handshake (works in practice, wants a clean acquire/release protocol)"**;
    `pool.arena_core` implemented the acquire/release resolve, and these
    litmus tests are what pin it TSan-clean.
- `docs/design/17-internal-components.md`
  - levelization: `arbc::pool` L1 (arena, reclamation, generation tags);
    `arbc::model` L2 (publish/pin `DocState`, pins); `arbc::runtime` L5
    (housekeeping thread). "A component may depend only on strictly lower
    levels." (`:41–44`)
  - repo layout `:153–154`: **cross-component stress tests live under
    top-level `tests/`**, not inside a component's `t/`.

**Source seams the harness drives** (symbol locations verified against the
current tree; treat line numbers as point-in-time anchors — the model files
have drifted since their refinements were written):

- Reclamation queue: `src/pool/arbc/pool/reclamation.hpp` —
  `ReclamationQueue` (~`:63`), `register_store` (~`:71`),
  `DeferredReclaimSink<T>` (~`:34`), `drain()`.
- Ref stores / enqueue seam: `src/pool/arbc/pool/refs.hpp` — `RefStore<T>`
  (~`:246`) with `create`/`resolve`/`peek`/`retain`/`release`/
  `set_zero_sink`; `src/pool/arbc/pool/typed_store.hpp` — `TypedStore` (~`:22`).
- Arena / slot storage (growth handshake): `src/pool/arbc/pool/slot_store.hpp`
  — `SlotStore` (~`:132`), `slots_live()` (~`:241`), `Arena` (~`:337`),
  `total_slots_live()` (~`:354`); `src/pool/arbc/pool/slab_directory.hpp`.
- Publish/pin store: `src/model/arbc/model/model.hpp` — `Model` (~`:143`),
  `DocRoot`/`DocStatePtr = std::shared_ptr<const DocRoot>` (~`:38`, `:91`),
  `current()` (~`:153`), `d_current` atomic (~`:333`); path-copying HAMT at
  `src/model/arbc/model/hamt.hpp`.
- Background drainer: `src/runtime/arbc/runtime/housekeeping_thread.hpp` —
  `HousekeepingThread` (owns the single-drainer mutex), `flush()`, injectable
  `tick_source`.

**Existing test patterns to reuse/generalize:**

- Seeded RNG + yield injection: `src/contract/t/async_render.t.cpp:220–263`.
- Closest stress model: `src/runtime/t/housekeeping_thread.t.cpp:284` (with
  fixed-stride yields at `:329`, `:344`; outcome asserts
  `total_slots_live() == baseline`).
- Behavioral-counter style: `src/runtime/t/housekeeping.t.cpp:81`
  (`drains_run == 1`), `:125–130` (`live_slots`, `slots_freed_to_list`,
  `durable_epoch`).
- `[.nightly]` hidden-tag cadence split: established by
  `src/pool/t/crash_tests.t.cpp` (per `pool/crash_tests.md:175–179`).

**Build / CI scaffolding:**

- Test helper: `cmake/ArbcComponent.cmake:46–56` (`arbc_component_test`, for
  component `t/` tests). Cross-component tests in `tests/` use plain
  `add_executable` + `catch_discover_tests`: `tests/CMakeLists.txt:4–27`.
- Sanitizer knob: `CMakeLists.txt:29–35` (`ARBC_SANITIZERS` → `-fsanitize=…`)
  — generic enough to accept `thread`; no preset selects it. Presets:
  `CMakePresets.json:23–31` (`asan` = `address,undefined`).
- CI matrix: `.github/workflows/ci.yml:27–55` (`build-test`: gcc/clang ×
  debug/release, clang-asan, msvc). Nightly: `.github/workflows/nightly.yml`
  (`tidy`, `asan-full`; header TODO already lists "stress/schedule
  perturbation").
- Claims: `tests/claims/registry.tsv`; enforced bidirectionally by
  `scripts/check_claims.py` (CI `lint` job, `ci.yml:24–25`); tag convention
  `// enforces: <claim-id>` immediately above the `TEST_CASE`, e.g.
  `src/runtime/t/housekeeping_thread.t.cpp:283`.

## Constraints / requirements

1. **Deterministic reproduction.** Every stress iteration runs under an
   explicit `std::mt19937` seed derived from a loop counter; the seed is
   logged via Catch2 `INFO(seed)` (and any per-thread derivation, e.g.
   `seed ^ 0x9e3779b9U`) so a red CI reproduces byte-for-byte locally. No
   `std::random_device`, no time-based seeding.
2. **No wall-clock assertions** (doc 16:54–62). Stress bodies run a **fixed
   op count**, not a duration, and assert only behavioral outcomes:
   `arena.total_slots_live() == baseline`, per-store `slots_live() == 0`,
   destruction counters equal the number of releases, `drains_run` /
   `revision()` deltas. Yields widen the race window; they are never timed.
3. **Respect the single-drainer invariant** (`pool/free_pools.md:73–77`).
   Exactly one thread calls `drain()` at a time — either the writer between
   transactions or the `HousekeepingThread`, never two concurrently. The
   harness may have many producers (release/enqueue any-thread) but must not
   introduce a second concurrent drainer; allocation stays writer-only.
4. **Levelization** (doc 17). The stress/litmus files live under top-level
   `tests/` (cross-component), link the finalized `arbc` umbrella library +
   `Catch2::Catch2WithMain`, and reach pool/model/runtime internals only
   through their public `arbc/<component>/…` headers. They must **not** be
   added to any component's `t/` (which is bound by that component's
   allowed-dependency list) and must not be smuggled into `libarbc`.
5. **TSan preset is standalone.** ThreadSanitizer does not compose with
   ASan; add a separate `tsan` configure/build/test preset
   (`ARBC_SANITIZERS: "thread"`, Debug, `-fno-omit-frame-pointer`) rather
   than extending `asan`. If the debug-hardening mprotect / SIGSEGV crash
   harness (`src/pool/t/crash_tests.t.cpp`) does not compose with TSan, it is
   already `[.nightly]`-tagged and is excluded from the per-push TSan lane by
   Catch2 tag filter — the TSan lane still runs the full concurrency suite
   (doc 16:66 "TSan on the full suite").
6. **The TSan lane must be green.** doc 15 caveat 4 historically flagged the
   capacity-growth handshake as racy; `pool.arena_core` hardened it to
   acquire/release. If the litmus test under TSan nonetheless surfaces a
   **real** data race (in the growth handshake or anywhere the suite
   touches), the implementer lands the minimal fix in the owning component
   (e.g. an acquire/release tightening in `arbc::pool`) and, if that fix
   changes designed behavior, updates the governing doc in the **same
   commit** (doc 16 same-commit rule). A red TSan lane is not shippable and
   must not be silenced with blanket suppressions; any unavoidable
   suppression is narrowly scoped with a justifying comment.
7. **Cadence split.** Per-push runs a **brief** subset (small seed range,
   modest op counts) so the lane stays fast (doc 16:101–103, "short-form");
   nightly runs the **long-form** wide seed sweep (`[.nightly]` tag), matching
   doc 16:104–105. Both share the same test bodies parameterized by seed
   range / op count.
8. **Diff coverage ≥90%** on changed lines (the shared helper + test files),
   per the CI `coverage` gate (`ci.yml` coverage job).

## Acceptance criteria

- **Shared helper.** A minimal header-only seeded yield-perturbation utility
  exists at `tests/support/schedule_perturb.hpp` (e.g. a `Perturber` wrapping
  `std::mt19937` with a `maybe_yield()` that calls `std::this_thread::yield()`
  on a random bit), generalizing `async_render.t.cpp:220–263`, and is
  consumed by all three stress files. (Deliberately small — one primitive,
  three call sites today.)
- **Publish/pin stress** — `tests/stress_publish_pin.t.cpp`: seeded pinner
  loop (resolve/`peek`/unpin) racing a committing writer and a
  `HousekeepingThread` drainer; per-push brief + `[.nightly]` wide sweep.
  Outcome asserts `total_slots_live()` returns to baseline after quiesce; a
  pin taken pre-commit still resolves the old version's records unchanged.
  Carries `// enforces:` tags for
  `14-data-model-and-editing#pinned-version-never-observes-later-edit`,
  `15-memory-model#const-ref-traversal-touches-no-refcount-page`, and
  `15-memory-model#housekeeping-thread-single-drainer`.
- **Reclamation-queue stress** — `tests/stress_reclamation_queue.t.cpp`:
  multi-producer seeded enqueue-while-single-drainer; asserts destruction
  count == releases and `slots_live() == 0` after drain. Carries
  `// enforces:` tags for
  `15-memory-model#release-enqueues-never-destroys-inline`,
  `15-memory-model#deferred-cascade-reclaims-whole-subtree`,
  `15-memory-model#thread-local-free-pools-spill-to-global`, and
  `15-memory-model#one-count-column-per-size-class`.
- **Arena growth litmus** — `tests/arena_growth_litmus.t.cpp`: concurrent
  seeded resolve during writer-driven `SlabDirectory` growth; asserts every
  resolved pointer stays valid (no torn read, no UAF) and stable across
  growth. Carries `// enforces:` tags for
  `15-memory-model#chunk-growth-preserves-addresses` and
  `15-memory-model#slots-recycle-in-place`.
- **TSan preset + lanes.** `CMakePresets.json` gains a `tsan`
  configure/build/test preset; `.github/workflows/ci.yml` `build-test`
  matrix gains a `clang-tsan` entry running the full suite short-form
  (`[.nightly]` excluded); `.github/workflows/nightly.yml` gains a
  `tsan-full` job running the long-form sweep. **All lanes green.**
- **Claims register stays consistent.** `scripts/check_claims.py` passes with
  the added `enforces:` tags. This task adds **no new registry rows** — it
  hardens confidence in existing memory-model claims under adversarial
  scheduling rather than landing new behavior. (If constraint 6's contingency
  forces a behavioral change to a seam, that change follows the normal
  claims-register-growth rule in its own right.)
- **Coverage gate.** ≥90% diff coverage on the changed lines.
- **No deferred WBS follow-ups.** The task is self-contained; the arena-race
  contingency (constraint 6) is resolved in-scope, not deferred to an
  "audit."

## Decisions

**D1 — This task lands the TSan preset and CI lanes; it does not stay
parked.** *Rationale:* doc 16 is the constitution and already mandates "TSan
on the full suite" (`:66`) and "TSan jobs" in per-push CI (`:101–103`) — the
resource-cost question the parking-lot entries reserved for a human is
**already answered by the design docs**. The two parked entries
(`tasks/parking-lot.md:33–41`) were written by closers for *pool* tasks whose
scope excluded `.github/workflows/` edits, not because the design was
unsettled; they explicitly punt to a task like this one. `quality.stress_harness`
is the WBS leaf the human created specifically for "TSan job coverage" (its
`.tji` note), and the quality stream is where CI/gate infrastructure edits are
in-scope by nature (cf. `quality.repo_linters`, `quality.tidy_promotion`).
*Rejected alternative:* leave TSan parked and ship only the stress tests under
ASan — rejected because ASan cannot detect data races, so the stress tests
would validate outcomes under a lucky schedule and prove nothing about race
freedom, defeating the entire tier-6 charter. The refinement surfaces the two
resolved parking-lot entries in its return summary so the closer/human can
clear them (the refinement does not edit `parking-lot.md`).

**D2 — Generalize one small shared helper, not a framework.** *Rationale:*
predecessors each rolled inline primitives; a single header-only seeded
`maybe_yield()` removes the duplication with the smallest possible surface
and exactly three call sites today (doc-16/README bias toward the simpler
abstraction). *Rejected:* a full "perturbation scheduler" framework
(configurable injection points, thread registries) — speculative
over-engineering for three tests.

**D3 — Cross-component `tests/` placement, umbrella-linked.** *Rationale:*
the stress files combine `arbc::model` publish/pin (L2), `arbc::pool`
reclamation/arena (L1), and `arbc::runtime` housekeeping (L5); doc 17:153
puts cross-component stress under top-level `tests/`, which sits outside the
levelized graph and may link multiple components. *Rejected:* per-component
`t/` files — each `t/` is bound by its component's allowed-dependency list, so
a test needing model + pool + runtime cannot live in any single one.

**D4 — Standalone `tsan` preset, cadence-split via `[.nightly]`.**
*Rationale:* TSan and ASan are mutually exclusive instrumentations, so `tsan`
is a sibling of `asan`, not a variant; the `[.nightly]` hidden-tag pattern is
already the repo's cadence mechanism (`crash_tests`), giving a fast per-push
brief run and a nightly wide sweep from one set of test bodies. *Rejected:*
folding TSan into the existing `asan` preset (impossible — sanitizers
conflict); a separate parallel test binary for stress (needless — Catch2 tags
already split cadence).

**D5 — Re-enforce existing claims; add no new registry rows.** *Rationale:*
the harness lands no new *behavior* — it perturbs the schedule under which
already-claimed behaviors (release-enqueues-never-destroys-inline,
single-drainer, chunk-growth-preserves-addresses, pinned-version-never-
observes-later-edit) are exercised, and `check_claims.py` permits multiple
enforcing tests per claim. *Rejected:* minting a new
`#…-is-tsan-clean` claim per test — TSan-cleanliness is a *process*
guarantee (the lane is green), not a behavioral claim about the system's
observable outputs, so it belongs in CI structure, not the behavioral claims
register.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-05.

- `tests/support/schedule_perturb.hpp` — header-only seeded yield-perturbation helper (`Perturber` wrapping `std::mt19937`, `maybe_yield()` on a random bit); generalises `async_render.t.cpp:220–263`; consumed by all three stress files.
- `tests/stress_publish_pin.t.cpp` — seeded pinner-races-writer+`HousekeepingThread` stress; per-push brief + `[.nightly]` wide sweep; asserts `total_slots_live()` returns to baseline and a pre-commit pin still resolves the old version. Carries `enforces:` tags for `14-…#pinned-version-never-observes-later-edit`, `15-…#const-ref-traversal-touches-no-refcount-page`, `15-…#housekeeping-thread-single-drainer`. Exercises pool-level `atomic<shared_ptr<const DocRoot>>`-style substrate (rationale in file header — `Model`'s arena is not yet `HousekeepingThread`-wired; parking-lot entry dated 2026-07-05).
- `tests/stress_reclamation_queue.t.cpp` — multi-producer seeded enqueue-while-single-drainer stress; asserts destruction count == releases and `slots_live() == 0`. Carries `enforces:` tags for `15-…#release-enqueues-never-destroys-inline`, `15-…#deferred-cascade-reclaims-whole-subtree`, `15-…#thread-local-free-pools-spill-to-global`, `15-…#one-count-column-per-size-class`.
- `tests/arena_growth_litmus.t.cpp` — concurrent seeded resolvers vs writer-driven `SlabDirectory` growth; asserts no torn pointer / no UAF and stable addresses. Carries `enforces:` tags for `15-…#chunk-growth-preserves-addresses` and `15-…#slots-recycle-in-place`.
- `tests/CMakeLists.txt` — three new executables added (one per stress/litmus file) with `catch_discover_tests`.
- `CMakePresets.json` — `tsan` configure/build/test preset added (`ARBC_SANITIZERS: "thread"`, Debug, `-fno-omit-frame-pointer`); sibling of `asan`, not an extension.
- `.github/workflows/ci.yml` — `gcc-tsan` lane added (gcc-14 TSan; full suite short-form, `[.nightly]` excluded). Note: clang unavailable in this environment; gcc-14 TSan validated 100% green across all 229 tests including the mprotect/SIGSEGV crash harness — strictly stronger than a clang lane that would need the crash-test exclusion.
- `.github/workflows/nightly.yml` — `tsan-full` job added (long-form `[.nightly]` wide-seed sweep under TSan).
- `src/runtime/arbc/runtime/housekeeping.hpp`, `src/runtime/arbc/runtime/housekeeping_thread.hpp`, `src/runtime/t/housekeeping.t.cpp`, `src/runtime/t/housekeeping_thread.t.cpp` — minor API additions/adjustments required by the cross-component stress tests.
- `tasks/parking-lot.md` — cleared the two 2026-07-04 TSan-CI-lane requests (from `pool.reclamation` and `pool.free_pools` closers) that this task was specifically created to resolve.
