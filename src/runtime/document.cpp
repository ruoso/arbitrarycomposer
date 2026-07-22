#include <arbc/runtime/document.hpp>

#include <memory>
#include <utility>

namespace arbc {

HousekeepingConfig Document::policy_for(const DocumentHousekeepingConfig& config,
                                        bool workspace_backed) {
  HousekeepingConfig policy;
  policy.drain_between_transactions = config.drain_between_transactions;
  // The transaction-count trigger fires from `after_commit`, i.e. on the writer --
  // the only thread a `Checkpointer::commit` may ever run on. Inert without a file.
  if (workspace_backed && config.checkpoint_every_n_transactions > 0) {
    policy.checkpoint_every_n_transactions = config.checkpoint_every_n_transactions;
  }
  // `checkpoint_tick_interval` is deliberately LEFT EMPTY, always: it is the one
  // trigger that fires from the background thread's `tick()`, and a commit there
  // races the writer's allocator (doc 15 § Version reclamation). The background
  // thread drains; the writer checkpoints.
  return policy;
}

Document::Document(DocumentHousekeepingConfig housekeeping)
    : Document(std::make_unique<Model>(), std::move(housekeeping)) {}

Document::Document(std::unique_ptr<Model> model, DocumentHousekeepingConfig housekeeping)
    // `housekeeping.thread` is COPIED, not moved: two arguments of one call have
    // unspecified evaluation order, and a move would leave `policy_for` reading a
    // moved-from struct if it happened to run second.
    : d_model(std::move(model)),
      d_housekeeping(d_hk_target, policy_for(housekeeping, d_model->workspace_backed()),
                     housekeeping.thread) {
  // The document owns the single, document-wide history (doc 14:193-195): every
  // Document mutator's commit appends one entry and is undoable. The relay -- not the
  // journal itself -- is the model's CommitSink, so the same post-publish hook also
  // drives the housekeeper's between-transaction drain and checkpoint cadence for
  // EVERY commit, including one a host makes through a raw `transact()`.
  d_model->set_commit_sink(&d_commit_relay);
  d_binding.attach(*d_model, d_journal);
  // The binding's teardown drains must go through the single drainer, not around it
  // (Constraint 2): `unbind`/`unbind_all` would otherwise race the background loop.
  d_binding.set_drain_hook([this] { d_housekeeping.drain_and_quiesce(); });
}

// Out-of-line so `~HousekeepingThread` (which joins the loop) is instantiated here,
// with the whole member set complete -- the teardown contract in document.hpp.
Document::~Document() = default;

expected<std::unique_ptr<Document>, WorkspaceFileError>
Document::create(const std::string& path, DocumentHousekeepingConfig housekeeping) {
  expected<std::unique_ptr<Model>, WorkspaceFileError> model = Model::create(path);
  if (!model) {
    return unexpected(model.error());
  }
  return std::unique_ptr<Document>(new Document(std::move(*model), std::move(housekeeping)));
}

expected<std::unique_ptr<Document>, WorkspaceFileError>
Document::open(const std::string& path, DocumentHousekeepingConfig housekeeping) {
  expected<std::unique_ptr<Model>, WorkspaceFileError> model = Model::open(path);
  if (!model) {
    return unexpected(model.error());
  }
  return std::unique_ptr<Document>(new Document(std::move(*model), std::move(housekeeping)));
}

void Document::CommitRelay::on_commit(JournalEntry entry) {
  d_doc->d_journal.on_commit(std::move(entry));

  // The between-transaction drain + the transaction-count checkpoint cadence, on the
  // writer thread, synchronized against the background loop by the thread's mutex.
  expected<std::monostate, WorkspaceFileError> kept = d_doc->d_housekeeping.after_commit();
  if (!kept) {
    // A cadence commit's I/O failure has no caller to return a value to (the publish
    // itself succeeded and is not being unwound): record it, never throw off a commit.
    d_doc->d_last_checkpoint_error = kept.error();
    return;
  }
  d_doc->d_last_checkpoint_error.reset();
}

ObjectId Document::add_content(std::shared_ptr<Content> content, std::uint64_t kind) {
  // Mint a versioned ContentRecord (kind id + inert StateHandle) as a top-level
  // DocState entry, published as one new version -- exactly like every other
  // Document mutator. The id->Content vtable binding then rides the runtime
  // side-map keyed by that record's id (doc 17:66-72 keeps the model free of the
  // Content vtable). resolve(id) serves it; find_content(id) carries no pointer.
  Content& live = *content;
  auto txn = d_model->transact();
  const ObjectId id = txn.add_content(kind);

  // Register-on-instantiate, the state-sink analogue of the damage sink the core
  // connects on attach (doc 03:113-118). Through the `Editable` facet only, so the
  // runtime names no concrete kind (doc 17:66-72); non-editable content binds
  // nothing. Routed INSIDE the transaction: the content's row must be live before
  // the commit publishes the record, or the retain owed for the state it embeds
  // would find no owner. Any number of editable contents may be added -- the
  // document-wide sink trio dispatches by the owning id (`EditableBinding`).
  if (Editable* editable = d_binding.bind(id, live)) {
    // Close the `model.content_binding` gap: that task deliberately left the fresh
    // ContentRecord's StateHandle inert. An editable content already HAS state at
    // instantiation, so capture it onto the record here, in the same transaction --
    // one published version, exactly as before. Without this the record names no
    // state, so the first edit's journal entry has an INERT *before* handle and its
    // undo would restore the content to nothing (doc 14:133-152).
    const StateHandle initial = editable->capture();
    if (initial.has_state()) {
      txn.set_content_state(id, initial);
    }
  }

  txn.commit();

  // Publish the id->Content binding COPY-ON-WRITE (issue #10): copy the current
  // immutable map, append the new row, and atomically swap it in. The whole-map copy is
  // near-free -- the values are `shared_ptr` and `emplace` is rare -- and it is what lets
  // `resolve()`/`for_each_content()` be lock-free pinned reads on the render thread. This
  // is the SINGLE structural mutation of the table, and it runs on the writer thread
  // alone, so a plain load/copy/store needs no CAS loop. A reader mid-walk keeps its own
  // pinned generation; it simply does not see this new row until its next load.
  auto next = std::make_shared<ContentBindings>(*d_contents.load());
  next->emplace(id, std::move(content));
  d_contents.store(std::move(next));
  return id;
}

void Document::remove_content(ObjectId content, ObjectId composition, ObjectId layer) {
  // The inverse of `add_content`, composing three existing model teardowns into one
  // atomic, undoable, per-content deletion (doc 14 § Transactions, the removal
  // paragraph). One transaction => one publish, one journal entry, one damage flush:
  // detach the referencing layer from its composition's ordered membership, erase the
  // layer record, and erase the content record. Because the mutations share a
  // transaction an observer sees the document with the content present or fully gone,
  // never a layer naming an erased content.
  auto txn = d_model->transact();
  txn.detach_layer(composition, layer);
  txn.remove(layer);
  txn.remove(content);
  txn.commit();

  // Decision 1 (`model.content_removal`): do NOT `d_binding.unbind(content)` or
  // `d_contents.erase(content)` here. The removal is journaled, so the erased
  // ContentRecord is NOT reclaimed -- the journal's `before` edge holds it so undo
  // can restore it (doc 14 § History). That record's `StateHandle::release` is
  // deferred to its eventual reclaim and routed to the content's binding row; dropping
  // the row now would strand that future release with nowhere to route, tripping the
  // binding's asserted-zero `unrouted_state_calls()` invariant and leaking the
  // handle's pool blocks -- the exact hazard `EditableBinding::unbind`'s own
  // doc-comment warns against. So the live `Content*` and its row are RETAINED here;
  // they are torn down by the document-close declaration-order drain, and, once the
  // named `runtime.removed_content_reclaim` follow-up lands, at history-trim.
}

Model::Transaction Document::transact(std::string name) {
  return d_model->transact(std::move(name));
}

void Document::drain() { d_housekeeping.drain_and_quiesce(); }

expected<std::monostate, WorkspaceFileError> Document::checkpoint() {
  if (!d_model->workspace_backed()) {
    // The anonymous document answers rather than asserting: there is no file, so there
    // is nothing to make durable (Constraint 6). `Model::checkpoint()` says the same.
    return unexpected(WorkspaceFileError{WorkspaceFileErrc::Unsupported, 0});
  }
  return d_housekeeping.request_checkpoint();
}

HousekeepingStats Document::memory_stats() const { return d_housekeeping.stats(); }

std::uint64_t Document::background_ticks() const noexcept {
  return d_housekeeping.background_ticks();
}

std::uint64_t Document::flush_housekeeping() { return d_housekeeping.flush(); }

ObjectId Document::add_layer(ObjectId content, const Affine& transform, double opacity) {
  auto txn = d_model->transact();
  const ObjectId id = txn.add_layer(content, transform, opacity);
  txn.commit();
  return id;
}

void Document::set_layer_transform(ObjectId layer, const Affine& transform) {
  auto txn = d_model->transact();
  txn.set_transform(layer, transform);
  txn.commit();
}

void Document::set_layer_span(ObjectId layer, const TimeRange& span) {
  auto txn = d_model->transact();
  txn.set_span(layer, span);
  txn.commit();
}

void Document::set_layer_time_map(ObjectId layer, const TimeMap& time_map) {
  auto txn = d_model->transact();
  txn.set_time_map(layer, time_map);
  txn.commit();
}

void Document::attach_layer(ObjectId composition, ObjectId layer) {
  auto txn = d_model->transact();
  txn.attach_layer(composition, layer);
  txn.commit();
}

void Document::set_layer_gain(ObjectId layer, double gain) {
  auto txn = d_model->transact();
  txn.set_gain(layer, gain);
  txn.commit();
}

void Document::set_layer_audible(ObjectId layer, bool audible) {
  auto txn = d_model->transact();
  txn.set_audible(layer, audible);
  txn.commit();
}

void Document::set_working_audio_format(ObjectId composition, const AudioFormat& format) {
  auto txn = d_model->transact();
  txn.set_working_audio_format(composition, format);
  txn.commit();
}

ObjectId Document::add_composition(double canvas_w, double canvas_h) {
  auto txn = d_model->transact();
  const ObjectId id = txn.add_composition(canvas_w, canvas_h);
  txn.commit();
  return id;
}

void Document::set_working_space(ObjectId composition, const SurfaceFormat& format) {
  auto txn = d_model->transact();
  txn.set_working_space(composition, format);
  txn.commit();
}

DocStatePtr Document::pin() const { return d_model->current(); }

Content* Document::resolve(ObjectId content) const {
  // Lock-free pinned read (issue #10): load the current published map once and look up
  // in that immutable snapshot. A concurrent writer publishing a new map cannot tear this
  // lookup -- the pinned `shared_ptr` keeps the snapshot alive for the call's duration.
  const std::shared_ptr<const ContentBindings> snap = d_contents.load();
  const auto found = snap->find(content);
  return found == snap->end() ? nullptr : found->second.get();
}

void Document::for_each_content(const std::function<void(Content*)>& fn) const {
  // Pin one published generation for the whole walk (issue #10): a content the writer
  // appends mid-iteration lands in a NEW map the writer swaps in, leaving this snapshot
  // untouched, so the walk never observes a rehash of the container it is iterating.
  const std::shared_ptr<const ContentBindings> snap = d_contents.load();
  for (const auto& entry : *snap) {
    fn(entry.second.get());
  }
}

void Document::for_each_content(const std::function<void(ObjectId, Content*)>& fn) const {
  const std::shared_ptr<const ContentBindings> snap = d_contents.load();
  for (const auto& entry : *snap) {
    fn(entry.first, entry.second.get());
  }
}

} // namespace arbc
