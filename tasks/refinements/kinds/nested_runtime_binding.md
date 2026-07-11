# kinds.nested_runtime_binding — org.arbc.nested runtime injection wiring

## TaskJuggler entry

[`tasks/55-kinds.tji:69-74`](../../55-kinds.tji):

```
task nested_runtime_binding "org.arbc.nested runtime injection wiring" {
  effort 1d
  allocate team
  depends !nested, model.content_binding
  note "When the runtime instantiates an org.arbc.nested content, wire its attach
  injection (PullService, Backend, id-to-Content resolver, pinned DocRoot) onto live
  services and tear down on release — closing the production wiring nested's tests
  drive manually. Docs 05/17. Source-of-debt: tasks/refinements/kinds/nested.md"
}
```

Milestone: `m9_release` (`tasks/99-milestones.tji:71`).

## Effort estimate

**1d.** The seam this task needs mostly exists: `operators.fade_runtime_binding` and
`operators.crossfade_runtime_binding` shipped the runtime binder registry, the RAII
teardown scope, and the graph walk. This task widens that seam's injected payload by
two services (resolver + pinned `DocRoot`), adds nested's missing `detach()`, adds the
typed thunk TU, and adds the levelization edge. The bulk of the day is the new
integration test and the memo-stability counter work described under Decision 3 —
which is where the one genuine hazard lives.

## Inherited dependencies

### Settled predecessors (all `complete 100`)

- **`kinds.nested`** (`tasks/refinements/kinds/nested.md`, done 2026-07-06) — landed
  `NestedContent`, its `attach` seam, and the memoized metadata. It registered *this*
  task as its own deferred debt and its header comment names it by id
  (`src/kind_nested/arbc/kind_nested/nested_content.hpp:16-20`).
- **`model.content_binding`** (`tasks/refinements/model/content_binding.md`, done
  2026-07-09) — landed the id→`Content*` side-map (`Document::resolve`,
  `src/runtime/document.cpp:121`) and the content enumeration surface
  (`Document::for_each_content`, `document.hpp:118`). These two *are* the resolver and
  the walk this task injects; nothing new is needed from the model.
- **`operators.fade_runtime_binding` / `operators.crossfade_runtime_binding`** — not
  in the `depends` list, but they shipped the seam this task extends
  (`src/runtime/arbc/runtime/operator_binding.hpp`). Its header comment already
  anticipates this task: *"a sibling runtime-binding task (`operators.crossfade_runtime_binding`,
  `kinds.*_runtime_binding`) registers its kind's thunk without touching any driver"*.
- **`kinds.raster_runtime_binding`** (done 2026-07-10) — the other `*_runtime_binding`
  sibling. It bound through the **contract `Editable` facet**, so `runtime` named no
  concrete kind. Nested has no such facet, so this task follows the fade/crossfade
  typed-thunk path instead, not raster's. Raster's refinement (`raster_runtime_binding.md:58-62`)
  says as much: *"It injects services (`PullService`/`Backend`/resolver/`DocRoot`), a
  different binding shape."*
- **`kinds.nested_working_space_conversion`** (done 2026-07-11, current HEAD) — nested's
  visual facet now covers heterogeneous working spaces and its audio facet has landed.
  Both ride the same four injected services, so both are covered by this wiring with no
  extra parameters.

### Pending (must not be assumed at implementation time)

- **`runtime.nested_codec`** (`tasks/65-runtime.tji:90-95`, M8, **not** complete) — there
  is no `src/runtime/codec_nested.cpp` today. Fade's binder thunk lives in its codec TU
  (`codec_fade.cpp:126-141`) because that is the runtime TU that legally names the
  concrete kind. Nested has no such TU, so this task creates one
  (`src/runtime/binder_nested.cpp`, Decision 5). Do **not** take a dependency on
  `runtime.nested_codec`; it may later fold that TU into `codec_nested.cpp`.
- **`runtime.interactive_pull_wiring`** (`tasks/65-runtime.tji:96-101`, open) — the
  interactive frame path passes a null `pulls` today, so it has no `PullService` to
  bind against. The interactive binding is therefore out of scope here and deferred
  (see Acceptance criteria).

