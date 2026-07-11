# Parking lot — items for human review

This file is the queue for decisions the automated WBS loop (orchestrator +
closer + refinement_writer) **must not** make on its own: judgment calls,
"should we revisit X later" questions, scope/descope decisions, and anything
that would otherwise be (mis)encoded as a self-perpetuating "audit" WBS
task.

**Why this exists:** an open question encoded as a WBS task gets picked up
by the orchestrator, can't be closed by an implementer (the work is a human
call), and spawns a successor — a loop. Instead of a task, the closer
appends an entry here and moves on. The human triages this file and either
resolves the item, wires real *implementation* work into a milestone, or
deletes the entry.

**Who writes here:** the closer (`orchestrator/prompts/closer.md`, ritual
step 4) appends entries — both items it hits during the ritual and items
the implementer / refinement_writer flagged for human review in their
return summaries. The orchestrator's `human-intervention-needed` stop also
points here.

**What triage does with an entry.** A triage round ends an entry in one of
three ways:

- **Resolved → delete the block.** The decision does not live here; it lives
  where it will actually be read. If the resolution is work, that is the new
  WBS leaf's `note` (which cites `tasks/parking-lot.md <date> (<title>)` as
  its source); if it is a rejection or a finding, that is a comment or a test
  in the code it concerns. `git log -p tasks/parking-lot.md` recovers the
  original question whenever the provenance is wanted. **A resolution that
  leaves no trace outside this file has not been resolved** — give it a home
  first, then delete the block.
- **Re-parked → append an `- **Updated** (<date>, triage):` line.** Say what
  changed and why the trigger still has not fired. Correct the premise if the
  world moved under it; a stale premise silently keeps an entry alive.
- **Still open, nothing new → leave it alone.**

So this file holds only *live* questions. It shrinks.

## Format

Append one `###` block per item, newest at the bottom:

```
### <YYYY-MM-DD> — <short title>
- **Source**: closer for `<task_id>` (commit `<sha>`), or the run that surfaced it.
- **Question**: the decision the loop could not make.
- **Why parked**: judgment call / preconditions unmet / scope decision.
```

### 2026-07-05 — SurfacePool byte-budget / one-frame-trim eviction policy
- **Source**: closer for `surfaces.surface_pool` (see accompanying commit); flagged in refinement Open questions.
- **Question**: Should a byte-budget or one-frame-trim eviction policy be added to `SurfacePool`? A long-lived cross-frame pool under sustained camera motion (many distinct temp sizes) could accumulate stale entries, but the only cross-frame caller — the interactive/video renderer — does not exist yet. Whether accumulation matters is a profiling-dependent judgment.
- **Why parked**: Trigger is interactive-renderer profiling evidence that doesn't exist yet. Adding a policy now is speculative; the refinement explicitly defers this until that renderer's profiling reveals a real problem. Human call once the interactive renderer is built and profiled.
- **Updated** (2026-07-10, triage): the interactive per-frame driver now exists (`InteractiveRenderer::render_frame`, `src/runtime/interactive.cpp`) but no profiling/benchmark infrastructure targets it. The profiling trigger stands; still parked.

### 2026-07-05 — KeyedStore concurrency hardening for async worker-fill + render-thread lookup
- **Source**: closer for `cache.keyed_store` (see accompanying commit); flagged in refinement Open questions.
- **Question**: `KeyedStore` is single-threaded-confined at v1. When the async render/mix model (doc 02:101-117) drives cache fills from worker threads while the render thread does lookups, the store needs reader-safe lookup and synchronized insert/evict (plus TSan + schedule-perturbation stress, doc 16:64-73). Should `KeyedStore` be hardened for concurrent access at that point?
- **Why parked**: Trigger is the async off-thread fill path landing — a runtime-architecture decision not yet made (doc 02:118-120 leaves it open). Hardening now would guess at the concurrent design and pay synchronization cost on the hot path meanwhile. Human call once the async fill model is decided.

