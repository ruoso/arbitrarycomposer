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

### 2026-07-05 — KeyedStore concurrency hardening for async worker-fill + render-thread lookup
- **Source**: closer for `cache.keyed_store` (see accompanying commit); flagged in refinement Open questions.
- **Question**: `KeyedStore` is single-threaded-confined at v1. When the async render/mix model (doc 02:101-117) drives cache fills from worker threads while the render thread does lookups, the store needs reader-safe lookup and synchronized insert/evict (plus TSan + schedule-perturbation stress, doc 16:64-73). Should `KeyedStore` be hardened for concurrent access at that point?
- **Why parked**: Trigger is the async off-thread fill path landing — a runtime-architecture decision not yet made (doc 02:118-120 leaves it open). Hardening now would guess at the concurrent design and pay synchronization cost on the hot path meanwhile. Human call once the async fill model is decided.

### 2026-07-05 — Recycling evicted backend surfaces back into SurfacePool
- **Source**: closer for `cache.keyed_store` (see accompanying commit); flagged in refinement Open questions.
- **Question**: On eviction `KeyedStore` drops the `Value`, freeing its backend surface. That surface could instead be returned to `surfaces::SurfacePool` for reuse. Should this recycling path be added?
- **Why parked**: Whether the recycling pays is profiling-dependent and it couples the cache's eviction path to the pool's lifecycle. No interactive renderer exists yet to profile against. Human call once the interactive renderer is built and profiling reveals whether the optimization is worth the coupling.

### 2026-07-05 — HAMT interior nodes not trivially destructible (model.persistent_state design deviation)
- **Source**: closer for `model.persistent_state` (see accompanying commit); flagged by implementer.
- **Question**: Docs 14/15 require records to be trivially destructible so `ReclamationQueue::drain` walks them explicitly. The three *object* record types (composition, layer, content) satisfy this and pass the `static_assert` in acceptance criteria. However HAMT interior nodes are **not** trivially destructible — they own child `SlotRef` edges and the drain cascade releases children through `~T`, with nodes reaching their stores via a thread-local `ReclaimContext` the drainer publishes. Should docs 14/15 be amended to distinguish between object records (trivially destructible, mmap-compatible) and map-node records (allowed non-trivial dtor via sink-routed cascade)? Or should the HAMT implementation be redesigned to make interior nodes also trivially destructible?
- **Why parked**: Amending normative design docs is a project-shaping decision (doc 16 same-commit rule + doc 00 bullet); the choice between amending docs vs. redesigning the HAMT is a human call.

### 2026-07-05 — `map_input_damage` covering requirement: clarification vs. new normative content
- **Source**: closer for `contract.operator_members` (see accompanying commit); flagged by implementer (Decision 5).
- **Question**: The header doc-comment for `map_input_damage` explicitly states the covering (over-approximation) direction — "mapped output damage must cover every affected output pixel; over-approximation is sound, under-approximation is a bug" — as a clarification of what `docs/design/13-effects-as-operators.md:104-107` already entails. If a reviewer reads the comment as introducing new normative content rather than clarifying existing intent, should doc 13 be amended to add the explicit covering wording?
- **Why parked**: Whether the header comment constitutes a doc-13 delta triggering the doc 16 same-commit amend rule is a human judgment call. The implementer and refinement both treat it as a clarification (same move as `snapshot_pins` for `SnapshotToken*`→`StateHandle`); a reviewer may disagree.

### 2026-07-05 — Document→slab-arena rewire to let Housekeeper drive live document memory
- **Source**: closer for `runtime.housekeeping` (see accompanying commit); flagged in refinement Open questions and implementer summary.
- **Question**: The `runtime.housekeeping` task validated its policy against pool fixtures (an `Arena` + `RefStore<T>` + `ReclamationQueue` + workspace `Checkpointer`) rather than the live `runtime::Document`, because `Document` is not yet slab-arena-backed with installed `DeferredReclaimSink`s and a `Checkpointer` (`document.hpp:29-32`, "migrates … when the Editable facet and the slab arenas land"). Should a follow-up task be added to wire the `Housekeeper` into the live `Document`/`Model` arenas once the Editable-facet/arena migration (docs 14/15) is complete?
- **Why parked**: The exact task shape and dependencies are not settled — the trigger is the Editable-facet/arena migration landing, which is a larger ongoing effort. Adding a WBS leaf now would encode unknown prerequisites. Human call once `model.editable_facet` and the slab-arena migration are far enough along to scope the rewire concretely.

