#pragma once

#include <arbc/base/ids.hpp>
#include <arbc/base/rational_time.hpp> // TimeMap (set_layer_time_map)
#include <arbc/base/time.hpp>          // TimeRange (set_layer_span)
#include <arbc/base/transform.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/media/surface_format.hpp>
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

  // Configure a layer's temporal placement (doc 11:59-73): the half-open parent-
  // time span `[in, out)` the layer exists over, and the 1D affine time map from
  // parent time to content-local time. Host-facing wrappers over the model's
  // transactional `set_span`/`set_time_map`, mirroring `set_layer_transform`; each
  // commits its own version and bumps the revision. The offline/interactive render
  // drivers read these off the pinned layer record for span-cull + retiming.
  void set_layer_span(ObjectId layer, const TimeRange& span);
  void set_layer_time_map(ObjectId layer, const TimeMap& time_map);

  // Insert a composition (doc 07 rule 2: the unit that owns a working space). Its
  // working space defaults to the doc 07 walking-skeleton format; the render
  // drivers read it from the pinned state for target/temp allocation.
  ObjectId add_composition(double canvas_w, double canvas_h);
  // Configure a composition's working space -- the `SurfaceFormat` the compositor
  // blends it in (doc 07). Committed as its own version, bumping the revision.
  void set_working_space(ObjectId composition, const SurfaceFormat& format);

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
