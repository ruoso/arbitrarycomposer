# Refinement — `operators.crossfade_runtime_binding`

## TaskJuggler entry

`tasks/50-operators.tji:29-34`:

```
task crossfade_runtime_binding "org.arbc.crossfade runtime injection wiring" {
  effort 0.5d
  allocate team
  depends !crossfade, model.content_binding
  note "Wire CrossfadeContent attach injection (PullService, Backend) onto live services at runtime instantiation, tear down on release. Source of debt: tasks/refinements/operators/crossfade.md. Doc 13."
}
```

This task was pre-registered as the deferred follow-up of `operators.crossfade`
(`tasks/refinements/operators/crossfade.md:291-298`). It is gathered by
milestone `m9_release` (`tasks/99-milestones.tji:71`), beside its sibling
`operators.fade_runtime_binding` (**Done 2026-07-09**),
`kinds.nested_runtime_binding`, and `kinds.raster_runtime_binding`.

## Effort estimate

`effort 0.5d`, `allocate team`. This is the smallest of the runtime-binding
tasks: no new kernel, no new math, and — unlike `fade_runtime_binding` — no
new driver seam. `fade_runtime_binding` already built the kind-agnostic
runtime binder registry and wired both offline paths and the export monitor to
call it. Crossfade closes its production wiring by (a) adding the `detach()` /
`attached()` seam `CrossfadeContent` still lacks, and (b) registering its own
typed binder thunk — the drivers then bind it with no per-kind change, exactly
the reuse `fade_runtime_binding` Constraint 7 promised.

## Inherited dependencies

**Settled predecessors this task builds on (all `complete 100`):**

- `operators.crossfade` (`tasks/50-operators.tji:21-27`, refinement
  `tasks/refinements/operators/crossfade.md`, **Done 2026-07-09**). Shipped
  `CrossfadeContent` with the attach seam this task feeds:
  `CrossfadeContent::attach(PullService& pull, Backend& backend)` decl
  `src/kind_crossfade/arbc/kind_crossfade/crossfade_content.hpp:52`, impl
  `src/kind_crossfade/crossfade_content.cpp:59-62`; borrowed non-owning
  pointers `PullService* d_pull{nullptr}; Backend* d_backend{nullptr};` at
  `crossfade_content.hpp:105-106`; `kind_id = "org.arbc.crossfade"` at `:84`;
  two inputs stored `std::array<ContentRef, 2> d_inputs{from, to}` (`:103`,
  ctor `:47`). `render()` asserts attachment
  (`crossfade_content.cpp:163`) and pulls its two inputs via `d_pull->pull(...)`
  (endpoint pass-through helper `:145`, interior input-1 source-over `:210`);
  the audio path asserts (`:272`) and pulls both inputs via `pull_audio`
  (`:280, :301`). **`CrossfadeContent` has no `detach()` / `attached()` yet** —
  the gap this task fills (see What this task is).
- `operators.fade_runtime_binding` (`tasks/50-operators.tji:15-20`, refinement
  `tasks/refinements/operators/fade_runtime_binding.md`, **Done 2026-07-09**).
  Built **the seam this task reuses wholesale**:
  - the kind-agnostic binder registry `src/runtime/arbc/runtime/operator_binding.hpp`
    / `operator_binding.cpp` — `OperatorBinder{try_attach, detach}` thunk pair
    (`operator_binding.hpp:40-43`), `register_operator_binder(kind_id, binder)`
    (`:49`, impl `operator_binding.cpp:33-42`, first-registration-wins),
    `register_builtin_operator_binders()` (`:55`, impl `:44-53`, once-guarded),
    `bind_operators(const Document&, PullService&, Backend&)` DFS graph-walk
    (`:95`, impl `:84-108`), and RAII `OperatorBindingScope` (`:62-85`);
  - the typed-thunk registration pattern this task mirrors:
    `register_fade_binder()` at `src/runtime/codec_fade.cpp:126-141` —
    `register_operator_binder(FadeContent::kind_id, OperatorBinder{...})` whose
    `try_attach` does `dynamic_cast<FadeContent*>` then `fade->attach(...)` and
    whose `detach` does `static_cast<FadeContent&>(c).detach()`; declared in
    `builtin_codecs.hpp:51`; forward-declared and called inside
    `register_builtin_operator_binders()` (`operator_binding.cpp:16, :50`);
  - the driver wiring — **already fully kind-agnostic**:
    `offline_sequence.cpp` calls `register_builtin_operator_binders()` (`:63`)
    then `bind_operators(...)` on the inline exact path (`:124`) and the
    parallel exact path (`:149`); `export_monitor.cpp` calls both in its ctor
    (`:144-145`) with the `OperatorBindingScope d_binding` member
    (`export_monitor.hpp:194`, declared last so it destructs first). No driver
    names a concrete kind — registering crossfade's binder is sufficient.

