# surfaces.surface_pool — Surface pooling

## TaskJuggler entry

`tasks/20-surfaces.tji:24-29` → `surfaces.surface_pool` ("Surface pooling"),
the fourth leaf under `task surfaces`. `depends !capabilities` — it reads the
capability/allocation seam the first leaf established. Note line:
"Compositor-owned allocation through backend pools; temp-surface reuse in the
render path (the skeleton allocates per request). Docs 02/09."

## Effort estimate

1d.

## Inherited dependencies

**Settled:**

- `surfaces.capabilities` (commit `62ff4df`) — `Backend::make_surface` is now
  `expected<std::unique_ptr<Surface>, SurfaceError>`
  (`src/surface/arbc/surface/backend.hpp:31-32`); the null-on-failure seam is
  gone. This task builds its acquire path over that errors-as-values return
  and propagates the same `SurfaceError`
  (`src/surface/arbc/surface/surface_error.hpp:13`). `BackendCaps`
  (`src/surface/arbc/surface/capabilities.hpp:53`) exists but is not needed
  here — pooling composes over `make_surface`, which already answers
  "can this backend store this format?" as a value.
- `color.format_set` / `pool.arena_core` (transitively, via capabilities) —
  `arbc::expected` lives in `base`
  (`src/base/arbc/base/expected.hpp`), with a move constructor so a move-only
  RAII handle is returnable by value.

**Pending:** none — the sole predecessor is landed.

## What this task is

Replace the render path's allocate-and-free-per-request pattern with a
**core-owned surface pool** that recycles temp surfaces.

Today `render_frame` (`src/compositor/compositor.cpp:53-54`) calls
`backend.make_surface(...)` fresh for **every layer, every frame**, and the
resulting `std::unique_ptr<Surface>` is scoped to the per-layer lambda body
(`compositor.cpp:16-71`) — so each temp surface is heap-allocated and then
freed at the end of its layer iteration. That is the "skeleton allocates per
request" the note names.

Two concrete deliverables:

1. **A `SurfacePool` value class in `arbc::surface`** that composes over a
   `Backend&`. `acquire(width, height, SurfaceFormat)` returns a move-only
   RAII handle (`PooledSurface`) wrapping a live `Surface`; on a free-list
   hit for that exact `(width, height, SurfaceFormat)` key it recycles a
   previously-released surface, on a miss it calls `Backend::make_surface`
   and propagates its `SurfaceError`. Destroying the handle **returns** the
   surface to the pool's free list rather than freeing it. Return type is
   `expected<PooledSurface, SurfaceError>`, mirroring `make_surface`.
2. **Rewire the render path onto the pool.** `render_frame` takes a
   `SurfacePool&`; the per-layer body acquires its temp from the pool instead
   of calling `make_surface` directly. `render_offline`
   (`src/runtime/offline.cpp`) owns a local `SurfacePool` for its frame and
   threads it in — its public signature is unchanged.

**Not this task:** a byte-budgeted / LRU eviction policy for the pool (the
render-thread working set is small and single-frame today; see Decisions and
Open questions); content-provided surface adoption and the "returned to the
pool untouched" path for `RenderResult.provided`
(`surfaces.provided_surfaces`, which owns doc 09:78-81); external import
(`surfaces.import`); any GPU-specific transient allocator (post-v1).

## Why it needs to be done

Doc 09 makes pooling a first-class part of the surface contract:
"allocation/pooling — **by the core only**; content receives targets"
(`docs/design/09-surfaces-and-backends.md:14`) and "A backend implements:
surface allocation and pooling" (`09:24-25`). The walking skeleton satisfies
neither the *reuse* promise nor doc 02's still-scene ethos — every frame
reallocates every temp. Landing the pool turns the render path from
"allocate per request" into "acquire/recycle," which is the precondition for
the interactive/video renderer to replay a mostly-static scene without
per-frame allocator churn.

