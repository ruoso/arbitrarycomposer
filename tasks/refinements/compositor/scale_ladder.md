# compositor.scale_ladder — Scale ladder

## TaskJuggler entry

`tasks/35-compositor.tji:9-13` → `compositor.scale_ladder` ("Scale ladder"),
the first leaf under `task compositor`. It carries no own `depends`, so through
the parent `task compositor` it inherits
`depends contract.async_render, cache.key_shapes, color.resampling`
(`35-compositor.tji:7`). Four siblings chain off it:
`compositor.tile_planning` (`depends !scale_ladder, contract.snapshot_pins`,
`35-compositor.tji:17`), and transitively `damage_planning`, `pull_service`,
`anchored_viewports`, `refinement`, `counters` (all `depends !tile_planning`).
Note line:

> "Quantize interactive request scales to power-of-two rungs; composite applies
> the <=1-octave remainder; prefer-downsample convention. Doc 04."

## Effort estimate

**2d.** The deliverable is one small pure-arithmetic module — quantize a
requested scale to a power-of-two rung, report the sub-octave remainder — plus
a thin `Backend::downsample` wrapper that builds the next-coarser rung, plus the
exhaustive unit tests (doc 16:46 lists the scale ladder as a fast, edge-case
unit-test item), one box-reduction golden, one claims-register entry, and the
one-line `cache` dependency-edge widening of the compositor component. No new
renderer wiring: the resampling capability (bilinear composite tap, box 2:1
downsample) already landed in `color.resampling` behind the `surface::Backend`
seam; this leaf turns doc 04's ladder *convention* into a concrete, testable
rung algebra the interactive planner will call.

## Inherited dependencies

**Settled:**

- `color.resampling` (commit `fa895d5`, DONE 2026-07-05) — the capability
  predecessor. It delivered the two resampling tools the ladder consumes, both
  exposed on the abstract L2 `surface::Backend` vtable so the L4 compositor
  reaches them without an `arbc::backend-cpu` edge:
  - **Bilinear composite tap** folded into `Backend::composite(dst, src,
    src_to_dst, opacity)` (`src/surface/arbc/surface/backend.hpp:39-40`) — the
    sub-octave remainder is resampled *inside* `composite`, with **no signature
    change** and a byte-exact collapse to the incumbent nearest tap at integer
    alignment (`frac == 0`). The ladder feeds `composite` a rung-scale source
    and lets it absorb the ≤1-octave remainder.
  - **Box 2:1 downsample** as a new `Backend::downsample(dst, src)` pure virtual
    (`backend.hpp:42-48`): `dst` dims = `src` dims / 2, **even** source dims,
    same format, mean taken in decoded premultiplied linear working space
    (doc 07 rule 3). Its comment states the intent explicitly: "The scale ladder
    (compositor) uses this to build coarser rungs; rung selection is not the
    backend's concern." The `CpuBackend` implementation asserts the geometry
    (`src/backend_cpu/cpu_backend.cpp:103-109`) and no-ops on violation in
    release.
  - Registered claim `07-color-and-pixel-formats#resampling-in-linear-premultiplied-space`
    (`tests/claims/registry.tsv:31`) pins that both tools interpolate decoded
    premultiplied linear floats. This leaf does **not** re-register it — it
    depends on it and adds the distinct *ladder* claim (below).
- `cache.key_shapes` (commit `a198981`, DONE 2026-07-05) — the type
  predecessor. It defined **`struct ScaleRung { std::int32_t index; }`**
  (`src/cache/arbc/cache/key_shapes.hpp:39-43`), the cache-local integer rung
  index that keys `TileKey` (`:56-76`), with the explicit note "the compositor
  owns the ladder and hands the rung down" (`key_shapes.hpp:38`). This leaf is
  the code that *computes* that `ScaleRung`. It reuses the type directly rather
  than inventing a parallel compositor rung type (see Decisions).
- `contract.async_render` (commit `92c3d3b`, DONE) — established the
  `RenderRequest { Rect region; double scale; Time time; StateHandle snapshot;
  Surface& target; … }` descriptor (`src/contract/arbc/contract/content.hpp:67-75`)
  whose `scale` field is what the ladder quantizes, and `RenderResult
  { double achieved_scale; bool exact; }` (`content.hpp:77-80`) whose
  `achieved_scale` the composite pass already honours. This leaf does not change
  either struct.

