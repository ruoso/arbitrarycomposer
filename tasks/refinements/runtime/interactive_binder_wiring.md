# runtime.interactive_binder_wiring — Wire `bind_operators` into the interactive frame path

## TaskJuggler entry

[`tasks/65-runtime.tji:139-144`](../../65-runtime.tji), milestone `m9_release`
([`tasks/99-milestones.tji:72`](../../99-milestones.tji)).

```
task interactive_binder_wiring "Wire bind_operators into the interactive frame path" {
  effort 1d
  allocate team
  depends !interactive_pull_wiring, kinds.nested_runtime_binding
  note "Call bind_operators per interactive frame on the interactive path
        (host_viewport.cpp pins a DocStatePtr but never calls bind_operators, and
        interactive.cpp passes null pulls), so fade, crossfade, and nested all render
        attached interactively. Blocked on interactive_pull_wiring supplying the
        PullService. Source-of-debt: tasks/refinements/kinds/nested_runtime_binding.md.
        Docs 02/13/17."
}
```

The note's parenthetical is now half-stale: `runtime.interactive_pull_wiring`
landed on 2026-07-11 and `interactive.cpp:239` builds a real `PullServiceImpl`.
The `bind_operators` half stands exactly as written, and is the whole of this
task.

## Effort estimate

**1d.** The production edit is small — a trailing defaulted parameter on
`InteractiveRenderer::render_frame`, a `const Document*` member on
`HostViewport`, one `register_builtin_operator_binders()` call, and one
`OperatorBindingScope` local, roughly thirty lines. The day is the tests: two
byte-exact goldens against the offline oracle, three behavioral counters (one of
which pins a shipped claim that this task could silently break), a TSan case, and
the claims-register work.

## Inherited dependencies

**Settled:**

- **`runtime.interactive_pull_wiring`** (done 2026-07-11) — built the frame-local
  `PullServiceImpl` this task binds against: `PullConfig` at
  `src/runtime/interactive.cpp:233-238`, service at `:239`, passed to
  `render_frame_interactive` at `:254-256`. It also memoized the identity map on
  revision (`refresh_identity_memo`, `interactive.cpp:43-45`, called at `:170-172`
  and `:232`) and added the inverse `ObjectId → const Content*` map and the
  operator-layer set that `route_operator_damage` needs. Its Constraint 5 named
  this task by name and deliberately left the binder alone.
- **`kinds.nested_runtime_binding`** (done 2026-07-11) — productionized
  `bind_operators` itself: the `BindContext` seam
  (`src/runtime/arbc/runtime/operator_binding.hpp:57-62`), the pin-owning
  `OperatorBindingScope` (`:97-128`), the `DocStatePtr`-taking signature (`:150-151`),
  `NestedContent::detach()`, and `src/runtime/binder_nested.cpp`. Its
  out-of-scope section minted this leaf.
- **`runtime.host_viewport_document_binding`** (done 2026-07-11) — added the
  `Document&` constructor (`src/runtime/host_viewport.cpp:70-76`) and the
  `HostViewportDocumentAccess` attorney (`src/runtime/document_access.hpp:20-22`).
  It is the constructor this task hangs the binding off, and it stated in advance
  that when the binder lands "the interactive path inherits the pixels with no
  further work here, because it is the same settle and the same damage."
- **`runtime.pull_identity_disjoint_ids`** (done 2026-07-11) — synthesized pull
  identities now come from the reserved half of the id space
  (`synthetic_id`, `src/base/arbc/base/ids.hpp`), so nothing this task binds can
  mint an id that collides with a model object.

**Pending (deliberately downstream — this task must not pre-empt them):**

- **`runtime.worker_dispatch_leaf_only`** (`tasks/65-runtime.tji:145-150`) — owns
  the leaf-only worker-dispatch invariant and its TSan coverage. This task must
  not introduce a pool-submitting `RenderDispatch` on the interactive path; see
  Constraint 6.

## What this task is

