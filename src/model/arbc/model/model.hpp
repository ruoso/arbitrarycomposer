#pragma once

#include <arbc/arbc_api.h>
#include <arbc/base/expected.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/rational_time.hpp> // TimeRange span / TimeMap time_map setter params
#include <arbc/base/transform.hpp>
#include <arbc/model/damage.hpp>
#include <arbc/model/hamt.hpp>
#include <arbc/model/journal_entry.hpp>
#include <arbc/model/records.hpp>
#include <arbc/pool/checkpoint.hpp>
#include <arbc/pool/reclamation.hpp>
#include <arbc/pool/refs.hpp>
#include <arbc/pool/slot_store.hpp>
#include <arbc/pool/workspace_file.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace arbc {

// One immutable document version (doc 14): a per-version root holder owning one
// `Ref` into the arena's HAMT root node, plus the revision. This is the pinnable
// unit -- doc 14's "`Document` holds the current `DocState` in an atomic shared
// pointer": readers pin a version by acquiring a `shared_ptr<const DocRoot>`,
// which grants both the version handle and a count in one lock-free step. While
// any pin is held the whole reachable subtree is memory-live (the root's count
// transitively keeps it), so `peek` traversal is safe and touches no refcount
// page (doc 15:42-44, 60-62).
//
// Not a slab record -- a heap control object -- so it may hold pointers to the
// document's stores for read traversal.
class ARBC_API DocRoot {
public:
  DocRoot(StoreBundle stores, Ref<HamtNode> root, std::uint64_t revision) noexcept
      : d_stores(stores), d_root(std::move(root)), d_revision(revision) {}

  DocRoot(const DocRoot&) = delete;
  DocRoot& operator=(const DocRoot&) = delete;

  std::uint64_t revision() const noexcept { return d_revision; }

  // Lock-free, refcount-free reads (doc 14:95-96): follow `SlotRef` edges via
  // `peek`. Return nullptr when the id is absent or bound to a different kind.
  const LayerRecord* find_layer(ObjectId id) const;
  const ContentRecord* find_content(ObjectId id) const;
  const CompositionRecord* find_composition(ObjectId id) const;
  bool contains(ObjectId id) const;

  // The document's single composition (doc 14 models exactly one; where several
  // exist the lowest-id one wins, matching working_space()/working_audio_format()).
  // On success writes the chosen id and record pointer to the out-params and
  // returns true; returns false and leaves them untouched when the document has no
  // composition. The serializer's composition-discovery seam (serialize.writer):
  // a refcount-free peek traversal.
  //
  // Lowest-id-wins IS the v0.1 root rule (parking-lot triage 2026-07-16): an explicit
  // root marker on the fixed-layout `CompositionRecord` (a doc 14 + doc 15 delta) was
  // considered and REJECTED. The reader guarantees the invariant for every loaded
  // document (root id allocated before any child's), the render path takes an explicit
  // root id and never calls this, and the residual hazard is authoring-only: a
  // programmatic host that creates a child composition before its root would serialize
  // the child as the root. Revisit the marker only if such a host is ever scoped.
  bool find_first_composition(ObjectId& out_id, const CompositionRecord*& out_rec) const;

  // The working space the compositor blends this document in (doc 07 rule 2):
  // the configured `SurfaceFormat` of the document's single composition, or the
  // doc 07 default (`k_working_rgba32f`) when the document has no composition yet
  // -- a fresh document still renders out of the box. When more than one
  // composition exists the lowest-id one wins (deterministic); true
  // multi-composition selection lands with `kinds.nested`. Refcount-free peek
  // traversal.
  SurfaceFormat working_space() const;

  // The working audio format the mix engine pulls this document at (doc 12): the
  // configured `AudioFormat` of the document's single composition, or the doc 12
  // default (`k_working_audio`, 48 kHz stereo) when the document has no
  // composition yet. The audio twin of `working_space()`: same single-composition
  // resolution (lowest-id composition wins when several exist) and same
  // refcount-free peek traversal.
  AudioFormat working_audio_format() const;

  // Visit every layer record in ascending object-id order. Object ids are
  // assigned monotonically, so ascending-id order reproduces the walking-
  // skeleton's insertion order (bottom-to-top, doc 02); explicit layer reorder
  // is `model.transactions`' concern. Refcount-free peek traversal.
  void for_each_layer(const std::function<void(const LayerRecord&)>& fn) const;

  // Visit a composition's members in true bottom-to-top membership order
  // (`model.composition_membership`, doc 14 § The central decision): the inline
  // `layers[]` array while the count is within `k_max_inline_layers`, else the
  // HAMT-backed spill-chunk chain. Refcount-free peek traversal; a no-op when
  // `composition` is absent or not a composition. This is the composition-scoped
  // accessor consumers adopt over the global-order `for_each_layer`.
  void for_each_layer_in(ObjectId composition, const std::function<void(ObjectId)>& fn) const;

