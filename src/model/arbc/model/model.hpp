#pragma once

#include <arbc/base/expected.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/rational_time.hpp> // TimeRange span / TimeMap time_map setter params
#include <arbc/base/transform.hpp>
#include <arbc/model/damage.hpp>
#include <arbc/model/hamt.hpp>
#include <arbc/model/journal_entry.hpp>
#include <arbc/model/records.hpp>
#include <arbc/pool/reclamation.hpp>
#include <arbc/pool/refs.hpp>
#include <arbc/pool/slot_store.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
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
class DocRoot {
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
class StateRefSink {
public:
  virtual ~StateRefSink() = default;
  virtual void retain(StateHandle handle) = 0;
  virtual void release(StateHandle handle) = 0;
};

// The ObjectRecord store's zero-count sink: at the zero-count-reclaim boundary of
// a content `ObjectRecord`, release its embedded `StateHandle` through the
// registered `StateRefSink` (the record is still readable -- reclamation is
// deferred), then hand the slot to the deferred reclamation queue exactly as the
// plain `DeferredReclaimSink` would. This is what makes the `StateHandle`
// lifecycle ride the record slot without a non-trivial `~ObjectRecord`
// (records.hpp:12-19, refinement Decision 2). Reached only on the single drain
// thread (every ObjectRecord count decrement is writer/drain-thread), so the
// L3 `release` stays writer-thread-only.
class ContentStateReclaimSink final : public ZeroCountSink {
public:
  ContentStateReclaimSink(RefStore<ObjectRecord>& records, StateRefSink** sink) noexcept
      : d_records(&records), d_sink(sink) {}

  void on_zero(SlotIndex index) override {
    const ObjectRecord* r = d_records->peek_index(index);
    if (r->kind == RecordKind::Content && r->as.content.state.has_state() && *d_sink != nullptr) {
      (*d_sink)->release(r->as.content.state);
    }
    d_records->enqueue_reclaim(index);
  }

private:
  RefStore<ObjectRecord>* d_records;
  StateRefSink** d_sink;
};

// The versioned scene model (doc 14): single writer, lock-free pinned reads.
// `DocState` is a path-copying persistent HAMT over `ObjectId`; records live as
// fixed-size slabs on a document-owned arena; a commit builds the next version by
// copying only the touched path and publishes it by an atomic swap of the
// current-version handle.
class Model {
public:
  Model();
  ~Model();

  Model(const Model&) = delete;
  Model& operator=(const Model&) = delete;

  // Pin the current version; the returned handle is immutable and outlives any
  // later transaction (until the pin is dropped). Any thread.
  DocStatePtr current() const;

  // Allocate a fresh, document-unique object id (doc 14 § Identity). Any thread.
  ObjectId allocate_id();

  // Run deferred reclamation to quiescence. WRITER-ONLY / single-drainer (between
  // transactions): publishes the reclamation context so node destructors can
  // release their child edges, then drains the cascade iteratively.
  void drain();

  // Total live record + node slots across the document arena (doc 15:149-154 --
  // the per-arena live count exposed for a host memory panel, and the behavioral
  // witness of structural sharing and reclamation, doc 16:54-62).
  std::size_t live_slots() const noexcept;

  // Install the writer-owned commit / damage seams (single sink each; abstract,
  // model-defined per doc 02). A commit assembles the entry / damage union and
  // notifies; `model.journal` and `model.damage` register the concrete consumers
  // from above at wiring time (doc 17:66-72). Null clears. WRITER-THREAD ONLY.
  void set_commit_sink(CommitSink* sink) noexcept { d_commit_sink = sink; }
  void set_damage_sink(DamageSink* sink) noexcept { d_damage_sink = sink; }

  // Install the writer-owned content-state retain/release seam. Lifecycle rides
  // the record store the `Model` owns, so it registers here (not on `Journal`,
  // where cost/restore live -- refinement Decision 4). Null clears (inert).
  // WRITER-THREAD ONLY.
  void set_state_ref_sink(StateRefSink* sink) noexcept { d_state_ref_sink = sink; }

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

  class Transaction {
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

  // Declaration order is init order: the arena backs the stores; the stores back
  // the sinks; the queue and reclaim context reference the stores. Destroyed in
  // reverse, so nothing outlives what it points at.
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

  // Writer-owned single sinks (doc 02, doc 17:66-72): the transaction notifies
  // through these; the journal / damage-propagation consumers register above.
  CommitSink* d_commit_sink{nullptr};
  DamageSink* d_damage_sink{nullptr};
  // L3 content-state retain/release seam (writer-thread only; null == inert).
  StateRefSink* d_state_ref_sink{nullptr};
  // The ObjectRecord zero-count sink that releases a reclaimed content record's
  // embedded `StateHandle` before deferring the slot. Constructed with the record
  // store and the address of `d_state_ref_sink`, and installed over the plain
  // deferred sink in the constructor. Declared last so the members it references
  // are already alive.
  ContentStateReclaimSink d_content_reclaim_sink{d_records, &d_state_ref_sink};
};

} // namespace arbc
