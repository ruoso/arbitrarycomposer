# model.damage — Damage records + propagation

## TaskJuggler entry

`task damage "Damage records + propagation"` in
[`tasks/10-model.tji`](../../10-model.tji) (lines 43-48), under
`task model "Versioned model"`. Note line: _"Damage as (ObjectId, region,
time-range) values; placement changes auto-damage; propagation up nesting to
viewports. Docs 01/14."_

## Effort estimate

**2d** (`effort 2d`, `tasks/10-model.tji:44`). The `Damage` value, the
per-transaction accumulator, the `DamageSink`, the flush-once-per-commit
plumbing, and coarse per-object auto-damage on every mutator **already
shipped** with `model.transactions`. This task is therefore the *precision +
boundary* pass — give auto-damage a real spatial/temporal footprint, settle
the empty/whole conventions the downstream cache and compositor consume, and
draw the model↔compositor propagation line — not new commit machinery.

## Inherited dependencies

- **`model.transactions`** — **settled** (landed; see
  [`tasks/refinements/model/transactions.md`](transactions.md) Status,
  2026-07-05). It froze exactly the surface this task refines:
  - `struct Damage { ObjectId object; Rect rect; Time start; Time end; }`
    (`src/model/arbc/model/damage.hpp:19-26`) — the `(ObjectId, region,
    time-range)` value, all `arbc::base` types, transient handle (never an
    in-arena record).
  - `rect_union` (`damage.hpp:30-38`) and `damage_add` (union/dedup by object
    id, `damage.hpp:44-54`) — the accumulator the flush-once contract rides.
  - `class DamageSink { virtual void flush(const std::vector<Damage>&) = 0; }`
    (`damage.hpp:60-64`) — the abstract, model-defined, writer-owned seam,
    installed via `Model::set_damage_sink` (`model.hpp:173`, field
    `d_damage_sink` `model.hpp:338`). **Still has no concrete consumer.** Its
    own header comment (`damage.hpp:12-18, 56-59`) reserves this task by name:
    _"`model.damage` reuses it and adds auto-damage-on-placement and
    up-nesting propagation on top."_
  - `Transaction::add_damage(const Damage&)` (`model.hpp:265`, impl
    `model.cpp:928`), accumulator field `d_damage` (`model.hpp:310`), commit
    flush (`model.cpp:990-992`), undo/redo replay of the entry's stored damage
    (`model.cpp:1033-1037`), coalescing union via `coalesce_entries`
    (`journal_entry.hpp:55`, impl `model.cpp:359-360`).
  - `struct JournalEntry { … std::vector<Damage> damage; }`
    (`journal_entry.hpp:43-49`) — damage already rides the entry, so undo/redo
    replay it.
- **`model.composition_membership`** — **settled** (see
  [`composition_membership.md`](composition_membership.md)). Its
  `attach_layer` / `detach_layer` / `reorder_layer` mutators
  (`model.hpp:228,237,243`) each already emit **one** coarse damage for the
  composition id (`model.cpp:774,815,848`; claim
  `14-data-model-and-editing#membership-edit-damages-composition`,
  `tests/claims/registry.tsv:22`). This task upgrades their footprint, not
  their count/keying.

No **pending** inherited dependencies.

## What this task is

Turn the *coarse, region-less, time-less* auto-damage the model emits today
into **precise, well-typed damage values**, and pin the boundary that
realizes doc 01's "propagates up through nesting to every viewport." Three
concrete moves:

1. **Structural auto-damage gets a real footprint.** Every placement/graph
   mutator (`set_transform`, opacity, attach/detach/reorder, `remove`)
   currently emits `Damage{id, {}, {}, {}}` — a default-empty `Rect` and a
   degenerate `[0,0]` time range (`model.cpp:518,545,574,607,774,815,848,920`).
   Because model sits below the `Content` vtable (L2 < L3) it cannot *bound*
   what changed, so it over-approximates soundly: **whole object, all time**.
