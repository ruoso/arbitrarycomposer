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

### 2026-07-06 — Concurrent operator-input pull evaluation (sharded cache)
- **Source**: closer for `compositor.pull_service` (see accompanying commit); Decision 4 and implementer return summary.
- **Question**: `PullServiceImpl` v1 evaluates recursive operator-input pulls synchronously on the frame thread (single-writer cache discipline). Doc 05 / doc 13 do not forbid parallel input evaluation; fanning input pulls onto workers concurrently could improve throughput for wide operator graphs. Should a follow-up task be added to implement concurrent/sharded input evaluation once a concurrent-safe `KeyedStore` variant exists?
- **Why parked**: The optimization requires a concurrent or sharded cache (the single-writer discipline is load-bearing for v1 correctness) and a larger TSan surface. No v1 milestone requires it. The decision of whether and when to build it is profiling-driven and depends on the concurrent-cache design question (already parked 2026-07-05 for `cache.keyed_store`). Human call once profiling evidence and a concurrent cache are available.
- **Updated** (2026-07-12, triage): **premise sharpened, and this entry ABSORBS the former "KeyedStore concurrency hardening for async worker-fill" entry (parked 2026-07-05), which was deleted this round as resolved.** That entry asked whether `KeyedStore` should be hardened for concurrent access once the async off-thread fill path landed. The path has now landed and a non-zero worker count ships (`runtime.worker_dispatch_leaf_only` → `runtime.interactive_worker_count_default`), and **the answer was no**: rather than harden the cache, the architecture CONSTRAINS DISPATCH so the render-thread confinement survives. Leaf contents compute on workers (a leaf render pulls nothing, so it touches no cache); operator contents render inline on the driver thread, which stays the sole cache writer. That is now a stated architectural principle (doc 00:203, doc 02:166 "Worker dispatch is leaf-only"), a registered claim with an enforcing TSan-clean test over all three operator kinds on both drivers (`02-architecture#worker-dispatch-is-leaf-only`, registry.tsv:150), and a gate lint keeping `worker_backed_dispatch` the only submission site (`scripts/check_worker_dispatch.py`). The claim states the reason outright: an operator's render re-enters the `PullService`, "whose cache probe/insert and descent-depth accounting are render-thread-confined". So a concurrent/sharded `KeyedStore` is not an open question in its own right — **it is merely the enabling primitive for THIS entry's question, and has no other consumer.** The two were always one decision; they are now one entry. That makes the bar here strictly higher than when this was written: the question is no longer "should we add a concurrent path?" but "**should we lift a rule the codebase now actively defends in a doc, a claim, a test, and a lint?**" And the throughput case is weaker, too — worker parallelism now ships for the leaf renders that dominate real scenes, so the residual win is confined to *wide operator graphs*, which the v0.1 image-editor scope (2026-07-12 audit) does not produce. Revisit only with profiling evidence from a genuinely operator-heavy workload; the cost is a concurrent cache, a larger TSan surface, and unwinding four enforcement points. Still parked.

### 2026-07-07 — Zero-copy adoption of a non-transient provided surface as a cache value
- **Source**: closer for `surfaces.provided_surfaces` (see accompanying commit); flagged in refinement Open questions and Decisions.
- **Question**: Doc 09 permits caching a provided surface by holding the `SurfaceRef` directly as a `TileValue` (no copy). v1 always copies into a cache-owned surface. Should a follow-up task add zero-copy adoption (holding `SurfaceRef` inside `TileValue`) once a GPU backend makes the copy cost measurable?
- **Why parked**: The optimization requires a `TileValue` variant that owns either a `unique_ptr<Surface>` or a `SurfaceRef`, plus updated byte-accounting and pin logic — a `cache`-layer change whose payoff is real only for GPU textures (a backend not yet built). Profiling-dependent judgment call; no WBS task until the GPU backend exists and a measured copy cost motivates it.
- **Updated** (2026-07-12, triage): **premise corrected — this is no longer GPU-gated.** The entry assumes the copy is only measurable for GPU textures, so it parks behind a backend that does not exist. `kinds.image` (M9) breaks that assumption: `org.arbc.image` is a new **CPU** producer of large *non-transient* provided surfaces, and the v0.1 image-editor scope points it at exactly the worst case — a full-resolution imported photograph. Following imageseq's shape (`imageseq_content.cpp:158-170`), the kind decodes a whole frame into a plugin-owned working-space surface and hands it over as a provided surface, which the cache then COPIES. At 24 MP and `rgba32f` that is a ~384 MB copy per cache insert, on the CPU, with no GPU anywhere in sight. So the trigger this entry is waiting for ("a backend makes the copy cost measurable") has effectively fired on the CPU path instead. **Still parked, because the right first move is probably not this optimization**: before adding a `TileValue` variant and reworking byte-accounting and pin logic, check whether `org.arbc.image` should return only the *requested region* at the requested scale rather than a whole decoded frame — which is what the pull contract already asks for, and would shrink the copy instead of eliminating it. Re-triage once `kinds.image` lands and its actual provided-surface granularity is known; if it does hand over whole frames, this entry should become a leaf.
- **Updated** (2026-07-14, closer for `kinds.image`): **The trigger condition ("if it does hand over whole frames") did NOT fire — Decision 1 resolved it.** `org.arbc.image` (Decision 1, `tasks/refinements/kinds/image.md`) returns a provided surface sized to the *requested region at the achieved scale*, not a whole decoded frame. Every cache copy is tile-sized, not image-sized; the 384 MB-per-insert concern never arises on the CPU path. This entry does NOT become a leaf for the CPU image kind. The GPU-backend question (a `TileValue` variant holding a `SurfaceRef` for zero-copy GPU texture hand-off) remains open, gated on a GPU backend that does not yet exist.

