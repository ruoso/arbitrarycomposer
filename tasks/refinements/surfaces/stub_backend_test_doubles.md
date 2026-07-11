# surfaces.stub_backend_test_doubles — Migrate out-of-tree Backend test doubles onto the shared testing bases

## TaskJuggler entry

[`tasks/20-surfaces.tji:25-30`](../../20-surfaces.tji) — `task
stub_backend_test_doubles "Migrate out-of-tree Backend test doubles onto
StubBackend"`, milestone `m9_release`.

## Effort estimate

**0.5d.** Holds, but it is a tight 0.5d: every edit is mechanical (three new
header-only classes, five test files rebased, two CMake header-list entries,
one new ~70-line unit test), and there is no new runtime behavior to design.
The parts that could push it to 0.75d are the new `src/surface/t/` unit test
and the `StubSurface` hoist — both load-bearing (see Decisions), neither
severable.

## Inherited dependencies

**Settled:**

- `kinds.nested_working_space_conversion` (done) — added `Backend::convert`
  as a new pure virtual
  ([`src/surface/arbc/surface/backend.hpp:70`](../../../src/surface/arbc/surface/backend.hpp)),
  which broke every `Backend` double in the tree and forced a dead-stub
  repair into 18 of them. Its response was
  `arbc::testing::StubBackend`
  ([`src/surface/arbc/surface/testing/stub_backend.hpp`](../../../src/surface/arbc/surface/testing/stub_backend.hpp),
  new), recorded in that refinement's Status block at
  [`tasks/refinements/kinds/nested_working_space_conversion.md:682`](../kinds/nested_working_space_conversion.md).
  The in-lib doubles under `src/*/t/` were migrated onto it; the `tests/`
  doubles were not. **This task is that leftover.**
- `surfaces.capabilities`, `surfaces.surface_pool`,
  `surfaces.provided_surfaces` (all done) — they own the counter assertions
  and the claims that the doubles being migrated are load-bearing for.

**Pending:** none.

## What this task is

Four `Backend` test doubles under `tests/` still derive straight from
`arbc::Backend` and hand-spell all six pure virtuals. This task removes that
boilerplate by giving the `surface` component the test-double bases the
doubles actually need, and rebasing the doubles onto them.

The task name says "onto `StubBackend`". **The inventory says otherwise, and
the refinement corrects the premise:** all four out-of-tree doubles are
*forwarding decorators* over a real `CpuBackend` — they count or perturb one
operation and pass the other five straight through so a real render happens.
`StubBackend`'s defaults are *no-ops*. A decorator that inherited them would
not be a decorator; it would silently stop forwarding. So the migration
target is not `StubBackend` but a second base of the opposite shape —
`ForwardingBackend` (delegate everything) — plus a `CountingBackend` derived
from it, which is what all three `CountingBackend` copies in `tests/` are
open-coding today. The task's *intent* (consistency; no per-double dead
stubs; a new `Backend` operation absorbed in one place) is served exactly;
only the named base changes. See Decisions 1 and 2, and the doc 09 delta.

Scope:

1. **New** `arbc::testing::ForwardingBackend` — decorator over an injected
   `Backend&`, delegates all six virtuals.
2. **New** `arbc::testing::CountingBackend : ForwardingBackend` — tallies one
   call per operation, forwards through the base, plus `reset()`.
3. **New** `arbc::testing::StubSurface` — abstract-`Surface` implementation
   with no pixel storage, hoisted from the copy that already exists in
   `src/surface/t/surface_pool.t.cpp:20-42`.
4. **Migrate** the four `tests/` doubles and delete the redundant overrides
   in the one in-lib double the `StubSurface` hoist touches.
5. **New** unit test pinning the bases' own behavior, and one claims-register
   row for the delegation promise.

## Why it needs to be done

The immediate payoff is the boilerplate: three byte-identical
`CountingBackend` classes (~29 lines each) collapse to a construction site
apiece, and `RefusingBackend` (31 lines) drops to about ten. But the reason
the debt was registered is forward-looking, and it is the same reason
`StubBackend` exists: `Backend` is a pure-virtual interface that is *going to
grow again* (doc 09:44-58 has GPU backends as an explicit post-v1 shape, and
import/sync/display-out edges are named as future users of `convert`). Every
growth event today ripples a dead stub into every double in the tree — and a
dead stub is a line that answers the compiler while documenting nothing,
unreachable by construction, which is precisely the code doc 16:112-118's
diff-coverage gate is right to reject. The in-lib doubles have already been
inoculated. The `tests/` doubles are the remaining blast radius.

