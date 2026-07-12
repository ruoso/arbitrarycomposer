# runtime.pull_identity_disjoint_ids — Allocate synthesized pull identities from a namespace disjoint from every model ObjectId

## TaskJuggler entry

[`tasks/65-runtime.tji:132-137`](../../65-runtime.tji)

> `task pull_identity_disjoint_ids "Allocate synthesized pull identities from a
> namespace disjoint from every model ObjectId"` — effort `1d`,
> `depends !operator_model_damage_routing`, milestone `m9_release`.
>
> "build_pull_identity_map seeds next = max(layer-root ids) + 1
> (pull_identity.cpp:45), so a synthesized id can collide with the model id of a
> contents-table content that is not a layer. Consequence: invalidate_damage on a
> model id can drop an unrelated content's tiles (sound over-approximation but a
> silent cache-hygiene bug), and the disjointness claim in
> interactive_operator_identity.t.cpp:5 is stronger than what the code guarantees.
> Seed above the model's maximum allocated ObjectId (or tag a high bit) and tighten
> claim registry:177's wording to match. Source-of-debt:
> tasks/refinements/runtime/operator_model_damage_routing.md Constraint 1 /
> Decision 2. Docs 02/13."

## Effort estimate

**1d.** The code change is small and local: a reserved-bit constant plus two
predicates in `arbc::base` (`src/base/arbc/base/ids.hpp`, ~10 lines), one changed
seed line and its comment in `src/runtime/pull_identity.cpp` (the `max_seeded`
tracking loop goes away entirely), a defensive assertion in `Model::allocate_id`,
and comment repairs at the three sites that currently over- or under-state the
guarantee. The day is spent on the *tests*, which are the deliverable: a
regression document whose shape provokes the collision under the old seed, a
cache-hygiene test proving the cross-namespace eviction is gone, a
no-synthesized-id-escapes serialization round-trip, and re-running the byte-exact
golden suite unchanged (identities move, pixels must not). Plus the doc 14 / doc 00
deltas and the registry:177 rewording. No new component, no new seam, no
concurrency surface.

## Inherited dependencies

**Settled:**

- `runtime.operator_input_cache_identity` — minted the synthesized-pull-identity
  scheme (`src/runtime/pull_identity.{hpp,cpp}`), its pointer-keyed map, its
  transitive `inputs()` frontier walk, and the `1 + max(seeded layer id)` seed this
  task replaces. Its **Decision 4** and **Constraint 4** are the exact statements
  being amended (see Decisions below); its Constraints 2 (sharing preserved — one
  identity per `Content*`, keyed by pointer) and 3 (cross-frame stability — the same
  child yields the same id on every frame of one render sequence) are **preserved
  verbatim** and this task must not weaken them.
- `runtime.interactive_pull_wiring` — put the identity map on the interactive path,
  memoized per revision (`interactive.cpp:49`, `refresh_identity_memo`), and built
  the inverse `d_content_by_id` map. Its Constraint 6 asserts the synthesized ids
  are "disjoint from every model id by construction" — the over-strong claim this
  task *makes true* rather than retracts.
- `runtime.operator_model_damage_routing` (the direct predecessor) — routed model
  damage through `route_operator_damage` before `map_damage_to_device` /
  `invalidate_damage` (`interactive.cpp:218`). Its **Constraint 1** and **Decision 2**
  are the source-of-debt: they established that a damaged model `ObjectId` must be
  resolved through the `ContentResolver` and *never* through `d_content_by_id`,
  because the inverse map is keyed in the pull-id space where a model id "can alias a
  *different* content's synthetic id". That decision **stands after this task** (see
  Decision 4) — this task removes the aliasing hazard, not the reason for the seam.

**Pending (deliberately downstream):** none. Nothing in the WBS is waiting on this;
it is a correctness/hygiene repair of an existing shipped seam, and it is the last
`runtime` leaf that touches the identity scheme.

## What this task is