Call `bind_operators` once per interactive frame that actually renders, from
inside `InteractiveRenderer::render_frame`, against the frame-local
`PullServiceImpl` that `runtime.interactive_pull_wiring` already built. That
requires plumbing the two things `bind_operators` needs and `render_frame` does
not currently receive — the `const Document&` and the `DocStatePtr` pin — from
`HostViewport::step()`, which holds the pin (`host_viewport.cpp:121`) and, since
`runtime.host_viewport_document_binding`, is constructible against a `Document`.

## Not this task

Changing how the interactive driver dispatches renders (it stays inline —
`direct_dispatch()`, `interactive.cpp:239`), touching the identity memo's build
order, hoisting the `PullService` into `HostViewport`
(`runtime.interactive_pull_wiring` Decision 1 rejected that), or making the
`Model&`-constructed `HostViewport` bind anything.

## Why it needs to be done

1. **Non-endpoint operator layers assert or blank interactively.** A fade at
   `envelope == 0.5` or a crossfade at `w == 0.5` is not an identity endpoint, so
   the driver does not serve an input's tiles directly — the operator's own
   `render()` runs, and it immediately trips
   `assert(d_pull != nullptr && ... "rendered before attach")`
   (`src/kind_fade/fade_content.cpp:94`). In a release build it composites
   nothing. `runtime.interactive_pull_wiring` fixed only the *endpoint* case
   (`w == 0` / `w == 1` / `envelope == 1`), which needs no attach because the
   driver, not the operator, issues the pull; its tests are confined to endpoints
   for exactly this reason.
2. **Nested never renders interactively at all.** `NestedContent::inputs()` is a
   memo projected from the child composition and is empty until attach
   (`src/kind_nested/nested_content.cpp:110-136, 161-171`), and `render()` composes
   the child by pulling each child layer through `d_pull`
   (`nested_content.cpp:390-391`). Unbound, there is no `d_pull`. A nested scene
   that exports correctly through `SequenceRenderer` shows nothing in a viewport.
3. **A shipped claim already asserts this works.** Registry row 246
   (`08-serialization#nesting-inputs-are-derived-not-persisted`) states that
   `NestedContent::inputs()` "is non-empty exactly when `bind_operators` has
   attached it — **the state every rendered frame of an interactive session** and
   every frame of an offline export leaves it in." The interactive half of that
   sentence is presently unbacked by any code.
4. **Doc 01 forbids the workaround.** Binding a viewport to a document is "the
   host's single wiring step" (doc 01:112-120), so a host may not be told to call
   `bind_operators` itself. If the runtime does not bind, nobody legally can.

## Inputs / context

### Design docs (normative)

- **doc 13:69-71** — *"**Pulling inputs goes through the core.** Operators do not
  call `input->render()` directly … At attach, content receives a `PullService`."*
  Attach is the only legal way an operator acquires its pull service;
  `bind_operators` is that attach.
- **doc 13 § "Pulling inputs goes through the core"** — amended by this task (see
  Decision 7): *"**Binding is the render driver's obligation, and every driver
  discharges it.** … The interactive frame loop therefore binds per frame against
  its frame-local pull service exactly as the offline and export drivers do, and
  an operator renders the same under a deadline as it does in an exact export."*
- **doc 13:145-158** — *"deadlines and cache budgets flow through pulls exactly as
  they flow through nesting … a pull configured without an async reap sink must be
  given a synchronous dispatch."* Deadlines are an interactive-only concept; the
  sentence is a statement about the path this task lights up, and its second half
  is what Constraint 6 preserves.
- **doc 02:40-41** — *"**Renderers**: two drivers over the same core. Interactive
  owns a frame loop, deadlines, and progressive refinement. Offline owns exact
  evaluation."* Two drivers, one core — not one operator-capable driver and one
  operator-blind one.
- **doc 02:135-137** — *"This is the *model*; v1 may degenerate to 'everything on
  one thread' while keeping the request/completion structure."* The normative
  licence for the interactive path's inline-only dispatch, which is what makes
  Constraint 6's answer "no new concurrency surface" correct rather than a dodge.
- **doc 01 § Viewport** — amended by this task (Decision 7): the seams the document
  supplies now include *"the content graph each frame binds to give its operators
  their render-time services (doc 13)"*, and the host *"never attaches an operator
  by hand."*
