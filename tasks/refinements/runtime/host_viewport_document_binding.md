# runtime.host_viewport_document_binding — a host that owns a `Document` constructs a viewport against it

## TaskJuggler entry

`tasks/65-runtime.tji:111-116`:

```
task host_viewport_document_binding "Bind HostViewport directly against a Document" {
  effort 1d
  allocate team
  depends !async_external_load
  note "HostViewport takes a bare Model& + injected ContentResolver, so a host that owns a Document cannot construct one against it directly and must hand-roll the resolver and the settle hook. Add a HostViewport::Config factory (or constructor overload) that accepts a Document& and wires the resolver, settle hook, and damage sink automatically. Source-of-debt: tasks/refinements/runtime/async_external_load.md (implementer deviation from Decision 7). Docs 02/17."
}
```

Already wired into the milestone `depends` list at `tasks/99-milestones.tji:72`.

## Effort estimate

**1d.** The engineering is small because the seams all exist; the day goes to
getting the binding *right* and to writing the test `async_external_load`
Decision 7 promised and never landed.

1. **The delegating constructor** (~2h). A second `HostViewport` constructor
   taking `Document&`, delegating to the existing one with a derived
   `ContentResolver` and a derived settle hook. No new member, no second code
   path in `step()`.
2. **Reaching the `Model&`** (~1h). `Document` has no `Model` accessor; the
   attorney-client pattern it already blesses (`document.hpp:169-174`) grants
   one narrowly.