There is a second, sharper reason, and it is why this task does not simply do
what its title says. A forwarding decorator that inherits *no-op* defaults
absorbs a new `Backend` operation by **silently not forwarding it** — the
inner backend implements the operation, the decorator swallows it, and the
render produces wrong pixels with every test still passing until a golden
happens to notice. Three of the four doubles here sit directly under
byte-exact goldens (`tests/nested_goldens.t.cpp`). Migrating them onto
`StubBackend` would install exactly that trap. `ForwardingBackend` is the
shape that fails loudly instead.

## Inputs / context

**The contract being doubled.**

- [`src/surface/arbc/surface/backend.hpp`](../../../src/surface/arbc/surface/backend.hpp)
  — `class Backend` (`:16`), six pure virtuals: `capabilities()` (`:26`),
  `make_surface(int, int, SurfaceFormat)` (`:31-32`), `clear` (`:35`),
  `composite` (`:39-40`), `downsample` (`:48`), `convert` (`:70`);
  `protected: Backend() = default` (`:73`).
- [`src/surface/arbc/surface/surface.hpp`](../../../src/surface/arbc/surface/surface.hpp)
  — `Surface` is abstract: `width()`, `height()`, `format()`, and the two
  `cpu_bytes()` overloads (as implemented by the local `StubSurface` at
  `src/surface/t/surface_pool.t.cpp:31-35`).

**The existing base and its stated intent.**

- [`src/surface/arbc/surface/testing/stub_backend.hpp`](../../../src/surface/arbc/surface/testing/stub_backend.hpp)
  — 49 lines, header-only, `namespace arbc::testing`, `class StubBackend :
  public Backend` (`:29`); all six virtuals defaulted (`:31-45`):
  `capabilities()` → `{}`, `make_surface` →
  `unexpected(SurfaceError::UnsupportedFormat)`, `clear`/`composite`/
  `downsample`/`convert` → no-ops. No state, no knobs, no counters. The
  rationale comment (`:14-28`) is the normative statement of why it exists and
  is worth reading in full before writing the sibling base — this task's
  `ForwardingBackend` is the same argument applied to the other shape.

**The four doubles to migrate** (all derive from `arbc::Backend` directly;
none uses `StubBackend`; all forward to a real `CpuBackend`):

| double | span | overrides that are *real* | overrides that are pure boilerplate |
|---|---|---|---|
| `tests/surface_pool_integration.t.cpp:17-45` `CountingBackend` | 29 L | `make_surface` (count + forward) | `capabilities`, `clear`, `composite`, `downsample`, `convert` (all bare forwards) |
| `tests/provided_surfaces.t.cpp:33-59` `CountingBackend` | 27 L | `make_surface` (count + forward) | same five (its own comment at `:32` says it "Mirrors the decorator in `surface_pool_integration.t.cpp`") |
| `tests/nested_goldens.t.cpp:181-212` `CountingBackend` | 32 L | `make_surface`, `convert` (count + forward), `reset()` | `capabilities`, `clear`, `composite`, `downsample` |
| `tests/nested_goldens.t.cpp:220-250` `RefusingBackend` | 31 L | `make_surface` (refuse one `SurfaceFormat`, else forward), `convert` (count + forward) | `capabilities`, `clear`, `composite`, `downsample` |

The two in `nested_goldens.t.cpp` **own** their inner backend (`CpuBackend
d_inner`, `:211` and `:249`); the other two take `Backend&` (`:19`, `:39`).
This asymmetry matters — see Decision 3.

**What the doubles are load-bearing for** (all four already carry `enforces:`
tags; the counter assertions must survive byte-for-byte):

- `tests/surface_pool_integration.t.cpp:47` → `09-surfaces-and-backends#surface-pool-recycles`;
  asserts `make_surface_calls == 2` after frame 1 and *still* `== 2` after
  frame 2 (`:87`, `:92`).
- `tests/provided_surfaces.t.cpp` → `#content-provided-surface-honored`,
  `#transient-provided-copied-not-cached`,
  `#provided-surface-released-after-consume`; asserts `make_surface_calls == 0`
  on the warm inline path (`:202`) and `== 1` on the transient/cache path
  (`:240`).
