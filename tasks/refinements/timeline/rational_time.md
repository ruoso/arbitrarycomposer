# timeline.rational_time — Rational time math

## TaskJuggler entry

`task rational_time "Rational time math"` in
[`tasks/40-time.tji`](../../40-time.tji) (lines 6-10), under
`task timeline "Time and video"`. Note line: _"Flicks instants, exact
rational rates, per-edge time maps composed in rational arithmetic with one
rounding at the leaf; half-open span culling. Doc 11."_

## Effort estimate

**2d** (`effort 2d`, `tasks/40-time.tji:7`). Doc 11 itself frames this leaf
as "**Small, self-contained**" — the first half of "New machinery" item 1,
_"A `Time` type (flicks) + rational rate arithmetic"_ (`11-time-and-video.md:204`).
The `Time`/`TimeRange` value types already exist (landed by
`contract.temporal_fields`); this task adds only the rational rate type and
the time-map composition/evaluation math beside them, plus its unit and
numeric-property tests. No new component, no new dependency edge.

## Inherited dependencies

Both are the parent `timeline` task's `depends` (`tasks/40-time.tji:4`),
inherited by every child.

- **`contract.temporal_fields`** — **settled** (Done 2026-07-05,
  `tasks/refinements/contract/temporal_fields.md`). It landed the two value
  types this task builds directly on, both in `arbc::base` (L0):
  - `struct Time { std::int64_t flicks; static constexpr flicks_per_second =
    705'600'000; ... }` — an instant on a local axis in integer flicks
    (`src/base/arbc/base/time.hpp:11-20`).
  - `struct TimeRange { Time start, end; ... all()/empty()/contains() }` —
    the half-open `[start, end)` interval, the temporal analog of `Rect`
    (`src/base/arbc/base/time.hpp:29-54`). `TimeRange::all()` is the
    `[int64_min, int64_max)` absorbing range (`:38-41`); `contains(t)` is
    exactly `start <= t < end` (`:49-51`); `empty()` is `end <= start`
    (`:45`). The predecessor **deliberately left this type arithmetic-free**
    and named `time.rational_time` as the owner of "the rational rate /
    time-map arithmetic that composes ranges across edges … which builds on
    this minimal type, not part of it" (`time.hpp:26-28`,
    `temporal_fields.md` Decisions/Constraint 3). This task must **not**
    redefine `Time`/`TimeRange`.
  - It also landed the downstream contract seam this math ultimately feeds:
    `RenderRequest.time` (`src/contract/arbc/contract/content.hpp:79`),
    `RenderResult::achieved_time` (`content.hpp:88-95`),
    `Content::time_extent()` (`content.hpp:214`), `Content::quantize_time()`
    (`content.hpp:235`), and the three-way `Stability` enum
    (`content.hpp:25-28`).
- **`model.transactions`** — **settled** (Done 2026-07-05,
  `tasks/refinements/model/transactions.md`). Not consumed by the math
  itself; it prepared the model records that the *next* timeline leaf
  (`temporal_placement`) will extend — `LayerRecord` leaves explicit room
  for "span/time-map presence" placement flags
  (`src/model/arbc/model/records.hpp:37-38`). This task defines the
  `time_map` value type those flags will gate; it does not touch the records.

No **pending** inherited dependencies.

## What this task is

Add the exact-rational time arithmetic doc 11 specifies, as a new module
**inside `arbc::base`** (`src/base/arbc/base/rational_time.hpp`, `.cpp` if a
TU is warranted), sitting beside `time.hpp`. Three pieces:

1. **`Rational`** — an exact rational rate, `{int64 num, int64 den}` kept
   canonical (reduced to lowest terms, `den > 0`, sign in the numerator,
   `0 == 0/1`). Composition of rates is exact rational multiply; realistic
   nested rates (e.g. `24000/1001` inside `1/2`) stay exact and reduced.
2. **`TimeMap`** — the per-edge 1D affine map doc 11 defines,
   `local_time = (parent_time − in) × rate + offset`
   (`11-time-and-video.md:58`), stored as `{ Time in; Rational rate; Time
   offset; }`. A chain of these composes **down a graph edge stack** into a
   single rational affine accumulator (`local = a·parent + b`, `a` a
   `Rational`, `b` an exact rational offset) held **unrounded** through the
   whole chain, so the composition is evaluated entirely in rational
   arithmetic and the conversion to an integer-flick `Time` happens **exactly
   once, at the leaf** (`11-time-and-video.md:44-48`, `:144-146`).
