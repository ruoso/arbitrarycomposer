# Refinement — `operators.fade_runtime_binding`

## TaskJuggler entry

`tasks/50-operators.tji:15-20`:

```
task fade_runtime_binding "org.arbc.fade runtime injection wiring" {
  effort 0.5d
  allocate team
  depends !fade, model.content_binding
  note "Wire FadeContent attach injection (PullService, Backend) onto live services at runtime instantiation, tear down on release. Source of debt: tasks/refinements/operators/fade.md. Doc 13."
}
```

This task was pre-registered as the deferred follow-up of `operators.fade`
(`tasks/refinements/operators/fade.md:315-325`, Status pointer at
`:441`). It is gathered by milestone `m9_release`
(`tasks/99-milestones.tji:71`), beside `operators.crossfade_runtime_binding`,
`kinds.nested_runtime_binding`, and `kinds.raster_runtime_binding`.

## Effort estimate

`effort 0.5d`, `allocate team`. This is a wiring task: no new kernel, no new
math. It closes one seam — the production attach injection that `org.arbc.fade`'s
tests drive by hand today — by making the runtime drivers do at
instantiation what `tests/fade_conformance.t.cpp:83-84` and
`tests/fade_goldens.t.cpp:121` do inline.

## Inherited dependencies

**Settled predecessors this task builds on (all `complete 100`):**

- `operators.fade` (`tasks/50-operators.tji:9-14`, refinement
  `tasks/refinements/operators/fade.md`, **Done 2026-07-08**). Shipped
  `FadeContent` with the attach seam this task now feeds:
  `FadeContent::attach(PullService& pull, Backend& backend)` decl
  `src/kind_fade/arbc/kind_fade/fade_content.hpp:53` (comment `:51-52`
  names *this* task as its production wiring), impl
  `src/kind_fade/fade_content.cpp:20-22`, borrowed non-owning pointers
  `PullService* d_pull{nullptr}; Backend* d_backend{nullptr};` at
  `fade_content.hpp:104-105`. `render()` asserts attachment
  (`fade_content.cpp:87`) and pulls its input via `d_pull->pull(...)`
  (`:102, :138`); the audio path asserts (`:209`) and pulls via
  `d_pull->pull_audio(...)` (`:218`).
- `runtime.operator_codecs` (commit `aa21f59`). The runtime's built-in fade
  codec — the one L5 TU that legally sees the concrete `FadeContent` type —
  mints instances at load: `std::make_unique<FadeContent>(inputs[0], p)` at
  `src/runtime/codec_fade.cpp:118`, registered by `fade_codec()`
  (`src/runtime/arbc/runtime/builtin_codecs.hpp`) into the load façade
  `src/runtime/document_serialize.cpp`. The façade already "wraps their
  deserialize with per-load kind recording for the read path" — the hook
  this task extends to record the binding.
- `compositor.pull_service`. The concrete live service to inject:
  `PullServiceImpl` at `src/compositor/arbc/compositor/pull_service.hpp:149-209`,
  ctor `:151`. Its class comment `:144-148` states it is "injected as a
  `PullService*` to content at attach"; the header comment `:32-34` states
  "Runtime (L5) binds that functor to `WorkerPool::submit` and performs the
  attach-time injection" — the seam this task realizes.

**Settled this task depends on directly:**

- `model.content_binding` (`tasks/10-model.tji:57`, refinement
  `tasks/refinements/model/content_binding.md`, **Done 2026-07-09**).
  Establishes the permanent id→`Content*` binding in the runtime side-map:
  `Document::add_content(std::shared_ptr<Content>, std::uint64_t kind = 0)`
  (`src/runtime/arbc/runtime/document.hpp:31`) mints a versioned
  `ContentRecord{kind, StateHandle}` and stores the vtable binding in
  `d_contents` (`document.hpp:85-89`); `Document::resolve(ObjectId) const ->
  Content*` (`document.hpp:74`) serves it. **This is the enumeration surface
  this task walks** — but `content_binding` deliberately stops at the
  record + side-map and leaves live-service injection "the downstream
  runtime-binding concern." That downstream concern, for fade, is *this
  task*.

**Pending — nothing.** All predecessors are `complete 100`. This task
introduces no new dependency and, per the Decisions below, no design-doc
delta.

## What this task is