3. **The tests** (~4h). The Document-bound settle-across-a-frame case (Decision
   7's unwritten promise), the byte-exact equivalence against the hand-wired
   path, the two-viewport router fan-out over one `Document`, and the
   idle-viewport counter case.
4. **Claim + doc-01 delta** (~1h).

## Inherited dependencies

**Settled:**

- `runtime.async_external_load` (done 2026-07-11,
  `tasks/refinements/runtime/async_external_load.md`) — landed
  `HostViewport::Config::settle_external_loads` (`host_viewport.hpp:79-92`),
  `Document::set_damage_sink` (`document.hpp:109-119`),
  `Document::pending_external_loads()` (`document.hpp:151-158`), and the free
  function `settle_external_loads(Document&, KindBridge&, const Registry&)`
  (`document_serialize.hpp:180`). **This task is the debt that landing left.**
- `runtime.host_objects` — `HostViewport` itself: the `(anchor, camera)` pair,
  the owned `Transport`, the RAII `DamageAccumulator` install, the poll-style
  `StepOutcome` (`host_viewport.hpp:58-234`).
- `runtime.damage_router` — `DamageRouter` and its RAII `Registration`, already
  an optional field of `HostViewport::Config` (`host_viewport.hpp:71-77`).

**Pending, and deliberately not depended on:**

- `runtime.interactive_pull_wiring` (a null `PullService` on the interactive
  path) and `runtime.interactive_binder_wiring` (no `bind_operators` call per
  interactive frame). A Document-bound viewport still cannot render an
  *operator* layer — fade, crossfade, nested — to real pixels interactively,
  and this task does not change that. It is the same split
  `async_external_load` Decision 7 already reasoned through and accepted: the
  byte-exact placeholder→pixels proof lives on the offline path, which is
  wired; the interactive path proves the *revision and damage* outcome. Gating
  a 1d wiring task behind those two would buy a proof the offline path already
  gives.

## What this task is

`HostViewport`'s one constructor takes a `Model&` and an injected
`ContentResolver` (`host_viewport.hpp:109-111`). `Document` — the object a host
actually owns — exposes **no** `Model&`; its model is private behind a single
`friend struct DocumentSerializeAccess` (`document.hpp:174, :200`). The two
cannot be joined. A host holding a `Document` cannot construct a viewport
against it at all, and the seam `async_external_load` added to close the loop —
`Config::settle_external_loads` — has **zero callers in the tree**: production
has none, and the async tests call the free `settle_external_loads(doc, bridge,
registry)` directly (`tests/async_external_load.t.cpp:300, :368, :415, …`),
stepping around the viewport entirely.

So this task adds a second `HostViewport` constructor that takes a `Document&`
and derives, from the document itself, the three things the host is currently
expected to hand-assemble and cannot: the `ContentResolver` (`doc.resolve`), the
external-arrival settle hook (`settle_external_loads(doc, bridge, registry)`),
and the damage-sink install (`doc.set_damage_sink`, or the `DamageRouter`
registration when `Config::router` is set). The existing `Model&` constructor
stays, as the unit-test seam it already is.

It is **not**: a change to `step()`, to the frame loop, to the rebase math, or
to the transport policy — the new constructor *delegates* to the old one, so
there is exactly one code path through a frame, and every existing test's
behavior is byte-identical. It is **not** the operator-binding wiring
(`bind_operators` per frame) — that is `runtime.interactive_binder_wiring`, and
it hangs off this constructor when it lands. It is **not** a public `Model&`
accessor on `Document`.

## Why it needs to be done

Three things converge on it.

1. **The `async_external_load` seam is dead code today.** Decision 7 committed
   to wiring the settle call into a production frame path precisely "so the seam
   is not left with no production caller — dead code by the coverage gate's own
   reckoning". The implementer's deviation moved the call from
   `render_frame_interactive` onto `HostViewport::Config::settle_external_loads`
   — a reasonable move, since the settle needs the document's `KindBridge` and
   `Registry`, which the renderer has no business holding — but it landed the
   hook on an object no host can build against a `Document`. The seam ended up
   exactly where the decision said it must not: with no caller.

2. **Decision 7's promised test was never written.** It committed to "a test
   asserting the **pending count drops to zero and a new revision publishes**
   across a frame". No such test exists; the async tests drive the free
   function. The frame-integrated behavior — *an arrival is damage, and the
   frame that settles it composites it* — is unpinned.

3. **Doc 17 already colocates them.** `Document` and the viewport/transport
   objects are the *same* L5 component, `arbc::runtime` (doc 17:24, and 17:60:
   "`Document` (arenas + model + registry + loaders), viewport/transport/monitor
   objects, interactive frame loop"). There is no levelization reason for the
   gap. A component whose two headline objects cannot be connected to each other
   is the anomaly, not the design.

## Inputs / context

### Design docs (normative, doc 16)

- **`docs/design/01-core-concepts.md:91-113` (§ Viewport)** — the viewport is
  "(anchor, camera, transport)" observing a composition, and "Multiple viewports
  may observe the same composition simultaneously". The section says what a
  viewport *is* but, before this task's delta, never said what it is *bound to*.
  That silence is the doc-level shape of the gap.

- **`docs/design/02-architecture.md:49-52` (§ The frame, interactively, step 1)**
  — "**Collect damage.** Since the last frame: content damage, placement
  changes, camera changes. **No damage → no work.**" This is why the settle hook
  must run inside `step()` ahead of the damage drain, and why a document whose
  external child arrives with no reachable damage sink is invisible: the frame
  never wakes. `Document::set_damage_sink`'s own comment says so
  (`document.hpp:114-118`).

- **`docs/design/17-internal-components.md:24, :60`** — `Document` and
  viewports/transports are both L5 `arbc::runtime`. **The binding adds no
  levelization edge whatsoever**; it is an intra-component connection. This is
  the single strongest argument that a `Document&` constructor is the natural
  shape rather than an intrusion.

### Design-doc delta (this task, same commit — doc 16's rule)

`docs/design/01-core-concepts.md` § Viewport gains a paragraph (after the
multiple-viewports paragraph, :108-110) stating the promise this task makes
concrete — that a viewport observes a *document*, that the document supplies the
resolution / damage / settle seams rather than the host assembling them, and
that a host owning a document constructs a viewport directly against it and
"never needs a reference to the versioned model underneath". This is the
normative anchor for the new claim `01-core-concepts#viewport-binds-to-document`.

**No doc 00 decision-record bullet.** The delta makes an existing promise
explicit and adds a constructor overload inside one component; it is not
project-shaping in doc 00's sense (contrast `async_external_load`, which
introduced async model publishing and did earn a bullet).

### Source seams

| seam | file:line | change |
| --- | --- | --- |
| `HostViewport` ctor | `src/runtime/arbc/runtime/host_viewport.hpp:109-111` | unchanged; becomes the delegated-to target |
| `HostViewport::DocumentBinding` | `host_viewport.hpp` (new) | `{KindBridge* bridge; const Registry* registry;}` — both non-null ⇒ settle hook wired |
| `HostViewport(…, Document&, DocumentBinding, …)` | `host_viewport.hpp` / `host_viewport.cpp` (new) | the delegating constructor |
| `HostViewportDocumentAccess` | `host_viewport.cpp` (new attorney) | grants `Model&` from a `Document&`; mirrors `DocumentSerializeAccess` |
| `friend struct HostViewportDocumentAccess;` | `src/runtime/arbc/runtime/document.hpp:174` (beside the existing friend) | the only edit to `Document` |
| `Document::resolve` | `document.hpp:128` | the derived `ContentResolver` |
| `Document::set_damage_sink` | `document.hpp:119` | forwards to `d_model.set_damage_sink` — so the delegated ctor body already does the right thing unchanged |
| `settle_external_loads` | `document_serialize.hpp:180` | the derived settle hook |
| `Document::pending_external_loads()` | `document.hpp:158` | the counter the new test asserts against |
| `Config::settle_external_loads` | `host_viewport.hpp:79-92` | retained as the explicit-override escape hatch |

`ContentResolver` is `std::function<Content*(ObjectId)>`
(`src/compositor/arbc/compositor/compositor.hpp:32`), so `[&doc](ObjectId id) {
return doc.resolve(id); }` satisfies it directly.

### Tests / claims

- `src/runtime/t/host_viewport.t.cpp` — ten `Model&`-constructed viewports
  (`:209, :273, :331, :418, :533, :575, :628, :630, :710, :743`). **All must
  keep compiling and passing untouched**: they are the regression proof that the
  delegating constructor changed no behavior.
- `tests/host_viewport_reanchor_golden.t.cpp:95` — the eleventh, a byte-exact
  golden. Likewise untouched.
- `tests/async_external_load.t.cpp` — the deferring `AssetSource` test double
  (`:82-85`, `request`/`on_ready`) and the `Document` + `KindBridge` + `Registry`
  fixture the new test reuses.
- `tests/claims/registry.tsv` — `01-core-concepts#multiple-viewports-observe-one-composition`,
  `02-architecture#idle-viewport-issues-no-frames`,
  `01-core-concepts#viewport-step-drives-transport-damage-frame`,
  `05-recursive-composition#deferred-external-child-installs-live` are all
  re-enforced here; one new row is added.

## Constraints / requirements

1. **The existing `Model&` constructor survives, unchanged and public.** It is
   the unit-test seam: eleven call sites stand up a bare `Model` where a full
   `Document` (journal, editable binding, pending-loads queue) would be dead
   weight. Removing it in favor of a `Document&`-only shape would churn every
   one of them for no gain.

2. **The new constructor *delegates*; it does not fork `step()`.** No mode flag,
   no `Document*` member, no second path through a frame. `HostViewport`'s member
   set is unchanged. This is what makes "every existing test still passes,
   byte-identically" a cheap and total regression argument.

3. **`Document` gains a friend, not a `Model&` accessor.** A public `Model&
   Document::model()` would hand every host the unguarded model, letting it
   `add_content` straight onto the model and bypass the editable-facet
   registration, the journal wiring, and the captured-initial-state record that
   `Document::add_content` exists to perform (`document.hpp:40-52`). The
   attorney grants exactly one collaborator exactly what it needs — the pattern
   `document.hpp:169-174` already states and justifies for the serialize façade.

4. **The `Document` must outlive the viewport.** The derived resolver and settle
   hook capture `&doc`. This is the same lifetime contract the current
   constructor already carries for `Model&`, `Backend&`, `SurfacePool&`,
   `TileCache&`, and `Surface&` ("references, not owned", `host_viewport.hpp:105-107`)
   — no new obligation, but state it in the header.

5. **A settle hook is wired only when both `bridge` and `registry` are given.** A
   `DocumentBinding{}` — a programmatically-built document with no external refs
   — installs none, preserving `Config::settle_external_loads`'s promise that
   "a host with no external references pays nothing at all"
   (`host_viewport.hpp:91`). An explicitly-set `Config::settle_external_loads`
   wins over the derived one, so the escape hatch stays open.

6. **The router path and the single-slot path stay mutually exclusive.** When
   `Config::router` is set the viewport registers its accumulator with the
   router and does **not** touch the damage-sink slot; when null it installs
   directly. This is already true (`host_viewport.hpp:71-77`,
   `host_viewport.cpp:35-42`) and delegation preserves it for free — but the
   Document-bound path must be *tested* on both branches, because
   `Document::set_damage_sink` is the seam a host would otherwise reach for and
   double-installing would silently steal the router's slot.

7. **The settle hook runs first in `step()`, ahead of the pin and the damage
   drain.** Unchanged from `async_external_load` (`host_viewport.cpp:63-64`,
   before `d_model.current()` at `:89`) — an arrival *is* damage, so it must be
   in the set the same step drains (doc 02 step 1). The new test is what finally
   pins this ordering end to end.

8. **No new levelization edge.** `Document`, `settle_external_loads`,
   `KindBridge`, and `HostViewport` are all `arbc::runtime` (doc 17:24, :60).
   `Registry` is `contract`, which `runtime` already depends on. Forward-declare
   `Document`, `KindBridge`, and `Registry` in `host_viewport.hpp` and include
   `document_serialize.hpp` only in the `.cpp`, so the header's weight does not
   regress. `scripts/check_levels.py` must stay green.

## Acceptance criteria

### `src/runtime/t/host_viewport.t.cpp` (extended)

- **A Document-bound viewport resolves the document's contents with no
  host-supplied resolver.** Build a `Document`, `add_content` a solid, attach a
  layer, construct a `HostViewport` through the `Document&` constructor with
  `DocumentBinding{}`, `step()`. Assert: one frame issued
  (`frames_issued() == 1`), and the composited target bytes are **byte-identical**
  to the same scene driven through the `Model&` constructor with a hand-written
  `[&doc](ObjectId id){ return doc.resolve(id); }` resolver. The binding changes
  the wiring and nothing else. — *pins*
  `01-core-concepts#viewport-binds-to-document`.

- **The document's damage reaches the Document-bound viewport's frame.** After
  the first frame, `doc.set_layer_transform(...)` (which auto-damages,
  `01-core-concepts#placement-change-auto-damages`), then `step()`. Assert a
  second frame issues — i.e. the constructor installed the accumulator on the
  document's sink slot without the host calling `set_damage_sink` at all. —
  *pins* `01-core-concepts#viewport-binds-to-document`; *re-enforces*
  `01-core-concepts#viewport-step-drives-transport-damage-frame`.

- **An idle Document-bound viewport with a settle hook and an empty queue issues
  zero frames.** Construct with a full `DocumentBinding` over a document with no
  external references; `step()` repeatedly after the first frame. Assert
  `frames_issued()` does not rise and `external_loads_settled() == 0` — the
  settle hook is "cheap and safe to call on a document with nothing pending"
  (`document_serialize.hpp:178-179`) and, crucially, does **not** wake the frame
  loop by itself. — *re-enforces* `02-architecture#idle-viewport-issues-no-frames`.

- **Two Document-bound viewports over one `DamageRouter` both observe one
  commit.** Construct two through the `Document&` constructor with
  `Config::router` set, install the router on the document
  (`doc.set_damage_sink(&router)`), commit one edit, `step()` both. Assert both
  issue a frame, and that neither viewport stole the document's sink slot (the
  router still receives the next batch after one viewport is destroyed). —
  *re-enforces* `01-core-concepts#multiple-viewports-observe-one-composition`.