### 2026-07-05 — Recycling evicted backend surfaces back into SurfacePool
- **Source**: closer for `cache.keyed_store` (see accompanying commit); flagged in refinement Open questions.
- **Question**: On eviction `KeyedStore` drops the `Value`, freeing its backend surface. That surface could instead be returned to `surfaces::SurfacePool` for reuse. Should this recycling path be added?
- **Why parked**: Whether the recycling pays is profiling-dependent and it couples the cache's eviction path to the pool's lifecycle. No interactive renderer exists yet to profile against. Human call once the interactive renderer is built and profiling reveals whether the optimization is worth the coupling.
- **Updated** (2026-07-10, triage): the interactive per-frame driver now exists but remains unprofiled (see the SurfacePool entry above). The profiling trigger stands; still parked.

### 2026-07-06 — Per-content revision granularity for aggregate-revision fold
- **Source**: closer for `compositor.operator_graph` (see accompanying commit); flagged in refinement Open questions and Decision 3.
- **Question**: The aggregate-revision fold in `operator_graph.cpp` uses the document-global `DocRoot::revision()` as every node's contribution (correct and never stale, but conservative — any edit bumps every operator's cache key). Doc 05:84 envisions per-object revisions so that a static sibling's cached tiles survive an unrelated edit elsewhere. Realizing this optimization requires the model to expose and bump per-content revision numbers (distinct from the document-global counter), with coalescing/undo semantics settled. Should a model-stream task be added to expose per-object revisions so the compositor fold can drop in finer-grained contributions with no compositor change?
- **Why parked**: The selectivity optimization depends on a model-stream design judgment (where per-object revisions live, how coalescing/undo affect them) that is out of the compositor's levelization and not required for correctness. The fold's `contribution` callback seam (`operator_graph.hpp`) already accepts per-node values — the compositor change is zero when model revisions land. Human call once the model stream is ready to scope per-object revisions.
- **Updated** (2026-07-10, triage): the model stream is ready, but the payoff (cached tiles surviving unrelated edits) is unproven without interactive profiling. Re-parked with the trigger re-worded to interactive-renderer profiling evidence, matching the SurfacePool/recycling entries. The contribution seam still means zero compositor work when it lands.

### 2026-07-06 — Concurrent operator-input pull evaluation (sharded cache)
- **Source**: closer for `compositor.pull_service` (see accompanying commit); Decision 4 and implementer return summary.
- **Question**: `PullServiceImpl` v1 evaluates recursive operator-input pulls synchronously on the frame thread (single-writer cache discipline). Doc 05 / doc 13 do not forbid parallel input evaluation; fanning input pulls onto workers concurrently could improve throughput for wide operator graphs. Should a follow-up task be added to implement concurrent/sharded input evaluation once a concurrent-safe `KeyedStore` variant exists?
- **Why parked**: The optimization requires a concurrent or sharded cache (the single-writer discipline is load-bearing for v1 correctness) and a larger TSan surface. No v1 milestone requires it. The decision of whether and when to build it is profiling-driven and depends on the concurrent-cache design question (already parked 2026-07-05 for `cache.keyed_store`). Human call once profiling evidence and a concurrent cache are available.

### 2026-07-07 — Zero-copy adoption of a non-transient provided surface as a cache value
- **Source**: closer for `surfaces.provided_surfaces` (see accompanying commit); flagged in refinement Open questions and Decisions.
- **Question**: Doc 09 permits caching a provided surface by holding the `SurfaceRef` directly as a `TileValue` (no copy). v1 always copies into a cache-owned surface. Should a follow-up task add zero-copy adoption (holding `SurfaceRef` inside `TileValue`) once a GPU backend makes the copy cost measurable?
- **Why parked**: The optimization requires a `TileValue` variant that owns either a `unique_ptr<Surface>` or a `SurfaceRef`, plus updated byte-accounting and pin logic — a `cache`-layer change whose payoff is real only for GPU textures (a backend not yet built). Profiling-dependent judgment call; no WBS task until the GPU backend exists and a measured copy cost motivates it.