**Pending:** none — every predecessor is landed.

## What this task is

Deliver the **scale-ladder rung algebra** for `arbc::compositor` (L4): the pure
functions that turn doc 04's ladder convention into concrete, testable code, and
the thin capability wrapper that builds a coarser rung from a finer one. In a new
header `src/compositor/arbc/compositor/scale_ladder.hpp`:

1. **`RungSelection { ScaleRung rung; double remainder; }`** — the result of
   quantizing a requested scale.
2. **`RungSelection select_rung(double scale)`** — quantize an interactive
   request scale (the larger singular value of the composed mapping,
   `Affine::max_scale()`, `src/base/arbc/base/transform.hpp:31-34`; doc 04:104)
   to the **smallest power-of-two rung `2^k ≥ scale`** (prefer-downsample,
   doc 04:96-98), leaving `remainder = scale / 2^k ∈ (0.5, 1.0]` — the ≤1-octave
   residual the composite pass resamples, exactly `1.0` at power-of-two scales.
   Precondition: `scale > 0` and finite (callers cull non-positive / non-finite
   composed scale upstream — `compositor.cpp:44`, doc 04:115-117).
3. **`double rung_scale(ScaleRung rung)`** — the device-pixels-per-local-unit a
   rung renders at: `2^rung.index` (rung 0 = native scale `1.0`; doc 04:90-91's
   "(…, ½, 1, 2, 4, …)").
4. **`ScaleRung reduce_rung(Backend& backend, Surface& dst, const Surface& src,
   ScaleRung src_rung)`** — build the next-coarser rung (`index - 1`) from a
   finer-rung surface by exact 2:1 box reduction (`Backend::downsample`),
   returning the coarser rung. Centralises the even-dims / `dst == src/2`
   precondition documentation and the `index - 1` relationship; the box mean is
   taken in linear premultiplied working space by the delegated kernel. This is
   the compositor's use of the box 2:1 reducer the resampling task built for it.