- **doc 17:60** — `arbc::runtime` is level 5 and DEPENDS "everything below."
  `operator_binding.cpp`, `interactive.cpp` and `host_viewport.cpp` are already
  sources of the same `runtime` component
  (`src/runtime/CMakeLists.txt:7,10`), and `kind_fade`/`kind_crossfade`/`kind_nested`
  are already in its DEPENDS closure (`:30-31`). **This task adds no levelization
  edge.**

### Source seams

| What | Where |
| --- | --- |
| `bind_operators(const Document&, PullService&, Backend&, DocStatePtr)` | `src/runtime/arbc/runtime/operator_binding.hpp:150-151` |
| `BindContext { PullService&; Backend&; const ContentResolver&; const DocRoot&; }` | `operator_binding.hpp:57-62` |
| `OperatorBindingScope` — movable, owns the pin, detaches in reverse order on release | `operator_binding.hpp:97-128`, `operator_binding.cpp:79-95` |
| The bind walk: attach, *then* recurse into `inputs()` | `src/runtime/operator_binding.cpp:99-135` |
| `register_builtin_operator_binders()` — thread-safe, idempotent | `operator_binding.hpp:87`, `operator_binding.cpp:49-60` |
| `InteractiveRenderer::render_frame` signature (no `Document`, no pin) | `src/runtime/arbc/runtime/interactive.hpp:138-142` |
| Frame-local `PullServiceImpl` — the thing to bind against | `src/runtime/interactive.cpp:233-239` |
| The still-scene early-out (returns *before* the pull service exists) | `interactive.cpp:208-212` |
| `render_frame_interactive` call | `interactive.cpp:254-256` |
| `HostViewport::step()` — the pin at `:121`, `render_frame` call at `:156-158` | `src/runtime/host_viewport.cpp:86-167` |
| `HostViewport` members — `Model& d_model`, **no `Document`** | `src/runtime/arbc/runtime/host_viewport.hpp:161-164, 256-289` |
| `Document::pin()` / `resolve()` / `for_each_content()` | `src/runtime/arbc/runtime/document.hpp:233, 234, 240` |
| Offline precedent: per-frame bind, scope declared *after* the pull service | `src/runtime/offline_sequence.cpp:120-127, 172-177` |
| Export precedent: one long-lived scope over one frozen pin | `src/runtime/export_monitor.cpp:138-139` |
| Fade's unbound assert | `src/kind_fade/fade_content.cpp:94` |
| Nested's memo: empty until attach; `render()` pulls each child layer | `src/kind_nested/nested_content.cpp:110-136, 161-171, 390-391` |
| Stale comments this task retires | `tests/interactive_operator_identity_golden.t.cpp:21`, `tests/async_external_load_golden.t.cpp:11`, `tests/interactive_model_damage_routing.t.cpp:120` |

### Predecessor decisions this task inherits verbatim

- **`interactive_pull_wiring` Constraint 2** — the `PullServiceImpl` must be
  frame-local: it borrows `TileCache&` and `Backend&`, which are *parameters* of
  `render_frame`, not members. This is what forces Constraint 2 below.
- **`interactive_pull_wiring` Decision 3** — interactive dispatch is
  `direct_dispatch()`, byte-for-byte the driver's pre-wiring inline fill. Worker
  dispatch belongs to `runtime.worker_dispatch_leaf_only`.
- **`interactive_pull_wiring` Decision 5** — do not mint a claim id for a sentence
  no design doc contains. Prefer a second `enforces:` tag on an existing row.
- **`nested_runtime_binding` Decision 3** — `NestedContent::attach` re-keys its
  metadata memo **only on an actually-new pin**
  (`repinned = (d_doc != &doc) || (d_doc->revision() != doc.revision())`), because
  drivers bind *per frame* against a pin taken once. Load-bearing here: see
  Constraint 4.
- **`pull_identity_disjoint_ids` Constraint 6** — *"Pixels must not move… A golden
  that needs a new baseline is a bug in this task, not a new baseline."*