### 2026-07-07 — Cross-tag convert-at-composite for content-provided surfaces
- **Source**: closer for `surfaces.provided_surfaces` (see accompanying commit); flagged in refinement Open questions and Decisions.
- **Question**: Doc 09:102-105 allows a provided surface to carry a differently-tagged format (e.g. sRGB8) and have the backend convert at composite time. v1 requires a working-space tag because the CPU backend stores one format and asserts tag agreement. Should convert-at-composite be added once a backend advertises a second storable format?
- **Why parked**: The conversion extension is gated on a backend that supports multiple storable formats (`color.kernels` / GPU backends) — a capability not yet built. The owning task for that backend should add the conversion at that time. No standalone WBS task needed; this entry is the trigger pointer.
- **Updated** (2026-07-10, triage): the parked premise is stale — the CPU backend now stores three working formats (rgba32f/rgba16f/rgba8srgb via `color.kernels`, `cpu_backend.cpp:43-53`) though it still asserts exact tag agreement. Re-parked with concrete owners: `surfaces.import` / `kinds.raster_runtime_binding` (M9) are the first real foreign-tag consumers; whichever lands imported sRGB8 surfaces should decide convert-at-decode vs convert-at-composite and implement it there.
- **Updated** (2026-07-11, triage): one of the two named owners has now landed — `kinds.raster_runtime_binding` is `complete 100` and did **not** take the decision (it introduced no foreign-tag path). The sole remaining owner is therefore `surfaces.import` (M9, READY), which is where imported foreign-tagged surfaces actually arrive. Still parked, but this is no longer a floating "whichever gets there first": `surfaces.import` owns it, and if it lands without deciding, this entry should become a leaf rather than be re-parked a third time.

### 2026-07-08 — Device-loss / hot-plug / mid-stream device-format-change resilience
- **Source**: closer for `audio.device_monitor` (see accompanying commit); flagged in implementer return summary and refinement Acceptance criteria ("Registers no successor").
- **Question**: Should a v1-scope leaf be added for resilience to device-loss, hot-plug, and mid-stream device-format-change events (graceful reconnect, format renegotiation)?
- **Why parked**: The refinement explicitly flags this as a human judgment call — the feature "needs the real backend to be meaningful and may be v2 hardening." No simulation path exists for device-loss in a headless environment; meaningful testing requires a real backend and real hardware events. Scope/release-generation decision for a human once the reference backend and the associated integration test infrastructure are mature enough to make the behavior testable.

### 2026-07-08 — HRTF, distance models beyond attenuation, and non-collapsing per-leaf pan
- **Source**: closer for `audio.spatial_policy` (see accompanying commit); "Out of scope" section and doc 12:162-165.
- **Question**: Should follow-up tasks be added for HRTF / 3D audio monitors, distance models beyond per-edge scale attenuation, and non-collapsing per-leaf pan (preserving nested internal stereo width rather than mono-collapsing at nesting boundaries)?
- **Why parked**: Doc 12:162-165 explicitly defers these as "monitor-implementation territory, extensible later." They require multichannel accumulation, IR convolution infrastructure, and a non-collapsing pan model — scope beyond any current milestone. Human call once a milestone consumer is scoped for 3D/HRTF audio.

### 2026-07-09 — Symmetric additive crossfade for semi-transparent layers

- **Source**: closer for `operators.crossfade` (see accompanying commit); flagged in Decision 1 (Decisions section) and implementer return summary.
- **Question**: Decision 1 uses source-over-at-opacity `w` for the visual dissolve. For opaque inputs this yields the textbook linear crossfade. For **semi-transparent** inputs the result is asymmetric: input 0's pre-existing alpha interacts with input 1's overlay in a way that a symmetric additive `in0·(1−w) + in1·w` in premultiplied space would not. Should a symmetric additive `Backend` op (`accumulate(dst, src, weight)`) be added and a new `org.arbc.crossfade_additive` kind or mode registered to handle semi-transparent layer dissolves correctly?
- **Why parked**: Adding an additive backend op is a new L3/L4 seam every backend (CPU + future GPU) must implement — a doc-09 delta — for a v1 whose inputs are overwhelmingly opaque clip frames. The decision is explicitly deferred in Decision 1's "Rejected" rationale. Human call once a real semi-transparent dissolve use case surfaces and motivates the backend seam and doc amendment.

