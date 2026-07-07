# kinds.imageseq_plugin — `arbc-plugin-imageseq`

## TaskJuggler entry

Defined at `tasks/55-kinds.tji:61-66`:

```
task imageseq_plugin "arbc-plugin-imageseq" {
  effort 2d
  allocate team
  depends timeline.temporal_cache
  note "Out-of-lib plugin artifact carrying the decode dependency (doc 17 codec line):
        Timed visual content, spans, achieved_time on source frames, temporal prefetch;
        the permanent end-to-end test of the runtime plugin path. Docs 11/17."
}
```

It is the last `kinds`-tree leaf feeding milestone **M5: Video**
(`tasks/99-milestones.tji:42-45`), alongside `timeline.transport`,
`timeline.temporal_cache`, `timeline.playback_hints`,
`runtime.offline_sequences`, and `surfaces.provided_surfaces`.

## Effort estimate

**2d** (`tasks/55-kinds.tji:62`). Justified: the `Content` contract this kind
implements is already fully landed (Stability, `time_extent`, `quantize_time`,
`playback_hint`, `RenderResult.achieved_time`/`.provided` — see Inputs), so the
`Content` body reuses the `SolidContent`/`RasterContent` decode idiom verbatim.
The genuinely new surface is small and mechanical: an out-of-lib `MODULE`
CMake target, a minimal `Registry` + `extern "C"` entry-point seam in
`arbc::contract` (a doc-03-specified seam, not new design), and the plugin's
own private stb-class decode dependency. The bulk of the day-count is the
end-to-end test matrix (dlopen → register → conformance → temporal goldens →
prefetch counters → TSan), which is the task's actual product.

## Inherited dependencies

**Settled predecessors this task builds on (all `complete 100`):**

- `timeline.temporal_cache` — *settled 2026-07-07*, `d162f56`/`5721570` lineage,
  refinement `tasks/refinements/timeline/temporal_cache.md`. Landed the
  plan-time-key == render-time-`achieved_time` soundness loop at the live
  `TileCache` boundary: the compositor keys a `Timed` tile at plan time with
  `content->quantize_time(time).value_or(time)` (`src/compositor/tile_planning.cpp:141-149`),
  renders on miss, and asserts `timed_insert_key_consistent(key, result, stability)`
  at every insert site (`src/compositor/arbc/compositor/tile_planning.hpp`;
  `tile_planning.cpp:378-382`, `src/compositor/pull_service.cpp:180-185`,
  `src/compositor/refinement.cpp:79-87`). This is the tripwire imageseq's
  `render`/`quantize_time` pair must satisfy.
- `timeline.temporal_placement` — *settled*, refinement
  `tasks/refinements/timeline/temporal_placement.md`. Put `span`
  (`TimeRange`, half-open `[in,out)`) and `time_map` (`TimeMap`) on
  `LayerRecord` beside `transform` (`src/model/arbc/model/records.hpp:74-75`),
  with `set_span`/`set_time_map` transactional setters
  (`src/model/arbc/model/model.hpp:252,259`). Span culling and time-mapping are
  the compositor/transport's job over these fields; imageseq's placement in the
  end-to-end test uses them.
- `timeline.transport` — *settled*, `9456bf3`. `arbc::runtime::Transport`
  (`src/runtime/arbc/runtime/transport.hpp:34`) samples the `position()` that
  fills `RenderRequest.time`; `rate()` (negative allowed) drives the hint.
- `timeline.playback_hints` — *settled*, `d162f56`, refinement
  `tasks/refinements/timeline/playback_hints.md`. Landed
  `PlaybackHint`/`Content::playback_hint` and the runtime drive
  `derive_playback_hint` + `drive_playback_prefetch`
  (`src/runtime/arbc/runtime/playback_hints.hpp:57,77-80`). imageseq is the
  first production content this drive fans a hint out to.
- `surfaces.provided_surfaces` — *settled*, `bb5ab24`. `RenderResult.provided`
  (`SurfaceRef`, `content.hpp:113`) with the compositor consume/copy/release
  path — the decoder path imageseq returns frames through.