When a runtime instance holds a `Document` whose content graph contains an
`org.arbc.fade` node, that `FadeContent` must have a live `PullService` and
`Backend` attached before its `render()` / `render_audio()` is invoked —
otherwise the attach assertions (`fade_content.cpp:87, :209`) abort. Today no
production code calls `FadeContent::attach`; a repo-wide grep for
`.attach(` / `->attach(` outside test directories finds only the unrelated
`Document::attach_layer` (model membership). Every real attach lives in a
test with inline doubles. This task wires the runtime's render drivers to
attach their live `PullServiceImpl` + `Backend` onto each fade content at
instantiation and tear the binding down on release, closing the production
wiring that fade's tests stub.

## Why it needs to be done

- **Without it, a loaded fade aborts.** `offline_sequence.cpp` and
  `export_monitor.cpp` construct a live `PullServiceImpl` and call
  `resolve(id)->render()` through `render_frame_interactive`; when that
  `Content*` is a `FadeContent`, `render()` hits its `assert(d_pull != …)`
  and aborts. The manual attach in tests is the only thing keeping fade
  renderable — real documents cannot render a fade until this lands.
- **It is a gate for `m9_release`** (`tasks/99-milestones.tji:71`),
  alongside its three siblings; the reference operators are not shippable
  end-to-end until their runtime binding closes.
- **It establishes the pattern** `operators.crossfade_runtime_binding` and
  the two `kinds.*_runtime_binding` tasks mirror — the runtime-side operator
  binding seam is authored once here and reused per kind.

## Inputs / context

**Design docs (normative — doc 16's constitution rule):**

- `docs/design/13-effects-as-operators.md`:
  - `:69-71` — "Operators do not call `input->render()` directly … At
    attach, content receives a `PullService`." The normative source of the
    attach-injection contract.
  - `:73-83` — the `PullService` sketch (`pull` / `pull_audio`): "the same
    machinery as a compositor-issued request … snapshot token respected,
    deadline/budget inherited."
  - `:122-138` — caching/scheduling: operator output caches by aggregate
    revision; `identity()` short-circuits; budgets/deadlines flow through
    pulls.
  - `:173-188` — new machinery: the real `PullService`; "operators receive
    and produce working-space surfaces like everyone."
  - `:199-208` — scheduling decision: `PullService`, `identity()`, and the
    `org.arbc.fade` / `org.arbc.crossfade` kinds ship in v1.
- `docs/design/17-internal-components.md`:
  - `:57` (`arbc::contract`, L3) holds the `PullService` *interface*; `:56`
    (`arbc::compositor`, L4) holds the *implementation*; `:59`
    (`arbc::kind-*`, L4) depend on `contract` only.
  - `:60` (`arbc::runtime`, L5) — "everything below": the only level that
    may name both the concrete `PullServiceImpl` (L4) and the operator
    kinds (L4) and hand one to the other. **This task lives here.**
  - `:66-72` — "The model stays free of the `Content` vtable (records hold
    opaque content slots; binding happens in `runtime`)." The load-bearing
    reason the attach injection can only live in `runtime`.

**Source seams (real paths + current lines):**

- Attach seam (the hook to feed): `FadeContent::attach(PullService&,
  Backend&)` — `src/kind_fade/arbc/kind_fade/fade_content.hpp:53`, impl
  `src/kind_fade/fade_content.cpp:20-22`; borrowed pointers
  `fade_content.hpp:104-105`; `kind_id = "org.arbc.fade"` at `:82`; render
  asserts `:87, :209`.
- Contract seams: `PullService` (`src/contract/arbc/contract/content.hpp:617`),
  `pull` (`:625-626`), `pull_audio` (`:640-641`); the header comment
  `:611-615` — "the attach-time injection that hands a service to content
  is the L4 concern (`compositor.pull_service`)."
- Live service to inject: `PullServiceImpl`
  (`src/compositor/arbc/compositor/pull_service.hpp:149-209`); `Backend`
  seam `src/surface/arbc/surface/backend.hpp:37-40`
  (`composite(dst, src, src_to_dst, opacity)`).
- Enumeration surface: `Document::resolve` / `d_contents` side-map
  (`src/runtime/arbc/runtime/document.hpp:74, :85-89`); the concrete
  instantiation point `src/runtime/codec_fade.cpp:118`; the load façade's
  per-load recording hook in `src/runtime/document_serialize.cpp`
  (fade codec registration around `:289-291`).
