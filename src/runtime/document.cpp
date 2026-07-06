#include <arbc/runtime/document.hpp>

#include <utility>

namespace arbc {

ObjectId Document::add_content(std::shared_ptr<Content> content) {
  const ObjectId id = d_model.allocate_id();
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