It is also the seam `surfaces.provided_surfaces` reads when it implements the
doc-09 "the request's target is returned to the pool untouched" behavior
(`09:80`) — that task needs a pool to return the target *to*. Landing the
pool now gives it a stable acquire/release contract.

## Inputs / context

- `docs/design/09-surfaces-and-backends.md`:
  - `:14` — "allocation/pooling — **by the core only**; content receives
    targets" (the core-owned rule this task honors).
  - `:24-25` — "A backend implements: surface allocation and pooling" (the
    backend-responsibility wording the design-doc delta reconciles).
  - `:55-60` — surface creation is errors-as-values; `acquire` inherits this
    on the miss path.
  - `:80` — "the request's target is returned to the pool untouched" (the
    downstream `provided_surfaces` consumer of the acquire/release seam).
  - `:108-116` — Threading note: "surface allocation and composite run on the
    render thread (or its queue)"; the pool inherits this confinement.
- `docs/design/02-architecture.md`:
  - `:57-68` — the frame's plan/composite steps: the compositor allocates the
    per-request target that content fills, then composites it (`:66-68`).
  - `:92-95` — the **tile cache** is a *separate*, byte-budgeted LRU structure
    keyed by `(content id, revision, scale rung, tile coords)`; the temp/
    transient pool is not the tile cache and does not share its budget.
  - `:101-120` — threading model: compositing on the render thread; "v1 may
    degenerate to 'everything on one thread'" (`:118-120`).
- `docs/design/17-internal-components.md:51` — `arbc::surface` (L2, DEPENDS
  `base media`): owns "`Surface` handles, the backend contract." `SurfacePool`
  belongs here.
- `src/compositor/compositor.cpp:53-54` — the per-request `make_surface` call
  to replace; `:58-59` — `Surface& temp = **temp_result;` then
  `backend.clear(temp, …)` (the caller-clears invariant that keeps recycled
  surfaces correct); `:70` — the composite that ends the temp's useful life.
- `src/compositor/arbc/compositor/compositor.hpp:28` — `render_frame` decl
  (gains a `SurfacePool&` parameter).
- `src/runtime/offline.cpp` — `render_offline`; `:19` — the `render_frame`
  call site to thread the pool through.
- `src/surface/arbc/surface/backend.hpp:31-32` — `make_surface`, the seam the
  pool composes over; `:26` — `capabilities()` (not consumed here).
- `src/surface/arbc/surface/surface_error.hpp:13` — `SurfaceError`, propagated
  on the miss path.
- `src/backend_cpu/cpu_backend.cpp:30-46` — the CPU `make_surface` impl the
  pool calls on a miss (rejects non-`k_working_rgba32f` with
  `unexpected(SurfaceError::UnsupportedFormat)`).
- `src/surface/CMakeLists.txt` — component wiring (`DEPENDS base media`); the
  new header + test target land here.
- `tests/walking_skeleton.t.cpp:44` (`render_offline` driver), `:67-84` (the
  `16-sdlc-and-quality#byte-exact-goldens` byte-exact test) — must stay
  byte-identical after the rewire.
- `tests/claims/registry.tsv` — TAB-separated `<claim-id>\t<description>`;
  existing surface claims `09-surfaces-and-backends#make-surface-faults-as-value`,
  `#capabilities-are-honest`, `#byte-exact-goldens`. This task adds one.
- `tasks/refinements/surfaces/capabilities.md` — sibling decisions:
  "one value struct, not loose flags" (Decisions), errors-as-values on
  `make_surface`, capability honesty. The `SurfacePool` follows the same
  value-type / errors-as-values idiom.

## Constraints / requirements

- **Levelization (doc 17).** `SurfacePool` + `PooledSurface` live in
  `arbc::surface` (L2), composing over `Backend`/`Surface`/`SurfaceError`
  (all in `surface`) and `SurfaceFormat` (`media`, already a dep). **No new
  component and no new dependency edge** — the compositor (L4) and runtime
  already `#include <arbc/surface/…>` and compile, so adding
  `arbc/surface/surface_pool.hpp` there crosses no new edge.
