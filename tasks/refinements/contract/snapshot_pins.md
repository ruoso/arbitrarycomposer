# contract.snapshot_pins — Snapshot pins in requests

## TaskJuggler entry

Back-link: `tasks/25-contract.tji:14-18`, `task snapshot_pins "Snapshot pins in requests"` under `task contract`.

> note "RenderRequest carries the pinned version / content StateHandle; rendering becomes a pure function of (state, region, scale, time). Docs 03/14."

## Effort estimate

**1d** (`tasks/25-contract.tji:15`). A small, well-fenced seam extension: one
field on an existing public struct, a pass-through at the single producer,
one contract-doc obligation, and its enforcing test. The heavy lifting —
the immutable `StateHandle`, the `content_state` resolver, and the
retain/release lifecycle — already landed in `model.editable_facet`; this
task only makes the render contract *carry and honor* that handle.

## Inherited dependencies

`snapshot_pins` declares no `depends` of its own, so it inherits the parent
`task contract` edges (`tasks/25-contract.tji:7`):
`depends model.editable_facet, surfaces.capabilities`.

**Settled:**

- `model.editable_facet` (`6a799ae`, DONE 2026-07-05,
  `tasks/refinements/model/editable_facet.md`). Froze the exact seams this
  task consumes:
  - `StateHandle` — an index-only, inert, trivially-copyable slab handle
    with sentinel `k_state_none`
    (`src/model/arbc/model/records.hpp:43-48`, sentinel at `:29`,
    static-asserts at `:124-125`), embedded in `ContentRecord.state`
    (`records.hpp:52-55`). It carries **no store identity and no type tag**
    by design — the content state's type and destructor are kind-owned (L3);
    the model only drives retain/release.
  - `DocRoot::content_state(ObjectId) const -> StateHandle`
    (`src/model/arbc/model/model.hpp:73-80`, impl `src/model/model.cpp:393-398`)
    — the L2 resolver. Its doc-comment names *this* task verbatim:
    "`contract.snapshot_pins` (L3) consumes this to place the handle on a
    `RenderRequest`."
  - `StateRefSink` retain/release + the pin→content-state lifecycle
    (`model.hpp:104-109`), and the pinned-version type
    `DocStatePtr = std::shared_ptr<const DocRoot>` (`model.hpp:91`). A pinned
    `DocState` keeps every content object's captured `StateHandle` live and
    resolvable (claims `14-data-model-and-editing#pin-holds-content-state`,
    `#pinned-version-never-observes-later-edit`,
    `#content-state-reclaimed-by-refcount`).
- `surfaces.capabilities` (`62ff4df`, DONE 2026-07-05,
  `tasks/refinements/surfaces/capabilities.md`). Not consumed directly by
  this task, but it completes the `Surface`/`Backend` contract the
  `RenderRequest.target` seam sits on.

**Context (not a formal `depends`, but the settled render substrate):**

- `surfaces.surface_pool` (`8063b91`, DONE 2026-07-05,
  `tasks/refinements/surfaces/surface_pool.md`). `SurfacePool::acquire(w, h,
  format)` recycles temp surfaces keyed by the exact `(w, h, format)` triple.
  This task does not touch the pool; it renders *into* the compositor's
  pooled temp (`src/compositor/compositor.cpp`). The determinism the pool
  preserves (compositor clears each recycled temp before render) is what
  keeps the purity golden byte-stable across pool hits and misses.

## What this task is

The walking-skeleton `RenderRequest`
(`src/contract/arbc/contract/content.hpp:20-25`) carries only `{region,
scale, time, target}`. Editable content, however, must render a *specific
frozen version* of its state, not "whatever the writer thread has now."
This task adds the missing input: a pinned content `StateHandle` on
`RenderRequest`, and pins down the corresponding obligation on
`Content::render` — that it renders *that* handle's state, making
`render()` a pure function of `(state, region, scale, time)`. It is the L3
half of the render-purity path whose L2 half (the immutable handle + the
`content_state` resolver) `model.editable_facet` already landed.

## Why it needs to be done

Purity over the pinned state is the load-bearing property beneath honest
caching and lock-free edit/render concurrency
(`docs/design/14-data-model-and-editing.md:181-187`): "revision identifies
state, state is immutable, so a cached tile can never show pixels newer
than its key." Without the handle on the request, a render worker reading
live model state while the writer paints would produce torn or
newer-than-key pixels, and cache keys built from `(revision, region, scale,
time)` would be dishonest.

