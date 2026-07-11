#include <arbc/runtime/document.hpp>

#include <utility>

namespace arbc {

Document::Document() {
  // The document owns the single, document-wide history (doc 14:193-195): the
  // journal IS the model's CommitSink, so every Document mutator's commit appends
  // one entry and is undoable. The binding then has a live Model/Journal to
  // register an editable content's state sinks onto (add_content, below).
  d_model.set_commit_sink(&d_journal);
  d_binding.attach(d_model, d_journal);
}

ObjectId Document::add_content(std::shared_ptr<Content> content, std::uint64_t kind) {
  // Mint a versioned ContentRecord (kind id + inert StateHandle) as a top-level
  // DocState entry, published as one new version -- exactly like every other
  // Document mutator. The id->Content vtable binding then rides the runtime
  // side-map keyed by that record's id (doc 17:66-72 keeps the model free of the
  // Content vtable). resolve(id) serves it; find_content(id) carries no pointer.
  Content& live = *content;
  auto txn = d_model.transact();
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
  d_contents.emplace(id, std::move(content));
  return id;
}

Model::Transaction Document::transact(std::string name) {
  return d_model.transact(std::move(name));
}

void Document::drain() { d_model.drain(); }

ObjectId Document::add_layer(ObjectId content, const Affine& transform, double opacity) {
  auto txn = d_model.transact();
  const ObjectId id = txn.add_layer(content, transform, opacity);
  txn.commit();
  return id;
}

void Document::set_layer_transform(ObjectId layer, const Affine& transform) {
  auto txn = d_model.transact();
  txn.set_transform(layer, transform);
  txn.commit();
}

void Document::set_layer_span(ObjectId layer, const TimeRange& span) {
  auto txn = d_model.transact();
  txn.set_span(layer, span);
  txn.commit();
}

void Document::set_layer_time_map(ObjectId layer, const TimeMap& time_map) {
  auto txn = d_model.transact();
  txn.set_time_map(layer, time_map);
  txn.commit();
}

void Document::attach_layer(ObjectId composition, ObjectId layer) {
  auto txn = d_model.transact();
  txn.attach_layer(composition, layer);
  txn.commit();
}

void Document::set_layer_gain(ObjectId layer, double gain) {
  auto txn = d_model.transact();
  txn.set_gain(layer, gain);
  txn.commit();
}

void Document::set_layer_audible(ObjectId layer, bool audible) {
  auto txn = d_model.transact();
  txn.set_audible(layer, audible);
  txn.commit();
}

void Document::set_working_audio_format(ObjectId composition, const AudioFormat& format) {
  auto txn = d_model.transact();
  txn.set_working_audio_format(composition, format);
  txn.commit();
}

ObjectId Document::add_composition(double canvas_w, double canvas_h) {
  auto txn = d_model.transact();
  const ObjectId id = txn.add_composition(canvas_w, canvas_h);
  txn.commit();
  return id;
}

void Document::set_working_space(ObjectId composition, const SurfaceFormat& format) {
  auto txn = d_model.transact();
  txn.set_working_space(composition, format);
  txn.commit();
}

DocStatePtr Document::pin() const { return d_model.current(); }

Content* Document::resolve(ObjectId content) const {
  const auto found = d_contents.find(content);
  return found == d_contents.end() ? nullptr : found->second.get();
}

void Document::for_each_content(const std::function<void(Content*)>& fn) const {
  for (const auto& entry : d_contents) {
    fn(entry.second.get());
  }
}

void Document::for_each_content(const std::function<void(ObjectId, Content*)>& fn) const {
  for (const auto& entry : d_contents) {
    fn(entry.first, entry.second.get());
  }
}

} // namespace arbc
