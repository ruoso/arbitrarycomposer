# runtime.async_external_load — a deferring `AssetSource` installs the child on a later revision

## TaskJuggler entry

`tasks/65-runtime.tji:104-109`:

```tji
  task async_external_load "Deferring AssetSource for externally-loaded nested child" {
    effort 2d
    allocate team
    depends !nested_external_ref
    note "Drive a deferring AssetSource — one whose on_ready fires after
          load_document returns — installing the child composition on a later
          model revision and damaging the embedding content so the placeholder is
          replaced live. Doc 05:47-52 (external loading is async by nature) and
          doc 03 async render path.
          Source-of-debt: tasks/refinements/runtime/nested_external_ref.md.
          Docs 05/03."
  }
```

Wired into `m9_release` (`tasks/99-milestones.tji:72`) — the milestone carrying
the post-M8 runtime wiring work.

## Effort estimate

**2d.** The render side is already done: `NestedContent` renders a child id
that names no composition record as the empty placeholder today
(`src/kind_nested/nested_content.cpp:437-445`, whose comment already reads
"Unresolved / **not-yet-loaded** child (doc 05:50-52)"). The kind needs no
change, the compositor needs no change, and no new content type is introduced.

The 2d is spent in three places:

1. **Splitting one outcome into two** in the loader — today "no bytes came
   back" means *unavailable*; it must mean *unavailable* only when the source
   actually answered, and *pending* when it merely has not answered yet
   (`src/runtime/external_composition_loader.cpp:48-56`).
2. **Making the load-scoped state outlive the load.** Every object a deferred
   `on_ready` would need is a stack local of `runtime::load_document` —
   `LoadContext ctx`, `CodecTable codecs`, `ExternalCompositionLoader loader`,
   and the `sink` lambda (`src/runtime/document_serialize.cpp:424-475`). All
   four are destroyed before the callback fires. This is the real engineering
   core of the task.
3. **Marshalling the arrival onto the writer thread** and publishing it as one
   transaction that carries its own damage.

## Inherited dependencies

**Settled** (`runtime.nested_external_ref`, done 2026-07-11 — commit `40613a1`):

- `AssetSource` is the fetch seam and it **already permits deferral**:
  "Begin loading the asset at `resolved_uri`; `on_ready` is invoked with the
  loaded bytes (empty on failure/absence) once they are available.
  Non-blocking." (`src/serialize/arbc/serialize/load_context.hpp:35-38`). The
  interface does not change in this task.
- **Allocate-before-parse**: the loader takes the child's root `ObjectId` from
  `Model::allocate_id()` and records `resolved URI -> id` in its dedup map
  *before* parsing the child's bytes
  (`src/runtime/external_composition_loader.cpp:45-46`). This task's whole
  design is that rule, extended one step: the id is minted before the *fetch*,
  not merely before the *parse*.
- **"External" is provenance, not a runtime representation** (Decision 1): the
  child is an ordinary composition in the host document's own `Model`; render,
  audio, aggregate revision, damage routing and tile caching see no difference.
- **Unavailable is not an error** (Decision 6): null child, `ref` preserved,
  placeholder rendered, parent load succeeds.
- `serialize::load_composition` installs one document's composition graph into
  an existing model under a caller-seeded root id **through an ordinary
  transaction rather than `load_baseline`**
  (`src/serialize/arbc/serialize/reader.hpp:144-147`, and its header at
  `:128-130`). This is precisely the "install on a later revision" primitive —
  it already exists and already publishes a revision.
- `FilesystemAssetSource` (`src/runtime/arbc/runtime/filesystem_asset_source.hpp`)
  fires `on_ready` **inline**, and its header names this task by id as the
  successor that ships the deferring case (`:30-33`).

**Amended by this task** (see Decisions 2 and 6):

- `external_composition_loader.hpp:49-51` — "Owned by one load, driven on one
  thread… **It is not a shared service and is not stored on the `Document`**."
- `nested_external_ref.md` Constraint 10 — "Loading is single-threaded."

**Pending, and deliberately not depended on:**

- `runtime.interactive_pull_wiring` / `runtime.interactive_binder_wiring` —
  the interactive frame path passes a null `PullService` and never calls
  `bind_operators`, so nested renders blank *interactively* regardless of this
  task. The byte-exact "placeholder replaced by real pixels" proof therefore
  rides the offline/`SequenceRenderer` path, which **is** wired and is what
  `nested_external_ref`'s golden already uses. See Decision 7.

## What this task is

Today an external nested child only loads if the `AssetSource` answers *inside*
the load. `ExternalCompositionLoader::load` calls `ctx.load_asset(ref, …)` with
a lambda that assigns into a stack local, and reads that local on the very next
line (`src/runtime/external_composition_loader.cpp:48-50`). A source that
defers — a network fetch, a content-addressed store, anything that returns from
`request()` before it has bytes — leaves the local empty, and the loader reports
the reference **unavailable**: a null child, a permanent placeholder, and a
scene that never appears no matter how fast the bytes eventually arrive.