## Constraints / requirements

1. **The bind happens inside `render_frame`, not inside `step()`.**
   `bind_operators` needs a live `PullService&`, and the only one that exists is
   `render_frame`'s frame-local `PullServiceImpl` (`interactive.cpp:239`). Binding
   from `HostViewport::step()` would require hoisting that service up into the
   viewport — which `runtime.interactive_pull_wiring` Decision 1 rejected, and
   which Constraint 2 of that task shows is not even possible without also
   hoisting `TileCache&` and `Backend&` out of the frame signature.

2. **The scope is strictly frame-local and must outlive the pull service by
   nothing.** `bind_operators` injects a `PullService&` pointing at a stack object
   in `render_frame`. A scope that survives the frame leaves every bound content
   holding a dangling pull pointer. So: declare the `OperatorBindingScope`
   *after* `pulls` (reverse destruction order detaches first —
   `operator_binding.hpp:95-96` states this ordering requirement explicitly, and
   `offline_sequence.cpp:173-176` is the precedent), and give it function scope so
   it is still alive through Step 5's park, Step 6's `poll_refinements` composite,
   and Step 7's prefetch. Caching the scope across frames as a member is
   **forbidden**, not merely unwise.

3. **Do not reorder the identity-map build relative to the bind.** The order is
   forced and already correct: `refresh_identity_memo` (`:232`) → `PullConfig::id_of`
   → `PullServiceImpl` (`:239`) → `bind_operators`. `id_of` is an *input* to the
   service that `bind_operators` injects, so the map cannot be built after the bind
   without a two-phase bind. It does not need to be: `build_pull_identity_map` seeds
   from `DocRoot::for_each_layer`, which is **document-global** — it enumerates every
   `LayerRecord` in the records HAMT, child compositions included
   (`src/model/model.cpp:483-495`). A nested's child layers are therefore already in
   the map under their real model `ObjectId`s before the `inputs()` walk runs, and
   they stay there: the walk's `walked` set is seeded from `roots`
   (`pull_identity.cpp:45`), so once nested *does* report inputs on a later frame
   they are skipped, no synthetic id is minted, and the map is identical frame 1 vs
   frame N. This is the same pre-bind order the offline driver has shipped with
   (`offline_sequence.cpp:92` builds the map; `:126` binds). Mirror it.

4. **The per-frame re-bind must not thrash nested's metadata memo.** `bind_operators`
   re-runs `try_attach` on every content on every call. `nested_runtime_binding`
   Decision 3 made `NestedContent::attach` re-key its memo only on an actually-new
   pin precisely so that a per-frame bind against a *stable* pin is free — and the
   shipped claim `05-recursive-composition#nested-metadata-memoized-on-aggregate-revision`
   depends on it. The interactive loop re-pins every frame
   (`host_viewport.cpp:121`), but the pin is the same `DocRoot` while the revision is
   unchanged, so `repinned` stays false and the memo survives. This is not a hope —
   it is A4.

5. **The `Model&`-constructed `HostViewport` must not change at all.** Eleven
   existing viewports are constructed that way, and they have no `Document` to bind.
   The binding is opt-in: absent, `render_frame` behaves exactly as it does today.
   No assert, no diagnostic, no behavior change. This is what makes the zero-golden-churn
   claim (A6) achievable.

6. **No new concurrency surface.** The interactive driver installs
   `direct_dispatch()` (`interactive.cpp:239`) and never calls `d_pool.submit` — its
   only use of the worker pool is `wait_completions(deadline_at)` at `:266`. Every
   render, leaf and operator alike, therefore runs inline on the frame thread, so a
   bound operator's re-entrant `PullService::pull` (which probes and inserts into the
   `TileCache`) touches the cache only from the thread that owns it —
   `worker_pool.hpp:37-40`'s invariant holds untouched. **The offline path's
   `is_operator` leaf-only gate (`offline_sequence.cpp:163-166`) must not be copied
   here.** That gate exists because *that* driver's `RenderDispatch` closes over
   `d_pool.submit`; no such lambda exists interactively. This task must not add one —
   `runtime.worker_dispatch_leaf_only` owns that change and its TSan coverage.

