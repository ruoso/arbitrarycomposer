# audio.spatial_blockkey_disambiguation — Disambiguate the spatial-agnostic BlockKey for multi-context Spatial scenes

## TaskJuggler entry

Back-link: [`tasks/45-audio.tji:103-108`](../../45-audio.tji).

> task spatial_blockkey_disambiguation "Disambiguate spatial-agnostic BlockKey for multi-context Spatial scenes" {
> &nbsp;&nbsp;effort 2d
> &nbsp;&nbsp;allocate team
> &nbsp;&nbsp;depends !spatial_nested_warm_context
> &nbsp;&nbsp;note "Add a spatial-context digest to BlockKey so two distinct spatial contexts over the same (content, revision, block_index, rate) do not collide on one cache slot. Reproduce: a Spatial scene embedding one nested composition twice at different spatial positions but the same time map, or two monitors of differing policy/listener sharing one BlockCache. Fix: add spatial digest to BlockKey (key_shapes.hpp:83-90), thread through contribution_key (lookahead.cpp:276-286) and pull_service.cpp:301, update hash (key_shapes.hpp:163-171). Design-doc delta: amend doc 12:249-254 key definition to add spatial digest. Golden: two-embedding Spatial scene drains threaded==inline byte-identically. Source: tasks/refinements/audio/spatial_nested_warm_context.md. Doc 12."

## Effort estimate

`2d` (`tasks/45-audio.tji:104`). Larger than its `1d` predecessor because the
change touches a cache-layer key shape (`BlockKey` + its hash) and every
`BlockKey` construction site under a spatial context — the root/anchor, each
contribution, the below-rate native re-request, and the read-side pull — and
carries a design-doc delta amending a normative key definition. The two days
are: (a) the confirming multi-context test that reproduces the collision
against a fresh-render oracle; (b) the digest helper in `contract`, the
opaque digest field + hash update in `cache`, and the four key-build sites in
`audio-engine`/`compositor`; (c) the byte-exact regression golden, behavioral
counters, and a TSan/stress extension over a shared cache; (d) the doc 12
delta (written with this refinement, lands in the closer's commit).

## Inherited dependencies

- **`audio.spatial_nested_warm_context`** — *settled, DONE 2026-07-08*
  ([`tasks/refinements/audio/spatial_nested_warm_context.md`](spatial_nested_warm_context.md)).
  This is the task that **named and deferred** the present work
  (`spatial_nested_warm_context.md:305-328`, its D1 consequence and D5). It
  fixed the warm-*context* mismatch — `descend` now reconstructs the per-edge
  `Spatialization` and the pump populates `AudioRequest.spatial` so a nested
  contributor is warmed under the same context the mixer pulls it with — but
  deliberately left the *key* spatial-agnostic (its D1). Its byte-identity
  invariant is the load-bearing precondition here: the write-side warm
  `Spatialization` is **bit-identical** to the read-side mixer `Spatialization`
  for the same edge (`spatial_nested_warm_context.md:217-223`, Constraint 1),
  so a digest computed from each side lands the same value and the warm key
  still equals the pull key. This task adds the *dimension* that
  distinguishes two *distinct* such contexts.
- **`audio.spatial_fill_cull`** — *settled, DONE 2026-07-08*
  ([`tasks/refinements/audio/spatial_fill_cull.md`](spatial_fill_cull.md)).
  Established the scalar `accum_atten` product threaded through `descend` and
  the sub-audible cull; those scalars are part of the `Spatialization` this
  task digests.
- **`audio.spatial_policy`** — *settled, DONE 2026-07-08*
  ([`tasks/refinements/audio/spatial_policy.md`](spatial_policy.md)).
  Defined `Spatialization` (`content.hpp:247-264`), `AudioRequest.spatial`
  (`content.hpp:320-333`), `spatial_edge_atten`, `spatial_pan_gains`, and the
  `request.spatial.has_value()` branch that is the sole Spatial discriminator
  (never the L4 `MixPolicy` enum). The digest is a pure function of that
  struct.
- **`audio.mix_engine`** / **`audio.lookahead`** — *settled, DONE 2026-07-07 / -08*.
  `mix_engine` established `pull_audio` as a cache-first single-settle probe on
  `BlockKey` (`pull_service.cpp:301`, hit test `:303-318`, claim
  `12-audio#pull-audio-is-cache-first-single-settle`); `lookahead` built
  `contribution_key` deliberately byte-identical to the pull key
  (`lookahead.cpp:302-312` ↔ `pull_service.cpp:301`). Both key-build sites
  gain the digest here, preserving that byte-identity.

## What this task is

**Add a spatial-context dimension to the block cache key so two distinct
spatial contexts over the same `(content, revision, block index, rate)` no
longer collide on one cache slot.** Concretely:

1. **Confirm the collision** with a test scene that provably triggers it: a
   single Spatial monitor over a composition that embeds *one* nested
   composition **twice, as two layers with different transforms (positions)
   but the same time map** — so both embeddings resolve to the identical
   `(content id, revision, block index, rate)` but must render under
   *different* composed listeners (different pan). Under one shared
   `BlockCache`, both the threaded (`worker_count>0`) and inline pull
   (`worker_count==0`) paths warm/pull the first embedding's block, then serve
   *that same block* for the second embedding — diverging from a direct
   fresh-render `mix_composition` oracle that renders each embedding under its
   own context. This test **fails before the fix** and becomes the regression
   golden.
2. **Add the digest.** A pure `contract` helper reduces a `Spatialization` to
   a 64-bit digest; `BlockKey` gains a trailing opaque `std::uint64_t
   spatial_digest{0}` (zero ⇔ Flat/absent context). Every `BlockKey`
   construction site that keys spatially-dependent content folds in the digest
   of the context under which that block renders — the root/anchor
   (`d_config.spatial`), each `contribution_key` (`c.spatial`), the below-rate
   `native_rerequest_want` (`c.spatial`), and the read-side `pull_audio`
   (`request.spatial`). The `std::hash<BlockKey>` combines the new field. The
   `cache` layer stays spatially agnostic — it carries an opaque scalar and
   never sees `Spatialization` (levelization).
3. **Pin it** with a byte-exact golden (multi-context drain == per-context
   fresh oracle, for both worker counts), behavioral-counter assertions (two
   distinct cache slots ⇒ two dispatches, not one; `silence_mixed()==0`), a
   new claims-register entry, and a TSan/stress extension over the shared
   cache.

**Out of scope**, surfaced to named leaves / the parking lot:

- **HRTF / distance models / non-collapsing per-leaf pan** — doc 12:162-165,
  parking lot (not WBS work; a human design call, never an "audit" task).
- **Live camera-follow** — `audio.spatial_camera_follow` (unrelated seam; a
  live listener still keys correctly here because each tick's listener yields
  its own digest).
- **A content-affecting-subset digest** (digesting only the fields that change
  a given content's output) — rejected below (D3); if ever pursued it is a
  cache-efficiency optimization, not correctness, and gets its own task then.

## Why it needs to be done

Doc 12's Recursion section makes the threaded fill (`worker_count>0`)
byte-identical to the inline fill (`worker_count==0`) a normative guarantee,
and `spatial_nested_warm_context` extended it to hold for a *single* Spatial
context. But the block cache key is `(content, revision, block index, rate)`
— **spatially agnostic** (`docs/design/12-audio.md:249-254` as it stood;
`key_shapes.hpp:83-90`). For a nested composition, whose `render_audio` output
*depends on* the spatial context (listener → pan/atten of its children), two
distinct contexts over the same key **collide on one cache slot**: the first
render fills the slot, the second reads it back and gets the wrong (foreign)
context's block. Any scene with two embeddings of one nested composition at
different positions — or two monitors of differing listener/policy sharing one
`BlockCache` — therefore produces wrong audio, silently, against the design's
byte-identity-to-a-fresh-render promise. `spatial_nested_warm_context` fixed
*single*-context correctness and explicitly deferred this residual
multi-context collision (its D1 consequence, D5), naming this task as the WBS
leaf that closes it with its own golden and doc delta.

## Inputs / context

**Governing design doc — doc 12 (normative, doc 16):**

- `docs/design/12-audio.md:167-206` — *The v1 Spatial model (concrete)*. The
  `Spatialization` context is a listener transform, viewport extent,
  accumulated attenuation scalar, and sub-audible threshold; a nested
  composition spatializes "on the same footing as the root," so its rendered
  block content depends on the full context. This is the model the digest
  reduces.
- `docs/design/12-audio.md:249-254` — *Prefetch and caching*: the key
  definition. **This task's delta amends it** to add the spatial-context
  digest (see Decisions D5 and the delta below).
- `docs/design/12-audio.md:283-304` — *Recursion*: the threaded==inline
  byte-identity guarantee and the residency invariant (a contributor block is
  dispatched only once its closure is resident). The digest preserves this:
  the write-side warm key and read-side pull key stay byte-identical because
  both digest the *same* per-edge `Spatialization`.

**Doc 17 levelization (CI-enforced) — the decisive constraint:**

- `docs/design/17-internal-components.md:32,54` — `arbc::cache` is **L3**,
  depending only on `base` + `surface`. `docs/design/17-internal-components.md:32`
  places `contract` and `cache` as **peers on the same level (L3)**; a
  same-level edge is forbidden (doc 17:40-44). `BlockKey` (in `cache`)
  therefore **cannot** embed `Spatialization` (in `contract`) — exactly the
  constraint the existing `TileMeta` obeys by *mirroring* contract's fields
  rather than reusing the type (`key_shapes.hpp:92-102`). The digest is an
  **opaque scalar**; the `Spatialization`→digest reduction lives above the
  cache. `Affine` itself is in `arbc::base` (`base/transform.hpp:12-40`,
  6 `double` coefficients + defaulted `==`), which `cache` *may* use — but the
  digest is computed from the full `Spatialization`, which it may not.
- `docs/design/17-internal-components.md:56-57` — `arbc::compositor` and
  `arbc::audio-engine` are **L4**, both depending on `contract` + `cache`.
  They already hold both the `Spatialization` and the `BlockKey`, so computing
  the digest and folding it into the key at these sites introduces **no new
  levelization edge**. The digest helper is placed in `contract` (L3), next to
  `spatial_edge_atten`/`spatial_pan_gains`, and consumed by both L4 engines.

**Code seams (real paths + lines):**

- **The key (change site).**
  [`src/cache/arbc/cache/key_shapes.hpp:83-90`](../../../src/cache/arbc/cache/key_shapes.hpp)
  — `struct BlockKey { ObjectId content; std::uint64_t revision; std::int64_t
  block_index; std::uint32_t rate; }`, `= default` equality; hash at
  `key_shapes.hpp:163-171`. Gains a trailing `std::uint64_t spatial_digest{0}`;
  `= default` equality picks it up automatically; the hash folds it via the
  existing `detail::key_hash_combine` (`key_shapes.hpp:136-138`). The
  `TileMeta` comment at `key_shapes.hpp:92-102` is the precedent for
  cache-mirrors-not-reuses.
- **The Spatialization to digest (read-only input).**
  [`src/contract/arbc/contract/content.hpp:247-264`](../../../src/contract/arbc/contract/content.hpp)
  — `Spatialization{ Affine listener; double viewport_w; double viewport_h;
  float accum_atten; float sub_audible; }`. `AudioRequest.spatial`
  (`content.hpp:320-333`) is the optional carrier. New helper
  `spatial_context_digest(const Spatialization&)` lands next to
  `spatial_edge_atten` (`content.hpp:266-282`) / `spatial_pan_gains`
  (`content.hpp:284-304`).
- **Write-side key-build sites (`arbc::audio-engine`, L4).**
  [`src/audio_engine/lookahead.cpp:302-312`](../../../src/audio_engine/lookahead.cpp)
  — `contribution_key(const Contribution& c)` builds `BlockKey{c.content,
  d_config.revision, block_index, c.rate}`; `c.spatial` (added by
  `spatial_nested_warm_context`) is the context to digest.
  `native_rerequest_want` (`lookahead.cpp:326-349`, key at `:342`) builds the
  below-rate `BlockKey` — **also folds `c.spatial`**. The root/anchor key
  (`lookahead.cpp:121`, `BlockKey anchor{ObjectId{}, 0, base,
  d_config.sample_rate}`) folds `d_config.spatial` — two monitors of differing
  listener collide on the *root* output slot otherwise.
- **Read-side key-build site (`arbc::compositor`, L4).**
  [`src/compositor/pull_service.cpp:301`](../../../src/compositor/pull_service.cpp)
  — `PullServiceImpl::pull_audio` builds `BlockKey{id, revision,
  audio_block_index(request), request.sample_rate}`, hit test `:303-318`.
  Folds `request.spatial`. The digest computed here must equal the write-side
  digest for the same edge — guaranteed by `spatial_nested_warm_context`'s
  byte-identical-`Spatialization` invariant.

**Existing claims to extend, not duplicate:**

- `12-audio#pull-audio-is-cache-first-single-settle` — the cache-first key
  probe; this task refines the key it probes on.
- `12-audio#lookahead-warms-recursive-contributor-closure`
  (`tests/claims/registry.tsv:80`) and
  `12-audio#spatial-warms-nested-with-pull-context` (added by
  `spatial_nested_warm_context`) — the single-context warm/pull byte-identity
  this task must not regress.

## Constraints / requirements

1. **Flat and leaf-only scenes key byte-identically to today.** When
   `request.spatial == nullopt` (Flat), the digest is `0` and `BlockKey` is
   bit-for-bit the pre-task key. Every existing Flat/leaf golden and the
   shipped nested-of-tones threaded goldens stay byte-identical. The
   `spatial_digest` field is trailing and defaulted (`{0}`) so existing
   `BlockKey{…}` aggregate initializers still compile.
2. **Write key == read key, per edge.** The digest `contribution_key` /
   `native_rerequest_want` fold from `c.spatial` must equal the digest
   `pull_audio` folds from `request.spatial` for the same edge — the
   deliberate write==read byte-identity (`lookahead.cpp:302-308` comment) is
   preserved. This holds because `spatial_nested_warm_context` made the two
   `Spatialization` structs bit-identical; a divergent digest would re-break
   the residency invariant (warm fills key K1, pull probes K2, miss forever).
3. **Distinct contexts ⇒ distinct keys.** Two `Spatialization` structs that
   differ in *any* field (listener coefficients, viewport, `accum_atten`,
   `sub_audible`) yield different digests with overwhelming probability, hence
   different keys and separate cache slots. The digest is over the **whole**
   struct — an over-key, never an under-key (see D3).
4. **Cache stays spatially agnostic (levelization).** `arbc::cache` carries
   `spatial_digest` as an opaque `std::uint64_t`; it never includes
   `content.hpp` and never sees `Spatialization`. The reduction lives in
   `contract` (the helper) and is invoked in `audio-engine` / `compositor`
   (L4). No new levelization edge; the CI levelization gate (doc 17) stays
   green.
5. **Digest is deterministic and byte-exact.** The helper hashes the exact bit
   patterns of the struct's fields (`std::bit_cast<std::uint64_t>` on the
   doubles, `std::uint32_t` on the floats, then `key_hash_combine`) — no
   float tolerance, no locale, no platform-variant formatting. The same
   `Spatialization` always yields the same digest, so goldens are stable
   (doc 16).
6. **No contract behavior change.** `Spatialization`, `AudioRequest`, the
   `render_audio` Spatial branch, and the mix walk are untouched; only a new
   pure digest helper is added to `contract`. No conformance family changes.
7. **Diff coverage ≥ 90%** on changed lines (doc 16 CI gate).

## Acceptance criteria

**Claims-register growth.** Add
`12-audio#block-key-disambiguates-spatial-context` to
`tests/claims/registry.tsv`, enforced by an `// enforces:`-tagged test:

> The block cache key carries a 64-bit spatial-context digest that is zero
> exactly when the request is Flat and otherwise reduces the full
> `Spatialization` (listener, viewport, accumulated attenuation, sub-audible
> threshold) under which the block is rendered. Two distinct spatial contexts
> over the same `(content, revision, block index, rate)` therefore key to
> distinct cache slots, so a Spatial scene embedding one nested composition
> twice at different positions (same time map) drains — for both
> `worker_count==0` and `worker_count>0` — byte-identically to a fresh
> per-embedding `mix_composition` oracle, whereas the pre-digest key served
> one embedding's block for both. The write-side warm digest equals the
> read-side pull digest for each edge, so residency is preserved; a Flat scene
> keys byte-identically to the pre-task key.

`extends 12-audio#spatial-warms-nested-with-pull-context` and
`12-audio#pull-audio-is-cache-first-single-settle`.

**Confirming test → byte-exact regression golden.** In a new
`tests/audio_blockkey_spatial_disambiguation.t.cpp` (registered in
`tests/CMakeLists.txt`), drive a single Spatial monitor over a composition
embedding one nested composition (an off-center child tone) **twice at
different pan positions but the same time map**, through `LookaheadPump` +
`AudioWorkerPool` sharing one `BlockCache`. Assert the primed-ring drain is
byte-identical (`std::memcmp`, no tolerance) to a direct `mix_composition`
oracle that renders each embedding fresh under its own composed listener, for
`worker_count` 0 **and** 4. Oracle tones use the integer-flick `parab_sine`
generator and √-law pan via IEEE `std::sqrt` (never `std::sin`/`sin`/`cos`),
per the sibling goldens. A second case pins the **two-monitor** variant: two
monitors of differing listener over the same content sharing one `BlockCache`
each drain to their own fresh oracle. **This test must be verified to fail on
the pre-fix tree** (documented in the test comment as the confirmed
collision — pre-fix, the second context reads the first's block) and pass
after.

**Behavioral-counter assertions** (wall-clock-free, doc 16). On the
two-embedding scene with the shared cache: (a) the mixer issues **two**
distinct audio dispatches for the nested contributor (one per digest-distinct
slot), asserted via the audio-dispatch counter
(`src/compositor/arbc/compositor/counters.hpp`) — the pre-fix single-slot
path issues one and reuses it; (b) `silence_mixed() == 0` (residency held —
write==read digest); (c) no regression on the shipped Flat/nested/Droste and
single-context Spatial goldens (`src/audio_engine/t/lookahead.t.cpp:356-418`,
`tests/audio_lookahead_recursive.t.cpp`,
`tests/audio_lookahead_spatial_nested.t.cpp`) — a **Flat** scene's `BlockKey`
values and dispatch counts are unchanged.

**Digest-level unit test.** A direct test of `spatial_context_digest`:
identical `Spatialization` ⇒ identical digest; a one-field perturbation in
each of the five fields ⇒ a changed digest; and `BlockKey` equality/hash
consistency (equal keys hash equal; a digest-only difference makes keys
unequal) — pinning Constraints 3 and 5.

**Concurrency / TSan** (concurrency-touching — shared `BlockCache` across
contexts, doc 16). Extend `tests/audio_lookahead_concurrency.t.cpp` with the
two-context scene under concurrent fill + drain over one shared cache: each
`AudioCompletion` settles exactly once, no data race on the cache, drain
byte-identical to the per-context fresh oracle.

**Design-doc delta (lands in the closer's commit, doc 16 same-commit rule).**
`docs/design/12-audio.md:249-254` amended to define the key as
`(content id, revision, block index, rate, spatial-context digest)` with the
zero-when-Flat rule and the levelization note (written with this refinement).
**No doc 00 decision-record bullet** — this refines doc 12's audio key
definition (a doc-12-local amendment, like the predecessor spatial deltas at
doc 12:210-222), not a project-shaping decision (D5).

**No new conformance family.** No new content kind or operator; the contract
conformance suite is unaffected.

**No deferred WBS successor.** This task closes the multi-context collision
completely; it registers **no** successor leaf. The lone out-of-scope items
are the pre-existing HRTF/per-leaf-pan parking-lot entry (human design call)
and `audio.spatial_camera_follow` (already a registered independent leaf) —
neither is spawned by this task.

**WBS gate.** After the closer marks `complete 100`,
`tj3 project.tjp 2>&1 | grep -iE "error|warning"` is silent.

## Decisions

**D1 — Add the digest as an opaque `std::uint64_t` field on `BlockKey`,
computed above the cache — not by embedding `Spatialization`.** `BlockKey`
gains `std::uint64_t spatial_digest{0}`; the `Spatialization`→digest reduction
is a pure `contract` helper invoked at the L4 key-build sites.
*Rationale:* levelization is decisive. `cache` (L3) and `contract` (L3) are
same-level peers; a `cache`→`contract` edge is forbidden (doc 17:32,40-44),
so `BlockKey` cannot hold a `Spatialization`. This is exactly why the existing
`TileMeta` *mirrors* contract's `{achieved_scale, exact}` rather than reusing
the type (`key_shapes.hpp:92-102`). An opaque scalar keeps the cache
engine-agnostic (doc 17:79) and needs no new dependency (doc 10 policy). Both
L4 engines already hold both types, so no new edge is introduced.
*Rejected — embed `std::optional<Spatialization>` in `BlockKey`:* forbidden
same-level `cache`→`contract` edge; also bloats the key and makes the cache
carry a semantically rich type it must not interpret.

**D2 — Digest the *full* `Spatialization` (all five fields), keyed present ⇔
`request.spatial` present.** The helper folds the six `Affine` coefficients,
`viewport_w`, `viewport_h`, `accum_atten`, and `sub_audible`; the digest is
`0` iff there is no context.
*Rationale:* the whole struct determines the rendered block — the listener
drives pan and per-edge attenuation of children, and `accum_atten` /
`sub_audible` drive the sub-audible cull, which *changes content* (a culled
grandchild is silence). Digesting everything guarantees an **over-key** (two
distinct contexts never share a slot); the only cost is that two contexts
which happen to render identically get two slots — pure cache duplication,
explicitly acceptable per doc 12:252 ("caching matters less"). The uniform
present-⇔-present rule means leaf content under Spatial also gets a nonzero
digest (duplicating the Flat leaf block) — the accepted "leaf-under-Spatial
cache duplication" the predecessor named
(`spatial_nested_warm_context.md:316-319`).
*Rejected — digest only a content-affecting subset (e.g. listener only):*
would under-key any content whose output depends on the cull-driving scalars,
re-introducing the collision for the exact multi-context case this task must
close. Correctness beats the marginal cache saving; a subset digest, if ever
worth it, is a later cache-efficiency task, not correctness.

**D3 — Accept 64-bit digest collision as the residual risk, mixed strongly;
do not store the struct for exact comparison.** `BlockKey` equality now
includes a 64-bit hash-derived field, so two truly-distinct contexts colliding
in 64 bits would be treated as equal.
*Rationale:* levelization (D1) forbids storing the struct for exact
member-wise comparison, so a fixed-width digest is the only option, and 64 bits
matches the existing key-hash discipline (`std::size_t` throughout
`key_shapes.hpp`). Collision probability is ~2⁻⁶⁴ per key pair; on collision
the behavior degrades to exactly the pre-fix single-slot case (one context's
block served for another) — a rare correctness degradation, never a crash or
UB — and audio caching "matters less" (doc 12:252). The helper uses the
project's `detail::key_hash_combine` mixer over `std::bit_cast` field bit
patterns for good dispersion.
*Rejected — 128-bit digest:* doubles the key width and diverges from the
established 64-bit key-hash convention for a probability already negligible.
*Rejected — store the `Spatialization` for exact `==`:* forbidden by
levelization (D1).

**D4 — Fold the digest at *every* `BlockKey` construction site under a
spatial context — root/anchor, contribution, native re-request, and pull — not
only the two named in the task note.** The task note names `contribution_key`
and `pull_service.cpp:301`; correctness also requires the root/anchor
(`lookahead.cpp:121`, keyed by `d_config.spatial`) and the below-rate
`native_rerequest_want` (`lookahead.cpp:342`, keyed by `c.spatial`).
*Rationale:* two monitors of differing listener collide on the **root output
block** slot, and a below-rate Spatial nested contributor collides on its
**native re-request** slot, if either omits the digest — the same bug one seam
over. A single shared `spatial_context_digest(request.spatial)` /
`(d_config.spatial)` / `(c.spatial)` call at each site keeps them uniform.
*Rejected — only the two named sites:* would leave the two-monitor and
below-rate variants uncovered; the task's own reproduction ("two monitors …
sharing one BlockCache") exercises the root-block collision.

**D5 — Amend doc 12:249-254 (the key definition); no doc 00 bullet.** The
delta adds the spatial-context digest to the normative key definition, with
the zero-when-Flat rule and the levelization note.
*Rationale:* the task note mandates this delta, and it *does* alter a
normative key shape the predecessors relied on (the spatial-agnostic key), so
doc 16's same-commit rule applies — the edit rides in the closer's commit. It
stays doc-12-local (an audio cache-key refinement), matching the predecessor
spatial deltas (doc 12:210-222), so it warrants **no** doc 00 decision-record
bullet — it is not a project-shaping decision on the order of doc 00's "Audio:
full audio in v1."

**D6 — Reproduce with two embeddings of a *distinct* nested composition (and a
two-monitor variant), not a Droste self-embed.** The confirming golden uses
one nested composition embedded twice at different pan positions but the same
time map (identical `block_index`), plus a two-monitor-shared-cache case.
*Rationale:* the collision needs two *distinct* spatial contexts landing on
the *same* key. Two same-time-map embeddings at different positions do exactly
that (same content/revision/block/rate, different composed listener). A Droste
self-embed varies `block_index`/rate down its zoom chain, so it does *not*
cleanly land two contexts on one key — which is precisely why the shipped
Droste goldens never caught this. The oracle renders each embedding **fresh**
under its own context, so it is the correct reference: both the threaded and
the inline-pull paths collide identically pre-fix, so threaded==inline alone
is *necessary but not sufficient* — the golden compares against the fresh
oracle, and post-fix threaded==inline==oracle all hold.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-08.

- Added `spatial_context_digest(const Spatialization&)` helper in `src/contract/arbc/contract/content.hpp` reducing all five `Spatialization` fields to a 64-bit opaque digest; zero for the Flat/absent case.
- Added trailing `std::uint64_t spatial_digest{0}` to `BlockKey` in `src/cache/arbc/cache/key_shapes.hpp`; `std::hash<BlockKey>` folds it via `detail::key_hash_combine`; `= default` equality picks it up.
- Folded digest at all four `BlockKey` build sites in `src/audio_engine/lookahead.cpp` (root/anchor, `contribution_key`, `native_rerequest_want`) and `src/compositor/pull_service.cpp` (`pull_audio` read key).
- New test `tests/audio_blockkey_spatial_disambiguation.t.cpp`: unit digest perturbation (5-field) + BlockKey eq/hash, byte-exact goldens for two-embedding and two-monitor scenes (workers 0 & 4), behavioral counters (`tasks_submitted==12`, `dispatches==0`, `silence_mixed==0`); claim `12-audio#block-key-disambiguates-spatial-context` registered in `tests/claims/registry.tsv`.
- Extended `tests/audio_lookahead_concurrency.t.cpp` with a two-context TSan case over a shared `BlockCache`.
- Updated three existing lookahead test doubles (`tests/audio_lookahead_{spatial_nested,recursive,concurrency}.t.cpp`) to mirror the new write-side warm key (fold digest in `CachingPull`).
- Amended `docs/design/12-audio.md:249-254` to define the key as `(content id, revision, block index, rate, spatial-context digest)` with zero-when-Flat rule and levelization note.
- `tests/CMakeLists.txt` updated to register the new test.
