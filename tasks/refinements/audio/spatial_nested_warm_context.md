# audio.spatial_nested_warm_context — Nested-Spatial pump warm context vs mixer pull context mismatch

## TaskJuggler entry

Back-link: [`tasks/45-audio.tji:96-101`](../../45-audio.tji).

> task spatial_nested_warm_context "Nested-Spatial pump warm context vs mixer pull context mismatch" {
> &nbsp;&nbsp;effort 1d
> &nbsp;&nbsp;allocate team
> &nbsp;&nbsp;depends !spatial_fill_cull
> &nbsp;&nbsp;note "The pump warms nested contributors Flat (no spatial child-context on AudioRequest, lookahead_pump.cpp:119) while the mixer pulls them with a spatial child-context under the same spatial-agnostic BlockKey; a threaded (cache) nested-Spatial drain may diverge from a true inline spatial render. Pre-existing (spatial_policy), unverified — surfaced during audio.spatial_fill_cull. Investigate whether divergence occurs and fix the BlockKey or context mismatch if confirmed. Source: tasks/refinements/audio/spatial_fill_cull.md. Doc 12."

## Effort estimate

`1d` (`tasks/45-audio.tji:97`). The fix is localized to two seams already
built by predecessors: augment `LookaheadRing::descend` /
`Contribution` / `PrefetchWant` in `arbc::audio-engine` (L4) to reconstruct
and carry the per-edge `Spatialization` the mixer already builds, and thread
that context onto the `AudioRequest` the pump submits in `arbc::runtime`
(L5). No contract change, no `BlockKey` change, no design-doc delta. The day
is: (a) the confirming test that reproduces the divergence against the inline
oracle, (b) the descend/pump augmentation, (c) the byte-exact regression
golden + behavioral counter + a TSan extension (concurrency-touching, doc 16).

## Inherited dependencies

- **`audio.spatial_fill_cull`** — *settled, DONE 2026-07-08*
  ([`tasks/refinements/audio/spatial_fill_cull.md`](spatial_fill_cull.md)).
  Added the scalar `float accum_atten` parameter to `descend`
  (`src/audio_engine/lookahead.cpp`), seeded from
  `d_config.spatial->accum_atten` in `contributions_for`, and ported the
  `mix.cpp:64-66` sub-audible cull into the warming descent (D1/D3). Its D1
  rationale — *"only the scalar attenuation product drives the cull; pan and
  the composed listener do not change WHICH blocks warm, so no full
  `Spatialization` is threaded"* (`lookahead.hpp:280-282`) — is correct for
  the cull decision but is exactly the reasoning that left the warmed block
  *content* Flat; this task augments (does not revert) that descent. This is
  the task that surfaced the present tech debt.
- **`audio.spatial_policy`** — *settled, DONE 2026-07-08*
  ([`tasks/refinements/audio/spatial_policy.md`](spatial_policy.md)).
  Introduced `std::optional<Spatialization> spatial{}` on `AudioRequest`
  (D1) as the sole carrier of the Spatial context across the pull boundary;
  the branch keys off `request.spatial.has_value()`, never the L4
  `MixPolicy` enum. Defined `Spatialization` (`content.hpp:247-264`),
  `spatial_edge_atten`, `spatial_pan_gains`, `k_sub_audible_atten`, and the
  duplicated Spatial branch at both walk sites (`mix_layer` L4,
  `NestedContent::mix_child_layer` L3). Its D5/Constraint 7 asserted the ring
  "warms a *superset* of what the culling mixer pulls," keeping the drain
  byte-exact — a coverage argument over *which* keys warm that is silent on
  the warmed block's *content*.
- **`audio.lookahead_recursive_prefetch`** — *settled, DONE 2026-07-08*
  ([`tasks/refinements/audio/lookahead_recursive_prefetch.md`](lookahead_recursive_prefetch.md)).
  Made the warming fill transitive (recursive `Contribution.children`,
  injected `nested_composition` enumerator, bottom-up residency gating) and
  landed the doc 12 Recursion delta (doc 12:210-222) making
  `worker_count>0 ↔ worker_count==0` byte-identity normative "for nested and
  below-rate scenes." Its claim
  `12-audio#lookahead-warms-recursive-contributor-closure`
  (`registry.tsv:80`) does not enumerate Spatial — the hole this task fills.
- **`audio.mix_engine`** / **`audio.lookahead`** — *settled, DONE 2026-07-07 / -08*.
  `mix_engine` established `pull_audio` cache-first single-settle on the
  spatial-agnostic `BlockKey`; `lookahead` built the ring/pump/worker-pool
  and the `contribution_key` deliberately byte-identical to the pull-side key
  (`lookahead.cpp:276-286` ↔ `pull_service.cpp:301`).

## What this task is

**Investigate and fix the warm-context / pull-context mismatch for Spatial
nested contributors.** Concretely:

1. **Confirm the divergence** (the investigation the task mandates) with a
   test scene that provably triggers it: a Spatial monitor over a scene whose
   contributor is a **nested composition holding an off-center child**, so the
   nested composition's *internal* Spatial mix differs from its Flat mix.
   Drive it through the threaded pump (`worker_count>0`) and compare the drain
   to the inline oracle (`worker_count==0`). This test **fails before the fix**
   and becomes the regression golden.
2. **Fix the context mismatch.** Augment `LookaheadRing::descend` to
   reconstruct, per edge, the same `Spatialization` the mixer's `mix_layer`
   builds — the composed listener `compose(parent_listener, layer.transform)`,
   the viewport extent, the accumulated attenuation product (already
   threaded), and the sub-audible threshold — and carry it onto the
   `Contribution`, then onto the `PrefetchWant`, so the pump populates
   `AudioRequest.spatial` for each nested contributor. The warmed block is
   then rendered under the identical context the mixer pulls it with, so the
   Flat-vs-Spatial content mismatch cannot arise for the anchor block's
   contributor closure.
3. **Pin it** with a byte-exact golden (threaded == inline), a
   behavioral-counter assertion (`silence_mixed() == 0`; the warmed nested
   block is a zero-dispatch cache hit), a new claims-register entry, and a
   TSan extension.

**Out of scope**, deferred to named leaves:

- **The residual multi-context `BlockKey` collision** — two *distinct* spatial
  contexts over the *same* `(content, revision, block_index, rate)` (two
  embeddings of one nested composition at different positions but the same
  time map; or two monitors of differing policy/listener sharing one
  `BlockCache`) collide on one cache slot even after this fix, because the key
  carries no spatial dimension. Deferred to
  `audio.spatial_blockkey_disambiguation` (see Acceptance criteria).
- **Live camera-follow** — `audio.spatial_camera_follow` (unrelated seam).
- **HRTF / distance models / non-collapsing per-leaf pan** — doc 12:162-165,
  parking lot (not WBS work).

## Why it needs to be done

Doc 12's Recursion section, as amended by the
`audio.lookahead_recursive_prefetch` delta (doc 12:210-222), makes the
threaded fill (`worker_count>0`) **byte-identical to the inline fill** a
normative guarantee — "the recursion never mixes silence for a
not-yet-rendered descendant … the closure the fill warms is exactly the tree
the mixer would walk." That guarantee currently holds for Flat, nested, and
below-rate scenes but is **violated for Spatial scenes containing a
nested-composition contributor**: the threaded drain reads a Flat-warmed
nested block where the inline path renders a Spatial one. A device monitor in
Spatial mode over such a scene therefore produces different (wrong) audio than
the offline/inline oracle — a silent correctness regression against the
design's stated byte-identity, discoverable only once a Spatial nested scene
is actually driven through the threaded pump (no shipped golden does).

## Inputs / context

**Governing design doc — doc 12 (normative, doc 16):**

- `docs/design/12-audio.md:167-206` — *The v1 Spatial model (concrete)*. The
  spatial context "carries the composed transform into the mix — including
  across the pull boundary into nested contributors … so a nested composition
  spatializes on the same footing as the root." Descending at embedding `E`
  composes `compose(listener, E)` and multiplies accumulated attenuation.
- `docs/design/12-audio.md:284-304` — *Recursion*, incl. the
  `lookahead_recursive_prefetch` delta (doc 12:210-222): the fill warms the
  transitive closure and the threaded fill is byte-identical to inline. This
  task makes that hold for Spatial too — a bugfix restoring the stated
  guarantee, **not** a design change, so no delta.
- `docs/design/12-audio.md:249-254` — *Prefetch and caching*: the block cache
  key is `(content id, revision, block index, rate)` — spatial-agnostic. This
  key is correct for spatially-invariant leaf content (a leaf's samples do not
  depend on the spatial context; the parent applies pan/atten) but under-
  specifies a nested composition whose `render_audio` output *does* depend on
  the context. This task does not change the key; the residual collision it
  cannot cover is the deferred follow-up.

**Doc 17 levelization (CI-enforced):**

- `docs/design/17-internal-components.md:57` — `arbc::audio-engine` is L4,
  depending on `contract` + `cache`. `descend` may compose `Affine`
  transforms and call `spatial_edge_atten` / build `Spatialization` — all L1
  `contract` types (`content.hpp`), already used by `descend` today (the
  scalar cull). **No new levelization edge** is introduced.
- The pump (`arbc::runtime`, L5) already consumes `PrefetchWant` from the ring
  (L4→L5) and constructs the `AudioRequest`; adding an optional
  `Spatialization` field to the want and reading it in the pump stays within
  the existing L5→L4 dependency. `descend` must **not** call L3
  `NestedContent` render code (levelization) — it re-expresses the per-edge
  listener composition structurally, exactly as the shipped
  `nested_composition` enumerator re-expresses the descent (Constraint 4 of
  `lookahead_recursive_prefetch`).

**Code seams (the mismatch, verified):**

- **Flat warm (write side).**
  [`src/runtime/lookahead_pump.cpp:113-122`](../../../src/runtime/lookahead_pump.cpp)
  (`fill_and_insert`): a **6-field** aggregate `AudioRequest{w.window, w.rate,
  w.layout, targets[i], Exactness::BestEffort, StateHandle{}}` — the trailing
  7th field `spatial` defaults to `nullopt` ⇒ Flat. Submitted as
  `AudioTask{content, req, done}`
  ([`src/runtime/arbc/runtime/audio_worker_pool.hpp:42-45`](../../../src/runtime/arbc/runtime/audio_worker_pool.hpp))
  straight to `render_audio` on the worker — no spatial context is
  reconstructed. For a `NestedContent` contributor this warms a **Flat
  internal mix**.
- **Spatial pull (read side).**
  [`src/audio_engine/mix.cpp:57-74`](../../../src/audio_engine/mix.cpp)
  derives per-edge `child_spatial = Spatialization{compose(sp.listener,
  layer.transform), viewport…, sp.accum_atten*edge_atten, sp.sub_audible}` and
  passes it as the 7th field of `child_req` at `mix.cpp:113-122`, then
  `pull.pull_audio(content, child_req, done)` (`mix.cpp:129`). The below-rate
  native re-request (`mix.cpp:168-177`) carries the same `child_spatial`. The
  L3 twin is `NestedContent::mix_child_layer`
  (`src/kind_nested/nested_content.cpp:425-486`).
- **The discriminator.**
  [`src/contract/arbc/contract/content.hpp:320-333`](../../../src/contract/arbc/contract/content.hpp)
  — `AudioRequest.spatial` (trailing, defaulted); `content.hpp:247-264` —
  `Spatialization`. `NestedContent::render_audio` branches purely on
  `request.spatial.has_value()` (`nested_content.cpp:425`).
- **The spatial-agnostic key.**
  [`src/cache/arbc/cache/key_shapes.hpp:83-90`](../../../src/cache/arbc/cache/key_shapes.hpp)
  — `BlockKey{content, revision, block_index, rate}`, `= default` equality;
  hash at `key_shapes.hpp:163-171`. No spatial dimension. Write-side key:
  `LookaheadRing::contribution_key` (`src/audio_engine/lookahead.cpp:276-286`);
  read-side key: `PullServiceImpl::pull_audio`
  (`src/compositor/pull_service.cpp:301`, hit test `:303-318`). Deliberately
  byte-identical — so the Flat warm satisfies the Spatial read.
- **The warming descent (the fix site).**
  [`src/audio_engine/arbc/audio_engine/lookahead.hpp:257-292`](../../../src/audio_engine/arbc/audio_engine/lookahead.hpp)
  — `Contribution{content, rate, layout, frames, window_start, children}`
  (`:257-264`), `PrefetchWant{key, content, window, rate, layout, frames}`
  (`:123-130`), `descend(composition, request_rate, window_start, depth,
  accum_atten, out)` (`:283-284`), `make_want` (`:287`),
  `native_rerequest_want` (`:291`). `contributions_for` (`lookahead.cpp:172-185`)
  seeds `accum_atten` from `d_config.spatial->accum_atten`. The comment at
  `lookahead.hpp:280-282` states the current scalar-only assumption this task
  augments.

**Existing claims to extend, not duplicate:**

- `12-audio#lookahead-warms-recursive-contributor-closure`
  (`tests/claims/registry.tsv:80`) — threaded == inline for nested/below-rate;
  silent on Spatial.
- `12-audio#spatial-fill-cull-terminates-warming` (`registry.tsv:169`) — the
  warming cull; asserts threaded==inline with `silence_mixed()==0` for the
  Spatial *Droste* warming-cost case.
- `12-audio#spatial-sub-audible-cull-terminates-recursion` (`registry.tsv:168`).

## Constraints / requirements

1. **Warm context == pull context, per edge.** The `Spatialization` `descend`
   attaches to each `Contribution`/`PrefetchWant` must be **byte-identical** to
   the one `mix_layer` builds for the same edge: same
   `compose(parent_listener, layer.transform)` (per-edge composition, never
   accumulated — doc 04, doc 11:187-188), same viewport extent, same
   `accum_atten * edge_atten` product (reuse the scalar already threaded), same
   `sub_audible`. Any divergence reintroduces the bug one edge down.
2. **Flat stays a byte-exact no-op.** When `d_config.spatial == nullopt`,
   `descend` attaches no `Spatialization`, the want's field stays `nullopt`,
   and the pump submits the same 6-field-equivalent `AudioRequest` — every
   existing Flat/leaf golden and the shipped nested-of-tones threaded golden
   remain byte-identical. The `PrefetchWant`/`Contribution` aggregate gains a
   trailing defaulted `std::optional<Spatialization> spatial{}` so existing
   initializers still compile.
3. **No contract, `BlockKey`, or cache-layer change.** The fix lives entirely
   in `arbc::audio-engine` (descend/Contribution/PrefetchWant/make_want) and
   `arbc::runtime` (the pump's `AudioRequest` construction). `BlockKey`,
   `AudioRequest`, `Spatialization`, and `pull_service.cpp` are untouched.
4. **Levelization.** `descend` reads only `layer->transform` and L1-contract
   helpers (`compose`, `Affine`, `Spatialization`, `spatial_edge_atten`,
   `spatial_pan_gains` if needed); it does not depend on L3 `NestedContent`.
   The nesting edge stays injected via `d_config.nested_composition`.
5. **The scalar cull is preserved.** The augmentation is *additive* to
   `spatial_fill_cull`'s sub-audible cull: `descend` still culls on
   `accum_atten * edge_atten < sub_audible` (neither warmed nor descended), and
   additionally reconstructs the per-edge `Spatialization` for the contributors
   it *does* warm. The warmed set is unchanged; only each warmed want's context
   is now populated.
6. **Threaded == inline byte-identity restored for Spatial scenes.** Draining a
   fully-primed ring in order yields blocks bit-identical between
   `worker_count==0` (inline oracle) and `worker_count>0` (threaded) for every
   scene, now including a Spatial scene with a nested-composition contributor;
   `silence_mixed()` stays 0. No tolerances (doc 16).
7. **Diff coverage ≥ 90%** on changed lines (doc 16 CI gate).

## Acceptance criteria

**Claims-register growth.** Add
`12-audio#spatial-warms-nested-with-pull-context` to
`tests/claims/registry.tsv`, enforced by an `// enforces:`-tagged test:

> The lookahead warming descent reconstructs, per edge, the same
> `Spatialization` the mixer's `mix_layer` builds (composed listener =
> `compose(parent_listener, layer.transform)`, viewport extent, accumulated
> attenuation product, sub-audible threshold) and carries it onto the
> `AudioRequest` the pump submits for each warmed contributor, so a
> spatial-context-consuming contributor (a nested composition) is warmed under
> the identical context the mixer pulls it with; therefore the threaded
> (`worker_count>0`) drain of a Spatial scene containing a nested-composition
> contributor is byte-identical to the inline (`worker_count==0`) spatial
> render, with `silence_mixed()==0`. Flat (`spatial` absent) warms with no
> context — a byte-exact no-op.

`extends 12-audio#lookahead-warms-recursive-contributor-closure`
(`registry.tsv:80`) `and 12-audio#spatial-fill-cull-terminates-warming`
(`registry.tsv:169`).

**Confirming test → byte-exact regression golden.** In
`src/runtime/t/lookahead_pump.t.cpp` (or extend `tests/nested_audio_goldens.t.cpp`),
drive a Spatial monitor over a scene whose contributor is a **nested
composition holding an off-center child tone** (so its internal Spatial mix ≠
its Flat mix), through `LookaheadPump` + `AudioWorkerPool` with
`worker_count>0`. Assert the primed-ring drain is byte-identical
(`std::memcmp`, no tolerance) to the `worker_count==0` inline render and to a
direct `mix_composition` oracle over the same windows. Oracle tones use the
integer-flick `parab_sine` generator and √-law pan via IEEE `std::sqrt` (never
`std::sin`/`sin`/`cos`), per the sibling goldens. **This test must be verified
to fail on the pre-fix tree** (documented in the test's comment as the
confirmed divergence) and pass after.

**Behavioral-counter assertions** (wall-clock-free, doc 16). On the primed
Spatial nested scene with `worker_count>0`: (a) `silence_mixed() == 0` (the
transitive-residency gate held); (b) the mixer's `pull_audio` for the warmed
nested contributor is a **zero-dispatch cache hit** (reuse the audio-dispatch
counter, `src/compositor/arbc/compositor/counters.hpp`) — proving the warmed
block is both resident *and* the spatially-correct one the mixer wanted; (c)
no regression on the shipped Flat/nested/Droste goldens
(`src/audio_engine/t/lookahead.t.cpp:356-418`, the recursive golden in
`tests/audio_lookahead_recursive.t.cpp`).

**Concurrency / TSan** (concurrency-touching, doc 16). Extend
`tests/audio_lookahead_concurrency.t.cpp` with the Spatial nested scene under
concurrent fill + drain: each `AudioCompletion` settles exactly once, no data
race, drain byte-identical to inline.

**No new conformance family.** No new content kind or operator; the contract
conformance suite is unaffected.

**Deferred follow-up (closer registers in WBS).** Register
`audio.spatial_blockkey_disambiguation` — *effort `2d`, depends
`!spatial_nested_warm_context`, milestone `m6_audio`* — a real WBS leaf:

> Disambiguate the spatial-agnostic `BlockKey` so two *distinct* spatial
> contexts over the same `(content, revision, block_index, rate)` do not
> collide on one cache slot. Reproduce: a Spatial scene embedding one nested
> composition twice at different spatial positions but the same time map (same
> block window), or two monitors of differing policy/listener sharing one
> `BlockCache` — the threaded drain reads one warmed block for both and
> diverges from the inline oracle (which renders each fresh). Fix: add a
> spatial-context digest to `BlockKey` (`key_shapes.hpp:83-90`), populated only
> when `request.spatial` is present so Flat and leaf-only scenes key
> byte-identically to today (accept the leaf-under-Spatial cache duplication —
> doc 12:252 "caching matters less"); thread it through `contribution_key`
> (`lookahead.cpp:276-286`) and `pull_service.cpp:301`; update the hash
> (`key_shapes.hpp:163-171`). **Design-doc delta**: amend doc 12:249-254's key
> definition to add the spatial digest. Golden: the two-embedding Spatial scene
> drains threaded == inline byte-identically. `note` cites this refinement and
> `docs/design/12-audio.md:249-254`.

Note for the closer: this is concrete, agent-implementable work (a key field +
hash + two construction sites + a golden + a doc delta), not an audit — it has
a definite deliverable and a definite done-test.

**WBS gate.** After the closer marks `complete 100`,
`tj3 project.tjp 2>&1 | grep -iE "error|warning"` is silent.

**Registers no successor beyond the named `audio.spatial_blockkey_disambiguation`.**

## Decisions

**D1 — Fix the *context* (warm nested contributors with the pull context),
not the *key*, for this task.** `descend` reconstructs the per-edge
`Spatialization` and the pump populates `AudioRequest.spatial`; the warmed
block is rendered under the identical context the mixer reads it with, so the
Flat-vs-Spatial content mismatch is eliminated for the anchor block's
contributor closure.
*Rationale:* the confirmed, reachable divergence is single-context (one
Spatial monitor, one embedding per key). Under one monitor there is exactly
one spatial context per `BlockKey`, so warming with that context is exact —
threaded == inline byte-identity is restored with no key, contract, cache, or
design-doc change, entirely inside the two components predecessors already
own. This directly serves doc 12:169-172 ("carries the composed transform …
across the pull boundary into nested contributors").
*Rejected — key-only (spatial digest without fixing the warm context):* the
pump would still warm Flat, the Spatial pull would then *miss* the Flat key,
the residency gate would never be satisfied for the warmed closure, and the
fill would either stall or mix silence — breaking the residency invariant
`lookahead_recursive_prefetch` established. Correct warming is required
regardless; the key change is neither sufficient nor, for the single-context
case, necessary.
*Rejected — both (fix context *and* change the key now):* the key change is a
broader, project-shaping cache-layer edit fighting doc 12:249's stated 4-field
key, needing a design-doc delta and touching L3 `cache`, all sites, and leaf-
cache duplication tradeoffs — out of proportion to a `1d` task and to the
confirmed single-context bug. Deferred to
`audio.spatial_blockkey_disambiguation` where it gets its own golden and delta.

**D2 — Augment `descend` to thread the composed listener, superseding
`spatial_fill_cull` D1's "scalar-only" rationale — additively.** `descend`
keeps its scalar `accum_atten` cull (unchanged) *and* now accumulates the
composed listener `Affine` down each edge to build the per-edge
`Spatialization` for the contributors it warms, attached to
`Contribution`/`PrefetchWant`.
*Rationale:* `spatial_fill_cull` D1 correctly observed the listener does not
change *which* blocks warm — true for the cull decision. But the warmed
block's *content* (a nested composition's internal mix) *does* depend on the
full context, which is precisely what this task must fix. The two statements
are compatible: the scalar drives the cull; the full context populates the
warmed request. The source comment at `lookahead.hpp:280-282` is updated in
the fix commit to record this (a source comment, not a design doc — no delta).
*Rejected — reconstruct the context in the pump instead of the ring:* the pump
(L5) lacks the tree-walk state (the parent chain of embedding transforms); the
per-edge listener composition only exists inside `descend`. Reconstructing it
in the pump would duplicate the descent structurally at the wrong level.

**D3 — Carry the full `Spatialization` on the want, not a
listener-transform-only subset.** The want gains
`std::optional<Spatialization> spatial{}`, mirroring `AudioRequest.spatial`
exactly.
*Rationale:* the pump copies it straight onto `AudioRequest.spatial` with no
reconstruction, guaranteeing byte-identity with the mixer's `child_req` (which
carries the same full struct). A subset would force the pump to re-derive
`accum_atten`/`sub_audible`, reintroducing a divergence surface.
*Rejected — a listener `Affine` only:* the pump would have to recombine it with
attenuation/threshold/viewport, a second place for the two contexts to drift.