- **Core-owned, backend-composed (doc 09:14/:24).** The recycling pool is a
  core-owned `SurfacePool` over `Backend::make_surface`, **not** a per-backend
  virtual. This reuses the one existing allocation seam and avoids
  duplicating a free list into every backend. (Reconciled in the doc-09 delta;
  see Decisions.)
- **Recycled surfaces carry undefined contents.** `acquire` guarantees a live
  surface of exactly the requested `(width, height, SurfaceFormat)`, **not**
  cleared pixels — a recycled surface holds its prior frame's data. Callers
  must clear or fully overwrite before reading; the compositor already clears
  each temp (`compositor.cpp:59`), so this is the invariant that keeps output
  byte-identical. Document it on `acquire`.
- **Errors as values (doc 10).** `acquire` returns
  `expected<PooledSurface, SurfaceError>`; on a free-list miss it forwards
  `make_surface`'s error unchanged. Never null, never abort.
- **RAII release, move-only handle.** `PooledSurface` is move-only; its
  destructor returns the surface to the pool. No manual release call in the
  render path — the handle's lambda-body scope (`compositor.cpp:16-71`) drives
  release, matching the current `unique_ptr` scoping exactly.
- **Render-thread-confined (doc 09:110-113).** The pool is not thread-safe and
  does not need to be: acquire/release run only on the render thread where
  allocation runs. Worker threads (in the future async model) touch surfaces
  *handed to a render request*, not the pool. **No lock is added.** This is a
  design decision, not an omission (see Decisions / Acceptance criteria).
- **Goldens byte-identical.** The rewire changes *where surfaces come from*,
  not the pixels. The walking-skeleton exact-pixel test
  (`walking_skeleton.t.cpp:32`) and byte-exact test (`:68`) stay unchanged and
  green.
- **CI diff coverage ≥90%** on changed lines (doc 16).

## Acceptance criteria

- **Unit tests — `src/surface/t/surface_pool.t.cpp` (new, L2).** Uses an
  in-test `StubBackend` (a minimal `Backend` that counts `make_surface` calls
  and returns a trivial surface for a chosen format, `unexpected(...)` for
  others) — **not** `CpuBackend` (that is L3; an L2 test may not depend on it).
  Assertions:
  - acquire miss → exactly one `make_surface`; the handle yields a live
    surface of the requested size/format.
  - release (handle destruction) then re-acquire of the **same key** →
    **zero** additional `make_surface` (recycle).
  - two acquisitions of **distinct keys** → two `make_surface` calls; distinct
    live surfaces.
  - two concurrent live handles of the same key → two `make_surface` (the free
    list only recycles *released* surfaces).
  - acquire of a format the stub rejects → `unexpected(SurfaceError)`
    propagated verbatim, never null/abort.
  - move semantics: moving a `PooledSurface` transfers the release obligation
    (no double-release, no leak) — assert via the stub's alloc/live counters.
- **Integration behavioral-counter test — top-level `tests/` target
  (alongside `walking_skeleton.t.cpp`, where the full model→compositor→backend
  stack is assemblable).** A counting decorator backend wrapping `CpuBackend`,
  a **persistent** `SurfacePool`, and the same scene rendered through
  `render_frame` twice:
  - frame 1 issues one `make_surface` per distinct temp size;
  - frame 2 (identical scene, same pool) issues **zero** additional
    `make_surface` — every temp is recycled across the frame boundary.
  A behavioral counter, never a wall-clock assertion (doc 16). Also assert a
  within-frame recycle: a scene with two same-sized temps issues one
  `make_surface`, not two.