### `tests/async_external_load.t.cpp` (extended) — Decision 7's unwritten test

- **A deferred external arrival settles inside the frame that observes it.**
  Load a document whose nested content holds a `params.ref` served by the
  deferring `AssetSource` double (`:82-85`); assert
  `doc.pending_external_loads() == 1` and the child renders as the doc-05
  placeholder. Construct a `HostViewport` through the `Document&` constructor
  with `DocumentBinding{&bridge, &registry}` — **no manual settle call anywhere
  in the test**. `step()` once (baseline frame), fire the asset source, then
  `step()`. Assert: `doc.pending_external_loads()` is now **0**;
  `viewport.external_loads_settled() == 1`; the model's `revision()` **rose**;
  and a frame **issued on that same step** — the install's commit flushed damage
  naming the embedding content into the viewport's accumulator, which the same
  step drained (Constraint 7). This is verbatim the assertion Decision 7 promised
  ("the pending count drops to zero and a new revision publishes across a
  frame") and never landed. — *pins* `01-core-concepts#viewport-binds-to-document`;
  *re-enforces* `05-recursive-composition#deferred-external-child-installs-live`.

- **A deferring grandchild chain lands over successive frames, driven only by
  `step()`.** A child that itself holds a deferring external ref: fire, `step()`,
  fire, `step()`. Assert the pending count walks to zero across frames and
  `external_loads_settled() == 2` — the "host calls this once per frame and the
  chain lands over successive frames" contract (`document_serialize.hpp:172-175`),
  now actually driven by a frame. — *re-enforces*
  `05-recursive-composition#deferred-external-chain-and-cycle-terminate`.

The byte-exact **placeholder→pixels** proof stays where `async_external_load`
Decision 7 put it — on the offline/`SequenceRenderer` path
(`tests/async_external_load_golden.t.cpp`) — because the interactive path still
passes a null `PullService` and never calls `bind_operators`, so a *nested*
content renders blank there regardless of this task. That is
`runtime.interactive_pull_wiring` + `runtime.interactive_binder_wiring`, both
already WBS leaves; when they land, the interactive path inherits the pixels with
no further work here, because it is the same settle and the same damage.

### Claims register (`tests/claims/registry.tsv`)

One new row:

```
01-core-concepts#viewport-binds-to-document	A HostViewport constructed against a Document derives its content resolution, its damage-sink install, and its external-arrival settle hook from that document, with the host supplying none of them and never holding a reference to the versioned model: the viewport resolves the document's contents and composites them byte-identically to a hand-wired Model-and-resolver viewport; a commit's damage reaches its frame with no set_damage_sink call by the host; a deferred external arrival settles within the very step that observes it, dropping pending_external_loads() to zero, publishing a new revision, and issuing the frame that composites it; and a router-configured viewport registers with the DamageRouter instead of occupying the document's single sink slot
```

### Gates

- `scripts/check_levels.py` green (Constraint 8 — no new edge).
- `scripts/check_claims.py` green (the new row carries an `enforces:` tag).
- **≥90% diff coverage** on changed lines. Note that this task *raises* coverage
  on already-landed code: `Config::settle_external_loads` currently has no
  caller at all.
- `-Werror`, `clang-format`.

### Milestone

Already wired at `tasks/99-milestones.tji:72`. No milestone edit needed.

### Deferred

**(none.)** The task closes its own scope, and it closes `async_external_load`'s
too. No follow-up WBS leaf is registered: the interactive operator-pixel gap it
deliberately does not close is already carried by
`runtime.interactive_pull_wiring` and `runtime.interactive_binder_wiring`, which
exist, are wired, and need nothing from this refinement.

## Decisions

### 1. A constructor overload, not a `Config` factory — because `HostViewport` cannot be returned by value

The `.tji` note offers "a `HostViewport::Config` factory (or constructor
overload)". The choice is forced, not stylistic: `HostViewport` deletes its copy
*and* move constructors (`host_viewport.hpp:114-117`). A `make_host_viewport(doc,
…)` factory therefore cannot return one by value; it would have to return
`std::unique_ptr<HostViewport>`, imposing a heap allocation and an indirection on
an object designed to be a host's plain member — and every existing call site
constructs it in place (`arbc::HostViewport viewport(...)`, ten times over).