**D4 — The confirming test uses a *distinct* nested composition with an
off-center child, not the self-embedding Droste.** A Droste self-embed
collapses every recursion depth to one `BlockKey` (the spatial transform is a
zoom; the time map is unchanged), so it does not cleanly exhibit a
Flat-vs-Spatial *content* difference the way a distinct nested composition
with an off-center (non-centered pan) child does.
*Rationale:* this guarantees the pre-fix divergence is real and observable and
that the golden pins the exact behavior. It also explains why the shipped
`spatial_fill_cull` Droste byte-identity golden (`registry.tsv:169`, focused on
cull *termination* / warmed-count) did not catch this — no shipped golden
drives a distinct nested-composition contributor with an internally
spatialized off-center child through the threaded pump. That coverage hole is
what left the mismatch "unverified" per the task note.

**D5 — No design-doc delta.** Doc 12:210-222 (the
`lookahead_recursive_prefetch` delta) already makes threaded == inline
byte-identity normative; this task makes it *hold* for Spatial scenes. A
bugfix restoring a stated guarantee amends no designed behavior.
*Consequence, accepted:* the residual multi-context key collision (D1's
deferral) *is* a genuine gap in that guarantee that this task does not close;
it is surfaced as a WBS leaf with its own delta rather than silently left in
prose.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-08.