- **Claim (register + `enforces:` tag):**
  `09-surfaces-and-backends#surface-pool-recycles` — "A released pooled
  surface is reused for a subsequent same-`(size, format)` acquisition rather
  than reallocated; the backend allocates once per distinct `(size, format)`,
  not once per request." Registered in `tests/claims/registry.tsv`, enforced
  from the unit test (and the integration counter test).
- **Byte-exact goldens preserved.** `walking_skeleton.t.cpp:32` and `:68`
  (`16-sdlc-and-quality#byte-exact-goldens`) pass unchanged after the rewire.
- **Call-site migration compiles and passes:** `render_frame`'s new
  `SurfacePool&` parameter is threaded through `src/runtime/offline.cpp` (a
  local pool for the single frame); no behavior change on the render path.
- **Gate green (including asan). No TSan obligation** — the pool is
  render-thread-confined by design (single-threaded structure; nothing to
  race). The confinement is documented and enforced structurally (no shared
  access), so a TSan/stress suite here would exercise no concurrency. If a
  future task moves allocation off the render thread, thread-safety becomes
  *that* task's obligation.

## Decisions

- **Pool is a core-owned `SurfacePool` composing over `make_surface`, not a
  per-backend virtual.** Doc 09 says both "pooling by the core only" (`:14`)
  and "a backend implements … pooling" (`:24`); the defensible reading is that
  the backend owns *allocation* (`make_surface`) and the core owns the
  *recycling policy* over it. A generic free-list pool built once over the
  single existing allocation seam beats a `virtual acquire/release` on
  `Backend` that would duplicate the same keyed free list into every backend.
  This reuses an existing seam and keeps the abstraction at one/two call sites
  (the capabilities refinement's stated bias). *Rejected:* pool as a `Backend`
  virtual (free-list duplication per backend, more contract surface for one
  policy); *rejected:* pooling folded into the tile cache (doc 02:92 — the
  cache is a byte-budgeted LRU keyed by content/revision/scale/tile; the temp
  pool is a different structure with different keys and lifetimes, and
  conflating them couples two policies). A backend may still specialize
  allocation *behind* `make_surface` later (a GPU transient allocator) without
  disturbing this seam.
- **`render_frame` takes a `SurfacePool&` (caller-owned) rather than owning
  one internally.** A caller-owned pool enables **cross-frame** reuse — the
  interactive/video renderer holds a long-lived pool and replays a static
  scene with zero reallocation, which is the doc-02 still-scene payoff. An
  internally-owned per-call pool would give within-frame reuse only and make
  cross-frame reuse impossible. The only caller today (`render_offline`) is
  single-frame, so it owns a local pool and gets within-frame reuse now; the
  seam is ready for a looping renderer (exercised by the integration test's
  double-render). *Note (no silent cap, doc 16):* no production caller loops
  frames yet, so the cross-frame win is latent until the interactive/video
  renderer (runtime area) lands — the seam and its test exist now.
- **`acquire` returns `expected<PooledSurface, SurfaceError>`; recycled
  surfaces are uncleared.** Mirrors `make_surface`'s errors-as-values return
  (the miss path just forwards it) and the sibling capabilities idiom.
  Not clearing on acquire avoids a redundant clear (the compositor clears
  every temp anyway, `compositor.cpp:59`) and keeps the pool a pure
  allocation-recycling layer. *Rejected:* clearing inside `acquire`
  (double-clears the common path; hides the caller's responsibility);
  *rejected:* `std::optional<PooledSurface>` (drops the `SurfaceError` reason a
  capability-negotiating caller needs).
- **Free-list key is exact `(width, height, SurfaceFormat)`; no resize/
  repurpose.** A recycled surface must match the request's full tag triple and
  dimensions, so a size/format mismatch is simply a different key (miss →
  fresh `make_surface`). Resizing or reinterpreting a pooled surface would
  reintroduce the tag-mismatch hazard the surface contract forbids.
- **Render-thread confinement over locking.** Per doc 09:110-113 allocation is
  render-thread-only; a lock-free single-threaded pool is correct and cheaper
  than a synchronized one guarding against a caller the model forbids.
  Documented as an invariant. *Rejected:* a mutex-guarded pool "to be safe"
  (guards against nothing under the doc-02 threading model and pays a cost on
  the hot render path).
- **Design-doc delta to doc 09 (rides the closer's commit).** Doc 09 names
  pooling in two places (`:14`, `:24`) and the recycle semantic once (`:80`)
  but pins no seam: is the pool a backend virtual or a core layer? who clears
  recycled surfaces? which thread? A short "Surface pooling" note under
  **Backend contract** fixes it: pooling is a core-owned `SurfacePool` over
  `make_surface`, acquire/release keyed by `(size, format)`, recycled surfaces
  carry undefined contents (callers clear), render-thread-confined; a backend
  may specialize allocation behind `make_surface`. This concretizes an
  ambiguous seam rather than shaping the project, so it takes **no doc 00
  decision-record bullet**.
- **No new WBS task.** A byte-budgeted eviction policy is not needed at v1
  (see Open questions) and its trigger is a judgment call, not
  agent-implementable work — surfaced for the parking lot, not deferred as a
  leaf. `surfaces.provided_surfaces` (the "returned to the pool untouched"
  consumer) and the future interactive/video renderer already exist / are
  scoped as their own leaves; nothing here needs a successor.

## Open questions

- **Pool eviction / byte budget.** Docs 02/09 specify no sizing, high-water,
  or eviction policy for the temp pool (only the *tile cache* is budgeted,
  doc 02:92). At v1 the render path is single-frame and synchronous, so the
  resident free list is bounded by one frame's small concurrent working set —
  a byte budget would bound nothing observable. A long-lived cross-frame pool
  under sustained camera motion (many distinct temp sizes) *could* accumulate
  stale sizes, but the only cross-frame caller — the interactive/video
  renderer — does not exist yet, and *whether* the accumulation matters is a
  profiling-dependent judgment, not a fixed spec. Deciding the eviction policy
  now would be speculative. **Resolution:** ship an unbounded-but-render-
  thread-confined free list; surface "add a byte-budget/one-frame-trim to
  `SurfacePool` if the interactive renderer's profiling shows accumulation" as
  a **parking-lot** item for the closer (a judgment call tied to a renderer
  that isn't built), not a WBS leaf.

## Status

**Done** — 2026-07-05.

- `src/surface/arbc/surface/surface_pool.hpp` + `src/surface/surface_pool.cpp` — `SurfacePool` value class and move-only `PooledSurface` RAII handle; acquire/release keyed by exact `(width, height, SurfaceFormat)` triple; miss path calls `Backend::make_surface` and propagates `SurfaceError`.
- `src/surface/t/surface_pool.t.cpp` — 7 unit-test cases using an in-test `StubBackend` (L2-clean): miss→one alloc, release+reacquire→zero alloc (recycle), distinct keys→two allocs, two live same-key→two allocs, format-reject→`unexpected` propagated, move semantics→no double-release.
- `tests/surface_pool_integration.t.cpp` — cross/within-frame recycle counter test via a counting decorator wrapping `CpuBackend`; frame 2 identical scene issues zero additional `make_surface`; same-sized within-frame temps recycle.
- `src/compositor/arbc/compositor/compositor.hpp` + `src/compositor/compositor.cpp` — `render_frame` gains a `SurfacePool&` parameter; per-layer temps acquired from the pool instead of direct `make_surface`.
- `src/runtime/offline.cpp` — `render_offline` owns a local `SurfacePool` for its frame; public signature unchanged.
- `src/surface/CMakeLists.txt` + `tests/CMakeLists.txt` — wired new source, header, and test targets.
- `tests/claims/registry.tsv` — registered claim `09-surfaces-and-backends#surface-pool-recycles` enforced from unit and integration tests.
- Byte-exact golden (`walking_skeleton.t.cpp:32`, `:68`) output unchanged after rewire.
