#include <arbc/model/model.hpp>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
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

// Acquire a fresh owning `Ref` to an EXISTING node slot (structural sharing: the
// untouched subtree is retained, not copied). Maps the count-overflow surface
// onto the writer-path error channel like `retain_node`.
expected<Ref<HamtNode>, PoolError> share_node(StoreBundle& sb, SlotRef<HamtNode> slot) {
  expected<Ref<HamtNode>, RefError> r = sb.nodes->resolve(slot);
  if (!r) {
    return unexpected(PoolError::CapacityExhausted);
  }
  return std::move(*r);
}

// Build a new branch from `base` with child[`skip`] dropped; every OTHER present
// child is shared (retained). Untouched siblings keep their `SlotRef` identity.
expected<Ref<HamtNode>, PoolError> branch_without_child(StoreBundle& sb, const HamtNode* base,
                                                        std::uint32_t skip) {
  expected<Ref<HamtNode>, PoolError> node = sb.nodes->create();
  if (!node) {
    return node;
  }
  HamtNode& n = **node;
  n.is_leaf = 0;
  n.bitmap = 0;
  std::uint32_t remaining = base->bitmap;
  while (remaining != 0) {
    const std::uint32_t i = static_cast<std::uint32_t>(std::countr_zero(remaining));
    remaining &= remaining - 1;
    if (i == skip) {
      continue;
    }
    if (expected<std::monostate, PoolError> retained = retain_node(sb, base->children[i]);
        !retained) {
      return unexpected(retained.error());
    }
    n.children[i] = base->children[i];
    n.bitmap |= (std::uint32_t{1} << i);
  }
  return node;
}

// Path-copy erase of `key` under `node_slot`. Returns the new (path-copied)
// subtree, or an EMPTY `Ref` meaning "this subtree is now empty" (the parent
// drops the child edge). A key absent below `node_slot` yields a shared
// (retained) copy of the untouched subtree -- a structural no-op. Collapses a
// branch left with a single leaf child back into that leaf.
expected<Ref<HamtNode>, PoolError> erase_rec(StoreBundle& sb, SlotRef<HamtNode> node_slot,
                                             std::uint64_t key, std::uint32_t shift) {
  const HamtNode* node = sb.nodes->peek(node_slot);
  if (node->is_leaf != 0) {
    if (node->key == key) {
      return Ref<HamtNode>{}; // removed -> empty subtree
    }
    return share_node(sb, node_slot); // absent: share unchanged
  }
  const std::uint32_t digit = static_cast<std::uint32_t>((key >> shift) & k_hamt_mask);
  const std::uint32_t bit = std::uint32_t{1} << digit;
  if ((node->bitmap & bit) == 0) {
    return share_node(sb, node_slot); // absent under this branch
  }
  expected<Ref<HamtNode>, PoolError> child =
      erase_rec(sb, node->children[digit], key, shift + k_hamt_bits);
  if (!child) {
    return child;
  }
  if (*child) {
    return branch_with_child(sb, node, digit, std::move(*child)); // path-copy replace
  }
  // The child emptied: drop its edge, collapsing where possible.
  const std::uint32_t new_bitmap = node->bitmap & ~bit;
  if (new_bitmap == 0) {
    return Ref<HamtNode>{}; // whole branch emptied
  }
  if (std::popcount(new_bitmap) == 1) {
    const std::uint32_t only = static_cast<std::uint32_t>(std::countr_zero(new_bitmap));
    const SlotRef<HamtNode> only_slot = node->children[only];
    if (sb.nodes->peek(only_slot)->is_leaf != 0) {
      return share_node(sb, only_slot); // collapse single-leaf branch into the leaf
    }
  }
  return branch_without_child(sb, node, digit);
}