2. **Content damage becomes caller-supplied and is carried faithfully.** A
   content edit's real region and time-range are known only to the kind's
   `Editable` method (L3) — it calls `txn.add_damage(Damage{content, region,
   range})` under doc 14's "capture, mutate, **damage**" discipline. This task
   removes the redundant coarse model floor on `set_content_state`
   (`model.cpp:892`) so the caller's precise region is never clobbered.
3. **The value carries a first-class time-range and the propagation boundary
   is drawn.** Migrate `Damage`'s loose `Time start/end` to the purpose-built
   `base::TimeRange`; define the empty/whole conventions (`Rect::infinite()` /
   `TimeRange::all()`); and record that the transitive up-nesting-to-viewport
   routing is the **compositor's** `damage routing over inputs()` (doc 17:56),
   consumed through the existing `DamageSink`. Model produces object-keyed,
   soundly-approximated damage; it never reaches viewports.

## Why it needs to be done

Auto-damage is the *only* path by which anything re-renders in interactive
mode (doc 01:139-141), and the whole invalidation chain downstream is already
built against damage it cannot yet trust:

- **`compositor.refinement`** (`src/compositor/arbc/compositor/refinement.hpp:13`
  includes `damage.hpp`) already **produces** `Damage` on async tile arrival
  (`refinement.hpp:139`) but is explicitly _not_ the sink consumer.
- **`compositor.damage_planning`** (`tasks/35-compositor.tji:22-27`) is the
  scheduled consumer that maps `Damage` to per-viewport device dirty regions
  and schedules follow-up frames, routing each member's damage to its parent
  via `Content::map_input_damage(input, rect)`
  (`src/contract/arbc/contract/content.hpp:228`, "over-approximation is sound,
  under-approximation drops repaint").
- **`cache::invalidate_region(content, rect, geom)`**
  (`src/cache/arbc/cache/invalidation.hpp`) decomposes damage into
  `(ObjectId, base::Rect)` tile drops — it never sees `Damage` itself (cache
  is L3, deps `{base, surface}`, no `model` edge).

All three need the model's damage to (a) carry a **sound** region — an empty
`Rect` that silently unions to "nothing" (today's default) drops repaint; and
(b) carry a **sound** time-range — a `[0,0]` range means "damaged at no
instant," so the audio lookahead ring, which "re-mixes only blocks whose
sources took damage inside the window" (doc 14:213-217), would skip a
structural edit. This task makes both sound.

## Inputs / context

Design docs (normative — doc 16's executable-spec discipline):

- `docs/design/01-core-concepts.md`
  - § *Invalidation* (lines 133-141) — the canonical spec. "Content pushes
    **damage**, never pixels: 'region R of my local space changed' (**R may be
    everything**)." "Placement changes (transform, opacity, order) generate
    damage in the parent automatically." "Damage propagates up through nesting
    to every viewport observing an affected composition; **each viewport maps
    it to a dirty device region** and schedules re-rendering." — note the
    subject that maps to device regions is the *viewport/compositor*, not the
    model. "R may be everything" licenses the whole-object over-approximation.
  - § *Content and layer kinds* (lines 50-63) — **bounds** "may be finite …,
    infinite (a procedural background), or unknown-until-asked" (so an infinite
    rect is a first-class region), and **stability** `Static`/`Timed`/`Live`
    (lines 60-63) — the temporal axis motivating a real time-range; `Live`
    content is never journaled.
  - § *Identity and versioning* (lines 143-153) — `ObjectId` is "a stable
    per-object `ObjectId`"; cache keys are `(content identity, revision,
    region, quantized scale)` "without the cache needing to understand what
    changed" (lines 146-148).
  - § *Transforms* (lines 79-89) — transforms "compose multiplicatively down
    the nesting hierarchy"; the compositor resolves the single affine between
    viewport device pixels and a layer's local space (the mapping the *routing*
    half uses).
- `docs/design/14-data-model-and-editing.md`
  - § *Identity* (lines 74-83) — `ObjectId` (64-bit, document-unique) is "the
    address used by the journal, by editor selection, **by damage records**"
    (lines 76-79). Direct evidence for the `ObjectId` key.
  - § *Transactions* (lines 86-127) — **Damage rides the transaction**: "each
    mutation contributes damage (doc 01); commit flushes the union once.
    Undo/redo replays the entry's damage so invalidation is exactly right
    without diffing" (lines 108-110); **coalescing** merges consecutive gesture
    commits (lines 102-107); membership edits "**damage the parent composition
    once**" (lines 119-124, anchors `#membership-edit-damages-composition`,
    `#layer-order-is-explicit`).
  - § *Content state: the `Editable` facet* (lines 129-187) — the edit
    discipline is "**capture-once-per-entry, mutate, damage**" (lines 151-153);
    `restore` must "**emit damage for what changed**" (lines 142-144); "**damage
    equals the stroke's tile set**", so undo memory is `O(touched tiles)` (lines
    164-171); **Live content opts out** — never journaled, no `Editable`, no
    document damage (lines 173-179); render purity makes `render()` a pure
    function of `(state, region, scale, time)` (lines 181-187).
  - § *History* (lines 189-209) — the journal entry stores the "**damage set**"
    (lines 194-195); undo/redo are "ordinary publishes" that replay it (lines
    196-198).
  - § *The scenarios, validated* (lines 211-231) — audio lookahead "re-mixes
    only blocks whose sources took damage inside the window" (lines 213-217,
    the one real *time-range* consumer); viewports "re-render only damaged tiles
    at their current scale rungs" (lines 218-220); the same content embedded
    three compositions deep "updates everywhere via **aggregate revisions**
    (docs 05/13)" (lines 221-225) — nested propagation is the compositor's, not
    the model's.