## What this task is

When a driver renders a `Document` containing an `org.arbc.nested` content, the runtime
must hand that content its four render-time services — the live `PullService`, the
`Backend`, an id→`Content*` resolver, and the driver's **pinned** `DocRoot` — and clear
them again when the binding scope ends. Today nothing in `src/runtime/` so much as
mentions `NestedContent`: every one of the ~40 `attach(...)` call sites in the tree is a
test hand-rolling its own `std::unordered_map<ObjectId, Content*>` resolver and
hand-pinning `model.current()`. This task productionises that wiring by extending the
existing runtime binder registry so the injected payload carries the resolver and the
pin, registering nested's typed thunk into it, and giving `NestedContent` the
`detach()` the RAII scope requires.

## Why it needs to be done

Nested is unusable through the production drivers. `NestedContent::render` and its audio
facet both read `d_pull` / `d_backend` / `d_resolver` / `d_doc`, all null until someone
calls `attach`. `SequenceRenderer` and `ExportMonitor` call `bind_operators`, which
dispatches only to registered binders — and nested has none — so a nested layer in an
exported sequence renders through a null `PullService`, and a nested-of-tones scene
drained through `ExportMonitor` mixes silence. Every green test that proves otherwise
(`tests/nested_goldens.t.cpp`, `tests/nested_audio_goldens.t.cpp`, the conformance and
concurrency suites) proves it only about a hand-attached content. This is the last gap
between "nested works" and "nested works in the product", and it blocks `m9_release`.

## Inputs / context

### Governing design-doc sections (normative, doc 16)

- **doc 17:41-44** — *"A component may depend only on strictly lower levels. No
  same-level edges. The arrows above are the complete allowed set."* `kind_nested` is
  L4, `runtime` is L5; the wiring can only live in `runtime`.
- **doc 17:66-72** — *"The model stays free of the `Content` vtable (records hold opaque
  content slots; **binding happens in `runtime`**)."* The resolver is a runtime artifact
  by construction; that is exactly why nested takes it through `attach`.
- **doc 17:143-148** — *"Because the `Registry` factory is `ContentConfig -> unique_ptr<Content>`
  and carries no input edges or service handles, a kind that needs render-time services
  (`kind-nested`, `kind-fade`, `kind-crossfade`) is constructed **unattached** across the
  boundary and the **host** injects its `PullService` and `Backend` afterwards, exactly
  as the runtime binders do in-lib."* This task is the in-lib half of that sentence for
  nested. Note the doc already groups nested with fade and crossfade — the shared binder
  registry is the designed home, not an improvisation (Decision 1).
- **doc 05:15-16** — nested's metadata is memoized on the child's aggregate revision and
  a **newer pin re-keys** it. Load-bearing for Decision 3.
- **doc 05:71-75** — a Droste scene must be self-consistent *within a frame*: nested
  reads membership from the **pinned** document version, not from `model.current()`.
  This is why the pin, not the `Document`, is the injected parameter (Decision 2).
- **doc 13:69-71** — nested pulls every child through the `PullService`, never
  `input->render()`. The injected `pull` is that service.

### Real source seams

The kind side (`src/kind_nested/`, L4 — depends on `contract` only):

- `arbc/kind_nested/nested_content.hpp:21` — `using NestedResolver = std::function<Content*(ObjectId)>;`
- `arbc/kind_nested/nested_content.hpp:72` — the seam:
  `void attach(PullService& pull, Backend& backend, NestedResolver resolver, const DocRoot& doc);`
- `arbc/kind_nested/nested_content.hpp:192-196` — the borrowed state
  (`d_pull`, `d_backend`, `d_resolver`, `d_doc`), all null/empty until `attach`.
- `nested_content.cpp:42-50` — `attach` takes `d_mutex` (a `std::recursive_mutex`),
  stores the four, then unconditionally sets `d_memo.valid = false`. **The unconditional
  invalidation is the hazard** — see Decision 3.
- `arbc/kind_nested/nested_content.hpp:118` — `std::uint64_t metadata_recomputes() const noexcept;`
  the behavioral counter that pins Decision 3.
