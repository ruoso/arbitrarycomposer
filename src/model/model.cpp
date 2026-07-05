#include <arbc/model/model.hpp>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <utility>
#include <vector>

namespace arbc {
namespace {

// A stored `SlotRef` edge holds one count on its target (holder-holds-a-count).
// A 32-bit refcount saturating (2^32 edges to one node) is astronomically
// unlikely, but per doc 10 it surfaces as a value rather than wrapping silently;
// map it onto the writer-path error channel.
expected<std::monostate, PoolError> retain_node(StoreBundle& sb, SlotRef<HamtNode> ref) {
  if (!sb.nodes->retain(ref)) {
    return unexpected(PoolError::CapacityExhausted);
  }
  return std::monostate{};
}

expected<std::monostate, PoolError> retain_record(StoreBundle& sb, SlotRef<ObjectRecord> ref) {
  if (!sb.records->retain(ref)) {
    return unexpected(PoolError::CapacityExhausted);
  }
  return std::monostate{};
}

// Allocate a fresh leaf binding `key -> record`. The leaf takes its OWN count on
// the record (the caller keeps its create-count and drops it after the insert).
expected<Ref<HamtNode>, PoolError> make_leaf(StoreBundle& sb, std::uint64_t key,
                                             SlotRef<ObjectRecord> record) {
  expected<Ref<HamtNode>, PoolError> node = sb.nodes->create();
  if (!node) {
    return node;
  }
  if (expected<std::monostate, PoolError> retained = retain_record(sb, record); !retained) {
    return unexpected(retained.error());
  }
  HamtNode& n = **node;
  n.is_leaf = 1;
  n.key = key;
  n.record = record;
  return node;
}

// Build a new branch: `base` (peeked; may be null for a fresh branch) with
// child[digit] set to `new_child`, and every OTHER present child of `base` shared
// (retained). The new branch adopts `new_child`'s single count via a retain; the
// caller's `new_child` Ref then drops, leaving exactly one count owned by the
// branch edge. Untouched siblings are shared by `SlotRef` identity -- structural
// sharing (14-data-model-and-editing#commit-shares-untouched-structure).
expected<Ref<HamtNode>, PoolError> branch_with_child(StoreBundle& sb, const HamtNode* base,
                                                     std::uint32_t digit, Ref<HamtNode> new_child) {
  expected<Ref<HamtNode>, PoolError> node = sb.nodes->create();
  if (!node) {
    return node;
  }
  HamtNode& n = **node;
  n.is_leaf = 0;
  n.bitmap = 0;
  if (base != nullptr) {
    std::uint32_t remaining = base->bitmap;
    while (remaining != 0) {
      const std::uint32_t i = static_cast<std::uint32_t>(std::countr_zero(remaining));
      remaining &= remaining - 1;
      if (i == digit) {
        continue; // superseded by new_child below
      }
      if (expected<std::monostate, PoolError> retained = retain_node(sb, base->children[i]);
          !retained) {
        return unexpected(retained.error());
      }
      n.children[i] = base->children[i];
      n.bitmap |= (std::uint32_t{1} << i);
    }
  }
  n.children[digit] = new_child.slot();
  if (expected<std::monostate, PoolError> retained = retain_node(sb, new_child.slot()); !retained) {
    return unexpected(retained.error());
  }
  n.bitmap |= (std::uint32_t{1} << digit);
  return node;
}

expected<Ref<HamtNode>, PoolError> insert_rec(StoreBundle& sb, SlotRef<HamtNode> node_slot,
                                              std::uint64_t key, SlotRef<ObjectRecord> record,
                                              std::uint32_t shift);

// Two distinct keys share the prefix below `shift`. Descend, creating a
// single-child branch per still-colliding level, until they diverge into a
// two-child branch; the existing key's leaf is SHARED (retained) unchanged.
expected<Ref<HamtNode>, PoolError> split(StoreBundle& sb, SlotRef<HamtNode> existing_leaf,
                                         std::uint64_t existing_key, std::uint64_t new_key,
                                         SlotRef<ObjectRecord> record, std::uint32_t shift) {
  const std::uint32_t ed = static_cast<std::uint32_t>((existing_key >> shift) & k_hamt_mask);
  const std::uint32_t nd = static_cast<std::uint32_t>((new_key >> shift) & k_hamt_mask);
  if (ed == nd) {
    expected<Ref<HamtNode>, PoolError> inner =
        split(sb, existing_leaf, existing_key, new_key, record, shift + k_hamt_bits);
    if (!inner) {
      return inner;
    }
    return branch_with_child(sb, nullptr, ed, std::move(*inner));
  }
  expected<Ref<HamtNode>, PoolError> new_leaf = make_leaf(sb, new_key, record);
  if (!new_leaf) {
    return new_leaf;
  }
  expected<Ref<HamtNode>, PoolError> branch =
      branch_with_child(sb, nullptr, nd, std::move(*new_leaf));
  if (!branch) {
    return branch;
  }
  if (expected<std::monostate, PoolError> retained = retain_node(sb, existing_leaf); !retained) {
    return unexpected(retained.error());
  }
  HamtNode& b = **branch;
  b.children[ed] = existing_leaf;
  b.bitmap |= (std::uint32_t{1} << ed);
  return branch;
}

expected<Ref<HamtNode>, PoolError> insert_rec(StoreBundle& sb, SlotRef<HamtNode> node_slot,
                                              std::uint64_t key, SlotRef<ObjectRecord> record,
                                              std::uint32_t shift) {
  const HamtNode* node = sb.nodes->peek(node_slot);
  if (node->is_leaf != 0) {
    if (node->key == key) {
      return make_leaf(sb, key, record); // replace binding; old leaf stays in the old tree
    }
    return split(sb, node_slot, node->key, key, record, shift);
  }
  const std::uint32_t digit = static_cast<std::uint32_t>((key >> shift) & k_hamt_mask);
  const std::uint32_t bit = std::uint32_t{1} << digit;
  if ((node->bitmap & bit) != 0) {
    expected<Ref<HamtNode>, PoolError> new_child =
        insert_rec(sb, node->children[digit], key, record, shift + k_hamt_bits);
    if (!new_child) {
      return new_child;
    }
    return branch_with_child(sb, node, digit, std::move(*new_child));
  }
  expected<Ref<HamtNode>, PoolError> new_leaf = make_leaf(sb, key, record);
  if (!new_leaf) {
    return new_leaf;
  }
  return branch_with_child(sb, node, digit, std::move(*new_leaf));
}

// Collect every (key, record-edge) leaf binding reachable from `node_slot`.
void collect_leaves(const StoreBundle& sb, SlotRef<HamtNode> node_slot,
                    std::vector<std::pair<std::uint64_t, SlotRef<ObjectRecord>>>& out) {
  const HamtNode* node = sb.nodes->peek(node_slot);
  if (node->is_leaf != 0) {
    out.emplace_back(node->key, node->record);
    return;
  }
  std::uint32_t remaining = node->bitmap;
  while (remaining != 0) {
    const std::uint32_t digit = static_cast<std::uint32_t>(std::countr_zero(remaining));
    remaining &= remaining - 1;
    collect_leaves(sb, node->children[digit], out);
  }
}

} // namespace

expected<Ref<HamtNode>, PoolError> hamt_insert(StoreBundle& sb, const Ref<HamtNode>& root,
                                               std::uint64_t key, SlotRef<ObjectRecord> record) {
  if (!root) {
    return make_leaf(sb, key, record);
  }
  return insert_rec(sb, root.slot(), key, record, 0);
}

bool hamt_lookup(const StoreBundle& sb, const Ref<HamtNode>& root, std::uint64_t key,
                 SlotRef<ObjectRecord>& out) {
  if (!root) {
    return false;
  }
  SlotRef<HamtNode> cur = root.slot();
  std::uint32_t shift = 0;
  for (;;) {
    const HamtNode* n = sb.nodes->peek(cur);
    if (n->is_leaf != 0) {
      if (n->key == key) {
        out = n->record;
        return true;
      }
      return false;
    }
    const std::uint32_t digit = static_cast<std::uint32_t>((key >> shift) & k_hamt_mask);
    const std::uint32_t bit = std::uint32_t{1} << digit;
    if ((n->bitmap & bit) == 0) {
      return false;
    }
    cur = n->children[digit];
    shift += k_hamt_bits;
  }
}

// ---- DocRoot reads (lock-free, refcount-free peek traversal) ----------------

bool DocRoot::record_edge(ObjectId id, SlotRef<ObjectRecord>& out) const {
  return hamt_lookup(d_stores, d_root, id.value, out);
}

bool DocRoot::contains(ObjectId id) const {
  SlotRef<ObjectRecord> edge;
  return hamt_lookup(d_stores, d_root, id.value, edge);
}

const LayerRecord* DocRoot::find_layer(ObjectId id) const {
  SlotRef<ObjectRecord> edge;
  if (!hamt_lookup(d_stores, d_root, id.value, edge)) {
    return nullptr;
  }
  const ObjectRecord* r = d_stores.records->peek(edge);
  return r->kind == RecordKind::Layer ? &r->as.layer : nullptr;
}

const ContentRecord* DocRoot::find_content(ObjectId id) const {
  SlotRef<ObjectRecord> edge;
  if (!hamt_lookup(d_stores, d_root, id.value, edge)) {
    return nullptr;
  }
  const ObjectRecord* r = d_stores.records->peek(edge);
  return r->kind == RecordKind::Content ? &r->as.content : nullptr;
}

const CompositionRecord* DocRoot::find_composition(ObjectId id) const {
  SlotRef<ObjectRecord> edge;
  if (!hamt_lookup(d_stores, d_root, id.value, edge)) {
    return nullptr;
  }
  const ObjectRecord* r = d_stores.records->peek(edge);
  return r->kind == RecordKind::Composition ? &r->as.composition : nullptr;
}

void DocRoot::for_each_layer(const std::function<void(const LayerRecord&)>& fn) const {
  if (!d_root) {
    return;
  }
  std::vector<std::pair<std::uint64_t, SlotRef<ObjectRecord>>> leaves;
  collect_leaves(d_stores, d_root.slot(), leaves);
  std::sort(leaves.begin(), leaves.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  for (const auto& [key, edge] : leaves) {
    const ObjectRecord* r = d_stores.records->peek(edge);
    if (r->kind == RecordKind::Layer) {
      fn(r->as.layer);
    }
  }
}

// ---- Model ------------------------------------------------------------------

Model::Model()
    : d_nodes(d_arena), d_records(d_arena), d_bundle{&d_nodes, &d_records}, d_node_sink(d_nodes),
      d_record_sink(d_records), d_reclaim_ctx{&d_nodes, &d_records} {
  // Install the deferred sinks so any release (including a render thread dropping
  // a pin) only enqueues -- never a destructor storm inline (doc 15:129-136).
  d_queue.register_store(d_nodes, d_node_sink);
  d_queue.register_store(d_records, d_record_sink);
  d_current.store(std::make_shared<const DocRoot>(d_bundle, Ref<HamtNode>{}, 0));
}

Model::~Model() {
  // Drop the current version's root, then drain the cascade with the reclamation
  // context published so node destructors can release their child edges. Any
  // pins outstanding on other threads must already be released (precondition).
  d_current.store(nullptr);
  ReclaimContextGuard guard(d_reclaim_ctx);
  d_queue.drain();
}

DocStatePtr Model::current() const { return d_current.load(); }

ObjectId Model::allocate_id() { return ObjectId{d_next_id.fetch_add(1)}; }

void Model::drain() {
  ReclaimContextGuard guard(d_reclaim_ctx);
  d_queue.drain();
}

std::size_t Model::live_slots() const noexcept { return d_arena.total_slots_live(); }

Model::Transaction Model::transact() { return Transaction(*this); }

// ---- Transaction ------------------------------------------------------------

Model::Transaction::Transaction(Model& model) : d_model(&model), d_status(std::monostate{}) {
  const DocStatePtr current = model.current();
  d_root = current->root_ref(); // fork: pin the current version's root
  d_base_revision = current->revision();
}

ObjectId Model::Transaction::add_layer(ObjectId content, const Affine& transform, double opacity) {
  if (!d_status) {
    return ObjectId{};
  }
  const ObjectId id = d_model->allocate_id();
  expected<Ref<ObjectRecord>, PoolError> rec = d_model->d_records.create();
  if (!rec) {
    d_status = unexpected(rec.error());
    return ObjectId{};
  }
  ObjectRecord& r = **rec;
  r.kind = RecordKind::Layer;
  r.id = id;
  r.as.layer = LayerRecord{content, transform, opacity, k_layer_visible};

  expected<Ref<HamtNode>, PoolError> next =
      hamt_insert(d_model->d_bundle, d_root, id.value, rec->slot());
  if (!next) {
    d_status = unexpected(next.error());
    return ObjectId{};
  }
  d_root = std::move(*next);
  return id;
}

ObjectId Model::Transaction::add_content(std::uint64_t kind) {
  if (!d_status) {
    return ObjectId{};
  }
  const ObjectId id = d_model->allocate_id();
  expected<Ref<ObjectRecord>, PoolError> rec = d_model->d_records.create();
  if (!rec) {
    d_status = unexpected(rec.error());
    return ObjectId{};
  }
  ObjectRecord& r = **rec;
  r.kind = RecordKind::Content;
  r.id = id;
  r.as.content = ContentRecord{kind, StateHandle{}};

  expected<Ref<HamtNode>, PoolError> next =
      hamt_insert(d_model->d_bundle, d_root, id.value, rec->slot());
  if (!next) {
    d_status = unexpected(next.error());
    return ObjectId{};
  }
  d_root = std::move(*next);
  return id;
}

ObjectId Model::Transaction::add_composition(double canvas_w, double canvas_h) {
  if (!d_status) {
    return ObjectId{};
  }
  const ObjectId id = d_model->allocate_id();
  expected<Ref<ObjectRecord>, PoolError> rec = d_model->d_records.create();
  if (!rec) {
    d_status = unexpected(rec.error());
    return ObjectId{};
  }
  ObjectRecord& r = **rec;
  r.kind = RecordKind::Composition;
  r.id = id;
  r.as.composition = CompositionRecord{};
  r.as.composition.canvas_w = canvas_w;
  r.as.composition.canvas_h = canvas_h;

  expected<Ref<HamtNode>, PoolError> next =
      hamt_insert(d_model->d_bundle, d_root, id.value, rec->slot());
  if (!next) {
    d_status = unexpected(next.error());
    return ObjectId{};
  }
  d_root = std::move(*next);
  return id;
}

void Model::Transaction::set_transform(ObjectId layer, const Affine& transform) {
  if (!d_status) {
    return;
  }
  SlotRef<ObjectRecord> old_edge;
  if (!hamt_lookup(d_model->d_bundle, d_root, layer.value, old_edge)) {
    return; // absent: no-op (matches the walking-skeleton contract)
  }
  const ObjectRecord* old = d_model->d_records.peek(old_edge);
  if (old->kind != RecordKind::Layer) {
    return;
  }
  expected<Ref<ObjectRecord>, PoolError> rec = d_model->d_records.create();
  if (!rec) {
    d_status = unexpected(rec.error());
    return;
  }
  ObjectRecord& nr = **rec;
  nr = *old; // trivial copy of the immutable old record, then override placement
  nr.as.layer.transform = transform;

  expected<Ref<HamtNode>, PoolError> next =
      hamt_insert(d_model->d_bundle, d_root, layer.value, rec->slot());
  if (!next) {
    d_status = unexpected(next.error());
    return;
  }
  d_root = std::move(*next);
}

expected<std::monostate, PoolError> Model::Transaction::commit() {
  if (!d_status) {
    return d_status;
  }
  DocStatePtr next =
      std::make_shared<const DocRoot>(d_model->d_bundle, std::move(d_root), d_base_revision + 1);
  d_model->d_current.store(std::move(next));
  return std::monostate{};
}

} // namespace arbc
