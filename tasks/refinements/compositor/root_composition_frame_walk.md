# compositor.root_composition_frame_walk — Composition-scoped visual frame walk

## TaskJuggler entry

`tasks/35-compositor.tji:98-103` → `compositor.root_composition_frame_walk`
("Composition-scoped visual frame walk"), a leaf under `task compositor`. It
carries `depends !tile_planning, kinds.nested_runtime_binding,
model.composition_membership` (`35-compositor.tji:101`) and, through the parent
`task compositor`, inherits `depends contract.async_render, cache.key_shapes,
color.resampling` (`35-compositor.tji:7`). It is wired into `m9_release`
(`tasks/99-milestones.tji:72`) and is the milestone note's first-named
correctness gap ("the composition-scoped visual walk (nested double-draw)").
Note line:

> "Give the visual frame walk the composition it is rendering. render_frame
> (compositor.cpp:114) and render_frame_interactive (tile_planning.cpp:260)
> walk the document-global for_each_layer, so a document that nests a child
> composition DRAWS THE CHILD'S LAYERS TWICE: once at top level with
> child-local transforms, once (correctly) through NestedContent::render. Scope
> both to for_each_layer_in(root), as ExportMonitor's audio mix already does
> (mix.cpp:285), letting nested's own render own the child. Settle the two
> nesting representations first: anchored_viewports' walk_composition
> (anchored_viewports.cpp:148) treats layer.content AS a composition ObjectId,
> while NestedContent wraps the child id in a ContentRecord — the cull_walk
> k_root_anchor fallback (anchored_viewports.cpp:147) has the identical
> global-walk bug and must move with it. Drivers must source the root id (no
> driver calls find_first_composition today; see the model root-marker
> question, still parked). Source: tasks/parking-lot.md 2026-07-11
> (root_composition_frame_walk double-draw). Docs 02/05."

## Effort estimate

**3d.** Larger than the 1–2d seam-growth siblings because the fix touches
three compositor walk entry points plus their runtime drivers and settles an
architectural question the walking-skeleton and recursive-composition streams
each deferred to the other, not one arithmetic module. The three coupled
pieces: (1) re-scope the document-global `for_each_layer` at `render_frame`
(`compositor.cpp:114`), `render_frame_interactive` (`tile_planning.cpp:371`),
and `cull_walk`'s `k_root_anchor` branch (`anchored_viewports.cpp:147`) onto the
composition-scoped `DocRoot::for_each_layer_in` (`model.hpp:94`), so a frame
enumerates exactly one composition's direct members; (2) source the root
composition `ObjectId` at each visual driver (`offline.cpp:25`,
`offline_sequence.cpp:146/196`, `host_viewport.cpp:192`) via the existing
lowest-id `DocRoot::find_first_composition` (`model.cpp:493-516`) and pass it in
through `Viewport::anchor` (`compositor.hpp:15-28`), so the compositor stops
re-deriving the root; (3) a byte-exact golden + behavioral-counter guard that
pins "each nested child layer is drawn once." It reuses the accessor
`for_each_layer_in` (already the mechanism inside `walk_composition`,
`anchored_viewports.cpp:43`, and `NestedContent::render`,
`nested_content.cpp:498`) and the `mix_composition` reference shape
(`mix.cpp:262-294`); it builds new only the driver root-sourcing wiring and the
double-draw goldens. No new model or compositor API — `for_each_layer_in` and
`find_first_composition` both already exist.

## Inherited dependencies

**Settled:**

- `compositor.tile_planning` (commit `5d3f6c2`, DONE 2026-07-05) — the direct
  predecessor via `depends !tile_planning`. Delivered `render_frame_interactive`
  (`src/compositor/tile_planning.cpp`), whose per-layer cull/compose/tile walk
  reuses `render_frame`'s front half and, load-bearing here, **inherits the flat
  document-global `for_each_layer`** (`tile_planning.cpp:371`) — one of the two
  driver sites this task re-scopes. Its pure/allocation-free/lock-free planning
  rule (doc 02:123-125) and `LayerTilePlan`
  (`tile_planning.hpp:105-114`) are the contract the scoped walk must not break:
  the change is *which* layers are enumerated, never the per-layer machinery.