  // The object-record edge `id` is bound to (the HAMT leaf's `SlotRef`). Exposed
  // so a test can witness that an untouched object is shared by `SlotRef`
  // identity between two versions (14-data-model-and-editing#commit-shares-
  // untouched-structure). Returns false when absent.
  bool record_edge(ObjectId id, SlotRef<ObjectRecord>& out) const;

  // `id`'s PER-OBJECT REVISION STAMP as of this pinned version (`ObjectRecord::
  // revision`, `model.per_object_revision` Decision 1) -- the value the compositor
  // projects into the opaque `TileKey::revision` slot instead of the document-global
  // revision, so an edit to one object stops orphaning every other object's tiles.
  // `0` for an absent id: a well-defined no-op, never a stale hit (nothing renders
  // under an id the document does not contain). A refcount-free `peek` read, so it
  // holds on a parallel render worker (15-memory-model#const-ref-traversal-touches-no-
  // refcount-page).
  std::uint64_t object_revision(ObjectId id) const;

  // A composition's ARRANGEMENT STAMP: its own record stamp folded with every member
  // layer's record stamp, inline arm and spill chain alike (Decision 5).
  //
  // This exists because the compositor's aggregate fold walks `inputs()`, which yields
  // CONTENTS -- while a nested composition's composed pixels also depend on things that
  // are not contents: the child's layer ORDER, and each member layer's transform,
  // opacity, span and time map. Those live in `CompositionRecord` and `LayerRecord`,
  // separate objects with their own ids. Reorder a child composition or nudge one
  // member layer's transform and no child CONTENT's stamp moves, so a contribution
  // built from the content stamp alone would leave the embedder's composed-result key
  // unchanged and the cache would serve the pre-edit composite
  // (05-recursive-composition#composition-arrangement-joins-the-contribution).
  //
  // A SHALLOW one-level walk -- it does NOT recurse. The grandchildren are reached by
  // the compositor's own `inputs()` fold, each level contributing its own composition's
  // arrangement, so the induction closes without a second traversal here. Pure model
  // vocabulary (`CompositionRecord::layers` / `spill_root`), which is why the primitive
  // lives at L2; the JOINING of it with a content's `composition_ref()` happens in
  // `runtime`, the only level that sees both the `DocRoot` and the `Content` vtable
  // (doc 17:80-84 keeps the model free of that vtable). `0` when `composition` is
  // absent or is not a composition -- a well-defined no-op. Refcount-free peek
  // traversal.
  std::uint64_t composition_revision(ObjectId composition) const;

  // Resolve `id`'s captured content `StateHandle` as of THIS pinned version --
  // the L2 half of the render-purity refinement (doc 14:155-161). A lock-free
  // `peek` that touches no refcount page (15-memory-model#const-ref-traversal-
  // touches-no-refcount-page), so a pinned render worker resolves the frozen
  // handle a version was published with, even while the writer captures newer
  // ones. `contract.snapshot_pins` (L3) consumes this to place the handle on a
  // `RenderRequest`. Returns `k_state_none` when `id` is absent or not content.
  StateHandle content_state(ObjectId id) const;

  // Internal: the owning root reference a new transaction forks from.
  const Ref<HamtNode>& root_ref() const noexcept { return d_root; }

private:
  StoreBundle d_stores;
  Ref<HamtNode> d_root; // owning: one count on the version root (empty == empty map)
  std::uint64_t d_revision;
};

using DocStatePtr = std::shared_ptr<const DocRoot>;

// Abstract, model-defined seam (pure retain/release, doc 02): the reference
// lifecycle of a content object's editable `StateHandle`. Type-erased -- the
// model knows only the opaque `StateHandle`; the kind (L3, via runtime binding)
// owns the state's store and adjusts its per-slot count (doc 17:66-72,
// refinement Decision 3). The writer retains a non-`none` handle when its
// embedding content `ObjectRecord` slot is CREATED and releases it when that
// slot is RECLAIMED, so doc 14's two promises come true: "a pinned version pins
// content state too" (doc 14:133-136) and "version GC releases ... unreferenced
// state handles by refcount" (doc 14:173-176). Registered via
// `Model::set_state_ref_sink`; while none is registered retain/release are
// no-ops and behavior is byte-identical to inert handles. WRITER-THREAD ONLY.
// Every state seam names its owner: a `StateHandle` is a bare slot index, local
// to the owning content's own store (records.hpp:51-56), so two contents both
// holding slot 3 are indistinguishable by handle alone. The owning `ObjectId`
// therefore rides the call, exactly as `RestoreSink::on_restore` always did
// (journal.hpp) -- and it costs nothing, because the owner is already in hand at
// both call sites (`set_content_state`'s own parameter, and the reclaimed
// `ObjectRecord`'s `id`). Widening the seam rather than the handle keeps the
// mmapped record types standard-layout and fixed-size (doc 15:258-260).
class ARBC_API StateRefSink {
public:
  virtual ~StateRefSink() = default;
  virtual void retain(ObjectId content, StateHandle handle) = 0;
  virtual void release(ObjectId content, StateHandle handle) = 0;
};

