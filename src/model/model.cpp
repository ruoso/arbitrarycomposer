#include <arbc/model/model.hpp>

namespace arbc {

Model::Model() { d_current.store(std::make_shared<const DocState>()); }

DocStatePtr Model::current() const { return d_current.load(); }

ObjectId Model::allocate_id() { return ObjectId{d_next_id.fetch_add(1)}; }

Model::Transaction::Transaction(Model& model)
    : d_model(&model), d_next(std::make_shared<DocState>(*model.current())) {}

ObjectId Model::Transaction::add_layer(ObjectId content, const Affine& transform, double opacity) {
  const ObjectId id = d_model->allocate_id();
  d_next->layers.push_back(LayerRecord{id, content, transform, opacity, true});
  return id;
}

void Model::Transaction::set_transform(ObjectId layer, const Affine& transform) {
  for (LayerRecord& record : d_next->layers) {
    if (record.id == layer) {
      record.transform = transform;
      return;
    }
  }
}

void Model::Transaction::commit() {
  d_next->revision += 1;
  d_model->d_current.store(std::move(d_next));
}

Model::Transaction Model::transact() { return Transaction(*this); }

} // namespace arbc