This task makes the deferred arrival land. The loader gains a third outcome —
**pending** — distinguished from *unavailable* not by the emptiness of the bytes
but by **whether `on_ready` fired at all** before `request()` returned. A pending
reference returns the child `ObjectId` the loader *already minted* under
allocate-before-parse, so the embedding `NestedContent` binds a **valid** child
id whose composition record simply does not exist yet, and the parent load
completes normally at revision 0. That state needs no new code to render: a child
id that resolves to no `CompositionRecord` is already the doc-05 placeholder
(`nested_content.cpp:148-150`, `:437-445`, `:783`).

When the bytes arrive, a writer-thread **settle** step installs the child's
composition graph under that same pre-allocated id via
`serialize::load_composition` — one ordinary transaction, which publishes a
**new revision** and, in the same commit, flushes damage naming the **embedding
content**. Doc 02's *Refine* step turns that damage into a follow-up frame, and
the placeholder is replaced live.

It is **not**: a new placeholder type, a mutation of `NestedContent` (which has
no `set_child` and gains none), a background thread of its own, or any change to
the `AssetSource` interface.

## Why it needs to be done

Doc 05 says external loading "is async by nature" and M8's external loader
satisfied that claim with a source driven to completion *inside* the load — which
is enough for a local file and not enough for anything else. Every real asset
source is deferring: an HTTP fetch, a content store, a plugin-supplied loader.
The `AssetSource` contract was written for them ("non-blocking… once they are
available"), and shipping an interface whose only working implementation must
answer synchronously means the contract is a lie the first time somebody
implements it.

It is also the last structural gap in doc 05's external story. `nested_external_ref`
proved external loading works end to end; this proves it works when the bytes are
*late*, which is the only case that distinguishes an async design from a
synchronous one wearing an async signature.

Downstream: any future non-filesystem `AssetSource` (the scheme-dispatch
extension point doc 08 Principle 3 stubs out), `kinds.raster_pool_backing`'s
externally-referenced image assets, and the out-of-process plugin isolation doc
03:204-207 defers — all inherit the writer-thread marshalling seam this task
builds.

## Inputs / context

### Design docs (normative, doc 16)

- **`docs/design/05-recursive-composition.md:56-61`** — the source line. As it
  stands it defers the not-yet-loaded state to "the nested kind's async render
  path plus placeholder policy", **neither of which exists**: doc 03 has no
  "async render path" section and the string "placeholder" does not appear in
  doc 03 at all. Doc 02:61-63's "placeholder policy (see doc 03)" is the same
  dangling pointer. This task rewrites 05's paragraph to state the real
  mechanism — see the delta below.
- **`docs/design/05-recursive-composition.md:63-74`** — "The loaded child is
  installed as an ordinary composition in the **host document's model**, so
  render, aggregate revision, damage routing and tile caching see no difference
  between an inline child and an external one." The install this task performs
  late is the *same* install, on a later revision.
- **`docs/design/02-architecture.md:69-71`** — *Refine*: "Async results that
  arrive later produce damage for their region, scheduling a follow-up frame.
  Zooming therefore shows progressively sharper content rather than blocking."
  This is the frame-level contract the arrival edge plugs into, and it is why
  the arrival must emit **damage** and not merely bump a revision (doc 02 step
  1: "No damage → no work").
- **`docs/design/02-architecture.md:51-52`** — damage is what wakes the frame.
- **`docs/design/14-data-model-and-editing.md`** (via `model.hpp:175`, "single
  writer, lock-free pinned reads") — the writer-thread confinement every
  publish path is annotated with.
- **`docs/design/17-internal-components.md:60`** — `arbc::runtime` is L5 and its
  contents are "`Document` (arenas + model + registry + **loaders**)". The
  pending-load state and the settle step belong in runtime by name.

### Design-doc delta (this task, same commit — doc 16's rule)

- **`docs/design/05-recursive-composition.md`** — the paragraph at `:56-61` is
  replaced. The dangling deferral to doc 03 is removed and the actual mechanism
  stated: the not-yet-loaded state is a **model** state, not a render-completion
  one; the loader mints the child id before fetching, so the embedding content
  binds a valid id immediately and the parent load never blocks; until the bytes
  land that id names no record, which is already the placeholder state; arrival
  installs the child under that same id in one ordinary transaction, publishing a
  new revision and flushing damage on the embedding content, which doc 02's
  *Refine* step turns into a follow-up frame. It also states the pending /
  unavailable distinction (one bit, loader-only; both render the placeholder,
  both keep their `ref`, both re-save byte-identically) and the threading line:
  the fetch may run on any thread, the install is marshalled onto the single
  model writer.
- **`docs/design/00-overview.md`** — a decision-record bullet ("Async external
  loading") in *Resolved questions*. This is project-shaping: it fixes what
  "not-yet-loaded" **means** system-wide — a model state, costing a revision bump
  and a damage route, rather than a second placeholder type or a new content
  state — and that answer governs every future `AssetSource` consumer, not just
  nested.

### Source seams

- `src/runtime/external_composition_loader.cpp:20-79` — `load()`. Lines `48-56`
  are the defect: `ctx.load_asset(ref, [&bytes](…){ … });` followed immediately
  by `if (bytes.empty()) { … return ObjectId{}; }`. The inline-firing assumption
  is load-bearing and undocumented at the call site.
- `src/runtime/arbc/runtime/external_composition_loader.hpp:91-100` — the state:
  `std::unordered_map<std::string, ObjectId> d_by_uri` (the resolved-identity
  dedup map) and `std::size_t d_depth` (the live recursion counter). Both must
  survive across the async boundary; see Decisions 2 and 5.
- `src/runtime/document_serialize.cpp:424-475` — the load assembly:
  `LoadContext ctx{base_uri}` (`:424`) → `ctx.set_asset_source(assets)` (`:425`)
  → `into.allocate_id()` for the host root (`:433`) → `CodecTable codecs` +
  `ExternalCompositionLoader loader(into, registry, codecs, sink)` (`:439-440`)
  → `loader.seed(ctx.base_uri(), root_composition)` (`:441`) →
  `nested_codec(&loader)` (`:465`) → `arbc::load_document(…)` (`:475`).
  **Every one of `ctx`, `codecs`, `loader`, `sink` is a stack local.**
- `src/runtime/arbc/runtime/document_serialize.hpp:145-148` — the host entry
  point, already carrying `std::string base_uri` and `AssetSource* assets`.
- `src/serialize/arbc/serialize/reader.hpp:144-147` — `load_composition`, the
  install-into-an-existing-model primitive, already transactional.
- `src/serialize/arbc/serialize/load_context.hpp:31-39` (`AssetSource`), `:94`
  (`load_asset`), `:60-61` ("single-writer, not thread-safe: a load runs on one
  thread").
- `src/runtime/codec_nested.cpp:121-122` — `const ObjectId child = (loader !=
  nullptr) ? loader->load(ctx, ref) : ObjectId{}; … NestedContent(child, ref)`.
  The codec is untouched by this task: it already binds whatever id the loader
  returns, and *pending* returns a valid one.
- `src/kind_nested/nested_content.cpp:148-150` (`ensure_memo`), `:437-445`
  (`render`), `:783` (audio) — all three resolve the child via
  `d_doc->find_composition(d_child)` and answer the empty placeholder on null.
  `:439` already says "Unresolved / **not-yet-loaded** child (doc 05:50-52)".
- `src/kind_nested/arbc/kind_nested/nested_content.hpp:284` — `ObjectId d_child`,
  set only by the two constructors; **there is no `set_child` mutator** and this
  task adds none (Decision 3).
- `src/model/arbc/model/model.hpp:264` — `allocate_id()`, "Any thread", a bare
  monotonic counter bump installing no record. `:261` — `current()`, "Any
  thread". `:454` — `Transaction::commit()`, WRITER-THREAD ONLY. `:446` —
  `Transaction::add_damage(const Damage&)`.
- `src/model/arbc/model/damage.hpp:19-25` (`struct Damage`), `:64-73`
  (`damage_add`: `Rect::infinite()` / `TimeRange::all()` are absorbing — the
  shape a "child just arrived" damage takes).
- `src/runtime/arbc/runtime/damage_router.hpp:28-34` — the router is
  writer-thread-confined and "adds NO shared mutable state and NO cross-thread
  channel". Constraint 4 below preserves that.
- `src/runtime/arbc/runtime/document.hpp:97` (`transact`), `:109` (`drain`),
  `:112-113` (`pin` / `resolve`), `:150` (`friend struct
  DocumentSerializeAccess` — the existing attorney-client seam the load façade
  already uses to reach the mutable `Model`).
- **Prior art for the marshalling pattern**: the compositor's refinement queue —
  async render results are produced off-thread and *inserted on the render
  thread* via `poll_refinements` (`tasks/refinements/runtime/threading.md:539-556`,
  "Workers never touch the cache; all cache mutation stays on the render
  thread"). The settle step is that pattern, one level down, at the model.

### Tests / claims

- `tests/nested_external_ref.t.cpp` — `MemoryAssetSource` (`:58-72`), an
  in-memory URI→bytes map with a request counter. The deferring double is its
  sibling.
- `tests/nested_external_ref_golden.t.cpp:242,:272,:314,:350` — the pixel golden
  against the in-document oracle, save-back-as-reference, and load-state
  invariance. The async golden asserts against the **same oracle**.
- `tests/document_serialize_concurrency.t.cpp:421-438` — the existing TSan lane
  for the external path.
- `tests/claims/registry.tsv:249-253` — the five claims `nested_external_ref`
  landed. `:251` (`#unresolvable-external-ref-renders-placeholder`) and `:252`
  (`#external-composition-ref-round-trips`, "the bytes do not depend on **LOAD
  state**… byte-identical") are the two this task must not break, and both get a
  second `enforces:` tag from the new pending-state tests.

## Constraints / requirements

1. **The `AssetSource` interface does not change.** It already permits deferral
   (`load_context.hpp:35-38`). A task that had to widen the interface to make
   async work would be admitting the interface was wrong; it isn't.

2. **Pending is distinguished by *whether `on_ready` fired*, never by the bytes
   being empty.** Empty bytes from a source that *did* answer is
   **unavailable** (a missing file) — today's behavior, unchanged, claim `:251`.
   No answer yet is **pending**. Conflating them is the exact bug this task
   fixes, so the flag is set inside the callback, not inferred afterwards.

3. **The parent load must still complete, at revision 0, without waiting.**
   A pending child may not block `load_document`, may not fail it, and may not
   leave the model at anything other than the baseline the synchronous path
   produces. A document whose every external child is pending loads exactly as
   fast as one with no children at all.

4. **The fetch may run on any thread; the install may not.** `Model::Transaction::commit()`
   (`model.hpp:454`), `set_damage_sink` (`:281`), and `DamageRouter::flush`
   (`damage_router.hpp:28-34`) are all WRITER-THREAD ONLY, and the router
   explicitly has "NO cross-thread channel". So `on_ready` must touch **no**
   model state: it copies the bytes and enqueues them. All parsing, installing,
   damaging and committing happens on the writer thread in the settle step. This
   is what lets `damage_router`'s Decision 5 ("no new concurrency obligation")
   stand unamended.

5. **`on_ready` receives a `std::string_view` valid only for the callback.**
   The completion queue **copies** into an owned `std::string` before returning.

6. **`on_ready` may fire after the `Document` is gone.** A network fetch can
   outlive the document that started it. The completion queue is held by
   `shared_ptr` and each callback captures a `weak_ptr`; a callback whose queue
   has expired drops its bytes and returns. No use-after-free, no crash, no
   resurrection of a dead document.

7. **Dedup and the depth cap must survive the async boundary.** Claim `:250`
   promises `b` is fetched exactly **once** across a cycle and that a hostile
   acyclic chain is capped at `k_external_ref_depth_cap = 64`. Both are
   properties of `d_by_uri` and `d_depth`, which today die with the load. The
   resolved-identity map must therefore outlive one load (Decision 2), and each
   pending entry must remember **the depth at which its reference was reached**
   so a chain that defers at every link is still capped at 64 total (Decision 5).

8. **Allocate-before-parse becomes allocate-before-*fetch*.** The id is recorded
   in the dedup map before `request()` is issued, so a back-edge reaching the URI
   while its bytes are still in flight resolves to the in-flight id and issues no
   second request. This is claim `:250`'s knot-cut, and it is what makes a
   *deferring* cycle terminate.

9. **Save bytes must not depend on load state.** Claim `:252` pins that a
   document saved with the widget file missing is byte-identical to one saved
   with it present. Pending is the **first** state where `composition_ref()` is
   valid while the composition record is *absent* — a writer that walked
   `composition_ref()` would emit a dangling `"composition": <id>`. The existing
   suppression is keyed on `external_composition_ref()` being non-empty
   (`src/serialize/writer.cpp:287,:337`; snapshot mirror
   `src/runtime/document_serialize.cpp:274`), which is true while pending — so
   this should already hold, and Acceptance pins it rather than assuming it.

10. **Levelization (doc 17).** The pending-load state, the settle step and the
    deferring test double all live in `arbc::runtime` (L5), which "depends on
    everything below" and whose contents are "`Document` (arenas + model +
    registry + loaders)" (`17:60`). `serialize` (L4) is not widened to know
    about pending loads; `kind-nested` (L4) is not touched at all.
    `scripts/check_levels.py` must stay silent.

11. **No new dependency** (doc 10). The queue is `std::mutex` + `std::vector`.

## Acceptance criteria

### `tests/async_external_load.t.cpp` (new) — behavioral, an in-memory deferring double

A `DeferringAssetSource`: records `(uri, on_ready)` on `request()` and fires
nothing; a `fire(uri)` / `fire_all()` the test calls when it chooses; counters
`requests()`, `outstanding()`. This is the whole point of the task made drivable
— arrival is a thing the *test* schedules, so nothing is timing-dependent
(behavioral counters, not wall-clock — doc 16).

- **A pending child leaves a valid id and an absent record.** Load a document
  whose nested content carries `params.ref`, with a deferring source. Assert:
  `load_document` **succeeds**; `model.revision() == 0`; the `NestedContent`'s
  `child()` is **valid**; `pin()->find_composition(child())` is **null**; the
  content's `ref()` is preserved; `describe`/`bounds`/`stability` are the empty
  placeholder; a render produces the placeholder and **does not fault**. This is
  the state doc 05 now names, asserted directly.
  — *pins* `05-recursive-composition#deferred-external-child-installs-live`.
- **Settling installs on a later revision and damages the embedding content.**
  `fire_all()`, then `settle_external_loads()`. Assert: `revision()` is **> 0**
  (a new revision, not a republished baseline — the host root id is unchanged
  and the host's own layers survive, proving `load_baseline` was not used); the
  child composition record now **resolves**; a `DamageSink` installed on the
  model recorded **exactly one** flush whose damage set names the **embedding
  content** id (whole-object, all-time); and the settle returned a count of 1.
  — *pins* the same claim.
- **A deferring cycle terminates and dedups.** `a.arbc → b.arbc → a.arbc`, both
  deferring. Assert: `b` is requested exactly **once**, `a` is **never**
  re-requested (its URI is seeded), the graph settles finite, and `b`'s nested
  child resolves to `a`'s **root** composition. The allocate-before-*fetch*
  knot-cut, asserted across the async boundary.
  — *pins* `05-recursive-composition#deferred-external-chain-and-cycle-terminate`.
- **A deferring grandchild chain lands across successive settles.** `a → b → c`,
  all deferring: `c`'s request cannot even be *issued* until `b`'s bytes are
  parsed, so the chain needs two settle rounds. Assert each round installs
  exactly one composition and the final graph is complete; assert `settle`
  loops its own ready queue to quiescence within a round.
  — *pins* the same claim.
- **The depth cap counts pre-deferral depth.** A hostile acyclic chain of 70
  deferring links caps at `k_external_ref_depth_cap = 64`: link 65 is
  **unavailable** (null child, placeholder), no further requests are issued, and
  the loader does not recurse without bound. Proves the per-entry depth is
  restored at settle rather than reset to 0 — the bug a naive queue would ship.
  — *pins* the same claim.
- **Unavailable is still unavailable.** A deferring source that fires with
  **empty** bytes yields a **null** child, not a pending one; a source that never
  fires at all leaves the child pending forever and the document still saves,
  renders and closes cleanly. The two-outcomes-become-three split, asserted from
  both sides.
  — *re-enforces* `05-recursive-composition#unresolvable-external-ref-renders-placeholder`
  (second `enforces:` tag, no new row).
- **A callback outliving its document is safe.** Destroy the `Document`, *then*
  `fire_all()`. Assert no fault and no install (the `weak_ptr` fails to lock).
  Runs under ASan in the existing lane.
  — *pins* the same chain-and-cycle claim (its lifetime half).

### `tests/async_external_load_golden.t.cpp` (new) — byte-exact pixels

- **The placeholder is replaced by the real pixels, live.** A scene embedding an
  external child through a **deferring** source: render before settle → matches
  the placeholder (empty). `fire_all()`; settle; re-render → **byte-identical**
  to the same scene loaded through the *inline* `FilesystemAssetSource`, and to
  the in-document oracle `nested_external_ref_golden.t.cpp` already builds. Not a
  hand-typed constant — the oracle is the same scene authored in-document, which
  is doc 05's "no difference between an inline child and an external one"
  asserted at the pixel, now across the async boundary.
  — *pins* `05-recursive-composition#deferred-external-child-installs-live`;
  *re-enforces* `05-recursive-composition#external-nested-loads-through-loadcontext`.
- **The metadata memo re-keys exactly once.** `bounds()` / `stability()` /
  `time_extent()` report the empty placeholder while pending and the child's real
  values after settle, with the memo recomputing **once** across the transition
  (the new revision re-keys it — `nested_content.hpp:126`, `:236`) and **zero**
  times on repeated queries either side of it.
  — *re-enforces* `05-recursive-composition#nested-metadata-memoized-on-aggregate-revision`.
- **Save-while-pending is a fixed point.** Save the document while the child is
  pending. Assert byte-identical to saving it **loaded**, and to saving it
  **missing** — the authored URI, never a `"composition"` id, never a
  `compositions` entry for the child. Constraint 9, asserted; this is the one
  place the new valid-id-with-absent-record state could have leaked into the
  format.
  — *re-enforces* `08-serialization#external-composition-ref-round-trips`.

### `tests/document_serialize_concurrency.t.cpp` (extended) — TSan/stress

The task introduces exactly one cross-thread channel (the completion queue), so
it owes exactly one lane — and must say so rather than inherit
`damage_router`'s "no new concurrency obligation" by silence.

- **N deferring sources firing `on_ready` from N threads** while the writer
  thread settles, renders and saves in a loop. Assert: TSan clean; every child
  lands exactly once; the request count is one per resolved URI (no double-fetch
  under concurrent arrival); no torn read of the dedup map. Run in the existing
  TSan lane.
- **Arrival racing document teardown**: fire callbacks from a worker while the
  main thread destroys the `Document`. ASan + TSan clean, no install after
  teardown (Constraint 6).

### Claims register (`tests/claims/registry.tsv`)

Two new rows:

- `05-recursive-composition#deferred-external-child-installs-live` — A deferring
  `AssetSource`, whose `on_ready` fires after `load_document` has already
  returned, leaves the embedding `NestedContent` holding a **valid** child
  `ObjectId` that names no `CompositionRecord`: the parent load succeeds at
  revision 0 without blocking, and the content describes and renders as the
  doc-05 placeholder — pending, not unavailable, and not an error. When the bytes
  arrive, the writer-thread settle installs the child's composition graph under
  that **same pre-allocated id** through one ordinary transaction, publishing a
  **new revision** (never a republished baseline) and flushing, in that same
  commit, damage naming the **embedding content** — which doc 02's *Refine* step
  turns into a follow-up frame. The scene then renders **byte-identically** to
  the same scene loaded through an inline source and to the in-document oracle,
  and the metadata memo re-keys exactly once across the transition. Empty bytes
  from a source that *did* answer remain unavailable (a null child), so the
  pending/unavailable split is a property of whether the source answered, never
  of the bytes being empty (doc 05, doc 02:69-71).
- `05-recursive-composition#deferred-external-chain-and-cycle-terminate` — Dedup
  and the depth cap survive the async boundary, because the child's `ObjectId` is
  recorded in the resolved-identity map before the **fetch** is issued, not merely
  before the parse: a deferring `a → b → a` cycle terminates with `b` fetched
  exactly once and `a` never re-fetched, resolving `b`'s child to `a`'s root; a
  deferring `a → b → c` chain lands across successive settles (a grandchild's
  request cannot be issued until its parent's bytes are parsed) with each settle
  draining its own ready queue to quiescence; and a hostile acyclic chain of
  deferring links is still capped at `k_external_ref_depth_cap`, because each
  pending entry restores the depth at which its reference was reached rather than
  resuming from zero. A callback firing after its `Document` is destroyed drops
  its bytes and faults nothing (doc 05, doc 08 Principle 7).

Re-enforced with a second `enforces:` tag, **no new row**: `:249`
`#external-nested-loads-through-loadcontext`, `:251`
`#unresolvable-external-ref-renders-placeholder`, `:252`
`#external-composition-ref-round-trips`, `:157`
`#nested-metadata-memoized-on-aggregate-revision`, `:219`
`08-serialization#loadcontext-dedups-by-resolved-identity`.

### Gates

- `scripts/check_levels.py` silent (the new state is L5-only).
- `scripts/check_claims.py` silent (both new rows enforced).
- **≥90% diff coverage** on changed lines — including the settle call site added
  to the interactive frame path (Decision 7).
- `-Werror -Wpedantic` clean; `clang-format` clean; full suite green across
  gcc/clang × debug/release/asan/tsan/rtsan.
- Fuzz seed: a `.arbc` whose `params.ref` is never fired, added to
  `tests/fuzz/corpus/load_document/` — a pending load must not make the loader
  fault on hostile input (`08-serialization#loader-never-faults-on-hostile-input`).

### Milestone

`m9_release` (`tasks/99-milestones.tji:72`) already lists
`runtime.async_external_load`; no milestone edit is needed beyond `complete 100`.

### Deferred

**(none.)** The task closes its own scope: the settle seam is wired into the one
driver that exists for it (Decision 7), and no follow-up WBS leaf is registered.

## Decisions

### 1. Pending is a *valid child id with no record*, not a new state

The loader already mints the child's `ObjectId` before it does anything else
(allocate-before-parse, `external_composition_loader.cpp:45-46`). So on deferral
it has an id in hand and can simply **return it**. The `NestedContent` binds a
valid id; the composition record shows up later, under that same id.

This is free at the render layer. `NestedContent` resolves its child through
`d_doc->find_composition(d_child)` and answers the empty placeholder when that
returns null — in `ensure_memo` (`nested_content.cpp:148-150`), in `render`
(`:437-445`) and in the audio facet (`:783`). The `render` comment *already*
reads "Unresolved / **not-yet-loaded** child (doc 05:50-52)". The kind was
written for this state a task before it could occur.

*Rejected: a `PendingContent` placeholder type* substituted for the nested
content until the child lands. It would have to be swapped back out on arrival
(mutating `Document::d_contents` under a live binding), it would lose the `ref`
and the editable identity, and it would give the same pixels. `nested_external_ref`
Decision 6 rejected exactly this reasoning for the *unavailable* case ("the kind
is perfectly well known and its reference must stay live, editable and
re-resolvable"), and pending is strictly the weaker case.

*Rejected: a `NestedContent::set_child` mutator* called on arrival. It would make
`d_child` mutable-after-construction on a content read concurrently by render
workers — a data race the current design does not have — to express something the
id-minted-up-front approach expresses with no mutation at all. This is why the
task adds no `set_child` and no `Document` content-replace API.

### 2. The resolved-identity map moves to the `Document`; the loader stays load-scoped

Every load object is a stack local of `runtime::load_document`
(`document_serialize.cpp:424-475`) and is gone before a deferred `on_ready`
fires. Something must outlive the load — but not everything need.

What is genuinely durable is the **resolved-identity map + the pending set + the
completion queue**: a `PendingExternalLoads` object owned by the `Document`
(by `shared_ptr`, per Constraint 6), holding the `AssetSource*`, the base URI,
`unordered_map<std::string, ObjectId> by_uri`, the pending entries
`{child id, resolved uri, embedding content id, depth}`, and a mutex-guarded
completion queue of `{child id, bytes}`.

`ExternalCompositionLoader` keeps its character — constructed per load (and per
settle), driven on one thread, not a shared service — but borrows the durable map
instead of owning one. The amendment to
`external_composition_loader.hpp:49-51` is therefore narrow and honest: *the
dedup map outlives one load; the loader does not.*

The `CodecTable` and `ContentSink` a settle needs are **rebuilt** from the
`Document` by the same helper `load_document` uses — extracted from
`document_serialize.cpp` and called from both. Nothing extra is kept alive.

*Rejected: parking the whole `ExternalCompositionLoader` on the `Document`.* It
binds a `CodecTable&` and a `ContentSink&` that are load-scoped by nature; keeping
it would mean keeping them, or re-binding its members between loads — a mutable,
half-initialized shared service, which is precisely what its header says it is
not.

*Rejected: keeping everything load-scoped and having the deferred callback carry
copies.* The dedup map is **shared across the whole load tree** by construction;
a per-callback copy would let two pending references to one URI each fetch it,
breaking claim `:250`'s "fetched exactly ONCE".

### 3. The arrival is a `Model::Transaction`, and its damage rides the same commit

`serialize::load_composition` already installs into an existing model "through an
ordinary transaction rather than `load_baseline`" (`reader.hpp:128-130`) — the
primitive exists and already publishes a revision. Settle calls it with the
pending entry's child id.

The damage naming the **embedding content** must land in **that same commit**.
A commit that publishes a structural change with an empty damage set violates doc
02's contract that a revision carries the reason to re-render it, and doc 02 step
1's "No damage → no work" means the frame loop would simply never wake: the child
would be in the model and invisible until something unrelated damaged the scene.
So `load_composition` gains an optional damage parameter — a `std::span<const
Damage>` unioned into its transaction before commit — and runtime passes the
`Damage{embedding_content, Rect::infinite(), TimeRange::all()}` (`damage.hpp:19-25`,
`:64-73`: infinite/all are the absorbing, structural shape).

This is a level-appropriate widening: serialize is told "install this subtree and
publish this damage atomically with it", not "here is an embedding content", so it
learns nothing about nesting.

*Rejected: a second transaction after the install* (`doc.transact().add_damage(…).commit()`).
It publishes an intermediate revision whose damage set is empty despite
structurally changing what the parent renders, and makes "the placeholder is
replaced live" depend on an ordering argument between two commits rather than on
atomicity. Two revisions where one will do, for a worse invariant.

*Rejected: relying on the aggregate-revision fold alone.* Installing the child
does bump the nested content's aggregate revision, which re-keys its composed
tile (claim `:156`) — so the *cache* would do the right thing. But nothing would
have **woken the frame** to ask (doc 02 step 1). Explicit damage on the embedding
content is what schedules the follow-up frame, and it is what the `.tji` note
asks for in as many words.

### 4. `on_ready` enqueues; the writer thread installs

The fetch is the only part that may be off-thread. `Transaction::commit()`
(`model.hpp:454`) and `DamageRouter::flush` (`damage_router.hpp:28-34`) are
writer-thread-only, and the router "adds NO shared mutable state and NO
cross-thread channel" — an invariant worth keeping, since it is what lets
`damage_router` Decision 5 stand.

So `on_ready` copies the bytes into the mutex-guarded completion queue and
returns. It touches no `Model`, no `Document`, no `LoadContext`. A writer-thread
`settle_external_loads()` drains the queue, and it **loops to quiescence**: a
settled child may itself hold external refs whose bytes are already in the queue.

This is the compositor's refinement-queue pattern (`threading.md:539-556` —
"Workers never touch the cache; all cache mutation stays on the render thread"),
applied one level down, at the model. Reusing an established seam's shape beats
inventing a second concurrency idiom for the same problem.

*Rejected: installing directly from `on_ready` under a model write lock.* There
is no such lock — the model is single-writer by design (`model.hpp:175`, "single
writer, lock-free pinned reads"), and adding one to serve the loader would be the
largest architectural change in the project made for its smallest consumer.

*Rejected: a dedicated arrival thread.* The housekeeping thread is a drainer and
explicitly not a model writer (`housekeeping_thread.hpp:22-43`); a *new* thread
that writes the model would break the single-writer invariant just as surely as
the callback thread would.

### 5. Each pending entry remembers its depth; the cap counts pre-deferral depth

`d_depth` is a live recursion counter today (`external_composition_loader.cpp:25`,
`:66-69`) — correct for a synchronous descent, meaningless across an async
boundary, where the parse resumes on a fresh stack. If the depth reset to zero at
settle, a hostile chain of deferring links would be **uncapped**: every link would
look like depth 0, and claim `:250`'s "a hostile ACYCLIC chain… is capped" would
silently become false in exactly the configuration (a network source) where it
matters most.

So the pending entry carries the depth at which its reference was reached, and
settle restores it before parsing. The cap remains 64 **total links**, deferring
or not. The test asserting link 65 is unavailable is what keeps this honest — it
is the one place a plausible implementation ships a security regression.

### 6. The completion queue is `shared_ptr`, the callbacks hold `weak_ptr`

An `AssetSource` that reaches the network can fire long after the user closed the
document. The queue is owned by the `Document` via `shared_ptr`; each `on_ready`
closure captures a `weak_ptr` and returns immediately if the lock fails. Bytes for
a dead document are dropped.

*Rejected: requiring the `AssetSource` to outlive the `Document`, or to cancel in
its destructor.* It pushes a hard lifetime obligation onto every third-party
source implementer to save the core one `weak_ptr`, and it is unenforceable — the
first plugin to get it wrong is a use-after-free in the host.

### 7. `settle_external_loads()` is called from the interactive frame path now, and proven through the offline path

The call belongs at the top of the interactive frame, in doc 02's step 1 (collect
damage) — arrivals are damage, and this is where damage is collected. It is a
two-line addition to `render_frame_interactive` and it is added in this task, so
the seam is not left with no production caller.

But the interactive path cannot yet *prove* the pixel outcome: it passes a null
`PullService` and never calls `bind_operators`, so nested renders blank there
regardless of this task — the gaps `runtime.interactive_pull_wiring` and
`runtime.interactive_binder_wiring` exist to close. Blocking on them would gate a
2d task behind two others for a proof the offline path can already give.

So: the interactive call site is covered by a test asserting the **pending count
drops to zero and a new revision publishes** across a frame (which does not need
the binder), while the **byte-exact placeholder-replaced-by-pixels** proof rides
the offline/`SequenceRenderer` path — which is wired, and which is the path
`nested_external_ref_golden.t.cpp` already uses for exactly this comparison. When
`interactive_binder_wiring` lands, the interactive path inherits the behavior with
no further work here, because it is the same settle and the same damage.

*Rejected: deferring the frame-loop call site to a follow-up task.* It would ship
a seam with no production caller — dead code by the coverage gate's own reckoning
— and the follow-up would be a two-line edit dressed up as a WBS leaf.

### 8. No new `AssetSource` implementation ships

`FilesystemAssetSource` stays inline-firing: a local file read is not a blocking
wait, and its header already says so (`:27-33`). The deferring source is a **test
double**, because the production deferring source is whatever a host or plugin
supplies over a network — which this project does not have and should not invent
to prove a seam. The double drives arrival *on command*, which is strictly better
for testing than a real async source would be: no timing, no flake, no
wall-clock assertions (doc 16).

## Open questions

(none — all decided.)

## Status

**Done** — 2026-07-11.

- Created `src/runtime/arbc/runtime/pending_external_loads.hpp` and `src/runtime/pending_external_loads.cpp`: `PendingExternalLoads` (owned by `Document` via `shared_ptr`) holding the resolved-identity map, per-entry depth, and a mutex-guarded completion queue; `shared_ptr`/`weak_ptr` lifetime guard for post-teardown `on_ready` firings (Constraint 6).
- Modified `src/runtime/external_composition_loader.{cpp,hpp}`: split "no bytes" into *unavailable* (source answered, empty bytes) vs *pending* (source deferred, `on_ready` not yet fired); loader borrows the durable resolved-identity map from `PendingExternalLoads` rather than owning one.
- Modified `src/runtime/document_serialize.{cpp,hpp}`, `src/runtime/host_viewport.{cpp,hpp}`, and `src/runtime/arbc/runtime/host_viewport.hpp`: `settle_external_loads()` wired into `HostViewport::Config::settle_external_loads` (a `std::function<std::size_t()>`) called at the top of `HostViewport::step()`, ahead of pin and damage drain (Decision 7 deviation — not `render_frame_interactive` directly; `Document::set_damage_sink` added as the narrow seam for routing arrival damage).
- Modified `src/serialize/reader.{cpp,hpp}`: `load_composition` gains an optional `std::span<const Damage>` parameter so the arrival transaction carries embedding-content damage atomically in the same commit (Decision 3).
- Added `tests/async_external_load.t.cpp`: 9 behavioral cases (pending state, settle installs/damages, deferred cycles, grandchild chains, depth cap, unavailable/pending distinction, post-teardown safety); `tests/async_external_load_golden.t.cpp`: 3 golden cases (placeholder→pixels byte-identical to inline and in-document oracle, memo re-keys once, save-while-pending fixed point).
- Extended `tests/document_serialize_concurrency.t.cpp`: 2 TSan lanes (N-thread concurrent `on_ready` under a settling writer thread; arrival racing document teardown); extended `tests/fuzz/fuzz_target.hpp`; added `tests/fuzz/corpus/load_document/pending_external_ref.arbc`.
- Claims: two new rows (`#deferred-external-child-installs-live`, `#deferred-external-chain-and-cycle-terminate`) and second `enforces:` tags on five re-enforced claims in `tests/claims/registry.tsv`.
- Design-doc deltas: `docs/design/05-recursive-composition.md` (pending/unavailable mechanism, dangling doc-03 deferral removed); `docs/design/00-overview.md` (async external loading decision record).