// The ObjectRecord store's zero-count sink: at the zero-count-reclaim boundary of
// a content `ObjectRecord`, release its embedded `StateHandle` through the
// registered `StateRefSink` (the record is still readable -- reclamation is
// deferred), then hand the slot to the deferred reclamation queue exactly as the
// plain `DeferredReclaimSink` would. This is what makes the `StateHandle`
// lifecycle ride the record slot without a non-trivial `~ObjectRecord`
// (records.hpp:12-19, refinement Decision 2). Reached only on the single drain
// thread. Since `runtime.housekeeping_document_wiring` that drain thread is NOT
// the writer (doc 15 § Version reclamation, doc 14:168-181): a `Document`'s
// background housekeeping thread reclaims while the writer transacts, so the sink
// slot is read here off the writer's `set_state_ref_sink` store. Hence the
// `std::atomic` indirection -- the pointer itself is the cross-thread datum; the
// `Editable::release` it lands on is the L3 contract's drain-thread half.
class ContentStateReclaimSink final : public ZeroCountSink {
public:
  ContentStateReclaimSink(RefStore<ObjectRecord>& records,
                          std::atomic<StateRefSink*>* sink) noexcept
      : d_records(&records), d_sink(sink) {}

  void on_zero(SlotIndex index) override {
    const ObjectRecord* r = d_records->peek_index(index);
    StateRefSink* const sink = d_sink->load(std::memory_order_acquire);
    if (r->kind == RecordKind::Content && r->as.content.state.has_state() && sink != nullptr) {
      sink->release(r->id, r->as.content.state);
    }
    d_records->enqueue_reclaim(index);
  }

private:
  RefStore<ObjectRecord>* d_records;
  std::atomic<StateRefSink*>* d_sink;
};

// The versioned scene model (doc 14): single writer, lock-free pinned reads.
// `DocState` is a path-copying persistent HAMT over `ObjectId`; records live as
// fixed-size slabs on a document-owned arena; a commit builds the next version by
// copying only the touched path and publishes it by an atomic swap of the
// current-version handle.
//
// Backing is a CONSTRUCTION-TIME arena policy (doc 15:158-160). `Model()` is the
// anonymous document: process memory, no file, no checkpointer, no slot fence --
// byte-for-byte the pre-durability model, which is what a live-only (OBS-style)
// host wants. `Model::create(path)` / `Model::open(path)` mint the WORKSPACE-BACKED
// document: records live in an mmapped per-document workspace file, `checkpoint()`
// makes the current version crash-recoverable (msync data, flip the A/B root, msync
// the header), and `open` rebuilds the whole document from the last durable root --
// counts and free lists included -- by the typed reachability walk below.
class ARBC_API Model {
public:
  Model();
  ~Model();

  Model(const Model&) = delete;
  Model& operator=(const Model&) = delete;

  // The root index a checkpoint publishes for an EMPTY document. Slot 0 is a
  // perfectly good HamtNode, so "no root" needs a value outside the index space;
  // the max `SlotIndex` is never a real slot (refs.hpp reserves it as the reclaim
  // sentinel for exactly this reason).
  static constexpr SlotIndex k_no_root = 0xFFFFFFFFu;

  // Mint a FRESH workspace-backed document over a newly created file at `path`
  // (doc 15:156-160): both document stores are minted from the file's per-store
  // chunk views, a `Checkpointer` is bound to the file + arena, and the
  // durability-epoch slot fence is installed on BOTH stores. The document starts
  // empty at revision 0, exactly as `Model()` does; nothing is durable until the
  // first `checkpoint()`. Errors -- including a platform with no workspace-file
  // support -- surface as values (doc 10), never as a throw or an abort.
  static expected<std::unique_ptr<Model>, WorkspaceFileError> create(const std::string& path);

  // RECOVERY (doc 15:163-166, "map the file, read the last valid root, resume").
  // Selects the highest-generation durable root, re-binds every store to exactly
  // the chunks the file's arena directory tags as its own at exactly the high-water
  // that root published, then runs the model's typed reachability walk over the
  // `DocState` HAMT to rebuild every reached slot's refcount from its in-degree --
  // no count is ever read from disk (doc 15:184-190) -- and derives each store's
  // free list as the below-high-water complement of the walk's live set. Publishes
  // the recovered version at revision 0: the durable root is the document, and the
  // in-memory journal is not reconstructed across a close/open (Decision 2 -- an
  // editor crash costs at-most-since-last-checkpoint, not the document). A file
  // that was created but never checkpointed has no durable root and recovers as an
  // empty document. A file whose store table disagrees with this build's slot
  // geometry, a truncated file, and an I/O fault all surface as `WorkspaceFileError`
  // values. WRITER-ONLY, and single-threaded: `open` runs before any reader pins
  // the recovered version.
  static expected<std::unique_ptr<Model>, WorkspaceFileError> open(const std::string& path);