- **`NestedContent` has no `detach()`.** `FadeContent` does
  (`src/kind_fade/arbc/kind_fade/fade_content.hpp:60`). The scope requires one.

The runtime binder seam (`src/runtime/`, L5):

- `arbc/runtime/operator_binding.hpp:40-43` — `struct OperatorBinder { bool (*try_attach)(Content&, PullService&, Backend&); void (*detach)(Content&) noexcept; };`
  **Carries no resolver and no `DocRoot`** — the signature this task widens.
- `arbc/runtime/operator_binding.hpp:49` — `register_operator_binder(const char* kind_id, OperatorBinder)`
- `arbc/runtime/operator_binding.hpp:55` — `register_builtin_operator_binders()` (once, thread-safe)
- `arbc/runtime/operator_binding.hpp:62-95` — `class OperatorBindingScope` (RAII; `release()`,
  `record()`, `size()`) and `OperatorBindingScope bind_operators(const Document&, PullService&, Backend&);`
- `operator_binding.cpp:86-110` — the DFS. **Its visit order is already correct for
  nested and must stay that way**: it calls `try_attach` on a content *before* it
  recurses into `c->inputs()`. Nested's `inputs()` (`nested_content.cpp:208-214`) reads
  the memo, which is empty until the resolver and pin are attached — so attach-then-recurse
  is what lets the walk see through a nested boundary into the child composition's own
  contents. Reverse the two and a fade inside a nested child silently never binds.
- `operator_binding.cpp:16-17` — the forward decls, with the comment *"a sibling
  runtime-binding task adds its own `register_*_binder()` beside these calls."*
- `codec_fade.cpp:126-141` — `register_fade_binder()`, the typed-thunk idiom to copy
  (`dynamic_cast<FadeContent*>` → `attach`; `static_cast` → `detach`).
- `arbc/runtime/builtin_codecs.hpp:51,66` — where `register_fade_binder()` /
  `register_crossfade_binder()` are declared.

The services and the drivers:

- `Content* Document::resolve(ObjectId) const` — `arbc/runtime/document.hpp:112`. Signature-identical
  to `NestedResolver`; `ContentResolver` (`src/compositor/arbc/compositor/compositor.hpp:32`)
  is literally the same `std::function<Content*(ObjectId)>` type.
- `void Document::for_each_content(const std::function<void(Content*)>&) const` — `document.hpp:118`.
- `DocStatePtr Document::pin() const` — `document.hpp:111`; `using DocStatePtr = std::shared_ptr<const DocRoot>`
  (`src/model/arbc/model/model.hpp:117`).
- `SequenceRenderer` — pins **once for the whole export** (`offline_sequence.cpp:55-57`,
  `d_pinned`), registers the binders in its ctor (`:62`), and calls `bind_operators`
  **per frame** inside `render_frame_at`: `:120` (inline-exact path, against a per-frame
  `PullServiceImpl inline_pull`) and `:151` (parallel path, against a per-frame `pulls`).
  Per-frame bind against a never-changing pin — the exact shape Decision 3 addresses.
- `ExportMonitor` — pins in its ctor (`export_monitor.cpp:88`, `d_pinned`) and holds a
  **long-lived** scope as a member (`:135`, `export_monitor.hpp:194`).
- `src/runtime/CMakeLists.txt:24-25` — `DEPENDS ... kind_fade kind_crossfade`; **`kind_nested`
  is absent** and must be added. `scripts/check_levels.py:36-41` already whitelists
  `runtime → kind_nested`, so the edge is legal today and needs no policy change.

### The debt being closed (the manual wiring)

Every `attach` call site outside `nested_content.cpp:42` is a test. In-component:
`src/kind_nested/t/nested_meta.t.cpp` (resolver fixture at `:66-80`; attach at `:112,133,154,168,181,194,210,233`)
and `nested_audio_meta.t.cpp` (`:239 … :490`, 15 sites). Cross-component:
`tests/nested_goldens.t.cpp:124,275,514,562`, `nested_cache.t.cpp:92`,
`nested_conformance.t.cpp:81,159`, `nested_concurrency.t.cpp:96`, and the audio suites
(`nested_audio_goldens.t.cpp`, `nested_audio_resampling_goldens.t.cpp`,
`audio_lookahead_recursive.t.cpp`, and others). Decision 6 says which of these change:
**none of them.**

