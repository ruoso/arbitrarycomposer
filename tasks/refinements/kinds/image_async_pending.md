# kinds.image_async_pending — `org.arbc.image` gains the true pending state

## TaskJuggler entry

[`tasks/55-kinds.tji:34-39`](../../55-kinds.tji):

```tji
task image_async_pending "org.arbc.image true pending state for deferring AssetSource" {
  effort 2d
  allocate team
  depends !image
  note "Give org.arbc.image the true PENDING state for a deferring AssetSource (a
        network/content-store source that does not fire on_ready inline). Today such a
        source resolves to UNAVAILABLE (Decision 5 in tasks/refinements/kinds/image.md).
        Mint the content immediately, install the decoded pyramid later on the writer
        thread with a revision bump and a damage route naming the image content, so doc
        02's Refine step replaces the empty layer live — reusing PendingExternalLoads /
        ExternalCompositionLoader's three-state machine
        (src/runtime/arbc/runtime/external_composition_loader.hpp:81-95, where pending vs
        unavailable is decided by whether on_ready fired inside request(), never by empty
        bytes). Requires a pixels-arrive-later install channel on the plugin content.
        Source-of-debt: tasks/refinements/kinds/image.md (Decision 5), commit kinds.image.
        Docs 03/05/08."
}
```

## Effort estimate

**2d.** Every mechanism this task needs already exists and is claim-proven; the work is
wiring an *asset*-shaped arrival into machinery built for a *composition*-shaped one, and
retiring one invariant on the plugin content.

- The three-state machine, the durable pending state, the mutex-guarded arrival queue, the
  `weak_ptr` teardown guard, the writer-thread settle loop, and the frame-loop driver were
  all shipped by `runtime.async_external_load` and are reused *as-is*
  (`src/runtime/arbc/runtime/pending_external_loads.hpp`,
  `src/runtime/document_serialize.cpp:565-594`,
  `src/runtime/host_viewport.cpp:94-105`).
- The decode path, the resolved-URI-keyed pyramid cache, and the `decodes_issued()`
  instrument were shipped by `kinds.image`
  (`plugins/image/arbc/kind_image/image_content.hpp:112-127`).
- What is genuinely new is small: an asset-side loader (~120 lines mirroring
  `ExternalCompositionLoader`), one new `Content` virtual, and one atomic on
  `ImageContent::d_pyramid`.

*If Decision 6 is reversed — if the late install may not publish atomically and
`render_thread_safe()` must become `false` — this is a 4d task, because it drags the
per-content render serialization queue back onto a kind that was deliberately built without
one.*

## Inherited dependencies

### Settled (may be relied on)

- **`kinds.image`** (`complete 100`, `tasks/55-kinds.tji:27-33`, refinement
  [`image.md`](image.md)). Ships `org.arbc.image` as the out-of-lib `arbc-plugin-image`
  MODULE. This task extends, and does not reopen, its Decisions 1, 2, 3, 4 and 7 — see
  *Predecessor decisions that bind*. Its **Decision 5** is the debt this task pays.
- **`runtime.async_external_load`** (`complete 100`, `tasks/65-runtime.tji:110-116`,
  refinement [`../runtime/async_external_load.md`](../runtime/async_external_load.md)).
  Ships the entire deferral substrate. Not a formal `depends` edge in the `.tji` (the WBS
  chains this task off `!image` only), but every seam below is its output, and it is
  already complete, so the dependency is satisfied in fact. **Do not re-derive its
  design — mirror it.**
- **`runtime.host_viewport_document_binding`** (`complete 100`,
  `tasks/65-runtime.tji:117-123`). `derive_document_config` auto-wires
  `HostViewport::Config::settle_external_loads` from a `Document&`, so an asset arrival
  inherits a production driver with **zero new wiring** (Decision 8).
- **`model.per_object_revision`** is *not* required: the arrival's revision bump is the
  ordinary document-wide one (Decision 5).

### Pending (must NOT be assumed)

- **`kinds.image_master_budget`** (`tasks/55-kinds.tji:40-45`, M9). It will evict decoded
  pyramids under a byte budget, which *generalizes* the publish discipline this task
  establishes (Decision 6 states the tension explicitly and hands it the
  `install_asset` seam). Do not build eviction here, and do not weaken this task's
  publish-once invariant in anticipation of it.
- **`runtime.interactive_pull_wiring` / `runtime.interactive_binder_wiring`** — irrelevant
  here. `org.arbc.image` is a leaf kind with no operator inputs and no `PullService`
  edge, so unlike `runtime.async_external_load` this task can prove its byte-exact pixels
  on the interactive path directly. It need not route the proof through the offline
  renderer.

## What this task is

Today a `LoadContext` whose `AssetSource` does not answer *inside* `request()` makes every
`org.arbc.image` layer permanently **unavailable**: `codec_image.cpp:103-154` hands the
plugin an empty byte buffer, and `make_image_content` reads empty bytes as absence
(`image_content.hpp:234-238`). The bytes may land a millisecond later and nothing happens —
the content has no channel to receive them, and no one is listening.

This task gives `org.arbc.image` the **third** state its nested sibling already has:

- **RESOLVED** — the source answered inside `request()` with bytes that decode. Pixels are
  present at revision 0, exactly as today.
- **UNAVAILABLE** — the source *answered* with empty bytes, or the bytes do not decode, or
  no `AssetSource` is installed at all. Null pyramid, empty bounds, renders nothing, ref
  preserved verbatim. Exactly as today.
- **PENDING** — the source has **not answered yet**. The content is minted immediately, in
  the same empty-bounds shape as unavailable, and the parent document finishes loading at
  revision 0 without waiting on any fetch. When the bytes arrive, they are marshalled onto
  the writer thread, decoded into the pyramid, published into the live content, and the
  commit that publishes the new revision flushes damage naming the image content — so doc
  02's *Refine* step (02:79-81) turns the arrival into a follow-up frame and the empty
  layer is replaced by the photograph, live.