- `contract.conformance_suite` — *settled* (inherited via the `kinds` group
  `depends contract.conformance_suite`, `tasks/55-kinds.tji:4`), refinement
  `tasks/refinements/contract/conformance_suite.md`. Ships the `arbc-testing`
  STATIC library and `arbc::contract_tests(factory[, options])`
  (`testing/arbc/testing/contract_tests.hpp`) that imageseq's test binary links
  and runs — Decision 2 of that refinement: *each reference kind wires its own
  `arbc::contract_tests` run — that is the kind's task*.

**Pending (must NOT be assumed at implementation time):**

- `runtime.plugin_loading` (`tasks/65-runtime.tji:33-37`, milestone
  **M8**, `tasks/99-milestones.tji:62`) — the *production* host-side loader
  (explicit host registration API, opt-in `ARBC_PLUGIN_PATH` directory scan,
  errors-as-values across the boundary). It is a **later** milestone than M5,
  so imageseq cannot depend on it; imageseq lands the minimal `Registry` +
  `extern "C"` entry-point *seam* and dlopens its own `.so` **directly in its
  test**, and `runtime.plugin_loading` builds the scan-and-load machinery on
  top of that seam (see Decisions §1).
- `kinds.dual_build` (`tasks/55-kinds.tji:67`) — folds the six in-lib kinds
  into CI-only `.so`s through the same entry point; consumes imageseq's seam,
  is not consumed by it.
- `packaging.plugin_helper` (`tasks/75-packaging.tji:11`) — the shipped
  `arbc_add_plugin()` CMake helper. Absent today; imageseq hand-rolls its
  `MODULE` target and is the natural migration target when the helper lands
  (see Decisions §6).
- `model.content_binding` (`tasks/10-model.tji:57`) — runtime instantiation /
  live-sink wiring. imageseq's tests construct content directly, as
  `raster`/`nested` do; production runtime binding is out of scope.

## What this task is

Ship `arbc-plugin-imageseq`: an **out-of-`libarbc`** shared-library plugin,
built and tested in this repo, that registers the `org.arbc.imageseq`
content kind — `Timed` visual content backed by a sequence of decoded still
image frames at a fixed native rate. It carries its own **stb-class decode
dependency**, kept entirely private so no codec symbol ever enters `libarbc`'s
(or an embedder's) link line. The kind implements the temporal `Content`
contract end to end: `stability() == Timed`, `time_extent()` = clip duration,
`quantize_time(t)` = the nearest native source-frame instant, `render(t)`
returning that frame with `achieved_time == quantize_time(t)` and the decoded
frame handed back through `RenderResult.provided`, and `playback_hint(...)`
pre-rolling the decoder sequentially. Because it is the first artifact loaded
across the `extern "C" arbc_plugin_register(Registry&)` boundary, this task
also lands the minimal `Registry` and entry-point seam that boundary needs and
proves the whole plugin path in one dlopen-driven test.

## Why it needs to be done

- **M5: Video (`tasks/99-milestones.tji:42-45`) cannot close without it.** The
  timeline stack (transport, temporal cache, playback hints) built the
  temporal machinery against in-repo test doubles; imageseq is the first *real*
  `Timed` content that proves `achieved_time` on source frames, span-culled
  placement, and temporal prefetch end to end (`docs/design/11-time-and-video.md:256-257,288-289`).
- **It is the permanent end-to-end test of the runtime plugin path.** Doc 17's
  codec line (`docs/design/17-internal-components.md:150-159`) makes imageseq
  the one shipped artifact that exercises the real `dlopen` + `extern "C"`
  registration path at runtime — the in-lib kinds only exercise it through the
  CI-only dual-build. If this path rots, imageseq's test goes red.
- **It is the reference decoder for `RenderResult.provided`.** `surfaces.provided_surfaces`
  landed the content-provided-surface consume path; imageseq is the first
  content that returns a decoded frame through it (doc 03 "video decoder"
  case, `docs/design/03-layer-plugin-interface.md`), pinning that path against
  a real producer.
- **Downstream consumers.** `runtime.plugin_loading` (M8) builds its scan
  loader on the `Registry`/entry-point seam landed here; `kinds.dual_build`
  reuses the same entry point; the future codec-backed video kind
  (`docs/design/11-time-and-video.md:289`) reuses imageseq's `Timed`-source
  shape.

## Inputs / context

**Governing design-doc sections (normative, doc 16):**

- `docs/design/11-time-and-video.md` — the temporal `Content` contract. §"Time
  model" (flicks timebase = 705'600'000/s, exact rational rates, single
  ties-to-even leaf rounding, overflow as an error value). §"Model changes"
  (`span`/`time_map`/`time_extent`; `Timed` = deterministic function of request
  time, cacheable per time). §"Contract changes (doc 03)" (`11:110-114`
  `achieved_time` = nearest source frame; `11:124-126` the **MUST**:
  `quantize_time(t)` equals `render(time=t).achieved_time` and is idempotent;
  `playback_hint(direction,rate,horizon)` advisory, no-op-safe). §"Pipeline
  changes" (`11:138-143` cache key gains the achieved-time component; temporal
  prefetch ring; eviction order). §"Scheduling decision" (`11:256-257,288-289`
  names the image-sequence kind as the reference `Timed` proof;
  `org.arbc.imageseq`; a codec-backed video kind stays a later plugin).