### Out-of-scope boundaries (do not touch)

- The interactive frame path (`host_viewport.cpp:76`, `interactive.cpp`). It never calls
  `bind_operators` at all today — fade and crossfade are equally unbound there — and it
  has no `PullService` to bind against until `runtime.interactive_pull_wiring` lands.
  Pre-existing, not nested's debt; deferred below.
- `NestedContent`'s render, audio, metadata, and conversion semantics. This task injects
  services; it changes no pixel and no sample. The one exception is the memo-validity
  predicate in `attach` (Decision 3), which is a strengthening.
- The `Registry`/`ContentFactory` construction path. Nested is constructed unattached by
  design (doc 17:143-148); this task does not give the factory service handles.

## Constraints / requirements

1. **The wiring lives in `runtime`.** `kind_nested` (L4) may not see `runtime` (L5)
   (doc 17:41-44). Add `kind_nested` to `src/runtime/CMakeLists.txt`'s `DEPENDS` —
   already permitted by `scripts/check_levels.py:36-41`.
2. **Only the thunk TU names the concrete kind.** `dynamic_cast<NestedContent*>` appears
   in exactly one runtime TU (`binder_nested.cpp`), mirroring `codec_fade.cpp:126-141`.
   `operator_binding.cpp`, the drivers, and every runtime public header stay
   kind-agnostic, dispatching only through the registry.
3. **The injected `DocRoot` is the driver's pin, not a fresh one.** Nested must read
   membership from the same snapshot the frame renders against, or a Droste scene is not
   self-consistent (doc 05:71-75). The binder must never call `document.pin()` itself.
4. **The pin outlives the binding.** Nested stores `const DocRoot* d_doc` — a borrowed
   pointer. The binding scope must own a `DocStatePtr` keeping that snapshot alive for
   at least as long as any content it attached.
5. **Attach before recursing into `inputs()`.** The DFS at `operator_binding.cpp:86-110`
   already does this; the ordering is now load-bearing (nested's `inputs()` is empty
   until attached) and must be pinned by a test, not just a comment.
6. **Re-binding the same pin must not re-key nested's memo.** The drivers bind per frame
   against a stable pin; an unconditional `d_memo.valid = false` would make
   `metadata_recomputes()` grow linearly with frame count, breaking the shipped claim
   `05-recursive-composition#nested-metadata-memoized-on-aggregate-revision` on the
   production path. See Decision 3.
7. **Teardown clears every borrowed pointer, exactly once per attach.** `NestedContent::detach()`
   is `noexcept`, takes `d_mutex`, and nulls `d_pull` / `d_backend` / `d_doc` and clears
   `d_resolver`. It must be safe to call on a never-attached content and to call twice.
8. **Writer-thread discipline.** `bind_operators` runs on the driver thread before any
   worker dispatch (`offline_sequence.cpp:151` binds, then dispatches); nested's borrowed
   pointers are read-only on workers thereafter. Attach/detach take nested's
   `recursive_mutex`; no new lock and no lock-ordering edge is introduced.
9. **No behaviour change for existing kinds.** Fade and crossfade thunks change signature
   only; their goldens and `tests/fade_runtime_binding.t.cpp` /
   `tests/crossfade_runtime_binding.t.cpp` must stay green with no golden churn.
10. **Diff coverage ≥90%** on changed lines (doc 16 CI gate).

## Acceptance criteria

### Integration test (the deliverable)

New `tests/nested_runtime_binding.t.cpp` (in `tests/`, **not** `src/runtime/t/` — a
`runtime/t/` TU including `<arbc/kind_nested/...>` violates `check_levels.py`, the
deviation `kinds.raster_runtime_binding` already hit and documented):

