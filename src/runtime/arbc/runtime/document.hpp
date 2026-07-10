#pragma once

#include <arbc/base/ids.hpp>
#include <arbc/base/rational_time.hpp> // TimeMap (set_layer_time_map)
#include <arbc/base/time.hpp>          // TimeRange (set_layer_span)
#include <arbc/base/transform.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/media/audio_format.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/model/model.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>

namespace arbc {

// Host-facing document: the versioned model plus the content binding.
// Records hold opaque content ids; the id-to-Content binding lives here,
// keeping the model free of the contract vtable (doc 17).
class Document {
public:
  Document() = default;

  // Mint a versioned content object: commits a `Transaction::add_content(kind)`,
  // publishing a `ContentRecord` (opaque `kind` id + inert `StateHandle`) into a
  // pinnable `DocState` (revision +1), and binds the returned record id to
  // `content` in the runtime side-map. `kind` is an opaque caller-supplied token
  // (the reverse-DNS<->numeric bridge is `runtime.document_serialize`'s); it
  // defaults to 0. `resolve(id)` serves the vtable binding, unchanged.
  ObjectId add_content(std::shared_ptr<Content> content, std::uint64_t kind = 0);
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

  // Attach `layer` at the top of `composition`'s ordered membership (doc 14): the
  // host-facing wrapper over the model's transactional attach, mirroring
  // `set_layer_transform`. The audio mix drives composition membership
  // (`mix_composition` walks `for_each_layer_in`, `mix.hpp:59`), so a composition an
  // export monitor mixes has its layers attached here. Commits its own version.
  void attach_layer(ObjectId composition, ObjectId layer);

  // Configure a layer's audio placement (doc 12:89-92): the additive-mix `gain`
  // (analog of opacity, not clamped at 1) and the `audible` flag (analog of
  // `visible`). Host-facing wrappers over the model's transactional
  // `set_gain`/`set_audible`, the audio siblings of `set_layer_transform`. Each
  // commits its own version.
  void set_layer_gain(ObjectId layer, double gain);
  void set_layer_audible(ObjectId layer, bool audible);

  // Configure a composition's working audio format (doc 12:94-104): the working
  // sample rate + channel layout the mix is produced at, the audio twin of
  // `set_working_space`. Committed as its own version, bumping the revision.
  void set_working_audio_format(ObjectId composition, const AudioFormat& format);

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

  // Visit every minted top-level content (the runtime operator binder walks these
  // plus each content's `inputs()` to reach operator input children,
  // `operator_binding.cpp`). Read-only over the side-map; `fn` receives the borrowed
  // `Content*` (never null). The `Content` vtable stays in `runtime`, doc 17:66-72.
  void for_each_content(const std::function<void(Content*)>& fn) const;

private:
  // The runtime load façade (`runtime.document_serialize`) installs a reconstructed
  // graph into `d_model` through the serialize reader, which needs the mutable
  // `Model`. Granted through an attorney-client accessor (defined in
  // `document_serialize.cpp`) rather than a public method, so `Document`'s public
  // shape and member set stay unchanged (refinement Constraint 4).
  friend struct DocumentSerializeAccess;

  Model d_model;
  // The permanent, levelization-mandated home of the id->Content vtable binding
  // (doc 17:66-72): the versioned `ContentRecord` in `DocState` holds only the
  // opaque `{kind, StateHandle}`, so the model stays free of the `Content` vtable.
  // Writer-thread-owned; keyed by the record's `ObjectId`; `resolve()` reads it.
  std::unordered_map<ObjectId, std::shared_ptr<Content>> d_contents;
};

} // namespace arbc
