# runtime.damage_router — Fan-out DamageSink for multi-viewport dispatch

## TaskJuggler entry

Back-link: `task damage_router` in [`tasks/65-runtime.tji`](../../65-runtime.tji)
(lines 35-40), under `task runtime`.

> `damage_router "Fan-out DamageSink for multi-viewport dispatch"` — "Fan-out
> DamageSink installed once on DocState that dispatches each flushed
> model-damage batch to N registered per-viewport HostViewport sinks (RAII
> register/unregister); enables multiple viewports observing one composition.
> Lands 11-time-and-video#transports-observe-composition-independently
> end-to-end and 01-core-concepts#multiple-viewports-observe-one-composition
> claim. Docs 01/14. Source-of-debt: tasks/refinements/runtime/host_objects.md."

## Effort estimate

**1d.** A clean additive layer: one small `DamageRouter` class (a `DamageSink`
that multiplexes to a registrant list via an RAII handle), a backward-compatible
attach point on `HostViewport`, and two behavioral tests (fan-out delivery +
the two-viewport independence end-to-end). No new levelization edge, no golden,
no new concurrency obligation. The scope, the seam, and the deferral were all
fixed in advance by the predecessor's Decision 6 (see below), so this task
executes a settled plan rather than deciding one.

## Inherited dependencies

**Settled:**

- `runtime.host_objects` (`depends !host_objects`, `complete 100`) — landed
  `HostViewport` (`src/runtime/arbc/runtime/host_viewport.hpp`,
  `src/runtime/host_viewport.cpp`), including the per-viewport
  `DamageAccumulator` (`host_viewport.hpp:148-163`) this task fans out into,
  and the anchored-camera / transport / audio-master policy this task does not
  touch. Its **Decision 6** explicitly deferred multi-viewport damage fan-out
  here and pre-scoped the router as "a clean additive layer (it forwards to
  each viewport's existing sink), not a rewrite," with "`HostViewport`'s damage
  sink … designed to sit behind a future router unchanged."
- Transitively (via `host_objects`): `runtime.interactive`,
  `model.content_binding` — the model damage seam, `Model::current()`, and the
  interactive frame loop are all in place and stable.

**Pending:** none. Every seam this task extends already exists and is marked
complete.

## What this task is

The `Model` exposes exactly **one** damage-sink slot
(`Model::set_damage_sink(DamageSink*)`, `src/model/arbc/model/model.hpp:199`,
single-slot field `:441`). Today a `HostViewport` grabs that slot directly in
its constructor (`host_viewport.cpp:36`) and clears it on destruction (`:39`).
That works for one viewport but structurally forbids two: a second
`HostViewport` constructed over the same `Model` overwrites the first's sink,
silently unsubscribing it.

This task introduces a **`DamageRouter`**: a `DamageSink` that occupies the
`Model`'s single slot **once** and multiplexes each flushed damage batch to
**N** registered child `DamageSink`s (each viewport's existing
`DamageAccumulator`). A `HostViewport` registers with the router instead of the
`Model` when one is supplied, holding a move-only RAII registration handle that
unregisters on destruction. The router lands the design's long-promised
capability — **multiple viewports observing one composition simultaneously**
(doc 01:108-110) — which the single-slot `Model` API cannot express, and does so
as the pure additive occupant the `Model` damage seam's own comment already
anticipates ("The concrete consumer … is wired from above",
`damage.hpp:75-78`; and the ctor comment "a future fan-out router
(`runtime.damage_router`, Decision 6) sits in front of this sink unchanged",
`host_viewport.cpp:33-35`).

## Why it needs to be done

- **It lands two doc-promised behaviors end-to-end.** Doc 01 states N viewports
  may observe one composition simultaneously and that damage "propagates … to
  every viewport observing an affected composition" (01:108-110, 137-138); doc
  11 states transport state is strictly per-viewport (11:88-93). The predecessor
  proved the *value-state* half of the temporal promise
  (`11-time-and-video#transports-observe-composition-independently`,
  `tests/claims/registry.tsv:167`, at the `Transport` level) and explicitly
  deferred the **end-to-end two-`HostViewport` version** here, together with a
  new `01-core-concepts#multiple-viewports-observe-one-composition` claim.
