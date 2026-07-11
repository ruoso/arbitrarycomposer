# 14 — Data Model and Editing

How the C++ data model supports change *to* the composition — as opposed to
change *within* it (playback time, doc 11). The forcing scenario: a host
application that is simultaneously an NLE (Kdenlive), a raster editor
(GIMP), a vector editor (Inkscape), and a live production tool (OBS), all
over one document. That host needs:

- undo/redo that interleaves paint strokes, bezier drags, timeline trims,
  and effect-graph rewires in one history;
- a live program output (stream/record) that keeps rendering and mixing,
  glitch-free, *while* the document is being edited;
- edits during playback picked up cleanly at frame boundaries;
- autosave that never pauses editing or output.

Three placeholders in earlier docs — per-object revisions (doc 01), the
snapshot token (doc 02), and "mutation must be visible, not expressible"
(doc 03) — are one mechanism. This doc makes it concrete.

## The central decision: versions, not mutations

The document is modeled as a succession of **immutable versions** with
structural sharing. Editing never mutates the version anyone else can see;
it builds the next version and publishes it atomically.

```
DocState v41 ──edit──▶ v42 ──edit──▶ v43        (publish = atomic swap)
   ▲                      ▲
   │                      └── UI viewport renders latest
   └── stream encoder pinned here until its frame completes
```

Why this over mutable-objects-plus-locks:

- **The snapshot token becomes real and free.** A pinned `DocState` *is*
  the doc-02 revision fence: render planning, the audio lookahead window,
  an offline export frame, and an autosave each pin a version and read it
  without locks, at any pace, while editing proceeds. The OBS requirement
  is satisfied by construction, not by careful locking.
- **Undo/redo becomes navigation.** History is a journal of versions (with
  deltas for damage replay); undo publishes a new version restoring prior
  state — history itself is never mutated, so redo, coalescing, and
  history inspection are trivial and always consistent.
- **Value semantics matches the scale.** Compositions are small graphs
  (hundreds of layers × small placement structs); copying the touched path
  of a persistent map per commit is nanoseconds-to-microseconds. The big
  state — pixels, vector trees — is inside content, handled by the same
  principle one level down (below).

Concretely: `DocState` is a persistent (path-copying) map from `ObjectId`
to immutable object records — composition records (layer order, canvas,
working spaces), layer records (placement: transform, opacity/gain,
span/time map, flags, content reference), and content records (kind id +
a state handle, below). The writer thread is the single mutator (doc 02's
single-writer rule, unchanged); `Document` holds the current `DocState`
in an atomic shared pointer.

A composition's ordered layer list is stored inline in the composition
record up to a small fixed cap (`k_max_inline_layers`); past the cap it
spills to a HAMT-backed chain of order-chunk objects — ordinary records in
the same `DocState` map, keyed by their own `ObjectId` and named from the
composition by a plain `ObjectId` spill-root value (never an owning edge, so
the composition record stays fixed-size and trivially destructible like every
other *object* record — the persistent map's interior nodes are the one
deliberate exception, doc 15). Crossing the cap migrates inline → chunks; dropping back to
the cap collapses chunks → inline, so a given membership has one canonical
record. Because chunks are ordinary map objects, the persistent map owns,
path-copies, and reclaims them with no bespoke ownership machinery, and a
membership edit path-copies only the touched chunk map-paths — every
untouched chunk is shared by `SlotRef` identity across versions
(`14-data-model-and-editing#membership-spills-past-inline-cap`). Order
rewrites are O(members) worst case, which value semantics blesses at this
scale ("hundreds of layers … nanoseconds-to-microseconds", above).

## Identity

Every object carries a stable `ObjectId` (64-bit, document-unique,
assigned at creation) — the address used by the journal, by editor
selection, by damage records, and by the `contents` sharing table when
serialized (doc 08). Identity is runtime state, not (yet) file format
state; persistent identity across save/load is only guaranteed where the
format already references it (`$ref` targets). If real-time collaboration
ever lands, per-object ids plus the journal are its hooks (deferred, out
of scope, but deliberately not precluded).

## Transactions

All mutation flows through a transaction on the writer thread:

```cpp
{
  auto txn = doc.transact("Trim clip");
  txn.set_span(layer_id, TimeSpan{in2, out2});
  txn.set_transform(other_layer, m);
  txn.set_opacity(other_layer, 0.5);   // placement mutators (span, transform,
                                       // opacity, ...) all auto-damage
  txn.commit();          // builds next DocState, publishes atomically,
                         // appends ONE journal entry, flushes damage once
}
```