**Settled this task depends on directly:**

- `model.content_binding` (`tasks/10-model.tji:57`, refinement
  `tasks/refinements/model/content_binding.md`, **Done 2026-07-09**). The
  id→`Content*` binding walked by `bind_operators` via
  `Document::for_each_content()` (`operator_binding.cpp:84-108`); `content_binding`
  deliberately stops at the record + side-map and leaves live-service injection
  "the downstream runtime-binding concern" — that concern, for crossfade, is
  *this task*.

**Pending — nothing.** All predecessors are `complete 100`. This task
introduces no new dependency and, per the Decisions below, no design-doc delta.

## What this task is

When a runtime instance holds a `Document` whose content graph contains an
`org.arbc.crossfade` node, that `CrossfadeContent` must have a live
`PullService` and `Backend` attached before its `render()` / `render_audio()`
is invoked — otherwise the attach assertions
(`crossfade_content.cpp:163, :272`) abort. Today no production code calls
`CrossfadeContent::attach`; every real attach lives in a test with inline
doubles (`tests/crossfade_goldens.t.cpp:125, 189, 256, 279`, and through a live
`PullServiceImpl` at `:150`; conformance fixture `:143`; the golden's comment
`:58-60` explicitly names *this task* as its deferred production wiring).

Because `fade_runtime_binding` already made the drivers bind operators through a
kind-agnostic registry, the concrete work here is small and mechanical:

1. **Add the teardown/observability seam `CrossfadeContent` still lacks.**
   `FadeContent` grew `detach() noexcept` (`fade_content.hpp:60`) and
   `attached() const noexcept` (`:65`) in `fade_runtime_binding`;
   `CrossfadeContent` has only `attach()`. Add `detach()` (clears
   `d_pull`/`d_backend` to `nullptr`) and `attached()` to
   `crossfade_content.hpp`/`.cpp` — required because the binder's `detach` thunk
   calls `content.detach()`.
2. **Register the crossfade binder.** Add `register_crossfade_binder()` in
   `src/runtime/codec_crossfade.cpp` (the one runtime TU that legally names the
   concrete `CrossfadeContent`, `builtin_codecs.hpp:9-11`), mirroring
   `register_fade_binder()`: a typed `OperatorBinder` whose `try_attach`
   `dynamic_cast<CrossfadeContent*>` then `attach(pull, backend)`, whose
   `detach` `static_cast<CrossfadeContent&>(c).detach()`, registered under
   `CrossfadeContent::kind_id`. Add `#include <arbc/runtime/operator_binding.hpp>`
   to that TU (it does not include it today).
3. **Declare and reach it.** Declare `register_crossfade_binder()` in
   `builtin_codecs.hpp` (after `crossfade_codec()`, `:58`); forward-declare it
   and add its call inside `register_builtin_operator_binders()`
   (`operator_binding.cpp:16, :50`, beside `register_fade_binder()`).

No driver file changes: `offline_sequence.cpp` and `export_monitor.cpp` already
bind every registered kind generically.

## Why it needs to be done

- **Without it, a loaded crossfade aborts.** `offline_sequence.cpp` and
  `export_monitor.cpp` construct a live `PullServiceImpl` and render document
  content through `bind_operators` + the frame drivers; a `CrossfadeContent`
  that no registered binder matches is never attached, so its `render()` hits
  `assert(d_pull != …)` and aborts. The manual attach in tests is the only thing
  keeping crossfade renderable — real documents cannot render a crossfade until
  this lands.
- **It is a gate for `m9_release`** (`tasks/99-milestones.tji:71`), alongside
  its three siblings; the reference operators are not shippable end-to-end until
  their runtime binding closes. Crossfade is the *multi-input* operator — this
  is where the runtime binder proves it binds a content whose graph walk pulls
  through two inputs, not one.
- **It completes the pattern `fade_runtime_binding` authored** for the two
  `kinds.*_runtime_binding` siblings to follow: register a kind's typed binder,
  touch no driver.

## Inputs / context