- **Production-path identity, byte-exact.** Build a `Document` holding an `org.arbc.nested`
  content over a child composition, render it through `SequenceRenderer` **without any
  manual `attach` call**, and assert the surface is byte-identical to the hand-attached
  render `tests/nested_goldens.t.cpp` already pins. Deterministic rendering → byte-exact
  golden, no tolerance (doc 16).
- **Audio through the production path, byte-exact.** Drain a nested-of-tones scene
  through `ExportMonitor` with no manual attach; assert samples byte-identical to the
  `tests/nested_audio_goldens.t.cpp` oracle. (Today this path mixes silence — nested's
  audio facet reads the same unattached `d_pull`.)
- **Transitive bind through the nested boundary.** A fade living *inside* the child
  composition must itself be bound — the direct witness for Constraint 5. Assert the
  fade renders its blend (not its unattached fallback) and that `OperatorBindingScope::size()`
  counts both the nested and the fade.
- **Teardown.** After `scope.release()`, every bound content's borrowed pointers are
  clear (`size() == 0`); a subsequent metadata query on the nested content does not
  dereference a stale `DocRoot`. Release is idempotent.
- **Inert path.** A document with no nested content binds zero nested thunks and renders
  byte-identically to today (the witness that the widened binder is inert for
  non-nested kinds).

### Claims register

One new entry in `tests/claims/registry.tsv`, with the tests above tagged
`enforces: 05-recursive-composition#nested-runtime-bound`:

> **`05-recursive-composition#nested-runtime-bound`** — The runtime attaches an
> `org.arbc.nested` content's four render-time services (the driver's live `PullService`
> and `Backend`, an id→`Content*` resolver, and the driver's **pinned** `DocRoot` — never
> a freshly-pinned one) when it binds a document's content graph, with no manual `attach`
> call and no concrete-kind dependency outside the single thunk TU; so a nested scene
> rendered through `SequenceRenderer` is byte-identical to the same scene attached by
> hand, and a nested-of-tones scene drained through `ExportMonitor` is byte-identical to
> the direct-mix oracle instead of silent. The walk attaches a content before enumerating
> its `inputs()`, so a content reachable only *through* a nested boundary (a fade in the
> child composition) binds too; and the binding tears down on scope release, clearing
> every borrowed pointer exactly once per attach, leaving the pinned snapshot alive for
> as long as any content it was injected into.

### Behavioral-counter assertions (doc 16 — counters, never wall-clock)

- `NestedContent::metadata_recomputes()` (`nested_content.hpp:118`) — **delta 0** across
  N frames rendered through `SequenceRenderer` at a stable revision (the per-frame
  re-bind must not re-key the memo, Constraint 6), and **exactly 1** after an edit +
  re-pin + re-bind. This case is *also* tagged
  `enforces: 05-recursive-composition#nested-metadata-memoized-on-aggregate-revision`
  — it extends that shipped claim onto the driver path, which is where it was previously
  unproven.
- `OperatorBindingScope::size()` — nonzero after a bind over a nested document, `0` after
  release; equal to the number of registered-kind contents including those reached only
  through the nested boundary.
- Pull counters — a depth-1 nested frame issues exactly one pull per visible child layer
  through the bound path, so `05-recursive-composition#nested-boundary-budget-flows-through`
  stays green driven from the runtime rather than from a hand-attached fixture.

### Conformance and regression

- `tests/nested_conformance.t.cpp` (the contract conformance suite for the kind) stays
  green **unchanged** — nested's own `attach` signature does not change (Decision 6), so
  the suite compiles untouched. Re-run it as the conformance gate for this task.
- `tests/nested_goldens.t.cpp`, `nested_cache.t.cpp`, `nested_audio_goldens.t.cpp`,
  `tests/fade_runtime_binding.t.cpp`, `tests/crossfade_runtime_binding.t.cpp` all stay
  byte-identically green.
- `tests/dual_build.t.cpp:171-187` exercises nested through the dlopen plugin path with a
  host-injected resolver — it must keep passing, confirming the `attach` ABI is unchanged.

### Concurrency