- `docs/design/17-internal-components.md`
  - Component table (line 52): `arbc::model` is **L2**, owning "object records,
    persistent `DocState`, transactions, journal/undo, **damage**, revisions,
    pins", depending on **`base`, `pool` only**.
  - Line 53: `arbc::contract` (L3) owns "**damage sinks**"; line 56:
    `arbc::compositor` (L4) owns "**damage routing over `inputs()`, aggregate
    revisions**". This is the normative split this task's boundary follows.
  - Levelization rule (lines 41-44): "A component may depend only on strictly
    lower levels"; CI validates via `scripts/check_levels.py` (`"model":
    {"base", "pool"}`).
- `docs/design/16-sdlc-and-quality.md`
  - § *Test taxonomy* — the conformance suite gates "**damage soundness
    (undamaged regions bit-identical across edits)**" (lines 35-37); tier-4
    behavioral counters (lines 54-62, "Wall-clock tests lie in CI; counters
    don't"; "a change deep in a shared child invalidates exactly the
    embeddings' mapped regions"); byte-exact goldens (lines 50-53).
  - Diff-coverage hard gate ≥90% (lines 112-118); claims mechanics
    (`tests/claims/registry.tsv`, `scripts/check_claims.py`, `enforces:
    <claim-id>` tags).

Source seams this task extends:

- `src/model/arbc/model/damage.hpp` — the whole file (`Damage` :19-26,
  `rect_union` :30-38, `damage_add` :44-54, `DamageSink` :60-64). Migrated +
  the whole/empty helpers added here.
- `src/base/arbc/base/geometry.hpp:14-32` — `struct Rect` (`double
  x0,y0,x1,y1`, half-open, `empty()` :24, `intersect` :26-29). Add
  `Rect::infinite()`.
- `src/base/arbc/base/time.hpp:21-43` — `struct TimeRange` (half-open
  `[start,end)`, `empty()` iff `end<=start` :34, the "temporal analog of a
  `Rect`", the return type of `Content::time_extent()`). Add `TimeRange::all()`.
- `src/model/arbc/model/records.hpp:60-67` — `LayerRecord { ObjectId content;
  Affine transform; double opacity; std::uint32_t flags; }` — **no temporal
  span field**; the flags comment (`records.hpp:36-38`) reserves span/time-map
  bits for the *timeline* tasks. This is why placement damage's time-range is
  `all()`, not a per-layer span, at L2 today. `CompositionRecord` layers
  (`records.hpp:95-102`).
- `src/model/model.cpp` — the coarse-emission sites to refine: `:607`
  (`set_transform`), `:774/:815/:848` (attach/detach/reorder), `:892`
  (`set_content_state` — floor removed), `:920` (`remove`), `:518/:545/:574`
  (object creation). Flush `:990-992`; replay `:1033-1037`; coalesce
  `:359-360`. Mutator declarations `model.hpp:219,228,237,243,250,255`.
- `src/model/t/transactions.t.cpp:15-31` — `RecordingCommitSink` /
  `RecordingDamageSink` test doubles (reuse/extend, don't re-invent).
- Downstream consumers (for the boundary, not edited here):
  `src/compositor/arbc/compositor/refinement.hpp:13,139`;
  `src/contract/arbc/contract/content.hpp:228` (`map_input_damage`);
  `src/cache/arbc/cache/invalidation.hpp` (`invalidate_region`);
  `src/compositor/arbc/compositor/compositor.hpp:16-20` (`Viewport`, carries
  `ObjectId anchor`).

Predecessor-refinement conventions:
[`transactions.md`](transactions.md) (the `Damage`/sink surface, behavioral
counters over wall-clock, asan-lane concurrency),
[`composition_membership.md`](composition_membership.md) (one-damage-per-edit
keyed to the composition, structural-sharing invariant),
[`journal.md`](journal.md) (undo is a forward publish replaying stored damage).

## Constraints / requirements

- **Levelization (doc 17, CI-gated).** `arbc::model` stays L2, deps `base` +
  `pool`. The model **cannot** call `Content::bounds()` /
  `Content::time_extent()` / `Content::map_input_damage()` (all L3
  `contract`), so it cannot compute a content-space footprint or temporal
  extent for a placement change — the over-approximation to whole-object /
  all-time is *forced* by levelization, not laziness. The transitive routing
  to viewports/device regions is the compositor (L4). `scripts/check_levels.py`
  stays green; no new down-edge, no new model-defined sink (the existing
  `DamageSink` is sufficient).
- **Single-writer, no new invariant.** Auto-damage is emitted on the writer
  thread inside the mutator path-copy, exactly where the coarse damage is
  emitted today. This task **adds no reverse parent-index** and maintains no
  new cross-version structure (see Decisions) — damage stays keyed to the
  ObjectId of the record the mutation wrote.
- **Sound region convention.** In a `Damage`, an **empty** `Rect` contributes
  nothing to a union (identity, as `rect_union` already treats it); an
  **infinite** `Rect` (`Rect::infinite()`) means "whole object / region
  unknown" and is **absorbing** under union (min/max with ±∞). Model emits
  `Rect::infinite()` for structural damage; finite rects arrive only from
  callers. The two never share an object id (structural damage is keyed to the
  layer/composition; content damage to the content), so a whole-object marker
  never clobbers a caller's finite region.
- **Sound time-range convention.** Migrate to `base::TimeRange`. An **empty**
  range (`end<=start`) contributes nothing (identity); `TimeRange::all()`
  (`[Time::min, Time::max)`) means "all instants" and is absorbing. Structural
  damage carries `all()`; content damage carries the caller's finite range (or
  `all()` for `Static` content). `damage_add`'s range union must treat empty as
  identity — replacing today's raw `min(start)/max(end)`, which wrongly folds a
  `[0,0]` default toward instant 0.
- **Structural auto-damage: whole object, all time, one record per edit, keyed
  to the edited record.** `set_transform`, opacity, `attach/detach/reorder`,
  `remove` each emit exactly one `Damage{edited_id, Rect::infinite(),
  TimeRange::all()}`. Count and keying are unchanged from the shipped coarse
  behavior (so `#membership-edit-damages-composition` still holds); only the
  footprint changes. "Damage in the parent" (doc 01:137) is realized by the
  compositor routing a member's damage to its containing composition over
  `inputs()`, not by model re-keying.