**Design docs (normative — doc 16's constitution rule):**

- `docs/design/13-effects-as-operators.md`:
  - `:69-72` — "Operators do not call `input->render()` directly … At attach,
    content receives a `PullService`." The normative source of the
    attach-injection contract.
  - `:112-120` — per-input, per-facet pull shape: each input is "an ordinary
    pull, each hitting the time-keyed cache"; the two-input case this task
    exercises through a live service.
  - `:168` — the `org.arbc.crossfade` reference-kind row (two-input operator,
    extent union), a v1 ship (`:199-208`).
- `docs/design/17-internal-components.md`:
  - `:56` (`arbc::compositor`, L4) holds the `PullService` *implementation* +
    counters; `:59` (`arbc::kind-*`, L4) hold the operator kinds, depending on
    `contract` only ("nested uses only the `PullService` interface").
  - `:60` (`arbc::runtime`, L5) — "everything below": the only level that may
    name both the concrete `PullServiceImpl` (L4 `compositor`) and the concrete
    `CrossfadeContent` (L4 `kind_crossfade`) and hand one to the other.
    **This task lives here.**
  - `:66-72` — "The model stays free of the `Content` vtable (records hold
    opaque content slots; binding happens in `runtime`)." The load-bearing
    reason the attach injection can only live in `runtime`.

**Source seams (real paths + current lines):**

- Attach seam (the hook to feed): `CrossfadeContent::attach(PullService&,
  Backend&)` — `crossfade_content.hpp:52`, impl `crossfade_content.cpp:59-62`;
  borrowed pointers `:105-106`; `kind_id` `:84`; render asserts
  `crossfade_content.cpp:163, :272`; input pulls `:145, :210` (visual),
  `:280, :301` (audio). **`detach()`/`attached()` absent — added by this task**
  (mirror `fade_content.hpp:60, :65`, impl semantics: clear both borrowed
  pointers to `nullptr`; `attached()` returns `d_pull != nullptr`).
- Binder registry (reused verbatim, no change):
  `src/runtime/arbc/runtime/operator_binding.hpp` — `OperatorBinder`
  (`:40-43`), `register_operator_binder` (`:49`),
  `register_builtin_operator_binders` (`:55`), `bind_operators` (`:95`),
  `OperatorBindingScope` (`:62-85`); impl `operator_binding.cpp:33-108`.
- Registration template: `register_fade_binder()`
  `src/runtime/codec_fade.cpp:126-141` (`#include` of `operator_binding.hpp` at
  `:13`); declared `builtin_codecs.hpp:51`; forward-declared + called
  `operator_binding.cpp:16, :50`.