- `model.composition_membership` (commit `b038fa6`, DONE 2026-07-05) — landed the
  composition-scoped accessor this task consumes:
  `void DocRoot::for_each_layer_in(ObjectId composition, const
  std::function<void(ObjectId)>& fn) const` (`model.hpp:94`, impl
  `model.cpp:534-545`), visiting a composition's members in true bottom-to-top
  membership order (inline `layers[]` up to `k_max_inline_layers=8`, else the
  `LayerOrderChunk` HAMT spill chain), a no-op when `composition` is absent or
  not a composition. It made `CompositionRecord`'s membership *live* (previously
  never written), so a single-composition document's `for_each_layer_in(root)`
  now yields the full, correctly-ordered member set. Its Deferred-follow-up
  section names this task exactly: "migrating `arbc::compositor` and the
  recursive-composition traversal off the global-order `for_each_layer` onto it
  is the consumers' own already-planned work." It registered **no** new model
  leaf; the migration is here.
- `kinds.nested_runtime_binding` (commit `d443f09`, DONE 2026-07-11) — bound
  `org.arbc.nested` content to its render-time services, so `NestedContent`
  (`nested_content.hpp:57`, child id `d_child`, `nested_content.hpp:284`) now
  actually renders its child in production, walking the child's members with the
  same scoped accessor: `d_doc->for_each_layer_in(d_child, ...)`
  (`nested_content.cpp:498`), each child layer pulled through the injected
  `PullService`. This is the "correct" second draw the double-draw bug pairs
  with the erroneous top-level one — and the reason scoping the top walk (not
  suppressing the child) is the fix: nested's own render owns the child.

**Pending:** none — all three dependencies are `complete 100`.

## What this task is

Give the compositor's visual frame walk the composition it is rendering, so a
frame draws exactly one composition's layers. Concretely:

1. **Re-scope the three walk sites** from the document-global
   `DocRoot::for_each_layer` (all leaves in the document, ascending object-id
   order; `model.cpp:518-532`) onto the composition-scoped
   `DocRoot::for_each_layer_in(root, …)`:
   - `render_frame` (`compositor.cpp:114`) — the offline flat driver.
   - `render_frame_interactive` (`tile_planning.cpp:371`) — the interactive
     tiled driver.
   - `cull_walk`'s `k_root_anchor` branch (`anchored_viewports.cpp:143-149`) —
     the anchored viewport-outward walk's flat fallback, which carries the
     identical global-walk bug.
2. **Source the root composition id at the visual drivers.** Each driver that
   builds a `Viewport` and calls a walk entry point
   (`offline.cpp:25`, `offline_sequence.cpp:146/196`, `host_viewport.cpp:192`)
   sets `Viewport::anchor` to the document's root composition, obtained from the
   existing lowest-id `DocRoot::find_first_composition`
   (`model.cpp:493-516`) — the same rule the serializer
   (`writer.cpp:493`) and `working_space()`/`working_audio_format()` already
   use, and the mechanism `ExportMonitor`/`mix_composition` already thread for
   audio (`export_monitor.cpp:163`, `mix.cpp:262-294`).
3. **Settle the two nesting representations** into one compositing rule (see
   Decision 1): the compositor's frame walk is composition-scoped and
   single-level for compositing; a nested child's layers are the child
   composition's members and are reached only by recursing through the enclosing
   layer's content (doc 05, `NestedContent`). Document-global `for_each_layer`
   is removed from every compositing path.

**Not this task:**

- Persistent cross-frame anchor/rebase state and re-anchor events →
  `compositor.anchored_viewports` (Done; unchanged here — this task only fixes
  its `k_root_anchor` flat-fallback bug, preserving the non-root
  `walk_composition` descent verbatim).
- An explicit root-marker field on `CompositionRecord` → **parked, human call**
  (`tasks/parking-lot.md` 2026-07-11); this task removes the render-path
  motivation for it by threading the id in, exactly as the triage predicted.
- Multi-level structural descent into deeply nested compositions for compositing
  and cycle bounding → `compositor.operator_graph` / `compositor.anchored_viewports`
  (their existing scope; unchanged).

## Why it needs to be done