- `tests/nested_goldens.t.cpp:388-390` →
  `07-color-and-pixel-formats#homogeneous-trees-pay-nothing`,
  `#nesting-boundary-converts-composed-output`; asserts homogeneous →
  `convert_calls == 0`, heterogeneous → `convert_calls == 1` and one extra
  `make_surface` (`:403-414`).
- `tests/nested_goldens.t.cpp:425` (`RefusingBackend`) →
  the `09-surfaces-and-backends#make-surface-faults-as-value` edge; asserts
  all-zero output bytes (`:434`) and `convert_calls == 0` (`:437`).

**The duplication the hoist removes.**

- [`src/surface/t/surface_pool.t.cpp:20-42`](../../../src/surface/t/surface_pool.t.cpp)
  — local `StubSurface` (abstract-`Surface` impl, `int& live` refcount knob).
  Its sibling backend at `:47` already derives from `arbc::testing::StubBackend`
  but *still* spells out `capabilities()` (`:49`), `clear` (`:62`) and
  `composite` (`:63`) — all three now redundant with the base. Its comment at
  `:44-46` records the constraint that shapes this task's unit test: "not
  CpuBackend, which is L3".

**Build / CI.**

- [`src/surface/CMakeLists.txt:1-9`](../../../src/surface/CMakeLists.txt) —
  `stub_backend.hpp` is an ordinary `PUBLIC_HEADERS` entry (`:8`) of the
  `surface` component. There is **no separate testing target**; the predecessor
  refinement's Status line naming an "`arbc::surface::testing` CMake target"
  (`nested_working_space_conversion.md:682`) does not match what shipped.
  `arbc_component_test(COMPONENT surface SOURCES …)` at `:11-13` is where the
  new unit test registers.
- [`cmake/ArbcComponent.cmake:137-140`](../../../cmake/ArbcComponent.cmake) —
  `arbc_finalize_library()` re-exports every component's source dir as a
  `PUBLIC` include dir on the umbrella `arbc` target. All three affected
  `tests/` targets (`tests/CMakeLists.txt:328-331`, `:492-495`, `:497-500`)
  already link `arbc`, so **`#include <arbc/surface/testing/…>` resolves with
  no CMake change beyond the two header-list entries.**
- [`scripts/check_levels.py`](../../../scripts/check_levels.py) — hardcodes
  `"surface": {"base", "media"}` and scans `src/<component>/` **including
  `t/`**; it does *not* scan `tests/`, `testing/`, or `plugins/`.
- [`scripts/check_claims.py:25-42`](../../../scripts/check_claims.py) —
  bidirectional: a registered claim with no enforcing test fails, and an
  `enforces:` tag naming an unregistered claim fails.
- [`tests/claims/registry.tsv:1-3`](../../../tests/claims/registry.tsv) —
  TAB-separated `<claim-id>\t<description>`; id is `<doc-file-stem>#<slug>`;
  descriptions are ASCII-only (`--`, not an em-dash).

**Governing design-doc sections.**

- **Doc 09** `## Backend contract` (`:22-93`) — the six operations, `convert`
  as a transcode (`:29-42`), `BackendCaps` (`:60-68`), errors-as-values
  (`:70-76`), core-owned pooling (`:77-93`). **Amended by this task** — see
  Decision 1.
- **Doc 16** — claims register (`:8-26`); byte-exact goldens (`:48-53`);
  behavioral counters, never wall-clock (`:54-62`); diff coverage ≥90%, hard
  gate (`:112-118`); public API is a deliberate surface (`:128-132`); "mocks
  are reserved for fault injection" (`:227-229`) — note `RefusingBackend` *is*
  fault injection, squarely inside that carve-out.
- **Doc 17** — levelization is CI-enforced and the arrows are the complete
  allowed set (`:41-44`); `surface` is L2, may depend only on `base`, `media`
  (`:51`); publicness is file-set membership (`:170-180`); the repo layout
  block (`:224-233`). **Amended by this task** — see Decision 1.

## Constraints / requirements