7. **`register_builtin_operator_binders()` must be called on the interactive path.**
   `SequenceRenderer` calls it in its constructor (`offline_sequence.cpp:63`),
   `ExportMonitor` in its (`export_monitor.cpp:138`). The registry is a thread-safe
   function-local static and the call is idempotent, so `InteractiveRenderer`'s
   constructor calls it too. A driver that binds without registering binds nothing,
   silently.

8. **No new levelization edge** (doc 17:60). `check_levels.py` must stay green with
   no `CMakeLists.txt` DEPENDS change: `interactive.cpp` including
   `arbc/runtime/operator_binding.hpp` and `arbc/runtime/document.hpp` is an
   intra-component include.

## Acceptance criteria

**A1 — Byte-exact interactive/offline parity for a non-endpoint operator layer.**
New `tests/interactive_operator_binding_golden.t.cpp`: a document with a fade at
`envelope == 0.5` and a crossfade at `w == 0.5` over leaf inputs, rendered through a
`Document`-bound `HostViewport::step()`, is **byte-identical** to the same frame
rendered through `SequenceRenderer`. This is the case that asserts (`fade_content.cpp:94`)
or blanks today. `enforces: 13-effects-as-operators#operators-bound-on-every-driver`.

**A2 — Nested renders interactively.** Same test file: a nested scene (a child
composition of two leaf layers, at least one of which is itself a fade, so the walk's
attach-before-`inputs()` order is exercised through the nesting boundary) rendered
through a `Document`-bound `HostViewport` is byte-identical to its `SequenceRenderer`
export, and to the same scene attached by hand. Assert after the frame that
`NestedContent::inputs()` is non-empty — the interactive half of registry row 246.
`enforces: 05-recursive-composition#nested-runtime-bound`,
`enforces: 08-serialization#nesting-inputs-are-derived-not-persisted`.

**A3 — A still scene binds nothing.** New counter `operator_binds()` on
`InteractiveRenderer` (mirroring `identity_map_builds()`, `interactive.hpp:161`). Over
an N-frame loop at a fixed revision on an operator scene: the first frame binds once,
every subsequent still frame returns through the early-out at `interactive.cpp:208-212`
and binds **zero** times, so `operator_binds() == 1` after N frames. Behavioral counter,
never wall-clock (doc 16). Assert in `src/runtime/t/interactive.t.cpp`.

**A4 — The per-frame re-bind does not thrash nested's metadata memo.** Over the same
N-frame still loop on a nested scene, `NestedContent::metadata_recomputes()` does not
grow with frame count. This pins `nested_runtime_binding` Decision 3's `repinned` rule
under the new per-frame binder — the shipped claim
`05-recursive-composition#nested-metadata-memoized-on-aggregate-revision` is what a
careless implementation of this task would break, and A3's early-out is not sufficient
protection (a scene with *any* live damage rebinds every frame). Drive damage each frame
and assert the memo still holds.
`enforces: 05-recursive-composition#nested-metadata-memoized-on-aggregate-revision`.

**A5 — Nested's child layers carry distinct pull identities interactively.** Assert that
after an interactive frame of the A2 nested scene, two same-stability child layers of the
nesting resolve to *different* `ObjectId`s under the frame's `id_of`, and neither is
`ObjectId{}`. This pins Constraint 3's load-bearing fact — that nested's children are
identified by the **global layer seed**, not by the `inputs()` walk — so that anyone who
later makes `for_each_layer` composition-scoped (which `model.hpp:88-94` openly nudges
toward) fails this test instead of silently collapsing two child layers onto one
`TileKey`. `enforces: 13-effects-as-operators#operator-input-children-have-distinct-cache-identity`
(second tag on the existing row; no new row).

**A6 — Zero regression, zero golden churn.** Every existing `Model&`-constructed
`HostViewport` test and every direct-`InteractiveRenderer` test compiles unchanged and
passes; **no golden file's bytes change**. The default `FrameBinding{}` is no binding,
which is today's behavior exactly. Per `pull_identity_disjoint_ids` Constraint 6: a golden
that needs a new baseline is a bug in this task, not a new baseline.