- **Atomicity**: observers see v(n) or v(n+1), never a half-edit. A
  crossfade insertion that retims two layers and inserts an operator is
  one publish.
- **Coalescing**: interactive gestures (brush stroke, handle drag,
  scrubbing a parameter) issue many commits; a coalescing key
  (`txn.coalesce(gesture_id)`) merges consecutive journal entries so undo
  lands at gesture granularity while the display still updates per-commit.
  This is the standard editor discipline; the model supports it natively
  instead of every host reinventing it.
- **Damage rides the transaction**: each mutation contributes damage
  (doc 01); commit flushes the union once. Undo/redo replays the entry's
  damage so invalidation is exactly right without diffing.
- **Abort**: a transaction that is dropped without `commit()` (or whose
  `txn.abort()` is called) publishes nothing — the current version and its
  revision are unchanged, the working records it built are reclaimed, and no
  journal entry or damage is emitted. Nothing is observable until commit, so
  abort is a discard, not a rollback.
- Reading needs no transaction: the writer reads current state directly;
  everyone else reads pinned versions.

Composition membership edits — `attach_layer`, `detach_layer`,
`reorder_layer` — are placement/graph transactions in exactly this class:
they rewrite pure core records (the composition and its spill chunks), damage
the parent composition once
(`14-data-model-and-editing#membership-edit-damages-composition`,
`#layer-order-is-explicit`), and undo/redo as ordinary inverse publishes over
the existing journal `ObjectEdit` edges with no new entry type, restoring the
prior order even across the inline↔spill boundary
(`14-data-model-and-editing#membership-undo-round-trips`).

## Content state: the `Editable` facet

Placement lives in core records, but content *internals* (the pixels, the
bezier tree, the child composition) are kind-owned. For those to
participate in the same history and the same snapshots, editable kinds
implement a third facet (alongside visual and audio):

```cpp
class Editable {
public:
  // An immutable, structurally-shared snapshot of internal state.
  // capture() must be O(small) — cheap enough to call once per gesture.
  virtual StateHandle capture() const = 0;

  // Adopt a prior state (undo/redo path); emit damage for what changed.
  virtual void restore(const StateHandle&) = 0;

  // Journal memory budgeting.
  virtual size_t state_cost(const StateHandle&) const = 0;

  // State-handle lifetime. The runtime binding drives these off the model's
  // StateRefSink seam: retain when a published version pins the handle,
  // release when its record is reclaimed, so "a pinned version pins content
  // state too" and "version GC releases unreferenced state handles by
  // refcount" both hold. Writer/drain-thread only.
  virtual void retain(StateHandle) = 0;
  virtual void release(StateHandle) = 0;
};
```

Kind-specific edit APIs take the transaction and follow one discipline:
capture-once-per-entry, mutate, damage:

```cpp
raster->paint(txn, brush, stroke);      // RasterContent API
vector->move_anchor(txn, node, p);      // VectorContent API
```

The transaction stores (before, after) state handle pairs in the journal
entry; the published `DocState` holds the *after* handle, so a pinned
version pins content state too — render workers see frozen pixels while
the user keeps painting.

**Structural sharing is what makes this viable at GIMP scale**, and the
reference kinds prove the pattern: `org.arbc.raster`'s state is a
persistent tile table — a paint stroke copies only touched tiles, so
`capture()` is O(1), undo memory is O(touched tiles), and damage equals
the stroke's tile set. A vector kind shares unchanged subtrees. This is a
*discipline the contract imposes* (capture must be cheap and shared, not a
deep copy), documented as such for plugin authors; the contract tests
(doc 10) enforce capture/restore round-trips and damage honesty.

**Live content opts out.** A camera feed, a capture source, a 3D engine
view (the OBS-style sources) have no meaningful document state — they are
`Live` (doc 11) and simply don't implement `Editable`: never journaled,
never restored, still frame-consistent in outputs via ordinary pull
snapshotting. The facet split keeps "editable document" and "window onto
the live world" from contaminating each other — precisely the
multi-disciplinary line between the GIMP half and the OBS half of the app.

**Purity refinement to the render contract (doc 03):** for editable
content, the pinned state travels with the request — `RenderRequest`'s
snapshot resolves to the content's `StateHandle`, and `render()` must
render *that* state, making rendering a pure function of
(state, region, scale, time). This is what makes cache keys honest while
edits race renders: revision identifies state, state is immutable, so a
cached tile can never show pixels newer than its key.