- **It is on the M9 (v0.1 release) critical path.** The task is already a
  dependency of `m9_release` (`tasks/99-milestones.tji:71`) — the
  editor-with-overview-and-detail-view scenario doc 14's intro names
  (14:11-13) needs simultaneous viewports over one document.
- **It closes a registered tech-debt item.** `host_objects` Status registered
  this exact task (`~1d`, `depends runtime.host_objects`, wired into
  `m9_release`); this refinement is its work order.

## Inputs / context

**Governing design-doc sections (normative):**

- **doc 01 (`docs/design/01-core-concepts.md`)**
  - `## Viewport` (line 91), lines 108-110: "Multiple viewports may observe the
    same composition simultaneously (an editor with an overview and a detail
    view), which is again free under pull-based rendering." — the spatial
    fan-out promise this task realizes.
  - `## Invalidation` (line 133), lines 137-138: "Damage propagates up through
    nesting to **every viewport** observing an affected composition; **each**
    viewport maps it to a dirty device region and schedules re-rendering." —
    fan-out (one batch → N viewports, each independent) is the stated model.
- **doc 11 (`docs/design/11-time-and-video.md`)**, lines 88-93 (the bold
  "**Viewport** (doc 01) gains a **transport**" paragraph): transport is
  "per-viewport state like the camera — two viewports may observe the same
  composition *at different times* … which the pull-based design gives for
  free." The router must not couple per-viewport transport/camera state.
- **doc 14 (`docs/design/14-data-model-and-editing.md`)**
  - `## The central decision` (line 20), lines 22-24, 50-56: editing publishes
    immutable `DocState` versions; "The writer thread is the single mutator."
  - `## Transactions` (line 85), lines 94-95 & 108-110: **damage is flushed
    once per commit as the union** of that transaction's mutations; abort emits
    none (111-115). One batch per published revision — the router's dispatch
    granularity.
- **doc 17 (`docs/design/17-internal-components.md`)** levelization: `arbc::model`
  is L2 (owns "damage, revisions, pins", line 52); `arbc::runtime` is L5 and
  "may depend on everything below" (lines 24, 60). The router is an L5 runtime
  concern (host-viewport dispatch) — deliberately **not** in the model, which
  doc 17 keeps ignorant of host viewports, and distinct from the compositor's
  L4 "damage routing over `inputs()`" (operator-graph propagation, line 56).

**Source seams this task extends (verified against live source):**

- **The model damage seam** — `class DamageSink { virtual void flush(const
  std::vector<Damage>&) = 0; }` (`src/model/arbc/model/damage.hpp:79-83`); the
  `Damage` value (`damage.hpp:19-25`) and the `damage_add` union/dedup helper
  (`damage.hpp:64-73`). Install slot: `Model::set_damage_sink(DamageSink*)`
  (`src/model/arbc/model/model.hpp:199`, **WRITER-THREAD ONLY**), backing field
  `d_damage_sink{nullptr}` (`model.hpp:441`). Flush-once dispatch sites, each
  null-guarded: commit (`src/model/model.cpp:1361-1363`), navigate/undo-redo
  replay (`model.cpp:1406-1408`), reconstruction/load (`model.cpp:569-571`).
- **The registrant** — `HostViewport::DamageAccumulator final : public
  DamageSink` (`host_viewport.hpp:148-163`): `flush()` folds the batch into its
  own `std::vector<Damage>` via `damage_add`; `drain()` swaps it out for the
  frame plan (`host_viewport.cpp:91`). This is the child sink the router feeds;
  it is unchanged by this task.