**A7 — TSan: a concurrent save against a live interactive binding.** Registry row 246
claims that a save taken while a live `OperatorBindingScope` is held — *"even concurrently,
on another thread, TSan clean and with no new lock"* — is byte-identical to the unbound
save. Until now the only live scopes were offline/export ones. Add a case to the existing
concurrent-save test that drives a `HostViewport::step()` loop on one thread while
serializing the `Document` on another, and run it under TSan: clean, and the emitted bytes
are identical to the unbound save (no `inputs` array, no `contents` table, no `$ref`). No
new lock. `enforces: 08-serialization#nesting-inputs-are-derived-not-persisted`.

**A8 — Claims register.** In `tests/claims/registry.tsv`:
- **New row** `13-effects-as-operators#operators-bound-on-every-driver`, backed by the
  doc 13 delta (Decision 7): every render driver binds the document's content graph before
  it renders — the interactive frame loop per frame against its frame-local pull service,
  the offline and export drivers against theirs — so an operator layer composites the same
  pixels under a deadline as in an exact export, no host attaches an operator by hand, and
  the binding is torn down with the frame that borrowed the pull service it holds.
- **Reword** row 165 (`05-recursive-composition#nested-runtime-bound`): it currently names
  only `SequenceRenderer` and `ExportMonitor` as bearers, which was true when it shipped and
  is now an understatement. Name the interactive frame loop alongside them. (Precedent:
  `pull_identity_disjoint_ids` reworded row 177 the same way, for the same reason.)
- **Second `enforces:` tags** on rows 246 and the operator-input-cache-identity row, per A2 /
  A5 / A7. No new rows for those — `interactive_pull_wiring` Decision 5.
- `scripts/check_claims.py` green.

**A9 — Gate.** `scripts/gate` green: build, full suite, `check_levels.py` (Constraint 8 — no
new component edge), `check_claims.py`, ≥90% diff coverage on changed lines (doc 16:112-118).

### Deferred follow-up (closer registers in WBS)

**None.** The obvious candidate — memoizing the bind walk, since `bind_operators` re-walks
the whole content graph (`operator_binding.cpp:99-135`, a fresh `unordered_set` and `vector`
per call) on every rendering frame — is deliberately **not** minted as a task. It cannot be
memoized in the way that matters: the scope injects a pointer to a frame-local pull service,
so the injection must be redone every frame regardless of what is cached about the graph
shape (Constraint 2). What is left to save is one hash-set allocation per rendering frame,
which the still-scene early-out (A3) already keeps off the idle path, and which the offline
driver has shipped per-frame without complaint (`offline_sequence.cpp:126`). Minting a task
for it would be speculative optimization, and minting one to *decide whether* to optimize
would be an audit task.

## Decisions

**Decision 1 — Bind inside `render_frame`, immediately after the `PullServiceImpl`, at
function scope.** The insertion point is `interactive.cpp:240`, between the pull service
(`:239`) and Step 4's `render_frame_interactive` (`:254`). Declaring it there gets all three
required properties at once: it is after `pulls` (so it destructs before it — Constraint 2),
it is after the still-scene early-out (so a still scene binds nothing — A3), and it is at
function scope (so it survives Step 5's park and Step 6's arrival composite, which re-drive
operator layers whose inputs settled late).

*Alternative rejected:* bind in `HostViewport::step()`, where both the `Document` and the pin
already live (`host_viewport.cpp:121`). There is no `PullService` there to bind against, and
creating one would mean hoisting `PullServiceImpl` — and with it `TileCache&` and `Backend&`
— out of the frame signature and into the viewport, which `runtime.interactive_pull_wiring`
Decision 1 explicitly rejected as pre-empting *this* task's design in the wrong task. It
would be perverse to accept the pre-emption now that we are the task being pre-empted.

*Alternative rejected:* hold the scope as an `InteractiveRenderer` member and rebuild it on
revision change, mirroring the identity memo. The scope holds a `PullService&` into a stack
frame that no longer exists by the next frame. This is a use-after-free, not a trade-off.