  // Make the currently published version durable (doc 15:205-216): `Checkpointer::
  // commit` msyncs the live data chunks, publishes this version's root into the
  // inactive header slot with a bumped generation and flips, then msyncs the header
  // -- so a crash lands on the old root or the new one, both consistent. On success
  // the durability fences drain: the slots and chunks freed since the last
  // checkpoint, which the on-disk root may still have referenced, become reusable.
  //
  // A DECOUPLED PRIMITIVE, not a hook on the version publish: the per-transaction
  // `commit()` swap stays lock-free and msync-free, and checkpoint CADENCE (timer,
  // transaction count, explicit host call) is the host's / `runtime.housekeeping`'s
  // policy, not this layer's (doc 15:213-216). Mutates no live record -- records are
  // immutable -- so a reader pinned across a checkpoint observes an unchanged,
  // consistent version. WRITER-THREAD ONLY. On an anonymous `Model` there is no file
  // and nothing to make durable: returns `WorkspaceFileErrc::Unsupported`.
  expected<std::monostate, WorkspaceFileError> checkpoint();

  // Whether this document is workspace-file-backed (and so checkpointable).
  bool workspace_backed() const noexcept { return d_source != nullptr; }

  // The document's checkpointer, or null when anonymous. The seam
  // `runtime.housekeeping` drives for cadence, and the behavioral counters
  // (`commit_count`, `data_msyncs`, `header_msyncs`, `slots_freed_to_list`,
  // `generation`) a test asserts over instead of wall-clock (doc 16:54-62).
  Checkpointer* checkpointer() noexcept { return d_checkpointer ? &*d_checkpointer : nullptr; }

  // The document's workspace-file chunk source, or null when anonymous. The seam a
  // host queries for file-level accounting, and the one a crash-recovery harness
  // installs its `SyscallInjector` fault shim on (doc 16:74-78).
  WorkspaceFileChunkSource* workspace_source() noexcept { return d_source.get(); }

  // A persisted non-inert content-state handle the recovery walk found reachable
  // (model.persistent_state_walk_hook). The model cannot descend a kind-owned
  // state slab -- it holds only the opaque `slot`, and levelization forbids it
  // naming the kind (contract depends on model, never the reverse) -- so `open`
  // COLLECTS these and the runtime replays them through the per-kind Registry
  // walker once the `Document` and its kind stores exist. This is the deferred
  // half of the `StateRefSink` retain/release mirror: `Model::open` runs before
  // any Document, so the walk cannot dispatch into a live kind store inline.
  struct RecoveredContentState {
    ObjectId content;   // the content record that owns the handle
    std::uint64_t kind; // the record's kind id; the runtime maps it to the string id
    StateHandle state;  // the non-inert handle into the kind-owned state store
  };

  // The persisted non-inert content-state handles the last `open()` found
  // reachable, for the runtime to replay through per-kind state-slab walkers.
  // Empty for an anonymous model, a freshly created document, or a document whose
  // kinds ship only inert state (every kind today -- the list stays empty until
  // the first persistent workspace-backed kind lands). Read once, post-open, on
  // the single writer thread; never mutated after `open` returns.
  const std::vector<RecoveredContentState>& recovered_content_state() const noexcept {
    return d_recovered_content_state;
  }

  // Pin the current version; the returned handle is immutable and outlives any
  // later transaction (until the pin is dropped). Any thread.
  DocStatePtr current() const;

  // Allocate a fresh, document-unique object id (doc 14 § Identity). Any thread.
  ObjectId allocate_id();

  // Run deferred reclamation to quiescence. SINGLE-DRAINER, any thread: publishes
  // the reclamation context so node destructors can release their child edges,
  // then drains the cascade iteratively. THE reclaim seam -- every drainer must
  // come through here, never through the queue directly, or `~HamtNode` finds no
  // context and silently drops its child edges (hamt.hpp:103-109). A `Document`
  // enforces that by routing its background thread through
  // `ModelHousekeepingTarget` (`runtime/housekeeping_targets.hpp`).
  void drain();

  // Total live record + node slots across the document arena (doc 15:149-154 --
  // the per-arena live count exposed for a host memory panel, and the behavioral
  // witness of structural sharing and reclamation, doc 16:54-62).
  std::size_t live_slots() const noexcept;

  // Bytes the document arena has reserved from its chunk source -- the byte half
  // of doc 15:164-169's memory-panel accounting, beside `live_slots()`.
  std::size_t bytes_reserved() const noexcept;

  // Install the writer-owned commit / damage seams (single sink each; abstract,
  // model-defined per doc 02). A commit assembles the entry / damage union and
  // notifies; `model.journal` and `model.damage` register the concrete consumers
  // from above at wiring time (doc 17:66-72). Null clears. WRITER-THREAD ONLY.
  void set_commit_sink(CommitSink* sink) noexcept { d_commit_sink = sink; }
  void set_damage_sink(DamageSink* sink) noexcept { d_damage_sink = sink; }