### 2026-07-06 — Per-content revision granularity for aggregate-revision fold
- **Source**: closer for `compositor.operator_graph` (see accompanying commit); flagged in refinement Open questions and Decision 3.
- **Question**: The aggregate-revision fold in `operator_graph.cpp` uses the document-global `DocRoot::revision()` as every node's contribution (correct and never stale, but conservative — any edit bumps every operator's cache key). Doc 05:84 envisions per-object revisions so that a static sibling's cached tiles survive an unrelated edit elsewhere. Realizing this optimization requires the model to expose and bump per-content revision numbers (distinct from the document-global counter), with coalescing/undo semantics settled. Should a model-stream task be added to expose per-object revisions so the compositor fold can drop in finer-grained contributions with no compositor change?
- **Why parked**: The selectivity optimization depends on a model-stream design judgment (where per-object revisions live, how coalescing/undo affect them) that is out of the compositor's levelization and not required for correctness. The fold's `contribution` callback seam (`operator_graph.hpp`) already accepts per-node values — the compositor change is zero when model revisions land. Human call once the model stream is ready to scope per-object revisions.

### 2026-07-06 — `set_opacity` added as unplanned seam in `model.damage`
- **Source**: closer for `model.damage` (see accompanying commit); flagged by implementer.
- **Question**: The `model.damage` acceptance criteria require enforcement of `#placement-change-auto-damages` for opacity edits, but no `set_opacity` mutator existed — opacity was creation-only via `add_layer`. The implementer added a minimal `set_opacity` (mirroring `set_transform`, path-copy only, no new sink/invariant/commit machinery) to make the claim enforceable. This seam is not in the refinement's enumerated seam list. Should `set_opacity` be formally enumerated in the model API design docs, and should a downstream task (e.g. `compositor.damage_planning` or a kind-facing adapter) be expected to call it?
- **Why parked**: Adding `set_opacity` was the right call to satisfy the claim, but the broader API surface (who calls it, what kinds drive it, whether the refinement list needs retroactive update) is a human design judgment. The seam itself is already in the tree; this entry tracks whether its documentation and downstream wiring need follow-up.

### 2026-07-06 — Concurrent operator-input pull evaluation (sharded cache)
- **Source**: closer for `compositor.pull_service` (see accompanying commit); Decision 4 and implementer return summary.
- **Question**: `PullServiceImpl` v1 evaluates recursive operator-input pulls synchronously on the frame thread (single-writer cache discipline). Doc 05 / doc 13 do not forbid parallel input evaluation; fanning input pulls onto workers concurrently could improve throughput for wide operator graphs. Should a follow-up task be added to implement concurrent/sharded input evaluation once a concurrent-safe `KeyedStore` variant exists?
- **Why parked**: The optimization requires a concurrent or sharded cache (the single-writer discipline is load-bearing for v1 correctness) and a larger TSan surface. No v1 milestone requires it. The decision of whether and when to build it is profiling-driven and depends on the concurrent-cache design question (already parked 2026-07-05 for `cache.keyed_store`). Human call once profiling evidence and a concurrent cache are available.

### 2026-07-06 — `pull_audio` leaf scoping under the `audio` stream
- **Source**: closer for `compositor.pull_service` (see accompanying commit); Open questions and implementer return summary.
- **Question**: `PullServiceImpl` implements only the visual `pull`; the audio pull (`pull_audio`, doc 13:80, `content.hpp:324-326`) is assigned to the `audio` stream / `arbc::audio-engine` component and joins the `PullService` interface when `contract.audio_facet` fixes `AudioRequest`. Should the `audio` stream's refinement_writer scope a `pull_audio` WBS leaf under `tasks/45-audio.tji` once `contract.audio_facet` lands?
- **Why parked**: The leaf's effort, dependencies, and exact home within the audio stream depend on the `AudioRequest` type that `contract.audio_facet` will define — not yet landed. The interface comment (`content.hpp:324-326`) already assigns ownership to the audio stream. Human call for the audio stream's refinement_writer once `contract.audio_facet` is complete.