- `docs/design/17-internal-components.md` — §"Shipped artifacts"
  (`17:15` `arbc-plugin-imageseq` shared, *outside* `libarbc`; `arbc-testing`
  static, linked by plugin test binaries, never by `libarbc`). §"The codec
  line" (`17:150-159`, **authoritative on placement**: built-in kinds are
  codec-free, imageseq ships separately with its own stb-class decode dep,
  becoming the permanent end-to-end plugin-path test). §"Repo layout"
  (`17:171` `plugins/imageseq/` "out-of-lib reference plugin (own deps)").
  §"The component graph" (`contract` L3 owns `Content` + `Registry`; `cache`
  L3 owns the prefetch ring; `runtime` L5 owns `dlopen`).
- `docs/design/03-layer-plugin-interface.md` — §"Sketch" (the `Content`
  vtable). §"Plugin mechanism" (`03:164-171` Stage-1 v1 regime: link-time or
  `dlopen` with `extern "C" arbc_plugin_register(Registry&)`; `03:177-180`
  boundary shaping: errors-as-values, no exceptions across the boundary).
  §"Registry" (`03:188-194` id → factory + metadata, reverse-DNS ids are the
  persistent contract). §"Reference implementations" table (`03:202`
  `org.arbc.imageseq`), **as amended by this task's doc delta** to mark the
  imageseq row out-of-lib per doc 17's codec line (see Decisions §2).
- `docs/design/09-surfaces-and-backends.md` — the content-provided-surface
  adoption/lifetime/sync rules `RenderResult.provided` obeys (`09:87-112`),
  cross-referenced from `content.hpp:101-112`.
- `docs/design/16-sdlc-and-quality.md` — testing taxonomy: content kinds run
  the contract conformance suite; deterministic rendering gets byte-exact
  goldens; performance-shaped promises get behavioral counters, never
  wall-clock (`16:54-62`); ≥90% diff coverage; concurrency-touching work scopes
  TSan.

**Existing seams the implementation extends (real paths + lines, current at HEAD):**

- `src/contract/arbc/contract/content.hpp` — the whole contract imageseq
  implements: `enum class Stability { Static, Timed, Live }` (`:27-33`);
  `struct RenderRequest { … Time time; … }` (`:78-86`, `time` at `:81`);
  `struct RenderResult { double achieved_scale; bool exact; std::optional<Time> achieved_time; std::optional<SurfaceRef> provided; }`
  (`:88-114`, `achieved_time` `:100`, `provided` `:113`); `struct PlaybackHint { int direction; Rational rate; Time horizon; }`
  (`:129-132`); pure `bounds()` (`:236`), `stability()` (`:237`),
  `time_extent()` (`:248`); null-defaulted `quantize_time(Time)` (`:269`),
  `render_thread_safe()` (`:309`), non-const `playback_hint(const PlaybackHint&)`
  (`:323`); the **MUST** contract comment at `:261-263`; `RenderCompletion`
  (`:153-185`); `editable()`/`audio()` facet defaults.