  // Install the writer-owned content-state retain/release seam. Lifecycle rides
  // the record store the `Model` owns, so it registers here (not on `Journal`,
  // where cost/restore live -- refinement Decision 4). Null clears (inert).
  // WRITER-THREAD ONLY -- but the slot is READ on the drain thread (the reclaim
  // sink's `release`), so the pointer is published atomically.
  void set_state_ref_sink(StateRefSink* sink) noexcept {
    d_state_ref_sink.store(sink, std::memory_order_release);
  }

  // Direction of a navigation publish: undo restores each edit's *before* edge,
  // redo re-applies its *after* edge (doc 14:168-172).
  enum class NavDirection { Undo, Redo };

  // Publish an undo/redo version (WRITER-THREAD ONLY). The navigation primitive
  // `model.journal` drives: rebind each of `entry`'s touched objects to the
  // target-direction stored owning edge -- an EMPTY target `Ref` erases the id
  // (it did not exist in that direction); a non-empty target `hamt_insert`s its
  // slot, REUSING the entry's owning edge by `SlotRef` identity (the new leaf
  // takes its own count, the entry keeps its `Ref`; never a deep copy,
  // doc 14:175-178). Builds a `DocRoot` at revision +1 and swaps `d_current`
  // atomically -- an ordinary forward publish, so outputs/viewports need no
  // special path (doc 14:170-172) -- then flushes `entry.damage` once through the
  // installed `DamageSink`. It NEVER notifies the `CommitSink`: navigation moves a
  // cursor, it does not re-journal itself, so "history is never mutated"
  // (doc 14:43). Returns the sticky writer-path status; an allocation failure
  // leaves the current version in place (nothing observed).
  expected<std::monostate, PoolError> navigate(const JournalEntry& entry, NavDirection dir);

  // ARBC_API on its own: MSVC's __declspec(dllexport) on the enclosing Model does
  // NOT propagate to a nested class, so without this the shared arbc.dll omits every
  // Transaction method from its export table and downstream test/plugin images fail
  // to link (LNK2019). Inert in the static build (ARBC_API is empty) and on ELF
  // (visibility("default"), where the gcc-shared lane already resolved these).
  class ARBC_API Transaction {
  public:
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    Transaction(Transaction&&) = default;

    // Insert a new layer bound to `content`; returns its fresh id. No-op after a
    // prior allocation failure (the sticky error surfaces at `commit`).
    ObjectId add_layer(ObjectId content, const Affine& transform, double opacity = 1.0);

    // Insert a new content object of `kind` (its state handle is inert here).
    ObjectId add_content(std::uint64_t kind);

    // Insert a new composition object (canvas + empty layer order). Layer-order
    // population is `model.transactions`' concern; this lands the record shape.
    // The working space defaults to the doc 07 walking-skeleton format
    // (`k_working_rgba32f`); `set_working_space` configures it.
    ObjectId add_composition(double canvas_w, double canvas_h);

    // The same insert, but under a caller-PRE-ALLOCATED id (`Model::allocate_id`).
    // The serialization read path needs it (doc 08 Principle 7,
    // serialize.compositions_table Decision 4): a nested content cannot be
    // constructed without its child composition's `ObjectId`, but the child's
    // `CompositionRecord` cannot exist before its layers, which cannot exist before
    // their contents. `allocate_id()` is a bare monotonic counter bump that installs
    // no record, so the reader mints every reachable composition's id up front --
    // mutating no `DocState`, so a failed load still leaves the model at revision 0
    // -- and materializes the records here, inside `load_baseline`. Ids are never
    // reused, so a pre-allocated id cannot alias. Returns `id` (or `ObjectId{}` after
    // a sticky failure).
    ObjectId add_composition(ObjectId id, double canvas_w, double canvas_h);

    // Replace a composition's working space (path-copies its record + its map
    // path), the per-composition configuration of doc 07 rule 2. A configuration
    // change that invalidates every rendered pixel of the composition, so it
    // auto-damages the whole composition, all time, once at commit -- and bumps
    // the revision like any mutation. No-op if the composition is absent or not a
    // composition.
    void set_working_space(ObjectId composition, const SurfaceFormat& format);

    // Replace a composition's working audio format (path-copies its record + its
    // map path), the per-composition audio configuration of doc 12. The audio
    // twin of `set_working_space`: a configuration change that invalidates every
    // mixed sample of the composition, so it auto-damages the whole composition,
    // all time, once at commit -- and bumps the revision like any mutation. No-op
    // if the composition is absent or not a composition.
    void set_working_audio_format(ObjectId composition, const AudioFormat& format);

    // Replace an existing layer's transform (path-copies its record + its map
    // path). No-op if the layer is absent.
    void set_transform(ObjectId layer, const Affine& transform);

