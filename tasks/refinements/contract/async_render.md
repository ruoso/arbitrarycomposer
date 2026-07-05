# contract.async_render — Async rendering path

## TaskJuggler entry

Back-link: `tasks/25-contract.tji:9-13`, `task async_render "Async rendering path"`
under `task contract`.

> note "One code path for sync and async: render returns inline or completes
> via RenderCompletion; cancellation query; deadline/exactness disciplines.
> Doc 03."

## Effort estimate

**3d** (`tasks/25-contract.tji:10`). Larger than its 1d sibling
`snapshot_pins` because it introduces four new contract types
(`RenderCompletion`, `Exactness`, `Deadline`, `RenderError`), changes the
single load-bearing virtual (`Content::render`'s signature), migrates every
`Content` implementor and the sole producer to the new signature, and lands a
thread-safe one-shot completion primitive with explicit TSan/stress coverage
(doc 16:66-73). The heavy conceptual lifting — *what* the async path means —
is already settled by doc 03; this task turns that sketch into concrete,
testable code.

## Inherited dependencies

`async_render` declares no `depends` of its own, so it inherits the parent
`task contract` edges (`tasks/25-contract.tji:7`):
`depends model.editable_facet, surfaces.capabilities`.

**Settled:**

- `model.editable_facet` (`6a799ae`, DONE 2026-07-05,
  `tasks/refinements/model/editable_facet.md`) and
  `surfaces.capabilities` (`62ff4df`, DONE 2026-07-05,
  `tasks/refinements/surfaces/capabilities.md`) — the inherited parent edges.
  Neither is consumed *directly* by this task; they gate the `RenderRequest`
  snapshot seam and the `Surface`/`Backend` contract the request's `target`
  sits on.

**Sibling substrate (not a formal `depends`, but landed and relied on):**

- `contract.snapshot_pins` (DONE 2026-07-05,
  `tasks/refinements/contract/snapshot_pins.md`) already extended
  `RenderRequest` with the pinned `StateHandle snapshot`
  (`src/contract/arbc/contract/content.hpp:30-36`) and documented the
  render-purity obligation on `Content::render` (`content.hpp:54-63`). This
  task adds the *async discipline* fields (`exactness`, `deadline`) alongside
  that `snapshot` field and preserves the purity wording verbatim — the two
  disciplines are orthogonal (purity is over `snapshot`; exactness/deadline
  govern *how hard* and *how long* the render tries).

**Downstream (this task unblocks them):**

- `contract.conformance_suite` (`tasks/25-contract.tji:40-45`) declares
  `depends !async_render, !temporal_fields, !operator_members`. Its
  property-based public suite ships the **"async completion and cancellation
  behavior"** property (`docs/design/16-sdlc-and-quality.md:38`) over
  arbitrary plugin content — it needs the `render(request, done)` signature
  and `RenderCompletion` to exist and be honored.

## What this task is

The walking-skeleton `Content::render`
(`src/contract/arbc/contract/content.hpp:63`) is **synchronous only**:
`RenderResult render(const RenderRequest&)`. The header says so explicitly —
"Walking-skeleton subset: synchronous rendering only — the async completion
path … land[s] with [its] system" (`content.hpp:43-45`). This task lands that
deferred async completion path, bringing the code up to the doc-03 sketch:

1. **Unify the render entry point** — change the signature to
   `std::optional<RenderResult> render(const RenderRequest&, std::shared_ptr<RenderCompletion> done)`
   (`docs/design/03-layer-plugin-interface.md:80-84`). Returning a
   `RenderResult` means "done inline"; returning `nullopt` means "I answered
   asynchronously and will settle via `done`". One method, two ways to answer.
2. **Add the `RenderCompletion` primitive** (`03:62-67`) — a thread-safe,
   one-shot completion handle the renderer settles via `complete(RenderResult)`
   or `fail(RenderError)`, and polls for cooperative cancellation via
   `cancelled()`.
3. **Add the two request disciplines** (`03:12-13,48-49`) — `Exactness`
   (`BestEffort` | `Exact`) and `Deadline` fields on `RenderRequest`, and the
   documented obligations they place on `render()`.
4. **Migrate every implementor and the sole producer** to the new signature.

The *async driver* — a thread pool, a frame loop that sets deadlines, decides
when to cancel, and services pending (`nullopt`) renders by re-compositing on
completion — is **runtime policy** (`docs/design/17-internal-components.md:39-41`)
and is out of scope (see Decision 5). This task ships the *contract surface*
that makes one code path possible; the still-synchronous compositor uses the
inline branch of it.

## Why it needs to be done

Design force 4 (`03:14-16`): "trivial content (solid color) should not pay
async ceremony; heavyweight content (3D engine, network) must not block a
frame." A synchronous-only `render()` forces every content to block the
calling thread until pixels exist — a networked or GPU-pipeline content would
stall the whole compose. The async path is the seam that lets heavyweight
content answer "later" without blocking, while trivial content keeps answering
inline with zero ceremony. It is also, per `03:164-168`, "the natural seam"
for out-of-process plugin isolation later.

Downstream consumers that assume this seam:

- `contract.conformance_suite` (`tasks/25-contract.tji:40-45`) — ships the
  async-completion-and-cancellation property (`16:38`); blocked on this task.
- `runtime.interactive` / `runtime.offline_sequences`
  (`tasks/65-runtime.tji`) — the interactive frame loop (`deadlines`,
  `src/runtime/arbc/runtime/offline.hpp:11`) is the real async *driver*: it
  sets `Deadline`, calls `cancelled()`-triggering `cancel()` when the view
  moves, and services pending completions. It cannot exist without this
  contract surface.
- `compositor` request planning (`docs/design/17-internal-components.md:56`)
  — the request planner sets `Exactness`/`Deadline` per pass (interactive vs
  offline).

## Inputs / context

Design docs (normative, doc 16):

- `docs/design/03-layer-plugin-interface.md:12-16` — design forces 3
  ("Two request disciplines: interactive (deadline, best-effort, may answer
  async or decline) and exact (no deadline, must be faithful)") and 4
  ("Async-capable but sync-friendly").
- `docs/design/03-layer-plugin-interface.md:40-51` — the `RenderRequest`
  sketch: `:48` `Exactness exactness; // BestEffort (interactive) | Exact
  (offline)`, `:49` `Deadline deadline; // meaningful only for BestEffort`.
- `docs/design/03-layer-plugin-interface.md:53-60` — the `RenderResult`
  sketch (`achieved_scale`, `exact`; `achieved_time` and `provided` are
  sibling scope, see Constraints).
- `docs/design/03-layer-plugin-interface.md:62-67` — the `RenderCompletion`
  class: `void complete(RenderResult); void fail(Error); bool cancelled()
  const; // compositor lost interest`.
- `docs/design/03-layer-plugin-interface.md:80-84` — the unified render
  signature: "implement one … Return the result, or nullopt meaning 'I
  answered asynchronously / will complete via `done`'.
  `virtual std::optional<RenderResult> render(const RenderRequest&,
  std::shared_ptr<RenderCompletion> done) = 0;`"
- `docs/design/03-layer-plugin-interface.md:117-127` — the three "points
  worth calling out": `:117-121` "One render entry point, sync and async
  unified … The compositor treats 'returned inline' as an immediately-completed
  async request; there is one code path."; `:122-123` "`cancelled()` … lets
  long renders abandon work … Cooperative, best-effort."; `:124-127`
  "`Exact` requests may take unbounded time but must be faithful … reports
  `achieved_scale`/`exact=false` honestly."
- `docs/design/10-*` (errors-as-values) — the project's error policy: errors
  are values, never exceptions (evidenced by `src/base/arbc/base/expected.hpp:24`,
  "Minimal value-or-error result type (doc 10: errors as values …)"). This
  governs how `fail(Error)` is resolved (Decision 2).
- `docs/design/11-time-and-video.md:87-95` — "Contract changes (doc 03)":
  the doc-11 `RenderRequest` sketch lists `Exactness, Deadline … unchanged`
  (`:95`), i.e. doc 11 treats these as **owned by doc 03**, not by the time
  stream. Doc 11 owns `achieved_time`/`time_extent()` (`:69-71,101-114`) —
  those are the `temporal_fields` sibling's scope, **excluded here**
  (Constraint 6).
- `docs/design/16-sdlc-and-quality.md:31-44` — Tier-1 conformance suite,
  which includes "async completion and cancellation behavior" (`:38`).
  `:48-53` byte-exact goldens (deterministic CPU backend). `:54-62`
  behavioral counters. `:66-73` Tier-6 concurrency tests — "TSan on the full
  suite; dedicated stress tests … with schedule perturbation (randomized
  yields under a seed)".
- `docs/design/17-internal-components.md:39-41` — "**deadlines, frame loops,
  and device clocks are runtime policy**" (L5), not the engines/contract.
  `:53` — `arbc::contract` is **Level 3**, contents include "requests/results,
  `Stability`", may depend on `base, pool, media, surface, model`. `:56` —
  `arbc::compositor` is Level 4 (`request planning`), depends on
  `contract, cache`; it sits **above** contract and consumes `RenderResult`.

Source seams:

- `src/contract/arbc/contract/content.hpp` — `Stability` (`:13-17`),
  `RenderRequest` (`:30-36`, currently `{region, scale, time, snapshot,
  target}`), `RenderResult` (`:38-41`, `{achieved_scale, exact}`), the
  walking-skeleton comment (`:43-45`), `Content` (`:46-67`) and the **sync**
  render virtual (`:63`). Purity obligation doc-comment (`:54-62`) must be
  **preserved verbatim** and extended, not replaced.
- `src/contract/CMakeLists.txt` — `arbc_add_component(NAME contract SOURCES
  content.cpp PUBLIC_HEADERS arbc/contract/content.hpp DEPENDS base media
  surface model)`. The new `RenderCompletion` may need a `.cpp` (out-of-line
  settle logic) added to `SOURCES`; the new test TU
  (`t/async_render.t.cpp`) added via `arbc_component_test`.
- `src/contract/t/snapshot_pins.t.cpp:51,71` — the two test doubles
  (`DeterministicContent`, `RecordingContent`) override the **sync** render
  (`RenderResult render(const RenderRequest&) override`) — **must migrate**
  to the new signature.
- `src/kind_solid/arbc/kind_solid/solid_content.hpp:20,26` +
  `src/kind_solid/solid_content.cpp:16` — `SolidContent : public Content`
  overrides the sync render — **must migrate** (returns its `RenderResult`
  inline, `nullopt` never).
- `src/compositor/compositor.cpp:71-72` — the **sole producer**:
  `const RenderRequest request{region, scale, Time::zero(), StateHandle{},
  temp}; const RenderResult result = content->render(request);` — **must
  migrate** to the async-capable signature (construct a `RenderCompletion`,
  handle the inline branch, skip on `nullopt`; see Decision 5).
- `src/base/arbc/base/expected.hpp:12,24-33` — `unexpected<E>` /
  `expected<T, E>`, the errors-as-values idiom `fail`/settlement reuses.
- `src/base/arbc/base/time.hpp:10-19` — `Time` is **content-local media
  time** (flicks), *not* a wall clock; a render `Deadline` is a *monotonic
  wall-clock* instant, a distinct clock (Decision 3).
- `tests/claims/registry.tsv` — TAB-separated `<claim-id>\t<description>`,
  gated both directions by `scripts/check_claims.py`. No async/completion
  claim exists yet.

Predecessor / sibling refinements:
`tasks/refinements/contract/snapshot_pins.md`,
`tasks/refinements/model/editable_facet.md`.

## Constraints / requirements

1. **Unify the render signature.** Replace `virtual RenderResult
   render(const RenderRequest&) = 0;` (`content.hpp:63`) with
   `virtual std::optional<RenderResult> render(const RenderRequest& request,
   std::shared_ptr<RenderCompletion> done) = 0;` (`03:83-84`). A non-`nullopt`
   return is an inline (synchronous) settlement; `nullopt` means the content
   stored `done` and will settle later. Keep and extend the existing
   render-purity doc-comment (`content.hpp:54-62`); add the exactness/deadline
   obligations (Constraint 4) beneath it.

2. **Add `RenderCompletion` — a thread-safe, one-shot completion.** Public
   contract type (header + likely a `.cpp`). Surface (matching `03:62-67`,
   resolving `Error` per Decision 2, and adding the minimal pull seam the
   still-synchronous caller needs):
   - `void complete(RenderResult)` / `void fail(RenderError)` — the renderer
     settles **exactly once**. A second settle attempt (or settle-after-take)
     is ignored (CAS-guarded), never UB. `complete` and `fail` are mutually
     exclusive.
   - `bool cancelled() const` — cooperative cancellation query the renderer
     polls (`03:66,122-123`). Advisory: cancellation does **not** prevent a
     later `complete`/`fail`; it signals the renderer *may* abandon work.
   - `void cancel()` — the caller-side (compositor/runtime) request that sets
     the cancelled flag.
   - `std::optional<expected<RenderResult, RenderError>> take()` +
     `bool settled() const` — the caller retrieves the single settlement,
     non-blocking (`nullopt` = not yet settled). This is the *one code path*:
     both an inline `complete(*inline_result)` and a later off-thread
     `complete(...)` are drained through the same `take()`. **How the caller
     is *woken* on completion (condvar/eventfd) is runtime policy** and is out
     of scope.
   - **Thread-safe.** `complete`/`fail` may run on a renderer thread while
     `cancel`/`cancelled`/`take`/`settled` run on the compositor thread. Use
     `std::atomic` state + fenced payload (or a small mutex); settle the state
     first, then publish the payload; never hold a lock across user code. No
     third-party dependency (Decision 3) — `<atomic>`, `<memory>`, `<mutex>`,
     `<optional>` are std, allowed.

3. **Add the request disciplines.** Add to `RenderRequest` (`content.hpp:30-36`):
   - `Exactness exactness{Exactness::BestEffort};` — enum
     `enum class Exactness { BestEffort, Exact };` (`03:48`).
   - `Deadline deadline{};` — a monotonic wall-clock instant type wrapping
     `std::chrono::steady_clock::time_point` with a `Deadline::none()`
     sentinel (default; also used for `Exact`, which has no deadline,
     `03:49`). Contract carries the **value only** — it provides **no**
     `now()`/`expired()` (reading the clock is runtime policy, `17:39-41`;
     Decision 3). Preserve the by-value/cheap character of `RenderRequest`
     (no allocation, no refcount): `Exactness` is an enum, `Deadline` a
     trivially-copyable `time_point` wrapper.

4. **State the discipline obligations** on `Content::render` (`content.hpp`
   doc-comment): (a) render is still **pure over `snapshot`** — preserve the
   snapshot_pins wording; (b) a `BestEffort` render *may* answer async
   (return `nullopt`), degrade (`achieved_scale < scale`, `exact=false`), or
   observe `deadline`; an `Exact` render *must* be faithful — it may take
   unbounded time and reports `achieved_scale`/`exact` honestly, and it does
   not consult `deadline` (`03:124-127`).

5. **Migrate all implementors and the sole producer** (Constraint-1 signature
   change is a breaking change; `scripts/gate` must stay green):
   - `SolidContent` (`src/kind_solid/...`) — return its `RenderResult` inline.
   - `DeterministicContent`, `RecordingContent`
     (`src/contract/t/snapshot_pins.t.cpp:51,71`) — return inline; keep their
     purity assertions intact.
   - `src/compositor/compositor.cpp:71-72` — construct a `RenderCompletion`,
     call `render(request, done)`, and on an inline result drive it through
     the completion (`done->complete(*r)` → `done->take()`) so the composite
     step is the **one settle path**; on `nullopt` (async content) the
     synchronous single-pass compose cannot service the pending render this
     pass — release `temp` and skip compositing that layer (servicing pending
     completions is `runtime.interactive`, see Acceptance/Deferred). Do **not**
     add a thread pool or frame loop here.
   - Implementer must sweep for any other `: public Content` /
     `render(...) override` sites beyond those above and migrate them.

6. **Scope exclusions (siblings own these).** Do **not** add `achieved_time`
   or `time_extent()` (doc 11, `temporal_fields`, `tasks/25-contract.tji:20-24`).
   Do **not** add `RenderResult::provided` / `SurfaceRef` (doc 09
   content-provided surfaces — a separate concern, undefined type, not in this
   note). `RenderResult` stays `{achieved_scale, exact}`.

7. **Levelization.** Change confined to `arbc::contract` (L3) and its consumers
   `arbc::compositor` (L4) and `arbc::kind_solid`; all sit above/beside
   contract's allowed deps. No new component edge. `scripts/check_levels.py`
   gates.

8. **Determinism + gates.** The inline-vs-async equivalence golden must be
   byte-exact (`16:48-53`), no tolerance. `scripts/gate` (build + asan +
   check_levels + check_claims) green; the concurrency test runs under **TSan**
   (`16:66-73`); ≥90% diff coverage on changed lines (doc 16).

## Acceptance criteria

- **Claim (register + `enforces:` tag):**
  `03-layer-plugin-interface#render-inline-or-async` — *"`Content::render`
  answers along one code path: it either returns a `RenderResult` inline or
  returns `nullopt` and settles later via the supplied `RenderCompletion`.
  Driving a synchronous content (returns a value) and an asynchronous content
  (returns `nullopt`, later `complete`s) through the identical
  render→settle→`take()` path yields equivalent, byte-identical settlements."*
  Registered in `tests/claims/registry.tsv`; enforced by
  `// enforces: 03-layer-plugin-interface#render-inline-or-async` in
  `src/contract/t/async_render.t.cpp`.
- **Claim (register + `enforces:` tag):**
  `03-layer-plugin-interface#render-completion-settles-once` — *"A
  `RenderCompletion` delivers exactly one settlement: `complete`/`fail` are
  mutually exclusive and a second settle attempt is ignored; `take()` yields
  the settlement once. `cancelled()` is an advisory cooperative flag —
  `cancel()` makes `cancelled()` observe `true` but does not prevent a
  subsequent settlement, and this holds under concurrent `complete`/`cancel`
  from different threads."* Registered in `tests/claims/registry.tsv`; enforced
  by the tag in `src/contract/t/async_render.t.cpp` **including a TSan/stress
  case** (see below). `scripts/check_claims.py` passes both directions for
  both claims.
- **Byte-exact one-code-path golden** (`src/contract/t/async_render.t.cpp`,
  `16:48-53`): a deterministic sync test `Content` (returns a value inline) and
  a deterministic async test `Content` (returns `nullopt`, stores `done`, later
  `done->complete(result)` with the *same* pixel-writing logic). A shared
  `drive(content)` helper runs render → (fold inline via `complete`) → `take()`
  → observe. Assert the two contents produce **byte-identical** target buffers
  and equal `RenderResult` for identical requests. Self-contained test
  `Content`s (no higher component linked).
- **Completion-semantics unit cases** (same TU): (a) `take()` before settle →
  `nullopt`, `settled()` → `false`; after `complete` → the value once, then
  `nullopt`; (b) `fail(RenderError::…)` → `take()` yields the `unexpected`
  error; (c) double settle (`complete` then `complete`/`fail`) is ignored — the
  first settlement stands; (d) `cancel()` → `cancelled()==true` yet a following
  `complete` still settles (advisory).
- **Concurrency (TSan/stress, `16:66-73`)** — explicitly scoped, not deferred:
  a stress case spawning a renderer thread that `complete`s (or `fail`s) while
  a consumer thread races `cancel()`/`cancelled()`/`take()`, iterated with
  seeded schedule perturbation (randomized yields). Runs clean under
  ThreadSanitizer; asserts exactly-one settlement observed and no torn payload.
  Tagged to `#render-completion-settles-once`.
- **Structural / migration checks:** `RenderRequest{}` defaults
  `exactness == BestEffort` and `deadline` to `Deadline::none()`; the migrated
  `SolidContent`, the compositor producer, and existing
  `snapshot_pins.t.cpp` / walking-skeleton / smoke tests compile and stay
  green (the signature migration is behavior-preserving for inline content).
- **Deferred (owners already WBS leaves — no new task):**
  - The **property-based** async-completion-and-cancellation check over
    *arbitrary plugin content* is `contract.conformance_suite`
    (`tasks/25-contract.tji:40-45`, `16:38`) — this task's tests cover concrete
    deterministic contents only.
  - The **async driver** — a frame loop / thread pool that sets `Deadline`,
    issues `cancel()` on view change, and services pending (`nullopt`) renders
    by holding the temp surface and re-compositing on completion — is
    `runtime.interactive` / `runtime.offline_sequences`
    (`tasks/65-runtime.tji`; `17:39-41`). Out of this L3 task's scope. No new
    leaf.
- **No under-registered follow-ups.** Every deferral above maps to an existing
  WBS leaf; this task registers no new task.

## Decisions

1. **One unified render method (`optional<RenderResult> render(request, done)`),
   not a sync/async method pair.**
   *Rationale:* the doc-03 sketch (`03:83-84`) shows a **single** pure-virtual;
   "one render entry point, sync and async unified" (`03:117-121`) is the whole
   point. Returning a value vs `nullopt` *are* the "two ways to implement the
   one method" — that is how I read the `:80` "implement one, the other has a
   default" comment (a pre-sketch phrasing; the concrete sketch collapsed it to
   one method). A single method keeps the vtable minimal and forces every
   content to acknowledge the async seam (it receives `done` regardless), which
   is what makes the compositor's one-code-path real.
   *Rejected — keep a sync `render(request)` + add an async overload:* two
   virtuals with a default forwarding between them is more surface, invites
   contents to implement the "wrong" one, and diverges from the `03:83-84`
   sketch for no gain.

