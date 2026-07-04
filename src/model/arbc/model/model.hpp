#pragma once

#include <arbc/base/ids.hpp>
#include <arbc/base/transform.hpp>

#include <atomic>
#include <memory>
#include <vector>

namespace arbc {

// Layer placement in its parent composition (doc 01). Content is referenced
// by opaque id; the binding to Content objects lives in the runtime
// component (doc 17).
struct LayerRecord {
  ObjectId id;
  ObjectId content;
  Affine transform;
  double opacity{1.0};
  bool visible{true};
};

// One immutable document version (doc 14). Never mutated after publish.
struct DocState {
  std::uint64_t revision{0};
  std::vector<LayerRecord> layers; // bottom to top
};

using DocStatePtr = std::shared_ptr<const DocState>;

// The versioned scene model: single writer, lock-free pinned reads
// (doc 14). Walking-skeleton subset: whole-state copy-on-commit stands in
// for the persistent structure; the journal, coalescing, and the slab
// arenas (doc 15) land next.
class Model {
public:
  Model();

  // Pin the current version; the returned state is immutable and outlives
  // any later transaction.
  DocStatePtr current() const;

  ObjectId allocate_id();

  class Transaction {
  public:
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    ObjectId add_layer(ObjectId content, const Affine& transform, double opacity = 1.0);
    void set_transform(ObjectId layer, const Affine& transform);
    void commit();

  private:
    friend class Model;
    explicit Transaction(Model& model);

    Model* d_model;
    std::shared_ptr<DocState> d_next;
  };

  Transaction transact();

private:
  std::atomic<std::uint64_t> d_next_id{1};
  std::atomic<DocStatePtr> d_current;
};

} // namespace arbc