3. **Half-open span culling** — the presence predicate over the existing
   `TimeRange` (a span is `[in, out)` in parent time): a layer is present at
   `parent_time` iff `span.contains(parent_time)`, the default all-present
   span is `TimeRange::all()` (stills are the degenerate case), and outside
   the span the layer is culled before its time map is evaluated
   (`11-time-and-video.md:54-57,64-65`).

The math is pure value-type code with no `Content` vtable, no model records,
and no render pipeline — it is the exact substrate the temporal placement,
transport, audio varispeed, and achieved-time-coalescing tasks build on.

## Why it needs to be done

It is the root of the timeline work-stream: `temporal_placement`
(`tasks/40-time.tji:11-16`, `depends !rational_time`) stores `span` +
`time_map` on the layer records using these types; `temporal_cache`
(`:23-28`, `depends !rational_time`) keys achieved-time entries off the
quantized instants this math produces; `transport` and `playback_hints`
sample and drive the composition time these maps consume. Doc 12's audio
varispeed is "well-defined at any nesting depth because composed rates are
exact rationals (doc 11)" (`12-audio.md:104`) — that guarantee is exactly
what this task realizes. Every consumer needs the exactness invariant
(no precision degradation with depth) and the single-rounding-at-leaf
discipline nailed down before it can be relied on.

## Inputs / context

