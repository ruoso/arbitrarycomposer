#pragma once

#include <arbc/base/expected.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/model/hamt.hpp>
#include <arbc/model/records.hpp>
#include <arbc/pool/reclamation.hpp>
#include <arbc/pool/refs.hpp>
#include <arbc/pool/slot_store.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <variant>

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

  // Visit every layer record in ascending object-id order. Object ids are
  // assigned monotonically, so ascending-id order reproduces the walking-
  // skeleton's insertion order (bottom-to-top, doc 02); explicit layer reorder
  // is `model.transactions`' concern. Refcount-free peek traversal.
  void for_each_layer(const std::function<void(const LayerRecord&)>& fn) const;

  // The object-record edge `id` is bound to (the HAMT leaf's `SlotRef`). Exposed
  // so a test can witness that an untouched object is shared by `SlotRef`
  // identity between two versions (14-data-model-and-editing#commit-shares-
  // untouched-structure). Returns false when absent.
  bool record_edge(ObjectId id, SlotRef<ObjectRecord>& out) const;

  // Internal: the owning root reference a new transaction forks from.
  const Ref<HamtNode>& root_ref() const noexcept { return d_root; }

private:
  StoreBundle d_stores;
  Ref<HamtNode> d_root; // owning: one count on the version root (empty == empty map)
  std::uint64_t d_revision;
};

using DocStatePtr = std::shared_ptr<const DocRoot>;

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
    ObjectId add_composition(double canvas_w, double canvas_h);

    // Replace an existing layer's transform (path-copies its record + its map
    // path). No-op if the layer is absent.
    void set_transform(ObjectId layer, const Affine& transform);

    // Publish the built version by an atomic swap of the current-version handle.
    // Observers see the old or the new root, never a half-edit (doc 14:83-85).
    // Returns the sticky writer-path status (an allocation failure aborts the
    // publish and leaves the current version in place).
    expected<std::monostate, PoolError> commit();

  private:
    friend class Model;
    explicit Transaction(Model& model);

    Model* d_model;
    Ref<HamtNode> d_root; // working-tree root (owning); empty == empty map
    std::uint64_t d_base_revision;
    expected<std::monostate, PoolError> d_status;
  };

  Transaction transact();

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
};

} // namespace arbc
