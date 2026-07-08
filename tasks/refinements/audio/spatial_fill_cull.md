# audio.spatial_fill_cull — Spatial sub-audible cull inside LookaheadRing descent

## TaskJuggler entry

`tasks/45-audio.tji:89-94` — `task spatial_fill_cull "Spatial sub-audible
cull inside LookaheadRing descent"`, in the `audio` work-stream (doc 12).

## Effort estimate

**1d** (from the `.tji`). A single localized change to `LookaheadRing::descend`
plus one behavioral-counter test and a determinism golden; no new seam, no
contract change, no design-doc delta.

## Inherited dependencies

- **`!spatial_policy`** — settled. `audio.spatial_policy` (Done 2026-07-08,
  `tasks/refinements/audio/spatial_policy.md`) landed the entire Spatial mix
  core: the `Spatialization` context on `AudioRequest`, the per-edge
  attenuation / √-law pan / mono-collapse, and the **sub-audible cull on the
  mix/render walk** (`mix_layer` and the `NestedContent::mix_child_layer`
  twin). Crucially it also landed the ring's `LookaheadRingConfig::spatial`
  seed and threaded it onto every `mix_block` request — but deliberately did
  **not** apply the sub-audible cull inside the ring's *warming* descent,
  registering that tightening as this leaf (spatial_policy Decision **D5**).
- Transitively settled through `spatial_policy`: `!mix_engine` (additive mix,
  `MixPolicy` seam), `!lookahead` (the `LookaheadRing` prepared-block ring),
  and `!lookahead_recursive_prefetch` (the recursive `descend` /
  `contributions_for` / `collect_wants` warming walk this task modifies).

No pending dependencies.

## What this task is

The Spatial mix walk already terminates a Droste / infinite-zoom chain early:
before pulling a contributor, if its accumulated attenuation × its edge
attenuation falls below the sub-audible threshold, the mixer neither pulls nor
descends it (`src/audio_engine/mix.cpp:64-66`). The **warming** walk that
primes the lookahead ring, `LookaheadRing::descend`
(`src/audio_engine/lookahead.cpp:183-249`), does **not** yet apply that cull:
it recurses into every nested contributor bounded only by
`depth < max_depth` (`lookahead.cpp:241`), so a Spatial Droste scene warms the
full 64-level chain even though the mixer will only pull a dozen of those
levels. This task ports the sub-audible cull — the exact
`accum_atten * edge_atten < sub_audible` predicate — into `descend`, so the
warmed contributor closure terminates at the sub-audible depth instead of at
`max_depth`.

This is a **warming-cost optimization, not a correctness fix**: drain
byte-exactness already holds because the ring warms a *superset* of the
mixer's pulls (spatial_policy Constraint 7 / D5). After this change the warmed
set equals the culled tree the mixer actually walks — still a covering set, so
the threaded-vs-inline drain stays byte-identical.

## Why it needs to be done

Without the cull, a Spatial Droste/infinite-zoom scene pays warming cost for
~64 nesting levels per output block on the pump/worker threads, while the
mixer discards all but the audible prefix — wasted `mix_composition`/
`render_audio` work and wasted `BlockCache` residency on blocks that are never
pulled. `spatial_policy` shipped the mix-side cull (the correctness-bearing
part) and explicitly separated this ring-side tightening so the 2d semantics
core wasn't bundled with a performance optimization (D5). Downstream, the live
interactive `audio.spatial_camera_follow` leaf (which will drive deep dynamic
zoom) inherits a warming walk that already scales with audible depth rather
than the hard depth budget.

## Inputs / context

**The seam this task modifies:**
- `src/audio_engine/lookahead.cpp:172-181` — `LookaheadRing::contributions_for`,
  which seeds the descent: `descend(d_config.composition, sample_rate,
  block_start(index), 1, out)`.
- `src/audio_engine/lookahead.cpp:183-249` — `LookaheadRing::descend`, the
  recursive warming walk. It already mirrors `mix_layer`'s Flat culls
  (`!audible() || gain<=0` at :207-209, unresolved/facet-less :210-213, span
  :214-217, reverse/zero-rate :218-229, time-map evaluate :230-233) but
  computes **no** `spatial_edge_atten` and reads **no** `d_config.spatial`.
  Recursion into a nested composition is at :241-246, bounded solely by
  `depth < d_config.max_depth`.