`build_pull_identity_map` assigns each operator input child that the model never
named a *synthesized* `ObjectId`, so two same-stability inputs of one operator cache
under different `TileKey`s. Today those synthesized values are drawn by counting up
from `1 + max(layer-root ObjectId)` (`src/runtime/pull_identity.cpp:45`). That is
disjoint from the **layer roots** — and from nothing else. A document's `contents`
table can hold a content that is *not* a layer (a `$ref` target consumed only as an
operator input, doc 13:181-184); its model `ObjectId` was allocated from the same
monotonic counter and is under no obligation to exceed the max layer-root id. So a
synthesized child id and a real model id can be the same 64-bit number.

This task makes the two namespaces **disjoint by construction**: reserve the top bit
of the `ObjectId` space. The model allocator (`Model::allocate_id`, counting up from
1) issues only ids with bit 63 clear; `build_pull_identity_map` mints synthesized ids
with bit 63 set. No arithmetic, no counter read, no ordering assumption — the halves
cannot meet. The registry:177 wording, the `pull_identity.hpp` header comment, the
`interactive.cpp` alias warning, and the `interactive_operator_identity.t.cpp:4-6`
comment all get tightened to state the guarantee the code now actually provides, and
doc 14's § Identity gains the normative sentence that makes the split part of the
constitution rather than a driver-local trick.

## Why it needs to be done

1. **It is a live cache-hygiene bug.** `invalidate_damage` (`damage_planning.cpp:84-100`)
   funnels into `cache::invalidate_content` / `invalidate_region`
   (`src/cache/arbc/cache/invalidation.hpp:50-64`), whose predicates match on
   `key.content == content` **and nothing else** — revision-agnostic, rung-agnostic,
   time-agnostic. So invalidating the model id `3` of a `contents`-table content
   also drops every tile an operator input child happens to hold under synthesized
   id `3`, at every revision, wholesale. It is a *sound* over-approximation (a
   spurious drop, never a stale read — tile keys carry the revision) which is
   precisely why it has been silent: the only symptom is unexplained re-render work
   in exactly the documents where operators and shared `$ref` content coexist, i.e.
   the ones doc 13 is for.
2. **The "seed above the current max" fix would not actually fix it.** Seeding above
   the model's *present* maximum allocated id only pushes the collision into the
   future: the model keeps allocating, a later `allocate_id()` hands out the same
   64-bit number the driver already synthesized, and because the invalidation
   predicates ignore the revision field, tiles cached under the *old* revision's
   synthetic id are still reachable and still get dropped by damage on the *new*
   model object. Disjointness has to be structural, not a high-water mark. (See
   Decision 1.)
3. **Three artifacts currently assert a guarantee the code does not make.**
   `interactive_operator_identity.t.cpp:4-6` says synthesized ids are "disjoint from
   every model id by construction" (false today); `pull_identity.hpp:26-30` says
   "distinct from every seeded layer id" (true but weaker than the consumer
   comments assume); `interactive.cpp:98-104` documents the alias hazard as a known
   defect naming this very task. Doc 14:77-84 promises `ObjectId` is
   "document-unique" — which the synthesized ids violate. One of these has to
   move, and the doc is the constitution, so the code moves.
4. **It unblocks nothing but it de-risks everything downstream that keys on
   `ObjectId`.** Every future consumer of an identity — audio `BlockKey`
   (`key_shapes.hpp:95+`, same `ObjectId content` field), a future selection or
   journal surface, an editor addressing a content — inherits either a real
   invariant or a footgun. This is the last cheap moment to make it an invariant.

## Inputs / context

### Design docs (normative)

- **doc 14:75-95 (§ Identity)** — `ObjectId` is 64-bit, document-unique, assigned at
  creation; "the address used by the journal, by editor selection, by damage
  records, and by the `contents` sharing table when serialized". **This task lands a
  delta here** (see Decisions / Design-doc delta below): the top-bit split and the
  synthesized-identity namespace become normative.