- **Content auto-damage: caller-supplied, floor removed.** `set_content_state`
  no longer self-emits damage. The content's region and time-range come from
  the kind's `Editable` method via `add_damage` (doc 14:151-171). Damage
  honesty — that a content edit damages exactly its touched region — is a
  conformance obligation (doc 16:35-37), not something model can enforce from
  L2. Callers that swap content state without an `Editable` (e.g. a future
  `model.content_binding` migration) supply their own whole-content damage
  `Damage{content, Rect::infinite(), TimeRange::all()}` at their call site.
- **Undo/redo and coalescing carry the refined footprint.** The journal entry
  stores the flushed damage set (`journal_entry.hpp:48`); `navigate` replays it
  through the `DamageSink` unchanged (`model.cpp:1033-1037`) — so a replayed
  region/range must be bit-identical to the forward one. `coalesce_entries`
  (`model.cpp:359-360`) unions per-object regions (bbox) and ranges under the
  same empty=identity / whole=absorbing rules.
- **Transient value, record invariants untouched.** `Damage`, `TimeRange`, and
  `JournalEntry` remain transient handle types (may hold STL/`Ref`); no
  `records.hpp` `static_assert` (standard-layout, pointer-free, trivially
  destructible) is affected. `Rect::infinite()` / `TimeRange::all()` are
  `constexpr` value helpers on existing base structs.