- `src/audio_engine/arbc/audio_engine/lookahead.hpp:82` — the
  `LookaheadRingConfig::spatial` seed; its doc-comment (:76-81) names this
  exact deferral ("the warming descent does NOT apply the sub-audible cull
  (deferred to `audio.spatial_fill_cull`), the ring warms a SUPERSET").
- `src/audio_engine/arbc/audio_engine/lookahead.hpp:105` — `max_depth{64}`,
  today's only terminator for the warming recursion.
- `src/audio_engine/arbc/audio_engine/lookahead.hpp:257-264` — the
  `Contribution` struct (no attenuation field today; none needed — see D1).

**The reference behavior to port (already shipped by spatial_policy):**
- `src/audio_engine/mix.cpp:57-74` — the `mix_layer` Spatial branch; the cull
  itself at **`mix.cpp:64-66`**: `edge_atten = spatial_edge_atten(layer.transform);
  if (sp.accum_atten * edge_atten < sp.sub_audible) return;`, and the child
  context accumulation `sp.accum_atten * edge_atten` at :72-73.
- `src/kind_nested/nested_content.cpp:433-435` — the identical L3 twin cull.
- `src/contract/arbc/contract/content.hpp:236` —
  `inline constexpr float k_sub_audible_atten = 1.0F / 4096.0F;` (2^-12,
  ≈ −72 dBFS).
- `src/contract/arbc/contract/content.hpp:247-264` — `struct Spatialization`
  (`float accum_atten{1.0F}`, `float sub_audible{k_sub_audible_atten}`).
- `src/contract/arbc/contract/content.hpp:273-282` —
  `spatial_edge_atten(const Affine&) = clamp(max_scale(transform), 0, 1)`, the
  pure helper this task reuses verbatim.

**Behavioral-counter / test infrastructure:**
- Ring counters on `LookaheadRing` (`lookahead.hpp:195-211`):
  `blocks_mixed()`, `silence_mixed()` (:204-206, the residency-breach counter
  that must stay 0), `underruns()`, `prepared_count()`.
- The `CachingPull::dispatches()` test double already used to count warming
  pulls: `src/audio_engine/t/lookahead.t.cpp:203` (unit) and
  `tests/audio_lookahead_recursive.t.cpp:170` (threaded integration).
- Existing Spatial ring golden (non-Droste, must not regress):
  `src/audio_engine/t/lookahead.t.cpp:356-418` — threaded==inline Spatial mix,
  `silence_mixed()==0`.
- Precedent for a "cull warms nothing" ring assertion:
  `src/audio_engine/t/lookahead.t.cpp:721` (Flat inaudible-child cull).
- Precedent for a Droste-termination golden: `tests/nested_audio_goldens.t.cpp:308`
  (gain<1 decays to a stable mix) and `:359` (gain≥1 terminates on the budget).

**Governing design-doc sections (normative, doc 16):**
- `docs/design/12-audio.md:200-206` — **Sub-audible cull**: "Before pulling a
  contributor, if its accumulated attenuation × its edge attenuation is below
  the threshold, the layer contributes nothing and is *not descended* … a
  Droste/infinite-zoom chain crosses the threshold at a finite depth —
  terminating the recursion earlier than, but still backstopped by, the doc-05
  depth budget." (The doc frames the cull generically over *the* recursion; the
  warming walk is one instance of it.)
- `docs/design/12-audio.md:167-206` — the v1 Spatial model (attenuation
  composition, threshold).
- `docs/design/12-audio.md:208+` — the engine/lookahead section; drain
  byte-exactness rests on the warmed closure covering the mixer's pulls (a
  superset satisfies it — spatial_policy D5, citing doc 12:258-263).
- `docs/design/05-recursive-composition.md:68-70` — the shared depth budget
  that stays the hard backstop.
- `docs/design/17-internal-components.md:41,57` — the L4 `LookaheadRing` must
  not introspect an L3 `NestedContent`; the structural nesting edge stays
  injected via `d_config.nested_composition`.

**Predecessor decisions carried in (spatial_policy.md):**
- D2 — per-edge attenuation `clamp(max_scale(layer.transform),0,1)` composed
  multiplicatively, once per edge, no double counting.
- D4 — sub-audible threshold is the named default `2^-12` carried in the
  context; the depth budget stays the hard backstop.
- D5 — the ring cull is a bounded warming-cost optimization cleanly separable
  into this leaf; the drain is byte-exact whether the ring warms exactly the
  culled tree or a superset.

## Constraints / requirements

1. **Reuse the shipped predicate, no second algorithm.** The cull must use the
   same `spatial_edge_atten` helper (`content.hpp:273-282`), the same
   `accum_atten * edge_atten < sub_audible` comparison, and the same
   `k_sub_audible_atten` default (`content.hpp:236`) as `mix.cpp:64-66`. The
   warmed set must equal the tree the mixer walks — same predicate, same
   accumulation, so no drift between warming and mixing.
