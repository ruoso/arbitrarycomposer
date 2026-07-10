# Refinement — `runtime.operator_input_cache_identity`

## TaskJuggler entry

`tasks/65-runtime.tji:67-72`:

```
task operator_input_cache_identity "Distinct cache identity for operator input children" {
  effort 1d
  allocate team
  depends operators.crossfade_runtime_binding
  note "Give operator input children distinct, stable cache identities in the offline/export PullConfig (id_of) so multi-input operators render byte-exact; fixes ObjectId collision where both same-stability inputs key on ObjectId{} and receive identical tiles. Carries deferred crossfade visual byte-exact + parallel/TSan acceptance from operators.crossfade_runtime_binding. Source: tasks/refinements/operators/crossfade_runtime_binding.md. Doc 13/17."
}
```

This task is the deferred follow-up minted by `operators.crossfade_runtime_binding`
(`tasks/refinements/operators/crossfade_runtime_binding.md:420`): it inherits that
task's *visual* byte-exact and parallel/TSan acceptance, which crossfade binding
could not land because the offline driver's `id_of` collapses operator-input
children to `ObjectId{}`. It belongs to milestone `m9_release`
(`tasks/99-milestones.tji`), the same milestone that gathers the runtime-binding
tasks — the reference operators are not shippable end-to-end until a **multi-input**
operator renders byte-exact through the real offline driver, which is this fix.

## Effort estimate

`effort 1d`, `allocate team`. One production seam (the driver-side reverse
identity map), duplicated across two driver TUs, plus its tests. No new kernel,
no new math, no compositor/core change, no design-doc delta. The bulk of the day
is the carried-in acceptance surface: the visual byte-exact goldens (inline +
parallel) and the TSan lane that `crossfade_runtime_binding` deferred here.

## Inherited dependencies

**Settled predecessors this task builds on (all `complete 100`):**

- `operators.crossfade_runtime_binding` (`tasks/50-operators.tji`, refinement
  `tasks/refinements/operators/crossfade_runtime_binding.md`, **Done 2026-07-10**).
  Shipped the runtime binding that attaches a live `PullServiceImpl`/`Backend` to
  `CrossfadeContent` at instantiation, and closed audio byte-exact through the
  export monitor — but **explicitly deferred the visual byte-exact offline
  acceptance (inline + parallel/TSan interior dissolve) to this task**
  (`crossfade_runtime_binding.md:346-351, 418-420`), because the offline/export
  `id_of` gives `ObjectId{}` to operator-input children, colliding two
  same-stability inputs onto one cache key.
- `operators.fade_runtime_binding` (**Done 2026-07-09**) and
  `operators.crossfade` (**Done 2026-07-09**) — established that a bound operator
  pulls **each** input only through the injected `PullService`
  (`crossfade.md` Constraint 2), which is precisely the pull path whose cache key
  this task corrects.