- **doc 02:89** — the tile-cache key: "Key: `(content id, revision, scale rung, tile
  coords)`."
- **doc 02:94-95** — "Damage invalidates by `(content id, region)` across all rungs;
  revision bumps invalidate wholesale by making old keys unreachable." Note what this
  sentence *does not* say: it does not scope invalidation by revision. Invalidation
  is by content id, full stop — which is why a colliding id is destructive rather
  than merely confusing.
- **doc 13:143-149 (§ Caching and scheduling)** — "input tiles cache under the
  input's identity (shared by every consumer), operator output under the operator's."
  Doc 13 mandates *that* each input has an identity and that a shared input shares
  one; it never says where an unnamed child's identity comes from. That gap is what
  `pull_identity.cpp` fills and what this task makes safe.
- **doc 13:181-184** — the `contents` table + `{"$ref": "id"}`: the mechanism that
  creates non-layer, model-id-bearing contents in the first place, i.e. the other
  half of the collision.
- **doc 16:8-26 (claims register), 16:28-90 (test taxonomy), 16:112-118 (≥90% diff
  coverage)** — governs the acceptance criteria below.
- **doc 17:48 (`arbc::base`, level 0 — owns `ObjectId`), :52 (`arbc::model`, level 2),
  :54 (`arbc::cache`, level 3), :60 (`arbc::runtime`, level 5)** — the levelization
  this must respect.

### Source seams

| What | Where |
| --- | --- |
| `ObjectId` — plain `uint64_t` wrapper, zero-is-invalid, **no reserved bits today** | `src/base/arbc/base/ids.hpp:9-25` |
| The synthesized-id seed (the bug) | `src/runtime/pull_identity.cpp:45` (`std::uint64_t next = max_seeded + 1;`) |
| `max_seeded` tracking loop (becomes dead) | `src/runtime/pull_identity.cpp:22-33` |
| The child walk that consumes `next` | `src/runtime/pull_identity.cpp:48-61` (`ids->emplace(child, ObjectId{next++})`, line 58) |
| The over-strong / now-repairable header comment | `src/runtime/arbc/runtime/pull_identity.hpp:26-30` |
| Model id allocator — monotonic, from 1, atomic | `src/model/model.cpp:743` (`ObjectId Model::allocate_id()`), counter at `src/model/arbc/model/model.hpp:572` (`std::atomic<std::uint64_t> d_next_id{1}`) |
| Recovery reseed (must also stay in the model half) | `src/model/model.cpp:642` (`d_next_id.store(recovered.max_id + 1, …)`), `model.hpp:545` |
| Invalidation predicates — match on `key.content` **only** | `src/cache/arbc/cache/invalidation.hpp:50-56`, `:62-64` |
| `invalidate_damage` — the driver-facing entry | `src/compositor/damage_planning.cpp:84-100`, decl `arbc/compositor/damage_planning.hpp:92` |
| `TileKey` (`ObjectId content; revision; rung; coord; achieved_time`) | `src/cache/arbc/cache/key_shapes.hpp:64-77`; `TileCache` alias at `:142` |
| `BlockKey` — same `ObjectId content` field (audio; not touched by `invalidate_damage`, but shares the namespace) | `src/cache/arbc/cache/key_shapes.hpp:95+` |
| The in-code TODO naming this task, and the alias hazard | `src/runtime/interactive.cpp:98-104` |
| Interactive per-revision memo + inverse map | `src/runtime/interactive.cpp:49-55` (`refresh_identity_memo`), invalidation call at `:218` |
| The comment that over-claims disjointness | `tests/interactive_operator_identity.t.cpp:4-6` |
| The claim row whose wording is scoped to layer roots | `tests/claims/registry.tsv:177` (`13-effects-as-operators#operator-input-children-have-distinct-cache-identity`) |
| Existing unit tests over the map | `src/runtime/t/pull_identity.t.cpp:61,82,105` |
| Other `make_pull_identity_of` consumers (must keep working unchanged) | `src/runtime/offline_sequence.cpp:92`, `src/runtime/export_monitor.cpp:115` |

### Predecessor decisions this task inherits verbatim

- **`operator_input_cache_identity` Constraint 2 — sharing preserved.** One identity
  per `Content*`, keyed by pointer, emplaced once; a child reached from two parents
  keys under one id. Unchanged.
