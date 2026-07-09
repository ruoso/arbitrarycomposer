#include <arbc/runtime/document.hpp>

#include <utility>

namespace arbc {

ObjectId Document::add_content(std::shared_ptr<Content> content, std::uint64_t kind) {
  // Mint a versioned ContentRecord (kind id + inert StateHandle) as a top-level
  // DocState entry, published as one new version -- exactly like every other
  // Document mutator. The id->Content vtable binding then rides the runtime
  // side-map keyed by that record's id (doc 17:66-72 keeps the model free of the
  // Content vtable). resolve(id) serves it; find_content(id) carries no pointer.
  auto txn = d_model.transact();
  const ObjectId id = txn.add_content(kind);
  txn.commit();
  d_contents.emplace(id, std::move(content));
  return id;
}

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

} // namespace arbc
