# Changelog

All notable changes to this project are documented here, in
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) form. Versioning is
[semantic](https://semver.org/spec/v2.0.0.html) from the first tag; pre-1.0 the
surface moves freely, and changelog honesty is what makes that safe
(design doc 16, § Physical design and maintainability).

**The discipline** (kept by hand — there is no CI check, deliberately):

- One entry per landed change that a *consumer* could observe. If a commit changes
  the shipped surface — public headers, the installed package, behavior, a
  dependency — it lands its changelog line in the same commit.
- This file tracks the **shipped surface**; the git log tracks commits. It is not a
  transcription of the log, and a docs-only, test-only, or internal refactoring
  commit belongs in the log alone.
- New entries go under `[Unreleased]`, in the `Added` / `Changed` / `Fixed` /
  `Removed` groups. A release moves that section under its version heading.

## [Unreleased]

### Added

- **`install_external_composition(doc, bridge, registry, reference, base_uri, ...)`**
  (issue #32) — the seam that installs an external composition into an ALREADY-OPEN
  document. `ExternalCompositionLoader` was `LoadContext`-scoped and therefore
  reachable only at deserialize time, so a nested reference a user created during a
  session could not resolve until the project was reopened: a host that lets a user
  drop one `.arbc` into another minted the cell and rendered the placeholder box, with
  the child sitting right there on disk. It drives the same machinery a load and a
  late arrival do — the same three outcomes (resolved / pending / unavailable, the
  last a value and not an error), the same writer-thread rule, and the same
  document-wide durable dedup, so installing a URI the document already holds returns
  that composition and parses nothing. Not undoable, deliberately: the child arriving
  is not the user's edit, the *placement* is, so an undo of the placement leaves the
  child installed and a redo re-binds it without re-reading a file that may have moved.
  `assets` defaults to the source the document's own load recorded.

- **`HostViewport::attach_damage_sink()` / `detach_damage_sink()`, and
  `Config::install_damage_sink`** (issue #28) — the successor to `#25`'s settler
  split, and the half that actually unblocks it. With only the settler deferrable a
  host still could not construct a viewport off its writer thread: the constructor
  reached `DamageRouter::register_sink`, which mutates an unsynchronized registrant
  vector that the writer thread's concurrent flushes iterate. Both installs — the
  router registration and the direct `Model::set_damage_sink` slot — are now
  deferrable through one flag and one idempotent pair, so every writer-thread-only
  mutation in a viewport's lifetime is reachable from a closure the host can post,
  and construction and destruction can happen on the thread that owns the object.
  The default stays fused, which is right for a single-threaded host.

- **`org.arbc.nested` advertises an insert schema** (issue #33) — one labelled
  `child` field of the new `KindInsertField::Type::ObjectId`, with a `min` of 1 and
  no default. It was the last config-constructible builtin without a schema, so a
  host driving its insert dialog off `Registry::insert_schema` with no per-kind
  allowlist had to render it as an unlabelled box holding a bare decimal. The new
  type says the value NAMES another object, so a host can offer a composition
  picker; one that does not distinguish it collects the field exactly as an
  `Integer` and stays correct — the type narrows presentation, never parsing.

- **The `Resampleable` facet and `Content::resampleable()`** (issue #31) — a generic
  content-resample verb, so a host can ask a content to grow its working grid
  ("resample to crisp" past a cell's detail floor) without naming a concrete kind or
  owning the upsampling itself. The discovery half carries as much as the verb: a
  painted `org.arbc.raster` returns the facet, a referenced `org.arbc.image` keeps the
  `nullptr` default, and a host greys the action with an honest reason having named no
  kind. Transactional like `RasterContent::paint` — one call, one journal entry,
  undoable, `ObjectId` preserved — and its pixels are byte-identical to what a render
  at that density produces, so there is one sampling policy rather than two.
  `RasterContent` implements it; `RasterStore::resample` is the store-level verb.

- **Asset GC sees project-owned image blobs** (issue #30) — `GcRoots`,
  `collect_referenced_assets`, `sweep_asset_store`, three defaulted `AssetReaper`
  virtuals (`list_asset_uris` / `asset_size` / `remove_asset`), the pure
  `unreferenced_assets` subtraction, and a `FilesystemAssetReaper` that takes named
  asset bases beside the tiles one. `gc_project_directory` now marks `params.source`
  URIs as well as `params.blobs` hashes and sweeps `assets/images/` as well as
  `assets/tiles/`, so a host that mints owned image assets gets its
  paste → undo → save → Clean-Up cycle to actually reclaim the orphaned blob. Both
  halves landed together on purpose: a reaper that enumerated owned images without a
  mark that rooted them would delete every one of them. A **referenced** image, whose
  URI points outside the project's owned subtree, is never enumerated and so can never
  be reclaimed. Matching an authored URI against an on-disk key is deliberately
  over-approximate (a shared basename roots a blob) — retaining an orphan is a leak,
  failing to recognise a live reference is data loss.

### Changed

- **`RasterContent::bounds()` is derived from its live tile table** rather than cached
  at construction (issue #31). The working grid is no longer fixed for the object's
  lifetime, and a cached extent would be both stale after a resample and a data race
  against the render thread; the store publishes its base under the same lock every
  render already takes, so extent and pixels cannot disagree.

- **A malformed `params.source` now fails the GC mark walk** (issue #30), where the
  image codec's *load* path still treats it leniently as absent. Leniency on a read
  means "round-trip the key verbatim"; on a delete plan the same doubt has to stop the
  sweep, which then deletes nothing.

- **`KindInsertField::Type` gained an `ObjectId` enumerator.** Additive, but a host
  that switches exhaustively over the enum gains a case to handle.

## [0.4.1] - 2026-07-31

A memory-safety fix for one specific, entirely reachable host shape: two
`bind_operators` walks over one document that are not serialized with each
other. That is not an exotic arrangement — it is what a host does the moment it
samples a single cell offline (`render_offline` over a pinned document, on its
UI thread) while the interactive renderer is drawing that same document on the
frame thread. Purely additive: one new `Content` virtual with a correct default,
so a kind that does not override it is unaffected and every 0.4.0 call compiles
and behaves unchanged.

### Fixed

- **Concurrent binds no longer free a nested content's input edges under a
  graph walk.** `NestedContent::inputs()` returns a span into storage owned by
  its metadata memo, and released the memo lock before returning it. The memo is
  keyed by (snapshot, revision), so the storage is stable only for the single
  binder holding that pin: a second binder attaching a *different* pin re-keys
  the memo, rebuilds the edge vector, and frees the buffer under a walker still
  iterating the span. The compositor's damage router and aggregate-revision fold
  both hold one across a recursive descent, so this reproduced as a **data race
  and a heap-use-after-free** in `map_damage_up` — a use-after-free, not a stale
  read. The core's structural walks now read edges through the new
  `Content::visit_inputs` (below); `inputs()` is unchanged for the
  single-threaded callers (the serializer's save traversal, kinds' own tests).

### Added

- **`Content::visit_inputs(InputVisitor, void*)`** — the any-thread form of
  `inputs()`, visiting the input edges in declared order with the kind's own
  storage lock held across the visit, plus the `for_each_input(content, fn)`
  helper that wraps a lambda into it so call sites still read as a range-for.
  The visitor is a function pointer and an opaque context rather than a
  `std::function`, so the per-frame damage and aggregate-revision walks stay
  allocation-free. The default implementation iterates `inputs()`, which is
  correct for every kind whose edge storage is a fixed member array (fade,
  crossfade, placeholder) — only `org.arbc.nested`, whose edges are a projection
  of its child composition's membership and are therefore rebuilt on every memo
  re-key, needs the override. A kind outside this repository inherits the
  default and is unaffected. The visit is a metadata-only walk: it may recurse
  (the memo lock is recursive by design) but must never block on a pull or issue
  a render, which is what keeps the lock off the pixel path.

## [0.4.0] - 2026-07-28

The first release that is not purely additive, and the two breaks are worth
naming up front: the untiled `arbc::render_frame` free function is **removed**,
and `arbc::Backend` gains a `composite_windowed` virtual every implementation
must supply. Both fall out of the same change — there is now ONE render path.
`render_offline` renders through the tiled driver the interactive loop and the
sequence exporter already ran, and `Exactness` is the only axis that separates
them. The unification is what this release's rendering fixes ride on: the tile
apron that makes an opaque fill opaque across tile boundaries at any fractional
composite phase, the in-tile extent enforcement that antialiases a bounded
layer's edge instead of clipping it to whole device pixels, and magnification
that belongs to the kind rather than the compositor (issue #18).

The other half is host surface. A reopened workspace gives back its *content*
and not just its record graph (#19), with `rebind_content` as the repair seam
for what reconstruction cannot cover; one user-visible action is one journal
entry (#20); a kind describes its own insert config, so a host can offer
"insert a cell of kind X" without a hardcoded per-kind grammar (#21); a History
panel can draw the journal off the writer thread (#24); a viewport's settle
hook installs apart from its lifetime (#25); and a batch export can hold one
pin across every frame (#27). Removed content is now reclaimed once its removal
leaves history (#26), so an insert/delete session stops growing monotonically.

Every 0.3.0 call that neither implements `arbc::Backend` nor names
`render_frame` compiles and behaves unchanged; the plugin surface stays
same-toolchain and unversioned (the C ABI still arrives at 1.0).

### Added

- **A workspace reopen gives back its CONTENT, not just its record graph**
  (issue #19). `Document::open` restored records and bound no `Content` for any
  kind, so a reopened workspace could be neither rendered, edited nor hit-tested,
  and a host had no repair available. `ContentRecord` now carries the content's
  **construction identity** — the reverse-DNS `kind_id` and the kind's canonical
  `params` — and its **input edges**, both as spill chains of ordinary records
  (the shape a composition's overflowing layer order already uses), captured at
  `add_content` through the kind's own registered codec. `arbc::open_document`
  rebuilds each content through the same routing `load_document` uses for a
  canonical file. A record that cannot be rebuilt — a plugin absent this session,
  a kind with no codec, a file written before this — is **reported** in
  `ReopenedDocument::unreconstructed` and left unbound, never filled with a
  default-constructed stand-in of the right kind (design docs 14/15).

- **`Document::rebind_content(id, content)`** binds an object to a content record
  that already exists, and rebinds one that is bound. It is the repair seam for
  everything reconstruction cannot cover; `add_content` could not serve, since it
  mints a new record with a new id while the recovered layers, journal edges and
  state handles all name the old one. Publishes no version and appends no journal
  entry. **`Document::recovered_content_state()`** ships with it, making the
  issue-#5 recovery-replay trio reachable from the public host surface.

- **`Document::create_content_and_attach` and `Document::remove_contents`** make
  one user-visible action one journal entry (issue #20). `add_content`
  self-commits, so creating a *placed* object took two entries, two undo presses,
  and passed through a published state in which a content existed attached to
  nothing; `remove_content` is atomic within one object but had no batching hook,
  so deleting a multi-selection of N objects took N undo presses to reverse
  something the user did once. Both now publish once, append one entry, and
  reverse whole.

- **`Registry` carries a per-kind insert schema** (issue #21):
  `KindInsertSchema` — the fields a host must collect, with types, defaults,
  ranges and units — plus an `assemble` thunk turning collected strings into the
  kind's config. `ContentConfig` is opaque, so a host offering "insert a cell of
  kind X" had to hardcode a per-kind grammar table, and a plugin kind could be
  listed in a menu but not inserted with anything but a guessed config — the case
  the plugin seam exists to serve. The grammar stays the kind's own: a host
  renders fields and hands back strings, never learning that solid's separator is
  a comma. Registered on the same atomic `add` as the factory, so a plugin cannot
  describe another kind's config after the fact; a kind that registers none is
  unaffected. The built-ins advertise theirs, with solid's extent defaulted so an
  insert dialog produces a *placeable* solid.

- **`Journal::history()` publishes an any-thread view of the entry list** (issue
  #24), the follow-on to the enable pair #15 published. A History panel draws one
  row per entry every frame, off the writer thread, against a vector a commit may
  reallocate — the one UI read the single published word could not cover, and one
  every host would otherwise re-implement. Returned as a pinned immutable snapshot
  (the shape `Model::current()` already uses), carrying the projection a panel
  draws — name and byte cost — rather than the entries themselves, whose edit and
  damage vectors a UI never shows and would copy per commit. Unchanged rows are
  shared by pointer, so a commit copies N pointers, not N strings, and an undo
  republishes without allocating a row. `entry_at` stays writer-thread-only.

- **`HostViewport` can install its settle hook apart from its lifetime** (issue
  #25): `attach_settler()` / `detach_settler()`, opted into with
  `Config::install_settler = false`. The `Document&` constructor installed the
  hook itself, which welded a writer-thread requirement onto `new` and `delete` —
  so a host whose viewports live on a render thread had to post the whole
  construction to the writer thread for one line inside it, paying a synchronous
  round trip per canvas add and remove and giving up scoped ownership. The install
  counting and the N-viewports-per-document rule are unchanged; only *when* they
  happen moves. The default keeps the fused behaviour.

- **`render_offline` takes a caller-held pin**, so a batch export is coherent
  across one document state (issue #27):
  `render_offline(document, pinned, viewport, backend)` beside the
  pin-per-call overload. An N-frame export previously pinned N times, so an edit
  landing mid-batch made item 3 reflect a state item 1 did not, and the host had
  no way to ask for the same version twice — its alternatives being to block the
  writer for the whole batch or to serialize-and-reload a private copy. A
  `DocStatePtr` retains its version, so there is no "that revision is gone" error
  case a revision number would have needed.

- **`org.arbc.solid`'s factory grammar admits the extent the type always
  supported** (issue #22): `"r,g,b,a,x,y,w,h"` alongside the unchanged
  `"r,g,b,a"`. A solid built through the `Registry` — the only route a host has
  that does not bypass the factory to name the concrete type — was always
  unbounded, and an unbounded solid fills everywhere, so its layer transform was
  a no-op: placing, scaling or rotating one changed nothing on screen. Origin
  plus size, so a typo cannot express an inverted rect; a non-positive size is an
  error value, since an empty extent renders nothing and would look exactly like
  the bug this fixes.

- **`Document::set_content_identity_capture`**, the hook `add_content` runs to
  capture the above; `arbc::codec_identity_capture(codecs, bridge)` is the
  ready-made codec-backed one. A host that installs none keeps the previous record
  shape exactly, and its documents still reopen — they simply report every content
  as unreconstructed.

### Changed

- **`Content::bounds()` is documented ANY THREAD**, and a kind whose extent
  changes under an edit must publish the change atomically (issue #23). No
  behaviour changed: the compositor already calls it on the frame thread once per
  visible layer per frame while the writer may be committing, and a host
  hit-testing through the lock-free `pin()` seam already did the same — it was
  safe only because every shipped kind fixes its extent at construction, which is
  an accident of the kind set rather than a contract. Kind authors with a mutable
  extent are now on notice; every other kind is unaffected (design doc 03).

- **Tiles are now rendered over their cell plus an apron, and paint only the
  cell.** Every tile surface grew from `256²` to `264²` device pixels and covers
  its cell plus a 4px margin of its neighbours' own content; the composite then
  paints through a new `Backend::composite_windowed(dst, src, src_to_dst, opacity,
  device_clip, src_window)`, whose source-space window is that cell. The
  resampling tap reads freely into the apron while each destination pixel is
  painted by exactly one tile — a partition that holds under rotation and shear,
  which no destination-space scissor can express. Cache residency per tile rises
  6.3%; `composites` and `requests_issued` are unchanged. Backends implementing
  `arbc::Backend` must supply the new virtual; a double that models pixels must
  model it, and must model `clear_rect` too (design docs 02/09).

- **A layer with finite `bounds()` now has its extent enforced in the tile's own
  pixels, not by clipping the composite.** Each rendered tile is zeroed outside
  the content's declared extent before it is cached or composited, and
  `compositor.bounded_content_tile_clip`'s device-space round-out clip is
  **retired**. The visible difference is at the edge: the extent boundary now
  antialiases through the composite's own tap instead of being clipped to whole
  device pixels, so a half-covered edge pixel carries its coverage and the
  previous ≤1px of un-attenuated bleed is gone. A `bounds()`-less layer is
  untouched (design doc 02).

- **`SurfaceRef` gained an optional `origin`**, the content-local point a
  content-provided surface's pixel `(0,0)` covers. Absent (the default, and every
  non-providing content) it is the request region's origin, which is what the
  compositor assumed unconditionally before. Content that hands back a surface it
  did not render for this request — a decoder returning its native frame — must
  now say where those pixels belong; `org.arbc.imageseq` does. It rides the
  handle beside `transient`, for the reason doc 09 already gives for that flag:
  both describe the surface, not the `RenderResult` carrying it (design doc 09).

- **Design docs 02 and 09 reconciled with the one-render-path unification.** Doc
  02 said "two drivers over the same core" and described the offline frame as
  running "without quantization"; both were left stale by `6a3e30a`, and the
  second was the more misleading — an offline frame plans against the same scale
  ladder as an interactive one, and "no degradation" is a behavioural zero
  (`degraded_composites == 0`) rather than a separate path. Doc 09's provided-
  surface addendum said the compositor composites inline from a provided surface
  zero-copy; that branch has no production caller left, so every provided surface
  a shipped frame consumes is copied. No behaviour changed with this entry — the
  documents did.

- **There is now ONE render path.** `render_offline` renders through
  `render_frame_interactive` — the same tiled driver the interactive loop and the
  sequence exporter already ran — and the untiled `arbc::render_frame` free
  function is **removed**. The only axis that separates the drivers is
  `Exactness`: `BestEffort` trades fidelity for frame rate, `Exact` (with
  `Deadline::none()`) takes as long as it needs. The retired path had quietly
  fallen behind on four axes its own comments admitted — it asked for the raw
  composed scale rather than a ladder rung, hard-coded `Time::zero()` (so no span
  culling), passed an inert `StateHandle{}` (so no content state), and dropped any
  layer whose content answered asynchronously. `render_layer` is unaffected
  (`anchored_viewports` still uses it). A still export now also composites
  slightly differently where the two paths disagreed: the retired driver sized its
  temp to exactly the content, so a minification's outer resampling tap fell on
  that surface's transparent border and rang the value up (design docs 02/09).

- **Zero-copy adoption of a content-provided surface is abandoned.** It existed
  only on the untiled path, which composited directly from content memory
  (doc 09:122-124). The tiled driver renders each miss straight into the
  cache-owned surface it will insert and always copies a provided surface into it,
  so with the untiled path gone there is no zero-copy consumer left. Provided
  surfaces are otherwise unchanged: their pixels are honored, and the release
  callback still fires within the frame that consumed them, so the compositor
  holds no reference to content memory across frames.

### Fixed

- **Removed content is reclaimed once its removal leaves history** (issue #26,
  `runtime.removed_content_reclaim`). `Document::remove_content` retains the live
  `Content*` and its binding row so the erased record's deferred `StateHandle`
  release can still route while the journal holds the removal — correct, but it
  meant a long session of insert/delete cycles grew monotonically in resident
  memory until close, even after history trims, with no host-side mitigation. The
  teardown now completes at the LAST release: zero outstanding retains means no
  live `ContentRecord` instance — current version, pinned snapshot or journal edge
  — still embeds that content's state, so nothing can route to it and nothing can
  strand. `Journal::set_byte_budget` re-budgets history after construction, which
  is what lets a removal leave it at all.

- **An opaque fill is opaque again across tile boundaries.** At any fractional
  composite phase — a sub-pixel pan, an off-rung scale, any rotation — each tile's
  Catmull-Rom tap fell on its own surface's transparent border, so the two tiles
  abutting a boundary each painted the boundary pixel at roughly half weight and
  premultiplied source-over of two halves is `0.75`, not `1.0`. Measured on a
  600×600 opaque solid at `translation(0.5, 0.5)`: alpha `0.75` at device x=256 and
  x=512, ringing to `1.0625` either side — a translucent grid every 256 device
  pixels, appearing the moment the camera moved by half a pixel. It was invisible
  to every landed golden because they all composite at an integral phase, where the
  cubic collapses to `(0, 1, 0, 0)` and reads one texel. Fixed by the tile apron
  above. The same defect on `PullServiceImpl`'s delivery path — whose "each covering
  tile delivers seam-free" claim was false for the same reason — is fixed with it.

- **A nested composition renders byte-identically to compositing its child's
  layers flat again**, at minifying scales as well as at native (doc 05 §
  Rendering is recursion). The two paths had disagreed about what happens at a
  content's edge: `NestedContent` sizes its temp to the content's own region and so
  falls off across the extent, while the tiled walk filled each tile past the
  extent and re-imposed it with a whole-pixel clip. Rendering the same scene both
  ways at 0.5× differed on 17 of 64 pixels — 7 inside the content's device bounds
  and 10 outside it. The tiled path was the wrong one; the three goldens tagged
  `[!mayfail]` for this are re-enabled.

- **A content-provided surface is no longer mislaid when the request does not
  start at the content's own origin.** The zero-copy path copied at the identity,
  which silently assumed the two origins coincide — true only for the tile at the
  local origin, so a decoder's frame delivered into any other tile already landed
  in the wrong place. See `SurfaceRef::origin` above.

- `org.arbc.raster` and `org.arbc.image` now render at the **requested scale**
  under `BestEffort`, magnifying past native exactly as they already did under
  `Exact`, instead of clamping to native and reporting `achieved_scale <
  request.scale`. The tiled compositor carries no `achieved_scale` term in any
  composite arm, so the clamped answer broke two things at once: a magnified
  layer drew at native size however far the camera zoomed, and its tile
  satisfied neither cache probe, so it re-rendered at frame rate forever — every
  camera scale strictly above 1.0, not just extreme zooms. Magnification also
  belongs to the kind rather than the compositor: a kind samples its own source
  across its internal tile boundaries, where the compositor's tap fetches a
  transparent border at every isolated tile edge (one source pixel of falloff —
  a device pixel at the ladder's ≤1-octave remainder, a visible seam grid at
  10×). The two exactness modes now differ only in **time**, which is what doc
  03 says `BestEffort` is for (design docs 03/04; issue #18).

- A nested composition whose child is an **external** reference no longer
  composites blank under a worker pool. The interactive driver's operator-layer
  memo admitted a nesting layer on its structural child edge but excluded one
  holding an external child, so a deferred external child's leaf arrival routed
  to no layer root the embedding viewport walks: it mapped to zero device rects,
  scheduled no follow-up frame, and the scene quiesced fully transparent — while
  compositing correctly under inline dispatch, where the render happens inside
  `submit` and no arrival is routed at all. The exclusion was the serializer's
  rule (an external child is not ours to inline on save), which does not hold at
  render time, where an externally-loaded child is an ordinary composition in
  this document's model (design doc 05, § Recursive composition; issue #17).

- `Journal::can_redo()` no longer reports a redo that does not exist when read
  from a non-writer thread. The published cursor and entry count were two
  separate atomics, so a reader whose two loads straddled a commit compared a
  stale cursor against a fresh depth; they are now published together in one
  word, which readers load as a snapshot (design doc 14, § The enable state is
  published).

## [0.3.0] - 2026-07-23

Additive since 0.2.0, and all of it about one thing: a host whose UI thread is
not its writer thread. `HostViewport::step()` no longer publishes structural
writes off the writer thread (issue #13), which made the doc 15 single-writer
identity queryable rather than merely asserted; the journal's undo/redo enable
state — the last per-frame UI read that was neither pinned nor published — is now
an any-thread lock-free read (issue #15). The new surface is what those two fixes
need: the writer-thread predicate, the ready-arrival counts, and the automatic
writer-thread settler. Every 0.2.0 call compiles and behaves unchanged on a
single-threaded host; the plugin surface stays same-toolchain and unversioned
(the C ABI still arrives at 1.0).

### Added

- **`Model::on_writer_thread()` / `Document::on_writer_thread()`** — the document's
  single writer identity, now bound in *every* build (one atomic `thread::id`, set by
  the first transaction, never rebound) and queryable from any thread. True before any
  write, when the caller would become the writer. Doc 15's single-writer-identity
  contract was previously only a debug assert one level down in `SlotStore`; a host —
  and the library itself — can now *ask* instead of finding out by corruption.
- **`Document::external_loads_ready()`** — how many fetched external arrivals are queued
  awaiting a writer-thread install. Lock-free, allocation-free, any-thread: the poll a
  render loop uses to learn that a settle is owed. Beside the existing
  `pending_external_loads()` (fetches still in flight).
- **`HostViewport::StepOutcome::external_loads_ready`** — the same count, reported per
  frame by a step that declined to install (see *Fixed*). Zero when the step settled.
- **`HostViewport::Config::external_loads_ready`** — the readiness probe beside
  `Config::settle_external_loads`, derived from a bound `Document` and overridable, for
  a host driving a bespoke settle hook off its writer thread.
- **`Document::set_external_load_settler()` / `Document::external_loads_auto_settled()`**
  — the writer-thread settler a `Document`-bound `HostViewport` installs (and releases)
  automatically, run immediately ahead of the document's next edit whenever an arrival
  is waiting, plus its behavioral counter.

### Fixed

- **`HostViewport::step()` no longer publishes structural writes off the writer thread**
  (issue #13). Frame planning is render-thread-confined by design, but step 0 ran the
  external-arrival settle — a model transaction, an `add_content` and a commit —
  unconditionally, so a host that edits on its UI thread and renders on another got a
  *second* writer identity, which doc 15 forbids and no host-side mutex can repair. The
  step now asks `Model::on_writer_thread()`: on the writer thread it settles inline
  exactly as before (single-threaded hosts, and every driver in the tree, are
  unaffected); off it, it publishes nothing and reports `external_loads_ready`. The
  install then happens on the writer thread — driven by the host, or automatically ahead
  of the host's next edit, so ignoring the report costs latency, never correctness.
  `settle_external_loads()` is documented writer-thread-only and debug-asserts it.
- **`Journal::can_undo()` / `can_redo()` / `depth()` / `cursor()` are any-thread reads**
  (issue #15). They were plain reads of the cursor and the entry vector while the writer
  mutated both — `can_redo()` read `d_entries.size()` across a `push_back` that may
  reallocate — so a host whose UI thread is not its writer thread could not ask whether
  to enable its undo/redo affordances without a data race, and it must ask every frame.
  The cursor and the entry count are now published as relaxed atomics (the entry vector
  stays writer-owned), making the four accessors lock-free from any thread. The
  published pair is never *ahead* of the history: a reader that catches one of the two
  stores without the other is never offered an undo or redo that does not exist, only —
  for at most a frame — denied one that does, and the writer re-checks before navigating,
  so a stale enable costs a refused no-op and never a wrong mutation. Ironic provenance:
  doc 15's advice for a two-writer host is to funnel writes onto one dedicated writer
  thread, which is exactly what moves a host's UI thread off the writer and trips this.
  History *inspection* — `entry_at()`, `byte_cost()` — stays writer-thread only and is
  now documented as such; publishing the entry list would be a copy-on-write of the
  history itself, which nothing needs yet.
- **The `HostViewport` damage handoff is synchronized.** `DamageAccumulator::flush` runs
  inside a commit on the writer thread while `step()` drains it on the render thread —
  an unguarded `std::vector<Damage>` for any host rendering off-thread. It now carries a
  mutex for that handoff alone (a bounded append or a swap; no render, plan, or pull
  inside it), so an off-thread host needs no coarse per-frame lock of its own.

## [0.2.0] - 2026-07-22

Additive since 0.1.0: the per-kind state-slab walk hook lands the recovery half
of the editable-state seam, plus a rendering-correctness fix and a lock-free
render read path. The plugin surface stays same-toolchain and unversioned
(the C ABI still arrives at 1.0); every 0.1.0 registration compiles unchanged.

### Added

- **`Registry::KindStateWalker`** — a per-kind state-slab reachability walker,
  registered atomically with the factory and looked up lock-free via
  `Registry::state_walker(id)`. `Registry::add()` gains a trailing defaulted
  `std::optional<KindStateWalker>` parameter, so every existing registration
  compiles unchanged. Mirrors `KindBinder`'s static-thunk idiom: the store is
  type-erased across the registry boundary and the owning kind's TU casts it back.
- **`Model::recovered_content_state()`** (and `Model::RecoveredContentState`) — on
  a workspace fast-reopen the model's recovery walk collects each reachable
  non-inert content `StateHandle` it cannot descend itself (owning `ObjectId`,
  kind id, handle), for the runtime to replay. The model holds only the opaque
  slot and, by levelization, cannot name the kind — so it collects rather than
  descends.
- **`arbc/runtime/recovered_state_replay.hpp`** — `replay_recovered_content_state()`
  routes each collected handle to its owning kind's registered walker, so a
  reopened document rebuilds the slab refcounts a persisted handle keeps reachable
  (the recovery twin of the writer-owned `StateRefSink` retain/release seam).
  Unresolvable kind tokens and walkerless kinds are skipped and counted, never
  fatal; it returns `{dispatched, skipped}` as a behavioral witness.

### Changed

- **Render reads are lock-free** — a `Document`'s content bindings now publish
  copy-on-write through an atomic `shared_ptr<const ContentBindings>`, so a render
  snapshot never contends with a concurrent edit that rebinds contents.

### Fixed

- **`render_offline` binds operators** — nested compositions rendered through the
  offline one-shot now have their operator graphs bound, instead of rendering
  unbound and producing wrong output.

## [0.1.0] - 2026-07-17

The surface the first tag (0.1.0) names. There is no predecessor version, so
there is nothing for these to have changed *from*: this section describes what
0.1.0 ships, not a diff against a predecessor.

### Added

- **`libarbc`** — a 2D scene composer with pluggable layer kinds, shipped as a
  single static library composed of levelized internal components (design doc 17).
  Its public headers install under `<prefix>/include/arbc/<component>/`.
- **The layer/plugin contract** (`arbc/contract/`) — `Content` and its optional
  facets (operator graph, audio, editable state), rendering as a pure function of
  (snapshot state, region, scale, time), settling inline or asynchronously through
  a `RenderCompletion`. Errors are values; no exceptions cross the public boundary.
- **`arbc-testing`** — the contract conformance suite, shipped as public API behind
  `find_package(arbc CONFIG REQUIRED COMPONENTS testing)`. A plugin author runs it
  over their own `Content` factory and gets the contract's behavioral promises
  checked for them.
- **Reference layer kinds** — `org.arbc.solid`, `org.arbc.tone`, `org.arbc.raster`,
  `org.arbc.nested`, `org.arbc.fade`, `org.arbc.crossfade`, built into the library
  and dual-built as `dlopen` plugins in CI; the codec-carrying `org.arbc.imageseq`
  ships as a separate plugin artifact.
- **The plugin seam** — a single `extern "C" arbc_plugin_register(Registry&)` entry
  point. It carries no ABI number and negotiates nothing: v1 accepts same-toolchain
  coupling (doc 03, Stage 1), and a versioned C ABI arrives at 1.0.
- **Registry-carried codecs and binders** — a `Registry` entry optionally supplies,
  atomically with its factory, a JSON-free `KindCodec` (text params ↔ content
  state) and a `KindBinder` (operator-graph input binding), so a loaded plugin's
  kind round-trips through document save/load and participates in the operator
  graph entirely from its own module — no JSON type crosses the plugin surface.
- **`arbc::register_builtin_kinds()`** — one call at the umbrella presents the six
  in-lib kinds (factory + metadata, skip-on-duplicate, idempotent) through the same
  `Registry` surface loaded plugins register into, so a host can enumerate what the
  library can instantiate — an "insert layer" menu straight off `Registry::ids()`.
- **Rendering** — a CPU reference backend with byte-exact deterministic output,
  compositing source-over on premultiplied alpha in a per-composition working color
  space, with a power-of-two scale-rung tile cache and higher-order (Lanczos-3 /
  Catmull-Rom) resampling. Content may hand back its own surface (a decoder's
  output, an engine framebuffer) instead of filling the target, and caller CPU
  memory imports wrap-or-copy through the backend — foreign pixel formats converted
  at import so no foreign tag reaches the compositor.
- **Audio** — a block cache, a composition mixer, and a lookahead scheduler that
  keeps the RT device callback free of rendering work.
- **The versioned data model** — persistent, structurally shared document state with
  transactions, undo/redo, damage propagation, and an arena-backed workspace file
  with crash-consistent checkpointing.
- **Serialization** — a JSON document format with zstd-compressed raster tile blobs.
- **`arbc/version.hpp` and the version symbols** — `ARBC_VERSION_MAJOR` / `_MINOR` /
  `_PATCH`, a comparable `ARBC_VERSION` with `ARBC_VERSION_ENCODE(major, minor,
  patch)` to build comparands, `ARBC_VERSION_STRING`, and a `constexpr`
  `arbc::compiled_version()` reporting the headers you compiled against — paired with
  `arbc::linked_version()` / `arbc::linked_version_string()`, out-of-line symbols
  reporting the library you actually linked or loaded. The two exist so header/library
  skew is *observable*; the library reports it and never enforces it (doc 10,
  § Versioning and the version API).
- **A CMake package** — `find_package(arbc CONFIG)`, with `SameMajorVersion`
  compatibility and an optional `testing` component.
- **`arbc_add_plugin()`** — a CMake helper shipped inside the package config:
  against an installed arbc, a complete third-party plugin build is
  `find_package(arbc CONFIG REQUIRED)` plus one `arbc_add_plugin()` call, producing
  a loadable module. `examples/plugin-template/` is the copyable starting point.
- **Shipped embedding examples** — `examples/host-offline/` (one exact frame to
  PNG, the minimal embedding) and `examples/host-interactive/` (a `HostViewport` +
  `InteractiveRenderer` pan/zoom frame loop, headless via a scripted gesture tape),
  standalone foreign projects that CI configures, builds, and runs against a staged
  install on every lane, validating their PNG output byte-exactly.
- **Dependencies: nlohmann/json** (header-only) and **zstd** — the core's only two,
  both consumed find-first with a pinned `FetchContent` fallback. Neither appears in
  an installed header, so no embedder compiles against either.