- **`operator_input_cache_identity` Constraint 3 — cross-frame stability.** The map is
  built once per render sequence over the immutable pinned graph, and the walk order
  is deterministic (layer order, then a LIFO frontier), so the same child yields the
  same id every frame. Unchanged — the new scheme changes only the *base* of the
  counter, not its determinism.
- **`interactive_pull_wiring` Decision 2 — memoize the map on `state.revision()`.**
  Unchanged; the memo remains exact.
- **`operator_model_damage_routing` Decision 2 — resolve a damaged model `ObjectId`
  through the `ContentResolver`, never through `d_content_by_id`.** Unchanged and
  still correct after this task (Decision 4 below explains why the seam survives the
  removal of its original justification).

## Constraints / requirements

1. **Disjointness must be structural, not arithmetic.** After this task, "a
   synthesized pull identity is never equal to any model `ObjectId`" must hold
   *without* any assumption about allocation order, about the model's current
   high-water mark, or about what the model allocates after the map is built. A
   fix that reads the model's counter at map-build time does not satisfy this
   (rationale in Decision 1). The property must be checkable from a single id in
   isolation: `synthetic(id)` is a pure predicate on the bit pattern.

2. **The model allocator must never issue a reserved id.** `Model::allocate_id`
   counts up from 1 and `Model::recover` reseeds from `max_id + 1`; both must be
   provably confined to the model half. Add a debug assertion at the allocator (and
   at the recovery reseed) that the produced id has the reserved bit clear. Reaching
   bit 63 requires 2^63 allocations, so the assertion is a tripwire against a future
   change that seeds the counter from untrusted data (e.g. a recovered `max_id`
   read from a corrupt workspace file) — not against ordinary exhaustion.

3. **Zero remains the only invalid id.** `ObjectId::valid()` is `value != 0`
   (`ids.hpp:13`) and `pull_identity_of` falls back to `ObjectId{}` for an unmapped
   `Content*` (`pull_identity.cpp:66-72`). The synthesized counter therefore starts
   at 1 *within* the reserved half — the first synthesized id is
   `kSyntheticIdBit | 1`, never the bare bit — so no synthesized id can be confused
   with the fallback, and `valid()` needs no change.

4. **Sharing, injectivity, and cross-frame stability survive unchanged.**
   (`operator_input_cache_identity` Constraints 2 and 3.) The walk keeps its pointer
   key, its `walked` guard, its deterministic order, and its one-emplace-per-child
   rule; only the counter's base moves. A child that *is* also a layer root keeps its
   model id (`pull_identity.cpp:57` — "A child already carrying a layer-root identity
   … keeps it"), which is correct and stays: that content *is* a model object and
   should key under its real identity.

5. **No synthesized id may escape into the model, the journal, or the file format.**
   Doc 14's delta says synthesized ids are render-time state. The serializer assigns
   `contents`-table ids from the model, not from the pull map, so this holds today by
   construction — but it must be *tested* (A4), because the round-trip is the only
   place the property could silently break.

6. **Pixels must not move.** Identities are cache keys. Changing them changes which
   `TileKey` a tile lands under and nothing else. Every byte-exact golden in the
   suite (nested, fade, crossfade, external-ref, async-load) must pass **unchanged,
   with no re-baselining**. A golden that needs a new baseline is a bug in this
   task, not a new baseline (A6).

7. **No new levelization edge.** `ObjectId` lives in `arbc::base` (level 0, doc
   17:48) — every component already sees it, so the reserved-bit constant and the
   `synthetic()` predicate go there and are visible to `model` (L2), `cache` (L3),
   and `runtime` (L5) with no new dependency. Nothing in this task makes `runtime`
   reach into `Model`'s private counter, and nothing makes `cache` (which cannot see
   `model`) aware of the split. `scripts/check_levels.py` must stay green.

8. **The comments that currently misstate the guarantee are part of the deliverable.**
   `pull_identity.hpp:26-30`, `interactive.cpp:98-104`, and
   `interactive_operator_identity.t.cpp:4-6` each describe the old guarantee (two of
   them wrongly). Leaving a stale "can ALIAS" warning in `interactive.cpp` after the
   alias is impossible is a documentation defect that will mislead the next reader
   into re-deriving a hazard that no longer exists.