    // Replace an existing layer's opacity (path-copies its record + its map
    // path). A placement change (doc 01:137, "transform, opacity, order") that
    // auto-damages the whole layer, all time, once at commit. No-op if the layer
    // is absent or not a layer.
    void set_opacity(ObjectId layer, double opacity);

    // Replace an existing layer's audio placement gain (path-copies its record +
    // its map path), the additive-mix twin of `set_opacity` (doc 12:89-92). An
    // audio-placement change that auto-damages the whole layer, all time, once at
    // commit -- the audio revision space is the visual one (doc 12:208), so the
    // whole-object/all-time damage covers the layer's mixed samples too. No-op if
    // the layer is absent or not a layer.
    void set_gain(ObjectId layer, double gain);

    // Set or clear an existing layer's audibility (the `k_layer_audible` flag bit,
    // path-copies its record + its map path), the audio twin of the `visible`
    // flag (doc 12:89-92). Auto-damages the whole layer, all time, once at commit.
    // No-op if the layer is absent or not a layer.
    void set_audible(ObjectId layer, bool audible);

    // Set or clear an existing layer's visibility (the `k_layer_visible` flag bit,
    // path-copies its record + its map path), the visual twin of `set_audible`
    // (doc 01:137, "transform, opacity, order"). Auto-damages the whole layer, all
    // time, once at commit. No-op if the layer is absent or not a layer.
    void set_visible(ObjectId layer, bool visible);

    // Replace an existing layer's temporal span (path-copies its record + its
    // map path), the parent-time interval `[in, out)` the layer exists over
    // (doc 11:59-73). The temporal sibling of `set_transform`: it stores the
    // span, it does not cull by it. Auto-damages the whole layer, all time, once
    // at commit. No-op if the layer is absent or not a layer.
    void set_span(ObjectId layer, const TimeRange& span);

    // Replace an existing layer's time map (path-copies its record + its map
    // path), the 1D affine map from parent time to content-local time; a
    // negative rate is first-class reverse playback (doc 11:65-71). Stores the
    // map, it does not compose or round it. Auto-damages the whole layer, all
    // time, once at commit. No-op if the layer is absent or not a layer.
    void set_time_map(ObjectId layer, const TimeMap& time_map);

    // Insert `layer` into `composition`'s ordered membership at `at_index`
    // (bottom-to-top, doc 01:6-11); an `at_index` at or past the current count
    // appends at the top. The order lives inline while it fits in
    // `k_max_inline_layers`, spilling to a HAMT chunk chain past the cap
    // (`model.composition_membership`, doc 14). Path-copies the composition record
    // and only the touched spill chunks, damages the composition once. No-op if
    // the composition or the layer is absent (or after a prior sticky failure).
    void attach_layer(ObjectId composition, ObjectId layer, std::uint32_t at_index);
    // Convenience: append `layer` at the top of `composition`'s order.
    void attach_layer(ObjectId composition, ObjectId layer);

    // Remove `layer` from `composition`'s ordered membership (dropping back to the
    // inline representation when the count falls to `k_max_inline_layers`).
    // Path-copies the composition record and the touched chunks, damages the
    // composition once. No-op if the composition is absent or `layer` is not a
    // member.
    void detach_layer(ObjectId composition, ObjectId layer);

    // Move the member at `from_index` to `to_index` (a stable move: the relative
    // order of the untouched members is preserved). Damages the composition once.
    // No-op if the composition is absent, either index is out of range, or the
    // indices are equal.
    void reorder_layer(ObjectId composition, std::uint32_t from_index, std::uint32_t to_index);

    // Assign a caller-captured, OPAQUE content-state handle to a content object
    // (path-copies its record + its map path) and record the prior handle as the
    // entry's *before* (doc 14:133-135). The transaction never calls
    // `Editable::capture()` -- the handle crosses the L2/L3 boundary opaquely
    // (doc 17). No-op if `content` is absent or not a content object.
    void set_content_state(ObjectId content, StateHandle after);

    // Remove an object: a `hamt_erase` path-copy that shares untouched siblings
    // and collapses emptied branches. Enables `model.journal`'s inverse of an
    // add. No-op if the id is absent.
    void remove(ObjectId id);

    // Stamp a non-zero gesture key onto the emitted entry so `model.journal`
    // merges consecutive commits into one undo unit (doc 14:86-91); each
    // coalesced commit still publishes. `0` == no coalescing.
    Transaction& coalesce(CoalesceKey key);

    // Union `d` into the per-transaction damage set (dedup by object, both axes
    // under empty=identity / whole=absorbing). Structural mutators auto-contribute
    // a whole-object / all-time damage (level-forced over-approximation, doc 17);
    // a caller above (a kind's L3 `Editable`) adds the precise content region and
    // time-range here. Flushed once at commit; abort flushes nothing.
    void add_damage(const Damage& d);