- **The current direct-install wiring to replace behind the router** —
  `d_model.set_damage_sink(&d_sink)` (`host_viewport.cpp:36`) /
  `d_model.set_damage_sink(nullptr)` (`host_viewport.cpp:39`); the ctor comment
  at `host_viewport.cpp:32-35` and header comment at `host_viewport.hpp:37-39`
  already name this task as the router that sits in front.
- **RAII-handle precedent** — `CacheHold` (`src/cache/arbc/cache/keyed_store.hpp:80-108`):
  the move-only, release-on-destroy token to model the registration handle on
  (deleted copy `:82-83`, move-with-release `:85-99`, `~CacheHold(){release();}`
  `:101`, `valid()` for moved-from `:108`).
- **Test fixture** — `src/runtime/t/host_viewport.t.cpp`: `arbc::Model model;`
  per case, `add_single_layer` scene builder (`:160`), and the `bump_damage`
  helper (`:169-174`) that commits a `set_transform` so the commit auto-damages
  a layer — the exact pattern a multi-viewport test extends. Runtime unit tests
  are co-located under `src/runtime/t/`; the claims register is
  `tests/claims/registry.tsv`.

**Predecessor decision carried in verbatim:** `host_objects` Decision 6 fixed
the shape — router installed once on the model slot, forwards each flushed
batch to each viewport's existing accumulator, RAII register/unregister,
`HostViewport` sink unchanged.

## Constraints / requirements