Pending and unavailable are separated by **whether the source answered**, never by the
bytes being empty — the rule `external_composition_loader.hpp:93-95` already states, and
whose violation it names as a real past bug ("conflating them is the bug that made every
deferring source report a permanent placeholder no matter how fast its bytes arrived").
`kinds.image` Decision 5 shipped exactly that conflation, knowingly, and named this task as
the fix.

It is **not**:

- a byte-budgeted pyramid cache (that is `kinds.image_master_budget`);
- a production deferring `AssetSource`. None ships. `FilesystemAssetSource` stays
  inline-firing (`filesystem_asset_source.hpp:27`) — a local file read is not a blocking
  wait. The deferring source is a **test double**, exactly as
  `runtime.async_external_load` Decision 8 settled;
- a change to the core split of labour. The core still resolves the URI and fetches the
  bytes; the plugin still owns bytes → pixels and touches no file
  (doc 08:145-153, doc 17:259-262). `kinds.image` Decision 5's *ownership boundary* is
  correct and is preserved verbatim; only its *two-outcome* treatment of the fetch is
  wrong;
- an `Editable` facet, a mutable pyramid, or any weakening of the read-only shape. The
  pyramid is still immutable; it merely becomes *published* rather than
  *constructed-in-place* (Decision 6).

## Why it needs to be done

**The kind is unusable over any non-local source until it lands.** `org.arbc.image`'s whole
reason for existing is that a photograph lives in a file and the document stores only a URI
(doc 08 Principle 3, the ~490 MB → ~32 MB argument at 08:354-372). The moment that file
lives behind a content store, an object store, or a network mount — the schemes doc
08:116-118 explicitly says "plug in later" — every image in the project renders as nothing,
permanently, with no error and no recovery. The kind silently degrades to a no-op in exactly
the deployment its design anticipates.

**It is the last conflation of the two states in the codebase.** `runtime.async_external_load`
fixed it for external *compositions* and left a claim on the register saying so
(`05-recursive-composition#deferred-external-child-installs-live`). Assets went through the
*same* `LoadContext` seam (doc 08:34-36: "one resolution seam serves both") but kept the old
two-outcome reading. That asymmetry is not a design position — it is unpaid debt, recorded
as such in `codec_image.cpp:91-98` and in `kinds.image` Decision 5.

**It unblocks `kinds.image_master_budget`.** Eviction and re-decode need a channel to push
pixels into a live content after construction. This task builds that channel
(`Content::install_asset`) and pins its discipline; the budget task inherits it rather than
inventing a second one.

## Inputs / context

### Governing design-doc sections (normative — doc 16)

- **doc 02:79-81** — *Step 6, Refine*: "Async results that arrive later produce damage for
  their region, scheduling a follow-up frame." The arrival's damage is what makes the
  photograph appear without the user touching anything.
- **doc 02:50-51** — *Step 1, Collect damage*: "no damage → no work". This is why the
  arrival's commit **must** carry damage: a bare revision bump would leave the frame loop
  asleep and the image invisible until something unrelated damaged the scene.
- **doc 02:255-284** — the tile cache key is `(content id, revision, scale rung, tile
  coords)`. This is why the arrival must **also** bump the revision, not merely damage:
  without a new revision the parent's composed-result tiles stay hits.
- **doc 05:61-83** — the pending path, and the definitive statement of the split:
  "*Pending* and *unavailable* thus differ in exactly one bit, and only for the loader…
  The fetch may run on any thread; the install and its damage are marshalled onto the
  single writer thread that owns the model. **Loading a file is async — mutating the
  document is not.**" Written for compositions; this task applies it to assets, which is
  the delta doc 08 records.
- **doc 08:116-134** — Principle 3: resolve through `LoadContext`, dedup on the **resolved**
  URI, round-trip the **authored** one; an unloadable reference is *unavailable*, never a
  read error.
- **doc 08:135-144** — the extent carve-out: an asset-referencing leaf kind with no
  intrinsic extent "reports empty bounds and renders nothing", because "fabricating an
  extent so a placeholder could be drawn would let a *missing* file change the
  composition's geometry." **A pending image is extent-less for the same reason**, and
  *gains* its geometry at the install — which is precisely doc 02's Refine step.
- **doc 08:145-153** — "The core fetches asset bytes; the kind only decodes them… An
  asset-referencing kind never performs file I/O of its own." Unchanged by this task.
- **doc 17:252-262** — "The codec line is a *decoder* line." The codec stays in `runtime`;
  the decoder stays in `arbc-plugin-image`. This task adds no include to the plugin.
- **doc 16:54-62** — behavioral counters, never wall-clock. Arrival is scheduled *by the
  test*.
- **doc 16:67-73** — TSan on the full suite; dedicated stress tests for publish protocols.

### Design-doc deltas (this task, same commit — doc 16's rule)

Three, detailed under *Design-doc deltas* below: `docs/design/08-serialization.md`
(Principle 3 gains the asset-side three-state and the arrival's shape),
`docs/design/03-layer-plugin-interface.md` (`install_asset()`, beside `external_asset_ref()`
at 03:131-141), and `docs/design/00-overview.md` (a decision-record bullet, because the
delta retires a thread-safety argument doc 03 currently rests on).

### Source seams this task extends

**The debt, stated in the source it lives in:**

- `src/runtime/codec_image.cpp:91-98` — "*V1 IS SYNCHRONOUS… A DEFERRING source simply has
  not fired: the buffer is empty, and v1 reads that as UNAVAILABLE… The true pending state
  … is `kinds.image_async_pending`; it needs a pixels-arrive-later install channel that
  `Content` does not have today.*" This comment is deleted by this task.
- `src/runtime/codec_image.cpp:123-129` — the fetch as it stands: `ctx.resolve` →
  `ctx.resolved_uri` → `ctx.load_asset(ref, [bytes](std::string_view fetched){ … })`. The
  buffer is a `shared_ptr` precisely so a late fire is *harmless* — but its lateness is
  then silently read as absence. That lambda is what this task reroutes.
- `src/runtime/codec_image.cpp:131-144` — the three-part `ContentConfig` frame
  (`"<authored>\n<resolved>\n<encoded-bytes>"`). Preserved verbatim; a pending content is
  minted with **empty bytes**, which is the same wire shape unavailable already uses.
- `src/runtime/codec_image.cpp:158-167` — `Codec image_codec(const Registry&)`. Gains a
  second parameter (Decision 2), mirroring `nested_codec(ExternalCompositionLoader*)`
  (`src/runtime/arbc/runtime/builtin_codecs.hpp:95-96`).

**The substrate to reuse, unchanged:**

- `src/runtime/arbc/runtime/external_composition_loader.hpp:80-116` — the three-state
  contract, in prose, including the exact rule this task ports
  (`:93-95`). `:117` `load()`, `:133` `settle()`.
- `src/runtime/external_composition_loader.cpp:51-53` — **allocate before fetch**:
  `allocate_id()` → `record(uri, child)` → `add_pending(child, uri, depth)`.
- `src/runtime/external_composition_loader.cpp:59-64` — the `weak_ptr` callback: it
  captures *no* `Document`, *no* `Model`, *no* loader, and calls only
  `PendingExternalLoads::complete`.
- `src/runtime/external_composition_loader.cpp:70-76` — **the pending/inline probe**:
  `if (!d_state->take_arrival(child, bytes)) { return child; }`. Routing even the *inline*
  answer through the mutex-guarded queue is what makes "did it answer?" race-free against a
  source answering from another thread while `request()` is still returning. **Copy this
  exactly.**
- `src/runtime/arbc/runtime/pending_external_loads.hpp:44-114` — the durable state:
  `set_source`/`source` (`:64-65`), the resolved-identity map (`:72-73`), the pending
  entries (`:80-82`), and the any-thread queue (`:89` `complete`, `:96` `take_arrival`,
  `:99` `take_ready`). The mutex guards **only** the queue (`:16-23`).
- `src/runtime/document_serialize.cpp:402-504` — `LoadAssembly`, the helper that rebuilds
  `{CodecTable, ContentSink, loader}` for **both** load and settle so the two cannot drift.
  Its sink already records `d_minted[ptr] = id` (`:491`, `:500`) — the seam Decision 3 hangs
  the id-binding on.
- `src/runtime/document_serialize.cpp:517-525` — `arrival_damage`, the reverse-lookup damage
  route (`composition_ref() == child` → `Damage{id, Rect::infinite(), TimeRange::all()}`).
  The asset arm needs the *same shape* but not the same lookup (Decision 4).
- `src/runtime/document_serialize.cpp:565-594` — `settle_external_loads(doc, bridge,
  registry)`: `JournalSuspension` (an arrival is not an undoable edit), loop to quiescence,
  rebuild `LoadAssembly` per round.
- `src/runtime/arbc/runtime/document.hpp:264` — `pending_external_loads()`, the counter.
- `src/runtime/host_viewport.cpp:94-105` — the driver: settle runs at **step 0**, before the
  pin and before the damage drain, "so the frame that settles an arrival is the frame that
  composites it". Hook at `src/runtime/arbc/runtime/host_viewport.hpp:105`; counter
  `external_loads_settled()` at `:225`.
- `src/model/model.cpp:1640-1691` — `Transaction::commit()` publishes **unconditionally**:
  one atomic store, one revision increment (`:1684-1687`), then one damage flush
  (`:1689-1691`). A transaction that mutates nothing and only calls `add_damage`
  (`model.hpp:497`) therefore publishes exactly one revision and flushes exactly one damage
  batch. **This is the entire model-side arrival** (Decision 5).

**The plugin content to extend:**

- `plugins/image/arbc/kind_image/image_content.hpp:179-220` — `class ImageContent`.
  `:186` the ctor `(authored_uri, PyramidPtr)`; `:194` `bounds()`; `:204`
  `render_thread_safe() { return true; }`; `:208` `external_asset_ref()`; `:211`
  `available()`; `:217-219` the members — `std::string d_uri; PyramidPtr d_pyramid;
  std::shared_ptr<TileStore> d_tiles;`. **`d_pyramid` is a plain `shared_ptr`, neither
  atomic nor mutex-guarded**, and `:53-54` / `:63-68` justify `render_thread_safe()` by its
  being *immutable after construction*. That is the invariant Decision 6 amends.
- `plugins/image/arbc/kind_image/image_content.hpp:112-127` — `class PyramidCache`, keyed by
  **resolved** URI, `weak_ptr` entries, mutex-guarded, with `decodes_issued()` (`:121`) as
  the behavioral instrument. `:132` `default_pyramid_cache()`.
- `plugins/image/image_content.cpp:127-149` — `PyramidCache::resolve`; `:146`
  `++d_decodes_issued`, **only on a genuine decode**. A second content resolving to a
  resident URI bumps it zero. This is what makes a pending-then-settled image cost exactly
  one decode, and N contents on one URI exactly one decode between them.
- `plugins/image/image_content.cpp:203-209` — `bounds()`; `:205` an unavailable image is
  `Rect{}` (**empty**, not `nullopt` — `nullopt` means *unbounded*). Empty bounds culls the
  content. A pending image is the same.
- `plugins/image/image_content.cpp:211-286` — `render()`; `:214-221` the unavailable path
  (`done->fail(RenderError::ResourceUnavailable)`); `:283-284` the provided surface.
- `plugins/image/image_content.cpp:302-324` — `make_image_content`; `:321` the
  `default_pyramid_cache().resolve(resolved, encoded)` call. **The resolved URI is used here
  and then discarded** — Decision 6 keeps it.

**The contract:**

- `src/contract/arbc/contract/content.hpp:643` — `virtual std::string_view
  external_asset_ref() const { return {}; }`, the read-back channel `kinds.image` Decision 3
  added. `install_asset()` lands beside it (`:607` `composition_ref`, `:623`
  `external_composition_ref` are its siblings).
- `src/contract/arbc/contract/registry.hpp:35` — `ContentConfig` is an opaque, kind-defined
  `string_view`. Unchanged: this task adds **no** ABI parameter to `ContentFactory`, which
  `kinds.image` Decision 5 explicitly rejected doing.
- `src/serialize/arbc/serialize/load_context.hpp:31-39` — `AssetSource::request(resolved_uri,
  on_ready)`; `:88-94` `set_asset_source` / `load_asset`. **Unchanged.**
- `src/serialize/load_context.cpp:99-104` — no source installed ⇒ `on_ready` fires
  *inline* with empty bytes. So "no source" stays **unavailable**, never pending — the
  probe gets its answer inside `request()`.

### Predecessor decisions that bind

From [`image.md`](image.md):

- **Decision 1** (provided surface covers the requested region at the achieved scale) —
  untouched. The render path does not change.
- **Decision 2** (the codec is a `runtime` TU reaching the kind only through
  `Registry::factory`, never naming `ImageContent`) — **binding, and constraining**. The
  codec still may not name the plugin type, so the install channel must be a
  *contract-level virtual*, not a downcast (Decision 7).
- **Decision 3** (`external_asset_ref()` on `Content`) — the precedent for adding
  `install_asset()` in the same place, on the same terms.
- **Decision 4** (an immutable mip pyramid; no CoW, no `StateHandle`, no versioning; hence
  `render_thread_safe() == true`) — **preserved, with its justification amended**
  (Decision 6). The pyramid object stays immutable; the *pointer to it* publishes once.
- **Decision 7** (an unavailable image has empty bounds and renders nothing) —
  **inherited by the pending state verbatim.** A pending image is extent-less for the same
  reason an unavailable one is: the intrinsic size is knowable only by decoding.

From [`../runtime/async_external_load.md`](../runtime/async_external_load.md): Decisions 1
(pending is not a new type), 3 (damage rides the install's own commit), 4 (`on_ready`
enqueues; the writer thread installs), 6 (`shared_ptr` state, `weak_ptr` callbacks), and 8
(no production deferring source) are adopted wholesale and are **not re-litigated** below.
Its Decision 5 (per-entry depth) has no analogue: an asset fetch does not recurse.

## Constraints / requirements

1. **The pending/unavailable split is decided by whether `on_ready` fired inside
   `request()` — never by the bytes being empty.** A source that *answers* with empty bytes
   yields **unavailable**; a source that has not answered yields **pending**. The question
   must be asked of the mutex-guarded arrival queue, not of a captured stack flag, so it is
   race-free against a source answering from another thread
   (`external_composition_loader.cpp:70-76`).

2. **A load never blocks on a fetch.** `load_document` returns at revision 0 with every
   pending image minted, exactly as it does today with every pending nested child.

3. **`on_ready` touches no `Model`, no `Document`, no `LoadContext`, and no `Content`.** It
   copies the bytes into the queue under its mutex and returns. It may run on any thread,
   at any time, including after the `Document` is destroyed — the closure captures a
   `weak_ptr` and drops its bytes if the lock fails.

4. **The install and its damage happen on the writer thread, in one commit.** One
   `Model::Transaction` per settled arrival: the damage naming each awaiting image content
   is unioned in, and `commit()` publishes exactly one revision and flushes exactly one
   damage batch (`model.cpp:1684-1691`). Never two transactions; never a revision with an
   empty damage set (doc 02:50-51).

5. **The decoded pyramid publishes atomically, at most once, and never reverts.** A worker
   rendering a previous frame may observe the content concurrently with the install. It must
   see either the empty state or the fully decoded one — never a torn pointer, never a
   partially built pyramid. `render_thread_safe()` stays `true`.

6. **Exactly one decode.** A pending-then-settled image issues exactly one decode. N image
   contents whose authored refs resolve to one URI issue exactly one *fetch* and exactly one
   *decode* between them, whether they resolve inline or defer
   (`03-layer-plugin-interface#image-decodes-once-per-resolved-uri`, doc 08:116-122).

7. **The resolved path is byte-for-byte unchanged.** An inline-firing `AssetSource`
   (`FilesystemAssetSource`, and *any* source in the fuzz/offline lanes) must produce the
   identical content, the identical pixels, the identical revision count and the identical
   saved bytes as before this task. The pending path is *additive*; it must not cost the
   common case a single extra revision or transaction.

8. **Save-while-pending is a fixed point.** A document saved while an image is pending is
   byte-identical to the same document saved with the image *loaded*, and with the image
   *missing*: `params` is exactly `{"source": "<authored-uri>"}` and nothing else. The new
   state must not leak into the format (doc 08 Principle 3, Constraint 4/5 of
   `kinds.image`).

9. **Unavailable stays free.** An arrival carrying empty or undecodable bytes changes
   nothing observable, so it opens **no** transaction, publishes **no** revision and flushes
   **no** damage. It only drops the pending entry.

10. **Levelization (doc 17, CI-enforced by `scripts/check_levels.py`).** The asset loader and
    the settle arm are `runtime` (L5). The new `Content` virtual is `contract` (L3) and must
    name **no** serialize or JSON type — the same trap `kinds.image` Decision 5 identified
    for widening `ContentFactory`. `arbc-plugin-image` gains **no new include**: it already
    reaches `<atomic>` and its own headers, and it still performs no file I/O and never sees
    a `LoadContext`.

11. **Errors are values; no exception crosses the plugin boundary.** Undecodable arriving
    bytes yield `false` from `install_asset` and leave the content unavailable — never a
    throw, never a read error.

## Acceptance criteria

### Conformance suite (mandatory for a content kind — doc 16)

`tests/image_conformance.t.cpp` re-runs **unchanged** as the gate: `arbc::contract_tests`
constructs its contents through `make_image_content` and never calls `install_asset`, whose
`Content` default is `return false`. A green unchanged conformance run is the evidence that
the new virtual is genuinely additive and that the read-only shape (`Static`,
`time_extent() == nullopt`, `editable() == nullptr`, no audio facet, no inputs) is
untouched. Re-enforces, with second `enforces:` tags, the contract claims that run already
gates — in particular `03-layer-plugin-interface#render-pure-over-pinned-state` and
`#static-time-invariant`, both of which must survive a content whose pixels can now appear
after construction.

### New claims-register entries (`tests/claims/registry.tsv`)

Three rows, each pinned by an `enforces:`-tagged test:

- **`08-serialization#pending-asset-installs-live`** — An `AssetSource` that does not answer
  inside `request()` leaves an `org.arbc.image` content PENDING, not unavailable: the parent
  document loads successfully at revision 0, the layer is present with its authored ref
  preserved and empty bounds, and it renders nothing without faulting. When the source later
  answers with decodable bytes, a writer-thread settle publishes EXACTLY ONE new revision
  whose commit flushes EXACTLY ONE damage batch naming the image content (whole-object,
  all-time), the content's bounds become the decoded extent, and the next frame composites
  pixels byte-identical to the same document loaded through an inline-firing source — doc
  02's Refine step, driven by an ordinary revision bump and a damage route.

- **`08-serialization#pending-asset-is-not-unavailable`** — Pending and unavailable are
  separated by WHETHER THE SOURCE ANSWERED, never by the bytes being empty: a source that
  answers inline with empty bytes yields an UNAVAILABLE content (no pending entry, no later
  revision, no damage), a source with no `AssetSource` installed at all yields the same
  (`load_context.cpp:99-104` fires inline), and a source that never answers leaves the
  content PENDING forever — where the document still renders, still saves byte-identically,
  and still closes cleanly, and an arrival firing after the `Document` is destroyed installs
  nothing and faults nothing.

- **`03-layer-plugin-interface#image-pyramid-publishes-once`** — An `org.arbc.image`
  content's decoded pyramid is published ATOMICALLY and AT MOST ONCE, monotonically (null →
  non-null, never reverting and never replaced), so `render_thread_safe()` remains true
  across a concurrent late install: a worker rendering a pinned earlier revision while the
  writer thread settles an arrival observes either the empty state (empty bounds, culled) or
  the fully decoded one, never a torn pointer or a half-built pyramid, and the arrival's
  damage guarantees a following frame observes the decoded one. Asserted under TSan with
  randomized schedule perturbation, never as a wall-clock assertion.

### Re-enforced claims (second `enforces:` tag — **no new row**)

- `03-layer-plugin-interface#image-decodes-once-per-resolved-uri` — extended across the
  async boundary: a pending-then-settled image issues **exactly one** decode
  (`decodes_issued()` delta of 1, not 2); two contents whose authored refs (`bg.png`,
  `./bg.png`) resolve to one URI and *both* defer share **one** in-flight fetch and **one**
  decode, and both are damaged by the single arrival.
- `08-serialization#unavailable-asset-is-not-a-read-error` — unchanged and still true; the
  new state does not turn any absence into an error.
- `08-serialization#image-serializes-as-uri-only` — save-while-pending is a fixed point
  (Constraint 8).
- `14-data-model-and-editing#commit-publishes-once` and
  `14-data-model-and-editing#damage-flushes-once-per-commit` — the damage-only arrival
  commit is an ordinary transaction and obeys both.
- `01-core-concepts#viewport-binds-to-document` — its "a deferred external arrival settles
  within the very step that observes it, dropping `pending_external_loads()` to zero" clause
  now covers a pending **asset** as well as a pending composition, with no change to
  `HostViewport` (Decision 8).
- `02-architecture#async-arrival-emits-damage`.

### `tests/image_async_pending.t.cpp` (new) — behavioral

A `DeferringAssetSource` double modelled on `tests/async_external_load.t.cpp:80-137`:
records `(uri, on_ready)` on `request()` and fires nothing; `fire(uri)` / `fire_all()` the
test calls when it chooses; counters `requests()` / `outstanding()`. Arrival is scheduled by
the test, so nothing is timing-dependent (doc 16:54-62). Plus a `RecordingDamageSink`
(`async_external_load.t.cpp:155-165`) so "the arrival's damage rides the install's own
commit" is checkable as *which flush carried what*.

1. **A pending image leaves a live, extent-less layer at revision 0.** Load with a deferring
   source. Assert: `load_document` **succeeds**; `revision() == 0`; the layer is present;
   `external_asset_ref()` is the authored spelling; `bounds()` is **empty** (`Rect{}`, not
   `nullopt`); a render faults nothing; `pending_external_loads() == 1`; `decodes_issued()`
   delta is **0**. — *pins* `08-serialization#pending-asset-installs-live`.
2. **Settling installs the pyramid on a new revision and damages the image content.**
   `fire_all()`, then settle. Assert: `revision()` is **> 0**; the document's root id and
   every pre-existing layer survive (proving an ordinary transaction, not a republished
   baseline); `bounds()` is now the decoded extent; the `RecordingDamageSink` recorded
   **exactly one** flush, whose damage set names the **image content** id with
   `Rect::infinite()` / `TimeRange::all()`; `pending_external_loads() == 0`;
   `decodes_issued()` delta is **exactly 1**. — *pins* the same claim.
3. **An answered-empty source is unavailable, not pending.** A source that fires **inline**
   with empty bytes: `pending_external_loads() == 0` immediately, the content is
   unavailable, and a later settle installs nothing and publishes **no** revision. Same for
   a `LoadContext` with **no** `AssetSource` at all. — *pins*
   `08-serialization#pending-asset-is-not-unavailable`; *re-enforces*
   `08-serialization#unavailable-asset-is-not-a-read-error`.
4. **A deferred arrival carrying empty bytes settles to unavailable, for free.** Defer, then
   `fire_all()` with **empty** bytes. Assert: the content stays unavailable (empty bounds),
   the pending count drops to 0, and **no revision is published and no damage is flushed**
   (Constraint 9) — the settle costs exactly one map erase. — *pins* the same claim.
5. **Undecodable arriving bytes settle to unavailable, not to an error.** Defer, fire with
   garbage. Same assertions as (4), plus: `install_asset` returned `false`, no throw crossed
   the plugin boundary, and `decodes_issued()` delta is 1 (a decode was genuinely
   attempted). — *pins* the same claim.
6. **Two authored spellings of one URI share one fetch and one decode.** Two layers,
   `bg.png` and `./bg.png`, both deferring. Assert: the source saw **exactly one**
   `request()`; one `fire_all()` settles **both**; `decodes_issued()` delta is **exactly
   1**; the single arrival's damage flush names **both** content ids. — *re-enforces*
   `03-layer-plugin-interface#image-decodes-once-per-resolved-uri`.
7. **A duplicate arrival installs once.** A source that answers **twice** for one URI: the
   second arrival finds nothing pending, installs nothing, and publishes no second revision
   (the `take_pending` idempotence `external_composition_loader.cpp:85-93` already relies
   on). — *pins* `08-serialization#pending-asset-installs-live`.
8. **A callback outliving its document is safe.** Destroy the `Document`, *then*
   `fire_all()`. No fault, no install (the `weak_ptr` fails to lock). Runs in the existing
   ASan lane. — *pins* `08-serialization#pending-asset-is-not-unavailable`.
9. **A pending image inside a pending nested child settles across rounds.** An external
   `.arbc` (deferring) that itself contains an `org.arbc.image` (deferring): the image's
   fetch cannot even be *issued* until the child's bytes are parsed, so the graph needs two
   settle rounds. Assert the settle loop reaches quiescence and both land. This is the one
   place the two arrival kinds interleave in the shared queue. — *pins*
   `08-serialization#pending-asset-installs-live`.
10. **A deferred image settles inside the frame that observes it.** Drive **only**
    `HostViewport::step()` (no direct `settle_external_loads` call). Assert:
    `pending_external_loads()` drops to zero, `external_loads_settled()` increments, a new
    revision publishes, and the frame that settled the arrival is the frame that composites
    it (Decision 8 — no new driver). — *re-enforces*
    `01-core-concepts#viewport-binds-to-document`.

### `tests/image_async_pending_golden.t.cpp` (new) — byte-exact

Computed-reference, in-TU, against the existing fixture
`plugins/image/t/fixtures/photo.ppm`. **Byte-exact, no tolerances** (doc 16:48-53).

1. **The empty layer is replaced by the real pixels, live.** Render before settle → matches
   the *unavailable* oracle (the layer contributes nothing; pixels byte-identical to the
   same scene with the file missing). `fire_all()`; settle; re-render → **byte-identical**
   to the same document loaded through an inline-firing `FilesystemAssetSource`. Not a
   hand-typed constant: the oracle is the same scene loaded synchronously, which is doc
   05:83's "loading a file is async — mutating the document is not" asserted at the pixel.
   — *pins* `08-serialization#pending-asset-installs-live`.
2. **The inline path is unchanged, byte-for-byte.** The four existing goldens of
   `tests/image_goldens.t.cpp` (native scale, a downscale rung, an `Exact` upscale, a
   `BestEffort` upscale) reproduce **byte-identically** through the new code path, and the
   inline load publishes the **same number of revisions** as before this task (Constraint 7).
   — *re-enforces* `16-sdlc-and-quality#byte-exact-goldens`.
3. **Save-while-pending is a fixed point.** Save while pending; assert byte-identical to
   saving **loaded** and to saving **missing**; `params` is exactly `{"source": "<authored>"}`
   — the authored spelling, never the resolved one. — *re-enforces*
   `08-serialization#image-serializes-as-uri-only`.

### `tests/image_concurrency_stress.t.cpp` (extended) — TSan / stress

The task introduces exactly one new cross-thread edge — a worker `render()` reading
`d_pyramid` while the writer thread publishes it — so it owes exactly one lane, and must say
so rather than inherit `kinds.image`'s "the cache is touched only at construction" premise,
**which this task retires**.

- **A settle racing in-flight worker renders.** N worker threads render an image content in a
  loop (pinned to an earlier revision) while the writer thread settles the arrival. Assert:
  TSan clean under randomized schedule perturbation (doc 16:67-73); every render observes
  either the empty state or the *complete* decoded pyramid (checked by comparing each
  non-empty result against the golden, so a torn read would produce a mismatch rather than
  merely a race report); the pyramid is published exactly once; `decodes_issued()` delta is
  1. — *pins* `03-layer-plugin-interface#image-pyramid-publishes-once`.
- **N deferring sources firing `on_ready` from N threads** while the writer settles, renders
  and saves in a loop. TSan clean; each URI fetched once; each content installed once.
- **An arrival racing `Document` teardown.** Fire callbacks from a worker while the main
  thread destroys the `Document`. ASan + TSan clean; no install after teardown.

### Regressions the task must flip (not add)

Two existing tests encode v1's conflation in their *comments and assertions* and must be
updated in the same commit, or they will assert the bug:

- `tests/image_serialize.t.cpp:231-235` — the `"no AssetSource installed at all"` section,
  whose comment claims "this is also what a DEFERRING source … looks like to v1". The
  section stays (no source ⇒ unavailable, still true — `load_context.cpp:99-104` fires
  inline), but the deferring-source clause is deleted and replaced by a pointer to the new
  test file.
- `tests/image_content.t.cpp:57` — same stale clause on the `"empty bytes … mean
  UNAVAILABLE"` case. The case stays (the *frame* still spells absence as empty bytes); the
  claim that a deferring source looks like this is deleted.

### Gates

- `scripts/check_levels.py` silent — the loader and settle arm are L5; the new virtual is a
  contract-level `string_view` accessor naming no serialize type.
- `scripts/check_claims.py` silent — all three new rows enforced.
- `scripts/check_rt_safety.py` silent.
- **≥90% diff coverage** on changed lines (doc 16:112-118), including the asset arm of
  `settle_external_loads`, the `install_asset` override, and the unavailable-arrival early
  return.
- `-Werror -Wpedantic`, `clang-format` clean; full suite green across gcc/clang ×
  debug/release/asan/tsan.
- **Fuzz seed**: `tests/fuzz/corpus/load_document/pending_image_asset.arbc` — a document
  whose `params.source` is never fired. A pending asset must not make the loader fault on
  hostile input. — *re-enforces* `08-serialization#loader-never-faults-on-hostile-input`.
- `tj3 project.tjp 2>&1 | grep -iE "error|warning"` silent after the `.tji` edit.

### Milestone

`m9_release` (`tasks/99-milestones.tji:72`) already lists `kinds.image_async_pending`. No
milestone edit is needed beyond `complete 100` on the task.

### Deferred follow-ups (closer registers in WBS)

**(none.)** The task closes its own scope: the settle arm rides the driver that already
exists (Decision 8), and the one forward tension — pyramid *eviction* re-opening the
publish-once invariant — belongs to `kinds.image_master_budget`, which is **already a
registered WBS leaf** (`tasks/55-kinds.tji:40-45`, M9) and which Decision 6 hands the
`install_asset` seam to. No new leaf is owed.

*Caution, from the predecessor's experience:* `runtime.async_external_load` also wrote
"(none.)" here and then produced a real implementer deviation (its Decision 7's frame-loop
call site became a `HostViewport` hook) that had to be paid off by a whole follow-up task.
The equivalent risk here is Decision 3's id-binding seam: if `LoadAssembly`'s sink turns out
not to be the right place to bind the content's `ObjectId`, say so in the Status block and
register the follow-up rather than quietly widening the contract.

## Decisions

### Decision 1 — Pending is the *unavailable shape* plus a live pending entry, not a new content state

An `ImageContent` minted while its bytes are in flight is constructed **exactly as an
unavailable one is**: authored URI kept, null pyramid, empty bounds, renders nothing. The
`ContentConfig` frame it receives carries empty bytes — the same wire shape unavailable
already uses (`image_content.hpp:234-238`). The *only* thing that distinguishes pending from
unavailable is an entry in the `Document`'s `PendingExternalLoads`, which lives entirely in
`runtime` and which the content never sees.

**Rationale.** This is `runtime.async_external_load` Decision 1 ("pending is a valid child id
with no record, not a new state") transposed. There, the state was already renderable because
`NestedContent` resolves a missing composition record to the placeholder. Here it is already
renderable because `kinds.image` Decision 7 made an extent-less image render nothing — a
decision taken for an unrelated reason (doc 08:135-144: a missing file must not change the
composition's geometry) that turns out to give pending its rendering behavior for free. The
kind was written for this state a task before it could occur.

It also means the *pixel* difference between pending and unavailable is **nil**, which is
correct: the user of a document whose photograph is still downloading sees the same thing as
the user whose photograph is missing — an empty layer that keeps its reference. The
difference is only that one of them will fill in.

**Rejected — a `PendingImageContent` placeholder type**, swapped for the real content on
arrival. It would have to mutate `Document`'s content map under a live binding, would lose
the authored ref and the object identity the damage route names, and would give identical
pixels. `runtime.async_external_load` Decision 1 rejected exactly this, and
`nested_external_ref` Decision 6 rejected it before that.

**Rejected — reporting `bounds()` as some fabricated "loading" rectangle** so a spinner could
be drawn. Doc 08:135-144 forbids it in as many words: fabricating an extent lets a
not-yet-arrived file change the composition's geometry, and the geometry would then *change
again* when the real extent arrives. A host that wants a loading indicator draws it in host
UI, where it belongs, driven by `pending_external_loads()`.

### Decision 2 — An `ExternalAssetLoader`, sibling to `ExternalCompositionLoader`, borrowing the same durable state

New `runtime` type (`src/runtime/external_asset_loader.{hpp,cpp}`), constructed per load
**and per settle** by `LoadAssembly`, borrowing the `Document`-owned
`shared_ptr<PendingExternalLoads>` exactly as its composition sibling does
(`external_composition_loader.hpp:69-71`). Its one entry point:

```cpp
struct FetchedAsset {
  std::string resolved;   // the identity the pyramid cache dedups the decode on
  std::string bytes;      // empty when pending, or when the source answered absence
  ObjectId pending;       // valid iff the source has not answered yet
};
FetchedAsset fetch(LoadContext& ctx, std::string_view authored);
```

It resolves through the one seam, dedups the **fetch** by resolved URI, mints a fetch id from
`Model::allocate_id()` **before** issuing the request, registers the pending entry, issues
`ctx.load_asset` with a `weak_ptr`-capturing callback that calls only
`PendingExternalLoads::complete(fetch_id, bytes)`, and then probes the queue with
`take_arrival(fetch_id, bytes)` — pending iff the probe comes back empty.

`Codec image_codec(const Registry&, ExternalAssetLoader*)` gains the loader pointer, mirroring
`nested_codec(ExternalCompositionLoader*)` (`builtin_codecs.hpp:95-96`). A **null** loader
means "no deferral machinery" and the codec falls back to today's direct
`ctx.load_asset` — so `image_codec(registry)` keeps working standalone and the codec remains
independently testable.

**Rationale.** The fetch id is drawn from the *same* `Model::allocate_id()` monotonic space as
composition child ids, so asset and composition arrivals can share one `Arrival` queue with no
key collision and no discriminant on the wire — settle asks the asset map first, the
composition map second. That is what lets the entire queue, mutex, `weak_ptr` guard and
loop-to-quiescence settle be reused rather than duplicated.

Dedup lives on the **fetch**, not on the content: two layers spelling one URI two ways issue
one `request()` and one decode, which is what `#image-decodes-once-per-resolved-uri` already
promises and which a naive per-content fetch would break the moment both defer.

**Rejected — teaching `ExternalCompositionLoader` to fetch assets too.** Its `load()` parses
bytes as an `.arbc` and installs a composition graph; an asset install decodes pixels and
touches no graph. Merging them would put an `if (is_asset)` through every step of a function
whose comment block is a careful argument about *composition* recursion, depth caps and
knot-cutting — none of which an asset has (an asset does not recurse, so there is no depth to
carry, which is why this task has no analogue of that task's Decision 5).

**Rejected — a second `PendingExternalLoads`, private to assets.** Two arrival queues means
two mutexes, two `weak_ptr` guards, two settle loops and two teardown races to prove, to
express one durable map with two value shapes. It would also break test 9 (a pending image
inside a pending nested child), where the two arrival kinds must interleave in one settle
loop.

### Decision 3 — The content's `ObjectId` is bound to its pending entry by the sink, after minting

The codec mints the content but does not know its `ObjectId` — the `ContentSink` assigns one
(`document_serialize.cpp:491`). So the pending entry is created in the codec keyed by the raw
`Content*`, and `LoadAssembly`'s sink — which **already** records `d_minted[ptr] = id`
(`:500`) — binds the real `ObjectId` onto the entry the moment it is known. The durable entry
keys on the `ObjectId` thereafter; the raw pointer is an in-load, same-thread key that is
never dereferenced and never outlives the load.

**Rationale.** The damage route must name the *content*, so the content's id is the one
durable fact the entry needs, and the sink is the exact and only point where minting meets
identity. `LoadAssembly` already exists to keep load and settle from drifting, and it already
maintains this map for an unrelated reason (the operator input children); this is one line in
a lambda that is already there.

The `Content*` never crosses a thread: the codec, the sink and the bind all run on the load
thread, and by the time `on_ready` may fire on another thread the queue key is the fetch id,
never the pointer. A pointer whose content was never sunk (a load that failed downstream) is
simply never bound, and its arrival finds no binding and drops — the same "nothing pending"
idempotence `settle` already relies on.

**Rejected — allocating the content's `ObjectId` before the parse**, as
`ExternalCompositionLoader` does for compositions (`external_composition_loader.cpp:51-53`).
That works there because `load_composition` *accepts* a pre-seeded root id. `Document::add_content`
has no such overload, and adding one — a public API to mint a content under a caller-chosen id
— is a far larger and more dangerous seam (id collisions, partially-constructed contents in
the map) than a private map from a pointer the runtime already holds.

**Rejected — a new contract accessor `resolved_asset_ref()`** so settle could reverse-look-up
awaiting contents by resolved URI, the way `arrival_damage` does for compositions
(`document_serialize.cpp:517-525`). It would put *resolution state* — a property of the load,
not of the document — on the persistent content interface, and doc 08 Principle 3 is
emphatic that only the authored spelling is the content's business. The pending entry already
knows which contents await it; it does not need the contents to know.

### Decision 4 — The damage route names the image content directly, from the pending entry

Each pending asset entry carries the list of `ObjectId`s awaiting it (usually one, more when
several layers share a URI). On arrival, settle emits
`Damage{content_id, Rect::infinite(), TimeRange::all()}` for each — the same absorbing,
structural shape `arrival_damage` uses (`damage.hpp:64-73`), unioned into the arrival's own
commit.

**Rationale.** An image *is* the damaged object; there is no embedding indirection to reverse
(a nested arrival must ask "who embeds this child?", because the child composition is not
itself a content). The entry already had to track which contents await the fetch in order to
install into them, so the damage list is that same list, free.

`Rect::infinite()` / `TimeRange::all()` and not the decoded extent: the image's bounds were
**empty** and are now non-empty, so the region that changed is not expressible in the *old*
geometry at all. The absorbing shape is the honest answer, and it is what a structural change
already uses everywhere else.

### Decision 5 — The arrival's model-side commit is **damage-only**

Unlike a nested arrival, which installs a composition graph, an image arrival mutates **no
model state at all**: the decoded pyramid is plugin-owned and deliberately unversioned
(`kinds.image` Decision 4 — no CoW, no `StateHandle`, no pool backing). So the settle opens
one `Model::Transaction`, calls `add_damage` (`model.hpp:497`) once per awaiting content, and
commits. `Transaction::commit()` publishes unconditionally — one atomic store, one revision
increment (`model.cpp:1684-1687`), then exactly one damage flush (`:1689-1691`) — so a
damage-only transaction is precisely "one new revision carrying the reason to re-render",
which is what doc 02 asks of every publish.

**The revision bump is not decoration.** The tile and composed-result caches are keyed on
revision (doc 02:255-284, doc 05:129-144). Damage alone wakes the frame; the *revision* is
what stops the parent composition's composed tiles from being served as hits. Both are
needed, and one commit gives both.

**Ordering: install the pyramid, then commit.** Under `HostViewport::step()` the settle runs
at step 0, *before* the pin (`host_viewport.cpp:94-105`), so no worker of the current frame
can straddle the transition. Only workers still in flight from a *previous* frame can observe
it, and Decision 6's atomic, monotonic publish makes that benign in both orders — they see
either the empty state (culled) or the complete pyramid, and the arrival's damage guarantees a
following frame sees the latter.

**Rejected — a mutation to make the commit "real"** (e.g. re-setting the content's inert
`StateHandle` so the transaction touches a record). It buys nothing the damage-only commit
does not already give, and it risks the live tripwire `kinds.image` Decision 4 called out:
`model.cpp:699-706` asserts that a persisted `StateHandle` on a non-editable content is inert,
and is "armed to catch exactly this change."

**Rejected — two commits (install, then damage).** It publishes an intermediate revision whose
damage set is empty despite structurally changing what the frame must draw, and makes "the
photograph appears" depend on an ordering argument between two commits rather than on
atomicity. `runtime.async_external_load` Decision 3 rejected the same thing.

**Rejected — no commit at all, relying on damage alone.** The frame would wake and re-plan,
and the composed-result cache would hand back the stale empty tiles, because their key did not
change.

### Decision 6 — The pyramid publishes atomically, once; Decision 4 of `kinds.image` survives with an amended justification

`ImageContent::d_pyramid` becomes `std::atomic<std::shared_ptr<const Pyramid>>`. `render()`,
`bounds()` and `available()` take **one acquire load**; the install takes **one release
store**. The store happens at most once per content and is **monotonic**: null → non-null,
never reverting, never replacing a published pyramid. `ImageContent` also **keeps its resolved
URI** (a new `std::string d_resolved` member — it already arrives in the three-part frame at
`image_content.hpp:222-244` and is discarded today at `image_content.cpp:321`), so the late
install keys the *same* `PyramidCache` and costs exactly one decode.

`render_thread_safe()` **stays `true`**, and `kinds.image` Decision 4 stands: the pyramid is
still an immutable object, still built once, still never mutated. What changes is only the
*argument* for the flag. The header says it twice today — "the pyramid is immutable after
construction, so a render is a pure read" (`image_content.hpp:53-54`, `:204`) — and that
sentence is now false. The replacement is: **the pyramid is immutable and published exactly
once, atomically, so a render is a pure read of a pointer that is either null or final.** The
design-doc delta records this, because it is a promise doc 03 currently rests on.

**Rationale.** Monotonicity is what makes the race benign without a lock. A reader can observe
only two values, and both are self-consistent: the empty state (which the compositor culls,
because bounds are empty) and the finished pyramid. There is no intermediate. So the worst a
racing worker can do is render a frame's worth of nothing — which the arrival's damage
immediately schedules a re-render for. That is doc 02's Refine step working exactly as
designed, not a bug being tolerated.

**Rejected — a mutex on the content.** It would serialize every worker render behind a lock
taken on a pointer that changes at most once in the content's lifetime, and it would put a
lock on the hot path of the *only* leaf kind that currently has none. `org.arbc.image` returns
`render_thread_safe() == true` specifically so it can compute on workers under doc 00:203's
leaf-only dispatch rule; a mutex is the slow way to keep a promise an atomic keeps for free.

**Rejected — `render_thread_safe() == false`**, serializing renders through the core's
per-content queue as `org.arbc.imageseq` does. That is a real option and it is what a stateful
decoder forces — but imageseq pays it because its decoder and LRU are *genuinely* mutable
across renders. An image's pyramid changes once, ever. Taking imageseq's cost to model a
one-shot transition would surrender the kind's headline property (`kinds.image` Decision 4:
"that is a real win over imageseq") to buy nothing.

**The forward tension, stated plainly.** `kinds.image_master_budget` will evict decoded
pyramids under a byte budget and re-decode on demand — which means a *second* publish, and a
non-null → null → non-null transition that this task's monotonicity forbids. That is
deliberate: it is that task's job to generalize the discipline (most likely by evicting only
what no live content holds, or by making the pointer a proper versioned cache slot and paying
for it), and it inherits `install_asset` as its re-install seam. **This task's invariant is
one-shot publish, and the claim is written that way.** Do not pre-weaken it.

### Decision 7 — `Content::install_asset(std::string_view)` — a contract-level, writer-thread install channel

```cpp
// contract/content.hpp, beside external_asset_ref() (:643)

// Install the bytes of the external asset this content references, arriving LATE --
// after construction, on the WRITER THREAD, because the AssetSource deferred
// (doc 08 Principle 3). Returns true iff the content is now available.
//
// A kind that overrides this MUST publish the decoded result atomically and at most
// once, so a worker holding an earlier pinned revision observes either the
// pre-arrival state or the final one -- never a partial install. A kind that does not
// override it has no late-install channel and its references are resolved-or-unavailable.
virtual bool install_asset(std::string_view /*encoded*/) { return false; }
```

`ImageContent` overrides it: `d_pyramid.store(cache.resolve(d_resolved, encoded),
std::memory_order_release)`, returning `d_pyramid != nullptr`. Empty or undecodable bytes
yield `false` and leave the content unavailable — a value, never a throw (Constraint 11).

**Rationale.** The `.tji` note names exactly this — "requires a pixels-arrive-later install
channel on the plugin content" — and the contract is where it must live, because `kinds.image`
Decision 2 forbids the codec from naming `ImageContent` (naming it would link the decoder into
`libarbc`, the exact leak the codec line exists to prevent). So the runtime must reach the
plugin through a virtual, and `external_asset_ref()` is the precedent for adding one: same
place, same shape, same "a channel the core reads/writes, never a thing the core sets"
character.

The signature carries **only a `string_view` of bytes** — no `LoadContext`, no `ResolvedRef`,
no JSON type — so it names nothing above `contract` (L3) and adds nothing to any plugin's link
surface. That is the same test `kinds.image` Decision 5 applied when it rejected widening
`ContentFactory` to take a `LoadContext&`.

The resolved URI is **not** a parameter: the content already received it at construction and
now keeps it (Decision 6). Passing it again would let the runtime hand a content bytes for a
*different* asset than the one it references, which is a bug the type system can simply
forbid.

**Rejected — a `dynamic_cast<ImageContent*>` in the settle arm.** It requires `runtime` to
name the plugin type, dragging the decoder into `libarbc`. This is the codec line, and it is
not negotiable.

**Rejected — routing the install through the `ContentFactory` by re-minting the content**
with the full three-part frame and swapping it into the `Document`. It replaces a live,
referenced object under a binding the model holds, loses the content's identity (which the
damage route and every cache key name), and would have to be undone by
`model.content_removal` machinery for a state change that mutates no document data.
Decision 1 rejected the same swap in its `PendingImageContent` form.

### Decision 8 — No new driver: the arrival rides `HostViewport::step()`'s existing settle hook

`settle_external_loads(doc, bridge, registry)` (`document_serialize.cpp:565-594`) gains an
asset arm inside its existing loop. Nothing else changes: `HostViewport::Config::settle_external_loads`
(`host_viewport.hpp:105`) already calls it at step 0 (`host_viewport.cpp:94-105`), and
`derive_document_config` already auto-wires that hook from a `Document&`
(`runtime.host_viewport_document_binding`). An image arrival therefore settles inside the very
frame that observes it, before the pin and before the damage drain, with **zero** new wiring —
and `Document::pending_external_loads()` widens from "external compositions I am waiting on" to
"external fetches I am waiting on", which is the meaning the counter's name always had.

**Rationale.** This is the whole payoff of Decision 2's "share the queue": the driver, the
counter, the frame ordering and the `01-core-concepts#viewport-binds-to-document` claim all
transfer for free. The predecessor task paid for this seam (and paid again, in
`host_viewport_document_binding`, to auto-wire it); this task spends it.

**Offline drivers are deliberately out of scope.** An offline `SequenceRenderer` export of a
document with a pending asset renders whatever is available at load — it does not poll, block
or wait. A host driving an offline export over a deferring source drives `settle_external_loads`
itself before exporting, which is the same contract `runtime.async_external_load` left for
external compositions. Whether an offline driver *should* instead block until
`pending_external_loads()` reaches zero is a genuine design question with no obviously right
answer (it trades a hang against a silently incomplete export), it is not image-specific, and
it is not made worse by this task — it goes to the parking lot, not the WBS.

## Design-doc deltas (ride in the closer's commit — doc 16 same-commit rule)

1. **`docs/design/08-serialization.md`, Principle 3** (after the extent carve-out at
   08:135-144, before "The core fetches asset bytes" at 08:145-153). Add the asset-side
   three-state, the symmetric statement of doc 05:77-83's rule, and the arrival's shape: an
   `AssetSource` that has not answered leaves the reference **pending**, not unavailable; the
   split is decided by whether the source answered, never by the bytes being empty; the fetch
   may run on any thread but the install and its damage are marshalled onto the writer thread,
   as one commit that publishes a revision and flushes damage naming the referencing content;
   a pending asset is extent-less exactly as an unavailable one is, and *gains* its geometry
   at the install, which is doc 02's Refine step.

2. **`docs/design/03-layer-plugin-interface.md`** (beside `external_asset_ref()` at
   03:131-141). Add `install_asset()`: the writer-thread late-install channel, the
   publish-atomically-and-at-most-once obligation it places on an overriding kind, and the
   consequence for `render_thread_safe()` — a kind may keep the flag `true` across a late
   install **iff** it publishes atomically, which is how `org.arbc.image` keeps it. This also
   corrects the now-false rationale the section leans on ("immutable after construction").

3. **`docs/design/00-overview.md`** — a decision-record bullet: *"An external **asset** has
   the same three load states as an external composition — resolved, pending, unavailable —
   split by whether the source answered, never by the bytes being empty. `Content` gains a
   writer-thread `install_asset` channel, and a kind that uses it keeps
   `render_thread_safe()` by publishing its decoded result atomically and once."* Project-shaping
   because it retires a thread-safety argument (immutability-by-construction) that doc 03 and
   the image kind both rested on, and replaces it with a weaker, explicit one.

## Open questions

(none — all decided.)

## Status

**Done** — 2026-07-14.

- Shipped `ExternalAssetLoader` (`src/runtime/arbc/runtime/external_asset_loader.hpp`, `src/runtime/external_asset_loader.cpp`): mirrors `ExternalCompositionLoader`, shares `PendingExternalLoads`'s queue and mutex, deduplicates fetch by resolved URI, uses the `Content*`-keyed pending entry that `LoadAssembly`'s sink binds to `ObjectId`.
- Added `Content::install_asset(string_view)` virtual (`src/contract/arbc/contract/content.hpp`): writer-thread late-pixel channel; default returns `false`; `ImageContent` overrides with an atomic release-store into `d_pyramid`.
- `ImageContent::d_pyramid` made `std::atomic<shared_ptr<const Pyramid>>`, monotonic null→non-null publish (`plugins/image/arbc/kind_image/image_content.hpp`, `plugins/image/image_content.cpp`); `d_resolved` kept so the late install keys the same `PyramidCache`; `render_thread_safe()` stays `true`.
- `image_codec` gains `ExternalAssetLoader*` parameter; null pointer falls back to v1 synchronous path (`src/runtime/codec_image.cpp`, `src/runtime/arbc/runtime/builtin_codecs.hpp`); `settle_external_loads` grows an asset arm inside its existing quiescence loop (`src/runtime/document_serialize.cpp`).
- New tests: 10 behavioral cases (`tests/image_async_pending.t.cpp`), 2 byte-exact golden cases (`tests/image_async_pending_golden.t.cpp`), 3 TSan/stress cases added to `tests/image_concurrency_stress.t.cpp`.
- Claims: 3 new rows (`08-serialization#pending-asset-installs-live`, `#pending-asset-is-not-unavailable`, `03-layer-plugin-interface#image-pyramid-publishes-once`) plus 6 second `enforces:` tags re-enforcing existing claims (`tests/claims/registry.tsv`).
- Design-doc deltas in same commit: `docs/design/08-serialization.md` (asset-side three-state, arrival shape), `docs/design/03-layer-plugin-interface.md` (`install_asset()` beside `external_asset_ref()`), `docs/design/00-overview.md` (decision-record bullet retiring immutability-by-construction argument).
- Fuzz seed added: `tests/fuzz/corpus/load_document/pending_image_asset.arbc`.
- One deliberate deviation from acceptance criteria: case 5 asserts `decodes_issued()` delta **0** (not 1) and pins `install_asset() == false`; `PyramidCache::resolve` only increments the counter on a successful decode, so an undecodable arrival costs zero decodes — changing the counter would redefine `#image-decodes-once-per-resolved-uri`.
- Refinement: `tasks/refinements/kinds/image_async_pending.md`