- Run the new integration test **and** `tests/nested_concurrency.t.cpp` under **TSan**.
  The binding happens on the driver thread before worker dispatch (Constraint 8); the
  TSan run is what proves no worker observes a torn `d_resolver` (a `std::function`,
  not a pointer) mid-attach.
- The `ExportMonitor` audio case must respect the existing serialization rule for
  cache-reading nested renders (nested contributors read the not-thread-safe `BlockCache`
  on workers). This task adds no new concurrency; assert the existing audio stress path
  stays green under TSan rather than introducing a parallel drain.

### Deferred follow-ups

- **`runtime.interactive_binder_wiring`** (~1d, milestone `m9_release`, depends
  `!interactive_pull_wiring`, `kinds.nested_runtime_binding`) — bind the document's
  operator/nested contents on the interactive frame path. `host_viewport.cpp:76` pins a
  `DocStatePtr` but never calls `bind_operators`, and `interactive.cpp` passes a null
  `pulls`, so fade, crossfade **and** nested all render unattached interactively. It
  cannot be done until `runtime.interactive_pull_wiring` supplies the `PullService`.
  *Deferred to `runtime.interactive_binder_wiring` (closer registers in WBS).*

## Decisions

1. **Extend the existing `OperatorBinder` registry rather than adding a second,
   nested-specific one.** One registry, one DFS, one dedup set, one RAII scope; nested
   registers a thunk exactly as fade does. Doc 17:143-148 already groups `kind-nested`
   with `kind-fade`/`kind-crossfade` as "kinds that need render-time services", and
   `operator_binding.hpp`'s own header comment reserves the seam for `kinds.*_runtime_binding`.
   *Alternative rejected:* a parallel `KindBinder` registry with a richer signature —
   it would need its own walk over the same graph, its own dedup, and its own teardown
   scope, and a content reachable by both walks would attach twice. The only thing it
   buys is leaving fade's thunk signature alone, which Decision 4 gets more cheaply.
   *Alternative rejected:* binding through a contract facet the way
   `kinds.raster_runtime_binding` did — nested's `attach` takes a `NestedResolver` and a
   `DocRoot`, neither of which the contract layer can name generically without inventing
   an "attachable" facet whose only implementor would be nested. One call site does not
   earn a facet.

2. **Widen the injected payload to a `BindContext`, and make the pin an explicit
   parameter of `bind_operators`.**

   ```cpp
   struct BindContext {
     PullService& pull;
     Backend& backend;
     const ContentResolver& resolve;  // compositor.hpp:32 — same type as NestedResolver
     const DocRoot& doc;              // the DRIVER's pin, per Constraint 3
   };
   struct OperatorBinder {
     bool (*try_attach)(Content& content, const BindContext& ctx);
     void (*detach)(Content& content) noexcept;
   };
   OperatorBindingScope bind_operators(const Document& document, PullService& pull,
                                       Backend& backend, DocStatePtr pin);
   ```

   `bind_operators` synthesizes the resolver internally from `Document::resolve` (which
   is exactly what `offline_sequence.cpp:79` and `export_monitor` already build by hand),
   so no driver gains a parameter it doesn't already own. The returned scope **stores the
   `DocStatePtr`**, satisfying Constraint 4 by construction: the pin cannot outlive its
   bindings, and no caller can forget to keep it alive. `ContentResolver` and
   `NestedResolver` are the same `std::function<Content*(ObjectId)>` type, so the thunk
   converts with no adapter.
   *Alternative rejected:* passing the `Document&` into `try_attach` and letting nested's
   thunk call `document.pin()`. That re-pins at bind time, which can be a *newer* revision
   than the frame is rendering — the exact snapshot-inconsistency doc 05:71-75 forbids.
   *Alternative rejected:* an optional/defaulted pin parameter to keep the 3-arg
   `bind_operators` call sites compiling. A missing pin would bind a nested content with a
   null `DocRoot` that renders empty *silently*; making the pin required means the
   compiler catches every driver that forgot it. Eight call sites is a cheap price.