### 2026-07-11 — Thread-per-Document vs. process-wide housekeeping thread for multi-document hosts
- **Source**: closer for `runtime.housekeeping_document_wiring`; surfaced in refinement Decision 5 ("Surfaced for the parking lot").
- **Question**: `Document` now owns one `HousekeepingThread` per instance, which parks on a condition variable and wakes every 50 ms. A host that opens dozens of documents concurrently spawns a proportional thread pool of idle waiters. Should a process-wide housekeeping thread servicing N registered `HousekeepingTarget`s be added instead — or as an opt-in mode?
- **Why parked**: No such multi-document host exists today and the target seam introduced by `housekeeping_document_wiring` (Decision 1: `HousekeepingTarget`) is exactly the abstraction a shared-thread would need, so the migration stays cheap if it is ever wanted. The decision depends on host-profiling evidence (thread count / wake-up overhead at scale) that does not exist yet. Human call once a real multi-document host is built and the overhead is measurable.
- **Updated** (2026-07-16, triage): considered against the `runtime.shared_worker_pool` precedent (landed since — the analogous shared-resource change for `WorkerPool`, wired on structural grounds without profiling) and deliberately kept parked: K viewports over one document is a real v0.1 shape, but a host opening dozens of documents exists nowhere, even in design. The `HousekeepingTarget` seam keeps the migration cheap whenever one appears.

### 2026-07-14 — Cross-process concurrent-editor coordination for GC (host policy)
- **Source**: closer for `serialize.asset_gc` (commit serialize.asset_gc); Decision 5 in `tasks/refinements/serialize/asset_gc.md`.
- **Question**: Doc 08 names a concurrent editor as a reason a save cannot delete blobs. `serialize.asset_gc` addresses the in-process case via the caller-completeness contract (Constraint 5: the caller must name every open document in the root set) and the write-if-absent repair property (Decision 6: a re-saved live tile is re-written). The cross-process case — two editors sharing one `.assets/` directory, one running GC while the other is actively painting — is entirely the host's responsibility. Should the library provide a file-lock or coordination primitive (e.g. a `.assets/.gc-lock`) to let the host gate GC safely?
- **Why parked**: A coordination primitive (lock file, advisory lock, lease) is a host policy question, not a library contract. The host is the only party that knows which editor processes share a project directory and when a "clean up" is safe to permit. Adding a lock to the library would both over-specify how hosts coordinate and under-specify what "coordinated" means across OS boundaries. This is explicitly recorded as Decision 5 in the refinement and flagged as a human judgment call. Human call once a real multi-editor / multi-process host scenario is scoped.

### 2026-07-14 — User-facing "clean up" UI/CLI affordance for asset GC (host/packaging scope)
- **Source**: closer for `serialize.asset_gc` (commit serialize.asset_gc); §"No deferred WBS follow-ups" in `tasks/refinements/serialize/asset_gc.md`.
- **Question**: `gc_project_directory` is the library entry point for the mark-and-sweep GC. Nothing in libarbc, arbc-testing, or any current plugin exposes it to a user (no CLI flag, no menu item, no keyboard shortcut). Should a follow-up WBS leaf add a user-facing "clean up" affordance — either a CLI subcommand in an `arbc` tool or a hook in the reference UI?
- **Why parked**: The library deliverable is complete; exposing it to end users is host/packaging surface whose shape depends on what host is being built. The v0.1 image-editor scope (2026-07-12 audit) does not specify a CLI tool or a reference UI, so there is no current milestone to wire a leaf to. Human call when the host or packaging design is scoped enough to say what "clean up" means to a user in that context.