## Acceptance criteria

Testable checks that pin the behavior. The design-doc delta (doc 14 § Identity, doc
00 decision record) rides in the same commit, per doc 16's same-commit rule.

**A1 — Synthesized ids are drawn from the reserved half, and the predicate is exact.**
Unit-test `synthetic()` / the reserved-bit constant in `src/base/t/ids.t.cpp` over the
boundary values: `ObjectId{}` (invalid, not synthetic), `ObjectId{1}` (model half),
`ObjectId{(1ull<<63) - 1}` (largest model id — not synthetic), `ObjectId{1ull<<63}`
and `ObjectId{(1ull<<63) | 1}` (synthetic), `ObjectId{~0ull}` (synthetic). Extend
`src/runtime/t/pull_identity.t.cpp`: for a document with operators and inline input
children, **every** value in the built `PullIdentityMap` is either a layer root's real
model id (bit clear) or has the reserved bit set — no third case.

**A2 — The collision regression (the test that fails on the old seed).** New case in
`src/runtime/t/pull_identity.t.cpp`, over a document shaped to provoke the old bug:
layer roots allocated ids `1` and `2`; a `contents`-table content `C` (a `$ref`
target consumed only as an operator input, never a layer) allocated id `3`; and an
operator layer with at least two *inline* input children. Under the old seed
(`1 + max(layer-root id)` = `3`) the first inline child synthesizes id `3` — equal to
`C`'s model id. Assert the new map contains **no** identity equal to any
`ObjectId` allocated by the model in that document (collect the document's allocated
ids and intersect with the map's values — the intersection must be exactly the layer
roots that *are* model objects). This test is the executable statement of Constraint
1; it must fail if the seed is ever reverted to a high-water mark.

**A3 — Cross-namespace cache eviction is gone (the behavioral consequence).** New
test `tests/pull_identity_disjoint_ids.t.cpp` over the A2 document shape: seed a
`TileCache` with tiles under `C`'s model id *and* under the inline child's
synthesized id (both reachable via the real render path — render a frame, then read
the cache), then damage `C` and run the driver's `invalidate_damage`
(`interactive.cpp:218`). Assert with a **behavioral counter** (the `remove_if` /
eviction count, or the `TileCache` entry-count delta — never wall-clock, doc 16 tier
4) that exactly `C`'s entries are dropped and the inline child's entries survive.
Under the old seed this test evicts both.
*enforces:* `14-data-model-and-editing#synthesized-identities-never-collide-with-model-ids`
(new registry row, see A7) and, as a second tag on the existing row,
`02-architecture#damage-invalidates-by-content-region-across-rungs`
(`registry.tsv:102`).

**A4 — No synthesized id escapes.** Round-trip a document containing operators with
inline input children through the serializer (reuse the harness in the existing
`document_serialize` / `nested_codec_golden` tests): assert **no** `ObjectId` in the
written document's `contents` table, and no id in the reloaded `Model`, has the
reserved bit set. Pins Constraint 5 and doc 14's "never serialized" sentence.

**A5 — The model allocator stays in its half.** Unit test in `src/model/t/`: a
`Model` that allocates N ids yields only bit-63-clear ids (`!synthetic(id)` for
every one). The `ARBC_ASSERT` tripwire in `allocate_id` / the recovery reseed guards
the 2^63 case, which is unreachable in a test; mark it `GCOV_EXCL` with the
justification "unreachable without 2^63 allocations; a tripwire against a corrupt
recovered `max_id`" (doc 16:112-118 requires the justification).

**A6 — Byte-exact goldens unchanged, with no re-baselining.** The full golden suite
(`nested_codec_golden`, `nested_external_ref_golden`, `async_external_load_golden`,
the fade/crossfade goldens) passes **against the existing baselines**. Identities are
cache keys; a moved identity that moves a pixel means the cache is serving results
keyed by something it shouldn't be. State in the commit that zero golden files
changed — that is the positive signal, not an absence of news.