- Driver wiring points (where a live `PullServiceImpl` is constructed and
  document content is rendered):
  - `src/runtime/offline_sequence.cpp` — resolver lambda `:69`, parallel
    `PullServiceImpl pulls(...)` `:101`, `render_frame_interactive(...)`
    calls `:103, :116`. Note the **inline (non-parallel) path** (`:83`)
    passes `pulls = nullptr` and drives misses through the compositor's
    synchronous fill; a fade content still needs a `PullService` to pull
    *its own* input, so this path must also construct and bind one.
  - `src/runtime/export_monitor.cpp` — long-lived
    `d_pull = std::make_unique<PullServiceImpl>(...)` `:135`, mix pulls at
    `:153`; substrate members `export_monitor.hpp:170` (`d_resolve`),
    `:181` (`d_pull`).
- Manual-attach idiom this replaces (the tests that keep fade renderable):
  `tests/fade_conformance.t.cpp:31` (`InlineAudioPull`), `:77`
  (`CpuBackend`), `:83-84, :98-99` (`fade->attach(pull, backend)`);
  `tests/fade_goldens.t.cpp:121` (golden attaches inline; `:56` comment
  names this task as the deferred production wiring).
- Counter surface for behavioral assertions:
  `CompositorCounters::operator_renders()` / `note_operator_render()` at
  `src/compositor/arbc/compositor/counters.hpp:47-54`.

**Predecessor decisions carried in:**

- `fade.md` Decision 1 (`:329-339`): `kind_fade` `DEPENDS contract` only, no
  doc-17 delta — because `Backend`/`PullService` reach fade as abstract
  contract-reachable interfaces *injected at attach*, creating no new
  dependency edge. This task adds the injection on the `runtime` side, where
  the edge to `kind_fade` and `compositor` already exists — so it likewise
  needs no doc-17 delta.
- `fade.md` Constraint 10 (`:256-258`): "Fade receives `PullService&` and
  `Backend&` at attach (mirroring `nested_content.hpp:68`); it owns
  neither." This task supplies those references from live services and
  guarantees they outlive every render between attach and detach.

## Constraints / requirements

1. **Levelization (doc 17).** All new code lives in `arbc::runtime` (L5).
   It may name `PullServiceImpl` (L4 `compositor`) and `FadeContent` (L4
   `kind_fade`) — both edges `runtime` already declares. It must add **no**
   new component dependency and **no** `kind_fade → compositor`/`runtime`
   edge. The CI levelization check must stay green.
2. **Bind at instantiation, before first render.** Every `FadeContent`
   reachable in a rendered document is attached to the driver's live
   `PullServiceImpl` and `Backend` before `render_frame_interactive` can
   resolve and render it — on both the inline and parallel offline paths and
   the export-monitor path.
3. **Tear down on release.** When the binding scope ends (the frame/driver
   completes, the content is removed, or the runtime instance is torn down),
   the borrowed `d_pull`/`d_backend` in each fade content are cleared so no
   render after release dereferences a dangling service. The service objects
   are borrowed, never owned by the content (Constraint 10).
4. **Services outlive the binding.** The live `PullServiceImpl` + `Backend`
   must outlive every render issued between attach and detach — the binding
   scope is nested inside the service lifetime, never the reverse.
5. **Pull discipline preserved.** After binding, fade pulls its input only
   through the injected live `PullServiceImpl::pull` / `pull_audio` — never
   `input->render()`. The bound service carries the request's snapshot,
   exactness, deadline, and budget verbatim (doc 13:69-83), identical to the
   inline-double path the goldens froze.
6. **Byte-identical to the manual-attach goldens.** A fade rendered through
   the live-bound driver must produce bytes identical to the frozen
   `tests/fade_goldens.t.cpp` tables that use an inline attach — binding
   changes *who* calls `attach`, never *what* fade computes.
7. **Fade-scoped, sibling-mirrorable.** This task wires `org.arbc.fade`
   only. The runtime binding seam it introduces must be shaped so
   `operators.crossfade_runtime_binding` and the `kinds.*_runtime_binding`
   tasks reuse it by registering their kind, not by rewriting the driver.
8. **Concurrency.** On the parallel offline path, fade content is bound once
   on the driver thread before any worker dispatch; its borrowed service
   pointers are read-only on worker threads during render. No worker mutates
   the binding. This must be race-free under TSan (Constraint 12 coverage
   below).

## Acceptance criteria

**End-to-end render binding (new runtime-component test,
`src/runtime/t/fade_runtime_binding.t.cpp`):** build a `Document` containing
an `org.arbc.fade` layer over a solid (and a tone, for audio) input via the
production instantiation path (`Document::add_content` / the fade codec),
render it through the real `offline_sequence` driver (inline *and* parallel)
and the `export_monitor` audio path, and assert it renders to completion
**without** the attach assertions firing — i.e. the production wiring exists.
This test would abort today.