// Walk a composition's ordered layer membership into `order` (bottom-to-top): the
// inline `layers[]` array while inline, else the HAMT-backed spill-chunk chain
// headed by `comp.spill_root`. When `chunk_ids` is non-null, collect the chain's
// chunk `ObjectId`s in order. Pure peek traversal against `root` (refcount-free).
void read_layer_order(const StoreBundle& sb, const Ref<HamtNode>& root,
                      const CompositionRecord& comp, std::vector<ObjectId>& order,
                      std::vector<ObjectId>* chunk_ids) {
  if (!comp.spill_root.valid()) {
    for (std::uint32_t i = 0; i < comp.layer_count; ++i) {
      order.push_back(comp.layers[i]);
    }
    return;
  }
  ObjectId cur = comp.spill_root;
  while (cur.valid()) {
    SlotRef<ObjectRecord> edge;
    if (!hamt_lookup(sb, root, cur.value, edge)) {
      break; // defensive: a well-formed chain always resolves
    }
    const ObjectRecord* r = sb.records->peek(edge);
    if (r->kind != RecordKind::LayerOrderChunk) {
      break;
    }
    const LayerOrderChunk& ch = r->as.order_chunk;
    for (std::uint32_t i = 0; i < ch.count; ++i) {
      order.push_back(ch.members[i]);
    }
    if (chunk_ids != nullptr) {
      chunk_ids->push_back(cur);
    }
    cur = ch.next;
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

expected<Ref<HamtNode>, PoolError> hamt_erase(StoreBundle& sb, const Ref<HamtNode>& root,
                                              std::uint64_t key) {
  if (!root) {
    return Ref<HamtNode>{}; // empty map: erase is the empty map
  }
  return erase_rec(sb, root.slot(), key, 0);
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

// ---- Coalescing (pure JournalEntry value operation) -------------------------

void coalesce_entries(JournalEntry& base, const JournalEntry& follow) {
  // First entry's *before* + last entry's *after*, unioned object/content sets.
  for (const ObjectEdit& fe : follow.objects) {
    bool merged = false;
    for (ObjectEdit& be : base.objects) {
      if (be.object == fe.object) {
        be.after = fe.after; // keep the first before, take the last after
        merged = true;
        break;
      }
    }
    if (!merged) {
      base.objects.push_back(fe);
    }
  }
  for (const ContentStateEdit& fc : follow.contents) {
    bool merged = false;
    for (ContentStateEdit& bc : base.contents) {
      if (bc.object == fc.object) {
        bc.after = fc.after;
        merged = true;
        break;
      }
    }
    if (!merged) {
      base.contents.push_back(fc);
    }
  }
  for (const Damage& d : follow.damage) {
    damage_add(base.damage, d);
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

StateHandle DocRoot::content_state(ObjectId id) const {
  // Pure peek traversal: resolve the content record and read its captured handle;
  // absent / non-content resolves to the inert sentinel. No refcount page touched.
  const ContentRecord* c = find_content(id);
  return c != nullptr ? c->state : StateHandle{};
}

const CompositionRecord* DocRoot::find_composition(ObjectId id) const {
  SlotRef<ObjectRecord> edge;
  if (!hamt_lookup(d_stores, d_root, id.value, edge)) {
    return nullptr;
  }
  const ObjectRecord* r = d_stores.records->peek(edge);
  return r->kind == RecordKind::Composition ? &r->as.composition : nullptr;
}

SurfaceFormat DocRoot::working_space() const {
  // Doc 07 rule 2: the compositor blends in the composition's working space. The
  // skeleton models exactly one composition (refinement Decision "singular"); we
  // return its configured `SurfaceFormat`, or the doc 07 default when the
  // document has none yet so a fresh document still renders out of the box. The
  // lowest-id composition wins so the result is deterministic if several exist.
  if (!d_root) {
    return k_working_rgba32f;
  }
  std::vector<std::pair<std::uint64_t, SlotRef<ObjectRecord>>> leaves;
  collect_leaves(d_stores, d_root.slot(), leaves);
  const CompositionRecord* chosen = nullptr;
  std::uint64_t chosen_id = 0;
  for (const auto& [key, edge] : leaves) {
    const ObjectRecord* r = d_stores.records->peek(edge);
    if (r->kind == RecordKind::Composition && (chosen == nullptr || key < chosen_id)) {
      chosen = &r->as.composition;
      chosen_id = key;
    }
  }
  return chosen != nullptr ? chosen->working_space : k_working_rgba32f;
}

AudioFormat DocRoot::working_audio_format() const {
  // Doc 12: the mix engine pulls the composition at its working audio format --
  // the audio twin of `working_space()`. Return the single composition's
  // configured `AudioFormat`, or the doc 12 default when the document has none
  // yet so a fresh document mixes out of the box. The lowest-id composition wins
  // so the result is deterministic if several exist.
  if (!d_root) {
    return k_working_audio;
  }
  std::vector<std::pair<std::uint64_t, SlotRef<ObjectRecord>>> leaves;
  collect_leaves(d_stores, d_root.slot(), leaves);
  const CompositionRecord* chosen = nullptr;
  std::uint64_t chosen_id = 0;
  for (const auto& [key, edge] : leaves) {
    const ObjectRecord* r = d_stores.records->peek(edge);
    if (r->kind == RecordKind::Composition && (chosen == nullptr || key < chosen_id)) {
      chosen = &r->as.composition;
      chosen_id = key;
    }
  }
  return chosen != nullptr ? chosen->working_audio_format : k_working_audio;
}

bool DocRoot::find_first_composition(ObjectId& out_id, const CompositionRecord*& out_rec) const {
  // Mirror the single-composition resolution of working_space()/working_audio_format()
  // (lowest-id wins): a refcount-free peek over the pinned root's leaves.
  if (!d_root) {
    return false;
  }
  std::vector<std::pair<std::uint64_t, SlotRef<ObjectRecord>>> leaves;
  collect_leaves(d_stores, d_root.slot(), leaves);
  const CompositionRecord* chosen = nullptr;
  std::uint64_t chosen_id = 0;
  for (const auto& [key, edge] : leaves) {
    const ObjectRecord* r = d_stores.records->peek(edge);
    if (r->kind == RecordKind::Composition && (chosen == nullptr || key < chosen_id)) {
      chosen = &r->as.composition;
      chosen_id = key;
    }
  }
  if (chosen == nullptr) {
    return false;
  }
  out_id = ObjectId{chosen_id};
  out_rec = chosen;
  return true;
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

void DocRoot::for_each_layer_in(ObjectId composition,
                                const std::function<void(ObjectId)>& fn) const {
  const CompositionRecord* c = find_composition(composition);
  if (c == nullptr) {
    return;
  }
  std::vector<ObjectId> order;
  read_layer_order(d_stores, d_root, *c, order, nullptr);
  for (const ObjectId id : order) {
    fn(id);
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
  // Override the records' plain deferred zero sink with one that first releases a
  // reclaimed content record's embedded StateHandle through the L3 seam, then
  // defers the slot the same way (refinement Decision 2). register_store already
  // recorded the drain thunk; this only swaps the zero sink.
  d_records.set_zero_sink(&d_content_reclaim_sink);
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

Model::Transaction Model::transact(std::string name) { return Transaction(*this, std::move(name)); }

expected<std::monostate, PoolError>
Model::load_baseline(const std::function<void(Transaction&)>& build) {
  // Reuse the transaction machinery to reconstruct the graph (add_composition /
  // add_layer / attach_layer / set_*), then publish its root as the version-0
  // baseline WITHOUT journaling it (serialize.reader Decision 3): a load is a
  // fresh document at version 0, not an undoable edit (doc 14:263-264, 40-43).
  Transaction txn(*this, "load");
  build(txn);
  if (!txn.d_status) {
    return txn.d_status; // a reconstruction step exhausted the arena: publish nothing
  }
  txn.d_open = false; // consume the transaction: its teardown drops nothing extra

  // Publish: revision stays 0 and the CommitSink is deliberately untouched (no
  // journal entry), so the loaded graph is the baseline -- undo has nothing to
  // move to (doc 14:40-43). Exactly one atomic store, mirroring commit()/navigate().
  d_current.store(std::make_shared<const DocRoot>(d_bundle, std::move(txn.d_root), 0));

  // Flush the reconstruction's damage once (whole-document), mirroring commit, so
  // any subscribed output invalidates against the freshly-loaded content; a fresh
  // load with no sink installed simply discards it.
  if (d_damage_sink != nullptr) {
    d_damage_sink->flush(txn.d_damage);
  }
  return std::monostate{};
}

// ---- Transaction ------------------------------------------------------------

Model::Transaction::Transaction(Model& model, std::string name)
    : d_model(&model), d_name(std::move(name)), d_status(std::monostate{}) {
  d_base = model.current(); // pin the base version: fork its root + the before-edges
  d_root = d_base->root_ref();
  d_base_revision = d_base->revision();
}

void Model::Transaction::touch(ObjectId id) {
  for (const ObjectId seen : d_touched) {
    if (seen == id) {
      return;
    }
  }
  d_touched.push_back(id);
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
  // gain / flags (audible+visible) / span / time_map take their still-defaults.
  r.as.layer = LayerRecord{content, transform, opacity};

  expected<Ref<HamtNode>, PoolError> next =
      hamt_insert(d_model->d_bundle, d_root, id.value, rec->slot());
  if (!next) {
    d_status = unexpected(next.error());
    return ObjectId{};
  }
  d_root = std::move(*next);
  touch(id);
  add_damage(Damage{id, Rect::infinite(), TimeRange::all()});
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
  touch(id);
  add_damage(Damage{id, Rect::infinite(), TimeRange::all()});
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
  touch(id);
  add_damage(Damage{id, Rect::infinite(), TimeRange::all()});
  return id;
}

void Model::Transaction::set_working_space(ObjectId composition, const SurfaceFormat& format) {
  if (!d_status) {
    return;
  }
  SlotRef<ObjectRecord> old_edge;
  if (!hamt_lookup(d_model->d_bundle, d_root, composition.value, old_edge)) {
    return; // absent: no-op (matches the walking-skeleton contract)
  }
  const ObjectRecord* old = d_model->d_records.peek(old_edge);
  if (old->kind != RecordKind::Composition) {
    return; // not a composition: no-op
  }
  expected<Ref<ObjectRecord>, PoolError> rec = d_model->d_records.create();
  if (!rec) {
    d_status = unexpected(rec.error());
    return;
  }
  ObjectRecord& nr = **rec;
  nr = *old; // trivial copy of the immutable old record, then override the config
  nr.as.composition.working_space = format;

  expected<Ref<HamtNode>, PoolError> next =
      hamt_insert(d_model->d_bundle, d_root, composition.value, rec->slot());
  if (!next) {
    d_status = unexpected(next.error());
    return;
  }
  d_root = std::move(*next);
  touch(composition);
  add_damage(Damage{composition, Rect::infinite(), TimeRange::all()});
}

void Model::Transaction::set_working_audio_format(ObjectId composition, const AudioFormat& format) {
  if (!d_status) {
    return;
  }
  SlotRef<ObjectRecord> old_edge;
  if (!hamt_lookup(d_model->d_bundle, d_root, composition.value, old_edge)) {
    return; // absent: no-op (matches the walking-skeleton contract)
  }
  const ObjectRecord* old = d_model->d_records.peek(old_edge);
  if (old->kind != RecordKind::Composition) {
    return; // not a composition: no-op
  }
  expected<Ref<ObjectRecord>, PoolError> rec = d_model->d_records.create();
  if (!rec) {
    d_status = unexpected(rec.error());
    return;
  }
  ObjectRecord& nr = **rec;
  nr = *old; // trivial copy of the immutable old record, then override the config
  nr.as.composition.working_audio_format = format;

  expected<Ref<HamtNode>, PoolError> next =
      hamt_insert(d_model->d_bundle, d_root, composition.value, rec->slot());
  if (!next) {
    d_status = unexpected(next.error());
    return;
  }
  d_root = std::move(*next);
  touch(composition);
  add_damage(Damage{composition, Rect::infinite(), TimeRange::all()});
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
  touch(layer);
  add_damage(Damage{layer, Rect::infinite(), TimeRange::all()});
}

void Model::Transaction::set_span(ObjectId layer, const TimeRange& span) {
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
  nr.as.layer.span = span;

  expected<Ref<HamtNode>, PoolError> next =
      hamt_insert(d_model->d_bundle, d_root, layer.value, rec->slot());
  if (!next) {
    d_status = unexpected(next.error());
    return;
  }
  d_root = std::move(*next);
  touch(layer);
  add_damage(Damage{layer, Rect::infinite(), TimeRange::all()});
}

void Model::Transaction::set_time_map(ObjectId layer, const TimeMap& time_map) {
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
  nr.as.layer.time_map = time_map;

  expected<Ref<HamtNode>, PoolError> next =
      hamt_insert(d_model->d_bundle, d_root, layer.value, rec->slot());
  if (!next) {
    d_status = unexpected(next.error());
    return;
  }
  d_root = std::move(*next);
  touch(layer);
  add_damage(Damage{layer, Rect::infinite(), TimeRange::all()});
}

void Model::Transaction::set_opacity(ObjectId layer, double opacity) {
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
  nr.as.layer.opacity = opacity;

  expected<Ref<HamtNode>, PoolError> next =
      hamt_insert(d_model->d_bundle, d_root, layer.value, rec->slot());
  if (!next) {
    d_status = unexpected(next.error());
    return;
  }
  d_root = std::move(*next);
  touch(layer);
  add_damage(Damage{layer, Rect::infinite(), TimeRange::all()});
}

void Model::Transaction::set_gain(ObjectId layer, double gain) {
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
  nr.as.layer.gain = gain;

  expected<Ref<HamtNode>, PoolError> next =
      hamt_insert(d_model->d_bundle, d_root, layer.value, rec->slot());
  if (!next) {
    d_status = unexpected(next.error());
    return;
  }
  d_root = std::move(*next);
  touch(layer);
  add_damage(Damage{layer, Rect::infinite(), TimeRange::all()});
}

void Model::Transaction::set_audible(ObjectId layer, bool audible) {
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
  if (audible) {
    nr.as.layer.flags |= k_layer_audible;
  } else {
    nr.as.layer.flags &= ~k_layer_audible;
  }

  expected<Ref<HamtNode>, PoolError> next =
      hamt_insert(d_model->d_bundle, d_root, layer.value, rec->slot());
  if (!next) {
    d_status = unexpected(next.error());
    return;
  }
  d_root = std::move(*next);
  touch(layer);
  add_damage(Damage{layer, Rect::infinite(), TimeRange::all()});
}

void Model::Transaction::set_visible(ObjectId layer, bool visible) {
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
  if (visible) {
    nr.as.layer.flags |= k_layer_visible;
  } else {
    nr.as.layer.flags &= ~k_layer_visible;
  }

  expected<Ref<HamtNode>, PoolError> next =
      hamt_insert(d_model->d_bundle, d_root, layer.value, rec->slot());
  if (!next) {
    d_status = unexpected(next.error());
    return;
  }
  d_root = std::move(*next);
  touch(layer);
  add_damage(Damage{layer, Rect::infinite(), TimeRange::all()});
}

bool Model::Transaction::store_membership(ObjectId composition, const ObjectRecord& base,
                                          const std::vector<ObjectId>& old_order,
                                          const std::vector<ObjectId>& old_chunk_ids,
                                          const std::vector<ObjectId>& new_order) {
  const std::size_t cap = k_max_inline_layers;
  const std::size_t n = new_order.size();

  // --- Fits inline: rewrite the composition record, drop any spill chunks. ---
  if (n <= cap) {
    for (const ObjectId cid : old_chunk_ids) {
      expected<Ref<HamtNode>, PoolError> er = hamt_erase(d_model->d_bundle, d_root, cid.value);
      if (!er) {
        d_status = unexpected(er.error());
        return false;
      }
      d_root = std::move(*er);
      touch(cid);
    }
    expected<Ref<ObjectRecord>, PoolError> rec = d_model->d_records.create();
    if (!rec) {
      d_status = unexpected(rec.error());
      return false;
    }
    ObjectRecord& nr = **rec;
    nr = base;
    nr.as.composition.layer_count = static_cast<std::uint32_t>(n);
    nr.as.composition.spill_root = ObjectId{};
    for (std::size_t i = 0; i < cap; ++i) {
      nr.as.composition.layers[i] = (i < n) ? new_order[i] : ObjectId{};
    }
    expected<Ref<HamtNode>, PoolError> next =
        hamt_insert(d_model->d_bundle, d_root, composition.value, rec->slot());
    if (!next) {
      d_status = unexpected(next.error());
      return false;
    }
    d_root = std::move(*next);
    touch(composition);
    return true;
  }

  // --- Spilled: a chunk chain of ceil(n / cap) chunks. ---
  const std::size_t c_new = (n + cap - 1) / cap;
  const std::size_t c_old = old_chunk_ids.size();

  // Assign chunk ids positionally: reuse existing chunk ids (so unchanged prefix
  // chunks keep their `SlotRef` identity and their `next` edges stay valid),
  // allocate fresh ids only for the growth tail.
  std::vector<ObjectId> chunk_ids(c_new);
  for (std::size_t p = 0; p < c_new; ++p) {
    chunk_ids[p] = (p < c_old) ? old_chunk_ids[p] : d_model->allocate_id();
  }
  // Erase surplus tail chunks (a shrink that stays spilled).
  for (std::size_t p = c_new; p < c_old; ++p) {
    expected<Ref<HamtNode>, PoolError> er =
        hamt_erase(d_model->d_bundle, d_root, old_chunk_ids[p].value);
    if (!er) {
      d_status = unexpected(er.error());
      return false;
    }
    d_root = std::move(*er);
    touch(old_chunk_ids[p]);
  }

  // Rewrite only the chunks whose members or `next` edge actually change; every
  // other chunk is shared from the base version by `SlotRef` identity.
  for (std::size_t p = 0; p < c_new; ++p) {
    const std::size_t begin = p * cap;
    const std::size_t cnt = std::min(cap, n - begin);
    const ObjectId next_id = (p + 1 < c_new) ? chunk_ids[p + 1] : ObjectId{};

    bool unchanged = p < c_old;
    if (unchanged) {
      const std::size_t old_begin = p * cap;
      const std::size_t old_cnt = std::min(cap, old_order.size() - old_begin);
      const ObjectId old_next = (p + 1 < c_old) ? old_chunk_ids[p + 1] : ObjectId{};
      unchanged = (cnt == old_cnt) && (old_next == next_id);
      for (std::size_t i = 0; unchanged && i < cnt; ++i) {
        unchanged = new_order[begin + i] == old_order[old_begin + i];
      }
    }
    if (unchanged) {
      continue; // share the base chunk unchanged
    }

    expected<Ref<ObjectRecord>, PoolError> rec = d_model->d_records.create();
    if (!rec) {
      d_status = unexpected(rec.error());
      return false;
    }
    ObjectRecord& cr = **rec;
    cr.kind = RecordKind::LayerOrderChunk;
    cr.id = chunk_ids[p];
    cr.as.order_chunk = LayerOrderChunk{};
    cr.as.order_chunk.count = static_cast<std::uint32_t>(cnt);
    for (std::size_t i = 0; i < cnt; ++i) {
      cr.as.order_chunk.members[i] = new_order[begin + i];
    }
    cr.as.order_chunk.next = next_id;
    expected<Ref<HamtNode>, PoolError> ins =
        hamt_insert(d_model->d_bundle, d_root, chunk_ids[p].value, rec->slot());
    if (!ins) {
      d_status = unexpected(ins.error());
      return false;
    }
    d_root = std::move(*ins);
    touch(chunk_ids[p]);
  }

  // Path-copy the composition record: head the chain, hold the authoritative
  // count, and clear the now-dead inline array for a canonical record.
  expected<Ref<ObjectRecord>, PoolError> rec = d_model->d_records.create();
  if (!rec) {
    d_status = unexpected(rec.error());
    return false;
  }
  ObjectRecord& nr = **rec;
  nr = base;
  nr.as.composition.layer_count = static_cast<std::uint32_t>(n);
  nr.as.composition.spill_root = chunk_ids[0];
  for (std::size_t i = 0; i < cap; ++i) {
    nr.as.composition.layers[i] = ObjectId{};
  }
  expected<Ref<HamtNode>, PoolError> next =
      hamt_insert(d_model->d_bundle, d_root, composition.value, rec->slot());
  if (!next) {
    d_status = unexpected(next.error());
    return false;
  }
  d_root = std::move(*next);
  touch(composition);
  return true;
}

void Model::Transaction::attach_layer(ObjectId composition, ObjectId layer,
                                      std::uint32_t at_index) {
  if (!d_status) {
    return;
  }
  SlotRef<ObjectRecord> comp_edge;
  if (!hamt_lookup(d_model->d_bundle, d_root, composition.value, comp_edge)) {
    return; // absent composition: no-op
  }
  const ObjectRecord* comp = d_model->d_records.peek(comp_edge);
  if (comp->kind != RecordKind::Composition) {
    return; // not a composition: no-op
  }
  SlotRef<ObjectRecord> layer_edge;
  if (!hamt_lookup(d_model->d_bundle, d_root, layer.value, layer_edge)) {
    return; // absent layer: no-op
  }

  const ObjectRecord base = *comp;
  std::vector<ObjectId> old_order;
  std::vector<ObjectId> old_chunk_ids;
  read_layer_order(d_model->d_bundle, d_root, base.as.composition, old_order, &old_chunk_ids);

  std::vector<ObjectId> new_order = old_order;
  const std::size_t at = std::min(static_cast<std::size_t>(at_index), new_order.size());
  new_order.insert(new_order.begin() + static_cast<std::ptrdiff_t>(at), layer);

  if (!store_membership(composition, base, old_order, old_chunk_ids, new_order)) {
    return;
  }
  add_damage(Damage{composition, Rect::infinite(), TimeRange::all()});
}

void Model::Transaction::attach_layer(ObjectId composition, ObjectId layer) {
  attach_layer(composition, layer, std::numeric_limits<std::uint32_t>::max());
}

void Model::Transaction::detach_layer(ObjectId composition, ObjectId layer) {
  if (!d_status) {
    return;
  }
  SlotRef<ObjectRecord> comp_edge;
  if (!hamt_lookup(d_model->d_bundle, d_root, composition.value, comp_edge)) {
    return; // absent composition: no-op
  }
  const ObjectRecord* comp = d_model->d_records.peek(comp_edge);
  if (comp->kind != RecordKind::Composition) {
    return;
  }

  const ObjectRecord base = *comp;
  std::vector<ObjectId> old_order;
  std::vector<ObjectId> old_chunk_ids;
  read_layer_order(d_model->d_bundle, d_root, base.as.composition, old_order, &old_chunk_ids);

  std::size_t idx = old_order.size();
  for (std::size_t i = 0; i < old_order.size(); ++i) {
    if (old_order[i] == layer) {
      idx = i;
      break;
    }
  }
  if (idx == old_order.size()) {
    return; // not a member: no-op
  }
  std::vector<ObjectId> new_order = old_order;
  new_order.erase(new_order.begin() + static_cast<std::ptrdiff_t>(idx));

  if (!store_membership(composition, base, old_order, old_chunk_ids, new_order)) {
    return;
  }
  add_damage(Damage{composition, Rect::infinite(), TimeRange::all()});
}

void Model::Transaction::reorder_layer(ObjectId composition, std::uint32_t from_index,
                                       std::uint32_t to_index) {
  if (!d_status) {
    return;
  }
  SlotRef<ObjectRecord> comp_edge;
  if (!hamt_lookup(d_model->d_bundle, d_root, composition.value, comp_edge)) {
    return; // absent composition: no-op
  }
  const ObjectRecord* comp = d_model->d_records.peek(comp_edge);
  if (comp->kind != RecordKind::Composition) {
    return;
  }

  const ObjectRecord base = *comp;
  std::vector<ObjectId> old_order;
  std::vector<ObjectId> old_chunk_ids;
  read_layer_order(d_model->d_bundle, d_root, base.as.composition, old_order, &old_chunk_ids);

  if (from_index >= old_order.size() || to_index >= old_order.size() || from_index == to_index) {
    return; // out of range or a no-op move
  }
  std::vector<ObjectId> new_order = old_order;
  const ObjectId moved = new_order[from_index];
  new_order.erase(new_order.begin() + static_cast<std::ptrdiff_t>(from_index));
  new_order.insert(new_order.begin() + static_cast<std::ptrdiff_t>(to_index), moved);

  if (!store_membership(composition, base, old_order, old_chunk_ids, new_order)) {
    return;
  }
  add_damage(Damage{composition, Rect::infinite(), TimeRange::all()});
}

void Model::Transaction::set_content_state(ObjectId content, StateHandle after) {
  if (!d_status) {
    return;
  }
  SlotRef<ObjectRecord> old_edge;
  if (!hamt_lookup(d_model->d_bundle, d_root, content.value, old_edge)) {
    return; // absent: no-op
  }
  const ObjectRecord* old = d_model->d_records.peek(old_edge);
  if (old->kind != RecordKind::Content) {
    return; // not a content object: no-op
  }
  const StateHandle before = old->as.content.state;

  expected<Ref<ObjectRecord>, PoolError> rec = d_model->d_records.create();
  if (!rec) {
    d_status = unexpected(rec.error());
    return;
  }
  ObjectRecord& nr = **rec;
  nr = *old; // trivial copy of the immutable old record, then override the handle
  nr.as.content.state = after;

  // Retain the captured handle for THIS newly-created record slot (one retain per
  // distinct content ObjectRecord instance, doc 14:133-136). The matching release
  // fires when the slot is reclaimed (ContentStateReclaimSink). If the insert
  // below fails, `rec` drops here and its zero-count reclaim releases it -- so the
  // pairing holds even on the error path. The `before` handle keeps the retain it
  // took when the base record was created; this call touches only `after`.
  if (after.has_state() && d_model->d_state_ref_sink != nullptr) {
    d_model->d_state_ref_sink->retain(content, after);
  }

  expected<Ref<HamtNode>, PoolError> next =
      hamt_insert(d_model->d_bundle, d_root, content.value, rec->slot());
  if (!next) {
    d_status = unexpected(next.error());
    return;
  }
  d_root = std::move(*next);
  touch(content);
  // No model-emitted damage floor here (doc 14 Decisions): only the kind's L3
  // `Editable` method knows the touched region and temporal extent, and it
  // supplies precise damage via `add_damage` under the "capture, mutate, damage"
  // discipline (doc 14:151-171). A coarse whole-content floor would be absorbing
  // and erase that precision, defeating the O(touched tiles) promise; an
  // empty-rect floor would be unsound. Callers that swap state without an
  // `Editable` supply their own `Damage{content, Rect::infinite(),
  // TimeRange::all()}` at their call site.

  // Record the (before, after) handle pair; first-before / last-after within a
  // single transaction (a second set on the same object keeps the first before).
  for (ContentStateEdit& e : d_contents) {
    if (e.object == content) {
      e.after = after;
      return;
    }
  }
  d_contents.push_back(ContentStateEdit{content, before, after});
}

void Model::Transaction::remove(ObjectId id) {
  if (!d_status) {
    return;
  }
  SlotRef<ObjectRecord> edge;
  if (!hamt_lookup(d_model->d_bundle, d_root, id.value, edge)) {
    return; // absent: no-op
  }
  expected<Ref<HamtNode>, PoolError> next = hamt_erase(d_model->d_bundle, d_root, id.value);
  if (!next) {
    d_status = unexpected(next.error());
    return;
  }
  d_root = std::move(*next);
  touch(id);
  add_damage(Damage{id, Rect::infinite(), TimeRange::all()});
}

Model::Transaction& Model::Transaction::coalesce(CoalesceKey key) {
  d_coalesce = key;
  return *this;
}

void Model::Transaction::add_damage(const Damage& d) { damage_add(d_damage, d); }

void Model::Transaction::abort() {
  if (!d_open) {
    return;
  }
  d_open = false;
  // Discard the working tree: dropping the root cascades its unique nodes through
  // the reclamation queue. Nothing was published, so nothing is observable.
  d_root = Ref<HamtNode>{};
  d_touched.clear();
  d_contents.clear();
  d_damage.clear();
}

expected<std::monostate, PoolError> Model::Transaction::commit() {
  if (!d_open) {
    return d_status; // already committed or aborted: publish nothing
  }
  if (!d_status) {
    return d_status; // a mutation failed: abort the publish, current stays put
  }

  // Assemble the journal entry BEFORE the publish so a resolve failure aborts
  // atomically (nothing observed). Only when a commit sink is installed.
  JournalEntry entry;
  const bool build_entry = d_model->d_commit_sink != nullptr;
  if (build_entry) {
    entry.name = d_name;
    entry.coalesce_key = d_coalesce;
    entry.contents = d_contents;
    entry.damage = d_damage;
    for (const ObjectId id : d_touched) {
      ObjectEdit e;
      e.object = id;
      SlotRef<ObjectRecord> before_edge;
      if (hamt_lookup(d_model->d_bundle, d_base->root_ref(), id.value, before_edge)) {
        expected<Ref<ObjectRecord>, RefError> r = d_model->d_records.resolve(before_edge);
        if (!r) {
          d_status = unexpected(PoolError::CapacityExhausted);
          return d_status;
        }
        e.before = std::move(*r);
      }
      SlotRef<ObjectRecord> after_edge;
      if (hamt_lookup(d_model->d_bundle, d_root, id.value, after_edge)) {
        expected<Ref<ObjectRecord>, RefError> r = d_model->d_records.resolve(after_edge);
        if (!r) {
          d_status = unexpected(PoolError::CapacityExhausted);
          return d_status;
        }
        e.after = std::move(*r);
      }
      entry.objects.push_back(std::move(e));
    }
  }

  // Publish: exactly one atomic store, exactly one revision increment.
  d_model->d_current.store(
      std::make_shared<const DocRoot>(d_model->d_bundle, std::move(d_root), d_base_revision + 1));
  d_open = false;

  // Flush damage once, then notify the commit sink once (doc 14:92-94, :164).
  if (d_model->d_damage_sink != nullptr) {
    d_model->d_damage_sink->flush(d_damage);
  }
  if (build_entry) {
    d_model->d_commit_sink->on_commit(std::move(entry));
  }
  return std::monostate{};
}

// ---- Navigation publish (undo/redo) -----------------------------------------

expected<std::monostate, PoolError> Model::navigate(const JournalEntry& entry, NavDirection dir) {
  // Fork the current version's root and rebind every touched object to its
  // target-direction edge. Path-copies through the same primitives a commit uses,
  // so navigation is an ordinary forward publish (doc 14:170-172). Assemble the
  // whole new root BEFORE the atomic store so an allocation failure aborts with
  // nothing observed (the partial tree drops here and cascades on the next drain).
  const DocStatePtr base = d_current.load();
  Ref<HamtNode> root = base->root_ref(); // owning: forked, retained
  for (const ObjectEdit& oe : entry.objects) {
    const Ref<ObjectRecord>& target = (dir == NavDirection::Undo) ? oe.before : oe.after;
    if (target) {
      // Reuse the stored owning edge by identity: the new leaf takes its own count.
      expected<Ref<HamtNode>, PoolError> next =
          hamt_insert(d_bundle, root, oe.object.value, target.slot());
      if (!next) {
        return unexpected(next.error());
      }
      root = std::move(*next);
    } else {
      // Empty target: the object did not exist in that direction -- erase it.
      expected<Ref<HamtNode>, PoolError> next = hamt_erase(d_bundle, root, oe.object.value);
      if (!next) {
        return unexpected(next.error());
      }
      root = std::move(*next);
    }
  }

  // Publish: one atomic store, revision +1 -- exactly like a commit.
  d_current.store(std::make_shared<const DocRoot>(d_bundle, std::move(root), base->revision() + 1));

  // Replay the entry's damage once (doc 14: invalidation is exactly right without
  // diffing); the commit sink is deliberately untouched -- history is never
  // mutated (doc 14:43).
  if (d_damage_sink != nullptr) {
    d_damage_sink->flush(entry.damage);
  }
  return std::monostate{};
}

} // namespace arbc