**A7 — Claims register: one new row, one reworded row, one second tag.**
- **New row** in `tests/claims/registry.tsv`:
  `14-data-model-and-editing#synthesized-identities-never-collide-with-model-ids` —
  *"Runtime-synthesized identities (an operator's inline input children) are drawn
  from a reserved half of the 64-bit ObjectId space that the model allocator never
  issues, so a synthesized cache identity never equals any model ObjectId in the
  document -- damage or cache invalidation naming a model id cannot evict a
  synthesized id's tiles, and no synthesized id is ever journaled or serialized."*
  Enforced by A2/A3/A4 (`enforces:` tags in `src/runtime/t/pull_identity.t.cpp` and
  `tests/pull_identity_disjoint_ids.t.cpp`). This row is legitimate because the doc
  14 delta lands the normative sentence it quotes — it is not an implementation-axis
  claim (the trap `interactive_pull_wiring` Decision 5 warns against).
- **Reword** `registry.tsv:177`
  (`13-effects-as-operators#operator-input-children-have-distinct-cache-identity`):
  "distinct from every **layer-root** identity and from every sibling child" →
  "distinct from **every model ObjectId in the document** (drawn from a reserved
  identity namespace the model allocator never issues) and from every sibling child".
  No new row, no change to its enforcing tests — the wording now matches what the
  code guarantees rather than the weaker property it settled for.
- **Second `enforces:` tag** on `registry.tsv:102` from A3, per the established
  "second tag, no new row" pattern.
`scripts/check_claims.py` must be green in both directions (every row enforced;
every tag registered).

**A8 — The stale comments are repaired.** `pull_identity.hpp:26-30` states the new
guarantee (reserved namespace, disjoint from every model id, cites doc 14 § Identity);
`interactive.cpp:98-104` drops the "at worst an ALIAS" warning and the
`runtime.pull_identity_disjoint_ids` TODO, keeping only the true residual statement
(a model id looked up in `d_content_by_id` is a *guaranteed miss*, never an alias —
which is exactly why `operator_model_damage_routing` Decision 2's `ContentResolver`
seam is still the right one; see Decision 4); `tests/interactive_operator_identity.t.cpp:4-6`
keeps its "disjoint from every model id by construction" sentence, which is now true,
and cites doc 14 rather than `pull_identity.hpp`.

**A9 — Gates.** `scripts/gate` green; `scripts/check_levels.py` green (no new
levelization edge, Constraint 7); `scripts/check_claims.py` green; CI diff coverage
≥90% on changed lines (the tests above are part of this task); the WBS gate
`tj3 project.tjp 2>&1 | grep -iE "error|warning"` silent after the closer adds
`complete 100` and the `Refinement:` note back-link.

**A10 — Not applicable, stated for the record.** No contract conformance-suite run
(this task ships no new content kind or operator — it changes how an existing driver
names cache entries). No TSan/stress suite (the map is built on the driver thread
over an immutable pinned graph and shared read-only with workers — unchanged by this
task, `operator_input_cache_identity` Constraint 7; the one new atomic touch, the
`allocate_id` assertion, reads the value the `fetch_add` already returned). No new
golden baseline (A6 is the opposite).

### Deferred follow-up (closer registers in WBS)

**None.** The reserved-bit split is complete in one step: after it, the disjointness
property holds for every consumer of `ObjectId` (`TileKey`, `BlockKey`, damage
records) without any further per-consumer work, and no other seam in the WBS needs to
learn about the split. There is nothing left for a successor task.

## Decisions

**Decision 1 — Reserve the top bit of the `ObjectId` space (bit 63) as the
synthesized-identity namespace; the model allocator issues only bit-63-clear ids.**
`build_pull_identity_map` mints `ObjectId{kSyntheticIdBit | n}` for `n = 1, 2, 3, …`
in walk order; the `max_seeded` tracking loop disappears. Disjointness becomes a
property of a single id's bit pattern, checkable in isolation, independent of
allocation order and of anything the model does after the map is built.