- `src/audio_engine/arbc/audio_engine/lookahead.hpp` — `Contribution` and `PrefetchWant` gained trailing `std::optional<Spatialization> spatial{}` field; `descend` signature extended with composed-listener threading; updated comment at `:280-282` to record additive scalar-cull + full-context augmentation.
- `src/audio_engine/lookahead.cpp` — `descend` reconstructs per-edge `Spatialization` (composed listener via `compose(parent_listener, layer.transform)`, viewport/sub-audible from `d_config.spatial`, scalar `accum_atten` product reused for cull) onto each `Contribution`/`PrefetchWant`; `contributions_for` seeds the composed listener from `d_config.spatial`.
- `src/runtime/lookahead_pump.cpp` — `fill_and_insert` threads `w.spatial` onto the worker `AudioRequest`, so nested-composition contributors are warmed under the identical spatial context the mixer pulls them with.
- `tests/audio_lookahead_spatial_nested.t.cpp` (new) — byte-exact Spatial-nested drain == direct `mix_composition` oracle for `worker_count` 0 and 4; divergence-witness Spatial≠Flat; `silence_mixed()==0` and zero-dispatch cache-hit (`pull.dispatches()==0`). Verified to fail on pre-fix tree (pump's spatial threading reverted → oracle mismatch at line 310).
- `tests/audio_lookahead_concurrency.t.cpp` — TSan extension: Spatial-nested case (settle-once, `silence_mixed()==0`, drain==inline Spatial oracle under concurrent fill+drain).
- `tests/claims/registry.tsv` — new claim `12-audio#spatial-warms-nested-with-pull-context`.
- `tests/CMakeLists.txt` — registered `audio_lookahead_spatial_nested` test target.
- Deferred to `audio.spatial_blockkey_disambiguation` (registered in WBS): two-embedding multi-context `BlockKey` collision not covered by this fix.