- `src/compositor/tile_planning.cpp:141-149,378-382` — the plan-time key and
  the consistency assertion imageseq's `render`/`quantize_time` must satisfy.
- `src/cache/arbc/cache/key_shapes.hpp:64-76` — `TileKey` with
  `std::optional<Time> achieved_time` (`:69`); `TileValue`/`TileMeta`
  (`:99-113`); `TileCache` (`:129`); the flicks==0 present-vs-absent hash guard
  (`:144-161`). imageseq feeds these indirectly; it never names cache types.
- `src/cache/arbc/cache/prefetch.hpp:110,133` — `temporal_prefetch_ring` /
  `prime_ring`; `PriorityClass::Temporal` (`src/cache/arbc/cache/keyed_store.hpp:29`).
  Driven by the runtime, fed by imageseq's `playback_hint` override.
- `src/runtime/arbc/runtime/playback_hints.hpp:57,77-80` — `derive_playback_hint`
  / `drive_playback_prefetch`, exercised by the prefetch test.
- `src/model/arbc/model/records.hpp:74-75` — `LayerRecord.span` / `.time_map`;
  `src/model/arbc/model/model.hpp:252,259` — `set_span` / `set_time_map`;
  `src/base/arbc/base/time.hpp` (`Time`/`TimeRange::all()`) and
  `rational_time.hpp` (`Rational`/`TimeMap`/`present_in_span` at `:163-165`).
- `src/kind_raster/` and `src/kind_solid/` — the decode idiom to reuse: pixels
  flow only through `arbc::media` `PixelTraits<F>` decode→working-floats→encode
  + `visit_surface`/`span<F>()`, **never** `backend-cpu` kernels
  (`src/backend_cpu/kernels.hpp` is L3 and forbidden to a plugin).
- CMake: `cmake/ArbcComponent.cmake` — `arbc_add_component` (OBJECT libs folded
  into umbrella `arbc` by `arbc_finalize_library`), `arbc_add_testing_library`
  (the STATIC `arbc-testing`, include-only DEPENDS), `arbc_component_test`.
  Root `CMakeLists.txt` adds `src`/`testing`/`tests`; global `-fvisibility=hidden`
  (`CMakeLists.txt:16-17`). `tests/CMakeLists.txt:23-27,32-36` — the
  per-kind conformance executables (`arbc-testing arbc Catch2::Catch2WithMain`,
  arbc-testing before arbc on the link line).

**Predecessor / sibling conventions followed:** `tasks/refinements/kinds/raster.md`
and `.../nested.md` (house structure, acceptance shape, `enforces:`-tagged
claims, behavioral counters, deferred-follow-up formatting);
`tasks/refinements/timeline/temporal_cache.md`, `.../playback_hints.md`,
`.../temporal_placement.md` (temporal seam contracts and claim-slug style).

## Constraints / requirements

1. **Levelization / codec containment (CI-enforced, doc 17 §codec line,
   `scripts/check_levels.py`).** `arbc-plugin-imageseq` is an out-of-lib
   `SHARED`/`MODULE` target, **not** an `arbc_add_component` OBJECT library
   (those fold into `libarbc`, which would drag the codec into core). It links
   the umbrella `arbc` (PUBLIC, for `Content`/contract symbols + headers) plus
   its **own private** stb-class decode dependency. The decode dependency must
   **never** be added to any `arbc_*` component, to `arbc`, or to `arbc-testing`.
   The plugin must not link `backend-cpu` kernels. A repo-wide check that no
   codec symbol appears in `libarbc`'s link line is part of the task
   (see Acceptance).
2. **The `Registry` + entry-point seam is minimal and doc-specified.** Land
   `Registry` (id → factory + metadata) in `arbc::contract` (L3, where doc 17
   places it and doc 03 §Registry specifies it) and an `arbc/contract/plugin.hpp`
   declaring `extern "C" void arbc_plugin_register(arbc::Registry&)` plus an
   export-visibility macro (global preset is `-fvisibility=hidden`). This is
   implementing a doc-03-specified seam, not new design (Decisions §1) — do
   **not** build the `ARBC_PLUGIN_PATH` scanner or host registration API; those
   are `runtime.plugin_loading`'s (M8).