Doc 05 makes it normative that "a frame therefore renders exactly one
composition's layers … A child composition's layers are never also drawn by the
enclosing walk … drawing them there would both double-draw them and do it with
child-local transforms, outside the nesting layer's embedding. The audio mix
already reads this way (it takes the composition to mix); the visual frame walk
must too" (`docs/design/05-recursive-composition.md:28-36`). Today the visual
walk does not: `for_each_layer` collects **every** layer leaf in the document
HAMT regardless of composition membership, so when a root composition R nests a
child C (via a `NestedContent` layer L), the global walk enumerates C's members
`L1,L2` at top level — composited with child-local transforms against the root
camera — *and* L resolves to `NestedContent`, whose `render` composites `L1,L2`
again, correctly, through the nesting transform. `L1,L2` land twice. Because
source-over is non-idempotent (doc 02:89-130), a translucent child double-blends
— a real pixel bug, not just wasted work. `kinds.nested_runtime_binding` (M8/M9)
made `NestedContent::render` live, so the erroneous second draw is now routine
rather than latent. This is the first-named v0.1 correctness gap on `m9_release`
(`99-milestones.tji:72`).

## Inputs / context

**Governing design-doc sections (normative):**

- `docs/design/05-recursive-composition.md:22-26` — "Rendering a request means:
  run the compositor over the child composition with a synthetic viewport …
  Rendering *is* recursion." Lines `28-36` — the double-draw paragraph (quoted
  above); the governing sentence for this fix.
- `docs/design/02-architecture.md:53-56` — the interactive frame's "Resolve and
  cull. Walk the composition from the viewport's anchor, composing transforms."
  Lines `89-130` — clear-first / disjoint-rects, i.e. why a doubled source-over
  is a correctness defect.
- `docs/design/01-core-concepts.md:91-101` — Viewport anchor = "the composition
  … whose space the camera coordinates live in"; a composition is "the unit of
  recursion."
- `docs/design/08-serialization.md:299-332` (Principle 7) — "A document holds
  exactly one root `composition`"; the root is "encountered first" and holds
  ordinal `"0"`. Confirms a document has a single well-defined root.
- `docs/design/17-internal-components.md:56` — compositor is Level 4,
  `Depends on: contract, cache`; drivers are Level 5 runtime.

**Source seams:**

- `src/compositor/compositor.cpp:103-118` — `render_frame`; global walk at `:114`,
  ignores `viewport.anchor` entirely today.
- `src/compositor/tile_planning.cpp:302` (decl), global walk at `:371` — the
  interactive driver.
- `src/compositor/anchored_viewports.cpp:141-154` — `cull_walk`; `k_root_anchor`
  → global `for_each_layer` at `:147`; non-root → `walk_composition` at `:153`.
  `walk_composition` at `:40-65` (the scoped recursive descent; `find_composition(layer.content)`
  test at `:47`). `k_root_anchor` = `inline constexpr ObjectId k_root_anchor{}`
  (`anchored_viewports.hpp:37`), the zero/invalid `ObjectId`.
- `src/model/arbc/model/model.hpp:86` / `model.cpp:518-532` — global
  `for_each_layer` (yields `const LayerRecord&`, id-order). `model.hpp:94` /
  `model.cpp:534-545` — scoped `for_each_layer_in` (yields member `ObjectId`,
  membership order). `model.hpp:63` / `model.cpp:493-516` —
  `find_first_composition` (lowest-id wins).
- `src/audio_engine/mix.cpp:262-294` — `mix_composition(doc, composition, …)`,
  the reference: an explicit composition `ObjectId` threaded into
  `for_each_layer_in` at `:285`. `src/runtime/export_monitor.cpp:83,163` —
  `ExportMonitor::d_composition` passed straight in.
- `src/kind_nested/nested_content.cpp:425-508` — `NestedContent::render`;
  `find_composition(d_child)` at `:437`, `for_each_layer_in(d_child, …)` at `:498`.
- `src/compositor/arbc/compositor/compositor.hpp:15-28` — `Viewport` with
  `ObjectId anchor{}`; `:32` — `using ContentResolver = std::function<Content*(ObjectId)>`.
- `src/model/arbc/model/records.hpp:60-63` (`ContentRecord{kind,state}`),
  `:68-92` (`LayerRecord`, `ObjectId content`), `:136-144` (`CompositionRecord`,
  membership). `src/base/arbc/base/ids.hpp:11-17` — `ObjectId{value}`,
  `valid() ⇔ value != 0`.