*Alternative rejected — "seed above the model's maximum allocated `ObjectId`"* (the
option the ticket note lists first). It does not actually fix the bug. The seed would
be a high-water mark taken at map-build time; the model keeps allocating, and a later
`allocate_id()` will hand out a number the driver already synthesized. Because the
cache's invalidation predicates match on `key.content` alone and *ignore the revision
field* (`invalidation.hpp:50-64`), the tiles cached under the old revision's
synthetic id remain reachable and are still dropped by damage naming the new model
object. So the collision returns as soon as the document grows — the same bug, one
edit later, and harder to see. It also costs more: `Model` has no public
"peek next id" accessor (only the destructive `allocate_id()`, `model.cpp:743`), and
`Document` deliberately refuses to expose `Model&` (`document.hpp:174-185`, the
`friend struct` attorney pattern) — so this option requires a new `Model` accessor
*and* a new attorney seam, to buy a weaker guarantee. Structural beats arithmetic.

*Alternative rejected — hash `(parent_id, slot)` into a synthesized id.* Already
rejected by `operator_input_cache_identity` Decision 4 and still rejected: it is not
injective (birthday collisions in the same 64-bit space, now *inside* the namespace
we are trying to make clean), and it breaks the pointer-keyed sharing rule
(Constraint 4) — a child reached from two parents would get two ids.

*Note on the prior rejection.* `operator_input_cache_identity` Decision 4 explicitly
rejected "reserving a high bit of `ObjectId`" in favor of `1 + max(seeded layer id)`,
with the rationale that the latter is "provably disjoint from every seeded layer id"
— which it is. That decision was correct **for the property it was scoped to**
(Constraint 4 of that refinement: "must not collide with any seeded **layer** id used
in the same render"). The property turned out to be too weak: the `contents` table
(doc 13:181-184) puts model ids in the graph that are *not* layer roots. This task
strengthens the property, and with the stronger property in hand the high-bit option
is the one that satisfies it. The earlier decision is superseded, not contradicted.

**Decision 2 — The constant and the predicate live in `arbc::base`
(`src/base/arbc/base/ids.hpp`), next to `ObjectId` itself.** Add
`inline constexpr std::uint64_t kSyntheticIdBit = 1ull << 63;` and
`constexpr bool synthetic(ObjectId id) { return (id.value & kSyntheticIdBit) != 0; }`
(plus a `constexpr ObjectId synthetic_id(std::uint64_t n)` minting helper). The split
is a property of the *id type*, not of the runtime that happens to exploit it: the
model asserts against it (L2), the tests check it, and any future consumer that keys
on `ObjectId` can ask. `arbc::base` is level 0 (doc 17:48) so this introduces no
edge anywhere (Constraint 7).

*Alternative rejected:* keeping the constant private to `src/runtime/pull_identity.cpp`.
Then `Model::allocate_id`'s tripwire assertion (Constraint 2) could not see it, and
the invariant would be enforced on only one side of a two-sided contract — the exact
shape of the bug we are fixing.

**Decision 3 — Do not change `ObjectId::valid()`; start the synthesized counter at 1
within the reserved half.** The first synthesized id is `kSyntheticIdBit | 1`, so no
synthesized id is `ObjectId{}` and `valid()` keeps its single, simple rule (zero is
invalid, `ids.hpp:13`). The bare `kSyntheticIdBit` value is never minted — it is not
special, just unused, which keeps the minting helper's arithmetic trivially
injective.

*Alternative rejected:* treating "synthetic" as a *third* validity state
(`valid()` false for reserved ids). It would silently change the meaning of every
existing `valid()` check on a pull identity, including inside the cache, and buys
nothing — the code never wants to reject a synthesized id, it wants to distinguish
one.

**Decision 4 — `operator_model_damage_routing` Decision 2 stands: model damage keeps
resolving through the `ContentResolver`, not through `d_content_by_id`.** Disjointness
removes the *aliasing* half of that decision's justification (a model id can no longer
collide with a synthetic one), but not the *miss* half: a `contents`-table content
consumed only as an operator input is still keyed in the pull map under a synthesized
id, so `d_content_by_id.find(model_id)` still finds nothing. The `ContentResolver` is
still the model id space's own inverse and still the correct seam. What changes is
only the comment: a guaranteed miss is a benign lookup failure the caller can handle,
where an alias was a silent mis-route (`interactive.cpp:98-104` is rewritten
accordingly, A8).