3. **`Timed` contract, exactly (doc 11).** `stability()` returns
   `Stability::Timed`. `time_extent()` returns the clip's local-time duration
   `[0, N·period)` (never `nullopt` — that is `Static`). `quantize_time(t)`
   returns the nearest native source-frame instant as an exact rational rounded
   once to flicks (ties-to-even), clamped to the available frame range;
   `render(time=t).achieved_time` **must equal** `quantize_time(t)` and
   `quantize_time` **must be idempotent** (`content.hpp:261-263`,
   `11:124-126`). Native-rate math is exact rational; a rate/time-map overflow
   surfaces as an error value, never silent wrap.
4. **Provided-surface decoder path (doc 03 §points, doc 09).** `render` decodes
   the resolved frame via `arbc::media` `PixelTraits` into a surface carrying
   the composition working-space tag triple and returns it through
   `RenderResult.provided` (non-transient, refcounted from the plugin's decoded-
   frame cache), honoring the compositor's consume/release contract. Pixels
   route only through `media` decode/encode, never `backend-cpu`.
5. **`render_thread_safe() == false`.** The decoder is stateful (a bounded
   decoded-frame cache + pre-roll state mutated by `playback_hint`), so it
   returns `false` and the core serializes its requests through the per-content
   queue (doc 03 sketch names "a stateful decoder"; `runtime.threading`
   machinery, landed). This is a deliberate exercise of that path, not an
   accident (Decisions §5).
6. **`playback_hint` is advisory and correctness-neutral.** The override may
   pre-roll the decoder in `hint.direction` up to `hint.horizon`, but rendered
   pixels **must be byte-identical whether or not a hint was issued** (an empty
   paused hint pre-rolls nothing). Determinism stays owned by
   `quantize_time`/`achieved_time`.
7. **Boundary discipline (doc 03 §Stage-2 shaping, `03:177-180`).** No
   exceptions cross the `extern "C"` boundary; decode/open failures are values
   (`expected`/`RenderError` via `RenderCompletion::fail`), never thrown or
   aborting. A missing/corrupt frame file yields an error value, not UB.
8. **Determinism.** stb decode + `media` sRGB8→working-space conversion is
   byte-exact and reproducible (leaning on the landed
   `07-color-and-pixel-formats#srgb8-round-trips-exactly` /
   `#kernels-byte-exact-per-format` guarantees), so goldens are byte-exact with
   no tolerance.
9. **Repo layout (doc 17 §repo layout).** Plugin sources live under
   `plugins/imageseq/` (peer to `src`/`testing`/`tests`), added via a guarded
   `add_subdirectory(plugins/imageseq)` from the root `CMakeLists.txt`. Test
   fixture frames live under the plugin's test data, checked in and tiny.

## Acceptance criteria

**Conformance run (doc 16 — content kinds run the contract suite).** A Catch2
test binary under `plugins/imageseq/t/` (or `tests/`) links
`arbc-testing arbc Catch2::Catch2WithMain` and calls `arbc::contract_tests(factory)`
with a factory producing a fresh `org.arbc.imageseq` content over the checked-in
fixture sequence. It gates the already-registered plugin-contract claims via a
second `enforces:` test (not re-registered), including
`03-layer-plugin-interface#render-pure-over-pinned-state`,
`#render-completion-settles-once`, `#render-scale-honest`,
`#render-within-declared-bounds`, `#undamaged-regions-stable`,
`#facet-consistency`, `#leaf-content-has-no-operator-graph`, and the temporal
pair `#render-time-honest` / (Timed, so **not** `#static-time-invariant`).

**New claims-register entries (`tests/claims/registry.tsv`, each pinned by an
`enforces:`-tagged `TEST_CASE`, format `<doc-stem>#<slug>`):**

- `11-time-and-video#imageseq-achieved-time-is-nearest-source-frame` — an
  `org.arbc.imageseq` content asked for a sub-frame time reports
  `achieved_time == quantize_time(t) ==` the nearest native source-frame
  instant, `quantize_time` is idempotent, and two distinct times inside one
  native frame interval render byte-identical pixels. *(The reference-kind
  proof of the doc-11 `achieved_time` contract.)*