**Runtime binding of the facet.** The `Editable` methods are the content's
half of the contract; the model's half is the state sinks it exposes
(`StateRefSink` on the `Model`, `StateCostFn`/`RestoreSink` on the `Journal`).
Joining them is a `runtime` concern (doc 17: the model stays free of the
`Content` vtable): when the runtime instantiates an editable content it
registers generic facet-backed adapters onto the live `Model`/`Journal` and
tears them down on release — the state-sink analogue of the damage sink the
core connects on attach (doc 03). The record minted for an editable content
embeds its `capture()`d initial state, not an inert handle, published in the
same transaction that mints it: a content that is editable already *has* state
at instantiation, and a record naming no state would leave the first edit's
journal entry with an inert *before* handle — an undo that restores the content
to nothing. Non-editable content keeps the inert record. v1 binds one editable
content per document (the seams are single-slot and a `StateHandle` does not
name its owner); multiplexing many editable contents onto the shared journal is
a later refinement.

## History

The journal is core-owned and document-wide — one history across all
kinds, which is the whole point for a multi-disciplinary editor:

- Entry: name, coalescing key, per-object placement deltas, per-content
  (before, after) state handle pairs, damage set.
- Undo publishes a new version applying the entry's inverse; redo
  re-applies. Both are ordinary publishes: outputs and viewports need no
  special path (undo during playback or streaming just works).
- Budgeted by bytes via `state_cost` (+ record sizes), trimmed from the
  tail; version GC releases unpinned `DocState` nodes and unreferenced
  state handles by refcount. A pinned old version (a slow export) keeps
  only what it references alive — structural sharing bounds the cost to
  what actually changed since. The allocator model that makes this exact,
  incremental, and fragmentation-free — inside-out slab arenas with
  deferred reclamation — is doc 15.
- Deltas as an optimization (a kind supplying operation records instead of
  handle pairs, for very fine-grained histories) are a deferred extension;
  handle pairs with structural sharing are the v1 contract because they
  are impossible to implement incorrectly in ways that corrupt history.

## The scenarios, validated

- **OBS half**: the stream/record output pins the latest published version
  per frame; the audio engine's lookahead ring re-mixes only blocks whose
  sources took damage inside the window (doc 12). Editing, undo, even
  loading another project into a nested layer never block the output
  thread — worst case it renders one version behind.
- **GIMP half**: brush strokes are coalesced transactions over CoW tiles;
  mid-stroke, viewports re-render only damaged tiles at their current
  scale rungs; undo is a tile-set swap plus replayed damage.
- **Inkscape half**: node drags coalesce identically; damage re-rasterizes
  the vector content at each viewport's rung (doc 01's pull contract does
  the rest). The same content edited while embedded three compositions
  deep in a graded, faded nest updates everywhere via aggregate revisions
  (docs 05/13) — in-context editing needs no extra machinery.
- **Kdenlive half**: trims, moves, and transition inserts are placement/
  graph transactions — pure core records, no content involvement; playback
  picks up the new version at the next frame plan. Scrubbing while
  trimming works because transports are per-viewport (doc 11).
- **Autosave**: pin, serialize (doc 08) on a background thread, unpin.
  Canonical output + a consistent version = safe, diff-friendly, pause-free.

## What stays out of the core

Selection, tool state, per-editor UI (rulers, guides, snapping): host
territory, keyed by `ObjectId`s. Multi-user collaboration: deferred
(hooks: ids + journal). Branching/forking history beyond linear
undo/redo: the version model trivially permits it; deliberately not API
until a host needs it.

## Scheduling decision

**Decision: full editing model in v1.** Versioned `DocState` with atomic
publish, transactions with gesture coalescing, the document-wide journal
with undo/redo, and the `Editable` facet are all v1 contract and
implementation, with `org.arbc.raster`'s persistent tile table as the
reference proof of the capture discipline. Rationale: the version
machinery is required by v1 regardless (offline snapshots, the audio
lookahead, autosave consistency), and the capture/mutate/damage discipline
must be baked into every kind's mutation API from its first version —
retrofitting it is a rewrite of exactly the code plugin authors copy from
the reference kinds.

## Cross-doc impact

- Doc 01's "identity and versioning" section is realized by `ObjectId` +
  `DocState`; revisions become version-monotonic as before.
- Doc 02's "single-writer / snapshot fence" threading model is unchanged
  in shape; the fence is now a pinned persistent structure rather than an
  abstract token.
- Doc 03 gains the `Editable` facet (null-default, like audio) and the
  render-purity refinement.
- Doc 08 serialization reads a pinned version; load constructs a fresh
  document at version 0.
- Doc 10's contract tests gain capture/restore/damage suites; the tiled
  CoW raster state is part of `org.arbc.raster`'s brief.