1. **Levelization (doc 17), no new component and no new dependency edge.**
   All three new classes live in `arbc::testing` inside the **`surface`
   component (L2)**, as `PUBLIC_HEADERS` members under
   `arbc/surface/testing/`. They may include only `arbc/base/…`,
   `arbc/media/…`, `arbc/surface/…`. `ForwardingBackend` holds a `Backend&`
   and must **not** include or name `CpuBackend` (L3) — the reference is
   injected by the caller, which is what keeps the edge from existing.
   `src/surface/CMakeLists.txt` keeps `DEPENDS base media`;
   `scripts/check_levels.py`'s `ALLOWED` table is **not** edited.
2. **Behavior-preserving.** This is a refactor. No golden byte may move; no
   counter assertion may change value; no `enforces:` tag may be dropped. The
   four doubles' *observable* behavior — what they count, what they refuse,
   what they forward — is identical before and after.
3. **Counter member names are load-bearing.** `testing::CountingBackend` must
   expose `make_surface_calls` and `convert_calls` under exactly those names,
   because the existing assertions (`tests/nested_goldens.t.cpp:403-414`,
   `tests/surface_pool_integration.t.cpp:87`, `tests/provided_surfaces.t.cpp:202`)
   read them. Adding `clear_calls`, `composite_calls`, `downsample_calls`
   alongside is free; renaming the two that exist is not.
4. **`ForwardingBackend` delegates *every* operation, with no exceptions and
   no defaults.** A subclass overrides what it observes and inherits a real
   forward for the rest. An operation added to `Backend` later gets a forward
   here, never a no-op (Constraint: this is the claim registered below).
5. **A double must not bind a reference to a not-yet-constructed member.**
   Base classes are initialized before members, so a double that owns its
   inner backend as a member *cannot* pass it to a `ForwardingBackend(Backend&)`
   base ctor without binding a reference into raw storage. The two
   `nested_goldens.t.cpp` doubles do own theirs today (`:211`, `:249`) — they
   move to caller-owned inners (Decision 3).
6. **Production code stays out.** `src/runtime/export_monitor.cpp:26`'s
   `NullBackend` hand-spells five no-ops and is a tempting `StubBackend`
   client. It is **production** code; it does not move (Decision 5).
7. **Coverage (doc 16:112-118).** The new headers' inline bodies are changed
   lines and face the ≥90% diff gate. Every forwarding body must be executed
   by a test — which is what the new unit test is for, since no existing test
   drives `downsample` or `capabilities` through a decorator.
8. **No TSan obligation.** The doubles are constructed and driven on the test
   thread, the render path they decorate is the render thread's, and none of
   the three new classes adds shared mutable state beyond plain `int` counters
   owned by the test. Nothing here touches the pool, model publish/pin, or the
   audio engine. If a future task drives a counting double from the worker
   pool, thread-safety of the counters becomes *that* task's obligation.

## Acceptance criteria

- **New headers — `src/surface/arbc/surface/testing/` (L2), added to
  `PUBLIC_HEADERS` in `src/surface/CMakeLists.txt`.**
  `forwarding_backend.hpp` (`ForwardingBackend`: `explicit
  ForwardingBackend(Backend& inner)`, six forwarding overrides, `protected:
  Backend& inner() const`), `counting_backend.hpp` (`CountingBackend :
  ForwardingBackend`, six counters, `reset()`), `stub_surface.hpp`
  (`StubSurface : Surface`, `(width, height, format, int* live = nullptr)`,
  empty `cpu_bytes()` spans). Header-only; `libarbc` gains zero objects.
- **Unit test — `src/surface/t/backend_doubles.t.cpp` (new, L2), registered in
  `arbc_component_test(COMPONENT surface …)`.** Drives all six `Backend`
  operations through a `testing::CountingBackend` whose inner is a
  `testing::StubBackend` subclass that records which operations it received
  (and allocates a `testing::StubSurface`, so the test never reaches for
  `CpuBackend`/L3 — the constraint `src/surface/t/surface_pool.t.cpp:44-46`
  already records). Asserts: each of the six counters is exactly 1 after one
  call apiece; each operation *arrived at the inner backend*; `make_surface`
  propagates the inner's `unexpected(SurfaceError::UnsupportedFormat)`
  unchanged; `reset()` zeroes the counters. This is what makes every
  forwarding line in the two new headers a covered line.