Downstream consumers that assume this seam:

- `contract.conformance_suite` (`tasks/25-contract.tji:39-44`) ships the
  property-based **render-purity** check ("same pinned state + request ⇒
  same pixels", `docs/design/16-sdlc-and-quality.md:31-44`) over arbitrary
  plugin content factories — it needs the field to exist and be honored.
- `cache.*` builds tile keys that include the pinned revision; the handle is
  what guarantees the key uniquely identifies the pixels.
- The runtime renderers (`runtime.interactive`, `runtime.offline_sequences`)
  resolve `content_state(id)` from the frame's pinned `DocState` and place
  the handle on each request — that resolution rides on
  `model.content_binding` (`tasks/10-model.tji:56-61`), which binds content
  records to `(kind id + state handle)`.

## Inputs / context

Design docs (normative, doc 16):

- `docs/design/03-layer-plugin-interface.md:40-51` — the `RenderRequest`
  sketch. Line `:50` lists `const SnapshotToken* snapshot;` as the "revision
  fence, for nested compositions". Lines `:134-140` ("Parameters and
  editing"), esp. `:138-140`: "rendering to be **pure over the pinned
  state**." Lines `:164-168` — a plugin process maps the workspace read-only
  and "render[s] from a pinned version it cannot corrupt."
- `docs/design/14-data-model-and-editing.md:181-187` — **"Purity refinement
  to the render contract (doc 03)"**, the governing normative text: "the
  pinned state travels with the request — `RenderRequest`'s snapshot
  resolves to the content's `StateHandle`, and `render()` **must render
  *that* state, making rendering a pure function of (state, region, scale,
  time)**." Lines `:159-162`: "a pinned version pins content state too —
  render workers see frozen pixels while the user keeps painting."
- `docs/design/16-sdlc-and-quality.md:15-25` — claims register (an
  `// enforces: <doc-slug>#<anchor>` tag pairs a test to a registered
  claim). `:31-44` — conformance suite render-purity property. `:48-53` —
  byte-exact goldens (CPU backend is deterministic: fixed FP flags, no FMA,
  ordered reductions). `:54-62` — behavioral counters.
- `docs/design/17-internal-components.md:53` — `arbc::contract` is L3 and may
  depend on `base, pool, media, surface, model`. `:68-72` — "**`contract`
  sits above `model`** because requests carry snapshot pins and `Editable`
  trades in journal-visible state handles"; the model stays free of the
  `Content` vtable (content is bound in `runtime`).

Source seams:

- `src/contract/arbc/contract/content.hpp:20-25` — `RenderRequest`
  (`{region, scale, time, target}`), `:43` — `virtual RenderResult
  render(const RenderRequest&) = 0;`, `:12-16` — `enum class Stability`.
- `src/compositor/compositor.cpp:66` — the **only** `RenderRequest`
  producer: `const RenderRequest request{region, scale, Time::zero(),
  temp};`. `compose()` at `:8`, signature at
  `src/compositor/arbc/compositor/compositor.hpp:34`.
- `src/model/arbc/model/records.hpp:43-48` (`StateHandle`), `:29`
  (`k_state_none`), `:41-42` (the "INERT … never populated" note — the
  handle machinery exists; runtime *population* of non-none handles rides
  `model.content_binding`).
- `src/model/arbc/model/model.hpp:73-80` / `src/model/model.cpp:393-398`
  (`DocRoot::content_state`), `model.hpp:104-109` (`StateRefSink`), `:91`
  (`DocStatePtr`).
- `src/contract/CMakeLists.txt` — `arbc_add_component(NAME contract …
  DEPENDS base media surface model)`: contract **already** depends on model,
  and `arbc/model/records.hpp` is a **public** header of model
  (`src/model/CMakeLists.txt:4`), so naming `StateHandle` in the contract
  public header needs **no new dependency edge**.
- `tests/claims/registry.tsv` — TAB-separated `<claim-id>\t<description>`,
  gated both directions by `scripts/check_claims.py`. No `contract#…` or
  render-purity claim exists yet.

Predecessor refinements: `tasks/refinements/model/editable_facet.md`,
`tasks/refinements/surfaces/surface_pool.md`.

## Constraints / requirements

1. **Add the pinned-state field to `RenderRequest`.** Add a
   `StateHandle snapshot{};` member to `RenderRequest`
   (`src/contract/arbc/contract/content.hpp`), defaulting to `k_state_none`.
   Include `<arbc/model/records.hpp>` in the contract public header (allowed
   by the existing L3→model edge; `scripts/check_levels.py` must stay green).
   The type is the resolved handle, not `DocStatePtr` and not a new
   `SnapshotToken` (see Decision 1).
2. **Preserve the value character of `RenderRequest`.** `StateHandle` is
   trivially copyable (`records.hpp:124-125`); adding it must not introduce
   any refcount touch, allocation, or atomic op on the request path. The
   request stays a cheap by-value descriptor built per render call.
3. **Fix the producer.** Update the sole call site
   (`src/compositor/compositor.cpp:66`) to construct the request without
   relying on positional aggregate order for the new member — pass an
   explicit `StateHandle{}` (the compositor has no pinned `DocState` to
   resolve against in the walking skeleton; population is downstream, see
   Decision 2). Do not thread a `DocState` into `compose()` in this task.
4. **State the render obligation in the contract.** Document on
   `Content::render` (`content.hpp:43`) that, for content with editable
   state, `render` must be a pure function of `(request.snapshot, region,
   scale, time)` — identical inputs yield byte-identical pixels, and
   `snapshot` is a genuine input (`docs/design/14-…:181-187`,
   `docs/design/03-…:138-140`).
5. **Levelization.** Change is confined to `arbc::contract` (L3) and its
   producer `arbc::compositor`; both already sit above `model`. No new
   component edge. `scripts/check_levels.py` gates.
6. **Determinism.** The purity test's pixels must be byte-exact
   (`docs/design/16-…:48-53`); no tolerance. `scripts/gate` (build + asan +
   check_levels + check_claims) green; ≥90% diff coverage on changed lines
   (doc 16).

## Acceptance criteria

- **Claim (register + `enforces:` tag):**
  `03-layer-plugin-interface#render-pure-over-pinned-state` — *"`Content::render`
  is a pure function of (snapshot state, region, scale, time): two calls
  with an identical `RenderRequest` (same `snapshot`, region, scale, time)
  yield byte-identical target pixels, and the `snapshot` handle is a real
  input — two requests differing only in `snapshot` yield different pixels."*
  Registered in `tests/claims/registry.tsv`; enforced by
  `// enforces: 03-layer-plugin-interface#render-pure-over-pinned-state` in
  `src/contract/t/snapshot_pins.t.cpp`. `scripts/check_claims.py` passes
  both directions.
- **Byte-exact purity test** (`src/contract/t/snapshot_pins.t.cpp`,
  `docs/design/16-…:48-53`): a self-contained deterministic test `Content`
  whose `render()` writes target pixels as a pure function of
  `(request.snapshot.slot, region, scale, time)`. Assert:
  (a) same request rendered twice → **byte-identical** target buffers
  (purity); (b) two requests differing **only** in `snapshot.slot` →
  **different** buffers (the handle is genuinely consulted, i.e. render is a
  function *of state*). Kept self-contained (a local test `Content`) so the
  contract unit test links no higher component.
- **Wiring assertion** (same test TU): a recording `Content` whose
  `render()` copies `request.snapshot` into a member; assert the value
  received equals the `StateHandle` placed on the request — pins that the
  pinned handle actually reaches `render()` unchanged.
- **Structural check:** `RenderRequest{}`'s default `snapshot.has_state()`
  is `false` (i.e. defaults to `k_state_none`); the compositor producer
  compiles and `walking_skeleton.t.cpp` / `smoke.t.cpp` stay green,
  confirming the producer change is behavior-preserving.
- **Deferred (owners already WBS leaves — no new task):**
  - The **property-based** render-purity check over *arbitrary editable
    plugin factories* is `contract.conformance_suite`
    (`tasks/25-contract.tji:39-44`) — this task's golden covers a concrete
    deterministic `Content` only; the general property is the suite's job.
  - **Compositor-side resolution** (calling `content_state(id)` from the
    frame's pinned `DocState` and placing the resolved handle on each
    request) rides `model.content_binding` (`tasks/10-model.tji:56-61`) +
    the runtime renderers (`runtime.interactive`,
    `runtime.offline_sequences`); it is out of this L3 task's scope. No new
    leaf.
- **Concurrency:** none added. `snapshot` is a trivially-copyable index
  copied by value onto a per-call request; the pinned handle's liveness is
  guaranteed by the caller holding the `DocStatePtr` (already claimed under
  `model.editable_facet`). No new TSan/stress obligation — stated explicitly
  so the closer does not scope one.

## Decisions

1. **The field is the resolved `StateHandle` (named `snapshot`), not a
   `DocStatePtr` and not a new `SnapshotToken`.**
   *Rationale:* `docs/design/14-…:182-184` is explicit — the request's
   snapshot "resolves to the content's `StateHandle`", and `render()` renders
   *that* state. Because a version is immutable with structural sharing
   (`14-…:22-24`), a content's captured `StateHandle` is a **complete** frozen
   snapshot: a composition kind resolves its children from its own captured
   state (kind-owned interpretation, L3), so no live `DocState` need ride the
   request. The kind's `render()` is exactly the L3 code that interprets the
   index-only, tag-less handle back into typed state, which matches the
   levelization (`17-…:68-72`: model stays free of the content vtable). The
   name `snapshot` preserves the doc-03 field name (`03-…:50`) while the type
   materializes doc-14's refinement.
   *Rejected — `const SnapshotToken* snapshot;` (doc-03 sketch verbatim):* no
   such type exists; doc 14, titled the "Purity refinement to the render
   contract (doc 03)", already resolves the token to a `StateHandle`.
   Introducing a `SnapshotToken` wrapper is speculative indirection over the
   handle the model already exposes.
   *Rejected — `DocStatePtr` on the request:* would add a `shared_ptr`
   refcount touch to every per-call request (violates the cheap-value
   constraint), and pushes model resolution into kind `render()`, which has
   no `ObjectId` at the contract level (binding is runtime, `17-…:60,68-72`).
   *No design-doc delta:* doc 14 §"Purity refinement" and doc 03:138-140
   already state the `StateHandle` resolution and the purity obligation, so
   the constitution is self-consistent (doc 03:50's `SnapshotToken*` is the
   pre-doc-14 sketch it refines). Nothing behavior-altering is introduced,
   so doc 16's same-commit amend rule is not triggered.

2. **The compositor passes a default (`k_state_none`) handle; resolution is
   deferred to the runtime binding path, not scoped here.**
   *Rationale:* `compose()` has no pinned `DocState` and no Content↔`ObjectId`
   binding in the walking skeleton (`compositor.cpp:66` hard-codes
   `Time::zero()` similarly). Resolving `content_state(id)` requires
   `model.content_binding` (`tasks/10-model.tji:56-61`) to migrate the
   runtime side-map into versioned content records first. Landing the
   *carrier* + the *render obligation* now, with the producer supplying the
   inert default, is the honest minimal slice for a 1d task and unblocks the
   conformance suite.
   *Rejected — thread a `DocState` through `compose()` now:* that is runtime
   binding work (L5), out of the contract stream and not yet unblocked; it
   would balloon a 1d contract task into cross-stream work.

3. **Claim anchored to doc 03 (the render contract), not doc 14.**
   *Rationale:* the claim constrains `Content::render`'s observable behavior,
   which lives in the layer-plugin-interface doc; doc 14 supplies the
   *why*. Sibling claims follow the "behavior lives where the contract is
   stated" pattern (e.g. `09-…#surface-pool-recycles`).

4. **Do not register `#snapshot-pins-release-on-unpin`.** The orchestrator
   suggested claiming release-on-unpin, but that lifecycle (retaining a
   pinned handle and releasing it by refcount when the pin drops) is L2
   model behavior already landed and claimed under `model.editable_facet`
   (`14-…#content-state-reclaimed-by-refcount`, `#pin-holds-content-state`).
   This L3 task only *carries* the handle; it neither retains nor releases,
   so a release-on-unpin claim here would be miscategorized and untestable
   at the contract level. Release/unpin behavior stays where it is enforced.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-05.

- Added `StateHandle snapshot{}` field to `RenderRequest` in `src/contract/arbc/contract/content.hpp`; added `#include <arbc/model/records.hpp>` and documented purity obligation on `Content::render`.
- Updated sole `RenderRequest` producer in `src/compositor/compositor.cpp` to pass explicit `StateHandle{}` (inert default; runtime resolution deferred to `model.content_binding`).
- Registered new test TU in `src/contract/CMakeLists.txt`.
- Created `src/contract/t/snapshot_pins.t.cpp` with 3 Catch2 test cases / 5 assertions covering purity (byte-identical for identical requests), snapshot-as-genuine-input (differing only in snapshot → different pixels), and wiring (handle reaches `render()` unchanged) plus default-sentinel structural check.
- Registered claim `03-layer-plugin-interface#render-pure-over-pinned-state` in `tests/claims/registry.tsv` with matching `// enforces:` tag in the test TU.