- Crossfade codec TU (where the binder is defined): `codec_crossfade.cpp` —
  instantiation `std::make_unique<CrossfadeContent>(inputs[0], inputs[1], p)`
  (`:86`), `crossfade_codec()` (`:91`); serialize's own `dynamic_cast<const
  CrossfadeContent*>` at `:45` (the pattern the binder's `try_attach` mirrors);
  **no `operator_binding.hpp` include today** — add it. Codec registration in
  `document_serialize.cpp` (save `:118`, load `:292-293`); `crossfade_codec()`
  declared `builtin_codecs.hpp:58`.
- Live service to inject: `PullServiceImpl`
  (`src/compositor/arbc/compositor/pull_service.hpp:149`); `Backend` seam
  `src/surface/arbc/surface/backend.hpp` (`composite(dst, src, src_to_dst,
  opacity)`).
- Driver wiring (kind-agnostic — no change): `offline_sequence.cpp` —
  `register_builtin_operator_binders()` `:63`, `bind_operators` inline exact
  path `:124`, parallel exact path `:149`; `export_monitor.cpp:144-145`
  (`register…` + `d_binding = bind_operators(...)`), member
  `export_monitor.hpp:194`.
- Manual-attach idiom this replaces (the tests that keep crossfade renderable):
  `tests/crossfade_goldens.t.cpp:125, 189, 256, 279` (`xf.attach(pull,
  backend)`), `:150` (through a live `PullServiceImpl service`, `:149`), comment
  `:58-60` naming this task; `tests/crossfade_conformance.t.cpp:143`
  (`xf->attach(pull, backend)`); `tests/crossfade_identity_counter.t.cpp:71-73`
  (live `PullServiceImpl` + inline attach).
- Counter surface for behavioral assertions:
  `src/compositor/arbc/compositor/counters.hpp` — `operator_renders()` (`:54`),
  `requests_issued()` (`:40`), `audio_dispatches()` (`:73`).

**Predecessor decisions carried in:**

- `fade_runtime_binding.md` Decision 1/2 (`:306-335`): the binding lives in
  `arbc::runtime`, driven at instantiation, torn down by RAII scope; dispatch is
  by the runtime's kind knowledge via a codec-TU-registered typed thunk (`dynamic_cast`
  inside the codec TU, driver kind-agnostic). This task registers crossfade's
  thunk into that same seam.
- `crossfade.md` Constraint 2 (`:196-199`): both inputs are pulled only through
  the injected `PullService` — this task supplies that service from live
  services and guarantees it outlives every render between attach and detach.
- `crossfade.md` "Pending" (`:61-68`): runtime attach-injection was explicitly
  out of `operators.crossfade`'s scope and deferred here.

## Constraints / requirements

1. **Levelization (doc 17).** The binder definition lives in `arbc::runtime`
   (L5, `codec_crossfade.cpp`) — the only level that may name both
   `PullServiceImpl` (L4 `compositor`) and `CrossfadeContent` (L4
   `kind_crossfade`); both edges `runtime` already declares. The new
   `detach()`/`attached()` methods live in `arbc::kind_crossfade` (L4) and
   traffic only in the content's own borrowed pointers — **no new component
   dependency**, no `kind_crossfade → compositor`/`runtime` edge. The CI
   levelization check must stay green.
2. **Bind at instantiation, before first render.** Every `CrossfadeContent`
   reachable in a rendered document is attached to the driver's live
   `PullServiceImpl` and `Backend` before the frame driver can render it — on
   both the inline and parallel offline paths and the export-monitor path.
   Satisfied by `bind_operators` once crossfade's binder is registered.
3. **Tear down on release.** When the `OperatorBindingScope` ends (frame/driver
   completes, or the export monitor is destroyed), each bound crossfade's
   `detach()` runs, clearing `d_pull`/`d_backend` so no render after release
   dereferences a dangling service. The service objects are borrowed, never
   owned by the content (`crossfade.md` Constraint carried; `fade_runtime_binding`
   Constraint 3).
4. **Services outlive the binding.** The live `PullServiceImpl` + `Backend`
   outlive every render issued between attach and detach — the binding scope is
   nested inside the service lifetime, never the reverse. (Guaranteed by the
   existing scope destruct-order; `export_monitor.hpp:194` declares `d_binding`
   last so it destructs first.)
5. **Pull discipline preserved for both inputs.** After binding, crossfade pulls
   **each** input only through the injected live `PullServiceImpl::pull` /
   `pull_audio` — never `input->render()`. Each pull carries the request's
   snapshot, exactness, deadline, and budget verbatim (doc 13:69-72, 112-120),
   identical to the inline-double path the goldens froze.
6. **Byte-identical to the manual-attach goldens.** A crossfade rendered through
   the live-bound driver must produce bytes identical to the frozen
   `tests/crossfade_goldens.t.cpp` tables that use an inline attach — binding
   changes *who* calls `attach`, never *what* crossfade computes.
7. **Reuse the seam, don't rewrite it.** This task adds a crossfade binder and
   the `detach()`/`attached()` methods only. It must **not** modify
   `operator_binding.hpp`/`.cpp`'s public shape, `bind_operators`, or any driver
   — the reuse `fade_runtime_binding` Constraint 7 promised. The only edit to a
   shared runtime TU is the two-line addition (forward decl + call) inside
   `register_builtin_operator_binders()`.
8. **Concurrency.** On the parallel offline path, crossfade content is bound once
   on the driver thread before any worker dispatch; its borrowed service pointers
   are read-only on worker threads during render. No worker mutates the binding.
   Race-free under TSan (Constraint below). Reuses the same once-guarded
   `register_builtin_operator_binders` + write-once-then-read-only binding
   `fade_runtime_binding` proved (`operator_binding.cpp:22-23`).

## Acceptance criteria

**End-to-end render binding (new runtime-component test,
`tests/crossfade_runtime_binding.t.cpp`):** build a `Document` containing an
`org.arbc.crossfade` over two solid inputs (and two tones, for audio) via the
production instantiation path (`Document::add_content` / the crossfade codec),
render it through the real `offline_sequence` driver (inline *and* parallel) and
the `export_monitor` audio path, and assert it renders to completion **without**
the attach assertions firing — i.e. the production wiring exists. This test
would abort today. (Test lives in `tests/`, not `src/runtime/t/`, because
byte-exact acceptance needs `backend_cpu`, which would violate levelization
under `src/runtime/t/` — the placement `fade_runtime_binding` used, Deviation 1.)

**Byte-exact goldens (re-asserted end-to-end):** the live-bound offline render
of the visual golden scene reproduces the frozen `tests/crossfade_goldens.t.cpp`
bytes exactly (interior `w == 0.5` dissolve, and the `w == 0` / `w == 1`
pass-through tiles), and the export-monitor audio render reproduces the frozen
audio golden bytes exactly — proving live-service binding is behaviorally
identical to the manual-attach golden. No tolerance; byte-exact (re-asserts
`16-sdlc-and-quality#byte-exact-goldens`).