**Decision 2 — Plumb the `Document` and the pin as one trailing defaulted struct.**

```cpp
struct FrameBinding {
  const Document* document{nullptr};
  DocStatePtr pin{nullptr};
};
```

appended to `render_frame` as `FrameBinding binding = {}`. `HostViewport::step()` passes
`{d_document, state}` — it already holds the pin (`host_viewport.cpp:121`) and copying a
`DocStatePtr` once per frame is one atomic increment. `render_frame` asserts the pair is
coherent: both null or both non-null, and `&*binding.pin == &state`.

The struct, rather than two loose parameters, because a `Document` without a pin (or a pin
without a `Document`) is not a meaningful state — the struct makes the pair atomic and lets
one assert cover it. Trailing and defaulted, because that is exactly how `pulls` was added to
`render_frame_interactive` (`compositor.pull_service` Decision 3) and it is what keeps every
existing call site compiling and byte-identical (Constraint 5, A6).

*Alternative rejected:* change `render_frame`'s `const DocRoot& state` to `DocStatePtr state`
and derive the pin from it. Cleaner on paper — it removes the redundancy the assert guards —
but it churns every call site in the tree, including eleven `Model&`-ctor viewports and every
direct-renderer test, for a task whose central regression argument is that none of them move.
And it still would not supply the `Document`, which is the parameter actually missing.

**Decision 3 — `HostViewport` gains a `const Document* d_document`, null under the `Model&`
constructor.** `runtime.host_viewport_document_binding` stated flatly that "there is no
`Document` member" (`host_viewport.hpp:161-164`), and this task overturns that — the header
comment is updated in the same commit. The reason it held then was that the `Document&`
constructor used the document only transiently, to derive three seams that are all
`std::function`s. Binding is the fourth seam and it is not derivable: `bind_operators` needs
`Document::for_each_content()` (`document.hpp:240`) to enumerate contents-table contents that
no `DocRoot` layer walk reaches. The lifetime contract does not change: the `Model&` the
viewport already stores *is* `doc.d_model`, owned by that same `Document`, so a host that
could legally supply the `Model&` reference could already legally supply the `Document&`. Doc
01 § Viewport is amended to match (Decision 7).

*Alternative rejected:* a `Model → Document` back-pointer, so the `Model&` constructor could
find its document too. It would invert the ownership the `HostViewportDocumentAccess` attorney
(`document_access.hpp:20-22`) was built to keep one-directional, and it would let a
`Model&`-constructed viewport start binding — changing behavior for eleven call sites that
this task's regression argument depends on not changing.

**Decision 4 — Bind on every rendering frame; the still-scene early-out is the only
throttle.** This mirrors the offline driver exactly (`offline_sequence.cpp:126`, per frame,
against a pin taken once), and it is forced by Constraint 2 anyway. The cost is one graph walk
per *rendering* frame, and a frame that renders is already doing far more work than a walk of
its own content list. A still scene — the case that matters, because it is the one that runs at
60Hz doing nothing — returns at `interactive.cpp:208-212` before the pull service is even
constructed, so it binds zero times (A3).

**Decision 5 — Do not copy offline's leaf-only dispatch gate.** The interactive path has no
worker dispatch to gate: `direct_dispatch()` at `interactive.cpp:239` renders every miss inline,
and `d_pool` is only ever `wait_completions`-ed (`:266`) — `submit` appears nowhere in
`interactive.cpp`. So a bound operator's re-entrant pull cannot reach the `TileCache` from any
thread but the frame thread, and the TSan race that `offline_sequence.cpp:148-161` documents has
no thread to run on. Copying the gate here would be copying the *symptom* of an invariant this
path does not yet violate, and would put a second hand-rolled copy of it in the tree — which is
the exact thing `runtime.worker_dispatch_leaf_only` exists to prevent.

*Alternative rejected:* land `worker_backed_dispatch(pool)` as part of this task, since it is
the same file. That is a scoped, sequenced task with its own TSan obligations; folding it in
would put the concurrency change and the pixel change in one commit, so a golden regression and
a race would be indistinguishable at bisect time.