- `03-layer-plugin-interface#plugin-registers-through-extern-c-entry` — a
  shared-library plugin `dlopen`ed and invoked through
  `extern "C" arbc_plugin_register(Registry&)` populates the `Registry` with
  its kind id and yields a factory that constructs a working `Content`;
  registration errors are values, not exceptions.
- `17-internal-components#imageseq-decode-dep-stays-out-of-libarbc` — the
  stb-class decode dependency resolves only in `arbc-plugin-imageseq` and no
  codec symbol appears in `libarbc`'s exported symbols / link line. *(The codec-
  line containment proof.)*

**Re-asserted end-to-end (second `enforces:` tests, not re-registered):**
`11-time-and-video#coalesced-timed-tile-round-trips-through-cache`,
`#achieved-time-coalescing-issues-zero-renders`,
`#temporal-prefetch-ring-bounded-by-horizon`, `#span-cull-is-half-open`,
`09-surfaces-and-backends#content-provided-surface-honored`,
`#provided-surface-released-after-consume`.

**Byte-exact goldens (deterministic rendering, doc 16 — goldens the default,
tolerances the justified exception).** A cross-component golden test drives an
imageseq layer through the compositor at fixed times and asserts byte-identical
output against checked-in goldens: (a) an on-grid frame instant; (b) a
sub-frame time resolving to the same frame (proving `achieved_time` coalescing
serves the same pixels from one cache entry); (c) a reverse-rate time. Suite
lives under `plugins/imageseq/t/` for the content-local render and `tests/` for
the through-compositor path.

**Behavioral-counter assertions (doc 16 `16:54-62` — never wall-clock).** The
content exposes a `decodes_issued` counter (decoded-frame-cache miss count).
Assertions: N sub-frame times inside one native interval issue exactly one
decode; a paused (empty) hint pre-rolls zero frames; a forward hint with
horizon H pre-rolls exactly the frames the ring covers. Tile coalescing /
zero-render-under-clock-advance are asserted via
`KeyedStore::hits()/misses()`, mirroring the temporal-cache tests.

**Provided-surface + advisory-hint safety.** A test asserts the decoded frame
returned via `RenderResult.provided` is honored and released per doc 09, and
that rendered pixels are byte-identical with and without a `playback_hint`
issued (constraint §6).

**Concurrency (doc 16 — concurrency-touching tasks scope their coverage).**
Because `render_thread_safe() == false`, a TSan + stress test drives concurrent
interactive + temporal-prefetch renders of one imageseq content through the
per-content serialization queue (`runtime.threading` machinery) and asserts no
data race and a correct serialized decode order.

**Plugin-path end-to-end.** One test performs the full path: build
`arbc-plugin-imageseq` → `dlopen` the `.so` → resolve and call
`arbc_plugin_register` → obtain the `org.arbc.imageseq` factory from the
`Registry` → construct the content → run a render and a conformance slice. This
is the permanent regression guard for the runtime plugin path.

**CI gates.** `scripts/check_claims.py` (both directions), `scripts/check_levels.py`
(including codec-containment), `diff-cover --fail-under=90`, clang-format
`-Werror`, and warning-free `tj3 project.tjp` all green. Design-doc delta
(Decisions §2) rides in the same commit.

**Deferred follow-ups (no new WBS leaf required; consumed by existing pending
tasks — closer wires the edges noted):**

- `runtime.plugin_loading` (`tasks/65-runtime.tji:33`, M8) builds the production
  scan-and-load loader on the `Registry`/entry-point seam landed here. **Closer
  should add `depends kinds.imageseq_plugin` to `runtime.plugin_loading`** so
  the seam is a settled predecessor when the loader is written. (M8 already
  follows M5, so this respects milestone order.)
- `packaging.plugin_helper` (`tasks/75-packaging.tji:11`, M9) migrates
  imageseq's hand-rolled `MODULE` target onto the shipped `arbc_add_plugin()`
  helper as its proof — owned by that task, no new leaf.
- `kinds.dual_build` (`tasks/55-kinds.tji:67`, M9) reuses this entry point for
  the in-lib kinds' CI `.so`s — already wired.