**Byte-exact goldens (re-asserted end-to-end):** the live-bound offline
render of the visual golden scene reproduces the frozen
`tests/fade_goldens.t.cpp` bytes exactly, and the export-monitor audio render
reproduces the frozen audio golden bytes exactly — proving live-service
binding is behaviorally identical to the manual-attach golden. No tolerance;
byte-exact (re-asserts `16-sdlc-and-quality#byte-exact-goldens`,
`registry.tsv:42`).

**Behavioral counter (identity through the live driver):** a visible fade
layer at a fully-open envelope (`E == 1`) rendered through the real driver
issues zero operator renders — `CompositorCounters::operator_renders()` delta
0 — while a non-identity fade records exactly one. Re-asserts
`13-effects-as-operators#identity-plan-issues-no-operator-render`
(`registry.tsv:138`) against live binding rather than an inline double.

**Pull discipline through the live driver:** re-assert
`13-effects-as-operators#operator-pulls-only-via-pull-service`
(`registry.tsv:144`) with the fade bound to a *live* `PullServiceImpl`
instrumented via `CompositorCounters`: each input render the fade provokes
equals one the service issued (`requests_issued` / `audio_dispatches`
deltas), and no input's direct-render flag flips. Also re-asserts the fade
math/stability claims `13-effects-as-operators#fade-attenuates-both-facets`
(`:141`), `#fade-timed-over-static` (`:142`),
`#fade-identity-at-open-envelope` (`:143`) now exercised through the runtime.

**New claim (the production-binding promise this task lands):** register
`13-effects-as-operators#operator-bound-to-live-services-at-instantiation`
in `tests/claims/registry.tsv`, enforced by a test in the new runtime
component test tagged `enforces:`. Claim text: *the runtime attaches a live
`PullService`/`Backend` onto an operator content (`org.arbc.fade`) at
instantiation so its `render()`/`render_audio()` runs with no manual attach,
and tears the binding down on release so no borrowed service is dereferenced
after; a document containing a fade layer rendered through the real
offline/export driver produces bytes identical to the manually-attached
golden and issues its input pulls only through the live `PullServiceImpl`.*
This is a design promise (doc 13:69-71, doc 17:60/66-72,
`pull_service.hpp:32-34`) not previously pinned by any claim — the existing
fade claims pin fade's math/pull with inline doubles; this pins the
production wiring.

**Teardown assertion:** a test that binds, renders, then ends the binding
scope (release) and asserts the fade's borrowed pointers are cleared — a
render attempted after release re-binds through the driver or is inert, never
dereferencing the released service. Behavioral, exercising Constraint 3.

**Concurrency (TSan lane):** a stress test rendering a fade scene through the
parallel offline path under TSan — the binding is written once on the driver
thread before dispatch and read-only on workers — asserts no data race
(Constraint 8). Reuses the parallel offline reap-to-quiescence path
(`offline_sequence.cpp:100-118`).

**Coverage:** ≥90% diff coverage on changed lines (CI gate). Tests ship in
this task.

**Deferred follow-ups:** none new. `operators.crossfade_runtime_binding`
(`tasks/50-operators.tji:28-33`) and `kinds.nested_runtime_binding` /
`kinds.raster_runtime_binding` (`tasks/55-kinds.tji`) already exist as WBS
leaves that reuse the seam introduced here; this task does not spawn a new
one. Plugin-provided (non-built-in) operator kinds bind through the same
runtime seam when the plugin registry lands — already covered by the plugin
loading tasks, not deferred here.

## Decisions

1. **The binding lives in `arbc::runtime`, driven at instantiation, torn
   down by scope.** *Rationale:* doc 17:60/66-72 and
   `pull_service.hpp:32-34` assign the attach-time injection to `runtime`
   explicitly — it is the only level that sees both the concrete
   `PullServiceImpl` and the concrete `FadeContent`. RAII scope for teardown
   matches Constraint 3/4 (services outlive the binding; binding cleared on
   release) without a manual detach-everywhere discipline. *Rejected:*
   attaching inside `Document::add_content` — the live `PullServiceImpl` does
   not exist at document-build time (it is constructed per-render in the
   offline driver, long-lived in the export monitor), so add_content has
   nothing to inject.