*Alternative rejected:* now that ids are disjoint, double-key the pull map so
non-layer inputs appear under *both* their model id and their synthesized id, making
`d_content_by_id` serve model lookups too. Disjointness makes this *safe* where it
previously was not, but it is still wrong: it muddles the two id spaces the map exists
to separate, gives one content two cache identities (breaking the "shared by every
consumer" invariant doc 13:147-149 requires, since a tile cached under the model id is
a different `TileKey` from the same tile under the synthetic id), and duplicates work
the `ContentResolver` already does correctly in one call. Same conclusion as
`operator_model_damage_routing` Decision 2, now for a different reason.

**Decision 5 — Design-doc delta: doc 14 § Identity + a doc 00 decision-record bullet.**
The top-bit split is a global property of `ObjectId`, the type doc 14 owns
("64-bit, document-unique", 14:77-79) — and the *current* code violates that
document-uniqueness promise, so this is not a new invention but the restoration of an
existing doc promise plus the mechanism that makes it hold. Doc 14's § Identity gains
the normative paragraph (the halves, what the model may issue, what the runtime may
mint, and "never journaled, never serialized"); doc 00 gains an **Object identity
namespaces** decision bullet because the split is visible to every component that
keys on an id, which makes it project-shaping. Both edits are made by this refinement
and ride in the closer's commit (doc 16's same-commit rule).

*Alternative rejected:* documenting the split only in `pull_identity.hpp` and the
refinement, as `operator_input_cache_identity` Decision 5 did ("No design-doc delta")
for the original scheme. That was defensible when the scheme was a driver-local cache
trick disjoint only from layer roots. It is not defensible for a reserved bit in the
document's identity type that the *model* must assert against: a future contributor
reading doc 14 must not be able to conclude that all 2^64 ids are the model's to
allocate.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-11.

- Reserved bit 63 of `ObjectId` as the synthesized-identity namespace; added `kSyntheticIdBit`, `synthetic()` predicate, and `synthetic_id()` minting helper to `src/base/arbc/base/ids.hpp` (Decision 2, Constraint 1).
- Replaced the `max_seeded` tracking loop in `src/runtime/pull_identity.cpp` with a counter seeded at `kSyntheticIdBit | 1`; removed dead loop (Decision 1, Constraint 3).
- Added `assert(!synthetic(id))` tripwires (using `<cassert>`) to `Model::allocate_id` and the recovery reseed in `src/model/model.cpp` (Constraint 2; A5 marks GCOV_EXCL with justification).
- Repaired stale comments in `src/runtime/arbc/runtime/pull_identity.hpp`, `src/runtime/interactive.cpp`, and `tests/interactive_operator_identity.t.cpp` (A8, Constraint 8).
- Unit tests: boundary-value `synthetic()`/`synthetic_id()` suite in `src/base/t/ids.t.cpp` (A1); collision regression + bit-pattern sweep in `src/runtime/t/pull_identity.t.cpp` (A1/A2); model-allocator half in `src/model/t/model.t.cpp` (A5).
- Behavioral cache test `tests/pull_identity_disjoint_ids.t.cpp`: `remove_if`-based tile counter proves damaging the `$ref` content leaves the inline child's 8 tiles resident (A3; cross-namespace eviction confirmed gone).
- Serialization round-trip in `src/runtime/t/pull_identity.t.cpp` asserts no reserved-bit id appears in the written or reloaded model (A4, Constraint 5).
- Claims register: new row `14-data-model-and-editing#synthesized-identities-never-collide-with-model-ids`, reworded row 177, second `enforces:` tag on row 102 in `tests/claims/registry.tsv` (A7).
- Design-doc deltas: `docs/design/14-data-model-and-editing.md` § Identity gains normative top-bit paragraph; `docs/design/00-overview.md` gains **Object identity namespaces** decision bullet (Decision 5).
- A6 confirmed: zero golden files changed; 892/892 tests green against existing baselines.
