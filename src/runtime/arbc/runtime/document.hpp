#pragma once

#include <arbc/base/ids.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/model/model.hpp>

#include <memory>
#include <unordered_map>

namespace arbc {

// Host-facing document: the versioned model plus the content binding.
// Records hold opaque content ids; the id-to-Content binding lives here,
// keeping the model free of the contract vtable (doc 17).
class Document {
public:
  Document() = default;

  ObjectId add_content(std::shared_ptr<Content> content);
  ObjectId add_layer(ObjectId content, const Affine& transform, double opacity = 1.0);
  void set_layer_transform(ObjectId layer, const Affine& transform);

  // Pin the current version for rendering (doc 14).
  DocStatePtr pin() const;
  Content* resolve(ObjectId content) const;

private:
  Model d_model;
  // Writer-thread-owned for now; migrates into versioned content records
  // when the Editable facet and the slab arenas land (docs 14/15).
  std::unordered_map<ObjectId, std::shared_ptr<Content>> d_contents;
};

} // namespace arbc