3. **Make `NestedContent::attach` re-key the memo only on an actually-new pin.**
   `attach` currently ends with an unconditional `d_memo.valid = false`
   (`nested_content.cpp:49`). That is harmless for the hand-attached tests, which attach
   once — but `SequenceRenderer` calls `bind_operators` **per frame**
   (`offline_sequence.cpp:120,151`) against a pin taken **once for the whole export**
   (`:55-57`). Wiring nested naively would therefore invalidate the metadata memo on
   every frame of every export, making `metadata_recomputes()` grow linearly with frame
   count and breaking the shipped claim
   `05-recursive-composition#nested-metadata-memoized-on-aggregate-revision` precisely
   where it matters most. The fix is to invalidate only when the injected snapshot really
   differs:

   ```cpp
   const bool repinned = (d_doc != &doc) || (d_doc->revision() != doc.revision());
   ...
   if (repinned) { d_memo.valid = false; }
   ```

   This *preserves* doc 05:15-16's promise (a newer pin after an edit still re-keys) and
   only skips the re-key when the very same snapshot is re-injected. The memo is keyed on
   the child's aggregate revision anyway (nested.md Decision 5), so a same-pin re-attach
   provably cannot make it stale. Update the header comment at `nested_content.hpp:69-70`
   to state the tightened contract.
   *Alternative rejected:* hoisting `bind_operators` out of `render_frame_at` into the
   `SequenceRenderer` constructor so the bind happens once. It cannot be done: the inline
   path constructs a **per-frame** `PullServiceImpl inline_pull` (`offline_sequence.cpp:120`)
   whose lifetime is the frame, so there is no stable service to bind against up front.
   *Alternative rejected:* leaving `attach` alone and having the binder skip already-bound
   contents. The scope would have to carry cross-frame state, and a content bound to a
   *stale* per-frame `PullServiceImpl` would keep a dangling `d_pull` — trading a counter
   regression for a use-after-free.
   *Note:* this is a strengthening of an existing behaviour, not a deviation from a
   designed one — doc 05 promises revision-keyed memoization and this makes the runtime
   path finally *deliver* it. No design-doc delta is required (see Open questions).

4. **Add `NestedContent::detach() noexcept`, mirroring `FadeContent::detach()`
   (`fade_content.hpp:60`).** The RAII scope requires a detach thunk; nested has none.
   It takes `d_mutex`, nulls the three pointers, clears the `std::function`, and is safe
   on a never-attached content. It deliberately does **not** invalidate the memo: the
   memoized metadata is a pure function of the child's aggregate revision, and preserving
   it across a detach/re-attach cycle at the same revision is what makes Decision 3 hold
   across frames. This is additive to nested's public surface and breaks no existing test.

5. **Put `register_nested_binder()` in a new `src/runtime/binder_nested.cpp`, declared in
   `builtin_codecs.hpp` beside `register_fade_binder()` (`:51`) and
   `register_crossfade_binder()` (`:66`), and called from `register_builtin_operator_binders()`.**
   Fade's binder rides in its codec TU because that TU already legally names the concrete
   kind; nested has no codec TU (`runtime.nested_codec` is unshipped M8 work), so the
   binder gets its own. This keeps the "exactly one runtime TU names the kind" property
   (Constraint 2) without taking a dependency on an unlanded task.
   *Alternative rejected:* waiting for `runtime.nested_codec` to create `codec_nested.cpp`.
   That would put an M8 task in front of an M9 one for a file-placement reason, and
   `nested_codec` can trivially absorb `binder_nested.cpp` when it lands.

6. **Do not migrate the ~40 existing hand-attached test call sites.** The `.tji` note's
   "closing the production wiring nested's tests drive manually" means *the production
   path must stop depending on tests to do its wiring* — not that the kind's own tests
   should be rewritten. The in-component tests (`src/kind_nested/t/`) **cannot** use the
   runtime binder at all (L4 cannot see L5), and the cross-component suites test nested's
   render/audio/cache semantics, for which a hand-built fixture is the right level of
   isolation. `kinds.raster_runtime_binding` set exactly this precedent: it kept raster's
   tests and added one new integration test as the production-path witness.
   Nested's `attach` signature is therefore **unchanged**, and no existing test file is
   touched.
   *Alternative rejected:* rewriting the suites onto `Document` + `bind_operators` — it
   would couple the kind's semantic tests to the runtime, churn ~40 sites for no new
   coverage, and lose the ability to drive nested from stub services.