2. **Flat path byte-for-byte unchanged.** When `d_config.spatial` is absent
   (`std::nullopt`), `descend` behaves exactly as today — the cull is gated on
   `d_config.spatial.has_value()`, mirroring `mix.cpp:57`'s
   `request.spatial.has_value()` gate.
3. **Drain determinism invariant preserved.** After the change the drain of a
   fully-primed ring stays byte-identical between `worker_count==0` (inline)
   and `worker_count>0` (threaded) for every scene including the Spatial
   Droste scene, and `silence_mixed()` stays **0**. The warmed set is now equal
   to (still covers) the mixer's culled pull set, so no pull ever misses a
   not-yet-rendered block. No drain-path, `Contribution`, `AudioRequest`, or
   contract change.
4. **Levelization (doc 17:41,57).** `LookaheadRing` stays L4. The cull reads
   only `layer->transform` (model) and the L1-contract `spatial_edge_atten` /
   `Spatialization` (`content.hpp`); it introduces **no** new dependency on L3
   `NestedContent`. The nesting structural edge stays injected via
   `d_config.nested_composition` — untouched. No new levelization edge (doc 17
   is CI-enforced).
5. **Depth budget stays the hard backstop.** `max_depth = 64` (doc 05:68-70)
   remains the terminator for a non-shrinking (scale ≥ 1) cycle; the
   sub-audible cull is an *earlier, natural* terminator for shrinking chains,
   not a replacement. A scale-½ chain terminates at ~depth 12; a scale≥1 cycle
   still bottoms out on `max_depth`.
6. **RT-safety unaffected.** `descend` runs on the pump/worker threads, never
   the RT callback; the change adds only float multiplies and a comparison
   (zero allocation), so `audio.rt_safety`'s callback-chain annotations are
   untouched.
7. **Diff coverage ≥ 90%** on the changed lines (doc 16 CI gate).

## Acceptance criteria

1. **Behavioral-counter: warming terminates at the cull depth.** A new claim
   **`12-audio#spatial-fill-cull-terminates-warming`** in
   `tests/claims/registry.tsv`, enforced by an `// enforces:` test in
   `src/audio_engine/t/lookahead.t.cpp` (and/or the threaded
   `tests/audio_lookahead_recursive.t.cpp`). For a Spatial Droste scene
   (self-nesting composition, per-edge scale ≤ ½, `max_depth = 64`) the test
   asserts that the count of warmed contributors — measured via the want-list
   size from `prime`/`collect_wants` and/or `CachingPull::dispatches()` — is
   **finite and equals the sub-audible cull depth's contributor count**, i.e.
   equals the mixer's `pull_audio` dispatch count for the identical Spatial
   scene, and is **strictly less than** the count the same scene warms in
   **Flat** mode (which reaches `max_depth`). This is the doc's promised
   "warmed-block count drops to the cull depth" (never a wall-clock assertion,
   doc 16). The claim `extends 12-audio#spatial-sub-audible-cull-terminates-recursion`
   (the mix-walk cull, registry.tsv:168) `and 12-audio#lookahead-warms-recursive-contributor-closure`
   (the warming walk, registry.tsv:80).
2. **Determinism golden (no drain regression).** A byte-exact golden (memcmp,
   no tolerance, `parab_sine` tone oracle) proving the Spatial Droste scene's
   primed-ring drain is byte-identical between `worker_count==0` and
   `worker_count>0`, and `silence_mixed()==0` — the superset (now equal-set)
   invariant holds after the cull. Mirrors the shape of
   `lookahead.t.cpp:356-418`.
3. **No regression on the shipped Spatial ring test.** The existing non-Droste
   Spatial ring golden (`lookahead.t.cpp:356-418`, attenuations all above
   threshold) is unchanged — the cull is a no-op there — and the Flat recursive
   descent tests are unchanged.