- Drivers: `src/runtime/offline.cpp:25`, `src/runtime/offline_sequence.cpp:146,196`,
  `src/runtime/host_viewport.cpp:192`. None calls `find_first_composition` today;
  all render the global walk with a default (`k_root_anchor`) anchor.
- `src/compositor/arbc/compositor/counters.hpp:34-56` — `CompositorCounters`;
  `composites()` (`:43`) is the double-draw's behavioral surface.

**Predecessor refinement decisions carried forward:**

- `tasks/refinements/compositor/anchored_viewports.md` Decision 1 (anchor is an
  `ObjectId` field on `Viewport`, `k_root_anchor == ObjectId{}`) and Decision 5
  (the walk is depth-agnostic; `render_frame_anchored` shares `render_layer` so
  the root-anchor path was byte-identical). This task supersedes the specific
  "root-anchor ⇒ document-global `for_each_layer`" byte-compat clause, which was
  the bug; it preserves the non-root `walk_composition` descent unchanged.
- `tasks/refinements/model/composition_membership.md` (the scoped accessor is
  the seam consumers adopt) and `tasks/refinements/kinds/nested_runtime_binding.md`
  (nested renders its child via the same accessor).

**Test conventions:** claims register `tests/claims/registry.tsv` +
`// enforces: <claim-id>` (doc 16:14-25); byte-exact CPU goldens in
`tests/<name>_golden.t.cpp` linking `arbc`+`CpuBackend` (doc 16:47-53);
behavioral counters on the caller-owned `CompositorCounters` (doc 16:54-62).

## Constraints / requirements

1. **Levelization (doc 17:56).** The compositor stays Level 4,
   `Depends on: contract, cache`; no new edge. `for_each_layer_in` and
   `find_first_composition` are model symbols the compositor already reaches
   (`walk_composition` uses `for_each_layer_in`); the compositor still must not
   name `kind_nested` — `NestedContent` is reached only through the L3
   `ContentResolver`/`PullService` seam. `find_first_composition` is called from
   L5 drivers, not from the compositor.
2. **Pure per-frame library, caller-owned state.** The walk re-scoping adds no
   allocation, lock, or cross-frame state to the compositor; the root id is a
   plain `ObjectId` read from `Viewport::anchor`. Planning stays pure /
   allocation-free / lock-free (doc 02:123-125), consistent with `tile_planning`.
3. **The compositor never re-derives the root.** It walks
   `for_each_layer_in(viewport.anchor)` and nothing else; sourcing the root id is
   the driver's job (parking-lot 2026-07-11: "the render path take an explicit
   root-composition `ObjectId` rather than re-deriving one"). `k_root_anchor` at
   the compositor means "no composition bound" → `for_each_layer_in` returns
   immediately (`find_composition(ObjectId{})` is null) → an empty walk.
4. **A single-composition scene stays byte-identical.** For a document whose
   every layer is a member of its one composition, `for_each_layer_in(root)`
   yields the same layer set as the old global `for_each_layer`, in membership
   (z) order. The existing `render_frame`/`tile_planning`/`anchored_viewports`
   goldens must remain byte-exact once their fixtures set `anchor` to the root
   composition and build full membership (both now guaranteed by
   `model.composition_membership`).
5. **No TSan obligation.** This is a pure per-frame walk change, not a
   concurrency-touching task; no stress/TSan coverage is scoped (consistent with
   the sibling compositor refinements).
6. **≥90% diff coverage** on changed lines (doc 16), including the driver
   root-sourcing lines and the scoped-walk branches.

## Acceptance criteria

1. **Claims-register entry (new).** Register
   `05-recursive-composition#frame-renders-one-compositions-layers` in
   `tests/claims/registry.tsv`, enforced by a byte-exact golden (below) tagged
   `// enforces: 05-recursive-composition#frame-renders-one-compositions-layers`.
   The claim: a frame renders exactly one composition's layers; a nested child's
   layers are drawn once, through the nesting layer's embedding.