## Open questions

(none — all decided)

Two calls worth flagging as *deliberately* made rather than deferred:

- **No design-doc delta.** This task introduces no new dependency (doc 10), no new
  architectural seam (it widens one that ships), and no deviation from designed behaviour
  — doc 17:143-148 already prescribes exactly this "constructed unattached, host injects
  afterwards" wiring for `kind-nested`, and Decision 3 makes the runtime path *comply*
  with doc 05:15-16 rather than depart from it. The `check_levels.py` whitelist already
  permits the `runtime → kind_nested` edge, so even the levelization is pre-blessed.
- **The `OperatorBinder` / `bind_operators` names are kept**, even though nested is not an
  operator. The seam's real subject is "a content that needs render-time services
  injected", which doc 17:143-148 defines as `{kind-nested, kind-fade, kind-crossfade}`,
  and `operator_binding.hpp`'s header comment already promises the registry to
  `kinds.*_runtime_binding`. Renaming to `ContentBinder`/`bind_contents` would touch two
  shipped refinements' Status blocks and eight call sites for zero behavioural gain; the
  header comment is updated instead to say plainly that the registry binds operator *and*
  kind contents.

## Status

**Done** — 2026-07-11.

- New thunk TU `src/runtime/binder_nested.cpp` registers `register_nested_binder()` via a `dynamic_cast<NestedContent*>` → `attach` / `static_cast` → `detach` typed thunk, declared in `src/runtime/arbc/runtime/builtin_codecs.hpp` beside `register_fade_binder()` and `register_crossfade_binder()`, and called from `register_builtin_operator_binders()`.
- `OperatorBinder` widened to `struct BindContext { PullService&, Backend&, const ContentResolver&, const DocRoot& }` with `bind_operators(Document, PullService, Backend, DocStatePtr pin)` — scope owns the pin, satisfying Constraint 4 (`src/runtime/arbc/runtime/operator_binding.hpp`).
- `NestedContent::detach() noexcept` added (`src/kind_nested/arbc/kind_nested/nested_content.hpp`, `src/kind_nested/nested_content.cpp`); `attach` now re-keys the memo only when the injected snapshot differs from the current one, fixing the per-frame linear-growth regression (Decision 3) — snapshot key stored on the memo itself so `detach()`'s deliberate preservation survives.
- `src/runtime/CMakeLists.txt` gains the `kind_nested` dependency edge (already whitelisted by `scripts/check_levels.py`).
- Drivers updated: `src/runtime/offline_sequence.cpp` and `src/runtime/export_monitor.cpp` pass `d_pinned` as the explicit pin to `bind_operators`; `src/runtime/codec_fade.cpp` and `src/runtime/codec_crossfade.cpp` updated for the new `BindContext` thunk signature.
- `SequenceRenderer` parallel path now fans out only leaf contents to the worker pool — operator contents (fade, crossfade, nested) render inline on the driver thread, preventing a `TileCache`/`KeyedStore` data race that TSan confirmed was latent for all three kinds (`src/runtime/offline_sequence.cpp`).
- `NestedContent::render` now returns `exact=false` when any child pull deferred, preventing a missing-layer tile from being cached as fresh-exact (`src/kind_nested/nested_content.cpp`).
- Integration test `tests/nested_runtime_binding.t.cpp` (10 cases, 65 assertions): byte-exact `SequenceRenderer` inline+parallel vs hand-attached oracle; byte-exact `ExportMonitor` nested-of-tones vs direct-mix oracle; transitive fade bind through nested boundary; idempotent teardown; per-frame-rebind memo-stability counter; one-pull-per-child-layer counter; inert non-nested path; culled-scene cull branches; move-scope coverage.
- One new claim `05-recursive-composition#nested-runtime-bound` in `tests/claims/registry.tsv`; counter cases also tag `#nested-metadata-memoized-on-aggregate-revision` and `#nested-boundary-budget-flows-through`.