### 2026-07-07 — Zero-copy adoption of a non-transient provided surface as a cache value
- **Source**: closer for `surfaces.provided_surfaces` (see accompanying commit); flagged in refinement Open questions and Decisions.
- **Question**: Doc 09 permits caching a provided surface by holding the `SurfaceRef` directly as a `TileValue` (no copy). v1 always copies into a cache-owned surface. Should a follow-up task add zero-copy adoption (holding `SurfaceRef` inside `TileValue`) once a GPU backend makes the copy cost measurable?
- **Why parked**: The optimization requires a `TileValue` variant that owns either a `unique_ptr<Surface>` or a `SurfaceRef`, plus updated byte-accounting and pin logic — a `cache`-layer change whose payoff is real only for GPU textures (a backend not yet built). Profiling-dependent judgment call; no WBS task until the GPU backend exists and a measured copy cost motivates it.

### 2026-07-07 — Cross-tag convert-at-composite for content-provided surfaces
- **Source**: closer for `surfaces.provided_surfaces` (see accompanying commit); flagged in refinement Open questions and Decisions.
- **Question**: Doc 09:102-105 allows a provided surface to carry a differently-tagged format (e.g. sRGB8) and have the backend convert at composite time. v1 requires a working-space tag because the CPU backend stores one format and asserts tag agreement. Should convert-at-composite be added once a backend advertises a second storable format?
- **Why parked**: The conversion extension is gated on a backend that supports multiple storable formats (`color.kernels` / GPU backends) — a capability not yet built. The owning task for that backend should add the conversion at that time. No standalone WBS task needed; this entry is the trigger pointer.

### 2026-07-07 — Multi-edge time_map composition across nested-composition edges
- **Source**: closer for `compositor.temporal_placement_culling` (see accompanying commit); flagged in refinement Open questions / Decision D2.
- **Question**: `compositor.temporal_placement_culling` uses single-edge `TimeMap::evaluate` because the compositor's `for_each_layer` is a flat walk with one time_map per layer. When the compositor gains nested-composition recursion, each nested boundary adds a second (or deeper) time_map edge and the correct evaluation becomes `ComposedTimeMap::compose` over an edge stack (multi-edge fold). Should that fold replace the current `evaluate` call, and should a WBS leaf be registered for it?
- **Why parked**: No nested-composition recursion path exists yet — the spatial+temporal axis is blocked on unbuilt nested-composition rendering. Registering a WBS leaf now would encode unknown prerequisites and the "revisit" anti-pattern. The future nested-composition-rendering task is the natural owner; it will introduce the fold as part of recursing. Human call for the compositor stream's refinement_writer once nested-composition rendering is in scope.

### 2026-07-08 — Sub-block (sample-exact) seek phase for device drain
- **Source**: closer for `audio.seek_drain_realign` (see accompanying commit); Decision D2 and Constraint 5 in the refinement.
- **Question**: Should a follow-up task add sub-block (sample-exact) seek phase to the device drain — i.e. starting the post-seek drain mid-block at the exact sample offset of the new playhead, rather than flooring to the block boundary?
- **Why parked**: The device drain has always been block-granular; making seek sample-exact requires changes to the ring's window model, the block-index compute, and the carry logic — a scope larger than a 1d realignment leaf. It is neither a regression introduced by `audio.seek_drain_realign` nor a behavior any predecessor delivered. The effort and priority are a human judgment call; no agent-implementable prerequisite has landed that makes this the natural next step.

### 2026-07-08 — Device-loss / hot-plug / mid-stream device-format-change resilience
- **Source**: closer for `audio.device_monitor` (see accompanying commit); flagged in implementer return summary and refinement Acceptance criteria ("Registers no successor").
- **Question**: Should a v1-scope leaf be added for resilience to device-loss, hot-plug, and mid-stream device-format-change events (graceful reconnect, format renegotiation)?
- **Why parked**: The refinement explicitly flags this as a human judgment call — the feature "needs the real backend to be meaningful and may be v2 hardening." No simulation path exists for device-loss in a headless environment; meaningful testing requires a real backend and real hardware events. Scope/release-generation decision for a human once the reference backend and the associated integration test infrastructure are mature enough to make the behavior testable.

### 2026-07-08 — Container layout conversion at the export edge
- **Source**: closer for `audio.export_edge_resample` (see accompanying commit); Decision D5 and "Out of scope" section in the refinement.
- **Question**: Should a follow-up task add container layout conversion (e.g. stereo working composition written to a mono container) at the export edge, wiring the device edge's `convert_frames` layout remix into the export path?
- **Why parked**: No milestone currently consumes a container layout differing from the working layout — a WBS leaf would orphan immediately (`scripts/unblocked.py` ORPHANS). The scope, effort, and milestone placement are a human judgment call; the entry is the trigger pointer for when a real milestone consumer is scoped.

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