- **Claim (register + `enforces:` tag).**
  `09-surfaces-and-backends#forwarding-double-delegates-every-op` — "A
  forwarding Backend test double delegates every Backend operation to its
  inner backend and adds no pixel behavior of its own -- an operation the
  double does not explicitly override is forwarded, decisively not silently
  no-oped." One row in `tests/claims/registry.tsv` under the
  `09-surfaces-and-backends#` stem; enforced from
  `src/surface/t/backend_doubles.t.cpp`. This is the promise the doc 09 delta
  makes normative and the one that justifies the second base existing at all.
  **No other registry row is added** — the migration is behavior-preserving,
  so it lands no new behavioral promise (Decision 6).
- **The four `tests/` doubles are gone, and their claims still pass
  unchanged.** `tests/surface_pool_integration.t.cpp:17-45` and
  `tests/provided_surfaces.t.cpp:33-59` delete their `CountingBackend` outright
  and construct `arbc::testing::CountingBackend{cpu}`;
  `tests/nested_goldens.t.cpp:181-212` deletes its `CountingBackend` likewise;
  `tests/nested_goldens.t.cpp:220-250`'s `RefusingBackend` derives from
  `arbc::testing::CountingBackend` and overrides **only** `make_surface`
  (refuse-or-delegate; `convert_calls` now comes from the base). All existing
  `enforces:` tags and all counter assertions (`:87`, `:92`, `:202`, `:240`,
  `:403-414`, `:434`, `:437`, `:446`) are **untouched and still green**.
  Net: roughly 100 lines of double deleted from `tests/`.
- **In-lib cleanup rides along.** `src/surface/t/surface_pool.t.cpp` drops its
  local `StubSurface` (`:20-42`) for `testing::StubSurface`, and drops the
  three overrides its `StubBackend` subclass no longer needs (`:49`, `:62`,
  `:63`) — the dead stubs the base already answers. Its `live`-count assertion
  keeps working through `StubSurface`'s `int* live` knob.
- **Byte-exact goldens preserved (doc 16:48-53).** `tests/nested_goldens.t.cpp`
  regenerates to **identical bytes** — the diff contains no golden file. This
  is the strongest signal that the decorator rebase did not silently drop a
  forwarded operation, and it is the criterion that would have caught a
  `StubBackend`-based migration.
- **Design-doc delta lands in the same commit (doc 16:23-25).**
  `docs/design/09-surfaces-and-backends.md` — new "Test doubles are part of
  the contract, and the contract ships them" subsection at the end of `##
  Backend contract`. `docs/design/17-internal-components.md` — the repo-layout
  block gains `arbc/<component>/testing/`, plus the paragraph distinguishing it
  from top-level `testing/` (`arbc-testing`). Both are written; the closer
  commits them with the code.
- **Gate green (including asan).** `scripts/gate` clean: `check_levels.py`
  (no new edge), `check_claims.py` (the new row has its enforcing test; no
  tag orphaned by a deleted double), diff coverage ≥90% on the changed lines,
  clang-tidy/IWYU clean on the three new headers.
- **Deferred follow-ups: none.** The task closes its own scope.

## Decisions

1. **The migration target is a new `ForwardingBackend`, not `StubBackend` —
   and doc 09 grows a section saying so.** The `.tji` note (`tasks/20-surfaces.tji:29`)
   assumes the `tests/` doubles are stubs hand-rolling no-op overrides. They
   are not: all four forward to a live `CpuBackend` (`tests/surface_pool_integration.t.cpp:19-44`,
   `tests/provided_surfaces.t.cpp:33-59`, `tests/nested_goldens.t.cpp:183-199`,
   `:224-243`). Deriving them from `StubBackend` would let them drop exactly
   two overrides (`capabilities`, `downsample`) while *silently changing the
   semantics of both*: the inherited defaults no-op instead of forwarding.
   Today that is invisible (`capabilities()` is never called on the render
   path, and these scenes never hit the scale ladder's `downsample` at
   `src/compositor/arbc/compositor/scale_ladder.hpp:106`). Tomorrow, when a
   golden scene does take a ladder rung, the decorator swallows the
   downsample and the golden goes quietly wrong. **A decorator must never
   inherit no-op defaults** — that is the whole finding, and it is what the
   doc 09 delta now makes normative. *Rejected:* migrate onto `StubBackend`
   as literally named — buys two deleted lines per double and installs a
   silent-wrong-pixels trap under three byte-exact goldens. *Rejected:* leave
   the four doubles alone and close the task as "premise invalid" — the debt
   (a `Backend` operation rippling dead stubs into `tests/`) is real; only the
   prescribed base was wrong.
   **Design-doc delta (both already written, closer commits them):**
   - `docs/design/09-surfaces-and-backends.md`, `## Backend contract` — new
     final subsection specifying the two double shapes, why the distinction is
     load-bearing (stub absorbs a new operation by no-oping; forwarder by
     delegating; each is correct for its shape), and that the component ships
     them as public headers.
   - `docs/design/17-internal-components.md`, repo-layout section — closes the
     gap Explore found: doc 17 defined `testing/` as *the conformance suite*
     and `t/` as *unit tests*, leaving a component-shipped test double in
     neither bucket. The layout block now names `arbc/<component>/testing/`
     and the following paragraph separates it from `arbc-testing`.
   - **No doc 00 decision-record bullet.** Doc 00's `## Resolved questions`
     bullets are project-shaping calls at the altitude of "Memory model" and
     "Internal components" (`docs/design/00-overview.md:149-182`). "Where test
     doubles for a component's own contract live" is a layout rule, and doc 17
     *is* the layout constitution — it belongs there, at its natural altitude,
     not in doc 00.

