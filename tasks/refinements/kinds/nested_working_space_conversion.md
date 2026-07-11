# Refinement — `kinds.nested_working_space_conversion`

## TaskJuggler entry

[`tasks/55-kinds.tji:62-67`](../../55-kinds.tji) — task `nested_working_space_conversion`,
"org.arbc.nested heterogeneous working-space conversion".

> Wire the heterogeneous nesting-boundary conversion (doc 07 rule 4): add a
> convert operation to the Backend contract, implement it in CpuBackend over
> the landed convert_kernel, invoke when child working_space() differs from
> parent's — replacing the placeholder-on-mismatch. Pin per-format byte-exact
> goldens. Docs 05/07/09. Source-of-debt: tasks/refinements/kinds/nested.md

`depends !nested`. Milestone: **m9_release** (`tasks/99-milestones.tji:71`).

## Effort estimate

`effort 2d`, `allocate team`.

This is a **seam exposure plus one call site**, not new machinery. The
arithmetic already exists and is already golden-pinned: `convert_kernel`
landed with the CPU backend and its six directed format pairs are frozen
byte-exact today (`src/backend_cpu/t/kernel_goldens.t.cpp:314-331`). Nothing
in this task writes a new pixel loop.

Day one buys the L2 surface: one pure virtual on `Backend`, its
`CpuBackend` override with the one-variant-dispatch-per-operation shape the
other four operations already use, and the surface-level goldens — which
reuse the *existing* frozen tables rather than adding new ones (see the
golden-movement prediction under Acceptance criteria). Day two buys nested's
heterogeneous path: a child-working-space intermediate, the composed-output
conversion, the counting test double that pins "homogeneous trees pay
nothing", the self-checking boundary golden, the heterogeneous conformance
and TSan fixtures, three claims rows, and the doc-09 delta.

Explicitly **untouched**: `composite`'s tag-agreement refusal (it keeps
refusing — see Constraint 5), `downsample`, `make_surface`, `BackendCaps`,
the plugin ABI, nested's audio facet, nested's per-layer cull/pull/temp
math, the cache seam, and metadata memoization.

## Inherited dependencies

**Settled predecessor this task builds on (`complete 100`):**

