# surfaces.provided_surfaces — Content-provided surfaces

## TaskJuggler entry

`tasks/20-surfaces.tji:12-17` → `surfaces.provided_surfaces`
("Content-provided surfaces"), the second leaf under `task surfaces`.
`depends !capabilities`. Note line: "RenderResult.provided: refcounted with
release callback, transient marking (consume-within-frame, copy-to-cache),
compositor path honoring it. Doc 09."

## Effort estimate

2d.

## Inherited dependencies

**Settled:**

- `surfaces.capabilities` (the formally-declared `!capabilities` predecessor,
  landed) — `Backend::capabilities()`
  (`src/surface/arbc/surface/backend.hpp:26`) and `BackendCaps`
  (`src/surface/arbc/surface/capabilities.hpp:53-59`) with
  `import_handles` (`ImportHandleTypes`, `capabilities.hpp:20-45`) and
  `sync_primitives` establish the vocabulary that gates *which* handle types a
  content-provided surface may ride on. This task consumes the `ImportHandle`
  enum (`capabilities.hpp:9-14`) as the tag vocabulary but does **not** build
  the import path itself (that is `surfaces.import`). `make_surface` is already
  errors-as-values (`backend.hpp:31`).
- `surfaces.surface_pool` (landed; `tasks/refinements/surfaces/surface_pool.md`)
  — `SurfacePool::acquire` / `PooledSurface`
  (`src/surface/arbc/surface/surface_pool.hpp:22-46,58-85`) is the seam the
  doc-09 "the request's target is returned to the pool untouched" (`09:80`)
  behavior reads: when content provides its own surface, the pooled temp the
  simple compositor path acquired (`src/compositor/compositor.cpp:48-49`) is
  released back to the pool untouched by `PooledSurface`'s normal RAII drop.
  surface_pool's Decisions named this task as its downstream consumer.
- `color.format_set` (transitively) — `SurfaceFormat`
  (`src/media/arbc/media/surface_format.hpp:26`) is the tag triple a provided
  surface must carry; `arbc::expected` lives in `base`
  (`src/base/arbc/base/expected.hpp`).

**Pending:** none — both the formal predecessor (`capabilities`) and the
de-facto seam (`surface_pool`) are landed. *(The `.tji` formally declares only
`depends !capabilities`; `surface_pool` is complete so there is no scheduling
hazard. Flagged for the closer in the return summary as a candidate
`depends !surface_pool` edge.)*

## What this task is

Let content answer a render request by returning **its own** `Surface`
(a 3D engine's framebuffer, a video decoder's output) instead of filling the
compositor-allocated target, and teach the compositor path to honor it —
compositing directly from it where it can, copying it into the tile cache
where it must, and releasing it the instant it is done.

Three concrete deliverables:

1. **A `SurfaceRef` refcounted handle in `arbc::surface`.** A small,
   copyable handle to a `Surface` (`src/surface/arbc/surface/surface.hpp:13-60`)
   carrying (a) a thread-safe refcount, (b) a **release callback to the
   content** fired when the last reference drops, and (c) a `bool transient`
   flag. Modelled as a `std::shared_ptr<Surface>` whose deleter is the
   content-supplied release callback, plus the `transient` bit — the
   shared_ptr control block already gives the atomic, thread-safe refcount the
   doc-09 threading note requires (release callbacks "may arrive from content
   threads", `09` §Threading note). New header
   `src/surface/arbc/surface/surface_ref.hpp`.
2. **`RenderResult.provided`.** Add `std::optional<SurfaceRef> provided` to
   `RenderResult` (`src/contract/arbc/contract/content.hpp:86-99`), matching
   the doc-09 struct sketch (`09:93-100`) and the authoritative field already
   named in doc 03 (`03:59`) and doc 11 (`11:112`). Absent (`nullopt`) is the
   default and keeps the common case a trivial copy — the atomic is paid only
   when a surface is actually adopted.
3. **The compositor path that honors it.** A single compositor-level helper
   that, at each site where a settled `RenderResult` is consumed, branches on
   `result.provided`:
   - **Inline composite (no cache):** composite directly *from* the provided
     surface — zero copy, the exact win doc 09 targets (`09:122-124`).
   - **Cache path:** the provided surface is **copied** into a cache-owned
     `unique_ptr<Surface>` (via `Backend::composite`
     (`src/surface/arbc/surface/backend.hpp:39`) into a freshly cleared
     `make_surface` destination), which is inserted as an ordinary
     `TileValue` — the cache never learns the surface was "provided".
   - **Release** the `SurfaceRef` the moment the compositor has composited or
     copied it (never before), and return the untouched pooled/allocated
     target to the pool (`09:80`).

**Not this task:** the backend **import** machinery — GL/Vulkan/DMA-BUF/CPU
wrap-or-copy and the sync-token API (`surfaces.import`, owns `09:114-120`);
zero-copy *adoption* of a non-transient provided surface as a cache value
(holding the `SurfaceRef` in `TileValue` instead of copying — a cache-layer
change that pays off only on GPU backends; see Open questions → parking lot);
cross-tag convert-at-composite for a provided surface carrying a
non-working-space tag (gated on a backend that stores a second format; see
Open questions); pooled allocation (`surfaces.surface_pool`, landed).

## Why it needs to be done

Doc 03's contract has the compositor allocate the target and content fill it;
that "forces a copy on content that already owns a rendered target" (doc 09
§Content-provided surfaces, opening). Doc 09 resolves this by letting
`RenderResult` carry the content's own surface, and doc 11 names the video
decoder as "the motivating case for content-provided transient surfaces"
(`11:205`). Doc 00's decision record lists it as decided (`00:112-114`). The
capability vocabulary (`surfaces.capabilities`) and the pool seam
(`surfaces.surface_pool`) are landed specifically to be read here — the
`RenderResult.provided` field, its `SurfaceRef` handle, and the
compositor branch are the one piece still missing in code. Without it every
texture-owning content (3D engines, video, camera feeds — the whole doc-11
video stream and the nested/effects operator graph, `13:188`) pays a forced
readback-copy on every frame.

## Inputs / context

- `docs/design/09-surfaces-and-backends.md` — **§Content-provided surfaces
  (texture adoption)** (heading line 80): the normative spec.
  - `:87-100` — `RenderResult.provided` sketch; the struct comment
    (`:97-98`): "provided != target implies: compositor composites/caches from
    `provided`; the request's target is returned to the pool untouched."
  - `:102-105` — tag-compatibility: "must carry compatible tags or be
    convertible; the backend converts at composite time if needed (doc 07)."
  - `:106-112` — **Lifetime:** "`provided` is refcounted with a release
    callback to the content, and is pinned only until the compositor has
    composited *or* copied it into cache. Content that reuses its framebuffer
    every frame … marks the surface `transient`: the compositor must consume
    it within the current frame and may not cache it directly (it copies if it
    wants to cache — correct for `Volatile`-stability content …)."
  - `:114-120` — backend import + sync primitive (owned by `surfaces.import`,
    not here).
  - `:122-124` — "keeps the pull contract intact — the request still names
    region and scale, and content that provides its own surface must still
    honor them."
  - §Threading note (`:131-134`) — "External import/release callbacks may
    arrive from content threads; backends must make import thread-safe …."
  - §Surface pooling (`:62-78`, `:80` "returned to the pool untouched") — the
    target-release half of the branch.
- `docs/design/03-layer-plugin-interface.md` — `:53-60` authoritative
  `RenderResult` (`provided` at `:59`); `:126-130` "one render entry point"
  (inline-or-async, the path `provided` rides); `:146-151` the normative
  pointer deferring adoption/lifetime/sync detail to doc 09.
- `docs/design/11-time-and-video.md:112,205` — `provided` field; video as the
  canonical `transient` provider.
- `docs/design/13-effects-as-operators.md:188` — "provided surfaces flow
  through pulls" (they travel the PullService graph, not a side channel).
- `docs/design/02-architecture.md` — §Tile cache (`:87-109`): value is a
  backend surface + `{achieved_scale, exact}` meta (`:90-91`); **Residency
  pin vs. payload refcount** (`:100-109`) — the cache's residency pin is
  distinct from the backend-pool payload refcount; a copied provided surface
  becomes an ordinary pinned cache value. Frame composite step (`:66-68`).
- `docs/design/17-internal-components.md` — levelization (rule at `:41`;
  table `:46-59`): `surface` L2 owns "external import + sync tokens" →
  `SurfaceRef` lives here; `contract` L3 (depends `surface`) owns
  `RenderResult` → the `provided` field lands here; `cache` L3 (depends
  `surface`, **not** `contract`) → the cache stays oblivious; `compositor` L4
  (sees both `contract` and `cache`) → the consume-vs-copy branch lands here.
- `src/contract/arbc/contract/content.hpp:86-99` — `RenderResult` (gains
  `provided`); `:96-97` the "cheap by-value descriptor — no allocation or
  atomic" property `nullopt`-default preserves; `RenderCompletion`
  (`:119-151`) carries the result across the renderer/caller thread boundary
  with release/acquire ordering (`:148-150`) — a `SurfaceRef` inside it must
  survive that move; `Live`-stability "cacheable only within a frame/snapshot"
  (`:29`).
- `src/surface/arbc/surface/surface.hpp:13-60` — `Surface` (abstract,
  `unique_ptr`-owned today, **no refcount** — the machinery this task adds via
  `SurfaceRef`).
- `src/surface/arbc/surface/backend.hpp:26,31,39` — `capabilities()`,
  `make_surface`, `composite(dst, src, affine, opacity)` (the blit-into-cache
  primitive; identity affine + opacity 1 over a cleared destination = a copy —
  **no new backend method needed**).
- `src/surface/arbc/surface/capabilities.hpp:9-14,53-59` — `ImportHandle`,
  `BackendCaps`.
- `src/surface/arbc/surface/surface_pool.hpp:22-46,58-85` — `PooledSurface`
  RAII drop returns the untouched target to the pool.
- The four `RenderResult`-consumption sites the branch threads through:
  - `src/compositor/compositor.cpp:48-49,70,83-90` — simple non-tiled path:
    pooled temp, render, `backend.composite(target, temp, …)` (`:90`). The
    inline zero-copy composite-from-provided lands here.
  - `src/compositor/tile_planning.cpp:327-328,393-395,401,403-405` —
    interactive tiled driver: `make_surface`, `done->take()` →
    `const RenderResult result = *…`, `timed_insert_key_consistent(…)`
    guard, `cache.insert(tile.key, TileValue{…}, …)`.
  - `src/compositor/pull_service.cpp:141-147,181-183,188,190-191` — the
    PullService impl; comment `:141-145` states the invariant this relaxes
    ("the render targets a freshly-allocated cache-destined surface — not the
    caller's `request.target`").
  - `src/compositor/refinement.cpp:87,91-92` + `PendingTile`
    (`src/compositor/arbc/compositor/refinement.hpp:72-85`, `surface` at `:83`,
    `done` at `:84`) — async arrival drain; a provided surface arriving async
    is copied into the cache at drain time through the same helper.
- `src/cache/arbc/cache/key_shapes.hpp:99-102,104-113,120-124` — `TileMeta`,
  `TileValue{unique_ptr<Surface>, TileMeta}` (owning, move-only, "**not** a
  `PooledSurface`"), `tile_byte_cost`. Unchanged by this task.
- `src/cache/arbc/cache/keyed_store.hpp:26,60,78,163` — `PriorityClass`,
  `CacheHold`/`CacheHoldOwner`, `insert`. Unchanged.
- `tests/claims/registry.tsv` — TAB-separated `<claim-id>\t<description>`;
  existing surface/render claims
  `09-surfaces-and-backends#make-surface-faults-as-value`,
  `#capabilities-are-honest`, `#surface-pool-recycles`,
  `07-color-and-pixel-formats#surfaces-carry-tags`,
  `03-layer-plugin-interface#render-scale-honest`,
  `02-architecture#cache-pin-survives-eviction`. This task adds three under the
  `09-surfaces-and-backends#` stem.
- `tests/walking_skeleton.t.cpp:32,68` — the byte-exact goldens
  (`16-sdlc-and-quality#byte-exact-goldens`) that must stay green.
- `tasks/refinements/surfaces/capabilities.md`,
  `tasks/refinements/surfaces/surface_pool.md` — sibling idioms:
  one value/handle type, errors-as-values, "Design-doc delta to doc 09 (rides
  the closer's commit)", behavioral-counter (not wall-clock) tests, no silent
  caps.

## Constraints / requirements

- **Levelization (doc 17), no new component or edge.** `SurfaceRef` in
  `surface` (L2); `RenderResult.provided` in `contract` (L3, already
  `#include`s `<arbc/surface/…>` — `content.hpp` includes surface headers);
  the consume-vs-copy branch and the backend copy in `compositor` (L4, sees
  `contract`+`cache`+`surface`). **The cache does not change and never learns a
  surface was provided** — the compositor hands it a plain
  `TileValue{unique_ptr<Surface>, TileMeta}`, respecting the forbidden L3→L3
  `cache`→`contract` edge that `TileMeta` already exists to avoid.
- **Refcount is thread-safe; release fires from the content thread.** Per the
  doc-09 threading note, the release callback may run on a content thread.
  `SurfaceRef`'s refcount (shared_ptr control block) is atomic; the
  compositor drops its ref with no lock, and the callback runs when whichever
  holder drops last. No compositor-side synchronization is added — the
  refcount is the synchronization.
- **The pull contract still binds a provided surface (doc 09:122-124).** A
  provided surface must cover the request's region at `result.achieved_scale`,
  and `achieved_scale ≤ request.scale` still holds
  (`03-layer-plugin-interface#render-scale-honest`). The
  `timed_insert_key_consistent` soundness guards
  (`tile_planning.cpp:401`, `pull_service.cpp:188`, `refinement.cpp:87`) read
  the same `RenderResult` fields and hold **unchanged** — a provided surface
  changes *where the pixels come from*, not the key.
- **`transient` ⇒ consume-within-frame, copy-to-cache, never retained.** A
  provided surface marked `transient` is composited/copied within the current
  frame and its `SurfaceRef` is dropped before the frame's per-tile processing
  ends; it is **never** stored in a structure that outlives the frame. Because
  the cache path always **copies** (v1), a transient surface is never adopted
  as a live cache value — the cached tile is an independent copy, correct for
  the `Volatile`/`Live`-stability content that declares `transient`
  (`content.hpp:29`, `09:109-112`).
- **v1 requires a backend-compatible (working-space) tag.** The CPU backend
  stores only `k_working_rgba32f` and its `composite` asserts tag agreement,
  so a provided surface must carry the composition working-space tag. Cross-tag
  convert-at-composite (doc 09:102-105's sRGB8→linear-f16 case) is out of scope
  until a backend advertises a second storable format (see Open questions).
  Existing tag discipline (`07-color-and-pixel-formats#surfaces-carry-tags`)
  is unchanged.
- **Goldens byte-identical.** Rendering content that returns `provided` must
  produce byte-identical output to the same content filling the target
  (`walking_skeleton.t.cpp:32,68` stay green).
- **Errors as values / no leak / no double-release.** No new failure mode is
  introduced on the settled-result path; the `SurfaceRef` is released exactly
  once (last-ref-drops), never leaked across the frame, never double-dropped
  under move.
- **CI diff coverage ≥90%** on changed lines (doc 16).

## Acceptance criteria

- **`SurfaceRef` unit tests — `src/surface/t/surface_ref.t.cpp` (new, L2).**
  - a `SurfaceRef` fires its release callback **exactly once**, when the last
    reference drops (copy → two refs → release after both drop); a
    behavioral counter on the callback, never a timer.
  - move transfers the reference (no double-release, no leak) — assert via the
    callback counter.
  - `transient` flag round-trips and is queryable on the handle.
  - the wrapped `Surface` is reachable (`width/height/format`) while any ref is
    live.
- **Compositor honoring — inline zero-copy composite.** A test content that
  returns `RenderResult.provided` on the simple non-tiled path
  (`compositor.cpp`) is composited **directly from** the provided surface:
  - byte-exact golden — output equals the same content filling the target
    (`16-sdlc-and-quality#byte-exact-goldens`, byte-exact, not tolerance);
  - **behavioral counter** — the inline composite issues **zero** `make_surface`
    calls for a copy (a counting decorator backend wrapping `CpuBackend`); and
    the pooled target is returned to the pool untouched (no clear/render on it).
- **Compositor honoring — transient copy-to-cache.** A `transient` provided
  surface on the tiled/cache path (`pull_service.cpp` / `tile_planning.cpp`) is
  **copied** into a cache-owned surface:
  - **behavioral counter** — exactly one `make_surface` for the cache copy per
    cached tile (counting decorator backend);
  - **independence** — mutating the content's source surface *after* the frame
    does not change the cached tile (proves a copy, not adoption);
  - **release-within-frame** — the provided `SurfaceRef`'s release callback has
    fired by the time the frame's render pass completes (the compositor holds
    no reference into the next frame); a callback counter, never a timer.
- **Region/scale still honored.** A provided surface reports
  `achieved_scale ≤ request.scale` and the existing scale-honesty guard passes
  (`03-layer-plugin-interface#render-scale-honest`).
- **Claims (register + `enforces:` tags)** — three rows added to
  `tests/claims/registry.tsv` under the `09-surfaces-and-backends#` stem:
  - `#content-provided-surface-honored` — "When a `RenderResult` carries a
    `provided` surface, the compositor composites/caches from it instead of the
    request's target, honoring the request's region and scale; the untouched
    target is returned to the pool." Enforced from the inline-composite test.
  - `#provided-surface-released-after-consume` — "A provided surface is
    refcounted; the compositor releases its reference (invoking the content
    release callback when it drops the last one) once it has composited or
    copied the surface, and never before." Enforced from the `SurfaceRef` unit
    test and the transient copy-to-cache test.
  - `#transient-provided-copied-not-cached` — "A `transient` provided surface
    is copied into a cache-owned surface when cached and never retained across
    the frame boundary; the cached tile is independent of the content surface."
    Enforced from the transient copy-to-cache test.
- **Gate green (incl. asan).** No dedicated TSan suite: `SurfaceRef`'s refcount
  is a `std::shared_ptr` control block (already race-safe) and the compositor
  touches the ref only on the render thread. The cross-thread release path
  (callback from a content thread) is exercised structurally through
  `RenderCompletion`'s existing release/acquire hand-off; if `surfaces.import`
  later builds a real off-thread producer, its TSan obligation lands there.
- **Byte-exact goldens preserved** (`walking_skeleton.t.cpp:32,68`).

## Decisions

- **`SurfaceRef` is a `std::shared_ptr<Surface>` + a `transient` bool, not a
  hand-rolled intrusive refcount.** Doc 09 demands "refcounted with a release
  callback to the content" and (Threading note) a refcount safe against release
  from a content thread — `shared_ptr`'s control block is exactly a thread-safe
  atomic refcount, and a custom deleter *is* the release-callback-to-content.
  Reuses std machinery, no new atomics to get wrong. *Rejected:* an intrusive
  refcount on `Surface` (reinvents `shared_ptr`'s thread-safe count, and forces
  every `Surface` to carry a count even when never adopted); *rejected:*
  putting the refcount on `RenderResult` (the surface, not the result, is what
  outlives the result and is shared with the cache-copy step).
- **`provided` copies into the cache; it does not adopt.** Doc 09 says the ref
  is "pinned only until the compositor has composited *or* copied it into
  cache" and "it copies if it wants to cache" — the cache path **always copies**
  a provided surface into a cache-owned `unique_ptr<Surface>` (both `transient`
  and non-`transient` in v1). This keeps `TileValue` and the entire `cache`
  component (L3) unchanged and oblivious, respects the L3→L3 forbidden edge,
  and is the literal realization of the doc's "or copied into cache". The
  zero-copy win doc 09 targets ("removing the forced copy exactly where it
  hurts", `:122-124`) is delivered on the **inline composite** path, which is
  where the GPU-readback cost actually bit. *Rejected:* zero-copy adoption of a
  non-transient provided surface as a cache value (holding the `SurfaceRef`
  inside `TileValue`) — it forces a cache-layer change (a refcounted-or-owned
  surface variant, byte accounting, pin interplay) for a payoff that only
  materializes on a GPU backend that does not exist yet; surfaced to the
  parking lot, not built (see Open questions).
- **One shared compositor helper, four call sites.** The
  branch-on-`result.provided` (composite-inline vs copy-to-cache, then release)
  is factored into a single compositor-level function invoked at each
  `RenderResult`-consumption site (`compositor.cpp`, `tile_planning.cpp`,
  `pull_service.cpp`, `refinement.cpp`). One code path keeps the inline and
  async arrivals identical (doc 03:126-130's "one render entry point") and puts
  the copy-to-cache logic in one testable place. *Rejected:* inlining the
  branch at each site (four copies of the copy-and-release logic to drift).
- **`transient` lives on `SurfaceRef`, not on `RenderResult`.** Doc 09 says
  content "marks the surface `transient`", and it is the *surface* whose
  lifetime the flag bounds. Keeping it on the handle means the compositor reads
  it exactly where it decides retain-vs-copy, and `RenderResult` stays the
  cheap descriptor it is (`content.hpp:96-97`) with only the `optional<SurfaceRef>`
  added. *Rejected:* a parallel `bool provided_transient` on `RenderResult`
  (splits one fact across two types).
- **`nullopt` default preserves the trivial-copy common case.** The overwhelming
  majority of renders fill the target and set no `provided`; `std::optional`
  empty is a trivial copy with no atomic, so the doc-09 "cheap by-value
  descriptor" property (`content.hpp:96-97`) survives — the shared_ptr atomic
  is paid only when a surface is genuinely adopted.
- **v1 requires a working-space tag; cross-tag conversion is deferred (not a
  WBS task).** Doc 09:102-105 allows a provided surface to carry a convertible
  tag with the backend converting at composite time, but the only backend
  (CPU) stores one format and asserts tag agreement in `composite`. Requiring
  the working-space tag now is correct and testable; convert-at-composite is
  latent until a backend advertises a second storable format — a capability
  that arrives with GPU backends / `color.kernels`, whose owning task will add
  the conversion. Surfaced to the parking lot, not deferred as a leaf (its
  trigger is another component landing, not standalone work).
- **Design-doc delta to doc 09 (rides the closer's commit).** Doc 09 fully
  specifies the *contract* but leaves three points implicit that this task
  pins: (1) `provided` is a `SurfaceRef` = a `shared_ptr<Surface>`-backed
  refcounted handle whose deleter is the release callback, carrying the
  `transient` bool (the flag lives on the handle, not `RenderResult`); (2) the
  compositor's v1 realization — composite directly from `provided` on the
  inline path, **copy** into a cache-owned surface on the cache path for both
  transient and non-transient, release after composite-or-copy; (3) v1 requires
  a working-space tag, with cross-tag convert-at-composite gated on multi-format
  backend support. A short addendum under §Content-provided surfaces records
  these. This **concretizes an already-decided seam** rather than shaping the
  project, so it takes **no doc 00 decision-record bullet** — matching the
  `capabilities`/`surface_pool` sibling deltas.
- **No new WBS task.** Both deferrals (zero-copy cache adoption; cross-tag
  convert-at-composite) are gated on a GPU/multi-format backend that does not
  exist and are profiling/capability judgment calls, not standalone
  agent-implementable work — they go to the parking lot, not the WBS.
  `surfaces.import` already owns the backend import + sync-token machinery as
  its own leaf; nothing here needs a successor.

## Open questions

- **Zero-copy adoption of a non-transient provided surface into the cache.**
  Doc 09 permits caching a provided surface; v1 copies it. Holding the
  `SurfaceRef` directly as a cache value (no copy) would save a blit but
  requires `TileValue` to own a refcounted-or-plain surface variant and the
  cache's byte accounting/pin logic to follow — a `cache`-layer change whose
  payoff is real only when the copy is expensive (GPU textures), i.e. on a
  backend not yet built. **Resolution:** ship copy-to-cache; surface "adopt a
  non-transient provided surface as a zero-copy cache value once a GPU backend
  makes the copy a measured cost" as a **parking-lot** item for the closer (a
  profiling-and-capability judgment tied to a backend that isn't built), not a
  WBS leaf.
- **Cross-tag convert-at-composite for provided surfaces.** Doc 09:102-105
  wants the backend to convert a differently-tagged provided surface at
  composite time (sRGB8 framebuffer → linear-f16). The CPU backend stores one
  format and asserts tag agreement, so v1 requires the working-space tag. The
  conversion lands with the backend/kernel work that introduces a second
  storable format (`color.kernels` / GPU backends). **Resolution:** require a
  working-space tag now; surface the convert-at-composite extension as a
  **parking-lot** item tied to that later capability, not a WBS leaf.

## Status

**Done** — 2026-07-07.

- `src/surface/arbc/surface/surface_ref.hpp` (new) — `SurfaceRef`: `shared_ptr<Surface>`-backed refcounted handle with release-callback-as-deleter and `transient` bool.
- `src/surface/t/surface_ref.t.cpp` (new) — unit tests: release-once, move, transient round-trip, reachable-while-live.
- `src/compositor/arbc/compositor/provided_surface.hpp` (new) — `consume_render_result` helper branching on `result.provided` at all four consumption sites.
- `src/contract/arbc/contract/content.hpp` — `std::optional<SurfaceRef> provided` added to `RenderResult`.
- `src/compositor/compositor.cpp`, `tile_planning.cpp`, `pull_service.cpp`, `refinement.cpp`, `arbc/compositor/refinement.hpp` — four `RenderResult`-consumption sites wired to `consume_render_result`; `poll_refinements` gained optional `Backend*` trailing parameter.
- `src/runtime/interactive.cpp` — passes `&backend` to `poll_refinements`.
- `src/surface/CMakeLists.txt`, `src/compositor/CMakeLists.txt`, `tests/CMakeLists.txt` — build wiring for new headers and test binaries.
- `tests/provided_surfaces.t.cpp` (new) — integration tests: inline byte-exact golden, zero-copy counter, transient copy-to-cache counter=1/independence/release-within-frame.
- `tests/claims/registry.tsv` — 3 new rows: `#content-provided-surface-honored`, `#provided-surface-released-after-consume`, `#transient-provided-copied-not-cached`.
- `docs/design/09-surfaces-and-backends.md` — realization addendum under §Content-provided surfaces pinning v1 decisions.