## Acceptance criteria

- **Unit tests** (`src/model/t/damage.t.cpp`, wired via
  `arbc_component_test(COMPONENT model …)` in `src/model/CMakeLists.txt`;
  Catch2, extending the `RecordingDamageSink` from `transactions.t.cpp:15-31`):
  - each of `set_transform` / opacity / `attach_layer` / `detach_layer` /
    `reorder_layer` / `remove` flushes exactly one damage for the edited
    object id, with an **infinite** rect and an **all** time-range;
  - a caller `add_damage(Damage{content, finite_rect, finite_range})` survives
    commit → flush → journal entry → undo/redo replay **bit-identical** (no
    model floor widens or clobbers it);
  - within one transaction, two finite content damages on the same content id
    union to the bbox / `[min,max]`; an empty-rect / empty-range damage is
    identity; an infinite rect / `all()` range is absorbing;
  - `set_content_state` alone (no `add_damage`) flushes **no** content damage.
- **Behavioral-counter assertions (doc 16:54-62 — counters, never
  wall-clock).** On `RecordingDamageSink`: N placement mutations in one commit
  → exactly one `flush` whose set has one record per distinct edited object;
  an aborted transaction → zero flushes; `Model::revision()` +1 per commit
  unchanged. (No wall-clock.)
- **Claims (register in `tests/claims/registry.tsv`; enforce with an
  `// enforces: <claim-id>` tagged test — `scripts/check_claims.py` gates both
  directions):**
  - `01-core-concepts#placement-change-auto-damages` — a placement edit
    (transform, opacity, order) auto-emits exactly one damage for the edited
    object, spanning the whole object (infinite region) at all instants (whole
    range), flushed once at commit (realizes doc 01:137-141).
  - `14-data-model-and-editing#damage-carries-region-and-time` — a
    caller-supplied content damage's finite region **and** time-range survive
    mutation → commit → flush → journal → undo/redo replay unchanged; the model
    adds no coarse floor that widens them (realizes the `(ObjectId, region,
    time-range)` shape + the `O(touched tiles)` promise, doc 14:164-171).
  - `14-data-model-and-editing#structural-damage-spans-all-time` — structural /
    placement damage carries an unbounded (`all()`) time-range, so a temporal
    consumer (the audio lookahead window, doc 14:213-217) never skips a
    structural edit.
  - The existing `#membership-edit-damages-composition`
    (`registry.tsv:22`) is **extended** (rect now infinite, range now `all()`);
    its exactly-one-damage-per-edit assertion is preserved. Update the enforcing
    test's footprint expectation accordingly.