### 2026-07-09 — Layer-level unknown-field preservation gap vs. doc 08 Principle 4

- **Source**: closer for `serialize.format_tests` (see accompanying commit); flagged in implementer deviation note.
- **Question**: The runtime document reader currently drops unknown sibling fields at the **layer level** (only `params`/content-body unknowns survive the round-trip). Doc 08:88-91 (Principle 4) states unknown fields within a major are "preserved-and-ignored." If layer-level unknown fields are dropped, that may violate the Principle 4 guarantee. Should the loader be fixed to round-trip layer-level unknown sibling fields verbatim, and should `tests/serialize_determinism_corpus.t.cpp` be updated to assert that?
- **Why parked**: Determining whether the current behavior is a violation (or whether Principle 4 scopes only to the params/body level) requires reading doc 08 normative intent against the actual loader code — a human design judgment. If it is a violation, fixing it is a reader change with doc-16 same-commit doc-amendment implications. Implementer pinned only the guaranteed (params/body) level; human call on whether and how to extend the guarantee to the layer level.

### 2026-07-09 — Nested-composition serialization format design (`org.arbc.nested` codec)
- **Source**: closer for `runtime.operator_codecs` (see accompanying commit); Decision 1 in `tasks/refinements/runtime/operator_codecs.md`.
- **Question**: How should v1 persist an `org.arbc.nested` content's child-composition reference? The `.arbc` format today models exactly one unkeyed composition (`"composition"` root, no id/name/URI); model `ObjectId`s are re-allocated on load; and doc 08's `params.ref` is reserved for an **external** project file (doc 05:47-52) which needs a cross-file composition loader `NestedContent` has no field for. The two candidate approaches are: (a) an in-document **`compositions` table** (analogous to `contents`) keyed by stable ordinal, with L4 reader/writer composition recursion and multi-`CompositionRecord` model round-trip — this also settles doc 05 Droste serialization; or (b) **external `params.ref`-plus-loader only** for v1, deferring in-document multi-composition to a later format version. Either choice reshapes doc 05/08 and the L4 serialize stream.
- **Why parked**: The decision is a project-shaping format judgment (doc 00 / doc 16 same-commit rule): it originates new normative content in docs 05 and 08, expands the L4 serialize reader/writer composition recursion, and adds multi-`CompositionRecord` model round-trip — scope well beyond a runtime-codec leaf. A `runtime.operator_codecs` implementer cannot make this call unilaterally. Once the design is chosen, it spawns a concrete implementation leaf under milestone M8; no WBS leaf is registered now (an uncloseable "decide-then-do" task).

### 2026-07-09 — L6 umbrella built-in-kind → Registry bootstrap (doc 17:61) has no WBS home

- **Source**: closer for `runtime.plugin_loading` (see accompanying commit); flagged in implementer return summary as open-question surface (a).
- **Question**: Doc 17:61 assigns "registry bootstrap; built-in kind registration" to the L6 umbrella `arbc` target — but no WBS task currently maps to this work. The current built-in kinds (`org.arbc.solid`, `tone`, `fade`, `crossfade`, `nested`) populate the serialize `CodecTable` / `KindBridge`, not any `Registry`. Should a WBS leaf be added under a `runtime` or top-level `arbc` task to wire built-in kinds into a `Registry` for the L6 umbrella?
- **Why parked**: The scope, dependencies, and milestone placement of a "built-in-kind → Registry bootstrap" leaf depend on whether and when an L6 umbrella `arbc` target is built — a project-architecture decision not yet made. The implementer correctly left this out of scope for the L5 loader. Human call once the L6 umbrella design is settled.

### 2026-07-09 — Confirm no Windows CI runner exists (Decision 4 premise for plugin_loading_win32 deferral)

- **Source**: closer for `runtime.plugin_loading` (see accompanying commit); flagged in implementer return summary as open-question surface (b).
- **Question**: Decision 4 in `tasks/refinements/runtime/plugin_loading.md` defers `runtime.plugin_loading_win32` on the premise that "no Windows CI runner exists today." Is this still true? If a Windows CI runner has been added, the deferral premise is invalid and `runtime.plugin_loading_win32` should be moved to an earlier milestone or prioritized within M9.
- **Why parked**: Confirming CI runner existence is a project-state/infrastructure check that only a human (or the CI admin) can definitively answer. The WBS leaf is registered at M9 and will not block M8 either way; this entry is a trigger to re-evaluate milestone placement if Windows CI lands sooner than M9.