    // Publish the built version by an atomic swap of the current-version handle.
    // Observers see the old or the new root, never a half-edit (doc 14:83-85).
    // When a `CommitSink`/`DamageSink` is installed, assembles ONE journal entry
    // and flushes the damage union exactly once. Returns the sticky writer-path
    // status (an allocation failure aborts the publish and leaves the current
    // version in place).
    expected<std::monostate, PoolError> commit();

    // Discard the transaction: publish nothing (current version + revision
    // unchanged), reclaim the working records via the existing reclamation queue,
    // emit no entry and no damage (doc 14 §Transactions Abort). A dropped (never
    // committed) transaction aborts implicitly through member destruction.
    void abort();

  private:
    friend class Model;
    Transaction(Model& model, std::string name);

    // Note `id` as touched so `commit()` assembles its (before, after) edge.
    void touch(ObjectId id);

    // Stamp a record this transaction just path-copied with the revision this
    // transaction will PUBLISH (`model.per_object_revision` Decision 1). Called on the
    // fresh record inside every mutator's existing `create -> copy -> override ->
    // insert -> touch` sequence, which is what makes the stamp cost no traversal and
    // perturb no structural sharing: a record the commit did not path-copy is never
    // written, so it keeps its old stamp for free
    // (14-data-model-and-editing#commit-stamps-only-touched-objects).
    void stamp(ObjectRecord& record) const noexcept { record.revision = d_publish_revision; }

    // Path-copy `composition`'s membership in the working tree to `new_order`,
    // sharing every spill chunk whose members and `next` edge are unchanged and
    // creating/erasing chunk records only at the touched tail (so live-slot growth
    // is O(touched chunks + path depth), not O(members)). Collapses to the inline
    // representation when `new_order` fits `k_max_inline_layers`. Touches the
    // composition and every rewritten/erased chunk. Sets `d_status` and returns
    // false on an allocation failure. `base` is a value copy of the composition
    // record to rewrite from; `old_order`/`old_chunk_ids` are its current
    // membership + chunk-id chain (empty chain when inline).
    bool store_membership(ObjectId composition, const ObjectRecord& base,
                          const std::vector<ObjectId>& old_order,
                          const std::vector<ObjectId>& old_chunk_ids,
                          const std::vector<ObjectId>& new_order);

    Model* d_model;
    std::string d_name;
    CoalesceKey d_coalesce{k_no_coalesce};
    DocStatePtr d_base;   // pinned base version: the source of before-edges
    Ref<HamtNode> d_root; // working-tree root (owning); empty == empty map
    std::uint64_t d_base_revision;
    // The revision this transaction's publish will carry, and therefore the stamp it
    // mints into every record it path-copies. `d_base_revision + 1` for an ordinary
    // commit; `load_baseline` re-points it at 0, because a doc-08 load PUBLISHES
    // revision 0 (08-serialization#load-installs-version-0-baseline) and a record
    // stamped 1 under a document opened at 0 would collide with the first real commit's
    // stamp on any object that commit touches -- two different renderings of one object
    // under one key.
    std::uint64_t d_publish_revision;
    std::vector<ObjectId> d_touched;
    std::vector<ContentStateEdit> d_contents;
    std::vector<Damage> d_damage;
    bool d_open{true}; // false once committed or aborted: commit() is then inert
    expected<std::monostate, PoolError> d_status;
  };

  Transaction transact(std::string name = {});

  // Install a caller-reconstructed graph as the document's version-0 baseline
  // with an EMPTY journal -- the serialization load seam (doc 08; doc 14:263-264,
  // "load constructs a fresh document at version 0"). `build` populates a fresh
  // transaction with the reconstructed composition + layers (add_composition /
  // add_layer / attach_layer / set_*); `load_baseline` then publishes the built
  // root at `revision 0` WITHOUT notifying the `CommitSink`, so no journal entry
  // is recorded and an undo immediately after load is a no-op that never reverts
  // the freshly-loaded document to empty (doc 14:40-43 -- a clean baseline
  // install, not a journal truncation; serialize.reader Decision 3, following the
  // writer's precedent of adding find_first_composition / set_visible from a
  // serialize task). The `DamageSink`, if installed, is flushed once so a
  // subscribed output repaints the loaded document. Precondition: a freshly
  // constructed `Model` (revision 0, empty). Returns the sticky writer-path
  // status; an allocation failure leaves the current (empty) version in place,
  // nothing observed. WRITER-THREAD ONLY.
  expected<std::monostate, PoolError> load_baseline(const std::function<void(Transaction&)>& build);

private:
  friend class Transaction;

  // The two factories construct through this; a workspace `Model` is not
  // default-constructible, because a file open can fail and a constructor cannot
  // return a value.
  struct WorkspaceTag {};
  Model(WorkspaceTag, std::unique_ptr<WorkspaceFileChunkSource> source);

  // The construction tail both constructors share: register the deferred sinks and
  // publish the empty version-0 root.
  void wire_stores();

  // The workspace-only construction tail: bind the checkpointer to the file + arena
  // and install the durability-epoch slot fence on BOTH document stores.
  void install_durability();