- **Concurrency (doc 16 tier 6).** Extend the pin/traverse/commit asan smoke
  (per `transactions.t.cpp`) so the committing writer emits refined damage
  through a no-op sink while a reader pins + traverses; assert no torn read /
  use-after-free under the **asan lane** (no TSan preset in-tree — the parked
  convention from `pool/reclamation.md`; full seeded stress remains
  `quality.stress_harness`, `tasks/70-quality.tji`, not duplicated).
- **Coverage / gate.** ≥90% diff coverage on changed lines; `scripts/gate`
  green including asan, `scripts/check_levels.py`, `scripts/check_claims.py`;
  the shipped `transactions` / `composition_membership` model tests stay green
  (only footprint expectations updated).
- **Deferred follow-up.** None new. The transitive up-nesting-to-viewport
  routing is `compositor.damage_planning` (`tasks/35-compositor.tji:22-27`,
  already a WBS leaf) — this task's output is its input, so it is a downstream
  consumer, not a deferral. No "audit/revisit" task is created.

## Decisions

- **Model over-approximates structural damage to whole-object / all-time; it
  does not (cannot) compute a footprint.** `Content::bounds()` and
  `time_extent()` are L3; model is L2 (doc 17:52-53). Doc 01:136 explicitly
  allows "R may be everything," so an **infinite** rect + `all()` range is a
  sound, doc-sanctioned over-approximation, and the compositor refines the real
  device footprint using bounds it *can* query. _Rejected:_ keeping the
  default-**empty** rect/`[0,0]` range (unsound — empty unions to *nothing*, so
  a structural edit could invalidate no region and re-mix no audio block;
  precisely the "under-approximation drops repaint" failure, doc 13 /
  content.hpp:228). _Rejected:_ pulling `Content` bounds down into model to
  compute a real footprint (illegal L2→L3 down-edge, breaks
  `check_levels.py`).
- **Damage stays keyed to the edited record's `ObjectId`; the compositor does
  the up-nesting.** Doc 17 splits the work: model owns "damage" (line 52), the
  compositor owns "damage routing over `inputs()`, aggregate revisions" (line
  56); doc 01 itself locates "each viewport maps it to a dirty device region"
  at the viewport. So a `set_transform` on a layer keys damage to the *layer*
  (as shipped, `model.cpp:607`); the compositor maps the member's damage to its
  parent composition and onward via `Content::map_input_damage`
  (`content.hpp:228`). Membership edits key to the *composition* because they
  *are* composition-record edits — the rule is uniform (key = the record
  written), not a special case. _Rejected:_ a model-maintained reverse
  parent-index (layer→composition) so placement re-keys to the parent — it
  duplicates the nesting graph the compositor already resolves top-down for
  rendering, adds a cross-version invariant to maintain across path-copy
  publishes, and still could only yield a *whole-parent* damage (strictly
  coarser than keying to the member and letting the compositor map the member's
  footprint). _Rejected:_ model walking the composition graph to pre-expand
  damage for all ancestors — the up-mapping through an embedding needs
  `map_input_damage` (content covering) and resolved transforms, both L3+
  (docs 05/13; doc 14:221-225 routes nested propagation via aggregate revisions
  at the compositor).