2. **Ship a `CountingBackend` in `arbc::testing`, not just the forwarding
   base.** Once `ForwardingBackend` exists, the three `CountingBackend`
   copies in `tests/` shrink to ~8 lines each — but they would still be three
   copies of the same eight lines, and the *next* behavioral-counter test
   (doc 16:54-62 makes clear there will be more) writes a fourth. Counting is
   the overwhelmingly common decorator, the counters are strictly additive (a
   test asserts the ones it cares about and ignores the rest), and shipping it
   deletes the duplication rather than shrinking it. Three call sites today,
   which clears the "one or two call sites" bar comfortably. *Rejected:* ship
   only `ForwardingBackend` and keep a small local counting double per file —
   leaves the triplicate in place, i.e. leaves the actual debt unpaid.
   *Rejected:* fold the counters into `ForwardingBackend` itself — makes every
   forwarding double pay for counter state it may not want, and conflates
   "delegate" with "observe"; the two-class split keeps each honest.

3. **`ForwardingBackend` holds `Backend&`; a double never owns its inner
   backend.** Base classes initialize before members, so a double that keeps
   `CpuBackend d_inner` as a member and passes it to a `ForwardingBackend(Backend&)`
   base ctor binds a reference into not-yet-constructed storage — legal to
   bind, a landmine to maintain, and gratuitous. The two `nested_goldens.t.cpp`
   doubles own theirs today (`:211`, `:249`); they change to caller-owned
   (`CpuBackend cpu; testing::CountingBackend backend{cpu};` in the `TEST_CASE`),
   which is already how the other two files do it (`tests/surface_pool_integration.t.cpp:19`,
   `tests/provided_surfaces.t.cpp:39`) and makes all four uniform. Two-line
   change per call site. *Rejected:* a `ForwardingBackend` template
   parameterized on an owned inner (`ForwardingBackend<CpuBackend>`) — drags
   `CpuBackend` (L3) into a `surface` (L2) header's instantiation and would
   *create* the levelization edge Constraint 1 forbids. *Rejected:* a
   private-base holder idiom to force member-before-base construction —
   solves a problem that a reference parameter simply does not have.

4. **Hoist `StubSurface` too, even though the task title does not mention
   it.** The new unit test lives in `src/surface/t/` and therefore cannot
   reach `CpuBackend` (L3) — `scripts/check_levels.py` scans `t/` under the
   component's `ALLOWED` set, and `src/surface/t/surface_pool.t.cpp:44-46`
   already records the constraint. So the test needs a `Surface` it can
   fabricate without a real backend. Exactly one already exists
   (`src/surface/t/surface_pool.t.cpp:20-42`). Not hoisting it means
   *copy-pasting a second one* — the task would create the duplication it
   exists to remove. Two call sites, both concrete today. *Rejected:* give the
   new unit test a `CpuBackend` inner — an L2→L3 edge, rejected by CI, and it
   would test the CPU backend rather than the double.