A `Config`-only factory (`Config cfg = HostViewport::config_for(doc, bridge,
registry);`) was the other reading. It fails on the resolver: `ContentResolver`
and `Model&` are *constructor parameters*, not `Config` fields
(`host_viewport.hpp:109-111`). A `Config` factory could fill in
`settle_external_loads` but could not supply the resolver or the model — so the
host would *still* be hand-rolling two of the three seams, and the `Model&` it
cannot obtain at all. It does not solve the stated problem.

*Rejected: a factory returning `unique_ptr`.* Heap-allocates a member-shaped
object to work around a deleted move that exists for good reason (the viewport
installs itself as a damage sink in its constructor and removes itself in its
destructor — an address-stable RAII object, `host_viewport.cpp:35-50`).

### 2. `Document` gains a second attorney, not a public `Model&`

`HostViewport` needs `Model&` for exactly two things: `set_damage_sink(&d_sink)`
(`host_viewport.cpp:42`) and `current()` (`:89`). `Document` publicly offers
*both* — `set_damage_sink` (`document.hpp:119`) and `pin()` (`:127`). So the
binding could in principle be built with no new access at all, by giving
`HostViewport` a `Document*` member and branching in `step()`.

That is rejected: it forks the frame path. Two modes through `step()` means the
eleven existing tests exercise one of them and the new host-facing path is the
*untested* one — precisely inverting what the regression suite is for. Constraint
2 is worth more than the avoided friend declaration.