**Decision 6 — Assert the `FrameBinding` pair rather than trusting it.** `bind_operators`
already asserts `pin != nullptr` (`operator_binding.cpp:99`), but the failure mode this task can
introduce is subtler: a pin that is *valid* but is not the `DocRoot` the frame is compositing.
The compositor would walk one snapshot while the operators pulled from another — a stale-pixel
bug with no crash. `assert(&*binding.pin == &state)` costs nothing and makes it impossible.

**Decision 7 — Design-doc delta on docs 13 and 01; no doc 00 bullet.**
- **`docs/design/13-effects-as-operators.md`** (§ "Pulling inputs goes through the core") gains
  the normative paragraph *"Binding is the render driver's obligation, and every driver
  discharges it"* — that binding is the driver's job and not the host's, that the interactive
  loop binds per frame against its frame-local pull service exactly as the offline and export
  drivers do, and that a binding is scoped to the frame whose pull service it borrows. Doc 13
  said what attach *gives* a content (`:69-71`) but never said *who* must perform it, which is
  precisely the omission that let the interactive driver ship without a binder. Claim
  `13-effects-as-operators#operators-bound-on-every-driver` (A8) hangs on this paragraph.
- **`docs/design/01-core-concepts.md`** (§ Viewport) — the document supplies not "three things"
  but four; the fourth is the content graph each frame binds, and the host "never attaches an
  operator by hand." Without this, doc 01's "single wiring step" and doc 13's attach requirement
  are in unstated tension, and a host reading only doc 01 would not know who binds.
- **No doc 00 decision-record bullet.** This is a driver-symmetry omission being closed, not a
  project-shaping choice — the same call `runtime.host_viewport_document_binding` made, and for
  the same reason.

*Alternative rejected:* land the code with no doc delta and mint no claim, leaning on registry
row 246's existing "every rendered frame of an interactive session" clause as sufficient. That
clause is real and this task does back it (A2, A7), but it is a *serialization* claim that
mentions the interactive frame in passing. The load-bearing promise here — an operator composites
the same pixels under a deadline as in an exact export — would then be pinned by nothing, in a
codebase where its absence has already shipped once.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-12.

- `InteractiveRenderer::render_frame` gains a trailing defaulted `FrameBinding{const Document*, DocStatePtr}` parameter; on every rendering frame, calls `register_builtin_operator_binders()` and constructs a frame-local `OperatorBindingScope` after the `PullServiceImpl` (`src/runtime/interactive.cpp`, `src/runtime/arbc/runtime/interactive.hpp`).
- `HostViewport` gains a `const Document* d_document` member (null under `Model&` ctor); `step()` passes `{d_document, state}` to `render_frame` (`src/runtime/host_viewport.cpp`, `src/runtime/arbc/runtime/host_viewport.hpp`).
- New golden test file `tests/interactive_operator_binding_golden.t.cpp`: A1 fade/crossfade byte-exact vs. `SequenceRenderer`, A2 nested scene byte-exact, A4 memo non-thrash, A5 distinct child pull identities.
- Counter `operator_binds()` added to `InteractiveRenderer`; A3 still-scene counter asserted in `src/runtime/t/interactive.t.cpp`.
- A7 TSan case added to `tests/document_serialize_concurrency.t.cpp`: concurrent save races a live interactive binding loop, TSan clean, bytes identical to unbound save.
- Claims register updated: new row `13-effects-as-operators#operators-bound-on-every-driver`, reworded row 165, second `enforces:` tags on rows 246 and operator-input-cache-identity (`tests/claims/registry.tsv`).
- Stale "interactive never binds" comments retired in `tests/interactive_operator_identity_golden.t.cpp`, `tests/async_external_load_golden.t.cpp`, `tests/interactive_model_damage_routing.t.cpp`.
- Doc 13 § "Pulling inputs goes through the core" and doc 01 § Viewport amended per Decision 7 (`docs/design/13-effects-as-operators.md`, `docs/design/01-core-concepts.md`).