5. **`src/runtime/export_monitor.cpp:26`'s `NullBackend` does not move.** It
   hand-spells five no-ops and is a perfect `StubBackend` client on the merits.
   It is also **production** code, and `arbc/surface/testing/` is test support:
   a production translation unit that includes a testing header inverts what
   the directory means and makes the `testing` namespace load-bearing for
   shipped behavior. The dead-stub cost there is five lines in one file, paid
   once. *Rejected:* migrate it and treat `arbc::testing` as "just another
   public header" — technically permitted by doc 17's file-set rule, but it
   erases the only line separating test scaffolding from shipped code, for a
   five-line saving. If `NullBackend`'s no-op-backend behavior is ever wanted
   in production for real, the answer is a production `NullBackend` in
   `arbc::surface`, not a testing header.

6. **One new claim, not four.** The migration is behavior-preserving, so it
   lands no new *behavioral* promise about the library — the four existing
   claims the doubles already enforce (`#surface-pool-recycles`,
   `#content-provided-surface-honored`, `#transient-provided-copied-not-cached`,
   `#provided-surface-released-after-consume`, plus the two
   `07-color-and-pixel-formats#` nesting claims) simply keep passing, which is
   the acceptance criterion. But the doc 09 delta *does* make one new
   falsifiable promise — "a forwarding double delegates every operation" — and
   doc 16:8-26 says a normative claim in a design doc gets an id and a test.
   Hence `#forwarding-double-delegates-every-op`, and only that. *Rejected:*
   register nothing on the grounds that "it's only a refactor" — it would leave
   the doc 09 paragraph aspirational, which is the exact drift doc 16's register
   exists to prevent. *Rejected:* register a claim per double shape (stub
   no-ops, forwarder delegates) — the stub half is already pinned by every
   existing double that inherits it and by `stub_backend.hpp`'s own defaults;
   only the delegation promise is both new and untested.

7. **The doubles stay in `arbc::testing` in the `surface` component, not in
   the top-level `testing/` (`arbc-testing`) library.** Doc 17:13-14 scopes
   `arbc-testing` to *the content conformance suite* linked by plugin authors,
   and `libarbc` never links it. A `Backend` double belongs with the `Backend`
   contract — same component, so the levelization table binds it (a `surface`
   double cannot reach `backend_cpu`), and so a future `Backend` operation is
   absorbed in the same commit that adds it. `StubBackend` already set this
   precedent; this task follows it and documents it (Decision 1's doc 17 delta)
   rather than inventing a second convention. *Rejected:* move all `Backend`
   doubles to `testing/` — turns a header-only, zero-object test aid into a
   static-library dependency for in-lib unit tests, and puts the doubles
   outside the levelization check entirely (`check_levels.py` does not scan
   `testing/`).

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-11.

- Shipped `arbc::testing::ForwardingBackend` (`src/surface/arbc/surface/testing/forwarding_backend.hpp`) — decorator over injected `Backend&`, delegates all six virtuals with no no-op defaults.
- Shipped `arbc::testing::CountingBackend` (`src/surface/arbc/surface/testing/counting_backend.hpp`) — derives from `ForwardingBackend`, tallies one counter per operation, adds `reset()`.
- Shipped `arbc::testing::StubSurface` (`src/surface/arbc/surface/testing/stub_surface.hpp`) — abstract `Surface` with no pixel storage, hoisted from `src/surface/t/surface_pool.t.cpp:20-42`.
- New unit test `src/surface/t/backend_doubles.t.cpp`: 3 `TEST_CASE`s covering every-op delegation with per-op arrival assertions, allocation-fault propagation as a value, and `StubSurface` contract.
- Claim `09-surfaces-and-backends#forwarding-double-delegates-every-op` registered in `tests/claims/registry.tsv` and enforced from `backend_doubles.t.cpp`.
- Rebased all four `tests/` doubles (`surface_pool_integration.t.cpp`, `provided_surfaces.t.cpp`, `nested_goldens.t.cpp` ×2) onto the new bases, deleting ~100 lines of triplicated decorator; all counter assertions and `enforces:` tags preserved unchanged.
- Cleaned up `src/surface/t/surface_pool.t.cpp`: dropped local `StubSurface` and three dead overrides from its `StubBackend` subclass.
- Design-doc deltas (`docs/design/09-surfaces-and-backends.md`, `docs/design/17-internal-components.md`) committed in the same change; all four test binaries pass; goldens byte-exact; `check_levels` and `check_claims` clean.