Design docs (normative — doc 16's executable-spec discipline):

- `docs/design/11-time-and-video.md`
  - § *Time model / Representation* (lines 41-48) — flicks
    (`1/705,600,000 s`, "exactly divisible by all common video and audio
    rates"); rates are "exact rationals (e.g. 24000/1001)"; "Composed time
    maps are evaluated in rational arithmetic and **rounded to the timebase
    once, at the leaf**"; "exact rational composition **doesn't degrade with
    depth**" (the central correctness invariant). **Design-doc delta** (this
    task, same commit): the two sentences appended at 11:46-… pin the leaf
    rounding mode (nearest flick, ties to even) and overflow-as-value — see
    Decisions 4 and 3.
  - § *Model changes / Layer instance* (lines 54-65) — `span` half-open
    `[in, out)` in parent time, default `(−∞, +∞)` ("stills are the
    degenerate case, not a special one"); the `time_map` affine formula with
    `rate` rational, **negative allowed (reverse playback)**; "Outside its
    span a layer is culled".
  - § *Pipeline changes* (lines 144-146) — frame planning "computes each
    layer's local time by composing time maps down the tree … **one rational
    multiply-add per edge**".
  - § *Recursion* (lines 173-191) — "time remapping falls out of nesting";
    rates multiply through composition (rate ½ inside rate ½ plays at ¼).
  - § *New machinery* item 1 (line 204) — this task's scope, "Small,
    self-contained."
- `docs/design/04-transforms-and-infinite-zoom.md`
  - lines 44-47, 113 — the sibling "recompute from per-edge matrices, **never
    accumulate**" rule doc 11 cites; time maps follow it, with the extra
    benefit that rational composition is exact (no rebasing needed).
- `docs/design/16-sdlc-and-quality.md`
  - § *Test taxonomy* — tier 2 unit tests name "**rational time**" explicitly
    ("fast, exhaustive on edge cases", lines 45-47); tier 5 **Numeric
    invariant tests** call out "**rational-time exactness across pathological
    rate stacks**" (lines 63-65) — the property test written for this task;
    goldens/tolerance rule (lines 47-53, tolerances are the justified
    exception); diff-coverage hard gate ≥90% (lines 112-118).
  - Claims mechanics: `tests/claims/registry.tsv` (two tab-separated columns
    `<claim-id>\t<description>`; a claim id is `<doc-file-stem>#<slug>`;
    descriptions single-line, present tense, ASCII) + `scripts/check_claims.py`
    (the `enforces: <claim-id>` tag scan, both directions gated).
- `docs/design/17-internal-components.md`
  - Component table (line 48): `arbc::base` is **L0**, owning "`Time`/rational
    rates/time maps" among the foundation value types, **depending on
    nothing** (governing docs listed for base include 11).
  - Levelization rule (lines 41-44) — "A component may depend only on
    strictly lower levels"; CI validates the CMake + include graph
    (`scripts/check_levels.py`, wired at `.github/workflows/ci.yml:22-23`).
- `docs/design/10-tooling-and-packaging.md`
  - line 27 — "lean Catch2"; the dependency-minimalism policy this task
    honours by hand-rolling property loops rather than adding a generator
    library.

Source seams this task extends (all current at `HEAD`):

- `src/base/arbc/base/time.hpp:11-20` (`Time`, `flicks_per_second =
  705'600'000`), `:29-54` (`TimeRange`: `all()` at `:38-41`, `empty()` at
  `:45`, `contains()` at `:49-51`), `:26-28` (the comment naming
  `time.rational_time` as this arithmetic's owner). New `Rational`/`TimeMap`
  types live in a sibling header; span culling reuses `TimeRange`.
- `src/base/arbc/base/expected.hpp` — `expected<T, E>` / `unexpected<E>`, the
  "errors as values, never thrown, never aborting" result type (doc 10);
  the overflow boundary (Decision 3) returns `expected<…, TimeError>` in this
  idiom.
- `src/base/CMakeLists.txt:1-7` — the `arbc_add_component(NAME base …)` call
  the new header/source and `arbc_component_test(COMPONENT base SOURCES …)`
  extend (no `DEPENDS`; base is L0).
- `src/contract/arbc/contract/content.hpp:79,88-95,214,235,25-28` — the
  downstream `RenderRequest.time` / `achieved_time` / `time_extent` /
  `quantize_time` / `Stability` surface these instants ultimately feed
  (consumed by later tasks, not by this one).
- `src/model/arbc/model/records.hpp:37-38` — the reserved "span/time-map
  presence" placement flags `temporal_placement` will populate with the
  `TimeMap` type defined here.

Predecessor / sibling conventions followed:
`tasks/refinements/contract/temporal_fields.md` (the immediate sibling — base
value types, claims anchored to the owning behavior doc, deferrals stated
explicitly), `tasks/refinements/compositor/anchored_viewports.md` (numeric
property test + exact-equality vs. justified-tolerance split), and the
tier-5 numeric-invariant Catch2 style in
`src/compositor/t/anchored_viewports.t.cpp` (seeded deterministic loops, no
external generator).

## Constraints / requirements

- **Levelization (doc 17:48, CI-gated).** The math lives **inside the
  existing `arbc::base` (L0) component** — new `arbc/base/rational_time.hpp`
  (+ `.cpp`) added to base's `arbc_add_component` `PUBLIC_HEADERS`/`SOURCES`.
  It introduces **no new component and no new dependency edge** (base depends
  on nothing; everyone already depends on base). Do **not** add a `time`
  component — a `NAME` absent from `check_levels.py`'s `ALLOWED` map is a CI
  failure. `scripts/check_levels.py` must stay green.
- **Exactness never degrades with depth (doc 11:44-48).** Composition is
  evaluated entirely in rational arithmetic; the composed result of a chain
  is independent of how the chain is grouped/nested (associativity), and
  equal to the mathematically exact affine composition. **Rounding to the
  integer flick timebase happens exactly once, at the leaf** — never per
  edge. This is the load-bearing invariant; it is what the property test
  pins.
- **Rationals stay canonical.** `Rational` is always reduced to lowest terms
  with `den > 0` and sign in the numerator; `0` is `0/1`. Equality is
  value-exact on the canonical form. `TimeRange`/`Time` remain untouched
  STL-free trivially-copyable value types; `Rational`/`TimeMap` follow the
  same discipline (trivially copyable, no STL members, `constexpr`-friendly).
- **Negative rates (doc 11:59).** Reverse playback is a first-class case:
  evaluation, composition, and the leaf rounding must be correct and
  sign-symmetric for negative `rate`.
- **Faults as values (doc 10, `expected.hpp`).** A composition or evaluation
  that would overflow the fixed integer width even after reduction returns an
  `expected<…, TimeError>` error — never a silent wrap, never an abort/UB.
  Realistic rate stacks reduce and never reach this; the error path exists
  for adversarial inputs and is a tested behavior.
- **Media time, not wall clock (temporal_fields Decision).** These instants
  are content-local media time in flicks; the `steady_clock` `Deadline`
  budget stays a separate concern. This task defines no wall-clock type.
- **Scope boundary — no quantize_time here.** The content-native-grid
  quantization `Content::quantize_time()` (`content.hpp:235`, claim
  `11-time-and-video#quantize-time-matches-achieved-time`, already
  registered) is a downstream Content/`temporal_cache` concern, not this
  task. This task's only rounding is the single leaf conversion of a composed
  rational instant to the fine flick timebase. Do not re-register or
  re-implement quantize here.

## Acceptance criteria

- **Claims-register growth** (`tests/claims/registry.tsv`,
  `scripts/check_claims.py`), anchored to the owning behavior doc **doc 11**
  and cited from `src/base/t/rational_time.t.cpp` via `// enforces:`
  (descriptions single-line, present tense, ASCII):
  - `11-time-and-video#time-map-composes-in-exact-rational-with-one-rounding`
    — "A chain of per-edge time maps composed to a leaf is evaluated in exact
    rational arithmetic and converted to an integer-flick instant exactly
    once, at the leaf; the composed result equals the mathematically exact
    affine composition (including reverse playback under a negative rate) and
    is independent of how the chain is grouped, so precision does not degrade
    with depth."
  - `11-time-and-video#rational-rate-composition-is-exact` — "Rational rates
    compose as exact reduced rationals with no floating-point error across
    deep pathological stacks (e.g. repeated 24000/1001 and 1/2 nesting stays
    exact and in lowest terms), and a composition that would overflow the
    integer width even after reduction returns a TimeError value rather than
    wrapping or aborting."
  - `11-time-and-video#span-cull-is-half-open` — "A layer with span [in, out)
    in parent time is present iff in <= parent_time < out (half-open, out
    excluded); the default all() span is always present so a still is the
    degenerate case, and a degenerate span (out <= in) is present at no
    instant."
- **Tier 2 — Catch2 unit tests** (`src/base/t/rational_time.t.cpp`,
  registered in `src/base/CMakeLists.txt`'s `arbc_component_test`), exhaustive
  on edge cases with **exact-equality** assertions (integer/rational math —
  no tolerance, per doc 16:47-53): `Rational` canonicalization (zero,
  negatives, gcd reduction, `den > 0` normalization); rate multiply;
  `TimeMap::evaluate` on exactly-representable inputs; the leaf rounding rule
  (nearest flick, ties-to-even — cases straddling a half-flick and their
  negative mirrors); span `contains` half-open boundaries (`in` included,
  `out` excluded), `TimeRange::all()` always-present, degenerate span
  culls everywhere.
- **Tier 5 — numeric invariant / property test** (doc 16:63-65), a seeded
  deterministic Catch2 case (e.g. `mt19937`, no external generator per doc
  10) in the same TU: build **pathological rate stacks** and deep time-map
  chains, assert composed `evaluate` equals a rational reference computed at
  higher intermediate width, assert **grouping/associativity independence**
  (depth invariance), and assert the overflow path returns `TimeError` (never
  UB) on adversarial coprime rationals. Enforces the two composition claims.
- **No byte-exact pixel golden, no conformance suite, no behavioral
  counters in this task.** The math produces no pixels (a `*_golden.t.cpp` and
  the `16-sdlc-and-quality#byte-exact-goldens` tag apply only to rendering
  tasks); it is pure value math with no `Content` vtable, so the
  `arbc-testing` conformance suite does not apply; the "zero renders within
  one native frame" behavioral-counter promises belong to
  `time.temporal_cache` (achieved-time coalescing) and are scoped there, not
  here — stated explicitly to keep counter work out of the math task, exactly
  as `temporal_fields.md` deferred its render-pipeline criteria.
- **Diff coverage ≥90%** on the changed lines, enforced by
  `.github/workflows/ci.yml` (`diff-cover … --fail-under=90`) and the local
  `scripts/gate`. Tests are part of this task, not a follow-up.
- **Levelization + build green:** `scripts/check_levels.py` passes (no new
  edge), the `format` (clang-format `-Werror`) and `claims register` gates
  pass, and `tj3 project.tjp` is warning-free after the closer marks
  `complete 100`.

## Decisions

1. **Place the math in `arbc::base`, not a new `time` component.** Doc
   17:48 already assigns "`Time`/rational rates/time maps" to `base` (L0),
   and `time.hpp:26-28` names `time.rational_time` as base-resident. Adding a
   new header to the existing component introduces zero levelization edges.
   *Rejected:* a standalone `time` component — it would be a new node absent
   from `check_levels.py`'s `ALLOWED` map (an outright CI failure), must be
   L0 regardless, and would split two tightly-coupled value families
   (`Time`/`TimeRange` vs. `Rational`/`TimeMap`) across components for no
   gain. `time.rational_time` is a work-stream label, not a component.
2. **`Rational` = reduced `{int64 num, int64 den}`; compose via 128-bit
   intermediates + gcd reduction.** Rates multiply using a wider intermediate
   (`__int128` / checked `__builtin_mul_overflow`) then reduce to canonical
   `int64` form after every op, keeping the stored ratio minimal so realistic
   periodic rates never grow. *Rejected:* `double` rates — cannot represent
   `24000/1001` and would violate doc 11's "doesn't degrade with depth"
   exactness. *Rejected:* arbitrary-precision bignum — a new dependency
   against doc 10's minimalism policy and unnecessary, since reduction keeps
   realistic stacks bounded; the fixed-width overflow case (Decision 3)
   covers the adversarial tail more cheaply than a bignum everywhere.
3. **Overflow surfaces as an `expected<…, TimeError>` value.** A compose or
   evaluate that would exceed the integer width even after reduction returns
   an error in base's existing faults-as-values idiom
   (`expected.hpp`, doc 10). *Rejected:* saturating/clamping — silently
   returns a wrong (corrupted) time, the worst failure mode for exact math.
   *Rejected:* `assert`/abort — violates doc 10's "never aborting across the
   public API"; a pathological scene should fail honestly, not crash. This is
   a **design-doc delta** to doc 11 (§Representation), committed with the
   task, since the doc left overflow behavior unstated.
4. **Single leaf rounding = nearest flick, ties to even.** The composed
   rational instant is converted to `Time` once, at the leaf, rounding to the
   nearest flick with ties broken to even. *Rejected:* rounding per edge —
   accumulates error and directly contradicts doc 11:44-48 ("rounded to the
   timebase once, at the leaf") and doc 04's never-accumulate rule.
   *Rejected:* truncation toward −∞ (floor) — asymmetric under reverse
   playback (a negative rate would bias systematically), whereas
   ties-to-even is sign-symmetric and unbiased; floor also risks conflation
   with the *content-native-grid* `quantize_time` floor, which is a distinct
   downstream operation. Because the flick timebase divides all common rates
   exactly, this rounding is a no-op on realistic inputs and bites only on
   adversarial rationals. This is a **design-doc delta** to doc 11
   (§Representation), committed with the task, since the doc left the rounding
   mode unstated.
5. **Reuse `TimeRange` as the span type; culling is a thin half-open
   predicate.** `TimeRange::contains(parent_time)` already gives exact
   `[in, out)` half-open membership and `TimeRange::all()` the always-present
   default (`time.hpp:38-51`); the task adds only the presence check that
   gates time-map evaluation, plus the tests pinning the semantics.
   *Rejected:* a new `Span` type — `temporal_fields` deliberately placed
   `TimeRange` in base for exactly this reuse; a parallel type would
   duplicate the half-open interval for no benefit.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-07.

- `src/base/arbc/base/rational_time.hpp` — new header: `Rational` (reduced `{int64 num, int64 den}`, canonical form), `TimeMap` (per-edge affine map), `ComposedTimeMap` (accumulated rational affine, unrounded through the chain), `TimeError`, `present_in_span` half-open culling predicate.
- `src/base/rational_time.cpp` — implementation: 128-bit checked reduction, rate multiply/add with overflow-as-`TimeError`, edge-stack fold, single ties-to-even leaf rounding at the flick timebase.
- `src/base/t/rational_time.t.cpp` — tier-2 unit tests (canonicalization, exact arithmetic, leaf rounding sign-symmetric, half-open span boundaries) + tier-5 seeded `mt19937` property test (composed rate/offset vs independent higher-width reference, grouping/associativity/depth invariance, overflow-faults-as-values). 30 906 Catch2 assertions total.
- `src/base/CMakeLists.txt` — added `rational_time.hpp`/`.cpp` to `arbc_add_component(NAME base …)` and the test TU to `arbc_component_test`.
- `tests/claims/registry.tsv` — registered three scoped claims: `time-map-composes-in-exact-rational-with-one-rounding`, `rational-rate-composition-is-exact`, `span-cull-is-half-open` (all anchored to `11-time-and-video`).
- `src/base/arbc/base/expected.hpp` — added `#include <new>` (latent IWYU bug for placement-new first surfaced by this TU; add-only, safe).
- `docs/design/11-time-and-video.md` — design-doc delta: pinned leaf rounding mode (nearest flick, ties-to-even) and overflow-as-value (both were unstated in the doc; Decisions 3 and 4 in the refinement).