2. **Dispatch by the runtime's kind knowledge, calling the concrete
   `FadeContent::attach`, registered by the fade codec TU.** The one L5 TU
   that legally sees the concrete `FadeContent` type is `codec_fade.cpp`
   (`builtin_codecs.hpp` states this explicitly). It registers a per-kind
   bind thunk alongside its codec, capturing the concrete `FadeContent*` and
   calling `attach(pull, backend)`; the driver runs every registered thunk
   over the document's content before render and clears them on scope exit.
   *Rationale:* reuses the codec's unique concrete-type visibility and the
   load façade's existing per-load recording hook; no RTTI; each sibling
   runtime-binding task registers its own kind's thunk without touching the
   driver (Constraint 7). *Rejected:* (a) `dynamic_cast<FadeContent*>` in the
   driver — works (runtime links `kind_fade`) but grows an RTTI cast chain in
   the driver as each sibling lands, coupling the driver to every kind;
   (b) a uniform virtual `Content::bind(PullService&, Backend&)` on the
   contract — a doc-13/doc-17 delta touching the whole operator family, and
   it cannot serve `NestedContent::attach`'s richer signature
   (`nested_content.hpp:68` takes a resolver + pinned `DocRoot`), so a uniform
   two-arg hook is the wrong abstraction and out of a 0.5d fade scope.

3. **No design-doc delta.** *Rationale:* doc 13:69-71 already promises
   attach-time injection and doc 17:60/66-72 already assigns it to
   `runtime`; this task *implements* the promised behavior at the assigned
   level and creates no new component edge (`runtime` → `kind_fade` /
   `compositor` already exist; the attach traffics only in abstract
   `contract` interfaces — `fade.md` Decision 1's reasoning, applied on the
   injecting side). Amending a doc for a promise already written would be
   noise. *Rejected:* a doc-13 amendment describing the runtime binder — the
   binder is a runtime-internal seam, not designed behavior; refinements, not
   docs, are the home for internal seam shape.

4. **Both offline paths and the export monitor are wired in this task.**
   *Rationale:* Constraint 2 — a fade that renders in offline but aborts in
   the export/interactive path is a latent crash; the drivers are the
   complete set of live-`PullServiceImpl` construction sites
   (`offline_sequence.cpp:83/101`, `export_monitor.cpp:135`). Wiring only one
   would leave the promise half-kept. *Rejected:* wiring only the parallel
   offline path (the one that already builds a `PullServiceImpl`) — the
   inline path renders fade too and needs a service for fade's *own* input
   pull.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-09.

- Teardown/observability seam added to `FadeContent`: `detach()` and `attached()` in `src/kind_fade/arbc/kind_fade/fade_content.hpp` and `src/kind_fade/fade_content.cpp`.
- Kind-agnostic operator binder registry added at `src/runtime/arbc/runtime/operator_binding.hpp` and `src/runtime/operator_binding.cpp` — typed `try_attach`/`detach` thunks, `bind_operators()` graph-walk, RAII `OperatorBindingScope`.
- `register_fade_binder()` typed thunk added in `src/runtime/codec_fade.cpp` and declared in `src/runtime/arbc/runtime/builtin_codecs.hpp` (uses `dynamic_cast` inside the codec TU, per Decision 2 deviation).
- `Document::for_each_content()` enumeration surface added in `src/runtime/arbc/runtime/document.hpp` and `src/runtime/document.cpp`.
- Both inline and parallel offline paths in `src/runtime/offline_sequence.cpp` bind fades to a live `PullServiceImpl` before render, with RAII teardown; inline path now constructs its own `PullServiceImpl` (previously `nullptr`).
- Export-monitor path wired in `src/runtime/arbc/runtime/export_monitor.hpp` and `src/runtime/export_monitor.cpp` — `d_binding` member, bind in ctor, RAII teardown.
- End-to-end test at `tests/fade_runtime_binding.t.cpp` — 6 cases: byte-exact inline offline, byte-exact parallel (TSan stress), byte-exact audio export, identity counter (0/1), timed-over-static, teardown.
- New claim `13-effects-as-operators#operator-bound-to-live-services-at-instantiation` registered in `tests/claims/registry.tsv` and enforced by the new test.
- Build/test registration in `src/runtime/CMakeLists.txt` and `tests/CMakeLists.txt`.
- **Deviation 1:** test lives in `tests/` (not `src/runtime/t/`) — requires `backend_cpu` for byte-exact acceptance, which would violate levelization under `src/runtime/t/`.
- **Deviation 2:** dispatch uses `dynamic_cast` inside the codec-registered thunk; the driver itself remains kind-agnostic per Decision 2's core intent.