  // The typed reachability walk `Checkpointer::finalize_open` requires -- the
  // deliverable `pool.checkpoints` left to the model, because `pool` (L1) must not
  // learn about `DocState` (L2, doc 17:41-43). The ENTIRE ownership graph is the
  // HAMT: every `ObjectRecord` arm (composition / layer / content / spill chunk) is
  // a HAMT leaf keyed by its own `ObjectId`, and records name each other only by
  // `ObjectId` VALUE, never an owning edge (records.hpp:104-105, doc 14:58-64). So
  // the walk follows exactly the counted `SlotRef` edges -- `HamtNode` -> child
  // `HamtNode` and `HamtNode` -> leaf `ObjectRecord` -- and there is nothing else to
  // follow. It reads records by raw storage index and asserts no `SlotRef`
  // generation (refs.hpp:375-378: generations are anonymous and reset on open).
  struct Recovered {
    std::vector<SlotIndex> nodes;   // reachable HamtNode slots (d_nodes' live set)
    std::vector<SlotIndex> records; // reachable ObjectRecord slots (d_records' live set)
    std::uint64_t max_id{0};        // the highest reachable ObjectId: reseeds d_next_id
    // The highest reachable PER-OBJECT REVISION STAMP: reseeds the document revision
    // (`model.per_object_revision` Decision 2, 15-memory-model#recovery-resumes-above-
    // every-persisted-stamp). The stamp lives IN the record, so it persists into the
    // workspace file -- while a recovered document would otherwise reopen at revision
    // 0. Left alone that is a wrong-pixel path: after 500 commits in the recovered
    // session a new commit would mint stamp 500 for object X, while X's RECOVERED
    // record -- still reachable from a journal before-edge or a pinned snapshot, and
    // carrying different content -- also reads 500. The walk already visits every
    // record to rebuild its refcount, so the max is free.
    std::uint64_t max_revision{0};
    // Every reachable content record carrying a non-inert `StateHandle`, in walk
    // order. The walk cannot descend a kind-owned state slab (see
    // `recovered_content_state`), so it collects here and `open` hands the list to
    // the Model for the runtime to replay. Empty while no kind persists state.
    std::vector<RecoveredContentState> content_state;
  };
  Recovered rebuild_counts(SlotIndex root_index);

  // Declaration order is init order: the arena backs the stores; the stores back
  // the sinks; the queue and reclaim context reference the stores. Destroyed in
  // reverse, so nothing outlives what it points at.
  //
  // The workspace source and its checkpointer lead, so they OUTLIVE the arena and
  // stores they back: teardown returns the stores' chunks through the source (still
  // alive) and through the checkpointer's chunk fence (still alive), and the slot
  // fence the stores point at is destroyed only after them. Both are null/empty on
  // the anonymous path -- which then constructs, allocates, and tears down exactly
  // as it did before durability existed (`checkpoint.hpp:52-57`: the fence is off by
  // default so anonymous paths stay byte-for-byte unchanged).
  std::unique_ptr<WorkspaceFileChunkSource> d_source; // null == anonymous
  std::optional<Checkpointer> d_checkpointer;         // engaged iff d_source

  Arena d_arena;
  RefStore<HamtNode> d_nodes;
  RefStore<ObjectRecord> d_records;
  StoreBundle d_bundle;
  DeferredReclaimSink<HamtNode> d_node_sink;
  DeferredReclaimSink<ObjectRecord> d_record_sink;
  ReclamationQueue d_queue;
  ReclaimContext d_reclaim_ctx;

  std::atomic<std::uint64_t> d_next_id{1};
  std::atomic<DocStatePtr> d_current;

  // Persisted non-inert content-state handles the last `open()` collected, for the
  // runtime to replay through per-kind state-slab walkers (see
  // `recovered_content_state`). Written once at the tail of `open`, on the single
  // writer thread, before any reader pins the recovered version; empty otherwise.
  std::vector<RecoveredContentState> d_recovered_content_state;

  // Writer-owned single sinks (doc 02, doc 17:66-72): the transaction notifies
  // through these; the journal / damage-propagation consumers register above.
  CommitSink* d_commit_sink{nullptr};
  DamageSink* d_damage_sink{nullptr};
  // L3 content-state retain/release seam (null == inert). Installed by the writer,
  // read by the drainer -- which since `runtime.housekeeping_document_wiring` is a
  // different thread, so the slot is atomic.
  std::atomic<StateRefSink*> d_state_ref_sink{nullptr};
  // The ObjectRecord zero-count sink that releases a reclaimed content record's
  // embedded `StateHandle` before deferring the slot. Constructed with the record
  // store and the address of `d_state_ref_sink`, and installed over the plain
  // deferred sink in the constructor. Declared last so the members it references
  // are already alive.
  ContentStateReclaimSink d_content_reclaim_sink{d_records, &d_state_ref_sink};
};

} // namespace arbc
