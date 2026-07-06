# color.kernel_goldens — Per-format golden kernel tests

## TaskJuggler entry

`tasks/15-color.tji:35-40` — `color.kernel_goldens` ("Per-format golden
kernel tests"). Back-link:

```
task kernel_goldens "Per-format golden kernel tests" {
  effort 1d
  allocate team
  depends !kernels, !resampling
  note "Byte-exact goldens per format for blend/convert/resample; the doc 16
        determinism discipline applied at kernel granularity."
}
```

## Effort estimate

**1d.** A test-authoring task standing entirely on finished kernels — no new
production code. The work is: assemble representative fixed inputs, freeze
their byte-exact kernel outputs across the three formats and the codec layer,
wire one new component-test target, register one claim, and document the
regenerate procedure. The kernels, codecs, and dispatch seam already exist
and are covered by property tests; this task adds the absolute-reference
determinism layer on top.

## Inherited dependencies

**Settled (both `complete 100`, Done 2026-07-05):**

- `color.kernels` (`tasks/refinements/color/kernels.md`) — landed the
  format-templated kernels in `src/backend_cpu/kernels.hpp`
  (`fill_kernel`, `source_over_kernel`, `convert_kernel`) and the codec
  layer in `src/media/arbc/media/pixel_traits.hpp` (`WorkingPixel`,
  software f16, sRGB EOTF, premultiply/unpremultiply, unorm8). Its
  refinement explicitly hands the per-format byte-exact golden matrix to
  this task (kernels.md:141: *"`color.kernel_goldens` must cover
  `fill_kernel` / `source_over_kernel` / `convert_kernel` … across all
  three formats, plus `pixel_traits.hpp` codecs"*). Chose **software f16**
  precisely so kernel outputs are byte-identical across the three
  compilers (kernels.md Decisions) — this is what makes per-format goldens
  possible at all.
- `color.resampling` (`tasks/refinements/color/resampling.md`) — folded the
  bilinear tap into `source_over_kernel` and added `downsample_box_kernel`
  in `src/backend_cpu/kernels.hpp`; added `Backend::downsample` dispatch.
  Its refinement also defers the per-format golden matrix here
  (resampling.md: *"The per-format byte-exact golden matrix stays in
  `color.kernel_goldens`"*) and is the reason the `depends !resampling`
  edge exists in the `.tji`.

**Pending (not a dependency):** `color.working_space` is still
`_pending implementation_`. It changes the *default* compositing format but
not the kernels or codecs this task pins; it is out of scope here.

## What this task is

Freeze a **byte-exact reference-vector golden suite** for the color kernels
and codecs, one snapshot per format per operation. For each supported
`PixelFormat` — `Rgba32fLinearPremul`, `Rgba16fLinearPremul`, `Rgba8Srgb` —
and each kernel family (fill, source-over/blend, bilinear-tap resample, box
2:1 downsample, cross-format convert), and for the underlying codecs
(sRGB8↔linear, f16↔float, unorm8, premultiply/unpremultiply), the suite runs
a fixed representative input through the real kernel and asserts the output
bytes equal a frozen expected table with **no tolerance**. This is doc 16's
tier-3 "Golden rendering tests" discipline (`docs/design/16-sdlc-and-quality.md`,
"Test taxonomy" §Golden rendering tests, 16:47-53) applied at kernel
granularity rather than whole-frame granularity.

## Why it needs to be done

The two predecessors landed **relational / property** correctness: that
source-over equals `s + (1-aₛ)d`, that integer-aligned composites reduce to
the nearest tap, that Rgba8Srgb blends in linear light and not gamma space,
that sRGB8 round-trips exactly, that the box filter is a four-tap mean. Where
the math is interpolative those tests compare against an *analytic ideal*
with an explicit per-format tolerance (`require_close` at
`src/backend_cpu/t/cpu_backend.t.cpp:33`, tolerances 1e-6/0.005/0.02 for
32f/16f/8 at cpu_backend.t.cpp:487-491).

What is **not** yet pinned is the *absolute output bytes* of each kernel for a
fixed input. A property test that asserts `encode(decode(x)) == x` still
passes if the sRGB EOTF constant shifts, so long as the round-trip stays
self-consistent; a magnification test that asserts "within 0.02" still passes
if a tap weight drifts. A byte-exact golden catches exactly these
regressions — any future edit to a kernel, a codec constant, a reduction
order, or the FP flags that perturbs a single output byte becomes a diff.
That is the doc 16 determinism contract ("fixed FP flags, no FMA variance in
kernels, ordered reductions", 16:47-49); this task is where it is enforced at
the kernel seam, so the compositor's whole-frame goldens (scale_ladder,
refinement, damage_planning, …) inherit a firm foundation rather than
re-deriving kernel determinism.

## Inputs / context

**Kernels to pin** — `src/backend_cpu/kernels.hpp`:

- `fill_kernel<F>(TypedSpan<F> dst, const WorkingPixel& color)` — line 26.
- `source_over_kernel<F>(TypedSpan<F> dst, …, const Affine& dst_to_src,
  float opacity)` — line 65; the blend kernel with the bilinear tap folded
  in (`fetch_texel<F>` at line 39, texel-center convention lines 73-75).
- `downsample_box_kernel<F>(…)` — line 121; exact 2:1 four-tap box mean
  (`* 0.25F` at line 137).
- `convert_kernel<SrcF, DstF>(std::span<const SrcStorage> src,
  TypedSpan<DstF> dst, std::size_t pixel_count)` — line 153; routes
  src→working→dst.

**Codecs to pin** — `src/media/arbc/media/pixel_traits.hpp`:
`premultiply` (25), `unpremultiply` (30), `srgb8_to_linear` (43),
`linear_to_srgb8` (48), `unorm8_encode/decode` (56/60), `f16_to_float` (70),
`f16_from_float` (102); `WorkingPixel = std::array<float,4>` (17);
`PixelTraits<F>` specializations at 158/174/191.

**Format vocabulary** — `src/media/arbc/media/pixel_format.hpp`:
`enum class PixelFormat { Rgba32fLinearPremul, Rgba16fLinearPremul,
Rgba8Srgb }` (16), `bytes_per_pixel` 16/8/4 (23). Tag triples in
`src/media/arbc/media/surface_format.hpp` (`k_working_rgba32f` 36,
`k_working_rgba16f` 42, `k_fast_rgba8srgb` 49).

**Dispatch seam** — `src/surface/arbc/surface/typed_span.hpp`:
`TypedSpan<F>` (with `static constexpr PixelFormat format = F`) and
`visit_surface`. Runtime entry points in
`src/backend_cpu/cpu_backend.cpp`: `clear`→`fill_kernel` (61/67),
`composite`→`source_over_kernel` (70/92), `downsample`→`downsample_box_kernel`
(97/119). `convert_kernel` has **no** `CpuBackend` method — goldens call the
kernel template directly, as the existing convert test at
cpu_backend.t.cpp:455 already does.

**Existing test idioms to reuse** (there is no shared golden harness and no
on-disk fixtures — every golden in this repo is computed in-code):

- Raw byte-exact assertion on `Storage`: `require_px_eq` /
  `read_px`/`write_px` in `tests/scale_ladder_golden.t.cpp:34,41,49`.
- Byte-exact via typed span + `std::memcmp`: `byte_identical` in
  `tests/refinement_golden.t.cpp:69`; span access
  `surface.span<PixelFormat::Rgba32fLinearPremul>()`
  (`tests/walking_skeleton.t.cpp:82`).
- Per-format templated case bodies driven across all three formats:
  `check_integer_is_byte_exact` (cpu_backend.t.cpp:107, the existing
  byte-exact per-format assertion), `check_magnification` (68).

**Governing design docs (normative — doc 16 §Philosophy):**

- `docs/design/07-color-and-pixel-formats.md`, §"Templates and variants:
  where compile-time formats live" (07:67-70) — the kernel operation set
  (blend, resample, premultiply, space conversion) as monomorphized
  templates, one `std::visit` per operation; §"The model" rules 1-4 for the
  linear-light / premultiplied contract.
- `docs/design/16-sdlc-and-quality.md`, §"Test taxonomy" tier 3 "Golden
  rendering tests" (16:47-53) — byte-exact discipline and the
  tolerance-is-the-exception policy; §"CI structure … Diff coverage: hard
  gate" (16:112-118) — ≥90% changed-line coverage.
- `docs/design/17-internal-components.md`, component table row
  `arbc::backend-cpu` (Level 3, docs 07/09; depends on base, media,
  surface) and note "Kernels are not `media`" (17:75-77) — the levelization
  edge this test target must respect; §"Levelization rule" (17:41-44).

**Claims already registered** (`tests/claims/registry.tsv`) that this suite
adds enforcing coverage for: `#premultiplied-source-over` (27),
`#blending-in-linear-working-space` (29), `#srgb8-round-trips-exactly` (30),
`#f16-conversion-portable-and-exact` (31),
`#conversions-route-through-working-space` (33),
`#resampling-in-linear-premultiplied-space` (34), and
`16-sdlc-and-quality#byte-exact-goldens` (38).

## Constraints / requirements

1. **Byte-exact, zero tolerance.** Every golden asserts raw-`Storage`
   equality (`==` / `memcmp`), for all three formats **including
   `Rgba16fLinearPremul`**. The software f16 codec is deterministic and
   portable, so its *output bytes* are frozen even though its *accuracy*
   carries a 1-ULP bound (kernels.md:86); the 1-ULP figure is an
   accuracy-vs-ideal statement, not a reproducibility one, and must not
   become a golden tolerance. Any tolerance introduced here would violate
   doc 16 tier 3 (16:51-53) and requires a justifying comment — there is no
   justified case in this suite (see Decisions).
2. **Real kernels, not reimplementations.** Goldens drive the shipped
   `kernels.hpp` templates (via `visit_surface`/`CpuBackend` where a method
   exists, directly where — `convert_kernel` — it does not). The expected
   side is a frozen literal table, not a second in-test computation of the
   same formula (that would only test the test).
3. **Levelization (doc 17).** The suite is a `backend_cpu` **component**
   test (`src/backend_cpu/t/`), linking only `base`, `media`, `surface`
   (L0-L2) below the L3 kernels — matching the existing
   `arbc_component_test(COMPONENT backend_cpu SOURCES t/cpu_backend.t.cpp)`
   registration (`src/backend_cpu/CMakeLists.txt:7`). It must **not** pull
   in the umbrella `arbc` target or any L4+ component (compositor, runtime);
   the CI include-graph check (doc 16 §Levelized components) would reject
   that.
4. **Coverage of the full handed-off matrix** (kernels.md:141 ∪
   resampling.md): `fill_kernel`, `source_over_kernel` (integer-aligned
   blend *and* fractional bilinear tap), `downsample_box_kernel`,
   `convert_kernel` across all three formats, plus the `pixel_traits.hpp`
   codecs. No kernel or codec in the handed-off set may be left without a
   frozen reference vector.
5. **Determinism inputs are fixed.** No RNG, no clock, no platform-varying
   input; the golden inputs are small hand-chosen constant tables committed
   in the test source.
6. **Regenerate discipline (doc 16 tier 3: "regenerate with an audited
   script").** Provide a documented, non-default way to dump the current
   kernel output bytes for pasting into the expected tables (a
   compile-time-guarded `dump` helper or a tagged `[.regen]` Catch2 case
   that is excluded from the default run), so an intended kernel change
   regenerates goldens deliberately and never silently. A comment at the top
   of the file states the procedure.

## Acceptance criteria

- **New test target `src/backend_cpu/t/kernel_goldens.t.cpp`**, registered
  by appending an `arbc_component_test(COMPONENT backend_cpu SOURCES
  t/kernel_goldens.t.cpp)` line to `src/backend_cpu/CMakeLists.txt`,
  discovered by CTest via the existing `catch_discover_tests` path. Building
  and running the `dev` (and `asan`) preset shows the new cases green.
- **Per-format kernel goldens.** For each of the three formats, a byte-exact
  case for: `fill_kernel`; `source_over_kernel` integer-aligned (pure blend,
  reduces to nearest tap) and fractional (bilinear); `downsample_box_kernel`
  (2:1). Assertions are raw-`Storage` `==` on the frozen expected table.
- **Cross-format convert goldens.** `convert_kernel<SrcF,DstF>` for every
  ordered pair among the three formats (six directed conversions), each
  pinned byte-exact on a fixed input including at least one non-opaque
  (straight→premultiplied) sample, exercising the src→working→dst route.
- **Codec reference vectors.** Frozen expected bytes/bits for
  `srgb8_to_linear`/`linear_to_srgb8` at representative codes (0, 1, 128,
  255), `unorm8_encode/decode`, `f16_to_float`/`f16_from_float` at the IEEE
  landmarks (±0, 0.5, 1.0, a subnormal, inf), and
  `premultiply`/`unpremultiply` at an alpha ≠ {0,1}. These pin the absolute
  curve values that the predecessors' round-trip tests do not.
- **Claims register.** Each case carries an `// enforces: <claim-id>` tag.
  The suite enforces the existing doc-07 per-operation claims (27/29/30/31/
  33/34) and `16-sdlc-and-quality#byte-exact-goldens` (38). It **adds one
  new claim** row to `tests/claims/registry.tsv`:
  `07-color-and-pixel-formats#kernels-byte-exact-per-format` — *"Each
  format-templated color kernel (fill, source-over/bilinear, box downsample,
  cross-format convert) and its codecs produce byte-identical output for
  fixed inputs across the supported compilers, per format, with no
  tolerance."* — referenced by an `enforces:` tag in the new file. The
  nightly claims-register audit (doc 16 §CI structure) stays green (no
  claim without a live test).
- **Diff coverage ≥90%** on changed lines (doc 16 16:112-118). The changed
  lines are test code that executes on every run, so the gate is met without
  `GCOV_EXCL`; the `[.regen]`/dump path, if not run by default, carries a
  `GCOV_EXCL` with a justifying comment or is written so its body still
  executes under the normal case.
- **Deferred follow-up:** none. The handed-off matrix is fully covered; no
  new WBS leaf is spawned by this task.

## Decisions

- **Reference-vector goldens, distinct from the predecessors' property
  tests.** *Chosen:* freeze absolute output bytes for fixed inputs.
  *Alternative rejected:* treat the existing `cpu_backend.t.cpp` /
  `pixel_traits.t.cpp` property tests as sufficient and close this task as a
  no-op. Rejected because round-trip and relational assertions (and the
  `require_close` tolerances they use for interpolation) do not pin the
  actual curve/kernel output and would silently pass a codec-constant or
  tap-weight drift — precisely the regression class doc 16 tier 3 exists to
  catch. The task note's phrase "at kernel granularity" is read as: absolute
  byte snapshots, not more properties.

- **Component test in `src/backend_cpu/t/`, not a `tests/` cross-component
  golden.** *Chosen:* a `backend_cpu` unit-golden linking base/media/surface.
  *Alternative rejected:* a top-level `tests/color_kernel_goldens.t.cpp`
  linking the umbrella `arbc` target (the pattern the existing `*_golden.t.cpp`
  files follow). Rejected because those existing goldens are multi-component
  *integration* renders that genuinely need the compositor; a kernel snapshot
  needs nothing above `surface` (L2). Doc 17 places the kernels in
  `arbc::backend-cpu` (L3) and explicitly separates them from `media`
  (17:75-77), so the tightest correct target links exactly the L0-L2 deps —
  faster to build, and it keeps the levelization surface honest.

- **All three formats byte-exact, f16 included; zero tolerances.** *Chosen:*
  no tolerance anywhere in the suite. *Alternative rejected:* a 1-ULP
  tolerance on the f16 goldens mirroring the accuracy bound in kernels.md:86.
  Rejected because the software f16 encoder is deterministic — its output
  bytes are fixed for a fixed input on every supported compiler (the whole
  reason software f16 was chosen over `_Float16`). The 1-ULP bound describes
  distance from the mathematically ideal half, which is irrelevant to a
  snapshot of the actual deterministic output. Admitting a tolerance would
  weaken the golden and contradict doc 16's "tolerances are the exception,
  each with a justifying comment" (16:51-53).

- **Convert covers all six directed format pairs.** *Chosen:* pin every
  ordered pair. *Alternative rejected:* a single representative pair (e.g.
  the rgba8↔rgba32f already tested at cpu_backend.t.cpp:455). Rejected
  because the src→working→dst routing (2N codecs) means each pair exercises a
  distinct src-decode/dst-encode combination; a codec asymmetry could surface
  in exactly one direction of one pair. Six directed conversions is cheap and
  leaves no codec edge unpinned.

- **In-code frozen tables, no on-disk fixtures.** *Chosen:* expected bytes
  are literals in the test source, matching every existing golden in the repo
  (`scale_ladder`, `refinement`, …). *Alternative rejected:* a
  `tests/golden/` fixtures directory with binary blobs. Rejected: the inputs
  are tile-sized and tiny, on-disk blobs add a load/compare harness the repo
  deliberately does not have, and inline tables diff cleanly in review.

- **No new production code, no design-doc delta.** The byte-exact promise is
  already implied by doc 07 (deterministic monomorphized kernels, 07:78-90)
  and doc 16 tier 3; the new claim id is a finer-grained enforcement of an
  existing promise, not a new architectural behavior, so it needs only a
  `registry.tsv` row, not an amendment to `docs/design/`.

## Open questions

(none — all decided.)

## Status

**Done** — 2026-07-06.

- Created `src/backend_cpu/t/kernel_goldens.t.cpp` with 6 golden unit test cases (fill, source-over integer + fractional, box downsample, six-directed convert, and three codec-vector cases — sRGB/unorm8, software f16, premultiply/unpremultiply), all byte-exact/zero-tolerance; plus a `[.regen]` dump case (GCOV-excluded).
- Folded new source into the existing `arbc_component_test` call in `src/backend_cpu/CMakeLists.txt` (a second call would collide on the derived `arbc_backend_cpu_t` target name).
- Registered new claim `07-color-and-pixel-formats#kernels-byte-exact-per-format` in `tests/claims/registry.tsv`; tagged existing claims 27/29/30/31/33/34/38.
- 242 assertions / 27 cases green under both `dev` and `asan` presets.
- No deferred follow-up: the full handed-off matrix (kernels.md + resampling.md) is covered with zero tolerance across all three formats.