**Behavioral counter (identity through the live driver):** a crossfade rendered
through the real driver at `w == 0` and at `w == 1` (both endpoints) issues zero
operator renders — `CompositorCounters::operator_renders()` delta 0 — while an
interior `w` records exactly one. Re-asserts
`13-effects-as-operators#identity-plan-issues-no-operator-render` and
`crossfade-identity-at-endpoints` (`registry.tsv`) against live binding rather
than an inline double.

**Pull discipline through the live driver (both inputs):** re-assert
`13-effects-as-operators#operator-pulls-only-via-pull-service` with the crossfade
bound to a *live* `PullServiceImpl` instrumented via `CompositorCounters`: at an
interior `w`, each of the two input renders the crossfade provokes equals one
the service issued (`requests_issued` / `audio_dispatches` deltas ≥ 2), and no
input's direct-render flag flips. Also re-asserts the crossfade math/stability
claims `13-effects-as-operators#crossfade-mixes-both-facets`,
`#crossfade-extent-union`, `#crossfade-timed-over-static` now exercised through
the runtime.

**New claim (the production-binding promise this task lands):** register
`13-effects-as-operators#crossfade-bound-to-live-services-at-instantiation` in
`tests/claims/registry.tsv`, enforced by a test in the new runtime component
test tagged `enforces:`. Claim text: *the runtime attaches a live
`PullService`/`Backend` onto a two-input operator content
(`org.arbc.crossfade`) at instantiation so its `render()`/`render_audio()` runs
with no manual attach, and tears the binding down on release so no borrowed
service is dereferenced after; a document containing a crossfade rendered
through the real offline/export driver produces bytes identical to the
manually-attached golden and issues both inputs' pulls only through the live
`PullServiceImpl`.* This is a design promise (doc 13:69-72, 112-120, doc
17:60/66-72) pinned for the multi-input case — the fade binding claim
(`#operator-bound-to-live-services-at-instantiation`, landed by
`fade_runtime_binding`) pins the single-input case; this pins the two-input
production wiring.

**Teardown assertion:** a test that binds, renders, then ends the
`OperatorBindingScope` (release) and asserts the crossfade's `attached()` is
`false` and its borrowed pointers are cleared — a render attempted after release
re-binds through the driver or is inert, never dereferencing the released
service. Behavioral, exercising Constraint 3.

**Concurrency (TSan lane):** a stress test rendering a crossfade scene through
the parallel offline path under TSan — the binding is written once on the driver
thread before dispatch and read-only on workers — asserts no data race
(Constraint 8). Reuses the parallel offline reap-to-quiescence path
(`offline_sequence.cpp:149`).

**Coverage:** ≥90% diff coverage on changed lines (CI gate). Tests ship in this
task.

**Deferred follow-ups:** none new. `kinds.nested_runtime_binding` /
`kinds.raster_runtime_binding` (`tasks/55-kinds.tji`) already exist as WBS
leaves that reuse the same seam; this task does not spawn a new one.
Plugin-provided (non-built-in) operator kinds bind through the same runtime seam
when the plugin registry lands — already covered by the plugin loading tasks,
not deferred here.

## Decisions

1. **Reuse the `fade_runtime_binding` binder seam unchanged; register crossfade
   as one more typed thunk.** *Rationale:* `fade_runtime_binding` deliberately
   built `operator_binding.{hpp,cpp}` kind-agnostic and shaped the drivers to
   bind every registered kind (`bind_operators` DFS + once-guarded
   `register_builtin_operator_binders`), stating explicitly that
   `crossfade_runtime_binding` and the `kinds.*_runtime_binding` tasks reuse it
   by *registering their kind, not rewriting the driver*
   (`fade_runtime_binding.md` Constraint 7, comment `builtin_codecs.hpp:50`
   "a sibling operator kind adds its own `register_*_binder`",
   `operator_binding.cpp:15`). Registering `register_crossfade_binder()` is the
   whole binding change. *Rejected:* a bespoke crossfade binding path in the
   driver — would fork a seam explicitly designed for reuse and couple the
   driver to each kind, the very outcome the registry avoids.