2. **Byte-exact double-draw golden** (`tests/root_composition_frame_walk_golden.t.cpp`,
   links `arbc`+`CpuBackend`). Build a document: root composition R with (a) one
   direct opaque layer and (b) a `NestedContent` layer L embedding child C, where
   C holds two *translucent* member layers. Render via `render_frame_interactive`
   and assert the output is `memcmp`-identical to a single-pass oracle that
   composites R's direct layer plus C's two layers once each, through L's
   transform. Before the fix this differs (child layers double-blended at
   child-local placement); after, it matches. Also assert the same document under
   the offline `render_frame` path matches its oracle.
3. **Behavioral-counter assertion (double-draw is work, too).** On the same
   nested document, assert `CompositorCounters::composites()`
   (`counters.hpp:43`) equals the single-pass composite count (R's direct layer
   + C's two child layers, each once), **not** the doubled count. This is the
   performance-shaped proof that the extra draws are gone, per doc 16:54-62 — an
   assertion on the counter value, never wall-clock.
4. **Byte-exact regression guard (single-composition unchanged).** The existing
   `render_frame` / `render_frame_interactive` / `anchored_viewports` goldens
   re-run unchanged and stay byte-exact once their fixtures set `Viewport::anchor`
   to the root composition (Constraint 4). No new golden for the flat path; the
   guard is that these still pass.
5. **Driver root-sourcing.** A test drives each updated visual driver
   (`offline`, `offline_sequence`, `host_viewport`) over a two-composition
   document and asserts it renders the lowest-id (root) composition's members and
   not the second composition's — pinning that the drivers source the root via
   `find_first_composition` and pass it as `anchor`.
6. **No new WBS leaf.** The scoped accessor and the root-sourcing rule both use
   pre-existing model symbols; the only deferred item — an explicit
   `CompositionRecord` root-marker field — is a record-layout / doc-14+doc-15
   human judgment call already parked (`tasks/parking-lot.md` 2026-07-11), not an
   agent-implementable leaf. Nothing is registered in the WBS by this task.
7. **Component wiring & CI dependency check.** `scripts/check_levelization`
   (or the doc-17 CI check) stays green — no new compositor out-edge.
8. **Gate green.** Build + full test suite pass before commit (global rule);
   ≥90% diff coverage on the changed lines.

## Decisions

1. **The compositor walk is composition-scoped and single-level for
   compositing; nesting is reached only through content recursion (doc 05,
   Representation B).** The "two nesting representations" the note flags are
   *mutually exclusive per layer*: a layer's `content` is **either** a raw
   composition `ObjectId` (the `anchored_viewports` walking-skeleton form, which
   `walk_composition` descends via `find_composition(layer.content)`,
   `anchored_viewports.cpp:47`) **or** a `ContentRecord` of kind `org.arbc.nested`
   bound to a `NestedContent` (the doc-05 production form, where
   `find_composition(layer.content)` returns null because the content is a
   `ContentRecord`). Neither double-draws *once the top walk is scoped*: the
   scoped walk yields only the enclosing composition's direct members, so a
   child's members never leak into it; a `NestedContent` layer is visited as a
   leaf and its child drawn once by content recursion, while a
   composition-id-as-content layer is descended once structurally. The
   settlement is therefore: **remove document-global `for_each_layer` from every
   compositing path; the child is owned by the enclosing layer's content.**
   *Rationale:* this is doc 05:28-36 verbatim ("reached only by recursing through
   that layer's content"), and it keeps the L4/L3 boundary clean (the compositor
   never dereferences a nested child itself in the production path).
   *Rejected:* (a) suppress the child's members inside the child's own render —
   inverts ownership, contradicts doc 05, and cannot fix `render_frame` which has
   no child context; (b) make `render_frame` structurally descend all nesting
   like `walk_composition` — would double-draw doc-05 `NestedContent` (content
   *and* structural descent both composite the child) and drag composition
   knowledge into the flat compositor.
2. **Carry the root composition id in `Viewport::anchor`; the driver sources it
   via `find_first_composition`.** `anchor` already means "the composition whose
   space the camera lives in" (`compositor.hpp:19-27`); for a flat render that is
   the root composition. Drivers set it from the existing lowest-id
   `find_first_composition` (`model.cpp:493-516`).
   *Rationale:* zero new API; mirrors the audio path exactly
   (`mix_composition`(comp) ← `ExportMonitor::d_composition`, `mix.cpp:262`,
   `export_monitor.cpp:163`) — the "audio mix already reads this way" doc 05
   points at; and it honors the parking-lot triage's explicit direction that the
   render path *take* an explicit id rather than re-derive one, removing the
   render-path motivation for a record-layout marker. *Rejected:* (a) the
   compositor internally resolving `k_root_anchor → find_first_composition` —
   less migration but re-introduces the "compositor re-derives the root" coupling
   the triage rejected, and bakes lowest-id-wins into L4; (b) a new `root`
   parameter separate from `anchor` — redundant, since `anchor` already denotes
   the composition to walk.
3. **`k_root_anchor` (`ObjectId{}`) at the compositor means "no composition
   bound" → empty walk; drivers must set a real anchor.** After this task no
   production driver passes the sentinel for a real render; `for_each_layer_in`
   already no-ops on an invalid/absent composition, so the sentinel degrades
   safely to drawing nothing rather than silently reviving the global walk.
   *Rationale:* removes the buggy "sentinel ⇒ global walk" clause at its root
   and makes the invalid-id case behave like every other invalid `ObjectId`.
   *Rejected:* keeping the global-walk fallback for back-compat — that *is* the
   bug the task exists to kill.
4. **Preserve `walk_composition`'s non-root descent verbatim; touch only
   `cull_walk`'s `k_root_anchor` branch.** The `anchor != k_root_anchor` path in
   `cull_walk` (`anchored_viewports.cpp:153`) and the recursive `walk_composition`
   are already composition-scoped and correct; only the flat fallback at `:147`
   is re-pointed. *Rationale:* minimal, surgical change to a Done task's surface;
   keeps deep-zoom/rebase behavior byte-identical. *Rejected:* rewriting
   `cull_walk` to always route through `walk_composition(root)` — larger blast
   radius on `anchored_viewports` for no behavioral gain on the flat path.

**No design-doc delta.** The behavior is already normative in doc 05:28-36
("the visual frame walk must too"); this task concretizes it into C++. The
root-sourcing-by-lowest-id rule is a pre-existing code convention
(`find_first_composition`), not a new architectural seam, and the record-layout
root-marker question stays parked (human call) rather than being decided here.

## Open questions

(none — all decided.) The explicit `CompositionRecord` root-marker remains
parked as a human judgment call (`tasks/parking-lot.md` 2026-07-11); this task
deliberately removes the render-path pressure for it rather than resolving it,
consistent with the triage note.

## Status

**Done** — 2026-07-15.

- Re-scoped `render_frame` (`src/compositor/compositor.cpp`) and `render_frame_interactive` (`src/compositor/tile_planning.cpp`) from document-global `DocRoot::for_each_layer` to `DocRoot::for_each_layer_in(viewport.anchor)`.
- Fixed `cull_walk`'s `k_root_anchor` branch (`src/compositor/anchored_viewports.cpp`) to use the composition-scoped `for_each_layer_in`, eliminating the identical global-walk bug; updated `anchored_viewports.hpp` accordingly.
- Updated all visual drivers (`src/runtime/offline.cpp`, `src/runtime/offline_sequence.cpp`, `src/runtime/host_viewport.cpp`, `src/runtime/interactive.cpp`) to source the root composition via `find_first_composition` and pass it as `Viewport::anchor`.
- Fixed pre-attach ordering in `src/runtime/pull_identity.cpp`: descends the nesting boundary via `composition_ref()` in `build_pull_identity_map` and admits composition-naming contents into `d_operator_layers` even when `is_operator` is momentarily false pre-attach, so nested children's leaves receive pull identity and async arrivals route correctly.
- Added byte-exact double-draw golden (`tests/root_composition_frame_walk_golden.t.cpp`, wired in `tests/CMakeLists.txt`) covering `render_frame` and `render_frame_interactive` paths; registered claim `05-recursive-composition#frame-renders-one-compositions-layers` in `tests/claims/registry.tsv`.
- Updated existing goldens and counter assertions across compositor and runtime test suites to set `Viewport::anchor` to the root composition; adjusted two runtime `renders_coalesced()` assertions from `> 0` to `== 0` (the prior double-draw had been masking the correct count).
- Removed debug `fprintf` from `src/runtime/bench/interactive_bench_workloads.hpp`.