4. **WBS gate.** After implementation, `complete 100` is added after the
   `allocate team` line of `tasks/45-audio.tji:89-94` and the `note` gains the
   `Refinement: tasks/refinements/audio/spatial_fill_cull.md` back-link;
   `tj3 project.tjp 2>&1 | grep -iE "error|warning"` is silent. Milestone
   `m6_audio` in `tasks/99-milestones.tji` already depends on this leaf (wired
   by spatial_policy's closer); it gains `complete 100` iff this is the last of
   its dependencies to land.
5. **Registers no successor.** This task fully closes the ring-cull deferral;
   it introduces no deferred follow-up. (The remaining spatial leaf,
   `audio.spatial_camera_follow`, is an independent sibling under `m6_audio`,
   not a successor of this task.)

## Decisions

- **D1 — Thread a scalar `float accum_atten` through `descend`; do not carry a
  full `Spatialization` context and do not add a field to `Contribution`.**
  *Rationale:* the warming walk only decides *which content blocks to render*.
  Pan (the √-law constant-power gains) and the composed listener transform do
  not change *which* blocks are warmed — only the multiplicative attenuation
  product drives the cull, and `spatial_edge_atten(layer->transform)` is a
  purely local per-edge quantity needing no composed listener. So the minimal
  change is a `float accum_atten` parameter on `descend` (seeded in
  `contributions_for` from `d_config.spatial ? d_config.spatial->accum_atten :
  1.0F`), multiplied by `spatial_edge_atten(layer->transform)` per edge and
  compared against `d_config.spatial->sub_audible`. A culled subtree is simply
  not emitted, so `Contribution` needs no attenuation field. *Rejected:*
  threading the full composed `Spatialization` (composing the listener per edge
  as `mix_layer` does at mix.cpp:63,72) — unnecessary work in the warming walk
  since pan is irrelevant to residency, and it would duplicate the mixer's
  listener composition for no warming benefit. *Rejected:* storing `accum_atten`
  on `Contribution` — the cull decision is made before the contribution is
  emitted, so there is nothing to store.
- **D2 — Gate the cull on `d_config.spatial.has_value()`; Flat is a byte-exact
  no-op.** *Rationale:* mirrors `mix.cpp:57`'s `request.spatial.has_value()`
  gate exactly, so the ring's warming walk and the mixer's pull walk agree
  edge-for-edge, and a Flat scene warms exactly what it warms today
  (Constraint 2). *Rejected:* keying off `d_config.policy == MixPolicy::Spatial`
  — spatial_policy Decision D1 established that the cull is keyed off the
  presence of the `spatial` context (an L3-visible field), never the L4-only
  `MixPolicy` enum, so the warming walk uses the same key.
- **D3 — Place the cull before emitting the `Contribution` and before
  recursing, byte-for-byte matching `mix.cpp:64-66`.** *Rationale:* a
  sub-audible layer must be neither pulled nor descended — matching the
  mixer's terminator exactly means the warmed set equals the culled tree, which
  is what keeps the drain byte-exact (Constraint 3). Using the shipped
  `spatial_edge_atten` and `k_sub_audible_atten` (no second algorithm,
  Constraint 1) guarantees no divergence between "what warming stops at" and
  "what mixing stops at". *Rejected:* culling only the descent but still
  emitting a leaf `Contribution` for the sub-audible layer — that would warm a
  block the mixer never pulls, wasting exactly the cost this task removes.
- **D4 — No design-doc delta.** *Authority:* doc 12:200-206 already specifies
  the sub-audible cull as terminating *the* recursion at a finite depth; the
  engine section already rests drain byte-exactness on the warmed closure
  *covering* the mixer's pulls (spatial_policy D5, citing doc 12:258-263). This
  task aligns the warming recursion with the already-documented mix cull — an
  implementation optimization, not a new architectural seam, new dependency, or
  deviation from designed behavior. Per doc 16 no doc amendment is required.
  *Rejected:* adding a doc-12 note that "the warming walk also culls" — the
  doc's cull language is already generic over the recursion; a redundant note
  would over-specify an implementation detail the constitution intentionally
  leaves to the refinement.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-08.

- Ported the `accum_atten * edge_atten < sub_audible` cull into `LookaheadRing::descend` (`src/audio_engine/lookahead.cpp`), gated on `d_config.spatial.has_value()`; Flat path is byte-for-byte unchanged.
- Added scalar `float accum_atten` parameter to `descend` (D1); seeded in `contributions_for` from `d_config.spatial->accum_atten`; composed multiplicatively per edge using the shipped `spatial_edge_atten` helper (`content.hpp:273-282`).
- New claim `12-audio#spatial-fill-cull-terminates-warming` registered in `tests/claims/registry.tsv`; enforced by two new tests in `src/audio_engine/t/lookahead.t.cpp`: behavioral-counter (warmed count equals cull depth, strictly less than Flat's full chain) and determinism golden (primed-ring drain byte-identical between `worker_count==0` and `worker_count>0`, `silence_mixed()==0`).
- `src/audio_engine/arbc/audio_engine/lookahead.hpp` updated (scalar `accum_atten` param threaded through `descend` signature).
- Spatial Droste scene warms to depth ~13 (cull depth) vs. Flat's full 64-level chain; non-Droste Spatial golden (`lookahead.t.cpp:356-418`) and all Flat recursive descent tests unchanged.
- Tech-debt surfaced: `audio.spatial_nested_warm_context` — pump warms nested contributors Flat while mixer pulls them with a spatial child-context under the same spatial-agnostic `BlockKey`; registered in `tasks/45-audio.tji`.