- `compositor.pull_service` (refinement
  `tasks/refinements/compositor/pull_service.md`) — landed `PullConfig`,
  `PullServiceImpl`, the two-level cache-identity model ("input tiles cache under
  the input's identity (shared by every consumer), operator output under the
  operator's", `pull_service.md:242-245`), and Decision 6
  (`pull_service.md:509-515`: an operator input's tiles key by `aggregate_revision`;
  a leaf input keys on its own revision). `PullConfig::id_of` is the seam this task
  populates; the key shape and revision-folding are frozen and **not reopened**.
- `compositor.operator_pull_delivers_target` /
  `compositor.pull_multi_tile_region` (recent commits `5dedbc8`, `90938a3`) —
  the delivery + multi-tile-coverage path that surfaces the collision: every
  covering tile keys under the same `id`, so a wrong `id` mis-serves the whole
  region. Neither commit touched `id` derivation.

**Settled architecture this task must honor (not a dependency edge, a decision):**

- `compositor.operator_graph` Decision (`tasks/refinements/compositor/operator_graph.md:420-427`):
  **no persistent `Content*→ObjectId` reverse map in the compositor core** — "new
  persistent state in a pure library" is rejected; the resolver maps `ObjectId→Content*`
  *forward* only. The offline/export drivers already build an **ephemeral,
  per-render** `Content*→ObjectId` map inline to feed `id_of`
  (`offline_sequence.cpp:87-92`); this task **extends that existing driver-owned
  ephemeral map**, adding no state to the compositor.

**Pending — nothing.** All predecessors are `complete 100`. This task introduces
no new dependency and (per Decisions) no design-doc delta.

## What this task is

The offline and export render drivers build a reverse `Content* → ObjectId` map
to supply `PullConfig::id_of` — the function `PullServiceImpl::pull` /
`pull_audio` call to key each pulled input's cache tiles under "the input's
identity" (doc 13's caching contract). Today both drivers seed that map **only
from `DocRoot::for_each_layer`** (`offline_sequence.cpp:88-92`,
`export_monitor.cpp:114-118`), which visits layer-root contents only. Operator
**input children** — a crossfade's `from`/`to`, a fade's input — are demoted,
`ObjectId`-less nodes reached solely through `Content::inputs()`
(`document_serialize.cpp:299-312`, "a child needs no ObjectId"; they are never
layer roots). They miss the map, so `id_of` returns the default `ObjectId{}`
(value 0, the reserved sentinel — `src/base/arbc/base/ids.hpp:9-10`) for **every**
such child. Two same-stability sibling inputs of one operator therefore fold into
byte-identical keys `TileKey{ ObjectId{}, revision, rung, coord, achieved_time }`
(`pull_service.cpp:175, :217`) and `BlockKey{ ObjectId{}, ... }`
(`pull_service.cpp:413, :429`): the first input's rendered tile is inserted, and
the second gets a cache *hit* on that key and is served the **first** input's
pixels/samples. A crossfade interior dissolve (`w == 0.5`) then computes
`(1-w)·a + w·a = a` instead of `(1-w)·a + w·b` — wrong, and the reason
`crossfade_runtime_binding` could not freeze a visual golden.

This task gives operator input children **distinct, stable** cache identities in
the offline/export `id_of` by extending the driver-side ephemeral map with a
transitive `Content::inputs()` walk (mirroring the serializer's existing walk,
`document_serialize.cpp:161-189`) that assigns each reachable, un-mapped child a
freshly-synthesized `ObjectId` — distinct from every seeded layer id and from
every other child, and stable across the frames of one render sequence. The
one-real-input identity short-circuit (`Content::identity()` at `w==0`/`w==1`)
is untouched: it never allocates a child key. The map stays keyed by `Content*`
pointer identity, so a **shared** input reached from two parents is emplaced once
and keys under one identity — "shared by every consumer" (doc 13:141-154)
preserved.

## Not this task

- **No compositor/core change.** `PullServiceImpl`, `TileKey`/`BlockKey`,
  `aggregate_revision`, and the `PullConfig` field set are frozen. The fix lives
  entirely in the two `runtime` drivers' `id_of` construction. No new `PullConfig`
  field (`operator_pull_delivers_target.md:107-110`: "no new `PullConfig` field
  beyond what pull_service landed").
- **No model change.** Operator input children legitimately carry no `ObjectId`
  in the model (`document_serialize.cpp:299-312`; the `SunkContent{ ObjectId{},
  live }` convention, `operator_codecs.md:206`). This task does **not** re-promote
  them or give them model records; it synthesizes cache identity in the driver.
- **No per-content revision granularity / sharded input-tile cache.** Those are
  parked (`pull_service.md:200-203` Decision 4;
  `operator_pull_delivers_target.md:123-125`) and stay parked.
- **Interactive path.** The interactive frame loop (`interactive.cpp`) and the
  interactive audio pump (`lookahead_pump.cpp` / `device_monitor.cpp`) build **no**
  production `PullConfig::id_of` at all today — `config.id_of = …` appears only in
  `offline_sequence.cpp:98` and `export_monitor.cpp:122` in non-test source.
  Introducing `id_of` on the interactive audio pump is the interactive-audio
  wiring stream's concern, not a collision fix; it is out of scope here (see Open
  questions). Extracting the shared helper below makes that future wiring a
  one-line reuse.

## Why it needs to be done

- **It closes the crossfade debt and unblocks `m9_release`.** The multi-input
  reference operator cannot freeze a visual byte-exact golden — the headline
  acceptance of `crossfade_runtime_binding` — until sibling inputs stop aliasing.
  This is the last correctness gap between "crossfade binds" and "crossfade
  renders right end-to-end."
- **It is a general multi-input correctness fix, not crossfade-specific.** Any
  operator with ≥2 same-stability inputs (present and future kinds) mis-renders
  under the collision. Fixing `id_of` fixes them all at once.
- **It restores the doc-13 caching promise for owned children.** Doc 13:141-154
  keys operator output by its id and input tiles by the input's id; with all
  children keyed on `ObjectId{}`, that promise is silently broken for owned
  children. This task makes the observable behavior match the design.

## Inputs / context

**Design docs (normative — doc 16's constitution rule):**

- `docs/design/13-effects-as-operators.md`:
  - "Caching and scheduling" (`:141-154`) — the crux: "An operator's output
    caches like any content: keyed by its id and its *aggregate* revision …
    input tiles cache under the input's identity (shared by every consumer),
    operator output under the operator's. `identity()` short-circuits both
    levels." Doc 13 mandates *that* each input keys by its identity and that a
    shared input is shared by every consumer, but leaves **how an `ObjectId`-less
    owned child's identity is derived unspecified** — the gap this task fills in
    the driver (see Decision 4). Nothing in doc 13 permits two distinct children
    to share one identity.
  - `:50-52` — `inputs()` is core-visible; leaf content returns empty. The graph
    edge the driver walks.
  - `:60-65` — `identity()` pass-through: at `w==0`/`w==1` "the compositor serves
    the input's cached tiles directly — no render, no copy, no new cache entry."
    The endpoint case allocates no child key (untouched by this fix).
  - `:69-82` — operators pull only via the `PullService`, "same machinery as a
    compositor-issued request: cache lookup first …" — the lookup this fix keys
    correctly.
  - `:108-112` — a crossfade is `Timed` even over `Static` input, extent is the
    union of its inputs'. The two inputs can be same-stability (two `Static`
    solids) — exactly the collision case.
- `docs/design/17-internal-components.md`:
  - `:41-44` — "A component may depend only on strictly lower levels … the CI
    dependency check validates the CMake target graph and the include graph."
  - `:48` — `arbc::base` (L0) owns `ObjectId`; available to every component.
  - `:53-60` — the allowed edges: `cache` (L3) depends only on `base`+`surface`
    (so cache-key types stay at/below L3 — `ObjectId` qualifies); `compositor`
    (L4) owns the `PullService` *implementation*; `kind-*` (L4) see only the
    `contract` interface; `arbc::runtime` (L5) "everything below".
  - `:60`, `:85` — "The two render drivers live in `runtime`, not the engines."
    Runtime is the only level that may name the model (`DocRoot`), the contract
    (`Content::inputs()`), `base` (`ObjectId`), and the compositor
    (`PullConfig`) together — **this task lives here.**

**Source seams (real paths + current lines):**

- The two `id_of` construction sites (**the edit surface**), byte-identical:
  - `src/runtime/offline_sequence.cpp:80` (`ContentResolver resolve`), `:87-92`
    (the `ids` map seeded from `for_each_layer`), `:98-101` (the `id_of` lambda
    with the `ObjectId{}` fallback), `:102` (`contribution` — one pinned
    revision), inside `make_config` (`:94-104`).
  - `src/runtime/export_monitor.cpp:113-118` (map seed), `:122-125` (`id_of`
    lambda), `:128-129` (`contribution`).
- The identity-consuming key derivation (**frozen — do not edit**):
  `PullConfig::id_of` decl `src/compositor/arbc/compositor/pull_service.hpp:122-127`
  ("`Content*` -> `ObjectId`: the input's cache identity … Empty -> the default
  (root) id"); visual `id` `src/compositor/pull_service.cpp:175`, `TileKey` fold
  `:217`, cache-hit serve `:223-228`; audio `id` `:413`, `BlockKey` fold `:429`.
- The graph edge to walk: `Content::inputs()`
  `src/contract/arbc/contract/content.hpp:579`
  (`std::span<const ContentRef> inputs() const`); `ContentRef` **is** `Content*`
  (`content.hpp:211-212`, `using ContentRef = Content*`) — dereference directly,
  no accessor.
- The precedent walk to mirror: `document_serialize.cpp:161-189` — a transitive
  `inputs()` frontier walk over the pinned graph with an
  `std::unordered_set<const Content*> walked` guard against cycles and shared
  re-encounters ("Extend the reverse map to the operator INPUT CHILDREN"). Same
  shape, different payload (this task assigns cache ids, not serialize meta).
- Model iteration available: `DocRoot::for_each_layer`
  `src/model/arbc/model/model.hpp:83` (yields `const LayerRecord&`, filtered to
  `RecordKind::Layer`, `model.cpp:490`); resolve is `d_document.resolve(ObjectId)
  → Content*`. **There is no `for_each_content`** and no forward `Content*→ObjectId`
  lookup — the driver must synthesize child ids, it cannot read them from the
  model.
- Why children are `ObjectId`-less: `document_serialize.cpp:299-312` ("DEMOTED to
  an owned-only input child … a child needs no ObjectId"), `:44-46` ("An operator
  input child carries no ObjectId"). The `ObjectId{}`-means-no-model-edge
  convention: `operator_codecs.md:206` (`SunkContent{ ObjectId{}, live }`).
- `ObjectId`: `src/base/arbc/base/ids.hpp:11-17` (`std::uint64_t value{0}`,
  `valid() == value != 0`); `:9-10` — value 0 is "never a valid id", the reserved
  sentinel the collision lands on.
- `TileKey` / `BlockKey` shape: `src/cache/arbc/cache/key_shapes.hpp:64-76`
  (`TileKey{content, revision, rung, coord, achieved_time}`).
- The operator whose inputs collide: `CrossfadeContent`
  `src/kind_crossfade/crossfade_content.cpp` — `d_inputs{from, to}` pulled via
  `d_pull->pull(...)` (visual `:187, :202, :219`) and `pull_audio(...)` (audio
  `:289, :310`), each input a bare `ContentRef` carrying no id.
- Behavioral-counter surface: `src/compositor/arbc/compositor/counters.hpp` —
  `operator_renders()`, `requests_issued()`, `audio_dispatches()`.
- Existing crossfade goldens to render end-to-end:
  `tests/crossfade_goldens.t.cpp` (interior `w==0.5` dissolve, endpoint
  `w==0`/`w==1` pass-through); binding harness `tests/crossfade_runtime_binding.t.cpp`.

**Predecessor decisions carried in:**

- `crossfade_runtime_binding.md:420` — the exact deferral: "the offline driver's
  `id_of` gives `ObjectId{}` to operator-input children, causing a cache-key
  collision on multi-input operators; audio is unaffected." This task carries the
  visual byte-exact + parallel/TSan acceptance named there, and additionally pins
  the audio `BlockKey` (which folds the same `id`) with a distinct-two-input guard
  so the "audio is unaffected" masking (the audio golden did not exercise two
  distinct same-stability inputs under a cached hit) cannot silently regress.
- `pull_service.md:242-245, :509-515` (Decision 6) — the two-level keying and
  `aggregate_revision` folding are authoritative and untouched; this task only
  supplies the `content` component of the key correctly.
- `operator_graph.md:420-427` — no persistent reverse map in the core; the
  ephemeral driver map is the sanctioned home.
- Test-placement rule `crossfade_runtime_binding.md:285-287` (Deviation 1):
  byte-exact acceptance needs `backend_cpu`, so it lives in top-level `tests/`,
  not `src/runtime/t/`, to keep runtime-component tests within levelization.

## Constraints / requirements

1. **Distinctness (correctness).** For any operator, two distinct input children
   must receive two distinct `ObjectId`s from `id_of`, and every child's id must
   differ from every layer-root id in the same render. No child keys on
   `ObjectId{}`. This is the invariant whose violation is the bug.
2. **Sharing preserved.** A single content reached as an input from more than one
   parent (a shared `$ref` child) keys under **one** identity, shared by every
   consumer (doc 13:141-154). Guaranteed by keying the map on `Content*` pointer
   identity and emplacing once (the `walked`/map guard).
3. **Cross-frame stability (performance / behavioral).** Within one render
   sequence (an offline sequence, an export session), the same child yields the
   same id on every frame, so its `Static` input tiles survive clock advance and
   achieved-time-coalesced frames re-render zero times
   (`11-time-and-video#static-tiles-survive-clock`,
   `#achieved-time-coalescing-issues-zero-renders`). Satisfied by building the map
   **once** per render sequence (where the current `ids` map is built) over the
   pinned, immutable graph, with a deterministic walk order.
4. **Disjoint from model ids, no `ObjectId` layout change.** Synthesized child ids
   must not collide with any seeded layer id used in the same render. Allocate them
   from a value range strictly above the maximum seeded layer id
   (`next = 1 + max(seeded values)`, incrementing per newly-reached child), which
   is provably disjoint from every layer id in the map and needs no reserved bit or
   change to `ObjectId`'s meaning (`base` untouched). Byte-exact goldens depend on
   child-id *distinctness*, never on the specific numeric values, so the exact
   allocation is not a golden input.
5. **Levelization (doc 17).** The fix lives in `arbc::runtime` (L5), which already
   names `DocRoot` (model), `Content::inputs()` (contract), `ObjectId` (base), and
   `PullConfig` (compositor). **No new component edge**; the CI dependency check
   stays green. Nothing is added to the compositor core (Constraint honoring
   `operator_graph.md:420-427`).
6. **Identity short-circuit untouched.** At `w==0`/`w==1` the crossfade's
   `identity()` serves an input directly with no new cache entry (doc 13:60-65);
   the fix must not allocate a child key on that path or perturb endpoint bytes.
7. **Concurrency.** On the parallel offline path the identity map is built once on
   the driver thread before any worker dispatch and is read-only (through the
   `shared_ptr`-captured `const` map) on workers during render — no worker mutates
   it. Race-free under TSan (reuses the write-once-then-read-only discipline
   `crossfade_runtime_binding` proved).
8. **De-duplicate the seam.** The map-build + `id_of` logic is currently copied
   verbatim across `offline_sequence.cpp` and `export_monitor.cpp`. Factor it into
   one runtime-internal helper both drivers call (see Decision 2), so the fix lands
   in one place and the future interactive-audio wiring reuses it.

## Acceptance criteria

**New claim (the promise this task lands):** register
`13-effects-as-operators#operator-input-children-have-distinct-cache-identity` in
`tests/claims/registry.tsv`, enforced by a test tagged `enforces:`. Claim text:
*In the offline/export pull configuration each operator input child receives a
distinct, stable cache identity — distinct from every layer-root identity and
from every sibling child, keyed by pointer identity so a shared child is shared
by every consumer — so two same-stability inputs of one operator cache under
different keys and render byte-exact.* Pins doc 13:141-154 for the owned-child
case.

**Identity-map unit test (direct pin):** construct a crossfade over two distinct
same-stability inputs (two `Static` solids), build the driver's `id_of`, and
assert: `id_of(from) != id_of(to)`; neither equals `ObjectId{}`; both differ from
the enclosing layer's id; and a shared child referenced from two slots returns the
*same* id from both. This test fails on today's code (both return `ObjectId{}`).
Component-local under `src/runtime/t/` (no `backend_cpu` needed).

**Visual byte-exact goldens (the carried-in headline, inline + parallel):** the
live-bound offline render of the crossfade interior dissolve (`w == 0.5`) over two
**distinct** solids, driven through the real `offline_sequence` driver on **both**
the inline and the parallel exact paths, reproduces the correct blended bytes
byte-exact against a golden — and the endpoint `w==0`/`w==1` pass-through tiles
stay byte-exact (identity short-circuit unregressed). No tolerance; re-asserts
`16-sdlc-and-quality#byte-exact-goldens`. Lives in top-level `tests/`
(needs `backend_cpu`; `crossfade_runtime_binding.md` Deviation 1). This is the
acceptance `operators.crossfade_runtime_binding` deferred here.

**Cross-frame stability (behavioral counter):** a crossfade over two `Static`
solids at a fixed interior `w`, rendered across a sequence whose playback clock
advances but whose achieved time coalesces, issues **zero** input re-renders after
the first frame (`CompositorCounters::operator_renders()` / input-render delta 0
on subsequent frames) — proving the synthesized ids are stable across frames and
the input tiles survive the clock. Re-asserts (second `enforces:` tag, no new row)
`11-time-and-video#static-tiles-survive-clock` and
`#achieved-time-coalescing-issues-zero-renders` for a multi-input operator.

**Distinct-input audio guard (byte-exact):** a crossfade over two **distinct**
tones rendered through the export-monitor audio path at an interior `w` produces
the correct mix (not input[0]'s block duplicated), byte-exact against an audio
golden — pinning the `BlockKey` `id` fix (`pull_service.cpp:429`) so the
"audio unaffected" masking cannot silently regress. Re-enforces
`13-effects-as-operators#crossfade-mixes-both-facets`.

**Concurrency (TSan lane, carried in):** the interior dissolve rendered through
the **parallel** offline path under TSan asserts no data race on the identity map
— written once on the driver thread before dispatch, read-only on workers
(Constraint 7). This is the parallel/TSan acceptance
`operators.crossfade_runtime_binding` deferred here (reuses the parallel offline
reap-to-quiescence path).

**Re-enforced end-to-end (no new rows):** with the collision fixed, the
crossfade production-binding claim
`13-effects-as-operators#crossfade-bound-to-live-services-at-instantiation`
(landed by `crossfade_runtime_binding`) is now exercised visually byte-exact end
to end, and `#operator-pulls-only-via-pull-service` holds with both inputs keyed
distinctly. Add a second `enforces:` tag on the new tests; add no new registry
rows for these.

**Coverage:** ≥90% diff coverage on changed lines (CI gate). Tests ship in this
task.

**Deferred follow-ups:** none new. This task **consumes** the visual byte-exact +
parallel/TSan debt `operators.crossfade_runtime_binding` minted; it spawns no new
WBS leaf. (The interactive-audio `id_of` wiring observation is surfaced under Open
questions for the parking lot, not minted as a task — see there.)

## Decisions

1. **Fix in the driver-side `id_of`, by synthesizing distinct child ids — not in
   the model, not in the compositor.** *Rationale:* the task note scopes it to the
   offline/export `PullConfig (id_of)`; operator input children are deliberately
   `ObjectId`-less in the model (`document_serialize.cpp:299-312`) and the
   compositor core deliberately holds no persistent reverse map
   (`operator_graph.md:420-427`). The drivers already own an ephemeral per-render
   `Content*→ObjectId` map (`offline_sequence.cpp:87-92`); extending it is the
   minimal, in-scope change with no new component edge. *Rejected:* (a)
   re-promoting children to model `ObjectId`s — reverses a settled model/serialize
   decision, touches L1/L5, far out of a 1d scope; (b) a reverse map in the
   compositor — the exact "persistent state in a pure library"
   `operator_graph.md` rejected.

2. **Factor the map-build + `id_of` into one runtime-internal helper both drivers
   call.** The logic is byte-identical across `offline_sequence.cpp` and
   `export_monitor.cpp` today; a single helper (e.g.
   `src/runtime/pull_identity.{hpp,cpp}`, taking the pinned `DocRoot` and the
   `ContentResolver`, returning the `shared_ptr<const std::unordered_map<const
   Content*, ObjectId>>` or the `id_of` functor) lands the fix once and makes the
   future interactive-audio wiring a one-line reuse (Constraint 8). *Rationale:*
   DRY, and the levelization closure (model + contract + base) is already what both
   drivers include. *Rejected:* editing the two copies in place — leaves the seam
   duplicated and invites drift; a third caller (interactive audio) is already
   foreseeable.

3. **Mirror the serializer's transitive `inputs()` frontier walk with a `walked`
   guard.** `document_serialize.cpp:161-189` already walks `Content::inputs()`
   over the pinned graph with an `unordered_set<const Content*>` guard against
   cycles and shared re-encounters, seeded from the layer-root frontier. This task
   uses the identical structure, seeding from the layer map and assigning each
   newly-reached child a synthesized id. *Rationale:* proven pattern, same graph,
   same immutability/thread-safety story (walk on the driver thread over the
   immutable pinned graph); pointer-keyed emplace gives sharing for free
   (Constraint 2). *Rejected:* a bespoke recursion — reinvents a walk the codebase
   already validated.

4. **Synthesize child ids as `1 + max(seeded layer id values)`, incrementing per
   newly-reached child in walk order — no reserved bit, no `ObjectId` layout
   change.** *Rationale:* provably disjoint from every seeded layer id (all
   synthetics exceed the max) and injective across children; stable across a
   sequence because the map is built once over the immutable pinned graph; and it
   leaves `ObjectId` an opaque `uint64_t` with value 0 as the only reserved
   sentinel (`ids.hpp:9-10`) — `base` untouched, so no design-doc delta on id
   layout. Byte-exact goldens depend only on distinctness, never on the values, so
   the scheme is golden-neutral. *Rejected:* (a) reserving a high bit of `ObjectId`
   as a "synthetic" flag — changes a `base`-level vocabulary type's meaning for a
   driver-local need, inviting a doc delta for no benefit; (b) hashing
   `(parent_id, slot)` into the same value space — reintroduces a collision risk
   with real ids that `1 + max` eliminates by construction.

5. **No design-doc delta.** *Rationale:* doc 13:141-154 already promises input
   tiles key by "the input's identity" and forbids nothing this task does; the fix
   *implements* that promise for the owned-child case and deviates from no stated
   behavior. It creates no new component edge (doc 17 respected — runtime L5
   already sees model/contract/base/compositor). The id-synthesis scheme is a
   runtime-internal driver seam whose home is this refinement, exactly as
   `fade_runtime_binding` Decision 3 kept the binder-registry seam in refinements
   rather than the docs. The doc-13 gap (id-derivation for `ObjectId`-less owned
   children left unspecified) is filled at the assigned level without amending
   designed behavior. *Rejected:* a doc-13 clarification sentence — refinements,
   not docs, are the home for internal driver-seam shape (the `operator_graph.md`
   /`fade_runtime_binding.md` precedent), and no promise or edge changes.

## Open questions

The interactive audio pump (`lookahead_pump.cpp` / `device_monitor.cpp`) builds
**no** production `PullConfig::id_of` at all today (only offline/export and test
scaffolding do), so interactive audio currently keys *all* content — layers
included — on `ObjectId{}`. That is a broader "interactive audio not yet fully
pull-wired" gap owned by the interactive-audio stream, not a multi-input collision
this task can close, and minting a WBS leaf for it risks duplicating an existing
interactive-stream task. It is **not** deferred as a named task here; it is flagged
for the parking lot so a human can confirm whether the interactive-audio wiring
task already covers reusing this task's helper. (All in-scope questions are decided.)

## Status

**Done** — 2026-07-10.

- `src/runtime/pull_identity.{hpp,cpp}`: new runtime-internal helper factoring the ephemeral `Content*→ObjectId` map build (transitive `inputs()` walk, synthesized ids above max seeded layer id) into one callable, satisfying Constraint 8 (dedup the seam).
- `src/runtime/offline_sequence.cpp`, `src/runtime/export_monitor.cpp`: both drivers simplified to call the new helper instead of inline map-build, closing the byte-identical seam duplication.
- `src/runtime/t/pull_identity.t.cpp`: unit test pinning distinctness, non-sentinel, cross-frame stability, and shared-child sharing (all four identity constraints); component-local, no `backend_cpu`.
- `tests/crossfade_offline_dissolve.t.cpp`: e2e golden (inline + parallel/TSan interior dissolve byte-exact; cross-frame static-input zero-re-render counter; distinct-tone export-audio byte-exact; endpoint identity-fires counter). Carries visual byte-exact + parallel/TSan acceptance deferred by `operators.crossfade_runtime_binding`.
- `tests/claims/registry.tsv`: new claim `13-effects-as-operators#operator-input-children-have-distinct-cache-identity` registered.
- `src/runtime/CMakeLists.txt`, `tests/CMakeLists.txt`: new targets wired.
- Deferred: endpoint `w==0`/`w==1` byte-exact-frame delivery through the offline driver remains blank (pre-existing compositor delivery gap); registered as `runtime.operator_identity_offline_delivery`. Interactive-audio `PullConfig::id_of` wiring flagged to parking lot.