1. **One slot, N registrants.** The `DamageRouter` occupies `Model`'s single
   `set_damage_sink` slot exactly once (installed in its constructor, cleared in
   its destructor — RAII, mirroring today's `HostViewport`). Child sinks
   register/unregister with the router, never with the `Model` directly.
2. **Batch fidelity.** On `flush(batch)` the router forwards the *same*
   `const std::vector<Damage>&` to every registrant's `flush`, exactly once per
   registrant, in deterministic **registration order**. It performs no union,
   filtering, or copy of its own — each registrant's `DamageAccumulator` already
   unions via `damage_add`. Empty registrant set ⇒ the flush is a no-op (zero
   deliveries). This preserves the doc 14 "flush once per commit" contract
   per-viewport (each viewport sees exactly the batches the `Model` flushed).
3. **RAII registration handle.** `register_sink` returns a move-only
   `DamageRouter::Registration` (modeled on `CacheHold`) that unregisters on
   destruction; a moved-from handle is inert (`valid()==false`). Unregister is
   O(registrants) removal by identity; a viewport destroyed while registered
   leaves the router (and the other viewports) intact.
4. **Backward-compatible attach on `HostViewport`.** Add a nullable
   `DamageRouter*` to `HostViewport::Config` (default `nullptr`). When set, the
   ctor registers `&d_sink` with the router and holds the `Registration` (and
   does **not** touch `Model::set_damage_sink`); when null, it retains today's
   direct `d_model.set_damage_sink(&d_sink)` / clear-on-dtor path unchanged.
   Every existing single-viewport `host_viewport.t.cpp` case keeps passing
   verbatim. The two paths are mutually exclusive by construction (a caller uses
   a router for multi-viewport or direct install for single) — the router and a
   direct viewport must not both claim the model slot.
5. **Lifetime ordering.** The router must outlive all its registrants (each
   viewport's `Registration` references it). The RAII handles enforce
   unregister-before-router-destroy at each viewport's scope exit; the router
   asserts an empty registrant list on destruction to catch misuse.
6. **Writer-thread confinement / no new concurrency obligation.** `flush`,
   `register_sink`, and unregister all run on the single writer/UI thread — the
   same thread that owns `Model::set_damage_sink` (WRITER-THREAD ONLY) and the
   single-owner `HostViewport` value state. The router adds **no** new shared
   mutable state and **no** cross-thread channel, so it carries **no** new
   TSan/stress obligation — matching `host_objects` Decision 5. Register/
   unregister during a flush is not a supported call pattern (no registrant's
   `flush` mutates the router); the router iterates its list by index.
7. **Levelization.** `DamageRouter` lives in `arbc::runtime` (L5),
   `src/runtime/arbc/runtime/damage_router.hpp` +
   `src/runtime/damage_router.cpp`, depending only on `arbc::model`
   (`DamageSink`, `Damage`, `Model`, L2) and `arbc::base` — all strictly lower.
   No new component edge; `scripts/check_levels.py` stays green.
8. **No design-doc delta required.** The multi-viewport fan-out (01:108-110,
   137-138) and per-viewport transports (11:88-93) are already normative, and
   doc 17 L5 already covers "viewport/transport/monitor objects." The router is
   the concrete realization within the existing architecture, and the claim
   slugs are free-form claim identifiers (per `tests/claims/registry.tsv`'s
   header convention), not doc-section anchors — so no doc amendment is owed.

## Acceptance criteria

Testable checks that pin the behavior. New claim rows land in
`tests/claims/registry.tsv`, each referenced by an `// enforces: <claim-id>`
test comment (doc 16).

1. **New claim `01-core-concepts#multiple-viewports-observe-one-composition`.**
   Register the row and enforce it with a behavioral test (new
   `src/runtime/t/damage_router.t.cpp`, or an extension of
   `host_viewport.t.cpp`) over one `Model` + one `DamageRouter` + two
   registered `HostViewport`s:
   - A single committed edit (`bump_damage`-style, `host_viewport.t.cpp:169-174`)
     is delivered to **both** viewports' accumulators (each drains the batch);
     delivery is exactly once per viewport, in registration order.
   - Destroying one viewport's `Registration` (RAII) stops delivery to it while
     the other keeps receiving subsequent commits.
   - A router with **zero** registrants flushes a batch as a no-op (a router
     exposes a `registered()` count and a `deliveries()` behavioral counter;
     assert `deliveries() == 0` for the empty fan-out, `== registrants` per
     batch otherwise — a behavioral counter, never wall-clock).

2. **`11-time-and-video#transports-observe-composition-independently`
   end-to-end (existing claim, `registry.tsv:167`).** Add a second enforcing
   test (`enforces:` the existing id — no new row) that lands the deferred
   two-`HostViewport` version: two viewports over one `Model` + one
   `DamageRouter`, each owning its own `Transport`. Advancing or seeking one
   viewport's transport leaves the other's playhead unchanged, and each viewport
   samples the shared `current()` document at its own instant — the value-state
   independence proven per-`Transport` in the predecessor, now proven through
   the live host object with fan-out damage in play.

3. **RAII / lifetime coverage.** Unregister-on-destroy leaves the router and
   remaining registrants intact; a moved-from `Registration` is inert
   (`valid()==false`) and its destruction unregisters nothing twice.

4. **Gates.** `scripts/check_levels.py` green (no new levelization edge);
   CI diff coverage ≥90% on changed lines (tests are part of this task); the WBS
   gate `tj3 project.tjp 2>&1 | grep -iE "error|warning"` silent after the
   closer adds `complete 100` + the `Refinement:` note back-link.

5. **Not applicable, stated for the record.** No byte-exact golden (this task
   ships no deterministic rendering — it dispatches value batches); no contract
   conformance-suite run (no new content kind or operator); no new TSan/stress
   suite (writer-thread-confined, Constraint 6).

**Deferred follow-ups:** none. The router is a complete additive layer; nothing
is left for a successor task. (A future migration making the router the *only*
`HostViewport` damage path — retiring the direct-install branch — is a
behavior-preserving refactor with no v0.1 benefit and is intentionally not
scoped as WBS work.)

## Decisions

1. **A dedicated `DamageRouter` `DamageSink` occupies the model slot; viewports
   register with it.** The `Model` deliberately exposes one sink (doc 17 keeps
   the model ignorant of host viewports), and the seam's own comment names "a
   fan-out router" as the intended occupant (`damage.hpp:75-78`,
   `host_viewport.cpp:33-35`). A router that implements `flush` by forwarding to
   a registrant list is the minimal realization of doc 01:137-138 ("every
   viewport … each viewport …"). *Rejected:* widening `Model` to hold a vector
   of sinks — pushes host-dispatch policy into L2, violating the doc-17
   layering that keeps the model host-agnostic, and complicates the writer-only
   slot contract for no gain.

2. **Backward-compatible optional attach (nullable `DamageRouter*` in
   `HostViewport::Config`), not a mandatory router.** When a router is supplied
   the viewport registers with it; when absent it keeps today's direct install.
   This keeps the router a pure additive layer: every existing single-viewport
   `host_viewport.t.cpp` case compiles and passes unchanged, and only the new
   multi-viewport tests construct a router — honoring Decision 6's "sits behind
   a future router unchanged" and the 1d budget. *Rejected:* making every
   `HostViewport` route through a mandatory router — it forces a router at every
   construction site and rewrites every existing viewport test on a
   `complete 100` predecessor, a wide blast radius for capability v0.1 does not
   need on that path. The mutual-exclusion of the two attach paths (Constraint 4)
   is a documented, construction-time property, not a runtime hazard.

3. **Move-only RAII `Registration` handle modeled on `CacheHold`.** The runtime
   layer has no generic subscription token today; `CacheHold`
   (`keyed_store.hpp:80-108`) is the established move-only, release-on-destroy
   idiom to copy (deleted copy, move-with-release, unregister in the destructor,
   `valid()` for moved-from). This matches the RAII discipline `HostViewport`
   itself already uses for the model slot and makes unregister-before-destroy
   structural rather than a caller obligation. *Rejected:* raw
   `register`/`unregister` calls the caller must pair by hand — error-prone and
   inconsistent with the codebase's RAII conventions.

4. **Deterministic registration-order fan-out, no router-side union.** The
   router forwards the model's already-unioned batch verbatim, in registration
   order, once per registrant. Each `DamageAccumulator` does its own
   `damage_add` union (`host_viewport.hpp:150-154`), so a router-side union
   would be redundant work and would blur which viewport saw what. Deterministic
   order makes the fan-out test's "exactly once, in order" assertion exact.
   *Rejected:* set/unordered delivery — needlessly non-deterministic for a
   writer-thread-serial dispatch.

5. **Writer-thread-confined; no new concurrency obligation.** Register,
   unregister, and flush share the single writer thread that already owns the
   model slot (WRITER-THREAD ONLY) and the single-owner `HostViewport` value
   state; the router introduces no shared mutable state and no cross-thread
   read. This inherits `host_objects` Decision 5's conclusion directly — no new
   TSan/stress coverage is owed. *Rejected:* a lock-protected registrant list —
   guards against a concurrency the architecture does not have, adding cost and
   a false signal that the router is a cross-thread object.

## Open questions

(none — all decided.)

## Status

**Done** — 2026-07-10.

- `src/runtime/arbc/runtime/damage_router.hpp` — new `DamageRouter` (fan-out `DamageSink`) + move-only RAII `Registration` handle modeled on `CacheHold`.
- `src/runtime/damage_router.cpp` — install/clear model slot, registration-order flush with `deliveries()` counter, unregister-by-identity, empty-list dtor assert.
- `src/runtime/t/damage_router.t.cpp` — pure-router unit tests: fan-out order/empty/deliveries, unregister-on-destroy, moved-from inert.
- `src/runtime/arbc/runtime/host_viewport.hpp` / `src/runtime/host_viewport.cpp` — nullable `Config::router`; ctor registers `&d_sink` (holds `Registration`) when set, else retains direct `set_damage_sink`; dtor conditional.
- `src/runtime/t/host_viewport.t.cpp` — two-viewport fan-out end-to-end test (claim `01-core-concepts#multiple-viewports-observe-one-composition`) + transport-independence end-to-end test (second enforcer for `11-time-and-video#transports-observe-composition-independently`).
- `src/runtime/CMakeLists.txt` — new source, public header, unit test wired in.
- `tests/claims/registry.tsv` — new claim row `01-core-concepts#multiple-viewports-observe-one-composition`.