### 2026-07-15 — Fetch-on-workers for slow/remote AssetSource during parallel raster load
- **Source**: closer for `serialize.tile_store_parallel_load` (commit serialize.tile_store_parallel_load); Decision 2 in the refinement and implementer return summary.
- **Question**: The parallel load keeps `LoadContext::resolve` + `AssetSource::request` on the loading thread (Decision 2: `LoadContext` is single-writer, `FilesystemAssetSource` bumps non-atomic witness counters). If a slow or remote `AssetSource` ever exists, moving the byte-read onto workers — overlapping I/O latency with the previous tile's decode — would be a throughput win. Should a concurrent-read `AssetSource` contract be defined and a fetch-on-workers path added to `TileDecodeDispatch`?
- **Why parked**: No slow/remote `AssetSource` exists; the only extant backing (`FilesystemAssetSource`) is fast and OS-page-cached, so the I/O overlap buys nothing today. A concurrent-read source requires a wider `AssetSource` contract (API change), atomic or per-thread counters, and a new TSan surface on the fetch path — none of which is decidable without a concrete slow source to profile against. The implementer explicitly deferred this ("not decidable today — no such source exists"). Human call once a slow/remote `AssetSource` is proposed or profiling on a network-backed source demonstrates the I/O latency dominates.

### 2026-07-15 — WorkerPool QoS / priority split between the render lane and the save lane
- **Source**: closer for `serialize.tile_store_parallel_save` (commit serialize.tile_store_parallel_save); §"Tech-debt follow-up proposed" in the implementer return summary.
- **Question**: The bounded in-flight window (Constraint 4 — at most O(`worker_count`) encode jobs outstanding) already caps an autosave's interference with interactive rendering to O(`worker_count`) queued jobs. Should a priority or QoS split be added to `WorkerPool` so that save-lane jobs are preemptable or deprioritized relative to interactive render jobs — guaranteeing that autosave never delays a render frame by more than the bounded window, even when the pool is saturated?
- **Why parked**: The bounded in-flight cap already limits interference; a measured priority need would require profiling evidence of a real delay on a real workload with both autosave and interactive rendering active simultaneously. Adding a QoS split now would be a speculative optimization over a surface (the generic work lane) that has no profiling history yet. The implementer explicitly flagged this as parking-lot only ("not a decidable unit today"). Human call once profiling of the shared pool under concurrent autosave + interactive rendering reveals a frame-delay problem that the bounded window alone cannot solve.

### 2026-07-16 — CPS consumer round-trip: gate CI on a cps-config reader once tooling stabilizes
- **Source**: closer for `packaging.install` (commit packaging.install); D4 in `tasks/refinements/packaging/install.md` and "Open questions" section.
- **Question**: `arbc.cps` is installed and validated as well-formed JSON with the expected shape, but no CI lane runs a `cps-config`-style consumer round-trip (D4: "as it becomes consumable by tooling"). Once `cps-config` or an equivalent CPS reader stabilizes and is available in the CI environment, should the consumer test be extended to probe `arbc.cps` via that tool and assert a successful compile+link, matching what `pkgconfig_probe` does for `arbc.pc`?
- **Why parked**: Doc 10:43-46 explicitly keeps CPS aspirational ("as it becomes consumable by tooling") and `cps-config` is still evolving. Adding a dependency on an unstable tool would introduce CI flakiness for a promise the doc already scopes as metadata-only for now. Human call once a stable, commonly-packaged CPS reader exists.

### 2026-07-16 — Shared libarbc on a system with only non-PIC libzstd_static (no shared sibling)
- **Source**: closer for `packaging.shared_library_zstd_shared_link` (see accompanying commit); §"Informational (parking lot)" in the implementer return summary and §"No deferred WBS leaves" in `tasks/refinements/packaging/shared_library_zstd_shared_link.md`.
- **Question**: After D1, the `arbc_zstd` shim under `BUILD_SHARED_LIBS=ON` prefers `zstd::libzstd_shared` → `zstd::libzstd` → fetched-PIC `libzstd_static` → `zstd::libzstd_static` (last resort). A system that exposes only `zstd::libzstd_static` (no shared `libzstd.so`) with a non-PIC archive falls to that last resort — which links correctly if the archive is PIC (unlikely for distro-packaged static zstd) but SIGSEGVs otherwise. The escape hatches are `FETCHCONTENT_TRY_FIND_PACKAGE_MODE=NEVER` (forces the PIC fetched fallback) or installing the shared `libzstd` package. Should this case be made a harder error, or should a detection + diagnostic be added?
- **Why parked**: The escape hatches are documented in the CMake shim comment; the case is a packaging error (the packager shipped only a non-PIC static), and the right fix is to install `libzstd-dev` (shared) rather than add a CMake detection path. The refinement calls this a "defensible make-the-call resolution" and explicitly defers it to the parking lot as informational, not a WBS leaf. Human call if a concrete distribution is reported where `FETCHCONTENT_TRY_FIND_PACKAGE_MODE=NEVER` is insufficient and a diagnostic is wanted.