**Not this task:** the visible-region → local-space-aligned **tile grid** split,
per-rung **tile-coordinate** enumeration, and **cache lookup/fill** of rung tiles
(`compositor.tile_planning`, `35-compositor.tji:14-19`); **speculative next-rung
requests** during zoom gestures (`compositor.refinement`, `:43-48`; doc 04:99-101);
the **coarser-tile fallback rescale** in the deadline-degradation order
(`compositor.tile_planning`'s planning, doc 02:63-65); the **behavioral counters**
(requests / cache hits / composites) exposed for claims tests
(`compositor.counters`, `:49-54`); any change to the **offline** `render_frame`,
which stays exact-scale by design (doc 02:74-85 — offline has "no quantization").
The ladder is *interactive*-path machinery (doc 04:90); the interactive planner
that calls it is `tile_planning` + the runtime frame loop (doc 17:60).

## Why it needs to be done

The ladder is the pivot of the whole interactive-zoom story. Doc 02's request
plan step 3 (`02:57-60`) is "map the visible region into layer-local space,
**quantize scale to the ladder**, split into tiles … and look each tile up in the
cache" — the quantization is the first operation, and every downstream tile key
carries the `ScaleRung` it produces (doc 02:89, `key_shapes.hpp:60`). Doc 04:93-94
makes the payoff concrete: "a smooth pinch-zoom reuses one rung's tiles across an
octave instead of thrashing re-renders every frame" — reuse that only exists once
requests are snapped to rungs. Until `select_rung` exists, `compositor.tile_planning`
(`depends !scale_ladder`) has no rung to key tiles by, no remainder to hand the
composite pass, and no way to build the coarser rungs the degradation order
(doc 02:64-65) rescales from. This leaf gives the planner the rung algebra and the
`reduce_rung` primitive; `tile_planning` wires them into visible-region planning.

## Inputs / context

- `docs/design/04-transforms-and-infinite-zoom.md`:
  - `:88-106` — the governing **Scale ladders and tile geometry** section.
    `:90-91` "Interactive rendering quantizes requested scale to a ladder of
    powers of two (…, ½, 1, 2, 4, …) in the layer's local space" — the rung
    values and that rung 0 = native scale. `:93-94` rungs key the cache and a
    pinch-zoom reuses one rung across an octave. `:95-98` "The remainder
    (≤1 octave) is applied as resampling during compositing — and by convention
    the ladder is chosen so tiles are *downsampled* (rung ≥ needed scale) once
    the next rung is available, since minification looks better than
    magnification" — the prefer-downsample convention and the ≤1-octave bound.
    `:103-106` request scale = "the larger singular value of the composed
    mapping", tiles axis-aligned in *local* space, "the composite pass applies
    the full affine".
  - `:99-101` — speculative next-rung request during a gesture (the
    "once the next rung is available" qualifier); this leaf owns the rung
    *math*, not the speculation (that is `compositor.refinement`).
  - `:63-66` — the illustrative `2^16` re-anchor threshold: a *precision /
    viewport-rebasing* mechanism owned by `compositor.anchored_viewports`,
    distinct from the per-octave tile ladder. The ladder does **not** clamp at
    it (see Decisions).
  - `:110`, `:113-117` — geometry is `double`; a degenerate/near-zero composed
    scale culls the layer rather than propagating NaNs — the precondition that
    guarantees `select_rung` is only ever handed a positive finite scale.
- `docs/design/02-architecture.md`:
  - `:57-60` — request-plan step 3: quantize scale to the ladder, then tile,
    then cache-lookup. The quantize is this leaf; the tile+lookup is
    `tile_planning`.
  - `:66-68` — composite step 5: "Tiles rendered at a ladder rung are resampled
    by the ≤1-octave remainder during this pass" — the remainder is a
    composite-time resample, already delivered by the bilinear tap.
  - `:63-65` — the deadline-degradation order ("coarser-scale tiles rescaled")
    the `reduce_rung`-built coarser rungs feed; the *selection* of a fallback is
    `tile_planning`'s.
  - `:74-85` — the offline discipline: "exact scale", no quantization. This leaf
    leaves the offline `render_frame` untouched.
  - `:89-91` — the tile key carries the `scale rung`; the value metadata is
    `{achieved_scale, exact}`.
- `docs/design/16-sdlc-and-quality.md`:
  - `:46` — "Unit tests for core machinery (… **scale ladder** … culling …) —
    Catch2, fast, exhaustive on edge cases": the ladder is explicitly a
    unit-test-tier item, not a golden-rendering item.
  - `:47-53` — byte-exact CPU goldens (deterministic fixed-FP kernels): the
    `reduce_rung` box-reduction golden is byte-exact.
  - `:14-25` — the claims register: id is `<doc-file-stem>#<slug>`, enforced by a
    `// enforces: <claim-id>` test comment; CI fails on a registered claim with
    no live test.
  - `:112-118` — ≥90% diff coverage on changed lines.
- `docs/design/17-internal-components.md`:
  - `:56` — `arbc::compositor` is **Level 4**, "transform resolution, culling,
    request planning, **scale ladder**, …", `Depends on: contract, cache
    (+ below)`. The `cache` edge (for `ScaleRung`) is directly authorised; the
    resampling kernels are reached via the transitive `surface::Backend` seam,
    **not** a direct `backend-cpu` edge (`:75-77` "Kernels are not media … the
    templated kernel bodies are backend implementation (L3, backend-cpu)").
  - `:40-44` — "depend only on strictly lower levels; no same-level edges"; CI
    validates the CMake target graph and include graph against the table.
  - `:145-155` — repo layout: public headers under
    `src/<component>/arbc/<component>/`, unit tests in `src/<component>/t/`,
    cross-component (golden/integration) tests in top-level `tests/`.
- `src/surface/arbc/surface/backend.hpp:39-48` — the `composite` and `downsample`
  virtuals the ladder drives (abstract L2 seam).
- `src/base/arbc/base/transform.hpp:31-34` — `Affine::max_scale()`: the larger
  singular value = the request scale `select_rung` quantizes.
- `src/cache/arbc/cache/key_shapes.hpp:38-43` — `struct ScaleRung { std::int32_t
  index; }` with defaulted `operator==`; this leaf computes it.
- `src/compositor/compositor.cpp:43,:71,:95-99` — the walking-skeleton offline
  `render_frame`: `const double scale = composed.max_scale();` (`:43`), the
  exact-scale `RenderRequest{region, scale, …}` (`:71`), and the composite
  remainder `temp_to_dst` (`:95-99`). This leaf adds the rung module beside it;
  the offline path is not rewired.
- `src/compositor/CMakeLists.txt:1-6` — currently `DEPENDS contract`; the
  `arbc_add_component(...)` + `arbc_component_test(...)` pattern (mirrors
  `src/cache/CMakeLists.txt`). This leaf adds `cache` and the new header + test.
- `tests/CMakeLists.txt:9-12` — the cross-component golden pattern (links `arbc`
  + `Catch2::Catch2WithMain`), mirrored for the `reduce_rung` golden.
- `tests/claims/registry.tsv` — 2-column TAB-separated `<claim-id>\t
  <description>`; this leaf appends one row. No ladder/rung claim exists yet.

## Constraints / requirements

- **Levelization (doc 17:40-44, :56).** The module lives in **L4
  `arbc::compositor`**. It may include `cache` (for `ScaleRung`) and, transitively
  through `contract`/`cache`, the L2 `surface::Backend` seam. **No direct
  `backend-cpu` edge** — the box/bilinear kernels are reached only through the
  `Backend` vtable. The compositor's `DEPENDS contract` widens to
  `DEPENDS contract cache`; `surface` stays transitive (contract and cache both
  depend on it), matching the existing include of `arbc/surface/backend.hpp` in
  `compositor.hpp`. The CI dependency check must still pass with no new listed
  edge beyond `cache`.
- **Prefer-downsample quantization (doc 04:96-98).** `select_rung(scale)` returns
  the **smallest** rung with `rung_scale ≥ scale` (round the rung *up* in scale),
  so the composite remainder is always a **downsample** (`remainder ≤ 1.0`).
  Never round to a coarser-than-needed rung and magnify.
- **Exact, deterministic rung math (doc 16:47-53).** The quantization must be
  **exact at power-of-two boundaries** — `select_rung(2^k)` returns exactly
  `{ index: k, remainder: 1.0 }`, with no floating off-by-one. Implement via
  `std::frexp`, **not** `std::log2` + `ceil` (which rounds unpredictably at
  powers of two): `frexp(scale, &e)` gives `scale = m·2^e`, `m ∈ [0.5, 1)`; the
  rung index is `e - 1` when `m == 0.5` (scale is the exact power `2^(e-1)`) and
  `e` otherwise. `rung_scale(rung) = std::ldexp(1.0, rung.index)` and
  `remainder = scale / rung_scale`, so a power-of-two scale yields `remainder ==
  1.0` bit-exactly (division of equal exact doubles). This determinism is what
  lets the composite collapse to the byte-exact nearest tap at power-of-two
  scales.
- **≤1-octave remainder bound (doc 04:95).** Because the rung is the *smallest*
  `2^k ≥ scale`, `remainder = scale / 2^k ∈ (0.5, 1.0]` for every positive finite
  input — at most one octave of composite-time downsampling. This invariant is an
  asserted post-condition of `select_rung`.
- **Precondition, not defensive clamp.** `select_rung` requires `scale > 0 &&
  std::isfinite(scale)`; the caller (`compositor.cpp:44`, doc 04:115-117) culls
  degenerate scale first. `select_rung` asserts the precondition (debug) rather
  than inventing a NaN/zero policy the ladder does not own.
- **No `2^16` clamp (doc 04:63-66).** The ladder produces the raw rung index for
  any positive scale; `std::int32_t` spans far beyond any representable zoom.
  Precision at extreme scale is handled by viewport **rebasing**
  (`compositor.anchored_viewports`), a separate mechanism — the ladder does not
  clamp or saturate.
- **`reduce_rung` geometry contract (backend.hpp:42-48).** `dst` dims must equal
  `src` dims / 2 with **even** source dims and identical format; `reduce_rung`
  documents and (debug-)checks this before delegating to `Backend::downsample`,
  and returns `{ index: src_rung.index - 1 }`. The even-dims guarantee is a
  property of power-of-two **tile** geometry, owned by `tile_planning`; the
  wrapper states that dependency rather than allocating or resizing itself.
- **Working-space correctness is delegated, not re-implemented (doc 07 rule 3).**
  Both the composite tap and the box mean interpolate decoded premultiplied
  linear floats inside the backend kernels; the compositor never touches pixels
  (doc 02:36-39). `reduce_rung` is pixel-loop-free.
- **Single-threaded, no concurrency surface.** The rung algebra is pure value
  math; `reduce_rung` is a synchronous backend call. **No TSan obligation.**
- **CI diff coverage ≥90%** (doc 16:112-118); the public header compiles
  standalone.

## Acceptance criteria

- **Unit tests — `src/compositor/t/scale_ladder.t.cpp` (new, Catch2), registered
  via `arbc_component_test`.** Exhaustive on edge cases (doc 16:46):
  - **Power-of-two exactness:** for `scale ∈ {2^k : k ∈ [-20, 20]}` (via
    `std::ldexp`), `select_rung(scale)` returns `{ index: k, remainder: 1.0 }`
    with `remainder == 1.0` compared **bit-exactly**, and `rung_scale({k}) ==
    scale` bit-exactly.
  - **Prefer-downsample / round-up:** for `scale` just above a power
    (`std::nextafter(2^k, ∞)`), the rung is `k+1` and `remainder ≈ 0.5⁺`; for
    `scale` just below (`std::nextafter(2^k, 0)`), the rung is `k` and
    `remainder ≈ 1.0⁻`. Confirms the boundary lands on the finer rung.
  - **Remainder invariant:** across a dense sweep of scales spanning several
    octaves (small and large, including sub-1 and >1), assert `rung_scale(rung)
    ≥ scale`, `rung_scale(rung) < 2·scale`, and `remainder ∈ (0.5, 1.0]`, and
    that `remainder * rung_scale(rung)` reconstructs `scale` to within a 1-ulp
    tolerance (justified-tolerance comment: the reconstruction is a float
    round-trip, not a byte-exact render).
  - **Monotonicity:** `select_rung` is non-decreasing in `scale` (a larger scale
    never selects a coarser rung).
  - **Precondition (debug death test or documented contract):** non-positive /
    non-finite input trips the assert — asserted as a contract, not a return
    value.
- **Golden — `tests/scale_ladder_golden.t.cpp` (new, cross-component, links
  `arbc` + `CpuBackend`).** Byte-exact (doc 16:47-53):
  - **Box 2:1 rung reduction:** fill an even-dimensioned `Rgba32fLinearPremul`
    surface with a known non-uniform pattern, `reduce_rung` it into a
    half-size surface, and assert the result byte-for-byte against a
    hand-computed 2×2-mean reference; assert the returned rung is
    `src_rung.index - 1`.
  - **Energy / mean conservation:** a *uniform* surface reduces to the same
    uniform value byte-exactly (the box reducer preserves the linear-light
    mean), and a two-value checker reduces to the exact average — pinning the
    2:1 energy-conservation property the ladder relies on.
  - **Prefer-downsample collapse:** compositing a rung-scale source at a
    power-of-two remainder (`remainder == 1.0`, integer alignment) through
    `Backend::composite` is byte-identical to the un-resampled source-over
    (the bilinear-tap-collapses-to-nearest property, viewed from the ladder's
    seam) — a regression guard that a power-of-two rung request pays no
    resampling cost.
- **Claim (register + `enforces:` tag)** appended to
  `tests/claims/registry.tsv`, enforced from the tests above:
  - `04-transforms-and-infinite-zoom#scale-quantized-to-power-of-two-rung-prefer-downsample`
    — "An interactive request scale `s` is quantized to the smallest power-of-two
    rung `2^k ≥ s` (prefer-downsample: rung ≥ needed scale), leaving a composite
    remainder `s / 2^k ∈ (0.5, 1.0]` (≤1 octave), exactly `1.0` at power-of-two
    scales; coarser rungs are built by exact 2:1 box reduction in linear
    premultiplied working space." (doc 04:90-98)
  - This leaf does **not** re-register the resampling linear-space claim
    (`07-…#resampling-in-linear-premultiplied-space`, `registry.tsv:31`) —
    it depends on it; the golden's linear-light mean check exercises that path
    but the registered ownership stays with `color.resampling`.
- **No behavioral-counter test here.** Requests-issued / cache-hits / composites
  counters are `compositor.counters`' surface (`35-compositor.tji:49-54`,
  doc 16:54-62); the ladder exposes none. Noted so the absence is a scoped
  decision, not a gap (same posture as `key_shapes.md`'s no-golden note).
- **Component wiring & CI dependency check:** `src/compositor/CMakeLists.txt`
  widened to `DEPENDS contract cache`, `arbc/compositor/scale_ladder.hpp` added
  to `PUBLIC_HEADERS`, the new component test registered, and the cross-component
  golden added to `tests/CMakeLists.txt`; the header compiles standalone; the
  doc-17 dependency check passes (no `backend-cpu` or same-level edge introduced).
- **Gate green (build + tiers 1-5 in Debug + ASan/UBSan).** No TSan obligation
  (pure value math + one synchronous backend call).

## Decisions

- **Reuse `cache::ScaleRung`; invent no compositor-local rung type.** The rung
  index is *the* cache-key discriminator (`key_shapes.hpp:39-43,:60`), and
  `key_shapes.hpp:38` explicitly reserves rung *computation* for the compositor
  ("the compositor owns the ladder and hands the rung down"). Producing
  `cache::ScaleRung` directly makes the hand-off to `tile_planning`'s `TileKey`
  a plain copy, and the compositor already depends on `cache`. *Rejected:* a
  parallel `compositor::Rung` newtype converted at the tile-key fill site — two
  identical `int32` wrappers and a lossless conversion for no gain.
- **`frexp`-based exact quantization, not `log2 + ceil`.** `select_rung` must be
  bit-exact at power-of-two boundaries so a power-of-two request pays zero
  resampling (the composite tap collapses to the nearest tap only when
  `remainder == 1.0` exactly). `std::log2(8.0)` is not guaranteed to be exactly
  `3.0`, so `ceil(log2(scale))` can land one rung high or low at the boundary;
  `std::frexp` decomposes `scale = m·2^e` exactly and the `m == 0.5` test picks
  the exact-power case with no rounding. `rung_scale`/`remainder` via
  `std::ldexp` keep the round-trip exact. *Rejected:* `ceil(log2(scale))` —
  boundary-fragile, and its errors surface as one-octave scale jumps and lost
  golden byte-exactness. *Rejected:* an integer bit-scan on the IEEE exponent —
  correct but re-implements `frexp` with UB-adjacent bit punning for no speed
  that matters at one call per layer per frame.
- **Prefer-downsample = round the rung *up* in scale (rung ≥ needed).** Doc
  04:96-98 fixes this: minification beats magnification, so `select_rung` picks
  the smallest rung at-or-above the requested scale and the composite always
  *downsamples* the residual. The cost is at most 2× oversampling (remainder just
  above 0.5) for a scale a hair over a rung — bounded to one octave and paid in
  the composite tap, exactly the doc's trade. *Rejected:* nearest-rung rounding
  (round to whichever power of two is closer) — it magnifies for scales in the
  lower half of an octave, which doc 04 rules out; and it complicates the
  remainder sign (a remainder > 1 is an upsample). *Rejected:* round-down
  (coarser rung, always magnify) — the opposite of the convention.
- **Remainder carried as a bare `double ∈ (0.5, 1.0]`, applied by the existing
  `Backend::composite`.** The composite tap already resamples the src→dst affine
  (`backend.hpp:39-40`), and `render_frame` already builds that affine from the
  achieved scale (`compositor.cpp:95-99`). Reporting the remainder as a scalar
  the planner folds into the composite affine reuses that seam with **no
  `composite` signature change and no separate resample-to-temp pass**. *Rejected:*
  a dedicated `resample(dst, src, remainder)` backend call — redundant with the
  composite tap the resampling task deliberately folded in-line to avoid a temp.
- **`reduce_rung` is a thin `Backend::downsample` wrapper owned by the ladder,
  not deferred wholesale to `tile_planning`.** The box 2:1 reducer was built
  *for* the scale ladder (`backend.hpp:46-47`; commit `fa895d5` message), and
  "building the next rung down" is a ladder operation, not a planning one.
  Landing the wrapper (with the `index - 1` bookkeeping, the even-dims/`dst==src/2`
  contract, and a byte-exact golden) here keeps the rung algebra and its one
  pixel-producing operation co-located and co-tested, and gives `tile_planning`
  a ready primitive. Its *production* call site — populating coarser cache rungs
  on a miss or for the degradation fallback — is `tile_planning`'s; until then
  the golden is its exercised caller (same posture as `cache.key_shapes` shipping
  `TileCache` tested-but-not-yet-production-called). *Rejected:* deferring
  `downsample` consumption entirely to `tile_planning` — it would leave this
  task not consuming the box kernel the orchestrator directed it to, and split
  the ladder's two natural operations across two tasks.
- **Offline `render_frame` stays exact-scale; the ladder is interactive-only.**
  Doc 02:74-85 specifies the offline path renders at exact scale with "no
  quantization"; `render_frame` (`compositor.hpp:26`) is that path. Wiring
  `select_rung` into it would violate the offline discipline. The ladder is
  interactive machinery (doc 04:90) consumed by `tile_planning` + the runtime
  interactive loop; this leaf ships and unit/golden-tests it without touching the
  offline frame. *Rejected:* quantizing `render_frame` now — breaks offline
  exactness and pre-empts `tile_planning`'s planning ownership.
- **No `2^16` clamp in the ladder.** The `2^16` figure (doc 04:63-66, "say 2¹⁶")
  is an illustrative viewport-rebasing threshold owned by
  `compositor.anchored_viewports`, not a ladder bound; conflating them would put
  a precision-management policy in the rung arithmetic. The ladder returns the
  raw `int32` rung; rebasing keeps the *camera* well-conditioned separately.
  *Rejected:* saturating the rung index at ±16 — mixes two orthogonal mechanisms
  and would silently mis-key tiles past the threshold.
- **No design-doc delta.** Every rule here is settled doc text: the ladder values
  and native rung (doc 04:90-91), prefer-downsample and the ≤1-octave remainder
  (doc 04:95-98), request scale = larger singular value (doc 04:104), the box
  reducer for coarser rungs (doc 04:96-98 + `backend.hpp:46-47`), and the
  `contract, cache` dependency edge (doc 17:56). This leaf *concretizes* those
  promises into C++ without altering designed behavior — the `cache` edge it adds
  is already authorised in the doc-17 table — so it needs no doc edit and no
  doc-00 bullet.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-05.

- `src/compositor/arbc/compositor/scale_ladder.hpp` — new header: `RungSelection`, `select_rung` (frexp-based, bit-exact at power-of-two boundaries), `rung_scale`, and `reduce_rung` (thin `Backend::downsample` wrapper returning the next-coarser rung index).
- `src/compositor/t/scale_ladder.t.cpp` — 5 Catch2 unit-test cases: power-of-two exactness, prefer-downsample boundary, remainder invariant sweep, monotonicity, and precondition contract.
- `tests/scale_ladder_golden.t.cpp` — 3 byte-exact cross-component golden cases: box 2:1 rung reduction against hand-computed reference, energy/mean conservation, and prefer-downsample collapse (power-of-two remainder composites identically to un-resampled source-over).
- `src/compositor/CMakeLists.txt` — `DEPENDS contract` widened to `DEPENDS contract cache`; `scale_ladder.hpp` added to `PUBLIC_HEADERS`; component test registered.
- `tests/CMakeLists.txt` — cross-component golden target added.
- `tests/claims/registry.tsv` — claim `04-transforms-and-infinite-zoom#scale-quantized-to-power-of-two-rung-prefer-downsample` registered and enforced from both test files.