- **Content damage is caller-supplied; the coarse model floor on
  `set_content_state` is removed.** Only the kind's `Editable` method (L3)
  knows the touched region and temporal extent, and doc 14 makes emitting it
  part of the "capture, mutate, **damage**" discipline (lines 151-171); damage
  honesty is conformance-tested (doc 16:35-37). A model-emitted whole-content
  floor would be **absorbing** and erase the caller's precise tile-rect,
  defeating the `O(touched tiles)` promise (doc 14:164-171); an empty-rect
  floor would be **unsound**. Removing it is the only correct option at L2.
  _Rejected:_ keeping a whole-content floor (clobbers precision) or a
  suppress-floor-if-refined bookkeeping pass (needless complexity — the
  discipline + conformance suite already guarantee the caller damages).
- **Migrate `Damage` to `base::TimeRange`; add `Rect::infinite()` /
  `TimeRange::all()`.** `TimeRange` already exists as "the temporal analog of a
  `Rect`" with proper half-open `empty()` semantics (`time.hpp:21-43`), and the
  `transactions` refinement froze the field conceptually as a "time-range." The
  loose `Time start/end` pair carries a latent footgun: `damage_add`'s raw
  `min(start)/max(end)` (`damage.hpp:48-49`) does **not** treat a `[0,0]`
  default as identity, so a coarse default would fold real ranges toward
  instant 0. Migrating gives symmetric `Rect`/`TimeRange` value fields, an
  empty=identity / whole=absorbing union for both axes, and a value that
  literally reads `(ObjectId, region, time-range)`. The churn is contained to
  `damage.hpp`, the emission sites, and `transactions.t.cpp`. _Rejected:_
  keeping `Time start/end` and just populating them (leaves the documented
  discrepancy and the `[0,0]`-not-identity footgun, and keeps the value
  asymmetric — `Rect` for space, loose fields for time).

**Design-doc delta.** None. The behavior is already normative: doc 01:133-141
(placement auto-damages; "R may be everything"; viewports map damage to device
regions), doc 14:108-124 / 151-171 (damage rides the transaction; the kind
emits the region; `O(touched tiles)`), and doc 17:52/56 (model owns damage, the
compositor owns damage routing over `inputs()`) settle both the auto-damage and
the propagation boundary. The empty/infinite-`Rect` and `all()`/empty-`TimeRange`
encodings are an implementation convention pinned here in Decisions + the
acceptance claims, not an amendment to the constitution — matching the
"No design-doc delta" precedent of the sibling model refinements.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-06.

- `src/base/arbc/base/geometry.hpp` — added `Rect::infinite()` constexpr helper (absorbing under `rect_union`)
- `src/base/arbc/base/time.hpp` — added `TimeRange::all()` constexpr helper (`[Time::min, Time::max)`, absorbing under range union)
- `src/model/arbc/model/damage.hpp` — migrated `Damage` `Time start/end` → `TimeRange range`; added `range_union`; empty=identity/whole=absorbing on both spatial and temporal axes
- `src/model/model.cpp` — all structural emission sites now emit `{Rect::infinite(), TimeRange::all()}`; removed the coarse `set_content_state` floor; added minimal `set_opacity` mutator (mirrors `set_transform`, enables enforcement of `#placement-change-auto-damages` claim)
- `src/model/arbc/model/model.hpp` — declared `set_opacity`; updated `add_damage` doc
- `src/compositor/refinement.cpp` + `src/compositor/arbc/compositor/refinement.hpp` — compile follow-through of the field rename: tile-arrival `Damage` uses `TimeRange{when,when}`
- `src/compositor/t/counters.t.cpp` — updated damage construction for the field rename
- `src/model/t/composition_membership.t.cpp` — extended membership footprint assertion to infinite rect / all() range
- `src/model/t/damage.t.cpp` (new) + `src/model/CMakeLists.txt` — new unit test: structural footprint per mutator, caller-damage survives undo/redo bit-identical, floor-removed no-op, union semantics, absorbing/identity, behavioral-counter (N-mutation→one-flush / abort→zero), asan concurrency smoke
- `tests/claims/registry.tsv` — extended `#membership-edit-damages-composition`; added `#placement-change-auto-damages`, `#damage-carries-region-and-time`, `#structural-damage-spans-all-time`