## Decisions

1. **This task lands the minimal `Registry` + `extern "C"` entry-point seam and
   dlopens its own `.so` in-test; it does not build the production loader.**
   The `Content` contract needs a `Registry` to register into and an
   entry-point symbol to be loaded across, but the production loader
   (`ARBC_PLUGIN_PATH` scan, host registration API, error plumbing) is
   `runtime.plugin_loading`'s explicit deliverable in **M8** — a *later*
   milestone than imageseq's **M5** (`tasks/99-milestones.tji:44` vs `:62`), so
   imageseq cannot depend on it. Resolution: imageseq lands the small,
   doc-specified `Registry` (doc 03 §Registry, placed at `contract` L3 by doc
   17) and the `arbc_plugin_register(Registry&)` ABI, and its end-to-end test
   `dlopen`s the plugin directly — which *is* the "permanent end-to-end test of
   the runtime plugin path" the note calls for. `runtime.plugin_loading` then
   builds the scanner on this seam. *Rejected: making imageseq `depend
   runtime.plugin_loading`* — inverts the M5/M8 milestone order and would push
   the video milestone behind persistence. *Rejected: a fresh
   `contract.registry` task* — no dependent needs `Registry` before imageseq,
   and landing a doc-specified L3 seam inside the kind that first needs it is
   the established precedent (`kinds.raster` landed the `Editable` L3 interface
   the same way, `tasks/refinements/kinds/raster.md`).
2. **Reconcile doc 03's stale "shipped with core" table with doc 17's codec
   line via a design-doc delta.** doc 03 §"Reference implementations (shipped
   with core)" lists `org.arbc.imageseq` (`03:202`), but doc 17 §"The codec
   line" (`17:150-159`) rules it ships *outside* `libarbc`. Doc 17 is the later,
   more specific ruling (it reconciles doc-11 v1-scope against doc-10 dependency
   policy). Delta: a clarifying paragraph after the doc-03 table marking the
   imageseq row as the out-of-lib exception per doc 17, so an implementer never
   folds it into core. This is a documentation-consistency clarification, not a
   behavior change — no doc-00 decision bullet (doc 17 already recorded the
   decision). *Rejected: leaving doc 03 stale and only citing doc 17 in the
   refinement* — the constitution should not contradict itself on this task's
   central artifact.
3. **"Image sequence" = decoded still frames via an stb-class dependency, not a
   real video codec.** The kind decodes numbered still frames (stb_image-class,
   header-only, permissively licensed) at a fixed native rate. This is exactly
   the "stb-class decode dependency" doc 17 §codec line pre-sanctions
   (`17:156`), so it introduces **no** new doc-10-policy dependency decision,
   yet it is a genuine third-party decode dependency that must stay out of
   `libarbc` — satisfying the codec-line containment proof honestly. *Rejected:
   an ffmpeg-class codec-backed video kind* — doc 11 explicitly defers that to a
   later plugin (`11:289`) and it is a doc-10 dependency + licensing judgment
   (surfaced to the parking lot, not scoped here).
4. **Return the decoded frame through `RenderResult.provided` (non-transient),
   not by filling `request.target`.** The decoder owns a bounded decoded-frame
   cache; returning the working-space-tagged frame as a refcounted provided
   surface lets many tiles / sub-frame times at one instant share a single
   decode (the compositor samples the requested region/scale from it), which is
   the whole point of a decoder kind and the first real exercise of the
   `surfaces.provided_surfaces` consume path. *Rejected: filling `request.target`
   like `raster`* — would re-decode per tile and not exercise the provided-surface
   decoder path that is imageseq's reason to exist. *Also rejected: a `transient`
   provided surface* — the frame is reused across many cache entries within a
   frame, so a retained refcounted surface (copied into cache-owned tiles by the
   compositor) is the correct lifetime.
5. **`render_thread_safe()` returns `false`.** The decoder is stateful (frame
   cache + `playback_hint` pre-roll), so serialization through the per-content
   queue is both required for correctness and the deliberate reference exercise
   of that path (`runtime.threading`, landed; doc 03 names "a stateful decoder"
   as the case). *Rejected: a stateless open-decode-close returning `true`* —
   would re-decode redundantly and leave the per-content serialization path with
   no shipped exerciser.