- `kinds.nested` — `tasks/refinements/kinds/nested.md`. It shipped the
  visual facet for **homogeneous** trees and deliberately left `Backend::convert`
  absent (`nested.md:351-359`, `:394-406`, and the Status design call at
  `:449`: "`Backend::convert` intentionally absent — deferred to
  `kinds.nested_working_space_conversion`"). This task is the named successor.

  **Read the shipped code, not the refinement's Constraint 8.** `nested.md:248-252`
  promised a placeholder render plus a `GraphDiagnostic` on mismatch. That is
  *not* what landed: `GraphDiagnostics` lives in `arbc::compositor` (L4) and
  `kind_nested` is also L4, so the doc-17 no-same-level-edges rule made it
  unreachable (`nested.md:370-386`), and the implementer downgraded it to a
  debug precondition. **This is the state you inherit**, at
  `src/kind_nested/nested_content.cpp:347-354`.

**Structural precedent (settled; mirrored for shape — not a dependency):**

- `kinds.raster_resampling_quality` — `tasks/refinements/kinds/raster_resampling_quality.md`.
  The house template for "swap/expose a kernel behind a landed seam, then pin
  it with byte-exact goldens." Two of its moves are load-bearing here: choose
  a shape that **collapses byte-for-byte to the incumbent at the trivial case**
  so pre-existing goldens survive untouched, and state a **golden-movement
  prediction** up front so any unexpected movement is a bug signal rather than
  a re-freeze prompt.

**Pending (must not be assumed at implementation time):**

Nothing. `kinds.nested` is complete and `convert_kernel` is landed; this task
is unblocked the moment it is picked. In particular it does **not** wait on
`surfaces.import`, on a GPU backend, or on `kinds.nested_runtime_binding` —
nested's tests inject the `Backend&` manually today and that is sufficient.

## What this task is

`org.arbc.nested` renders a child composition by re-expressing the
compositor's per-layer loop and source-over'ing each child layer into the
parent's target surface. Doc 07 rule 2 requires that *all compositing happen
in the composition's working space*, and doc 07 rule 4 says the nesting
boundary is a conversion point: **the child's composed output converts into
the parent's working space.** Today nested only honors the homogeneous case
(child working space == parent's); the heterogeneous case is a debug
precondition assert and, in release, degrades to a transparent target.

This task makes the heterogeneous boundary real, in three pieces:

1. **`Backend::convert`** — a new pure virtual on the L2 backend contract
   (`src/surface/arbc/surface/backend.hpp`): same-geometry,
   position-for-position rewrite of a source surface's pixels into the
   destination surface's tag triple.
2. **`CpuBackend::convert`** — the override, dispatching once per operation
   over the source/destination format pair and calling the already-landed
   `convert_kernel` (`src/backend_cpu/kernels.hpp:153-164`).
3. **Nested's heterogeneous path** — when the child composition's
   `working_space` differs from the render target's format, compose the
   child's layers into a **child-working-space intermediate** the same size
   as the target, then `convert` that composed result into the target once.

**Out of scope:** cross-tag convert-*at-composite* for content-provided
(imported) surfaces (doc 09:149-156 defers it to the multi-format/GPU backend
work and explicitly calls it "a parking-lot item tied to that capability, not
a standalone task"); nested's audio facet, whose rate/channel conversion is a
separate seam already handled at `src/kind_nested/nested_content.cpp:467-494`
with its own deferred successor (`kinds.nested_audio_resampling`); the
display-out and import consumers of `convert_kernel` named at
`src/backend_cpu/kernels.hpp:150-152`.

## Why it needs to be done

Doc 07 rule 4 is a promise the codebase does not yet keep. A user who sets a
child composition's working space to the `RGBA8` fast mode (doc 07 rule 3's
"configured fast mode for memory-constrained embedders") and nests it inside
a linear-f16 parent gets, today, a transparent hole — every per-layer
`composite` no-ops on the tag mismatch
(`src/backend_cpu/cpu_backend.cpp:76-79`). That is honest degradation rather
than corruption, which is why it was allowed to ship, but it is not the
designed behavior.

Beyond nested, this task lands the **first extension of the `Backend`
contract** and therefore the interface that doc 07's other two conversion
consumers will reuse. `src/backend_cpu/kernels.hpp:150-152` names them
directly: *"This is the kernel the later edge tasks (imports, nesting
boundary, display-out) wire into their call sites."* The signature chosen here
is the one those tasks inherit, so it must not be nested-shaped.

There is also a doc-05 argument. Doc 05:28-31: nested "is implemented purely
against public interfaces … it doubles as the proof that third-party plugins
have enough API surface to do anything the core can do." A third-party kind
facing a heterogeneous child must be able to convert. That forecloses solving
this with a nested-private helper — the operation has to be on the public
contract, which is exactly where doc 17 already scopes it.

## Inputs / context

### Governing design-doc sections (normative, doc 16)

- **`docs/design/07-color-and-pixel-formats.md:32-35` — rule 4, the mandate:**

  > 4. Nested compositions may declare different working spaces; the nesting
  >    boundary is a conversion point like any other content (the child's
  >    composed output converts into the parent's working space). Homogeneous
  >    trees pay nothing.

  Two words do a lot of work. **"composed output"** — it is the child's
  *finished* image that converts, once, not its individual layers. And
  **"pay nothing"** is a performance carve-out (skip the conversion when the
  tags already agree), not a semantic escape hatch: rule 1 (`07:16-18`,
  "Every surface is tagged") forecloses tag-through independently.
- **`docs/design/07-color-and-pixel-formats.md:19-24` — rule 2:** "**All
  compositing happens in the composition's working space** … Layers are asked
  to render *into the working space* (the `RenderRequest`'s target surface
  carries the tag)." Read with rule 4, this settles *where* the child's layers
  blend: in the **child's** working space, because they are the **child**
  composition's layers. See Decision 1.
- **`docs/design/07-color-and-pixel-formats.md:104-108`:** "Cross-format
  compositing (mixed-working-space nesting, rule 4) instantiates conversion
  kernels over format *pairs*; the set stays small because the format list is
  closed and conversions route through the working space rather than N×N."
- **`docs/design/07-color-and-pixel-formats.md:79-95`:** kernels are
  `template <PixelFormatDesc F>` function templates with **one `std::visit`
  dispatch per *operation*, never per pixel**, "Internal to the CPU backend —
  never visible to plugins."
- **`docs/design/09-surfaces-and-backends.md:24-27` — the operation is already
  promised:** "A backend implements: surface allocation and pooling, the
  composite operation set …, **format/space conversion kernels (doc 07)**, and
  capability flags." The delta below spells the operation out; it does not
  invent it.
- **`docs/design/09-surfaces-and-backends.md:55-60`** — surface creation is
  errors-as-values; the closed core-owned format set "makes the answer a
  decision over an enumerable universe." This is what makes `convert`
  infallible by construction (Decision 3).
- **`docs/design/09-surfaces-and-backends.md:149-156`** — the deferral of
  cross-tag convert-at-composite. **This clause survives this task**; the
  delta narrows its wording so a future reader does not read it as stale. See
  Constraint 5.
- **`docs/design/09-surfaces-and-backends.md:158-166`** — the threading note:
  allocation and composite run on the render thread. `convert` inherits that
  confinement.
- **`docs/design/05-recursive-composition.md:22-31`** — "Rendering *is*
  recursion", and the public-interfaces-only argument quoted above.
- **`docs/design/17-internal-components.md:51`** — `arbc::surface` | L2 |
  "`Surface` handles, **the backend contract**, external import + sync tokens,
  **format conversion *interfaces***" | doc 09 | depends `base, media`. The
  interface is already scoped to L2 by name: **this task creates no new
  dependency edge.**
- **`docs/design/17-internal-components.md:55`** — `arbc::backend-cpu` | L3 |
  "format-templated kernels + variant dispatch". And **`:81-98`**: "if it needs
  `PixelTraits<F>` or a `Surface`, it is `backend-cpu`; if it is arithmetic on
  decoded working values, it is `media`." `convert_kernel` names
  `PixelTraits<F>` → it stays in `backend-cpu`. Confirmed, not moved.
- **`docs/design/17-internal-components.md:41-44`** — "A component may depend
  only on strictly lower levels. No same-level edges." This is why
  `kind-nested` (L4) may call `Backend::convert` through its injected handle
  but may never name `CpuBackend` or `convert_kernel`, and why the
  mismatch diagnostic could not go through `GraphDiagnostics` (L4→L4).
- **`docs/design/16-sdlc-and-quality.md:31-44`** — the contract conformance
  suite, tier 1. **`:49-53`** — "The CPU backend is specified deterministic
  (fixed FP flags, no FMA variance in kernels, ordered reductions), so goldens
  are **byte-exact**."

### Real source seams (paths + lines, at HEAD `312fbec`)

- **`src/surface/arbc/surface/backend.hpp:16-52`** — the `Backend` abstract
  base. Five pure virtuals: `capabilities()` (`:26`), `make_surface()`
  (`:31-32`), `clear()` (`:35`), `composite()` (`:39-40`), `downsample()`
  (`:48`). Plain C++ abstract class — a new pure virtual is a recompile, not
  an ABI break: the plugin ABI is `arbc_plugin_register(Registry&)`
  (`src/contract/arbc/contract/plugin.hpp:20`) and **plugins never receive a
  `Backend&`**. `downsample`'s doc comment (`:42-48`) is the citation style to
  mirror.
- **`src/backend_cpu/cpu_backend.cpp:69-93`** — `CpuBackend::composite`. Note
  `:76-79`: on tag mismatch it asserts in debug and **returns (no-ops) in
  release** — "a caller error, never a silent reinterpretation." Its comment
  at `:71-75` already names this task: "converting between differing tags
  routes through convert_kernel and is wired by the edge tasks (imports,
  nesting, display-out), not here."
- **`src/backend_cpu/cpu_backend.cpp:95-124`** — `CpuBackend::downsample`: the
  exact shape to copy for `convert` (precondition assert + release cull, then
  one `visit_surface` dispatch resolving the runtime tag to a compile-time
  format, then the monomorphized kernel).
- **`src/backend_cpu/kernels.hpp:150-164`** — `convert_kernel<SrcF, DstF>`,
  taking `std::span<const PixelTraits<SrcF>::Storage>` → `TypedSpan<DstF>` over
  `pixel_count` pixels. **Flat, same-geometry, position-for-position** — it does
  not resample. Routes format → premultiplied linear `WorkingPixel` → format
  (2N codecs). `static_assert(SrcTraits::channels == DstTraits::channels)`. It
  lives in the *private* header, invisible outside `backend-cpu`.
- **`src/kind_nested/nested_content.cpp:328-354`** — `NestedContent::render`.
  `:338` clears the target; `:340-345` is the unresolved-child placeholder
  (`return RenderResult{request.scale, true}` over the cleared target — the
  honest-empty precedent this task reuses); **`:347-354` is the precondition
  assert this task replaces.**
- **`src/kind_nested/nested_content.cpp:238-326`** — `compose_child_layer`, the
  per-layer loop. `:286-287` allocates the per-layer temp at `target.format()`;
  `:305` pulls through the injected `PullService`; `:325` composites the temp
  into the target. In the heterogeneous path, the *only* change is which
  surface plays the role of "target" — see Decision 2.
- **`src/kind_nested/arbc/kind_nested/nested_content.hpp:35-46`** — the header
  comment scoping nested to "the VISUAL facet, HOMOGENEOUS working-space
  trees", with the deferral note at `:44-46`. Both need updating.
- **`src/model/arbc/model/records.hpp:139`** — `SurfaceFormat working_space{};`
  on `CompositionRecord`. This is `comp->working_space`, read from the pinned
  `DocRoot`. There is **no `working_space()` on `Content`** — a content renders
  into whatever tag it is handed (`src/contract/arbc/contract/content.hpp:114`).
  The WBS note's phrase "child `working_space()`" means the child
  *composition record's* working space.
- **`src/media/arbc/media/surface_format.hpp:26-49`** — `SurfaceFormat` is a
  tag triple (`PixelFormat`, `ColorSpace{Primaries, TransferFunction}`,
  `Premultiplied`) with member-wise `operator==`. Three named values exist:
  `k_working_rgba32f` (`:36`, the default), `k_working_rgba16f` (`:42`),
  `k_fast_rgba8srgb` (`:49`) — matching `PixelFormat`'s three members
  (`Rgba32fLinearPremul`, `Rgba16fLinearPremul`, `Rgba8Srgb`). So the universe
  is 3 formats → 9 directed pairs → 3 identity + 6 cross.
- **`src/backend_cpu/t/kernel_goldens.t.cpp`** — the byte-exact kernel suite.
  `convert_bytes<SrcF,DstF>` builder at `:164-169`, the FROZEN EXPECTED TABLES
  block at `:175`, the convert tables at `~:225`, and
  `TEST_CASE("convert_kernel is byte-exact for every directed format pair")` at
  `:314-331` covering all six cross pairs. Regen procedure at `:41-53`
  (`[.regen]` hidden tag, `GCOV_EXCL`-wrapped dump case at `:413`).
- **`tests/nested_goldens.t.cpp:24-28`** — nested's goldens are **self-checking**:
  nested's output is compared against the compositor's own `render_frame` over
  the same layers, "so there are no frozen tables to regenerate: the 'rendering
  is recursion' identity (doc 05:24) is the oracle." This oracle extends
  cleanly to the heterogeneous case (Acceptance criteria).
- **`tests/nested_conformance.t.cpp:32-45`** (`InlinePull`), **`:171-180`**
  (`arbc::contract_tests(fx.factory(), options)`). Cross-component, so it lives
  in `tests/`, not `src/kind_nested/t/`.
- **`tests/claims/registry.tsv`** — `:33` `#surfaces-carry-tags`, `:38`
  `#conversions-route-through-working-space`, `:41` `#capabilities-are-honest`,
  `:47` `#kernels-byte-exact-per-format` (which **already names "cross-format
  convert"** among the kernels required byte-identical).
- **`scripts/check_levels.py`** (`ALLOWED` map, `:17-41`) and
  `scripts/check_claims.py` (bidirectional, `:39-42`).

### Predecessor decisions carried forward

From `nested.md`: nested may not name `arbc::compositor` (L4→L4), so the
honest channel for a boundary failure is a **value out of the L2 `Backend`**
or an honest-empty placeholder — never a `GraphDiagnostic` (`nested.md:370-386`).
Nested reuses the injected `PullService`, never `content->render`
(`nested.md`, doc 13:69-71). The sub-`RenderRequest` carries the outer
request's snapshot, exactness, and deadline **verbatim** (`nested.md`
constraint 2) — the heterogeneous path must not disturb this.

## Constraints / requirements

1. **`convert` goes on `Backend` in `arbc::surface` (L2).** Doc 17:51 already
   scopes "format conversion *interfaces*" there. `CpuBackend` (L3) implements
   it over `convert_kernel` (L3). `kind-nested` (L4) calls it through the
   `Backend&` it is already injected at attach
   (`nested_content.hpp:68`). **No new dependency edge is created, in either
   the CMake target graph or the include graph** — verify with
   `scripts/check_levels.py`, do not edit it.

2. **The child composites in the child's working space; the composed output
   converts once.** Doc 07 rule 2 + rule 4. Concretely: on mismatch, allocate a
   `comp->working_space`-tagged intermediate with the **same dimensions as the
   request's target**, clear it, run the existing per-layer loop into it
   (unchanged — every per-layer temp then follows the intermediate's tag, since
   `compose_child_layer` already allocates its temp at its target's format),
   then `backend.convert(target, intermediate)` exactly once. **One conversion
   per nested render, never one per layer.**

3. **`convert` is same-geometry and replacing, not blending.** `dst` dims ==
   `src` dims (the kernel is position-for-position); every destination pixel is
   overwritten with the transcoded source pixel. It is not a compositing
   operation and takes no transform and no opacity. Mismatched dimensions are a
   caller error: debug assert, release cull — the exact convention `composite`
   and `downsample` already use (`cpu_backend.cpp:76-79`, `:104-110`).

4. **One variant dispatch per operation, never per pixel** (doc 07:79-95).
   `CpuBackend::convert` resolves the `(src, dst)` runtime tag pair to
   compile-time formats once, then runs the monomorphized `convert_kernel`.
   The dispatch must be **total over the 9 directed pairs** — the format set is
   closed and core-owned (doc 07:110-115), so there is no default-case hole.
   Equal-tag pairs are an **exact copy**, not a decode/encode round-trip (see
   Decision 4).

5. **`composite` keeps refusing mismatched tags.** This task does *not* make
   `composite` cross-tag. The sanctioned shape is **convert-then-composite**
   (an explicit conversion into a correctly-tagged surface, then the existing
   tag-agreeing composite), which leaves both claim `07-…#surfaces-carry-tags`
   (`registry.tsv:33`, "the reference backend refuses to composite across
   mismatched tags") and doc 09:149-156's deferral of cross-tag
   convert-*at-composite* intact. Its test stays in the regression envelope and
   must still pass unchanged.

6. **The homogeneous path is byte-identical to today's, and pays nothing.**
   When `comp->working_space == target.format()`, the code takes exactly the
   current path: no intermediate allocation, no `convert` call, no extra clear.
   This is doc 07 rule 4's "Homogeneous trees pay nothing", and it is what keeps
   every existing nested golden frozen (see the prediction table below).

7. **A backend that cannot store the child's working space degrades honestly.**
   `make_surface` for the intermediate returns `expected<…, SurfaceError>`
   (doc 09:55-60). On error, return the honest-empty placeholder —
   `RenderResult{request.scale, true}` over the already-cleared target — exactly
   as the unresolved-child path does (`nested_content.cpp:341-345`) and as the
   per-layer unstorable-format path does (`:286-290`). No crash, no wrong
   pixels, no `GraphDiagnostic` (L4→L4 forbidden).

8. **The intermediate is allocated through `backend.make_surface`, not a
   `SurfacePool`.** `SurfacePool` is compositor-owned and render-thread-confined
   (doc 09:62-78); `kind-nested` is L4 and cannot reach it. This mirrors how
   nested already allocates its per-layer temps (`nested_content.cpp:286-287`).
   The intermediate is owned by the `render` call frame and never outlives it.

9. **Render-thread confinement holds** (doc 09:158-166). The intermediate is a
   stack-local `unique_ptr` in one `render` invocation, and `convert` runs on
   the frame thread alongside the existing `clear`/`composite`. No new shared
   mutable state, no new lock. This must be *demonstrated*, not assumed — see
   the TSan scope under Acceptance criteria.

10. **The sub-request invariants survive.** Snapshot, exactness, and deadline
    still pass through verbatim; `achieved_scale` honesty math, the
    recursion-depth backstop, the sub-pixel cull, and the async-layer
    placeholder are all untouched. The async placeholder is worth one thought:
    a deferred layer leaves its region transparent in the intermediate, and
    premultiplied transparent (0,0,0,0) converts to premultiplied transparent
    in every format — so the placeholder stays a placeholder across the
    boundary.

11. **Signature neutrality.** `convert` is inherited by the import and
    display-out call sites (`kernels.hpp:150-152`). It must be
    `(Surface& dst, const Surface& src)` — no nested-specific parameters, no
    region, no transform.

## Acceptance criteria

### The golden-movement prediction (the primary correctness gate)

The heterogeneous path is new code on a branch nothing currently takes, and
the homogeneous path is untouched. Therefore:

| Frozen / existing golden | Expected |
| --- | --- |
| `src/backend_cpu/t/kernel_goldens.t.cpp` — the convert tables (`~:225`) and `TEST_CASE(...convert_kernel is byte-exact...)` (`:314-331`) | **unchanged** — `Backend::convert` routes to the same `convert_kernel`; the new surface-level test asserts against these *exact same* tables |
| `kernel_goldens.t.cpp` — fill / source-over / box-downsample tables | **unchanged** — those kernels are not touched |
| `tests/nested_goldens.t.cpp` | **unchanged** — self-checking, homogeneous fixtures, and the homogeneous path is byte-identical (Constraint 6) |
| `src/kind_raster/t/raster_goldens.t.cpp`, `tests/crossfade_goldens.t.cpp`, `tests/fade_goldens.t.cpp` | **unchanged** — not on this path |

**No new frozen tables are added, and no existing table is regenerated.** Any
golden movement at all means the homogeneous fast path is not byte-identical
to the incumbent, or `Backend::convert` is not routing to `convert_kernel` —
in either case the implementation is wrong. Do not re-freeze; fix it.

### New test coverage

1. **Surface-level `Backend::convert` goldens** (extend
   `src/backend_cpu/t/kernel_goldens.t.cpp`, in-component). For all six
   directed cross-format pairs: build a source `Surface` through
   `make_surface`, fill it with the same fixed input the kernel test uses,
   `CpuBackend::convert` into a destination surface of the other format, and
   `memcmp` against the **existing** frozen expected table. This is what proves
   the L2 operation is a faithful wrapper and not a second, divergent
   implementation. It reuses the frozen data, so it adds no regeneration
   surface.
2. **Equal-tag identity golden.** `convert` with `src.format() == dst.format()`
   is byte-identical to the source for all three formats — including
   `Rgba8Srgb`, where a decode/encode round-trip would *not* be guaranteed
   byte-exact (Decision 4). This is the test that stops the identity case from
   silently quantizing.
3. **Dimension-mismatch cull.** Debug-assert / release-no-op on mismatched
   dimensions, matching `downsample`'s existing convention test.
4. **The heterogeneous nesting-boundary golden** (new fixture in
   `tests/nested_goldens.t.cpp`, cross-component — it needs `kind_nested` (L4)
   and `CpuBackend` (L3) together, so it cannot live in `src/kind_nested/t/`).
   **Self-checking, no frozen table**, extending the file's existing oracle:
   render the child composition through the *compositor's* `render_frame` into
   a surface tagged with the **child's** working space, `backend.convert` that
   into a parent-tagged surface, and assert nested's heterogeneous output is
   **byte-identical**. This is the "rendering is recursion" identity of
   `nested_goldens.t.cpp:24-28` composed with doc 07 rule 4, and it pins the
   whole design in one equality.
5. **The discriminator.** A golden alone cannot distinguish "composed in the
   child's space then converted" from "composed directly in the parent's
   space." Add the explicit inequality: for a child whose working space is
   `k_fast_rgba8srgb` with two overlapping semi-transparent layers, the two
   images **differ** (blending in nonlinear sRGB8 is not blending in linear
   f16 — doc 07 rule 3 is precisely the statement that these are different
   images). Assert the difference, so a regression to per-layer conversion or
   to compose-in-parent-space fails loudly rather than passing a golden that
   was frozen from the wrong pipeline.
6. **Behavioral counters — "homogeneous trees pay nothing".** This is a
   performance-shaped promise, so it gets counters, never wall-clock (doc 16).
   Add a small counting `Backend` decorator in `tests/` wrapping `CpuBackend`
   and tallying `convert` and `make_surface` calls. Assert: a **homogeneous**
   nested render issues **zero** `convert` calls and allocates **zero**
   surfaces beyond the per-layer temps it allocates today; a **heterogeneous**
   render of the same scene issues **exactly one** `convert` and **exactly one**
   additional (full-size) surface — *not* one per layer. Drive it with a
   multi-layer child so "one, not N" is a real assertion.
7. **Conformance suite, heterogeneous fixture.** `org.arbc.nested` is a content
   kind, so it runs the contract conformance suite (doc 16 tier 1). Add a
   second factory configuration to `tests/nested_conformance.t.cpp` whose child
   composition declares `k_fast_rgba8srgb` while the suite renders into the
   default working target, and run the full `arbc::contract_tests(factory,
   options)` over it. Render purity must still hold **byte-exactly** across the
   converted intermediate (the CPU backend is specified deterministic, doc
   16:49-53); bounds, scale, and time honesty and the operator-graph families
   must all still pass. (Confirm the suite's target tag when wiring the
   fixture — the mismatch must be real, not accidentally homogeneous.)
8. **Concurrency.** Extend `tests/nested_concurrency.t.cpp` with a
   heterogeneous fixture and run the existing stress case under **TSan**. The
   claim is narrow and worth pinning: the added allocation and conversion are
   frame-thread-local (Constraint 9), so a heterogeneous child introduces no
   new race relative to the homogeneous child the test already exercises
   concurrently.

### Claims-register entries

Three new rows in `tests/claims/registry.tsv`, each with an
`// enforces: <claim-id>` tagged test. House style is the "decisively not X"
construction (`registry.tsv:38`, `:47`) — it is what stops a degenerate
implementation from satisfying the claim.

1. **`07-color-and-pixel-formats#nesting-boundary-converts-composed-output`** —
   *A nested child whose composition declares a different working space
   composites its own layers in its own declared working space and the composed
   result converts exactly once into the parent's working space at the boundary
   — decisively not blending the child's layers in the parent's space, and
   decisively not one conversion per child layer.* Enforced by the self-checking
   boundary golden (4), the discriminator (5), and the one-convert counter (6).
2. **`07-color-and-pixel-formats#homogeneous-trees-pay-nothing`** — *A nested
   child whose working space equals the parent's issues zero conversions and
   allocates no boundary intermediate: the homogeneous render is byte-identical
   to, and allocates exactly as much as, the render that predates the
   conversion path.* Enforced by the counting-backend test (6).
3. **`09-surfaces-and-backends#convert-is-same-geometry-replace`** —
   *`Backend::convert` rewrites the source surface into the destination's tag
   triple position-for-position and replaces every destination pixel: it takes
   no transform and no opacity, blends nothing, and for equal tags is an exact
   byte-for-byte copy — decisively not a source-over composite and decisively
   not a decode/encode round-trip.* Enforced by (1), (2), (3).

**Re-asserted (a second `enforces:` test each, no new registry row):**

- `07-color-and-pixel-formats#conversions-route-through-working-space`
  (`registry.tsv:38`) — now also through the L2 `Backend::convert`.
- `07-color-and-pixel-formats#kernels-byte-exact-per-format` (`registry.tsv:47`)
  — it **already names "cross-format convert"**; the surface-level goldens (1)
  are a second enforcer, not a new claim.
- `07-color-and-pixel-formats#surfaces-carry-tags` (`registry.tsv:33`) — its
  "refuses to composite across mismatched tags" half must still pass unchanged
  (Constraint 5).

### Design-doc deltas

There is **no design gap here.** Doc 07 rules 2 and 4 already mandate the
behavior, doc 09:24-27 already promises the operation, and doc 17:51 already
scopes the interface to L2. The docs authorize this design as written; the
refinement's job is to build it, not to amend them.

One delta is required, and it **lands with the implementation commit** (doc
16:23-26's same-commit rule — writing it now would make doc 09 describe an
operation that does not exist yet). In
`docs/design/09-surfaces-and-backends.md`, under `## Backend contract`, after
the paragraph at `:24-27`:

> **The conversion operation.** `Backend::convert(dst, src)` rewrites `src`'s
> pixels into `dst`'s tag triple: same geometry, position-for-position,
> replacing every destination pixel. It blends nothing and takes neither a
> transform nor an opacity — it is a transcode, and the compositing operation
> set stays separate from it. Conversion routes format → premultiplied linear
> working float → format (doc 07:104-108), so the CPU backend needs 2N codecs
> rather than N×N kernels, and one variant dispatch per operation resolves the
> source/destination pair. It is infallible: the closed, core-owned format set
> makes the dispatch total, and the only unsupported-format failure — *can this
> backend store that tag at all?* — is already surfaced as a value by
> `make_surface`. A caller therefore holds two live surfaces before it can
> call `convert`, and by then the answer is yes. This is the operation doc 07
> rule 4's nesting boundary uses, and the one the import and display-out edges
> will reuse.

And amend item 3 of the realization addendum (`:149-156`) so its deferral reads
as scoped to convert-*at-composite*, not to conversion as such:

> 3. **v1 requires a working-space tag on a *provided* surface.** The
>    tag-compatibility clause above (a differently-tagged provided surface
>    converting **at composite time**) is latent until a backend advertises a
>    second storable format: the CPU backend asserts tag agreement in
>    `composite`, so v1 requires the provided surface to carry the composition
>    working-space tag. Note this is narrower than it looks — explicit
>    conversion *is* available (`Backend::convert`, above), and the nesting
>    boundary uses it (doc 07 rule 4, `kinds.nested_working_space_conversion`);
>    what remains deferred is folding the conversion *into* `composite` so a
>    mismatched source needs no explicit step. That lands with the
>    multi-format/GPU backend work (`color.kernels` / GPU backends) — a
>    parking-lot item tied to that capability, not a standalone task.

No doc-00 decision-record bullet: this implements an existing decision rather
than making a project-shaping one.

### CI gates

Levelization (`scripts/check_levels.py`) and include hygiene pass **without
edits to the allowed-edge map** — a required edit would mean the operation
landed at the wrong level. Claims check (`scripts/check_claims.py`) passes
bidirectionally. Diff coverage ≥90% on changed lines: note that Constraint 3's
release-cull branches are the usual coverage hazard, so cover them (the
`downsample` convention tests show how). Benchmarks clean; examples still
building (doc 16:138-142).

### Deferred follow-ups

**None.** The task closes its own scope. Two things it deliberately does *not*
spawn a WBS leaf for:

- **Cross-tag convert-at-composite** — doc 09:154-156 already rules that it
  "lands with the multi-format/GPU backend work … a parking-lot item tied to
  that capability, **not a standalone task**." Registering it would contradict
  the doc.
- **The import and display-out consumers** of `convert_kernel`
  (`kernels.hpp:150-152`) — those are their own seams with their own tasks;
  this task only guarantees the interface they will reuse is not
  nested-shaped (Constraint 11).

## Decisions

- **The child's layers blend in the child's working space; the composed output
  converts once.** This is the reading doc 07 forces: rule 2 says *all
  compositing happens in the composition's working space*, and the child's
  layers belong to the **child** composition; rule 4 says it is the child's
  *composed output* that converts. So the heterogeneous path allocates a
  child-tagged, target-sized intermediate, runs the existing per-layer loop
  into it, and converts once.

  *Alternative rejected:* **convert each per-layer temp into the parent's
  format and composite as today** (i.e. keep compositing in the parent's space,
  just transcode each child layer on the way in). It is a strictly smaller
  diff — `compose_child_layer` would allocate its temp at the child's format
  and convert before `:325` — which is precisely why it is tempting. But it is
  **wrong**, not merely slower. It blends the child's layers in the *parent's*
  space, which contradicts rule 2 and produces a visibly different image: a
  child composition declaring the `RGBA8`-sRGB fast mode is *asking* for its
  layers to blend in that space (with the artifacts doc 07 rule 3 names), and
  blending them in linear f16 instead silently upgrades the image the user
  configured. It also costs N conversions instead of 1. It fails the
  discriminator test in Acceptance criteria (5) by construction — which is why
  that test exists.

- **`convert` is a new pure virtual on `Backend` (L2), not a nested-private
  helper and not a free function in `backend-cpu`.** Doc 17:51 already scopes
  "format conversion *interfaces*" to `arbc::surface`, so the edge is
  pre-authorized; doc 05:28-31 requires that a third-party kind be able to do
  everything the core kind can, which forecloses a private helper; and
  `kind-nested` (L4) is forbidden from naming `backend-cpu` (L3 kernels) or
  including its private `kernels.hpp` at all, which forecloses the free
  function. The `Backend&` nested already receives at attach is the only handle
  it has, and it is exactly the right one.

  *Alternative rejected:* a `ConversionService` injected alongside `PullService`
  and `Backend`. It avoids growing the `Backend` vtable, but it invents a second
  seam for something doc 09:24-27 already lists as part of the backend's job,
  and a GPU backend would have to implement it anyway (its conversion is a
  shader, inseparable from its surfaces) — so the second seam would end up
  being a `Backend` in all but name.

- **`convert` returns `void` and is infallible.** It mirrors `clear`,
  `composite`, and `downsample`. The format set is closed and core-owned (doc
  07:110-115), so the 9-pair dispatch is total; the only real failure — *can
  this backend store that tag at all?* — is already an errors-as-value on
  `make_surface` (doc 09:55-60), and a caller cannot reach `convert` without
  two live `Surface` handles, i.e. without both answers already being yes. So
  the mismatch that matters is caught earlier, and nested handles it as an
  honest-empty placeholder (Constraint 7).

  *Alternative rejected:* `expected<void, SurfaceError>`. It looks more honest
  but adds an error path no caller can trigger — dead code that also fails the
  ≥90% diff-coverage gate on an unreachable branch — and it duplicates a
  guarantee `make_surface` already carries. *Alternative rejected:* a
  `BackendCaps` bit for conversion support. Capability flags describe what a
  backend *optionally* offers (import handles, sync primitives); conversion is
  mandatory — doc 07 rule 4 makes it so — and a backend that could decline it
  could not render a heterogeneous tree at all. Leaving `BackendCaps` alone also
  leaves claim `09-…#capabilities-are-honest` (`registry.tsv:41`) untouched.

- **Equal-tag `convert` is an exact copy, not a decode/encode round-trip.** The
  dispatch is total over 9 pairs, so the 3 identity pairs must do *something*.
  Specifying them as a byte-for-byte copy costs three lines
  (`if (src.format() == dst.format())`) and buys a clean contract property:
  `convert` never perturbs pixels it has no reason to perturb. The alternative —
  letting the identity pairs fall through to `convert_kernel<F, F>` — would
  decode to linear float and re-encode, which is exactly byte-identical for the
  float formats but **not guaranteed so for `Rgba8Srgb`**, where the sRGB
  encode's rounding could move a low bit. Nested never takes this path (it has
  the homogeneous fast path), but the *contract* is public, and a public
  operation that silently quantizes on a no-op is a trap for the import and
  display-out callers who inherit it. Pinned by Acceptance criteria (2).

- **The mismatch degrades to the honest-empty placeholder when the backend
  cannot store the child's working space** — not a diagnostic, not an abort.
  `GraphDiagnostics` is unreachable from L4 (`nested.md:370-386`), and both
  existing nested failure paths (unresolved child, unstorable per-layer temp)
  already return transparent pixels with `RenderResult{request.scale, true}`.
  Consistency with the established precedent beats inventing a third failure
  idiom for a case that, on the CPU backend, cannot currently occur (it stores
  all three formats).

- **Reuse the existing frozen tables; add no new ones.** The surface-level
  `Backend::convert` goldens assert against the very tables
  `kernel_goldens.t.cpp` already freezes for `convert_kernel`, and the
  nesting-boundary golden is self-checking against the compositor. This follows
  `raster_resampling_quality`'s "collapse to the incumbent baseline" move: it
  makes *any* golden movement a bug signal rather than a re-freeze prompt, and
  it means this task adds zero new regeneration surface. The frozen-table
  approach is reserved for arithmetic that has no independent oracle;
  `Backend::convert` has one (the kernel it wraps), and nested has one (the
  compositor it re-expresses).

## Open questions

(none — all decided.)

Two non-blocking observations for the closer, surfaced here rather than
encoded as WBS work:

- The WBS note's phrase *"replacing the placeholder-on-mismatch"* is
  imprecise about what shipped. There is no explicit placeholder branch for the
  mismatch: `nested_content.cpp:347-354` is a debug **precondition assert**, and
  the placeholder-like behavior in release is emergent — every per-layer
  `composite` no-ops on the tag mismatch (`cpu_backend.cpp:76-79`), leaving the
  target transparent. The effect matches the note (honest empty pixels, no
  corruption), so no bug is being reported here; the note is just describing a
  mechanism that doesn't exist. Nothing to fix in the `.tji`; recorded so the
  implementer does not go hunting for a branch to delete.
- `nested.md`'s Constraint 8 (a `GraphDiagnostic` on mismatch) was overridden by
  its own Status block and never shipped. It stays in the historical record per
  the refinement ritual, but a reader who takes it at face value will look for
  diagnostics machinery that isn't there. This refinement's Inherited
  dependencies section flags it explicitly.

## Status

**Done** — 2026-07-11.

- `src/surface/arbc/surface/backend.hpp`: new pure-virtual `Backend::convert(dst, src)` on the L2 contract.
- `src/backend_cpu/arbc/backend_cpu/cpu_backend.hpp`, `src/backend_cpu/cpu_backend.cpp`: `CpuBackend::convert` override — geometry cull, equal-tag exact-copy (`if constexpr SrcF == DstF`), 9-pair dispatch into `convert_kernel` for all 6 cross pairs.
- `src/kind_nested/arbc/kind_nested/nested_content.hpp`, `src/kind_nested/nested_content.cpp`: heterogeneous boundary — child-tagged target-sized intermediate, per-layer loop unchanged, one `convert` at end; honest-empty on unstorable working space.
- `src/backend_cpu/t/kernel_goldens.t.cpp`: surface-level `Backend::convert` goldens for all 6 directed cross pairs reusing existing frozen tables; equal-tag exact-copy (inc. sRGB8 round-trip inequality); dimension-mismatch cull.
- `src/surface/arbc/surface/testing/stub_backend.hpp` (new), `src/surface/CMakeLists.txt`: `arbc::testing::StubBackend` no-op base for test doubles; `arbc::surface::testing` CMake target.
- `tests/nested_goldens.t.cpp`: self-checking heterogeneous nesting-boundary golden (4 sections) + discriminator inequality (child-space ≠ parent-space blending).
- `tests/nested_conformance.t.cpp`: heterogeneous factory fixture over full `arbc::contract_tests`.
- `tests/nested_concurrency.t.cpp`: heterogeneous TSan stress section.
- `tests/claims/registry.tsv`: 3 new rows — `07-…#nesting-boundary-converts-composed-output`, `07-…#homogeneous-trees-pay-nothing`, `09-…#convert-is-same-geometry-replace`.
- `docs/design/09-surfaces-and-backends.md`: conversion-operation paragraph + narrowed item-3 deferral wording (same-commit delta per doc 16:23-26).
- 18 Backend test doubles (across `src/compositor/t/`, `src/kind_nested/t/`, `src/runtime/t/`, `src/surface/t/`, `tests/`, `tests/ci_plugins/`, `src/runtime/`): stub `convert` overrides added.