So the new constructor delegates to the old, which needs the `Model&` up front,
in the member-init list. It gets it through `struct HostViewportDocumentAccess`,
defined in `host_viewport.cpp` and named as a friend beside the existing
`friend struct DocumentSerializeAccess` (`document.hpp:174`). This is not a new
pattern — `document.hpp:169-174` already spells out why the attorney is the right
shape here: "Granted through an attorney-client accessor … rather than a public
method, so `Document`'s public shape and member set stay unchanged."

*Rejected: a public `Model& Document::model()`.* It would let any host mutate the
model directly and bypass `Document::add_content`'s editable-facet registration,
journal wiring, and captured-initial-state record (`document.hpp:40-52`) — the
invariants the class exists to hold. Handing out the model to save one `friend`
line trades an enforced invariant for a convenience.

*Rejected: reusing `DocumentSerializeAccess`.* It is the serialize façade's, and
its name would then be a lie. A second, honestly-named attorney costs one line.

### 3. The bridge and registry ride a `DocumentBinding` parameter, not `Config`

The settle hook needs `KindBridge&` and `const Registry&`
(`document_serialize.hpp:180`) — which is exactly *why* the implementer reached
for a `std::function` in the first place (`host_viewport.hpp:87-90`: "A
`std::function` rather than a `Document*` because the settle needs the document's
`KindBridge` and `Registry` too"). The deviation's reasoning was sound; it just
stopped one step short of giving the host something to *call*.

Folding the two into `Config` was the obvious move and is rejected: they are
meaningless on the `Model&` path, and this header documents every field
scrupulously. A `Config` carrying two fields that a documented constructor
silently ignores is a trap.

So: `struct DocumentBinding { KindBridge* bridge{nullptr}; const Registry*
registry{nullptr}; };`, passed as one parameter to the `Document&` constructor.
Both non-null ⇒ the settle hook is derived; `DocumentBinding{}` ⇒ none, which is
correct for a programmatically-built document and preserves "a host with no
external references pays nothing at all" (`host_viewport.hpp:91`). Pointers, not
references, because absence is a real and common state. The parameter count stays
at nine, and the call site reads `{&bridge, &registry}`.

`Config::settle_external_loads`, if the host set it explicitly, wins over the
derived hook — the escape hatch for a host with a bespoke settle policy stays
open, and the seam `async_external_load` landed is not thrown away.

### 4. The claim is anchored in doc 01, and the doc-01 delta is what makes it a claim

A claim id must name a doc promise. Doc 01 § Viewport (`:91-113`) describes a
viewport as `(anchor, camera, transport)` observing a *composition* and never
says what it binds *to* — the doc-level silence that let the implementation grow
a viewport that cannot be attached to a document. The delta (Inputs / context)
states the promise: the document supplies resolution, damage, and settle; the
host assembles none of them and needs no model reference.

*Rejected: anchoring the claim in doc 02.* Doc 02 is the *frame*, and this is the
*binding* — doc 02's step 1 is a consumer of the damage seam, not the place the
viewport-to-document relationship is defined. Doc 01 is where the object model
lives, and doc 01 is where the gap was.

*Rejected: no delta, and re-enforcing existing claims only.* The core behavior —
"the host supplies none of the three seams" — is genuinely new and genuinely
promised to no one right now. It earns a row.

## Open questions

(none — all decided.)

## Status

**Done** — 2026-07-11.

- Added `HostViewport::DocumentBinding` struct and `HostViewport(renderer, Document&, DocumentBinding{…}, …)` delegating constructor in `src/runtime/arbc/runtime/host_viewport.hpp` and `src/runtime/host_viewport.cpp` — derives content resolver, damage-sink install, and external-arrival settle hook from the document; delegates to the existing `Model&` constructor with no fork in `step()`.
- Added `HostViewportDocumentAccess` attorney in `src/runtime/document_access.hpp` (new private header) — grants the `Model&` a `Document` owns, mirroring `DocumentSerializeAccess`; named as a friend in `src/runtime/arbc/runtime/document.hpp:174`.
- Extended `src/runtime/arbc/runtime/damage_router.hpp` and `src/runtime/damage_router.cpp` with a `DamageRouter(Document&)` delegating constructor — required for acceptance criterion 4 (two Document-bound viewports over one router), since a Document-owning host could not previously construct a `DamageRouter` directly.
- Added 4 unit tests in `src/runtime/t/host_viewport.t.cpp`: Document-bound viewport byte-identical to hand-wired Model+resolver; document damage reaches frame with no `set_damage_sink` call; idle Document-bound viewport with a settle hook issues zero frames; two Document-bound viewports fan out through one `DamageRouter`.
- Added 2 integration tests in `tests/async_external_load.t.cpp`: deferred external arrival settles inside the frame that observes it (Decision 7's unwritten test); deferring grandchild chain lands over successive frames driven only by `step()`.
- Updated `tests/async_external_load_golden.t.cpp` (minor adjustment for updated fixture).
- Added claim `01-core-concepts#viewport-binds-to-document` to `tests/claims/registry.tsv`.
- Applied doc delta to `docs/design/01-core-concepts.md` § Viewport — states that a viewport observes a document, which supplies resolution/damage/settle seams.