2. **`fail(Error)` is resolved to a contract-local `enum class RenderError`,
   not a new unified `Error` type.**
   *Rationale:* no `Error` type exists; doc 03 states its names are
   "provisional" (`03:4-5`), and doc 10 mandates errors-as-values via
   `expected`/`unexpected` (`src/base/arbc/base/expected.hpp:24`). The codebase
   already realizes this with **per-component error enums** — `SurfaceError`,
   `PoolError`, `RefError`. A small `RenderError` enum (e.g.
   `{ ContentFailed, ResourceUnavailable }`; widened as real async content
   lands) matches that established idiom exactly and lets settlement be an
   `expected<RenderResult, RenderError>`. Resolving a provisional doc name to
   the project's existing error idiom alters no designed behavior, so doc 16's
   same-commit amend rule is not triggered — this mirrors `snapshot_pins`
   resolving `SnapshotToken*` → `StateHandle` with no delta.
   *Rejected — a new unified `Error` struct (message + code):* speculative;
   there is exactly one consumer today and doc 10's per-component-enum pattern
   is already the constitution's answer. A cross-cutting error type is a
   project-shaping decision that would need its own doc delta and has no
   demand.

3. **`Deadline` wraps `std::chrono::steady_clock::time_point` (monotonic),
   with a `none()` sentinel and no clock reads; `Exactness` is a plain enum.**
   *Rationale:* a render deadline is a *wall-clock* instant, a different clock
   from `base::Time` (content-local media flicks, `time.hpp:10-19`) — conflating
   them would be a category error, so `Deadline` is its own type.
   `steady_clock` is the standard monotonic clock a frame loop uses; it is a
   `std` facility (not a third-party dependency, so doc 10's dependency policy
   is untouched). Crucially, contract carries the deadline **value** but
   exposes **no `now()`/`expired()`** — reading the clock and enforcing the
   deadline is *runtime policy* (`17:39-41`: "deadlines, frame loops, and
   device clocks are runtime policy"). This keeps the L3 contract a pure data
   surface and leaves enforcement to L5, respecting the levelization. The
   `none()` sentinel (`time_point::max()`) means "no deadline" — the default,
   and the mandatory value for `Exact` requests.
   *Rejected — reuse `base::Time` for the deadline:* conflates media time with
   wall-clock time; a `Time` is a position in the content's timeline, not a
   compute budget.
   *Rejected — opaque `int64_t` nanos with the clock left unspecified:* less
   type-safe and pushes clock-identity ambiguity onto every caller;
   `steady_clock::time_point` is the honest type and runtime would adopt it
   anyway.
   *No design-doc delta:* doc 03 already sketches `Deadline`/`Exactness` as
   request fields (`03:48-49`) and doc 17 already reserves clock policy for
   runtime (`17:39-41`); concretizing the provisional field types without
   adding a `now()`/enforcement seam introduces no new architecture and no
   behavior change.

4. **`RenderCompletion` is a pull-based one-shot (`take()`/`settled()`), not a
   push continuation baked in at construction.**
   *Rationale:* a pull primitive is policy-neutral — it stores the single
   settlement atomically and lets the caller decide *when* and *on which
   thread* to consume it. A baked-in continuation would commit the contract to
   "the settle callback runs on the *renderer's* thread", forcing the
   compositor's compositing (and its surface/backend lifetimes) to be
   thread-safe or marshalled — precisely the frame-loop policy that
   `17:39-41` reserves for runtime. Pull still delivers "one code path": inline
   and off-thread settlements both drain through the same `take()`. The wakeup
   mechanism (how runtime avoids busy-polling) layers on top in runtime.
   *Rejected — a `std::function` continuation invoked from `complete`/`fail`:*
   over-commits the callback-thread policy at L3; also C++20 lacks
   `std::move_only_function` (`CMAKE_CXX_STANDARD 20`,
   `CMakeLists.txt:9`), so a move-only continuation would need a hand-rolled
   type-erasure for no benefit over `take()`.

5. **The compositor stays synchronous: it uses the inline branch and skips
   pending (`nullopt`) renders; the async *driver* is deferred to runtime.**
   *Rationale:* `render_frame`/`compose` (`compositor.cpp:7-82`) is a
   single-pass synchronous walk with a pool-scoped temp surface
   (`compositor.cpp:56-64`). Servicing a `nullopt` render means holding the
   temp alive past the pass and re-compositing when the completion settles —
   that requires a frame loop, a completion wakeup, and deadline/cancel policy,
   all of which are **runtime** (L5, `17:39-41`). The honest minimal slice for
   this L3 task is to land the *contract surface* + migrate the caller to fold
   inline results through the completion (proving the one settle path) while
   skipping async content for the synchronous pass. `runtime.interactive`
   (`tasks/65-runtime.tji`) is the existing leaf that turns the skipped
   `nullopt` path into real deferred compositing.
   *Rejected — build a mini async driver in the compositor now:* that is L5
   runtime work pulled down into L4, balloons a 3d contract task into a frame
   loop, and duplicates what `runtime.interactive` will own.

6. **Claims anchored to doc 03 (the render contract), not docs 11/16.**
   *Rationale:* both claims constrain `Content::render` / `RenderCompletion`
   observable behavior, which lives in the layer-plugin-interface doc; docs
   11/16 supply *why/how-tested*. This follows the sibling pattern
   (`snapshot_pins` anchored `#render-pure-over-pinned-state` to doc 03).

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-05.

- Added `Exactness` (BestEffort/Exact) and `Deadline` enums+types, `RenderError` enum, and `RenderCompletion` class to `src/contract/arbc/contract/content.hpp`; unified render signature to `std::optional<RenderResult> render(const RenderRequest&, std::shared_ptr<RenderCompletion>)`.
- Implemented thread-safe one-shot `RenderCompletion` (CAS-claimed settle, release/acquire payload publish) in `src/contract/render_completion.cpp`; added to `src/contract/CMakeLists.txt`.
- Migrated `SolidContent` (`src/kind_solid/arbc/kind_solid/solid_content.hpp`, `src/kind_solid/solid_content.cpp`) to new signature — returns inline, `nullopt` never.
- Migrated `DeterministicContent`/`RecordingContent` test doubles in `src/contract/t/snapshot_pins.t.cpp` to new signature.
- Migrated sole compositor producer in `src/compositor/compositor.cpp` — folds inline results through `done->complete(*r)→take()`; skips `nullopt` (async) layers.
- Added `src/contract/t/async_render.t.cpp`: byte-exact one-code-path golden, completion-semantics cases (take/settled, fail, double-settle, advisory cancel), and seeded TSan/stress concurrency case.
- Registered claims `#render-inline-or-async` and `#render-completion-settles-once` in `tests/claims/registry.tsv`.