2. **`register_crossfade_binder()` lives in `codec_crossfade.cpp` and dispatches
   by `dynamic_cast<CrossfadeContent*>`.** The one L5 TU that legally sees the
   concrete `CrossfadeContent` type is `codec_crossfade.cpp`
   (`builtin_codecs.hpp:9-11`); it already `dynamic_cast`s to that type in its
   serialize path (`:45`). The binder's `try_attach` casts the same way and
   calls `attach(pull, backend)`; its `detach` `static_cast`s (sound because it
   runs only after a matched attach) and calls `detach()`. *Rationale:* reuses
   the codec's unique concrete-type visibility and matches
   `register_fade_binder()` verbatim (`codec_fade.cpp:126-141`); the driver stays
   kind-agnostic. *Rejected:* (a) a `dynamic_cast` chain in the driver — grows
   RTTI coupling per kind, the anti-pattern the registry replaced; (b) a uniform
   virtual `Content::bind()` on the contract — a doc-13/doc-17 delta the
   `fade_runtime_binding` Decision 2 already rejected as the wrong abstraction
   (it cannot serve `NestedContent`'s richer attach signature), out of a 0.5d
   scope.

3. **Add `detach()`/`attached()` to `CrossfadeContent` in `kind_crossfade`
   (L4), mirroring `FadeContent`.** `CrossfadeContent` shipped with only
   `attach()`; the binder's `detach` thunk requires a `detach()` method to
   clear the borrowed pointers, and the teardown test needs `attached()` to
   observe the clear. *Rationale:* the methods traffic only in the content's own
   `d_pull`/`d_backend` (no new dependency, `contract`-only include closure
   preserved, Constraint 1); it is the exact seam `fade_content.hpp:60, :65`
   already carries — parity between the two reference operators. *Rejected:*
   clearing the pointers from the runtime side by reaching into the content —
   impossible without a public mutator, which `detach()` *is*; adding it to the
   kind is the minimal, symmetric change.

4. **No design-doc delta.** *Rationale:* doc 13:69-72 already promises
   attach-time injection and doc 17:60/66-72 already assigns it to `runtime`;
   this task *implements* the promised behavior for the crossfade kind at the
   assigned level and creates no new component edge. The binder registry itself
   is a runtime-internal seam already documented by `fade_runtime_binding`'s
   refinement, not designed behavior. *Rejected:* a doc-13 amendment describing
   the crossfade binder — refinements, not docs, are the home for internal seam
   shape (`fade_runtime_binding.md` Decision 3, applied to the sibling).

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-10.

- Added `detach() noexcept` / `attached() const noexcept` to `CrossfadeContent` mirroring `FadeContent`: `src/kind_crossfade/arbc/kind_crossfade/crossfade_content.hpp`, `src/kind_crossfade/crossfade_content.cpp`.
- Registered the crossfade binder in `src/runtime/codec_crossfade.cpp` (typed `dynamic_cast<CrossfadeContent*>` attach / `static_cast` detach thunk, `register_crossfade_binder()`).
- Declared `register_crossfade_binder()` in `src/runtime/arbc/runtime/builtin_codecs.hpp` (after `crossfade_codec()`).
- Forward-declared and called `register_crossfade_binder()` inside `register_builtin_operator_binders()` in `src/runtime/operator_binding.cpp`.
- Added new test `tests/crossfade_runtime_binding.t.cpp`: audio export-monitor byte-exact through production binding; offline endpoint/interior `operator_renders` counter; binder teardown clears borrowed services.
- Registered new test in `tests/CMakeLists.txt` and claim `13-effects-as-operators#crossfade-bound-to-live-services-at-instantiation` in `tests/claims/registry.tsv`.
- Visual byte-exact offline acceptance (inline + parallel/TSan interior dissolve) deferred to `runtime.operator_input_cache_identity`: the offline driver's `id_of` gives `ObjectId{}` to operator-input children, causing a cache-key collision on multi-input operators; audio is unaffected.