6. **Hand-roll the `MODULE` target now; migrate to `arbc_add_plugin()` later.**
   `packaging.plugin_helper` (which ships `arbc_add_plugin()`) is M9; imageseq
   is M5. imageseq adds its own `add_library(arbc-plugin-imageseq MODULE …)` in
   `plugins/imageseq/CMakeLists.txt` linking `arbc` PUBLIC + the private decode
   dep, with a guarded root `add_subdirectory`. When the helper lands, that task
   migrates imageseq as its proof. *Rejected: blocking on the helper* — inverts
   the milestone order.
7. **Native frame math is exact rational, quantized once to flicks.**
   `quantize_time`/`achieved_time` compute the frame instant in exact rationals
   (`Rational`/`TimeMap`) and round once, ties-to-even, at the leaf
   (doc 11 §time model), clamped to `[0, N·period)`; overflow surfaces as a
   `TimeError` value. This makes the `timed_insert_key_consistent` assertion and
   the idempotence MUST hold by construction. *Rejected: floating-point frame
   index* — would break byte-exact goldens and the key-consistency tripwire.

## Open questions

(none — all decided.) One non-blocking WBS-shape observation is surfaced to the
closer in the return summary: `runtime.plugin_loading` should gain a
`depends kinds.imageseq_plugin` edge so it builds on the `Registry`/entry-point
seam this task lands (milestone order M5→M8 already permits it). The future
codec-backed (ffmpeg-class) video kind is a doc-10 dependency + licensing
judgment — parking-lot material, not a WBS leaf.

## Status

**Done** — 2026-07-07.

- Shipped `arbc-plugin-imageseq` as an out-of-`libarbc` `MODULE` target under `plugins/imageseq/`, carrying its own private PPM/PGM decoder (`plugins/imageseq/third_party/imdec.h`, public-domain stb-class) — no codec symbol enters `libarbc`'s link line.
- Landed the minimal `contract::Registry` + `extern "C" arbc_plugin_register(Registry&)` seam at L3 (`src/contract/arbc/contract/registry.hpp`, `src/contract/arbc/contract/plugin.hpp`, `src/contract/registry.cpp`, `src/contract/t/registry.t.cpp`), implementing the doc-03/doc-17-specified boundary; `runtime.plugin_loading` (M8) builds the production scanner on this seam.
- Implemented `ImageseqContent` in `plugins/imageseq/arbc/kind_imageseq/imageseq_content.hpp` / `imageseq_content.cpp`: `Stability::Timed`, exact-rational `quantize_time`, `render` returning decoded frames via `RenderResult.provided`, `render_thread_safe()=false`, advisory `playback_hint` pre-roll; `render(t).achieved_time == quantize_time(t)` by construction.
- Added test fixtures (`plugins/imageseq/t/fixtures/frame_000{0..3}.ppm`) and 8 acceptance test drivers under `tests/imageseq_*.t.cpp` + `tests/support/imageseq_fixtures.hpp`: conformance, achieved-time, provided-surface, temporal, prefetch, plugin-path dlopen, codec-containment byte-scan, TSan/stress concurrency.
- Registered 3 new claims in `tests/claims/registry.tsv`: `11-time-and-video#imageseq-achieved-time-is-nearest-source-frame`, `03-layer-plugin-interface#plugin-registers-through-extern-c-entry`, `17-internal-components#imageseq-decode-dep-stays-out-of-libarbc`.
- Updated `CMakeLists.txt` (guarded `add_subdirectory(plugins/imageseq)`), `src/contract/CMakeLists.txt`, `tests/CMakeLists.txt`; design-doc delta (doc-03 Decision 2 clarifying imageseq as out-of-lib exception) applied to `docs/design/03-layer-plugin-interface.md`.
- Deferred: compositor does not yet consume `LayerRecord.span`/`time_map` at `composition_time`; reverse-via-`time_map` through-compositor golden deferred; `#span-cull-is-half-open` stays enforced by `rational_time.t.cpp`. Follow-up registered as `compositor.temporal_placement_culling`.