### 2026-07-08 — Live viewport-extent / window-resize follow for audio pan normalization
- **Source**: closer for `audio.spatial_camera_follow` (see accompanying commit); "Out of scope" section in the refinement.
- **Question**: Should audio pan normalization (`viewport_w/h` in `Spatialization`) follow live window-resize events, not just the initial static seed? The `camera_source` seam from `audio.spatial_camera_follow` makes this a trivial extension (inject a closure returning current extent alongside the camera); the question is whether v1 needs it.
- **Why parked**: Window resize is a rare event and the pan normalization behavior under a fixed window is already correct and goldened. Whether re-normalizing on resize matters to the product is a judgment call; no current milestone or user story requires it. Human call once an interactive host exists and user-facing pan behavior under resize can be evaluated.

### 2026-07-11 — Should the model carry an explicit root-composition marker instead of lowest-id-wins?
- **Source**: `serialize.compositions_table` (Decision 8); surfaced deliberately as a parking-lot question rather than a WBS leaf.
- **Question**: The document's root composition is defined as "the lowest-id composition" (`DocRoot::find_first_composition`, `model.hpp:60`), and `working_space()` / `working_audio_format()` / `capture_snapshot` all assume it. `serialize.compositions_table` made a document a *graph* of compositions, which sharpens the question: the reader now GUARANTEES the invariant (it allocates the root's `ObjectId` before any child's, so a loaded document always satisfies it), but a *programmatic* `Document` that creates a child composition BEFORE its root would serialize the child as the root. Should the model instead carry an explicit root marker?
- **Why parked**: An explicit marker means a new field on the fixed-layout, mmap-backed `CompositionRecord` (`records.hpp:136-144`) — workspace-file shape (doc 15) — plus a doc 14 delta: an order of magnitude more work than the residual hazard justifies, and a record-layout judgment call rather than a closeable leaf. The hazard is narrow and is an *authoring* question, not a load-path one: every loaded document is safe by construction, and `runtime.nested_codec` (M8) creates the parent before the child. Human call on whether v1 wants the marker.
- **Updated** (2026-07-11, triage): sharpened, and deliberately kept parked. `compositor.root_composition_frame_walk` (M9, scoped this round) makes the *render* path take an explicit root-composition `ObjectId` rather than re-deriving one — today no render driver calls `find_first_composition` at all, and lowest-id-wins survives only inside `working_space()`/`working_audio_format()` (`model.cpp:412-435`, which re-implements the scan) and the serialize writer. Passing the id in means the compositor stops caring how the root is chosen, which removes the render-path motivation for a record-layout marker and leaves only the narrow authoring hazard. Revisit if a host is ever expected to author a child composition before its root; the record-layout change is a doc 14 + doc 15 delta either way.

### 2026-07-11 — `model.persistent_state_walk_hook`: StateHandle slab-walk hook for persistent kind-owned content state
- **Source**: closer for `model.workspace_backing`; mandated by refinement Decision 5 and the "Deferred follow-up" section.
- **Question**: The recovery walk visits each `ContentRecord`'s `StateHandle` but descends nothing (all are `k_state_none` in v1). When a persistent kind ships workspace-backed content-state slabs, the walk needs a per-kind hook (`StateReachabilitySink`, mirroring `StateRefSink`) to count those slab slots. Should that hook be wired when the first persistent kind lands, and what shape should the sink take?
- **Why parked**: No kind ships persistent workspace-backed content state yet; the hook's shape is determined by the first such kind's slab layout — a design call that must accompany that kind's task. The walk is structured (Decision 5) so the hook slots in without rework. Not a WBS leaf until a concrete persistent kind defines the slab.

